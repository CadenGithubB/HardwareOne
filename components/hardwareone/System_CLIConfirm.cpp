// System_CLIConfirm.cpp
//
// Implementation of the yes/no confirm mode built on the CLIMode
// framework. See System_CLIConfirm.h for the caller pattern + design
// notes.

#include "System_CLIConfirm.h"
#include "System_CLIMode.h"
#include "System_Debug.h"
#include "System_Utils.h"          // broadcastOutput, logCommandExecution
#include "System_AuthIdentity.h"   // currentAuthContext — audit attribution for the confirmer

// Single-slot state for the pending confirm. Touched only from cmd_exec
// (the same task that runs every CLI handler and the CLIMode dispatch
// hook), so no synchronization is needed today. If the framework ever
// runs on multiple tasks concurrently this becomes per-task state -- but
// that's the same redesign trigger noted in System_CLIMode.cpp.
static struct {
  CLIConfirmCallback onConfirm;
  CLIConfirmCallback onCancel;
  void* userData;
  // Stored prompt for re-display from onEnter. Sized large enough for
  // "Confirm delete of /some/long/path? (cannot be undone)" with
  // headroom. Truncated with snprintf if a caller passes more.
  char prompt[192];
  // Originating command line (e.g. "filedelete /system/foo") -- preserved
  // for the resolution audit so forensic logs show what was confirmed,
  // not just the bare "yes" / "no". Empty when the caller opted out.
  char originatingCmd[160];
} sConfirm = { nullptr, nullptr, nullptr, "", "" };

static void confirm_onEnter(void* /*userData*/) {
  // Print the prompt and a hint. The destructive command itself returns
  // a short acknowledgement after this so the user sees:
  //   <prompt>
  //   Type 'yes' to confirm or anything else to cancel.
  //   Type 'yes' to confirm or anything else to cancel.   <-- command result
  // Wait -- that would double-print the hint. So onEnter only prints the
  // prompt; the destructive command returns the hint as its response.
  broadcastOutput(sConfirm.prompt);
}

static CLIModeInputResult confirm_onInput(const String& line, void* /*userData*/,
                                          char* out, size_t outSize) {
  String lc = line;
  lc.trim();
  lc.toLowerCase();

  // Match CommandArgs::argBool() semantics for consistency: "yes", "y",
  // "true", "1", "on" all count as yes. Everything else cancels --
  // including bare empty input (user hit Enter without typing), to bias
  // toward NOT performing the destructive action.
  const bool yes = (lc == "yes" || lc == "y" || lc == "true" ||
                    lc == "1"   || lc == "on");

  const char* response = nullptr;

  if (yes) {
    if (sConfirm.onConfirm) {
      response = sConfirm.onConfirm(sConfirm.userData);
    } else {
      // Misconfigured caller -- no action wired. Don't pretend something
      // happened.
      response = "Confirmed (no action registered).";
    }
  } else {
    if (sConfirm.onCancel) {
      response = sConfirm.onCancel(sConfirm.userData);
    } else {
      response = "Cancelled.";
    }
  }

  if (response && out && outSize > 0) {
    strncpy(out, response, outSize - 1);
    out[outSize - 1] = '\0';
  }

  // Audit the confirm resolution with full context. The prompt step
  // (e.g. "filedelete /foo") was deliberately NOT audited by the
  // dispatcher because it only requested confirmation -- no action
  // completed. Now that the user has resolved the prompt and a real
  // action (or cancellation) has happened, write the audit entry.
  //
  // The composed line shows what was asked + what was answered:
  //   filedelete /foo (confirm: yes)    -> Deleted file: /foo
  //   filedelete /foo (confirm: no)     -> Cancelled. /foo not deleted.
  //   userdelete bob  (confirm: yes)    -> Deleted user 'bob'
  //
  // Attribution is the CONFIRMER's identity (currentAuthContext) -- the
  // person who answered yes/no -- because that's the auditable decision
  // point. The originating command author is reflected in the cmd
  // string itself. Destructive actions ran against the ORIGINAL caller's
  // captured AuthContext for permission purposes (see filedelete_confirmed
  // etc.), but the audit attribution lives with the confirmer.
  if (response) {
    char auditCmd[224];
    if (sConfirm.originatingCmd[0] != '\0') {
      snprintf(auditCmd, sizeof(auditCmd), "%s (confirm: %s)",
               sConfirm.originatingCmd, yes ? "yes" : "no");
    } else {
      // Caller opted out of originating-cmd context -- just log the
      // bare yes/no resolution.
      snprintf(auditCmd, sizeof(auditCmd), "(confirm: %s)", yes ? "yes" : "no");
    }
    const bool actionSucceeded =
      (strncmp(response, "Error", 5) != 0) && (strncmp(response, "ERROR", 5) != 0);
    logCommandExecution(currentAuthContext(), auditCmd, actionSucceeded, response);
  }

  // Mode is done either way -- exit so the next user input goes through
  // normal command dispatch again.
  return CLI_MODE_HANDLED_AND_EXIT;
}

static void confirm_onExit(void* /*userData*/) {
  // Defensive zeroing so a stale callback can't be invoked accidentally
  // if cliEnterMode is called again before the next cliRequestConfirm.
  sConfirm.onConfirm         = nullptr;
  sConfirm.onCancel          = nullptr;
  sConfirm.userData          = nullptr;
  sConfirm.prompt[0]         = '\0';
  sConfirm.originatingCmd[0] = '\0';
  DEBUGF(DEBUG_CLI, "[climode/confirm] exited");
}

static const CLIMode kConfirmMode = {
  "confirm",
  confirm_onEnter,
  confirm_onInput,
  confirm_onExit,
  nullptr,  // onTick — confirm is purely input-driven
  nullptr,  // userData — state lives in sConfirm static
};

bool cliRequestConfirm(const String& prompt,
                       const String& originatingCmd,
                       CLIConfirmCallback onConfirm,
                       CLIConfirmCallback onCancel,
                       void* userData) {
  if (cliInModeActive()) {
    // Another mode (help, wizard, or a prior outstanding confirm) holds
    // the slot. Caller must surface this to the user and abort.
    DEBUGF(DEBUG_CLI, "[climode/confirm] rejected: '%s' already active",
           cliCurrentMode() && cliCurrentMode()->name ? cliCurrentMode()->name : "(unnamed)");
    return false;
  }

  // Stash the callbacks + audit context BEFORE entering the mode so
  // onEnter (which prints the prompt) sees the populated buffers.
  sConfirm.onConfirm = onConfirm;
  sConfirm.onCancel  = onCancel;
  sConfirm.userData  = userData;
  snprintf(sConfirm.prompt,         sizeof(sConfirm.prompt),         "%s", prompt.c_str());
  snprintf(sConfirm.originatingCmd, sizeof(sConfirm.originatingCmd), "%s", originatingCmd.c_str());

  if (!cliEnterMode(&kConfirmMode)) {
    // Defensive: cliEnterMode rejected for some other reason. Clear state.
    sConfirm.onConfirm         = nullptr;
    sConfirm.onCancel          = nullptr;
    sConfirm.userData          = nullptr;
    sConfirm.prompt[0]         = '\0';
    sConfirm.originatingCmd[0] = '\0';
    return false;
  }
  return true;
}
