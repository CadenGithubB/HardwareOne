// ============================================================================
// OLED CLI Input Mode
// ============================================================================
// Keyboard-based command input for the OLED display.
// Shows the OLED keyboard full-screen for typing commands; executes via
// SOURCE_LOCAL_DISPLAY (same auth context as all other OLED operations). The
// typed command and its result are echoed to the shared console and are viewable
// on the separate CLI Output page.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "OLED_ConsoleBuffer.h"
#include "System_Debug.h"
#include "System_Utils.h"

extern OLEDConsoleBuffer gOledConsole;

static bool     sKeyboardActive  = false;
static bool     sAwaitingResult  = false;
// Snapshot of the console line count taken at command submit time,
// used to detect when new output has arrived.
static int      sPreSubmitCount  = 0;

// After a command runs we show an inline result screen (instead of silently
// re-opening a blank keyboard) so the user gets affirmative/negative feedback
// right here on the CLI Input page — not only on the separate CLI Output page.
static bool     sShowingResult   = false;
static bool     sLastOk          = false;
static String   sLastCommand     = "";
static String   sLastResult      = "";

static void startInputKeyboard() {
  oledKeyboardInit("Command:", nullptr, OLED_KEYBOARD_MAX_LENGTH);
  sKeyboardActive = true;
}

// ============================================================================
// Display
// ============================================================================

// Draw the post-command result screen: the command that ran, a clear OK/FAILED
// badge, and the returned message (wrapped). Shown after each command so the
// user always gets visible pass/fail feedback without leaving this page. The
// keyboard is inactive here, so the system draws the header ("CLI Input") and
// the footer ("A:New B:Back", from the mode's registered hints).
static void displayCLIResult() {
  int y = OLED_CONTENT_START_Y;

  // Echoed command for context (truncated to one line).
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, y);
  String cmdLine = "> " + sLastCommand;
  if (cmdLine.length() > 21) cmdLine = cmdLine.substring(0, 21);
  oledDisplay->print(cmdLine);
  y += 11;

  // Status badge: inverted box, affirmative (OK) or negative (FAILED).
  const char* status = sLastOk ? "OK" : "FAILED";
  int badgeW = (int)strlen(status) * 6 + 3;
  oledDisplay->fillRect(0, y - 1, badgeW, 10, DISPLAY_COLOR_WHITE);
  oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
  oledDisplay->setCursor(2, y);
  oledDisplay->print(status);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  y += 11;

  // Result message, wrapped across the remaining content lines.
  const int kMaxCols  = 21;
  const int kMaxLines = 2;
  int pos = 0;
  int line = 0;
  int len = (int)sLastResult.length();
  while (pos < len && line < kMaxLines) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print(sLastResult.substring(pos, pos + kMaxCols));
    y += 10;
    pos += kMaxCols;
    line++;
  }
  if (pos < len) {
    // Signal that the message was truncated (full text is on CLI Output page).
    oledDisplay->setCursor(122, y - 10);
    oledDisplay->print("~");
  }
}

static void displayCLIInput() {
  if (!oledDisplay) return;

  // After a command runs, show the result screen until the user dismisses it.
  if (sShowingResult) {
    displayCLIResult();
    return;
  }

  // Auto-open the keyboard on the first frame after entry so it renders this
  // same frame (oledKeyboardInit sets active=true synchronously).
  if (!sKeyboardActive) {
    startInputKeyboard();
  }

  // Render the keyboard full-screen, like every other keyboard-input mode. The
  // global header is suppressed while a keyboard is active, so the keyboard owns
  // the whole content area. Command results are echoed to the shared console and
  // are viewable on the separate CLI Output page.
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
  }
}

// ============================================================================
// Input
// ============================================================================

static bool handleCLIInputInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  // Result screen: A/X/START runs another command; B falls through to the
  // global handler, which turns it into oledMenuBack().
  if (sShowingResult) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) ||
        INPUT_CHECK(newlyPressed, INPUT_BUTTON_X) ||
        INPUT_CHECK(newlyPressed, INPUT_BUTTON_START)) {
      sShowingResult = false;
      startInputKeyboard();
      return true;
    }
    return false;
  }

  if (!sKeyboardActive) return false;

  if (oledKeyboardIsCompleted()) {
    const char* text = oledKeyboardGetText();
    String cmd = String(text);
    cmd.trim();
    oledKeyboardReset();
    sKeyboardActive = false;

    if (cmd.length() == 0) {
      // Nothing typed — just re-open the keyboard.
      startInputKeyboard();
      return true;
    }

    // Echo the command into the shared console (viewable on CLI Output page).
    char echoBuf[64];
    snprintf(echoBuf, sizeof(echoBuf), "> %s", cmd.c_str());
    gOledConsole.append(echoBuf, millis());

    // Record console size before executing so we can detect new output.
    sPreSubmitCount = gOledConsole.getLineCount();
    sAwaitingResult = true;

    // Execute through the same auth context used by all OLED commands.
    char out[256];
    bool ok = executeOLEDCommandWithResult(cmd, out, sizeof(out));

    // If the command produced no output to the console buffer itself,
    // append the returned string so the CLI Output page always has feedback.
    if (gOledConsole.getLineCount() == sPreSubmitCount) {
      const char* feedback = (strlen(out) > 0) ? out : (ok ? "OK" : "Error");
      gOledConsole.append(feedback, millis());
    }
    sAwaitingResult = false;

    // Show the inline result screen so the user sees pass/fail right here
    // instead of a blank re-opened keyboard.
    sLastCommand = cmd;
    sLastResult  = (strlen(out) > 0) ? String(out) : (ok ? "OK" : "(no output)");
    sLastOk      = ok;
    sShowingResult = true;
    oledMarkDirty();
    return true;
  }

  if (oledKeyboardIsCancelled()) {
    oledKeyboardReset();
    sKeyboardActive = false;
    oledMenuBack();
    return true;
  }

  // Keyboard still active — central dispatch handles joystick/button input
  return false;
}

// ============================================================================
// Availability
// ============================================================================

static bool isCLIInputAvailable(String* outReason) {
  if (!isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
    if (outReason) *outReason = "Login required";
    return false;
  }
  return true;
}

// ============================================================================
// Mode entry reset
// ============================================================================

void resetCLIInputState() {
  if (sKeyboardActive) {
    oledKeyboardReset();
    sKeyboardActive = false;
  }
  sShowingResult = false;
  sLastCommand = "";
  sLastResult = "";
  sAwaitingResult = false;
  sPreSubmitCount = 0;
}

// ============================================================================
// Registration
// ============================================================================

// Entry hook (was an inline reset inside requestOLEDMode): clear the input
// buffer on every entry, forward or back.
static void cliInputOnEnter(bool /*isForward*/) {
  resetCLIInputState();
}

static const OLEDModeEntry sCLIInputMode = {
  OLED_CLI_INPUT,
  "CLI Input",
  "terminal",
  displayCLIInput,
  isCLIInputAvailable,
  handleCLIInputInput,
  true,
  93,   // Just after CLI Output (92)
  "A:New B:Back",   // shown on the result screen (keyboard supplies its own hints)
  cliInputOnEnter
};

REGISTER_OLED_MODE_MODULE(&sCLIInputMode, 1, "CLIInput");

// Force linker to include this file - called from OLED_Utils.cpp
void oledCLIInputModeInit() {}

#endif // ENABLE_OLED_DISPLAY
