// Host harness for the automation id range.
//
// System_AutomationId.h is included as it is (test_automation_ids.py puts the
// component directory on the include path). The driver inserts the production
// automationIdExistsInJson and sanitizeAutomationsJson (System_Automation.cpp).
// ArduinoJson is the real library. Host boundaries: the Arduino String stub, a
// settable 32-bit millis(), a counting random(), the scratch allocator and the
// log macros.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "System_AutomationId.h"

void webBatchTestStringAppend() {}
#define PSRAM_JSON_DOC(name) JsonDocument name
#define DEBUG_AUTOMATIONSF(...) ((void)0)
#define ERROR_SYSTEMF(...) ((void)0)
enum class AllocPref { RequireInternal, PreferPsram };
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::malloc(size); }
static void ps_free(void* memory) { std::free(memory); }

static uint32_t gNowMs = 0;
static uint32_t millis() { return gNowMs; }
static long gRandomStep = 1;
static long random(long low, long high) {
  const long value = low + gRandomStep % (high - low);
  ++gRandomStep;
  return value;
}

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

static bool inRange(uint64_t id) { return id >= 1 && id <= 0x7FFFFFFFull; }

// The ids of a stored file, as numbers.
static std::vector<uint64_t> idsOf(const String& json) {
  JsonDocument doc;
  check(!deserializeJson(doc, json.c_str()), "the file parses");
  std::vector<uint64_t> ids;
  for (JsonObjectConst automation : doc["automations"].as<JsonArrayConst>()) {
    ids.push_back(automation["id"].as<uint64_t>());
  }
  return ids;
}
// What every reader of the id sees: the device's 32-bit `long`. ArduinoJson
// gives 0 for a number that does not fit.
static int32_t asDeviceLong(const String& json, size_t index) {
  JsonDocument doc;
  deserializeJson(doc, json.c_str());
  return doc["automations"][index]["id"].as<int32_t>();
}

int main() {
  // ---- the range owner ----------------------------------------------------------
  check(automationIdSeed(5000) == 5000u, "a young device: the id is the uptime");
  check(automationIdSeed(0x7FFFFFFFu) == 0x7FFFFFFFu, "the last uptime that fits is itself");
  check(automationIdSeed(0) == 1u, "an id is never 0");
  check(automationIdSeed(0x80000000u) == 1u, "2^31 folds to 0, which becomes 1");
  for (uint32_t nowMs : {0x80000001u, 2200000000u, 3000000000u, 0xFFFFFFFFu, 0x7FFFFFFFu, 123456789u, 1u}) {
    check(inRange(automationIdSeed(nowMs)), "a new id fits a signed long at uptime " + std::to_string(nowMs) + " ms");
  }
  check(automationIdSeed(3000000000u) == 3000000000u - 0x80000000u, "above 2^31 the uptime is folded");
  check(automationIdAdvance(100, 7) == 107u, "the next candidate is the step further on");
  check(automationIdAdvance(0x7FFFFFFFu, 1) == 1u, "stepping off the end of the range wraps to 1");
  for (uint32_t id : {1u, 5000u, 0x7FFF0000u, 0x7FFFFFFEu, 0x7FFFFFFFu}) {
    for (uint32_t stepBy : {1u, 2u, 99999u, 100000u}) {
      check(inRange(automationIdAdvance(id, stepBy)), "a stepped id stays in range");
    }
  }
  for (const char* above : {"2147483648", "3000000000", " 3000000000 ", "4294967295"}) {
    check(automationIdTextAboveRange(above), std::string("above the range: '") + above + "'");
  }
  for (const char* other : {"0", "1", "5000", "2147483647", "-5", "-3000000000", "4294967296", "99999999999",
                            "3000000000.5", "3e9", "\"3000000000\"", "abc", ""}) {
    check(!automationIdTextAboveRange(other), std::string("not above the range: '") + other + "'");
  }
  check(!automationIdTextAboveRange(nullptr), "no text is not above the range");

  // ---- the sanitizer ---------------------------------------------------------------
  // A file whose ids are fine is left alone, the largest id that fits included.
  {
    String json(R"({"version":3,"automations":[{"id":1,"name":"a"},{"id":5000,"name":"b"},{"id":2147483647,"name":"c"}]})");
    const std::string before = json.c_str();
    check(!sanitizeAutomationsJson(json) && before == json.c_str(), "a file with ids in range is not changed");
  }
  // A duplicate, found on a device that has been up 34.7 days: millis() is
  // 3000000000. The replacement used to be exactly that number.
  {
    gNowMs = 3000000000u;
    String json(R"({"automations":[{"id":100,"name":"first"},{"id":100,"name":"second"}]})");
    check(sanitizeAutomationsJson(json), "a duplicate id is replaced");
    const auto ids = idsOf(json);
    check(ids.size() == 2 && ids[0] == 100 && ids[1] != 100, "the first holder keeps the id, the second gets a new one");
    check(ids.size() == 2 && inRange(ids[1]), "the new id fits a signed long after 34.7 days of uptime");
    check(asDeviceLong(json, 1) == static_cast<int32_t>(ids[1]) && asDeviceLong(json, 1) > 0,
          "a reader that takes the id as long sees the id, not 0");
  }
  // An id above the range, as the old code made it: it is given a new one.
  {
    gNowMs = 60000;
    String json(R"({"automations":[{"id":7,"name":"ok"},{"id":3000000000,"name":"made on day 35","enabled":true,)"
                R"("commands":["ledon"],"triggers":[{"type":"interval","intervalMs":60000,"nextAt":1763000000}]}]})");
    check(asDeviceLong(json, 1) == 0, "before: a reader that takes the id as long sees 0");
    check(sanitizeAutomationsJson(json), "an id above the range is replaced");
    const auto ids = idsOf(json);
    check(ids.size() == 2 && ids[0] == 7 && inRange(ids[1]) && ids[1] != 7, "it gets an id in range; the other is kept");
    JsonDocument doc;
    deserializeJson(doc, json.c_str());
    JsonObjectConst second = doc["automations"][1];
    check(std::string(second["name"] | "") == "made on day 35" && (second["enabled"] | false) &&
              std::string(second["commands"][0] | "") == "ledon" &&
              second["triggers"][0]["intervalMs"].as<uint32_t>() == 60000u &&
              second["triggers"][0]["nextAt"].as<uint32_t>() == 1763000000u,
          "the rest of the automation is unchanged");
    const std::string once = json.c_str();
    check(!sanitizeAutomationsJson(json) && once == json.c_str(), "a second pass changes nothing");
  }
  // Two ids above the range and one in range that the new ids must not take.
  {
    gNowMs = 5000;  // the first candidate is 5000, which is taken
    gRandomStep = 1;
    String json(R"({"automations":[{"id":5000},{"id":2147483648},{"id":4294967295},{"id":5000}]})");
    check(sanitizeAutomationsJson(json), "ids above the range and a duplicate are replaced");
    const auto ids = idsOf(json);
    std::set<uint64_t> distinct(ids.begin(), ids.end());
    check(ids.size() == 4 && distinct.size() == 4, "all four ids are different afterwards");
    bool allFit = true;
    for (uint64_t id : ids) allFit = allFit && inRange(id);
    check(allFit && ids[0] == 5000, "all fit a signed long, and the first holder of 5000 keeps it");
  }
  // The id text check and the sanitizer agree: what the scheduler flags, the sanitizer changes.
  for (const char* id : {"2147483648", "3000000000", "4294967295"}) {
    gNowMs = 1234;
    String json((std::string(R"({"automations":[{"id":)") + id + "}]}").c_str());
    check(automationIdTextAboveRange(id) && sanitizeAutomationsJson(json) && inRange(idsOf(json)[0]),
          std::string("a flagged id is renumbered: ") + id);
  }
  // automationIdExistsInJson finds what the add path writes.
  check(automationIdExistsInJson(String("{\n  \"id\": 852516352,\n"), 852516352ul), "the add path's own id form is found");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d automation id checks failed\n", failures, checks);
    return 1;
  }
  std::printf("automation ids: %d checks passed\n", checks);
  return 0;
}
