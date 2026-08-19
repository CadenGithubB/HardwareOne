#ifndef SYSTEM_CLIMODE_H
#define SYSTEM_CLIMODE_H

#include <Arduino.h>
#include "System_User.h"

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
//   1. A wizard can be invoked from a stateful interactive transport with a
//      live generation (human web CLI, serial, or OLED). Machine/stateless
//      sources such as CM5 UART traffic, G2 callbacks, MQTT, automation and
//      the currently non-transactional BLE link remain mode-independent.
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
// Only ONE mode is active at a time. It is bound to the exact transport login
// generation that opened it. Input from every other session bypasses the mode
// and continues through ordinary registry dispatch without mutating it.
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

  // Called for each owning-session command line while this mode is active.
  // The slot remains reserved, but the global mode mutex is NOT held across
  // this callback, so filesystem/network work cannot invert lifecycle locks.
  // See the CLIModeInputResult comments above for what to return.
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

  // Optional periodic tick. Called by cliModeTick() from the main loop
  // every iteration while this mode is active. Use this for non-textual
  // input that arrives outside the command dispatcher -- e.g. the setup
  // wizard polls OLED joystick state in its onTick so joystick presses
  // translate into page transitions without needing to flow through the
  // CLI input pipeline.
  //
  // KEEP THIS FAST. It's called from the main task's loop and competes
  // for the same time budget as oledUpdate, the serial-CLI drain, and
  // every other periodic concern. Sub-millisecond work only. If a tick
  // needs to do something heavier, mark a flag and schedule the work
  // on a different task / esp_timer.
  //
  // Pass nullptr if the mode is purely input-driven (help, confirm).
  void (*onTick)(void* userData);

  // Per-mode opaque pointer. Lifetime is owned by the mode -- typically a
  // file-static struct or a heap allocation made in onEnter and freed in
  // onExit. The framework never dereferences it.
  void* userData;

  // Monotonic idle timeout. Measured with esp_timer_get_time(), never with
  // RTC/Unix/NTP time, so acquiring NTP mid-session has no effect. Zero uses
  // the framework default (5 minutes).
  uint32_t idleTimeoutMs = 0;
};

// Synchronous, non-retained preparation hook used when a mode and its
// caller-owned payload must become visible atomically. It runs only after the
// exact owner was validated and the global slot was proven empty, while the
// mode mutex is held. The hook must be short, cannot fail, and must not block.
using CLIModeEntryCommit = void (*)(void* data);

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

// Activate `mode` for the current queued CommandContext. Entry fails for a
// stateless/stale session or a MODE_INDEPENDENT machine invocation.
//
// The pointer must remain valid for the lifetime of the mode -- pass a
// pointer to a file-static or globally-allocated CLIMode, not a stack
// instance.
bool cliEnterMode(const CLIMode* mode);

bool cliEnterModePrepared(const CLIMode* mode,
                          CLIModeEntryCommit commit,
                          void* commitData);

// Request deactivation only when the current command owns the mode. A
// no-context main-loop call is accepted only for a local-display-owned mode
// (the OLED wizard tick). Cleanup/onExit is drained on cmd_exec; true means the
// exit request was accepted, not necessarily that cleanup already completed.
bool cliExitMode();

// True if any mode is currently active.
bool cliInModeActive();

// The currently-active mode (read-only), or nullptr if none.
const CLIMode* cliCurrentMode();

// True only when the current queued command owns the active mode. `expected`
// optionally narrows the check to one mode (e.g. help).
bool cliModeCurrentCommandOwns(const CLIMode* expected = nullptr);

// Cross-task delivery check for the command submitter after cmd_exec returns.
// Used to keep an interactive prompt's result off shared mirror/log lanes
// while still returning it directly to the owning serial/web/OLED session.
bool cliModeOwnedBySession(CommandSource source,
                           TransportSessionEpoch epoch,
                           const CLIMode* expected = nullptr);

// True when the current command is allowed to participate in an interactive
// mode. Validation and MODE_INDEPENDENT machine contexts return false. Output
// capture is orthogonal: the human web terminal captures its addressed reply.
bool cliModeCurrentInvocationCanInteract();

// Refresh the active mode's monotonic idle timer for an exact owner input.
// The no-command form is accepted only for local-display physical input.
bool cliModeNoteActivity();

// True while the physical serial console owns a live interactive slot. The
// debug drain uses this to suppress only ambient serial lines (other sinks keep
// running), so background logs cannot overwrite a human prompt.
bool cliModeSuppressesAmbientSerial();

// Monotonic nonzero instance id of the active mode, or zero. Used to detect
// that a handler entered a mode without relying on a racy global bool.
uint32_t cliModeInstanceId();

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

// Called periodically from the main task's loop(). If a mode is active
// and has an onTick callback, invokes it. Used by the wizard mode to
// poll OLED joystick state and translate presses into page transitions
// without needing to route through the CLI input pipeline.
//
// Safe to call when no mode is active (no-op) or when the active mode
// has no onTick (also no-op).
void cliModeTick();

// Run pending onExit cleanup on cmd_exec_task. Queries and transport callback
// tasks only request cancellation; they never execute mode cleanup themselves.
bool cliModeExecutorDrainPending();

#endif // SYSTEM_CLIMODE_H
