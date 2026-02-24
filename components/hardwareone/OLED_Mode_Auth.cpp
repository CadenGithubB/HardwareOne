// ============================================================================
// OLED Auth Mode - Login and Logout
// ============================================================================
// Merged from OLED_Mode_Login.cpp and OLED_Mode_Logout.cpp for consistency
// with System_Auth naming convention

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "System_Auth.h"
#include "System_Utils.h"
#include "System_Notifications.h"
#include "System_Settings.h"
#include "i2csensor-seesaw.h"
#include "System_Debug.h"

// External OLED display pointer (provided via HAL_Display.h #define oledDisplay gDisplay)
extern OLEDMode currentOLEDMode;
extern void resetOLEDMenu();
extern void tryAutoStartGamepadForMenu();

// ============================================================================
// Login Mode
// ============================================================================

// Login state
enum LoginField {
  FIELD_USERNAME,
  FIELD_PASSWORD,
  FIELD_LOGIN_BUTTON
};

static LoginField currentField = FIELD_USERNAME;
static String usernameBuffer = "";
static String passwordBuffer = "";
static String errorMessage = "";
static unsigned long errorDisplayUntil = 0;
static bool loginKeyboardActive = false;

// Login display function
static void displayLoginMode() {
  if (!oledDisplay) {
    ERROR_SYSTEMF("[LOGIN_RENDER] FATAL: oledDisplay is null!");
    return;
  }
  
  // If keyboard is active, display it instead
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
    return;
  }
  
  // Content starts after global header - compact layout for 44px content area
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Show current user if switching sessions (optional context line)
  int yPos = OLED_CONTENT_START_Y;
  if (isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
    String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
    oledDisplay->setCursor(0, yPos);
    oledDisplay->print("User: ");
    oledDisplay->println(currentUser);
    yPos += 10;
  }
  
  // Three fields with compact spacing (12px each = 36px total, fits in 44px)
  const int fieldSpacing = 12;
  const int fieldY1 = OLED_CONTENT_START_Y + (isTransportAuthenticated(SOURCE_LOCAL_DISPLAY) ? 10 : 2);
  const int fieldY2 = fieldY1 + fieldSpacing;
  const int fieldY3 = fieldY2 + fieldSpacing;
  
  // Username field
  if (currentField == FIELD_USERNAME) {
    oledDisplay->fillRect(0, fieldY1, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY1);
  oledDisplay->print(currentField == FIELD_USERNAME ? ">" : " ");
  oledDisplay->print("User: ");
  oledDisplay->println(usernameBuffer.length() > 0 ? usernameBuffer : "_____");
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Password field
  if (currentField == FIELD_PASSWORD) {
    oledDisplay->fillRect(0, fieldY2, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY2);
  oledDisplay->print(currentField == FIELD_PASSWORD ? ">" : " ");
  oledDisplay->print("Pass: ");
  if (passwordBuffer.length() > 0) {
    for (size_t i = 0; i < min(passwordBuffer.length(), (size_t)8); i++) {
      oledDisplay->print("*");
    }
  } else {
    oledDisplay->print("_____");
  }
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Login button
  if (currentField == FIELD_LOGIN_BUTTON) {
    oledDisplay->fillRect(0, fieldY3, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY3);
  oledDisplay->print(currentField == FIELD_LOGIN_BUTTON ? ">" : " ");
  oledDisplay->print("[Login]");
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Show error message if active - overlay with centered message box
  if (millis() < errorDisplayUntil && errorMessage.length() > 0) {
    // Draw semi-transparent overlay effect (clear center area)
    oledDisplay->fillRect(8, 20, 112, 24, DISPLAY_COLOR_BLACK);
    oledDisplay->drawRect(8, 20, 112, 24, DISPLAY_COLOR_WHITE);
    oledDisplay->drawRect(9, 21, 110, 22, DISPLAY_COLOR_WHITE);
    
    // Center the message text - truncate if too long
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    String displayMsg = errorMessage;
    if (displayMsg.length() > 16) {
      displayMsg = displayMsg.substring(0, 15) + "~";
    }
    int textWidth = displayMsg.length() * 6;
    int textX = 64 - (textWidth / 2);
    if (textX < 12) textX = 12;
    oledDisplay->setCursor(textX, 28);
    oledDisplay->print(displayMsg);
  }
}

// Login input handler
static bool handleLoginModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Keyboard input is now handled centrally in processGamepadMenuInput() before this is called.
  // We only need to check for keyboard completion/cancellation to update our state.
  if (loginKeyboardActive) {
    if (oledKeyboardIsCompleted()) {
      const char* input = oledKeyboardGetText();
      if (currentField == FIELD_USERNAME) {
        usernameBuffer = String(input);
      } else if (currentField == FIELD_PASSWORD) {
        passwordBuffer = String(input);
      }
      oledKeyboardReset();
      loginKeyboardActive = false;
      return true;
    } else if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      loginKeyboardActive = false;
      // If cancelling from password, go back to username entry
      if (currentField == FIELD_PASSWORD) {
        currentField = FIELD_USERNAME;
      }
      return true;
    }
    // Keyboard still active - central dispatch already handled input
    return false;
  }
  
  // Early return if no meaningful input
  if (newlyPressed == 0 && abs(deltaX) < JOYSTICK_DEADZONE && abs(deltaY) < JOYSTICK_DEADZONE) {
    return false;
  }
  
  bool handled = false;
  
  // Field selection mode
  // Navigate between fields using centralized navigation events
  if (gNavEvents.down) {
    currentField = (LoginField)((currentField + 1) % 3);
    handled = true;
  } else if (gNavEvents.up) {
    currentField = (LoginField)((currentField + 2) % 3);
    handled = true;
  }
  
  // A button: Select field or login
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (currentField == FIELD_LOGIN_BUTTON) {
      // Attempt login
      if (usernameBuffer.length() > 0 && passwordBuffer.length() > 0) {
        if (loginTransport(SOURCE_LOCAL_DISPLAY, usernameBuffer, passwordBuffer)) {
          notifyLoginSuccess(usernameBuffer.c_str(), "display");
          errorMessage = "Login successful!";
          errorDisplayUntil = millis() + 2000;
          oledMarkDirtyUntil(errorDisplayUntil);
          
          // Securely clear buffers for next login
          secureClearString(usernameBuffer);
          secureClearString(passwordBuffer);
          currentField = FIELD_USERNAME;
          
          // Go to menu after successful login
          setOLEDMode(OLED_MENU);
          resetOLEDMenu();
          tryAutoStartGamepadForMenu();
          
          return true;
        } else {
          notifyLoginFailed(usernameBuffer.c_str(), "display");
          errorMessage = "Invalid credentials";
          errorDisplayUntil = millis() + 3000;
          oledMarkDirtyUntil(errorDisplayUntil);
        }
      } else {
        if (millis() >= errorDisplayUntil) {
          errorMessage = "Enter user/pass";
          errorDisplayUntil = millis() + 5000;
          oledMarkDirtyUntil(errorDisplayUntil);
        }
      }
    } else {
      // Launch keyboard for username or password
      const char* initialText = "";
      const char* title = "";
      
      if (currentField == FIELD_USERNAME) {
        title = "Enter Username:";
        initialText = usernameBuffer.c_str();
      } else if (currentField == FIELD_PASSWORD) {
        title = "Enter Password:";
        initialText = passwordBuffer.c_str();
      }
      
      oledKeyboardInit(title, initialText, 32);
      loginKeyboardActive = true;
    }
    handled = true;
  }
  
  // B button: Block navigation when authentication is required and user not logged in
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (gSettings.localDisplayRequireAuth && !isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
      // Authentication is required and user is not logged in - block navigation
      if (millis() >= errorDisplayUntil) {
        errorMessage = "Login required";
        errorDisplayUntil = millis() + 5000;
        oledMarkDirtyUntil(errorDisplayUntil);
      }
      handled = true;
    } else {
      // User is authenticated or auth not required - allow back navigation
      // Securely clear any entered credentials when leaving
      secureClearString(usernameBuffer);
      secureClearString(passwordBuffer);
      currentField = FIELD_USERNAME;
    }
    // Let default handler process B button for mode change
  }
  
  return handled;
}

// Login availability check
static bool isLoginModeAvailable(String* outReason) {
  return true;
}

// ============================================================================
// Logout Mode
// ============================================================================

// Logout state
static unsigned long logoutMessageUntil = 0;
static String loggedOutUser = "";

// Display logout confirmation
static void displayLogoutMode() {
  if (!oledDisplay) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  
  // Check if we just logged out (message timer active)
  if (millis() < logoutMessageUntil && loggedOutUser.length() > 0) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("User: ");
    oledDisplay->println(loggedOutUser);
    y += 10;
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Session ended");
    y += 10;
    oledDisplay->setCursor(0, y);
    oledDisplay->println("successfully.");
    return;
  }
  
  // Normal logout prompt (if somehow we get here without being logged in)
  String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
  bool isAuthed = isTransportAuthenticated(SOURCE_LOCAL_DISPLAY);
  
  if (!isAuthed || currentUser.length() == 0) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("No active session");
    y += 10;
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Press B to return");
    return;
  }
  
  // Show logout confirmation prompt
  oledDisplay->setCursor(0, y);
  oledDisplay->print("Current user: ");
  oledDisplay->println(currentUser);
  y += 10;
  oledDisplay->setCursor(0, y);
  oledDisplay->println("Press A to confirm");
  y += 10;
  oledDisplay->setCursor(0, y);
  oledDisplay->println("Press B to cancel");
}

// Handle logout input
static bool handleLogoutModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Early return if no meaningful input
  if (newlyPressed == 0 && abs(deltaX) < JOYSTICK_DEADZONE && abs(deltaY) < JOYSTICK_DEADZONE) {
    return false;
  }
  
  bool handled = false;
  
  // If showing logout confirmation message, any button returns to previous mode
  if (millis() < logoutMessageUntil) {
    if (newlyPressed != 0) {
      logoutMessageUntil = 0;
      loggedOutUser = "";
      oledMenuBack();
      handled = true;
    }
    return handled;
  }
  
  // A button: Confirm logout
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
    bool isAuthed = isTransportAuthenticated(SOURCE_LOCAL_DISPLAY);
    
    if (isAuthed && currentUser.length() > 0) {
      // Perform logout
      loggedOutUser = currentUser;
      logoutTransport(SOURCE_LOCAL_DISPLAY);
      logoutMessageUntil = millis() + 3000;  // Show message for 3 seconds
      
      DEBUG_SYSTEMF("[LOGOUT] User '%s' logged out from OLED", loggedOutUser.c_str());
      handled = true;
    } else {
      // No session to log out - go back to previous mode
      oledMenuBack();
      handled = true;
    }
  }
  
  // B button: Cancel logout - return false to let global handler call oledMenuBack()
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    return false;
  }
  
  return handled;
}

// Logout availability check - only available if user is logged in
static bool isLogoutModeAvailable(String* outReason) {
  bool isAuthed = isTransportAuthenticated(SOURCE_LOCAL_DISPLAY);
  
  if (!isAuthed) {
    if (outReason) *outReason = "Not logged in";
    return false;
  }
  
  return true;
}

// ============================================================================
// Mode Registration
// ============================================================================

// Login mode entry
static const OLEDModeEntry loginModeEntry = {
  OLED_LOGIN,
  "Login",
  "notify_system",
  displayLoginMode,
  isLoginModeAvailable,
  handleLoginModeInput,
  true,
  1
};

// Logout mode entry
static const OLEDModeEntry logoutModeEntry = {
  OLED_LOGOUT,
  "Logout",
  "notify_system",
  displayLogoutMode,
  isLogoutModeAvailable,
  handleLogoutModeInput,
  true,
  1
};

// Combined auth modes array
static const OLEDModeEntry authModes[] = { loginModeEntry, logoutModeEntry };

// Register both modes with a single registrar
static OLEDModeRegistrar _oled_mode_registrar_auth(authModes, sizeof(authModes) / sizeof(authModes[0]), "Auth");

// Force linker to include this file - called from OLED_Utils.cpp
void oledAuthModeInit() {
  // Static registrar already handles registration during global init
  // This function exists solely to force the linker to include this translation unit
}

// Legacy compatibility - redirect old init functions
void oledLoginModeInit() { oledAuthModeInit(); }
void oledLogoutModeInit() { oledAuthModeInit(); }

#endif // ENABLE_OLED_DISPLAY
