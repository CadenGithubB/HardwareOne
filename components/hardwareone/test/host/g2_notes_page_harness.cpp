// Behavioural harness for the production Notes lens page (G2_Page_Notes.cpp):
// commands, JSON replies, views, holds (page handler and the global hold
// action), the no-rebuild-while-recording rule, stale replies and privacy.
// test_g2_notes_page.py splices the page at INSERT_PRODUCTION (includes
// stripped) next to the real UI policy and pager; everything else is a fake.
#include <ArduinoJson.h>
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "G2_HoldGesture.h"     // -I<component>: G2HoldPhase, G2HoldReason
#include "QuickNote_Policy.h"   // QuickNotePolicy::kPathMax

static int failures = 0;
#define CHECK(condition, message)                                            \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

#define ENABLE_QUICKNOTE 1
#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static int criticalDepth = 0;
#define portENTER_CRITICAL(m) ((m)->lock(), ++criticalDepth)
#define portEXIT_CRITICAL(m) (--criticalDepth, (m)->unlock())
// INSERT_POLICY
// INSERT_PAGER

// ---- platform and G2 fakes ---------------------------------------------------
static uint32_t clockMs = 1000, liveEpoch = 10, menuGen = 1;
static bool ownerAuthed = true, queueOkay = true, commandOkay = true, uiOkay = true;
uint32_t millis() { return clockMs; }
enum { BLE_PEER_G2_GLASSES = 1 };
struct BlePeerOwnerSession { uint32_t transportEpoch = 0; bool live() const { return transportEpoch != 0; } };
bool blePeerOwnerSessionSnapshot(int, BlePeerOwnerSession& owner) {
  owner.transportEpoch = ownerAuthed ? liveEpoch : 0;
  return ownerAuthed;
}
struct G2HijackCtxGuard { uint32_t epoch = liveEpoch; bool stillCurrent() const { return ownerAuthed && epoch == liveEpoch; } };
enum G2HijackPage : uint8_t { G2_HIJACK_PAGE_APPS = 11, G2_HIJACK_PAGE_TRANSCRIPTION = 23, G2_HIJACK_PAGE_NOTES = 24 };
struct LensState { bool hijackActive = true; };
static LensState lens;
LensState g2LensGetState() { return lens; }
static G2HijackPage page = G2_HIJACK_PAGE_APPS;
static std::vector<std::string> trace;   // lens jobs and commands, in order
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage p) { if (page != p) ++menuGen; page = p; }
void g2BumpMenuGen() { ++menuGen; }
uint32_t g2CurrentMenuGen() { return menuGen; }
void g2ShowAppsMenu() { g2SetHijackPage(G2_HIJACK_PAGE_APPS); }

// QuickNote service and hold-policy fakes.
static bool canStart = true;
static const char* canStartWhy = nullptr;
static int canStartCalls = 0;
bool quickNoteCanStart(const char** why) {
  CHECK(criticalDepth == 0, "quickNoteCanStart (filesystem lock) never runs inside stateMux");
  ++canStartCalls;
  if (why) *why = canStart ? nullptr : canStartWhy;
  return canStart;
}
static int warmCalls = 0;      // quickNoteWarm: the page asks for the model while shown
void quickNoteWarm() {
  CHECK(criticalDepth == 0, "quickNoteWarm never runs inside stateMux");
  ++warmCalls;
}
static bool ringOnly = true;   // reliable-source profile, g2holdsource 0
bool g2HoldSourceAdmitted(uint8_t source) { return !ringOnly || source == 2; }

struct G2CmdCookie { uint64_t seq = 0; uint32_t menuGen = 0; G2HijackPage targetPage = G2_HIJACK_PAGE_APPS; uint8_t targetNetSub = 0; };
using Callback = void (*)(bool, const char*, const G2CmdCookie&, void*);
struct PendingCommand { std::string line; G2CmdCookie cookie; Callback callback; void* opaque; uint32_t epoch; };
static std::vector<PendingCommand> commands;
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie& cookie, Callback cb, void* opaque, uint32_t expectedEpoch) {
  CHECK(criticalDepth == 0, "commands are never submitted inside stateMux");
  if (!commandOkay || !ownerAuthed || expectedEpoch != liveEpoch) return false;
  auto c = cookie; c.menuGen = menuGen;
  commands.push_back({line, c, cb, opaque, liveEpoch});
  trace.push_back(std::string("cmd:") + line);
  return true;
}
struct RedrawSpec { void (*render)() = nullptr; };
enum class LensJobKind { Redraw };
enum class G2LensEnqueueWait { NoWait };
struct LensUiJob { LensJobKind kind; uint32_t submitMenuGen = 0; G2HijackPage targetPage = G2_HIJACK_PAGE_APPS; struct { RedrawSpec* redraw = nullptr; } payload; };
static std::vector<LensUiJob*> jobs;
bool g2EnqueueLensJob(LensUiJob* job, G2LensEnqueueWait) {
  if (!queueOkay) return false;
  jobs.push_back(job);
  trace.push_back("render");
  return true;
}
static void drainUi() {
  while (!jobs.empty()) {
    auto* job = jobs.front();
    jobs.erase(jobs.begin());
    if (job->submitMenuGen == menuGen && job->targetPage == page) job->payload.redraw->render();
    delete job->payload.redraw;
    delete job;
  }
}
struct G2ContainerGeom { uint32_t x = 0, y = 0, w = 0, h = 0; };
static const G2ContainerGeom G2_GEOM_LARGE{}, G2_GEOM_SPLIT_LIST{};
constexpr size_t G2_TEXT_DEFAULT_COLS = 48;
struct G2TextChildSpec { const char* containerName; const char* content; uint32_t containerId; G2ContainerGeom geom; bool eventCapture; };
enum G2TapKind { G2_TAP_PAGE_NEXT, G2_TAP_PAGE_PREV };
using G2TapFn = void (*)(G2TapKind);
static std::string displayed, displayedHead, textShown;
static std::vector<std::string> menu;
static unsigned creates = 0, updates = 0, textPages = 0;
static void (*exitCallback)() = nullptr;
static G2TapFn navCallback = nullptr;
bool g2ShowListPage(const char* const* rows, size_t count) {
  if (!uiOkay) return false;
  menu.assign(rows, rows + count); displayed = displayedHead = ""; ++creates; return true;
}
bool g2ShowMixedListText2(const char* const* rows, size_t count, const G2ContainerGeom&,
                          const G2TextChildSpec& head, const G2TextChildSpec& body) {
  CHECK(std::strlen(head.content) <= 180 && std::strlen(body.content) <= 180, "children fit one UPDATE_TEXT");
  if (!uiOkay) return false;
  menu.assign(rows, rows + count); displayedHead = head.content; displayed = body.content; ++creates;
  return true;
}
bool g2UpdateMixedTextChild(const char* name, uint32_t, const char* text) {
  CHECK(std::strlen(text) <= 180, "an UPDATE_TEXT child stays within one write");
  if (!uiOkay) return false;
  (std::string(name) == "nthead" ? displayedHead : displayed) = text; ++updates; return true;
}
bool g2ShowTextPage(const char* text, const G2ContainerGeom&, void (*exit)(), G2TapFn nav) {
  if (!uiOkay) return false;
  textShown = text; exitCallback = exit; navCallback = nav; ++textPages; menu.clear(); return true;
}

// INSERT_PRODUCTION

// ---- helpers -----------------------------------------------------------------
static const char* kId = "0123456789abcdef";
static void answer(const std::string& json, bool okay = true) {
  CHECK(commands.size() == 1, "exactly one command outstanding");
  if (commands.empty()) return;
  auto c = commands.front(); commands.clear();
  if (c.epoch == liveEpoch && ownerAuthed) c.callback(okay, json.c_str(), c.cookie, c.opaque);
}
static void tick(unsigned ms = 1000) { clockMs += ms; g2NotesTick(); drainUi(); }
static std::string front() { return commands.empty() ? std::string() : commands.front().line; }
static std::string live(bool hold, bool capture, unsigned elapsedMs, const char* phase = "recording") {
  return std::string("{\"success\":true,\"id\":\"") + kId + "\",\"phase\":\"" + phase +
         "\",\"active\":true,\"trigger\":\"" + (hold ? "hold" : "lens") + "\",\"mic\":\"g2\",\"hold\":" +
         (hold ? "true" : "false") + ",\"captureActive\":" + (capture ? "true" : "false") +
         ",\"preparing\":false,\"elapsedMs\":" + std::to_string(elapsedMs) + ",\"level\":3,\"maxMs\":60000}";
}
static std::string ended(const char* outcome, const char* reason, unsigned words = 0) {
  return std::string("{\"success\":true,\"id\":\"") + kId + "\",\"phase\":\"idle\",\"active\":false,\"outcome\":\"" +
         outcome + "\",\"reason\":\"" + reason + "\",\"path\":\"" +
         (std::strcmp(outcome, "saved") ? "" : "/stt/u2/notes/20261004T153012Z-0123456789abcdef.txt") +
         "\",\"words\":" + std::to_string(words) + ",\"durationMs\":4000}";
}
static const char* kIdle = "{\"success\":true,\"id\":\"\",\"phase\":\"idle\",\"active\":false,\"busy\":false,\"available\":true,\"reason\":\"\",\"maxMs\":60000}";
static std::string listing(int rows, bool more = false, unsigned offset = 0) {
  std::string out = "{\"success\":true,\"notes\":[";
  for (int i = 0; i < rows; ++i) {
    char row[320];
    std::snprintf(row, sizeof(row),
        "%s{\"id\":\"00000000000000%02x\",\"path\":\"/stt/u2/notes/2026100%dT1%d3012Z-00000000000000%02x.txt\","
        "\"created\":\"2026100%dT1%d3012Z\",\"when\":\"1%d:30\",\"title\":\"Note number %d\",\"words\":3,\"trigger\":\"hold\",\"mic\":\"g2\",\"durationMs\":900}",
        i ? "," : "", i + 1, 1 + i % 9, i % 10, i + 1, 1 + i % 9, i % 10, i % 10, i + 1);
    out += row;
  }
  out += std::string("],\"offset\":") + std::to_string(offset) + ",\"nextOffset\":" +
         std::to_string(offset + rows) + ",\"total\":" + std::to_string(offset + rows) +
         ",\"more\":" + (more ? "true" : "false") + ",\"capped\":false}";
  return out;
}
static void clearAll() {
  commands.clear();
  while (!jobs.empty()) { auto* j = jobs.back(); jobs.pop_back(); delete j->payload.redraw; delete j; }
  state = State{}; renderState = State{}; renderedCount = 0; renderedEpoch = 0; renderedShow = 0;
  ownerAuthed = true; liveEpoch = 10; page = G2_HIJACK_PAGE_APPS; lens.hijackActive = true;
  queueOkay = commandOkay = uiOkay = true; canStart = true; canStartWhy = nullptr; ringOnly = true;
  clockMs += 60000; ++menuGen; displayed.clear(); displayedHead.clear(); textShown.clear();
  menu.clear(); trace.clear(); creates = updates = textPages = 0; exitCallback = nullptr; navCallback = nullptr;
}
// Answers the page's idle availability polls (every 5 s on an idle list).
static void settle() { while (front() == "note status") { answer(kIdle); tick(1); } }
// Opens the page idle with `rows` saved notes listed.
static void home(int rows = 2, bool more = false) {
  g2ShowNotesMenu(); drainUi();
  tick(); CHECK(front() == "note status", "a show first asks for the wearer's live note");
  answer(kIdle); tick(1);
  CHECK(front() == "note list 0", "then lists the newest notes");
  answer(listing(rows, more)); tick(1);
}

// ---- cases -------------------------------------------------------------------
static void test_list_view() {
  clearAll(); home(8, true);
  CHECK(page == G2_HIJACK_PAGE_NOTES, "the page is current");
  CHECK(menu.size() == 9 && menu[0] == "<- Apps" && menu[1] == "● New note" && menu[8] == "More…",
        "Back, New note, six notes and More…");
  CHECK(menu[2] == "10:30 Note number 1" && menu[3] == "11:30 Note number 2",
        "rows are the reply's time label and the title");
  CHECK(displayedHead == "Ready" && displayed == "Hold to record", "idle header and body");
  g2NotesHandleTap(8); tick(1);
  CHECK(front() == "note list 6", "More… pages by six");
  answer(listing(1, false, 6)); tick(1);
  CHECK(menu.size() == 4 && menu[3] == "More…", "the last page keeps More…, which wraps to the newest");
  g2NotesHandleTap(3); tick(1);
  CHECK(front() == "note list 0", "and wraps");
  answer(listing(2)); tick(1);
  CHECK(menu.size() == 4, "two notes, no More…");
  // A note saved before the clock was set.
  clearAll(); g2ShowNotesMenu(); drainUi(); tick(); answer(kIdle); tick(1);
  answer("{\"success\":true,\"notes\":[{\"path\":\"/stt/u2/notes/boot-00000000000000aa.txt\",\"created\":\"boot\",\"when\":\"5m ago\",\"title\":\"\"},"
         "{\"path\":\"/stt/u2/notes/boot-00000000000000ab.txt\",\"created\":\"boot\",\"title\":\"Older reply\"},"
         "{\"path\":\"/stt/u2/notes/boot-00000000000000ac.txt\",\"created\":\"boot\",\"when\":\"49710d ago\",\"title\":\"" +
         std::string(48, 'T') + "\"}],\"more\":false}");
  tick(1);
  CHECK(menu.size() == 5 && menu[2] == "5m ago note", "an unsynced stamp's age this boot and an empty title");
  CHECK(menu[3] == "earlier Older reply", "a row without a time label is \"earlier\", never --:--");
  CHECK(menu[4] == "49710d ago " + std::string(48, 'T'), "the longest label and title fit a row whole");
  // An unavailable service names why in the header.
  clearAll(); g2ShowNotesMenu(); drainUi(); tick();
  answer("{\"success\":true,\"id\":\"\",\"active\":false,\"busy\":true,\"available\":false,\"reason\":\"Dictation is busy\"}");
  tick(1);
  CHECK(displayedHead == "Dictation is busy", "header names why a note cannot start");
}

static void test_toggle_note_and_saved_view() {
  clearAll(); home();
  const unsigned created = creates;
  g2NotesHandleTap(1); drainUi();
  CHECK(displayedHead == "● REC 0:00 · tap New note to save", "Recording header for a tapped note");
  tick(1); CHECK(front() == "note start", "a tap starts a lens note");
  answer(live(false, false, 200, "starting")); tick(1);
  CHECK(displayed.empty(), "no 'speak now' before capture is live");
  tick(500); CHECK(front() == std::string("note status ") + kId, "a live note is polled by id");
  answer(live(false, true, 7400)); tick(1);
  CHECK(displayedHead == "● REC 0:07 · tap New note to save" && displayed == "speak now",
        "elapsed time and 'speak now' once capture is live");
  // A listing that changes the rows lands mid-recording: no rebuild.
  {
    std::lock_guard<std::mutex> lock(stateMux);
    state.count = 3; std::snprintf(state.entries[2].label, sizeof(state.entries[2].label), "09:00 late row");
    state.dirty = true;
  }
  tick(500); answer(live(false, true, 8100)); tick(1);
  CHECK(creates == created, "never a container rebuild while recording (UPDATE_TEXT only)");
  CHECK(menu.size() == 4 && displayedHead == "● REC 0:08 · tap New note to save",
        "the old rows stay; only the text children change");
  g2NotesHandleTap(1); drainUi();
  CHECK(displayedHead == "Saving…", "a second tap saves");
  tick(1); CHECK(front() == std::string("note stop ") + kId, "stop by id");
  answer(live(false, true, 8300)); tick(1);
  CHECK(displayedHead == "Saving…", "a stop reply that still shows the note keeps Saving");
  CHECK(creates == created, "still no rebuild while the note holds the lease");
  tick(500); answer(ended("saved", "", 9)); tick(1);
  CHECK(displayedHead == "✓ Saved · 9 words", "saved header");
  CHECK(front() == "note read \"/stt/u2/notes/20261004T153012Z-0123456789abcdef.txt\" 0", "preview read");
  std::string text = "HardwareOne note v1\\nId: 0123456789abcdef\\nCreated: 20261004T153012Z\\nTrigger: lens\\n"
                     "Mic: g2\\nDuration-ms: 4000\\nWords: 9\\nTitle: Buy oat milk\\n\\n";
  std::string words;
  for (int i = 0; i < 40; ++i) words += "wörd ";
  answer(std::string("{\"success\":true,\"sttText\":\"") + text + "Buy oat milk " + words +
         "\",\"offset\":0,\"nextOffset\":400,\"total\":400,\"eof\":true}");
  tick(1);
  CHECK(displayed.rfind("Buy oat milk wörd", 0) == 0 && displayed.find("HardwareOne") == std::string::npos,
        "the preview is the text, never the file header");
  CHECK(displayed.size() <= 150 && (static_cast<unsigned char>(displayed.back()) & 0xc0) != 0x80,
        "at most 150 bytes, ending on a UTF-8 boundary");
  CHECK(front() == "note list 0", "the list refreshes once the note is over");
  answer(listing(3)); tick(1);
  CHECK(creates > created && menu.size() == 5 && menu[4] == "12:30 Note number 3",
        "rows rebuild only after the note");
  CHECK(displayedHead == "✓ Saved · 9 words", "the saved view stays up");
  tick(5000);
  CHECK(displayedHead == "Ready" && displayed == "Hold to record", "then returns to Ready after 5 s");
  // Not saved: a discard names why.
  g2NotesHandleTap(1); tick(1); answer(live(false, false, 100, "starting")); tick(1);
  g2NotesHandleTap(1); tick(1); answer(live(false, false, 300)); tick(500);
  answer(ended("discarded", "stopped before recording")); tick(1);
  CHECK(displayedHead == "Not saved: stopped before recording", "discard reason");
}

static void test_hold_on_page() {
  clearAll(); home();
  const unsigned created = creates;
  CHECK(g2NotesHandleHold(9, 2), "a ring press on the page is consumed");
  drainUi();
  CHECK(displayedHead == "● REC 0:00 · release to save", "hold header");
  tick(1); CHECK(front() == "note start hold", "the press starts a hold note");
  answer(live(true, true, 2100)); tick(1);
  CHECK(displayed == "speak now" && creates == created, "the hold never rebuilds the page");
  CHECK(g2NotesHandleHold(10, 2), "the release is taken");
  tick(1); CHECK(front() == std::string("note stop ") + kId, "the release stops (saves) the note");
  answer(live(true, true, 2900)); tick(500);
  answer(ended("saved", "", 4)); tick(1);
  answer("{\"success\":true,\"sttText\":\"x\",\"offset\":0,\"eof\":true}"); tick(1);
  answer(listing(2)); tick(1);
  // Abort (overlay, FG_EXIT, lifecycle) saves too.
  clockMs += 6000; tick(1); settle();
  CHECK(g2NotesHandleHold(9, 2), "press");
  tick(1); answer(live(true, true, 1500)); tick(1);
  CHECK(g2NotesHandleHold(0, 2), "abort");
  tick(1); CHECK(front() == std::string("note stop ") + kId, "an abort stops and saves what was said");
  answer(live(true, true, 1600)); tick(500); answer(ended("saved", "", 2)); tick(1);
  answer("{\"success\":true,\"sttText\":\"x\",\"offset\":0,\"eof\":true}"); tick(1);
  answer(listing(2)); tick(1);
  // Source policy: ring only on a profile that names the source.
  clockMs += 6000; tick(1); settle();
  CHECK(!g2NotesHandleHold(9, 1), "a temple press is declined under ring-only");
  tick(1); CHECK(commands.empty(), "and starts nothing");
  ringOnly = false;
  CHECK(g2NotesHandleHold(9, 1), "ring + temples admits it");
  tick(1); CHECK(front() == "note start hold", "start");
  answer(live(true, false, 100, "starting")); g2NotesHandleHold(10, 1); tick(1);
  answer(live(true, false, 200)); tick(500);
  answer(ended("discarded", "too short")); tick(1); answer(listing(2)); tick(1);
  CHECK(displayedHead == "Not saved: too short", "the service's slip rule shows");
  // A tapped note running: the page declines, and the dispatcher may ask the
  // global action (which refuses too).
  clockMs += 6000; tick(1); settle();
  g2NotesHandleTap(1); tick(1); answer(live(false, true, 900)); tick(1);
  CHECK(!g2NotesHandleHold(9, 2), "a press while a note runs is declined");
  CHECK(front().empty() || front() == std::string("note status ") + kId, "and starts nothing");
  const char* why = nullptr;
  CHECK(!g2NotesHoldBegin(4, &why) && why && std::string(why) == "A note is already recording",
        "the global action refuses for the same reason");
  CHECK(page == G2_HIJACK_PAGE_NOTES, "still on Notes");
}

static void test_release_before_start() {
  // Released before the start was even submitted: nothing is started.
  clearAll(); home();
  CHECK(g2NotesHandleHold(9, 2), "press");
  g2NotesHandleHold(10, 2);
  tick(1);
  CHECK(commands.empty(), "release before start: the start is cancelled before it is sent");
  CHECK(displayedHead == "Ready", "and the page is idle again");
  // Released while the start is in flight: cancelled once its reply names it.
  CHECK(g2NotesHandleHold(9, 2), "press");
  tick(1); CHECK(front() == "note start hold", "start in flight");
  g2NotesHandleHold(10, 2); drainUi();
  CHECK(displayedHead == "Saving…", "stopAfterStart shows the note ending");
  answer(live(true, false, 50, "starting")); tick(1);
  CHECK(front() == std::string("note cancel ") + kId, "stopAfterStart cancels the note it named");
  answer(live(true, false, 80)); tick(500);
  answer(ended("discarded", "cancelled")); tick(1);
  CHECK(state.rec == Rec::Done && !state.live && !state.stopAfterStart, "and the page follows it to its end");
}

static void test_stale_replies() {
  clearAll(); home();
  g2NotesHandleTap(1); tick(1);
  PendingCommand old = commands.front(); commands.clear();
  // The owner re-pairs (new epoch) before the reply lands.
  liveEpoch = 11;
  old.callback(true, live(false, true, 100).c_str(), old.cookie, old.opaque);
  CHECK(!state.live && state.id[0] == '\0', "a reply for another owner epoch is dropped");
  tick(1);
  CHECK(state.epoch == 0 && commands.empty(), "a revoked owner resets the page");
  // A reply to a superseded request is dropped.
  clearAll(); home();
  g2NotesHandleTap(1); tick(1);
  PendingCommand first = commands.front(); commands.clear();
  {
    std::lock_guard<std::mutex> lock(stateMux);
    ++state.request;   // as if that request was abandoned
  }
  first.callback(true, live(false, true, 100).c_str(), first.cookie, first.opaque);
  CHECK(!state.live, "a reply to a superseded request is dropped");
  // A note reply from before a newer start does not undo that start.
  clearAll(); home();
  clockMs += 6000; tick(1);
  CHECK(front() == "note status", "an idle poll is in flight");
  CHECK(g2NotesHandleHold(9, 2), "a hold starts meanwhile");
  answer(kIdle); tick(1);
  CHECK(front() == "note start hold" && state.rec == Rec::Recording,
        "the older idle reply cannot cancel the newer start");
  answer(live(true, true, 100)); g2NotesHandleHold(10, 2); tick(1);
  answer(live(true, true, 200)); tick(500); answer(ended("saved", "", 1)); tick(1);
}

static void test_global_hold() {
  // Refused: no navigation, no command, no drawing.
  clearAll();
  page = G2_HIJACK_PAGE_TRANSCRIPTION;
  canStart = false; canStartWhy = "Dictation is busy";
  const char* why = nullptr;
  CHECK(!g2NotesHoldBegin(5, &why), "the global action refuses when a note cannot start");
  drainUi();
  CHECK(why && std::string(why) == "Dictation is busy", "with the service's reason");
  CHECK(page == G2_HIJACK_PAGE_TRANSCRIPTION && commands.empty() && creates == 0,
        "a refusal never navigates away (e.g. from a running Transcription)");
  ownerAuthed = false; canStart = true;
  CHECK(!g2NotesHoldBegin(5, &why) && why && std::string(why) == "Pair glasses as a signed-in user",
        "no paired wearer: refused");
  CHECK(page == G2_HIJACK_PAGE_TRANSCRIPTION, "still not navigated");

  // Admitted: Notes is shown (CREATE queued first), then the hold note starts.
  clearAll();
  page = G2_HIJACK_PAGE_APPS;
  CHECK(g2NotesHoldBegin(7, &why), "admitted");
  CHECK(page == G2_HIJACK_PAGE_NOTES, "Notes is shown");
  drainUi();
  CHECK(creates == 1 && displayedHead == "● REC 0:00 · release to save" && menu.size() == 2,
        "in the Recording view, created once");
  tick(1);
  CHECK(front() == "note start hold", "then the start");
  CHECK(trace.size() >= 2 && trace.front() == "render" && trace.back() == "cmd:note start hold",
        "the page's CREATE is queued before the start brings up the mic");
  answer(live(true, true, 1200)); tick(1);
  g2NotesHoldEnd(99, 1000);
  tick(1);
  CHECK(front() == std::string("note status ") + kId || commands.empty(), "another press's end is ignored");
  CHECK(state.live && state.pressOpen, "the note keeps recording");
  if (!commands.empty()) { answer(live(true, true, 1500)); tick(1); }
  g2NotesHoldEnd(7, 2400);
  tick(500);
  CHECK(front() == std::string("note stop ") + kId, "its own release stops it");
  answer(live(true, true, 2500)); tick(500); answer(ended("saved", "", 5)); tick(1);
  CHECK(creates == 1, "no rebuild until the note is over");
  answer("{\"success\":true,\"sttText\":\"x\",\"offset\":0,\"eof\":true}"); tick(1);
  CHECK(front() == "note list 0", "the list follows");
  answer(listing(2)); tick(1);
  CHECK(creates == 2 && menu.size() == 4, "then the rows appear");

  // Shown but the lens went away before the start: it waits, and the abort
  // drops it.
  clearAll();
  CHECK(g2NotesHoldBegin(8, &why), "admitted");
  lens.hijackActive = false;
  tick(1);
  CHECK(commands.empty(), "the start waits for the page");
  g2NotesHoldAbort(8, G2HoldReason::Lifecycle);
  tick(1);
  CHECK(commands.empty() && state.epoch == 0, "the abort drops it and the page resets off-lens");
}

static void test_headless_note() {
  // The lens blanks mid-note: polling and the stop carry on headless.
  clearAll(); home();
  g2NotesHandleTap(1); tick(1); answer(live(false, true, 900)); tick(1);
  lens.hijackActive = false;
  tick(500);
  CHECK(front() == std::string("note status ") + kId, "a running note is still followed off the lens");
  answer(live(false, true, 1500)); tick(1);
  // Back while recording saves and leaves.
  lens.hijackActive = true;
  g2NotesHandleTap(0);
  CHECK(page == G2_HIJACK_PAGE_APPS, "Back leaves");
  tick(1);
  CHECK(front() == std::string("note stop ") + kId, "and saves the note on the way out");
  answer(live(false, true, 1600)); tick(500); answer(ended("saved", "", 3)); tick(1);
  CHECK(state.epoch == 0 && state.preview[0] == '\0' && commands.empty(),
        "once over, the page forgets everything off-page (no preview read)");
}

static void test_note_view_reader_delete() {
  clearAll(); home(3);
  {
    // A note was just saved: its banner shows for 5 s.
    std::lock_guard<std::mutex> lock(stateMux);
    state.rec = Rec::Done; state.doneAt = clockMs;
    std::snprintf(state.message, sizeof(state.message), "✓ Saved · 2 words");
    state.dirty = true;
  }
  g2NotesHandleTap(2); drainUi();
  CHECK(menu.size() == 3 && menu[0] == "<- Notes" && menu[1] == "Read" && menu[2] == "Delete", "note view rows");
  CHECK(displayedHead == "10:30 Note number 1", "the note's label heads it");
  tick(1);
  CHECK(front() == "note read \"/stt/u2/notes/20261001T103012Z-0000000000000001.txt\" 0", "preview read of that note");
  answer("{\"success\":true,\"sttText\":\"HardwareOne note v1\\nTitle: t\\n\\nfirst words\",\"offset\":0,\"nextOffset\":40,\"eof\":true}");
  tick(1);
  CHECK(displayed == "first words", "preview body");
  tick(6000);
  CHECK(displayed == "first words" && displayedHead == "10:30 Note number 1",
        "opening a note dismissed the Saved banner; its timeout leaves the preview");
  // Delete needs a second tap within 4 s.
  g2NotesHandleTap(2); drainUi();
  CHECK(displayedHead.find("Tap Delete again") != std::string::npos, "first tap asks to confirm");
  tick(4100);
  CHECK(displayedHead == "10:30 Note number 1" && commands.empty(), "the confirmation lapses after 4 s");
  g2NotesHandleTap(2); clockMs += 3000; g2NotesHandleTap(2); tick(1);
  CHECK(front() == "note delete \"/stt/u2/notes/20261001T103012Z-0000000000000001.txt\"", "second tap deletes");
  answer("{\"success\":true,\"deleted\":\"x\"}"); tick(1);
  CHECK(state.view == View::List && displayedHead == "Note deleted", "back to the list");
  CHECK(front() == "note list 0", "which refreshes");
  answer(listing(2)); tick(1);
  // Reader: windows, paging and exit.
  g2NotesHandleTap(3); tick(1); answer("{\"success\":true,\"sttText\":\"p\",\"offset\":0,\"eof\":true}"); tick(1);
  g2NotesHandleTap(1); tick(1);
  CHECK(front() == "note read \"/stt/u2/notes/20261002T113012Z-0000000000000002.txt\" 0", "reader window 0");
  answer("{\"success\":true,\"sttText\":\"HardwareOne note v1\\n\\nwindow one\",\"offset\":0,\"nextOffset\":512,\"eof\":false}");
  tick(1);
  CHECK(textShown.find("window one") != std::string::npos && textShown.find("scroll=more") != std::string::npos &&
        textShown.find("HardwareOne") == std::string::npos, "first window, header skipped, more to come");
  const unsigned pages = textPages;
  tick(1);
  CHECK(textPages == pages, "an unchanged reader page is not re-sent");
  navCallback(G2_TAP_PAGE_NEXT); tick(1);
  CHECK(front() == "note read \"/stt/u2/notes/20261002T113012Z-0000000000000002.txt\" 512", "next window");
  answer("{\"success\":true,\"sttText\":\"window two\",\"offset\":512,\"nextOffset\":600,\"eof\":true}"); tick(1);
  CHECK(textShown.find("window two") != std::string::npos, "second window");
  navCallback(G2_TAP_PAGE_PREV); tick(1);
  CHECK(front() == "note read \"/stt/u2/notes/20261002T113012Z-0000000000000002.txt\" 0", "previous window");
  // A late window cannot land after leaving the reader.
  exitCallback();
  answer("{\"success\":true,\"sttText\":\"late secret\",\"offset\":0,\"eof\":true}"); tick(1);
  CHECK(state.view == View::Note && std::string(state.text).find("late secret") == std::string::npos,
        "late reader replies are dropped");
  // Busy taps: while a note records, note rows are ignored.
  g2NotesHandleTap(0); drainUi();
  g2NotesHandleTap(1); tick(1); answer(live(false, true, 500)); tick(1);
  g2NotesHandleTap(2); drainUi();
  CHECK(state.view == View::List, "a note row does nothing while recording");
}

static void test_keep_warm() {
  // While the page is shown the note backend is kept ready; off the page it is not asked.
  clearAll(); warmCalls = 0;
  home();
  CHECK(warmCalls > 0, "an open Notes page asks for the model");
  const int open = warmCalls;
  tick(1); settle();
  CHECK(warmCalls > open, "every tick while it stays open");
  g2NotesHandleTap(0); drainUi(); tick(1);
  const int left = warmCalls;
  tick(1); tick(1);
  CHECK(page == G2_HIJACK_PAGE_APPS && warmCalls == left, "never once the page is left");
  // No paired wearer: nothing.
  clearAll(); warmCalls = 0; ownerAuthed = false;
  g2ShowNotesMenu(); drainUi(); tick(1);
  CHECK(warmCalls == 0, "no warm-up without the paired wearer's session");
}

static void test_tap_not_swallowed_and_privacy() {
  // A tap on New note while a listing is in flight still starts the note.
  clearAll();
  g2ShowNotesMenu(); drainUi(); tick(); answer(kIdle); tick(1);
  CHECK(front() == "note list 0", "listing in flight");
  g2NotesHandleTap(1);
  answer(listing(2)); tick(1);
  CHECK(front() == "note start", "the tap was queued, not swallowed");
  answer(live(false, true, 100)); g2NotesHandleTap(1); tick(1);
  answer(live(false, true, 200)); tick(500); answer(ended("saved", "", 2)); tick(1);
  answer("{\"success\":true,\"sttText\":\"HardwareOne note v1\\n\\nprivate words\",\"offset\":0,\"eof\":true}"); tick(1);
  CHECK(displayed == "private words", "preview shown on the lens");
  // Owner revocation clears the page and never shows the text again.
  ownerAuthed = false; tick(1);
  CHECK(state.epoch == 0 && state.preview[0] == '\0', "revocation wipes the model");
  CHECK(displayed.find("private") == std::string::npos, "and the lens");
  // Leaving the page idle forgets the list and any text.
  clearAll(); home(3);
  g2NotesHandleTap(2); tick(1);
  answer("{\"success\":true,\"sttText\":\"HardwareOne note v1\\n\\nsecret\",\"offset\":0,\"eof\":true}"); tick(1);
  g2NotesHandleTap(0); drainUi(); g2NotesHandleTap(0);
  tick(1);
  CHECK(page == G2_HIJACK_PAGE_APPS && state.epoch == 0 && state.preview[0] == '\0' && state.count == 0,
        "off the page with nothing running, the page model is wiped");
  // A failed submission retries the same request.
  clearAll(); home();
  commandOkay = false; g2NotesHandleTap(1); tick(1);
  CHECK(commands.empty() && state.queued == Op::Start, "queue refusal keeps the start queued");
  commandOkay = true; tick(1);
  CHECK(front() == "note start", "and retries it");
  answer("{\"success\":false,\"error\":\"Glasses mic unavailable\"}"); tick(1);
  CHECK(displayedHead == "Glasses mic unavailable" && state.rec == Rec::Idle, "a refused start shows why");
  // An ambiguous start reply adopts the wearer's live note instead of
  // starting a second one.
  g2NotesHandleTap(1); tick(1); answer("garbled", false); tick(1);
  CHECK(front() == "note status", "recovery asks for the live note");
  answer(live(false, true, 300)); tick(1);
  CHECK(state.live && std::string(state.id) == kId, "and adopts it");
}

int main() {
  setenv("TZ", "UTC0", 1);
  tzset();
  test_list_view();
  test_toggle_note_and_saved_view();
  test_hold_on_page();
  test_release_before_start();
  test_stale_replies();
  test_global_hold();
  test_headless_note();
  test_note_view_reader_delete();
  test_tap_not_swallowed_and_privacy();
  test_keep_warm();
  clearAll();
  CHECK(criticalDepth == 0, "stateMux always released");
  if (failures) {
    std::fprintf(stderr, "%d G2 Notes page check(s) failed\n", failures);
    return 1;
  }
  std::puts("G2 Notes page harness passed");
  return 0;
}
