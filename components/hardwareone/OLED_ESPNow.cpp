#include "OLED_ESPNow.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_ESPNow.h"
#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw.h"
#endif

// =============================================================================
// OLED ESP-NOW Interface Implementation
// =============================================================================

// Global state
OLEDEspNowState gOLEDEspNowState;

void oledEspNowInit() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_LIST;
  gOLEDEspNowState.interactionMode = ESPNOW_MODE_TEXT;
  gOLEDEspNowState.modeSelectorIndex = 0;
  gOLEDEspNowState.modeSelectorActive = false;
  gOLEDEspNowState.lastUpdate = 0;
  gOLEDEspNowState.needsRefresh = true;
  memset(gOLEDEspNowState.selectedDeviceMac, 0, 6);
  gOLEDEspNowState.selectedDeviceName = "";
  
  // Text mode state
  gOLEDEspNowState.textMessageBuffer = "";
  
  // Remote mode state
  gOLEDEspNowState.remoteFormField = 0;
  gOLEDEspNowState.remoteUsername = "";
  gOLEDEspNowState.remotePassword = "";
  gOLEDEspNowState.remoteCommand = "";
  
  // Initialize scrolling lists
  oledScrollInit(&gOLEDEspNowState.deviceList, "ESP-NOW Devices", 3);
  oledScrollInit(&gOLEDEspNowState.messageList, nullptr, 3);
  
  // Settings menu state
  gOLEDEspNowState.settingsMenuIndex = 0;
  gOLEDEspNowState.settingsEditField = -1;
}

void oledEspNowShowInitPrompt() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_INIT_PROMPT;
}

void oledEspNowShowNameKeyboard() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_NAME_KEYBOARD;
  const char* initialText = "";
  if (gSettings.espnowDeviceName.length() > 0) {
    initialText = gSettings.espnowDeviceName.c_str();
  }
  oledKeyboardInit("Device Name:", initialText, 20);
}

void oledEspNowDisplay(Adafruit_SSD1306* display) {
  if (!display) return;

  if (gOLEDEspNowState.currentView == ESPNOW_VIEW_INIT_PROMPT && gEspNow && gEspNow->initialized) {
    oledEspNowInit();
  }
  
  // Handle views that don't require ESP-NOW to be initialized
  if (gOLEDEspNowState.currentView == ESPNOW_VIEW_INIT_PROMPT ||
      gOLEDEspNowState.currentView == ESPNOW_VIEW_NAME_KEYBOARD) {
    // These views are shown before ESP-NOW init
    switch (gOLEDEspNowState.currentView) {
      case ESPNOW_VIEW_INIT_PROMPT:
        display->setTextSize(1);
        display->setTextColor(DISPLAY_COLOR_WHITE);
        display->setCursor(0, 0);
        display->println("=== ESP-NOW Setup ===");
        display->println();
        display->println("ESP-NOW not initialized");
        display->println();
        display->println("Press Y to set device");
        display->println("name and initialize");
        display->println();
        display->setCursor(0, 56);
        display->print("Y:Setup B:Back");
        break;
      case ESPNOW_VIEW_NAME_KEYBOARD:
        oledKeyboardDisplay(display);
        break;
      default:
        break;
    }
    return;
  }
  
  // All other views require ESP-NOW to be initialized
  if (!gEspNow || !gEspNow->initialized) return;
  
  // Refresh data periodically
  unsigned long now = millis();
  if (now - gOLEDEspNowState.lastUpdate > 1000 || gOLEDEspNowState.needsRefresh) {
    if (gOLEDEspNowState.currentView == ESPNOW_VIEW_DEVICE_LIST) {
      oledEspNowRefreshDeviceList();
    } else if (gOLEDEspNowState.currentView == ESPNOW_VIEW_DEVICE_DETAIL) {
      oledEspNowRefreshMessages();
    }
    gOLEDEspNowState.lastUpdate = now;
    gOLEDEspNowState.needsRefresh = false;
  }
  
  // Display current view
  switch (gOLEDEspNowState.currentView) {
    case ESPNOW_VIEW_DEVICE_LIST:
      oledEspNowDisplayDeviceList(display);
      break;
    case ESPNOW_VIEW_DEVICE_DETAIL:
      oledEspNowDisplayDeviceDetail(display);
      break;
    case ESPNOW_VIEW_MODE_SELECT:
      oledEspNowDisplayModeSelect(display);
      break;
    case ESPNOW_VIEW_BROADCAST:
      oledEspNowDisplayBroadcast(display);
      break;
    case ESPNOW_VIEW_TEXT_KEYBOARD:
      oledKeyboardDisplay(display);
      break;
    case ESPNOW_VIEW_REMOTE_FORM:
      oledEspNowDisplayRemoteForm(display);
      break;
    case ESPNOW_VIEW_SETTINGS:
      oledEspNowDisplaySettings(display);
      break;
    case ESPNOW_VIEW_SETTINGS_KEYBOARD:
      oledKeyboardDisplay(display);
      break;
    default:
      break;
  }
}

void oledEspNowDisplayDeviceList(Adafruit_SSD1306* display) {
  if (!display) return;
  
  // Build dynamic title with role indicator
  static char titleBuf[24];
  const char* roleStr = "[W]";
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    roleStr = "[M]";
  } else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) {
    roleStr = "[B]";
  }
  
  // Show encryption status in title
  if (gEspNow && gEspNow->encryptionEnabled) {
    snprintf(titleBuf, sizeof(titleBuf), "ESP-NOW %s E", roleStr);
  } else {
    snprintf(titleBuf, sizeof(titleBuf), "ESP-NOW %s", roleStr);
  }
  gOLEDEspNowState.deviceList.title = titleBuf;
  
  // Render device list using scrolling system
  oledScrollRender(display, &gOLEDEspNowState.deviceList, true, true);
  
  // Show footer with instructions (Y opens settings)
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, 56);
  display->print("A:Sel Y:Set B:Back");
}

void oledEspNowDisplayDeviceDetail(Adafruit_SSD1306* display) {
  if (!display) return;
  
  // Draw header with device name
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, 0);
  
  String header = gOLEDEspNowState.selectedDeviceName;
  if (header.length() == 0) {
    header = oledEspNowFormatMac(gOLEDEspNowState.selectedDeviceMac);
  }
  if (header.length() > 21) header = header.substring(0, 20) + "~";
  display->println(header);
  
  // Draw mode indicator
  display->setCursor(0, 8);
  display->print("Mode: ");
  if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
    display->println("Text");
  } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
    display->println("Remote");
  } else {
    display->println("File");
  }
  
  // Draw separator
  display->drawFastHLine(0, 17, 128, DISPLAY_COLOR_WHITE);
  
  // If in File mode, show file browser prompt instead of message list
  if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_FILE) {
    display->setCursor(0, 20);
    display->setTextSize(1);
    display->println("File Transfer Mode");
    display->println();
    display->println("Press A to browse");
    display->println("files to send");
    display->println();
    display->setCursor(0, 56);
    display->print("A:Browse X:Mode B:Back");
    return;
  }
  
  // Render message list (offset by header height)
  int yOffset = 18;
  int visibleStart = gOLEDEspNowState.messageList.scrollOffset;
  int visibleEnd = min(gOLEDEspNowState.messageList.itemCount, 
                       visibleStart + gOLEDEspNowState.messageList.visibleLines);
  
  int yPos = yOffset;
  int lineHeight = 8;
  
  for (int i = visibleStart; i < visibleEnd && yPos < 56; i++) {
    OLEDScrollItem* item = &gOLEDEspNowState.messageList.items[i];
    bool isSelected = (i == gOLEDEspNowState.messageList.selectedIndex);
    
    // Draw selection indicator
    if (isSelected) {
      display->fillRect(0, yPos, 2, lineHeight * 2, DISPLAY_COLOR_WHITE);
      display->setCursor(4, yPos);
    } else {
      display->setCursor(0, yPos);
    }
    
    // Draw message text (truncated)
    String msg = item->line1;
    if (msg.length() > 20) msg = msg.substring(0, 19) + "~";
    display->println(msg);
    
    yPos += lineHeight;
    
    // Draw status/time on second line
    if (isSelected) {
      display->setCursor(4, yPos);
    } else {
      display->setCursor(0, yPos);
    }
    display->println(item->line2);
    
    yPos += lineHeight;
  }
  
  // Show scrollbar if needed
  if (gOLEDEspNowState.messageList.itemCount > gOLEDEspNowState.messageList.visibleLines) {
    int scrollbarX = SCREEN_WIDTH - 1;
    int scrollbarHeight = 38;  // 56 - 18
    int scrollbarY = yOffset;
    
    display->drawFastVLine(scrollbarX, scrollbarY, scrollbarHeight, DISPLAY_COLOR_WHITE);
    
    int thumbHeight = max(4, (scrollbarHeight * gOLEDEspNowState.messageList.visibleLines) / 
                            gOLEDEspNowState.messageList.itemCount);
    int thumbY = scrollbarY + (scrollbarHeight - thumbHeight) * 
                 gOLEDEspNowState.messageList.scrollOffset / 
                 max(1, gOLEDEspNowState.messageList.itemCount - gOLEDEspNowState.messageList.visibleLines);
    
    display->fillRect(scrollbarX - 1, thumbY, 3, thumbHeight, DISPLAY_COLOR_WHITE);
  }
  
  // Footer - show mode-specific instructions
  display->setCursor(0, 56);
  if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
    display->print("A:Send X:Mode B:Back");
  } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
    display->print("A:Remote X:Mode B:Back");
  } else {
    display->print("Y:Unpair X:Mode B:Back");
  }
}

void oledEspNowDisplayModeSelect(Adafruit_SSD1306* display) {
  if (!display) return;
  
  // Draw semi-transparent background (drop-up menu effect)
  display->fillRect(20, 16, 88, 38, DISPLAY_COLOR_BLACK);
  display->drawRect(20, 16, 88, 38, DISPLAY_COLOR_WHITE);
  
  // Draw title
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(24, 18);
  display->println("Select Mode:");
  
  // Draw options
  display->setCursor(24, 28);
  if (gOLEDEspNowState.modeSelectorIndex == 0) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> Text     ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  Text     ");
  }
  
  display->setCursor(24, 36);
  if (gOLEDEspNowState.modeSelectorIndex == 1) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> Remote   ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  Remote   ");
  }
  
  display->setCursor(24, 44);
  if (gOLEDEspNowState.modeSelectorIndex == 2) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    display->print("> File     ");
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print("  File     ");
  }
}

void oledEspNowDisplayBroadcast(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, 0);
  display->println("=== Broadcast ===");
  display->println();
  display->println("Broadcast to all");
  display->println("paired devices");
  display->println();
  display->println("(Not yet impl.)");
  display->println();
  display->setCursor(0, 56);
  display->print("B:Back");
}

bool oledEspNowHandleInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Handle input based on current view
  switch (gOLEDEspNowState.currentView) {
    case ESPNOW_VIEW_INIT_PROMPT:
      // Init prompt is handled in oled_display.cpp
      return false;
      
    case ESPNOW_VIEW_NAME_KEYBOARD:
      // Let keyboard handle input
      return oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
      
    case ESPNOW_VIEW_DEVICE_LIST:
      // Navigate device list using centralized navigation events
      if (gNavEvents.up) {
        oledScrollUp(&gOLEDEspNowState.deviceList);
        return true;
      }
      if (gNavEvents.down) {
        oledScrollDown(&gOLEDEspNowState.deviceList);
        return true;
      }
      
      // A button: Select device
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        oledEspNowSelectDevice();
        return true;
      }
      
      // Y button: Open settings
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
        oledEspNowOpenSettings();
        return true;
      }
      
      // X button: Open broadcast panel
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        gOLEDEspNowState.currentView = ESPNOW_VIEW_BROADCAST;
        return true;
      }
      
      // B button: Back to menu
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        return false;  // Let default handler take us back to menu
      }
      return false;  // No input handled
      
    case ESPNOW_VIEW_SETTINGS:
      return oledEspNowHandleSettingsInput(deltaX, deltaY, newlyPressed);
      
    case ESPNOW_VIEW_SETTINGS_KEYBOARD:
      if (oledKeyboardHandleInput(deltaX, deltaY, newlyPressed)) {
        if (oledKeyboardIsCompleted()) {
          String value = oledKeyboardGetText();
          oledEspNowApplySettingsEdit(value);
          oledKeyboardReset();
          gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
        }
        return true;
      }
      // B button cancels keyboard
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        oledKeyboardReset();
        gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
        return true;
      }
      return false;
      
    case ESPNOW_VIEW_DEVICE_DETAIL:
      // If in File mode, A button opens file browser
      if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_FILE) {
        // A button: Open file browser
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          extern OLEDMode currentOLEDMode;
          extern void resetOLEDFileBrowser();
          currentOLEDMode = OLED_FILE_BROWSER;
          resetOLEDFileBrowser();
          return true;
        }
      } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
        // A button: Open text keyboard
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          gOLEDEspNowState.currentView = ESPNOW_VIEW_TEXT_KEYBOARD;
          gOLEDEspNowState.textMessageBuffer = "";
          oledKeyboardInit("Send Message:", "", 128);
          return true;
        }
        
        // Navigate message list using centralized navigation events
        if (gNavEvents.up) {
          oledScrollUp(&gOLEDEspNowState.messageList);
          return true;
        }
        if (gNavEvents.down) {
          oledScrollDown(&gOLEDEspNowState.messageList);
          return true;
        }
        
        // Y button: Unpair device
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
          oledEspNowUnpairDevice();
          return true;
        }
      } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
        // A button: Open remote form
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
          gOLEDEspNowState.currentView = ESPNOW_VIEW_REMOTE_FORM;
          gOLEDEspNowState.remoteFormField = 0;
          gOLEDEspNowState.remoteUsername = "";
          gOLEDEspNowState.remotePassword = "";
          gOLEDEspNowState.remoteCommand = "";
          return true;
        }
        
        // Navigate message list using centralized navigation events
        if (gNavEvents.up) {
          oledScrollUp(&gOLEDEspNowState.messageList);
          return true;
        }
        if (gNavEvents.down) {
          oledScrollDown(&gOLEDEspNowState.messageList);
          return true;
        }
        
        // Y button: Unpair device
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
          oledEspNowUnpairDevice();
          return true;
        }
      }
      
      // X button: Open mode selector (all modes)
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        oledEspNowOpenModeSelector();
        return true;
      }
      
      // B button: Back to device list (all modes)
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        oledEspNowBackToList();
        return true;
      }
      return false;  // No input handled
      
    case ESPNOW_VIEW_MODE_SELECT:
      // Navigate mode selector using centralized navigation events
      if (gNavEvents.up && gOLEDEspNowState.modeSelectorIndex > 0) {
        gOLEDEspNowState.modeSelectorIndex--;
        return true;
      }
      if (gNavEvents.down && gOLEDEspNowState.modeSelectorIndex < 2) {
        gOLEDEspNowState.modeSelectorIndex++;
        return true;
      }
      
      // A button: Select mode
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        oledEspNowSelectMode();
        return true;
      }
      
      // B button: Cancel
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        return true;
      }
      return false;  // No input handled
      
    case ESPNOW_VIEW_BROADCAST:
      // B button: Back to device list
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_LIST;
        return true;
      }
      return false;  // No input handled
      
    case ESPNOW_VIEW_TEXT_KEYBOARD:
      // Handle keyboard input
      if (oledKeyboardHandleInput(deltaX, deltaY, newlyPressed)) {
        return true;
      }
      
      // Check if keyboard completed or cancelled
      if (oledKeyboardIsCompleted()) {
        gOLEDEspNowState.textMessageBuffer = String(oledKeyboardGetText());
        oledEspNowSendTextMessage();
        oledKeyboardReset();
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        return true;
      }
      if (oledKeyboardIsCancelled()) {
        oledKeyboardReset();
        gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
        return true;
      }
      return false;
      
    case ESPNOW_VIEW_REMOTE_FORM:
      return oledEspNowHandleRemoteFormInput(deltaX, deltaY, newlyPressed);
  }
  
  return false;  // Default: no input handled
}

void oledEspNowSelectDevice() {
  OLEDScrollItem* selected = oledScrollGetSelected(&gOLEDEspNowState.deviceList);
  if (!selected || !selected->userData) return;
  
  // Store selected device MAC
  EspNowDevice* device = (EspNowDevice*)selected->userData;
  memcpy(gOLEDEspNowState.selectedDeviceMac, device->mac, 6);
  gOLEDEspNowState.selectedDeviceName = String(device->name);
  
  // Switch to device detail view
  gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
  gOLEDEspNowState.needsRefresh = true;
  
  // Refresh messages for this device
  oledEspNowRefreshMessages();
}

void oledEspNowBackToList() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_LIST;
  gOLEDEspNowState.needsRefresh = true;
}

void oledEspNowOpenModeSelector() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_MODE_SELECT;
  // Map current mode to selector index: Text=0, Remote=1, File=2
  if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_TEXT) {
    gOLEDEspNowState.modeSelectorIndex = 0;
  } else if (gOLEDEspNowState.interactionMode == ESPNOW_MODE_REMOTE) {
    gOLEDEspNowState.modeSelectorIndex = 1;
  } else {
    gOLEDEspNowState.modeSelectorIndex = 2;  // File
  }
  gOLEDEspNowState.modeSelectorActive = true;
}

void oledEspNowSelectMode() {
  // Map selector index to mode: 0=Text, 1=Remote, 2=File
  if (gOLEDEspNowState.modeSelectorIndex == 0) {
    gOLEDEspNowState.interactionMode = ESPNOW_MODE_TEXT;
  } else if (gOLEDEspNowState.modeSelectorIndex == 1) {
    gOLEDEspNowState.interactionMode = ESPNOW_MODE_REMOTE;
  } else {
    gOLEDEspNowState.interactionMode = ESPNOW_MODE_FILE;
  }
  gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
  gOLEDEspNowState.modeSelectorActive = false;
}

void oledEspNowUnpairDevice() {
  if (!gEspNow) return;
  
  // Find device in paired list
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, gOLEDEspNowState.selectedDeviceMac, 6) == 0) {
      // Remove from ESP-NOW peer list
      esp_now_del_peer(gEspNow->devices[i].mac);
      
      // Shift remaining devices
      for (int j = i; j < gEspNow->deviceCount - 1; j++) {
        gEspNow->devices[j] = gEspNow->devices[j + 1];
      }
      gEspNow->deviceCount--;
      
      // Go back to device list
      oledEspNowBackToList();
      break;
    }
  }
}

void oledEspNowRefreshDeviceList() {
  if (!gEspNow) return;
  
  oledScrollClear(&gOLEDEspNowState.deviceList);
  
  // Get own MAC to skip self
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  
  // Add all paired devices except self - use direct pointers to device data
  int visibleDeviceCount = 0;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    EspNowDevice* device = &gEspNow->devices[i];
    
    // Skip own device
    if (memcmp(device->mac, myMac, 6) == 0) {
      continue;
    }
    
    // Device name is a String, get c_str() pointer
    const char* line1 = device->name.c_str();
    if (!line1 || line1[0] == '\0') {
      // Use static fallback for unnamed devices
      static char fallbackNames[16][16];  // 16 devices max
      int deviceNum = (i + 1) % 10000;  // Limit to 4 digits max
      snprintf(fallbackNames[i % 16], 16, "Device %d", deviceNum);
      line1 = fallbackNames[i % 16];
    }
    
    // Format MAC address into static buffer (reused each iteration)
    static char macBuf[24];
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X%s",
             device->mac[0], device->mac[1], device->mac[2],
             device->mac[3], device->mac[4], device->mac[5],
             device->encrypted ? " E" : "");
    
    oledScrollAddItem(&gOLEDEspNowState.deviceList, line1, macBuf, true, device);
    visibleDeviceCount++;
  }
  
  // If no visible devices (excluding self), show message
  if (visibleDeviceCount == 0) {
    static const char* noDevLine1 = "No devices";
    static const char* noDevLine2 = "Pair via web UI";
    oledScrollAddItem(&gOLEDEspNowState.deviceList, noDevLine1, noDevLine2, false, nullptr);
  }
}

void oledEspNowRefreshMessages() {
  if (!gEspNow) return;
  
  oledScrollClear(&gOLEDEspNowState.messageList);
  
  // Get pointer to peer message history (direct access, no copy)
  PeerMessageHistory* peerHistory = findOrCreatePeerHistory(gOLEDEspNowState.selectedDeviceMac);
  if (!peerHistory || peerHistory->count == 0) {
    // No messages, show placeholder
    static const char* noMsgLine1 = "No messages yet";
    static const char* noMsgLine2 = "Start chatting!";
    oledScrollAddItem(&gOLEDEspNowState.messageList, noMsgLine1, noMsgLine2, false, nullptr);
    return;
  }
  
  // SAFETY: Iterate ring buffer correctly from tail to head
  // This handles wraparound safely - messages are stored in ring buffer order
  int messagesToShow = min(10, (int)peerHistory->count);  // Show last 10 messages
  int startOffset = max(0, (int)peerHistory->count - messagesToShow);
  
  for (int i = startOffset; i < peerHistory->count; i++) {
    // Calculate ring buffer index (handles wraparound)
    uint8_t idx = (peerHistory->tail + i) % MESSAGES_PER_DEVICE;
    ReceivedTextMessage* msg = &peerHistory->messages[idx];
    
    // SAFETY: Skip inactive messages (may have been overwritten)
    if (!msg->active) continue;
    
    // SAFETY: Validate pointer is still within bounds
    if (!oledEspNowValidateMessagePtr(msg, gOLEDEspNowState.selectedDeviceMac)) continue;
    
    // Use direct pointers to message buffer data (no String copies)
    const char* line1 = msg->message;
    const char* line2 = msg->senderName;
    
    // Check if this is a sent or received message
    uint8_t selfMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfMac);
    bool isSent = (memcmp(msg->senderMac, selfMac, 6) == 0);
    
    if (isSent) {
      // For sent messages, use static string for status
      static const char* sentStatus = "Sent";
      line2 = sentStatus;
    } else if (!line2 || line2[0] == '\0') {
      // For received messages with no sender name
      static const char* unknownSender = "Unknown";
      line2 = unknownSender;
    }
    
    // Add item with direct pointers - no data copying
    oledScrollAddItem(&gOLEDEspNowState.messageList, line1, line2, true, (void*)msg);
  }
}

String oledEspNowFormatMac(const uint8_t* mac) {
  if (!mac) return "00:00:00:00:00:00";
  
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

void oledEspNowDrawStatusIcon(Adafruit_SSD1306* display, int x, int y, bool delivered) {
  if (!display) return;
  
  // Draw checkmark(s)
  if (delivered) {
    // Double checkmark for delivered
    display->drawLine(x, y + 2, x + 1, y + 3, DISPLAY_COLOR_WHITE);
    display->drawLine(x + 1, y + 3, x + 3, y + 1, DISPLAY_COLOR_WHITE);
    display->drawLine(x + 2, y + 2, x + 3, y + 3, DISPLAY_COLOR_WHITE);
    display->drawLine(x + 3, y + 3, x + 5, y + 1, DISPLAY_COLOR_WHITE);
  } else {
    // Single checkmark for sent
    display->drawLine(x, y + 2, x + 1, y + 3, DISPLAY_COLOR_WHITE);
    display->drawLine(x + 1, y + 3, x + 3, y + 1, DISPLAY_COLOR_WHITE);
  }
}

// =============================================================================
// Buffer Safety Validation
// =============================================================================

bool oledEspNowValidateMessagePtr(const void* msgPtr, const uint8_t* peerMac) {
  if (!msgPtr || !peerMac || !gEspNow) return false;
  
  // Find the peer history for this MAC
  PeerMessageHistory* history = nullptr;
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gEspNow->peerMessageHistories[i].active && 
        memcmp(gEspNow->peerMessageHistories[i].peerMac, peerMac, 6) == 0) {
      history = &gEspNow->peerMessageHistories[i];
      break;
    }
  }
  
  if (!history) return false;
  
  // Check if pointer is within the message array bounds
  const ReceivedTextMessage* msg = (const ReceivedTextMessage*)msgPtr;
  const ReceivedTextMessage* arrayStart = &history->messages[0];
  const ReceivedTextMessage* arrayEnd = &history->messages[MESSAGES_PER_DEVICE];
  
  if (msg < arrayStart || msg >= arrayEnd) return false;
  
  // Check if message is still active
  return msg->active;
}

bool oledEspNowValidateDevicePtr(const void* devicePtr) {
  if (!devicePtr || !gEspNow) return false;
  
  // Check if pointer is within the device array bounds
  const EspNowDevice* device = (const EspNowDevice*)devicePtr;
  const EspNowDevice* arrayStart = &gEspNow->devices[0];
  const EspNowDevice* arrayEnd = &gEspNow->devices[16];  // devices[16] in EspNowSystem
  
  return (device >= arrayStart && device < arrayEnd);
}

// =============================================================================
// ESP-NOW Remote Form and Text Message Functions (merged from oled_espnow_remote.cpp)
// =============================================================================

void oledEspNowDisplayRemoteForm(Adafruit_SSD1306* display) {
  if (!display) return;
  
  // If keyboard is active, show it instead of the form
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(display);
    return;
  }
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, 0);
  display->println("=== Remote Command ===");
  display->println();
  
  // Display form fields with selection indicator
  // Field 0: Username
  if (gOLEDEspNowState.remoteFormField == 0) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> User: ");
  display->println(gOLEDEspNowState.remoteUsername.length() > 0 ? 
                   gOLEDEspNowState.remoteUsername.c_str() : "_____");
  
  // Field 1: Password
  if (gOLEDEspNowState.remoteFormField == 1) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> Pass: ");
  // Show asterisks for password
  if (gOLEDEspNowState.remotePassword.length() > 0) {
    for (size_t i = 0; i < gOLEDEspNowState.remotePassword.length(); i++) {
      display->print("*");
    }
    display->println();
  } else {
    display->println("_____");
  }
  
  // Field 2: Command
  if (gOLEDEspNowState.remoteFormField == 2) {
    display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    display->setTextColor(DISPLAY_COLOR_WHITE);
  }
  display->print("> Cmd: ");
  display->println(gOLEDEspNowState.remoteCommand.length() > 0 ? 
                   gOLEDEspNowState.remoteCommand.c_str() : "_____");
  
  // Footer
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->println();
  display->println("A:Edit Y:Send B:Cancel");
}

bool oledEspNowHandleRemoteFormInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Check if keyboard is active (inline editing) - handle this first
  if (oledKeyboardIsActive()) {
    // Let keyboard handle input
    oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
    
    // Check if keyboard completed
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      switch (gOLEDEspNowState.remoteFormField) {
        case 0:
          gOLEDEspNowState.remoteUsername = String(text);
          break;
        case 1:
          gOLEDEspNowState.remotePassword = String(text);
          break;
        case 2:
          gOLEDEspNowState.remoteCommand = String(text);
          break;
      }
      oledKeyboardReset();
      return true;
    }
    
    // Check if keyboard cancelled
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      return true;
    }
    
    return true;  // Keyboard is active, consume all input
  }
  
  // Keyboard not active - handle form navigation
  // Navigate between fields using centralized navigation events
  if (gNavEvents.up && gOLEDEspNowState.remoteFormField > 0) {
    gOLEDEspNowState.remoteFormField--;
    return true;
  }
  if (gNavEvents.down && gOLEDEspNowState.remoteFormField < 2) {
    gOLEDEspNowState.remoteFormField++;
    return true;
  }
  
  // A button: Edit current field with keyboard
  if (newlyPressed & GAMEPAD_BUTTON_A) {
    const char* title = "";
    const char* initialText = "";
    
    switch (gOLEDEspNowState.remoteFormField) {
      case 0:
        title = "Username:";
        initialText = gOLEDEspNowState.remoteUsername.c_str();
        break;
      case 1:
        title = "Password:";
        initialText = gOLEDEspNowState.remotePassword.c_str();
        break;
      case 2:
        title = "Command:";
        initialText = gOLEDEspNowState.remoteCommand.c_str();
        break;
    }
    
    oledKeyboardInit(title, initialText, 64);
    return true;
  }
  
  // Y button: Send remote command
  if (newlyPressed & GAMEPAD_BUTTON_Y) {
    oledEspNowSendRemoteCommand();
    gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    return true;
  }
  
  // B button: Cancel form
  if (newlyPressed & GAMEPAD_BUTTON_B) {
    gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_DETAIL;
    return true;
  }
  
  return false;
}

void oledEspNowSendTextMessage() {
  if (!gEspNow || gOLEDEspNowState.textMessageBuffer.length() == 0) return;
  
  // Send text message to selected device
  extern String executeCommandThroughRegistry(const String& cmd);
  
  // Format MAC address
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           gOLEDEspNowState.selectedDeviceMac[0],
           gOLEDEspNowState.selectedDeviceMac[1],
           gOLEDEspNowState.selectedDeviceMac[2],
           gOLEDEspNowState.selectedDeviceMac[3],
           gOLEDEspNowState.selectedDeviceMac[4],
           gOLEDEspNowState.selectedDeviceMac[5]);
  
  // Build command: espnow send <mac> <message>
  String cmd = "espnow send " + String(macStr) + " " + gOLEDEspNowState.textMessageBuffer;
  executeCommandThroughRegistry(cmd);
  
  // Clear buffer
  gOLEDEspNowState.textMessageBuffer = "";
  
  // Refresh message list
  gOLEDEspNowState.needsRefresh = true;
}

void oledEspNowSendRemoteCommand() {
  if (!gEspNow) return;
  
  // Validate that all fields are filled
  if (gOLEDEspNowState.remoteUsername.length() == 0 ||
      gOLEDEspNowState.remotePassword.length() == 0 ||
      gOLEDEspNowState.remoteCommand.length() == 0) {
    return;  // Don't send if any field is empty
  }
  
  extern String executeCommandThroughRegistry(const String& cmd);
  
  // Format MAC address
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           gOLEDEspNowState.selectedDeviceMac[0],
           gOLEDEspNowState.selectedDeviceMac[1],
           gOLEDEspNowState.selectedDeviceMac[2],
           gOLEDEspNowState.selectedDeviceMac[3],
           gOLEDEspNowState.selectedDeviceMac[4],
           gOLEDEspNowState.selectedDeviceMac[5]);
  
  // Build command: espnow remote <mac> <username> <password> <command>
  String cmd = "espnow remote " + String(macStr) + " " + 
               gOLEDEspNowState.remoteUsername + " " +
               gOLEDEspNowState.remotePassword + " " +
               gOLEDEspNowState.remoteCommand;
  
  executeCommandThroughRegistry(cmd);
  
  // Clear form
  gOLEDEspNowState.remoteUsername = "";
  gOLEDEspNowState.remotePassword = "";
  gOLEDEspNowState.remoteCommand = "";
  
  // Refresh message list
  gOLEDEspNowState.needsRefresh = true;
}

// =============================================================================
// ESP-NOW Settings Menu
// =============================================================================

// Settings menu items: 0=Name, 1=Passphrase, 2=Role, 3=MasterMAC, 4=BackupMAC
#define ESPNOW_SETTINGS_COUNT 5

static const char* espnowSettingsLabels[ESPNOW_SETTINGS_COUNT] = {
  "Device Name",
  "Passphrase",
  "Role",
  "Master MAC",
  "Backup MAC"
};

void oledEspNowOpenSettings() {
  gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS;
  gOLEDEspNowState.settingsMenuIndex = 0;
  gOLEDEspNowState.settingsEditField = -1;
}

void oledEspNowDisplaySettings(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Header
  display->setCursor(0, 0);
  display->println("=== ESP-NOW Settings ===");
  
  // Calculate visible items (5 items, 8 pixels per line, start at y=10)
  int startY = 10;
  int lineHeight = 9;
  
  for (int i = 0; i < ESPNOW_SETTINGS_COUNT; i++) {
    int y = startY + i * lineHeight;
    if (y > 48) break;  // Don't draw below footer area
    
    // Selection indicator
    if (i == gOLEDEspNowState.settingsMenuIndex) {
      display->fillRect(0, y, 2, lineHeight - 1, DISPLAY_COLOR_WHITE);
    }
    
    display->setCursor(4, y);
    display->print(espnowSettingsLabels[i]);
    display->print(": ");
    
    // Show current value (truncated)
    String value;
    switch (i) {
      case 0: // Device Name
        value = gSettings.espnowDeviceName;
        if (value.length() == 0) value = "(not set)";
        break;
      case 1: // Passphrase
        value = gSettings.espnowPassphrase.length() > 0 ? "****" : "(not set)";
        break;
      case 2: // Role
        if (gSettings.meshRole == MESH_ROLE_MASTER) value = "Master";
        else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) value = "Backup";
        else value = "Worker";
        break;
      case 3: // Master MAC
        value = gSettings.meshMasterMAC;
        if (value.length() == 0) value = "(auto)";
        break;
      case 4: // Backup MAC
        value = gSettings.meshBackupMAC;
        if (value.length() == 0) value = "(none)";
        break;
    }
    
    // Truncate value if needed
    int labelLen = strlen(espnowSettingsLabels[i]) + 2;  // label + ": "
    int maxValueLen = (128 - 4 - labelLen * 6) / 6;
    if (value.length() > maxValueLen && maxValueLen > 3) {
      value = value.substring(0, maxValueLen - 1) + "~";
    }
    display->print(value);
  }
  
  // Footer
  display->setCursor(0, 56);
  display->print("A:Edit ^v:Nav B:Back");
}

bool oledEspNowHandleSettingsInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Navigation
  if (gNavEvents.up && gOLEDEspNowState.settingsMenuIndex > 0) {
    gOLEDEspNowState.settingsMenuIndex--;
    return true;
  }
  if (gNavEvents.down && gOLEDEspNowState.settingsMenuIndex < ESPNOW_SETTINGS_COUNT - 1) {
    gOLEDEspNowState.settingsMenuIndex++;
    return true;
  }
  
  // A button: Edit selected item
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    gOLEDEspNowState.settingsEditField = gOLEDEspNowState.settingsMenuIndex;
    
    // For Role, cycle through options instead of keyboard
    if (gOLEDEspNowState.settingsEditField == 2) {
      // Cycle: Worker -> Master -> Backup -> Worker
      if (gSettings.meshRole == MESH_ROLE_WORKER) {
        gSettings.meshRole = MESH_ROLE_MASTER;
      } else if (gSettings.meshRole == MESH_ROLE_MASTER) {
        gSettings.meshRole = MESH_ROLE_BACKUP_MASTER;
      } else {
        gSettings.meshRole = MESH_ROLE_WORKER;
      }
      writeSettingsJson();
      gOLEDEspNowState.settingsEditField = -1;
      return true;
    }
    
    // For other fields, open keyboard
    const char* prompt = espnowSettingsLabels[gOLEDEspNowState.settingsEditField];
    String initialValue = "";
    int maxLen = 32;
    
    switch (gOLEDEspNowState.settingsEditField) {
      case 0: // Device Name
        initialValue = gSettings.espnowDeviceName;
        maxLen = 16;
        break;
      case 1: // Passphrase
        initialValue = "";  // Don't show existing passphrase
        maxLen = 32;
        break;
      case 3: // Master MAC
        initialValue = gSettings.meshMasterMAC;
        maxLen = 17;  // XX:XX:XX:XX:XX:XX
        break;
      case 4: // Backup MAC
        initialValue = gSettings.meshBackupMAC;
        maxLen = 17;
        break;
    }
    
    oledKeyboardInit(prompt, initialValue.c_str(), maxLen);
    gOLEDEspNowState.currentView = ESPNOW_VIEW_SETTINGS_KEYBOARD;
    return true;
  }
  
  // B button: Back to device list
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    gOLEDEspNowState.currentView = ESPNOW_VIEW_DEVICE_LIST;
    return true;
  }
  
  return false;
}

void oledEspNowApplySettingsEdit(const String& value) {
  switch (gOLEDEspNowState.settingsEditField) {
    case 0: // Device Name
      gSettings.espnowDeviceName = value;
      break;
    case 1: // Passphrase
      if (value.length() > 0) {
        gSettings.espnowPassphrase = value;
        // Re-derive encryption key if ESP-NOW is initialized
        if (gEspNow && gEspNow->initialized) {
          deriveKeyFromPassphrase(value, gEspNow->derivedKey);
        }
      }
      break;
    case 3: // Master MAC
      gSettings.meshMasterMAC = value;
      break;
    case 4: // Backup MAC
      gSettings.meshBackupMAC = value;
      break;
  }
  
  // Save settings
  writeSettingsJson();
  gOLEDEspNowState.settingsEditField = -1;
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW
