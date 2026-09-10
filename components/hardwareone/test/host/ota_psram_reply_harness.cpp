// Representative shipping handlers and memory macros are inserted below.
// Mock status/NVS data is not an implementation of the OTA state machine.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

static unsigned assertions = 0;
static void check(bool condition, const char* description) {
  ++assertions;
  if (!condition) throw std::runtime_error(description);
}
void webBatchTestStringAppend() {}
enum class AllocPref { PreferPSRAM, PreferInternal };
using AllocPolicy = AllocPref;
struct BufferAllocation { unsigned char* base; size_t size; std::string tag; };
static std::unordered_map<void*, BufferAllocation> buffers;
static unsigned bufferAttempts = 0;
static bool failNextBuffer = false;
static void* ps_alloc(size_t size, AllocPolicy policy, const char* tag) {
  ++bufferAttempts;
  check(policy == AllocPolicy::PreferPSRAM, "real buffer macro requests PSRAM preference");
  if (failNextBuffer) { failNextBuffer = false; return nullptr; }
  auto* base = static_cast<unsigned char*>(std::malloc(size + 32));
  check(base != nullptr, "host fixture allocation succeeds");
  std::memset(base, 0xa5, size + 32);
  void* output = base + 16;
  buffers[output] = {base, size, tag};
  return output;
}
static void checkCanaries() {
  for (const auto& entry : buffers) {
    for (size_t i = 0; i < 16; ++i) {
      check(entry.second.base[i] == 0xa5, "reply did not underflow its capacity");
      check(entry.second.base[16 + entry.second.size + i] == 0xa5,
            "reply did not overflow its original capacity");
    }
  }
}
static void expectCapacity(const char* pointer, size_t capacity) {
  const auto found = buffers.find(const_cast<char*>(pointer));
  check(found != buffers.end() && found->second.size == capacity,
        "handler uses exact original response capacity");
  check(std::strlen(pointer) < capacity, "response is NUL-terminated within capacity");
  checkCanaries();
}
class JsonAllocator : public ArduinoJson::Allocator {
 public:
  bool failNext = false;
  unsigned calls = 0;
  std::unordered_map<void*, size_t> live;
  void* allocate(size_t size) override {
    ++calls;
    if (failNext) { failNext = false; return nullptr; }
    void* ptr = std::malloc(size);
    if (ptr) live[ptr] = size;
    return ptr;
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
    check(live.erase(ptr) == 1, "JSON frees its owned pool/string once");
    std::free(ptr);
  }
  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    ++calls;
    if (failNext) { failNext = false; return nullptr; }
    auto found = live.find(ptr);
    check(found != live.end(), "JSON reallocates owned memory");
    void* replacement = std::realloc(ptr, size);
    if (replacement) { live.erase(found); live[replacement] = size; }
    return replacement;
  }
};
static JsonAllocator jsonAllocator;
static ArduinoJson::Allocator* psramJsonAllocator() { return &jsonAllocator; }

// INSERT_PRODUCTION_MEMORY_MACROS_HERE

struct AuthContext {};
struct FsLockGuard { explicit FsLockGuard(const char*) {} };
static constexpr char kCandidatePath[] = "candidate", kManifestPath[] = "manifest";
namespace VFS {
enum class Scopes { OTA };
static AuthContext systemAuth(Scopes, const char*) { return {}; }
static bool existsGuarded(const char*, const AuthContext&) { return true; }
}
#define HW1_OTA_BOARD_ID "test-board"
#define HW1_OTA_LAYOUT_ID "test-ota-layout"
struct hw1_ota_record_t {
  uint32_t sequence = 11;
  int phase = 2;
  uint64_t operation_id = 123;
  struct { int code = 1; uint32_t sequence = 9; char detail[96] = "previous update result"; } last_result;
};
struct hw1_ota_nvs_info_t {};
struct esp_partition_t { const char* label = "ota_0"; };
struct esp_app_desc_t { const char* version = "1.0.0+test"; };
using esp_ota_img_states_t = int;
static constexpr int ESP_OTA_IMG_UNDEFINED = 0;
static unsigned loadCalls = 0;
static bool journalOk = true;
static bool loadRecord(hw1_ota_record_t&, hw1_ota_nvs_info_t*, bool repair,
                       char* reason, size_t capacity) {
  check(repair, "status preserves its original journal repair mode");
  ++loadCalls;
  if (!journalOk) std::snprintf(reason, capacity, "test journal unavailable");
  return journalOk;
}
static const esp_partition_t* esp_ota_get_running_partition() { static esp_partition_t part; return &part; }
static int esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* state) { *state = 2; return 0; }
static const esp_app_desc_t* esp_app_get_description() { static esp_app_desc_t desc; return &desc; }
static uint32_t otaSlotSize() { return 0x480000; }
static bool credentialConfigured() { return true; }
static const char* phaseName(int) { return "requested"; }
static const char* resultName(int) { return "success"; }
static bool hw1_ota_result_pending(const hw1_ota_record_t*) { return true; }
enum OtaProbationCause { OTA_PROBATION_NONE, OTA_PROBATION_TEST };
static bool otaSafetyTakeProbationAbort(OtaProbationCause* cause, uint32_t* uptime,
                                       char* detail, size_t capacity, bool clear) {
  check(!clear, "status does not consume probation diagnostic");
  *cause = OTA_PROBATION_TEST; *uptime = 1000;
  std::snprintf(detail, capacity, "test probation detail");
  return true;
}
static const char* otaSafetyProbationCauseName(OtaProbationCause) { return "test"; }

static struct {
  bool active = true;
  char member[16] = "candidate";
  unsigned received = 12, expected = 64;
  char warning[96] = "";
} gBleUpload;
static constexpr size_t kBleUploadChunkMax = 477;
static unsigned gBleUploadFramesAccepted = 1, gBleUploadFramesRejected = 0;
static unsigned gBleUploadResumedFrom = 0, gBleUploadResumeMs = 0;
static const char* gBleUploadLastTeardown = "none";
static unsigned cmdExecDropCountLoad() { return 0; }

using nvs_handle_t = int;
using esp_err_t = int;
static constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 2, NVS_READWRITE = 1;
static constexpr int ORIGIN_SERIAL = 1, SYSEVT_OTA_RESULT = 1;
static constexpr char HW1_OTA_NVS_NAMESPACE[] = "hw1up";
static constexpr char HW1_OTA_NVS_SLOT_A_KEY[] = "tx_a", HW1_OTA_NVS_SLOT_B_KEY[] = "tx_b";
struct CommandContext { int origin = ORIGIN_SERIAL; };
static CommandContext context;
static bool automation = false, validateOnly = false;
static unsigned nvsOpens = 0, nvsErases = 0, nvsCommits = 0, events = 0;
static bool nvsOpenFails = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validateOnly) return "VALID"; } while (0)
static bool commandIsAutomation() { return automation; }
static void* currentCommandContext() { return &context; }
static int nvs_open(const char*, int, nvs_handle_t* handle) {
  ++nvsOpens; *handle = nvsOpenFails ? 0 : 1; return nvsOpenFails ? 1 : ESP_OK;
}
static int nvs_erase_key(nvs_handle_t, const char*) { ++nvsErases; return ESP_OK; }
static int nvs_commit(nvs_handle_t) { ++nvsCommits; return ESP_OK; }
static void nvs_close(nvs_handle_t) {}
static const char* esp_err_to_name(int) { return "test NVS failure with a message longer than a pointer"; }
static void systemEventPost(int, const char*, const char*) { ++events; }

// INSERT_PRODUCTION_OTA_REPLIES_HERE

// Exercises the real macro and real formatter without copying otawrite's
// upload state machine. Source guards ensure otawrite performs this allocation
// before upload mutations and passes response_SIZE at all five callsites.
static const char* invokeBleReply(bool success, bool final, const char* error = nullptr) {
  PSRAM_STATIC_BUF(response, 320);
  return bleUploadJson(response, response_SIZE, success, final, error);
}
static JsonDocument decoded(const char* text) {
  JsonDocument doc;
  check(!deserializeJson(doc, text), "OTA response is complete valid JSON");
  return doc;
}
static void testStatus() {
  check(bufferAttempts == 0 && buffers.empty(), "reply memory starts unallocated");
  check(std::string(cmdOtaStatus("invalid")).find("Usage:") != std::string::npos && bufferAttempts == 0,
        "invalid status argument does not allocate");
  failNextBuffer = true;
  check(std::string(cmdOtaStatus("json")) == "Error: Failed to allocate buffer",
        "status buffer OOM returns bounded error");
  check(loadCalls == 0 && buffers.empty(), "status buffer OOM occurs before journal access/repair");
  const char* json = cmdOtaStatus("json");
  expectCapacity(json, 1536);
  auto doc = decoded(json);
  check(doc["available"] == true && doc["slotSize"] == 0x480000 && doc["board"] == HW1_OTA_BOARD_ID,
        "status JSON content remains intact");
  check(doc["lastProbationAbortDetail"] == "test probation detail", "status trailing fields survive capacity handling");
  const unsigned attempts = bufferAttempts;
  const char* text = cmdOtaStatus("");
  check(text == json && bufferAttempts == attempts, "status text/JSON reuse persistent buffer");
  check(std::strlen(text) > 128 && std::string(text).find("Last result:") != std::string::npos,
        "status text uses real capacity rather than sizeof pointer");
  expectCapacity(text, 1536);
  journalOk = false;
  auto unavailable = decoded(cmdOtaStatus("json"));
  check(unavailable["journalOk"] == false && unavailable["journalError"] == "test journal unavailable",
        "status preserves journal error details");
  journalOk = true;
  jsonAllocator.failNext = true;
  check(std::string(cmdOtaStatus("json")) == "Error: OTA status response allocation failed",
        "real ArduinoJson allocation failure is reported instead of partial status");
  check(bufferAttempts == attempts && jsonAllocator.live.empty(), "JSON OOM preserves reply allocation and frees JSON pools");
  check(decoded(cmdOtaStatus("json"))["available"] == true, "status JSON retry recovers after allocator failure");
}
static void testBleReply() {
  const unsigned attempts = bufferAttempts, jsonCalls = jsonAllocator.calls;
  failNextBuffer = true;
  check(std::string(invokeBleReply(true, false)) == "Error: Failed to allocate buffer",
        "BLE reply macro fails cleanly on first allocation");
  check(jsonAllocator.calls == jsonCalls, "missing BLE buffer prevents serializer work");
  const char* reply = invokeBleReply(true, false);
  expectCapacity(reply, 320);
  auto doc = decoded(reply);
  check(doc["success"] == true && doc["size"] == 12 && doc["chunkMax"] == 477,
        "BLE reply contents and chunk limit remain unchanged");
  check(bufferAttempts == attempts + 2, "failed lazy allocation is retried exactly once");
  gBleUpload.received = 32;
  const char* next = invokeBleReply(false, true, "explicit test error");
  check(next == reply && bufferAttempts == attempts + 2, "BLE reply retains stable persistent storage");
  auto changed = decoded(next);
  check(changed["size"] == 32 && changed["final"] == true && changed["error"] == "explicit test error",
        "BLE response rewrites current fields without pointer-size truncation");
  expectCapacity(next, 320);
  jsonAllocator.failNext = true;
  check(std::string(invokeBleReply(true, true)) ==
        "Error: OTA response allocation failed; upload state may have changed",
        "BLE JSON OOM reports uncertainty rather than successful partial JSON");
  check(gBleUpload.received == 32 && jsonAllocator.live.empty(), "reply OOM does not fake upload rollback or leak JSON pools");
  check(decoded(invokeBleReply(true, true))["final"] == true, "BLE JSON retry recovers after allocator failure");
}
static void testJournalReset() {
  const unsigned attempts = bufferAttempts;
  context.origin = 99;
  check(std::string(cmdOtaResetJournal("confirm")).find("physical serial") != std::string::npos,
        "journal repair stays serial-only");
  context.origin = ORIGIN_SERIAL;
  check(std::string(cmdOtaResetJournal("invalid")).find("Usage:") != std::string::npos,
        "journal repair still requires confirmation");
  check(bufferAttempts == attempts && nvsOpens == 0, "rejected journal repair neither allocates nor opens NVS");
  failNextBuffer = true;
  check(std::string(cmdOtaResetJournal("confirm")) == "Error: Failed to allocate buffer",
        "journal reply allocation failure is reported");
  check(nvsOpens == 0 && nvsErases == 0 && nvsCommits == 0 && events == 0,
        "reply OOM precedes every journal mutation/event");
  check(std::string(cmdOtaResetJournal("confirm")).find("OTA journal reset;") == 0,
        "journal repair succeeds after allocation retry");
  check(nvsOpens == 1 && nvsErases == 2 && nvsCommits == 1 && events == 1,
        "successful journal repair preserves operation counts");
  nvsOpenFails = true;
  const char* reply = cmdOtaResetJournal("confirm");
  expectCapacity(reply, 192);
  check(std::strlen(reply) > 50 && std::string(reply).find("test NVS failure") != std::string::npos,
        "journal errors use original 192-byte buffer capacity");
  check(bufferAttempts == attempts + 2, "later journal calls reuse allocated reply");
}
int main() {
  try {
    testStatus(); testBleReply(); testJournalReset();
    check(jsonAllocator.calls > 0 && jsonAllocator.live.empty(), "real JSON allocator is used and all document storage released");
    checkCanaries();
    // Persistent firmware buffers intentionally live until reboot. Release the
    // host fixture only after its final handler call so leak sanitizers remain
    // useful without changing production lifetime or dangling-pointer tests.
    for (const auto& entry : buffers) std::free(entry.second.base);
    buffers.clear();
    std::cout << "OTA extracted reply tests passed (" << assertions
              << " assertions; real macros/ArduinoJson, mocked platform boundaries)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "OTA reply test failure: " << error.what() << '\n';
    return 1;
  }
}
