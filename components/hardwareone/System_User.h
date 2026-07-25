#ifndef SYSTEM_USER_H
#define SYSTEM_USER_H

#include <Arduino.h>

// ============================================================================
// User System - User and session management command handlers
// ============================================================================

// CommandEntry is defined in system_utils.h (included by files that need it)
// Forward declare here for header-only usage
struct CommandEntry;

// ============================================================================
// Authentication Context (used by command execution and HTTP handlers)
// ============================================================================

// Command source identifier for audit logging
//
// SOURCE_LOCAL_DISPLAY is the OLED specifically — physical buttons + screen,
// per-session login via gLocalDisplayUser/gLocalDisplayAuthed.
//
// SOURCE_G2_GLASSES is the BLE-attached lens — historically piggybacked on
// SOURCE_LOCAL_DISPLAY but has a completely different identity model:
// "pair-time" trust, no per-session credential check. The pairedByUser field
// on gBlePeerData[BLE_PEER_G2_GLASSES] is stamped when an authenticated CLI
// session runs `bleautoconnect g2-glasses on`; commands from the lens then
// run as that user until re-pair. See G2_HijackCmd.cpp for the auth flow.
enum CommandSource {
  SOURCE_WEB = 0,
  SOURCE_SERIAL = 1,
  SOURCE_INTERNAL = 2,
  SOURCE_ESPNOW = 3,
  SOURCE_LOCAL_DISPLAY = 4,
  SOURCE_BLUETOOTH = 5,
  SOURCE_MQTT = 6,
  SOURCE_VOICE = 7,
  SOURCE_G2_GLASSES = 8,
};

struct AuthContext {
  CommandSource transport;
  String path;   // URI for HTTP, command for CLI, view for TFT
  String ip;     // remote IP for HTTP, "local" for serial/TFT
  String user;   // resolved username when authenticated
  String sid;    // HTTP session id (empty for serial/TFT)
  void* opaque;  // httpd_req_t* when HTTP; nullptr otherwise
  String scope;  // optional path-prefix confinement (empty = unconfined); enforced in checkPerm. Set via VFS::systemAuth(scope, reason).
};

// User command registry - using userSystemCommands only

// ============================================================================
// Authentication State Globals (defined in HardwareOne.cpp)
// ============================================================================

extern bool gLocalDisplayAuthed;
extern String gLocalDisplayUser;
extern unsigned long gLocalDisplayLastInteractionMs;  // OLED session idle clock; see HardwareOne.cpp

// ============================================================================
// User Management Helper Functions (implemented in user_system.cpp)
// ============================================================================

// Password hashing function (used by .ino for first-time setup and password verification)
String hashUserPassword(const String& password);

// Reserved synthetic identity for an authenticated bonded master. A device that
// presents a valid bond session token — validated against the live, AEAD-encrypted
// session (see v4_handle_cmd) — runs its remote commands under this username.
// isAdminUser() grants it admin ONLY while a live bond session exists, so the
// elevation tracks the bond's lifetime exactly: no persisted account, no
// stale-admin window. It is in the reserved-name table (cannot be created) and
// has no users.json entry (cannot be logged into with a password). The bond
// session/token is X25519-ephemeral and re-derived every boot, which is why a
// persisted credential would be pointless — there'd be nothing to authenticate
// against it after a reboot.
inline constexpr const char* kBondAdminUser = "bond-admin";

// Transport-generic authentication functions
bool tgRequireAuth(AuthContext& ctx);
bool isAdminUser(const String& who);

// Founder / first users.json username (FTS creates id=1 as the first entry).
// Empty if users.json is missing or unreadable. Used as a recovery identity
// when G2 pairedByUser was never stamped or was cleared (ban/logout).
String getDeviceOwnerUsername();
// Top tier — role=="superadmin" (or a live bonded session). Gates the
// identity/crypto/destructive/auth-posture command set; see commandRequiresSuperAdmin.
bool isSuperAdminUser(const String& who);
// Bottom tier — role=="guest". Authenticated view-only: login/logout only
// for commands; filesystem reads use the user column masked to PERM_READ.
bool isGuestUser(const String& who);

// Privilege ranks — single source of truth for C++ comparisons. The settings
// page mirrors these as window.__hwRoleRank (injected from the same values).
// Accounts store role *names* in users.json, never these integers.
constexpr int kRoleRankGuest      = 0;
constexpr int kRoleRankUser       = 1;
constexpr int kRoleRankAdmin      = 2;
constexpr int kRoleRankSuperAdmin = 3;
// Sentinel for userMutationAllowed when the mutation grants no role (delete/ban).
constexpr int kRoleRankNoGrant    = -1;

// Map a role name ("guest"|"user"|"admin"|"superadmin") to a rank.
// Unrecognised names collapse to kRoleRankUser.
int userRoleRank(const String& role);
// Effective rank for a live account — applies bond/owner fallbacks via
// isSuperAdminUser/isAdminUser, then guest detection. Prefer this over
// hand-decoding admin/super booleans when comparing privileges.
int userAccountRank(const String& username);
// True for the four grantable role names (caller should lowercase first).
bool isKnownUserRole(const String& role);

// Centralized transport authentication management
bool loginTransport(CommandSource transport, const String& username, const String& password);
void logoutTransport(CommandSource transport);
bool isTransportAuthenticated(CommandSource transport);
String getTransportUser(CommandSource transport);
bool isTransportAdmin(CommandSource transport);

// Force-logout every session belonging to `username` across all transports.
// Returns count of sessions revoked. Called by user-mutation paths (delete,
// demote, password-change/reset) — see definition for the full rationale
// and the relationship to gIdentityGeneration (they are separate mechanisms;
// this one is for kicking sessions, the clock is for invalidating caches).
//
// `exceptSid` (web only): keep this specific web SID alive even if it
//   matches `username`. Used by self-password-change to keep the calling
//   session active.
// `exceptTransport`: keep this whole transport's session alive (the
//   non-web transports use one-session-per-transport, so SID isn't enough
//   to distinguish). Pass the calling task's transport when the caller is
//   themselves the user being modified.
int revokeUserSessions(const String& username,
                       const String& reason,
                       const String& exceptSid       = String(),
                       CommandSource exceptTransport = SOURCE_INTERNAL);

// Credentials validation helpers (moved from main .ino)
bool isValidUser(const String& username, const String& password);
bool verifyUserPassword(const String& inputPassword, const String& storedHash);

// User account ban (persisted in users.json "banned" field)
bool isUserBanned(const String& username);

// Update the "lastSeen" ISO-8601 timestamp in users.json for the given username.
// Only writes if the system clock is valid. Call after any successful login.
void updateUserLastSeen(const String& username);

// Update a user's password (stores hashed). If requireChangeOnNextLogin is true, sets mustChangePassword until they change it.
bool setUserPassword(const String& username, const String& newPasswordRaw, bool requireChangeOnNextLogin = false);

// True when per-user settings has mustChangePassword (user must set a new password after login).
bool userMustChangePassword(const String& username);

// Public username/password intake rules (register + admin create). Username:
// 1..64 chars of [A-Za-z0-9._-], not a reserved sentinel. Password: 6..64 chars.
// errorOut (optional) gets a short fixed reason string — safe to show on public pages.
bool isValidPublicUsername(const String& username, String* errorOut = nullptr);
bool isValidPublicPassword(const String& password, String* errorOut = nullptr);
static constexpr size_t kPublicUsernameMaxLen = 64;
static constexpr size_t kPublicPasswordMaxLen = 64;
static constexpr size_t kPublicPasswordMinLen = 6;

// Admin: create a new account immediately (not pending). Password is hashed into per-user settings.
// If mustChangeOnLogin is true, userMustChangePassword stays set until they change password via setUserPassword.
// `role` is "user" (default), "admin", or "superadmin". CALLERS MUST RANK-CHECK
// FIRST — this is a data mutator and does not enforce "cannot grant a role above
// your own"; cmd_user_add does that via userMutationAllowed().
bool adminCreateUser(const String& username, const String& plainPassword, bool mustChangeOnLogin,
                     const String& createdBy, String& errorOut, const String& role = "user");

// Update a user's gamepad pattern password (stores hashed, separate from text password)
bool setUserGamepadPassword(const String& username, const String& newPatternRaw);

// Check if a user has a gamepad password set
bool hasUserGamepadPassword(const String& username);

// Lookup user ID (primary key) by username
bool getUserIdByUsername(const String& username, uint32_t& outUserId);

// User sync helpers (for ESP-NOW credential propagation)
bool getUserRole(const String& username, String& outRole);

// ============================================================================
// User Filesystem Operations (migrated from main .ino)
// ============================================================================

// File paths
extern const char* USERS_JSON_FILE;  // Now points to "/system/users/users.json"

// Boot sequence and timestamp resolution
bool usernameExistsInUsersJson(const String& json, const String& username);
void resolvePendingUserCreationTimes();
void writeBootAnchor();
void cleanupOldBootAnchors(void* doc = nullptr);  // doc is StaticJsonDocument<8192>*

// ============================================================================
// User Command Handlers (implemented in user_system.cpp)
// ============================================================================
//
// TWO PROTOCOLS LIVE HERE: CLOCK BUMPS + SESSION REVOCATION
// ---------------------------------------------------------
// These are SEPARATE mechanisms. A command may use both, one, or neither:
//
//   * bumpIdentityGeneration(reason) — "permission topology changed,
//     auth-derived caches need to invalidate." See System_AuthIdentity.h
//     for the full protocol. Bumped when: a user is created/deleted, a
//     role changes (promote/demote), or peer ownership stamps (BLE pair).
//     NOT bumped on login/logout (sessions), password change/reset
//     (creds rotated but perms unchanged), or userrequest/userdeny
//     (pending acct has no perms).
//
//   * revokeUserSessions(user, reason [, except…]) — "kick this user
//     out of every active session." See System_User.cpp::revokeUserSessions
//     definition for the design. Called when: a user is deleted, demoted
//     from admin, or has their password changed/reset. NOT called on
//     promote (gained access, no kick needed) or add/approve (no sessions
//     existed before the account did).
//
// Annotations on each cmd_user_* below show which protocols fire. When
// adding a new mutator here, decide both questions independently:
//   - "Does the permission topology change?" → bump or don't.
//   - "Should affected users be kicked out?" → revoke or don't.

const char* cmd_login(const String& argsInput);
const char* cmd_logout(const String& argsInput);

// Export command registry for system_utils.cpp
extern const CommandEntry userSystemCommands[];
extern const size_t userSystemCommandsCount;
const char* cmd_user_approve(const String& argsInput);   // bump        — approval makes account real
const char* cmd_user_deny(const String& argsInput);      // —           — pending acct had no perms or sessions
const char* cmd_user_promote(const String& argsInput);   // bump        — gains admin perms (no kick: gained access)
const char* cmd_user_demote(const String& argsInput);    // bump+revoke — admin session must restart with lower perms
const char* cmd_user_delete(const String& argsInput);    // bump+revoke — account is gone, kick everywhere
const char* cmd_user_changepassword(const String& argsInput);  // revoke other — keep calling session, kick the rest

// Internal implementation shared between cmd_user_changepassword (CLI / OLED
// / serial / BLE — invoked via executeCommand which installs identity) and
// the web POST handler (which must install identity manually).
//
// PRECONDITION: caller MUST have an ExecIdentityGuard active for the
// requesting user's AuthContext. The function resolves the user via
// currentExecUser() and reads the calling session's sid/transport via
// currentAuthContext() to skip-revoke the caller's own session. Without an
// installed identity it returns "Error: Not authenticated".
//
// Returns a result string compatible with the CLI command convention:
//   "Password changed successfully for user '<name>'" on success,
//   "Error: <reason>" on validation/auth/storage failure.
// Pointer lifetime: backed by the shared debug buffer; valid until the next
// debug-buffer use. Caller should copy or consume immediately.
const char* userChangePasswordCore(const String& currentPassword,
                                   const String& newPassword,
                                   const String& confirmPassword);
const char* cmd_user_resetpassword(const String& argsInput);   // revoke       — admin reset target's creds
const char* cmd_user_add(const String& argsInput);       // bump        — new usable account exists (no sessions yet)
const char* cmd_user_list(const String& argsInput);
const char* cmd_user_request(const String& argsInput);   // —           — pending, can't auth yet
const char* cmd_user_sync(const String& argsInput);
const char* cmd_pending_list(const String& argsInput);
const char* cmd_session_list(const String& argsInput);
const char* cmd_session_revoke(const String& argsInput); // manual revoke — uses revokeUserSessions internally

// ----------------------------------------------------------------------------
// Shared session idle-timeout policy (Step 0)
//
// One owner of the "log out after N minutes of no REAL interaction" rule, used
// by every authenticated channel (web, serial, BLE, ESP-NOW bond). A channel
// stamps its own "last interaction" field with sessionStampNow() ONLY on real
// user/peer actions — automatic/keepalive traffic (SSE, status polls, BLE
// notifies, ESP-NOW heartbeats) must NOT stamp; that classification stays
// per-channel — then calls sessionIdleExpired() to decide logout.
//
// Centralizing this keeps the window, the 0=disabled rule, the millis() wrap-
// safety, and the 0-sentinel identical everywhere.

// millis() timestamp for a "last interaction" field, never 0 (0 = "never
// stamped"). Use this when recording activity so the sentinel stays reserved.
unsigned long sessionStampNow();

// Per-transport idle window in ms. Reads that transport's own gSettings knob
// (sessionIdleWeb / sessionIdleSerial / sessionIdleBle / …); 0 = disabled.
// One shared policy, independent values. Transports not yet wired return 0.
unsigned long sessionIdleWindowMs(CommandSource transport);

// Core wrap-safe expiry against an EXPLICIT window. True if a session whose
// last real interaction was `lastInteractionMs` (millis timebase) has exceeded
// `windowMs` as of `nowMs`. Always false when the window is disabled (0) or the
// field was never stamped (0).
bool sessionIdleExpired(unsigned long lastInteractionMs, unsigned long nowMs, unsigned long windowMs);

// Convenience: look up the transport's window then test expiry. This is what
// channels normally call — they name their transport, the policy does the rest.
bool sessionIdleExpired(CommandSource transport, unsigned long lastInteractionMs, unsigned long nowMs);
inline bool sessionIdleExpired(CommandSource transport, unsigned long lastInteractionMs) {
  return sessionIdleExpired(transport, lastInteractionMs, millis());
}

// ============================================================================
// Boot Sequence Management
// ============================================================================

// Increment boot sequence counter (memory-only, resets on power cycle)
void loadAndIncrementBootSeq();

// User system command registration
// User system command module is automatically registered

#endif // SYSTEM_USER_H
