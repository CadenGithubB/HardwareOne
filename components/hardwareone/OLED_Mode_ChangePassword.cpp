// ============================================================================
// OLED Change Password Mode
// ============================================================================
// Allows authenticated users to change their password via OLED interface

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "System_Debug.h"
#include "System_User.h"
#include "System_Utils.h"

extern DisplayDriver* oledDisplay;
extern bool oledConnected;

// ============================================================================
// Password Change State
// ============================================================================

enum PasswordChangeField {
  FIELD_CURRENT_PASSWORD,
  FIELD_NEW_PASSWORD,
  FIELD_CONFIRM_PASSWORD,
  FIELD_CHANGE_BUTTON
};

static PasswordChangeField currentField = FIELD_CURRENT_PASSWORD;
static String currentPassBuffer = "";
static String newPassBuffer = "";
static String confirmPassBuffer = "";
static String errorMessage = "";
static unsigned long errorDisplayUntil = 0;
static bool changePasswordKeyboardActive = false;
static bool passwordChangeInProgress = false;

// ============================================================================
// Display Function
// ============================================================================

static void displayChangePasswordMode() {
  if (!oledDisplay) {
    ERROR_SYSTEMF("[CHANGE_PASSWORD] FATAL: oledDisplay is null!");
    return;
  }
  
  oledDisplay->clearDisplay();
  
  // Header
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  oledDisplay->setTextSize(1);
  oledDisplay->println("Change Password");
  oledDisplay->drawFastHLine(0, 9, 128, SSD1306_WHITE);

  // Guard: must be authenticated to change password
  if (!isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Not logged in.");
    oledDisplay->println();
    oledDisplay->println("Enable display auth");
    oledDisplay->println("in Output settings,");
    oledDisplay->println("then log in first.");
    return;
  }

  // Show current user
  String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
  int yPos = OLED_CONTENT_START_Y;
  oledDisplay->setCursor(0, yPos);
  oledDisplay->print("User: ");
  oledDisplay->println(currentUser);
  yPos += 10;
  
  // Four fields with compact spacing
  const int fieldSpacing = 11;
  const int fieldY1 = yPos;
  const int fieldY2 = fieldY1 + fieldSpacing;
  const int fieldY3 = fieldY2 + fieldSpacing;
  const int fieldY4 = fieldY3 + fieldSpacing;
  
  // Current password field
  if (currentField == FIELD_CURRENT_PASSWORD) {
    oledDisplay->fillRect(0, fieldY1, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY1);
  oledDisplay->print(currentField == FIELD_CURRENT_PASSWORD ? ">" : " ");
  oledDisplay->print("Current: ");
  if (currentPassBuffer.length() > 0) {
    for (size_t i = 0; i < min(currentPassBuffer.length(), (size_t)6); i++) {
      oledDisplay->print("*");
    }
  } else {
    oledDisplay->print("___");
  }
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // New password field
  if (currentField == FIELD_NEW_PASSWORD) {
    oledDisplay->fillRect(0, fieldY2, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY2);
  oledDisplay->print(currentField == FIELD_NEW_PASSWORD ? ">" : " ");
  oledDisplay->print("New: ");
  if (newPassBuffer.length() > 0) {
    for (size_t i = 0; i < min(newPassBuffer.length(), (size_t)9); i++) {
      oledDisplay->print("*");
    }
    // Show strength indicator
    if (newPassBuffer.length() >= 8) {
      oledDisplay->print(" +");
    }
  } else {
    oledDisplay->print("___");
  }
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Confirm password field
  if (currentField == FIELD_CONFIRM_PASSWORD) {
    oledDisplay->fillRect(0, fieldY3, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY3);
  oledDisplay->print(currentField == FIELD_CONFIRM_PASSWORD ? ">" : " ");
  oledDisplay->print("Confirm: ");
  if (confirmPassBuffer.length() > 0) {
    for (size_t i = 0; i < min(confirmPassBuffer.length(), (size_t)6); i++) {
      oledDisplay->print("*");
    }
  } else {
    oledDisplay->print("___");
  }
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Change button
  if (currentField == FIELD_CHANGE_BUTTON) {
    oledDisplay->fillRect(0, fieldY4, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY4);
  oledDisplay->print(currentField == FIELD_CHANGE_BUTTON ? ">" : " ");
  oledDisplay->print(passwordChangeInProgress ? "[Changing...]" : "[Change Password]");
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Show error message if active - overlay with centered message box
  if (millis() < errorDisplayUntil && errorMessage.length() > 0) {
    const int boxX = 10;
    const int boxY = 20;
    const int boxW = 108;
    const int boxH = 24;
    
    oledDisplay->fillRect(boxX, boxY, boxW, boxH, DISPLAY_COLOR_BLACK);
    oledDisplay->drawRect(boxX, boxY, boxW, boxH, DISPLAY_COLOR_WHITE);
    
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(boxX + 4, boxY + 8);
    
    // Word wrap error message
    String msg = errorMessage;
    if (msg.length() > 16) {
      int space = msg.lastIndexOf(' ', 16);
      if (space > 0) {
        oledDisplay->println(msg.substring(0, space));
        oledDisplay->setCursor(boxX + 4, boxY + 16);
        oledDisplay->print(msg.substring(space + 1));
      } else {
        oledDisplay->print(msg.substring(0, 16));
      }
    } else {
      oledDisplay->print(msg);
    }
  }
}

// ============================================================================
// Input Handler
// ============================================================================

static bool handleChangePasswordModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Keyboard input handling
  if (changePasswordKeyboardActive) {
    if (oledKeyboardIsCompleted()) {
      const char* input = oledKeyboardGetText();
      if (currentField == FIELD_CURRENT_PASSWORD) {
        currentPassBuffer = String(input);
      } else if (currentField == FIELD_NEW_PASSWORD) {
        newPassBuffer = String(input);
      } else if (currentField == FIELD_CONFIRM_PASSWORD) {
        confirmPassBuffer = String(input);
      }
      oledKeyboardReset();
      changePasswordKeyboardActive = false;
      return true;
    } else if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      changePasswordKeyboardActive = false;
      return true;
    }
    return true;
  }
  
  // Don't accept input while password change is in progress
  if (passwordChangeInProgress) {
    return true;
  }
  
  bool handled = false;
  
  // Navigate between fields
  if (gNavEvents.down) {
    currentField = (PasswordChangeField)((currentField + 1) % 4);
    handled = true;
  } else if (gNavEvents.up) {
    currentField = (PasswordChangeField)((currentField + 3) % 4);
    handled = true;
  }
  
  // A button: Select field or change password
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (currentField == FIELD_CHANGE_BUTTON) {
      // Validate all fields are filled
      if (currentPassBuffer.length() == 0) {
        errorMessage = "Enter current pass";
        errorDisplayUntil = millis() + 2000;
        oledMarkDirtyUntil(errorDisplayUntil);
        return true;
      }
      if (newPassBuffer.length() < 6) {
        errorMessage = "New: min 6 chars";
        errorDisplayUntil = millis() + 2000;
        oledMarkDirtyUntil(errorDisplayUntil);
        return true;
      }
      if (confirmPassBuffer.length() == 0) {
        errorMessage = "Confirm password";
        errorDisplayUntil = millis() + 2000;
        oledMarkDirtyUntil(errorDisplayUntil);
        return true;
      }
      if (newPassBuffer != confirmPassBuffer) {
        errorMessage = "Passwords differ";
        errorDisplayUntil = millis() + 2000;
        oledMarkDirtyUntil(errorDisplayUntil);
        return true;
      }
      if (newPassBuffer == currentPassBuffer) {
        errorMessage = "New must differ";
        errorDisplayUntil = millis() + 2000;
        oledMarkDirtyUntil(errorDisplayUntil);
        return true;
      }
      
      // Execute password change command
      passwordChangeInProgress = true;
      String args = currentPassBuffer + " " + newPassBuffer + " " + confirmPassBuffer;
      const char* result = cmd_user_changepassword(args);
      
      passwordChangeInProgress = false;
      
      if (result && strstr(result, "Error") == NULL && strstr(result, "successfully") != NULL) {
        // Success
        errorMessage = "Password changed!";
        errorDisplayUntil = millis() + 2000;
        oledMarkDirtyUntil(errorDisplayUntil);
        
        // Clear buffers securely
        secureClearString(currentPassBuffer);
        secureClearString(newPassBuffer);
        secureClearString(confirmPassBuffer);
        currentField = FIELD_CURRENT_PASSWORD;
        
        // Return to menu after delay (no stack push — don't keep change-password in history)
        delay(2000);
        requestOLEDMode(OLED_MENU, "changepw.done", false);
        resetOLEDMenu();
      } else {
        // Error
        String errMsg = result ? String(result) : "Unknown error";
        if (errMsg.indexOf("Current password incorrect") >= 0) {
          errorMessage = "Wrong password";
        } else if (errMsg.indexOf("do not match") >= 0) {
          errorMessage = "Passwords differ";
        } else {
          errorMessage = "Change failed";
        }
        errorDisplayUntil = millis() + 3000;
        oledMarkDirtyUntil(errorDisplayUntil);
        
        // Clear only current password on error
        secureClearString(currentPassBuffer);
        currentField = FIELD_CURRENT_PASSWORD;
      }
      
    } else {
      // Launch keyboard for selected field
      const char* initialText = "";
      const char* title = "";
      
      if (currentField == FIELD_CURRENT_PASSWORD) {
        title = "Current Password:";
        initialText = currentPassBuffer.c_str();
      } else if (currentField == FIELD_NEW_PASSWORD) {
        title = "New Password:";
        initialText = newPassBuffer.c_str();
      } else if (currentField == FIELD_CONFIRM_PASSWORD) {
        title = "Confirm Password:";
        initialText = confirmPassBuffer.c_str();
      }
      
      oledKeyboardInit(title, initialText, 32);
      changePasswordKeyboardActive = true;
    }
    handled = true;
  }
  
  // B button: Cancel and return to menu
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    // Securely clear all buffers
    secureClearString(currentPassBuffer);
    secureClearString(newPassBuffer);
    secureClearString(confirmPassBuffer);
    currentField = FIELD_CURRENT_PASSWORD;
    errorMessage = "";
    // Let default handler process B button for mode change
  }
  
  return handled;
}

// ============================================================================
// Availability Check
// ============================================================================

static bool isChangePasswordModeAvailable(String* outReason) {
  if (!isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
    if (outReason) *outReason = "Not logged in";
    return false;
  }
  return true;
}

// ============================================================================
// Mode Registration
// ============================================================================

// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OLEDModeEntry changePasswordModeEntries[] = {
  {
    OLED_CHANGE_PASSWORD,
    "Change Password",
    "password",
    displayChangePasswordMode,
    isChangePasswordModeAvailable,
    handleChangePasswordModeInput,
    false,  // shown via sub-menu, not top-level
    -1,
    nullptr
  }
};

REGISTER_OLED_MODE_MODULE(changePasswordModeEntries, 1, "ChangePassword");

// Force linker to include this file - called from OLED_Utils.cpp
void oledChangePasswordModeInit() {}

#endif // ENABLE_OLED_DISPLAY
