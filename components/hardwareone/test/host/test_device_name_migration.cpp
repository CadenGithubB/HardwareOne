// The device name's once-only move from the names it replaced
// (devicename::pickLegacy in System_DeviceName.h; the settings.json keys and
// the file patch in System_DeviceNameLegacy.h), with the vendored ArduinoJson
// parsing settings files as the builds before and after the rename wrote them.
//
// Bench 2026-11-18: both boards came up with "Device name: (not set)" after the
// rename, so ESP-NOW refused to start. Each check below fails on the code
// before this fix, which read only system.device.identity.deviceName.
//
// The load is modelled as the firmware runs it: the registered read gives the
// setting the file's device name (or ""), then the move runs, then ESP-NOW's
// start gate (checkEspNowFirstTimeSetup) needs devicename::check() == None on
// the setting, which is also what Bluetooth and the hostname use
// (SelfDevice::deviceName). test_device_name_sources.py pins that the firmware
// calls these in this order.
#include "System_DeviceName.h"
#include "System_DeviceNameLegacy.h"

#include <cstdio>
#include <cstring>
#include <string>

static int gFailures = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
      gFailures++;                                     \
    }                                                  \
  } while (0)

using devicename::LegacySource;

struct Boot {
  std::string setting;          // gSettings.espnowDeviceName after the load
  devicename::LegacyPick pick;  // what the move took
  bool patchPending = false;    // the file patch is due
  bool startGateOpen = false;   // checkEspNowFirstTimeSetup would return ""
};

// readSettingsJson's order: registered read, then deviceNameMigrateLegacy.
static Boot boot(const char* json) {
  Boot b;
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    std::printf("FAIL fixture does not parse: %s\n", json);
    gFailures++;
    return b;
  }
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  b.setting = devicename::storedNameIn(root);  // the registered read (string or default "")
  if (devicename::hasLegacyKeys(root)) {
    b.patchPending = true;
    b.pick = devicename::pickLegacyFrom(root, b.setting.c_str());
    if (b.pick.source != LegacySource::None) b.setting = b.pick.name;
  }
  b.startGateOpen = devicename::check(b.setting.c_str()) == devicename::Fault::None;
  return b;
}

// The bench boards: a file from the build before the rename (ESP-NOW name in
// network.espnow.identity, the Bluetooth default beside it, no system.device).
static const char* kBeforeRename =
    R"({"firmwareVersion":"0.99.99.1","network":{"espnow":{"enabled":true,"identity":{"deviceName":"benchxiao","room":"lab"}},)"
    R"("bluetooth":{"bluetoothDeviceName":"HardwareOne","bleEnabled":true}}})";
// The same board after one save by 7f433e04: the merge kept the old key and the
// full save wrote the new one empty.
static const char* kSavedByRename =
    R"({"system":{"device":{"identity":{"deviceName":""}}},)"
    R"("network":{"espnow":{"identity":{"deviceName":"benchp4b"}},"bluetooth":{"bluetoothDeviceName":"HardwareOne"}}})";

static void testOldFileGivesTheNameBack() {
  Boot b = boot(kBeforeRename);
  CHECK(b.setting == "benchxiao", "M1 the old ESP-NOW name is the device name (got '%s')", b.setting.c_str());
  CHECK(b.pick.source == LegacySource::EspNow && !b.pick.converted, "M1 taken as it is, from the ESP-NOW name");
  CHECK(b.startGateOpen, "M1 ESP-NOW's start gate sees a valid name");
  CHECK(b.patchPending, "M1 the file patch is due");

  b = boot(kSavedByRename);
  CHECK(b.setting == "benchp4b" && b.startGateOpen,
        "M2 a file saved by the rename build (new key \"\") still gives the old name (got '%s')", b.setting.c_str());
}

static void testPatchIsOnceOnly() {
  JsonDocument doc;
  (void)deserializeJson(doc, kBeforeRename);
  const devicename::LegacyPick pick = devicename::pickLegacyFrom(doc.as<JsonVariantConst>(), "");
  CHECK(devicename::patchLegacy(doc, pick.name), "M3 the patch changes the file");
  std::string text;
  serializeJson(doc, text);
  JsonDocument again;
  (void)deserializeJson(again, text);
  const JsonVariantConst root = again.as<JsonVariantConst>();
  CHECK(std::strcmp(devicename::storedNameIn(root), "benchxiao") == 0, "M3 the new key holds the name");
  CHECK(!devicename::hasLegacyKeys(root), "M3 both old keys are gone");
  CHECK(root["network"]["espnow"]["identity"]["room"] == "lab" && root["network"]["espnow"]["enabled"] == true &&
            root["network"]["bluetooth"]["bleEnabled"] == true,
        "M3 keys beside the old ones stay");
  const Boot next = boot(text.c_str());
  CHECK(next.setting == "benchxiao" && !next.patchPending && next.startGateOpen,
        "M3 the next boot reads the new key and has nothing to move");
  CHECK(!devicename::patchLegacy(again, "other"), "M3 a patched file is not changed again");
}

static void testInvalidOldNamesConvert() {
  struct Case {
    const char* old;
    const char* want;
  };
  const Case cases[] = {
      {"Living_Room", "Living-Room"},       // the old rule allowed '_'
      {"_bench__p4_", "bench-p4"},          // runs collapse, ends trimmed
      {"kitchen cam 2", "kitchen-cam-2"},   // hand-edited file: spaces
      {"caf\xC3\xA9-1", "caf-1"},           // other bytes dropped
      {"abcdefghi_klmnopqrs", "abcdefghi-klmnopqrs"},    // 19 old bytes stay 19
  };
  for (const Case& c : cases) {
    const devicename::LegacyPick p = devicename::pickLegacy("", c.old, nullptr);
    CHECK(p.source == LegacySource::EspNow && p.converted && std::strcmp(p.name, c.want) == 0,
          "M4 '%s' converts to '%s' (got '%s')", c.old, c.want, p.name);
    CHECK(devicename::valid(p.name), "M4 the converted name passes the rule");
    const devicename::LegacyPick q = devicename::pickLegacy("", c.old, nullptr);
    CHECK(std::strcmp(p.name, q.name) == 0, "M4 the conversion is deterministic");
  }
  // A Bluetooth name longer than 31 bytes is cut.
  const std::string longName(40, 'b');
  const devicename::LegacyPick cut = devicename::pickLegacy("", nullptr, (longName + " x").c_str());
  CHECK(cut.source == LegacySource::Bluetooth && std::strlen(cut.name) == devicename::kMaxLen,
        "M4 a long old name is cut to 31 bytes");

  // Nothing valid left: the next old name is tried, else nothing is taken.
  devicename::LegacyPick p = devicename::pickLegacy("", "DEADBEEFCAFE", "Kitchen_BLE");
  CHECK(p.source == LegacySource::Bluetooth && std::strcmp(p.name, "Kitchen-BLE") == 0 &&
            p.unusable == LegacySource::EspNow,
        "M5 an ESP-NOW name that reads as an address falls to the Bluetooth name (got '%s')", p.name);
  p = devicename::pickLegacy("", "clear", nullptr);
  CHECK(p.source == LegacySource::None && p.name[0] == '\0' && p.unusable == LegacySource::EspNow,
        "M5 a reserved word gives no name");
  p = devicename::pickLegacy("", "___", nullptr);
  CHECK(p.source == LegacySource::None && p.unusable == LegacySource::EspNow, "M5 only separators give no name");
}

static void testBluetoothName() {
  devicename::LegacyPick p = devicename::pickLegacy("", "", "Kitchen-Speaker");
  CHECK(p.source == LegacySource::Bluetooth && std::strcmp(p.name, "Kitchen-Speaker") == 0,
        "M6 a chosen Bluetooth name is taken when there is no ESP-NOW name");
  p = devicename::pickLegacy("", "", "HardwareOne");
  CHECK(p.source == LegacySource::None && p.unusable == LegacySource::None,
        "M6 the old Bluetooth default is not a chosen name");
  p = devicename::pickLegacy("", "", "hardwareone");
  CHECK(p.source == LegacySource::None, "M6 the old default in any case");
  p = devicename::pickLegacy("", "benchxiao", "Kitchen-Speaker");
  CHECK(p.source == LegacySource::EspNow && std::strcmp(p.name, "benchxiao") == 0,
        "M6 the ESP-NOW name comes first");
  const Boot b = boot(R"({"network":{"espnow":{"identity":{"room":"lab"}},"bluetooth":{"bluetoothDeviceName":"HardwareOne"}}})");
  CHECK(b.setting.empty() && !b.startGateOpen && b.patchPending,
        "M6 a device that never had a name stays without one (the old key still leaves)");
}

static void testExistingNameNeverOverwritten() {
  Boot b = boot(R"({"system":{"device":{"identity":{"deviceName":"Desk"}}},)"
                R"("network":{"espnow":{"identity":{"deviceName":"benchxiao"}}}})");
  CHECK(b.setting == "Desk" && b.pick.source == LegacySource::None, "M7 a stored device name stays");
  CHECK(b.patchPending, "M7 the old key still leaves the file");
  // An invalid stored name is the user's too: the move does not replace it.
  b = boot(R"({"system":{"device":{"identity":{"deviceName":"bad_name"}}},)"
           R"("network":{"espnow":{"identity":{"deviceName":"benchxiao"}}}})");
  CHECK(b.setting == "bad_name" && b.pick.source == LegacySource::None, "M7 an invalid stored name is not replaced");

  JsonDocument doc;
  (void)deserializeJson(doc, R"({"system":{"device":{"identity":{"deviceName":"Desk"}}},)"
                             R"("network":{"espnow":{"identity":{"deviceName":"benchxiao"}}}})");
  CHECK(devicename::patchLegacy(doc, "benchxiao"), "M7 the patch removes the old key");
  CHECK(doc["system"]["device"]["identity"]["deviceName"] == "Desk", "M7 the patch keeps the stored name");

  // The full save's removal (after 'devicename clear', RAM has ""): the old
  // key goes and the cleared name stays cleared.
  JsonDocument cleared;
  (void)deserializeJson(cleared, kSavedByRename);
  CHECK(devicename::removeLegacyKeys(cleared) && !devicename::hasLegacyKeys(cleared.as<JsonVariantConst>()),
        "M8 a full save removes both old keys");
  CHECK(cleared["system"]["device"]["identity"]["deviceName"] == "", "M8 the removal leaves the device name alone");
  CHECK(!devicename::removeLegacyKeys(cleared), "M8 nothing to remove the second time");
}

static void testWrongTypes() {
  const Boot b = boot(R"({"network":{"espnow":{"identity":{"deviceName":42}},"bluetooth":{"bluetoothDeviceName":null}}})");
  CHECK(b.setting.empty() && b.pick.source == LegacySource::None, "M9 a non-string old value gives no name");
  CHECK(b.patchPending, "M9 a non-string old key still leaves the file");
  const Boot none = boot(R"({"network":{"espnow":{"enabled":true}}})");
  CHECK(!none.patchPending && none.setting.empty(), "M9 a file without old keys has nothing to do");
}

int main() {
  testOldFileGivesTheNameBack();
  testPatchIsOnceOnly();
  testInvalidOldNamesConvert();
  testBluetoothName();
  testExistingNameNeverOverwritten();
  testWrongTypes();
  if (gFailures) {
    std::printf("device_name_migration_tests: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("device_name_migration_tests: all passed\n");
  return 0;
}
