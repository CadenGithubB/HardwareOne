#ifndef G2_HIJACKCMD_H
#define G2_HIJACKCMD_H

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
#include "System_AuthIdentity.h"   // CommandIdentityScope (wrapped by G2HijackCtxGuard)
                                   // — transitively pulls System_Notifications.h
#include "BLE_Peers.h"            // BlePeerOwnerSession generation fence

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
  uint8_t       targetNetSub; // [caller-filled] page-specific sub-mode value
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
// Returns true if the request was queued. False means blank pairedByUser,
// cmd_exec queue full, or allocation failed. Callers must treat false as a
// hard no-op — never run the mutation inline on the tap dispatcher (mixes
// command execution with UI work and skips authorizeCommand). Optional: show
// a "busy" toast.
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
// g2FilesHandleTap) — don't go through cmd_exec. They run synchronously on
// g2_tap_disp and call FileManager / VFS::*Guarded directly. Those
// reads need an AuthContext too: without one the task's TLS slot is still
// ANON and every guarded call fails "anonymous never permitted".
//
// g2HijackAuthContext() is the single source of truth for "what AuthContext
// represents a tap from the lens" — pairedByUser as the user, transport =
// SOURCE_G2_GLASSES. Used by:
//   * g2SubmitHijackCommand (async submit path — copies into cmd.ctx.auth)
//   * G2HijackCtxGuard      (sync direct-FS path — feeds CommandIdentityScope)
//   * any future site that needs to talk on behalf of the lens
//
// G2HijackCtxGuard is sugar over CommandIdentityScope: drop one at the top
// of every direct-FS hijack callback and you get the paired-user identity +
// NOTIF_SOURCE_G2 + paired-user-as-subsource installed in the calling
// task's TLS slots for the lifetime of the scope. Both inner guards (via
// CommandIdentityScope) save/restore the prior TLS state, so nesting and
// early-return unwind cleanly.
//
// Glasses inherit the privileges of their pairer: admin pairs → admin
// reads, regular user pairs → user-level reads. If pairedByUser is blank
// (legacy peer paired before this field existed), the guard installs an
// empty user → all guarded reads fail. To recover, re-run
// `bleautoreconnect g2-glasses on` which stamps pairedByUser from the
// caller's currentAuthContext().user via bleSavePeerMac.
// =============================================================================
AuthContext g2HijackAuthContext();

class G2HijackCtxGuard {
 public:
  G2HijackCtxGuard();
  ~G2HijackCtxGuard() = default;
  G2HijackCtxGuard(const G2HijackCtxGuard&) = delete;
  G2HijackCtxGuard& operator=(const G2HijackCtxGuard&) = delete;
  G2HijackCtxGuard(G2HijackCtxGuard&&) = delete;
  G2HijackCtxGuard& operator=(G2HijackCtxGuard&&) = delete;
  // Pair ownership can change while a synchronous page callback is doing
  // work. Call before committing any owner-visible result/side effect that
  // did not already pass through cmd_exec's session fence.
  bool stillCurrent() const;
 private:
  // One composed scope. CommandIdentityScope's ctor reads from the temporary
  // AuthContext returned by g2HijackAuthContext() and copies what it needs
  // into the two TLS slots; the temporary is destroyed before this member
  // is fully constructed (standard temporary lifetime), which is safe.
  BlePeerOwnerSession owner_;
  CommandIdentityScope scope_;
};

// =============================================================================
// LensUiJob — unified message type for the lens applier queue.
// =============================================================================
// This wraps the existing PageSwapArgs path and the Redraw/Notify/Custom/
// NativeNotif variants consumed by the persistent `g2_page_swap_w` lens
// applier. The current worker stack is 8192 bytes; see G2_Glasses.cpp for the
// measured sizing evidence. Toast remains reserved but unimplemented.
//
// Producers stamp the cookie fields (submitMenuGen / targetPage /
// targetNetSub) at enqueue time. The applier checks submitMenuGen against
// the live gMenuGen. Redraw jobs hard-drop when stale; PageSwap jobs log and
// continue, while the other kinds use their own applicability rules.
// =============================================================================

// Forward decls — concrete payload structs live in G2_Glasses.cpp
// (PageSwapArgs). LensUiJob only stores pointers, so callers don't need full
// definitions to construct one. Toast remains reserved and undefined.
struct PageSwapArgs;
struct ToastSpec;

// Custom payload — runs an arbitrary function on the lens applier worker.
// Used for one-shot work that needs to happen off the
// G2 control/ACK owner but doesn't fit Redraw semantics (e.g. hijack bootstrap,
// which is the very first thing on a new hijack session — no view exists
// yet to "redraw"). Not gen-guarded by the applier; the run function is
// expected to do its own state checks if it cares.
struct CustomSpec {
  void (*run)();
};

// Native-notification payload — the fields for one G2 firmware-NATIVE
// notification card, pushed over the Even File Service (EFS). Unlike the
// full-screen g2notify placeholder, the firmware renders its own card,
// auto-wakes the display, and applies its own silent/DND. The lens applier
// runs the blocking 4-frame EFS send (g2SendNativeNotification) off its
// producer's task and deletes the spec afterward. Not gen-guarded — an OS-style
// notification is not tied to the interactive menu page. Fixed inline buffers
// (no heap-owned strings) so the queue message stays a single allocation.
// See docs/G2_NATIVE_NOTIFICATION_PLAN.md.
struct NativeNotifSpec {
  char app[64];          // app_identifier (package)
  char displayName[48];  // display_name
  char title[64];
  char subtitle[48];
  char body[192];        // message
};

// Notify payload — fires when a notification's auto-clear timer expires.
// Carries the gNotifyGen value captured when the notification was shown so
// the lens applier can drop the clear if a newer notification has replaced
// the one this timer was for. This retired notifyClearTaskBody, which spawned
// a fresh task per notification.
struct NotifySpec {
  uint32_t gen;   // gNotifyGen at submission; applier compares against live value
};

// Redraw payload — re-render a view from canonical state. Used by
// cmd-completion callbacks (e.g. the WiFi Auto-Start toggle re-renders
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
  PageSwap = 0,   // page-swap (CREATE/SHUTDOWN protocol)
  Redraw,         // re-render current view from canonical state
  Toast,          // reserved transient overlay; unimplemented
  Notify,         // notification + clear-timer (subsumes notifyClearTaskBody)
  Custom,         // typed one-off work on the lens-applier context
  NativeNotif,    // firmware-native EFS notification card (docs/G2_NATIVE_NOTIFICATION_PLAN.md)
  Barrier,        // lifecycle-only queue fence; carries no owned heap payload
};

struct LensUiJob {
  LensJobKind   kind;
  uint32_t      submitMenuGen;   // captured at enqueue time
  uint32_t      enqueuedAtMs;    // set by g2EnqueueLensJob/pageSwapEnqueue
  uint64_t      cmdSeq;          // 0 for navigation-origin (no command in flight)
  G2HijackPage  targetPage;      // viewId snapshot at submit time
  uint8_t       targetNetSub;    // page-specific sub-mode (0 if not captured)
  union Payload {
    PageSwapArgs* swap;
    RedrawSpec*   redraw;
    ToastSpec*    toast;
    NotifySpec*   notify;
    CustomSpec*   custom;
    NativeNotifSpec* nativeNotif;
    uint32_t      barrierGeneration;
  } payload;
};

// This pointer-queued envelope is intentionally compact; changing it affects
// every queued/in-flight lens job's ordinary-heap footprint.
static_assert(sizeof(LensUiJob) == 32,
              "LensUiJob layout changed; re-audit G2 internal-DRAM accounting");
static_assert(alignof(LensUiJob) == 8,
              "LensUiJob alignment changed; re-audit queued job allocation");

// A few producers run on shared infrastructure that must never spend 50 ms
// waiting for lens-queue space (notably ESP_TIMER_TASK, ESP-NOW RX, and the G2
// control/ACK owner). Other task contexts retain the short bounded wait.
enum class G2LensEnqueueWait : uint8_t {
  Bounded50Ms = 0,
  NoWait,
};

// Enqueue a fully-prepared LensUiJob (heap-owned, allocated with `new`).
// On success the lens applier takes ownership and deletes after dispatch.
// On failure (queue not initialised or full) ownership stays with the
// caller — the caller must free both the payload and the LensUiJob itself.
bool g2EnqueueLensJob(
    LensUiJob* job,
    G2LensEnqueueWait wait = G2LensEnqueueWait::Bounded50Ms);

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif // G2_HIJACKCMD_H
