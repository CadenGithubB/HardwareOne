// Production state, formatter and command definitions are inserted by the runner.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

static unsigned assertions = 0;
static void check(bool value, const char* message) {
  ++assertions;
  if (!value) throw std::runtime_error(message);
}
void webBatchTestStringAppend() {}
class TestAllocator : public ArduinoJson::Allocator {
 public:
  int calls = 0, failAt = -1, outstanding = 0;
  std::unordered_map<void*, size_t> sizes;
  void* allocate(size_t size) override {
    if (calls++ == failAt) return nullptr;
    void* ptr = std::malloc(size);
    if (ptr) { ++outstanding; sizes[ptr] = size; }
    return ptr;
  }
  void deallocate(void* ptr) override {
    if (ptr) { --outstanding; sizes.erase(ptr); std::free(ptr); }
  }
  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    // ArduinoJson's string builder requires shrinking to succeed; retain its
    // already-owned memory and inject failures only for new/growing blocks.
    if (size <= sizes.at(ptr)) return ptr;
    if (calls++ == failAt) return nullptr;
    void* replacement = std::realloc(ptr, size);
    if (replacement) { sizes.erase(ptr); sizes[replacement] = size; }
    return replacement;
  }
  void reset(int failure = -1) { calls = 0; failAt = failure; }
};
static TestAllocator allocator;
#define PSRAM_JSON_DOC(name) JsonDocument name(&allocator)
#define EXT_RAM_BSS_ATTR
class BLEClient;
class BLERemoteCharacteristic;
using SemaphoreHandle_t = void*;
// INSERT_STATE
static G2RingState gRing;
static String gRingDeviceName, gRingDeviceAddress;
static volatile bool gRingScanFound = false, gRingConnectTaskActive = false;
static void* sRingOwnerTask = nullptr;
static int gRingConnectMux = 0;
static unsigned criticalDepth = 0, criticalCalls = 0, stackCalls = 0, timeCalls = 0;
static uint32_t fakeNow = 10000, stackFree = 4096;
static uint32_t millis() { ++timeCalls; return fakeNow; }
static unsigned uxTaskGetStackHighWaterMark(void* task) {
  check(task == sRingOwnerTask && task != nullptr, "stack sampled for current owner only");
  ++stackCalls; return stackFree;
}
#define portENTER_CRITICAL(mux) do { \
  check((mux) == &gRingConnectMux, "pending state uses existing critical section"); \
  ++criticalDepth; ++criticalCalls; \
} while (0)
#define portEXIT_CRITICAL(mux) do { \
  check((mux) == &gRingConnectMux && criticalDepth == 1, "pending reader releases critical section"); \
  --criticalDepth; \
} while (0)
bool gCLIValidateOnly = false;

// INSERT_PRODUCTION

static JsonDocument parsed(const char* output) {
  JsonDocument doc;
  check(!deserializeJson(doc, output), "ring output must parse as JSON");
  return doc;
}
static uint32_t expectedUp() {
  return gRing.connected && gRing.connectedSince ? fakeNow - gRing.connectedSince : 0;
}
static void checkSchema(JsonVariantConst doc) {
  const uint32_t free = sRingOwnerTask ? stackFree : RING_OWNER_STACK_BYTES;
  check(doc.as<JsonObjectConst>().size() == 12, "complete twelve-field ring schema preserved");
  check(doc["schema"] == 1 && doc["connected"] == gRing.connected &&
        doc["connectPending"] == gRingConnectTaskActive, "connection and pending facts preserved");
  check(doc["name"] == (gRingDeviceName.length() ? gRingDeviceName.c_str() : "") &&
        doc["addr"] == (gRingDeviceAddress.length() ? gRingDeviceAddress.c_str() : ""),
        "names and address preserve complete escaped values");
  check(doc["mtu"] == gRing.mtu && doc["rx"] == gRing.packetsReceived &&
        doc["upMs"] == expectedUp(), "MTU, counters and wrapping uptime preserved");
  check(doc["stackBytes"] == RING_OWNER_STACK_BYTES && doc["stackFreeMin"] == free &&
        doc["stackUsedMax"] == (free < RING_OWNER_STACK_BYTES ? RING_OWNER_STACK_BYTES - free : 0) &&
        doc["scanFound"] == gRingScanFound, "stack units, clamping and discovery preserved");
}
static void emit(const char* kind) {
  const std::string output = cmd_ringstatus("json");
  JsonDocument doc = parsed(output.c_str());
  checkSchema(doc.as<JsonVariantConst>());
  std::cout << kind << '\t' << output << '\n';
}
static void populate() {
  gRing = {};
  gRing.connected = true; gRing.connectedSince = 2000;
  gRing.packetsReceived = 42; gRing.mtu = 247;
  gRingDeviceName = "EVEN R1 Bench"; gRingDeviceAddress = "00:11:22:33:44:55";
  gRingConnectTaskActive = true; gRingScanFound = true;
  sRingOwnerTask = &gRing; stackFree = 4096; fakeNow = 10000;
}
static void characterize() {
  emit("JSON");
  gRingDeviceName.invalidateForTest(); gRingDeviceAddress.invalidateForTest();
  check(gRingDeviceName.c_str() == nullptr && gRingDeviceAddress.c_str() == nullptr,
        "host boundary represents invalid Arduino String storage");
  emit("JSON");
  std::cout << "TEXT\t" << cmd_ringstatus("") << '\n';
  populate(); emit("JSON");
  std::cout << "TEXT\t" << cmd_ringstatus("jsonish") << '\n';
  const std::string canonical = cmd_ringstatus("json");
  check(canonical == cmd_ringstatus(" json "),
        "existing whitespace JSON argument parsing remains accepted");
  gRing.connected = false; emit("JSON");
  gRing.connected = true; gRing.connectedSince = 0; emit("JSON");
  gRing.connectedSince = 0xfffffff0u; fakeNow = 0x20; emit("JSON");
  gRing.packetsReceived = 0xffffffffu; gRing.mtu = 0xffff;
  gRing.connectedSince = 1; fakeNow = 0; emit("JSON");
  for (uint32_t free : {0u, RING_OWNER_STACK_BYTES, RING_OWNER_STACK_BYTES + 100u}) {
    stackFree = free; emit("JSON");
  }
  sRingOwnerTask = nullptr; emit("JSON");
  populate();
  for (unsigned value = 1; value < 32; ++value) {
    gRingDeviceName = std::string("control-") + static_cast<char>(value) + "-end";
    emit("ESCAPE");
  }
  gRingDeviceName = "quote\"slash\\tab\tline\nreturn\r"; emit("ESCAPE");
  gRingDeviceName = "R1 caf\xc3\xa9"; emit("ESCAPE");
  gRingDeviceName = std::string("R1\0hidden", 9); emit("ESCAPE");
  populate();
  const unsigned beforeStack = stackCalls, beforeTime = timeCalls, beforeCritical = criticalCalls;
  allocator.reset(); gCLIValidateOnly = true;
  check(std::strcmp(cmd_ringstatus("json"), "VALID") == 0 &&
        std::strcmp(cmd_ringstatus(""), "VALID") == 0, "validation-only returns original response");
  check(stackCalls == beforeStack && timeCalls == beforeTime && criticalCalls == beforeCritical &&
        allocator.calls == 0, "validation returns before sampling or allocations");
  gCLIValidateOnly = false;
}

#if !TEST_BASELINE
static void serializerChecks() {
  std::string value;
  for (unsigned byte = 0; byte < 32; ++byte) value += static_cast<char>(byte);
  value += "\"/\\caf\xc3\xa9";
  JsonDocument doc;
  const JsonString text(value.c_str(), value.size(), false);
  doc[text] = text;
  for (bool pretty : {false, true}) {
    std::string output;
    const size_t written = pretty ? serializeJsonPretty(doc, output) : serializeJson(doc, output);
    const size_t measured = pretty ? measureJsonPretty(doc) : measureJson(doc);
    check(written == measured && output.size() == measured,
          "compact and pretty measured lengths equal actual escaped output bytes");
    JsonDocument roundTrip = parsed(output.c_str());
    JsonString restored = roundTrip[text].as<JsonString>();
    check(restored.size() == value.size() &&
          std::memcmp(restored.c_str(), value.data(), value.size()) == 0,
          "explicit-length embedded NUL and all other control values survive round trip");
    std::cout << "SERIALIZER\t";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : output) std::cout << hex[c >> 4] << hex[c & 15];
    std::cout << '\n';
  }
}
static void ownerChecks() {
  const unsigned beforeStack = stackCalls, beforeTime = timeCalls, beforeCritical = criticalCalls;
  check(!g2RingBuildStatusJson(JsonObject()), "null output is rejected");
  check(stackCalls == beforeStack && timeCalls == beforeTime && criticalCalls == beforeCritical,
        "null output avoids state sampling");
  {
    JsonDocument first, second;
    check(g2RingBuildStatusJson(first.to<JsonObject>()), "owner fills caller document");
    checkSchema(first.as<JsonVariantConst>());
    check(criticalCalls == beforeCritical + 1 && criticalDepth == 0,
          "pending state comes through the existing synchronized accessor");
    gRingDeviceName = "changed after sampling"; gRingDeviceAddress = "different address";
    check(first["name"] == "EVEN R1 Bench" && first["addr"] == "00:11:22:33:44:55",
          "names and address are owned after source replacement");
    check(g2RingBuildStatusJson(second.to<JsonObject>()), "independent second caller succeeds");
    second["name"] = "consumer edit";
    check(first["name"] == "EVEN R1 Bench" && gRingDeviceName == "changed after sampling",
          "caller documents and source have independent string ownership");
  }
  populate();
  // Locate the exact 320-byte boundary using the actual serializer. Only the
  // string length changes: no duplicate model of the formatter's field sizes.
  bool saw319 = false, saw320 = false;
  for (unsigned length = 0; length < 200; ++length) {
    gRingDeviceName = std::string(length, 'R');
    JsonDocument doc;
    check(g2RingBuildStatusJson(doc.to<JsonObject>()), "owner permits variable caller-sized output");
    const size_t bytes = measureJson(doc);
    if (bytes == 319) {
      checkSchema(parsed(cmd_ringstatus("json")).as<JsonVariantConst>());
      saw319 = true;
    } else if (bytes == 320) {
      check(std::strcmp(cmd_ringstatus("json"), "{\"error\":\"status too large\"}") == 0,
            "CLI rejects exact-capacity output requiring a NUL byte");
      saw320 = true;
    }
  }
  check(saw319 && saw320, "CLI boundary cases were both exercised");
  gRingDeviceName = std::string(1000, 'N');
  {
    JsonDocument doc;
    check(g2RingBuildStatusJson(doc.to<JsonObject>()), "owner permits output longer than CLI capacity");
    checkSchema(doc.as<JsonVariantConst>());
    check(measureJson(doc) > 1000 &&
          std::strcmp(cmd_ringstatus("json"), "{\"error\":\"status too large\"}") == 0,
          "oversized CLI output is explicit rather than missing fields");
  }
  populate(); allocator.reset();
  checkSchema(parsed(cmd_ringstatus("json")).as<JsonVariantConst>());
  const int allocations = allocator.calls;
  check(allocations > 0, "fault sweep observed real document allocations");
  for (int failure = 0; failure < allocations; ++failure) {
    allocator.reset(failure);
    check(std::strcmp(cmd_ringstatus("json"), "{\"error\":\"oom\"}") == 0,
          "every observed allocation failure gives complete explicit OOM JSON");
    check(allocator.outstanding == 0 && criticalDepth == 0,
          "failed CLI releases document storage and existing pending reader lock");
  }
  allocator.reset();
  checkSchema(parsed(cmd_ringstatus("json")).as<JsonVariantConst>());
}
#endif

int main() {
  characterize();
#if !TEST_BASELINE
  ownerChecks();
  serializerChecks();
#endif
  check(allocator.outstanding == 0, "all JSON allocator storage released");
  std::cerr << "Ring info (baseline=" << TEST_BASELINE << "): " << assertions << " assertions passed\n";
}
