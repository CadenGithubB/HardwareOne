#ifndef SYSTEM_DEVICE_NAME_LEGACY_H
#define SYSTEM_DEVICE_NAME_LEGACY_H

// ============================================================================
// Where settings.json kept the names that the one device name replaced, and
// the once-only move of one of them into the device name (the rule for which
// one is devicename::pickLegacy in System_DeviceName.h).
//
//   device name (now):   system.device.identity.deviceName
//   ESP-NOW name (old):  network.espnow.identity.deviceName     ('espnow' module, group 'identity')
//   Bluetooth (old):     network.bluetooth.bluetoothDeviceName  ('bluetooth' module)
//
// The Wi-Fi hostname had no setting of its own, and neither name was kept in
// NVS or in the ESP-NOW device registry (its own entry is written from the
// setting at every start), so these two keys are the only old copies.
//
// A full settings save merges into the file it read, so keys no build reads
// stay in it; the old keys are therefore removed explicitly, by the boot's
// one-key patch after a move and by every full save. Left in place, a later
// 'devicename clear' would bring the old name back at the next boot.
//
// Only ArduinoJson (the host tests compile it with the vendored copy).
// ============================================================================

#include <ArduinoJson.h>

#include "System_DeviceName.h"

namespace devicename {

// For log lines.
constexpr const char* kStoredPath = "system.device.identity.deviceName";
constexpr const char* kLegacyEspNowPath = "network.espnow.identity.deviceName";
constexpr const char* kLegacyBluetoothPath = "network.bluetooth.bluetoothDeviceName";

// A string at the path, or "" (absent, or not a string).
inline const char* storedNameIn(JsonVariantConst root) {
  return root["system"]["device"]["identity"]["deviceName"] | "";
}
inline const char* legacyEspNowNameIn(JsonVariantConst root) {
  return root["network"]["espnow"]["identity"]["deviceName"] | "";
}
inline const char* legacyBluetoothNameIn(JsonVariantConst root) {
  return root["network"]["bluetooth"]["bluetoothDeviceName"] | "";
}

// Whether either old key is in the document (any type).
inline bool hasLegacyKeys(JsonVariantConst root) {
  return !root["network"]["espnow"]["identity"]["deviceName"].isNull() ||
         !root["network"]["bluetooth"]["bluetoothDeviceName"].isNull();
}

// The pick for a parsed settings file whose device name, as loaded, is
// `loaded` (the setting after the registered read; the file's own value when
// called on the file).
inline LegacyPick pickLegacyFrom(JsonVariantConst root, const char* loaded) {
  return pickLegacy(loaded, legacyEspNowNameIn(root), legacyBluetoothNameIn(root));
}

// Remove both old keys. Returns whether anything was removed.
inline bool removeLegacyKeys(JsonDocument& doc) {
  bool removed = false;
  JsonObject identity = doc["network"]["espnow"]["identity"].as<JsonObject>();
  if (!identity.isNull() && !identity["deviceName"].isNull()) {
    identity.remove("deviceName");
    removed = true;
  }
  JsonObject bluetooth = doc["network"]["bluetooth"].as<JsonObject>();
  if (!bluetooth.isNull() && !bluetooth["bluetoothDeviceName"].isNull()) {
    bluetooth.remove("bluetoothDeviceName");
    removed = true;
  }
  return removed;
}

// The boot's one-key patch of the parsed file: store `moved` as the device
// name when the file still has none (a non-empty name in the file is never
// replaced), then remove the old keys. `moved` null or "" only removes them.
// Returns whether the document changed.
inline bool patchLegacy(JsonDocument& doc, const char* moved) {
  bool changed = false;
  if (moved && moved[0] && !storedNameIn(doc.as<JsonVariantConst>())[0]) {
    doc["system"]["device"]["identity"]["deviceName"] = moved;
    changed = true;
  }
  if (removeLegacyKeys(doc)) changed = true;
  return changed;
}

}  // namespace devicename

#endif  // SYSTEM_DEVICE_NAME_LEGACY_H
