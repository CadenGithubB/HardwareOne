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
#include "OLED_Utils.h"
#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Utils.h"

// I2C address for OLED (must match OLED_Display.cpp)
#ifndef OLED_I2C_ADDRESS
#define OLED_I2C_ADDRESS 0x3D
#endif

// Helper macro to wrap OLED operations in I2C transaction
#define OLED_TRANSACTION(code) \
  i2cDeviceTransactionVoid(OLED_I2C_ADDRESS, 100000, 50, [&]() { code; })

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw.h"  // For JOYSTICK_DEADZONE
#endif

#if ENABLE_WIFI
#include <WiFi.h>
#endif

// External references
extern Adafruit_SSD1306* oledDisplay;
extern bool oledConnected;
extern bool oledEnabled;
extern String waitForSerialInputBlocking();
extern void updateOLEDDisplay();

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Check if OLED is available for interactive input
 */
static bool isOLEDAvailable() {
  return oledDisplay && oledConnected && oledEnabled;
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
                        const char* initialText, int maxLength) {
  // Fallback to serial if OLED not available
  if (!isOLEDAvailable()) {
    Serial.print(prompt);
    Serial.print(": ");
    return waitForSerialInputBlocking();
  }

  // Initialize keyboard
  oledKeyboardInit(prompt, initialText, maxLength);
  
  // Store original keyboard state to modify for password mode
  bool originalActive = true;
  
  while (oledKeyboardIsActive()) {
    // Check for serial input first (non-blocking)
    if (Serial.available()) {
      String serialInput = Serial.readStringUntil('\n');
      serialInput.trim();
      if (serialInput.length() > 0) {
        // Serial input received - submit it as if user pressed Enter on keyboard
        // Copy the serial input into the keyboard state
        strncpy(gOLEDKeyboardState.text, serialInput.c_str(), OLED_KEYBOARD_MAX_LENGTH);
        gOLEDKeyboardState.textLength = min((int)serialInput.length(), OLED_KEYBOARD_MAX_LENGTH);
        gOLEDKeyboardState.text[gOLEDKeyboardState.textLength] = '\0';
        
        // Complete the keyboard (same as pressing Enter)
        oledKeyboardComplete();
        broadcastOutput(serialInput);  // Echo back to serial
        
        // Break immediately - keyboard is now inactive
        break;
      }
    }
    
    // Only update display and handle input if keyboard is still active
    if (!oledKeyboardIsActive()) {
      break;
    }
    
    // Clear display and render (wrapped in I2C transaction)
    OLED_TRANSACTION(
      oledDisplay->clearDisplay();
      
      // If password mode, temporarily modify the displayed text
      if (isPassword && gOLEDKeyboardState.textLength > 0) {
        // Save original text
        char originalText[OLED_KEYBOARD_MAX_LENGTH + 1];
        strncpy(originalText, gOLEDKeyboardState.text, sizeof(originalText));
        
        // Replace with asterisks for display
        for (int i = 0; i < gOLEDKeyboardState.textLength; i++) {
          gOLEDKeyboardState.text[i] = '*';
        }
        
        // Display keyboard with masked text
        oledKeyboardDisplay(oledDisplay);
        
        // Restore original text
        strncpy(gOLEDKeyboardState.text, originalText, sizeof(gOLEDKeyboardState.text));
      } else {
        // Normal display
        oledKeyboardDisplay(oledDisplay);
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
      
      // Instructions
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      oledDisplay->setCursor(0, 52);
      oledDisplay->print("L/R:Move A:OK");
      
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

bool getOLEDWiFiSelection(String& outSSID) {
  // Fallback to serial if OLED not available
  if (!isOLEDAvailable()) {
    Serial.println("Enter WiFi SSID (or press Enter to skip): ");
    outSSID = waitForSerialInputBlocking();
    outSSID.trim();
    return (outSSID.length() > 0);
  }

#if ENABLE_WIFI
  // Scan for networks
  OLED_TRANSACTION(
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 0);
    oledDisplay->print("Scanning WiFi...");
    oledDisplay->display();
  );
  
  WiFi.mode(WIFI_STA);
  int networkCount = WiFi.scanNetworks();
  
  if (networkCount == 0) {
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
    
    uint32_t pressed = waitForButtonPress();
    if (INPUT_CHECK(pressed, INPUT_BUTTON_A)) {
      return getOLEDWiFiSelection(outSSID);  // Retry
    }
    return false;  // Skip
  }
  
  // Build network list (limit to 20 networks)
  const int maxNetworks = 20;
  String networks[maxNetworks];
  int displayCount = min(networkCount, maxNetworks);
  
  for (int i = 0; i < displayCount; i++) {
    networks[i] = WiFi.SSID(i);
    // Add signal strength indicator
    int rssi = WiFi.RSSI(i);
    if (rssi > -50) networks[i] += " +++";
    else if (rssi > -70) networks[i] += " ++";
    else networks[i] += " +";
  }
  
  // Add "Skip" option at the end
  if (displayCount < maxNetworks) {
    networks[displayCount] = "< Skip WiFi Setup >";
    displayCount++;
  }
  
  // Show selection menu
  int selection = 0;
  int scrollOffset = 0;
  bool confirmed = false;
  
  while (!confirmed) {
    // Adjust scroll to keep selection visible
    const int maxVisible = 5;
    if (selection < scrollOffset) {
      scrollOffset = selection;
    } else if (selection >= scrollOffset + maxVisible) {
      scrollOffset = selection - maxVisible + 1;
    }
    
    // Render WiFi selection menu (wrapped in I2C transaction)
    OLED_TRANSACTION(
      oledDisplay->clearDisplay();
      oledDisplay->setTextSize(1);
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      
      // Title
      oledDisplay->setCursor(0, 0);
      oledDisplay->print("Select WiFi:");
      
      // Calculate visible items (5 lines, 10px each = 50px, starting at y=12)
      const int itemHeight = 10;
      const int startY = 12;
      
      // Draw visible items
      for (int i = 0; i < maxVisible && (scrollOffset + i) < displayCount; i++) {
        int idx = scrollOffset + i;
        int y = startY + i * itemHeight;
        
        if (idx == selection) {
          oledDisplay->fillRect(0, y - 1, 128, itemHeight, DISPLAY_COLOR_WHITE);
          oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
        } else {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        }
        
        oledDisplay->setCursor(2, y);
        // Truncate long SSIDs
        String displayName = networks[idx];
        if (displayName.length() > 20) {
          displayName = displayName.substring(0, 17) + "...";
        }
        oledDisplay->print(displayName);
      }
      
      // Scroll indicators
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
    
    // Check for serial input first (non-blocking)
    if (Serial.available()) {
      String serialInput = Serial.readStringUntil('\n');
      serialInput.trim();
      if (serialInput.length() > 0) {
        // Check if it's a skip command
        if (serialInput.equalsIgnoreCase("skip") || serialInput.length() == 0) {
          broadcastOutput("Skipping WiFi setup");
          return false;
        }
        // Otherwise treat as SSID
        outSSID = serialInput;
        broadcastOutput(serialInput);
        return true;
      }
    }
    
    // Handle input
    updateInputState();
    int deltaX, deltaY;
    getJoystickDelta(deltaX, deltaY);
    uint32_t newlyPressed = getNewlyPressedButtons();
    
    // Up/Down to navigate
    if (deltaY < -JOYSTICK_DEADZONE) {
      if (selection > 0) selection--;
      delay(150);
    } else if (deltaY > JOYSTICK_DEADZONE) {
      if (selection < displayCount - 1) selection++;
      delay(150);
    }
    
    // A button to confirm
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      confirmed = true;
    }
    
    // B button to cancel/skip
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      return false;
    }
    
    delay(50);
  }
  
  // Check if user selected "Skip"
  if (networks[selection].startsWith("< Skip")) {
    return false;
  }
  
  // Extract SSID (remove signal strength indicator)
  outSSID = networks[selection];
  int plusIdx = outSSID.lastIndexOf(" +");
  if (plusIdx > 0) {
    outSSID = outSSID.substring(0, plusIdx);
  }
  outSSID.trim();
  
  return true;
  
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
      oledDisplay->setCursor(0, 52);
      oledDisplay->print("Press A to continue");
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
