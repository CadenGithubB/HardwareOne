// ============================================================================
// OLED Change Password Mode
// ============================================================================
// Allows authenticated users to change their password via OLED interface

#include "OLED_Display.h"
#include "System_CommandReply.h"  // the reply words and how to read them
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "OLED_UI.h"
#include "System_Debug.h"
#include "System_User.h"
#include "System_Utils.h"

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
static bool changePasswordKeyboardActive = false;
static bool passwordChangeInProgress = false;

void oledChangePasswordModeResetSessionState() {
  changePasswordKeyboardActive = false;
  passwordChangeInProgress = false;
  currentField = FIELD_CURRENT_PASSWORD;
  secureClearString(currentPassBuffer);
  secureClearString(newPassBuffer);
  secureClearString(confirmPassBuffer);
}

static void wipeResultBuffer(char* buffer, size_t size) {
  volatile char* p = reinterpret_cast<volatile char*>(buffer);
  while (size-- > 0) *p++ = '\0';
}

// ============================================================================
// Display Function
// ============================================================================

static void displayChangePasswordMode() {
  if (!oledDisplay) {
    ERROR_SYSTEMF("[CHANGE_PASSWORD] FATAL: oledDisplay is null!");
    return;
  }

  String sessionUser;
  bool sessionAuthed = false;
  const TransportSessionEpoch sessionEpoch =
      localDisplayTransportSessionSnapshot(sessionUser, sessionAuthed);
  secureClearString(sessionUser);
  if (!sessionAuthed || sessionEpoch == kNoTransportSessionEpoch) return;

  // If the on-screen keyboard is active (a field was selected for entry), render
  // it full-screen instead of the fields view — same pattern every other
  // keyboard-input mode uses (Login, CLI Input, LLM, SetPattern, ...). Without
  // this guard the keyboard is active and receiving input but is never drawn, so
  // selecting a field appears to do nothing.
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
    return;
  }

  // The header bar shows the screen's name; the content starts below it.
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setTextSize(1);

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

}

// ============================================================================
// Input Handler
// ============================================================================

static bool handleChangePasswordModeInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  String sessionUser;
  bool sessionAuthed = false;
  const TransportSessionEpoch sessionEpoch =
      localDisplayTransportSessionSnapshot(sessionUser, sessionAuthed);
  secureClearString(sessionUser);
  if (!sessionAuthed || sessionEpoch == kNoTransportSessionEpoch) {
    oledChangePasswordModeResetSessionState();
    return true;
  }

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
        oledToastShow("Enter current pass", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      if (!isValidPublicPassword(newPassBuffer)) {
        oledToastShow("New: 6-64 chars", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      if (confirmPassBuffer.length() == 0) {
        oledToastShow("Confirm password", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      if (newPassBuffer != confirmPassBuffer) {
        oledToastShow("Passwords differ", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      if (newPassBuffer == currentPassBuffer) {
        oledToastShow("New must differ", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      
      // Execute password change via the unified command dispatcher so the
      // request runs under the OLED-logged-in user's identity (built by
      // executeOledCommand from SOURCE_LOCAL_DISPLAY + gLocalDisplayUser) and
      // gets a normal [CMD] audit-log entry. Direct cmd_user_changepassword()
      // call previously bypassed both the identity install and the audit log.
      passwordChangeInProgress = true;
      String args = "userchangepassword " + currentPassBuffer + " " + newPassBuffer + " " + confirmPassBuffer;
      // Do not pin identity replacement through password hashing/FS work. The
      // command carries the exact epoch and result publication is fenced.
      char resultBuf[256] = {};
      const bool commandOk = executeOledCommandWithResultForSession(
          args, sessionEpoch, resultBuf, sizeof(resultBuf));
      secureClearString(args);
      const char* result = resultBuf;

      passwordChangeInProgress = false;

      // A successful password change intentionally revokes this user's other
      // sessions but preserves the calling display session. If a future policy
      // rotates the caller too, the exact-epoch lease below simply fails.

      // The command may have waited behind a remote display replacement. Do
      // not publish its result/error state unless the exact submitting display
      // incarnation is still live. If replacement races the following owner-
      // only state update, the pending boundary wipes it before rendering.
      if (!transportSessionEpochIsLive(SOURCE_LOCAL_DISPLAY, sessionEpoch)) {
        wipeResultBuffer(resultBuf, sizeof(resultBuf));
        return true;
      }

      // The executor's verdict: every refusal of the handler leads with "Error".
      const bool passwordChanged = commandOk && result && !commandReplyIsFailure(result);
      if (passwordChanged) {
        // Success
        oledToastShow("Password changed!", 2500, OledUiIcon::SUCCESS);
        
        // Clear buffers securely
        secureClearString(currentPassBuffer);
        secureClearString(newPassBuffer);
        secureClearString(confirmPassBuffer);
        currentField = FIELD_CURRENT_PASSWORD;
      } else {
        // Error
        char feedback[oled_message::kMessageBytes];
        oledCommandReplyText(false, result, nullptr, "Password change failed. Try again.",
                             feedback, sizeof(feedback));
        oledToastShow(feedback, oled_message::kErrorDurationMs, OledUiIcon::ERROR);

        // Clear only current password on error
        secureClearString(currentPassBuffer);
        currentField = FIELD_CURRENT_PASSWORD;
      }

      wipeResultBuffer(resultBuf, sizeof(resultBuf));

      if (passwordChanged) {
        // Return to the menu only if this is still the same display session
        // after the acknowledgement delay. A replacement identity owns its
        // own navigation and must not be moved by this old handler.
        delay(2000);
        if (transportSessionEpochIsLive(
                SOURCE_LOCAL_DISPLAY, sessionEpoch)) {
          requestOledMode(OLED_MENU, "changepw.done", false);
          resetOledMenu();
        }
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
      
      oledKeyboardInit(title, initialText, 32,
                       OledKeyboardDictationPolicy::DENY, /*masked=*/true);
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
static const OledModeEntry changePasswordModeEntries[] = {
  {
    OLED_CHANGE_PASSWORD,
    "Change Password",
    "lock",
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
