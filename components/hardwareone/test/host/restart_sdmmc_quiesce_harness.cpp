// Host harness for the SDMMC quiesce before an intentional restart and for the
// companion start's wait for the slave. The runner
// (test_restart_sdmmc_quiesce.py) inserts the shipping definitions unchanged:
//   @UTILS@      quiesceSdmmcForRestart, rebootDevice, factoryreset_doRestart (System_Utils.cpp)
//   @SILENCE@    radioHalSilenceForRestart (HAL_Radio.cpp)
//   @START@      hostedLinkUp, startOnce and their constants (HAL_Radio.cpp)
//   @COMPANION@  radioCompanionBeforeRestart, submitOp (System_RadioCompanion.cpp)
// and the real System_RestartQuiesceCore.h / Radio_CompanionCore.h. Only the
// ESP-IDF, Arduino, FreeRTOS and Hosted boundaries are fakes: they record what
// was called, in order, on a fake millisecond clock. ESP.restart() unwinds.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../System_RestartQuiesceCore.h"
#include "../../Radio_CompanionCore.h"

// --- shared fake world ---------------------------------------------------------
static uint32_t gNow = 0;
static std::vector<std::string> gEvents;
static void note(const std::string& e) { gEvents.push_back(e); }
static void advance(uint32_t ms);  // also delivers a pending fake TRANSPORT_UP

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      dump();                                                                \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static void dump() {
  std::fprintf(stderr, "events:");
  for (const auto& e : gEvents) std::fprintf(stderr, " [%s]", e.c_str());
  std::fprintf(stderr, "\n");
}

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_WIFI_NOT_INIT 0x3001
static const char* esp_err_to_name(esp_err_t) { return "err"; }
#define ESP_LOGI(tag, fmt, ...) std::fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) std::fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) std::fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)

// --- System_Utils.cpp boundaries ------------------------------------------------
#define HW1_RADIO_COMPANION 1
#define ENABLE_SD_CARD 1
#define ENABLE_SDMMC_CARD 1
struct RestartSignal {};
struct EspClass {
  [[noreturn]] void restart() {
    note("restart");
    throw RestartSignal{};
  }
} ESP;
static unsigned long millis() { return gNow; }
static void delay(uint32_t ms) {
  note("delay " + std::to_string(ms));
  advance(ms);
}
static bool finalizeMicRecordingForRestart(const char*) {
  note("mic");
  return true;
}
static void recordRebootIntent(const char* reason, const char*) { note(std::string("intent ") + reason); }
static void radioCompanionBeforeRestart() { note("companion-silence"); }
static uint32_t gFsLockHoldMs = 0;  // how long another task keeps the filesystem lock
static bool fsLockForRestart(uint32_t timeoutMs) {
  note("fslock " + std::to_string(timeoutMs));
  const bool got = gFsLockHoldMs < timeoutMs;
  gNow += got ? gFsLockHoldMs : timeoutMs;
  return got;
}

namespace utils {
/*@UTILS@*/
}  // namespace utils

// --- HAL_Radio.cpp boundaries --------------------------------------------------
typedef void (*shutdown_handler_t)(void);
typedef int gpio_num_t;
enum { GPIO_MODE_OUTPUT = 2 };
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(ms) (ms)
static int gLock;
static esp_err_t esp_wifi_stop(void) { return ESP_OK; }
static esp_err_t esp_unregister_shutdown_handler(shutdown_handler_t h) {
  note(h == reinterpret_cast<shutdown_handler_t>(&esp_wifi_stop) ? "unregister esp_wifi_stop"
                                                                   : "unregister other");
  return ESP_OK;
}
static esp_err_t gpio_hold_dis(gpio_num_t pin) {
  note("hold_dis " + std::to_string(pin));
  return ESP_OK;
}
static esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
  note("level " + std::to_string(pin) + "=" + std::to_string(level));
  return ESP_OK;
}
static esp_err_t gpio_set_direction(gpio_num_t pin, int mode) {
  note("dir " + std::to_string(pin) + (mode == GPIO_MODE_OUTPUT ? " out" : " other"));
  return ESP_OK;
}
static void vTaskDelay(uint32_t ticks) { advance(ticks); }

// Hosted, as 2.12.13 behaves after its retry counter is spent: hostedInitWiFi()
// returns at once; the rx task finishes the handshake later and posts
// TRANSPORT_UP (here: at gUpAt). Every RPC first checks the link
// (check_transport_up) and fails at once while it is not up.
static bool gHostedInitialized = false;
static uint32_t gInitReturnsAfterMs = 5;
static uint32_t gUpAt = 0;  // 0: the slave never answers
static int gRpcCalls = 0;
static uint32_t gFirstRpcAt = 0;

struct esp_hosted_coprocessor_fwver_t {
  uint32_t major1, minor1, patch1;
};
#define HW1_C6_HOSTED_VERSION_MAJOR 2
#define HW1_C6_HOSTED_VERSION_MINOR 12
#define HW1_C6_HOSTED_VERSION_PATCH 13
enum { WIFI_IF_STA = 0 };
static esp_err_t esp_wifi_get_mac(int, uint8_t*) { return ESP_FAIL; }

namespace board {
constexpr gpio_num_t kC6Enable = 9;
}
struct Identity {
  bool versionKnown;
  uint32_t major, minor, patch;
  bool versionMatches;
};

namespace hal {
constexpr const char* kTag = "HW1_RADIO";
int& sLock = gLock;
bool sTransportUp = false;
bool sTransportLost = false;
bool sHeld = false;
bool sUpdateOnly = false;
bool sHostedLinkUp = false;
uint8_t sSessionBleLinks = 0;
Identity sIdentity = {};
uint32_t nowMs() { return gNow; }
bool hostedIsInitialized() { return gHostedInitialized; }
bool hostedInitWiFi() {
  note("hostedInitWiFi");
  if (!gHostedInitialized) gNow += gInitReturnsAfterMs;
  gHostedInitialized = true;
  return true;
}
esp_err_t esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t* v) {
  if (gRpcCalls++ == 0) gFirstRpcAt = gNow;
  note("version-rpc");
  if (!sHostedLinkUp) return ESP_FAIL;  // "ESP-Hosted link not yet up"
  v->major1 = 2;
  v->minor1 = 12;
  v->patch1 = 13;
  return ESP_OK;
}

/*@START@*/

/*@SILENCE@*/
}  // namespace hal

static void advance(uint32_t ms) {
  gNow += ms;
  if (gUpAt && gNow >= gUpAt && !hal::sHostedLinkUp) {
    hal::sHostedLinkUp = true;  // hostedEventHandler(TRANSPORT_UP)
    hal::sTransportUp = true;
    note("TRANSPORT_UP");
  }
}

// --- System_RadioCompanion.cpp boundaries ----------------------------------------
namespace comp {
enum class Op : uint8_t { None = 0, Start, Recover, Hold, Release, Update };
struct ExecReq {
  typedef void (*DeferredFn)(void*);
};
std::atomic<bool> sOpInFlight{false};
std::atomic<uint8_t> sOp{0};
std::atomic<bool> sRestarting{false};
static void radioHalSilenceForRestart() { note("hal-silence"); }
static bool submitDeferredToCmdExec(ExecReq::DeferredFn, void*) {
  note("submitted");
  return true;
}
static void finishOp() {
  sOp.store(0);
  sOpInFlight.store(false);
}
/*@COMPANION@*/
}  // namespace comp

// --- scenarios ---------------------------------------------------------------
static void reset_world() {
  gNow = 1000;
  gEvents.clear();
  gFsLockHoldMs = 0;
  gHostedInitialized = false;
  gInitReturnsAfterMs = 5;
  gUpAt = 0;
  gRpcCalls = 0;
  gFirstRpcAt = 0;
  hal::sTransportUp = false;
  hal::sHostedLinkUp = false;
  hal::sHeld = false;
}

static void reboot_quiesces_sdmmc_after_the_flush_and_before_the_restart() {
  reset_world();
  bool restarted = false;
  try {
    utils::rebootDevice("command", "commanded restart", 1000);
  } catch (const RestartSignal&) {
    restarted = true;
  }
  CHECK(restarted);
  const std::vector<std::string> want = {
      "mic", "intent command", "delay 1000",  // the reply and the REBOOT line still go out
      "companion-silence",                    // the C6 stops sending
      "fslock 2000",                          // no SD transfer starts or runs
      "delay 1100",                           // a cut-off transfer times out first
      "restart"};
  CHECK(gEvents == want);
}

static void reboot_with_a_busy_filesystem_still_restarts_without_extra_wait() {
  reset_world();
  gFsLockHoldMs = 5000;  // a writer stuck behind the dead link
  bool restarted = false;
  try {
    utils::rebootDevice("c6-companion", "outage", 750);
  } catch (const RestartSignal&) {
    restarted = true;
  }
  CHECK(restarted);
  const std::vector<std::string> want = {"mic", "intent c6-companion", "delay 750", "companion-silence",
                                         "fslock 2000", "restart"};
  CHECK(gEvents == want);
}

static void factory_reset_restart_quiesces_too() {
  reset_world();
  bool restarted = false;
  try {
    utils::factoryreset_doRestart(nullptr);
  } catch (const RestartSignal&) {
    restarted = true;
  }
  CHECK(restarted);
  const std::vector<std::string> want = {"companion-silence", "fslock 2000", "delay 1100", "restart"};
  CHECK(gEvents == want);
}

static void hal_silence_unregisters_the_wifi_stop_rpc_and_holds_the_c6() {
  reset_world();
  hal::sTransportUp = true;
  hal::sHostedLinkUp = true;
  hal::radioHalSilenceForRestart();
  const std::vector<std::string> want = {"unregister esp_wifi_stop", "hold_dis 9", "level 9=0", "dir 9 out"};
  CHECK(gEvents == want);
  CHECK(hal::sHeld && !hal::sTransportUp && !hal::sHostedLinkUp);
}

static void companion_silence_stops_new_work_then_silences_the_hal() {
  reset_world();
  comp::sRestarting.store(false);
  CHECK(comp::submitOp(comp::Op::Start, nullptr, nullptr));
  comp::finishOp();
  gEvents.clear();
  comp::radioCompanionBeforeRestart();
  CHECK(comp::sRestarting.load());
  CHECK((gEvents == std::vector<std::string>{"hal-silence"}));
  CHECK(!comp::submitOp(comp::Op::Recover, nullptr, nullptr));  // no recovery re-releases the C6
  CHECK(!comp::sOpInFlight.load());
  CHECK((gEvents == std::vector<std::string>{"hal-silence"}));
}

static void a_retry_waits_for_the_slave_when_hosted_gives_up_at_once() {
  reset_world();
  const uint32_t start = gNow;
  gUpAt = start + 420;  // the C6 opens its data path 420 ms after the start
  CHECK(hal::startOnce(false) == ESP_OK);
  CHECK(gFirstRpcAt >= gUpAt);
  CHECK(hal::sTransportUp);
  CHECK(gNow - start < 1000);
}

static void a_stale_link_flag_from_the_last_session_is_not_trusted() {
  reset_world();
  hal::sHostedLinkUp = true;  // left over from the session the retry tore down
  const uint32_t start = gNow;
  gUpAt = start + 300;
  CHECK(hal::startOnce(false) == ESP_OK);
  CHECK(gFirstRpcAt >= gUpAt);
}

static void a_slave_that_never_answers_fails_inside_hosteds_budget() {
  reset_world();
  const uint32_t start = gNow;
  CHECK(hal::startOnce(false) != ESP_OK);
  CHECK(gNow - start == hal::kSlaveReadyBudgetMs);
  CHECK(gRpcCalls == 1);
}

static void an_initialized_live_session_does_not_wait() {
  reset_world();
  gHostedInitialized = true;  // another claim kept Hosted up
  hal::sHostedLinkUp = true;
  const uint32_t start = gNow;
  CHECK(hal::startOnce(false) == ESP_OK);
  CHECK(gNow == start);
}

int main() {
  reboot_quiesces_sdmmc_after_the_flush_and_before_the_restart();
  reboot_with_a_busy_filesystem_still_restarts_without_extra_wait();
  factory_reset_restart_quiesces_too();
  hal_silence_unregisters_the_wifi_stop_rpc_and_holds_the_c6();
  companion_silence_stops_new_work_then_silences_the_hal();
  a_retry_waits_for_the_slave_when_hosted_gives_up_at_once();
  a_stale_link_flag_from_the_last_session_is_not_trusted();
  a_slave_that_never_answers_fails_inside_hosteds_budget();
  an_initialized_live_session_does_not_wait();
  std::puts("restart SDMMC quiesce and companion start wait: all scenarios passed");
  return 0;
}
