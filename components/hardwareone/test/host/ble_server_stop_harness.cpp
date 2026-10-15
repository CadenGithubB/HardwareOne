// Production definitions are inserted by test_ble_server_stop.py: the BLE
// server stop (deinitBluetooth), its session/advertising helpers and the
// `closeble` / `blemode` commands, extracted unchanged from Bluetooth.cpp.
// Role claims, the Ring owner, the host/controller teardown, transport
// sessions and settings writes are host boundaries.
#include "Arduino.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

// INSERT_TYPES

// ---------------------------------------------------------------------------
// Host boundaries
// ---------------------------------------------------------------------------
struct Boundary {
  bool roleBusy = false;      // another task holds a BLE role transition
  bool ringQuiesces = true;   // g2RingPrepareForStackTeardown() result
  bool halStops = true;       // bluetoothHalDeinit() result
  bool g2Client = false;      // isG2ClientInitialized()
  bool fault = false;
  bool phoneOnly = false, phoneStops = true;
  unsigned phoneStopCalls = 0, phoneHostAcks = 0;         // BLE lifecycle fault flag
  bool lockedTeardown = false;
  unsigned ringPrepares = 0, halDeinits = 0, advStops = 0, settingWrites = 0;
  unsigned sessionCloses = 0, bleOffEvents = 0, centralAcks = 0;
  std::vector<std::string> events, messages;
} boundary;

using TickType_t = uint32_t;
using BaseType_t = int;
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(uint32_t) {}

struct HostRecursiveMutex { int depth = 0; };
using SemaphoreHandle_t = HostRecursiveMutex*;
static HostRecursiveMutex gLifecycleMutex;
static SemaphoreHandle_t sBleLifecycleMutex = &gLifecycleMutex;
static BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t) {
  ++mutex->depth;
  return pdTRUE;
}
static BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
  --mutex->depth;
  return pdTRUE;
}

// The role token is recursive for its owner: a nested claim keeps the outer
// claim's published state (e.g. a companion reset's RECOVERING).
static int roleDepth = 0;
static BleRoleTransition roleState = BleRoleTransition::IDLE;
static bool bleRoleTransitionBegin(BleRoleTransition state) {
  if (boundary.roleBusy) return false;
  if (roleDepth++ == 0) roleState = state;
  return true;
}
static void bleRoleTransitionEnd() {
  if (--roleDepth == 0) roleState = BleRoleTransition::IDLE;
}
static BleRoleTransition bleRoleTransitionState() { return roleState; }
static void bleStackSetLifecycleFault(bool faulted) { boundary.fault = faulted; }
static void bleCentralClientsTerminalTeardownAcknowledged() { ++boundary.centralAcks; }

static void broadcastOutput(const char* line) { boundary.messages.emplace_back(line ? line : ""); }
#define BLE_DEBUGF(...) do {} while (0)

static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }
static const char* getBleModeString() { return "server"; }
static bool gCLIValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { if (::gCLIValidateOnly) return "VALID"; } while (0)

static struct Settings { int bleMode = BLE_MODE_SERVER; } gSettings;
template <typename T>
static void setSetting(T& field, const T& value) {
  field = value;
  ++boundary.settingWrites;
}

#if ENABLE_G2_GLASSES
static bool isG2ClientInitialized() { return boundary.g2Client; }
static bool deinitG2Client() { boundary.g2Client = false; return true; }
#else
static bool isG2ClientInitialized() { return false; }
static bool deinitG2Client() { return true; }
#endif
static bool phonePipeStandalone() { return boundary.phoneOnly; }
static bool phonePipeStop() { ++boundary.phoneStopCalls; if (boundary.phoneStops) boundary.phoneOnly=false; return boundary.phoneStops; }
static bool phonePipeHostStopped() { ++boundary.phoneHostAcks; return true; }

using TransportSessionEpoch = uint32_t;
enum HostCommandSource { SOURCE_BLUETOOTH = 1 };
static void transportSessionClose(HostCommandSource, TransportSessionEpoch epoch) {
  if (epoch == 0) return;
  ++boundary.sessionCloses;
  boundary.events.emplace_back("session-close");
}
static std::atomic<TransportSessionEpoch> sBleSessionEpoch[BLE_MAX_CONNECTIONS];

static bool g2RingPrepareForStackTeardown(uint32_t timeoutMs) {
  if (gLifecycleMutex.depth != 0) boundary.lockedTeardown = true;
  ++boundary.ringPrepares;
  boundary.events.emplace_back("ring-prepare");
  return timeoutMs > 0 && boundary.ringQuiesces;
}
static BluetoothHalShutdownResult bluetoothHalDeinit() {
  if (gLifecycleMutex.depth != 0) boundary.lockedTeardown = true;
  ++boundary.halDeinits;
  boundary.events.emplace_back("hal-deinit");
  BluetoothHalShutdownResult result{};
  result.success = boundary.halStops;
  result.phase = boundary.halStops ? 0 : 2;
  return result;
}

#define ESP_GATT_IF_NONE 0xff
static std::atomic<int> sBleGattsIf{ESP_GATT_IF_NONE};

struct BLEServer {};
struct BLEService {};
struct BLECharacteristic {};
struct BLEAdvertising {};
struct BLEDevice {
  static void stopAdvertising() {
    ++boundary.advStops;
    boundary.events.emplace_back("adv-stop");
  }
  static void setCustomGattsHandler(void*) {}
};
static BLEServer gServer;
static BLEService gService;
static BLECharacteristic gCharacteristic;
static BLEAdvertising gAdvertising;
static BLEServer* pServer = nullptr;
static BLEAdvertising* pAdvertising = nullptr;
static BLEService* pCommandService = nullptr;
static BLEService* pDataService = nullptr;
static BLEService* pDeviceInfoService = nullptr;
static BLECharacteristic* pCmdRequestChar = nullptr;
static BLECharacteristic* pCmdResponseChar = nullptr;
static BLECharacteristic* pCmdStatusChar = nullptr;
static BLECharacteristic* pSensorDataChar = nullptr;
static BLECharacteristic* pSystemStatusChar = nullptr;
static BLECharacteristic* pEventNotifyChar = nullptr;
static BLECharacteristic* pStreamControlChar = nullptr;
static BLECharacteristic* pManufacturerChar = nullptr;
static BLECharacteristic* pModelChar = nullptr;
static BLECharacteristic* pFirmwareChar = nullptr;

BLESystemState* gBLEState = nullptr;
template <typename T>
static void ps_delete(T* object) {
  if (!object) return;
  object->~T();
  std::free(object);
}
static int sBLEToggleCount = 0;
static size_t sBLEHeapBeforeInit = 0;
static struct HostEsp { size_t getFreeHeap() const { return 200000; } } ESP;
enum HostSystemEvent { SYSEVT_BLE_OFF = 1 };
static void systemEventPost(HostSystemEvent) {
  ++boundary.bleOffEvents;
  boundary.events.emplace_back("ble-off");
}

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static unsigned assertions = 0, failures = 0;
static const char* currentCase = "";
#define EXPECT(condition, message)                                   \
  do {                                                               \
    ++assertions;                                                    \
    if (!(condition)) {                                              \
      ++failures;                                                    \
      std::printf("FAIL [%s]: %s\n", currentCase, message);          \
    }                                                                \
  } while (0)

// A void deinitBluetooth() cannot tell its caller the stop was deferred; the
// harness reads that as "reported stopped".
static bool deinitReported(void (*stop)()) { stop(); return true; }
static bool deinitReported(bool (*stop)()) { return stop(); }

static void dropServerState() {
  if (gBLEState) {
    ps_delete(gBLEState);
    gBLEState = nullptr;
  }
}

static void resetWorld(const char* name) {
  currentCase = name;
  dropServerState();
  boundary = Boundary{};
  for (auto& epoch : sBleSessionEpoch) epoch.store(0);
  gSettings.bleMode = BLE_MODE_SERVER;
  sBleGattsIf.store(ESP_GATT_IF_NONE);
  pServer = nullptr;
  pAdvertising = nullptr;
  pCmdResponseChar = nullptr;
}

// A running server: advertising, two app clients with live session epochs.
static void startServer() {
  void* storage = std::malloc(sizeof(BLESystemState));
  if (!storage) std::abort();
  gBLEState = new (storage) BLESystemState{};
  gBLEState->initialized = true;
  gBLEState->connectionState = BLE_STATE_ADVERTISING;
  gBLEState->connections[0].active = true;
  gBLEState->connections[0].connId = 7;
  gBLEState->connections[1].active = true;
  gBLEState->connections[1].connId = 9;
  gBLEState->activeConnectionCount = 2;
  sBleSessionEpoch[0].store(41);
  sBleSessionEpoch[1].store(42);
  sBleGattsIf.store(3);
  pServer = &gServer;
  pAdvertising = &gAdvertising;
  pCommandService = pDataService = pDeviceInfoService = &gService;
  pCmdRequestChar = pCmdResponseChar = pCmdStatusChar = &gCharacteristic;
  pSensorDataChar = pSystemStatusChar = pEventNotifyChar = &gCharacteristic;
  pStreamControlChar = pManufacturerChar = pModelChar = pFirmwareChar = &gCharacteristic;
}

static bool said(const char* fragment) {
  for (const auto& line : boundary.messages) {
    if (line.find(fragment) != std::string::npos) return true;
  }
  return false;
}
static bool isError(const char* reply) { return reply && std::strncmp(reply, "Error", 5) == 0; }
static bool sessionsLive() {
  return bleSessionEpochMatches(7, 41) && bleSessionEpochMatches(9, 42) &&
         boundary.sessionCloses == 0;
}
static size_t eventAt(const char* name) {
  for (size_t i = 0; i < boundary.events.size(); ++i) {
    if (boundary.events[i] == name) return i;
  }
  return SIZE_MAX;
}
static void expectReleased() {
  EXPECT(roleDepth == 0, "role transition claim leaked");
  EXPECT(gLifecycleMutex.depth == 0, "lifecycle mutex leaked");
  EXPECT(!boundary.lockedTeardown, "Ring prepare / host teardown ran under the lifecycle mutex");
}

static void expectServerStoppedCleanly() {
  EXPECT(gBLEState == nullptr, "server state was not freed");
  EXPECT(pServer == nullptr && pAdvertising == nullptr && pCmdResponseChar == nullptr,
         "GATT object pointers survived the stop");
  EXPECT(sBleSessionEpoch[0].load() == 0 && sBleSessionEpoch[1].load() == 0 &&
         boundary.sessionCloses == 2, "app sessions were not closed by the stop");
  EXPECT(sBleGattsIf.load() == ESP_GATT_IF_NONE, "GATT interface id survived the stop");
  EXPECT(boundary.halDeinits == 1 && boundary.centralAcks == 1 && !boundary.fault,
         "host teardown did not complete");
  EXPECT(boundary.bleOffEvents == 1 && said("[BLE] Deinitialized"), "stop was not announced");
}

int main() {
  // S1: Ring owner still active.
  resetWorld("S1 closeble, Ring owner busy");
  startServer();
  boundary.ringQuiesces = false;
  const char* reply = cmd_blestop("");
  EXPECT(isError(reply),
         "closeble reported success although the Ring owner deferred the server stop");
  EXPECT(gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 0,
         "closeble persisted the default role although the server is still running");
  EXPECT(isBleServerInitialized(), "a deferred stop dropped the server state");
  EXPECT(sessionsLive(),
         "a deferred stop closed the connected clients' sessions (their commands are dropped "
         "until they reconnect)");
  EXPECT(gBLEState && gBLEState->connectionState == BLE_STATE_ADVERTISING &&
         boundary.advStops == 0, "a deferred stop stopped advertising");
  EXPECT(boundary.halDeinits == 0, "host torn down although the Ring owner did not quiesce");
  EXPECT(said("Ring owner did not quiesce"), "deferred reason was not broadcast");
  expectReleased();
  currentCase = "S1 closeble retry after the Ring owner quiesced";
  boundary.ringQuiesces = true;
  boundary.messages.clear();
  boundary.events.clear();
  reply = cmd_blestop("");
  EXPECT(!isError(reply) && std::strcmp(reply, "Bluetooth stopped") == 0,
         "a retried closeble did not report the stop");
  EXPECT(gSettings.bleMode == kBleModeDefaultForBuild, "retried closeble did not persist the default role");
  expectServerStoppedCleanly();
  expectReleased();

  resetWorld("S1 deinitBluetooth, Ring owner busy");
  startServer();
  boundary.ringQuiesces = false;
  EXPECT(!deinitReported(deinitBluetooth),
         "deinitBluetooth reported a stop that the Ring owner deferred");
  expectReleased();

  // C1: companion reset (System_RadioCompanion shutDownBleForCompanionReset):
  // the outer RECOVERING claim, then deinitBluetooth(), retried a bounded
  // number of times; the host is never forced down while the Ring owner is
  // busy (companion_ble_shutdown). If no attempt completes, the companion
  // reboots under a host left up, so no link survives: a Ring-deferred stop
  // must close session admission and advertising itself, or a directed sender
  // queues a result into a link that is about to vanish.
  resetWorld("C1 companion reset (RECOVERING), Ring owner busy");
  startServer();
  boundary.ringQuiesces = false;
  EXPECT(bleRoleTransitionBegin(BleRoleTransition::RECOVERING), "test setup: RECOVERING claim");
  EXPECT(!deinitReported(deinitBluetooth),
         "deinitBluetooth reported a stop that the Ring owner deferred");
  EXPECT(sBleSessionEpoch[0].load() == 0 && sBleSessionEpoch[1].load() == 0 &&
         !bleSessionEpochMatches(7, 41) && !bleSessionEpochMatches(9, 42) &&
         boundary.sessionCloses == 2,
         "a Ring-deferred stop under the companion reset left the app session epochs published "
         "(the companion then reboots under the host, so those links are about to vanish)");
  EXPECT(sBleGattsIf.load() == ESP_GATT_IF_NONE,
         "a Ring-deferred stop under the companion reset left the GATT interface id set");
  EXPECT(boundary.advStops == 1, "a Ring-deferred stop under the companion reset kept advertising");
  EXPECT(boundary.halDeinits == 0,
         "deinitBluetooth tore the host down although the Ring owner did not quiesce");
  EXPECT(said("Ring owner did not quiesce"), "deferred reason was not broadcast");
  bleRoleTransitionEnd();
  expectReleased();

  // C2: any other outer claim (initG2Client's G2_START re-checks the server
  // and aborts) keeps the server running with its sessions, as S1.
  resetWorld("C2 nested stop under G2_START, Ring owner busy");
  startServer();
  boundary.ringQuiesces = false;
  EXPECT(bleRoleTransitionBegin(BleRoleTransition::G2_START), "test setup: G2_START claim");
  EXPECT(!deinitReported(deinitBluetooth),
         "deinitBluetooth reported a stop that the Ring owner deferred");
  EXPECT(sessionsLive() && sBleGattsIf.load() == 3 && boundary.advStops == 0,
         "a Ring-deferred stop outside a companion reset closed the server's sessions or advertising");
  bleRoleTransitionEnd();
  expectReleased();

  // S2: another BLE role transition is active.
  resetWorld("S2 closeble, role transition busy");
  startServer();
  boundary.roleBusy = true;
  reply = cmd_blestop("");
  EXPECT(isError(reply), "closeble reported success while another role transition was active");
  EXPECT(gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 0,
         "closeble persisted the default role while another role transition was active");
  EXPECT(sessionsLive() && boundary.ringPrepares == 0 && boundary.advStops == 0,
         "a busy role transition still touched the server");
  EXPECT(said("another BLE role transition is active"), "busy reason was not broadcast");
  EXPECT(!deinitReported(deinitBluetooth),
         "deinitBluetooth reported a stop while another role transition was active");
  expectReleased();

  // S3: the host/controller teardown did not complete.
  resetWorld("S3 closeble, host teardown incomplete");
  startServer();
  boundary.halStops = false;
  reply = cmd_blestop("");
  EXPECT(isError(reply), "closeble reported success although the host teardown failed");
  EXPECT(gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 0,
         "closeble persisted the default role although the host teardown failed");
  EXPECT(boundary.fault && said("reboot required"), "teardown failure was not latched and broadcast");
  expectReleased();
  resetWorld("S3 deinitBluetooth, host teardown incomplete");
  startServer();
  boundary.halStops = false;
  EXPECT(!deinitReported(deinitBluetooth),
         "deinitBluetooth reported a stop although the host teardown failed");
  expectReleased();

  // S4: a clean stop keeps its order and outcome.
  resetWorld("S4 closeble success");
  startServer();
  reply = cmd_blestop("");
  EXPECT(std::strcmp(reply, "Bluetooth stopped") == 0, "closeble success reply changed");
  EXPECT(gSettings.bleMode == kBleModeDefaultForBuild && boundary.settingWrites == 1,
         "closeble did not persist the build's default role");
  expectServerStoppedCleanly();
  const size_t ring = eventAt("ring-prepare"), session = eventAt("session-close");
  const size_t adv = eventAt("adv-stop"), hal = eventAt("hal-deinit");
  EXPECT(ring != SIZE_MAX && session != SIZE_MAX && adv != SIZE_MAX && hal != SIZE_MAX,
         "stop skipped a teardown step");
  EXPECT(ring < session,
         "Ring prepare must run before app session admission closes");
  EXPECT(session < adv && adv < hal, "sessions, advertising and host stop order changed");
  expectReleased();

  // S5: nothing running.
  resetWorld("S5 closeble, server not running");
  reply = cmd_blestop("");
  EXPECT(std::strcmp(reply, "Bluetooth stopped") == 0 &&
         gSettings.bleMode == kBleModeDefaultForBuild && boundary.settingWrites == 1,
         "closeble without a server no longer persists the default role");
  EXPECT(boundary.ringPrepares == 0 && boundary.halDeinits == 0,
         "closeble without a server touched the Ring owner or the host");
  EXPECT(deinitReported(deinitBluetooth), "deinitBluetooth without a server must report stopped");
  expectReleased();

#if ENABLE_G2_GLASSES
  // M1: blemode client cannot stop the server.
  resetWorld("M1 blemode client, Ring owner busy");
  startServer();
  boundary.ringQuiesces = false;
  reply = cmd_blemode("client");
  EXPECT(isError(reply),
         "blemode client reported the client role although the server stop was deferred");
  EXPECT(gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 0,
         "blemode client persisted the client role while the server is still running");
  EXPECT(isBleServerInitialized() && sessionsLive(),
         "a deferred server stop for blemode client closed the app sessions");
  expectReleased();
  resetWorld("M1 blemode client, role transition busy");
  startServer();
  boundary.roleBusy = true;
  reply = cmd_blemode("1");
  EXPECT(isError(reply) && gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 0,
         "blemode 1 persisted the client role while another role transition was active");
  expectReleased();

  // M2: the client role is stored once the server is down.
  resetWorld("M2 blemode client success");
  startServer();
  reply = cmd_blemode("client");
  EXPECT(std::strcmp(reply, "[BLE] Mode set to client (G2)") == 0 &&
         gSettings.bleMode == BLE_MODE_G2_CLIENT, "blemode client success changed");
  expectServerStoppedCleanly();
  expectReleased();
  resetWorld("M2 blemode client, server not running");
  reply = cmd_blemode("g2");
  EXPECT(std::strcmp(reply, "[BLE] Mode set to client (G2)") == 0 &&
         gSettings.bleMode == BLE_MODE_G2_CLIENT && boundary.ringPrepares == 0,
         "blemode client without a server changed");
  expectReleased();
#else
  // M3: client role compiled out.
  resetWorld("M3 blemode client, G2 compiled out");
  startServer();
  reply = cmd_blemode("client");
  EXPECT(std::strncmp(reply, "Error: [BLE] G2 client not compiled", 35) == 0 &&
         boundary.settingWrites == 0 && isBleServerInitialized() && sessionsLive(),
         "blemode client in a build without G2 changed");
  expectReleased();
#endif

  // blemode server is untouched by the server stop result.
  resetWorld("blemode server");
  reply = cmd_blemode("server");
  EXPECT(std::strcmp(reply, "[BLE] Mode set to server") == 0 &&
         gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 1,
         "blemode server changed");
  expectReleased();

  resetWorld("phone stop blocks host teardown");
  startServer(); boundary.phoneStops=false;
  EXPECT(!deinitBluetooth() && boundary.halDeinits==0 && sessionsLive(),
         "host was torn down before the phone owner quiesced");
  expectReleased();
  resetWorld("phone-only stop"); boundary.phoneOnly=true;
  EXPECT(deinitBluetooth() && boundary.halDeinits==1 && boundary.phoneHostAcks==1,
         "phone-only stop did not retire its host and acknowledge teardown");
  expectReleased();
  resetWorld("phone-only mode choice");
  reply=cmd_blemode("phone-client");
#if ENABLE_BLE_PHONE_PIPE
  EXPECT(!isError(reply) && gSettings.bleMode==BLE_MODE_PHONE_CLIENT,
         "phone-only mode must be selectable without G2");
#else
  EXPECT(isError(reply) && gSettings.bleMode==BLE_MODE_SERVER,
         "an unavailable phone mode must not be persisted");
#endif
  expectReleased();
  resetWorld("legacy phone alias stays Server");
  reply=cmd_blemode("phone");
  EXPECT(!isError(reply) && gSettings.bleMode==BLE_MODE_SERVER, "legacy phone alias changed");
  expectReleased();
  dropServerState();
  std::printf("ble server stop (G2=%d): %u assertions, %u failures\n",
              ENABLE_G2_GLASSES, assertions, failures);
  return failures == 0 ? 0 : 1;
}
