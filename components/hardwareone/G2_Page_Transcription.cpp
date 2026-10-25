#include "G2_Page_Transcription.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#define TL_LENS_LOG(start, ok) ESP_LOGI("STT", "TL lens s=%lu e=%lu tail=%u draft=%u ok=%d", \
    (unsigned long)(start), (unsigned long)millis(), unsigned(strlen(s.tail)), unsigned(strlen(s.draft)), (ok) ? 1 : 0)
#else
#define TL_LENS_LOG(start, ok) ((void)(start))
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_DICTATION
#include "G2_Glasses.h"
#include "G2_HijackCmd.h"
#include "G2_Page_Common.h"
#include "BLE_Peers.h"
#include "System_MemUtil.h"
#include "System_TextPager.h"
#include "Transcription_UI_Policy.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <new>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>

extern void g2ShowAppsMenu();

namespace {
constexpr size_t kTailBytes = 1024, kEntries = 8, kWindowBytes = 512;
constexpr uint32_t kTextPollMs = 800, kStatusPollMs = 1000;
// The right column is two text children, each refreshed by its own
// single-write UPDATE_TEXT. The glasses negotiate MTU 244 (241-byte writes)
// and UPDATE_TEXT framing adds ~43 B, so each child stays <=180 B (a 200-byte
// sidebar produced 243-byte envelopes the lens rejected, freezing the panel).
constexpr size_t kSidebarUpdateMax = 180;
// Header (status) over body (newest text). Body sized to what fits on screen.
constexpr size_t kBodyMax = 150;
constexpr G2ContainerGeom kHeaderGeom = {280, 8, 288, 88};
constexpr G2ContainerGeom kBodyGeom = {280, 100, 288, 180};
constexpr uint32_t kHeaderId = 97, kBodyId = 98;
// Keep four text children and exactly one input owner: the one-line header
// captures scrolls while the three body bands update independently. With no
// capture owner the glasses display text but emit no page-navigation gestures.
// The old two-capture canary and nine-child failure do not prohibit one owner.
// Each complete band UPDATE fits one BLE write, including UTF-8/newlines.
constexpr size_t kLiveLines = 8, kLiveChildren = 4, kLinesPerBand = 3;
constexpr size_t kLiveLineBytes = (kSidebarUpdateMax - (kLinesPerBand - 1)) / kLinesPerBand;
constexpr size_t kPhraseCount = 16, kLayoutLines = 96;
// Anchors name retained source bytes, not display offsets that labels can move.
struct LiveAnchor {
  uint32_t offset = 0;
  bool draft = false, valid = false;
};
struct LiveSourceSpan {
  uint16_t begin = 0, end = 0, source = 0;
  bool draft = false;
};
struct Phrase {
  uint32_t sequence = 0;
  uint16_t begin = 0, end = 0;
  int8_t speaker = -1;
};
static const char* const kLiveNames[kLiveChildren] = {
  "trlh", "trl0", "trl1", "trl2"
};
static const G2ContainerGeom kLiveGeoms[kLiveChildren] = {
  {8, 8, 560, 28}, {8, 44, 560, 84}, {8, 128, 560, 84}, {8, 212, 560, 56}
};
// Small nonempty CREATE content avoids a blank display while the queued page
// commits. Actual text follows via bounded UPDATEs, keeping CREATE <=2 fragments.
static const char* const kLiveInitial[kLiveChildren] = {
  "Live text", "Loading text...", " ", " "
};
enum class View : uint8_t { Home, Live, List, File };
enum class Op : uint8_t { None, Start, Status, Next, Ack, Stop, Cancel, List, Read, Save, Mic };
struct Entry { char name[64] = {}, path[128] = {}; uint32_t bytes = 0; };
struct State {
  uint32_t epoch = 0, request = 0, requestView = 0, viewGeneration = 0;
  uint32_t lastPoll = 0, listOffset = 0, listNext = 0, fileOffset = 0, fileNext = 0;
  uint32_t lastTextPoll = 0, lastStatusPoll = 0;
  uint32_t tailOrigin = 0, liveNavigation = 0;
  LiveAnchor liveAnchor, livePrevious, liveNext;
  bool followTail = true;
  uint8_t liveFirstLine = 0, liveNewestLine = 0;
  uint32_t sequence = 0, receiptOffset = 0, receiptLength = 0;
  uint32_t elapsedMs = 0, history[32] = {};
  uint8_t historyCount = 0;
  Op pending = Op::None, queued = Op::None;
  View view = View::Home;
  bool active = false, done = false, available = false, busy = false, preparing = false;
  bool captureActive = false, inferenceActive = false, saveDefault = false, saveEnabled = false;
  bool saveComplete = false, sd = false, more = false, eof = false;
  bool exiting = false, dirty = false, ackPending = false, nextPoll = false;
  bool recoveringStart = false, exitReady = false, micPdm = false, micG2 = false;
  uint8_t count = 0, page = 0, pages = 0;
  char exchange[17] = {}, message[96] = {}, saveError[64] = {}, micSource[8] = "auto";
  char tail[kTailBytes + 1] = {}, fileText[kWindowBytes + 1] = {}, path[128] = {};
  char draft[kWindowBytes + 1] = {};  // live mode: words still being spoken
  uint32_t draftVersion = 0, lastNavigationMs = 0;
  bool navigationSeen = false;
  G2TapKind lastNavigation = G2_TAP_PAGE_NEXT;
  bool speakersAvailable = false;
  uint8_t phraseCount = 0;
  Phrase phrases[kPhraseCount];
  Entry entries[kEntries];
};
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static State state;
// Only the lens-applier renders. Its scratch stays off the 8 KiB worker stack.
EXT_RAM_BSS_ATTR static State renderState;
EXT_RAM_BSS_ATTR static char rows[12][72], sidebar[512], textBody[1200], textPage[304];
// Confirmed tail plus the live draft, and the last sidebar actually sent (so
// unchanged polls send nothing instead of an identical UPDATE_TEXT).
EXT_RAM_BSS_ATTR static char liveText[kTailBytes + kWindowBytes + 2], renderedSidebar[512];
EXT_RAM_BSS_ATTR static char body[kSidebarUpdateMax + 8], renderedBody[kSidebarUpdateMax + 8];
EXT_RAM_BSS_ATTR static const char* rowPtrs[12];
EXT_RAM_BSS_ATTR static uint16_t pageOffsets[9];
static View renderedView = View::File;
EXT_RAM_BSS_ATTR static char renderedRows[12][72];
static size_t renderedCount = 0;
static uint32_t renderedEpoch = 0, renderedGeneration = 0, renderedMenuGen = 0;
EXT_RAM_BSS_ATTR static char labelledText[2048];
// One source span per phrase or unlabelled gap, plus the draft. The mapping
// stays with the lens-applier scratch; navigation receives only three anchors.
EXT_RAM_BSS_ATTR static LiveSourceSpan liveSources[2 * kPhraseCount + 2];
static size_t liveSourceCount = 0;
EXT_RAM_BSS_ATTR static uint16_t liveLineOffsets[kLayoutLines + 1];
EXT_RAM_BSS_ATTR static char layoutLines[kLayoutLines][kSidebarUpdateMax + 1];
EXT_RAM_BSS_ATTR static char liveChildren[kLiveChildren][kSidebarUpdateMax + 1];
EXT_RAM_BSS_ATTR static char renderedLive[kLiveChildren][kSidebarUpdateMax + 1];
EXT_RAM_BSS_ATTR static char renderedText[sizeof(textPage)];

static bool ownerLive(uint32_t epoch) {
  BlePeerOwnerSession owner;
  return epoch && blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner)
      && owner.live() && owner.transportEpoch == epoch;
}
static bool exchangeValid(const char* id) {
  uint64_t value = 0;
  return TranscriptionUI::parseId(id, value);
}
static void resetLocked(uint32_t epoch) {
  const uint32_t request = state.request + 1;
  state = State{};
  state.epoch = epoch; state.request = request; state.dirty = true;
}
static void appendTailLocked(const char* text, size_t length, bool separator) {
  const size_t oldLength = strlen(state.tail);
  const size_t space = separator && oldLength ? 1 : 0;
  // A draft has no phrase identity in this protocol. Once a final arrives,
  // fall back to that committed boundary instead of reusing its draft offset
  // in a later utterance. Known committed anchors remain exact across reflow.
  if (!state.followTail && state.liveAnchor.valid && state.liveAnchor.draft)
    state.liveAnchor = {state.tailOrigin + uint32_t(oldLength + space), false, true};
  if (space) TranscriptionUI::appendRecent(state.tail, sizeof(state.tail), " ", 1);
  TranscriptionUI::appendRecent(state.tail, sizeof(state.tail), text, length);
  const size_t newLength = strlen(state.tail);
  const size_t trimmed = oldLength + space + length - newLength;
  state.tailOrigin += trimmed;
  size_t kept = 0;
  for (size_t i = 0; i < state.phraseCount; ++i) {
    auto phrase = state.phrases[i];
    if (phrase.end <= trimmed) continue;
    phrase.begin = phrase.begin > trimmed ? phrase.begin - trimmed : 0;
    phrase.end -= trimmed;
    state.phrases[kept++] = phrase;
  }
  state.phraseCount = kept;
  if (!kept || state.phrases[kept - 1].sequence != state.sequence) {
    if (kept == kPhraseCount) {
      memmove(state.phrases, state.phrases + 1, (kPhraseCount - 1) * sizeof(Phrase));
      --kept;
    }
    auto& phrase = state.phrases[kept++];
    phrase = Phrase{}; phrase.sequence = state.sequence;
    phrase.begin = newLength - length;
    state.phraseCount = kept;
  }
  state.phrases[kept - 1].end = newLength;
}

static void buildLabelledText(const State& s) {
  size_t used = 0;
  labelledText[0] = '\0'; liveSourceCount = 0;
  auto append = [&](const char* text, size_t length) {
    length = std::min(length, sizeof(labelledText) - used - 1);
    length = TranscriptionUI::completePrefix(text, length);
    memcpy(labelledText + used, text, length); used += length; labelledText[used] = '\0';
  };
  auto source = [&](size_t begin, size_t end, bool draft) {
    const size_t out = used;
    append((draft ? s.draft : s.tail) + begin, end - begin);
    if (used > out && liveSourceCount < sizeof(liveSources) / sizeof(liveSources[0]))
      liveSources[liveSourceCount++] = {uint16_t(out), uint16_t(used), uint16_t(begin), draft};
  };
  // Display casing only (the source bytes and anchors are untouched): a
  // paragraph, i.e. a known speaker's turn or the plain prose when speakers
  // are off, starts with a capital. Unassigned "?:" text keeps the decoder's
  // casing so its words do not change case when a label joins them to a turn.
  auto capitalize = [&](size_t at) {
    if (at < used && labelledText[at] >= 'a' && labelledText[at] <= 'z') labelledText[at] = char(labelledText[at] - ('a' - 'A'));
  };
  int lastSpeaker = -2;
  auto phrase = [&](size_t begin, size_t end, int speaker) {
    while (begin < end && s.tail[begin] == ' ') ++begin;
    while (end > begin && s.tail[end - 1] == ' ') --end;
    if (begin == end) return;
    // Unknown is not a shared identity: keep each waiting phrase distinct.
    const bool sameRun = used && speaker >= 0 && speaker == lastSpeaker;
    if (used) append(sameRun ? " " : "\n", 1);
    if (!sameRun) {
      char tag[24];
      if (speaker >= 0) snprintf(tag, sizeof(tag), "Person %d: ", speaker + 1);
      else snprintf(tag, sizeof(tag), "?: "); // unassigned text, never a guessed person
      append(tag, strlen(tag));
    }
    const size_t words = used;
    source(begin, end, false); lastSpeaker = speaker;
    if (!sameRun && speaker >= 0) capitalize(words);
  };
  const size_t tailLength = strlen(s.tail);
  if (!s.speakersAvailable) { source(0, tailLength, false); capitalize(0); }
  else {
    size_t cursor = 0;
    for (size_t i = 0; i < s.phraseCount; ++i) {
      const auto& item = s.phrases[i];
      if (item.begin < cursor || item.end > tailLength || item.end < item.begin) continue;
      // Text whose phrase metadata was evicted still belongs on screen,
      // but must not inherit the first retained speaker's identity.
      if (item.begin > cursor) phrase(cursor, item.begin, -1);
      phrase(item.begin, item.end, item.speaker);
      cursor = item.end;
    }
    if (cursor < tailLength) phrase(cursor, tailLength, -1);
  }
  if (s.draft[0]) {
    if (used) append("\n", 1);
    append("~ ", 2); // live words are provisional and have no speaker assignment
    source(0, strlen(s.draft), true);
  }
}

static LiveAnchor anchorAtLine(const State& s, size_t line) {
  const size_t out = liveLineOffsets[line];
  for (size_t i = 0; i < liveSourceCount; ++i) {
    const auto& span = liveSources[i];
    if (out < span.end) {
      const uint32_t offset = span.source + (out > span.begin ? out - span.begin : 0);
      return {offset + (span.draft ? 0 : s.tailOrigin), span.draft, true};
    }
  }
  return {};
}

static size_t lineAtAnchor(const State& s, const LiveAnchor& anchor, size_t lines) {
  if (!anchor.valid || !lines) return 0;
  const uint32_t relative = anchor.draft ? anchor.offset : anchor.offset - s.tailOrigin;
  // If the anchored words have left the bounded tail, stay at the oldest
  // retained text. Unsigned subtraction also handles origin rollover.
  if (!anchor.draft && int32_t(relative) < 0) return 0;
  size_t out = anchor.draft ? strlen(labelledText) : 0;
  for (size_t i = 0; i < liveSourceCount; ++i) {
    const auto& span = liveSources[i];
    if (span.draft != anchor.draft) continue;
    const size_t length = span.end - span.begin;
    out = span.end - 1;
    if (relative < span.source + length) {
      out = span.begin + (relative > span.source ? relative - span.source : 0);
      break;
    }
  }
  size_t line = 0;
  while (line + 1 < lines && liveLineOffsets[line + 1] <= out) ++line;
  return line;
}

// Word wrapping with complete UTF-8 characters and a separate byte bound for
// transport. Wide glyphs count as two cells; malformed bytes stay bounded.
static size_t splitLiveLines(const char* text) {
  memset(layoutLines, 0, sizeof(layoutLines));
  size_t count = 0;
  const char* p = text;
  while (*p && count < kLayoutLines) {
    liveLineOffsets[count] = p - text;
    auto& line = layoutLines[count++];
    size_t bytes = 0, cols = 0, breakBytes = 0;
    const char* wordBreak = nullptr;
    while (*p && *p != '\n') {
      const unsigned char lead = static_cast<unsigned char>(*p);
      if (lead == '\r' || (lead < 32 && lead != '\t') || lead == 127) { ++p; continue; }
      size_t width = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
          (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
      for (size_t i = 1; i < width; ++i) {
        if (!p[i] || !TranscriptionUI::continuation(static_cast<unsigned char>(p[i]))) { width = 1; break; }
      }
      const size_t cells = width >= 3 ? 2 : 1;
      if (cols + cells > G2_TEXT_DEFAULT_COLS || bytes + width > kLiveLineBytes) {
        if (wordBreak) { p = wordBreak; bytes = breakBytes; }
        break;
      }
      if (lead == ' ' || lead == '\t') {
        wordBreak = p + 1; breakBytes = bytes; line[bytes++] = ' ';
      } else { memcpy(line + bytes, p, width); bytes += width; }
      cols += cells; p += width;
    }
    line[bytes] = '\0';
    if (*p == '\n') ++p;
  }
  liveLineOffsets[count] = p - text;
  return count;
}
static void render();
static void enqueueRender() {
  if (g2GetHijackPage() != G2_HIJACK_PAGE_TRANSCRIPTION) return;
  auto* spec = new (std::nothrow) RedrawSpec{};
  auto* job = new (std::nothrow) LensUiJob{};
  if (!spec || !job) { delete spec; delete job; return; }
  spec->render = render;
  job->kind = LensJobKind::Redraw; job->submitMenuGen = g2CurrentMenuGen();
  job->targetPage = G2_HIJACK_PAGE_TRANSCRIPTION; job->payload.redraw = spec;
  portENTER_CRITICAL(&stateMux); state.dirty = false; portEXIT_CRITICAL(&stateMux);
  if (!g2EnqueueLensJob(job, G2LensEnqueueWait::NoWait)) {
    delete spec; delete job;
    portENTER_CRITICAL(&stateMux); state.dirty = true; portEXIT_CRITICAL(&stateMux);
  }
}
static void changeViewLocked(View view) {
  state.view = view; state.navigationSeen = false; ++state.viewGeneration; state.page = 0; state.pages = 0; state.dirty = true;
  state.followTail = true; state.liveAnchor = state.livePrevious = state.liveNext = LiveAnchor{};
  ++state.liveNavigation;
}
static void exitText() {
  g2BumpMenuGen();
  portENTER_CRITICAL(&stateMux);
  changeViewLocked(state.view == View::File ? View::List : View::Home);
  portEXIT_CRITICAL(&stateMux);
  enqueueRender();
}
static void navigateText(G2TapKind kind) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateMux);
  // Both temples may mirror a ring scroll. A stable page no longer has a
  // swap-in-flight window to absorb the second event from the same gesture.
  if (state.view == View::Live) {
    if (state.navigationSeen && state.lastNavigation == kind &&
        static_cast<uint32_t>(now - state.lastNavigationMs) < 120) {
      portEXIT_CRITICAL(&stateMux); return;
    }
    state.navigationSeen = true; state.lastNavigation = kind; state.lastNavigationMs = now;
  }
  if (state.pending == Op::Read || state.queued == Op::Read) { portEXIT_CRITICAL(&stateMux); return; }
  if (state.view == View::Live) {
    if (kind == G2_TAP_PAGE_NEXT) {
      state.liveAnchor = state.liveNext;
      // Next at the newest visible position also explicitly resumes following
      // after a label correction shrank an older anchored view to one page.
      state.followTail = (state.liveFirstLine / kLiveLines + 1) * kLiveLines >= state.liveNewestLine;
    } else if (state.liveFirstLine) {
      state.liveAnchor = state.livePrevious; state.followTail = false;
    }
    ++state.liveNavigation;
  } else if (kind == G2_TAP_PAGE_NEXT) {
    if (state.page + 1 < state.pages) ++state.page;
    else if (state.view == View::File && !state.eof) {
      if (state.historyCount == 32) {
        memmove(state.history, state.history + 1, 31 * sizeof(state.history[0]));
        --state.historyCount;
      }
      state.history[state.historyCount++] = state.fileOffset;
      state.fileOffset = state.fileNext; state.queued = Op::Read; state.page = 0;
      ++state.viewGeneration;
    }
  } else if (state.page) --state.page;
  else if (state.view == View::File && state.fileOffset) {
    // Exact prior UTF-8 boundaries. Beyond the bounded history, rewind to
    // the beginning rather than invent an offset inside a multibyte character.
    state.fileOffset = state.historyCount ? state.history[--state.historyCount] : 0;
    state.queued = Op::Read; ++state.viewGeneration;
  }
  state.dirty = true;
  portEXIT_CRITICAL(&stateMux);
  enqueueRender();
}
static void render() {
  G2HijackCtxGuard identity;
  portENTER_CRITICAL(&stateMux); renderState = state; portEXIT_CRITICAL(&stateMux);
  if (g2GetHijackPage() != G2_HIJACK_PAGE_TRANSCRIPTION || !g2LensGetState().hijackActive) { renderState = State{}; return; }
  if (!ownerLive(renderState.epoch) || !identity.stillCurrent()) {
    renderState = State{}; renderedCount = 0; renderedEpoch = 0;
    TranscriptionUI::clear(renderedText, sizeof(renderedText));
    TranscriptionUI::clear(labelledText, sizeof(labelledText));
    for (auto& span : liveSources) span = LiveSourceSpan{};
    liveSourceCount = 0;
    memset(liveLineOffsets, 0, sizeof(liveLineOffsets));
    TranscriptionUI::clear(reinterpret_cast<char*>(layoutLines), sizeof(layoutLines));
    TranscriptionUI::clear(reinterpret_cast<char*>(liveChildren), sizeof(liveChildren));
    TranscriptionUI::clear(reinterpret_cast<char*>(renderedLive), sizeof(renderedLive));
    TranscriptionUI::clear(textBody, sizeof(textBody));
    TranscriptionUI::clear(textPage, sizeof(textPage));
    TranscriptionUI::clear(sidebar, sizeof(sidebar));
    const char* locked[] = {"<- Apps", "Sign in and pair the glasses"};
    (void)g2ShowListPage(locked, 2);
    return;
  }
  const auto& s = renderState;
  bool shown = false;
  const uint32_t menuGeneration = g2CurrentMenuGen();
  const bool samePresentation = renderedEpoch == s.epoch && renderedView == s.view &&
      renderedGeneration == s.viewGeneration && renderedMenuGen == menuGeneration;
  const size_t tailLen = strlen(s.tail);
  const bool needSpace = tailLen && s.draft[0] && s.tail[tailLen - 1] != ' ';
  snprintf(liveText, sizeof(liveText), "%s%s%s", s.tail, needSpace ? " " : "", s.draft);
  if (s.view == View::Live) {
    buildLabelledText(s);
    const size_t lineCount = splitLiveLines(labelledText[0] ? labelledText : "Waiting for text");
    const size_t pages = std::max<size_t>(1, (lineCount + kLiveLines - 1) / kLiveLines);
    const size_t newestLine = lineCount > kLiveLines ? lineCount - kLiveLines : 0;
    const size_t firstLine = s.followTail ? newestLine :
        std::min(lineAtAnchor(s, s.liveAnchor, lineCount), newestLine);
    const size_t selected = firstLine == newestLine ? pages - 1 : firstLine / kLiveLines;
    // A source anchor can land between page boundaries after regrouping.
    // Adjacent grid boundaries keep every intervening line reachable.
    const size_t previousLine = firstLine ? (firstLine - 1) / kLiveLines * kLiveLines : 0;
    const size_t nextLine = std::min((firstLine / kLiveLines + 1) * kLiveLines, newestLine);
    portENTER_CRITICAL(&stateMux);
    const bool current = state.epoch == s.epoch && state.viewGeneration == s.viewGeneration &&
        state.liveNavigation == s.liveNavigation;
    if (current) {
      state.page = selected; state.pages = pages;
      state.liveFirstLine = firstLine; state.liveNewestLine = newestLine;
      // Keep the original reading anchor during reflow, rather than moving
      // it back to each newly wrapped line and gradually drifting backwards.
      if (s.followTail || !s.liveAnchor.valid) state.liveAnchor = anchorAtLine(s, firstLine);
      state.livePrevious = anchorAtLine(s, previousLine);
      state.liveNext = anchorAtLine(s, nextLine);
    } else state.dirty = true;
    portEXIT_CRITICAL(&stateMux);
    if (!current) { renderState = State{}; return; }
    memset(liveChildren, 0, sizeof(liveChildren));
    snprintf(liveChildren[0], sizeof(liveChildren[0]), "Live %u/%u | scroll=page  2x=back",
             unsigned(selected + 1), unsigned(pages));
    for (size_t i = 0; i < kLiveLines && firstLine + i < lineCount; ++i) {
      auto& band = liveChildren[1 + i / kLinesPerBand];
      size_t used = strlen(band);
      if (i % kLinesPerBand) band[used++] = '\n';
      const size_t bytes = strlen(layoutLines[firstLine + i]);
      memcpy(band + used, layoutLines[firstLine + i], bytes + 1);
    }
    bool pending = false;
    const bool ready = g2TextPageReady(exitText, navigateText, &pending);
    if (samePresentation && ready && identity.stillCurrent()) {
      shown = true;
      for (size_t i = 0; i < kLiveChildren; ++i) {
        if (strcmp(renderedLive[i], liveChildren[i]) == 0) continue;
        if (identity.stillCurrent() && g2UpdateMixedTextChild(kLiveNames[i], 110 + i, liveChildren[i]))
          memcpy(renderedLive[i], liveChildren[i], sizeof(renderedLive[i]));
        else shown = false;  // retry only unsuccessful bands, never tear down a live page
      }
    } else if (!pending && identity.stillCurrent()) {
      G2TextChildSpec children[kLiveChildren];
      for (size_t i = 0; i < kLiveChildren; ++i) {
        children[i] = {kLiveNames[i], kLiveInitial[i], static_cast<uint32_t>(110 + i), kLiveGeoms[i], i == 0};
      }
      if (g2ShowMultiTextPage(children, kLiveChildren, exitText, navigateText)) {
        for (size_t i = 0; i < kLiveChildren; ++i)
          snprintf(renderedLive[i], sizeof(renderedLive[i]), "%s", kLiveInitial[i]);
        renderedView = s.view; renderedEpoch = s.epoch; renderedCount = 0;
        renderedGeneration = s.viewGeneration; renderedMenuGen = menuGeneration;
      }
      // Admission is asynchronous. Patch the initial placeholders only once
      // the exact presentation/callback pair is committed.
    }
  } else if (s.view == View::File) {
    const char* text = s.view == View::Live ? liveText :
        s.message[0] ? s.message : s.fileText;
    bool truncated = false;
    const size_t bytes = textWrapInto(textBody, sizeof(textBody), text, G2_TEXT_DEFAULT_COLS, 0, true, &truncated);
    TextPager pager = {textBody, pageOffsets, 8, 176, 0, 0, truncated};
    textSplitPages(pager, bytes);
    pager.curPage = std::min<int>(s.page, std::max(0, pager.pageCount - 1));
    portENTER_CRITICAL(&stateMux);
    if (state.epoch == s.epoch && state.viewGeneration == s.viewGeneration) {
      state.page = pager.curPage; state.pages = pager.pageCount;
    }
    portEXIT_CRITICAL(&stateMux);
    char title[64];
    if (s.view == View::Live) snprintf(title, sizeof(title), "Live - recent text");
    else snprintf(title, sizeof(title), "Saved text @%lu%s", (unsigned long)s.fileOffset, s.eof ? " (end)" : "");
    G2TextPageChrome chrome = {title, "scroll=page  2x=back", "scroll=more  2x=back", nullptr,
                              text[0] ? "" : "Waiting for text"};
    const bool sameView = samePresentation;
    if (pager.pageCount > 1) {
      // Build the bounded page without submitting it twice. The shared renderer
      // itself enqueues a swap, so deduplicate before calling it.
      const size_t begin = pageOffsets[pager.curPage], end = pageOffsets[pager.curPage + 1];
      snprintf(textPage, sizeof(textPage), "%s [%d/%d] %s\n%.*s", title,
               pager.curPage + 1, pager.pageCount, chrome.navHint,
               static_cast<int>(end - begin), textBody + begin);
    } else {
      snprintf(textPage, sizeof(textPage), "%s %s\n%.176s", title, chrome.singleHint,
               textBody[0] ? textBody : chrome.emptyMsg);
    }
    if (sameView && strcmp(renderedText, textPage) == 0) shown = true;
    else if (identity.stillCurrent()) {
      const uint32_t sendStart = millis();
      shown = g2ShowTextPage(textPage, G2_GEOM_LARGE, exitText, navigateText);
      TL_LENS_LOG(sendStart, shown);  // timeline: lens page sent
    }
    if (shown) {
      snprintf(renderedText, sizeof(renderedText), "%s", textPage);
      renderedView = s.view; renderedEpoch = s.epoch; renderedCount = 0;
    }
  } else {
    size_t n = 0;
    memset(rows, 0, sizeof(rows));
    auto add = [&](const char* text) { snprintf(rows[n], sizeof(rows[n]), "%s", text); rowPtrs[n] = rows[n]; ++n; };
    if (s.view == View::Home) {
      add(s.active ? "<- Apps (cancel session)" : "<- Apps");
      add(s.recoveringStart ? "Checking start status..." : s.active ? "Stop transcription" : "Start transcription");
      add("Live text");
      add(s.saveDefault ? "Save next session: On" : "Save next session: Off");
      add("Saved: Internal"); add("Saved: SD card");
      char mic[32];
      snprintf(mic, sizeof(mic), "Mic: %s", TranscriptionUI::micLabel(s.micSource));
      add(mic);
      const char* phase = s.preparing ? "Getting ready" : s.captureActive ? "Listening" :
                          s.inferenceActive ? "Transcribing" : s.active ? "Stopping" : s.done ? "Finished" : "Ready";
      snprintf(sidebar, sizeof(sidebar), "%s%s\n%lus%s\n%.95s%s%.63s", phase,
               s.pending != Op::None ? "..." : "", (unsigned long)(s.elapsedMs / 1000),
               s.active ? (s.saveEnabled ? " | saving" : " | not saving") : (s.saveComplete ? " | saved" : ""),
               s.message, s.saveError[0] ? "\nSave: " : "", s.saveError);
      sidebar[kSidebarUpdateMax] = '\0';
      // Body: the newest words that fit, starting on a UTF-8 boundary.
      const size_t textLen = strlen(liveText);
      const char* newest = liveText + (textLen > kBodyMax ? textLen - kBodyMax : 0);
      while ((*newest & 0xc0) == 0x80) ++newest;
      snprintf(body, sizeof(body), "%s", newest);
    } else {
      add("<- Transcription");
      for (size_t i = 0; i < s.count; ++i) add(s.entries[i].name);
      if (!s.count) add("(no saved transcripts)");
      if (s.listOffset) add("<< Previous files");
      if (s.more) add("Next files >>");
      snprintf(sidebar, sizeof(sidebar), "%s storage\nSaved transcripts", s.sd ? "SD" : "Internal");
      snprintf(body, sizeof(body), "%.*s", (int)kBodyMax, s.message);
    }
    const G2TextChildSpec header = {"trhead", sidebar, kHeaderId, kHeaderGeom, false};
    const G2TextChildSpec text = {"transcription", body, kBodyId, kBodyGeom, false};
    const bool sameRows = samePresentation && g2LensGetState().containerReady && renderedCount == n &&
        memcmp(renderedRows, rows, n * sizeof(rows[0])) == 0;
    if (identity.stillCurrent()) {
      // Only the text half changes while the rows are unchanged; the list
      // (and its selection) is rebuilt only when a row label itself changes.
      // Each child is patched only when its own text changed.
      if (sameRows) {
        const bool headOk = strcmp(renderedSidebar, sidebar) == 0 ||
            g2UpdateMixedTextChild("trhead", kHeaderId, sidebar);
        bool bodyOk = strcmp(renderedBody, body) == 0;
        if (!bodyOk) {
          const uint32_t sendStart = millis();
          bodyOk = g2UpdateMixedTextChild("transcription", kBodyId, body);
          TL_LENS_LOG(sendStart, bodyOk);  // timeline: lens text sent
        }
        shown = headOk && bodyOk;
      }
      if (!shown) shown = g2ShowMixedListText2(rowPtrs, n, G2_GEOM_SPLIT_LIST, header, text);
    }
    if (shown) {
      memcpy(renderedRows, rows, n * sizeof(rows[0])); renderedView = s.view; renderedCount = n; renderedEpoch = s.epoch;
      snprintf(renderedSidebar, sizeof(renderedSidebar), "%s", sidebar);
      snprintf(renderedBody, sizeof(renderedBody), "%s", body);
    }
  }
  if (shown) { renderedGeneration = s.viewGeneration; renderedMenuGen = menuGeneration; }
  if (!shown) { portENTER_CRITICAL(&stateMux); state.dirty = true; portEXIT_CRITICAL(&stateMux); }
  renderState = State{};
}

static void complete(bool ok, const char* result, const G2CmdCookie&, void* opaque) {
  const uint32_t request = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(opaque));
  uint32_t epoch;
  portENTER_CRITICAL(&stateMux); epoch = state.epoch; portEXIT_CRITICAL(&stateMux);
  if (!ownerLive(epoch)) return;
  PSRAM_JSON_DOC(doc);
  const bool parsed = result && !deserializeJson(doc, result);
  const bool onPage = g2GetHijackPage() == G2_HIJACK_PAGE_TRANSCRIPTION && g2LensGetState().hijackActive;
  portENTER_CRITICAL(&stateMux);
  if (state.request != request || state.pending == Op::None || state.epoch != epoch) {
    portEXIT_CRITICAL(&stateMux); return;
  }
  const Op op = state.pending;
  state.pending = Op::None;
  const bool success = ok && parsed && (doc["success"] | false) &&
      !(op == Op::Status && state.recoveringStart && !doc["exchange"].is<const char*>());
  if (op == Op::Save || op == Op::Mic) {
    if (!ok || (result && strncmp(result, "Error", 5) == 0))
      snprintf(state.message, sizeof(state.message), "Setting change denied or failed");
    if (state.queued == Op::None) state.queued = Op::Status;
  } else if (!success) {
    snprintf(state.message, sizeof(state.message), "%s", parsed ? (doc["error"] | "Request failed") : "Request failed");
    if (op == Op::Ack) { state.ackPending = false; state.queued = Op::Cancel; }
    if (op == Op::Start && (!ok || !parsed || !doc["success"].is<bool>())) {
      // Admission may have succeeded before its reply failed. Do not repeat
      // Start or forget this one outstanding admission on leave/re-entry.
      state.recoveringStart = true; state.queued = Op::None;
      snprintf(state.message, sizeof(state.message), "Checking start status before continuing");
    }
    if (op == Op::Status && !state.recoveringStart) { state.exchange[0] = '\0'; state.active = false; state.nextPoll = false; }
    if (op == Op::Cancel) { // exact lease is already unavailable
      const bool exitReady = state.exiting && onPage;
      resetLocked(onPage ? epoch : 0); state.exitReady = exitReady;
    }
  } else if (op == Op::Start || op == Op::Status || op == Op::Stop || op == Op::Cancel) {
    const char* id = doc["exchange"] | "";
    if (exchangeValid(id)) snprintf(state.exchange, sizeof(state.exchange), "%s", id);
    else state.exchange[0] = '\0';
    if (op == Op::Status && state.recoveringStart) {
      state.recoveringStart = false; state.message[0] = '\0';
    }
    state.active = doc["active"] | false; state.done = doc["done"] | false;
    state.available = doc["available"] | false; state.busy = doc["busy"] | false;
    state.preparing = doc["preparing"] | false;
    state.captureActive = doc["captureActive"] | false; state.inferenceActive = doc["inferenceActive"] | false;
    state.elapsedMs = doc["elapsedMs"] | 0u;
    state.saveDefault = doc["saveDefault"] | false; state.saveEnabled = doc["saveEnabled"] | false;
    state.saveComplete = doc["saveComplete"] | false;
    snprintf(state.micSource, sizeof(state.micSource), "%s", doc["micSource"] | "auto");
    state.micPdm = doc["micPdm"] | false; state.micG2 = doc["micG2"] | false;
    snprintf(state.saveError, sizeof(state.saveError), "%s", doc["saveError"] | "");
    const char* failure = doc["failure"] | "";
    if (failure[0]) snprintf(state.message, sizeof(state.message), "%s", failure);
    if (!state.available && !state.active && !state.message[0])
      snprintf(state.message, sizeof(state.message), "%s", doc["reason"] | "Unavailable");
    if (op == Op::Start) {
      state.tail[0] = '\0'; state.message[0] = '\0'; state.ackPending = false;
      state.draft[0] = '\0'; state.draftVersion = 0; state.phraseCount = 0;
      state.sequence = state.receiptOffset = state.receiptLength = 0;
      state.tailOrigin = 0; state.followTail = true;
      state.liveAnchor = state.livePrevious = state.liveNext = LiveAnchor{};
      ++state.liveNavigation;
      for (auto& phrase : state.phrases) phrase = Phrase{};
      state.speakersAvailable = false;
      if (!state.exchange[0]) {
        state.recoveringStart = true; state.queued = Op::None;
        snprintf(state.message, sizeof(state.message), "Checking start status before continuing");
      }
    }
    state.nextPoll = state.exchange[0] != '\0';
    if (op == Op::Cancel) {
      const bool exitReady = state.exiting && onPage;
      resetLocked(onPage ? epoch : 0); state.exitReady = exitReady;
    }
  } else if (op == Op::Next) {
    const char* id = doc["exchange"] | "";
    if (strcmp(id, state.exchange) == 0 && (doc["available"] | false) && !state.exiting) {
      const char* text = doc["sttText"] | "";
      const size_t length = strlen(text);
      if (length && length <= kWindowBytes && length == (doc["length"] | 0u)) {
        const uint32_t sequence = doc["sequence"] | 0u, offset = doc["offset"] | 0u;
        const bool duplicate = state.sequence == sequence && state.receiptOffset == offset && state.receiptLength == length;
        state.sequence = sequence; state.receiptOffset = offset; state.receiptLength = length;
        // The receipt names the raw bytes; the tail shows the display copy
        // (vocabulary spellings) when the adapter supplied one.
        const char* display = doc["display"] | text;
        const size_t shown = strlen(display);
        if (!duplicate) appendTailLocked(shown && shown <= kWindowBytes ? display : text,
                                         shown && shown <= kWindowBytes ? shown : length, state.receiptOffset == 0);
        state.ackPending = true;
      } else snprintf(state.message, sizeof(state.message), "Invalid text receipt");
    }
    if (strcmp(id, state.exchange) == 0 && !state.exiting) {
      state.speakersAvailable = doc["speakersAvailable"] | state.speakersAvailable;
      for (JsonObjectConst label : doc["speakers"].as<JsonArrayConst>()) {
        if (!(label["ready"] | false)) continue;
        const uint32_t sequence = label["sequence"] | 0u;
        const int speaker = label["speaker"] | -1;
        for (size_t i = 0; i < state.phraseCount; ++i)
          if (state.phrases[i].sequence == sequence)
            state.phrases[i].speaker = speaker >= 0 && speaker < 16 ? speaker : -1;
      }
      const uint32_t version = doc["draftVersion"] | 0u;
      if (version != state.draftVersion) {
        state.draftVersion = version;
        const char* draft = doc["draft"] | "";
        if (!draft[0] && !state.followTail && state.liveAnchor.valid && state.liveAnchor.draft)
          state.liveAnchor = {state.tailOrigin + uint32_t(strlen(state.tail)), false, true};
        snprintf(state.draft, sizeof(state.draft), "%s", draft);
      }
    }
    state.nextPoll = false;
  } else if (op == Op::Ack) {
    state.ackPending = false; state.nextPoll = true;
  } else if (state.requestView == state.viewGeneration && op == Op::List) {
    state.count = 0; state.listOffset = doc["offset"] | 0u;
    state.listNext = doc["nextOffset"] | 0u; state.more = doc["more"] | false;
    for (JsonObjectConst entry : doc["entries"].as<JsonArrayConst>()) {
      if (state.count == kEntries) break;
      auto& dst = state.entries[state.count++];
      snprintf(dst.name, sizeof(dst.name), "%s", entry["name"] | "");
      snprintf(dst.path, sizeof(dst.path), "%s", entry["path"] | "");
      dst.bytes = entry["bytes"] | 0u;
    }
    state.message[0] = '\0';
  } else if (state.requestView == state.viewGeneration && op == Op::Read) {
    snprintf(state.fileText, sizeof(state.fileText), "%s", doc["sttText"] | "");
    state.fileOffset = doc["offset"] | 0u; state.fileNext = doc["nextOffset"] | state.fileOffset;
    state.eof = doc["eof"] | true; state.page = 0; state.message[0] = '\0';
  }
  state.dirty = true;
  portEXIT_CRITICAL(&stateMux);
}
} // namespace

void g2BuildTranscriptionInfo(char* out, size_t cap) {
  if (out && cap) snprintf(out, cap, "Transcription: open the app on the lens for private live text and saved sessions.");
}
void g2ShowTranscriptionMenu() {
  BlePeerOwnerSession owner;
  const bool live = blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner) && owner.live();
  portENTER_CRITICAL(&stateMux);
  if (!live || state.epoch != owner.transportEpoch) resetLocked(live ? owner.transportEpoch : 0);
  changeViewLocked(View::Home);
  if (state.pending == Op::None && !state.exiting && !state.recoveringStart) state.queued = Op::Status;
  if (!live) snprintf(state.message, sizeof(state.message), "Pair glasses as a signed-in user");
  portEXIT_CRITICAL(&stateMux);
  g2SetHijackPage(G2_HIJACK_PAGE_TRANSCRIPTION);
  enqueueRender();
}
void g2TranscriptionHandleTap(uint32_t index) {
  uint32_t epoch;
  portENTER_CRITICAL(&stateMux); epoch = state.epoch; portEXIT_CRITICAL(&stateMux);
  if (!ownerLive(epoch)) { if (index == 0) g2ShowAppsMenu(); return; }
  bool back = false, viewChanged = false;
  portENTER_CRITICAL(&stateMux);
  state.message[0] = '\0';
  if (state.view == View::Home) {
    if (index == 0) {
      state.exiting = true; back = !state.recoveringStart;
      if (!back) snprintf(state.message, sizeof(state.message), "Checking start status before leaving");
    }
    else if (index == 1 && !state.exiting && !state.recoveringStart && state.pending != Op::Start && state.pending != Op::Cancel) state.queued = state.active ? Op::Stop : Op::Start;
    else if (index == 2) { changeViewLocked(View::Live); state.page = 255; viewChanged = true; }
    else if (index == 3) state.queued = Op::Save;
    else if (index == 6) {
      // The source is claimed when a session starts; changing it mid-session
      // would only apply to the next one, so make that explicit.
      if (state.active || state.recoveringStart)
        snprintf(state.message, sizeof(state.message), "Stop transcription to change mic");
      else state.queued = Op::Mic;
    }
    else if (index == 4 || index == 5) {
      changeViewLocked(View::List); state.sd = index == 5; state.count = 0;
      state.listOffset = 0; state.more = false; state.queued = Op::List; viewChanged = true;
    }
  } else if (state.view == View::List) {
    if (index == 0) { changeViewLocked(View::Home); viewChanged = true; }
    else if (index <= state.count) {
      snprintf(state.path, sizeof(state.path), "%s", state.entries[index - 1].path);
      changeViewLocked(View::File); state.fileOffset = 0; state.historyCount = 0; state.fileText[0] = '\0';
      state.eof = true; state.queued = Op::Read; viewChanged = true;
    } else {
      uint32_t row = 1 + (state.count ? state.count : 1);
      if (state.listOffset && index == row++) {
        state.listOffset = state.listOffset > kEntries ? state.listOffset - kEntries : 0; state.queued = Op::List;
      } else if (state.more && index == row) { state.listOffset = state.listNext; state.queued = Op::List; }
    }
  }
  state.dirty = true;
  portEXIT_CRITICAL(&stateMux);
  if (viewChanged) g2BumpMenuGen();
  if (back) g2ShowAppsMenu(); else enqueueRender();
}
void g2TranscriptionTick() {
  const uint32_t now = millis();
  uint32_t epoch;
  portENTER_CRITICAL(&stateMux); epoch = state.epoch; portEXIT_CRITICAL(&stateMux);
  if (!epoch) {
    bool dirty; portENTER_CRITICAL(&stateMux); dirty = state.dirty; portEXIT_CRITICAL(&stateMux);
    if (dirty) enqueueRender();
    return;
  }
  if (!ownerLive(epoch)) {
    portENTER_CRITICAL(&stateMux); if (state.epoch == epoch) resetLocked(0); portEXIT_CRITICAL(&stateMux);
    enqueueRender(); return;
  }
  char command[256] = {};
  uint32_t request = 0;
  bool dirty = false;
  const bool onPage = g2GetHijackPage() == G2_HIJACK_PAGE_TRANSCRIPTION && g2LensGetState().hijackActive;
  portENTER_CRITICAL(&stateMux);
  // The glasses blank the lens after ~30-60 s without ring/temple input
  // (DISPLAY_OFF), which also ends the hijack. That must not end a running
  // meeting transcription: keep polling/acking in the background and show
  // it again when the app is reopened. Explicit Back still cancels.
  if (!onPage && !(state.active && state.exchange[0])) state.exiting = true;
  if (state.exitReady || (state.exiting && !state.recoveringStart &&
      state.pending == Op::None && !state.exchange[0])) {
    resetLocked(0); portEXIT_CRITICAL(&stateMux);
    if (onPage) g2ShowAppsMenu();
    return;
  }
  if (state.pending == Op::None) {
    Op op = Op::None;
    if (state.recoveringStart) {
      // Read-only recovery is throttled and remains owned by the original
      // epoch/request chain. New Start is blocked until a definitive status.
      if ((uint32_t)(now - state.lastPoll) >= 1000) op = Op::Status;
    } else if (state.exiting) {
      if (state.exchange[0]) op = Op::Cancel;
      else { resetLocked(0); portEXIT_CRITICAL(&stateMux); return; }
    } else if (state.queued != Op::None) { op = state.queued; state.queued = Op::None; }
    else if (state.ackPending) op = Op::Ack;
    else if (onPage && state.view == View::Live && state.active && state.exchange[0]) {
      // Fetch drafts every 800 ms independently of status. Serial command
      // work may delay either poll; when both are due, serve the older
      // deadline first so neither can starve. ACKs do not postpone a poll.
      const uint32_t textElapsed = now - state.lastTextPoll;
      const uint32_t statusElapsed = now - state.lastStatusPoll;
      const bool textDue = textElapsed >= kTextPollMs;
      const bool statusDue = statusElapsed >= kStatusPollMs;
      if (textDue && (!statusDue || textElapsed - kTextPollMs >= statusElapsed - kStatusPollMs)) op = Op::Next;
      else if (statusDue) op = Op::Status;
    } else if ((uint32_t)(now - state.lastPoll) >= (state.active ? 500u : 1000u)) op = state.nextPoll ? Op::Next : Op::Status;
    switch (op) {
      case Op::Start: snprintf(command, sizeof(command), "transcription start"); break;
      case Op::Status: snprintf(command, sizeof(command), "transcription status%s%s", !state.recoveringStart && state.exchange[0] ? " " : "", state.recoveringStart ? "" : state.exchange); break;
      case Op::Next: snprintf(command, sizeof(command), "transcription next %s", state.exchange); break;
      case Op::Ack: snprintf(command, sizeof(command), "transcription ack %s %lu %lu %lu", state.exchange,
          (unsigned long)state.sequence, (unsigned long)state.receiptOffset, (unsigned long)state.receiptLength); break;
      case Op::Stop: snprintf(command, sizeof(command), "transcription stop %s", state.exchange); break;
      case Op::Cancel: snprintf(command, sizeof(command), "transcription cancel %s", state.exchange); break;
      case Op::List: snprintf(command, sizeof(command), "transcripts list %s %lu", state.sd ? "sd" : "internal", (unsigned long)state.listOffset); break;
      case Op::Read: snprintf(command, sizeof(command), "transcripts read \"%s\" %lu", state.path, (unsigned long)state.fileOffset); break;
      case Op::Save: snprintf(command, sizeof(command), "sttsavetranscripts %u", state.saveDefault ? 0 : 1); break;
      case Op::Mic: snprintf(command, sizeof(command), "micsource %s",
          TranscriptionUI::nextMicSource(state.micSource, state.micPdm, state.micG2)); break;
      default: break;
    }
    if (op != Op::None) {
      state.pending = op; state.requestView = state.viewGeneration; request = ++state.request; state.lastPoll = now;
      if (op == Op::Next || op == Op::Start) state.lastTextPoll = now;
      if (op == Op::Status || op == Op::Start || op == Op::Stop) state.lastStatusPoll = now;
    }
  }
  dirty = state.dirty;
  portEXIT_CRITICAL(&stateMux);
  if (command[0]) {
    G2CmdCookie cookie{}; cookie.targetPage = G2_HIJACK_PAGE_TRANSCRIPTION;
    if (!g2SubmitHijackCommand(command, cookie, complete, reinterpret_cast<void*>(static_cast<uintptr_t>(request)), epoch)) {
      portENTER_CRITICAL(&stateMux);
      if (state.request == request) {
        state.queued = state.pending; state.pending = Op::None;
        snprintf(state.message, sizeof(state.message), "Busy - waiting to submit"); state.dirty = true;
      }
      portEXIT_CRITICAL(&stateMux);
    }
  }
  if (dirty) enqueueRender();
}
#endif
