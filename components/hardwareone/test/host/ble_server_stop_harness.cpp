// Production definitions are inserted by test_ble_server_stop.py: the BLE
// server stop (deinitBluetooth), its session/advertising helpers and the
// `closeble` / `blemode` commands, extracted unchanged from Bluetooth.cpp, with
// bleLinkStatus() from BLE_LinkStatus.cpp for the `blemode` words.
// Role claims, the Ring owner, the host/controller teardown, transport
// sessions, settings writes and the pipe/console views are host boundaries.
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
  bool pipeBesideGlasses = false;  // phonePipeActive() without owning the host
  unsigned phoneStopCalls = 0, phoneHostAcks = 0;         // BLE lifecycle fault flag
  bool lockedTeardown = false;
  unsigned ringPrepares = 0, halDeinits = 0, advStops = 0, settingWrites = 0;
  unsigned sessionCloses = 0, bleOffEvents = 0, centralAcks = 0;
  // The client host (bleConsoleRegisterOnClientHost) and ServerCallbacks.
  bool hostReady = true;              // bluetoothHalStatus().ready()
  bool stackReady = false;            // bleStackReady(): the shared client stack is up
  bool linkHeld[4] = {};              // bleCentralLinkHeld(Left, Right, Ring, Phone)
  int regStatus = 0;                  // ESP_GATTS_REG_EVT status
  bool regLate = false;               // the tap sees REG only after createServer returns
  bool regNever = false;              // ... or never
  unsigned createServers = 0, createServices = 0, serviceStarts = 0, mtuSets = 0;
  unsigned advGets = 0, advStarts = 0, txPowerSets = 0, scInits = 0, scWarms = 0;
  unsigned sessionOpens = 0, connectedEvents = 0, disconnectedEvents = 0;
  std::vector<std::string> services;
  // The console as bluetoothHalDeinit found it, between the two hooks.
  struct AtStop {
    bool seen = false, stateAlive = false, admissionOpen = true, pointersNull = false;
    bool gattsIfNone = false, tapRemoved = false, epochsClosed = false;
  } atStop;
  std::vector<std::string> events, messages;
} boundary;

using TickType_t = uint32_t;
using BaseType_t = int;
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static uint32_t hostNowMs = 1000;
static uint32_t millis() { return hostNowMs; }
static void deliverLateRegistration();
// A delay lets BTC run: a registration the tap has not seen yet arrives now.
static void vTaskDelay(uint32_t ticks) {
  hostNowMs += ticks ? ticks : 1;
  deliverLateRegistration();
}

struct HostRecursiveMutex { int depth = 0; };
using SemaphoreHandle_t = HostRecursiveMutex*;
using TaskHandle_t = void*;
static HostRecursiveMutex gLifecycleMutex;
static SemaphoreHandle_t sBleLifecycleMutex = &gLifecycleMutex;
static bool bleInitLifecycleMutex() { return sBleLifecycleMutex != nullptr; }
// The leaf lock of the phone link's console row snapshot (one task here), and
// `outble`, which ends with the last signed-in session.
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
constexpr uint32_t MSG_ROUTE_BLE = 0x10;
static volatile uint32_t gOutputFlags = MSG_ROUTE_BLE;
static int gThisTask = 0;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return &gThisTask; }
// One task in this harness: the holder is this task while it holds the lock.
static TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t mutex) {
  return mutex && mutex->depth > 0 ? &gThisTask : nullptr;
}
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
static bool bleRoleTransitionHeldByCurrentTask() { return roleDepth > 0; }
static void bleStackSetLifecycleFault(bool faulted) { boundary.fault = faulted; }
static void bleCentralClientsTerminalTeardownAcknowledged() { ++boundary.centralAcks; }

static void broadcastOutput(const char* line) { boundary.messages.emplace_back(line ? line : ""); }
#define BLE_DEBUGF(...) do {} while (0)
#define BROADCAST_PRINTF(...)                              \
  do {                                                     \
    char line[256];                                        \
    std::snprintf(line, sizeof(line), __VA_ARGS__);        \
    broadcastOutput(line);                                 \
  } while (0)

// The client host's other inputs.
enum class BleCentralLink : uint8_t { Left, Right, Ring, Phone };
static bool bleCentralLinkHeld(BleCentralLink link) {
  return boundary.linkHeld[static_cast<unsigned>(link)];
}
struct HostHalStatus { bool ready() const { return boundary.hostReady; } };
static HostHalStatus bluetoothHalStatus() { return {}; }
static void bleScInit() { ++boundary.scInits; boundary.events.emplace_back("sc-init"); }
static void bleScWarmPsk() { ++boundary.scWarms; boundary.events.emplace_back("sc-warm"); }
static void bleScReset(uint16_t) {}
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::calloc(1, size); }

static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }
static bool gCLIValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { if (::gCLIValidateOnly) return "VALID"; } while (0)

static struct Settings {
  int bleMode = BLE_MODE_SERVER;
  bool bleConsoleOnPipe = true;
  String bleSecureChannelSecret;
} gSettings;
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
// The pipe's link record (BLE_PhonePipe.h), read by the phone-link console.
static PhonePipeLinkRecord gPipeRecord;
static bool gPipeRecordValid = false;
static bool phonePipeLinkRecord(PhonePipeLinkRecord* out) {
  if (out) *out = gPipeRecordValid ? gPipeRecord : PhonePipeLinkRecord{};
  return gPipeRecordValid;
}

// Link status inputs (BLE_LinkStatus.cpp): the pipe runs alone (phone-client)
// or beside the glasses; the console view follows the server state.
static bool phonePipeActive() { return boundary.phoneOnly || boundary.pipeBesideGlasses; }
// The shared client stack (BLE_CentralStack.cpp); blemode names it as the running role.
static bool bleStackReady() { return boundary.stackReady; }
static bool phonePipeReady() { return false; }
static bool phonePipeEnrolling() { return false; }
static uint8_t phonePipeStep() { return uint8_t(hw1link::PipeStep::Scanning); }
static const char* phonePipeStatus() { return "scanning"; }
// The phone link block's inputs (phone link v2): nothing offered here.
static hw1link::PhoneLinkInputs bleLinkPhoneInputs() { return {}; }
#if ENABLE_BLE_PHONE_PIPE
static bool blePeerReconnectSnapshot(BlePeerKind kind, BlePeerReconnectSnapshot& out) {
  out = BlePeerReconnectSnapshot{};
  out.hasSavedTarget = kind == BLE_PEER_PHONE;
  return true;
}
#endif

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
void bleConsoleHostTeardownBegin();
void bleConsoleHostTeardownEnd(bool stopped);
static void snapshotConsoleAtStop();
static void destroyGattTree();
// Mirrors HAL_Bluetooth.cpp: the console's hooks wrap every stop
// (test_hal_bluetooth checks the real HAL calls them in this order). A
// complete stop deletes Arduino's GATT tree with the host.
static BluetoothHalShutdownResult bluetoothHalDeinit() {
  if (gLifecycleMutex.depth != 0) boundary.lockedTeardown = true;
  bleConsoleHostTeardownBegin();
  ++boundary.halDeinits;
  boundary.events.emplace_back("hal-deinit");
  snapshotConsoleAtStop();
  BluetoothHalShutdownResult result{};
  result.success = boundary.halStops;
  result.phase = boundary.halStops ? 0 : 2;
  if (result.success) destroyGattTree();
  bleConsoleHostTeardownEnd(result.success);
  return result;
}
static bool bluetoothHalSupportsTxPower() { return true; }
static int bluetoothHalSetTxPower(uint8_t) { ++boundary.txPowerSets; return 0; }

// IDF GATTS surface used by the tap and ServerCallbacks.
#define ESP_GATT_IF_NONE 0xff
#define ESP_GATT_OK 0
#define ESP_APP_ID_MAX 0x7fff
#define ESP_GATT_PERM_READ (1 << 0)
#define ESP_GATT_PERM_WRITE (1 << 4)
using esp_gatt_if_t = uint8_t;
enum esp_gatts_cb_event_t { ESP_GATTS_REG_EVT, ESP_GATTS_UNREG_EVT, ESP_GATTS_CONNECT_EVT };
struct esp_ble_gatts_cb_param_t {
  struct { int status; uint16_t app_id; } reg;
  struct { uint16_t conn_id; uint8_t remote_bda[6]; } connect;
  struct { uint16_t conn_id; } disconnect;
};
static std::atomic<int> sBleGattsIf{ESP_GATT_IF_NONE};
static std::atomic<bool> sBleAdvertising{false};

// Arduino BLE objects: each records what the console builds on the host.
struct BLE2902 {};
struct BLECharacteristic {
  static constexpr uint32_t PROPERTY_READ = 1, PROPERTY_WRITE = 2, PROPERTY_NOTIFY = 4,
                            PROPERTY_WRITE_NR = 8;
  std::string uuid;
  uint32_t properties = 0;
  uint16_t permissions = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;  // Arduino's default
  bool preparedWrites = true;
  const void* callbacks = nullptr;
  unsigned descriptors = 0;
  bool descriptorOwned = false;
  void setAccessPermissions(uint16_t perm) { permissions = perm; }
  void setPreparedWriteEnabled(bool enabled) { preparedWrites = enabled; }
  void setCallbacks(const void* cb) { callbacks = cb; }
  void addDescriptor(BLE2902* descriptor, bool owned) {
    ++descriptors;
    descriptorOwned = owned;
    delete descriptor;  // owned: Arduino frees it with the tree
  }
};
struct BLEService {
  std::string uuid;
  std::vector<BLECharacteristic*> characteristics;
  unsigned starts = 0;
  BLECharacteristic* createCharacteristic(const char* id, uint32_t properties) {
    auto* c = new BLECharacteristic();
    c->uuid = id;
    c->properties = properties;
    characteristics.push_back(c);
    return c;
  }
  void start() {
    ++starts;
    ++boundary.serviceStarts;
    boundary.events.emplace_back("service-start");
  }
  void clear() {
    for (auto* c : characteristics) delete c;
    characteristics.clear();
    starts = 0;
  }
  ~BLEService() { clear(); }
};
struct BLEServer;
struct BLEServerCallbacks {
  virtual ~BLEServerCallbacks() = default;
  virtual void onConnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
  virtual void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
};
struct BLEServer {
  BLEServerCallbacks* callbacks = nullptr;
  std::vector<BLEService*> services;
  void setCallbacks(BLEServerCallbacks* cb) {
    callbacks = cb;
    boundary.events.emplace_back("server-callbacks");
  }
  BLEService* createService(const char* uuid) {
    ++boundary.createServices;
    boundary.services.emplace_back(uuid);
    boundary.events.emplace_back("create-service");
    auto* service = new BLEService();
    service->uuid = uuid;
    services.push_back(service);
    return service;
  }
  void disconnect(uint16_t) {}
  void clear() {
    for (auto* service : services) delete service;
    services.clear();
    callbacks = nullptr;
  }
};
struct BLEAdvertising {
  bool start() {
    ++boundary.advStarts;
    boundary.events.emplace_back("adv-start");
    return true;
  }
};
using GattsHandler = void (*)(esp_gatts_cb_event_t, esp_gatt_if_t, esp_ble_gatts_cb_param_t*);
static BLEServer gServer;
static BLEService gService;
static BLECharacteristic gCharacteristic;
static BLEAdvertising gAdvertising;
struct BLEDevice {
  static inline uint16_t m_appId = 3;
  static inline GattsHandler gattsHandler = nullptr;
  static inline bool registrationPending = false;
  // The reserved 15-bit allocator every client takes its id from (the phone
  // pipe's Arduino patch): masked, skipping reserved ids.
  static inline std::vector<uint16_t> reserved;
  static inline bool idsExhausted = false;
  static inline int registeredAppId = -1;  // the id createServer registered
  static bool allocateAppId(uint16_t& id) {
    if (idsExhausted) return false;
    for (unsigned tried = 0; tried < 0x8000; ++tried) {
      const uint16_t candidate = m_appId++ & 0x7fff;
      bool taken = false;
      for (uint16_t r : reserved) taken = taken || r == candidate;
      if (taken) continue;
      reserved.push_back(candidate);
      id = candidate;
      return true;
    }
    return false;
  }
  static void stopAdvertising() {
    ++boundary.advStops;
    boundary.events.emplace_back("adv-stop");
  }
  static void setCustomGattsHandler(GattsHandler handler) {
    gattsHandler = handler;
    boundary.events.emplace_back(handler ? "tap-installed" : "tap-removed");
  }
  static void setMTU(uint16_t) { ++boundary.mtuSets; }
  static BLEAdvertising* getAdvertising() { ++boundary.advGets; return &gAdvertising; }
  static void startAdvertising() { ++boundary.advStarts; }
  // Arduino registers the app (m_appId++) and returns once its own handler
  // saw ESP_GATTS_REG_EVT; this file's tap sees the event after that.
  static BLEServer* createServer() {
    ++boundary.createServers;
    boundary.events.emplace_back("create-server");
    registeredAppId = m_appId++;  // Arduino registers m_appId as it stands
    registrationPending = !boundary.regNever;
    if (!boundary.regLate) deliverLateRegistration();
    return &gServer;
  }
};
static void deliverLateRegistration() {
  if (!BLEDevice::registrationPending) return;
  BLEDevice::registrationPending = false;
  if (!BLEDevice::gattsHandler) return;
  esp_ble_gatts_cb_param_t param{};
  param.reg.status = boundary.regStatus;
  BLEDevice::gattsHandler(ESP_GATTS_REG_EVT, boundary.regStatus == ESP_GATT_OK ? 4 : 0, &param);
}
static void destroyGattTree() { gServer.clear(); }
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
// The characteristic callbacks the command service installs (Bluetooth.cpp's
// own are not under test here).
static struct HostCharCallbacks {} sCmdRequestCallbacks, sCmdStatusCallbacks;

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
enum HostSystemEvent { SYSEVT_BLE_OFF = 1, SYSEVT_BLE_CONNECTED, SYSEVT_BLE_DISCONNECTED };
static void systemEventPost(HostSystemEvent event, const char* = nullptr, const char* = nullptr) {
  if (event == SYSEVT_BLE_CONNECTED) {
    ++boundary.connectedEvents;
    boundary.events.emplace_back("ble-connected");
    return;
  }
  if (event == SYSEVT_BLE_DISCONNECTED) {
    ++boundary.disconnectedEvents;
    boundary.events.emplace_back("ble-disconnected");
    return;
  }
  ++boundary.bleOffEvents;
  boundary.events.emplace_back("ble-off");
}
static TransportSessionEpoch transportSessionOpen(HostCommandSource) {
  ++boundary.sessionOpens;
  return 1000 + boundary.sessionOpens;
}
static BLEDeviceType bleIdentifyDeviceByMAC(const uint8_t*) { return BLE_DEVICE_UNKNOWN; }

// The console view (Bluetooth.cpp bleConsoleView) for bleLinkStatus().
static BleConsoleView bleConsoleView() {
  BleConsoleView view;
  view.up = gBLEState && gBLEState->initialized;
  return view;
}

// INSERT_PRODUCTION

// What bluetoothHalDeinit sees between the console's two hooks.
static void snapshotConsoleAtStop() {
  auto& at = boundary.atStop;
  at.seen = true;
  at.stateAlive = gBLEState != nullptr;
  at.admissionOpen = sBleConsoleAdmissionOpen.load();
  at.pointersNull = !pServer && !pCommandService && !pCmdRequestChar && !pCmdResponseChar &&
                    !pCmdStatusChar;
  at.gattsIfNone = sBleGattsIf.load() == ESP_GATT_IF_NONE;
  at.tapRemoved = BLEDevice::gattsHandler == nullptr;
  at.epochsClosed = true;
  for (auto& epoch : sBleSessionEpoch) at.epochsClosed = at.epochsClosed && epoch.load() == 0;
}

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
  sBleStateHolders = 0;
  sBlePipeRowSlot = -1;
  sBlePipeIdentity = BlePipeConsoleIdentity{};
  gPipeHost = PipeHost{};
}

static void resetWorld(const char* name) {
  currentCase = name;
  dropServerState();
  boundary = Boundary{};
  for (auto& epoch : sBleSessionEpoch) epoch.store(0);
  gSettings.bleMode = BLE_MODE_SERVER;
  gSettings.bleConsoleOnPipe = true;
  gSettings.bleSecureChannelSecret = "";
  gPipeRecord = PhonePipeLinkRecord{};
  gPipeRecordValid = false;
  sBleConsoleAttach = hw1console::Attach{};
  sBleConsoleLogged = hw1console::Slot{};
  sBleGattsIf.store(ESP_GATT_IF_NONE);
  sBleGattsRegStatus.store(kBleGattsRegPending);
  sBleAdvertising.store(false);
  sBleServerUp.store(false);
  sBleConsoleMode.store(hw1console::Mode::Off);
  sBleConsoleAdmissionOpen.store(false);
  hostNowMs = 1000;
  roleDepth = 0;
  roleState = BleRoleTransition::IDLE;
  BLEDevice::gattsHandler = nullptr;
  BLEDevice::registrationPending = false;
  BLEDevice::m_appId = 3;
  BLEDevice::reserved.clear();
  BLEDevice::idsExhausted = false;
  BLEDevice::registeredAppId = -1;
  destroyGattTree();
  pServer = nullptr;
  pAdvertising = nullptr;
  pCommandService = pDataService = pDeviceInfoService = nullptr;
  pCmdRequestChar = pCmdResponseChar = pCmdStatusChar = nullptr;
  pSensorDataChar = pSystemStatusChar = pEventNotifyChar = nullptr;
  pStreamControlChar = pManufacturerChar = pModelChar = pFirmwareChar = nullptr;
}

// A running server: advertising, two app clients with live session epochs.
static void startServer() {
  void* storage = std::malloc(sizeof(BLESystemState));
  if (!storage) std::abort();
  gBLEState = new (storage) BLESystemState{};
  sBleStateHolders = kBleHoldServer;  // as initBluetooth takes it
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
  sBleAdvertising.store(true);
  pServer = &gServer;
  pAdvertising = &gAdvertising;
  pCommandService = pDataService = pDeviceInfoService = &gService;
  pCmdRequestChar = pCmdResponseChar = pCmdStatusChar = &gCharacteristic;
  pSensorDataChar = pSystemStatusChar = pEventNotifyChar = &gCharacteristic;
  pStreamControlChar = pManufacturerChar = pModelChar = pFirmwareChar = &gCharacteristic;
  sBleServerUp.store(true);
  sBleConsoleMode.store(hw1console::Mode::Server);  // as initBluetooth publishes it
}

static bool said(const char* fragment) {
  for (const auto& line : boundary.messages) {
    if (line.find(fragment) != std::string::npos) return true;
  }
  return false;
}
static bool isError(const char* reply) { return reply && std::strncmp(reply, "Error", 5) == 0; }
static bool same(const char* reply, const char* want) { return reply && std::strcmp(reply, want) == 0; }
// The role the client mode starts: the glasses, with the relay pipe when built.
// roleOfMode(1, pipeBuilt) names the client role by the pipe alone (a build
// with the pipe and no glasses still runs the client stack).
#if ENABLE_BLE_PHONE_PIPE
static const char* const kClientSet = "[BLE] Boot role set to client (Glasses+pipe); running now: nothing; 'openble' starts it now";
static const char* const kClientSetServerUp = "[BLE] Boot role set to client (Glasses+pipe); running now: app console server; 'closeconsole', then 'openble' starts it";
#elif ENABLE_G2_GLASSES
static const char* const kClientSet = "[BLE] Boot role set to client (Glasses); running now: nothing; 'openble' starts it now";
static const char* const kClientSetServerUp = "[BLE] Boot role set to client (Glasses); running now: app console server; 'closeconsole', then 'openble' starts it";
#endif
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
  EXPECT(bleConsoleMode() == hw1console::Mode::Off && !bleServerUp(),
         "a stopped server still reports the console mode or the server as up");
  EXPECT(pServer == nullptr && pAdvertising == nullptr && pCmdResponseChar == nullptr,
         "GATT object pointers survived the stop");
  EXPECT(sBleSessionEpoch[0].load() == 0 && sBleSessionEpoch[1].load() == 0 &&
         boundary.sessionCloses == 2, "app sessions were not closed by the stop");
  EXPECT(sBleGattsIf.load() == ESP_GATT_IF_NONE, "GATT interface id survived the stop");
  EXPECT(boundary.halDeinits == 1 && boundary.centralAcks == 1 && !boundary.fault,
         "host teardown did not complete");
  EXPECT(boundary.bleOffEvents == 1 && said("[BLE] Deinitialized"), "stop was not announced");
}

// ---------------------------------------------------------------------------
// The app console on the G2 client host (bleConsoleRegisterOnClientHost, the
// ServerCallbacks client-mode filter and the host-teardown hooks)
// ---------------------------------------------------------------------------
static constexpr uint32_t kWrite = BLECharacteristic::PROPERTY_WRITE |
                                   BLECharacteristic::PROPERTY_WRITE_NR;

// initG2Client's position: inside its role claim, the fresh host up, no link.
static void freshClientHost(const char* name) {
  resetWorld(name);
  EXPECT(bleRoleTransitionBegin(BleRoleTransition::G2_START), "test setup: G2_START claim");
}
static void endClaim() {
  while (roleDepth > 0) bleRoleTransitionEnd();
}

static BLECharacteristic* characteristic(const char* uuid) {
  if (gServer.services.size() != 1) return nullptr;
  for (auto* c : gServer.services[0]->characteristics) {
    if (c->uuid == uuid) return c;
  }
  return nullptr;
}

// Exactly the command service, with today's three characteristics, and
// nothing a server-mode start adds.
static void expectCommandServiceOnly() {
  EXPECT(boundary.createServers == 1 && boundary.createServices == 1 &&
         boundary.services.size() == 1 && boundary.services[0] == BLE_COMMAND_SERVICE_UUID,
         "the client host got another service than the command service");
  EXPECT(gServer.services.size() == 1 && gServer.services[0]->starts == 1 &&
         gServer.services[0]->characteristics.size() == 3,
         "the command service was not started once with its three characteristics");
  const BLECharacteristic* request = characteristic(BLE_CMD_REQUEST_CHAR_UUID);
  const BLECharacteristic* response = characteristic(BLE_CMD_RESPONSE_CHAR_UUID);
  const BLECharacteristic* status = characteristic(BLE_CMD_STATUS_CHAR_UUID);
  EXPECT(request && request == pCmdRequestChar && request->properties == kWrite &&
         !request->preparedWrites && request->callbacks == &sCmdRequestCallbacks,
         "request characteristic changed (write, no prepared writes, its callbacks)");
  // Bluedroid checks the permission, not the properties, on a read, and Arduino
  // keeps the last write as the value: write-only, so no link reads it back.
  EXPECT(request && request->permissions == ESP_GATT_PERM_WRITE,
         "the request characteristic is readable (the last command, a login too)");
  EXPECT(response && response == pCmdResponseChar &&
         response->properties == BLECharacteristic::PROPERTY_NOTIFY &&
         response->descriptors == 1 && response->descriptorOwned,
         "response characteristic changed (notify, one owned CCCD)");
  EXPECT(status && status == pCmdStatusChar &&
         status->properties == BLECharacteristic::PROPERTY_READ &&
         status->callbacks == &sCmdStatusCallbacks,
         "status characteristic changed (read, its callbacks)");
  EXPECT(pCommandService == gServer.services[0] && pServer == &gServer &&
         gServer.callbacks == &sServerCallbacks,
         "the command service or the server callbacks are not the ones published");
  EXPECT(!pDataService && !pDeviceInfoService && !pSensorDataChar && !pSystemStatusChar &&
         !pEventNotifyChar && !pManufacturerChar && !pModelChar && !pFirmwareChar,
         "the client host built a data or device-info characteristic");
  EXPECT(boundary.advGets == 0 && boundary.advStarts == 0 && boundary.advStops == 0 &&
         !pAdvertising && !bleAdvertising(),
         "the client host touched advertising");
  EXPECT(boundary.mtuSets == 0, "the client host changed the local MTU (the client runs 244)");
  EXPECT(boundary.txPowerSets == 0, "the client host set the TX power");
}

static void expectRegistered() {
  EXPECT(bleConsoleMode() == hw1console::Mode::ClientRegistered && sBleConsoleAdmissionOpen.load(),
         "registration did not publish ClientRegistered with admission open");
  EXPECT(gBLEState && !gBLEState->initialized && !isBleServerInitialized() && !bleServerUp() &&
         !bleConsoleView().up,
         "the client host claimed the server role (initialized means server mode owns the host)");
  EXPECT(gBLEState && gBLEState->activeConnectionCount == 0 &&
         gBLEState->connectionState == BLE_STATE_IDLE &&
         gBLEState->sensorStreamInterval == 1000 && gBLEState->systemStreamInterval == 5000,
         "the console state is not the shared fresh state");
  EXPECT(sBleGattsIf.load() == 4 && BLEDevice::gattsHandler == bleGattsEventTap,
         "the GATT interface tap is not installed with the registered interface");
  EXPECT(boundary.scInits == 1 && boundary.scWarms == 1, "Secure Channel was not prepared");
  EXPECT(said("App console registered on the client host"), "registration was not logged");
  EXPECT(BLEDevice::registeredAppId >= 0 && BLEDevice::registeredAppId <= ESP_APP_ID_MAX &&
         BLEDevice::reserved.size() == 1 && BLEDevice::reserved[0] == BLEDevice::registeredAppId,
         "the server app did not register an id from the reserved 15-bit allocator");
  const size_t tap = eventAt("tap-installed"), server = eventAt("create-server");
  const size_t callbacks = eventAt("server-callbacks"), service = eventAt("create-service");
  const size_t start = eventAt("service-start"), sc = eventAt("sc-init");
  EXPECT(tap < server && server < callbacks && callbacks < service && service < start &&
         start < sc, "registration order changed (tap, server, callbacks, service, start, SC)");
}

// Nothing created, nothing left behind.
static void expectNotRegistered(const char* why) {
  EXPECT(!gBLEState && bleConsoleMode() == hw1console::Mode::Off &&
         !sBleConsoleAdmissionOpen.load() && !pServer && !pCommandService && !pCmdRequestChar &&
         !pCmdResponseChar && !pCmdStatusChar,
         "a refused registration left console state or pointers behind");
  EXPECT(boundary.createServices == 0 && gServer.services.empty(),
         "a refused registration created a service");
  EXPECT(BLEDevice::gattsHandler == nullptr, "a refused registration left the tap installed");
  EXPECT(boundary.scInits == 0 && boundary.advGets == 0 && boundary.mtuSets == 0,
         "a refused registration went on to Secure Channel, advertising or the MTU");
  EXPECT(said(why), "the refusal reason was not logged");
}

static void expectClosedAtStop() {
  const auto& at = boundary.atStop;
  EXPECT(at.seen && at.stateAlive, "the console state was freed before the host stopped");
  EXPECT(!at.admissionOpen, "admission was still open when the host stopped");
  EXPECT(at.pointersNull, "the command characteristic pointers outlived the start of the stop");
  EXPECT(at.gattsIfNone && at.tapRemoved, "the GATT interface or the tap survived into the stop");
  EXPECT(at.epochsClosed, "a console session epoch was still live when the host stopped");
}

// Bluedroid reports every link to the server app: the two temples, the ring
// and the phone. None is admitted on the client host.
static void deliverEveryLink(bool connect) {
  BLEServerCallbacks* callbacks = gServer.callbacks;
  if (!callbacks) return;
  for (uint16_t connId = 0; connId < 4; ++connId) {
    esp_ble_gatts_cb_param_t param{};
    param.connect.conn_id = connId;
    param.disconnect.conn_id = connId;
    if (connect) callbacks->onConnect(&gServer, &param);
    else callbacks->onDisconnect(&gServer, &param);
  }
}

static void clientHostCases() {
  // R1: registration on the fresh host, the tap seeing REG at once or only
  // after createServer returned (Arduino releases its wait first).
  for (bool late : {false, true}) {
    freshClientHost(late ? "R1 register, REG seen late" : "R1 register");
    boundary.regLate = late;
    EXPECT(bleConsoleRegisterOnClientHost(), "registration on a fresh client host failed");
    expectRegistered();
    expectCommandServiceOnly();
    EXPECT(gLifecycleMutex.depth == 0, "registration left the lifecycle mutex held");
    // R2: the links Bluedroid reports are all ignored.
    currentCase = late ? "R2 links ignored (late REG)" : "R2 links ignored";
    deliverEveryLink(true);
    deliverEveryLink(false);
    bool anySlot = false;
    for (const auto& c : gBLEState->connections) anySlot = anySlot || c.active;
    EXPECT(!anySlot && gBLEState->activeConnectionCount == 0 &&
           gBLEState->totalConnections == 0 && gBLEState->connectionState == BLE_STATE_IDLE,
           "a temple, ring or phone link got a console slot, a count or a state");
    EXPECT(boundary.connectedEvents == 0 && boundary.disconnectedEvents == 0 &&
           boundary.sessionOpens == 0,
           "a client-host link posted a BLE app event or opened a session");
    EXPECT(boundary.advStops == 0 && boundary.advStarts == 0 && boundary.advGets == 0,
           "a client-host link made an advertising call");
    // R2b: every link comes up again, then the pipe goes READY on link 3 (its
    // record comes after its link's connect). Link events admit nothing; the
    // reconcile makes exactly that link's slot (typed Phone, one session, one
    // event, no advertising call), temple and ring drops leave it, and the
    // phone link's drop ends it.
    currentCase = late ? "R2b pipe link becomes the slot (late REG)" : "R2b pipe link becomes the slot";
    gSettings.bleSecureChannelSecret = "Pass-phrase1!";
    deliverEveryLink(true);
    gPipeRecord = PhonePipeLinkRecord{5, 3};
    gPipeRecordValid = true;
    EXPECT(gBLEState->activeConnectionCount == 0 && boundary.sessionOpens == 0 &&
           boundary.connectedEvents == 0, "a link event admitted a link before reconcile");
    {
      BleLifecycleGuard held;
      bleConsoleReconcileLocked();
    }
    EXPECT(gBLEState->activeConnectionCount == 0 && boundary.connectedEvents == 0 &&
           gBLEState->connectionState == BLE_STATE_IDLE,
           "the slot counted as an app before the app joined it");
    {
      BleLifecycleGuard held;  // the app's first write (the write path's join)
      bleConsoleApplyLocked(hw1console::join(sBleConsoleAttach, 3));
    }
    int phoneSlots = 0, otherSlots = 0;
    for (const auto& c : gBLEState->connections) {
      if (!c.active) continue;
      if (c.connId == 3 && c.deviceType == BLE_DEVICE_PHONE) ++phoneSlots;
      else ++otherSlots;
    }
    EXPECT(phoneSlots == 1 && otherSlots == 0 && gBLEState->activeConnectionCount == 1 &&
           gBLEState->connectionState == BLE_STATE_CONNECTED && bleSessionEpochForConnection(3) != 0,
           "the reconcile did not make exactly the pipe link's slot with a session");
    EXPECT(boundary.connectedEvents == 1 && boundary.sessionOpens == 1 && boundary.advStops == 0 &&
           boundary.advStarts == 0 && boundary.advGets == 0,
           "the phone-link slot posted the wrong events or touched advertising");
    const TransportSessionEpoch phoneEpoch = bleSessionEpochForConnection(3);
    for (uint16_t id = 0; id < 3; ++id) {
      esp_ble_gatts_cb_param_t param{};
      param.disconnect.conn_id = id;
      gServer.callbacks->onDisconnect(&gServer, &param);
    }
    EXPECT(bleSessionEpochForConnection(3) == phoneEpoch && boundary.disconnectedEvents == 0 &&
           gBLEState->activeConnectionCount == 1,
           "a temple or ring drop touched the phone-link slot");
    {
      esp_ble_gatts_cb_param_t param{};
      param.disconnect.conn_id = 3;
      gServer.callbacks->onDisconnect(&gServer, &param);
    }
    EXPECT(bleSessionEpochForConnection(3) == 0 && boundary.disconnectedEvents == 1 &&
           gBLEState->activeConnectionCount == 0 && gBLEState->connectionState == BLE_STATE_IDLE &&
           boundary.advStarts == 0,
           "the phone link's drop did not end its slot (or touched advertising)");
    // The pipe's record is still set (it has not noticed): no slot comes back.
    {
      BleLifecycleGuard held;
      bleConsoleReconcileLocked();
    }
    EXPECT(bleSessionEpochForConnection(3) == 0 && boundary.sessionOpens == 1,
           "a stale record of the dropped link got its slot back");
    // A new pipe session (generation 6) on link 3 attaches again for the
    // teardown case below.
    gPipeRecord = PhonePipeLinkRecord{6, 3};
    {
      BleLifecycleGuard held;
      bleConsoleReconcileLocked();
      bleConsoleApplyLocked(hw1console::join(sBleConsoleAttach, 3));
    }
    EXPECT(bleSessionEpochForConnection(3) != 0 && boundary.sessionOpens == 2,
           "the pipe's next session did not attach");
    // R3: a second registration on the same host is refused (no late service).
    currentCase = "R3 second registration on the same host";
    const unsigned services = boundary.createServices;
    EXPECT(!bleConsoleRegisterOnClientHost() && boundary.createServices == services &&
           bleConsoleMode() == hw1console::Mode::ClientRegistered,
           "a second registration started another service on a live host");
    EXPECT(said("still held"), "the second registration's refusal was not logged");
    // T1: a terminal host stop closes, then frees.
    // T1 also ends the phone-link slot R2b left attached (and an epoch on a
    // slot index nothing holds).
    currentCase = late ? "T1 teardown (late REG)" : "T1 teardown";
    sBleSessionEpoch[3].store(77);
    const unsigned closesBefore = boundary.sessionCloses;
    const unsigned endsBefore = boundary.disconnectedEvents;
    endClaim();
    const BluetoothHalShutdownResult stop = bluetoothHalDeinit();
    EXPECT(stop.success, "test setup: host stop");
    expectClosedAtStop();
    EXPECT(boundary.sessionCloses == closesBefore + 2 && boundary.disconnectedEvents == endsBefore + 1,
           "the teardown did not end the phone-link slot and close every live epoch");
    EXPECT(!sBleConsoleAttach.slot.present && !sBleConsoleAttach.closed,
           "the teardown left the attach state for the next host");
    EXPECT(!gBLEState && bleConsoleMode() == hw1console::Mode::Off &&
           !sBleConsoleAdmissionOpen.load(),
           "a terminal stop did not free the console state and turn the mode Off");
    expectReleased();
  }

  // T2: an incomplete stop keeps the state, closed; the next stop frees it,
  // and only then can a fresh host register again.
  freshClientHost("T2 incomplete stop");
  EXPECT(bleConsoleRegisterOnClientHost(), "test setup: registration");
  endClaim();
  boundary.halStops = false;
  EXPECT(!bluetoothHalDeinit().success, "test setup: failed stop");
  expectClosedAtStop();
  EXPECT(gBLEState && bleConsoleMode() == hw1console::Mode::ClientRegistered &&
         !sBleConsoleAdmissionOpen.load() && !pCmdResponseChar && !pServer,
         "an incomplete stop freed the state (Arduino keeps its callback targets) or reopened it");
  EXPECT(bleRoleTransitionBegin(BleRoleTransition::RECOVERING), "test setup: RECOVERING claim");
  const unsigned services = boundary.createServices;
  EXPECT(!bleConsoleRegisterOnClientHost() && boundary.createServices == services,
         "registration ran over the state an incomplete stop kept");
  endClaim();
  boundary.halStops = true;
  boundary.atStop = {};
  EXPECT(bluetoothHalDeinit().success, "test setup: retried stop");
  expectClosedAtStop();
  EXPECT(!gBLEState && bleConsoleMode() == hw1console::Mode::Off,
         "the retried terminal stop did not free the kept state");
  EXPECT(bleRoleTransitionBegin(BleRoleTransition::G2_START), "test setup: G2_START claim");
  boundary.messages.clear();
  EXPECT(bleConsoleRegisterOnClientHost() &&
         bleConsoleMode() == hw1console::Mode::ClientRegistered,
         "a fresh host after the completed stop could not register");
  endClaim();
  EXPECT(bluetoothHalDeinit().success && !gBLEState, "test cleanup: stop");
  expectReleased();

  // G1: the GATT registration failed, or its event never reached the tap.
  for (int failure = 0; failure < 2; ++failure) {
    freshClientHost(failure ? "G1 registration event never seen" : "G1 registration refused");
    if (failure) boundary.regNever = true;
    else boundary.regStatus = 0x80;  // ESP_GATT_NO_RESOURCES
    EXPECT(!bleConsoleRegisterOnClientHost(), "a failed GATT registration reported success");
    EXPECT(boundary.createServers == 1, "test setup: the server app was created");
    expectNotRegistered("GATT registration status");
    EXPECT(hostNowMs - 1000 <= 250, "the registration wait is not bounded");
    endClaim();
    EXPECT(bluetoothHalDeinit().success && !gBLEState, "a later host stop is unaffected");
    expectReleased();
  }

  // R4: the raw app-id counter has passed ESP_APP_ID_MAX (every client connect
  // advances it), where Arduino's createServer would wait forever: the server
  // still registers, with an id from the reserved 15-bit allocator that skips
  // the ids clients hold.
  freshClientHost("R4 app-id counter past ESP_APP_ID_MAX");
  BLEDevice::m_appId = ESP_APP_ID_MAX + 7;
  BLEDevice::reserved.push_back(6);  // a client holds the id the mask lands on first
  EXPECT(bleConsoleRegisterOnClientHost() && bleConsoleMode() == hw1console::Mode::ClientRegistered,
         "registration failed once the raw app-id counter passed ESP_APP_ID_MAX");
  EXPECT(BLEDevice::registeredAppId == 7 && BLEDevice::reserved.size() == 2 &&
         BLEDevice::reserved[1] == 7 && BLEDevice::m_appId == 8,
         "the server registered an unmasked id or a client's reserved one");
  endClaim();
  EXPECT(bluetoothHalDeinit().success && !gBLEState, "test cleanup: stop");
  expectReleased();

  // P: each precondition refuses before anything is created.
  struct Refusal { const char* name; const char* why; };
  const Refusal refusals[] = {
    {"P1 outside a role transition", "outside a BLE role transition"},
    {"P2 under the lifecycle lock", "under the lifecycle lock"},
    {"P3 host not ready", "host not ready"},
    {"P4 left temple link held", "a link may already be up"},
    {"P5 right temple link held", "a link may already be up"},
    {"P6 ring link held", "a link may already be up"},
    {"P7 phone link held", "a link may already be up"},
    {"P8 no free GATT app id", "no free GATT app id"},
    {"P9 server mode owns the host", "still held"},
#if ENABLE_G2_GLASSES
    {"P10 G2 client already initialized", "a link may already be up"},
#endif
  };
  for (const Refusal& r : refusals) {
    freshClientHost(r.name);
    const std::string name(r.name);
    if (name.rfind("P1 ", 0) == 0) endClaim();
    if (name.rfind("P3 ", 0) == 0) boundary.hostReady = false;
    if (name.rfind("P4 ", 0) == 0) boundary.linkHeld[0] = true;
    if (name.rfind("P5 ", 0) == 0) boundary.linkHeld[1] = true;
    if (name.rfind("P6 ", 0) == 0) boundary.linkHeld[2] = true;
    if (name.rfind("P7 ", 0) == 0) boundary.linkHeld[3] = true;
    if (name.rfind("P8 ", 0) == 0) BLEDevice::idsExhausted = true;
    if (name.rfind("P10 ", 0) == 0) boundary.g2Client = true;
    bool registered = false;
    if (name.rfind("P9 ", 0) == 0) {
      startServer();
      registered = bleConsoleRegisterOnClientHost();
      EXPECT(!registered && boundary.createServers == 0 && isBleServerInitialized() &&
             bleConsoleMode() == hw1console::Mode::Server && sessionsLive(),
             "registration touched a running server");
      EXPECT(said(r.why), "the refusal reason was not logged");
      endClaim();
      continue;
    }
    if (name.rfind("P2 ", 0) == 0) {
      BleLifecycleGuard held;
      registered = bleConsoleRegisterOnClientHost();
    } else {
      registered = bleConsoleRegisterOnClientHost();
    }
    EXPECT(!registered && boundary.createServers == 0, "a refused registration created the server app");
    expectNotRegistered(r.why);
    endClaim();
    expectReleased();
  }

  // S0: the filter leaves server mode as it was: an app connect still gets a
  // slot, a session, the event and re-armed advertising; its disconnect ends it.
  resetWorld("S0 server-mode connect and disconnect");
  startServer();
  gBLEState->connectionState = BLE_STATE_CONNECTED;
  {
    BLEServerCallbacks* callbacks = &sServerCallbacks;
    esp_ble_gatts_cb_param_t param{};
    param.connect.conn_id = 5;
    param.disconnect.conn_id = 5;
    callbacks->onConnect(&gServer, &param);
    EXPECT(gBLEState->activeConnectionCount == 3 && bleSessionEpochForConnection(5) != 0 &&
           boundary.connectedEvents == 1 && boundary.advStarts == 1,
           "server mode no longer admits an app connect (slot, session, event, advertising)");
    callbacks->onDisconnect(&gServer, &param);
    EXPECT(gBLEState->activeConnectionCount == 2 && bleSessionEpochForConnection(5) == 0 &&
           boundary.disconnectedEvents == 1 && boundary.sessionCloses == 1 &&
           bleSessionEpochMatches(7, 41) && bleSessionEpochMatches(9, 42),
           "server mode no longer ends an app link on its disconnect");
  }

  // H: the hooks leave every other host alone: nothing registered, or the
  // server role (deinitBluetooth owns that state).
  resetWorld("H1 hooks with no console");
  EXPECT(bluetoothHalDeinit().success && !gBLEState &&
         bleConsoleMode() == hw1console::Mode::Off && boundary.sessionCloses == 0,
         "the hooks acted on a host with no console");
  resetWorld("H2 hooks under the server role");
  startServer();
  bleConsoleHostTeardownBegin();
  bleConsoleHostTeardownEnd(true);
  EXPECT(isBleServerInitialized() && sessionsLive() && sBleGattsIf.load() == 3 &&
         pCmdResponseChar && pServer && bleConsoleMode() == hw1console::Mode::Server,
         "the client-host hooks touched the server role's state");

  // D: deinitBluetooth stays the server's stop: on the client host it reports
  // stopped and leaves the console and the host alone.
  freshClientHost("D1 deinitBluetooth on the client host");
  EXPECT(bleConsoleRegisterOnClientHost(), "test setup: registration");
  endClaim();
  EXPECT(deinitBluetooth() && boundary.halDeinits == 0 && gBLEState &&
         bleConsoleMode() == hw1console::Mode::ClientRegistered && pCmdResponseChar,
         "deinitBluetooth tore down the client host or its console");
  EXPECT(bluetoothHalDeinit().success && !gBLEState, "test cleanup: stop");
  expectReleased();
}

#if ENABLE_BLE_PHONE_PIPE
// The phone link's console row (phone link v2) and gBLEState's holders: the
// server role, the client host's command service and the pipe. The state is
// freed when the last holder leaves. A host teardown ends the pipe's row and
// closes every epoch before the host stops, with no CLOSE (the link goes with
// the host); the pipe's hold keeps the state for its next host.
static BlePipeConsoleIdentity pipeIdentity() {
  BlePipeConsoleIdentity id;
  blePipeConsoleIdentity(&id);
  return id;
}

static void pipeHolderCases() {
  // H1: phone-client mode. The pipe's hold allocates the state (no server
  // role); a bound row is a connected app; the host teardown ends it before
  // the stop; the pipe's release frees the state.
  freshClientHost("H1 phone-client hold, host teardown, release");
  EXPECT(blePipeConsoleStateAcquire() && gBLEState && !gBLEState->initialized &&
         !isBleServerInitialized() && bleConsoleMode() == hw1console::Mode::Off,
         "the pipe's hold did not allocate a state outside the server role");
  EXPECT(blePipeConsoleBind(5, 9, 3), "the console row did not bind");
  BlePipeConsoleIdentity id = pipeIdentity();
  EXPECT(id.bound && id.generation == 5 && id.streamId == 9 &&
         id.rowConnId == blePipeConsoleRowId(5) && id.epoch != 0 && !id.authed,
         "the snapshot does not name the fresh signed-out row");
  EXPECT(gBLEState->activeConnectionCount == 1 && gBLEState->connectionState == BLE_STATE_CONNECTED &&
         boundary.connectedEvents == 1 && gBLEState->deferredConnectPending,
         "a bound row is not a connected app");
  EXPECT(bleSessionEpochMatches(blePipeConsoleRowId(5), id.epoch) &&
         findConnectionSlotByConnIdLocked(3) < 0,
         "the row is not found by its own id, or took the link's id");
  endClaim();
  EXPECT(bluetoothHalDeinit().success, "test setup: host stop");
  EXPECT(boundary.atStop.seen && boundary.atStop.stateAlive && boundary.atStop.epochsClosed,
         "the console row's session outlived the start of the host stop");
  id = pipeIdentity();
  EXPECT(!id.bound && gBLEState && gBLEState->activeConnectionCount == 0 &&
         gBLEState->connectionState == BLE_STATE_IDLE && boundary.disconnectedEvents == 1 &&
         boundary.sessionCloses == boundary.sessionOpens,
         "the host teardown did not end the console row");
  EXPECT(pipeHostCloses().empty(), "a host teardown asked the pipe for a CLOSE");
  blePipeConsoleStateRelease();
  EXPECT(!gBLEState && sBleStateHolders == 0, "the pipe's release did not free the state");
  expectReleased();

  // H2: design A registers on a host whose pipe holds the state: the command
  // service joins it; the client host's terminal stop keeps it for the pipe,
  // whose release frees it.
  freshClientHost("H2 registration joins the pipe's state");
  EXPECT(blePipeConsoleStateAcquire(), "test setup: the pipe holds the state");
  BLESystemState* held = gBLEState;
  EXPECT(bleConsoleRegisterOnClientHost() && gBLEState == held &&
         sBleStateHolders == (kBleHoldPipe | kBleHoldClientConsole),
         "registration refused, or allocated a second state beside the pipe's");
  expectRegistered();
  EXPECT(blePipeConsoleBind(6, 1, 3), "the console row did not bind beside the command service");
  endClaim();
  EXPECT(bluetoothHalDeinit().success, "test setup: host stop");
  expectClosedAtStop();
  EXPECT(gBLEState == held && bleConsoleMode() == hw1console::Mode::Off && !pipeIdentity().bound &&
         sBleStateHolders == kBleHoldPipe,
         "the client host's stop freed the pipe's state or kept its row");
  blePipeConsoleStateRelease();
  EXPECT(!gBLEState, "the pipe's release did not free the state");
  expectReleased();

  // H3: the G2 order: registration first, then the pipe joins; the pipe stops
  // first (its row ends, the state stays for the command service) and the
  // host's terminal stop frees it.
  freshClientHost("H3 the pipe joins the command service's state");
  EXPECT(bleConsoleRegisterOnClientHost(), "test setup: registration");
  BLESystemState* registered = gBLEState;
  EXPECT(blePipeConsoleStateAcquire() && gBLEState == registered, "the pipe did not join the state");
  EXPECT(blePipeConsoleBind(7, 1, 3), "the console row did not bind");
  blePipeConsoleStateRelease();
  EXPECT(gBLEState == registered && !pipeIdentity().bound &&
         boundary.sessionCloses == boundary.sessionOpens && sBleStateHolders == kBleHoldClientConsole,
         "the pipe's release kept its row or freed the command service's state");
  endClaim();
  EXPECT(bluetoothHalDeinit().success && !gBLEState, "the client host's stop did not free the state");
  expectReleased();

  // H4: the server role never shares its state with the pipe.
  resetWorld("H4 the server's state is not the pipe's");
  startServer();
  EXPECT(!blePipeConsoleStateAcquire() && !blePipeConsoleBind(1, 1, 3) && sessionsLive() &&
         sBleStateHolders == kBleHoldServer,
         "the pipe took a hold on the server's state or bound a row there");
  expectReleased();

  // H5: binding is idempotent for its stream; a new stream's row replaces the
  // old one (one console row at a time); End matches generation and stream.
  freshClientHost("H5 bind, rebind, end");
  EXPECT(blePipeConsoleStateAcquire(), "test setup: hold");
  EXPECT(blePipeConsoleBind(8, 3, 2), "the console row did not bind");
  const TransportSessionEpoch first = pipeIdentity().epoch;
  EXPECT(blePipeConsoleBind(8, 3, 2) && pipeIdentity().epoch == first &&
         gBLEState->activeConnectionCount == 1,
         "binding the same stream again renewed its session");
  EXPECT(blePipeConsoleBind(8, 5, 2) && pipeIdentity().streamId == 5 && pipeIdentity().epoch != first &&
         gBLEState->activeConnectionCount == 1 && boundary.sessionCloses == 1,
         "a new stream's row did not replace the old one");
  blePipeConsoleEnd(8, 3);
  EXPECT(pipeIdentity().bound, "End for another stream ended the row");
  blePipeConsoleEnd(9, 5);
  EXPECT(pipeIdentity().bound, "End for another generation ended the row");
  blePipeConsoleEnd(8, 5);
  blePipeConsoleEnd(8, 5);
  EXPECT(!pipeIdentity().bound && gBLEState->activeConnectionCount == 0 &&
         boundary.sessionCloses == boundary.sessionOpens && boundary.disconnectedEvents == 2,
         "End did not end the row exactly once");
  blePipeConsoleStateRelease();
  endClaim();
  expectReleased();

  // H6: no transport-session slot leaks across start/stop cycles, whether the
  // pipe stops first or its host does.
  freshClientHost("H6 start/stop cycles");
  endClaim();
  for (int cycle = 0; cycle < 24; ++cycle) {
    EXPECT(blePipeConsoleStateAcquire(), "a cycle's hold failed");
    EXPECT(blePipeConsoleBind(uint32_t(100 + cycle), uint16_t(1 + cycle % 3), 3),
           "a cycle's row did not bind");
    if (cycle % 2) EXPECT(bluetoothHalDeinit().success, "a cycle's host stop failed");
    blePipeConsoleStateRelease();
  }
  EXPECT(!gBLEState && sBleStateHolders == 0 && boundary.sessionOpens == 24 &&
         boundary.sessionCloses == boundary.sessionOpens,
         "start/stop cycles leaked a transport session or the state");
  expectReleased();
}
#endif

int main() {
  // S1: Ring owner still active.
  resetWorld("S1 closeble, Ring owner busy");
  startServer();
  boundary.ringQuiesces = false;
  const char* reply = cmd_closeconsole("");
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
  EXPECT(bleConsoleMode() == hw1console::Mode::Server && bleServerUp(),
         "a deferred stop changed the console mode");
  expectReleased();
  currentCase = "S1 closeble retry after the Ring owner quiesced";
  boundary.ringQuiesces = true;
  boundary.messages.clear();
  boundary.events.clear();
  reply = cmd_closeconsole("");
  EXPECT(!isError(reply) && std::strcmp(reply, "App console server stopped (Bluetooth host off; 'openble' brings the client stack back)") == 0,
         "a retried closeconsole did not report the stop");
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
  reply = cmd_closeconsole("");
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
  reply = cmd_closeconsole("");
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
  EXPECT(isBleServerInitialized() && bleConsoleMode() == hw1console::Mode::Server,
         "a failed server teardown dropped the server state or its console mode");
  EXPECT(boundary.atStop.seen && boundary.atStop.stateAlive,
         "the console hooks touched the server's state during its teardown");
  expectReleased();

  // S4: a clean stop keeps its order and outcome.
  resetWorld("S4 closeble success");
  startServer();
  reply = cmd_closeconsole("");
  EXPECT(std::strcmp(reply, "App console server stopped (Bluetooth host off; 'openble' brings the client stack back)") == 0, "closeconsole success reply changed");
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

  // S5: nothing running: closeconsole says so and changes nothing (the saved
  // boot role is the user's; only a real stop resets it).
  resetWorld("S5 closeconsole, server not running");
  reply = cmd_closeconsole("");
  EXPECT(std::strcmp(reply, "App console server not running") == 0 &&
         gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 0,
         "closeconsole without a server changed the saved role");
  EXPECT(boundary.ringPrepares == 0 && boundary.halDeinits == 0,
         "closeconsole without a server touched the Ring owner or the host");
  EXPECT(deinitReported(deinitBluetooth), "deinitBluetooth without a server must report stopped");
  expectReleased();

#if ENABLE_G2_GLASSES
  // S6: beside the glasses there is no server: closeconsole says so and leaves
  // the glasses, the pipe and the host exactly as they were (closephone and
  // closeg2 are the device verbs; closeble is the whole radio).
  resetWorld("S6 closeconsole beside the glasses");
  boundary.g2Client = true;
  boundary.pipeBesideGlasses = true;
  reply = cmd_closeconsole("");
  EXPECT(!isError(reply) && std::strcmp(reply, "App console server not running") == 0,
         "closeconsole beside the glasses did not say the server is not running");
  EXPECT(boundary.phoneStopCalls == 0 && boundary.g2Client && boundary.ringPrepares == 0 &&
         boundary.halDeinits == 0 && boundary.settingWrites == 0,
         "closeconsole beside the glasses touched the pipe, the glasses or the host");
  boundary.pipeBesideGlasses = false;
  expectReleased();
#endif

  // blemode is the saved BOOT role only: it stops nothing and claims no role,
  // so it succeeds beside a running server (and says what runs now) and while
  // another role transition is active. openconsole / closeconsole / openble are
  // the runtime switches.
#if ENABLE_G2_GLASSES || ENABLE_BLE_PHONE_PIPE
  resetWorld("M1 blemode client beside the running server");
  startServer();
  boundary.ringQuiesces = false;
  reply = cmd_blemode("client");
  EXPECT(same(reply, kClientSetServerUp) && gSettings.bleMode == BLE_MODE_G2_CLIENT && boundary.settingWrites == 1,
         "blemode client beside the server did not store the boot role and name the server");
  EXPECT(isBleServerInitialized() && sessionsLive() && boundary.ringPrepares == 0 && boundary.halDeinits == 0,
         "blemode client stopped the server or touched the host");
  expectReleased();
  resetWorld("M1 blemode client, role transition busy");
  startServer();
  boundary.roleBusy = true;
  reply = cmd_blemode("1");
  EXPECT(same(reply, kClientSetServerUp) && gSettings.bleMode == BLE_MODE_G2_CLIENT && boundary.settingWrites == 1,
         "blemode 1 needs no role claim: a busy transition must not block the saved role");
  expectReleased();

  // M2: with nothing running the reply says what starts the client stack.
  resetWorld("M2 blemode client, nothing running");
  reply = cmd_blemode("client");
  EXPECT(same(reply, kClientSet) && gSettings.bleMode == BLE_MODE_G2_CLIENT && boundary.settingWrites == 1,
         "blemode client with nothing running changed");
  expectReleased();
  resetWorld("M2 blemode client, client stack up");
  boundary.stackReady = true;
  reply = cmd_blemode("g2");
  EXPECT(same(reply, ENABLE_BLE_PHONE_PIPE
                  ? "[BLE] Boot role set to client (Glasses+pipe); running now: client stack"
                  : "[BLE] Boot role set to client (Glasses); running now: client stack") &&
         gSettings.bleMode == BLE_MODE_G2_CLIENT && boundary.ringPrepares == 0,
         "blemode client with the stack up changed");
  boundary.stackReady = false;
  expectReleased();
#else
  // M3: no client device compiled in: the client role is refused before any write.
  resetWorld("M3 blemode client, no client device compiled");
  startServer();
  reply = cmd_blemode("client");
  EXPECT(std::strncmp(reply, "Error: [BLE] no client device is compiled in", 44) == 0 &&
         boundary.settingWrites == 0 && isBleServerInitialized() && sessionsLive(),
         "blemode client in a build without any client device changed");
  expectReleased();
#endif

  // blemode server stores the boot role and says what starts the server now.
  resetWorld("blemode server");
  reply = cmd_blemode("server");
  EXPECT(same(reply, "[BLE] Boot role set to server (App console); running now: nothing; 'openconsole' starts it now") &&
         gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 1,
         "blemode server changed");
  expectReleased();
  resetWorld("blemode server, client stack up");
  boundary.stackReady = true;
  reply = cmd_blemode("server");
  EXPECT(same(reply, "[BLE] Boot role set to server (App console); running now: client stack; 'openconsole' switches to it now") &&
         gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 1,
         "blemode server with the client stack up changed");
  boundary.stackReady = false;
  expectReleased();

  // The phone-client mode is gone (the pipe is a device on the shared client
  // stack): its keyword and alias are refused with the usage, nothing stored.
  for (const char* gone : {"phone-client", "phonelink", "PHONELINK", "2"}) {
    resetWorld("removed phone-client mode");
    reply = cmd_blemode(gone);
    EXPECT(same(reply, "Error: invalid arguments — Usage: blemode [client|server]") &&
           gSettings.bleMode == BLE_MODE_SERVER && boundary.settingWrites == 0,
           "the removed phone-client mode was accepted or stored");
    expectReleased();
  }
  // glasses is client, any case.
  for (const char* alias : {"glasses", "GLASSES", "Glasses"}) {
    resetWorld("glasses alias");
    reply = cmd_blemode(alias);
#if ENABLE_G2_GLASSES || ENABLE_BLE_PHONE_PIPE
    EXPECT(same(reply, kClientSet) && gSettings.bleMode == BLE_MODE_G2_CLIENT && boundary.settingWrites == 1,
           "blemode glasses is not blemode client");
#else
    EXPECT(std::strncmp(reply, "Error: [BLE] no client device is compiled in", 44) == 0 && boundary.settingWrites == 0,
           "blemode glasses in a build without a client device is not blemode client's refusal");
#endif
    expectReleased();
  }
  // `phone` meant server and collided with the phone pipe's own command: it is
  // refused with the usage, before any role is claimed or stopped, as are the
  // near misses of the aliases.
  for (const char* alias : {"phone", "PHONE", "phone-only", "phone-link", "glass", "phonelinks"}) {
    resetWorld("removed phone alias");
    gSettings.bleMode = BLE_MODE_G2_CLIENT;
    boundary.g2Client = true;
    boundary.pipeBesideGlasses = true;
    reply = cmd_blemode(alias);
    EXPECT(same(reply, "Error: invalid arguments — Usage: blemode [client|server]") &&
           gSettings.bleMode == BLE_MODE_G2_CLIENT && boundary.settingWrites == 0 &&
           boundary.phoneStopCalls == 0 && boundary.messages.empty() && roleState == BleRoleTransition::IDLE,
           "a removed `phone` alias changed the mode or stopped a role");
#if ENABLE_G2_GLASSES
    EXPECT(boundary.g2Client, "a removed `phone` alias stopped the glasses");
#endif
    expectReleased();
  }
  resetWorld("unknown mode while a role transition runs");
  boundary.roleBusy = true;
  EXPECT(std::strncmp(cmd_blemode("bogus"), "Error: invalid arguments", 24) == 0,
         "an unknown mode must answer with the usage, not the busy role transition");
  expectReleased();

  // Q: the query names the saved mode with the role it starts and the role
  // running now, in BLE_LinkStatusCore.h's words.
  resetWorld("Q1 server saved, App console running");
  startServer();
  EXPECT(same(cmd_blemode(""), "Saved mode: server (App console); running: App console"),
         "blemode query for a running console changed");
  resetWorld("Q2 server saved, nothing running");
  EXPECT(same(cmd_blemode(""), "Saved mode: server (App console); running: Off"),
         "blemode query with nothing running changed");
  resetWorld("Q3 server saved, phone pipe running (phone connect)");
  boundary.phoneOnly = true;
  EXPECT(same(cmd_blemode(""), "Saved mode: server (App console); running: Phone pipe"),
         "blemode query hides a pipe that runs in server mode");
#if ENABLE_G2_GLASSES
  resetWorld("Q5 client saved, glasses and pipe running");
  gSettings.bleMode = BLE_MODE_G2_CLIENT;
  boundary.g2Client = true;
  boundary.pipeBesideGlasses = ENABLE_BLE_PHONE_PIPE;
  EXPECT(same(cmd_blemode(""), ENABLE_BLE_PHONE_PIPE
                  ? "Saved mode: client (Glasses+pipe); running: Glasses+pipe"
                  : "Saved mode: client (Glasses); running: Glasses"),
         "blemode query for the glasses changed");
  boundary.pipeBesideGlasses = false;
  EXPECT(same(cmd_blemode(""), ENABLE_BLE_PHONE_PIPE
                  ? "Saved mode: client (Glasses+pipe); running: Glasses"
                  : "Saved mode: client (Glasses); running: Glasses"),
         "blemode query for the glasses without the pipe changed");
#endif
  EXPECT(boundary.settingWrites == 0 && roleDepth == 0, "the blemode query changed state");
  gCLIValidateOnly = true;
  EXPECT(same(cmd_blemode("phone"), "VALID"), "validate-only blemode ran the handler");
  gCLIValidateOnly = false;
#if ENABLE_BLE_PHONE_PIPE
  clientHostCases();
  pipeHolderCases();
#else
  // Without the pipe there is no phone link: registration refuses and nothing
  // is created.
  currentCase = "R0 no pipe build";
  {
    const unsigned services = boundary.createServices;
    EXPECT(!bleConsoleRegisterOnClientHost() && boundary.createServices == services &&
           bleConsoleMode() != hw1console::Mode::ClientRegistered,
           "a build without the pipe registered the phone-link console");
  }
#endif
  dropServerState();
  destroyGattTree();
  std::printf("ble server stop (G2=%d): %u assertions, %u failures\n",
              ENABLE_G2_GLASSES, assertions, failures);
  return failures == 0 ? 0 : 1;
}
