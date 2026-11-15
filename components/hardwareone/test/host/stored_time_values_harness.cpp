// Host harness for clock values and intervals read back from JSON.
//
// test_stored_time_values.py inserts, from the production sources:
//   * System_User.cpp: struct BootAnchor and parseBootAnchors;
//   * System_Automation.cpp: struct Trigger, daysCsvToMask, parseOneTrigger
//     and nextFire.
// ArduinoJson is the real library. Host boundaries: the Arduino String stub,
// the PSRAM document macro and the event-kind lookup. `int` is 32 bits here as
// on the device, so a read through an int default loses a value above 2^31 in
// the same way.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

void webBatchTestStringAppend() {}
#define PSRAM_JSON_DOC(name) JsonDocument name
static constexpr size_t SYSEVT_DETAIL_LEN = 80;
static int systemEventKindFromName(const char*) { return 0; }

// INSERT_BOOT_ANCHORS

// INSERT_TRIGGERS

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

static std::string anchorsJson(const char* rows) {
  return std::string("{\"bootAnchors\":[") + rows + "]}";
}

// The trigger as the scheduler parses it from one automation's JSON.
static Trigger parse(const char* triggerJson, bool* ok = nullptr) {
  JsonDocument doc;
  check(!deserializeJson(doc, triggerJson), std::string("test JSON parses: ") + triggerJson);
  Trigger out;
  const bool parsed = parseOneTrigger(doc.as<JsonVariantConst>(), out);
  if (ok) *ok = parsed;
  return out;
}

int main() {
  BootAnchor anchors[8];

  // ---- boot anchors: written as uint32_t, read back as uint32_t ----------------
  // What writeBootAnchor() stores for a sync on a device that has been up a
  // few minutes: read back unchanged, as before.
  int n = parseBootAnchors(String(anchorsJson(
      R"({"ntpAnchorId":7,"epochAtSync":1763000000,"millisAtSync":184000})").c_str()), anchors, 8);
  check(n == 1 && anchors[0].ntpAnchorId == 7u && anchors[0].epochAtSync == 1763000000 &&
            anchors[0].millisAtSync == 184000u,
        "an ordinary anchor reads back unchanged");

  // The largest values below 2^31 read back unchanged.
  n = parseBootAnchors(String(anchorsJson(
      R"({"ntpAnchorId":2147483647,"epochAtSync":2147483647,"millisAtSync":2147483647})").c_str()), anchors, 8);
  check(n == 1 && anchors[0].ntpAnchorId == 2147483647u && anchors[0].epochAtSync == 2147483647 &&
            anchors[0].millisAtSync == 2147483647u,
        "values of 2^31 - 1 read back unchanged");

  // A sync taken after 34.7 days of uptime: millisAtSync is above 2^31.
  n = parseBootAnchors(String(anchorsJson(
      R"({"ntpAnchorId":9,"epochAtSync":1766000000,"millisAtSync":3000000000})").c_str()), anchors, 8);
  check(n == 1 && anchors[0].millisAtSync == 3000000000u,
        "millisAtSync above 2^31 reads back as written, not as 0");

  // January 2038 and later: epochAtSync is above 2^31.
  n = parseBootAnchors(String(anchorsJson(
      R"({"ntpAnchorId":11,"epochAtSync":2200000000,"millisAtSync":5000})").c_str()), anchors, 8);
  check(n == 1 && anchors[0].epochAtSync == static_cast<time_t>(2200000000u) && anchors[0].millisAtSync == 5000u,
        "an anchor from 2039 is kept with its epoch");
  n = parseBootAnchors(String(anchorsJson(
      R"({"ntpAnchorId":12,"epochAtSync":4294967295,"millisAtSync":4294967295})").c_str()), anchors, 8);
  check(n == 1 && anchors[0].epochAtSync == static_cast<time_t>(4294967295u) &&
            anchors[0].millisAtSync == 4294967295u,
        "the largest uint32_t values read back as written");

  // Rows that were never valid are still left out.
  n = parseBootAnchors(String(anchorsJson(
      R"({"ntpAnchorId":0,"epochAtSync":1763000000,"millisAtSync":1},)"
      R"({"ntpAnchorId":3,"epochAtSync":0,"millisAtSync":1},)"
      R"({"ntpAnchorId":-4,"epochAtSync":1763000000,"millisAtSync":1},)"
      R"({"ntpAnchorId":5,"epochAtSync":-1763000000,"millisAtSync":1},)"
      R"({"ntpAnchorId":"6","epochAtSync":1763000000,"millisAtSync":1},)"
      R"({"epochAtSync":1763000000,"millisAtSync":1},)"
      R"({"ntpAnchorId":8,"epochAtSync":1763000001,"millisAtSync":-5})").c_str()), anchors, 8);
  check(n == 1 && anchors[0].ntpAnchorId == 8u && anchors[0].millisAtSync == 0u,
        "anchors with a zero, negative, text or missing id or epoch are left out; a negative millis reads as 0");
  check(parseBootAnchors(String("{}"), anchors, 8) == 0 && parseBootAnchors(String("not json"), anchors, 8) == 0,
        "no array and broken JSON give no anchors");
  n = parseBootAnchors(String(anchorsJson(
      R"({"ntpAnchorId":1,"epochAtSync":1763000000,"millisAtSync":1},)"
      R"({"ntpAnchorId":2,"epochAtSync":1763000100,"millisAtSync":2},)"
      R"({"ntpAnchorId":3,"epochAtSync":1763000200,"millisAtSync":3})").c_str()), anchors, 2);
  check(n == 2 && anchors[1].ntpAnchorId == 2u, "the caller's capacity is respected");

  // ---- automation interval: stored as a number of ms, read back unsigned -------
  bool ok = false;
  Trigger t = parse(R"({"type":"interval","intervalMs":5000,"nextAt":1763000000})", &ok);
  check(ok && t.type == Trigger::INTERVAL && t.intervalMs == 5000u && t.nextAt == 1763000000,
        "a 5 s interval reads back unchanged");
  check(nextFire(t, 1763000000) == 1763000005, "and fires 5 s on");
  t = parse(R"({"type":"interval","intervalMs":2147483647})", &ok);
  check(ok && t.intervalMs == 2147483647u, "an interval of 2^31 - 1 ms reads back unchanged");
  check(nextFire(t, 1763000000) == 1763000000 + 2147483, "and fires 24.8 days on");

  // 30 days in ms is above 2^31: it used to read as 0, an interval that never fires.
  t = parse(R"({"type":"interval","intervalMs":2592000000})", &ok);
  check(ok && t.intervalMs == 2592000000u, "a 30 day interval reads back as written, not as 0");
  check(nextFire(t, 1763000000) == 1763000000 + 2592000, "and fires 30 days on");
  t = parse(R"({"type":"interval","intervalMs":4294967295})", &ok);
  check(ok && t.intervalMs == 4294967295u, "the largest uint32_t interval reads back as written");

  // What was never a valid interval still reads as "none".
  for (const char* none : {R"({"type":"interval"})", R"({"type":"interval","intervalMs":0})",
                           R"({"type":"interval","intervalMs":-5000})", R"({"type":"interval","intervalMs":"5000"})",
                           R"({"type":"interval","intervalMs":4294967296})"}) {
    t = parse(none, &ok);
    check(ok && t.intervalMs == 0u && nextFire(t, 1763000000) == 0, std::string("no interval: ") + none);
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d stored time value checks failed\n", failures, checks);
    return 1;
  }
  std::printf("stored time values: %d checks passed\n", checks);
  return 0;
}
