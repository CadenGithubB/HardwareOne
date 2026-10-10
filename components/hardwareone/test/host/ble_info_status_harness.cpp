// Shipping definitions are inserted by test_ble_info_status.py.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
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
static uint32_t millis() {
#if !TEST_BASELINE
  check(lockDepth > 0, "connection age must be read under lifecycle guard");
#endif
  return nowMs;
}
static bool argWantsJson(const String& args) { return args == "json"; }
static char debugBuffer[1024];
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
struct BLECharacteristic {
  const char* data = "help";
  const uint8_t* getData() { return reinterpret_cast<const uint8_t*>(data); }
  size_t getLength() { return std::strlen(data); }
};
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
using esp_err_t = int;
static constexpr int ESP_OK = 0, SOURCE_INTERNAL = 1, HTTPD_RESP_USE_STRLEN = -1;
#define EXT_RAM_BSS_ATTR
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
#endif

int main() {
  characterize();
#if !TEST_BASELINE
  writerChecks();
  ownerChecks();
  webChecks();
#endif
  check(jsonAllocator.outstanding == 0, "JSON allocations released");
  std::cerr << "BLE info (G2=" << ENABLE_G2_GLASSES << ", baseline=" << TEST_BASELINE
            << "): " << assertions << " assertions passed\n";
}
