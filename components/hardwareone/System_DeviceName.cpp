// System_DeviceName.cpp - the one device-name setter ('devicename') and its
// settings row, compiled in every build.
//
// One stored name (gSettings.espnowDeviceName) is this device's ESP-NOW name,
// its Bluetooth (GAP) name and its Wi-Fi hostname. System_DeviceName.h holds
// the rule; this file is the only place that hands a new name on
// (deviceNameApply: 'devicename' and the setup wizards' save). ESP-NOW's own part
// (its registry entry, the metadata push, the refusal to clear a name it is
// running under) is in System_ESPNow.cpp behind espnowDeviceNameApplied /
// espnowDeviceNameClearRefusal.

#include "System_BuildConfig.h"
#include "System_DeviceName.h"
#include "System_DeviceNameLegacy.h"  // the old names in settings.json and their once-only move
#include "System_SelfDevice.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "System_Command.h"     // CommandArgs
#include "System_Debug.h"       // getDebugBuffer, ensureDebugBuffer
#include "System_Events.h"      // systemEventPost, SYSEVT_SETTING_CHANGED
#include "System_Settings.h"    // gSettings, setSetting, SettingEntry
#include "System_Utils.h"       // RETURN_VALID_IF_VALIDATE_CSTR
#include "System_ESPNow.h"      // espnowDeviceNameApplied, espnowDeviceNameClearRefusal, espnowNameInUseBy (ESPNOW builds)

void deviceNameApply(const String& name, char* warn, size_t warnCap) {
  if (warn && warnCap) warn[0] = '\0';
  // Raw setSetting bypasses handleSettingCommand, which is where
  // notifySettingChanged normally fires — post the parity notification here.
  systemEventPost(SYSEVT_SETTING_CHANGED, "deviceName", name.length() ? name.c_str() : "(cleared)");
#if ENABLE_ESPNOW
  espnowDeviceNameApplied(name);
#endif
  SelfDevice::applyHostname();
#if ENABLE_ESPNOW
  // Another device this one knows already has the name: it is set all the same.
  char who[56];
  if (warn && warnCap && name.length() && espnowNameInUseBy(name.c_str(), nullptr, who, sizeof(who))) {
    snprintf(warn, warnCap,
             " Warning: %s has the same name. Both are shown by address, and commands need the MAC "
             "to reach either.", who);
  }
#endif
}

// ----------------------------------------------------------------------------
// The once-only move from the names the device name replaced (the rule is
// devicename::pickLegacy; the keys are in System_DeviceNameLegacy.h). Runs from
// readSettingsJson after the registered read, before anything that uses the
// name has started (ESP-NOW's start gate, Bluetooth, the hostname), so all of
// them see the moved name on this boot. The file is patched by the boot's
// one-key rewrite (settingsRestampFirmwareVersion), never by a full save from
// RAM; a failed patch is retried at the next boot, where the same pick runs
// again and gives the same name.
// ----------------------------------------------------------------------------

static bool sLegacyPatchPending = false;
static char sLegacyMoved[devicename::kFieldLen] = {};

void deviceNameMigrateLegacy(const JsonDocument& doc) {
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!devicename::hasLegacyKeys(root)) return;
  sLegacyPatchPending = true;  // the old keys leave the file in every case
  const char* espnowOld = devicename::legacyEspNowNameIn(root);
  const char* bluetoothOld = devicename::legacyBluetoothNameIn(root);
  const devicename::LegacyPick pick =
      devicename::pickLegacy(gSettings.espnowDeviceName.c_str(), espnowOld, bluetoothOld);
  const char* unusableOld = pick.unusable == devicename::LegacySource::EspNow ? espnowOld : bluetoothOld;
  if (pick.source == devicename::LegacySource::None) {
    if (gSettings.espnowDeviceName.length() == 0 && pick.unusable != devicename::LegacySource::None) {
      logSystemEvent("SETTINGS",
                     "device name: the old %s \"%.47s\" gives no valid name and is dropped; "
                     "set one with 'devicename <name>'",
                     devicename::legacySourceText(pick.unusable), unusableOld);
    }
    return;
  }
  // Direct assignment: the load is still running (no persist request, no
  // change events; the patch above writes the file).
  gSettings.espnowDeviceName = pick.name;
  memcpy(sLegacyMoved, pick.name, sizeof(sLegacyMoved));
  const char* old = pick.source == devicename::LegacySource::EspNow ? espnowOld : bluetoothOld;
  char also[96] = "";
  if (pick.unusable != devicename::LegacySource::None) {
    snprintf(also, sizeof(also), "; the old %s \"%.31s\" gave no valid name",
             devicename::legacySourceText(pick.unusable), unusableOld);
  }
  if (pick.converted) {
    logSystemEvent("SETTINGS",
                   "device name \"%s\" taken from the old %s \"%.47s\", changed to fit the rule "
                   "(letters, digits and '-')%s",
                   pick.name, devicename::legacySourceText(pick.source), old, also);
  } else {
    logSystemEvent("SETTINGS", "device name \"%s\" taken from the old %s%s", pick.name,
                   devicename::legacySourceText(pick.source), also);
  }
}

bool deviceNameLegacyPatchPending() { return sLegacyPatchPending; }

bool deviceNameLegacyPatch(JsonDocument& doc) { return devicename::patchLegacy(doc, sLegacyMoved); }

void deviceNameLegacyPatchSaved() { sLegacyPatchPending = false; }

// devicename [<name>|clear]
const char* cmd_devicename(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    if (gSettings.espnowDeviceName.length() > 0 && devicename::valid(gSettings.espnowDeviceName.c_str())) {
      snprintf(getDebugBuffer(), 1024, "Device name: %s", gSettings.espnowDeviceName.c_str());
    } else {
      snprintf(getDebugBuffer(), 1024, "Device name: (not set). Bluetooth and Wi-Fi use %s.",
               SelfDevice::deviceName().c_str());
    }
    return getDebugBuffer();
  }
  // The name is one token; a second one would be dropped without a word.
  if (a.count() > 1) {
    snprintf(getDebugBuffer(), 1024, "Error: %s Use for example Living-Room.", devicename::kRuleText);
    return getDebugBuffer();
  }

  const String name = a.arg(0);
  if (name == "clear") {
#if ENABLE_ESPNOW
    if (const char* refused = espnowDeviceNameClearRefusal()) return refused;
#endif
    if (gSettings.espnowDeviceName.length() == 0) return "Device name: (not set)";
    setSetting(gSettings.espnowDeviceName, String(""));
    deviceNameApply(String(""), nullptr, 0);
    snprintf(getDebugBuffer(), 1024,
             "Device name cleared. Bluetooth and Wi-Fi use %s from their next start"
#if ENABLE_ESPNOW
             "; ESP-NOW will not start until a name is set with 'devicename <name>'"
#endif
             ".",
             SelfDevice::deviceName().c_str());
    return getDebugBuffer();
  }

  const devicename::Fault fault = devicename::check(name.c_str());
  if (fault != devicename::Fault::None) {
    snprintf(getDebugBuffer(), 1024, "Error: %s %s", devicename::faultText(fault), devicename::kRuleText);
    return getDebugBuffer();
  }

  setSetting(gSettings.espnowDeviceName, name);
  char warn[200];
  deviceNameApply(name, warn, sizeof(warn));
  snprintf(getDebugBuffer(), 1024,
           "Device name set to: %s. Bluetooth uses it from its next start and Wi-Fi from its "
           "next connection.%s", name.c_str(), warn);
  return getDebugBuffer();
}

// The settings row. Its editors go through the setter (cmdKey), so the rule
// is checked there; a value loaded from settings.json is checked where it is
// used (SelfDevice::deviceName, the ESP-NOW start gate).
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry deviceSettingEntries[] = {
  { "deviceName", SETTING_STRING, &gSettings.espnowDeviceName, 0, 0, "", 0, 0, "Device Name", nullptr, false, "identity", "devicename" },
};

extern const SettingsModule deviceSettingsModule = {
  "device",
  "system.device",
  deviceSettingEntries,
  sizeof(deviceSettingEntries) / sizeof(deviceSettingEntries[0]),
  nullptr,  // always available
  "Device name: ESP-NOW name, Bluetooth name and Wi-Fi hostname (1-31 letters, digits and '-')"
};
