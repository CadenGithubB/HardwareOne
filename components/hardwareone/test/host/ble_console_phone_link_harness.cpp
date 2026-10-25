// The app console on the phone pipe's link (G2 client host), as shipped:
// test_ble_console_phone_link.py inserts Bluetooth.cpp's server callbacks,
// write callback, console plan helpers, senders, console view, commands,
// teardown hooks and bleUpdateStreams, and BLE_LinkStatus.cpp's
// bleLinkStatus(). The pipe's link record, the Secure Channel, the central TX
// gate, transport sessions and the Arduino GATT objects are host boundaries
// that record what was done. Link ids: temples 0 and 1, the ring 2, the phone 3.
#include "Arduino.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
using TransportSessionEpoch = uint32_t;

// INSERT_TYPES

// ---------------------------------------------------------------------------
// Host boundaries
// ---------------------------------------------------------------------------
struct World {
  std::vector<std::string> log;  // ordered boundary calls the cases read
  // The pipe and the roles bleLinkStatus() reads.
  bool recordValid = false;
  PhonePipeLinkRecord record;
  bool pipeActive = true, pipeReady = false, g2Client = true, serverUp = false;
  // Sessions, secure channel, events.
  unsigned sessionOpens = 0, sessionCloses = 0, scResets = 0;
  bool failSessionOpen = false;
  std::set<uint16_t> scEstablished;
  bool scRequired = false;
  unsigned connectedEvents = 0, disconnectedEvents = 0;
  std::string connectedSubject, connectedDetail;
  // Radio: advertising, GATT closes, the central TX gate, directed sends.
  unsigned advStarts = 0, advStops = 0, advCommandStarts = 0, serverCloses = 0;
  bool gateBusy = false, accessoryPressure = false;
  int gateHeld = 0;
  unsigned gateTries = 0, gateBypasses = 0, activity = 0;
  std::vector<uint16_t> sentTo;
  std::vector<std::string> sentText;
  // Commands and the main loop.
  std::vector<uint16_t> dispatched;  // processIncomingBLECommand
  std::vector<std::string> ran;      // processBleCommandLine
  unsigned sessionTicks = 0, settingWrites = 0, poolReleases = 0;
  unsigned lockTakes = 0, recordReads = 0;
  std::vector<std::string> debug, hints;
};
static World w;

using TickType_t = uint32_t;
using BaseType_t = int;
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static uint32_t nowMs = 1000;
static uint32_t millis() { return nowMs; }
static unsigned backoffs = 0;
static int landingAfterDelays = -1;  // a post in flight lands after this many delays
static void landPostInFlight();
static void vTaskDelay(TickType_t ticks) {
  ++backoffs;
  nowMs += ticks;
  if (landingAfterDelays > 0 && --landingAfterDelays == 0) landPostInFlight();
}

// The recursive lifecycle mutex: depth only (one task here, BTC included).
struct HostMutex { int depth = 0; };
using SemaphoreHandle_t = HostMutex*;
static HostMutex gLifecycle;
static SemaphoreHandle_t sBleLifecycleMutex = &gLifecycle;
static bool lifecycleBusy = false;  // another task holds it (only tries can tell)
static BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t) {
  if (lifecycleBusy && mutex->depth == 0) return 0;
  ++w.lockTakes;
  ++mutex->depth;
  return pdTRUE;
}
static BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
  --mutex->depth;
  return pdTRUE;
}
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))

#define ENABLE_OLED_DISPLAY 0
#define DEBUG_BLE_CORE 1
#define DEBUG_BLE_GATT 2
#define DEBUG_BLE_DATA 3
static void hostDebug(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  w.debug.emplace_back(line);
}
#define BLE_DEBUGF(flag, ...) hostDebug(__VA_ARGS__)
constexpr uint8_t MSG_ROUTE_ALL = 0x3F;
constexpr uint8_t MSG_ROUTE_BLE = 0x10;
static bool bleDataDebugEnabled() { return false; }
static void broadcastOutputCore_Routed(const char*, size_t, uint8_t) {}

// The shared debug buffer, which may never have been allocated (System_Debug).
static char gDebugBuffer[1024];
static bool gDebugBufferReady = true;
static bool ensureDebugBuffer() { return gDebugBufferReady; }
static char* getDebugBuffer() { return gDebugBufferReady ? gDebugBuffer : nullptr; }
static void cliHint(const char* text) { w.hints.emplace_back(text ? text : ""); }
static bool gCLIValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { if (::gCLIValidateOnly) return "VALID"; } while (0)

static struct Settings {
  bool bleEnabled = true, bleAutoStart = false, bleRequireAuth = true;
  String bleDeviceName = "HardwareOne";
  int bleTxPower = 3, bleMode = 1;
  bool bleRequireSecureChannel = true;
  String bleSecureChannelSecret = "Pass-phrase1!";
  String phonePipeSecret;
  bool bleConsoleOnPipe = true;
  bool phonePipeSecretGenerated = false;  // its settings row (phone link v2)
} gSettings;
template <typename T, typename V>
static void setSetting(T& field, const V& value) {
  if (field == static_cast<T>(value)) return;
  field = static_cast<T>(value);
  ++w.settingWrites;
}

// Transport sessions: never-repeating epochs, so a reused id is visible.
enum HostCommandSource { SOURCE_BLUETOOTH = 1 };
static TransportSessionEpoch nextEpoch = 100;
static TransportSessionEpoch transportSessionOpen(HostCommandSource) {
  if (w.failSessionOpen) return 0;
  ++w.sessionOpens;
  return ++nextEpoch;
}
static void transportSessionClose(HostCommandSource, TransportSessionEpoch epoch) {
  if (epoch) ++w.sessionCloses;
}
static std::atomic<TransportSessionEpoch> sBleSessionEpoch[BLE_MAX_CONNECTIONS];

// The pipe (BLE_PhonePipe.h).
static bool phonePipeLinkRecord(PhonePipeLinkRecord* out) {
  ++w.recordReads;
  if (out) *out = w.recordValid ? w.record : PhonePipeLinkRecord{};
  return w.recordValid;
}
static void phonePipeNoteConsoleActivity() {
  ++w.activity;
  w.log.emplace_back("activity");
}
static bool phonePipeActive() { return w.pipeActive; }
static bool phonePipeStandalone() { return false; }
static bool phonePipeReady() { return w.pipeReady; }
static bool phonePipeEnrolling() { return false; }
static uint8_t phonePipeStep() { return uint8_t(hw1link::PipeStep::Link); }
static const char* phonePipeStatus() { return w.pipeReady ? "ready" : "authenticating pipe"; }
// The phone link block's inputs (phone link v2): design A's suite offers no
// console stream (no device code), so its words are design A's.
static hw1link::PhoneLinkInputs bleLinkPhoneInputs() { return {}; }
static bool blePeerReconnectSnapshot(BlePeerKind, BlePeerReconnectSnapshot& out) {
  out = BlePeerReconnectSnapshot{};
  out.hasSavedTarget = true;
  return true;
}
static bool isG2ClientInitialized() { return w.g2Client; }
static bool bleServerUp() { return w.serverUp; }

// The Secure Channel (System_BleSecureChannel.h): established per link; an
// encrypted send is one frame through the shipped directed sender.
struct BleScBinding { uint16_t connId = 0; TransportSessionEpoch epoch = 0; uint32_t generation = 0; };
static bool bleScRequired() { return w.scRequired; }
static void bleScReset(uint16_t connId) {
  ++w.scResets;
  w.scEstablished.erase(connId);
}
static bool bleScEstablished(uint16_t connId) { return w.scEstablished.count(connId) != 0; }
static bool bleScBind(uint16_t connId, TransportSessionEpoch epoch, BleScBinding* out) {
  if (!epoch || !bleScEstablished(connId)) return false;
  if (out) *out = BleScBinding{connId, epoch, 1};
  return true;
}
bool bleRawNotifyToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                           const char* data, size_t len, bool blocking);
bool sendBLEResponseToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking = true);
static bool bleScSendEncryptedBound(const BleScBinding& binding, const char* text, size_t len,
                                    bool blocking, bool = false) {
  const std::string frame = "\x10" "enc:" + std::string(text, len);
  return bleRawNotifyToSession(binding.connId, binding.epoch, frame.data(), frame.size(), blocking);
}

// The shared central TX gate (BLE_CentralTx.h).
static bool bleCentralTxTryPhone(bool bypassPriority) {
  ++w.gateTries;
  w.log.emplace_back(gLifecycle.depth ? "gate-try-under-lock" : "gate-try");
  if (bypassPriority) ++w.gateBypasses;
  if (w.gateBusy || (!bypassPriority && w.accessoryPressure)) return false;
  ++w.gateHeld;
  return true;
}
static void bleCentralTxGive() {
  --w.gateHeld;
  w.log.emplace_back(gLifecycle.depth ? "gate-give-under-lock" : "gate-give");
}

// Events (System_Events.h).
enum HostSystemEvent : uint8_t { SYSEVT_BLE_CONNECTED = 1, SYSEVT_BLE_DISCONNECTED };
static void systemEventPost(uint8_t kind, const char* subject = nullptr, const char* detail = nullptr) {
  if (kind == SYSEVT_BLE_CONNECTED) {
    ++w.connectedEvents;
    w.connectedSubject = subject ? subject : "";
    w.connectedDetail = detail ? detail : "";
  } else if (kind == SYSEVT_BLE_DISCONNECTED) {
    ++w.disconnectedEvents;
  }
}

// IDF GATTS and the Arduino BLE objects.
#define ESP_GATT_IF_NONE 0xff
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
using esp_gatt_if_t = uint8_t;
struct esp_ble_gatts_cb_param_t {
  struct { uint16_t conn_id; uint8_t remote_bda[6]; } connect;
  struct { uint16_t conn_id; } disconnect;
  struct { uint16_t conn_id; } write;
  struct { uint16_t conn_id; } read;
};
static std::atomic<int> sBleGattsIf{ESP_GATT_IF_NONE};
static std::atomic<bool> sBleAdvertising{false};
// The post waits forever for room on BTC_TASK's queue, and BTC_TASK takes the
// lifecycle lock for the phone's writes and link events: on the client host it
// must run with that lock released and counted in flight (the teardown waits
// for it). Server mode posts under the lock, as before.
static unsigned postsUnderLock = 0;
static esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t gattsIf, uint16_t connId, uint16_t,
                                             size_t len, uint8_t* value, bool needConfirm) {
  if (gattsIf == ESP_GATT_IF_NONE || needConfirm) return ESP_FAIL;
  if (sBleConsoleMode.load() == hw1console::Mode::ClientRegistered) {
    if (gLifecycle.depth != 0 || sBleConsoleSendsInFlight.load() != 1) {
      ++postsUnderLock;
      w.log.emplace_back("send-under-lock");
    }
  } else if (gLifecycle.depth == 0) {
    return ESP_FAIL;
  }
  w.log.emplace_back(w.gateHeld ? "send" : "send-ungated");
  w.sentTo.push_back(connId);
  w.sentText.emplace_back(reinterpret_cast<const char*>(value), len);
  return ESP_OK;
}
struct BLECharacteristic {
  std::string value;
  const uint8_t* getData() { return reinterpret_cast<const uint8_t*>(value.c_str()); }
  size_t getLength() { return value.size(); }
  uint16_t getHandle() const { return 0x2a; }
  void setValue(const char* text) { value = text ? text : ""; }
};
struct BLECharacteristicCallbacks {
  virtual ~BLECharacteristicCallbacks() = default;
  virtual void onWrite(BLECharacteristic*, esp_ble_gatts_cb_param_t*) {}
  virtual void onRead(BLECharacteristic*, esp_ble_gatts_cb_param_t*) {}
};
struct BLEServer;
struct BLEServerCallbacks {
  virtual ~BLEServerCallbacks() = default;
  virtual void onConnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
  virtual void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
};
struct BLEServer {
  void disconnect(uint16_t) { ++w.serverCloses; }  // esp_ble_gatts_close
};
struct BLEService {};
struct BLEAdvertising {
  bool start() { ++w.advStarts; return true; }
};
struct BLEDevice {
  static void stopAdvertising() { ++w.advStops; }
  static void setCustomGattsHandler(void*) {}
};
static BLEServer gServer;
static BLEService gService;
static BLECharacteristic gRequest, gResponse, gStatus;
static BLEAdvertising gAdvertising;
static BLEServer* pServer = nullptr;
static BLEAdvertising* pAdvertising = nullptr;
static BLEService* pCommandService = nullptr;
static BLECharacteristic* pCmdRequestChar = nullptr;
static BLECharacteristic* pCmdResponseChar = nullptr;
static BLECharacteristic* pCmdStatusChar = nullptr;
static BLEDeviceType bleIdentifyDeviceByMAC(const uint8_t*) { return BLE_DEVICE_UNKNOWN; }

// Allocation (System_MemUtil.h).
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::calloc(1, size); }
template <typename T>
static void ps_delete(T* object) {
  if (!object) return;
  object->~T();
  std::free(object);
}
BLESystemState* gBLEState = nullptr;

// The command tail and the deferred jobs (cmd_exec): what would run.
static void processIncomingBLECommand(uint16_t connId, const char*, size_t) {
  w.dispatched.push_back(connId);
  w.log.emplace_back(gLifecycle.depth ? "dispatch-under-lock" : "dispatch");
}
static void processBleCommandLine(uint16_t, TransportSessionEpoch, const char* data, size_t len,
                                  bool) {
  w.ran.emplace_back(data, len);
}
static void bleScPoolRelease(BleScDeferred*) { ++w.poolReleases; }

// Server-mode advertising behind `bleadv` and the main loop's stream work.
static bool startBLEAdvertising() { ++w.advCommandStarts; return true; }
static void stopBLEAdvertising() {}
static bool bleAdvertising() { return false; }
static void otaBleUploadHousekeeping() {}
static void bleSessionTick() { ++w.sessionTicks; }
// The phone link's console row (phone link v2): `outble`, and the lifecycle
// mutex the pipe's hold creates when no console host has.
static volatile uint32_t gOutputFlags = 0;
static bool bleInitLifecycleMutex() { return true; }
static bool bleIsStreamEnabled(uint8_t) { return false; }
static void buildSensorDataJSON(char*, size_t) {}
static void buildSystemStatusJSON(char*, size_t) {}
static bool blePushSensorData(const char*, size_t) { return false; }
static bool blePushSystemStatus(const char*, size_t) { return false; }

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static unsigned checks = 0, failures = 0;
static std::string currentCase = "setup";
#define EXPECT(condition, message)                                            \
  do {                                                                        \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
      ++failures;                                                             \
      std::printf("FAIL [%s]: %s\n", currentCase.c_str(), message);           \
    }                                                                         \
  } while (0)

static constexpr uint16_t kLeft = 0, kRight = 1, kRing = 2, kPhone = 3;

// Another task's directed post, admitted before the teardown closed the epochs,
// lands now (bleRawNotifyToSession's fetch_sub).
static unsigned landedPosts = 0;
static void landPostInFlight() {
  ++landedPosts;
  sBleConsoleSendsInFlight.fetch_sub(1);
}

static void dropState() {
  if (gBLEState) ps_delete(gBLEState);
  gBLEState = nullptr;
}

// The client host as bleConsoleRegisterOnClientHost leaves it (its own suite,
// test_ble_server_stop, runs the registration): the fresh state, the command
// service, the tap's interface id, ClientRegistered with admission open.
static void clientHost(const std::string& name) {
  currentCase = name;
  dropState();
  w = World{};
  gSettings = Settings{};
  gLifecycle = HostMutex{};
  lifecycleBusy = false;
  gCLIValidateOnly = false;
  gDebugBufferReady = true;
  backoffs = 0;
  landingAfterDelays = -1;
  landedPosts = postsUnderLock = 0;
  sBleConsoleSendsInFlight.store(0);
  for (auto& epoch : sBleSessionEpoch) epoch.store(0);
  gBLEState = bleNewState();
  sBleConsoleMode.store(hw1console::Mode::ClientRegistered);
  sBleConsoleAdmissionOpen.store(true);
  sBleConsoleAttach = hw1console::Attach{};
  sBleConsoleLogged = hw1console::Slot{};
  sBleGattsIf.store(4);
  sBleAdvertising.store(false);
  pServer = &gServer;
  pAdvertising = nullptr;
  pCommandService = &gService;
  pCmdRequestChar = &gRequest;
  pCmdResponseChar = &gResponse;
  pCmdStatusChar = &gStatus;
}

// Bluedroid's link events, the phone app's writes and the main loop.
static void connect(uint16_t id) {
  esp_ble_gatts_cb_param_t p{};
  p.connect.conn_id = id;
  for (int i = 0; i < 6; ++i) p.connect.remote_bda[i] = uint8_t(0xA0 + id);
  BLEServerCallbacks& callbacks = sServerCallbacks;  // Arduino calls through the base
  callbacks.onConnect(&gServer, &p);
}
static void disconnect(uint16_t id) {
  esp_ble_gatts_cb_param_t p{};
  p.disconnect.conn_id = id;
  BLEServerCallbacks& callbacks = sServerCallbacks;
  callbacks.onDisconnect(&gServer, &p);
}
static bool write(uint16_t id, const char* text = "\x01hello") {
  const size_t before = w.dispatched.size();
  esp_ble_gatts_cb_param_t p{};
  p.write.conn_id = id;
  gRequest.value = text;
  BLECharacteristicCallbacks& callbacks = sCmdRequestCallbacks;
  callbacks.onWrite(&gRequest, &p);
  return w.dispatched.size() == before + 1 && w.dispatched.back() == id;
}
static void mainLoop() { bleUpdateStreams(); }
// A read of the status characteristic (…de03) from a link.
static std::string statusRead(uint16_t id) {
  esp_ble_gatts_cb_param_t p{};
  p.read.conn_id = id;
  gStatus.value.clear();
  BLECharacteristicCallbacks& callbacks = sCmdStatusCallbacks;
  callbacks.onRead(&gStatus, &p);
  return gStatus.value;
}
static void ready(uint32_t generation, uint16_t id) {
  w.record = PhonePipeLinkRecord{generation, id};
  w.recordValid = true;
  w.pipeReady = true;
}
static void pipeEnds() {
  w.recordValid = false;
  w.record = PhonePipeLinkRecord{};
  w.pipeReady = false;
}

static int slotsOn(uint16_t id) {
  int n = 0;
  for (const auto& c : gBLEState->connections) n += c.active && c.connId == id;
  return n;
}
static int slots() {
  int n = 0;
  for (const auto& c : gBLEState->connections) n += c.active;
  return n;
}
static bool slotIs(uint32_t generation, uint16_t id) {
  return sBleConsoleAttach.slot.present && sBleConsoleAttach.slot.generation == generation &&
         sBleConsoleAttach.slot.connId == id && slots() == 1 && slotsOn(id) == 1 &&
         bleSessionEpochForConnection(id) != 0;
}

// `bledisconnect`'s reply when a console on the phone link ends its session:
// design A's slot (G2 mode) and the pipe's console row (phone link v2) alike.
static const char kSessionEndedText[] =
    "App console session ended; the phone link stays and the console starts a new "
    "signed-out session";

// What holds on the client host after every step.
static void expectInvariants(const char* where) {
  std::string why;
  const int n = slots();
  // A slot counts as a connected app only once the app has joined it.
  const int joined = sBleConsoleAttach.slot.present && sBleConsoleAttach.slot.joined ? 1 : 0;
  if (n > 1) why += " more than one slot;";
  if (joined != gBLEState->activeConnectionCount) why += " count is not the joined slot;";
  if ((gBLEState->connectionState == BLE_STATE_CONNECTED) != (joined > 0)) why += " state is not the joined slot;";
  if ((n == 1) != sBleConsoleAttach.slot.present) why += " attach record is not the slot;";
  if (!sBleConsoleAttach.slot.present && sBleConsoleAttach.slot.joined) why += " joined without a slot;";
  if (sBleConsoleSendsInFlight.load() != 0) why += " a post left in flight;";
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
    const BLEConnection& c = gBLEState->connections[i];
    if (!c.active) continue;
    if (c.deviceType != BLE_DEVICE_PHONE) why += " a slot is not typed Phone;";
    if (c.connId != sBleConsoleAttach.slot.connId) why += " a slot is off the attach record's link;";
    if (sBleSessionEpoch[i].load() == 0) why += " a slot has no session;";
    for (uint8_t b : c.deviceAddr) if (b) { why += " a slot stores an address;"; break; }
  }
  if (w.advStarts || w.advStops || w.serverCloses) why += " advertising or a GATT close;";
  if (gLifecycle.depth || w.gateHeld) why += " the lifecycle lock or the gate leaked;";
  for (const auto& entry : w.log) {
    if (entry == "gate-try-under-lock" || entry == "gate-give-under-lock" || entry == "send-ungated" ||
        entry == "dispatch-under-lock" || entry == "send-under-lock") {
      why += " " + entry + ";";
      break;
    }
  }
  if (!why.empty()) EXPECT(false, (std::string(where) + ":" + why).c_str());
}

// T1: the temples and the ring never become a slot, whatever events they see
// and wherever the pipe's link is; only the ready pipe's link does, once.
static void linkTable() {
  for (uint16_t phone = 0; phone < 4; ++phone) {
    for (uint16_t other = 0; other < 4; ++other) {
      if (other == phone) continue;
      for (int order = 0; order < 8; ++order) {
        clientHost("T1 pipe on " + std::to_string(phone) + ", events on " + std::to_string(other) +
                   " #" + std::to_string(order));
        for (uint16_t id = 0; id < 4; ++id) connect(id);  // every link comes up
        expectInvariants("links up");
        EXPECT(slots() == 0 && w.connectedEvents == 0 && w.sessionOpens == 0,
               "a link event made a slot or posted an event");
        ready(7, phone);
        if (order & 1) {
          // READY makes the slot; it is not an app until the app writes.
          mainLoop();
          EXPECT(slotIs(7, phone) && w.connectedEvents == 0 && gBLEState->activeConnectionCount == 0 &&
                     !isBLEConnected(),
                 "a slot no app had joined counted as a connected app");
          EXPECT(write(phone), "the phone's first write was not admitted");
        } else {
          EXPECT(write(phone), "the phone's first write was not admitted by the fast path");
        }
        EXPECT(slotIs(7, phone) && w.connectedEvents == 1 && w.connectedSubject == "Phone" &&
                   w.connectedDetail.empty() && isBLEConnected(),
               "the app's first write did not join the one Phone slot (no address in the event)");
        const TransportSessionEpoch epoch = bleSessionEpochForConnection(phone);
        // The other link drops, comes back and writes, in either order. Its
        // link events never wait for the lifecycle lock (BTC).
        const unsigned takes = w.lockTakes;
        disconnect(other);
        connect(other);
        EXPECT(w.lockTakes == takes, "a temple or ring link event took the lifecycle lock");
        if (order & 2) { disconnect(other); connect(other); }
        EXPECT(!write(other), "a temple or ring write was dispatched");
        if (!(order & 2)) { disconnect(other); connect(other); }
        if (order & 4) mainLoop();
        EXPECT(!write(other), "a temple or ring write was dispatched after its link changed");
        expectInvariants("other link's events");
        EXPECT(slotIs(7, phone) && bleSessionEpochForConnection(phone) == epoch &&
                   slotsOn(other) == 0 && w.disconnectedEvents == 0 && w.connectedEvents == 1,
               "another link's events touched the phone slot or got one");
        EXPECT(write(phone) && w.activity >= 1, "the phone's write was not admitted as pipe activity");
      }
    }
  }
}

// T2: a reused connection id never inherits the slot or its session.
static void reuseCases() {
  // A drop, then a temple on the phone's old id while the pipe's record is
  // still set: no slot, no dispatch; the pipe's next session attaches anew.
  clientHost("T2a drop, then reuse under a stale record");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  EXPECT(write(kPhone), "test setup: the app joins");
  const TransportSessionEpoch old = bleSessionEpochForConnection(kPhone);
  EXPECT(slotIs(5, kPhone), "test setup: the slot");
  disconnect(kPhone);
  EXPECT(slots() == 0 && w.disconnectedEvents == 1 && gBLEState->connectionState == BLE_STATE_IDLE,
         "the phone link's drop did not end its slot");
  connect(kPhone);  // a temple reconnects on id 3
  mainLoop();
  EXPECT(!write(kPhone) && slots() == 0 && bleSessionEpochForConnection(kPhone) == 0,
         "a temple on the phone's old id inherited the console");
  pipeEnds();
  mainLoop();
  const unsigned takes = w.lockTakes;
  disconnect(kPhone);  // the temple drops: no slot or record on the id any more
  EXPECT(w.lockTakes == takes, "an event on an id with no slot or record took the lifecycle lock");
  connect(kPhone);  // the pipe's next link, on the same id
  ready(6, kPhone);
  mainLoop();
  EXPECT(slotIs(6, kPhone) && bleSessionEpochForConnection(kPhone) != old,
         "the pipe's next session did not get a new slot and session");
  expectInvariants("T2a");

  // A missed disconnect: the next link's connect on that id ends the slot.
  clientHost("T2b missed disconnect");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  EXPECT(write(kPhone), "test setup: the app joins");
  connect(kPhone);  // no disconnect was seen
  EXPECT(slots() == 0 && w.disconnectedEvents == 1, "a connect on the slot's id did not end it");
  mainLoop();
  EXPECT(!write(kPhone) && slots() == 0, "the stale record re-attached the new link");
  expectInvariants("T2b");

  // READY races the drop (Bluedroid tells the server first): the slot lands
  // on a dead id, and the id's next link ends it before anything is sent.
  clientHost("T2c READY racing the drop");
  connect(kPhone);
  disconnect(kPhone);
  ready(5, kPhone);
  mainLoop();
  EXPECT(slotIs(5, kPhone), "test setup: a slot on the dead id");
  connect(kPhone);  // the next link on the id: a temple, after the pipe's turn
  EXPECT(slots() == 0 && !write(kPhone), "the id's next link inherited the raced slot");
  mainLoop();
  EXPECT(slots() == 0, "the raced generation attached again");
  expectInvariants("T2c");

  // The pipe changes generation on the same id: a new session, the old one
  // ended first (never renamed).
  clientHost("T2d generation change on one id");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  EXPECT(write(kPhone), "test setup: the app joins");
  const TransportSessionEpoch first = bleSessionEpochForConnection(kPhone);
  const unsigned closes = w.sessionCloses;
  ready(6, kPhone);
  mainLoop();
  EXPECT(write(kPhone), "test setup: the app joins the new session");
  EXPECT(slotIs(6, kPhone) && bleSessionEpochForConnection(kPhone) != first &&
             w.sessionCloses > closes && w.disconnectedEvents == 1 && w.connectedEvents == 2,
         "a new generation reused the old session");
  expectInvariants("T2d");
}

// S1: the main loop reconciles before its no-client return; S2: the fast path.
static void mainLoopAndFastPath() {
  clientHost("S1 main loop reconciles with no client");
  connect(kPhone);
  mainLoop();
  EXPECT(slots() == 0 && w.sessionTicks == 0, "a slot without a ready pipe, or the no-client return skipped");
  ready(5, kPhone);
  mainLoop();
  EXPECT(slotIs(5, kPhone) && w.sessionTicks == 0,
         "the main loop did not attach, or ticked a session no app had joined");
  EXPECT(write(kPhone), "the app's first write was not admitted");
  mainLoop();
  EXPECT(w.sessionTicks == 1, "the main loop did not tick the joined session");
  pipeEnds();
  mainLoop();
  EXPECT(slots() == 0 && w.disconnectedEvents == 1 && w.sessionTicks == 1,
         "the pipe's end did not end the slot on the next pass");
  EXPECT(w.debug.size() >= 2 &&
             std::count(w.debug.begin(), w.debug.end(),
                        std::string("App console attached to the phone pipe's link")) == 1 &&
             std::count(w.debug.begin(), w.debug.end(),
                        std::string("App console session on the phone link ended")) == 1,
         "the attach and the end were not each logged once");
  expectInvariants("S1");

  clientHost("S2 the first write attaches (BTC fast path)");
  connect(kPhone);
  ready(5, kPhone);
  EXPECT(write(kPhone) && slotIs(5, kPhone) && gBLEState->commandsReceived == 1 && w.activity == 1 &&
             w.connectedEvents == 1 && sBleConsoleAttach.slot.joined,
         "the first write did not attach, join, count and note activity");
  EXPECT(write(kPhone) && w.connectedEvents == 1, "a later write joined the slot again");
  mainLoop();  // the next pass logs the attach the fast path made
  EXPECT(std::count(w.debug.begin(), w.debug.end(),
                    std::string("App console attached to the phone pipe's link")) == 1,
         "a fast-path attach was not logged by the main loop");
  EXPECT(!write(kLeft) && gBLEState->commandsReceived == 2, "a temple write was counted or dispatched");
  // A command from the phone link names no address in its audit identity.
  EXPECT(std::strcmp(blePeerIpTag(kPhone).c_str(), "ble:pipe-link") == 0 &&
             std::strcmp(blePeerIpTag(kLeft).c_str(), "ble") == 0,
         "the phone-link audit identity is not the address-free tag");
  expectInvariants("S2");

  clientHost("S2b no attach without the setting, a secret or admission");
  connect(kPhone);
  ready(5, kPhone);
  gSettings.bleConsoleOnPipe = false;
  EXPECT(!write(kPhone) && slots() == 0 && w.activity == 0, "bleconsole off still attached");
  mainLoop();
  EXPECT(slots() == 0, "bleconsole off attached in the main loop");
  gSettings.bleConsoleOnPipe = true;
  gSettings.bleSecureChannelSecret = "";
  EXPECT(!write(kPhone) && slots() == 0, "attached without blesecret");
  gSettings.bleSecureChannelSecret = "Pass-phrase1!";
  sBleConsoleAdmissionOpen.store(false);
  EXPECT(!write(kPhone) && slots() == 0, "attached with admission closed");
  sBleConsoleAdmissionOpen.store(true);
  EXPECT(write(kPhone) && slotIs(5, kPhone), "attach did not follow once allowed");
  gSettings.bleSecureChannelSecret = "";  // blesecret clear while attached
  mainLoop();
  EXPECT(slots() == 0 && w.disconnectedEvents == 1, "clearing blesecret did not end the slot");
  expectInvariants("S2b");

  // A joined slot whose pipe left READY takes no command, even before the main
  // loop's next pass: the write reconciles first and ends the slot. The same
  // for the setting turned off outside `bleconsole` and blesecret cleared.
  for (int cause = 0; cause < 3; ++cause) {
    clientHost(cause == 0 ? "S2d pipe left READY: the next write is refused"
               : cause == 1 ? "S2d setting off: the next write is refused"
                            : "S2d blesecret cleared: the next write is refused");
    connect(kPhone);
    ready(5, kPhone);
    EXPECT(write(kPhone) && slotIs(5, kPhone), "test setup: a joined slot");
    if (cause == 0) pipeEnds();
    if (cause == 1) gSettings.bleConsoleOnPipe = false;
    if (cause == 2) gSettings.bleSecureChannelSecret = "";
    const size_t ran = w.dispatched.size();
    EXPECT(!write(kPhone) && w.dispatched.size() == ran && slots() == 0 &&
               bleSessionEpochForConnection(kPhone) == 0 && w.disconnectedEvents == 1,
           "a write after the slot's conditions went was dispatched, or the slot outlived it");
    expectInvariants("S2d");
  }

  clientHost("S2c a session that could not open is retried");
  connect(kPhone);
  ready(5, kPhone);
  w.failSessionOpen = true;
  EXPECT(!write(kPhone) && slots() == 0 && !sBleConsoleAttach.slot.present,
         "a slot without a session was kept");
  w.failSessionOpen = false;
  EXPECT(write(kPhone) && slotIs(5, kPhone), "the next write did not retry the attach");
  expectInvariants("S2c");
}

// S3: Secure Channel only; S4: every console frame is one gate try, before the lock.
static void secureAndGate() {
  clientHost("S3 plaintext refused, encrypted output only");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  const TransportSessionEpoch epoch = bleSessionEpochForConnection(kPhone);
  EXPECT(!w.scRequired && !sendBLEResponseToSession(kPhone, epoch, "result", 6) && w.sentTo.empty(),
         "plaintext output reached the phone link");
  BleScDeferred job{};
  job.connId = kPhone;
  job.sessionEpoch = epoch;
  std::memcpy(job.buf, "status", 6);
  job.len = 6;
  blePlaintextDeferredInbound(&job);
  EXPECT(w.ran.empty() && w.sentTo.size() == 1 && w.sentTo[0] == kPhone &&
             w.sentText[0].find("Secure channel required") == 0 && w.poolReleases == 1,
         "a plaintext command ran, or the fixed refusal did not reach only the writer");
  w.scEstablished.insert(kPhone);
  w.log.clear();
  EXPECT(sendBLEResponseToSession(kPhone, epoch, "result", 6) && w.sentText.back() == "\x10" "enc:result",
         "an encrypted result was not sent");
  const std::vector<std::string> order{"gate-try", "send", "activity", "gate-give"};
  EXPECT(w.log == order, "the frame did not go gate try, send under the lock, activity, gate give");
  expectInvariants("S3");

  clientHost("S4 the gate: reliable past the soft priority, best effort yields, busy retries");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  w.scEstablished.insert(kPhone);
  const TransportSessionEpoch e = bleSessionEpochForConnection(kPhone);
  w.accessoryPressure = true;
  EXPECT(sendBLEResponseToSession(kPhone, e, "r", 1, true) && w.gateBypasses == 1,
         "a reliable reply did not pass a glasses burst");
  EXPECT(!sendBLEResponseToSession(kPhone, e, "n", 1, false) && w.sentTo.size() == 1 &&
             w.gateTries == 2 && backoffs == 0,
         "a best-effort frame did not yield to the accessories, once, without a wait");
  w.accessoryPressure = false;
  w.gateBusy = true;
  const unsigned tries = w.gateTries;
  EXPECT(!sendBLEResponseToSession(kPhone, e, "b", 1, true) && w.gateTries == tries + 6 &&
             w.sentTo.size() == 1 && w.activity == 1,
         "a busy gate must be six bounded tries and nothing sent");
  w.gateBusy = false;
  expectInvariants("S4");

  // Server mode: no gate, no activity, plaintext as the operator chose.
  clientHost("S4b server mode is not gated");
  sBleConsoleMode.store(hw1console::Mode::Server);
  gBLEState->initialized = true;
  connect(kPhone);  // the server path admits the app
  const TransportSessionEpoch se = bleSessionEpochForConnection(kPhone);
  EXPECT(se != 0 && w.advStarts == 0 && w.connectedEvents == 1, "test setup: a server-mode app");
  w.advStops = 0;  // the server path's own advertising re-arm (no advertising object here)
  EXPECT(sendBLEResponseToSession(kPhone, se, "p", 1) && w.gateTries == 0 && w.activity == 0,
         "a server-mode send was gated or counted as pipe activity");
}

// S5: bledisconnect ends the session, never the link; S6: bleadv refuses.
static void commands() {
  clientHost("S5 bledisconnect <connId> ends the session only");
  connect(kLeft);
  connect(kRight);
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  // A slot no app has joined is no client: nothing to end.
  const char* reply = cmd_bledisconnect("3");
  EXPECT(std::strcmp(reply, "Error: No client with that connection id") == 0 && slotIs(5, kPhone),
         "bledisconnect ended a slot no app had joined");
  reply = cmd_bledisconnect("");
  EXPECT(std::strcmp(reply, "Error: No client connected") == 0 && slotIs(5, kPhone),
         "bare bledisconnect ended a slot no app had joined");
  EXPECT(write(kPhone), "test setup: the app joins");
  const TransportSessionEpoch epoch = bleSessionEpochForConnection(kPhone);
  reply = cmd_bledisconnect("3");
  EXPECT(std::strcmp(reply, kSessionEndedText) == 0 &&
             w.serverCloses == 0 && slots() == 0 && bleSessionEpochForConnection(kPhone) == 0 &&
             w.disconnectedEvents == 1,
         "bledisconnect closed the link or kept the session");
  mainLoop();  // the pipe's link is still ready: a fresh session, waiting for the app
  EXPECT(slotIs(5, kPhone) && bleSessionEpochForConnection(kPhone) != epoch &&
             !sBleConsoleAttach.slot.joined && bleLinkStatus().console == hw1link::Console::WaitingPhone,
         "the phone link did not get a fresh session waiting for the app after bledisconnect");
  EXPECT(write(kPhone), "test setup: the app joins again");
  reply = cmd_bledisconnect("0");
  EXPECT(std::strcmp(reply, "Error: No client with that connection id") == 0 && slotIs(5, kPhone),
         "bledisconnect on a temple's id touched the console or claimed it");
  reply = cmd_bledisconnect("");
  EXPECT(std::strcmp(reply, kSessionEndedText) == 0 &&
             slots() == 0 && w.serverCloses == 0,
         "bare bledisconnect did not end the phone-link session");
  reply = cmd_bledisconnect("");
  EXPECT(std::strcmp(reply, "Error: No client connected") == 0, "bare bledisconnect without a slot");
  expectInvariants("S5");

  // No console host to ask (no server object) and the debug buffer never
  // allocated: the hint reply falls back instead of writing through null.
  clientHost("S5b bledisconnect with no console and no debug buffer");
  pServer = nullptr;
  gDebugBufferReady = false;
  reply = cmd_bledisconnect("3");
  EXPECT(std::strcmp(reply, "Error: App console not running") == 0,
         "bledisconnect without a debug buffer did not fall back");
  gDebugBufferReady = true;
  reply = cmd_bledisconnect("3");
  EXPECT(std::strncmp(reply, "Error: App console not running; ", 32) == 0,
         "bledisconnect without a console lost its hint");
  pServer = &gServer;

  clientHost("S6 bleadv refuses on the client host");
  for (const char* args : {"", "start", "stop", "toggle"}) {
    reply = cmd_bleadv(args);
    EXPECT(std::strcmp(reply, kBlePhoneLinkNoAdvertising) == 0 && w.advCommandStarts == 0,
           "bleadv on the client host did not refuse");
  }
  // Both refusals say what to use instead (test_ble_info_status runs the
  // stream commands' refusal).
  EXPECT(std::strstr(kBlePhoneLinkNoAdvertising, "'openconsole'") &&
             std::strstr(kBlePhoneLinkNoDataService, "'openconsole'"),
         "a client-host refusal does not name openconsole");
  sBleConsoleMode.store(hw1console::Mode::Server);
  EXPECT(std::strcmp(cmd_bleadv(""), "Advertising started") == 0 && w.advCommandStarts == 1,
         "server-mode bleadv changed");
}

// S7: bleconsole and its persisted setting.
static void consoleCommand() {
  clientHost("S7 bleconsole status, off, on");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  EXPECT(std::strcmp(cmd_bleconsole(""), "App console on the phone link: on (waiting for the phone link)") == 0,
         "bleconsole status before the app joined is not waiting");
  EXPECT(write(kPhone), "test setup: the app joins");
  EXPECT(std::strcmp(cmd_bleconsole(""), "App console on the phone link: on (app connected, signed out)") == 0 &&
             std::strcmp(cmd_bleconsole("status"), "App console on the phone link: on (app connected, signed out)") == 0 &&
             w.settingWrites == 0 && w.hints.empty(),
         "bleconsole status words changed");
  EXPECT(std::strcmp(cmd_bleconsole("off"), "App console on the phone link: off") == 0 &&
             !gSettings.bleConsoleOnPipe && w.settingWrites == 1 && slots() == 0 &&
             w.disconnectedEvents == 1,
         "bleconsole off did not persist and end the session at once");
  EXPECT(std::strcmp(cmd_bleconsole("0"), "App console on the phone link: off") == 0 &&
             w.settingWrites == 1,
         "a repeated off wrote the setting again");
  EXPECT(std::strcmp(cmd_bleconsole("ON"), "App console on the phone link: on (waiting for the phone link)") == 0 &&
             gSettings.bleConsoleOnPipe && w.settingWrites == 2 && slotIs(5, kPhone) &&
             !sBleConsoleAttach.slot.joined,
         "bleconsole on did not persist and attach a fresh session waiting for the app");
  EXPECT(std::strcmp(cmd_bleconsole("maybe"), "Error: invalid arguments — Usage: bleconsole [on|off|status]") == 0 &&
             w.settingWrites == 2,
         "an invalid argument was not a usage error");
  gCLIValidateOnly = true;
  EXPECT(std::strcmp(cmd_bleconsole("off"), "VALID") == 0 && gSettings.bleConsoleOnPipe,
         "validate-only bleconsole changed the setting");
  gCLIValidateOnly = false;
  pipeEnds();
  mainLoop();
  EXPECT(std::strcmp(cmd_bleconsole(""), "App console on the phone link: on (waiting for the phone link)") == 0,
         "bleconsole status while the pipe is down changed");
  gSettings.bleSecureChannelSecret = "";
  EXPECT(std::strcmp(cmd_bleconsole(""), "App console on the phone link: on (set blesecret first)") == 0 &&
             w.hints.size() == 1 && w.hints[0].find("blesecret") != std::string::npos,
         "bleconsole without blesecret did not say so");
  expectInvariants("S7");

  clientHost("S7b bleconsole outside glasses mode");
  sBleConsoleMode.store(hw1console::Mode::Off);
  w.g2Client = false;
  w.pipeActive = false;
  dropState();
  gBLEState = bleNewState();
  EXPECT(std::strcmp(cmd_bleconsole(""), "App console on the phone link: on (used while the phone link runs)") == 0,
         "bleconsole outside a client role changed");
  EXPECT(std::strcmp(cmd_bleconsole("off"), "App console on the phone link: off") == 0 &&
             w.settingWrites == 1, "bleconsole off outside glasses mode did not persist");

  // The settings row: persisted, default on, edited through bleconsole.
  currentCase = "S7c settings row";
  const SettingEntry* row = nullptr;
  for (size_t i = 0; i < bluetoothSettingsCount; ++i) {
    if (std::strcmp(bluetoothSettingsEntries[i].jsonKey, "bleConsoleOnPipe") == 0) row = &bluetoothSettingsEntries[i];
  }
  EXPECT(row && row->type == SETTING_BOOL && row->valuePtr == &gSettings.bleConsoleOnPipe &&
             row->intDefault == 1 && row->cmdKey && std::strcmp(row->cmdKey, "bleconsole") == 0 &&
             !row->isSecret && !row->readOnly,
         "the bleConsoleOnPipe setting row is wrong");
}

// S8: the teardown hook ends the slot and resets the attach state.
static void teardown() {
  clientHost("S8 teardown ends the slot and resets the attach state");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  disconnect(kPhone);
  connect(kPhone);
  ready(6, kPhone);
  mainLoop();
  EXPECT(slotIs(6, kPhone) && sBleConsoleAttach.closed, "test setup: a slot and a closed generation");
  const unsigned closes = w.sessionCloses;
  bleConsoleHostTeardownBegin();
  EXPECT(slots() == 0 && w.sessionCloses == closes + 1 && !sBleConsoleAdmissionOpen.load() &&
             !sBleConsoleAttach.slot.present && !sBleConsoleAttach.closed && !pCmdResponseChar &&
             !pServer && gBLEState != nullptr,
         "the teardown did not end the slot, reset the attach state and drop the pointers");
  EXPECT(!write(kPhone), "a write was admitted during the teardown");
  mainLoop();
  EXPECT(slots() == 0, "the main loop re-attached during the teardown");
  bleConsoleHostTeardownEnd(true);
  EXPECT(!gBLEState && sBleConsoleMode.load() == hw1console::Mode::Off, "a terminal stop kept the state");
  mainLoop();  // no state, mode Off: nothing to reconcile
  EXPECT(gLifecycle.depth == 0 && w.gateHeld == 0, "the lock or the gate leaked");

  // A reply admitted before the epochs closed is still posting, the lifecycle
  // lock released (another task): the teardown waits for it to land before the
  // host stops, without holding the lock meanwhile.
  clientHost("S8b teardown waits for a post in flight");
  connect(kPhone);
  ready(5, kPhone);
  EXPECT(write(kPhone), "test setup: the app joins");
  sBleConsoleSendsInFlight.store(1);
  landingAfterDelays = 3;
  const unsigned delays = backoffs;
  bleConsoleHostTeardownBegin();
  EXPECT(landedPosts == 1 && sBleConsoleSendsInFlight.load() == 0 && backoffs - delays == 3 &&
             gLifecycle.depth == 0 && slots() == 0,
         "the teardown did not wait for the post in flight before the host stops");
  bleConsoleHostTeardownEnd(true);
}

// N: the pipe READY with no app is a session waiting for the app: no count,
// connection state or event, and none when it ends; the app's first write
// joins it. The status read answers only the slot's link.
static void joinAndStatusRead() {
  clientHost("N1 READY without the app, then the pipe drops");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  EXPECT(slotIs(5, kPhone) && w.connectedEvents == 0 && gBLEState->activeConnectionCount == 0 &&
             gBLEState->totalConnections == 0 && !isBLEConnected() &&
             bleLinkStatus().console == hw1link::Console::WaitingPhone,
         "a pipe READY with no app joined counted as a connected app");
  disconnect(kPhone);
  EXPECT(slots() == 0 && w.disconnectedEvents == 0 && w.connectedEvents == 0,
         "a slot no app joined posted an event when it ended");
  expectInvariants("N1");

  clientHost("N2 status read per link");
  connect(kLeft);
  connect(kRing);
  connect(kPhone);
  static const char kUnavailable[] = "{\"state\":\"unavailable\"}";
  EXPECT(statusRead(kPhone) == kUnavailable, "the status read answered the phone without a console session");
  ready(5, kPhone);
  mainLoop();
  // The app reads it while attaching, before its first write.
  EXPECT(statusRead(kPhone).find("{\"state\":\"connected\"") == 0,
         "the slot's link did not read the console status");
  for (uint16_t id : {kLeft, kRight, kRing}) {
    EXPECT(statusRead(id) == kUnavailable, "a temple or the ring read the console's counters");
  }
  sBleConsoleMode.store(hw1console::Mode::Server);  // server mode answers every app, as before
  EXPECT(statusRead(kLeft).find("{\"state\":\"connected\"") == 0, "server mode's status read changed");
}

// S9: server mode keeps its path.
static void serverMode() {
  clientHost("S9 server mode unchanged");
  sBleConsoleMode.store(hw1console::Mode::Server);
  gBLEState->initialized = true;
  pAdvertising = &gAdvertising;
  ready(5, kRing);  // a stale pipe record changes nothing in server mode
  connect(kRing);
  EXPECT(slots() == 1 && bleSessionEpochForConnection(kRing) != 0 && w.connectedEvents == 1 &&
             w.advStarts == 1 && !sBleConsoleAttach.slot.present && !sBleConsoleAttach.closed,
         "server mode no longer admits an app connect (slot, session, event, advertising)");
  EXPECT(write(kRing) && w.activity == 0, "a server-mode write was filtered or noted as pipe activity");
  EXPECT(std::strcmp(blePeerIpTag(kRing).c_str(), "ble:A2:A2:A2:A2:A2:A2") == 0,
         "a server-mode app no longer carries its address in the audit identity");
  mainLoop();
  EXPECT(w.sessionTicks == 1 && !sBleConsoleAttach.slot.present, "server mode reconciled a console slot");
  disconnect(kRing);
  EXPECT(slots() == 0 && w.disconnectedEvents == 1, "server mode no longer ends an app on its disconnect");
  EXPECT(std::strcmp(cmd_bledisconnect(""), "Error: No client connected") == 0, "server-mode bledisconnect");
  connect(kLeft);
  EXPECT(std::strcmp(cmd_bledisconnect(""), "Disconnecting client...") == 0 && w.serverCloses == 1,
         "server-mode bledisconnect no longer closes the app's link");
}

// S10: the console words on the status surfaces through the flow.
static void statusWords() {
  clientHost("S10 status words through the flow");
  auto console = [] { return bleLinkStatus().console; };
  EXPECT(bleLinkStatus().role == hw1link::Role::GlassesPipe && console() == hw1link::Console::WaitingPhone,
         "before the pipe is ready: waiting for the phone link");
  connect(kPhone);
  ready(5, kPhone);
  mainLoop();
  BleLinkStatus s = bleLinkStatus();
  EXPECT(s.console == hw1link::Console::WaitingPhone && s.consoleView.clients == 0 &&
             !s.consoleView.attached && !isBLEConnected() && w.connectedEvents == 0,
         "a pipe READY with no app joined reads as an app (status, count, connection state, event)");
  EXPECT(write(kPhone), "test setup: the app joins");
  s = bleLinkStatus();
  EXPECT(s.console == hw1link::Console::Connected && s.consoleView.clients == 1 &&
             !s.consoleView.up && !s.consoleView.advertising && s.consoleView.attached,
         "a joined phone link is not one connected app");
  for (auto& c : gBLEState->connections) {
    if (c.active) { c.authed = true; c.user = "alice"; }
  }
  EXPECT(console() == hw1link::Console::SignedIn, "a signed-in phone link is not signed in");
  w.pipeReady = false;  // the pipe left READY; the slot ends on the next pass
  EXPECT(console() == hw1link::Console::WaitingPhone, "a slot without a ready pipe still shows connected");
  w.pipeReady = true;
  gSettings.bleConsoleOnPipe = false;
  EXPECT(console() == hw1link::Console::Off, "bleconsole off is not off");
  gSettings.bleConsoleOnPipe = true;
  gSettings.bleSecureChannelSecret = "";
  EXPECT(bleLinkStatus().console == hw1link::Console::SignedIn, "the slot still counts until reconcile");
  mainLoop();
  EXPECT(console() == hw1link::Console::NoSecret, "no blesecret is not said");
  lifecycleBusy = true;  // a busy lock: the last reading, never a wait
  EXPECT(console() == hw1link::Console::NoSecret, "a busy lock changed the words");
  lifecycleBusy = false;
}

// A random walk over a model of the four C6 links through the shipped wiring.
// The pipe publishes READY only while its link is up or just after a drop
// that races it (never once the id is reused: the setup turn). After every
// step the client-host invariants hold, a slot sits on the very link its
// generation ran on (or on its dead, unreused id), and only that link's
// writes are dispatched.
struct Rng {
  uint64_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); }
  uint32_t below(uint32_t n) { return next() % n; }
};

static void walk(uint64_t seed, unsigned steps) {
  clientHost("W random walk " + std::to_string(seed));
  constexpr uint16_t kIds = 4;
  Rng rng{seed};
  uint32_t holder[kIds] = {};
  uint32_t serial = 0, gens = 0, pipeSerial = 0, pipeGen = 0;
  uint16_t pipeConn = 0;
  bool pipeUp = false, published = false;
  std::vector<uint32_t> genSerial(1, 0);
  unsigned made = 0, phoneWrites = 0, otherRefused = 0, ended = 0;
  auto freeId = [&](uint16_t* id) {
    uint16_t ids[kIds];
    unsigned n = 0;
    for (uint16_t i = 0; i < kIds; ++i) if (!holder[i]) ids[n++] = i;
    if (!n) return false;
    *id = ids[rng.below(n)];
    return true;
  };
  for (unsigned step = 0; step < steps; ++step) {
    const unsigned opensBefore = w.sessionOpens, endsBefore = w.disconnectedEvents;
    const uint32_t e = rng.below(14);
    uint16_t id = 0;
    if (e == 0 && freeId(&id)) {  // a temple or the ring links up
      holder[id] = ++serial;
      connect(id);
    } else if (e == 1 && !pipeSerial && freeId(&id)) {  // the pipe's next link
      holder[id] = ++serial;
      pipeSerial = serial;
      pipeConn = id;
      pipeGen = ++gens;
      genSerial.push_back(serial);
      pipeUp = true;
      published = false;
      connect(id);
    } else if (e == 2) {  // any link drops
      id = uint16_t(rng.below(kIds));
      if (!holder[id]) continue;
      if (holder[id] == pipeSerial) pipeUp = false;
      holder[id] = 0;
      disconnect(id);
    } else if (e == 3 && pipeSerial && !published && (pipeUp || !holder[pipeConn])) {
      ready(pipeGen, pipeConn);  // READY, possibly just after a racing drop
      published = true;
    } else if (e == 4 && pipeSerial && !pipeUp) {  // the pipe notices its drop
      pipeEnds();
      pipeSerial = 0;
    } else if (e == 5 && pipeUp && w.recordValid) {  // the pipe fails; its link drops later
      pipeEnds();
    } else if (e == 6) {
      gSettings.bleConsoleOnPipe = rng.below(4) != 0;
    } else if (e == 7) {
      gSettings.bleSecureChannelSecret = rng.below(8) ? "Pass-phrase1!" : "";
    } else if (e == 8 && rng.below(4) == 0) {
      (void)cmd_bledisconnect("");
    } else if (e == 9 || e == 10) {  // a write from a link that is up
      id = uint16_t(rng.below(kIds));
      if (!holder[id]) continue;
      const bool sent = write(id);
      if (sent) {
        ++phoneWrites;
        // The pipe's own link, its current generation's slot (a write in the
        // moment between the pipe leaving READY and the next pass is its own).
        EXPECT(holder[id] == pipeSerial && sBleConsoleAttach.slot.generation == pipeGen,
               "a write from a link that is not the ready pipe's was dispatched");
      } else if (holder[id] != pipeSerial) {
        ++otherRefused;
      }
    } else {
      mainLoop();
    }
    made += w.sessionOpens - opensBefore;
    ended += w.disconnectedEvents - endsBefore;
    if (sBleConsoleAttach.slot.present) {
      const auto& slot = sBleConsoleAttach.slot;
      EXPECT(slot.generation < genSerial.size() &&
                 (holder[slot.connId] == genSerial[slot.generation] || holder[slot.connId] == 0),
             "the slot sits on a link that is not its generation's (a reused id inherited it)");
    }
    expectInvariants("walk");
    if (failures > 20) return;
  }
  EXPECT(made > 50 && ended > 50 && phoneWrites > 50 && otherRefused > 50,
         "the walk did not reach the interesting paths");
}

// K6: `bledisconnect` on the pipe's console row (phone link v2, phone-client
// mode: the pipe holds the state, no command service on this host) ends that
// session only, with the same reply as design A's slot: the link stays (no
// GATT close), and the stream renews (CLOSE 2) so the console starts a new
// signed-out session on a fresh row.
static void pipeRowDisconnect() {
  currentCase = "K6 bledisconnect on the pipe's console row";
  dropState();
  w = World{};
  gSettings = Settings{};
  gLifecycle = HostMutex{};
  lifecycleBusy = false;
  for (auto& epoch : sBleSessionEpoch) epoch.store(0);
  sBleConsoleMode.store(hw1console::Mode::Off);
  sBleConsoleAdmissionOpen.store(false);
  sBleConsoleAttach = hw1console::Attach{};
  sBleStateHolders = 0;
  sBlePipeRowSlot = -1;
  sBlePipeIdentity = BlePipeConsoleIdentity{};
  gPipeHost = PipeHost{};
  pServer = nullptr;
  pCommandService = nullptr;
  constexpr uint32_t kGeneration = 7;
  const uint16_t row = blePipeConsoleRowId(kGeneration);
  EXPECT(blePipeConsoleStateAcquire() && blePipeConsoleBind(kGeneration, 2, kPhone),
         "test setup: the pipe's console row binds");
  const TransportSessionEpoch epoch = bleSessionEpochForConnection(row);
  char id[8];
  std::snprintf(id, sizeof(id), "%u", (unsigned)row);
  const char* reply = cmd_bledisconnect(id);
  std::vector<PipeHostCall> closes = pipeHostCloses();
  EXPECT(std::strcmp(reply, kSessionEndedText) == 0 && epoch != 0 &&
             bleSessionEpochForConnection(row) == 0 && w.serverCloses == 0 && closes.size() == 1 &&
             closes[0].generation == kGeneration && closes[0].streamId == 2 &&
             closes[0].reason == kBlePipeCloseRenewed,
         "bledisconnect <row> did not end only the row's session and renew its stream");
  // The renewed stream binds a new row: a new session, signed out.
  EXPECT(blePipeConsoleBind(kGeneration, 3, kPhone) && bleSessionEpochForConnection(row) != 0 &&
             bleSessionEpochForConnection(row) != epoch,
         "the renewed stream did not get a new session");
  reply = cmd_bledisconnect("");
  closes = pipeHostCloses();
  EXPECT(std::strcmp(reply, kSessionEndedText) == 0 && closes.size() == 2 &&
             closes[1].streamId == 3 && closes[1].reason == kBlePipeCloseRenewed && w.serverCloses == 0,
         "bare bledisconnect did not end the row's session the same way");
  blePipeConsoleStateRelease();
  EXPECT(!gBLEState && sBleStateHolders == 0, "the pipe's release left the state");
  sBleConsoleMode.store(hw1console::Mode::ClientRegistered);
}

int main() {
  pipeRowDisconnect();
  linkTable();
  reuseCases();
  mainLoopAndFastPath();
  secureAndGate();
  commands();
  consoleCommand();
  teardown();
  joinAndStatusRead();
  serverMode();
  statusWords();
  for (uint64_t seed : {0x9e3779b97f4a7c15ull, 0x2545f4914f6cdd1dull, 0xd1b54a32d192ed03ull, 42ull})
    walk(seed, 60000);
  dropState();
  std::printf("BLE console on the phone link: %u checks, %u failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
