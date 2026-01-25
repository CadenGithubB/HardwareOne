// OLED_Settings.cpp - OLED display settings module
// Modular settings registration for OLED display configuration

#include "OLED_Display.h"
#include "System_Settings.h"

#if ENABLE_OLED_DISPLAY

// ============================================================================
// OLED Settings Module (for modular settings registry)
// ============================================================================

static const SettingEntry oledSettingEntries[] = {
  { "oledEnabled",          SETTING_BOOL,   &gSettings.oledEnabled,             true, 0, nullptr, 0, 1, "OLED Enabled", nullptr },
  { "oledAutoInit",         SETTING_BOOL,   &gSettings.oledAutoInit,            true, 0, nullptr, 0, 1, "Auto Initialize", nullptr },
  { "oledRequireAuth",      SETTING_BOOL,   &gSettings.localDisplayRequireAuth, true, 0, nullptr, 0, 1, "Require Authentication", nullptr },
  { "oledBootMode",         SETTING_STRING, &gSettings.oledBootMode,            0, 0, "logo", 0, 0, "Boot Mode", "logo,status,thermal,off" },
  { "oledDefaultMode",      SETTING_STRING, &gSettings.oledDefaultMode,         0, 0, "status", 0, 0, "Default Mode", "status,thermal,off" },
  { "oledBootDuration",     SETTING_INT,    &gSettings.oledBootDuration,        3000, 0, nullptr, 500, 10000, "Boot Duration (ms)", nullptr },
  { "oledUpdateInterval",   SETTING_INT,    &gSettings.oledUpdateInterval,      200, 0, nullptr, 10, 1000, "Update Interval (ms)", nullptr },
  { "oledBrightness",       SETTING_INT,    &gSettings.oledBrightness,          128, 0, nullptr, 0, 255, "Brightness", nullptr },
  { "oledThermalScale",     SETTING_FLOAT,  &gSettings.oledThermalScale,        0, 2.5f, nullptr, 1, 10, "Thermal Scale", nullptr },
  { "oledThermalColorMode", SETTING_STRING, &gSettings.oledThermalColorMode,    0, 0, "3level", 0, 0, "Thermal Color Mode", "3level,grayscale,binary" }
};

extern const SettingsModule oledSettingsModule = {
  "oled", "oled_ssd1306", oledSettingEntries,
  sizeof(oledSettingEntries) / sizeof(oledSettingEntries[0])
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

#endif // ENABLE_OLED_DISPLAY
