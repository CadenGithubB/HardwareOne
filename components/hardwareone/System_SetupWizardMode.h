#ifndef SYSTEM_SETUPWIZARDMODE_H
#define SYSTEM_SETUPWIZARDMODE_H

#include <Arduino.h>

// ============================================================================
// Setup Wizard — CLIMode-based, exact-session-owned
// ============================================================================
//
// Phase 5 of the CLIMode rollout. Replaces the synchronous-blocking
// runSetupWizard() for `cmd_featuresetup` invocations: instead of parking
// the cmd_exec task in waitForSerialInputBlocking() for the duration of
// the wizard, each user input is one short handler call that mutates
// persistent state and returns. The cmd_exec task is free between
// keystrokes. The wizard accepts input only from the live stateful session
// that opened it (human web CLI, serial, or OLED); machine/stateless inputs
// continue through normal command dispatch without touching wizard state.
//
// What stays the same:
//   - The wizard's logical state model (SetupWizardPage, currentSelection,
//     toggles, timezone/LED/etc. selectors) is unchanged. All the
//     existing state mutators (wizardToggleCurrentItem, wizardNextPage,
//     setWizardTimezoneSelection, ...) and the rendering helpers
//     (printSerialPageStatus, render*Page) are reused as-is.
//   - The result struct (SetupWizardResult) is unchanged.
//   - WiFi credentials and settings still get applied via
//     wizardFinalize() at completion.
//
// What's different:
//   - One CLIMode owns the wizard. cmd_featuresetup enters it and
//     returns immediately. The user's next CLI input is interpreted
//     as wizard navigation (n / b / number / field value).
//   - Sub-pages (ESPNOW identity, MQTT broker config, WiFi credentials)
//     are unrolled into linear sub-modes: one user input per field,
//     no inner while(!Serial.available()) loops.
//   - OLED joystick input (when the user is invoking featuresetup
//     from an OLED-equipped device) flows through the CLIMode's
//     onTick callback, called from the main task's loop().
//
// What is NOT migrated by this phase:
//   - The boot-time first-time-setup path (firstTimeSetupIfNeeded ->
//     runAndApplyFeatureWizard) keeps using the old synchronous
//     runSetupWizard(). That path has no cmd_exec hostage problem
//     because cmd_exec doesn't exist at boot yet; it's running on the
//     main task as part of app initialization. The OLED + joystick
//     polling integrates naturally there. A future phase can unify
//     FTS too if motivated, but the cost/benefit is much lower.
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
