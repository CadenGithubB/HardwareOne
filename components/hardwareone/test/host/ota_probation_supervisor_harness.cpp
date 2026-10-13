// Host harness for the OTA probation supervisor in System_OTASafety.cpp.
//
// The phase enum, timing constants, atomics, elapsedTicks, probationSupervisor,
// otaSafetyAcceptProvisioningBoot, otaSafetySetupReachedRunning and
// otaSafetyLoopHeartbeat are inserted unchanged by test_ota_probation_supervisor.py.
// The FreeRTOS tick is a settable fake (1 tick = 1 ms). vTaskDelay ends one
// supervisor pass by unwinding, so each call to supervisorPass() runs exactly one
// iteration of the production loop. rebootPendingImage records the cause and
// unwinds instead of restarting.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

using TickType_t = uint32_t;
using esp_err_t = int;
#define ESP_OK 0
#define ESP_FAIL -1
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))

template <typename... Args>
static void hostLog(const char*, const char*, Args...) {}
#define ESP_LOGE(...) hostLog(__VA_ARGS__)
#define ESP_LOGW(...) hostLog(__VA_ARGS__)
#define ESP_LOGI(...) hostLog(__VA_ARGS__)

static const char* esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

// INSERT_CAUSE_ENUM

static unsigned assertions = 0;
static void check(bool condition, const char* description) {
  ++assertions;
  if (!condition) throw std::runtime_error(description);
}

static TickType_t gNow = 0;
static TickType_t xTaskGetTickCount() { return gNow; }

struct SupervisorYield {};
struct SupervisorExit {};
struct Rebooted {};
static void vTaskDelay(TickType_t) { throw SupervisorYield{}; }
static void vTaskDelete(void*) { throw SupervisorExit{}; }

static unsigned gReboots = 0;
static OtaProbationCause gRebootCause = OTA_PROBATION_NONE;
[[noreturn]] static void rebootPendingImage(OtaProbationCause cause, const char*) {
  ++gReboots;
  gRebootCause = cause;
  throw Rebooted{};
}

namespace {
constexpr char kTag[] = "OTA-SAFETY";
// INSERT_STATE
}  // namespace

// INSERT_ELAPSED

void probationSupervisor(void*);

// Runs one supervisor iteration at the current fake tick. A reboot is recorded
// (gReboots/gRebootCause) and absorbed, so a caller in the middle of an accept
// keeps running and the scenario can assert on what the supervisor decided.
static void supervisorPass() {
  try {
    probationSupervisor(nullptr);
  } catch (const SupervisorYield&) {
  } catch (const SupervisorExit&) {
  } catch (const Rebooted&) {
  }
}

// Platform hooks the accept and mark paths call. Each can advance the clock and
// run a supervisor pass inside the window, which is where a real supervisor on
// the other core can wake.
static TickType_t gJournalCheckTicks = 20;
static TickType_t gMarkTicks = 40;
static bool gJournalCoherent = true;
static esp_err_t gMarkResult = ESP_OK;
static unsigned gMarkCalls = 0;
static unsigned gMarkedValidCalls = 0;
static bool gMarkedValidByShortcut = false;

static bool otaSystemCanMarkImageValid() {
  supervisorPass();
  gNow += gJournalCheckTicks;
  supervisorPass();
  return gJournalCoherent;
}

static esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
  ++gMarkCalls;
  supervisorPass();
  gNow += gMarkTicks;
  supervisorPass();
  return gMarkResult;
}

static void otaSystemOnImageMarkedValid(bool provisioningShortcut) {
  ++gMarkedValidCalls;
  gMarkedValidByShortcut = provisioningShortcut;
}

// INSERT_FUNCTIONS

// Same steps otaSafetyInitEarly() takes for an unverified image, minus the
// task creation: the boot tick and the Setup phase.
static void bootUnverifiedImage(TickType_t bootTick) {
  gNow = bootTick;
  sPhase.store(ProbationPhase::Inactive);
  sBootStartedTick.store(0);
  sRunningStartedTick.store(0);
  sHealthyStartedTick.store(0);
  sLastLoopHeartbeatTick.store(0);
  sLastMarkAttemptTick.store(0);
  sHealthyLoopCount.store(0);
  gReboots = 0;
  gRebootCause = OTA_PROBATION_NONE;
  gJournalCheckTicks = 20;
  gMarkTicks = 40;
  gJournalCoherent = true;
  gMarkResult = ESP_OK;
  gMarkCalls = 0;
  gMarkedValidCalls = 0;
  gMarkedValidByShortcut = false;
  sBootStartedTick.store(gNow);
  sPhase.store(ProbationPhase::Setup);
}

static void scenarioProvisioningAcceptAfterLongSetup(TickType_t acceptAfterMs) {
  bootUnverifiedImage(1000);
  gNow += acceptAfterMs;
  supervisorPass();
  check(gReboots == 0, "Setup within the 5-minute deadline does not reboot");
  check(otaSafetyAcceptProvisioningBoot(), "the provisioning accept succeeds");
  check(gReboots == 0, "no supervisor pass during the journal check or the otadata write reboots");
  check(gRebootCause == OTA_PROBATION_NONE, "no probation cause is recorded");
  check(gMarkCalls == 1, "the image is marked valid once");
  check(gMarkedValidCalls == 1 && gMarkedValidByShortcut,
        "the journal hears of a provisioning-shortcut acceptance");
  check(sPhase.load() == ProbationPhase::Validated, "the probation ends Validated");
  check(!otaSafetyIsPendingVerification(), "the image is no longer pending");
  supervisorPass();
  check(gReboots == 0, "the supervisor exits after validation");
}

static void scenarioStallDuringAcceptStillReboots() {
  // Seeding the ticks must not switch the hang check off: a journal check that
  // itself hangs for 30 s is still a stall.
  bootUnverifiedImage(0);
  gNow = 45000;
  gJournalCheckTicks = 31000;
  check(otaSafetyAcceptProvisioningBoot(), "the accept returns once the stalled check completes");
  check(gReboots >= 1 && gRebootCause == OTA_PROBATION_LOOP_STALL,
        "a 31-second hang inside the accept is diagnosed as a loop stall");
}

static void scenarioMarkFailureReturnsToSetup() {
  bootUnverifiedImage(0);
  gNow = 60000;
  gMarkResult = ESP_FAIL;
  check(!otaSafetyAcceptProvisioningBoot(), "a failed otadata write is reported");
  check(gReboots == 0, "no reboot while the mark is attempted");
  check(sPhase.load() == ProbationPhase::Setup, "the probation returns to Setup");
  check(otaSafetyIsPendingVerification(), "the image stays pending");
  gNow = 5 * 60 * 1000;
  supervisorPass();
  check(gReboots == 1 && gRebootCause == OTA_PROBATION_SETUP_TIMEOUT,
        "the setup deadline still applies after a failed provisioning accept");
}

static void scenarioRunningStallStillReboots() {
  bootUnverifiedImage(0);
  gNow = 10000;
  otaSafetySetupReachedRunning();
  check(sPhase.load() == ProbationPhase::Running, "setup reached RUNNING");
  otaSafetyLoopHeartbeat(true);
  gNow += 29000;
  supervisorPass();
  check(gReboots == 0, "a 29-second gap is not yet a stall");
  gNow += 1500;
  supervisorPass();
  check(gReboots == 1 && gRebootCause == OTA_PROBATION_LOOP_STALL,
        "a 30-second gap in Running is a loop stall");
}

static void scenarioHealthyProbationMarksValid() {
  bootUnverifiedImage(0);
  gNow = 8000;
  otaSafetySetupReachedRunning();
  for (int i = 0; i < 700 && sPhase.load() != ProbationPhase::Validated; ++i) {
    gNow += 100;
    otaSafetyLoopHeartbeat(true);
    if (i % 5 == 0) supervisorPass();
  }
  check(gReboots == 0, "a healthy probation never reboots");
  check(sPhase.load() == ProbationPhase::Validated, "a healthy minute marks the image valid");
  check(gMarkedValidCalls == 1 && !gMarkedValidByShortcut,
        "the journal hears of a full-probation acceptance");
}

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  try {
    if (scenario == "accept_after_45s") {
      scenarioProvisioningAcceptAfterLongSetup(45000);
    } else if (scenario == "accept_after_290s") {
      scenarioProvisioningAcceptAfterLongSetup(290000);
    } else if (scenario == "accept_early") {
      scenarioProvisioningAcceptAfterLongSetup(3000);
    } else if (scenario == "stall_during_accept") {
      scenarioStallDuringAcceptStillReboots();
    } else if (scenario == "mark_failure") {
      scenarioMarkFailureReturnsToSetup();
    } else if (scenario == "running_stall") {
      scenarioRunningStallStillReboots();
    } else if (scenario == "healthy_probation") {
      scenarioHealthyProbationMarksValid();
    } else {
      std::fprintf(stderr, "unknown scenario '%s'\n", scenario.c_str());
      return 2;
    }
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "FAIL %s: %s (reboots=%u cause=%u)\n", scenario.c_str(), failure.what(),
                 gReboots, static_cast<unsigned>(gRebootCause));
    return 1;
  }
  std::printf("%s ok (%u checks)\n", scenario.c_str(), assertions);
  return 0;
}
