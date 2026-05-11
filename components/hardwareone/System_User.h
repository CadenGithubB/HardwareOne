#ifndef USER_SYSTEM_H
#define USER_SYSTEM_H

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
enum CommandSource { 
  SOURCE_WEB = 0,
  SOURCE_SERIAL = 1,
  SOURCE_INTERNAL = 2,
  SOURCE_ESPNOW = 3,
  SOURCE_LOCAL_DISPLAY = 4,
  SOURCE_BLUETOOTH = 5,
  SOURCE_MQTT = 6,
  SOURCE_VOICE = 7
};

struct AuthContext {
  CommandSource transport;
  String path;   // URI for HTTP, command for CLI, view for TFT
  String ip;     // remote IP for HTTP, "local" for serial/TFT
  String user;   // resolved username when authenticated
  String sid;    // HTTP session id (empty for serial/TFT)
  void* opaque;  // httpd_req_t* when HTTP; nullptr otherwise
};

// User command registry - using userSystemCommands only

// ============================================================================
// Authentication State Globals (defined in HardwareOne.cpp)
// ============================================================================

extern bool gLocalDisplayAuthed;
extern String gLocalDisplayUser;

// ============================================================================
// User Management Helper Functions (implemented in user_system.cpp)
// ============================================================================

// Password hashing function (used by .ino for first-time setup and password verification)
String hashUserPassword(const String& password);

// Transport-generic authentication functions
bool tgRequireAuth(AuthContext& ctx);
bool isAdminUser(const String& who);

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

// Admin: create a new account immediately (not pending). Regular user role. Password is hashed into per-user settings.
// If mustChangeOnLogin is true, userMustChangePassword stays set until they change password via setUserPassword.
bool adminCreateUser(const String& username, const String& plainPassword, bool mustChangeOnLogin,
                     const String& createdBy, String& errorOut);

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
bool loadUsersFromFile(String& outUser, String& outPass);

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
const char* cmd_user_resetpassword(const String& argsInput);   // revoke       — admin reset target's creds
const char* cmd_user_add(const String& argsInput);       // bump        — new usable account exists (no sessions yet)
const char* cmd_user_list(const String& argsInput);
const char* cmd_user_request(const String& argsInput);   // —           — pending, can't auth yet
const char* cmd_user_sync(const String& argsInput);
const char* cmd_pending_list(const String& argsInput);
const char* cmd_session_list(const String& argsInput);
const char* cmd_session_revoke(const String& argsInput); // manual revoke — uses revokeUserSessions internally

// ============================================================================
// Boot Sequence Management
// ============================================================================

// Increment boot sequence counter (memory-only, resets on power cycle)
void loadAndIncrementBootSeq();

// User system command registration
// User system command module is automatically registered

#endif // USER_SYSTEM_H
