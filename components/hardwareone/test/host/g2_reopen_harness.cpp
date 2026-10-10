#include <cstdint>
#include <cstdio>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using String = std::string;
static unsigned checks = 0;
static void check(bool ok, const char* why) {
  ++checks;
  if (!ok) throw std::runtime_error(why);
}
#define DEBUG_G2F(...) do {} while (0)
#define BROADCAST_PRINTF(...) do {} while (0)
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
#define pdMS_TO_TICKS(ms) (ms)
struct G2Temple {
  bool connected = true, pluginDead = false, containerReady = true;
  uint32_t connectionGeneration = 4;
  char side = 'R';
};
static G2Temple gR;
static uint32_t gWidgetLifecycleEpoch = 7, gHijackStartedMs = 0;
static bool terminalLatched = false, hijackActive = true;
static int gSyncLifecycleMux, gTopologyMux;
static unsigned lockDepth = 0;
static void portENTER_CRITICAL(int*) { ++lockDepth; }
static void portEXIT_CRITICAL(int*) { --lockDepth; }
static bool completionAvailable = true;
struct G2CompletionGuard {
  explicit operator bool() const { return completionAvailable; }
};
static uint32_t now = 100, releaseLeaseAt = 0;
static bool leaseHeld = false, queueAvailable = true, teardownOk = true;
static bool cancelOk = true, createOk = true, publishOk = true;
static bool terminalWire = false, terminalKickOk = true;
static std::vector<std::string> events;
static std::function<void()> delayHook, cancelHook, teardownHook;
static uint32_t millis() { return now; }
static void vTaskDelay(uint32_t ms) {
  check(lockDepth == 0, "delay stays outside critical sections");
  now += ms;
  if (delayHook) { auto hook = std::move(delayHook); delayHook = {}; hook(); }
  if (releaseLeaseAt && now >= releaseLeaseAt && leaseHeld) {
    leaseHeld = false; events.emplace_back("lease-released");
  }
}
static bool interactiveLensLeaseHeld() { return leaseHeld; }
static bool g2LensTxFenceCurrent(const G2Temple& arm, uint32_t generation,
                                uint32_t epoch) {
  return generation && epoch && arm.connected && !arm.pluginDead &&
         generation == arm.connectionGeneration &&
         epoch == gWidgetLifecycleEpoch && !terminalLatched;
}
static bool tearDownActiveContainerAtGeneration(G2Temple& arm,
                                                uint32_t generation,
                                                bool force, uint32_t epoch) {
  check(!leaseHeld, "manual Shutdown waits for old interactive cleanup");
  check(force, "manual Shutdown remains forced when mirror says no container");
  if (!g2LensTxFenceCurrent(arm, generation, epoch)) return false;
  events.emplace_back("shutdown");
  if (teardownHook) teardownHook();
  if (!teardownOk || !g2LensTxFenceCurrent(arm, generation, epoch)) return false;
  arm.containerReady = false;
  return true;
}
static uint8_t allocSeq() { return 1; }
static constexpr unsigned G2_MAGIC_MENU_FAILED = 1;
static size_t g2BuildMenuStartupFailed(uint8_t, unsigned, int, const char*,
                                      uint8_t*, size_t) { return 10; }
static bool sendEnvelopeAtGeneration(G2Temple& arm, const uint8_t*, size_t,
                                     uint32_t generation, bool*, uint32_t,
                                     uint32_t, uint32_t epoch) {
  check(lockDepth == 0, "launch cancellation stays outside critical sections");
  if (cancelHook) cancelHook();
  if (!g2LensTxFenceCurrent(arm, generation, epoch)) return false;
  events.emplace_back("cancel");
  return cancelOk;
}
static constexpr size_t G2_PAGE_REGISTRY_MAX = 8;
static constexpr uint32_t BLOCKS_WIDGET_ID = 10509;
static constexpr int G2_GEOM_LARGE = 1, G2_HIJACK_PAGE_MAIN = 0;
static size_t populateHijackMenuItems(const char** rows, size_t) {
  rows[0] = "System"; rows[1] = "Apps"; return 2;
}
static bool sendCreateListAndWait(G2Temple&, const char* const*, size_t,
                                   uint32_t, int) {
  check(!leaseHeld, "CREATE never overlaps old interactive lease");
  events.emplace_back("create"); return createOk;
}
static void g2LensSetHijackActive(bool on) { hijackActive = on; }
static bool g2NoteCreateSuccess(G2Temple& arm, bool, uint32_t, uint32_t epoch) {
  if (!publishOk || !g2LensTxFenceCurrent(arm, arm.connectionGeneration, epoch))
    return false;
  events.emplace_back("published"); arm.containerReady = true; return true;
}
enum class HijackEvent { HijackExit };
static void hijackFsmDispatch(HijackEvent, const char*) { hijackActive = false; }
enum class HijackListPageShape { PureList };
static size_t gLastHijackListRowCount;
static HijackListPageShape gLastHijackListPageShape;
static void g2SetHijackPage(int) {}
static void g2PushStatusEvent(const char* event) { events.emplace_back(event); }
static constexpr int SYSEVT_G2_HIJACK_ENTERED = 1;
static void systemEventPost(int, const char*) {}
static void g2SyncLifecycleResetLocked(bool clear) {
  ++gWidgetLifecycleEpoch;
  if (clear) terminalLatched = false;
}
static void g2CreateQuarantineClearAtLifecycleBoundary() {}
static bool g2TextEntryCleanupKick() { return true; }
static void kbdPadSysDoubleUnpublish() {}
static void g2PresentationEpochAdvance() {}
static void g2SessionRequestAbort() { events.emplace_back("abort-request"); }
static bool g2TerminalWireBlocksCreate() { return terminalWire; }
static bool g2TerminalShutdownWireKick() {
  events.emplace_back("terminal-wire-queued"); return terminalKickOk;
}
static bool g2FsmHijackActive() { return hijackActive; }
struct CustomSpec { void (*run)(uint32_t); uint32_t lifecycleEpoch; };
enum class LensJobKind { Custom };
enum class G2LensEnqueueWait { NoWait };
struct LensUiJob {
  LensJobKind kind;
  uint32_t submitMenuGen;
  uint64_t cmdSeq;
  int targetPage, targetNetSub;
  struct { CustomSpec* custom; } payload;
};
static LensUiJob* queued = nullptr;
static uint32_t g2CurrentMenuGen() { return 1; }
static int g2GetHijackPage() { return 0; }
static bool g2EnqueueLensJob(LensUiJob* job, G2LensEnqueueWait) {
  check(!queued, "only one test job queued");
  if (!queueAvailable) return false;
  queued = job; events.emplace_back("queued"); return true;
}

// INSERT_PRODUCTION

static void drain() {
  check(queued != nullptr, "bootstrap queued");
  auto* job = queued; queued = nullptr;
  job->payload.custom->run(job->payload.custom->lifecycleEpoch);
  delete job->payload.custom; delete job;
}
static void reset() {
  check(!queued, "previous queued job consumed");
  gR = {}; gWidgetLifecycleEpoch = 7; gHijackStartedMs = 0;
  terminalLatched = false; hijackActive = true; lockDepth = 0;
  completionAvailable = true; now = 100; releaseLeaseAt = 0;
  leaseHeld = false; queueAvailable = teardownOk = cancelOk = true;
  createOk = publishOk = true; terminalWire = false; terminalKickOk = true;
  events.clear(); delayHook = cancelHook = teardownHook = {};
}
static size_t indexOf(const char* event) {
  for (size_t i = 0; i < events.size(); ++i) if (events[i] == event) return i;
  return events.size();
}
static bool saw(const char* event) { return indexOf(event) != events.size(); }
static bool error(const char* value) { return std::string(value).find("Error:") == 0; }
static void reopen() { check(!error(cmd_g2reopen("")), "reopen accepted"); }

int main() {
  reset();
  check(handleMenuStartUp(gR, BLOCKS_WIDGET_ID, 4, 7), "real cmd17 admitted");
  check(!saw("cancel") && !saw("shutdown"), "admission never blocks on wire");
  drain();
  check(!saw("shutdown") && saw("cancel") && saw("published"),
        "real cmd17 retains cancel then CREATE without premature Shutdown");
  reset(); leaseHeld = true; releaseLeaseAt = now + 500;
  check(handleMenuStartUp(gR, BLOCKS_WIDGET_ID, 4, 7), "real cmd17 with lease admitted");
  drain();
  check(!saw("shutdown") && indexOf("cancel") < indexOf("lease-released") &&
        indexOf("lease-released") < indexOf("create") && saw("published"),
        "real cmd17 cancels launch before waiting for old lease and creating");

  for (bool ready : {false, true}) {
    reset(); gR.containerReady = ready;
    const std::string reply = cmd_g2reopen("");
    check(reply.find("queued") != String::npos && reply.find("shown") == String::npos,
          "manual command reports queue acceptance, never visible success");
    check(gR.containerReady == ready,
          "manual admission leaves container mirror for fenced teardown");
    drain();
    check(indexOf("shutdown") < indexOf("cancel") &&
          indexOf("cancel") < indexOf("create") && saw("published"),
          "manual reopen tears down live or uncertain widget before shared CREATE");
  }
  reset(); leaseHeld = true; releaseLeaseAt = now + 30; reopen(); drain();
  check(indexOf("lease-released") < indexOf("shutdown") && saw("published"),
        "old worker finishes before reopen teardown");
  reset(); leaseHeld = true; reopen(); drain();
  check(now == 3600 && !saw("shutdown") && !saw("create"),
        "stuck lease refuses bootstrap after bounded wait");

  for (int boundary = 0; boundary < 3; ++boundary) {
    reset(); leaseHeld = true; reopen();
    delayHook = [boundary] {
      if (boundary == 0) ++gWidgetLifecycleEpoch;
      else if (boundary == 1) ++gR.connectionGeneration;
      else terminalLatched = true;
    };
    drain();
    check(!saw("shutdown") && !saw("create"),
          "replacement lifecycle/link/terminal while waiting cannot be touched");
  }
  reset(); teardownOk = false; reopen(); drain();
  check(saw("shutdown") && !saw("cancel") && !saw("create"),
        "failed teardown cannot proceed to CREATE");
  reset(); reopen(); teardownHook = [] { ++gWidgetLifecycleEpoch; }; drain();
  check(!saw("cancel") && !saw("create"), "new lifecycle during teardown wins");
  reset(); reopen(); cancelHook = [] { ++gR.connectionGeneration; }; drain();
  check(!saw("cancel") && !saw("create"), "cancel TX carries exact generation");
  reset(); reopen(); delayHook = [] { ++gWidgetLifecycleEpoch; }; drain();
  check(saw("cancel") && !saw("create"), "new lifecycle during cancel settle wins");
  reset(); cancelOk = false; reopen(); drain();
  check(!saw("create"), "failed launch cancellation cannot CREATE");
  reset(); createOk = false; reopen(); drain();
  check(saw("hijack-fail") && !saw("published"), "missing CREATE ack never publishes");
  reset(); publishOk = false; reopen(); drain();
  check(saw("create") && !saw("published") && !saw("hijack-on") && !hijackActive,
        "rejected CREATE publication revokes hijack and emits no success event");

  for (auto command : {cmd_g2reopen, cmd_g2hijacktest}) {
    reset(); queueAvailable = false;
    check(error(command("")) && !queued, "queue refusal reaches command caller");
    reset(); completionAvailable = false;
    check(error(command("")) && !queued, "lifecycle admission refusal reaches caller");
    reset(); gR.connected = false;
    check(error(command("")) && !queued, "disconnected command refuses startup");
  }
  reset(); gR.pluginDead = true;
  check(error(cmd_g2reopen("")) && !queued, "silent plugin requires reconnect");
  reset(); terminalWire = true; terminalKickOk = false;
  check(error(cmd_g2reopen("")) && !queued, "terminal wire ordering refusal propagates");
  reset();
  check(!handleMenuStartUp(gR, BLOCKS_WIDGET_ID, 3, 7) && !queued,
        "stale firmware generation rejected");
  reset();
  check(!error(cmd_g2hijacktest("")), "synthetic cmd17 reports queue acceptance");
  drain();
  check(!saw("shutdown") && saw("published"), "hijacktest retains cmd17 path");
  std::printf("G2 reopen: %u assertions passed (production admission/bootstrap/commands)\n", checks);
}
