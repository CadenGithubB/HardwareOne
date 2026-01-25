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
#include "System_Settings.h"
#include "i2csensor-seesaw.h"

// External OLED display pointer
extern Adafruit_SSD1306* oledDisplay;
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
    Serial.println("[LOGIN_RENDER] FATAL: oledDisplay is null!");
    return;
  }
  
  // If keyboard is active, display it instead
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
    return;
  }
  
  // Always draw at least a title so screen is never completely black
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  if (isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
    String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
    oledDisplay->print("Switch from: ");
    oledDisplay->println(currentUser);
  } else {
    oledDisplay->println("OLED Login");
  }
  
  // Field selection mode
  oledDisplay->setCursor(0, 12);
  oledDisplay->println("Select field:");
  
  oledDisplay->setCursor(0, 22);
  if (currentField == FIELD_USERNAME) {
    oledDisplay->fillRect(0, 22, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    oledDisplay->print("> Username: ");
    oledDisplay->println(usernameBuffer);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  } else {
    oledDisplay->println("  Username: " + usernameBuffer);
  }
  
  oledDisplay->setCursor(0, 32);
  if (currentField == FIELD_PASSWORD) {
    oledDisplay->fillRect(0, 32, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    oledDisplay->print("> Password: ");
    for (size_t i = 0; i < passwordBuffer.length(); i++) {
      oledDisplay->print("*");
    }
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  } else {
    oledDisplay->println("  Password: " + String(passwordBuffer.length() > 0 ? "*****" : ""));
  }
  
  oledDisplay->setCursor(0, 42);
  if (currentField == FIELD_LOGIN_BUTTON) {
    oledDisplay->fillRect(0, 42, 128, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    oledDisplay->println("> [Login]");
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  } else {
    oledDisplay->println("  [Login]");
  }
  
  // Show error message if active - overlay with centered message box
  if (millis() < errorDisplayUntil && errorMessage.length() > 0) {
    // Draw semi-transparent overlay effect (clear center area)
    oledDisplay->fillRect(8, 20, 112, 24, DISPLAY_COLOR_BLACK);
    oledDisplay->drawRect(8, 20, 112, 24, DISPLAY_COLOR_WHITE);
    oledDisplay->drawRect(9, 21, 110, 22, DISPLAY_COLOR_WHITE);
    
    // Center the message text
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    int textWidth = errorMessage.length() * 6;
    int textX = 64 - (textWidth / 2);
    if (textX < 12) textX = 12;
    oledDisplay->setCursor(textX, 28);
    oledDisplay->print(errorMessage);
  }
}

// Login input handler
static bool handleLoginModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Early return if no meaningful input (no buttons and joystick below threshold)
  if (newlyPressed == 0 && abs(deltaX) < JOYSTICK_DEADZONE && abs(deltaY) < JOYSTICK_DEADZONE) {
    return false;
  }
  
  bool handled = false;
  
  // If keyboard is active, handle keyboard input
  if (oledKeyboardIsActive()) {
    handled = oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
    
    // Check if keyboard completed
    if (oledKeyboardIsCompleted()) {
      const char* input = oledKeyboardGetText();
      if (currentField == FIELD_USERNAME) {
        usernameBuffer = String(input);
      } else if (currentField == FIELD_PASSWORD) {
        passwordBuffer = String(input);
      }
      oledKeyboardReset();
      loginKeyboardActive = false;
    } else if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      loginKeyboardActive = false;
      // If cancelling from password, go back to username entry
      if (currentField == FIELD_PASSWORD) {
        currentField = FIELD_USERNAME;
      }
    }
    
    return handled;
  }
  
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
          errorMessage = "Login successful!";
          errorDisplayUntil = millis() + 2000;
          
          // Reset buffers for next login
          usernameBuffer = "";
          passwordBuffer = "";
          currentField = FIELD_USERNAME;
          
          // Go to menu after successful login
          currentOLEDMode = OLED_MENU;
          resetOLEDMenu();
          tryAutoStartGamepadForMenu();
          
          return true;
        } else {
          errorMessage = "Invalid credentials";
          errorDisplayUntil = millis() + 3000;
        }
      } else {
        errorMessage = "Enter username/password";
        errorDisplayUntil = millis() + 2000;
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
    extern Settings gSettings;
    if (gSettings.localDisplayRequireAuth && !isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
      // Authentication is required and user is not logged in - block navigation
      errorMessage = "Login required";
      errorDisplayUntil = millis() + 2000;
      handled = true;
    } else {
      // User is authenticated or auth not required - allow back navigation
      // Clear any entered credentials when leaving
      usernameBuffer = "";
      passwordBuffer = "";
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
  
  // Check if we just logged out (message timer active)
  if (millis() < logoutMessageUntil && loggedOutUser.length() > 0) {
    // Show "Logged Out" confirmation message
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("=== LOGGED OUT ===");
    oledDisplay->println();
    
    oledDisplay->print("User: ");
    oledDisplay->println(loggedOutUser);
    oledDisplay->println();
    oledDisplay->println("Session ended");
    oledDisplay->println("successfully.");
    
    return;
  }
  
  // Normal logout prompt (if somehow we get here without being logged in)
  String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
  bool isAuthed = isTransportAuthenticated(SOURCE_LOCAL_DISPLAY);
  
  if (!isAuthed || currentUser.length() == 0) {
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("=== LOGOUT ===");
    oledDisplay->println();
    oledDisplay->println("No active session");
    oledDisplay->println();
    oledDisplay->println("Press B to return");
    return;
  }
  
  // Show logout confirmation prompt
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("=== LOGOUT ===");
  oledDisplay->println();
  
  oledDisplay->print("Current user: ");
  oledDisplay->println(currentUser);
  oledDisplay->println();
  oledDisplay->println("Press A to confirm");
  oledDisplay->println("Press B to cancel");
}

// Handle logout input
static bool handleLogoutModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Early return if no meaningful input
  if (newlyPressed == 0 && abs(deltaX) < JOYSTICK_DEADZONE && abs(deltaY) < JOYSTICK_DEADZONE) {
    return false;
  }
  
  bool handled = false;
  
  // If showing logout confirmation message, any button returns to menu
  if (millis() < logoutMessageUntil) {
    if (newlyPressed != 0) {
      currentOLEDMode = OLED_MENU;
      logoutMessageUntil = 0;
      loggedOutUser = "";
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
      
      Serial.printf("[LOGOUT] User '%s' logged out from OLED\n", loggedOutUser.c_str());
      handled = true;
    } else {
      // No session to log out - just go back to menu
      currentOLEDMode = OLED_MENU;
      handled = true;
    }
  }
  
  // B button: Cancel logout and return to menu
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    currentOLEDMode = OLED_MENU;
    handled = true;
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
