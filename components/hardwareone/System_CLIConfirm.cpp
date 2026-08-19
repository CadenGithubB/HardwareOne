// System_CLIConfirm.cpp
//
// Implementation of the yes/no confirm mode built on the CLIMode
// framework. See System_CLIConfirm.h for the caller pattern + design
// notes.

#include "System_CLIConfirm.h"
#include "System_CLIMode.h"
#include "System_CommandTypes.h"
#include "System_Debug.h"
#include "System_Utils.h"          // broadcastOutput, logCommandExecution
#include "System_AuthIdentity.h"   // currentAuthContext — audit attribution for the confirmer
#include "System_User.h"

// Single-slot state for the pending confirm. Publication is committed under
// the CLIMode mutex only after the exact owner and empty slot are validated.
// A losing foreign request therefore cannot overwrite the winning payload.
static struct {
  enum Resolution : uint8_t {
    None,
    Confirmed,
    Cancelled,
    PrivilegeLost,
  } resolution;
  CLIConfirmCallback onConfirm;
  CLIConfirmCallback onCancel;
  void* userData;
  char* responseOut;
  size_t responseOutSize;
  // Stored prompt for re-display from onEnter. Sized large enough for
  // "Confirm delete of /some/long/path? (cannot be undone)" with
  // headroom. Truncated with snprintf if a caller passes more.
  char prompt[192];
  // Originating command line (e.g. "filedelete /system/foo") -- preserved
  // for the resolution audit so forensic logs show what was confirmed,
  // not just the bare "yes" / "no". Empty when the caller opted out.
  char originatingCmd[160];
  char responseText[384];
  int requiredRoleRank;
} sConfirm = { decltype(sConfirm)::None, nullptr, nullptr, nullptr, nullptr, 0,
               "", "", "", kRoleRankGuest };

static void confirm_onEnter(void* /*userData*/) {
  // The initiating handler returns responseText through the normal addressed
  // result channel. Do not broadcast a potentially sensitive prompt into the
  // shared web/BLE/debug lanes.
  const CommandContext* ctx =
      static_cast<const CommandContext*>(currentCommandContext());
  if (ctx && ctx->auth.transport == SOURCE_LOCAL_DISPLAY) {
    broadcastOutputCore_Routed(sConfirm.responseText,
                               strlen(sConfirm.responseText),
                               MSG_ROUTE_OLED);
  }
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

  if (yes) {
    if (userAccountRank(currentAuthContext().user) < sConfirm.requiredRoleRank) {
      sConfirm.resolution = decltype(sConfirm)::PrivilegeLost;
    } else {
      sConfirm.resolution = decltype(sConfirm)::Confirmed;
    }
  } else {
    sConfirm.resolution = decltype(sConfirm)::Cancelled;
  }
  // The dispatcher drains onExit synchronously on cmd_exec before returning
  // from this input. Keep the caller's live response buffer so onExit can run
  // the potentially blocking/destructive callback *outside* the global mode
  // mutex, then place the final result into the normal addressed reply.
  sConfirm.responseOut = out;
  sConfirm.responseOutSize = outSize;

  // Mode is done either way -- exit so the next user input goes through
  // normal command dispatch again.
  return CLI_MODE_HANDLED_AND_EXIT;
}

static void confirm_onExit(void* /*userData*/) {
  if (sConfirm.resolution != decltype(sConfirm)::None) {
    const bool yes = sConfirm.resolution == decltype(sConfirm)::Confirmed ||
                     sConfirm.resolution == decltype(sConfirm)::PrivilegeLost;
    const char* response = nullptr;
    if (sConfirm.resolution == decltype(sConfirm)::PrivilegeLost) {
      response = "Error: session privileges changed; confirmation cancelled.";
    } else if (sConfirm.resolution == decltype(sConfirm)::Confirmed) {
      response = sConfirm.onConfirm
                     ? sConfirm.onConfirm(sConfirm.userData)
                     : "Confirmed (no action registered).";
    } else {
      response = sConfirm.onCancel
                     ? sConfirm.onCancel(sConfirm.userData)
                     : "Cancelled.";
    }
    if (!response) response = "Error: confirmation callback returned no result.";

    if (response && sConfirm.responseOut && sConfirm.responseOutSize > 0) {
      strncpy(sConfirm.responseOut, response, sConfirm.responseOutSize - 1);
      sConfirm.responseOut[sConfirm.responseOutSize - 1] = '\0';
    }

    // Audit the resolution only after the action/cancellation callback has
    // actually completed. Attribution remains the exact confirmer whose
    // CommandContext is still installed on cmd_exec during this drain.
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

  // Defensive zeroing so a stale callback can't be invoked accidentally
  // if cliEnterMode is called again before the next cliRequestConfirm.
  sConfirm.resolution        = decltype(sConfirm)::None;
  sConfirm.onConfirm         = nullptr;
  sConfirm.onCancel          = nullptr;
  sConfirm.userData          = nullptr;
  sConfirm.responseOut       = nullptr;
  sConfirm.responseOutSize   = 0;
  sConfirm.prompt[0]         = '\0';
  sConfirm.originatingCmd[0] = '\0';
  sConfirm.responseText[0]   = '\0';
  sConfirm.requiredRoleRank  = kRoleRankGuest;
  DEBUGF(DEBUG_CLI, "[climode/confirm] exited");
}

static const CLIMode kConfirmMode = {
  "confirm",
  confirm_onEnter,
  confirm_onInput,
  confirm_onExit,
  nullptr,  // onTick — confirm is purely input-driven
  nullptr,  // userData — state lives in sConfirm static
  2u * 60u * 1000u,
};

struct ConfirmPrepare {
  const String* prompt;
  const String* originatingCmd;
  CLIConfirmCallback onConfirm;
  CLIConfirmCallback onCancel;
  void* userData;
  CLIConfirmAcceptedCallback onAccepted;
  void* acceptedData;
  int requiredRoleRank;
};

static void confirmCommit(void* opaque) {
  ConfirmPrepare* p = static_cast<ConfirmPrepare*>(opaque);
  if (!p) return;
  sConfirm.onConfirm = p->onConfirm;
  sConfirm.onCancel = p->onCancel;
  sConfirm.userData = p->userData;
  snprintf(sConfirm.prompt, sizeof(sConfirm.prompt), "%s",
           p->prompt ? p->prompt->c_str() : "");
  snprintf(sConfirm.originatingCmd, sizeof(sConfirm.originatingCmd), "%s",
           p->originatingCmd ? p->originatingCmd->c_str() : "");
  snprintf(sConfirm.responseText, sizeof(sConfirm.responseText),
           "%s\nType 'yes' to confirm or anything else to cancel.",
           sConfirm.prompt);
  sConfirm.requiredRoleRank = p->requiredRoleRank;
  if (p->onAccepted) p->onAccepted(p->acceptedData);
}

bool cliRequestConfirm(const String& prompt,
                       const String& originatingCmd,
                       CLIConfirmCallback onConfirm,
                       CLIConfirmCallback onCancel,
                       void* userData,
                       CLIConfirmAcceptedCallback onAccepted,
                       void* acceptedData) {
  ConfirmPrepare prepare{
      &prompt,
      &originatingCmd,
      onConfirm,
      onCancel,
      userData,
      onAccepted,
      acceptedData,
      userAccountRank(currentAuthContext().user),
  };
  return cliEnterModePrepared(&kConfirmMode, confirmCommit, &prepare);
}

const char* cliConfirmPromptResponse() {
  return sConfirm.responseText[0]
             ? sConfirm.responseText
             : "Type 'yes' to confirm or anything else to cancel.";
}
