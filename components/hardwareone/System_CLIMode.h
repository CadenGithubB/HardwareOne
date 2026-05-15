#ifndef SYSTEM_CLIMODE_H
#define SYSTEM_CLIMODE_H

#include <Arduino.h>

// ============================================================================
// CLI Interactive Mode framework
// ============================================================================
//
// Generalizes the "help mode" pattern that lives today in System_CLI.cpp
// (gCLIState + handleHelpNavigation) so other interactive flows -- the
// first-time-setup / featuresetup wizards, confirm-this-destructive-command
// prompts, paginated output viewers -- can use the same machinery.
//
// The core invariant a CLIMode preserves: each user input is processed by a
// SHORT, NON-BLOCKING handler call. State that needs to persist between
// user inputs (current page, current selection, partially-entered fields)
// lives in the mode's `userData` struct, not on a task's stack. The cmd_exec
// task is therefore free between keystrokes -- the opposite of how the
// wizard works today, where cmd_exec is parked inside a synchronous
// `waitForSerialInputBlocking()` for the entire wizard duration.
//
// Why this matters operationally:
//   1. A wizard built on this framework can be invoked from any transport
//      (web CLI, Bluetooth, MQTT, internal automations) -- not just serial
//      and OLED. Each transport's input naturally flows through the
//      command dispatcher; the active mode's onInput handler runs on the
//      cmd_exec task and returns immediately.
//   2. Other CLI commands keep working while the user is "inside" a mode
//      because cmd_exec isn't held hostage.
//   3. No race between the main-loop's serial drain and a per-task
//      blocking reader (the original `gWizardOwnsSerial` workaround).
//   4. No 10-second `submitCommandSync` timeout fighting a long-running
//      handler -- handlers are short.
//
// ----------------------------------------------------------------------------
// Lifecycle
//
//   const CLIMode helpMode = { ... };
//   cliEnterMode(&helpMode);   // dispatcher now routes input through helpMode.onInput first
//   ...user types things...
//   cliExitMode();             // ends the mode; onExit fires
//
// Only ONE mode is active at a time in this initial implementation. Entering
// a mode while another is active is rejected; the active mode must exit first.
// (Stackable modes can be added later if needed -- e.g., a wizard popping up
// a confirm dialog mid-flow.)
//
// ----------------------------------------------------------------------------
// onInput return values
//
// CLI_MODE_HANDLED: mode consumed the input. Caller already wrote response
//   bytes into `out`. Dispatcher returns to the caller without normal lookup.
//
// CLI_MODE_HANDLED_AND_EXIT: same as HANDLED, but ALSO indicates the mode is
//   ready to exit (e.g. user typed "back" or "exit", or a wizard reached its
//   final page). Dispatcher calls onExit and clears the active mode pointer.
//
// CLI_MODE_PASSTHROUGH: mode wants the input dispatched as a normal CLI
//   command. Mode STAYS ACTIVE -- useful for help mode's behavior where
//   typing a module name renders that module's help page without leaving
//   help mode. The mode itself can render the module's help inside onInput
//   and use HANDLED for that case; PASSTHROUGH is for when the mode
//   *deliberately* wants normal dispatch (rare).
//
// CLI_MODE_PASSTHROUGH_AND_EXIT: dispatch as a normal command AND exit the
//   mode first. This is help mode's behavior when the user types a regular
//   command name like `wifistatus` -- help should exit, then `wifistatus`
//   runs normally.
//
// ----------------------------------------------------------------------------

enum CLIModeInputResult {
  CLI_MODE_HANDLED               = 0,
  CLI_MODE_HANDLED_AND_EXIT      = 1,
  CLI_MODE_PASSTHROUGH           = 2,
  CLI_MODE_PASSTHROUGH_AND_EXIT  = 3,
};

struct CLIMode {
  // Short human-readable identifier ("help", "wizard", "confirm").
  // Used in debug logs and could surface in a future "what mode am I in"
  // prompt prefix.
  const char* name;

  // Called once when this mode becomes active. Use this to render the
  // initial screen (broadcastOutput(...)), allocate state inside userData,
  // set legacy globals like gCLIState if other code still reads them, etc.
  //
  // `userData` is the same pointer you passed to cliEnterMode (which was
  // copied from the CLIMode struct itself).
  void (*onEnter)(void* userData);

  // Called for each user command line while this mode is active. See the
  // CLIModeInputResult comments above for what to return.
  //
  // `line`   -- the raw command line as typed (with the command name still
  //             attached). Already trimmed; not lowercased.
  // `out`    -- a fixed-size response buffer the dispatcher will return to
  //             the user. Mode should `snprintf` or `strncpy` into it. Leaving
  //             it empty is fine.
  // `outSize`-- capacity of out, including the null terminator.
  CLIModeInputResult (*onInput)(const String& line, void* userData,
                                char* out, size_t outSize);

  // Called when the mode is about to be deactivated (either voluntarily via
  // CLI_MODE_HANDLED_AND_EXIT / CLI_MODE_PASSTHROUGH_AND_EXIT, or because
  // someone called cliExitMode() externally -- e.g. a logout, a transport
  // disconnect, a timeout). Use this to free userData state, restore legacy
  // globals, print a goodbye banner, etc.
  //
  // Receives the same userData pointer as the other callbacks.
  void (*onExit)(void* userData);

  // Per-mode opaque pointer. Lifetime is owned by the mode -- typically a
  // file-static struct or a heap allocation made in onEnter and freed in
  // onExit. The framework never dereferences it.
  void* userData;
};

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

// Activate `mode`. Calls mode->onEnter(mode->userData). Returns false if
// another mode is already active (caller should cliExitMode() first).
//
// The pointer must remain valid for the lifetime of the mode -- pass a
// pointer to a file-static or globally-allocated CLIMode, not a stack
// instance.
bool cliEnterMode(const CLIMode* mode);

// Deactivate the current mode. Calls mode->onExit(userData). No-op if no
// mode is active. After this, cliInModeActive() returns false.
void cliExitMode();

// True if any mode is currently active.
bool cliInModeActive();

// The currently-active mode (read-only), or nullptr if none.
const CLIMode* cliCurrentMode();

// Called by the command dispatcher BEFORE normal command lookup. If a mode
// is active, the mode's onInput is invoked.
//
// Returns true when the dispatcher should STOP -- i.e., the mode handled
// the input (CLI_MODE_HANDLED or CLI_MODE_HANDLED_AND_EXIT). In the latter
// case, the mode has already been exited by the time this returns.
//
// Returns false when the dispatcher should continue with normal command
// lookup -- either because no mode is active, or because the mode asked
// for passthrough. If the result was CLI_MODE_PASSTHROUGH_AND_EXIT the
// mode has already been exited by the time this returns.
bool cliModeDispatchInput(const String& line, char* out, size_t outSize);

#endif // SYSTEM_CLIMODE_H
