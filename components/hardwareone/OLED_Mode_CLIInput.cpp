// ============================================================================
// OLED CLI Input Mode
// ============================================================================
// Keyboard-based command input for the OLED display.
// Shows the OLED keyboard for typing commands; executes via SOURCE_LOCAL_DISPLAY
// (same auth context as all other OLED operations). The last few lines of CLI
// console output are shown above the keyboard so the user can see the result.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "OLED_ConsoleBuffer.h"
#include "System_Debug.h"
#include "System_Utils.h"

extern OLEDConsoleBuffer gOledConsole;

// Number of output preview lines shown above the keyboard
static const int CLI_INPUT_PREVIEW_LINES = 2;
// Pixel height per preview line
static const int CLI_INPUT_LINE_HEIGHT   = 10;

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

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  // Always show last N output lines at the top of the content area
  if (gOledConsole.mutex && xSemaphoreTake(gOledConsole.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    int total = gOledConsole.getLineCount();
    int previewY = OLED_CONTENT_START_Y;

    for (int i = 0; i < CLI_INPUT_PREVIEW_LINES; i++) {
      int idx = total - CLI_INPUT_PREVIEW_LINES + i;
      if (idx < 0) {
        previewY += CLI_INPUT_LINE_HEIGHT;
        continue;
      }
      const char* line = gOledConsole.getLine(idx);
      if (line) {
        oledDisplay->setCursor(0, previewY);
        char truncated[22];
        strncpy(truncated, line, 21);
        truncated[21] = '\0';
        oledDisplay->print(truncated);
      }
      previewY += CLI_INPUT_LINE_HEIGHT;
    }
    xSemaphoreGive(gOledConsole.mutex);
  }

  // Separator line between preview and keyboard
  int sepY = OLED_CONTENT_START_Y + CLI_INPUT_PREVIEW_LINES * CLI_INPUT_LINE_HEIGHT;
  oledDisplay->drawFastHLine(0, sepY, 128, DISPLAY_COLOR_WHITE);

  // Keyboard renders below the separator.
  // The keyboard display function always fills from OLED_CONTENT_START_Y, so we
  // temporarily shift its origin by drawing it into a sub-region via a clip offset.
  // Since Adafruit_SSD1306 doesn't support clip regions natively, we instead
  // initialise the keyboard only when needed and let it fill the lower portion.
  // The keyboard is rendered at full height; the preview area is drawn on top each
  // frame, so only the lines below the separator are visible to the user.
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
  }

  // If keyboard isn't active yet, auto-open it
  if (!sKeyboardActive) {
    startInputKeyboard();
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

static const OLEDModeEntry sCLIInputMode = {
  OLED_CLI_INPUT,
  "CLI Input",
  "terminal",
  displayCLIInput,
  isCLIInputAvailable,
  handleCLIInputInput,
  true,
  93,   // Just after CLI Output (92)
  nullptr
};

REGISTER_OLED_MODE_MODULE(&sCLIInputMode, 1, "CLIInput");

// Force linker to include this file - called from OLED_Utils.cpp
void oledCLIInputModeInit() {}

#endif // ENABLE_OLED_DISPLAY
