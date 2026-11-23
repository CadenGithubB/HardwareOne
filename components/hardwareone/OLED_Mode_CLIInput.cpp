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
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Utils.h"

static bool     sKeyboardActive  = false;

// After a command runs we show an inline result screen (instead of silently
// re-opening a blank keyboard) so the user gets affirmative/negative feedback
// right here on the CLI Input page — not only on the separate CLI Output page.
static bool     sShowingResult   = false;
static bool     sLastOk          = false;
static String   sLastCommand     = "";
static String   sLastResult      = "";
static TransportSessionEpoch sResultEpoch = kNoTransportSessionEpoch;
static TransportSessionEpoch sInputEpoch = kNoTransportSessionEpoch;

void resetCliInputState();

enum class OledCliNativeTransition : uint8_t {
  None,
  Login,
  Logout,
};

static OledCliNativeTransition classifyNativeTransition(const String& cmd) {
  CommandArgs args(cmd);
  if (args.count() == 3 && args.arg(0).equalsIgnoreCase("login")) {
    return OledCliNativeTransition::Login;
  }
  if (args.count() == 1 && args.arg(0).equalsIgnoreCase("logout")) {
    return OledCliNativeTransition::Logout;
  }
  return OledCliNativeTransition::None;
}

// The echo and the executor's reply reach CLI Output through the shared output
// queue, routed to the OLED sink only: they keep their order relative to the
// lines the command broadcast while running and cross the same identity fence.
// The reply is an explicit handoff, not a guess about missing output; it is
// shown on no other display sink, so this is its one copy on the page.
static void cliInputPublish(const char* text) {
  if (!text || !text[0]) return;
  broadcastOutputCore_Routed(text, strlen(text), MSG_ROUTE_OLED);
}

static void startInputKeyboard() {
  String sessionUser;
  bool sessionAuthed = false;
  sInputEpoch =
      localDisplayTransportSessionSnapshot(sessionUser, sessionAuthed);
  secureClearString(sessionUser);
  if (!sessionAuthed) sInputEpoch = kNoTransportSessionEpoch;
  oledKeyboardInit("Command:", nullptr, OLED_KEYBOARD_MAX_LENGTH,
                   OledKeyboardDictationPolicy::DENY);
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
static void displayCliResult() {
  int y = OLED_CONTENT_START_Y;

  // Echoed command for context (truncated to one line).
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, y);
  String cmdLine = "> " + sLastCommand;
  const int cols = oledCols();  // 21 on the 128 px row, 10 in portrait
  if ((int)cmdLine.length() > cols) cmdLine = cmdLine.substring(0, cols);
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
  const int kMaxCols  = cols;
  const int kMaxLines = oledPortrait() ? 6 : 2;
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
    oledDisplay->setCursor(SCREEN_WIDTH - 6, y - 10);
    oledDisplay->print("~");
  }
}

static void displayCliInput() {
  if (!oledDisplay) return;

  // After a command runs, show the result screen until the user dismisses it.
  if (sShowingResult) {
    // Only render a result owned by the current authority. A replacement that
    // races after this check schedules an owner-side wipe, and the central
    // framebuffer commit fence prevents these pixels reaching its successor.
    if (transportSessionEpochIsLive(
            SOURCE_LOCAL_DISPLAY, sResultEpoch)) {
      displayCliResult();
    } else {
      resetCliInputState();
    }
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

static bool handleCliInputInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  // Result screen: A/X/START runs another command; B falls through to the
  // global handler, which turns it into oledMenuBack().
  if (sShowingResult) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) ||
        INPUT_CHECK(newlyPressed, INPUT_BUTTON_X) ||
        INPUT_CHECK(newlyPressed, INPUT_BUTTON_START)) {
      if (!transportSessionEpochIsLive(
              SOURCE_LOCAL_DISPLAY, sResultEpoch)) {
        resetCliInputState();
        return true;
      }
      sShowingResult = false;
      startInputKeyboard();
      return true;
    }
    return false;
  }

  if (!sKeyboardActive) return false;

  if (oledKeyboardIsCompleted()) {
    const TransportSessionEpoch sessionEpoch = sInputEpoch;
    if (sessionEpoch == kNoTransportSessionEpoch ||
        !transportSessionEpochIsLive(
            SOURCE_LOCAL_DISPLAY, sessionEpoch)) {
      resetCliInputState();
      return true;
    }

    // Only the OLED owner mutates the shared keyboard. Remote replacement can
    // change authority here but merely schedules a boundary; that boundary is
    // applied after this callback and wipes anything copied below.
    const char* text = oledKeyboardGetText();
    String cmd = String(text);
    cmd.trim();
    oledKeyboardReset();
    sKeyboardActive = false;
    sInputEpoch = kNoTransportSessionEpoch;

    if (cmd.length() == 0) {
      // Nothing typed — just re-open the keyboard.
      startInputKeyboard();
      return true;
    }

    // Echo only the audit-safe form; login/password commands must never live
    // in the Guest-visible shared console history.
    const String safeCommand = redactCmdForAudit(cmd);
    char echoBuf[64];
    snprintf(echoBuf, sizeof(echoBuf), "> %s", safeCommand.c_str());

    // Login/logout are local display lifecycle operations, not ordinary text
    // commands. Running them through cmd_exec would rotate the very epoch that
    // owns this result and could make a post-handler reply/audit land in the
    // replacement session. The native Login/Logout screens are the one safe
    // caller-local path; explicit cross-transport targets still flow below.
    const OledCliNativeTransition transition = classifyNativeTransition(cmd);
    if (transition != OledCliNativeTransition::None) {
      const char* guidance =
          transition == OledCliNativeTransition::Login
              ? "Use the OLED Login screen"
              : "Use the OLED Logout screen";
      cliInputPublish(echoBuf);
      cliInputPublish(guidance);
      sLastCommand = safeCommand;
      sLastResult = guidance;
      sLastOk = false;
      sResultEpoch = sessionEpoch;
      sShowingResult = true;
      oledMarkDirty();
      secureClearString(cmd);
      return true;
    }

    cliInputPublish(echoBuf);

    // Execute through the same auth context used by all OLED commands.
    char out[256] = {};
    bool ok = executeOledCommandWithResultForSession(
        cmd, sessionEpoch, out, sizeof(out));
    secureClearString(cmd);

    if (!transportSessionEpochIsLive(
            SOURCE_LOCAL_DISPLAY, sessionEpoch)) {
      volatile char* wipe = reinterpret_cast<volatile char*>(out);
      for (size_t i = 0; i < sizeof(out); ++i) wipe[i] = '\0';
      resetCliInputState();
      return true;
    }

    // Hand the reply to CLI Output after the lines the command broadcast.
    cliInputPublish((strlen(out) > 0) ? out : (ok ? "OK" : "Error"));

    // Show the inline result screen so the user sees pass/fail right here
    // instead of a blank re-opened keyboard.
    sLastCommand = safeCommand;
    sLastResult  = (strlen(out) > 0) ? String(out) : (ok ? "OK" : "(no output)");
    sLastOk      = ok;
    sResultEpoch = sessionEpoch;
    sShowingResult = true;
    oledMarkDirty();
    volatile char* wipe = reinterpret_cast<volatile char*>(out);
    for (size_t i = 0; i < sizeof(out); ++i) wipe[i] = '\0';
    return true;
  }

  if (oledKeyboardIsCancelled()) {
    if (!transportSessionEpochIsLive(
            SOURCE_LOCAL_DISPLAY, sInputEpoch)) {
      resetCliInputState();
      return true;
    }
    oledKeyboardReset();
    sKeyboardActive = false;
    sInputEpoch = kNoTransportSessionEpoch;
    oledMenuBack();
    return true;
  }

  // Keyboard still active — central dispatch handles joystick/button input
  return false;
}

// ============================================================================
// Availability
// ============================================================================

static bool isCliInputAvailable(String* outReason) {
  if (!isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
    if (outReason) *outReason = "Login required";
    return false;
  }
  return true;
}

// ============================================================================
// Mode entry reset
// ============================================================================

void resetCliInputState() {
  if (sKeyboardActive) {
    oledKeyboardReset();
    sKeyboardActive = false;
  }
  sShowingResult = false;
  sLastCommand = "";
  sLastResult = "";
  sResultEpoch = kNoTransportSessionEpoch;
  sInputEpoch = kNoTransportSessionEpoch;
}

void oledCliInputResetSessionState() {
  resetCliInputState();
}

// ============================================================================
// Registration
// ============================================================================

// Entry hook (was an inline reset inside requestOledMode): clear the input
// buffer on every entry, forward or back.
static void cliInputOnEnter(bool /*isForward*/) {
  resetCliInputState();
}

static const OledModeEntry sCliInputMode = {
  OLED_CLI_INPUT,
  "CLI Input",
  "terminal",
  displayCliInput,
  isCliInputAvailable,
  handleCliInputInput,
  true,
  93,   // Just after CLI Output (92)
  "A:New B:Back",   // shown on the result screen (keyboard supplies its own hints)
  cliInputOnEnter
};

REGISTER_OLED_MODE_MODULE(&sCliInputMode, 1, "CLIInput");

// Force linker to include this file - called from OLED_Utils.cpp
void oledCliInputModeInit() {}

#endif // ENABLE_OLED_DISPLAY
