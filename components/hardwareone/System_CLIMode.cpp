// System_CLIMode.cpp
//
// Implementation of the CLI interactive-mode framework. See System_CLIMode.h
// for the design rationale and how to register a mode.
//
// The state surface is intentionally tiny: one pointer. Single-threaded:
// only the cmd_exec task (or whoever ends up calling cliModeDispatchInput
// after a future refactor) touches it. No mutex needed today; if the
// dispatcher ever runs on multiple tasks concurrently this becomes a
// nullptr-or-pointer-CAS, but that's a future problem.

#include "System_CLIMode.h"
#include "System_Debug.h"

// File-static: the currently active mode, or nullptr when none.
//
// Not `volatile` -- there's only one writer (the cmd_exec task) and the
// only reads come from that same task during dispatch. If readers from
// other tasks are added later (e.g. an OLED prompt prefix that wants to
// say "(help mode)"), promote this to volatile + atomic load/store.
static const CLIMode* sActiveMode = nullptr;

bool cliEnterMode(const CLIMode* mode) {
  if (mode == nullptr) {
    DEBUGF(DEBUG_CLI, "[climode] cliEnterMode(nullptr) rejected");
    return false;
  }
  if (sActiveMode != nullptr) {
    // Stacking modes isn't supported in this iteration. Caller must
    // cliExitMode() first if they want to switch. Logging the rejection
    // makes accidental misuse visible without crashing.
    DEBUGF(DEBUG_CLI, "[climode] cliEnterMode('%s') rejected: '%s' already active",
           mode->name ? mode->name : "(unnamed)",
           sActiveMode->name ? sActiveMode->name : "(unnamed)");
    return false;
  }

  sActiveMode = mode;
  DEBUGF(DEBUG_CLI, "[climode] enter '%s'", mode->name ? mode->name : "(unnamed)");
  if (mode->onEnter) {
    mode->onEnter(mode->userData);
  }
  return true;
}

void cliExitMode() {
  if (sActiveMode == nullptr) return;

  const CLIMode* m = sActiveMode;
  // Clear the pointer BEFORE calling onExit so the mode can't accidentally
  // recurse into cliEnterMode (which would be rejected as "already active"
  // even though we're about to deactivate).
  sActiveMode = nullptr;

  DEBUGF(DEBUG_CLI, "[climode] exit '%s'", m->name ? m->name : "(unnamed)");
  if (m->onExit) {
    m->onExit(m->userData);
  }
}

bool cliInModeActive() {
  return sActiveMode != nullptr;
}

const CLIMode* cliCurrentMode() {
  return sActiveMode;
}

bool cliModeDispatchInput(const String& line, char* out, size_t outSize) {
  if (sActiveMode == nullptr) return false;
  if (sActiveMode->onInput == nullptr) {
    // Misconfigured mode: no input handler. Treat as "no mode" so the
    // dispatcher falls through to normal lookup -- the user shouldn't be
    // trapped if a mode forgot to wire its handler.
    DEBUGF(DEBUG_CLI, "[climode] '%s' has no onInput; passing through",
           sActiveMode->name ? sActiveMode->name : "(unnamed)");
    return false;
  }

  // Make sure `out` is at least null-terminated even if the mode forgets
  // to write to it -- otherwise callers may print garbage if HANDLED is
  // returned with an unwritten buffer.
  if (out && outSize > 0) {
    out[0] = '\0';
  }

  CLIModeInputResult result = sActiveMode->onInput(line, sActiveMode->userData, out, outSize);

  switch (result) {
    case CLI_MODE_HANDLED:
      // Mode consumed the input, response already in `out`. Stay active.
      return true;

    case CLI_MODE_HANDLED_AND_EXIT:
      // Mode consumed the input AND wants to exit. Run onExit, clear the
      // pointer, return true so dispatcher stops.
      cliExitMode();
      return true;

    case CLI_MODE_PASSTHROUGH:
      // Mode wants normal command dispatch to run. Stay active.
      return false;

    case CLI_MODE_PASSTHROUGH_AND_EXIT:
      // Normal dispatch AND exit the mode first (so the upcoming command
      // runs in normal-CLI state, not in mode-active state).
      cliExitMode();
      return false;
  }

  // Defensive: unknown enum value. Treat as HANDLED so we don't accidentally
  // dispatch the line twice (worse than dropping it).
  DEBUGF(DEBUG_CLI, "[climode] '%s' returned unknown CLIModeInputResult=%d; treating as HANDLED",
         sActiveMode->name ? sActiveMode->name : "(unnamed)", (int)result);
  return true;
}
