// Host harness for BLE_CentralStack.cpp, the one owner of the shared BLE
// central stack: the Bluedroid host and its controller link, the setup worker,
// the scan object, the GAP/GATTC observers and the console service on the
// client host.
//
// test_ble_central_stack.py splices production text at the INSERT markers,
// with its include lines blanked:
//   Arduino.h             constrain(), the unit's TX power clamp
//   HAL_Bluetooth.h       BluetoothHalStatus (ready/stopped/hostAbsent), the HAL API
//   Bluetooth.h           the BleRoleTransition states
//   G2_Glasses.h          G2_BLE_LOCAL_MTU_PREF, the unit's static_assert operand
//   BLE_CentralStack.h    the public API, which the fakes also observe
//   BLE_CentralStack.cpp  its whole #if ENABLE_BLUETOOTH region: the unit
// Every other symbol is a fake below. Each side-effecting boundary call
// appends one entry to world.events, so the cases assert the exact order;
// the role claim shows as "role+ <state>" ... "role-". Reads (HAL status,
// owner flags, settings) are inputs and are not recorded.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// ---- SDK and Arduino surface -----------------------------------------------
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
static const char* esp_err_to_name(esp_err_t err) {
  return err == ESP_OK ? "ESP_OK" : err == ESP_FAIL ? "ESP_FAIL" : "ESP_ERR_OTHER";
}
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))  // a 1 kHz tick: one tick per millisecond
void vTaskDelay(TickType_t ticks);

// INSERT_CONSTRAIN

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  const char* c_str() const { return text_.c_str(); }

 private:
  std::string text_;
};

void broadcastOutput(const char* line);
// System_Debug.h formats into a stack buffer and broadcasts the line.
#define BROADCAST_PRINTF(fmt, ...)                        \
  do {                                                    \
    char _bpBuf[256];                                     \
    snprintf(_bpBuf, sizeof(_bpBuf), fmt, ##__VA_ARGS__); \
    broadcastOutput(_bpBuf);                              \
  } while (0)
__attribute__((format(printf, 1, 2))) void debugG2(const char* fmt, ...);
#define DEBUG_G2F(fmt, ...) debugG2(fmt, ##__VA_ARGS__)
size_t hw1InternalFreeBytes() { return 98304; }
size_t hw1InternalLargestBlock() { return 65536; }

typedef enum { ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT = 20 } esp_gap_ble_cb_event_t;
typedef union { int placeholder; } esp_ble_gap_cb_param_t;
typedef void (*gap_event_handler)(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);

// The Arduino BLE library calls the unit makes (BLEDevice.h / BLEScan.h).
class BLEScan {
 public:
  void setActiveScan(bool active);
  void setInterval(uint16_t intervalMSecs);
  void setWindow(uint16_t windowMSecs);
};
class BLEDevice {
 public:
  static BLEScan* getScan();
  static esp_err_t setMTU(uint16_t mtu);
  static void setCustomGapHandler(gap_event_handler handler);
};

// ---- Production declarations -------------------------------------------------
// INSERT_HAL_BLUETOOTH
// INSERT_ROLE_TRANSITION
#if ENABLE_G2_GLASSES
// INSERT_G2_MTU
// Defined after the unit, which declares it again.
void g2GapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
#endif
// INSERT_STACK_HEADER

// ---- The world the unit runs in ------------------------------------------------
using Events = std::vector<std::string>;
using Backend = BluetoothHalBackend;
using Controller = BluetoothHalControllerState;

static BluetoothHalStatus halWith(Backend backend, Controller controller, bool hostEnabled,
                                  bool hostUninitialized, bool stackInitialized) {
  BluetoothHalStatus s{};
  s.backend = backend;
  s.controller = controller;
  s.controllerStateIsLive = backend == Backend::Native;
  s.hostEnabled = hostEnabled;
  s.hostUninitialized = hostUninitialized;
  s.stackInitialized = stackInitialized;
  return s;
}
// Never started on a native controller, or verified down: absent and stopped.
static BluetoothHalStatus halStopped(Backend b = Backend::Native) {
  return halWith(b, Controller::Idle, false, true, false);
}
// Hosted before its first acknowledged lifecycle step: absent, not stopped.
static BluetoothHalStatus halHostedNeverStarted() {
  return halWith(Backend::Hosted, Controller::Unknown, false, true, false);
}
static BluetoothHalStatus halReady(Backend b = Backend::Native) {
  return halWith(b, Controller::Enabled, true, false, true);
}
// A stale generation: controller and host run, Arduino's initialized flag was
// never published (a faulted init, a recovery that stopped halfway).
static BluetoothHalStatus halPartial(Backend b = Backend::Native) {
  return halWith(b, Controller::Enabled, true, false, false);
}
// The host went down but the controller did not return to Idle.
static BluetoothHalStatus halControllerStuck(Backend b = Backend::Native) {
  return halWith(b, Controller::Initialized, false, true, false);
}

struct Settings {
  bool bleEnabled = true;
  int bleTxPower = 3;  // System_Settings.cpp default
};
Settings gSettings;

struct World {
  // The HAL and the BLE library.
  BluetoothHalStatus hal = halStopped();
  bool halInitOk = true;
  bool halDeinitOk = true;
  bool halDeinitStops = true;  // a successful deinit also reaches stopped()
  bool txPowerSupported = true;
  esp_err_t txPowerResult = ESP_OK;
  bool scanAvailable = true;
  BLEScan scan;
  gap_event_handler gapHandler = nullptr;
  // Admission inputs.
  bool companionBlocks = false;
  const char* companionReason = nullptr;
  bool memoryOk = true;
  bool fault = false;
  bool retirementPending = false;
  // The console server and the device owners on the stack.
  bool serverUp = false;
  bool glassesOpen = false;
  bool ringOpen = false;
  bool phoneActive = false;
  bool ringConnectInFlight = false;
  bool ringQuiesces = true;
  bool phoneAcksStoppedHost = true;
  // The setup worker.
  bool setupStartOk = true;
  bool setupReleaseOk = true;
  bool phoneWorkerReleaseOk = true;  // pipe builds: the phone_pipe task left
  // The role-transition token (Bluetooth.cpp): re-entrant for the task that
  // holds it, keeping the outer state; another task's claim refuses.
  bool roleHeldByOtherTask = false;
  int roleDepth = 0;
  BleRoleTransition roleState = BleRoleTransition::IDLE;
  // Records.
  Events events;
  Events logs;
  Events debug;
  Events outsideClaim;         // boundary calls made without the role claim
  Events readyDuringTeardown;  // teardown steps that found admission open
  Events peersBeforeReady;     // peer specs registered before ready was published
  Events releasesWithHostUp;   // worker releases while a host of any generation was up
  BleRoleTransition stateAtHalInit = BleRoleTransition::IDLE;
  bool armSawReadyScan = false;
};
static World world;

static void ev(const std::string& e) {
  if (world.roleDepth == 0) world.outsideClaim.push_back(e);
  world.events.push_back(e);
}
// A teardown step: admission must already be closed (no ready flag, no scan).
static void teardownStep(const std::string& e) {
  ev(e);
  if (bleStackReady() || bleStackScan() != nullptr) world.readyDuringTeardown.push_back(e);
}

static const char* roleName(BleRoleTransition state) {
  switch (state) {
    case BleRoleTransition::STACK_UP: return "STACK_UP";
    case BleRoleTransition::STACK_DOWN: return "STACK_DOWN";
    case BleRoleTransition::RECOVERING: return "RECOVERING";
    default: return "OTHER";
  }
}
bool bleRoleTransitionBegin(BleRoleTransition state) {
  if (state == BleRoleTransition::IDLE) return false;
  if (world.roleHeldByOtherTask) {
    world.events.push_back(std::string("role-busy ") + roleName(state));
    return false;
  }
  if (world.roleDepth++ == 0) world.roleState = state;
  world.events.push_back(std::string("role+ ") + roleName(state));
  return true;
}
void bleRoleTransitionEnd() {
  if (world.roleDepth > 0 && --world.roleDepth == 0) world.roleState = BleRoleTransition::IDLE;
  world.events.push_back("role-");
}
BleRoleTransition bleRoleTransitionState() { return world.roleState; }
bool bleStackLifecycleFaulted() { return world.fault; }
void bleStackSetLifecycleFault(bool faulted) {
  world.fault = faulted;
  ev(faulted ? "fault 1" : "fault 0");
}

BluetoothHalStatus bluetoothHalStatus() { return world.hal; }
bool bluetoothHalInit(const char* deviceName) {
  ev(std::string("hal-init ") + (deviceName ? deviceName : "(null)"));
  world.stateAtHalInit = world.roleState;
  // A failed init can leave controller and host enabled without Arduino's
  // initialized flag: the partial host the unit rolls back.
  world.hal = world.halInitOk ? halReady(world.hal.backend) : halPartial(world.hal.backend);
  return world.halInitOk;
}
BluetoothHalShutdownResult bluetoothHalDeinit() {
  teardownStep("hal-deinit");
  // Phase 3, host and controller still up, a client quarantined.
  if (!world.halDeinitOk) return {false, 3, 1, 1, 2, 2, true};
  world.hal = world.halDeinitStops ? halStopped(world.hal.backend) : halControllerStuck(world.hal.backend);
  return {true, 0, 1, 3, 2, 0, false};
}
bool bluetoothHalSupportsTxPower() { return world.txPowerSupported; }
esp_err_t bluetoothHalSetTxPower(uint8_t level) {
  ev("tx-power " + std::to_string(level));
  return world.txPowerResult;
}

BLEScan* BLEDevice::getScan() {
  ev("get-scan");
  return world.scanAvailable ? &world.scan : nullptr;
}
esp_err_t BLEDevice::setMTU(uint16_t mtu) {
  ev("mtu " + std::to_string(mtu));
  return ESP_OK;
}
void BLEDevice::setCustomGapHandler(gap_event_handler handler) {
  world.gapHandler = handler;
  if (!handler) {
    teardownStep("gap-handler null");
    return;
  }
#if ENABLE_G2_GLASSES
  if (handler == &g2GapEventHandler) {
    ev("gap-handler g2");
    return;
  }
#endif
  ev("gap-handler other");
}
void BLEScan::setActiveScan(bool active) { ev(active ? "scan active 1" : "scan active 0"); }
void BLEScan::setInterval(uint16_t intervalMSecs) { ev("scan interval " + std::to_string(intervalMSecs)); }
void BLEScan::setWindow(uint16_t windowMSecs) { ev("scan window " + std::to_string(windowMSecs)); }

void vTaskDelay(TickType_t ticks) { ev("delay " + std::to_string(ticks)); }
void broadcastOutput(const char* line) { world.logs.push_back(line ? line : ""); }
void debugG2(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  world.debug.push_back(line);
}

bool checkMemoryAvailable(const char* component, String* outReason) {
  ev(std::string("mem ") + (component ? component : "(null)"));
  if (!world.memoryOk && outReason) *outReason = "blestack needs 52000 B internal, 41000 B free";
  return world.memoryOk;
}
bool radioCompanionBlocksRadio(const char** reason) {
  if (!world.companionBlocks) return false;
  if (reason) *reason = world.companionReason;
  return true;
}
bool isBleServerInitialized() { return world.serverUp; }
bool isG2ClientInitialized() { return world.glassesOpen; }
bool g2RingIsOpen() { return world.ringOpen; }
bool phonePipeActive() { return world.phoneActive; }
bool g2RingConnectInFlight() { return world.ringConnectInFlight; }

void bleCentralTxInit() { ev("tx-init"); }
bool bleCentralClientsRetirementPending() { return world.retirementPending; }
void bleCentralClientsTerminalTeardownAcknowledged() {
  teardownStep("ack");
  world.retirementPending = false;
}
void bleCentralLinksHostStopped() { teardownStep("links-host-stopped"); }
void bleCentralRecoveryBlock(bool blocked) { ev(blocked ? "recovery-block 1" : "recovery-block 0"); }
bool g2RingPrepareForStackTeardown(uint32_t timeoutMs) {
  teardownStep("ring-prepare " + std::to_string(timeoutMs));
  return world.ringQuiesces;
}
// As the pipe's own (BLE_PhonePipe.cpp): it acknowledges only a host the HAL
// reports stopped(), so it refuses a Hosted host that never ran.
bool phonePipeHostStopped() {
  teardownStep("phone-host-stopped");
  return bluetoothHalStatus().stopped() && world.phoneAcksStoppedHost;
}
void bleCoexInstallGattcObserver() { ev("coex-observer"); }
bool bleConsoleRegisterOnClientHost() {
  ev("console-register");
  return true;
}
bool bleSetupWorkerStart() {
  ev("setup-start");
  return world.setupStartOk;
}
// A worker leaves only after admission closed and once no host is up.
static void workerReleased(const std::string& e) {
  teardownStep(e);
  if (!bluetoothHalStatus().hostAbsent()) world.releasesWithHostUp.push_back(e);
}
bool bleSetupWorkerRelease(uint32_t timeoutMs) {
  workerReleased("setup-release " + std::to_string(timeoutMs));
  return world.setupReleaseOk;
}
#if ENABLE_BLE_PHONE_PIPE
// The phone_pipe task parks across `closephone` (its reset pass needs the
// stopped host); the stack owner releases it once the host is down.
bool phonePipeWorkerRelease(uint32_t timeoutMs) {
  workerReleased("phone-worker-release " + std::to_string(timeoutMs));
  return world.phoneWorkerReleaseOk;
}
#else
// BLE_PhonePipe.h's stub in a pipe-less build: no worker to release.
bool phonePipeWorkerRelease(uint32_t) { return true; }
#endif
// The peer specs are registered once the stack is published ready.
static void peerRegistered(const std::string& e) {
  ev(e);
  if (!bleStackReady()) world.peersBeforeReady.push_back(e);
}
void g2RegisterBlePeer() { peerRegistered("peer g2"); }
void g2RingRegisterPeer() { peerRegistered("peer ring"); }
void phonePipeRegisterPeer() { peerRegistered("peer phone"); }
void bleArmAutoReconnectPeers() {
  ev("arm-peers");
  // The reconnect tick admits through bleStackReady(): it must be up by now.
  world.armSawReadyScan = bleStackReady() && bleStackScan() == &world.scan;
}

// ---- The unit -------------------------------------------------------------------
// INSERT_PRODUCTION

#if ENABLE_G2_GLASSES
void g2GapEventHandler(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t*) {}
#endif

// ---- Expectations for this build (the lane's ENABLE_* flags) -----------------------
static const std::string kUp = "role+ STACK_UP";
static const std::string kDown = "role+ STACK_DOWN";
static const std::string kEnd = "role-";

static Events cat(std::initializer_list<Events> parts) {
  Events out;
  for (const Events& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

// Installed right after the MTU on a fresh host.
static const Events kObservers = {
#if ENABLE_G2_GLASSES
    "gap-handler g2",
#endif
#if ENABLE_BLE_COEX_RECORDS
    "coex-observer",
#endif
};
static const Events kScanDefaults = {"get-scan", "scan active 1", "scan interval 100", "scan window 99"};
// Registered on the fresh host before the setup worker and any connect.
static const Events kConsole = {
#if ENABLE_BLE_PHONE_PIPE
    "console-register",
#endif
};
// Every built device owner's peer spec, after ready and before the arm.
static const Events kPeers = {
#if ENABLE_G2_GLASSES
    "peer g2",
#endif
#if ENABLE_G2_GLASSES && ENABLE_R1_HEALTH
    "peer ring",
#endif
#if ENABLE_BLE_PHONE_PIPE
    "peer phone",
#endif
};

// bleStackUp() from an absent host, up to the memory check: no pipe handshake.
static const Events kAdmit = {"tx-init", "mem blestack"};
// The host init through the observers; `power` is the TX power step.
static Events hostUp(const Events& power = {"tx-power 3"}) {
  return cat({{"hal-init HardwareOne"}, power, {"mtu 244"}, kObservers});
}
// The client-role bring-up after the memory check.
static Events bringUp(const Events& power = {"tx-power 3"}) {
  return cat({hostUp(power), kScanDefaults, kConsole, {"setup-start"}, kPeers, {"arm-peers"}});
}
// The shared checked terminal teardown (hostTeardown()).
static const Events kTeardown = {"ring-prepare 2000", "gap-handler null", "hal-deinit", "delay 200",
                                 "ack", "links-host-stopped", "phone-host-stopped"};
// A partial host's rollback (rollbackPartialHost()).
static const Events kRollback = {"gap-handler null", "hal-deinit", "ack", "links-host-stopped",
                                 "phone-host-stopped"};
// bleStackDown() after the host is gone: the phone worker (pipe builds), then
// the setup worker, the recovery block, the fault and the reclaim tick.
static const Events kRelease = {
#if ENABLE_BLE_PHONE_PIPE
    "phone-worker-release 1500",
#endif
    "setup-release 2000", "recovery-block 0", "fault 0", "delay 20"};

// ---- Checks ---------------------------------------------------------------------------
static int failures = 0;
static int caseCount = 0;
static const char* currentCase = "";
static int outerDepth = 0;  // a role claim the case itself holds around the call

static std::string join(const Events& events) {
  std::string out;
  for (const std::string& e : events) out += (out.empty() ? "" : ", ") + e;
  return out;
}
static void fail(int line, const std::string& what) {
  std::fprintf(stderr, "FAIL %s (line %d): %s\n", currentCase, line, what.c_str());
  ++failures;
}
#define EXPECT(cond)                    \
  do {                                  \
    if (!(cond)) fail(__LINE__, #cond); \
  } while (0)
static void checkEvents(int line, const Events& expected) {
  if (world.events == expected) return;
  fail(line, "events\n  expected: " + join(expected) + "\n  actual:   " + join(world.events));
}
#define EXPECT_EVENTS(...) checkEvents(__LINE__, __VA_ARGS__)
static void checkRefusal(int line, const char* expected) {
  const char* actual = bleStackLastRefusal();
  if (actual && std::strcmp(actual, expected) == 0) return;
  fail(line, std::string("refusal\n  expected: ") + expected + "\n  actual:   " + (actual ? actual : "(null)"));
}
#define EXPECT_REFUSAL(text) checkRefusal(__LINE__, text)
static bool logged(const char* text) {
  for (const std::string& line : world.logs) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}
static long countOf(const char* event) { return std::count(world.events.begin(), world.events.end(), event); }
// The phone pipe's host-stopped handshakes the last call asked for.
static long handshakes() { return countOf("phone-host-stopped"); }

// Holds after every bleStackUp()/bleStackDown(), whatever the outcome.
static void invariants(const char* op, int line) {
  const std::string at = std::string(op) + ": ";
  if (world.roleDepth != outerDepth) fail(line, at + "the role claim was not released");
  if (!world.outsideClaim.empty()) fail(line, at + "calls outside the role claim: " + join(world.outsideClaim));
  if (!world.readyDuringTeardown.empty()) {
    fail(line, at + "teardown steps with admission open: " + join(world.readyDuringTeardown));
  }
  if (!world.peersBeforeReady.empty()) {
    fail(line, at + "peers registered before ready: " + join(world.peersBeforeReady));
  }
  if (!world.releasesWithHostUp.empty()) {
    fail(line, at + "workers released with a host up: " + join(world.releasesWithHostUp));
  }
  if (!bleStackLastRefusal()) fail(line, at + "bleStackLastRefusal() is nullptr");
  if (!bleStackReady() && bleStackScan() != nullptr) fail(line, at + "a scan object without a ready stack");
}
static bool up(int line) {
  const bool ok = bleStackUp();
  invariants("bleStackUp", line);
  return ok;
}
static bool down(int line) {
  const bool ok = bleStackDown();
  invariants("bleStackDown", line);
  return ok;
}
#define UP() up(__LINE__)
#define DOWN() down(__LINE__)

static void clearRecords() {
  world.events.clear();
  world.logs.clear();
  world.debug.clear();
  world.outsideClaim.clear();
  world.readyDuringTeardown.clear();
  world.peersBeforeReady.clear();
  world.releasesWithHostUp.clear();
  world.stateAtHalInit = BleRoleTransition::IDLE;
  world.armSawReadyScan = false;
}
// Every case starts from a native host that never ran and the stack down.
static void startCase(const char* name) {
  currentCase = name;
  ++caseCount;
  world = World{};
  gSettings = Settings{};
  outerDepth = 0;
  bleStackNoteHostLost();  // the unit's ready flag and scan pointer start clear
  clearRecords();
}
static void bringUpForCase(int line) {
  if (!up(line) || !bleStackReady()) fail(line, "precondition: bleStackUp() from an absent host");
  clearRecords();
}
#define BRING_UP() bringUpForCase(__LINE__)

int main() {
  // F0: the HAL fixtures mean what the cases say, under the production predicates.
  startCase("F0 HAL fixtures under HAL_Bluetooth.h's predicates");
  EXPECT(halStopped().hostAbsent() && halStopped().stopped() && !halStopped().ready());
  EXPECT(halHostedNeverStarted().hostAbsent() && !halHostedNeverStarted().stopped());
  EXPECT(halReady().ready() && !halReady().hostAbsent() && !halReady().stopped());
  EXPECT(!halPartial().ready() && !halPartial().hostAbsent() && !halPartial().stopped());
  EXPECT(!halControllerStuck().hostAbsent() && !halControllerStuck().stopped());

  // S1: up from a host that never ran. No HAL teardown; the bring-up order is
  // tx gate, the memory budget, host init, TX power,
  // MTU, observers, scan defaults, console service, setup worker, then ready,
  // the peer specs and the auto-reconnect arm.
  startCase("S1 up from an absent host");
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp(), {kEnd}}));
  EXPECT(bleStackReady() && bleStackScan() == &world.scan);
  EXPECT(world.armSawReadyScan);
  EXPECT(world.hal.ready() && world.stateAtHalInit == BleRoleTransition::STACK_UP);
  EXPECT(world.logs.empty() && !world.fault);
#if ENABLE_G2_GLASSES
  EXPECT(world.gapHandler == &g2GapEventHandler);
#else
  EXPECT(world.gapHandler == nullptr);
#endif

  // A stopped host settles a client retirement still pending from the last
  // generation before the new one starts.
  startCase("S1b a stopped host acknowledges a pending client retirement");
  world.retirementPending = true;
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp, "tx-init", "ack", "mem blestack"}, bringUp(), {kEnd}}));
  EXPECT(!world.retirementPending && bleStackReady());

  // Hosted with no acknowledged controller state (the P4's first start after
  // boot): absent, so no teardown and no pipe handshake, which would refuse a
  // host that is not stopped(); not stopped, so a pending retirement is not
  // acknowledged on it. The up succeeds; the backend keeps its TX power.
  startCase("S1c Hosted host absent but not stopped: the first start succeeds");
  world.hal = halHostedNeverStarted();
  world.retirementPending = true;
  world.txPowerSupported = false;
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp({}), {kEnd}}));
  EXPECT(handshakes() == 0);
  EXPECT(logged("[BLE] Radio backend controls BLE TX power; using companion default"));
  EXPECT(bleStackReady() && world.hal.backend == Backend::Hosted && world.retirementPending);

  // S2: a second up finds the stack ready: no HAL, observer or worker call.
  startCase("S2 up twice");
  BRING_UP();
  EXPECT(UP());
  EXPECT_EVENTS({kUp, kEnd});
  EXPECT(bleStackReady() && bleStackScan() == &world.scan);

  // The ready shortcut does not hide a console server on the host.
  startCase("S2b ready flag with the console server up");
  BRING_UP();
  world.serverUp = true;
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, kEnd});
  EXPECT_REFUSAL("the app console server owns the Bluetooth host; run 'closeconsole' first");

  // S3: the admission refusals, in their order, each before any HAL call.
  startCase("S3 up refusals in order, none touching the HAL");
  gSettings.bleEnabled = false;
  world.fault = true;
  world.companionBlocks = true;
  world.companionReason = "the radio companion is resetting";
  world.serverUp = true;
  world.hal = halReady();  // the console server's own host
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, kEnd});
  EXPECT_REFUSAL("Bluetooth is disabled - run 'bleenabled 1' first");
  gSettings.bleEnabled = true;
  clearRecords();
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, kEnd});
  EXPECT_REFUSAL("a prior Bluetooth teardown was incomplete; reboot required");
  world.fault = false;
  clearRecords();
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, kEnd});
  EXPECT(bleStackLastRefusal() == world.companionReason);  // passed through as is
  world.companionReason = nullptr;
  clearRecords();
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, kEnd});
  EXPECT_REFUSAL("the radio companion blocks the radio");
  world.companionBlocks = false;
  clearRecords();
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, kEnd});
  EXPECT_REFUSAL("the app console server owns the Bluetooth host; run 'closeconsole' first");
  EXPECT(world.hal.ready() && !bleStackReady() && !world.fault);

  // Under the recovery transaction's own claim a faulted stack may come up;
  // the claim nests and keeps RECOVERING. The fault stays for the recovery
  // to clear after its own verification.
  startCase("S3b a lifecycle fault under RECOVERING");
  world.fault = true;
  EXPECT(bleRoleTransitionBegin(BleRoleTransition::RECOVERING));
  outerDepth = 1;
  clearRecords();
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp(), {kEnd}}));
  EXPECT(world.stateAtHalInit == BleRoleTransition::RECOVERING);
  EXPECT(world.fault && bleStackReady());
  bleRoleTransitionEnd();
  outerDepth = 0;
  EXPECT(world.roleDepth == 0 && world.roleState == BleRoleTransition::IDLE);

  // S4: a stale generation (host not absent, not ready) is normalized by the
  // checked teardown before the new host starts.
  startCase("S4 stale host refused while a ring connect is in flight");
  world.hal = halPartial();
  world.ringConnectInFlight = true;
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, "tx-init", kEnd});
  EXPECT_REFUSAL("a ring connection is still active; retry shortly");
  EXPECT(!world.fault && !bleStackReady());

  startCase("S4b stale host torn down, then the bring-up");
  world.hal = halPartial();
  world.retirementPending = true;  // not stopped yet: the teardown acknowledges it
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp, "tx-init"}, kTeardown, {"mem blestack"}, bringUp(), {kEnd}}));
  EXPECT(handshakes() == 1);  // the teardown's own
  EXPECT(bleStackReady() && world.hal.ready() && !world.retirementPending && !world.fault);

  startCase("S4c stale host teardown fails in the HAL");
  world.hal = halPartial();
  world.halDeinitOk = false;
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, "tx-init", "ring-prepare 2000", "gap-handler null", "hal-deinit", "delay 200",
                 "fault 1", kEnd});
  EXPECT_REFUSAL("host teardown incomplete; reboot required");
  EXPECT(logged("[BLE] Host teardown incomplete (phase=3 host=1->1 controller=2->2 quarantined=1)"
                " - reboot required"));
  EXPECT(world.fault && !bleStackReady());
  clearRecords();  // the fault holds every later up outside a recovery
  world.halDeinitOk = true;
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, kEnd});
  EXPECT_REFUSAL("a prior Bluetooth teardown was incomplete; reboot required");

  startCase("S4d stale host: the ring owner does not quiesce");
  world.hal = halPartial();
  world.ringQuiesces = false;
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, "tx-init", "ring-prepare 2000", kEnd});
  EXPECT_REFUSAL("the ring owner did not quiesce; retry shortly");
  EXPECT(!world.fault && world.hal.controller == Controller::Enabled);

  startCase("S4e stale host: the deinit succeeds but the host does not stop");
  world.hal = halPartial();
  world.halDeinitStops = false;
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, "tx-init", "ring-prepare 2000", "gap-handler null", "hal-deinit", "delay 200", "ack",
                 "links-host-stopped", "fault 1", kEnd});
  EXPECT_REFUSAL("host did not stop; reboot required");
  EXPECT(logged("[BLE] Host did not reach a stopped state - reboot required") && world.fault);

  startCase("S4f stale host: the phone pipe does not acknowledge the stopped host");
  world.hal = halPartial();
  world.phoneAcksStoppedHost = false;
  EXPECT(!UP());
  EXPECT_EVENTS(cat({{kUp, "tx-init"}, kTeardown, {"fault 1", kEnd}}));
  EXPECT_REFUSAL("the phone pipe did not acknowledge the stopped host; reboot required");
  EXPECT(logged("[BLE] Phone pipe did not acknowledge the stopped host - reboot required") && world.fault);

  // A ready flag can outlive its host (the host went stale under a published
  // stack). The up withdraws the flag and the scan object before the first
  // teardown call (the teardown fakes record any step that finds them).
  startCase("S4g ready over a stale host: admission closed before the teardown");
  BRING_UP();
  world.hal = halPartial();
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp, "tx-init"}, kTeardown, {"mem blestack"}, bringUp(), {kEnd}}));
  EXPECT(world.readyDuringTeardown.empty() && handshakes() == 1);
  EXPECT(bleStackReady() && bleStackScan() == &world.scan);

  startCase("S4h ready over a stale host whose teardown fails");
  BRING_UP();
  world.hal = halPartial();
  world.halDeinitOk = false;
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, "tx-init", "ring-prepare 2000", "gap-handler null", "hal-deinit", "delay 200",
                 "fault 1", kEnd});
  EXPECT(world.fault && !bleStackReady() && bleStackScan() == nullptr);

  // S5: the stack's own memory budget is checked before any host init.
  startCase("S5 not enough internal DRAM for the stack");
  world.memoryOk = false;
  EXPECT(!UP());
  EXPECT_EVENTS({kUp, "tx-init", "mem blestack", kEnd});
  EXPECT_REFUSAL("not enough internal DRAM for the Bluetooth host");
  EXPECT(logged("[BLE] Stack start refused: blestack needs 52000 B internal, 41000 B free"));
  EXPECT(logged("[BLE] Free internal DRAM first ('closehttp' / 'closewifi') or reboot."));
  EXPECT(world.hal.stopped() && !world.fault && !bleStackReady());
  world.memoryOk = true;  // a successful up clears the refusal it followed
  clearRecords();
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp(), {kEnd}}));
  EXPECT_REFUSAL("");

  // An absent host has nothing for the pipe to acknowledge: the up never
  // asks, even with a pipe that would refuse.
  startCase("S5b an absent-host up makes no pipe handshake");
  world.phoneAcksStoppedHost = false;
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp(), {kEnd}}));
  EXPECT(handshakes() == 0 && bleStackReady());

  // S6: a host that came up only partly is rolled back.
  startCase("S6 host init fails");
  world.halInitOk = false;
  EXPECT(!UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, {"hal-init HardwareOne"}, kRollback, {kEnd}}));
  EXPECT_REFUSAL("BLE host initialization failed");
  EXPECT(logged("[BLE] BLE host initialization failed (rolled back)"));
  EXPECT(world.hal.stopped() && !world.fault && !bleStackReady());
  world.halInitOk = true;  // the retry starts a fresh host from the stopped one
  clearRecords();
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp(), {kEnd}}));
  EXPECT_REFUSAL("");

  startCase("S6b host init fails and the rollback teardown is incomplete");
  world.halInitOk = false;
  world.halDeinitOk = false;
  EXPECT(!UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, {"hal-init HardwareOne", "gap-handler null", "hal-deinit", "fault 1", kEnd}}));
  EXPECT_REFUSAL("BLE host initialization failed");
  EXPECT(logged("[BLE] BLE host initialization failed (teardown incomplete - reboot required)"));
  EXPECT(world.fault && !bleStackReady());

  startCase("S6c the setup worker does not start");
  world.setupStartOk = false;
  EXPECT(!UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, hostUp(), kScanDefaults, kConsole, {"setup-start"}, kRollback, {kEnd}}));
  EXPECT_REFUSAL("BLE setup worker unavailable (memory)");
  EXPECT(logged("[BLE] BLE setup worker start failed (rolled back)"));
  EXPECT(!bleStackReady() && bleStackScan() == nullptr && world.hal.stopped() && !world.fault);

  startCase("S6d no scan object");
  world.scanAvailable = false;
  EXPECT(!UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, hostUp(), {"get-scan"}, kRollback, {kEnd}}));
  EXPECT_REFUSAL("BLE scan object unavailable");
  EXPECT(logged("[BLE] BLE scan object failed (rolled back)"));
  EXPECT(!bleStackReady() && world.hal.stopped());

  // T: the TX power setting is applied on the 0..7 scale where the backend
  // can; a failure is reported and the stack still comes up.
  startCase("T1 TX power above the scale");
  gSettings.bleTxPower = 9;
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp({"tx-power 7"}), {kEnd}}));
  startCase("T2 TX power below the scale");
  gSettings.bleTxPower = -2;
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp({"tx-power 0"}), {kEnd}}));
  startCase("T3 TX power refused by the controller");
  world.txPowerResult = ESP_FAIL;
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp(), {kEnd}}));
  EXPECT(logged("[BLE] TX power setting failed: ESP_FAIL") && bleStackReady());

  // S7: down refuses while anything runs on the host, server first, and
  // leaves the ready flag as it was.
  startCase("S7 down refusals in order, ready kept");
  BRING_UP();
  world.serverUp = world.glassesOpen = world.ringOpen = world.phoneActive = true;
  bool* const owners[] = {&world.serverUp, &world.glassesOpen, &world.ringOpen, &world.phoneActive};
  const char* const refusals[] = {"the app console server is running; 'closeconsole' stops it",
                                  "the glasses are open; run 'closeg2' first",
                                  "the ring is open; run 'closering' first",
                                  "the phone pipe is open; run 'closephone' first"};
  for (int i = 0; i < 4; ++i) {
    clearRecords();
    EXPECT(!DOWN());
    EXPECT_EVENTS({kDown, kEnd});
    EXPECT_REFUSAL(refusals[i]);
    EXPECT(bleStackReady() && bleStackScan() == &world.scan && world.hal.ready());
    *owners[i] = false;
  }

  startCase("S7b down from an absent host");
  EXPECT(DOWN());
  EXPECT_EVENTS(cat({{kDown, "phone-host-stopped"}, kRelease, {kEnd}}));
  EXPECT(!bleStackReady() && bleStackScan() == nullptr && world.hal.stopped());

  startCase("S7c down from a Hosted host that never ran");
  world.hal = halHostedNeverStarted();
  EXPECT(DOWN());
  EXPECT_EVENTS(cat({{kDown, "phone-host-stopped"}, kRelease, {kEnd}}));

  // Admission closes before the host goes (the teardown fakes check it); the
  // phone worker (pipe builds only) and then the setup worker are released
  // after the host stopped.
  startCase("S7d down from a ready stack");
  BRING_UP();
  EXPECT(DOWN());
  EXPECT_EVENTS(cat({{kDown}, kTeardown, kRelease, {kEnd}}));
  EXPECT(countOf("phone-worker-release 1500") == ENABLE_BLE_PHONE_PIPE);
  EXPECT(!bleStackReady() && bleStackScan() == nullptr && world.hal.stopped() && !world.fault);
  EXPECT_REFUSAL("");

  startCase("S7e a busy setup worker stays parked; down still succeeds");
  BRING_UP();
  world.setupReleaseOk = false;
  EXPECT(DOWN());
  EXPECT_EVENTS(cat({{kDown}, kTeardown, kRelease, {kEnd}}));
  EXPECT(logged("[BLE] Setup worker stayed parked (busy); the next open reuses it"));
  EXPECT(!bleStackReady() && world.hal.stopped());

  // A busy phone worker is kept: the down still succeeds and still releases
  // the setup worker. A pipe-less build's stub has no worker to keep.
  startCase("S7i a busy phone worker stays parked; down still succeeds");
  BRING_UP();
  world.phoneWorkerReleaseOk = false;
  EXPECT(DOWN());
  EXPECT_EVENTS(cat({{kDown}, kTeardown, kRelease, {kEnd}}));
  EXPECT(logged("[BLE] Phone worker stayed parked (busy); the next open reuses it") == ENABLE_BLE_PHONE_PIPE);
  EXPECT(!logged("[BLE] Setup worker stayed parked"));
  EXPECT(!bleStackReady() && world.hal.stopped() && !world.fault);
  EXPECT_REFUSAL("");

  startCase("S7f the down teardown fails in the HAL");
  BRING_UP();
  world.halDeinitOk = false;
  EXPECT(!DOWN());
  EXPECT_EVENTS({kDown, "ring-prepare 2000", "gap-handler null", "hal-deinit", "delay 200", "fault 1", kEnd});
  EXPECT_REFUSAL("host teardown incomplete; reboot required");
  EXPECT(world.fault && !bleStackReady() && bleStackScan() == nullptr);

  // A down the ring owner refuses keeps admission closed and the host up;
  // the next up normalizes that host before a fresh bring-up.
  startCase("S7g the ring owner refuses the down");
  BRING_UP();
  world.ringQuiesces = false;
  EXPECT(!DOWN());
  EXPECT_EVENTS({kDown, "ring-prepare 2000", kEnd});
  EXPECT_REFUSAL("the ring owner did not quiesce; retry shortly");
  EXPECT(!world.fault && !bleStackReady() && world.hal.ready());
  world.ringQuiesces = true;
  clearRecords();
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp, "tx-init"}, kTeardown, {"mem blestack"}, bringUp(), {kEnd}}));

  // A verified down clears a fault an earlier incomplete teardown left.
  startCase("S7h a verified down after an incomplete teardown");
  world.hal = halPartial();
  world.fault = true;
  EXPECT(DOWN());
  EXPECT_EVENTS(cat({{kDown}, kTeardown, kRelease, {kEnd}}));
  EXPECT(!world.fault && world.hal.stopped());

  // S8: a second down finds the host absent; an up after it starts a fresh
  // generation.
  startCase("S8 down twice, then up");
  BRING_UP();
  EXPECT(DOWN());
  clearRecords();
  EXPECT(DOWN());
  EXPECT_EVENTS(cat({{kDown, "phone-host-stopped"}, kRelease, {kEnd}}));
  clearRecords();
  EXPECT(UP());
  EXPECT_EVENTS(cat({{kUp}, kAdmit, bringUp(), {kEnd}}));

  // S9: the companion lost the host under us: the flag and the scan object
  // go at once, with no boundary call; a later down still runs the checked
  // teardown when the HAL reports a host.
  startCase("S9 host lost");
  BRING_UP();
  bleStackNoteHostLost();
  EXPECT(world.events.empty());
  EXPECT(!bleStackReady() && bleStackScan() == nullptr);
  EXPECT(DOWN());
  EXPECT_EVENTS(cat({{kDown}, kTeardown, kRelease, {kEnd}}));

  // S10: another task's role transition refuses both, with no other call.
  startCase("S10 role claim busy");
  BRING_UP();
  world.roleHeldByOtherTask = true;
  EXPECT(!UP());
  EXPECT_EVENTS({"role-busy STACK_UP"});
  EXPECT_REFUSAL("another BLE role transition is active");
  clearRecords();
  EXPECT(!DOWN());
  EXPECT_EVENTS({"role-busy STACK_DOWN"});
  EXPECT_REFUSAL("another BLE role transition is active");
  EXPECT(bleStackReady() && bleStackScan() == &world.scan && world.hal.ready());

  std::printf("BLE central stack [G2=%d R1=%d PIPE=%d COEX=%d]: %d cases, %d failures\n",
              ENABLE_G2_GLASSES, ENABLE_R1_HEALTH, ENABLE_BLE_PHONE_PIPE, ENABLE_BLE_COEX_RECORDS,
              caseCount, failures);
  return failures == 0 ? 0 : 1;
}
