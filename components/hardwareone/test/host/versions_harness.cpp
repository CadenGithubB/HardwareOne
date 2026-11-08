// `versions`: the production collector (spliced from System_Versions.cpp by
// test_versions.py) over the production formatter (System_VersionsCore.h).
// Compile-time constants come from the real headers, or from their real
// source lines where the header needs the SDK; runtime reads are fixtures.
// Each feature selection is a separate compile, so an item whose boundary is
// compiled out is proven absent, not just empty.
//
//   versions_harness text|json   the collected report, as the command prints it
//   versions_harness addjson     `status json`'s "versions" object (versionsAddJson)
//   versions_harness selftest    formatter edge cases
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include <ArduinoJson.h>

#include "System_VersionsCore.h"

#include "BLE_PhonePipeConsoleCore.h"
#include "BLE_PhonePipeCore.h"
#include "esp_now_hosted_companion.h"
#include "esp_now_hosted_rpc.h"
#include "hw1_ota_protocol.h"
#if ENABLE_LOCAL_STT
#include "stt/stt_model_selection.h"
#endif

// ---- SDK fixtures ------------------------------------------------------------
struct esp_app_desc_t { char version[32]; };
static const esp_app_desc_t kApp = {"0.99.99-fixture"};
const esp_app_desc_t* esp_app_get_description() { return &kApp; }
const char* esp_get_idf_version() { return "v5.5.5-fixture"; }
#define ESP_ARDUINO_VERSION_STR "3.3.0"
#define ESP_NOW_MAX_IE_DATA_LEN 250  // Espressif: ESP-NOW v1 frame payload

// ---- Real constants (generated from production sources) ---------------------
// INSERT_CONSTANTS

// ---- Owner fixtures ------------------------------------------------------------
#define HW1_OTA_LAYOUT_ID "fixture-layout"
bool otaFactoryUpdaterVersion(char* out, size_t size) {
#if FIXTURE_UPDATER_PRESENT
  std::snprintf(out, size, "%s", "1.1.0+fixture");
  return true;
#else
  if (size) out[0] = '\0';
  return false;
#endif
}
uint32_t otaDataSchemaVersion() { return kFixtureDataSchema; }
uint32_t espnowStackVersion() { return FIXTURE_ESPNOW_STACK; }
uint8_t espnowIdentityFileVersion() { return kFixtureIdentityFileVersion; }
uint8_t peerIdentityFileVersion() { return kFixturePeerFileVersion; }
const char* bleScProtocolLabel() { return kFixtureScLabel; }
const char* cm5HostControlProtocolVersion() { return kFixtureCm5Version; }
namespace hw1phone { inline constexpr const char* kPipeLabel = kFixturePipeLabel; }

const RadioCompanionIdentity& radioHalIdentity() {
  static RadioCompanionIdentity id = [] {
    RadioCompanionIdentity v{};
    v.versionKnown = FIXTURE_COMPANION_KNOWN;
    v.major = 2; v.minor = 12; v.patch = 13;
    v.infoKnown = FIXTURE_COMPANION_KNOWN;
    v.bridgeWire = 1;
    v.nativeEspNowVersion = FIXTURE_COMPANION_KNOWN ? 2 : 0;
    return v;
  }();
  return id;
}

// The list's allocator: plain heap, so the sanitizer sees versionsAddJson free
// the list after copying its strings into the document.
enum class AllocPolicy { PreferPSRAM };
void* ps_alloc(size_t size, AllocPolicy, const char*) { return std::malloc(size); }
void ps_free(void* memory) { std::free(memory); }

// ---- Production collector ------------------------------------------------------
// INSERT_COLLECT

// ---- Production `status json` versions object ----------------------------------
// INSERT_ADDJSON

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } } while (0)

static int selftest() {
  hw1versions::List list;
  list.add("a", "Short", "%s", "1");
  list.add("bb", "A longer label", "%s", "two \"quoted\" \\ and\ttab");
  list.add("hidden", nullptr, "%d", 4);
  char text[256];
  const size_t n = hw1versions::formatText(list, text, sizeof(text));
  CHECK(n == std::strlen(text), "text length is returned");
  CHECK(std::strcmp(text, "Short           1\nA longer label  two \"quoted\" \\ and\ttab") == 0,
        "labels align, a JSON-only item is not printed");
  char json[256];
  hw1versions::formatJson(list, json, sizeof(json));
  CHECK(std::strcmp(json, "{\"schema\":1,\"a\":\"1\",\"bb\":\"two \\\"quoted\\\" \\\\ and\\u0009tab\","
                          "\"hidden\":\"4\"}") == 0, "json escapes and keeps JSON-only items");
  // Too small: refused whole, never cut.
  char small[16];
  CHECK(hw1versions::formatText(list, small, sizeof(small)) == 0 && small[0] == '\0', "text refuses to cut");
  CHECK(hw1versions::formatJson(list, small, sizeof(small)) == 0 && small[0] == '\0', "json refuses to cut");
  // A value longer than kValueMax is cut at the item, not past it.
  char longValue[hw1versions::kValueMax * 2];
  std::memset(longValue, 'x', sizeof(longValue) - 1);
  longValue[sizeof(longValue) - 1] = '\0';
  list.add("long", "Long", "%s", longValue);
  CHECK(std::strlen(list.find("long")) == hw1versions::kValueMax - 1, "value is cut to the item size");
  // Full list: the extra item is dropped and reported.
  hw1versions::List full;
  for (size_t i = 0; i < hw1versions::kMaxItems; ++i) full.add("k", "K", "%zu", i);
  CHECK(!full.overflowed, "a full list has not overflowed");
  full.add("x", "X", "%s", "over");
  CHECK(full.overflowed && full.count == hw1versions::kMaxItems && !full.find("x"), "overflow is flagged");
  full.clear();
  CHECK(full.count == 0 && !full.overflowed, "clear resets");
  if (failures) return 1;
  std::puts("selftest ok");
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  if (std::strcmp(argv[1], "selftest") == 0) return selftest();
  if (std::strcmp(argv[1], "addjson") == 0) {
    JsonDocument doc;
    versionsAddJson(doc["versions"].to<JsonObject>());
    if (doc.overflowed()) { std::fputs("document overflowed\n", stderr); return 1; }
    std::string out;
    serializeJson(doc, out);
    std::puts(out.c_str());
    return 0;
  }
  static hw1versions::List list;
  versionsCollect(list);
  if (list.overflowed) { std::fputs("list overflowed\n", stderr); return 1; }
  static char out[4096];
  const bool json = std::strcmp(argv[1], "json") == 0;
  const size_t n = json ? hw1versions::formatJson(list, out, sizeof(out))
                        : hw1versions::formatText(list, out, sizeof(out));
  if (!n) { std::fputs("does not fit\n", stderr); return 1; }
  std::puts(out);
  return 0;
}
