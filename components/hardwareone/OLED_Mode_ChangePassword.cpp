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

static PasswordChangeField sCurrentField = FIELD_CURRENT_PASSWORD;
static String sCurrentPassBuffer = "";
static String sNewPassBuffer = "";
static String sConfirmPassBuffer = "";
static bool sChangePasswordKeyboardActive = false;
static bool sPasswordChangeInProgress = false;

void oledChangePasswordModeResetSessionState() {
  sChangePasswordKeyboardActive = false;
  sPasswordChangeInProgress = false;
  sCurrentField = FIELD_CURRENT_PASSWORD;
  secureClearString(sCurrentPassBuffer);
  secureClearString(sNewPassBuffer);
  secureClearString(sConfirmPassBuffer);
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
  if (sCurrentField == FIELD_CURRENT_PASSWORD) {
    oledDisplay->fillRect(0, fieldY1, SCREEN_WIDTH, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY1);
  oledDisplay->print(sCurrentField == FIELD_CURRENT_PASSWORD ? ">" : " ");
  oledDisplay->print("Current: ");
  if (sCurrentPassBuffer.length() > 0) {
    for (size_t i = 0; i < min(sCurrentPassBuffer.length(), (size_t)6); i++) {
      oledDisplay->print("*");
    }
  } else {
    oledDisplay->print("___");
  }
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // New password field
  if (sCurrentField == FIELD_NEW_PASSWORD) {
    oledDisplay->fillRect(0, fieldY2, SCREEN_WIDTH, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY2);
  oledDisplay->print(sCurrentField == FIELD_NEW_PASSWORD ? ">" : " ");
  oledDisplay->print("New: ");
  if (sNewPassBuffer.length() > 0) {
    for (size_t i = 0; i < min(sNewPassBuffer.length(), (size_t)9); i++) {
      oledDisplay->print("*");
    }
    // Show strength indicator
    if (sNewPassBuffer.length() >= 8) {
      oledDisplay->print(" +");
    }
  } else {
    oledDisplay->print("___");
  }
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Confirm password field
  if (sCurrentField == FIELD_CONFIRM_PASSWORD) {
    oledDisplay->fillRect(0, fieldY3, SCREEN_WIDTH, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY3);
  oledDisplay->print(sCurrentField == FIELD_CONFIRM_PASSWORD ? ">" : " ");
  oledDisplay->print("Confirm: ");
  if (sConfirmPassBuffer.length() > 0) {
    for (size_t i = 0; i < min(sConfirmPassBuffer.length(), (size_t)6); i++) {
      oledDisplay->print("*");
    }
  } else {
    oledDisplay->print("___");
  }
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Change button
  if (sCurrentField == FIELD_CHANGE_BUTTON) {
    oledDisplay->fillRect(0, fieldY4, SCREEN_WIDTH, 8, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  }
  oledDisplay->setCursor(0, fieldY4);
  oledDisplay->print(sCurrentField == FIELD_CHANGE_BUTTON ? ">" : " ");
  oledDisplay->print(sPasswordChangeInProgress ? "[Changing...]" : "[Change Password]");
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
  if (sChangePasswordKeyboardActive) {
    if (oledKeyboardIsCompleted()) {
      const char* input = oledKeyboardGetText();
      if (sCurrentField == FIELD_CURRENT_PASSWORD) {
        sCurrentPassBuffer = String(input);
      } else if (sCurrentField == FIELD_NEW_PASSWORD) {
        sNewPassBuffer = String(input);
      } else if (sCurrentField == FIELD_CONFIRM_PASSWORD) {
        sConfirmPassBuffer = String(input);
      }
      oledKeyboardReset();
      sChangePasswordKeyboardActive = false;
      return true;
    } else if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      sChangePasswordKeyboardActive = false;
      return true;
    }
    return true;
  }
  
  // Don't accept input while password change is in progress
  if (sPasswordChangeInProgress) {
    return true;
  }
  
  bool handled = false;
  
  // Navigate between fields
  if (gNavEvents.down) {
    sCurrentField = (PasswordChangeField)((sCurrentField + 1) % 4);
    handled = true;
  } else if (gNavEvents.up) {
    sCurrentField = (PasswordChangeField)((sCurrentField + 3) % 4);
    handled = true;
  }
  
  // A button: Select field or change password
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (sCurrentField == FIELD_CHANGE_BUTTON) {
      // Validate all fields are filled
      if (sCurrentPassBuffer.length() == 0) {
        oledToastShow("Enter current pass", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      if (!isValidPublicPassword(sNewPassBuffer)) {
        oledToastShow("New: 6-64 chars", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      if (sConfirmPassBuffer.length() == 0) {
        oledToastShow("Confirm password", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      if (sNewPassBuffer != sConfirmPassBuffer) {
        oledToastShow("Passwords differ", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      if (sNewPassBuffer == sCurrentPassBuffer) {
        oledToastShow("New must differ", oled_message::kErrorDurationMs, OledUiIcon::ERROR);
        return true;
      }
      
      // Execute password change via the unified command dispatcher so the
      // request runs under the OLED-logged-in user's identity (built by
      // executeOledCommand from SOURCE_LOCAL_DISPLAY + gLocalDisplayUser) and
      // gets a normal [CMD] audit-log entry. Direct cmd_user_changepassword()
      // call previously bypassed both the identity install and the audit log.
      sPasswordChangeInProgress = true;
      String args = "userchangepassword " + sCurrentPassBuffer + " " + sNewPassBuffer + " " + sConfirmPassBuffer;
      // Do not pin identity replacement through password hashing/FS work. The
      // command carries the exact epoch and result publication is fenced.
      char resultBuf[256] = {};
      const bool commandOk = executeOledCommandWithResultForSession(
          args, sessionEpoch, resultBuf, sizeof(resultBuf));
      secureClearString(args);
      const char* result = resultBuf;

      sPasswordChangeInProgress = false;

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
        secureClearString(sCurrentPassBuffer);
        secureClearString(sNewPassBuffer);
        secureClearString(sConfirmPassBuffer);
        sCurrentField = FIELD_CURRENT_PASSWORD;
      } else {
        // Error
        char feedback[oled_message::kMessageBytes];
        oledCommandReplyText(false, result, nullptr, "Password change failed. Try again.",
                             feedback, sizeof(feedback));
        oledToastShow(feedback, oled_message::kErrorDurationMs, OledUiIcon::ERROR);

        // Clear only current password on error
        secureClearString(sCurrentPassBuffer);
        sCurrentField = FIELD_CURRENT_PASSWORD;
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
      
      if (sCurrentField == FIELD_CURRENT_PASSWORD) {
        title = "Current Password:";
        initialText = sCurrentPassBuffer.c_str();
      } else if (sCurrentField == FIELD_NEW_PASSWORD) {
        title = "New Password:";
        initialText = sNewPassBuffer.c_str();
      } else if (sCurrentField == FIELD_CONFIRM_PASSWORD) {
        title = "Confirm Password:";
        initialText = sConfirmPassBuffer.c_str();
      }
      
      oledKeyboardInit(title, initialText, 32,
                       OledKeyboardDictationPolicy::DENY, /*masked=*/true);
      sChangePasswordKeyboardActive = true;
    }
    handled = true;
  }
  
  // B button: Cancel and return to menu
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    // Securely clear all buffers
    secureClearString(sCurrentPassBuffer);
    secureClearString(sNewPassBuffer);
    secureClearString(sConfirmPassBuffer);
    sCurrentField = FIELD_CURRENT_PASSWORD;
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
static const OledModeEntry kChangePasswordModeEntries[] = {
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

REGISTER_OLED_MODE_MODULE(kChangePasswordModeEntries, 1, "ChangePassword");

// Force linker to include this file - called from OLED_Utils.cpp
void oledChangePasswordModeInit() {}

#endif // ENABLE_OLED_DISPLAY
