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

static void startInputKeyboard() {
  oledKeyboardInit("Command:", nullptr, OLED_KEYBOARD_MAX_LENGTH);
  sKeyboardActive = true;
}

// ============================================================================
// Display
// ============================================================================

static void displayCLIInput() {
  if (!oledDisplay) return;

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
  if (!sKeyboardActive) return false;

  if (oledKeyboardIsCompleted()) {
    const char* text = oledKeyboardGetText();
    String cmd = String(text);
    oledKeyboardReset();
    sKeyboardActive = false;

    if (cmd.length() > 0) {
      // Echo the command into the console so the user can see what was typed
      char echoBuf[64];
      snprintf(echoBuf, sizeof(echoBuf), "> %s", cmd.c_str());
      gOledConsole.append(echoBuf, millis());

      // Record console size before executing so we can detect new output
      sPreSubmitCount = gOledConsole.getLineCount();
      sAwaitingResult = true;

      // Execute through the same auth context used by all OLED commands
      char out[256];
      bool ok = executeOLEDCommandWithResult(cmd, out, sizeof(out));

      // If the command produced no output to the console buffer itself,
      // show the returned string so there's always visible feedback.
      if (gOledConsole.getLineCount() == sPreSubmitCount) {
        const char* feedback = (strlen(out) > 0) ? out : (ok ? "OK" : "Error");
        gOledConsole.append(feedback, millis());
      }
      sAwaitingResult = false;
    }

    // Re-open keyboard ready for next command
    startInputKeyboard();
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
  nullptr,
  cliInputOnEnter
};

REGISTER_OLED_MODE_MODULE(&sCLIInputMode, 1, "CLIInput");

// Force linker to include this file - called from OLED_Utils.cpp
void oledCLIInputModeInit() {}

#endif // ENABLE_OLED_DISPLAY
