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
// User Management Helper Functions (implemented in user_system.cpp)
// ============================================================================

// Password hashing function (used by .ino for first-time setup and password verification)
String hashUserPassword(const String& password);

// Transport-generic authentication functions
bool tgRequireAuth(AuthContext& ctx);
bool tgRequireAdmin(AuthContext& ctx);
bool isAdminUser(const String& who);

// Centralized transport authentication management
bool loginTransport(CommandSource transport, const String& username, const String& password);
void logoutTransport(CommandSource transport);
bool isTransportAuthenticated(CommandSource transport);
String getTransportUser(CommandSource transport);
bool isTransportAdmin(CommandSource transport);

// Credentials validation helpers (moved from main .ino)
bool isValidUser(const String& username, const String& password);
bool verifyUserPassword(const String& inputPassword, const String& storedHash);

// Update a user's password (stores hashed)
bool setUserPassword(const String& username, const String& newPasswordRaw);

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

const char* cmd_login(const String& cmd);
const char* cmd_logout(const String& cmd);

// Export command registry for system_utils.cpp
extern const CommandEntry userSystemCommands[];
extern const size_t userSystemCommandsCount;
const char* cmd_user_approve(const String& cmd);
const char* cmd_user_deny(const String& cmd);
const char* cmd_user_promote(const String& cmd);
const char* cmd_user_demote(const String& cmd);
const char* cmd_user_delete(const String& cmd);
const char* cmd_user_changepassword(const String& cmd);
const char* cmd_user_resetpassword(const String& cmd);
const char* cmd_user_list(const String& cmd);
const char* cmd_user_request(const String& cmd);
const char* cmd_user_sync(const String& cmd);
const char* cmd_pending_list(const String& cmd);
const char* cmd_session_list(const String& cmd);
const char* cmd_session_revoke(const String& cmd);

// ============================================================================
// Boot Sequence Management
// ============================================================================

// Increment boot sequence counter (memory-only, resets on power cycle)
void loadAndIncrementBootSeq();

// User system command registration
// User system command module is automatically registered

#endif // USER_SYSTEM_H
