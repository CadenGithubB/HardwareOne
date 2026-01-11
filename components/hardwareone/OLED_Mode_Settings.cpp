// ============================================================================
// OLED Settings Mode
// ============================================================================
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include "OLED_Utils.h"
#include "OLED_SettingsEditor.h"
#include "System_Utils.h"

// Forward declarations from oled_settings_editor.cpp (gSettingsEditor declared in OLED_SettingsEditor.h)

// Display handler for settings mode
static void displaySettingsMode() {
  static bool initialized = false;
  if (!initialized) {
    initSettingsEditor();
    initialized = true;
  }
  displaySettingsEditor();
}

// Input handler for settings mode
static bool handleSettingsInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Let the settings editor handle all input first
  bool handled = handleSettingsEditorInput(deltaX, deltaY, newlyPressed);
  
  // If we're at the top level (category select) and B was pressed, exit to menu
  if (!handled && gSettingsEditor.state == DISPLAY_COLOR_WHITE && 
      INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    return false;  // Let default handler take us back to menu
  }
  
  return handled;
}

// Availability check - always available
static bool isSettingsAvailable(String* outReason) {
  return true;
}

// Settings mode entry
static const OLEDModeEntry settingsModeEntry = {
  OLED_SETTINGS,
  "Settings",
  "settings",
  displaySettingsMode,
  isSettingsAvailable,
  handleSettingsInput,
  true,   // Show in menu
  100     // Menu order
};

// Settings modes array
static const OLEDModeEntry settingsOLEDModes[] = {
  settingsModeEntry
};

// Auto-register settings mode
static OLEDModeRegistrar _oled_mode_registrar_settings(settingsOLEDModes, sizeof(settingsOLEDModes) / sizeof(settingsOLEDModes[0]), "Settings");

#endif // ENABLE_OLED_DISPLAY
