// System_MQTTPayloadCore.h - what the MQTT client sends to Home Assistant
//
// The state document (<base>/state) and the Home Assistant discovery configs
// (<prefix>/<component>/<device id>/<object id>/config), built with ArduinoJson
// so free text (Wi-Fi SSID, device name, room, peer names) is always escaped.
// System_MQTT.cpp gathers the readings and owns the client; this header only
// turns plain values and the sensor builders' JSON text into documents.
//
// Pure on purpose: ArduinoJson, System_SensorJsonKeys.h and System_DeviceName.h
// only, no Arduino, no ESP-IDF, no #if on build features, no statics. The
// tables are constexpr (flash). The host test (test_mqtt_payload_core.cpp)
// compiles this file as it is.
//
// Every Home Assistant value template reads a key a sensor builder really
// sends (<x>BuildDataJson). Keys that another device also reads by name come
// from System_SensorJsonKeys.h (HW1_SK_*); the rest are spelled here once and
// checked against the builders' format strings by test_mqtt_payload_sources.py.
#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "System_DeviceName.h"
#include "System_SensorJsonKeys.h"

namespace mqttpayload {

// The two document ceilings, the buffers System_MQTT.cpp keeps for them. A
// document that does not fit is not sent (never cut).
constexpr size_t kStateCap = 16384;
constexpr size_t kDiscoveryCap = 1024;

// The state document's Wi-Fi link section {ssid, rssi, ip}. It is a reading
// of the current link, not the network.wifi settings section (owned by
// hw1wifijson), so it is named here rather than spelled at its use.
inline constexpr char kStateWifi[] = "wifi";

// Sections of the state document. One bit each; also what a discovery row
// needs compiled (and switched on) before it is declared.
enum Section : uint16_t {
  kSystem = 1u << 0,
  kPsram = 1u << 1,         // the board has PSRAM (system.psram_*)
  kWifi = 1u << 2,
  kThermal = 1u << 3,
  kTof = 1u << 4,
  kImu = 1u << 5,
  kPresence = 1u << 6,
  kGps = 1u << 7,
  kApds = 1u << 8,
  kRtc = 1u << 9,
  kInputPad = 1u << 10,      // gamepad builder: x / y / buttons
  kInputEncoder = 1u << 11,  // ANO or GPIO encoder builder: pos / axis / buttons
  kInputLegacy = 1u << 12,   // any input device (the old gamepad.* rows' gate)
};

// Which device a discovery row belongs to: this device, or a mesh peer the
// master bridges (state topic <base>/devices/<peer id>/state).
enum Scope : uint8_t { kLocal = 1u << 0, kPeer = 1u << 1, kBoth = kLocal | kPeer };

// One Home Assistant entity. `field` is the path in the state document the
// value comes from ("gps.speed", "tof.objects[0].distance_mm"); the host test
// resolves it, and every "value_json.<path>" in the template, in a full
// document built from the builders' text.
struct HaEntity {
  uint16_t section;
  uint8_t scope;
  const char* component;  // "sensor" | "binary_sensor"
  const char* objectId;   // unique_id suffix: never renamed without a retirement row
  const char* name;
  const char* field;
  const char* valueTemplate;
  const char* unit;         // nullptr = none
  const char* deviceClass;  // nullptr = none
  const char* icon;         // nullptr = none
};

// Template pieces. A sensor section carries the shared envelope ("valid"); a
// value is shown only while the section is present and valid, else HA gets
// "None", which an MQTT sensor shows as unknown.
#define MQP_ON(sec) "value_json." sec " is defined and value_json." sec ".valid"
#define MQP_VALUE(sec, path) "{{ value_json." sec "." path " if " MQP_ON(sec) " else none }}"
#define MQP_BINARY(sec, key) \
  "{{ ('ON' if value_json." sec "." key " else 'OFF') if " MQP_ON(sec) " else none }}"
// GPS position, height and speed only with a fix: the builder sends zeros
// without one.
#define MQP_GPS_FIX(path) \
  "{{ value_json.gps." path " if " MQP_ON("gps") " and value_json.gps." HW1_SK_FIX " else none }}"

// Every entity this firmware declares. Rows marked kPeer are also declared for
// each mesh peer that has the sensor (System_MQTT.cpp maps the section to the
// peer's sensor bit); peer state documents carry the remote builders' text
// under the same keys.
inline constexpr HaEntity kEntities[] = {
  // System. Values that are always present: the templates stay as they were.
  {kSystem, kLocal, "sensor", "uptime", "Uptime", "system.uptime",
   "{{ value_json.system.uptime }}", "s", "duration", "mdi:timer-outline"},
  {kSystem, kLocal, "sensor", "heap_free", "Heap Free", "system.heap_free",
   "{{ value_json.system.heap_free }}", "B", nullptr, "mdi:memory"},
  {kSystem, kLocal, "sensor", "heap_min", "Heap Min", "system.heap_min",
   "{{ value_json.system.heap_min }}", "B", nullptr, "mdi:memory"},
  // kPsram is active only when the board has PSRAM and system publishing is on.
  {kPsram, kLocal, "sensor", "psram_free", "PSRAM Free", "system.psram_free",
   "{{ value_json.system.psram_free }}", "B", nullptr, "mdi:memory"},
  // A peer's state carries no uptime, only how long ago it was heard.
  {kSystem, kPeer, "sensor", "last_seen", "Last Seen", "system.last_seen",
   "{{ value_json.system.last_seen }}", "s", "duration", "mdi:clock-outline"},

  // Wi-Fi.
  {kWifi, kLocal, "sensor", "wifi_rssi", "WiFi RSSI", "wifi.rssi",
   "{{ value_json.wifi.rssi }}", "dBm", "signal_strength", nullptr},
  {kWifi, kLocal, "sensor", "wifi_ssid", "WiFi SSID", "wifi.ssid",
   "{{ value_json.wifi.ssid }}", nullptr, nullptr, "mdi:wifi"},
  {kWifi, kLocal, "sensor", "wifi_ip", "WiFi IP", "wifi.ip",
   "{{ value_json.wifi.ip }}", nullptr, nullptr, "mdi:ip-network"},

  // Thermal camera: thermalBuildSummaryJson (raw-frame min / avg / max, °C).
  {kThermal, kLocal, "sensor", "thermal_min", "Thermal Min", "thermal.min",
   MQP_VALUE("thermal", "min"), "°C", "temperature", nullptr},
  {kThermal, kLocal, "sensor", "thermal_max", "Thermal Max", "thermal.max",
   MQP_VALUE("thermal", "max"), "°C", "temperature", nullptr},
  {kThermal, kLocal, "sensor", "thermal_avg", "Thermal Avg", "thermal.avg",
   MQP_VALUE("thermal", "avg"), "°C", "temperature", nullptr},

  // Time of flight: tofBuildDataJson lists detected targets only; the entity
  // is the nearest one, unknown while nothing is detected.
  {kTof, kBoth, "sensor", "tof_distance", "ToF Distance",
   "tof." HW1_SK_OBJECTS "[0]." HW1_SK_DISTANCE_MM,
   "{{ (value_json.tof." HW1_SK_OBJECTS " | map(attribute='" HW1_SK_DISTANCE_MM "') | min) if "
   MQP_ON("tof") " and value_json.tof." HW1_SK_OBJECTS " else none }}",
   "mm", "distance", nullptr},

  // IMU: imuBuildDataJson. Acceleration in m/s², rotation rate in rad/s (the
  // driver's getEvent converts the gyro to rad/s).
  {kImu, kBoth, "sensor", "imu_accel_x", "IMU Accel X", "imu.accel.x",
   MQP_VALUE("imu", "accel.x"), "m/s²", nullptr, "mdi:axis-x-arrow"},
  {kImu, kBoth, "sensor", "imu_accel_y", "IMU Accel Y", "imu.accel.y",
   MQP_VALUE("imu", "accel.y"), "m/s²", nullptr, "mdi:axis-y-arrow"},
  {kImu, kBoth, "sensor", "imu_accel_z", "IMU Accel Z", "imu.accel.z",
   MQP_VALUE("imu", "accel.z"), "m/s²", nullptr, "mdi:axis-z-arrow"},
  {kImu, kLocal, "sensor", "imu_gyro_x", "IMU Gyro X", "imu.gyro.x",
   MQP_VALUE("imu", "gyro.x"), "rad/s", nullptr, "mdi:rotate-3d-variant"},
  {kImu, kLocal, "sensor", "imu_gyro_y", "IMU Gyro Y", "imu.gyro.y",
   MQP_VALUE("imu", "gyro.y"), "rad/s", nullptr, "mdi:rotate-3d-variant"},
  {kImu, kLocal, "sensor", "imu_gyro_z", "IMU Gyro Z", "imu.gyro.z",
   MQP_VALUE("imu", "gyro.z"), "rad/s", nullptr, "mdi:rotate-3d-variant"},

  // Presence: presenceBuildDataJson. Binary sensors render ON / OFF, the
  // payloads an MQTT binary_sensor expects by default.
  {kPresence, kLocal, "binary_sensor", "presence_detected", "Presence Detected",
   "presence.presenceDetected", MQP_BINARY("presence", "presenceDetected"), nullptr,
   "presence", nullptr},
  {kPresence, kLocal, "binary_sensor", "motion_detected", "Motion Detected",
   "presence.motionDetected", MQP_BINARY("presence", "motionDetected"), nullptr, "motion",
   nullptr},
  {kPresence, kLocal, "sensor", "presence_ambient", "Presence Ambient Temp", "presence.ambient",
   MQP_VALUE("presence", "ambient"), "°C", "temperature", nullptr},

  // GPS: gpsBuildDataJson. Speed is knots at the source (PA1010D RMC); "kn"
  // is a unit of HA's speed device class, so HA converts it for display.
  {kGps, kBoth, "sensor", "gps_latitude", "GPS Latitude", "gps." HW1_SK_LAT,
   MQP_GPS_FIX(HW1_SK_LAT), "°", nullptr, "mdi:crosshairs-gps"},
  {kGps, kBoth, "sensor", "gps_longitude", "GPS Longitude", "gps." HW1_SK_LON,
   MQP_GPS_FIX(HW1_SK_LON), "°", nullptr, "mdi:crosshairs-gps"},
  {kGps, kLocal, "sensor", "gps_altitude", "GPS Altitude", "gps." HW1_SK_ALT,
   MQP_GPS_FIX(HW1_SK_ALT), "m", nullptr, "mdi:altimeter"},
  {kGps, kLocal, "sensor", "gps_speed", "GPS Speed", "gps." HW1_SK_SPEED,
   MQP_GPS_FIX(HW1_SK_SPEED), "kn", "speed", "mdi:speedometer"},
  {kGps, kLocal, "sensor", "gps_satellites", "GPS Satellites", "gps." HW1_SK_SATS,
   MQP_VALUE("gps", HW1_SK_SATS), nullptr, nullptr, "mdi:satellite-variant"},

  // APDS9960: apdsBuildDataJson (flat r / g / b / c / proximity).
  {kApds, kLocal, "sensor", "apds_proximity", "Proximity", "apds.proximity",
   MQP_VALUE("apds", "proximity"), nullptr, nullptr, "mdi:hand-wave"},
  {kApds, kLocal, "sensor", "apds_color_r", "Color Red", "apds.r", MQP_VALUE("apds", "r"),
   nullptr, nullptr, "mdi:palette"},
  {kApds, kLocal, "sensor", "apds_color_g", "Color Green", "apds.g", MQP_VALUE("apds", "g"),
   nullptr, nullptr, "mdi:palette"},
  {kApds, kLocal, "sensor", "apds_color_b", "Color Blue", "apds.b", MQP_VALUE("apds", "b"),
   nullptr, nullptr, "mdi:palette"},

  // RTC: rtcBuildDataJson plus "datetime", which addRtcSection makes from the
  // same reading's fields (the DS3231 holds UTC).
  {kRtc, kLocal, "sensor", "rtc_datetime", "RTC DateTime", "rtc.datetime",
   "{{ value_json.rtc.datetime if value_json.rtc is defined and value_json.rtc.datetime is "
   "defined else none }}",
   nullptr, "timestamp", "mdi:clock-outline"},
  {kRtc, kLocal, "sensor", "rtc_temperature", "RTC Temperature", "rtc.temp",
   MQP_VALUE("rtc", "temp"), "°C", "temperature", nullptr},

  // Input: the build's input builder under "input" (the same key the mesh
  // peers' documents use). Buttons are the builder's bits, as on every other
  // surface. The gamepad_* object ids are kept so HA keeps their history.
  {kInputPad, kLocal, "sensor", "gamepad_x", "Input X", "input." HW1_SK_X,
   MQP_VALUE("input", HW1_SK_X), nullptr, nullptr, "mdi:gamepad-variant"},
  {kInputPad, kLocal, "sensor", "gamepad_y", "Input Y", "input." HW1_SK_Y,
   MQP_VALUE("input", HW1_SK_Y), nullptr, nullptr, "mdi:gamepad-variant"},
  {kInputEncoder, kLocal, "sensor", "input_pos", "Input Position", "input." HW1_SK_POS,
   MQP_VALUE("input", HW1_SK_POS), nullptr, nullptr, "mdi:knob"},
  {kInputEncoder, kLocal, "sensor", "input_axis", "Input Axis", "input." HW1_SK_AXIS,
   MQP_VALUE("input", HW1_SK_AXIS), nullptr, nullptr, "mdi:axis-arrow"},
  {kInputPad | kInputEncoder, kLocal, "sensor", "gamepad_buttons", "Input Buttons",
   "input." HW1_SK_BUTTONS, MQP_VALUE("input", HW1_SK_BUTTONS), nullptr, nullptr, "mdi:gamepad"},
};
inline constexpr size_t kEntityCount = sizeof(kEntities) / sizeof(kEntities[0]);

#undef MQP_GPS_FIX
#undef MQP_BINARY
#undef MQP_VALUE
#undef MQP_ON

// An entity earlier firmware declared and this one does not (on a build that
// has `section`). Its retained config is cleared on connect (an empty
// retained payload), so Home Assistant removes it instead of keeping an entity
// that never updates.
struct RetiredEntity {
  uint16_t section;
  uint8_t scope;
  const char* component;
  const char* objectId;
};

inline constexpr RetiredEntity kRetired[] = {
  // Raw register counts sent as °C; no builder sends them.
  {kPresence, kLocal, "sensor", "presence_object"},
  // Builds whose input device is not the gamepad (encoders, touch) declared
  // these from getters that have no meaning there.
  {kInputLegacy, kLocal, "sensor", "gamepad_x"},
  {kInputLegacy, kLocal, "sensor", "gamepad_y"},
  {kInputLegacy, kLocal, "sensor", "gamepad_buttons"},
  // A peer's document never carried an uptime; replaced by last_seen.
  {kSystem, kPeer, "sensor", "uptime"},
  // The mesh does not carry thermal readings, so these never updated.
  {kThermal, kPeer, "sensor", "thermal_min"},
  {kThermal, kPeer, "sensor", "thermal_max"},
  {kThermal, kPeer, "sensor", "thermal_avg"},
};
inline constexpr size_t kRetiredCount = sizeof(kRetired) / sizeof(kRetired[0]);

// Whether `e` is declared in `scope` when the sections in `active` are there
// (compiled, and switched on for publishing). A row with several section bits
// (gamepad_buttons) needs any one of them.
inline bool entityActive(const HaEntity& e, uint8_t scope, uint16_t active) {
  return (e.scope & scope) != 0 && (e.section & active) != 0;
}

// Whether retiring `r` applies: the build has its section and no active row
// of the same scope still uses the object id.
inline bool retiredApplies(const RetiredEntity& r, uint8_t scope, uint16_t compiled) {
  if ((r.scope & scope) == 0 || (r.section & compiled) == 0) return false;
  for (size_t i = 0; i < kEntityCount; ++i) {
    if (entityActive(kEntities[i], scope, compiled) &&
        strcmp(kEntities[i].objectId, r.objectId) == 0)
      return false;
  }
  return true;
}

// Copies `in` to `out` (at most cap - 1 bytes and a NUL) with every byte that
// is not part of well-formed UTF-8 replaced by '?', and never cuts a
// character. A Wi-Fi SSID is any 32 bytes; Home Assistant refuses a whole
// document that is not UTF-8. Returns the bytes written.
inline size_t copyUtf8(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t o = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(in ? in : "");
  while (*p) {
    const unsigned c = *p;
    size_t more = 0;
    bool ok = true;
    if (c < 0x80) more = 0;
    else if (c >= 0xC2 && c <= 0xDF) more = 1;
    else if (c >= 0xE0 && c <= 0xEF) more = 2;
    else if (c >= 0xF0 && c <= 0xF4) more = 3;
    else ok = false;
    for (size_t k = 1; ok && k <= more; ++k) ok = (p[k] & 0xC0) == 0x80;
    if (ok && more == 2) {
      if (c == 0xE0 && p[1] < 0xA0) ok = false;   // overlong
      if (c == 0xED && p[1] >= 0xA0) ok = false;  // surrogate
    }
    if (ok && more == 3) {
      if (c == 0xF0 && p[1] < 0x90) ok = false;   // overlong
      if (c == 0xF4 && p[1] >= 0x90) ok = false;  // above U+10FFFF
    }
    const size_t take = ok ? more + 1 : 1;
    if (o + take + 1 > cap) break;
    if (ok) {
      memcpy(out + o, p, take);
    } else {
      out[o] = '?';
    }
    o += take;
    p += take;
  }
  out[o] = '\0';
  return o;
}

// "YYYY-MM-DDTHH:MM:SSZ" (20 bytes) for a UTC calendar time, or 0 and "" when
// a field is out of range. Years before 2020 are refused: a clock that reads
// earlier has not been set (the DS3231 starts at 2000-01-01).
inline size_t formatUtcFields(unsigned year, unsigned month, unsigned day, unsigned hour,
                              unsigned minute, unsigned second, char (&out)[21]) {
  out[0] = '\0';
  static constexpr unsigned char kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 2020 || year > 2099 || month < 1 || month > 12 || hour > 23 || minute > 59 ||
      second > 59)
    return 0;
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  const unsigned monthDays = kDays[month - 1] + ((month == 2 && leap) ? 1u : 0u);
  if (day < 1 || day > monthDays) return 0;
  const int n = snprintf(out, sizeof(out), "%04u-%02u-%02uT%02u:%02u:%02uZ", year, month, day,
                         hour, minute, second);
  if (n != 20) {
    out[0] = '\0';
    return 0;
  }
  return 20;
}

// The same text for seconds since 1970-01-01 UTC (the system clock).
inline size_t formatUtcEpoch(int64_t epoch, char (&out)[21]) {
  out[0] = '\0';
  if (epoch < 0) return 0;
  const int64_t days = epoch / 86400;
  const int64_t rem = epoch % 86400;
  // Civil date from a day count (H. Hinnant's days_from_civil, inverted).
  const int64_t z = days + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  if (y < 0 || y > 9999) return 0;
  const int n = snprintf(out, sizeof(out), "%04u-%02u-%02uT%02u:%02u:%02uZ", (unsigned)y,
                         (unsigned)m, (unsigned)d, (unsigned)(rem / 3600),
                         (unsigned)((rem / 60) % 60), (unsigned)(rem % 60));
  if (n != 20) {
    out[0] = '\0';
    return 0;
  }
  return 20;
}

// Plain values of the state document. Text is copied into the document.
struct StateInput {
  uint32_t uptimeMs = 0;       // "timestamp": milliseconds since start (as before)
  bool hasTime = false;        // the system clock is set
  int64_t epochUtc = 0;        // one read of it: "time"
  const char* deviceName = nullptr;  // the name in use (bare); "deviceName"
  bool deviceNameSet = false;        // a name is stored; "deviceNameSet"
  bool hasSystem = false;
  uint32_t uptimeS = 0, heapFree = 0, heapMin = 0;
  bool hasPsram = false;
  uint32_t psramFree = 0, psramTotal = 0;
  bool hasWifi = false;
  const char* ssid = nullptr;
  int rssi = 0;
  const char* ip = nullptr;
};

// The top of the state document: timestamp, time, deviceName, deviceNameSet,
// system, wifi. Clears `doc` first. Returns false when the document could not
// hold it (allocation failure).
inline bool beginState(const StateInput& in, JsonDocument& doc) {
  doc.clear();
  doc["timestamp"] = in.uptimeMs;
  char iso[21];
  if (in.hasTime && formatUtcEpoch(in.epochUtc, iso)) doc["time"] = iso;
  if (in.deviceName && in.deviceName[0]) {
    char name[devicename::kFieldLen * 2];
    copyUtf8(in.deviceName, name, sizeof(name));
    doc["deviceName"] = name;
    doc["deviceNameSet"] = in.deviceNameSet;
  }
  if (in.hasSystem) {
    JsonObject sys = doc["system"].to<JsonObject>();
    sys["uptime"] = in.uptimeS;
    sys["heap_free"] = in.heapFree;
    sys["heap_min"] = in.heapMin;
    if (in.hasPsram) {
      sys["psram_free"] = in.psramFree;
      sys["psram_total"] = in.psramTotal;
    }
  }
  if (in.hasWifi) {
    JsonObject wifi = doc[kStateWifi].to<JsonObject>();
    char ssid[32 + 1];  // an SSID is at most 32 bytes; copyUtf8 never grows text
    copyUtf8(in.ssid, ssid, sizeof(ssid));
    wifi["ssid"] = ssid;
    wifi["rssi"] = in.rssi;
    wifi["ip"] = in.ip ? in.ip : "";
  }
  return !doc.overflowed();
}

// One sensor builder's reading, `len` bytes of JSON text, as doc[key]. The
// text is copied. Text that is not one JSON object (a builder printed "nan",
// or ran out of room) leaves no `key` at all: one section is dropped, never
// the whole document. Returns whether the section is in the document.
inline bool addSection(JsonDocument& doc, const char* key, const char* text, size_t len) {
  if (!key || !text || len == 0) return false;
  JsonVariant slot = doc[key].to<JsonVariant>();
  const DeserializationError err = deserializeJson(slot, text, len);
  if (err || !slot.is<JsonObject>() || doc.overflowed()) {
    doc.remove(key);
    return false;
  }
  return true;
}

// The RTC section (rtcBuildDataJson) with "datetime" added from the same
// reading's fields, so date and time cannot come from two different reads.
// No "datetime" while the reading is not valid or not plausible.
inline bool addRtcSection(JsonDocument& doc, const char* text, size_t len) {
  if (!addSection(doc, "rtc", text, len)) return false;
  JsonObject rtc = doc["rtc"].as<JsonObject>();
  if (!rtc["valid"].as<bool>()) return true;
  char iso[21];
  if (formatUtcFields(rtc["year"].as<unsigned>(), rtc["month"].as<unsigned>(),
                      rtc["day"].as<unsigned>(), rtc["hour"].as<unsigned>(),
                      rtc["minute"].as<unsigned>(), rtc["second"].as<unsigned>(), iso))
    rtc["datetime"] = iso;
  return true;
}

// Serializes `doc` into `out` when it fits whole (text and NUL). Returns the
// text's length, or 0 (and "" in `out`) when it does not fit or the document
// overflowed.
inline size_t serializeBounded(const JsonDocument& doc, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (doc.overflowed()) return 0;
  const size_t need = measureJson(doc);
  if (need == 0 || need >= cap) return 0;
  const size_t n = serializeJson(doc, out, cap);
  if (n != need) {
    out[0] = '\0';
    return 0;
  }
  return n;
}

// The device block of a discovery config. Text fields are copied as UTF-8.
struct DeviceInfo {
  const char* id = nullptr;         // "hardwareone_<12 hex>"
  const char* name = nullptr;
  const char* model = nullptr;      // board name; nullptr for a peer (not known)
  const char* swVersion = nullptr;  // firmware version; nullptr for a peer
  const char* area = nullptr;       // room; nullptr or "" = none
  const char* viaDevice = nullptr;  // the master's id for a peer
};

// One discovery config, keys in the order the firmware has always sent them
// (plus "sw_version" after "model"). Clears `doc` first.
inline bool buildDiscovery(const HaEntity& e, const char* uniqueId, const char* stateTopic,
                           const char* availTopic, const DeviceInfo& dev, JsonDocument& doc) {
  doc.clear();
  doc["name"] = e.name;
  doc["unique_id"] = uniqueId;
  doc["state_topic"] = stateTopic;
  doc["value_template"] = e.valueTemplate;
  if (e.unit && e.unit[0]) doc["unit_of_measurement"] = e.unit;
  if (e.deviceClass && e.deviceClass[0]) doc["device_class"] = e.deviceClass;
  if (e.icon && e.icon[0]) doc["icon"] = e.icon;
  doc["availability_topic"] = availTopic;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"].to<JsonArray>().add(dev.id ? dev.id : "");
  char text[4 * devicename::kFieldLen];
  copyUtf8(dev.name, text, sizeof(text));
  device["name"] = text;
  if (dev.model && dev.model[0]) device["model"] = dev.model;
  if (dev.swVersion && dev.swVersion[0]) device["sw_version"] = dev.swVersion;
  device["manufacturer"] = "Custom";
  if (dev.viaDevice && dev.viaDevice[0]) device["via_device"] = dev.viaDevice;
  if (dev.area && dev.area[0]) {
    copyUtf8(dev.area, text, sizeof(text));
    device["suggested_area"] = text;
  }
  return !doc.overflowed();
}

// The HA device name of a mesh peer: its friendly name, else its device name,
// else the unknown-peer label ("peer-B2C3", System_DeviceName.h).
inline const char* peerDisplayName(const char* friendlyName, const char* name, const uint8_t* mac,
                                   char (&fallback)[devicename::kFieldLen]) {
  if (friendlyName && friendlyName[0]) return friendlyName;
  if (name && name[0]) return name;
  devicename::formatWithMacSuffix(devicename::kPeerPrefix, mac, fallback);
  return fallback;
}

}  // namespace mqttpayload
