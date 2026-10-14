// Behavioural harness for the production G2 long-press ("hold") section of
// G2_Glasses.cpp: ingest (pairing, gates, watchdog activity, job queueing),
// the g2_tap_disp delivery (routes, page side table, global action, events),
// implicit aborts, expiry and `g2hold sim`. test_g2_hold_dispatch.py splices
// the section verbatim at INSERT_PRODUCTION, and the exit tail of
// handleDevEvent's SysEvent branch at INSERT_SYS_EXIT_TAIL; everything else is
// a fake.
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "G2_HoldGesture.h"   // -I<component> (test_g2_hold_dispatch.py)

static int failures = 0;
#define CHECK(condition, message)                                            \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

// ---- platform fakes ---------------------------------------------------------
#define EXT_RAM_BSS_ATTR
struct portMUX_TYPE { int unused; };
#define portMUX_INITIALIZER_UNLOCKED {0}
static int gCriticalDepth = 0;
static int gCriticalMax = 0;
static void portENTER_CRITICAL(portMUX_TYPE*) {
  ++gCriticalDepth;
  if (gCriticalDepth > gCriticalMax) gCriticalMax = gCriticalDepth;
}
static void portEXIT_CRITICAL(portMUX_TYPE*) {
  assert(gCriticalDepth > 0);
  --gCriticalDepth;
}
static uint32_t gNowMs = 100000;
static uint32_t millis() { return gNowMs; }

static int gLogLines = 0;
static void fakeLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void fakeLog(const char* fmt, ...) {
  CHECK(gCriticalDepth == 0, "no logging inside a spinlock");
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  ++gLogLines;
}
#define DEBUG_G2F(fmt, ...) fakeLog(fmt, ##__VA_ARGS__)
#define DEBUG_G2_EVENTSF(fmt, ...) fakeLog(fmt, ##__VA_ARGS__)
static const uint32_t DEBUG_G2 = 1u;
static const uint32_t DEBUG_G2_EVENTS = 2u;
static bool isDebugOutputEnabled(uint32_t) { return true; }

struct String {
  std::string s;
  String(const char* v) : s(v) {}
  bool operator==(const char* v) const { return s == v; }
  const char* c_str() const { return s.c_str(); }
};

// ---- G2 fakes ---------------------------------------------------------------
enum G2HijackPage : uint8_t {
  G2_HIJACK_PAGE_MAIN = 0,
  G2_HIJACK_PAGE_APPS = 11,
  G2_HIJACK_PAGE_TRANSCRIPTION = 23,
  G2_HIJACK_PAGE_NOTES = 24,
  G2_HIJACK_PAGE_LLM_READER = 25,
};
struct G2PageModule { const char* name; };
typedef bool (*G2PageHoldFn)(uint8_t phase, uint8_t source);
bool g2RegisterPageHold(G2HijackPage page, G2PageHoldFn fn);
bool g2HoldSourceAdmitted(uint8_t source);

struct G2Temple {
  char side;
  bool current;
  char firmwareVersion[32];
};
static G2Temple gL = {'L', true, "2.3.2.14"};
static G2Temple gR = {'R', true, "2.3.2.14"};
static portMUX_TYPE gRxPacketMux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t gWidgetLifecycleEpoch = 5;
static uint32_t gPresentation = 40;
static uint32_t g2PresentationEpochCurrent() { return gPresentation; }
static bool g2LifecyclePacketCurrent(G2Temple& t, uint32_t generation,
                                     uint32_t lifecycleEpoch) {
  return t.current && generation == 1 && lifecycleEpoch == gWidgetLifecycleEpoch;
}
struct G2LensState { bool hijackActive; };
static bool gLensHijack = true;
static G2LensState g2LensGetState() { return G2LensState{gLensHijack}; }
static bool gOverlay = false;   // firmwareOverlayActive(): overlay or grace
static bool firmwareOverlayActive() { return gOverlay; }
static volatile bool gFirmwareOverlayUp = false;   // the raw flag (tick hint)
static bool gKeyboard = false;
static bool kbdPadSysDoubleOwned() { return gKeyboard; }
static volatile bool gCamStreamActive = false;
static volatile bool gImgProbeHoldActive = false;
static bool gSwapping = false;
static bool pageSwapInFlight() { return gSwapping; }
static bool gFsmHijack = true;
static bool g2FsmHijackActive() { return gFsmHijack; }
// What g2HoldPageBusy reads: a session job (Maps, Health, the LLM viewer, ...)
// or an image probe owns the lens; live list/text pages run as session jobs.
enum class HijackState : uint8_t { Idle = 0, Hijacked, PageSwapping, ImageProbing };
static HijackState gFsmState = HijackState::Hijacked;
static HijackState hijackFsmState() { return gFsmState; }
static bool gSessionBusy = false;
static bool g2SessionWorkerBusy() { return gSessionBusy; }
static bool gLivePage = false;
static bool gLiveText = false;
static bool livePageIsActive() { return gLivePage; }
static bool liveTextIsActive() { return gLiveText; }
static uint32_t gHijackStartedMs = 0;
static G2HijackPage gPage = G2_HIJACK_PAGE_MAIN;
static G2HijackPage g2GetHijackPage() { return gPage; }
static const G2PageModule kTranscription = {"transcription"};
static const G2PageModule* g2FindPageByHijackPage(G2HijackPage page) {
  return page == G2_HIJACK_PAGE_TRANSCRIPTION ? &kTranscription : nullptr;
}

struct FakeSettings {
  uint8_t g2HoldAction = 1;
  uint8_t g2HoldSource = 0;
  bool g2HoldUnprofiled = false;
};
static FakeSettings gSettings;

enum : uint8_t { SYSEVT_G2_LONG_PRESS = 200, SYSEVT_G2_LONG_RELEASE = 201 };
struct Event { uint8_t kind; std::string subject, detail; uint32_t atMs; };
static std::vector<Event> gEvents;
static void systemEventPost(uint8_t kind, const char* subject,
                            const char* detail) {
  CHECK(gCriticalDepth == 0, "events are never posted inside a spinlock");
  gEvents.push_back(Event{kind, subject ? subject : "", detail ? detail : "",
                          gNowMs});
}

struct QueuedJob { G2HoldJob job; uint32_t epoch; };
static std::vector<QueuedJob> gQueue;
static bool gQueueFull = false;
static bool tapDispatcherEnqueueHold(const G2HoldJob& job,
                                     uint32_t presentationEpoch) {
  CHECK(gCriticalDepth == 0, "jobs are never queued inside a spinlock");
  if (gQueueFull) return false;
  gQueue.push_back(QueuedJob{job, presentationEpoch});
  return true;
}

// The QuickNote side of the global hold action (G2_Page_Notes.cpp) and the
// note length the press expiry follows (System_QuickNote.cpp).
static uint32_t gNoteMaxMs = 60000;
static uint32_t quickNoteMaxMs() { return gNoteMaxMs; }
struct NotesCall { char kind; uint32_t pressId; uint32_t arg; };
struct NotesLog { std::vector<NotesCall> calls; bool accept = true; const char* why = "Dictation is busy"; };
static NotesLog gNotes;
static bool g2NotesHoldBegin(uint32_t pressId, const char** why) {
  CHECK(gCriticalDepth == 0, "the global action never runs inside a spinlock");
  gNotes.calls.push_back(NotesCall{'B', pressId, 0});
  if (!gNotes.accept) *why = gNotes.why;
  return gNotes.accept;
}
static void g2NotesHoldEnd(uint32_t pressId, uint32_t heldMs) {
  CHECK(gCriticalDepth == 0, "the global action never runs inside a spinlock");
  gNotes.calls.push_back(NotesCall{'E', pressId, heldMs});
}
static void g2NotesHoldAbort(uint32_t pressId, G2HoldReason reason) {
  CHECK(gCriticalDepth == 0, "the global action never runs inside a spinlock");
  gNotes.calls.push_back(NotesCall{'A', pressId, (uint32_t)reason});
}

// INSERT_PRODUCTION

// ---- SysEvent exit tail -----------------------------------------------------
// The end of handleDevEvent's SysEvent branch, spliced verbatim: SYSTEM_EXIT's
// split between HW1's own Shutdown echo and a terminal exit, then the implicit
// hold releases. The lifecycle side is faked here.
static bool gEchoLease = false;   // HW1 just sent a replacement Shutdown
static int gOverlayResets = 0;
static int gTerminals = 0;
static bool g2ExpectedEchoForGeneration(char, uint32_t generation) {
  return gEchoLease && generation == 1;
}
static void g2OverlayResetForPacket(G2Temple&, uint32_t, uint32_t) {
  ++gOverlayResets;
}
// As production: the claim advances the widget lifecycle, then closes any
// open hold.
static bool g2TerminalInvalidate(const char*, char side, uint32_t, uint32_t) {
  ++gTerminals;
  gWidgetLifecycleEpoch = gWidgetLifecycleEpoch + 1;
  (void)g2HoldAbort(G2HoldReason::Terminal, side);
  return true;
}
static void sysEventTail(G2Temple& t, uint32_t etype, uint32_t reason) {
  const uint32_t rxGeneration = 1;
  const uint32_t rxLifecycleEpoch = gWidgetLifecycleEpoch;
  // INSERT_SYS_EXIT_TAIL
}

// ---- helpers ----------------------------------------------------------------
static void deliverAll() {
  // The real worker is FIFO; jobs queued while delivering go to the back.
  while (!gQueue.empty()) {
    const QueuedJob q = gQueue.front();
    gQueue.erase(gQueue.begin());
    g2HoldDeliver(q.job, q.epoch);
  }
}

static void rx(char side, uint32_t etype, uint32_t src,
               G2HoldVia via = G2HoldVia::Sys, uint32_t generation = 1) {
  G2Temple& t = side == 'L' ? gL : gR;
  g2HoldIngest(t, etype, src, via, generation, gWidgetLifecycleEpoch,
               gPresentation);
}

static void reset() {
  gQueue.clear();
  gEvents.clear();
  gQueueFull = false;
  gOverlay = gKeyboard = gSwapping = false;
  gFirmwareOverlayUp = false;
  gCamStreamActive = gImgProbeHoldActive = false;
  gLensHijack = gFsmHijack = true;
  gFsmState = HijackState::Hijacked;
  gSessionBusy = gLivePage = gLiveText = false;
  gEchoLease = false;
  gOverlayResets = gTerminals = 0;
  gPage = G2_HIJACK_PAGE_MAIN;
  gSettings = FakeSettings();
  std::strcpy(gL.firmwareVersion, "2.3.2.14");
  std::strcpy(gR.firmwareVersion, "2.3.2.14");
  gL.current = gR.current = true;
  gNowMs += 60000;   // past any event rate window
  // Close anything a previous case left open, then drain it.
  (void)g2HoldAbort(G2HoldReason::Sim, '-');
  deliverAll();
  gEvents.clear();
  gNowMs += 60000;   // and past that cleanup's own events
  for (G2HijackPage p : {G2_HIJACK_PAGE_MAIN, G2_HIJACK_PAGE_APPS,
                         G2_HIJACK_PAGE_TRANSCRIPTION, G2_HIJACK_PAGE_NOTES}) {
    (void)g2RegisterPageHold(p, nullptr);
  }
  portENTER_CRITICAL(&gHoldMux);
  gHoldLogHead = gHoldLogCount = 0;
  gHoldCounters = {};
  portEXIT_CRITICAL(&gHoldMux);
  gNotes = NotesLog();
  gNoteMaxMs = 60000;
}

static G2HoldRecord newestPressRecord() {
  for (size_t i = 1; i <= gHoldLogCount; i++) {
    const G2HoldRecord& r =
        gHoldLog[(gHoldLogHead + kHoldLogCap - i) % kHoldLogCap];
    if (r.phase == (uint8_t)G2HoldPhase::Press &&
        (r.verdict == (uint8_t)G2HoldVerdict::Press ||
         r.verdict == (uint8_t)G2HoldVerdict::PressAfterLostRelease)) {
      return r;
    }
  }
  return G2HoldRecord{};
}

static std::string outcomeOf(const G2HoldRecord& r) {
  char out[56];
  g2HoldRecordOutcome(r, out, sizeof(out));
  return out;
}

static bool lastEventIs(uint8_t kind, const char* subject, const char* detail) {
  if (gEvents.empty()) return false;
  const Event& e = gEvents.back();
  return e.kind == kind && e.subject == subject && e.detail == detail;
}

struct PageLog { std::vector<std::pair<uint8_t, uint8_t>> calls; bool consume = true; };
static PageLog gPageLog;
static bool fakePageHold(uint8_t phase, uint8_t source) {
  CHECK(gCriticalDepth == 0, "page handlers never run inside a spinlock");
  gPageLog.calls.push_back({phase, source});
  return phase != 9 || gPageLog.consume;
}

// ---- cases ------------------------------------------------------------------
static void test_global_quicknote() {
  reset();
  rx('R', 9, 2);
  CHECK(gQueue.size() == 1 && gQueue[0].job.phase == 9 &&
            gQueue[0].job.source == 2 && gQueue[0].epoch == gPresentation &&
            gQueue[0].job.reason == 0,
        "a ring press on a profiled page queues one press job");
  CHECK(outcomeOf(newestPressRecord()) == "Press→pending",
        "before delivery the press row is pending");
  const uint32_t id = gQueue[0].job.pressId;
  deliverAll();
  CHECK(gNotes.calls.size() == 1 && gNotes.calls[0].kind == 'B' &&
            gNotes.calls[0].pressId == id,
        "the global action begins a note for that press");
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "quicknote"),
        "the event names the QuickNote consumer");
  CHECK(outcomeOf(newestPressRecord()) == "Press→quicknote",
        "the press row records the global consumer");
  gNowMs += 2400;
  rx('R', 10, 2);
  CHECK(gQueue.size() == 1 && gQueue[0].job.phase == 10 &&
            gQueue[0].job.heldMs == 2400 && gQueue[0].epoch == 0,
        "the release queues with its held time and no epoch");
  gPage = G2_HIJACK_PAGE_APPS;   // the action shows Notes: the page changed
  deliverAll();
  CHECK(gNotes.calls.size() == 2 && gNotes.calls[1].kind == 'E' &&
            gNotes.calls[1].pressId == id && gNotes.calls[1].arg == 2400,
        "its release ends that note, whatever page is current now");
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "2400ms"),
        "the release posts its held time");
  CHECK(gEvents.size() == 2, "one event per edge");

  // Refused (busy, Even AI, storage, no wearer): ignored, never consumed.
  reset();
  gNotes.accept = false;
  rx('R', 9, 2);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "refused:Dictation is busy"),
        "a refusal is posted with the action's reason");
  CHECK(outcomeOf(newestPressRecord()) == "Press→ignored:refused",
        "the press row shows the refusal");
  gNowMs += 900;
  rx('R', 10, 2);
  deliverAll();
  CHECK(gNotes.calls.size() == 1, "a refused press's release reaches nobody");
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "900ms"), "but is posted");

  // An abort of a consumed press reaches the note.
  reset();
  rx('R', 9, 2);
  deliverAll();
  gNowMs += 700;
  g2HoldAbortFromPacket(gR, 1, gWidgetLifecycleEpoch, G2HoldReason::FgExit);
  deliverAll();
  CHECK(gNotes.calls.size() == 2 && gNotes.calls[1].kind == 'A' &&
            gNotes.calls[1].arg == (uint32_t)G2HoldReason::FgExit,
        "FG_EXIT aborts the note's press (the note saves what was said)");
}

static void test_global_page_busy() {
  // Maps / Health / the LLM viewer: a session job draws the page. The global
  // action refuses without navigating (that worker would later tear down the
  // Notes page and pull the user away); the hold still counts as activity.
  reset();
  gSessionBusy = true;
  gHijackStartedMs = 1;
  rx('R', 9, 2);
  CHECK(gHijackStartedMs == gNowMs, "a hold on a session page refreshes the watchdog");
  deliverAll();
  CHECK(gNotes.calls.empty(), "the note is never begun: no navigation to Notes");
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "refused:page-busy"),
        "a session page refuses the global action as page-busy");
  CHECK(outcomeOf(newestPressRecord()) == "Press→ignored:refused",
        "the press row shows the refusal");
  gNowMs += 1500;
  rx('R', 10, 2);
  deliverAll();
  CHECK(gNotes.calls.empty() && lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "1500ms"),
        "its release is absorbed but still posted");

  // An image probe holds the lens (the FSM is ImageProbing), on any task.
  reset();
  gFsmState = HijackState::ImageProbing;
  rx('R', 9, 2);
  deliverAll();
  CHECK(gNotes.calls.empty() &&
            lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "refused:page-busy"),
        "an image probe refuses the global action as page-busy");
  gNowMs += 800;
  rx('R', 10, 2);
  deliverAll();

  // A live list or text page runs as a session job too, but the page swap to
  // Notes stops its worker: the global action may leave it.
  for (bool* live : {&gLivePage, &gLiveText}) {
    reset();
    gSessionBusy = true;
    *live = true;
    rx('R', 9, 2);
    deliverAll();
    CHECK(gNotes.calls.size() == 1 && gNotes.calls[0].kind == 'B' &&
              lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "quicknote"),
          "a live page does not block the global action");
    gNowMs += 1000;
    rx('R', 10, 2);
    deliverAll();
    CHECK(gNotes.calls.size() == 2 && gNotes.calls[1].kind == 'E',
          "and its release ends the note");
  }
}

static void test_action_off_events() {
  reset();
  gSettings.g2HoldAction = 0;
  rx('R', 9, 2);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "ignored:action-off"),
        "g2holdaction 0: the press is posted as ignored:action-off");
  gNowMs += 1500;
  rx('R', 10, 2);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "1500ms"),
        "and its release is absorbed but still posted");
}

static void test_source_policy() {
  reset();
  rx('R', 9, 1);   // right temple on a source-reliable profile, ring only
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "right", "ignored:source"),
        "ring-only drops a temple hold with reason source");
  CHECK(outcomeOf(newestPressRecord()) == "Press→ignored:source",
        "the row records the source drop");
  gNowMs += 900;
  rx('R', 10, 1);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "right", "900ms"),
        "the dropped hold's release is absorbed");

  reset();
  gSettings.g2HoldSource = 1;
  rx('R', 9, 3);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "left", "quicknote"),
        "ring + temples lets a left-temple hold reach the global action");

  reset();
  std::strcpy(gL.firmwareVersion, "2.2.9.22");
  std::strcpy(gR.firmwareVersion, "2.2.9.22");
  rx('R', 9, 0);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "unknown", "quicknote"),
        "2.2.9.22 (src 0, unreliable) accepts the unknown source under ring-only");
  CHECK(g2HoldSourceAdmitted(0) && g2HoldSourceAdmitted(1),
        "pages see the same policy: everything on an unreliable profile");
  std::strcpy(gL.firmwareVersion, "2.3.2.14");
  std::strcpy(gR.firmwareVersion, "2.3.2.14");
  CHECK(g2HoldSourceAdmitted(2) && !g2HoldSourceAdmitted(1) &&
            !g2HoldSourceAdmitted(0),
        "pages see ring-only on a reliable profile");
  gSettings.g2HoldSource = 1;
  CHECK(g2HoldSourceAdmitted(1) && g2HoldSourceAdmitted(3),
        "and ring + temples when set");
}

static void test_source_upgrade_from_later_copy() {
  reset();
  rx('R', 9, 0, G2HoldVia::List);   // List copy carries no source
  rx('R', 9, 2, G2HoldVia::Sys);    // its Sys copy names the ring
  CHECK(gQueue.size() == 1, "the duplicate copy queues nothing");
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "quicknote"),
        "delivery uses the source a later copy upgraded");
}

static void test_unprofiled() {
  reset();
  std::strcpy(gL.firmwareVersion, "2.3.3.1");
  std::strcpy(gR.firmwareVersion, "2.3.3.1");
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
  gPageLog = PageLog();
  rx('R', 9, 2);
  CHECK(gQueue.size() == 1 &&
            gQueue[0].job.reason == (uint8_t)G2HoldReason::Unprofiled,
        "an unprofiled pair is dropped at ingest");
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "ignored:unprofiled"),
        "and still posted, with detail unprofiled");
  CHECK(gPageLog.calls.empty(), "nothing is dispatched on unprofiled firmware");
  gNowMs += 700;
  rx('R', 10, 2);
  deliverAll();
  CHECK(gPageLog.calls.empty(), "not even the release");

  reset();
  std::strcpy(gL.firmwareVersion, "2.2.9.22");
  std::strcpy(gR.firmwareVersion, "2.3.2.14");
  rx('R', 9, 2);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "ignored:unprofiled"),
        "a mixed pair is unprofiled");

  reset();
  std::strcpy(gL.firmwareVersion, "2.3.3.1");
  std::strcpy(gR.firmwareVersion, "2.3.3.1");
  gSettings.g2HoldUnprofiled = true;
  rx('R', 9, 0);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "unknown", "quicknote"),
        "the admin override routes it, with no source restriction");
}

static void test_page_handler() {
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_TRANSCRIPTION, fakePageHold);
  gPage = G2_HIJACK_PAGE_TRANSCRIPTION;
  rx('R', 9, 1);   // a temple: pages get every source and apply the policy
  deliverAll();
  CHECK(gPageLog.calls.size() == 1 && gPageLog.calls[0].first == 9 &&
            gPageLog.calls[0].second == 1,
        "the current page's handler gets the press and its source");
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "right", "transcription"),
        "the event names the consuming page");
  CHECK(outcomeOf(newestPressRecord()) == "Press→page:23",
        "the row records the page consumer");
  gPage = G2_HIJACK_PAGE_MAIN;   // pages changed under the finger…
  gPage = G2_HIJACK_PAGE_TRANSCRIPTION;   // …and back: still current
  gNowMs += 3000;
  rx('R', 10, 1);
  deliverAll();
  CHECK(gPageLog.calls.size() == 2 && gPageLog.calls[1].first == 10,
        "its release reaches the same page");

  // The page declines: the global action may take the press.
  gPageLog = PageLog();
  gPageLog.consume = false;
  rx('R', 9, 2);
  deliverAll();
  CHECK(gPageLog.calls.size() == 1 &&
            lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "quicknote"),
        "a declining page falls through to the global action");
  gNowMs += 1000;
  rx('R', 10, 2);
  deliverAll();
  CHECK(gPageLog.calls.size() == 1,
        "a page that declined never sees the release");
  CHECK(gNotes.calls.size() == 2 && gNotes.calls[1].kind == 'E',
        "the global consumer that took it does");

  // A page that applies the source policy and declines a temple press: the
  // drop is "source", even with the global action off (live source policy).
  gPageLog = PageLog();
  gPageLog.consume = false;
  gNotes = NotesLog();
  gSettings.g2HoldAction = 0;
  rx('R', 9, 1);
  deliverAll();
  CHECK(gPageLog.calls.size() == 1 && gNotes.calls.empty() &&
            lastEventIs(SYSEVT_G2_LONG_PRESS, "right", "ignored:source"),
        "a page's source refusal is recorded as source");
  CHECK(outcomeOf(newestPressRecord()) == "Press→ignored:source", "on its row too");
  gNowMs += 1000;
  rx('R', 10, 1);
  deliverAll();
  CHECK(gPageLog.calls.size() == 1, "its release is absorbed");
  gSettings.g2HoldAction = 1;

  // A page swap away before the release: the page gets an abort instead.
  gPageLog = PageLog();
  rx('R', 9, 2);
  deliverAll();
  gPage = G2_HIJACK_PAGE_APPS;
  gNowMs += 1200;
  rx('R', 10, 2);
  deliverAll();
  CHECK(gPageLog.calls.size() == 2 && gPageLog.calls[1].first == 0,
        "a release after the page is gone is delivered as an abort");
  CHECK(gHoldCounters.pageGone == 1, "and counted as page-gone");
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "1200ms"),
        "the release is still posted");

  // A page without a handler never sees a hold.
  gPageLog = PageLog();
  gPage = G2_HIJACK_PAGE_APPS;
  rx('R', 9, 2);
  deliverAll();
  CHECK(gPageLog.calls.empty(), "only the current page's handler is asked");
}

static void test_gates() {
  struct Case { const char* reason; bool* flag; volatile bool* vflag; bool atIngest; };
  bool dummy = false;
  const Case cases[] = {
    {"overlay", &gOverlay, nullptr, true},
    {"keyboard", &gKeyboard, nullptr, false},
    {"swap-in-flight", &gSwapping, nullptr, false},
    {"camera", nullptr, &gCamStreamActive, false},
    {"image-probe", nullptr, &gImgProbeHoldActive, false},
    {"no-page", &dummy, nullptr, true},
  };
  for (const Case& c : cases) {
    reset();
    gPageLog = PageLog();
    (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
    const bool noPage = std::strcmp(c.reason, "no-page") == 0;
    if (noPage) gLensHijack = false;
    else if (c.flag) *c.flag = true;
    else *c.vflag = true;
    rx('R', 9, 2);
    CHECK((gQueue.size() == 1 && gQueue[0].job.reason != 0) == c.atIngest,
          "cheap gates drop at ingest, page-level gates at delivery");
    deliverAll();
    const std::string want = std::string("ignored:") + c.reason;
    CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", want.c_str()),
          "every gate names its drop in the event");
    CHECK(gPageLog.calls.empty(), "a gated press never reaches a page");
    gOverlay = gKeyboard = gSwapping = false;
    gCamStreamActive = gImgProbeHoldActive = false;
    gLensHijack = true;
    gNowMs += 800;
    rx('R', 10, 2);
    deliverAll();
    CHECK(gPageLog.calls.empty(), "nor does its release");
  }

  // The presentation moved on between ingest and delivery.
  reset();
  rx('R', 9, 2);
  gPresentation++;
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "ignored:stale"),
        "a press for a superseded presentation is stale at delivery");
  // A packet stamped with an older presentation is stale at ingest.
  reset();
  G2Temple& t = gR;
  g2HoldIngest(t, 9, 2, G2HoldVia::Sys, 1, gWidgetLifecycleEpoch,
               gPresentation - 1);
  CHECK(gQueue.size() == 1 &&
            gQueue[0].job.reason == (uint8_t)G2HoldReason::Stale,
        "an old presentation is dropped at ingest");
}

static void test_watchdog_activity() {
  reset();
  gSettings.g2HoldAction = 0;
  gHijackStartedMs = 1;
  rx('R', 9, 2);
  CHECK(gHijackStartedMs == gNowMs,
        "an ignored hold on a live page still refreshes the watchdog");
  gHijackStartedMs = 1;
  gNowMs += 2000;
  rx('R', 10, 2);
  CHECK(gHijackStartedMs == gNowMs, "so does its release");
  deliverAll();

  reset();
  std::strcpy(gL.firmwareVersion, "2.3.3.1");
  std::strcpy(gR.firmwareVersion, "2.3.3.1");
  gHijackStartedMs = 1;
  rx('R', 9, 2);
  CHECK(gHijackStartedMs == gNowMs, "even on unprofiled firmware");
  deliverAll();

  reset();
  gLensHijack = false;
  gHijackStartedMs = 1;
  rx('R', 9, 2);
  CHECK(gHijackStartedMs == 1, "no HW1 page: no refresh");
  deliverAll();

  reset();
  gHijackStartedMs = 1;
  G2Temple& t = gR;
  g2HoldIngest(t, 9, 2, G2HoldVia::Sys, 1, gWidgetLifecycleEpoch,
               gPresentation - 1);
  CHECK(gHijackStartedMs == 1, "an old presentation: no refresh");
  deliverAll();

  reset();
  gHijackStartedMs = 1;
  rx('R', 9, 2, G2HoldVia::Sys, /*generation=*/2);
  CHECK(gHijackStartedMs == 1 && gQueue.empty(),
        "a stale lifecycle packet neither refreshes nor queues");

  reset();
  gFsmHijack = false;
  gHijackStartedMs = 1;
  rx('R', 9, 2);
  CHECK(gHijackStartedMs == 1, "the watchdog is not armed outside a hijack");
  deliverAll();
}

static void test_aborts_reach_the_consumer() {
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
  rx('R', 9, 2);
  deliverAll();
  gNowMs += 500;
  g2HoldAbortFromPacket(gR, 1, gWidgetLifecycleEpoch, G2HoldReason::FgExit);
  CHECK(gQueue.size() == 1 && gQueue[0].job.phase == 0 &&
            gQueue[0].job.reason == (uint8_t)G2HoldReason::FgExit &&
            gQueue[0].job.heldMs == 500,
        "FG_EXIT queues an abort for the open press");
  deliverAll();
  CHECK(gPageLog.calls.size() == 2 && gPageLog.calls[1].first == 0,
        "the consuming page gets phase 0");
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "500ms aborted:fg-exit"),
        "the abort is posted as the end of the hold");
  rx('R', 10, 2);
  deliverAll();
  CHECK(gPageLog.calls.size() == 2,
        "a release after the abort is an orphan and reaches nobody");

  // A firmware overlay rising over the page (FG_ENTER; R11): the consumer
  // gets its abort while the overlay is up, not when it is dismissed.
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
  rx('R', 9, 2);
  deliverAll();
  gNowMs += 800;
  g2HoldAbortFromPacket(gR, 1, gWidgetLifecycleEpoch, G2HoldReason::Overlay);
  CHECK(gQueue.size() == 1 && gQueue[0].job.phase == 0 &&
            gQueue[0].job.reason == (uint8_t)G2HoldReason::Overlay,
        "FG_ENTER queues an overlay abort for the open press");
  deliverAll();
  CHECK(gPageLog.calls.size() == 2 && gPageLog.calls[1].first == 0,
        "the consuming page is aborted when the overlay rises");
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "800ms aborted:overlay"),
        "the overlay abort is posted as the end of the hold");
  g2HoldAbortFromPacket(gR, 1, gWidgetLifecycleEpoch, G2HoldReason::FgExit);
  CHECK(gQueue.empty(), "its later FG_EXIT finds nothing open");

  // An overlay seen only through SyncInfo (or a lost FG_ENTER): the next
  // main-loop tick ends the press.
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
  rx('R', 9, 2);
  deliverAll();
  gNowMs += 300;
  gFirmwareOverlayUp = gOverlay = true;
  g2HoldExpireTick();
  CHECK(gQueue.size() == 1 && gQueue[0].job.phase == 0 &&
            gQueue[0].job.reason == (uint8_t)G2HoldReason::Overlay &&
            gQueue[0].job.heldMs == 300,
        "a press open under the overlay is aborted on the next tick");
  deliverAll();
  CHECK(gPageLog.calls.size() == 2 && gPageLog.calls[1].first == 0,
        "and its consumer gets phase 0");
  g2HoldExpireTick();
  CHECK(gQueue.empty(), "only once");

  // A stale raw flag the overlay helper no longer honours ends nothing.
  reset();
  rx('R', 9, 2);
  deliverAll();
  gFirmwareOverlayUp = true;
  g2HoldExpireTick();
  CHECK(gQueue.empty(), "the raw overlay flag alone is only a hint");
  gFirmwareOverlayUp = false;
  gNowMs += 900;
  rx('R', 10, 2);
  CHECK(gQueue.size() == 1 && gQueue[0].job.phase == 10,
        "the press still pairs with its release");
  deliverAll();

  // Terminal loss.
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
  rx('R', 9, 2);
  deliverAll();
  CHECK(g2HoldAbort(G2HoldReason::Terminal, 'R'), "terminal closes the press");
  CHECK(!g2HoldAbort(G2HoldReason::Terminal, 'R'), "and only once");
  deliverAll();
  CHECK(gPageLog.calls.size() == 2 && gPageLog.calls[1].first == 0,
        "terminal loss aborts the consumer");

  // A lost release: the next press aborts the old consumer first.
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
  rx('R', 9, 2);
  deliverAll();
  gNowMs += 2000;
  rx('R', 9, 2);
  CHECK(gQueue.size() == 2 && gQueue[0].job.phase == 0 &&
            gQueue[0].job.reason == (uint8_t)G2HoldReason::LostRelease &&
            gQueue[1].job.phase == 9 &&
            gQueue[1].job.pressId == gQueue[0].job.pressId + 1,
        "PressAfterLostRelease queues the old press's abort ahead of the new");
  deliverAll();
  CHECK(gPageLog.calls.size() == 3 && gPageLog.calls[1].first == 0 &&
            gPageLog.calls[2].first == 9,
        "the old consumer is aborted before the new press is routed");

  // Widget lifecycle boundary without any exit event.
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
  rx('R', 9, 2);
  deliverAll();
  gWidgetLifecycleEpoch = gWidgetLifecycleEpoch + 1;
  g2HoldExpireTick();
  CHECK(gQueue.size() == 1 &&
            gQueue[0].job.reason == (uint8_t)G2HoldReason::Lifecycle,
        "a lifecycle boundary aborts the open press on the next tick");
  deliverAll();
  CHECK(gPageLog.calls.size() == 2 && gPageLog.calls[1].first == 0,
        "and its consumer");

  // Expiry follows the note length: notemaxsec + 15 s.
  reset();
  gNoteMaxMs = 30000;   // a CM5 build's cap
  CHECK(g2HoldMaxOpenMs() == 45000, "the bound is the longest note + 15 s");
  rx('R', 9, 2);
  deliverAll();
  gNowMs += 44000;
  g2HoldExpireTick();
  CHECK(gQueue.empty(), "not before the bound");
  gNowMs += 1000;
  g2HoldExpireTick();
  CHECK(gQueue.size() == 1 &&
            gQueue[0].job.reason == (uint8_t)G2HoldReason::Expired,
        "a press open past g2HoldMaxOpenMs() expires");
  deliverAll();
  CHECK(gNotes.calls.size() == 2 && gNotes.calls[1].kind == 'A' &&
            gNotes.calls[1].arg == (uint32_t)G2HoldReason::Expired,
        "and its note is ended");
}

static void test_own_shutdown_echo_keeps_the_press() {
  // A hold on another HW1 page: the global action shows Notes, a Shutdown +
  // CREATE inside the Hub app, and both arms echo SYSTEM_EXIT reason 1 for
  // that Shutdown while the finger is still down (E-3, probe H2).
  reset();
  gPage = G2_HIJACK_PAGE_APPS;
  rx('R', 9, 2);
  const uint32_t id = gQueue.empty() ? 0 : gQueue[0].job.pressId;
  deliverAll();
  CHECK(gNotes.calls.size() == 1 && gNotes.calls[0].kind == 'B',
        "the global action takes the press");
  gPage = G2_HIJACK_PAGE_NOTES;   // the swap the action started
  gPresentation++;
  gEchoLease = true;
  gNowMs += 400;
  sysEventTail(gR, 7, 1);
  sysEventTail(gL, 7, 1);
  gEchoLease = false;
  CHECK(gTerminals == 0 && gOverlayResets == 2,
        "HW1's own Shutdown echo is not a terminal exit");
  CHECK(gQueue.empty(), "and closes no open press");
  gNowMs += 2000;
  rx('R', 10, 2);
  CHECK(gQueue.size() == 1 && gQueue[0].job.phase == 10 &&
            gQueue[0].job.pressId == id && gQueue[0].job.heldMs == 2400,
        "the release after the swap pairs with the press");
  deliverAll();
  CHECK(gNotes.calls.size() == 2 && gNotes.calls[1].kind == 'E' &&
            gNotes.calls[1].pressId == id && gNotes.calls[1].arg == 2400,
        "the note gets End, not Abort");
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "2400ms"),
        "the release is posted as a release");

  // A hold on the Notes page itself, whose view change rebuilds the page.
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_NOTES, fakePageHold);
  gPage = G2_HIJACK_PAGE_NOTES;
  rx('R', 9, 2);
  deliverAll();
  gPresentation++;
  gEchoLease = true;
  sysEventTail(gR, 7, 1);
  gEchoLease = false;
  gNowMs += 1800;
  rx('R', 10, 2);
  deliverAll();
  CHECK(gPageLog.calls.size() == 2 && gPageLog.calls[1].first == 10,
        "the page that rebuilt itself gets the release");

  // Not HW1's echo: reason 1 outside the lease, or another reason inside it,
  // is a terminal exit; the press is aborted once and its note ended.
  const struct { bool lease; uint32_t reason; } exits[] = {{false, 1}, {true, 2}};
  for (const auto& x : exits) {
    reset();
    rx('R', 9, 2);
    deliverAll();
    gEchoLease = x.lease;
    gNowMs += 600;
    sysEventTail(gR, 7, x.reason);
    gEchoLease = false;
    CHECK(gTerminals == 1 && gQueue.size() == 1 && gQueue[0].job.phase == 0 &&
              gQueue[0].job.reason == (uint8_t)G2HoldReason::Terminal,
          "a real SYSTEM_EXIT aborts the open press once");
    deliverAll();
    CHECK(gNotes.calls.size() == 2 && gNotes.calls[1].kind == 'A',
          "and the note is ended (it saves what was said)");
    rx('R', 10, 2);
    CHECK(gQueue.empty(), "a release after the exit pairs with nothing");
  }

  // The other page exits still end the press, inside the lease too.
  const struct { uint32_t etype; G2HoldReason why; } others[] = {
    {4, G2HoldReason::Overlay}, {5, G2HoldReason::FgExit},
    {6, G2HoldReason::AbnormalExit}};
  for (const auto& x : others) {
    reset();
    rx('R', 9, 2);
    deliverAll();
    gEchoLease = true;
    sysEventTail(gR, x.etype, 1);
    gEchoLease = false;
    CHECK(gTerminals == 0 && gQueue.size() == 1 && gQueue[0].job.phase == 0 &&
              gQueue[0].job.reason == (uint8_t)x.why,
          "FG_ENTER / FG_EXIT / ABNORMAL_EXIT abort the open press");
    deliverAll();
  }
}

static void test_queue_full_and_reorder() {
  reset();
  gQueueFull = true;
  rx('R', 9, 2);
  CHECK(gHoldCounters.queueFull == 1 &&
            outcomeOf(newestPressRecord()) == "Press→ignored:queue-full",
        "a press the queue refuses is marked queue-full");
  gQueueFull = false;
  gNowMs += 900;
  rx('R', 10, 2);
  CHECK(gQueue.size() == 1, "its release still queues");
  deliverAll();
  CHECK(gEvents.empty(),
        "the dispatcher never saw the press: no event for either edge");

  // A consumed press whose release the queue refused: the next press
  // delivered aborts that stale consumer before it is routed.
  reset();
  gPageLog = PageLog();
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_MAIN, fakePageHold);
  rx('R', 9, 2);
  deliverAll();
  gQueueFull = true;
  gNowMs += 700;
  rx('R', 10, 2);
  gQueueFull = false;
  CHECK(gQueue.empty() && gHoldCounters.queueFull == 1, "the release was lost");
  gNowMs += 1000;
  rx('R', 9, 2);
  deliverAll();
  CHECK(gPageLog.calls.size() == 3 && gPageLog.calls[1].first == 0 &&
            gPageLog.calls[2].first == 9,
        "the stale consumer is aborted before the next press is routed");

  // An abort raised on another task overtakes the press it closes.
  reset();
  rx('R', 9, 2);
  const QueuedJob press = gQueue.front();
  gQueue.clear();
  CHECK(g2HoldAbort(G2HoldReason::Terminal, 'R'), "abort raced the press");
  gQueue.push_back(press);   // the press lands after its own abort
  deliverAll();
  CHECK(gEvents.empty(), "a press delivered after its end is dropped silently");
}

static void test_duplicates_and_stale_copies() {
  reset();
  rx('R', 9, 2, G2HoldVia::Sys);
  rx('L', 9, 0, G2HoldVia::Sys);
  rx('R', 9, 0, G2HoldVia::Text);
  CHECK(gQueue.size() == 1, "mirrored and Text copies queue nothing more");
  gNowMs += 1000;
  rx('R', 10, 2, G2HoldVia::Sys);
  rx('L', 10, 0, G2HoldVia::Sys);
  CHECK(gQueue.size() == 2, "a mirrored release queues nothing more");
  rx('R', 10, 2, G2HoldVia::Sys, /*generation=*/9);
  CHECK(gQueue.size() == 2, "a stale-lifecycle copy queues nothing");
  deliverAll();
  CHECK(gEvents.size() == 2, "one press and one release event");
}

static void test_event_rate_limit() {
  reset();
  gSettings.g2HoldAction = 0;
  for (int i = 0; i < 4; ++i) {
    rx('R', 9, 2);
    gNowMs += 450;
    rx('R', 10, 2);
    gNowMs += 50;
    deliverAll();
  }
  CHECK(gHoldCounters.eventsLimited > 0, "rapid holds hit the rate limit");
  for (size_t i = 2; i < gEvents.size(); ++i) {
    CHECK(gEvents[i].atMs - gEvents[i - 2].atMs >= 1000,
          "never more than two events in any second");
  }
  CHECK(gHoldCounters.eventsPosted == gEvents.size(),
        "posted events are counted");
}

static void test_sim() {
  reset();
  std::strcpy(gL.firmwareVersion, "2.3.3.1");   // sim skips the profile gate
  std::strcpy(gR.firmwareVersion, "2.3.3.1");
  const std::string pressed = g2HoldSim(String("press"), 2);
  CHECK(pressed.find("sim press") != std::string::npos &&
            pressed.find("Press→pending") != std::string::npos,
        "sim press reports the pending press");
  CHECK(gQueue.size() == 1 && gQueue[0].job.via == (uint8_t)G2HoldVia::Sim &&
            gQueue[0].job.side == 'R' && gQueue[0].job.source == 2 &&
            gQueue[0].job.reason == 0 && gQueue[0].epoch == gPresentation,
        "sim queues a right-arm press on the current presentation, ungated");
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "quicknote"),
        "sim reaches the dispatcher past the profile gate");
  gNowMs += 1500;
  (void)g2HoldSim(String("release"), 0);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "1500ms"),
        "sim release pairs with the sim press");
  CHECK(std::string(g2HoldSim(String("abort"), 0)) ==
            "G2 hold sim abort: no press was open",
        "sim abort with nothing open says so");
  gNowMs += 1000;   // leave the event rate window of the previous hold
  (void)g2HoldSim(String("press"), 2);
  const std::string aborted = g2HoldSim(String("abort"), 0);
  CHECK(aborted.find("Aborted(sim)") != std::string::npos,
        "sim abort closes the open press");
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_RELEASE, "ring", "0ms aborted:sim"),
        "and is posted as the end of the hold");
  CHECK(std::string(g2HoldSim(String("bogus"), 0)).rfind("Error:", 0) == 0,
        "an unknown sim verb is an error");

  // Overlay still applies to sim (only the profile gate is skipped).
  reset();
  gOverlay = true;
  (void)g2HoldSim(String("press"), 2);
  deliverAll();
  CHECK(lastEventIs(SYSEVT_G2_LONG_PRESS, "ring", "ignored:overlay"),
        "sim keeps every gate but the profile");
  gOverlay = false;
  (void)g2HoldSim(String("release"), 0);
  deliverAll();
}

static void test_events_carry_no_text() {
  // Every event so far: subject from the source vocabulary, detail from the
  // consumer/reason/duration vocabulary (R22). Re-run a mixed session.
  reset();
  gSettings.g2HoldAction = 0;
  rx('R', 9, 2);
  gNowMs += 1000;
  rx('R', 10, 2);
  deliverAll();
  for (const Event& e : gEvents) {
    CHECK(e.subject == "ring" || e.subject == "left" || e.subject == "right" ||
              e.subject == "unknown",
          "subjects are source names only");
    bool ok = e.detail.rfind("ignored:", 0) == 0 ||
              e.detail.rfind("refused:", 0) == 0 ||
              e.detail == "transcription" || e.detail == "quicknote";
    if (!ok) {
      size_t i = 0;
      while (i < e.detail.size() && e.detail[i] >= '0' && e.detail[i] <= '9') ++i;
      ok = i > 0 && e.detail.compare(i, 2, "ms") == 0;
    }
    CHECK(ok, "details are consumers, reasons or durations only");
  }
}

static void test_page_table_registration() {
  CHECK(g2RegisterPageHold(G2_HIJACK_PAGE_TRANSCRIPTION, fakePageHold),
        "a page id registers");
  CHECK(g2RegisterPageHold(G2_HIJACK_PAGE_NOTES, fakePageHold),
        "the highest page id (Notes) registers too");
  CHECK(!g2RegisterPageHold((G2HijackPage)32, fakePageHold),
        "ids past the table are refused");
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_NOTES, nullptr);
  (void)g2RegisterPageHold(G2_HIJACK_PAGE_TRANSCRIPTION, nullptr);
}

int main() {
  test_global_quicknote();
  test_global_page_busy();
  test_action_off_events();
  test_source_policy();
  test_source_upgrade_from_later_copy();
  test_unprofiled();
  test_page_handler();
  test_gates();
  test_watchdog_activity();
  test_aborts_reach_the_consumer();
  test_own_shutdown_echo_keeps_the_press();
  test_queue_full_and_reorder();
  test_duplicates_and_stale_copies();
  test_event_rate_limit();
  test_sim();
  test_events_carry_no_text();
  test_page_table_registration();
  CHECK(gCriticalDepth == 0 && gCriticalMax == 1,
        "spinlocks never nest and always release");
  if (failures) {
    std::fprintf(stderr, "%d G2 hold dispatch check(s) failed\n", failures);
    return 1;
  }
  std::puts("G2 hold dispatch harness passed");
  return 0;
}
