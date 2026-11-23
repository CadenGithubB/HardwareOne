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

// devicename [<name>|clear]
const char* cmd_devicename(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    // One wording on every surface: the name in use, with " (default)" when
    // none is set (SelfDevice::deviceNameDisplay).
    char shown[devicename::kDisplayCap];
    const bool set = SelfDevice::deviceNameDisplay(shown, sizeof(shown));
    snprintf(getDebugBuffer(), 1024, "Device name: %s%s", shown,
             set ? "" : ". Set one with 'devicename <name>'.");
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
    char shown[devicename::kDisplayCap];
    if (gSettings.espnowDeviceName.length() == 0) {
      (void)SelfDevice::deviceNameDisplay(shown, sizeof(shown));
      snprintf(getDebugBuffer(), 1024, "Device name: %s. Nothing to clear.", shown);
      return getDebugBuffer();
    }
    setSetting(gSettings.espnowDeviceName, String(""));
    deviceNameApply(String(""), nullptr, 0);
    (void)SelfDevice::deviceNameDisplay(shown, sizeof(shown));
    snprintf(getDebugBuffer(), 1024,
             "Device name cleared. Bluetooth and Wi-Fi use %s from their next start"
#if ENABLE_ESPNOW
             "; ESP-NOW will not start until a name is set with 'devicename <name>'"
#endif
             ".",
             shown);
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
