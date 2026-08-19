// Per-task identity for guarded operations. Each FreeRTOS task gets its own
// copy of (AuthContext, user, isAdmin) stored in its TCB's thread-local
// storage slot. Default is ANON (zero-initialized); tasks that need SYSTEM
// access must install it explicitly via ExecIdentityGuard.
//
// Replaces the old globals gExecAuthContext, gExecUser, gExecIsAdmin. During
// migration the legacy globals still exist and ExecIdentityGuard keeps them
// in sync; readers can switch to the accessor functions below file-by-file.
//
// Thread safety: each task sees only its own slot. No locks needed.
// Concurrent tasks can have completely different identities simultaneously
// with zero interference.

#ifndef SYSTEM_AUTH_IDENTITY_H
#define SYSTEM_AUTH_IDENTITY_H

#include <Arduino.h>
#include <atomic>
#include "System_User.h"           // AuthContext, isAdminUser, SOURCE_INTERNAL
#include "System_Notifications.h"  // NotificationContextGuard — composed by CommandIdentityScope

// Read accessors — return the calling task's current identity.
const AuthContext& currentAuthContext();
const String&      currentExecUser();

// Map a command transport to its NotificationSource (WEB→web, SERIAL→cli, …).
// Exposed so the reboot path can stamp the actor onto the next-boot reboot event.
uint8_t transportToNotifSource(CommandSource t);
bool               currentExecIsAdmin();

// Build a SYSTEM identity AuthContext (transport=SOURCE_INTERNAL, user="system").
// Use for firmware-internal work that needs full FS access. Always pair with
// ExecIdentityGuard — never assign directly.
AuthContext systemIdentity(const char* purpose);

// Scoped identity install. Constructor saves the prior identity and installs
// the new one in the calling task's TLS slot. Destructor restores. Construct
// on the stack to bracket the region where you want a specific identity
// active.
class ExecIdentityGuard {
 public:
  explicit ExecIdentityGuard(const AuthContext& install);
  ~ExecIdentityGuard();
  ExecIdentityGuard(const ExecIdentityGuard&)            = delete;
  ExecIdentityGuard& operator=(const ExecIdentityGuard&) = delete;
  ExecIdentityGuard(ExecIdentityGuard&&)                 = delete;
  ExecIdentityGuard& operator=(ExecIdentityGuard&&)      = delete;

 private:
  AuthContext savedCtx_;
  String      savedUser_;
  bool        savedIsAdmin_;
};

// Convenience: install SYSTEM for the rest of the current scope.
// Equivalent to `ExecIdentityGuard guard(systemIdentity(purpose));`.
#define SYSTEM_IDENTITY_SCOPE_CONCAT_INNER(a, b) a##b
#define SYSTEM_IDENTITY_SCOPE_CONCAT(a, b) SYSTEM_IDENTITY_SCOPE_CONCAT_INNER(a, b)
#define SYSTEM_IDENTITY_SCOPE(purpose)                                        \
  ExecIdentityGuard SYSTEM_IDENTITY_SCOPE_CONCAT(_sysIdentityGuard_, __LINE__)( \
      systemIdentity(purpose))

// ============================================================================
// CommandIdentityScope — composed identity + notification install
// ============================================================================
//
// The two TLS slots (auth identity + notification source) almost always want
// to move together: a command running on behalf of <user> via <transport>
// should also have its notifications attributed to <user> via the matching
// NotificationSource. Forgetting one half of the pair is a class of bug
// (sync direct-FS hijack work fired notifications with whatever stale
// NOTIF_SOURCE_* the task last carried — typically UNKNOWN).
//
// CommandIdentityScope composes both guards in one stack object and derives
// the NotificationSource from the AuthContext's transport via a private
// 1:1 mapping (SOURCE_WEB → NOTIF_SOURCE_WEB, SOURCE_G2_GLASSES →
// NOTIF_SOURCE_G2, etc. — see System_AuthIdentity.cpp). Callers don't pass
// a notification source separately, which removes the "wrong source for
// transport" footgun by construction.
//
// LAYER POSITION
// --------------
//   ExecIdentityGuard        (low-level: TLS auth slot)
//   NotificationContextGuard (low-level: TLS notification slot)
//        ↓ composed by ↓
//   CommandIdentityScope     (the unified per-command scope primitive)
//        ↓ named by ↓
//   G2HijackCtxGuard         (sugar for G2 direct-FS scopes)
//   OLEDFileBrowserCtxGuard  (sugar for OLED file browser scope)
//   executeCommand           (uses CommandIdentityScope directly)
//
// USAGE
// -----
//   CommandIdentityScope scope(g2HijackAuthContext());
//   CommandIdentityScope scope(gLiveTextOwnerCtx);  // works with any AuthContext
//   CommandIdentityScope scope(ctx);                // from executeCommand
//
// DESTRUCTION ORDERING
// --------------------
// notifGuard_ destroys first (declared last), then identityGuard_. So when
// the scope ends, the notification context is restored BEFORE the identity
// — meaning any notification fired during identity-restoration unwinds with
// the prior notification source (correct: the scope is gone).
class CommandIdentityScope {
 public:
  explicit CommandIdentityScope(const AuthContext& ctx);
  ~CommandIdentityScope() = default;
  CommandIdentityScope(const CommandIdentityScope&)            = delete;
  CommandIdentityScope& operator=(const CommandIdentityScope&) = delete;
  CommandIdentityScope(CommandIdentityScope&&)                 = delete;
  CommandIdentityScope& operator=(CommandIdentityScope&&)      = delete;

 private:
  // Declaration order = construction order. Both guards copy what they need
  // from `ctx` at construction time; neither retains a reference, so `ctx`
  // can be a temporary (e.g. `CommandIdentityScope(g2HijackAuthContext())`).
  ExecIdentityGuard        identityGuard_;
  NotificationContextGuard notifGuard_;
};

// Initialize the TLS slot for the calling task. Idempotent. Worker tasks
// don't need to call this — their first ExecIdentityGuard construction
// allocates the slot lazily. Calling explicitly from app_main is good
// documentation: it makes the main task's slot allocation explicit.
void initAuthIdentityForCurrentTask();

// ============================================================================
// Identity generation — cache-invalidation protocol for auth-dependent state
// ============================================================================
//
// THE PROBLEM
// -----------
// Subsystems sometimes cache state that depends on the current auth identity
// (e.g. a directory listing where each entry was filtered by VFS permission
// check). If that cache is filled while the calling task has the *wrong*
// identity — most commonly because pairing hasn't happened yet and the TLS
// slot still holds ANON — the cache is permanently stale once pairing fixes
// the identity. Subsequent reads return the old wrong picture forever (until
// reboot), because there's no event channel saying "permissions just changed,
// invalidate yourself."
//
// THE CASE THIS WAS BUILT FOR
// ---------------------------
// FileManager (G2 hijack file browser) is a per-boot singleton. Its very
// first navigate("/") happens on the first Files-menu tap. If that tap ran
// before the user paired the glasses, the calling task's TLS slot had
// pairedByUser="", VFS::openGuarded returned PERM DENY, and totalItems
// landed at 0. After pairing, every subsequent Files-menu open re-rendered
// the same stale 0-item cache, even though pairedByUser was now correct
// and a fresh scan would succeed.
//
// THE PROTOCOL
// ------------
// `gIdentityGeneration` is a monotonic atomic counter, bumped exactly once
// every time the "who can read what" topology changes. Consumers that cache
// auth-dependent state tag the cache with the generation it was filled
// under and re-check on use. Stale generation → invalidate and re-fill.
//
// CONSUMER USAGE (the only thing most callers need to know)
// ---------------------------------------------------------
//   class MyCache {
//     uint32_t loadedAtGen_ = 0;
//     ...
//     void useCacheOrRefill() {
//       const uint32_t cur = gIdentityGeneration.load(std::memory_order_acquire);
//       if (cur != loadedAtGen_) {
//         refill();                         // re-scan under current identity
//         loadedAtGen_ = cur;
//       }
//       // ... use cache normally ...
//     }
//   };
//
// PRODUCER USAGE (call this when permissions change)
// --------------------------------------------------
// Call `bumpIdentityGeneration(reason)` immediately AFTER successfully
// applying a change that affects what some user can or cannot access.
//
// The current bump sites (one line each, search for `bumpIdentityGeneration`
// across the tree to audit):
//
//   * bleStampPairedByIfBlank — first time a BLE peer gets owned by a user
//     (was unowned, now owned by "asd").
//   * cmd_user_add / cmd_user_approve — a new user account becomes real
//     (was nonexistent or pending, now real with permissions).
//   * user.sync / user.add / user.approve — a new usable account appears.
//   * user.delete — a user account goes away (their permissions
//     evaporate; any cache that filtered "what user X can see" must rerun).
//   * user.setrole — an account gains or loses permissions.
//   * banuser / unbanuser — an account becomes unusable or usable again.
//
// EVENTS THAT DO **NOT** BUMP
// ---------------------------
//   * Login / logout / session expiry — session lifecycle, not a permission
//     topology change. The user's permission set was the same before and
//     after; only whether they're presenting a valid session changed.
//   * Password change / reset — credentials rotate but the user's
//     permission set is unchanged. (These DO trigger session revocation
//     via revokeUserSessions — see the SISTER PROTOCOL note below — but
//     no auth-derived cache cares about the credential, only the user
//     identity that produced it, which hasn't changed.)
//   * `userrequest` / `userdeny` — a pending request can't authenticate, so
//     it has no permissions. Approving (cmd_user_approve) does bump,
//     because that's when the account starts being usable.
//   * Settings changes that don't touch auth (wifi SSID, brightness, etc.).
//   * BLE connect / disconnect to an already-paired peer.
//   * `setSetting(d.pairedByUser, ...)` direct assignment — should always
//     go through `bleStampPairedByIfBlank` which bumps for you.
//
// SISTER PROTOCOL: SESSION REVOCATION
// -----------------------------------
// The clock invalidates auth-derived CACHES. A separate, independent
// mechanism — `revokeUserSessions(user, reason, [exceptSid, exceptTransport])`
// declared in System_User.h — kicks a user out of all active SESSIONS
// across every transport (web/serial/oled/bluetooth).
//
// These are different concerns with different trigger points:
//
//   Event                          | Clock bump? | Session revoke?
//   -------------------------------|-------------|------------------
//   bleStampPairedByIfBlank        | yes         | no  (no sessions for the peer)
//   user.add / user.approve        | yes         | no  (no prior sessions)
//   user.promote                   | yes         | no  (gained access)
//   user.demote                    | yes         | yes (drop admin session)
//   user.delete                    | yes         | yes (account gone)
//   user.ban                       | yes         | yes (account suspended)
//   user.unban                     | yes         | no  (usable on next login)
//   user.changepassword (self)     | no          | yes (except calling session)
//   user.resetpassword (by admin)  | no          | yes (kick target everywhere)
//   login / logout                 | no          | n/a (session itself is the change)
//
// If you add a new user-mutation command, decide each axis independently:
//   * "Does this change WHAT users can do?" → bump (or don't).
//   * "Should affected users be kicked out?" → revoke (or don't).
//
// THE RULE OF THUMB
// -----------------
// Ask: "would a permission check that returned ALLOWED before this event
// possibly return DENIED after it (or vice versa)?" If yes, bump. If the
// answer is the same before and after, don't bump.
//
// COST
// ----
// One std::atomic<uint32_t> load on each cache read (free on ESP32 — it's
// a relaxed-ordered 32-bit load). Bumps happen handfuls of times per
// device LIFETIME, so the write side is effectively free too. The only
// real cost is the discipline of remembering to bump at producer sites
// and version-check at consumer sites — both single-line changes.
//
// ANTI-PATTERN TO AVOID
// ---------------------
// Don't bump "to be safe" on events that don't actually change the
// permission topology. Each spurious bump invalidates every cache in
// the firmware and forces a re-scan; do that on every login and you've
// rebuilt the original "no cache at all" performance problem with extra
// steps. Be conservative.

extern std::atomic<uint32_t> gIdentityGeneration;

// Bump the generation counter and log the reason for the audit trail.
// `reason` should be a short literal like "useradd" or "pair g2-glasses"
// so log lines are greppable. Pass nullptr only in tests.
void bumpIdentityGeneration(const char* reason);

// ============================================================================
// Per-task command-execution context (Stage 3)
// ============================================================================
//
// These were globals (gCurrentCommandContext, gCmdCaptureBuf/Len/Cap) read
// by broadcastOutput on any task. Cross-task reads routed output and
// captured bytes to whichever task happened to have set the globals last
// — i.e. cmd_exec_task's current command. A non-cmd_exec broadcast (e.g.
// from a sensor task) used to be silently routed to the WebSocket session
// running the current command, and was captured into its buffer.
//
// Now they live in the calling task's TLS slot. Tasks that never set them
// see nullptr / inactive defaults; broadcastOutput skips capture and
// falls back to MSG_ROUTE_ALL.

// Output capture state. Lives in the TLS slot owned by cmd_exec_task while
// a capture is active. broadcastOutput reads via currentCaptureState() and
// appends in-place (modifying buf/len via the returned pointer).
struct CaptureBufState {
  char*  buf;
  size_t len;
  size_t cap;
};

// Read accessors — return the calling task's context (or nullptr if no
// slot has been allocated). Safe to call from any task at any time.
void* currentCommandContext();
CaptureBufState* currentCaptureState();

// cmd_exec_task lifecycle helpers. These allocate the calling task's
// slot on first use (same lazy-init pattern as ExecIdentityGuard).
void setCurrentCommandContext(void* ctx);
void clearCurrentCommandContext();
void setCaptureBuffer(char* buf, size_t cap);
void clearCaptureBuffer();

#endif  // SYSTEM_AUTH_IDENTITY_H
