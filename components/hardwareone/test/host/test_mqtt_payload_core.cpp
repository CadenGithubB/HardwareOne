// The MQTT state document and Home Assistant discovery configs
// (System_MQTTPayloadCore.h), with the vendored ArduinoJson.
//
// U6 of the 2026-11-20 surface review: the state document was hand-built with
// snprintf (an SSID with a quote broke it), the thermal section never fit its
// buffer, the HA templates read keys no sensor builder sends (ToF, IMU, GPS
// satellites, presence, thermal, RTC temperature), GPS speed was labelled km/h
// while the value is knots, the gyro °/s while it is rad/s, and every device
// claimed to be an "ESP32-S3". Each check below fails on that code.
//
// The fixtures are the sensor builders' text with sample values.
// test_mqtt_payload_sources.py checks that each fixture has exactly the keys of
// its builder's format strings, so a template here resolves only if the key
// is one the firmware really sends.
#include "System_MQTTPayloadCore.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    ++gChecks;                                         \
    if (!(cond)) {                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
      gFailures++;                                     \
    }                                                  \
  } while (0)

using namespace mqttpayload;

// ---------------------------------------------------------------------------
// Builder fixtures. Each "fixture:" line names the production builder.
// ---------------------------------------------------------------------------

// fixture: i2csensor_mlx90640.cpp int thermalBuildSummaryJson(
static const char kFxThermal[] = R"json({"valid":true,"connected":true,"ts":123456,"min":21.5,"avg":24.3,"max":31.0})json";
// fixture: i2csensor_vl53l4cx.cpp int tofBuildDataJson(
static const char kFxTof[] = R"json({"valid":true,"connected":true,"ts":123456,"objects":[{"id":1,"distance_mm":1460,"status":0,"valid":true},{"id":3,"distance_mm":812,"status":0,"valid":true}]})json";
// fixture: HAL_Motion.cpp int imuBuildDataJson(
static const char kFxImu[] = R"json({"valid":true,"connected":true,"ts":123456,"accel":{"x":0.12,"y":-0.03,"z":9.81},"gyro":{"x":0.001,"y":-0.002,"z":0.000},"ori":{"yaw":12.50,"pitch":1.25,"roll":-0.75},"temp":27,"mag":{"x":21.50,"y":-4.25,"z":-40.06},"cal":255})json";
// fixture: i2csensor_sths34pf80.cpp int presenceBuildDataJson(
static const char kFxPresence[] = R"json({"valid":true,"connected":true,"ts":123456,"ambient":22.75,"presence":1500,"presenceDetected":true,"motion":40,"motionDetected":false,"tempShock":0,"tempShockDetected":false})json";
// fixture: i2csensor_pa1010d.cpp int gpsBuildDataJson(
static const char kFxGpsFix[] = R"json({"valid":true,"connected":true,"ts":123456,"fix":1,"quality":1,"sats":9,"lat":47.606200,"lon":-122.332100,"alt":56.20,"speed":3.40})json";
// fixture: i2csensor_pa1010d.cpp int gpsBuildDataJson(
static const char kFxGpsNoFix[] = R"json({"valid":true,"connected":true,"ts":123456,"fix":0,"quality":0,"sats":3,"lat":0,"lon":0,"alt":0,"speed":0})json";
// fixture: i2csensor_apds9960.cpp int apdsBuildDataJson(
static const char kFxApds[] = R"json({"valid":true,"connected":true,"ts":123456,"r":120,"g":95,"b":80,"c":310,"proximity":12})json";
// fixture: i2csensor_ds3231.cpp int rtcBuildDataJson(
static const char kFxRtc[] = R"json({"valid":true,"connected":true,"ts":123456,"year":2026,"month":11,"day":20,"hour":17,"minute":4,"second":9,"temp":24.5})json";
// fixture: i2csensor_seesaw.cpp int gamepadBuildDataJson(
static const char kFxGamepad[] = R"json({"valid":true,"connected":true,"ts":123456,"x":512,"y":498,"buttons":4294967295})json";
// fixture: i2csensor_ano_encoder.cpp int anoEncoderBuildDataJson(
static const char kFxAno[] = R"json({"valid":true,"connected":true,"ts":123456,"pos":-3,"axis":1,"buttons":0})json";
// fixture: Input_GPIOEncoder.cpp int gpioEncoderBuildDataJson(
static const char kFxGpioEncoder[] = R"json({"connected":true,"running":true,"valid":true,"ts":123456,"pos":7,"axis":0,"pendingDetents":0,"buttons":0,"pressed":false,"sampleMs":2,"auxButtonsEnabled":false})json";

template <size_t N>
static bool add(JsonDocument& doc, const char* key, const char (&text)[N]) {
  return std::strcmp(key, "rtc") == 0 ? addRtcSection(doc, text, N - 1)
                                      : addSection(doc, key, text, N - 1);
}

static StateInput plainInput() {
  StateInput in;
  in.uptimeMs = 4000000000u;
  in.hasTime = true;
  in.epochUtc = 1795194249;  // 2026-11-20T17:04:09Z
  in.deviceName = "HardwareOne-B2C3";
  in.deviceNameSet = false;
  in.hasSystem = true;
  in.uptimeS = 86400;
  in.heapFree = 61234;
  in.heapMin = 40211;
  in.hasPsram = true;
  in.psramFree = 6123456;
  in.psramTotal = 8388608;
  in.hasWifi = true;
  in.ssid = "Lab";
  in.rssi = -61;
  in.ip = "192.168.1.40";
  return in;
}

// A state document with every section; `encoder` picks the input device.
static void fullState(JsonDocument& doc, bool encoder) {
  CHECK(beginState(plainInput(), doc), "beginState");
  CHECK(add(doc, "thermal", kFxThermal), "thermal fixture parses");
  CHECK(add(doc, "tof", kFxTof), "tof fixture parses");
  CHECK(add(doc, "imu", kFxImu), "imu fixture parses");
  CHECK(add(doc, "presence", kFxPresence), "presence fixture parses");
  CHECK(add(doc, "gps", kFxGpsFix), "gps fixture parses");
  CHECK(add(doc, "apds", kFxApds), "apds fixture parses");
  CHECK(add(doc, "rtc", kFxRtc), "rtc fixture parses");
  CHECK(encoder ? add(doc, "input", kFxAno) : add(doc, "input", kFxGamepad), "input fixture parses");
}

// Resolves "a.b[0].c" in `root`; null when any step is missing.
static JsonVariantConst resolve(JsonVariantConst root, const std::string& path) {
  JsonVariantConst at = root;
  size_t i = 0;
  while (i < path.size()) {
    size_t end = path.find_first_of(".[", i);
    if (end == std::string::npos) end = path.size();
    const std::string key = path.substr(i, end - i);
    if (!key.empty()) {
      if (!at.is<JsonObjectConst>() || !at.as<JsonObjectConst>()[key.c_str()].is<JsonVariantConst>())
        return JsonVariantConst();
      at = at[key.c_str()];
      if (at.isNull()) return at;
    }
    i = end;
    if (i < path.size() && path[i] == '[') {
      const size_t close = path.find(']', i);
      const size_t index = std::stoul(path.substr(i + 1, close - i - 1));
      if (!at.is<JsonArrayConst>() || at.as<JsonArrayConst>().size() <= index) return JsonVariantConst();
      at = at[index];
      i = close + 1;
    }
    if (i < path.size() && path[i] == '.') ++i;
  }
  return at;
}

// Every "value_json.<path>" a template reads.
static std::vector<std::string> templatePaths(const char* tmpl) {
  std::vector<std::string> out;
  const std::string text = tmpl;
  const std::string marker = "value_json.";
  size_t at = 0;
  while ((at = text.find(marker, at)) != std::string::npos) {
    at += marker.size();
    size_t end = at;
    while (end < text.size() &&
           (std::isalnum((unsigned char)text[end]) || text[end] == '_' || text[end] == '.' ||
            text[end] == '[' || text[end] == ']'))
      ++end;
    std::string path = text.substr(at, end - at);
    while (!path.empty() && path.back() == '.') path.pop_back();
    out.push_back(path);
    at = end;
  }
  return out;
}

static const HaEntity* entity(const char* objectId, uint8_t scope) {
  for (size_t i = 0; i < kEntityCount; ++i)
    if (std::strcmp(kEntities[i].objectId, objectId) == 0 && (kEntities[i].scope & scope)) return &kEntities[i];
  return nullptr;
}

static std::string str(const char* s) { return s ? s : "(null)"; }

// ---------------------------------------------------------------------------

static void test_ssid_and_names_escape() {
  const char* ssids[] = {
      "a\"b\\c",                                    // quote and backslash
      "\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"",  // 32 quotes
      ("tab\there\x01" "end"),                      // control bytes
      "caf\xc3\xa9 \\\" }{",                        // UTF-8 and JSON punctuation
  };
  for (const char* ssid : ssids) {
    StateInput in = plainInput();
    in.ssid = ssid;
    JsonDocument doc;
    CHECK(beginState(in, doc), "beginState with ssid");
    char text[kStateCap];
    const size_t n = serializeBounded(doc, text, sizeof(text));
    CHECK(n > 0, "state with a hostile SSID serializes");
    JsonDocument back;
    CHECK(!deserializeJson(back, text, n), "state with SSID %s parses back", ssid);
    CHECK(std::string(back["wifi"]["ssid"] | "") == ssid, "SSID round-trips: %s", ssid);
  }

  // Not UTF-8 (a raw Latin-1 byte, a cut sequence, a surrogate): replaced, so
  // the document stays UTF-8 for HA's parser.
  StateInput in = plainInput();
  in.ssid = "caf\xe9" "-\xc3" "-\xed\xa0\x80";
  JsonDocument doc;
  beginState(in, doc);
  CHECK(std::string(doc["wifi"]["ssid"] | "") == "caf?-?-???", "invalid UTF-8 replaced: %s",
        doc["wifi"]["ssid"].as<const char*>());
  char cut[6];
  CHECK(copyUtf8("ab\xc3\xa9\xc3\xa9", cut, sizeof(cut)) == 4 && std::string(cut) == "ab\xc3\xa9",
        "copyUtf8 never cuts a character");

  // Discovery: friendly name and room with quotes, backslashes, braces.
  const HaEntity& uptime = *entity("uptime", kLocal);
  DeviceInfo dev;
  dev.id = "hardwareone_0123456789AB";
  dev.name = "Kid's \"Lab\" \\ {desk}";
  dev.area = "Room \"7\"\\";
  dev.model = "Seeed XIAO ESP32S3";
  dev.swVersion = "0.99.99.1";
  JsonDocument disc;
  CHECK(buildDiscovery(uptime, "hardwareone_0123456789AB_uptime", "hw1/state", "hw1/availability",
                       dev, disc),
        "discovery builds");
  char text[kDiscoveryCap];
  const size_t n = serializeBounded(disc, text, sizeof(text));
  JsonDocument back;
  CHECK(n > 0 && !deserializeJson(back, text, n), "discovery with quotes parses back");
  CHECK(std::string(back["device"]["name"] | "") == dev.name, "device name round-trips");
  CHECK(std::string(back["device"]["suggested_area"] | "") == dev.area, "room round-trips");
}

static void test_templates_read_sent_keys() {
  JsonDocument pad, enc;
  fullState(pad, false);
  fullState(enc, true);
  for (size_t i = 0; i < kEntityCount; ++i) {
    const HaEntity& e = kEntities[i];
    JsonDocument peerDoc;
    JsonDocument* doc = (e.section & kInputEncoder) && !(e.section & kInputPad) ? &enc : &pad;
    if (e.scope == kPeer) {
      // A peer's document: builder sections plus system.online / last_seen.
      peerDoc["system"]["online"] = true;
      peerDoc["system"]["last_seen"] = 3;
      doc = &peerDoc;
    }
    CHECK(!resolve(doc->as<JsonVariantConst>(), e.field).isNull(), "%s: field %s is in the state",
          e.objectId, e.field);
    for (const std::string& path : templatePaths(e.valueTemplate)) {
      CHECK(!resolve(doc->as<JsonVariantConst>(), path).isNull(), "%s: template reads %s, not sent",
            e.objectId, path.c_str());
    }
    // A sensor value is shown only while its section is valid.
    const std::string field = e.field;
    const std::string section = field.substr(0, field.find('.'));
    if (section != "system" && section != "wifi" && section != "rtc") {
      CHECK(std::strstr(e.valueTemplate, ("value_json." + section + ".valid").c_str()) != nullptr,
            "%s: template checks %s.valid", e.objectId, section.c_str());
    }
    // The table and the template agree on the key.
    const std::string last = field.substr(field.find_last_of(".]") + 1);
    CHECK(std::strstr(e.valueTemplate, last.c_str()) != nullptr, "%s: template names %s", e.objectId,
          last.c_str());
  }
  // The peer rows also resolve in a peer document built from the same builders.
  JsonDocument peer;
  peer["system"]["online"] = true;
  peer["system"]["last_seen"] = 3;
  CHECK(add(peer, "tof", kFxTof) && add(peer, "imu", kFxImu) && add(peer, "gps", kFxGpsFix), "peer sections");
  for (size_t i = 0; i < kEntityCount; ++i) {
    const HaEntity& e = kEntities[i];
    if (!(e.scope & kPeer)) continue;
    for (const std::string& path : templatePaths(e.valueTemplate)) {
      CHECK(!resolve(peer.as<JsonVariantConst>(), path).isNull(), "peer %s: template reads %s",
            e.objectId, path.c_str());
    }
  }
  // Values that the old templates read are now really there.
  CHECK(resolve(pad.as<JsonVariantConst>(), "tof.objects[1].distance_mm").as<int>() == 812, "ToF");
  CHECK(resolve(pad.as<JsonVariantConst>(), "imu.accel.z").as<float>() > 9.8f, "IMU accel");
  CHECK(resolve(pad.as<JsonVariantConst>(), "thermal.max").as<float>() == 31.0f, "thermal");
  CHECK(resolve(pad.as<JsonVariantConst>(), "gps.sats").as<int>() == 9, "GPS sats");
  CHECK(resolve(pad.as<JsonVariantConst>(), "rtc.temp").as<float>() == 24.5f, "RTC temp");
  // ToF reads the nearest target, GPS position needs a fix.
  CHECK(std::strstr(entity("tof_distance", kLocal)->valueTemplate, "| min") != nullptr, "ToF nearest");
  for (const char* id : {"gps_latitude", "gps_longitude", "gps_altitude", "gps_speed"}) {
    CHECK(std::strstr(entity(id, kLocal)->valueTemplate, "value_json.gps.fix") != nullptr,
          "%s needs a fix", id);
  }
  CHECK(std::strstr(entity("gps_satellites", kLocal)->valueTemplate, "value_json.gps.fix") == nullptr,
        "satellites are shown without a fix");
  // Binary sensors render the default payloads, not Python's True/False.
  for (const char* id : {"presence_detected", "motion_detected"}) {
    const HaEntity* e = entity(id, kLocal);
    CHECK(std::strcmp(e->component, "binary_sensor") == 0 &&
              std::strstr(e->valueTemplate, "'ON'") && std::strstr(e->valueTemplate, "'OFF'"),
          "%s renders ON/OFF", id);
  }
}

static void test_units_and_classes() {
  struct Want {
    const char* id;
    const char* unit;
    const char* deviceClass;
  } wants[] = {
      {"gps_speed", "kn", "speed"},          // knots at the source; HA converts
      {"imu_gyro_x", "rad/s", nullptr},      // getEvent converts to rad/s
      {"imu_gyro_y", "rad/s", nullptr},
      {"imu_gyro_z", "rad/s", nullptr},
      {"imu_accel_x", "m/s²", nullptr},
      {"thermal_min", "°C", "temperature"},
      {"presence_ambient", "°C", "temperature"},
      {"rtc_temperature", "°C", "temperature"},
      {"rtc_datetime", nullptr, "timestamp"},
      {"tof_distance", "mm", "distance"},
      {"heap_free", "B", nullptr},
      {"heap_min", "B", nullptr},
      {"psram_free", "B", nullptr},
      {"uptime", "s", "duration"},
  };
  for (const Want& w : wants) {
    const HaEntity* e = entity(w.id, kLocal);
    CHECK(e != nullptr, "%s exists", w.id);
    if (!e) continue;
    CHECK(str(e->unit) == str(w.unit), "%s unit %s, want %s", w.id, str(e->unit).c_str(), str(w.unit).c_str());
    CHECK(str(e->deviceClass) == str(w.deviceClass), "%s class %s, want %s", w.id,
          str(e->deviceClass).c_str(), str(w.deviceClass).c_str());
  }
  const HaEntity* last = entity("last_seen", kPeer);
  CHECK(last && str(last->unit) == "s" && str(last->deviceClass) == "duration", "peer last_seen");
}

static void test_device_block() {
  const HaEntity& heap = *entity("heap_free", kLocal);
  DeviceInfo dev;
  dev.id = "hardwareone_0123456789AB";
  dev.name = "Bench Board";
  dev.model = "Seeed XIAO ESP32S3";
  dev.swVersion = "0.99.99.1";
  dev.area = "Lab";
  JsonDocument doc;
  buildDiscovery(heap, "hardwareone_0123456789AB_heap_free", "hw1/bench/state",
                 "hw1/bench/availability", dev, doc);
  char text[kDiscoveryCap];
  const size_t n = serializeBounded(doc, text, sizeof(text));
  // Byte-exact: the old layout, the model from the board, sw_version added.
  const std::string want =
      "{\"name\":\"Heap Free\",\"unique_id\":\"hardwareone_0123456789AB_heap_free\","
      "\"state_topic\":\"hw1/bench/state\",\"value_template\":\"{{ value_json.system.heap_free }}\","
      "\"unit_of_measurement\":\"B\",\"icon\":\"mdi:memory\",\"availability_topic\":\"hw1/bench/availability\","
      "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
      "\"device\":{\"identifiers\":[\"hardwareone_0123456789AB\"],\"name\":\"Bench Board\","
      "\"model\":\"Seeed XIAO ESP32S3\",\"sw_version\":\"0.99.99.1\",\"manufacturer\":\"Custom\","
      "\"suggested_area\":\"Lab\"}}";
  CHECK(n == want.size() && want == text, "local discovery document:\n got %s\nwant %s", text, want.c_str());
  CHECK(std::strstr(text, "ESP32-S3") == nullptr, "no hard-coded ESP32-S3");

  // A peer: no model, no sw_version (the master does not know them), via the master.
  DeviceInfo peer;
  peer.id = "hardwareone_A1B2C3D4E5F6";
  const uint8_t mac[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
  char fallback[devicename::kFieldLen];
  peer.name = peerDisplayName("", "", mac, fallback);
  peer.viaDevice = "hardwareone_0123456789AB";
  const HaEntity& tof = *entity("tof_distance", kPeer);
  buildDiscovery(tof, "hardwareone_A1B2C3D4E5F6_tof_distance", "hw1/devices/x/state",
                 "hw1/devices/x/availability", peer, doc);
  JsonDocument back;
  CHECK(serializeBounded(doc, text, sizeof(text)) > 0 && !deserializeJson(back, text), "peer doc");
  CHECK(!back["device"]["model"].is<const char*>() && !back["device"]["sw_version"].is<const char*>(),
        "peer has no model or sw_version");
  char expectedLabel[devicename::kFieldLen];
  devicename::formatWithMacSuffix(devicename::kPeerPrefix, mac, expectedLabel);
  CHECK(std::string(back["device"]["name"] | "") == expectedLabel, "unnamed peer: %s",
        back["device"]["name"].as<const char*>());
  CHECK(std::strncmp(expectedLabel, "peer-", 5) == 0 && std::strstr(expectedLabel, "A1B2C3D4") == nullptr,
        "the unknown-peer label is not the full address: %s", expectedLabel);
  CHECK(std::string(back["device"]["via_device"] | "") == "hardwareone_0123456789AB", "via_device");
  CHECK(std::string(peerDisplayName("Porch", "porch-node", mac, fallback)) == "Porch", "friendly first");
  CHECK(std::string(peerDisplayName("", "porch-node", mac, fallback)) == "porch-node", "then name");
}

static void test_times_are_utc_with_z() {
  char out[21];
  CHECK(formatUtcEpoch(0, out) == 20 && std::string(out) == "1970-01-01T00:00:00Z", "epoch 0: %s", out);
  CHECK(formatUtcEpoch(1795194249, out) == 20 && std::string(out) == "2026-11-20T17:04:09Z", "2026: %s", out);
  CHECK(formatUtcEpoch(4102444799LL, out) == 20 && std::string(out) == "2099-12-31T23:59:59Z", "2099: %s", out);
  CHECK(formatUtcEpoch(951782400, out) == 20 && std::string(out) == "2000-02-29T00:00:00Z", "leap: %s", out);
  CHECK(formatUtcEpoch(-1, out) == 0 && out[0] == '\0', "negative refused");
  CHECK(formatUtcFields(2026, 2, 29, 0, 0, 0, out) == 0, "2026-02-29 refused");
  CHECK(formatUtcFields(2028, 2, 29, 23, 59, 59, out) == 20, "2028-02-29 accepted");
  CHECK(formatUtcFields(2000, 1, 1, 0, 0, 0, out) == 0, "unset DS3231 (2000) refused");
  CHECK(formatUtcFields(2026, 13, 1, 0, 0, 0, out) == 0, "month 13 refused");

  JsonDocument doc;
  fullState(doc, false);
  const std::string time = doc["time"] | "";
  const std::string rtc = doc["rtc"]["datetime"] | "";
  CHECK(time == "2026-11-20T17:04:09Z", "state time: %s", time.c_str());
  CHECK(rtc == "2026-11-20T17:04:09Z", "rtc datetime from the same reading: %s", rtc.c_str());
  CHECK(doc["rtc"]["year"].as<int>() == 2026, "rtc keeps the builder fields");

  // Not valid, or not plausible: no datetime (HA shows unknown), section kept.
  JsonDocument d2;
  const char notValid[] = R"({"valid":false,"connected":true,"ts":0,"year":0,"month":0,"day":0,"hour":0,"minute":0,"second":0,"temp":0})";
  addRtcSection(d2, notValid, sizeof(notValid) - 1);
  CHECK(d2["rtc"].is<JsonObject>() && !d2["rtc"]["datetime"].is<const char*>(), "invalid RTC: no datetime");
  JsonDocument d3;
  const char reset[] = R"({"valid":true,"connected":true,"ts":5,"year":2000,"month":1,"day":1,"hour":0,"minute":0,"second":3,"temp":21.0})";
  addRtcSection(d3, reset, sizeof(reset) - 1);
  CHECK(!d3["rtc"]["datetime"].is<const char*>(), "reset RTC: no datetime");

  StateInput in = plainInput();
  in.hasTime = false;
  JsonDocument d4;
  beginState(in, d4);
  CHECK(!d4["time"].is<const char*>(), "clock not set: no time");
  CHECK(d4["timestamp"].as<uint32_t>() == 4000000000u, "timestamp stays uptime ms");
}

static void test_bad_sections_dropped() {
  JsonDocument doc;
  beginState(plainInput(), doc);
  const char nan[] = R"({"valid":true,"connected":true,"ts":1,"min":nan,"avg":nan,"max":nan})";
  CHECK(!addSection(doc, "thermal", nan, sizeof(nan) - 1), "nan reading refused");
  CHECK(!doc["thermal"].is<JsonVariant>() || doc["thermal"].isNull(), "no thermal key left");
  const char cut[] = R"({"valid":true,"connected":true,"ts":1,"objects":[{"id":1,)";
  CHECK(!addSection(doc, "tof", cut, sizeof(cut) - 1), "cut reading refused");
  const char array[] = "[1,2]";
  CHECK(!addSection(doc, "imu", array, sizeof(array) - 1), "a non-object refused");
  CHECK(!addSection(doc, "imu", "", 0), "empty refused");
  CHECK(add(doc, "gps", kFxGpsNoFix), "a good section after bad ones");
  char text[kStateCap];
  const size_t n = serializeBounded(doc, text, sizeof(text));
  JsonDocument back;
  CHECK(n > 0 && !deserializeJson(back, text, n), "document stays valid");
  CHECK(!back["thermal"].is<JsonObject>() && !back["tof"].is<JsonObject>() && !back["imu"].is<JsonObject>(),
        "dropped sections absent");
  CHECK(back["gps"]["fix"].as<int>() == 0 && back["gps"]["sats"].as<int>() == 3, "gps kept");
}

static void test_state_keys() {
  JsonDocument doc;
  fullState(doc, false);
  CHECK(doc["system"]["psram_free"].as<uint32_t>() == 6123456 &&
            doc["system"]["psram_total"].as<uint32_t>() == 8388608,
        "PSRAM free/total");
  CHECK(std::string(doc["deviceName"] | "") == "HardwareOne-B2C3" && doc["deviceNameSet"].is<bool>() &&
            !doc["deviceNameSet"].as<bool>(),
        "deviceName (bare) and deviceNameSet");
  // Order of the top-level keys: timestamp first, as before.
  JsonObject root = doc.as<JsonObject>();
  std::vector<std::string> keys;
  for (JsonPair kv : root) keys.push_back(kv.key().c_str());
  const std::vector<std::string> want = {"timestamp", "time", "deviceName", "deviceNameSet", "system",
                                         "wifi", "thermal", "tof", "imu", "presence", "gps", "apds",
                                         "rtc", "input"};
  CHECK(keys == want, "top-level key order");

  StateInput in = plainInput();
  in.hasPsram = false;
  in.hasWifi = false;
  JsonDocument d2;
  beginState(in, d2);
  CHECK(!d2["system"]["psram_free"].is<uint32_t>() && !d2["wifi"].is<JsonObject>(), "no PSRAM, no Wi-Fi");
}

static void test_sizes() {
  // Worst case state: every section, four ToF targets, a 32-byte SSID of quotes.
  StateInput in = plainInput();
  in.ssid = "\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"";
  in.deviceName = "abcdefghijklmnopqrstuvwxyz01234";
  JsonDocument doc;
  beginState(in, doc);
  const char tof4[] = R"json({"valid":true,"connected":true,"ts":4294967295,"objects":[{"id":1,"distance_mm":65535,"status":255,"valid":true},{"id":2,"distance_mm":65535,"status":255,"valid":true},{"id":3,"distance_mm":65535,"status":255,"valid":true},{"id":4,"distance_mm":65535,"status":255,"valid":true}]})json";
  add(doc, "thermal", kFxThermal);
  add(doc, "tof", tof4);
  add(doc, "imu", kFxImu);
  add(doc, "presence", kFxPresence);
  add(doc, "gps", kFxGpsFix);
  add(doc, "apds", kFxApds);
  add(doc, "rtc", kFxRtc);
  add(doc, "input", kFxGpioEncoder);
  const size_t need = measureJson(doc);
  CHECK(need < 2048, "worst-case state %zu bytes, far below %zu", need, kStateCap);
  char small[64];
  CHECK(serializeBounded(doc, small, sizeof(small)) == 0 && small[0] == '\0', "too big: not cut, refused");

  // Worst-case discovery: longest board name, 47-byte friendly name, 31-byte
  // room, every entity, the default base topic ("hardwareone/<12 hex>") in a
  // peer's topics (the longer ones).
  DeviceInfo dev;
  dev.id = "hardwareone_0123456789AB";
  dev.name = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstu";
  dev.model = "Waveshare ESP32-P4-WIFI6-Touch-LCD-5";
  dev.swVersion = "0.99.99.1-dirty+20261120";
  dev.area = "abcdefghijklmnopqrstuvwxyzabcde";
  dev.viaDevice = "hardwareone_0123456789AB";
  size_t largest = 0;
  for (size_t i = 0; i < kEntityCount; ++i) {
    const HaEntity& e = kEntities[i];
    std::string unique = std::string(dev.id) + "_" + e.objectId;
    JsonDocument d;
    buildDiscovery(e, unique.c_str(),
                   "hardwareone/0123456789AB/devices/hardwareone_A1B2C3D4E5F6/state",
                   "hardwareone/0123456789AB/devices/hardwareone_A1B2C3D4E5F6/availability", dev, d);
    const size_t m = measureJson(d);
    if (m > largest) largest = m;
    CHECK(m < kDiscoveryCap, "%s discovery %zu bytes", e.objectId, m);
  }
  // At least 120 bytes left: a base topic about 60 characters longer than the
  // default still fits every config (it appears twice).
  CHECK(largest + 120 < kDiscoveryCap, "largest discovery config %zu bytes leaves room for the base topic",
        largest);
  std::printf("largest discovery config (worst-case names, default base topic): %zu bytes\n", largest);
  // A 400-character base topic does not fit and is refused, never cut.
  const std::string base(400, 't');
  JsonDocument d;
  buildDiscovery(*entity("uptime", kLocal), "x", (base + "/state").c_str(), (base + "/availability").c_str(),
                 dev, d);
  char text[kDiscoveryCap];
  CHECK(serializeBounded(d, text, sizeof(text)) == 0, "long base topic refused");
}

static void test_unique_ids_pinned() {
  std::set<std::string> local, peer;
  for (size_t i = 0; i < kEntityCount; ++i) {
    if (kEntities[i].scope & kLocal) CHECK(local.insert(kEntities[i].objectId).second, "dup %s", kEntities[i].objectId);
    if (kEntities[i].scope & kPeer) CHECK(peer.insert(kEntities[i].objectId).second, "dup %s", kEntities[i].objectId);
  }
  // Before U6 plus psram_free, input_pos, input_axis; minus presence_object.
  const std::set<std::string> wantLocal = {
      "uptime", "heap_free", "heap_min", "psram_free", "wifi_rssi", "wifi_ssid", "wifi_ip",
      "thermal_min", "thermal_max", "thermal_avg", "tof_distance", "imu_accel_x", "imu_accel_y",
      "imu_accel_z", "imu_gyro_x", "imu_gyro_y", "imu_gyro_z", "presence_detected", "motion_detected",
      "presence_ambient", "gps_latitude", "gps_longitude", "gps_altitude", "gps_speed",
      "gps_satellites", "apds_proximity", "apds_color_r", "apds_color_g", "apds_color_b",
      "rtc_datetime", "rtc_temperature", "gamepad_x", "gamepad_y", "gamepad_buttons", "input_pos",
      "input_axis"};
  // Before U6 minus uptime and thermal_*, plus last_seen.
  const std::set<std::string> wantPeer = {"last_seen", "tof_distance", "imu_accel_x", "imu_accel_y",
                                          "imu_accel_z", "gps_latitude", "gps_longitude"};
  CHECK(local == wantLocal, "local object ids changed");
  CHECK(peer == wantPeer, "peer object ids changed");
}

static void test_active_and_retired() {
  auto declared = [](uint8_t scope, uint16_t active) {
    std::set<std::string> out;
    for (size_t i = 0; i < kEntityCount; ++i)
      if (entityActive(kEntities[i], scope, active)) out.insert(kEntities[i].objectId);
    return out;
  };
  auto retired = [](uint8_t scope, uint16_t compiled) {
    std::set<std::string> out;
    for (size_t i = 0; i < kRetiredCount; ++i)
      if (retiredApplies(kRetired[i], scope, compiled)) out.insert(kRetired[i].objectId);
    return out;
  };
  // PSRAM entity only with PSRAM.
  CHECK(declared(kLocal, kSystem).count("psram_free") == 0, "no PSRAM: no psram_free");
  CHECK(declared(kLocal, kSystem | kPsram).count("psram_free") == 1, "PSRAM: psram_free");
  // Gamepad build: x / y / buttons, nothing retired for input.
  const uint16_t pad = kSystem | kInputPad | kInputLegacy;
  CHECK(declared(kLocal, pad).count("gamepad_x") && declared(kLocal, pad).count("gamepad_buttons") &&
            !declared(kLocal, pad).count("input_pos"),
        "gamepad build rows");
  CHECK(retired(kLocal, pad).empty(), "gamepad build retires nothing");
  // Encoder build (the P4X home node's GPIO encoder): pos / axis / buttons; x / y retired.
  const uint16_t enc = kSystem | kInputEncoder | kInputLegacy;
  CHECK(declared(kLocal, enc).count("input_pos") && declared(kLocal, enc).count("gamepad_buttons") &&
            !declared(kLocal, enc).count("gamepad_x"),
        "encoder build rows");
  CHECK(retired(kLocal, enc) == std::set<std::string>({"gamepad_x", "gamepad_y"}), "encoder retires x/y");
  // Touch-only build: no input builder, all three retired.
  CHECK(retired(kLocal, kSystem | kInputLegacy) ==
            std::set<std::string>({"gamepad_x", "gamepad_y", "gamepad_buttons"}),
        "touch build retires the input rows");
  CHECK(retired(kLocal, kSystem | kPresence) == std::set<std::string>({"presence_object"}),
        "presence build retires presence_object");
  CHECK(retired(kPeer, kSystem | kThermal) ==
            std::set<std::string>({"uptime", "thermal_min", "thermal_max", "thermal_avg"}),
        "peers retire uptime and thermal");
  CHECK(declared(kPeer, kSystem | kTof) == std::set<std::string>({"last_seen", "tof_distance"}),
        "peer rows follow the sensor mask");
}

int main() {
  test_ssid_and_names_escape();
  test_templates_read_sent_keys();
  test_units_and_classes();
  test_device_block();
  test_times_are_utc_with_z();
  test_bad_sections_dropped();
  test_state_keys();
  test_sizes();
  test_unique_ids_pinned();
  test_active_and_retired();
  if (gFailures) {
    std::printf("%d of %d checks failed\n", gFailures, gChecks);
    return 1;
  }
  std::printf("mqtt payload core: %d checks pass\n", gChecks);
  return 0;
}
