#include "OLED_Display.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

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
        oledDisplay->display();
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
static bool handleRemoteSettingsInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  extern SettingsEditorContext gSettingsEditor;
  
  // Use provided input parameters
  
  // Handle navigation using existing settings editor functions
  if (deltaY < 0) {
    extern void settingsEditorUp();
    settingsEditorUp();
  } else if (deltaY > 0) {
    extern void settingsEditorDown();
    settingsEditorDown();
  }
  
  // Handle A button (select/confirm)
  if (newlyPressed & GAMEPAD_BUTTON_A) {
    if (gSettingsEditor.state == SETTINGS_VALUE_EDIT && gSettingsEditor.hasChanges) {
      // Apply the change remotely
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
  }
  
  // Handle B button (back/cancel)
  if (newlyPressed & GAMEPAD_BUTTON_B) {
    if (gSettingsEditor.state == SETTINGS_CATEGORY_SELECT) {
      // Exit remote settings mode - clean up then let global handler pop mode stack
      freeRemoteSettingsModules();
      gRemoteSettingsActive = false;
      return false;  // Let global handler call oledMenuBack()
    } else {
      extern void settingsEditorBack();
      settingsEditorBack();
    }
    return true;
  }
  
  // Handle left/right for value editing
  if (gSettingsEditor.state == SETTINGS_VALUE_EDIT) {
    if (deltaX != 0 && gSettingsEditor.currentEntry) {
      int step = 1;
      // Larger steps for bigger ranges
      int range = gSettingsEditor.currentEntry->maxVal - gSettingsEditor.currentEntry->minVal;
      if (range > 1000) step = 100;
      else if (range > 100) step = 10;
      
      gSettingsEditor.editValue += deltaX * step;
      
      // Clamp to range
      if (gSettingsEditor.editValue < gSettingsEditor.currentEntry->minVal) {
        gSettingsEditor.editValue = gSettingsEditor.currentEntry->minVal;
      }
      if (gSettingsEditor.editValue > gSettingsEditor.currentEntry->maxVal) {
        gSettingsEditor.editValue = gSettingsEditor.currentEntry->maxVal;
      }
      
      gSettingsEditor.hasChanges = true;
      return true;
    }
  }
  
  return false;
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
  50        // menuOrder
};

// Register remote settings mode
static void registerRemoteSettingsMode() __attribute__((constructor));
static void registerRemoteSettingsMode() {
  extern void registerOLEDMode(const OLEDModeEntry* entry);
  registerOLEDMode(&remoteSettingsModeEntry);
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW
