// Host harness for the SDMMC quiesce before an intentional restart and for the
// companion start's wait for the slave. The runner
// (test_restart_sdmmc_quiesce.py) inserts the shipping definitions unchanged:
//   @MUTEX@      isHeldByCurrentTask, fsLockForRestart (System_Mutex.cpp)
//   @UTILS@      quiesceSdmmcForRestart, rebootDevice, factoryreset_doRestart (System_Utils.cpp)
//   @EVENTS@     deliver, hostedEventHandler, registerEvents (HAL_Radio.cpp)
//   @START@      hostedLinkUp, startOnce and their constants (HAL_Radio.cpp)
//   @SILENCE@    kHostedTxFlushMs, restartLatched, latchC6EnableLow,
//                radioHalHold, radioHalSilenceForRestart (HAL_Radio.cpp)
//   @PREPARE@    radioHalStopTransport, radioHalPrepare (HAL_Radio.cpp)
//   @USERS_TYPE@ struct RadioUsers (System_RadioCompanion.cpp)
//   @USERS@      kHttpRestoreWindowMs, snapshotUsers, stopUsers, the HTTP
//                restore (armHttpRestore, restartHttpServer, httpRestoreProc,
//                httpRestoreTick), restoreUsers, recoverProc, holdProc,
//                releaseProc (System_RadioCompanion.cpp)
//   @COMPANION@  submitOp, radioCompanionBeforeRestart, radioCompanionTick,
//                radioCompanionBlocksRadio (System_RadioCompanion.cpp)
// and the real System_RestartQuiesceCore.h / Radio_CompanionCore.h. Only the
// ESP-IDF, Arduino, FreeRTOS and Hosted boundaries are fakes: they record what
// was called, in order, on a fake millisecond clock. ESP.restart() unwinds.
// A fake TRANSPORT_UP goes through the real hostedEventHandler. Hosted's TX
// path is the transport state, HardwareOne's fence (the Hosted patch) and a
// queue: the write task sends one queued packet per millisecond as time
// passes, and a CMD53 to a C6 whose EN pad is low times out (0x107). Hosted's
// transport_drv_reconfigure (Wi-Fi init, BLE open) brings the slave up again
// (reset pulse, card init) whenever the state is below TX_ACTIVE. The
// companion's radio users (Wi-Fi, HTTP, ESP-NOW, BLE) are a small fake network
// whose `radiopower off`/`on` do what System_WiFi.cpp's do to the web server.
#include <atomic>
#include <cinttypes>
#include <cstdarg>
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
#define ESP_RETURN_ON_ERROR(x, tag, msg) \
  do {                                   \
    const esp_err_t err_ = (x);          \
    if (err_ != ESP_OK) return err_;     \
  } while (0)
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))

// HAL_Radio.h types
enum class RadioHalEvent : uint8_t { TransportUp = 0, TransportDown, TransportFailure, Heartbeat, CompanionBoot, Memory };
struct RadioHalEventData {
  RadioHalEvent type;
  uint32_t value;
  uint32_t value2;
};
typedef void (*RadioHalEventFn)(const RadioHalEventData& event);
struct RadioCompanionMemory {
  bool known;
  uint32_t totalHeap, freeHeap, minFreeHeap, internalFree, internalLargest, atMs;
};

// --- System_Mutex.cpp boundaries (a FreeRTOS mutex) ------------------------------
#define configUSE_MUTEXES 1
#define pdTRUE 1
#define pdFALSE 0
typedef uint32_t TickType_t;
struct FakeMutex {};
typedef FakeMutex* SemaphoreHandle_t;
static FakeMutex gFsMutexObj;
static SemaphoreHandle_t gFsMutex = &gFsMutexObj;
static int gThisTask = 0, gOtherTask = 0;  // task handles
static void* gFsHolder = nullptr;          // the task holding the filesystem lock
static uint32_t gFsLockHoldMs = 0;         // how long another holder keeps it
static void* xTaskGetCurrentTaskHandle() { return &gThisTask; }
static void* xSemaphoreGetMutexHolder(SemaphoreHandle_t) { return gFsHolder; }
static int xSemaphoreTake(SemaphoreHandle_t, TickType_t ticks) {
  note("fslock " + std::to_string(ticks));
  if (gFsHolder && gFsHolder != &gThisTask) {
    if (gFsLockHoldMs >= ticks) {
      gNow += ticks;
      return pdFALSE;
    }
    gNow += gFsLockHoldMs;
  }
  gFsHolder = &gThisTask;
  return pdTRUE;
}

/*@MUTEX@*/

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
static void (*gDuringDelay)() = nullptr;  // work another task does while rebootDevice waits
static void delay(uint32_t ms) {
  note("delay " + std::to_string(ms));
  if (gDuringDelay) gDuringDelay();
  advance(ms);
}
static bool finalizeMicRecordingForRestart(const char*) {
  note("mic");
  return true;
}
static void recordRebootIntent(const char* reason, const char*) { note(std::string("intent ") + reason); }
static void (*gCompanionSilence)() = nullptr;  // set: run the real HAL silence too
static void radioCompanionBeforeRestart() {
  note("companion-silence");
  if (gCompanionSilence) gCompanionSilence();
}
// Packets the fake producers dropped at the fence (hostedTx); the quiesce line
// reports the count (Hosted patch: esp_hosted_hw1_tx_fence_drops).
static uint32_t gFenceDrops = 0;
static uint32_t radioHalRestartTxDrops() { return gFenceDrops; }
namespace utils {
/*@UTILS@*/
}  // namespace utils

// --- HAL_Radio.cpp boundaries --------------------------------------------------
typedef void (*shutdown_handler_t)(void);
typedef int gpio_num_t;
enum { GPIO_MODE_OUTPUT = 2 };
static int gLock;
static esp_err_t esp_wifi_stop(void) { return ESP_OK; }
static esp_err_t esp_unregister_shutdown_handler(shutdown_handler_t h) {
  note(h == reinterpret_cast<shutdown_handler_t>(&esp_wifi_stop) ? "unregister esp_wifi_stop"
                                                                   : "unregister other");
  return ESP_OK;
}
// The C6 EN pad: gpio_set_level writes the output register, and the pad
// follows it unless gpio_hold_en latched it (ESP-IDF pad hold semantics).
static uint32_t gEnOut = 1, gEnPad = 1;
static uint32_t gEnLowAt = 0;  // the last write of EN low
static bool gEnPadHeld = false;
static void (*gBeforeHoldDis)() = nullptr;  // one-shot: another task runs first
static esp_err_t gpio_hold_dis(gpio_num_t pin) {
  if (gBeforeHoldDis) {
    void (*hook)() = gBeforeHoldDis;
    gBeforeHoldDis = nullptr;
    hook();
  }
  note("hold_dis " + std::to_string(pin));
  gEnPadHeld = false;
  gEnPad = gEnOut;
  return ESP_OK;
}
static esp_err_t gpio_hold_en(gpio_num_t pin) {
  note("hold_en " + std::to_string(pin));
  gEnPadHeld = true;
  return ESP_OK;
}
static esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
  note("level " + std::to_string(pin) + "=" + std::to_string(level));
  if (level == 0) gEnLowAt = gNow;
  gEnOut = level;
  if (!gEnPadHeld) gEnPad = level;
  return ESP_OK;
}
// Hosted's transport_gpio_reset (sdio_drv.c), from an op that was already
// inside hostedInitWiFi when the restart began: EN active, inactive, active.
static void hostedResetPulse() {
  gpio_set_level(9, 1);
  gpio_set_level(9, 0);
  gpio_set_level(9, 1);
}
static esp_err_t gpio_set_direction(gpio_num_t pin, int mode) {
  note("dir " + std::to_string(pin) + (mode == GPIO_MODE_OUTPUT ? " out" : " other"));
  return ESP_OK;
}
static void (*gDuringTaskDelay)() = nullptr;  // work another task does in a vTaskDelay
static void vTaskDelay(uint32_t ticks) {
  if (gDuringTaskDelay) gDuringTaskDelay();
  advance(ticks);
}

// ESP-Hosted's transport state and HardwareOne's TX fence (transport_drv.c, the
// Hosted patch) and its to_slave queues (sdio_drv.c). esp_hosted_tx and the
// STA/AP/serial wrappers refuse a packet unless esp_hosted_hw1_tx_open(): the
// state is TX_ACTIVE (2) and the fence is not set. The write task (advance)
// sends one queued packet per kFakeTxPacketMs.
static uint8_t gHostedTransportState = 0;
static bool gTxFenced = false;
static std::vector<std::string> gTxQueue;
static void (*gBeforeTxGate)() = nullptr;  // one-shot: a producer already past the check
static uint32_t gTxGateAt = 0;
// A full queue: MAX_PRIORITY_QUEUES (3) x the SDIO TX queue size (20).
constexpr int kHostedTxQueueDepth = 3 * 20;
// Per packet: at most 1536 bytes, about 0.15 ms at 20 MHz 4-bit; 1 ms is the
// harness's generous bound for a slave that has RX buffers.
constexpr uint32_t kFakeTxPacketMs = 1;
static void closeTxGate(const std::string& how) {
  if (gBeforeTxGate) {
    void (*hook)() = gBeforeTxGate;
    gBeforeTxGate = nullptr;
    hook();
  }
  note(how);
  gTxGateAt = gNow;
}
extern "C" uint8_t is_transport_tx_ready(void) { return gHostedTransportState >= 2; }
extern "C" void esp_hosted_hw1_fence_tx(void) {
  closeTxGate("hosted-tx fence");
  gTxFenced = true;
}
// Hosted's own state setter, which the silence must not use (it reopens the
// slave bring-up in transport_drv_reconfigure); kept so that variant compiles.
extern "C" void set_transport_state(uint8_t state) {
  closeTxGate("hosted-tx state " + std::to_string(state));
  gHostedTransportState = state;
}
static void queueTx(const std::string& what) { gTxQueue.push_back(what); }
static void hostedTx(const std::string& what) {  // a producer (lwIP, HCI, RPC)
  if (!is_transport_tx_ready() || gTxFenced) {
    if (gTxFenced) ++gFenceDrops;
    note("tx refused " + what);
    return;
  }
  queueTx(what);
}
// transport_drv_reconfigure (transport_drv.c): esp_wifi_remote_init and the
// BLE VHCI open call it. Below TX_ACTIVE it runs ensure_slave_bus_ready: a C6
// reset pulse (transport_gpio_reset) and SDIO card init on slot 1.
static void hostedReconfigure(const char* who) {
  if (is_transport_tx_ready()) {
    note(std::string("reconfigure ") + who + ": already up");
    return;
  }
  note(std::string("reconfigure ") + who + ": slave reset + card init");
  gpio_set_level(9, 0);
  gpio_set_level(9, 1);
}

// Hosted 2.12.13: a fresh hostedInitWiFi() resets the C6 and returns when
// Hosted's slave-ready wait ends (gInitReturnsAfterMs: its whole 20 s budget
// on a fresh retry counter with a silent C6, about 1.7 s after the counter is
// spent); a call on an initialised session returns at once. The rx task may
// finish the handshake later and post TRANSPORT_UP (here: at gUpAt), through
// the real hostedEventHandler. Every RPC first checks the link
// (check_transport_up) and fails at once while it is not up.
static bool gHostedInitialized = false;
static uint32_t gInitReturnsAfterMs = 5;
static uint32_t gUpAt = 0;  // 0: the slave never answers
static bool gUpPosted = false;
static int gResets = 0;                  // C6 reset pulses from a fresh hostedInitWiFi
static void (*gOnReset)(int n) = nullptr;  // called at the n-th reset
static int gRpcCalls = 0;
static uint32_t gFirstRpcAt = 0;

// esp_event / esp_hosted event boundaries
typedef const char* esp_event_base_t;
static const char kHostedEventBase[] = "ESP_HOSTED_EVENT";
static const char kOtherEventBase[] = "WIFI_EVENT";
static esp_event_base_t const ESP_HOSTED_EVENT = kHostedEventBase;
#define ESP_EVENT_ANY_ID -1
enum {
  ESP_HOSTED_EVENT_CP_INIT = 0,
  ESP_HOSTED_EVENT_CP_HEARTBEAT,
  ESP_HOSTED_EVENT_TRANSPORT_UP,
  ESP_HOSTED_EVENT_TRANSPORT_DOWN,
  ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
  ESP_HOSTED_EVENT_MEM_MONITOR,
};
struct esp_hosted_event_heartbeat_t {
  uint32_t heartbeat;
};
struct esp_hosted_event_init_t {
  int reason;
};
struct esp_hosted_mem_cap_t {
  uint32_t free_size, largest_free_block;
};
struct esp_hosted_mem_region_t {
  esp_hosted_mem_cap_t cap_8bit;
};
struct esp_hosted_event_mem_info_t {
  uint32_t curr_total_free_heap_size, curr_min_free_heap_size;
  esp_hosted_mem_region_t curr_internal;
};
typedef void (*esp_event_handler_t)(void*, esp_event_base_t, int32_t, void*);
static esp_event_handler_t gRegisteredHandler = nullptr;
static esp_err_t esp_event_loop_create_default() { return ESP_OK; }
static esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t h, void*) {
  if (base == ESP_HOSTED_EVENT && id == ESP_EVENT_ANY_ID) gRegisteredHandler = h;
  return ESP_OK;
}

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
bool sRestartLatched = false;
bool sUpdateOnly = false;
bool sHostedLinkUp = false;
bool sPrepared = false;
bool sEventsRegistered = false;
RadioHalEventFn sEventFn = nullptr;
RadioCompanionMemory sMemory = {};
uint8_t sSessionBleLinks = 0;
Identity sIdentity = {};
uint32_t nowMs() { return gNow; }
bool hostedIsInitialized() { return gHostedInitialized; }
esp_err_t prepareBoard() { return ESP_OK; }
void quietRpcLogs() {}
esp_err_t radioHalConsoleMirror(bool) { return ESP_OK; }
bool hostedInitWiFi() {
  note("hostedInitWiFi");
  if (!gHostedInitialized) {
    gHostedInitialized = true;
    note("c6-reset");  // Hosted's transport_gpio_reset
    if (gOnReset) gOnReset(++gResets);
    else ++gResets;
    advance(gInitReturnsAfterMs);
  }
  return true;
}
bool hostedIsBLEActive() { return false; }
void hostedReleaseBLE() { note("release-ble"); }
bool hostedDeinitWiFi() {
  note("hosted-deinit");
  gHostedInitialized = false;
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

/*@EVENTS@*/

/*@START@*/

// startTransport's own steps (identity, monitoring) are not under test here;
// its restart gate is a source check in the runner.
esp_err_t radioHalStartTransport() {
  note("start");
  return startOnce(false);
}

/*@SILENCE@*/

/*@PREPARE@*/
}  // namespace hal

static void advance(uint32_t ms) {
  // Hosted's write task runs as time passes, one packet per kFakeTxPacketMs:
  // each queued packet is a CMD53, answered by a running C6 and timed out
  // (0x107) by a held one.
  size_t sent = 0;
  while (sent < gTxQueue.size() && ms >= kFakeTxPacketMs) {
    note((gEnPad ? "tx sent " : "tx 0x107 ") + gTxQueue[sent++]);
    gNow += kFakeTxPacketMs;
    ms -= kFakeTxPacketMs;
  }
  gTxQueue.erase(gTxQueue.begin(), gTxQueue.begin() + static_cast<long>(sent));
  gNow += ms;
  if (gUpAt && gNow >= gUpAt && !gUpPosted) {
    gUpPosted = true;
    note("TRANSPORT_UP");
    hal::hostedEventHandler(nullptr, ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_UP, nullptr);
  }
}

// --- System_RadioCompanion.cpp boundaries ----------------------------------------
namespace comp {
using hw1::radio::CompanionAction;
using hw1::radio::CompanionMonitor;
using hw1::radio::CompanionState;
enum class Op : uint8_t { None = 0, Start, Recover, Hold, Release, Update };
struct ExecReq {
  typedef void (*DeferredFn)(void*);
};
int sLock;
CompanionMonitor sMonitor;
bool sBootInitDone = false;
std::atomic<bool> sOpInFlight{false};
std::atomic<uint8_t> sOp{0};
std::atomic<bool> sRestarting{false};
CompanionState gState = CompanionState::Online;  // what monitorState() reports
static CompanionState monitorState() { return gState; }
template <typename F>
void withMonitor(F fn) {
  fn(sMonitor);
}
// The silence records whether new companion work was already refused when it
// ran: a worker op submitted after the silence would start the C6 again.
static void radioHalSilenceForRestart() {
  note(std::string("hal-silence restarting=") + (sRestarting.load() ? "1" : "0"));
}
static esp_err_t radioHalHold(bool hold) {
  note(std::string("hal-hold ") + (hold ? "1" : "0"));
  return ESP_OK;
}
static bool gCompanionAnswers = true;  // the C6 comes back from a reset
static esp_err_t radioHalStartTransport() {
  note("hal-start");
  return gCompanionAnswers ? ESP_OK : ESP_FAIL;
}
static void applyPolicyFromSettings() {}
static void reportIdentity() {}
static bool popEvent(RadioHalEventData&) {
  note("pop");
  return false;
}
static void handleEvent(const RadioHalEventData&, uint32_t) {}
static void dispatch(CompanionAction, uint32_t) { note("dispatch"); }
static ExecReq::DeferredFn gDeferredFn = nullptr;  // the last job handed to cmd_exec
static void* gDeferredArg = nullptr;
static bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg) {
  note("submitted");
  gDeferredFn = fn;
  gDeferredArg = arg;
  return true;
}
static void runDeferred() {  // cmd_exec runs the job
  ExecReq::DeferredFn fn = gDeferredFn;
  gDeferredFn = nullptr;
  if (fn) fn(gDeferredArg);
}
static void finishOp() {
  sOp.store(0);
  sOpInFlight.store(false);
}
// The radio users. Every feature flag on, as in the P4X-EYE build.
#define ENABLE_WIFI 1
#define ENABLE_ESPNOW 1
#define ENABLE_BLUETOOTH 1
#define ENABLE_HTTP_SERVER 1
/*@USERS_TYPE@*/
RadioUsers sHeldUsers;

// The fake network. `radiopower off` (System_WiFi.cpp radioPowerOff) stops
// ESP-NOW, Wi-Fi and the HTTP server, the last through httpServerStopSafe:
// inline, or deferred to the main loop while a web request waits on cmd_exec
// (the handle stays set until then). `radiopower on` (radioPowerOn) restores
// ESP-NOW and Wi-Fi from its snapshot and nothing else. `openhttp`
// (cmd_httpstart) needs the Wi-Fi link.
struct FakeNet {
  bool wifiOn = false, linked = false, joinsOnPowerOn = true;
  bool http = false, httpStopPending = false, webWaiter = false;
  bool espnow = false, bleServer = false, g2 = false;
};
FakeNet gNet;
static bool isEspNowInitialized() { return gNet.espnow; }
static bool wifiRadioOn() { return gNet.wifiOn; }
static bool isBleServerInitialized() { return gNet.bleServer; }
static bool bleStackReady() { return gNet.g2; }
static void stopBootWifiConnect() {}
static void shutDownBleForCompanionReset(const RadioUsers&) {
  gNet.bleServer = false;
  gNet.g2 = false;
}
static const char* cmd_radiopower(const char* arg) {
  note(std::string("radiopower ") + arg);
  if (std::strcmp(arg, "off") == 0) {
    gNet.espnow = false;
    gNet.wifiOn = gNet.linked = false;
    if (gNet.webWaiter) gNet.httpStopPending = true;
    else gNet.http = false;
  } else {
    gNet.wifiOn = true;
    gNet.linked = gNet.joinsOnPowerOn;
  }
  return "OK";
}
static const char* cmd_espnow_deinit(const char*) {
  gNet.espnow = false;
  return "ESP-NOW deinitialized successfully";
}
static const char* cmd_espnow_init(const char*) {
  gNet.espnow = true;
  return "OK";
}
static bool initBluetooth() { return gNet.bleServer = true; }
static void startBleAdvertising() {}
static bool bleStackUp() { return gNet.g2 = true; }
static void bleBootReconnect() {}
static bool isHttpServerRunning() { return gNet.http; }
static bool httpServerStopPending() { return gNet.httpStopPending; }
static void mainLoopHttpStopTick() {  // httpServerStopPendingTick once the waiter drained
  gNet.webWaiter = false;
  if (gNet.httpStopPending) gNet.http = gNet.httpStopPending = false;
}
static const char* cmd_httpstart(const char*) {
  note("openhttp");
  if (!gNet.linked) return "ERROR: WiFi not connected. Connect to WiFi before starting HTTP server.";
  if (gNet.http) return "Error: HTTP server is already running. Stop it first with 'closehttp' to restart it.";
  gNet.http = true;
  return "HTTP server started";
}
enum WifiRadioState { WIFI_RADIO_OFF = 0, WIFI_RADIO_UP_FOR_ESPNOW, WIFI_RADIO_CONNECTED, WIFI_RADIO_UP_DISCONNECTED };
static WifiRadioState wifiRadioState() {
  return gNet.linked ? WIFI_RADIO_CONNECTED : gNet.wifiOn ? WIFI_RADIO_UP_DISCONNECTED : WIFI_RADIO_OFF;
}
static WifiRadioState wifiRadioDisplayState() { return wifiRadioState(); }
static void logSystemEvent(const char* category, const char* fmt, ...) {
  char text[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  if (std::strstr(text, "HTTP")) note(std::string("log ") + category + ": " + text);
}
static void setLastResult(const char*) {}
enum { SYSEVT_COMPANION_OFFLINE = 1 };
static void systemEventPost(int, const char*, const char*) {}
static bool radioHalTransportUp() { return true; }
static void radioHalMarkTransportLost() {}
static void radioHalStopTransport() { note("hal-stop"); }
std::atomic<bool> sHttpRestorePending{false};
std::atomic<uint32_t> sHttpRestoreArmedMs{0};
/*@USERS@*/

/*@COMPANION@*/
}  // namespace comp

// --- scenarios ---------------------------------------------------------------
static std::vector<RadioHalEvent> gDelivered;
static void recordEvent(const RadioHalEventData& e) { gDelivered.push_back(e.type); }

static void reset_world() {
  gNow = 1000;
  gEvents.clear();
  gFsMutex = &gFsMutexObj;
  gFsHolder = nullptr;
  gFsLockHoldMs = 0;
  gHostedInitialized = false;
  gInitReturnsAfterMs = 5;
  gUpAt = 0;
  gUpPosted = false;
  gResets = 0;
  gOnReset = nullptr;
  gRpcCalls = 0;
  gFirstRpcAt = 0;
  gRegisteredHandler = nullptr;
  gDelivered.clear();
  hal::sTransportUp = false;
  hal::sHostedLinkUp = false;
  hal::sHeld = false;
  hal::sRestartLatched = false;
  hal::sPrepared = false;
  hal::sEventsRegistered = false;
  hal::sEventFn = nullptr;
  hal::sSessionBleLinks = 0;
  comp::sRestarting.store(false);
  comp::sOpInFlight.store(false);
  comp::sBootInitDone = false;
  comp::gState = comp::CompanionState::Online;
  gEnOut = gEnPad = 1;  // the C6 is running
  gEnPadHeld = false;
  gCompanionSilence = nullptr;
  gDuringDelay = nullptr;
  gBeforeHoldDis = nullptr;
  gHostedTransportState = 0;  // Hosted not initialised
  gTxFenced = false;
  gFenceDrops = 0;
  gTxQueue.clear();
  gDuringTaskDelay = nullptr;
  gBeforeTxGate = nullptr;
  gTxGateAt = gEnLowAt = 0;
  comp::sMonitor = comp::CompanionMonitor();
  comp::gNet = comp::FakeNet();
  comp::sHeldUsers = comp::RadioUsers();
  comp::sHttpRestorePending.store(false);
  comp::sHttpRestoreArmedMs.store(0);
  comp::gDeferredFn = nullptr;
  comp::gCompanionAnswers = true;
}

// The main loop after the recovery: the web request drains, the deferred stop
// runs, then the companion tick hands the restart to cmd_exec.
static void mainLoopHttpStopTickAndRun() {
  comp::mainLoopHttpStopTick();
  comp::radioCompanionTick();
  CHECK(comp::gDeferredFn != nullptr);
  comp::runDeferred();
}

static bool sawEvent(const std::string& prefix) {
  for (const auto& e : gEvents)
    if (e.compare(0, prefix.size(), prefix) == 0) return true;
  return false;
}
static size_t eventIndex(const std::string& e) {
  for (size_t i = 0; i < gEvents.size(); i++)
    if (gEvents[i] == e) return i;
  return gEvents.size();
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
  gFsHolder = &gOtherTask;  // a writer stuck behind the dead link
  gFsLockHoldMs = 5000;
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
  const uint32_t start = gNow;
  hal::radioHalSilenceForRestart();  // Hosted's TX was not up: nothing queued, no flush
  const std::vector<std::string> want = {"unregister esp_wifi_stop", "hosted-tx fence", "hold_dis 9", "level 9=0",
                                         "dir 9 out", "hold_en 9"};
  CHECK(gEvents == want);
  CHECK(gNow == start);
  CHECK(hal::sHeld && !hal::sTransportUp && !hal::sHostedLinkUp && hal::sRestartLatched);
  CHECK(gEnPad == 0 && gEnPadHeld);
}

// An op already running in the companion worker (Start, Recover, c6restart)
// is inside hostedInitWiFi when rebootDevice runs. During the drain it pulses
// EN (Hosted's transport_gpio_reset) and then releases the hold the way a
// feature start does. Neither may raise EN before the restart.
static void an_op_in_flight_during_the_drain_cannot_release_the_c6() {
  reset_world();
  gCompanionSilence = [] { hal::radioHalSilenceForRestart(); };
  static bool releaseRefused = false;
  static bool padLowThroughout = true;
  releaseRefused = false;
  padLowThroughout = true;
  gDuringDelay = [] {
    if (!hal::sRestartLatched) return;  // the flush delay, before the silence
    hostedResetPulse();
    padLowThroughout = padLowThroughout && gEnPad == 0;
    releaseRefused = hal::radioHalHold(false) == ESP_ERR_INVALID_STATE;
    padLowThroughout = padLowThroughout && gEnPad == 0;
  };
  bool restarted = false;
  try {
    utils::rebootDevice("command", "commanded restart", 1000);
  } catch (const RestartSignal&) {
    restarted = true;
  }
  CHECK(restarted);
  CHECK(releaseRefused);
  CHECK(padLowThroughout);
  CHECK(gEnPad == 0 && gEnPadHeld);  // EN is low and latched at the restart
  CHECK(gEvents.back() == "restart");
}

// A feature start checked the latch just before the silence set it and wrote
// EN after the silence latched the pad: the release puts the line back.
static void a_release_racing_the_silence_leaves_en_low() {
  reset_world();
  hal::sHeld = true;  // the companion was held; a feature start releases it
  gEnOut = gEnPad = 0;
  gBeforeHoldDis = [] { hal::radioHalSilenceForRestart(); };
  CHECK(hal::radioHalHold(false) == ESP_ERR_INVALID_STATE);
  CHECK(gEnPad == 0 && gEnPadHeld);
  CHECK(hal::sHeld);
}

static void a_hold_after_the_silence_changes_nothing() {
  reset_world();
  hal::radioHalSilenceForRestart();
  gEvents.clear();
  CHECK(hal::radioHalHold(true) == ESP_OK);
  CHECK(hal::radioHalHold(false) == ESP_ERR_INVALID_STATE);
  CHECK(gEvents.empty());  // no pin write at all
  CHECK(gEnPad == 0 && gEnPadHeld);
}

static void companion_silence_stops_new_work_then_silences_the_hal() {
  reset_world();
  comp::sRestarting.store(false);
  CHECK(comp::submitOp(comp::Op::Start, nullptr, nullptr));
  comp::finishOp();
  gEvents.clear();
  comp::radioCompanionBeforeRestart();
  CHECK(comp::sRestarting.load());
  // New work was refused before the C6 went quiet, not after.
  CHECK((gEvents == std::vector<std::string>{"hal-silence restarting=1"}));
  CHECK(!comp::submitOp(comp::Op::Recover, nullptr, nullptr));  // no recovery re-releases the C6
  CHECK(!comp::sOpInFlight.load());
  CHECK((gEvents == std::vector<std::string>{"hal-silence restarting=1"}));
}

// A reboot can come before the companion's boot init finished (a command or
// the web page right after boot): radio features are refused anyway.
static void a_restart_before_boot_init_blocks_radio_features() {
  reset_world();
  const char* reason = nullptr;
  CHECK(!comp::radioCompanionBlocksRadio(&reason));  // before boot init the HAL alone decides
  comp::sRestarting.store(true);
  CHECK(comp::radioCompanionBlocksRadio(&reason));
  CHECK(reason && std::strcmp(reason, "the device is restarting") == 0);
  CHECK(gEvents.empty());
}

// A feature start on a held companion releases it and starts the transport;
// once the restart silenced it, the same call refuses and touches nothing.
static void a_restart_never_releases_a_held_companion() {
  reset_world();
  comp::sBootInitDone = true;
  comp::gState = comp::CompanionState::Held;
  comp::sRestarting.store(true);
  const char* reason = nullptr;
  CHECK(comp::radioCompanionBlocksRadio(&reason));
  CHECK(reason && std::strcmp(reason, "the device is restarting") == 0);
  CHECK(gEvents.empty());
  comp::sRestarting.store(false);  // control: without the restart it wakes the C6
  CHECK(!comp::radioCompanionBlocksRadio(&reason));
  CHECK((gEvents == std::vector<std::string>{"hal-hold 0", "hal-start"}));
}

static void the_health_tick_does_nothing_once_restarting() {
  reset_world();
  comp::sBootInitDone = true;
  comp::radioCompanionTick();
  CHECK((gEvents == std::vector<std::string>{"pop", "dispatch"}));  // control
  gEvents.clear();
  comp::sRestarting.store(true);
  comp::radioCompanionTick();
  CHECK(gEvents.empty());  // no event handling, no recovery dispatch
}

// The real fsLockForRestart over a fake FreeRTOS mutex.
static void the_restart_lock_is_bounded_and_kept() {
  reset_world();
  CHECK(fsLockForRestart(2000));  // free
  CHECK(gFsHolder == &gThisTask);
  gEvents.clear();
  CHECK(fsLockForRestart(2000));  // already ours (a command under FsLockGuard): no second take
  CHECK(gEvents.empty());
  gFsHolder = &gOtherTask;  // another task finishes its write in 300 ms
  gFsLockHoldMs = 300;
  uint32_t t = gNow;
  CHECK(fsLockForRestart(2000));
  CHECK(gNow - t == 300 && gFsHolder == &gThisTask);
  gFsHolder = &gOtherTask;  // a holder that never lets go
  gFsLockHoldMs = 5000;
  t = gNow;
  CHECK(!fsLockForRestart(2000));
  CHECK(gNow - t == 2000 && gFsHolder == &gOtherTask);
  gFsMutex = nullptr;  // before initMutexes()
  CHECK(!fsLockForRestart(2000));
}

static void hosted_events_track_the_link() {
  reset_world();
  hal::sEventFn = recordEvent;
  hal::hostedEventHandler(nullptr, ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_UP, nullptr);
  CHECK(hal::sTransportUp && hal::sHostedLinkUp);
  hal::sSessionBleLinks = 4;
  hal::hostedEventHandler(nullptr, ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_DOWN, nullptr);
  CHECK(!hal::sTransportUp && !hal::sHostedLinkUp && hal::sSessionBleLinks == 0);
  hal::hostedEventHandler(nullptr, ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_UP, nullptr);
  hal::sSessionBleLinks = 4;
  hal::hostedEventHandler(nullptr, ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_FAILURE, nullptr);
  CHECK(!hal::sTransportUp && !hal::sHostedLinkUp && hal::sSessionBleLinks == 0);
  hal::hostedEventHandler(nullptr, kOtherEventBase, ESP_HOSTED_EVENT_TRANSPORT_UP, nullptr);
  CHECK(!hal::sTransportUp && !hal::sHostedLinkUp);  // another event base is ignored
  CHECK((gDelivered == std::vector<RadioHalEvent>{RadioHalEvent::TransportUp, RadioHalEvent::TransportDown,
                                                  RadioHalEvent::TransportUp, RadioHalEvent::TransportFailure}));
}

static void stop_after_the_silence_leaves_hosted_up() {
  reset_world();
  gHostedInitialized = true;
  hal::radioHalSilenceForRestart();
  gEvents.clear();
  hal::radioHalStopTransport();
  CHECK(gEvents.empty());  // no teardown under live users
  CHECK(gHostedInitialized);
}

// Bench log 2026-11-08, first companion retry after the boot spent Hosted's
// retry counter (retry[100]): Hosted started at 45403 ms and gave up at 47138
// ms (1735 ms, right after the card init, in the same millisecond as "Waiting
// for esp_hosted slave to be ready"); the version query failed at 47148 ms and
// Hosted opened the data path at the slave at 47211 ms. That SDMMC host was
// wedged and TRANSPORT_UP never came: the start fails, after the full budget
// counted from the start, with one version query.
static void the_bench_retry_with_a_wedged_host_fails_after_the_budget() {
  reset_world();
  gNow = 45403;
  gInitReturnsAfterMs = 1735;
  CHECK(hal::startOnce(false) != ESP_OK);
  CHECK(gNow == 45403 + hal::kSlaveReadyBudgetMs);
  CHECK(gRpcCalls == 1 && gFirstRpcAt == gNow);
}

// The same retry on a healthy data path, where TRANSPORT_UP follows the data
// path opening as at that boot's first, good start (Open data path at 3215 ms,
// Transport active at 3244 ms): the start waits for it instead of failing.
static void a_retry_waits_for_the_slave_when_hosted_gives_up_at_once() {
  reset_world();
  gNow = 45403;
  gInitReturnsAfterMs = 1735;
  gUpAt = 47211 + 29;
  CHECK(hal::startOnce(false) == ESP_OK);
  CHECK(gFirstRpcAt >= gUpAt && gFirstRpcAt < gUpAt + hal::kSlaveReadyPollMs);
  CHECK(hal::sTransportUp && gRpcCalls == 1);
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

// An initialised session announces nothing new: no TRANSPORT_UP will come, so
// the start asks the companion at once instead of sitting out the budget.
static void an_initialized_session_without_a_link_is_asked_at_once() {
  reset_world();
  gHostedInitialized = true;
  hal::sHostedLinkUp = false;
  const uint32_t start = gNow;
  CHECK(hal::startOnce(false) != ESP_OK);
  CHECK(gNow == start && gRpcCalls == 1);
  CHECK(gResets == 0);
}

// Bench log 2026-11-08: boot attempt 1 spent Hosted's whole wait and failed;
// attempt 2 reused the still-initialised session and failed in the same
// millisecond (26756 ms). Attempt 2 must close the session, reset the C6 again
// and wait for it.
static void boot_attempt_two_resets_the_c6_and_waits_for_it() {
  reset_world();
  gInitReturnsAfterMs = hal::kSlaveReadyBudgetMs;  // attempt 1: Hosted's own wait runs out
  gOnReset = [](int n) {
    if (n != 2) return;
    gInitReturnsAfterMs = 1735;     // the counter is spent: Hosted gives up after the card init
    gUpAt = gNow + 1735 + 73 + 29;  // a healthy C6: up after the data path opens
  };
  CHECK(hal::radioHalPrepare() == ESP_OK);
  CHECK(hal::sPrepared);
  CHECK(gRegisteredHandler == &hal::hostedEventHandler);
  const std::vector<std::string> want = {
      "start", "hostedInitWiFi", "c6-reset", "version-rpc",                  // attempt 1
      "hosted-deinit",                                                       // the dead session closes
      "start", "hostedInitWiFi", "c6-reset", "TRANSPORT_UP", "version-rpc"};  // attempt 2
  CHECK(gEvents == want);
}

static void a_dead_c6_at_boot_gets_two_real_attempts() {
  reset_world();
  gInitReturnsAfterMs = hal::kSlaveReadyBudgetMs;
  gOnReset = [](int n) {
    if (n == 2) gInitReturnsAfterMs = 1735;
  };
  const uint32_t start = gNow;
  CHECK(hal::radioHalPrepare() != ESP_OK);
  CHECK(!hal::sPrepared);
  CHECK(gResets == 2 && gRpcCalls == 2);
  CHECK(gNow - start == 2 * hal::kSlaveReadyBudgetMs);  // each attempt within Hosted's budget
}

// Bench 2026-11-08, 7 of 15 reboots: between "commanded restart" and "SDMMC
// quiesced", Hosted's write task sent small packets (BLE HCI, RPC) to the
// held C6: "sdmmc_send_cmd returned 0x107", "Failed to send data", then
// "Unrecoverable host sdio state". The users keep producing through the
// drain; producers that passed the gate check just before it closed still
// queue their packets, here a full Hosted queue. TX must be closed and those
// packets sent while EN is high.
static void reboot_stops_hosted_tx_before_the_en_latch() {
  reset_world();
  gHostedTransportState = 2;  // TRANSPORT_TX_ACTIVE: a live link
  gCompanionSilence = [] { hal::radioHalSilenceForRestart(); };
  gBeforeTxGate = [] {
    for (int i = 1; i < kHostedTxQueueDepth; i++) queueTx("burst-" + std::to_string(i));
    queueTx("hci-racing-the-gate");
  };
  gDuringDelay = [] { hostedTx(hal::sRestartLatched ? "tcp-ack-in-drain" : "http-reply"); };
  bool restarted = false;
  try {
    utils::rebootDevice("command", "commanded restart", 1000);
  } catch (const RestartSignal&) {
    restarted = true;
  }
  CHECK(restarted);
  CHECK(!sawEvent("tx 0x107"));  // no CMD53 against the held C6
  CHECK(sawEvent("tx sent http-reply"));
  CHECK(sawEvent("tx refused tcp-ack-in-drain"));
  const size_t gate = eventIndex("hosted-tx fence");
  const size_t raced = eventIndex("tx sent hci-racing-the-gate");  // the last of the full queue
  const size_t latch = eventIndex("level 9=0");
  CHECK(gate < raced && raced < latch && latch < gEvents.size());
  CHECK(gEnLowAt - gTxGateAt >= hal::kHostedTxFlushMs);
  CHECK(gHostedTransportState == 2);  // the state stays up: no slave bring-up in the window
  CHECK(gEnPad == 0 && gEnPadHeld);
  CHECK(gEvents.back() == "restart");
}

// The gate and its flush, in order, ahead of the hold; the drain still runs
// in full after the hold (quietAt is taken when the silence returns).
static void hal_silence_closes_hosted_tx_then_holds_the_c6() {
  reset_world();
  gHostedTransportState = 2;
  hal::sTransportUp = true;
  const uint32_t start = gNow;
  hal::radioHalSilenceForRestart();
  const std::vector<std::string> want = {"unregister esp_wifi_stop", "hosted-tx fence", "hold_dis 9", "level 9=0",
                                         "dir 9 out", "hold_en 9"};
  CHECK(gEvents == want);
  CHECK(gNow - start == hal::kHostedTxFlushMs);
  CHECK(gTxFenced && is_transport_tx_ready() && hal::sRestartLatched && gEnPad == 0);
}

// Review 2026-11-08: lowering Hosted's transport state to close TX would turn
// a Wi-Fi init (esp_wifi_remote_init, System_WiFi.cpp's IDLE repair) or a BLE
// open that is already past the radio gate into a slave bring-up: a reset
// pulse on the live C6 during the flush, then SDIO card init on slot 1 in the
// drain. With the fence they find the transport up and touch nothing.
static void a_wifi_or_ble_init_in_the_window_starts_no_c6_bring_up() {
  reset_world();
  gHostedTransportState = 2;
  gCompanionSilence = [] { hal::radioHalSilenceForRestart(); };
  gDuringTaskDelay = [] { hostedReconfigure("wifi-init"); };    // during the flush
  gDuringDelay = [] {
    if (hal::sRestartLatched) hostedReconfigure("ble-open");  // during the drain
  };
  bool restarted = false;
  try {
    utils::rebootDevice("command", "commanded restart", 1000);
  } catch (const RestartSignal&) {
    restarted = true;
  }
  CHECK(restarted);
  CHECK(sawEvent("reconfigure wifi-init: already up"));
  CHECK(sawEvent("reconfigure ble-open: already up"));
  CHECK(!sawEvent("reconfigure wifi-init: slave reset") && !sawEvent("reconfigure ble-open: slave reset"));
  // EN went low once, at the latch, and stayed there.
  size_t lows = 0;
  for (const auto& e : gEvents) lows += e == "level 9=0";
  CHECK(lows == 1 && gEnPad == 0 && gEnPadHeld);
}

// The latch flags come before the flush: an op already running in the
// companion worker that tries to tear Hosted down, or to hold or release the
// C6, inside the 100 ms flush is refused like one in the drain.
static void an_op_inside_the_flush_is_refused() {
  reset_world();
  gHostedTransportState = 2;
  gHostedInitialized = true;
  static esp_err_t releaseRc = ESP_OK;
  releaseRc = ESP_OK;
  gDuringTaskDelay = [] {
    hal::radioHalStopTransport();             // recoverProc / holdProc
    CHECK(hal::radioHalHold(true) == ESP_OK);  // holdProc: no pin write before the flush ends
    releaseRc = hal::radioHalHold(false);      // a feature start
  };
  hal::radioHalSilenceForRestart();
  CHECK(gHostedInitialized && !sawEvent("hosted-deinit"));
  CHECK(releaseRc == ESP_ERR_INVALID_STATE);
  // The only EN write is the latch after the flush.
  CHECK(eventIndex("level 9=0") > eventIndex("hosted-tx fence"));
  CHECK(gEnLowAt - gTxGateAt >= hal::kHostedTxFlushMs);
  CHECK(gEnPad == 0 && gEnPadHeld);
}

// ---- in-place companion resets and the web server --------------------------
// Bench 2026-11-08: after `c6restart` the C6, Wi-Fi (same address, NTP) and
// BLE came back, and every HTTP request was refused until an esptool reset:
// stopUsers' `radiopower off` stopped the server and `radiopower on` does not
// start it again.
static void wifi_and_http_running() {
  comp::gNet.wifiOn = comp::gNet.linked = true;
  comp::gNet.http = true;
  comp::gNet.g2 = true;
}

static void c6restart_brings_the_web_server_back() {
  reset_world();
  wifi_and_http_running();
  comp::recoverProc(nullptr);
  CHECK(comp::gNet.wifiOn && comp::gNet.linked && comp::gNet.g2);
  CHECK(comp::gNet.http);
  const size_t off = eventIndex("radiopower off"), on = eventIndex("radiopower on"), http = eventIndex("openhttp");
  CHECK(off < on && on < http && http < gEvents.size());
  CHECK(!comp::sHttpRestorePending.load());
}

static void a_reset_with_the_web_server_off_leaves_it_off() {
  reset_world();
  wifi_and_http_running();
  comp::gNet.http = false;  // closehttp earlier
  comp::recoverProc(nullptr);
  CHECK(comp::gNet.linked && !comp::gNet.http);
  CHECK(!sawEvent("openhttp") && !comp::sHttpRestorePending.load());
}

static void c6hold_off_brings_the_web_server_back() {
  reset_world();
  wifi_and_http_running();
  comp::holdProc(nullptr);
  CHECK(!comp::gNet.wifiOn && !comp::gNet.http);
  comp::releaseProc(nullptr);
  CHECK(comp::gNet.linked && comp::gNet.http);
}

// A web request waiting on cmd_exec (the bench polling c6status) defers the
// server stop to the main loop, past the recovery that holds cmd_exec. The
// restart waits for that stop, then runs on cmd_exec from the tick.
static void a_deferred_stop_is_followed_by_a_restart() {
  reset_world();
  wifi_and_http_running();
  comp::gNet.webWaiter = true;
  comp::sBootInitDone = true;
  comp::recoverProc(nullptr);
  CHECK(comp::gNet.httpStopPending && !sawEvent("openhttp") && comp::sHttpRestorePending.load());
  comp::radioCompanionTick();  // the stop has not run yet: nothing to do
  CHECK(!sawEvent("submitted"));
  mainLoopHttpStopTickAndRun();
  CHECK(comp::gNet.http && !comp::sHttpRestorePending.load());
}

// Wi-Fi out of range when the restore runs: the server starts at the link,
// inside the window; after the window, nothing starts on its own.
static void a_late_link_still_gets_the_web_server() {
  reset_world();
  wifi_and_http_running();
  comp::gNet.joinsOnPowerOn = false;
  comp::sBootInitDone = true;
  comp::recoverProc(nullptr);
  CHECK(comp::gNet.wifiOn && !comp::gNet.linked && !comp::gNet.http);
  CHECK(comp::sHttpRestorePending.load());
  comp::radioCompanionTick();
  CHECK(!sawEvent("submitted"));
  advance(10000);
  comp::gNet.linked = true;
  comp::radioCompanionTick();
  CHECK(sawEvent("submitted"));
  comp::runDeferred();
  CHECK(comp::gNet.http && !comp::sHttpRestorePending.load());

  reset_world();
  wifi_and_http_running();
  comp::gNet.joinsOnPowerOn = false;
  comp::sBootInitDone = true;
  comp::recoverProc(nullptr);
  advance(comp::kHttpRestoreWindowMs);
  comp::radioCompanionTick();
  CHECK(!comp::sHttpRestorePending.load() && sawEvent("log C6: HTTP server not restarted"));
  comp::gNet.linked = true;
  comp::radioCompanionTick();
  CHECK(!sawEvent("submitted") && !comp::gNet.http);
}

// A second reset while a restart is still pending keeps the intent.
static void a_pending_restart_survives_another_reset() {
  reset_world();
  wifi_and_http_running();
  comp::gNet.joinsOnPowerOn = false;
  comp::recoverProc(nullptr);
  CHECK(comp::sHttpRestorePending.load() && !comp::gNet.http);
  comp::gNet.joinsOnPowerOn = true;
  comp::recoverProc(nullptr);
  CHECK(comp::gNet.http && !comp::sHttpRestorePending.load());
}

int main() {
  reboot_quiesces_sdmmc_after_the_flush_and_before_the_restart();
  reboot_with_a_busy_filesystem_still_restarts_without_extra_wait();
  factory_reset_restart_quiesces_too();
  hal_silence_unregisters_the_wifi_stop_rpc_and_holds_the_c6();
  an_op_in_flight_during_the_drain_cannot_release_the_c6();
  a_release_racing_the_silence_leaves_en_low();
  a_hold_after_the_silence_changes_nothing();
  companion_silence_stops_new_work_then_silences_the_hal();
  a_restart_before_boot_init_blocks_radio_features();
  a_restart_never_releases_a_held_companion();
  the_health_tick_does_nothing_once_restarting();
  the_restart_lock_is_bounded_and_kept();
  hosted_events_track_the_link();
  stop_after_the_silence_leaves_hosted_up();
  the_bench_retry_with_a_wedged_host_fails_after_the_budget();
  a_retry_waits_for_the_slave_when_hosted_gives_up_at_once();
  a_stale_link_flag_from_the_last_session_is_not_trusted();
  a_slave_that_never_answers_fails_inside_hosteds_budget();
  an_initialized_live_session_does_not_wait();
  an_initialized_session_without_a_link_is_asked_at_once();
  boot_attempt_two_resets_the_c6_and_waits_for_it();
  a_dead_c6_at_boot_gets_two_real_attempts();
  reboot_stops_hosted_tx_before_the_en_latch();
  hal_silence_closes_hosted_tx_then_holds_the_c6();
  a_wifi_or_ble_init_in_the_window_starts_no_c6_bring_up();
  an_op_inside_the_flush_is_refused();
  c6restart_brings_the_web_server_back();
  a_reset_with_the_web_server_off_leaves_it_off();
  c6hold_off_brings_the_web_server_back();
  a_deferred_stop_is_followed_by_a_restart();
  a_late_link_still_gets_the_web_server();
  a_pending_restart_survives_another_reset();
  std::puts("restart SDMMC quiesce, Hosted TX fence, companion start wait and HTTP restore: all scenarios passed");
  return 0;
}
