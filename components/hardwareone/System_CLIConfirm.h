#ifndef SYSTEM_CLICONFIRM_H
#define SYSTEM_CLICONFIRM_H

#include <Arduino.h>

// ============================================================================
// CLI Confirm — yes/no prompt for destructive commands
// ============================================================================
//
// First concrete user of the CLIMode framework after help. Pattern:
//
//   1. A "destructive" CLI command (filedelete, userdelete, factoryreset)
//      validates its arguments, but instead of executing the action it stages
//      the payload locally and calls cliRequestConfirm() with a callback plus
//      an accepted-payload publisher. Shared pending state is committed only
//      when mode entry succeeds.
//   2. cliRequestConfirm enters a CLIMode that:
//        a. prints the prompt + a yes/no hint
//        b. captures the user's NEXT command line as the response
//        c. on "yes"/"y"/"true"/"1" -> schedules the onConfirm callback
//        d. on anything else        -> schedules the onCancel callback (or
//                                      returns a default "Cancelled" msg)
//        e. exits the mode after either branch fires
//   3. The destructive command returns immediately with a short hint
//      string ("Type 'yes' to confirm..."); the cmd_exec task is free
//      between the prompt and the user's response.
//
// Caller pattern (file-statics keep accepted state alive across both calls):
//
//   static String      sPendingPath;
//   static AuthContext sPendingCtx;
//   struct Candidate { const String* path; const AuthContext* ctx; };
//   static void commitCandidate(void* opaque) {
//     Candidate* c = static_cast<Candidate*>(opaque);
//     sPendingPath = *c->path;
//     sPendingCtx = *c->ctx;
//   }
//
//   static const char* doDelete(void* /*ud*/) {
//     // Pending values were atomically published when mode entry succeeded.
//     if (!VFS::removeGuarded(sPendingPath, sPendingCtx))
//       return "Error: delete failed";
//     static char buf[80]; snprintf(buf, sizeof(buf), "Deleted %s",
//                                   sPendingPath.c_str());
//     return buf;
//   }
//
//   const char* cmd_filedelete(const String& args) {
//     // ...validate args, compute path...
//     AuthContext caller = currentAuthContext();
//     Candidate candidate{&path, &caller};
//     if (!cliRequestConfirm("Delete " + path + "?",
//                            "filedelete " + path,   // originatingCmd for audit
//                            doDelete, nullptr, nullptr,
//                            commitCandidate, &candidate))
//       return "Error: another interactive mode is active";
//     return "Type 'yes' to confirm or anything else to cancel.";
//   }
//
// ----------------------------------------------------------------------------
// Why a single-slot static (not per-instance):
//
//   Only ONE CLIMode is active at a time. The confirm mode is no exception
//   -- there's at most one outstanding confirm prompt. So the framework
//   keeps the callbacks + userData in file-static state in
//   System_CLIConfirm.cpp. Callers store their own per-confirm state
//   (path, captured AuthContext, etc.) in THEIR own statics. Two callers
//   can't request a confirm simultaneously: cliRequestConfirm returns
//   false when a mode is already active.
//
// ----------------------------------------------------------------------------
// Security boundary:
//
//   The framework binds the prompt to the exact transport-session generation
//   that opened it. A different browser cookie, physical-console login, or
//   same-user re-login cannot answer it. Effective role is re-checked when
//   "yes" arrives, and stateless/machine sources (including BLE in this phase)
//   cannot open an interactive prompt. The action/cancel callback runs on
//   cmd_exec after the global mode mutex is released.
//
// ----------------------------------------------------------------------------

// Callback signature. `userData` is the same pointer passed to
// cliRequestConfirm. Returns the response string sent back to whoever
// submitted the yes/no line. The pointer must remain valid until the
// next CLI command returns -- the dispatcher copies the string out
// before that. File-static char buffers are the simplest choice.
typedef const char* (*CLIConfirmCallback)(void* userData);
typedef void (*CLIConfirmAcceptedCallback)(void* acceptedData);

// Request a yes/no confirmation. See header comment for the typical
// caller pattern.
//
// Returns true if the confirm mode was entered. Returns false if another
// CLIMode is already active (help, wizard, or a prior outstanding
// confirm) -- caller must NOT execute the destructive action and should
// surface an error to the user.
//
// `prompt`         - returned to the initiating caller through the addressed
//                    command result. Keep it under ~150 chars; the confirm
//                    framework copies it into a fixed-size buffer.
// `originatingCmd` - the command line that triggered this confirm prompt
//                    (e.g. "filedelete /system/foo"). The confirm
//                    framework includes this in the audit log entry that
//                    fires when the user resolves the prompt:
//                      [CMD] user@src: filedelete /system/foo (confirm: yes)
//                        -> Deleted file: /system/foo
//                    That gives forensic auditors a single line showing
//                    what was asked, what was answered, and what
//                    happened. Pass an empty String() to opt out (then
//                    the audit line just shows "yes"/"no" as the cmd).
// `onConfirm`      - called when the user types yes/y/true/1. Must be
//                    non-null (otherwise nothing happens on confirm; the
//                    framework will substitute a placeholder response).
// `onCancel`       - called for any other response. Pass nullptr to use
//                    the default "Cancelled." reply.
// `userData`       - opaque, passed through to both callbacks. The
//                    framework never dereferences it.
bool cliRequestConfirm(const String& prompt,
                       const String& originatingCmd,
                       CLIConfirmCallback onConfirm,
                       CLIConfirmCallback onCancel,
                       void* userData,
                       CLIConfirmAcceptedCallback onAccepted = nullptr,
                       void* acceptedData = nullptr);

// Addressed response prepared by the last successful cliRequestConfirm().
// Destructive handlers should return this instead of broadcasting the prompt.
const char* cliConfirmPromptResponse();

#endif // SYSTEM_CLICONFIRM_H
