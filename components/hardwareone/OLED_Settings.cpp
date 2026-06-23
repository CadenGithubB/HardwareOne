// OLED_Settings.cpp - OLED display settings module
// Modular settings registration for OLED display configuration

#include "OLED_Display.h"
#include "System_Settings.h"

#if ENABLE_OLED_DISPLAY

// ============================================================================
// OLED Settings Module (for modular settings registry)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry oledSettingEntries[] = {
  { "oledEnabled", SETTING_BOOL, &gSettings.oledEnabled, false, 0, nullptr, 0, 1, "OLED Enabled", nullptr, false, nullptr, nullptr },
  { "oledRequireAuth", SETTING_BOOL, &gSettings.localDisplayRequireAuth, true, 0, nullptr, 0, 1, "Require Authentication", nullptr, false, "display", nullptr },
  { "oledBootMode", SETTING_STRING, &gSettings.oledBootMode, 0, 0, "logo", 0, 0, "Boot Mode", "logo,status,sensors,thermal,network,mesh,off", false, "boot", nullptr },
  { "oledDefaultMode", SETTING_STRING, &gSettings.oledDefaultMode, 0, 0, "status", 0, 0, "Default Mode", "logo,status,sensors,thermal,network,mesh,off", false, "boot", nullptr },
  { "oledBootDuration", SETTING_INT, &gSettings.oledBootDuration, 2000, 0, nullptr, 500, 10000, "Boot Duration (ms)", nullptr, false, "boot", nullptr },
  { "oledUpdateInterval", SETTING_INT, &gSettings.oledUpdateInterval, 125, 0, nullptr, 10, 1000, "Update Interval (ms)", nullptr, false, "display", nullptr },
  { "oledBrightness", SETTING_INT, &gSettings.oledBrightness, 255, 0, nullptr, 0, 255, "Brightness", nullptr, false, "display", nullptr },
  { "oledFlipped", SETTING_BOOL, &gSettings.oledFlipped, true, 0, nullptr, 0, 1, "Flip display 180°", nullptr, false, "display", "oledflip" },
  { "oledThermalScale", SETTING_FLOAT, &gSettings.oledThermalScale, 0, 2.5f, nullptr, 1, 10, "Thermal Scale", nullptr, false, "thermal", nullptr },
  { "oledThermalColorMode", SETTING_STRING, &gSettings.oledThermalColorMode, 0, 0, "3level", 0, 0, "Thermal Color Mode", "3level,grayscale", false, "thermal", nullptr }
};

static bool isOledConnected() {
  return oledConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule oledSettingsModule = {
  "oled", "hardware.sensors.oled", oledSettingEntries,
  sizeof(oledSettingEntries) / sizeof(oledSettingEntries[0]),
  isOledConnected,
  "OLED display (SSD1306)"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

#endif // ENABLE_OLED_DISPLAY
