/**
 * @file oled_first_time_setup.cpp
 * @brief OLED-based UI for first-time device setup
 * 
 * Provides interactive setup screens using OLED display and gamepad/joystick input.
 * Falls back to serial console if OLED is not available.
 */

#include "OLED_FirstTimeSetup.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>

#include "OLED_Display.h"
#include "OLED_Footer.h"
#include "OLED_Utils.h"
#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Utils.h"

// OLED_I2C_ADDRESS and OLED_TRANSACTION defined in OLED_Display.h

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"  // For JOYSTICK_DEADZONE
#endif

#if ENABLE_WIFI
#include <esp_attr.h>
#include "System_WiFi.h"
#endif

// External references
extern bool gOledRunning;
extern String waitForSerialInputBlocking();
extern void updateOLEDDisplay();

// Draw FTS footer at the standard position (matching drawOLEDFooter in the normal program)
static void drawFTSFooter(const char* text) {
  const int footerY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
  oledDisplay->drawFastHLine(0, footerY, 128, SSD1306_WHITE);
  oledDisplay->setCursor(0, footerY + 2);
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->print(text);
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Check if OLED is available for interactive input
 */
static bool isOLEDAvailable() {
  return oledDisplay && oledConnected && gOledRunning;
}

/**
 * Wait for any button press and return which button was pressed
 */
static uint32_t waitForButtonPress() {
  uint32_t pressed = 0;
  while (pressed == 0) {
    updateInputState();
    pressed = getNewlyPressedButtons();
    delay(10);
  }
  return pressed;
}

// ============================================================================
// OLED Text Input (with Virtual Keyboard)
// ============================================================================

String getOLEDTextInput(const char* prompt, bool isPassword,
                        const char* initialText, int maxLength,
                        bool* wasCancelled, bool canSkip) {
  // Initialize output parameter
  if (wasCancelled) *wasCancelled = false;
  
  // Fallback to serial if OLED not available
  if (!isOLEDAvailable()) {
    Serial.print(prompt);
    Serial.print(": ");
    return waitForSerialInputBlocking();
  }

  // Initialize keyboard
  oledKeyboardInit(prompt, initialText, maxLength);

  // Also echo prompt to serial so users monitoring via terminal know to type here
  Serial.print(prompt);
  if (canSkip) {
    Serial.println(" (type here, 'n' to skip, or use OLED keyboard):");
  } else {
    Serial.println(" (type here or use OLED keyboard):");
  }
  
  // Store original keyboard state to modify for password mode
  bool originalActive = true;
  
  while (oledKeyboardIsActive()) {
    // Check for serial input first (non-blocking)
    if (Serial.available()) {
      String serialInput = Serial.readStringUntil('\n');
      serialInput.trim();
      // 'n' = skip this field (leave blank) — only if skipping is allowed
      if (canSkip && serialInput.equalsIgnoreCase("n")) serialInput = "";
      // 'b' = cancel (if caller supports wasCancelled)
      if (serialInput.equalsIgnoreCase("b") || serialInput.equalsIgnoreCase("back")) {
        gOledKeyboardState.textLength = 0;
        gOledKeyboardState.text[0] = '\0';
        oledKeyboardComplete();
        if (wasCancelled) *wasCancelled = true;
        break;
      }
      strncpy(gOledKeyboardState.text, serialInput.c_str(), OLED_KEYBOARD_MAX_LENGTH);
      gOledKeyboardState.textLength = min((int)serialInput.length(), OLED_KEYBOARD_MAX_LENGTH);
      gOledKeyboardState.text[gOledKeyboardState.textLength] = '\0';
      oledKeyboardComplete();
      if (serialInput.length() > 0 && !isPassword) broadcastOutput(serialInput);
      break;
    }
    
    // Only update display and handle input if keyboard is still active
    if (!oledKeyboardIsActive()) {
      break;
    }
    
    // Clear display and render (wrapped in I2C transaction)
    OLED_TRANSACTION(
      oledDisplay->clearDisplay();
      
      // If password mode, temporarily modify the displayed text
      if (isPassword && gOledKeyboardState.textLength > 0) {
        // Save original text
        char originalText[OLED_KEYBOARD_MAX_LENGTH + 1];
        strncpy(originalText, gOledKeyboardState.text, sizeof(originalText));
        
        // Replace with asterisks for display
        for (int i = 0; i < gOledKeyboardState.textLength; i++) {
          gOledKeyboardState.text[i] = '*';
        }
        
        // Display keyboard with masked text
        oledKeyboardDisplay(oledDisplay);
        
        // Restore original text
        strncpy(gOledKeyboardState.text, originalText, sizeof(gOledKeyboardState.text));
      } else {
        // Normal display
        oledKeyboardDisplay(oledDisplay);
      }
      
      // Draw keyboard footer matching main keyboard style
      {
        const int ftrY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
        oledDisplay->drawFastHLine(0, ftrY, 128, SSD1306_WHITE);
        oledDisplay->setTextSize(1);
        oledDisplay->setTextColor(SSD1306_WHITE);
        oledDisplay->setCursor(0, ftrY + 2);
        oledDisplay->print("A:Sel Y:Del B:");
        oledDrawBackArrowIcon(oledDisplay, ftrY + 2);
        oledDisplay->print(" S:OK");
      }
      
      oledDisplay->display();
    );
    
    // Handle input
    updateInputState();
    int deltaX, deltaY;
    getJoystickDelta(deltaX, deltaY);
    uint32_t newlyPressed = getNewlyPressedButtons();
    
    oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
    
    delay(50);
  }
  
  // Clear the display after keyboard exits
  OLED_TRANSACTION(
    oledDisplay->clearDisplay();
    oledDisplay->display();
  );
  
  // Check if cancelled
  if (oledKeyboardIsCancelled()) {
    if (wasCancelled) *wasCancelled = true;
    oledKeyboardReset();
    return "";
  }
  
  // Get result
  String result = String(oledKeyboardGetText());
  oledKeyboardReset();
  
  return result;
}

// ============================================================================
// OLED Yes/No Prompt
// ============================================================================

bool getOLEDYesNoPrompt(const char* prompt, bool defaultYes) {
  // Fallback to serial if OLED not available
  if (!isOLEDAvailable()) {
    Serial.print(prompt);
    Serial.print(" (y/n) [default: ");
    Serial.print(defaultYes ? "y" : "n");
    Serial.print("]: ");
    
    String response = waitForSerialInputBlocking();
    response.trim();
    response.toLowerCase();
    
    if (response.length() == 0) {
      return defaultYes;
    }
    return (response == "y" || response == "yes");
  }

  // OLED UI
  int selection = defaultYes ? 0 : 1;  // 0 = Yes, 1 = No
  bool confirmed = false;
  
  while (!confirmed) {
    // Render Yes/No dialog (wrapped in I2C transaction)
    OLED_TRANSACTION(
      oledDisplay->clearDisplay();
      oledDisplay->setTextSize(1);
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      
      // Draw prompt - handle explicit \n newlines first, then word wrap each line
      oledDisplay->setCursor(0, 0);
      String promptStr = String(prompt);
      int lineY = 0;
      int lineStart = 0;
      
      while (lineStart < (int)promptStr.length() && lineY < 30) {
        // Find next newline or end of string
        int nlIdx = promptStr.indexOf('\n', lineStart);
        int lineEnd = (nlIdx >= 0) ? nlIdx : promptStr.length();
        String segment = promptStr.substring(lineStart, lineEnd);
        
        // Word-wrap this segment if needed (21 chars per line at size 1)
        int segStart = 0;
        while (segStart < (int)segment.length() && lineY < 30) {
          int segEnd = segStart + 21;
          if (segEnd > (int)segment.length()) segEnd = segment.length();
          
          // Try to break at space if not at end
          if (segEnd < (int)segment.length()) {
            int spaceIdx = segment.lastIndexOf(' ', segEnd);
            if (spaceIdx > segStart) segEnd = spaceIdx;
          }
          
          oledDisplay->setCursor(0, lineY);
          oledDisplay->print(segment.substring(segStart, segEnd));
          lineY += 10;
          segStart = segEnd;
          if (segStart < (int)segment.length() && segment.charAt(segStart) == ' ') segStart++;
        }
        
        lineStart = lineEnd + 1;  // Skip past the newline
      }
      
      // Draw options
      int optionY = 35;
      
      // Yes option
      if (selection == 0) {
        oledDisplay->fillRect(10, optionY - 2, 40, 12, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      }
      oledDisplay->setCursor(20, optionY);
      oledDisplay->print("Yes");
      
      // No option
      if (selection == 1) {
        oledDisplay->fillRect(70, optionY - 2, 40, 12, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      }
      oledDisplay->setCursor(82, optionY);
      oledDisplay->print("No");
      
      // Footer at standard position
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      drawFTSFooter("L/R:Move A:OK");
      
      oledDisplay->display();
    );
    
    // Check for serial input first (non-blocking)
    if (Serial.available()) {
      String serialInput = Serial.readStringUntil('\n');
      serialInput.trim();
      serialInput.toLowerCase();
      if (serialInput.startsWith("y")) {
        broadcastOutput("yes");
        return true;
      } else if (serialInput.startsWith("n")) {
        broadcastOutput("no");
        return false;
      }
    }
    
    // Handle input
    updateInputState();
    int deltaX, deltaY;
    getJoystickDelta(deltaX, deltaY);
    uint32_t newlyPressed = getNewlyPressedButtons();
    
    // Left/Right to change selection
    if (deltaX < -JOYSTICK_DEADZONE) {
      selection = 0;  // Yes
      delay(200);
    } else if (deltaX > JOYSTICK_DEADZONE) {
      selection = 1;  // No
      delay(200);
    }
    
    // A button to confirm
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      confirmed = true;
    }
    
    delay(50);
  }
  
  return (selection == 0);
}

// ============================================================================
// OLED WiFi Selection
// ============================================================================

#if ENABLE_WIFI
// The setup picker can wait indefinitely for a person. Keep only copied SSID
// metadata in PSRAM during that wait; Arduino/IDF scan-result storage is owned
// and released by wifiScanForEach before any input loop starts.
static constexpr int FTS_WIFI_MENU_MAX = 20;
static constexpr int FTS_WIFI_MENU_NAMED_MAX = FTS_WIFI_MENU_MAX - 3;
// Retain exactly the named entries the OLED can display. Additional APs are
// counted and can still be joined by typing the exact SSID; they do not consume
// a hidden, non-discoverable numeric slot.
static constexpr uint16_t FTS_WIFI_SCAN_CACHE_MAX =
    FTS_WIFI_MENU_NAMED_MAX;

struct FtsWifiScanEntry {
  char ssid[33];
  int8_t rssi;
};

EXT_RAM_BSS_ATTR static FtsWifiScanEntry
    sFtsWifiScanEntries[FTS_WIFI_SCAN_CACHE_MAX];
EXT_RAM_BSS_ATTR static char
    sFtsWifiMenuLabels[FTS_WIFI_MENU_MAX][40];
static uint16_t sFtsWifiStoredCount = 0;

static void clearFtsWifiScanCache() {
  memset(sFtsWifiScanEntries, 0, sizeof(sFtsWifiScanEntries));
  memset(sFtsWifiMenuLabels, 0, sizeof(sFtsWifiMenuLabels));
  sFtsWifiStoredCount = 0;
}

struct FtsWifiScanContext {
  uint16_t named = 0;
  uint16_t hidden = 0;
};

static bool cacheFtsWifiScanRecord(const WifiScanRecord& record,
                                   uint16_t /*index*/, uint16_t /*total*/,
                                   void* opaque) {
  FtsWifiScanContext* context =
      static_cast<FtsWifiScanContext*>(opaque);
  if (record.ssid[0] == '\0') {
    ++context->hidden;
    return true;
  }

  ++context->named;
  if (sFtsWifiStoredCount < FTS_WIFI_SCAN_CACHE_MAX) {
    FtsWifiScanEntry& entry = sFtsWifiScanEntries[sFtsWifiStoredCount++];
    memcpy(entry.ssid, record.ssid, sizeof(entry.ssid));
    entry.ssid[32] = '\0';
    entry.rssi = record.rssi;
  }
  return true;  // keep counting named/hidden APs even after the copy cap
}

struct FtsWifiScanCacheGuard {
  ~FtsWifiScanCacheGuard() { clearFtsWifiScanCache(); }
};
#endif

bool getOLEDWiFiSelection(String& outSSID) {
  // Fallback to serial if OLED not available
  if (!isOLEDAvailable()) {
    Serial.println("Enter WiFi SSID (or press Enter to skip): ");
    outSSID = waitForSerialInputBlocking();
    outSSID.trim();
    return (outSSID.length() > 0);
  }

#if ENABLE_WIFI
  FtsWifiScanCacheGuard cacheGuard;
  for (;;) {  // rescan/manual-back return here without recursive stack growth
    clearFtsWifiScanCache();

    OLED_TRANSACTION(
      oledDisplay->clearDisplay();
      oledDisplay->setTextSize(1);
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      oledDisplay->setCursor(0, 0);
      oledDisplay->print("Scanning WiFi...");
      oledDisplay->display();
    );

    FtsWifiScanContext scanContext;
    const WifiScanResult scanResult = wifiScanForEach(
        /*includeHidden=*/true, cacheFtsWifiScanRecord, &scanContext,
        /*acquireTimeoutMs=*/1000);

    if (!scanResult.ok()) {
      const char* status = wifiScanStatusText(scanResult.status);
      Serial.printf("WiFi scan %s (driver=%ld). Press A to retry or B to skip.\n",
                    status, (long)scanResult.driverError);
      OLED_TRANSACTION(
        oledDisplay->clearDisplay();
        oledDisplay->setTextSize(1);
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(0, 0);
        oledDisplay->print("WiFi scan ");
        oledDisplay->println(status);
        oledDisplay->setCursor(0, 20);
        oledDisplay->print("Press A to retry");
        oledDisplay->setCursor(0, 30);
        oledDisplay->print("Press B to skip");
        oledDisplay->display();
      );
      const uint32_t pressed = waitForButtonPress();
      if (INPUT_CHECK(pressed, INPUT_BUTTON_A)) continue;
      return false;
    }

    // Number named networks only. Hidden entries never consume a displayed or
    // serial selection number, so a numeric pick maps to the copied SSID that
    // was printed even after the native scan storage has been released.
    Serial.printf("Found %u networks:\n", (unsigned)scanResult.found);
    const uint16_t serialShown = sFtsWifiStoredCount;
    for (uint16_t i = 0; i < serialShown; ++i) {
      Serial.printf("  %u. %-24s  %lddBm\n", (unsigned)(i + 1),
                    sFtsWifiScanEntries[i].ssid,
                    (long)sFtsWifiScanEntries[i].rssi);
    }
    if (scanContext.named > serialShown) {
      Serial.printf("  ... and %u more named\n",
                    (unsigned)(scanContext.named - serialShown));
    }
    if (scanContext.hidden > 0) {
      Serial.printf("  (+%u hidden network%s — type the exact SSID to join)\n",
                    (unsigned)scanContext.hidden,
                    scanContext.hidden == 1 ? "" : "s");
    }
    Serial.println("Enter a number to select, type an SSID directly, 'rescan' to refresh, or 'skip':");
    Serial.print("> ");

    if (scanResult.found == 0) {
      OLED_TRANSACTION(
        oledDisplay->clearDisplay();
        oledDisplay->setCursor(0, 0);
        oledDisplay->print("No networks found");
        oledDisplay->setCursor(0, 20);
        oledDisplay->print("Press A to retry");
        oledDisplay->setCursor(0, 30);
        oledDisplay->print("Press B to skip");
        oledDisplay->display();
      );
      const uint32_t pressed = waitForButtonPress();
      if (INPUT_CHECK(pressed, INPUT_BUTTON_A)) continue;
      return false;
    }

    const int namedMenuCount =
        sFtsWifiStoredCount < FTS_WIFI_MENU_NAMED_MAX
            ? sFtsWifiStoredCount : FTS_WIFI_MENU_NAMED_MAX;
    int displayCount = namedMenuCount;
    for (int i = 0; i < namedMenuCount; ++i) {
      const int rssi = sFtsWifiScanEntries[i].rssi;
      const char* bars = (rssi > -50) ? " +++"
                         : (rssi > -70) ? " ++" : " +";
      snprintf(sFtsWifiMenuLabels[i], sizeof(sFtsWifiMenuLabels[i]),
               "%.32s%s", sFtsWifiScanEntries[i].ssid, bars);
    }

    int hiddenLabelIdx = -1;
    if (scanContext.hidden > 0 && displayCount < FTS_WIFI_MENU_MAX) {
      hiddenLabelIdx = displayCount;
      snprintf(sFtsWifiMenuLabels[displayCount],
               sizeof(sFtsWifiMenuLabels[displayCount]),
               "  %u Hidden Network%s", (unsigned)scanContext.hidden,
               scanContext.hidden == 1 ? "" : "s");
      ++displayCount;
    }

    const int rescanIdx = displayCount;
    snprintf(sFtsWifiMenuLabels[displayCount++],
             sizeof(sFtsWifiMenuLabels[0]), "< Rescan WiFi >");
    const int manualIdx = displayCount;
    snprintf(sFtsWifiMenuLabels[displayCount++],
             sizeof(sFtsWifiMenuLabels[0]), "< Manual Entry >");

    int selection = 0;
    int scrollOffset = 0;
    bool confirmed = false;
    bool rescanRequested = false;

    while (!confirmed) {
      const int maxVisible = 5;
      if (selection < scrollOffset) {
        scrollOffset = selection;
      } else if (selection >= scrollOffset + maxVisible) {
        scrollOffset = selection - maxVisible + 1;
      }

      OLED_TRANSACTION(
        oledDisplay->clearDisplay();
        oledDisplay->setTextSize(1);
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(0, 0);
        oledDisplay->print("Select WiFi:");

        const int itemHeight = 10;
        const int startY = 12;
        for (int i = 0;
             i < maxVisible && (scrollOffset + i) < displayCount; ++i) {
          const int idx = scrollOffset + i;
          const int y = startY + i * itemHeight;
          if (idx == selection) {
            oledDisplay->fillRect(0, y - 1, 128, itemHeight,
                                  DISPLAY_COLOR_WHITE);
            oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
          } else {
            oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          }

          oledDisplay->setCursor(2, y);
          char displayName[21];
          const size_t labelLength = strlen(sFtsWifiMenuLabels[idx]);
          if (labelLength > 20) {
            memcpy(displayName, sFtsWifiMenuLabels[idx], 17);
            memcpy(displayName + 17, "...", 4);
          } else {
            strncpy(displayName, sFtsWifiMenuLabels[idx],
                    sizeof(displayName) - 1);
            displayName[sizeof(displayName) - 1] = '\0';
          }
          oledDisplay->print(displayName);
        }

        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        if (scrollOffset > 0) {
          oledDisplay->setCursor(120, 12);
          oledDisplay->print("^");
        }
        if (scrollOffset + maxVisible < displayCount) {
          oledDisplay->setCursor(120, 52);
          oledDisplay->print("v");
        }
        oledDisplay->display();
      );

      if (Serial.available()) {
        String serialInput = Serial.readStringUntil('\n');
        serialInput.trim();
        if (serialInput.length() > 0) {
          if (serialInput.equalsIgnoreCase("skip")) {
            broadcastOutput("Skipping WiFi setup");
            return false;
          }
          if (serialInput.equalsIgnoreCase("rescan")) {
            broadcastOutput("Rescanning WiFi...");
            rescanRequested = true;
            break;
          }

          const int idx = serialInput.toInt();
          const bool isPureNumber =
              idx >= 1 && String(idx) == serialInput;
          if (isPureNumber) {
            if (idx > sFtsWifiStoredCount) {
              // A bare unmatched number is a typo, not a literal numeric SSID.
              // Digit-leading non-numbers (for example "4G-home") remain valid
              // exact SSIDs through the branch below.
              Serial.printf("'%s' isn't in the list (1-%u). Pick again, type an exact SSID, or 'skip'.\n> ",
                            serialInput.c_str(),
                            (unsigned)sFtsWifiStoredCount);
              continue;
            }
            outSSID = sFtsWifiScanEntries[idx - 1].ssid;
          } else {
            outSSID = serialInput;
          }
          broadcastOutput(outSSID);
          return true;
        }
      }

      updateInputState();
      int deltaX, deltaY;
      getJoystickDelta(deltaX, deltaY);
      const uint32_t newlyPressed = getNewlyPressedButtons();

      if (deltaY < -JOYSTICK_DEADZONE) {
        if (selection > 0) --selection;
        delay(150);
      } else if (deltaY > JOYSTICK_DEADZONE) {
        if (selection < displayCount - 1) ++selection;
        delay(150);
      }

      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        if (selection == hiddenLabelIdx) {
          if (selection < displayCount - 1) ++selection;
        } else {
          confirmed = true;
        }
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) return false;
      delay(50);
    }

    if (rescanRequested || selection == rescanIdx) continue;
    if (selection == manualIdx) {
      bool cancelled = false;
      outSSID = getOLEDTextInput("WiFi SSID:", false, "", 32,
                                 &cancelled);
      if (cancelled || outSSID.length() == 0) continue;
      return true;
    }

    if (selection >= 0 && selection < namedMenuCount) {
      outSSID = sFtsWifiScanEntries[selection].ssid;
      outSSID.trim();
      return outSSID.length() > 0;
    }
  }

#else
  // WiFi disabled at compile time
  OLED_TRANSACTION(
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 0);
    oledDisplay->print("WiFi disabled");
    oledDisplay->setCursor(0, 10);
    oledDisplay->print("at compile time");
    oledDisplay->setCursor(0, 30);
    oledDisplay->print("Press A to continue");
    oledDisplay->display();
  );
  waitForButtonPress();
  return false;
#endif
}

// ============================================================================
// OLED Message Display
// ============================================================================

void showOLEDMessage(const char* message, bool waitForButton) {
  if (!isOLEDAvailable()) {
    Serial.println(message);
    if (waitForButton) {
      Serial.println("Press Enter to continue...");
      waitForSerialInputBlocking();
    }
    return;
  }

  OLED_TRANSACTION(
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    
    // Word wrap message
    String msg = String(message);
    int lineY = 10;
    int startIdx = 0;
    
    while (startIdx < (int)msg.length() && lineY < 54) {
      int endIdx = startIdx + 21;  // ~21 chars per line
      if (endIdx > (int)msg.length()) endIdx = msg.length();
      
      // Try to break at space or newline
      int newlineIdx = msg.indexOf('\n', startIdx);
      if (newlineIdx >= 0 && newlineIdx < endIdx) {
        endIdx = newlineIdx;
      } else if (endIdx < (int)msg.length()) {
        int spaceIdx = msg.lastIndexOf(' ', endIdx);
        if (spaceIdx > startIdx) endIdx = spaceIdx;
      }
      
      oledDisplay->setCursor(0, lineY);
      oledDisplay->print(msg.substring(startIdx, endIdx));
      lineY += 10;
      startIdx = endIdx + 1;
    }
    
    if (waitForButton) {
      drawFTSFooter("A:Continue");
    }
    
    oledDisplay->display();
  );
  
  if (waitForButton) {
    while (true) {
      updateInputState();
      uint32_t pressed = getNewlyPressedButtons();
      if (INPUT_CHECK(pressed, INPUT_BUTTON_A)) {
        break;
      }
      delay(50);
    }
  }
}

#endif // ENABLE_OLED_DISPLAY
