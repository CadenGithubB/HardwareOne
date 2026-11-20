// The C6 companion's console mirror and lifetime fault counts, as the owner
// wires them: the shipping serviceFaults, consoleMirrorProc, noteLifetimeFault,
// faultCountsSaveProc, mirrorReason, mirrorText, dispatch, radioCompanionBootInit
// and cmd_c6console (spliced from System_RadioCompanion.cpp by
// test_companion_fault_mirror.py) over the real Radio_CompanionCore.h, with a
// recording command executor, a fake mirror, a fake clock and a fake settings
// writer. What runs on which task is part of what is checked: the mirror is
// started and stopped, and settings are saved, only on the command executor.
#include <atomic>
#include <cassert>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "../../Radio_CompanionCore.h"

using hw1::radio::CompanionAction;
using hw1::radio::CompanionMonitor;
using hw1::radio::CompanionOutage;
using hw1::radio::CompanionPolicy;
using hw1::radio::CompanionState;
using hw1::radio::CompanionStats;
using hw1::radio::ConsoleMirrorPolicy;
using hw1::radio::MirrorStep;
using hw1::radio::companionOutageName;

// ---- SDK and project fakes ---------------------------------------------------
typedef int esp_err_t;
enum { ESP_OK = 0, ESP_ERR_NO_MEM = 0x101, ESP_ERR_NOT_SUPPORTED = 0x106 };
static const char* esp_err_to_name(esp_err_t err) {
  return err == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : err == ESP_ERR_NOT_SUPPORTED ? "ESP_ERR_NOT_SUPPORTED" : "ESP_OK";
}
struct portMUX_TYPE {};
static portMUX_TYPE sLock;
static int gLockDepth = 0;
#define portENTER_CRITICAL(mux) do { (void)(mux); gLockDepth++; } while (0)
#define portEXIT_CRITICAL(mux) do { (void)(mux); gLockDepth--; } while (0)
#define RTC_DATA_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() do { } while (0)

static uint32_t gMillis = 0;
static uint32_t millis() { return gMillis; }

static std::vector<std::string> gLog;     // logSystemEvent lines
static std::vector<std::string> gTrace;   // what was queued, in order
static bool gOnCmdExec = false;           // the fake executor is running a job
static bool gQueueFull = false;

static void logSystemEvent(const char* category, const char* fmt, ...) {
  assert(gLockDepth == 0);  // never under the spinlock
  char text[320];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  gLog.push_back(std::string(category) + ": " + text);
}
static bool logged(const char* part) {
  for (const std::string& line : gLog) {
    if (line.find(part) != std::string::npos) return true;
  }
  return false;
}
static int logCount(const char* part) {
  int n = 0;
  for (const std::string& line : gLog) n += line.find(part) != std::string::npos;
  return n;
}

// The command executor: one queue, jobs in order.
struct ExecReq {
  typedef void (*DeferredFn)(void*);
};
struct Job {
  ExecReq::DeferredFn fn;
  void* arg;
  std::string name;
};
static std::vector<Job> gQueue;
static const char* jobName(ExecReq::DeferredFn fn, void* arg);
static bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg) {
  assert(gLockDepth == 0);
  if (gQueueFull) return false;
  gQueue.push_back({fn, arg, jobName(fn, arg)});
  gTrace.push_back(gQueue.back().name);
  return true;
}
static void runCmdExec() {
  while (!gQueue.empty()) {
    const Job job = gQueue.front();
    gQueue.erase(gQueue.begin());
    gOnCmdExec = true;
    if (job.fn) job.fn(job.arg);
    gOnCmdExec = false;
  }
}

// The mirror (HAL_Radio.cpp).
struct FakeMirror {
  bool supported = true;
  bool running = false;
  esp_err_t startResult = ESP_OK;
  int starts = 0;   // attempts
  int stops = 0;
} gMirror;
static esp_err_t radioHalConsoleMirror(bool on) {
  assert(gOnCmdExec);        // after boot only the command executor touches it
  assert(gLockDepth == 0);   // a stop waits for the mirror task
  if (!gMirror.supported) return on ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
  if (on == gMirror.running) return ESP_OK;
  if (on) {
    gMirror.starts++;
    if (gMirror.startResult != ESP_OK) return gMirror.startResult;
    gMirror.running = true;
    return ESP_OK;
  }
  gMirror.stops++;
  gMirror.running = false;
  return ESP_OK;
}
static bool radioHalConsoleMirrorOn() { return gMirror.running; }
static bool radioHalConsoleMirrorSupported() { return gMirror.supported; }

// Settings: the fields the code under test reads and writes.
struct FakeSettings {
  bool c6ConsoleMirror = false;
  uint32_t c6OutageCount = 0;
  uint32_t c6RebootCount = 0;
  uint32_t c6LastFaultEpochSec = 0;
} gSettings;
static int gPersists = 0;
static bool gPersistResult = true;
static int gSetSettingCalls = 0;
static bool requestSettingsPersist() {
  assert(gOnCmdExec);  // the settings writer never runs on the main loop here
  gPersists++;
  return gPersistResult;
}
static void setSetting(bool& field, bool value) {
  assert(&field == &gSettings.c6ConsoleMirror);  // the counters never go through setSetting
  gSetSettingCalls++;
  field = value;
}

namespace Clock {
static time_t gNow = 0;
static time_t epochSeconds() { return gNow; }
static bool isPlausibleEpoch(time_t t) { return t >= 1577836800 && t < 4102444800LL; }
}  // namespace Clock

// The monitor and its accessors, as in System_RadioCompanion.cpp.
static CompanionMonitor sMonitor;
template <typename F>
static void withMonitor(F fn) {
  portENTER_CRITICAL(&sLock);
  fn(sMonitor);
  portEXIT_CRITICAL(&sLock);
}
static CompanionState monitorState() { return sMonitor.state(); }
static CompanionStats monitorStats() { return sMonitor.stats(); }

// dispatch's other collaborators.
enum class Op : uint8_t { None = 0, Start, Recover, Hold, Release, Update };
static bool gSubmitOpOk = true;
static void startProc(void*) {}
static void recoverProc(void*) {}
static bool submitOp(Op op, ExecReq::DeferredFn fn, void* arg) {
  if (!gSubmitOpOk) return false;
  (void)op;
  return submitDeferredToCmdExec(fn, arg);
}
enum { SYSEVT_COMPANION_OFFLINE = 1, SYSEVT_COMPANION_REBOOT };
static void systemEventPost(int, const char*, const char*) {}
static void broadcastOutput(const char*) {}
static int gReboots = 0;
static void rebootDevice(const char* reason, const char*, uint32_t) {
  assert(std::strcmp(reason, "c6-companion") == 0);
  gReboots++;
}
static bool anyRadioUser() { return false; }
static esp_err_t radioHalHold(bool) { return ESP_OK; }
static uint32_t gCompanionRebootMarker = 0;

// radioCompanionBootInit's other collaborators.
static bool sBootInitDone = false;
static bool sEverOnline = false;
static bool sHardRebootAllowed = true;
static bool gBootTransportUp = true;
struct RadioHalEventData {};
static void onHalEvent(const RadioHalEventData&) {}
static void radioHalSetEventHandler(void (*)(const RadioHalEventData&)) {}
static void applyPolicyFromSettings() {
  CompanionPolicy policy;
  policy.heartbeatMs = 5000;
  policy.hardRebootAllowed = sHardRebootAllowed;
  withMonitor([&](CompanionMonitor& m) { m.setPolicy(policy); });
}
static bool radioHalPrepared() { return gBootTransportUp; }
static bool radioHalTransportUp() { return gBootTransportUp; }
static void reportIdentity() {}
static void setLastResult(const char*) {}

// cmd_c6console's other collaborators.
class String {
 public:
  String() {}
  String(const char* text) : text_(text ? text : "") {}  // NOLINT: Arduino's implicit conversion
  size_t length() const { return text_.size(); }
  void toLowerCase() {
    for (char& c : text_) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  }
  const std::string& str() const { return text_; }

 private:
  std::string text_;
};
struct CommandArgs {
  explicit CommandArgs(const String& line) : line_(line) {}
  String arg(int) const { return line_; }
  String line_;
};
static int parseBoolArg(const String& arg) {
  if (arg.str() == "on" || arg.str() == "1") return 1;
  if (arg.str() == "off" || arg.str() == "0") return 0;
  return -1;
}
static const char kCommandReplyNotOnOff[] = "Error: expected on or off";
static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }

// ---- the shipping code ---------------------------------------------------------
/*@CONSTANTS@*/
static ConsoleMirrorPolicy sMirror(kMirrorHealthyMs);
static uint32_t sFaultsSeen = 0;
static uint32_t sOutagesCounted = 0;
static std::atomic<bool> sFaultCountsDirty{false};

/*@SERVICE@*/

/*@DISPATCH@*/

/*@BOOT@*/

/*@CONSOLE@*/

static const char* jobName(ExecReq::DeferredFn fn, void* arg) {
  if (fn == consoleMirrorProc) return arg ? "mirror-start" : "mirror-stop";
  if (fn == faultCountsSaveProc) return "save-counts";
  if (fn == recoverProc) return "op-recover";
  if (fn == startProc) return "op-start";
  return "other";
}

// ---- scenarios -----------------------------------------------------------------
#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      for (const std::string& line : gLog) std::fprintf(stderr, "  log %s\n", line.c_str()); \
      for (const std::string& line : gTrace) std::fprintf(stderr, "  queued %s\n", line.c_str()); \
      std::exit(1); \
    } \
  } while (0)

// A power-on: everything but the saved settings (and, when asked, the RTC marker).
static void powerOn(bool keepMarker = false) {
  sMonitor = CompanionMonitor();
  sMirror = ConsoleMirrorPolicy(kMirrorHealthyMs);
  sFaultsSeen = 0;
  sOutagesCounted = 0;
  sFaultCountsDirty.store(false);
  sBootInitDone = false;
  sEverOnline = false;
  sHardRebootAllowed = true;
  if (!keepMarker) gCompanionRebootMarker = 0;
  gMirror = FakeMirror();
  gQueue.clear();
  gTrace.clear();
  gLog.clear();
  gQueueFull = false;
  gSubmitOpOk = true;
  gPersists = 0;
  gPersistResult = true;
  gSetSettingCalls = 0;
  gReboots = 0;
  gBootTransportUp = true;
  gMillis = 1000;
  Clock::gNow = 1763640000;  // 2025-11-20T12:00:00Z
}
static void factoryNew() {
  gSettings = FakeSettings();
  powerOn();
}

// radioCompanionTick's part after the events: the monitor's tick, then dispatch.
static CompanionAction loopTick() {
  CompanionAction action;
  portENTER_CRITICAL(&sLock);
  action = sMonitor.tick(gMillis);
  portEXIT_CRITICAL(&sLock);
  dispatch(action, gMillis);
  return action;
}
// The main loop for `ms`, with a heartbeat every 5 s and the executor idle
// enough to run what is queued.
static void runHealthy(uint32_t ms) {
  const uint32_t end = gMillis + ms;
  uint32_t beat = 0;
  while (gMillis != end) {
    gMillis += 1000;
    Clock::gNow += 1;
    if ((gMillis / 1000) % 5 == 0) withMonitor([&](CompanionMonitor& m) { m.onHeartbeat(gMillis, ++beat); });
    CHECK(loopTick() == CompanionAction::None);
    runCmdExec();
  }
}
// An outage while online, soft recovery, companion back after `recoveryMs`.
static void outageAndRecovery(uint32_t recoveryMs = 8000) {
  withMonitor([&](CompanionMonitor& m) { m.onTransportFailure(gMillis); });
  CHECK(loopTick() == CompanionAction::Recover);
  runCmdExec();  // the mirror start, then the (fake, empty) recovery procedure
  gMillis += recoveryMs;
  Clock::gNow += recoveryMs / 1000;
  withMonitor([&](CompanionMonitor& m) { m.onRecoveryResult(gMillis, true); });
}

static void a_clean_boot_starts_no_mirror_and_saves_nothing() {
  factoryNew();
  radioCompanionBootInit();
  CHECK(gQueue.empty() && gTrace.empty());
  runHealthy(30 * 60 * 1000);
  CHECK(gMirror.starts == 0 && gMirror.stops == 0 && !gMirror.running);
  CHECK(gTrace.empty() && gPersists == 0);
  CHECK(gSettings.c6OutageCount == 0 && gSettings.c6RebootCount == 0 && gSettings.c6LastFaultEpochSec == 0);
  CHECK(std::strcmp(mirrorReason(), "off") == 0);
  CHECK(std::strstr(mirrorText(), "starts by itself after a companion fault") != nullptr);
}

static void a_saved_c6console_on_starts_it_at_boot_and_keeps_it() {
  factoryNew();
  gSettings.c6ConsoleMirror = true;
  radioCompanionBootInit();
  // Queued for the executor by boot init; boot init itself touched nothing
  // (the fake mirror asserts it is only ever called on the executor).
  CHECK((gTrace == std::vector<std::string>{"mirror-start"}));
  runCmdExec();
  CHECK(gMirror.running && gMirror.starts == 1);
  CHECK(std::strcmp(mirrorReason(), "saved") == 0);
  runHealthy(30 * 60 * 1000);
  outageAndRecovery();
  runHealthy(30 * 60 * 1000);
  CHECK(gMirror.running && gMirror.starts == 1 && gMirror.stops == 0);
  CHECK(gSettings.c6OutageCount == 1);
}

static void an_outage_starts_the_mirror_before_the_recovery_and_counts_once() {
  factoryNew();
  radioCompanionBootInit();
  runHealthy(60000);
  const time_t faultAt = Clock::gNow;
  withMonitor([&](CompanionMonitor& m) { m.onTransportFailure(gMillis); });
  CHECK(loopTick() == CompanionAction::Recover);
  // The mirror start is ahead of the recovery in the one queue; nothing was
  // saved or started on the loop; the count is in RAM already.
  CHECK((gTrace == std::vector<std::string>{"mirror-start", "op-recover"}));
  CHECK(gPersists == 0 && gMirror.starts == 0);
  CHECK(gSettings.c6OutageCount == 1 && gSettings.c6RebootCount == 0);
  CHECK(gSettings.c6LastFaultEpochSec == static_cast<uint32_t>(faultAt));
  CHECK(gSetSettingCalls == 0);
  runCmdExec();
  CHECK(gMirror.running && gMirror.starts == 1);
  CHECK(std::strcmp(mirrorReason(), "fault") == 0);
  CHECK(logCount("console mirror on after a companion fault") == 1);
  // The companion is back: the save follows on the next quiet tick, once.
  gMillis += 8000;
  withMonitor([&](CompanionMonitor& m) { m.onRecoveryResult(gMillis, true); });
  gTrace.clear();
  CHECK(loopTick() == CompanionAction::None);
  CHECK((gTrace == std::vector<std::string>{"save-counts"}));
  CHECK(gPersists == 0);  // queued, not written on the loop
  runCmdExec();
  CHECK(gPersists == 1);
  runHealthy(9 * 60 * 1000);
  CHECK(gPersists == 1 && gMirror.running);
  // Ten healthy minutes after the return: the mirror goes, once.
  runHealthy(60 * 1000 + 2000);
  CHECK(!gMirror.running && gMirror.stops == 1);
  CHECK(logCount("console mirror off") == 1);
  runHealthy(30 * 60 * 1000);
  CHECK(gMirror.starts == 1 && gMirror.stops == 1 && gPersists == 1);
  CHECK(gSettings.c6OutageCount == 1);
}

static void a_failed_mirror_start_is_tried_once_per_arming() {
  factoryNew();
  radioCompanionBootInit();
  runHealthy(60000);
  gMirror.startResult = ESP_ERR_NO_MEM;
  // The companion fails and every recovery and retry fails after it.
  withMonitor([&](CompanionMonitor& m) { m.onTransportFailure(gMillis); });
  CHECK(loopTick() == CompanionAction::Recover);
  runCmdExec();
  withMonitor([&](CompanionMonitor& m) { m.onRecoveryResult(gMillis, false); });
  for (int i = 0; i < 4000; i++) {  // more than an hour of retries on the backoff ladder
    gMillis += 1000;
    const CompanionAction action = loopTick();
    runCmdExec();
    if (action == CompanionAction::StartTransport) {
      withMonitor([&](CompanionMonitor& m) { m.onStartResult(gMillis, false); });
    }
  }
  CHECK(sMonitor.stats().startFailures >= 5);
  CHECK(gMirror.starts == 1);  // one attempt, no loop
  CHECK(logCount("console mirror not started: ESP_ERR_NO_MEM") == 1);
  CHECK(gSettings.c6OutageCount == 1);  // the retries are the same incident
  CHECK(gPersists == 1);                // saved once, while offline (the radio users are stopped)
}

static void the_reboot_escalation_counts_at_the_next_boot() {
  factoryNew();
  radioCompanionBootInit();
  runHealthy(60000);
  for (int i = 0; i < 3; i++) {  // three soft recoveries inside the window
    outageAndRecovery();
    runHealthy(20000);
  }
  CHECK(gSettings.c6OutageCount == 3 && gPersists == 3);
  // The fourth outage asks for the reboot.
  gTrace.clear();
  const int persistsBefore = gPersists;
  withMonitor([&](CompanionMonitor& m) { m.onTransportFailure(gMillis); });
  CHECK(loopTick() == CompanionAction::HardReboot);
  CHECK(gReboots == 1 && gCompanionRebootMarker != 0);
  // Nothing was queued, saved or counted in the boot that is ending: a save
  // here would run the settings writer against a dead companion.
  CHECK(gTrace.empty() && gPersists == persistsBefore);
  CHECK(gSettings.c6OutageCount == 3 && gSettings.c6RebootCount == 0);

  // The next boot: the marker adds the outage and the reboot, arms the mirror,
  // and writes nothing during boot.
  powerOn(/*keepMarker=*/true);
  radioCompanionBootInit();
  CHECK(gCompanionRebootMarker == 0 && !sHardRebootAllowed);
  CHECK(gSettings.c6OutageCount == 4 && gSettings.c6RebootCount == 1);
  CHECK(gPersists == 0);
  CHECK((gTrace == std::vector<std::string>{"mirror-start"}));
  runCmdExec();
  CHECK(gMirror.running && gPersists == 0);
  gMillis += 1000;
  CHECK(loopTick() == CompanionAction::None);  // the first quiet tick queues the save
  runCmdExec();
  CHECK(gPersists == 1);
  runHealthy(11 * 60 * 1000);
  CHECK(!gMirror.running && gPersists == 1);
  // A boot after that one starts clean again.
  powerOn();
  radioCompanionBootInit();
  runHealthy(60000);
  CHECK(gTrace.empty() && gSettings.c6OutageCount == 4 && gSettings.c6RebootCount == 1);
}

static void a_companion_that_does_not_come_up_at_boot_counts_and_mirrors() {
  factoryNew();
  gBootTransportUp = false;
  gMirror.running = true;  // radioHalPrepare started it after the failed first attempt
  Clock::gNow = 37;        // no clock yet at boot: an unset clock reads its uptime, not 0
  radioCompanionBootInit();
  CHECK(gSettings.c6OutageCount == 1 && gSettings.c6LastFaultEpochSec == 0);
  CHECK(gPersists == 0 && gTrace.empty());  // already running: nothing to queue, nothing written in boot
  CHECK(sMirror.armed());
  gMillis += 1000;
  CHECK(loopTick() == CompanionAction::None);
  runCmdExec();
  CHECK(gPersists == 1);
  // A retry brings it up; the mirror goes ten minutes later.
  Clock::gNow = 1763640000;
  gMillis = sMonitor.stats().nextRetryAt;
  CHECK(loopTick() == CompanionAction::StartTransport);
  withMonitor([&](CompanionMonitor& m) { m.onStartResult(gMillis, true); });
  runHealthy(9 * 60 * 1000);
  CHECK(gMirror.running);
  runHealthy(2 * 60 * 1000);
  CHECK(!gMirror.running && gMirror.stops == 1);
  CHECK(gSettings.c6OutageCount == 1 && gPersists == 1);
}

static void a_boot_retry_that_worked_still_earns_the_mirror_its_ten_minutes() {
  factoryNew();
  gMirror.running = true;  // attempt 1 failed, attempt 2 came up
  radioCompanionBootInit();
  CHECK(sMirror.armed() && gSettings.c6OutageCount == 0);  // not an outage: the companion is up
  runHealthy(9 * 60 * 1000);
  CHECK(gMirror.running);
  runHealthy(2 * 60 * 1000);
  CHECK(!gMirror.running && gPersists == 0);
}

static void c6console_off_ends_a_fault_mirror_and_on_keeps_it() {
  factoryNew();
  radioCompanionBootInit();
  runHealthy(60000);
  outageAndRecovery();
  runHealthy(5000);
  CHECK(gMirror.running);
  gOnCmdExec = true;  // commands run on the executor
  const char* reply = cmd_c6console(String("off"));
  gOnCmdExec = false;
  CHECK(std::strstr(reply, "off (saved)") != nullptr && std::strstr(reply, "after a companion fault") != nullptr);
  CHECK(!gMirror.running && !sMirror.armed() && !gSettings.c6ConsoleMirror);
  runHealthy(20 * 60 * 1000);
  CHECK(!gMirror.running && gMirror.starts == 1);
  // The next fault starts it again.
  outageAndRecovery();
  CHECK(gMirror.running && gMirror.starts == 2);
  // `c6console on` while a fault holds it: kept past the ten minutes.
  gOnCmdExec = true;
  reply = cmd_c6console(String("on"));
  CHECK(std::strstr(reply, "on (saved)") != nullptr);
  reply = cmd_c6console(String(""));
  CHECK(std::strstr(reply, "C6 console mirror: on (saved: c6console on; c6console off makes it fault-only)") != nullptr);
  gOnCmdExec = false;
  CHECK(gSettings.c6ConsoleMirror && gMirror.starts == 2);
  runHealthy(30 * 60 * 1000);
  CHECK(gMirror.running && gMirror.stops == 1);
  CHECK(std::strcmp(mirrorReason(), "saved") == 0);
  gOnCmdExec = true;
  CHECK(std::strstr(cmd_c6console(String("maybe")), "Error") != nullptr);
  gOnCmdExec = false;
}

static void a_fault_between_the_stop_request_and_the_stop_keeps_the_mirror() {
  factoryNew();
  radioCompanionBootInit();
  runHealthy(60000);
  outageAndRecovery();
  // Run to the tick that asks for the stop, without letting the executor run it.
  int guard = 0;
  while (gQueue.empty() || gQueue.back().name != "mirror-stop") {
    gMillis += 1000;
    if ((gMillis / 1000) % 5 == 0) withMonitor([&](CompanionMonitor& m) { m.onHeartbeat(gMillis, 1); });
    CHECK(loopTick() == CompanionAction::None);
    if (gQueue.empty() || gQueue.back().name != "mirror-stop") runCmdExec();
    CHECK(++guard < 2000);
  }
  CHECK(gMirror.running);
  // The companion fails again before the executor gets to the stop.
  withMonitor([&](CompanionMonitor& m) { m.onTransportFailure(gMillis); });
  CHECK(loopTick() == CompanionAction::Recover);
  runCmdExec();  // stop (skipped: armed again), start (already running), recovery
  CHECK(gMirror.running && gMirror.stops == 0 && gMirror.starts == 1);
}

static void a_full_command_queue_delays_nothing_for_good() {
  factoryNew();
  radioCompanionBootInit();
  runHealthy(60000);
  gQueueFull = true;
  gSubmitOpOk = false;  // submitOp reports the same full queue
  withMonitor([&](CompanionMonitor& m) { m.onTransportFailure(gMillis); });
  CHECK(loopTick() == CompanionAction::Recover);  // the recovery could not be queued either
  CHECK(gTrace.empty());
  gMillis += 1000;
  loopTick();
  CHECK(gTrace.empty() && gSettings.c6OutageCount == 1);
  gQueueFull = false;
  gMillis += 1000;
  loopTick();
  runCmdExec();
  CHECK(gMirror.running && gMirror.starts == 1);
  CHECK(gPersists == 1);  // the save was still owed
}

static void a_board_without_the_console_stays_silent() {
  factoryNew();
  gMirror.supported = false;
  radioCompanionBootInit();
  runHealthy(60000);
  outageAndRecovery();
  runHealthy(20 * 60 * 1000);
  CHECK(gMirror.starts == 0 && !logged("console mirror"));
  CHECK(std::strcmp(mirrorReason(), "unsupported") == 0);
  CHECK(std::strstr(mirrorText(), "not available on this board") != nullptr);
  CHECK(gSettings.c6OutageCount == 1 && gPersists == 1);  // the counts do not depend on the mirror
  int mirrorJobs = 0;
  for (const std::string& name : gTrace) mirrorJobs += name == "mirror-start" || name == "mirror-stop";
  CHECK(mirrorJobs == 1);  // one no-op start for the arming, no loop
}

static void a_save_that_fails_is_left_to_the_settings_writer() {
  factoryNew();
  radioCompanionBootInit();
  runHealthy(60000);
  gPersistResult = false;
  outageAndRecovery();
  runHealthy(60000);
  CHECK(gPersists == 1);  // asked once; the writer's own retry path owns the rest
  CHECK(logCount("companion fault counts not saved yet") == 1);
}

static void a_fault_without_a_clock_does_not_keep_an_earlier_faults_time() {
  factoryNew();
  radioCompanionBootInit();
  runHealthy(60000);
  const time_t firstAt = Clock::gNow;
  outageAndRecovery();
  CHECK(gSettings.c6OutageCount == 1 && gSettings.c6LastFaultEpochSec == static_cast<uint32_t>(firstAt));
  runHealthy(60000);
  // The clock is lost (a cold boot before NTP or the phone has set it reads
  // seconds of uptime). The next fault counts, and its time is "not
  // recorded": the first fault's time would be shown as the last fault's.
  Clock::gNow = 37;
  outageAndRecovery();
  CHECK(gSettings.c6OutageCount == 2 && gSettings.c6LastFaultEpochSec == 0);
  runHealthy(60000);
  // A later fault with a clock records its own time again.
  Clock::gNow = 1763650000;
  const time_t thirdAt = Clock::gNow;
  outageAndRecovery();
  CHECK(gSettings.c6OutageCount == 3 && gSettings.c6LastFaultEpochSec == static_cast<uint32_t>(thirdAt));
}

int main() {
  a_clean_boot_starts_no_mirror_and_saves_nothing();
  a_saved_c6console_on_starts_it_at_boot_and_keeps_it();
  an_outage_starts_the_mirror_before_the_recovery_and_counts_once();
  a_failed_mirror_start_is_tried_once_per_arming();
  the_reboot_escalation_counts_at_the_next_boot();
  a_companion_that_does_not_come_up_at_boot_counts_and_mirrors();
  a_boot_retry_that_worked_still_earns_the_mirror_its_ten_minutes();
  c6console_off_ends_a_fault_mirror_and_on_keeps_it();
  a_fault_between_the_stop_request_and_the_stop_keeps_the_mirror();
  a_full_command_queue_delays_nothing_for_good();
  a_board_without_the_console_stays_silent();
  a_save_that_fails_is_left_to_the_settings_writer();
  a_fault_without_a_clock_does_not_keep_an_earlier_faults_time();
  std::puts("companion fault mirror: all scenarios passed");
  return 0;
}
