// G2_Page_Notes.cpp — Apps → Notes: QuickNote on the lens, and the consumer
// of a ring/temple hold.
//
// The page records and stores nothing itself. Every mutation is a `note …`
// command that cmd_exec runs as the paired wearer (g2SubmitHijackCommand), so
// the QuickNote service stays the only dictation lease holder and applies the
// CLI's authority, privacy and limits.
//
//   List       <- Apps, ● New note (a tap starts or stops a note), up to six
//              "<when> title" rows (`note list`'s "when": hh:mm, or "5m ago" /
//              "earlier" for a note saved before the clock was set, as of the
//              listing: a rebuild per minute would lose the wearer's place),
//              More…, and while a card is in "Flash notes · tap for card" (or
//              the reverse): the list starts on the tier a save uses now, and
//              this row lists the other one; header Ready or why not, body
//              "Hold to record".
//   Recording  the same rows. Header "● REC 0:07 · release to save"; "speak
//              now" only once the service reports live capture. A container
//              rebuild stops the glasses' LC3 mic stream, so while a note runs
//              only the two text children change (UPDATE_TEXT).
//   Saving, then Saved ("✓ Saved · 9 words" over its first 150 chars) or why
//              it was not saved, for 5 s.
//   Note       <- Notes, Read, Delete (a second tap within 4 s deletes).
//   Reader     the note's text, one 512 B `note read` window at a time.
//
// Holds: on this page a press starts a hold note, and its release or abort
// stops it (the service saves it, or discards a hold that never captured or
// ran under kMinHoldMs). From any other HW1 page the global hold action does
// the same after showing this page; a hold that cannot start is refused where
// it was made and never navigates. If the lens blanks or the page is left
// mid-note, the note carries on headless and this page still stops it.
// While the page is open it asks the service to have the note backend ready
// (quickNoteWarm: on local-STT builds the model is resident, not reloaded).
#include "G2_Page_Notes.h"
#if ENABLE_QUICKNOTE
#include "G2_Glasses.h"
#include "G2_HijackCmd.h"
#include "G2_Page_Common.h"
#include "BLE_Peers.h"
#include "System_MemUtil.h"
#include "System_QuickNote.h"   // quickNoteCanStart
#include "System_TextPager.h"
#include "Transcription_UI_Policy.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>

extern void g2ShowAppsMenu();

namespace {
constexpr size_t kRows = 6, kPreviewMax = 150, kWindowBytes = 512, kHistory = 12;
// Each right-column child is refreshed by a single-write UPDATE_TEXT; the G2
// MTU leaves room for at most 180 B of text (see G2_Page_Transcription.cpp).
constexpr size_t kChildMax = 180;
constexpr G2ContainerGeom kHeaderGeom = {280, 8, 288, 88};
constexpr G2ContainerGeom kBodyGeom = {280, 100, 288, 180};
constexpr uint32_t kHeaderId = 97, kBodyId = 98;
constexpr uint32_t kPollMs = 500, kIdlePollMs = 5000, kDoneMs = 5000, kConfirmMs = 4000;
enum class View : uint8_t { List, Note, Reader };
// The note this page started or adopted, as the page last saw it.
enum class Rec : uint8_t { Idle, Recording, Saving, Done };
enum class Op : uint8_t { None, Status, Start, Stop, Cancel, List, Read, Delete };
// The tier `note list` is asked for; Auto is the one a save would use now.
enum class Tier : uint8_t { Auto, Internal, Sd };
// "<when> <title>": when <= 11 bytes ("49710d ago"), the title <= 48.
struct Entry { char label[64] = {}, path[QuickNotePolicy::kPathMax] = {}; };
struct State {
  uint32_t epoch = 0, request = 0, requestView = 0, viewGeneration = 0, showGen = 0;
  uint32_t lastPoll = 0, doneAt = 0, confirmAt = 0, elapsedMs = 0, holdPress = 0;
  uint32_t listOffset = 0, listNext = 0, fileOffset = 0, fileNext = 0, history[kHistory] = {};
  uint8_t historyCount = 0, count = 0, page = 0, pages = 0;
  Op pending = Op::None, queued = Op::None;
  View view = View::List;
  Rec rec = Rec::Idle;
  // The tier wanted and the one the listing in flight asked for; the last
  // reply's tier (listedSd) and whether it saw a card (sdAvailable).
  Tier tier = Tier::Auto, sentTier = Tier::Auto;
  bool listedSd = false, sdAvailable = false;
  bool live = false, hold = false, capture = false, stopAfterStart = false;
  bool pressOpen = false, pagePress = false, wantList = false, more = false;
  bool eof = false, confirm = false, dirty = false;
  char id[17] = {}, message[64] = {}, label[64] = {}, path[QuickNotePolicy::kPathMax] = {};
  char preview[kPreviewMax + 1] = {}, text[kWindowBytes + 1] = {};
  Entry entries[kRows];
};
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static State state;
// Only the lens applier renders. Its scratch stays off the worker stack.
EXT_RAM_BSS_ATTR static State renderState;
// Back, New note, the notes, More… and the tier row.
EXT_RAM_BSS_ATTR static char rows[kRows + 4][64], header[kChildMax + 1], body[kChildMax + 1];
EXT_RAM_BSS_ATTR static char renderedRows[kRows + 4][64], renderedHeader[kChildMax + 1];
EXT_RAM_BSS_ATTR static char renderedBody[kChildMax + 1], textBody[1200], textPage[304], renderedText[304];
// A listing reply, labelled outside stateMux. cmd_exec only.
EXT_RAM_BSS_ATTR static Entry listed[kRows];
static const char* rowPtrs[kRows + 4];
static uint16_t pageOffsets[9];
static View renderedView = View::List;
static size_t renderedCount = 0;
static uint32_t renderedEpoch = 0, renderedShow = 0;

static bool ownerLive(uint32_t epoch) {
  BlePeerOwnerSession owner;
  return epoch && blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner)
      && owner.live() && owner.transportEpoch == epoch;
}
static void resetLocked(uint32_t epoch) {
  const uint32_t request = state.request + 1, show = state.showGen;
  state = State{};
  state.epoch = epoch; state.request = request; state.showGen = show; state.dirty = true;
}
static void changeViewLocked(View view) {
  state.view = view; ++state.viewGeneration; state.page = 0; state.pages = 0; state.dirty = true;
}
// This page's note holds the dictation lease, or is about to (a start queued
// or in flight), or is still finishing.
static bool noteBusyLocked() {
  return state.live || state.rec == Rec::Recording || state.rec == Rec::Saving;
}
static void beginNoteLocked(bool hold) {
  state.rec = Rec::Recording; state.hold = hold; state.live = state.capture = false;
  state.stopAfterStart = false; state.id[0] = '\0'; state.message[0] = '\0'; state.elapsedMs = 0;
  TranscriptionUI::clear(state.preview, sizeof(state.preview));
  state.queued = Op::Start; state.dirty = true;
}
// Ends this page's note: an unsent start is dropped, a start in flight is
// cancelled as soon as its reply names the note (nothing can have been said),
// and a live note is stopped, which saves it (the service discards a hold
// that never captured or ran under kMinHoldMs, on its own capture clock).
static void stopNoteLocked() {
  state.pressOpen = state.pagePress = false; state.holdPress = 0; state.dirty = true;
  if (state.pending == Op::Start) state.stopAfterStart = true;
  else if (state.queued == Op::Start) { state.queued = Op::None; state.rec = Rec::Idle; return; }
  else if (state.live) state.queued = Op::Stop;
  else return;
  state.rec = Rec::Saving;
}
// Whether the rows are (or are about to be) the card's, and whether the list
// offers the other tier: once a reply saw a card, or while the card is shown.
static bool cardShown(const State& s) {
  return s.tier == Tier::Auto ? s.listedSd : s.tier == Tier::Sd;
}
static bool tierRowShown(const State& s) { return s.sdAvailable || cardShown(s); }

static void render();
static void enqueueRender() {
  if (g2GetHijackPage() != G2_HIJACK_PAGE_NOTES) return;
  auto* spec = new (std::nothrow) RedrawSpec{};
  auto* job = new (std::nothrow) LensUiJob{};
  if (!spec || !job) { delete spec; delete job; return; }
  spec->render = render;
  job->kind = LensJobKind::Redraw; job->submitMenuGen = g2CurrentMenuGen();
  job->targetPage = G2_HIJACK_PAGE_NOTES; job->payload.redraw = spec;
  portENTER_CRITICAL(&stateMux); state.dirty = false; portEXIT_CRITICAL(&stateMux);
  if (!g2EnqueueLensJob(job, G2LensEnqueueWait::NoWait)) {
    delete spec; delete job;
    portENTER_CRITICAL(&stateMux); state.dirty = true; portEXIT_CRITICAL(&stateMux);
  }
}
static void exitReader() {
  g2BumpMenuGen();
  portENTER_CRITICAL(&stateMux); changeViewLocked(View::Note); portEXIT_CRITICAL(&stateMux);
  enqueueRender();
}
static void navigateReader(G2TapKind kind) {
  portENTER_CRITICAL(&stateMux);
  if (state.view != View::Reader || state.pending == Op::Read || state.queued == Op::Read) {
    portEXIT_CRITICAL(&stateMux); return;
  }
  if (kind == G2_TAP_PAGE_NEXT) {
    if (state.page + 1 < state.pages) ++state.page;
    else if (!state.eof) {
      if (state.historyCount < kHistory) state.history[state.historyCount++] = state.fileOffset;
      state.fileOffset = state.fileNext; state.queued = Op::Read; ++state.viewGeneration;
    }
  } else if (state.page) --state.page;
  else if (state.fileOffset) {
    // A note is at most ~9 windows, inside the history; past it, rewind.
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
  const State& s = renderState;
  if (g2GetHijackPage() != G2_HIJACK_PAGE_NOTES || !g2LensGetState().hijackActive) { renderState = State{}; return; }
  if (!ownerLive(s.epoch) || !identity.stillCurrent()) {
    renderState = State{}; renderedCount = 0; renderedEpoch = 0;
    TranscriptionUI::clear(renderedText, sizeof(renderedText));
    TranscriptionUI::clear(renderedBody, sizeof(renderedBody));
    TranscriptionUI::clear(textBody, sizeof(textBody));
    TranscriptionUI::clear(textPage, sizeof(textPage));
    const char* locked[] = {"<- Apps", "Sign in and pair the glasses"};
    (void)g2ShowListPage(locked, 2);
    return;
  }
  bool shown = false;
  const bool sameShow = renderedEpoch == s.epoch && renderedShow == s.showGen && renderedView == s.view;
  if (s.view == View::Reader) {
    bool truncated = false;
    const size_t bytes = textWrapInto(textBody, sizeof(textBody), s.text[0] ? s.text : s.message,
                                      G2_TEXT_DEFAULT_COLS, 0, true, &truncated);
    TextPager pager = {textBody, pageOffsets, 8, 176, 0, 0, truncated};
    textSplitPages(pager, bytes);
    pager.curPage = std::min<int>(s.page, std::max(0, pager.pageCount - 1));
    portENTER_CRITICAL(&stateMux);
    if (state.epoch == s.epoch && state.viewGeneration == s.viewGeneration) {
      state.page = pager.curPage; state.pages = pager.pageCount;
    }
    portEXIT_CRITICAL(&stateMux);
    const size_t begin = pageOffsets[pager.curPage];
    const size_t end = pager.pageCount ? pageOffsets[pager.curPage + 1] : begin;
    snprintf(textPage, sizeof(textPage), "Note [%d/%d]%s 2x=back\n%.*s", pager.curPage + 1,
             std::max(1, pager.pageCount), pager.pageCount > 1 || !s.eof ? " scroll=more" : "",
             static_cast<int>(end - begin), textBody + begin);
    if (sameShow && strcmp(renderedText, textPage) == 0) shown = true;
    else if (identity.stillCurrent()) shown = g2ShowTextPage(textPage, G2_GEOM_LARGE, exitReader, navigateReader);
    if (shown) {
      snprintf(renderedText, sizeof(renderedText), "%s", textPage);
      renderedView = s.view; renderedEpoch = s.epoch; renderedShow = s.showGen; renderedCount = 0;
    }
  } else {
    size_t n = 0;
    memset(rows, 0, sizeof(rows));
    auto add = [&](const char* text) { snprintf(rows[n], sizeof(rows[n]), "%s", text); rowPtrs[n] = rows[n]; ++n; };
    const char* head = s.message[0] ? s.message : "Ready";
    const char* text = "Hold to record";
    if (s.view == View::Note) {
      add("<- Notes"); add("Read"); add("Delete");
      snprintf(header, sizeof(header), "%.63s%s", s.label, s.confirm ? "\nTap Delete again to delete" : "");
      head = header; text = s.preview[0] ? s.preview : s.message;
    } else {
      add("<- Apps"); add("● New note");
      for (size_t i = 0; i < s.count; ++i) add(s.entries[i].label);
      if (s.more || s.listOffset) add("More…");
      if (tierRowShown(s)) add(cardShown(s) ? "Card notes · tap for flash" : "Flash notes · tap for card");
      if (s.rec == Rec::Recording) {
        const unsigned sec = s.elapsedMs / 1000;
        snprintf(header, sizeof(header), "● REC %u:%02u · %s", sec / 60, sec % 60,
                 s.pressOpen ? "release to save" : "tap New note to save");
        head = header; text = s.capture ? "speak now" : "";
      } else if (s.rec == Rec::Saving) {
        head = "Saving…"; text = "";
      } else if (s.rec == Rec::Done) {
        text = s.preview;
      }
    }
    // Every source is shorter than a child (message 63 B, preview 150 B).
    if (head != header) snprintf(header, sizeof(header), "%.180s", head);
    snprintf(body, sizeof(body), "%.180s", text);
    const G2TextChildSpec headSpec = {"nthead", header, kHeaderId, kHeaderGeom, false};
    const G2TextChildSpec bodySpec = {"notes", body, kBodyId, kBodyGeom, false};
    const bool drawn = sameShow && renderedCount;
    const bool sameRows = drawn && renderedCount == n && memcmp(renderedRows, rows, n * sizeof(rows[0])) == 0;
    // While a note may be capturing, the page this show already drew is
    // only ever patched: new rows (a listing that landed) wait for its end.
    const bool micBusy = s.live || s.rec == Rec::Recording;
    if (identity.stillCurrent()) {
      if (sameRows || (drawn && micBusy)) {
        const bool headOk = strcmp(renderedHeader, header) == 0 ||
            g2UpdateMixedTextChild("nthead", kHeaderId, header);
        const bool bodyOk = strcmp(renderedBody, body) == 0 ||
            g2UpdateMixedTextChild("notes", kBodyId, body);
        if (headOk) snprintf(renderedHeader, sizeof(renderedHeader), "%s", header);
        if (bodyOk) snprintf(renderedBody, sizeof(renderedBody), "%s", body);
        shown = headOk && bodyOk;
      }
      if (!shown && !(drawn && micBusy) &&
          g2ShowMixedListText2(rowPtrs, n, G2_GEOM_SPLIT_LIST, headSpec, bodySpec)) {
        shown = true;
        memcpy(renderedRows, rows, n * sizeof(rows[0])); renderedCount = n;
        renderedView = s.view; renderedEpoch = s.epoch; renderedShow = s.showGen;
        snprintf(renderedHeader, sizeof(renderedHeader), "%s", header);
        snprintf(renderedBody, sizeof(renderedBody), "%s", body);
        renderedText[0] = '\0';
      }
    }
  }
  if (!shown) { portENTER_CRITICAL(&stateMux); state.dirty = true; portEXIT_CRITICAL(&stateMux); }
  renderState = State{};
}

// Every reply field is read through one of these out-of-line readers, so each
// costs a call rather than an inlined ArduinoJson lookup (S3 flash).
__attribute__((noinline)) const char* str(JsonVariantConst at, const char* key, const char* fallback = "") {
  return at[key] | fallback;
}
__attribute__((noinline)) uint32_t num(JsonVariantConst at, const char* key, uint32_t fallback = 0) {
  return at[key] | fallback;
}
__attribute__((noinline)) bool flag(JsonVariantConst at, const char* key, bool fallback = false) {
  return at[key] | fallback;
}

// A note reply (start, status, stop, cancel): the live note, or how it ended.
// Caller holds stateMux.
static void applyNoteLocked(JsonVariantConst doc, uint32_t now) {
  uint64_t parsedId = 0;
  const char* id = str(doc, "id");
  if (flag(doc, "active") && TranscriptionUI::parseId(id, parsedId)) {
    snprintf(state.id, sizeof(state.id), "%s", id);
    state.live = true; state.capture = flag(doc, "captureActive");
    state.hold = flag(doc, "hold"); state.elapsedMs = num(doc, "elapsedMs");
    const char* phase = str(doc, "phase");
    if (state.stopAfterStart) {
      state.stopAfterStart = false; state.queued = Op::Cancel; state.rec = Rec::Saving;
    } else if (state.rec != Rec::Saving) {
      state.rec = strcmp(phase, "finishing") && strcmp(phase, "saving") ? Rec::Recording : Rec::Saving;
    }
    return;
  }
  const char* outcome = str(doc, "outcome");
  const bool ended = state.live || state.id[0] || state.rec == Rec::Recording || state.rec == Rec::Saving;
  state.live = state.capture = state.stopAfterStart = false; state.id[0] = '\0';
  state.rec = outcome[0] ? Rec::Done : Rec::Idle; state.doneAt = now;
  if (ended) {
    state.wantList = true;
    // A note that ended lists where saves go now (the new note's tier).
    if (state.tier != Tier::Auto) { state.tier = Tier::Auto; state.listOffset = 0; }
  }
  if (!strcmp(outcome, "saved")) {
    snprintf(state.message, sizeof(state.message), "✓ Saved · %lu words",
             static_cast<unsigned long>(num(doc, "words")));
    snprintf(state.path, sizeof(state.path), "%s", str(doc, "path"));
    if (state.path[0]) state.queued = Op::Read;  // the preview
  } else if (outcome[0]) {
    snprintf(state.message, sizeof(state.message), "%s: %s",
             strcmp(outcome, "failed") ? "Not saved" : "Failed", str(doc, "reason"));
  } else {
    snprintf(state.message, sizeof(state.message), "%s", flag(doc, "available") ? "" : str(doc, "reason"));
  }
}

static void complete(bool ok, const char* result, const G2CmdCookie&, void* opaque) {
  const uint32_t request = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(opaque));
  uint32_t epoch;
  portENTER_CRITICAL(&stateMux); epoch = state.epoch; portEXIT_CRITICAL(&stateMux);
  if (!ownerLive(epoch)) return;
  PSRAM_JSON_DOC(doc);
  const bool parsed = result && !deserializeJson(doc, result);
  const bool success = ok && parsed && flag(doc, "success");
  JsonArrayConst notes = doc["notes"].as<JsonArrayConst>();
  size_t count = 0;
  for (JsonObjectConst row : notes) {
    if (count == kRows) break;
    Entry& e = listed[count++];
    const char* title = str(row, "title");
    snprintf(e.label, sizeof(e.label), "%s %s", str(row, "when", "earlier"), title[0] ? title : "note");
    snprintf(e.path, sizeof(e.path), "%s", str(row, "path"));
  }
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateMux);
  if (state.request == request && state.pending != Op::None && state.epoch == epoch) {
    const Op op = state.pending;
    state.pending = Op::None;
    if (op != Op::Start && op <= Op::Cancel && state.queued == Op::Start) {
      // A start queued since supersedes what an older note reply says.
    } else if (op == Op::Start && !(ok && parsed)) {
      // Admission may have happened before the reply was lost: adopt the
      // wearer's live note, if any, rather than start a second one.
      state.queued = Op::Status;
    } else if (!success) {
      snprintf(state.message, sizeof(state.message), "%s", parsed ? str(doc, "error", "Request failed") : "Request failed");
      if (op == Op::Start) {
        state.rec = Rec::Idle; state.stopAfterStart = state.pressOpen = false; state.holdPress = 0;
      } else if ((op == Op::Stop || op == Op::Cancel) && state.id[0]) {
        state.queued = Op::Status;   // it already ended: show how
      } else if (op == Op::Status) {
        state.live = false; state.id[0] = '\0';
        if (state.rec != Rec::Done) state.rec = Rec::Idle;
      }
    } else if (op <= Op::Cancel) {
      applyNoteLocked(doc, now);
    } else if (op == Op::Delete) {
      changeViewLocked(View::List); state.wantList = true;
      snprintf(state.message, sizeof(state.message), "Note deleted");
    } else if (state.requestView == state.viewGeneration && op == Op::List &&
               state.sentTier == state.tier) {   // never a tier no longer wanted
      memcpy(state.entries, listed, count * sizeof(Entry)); state.count = count;
      state.listOffset = num(doc, "offset"); state.listNext = state.listOffset + count;
      state.more = notes.size() > kRows || flag(doc, "more");
      state.listedSd = !strcmp(str(doc, "tier"), "sd"); state.sdAvailable = flag(doc, "sdAvailable");
    } else if (state.requestView == state.viewGeneration && op == Op::Read) {
      const char* window = str(doc, "sttText");
      const uint32_t offset = num(doc, "offset");
      // A note file opens with its header; the lens shows only the text.
      const char* start = offset ? nullptr : strstr(window, "\n\n");
      if (start) window = start + 2;
      if (state.view == View::Reader) {
        snprintf(state.text, sizeof(state.text), "%s", window);
        state.fileOffset = offset; state.fileNext = num(doc, "nextOffset", offset);
        state.eof = flag(doc, "eof", true); state.page = 0;
      } else {
        size_t n = strlen(window);
        if (n > kPreviewMax) n = TranscriptionUI::completePrefix(window, kPreviewMax);
        memcpy(state.preview, window, n); state.preview[n] = '\0';
      }
    }
    state.dirty = true;
  }
  portEXIT_CRITICAL(&stateMux);
  // Titles are the start of each note's text.
  TranscriptionUI::clear(reinterpret_cast<char*>(listed), sizeof(listed));
}

// A hold may start a note only for the paired wearer, while the service can
// take one now. Not inside a critical section: the store check takes the
// filesystem lock (bounded).
static bool beginHold(uint32_t pressId, bool fromPage, const char** why) {
  BlePeerOwnerSession owner;
  const char* reason = "Pair glasses as a signed-in user";
  bool ok = blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner) && owner.live() &&
      quickNoteCanStart(&reason);
  portENTER_CRITICAL(&stateMux);
  if (ok && state.epoch == owner.transportEpoch && noteBusyLocked()) {
    ok = false; reason = "A note is already recording";
  }
  if (!ok && !reason) reason = "Note unavailable";
  if (ok) {
    if (state.epoch != owner.transportEpoch) resetLocked(owner.transportEpoch);
    // From another page this is a fresh show: its CREATE is queued ahead of
    // the start, so the mic comes up on a container that stays (one still in
    // flight re-arms the mic when it lands: g2LensApplyContainer).
    if (!fromPage || state.view != View::List) changeViewLocked(View::List);
    if (!fromPage) { ++state.showGen; state.listOffset = 0; state.wantList = true; state.tier = Tier::Auto; }
    beginNoteLocked(true);
    state.pressOpen = true; state.pagePress = fromPage; state.holdPress = pressId;
  } else if (fromPage) {
    snprintf(state.message, sizeof(state.message), "%s", reason);
    state.dirty = true;
  }
  portEXIT_CRITICAL(&stateMux);
  if (why) *why = reason;
  if (ok && !fromPage) g2SetHijackPage(G2_HIJACK_PAGE_NOTES);
  if (ok || fromPage) enqueueRender();   // a refusal elsewhere draws nothing
  return ok;
}
// The end (release or abort) of the press that started this page's note.
static void endHold(uint32_t pressId, bool fromPage) {
  portENTER_CRITICAL(&stateMux);
  const bool mine = state.pressOpen && state.pagePress == fromPage &&
      (fromPage || state.holdPress == pressId);
  if (mine) stopNoteLocked();
  portEXIT_CRITICAL(&stateMux);
  if (mine) enqueueRender();
}
} // namespace

void g2BuildNotesInfo(char* out, size_t cap) {
  if (out && cap) snprintf(out, cap, "Notes: open Apps > Notes on the lens, or hold the ring on any HW1 page, to record a private note.");
}
void g2ShowNotesMenu() {
  BlePeerOwnerSession owner;
  const bool live = blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner) && owner.live();
  portENTER_CRITICAL(&stateMux);
  if (!live || state.epoch != owner.transportEpoch) resetLocked(live ? owner.transportEpoch : 0);
  changeViewLocked(View::List);
  ++state.showGen; state.listOffset = 0; state.wantList = true; state.confirm = false; state.tier = Tier::Auto;
  // Picks up a note already running for the wearer (any trigger) first.
  if (state.pending == Op::None && state.queued == Op::None) state.queued = Op::Status;
  portEXIT_CRITICAL(&stateMux);
  g2SetHijackPage(G2_HIJACK_PAGE_NOTES);
  enqueueRender();
}
void g2NotesHandleTap(uint32_t index) {
  uint32_t epoch;
  portENTER_CRITICAL(&stateMux); epoch = state.epoch; portEXIT_CRITICAL(&stateMux);
  if (!ownerLive(epoch)) { if (index == 0) g2ShowAppsMenu(); return; }
  const uint32_t now = millis();
  bool back = false, viewChanged = false;
  portENTER_CRITICAL(&stateMux);
  if (state.view == View::Note) {
    if (index == 0) { changeViewLocked(View::List); viewChanged = true; }
    else if (index == 1) {
      changeViewLocked(View::Reader); state.fileOffset = 0; state.historyCount = 0;
      state.text[0] = '\0'; state.eof = true; state.queued = Op::Read; viewChanged = true;
    } else if (index == 2) {
      state.confirm = !state.confirm || now - state.confirmAt >= kConfirmMs;
      state.confirmAt = now;
      if (!state.confirm) state.queued = Op::Delete;
    }
  } else if (state.view == View::List) {
    const bool busy = noteBusyLocked();
    if (state.rec == Rec::Idle) state.message[0] = '\0';
    if (index == 0) { if (busy) stopNoteLocked(); back = true; }
    else if (index == 1) { if (busy) stopNoteLocked(); else beginNoteLocked(false); }
    else if (!busy && index - 2 < state.count) {
      const Entry& e = state.entries[index - 2];
      memcpy(state.label, e.label, sizeof(state.label)); memcpy(state.path, e.path, sizeof(state.path));
      TranscriptionUI::clear(state.preview, sizeof(state.preview));
      state.rec = Rec::Idle; state.message[0] = '\0';   // dismisses a Saved banner
      changeViewLocked(View::Note); state.confirm = false; state.queued = Op::Read; viewChanged = true;
    } else if (!busy && index == 2 + state.count && (state.more || state.listOffset)) {
      state.listOffset = state.more ? state.listNext : 0; state.queued = Op::List;
    } else if (!busy && tierRowShown(state) &&
               index == 2u + state.count + (state.more || state.listOffset)) {
      // The other tier from its newest note; this tier's rows go at once.
      state.tier = cardShown(state) ? Tier::Internal : Tier::Sd;
      state.listOffset = 0; state.count = 0; state.more = false; state.wantList = true;
    }
  }
  state.dirty = true;
  portEXIT_CRITICAL(&stateMux);
  if (viewChanged) g2BumpMenuGen();
  if (back) g2ShowAppsMenu(); else enqueueRender();
}
bool g2NotesHandleHold(uint8_t phase, uint8_t source) {
  if (phase != static_cast<uint8_t>(G2HoldPhase::Press)) { endHold(0, true); return true; }
  // Ring only (g2holdsource 0) on firmware that names the source: a temple
  // press is declined, and the dispatcher records it as "source".
  if (!g2HoldSourceAdmitted(source)) return false;
  return beginHold(0, true, nullptr);
}
bool g2NotesHoldBegin(uint32_t pressId, const char** why) { return beginHold(pressId, false, why); }
void g2NotesHoldEnd(uint32_t pressId, uint32_t) { endHold(pressId, false); }
void g2NotesHoldAbort(uint32_t pressId, G2HoldReason) { endHold(pressId, false); }
void g2NotesTick() {
  const uint32_t now = millis();
  uint32_t epoch;
  bool dirty;
  portENTER_CRITICAL(&stateMux); epoch = state.epoch; dirty = state.dirty; portEXIT_CRITICAL(&stateMux);
  if (!epoch) { if (dirty) enqueueRender(); return; }
  if (!ownerLive(epoch)) {
    portENTER_CRITICAL(&stateMux); if (state.epoch == epoch) resetLocked(0); portEXIT_CRITICAL(&stateMux);
    enqueueRender(); return;
  }
  char command[128] = {};
  uint32_t request = 0;
  const bool onPage = g2GetHijackPage() == G2_HIJACK_PAGE_NOTES && g2LensGetState().hijackActive;
  portENTER_CRITICAL(&stateMux);
  // Off the page with no note in flight: forget the list and any note text.
  // A running note carries on headless and is still stopped and followed here.
  if (!onPage && !noteBusyLocked() && state.pending == Op::None) {
    resetLocked(0); portEXIT_CRITICAL(&stateMux); return;
  }
  if (state.rec == Rec::Done && now - state.doneAt >= kDoneMs) {
    state.rec = Rec::Idle; state.message[0] = '\0'; state.dirty = true;
    TranscriptionUI::clear(state.preview, sizeof(state.preview));
  }
  if (state.confirm && now - state.confirmAt >= kConfirmMs) { state.confirm = false; state.dirty = true; }
  if (state.pending == Op::None) {
    Op op = state.queued;
    state.queued = Op::None;
    if (op == Op::None) {
      if (state.live) { if (now - state.lastPoll >= kPollMs) op = Op::Status; }
      else if (state.wantList) { op = Op::List; state.wantList = false; }
      else if (onPage && state.view == View::List && state.rec == Rec::Idle &&
               now - state.lastPoll >= kIdlePollMs) op = Op::Status;
    }
    if ((op == Op::Stop || op == Op::Cancel) && !state.id[0]) op = Op::Status;
    // A hold from another page starts once this page is current: its CREATE
    // goes first. Its release or abort drops the start if that never happens.
    if (op == Op::Start && state.pressOpen && !state.pagePress && !onPage) { state.queued = op; op = Op::None; }
    switch (op) {
      case Op::Status: snprintf(command, sizeof(command), "note status%s%s", state.id[0] ? " " : "", state.id); break;
      case Op::Start: snprintf(command, sizeof(command), "%s", state.hold ? "note start hold" : "note start"); break;
      case Op::Stop: snprintf(command, sizeof(command), "note stop %s", state.id); break;
      case Op::Cancel: snprintf(command, sizeof(command), "note cancel %s", state.id); break;
      case Op::List:
        state.sentTier = state.tier;
        snprintf(command, sizeof(command), "note list%s %lu",
                 state.tier == Tier::Sd ? " sd" : state.tier == Tier::Internal ? " internal" : "",
                 static_cast<unsigned long>(state.listOffset));
        break;
      case Op::Read: snprintf(command, sizeof(command), "note read \"%s\" %lu", state.path,
          static_cast<unsigned long>(state.view == View::Reader ? state.fileOffset : 0)); break;
      case Op::Delete: snprintf(command, sizeof(command), "note delete \"%s\"", state.path); break;
      default: break;
    }
    if (op != Op::None) { state.pending = op; state.requestView = state.viewGeneration; request = ++state.request; state.lastPoll = now; }
  }
  dirty = state.dirty;
  portEXIT_CRITICAL(&stateMux);
  if (onPage) quickNoteWarm();
  // Draw before submitting: a hold from another page queues this page's
  // CREATE ahead of the start that brings up the glasses mic. The start does
  // not wait for its ack; the container-ready re-arm covers the overlap.
  if (dirty) enqueueRender();
  if (command[0]) {
    G2CmdCookie cookie{}; cookie.targetPage = G2_HIJACK_PAGE_NOTES;
    if (!g2SubmitHijackCommand(command, cookie, complete, reinterpret_cast<void*>(static_cast<uintptr_t>(request)), epoch)) {
      portENTER_CRITICAL(&stateMux);
      if (state.request == request) {
        state.queued = state.pending; state.pending = Op::None;
        snprintf(state.message, sizeof(state.message), "Busy - waiting to submit"); state.dirty = true;
      }
      portEXIT_CRITICAL(&stateMux);
    }
  }
}
#endif
