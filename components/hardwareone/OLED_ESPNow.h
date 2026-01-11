#ifndef OLED_ESPNOW_H
#define OLED_ESPNOW_H

#include <Arduino.h>

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#include <Adafruit_SSD1306.h>

#include "OLED_Utils.h"

// =============================================================================
// OLED ESP-NOW Interface
// =============================================================================
// Provides a comprehensive ESP-NOW interface on OLED with:
// - Scrollable device list with names/MACs
// - Per-device message history viewing
// - Text/Remote command mode selection
// - Delivery status indicators (✓ sent, ✓✓ delivered)
// - Gamepad navigation for all interactions

// ESP-NOW OLED view states
enum OLEDEspNowView {
  ESPNOW_VIEW_INIT_PROMPT,    // Initialization prompt (Y to start)
  ESPNOW_VIEW_NAME_KEYBOARD,  // Virtual keyboard for device naming
  ESPNOW_VIEW_DEVICE_LIST,    // Main device list
  ESPNOW_VIEW_DEVICE_DETAIL,  // Single device with messages
  ESPNOW_VIEW_MODE_SELECT,    // Text/Remote selector (drop-up menu)
  ESPNOW_VIEW_BROADCAST,      // Broadcast panel
  ESPNOW_VIEW_TEXT_KEYBOARD,  // Text message keyboard
  ESPNOW_VIEW_REMOTE_FORM,    // Remote command form (username/password/command)
  ESPNOW_VIEW_SETTINGS,       // Settings submenu
  ESPNOW_VIEW_SETTINGS_KEYBOARD // Keyboard for settings input
};

// ESP-NOW interaction mode
enum OLEDEspNowMode {
  ESPNOW_MODE_TEXT,
  ESPNOW_MODE_REMOTE,
  ESPNOW_MODE_FILE
};

// OLED ESP-NOW state
struct OLEDEspNowState {
  OLEDEspNowView currentView;
  OLEDEspNowMode interactionMode;
  
  // Device list scrolling
  OLEDScrollState deviceList;
  
  // Message history scrolling
  OLEDScrollState messageList;
  
  // Currently selected device (MAC address)
  uint8_t selectedDeviceMac[6];
  String selectedDeviceName;
  
  // Mode selector state
  int modeSelectorIndex;  // 0=Text, 1=Remote, 2=File
  bool modeSelectorActive;
  
  // Text mode keyboard state
  String textMessageBuffer;
  
  // Remote mode form state
  int remoteFormField;  // 0=username, 1=password, 2=command
  String remoteUsername;
  String remotePassword;
  String remoteCommand;
  
  // UI state
  unsigned long lastUpdate;
  bool needsRefresh;
  
  // Settings menu state
  int settingsMenuIndex;      // 0=Name, 1=Passphrase, 2=Role, 3=MasterMAC, 4=BackupMAC
  int settingsEditField;      // Which field is being edited (-1 = none)
};

// Global ESP-NOW OLED state
extern OLEDEspNowState gOLEDEspNowState;

// Initialize OLED ESP-NOW interface
void oledEspNowInit();

// Main display function (called from displayEspNow())
void oledEspNowDisplay(Adafruit_SSD1306* display);

// Gamepad input handler
bool oledEspNowHandleInput(int deltaX, int deltaY, uint32_t newlyPressed);

// View-specific display functions
void oledEspNowDisplayDeviceList(Adafruit_SSD1306* display);
void oledEspNowDisplayDeviceDetail(Adafruit_SSD1306* display);
void oledEspNowDisplayModeSelect(Adafruit_SSD1306* display);
void oledEspNowDisplayBroadcast(Adafruit_SSD1306* display);
void oledEspNowDisplayRemoteForm(Adafruit_SSD1306* display);

// Navigation functions
void oledEspNowSelectDevice();
void oledEspNowBackToList();
void oledEspNowOpenModeSelector();
void oledEspNowSelectMode();
void oledEspNowUnpairDevice();

// Helper functions
void oledEspNowRefreshDeviceList();
void oledEspNowRefreshMessages();
String oledEspNowFormatMac(const uint8_t* mac);
void oledEspNowDrawStatusIcon(Adafruit_SSD1306* display, int x, int y, bool delivered);
void oledEspNowShowInitPrompt();
void oledEspNowShowNameKeyboard();

// Remote form and text message functions
bool oledEspNowHandleRemoteFormInput(int deltaX, int deltaY, uint32_t newlyPressed);
void oledEspNowSendTextMessage();
void oledEspNowSendRemoteCommand();

// Settings menu functions
void oledEspNowDisplaySettings(Adafruit_SSD1306* display);
bool oledEspNowHandleSettingsInput(int deltaX, int deltaY, uint32_t newlyPressed);
void oledEspNowOpenSettings();
void oledEspNowApplySettingsEdit(const String& value);

// Buffer safety validation (implemented in cpp)
bool oledEspNowValidateMessagePtr(const void* msgPtr, const uint8_t* peerMac);
bool oledEspNowValidateDevicePtr(const void* devicePtr);

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#endif // OLED_ESPNOW_H
