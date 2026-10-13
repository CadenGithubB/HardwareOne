// Shipping definitions are inserted by test_ble_info_status.py.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <strings.h>  // pre-fix blestream parser uses strncasecmp
#include <thread>
#include <unordered_map>
#include <vector>

static std::atomic<unsigned> assertions{0};
static void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}
static bool stringAppendAllowed = true;
bool bleTestStringAppend() { return stringAppendAllowed; }
class TestAllocator : public ArduinoJson::Allocator {
 public:
  std::atomic<int> calls{0}, failAt{-1}, outstanding{0};
  std::unordered_map<void*, size_t> sizes;
  void* allocate(size_t size) override {
    if (calls.fetch_add(1) == failAt.load()) return nullptr;
    void* ptr = std::malloc(size);
    if (ptr) { ++outstanding; sizes[ptr] = size; }
    return ptr;
  }
  void deallocate(void* ptr) override {
    if (ptr) { --outstanding; sizes.erase(ptr); std::free(ptr); }
  }
  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    // ArduinoJson's StringBuilder requires shrinking realloc to succeed.
    // Retain the already-owned block for that operation; inject only new/grow
    // failures, which are valid failure sites in the vendored allocator API.
    if (size <= sizes.at(ptr)) return ptr;
    if (calls.fetch_add(1) == failAt.load()) return nullptr;
    void* result = std::realloc(ptr, size);
    if (result) { sizes.erase(ptr); sizes[result] = size; }
    return result;
  }
  void reset(int fail = -1) { calls = 0; failAt = fail; }
};
static TestAllocator jsonAllocator;
#define PSRAM_JSON_DOC(name) JsonDocument name(&jsonAllocator)
using TransportSessionEpoch = uint32_t;
// INSERT_TYPES

static BLESystemState state{};
static BLESystemState* gBLEState = &state;
static struct {
  String bleDeviceName = "Bench \"Bluetooth\"\\unit\n";
  int bleTxPower = 5, bleMode = 0;
  bool bleAutoStart = true, bleRequireAuth = true;
} gSettings;
static bool secureRequired = true;
static std::atomic<unsigned> secureCalls{0};
static bool bleScRequired() { ++secureCalls; return secureRequired; }
using TickType_t = uint32_t;
static constexpr TickType_t portMAX_DELAY = 0xffffffffu;
static constexpr int pdTRUE = 1;
static TickType_t pdMS_TO_TICKS(TickType_t value) { return value; }
static std::recursive_timed_mutex lifecycleMutex;
static void* sBleLifecycleMutex = &lifecycleMutex;
static std::atomic<unsigned> takeCalls{0}, giveCalls{0};
static std::atomic<TickType_t> lastWait{0};
static bool rejectLock = false, mutateOnUnlock = false;
static thread_local unsigned lockDepth = 0;
static int xSemaphoreTakeRecursive(void*, TickType_t wait) {
  ++takeCalls; lastWait = wait;
  if (rejectLock) return 0;
  lifecycleMutex.lock(); ++lockDepth;
  return pdTRUE;
}
static void xSemaphoreGiveRecursive(void*) {
  check(lockDepth != 0, "release requires the held lifecycle lock");
  if (mutateOnUnlock) {
    state.connections[0].user = "replaced after lock release";
    state.connections[0].deviceAddr[0] = 0xff;
    state.totalConnections = 999;
  }
  --lockDepth; ++giveCalls; lifecycleMutex.unlock();
}
static uint32_t nowMs = 10000;
// The event stream stamps its JSON with millis() outside any guard.
static bool millisGuardRequired = true;
// Counted in every build so a non-aborting section can list unguarded reads.
static std::atomic<unsigned> unguardedMillis{0};
static uint32_t millis() {
  if (lockDepth == 0) ++unguardedMillis;
#if !TEST_BASELINE
  if (millisGuardRequired)
    check(lockDepth > 0, "connection age must be read under lifecycle guard");
#endif
  return nowMs;
}
static bool argWantsJson(const String& args) { return args == "json"; }
static constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 4096;
static char debugBuffer[GLOBAL_DEBUG_BUFFER_SIZE];
static bool debugAvailable = true;
static bool ensureDebugBuffer() { return debugAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
enum class AllocPref { PreferPSRAM };
static bool psAllocAllowed = true;
static char cliStorage[2048];
static void* ps_alloc(size_t size, AllocPref, const char*) {
  check(size == sizeof(cliStorage), "CLI retains bounded storage");
  return psAllocAllowed ? cliStorage : nullptr;
}
#define BLE_DEBUGF(...) ((void)0)
static void broadcastOutput(const char*) {}
struct esp_ble_gatts_cb_param_t { struct { uint16_t conn_id; } write; };
// The Arduino wrapper's setValue()+notify() is characteristic-wide: notify()
// reaches every connected peer. Counted so a broadcast stream is visible.
static unsigned setValueCalls = 0, broadcastNotifies = 0;
struct BLECharacteristic {
  const char* data = "help";
  uint16_t handle = 0;
  const uint8_t* getData() { return reinterpret_cast<const uint8_t*>(data); }
  size_t getLength() { return std::strlen(data); }
  uint16_t getHandle() { return handle; }
  void setValue(uint8_t*, size_t) { ++setValueCalls; }
  void notify() { ++broadcastNotifies; }
};
static BLECharacteristic sensorChar{"", 0x21}, systemChar{"", 0x24}, eventChar{"", 0x27};
static BLECharacteristic* pSensorDataChar = &sensorChar;
static BLECharacteristic* pSystemStatusChar = &systemChar;
static BLECharacteristic* pEventNotifyChar = &eventChar;
struct BLECharacteristicCallbacks {
  virtual ~BLECharacteristicCallbacks() = default;
  virtual void onWrite(BLECharacteristic*, esp_ble_gatts_cb_param_t*) {}
};
static unsigned processed = 0, advertisingStarts = 0, advertisingStops = 0;
static void processIncomingBLECommand(uint16_t connId, const char* data, size_t len) {
  check(lockDepth == 0, "command dispatch happens after counter-update lock release");
  check(connId == 17 && len == 4 && std::string(data, len) == "help", "callback forwards command bytes and identity");
  ++processed;
}
struct BLEDevice {
  static void startAdvertising() { ++advertisingStarts; }
  static void stopAdvertising() { ++advertisingStops; }
};
static void* pAdvertising = &state;
// GATT server double for bledisconnect: BLEServer::disconnect(connId) hands the
// id to esp_ble_gatts_close. Records each id and whether the lifecycle guard was
// held when the close was issued.
static std::vector<uint16_t> closedConns;
static unsigned closesUnderLock = 0;
struct BLEServer {
  void disconnect(uint16_t connId) {
    closedConns.push_back(connId);
    if (lockDepth) ++closesUnderLock;
  }
};
static BLEServer bleServer;
static BLEServer* pServer = &bleServer;
using esp_err_t = int;
static constexpr int ESP_OK = 0, SOURCE_INTERNAL = 1, HTTPD_RESP_USE_STRLEN = -1;
#define EXT_RAM_BSS_ATTR

// Stream CLI and data-service boundaries. sendBLEResponse is the console-mirror
// sender `blesend` uses; the mirror-recipient collector, the epoch fence and the
// GATT sender are the boundaries of the directed stream delivery.
static void cliHint(const char*) {}
static std::vector<std::string> mirrorSent;
static bool mirrorAccept = true;
static bool sendBLEResponse(const char* data, size_t len, bool blocking = true) {
  check(blocking, "blesend keeps the reliable (blocking) send");
  if (!mirrorAccept) return false;
  mirrorSent.emplace_back(data, len);
  return true;
}
static std::vector<BleSessionFence> mirrorRecipients;
static int bleCollectMirrorSessions(BleSessionFence* out, int cap) {
  check(lockDepth == 0, "recipients are collected before any per-recipient guard");
  int n = 0;
  for (const BleSessionFence& f : mirrorRecipients) {
    if (n == cap) break;
    out[n++] = f;
  }
  return n;
}
static std::unordered_map<uint16_t, TransportSessionEpoch> liveEpochs;
static bool bleConnectionEpochMatchesLocked(uint16_t connId, TransportSessionEpoch epoch) {
  check(lockDepth > 0, "epoch check runs under the lifecycle guard");
  auto it = liveEpochs.find(connId);
  return epoch != 0 && it != liveEpochs.end() && it->second == epoch;
}
using esp_gatt_if_t = int;
static constexpr int ESP_GATT_IF_NONE = 0xff, ESP_FAIL = -1;
static std::atomic<int> sBleGattsIf{4};
struct DirectedFrame { uint16_t connId, handle; std::string payload; };
static std::vector<DirectedFrame> directedFrames;
static std::vector<uint16_t> congested;
static esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t gattsIf, uint16_t connId,
                                             uint16_t handle, uint16_t len,
                                             uint8_t* value, bool needConfirm) {
  check(lockDepth > 0 && !needConfirm && gattsIf == sBleGattsIf.load(),
        "directed stream notify is admitted under the guard, no confirm");
  for (uint16_t busy : congested)
    if (busy == connId) return ESP_FAIL;
  directedFrames.push_back({connId, handle, std::string(reinterpret_cast<char*>(value), len)});
  return ESP_OK;
}
struct AuthContext { int transport = 0; String user, ip, path; };
struct httpd_req_t {
  bool authorized = true;
  std::string type, status = "200 OK", output;
  int sends = 0;
};
#define WEB_AUTH_OR_RETURN(req, ctx) \
  AuthContext ctx; if (!(req)->authorized) { (req)->status = "401 Unauthorized"; return ESP_OK; }
static void httpd_resp_set_type(httpd_req_t* req, const char* value) { req->type = value; }
static void httpd_resp_set_status(httpd_req_t* req, const char* value) { req->status = value; }
static int httpd_resp_send(httpd_req_t* req, const char* value, int length) {
  req->output.assign(value, length < 0 ? std::strlen(value) : static_cast<size_t>(length));
  ++req->sends; return ESP_OK;
}
static int httpd_resp_sendstr(httpd_req_t* req, const char* value) {
  return httpd_resp_send(req, value, HTTPD_RESP_USE_STRLEN);
}
static unsigned ringOwnerCalls = 0;
static unsigned g2ConnectionCalls = 0;
static bool peerAutoReconnect[BLE_PEER_MAX]{};
static unsigned peerPolicyCalls = 0;
static bool blePeerAutoReconnectEnabled(BlePeerKind kind) {
  check(kind == BLE_PEER_G2_GLASSES || kind == BLE_PEER_R1_RING,
        "HTTP queries only the rendered peers' reconnect policy");
  ++peerPolicyCalls;
  return peerAutoReconnect[kind];
}
static bool ringAvailable = true, g2SnapshotValid = true, g2ConnectionValid = true;
static G2ConnectionStatus g2Connection;
static String ringName = "Ring";
static bool g2RingBuildStatusJson(JsonObject out) {
  ++ringOwnerCalls;
  if (!ringAvailable || out.isNull()) return false;
  out["connected"] = true;
  out["name"] = ringName;
  return true;
}
static unsigned g2FormatCalls = 0;
static void formatG2Status(const G2ConnectionStatus& captured, char* out, size_t size) {
  ++g2FormatCalls;
  check(captured.state == g2Connection.state && captured.running == g2Connection.running &&
        captured.left.tx == g2Connection.left.tx && captured.right.rx == g2Connection.right.rx,
        "HTTP legacy display formats the same captured G2 snapshot as structured output");
  std::snprintf(out, size, "G2 is connected");
}
static bool g2ConnectionStatusSnapshot(G2ConnectionStatus* out) {
  ++g2ConnectionCalls;
  if (!out || !g2ConnectionValid) return false;
  *out = g2Connection;
  return true;
}
static bool g2ControlStatusSnapshot(G2ControlStatus* out) {
  *out = {};
  out->revision = 7; out->rightGeneration = 11;
  std::strcpy(out->firmwareLeft, "left-firmware");
  std::strcpy(out->firmwareRight, "right-firmware");
  out->rxQueued = 3; out->rxDrops = 2;
  out->headUp.desired = G2_CONTROL_ON;
  out->headUp.observed = G2_CONTROL_OBS_ON;
  out->headUp.phase = G2_CONTROL_VERIFIED;
  std::strcpy(out->headUp.detail, "confirmed \"up\"");
  out->headUp.changedAtMs = 123; out->headUp.appliedGeneration = 11;
  out->notifications.desired = G2_CONTROL_OFF;
  out->notifications.observed = G2_CONTROL_UNKNOWN;
  out->notifications.phase = G2_CONTROL_WAITING_LINK;
  return g2SnapshotValid;
}

// INSERT_PRODUCTION

static JsonDocument parsed(const char* json) {
  JsonDocument doc;
  check(!deserializeJson(doc, json), "response must be complete valid JSON");
  return doc;
}
static std::string jsonString(const JsonDocument& doc) {
  std::string out; serializeJson(doc, out); return out;
}
static void populate() {
  state = {}; gBLEState = &state; sBleLifecycleMutex = &lifecycleMutex;
  state.initialized = true; state.connectionState = BLE_STATE_CONNECTED;
  state.activeConnectionCount = 2; state.totalConnections = 19; state.commandsReceived = 31;
  for (int slot : {0, 2}) {
    BLEConnection& c = state.connections[slot];
    c.active = true; c.connId = static_cast<uint16_t>(slot + 7);
    c.connectedSince = static_cast<uint32_t>(1000 + slot * 1000);
    c.commandsReceived = static_cast<uint32_t>(slot + 10);
    for (int byte = 0; byte < 6; ++byte) c.deviceAddr[byte] = static_cast<uint8_t>(slot * 10 + byte);
  }
  state.connections[0].authed = true; state.connections[0].user = "Unknown";
  state.connections[2].authed = false; state.connections[2].user = "";
}
static void checkSchema(JsonVariantConst doc, bool initialized) {
  check(doc.as<JsonObjectConst>().size() == 13, "BLE schema contains exactly the characterized fields");
  check(doc["schema"] == 1 && doc["deviceName"] == gSettings.bleDeviceName,
        "schema version and escaped device name preserved");
  check(doc["txPower"] == 5 && doc["autoStart"] == true && doc["requireAuth"] == true &&
        doc["secureChannelRequired"] == secureRequired, "configuration facts preserved");
  check(doc["initialized"] == initialized && doc["maxConnections"] == BLE_MAX_CONNECTIONS,
        "initialization and compiled capacity preserved");
  if (!initialized) {
    check(doc["state"] == "uninitialized" && doc["connections"] == 0 &&
          doc["totalConnections"] == 0 && doc["commandsReceived"] == 0 &&
          doc["clients"].as<JsonArrayConst>().size() == 0, "uninitialized live fields empty");
  }
}
static void characterize() {
  psAllocAllowed = false;
  check(std::strcmp(cmd_bleinfo("json"), "{\"error\":\"oom\"}") == 0,
        "CLI initial allocation failure explicit");
  psAllocAllowed = true;
  gBLEState = nullptr;
  JsonDocument empty = parsed(cmd_bleinfo("json")); checkSchema(empty.as<JsonVariantConst>(), false);
  std::cout << jsonString(empty) << '\n';
  std::cout << cmd_bleinfo("") << '\n';
  populate();
  for (BLEConnectionState value : {BLE_STATE_IDLE, BLE_STATE_ADVERTISING, BLE_STATE_SCANNING,
                                   BLE_STATE_CONNECTING, BLE_STATE_CONNECTED, BLE_STATE_DISCONNECTING}) {
    state.connectionState = value;
    JsonDocument doc = parsed(cmd_bleinfo("json")); checkSchema(doc.as<JsonVariantConst>(), true);
    check(doc["clients"].size() == 2 && doc["clients"][0]["connId"] == 7 &&
          doc["clients"][1]["connId"] == 9, "active sparse slots retain order and IDs");
    check(doc["clients"][0]["authed"] == true && doc["clients"][0]["user"] == "Unknown" &&
          doc["clients"][1]["authed"] == false && doc["clients"][1]["user"] == "",
          "authenticated user named Unknown remains distinct from anonymous peer");
    check(doc["clients"][0]["mac"] == "00:01:02:03:04:05" &&
          doc["clients"][1]["mac"] == "14:15:16:17:18:19", "MAC stack buffers copied");
    check(doc["clients"][0]["since"] == 9 && doc["clients"][1]["since"] == 7,
          "connection age and ordering preserved");
    std::cout << jsonString(doc) << '\n';
  }
  std::cout << cmd_bleinfo("") << '\n';
  populate(); state.connections[0].user = "user \"quote\"\\slash\n";
  nowMs = 30; state.connections[0].connectedSince = 0xfffffc18u;
  JsonDocument wrap = parsed(cmd_bleinfo("json"));
  check(wrap["clients"][0]["since"] == 1 &&
        wrap["clients"][0]["user"] == state.connections[0].user, "age wraparound and username escaping");
  std::cout << jsonString(wrap) << '\n';
  nowMs = 10000; populate();
}

#if !TEST_BASELINE
static void writerChecks() {
  populate();
  CmdRequestCallbacks callback;
  BLECharacteristicCallbacks& interface = callback;
  BLECharacteristic characteristic;
  esp_ble_gatts_cb_param_t param{{17}};
  const unsigned priorCount = state.commandsReceived, priorTakes = takeCalls;
  interface.onWrite(&characteristic, nullptr);
  characteristic.data = ""; interface.onWrite(&characteristic, &param);
  check(state.commandsReceived == priorCount && processed == 0 && takeCalls == priorTakes,
        "empty or incomplete BLE write avoids counter mutation and dispatch");
  characteristic.data = "help"; interface.onWrite(&characteristic, &param);
  check(state.commandsReceived == priorCount + 1 && processed == 1 && takeCalls == priorTakes + 1 &&
        state.deferredCmdReceivedPending && state.deferredCmdReceivedLen == 4 &&
        state.deferredCmdReceivedConnId == 17 && lockDepth == 0,
        "actual BLE callback updates status/deferred metadata in one guarded scope");
  rejectLock = true; gBLEState = reinterpret_cast<BLESystemState*>(1);
  interface.onWrite(&characteristic, &param);
  check(processed == 1, "failed callback admission cannot access state or dispatch");
  rejectLock = false; populate();
  state.connectionState = BLE_STATE_IDLE;
  unsigned takes = takeCalls;
  check(startBLEAdvertising() && state.connectionState == BLE_STATE_ADVERTISING &&
        advertisingStarts == 1 && takeCalls == takes + 1 && lockDepth == 0,
        "advertising start publishes state under lifecycle guard");
  takes = takeCalls;
  stopBLEAdvertising();
  check(state.connectionState == BLE_STATE_IDLE && advertisingStops == 1 &&
        takeCalls == takes + 1 && lockDepth == 0,
        "advertising stop publishes state under lifecycle guard");
  state.connectionState = BLE_STATE_CONNECTED;
  check(!startBLEAdvertising() && advertisingStarts == 1, "already-connected advertising behavior preserved");
  state.initialized = false;
  check(!startBLEAdvertising(), "uninitialized advertising remains refused");
  stopBLEAdvertising();
  populate();
}

// blestatus / bleread render the guarded snapshot bleinfo json uses: the same
// client set and identity facts, read under the lifecycle guard. Non-aborting:
// every failed expectation is listed, then the run fails.
static unsigned statusFailures = 0;
static void statusExpect(bool ok, const std::string& what) {
  ++assertions;
  if (!ok) {
    ++statusFailures;
    std::cerr << "FAIL " << what << '\n';
  }
}
static bool statusSame(const char* got, const char* want) {
  return got && std::strcmp(got, want) == 0;
}
static std::string statusText(JsonVariantConst value) {
  std::string out; serializeJson(value, out); return out;
}
// populate() plus a logged-in "alice" in slot 1: slot 0 is a logged-in user
// named "Unknown", slot 2 an anonymous peer.
static void statusTable() {
  populate();
  BLEConnection& c = state.connections[1];
  c.active = true; c.connId = 8; c.connectedSince = 4000; c.commandsReceived = 4;
  for (int byte = 0; byte < 6; ++byte) c.deviceAddr[byte] = static_cast<uint8_t>(0x31 + byte);
  c.authed = true; c.user = "alice";
  state.activeConnectionCount = 3; state.responsesSent = 23;
}
// Both JSON views list the same connections with the same facts.
static void statusSameClients(const char* label) {
  JsonDocument info, status;
  const std::string infoText = cmd_bleinfo("json");
  const std::string statusJson = cmd_blestatus("json");
  const bool parsedInfo = !deserializeJson(info, infoText);
  const bool parsedStatus = !deserializeJson(status, statusJson);
  statusExpect(parsedInfo && parsedStatus,
               std::string(label) + ": both commands return complete JSON; blestatus json got " +
               statusJson.substr(0, 160) + (statusJson.size() > 160 ? "..." : "") +
               " (" + std::to_string(statusJson.size()) + " bytes)");
  if (!parsedInfo || !parsedStatus) return;
  JsonArrayConst clients = info["clients"], conns = status["connections"];
  statusExpect(conns.size() == clients.size() && clients.size() == state.activeConnectionCount,
               std::string(label) + ": blestatus json lists " + std::to_string(conns.size()) +
               " connections, bleinfo json " + std::to_string(clients.size()));
  statusExpect(status["activeConnections"] == info["connections"].as<int>() &&
               status["totalConnections"] == info["totalConnections"].as<unsigned>() &&
               status["commandsReceived"] == info["commandsReceived"].as<unsigned>() &&
               status["state"] == info["state"].as<const char*>() &&
               status["responsesSent"] == state.responsesSent,
               std::string(label) + ": counters and state differ from bleinfo json");
  int slot = -1;
  for (size_t i = 0; i < clients.size() && i < conns.size(); ++i) {
    JsonObjectConst a = clients[i], b = conns[i];
    while (++slot < BLE_MAX_CONNECTIONS && !state.connections[slot].active) {}
    const std::string where = std::string(label) + " slot " + std::to_string(slot) + ": ";
    statusExpect(b["connId"].is<unsigned>() && b["connId"] == a["connId"].as<unsigned>(),
                 where + "blestatus json lacks bleinfo's connId " + statusText(a["connId"]) +
                 "; got " + statusText(b));
    statusExpect(b["authed"].is<bool>() && b["authed"] == a["authed"].as<bool>(),
                 where + "blestatus json lacks bleinfo's authed " + statusText(a["authed"]) +
                 "; got " + statusText(b));
    statusExpect(b["mac"] == a["mac"].as<const char*>() && b["connectedSec"] == a["since"].as<unsigned>() &&
                 b["commands"] == a["commands"].as<unsigned>() && b["index"] == slot,
                 where + "mac/age/commands/index differ: bleinfo " + statusText(a) + ", blestatus " + statusText(b));
    const std::string user = a["user"].as<const char*>();
    const std::string shownUser = a["authed"].as<bool>() && !user.empty() ? user : "Unknown";
    statusExpect(b["user"] == shownUser.c_str(),
                 where + "display user should be \"" + shownUser + "\"; got " + statusText(b["user"]));
  }
}

static void blestatusChecks() {
  millisGuardRequired = false;
  // S1: a logged-in user named "Unknown", a logged-in "alice" and an anonymous
  // peer give the same client set from both commands, told apart by `authed`.
  statusTable();
  statusSameClients("S1 mixed table");
  {
    JsonDocument status;
    const std::string json = cmd_blestatus("json");
    if (!deserializeJson(status, json)) {
      statusExpect(status["connections"][0]["authed"] == true && status["connections"][2]["authed"] == false,
                   "S1 blestatus json cannot tell the logged-in user named \"Unknown\" from the anonymous "
                   "peer; got " + json);
    }
  }

  // S2: each form takes and releases the lifecycle guard once, with the bounded
  // 100 ms budget, and reads connection age inside it.
  for (const char* args : {"json", ""}) {
    statusTable();
    const unsigned takes = takeCalls, gives = giveCalls;
    lastWait = 0; unguardedMillis = 0;
    cmd_blestatus(args);
    const std::string form = std::string("S2 `blestatus") + (*args ? " json" : "") + "`: ";
    statusExpect(takeCalls - takes == 1 && giveCalls - gives == 1 && lockDepth == 0,
                 form + "reads gBLEState without the lifecycle guard (takes " +
                 std::to_string(takeCalls - takes) + ", releases " + std::to_string(giveCalls - gives) + ")");
    statusExpect(lastWait == 100, form + "guard wait should be 100 ms; got " + std::to_string(lastWait));
    statusExpect(unguardedMillis == 0,
                 form + "connection age read outside the lifecycle guard (" +
                 std::to_string(unguardedMillis.load()) + " reads)");
  }

  // S3: a busy lifecycle guard gives an explicit unavailable reply without
  // reading the connection table.
  statusTable(); rejectLock = true; unguardedMillis = 0;
  {
    const std::string json = cmd_blestatus("json");
    const std::string text = cmd_blestatus("");
    statusExpect(json == "{\"schema\":1,\"error\":\"unavailable\"}",
                 "S3 lifecycle busy: `blestatus json` should report unavailable; got " + json.substr(0, 120));
    statusExpect(text == "Error: BLE status unavailable (Bluetooth busy); try again",
                 "S3 lifecycle busy: `blestatus` should report unavailable; got " + text.substr(0, 120));
    statusExpect(unguardedMillis == 0 && lockDepth == 0,
                 "S3 lifecycle busy: the connection table was read anyway");
  }
  rejectLock = false;

  // S4: both forms render the copy taken under the guard; a change published
  // as the guard is released does not appear.
  statusTable(); mutateOnUnlock = true;
  const std::string snapJson = cmd_blestatus("json");
  mutateOnUnlock = false;
  {
    JsonDocument doc;
    statusExpect(!deserializeJson(doc, snapJson) && doc["totalConnections"] == 19 &&
                 doc["connections"][0]["mac"] == "00:01:02:03:04:05",
                 "S4 `blestatus json` mixed state from after the guard release; got " + snapJson);
  }
  statusTable(); mutateOnUnlock = true;
  const std::string snapText = cmd_blestatus("");
  mutateOnUnlock = false;
  statusExpect(snapText.find("Total connections: 19\n") != std::string::npos &&
               snapText.find("00:01:02:03:04:05") != std::string::npos,
               "S4 `blestatus` mixed state from after the guard release; got " + snapText);

  // S5: the text form keeps its lines and format.
  statusTable();
  const std::string text = cmd_blestatus("");
  const char* expectedText =
      "BLE Status: connected\n"
      "Active connections: 3/4\n"
      "[0] Unknown\n    MAC: 00:01:02:03:04:05 | 9 sec | 10 cmds\n"
      "[1] alice\n    MAC: 31:32:33:34:35:36 | 6 sec | 4 cmds\n"
      "[2] Unknown\n    MAC: 14:15:16:17:18:19 | 7 sec | 12 cmds\n"
      "Total connections: 19\n"
      "Commands received: 31\n"
      "Responses sent: 23";
  statusExpect(text == expectedText, "S5 `blestatus` text changed; got:\n" + text);

  // S6: not initialized keeps the existing replies; with no lifecycle mutex
  // blestatus agrees with bleinfo json and reads nothing.
  const char* notInitJson = "{\"schema\":1,\"initialized\":false}";
  const char* notInitText = "Error: Bluetooth not initialized. Run 'openble' first.";
  statusTable(); state.initialized = false;
  statusExpect(statusSame(cmd_blestatus("json"), notInitJson) && statusSame(cmd_blestatus(""), notInitText),
               "S6 server not initialized: existing replies changed");
  statusTable(); gBLEState = nullptr;
  statusExpect(statusSame(cmd_blestatus("json"), notInitJson) && statusSame(cmd_blestatus(""), notInitText),
               "S6 no BLE state: existing replies changed");
  statusTable(); sBleLifecycleMutex = nullptr; unguardedMillis = 0;
  {
    const std::string json = cmd_blestatus("json");
    JsonDocument info;
    deserializeJson(info, cmd_bleinfo("json"));
    statusExpect(info["initialized"] == false && json == notInitJson && unguardedMillis == 0,
                 "S6 no lifecycle mutex: bleinfo json reports uninitialized but blestatus json read the "
                 "unguarded table; got " + json.substr(0, 120));
  }
  sBleLifecycleMutex = &lifecycleMutex;

  // S7: four logged-in connections with 64-character usernames of quotes give
  // complete JSON (and the same client set) instead of a torn document.
  statusTable();
  for (int slot = 0; slot < BLE_MAX_CONNECTIONS; ++slot) {
    BLEConnection& c = state.connections[slot];
    c.active = true; c.authed = true; c.user = std::string(64, '"');
    c.connId = static_cast<uint16_t>(65530 + slot); c.commandsReceived = UINT32_MAX;
    c.connectedSince = nowMs - 4000000u;
  }
  state.activeConnectionCount = BLE_MAX_CONNECTIONS;
  state.totalConnections = state.commandsReceived = state.responsesSent = UINT32_MAX;
  statusSameClients("S7 long usernames");

  // S8: an allocation failure gives an explicit reply, never partial JSON.
  statusTable();
  jsonAllocator.reset();
  cmd_blestatus("json");
  const int allocations = jsonAllocator.calls;
  int partial = 0, leaked = 0;
  std::string sample;
  for (int failure = 0; failure < allocations; ++failure) {
    jsonAllocator.reset(failure);
    const std::string reply = cmd_blestatus("json");
    if (reply != "{\"schema\":1,\"error\":\"oom\"}") { ++partial; if (sample.empty()) sample = reply; }
    if (jsonAllocator.outstanding != 0 || lockDepth != 0) ++leaked;
  }
  jsonAllocator.reset();
  statusExpect(allocations > 0 && partial == 0,
               "S8 allocation failure: " + std::to_string(partial) + " of " + std::to_string(allocations) +
               " failure sites returned a document instead of the oom reply, e.g. " + sample.substr(0, 120));
  statusExpect(leaked == 0, "S8 allocation failure leaked JSON memory or the lifecycle guard");

  millisGuardRequired = true; rejectLock = false; mutateOnUnlock = false;
  sBleLifecycleMutex = &lifecycleMutex;
  populate();
  check(statusFailures == 0, "blestatus/bleread checks failed (listed above)");
}

static void ownerChecks() {
  unsigned before = takeCalls;
  check(!buildBLEInfoJson(JsonObject()) && takeCalls == before,
        "null caller object rejected without lifecycle access");
  sBleLifecycleMutex = nullptr;
  gBLEState = reinterpret_cast<BLESystemState*>(1);
  {
    JsonDocument doc;
    check(buildBLEInfoJson(doc.to<JsonObject>()), "absent lifecycle has a normal unavailable snapshot");
    checkSchema(doc.as<JsonVariantConst>(), false);
    check(takeCalls == before, "absent mutex must not inspect runtime state");
  }
  sBleLifecycleMutex = &lifecycleMutex; rejectLock = true;
  {
    JsonDocument doc;
    const unsigned releases = giveCalls;
    check(!buildBLEInfoJson(doc.to<JsonObject>()), "busy lifecycle refuses snapshot");
    check(lastWait == 100, "status lock admission uses bounded 100ms budget");
    check(lockDepth == 0 && giveCalls == releases, "failed lock is never released");
    check(std::strcmp(cmd_bleinfo("json"), "{\"error\":\"unavailable\"}") == 0,
          "CLI reports lifecycle contention rather than uninitialized");
  }
  rejectLock = false; populate();
  {
    state.initialized = false;
    JsonDocument doc;
    check(buildBLEInfoJson(doc.to<JsonObject>()), "published state awaiting initialization remains readable");
    checkSchema(doc.as<JsonVariantConst>(), false);
  }
  populate();
  {
    JsonDocument doc;
    mutateOnUnlock = true;
    check(buildBLEInfoJson(doc.to<JsonObject>()), "live snapshot succeeds");
    mutateOnUnlock = false;
    check(state.totalConnections == 999 && doc["totalConnections"] == 19 &&
          doc["clients"][0]["user"] == "Unknown" &&
          doc["clients"][0]["mac"] == "00:01:02:03:04:05",
          "caller retains owned strings/data after lifecycle release");
    JsonDocument second;
    check(buildBLEInfoJson(second.to<JsonObject>()), "second independent snapshot succeeds");
    second["clients"][0]["user"] = "consumer edit";
    check(doc["clients"][0]["user"] == "Unknown" &&
          state.connections[0].user == "replaced after lock release", "documents do not share mutable output");
  }
  populate();
  std::string oldName = gSettings.bleDeviceName.c_str();
  gSettings.bleDeviceName = std::string(2200, 'N');
  {
    JsonDocument doc;
    check(buildBLEInfoJson(doc.to<JsonObject>()) && measureJson(doc) > 2048,
          "owner permits caller-sized output beyond CLI buffer");
    check(std::strcmp(cmd_bleinfo("json"), "{\"error\":\"status too large\"}") == 0,
          "CLI rejects oversized output without truncation");
  }
  gSettings.bleDeviceName = oldName;
  // Characterize every observed JSON allocation failure site in the actual CLI.
  jsonAllocator.reset();
  check(parsed(cmd_bleinfo("json"))["schema"] == 1, "CLI baseline before fault sweep");
  const int allocationCount = jsonAllocator.calls;
  for (int failure = 0; failure < allocationCount; ++failure) {
    jsonAllocator.reset(failure);
    check(std::strcmp(cmd_bleinfo("json"), "{\"error\":\"oom\"}") == 0,
          "CLI document allocation failure cannot publish partial status");
    check(jsonAllocator.outstanding == 0 && lockDepth == 0,
          "failed CLI frees JSON and releases lifecycle lock");
  }
  jsonAllocator.reset();
  // Real host threads use the same guard-backed owner with independent docs;
  // the writer changes correlated fields under the lifecycle lock.
  state.totalConnections = state.commandsReceived = 0;
  std::thread writer([] {
    for (unsigned i = 1; i <= 500; ++i) {
      std::lock_guard<std::recursive_timed_mutex> guard(lifecycleMutex);
      state.totalConnections = i;
      std::this_thread::yield();
      state.commandsReceived = i;
    }
  });
  auto reader = [] {
    for (unsigned i = 0; i < 200; ++i) {
      JsonDocument doc;
      check(buildBLEInfoJson(doc.to<JsonObject>()), "concurrent reader completes");
      check(doc["totalConnections"].as<unsigned>() == doc["commandsReceived"].as<unsigned>(),
            "guard keeps correlated lifecycle fields coherent");
      check(doc["clients"][0]["user"] == "Unknown", "concurrent output retains identity");
    }
  };
  std::thread readerA(reader), readerB(reader);
  writer.join(); readerA.join(); readerB.join();
  populate();
}

static void webChecks() {
  httpd_req_t denied; denied.authorized = false;
  unsigned before = secureCalls;
  jsonAllocator.reset(); ringOwnerCalls = 0; g2ConnectionCalls = 0; g2FormatCalls = 0;
  peerPolicyCalls = 0;
  check(handleBleStatus(&denied) == ESP_OK && denied.status == "401 Unauthorized" &&
        secureCalls == before && jsonAllocator.calls == 0 && ringOwnerCalls == 0 &&
        g2ConnectionCalls == 0 && g2FormatCalls == 0 && peerPolicyCalls == 0,
        "HTTP authentication gates owners and allocation");
  rejectLock = true;
  httpd_req_t busy;
  handleBleStatus(&busy);
  check(busy.status == "503 Service Unavailable" &&
        parsed(busy.output.c_str())["error"] == "BLE status unavailable" && ringOwnerCalls == 0 &&
        g2ConnectionCalls == 0 && peerPolicyCalls == 0,
        "HTTP lock contention gives explicit 503 before other status work");
  rejectLock = false;
  g2Connection.state = "connecting"; g2Connection.running = true;
  g2Connection.left = {true, 517, -1, UINT32_MAX, 123456789};
  g2Connection.right = {false, 23, 0, 0, UINT32_MAX};
  for (int mode : {0, 1}) {
    gSettings.bleMode = mode;
    httpd_req_t req; ringOwnerCalls = 0; g2ConnectionCalls = 0; g2FormatCalls = 0;
    check(handleBleStatus(&req) == ESP_OK && req.sends == 1 && req.status == "200 OK" &&
          req.type == "application/json", "HTTP success envelope preserved");
    JsonDocument doc = parsed(req.output.c_str());
    check(doc["schema"] == 1 && doc["mode"] == (mode ? "client" : "server"),
          "mode and schema preserved");
    checkSchema(doc["ble"], true);
#if ENABLE_G2_GLASSES
    check(doc.as<JsonObject>().size() == 6 && ringOwnerCalls == 1 &&
          g2ConnectionCalls == 1 && g2FormatCalls == 1,
          "G2 build obtains ring and connection status directly from owners");
    check(doc["g2Text"] == "G2 is connected" && doc["ring"]["connected"] == true &&
          doc["g2"]["revision"] == 7 && doc["g2"]["rightGeneration"] == 11 &&
          doc["g2"]["firmware"]["left"] == "left-firmware" &&
          doc["g2"]["firmware"]["right"] == "right-firmware" &&
          doc["g2"]["rx"]["queued"] == 3 && doc["g2"]["rx"]["drops"] == 2,
          "G2 and ring response data preserved");
    JsonVariantConst connection = doc["g2"]["connection"];
    check(connection.as<JsonObjectConst>().size() == 4 &&
          connection["running"] == true && connection["state"] == "connecting" &&
          connection["left"].as<JsonObjectConst>().size() == 5 &&
          connection["left"]["connected"] == true && connection["left"]["mtu"] == 517 &&
          connection["left"]["battery"] == -1 && connection["left"]["tx"] == UINT32_MAX &&
          connection["left"]["rx"] == 123456789 &&
          connection["right"].as<JsonObjectConst>().size() == 5 &&
          connection["right"]["connected"] == false && connection["right"]["mtu"] == 23 &&
          connection["right"]["battery"] == 0 && connection["right"]["tx"] == 0 &&
          connection["right"]["rx"] == UINT32_MAX,
          "HTTP emits typed complete connection data independently of human status text");
    JsonVariantConst head = doc["g2"]["controls"]["headUp"];
    check(head["desired"] == "on" && head["observed"] == "on" && head["phase"] == "verified" &&
          head["detail"] == "confirmed \"up\"" && head["changedAtMs"] == 123 &&
          head["appliedGeneration"] == 11, "G2 control state and detail preserved");
#else
    check(doc.as<JsonObject>().size() == 3 && ringOwnerCalls == 0 &&
          g2ConnectionCalls == 0 && g2FormatCalls == 0,
          "non-G2 status has no command executor or G2 fields");
#endif
  }
  // The HTTP handler must preserve independent owner values, including false,
  // without consulting human command text or reusing the other peer's policy.
  for (bool g2Auto : {false, true}) {
    for (bool ringAuto : {false, true}) {
      peerAutoReconnect[BLE_PEER_G2_GLASSES] = g2Auto;
      peerAutoReconnect[BLE_PEER_R1_RING] = ringAuto;
      peerPolicyCalls = 0;
      httpd_req_t req; handleBleStatus(&req);
      JsonDocument doc = parsed(req.output.c_str());
#if ENABLE_G2_GLASSES
      check(req.status == "200 OK" && peerPolicyCalls == 2 &&
            doc["g2"]["autoReconnect"].is<bool>() &&
            doc["g2"]["autoReconnect"].as<bool>() == g2Auto &&
            doc["ring"]["autoReconnect"].is<bool>() &&
            doc["ring"]["autoReconnect"].as<bool>() == ringAuto,
            "HTTP exposes both peers' exact boolean reconnect policy");
#else
      check(req.status == "200 OK" && peerPolicyCalls == 0 &&
            doc["g2"].isNull() && doc["ring"].isNull(),
            "G2-disabled status neither reads nor exposes peer reconnect policy");
#endif
    }
  }
#if ENABLE_G2_GLASSES
  g2ConnectionValid = false;
  jsonAllocator.reset();
  httpd_req_t missingConnection; ringOwnerCalls = 0; g2ConnectionCalls = 0;
  handleBleStatus(&missingConnection);
  check(missingConnection.status == "503 Service Unavailable" && g2ConnectionCalls == 1 &&
        ringOwnerCalls == 0 && parsed(missingConnection.output.c_str())["error"] == "G2 status unavailable",
        "missing G2 connection snapshot returns explicit 503 before ring work");
  const int unavailableAllocations = jsonAllocator.calls;
  for (int failure = 0; failure < unavailableAllocations; ++failure) {
    jsonAllocator.reset(failure);
    httpd_req_t missingAndOom; handleBleStatus(&missingAndOom);
    check(missingAndOom.status == "500 Internal Server Error" &&
          parsed(missingAndOom.output.c_str())["error"] == "Out of memory",
          "every preceding allocation failure takes priority over unavailable G2 status");
    check(jsonAllocator.outstanding == 0, "unavailable G2 response releases failed allocations");
  }
  jsonAllocator.reset(); g2ConnectionValid = true;
  g2Connection = {};
  httpd_req_t off; handleBleStatus(&off);
  JsonDocument offDoc = parsed(off.output.c_str());
  check(offDoc["g2"]["connection"]["running"] == false &&
        offDoc["g2"]["connection"]["state"] == "off" &&
        offDoc["g2"]["connection"]["left"]["connected"] == false &&
        offDoc["g2"]["connection"]["left"]["battery"] == -1,
        "available off owner is successful complete status, not HTTP unavailability");
  ringAvailable = false;
  httpd_req_t unavailable; ringOwnerCalls = 0; handleBleStatus(&unavailable);
  check(unavailable.status == "503 Service Unavailable" && ringOwnerCalls == 1 &&
        parsed(unavailable.output.c_str())["error"] == "Ring status unavailable",
        "ring owner failure gives explicit HTTP 503");
  ringAvailable = true; g2SnapshotValid = false;
  httpd_req_t missing; handleBleStatus(&missing);
  JsonDocument missingDoc = parsed(missing.output.c_str());
  check(missingDoc["g2"].as<JsonObjectConst>().size() == 2 &&
        missingDoc["g2"]["autoReconnect"] == true &&
        missingDoc["g2"]["connection"]["state"] == "off" &&
        missingDoc["g2Text"] == "G2 is connected" &&
        missingDoc["ring"]["connected"] == true,
        "missing G2 control snapshot retains independent connection and ring status");
  g2SnapshotValid = true; ringName = std::string(800, 'R');
  httpd_req_t longRing; handleBleStatus(&longRing);
  check(longRing.status == "200 OK" && parsed(longRing.output.c_str())["ring"]["name"] == ringName,
        "web owner output is independent of bounded ring CLI buffer");
  ringName = "Ring";
#endif
  jsonAllocator.reset();
  httpd_req_t good; handleBleStatus(&good);
  const int allocationCount = jsonAllocator.calls;
  for (int failure = 0; failure < allocationCount; ++failure) {
    jsonAllocator.reset(failure);
    httpd_req_t req; handleBleStatus(&req);
    JsonDocument response = parsed(req.output.c_str());
    check(req.status == "500 Internal Server Error" && response["error"] == "Out of memory",
          "HTTP response allocation failure is explicit 500");
    check(jsonAllocator.outstanding == 0 && lockDepth == 0,
          "HTTP failure releases JSON and lifecycle ownership");
  }
  jsonAllocator.reset(); stringAppendAllowed = false;
  httpd_req_t shortWrite; handleBleStatus(&shortWrite);
  stringAppendAllowed = true;
  check(shortWrite.status == "500 Internal Server Error" &&
        parsed(shortWrite.output.c_str())["error"] == "Out of memory",
        "HTTP detects short final String serialization");
  httpd_req_t retry; handleBleStatus(&retry);
  check(retry.status == "200 OK" && parsed(retry.output.c_str())["ble"]["schema"] == 1,
        "normal request succeeds after allocation and serialization faults");
}

// blesend/blestream/bleevent receive the dispatcher's arguments (the command
// name already removed), and the data-service streams reach only the
// console-mirror recipients through one directed notify each. Non-aborting:
// every failed expectation is listed, then the run fails.
static unsigned streamFailures = 0;
static void expect(bool ok, const std::string& what) {
  ++assertions;
  if (!ok) {
    ++streamFailures;
    std::cerr << "FAIL " << what << '\n';
  }
}
static std::string shown(const char* text) { return text ? "\"" + std::string(text) + "\"" : "null"; }
static std::string flagsHex() {
  char out[8];
  std::snprintf(out, sizeof(out), "0x%02X", static_cast<unsigned>(state.streamFlags));
  return out;
}
static bool same(const char* got, const char* want) { return got && std::strcmp(got, want) == 0; }
static void streamReset() {
  populate();
  state.streamFlags = 0;
  state.sensorStreamInterval = 1000;
  state.systemStreamInterval = 5000;
  mirrorSent.clear(); mirrorAccept = true;
  mirrorRecipients = {{7, 3}, {9, 5}};
  liveEpochs = {{7, 3}, {9, 5}};
  directedFrames.clear(); congested.clear();
  setValueCalls = broadcastNotifies = 0;
  secureRequired = false; sBleGattsIf = 4; debugAvailable = true; rejectLock = false;
  pSensorDataChar = &sensorChar; pSystemStatusChar = &systemChar; pEventNotifyChar = &eventChar;
}
// Runs one `blestream` argument string and checks reply and resulting flags.
static void streamCase(const char* label, uint8_t before, const char* args,
                       const char* reply, uint8_t after) {
  state.streamFlags = before;
  const char* got = cmd_blestream(args);
  expect(same(got, reply) && state.streamFlags == after,
         std::string(label) + ": `blestream " + args + "` should reply \"" + reply +
         "\"; got " + shown(got) + ", flags " + flagsHex());
}

static void streamCliChecks() {
  const char* usage = "Error: invalid arguments — Usage: blestream <on|off|sensors|system|events|interval>";
  const char* streamUsage = "Error: invalid arguments — Usage: blestream <sensors|system|events> [on|off]";
  const char* intervalUsage = "Error: invalid arguments — Usage: blestream interval <sensor_ms> <system_ms>";
  const uint8_t all = static_cast<uint8_t>(BLE_STREAM_ALL);
  const uint8_t sen = static_cast<uint8_t>(BLE_STREAM_SENSORS);
  const uint8_t sys = static_cast<uint8_t>(BLE_STREAM_SYSTEM);
  const uint8_t evt = static_cast<uint8_t>(BLE_STREAM_EVENTS);
  millisGuardRequired = false;
  streamReset();

  // C1/C7: the whole-stream keywords (the web page's Stream on/off buttons).
  streamCase("C1 all on", 0, "on", "All streams enabled", all);
  streamCase("C7 all off", all, "off", "All streams disabled", 0);
  streamCase("C7 keyword case", 0, "ON", "All streams enabled", all);
  streamCase("C7 trailing token", 0, "on extra", "All streams enabled", all);

  // C2/C3: one stream at a time; "<stream> off" clears only that stream.
  streamCase("C2 sensors off", all, "sensors off", "Sensor stream disabled",
             static_cast<uint8_t>(all & ~sen));
  streamCase("C3 sensors", 0, "sensors", "Sensor stream enabled", sen);
  streamCase("C3 system", sen, "system", "System stream enabled",
             static_cast<uint8_t>(sen | sys));
  streamCase("C3 events", static_cast<uint8_t>(sen | sys), "events",
             "Event stream enabled", static_cast<uint8_t>(sen | sys | evt));
  streamCase("C3 system off", static_cast<uint8_t>(sen | sys | evt), "system off",
             "System stream disabled", static_cast<uint8_t>(sen | evt));
  streamCase("C3 events on", 0, "events on", "Event stream enabled", evt);

  // C6: a leading unknown word or a non-exact toggle is a usage error.
  streamCase("C6 unknown word", 0, "x on", usage, 0);
  streamCase("C6 offset is not off", all, "sensors offset", streamUsage, all);
  streamCase("C6 bad toggle", all, "events foo", streamUsage, all);

  // C4/C5: the web page's Apply interval button and its error paths.
  state.sensorStreamInterval = 2000; state.systemStreamInterval = 9000;
  const char* got = cmd_blestream("interval 1000 5000");
  expect(same(got, "Intervals set: sensor=1000ms system=5000ms") &&
         state.sensorStreamInterval == 1000 && state.systemStreamInterval == 5000,
         "C4 interval: `blestream interval 1000 5000` applies both intervals; got " + shown(got) +
         ", intervals " + std::to_string(state.sensorStreamInterval) + "/" +
         std::to_string(state.systemStreamInterval));
  debugAvailable = false;
  got = cmd_blestream("interval 300 400");
  expect(same(got, "Intervals set") && state.sensorStreamInterval == 300 &&
         state.systemStreamInterval == 400,
         "C4 interval without debug buffer still applies; got " + shown(got));
  debugAvailable = true;
  state.sensorStreamInterval = 1000; state.systemStreamInterval = 5000;
  struct { const char* args; const char* reply; } badIntervals[] = {
      {"interval 50 5000", "Error: Intervals must be >= 100ms"},
      {"interval 1000 99", "Error: Intervals must be >= 100ms"},
      {"interval abc 5000", intervalUsage},
      {"interval -5 5000", intervalUsage},
      {"interval 0 5000", intervalUsage},
      {"interval 1000", intervalUsage},
      {"interval", intervalUsage}};
  for (const auto& bad : badIntervals) {
    got = cmd_blestream(bad.args);
    expect(same(got, bad.reply) && state.sensorStreamInterval == 1000 &&
           state.systemStreamInterval == 5000,
           std::string("C5 `blestream ") + bad.args + "` is refused with \"" + bad.reply +
           "\" and leaves the intervals; got " + shown(got) + ", intervals " +
           std::to_string(state.sensorStreamInterval) + "/" +
           std::to_string(state.systemStreamInterval));
  }

  // C8/C9: status path and the not-initialized refusal stay as they were.
  state.streamFlags = sen;
  got = cmd_blestream("");
  expect(got && std::strncmp(got, "Stats: sensors=", 15) == 0 && state.streamFlags == sen,
         "C8 bare `blestream` returns the status lines; got " + shown(got));
  debugAvailable = false;
  got = cmd_blestream("");
  expect(same(got, "Error: Debug buffer unavailable"),
         "C8 status without debug buffer is explicit; got " + shown(got));
  debugAvailable = true;
  state.initialized = false;
  streamCase("C9 not initialized", 0, "on", "Error: Bluetooth not initialized", 0);
  state.initialized = true;
  gBLEState = nullptr;
  got = cmd_blestream("on");
  gBLEState = &state;
  expect(same(got, "Error: Bluetooth not initialized") && state.streamFlags == 0,
         "C9 absent BLE state refuses; got " + shown(got));

  // C10: blesend forwards the whole message through the console-mirror sender.
  streamReset();
  got = cmd_blesend("hi");
  expect(same(got, "Message sent via BLE") && mirrorSent.size() == 1 && mirrorSent.back() == "hi",
         "C10 `blesend hi` sends \"hi\"; got " + shown(got) + ", sent " +
         (mirrorSent.empty() ? std::string("nothing") : "\"" + mirrorSent.back() + "\""));
  mirrorSent.clear();
  got = cmd_blesend("hello world");
  expect(same(got, "Message sent via BLE") && mirrorSent.size() == 1 &&
         mirrorSent.back() == "hello world",
         "C10 `blesend hello world` sends the whole message; got " + shown(got) + ", sent " +
         (mirrorSent.empty() ? std::string("nothing") : "\"" + mirrorSent.back() + "\""));
  mirrorSent.clear();
  got = cmd_blesend("");
  expect(same(got, "Error: invalid arguments — Usage: blesend <message>") && mirrorSent.empty(),
         "C10 empty blesend is a usage error; got " + shown(got));
  got = cmd_blesend("   ");
  expect(same(got, "Error: invalid arguments — Usage: blesend <message>") && mirrorSent.empty(),
         "C10 blank blesend is a usage error; got " + shown(got));
  mirrorAccept = false;
  got = cmd_blesend("hi");
  expect(same(got, "Error: Failed to send message"),
         "C10 refused send is reported; got " + shown(got));
  mirrorAccept = true;
  state.connectionState = BLE_STATE_IDLE;
  got = cmd_blesend("hi");
  expect(same(got, "Error: No client connected") && mirrorSent.empty(),
         "C10 no client refuses; got " + shown(got));
  state.connectionState = BLE_STATE_CONNECTED;

  // C11: bleevent builds the event JSON and sends it to each recipient only.
  streamReset();
  const unsigned takesBefore = takeCalls, givesBefore = giveCalls;
  got = cmd_bleevent("x \"q\"");
  expect(same(got, "Event sent via BLE"),
         "C11 `bleevent x \"q\"` is sent; got " + shown(got));
  expect(directedFrames.size() == 2 && broadcastNotifies == 0 && setValueCalls == 0,
         "C11 the event reaches each recipient by a directed notify, never characteristic-wide; "
         "directed " + std::to_string(directedFrames.size()) + ", broadcast " +
         std::to_string(broadcastNotifies));
  if (directedFrames.size() == 2) {
    expect(directedFrames[0].connId == 7 && directedFrames[1].connId == 9 &&
           directedFrames[0].handle == 0x27 && directedFrames[1].handle == 0x27 &&
           directedFrames[0].payload == directedFrames[1].payload,
           "C11 event frames go to conn 7 and 9 on the event characteristic");
    JsonDocument doc;
    expect(!deserializeJson(doc, directedFrames[0].payload) &&
           doc["type"] == static_cast<int>(BLE_EVENT_CUSTOM) && doc["msg"] == "x \"q\"" &&
           doc["ts"].is<unsigned>(),
           "C11 event JSON carries the whole escaped message; got " + directedFrames[0].payload);
  }
  expect(state.eventCount == 1 && lockDepth == 0 && takeCalls - takesBefore == 2 &&
         giveCalls - givesBefore == 2,
         "C11 one event counted, one guard per recipient, all released; events " +
         std::to_string(state.eventCount));
  directedFrames.clear();
  got = cmd_bleevent("x");
  expect(same(got, "Event sent via BLE") && directedFrames.size() == 2,
         "C11 `bleevent x` sends \"x\"; got " + shown(got));
  directedFrames.clear();
  got = cmd_bleevent("");
  expect(same(got, "Error: invalid arguments — Usage: bleevent <message>") && directedFrames.empty(),
         "C11 empty bleevent is a usage error; got " + shown(got));

  // C12: a stale recipient session is dropped under the guard.
  streamReset();
  liveEpochs[9] = 6;
  bool ok = blePushSensorData("{\"a\":1}", 7);
  expect(ok && directedFrames.size() == 1 && broadcastNotifies == 0 && setValueCalls == 0 &&
         state.sensorStreamCount == 1,
         "C12 sensor sample reaches only the live recipient; directed " +
         std::to_string(directedFrames.size()) + ", broadcast " + std::to_string(broadcastNotifies) +
         ", count " + std::to_string(state.sensorStreamCount));
  if (directedFrames.size() == 1) {
    expect(directedFrames[0].connId == 7 && directedFrames[0].handle == 0x21 &&
           directedFrames[0].payload == "{\"a\":1}",
           "C12 sensor frame goes to conn 7 on the sensor characteristic unchanged");
  }

  // C13: nobody eligible receives nothing and nothing is counted; a congested
  // recipient misses this sample while the other still receives it.
  streamReset();
  mirrorRecipients.clear();
  ok = blePushSystemStatus("{}", 2);
  expect(!ok && directedFrames.empty() && broadcastNotifies == 0 && state.systemStreamCount == 0,
         "C13 with no logged-in recipient the system status is not sent or counted; broadcast " +
         std::to_string(broadcastNotifies) + ", count " + std::to_string(state.systemStreamCount));
  mirrorRecipients = {{7, 3}, {9, 5}};
  congested = {7};
  ok = blePushSystemStatus("{}", 2);
  expect(ok && directedFrames.size() == 1 && directedFrames[0].connId == 9 &&
         directedFrames[0].handle == 0x24 && state.systemStreamCount == 1,
         "C13 a congested recipient misses one sample, the other receives it");

  // C14: secure-required mode and a missing GATT interface send nothing.
  streamReset();
  secureRequired = true;
  expect(!blePushSensorData("{}", 2) && !blePushSystemStatus("{}", 2) &&
         !blePushEvent(BLE_EVENT_CUSTOM, "m") && directedFrames.empty() &&
         broadcastNotifies == 0 && state.sensorStreamCount == 0 && state.eventCount == 0,
         "C14 streams stay suppressed while the Secure Channel is required");
  secureRequired = false;
  sBleGattsIf = ESP_GATT_IF_NONE;
  unsigned gives = giveCalls;
  expect(!blePushSensorData("{}", 2) && directedFrames.empty() && broadcastNotifies == 0 &&
         giveCalls - gives == 1 && lockDepth == 0,
         "C14 no GATT interface: nothing sent, the one guard taken is released");
  sBleGattsIf = 4;

  // C15: a refused lifecycle lock sends nothing and is never released.
  streamReset();
  rejectLock = true;
  gives = giveCalls;
  expect(!blePushSensorData("{}", 2) && directedFrames.empty() && broadcastNotifies == 0 &&
         giveCalls == gives && lockDepth == 0,
         "C15 refused lifecycle lock: nothing sent, nothing released");
  streamReset();
  state.connectionState = BLE_STATE_IDLE;
  expect(!blePushSensorData("{}", 2) && directedFrames.empty() && broadcastNotifies == 0,
         "C15 no connected client: nothing sent");
  state.connectionState = BLE_STATE_CONNECTED;
  pEventNotifyChar = nullptr;
  expect(!blePushEvent(BLE_EVENT_CUSTOM, "m") && directedFrames.empty(),
         "C15 missing event characteristic: nothing sent");

  streamReset();
  secureRequired = true;
  millisGuardRequired = true;
  populate();
  check(streamFailures == 0, "BLE stream command/delivery checks failed (listed above)");
}

// bledisconnect: `<connId>` closes exactly that app client, the bare form closes
// every app client, a paired ring/glasses link is never closed, and unknown or
// malformed ids are refused without a close. Non-aborting: every failed
// expectation is listed, then the run fails.
static unsigned disconnectFailures = 0;
static void disconnectExpect(bool ok, const std::string& what) {
  ++assertions;
  if (!ok) {
    ++disconnectFailures;
    std::cerr << "FAIL " << what << '\n';
  }
}
static bool closedAre(std::initializer_list<uint16_t> want) {
  return closedConns == std::vector<uint16_t>(want);
}
static std::string closedText() {
  std::string out = "{";
  for (size_t i = 0; i < closedConns.size(); ++i)
    out += (i ? "," : "") + std::to_string(closedConns[i]);
  return out + "}";
}
// apps: slot 0 = logged-in phone (conn 0), slot 1 = unknown app peer (conn 1).
// peers: slot 2 = the paired R1 ring (conn 2), slot 3 = a G2 temple (conn 3);
// links opened as central that the GATT server still gives a slot.
static void disconnectTable(bool apps, bool peers) {
  state = {}; gBLEState = &state; sBleLifecycleMutex = &lifecycleMutex;
  pServer = &bleServer; rejectLock = false; debugAvailable = true;
  state.initialized = true;
  int count = 0;
  auto add = [&count](int slot, uint16_t connId, BLEDeviceType type) {
    BLEConnection& c = state.connections[slot];
    c.active = true; c.connId = connId; c.deviceType = type;
    ++count;
  };
  if (apps) {
    add(0, 0, BLE_DEVICE_PHONE);
    state.connections[0].authed = true; state.connections[0].user = "alice";
    add(1, 1, BLE_DEVICE_UNKNOWN);
  }
  if (peers) {
    add(2, 2, BLE_DEVICE_RING);
    add(3, 3, BLE_DEVICE_GLASSES_LEFT);
  }
  state.activeConnectionCount = static_cast<uint8_t>(count);
  state.connectionState = count ? BLE_STATE_CONNECTED : BLE_STATE_IDLE;
  closedConns.clear(); closesUnderLock = 0;
}

static void disconnectChecks() {
  const char* usage = "Error: invalid arguments — Usage: bledisconnect [connId]";
  const char* notFound = "Error: No client with that connection id";
  const char* peerLink = "Error: That connection is a paired peer link, not an app client";
  const char* unavailable = "Error: Bluetooth not initialized. Run 'openble' first.";
  const char* none = "Error: No client connected";

  // K1: the web Kick on the second client (conn 1) closes conn 1 only.
  disconnectTable(true, true);
  const char* got = cmd_bledisconnect("1");
  disconnectExpect(closedAre({1}) && same(got, "Disconnecting client 1..."),
                   "K1 `bledisconnect 1` closes exactly conn 1; got " + shown(got) +
                   ", closed " + closedText());
  disconnectExpect(closesUnderLock == 0 && lockDepth == 0,
                   "K1 the close is issued with the lifecycle guard released");
  disconnectExpect(state.connections[1].active && state.activeConnectionCount == 4,
                   "K1 the slot stays until onDisconnect clears it");

  // K2: conn 0 by id.
  disconnectTable(true, true);
  got = cmd_bledisconnect("0");
  disconnectExpect(closedAre({0}) && same(got, "Disconnecting client 0..."),
                   "K2 `bledisconnect 0` closes exactly conn 0; got " + shown(got) +
                   ", closed " + closedText());

  // K3: an id with no active slot is refused.
  disconnectTable(true, true);
  got = cmd_bledisconnect("5");
  disconnectExpect(closedConns.empty() && same(got, notFound),
                   "K3 `bledisconnect 5` (no such connection) is refused; got " + shown(got) +
                   ", closed " + closedText());

  // K4: the ring and a G2 temple are paired peer links, never closed here.
  for (const char* peer : {"2", "3"}) {
    disconnectTable(true, true);
    got = cmd_bledisconnect(peer);
    disconnectExpect(closedConns.empty() && same(got, peerLink),
                     std::string("K4 `bledisconnect ") + peer + "` (peer link) is refused; got " +
                     shown(got) + ", closed " + closedText());
  }

  // K5: malformed ids never become conn 0; surrounding blanks are accepted.
  for (const char* bad : {"abc", "-1", "+1", "1x", "0x1", "1 2", "70000"}) {
    disconnectTable(true, true);
    got = cmd_bledisconnect(bad);
    disconnectExpect(closedConns.empty() && same(got, usage),
                     std::string("K5 `bledisconnect ") + bad + "` is a usage error; got " +
                     shown(got) + ", closed " + closedText());
  }
  disconnectTable(true, true);
  got = cmd_bledisconnect(" 1 ");
  disconnectExpect(closedAre({1}), "K5 `bledisconnect  1 ` closes conn 1; closed " + closedText());

  // K6: the bare form (page button, OLED and G2 items) closes every app client
  // and spares the peer links; one app client keeps the historical reply.
  for (const char* bare : {"", "   "}) {
    disconnectTable(true, true);
    got = cmd_bledisconnect(bare);
    disconnectExpect(closedAre({0, 1}) && same(got, "Disconnecting 2 clients...") &&
                     closesUnderLock == 0 && lockDepth == 0,
                     std::string("K6 `bledisconnect") + bare + "` closes conn 0 and 1 only; got " +
                     shown(got) + ", closed " + closedText());
  }
  disconnectTable(true, true);
  state.connections[1] = BLEConnection{};
  state.activeConnectionCount = 3;
  got = cmd_bledisconnect("");
  disconnectExpect(closedAre({0}) && same(got, "Disconnecting client..."),
                   "K6 one app client: bare `bledisconnect` closes it with the historical reply; "
                   "got " + shown(got) + ", closed " + closedText());
  disconnectTable(true, true);
  debugAvailable = false;
  got = cmd_bledisconnect("");
  disconnectExpect(closedAre({0, 1}) && same(got, "Disconnecting clients..."),
                   "K6 without the debug buffer the bare form still closes and replies; got " +
                   shown(got));
  got = cmd_bledisconnect("1");
  disconnectExpect(closedAre({0, 1, 1}) && same(got, "Disconnecting client..."),
                   "K6 without the debug buffer the id form still closes and replies; got " +
                   shown(got));
  debugAvailable = true;

  // K7/K8: only peer links, or nothing connected: nothing is closed.
  disconnectTable(false, true);
  got = cmd_bledisconnect("");
  disconnectExpect(closedConns.empty() && same(got, none),
                   "K7 ring/glasses only: bare `bledisconnect` closes nothing; got " + shown(got) +
                   ", closed " + closedText());
  disconnectTable(false, false);
  got = cmd_bledisconnect("");
  disconnectExpect(closedConns.empty() && same(got, none),
                   "K8 nothing connected keeps the historical reply; got " + shown(got));
  got = cmd_bledisconnect("0");
  disconnectExpect(closedConns.empty() && same(got, notFound),
                   "K8 nothing connected: `bledisconnect 0` is refused; got " + shown(got));

  // K9-K11: no BLE state, no server, or no lifecycle guard: nothing is closed
  // and nothing claims success.
  struct Fault { const char* label; void (*apply)(); };
  const Fault faults[] = {
      {"K9 absent BLE state", [] { gBLEState = nullptr; }},
      {"K10 absent GATT server", [] { pServer = nullptr; }},
      {"K11 absent lifecycle mutex", [] { sBleLifecycleMutex = nullptr; }},
      {"K11 refused lifecycle lock", [] { rejectLock = true; }}};
  for (const Fault& fault : faults) {
    disconnectTable(true, true);
    const unsigned gives = giveCalls;
    fault.apply();
    const char* byId = cmd_bledisconnect("1");
    const char* bare = cmd_bledisconnect("");
    const bool released = giveCalls == gives || !rejectLock;
    disconnectExpect(closedConns.empty() && same(byId, unavailable) && same(bare, none) &&
                     lockDepth == 0 && released,
                     std::string(fault.label) + ": nothing closed; `bledisconnect 1` got " +
                     shown(byId) + ", bare got " + shown(bare) + ", closed " + closedText());
  }

#if BLE_DISCONNECT_OWNER
  // Owner API used by the command.
  disconnectTable(true, true);
  const unsigned takes = takeCalls, gives = giveCalls;
  disconnectExpect(bleDisconnectConnection(2) == BleDisconnectResult::PEER_LINK &&
                   bleDisconnectConnection(3) == BleDisconnectResult::PEER_LINK &&
                   closedConns.empty(), "O1 the owner refuses ring and glasses links");
  disconnectExpect(bleDisconnectConnection(9) == BleDisconnectResult::NOT_FOUND &&
                   closedConns.empty(), "O2 the owner reports an unknown id");
  disconnectExpect(bleDisconnectConnection(1) == BleDisconnectResult::REQUESTED && closedAre({1}),
                   "O3 the owner closes the named app client");
  disconnectExpect(takeCalls - takes == 4 && giveCalls - gives == 4 && lockDepth == 0,
                   "O3 one lifecycle guard per request, all released");
  disconnectTable(true, true);
  disconnectExpect(bleDisconnectAppClients() == 2 && closedAre({0, 1}) && closesUnderLock == 0,
                   "O4 the owner closes every app client outside the guard; closed " + closedText());
  disconnectTable(false, true);
  disconnectExpect(bleDisconnectAppClients() == 0 && closedConns.empty(),
                   "O5 a peer-only table has no app client to close");
  disconnectTable(true, true);
  pServer = nullptr;
  disconnectExpect(bleDisconnectConnection(1) == BleDisconnectResult::UNAVAILABLE &&
                   bleDisconnectAppClients() == 0 && closedConns.empty(),
                   "O6 no GATT server: unavailable, nothing closed");
#endif

  rejectLock = false; pServer = &bleServer;
  closedConns.clear(); closesUnderLock = 0;
  populate();
  check(disconnectFailures == 0, "bledisconnect checks failed (listed above)");
}
#endif

int main() {
  characterize();
#if !TEST_BASELINE
  writerChecks();
  blestatusChecks();
  disconnectChecks();
  ownerChecks();
  streamCliChecks();
  webChecks();
#endif
  check(jsonAllocator.outstanding == 0, "JSON allocations released");
  std::cerr << "BLE info (G2=" << ENABLE_G2_GLASSES << ", baseline=" << TEST_BASELINE
            << "): " << assertions << " assertions passed\n";
}
