#ifndef G2_HIJACK_CMD_H
#define G2_HIJACK_CMD_H

// =============================================================================
// G2 hijack command bridge — routes glasses-tap-driven system mutations
// through cmd_exec_task instead of running them inline on g2_tap_disp.
// =============================================================================
// See docs/G2_REFACTOR_PROPOSAL.md §5 for the full architectural rationale.
//
// Usage pattern (from a hijack tap handler):
//
//   G2CmdCookie cookie = {
//     .seq          = 0,                         // (step 2 will assign from gCmdSeq)
//     .menuGen      = 0,                         // (step 2 will snapshot gMenuGen)
//     .targetPage   = g2GetHijackPage(),
//     .targetNetSub = (uint8_t)gNetSub,
//   };
//   g2SubmitHijackCommand("wifi disconnect", cookie, onWifiDoneCb, nullptr);
//
// The callback runs on cmd_exec_task. It must NOT touch the lens directly —
// instead it should enqueue a LensUiJob (introduced in step 3) gated by the
// cookie's gen/seq. See proposal §5.4 for the staleness contract.
// =============================================================================

#include "System_BuildConfig.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>
#include <stdint.h>
#include "G2_Glasses.h"            // G2HijackPage
#include "System_CommandTypes.h"   // ExecAsyncCallback (forward used by callers)
#include "System_User.h"           // AuthContext (for g2HijackAuthContext / G2HijackCtxGuard)
#include "System_AuthIdentity.h"   // ExecIdentityGuard (wrapped by G2HijackCtxGuard)

// Snapshot of UI state at the moment a hijack command is submitted.
// The completion callback uses this to detect staleness — if the user has
// navigated away (gMenuGen changed) or a newer command for the same
// subsystem has completed (cmdSeq comparison), the lens redraw is dropped.
//
// Caller populates targetPage / targetNetSub before calling
// g2SubmitHijackCommand(). The helper itself overwrites `seq` (assigning
// the next gCmdSeq value) and `menuGen` (snapshotting the current value)
// — anything the caller wrote into those fields is ignored.
struct G2CmdCookie {
  uint64_t      seq;          // [helper-filled] monotonic per-submission id
  uint32_t      menuGen;      // [helper-filled] navigation generation at submit time
  G2HijackPage  targetPage;   // [caller-filled] hijack page at submit time
  uint8_t       targetNetSub; // [caller-filled] NetworkSubMode value
};

// =============================================================================
// Generation counter — bumped on every navigation that invalidates a
// previously-captured UI snapshot. Producers (page handlers, text-entry
// transitions, gNetSub mutations, hijack-page changes) call g2BumpMenuGen().
// Consumers (cmd-completion callbacks, future lens applier) compare against
// the cookie's snapshot to detect staleness.
//
// Spurious bumps are safe: they invalidate snapshots that didn't strictly
// need invalidating, but never incorrectly apply a stale one.
// =============================================================================
void     g2BumpMenuGen();
uint32_t g2CurrentMenuGen();

// Read-only accessor for the most recently assigned command sequence.
// Useful for "latest-wins" subsystem policy in the lens applier (step 3).
uint64_t g2CurrentCmdSeq();

// Hijack-command completion callback. Receives the cookie back so the
// implementation can apply gen/seq staleness checks before queueing UI work.
typedef void (*G2HijackCmdCallback)(bool ok,
                                    const char* result,
                                    const G2CmdCookie& cookie,
                                    void* userData);

// Submit a hijack-originated command to cmd_exec_task.
//
// `line`     : command line, same form as serial/web (e.g. "wifi disconnect").
// `cookie`   : caller-populated UI snapshot. The helper does NOT mutate it; it
//              copies the cookie into a heap wrapper that survives until the
//              completion callback fires.
// `callback` : invoked on cmd_exec_task with (ok, result, cookie, userData).
//              May be nullptr for fire-and-forget submissions.
// `userData` : opaque pointer forwarded to the callback. Lifetime is the
//              caller's responsibility.
//
// Returns true if the request was queued. False means the cmd_exec queue was
// full or allocation failed; the caller should surface this on the lens (e.g.
// a toast) rather than retry silently.
bool g2SubmitHijackCommand(const char* line,
                           const G2CmdCookie& cookie,
                           G2HijackCmdCallback callback,
                           void* userData);

// =============================================================================
// AuthContext for in-callback hijack work (no cmd_exec dispatch).
//
// g2SubmitHijackCommand sends taps through cmd_exec_task, which installs the
// command's identity (cmd.ctx.auth.user = pairedByUser) into the executor
// task's TLS slot for the duration of the command. That covers any tap that
// mutates state via a CLI command.
//
// But some hijack handlers — notably the file browser (g2ShowFilesMenu /
// g2FilesHandleTap) — don't go through cmd_exec. They run synchronously in
// the BLE callback and call FileManager / VFS::*Guarded directly. Those
// reads need an AuthContext too: without one the task's TLS slot is still
// ANON and every guarded call fails "anonymous never permitted".
//
// g2HijackAuthContext() builds the same identity g2SubmitHijackCommand uses
// (pairedByUser, transport=SOURCE_LOCAL_DISPLAY). G2HijackCtxGuard is a thin
// wrapper around ExecIdentityGuard that installs it into the calling task's
// slot for the lifetime of the scope — drop one at the top of every
// direct-FS hijack callback.
//
// Glasses inherit the privileges of their pairer: admin pairs → admin
// reads, regular user pairs → user-level reads. If pairedByUser is blank
// (legacy peer paired before this field existed), the guard installs an
// empty user → all guarded reads fail. To recover, re-run
// `bleautoconnect g2-glasses on` which stamps pairedByUser from the
// caller's currentAuthContext().user via bleSavePeerMac.
// =============================================================================
AuthContext g2HijackAuthContext();

class G2HijackCtxGuard {
 public:
  G2HijackCtxGuard();
  ~G2HijackCtxGuard() = default;
  G2HijackCtxGuard(const G2HijackCtxGuard&) = delete;
  G2HijackCtxGuard& operator=(const G2HijackCtxGuard&) = delete;
 private:
  ExecIdentityGuard guard_;
};

// =============================================================================
// LensUiJob — unified message type for the lens applier queue.
// =============================================================================
// Step 3 introduces this as a wrapper around the existing PageSwapArgs path.
// The persistent worker keeps its name ("g2_page_swap_w") and 4 KB stack for
// now; only its message type and dispatch shape change. Steps 4–5 will add
// the other LensJobKind variants — Redraw/Toast/Notify/Custom — to subsume
// notifyClearTaskBody, livePageWorker, liveTextWorker, and the future
// cmd-completion redraw path.
//
// Producers stamp the cookie fields (submitMenuGen / targetPage /
// targetNetSub) at enqueue time. The applier checks submitMenuGen against
// the live gMenuGen and logs (step 3) or drops (step 4 once
// G2_LENS_GEN_GUARD=1) stale jobs.
// =============================================================================

// Forward decls — concrete payload structs live in G2_Glasses.cpp (PageSwapArgs)
// or will be added in later steps. LensUiJob only stores pointers, so callers
// don't need full definitions to construct one.
struct PageSwapArgs;
struct ToastSpec;

// Custom payload — runs an arbitrary function on the lens applier worker.
// Used by step 5b/Group D for one-shot work that needs to happen off the
// BLE notify task but doesn't fit Redraw semantics (e.g. hijack bootstrap,
// which is the very first thing on a new hijack session — no view exists
// yet to "redraw"). Not gen-guarded by the applier; the run function is
// expected to do its own state checks if it cares.
struct CustomSpec {
  void (*run)();
};

// Notify payload — fires when a notification's auto-clear timer expires.
// Carries the gNotifyGen value captured when the notification was shown so
// the lens applier can drop the clear if a newer notification has replaced
// the one this timer was for. Step 5 uses this to retire notifyClearTaskBody
// (which spawned a fresh task per notification — see proposal §6.5).
struct NotifySpec {
  uint32_t gen;   // gNotifyGen at submission; applier compares against live value
};

// Redraw payload — re-render a view from canonical state. Step 4 uses this
// for cmd-completion callbacks (e.g. the WiFi Auto-Start toggle re-renders
// the WiFi menu after the underlying setting was persisted via cmd_exec).
//
// `render` is called by the lens applier on the page-swap worker context
// AFTER the gen-guard check passes. It must be safe to call there — i.e.
// the same context the existing page render functions already run in.
// The applier deletes the RedrawSpec after `render` returns.
struct RedrawSpec {
  void (*render)();
};

enum class LensJobKind : uint8_t {
  PageSwap = 0,   // existing page-swap (CREATE/SHUTDOWN protocol). Step 3 wires this.
  Redraw,         // re-render current view from canonical state (step 5+)
  Toast,          // transient overlay on the lens (step 4+)
  Notify,         // notification + clear-timer (step 5 — subsumes notifyClearTaskBody)
  Custom,         // typed one-off (BMP push, camera frame — step 6+)
};

struct LensUiJob {
  LensJobKind   kind;
  uint32_t      submitMenuGen;   // captured at enqueue time
  uint64_t      cmdSeq;          // 0 for navigation-origin (no command in flight)
  G2HijackPage  targetPage;      // viewId snapshot at submit time
  uint8_t       targetNetSub;    // NetworkSubMode value (0 if not captured)
  union Payload {
    PageSwapArgs* swap;
    RedrawSpec*   redraw;
    ToastSpec*    toast;
    NotifySpec*   notify;
    CustomSpec*   custom;
  } payload;
};

// Compile-time enforcement of the staleness drop. OFF by default in step 3
// (the applier logs mismatches but still dispatches). Step 4 will flip this
// once the first cmd-completion callback depends on the drop semantics.
#ifndef G2_LENS_GEN_GUARD
#define G2_LENS_GEN_GUARD 0
#endif

// Enqueue a fully-prepared LensUiJob (heap-owned, allocated with `new`).
// On success the lens applier takes ownership and deletes after dispatch.
// On failure (queue not initialised or full) ownership stays with the
// caller — the caller must free both the payload and the LensUiJob itself.
//
// Step 3 has no in-tree callers; step 4+ will use this from cmd-completion
// callbacks running on cmd_exec_task.
bool g2EnqueueLensJob(LensUiJob* job);

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif // G2_HIJACK_CMD_H
