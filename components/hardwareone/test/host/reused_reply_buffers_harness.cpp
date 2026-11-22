// Host harness for the two recurring reply buffers that are kept for reuse:
// the /api/cli/logs response (handleLogs, WebServer_Server.cpp) and the MQTT
// state document (publishMqttSensorData, System_MQTT.cpp).
// test_reused_reply_buffers.py splices the production functions in at the
// markers: handleLogs with the production WebMirrorBuf::snapshotTo, and
// publishMqttSensorData with its kept buffer and release helper, built with
// every sensor section and ESP-NOW off (the document is built by
// System_MQTTPayloadCore.h with the vendored ArduinoJson). The request, the
// auth helpers, the mirror's mutex, the MQTT client, the clock, the heap
// figures and ps_alloc are doubles; ps_alloc records the policy and size of
// every block and can refuse PSRAM or everything.
#include <Arduino.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

static int gFailures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++gFailures;
  }
}

// ---- allocation double -------------------------------------------------------
enum class AllocPref : uint8_t {
  PreferPsram = 0, PreferInternal = 1, RequirePsram = 2, RequireInternal = 3, DefaultHeap = 4
};
struct Request { AllocPref policy; size_t size; std::string tag; };
static std::vector<Request> gRequests;
static std::set<void*> gLive;
static bool gRefusePsram = false, gRefuseAll = false;
void* ps_alloc(size_t size, AllocPref policy, const char* tag) {
  gRequests.push_back({policy, size, tag ? tag : ""});
  if (gRefuseAll || (gRefusePsram && policy == AllocPref::RequirePsram)) return nullptr;
  void* block = std::malloc(size);
  gLive.insert(block);
  return block;
}
static void trackedFree(void* block) {
  if (!block) return;
  check(gLive.erase(block) == 1, "free releases a block ps_alloc made");
  std::free(block);
}
static void resetRequests() { gRequests.clear(); }
// Log lines: the arguments are evaluated, nothing is written.
template <typename... Args> static void hostLogSink(const char*, const Args&...) {}

#if defined(TEST_LOGS)
// ---- /api/cli/logs ------------------------------------------------------------
using CommandSource = int;
using TransportSessionEpoch = uint32_t;
struct AuthContext { CommandSource transport = 1; String user = "admin"; String ip = "local"; };
struct httpd_req_t { std::string type, headers, body, query, status; unsigned sends = 0; };
static AuthContext makeWebAuthCtx(httpd_req_t*) { return AuthContext{}; }
static bool tgRequireAuth(AuthContext&) { return true; }
static bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return true; }
static TransportSessionEpoch captureTransportSessionEpoch(const AuthContext&) { return 7; }
static bool cliHelpModeOwnedBySession(CommandSource, TransportSessionEpoch) { return false; }
static int httpd_resp_set_hdr(httpd_req_t* r, const char* k, const char* v) {
  r->headers += std::string(k) + ": " + v + "\n"; return 0;
}
static int httpd_resp_set_type(httpd_req_t* r, const char* type) { r->type = type; return 0; }
static int httpd_resp_set_status(httpd_req_t* r, const char* status) { r->status = status; return 0; }
static int httpd_resp_send(httpd_req_t* r, const char* body, size_t length) {
  r->body.assign(body, length); ++r->sends; return 0;
}
static int httpd_resp_sendstr(httpd_req_t* r, const char* body) {
  return httpd_resp_send(r, body, std::strlen(body));
}
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
static esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t len) {
  if (r->query.empty() || r->query.size() >= len) return ESP_FAIL;
  std::memcpy(buf, r->query.c_str(), r->query.size() + 1);
  return ESP_OK;
}
// Single key=value pairs are enough for these polls.
static esp_err_t httpd_query_key_value(const char* query, const char* key, char* val, size_t len) {
  const std::string q(query), prefix = std::string(key) + "=";
  if (q.compare(0, prefix.size(), prefix) != 0) return ESP_FAIL;
  const std::string v = q.substr(prefix.size(), q.find('&') == std::string::npos ? std::string::npos
                                                                                : q.find('&') - prefix.size());
  if (v.size() >= len) return ESP_FAIL;
  std::memcpy(val, v.c_str(), v.size() + 1);
  return ESP_OK;
}
#define DEBUG_HTTPF(...) hostLogSink(__VA_ARGS__)
#define ERROR_WEBF(...) hostLogSink(__VA_ARGS__)
using SemaphoreHandle_t = int*;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
static int xSemaphoreTake(SemaphoreHandle_t, int) { return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t) {}
static int gMutex = 0;
struct WebMirrorBuf {
  char* buf = nullptr;
  size_t cap = 0, len = 0;
  uint32_t seq = 0;
  SemaphoreHandle_t mutex = &gMutex;
  void init(size_t capacity) { cap = capacity; seq = 1; buf = static_cast<char*>(std::malloc(cap)); buf[0] = 0; }
  size_t snapshotTo(char* dest, size_t destSize, uint32_t* seqOut = nullptr);
};
static WebMirrorBuf gWebMirror;
static size_t gWebMirrorCap = 8192;
#define free trackedFree
// INSERT_LOGS
#undef free

int main() {
  gWebMirror.init(gWebMirrorCap);
  const std::string text = "[1000] boot\n[2000] wifi up\n[3000] " + std::string(300, 'x');
  std::memcpy(gWebMirror.buf, text.c_str(), text.size() + 1);
  gWebMirror.len = text.size();

  // No memory at all: the existing error reply, nothing kept.
  gRefuseAll = true; resetRequests();
  { httpd_req_t r; handleLogs(&r);
    check(r.body == "[ERROR] Memory allocation failed" && r.type == "text/plain" &&
          r.headers == "X-HW1-CLI-Help: inactive\n" && gLive.empty(), "no memory: the error reply"); }
  gRefuseAll = false;

  // PSRAM refuses: each poll borrows a PreferPsram block and frees it.
  gRefusePsram = true;
  for (int poll = 0; poll < 2; ++poll) {
    resetRequests();
    httpd_req_t r; handleLogs(&r);
    check(r.body == text && r.sends == 1, "PSRAM refused: the mirror text is sent");
    check(gRequests.size() == 2 && gRequests[0].policy == AllocPref::RequirePsram &&
          gRequests[1].policy == AllocPref::PreferPsram && gRequests[1].size == gWebMirrorCap &&
          gRequests[1].tag == "handleLogs.resp", "PSRAM refused: the poll borrows a PreferPsram block");
    check(gLive.empty(), "PSRAM refused: the borrowed block is freed after the send");
  }
  gRefusePsram = false;

  // PSRAM available: the first poll makes the buffer, later polls reuse it.
  resetRequests();
  { httpd_req_t r; handleLogs(&r);
    check(r.body == text && r.type == "text/plain" &&
          r.headers == "X-HW1-CLI-Help: inactive\nX-HW1-Log-Seq: 1\n",
          "first kept poll: same reply, with the mirror's change counter"); }
  check(gRequests.size() == 1 && gRequests[0].policy == AllocPref::RequirePsram &&
        gRequests[0].size == gWebMirrorCap && gRequests[0].tag == "handleLogs.resp" && gLive.size() == 1,
        "first kept poll: one RequirePsram block of the mirror capacity, kept");
  for (int poll = 0; poll < 3; ++poll) {
    resetRequests();
    gWebMirror.len = text.size() - poll * 10;  // the mirror text changes between polls
    httpd_req_t r; handleLogs(&r);
    check(r.body == text.substr(0, gWebMirror.len), "later polls: the current mirror text");
    check(gRequests.empty() && gLive.size() == 1, "later polls: no allocation, the kept block reused");
  }
  // ?since= the current counter: 204, no body, nothing copied or allocated.
  gWebMirror.seq = 42; resetRequests();
  { httpd_req_t r; r.query = "since=42"; handleLogs(&r);
    check(r.status == "204 No Content" && r.body.empty() && r.sends == 1 && gRequests.empty() &&
          r.headers == "X-HW1-CLI-Help: inactive\n", "unchanged mirror: 204 with the help header only"); }
  // A stale or zero counter gets the whole mirror and the new counter.
  for (const char* q : {"since=41", "since=0", "since=", "other=42"}) {
    resetRequests();
    httpd_req_t r; r.query = q; handleLogs(&r);
    check(r.status.empty() && r.body == text.substr(0, gWebMirror.len) &&
          r.headers == "X-HW1-CLI-Help: inactive\nX-HW1-Log-Seq: 42\n",
          "changed or unknown counter: the whole mirror and its counter");
  }
  // An empty mirror sends an empty body from the kept buffer.
  gWebMirror.len = 0; resetRequests();
  { httpd_req_t r; handleLogs(&r); check(r.body.empty() && r.sends == 1 && gRequests.empty(), "empty mirror"); }
  // The kept block keeps its own size: a later, larger gWebMirrorCap must not
  // make a poll copy past the 8 KB block that was kept.
  const size_t keptCap = gWebMirrorCap;
  std::free(gWebMirror.buf);
  gWebMirrorCap = 2 * keptCap;
  gWebMirror.init(gWebMirrorCap);
  const std::string longText(keptCap + 4000, 'y');
  std::memcpy(gWebMirror.buf, longText.c_str(), longText.size() + 1);
  gWebMirror.len = longText.size();
  resetRequests();
  { httpd_req_t r; handleLogs(&r);
    check(r.body == longText.substr(0, keptCap - 1) && gRequests.empty() && gLive.size() == 1,
          "a larger mirror capacity: the kept block is filled to its own size only"); }
  if (gFailures) return 1;
  std::puts("handleLogs: one kept PSRAM reply buffer; borrowed and freed only when PSRAM refuses");
  return 0;
}

#elif defined(TEST_MQTT)
// ---- MQTT state document -------------------------------------------------------
#define ENABLE_THERMAL_SENSOR 0
#define ENABLE_TOF_SENSOR 0
#define ENABLE_IMU_SENSOR 0
#define ENABLE_PRESENCE_SENSOR 0
#define ENABLE_GPS_SENSOR 0
#define ENABLE_APDS_SENSOR 0
#define ENABLE_RTC_SENSOR 0
#define ENABLE_OLED_INPUT 0
#define ENABLE_ESPNOW 0
#include "System_MQTTPayloadCore.h"
#define PSRAM_JSON_DOC(name) JsonDocument name
struct MqttClientUse { bool admitted() const { return true; } };
static std::atomic<bool> mqttTofConnected{true};
using esp_mqtt_client_handle_t = void*;
static int gClient = 0;
static esp_mqtt_client_handle_t mqttClient = &gClient;
static unsigned long gNowMs = 123456;
static unsigned long millis() { return gNowMs; }
struct { bool mqttPublishSystem = true; bool mqttPublishWifi = false; String mqttBaseTopic = "hw1/test"; } gSettings;
namespace SelfDevice {
uint64_t uptimeSeconds() { return 3600; }
uint32_t freeHeapBytes() { return 150000; }
uint32_t minFreeHeapBytes() { return 120000; }
uint32_t psramFreeBytes() { return 6000000; }
String deviceName() { return "HardwareOne-B2C3"; }
bool deviceNameSet() { return false; }
}
namespace Clock {
bool isSynced() { return false; }
long long epochSeconds() { return 0; }
}
#define MALLOC_CAP_SPIRAM 1024
static size_t heap_caps_get_total_size(unsigned) { return 8388608; }
struct HostIp { String toString() const { return "0"; } };
struct { bool isConnected() const { return false; } String SSID() const { return "x"; }
         int RSSI() const { return 0; } HostIp localIP() const { return {}; } } WiFi;
static std::vector<std::string> gWarnings;
#define WARN_MQTTF(...) \
  do { char w_[256]; std::snprintf(w_, sizeof(w_), __VA_ARGS__); gWarnings.push_back(w_); } while (0)
#define DEBUG_MQTT_PUBSUBF(...) hostLogSink(__VA_ARGS__)
static std::vector<std::string> gPublished;
static int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char* topic, const char* data, int len, int, bool) {
  check(len > 0 && std::strlen(data) == (size_t)len, "the state document is published with its length");
  gPublished.push_back(std::string(topic) + " " + data);
  return 1;
}
#define free trackedFree
// INSERT_MQTT
#undef free

int main() {
  // U6 (2026-11-20): deviceName / deviceNameSet and system.psram_free /
  // psram_total join the document; "time" only once the clock is set.
  const std::string expected =
      "hw1/test/state {\"timestamp\":123456,\"deviceName\":\"HardwareOne-B2C3\",\"deviceNameSet\":false,"
      "\"system\":{\"uptime\":3600,\"heap_free\":150000,\"heap_min\":120000,\"psram_free\":6000000,"
      "\"psram_total\":8388608}}";
  // PSRAM refuses: the publish borrows a PreferPsram block and frees it.
  gRefusePsram = true; resetRequests();
  publishMqttSensorData();
  check(gPublished.size() == 1 && gPublished[0] == expected, "PSRAM refused: the same state document");
  check(gRequests.size() == 2 && gRequests[0].policy == AllocPref::RequirePsram &&
        gRequests[1].policy == AllocPref::PreferPsram && gRequests[1].size == 16384 &&
        gRequests[1].tag == "mqtt.json" && gLive.empty(), "PSRAM refused: borrowed and freed");
  gRefusePsram = false;
  // No memory: the existing warning, nothing published, nothing kept.
  gRefuseAll = true; gPublished.clear();
  publishMqttSensorData();
  check(gPublished.empty() && gWarnings.size() == 1 && gWarnings[0] == "Failed to allocate JSON buffer" &&
        gLive.empty(), "no memory: warned, not published");
  gRefuseAll = false;
  // PSRAM available: made at the first publish, reused by the next ones.
  gPublished.clear(); resetRequests();
  publishMqttSensorData();
  check(gRequests.size() == 1 && gRequests[0].policy == AllocPref::RequirePsram && gRequests[0].size == 16384 &&
        gRequests[0].tag == "mqtt.json" && gLive.size() == 1, "first publish: one kept RequirePsram block");
  for (int i = 0; i < 3; ++i) {
    resetRequests(); gNowMs += 10000;
    publishMqttSensorData();
    check(gRequests.empty() && gLive.size() == 1, "later publishes: no allocation");
  }
  check(gPublished.size() == 4 && gPublished[0] == expected &&
        gPublished[3] == "hw1/test/state {\"timestamp\":153456,\"deviceName\":\"HardwareOne-B2C3\","
                         "\"deviceNameSet\":false,\"system\":{\"uptime\":3600,\"heap_free\":150000,"
                         "\"heap_min\":120000,\"psram_free\":6000000,\"psram_total\":8388608}}",
        "kept buffer: the same documents");
  // The client stop releases it; the next client makes it again.
  releaseMqttStateJson();
  check(gLive.empty(), "stop releases the kept buffer");
  releaseMqttStateJson();
  check(gLive.empty(), "a second release is harmless");
  resetRequests(); publishMqttSensorData();
  check(gRequests.size() == 1 && gLive.size() == 1, "the next client's first publish makes it again");
  releaseMqttStateJson();
  if (gFailures) return 1;
  std::puts("publishMqttSensorData: one PSRAM state buffer kept for the client; borrowed only when PSRAM refuses");
  return 0;
}
#endif
