#include "OLED_Display.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE

#include "OLED_Utils.h"
#include "OLED_SettingsEditor.h"
#include "OLED_RemoteSettings.h"
#include "System_Utils.h"

// Remote settings editor context (reuses SettingsEditorContext but with remote modules)
static bool gRemoteSettingsActive = false;

// Display handler for remote settings mode
static void displayRemoteSettingsMode() {
  // Load remote settings modules if not already loaded
  if (!gRemoteSettingsActive) {
    if (!loadRemoteSettingsModules()) {
      // Failed to load - show error and return to menu
      if (oledDisplay) {
        oledDisplay->clearDisplay();
        oledDisplay->setTextSize(1);
        oledDisplay->setCursor(0, 20);
        oledDisplay->println("No remote settings");
        oledDisplay->println("available");
        displayUpdate();
      }
      delay(1000);
      oledMenuBack();
      return;
    }
    
    // Temporarily swap in remote modules
    extern SettingsEditorContext gSettingsEditor;
    gSettingsEditor.state = SETTINGS_CATEGORY_SELECT;
    gSettingsEditor.categoryIndex = 0;
    gSettingsEditor.itemIndex = 0;
    gSettingsEditor.editValue = 0;
    gSettingsEditor.hasChanges = false;
    gSettingsEditor.currentModule = nullptr;
    gSettingsEditor.currentEntry = nullptr;
    
    gRemoteSettingsActive = true;
  }
  
  // Use the existing settings editor display function
  // It will automatically use the remote modules we loaded
  extern void displaySettingsEditor();
  displaySettingsEditor();
}

// Input handler for remote settings mode
static bool handleRemoteSettingsInput(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  extern SettingsEditorContext gSettingsEditor;

  // Canonical-signal pattern (matches local handleSettingsEditorInput):
  // gNavEvents carries direction for ALL input devices (joystick threshold
  // crossings with built-in auto-repeat, ANO wheel sign, ANO dpad edges).
  // wheelDelta carries fine rotary input for value editing.
  bool handled = false;

  // ---- List navigation ----
  if (gNavEvents.up) {
    extern void settingsEditorUp();
    settingsEditorUp();
    handled = true;
  } else if (gNavEvents.down) {
    extern void settingsEditorDown();
    settingsEditorDown();
    handled = true;
  }

  // ---- A button (select / confirm + remote apply) ----
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (gSettingsEditor.state == SETTINGS_VALUE_EDIT && gSettingsEditor.hasChanges) {
      if (gSettingsEditor.currentModule && gSettingsEditor.currentEntry) {
        String value;
        if (gSettingsEditor.currentEntry->type == SETTING_BOOL) {
          value = gSettingsEditor.editValue ? "1" : "0";
        } else {
          value = String(gSettingsEditor.editValue);
        }
        applyRemoteSettingChange(
          gSettingsEditor.currentModule->name,
          gSettingsEditor.currentEntry->jsonKey,
          value
        );
        gSettingsEditor.hasChanges = false;
      }
    }
    extern void settingsEditorSelect();
    settingsEditorSelect();
    handled = true;
  }

  // ---- B button (back / cancel) ----
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (gSettingsEditor.state == SETTINGS_CATEGORY_SELECT) {
      freeRemoteSettingsModules();
      gRemoteSettingsActive = false;
      return false;  // Let global handler call oledMenuBack()
    } else {
      extern void settingsEditorBack();
      settingsEditorBack();
    }
    return true;
  }

  // ---- Value adjustment in edit mode ----
  // Gamepad: LEFT/RIGHT joystick deflection nudges value (canonical — the
  // joystick has no dedicated value-adjust axis).
  // ANO encoder: wheelDelta is the canonical adjust signal; LEFT is reserved
  // for B-back. Without the !ENABLE_ANO_ENCODER gate, ANO LEFT would step
  // the value AND fire B-back in the same tick, persisting an unwanted
  // mutation. Sign-based step (not multiply by raw deltaX, which could be
  // 400+ for a full joystick deflection — that was an old latent bug that
  // scaled value changes wildly past the intended ±step).
  if (gSettingsEditor.state == SETTINGS_VALUE_EDIT && gSettingsEditor.currentEntry) {
    int dir = 0;
#if !ENABLE_ANO_ENCODER
    if (gNavEvents.right)             dir = +1;
    else if (gNavEvents.left)         dir = -1;
    else
#endif
    if (gNavEvents.wheelDelta != 0) dir = (gNavEvents.wheelDelta > 0) ? +1 : -1;

    if (dir != 0) {
      int step = 1;
      int range = gSettingsEditor.currentEntry->maxVal - gSettingsEditor.currentEntry->minVal;
      if (range > 1000) step = 100;
      else if (range > 100) step = 10;

      gSettingsEditor.editValue += dir * step;
      if (gSettingsEditor.editValue < gSettingsEditor.currentEntry->minVal) {
        gSettingsEditor.editValue = gSettingsEditor.currentEntry->minVal;
      }
      if (gSettingsEditor.editValue > gSettingsEditor.currentEntry->maxVal) {
        gSettingsEditor.editValue = gSettingsEditor.currentEntry->maxVal;
      }
      gSettingsEditor.hasChanges = true;
      handled = true;
    }
  }

  return handled;
}

// Remote settings mode entry
static const OLEDModeEntry remoteSettingsModeEntry = {
  OLED_REMOTE_SETTINGS,
  "Remote Settings",
  "settings",
  displayRemoteSettingsMode,
  nullptr,  // availFunc - always available when paired
  handleRemoteSettingsInput,
  true,     // showInMenu
  50,       // menuOrder
  nullptr   // hints
};

REGISTER_OLED_MODE_MODULE(&remoteSettingsModeEntry, 1, "RemoteSettings");

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE
