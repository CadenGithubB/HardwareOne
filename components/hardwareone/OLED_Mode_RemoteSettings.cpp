#include "OLED_Display.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE

#include "OLED_Utils.h"
#include "OLED_UI.h"
#include "OLED_SettingsEditor.h"
#include "OLED_RemoteSettings.h"
#include "System_Utils.h"

// True once the peer's module set is loaded and the shared editor is reset for it.
static bool gRemoteSettingsActive = false;

static void onEnterRemoteSettings(bool isForward) {
  // A fresh visit rebuilds from the caches; back-navigation keeps the list.
  if (isForward) {
    freeRemoteSettingsModules();
    gRemoteSettingsActive = false;
  }
}

// Display handler for remote settings mode
static void displayRemoteSettingsMode() {
  if (!gRemoteSettingsActive) {
    switch (remoteSettingsLoadStep()) {
      case REMOTE_LOAD_SYNCING:
        if (oledDisplay) {
          oledDisplay->setTextSize(1);
          oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
          oledDisplay->println("Fetching peer");
          oledDisplay->println("schema...");
        }
        oledMarkDirtyUntil(millis() + 250);  // keep frames coming so the load step runs
        return;
      case REMOTE_LOAD_FAILED: {
        // Failed to load - show why and return to menu
        if (oledDisplay) {
          String why = remoteSettingsLastError();
          if (why.length() > 21) why = why.substring(0, 21);
          oledDisplay->clearDisplay();
          oledDisplay->setTextSize(1);
          oledDisplay->setCursor(0, 20);
          oledDisplay->println("No remote settings");
          oledDisplay->println(why);
          displayUpdate();
        }
        delay(1000);
        oledMenuBack();
        return;
      }
      case REMOTE_LOAD_READY:
        initSettingsEditor();
        gRemoteSettingsActive = true;
        break;
      case REMOTE_LOAD_IDLE:
        return;
    }
  }

  // The peer's answer to the last commit (never the local send result).
  String text;
  switch (remoteSettingsPollOutcome(text)) {
    case REMOTE_OUTCOME_APPLIED:
      oledToastShow("Applied", 1500);
      oledMarkDirty();
      break;
    case REMOTE_OUTCOME_FAILED:
      settingsEditorShowError(String("Peer: ") + text, 3000);
      oledMarkDirtyUntil(millis() + 3200);
      break;
    case REMOTE_OUTCOME_TIMEOUT:
      settingsEditorShowError("No reply from peer", 3000);
      oledMarkDirtyUntil(millis() + 3200);
      break;
    case REMOTE_OUTCOME_PENDING:
      oledMarkDirtyUntil(millis() + 250);
      break;
    case REMOTE_OUTCOME_NONE:
      break;
  }
  displaySettingsEditor();
}

// Input handler for remote settings mode
static bool handleRemoteSettingsInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (!gRemoteSettingsActive) return false;
  if (gSettingsEditor.state == SETTINGS_CATEGORY_SELECT && INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    freeRemoteSettingsModules();
    gRemoteSettingsActive = false;
    return false;  // Let global handler call oledMenuBack()
  }
  // The shared editor owns the guest gate, navigation, slider/pick-list/keyboard
  // edits and A/B; its commit goes to the bonded peer while this mode is current.
  return handleSettingsEditorInput(deltaX, deltaY, newlyPressed);
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
  nullptr,  // hints
  onEnterRemoteSettings
};

REGISTER_OLED_MODE_MODULE(&remoteSettingsModeEntry, 1, "RemoteSettings");

// Linker anchor — keep this object file from being dropped by --gc-sections so the
// registrar above runs (otherwise OLED_REMOTE_SETTINGS renders "no render").
void oledRemoteSettingsModeInit() {}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE
