// Host harness for the 4 KB reply of `espnowsensorstatus json`: the
// production sensorStatusJsonFit of System_ESPNow_Sensors.cpp (between its
// SENSOR_STATUS_JSON_HOST_BEGIN and _END marks), compiled as it is with the
// vendored ArduinoJson.
//
// The reply leaves out the last devices until it fits, and says so with
// `devicesMore`. An earlier version cut to the buffer first and added the
// key afterwards: a document that ended up within 18 bytes of the buffer then
// ran over it, unterminated. Every document size around the buffer's size is
// tried here, with and without devices; each reply must be whole JSON, end
// inside the buffer and say truly whether a device was left out.
#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <string>

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

int main() {
  constexpr size_t kCap = 4096;
  static char buf[kCap + 64];
  unsigned cutSeen = 0, wholeSeen = 0;
  for (size_t filler = 2400; filler <= 4200; ++filler) {
    for (int deviceCount : { 0, 1, 3, 9 }) {
      JsonDocument doc;
      doc["schema"] = 1;
      doc["pad"] = std::string(filler, 'p');
      JsonArray devices = doc["devices"].to<JsonArray>();
      for (int i = 0; i < deviceCount; ++i) {
        JsonObject dev = devices.add<JsonObject>();
        dev["mac"] = "02:A0:00:00:00:01";
        dev["name"] = std::string(10 + (size_t)(filler + (size_t)i) % 23, 'n');
        dev["received"] = 1000 + i;
      }
      std::memset(buf, 'A', sizeof(buf));  // what lies behind the buffer
      const size_t n = sensorStatusJsonFit(doc, devices, "devicesMore", buf, kCap);
      const std::string what = "pad " + std::to_string(filler) + ", " + std::to_string(deviceCount) + " devices";
      if (n == 0) {
        check(buf[0] == '\0', what + ": a reply that cannot fit is empty, not cut");
        check(filler > 4000, what + ": only a document too large without any device fails");
        continue;
      }
      check(n < kCap && buf[n] == '\0' && std::strlen(buf) == n, what + ": ends inside the buffer, terminated");
      JsonDocument back;
      check(deserializeJson(back, buf, n) == DeserializationError::Ok, what + ": the reply is whole JSON");
      const size_t kept = back["devices"].size();
      const bool more = back["devicesMore"] | false;
      check(back["devicesMore"].is<bool>(), what + ": devicesMore is always there");
      check(more == (kept < (size_t)deviceCount), what + ": devicesMore says whether a device was left out");
      check(back["pad"].as<std::string>().size() == filler, what + ": nothing but devices is cut");
      if (more) ++cutSeen;
      else ++wholeSeen;
    }
  }
  check(cutSeen > 100 && wholeSeen > 100, "both cases were tried many times");
  // Without a list nothing is cut and no key is added.
  JsonDocument one;
  one["schema"] = 1;
  one["name"] = "porch";
  const size_t n = sensorStatusJsonFit(one, JsonArray(), nullptr, buf, kCap);
  check(n > 0 && std::string(buf) == "{\"schema\":1,\"name\":\"porch\"}", "a reply without a list is serialized as it is");
  JsonDocument big;
  big["pad"] = std::string(kCap, 'p');
  check(sensorStatusJsonFit(big, JsonArray(), nullptr, buf, kCap) == 0 && buf[0] == '\0',
        "a reply without a list that does not fit is empty");
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d status reply checks failed\n", failures, checks);
    return 1;
  }
  std::printf("espnowsensorstatus json: %d checks passed\n", checks);
  return 0;
}
