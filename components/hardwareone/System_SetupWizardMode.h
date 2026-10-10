#ifndef SYSTEM_SETUPWIZARDMODE_H
#define SYSTEM_SETUPWIZARDMODE_H

#include <Arduino.h>

// ============================================================================
// Setup Wizard — CLIMode-based, exact-session-owned
// ============================================================================
//
// Each input edits the shared wizard draft and returns, leaving cmd_exec free
// between keystrokes. Input belongs to the live stateful session that opened
// the mode; machine/stateless inputs continue through normal command dispatch.
// Serial and OLED reuse the same choices, navigation and rendering model as
// boot-time setup. Sub-pages collect one text field per input.
//
// Completion calls wizardSaveAndApplySettings once. Cancellation, inactivity
// timeout and session loss discard the draft. The boot-time entry point keeps
// its blocking input loop but uses this same draft and completion owner.
// OLED input is polled by onTick on the main task.
//
// Caller pattern (`cmd_featuresetup`):
//
//   const char* cmd_featuresetup(const String&) {
//     RETURN_VALID_IF_VALIDATE_CSTR();
//     if (!setupWizardMode_start()) {
//       return "Error: another interactive mode is active";
//     }
//     return "Wizard started. Use 'n'/'b'/numbers to navigate. "
//            "Type 'cancel' at any time to abort.";
//   }
//
// ----------------------------------------------------------------------------

// Enter the wizard CLIMode. Resets all wizard state, renders the first
// page, and registers the mode with the framework. Returns false if
// another mode is already active (caller should surface the error to
// the user and abort).
bool setupWizardMode_start();

// True if the wizard mode is currently the active CLIMode. Diagnostic
// helper; the framework itself uses cliCurrentMode() for the check.
bool setupWizardMode_isActive();

#endif // SYSTEM_SETUPWIZARDMODE_H
