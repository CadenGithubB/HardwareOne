#include "G2_Page_Transcription.h"
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
enum class View : uint8_t { Home, Live, List, File };
enum class Op : uint8_t { None, Start, Status, Next, Ack, Stop, Cancel, List, Read, Save };
struct Entry { char name[64] = {}, path[128] = {}; uint32_t bytes = 0; };
struct State {
  uint32_t epoch = 0, request = 0, requestView = 0, viewGeneration = 0;
  uint32_t lastPoll = 0, listOffset = 0, listNext = 0, fileOffset = 0, fileNext = 0;
  uint32_t sequence = 0, receiptOffset = 0, receiptLength = 0;
  uint32_t elapsedMs = 0, history[32] = {};
  uint8_t historyCount = 0;
  Op pending = Op::None, queued = Op::None;
  View view = View::Home;
  bool active = false, done = false, available = false, busy = false, preparing = false;
  bool captureActive = false, inferenceActive = false, saveDefault = false, saveEnabled = false;
  bool saveComplete = false, sd = false, more = false, eof = false;
  bool exiting = false, dirty = false, ackPending = false, nextPoll = false;
  bool recoveringStart = false, exitReady = false;
  uint8_t count = 0, page = 0, pages = 0;
  char exchange[17] = {}, message[96] = {}, saveError[64] = {};
  char tail[kTailBytes + 1] = {}, fileText[kWindowBytes + 1] = {}, path[128] = {};
  Entry entries[kEntries];
};
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static State state;
// Only the lens-applier renders. Its scratch stays off the 8 KiB worker stack.
EXT_RAM_BSS_ATTR static State renderState;
EXT_RAM_BSS_ATTR static char rows[12][72], sidebar[512], textBody[1200], textPage[304];
static const char* rowPtrs[12];
static uint16_t pageOffsets[9];
static View renderedView = View::File;
static char renderedRows[12][72];
static size_t renderedCount = 0;
static uint32_t renderedEpoch = 0;
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
  if (separator && state.tail[0]) TranscriptionUI::appendRecent(state.tail, sizeof(state.tail), " ", 1);
  TranscriptionUI::appendRecent(state.tail, sizeof(state.tail), text, length);
  if (state.view == View::Live) state.page = 255; // follow newly accepted words
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
  state.view = view; ++state.viewGeneration; state.page = 0; state.pages = 0; state.dirty = true;
}
static void exitText() {
  g2BumpMenuGen();
  portENTER_CRITICAL(&stateMux);
  changeViewLocked(state.view == View::File ? View::List : View::Home);
  portEXIT_CRITICAL(&stateMux);
  enqueueRender();
}
static void navigateText(G2TapKind kind) {
  portENTER_CRITICAL(&stateMux);
  if (state.pending == Op::Read || state.queued == Op::Read) { portEXIT_CRITICAL(&stateMux); return; }
  if (kind == G2_TAP_PAGE_NEXT) {
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
    TranscriptionUI::clear(textBody, sizeof(textBody));
    TranscriptionUI::clear(textPage, sizeof(textPage));
    TranscriptionUI::clear(sidebar, sizeof(sidebar));
    const char* locked[] = {"<- Apps", "Sign in and pair the glasses"};
    (void)g2ShowListPage(locked, 2);
    return;
  }
  const auto& s = renderState;
  bool shown = false;
  if (s.view == View::Live || s.view == View::File) {
    const char* text = s.view == View::Live ? s.tail :
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
    const bool sameView = renderedEpoch == s.epoch && renderedView == s.view;
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
    else if (identity.stillCurrent()) shown = g2ShowTextPage(textPage, G2_GEOM_LARGE, exitText, navigateText);
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
      const char* phase = s.preparing ? "Getting ready" : s.captureActive ? "Listening" :
                          s.inferenceActive ? "Transcribing" : s.active ? "Stopping" : s.done ? "Finished" : "Ready";
      const size_t tailLen = strlen(s.tail);
      const char* tail = s.tail + (tailLen > 160 ? tailLen - 160 : 0);
      while ((*tail & 0xc0) == 0x80) ++tail;
      snprintf(sidebar, sizeof(sidebar), "%s%s\n%lus%s\n%.95s%s%.63s\nRecent:\n%.160s", phase,
               s.pending != Op::None ? "..." : "", (unsigned long)(s.elapsedMs / 1000),
               s.active ? (s.saveEnabled ? " | saving" : " | not saving") : (s.saveComplete ? " | saved" : ""),
               s.message, s.saveError[0] ? "\nSave: " : "", s.saveError, tail);
    } else {
      add("<- Transcription");
      for (size_t i = 0; i < s.count; ++i) add(s.entries[i].name);
      if (!s.count) add("(no saved transcripts)");
      if (s.listOffset) add("<< Previous files");
      if (s.more) add("Next files >>");
      snprintf(sidebar, sizeof(sidebar), "%s storage\nSaved transcripts\n%s",
               s.sd ? "SD" : "Internal", s.message);
    }
    const G2TextChildSpec child = {"transcription", sidebar, 98, G2_GEOM_SPLIT_RIGHT, false};
    const bool sameRows = renderedEpoch == s.epoch && renderedView == s.view && renderedCount == n &&
        memcmp(renderedRows, rows, n * sizeof(rows[0])) == 0;
    if (identity.stillCurrent()) {
      if (sameRows) shown = g2UpdateMixedTextChild("transcription", 98, sidebar);
      if (!shown) shown = g2ShowMixedListText(rowPtrs, n, G2_GEOM_SPLIT_LIST, child);
    }
    if (shown) { memcpy(renderedRows, rows, n * sizeof(rows[0])); renderedView = s.view; renderedCount = n; renderedEpoch = s.epoch; }
  }
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
  if (op == Op::Save) {
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
    snprintf(state.saveError, sizeof(state.saveError), "%s", doc["saveError"] | "");
    const char* failure = doc["failure"] | "";
    if (failure[0]) snprintf(state.message, sizeof(state.message), "%s", failure);
    if (!state.available && !state.active && !state.message[0])
      snprintf(state.message, sizeof(state.message), "%s", doc["reason"] | "Unavailable");
    if (op == Op::Start) {
      state.tail[0] = '\0'; state.message[0] = '\0'; state.ackPending = false;
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
        state.sequence = doc["sequence"] | 0u; state.receiptOffset = doc["offset"] | 0u;
        state.receiptLength = length;
        appendTailLocked(text, length, state.receiptOffset == 0);
        state.ackPending = true;
      } else snprintf(state.message, sizeof(state.message), "Invalid text receipt");
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
  if (!onPage) state.exiting = true;
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
    else if ((uint32_t)(now - state.lastPoll) >= 1000) op = state.nextPoll ? Op::Next : Op::Status;
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
      default: break;
    }
    if (op != Op::None) { state.pending = op; state.requestView = state.viewGeneration; request = ++state.request; state.lastPoll = now; }
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
