// ============================================================================
// OLED Set Gamepad Password Mode - Joystick pattern password training flow
// ============================================================================
// Guided flow: Enter pattern → Confirm pattern → Save to user account
// Pattern is a sequence of joystick directions (^v<>) stored as a hashed password.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "System_Auth.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_Utils.h"
#include "i2csensor-seesaw.h"

extern OLEDMode currentOLEDMode;

// ============================================================================
// State Machine
// ============================================================================

enum PatternSetupStep {
  PATTERN_STEP_AUTH_CHECK,   // Check if auth is needed
  PATTERN_STEP_AUTH_GAMEPAD, // Auth with existing gamepad password
  PATTERN_STEP_AUTH_TEXT,    // Auth with text password (no gamepad pw set)
  PATTERN_STEP_AUTH_FAILED,  // Auth failed
  PATTERN_STEP_ENTER,        // "Enter new pattern"
  PATTERN_STEP_CONFIRM,      // "Confirm pattern"
  PATTERN_STEP_SUCCESS,      // "Pattern saved!"
  PATTERN_STEP_MISMATCH,     // "Patterns don't match"
  PATTERN_STEP_ERROR         // "Failed to save"
};

static PatternSetupStep sStep = PATTERN_STEP_AUTH_CHECK;
static String sFirstPattern = "";
static bool sKeyboardActive = false;
static unsigned long sMessageUntil = 0;
static bool sAuthUsingGamepad = false;  // True if authenticating with gamepad password

static void startPatternKeyboard(const char* title) {
  oledKeyboardInit(title, nullptr, OLED_KEYBOARD_MAX_LENGTH);
  // Force pattern mode as default for this flow
  gOLEDKeyboardState.mode = KEYBOARD_MODE_PATTERN;
  sKeyboardActive = true;
}

static void startTextKeyboard(const char* title) {
  oledKeyboardInit(title, nullptr, OLED_KEYBOARD_MAX_LENGTH);
  // Use lowercase mode for text password entry
  gOLEDKeyboardState.mode = KEYBOARD_MODE_LOWERCASE;
  sKeyboardActive = true;
}

// Check auth and start appropriate keyboard
static void startAuthFlow() {
  String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
  if (currentUser.length() == 0) {
    sStep = PATTERN_STEP_AUTH_FAILED;
    return;
  }
  
  if (hasUserGamepadPassword(currentUser)) {
    // User has gamepad password - require it for auth
    sAuthUsingGamepad = true;
    sStep = PATTERN_STEP_AUTH_GAMEPAD;
    startPatternKeyboard("Current pattern:");
  } else {
    // No gamepad password - require text password
    sAuthUsingGamepad = false;
    sStep = PATTERN_STEP_AUTH_TEXT;
    startTextKeyboard("Enter password:");
  }
}

// ============================================================================
// Display
// ============================================================================

static void displaySetPatternMode() {
  if (!oledDisplay) return;

  // If keyboard is active, show it
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
    return;
  }

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  switch (sStep) {
    case PATTERN_STEP_AUTH_CHECK:
      {
        // Auto-start auth flow on first display
        startAuthFlow();
      }
      break;
    
    case PATTERN_STEP_AUTH_GAMEPAD:
    case PATTERN_STEP_AUTH_TEXT:
      // Keyboard handles display
      break;
    
    case PATTERN_STEP_AUTH_FAILED:
      {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
        oledDisplay->println("Incorrect password.");
        oledDisplay->println("Please try again.");
        
        // Note: Footer is drawn by global render loop
      }
      break;
    
    case PATTERN_STEP_ENTER:
      {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
        oledDisplay->println("Set a joystick");
        oledDisplay->println("pattern as your");
        oledDisplay->println("gamepad login.");
        
        // Note: Footer is drawn by global render loop
      }
      break;

    case PATTERN_STEP_CONFIRM:
      // Keyboard handles display
      break;

    case PATTERN_STEP_SUCCESS:
      {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
        oledDisplay->println("Your gamepad");
        oledDisplay->println("password is now set.");
        oledDisplay->println();
        oledDisplay->println("Press any button");
        
        // Note: Footer is drawn by global render loop
      }
      break;

    case PATTERN_STEP_MISMATCH:
      {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
        oledDisplay->println("Patterns did not");
        oledDisplay->println("match. Try again.");
        
        // Note: Footer is drawn by global render loop
      }
      break;

    case PATTERN_STEP_ERROR:
      {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
        oledDisplay->println("Failed to save");
        oledDisplay->println("password.");
        
        // Note: Footer is drawn by global render loop
      }
      break;
  }
}

// ============================================================================
// Input
// ============================================================================

static bool handleSetPatternInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Keyboard input is now handled centrally in processGamepadMenuInput() before this is called.
  // We only need to check for keyboard completion/cancellation to update our state.
  if (sKeyboardActive) {
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      String inputStr = String(text);
      int len = inputStr.length();
      oledKeyboardReset();
      sKeyboardActive = false;

      // Handle auth steps
      if (sStep == PATTERN_STEP_AUTH_GAMEPAD || sStep == PATTERN_STEP_AUTH_TEXT) {
        String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
        if (currentUser.length() > 0 && isValidUser(currentUser, inputStr)) {
          // Auth successful - proceed to set new pattern
          secureClearString(inputStr);
          sStep = PATTERN_STEP_ENTER;
          DEBUG_SYSTEMF("[SETPATTERN] Auth successful for user '%s'", currentUser.c_str());
        } else {
          // Auth failed
          secureClearString(inputStr);
          sStep = PATTERN_STEP_AUTH_FAILED;
          DEBUG_SYSTEMF("[SETPATTERN] Auth failed for user '%s'", currentUser.c_str());
        }
        return true;
      }

      // Handle pattern entry steps
      if (len < 4) {
        // Too short — restart this step
        if (sStep == PATTERN_STEP_ENTER) {
          startPatternKeyboard("New pattern (min 4):");
        } else {
          startPatternKeyboard("Confirm (min 4):");
        }
        return true;
      }

      if (sStep == PATTERN_STEP_ENTER) {
        sFirstPattern = inputStr;
        sStep = PATTERN_STEP_CONFIRM;
        startPatternKeyboard("Confirm pattern:");
      } else if (sStep == PATTERN_STEP_CONFIRM) {
        if (sFirstPattern == inputStr) {
          // Patterns match — save as gamepad password (separate from text password)
          String currentUser = getTransportUser(SOURCE_LOCAL_DISPLAY);
          if (currentUser.length() > 0 && setUserGamepadPassword(currentUser, sFirstPattern)) {
            sStep = PATTERN_STEP_SUCCESS;
            DEBUG_SYSTEMF("[SETPATTERN] Gamepad password saved for user '%s'", currentUser.c_str());
          } else {
            sStep = PATTERN_STEP_ERROR;
            DEBUG_SYSTEMF("[SETPATTERN] Failed to save gamepad password for user '%s'", currentUser.c_str());
          }
        } else {
          sStep = PATTERN_STEP_MISMATCH;
        }
        secureClearString(sFirstPattern);
      }
      secureClearString(inputStr);
      return true;
    }

    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      sKeyboardActive = false;
      secureClearString(sFirstPattern);
      sStep = PATTERN_STEP_AUTH_CHECK;
      oledMenuBack();
      return true;
    }

    // Keyboard still active - central dispatch already handled input
    return false;
  }

  // Non-keyboard screens — require actual input
  if (newlyPressed == 0) return false;
  bool handled = false;
  switch (sStep) {
    case PATTERN_STEP_AUTH_CHECK:
      // Auto-handled in display function
      break;
    
    case PATTERN_STEP_AUTH_FAILED:
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        // Retry auth
        startAuthFlow();
        handled = true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        // Cancel - go back to previous mode
        sStep = PATTERN_STEP_AUTH_CHECK;
        oledMenuBack();
        handled = true;
      }
      break;
    
    case PATTERN_STEP_ENTER:
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        startPatternKeyboard("New pattern:");
        handled = true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sStep = PATTERN_STEP_AUTH_CHECK;
        secureClearString(sFirstPattern);
        oledMenuBack();
        handled = true;
      }
      break;

    case PATTERN_STEP_MISMATCH:
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        sStep = PATTERN_STEP_ENTER;
        secureClearString(sFirstPattern);
        startPatternKeyboard("New pattern:");
        handled = true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sStep = PATTERN_STEP_ENTER;
        secureClearString(sFirstPattern);
        oledMenuBack();
        handled = true;
      }
      break;

    case PATTERN_STEP_SUCCESS:
    case PATTERN_STEP_ERROR:
      if (newlyPressed != 0) {
        sStep = PATTERN_STEP_AUTH_CHECK;
        secureClearString(sFirstPattern);
        oledMenuBack();
        handled = true;
      }
      break;

    default:
      break;
  }

  return handled;
}

// ============================================================================
// Availability — requires OLED display authentication
// ============================================================================

static bool isSetPatternAvailable(String* outReason) {
  if (!isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
    if (outReason) *outReason = "Login required";
    return false;
  }
  return true;
}

// ============================================================================
// CLI command: setpattern — triggers the OLED flow
// ============================================================================

static const char* cmd_setpattern(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
    return "Error: Log in on OLED first (login <user> <pass> display)";
  }

  sStep = PATTERN_STEP_AUTH_CHECK;
  secureClearString(sFirstPattern);
  setOLEDMode(OLED_SET_PATTERN);
  return "Opening gamepad password setup on OLED...";
}

// ============================================================================
// Mode and Command Registration
// ============================================================================

static const OLEDModeEntry setPatternModeEntry = {
  OLED_SET_PATTERN,
  "Gamepad Password",
  "notify_system",
  displaySetPatternMode,
  isSetPatternAvailable,
  handleSetPatternInput,
  true,
  2
};

static OLEDModeRegistrar _oled_mode_registrar_setpattern(&setPatternModeEntry, 1, "GamepadPassword");

static const CommandEntry setPatternCommands[] = {
  { "set gamepad password", "Set gamepad joystick password (OLED).", true, cmd_setpattern }
};

static CommandModuleRegistrar _setpattern_cmd_registrar(
  setPatternCommands, sizeof(setPatternCommands) / sizeof(setPatternCommands[0]), "setpattern");

// Force linker to include this file
void oledSetPatternModeInit() {}

#endif // ENABLE_OLED_DISPLAY
