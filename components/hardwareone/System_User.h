#ifndef SYSTEM_USER_H
#define SYSTEM_USER_H

#include <Arduino.h>
#include <atomic>

// ============================================================================
// User System - User and session management command handlers
// ============================================================================

// CommandEntry is defined in System_CommandRegistry.h (included by files that need it)
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
// session runs `bleautoreconnect g2-glasses on`; commands from the lens then
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
  SOURCE_UART = 9,   // UART host link (System_UartLink.cpp) — a Linux host
                     // (CM5 carrier) driving the firmware over a board-to-board
                     // UART. Own synchronized session state, own
                     // brute-force key ("uart"), own idle window
                     // (sessionIdleUart). Deliberately NOT counted as power-save
                     // activity and NEVER treated as physical presence.
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

// A transport-session epoch is a boot-local, nonzero generation token. It is
// deliberately unrelated to wall-clock time: NTP may be absent at boot or may
// synchronize at any later point without changing command ownership.
using TransportSessionEpoch = uint32_t;
inline constexpr TransportSessionEpoch kNoTransportSessionEpoch = 0;

// Fixed-capacity registry used by transports with one or more live sessions
// (web, BLE, serial and the local display). Open only after the session's
// identity is fully initialized; close before clearing or replacing it.
TransportSessionEpoch transportSessionOpen(CommandSource source);
void transportSessionClose(CommandSource source, TransportSessionEpoch epoch);
bool transportSessionEpochIsLive(CommandSource source,
                                 TransportSessionEpoch epoch);

// Resolve the live epoch represented by an AuthContext. Stateless identities
// (system/internal work and HTTP Basic Auth) intentionally return zero.
TransportSessionEpoch captureTransportSessionEpoch(const AuthContext& ctx);

// Serial and OLED authentication globals predate transport-session fencing.
// These helpers are now the only mutation/snapshot front doors for queued
// command ownership; they invalidate the old generation before identity
// changes and publish the new one afterwards.
TransportSessionEpoch serialTransportSessionAuthenticated(const String& user);
// Local Serial intrinsics use the epoch that owned the completed input line.
// Login publishes only if that exact incarnation is still current; logout
// clears it only if it is. Both return with the writer released; the caller
// prints its console acknowledgement afterwards.
TransportSessionEpoch serialTransportSessionAuthenticatedIfEpoch(
    TransportSessionEpoch expectedEpoch, const String& user);
bool serialTransportSessionClearIfEpoch(TransportSessionEpoch expectedEpoch);
// Physical input is generation-bound even before authentication. These two
// helpers create/read the private pre-auth incarnation used only to ensure a
// partially typed line cannot cross a login/logout/replacement boundary.
TransportSessionEpoch serialTransportInputEpoch();
bool serialTransportInputEpochIsCurrent(TransportSessionEpoch epoch);
void serialTransportSessionCleared();
// Invalidate only an unauthenticated/AuthBypass incarnation when the live
// require-auth policy changes. A named authenticated session remains valid;
// any bypass command admitted under the previous policy generation does not.
void serialTransportAuthPolicyChanged();
// Owner-side check that the exact epoch is still live, linearized with
// login/logout/revocation and auth-policy rotation. Begin returns with the
// lifecycle writer (a FreeRTOS mutex) held only when it is; callers must pair
// a successful begin with end on the same task, and must do no console,
// display, log or users.json I/O in between: the writer is for the check or
// publication itself, and a slow holder blocks every other session writer.
bool serialTransportSessionBeginDelivery(TransportSessionEpoch epoch);
void serialTransportSessionEndDelivery();
bool localDisplayTransportSessionBeginDelivery(TransportSessionEpoch epoch);
void localDisplayTransportSessionEndDelivery();
// OLED-owner-only synchronization point. Begin always returns with the
// display lifecycle writer held, including when the authoritative state is
// logged out (epoch zero), and snapshots that state into the outputs. This is
// what lets the main loop update its legacy String/UI mirror without any
// foreign task touching OLED-owned state. Every Begin must be paired with End,
// and the display push itself belongs outside the window.
TransportSessionEpoch localDisplayTransportSessionBeginUiSync(
    String& userOut, bool& authedOut);
void localDisplayTransportSessionEndUiSync();
TransportSessionEpoch serialTransportSessionSnapshot(String& userOut,
                                                      bool& authedOut);
void localDisplayTransportSessionAuthenticated(const String& user);
void localDisplayTransportSessionCleared();
void localDisplayTransportAuthPolicyChanged();
TransportSessionEpoch localDisplayTransportSessionSnapshot(String& userOut,
                                                            bool& authedOut);
// Cross-task writers use this while holding the display lifecycle writer so
// the authority cutover and pending UI-boundary generation are one publication.
// Implemented by OLED_Utils as an atomic generation/dirty bump only.
void oledNotifyLocalDisplayAuthChanged();

// User command registry - using userSystemCommands only

// ============================================================================
// Authentication State Globals (defined in HardwareOne.cpp)
// ============================================================================

// Main/OLED-loop mirror only. Cross-task authentication and authorization
// must use localDisplayTransportSessionSnapshot(), never these legacy fields.
extern bool gLocalDisplayAuthed;
extern String gLocalDisplayUser;
extern std::atomic<unsigned long> gLocalDisplayLastInteractionMs;  // OLED session idle clock; see HardwareOne.cpp

// UART host-link session (defined in System_UartLink.cpp). One session per
// port, deliberately separate so the CM5 link and USB console can never fuse
// into one login. The username is private to System_UartLink; callers use its
// synchronized snapshot API rather than sharing an Arduino String cross-task.

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

// The live bonded-master identity: the ESP-NOW transport, the reserved
// kBondAdminUser name and a live bond session, all three. An account or a
// session on another transport that merely carries the name never qualifies.
// Always false when bonded mode is compiled out. isAdminUser/isSuperAdminUser
// answer the bare name with this test against the calling task's identity.
bool bondIdentityLive(const AuthContext& ctx);

// Reserved audit/dispatch names ("system", "AuthBypass", kBondAdminUser),
// compared case-insensitively. They are never creatable, never resolve to an
// account id and never pass a credential check.
bool isReservedUsername(const String& username);

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
// Bottom tier — role=="guest". Authenticated view-only: caller-local
// login/logout and whoami only; filesystem reads use the user column masked
// to PERM_READ.
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
// Fail-closed eligibility for controlling another transport's session.
// Returns true only when users.json is readable, the exact account exists,
// is not banned, and has a known non-Guest role. This is intentionally
// stronger than !isGuestUser(), whose legacy fallback treats lookup failures
// as ordinary User for non-security UI ranking.
bool userMayControlOtherSessions(const String& username);
bool getUserAuthorizationRole(const String& username, String& roleOut);

// Both answers from ONE read of users.json, for resolveRole()'s hot path.
// Equivalent to getUserAuthorizationRole() followed by isSuperAdminUser() —
// same two parsers over the same bytes, one file read instead of two.
// Does NOT handle the bond-admin identity; see the definition.
bool getUserRoleAndSuper(const String& who, String& roleOut, bool& isSuperOut);

// Centralized transport authentication management
bool loginTransport(CommandSource transport, const String& username, const String& password);
// BLE validates under the users database lock, then binds an exact GATT
// connection epoch before recording success. This avoids auditing a login as
// successful when the connection disappeared between validation and bind.
bool validateTransportCredentials(CommandSource transport,
                                  const String& username,
                                  const String& password);
void recordTransportLoginResult(CommandSource transport,
                                const String& username,
                                bool success,
                                const char* reason);
bool loginTransportFromNamedSession(CommandSource target,
                                    const String& username,
                                    const String& password,
                                    CommandSource source,
                                    const String& sourceUser,
                                    TransportSessionEpoch sourceEpoch);
// Publish an already credential-verified UART account as a named session.
// CM5 presence eligibility is resolved fail-closed from users.json and fenced
// against concurrent role/delete/ban mutations.
uint32_t publishUartAccountSession(const String& username,
                                   uint32_t* transportEpochOut = nullptr);
void logoutTransport(CommandSource transport);
// Refuses (with *denialOut set when given) to sign out a target session whose
// account outranks the source account, the rank rule the user mutations use.
bool logoutTransportFromNamedSession(CommandSource target,
                                     CommandSource source,
                                     const String& sourceUser,
                                     TransportSessionEpoch sourceEpoch,
                                     const char** denialOut = nullptr);
bool isTransportAuthenticated(CommandSource transport);
String getTransportUser(CommandSource transport);
bool isTransportAdmin(CommandSource transport);

// Security audit trail for credential logins. Defined in System_Debug.cpp
// (beside the log-file constants) rather than WebServer_Server.cpp, so the
// audit survives a headless build — that file compiles away entirely with
// ENABLE_HTTP_SERVER=0 and used to take login auditing with it on every
// transport. Call recordLoginAttempt(); it maps the transport to the
// canonical "<x>/login" path and delegates here.
void logAuthAttempt(bool success, const char* path, const String& userTried,
                    const String& ip, const String& reason);
void recordLoginAttempt(CommandSource transport, const String& user,
                        const String& ip, bool success, const char* reason);

// ============================================================================
// Credential attempt table: the brute-force lockout for credential logins
// ============================================================================
// One owner for login throttling, compiled on every build (with or without
// the web server) and guarded by its own lock, because httpd, the main loop,
// the MQTT task and cmd_exec all record attempts. Entries are keyed by
// transport plus peer: the web uses the client IP, serial "local", the UART
// link "uart", MQTT "mqtt:<broker>", and a targeted login the caller's own
// console peer. Check credentialAttemptAllowed() before validating a
// password and report the outcome with credentialAttemptRecord(); each
// transport keeps its own reply text and audit line.
//
// Failures inside LOGIN_ATTEMPT_WINDOW_MS count toward the tiers below. An
// entry never expires while its lock runs; after a lock it is kept for one
// more window, so the next failure moves on toward the next tier. A success
// clears the entry only when every failure in it named the account that
// succeeded. Serial, UART, the local display and MQTT (one broker at a time)
// have reserved slots, so network peers cannot crowd them out; network
// peers (the web today) share a pool of CREDENTIAL_ATTEMPT_POOL_SLOTS where a
// locked entry is never evicted: when every pool entry is locked, a new peer
// is refused until one of the locks ends. Times use the 64-bit esp_timer
// clock, so an ended lock cannot come back when millis() wraps.
#define CREDENTIAL_ATTEMPT_POOL_SLOTS 8
#define LOGIN_ATTEMPT_WINDOW_MS    (10UL * 60UL * 1000UL)  // 10 minutes
#define LOGIN_LOCKOUT_TIER1_COUNT  5   // 5  failures -> 30-second lockout
#define LOGIN_LOCKOUT_TIER1_MS     (30UL * 1000UL)
#define LOGIN_LOCKOUT_TIER2_COUNT  10  // 10 failures -> 5-minute lockout
#define LOGIN_LOCKOUT_TIER2_MS     (5UL * 60UL * 1000UL)
#define LOGIN_LOCKOUT_TIER3_COUNT  20  // 20 failures -> 30-minute lockout
#define LOGIN_LOCKOUT_TIER3_MS     (30UL * 60UL * 1000UL)

// True when this peer may try a credential now. retryMsOut (optional)
// receives the time left on the lock that refuses it.
bool credentialAttemptAllowed(CommandSource transport, const char* peer,
                              unsigned long* retryMsOut = nullptr);
// Record one checked credential attempt for `username`. A failure counts
// toward the peer's lock and posts SYSEVT_LOGIN_LOCKED when it sets one.
void credentialAttemptRecord(CommandSource transport, const char* peer,
                             const char* username, bool success);
// Failures currently counted for this peer (0 when it has no live entry).
uint8_t credentialAttemptFailures(CommandSource transport, const char* peer);
// Drop the peer's entry (for example when an admin bans the address).
void credentialAttemptForget(CommandSource transport, const char* peer);
// The fixed peer name of a console transport: "local" (serial), "uart",
// "display"; nullptr for every other transport.
const char* credentialAttemptConsolePeer(CommandSource transport);
// Failed credential attempts since boot. Monotonic: not reset by window
// expiry or by a success. Written under the table's lock; a reader sees a
// whole 32-bit value.
uint32_t getTotalFailedLoginCount();

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

// The one forced-password-change decision: true when the account's settings
// carry mustChangePassword and the account is not a guest (a guest cannot
// change its own password, so the flag never applies to one). Answers are
// cached per account and refilled after setUserPassword or an identity
// change. False when the account or its settings cannot be read.
// Web: tgRequireAuth admits only /account/password-change and /logout while
// this holds (webPasswordChangeAllows). Command transports do not gate on it
// yet; the user changes the password there with userchangepassword.
bool userMustChangePassword(const String& username);

// Public username/password intake rules (register + admin create). Username:
// 1..64 chars of [A-Za-z0-9._-], not a reserved sentinel. Password: 6..64 chars.
// errorOut (optional) gets a short fixed reason string — safe to show on public pages.
bool isValidPublicUsername(const String& username, String* errorOut = nullptr);
bool isValidPublicPassword(const String& password, String* errorOut = nullptr);
static constexpr size_t kPublicUsernameMaxLen = 64;
static constexpr size_t kPublicPasswordMaxLen = 64;
static constexpr size_t kPublicPasswordMinLen = 6;

// Self-registration owner, shared by /register/submit and `userrequest`. It
// takes the plaintext password directly (never through a command line, so
// spaces and quotes are kept), applies the public intake rules above, and
// under the filesystem lock appends the hashed request to pending_users.json
// through the atomic pending-list writer. At most 32 requests wait at once. A
// refused or failed request leaves the pending list as it was. It records no
// credential attempt; the caller decides what counts as one.
enum class AccountRequestResult : uint8_t {
  Submitted,
  InvalidUsername,
  PasswordTooShort,
  PasswordTooLong,
  AlreadyPending,
  QueueFull,
  NotReady,     // filesystem not mounted
  ReadFailed,   // pending list unreadable
  WriteFailed,  // pending list not written; the previous file is kept
};
AccountRequestResult submitAccountRequest(const String& username, const String& password);

// Read-only roster snapshot for the display pages and `userlist`: one row per
// users.json account in file order. username is the full stored name (an
// account whose stored name is longer than kPublicUsernameMaxLen is left out
// rather than cut). role is normalized to guest|user|admin|superadmin by the
// rule the authorization lookup uses: lower-cased, a missing role is
// superadmin for the first account and user otherwise, and an unknown or
// malformed role reads as user.
struct UserRosterEntry {
  uint32_t id;
  char username[kPublicUsernameMaxLen + 1];
  char role[11];
  bool banned;
};
// Fills up to `capacity` rows and stores the number written in countOut;
// totalOut (optional) gets the number of rows the roster holds. Returns false,
// with countOut 0, when users.json is missing, unreadable or malformed.
bool readUserRoster(UserRosterEntry* out, size_t capacity, size_t& countOut,
                    size_t* totalOut = nullptr);

// Admin: create a new account immediately (not pending). Password is hashed into per-user settings.
// If mustChangeOnLogin is true, userMustChangePassword stays set until they change password via setUserPassword.
// `role` is "user" (default), "admin", or "superadmin". CALLERS MUST RANK-CHECK
// FIRST — this is a data mutator and does not enforce "cannot grant a role above
// your own"; cmd_user_add does that via userMutationAllowed().
bool adminCreateUser(const String& username, const String& plainPassword, bool mustChangeOnLogin,
                     const String& createdBy, String& errorOut, const String& role = "user");

// Account creation owner. Every path that turns a name and a stored credential
// into a usable account (useradd, approve) calls this. It validates the name
// with isValidPublicUsername (reserved names included), requires a non-empty
// credential hash, and under one filesystem lock refuses a duplicate, allocates
// an id never issued before, stores the credential and publishes users.json
// last. Nothing is published when the credential write fails. The caller
// validates plaintext (isValidPublicPassword) and hashes it; this owner never
// sees a plaintext password. CALLERS MUST RANK-CHECK FIRST, as for adminCreateUser.
// A guest account with mustChangePassword is refused (a guest cannot change
// its own password, so it could never satisfy the forced change).
//
// "Never issued before" is kept by the account-id high-water mark in NVS
// (System_BootState): the new id is above the roster's ids, its nextId and the
// mark, and the mark is raised before anything is written. Creation fails when
// the mark cannot be read or raised. The first-setup owner is id 1 by design.
struct NewAccount {
  String username;                 // trimmed and validated here
  String passwordHash;             // hashUserPassword output, never plaintext
  String role = "user";            // unknown values collapse to "user"
  String createdBy;                // provenance recorded in users.json
  bool mustChangePassword = false; // userMustChangePassword until the first change
};
bool createAccount(const NewAccount& account, uint32_t& idOut, String& errorOut);

// Raises the account-id high-water mark to the highest id the current
// users.json has issued. factoryreset calls it before deleting users.json.
// False (with errorOut) when the roster cannot be read or the mark not raised.
bool recordAccountIdHighWater(String& errorOut);

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
//     role/account availability changes (promote/demote/ban/unban), or peer
//     ownership stamps (BLE pair).
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

// Export command registry for System_CommandRegistry.cpp
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
