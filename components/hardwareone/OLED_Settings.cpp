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
  { "oledAutoStart", SETTING_BOOL, &gSettings.oledAutoStart, 0, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, nullptr, "oledautostart" },
  { "oledDefaultMode", SETTING_STRING, &gSettings.oledDefaultMode, 0, 0, "status", 0, 0, "Default Mode", "logo,status,sensors,thermal,network,mesh,off", false, "boot", "oleddefaultmode" },
  { "oledBootDuration", SETTING_INT, &gSettings.oledBootDuration, 2000, 0, nullptr, 500, 10000, "Boot Duration (ms)", nullptr, false, "boot", "oledbootduration" },
  { "oledUpdateInterval", SETTING_INT, &gSettings.oledUpdateInterval, 125, 0, nullptr, 10, 1000, "Update Interval (ms)", nullptr, false, "display", "oledupdateinterval" },
  { "oledBrightness", SETTING_INT, &gSettings.oledBrightness, 255, 0, nullptr, 0, 255, "Brightness", nullptr, false, "display", "oledbrightness" },
#if DISPLAY_TYPE == DISPLAY_TYPE_HX8394_WAVESHARE
  { "oledLandscape", SETTING_BOOL, &gSettings.oledLandscape, false, 0, nullptr, 0, 1, "Landscape UI (90 degrees)", nullptr, false, "display", "oledlandscape" },
#endif
  // 90 and 270 (portrait, 64 x 128) are the SSD1306's; oledrotation refuses them on the P4 canvases.
  { "oledRotation", SETTING_INT, &gSettings.oledRotation, 0, 0, nullptr, 0, 270, "Rotation (degrees)", "0|0 (landscape),90|90 (portrait),180|180 (flipped),270|270 (portrait, flipped)", false, "display", "oledrotation" },
  { "oledThermalScale", SETTING_FLOAT, &gSettings.oledThermalScale, 0, 2.5f, nullptr, 1, 10, "Thermal Scale", nullptr, false, "thermal", "oledthermalscale" },
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
