/**
 * User System Implementation
 * 
 * User management, session management, and authentication commands
 * Moved from main .ino to reduce file size and improve organization
 */

#include "System_User.h"
#include "System_BuildConfig.h"  // ENABLE_HTTP_SERVER flag
#if ENABLE_HTTP_SERVER
  #include "WebServer_Server.h"
#endif
#include "System_SensorStubs.h" // Network stubs when disabled
#include "System_Utils.h"  // For CommandEntry
#include "System_Command.h"  // For CommandModuleRegistrar
#include "System_Mutex.h"  // For FsLockGuard
#include "System_Debug.h"  // For DEBUG_AUTHF, DEBUG_USERF
#include "System_Logging.h" // For log file paths and constants
#include "System_Filesystem.h"    // For writeText, readText
#include "System_Settings.h"
#include "OLED_Display.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "System_UserSettings.h"
#include "System_MemUtil.h"
#include <Arduino.h>

// ============================================================================
// External Dependencies from .ino
// ============================================================================

extern bool filesystemReady;
// gDebugBuffer, gDebugFlags, ensureDebugBuffer now from debug_system.h
extern void broadcastOutput(const String& s);
extern void broadcastOutput(const char* s);

// Session management now in web_server.h (included above)

// File paths
#define PENDING_USERS_FILE "/system/pending_users.json"

// Memory allocation
// Utility functions from .ino
extern String getDeviceEncryptionKey();
extern String jsonEscape(const String& in);
extern void fsLock(const char* reason);
extern void fsUnlock();
extern void resolvePendingUserCreationTimes();

// Boot tracking globals
extern uint32_t gBootSeq;
extern uint32_t gBootCounter;

// Serial authentication globals
extern bool gSerialAuthed;
extern String gSerialUser;

// Local display authentication globals
extern bool gLocalDisplayAuthed;
extern String gLocalDisplayUser;

// Bluetooth authentication globals
extern bool gBluetoothAuthed;
extern String gBluetoothUser;

// Web authentication functions (from web_server.h)
extern bool isAuthed(httpd_req_t* req, String& outUser);
extern esp_err_t sendAuthRequiredResponse(httpd_req_t* req);
extern void getClientIP(httpd_req_t* req, String& ip);

// File I/O functions
extern bool readText(const char* path, String& out);

// Debug macros now defined in debug_system.h with performance optimizations
// DEBUG_USERS, DEBUG_USERSF, DEBUG_CMD_FLOW, DEBUG_CMD_FLOWF, DEBUG_SYSTEM, DEBUG_SYSTEMF

// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations

// RETURN_VALID_IF_VALIDATE_CSTR is defined centrally in system_utils.h;
// use that shared definition to keep behavior consistent across modules.

// ============================================================================
// Helper Functions
// ============================================================================

// Logout reason tracking moved to web_server.h
// Session helpers now in web_server.h (included above)

// ============================================================================
// Transport-Generic Authentication Functions
// ============================================================================

#if ENABLE_HTTP_SERVER
// Require auth across transports. Returns true if authenticated; otherwise emits the
// appropriate denial (401/console note) and returns false.
bool tgRequireAuth(AuthContext& ctx) {
  if (ctx.transport == SOURCE_WEB) {
    httpd_req_t* req = reinterpret_cast<httpd_req_t*>(ctx.opaque);
    if (!req) return false;
    // Prefer cached auth for high-frequency endpoints
    String userTmp;
    bool ok = isAuthed(req, userTmp);
    if (!ok) {
      sendAuthRequiredResponse(req);
      return false;
    }
    ctx.user = userTmp;
    if (ctx.ip.length() == 0) { getClientIP(req, ctx.ip); }
    return true;
  } else if (ctx.transport == SOURCE_SERIAL) {
    // Serial console auth state
    if (!gSerialAuthed) {
      broadcastOutput("ERROR: auth required");
      return false;
    }
    ctx.user = gSerialUser;
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    // Local display auth state - check if auth is required via settings
    // Allow commands during boot phase (before auth is enforced)
    extern Settings gSettings;
    extern bool oledBootModeActive;
    
    if (gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed && !oledBootModeActive) {
      broadcastOutput("ERROR: auth required (display)");
      return false;
    }
    ctx.user = gLocalDisplayAuthed ? gLocalDisplayUser : "display_system";
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else {
    // Internal/ESP-NOW commands - already authenticated upstream
    return true;
  }
}

// Admin check across transports; for HTTP, send 403 on failure.
bool tgRequireAdmin(AuthContext& ctx) {
  if (!tgRequireAuth(ctx)) return false;
  if (ctx.transport == SOURCE_WEB) {
    if (!isAdminUser(ctx.user)) {
      httpd_req_t* req = reinterpret_cast<httpd_req_t*>(ctx.opaque);
      if (req) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Forbidden: admin required", HTTPD_RESP_USE_STRLEN);
      }
      return false;
    }
    return true;
  } else if (ctx.transport == SOURCE_SERIAL) {
    if (!isAdminUser(ctx.user)) {
      broadcastOutput("ERROR: admin required");
      return false;
    }
    return true;
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    // Allow admin commands during boot phase
    extern bool oledBootModeActive;
    if (oledBootModeActive) {
      return true;  // Boot phase - allow all commands
    }
    if (!isAdminUser(ctx.user)) {
      broadcastOutput("ERROR: admin required (display)");
      return false;
    }
    return true;
  } else {
    // Internal/ESP-NOW - check admin privilege
    return isAdminUser(ctx.user);
  }
}
#else
// Stub implementations when HTTP server is disabled
bool tgRequireAuth(AuthContext& ctx) {
  // Serial auth only when HTTP server disabled
  if (ctx.transport == SOURCE_SERIAL) {
    if (!gSerialAuthed) {
      broadcastOutput("ERROR: auth required");
      return false;
    }
    ctx.user = gSerialUser;
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    // Allow commands during boot phase
    extern Settings gSettings;
    extern bool oledBootModeActive;
    
    if (gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed && !oledBootModeActive) {
      broadcastOutput("ERROR: auth required (display)");
      return false;
    }
    ctx.user = gLocalDisplayAuthed ? gLocalDisplayUser : "display_system";
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  }
  return true; // Internal commands pass through
}
bool tgRequireAdmin(AuthContext& ctx) {
  if (!tgRequireAuth(ctx)) return false;
  
  // Allow admin commands during boot phase
  extern bool oledBootModeActive;
  if (ctx.transport == SOURCE_LOCAL_DISPLAY && oledBootModeActive) {
    return true;  // Boot phase - allow all commands
  }
  
  if (ctx.transport == SOURCE_SERIAL) {
    if (!isAdminUser(ctx.user)) {
      broadcastOutput("ERROR: admin required");
      return false;
    }
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    if (!isAdminUser(ctx.user)) {
      broadcastOutput("ERROR: admin required (display)");
      return false;
    }
  }
  return isAdminUser(ctx.user);
}
#endif // ENABLE_HTTP_SERVER

// Determine if the given username is admin (any user with role == admin)
bool isAdminUser(const String& who) {
  if (!filesystemReady) return false;
  // Prefer JSON
  if (LittleFS.exists(USERS_JSON_FILE)) {
    String json;
    if (!readText(USERS_JSON_FILE, json)) return false;
    int usersIdx = json.indexOf("\"users\"");
    if (usersIdx < 0) return false;
    int firstUKey = json.indexOf("\"username\"", usersIdx);
    String firstUser = "";
    if (firstUKey >= 0) {
      int uq1 = json.indexOf('"', json.indexOf(':', firstUKey) + 1);
      int uq2 = json.indexOf('"', uq1 + 1);
      if (uq1 > 0 && uq2 > uq1) firstUser = json.substring(uq1 + 1, uq2);
    }
    // Search for target user and role
    int pos = usersIdx;
    while (true) {
      int uKey = json.indexOf("\"username\"", pos);
      if (uKey < 0) break;
      int uq1 = json.indexOf('"', json.indexOf(':', uKey) + 1);
      int uq2 = json.indexOf('"', uq1 + 1);
      if (uq1 < 0 || uq2 <= uq1) break;
      String uname = json.substring(uq1 + 1, uq2);
      int rKey = json.indexOf("\"role\"", uKey);
      int nextU = json.indexOf("\"username\"", uKey + 1);
      if (rKey > 0 && (nextU < 0 || rKey < nextU)) {
        int rq1 = json.indexOf('"', json.indexOf(':', rKey) + 1);
        int rq2 = json.indexOf('"', rq1 + 1);
        String role = (rq1 > 0 && rq2 > rq1) ? json.substring(rq1 + 1, rq2) : String("");
        if (uname == who && role == "admin") return true;
      }
      pos = uq2 + 1;
    }
    // Fallback: first user without role is admin
    return (who == firstUser);
  }
  return false;
}

// ============================================================================
// Centralized Transport Authentication Management
// ============================================================================

bool loginTransport(CommandSource transport, const String& username, const String& password) {
  // Validate credentials first
  if (!isValidUser(username, password)) {
    return false;
  }
  
  // Set auth state based on transport
  switch (transport) {
    case SOURCE_SERIAL:
      gSerialAuthed = true;
      gSerialUser = username;
      return true;
      
    case SOURCE_LOCAL_DISPLAY:
      gLocalDisplayAuthed = true;
      gLocalDisplayUser = username;
      oledNotifyLocalDisplayAuthChanged();
      return true;
      
    case SOURCE_BLUETOOTH:
      gBluetoothAuthed = true;
      gBluetoothUser = username;
      return true;
      
    case SOURCE_WEB:
      // Web auth is handled separately via session cookies
      // This function doesn't apply to web transport
      return false;
      
    default:
      return false;
  }
}

void logoutTransport(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL:
      gSerialAuthed = false;
      gSerialUser = String();
      break;
      
    case SOURCE_LOCAL_DISPLAY:
      gLocalDisplayAuthed = false;
      gLocalDisplayUser = String();
      oledNotifyLocalDisplayAuthChanged();
      break;
      
    case SOURCE_BLUETOOTH:
      gBluetoothAuthed = false;
      gBluetoothUser = String();
      break;
      
    case SOURCE_WEB:
      // Web logout is handled separately via session management
      break;
      
    default:
      break;
  }
}

bool isTransportAuthenticated(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL:
      return gSerialAuthed;
      
    case SOURCE_LOCAL_DISPLAY:
      return gLocalDisplayAuthed;
      
    case SOURCE_BLUETOOTH:
      return gBluetoothAuthed;
      
    case SOURCE_WEB:
      // Web auth requires request context, can't check here
      return false;
      
    default:
      return false;
  }
}

String getTransportUser(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL:
      return gSerialUser;
      
    case SOURCE_LOCAL_DISPLAY:
      return gLocalDisplayUser;
      
    case SOURCE_BLUETOOTH:
      return gBluetoothUser;
      
    case SOURCE_WEB:
      // Web user requires request context, can't get here
      return String();
      
    default:
      return String();
  }
}

bool isTransportAdmin(CommandSource transport) {
  String user = getTransportUser(transport);
  if (user.length() == 0) {
    return false;
  }
  return isAdminUser(user);
}

// ============================================================================
// Password Hashing
// ============================================================================

// Password hashing
String hashUserPassword(const String& password) {
  if (password.length() == 0) return "";

  String salt = getDeviceEncryptionKey();
  String saltedPassword = password + salt;

  uint32_t hash = 0;
  for (int i = 0; i < saltedPassword.length(); i++) {
    hash = hash * 31 + (uint8_t)saltedPassword[i];
    hash ^= (hash >> 16);
  }

  String hashStr = String(hash, HEX);
  while (hashStr.length() < 8) hashStr = "0" + hashStr;

  return "HASH:" + hashStr;
}

bool verifyUserPassword(const String& inputPassword, const String& storedHash) {
  if (inputPassword.length() == 0 || storedHash.length() == 0) return false;

  if (!storedHash.startsWith("HASH:")) {
    return (inputPassword == storedHash);
  }

  // Hash the input password with device salt and compare
  String inputHash = hashUserPassword(inputPassword);
  return (inputHash == storedHash);
}

// Validate a username/password against users.json
bool isValidUser(const String& u, const String& p) {
  if (!filesystemReady) return false;

  if (LittleFS.exists(USERS_JSON_FILE)) {
    String json;
    if (!readText(USERS_JSON_FILE, json)) return false;
    int pos = json.indexOf("\"users\"");
    if (pos < 0) return false;
    while (true) {
      int uKey = json.indexOf("\"username\"", pos);
      if (uKey < 0) break;
      int uq1 = json.indexOf('"', json.indexOf(':', uKey) + 1);
      int uq2 = json.indexOf('"', uq1 + 1);
      if (uq1 < 0 || uq2 <= uq1) break;
      String uname = json.substring(uq1 + 1, uq2);
      int pKey = json.indexOf("\"password\"", uKey);
      int nextU = json.indexOf("\"username\"", uKey + 1);
      if (pKey > 0 && (nextU < 0 || pKey < nextU)) {
        int pq1 = json.indexOf('"', json.indexOf(':', pKey) + 1);
        int pq2 = json.indexOf('"', pq1 + 1);
        String pass = (pq1 > 0 && pq2 > pq1) ? json.substring(pq1 + 1, pq2) : String("");
        if (u == uname && verifyUserPassword(p, pass)) return true;
      }
      pos = uq2 + 1;
    }
    return false;
  }
  return false;
}

bool getUserIdByUsername(const String& username, uint32_t& outUserId) {
  outUserId = 0;
  if (!filesystemReady) return false;
  if (username.length() == 0) return false;

  {
    FsLockGuard guard("users.get_id");
    if (!LittleFS.exists(USERS_JSON_FILE)) return false;
    File f = LittleFS.open(USERS_JSON_FILE, "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    JsonArray users = doc["users"].as<JsonArray>();
    if (!users) return false;

    for (JsonObject uObj : users) {
      const char* uname = uObj["username"] | "";
      if (username == uname) {
        outUserId = (uint32_t)(uObj["id"] | 0);
        return outUserId > 0;
      }
    }
  }

  return false;
}


// Get user role from users.json
bool getUserRole(const String& username, String& outRole) {
  outRole = "";
  if (!filesystemReady) return false;
  if (username.length() == 0) return false;

  FsLockGuard guard("users.get_role");
  if (!LittleFS.exists(USERS_JSON_FILE)) return false;
  File f = LittleFS.open(USERS_JSON_FILE, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  JsonArray users = doc["users"].as<JsonArray>();
  if (!users) return false;

  for (JsonObject uObj : users) {
    const char* uname = uObj["username"] | "";
    if (username == uname) {
      outRole = String(uObj["role"] | "user");
      return true;
    }
  }

  return false;
}

// findSessionIndexBySID moved to web_server.cpp

// ============================================================================
// User Management Internal Functions
// ============================================================================

bool approvePendingUserInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] approve internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  // Load pending list and extract approved user using ArduinoJson
  String userPassword = "";
  bool found = false;

  if (!LittleFS.exists("/system/pending_users.json")) {
    errorOut = "User not found in pending list";
    return false;
  }

  // Parse pending_users.json with ArduinoJson
  File file = LittleFS.open("/system/pending_users.json", "r");
  if (!file) {
    errorOut = "Could not read pending list";
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    errorOut = "Malformed pending_users.json";
    return false;
  }

  // Build new array without the approved user
  JsonDocument newDoc;
  JsonArray newArray = newDoc.to<JsonArray>();
  JsonArray pendingArray = doc.as<JsonArray>();

  for (JsonObject user : pendingArray) {
    const char* objUsername = user["username"];
    if (objUsername && username == objUsername) {
      // Found the user to approve - extract password
      const char* pass = user["password"];
      if (pass) {
        userPassword = String(pass);
      }
      found = true;
      // Don't add this user to the new array (removing them)
    } else {
      // Keep this user in the list
      newArray.add(user);
    }
  }

  if (!found) {
    errorOut = "User not found in pending list";
    return false;
  }

  // Write updated pending list or remove file if empty
  if (newArray.size() == 0) {
    // Remove file if empty
    fsLock("pending_users.remove");
    LittleFS.remove("/system/pending_users.json");
    fsUnlock();
  } else {
    // Write updated list
    file = LittleFS.open("/system/pending_users.json", "w");
    if (!file) {
      errorOut = "Could not update pending list";
      return false;
    }
    size_t written = serializeJson(newDoc, file);
    file.close();
    
    if (written == 0) {
      errorOut = "Could not update pending list";
      return false;
    }
  }

  // Append approved user to users.json (JSON-only policy)
  uint32_t createdUserId = 0;
  if (!LittleFS.exists(USERS_JSON_FILE)) {
    // Create users.json with the first user (ID 1) using ArduinoJson
    JsonDocument doc;
    doc["version"] = 1;
    doc["bootCounter"] = gBootCounter;
    doc["nextId"] = 2;
    
    JsonArray users = doc["users"].to<JsonArray>();
    JsonObject user = users.add<JsonObject>();
    user["id"] = 1;
    user["username"] = username;
    user["password"] = userPassword;
    user["role"] = "admin";
    user["createdAt"] = (const char*)nullptr;  // null
    user["createdBy"] = "provisional";
    user["createdMs"] = millis();
    user["bootSeq"] = gBootSeq;
    user["bootCount"] = gBootCounter;
    
    doc["bootAnchors"].to<JsonArray>();
    
    DEBUG_SYSTEMF("ApproveInit: Creating users.json with bootCounter=%lu, admin.bootCount=%lu, gBootSeq=%lu", (unsigned long)gBootCounter, (unsigned long)gBootCounter, (unsigned long)gBootSeq);
    
    // Serialize to file
    File file = LittleFS.open(USERS_JSON_FILE, "w");
    if (!file) {
      errorOut = "Failed to create users.json";
      return false;
    }
    size_t written = serializeJson(doc, file);
    file.close();
    
    if (written == 0) {
      errorOut = "Failed to write users.json";
      return false;
    }

    createdUserId = 1;
  } else {
    // Parse existing users.json with ArduinoJson
    File file = LittleFS.open(USERS_JSON_FILE, "r");
    if (!file) {
      errorOut = "Failed to open users.json";
      return false;
    }
    
    JsonDocument doc;  // Use dynamic document for users.json
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
      errorOut = "Malformed users.json";
      return false;
    }
    
    // Get nextId
    int nextId = doc["nextId"] | 2;  // Default to 2 if missing
    
    // Check if user already exists
    JsonArray users = doc["users"];
    if (!users) {
      errorOut = "Malformed users.json - missing users array";
      return false;
    }
    
    for (JsonObject user : users) {
      const char* existingUsername = user["username"];
      if (existingUsername && username == existingUsername) {
        errorOut = "Username already exists";
        return false;
      }
    }
    
    JsonObject newUser = users.add<JsonObject>();
    newUser["id"] = nextId;
    newUser["username"] = username;
    newUser["password"] = userPassword;
    newUser["role"] = "user";
    newUser["createdAt"] = (const char*)nullptr;  // null
    newUser["createdBy"] = "provisional";
    newUser["createdMs"] = millis();
    newUser["bootSeq"] = gBootSeq;
    newUser["bootCount"] = gBootCounter;
    
    DEBUG_SYSTEMF("ApproveAppend: New user id=%d with bootCount=%lu, gBootSeq=%lu", nextId, (unsigned long)gBootCounter, (unsigned long)gBootSeq);
    
    // Update nextId
    doc["nextId"] = nextId + 1;
    
    // Write back to file
    file = LittleFS.open(USERS_JSON_FILE, "w");
    if (!file) {
      errorOut = "Failed to write users.json";
      return false;
    }
    size_t written = serializeJson(doc, file);
    file.close();
    
    if (written == 0) {
      errorOut = "Failed to write users.json";
      return false;
    }

    createdUserId = (uint32_t)nextId;
  }

  if (createdUserId > 0 && filesystemReady) {
    String settingsPath = getUserSettingsPath(createdUserId);
    FsLockGuard guard("user_settings.default");
    if (!LittleFS.exists(settingsPath.c_str())) {
      JsonDocument defaults;
      defaults["theme"] = "light";
      if (!saveUserSettings(createdUserId, defaults)) {
        WARN_SESSIONF("Failed to create default user settings for userId=%u", (unsigned)createdUserId);
      }
    }
  }

  broadcastOutput(String("[admin] Approved user: ") + username + " with requested password");

  // If NTP already synced, resolve the creation timestamp immediately
  if (time(nullptr) > 0) {
    resolvePendingUserCreationTimes();
  }

  return true;
}

bool denyPendingUserInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] deny internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  
  if (!LittleFS.exists("/system/pending_users.json")) {
    errorOut = "User not found in pending list";
    return false;
  }

  // Parse pending_users.json with ArduinoJson
  File file = LittleFS.open("/system/pending_users.json", "r");
  if (!file) {
    errorOut = "Could not read pending list";
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    errorOut = "Malformed pending_users.json";
    return false;
  }

  // Build new array without the denied user
  JsonDocument newDoc;
  JsonArray newArray = newDoc.to<JsonArray>();
  JsonArray pendingArray = doc.as<JsonArray>();
  bool found = false;

  for (JsonObject user : pendingArray) {
    const char* objUsername = user["username"];
    if (objUsername && username == objUsername) {
      // Found the user to deny - don't add to new array
      found = true;
    } else {
      // Keep this user in the list
      newArray.add(user);
    }
  }

  if (!found) {
    errorOut = "User not found in pending list";
    return false;
  }

  // Write updated pending list or remove file if empty
  if (newArray.size() == 0) {
    // Remove file if empty
    LittleFS.remove("/system/pending_users.json");
  } else {
    // Write updated list
    file = LittleFS.open("/system/pending_users.json", "w");
    if (!file) {
      errorOut = "Could not update pending list";
      return false;
    }
    size_t written = serializeJson(newDoc, file);
    file.close();
    
    if (written == 0) {
      errorOut = "Could not update pending list";
      return false;
    }
  }
  
  return true;
}

// Promote an existing user in users.json to admin (JSON-only)
static bool promoteUserToAdminInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] promote internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  if (!LittleFS.exists(USERS_JSON_FILE)) {
    errorOut = "users.json not found";
    return false;
  }
  String json;
  if (!readText(USERS_JSON_FILE, json)) {
    errorOut = "Failed to read users.json";
    return false;
  }
  int usersIdx = json.indexOf("\"users\"");
  int openBracket = (usersIdx >= 0) ? json.indexOf('[', usersIdx) : -1;
  int closeBracket = (openBracket >= 0) ? json.indexOf(']', openBracket) : -1;
  if (openBracket < 0 || closeBracket <= openBracket) {
    errorOut = "Malformed users.json";
    return false;
  }

  // Find the object for this username within the users array
  int searchPos = openBracket + 1;
  bool updated = false;
  while (true) {
    int objStart = json.indexOf('{', searchPos);
    if (objStart < 0 || objStart > closeBracket) break;
    int objEnd = json.indexOf('}', objStart);
    if (objEnd < 0 || objEnd > closeBracket) break;
    String obj = json.substring(objStart, objEnd + 1);

    int un = obj.indexOf("\"username\":");
    if (un >= 0) {
      un += 11;  // skip "username":
      // Skip optional space after colon
      while (un < obj.length() && obj[un] == ' ') un++;
      // Skip opening quote
      if (un < obj.length() && obj[un] == '"') un++;
      int unEnd = obj.indexOf('"', un);
      if (unEnd > un) {
        String name = obj.substring(un, unEnd);
        if (name == username) {
          // Check for ID field (founder protection) - only for the target user
          int idStart = obj.indexOf("\"id\":");
          if (idStart >= 0) {
            idStart += 5;  // skip "id":
            // Skip optional space
            while (idStart < obj.length() && obj[idStart] == ' ') idStart++;
            int idEnd = idStart;
            while (idEnd < obj.length() && obj[idEnd] >= '0' && obj[idEnd] <= '9') idEnd++;
            if (idEnd > idStart) {
              int userId = obj.substring(idStart, idEnd).toInt();
              if (userId == 1) {
                errorOut = "Cannot modify the first admin account";
                return false;
              }
            }
          }

          // Look for role field in the full JSON
          int roleFieldStart = json.indexOf("\"role\":", objStart);
          if (roleFieldStart >= 0 && roleFieldStart < objEnd) {
            int roleValueStart = roleFieldStart + 7;  // skip "role":
            // Skip optional space and quote
            while (roleValueStart < json.length() && (json[roleValueStart] == ' ' || json[roleValueStart] == '"')) {
              if (json[roleValueStart] == '"') { roleValueStart++; break; }
              roleValueStart++;
            }
            int roleValueEnd = json.indexOf('"', roleValueStart);
            if (roleValueEnd > roleValueStart && roleValueEnd < objEnd) {
              // Replace the entire role value with "admin"
              String before = json.substring(0, roleValueStart);
              String after = json.substring(roleValueEnd);
              json = before + String("admin") + after;
              updated = true;
              break;
            }
          } else {
            // No role field; insert before closing brace
            String ins = String(",\"role\":\"admin\"");
            String before = json.substring(0, objEnd);
            String after = json.substring(objEnd);
            json = before + ins + after;
            updated = true;
            break;
          }
        }
      }
    }
    searchPos = objEnd + 1;
  }
  if (!updated) {
    errorOut = "User not found";
    return false;
  }
  if (!writeText(USERS_JSON_FILE, json)) {
    errorOut = "Failed to write users.json";
    return false;
  }
  broadcastOutput(String("[admin] Promoted user to admin: ") + username);

  // Serial admin status now checked in real-time via isAdminUser()
  if (gSerialAuthed && gSerialUser == username) {
    broadcastOutput("[serial] Your admin privileges have been updated");
  }

  return true;
}

// Demote an existing admin user in users.json to regular user (JSON-only)
static bool demoteUserFromAdminInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] demote internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  if (!LittleFS.exists(USERS_JSON_FILE)) {
    errorOut = "users.json not found";
    return false;
  }
  String json;
  if (!readText(USERS_JSON_FILE, json)) {
    errorOut = "Failed to read users.json";
    return false;
  }
  int usersIdx = json.indexOf("\"users\"");
  int openBracket = (usersIdx >= 0) ? json.indexOf('[', usersIdx) : -1;
  int closeBracket = (openBracket >= 0) ? json.indexOf(']', openBracket) : -1;
  if (openBracket < 0 || closeBracket <= openBracket) {
    errorOut = "Malformed users.json";
    return false;
  }

  // Find the object for this username within the users array
  int searchPos = openBracket + 1;
  bool updated = false;
  while (true) {
    int objStart = json.indexOf('{', searchPos);
    if (objStart < 0 || objStart > closeBracket) break;
    int objEnd = json.indexOf('}', objStart);
    if (objEnd < 0 || objEnd > closeBracket) break;
    String obj = json.substring(objStart, objEnd + 1);

    int un = obj.indexOf("\"username\":");
    if (un >= 0) {
      un += 11;  // skip "username":
      // Skip optional space after colon
      while (un < obj.length() && obj[un] == ' ') un++;
      // Skip opening quote
      if (un < obj.length() && obj[un] == '"') un++;
      int unEnd = obj.indexOf('"', un);
      if (unEnd > un) {
        String name = obj.substring(un, unEnd);
        if (name == username) {
          // Check for ID field (founder protection) - only for the target user
          int idStart = obj.indexOf("\"id\":");
          if (idStart >= 0) {
            idStart += 5;  // skip "id":
            // Skip optional space
            while (idStart < obj.length() && obj[idStart] == ' ') idStart++;
            int idEnd = idStart;
            while (idEnd < obj.length() && obj[idEnd] >= '0' && obj[idEnd] <= '9') idEnd++;
            if (idEnd > idStart) {
              int userId = obj.substring(idStart, idEnd).toInt();
              if (userId == 1) {
                errorOut = "Cannot modify the first admin account";
                return false;
              }
            }
          }

          // Look for role field in the full JSON
          int roleFieldStart = json.indexOf("\"role\":", objStart);
          if (roleFieldStart >= 0 && roleFieldStart < objEnd) {
            int roleValueStart = roleFieldStart + 7;  // skip "role":
            // Skip optional space and quote
            while (roleValueStart < json.length() && (json[roleValueStart] == ' ' || json[roleValueStart] == '"')) {
              if (json[roleValueStart] == '"') { roleValueStart++; break; }
              roleValueStart++;
            }
            int roleValueEnd = json.indexOf('"', roleValueStart);
            if (roleValueEnd > roleValueStart && roleValueEnd < objEnd) {
              String currentRole = json.substring(roleValueStart, roleValueEnd);
              if (currentRole != "admin") {
                errorOut = "User is not an admin";
                return false;
              }
              // Replace the entire role value with "user"
              String before = json.substring(0, roleValueStart);
              String after = json.substring(roleValueEnd);
              json = before + String("user") + after;
              updated = true;
              break;
            }
          } else {
            // No role field means user role (default), already demoted
            errorOut = "User is already a regular user";
            return false;
          }
        }
      }
    }
    searchPos = objEnd + 1;
  }
  if (!updated) {
    errorOut = "User not found";
    return false;
  }
  if (!writeText(USERS_JSON_FILE, json)) {
    errorOut = "Failed to write users.json";
    return false;
  }
  broadcastOutput(String("[admin] Demoted user from admin: ") + username);

  // Serial admin status now checked in real-time via isAdminUser()
  if (gSerialAuthed && gSerialUser == username) {
    broadcastOutput("[serial] Your admin privileges have been revoked");
  }

  return true;
}

// Delete an existing user from users.json (JSON-only)
static bool deleteUserInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] delete internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  if (!LittleFS.exists(USERS_JSON_FILE)) {
    errorOut = "users.json not found";
    return false;
  }
  String json;
  if (!readText(USERS_JSON_FILE, json)) {
    errorOut = "Failed to read users.json";
    return false;
  }
  int usersIdx = json.indexOf("\"users\"");
  int openBracket = (usersIdx >= 0) ? json.indexOf('[', usersIdx) : -1;
  int closeBracket = (openBracket >= 0) ? json.indexOf(']', openBracket) : -1;
  if (openBracket < 0 || closeBracket <= openBracket) {
    errorOut = "Malformed users.json";
    return false;
  }

  // Find the object for this username within the users array
  int searchPos = openBracket + 1;
  bool deleted = false;
  while (true) {
    int objStart = json.indexOf('{', searchPos);
    if (objStart < 0 || objStart > closeBracket) break;
    int objEnd = json.indexOf('}', objStart);
    if (objEnd < 0 || objEnd > closeBracket) break;
    String obj = json.substring(objStart, objEnd + 1);

    int un = obj.indexOf("\"username\":");
    if (un >= 0) {
      un += 11;  // skip "username":
      // Skip optional space after colon
      while (un < obj.length() && obj[un] == ' ') un++;
      // Skip opening quote
      if (un < obj.length() && obj[un] == '"') un++;
      int unEnd = obj.indexOf('"', un);
      if (unEnd > un) {
        String name = obj.substring(un, unEnd);
        if (name == username) {
          // Check for ID field (founder protection) - only for the target user
          int idStart = obj.indexOf("\"id\":");
          if (idStart >= 0) {
            idStart += 5;  // skip "id":
            // Skip optional space
            while (idStart < obj.length() && obj[idStart] == ' ') idStart++;
            int idEnd = idStart;
            while (idEnd < obj.length() && obj[idEnd] >= '0' && obj[idEnd] <= '9') idEnd++;
            if (idEnd > idStart) {
              int userId = obj.substring(idStart, idEnd).toInt();
              if (userId == 1) {
                errorOut = "Cannot delete the first admin account";
                return false;
              }
            }
          }

          // Find the start and end of this user object including commas
          int deleteStart = objStart;
          int deleteEnd = objEnd + 1;

          // Check if there's a comma before this object
          int commaBeforePos = deleteStart - 1;
          while (commaBeforePos > openBracket && (json[commaBeforePos] == ' ' || json[commaBeforePos] == '\n' || json[commaBeforePos] == '\r' || json[commaBeforePos] == '\t')) {
            commaBeforePos--;
          }
          bool hasCommaBefore = (commaBeforePos > openBracket && json[commaBeforePos] == ',');

          // Check if there's a comma after this object
          int commaAfterPos = deleteEnd;
          while (commaAfterPos < closeBracket && (json[commaAfterPos] == ' ' || json[commaAfterPos] == '\n' || json[commaAfterPos] == '\r' || json[commaAfterPos] == '\t')) {
            commaAfterPos++;
          }
          bool hasCommaAfter = (commaAfterPos < closeBracket && json[commaAfterPos] == ',');

          // Determine what to delete
          if (hasCommaBefore && hasCommaAfter) {
            // Middle object: delete from start to after comma
            deleteEnd = commaAfterPos + 1;
          } else if (hasCommaBefore && !hasCommaAfter) {
            // Last object: delete from before comma to end
            deleteStart = commaBeforePos;
          } else if (!hasCommaBefore && hasCommaAfter) {
            // First object: delete from start to after comma
            deleteEnd = commaAfterPos + 1;
          }
          // If no commas (only object), just delete the object itself

          // Remove the user object
          String before = json.substring(0, deleteStart);
          String after = json.substring(deleteEnd);
          json = before + after;
          deleted = true;
          break;
        }
      }
    }
    searchPos = objEnd + 1;
  }
  if (!deleted) {
    errorOut = "User not found";
    return false;
  }
  if (!writeText(USERS_JSON_FILE, json)) {
    errorOut = "Failed to write users.json";
    return false;
  }

  // Force logout all sessions for the deleted user
  int revokedSessions = 0;
  String reason = "Account deleted by administrator";

#if ENABLE_HTTP_SERVER
  // Revoke web sessions
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    if (!gSessions[i].sid.length()) continue;
    if (!gSessions[i].user.equalsIgnoreCase(username)) continue;
    if (gSessions[i].ip.length() > 0) {
      storeLogoutReason(gSessions[i].ip, reason);
    }
    enqueueTargetedRevokeForSessionIdx(i, reason);
    revokedSessions++;
  }
#endif

  // Force logout serial session if this user is logged in
  if (gSerialAuthed && gSerialUser.equalsIgnoreCase(username)) {
    gSerialAuthed = false;
    gSerialUser = String();
    broadcastOutput("[serial] Your account has been deleted. You have been logged out.");
    revokedSessions++;  // Count serial session too
  }

  broadcastOutput(String("[admin] Deleted user: ") + username + (revokedSessions > 0 ? String(" (") + String(revokedSessions) + " active session(s) terminated)" : ""));
  return true;
}

// ============================================================================
// User Command Handlers
// ============================================================================

const char* cmd_user_approve(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsIn;
  username.trim();
  DEBUG_USERSF("[users] CLI approve username=%s", username.c_str());
  String err;
  if (!approvePendingUserInternal(username, err)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Approved";
  snprintf(getDebugBuffer(), 1024, "Approved user '%s'", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_deny(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsIn;
  username.trim();
  DEBUG_USERSF("[users] CLI deny username=%s", username.c_str());
  String err;
  if (!denyPendingUserInternal(username, err)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Denied";
  snprintf(getDebugBuffer(), 1024, "Denied user '%s'", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_promote(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsIn;
  username.trim();
  if (username.length() == 0) return "Usage: user promote <username>";
  DEBUG_USERSF("[users] CLI promote username=%s", username.c_str());
  String err;
  if (!promoteUserToAdminInternal(username, err)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Promoted";
  snprintf(getDebugBuffer(), 1024, "Promoted user '%s' to admin", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_demote(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsIn;
  username.trim();
  if (username.length() == 0) return "Usage: user demote <username>";
  DEBUG_USERSF("[users] CLI demote username=%s", username.c_str());
  String err;
  if (!demoteUserFromAdminInternal(username, err)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Demoted";
  snprintf(getDebugBuffer(), 1024, "Demoted user '%s' to regular user", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_delete(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsIn;
  username.trim();
  if (username.length() == 0) return "Usage: user delete <username>";
  DEBUG_USERSF("[users] CLI delete username=%s", username.c_str());
  String err;
  if (!deleteUserInternal(username, err)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Deleted";
  snprintf(getDebugBuffer(), 1024, "Deleted user '%s'", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_list(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  // Check if JSON output is requested (just check for "json" in args)
  bool jsonOutput = (args.indexOf("json") >= 0);
  
  DEBUG_USERSF("[USER_LIST_DEBUG] Called with args='%s', jsonOutput=%d", args.c_str(), jsonOutput);

  if (!LittleFS.exists(USERS_JSON_FILE)) {
    DEBUG_USERSF("[USER_LIST_DEBUG] File not found: %s", USERS_JSON_FILE);
    return jsonOutput ? "[]" : "No users found";
  }

  // Open and parse users file with ArduinoJson
  File file = LittleFS.open(USERS_JSON_FILE, "r");
  if (!file) {
    ERROR_SESSIONF("Failed to open users file");
    if (jsonOutput) return "[]";
    broadcastOutput("Error: Failed to read users file");
    return "ERROR";
  }

  // Parse JSON document (2KB should be enough for users file)
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    ERROR_SESSIONF("JSON parse error: %s", error.c_str());
    if (jsonOutput) return "[]";
    broadcastOutput("Error: Malformed users file");
    return "ERROR";
  }

  JsonArray users = doc["users"];
  if (!users) {
    DEBUG_USERSF("[USER_LIST_DEBUG] No users array found");
    return jsonOutput ? "[]" : "No users found";
  }

  if (jsonOutput) {
    // Use static PSRAM buffer - 2KB sufficient for user list
    static char* jsonBuf = nullptr;
    static const size_t kBufSize = 2048;
    if (!jsonBuf) {
      jsonBuf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "user.list.json");
      if (!jsonBuf) return "[]";
    }
    size_t len = serializeJson(users, jsonBuf, kBufSize);
    if (len >= kBufSize) {
      ERROR_MEMORYF("user list JSON truncated: %zu >= %zu", len, kBufSize);
    }
    return jsonBuf;
  } else {
    // Stream human-readable format
    broadcastOutput("Users:");

    int userCount = 0;
    for (JsonObject user : users) {
      const char* username = user["username"];
      const char* role = user["role"] | "user";
      
      if (username) {
        BROADCAST_PRINTF("  %s (%s)", username, role);
        userCount++;
      }
    }

    if (userCount == 0) {
      broadcastOutput("No users found");
    }
    return "OK";
  }
}

const char* cmd_pending_list(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  // Check if JSON output is requested
  bool jsonOutput = (args.indexOf("json") >= 0);

  if (!LittleFS.exists("/system/pending_users.json")) {
    if (jsonOutput) return "[]";
    broadcastOutput("No pending users");
    return "OK";
  }

  // Open and parse pending users file with ArduinoJson
  File file = LittleFS.open("/system/pending_users.json", "r");
  if (!file) {
    if (jsonOutput) return "[]";
    ERROR_SESSIONF("Failed to read pending users file");
    broadcastOutput("Error: Failed to read pending users file");
    return "ERROR";
  }

  // Parse JSON array (1KB should be enough for pending users)
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    if (jsonOutput) return "[]";
    ERROR_SESSIONF("Malformed pending users file");
    broadcastOutput("Error: Malformed pending users file");
    return "ERROR";
  }

  JsonArray pending = doc.as<JsonArray>();
  if (!pending) {
    return jsonOutput ? "[]" : "No pending users";
  }

  if (jsonOutput) {
    // Use static PSRAM buffer - 2KB sufficient for pending list
    static char* jsonBuf = nullptr;
    static const size_t kBufSize = 2048;
    if (!jsonBuf) {
      jsonBuf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "pending.list.json");
      if (!jsonBuf) return "[]";
    }
    // Self-repair on overflow (prevents DoS via flooding pending requests)
    serializeJsonArrayWithRepair(pending, jsonBuf, kBufSize, "pending list");
    return jsonBuf;
  } else {
    // Stream human-readable format
    broadcastOutput("Pending Users:");

    int userCount = 0;
    for (JsonObject user : pending) {
      const char* username = user["username"];
      if (username) {
        BROADCAST_PRINTF("  %s (pending approval)", username);
        userCount++;
      }
    }

    if (userCount == 0) {
      broadcastOutput("No pending users");
    }
    return "OK";
  }
}

#if ENABLE_HTTP_SERVER
const char* cmd_session_list(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Check if JSON output is requested
  bool jsonOutput = (args.indexOf("json") >= 0);

  if (jsonOutput) {
    // Use static PSRAM buffer - 2KB sufficient for session list
    static char* jsonBuf = nullptr;
    static const size_t kBufSize = 2048;
    if (!jsonBuf) {
      jsonBuf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "session.list.json");
      if (!jsonBuf) return "[]";
    }
    JsonDocument doc;
    JsonArray sessions = doc.to<JsonArray>();
    buildAllSessionsJson("", sessions);
    size_t len = serializeJson(sessions, jsonBuf, kBufSize);
    if (len >= kBufSize) {
      ERROR_MEMORYF("session list JSON truncated: %zu >= %zu", len, kBufSize);
    }
    return jsonBuf;
  } else {
    // Stream human-readable format
    broadcastOutput("Active Sessions:");

    int sessionCount = 0;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
      const SessionEntry& s = gSessions[i];
      if (s.user.length() == 0) continue;  // empty slot
      BROADCAST_PRINTF("  %s from %s (last: %lu)", s.user.c_str(), s.ip.c_str(), s.lastSeen);
      sessionCount++;
    }

    if (sessionCount == 0) {
      broadcastOutput("No active sessions");
    }
    return "OK";
  }
}

const char* cmd_login(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String cmd = originalCmd;
  cmd.trim();

  // Parse: <username> <password> [transport]
  // transport can be: serial, display, bluetooth
  String rest = cmd;
  rest.trim();

  int sp1 = rest.indexOf(' ');
  if (sp1 <= 0) {
    return "Usage: login <username> <password> [transport]\nTransport: serial (default), display, bluetooth";
  }

  String username = rest.substring(0, sp1);
  String remainder = rest.substring(sp1 + 1);
  remainder.trim();

  int sp2 = remainder.indexOf(' ');
  String password;
  String transportStr = "serial";  // default

  if (sp2 > 0) {
    password = remainder.substring(0, sp2);
    transportStr = remainder.substring(sp2 + 1);
    transportStr.trim();
    transportStr.toLowerCase();
  } else {
    password = remainder;
  }

  // Map transport string to enum
  CommandSource transport = SOURCE_SERIAL;
  if (transportStr == "display") {
    transport = SOURCE_LOCAL_DISPLAY;
  } else if (transportStr == "bluetooth") {
    transport = SOURCE_BLUETOOTH;
  } else if (transportStr == "serial") {
    transport = SOURCE_SERIAL;
  } else {
    return "Invalid transport. Use: serial, display, or bluetooth";
  }

  // Attempt login
  if (loginTransport(transport, username, password)) {
    bool isAdmin = isAdminUser(username);
    static char buf[128];
    snprintf(buf, sizeof(buf), "Login successful for '%s' on %s%s",
             username.c_str(), transportStr.c_str(), isAdmin ? " (admin)" : "");
    return buf;
  } else {
    return "Authentication failed";
  }
}

const char* cmd_logout(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String cmd = originalCmd;
  cmd.trim();

  // Parse: [transport]
  String rest = cmd;
  rest.trim();
  rest.toLowerCase();

  CommandSource transport = SOURCE_SERIAL;  // default
  if (rest.length() > 0) {
    if (rest == "display") {
      transport = SOURCE_LOCAL_DISPLAY;
    } else if (rest == "bluetooth") {
      transport = SOURCE_BLUETOOTH;
    } else if (rest == "serial") {
      transport = SOURCE_SERIAL;
    } else {
      return "Invalid transport. Use: serial, display, or bluetooth";
    }
  }

  logoutTransport(transport);
  static char buf[64];
  snprintf(buf, sizeof(buf), "Logged out from %s", rest.length() > 0 ? rest.c_str() : "serial");
  return buf;
}

const char* cmd_session_revoke(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsIn;
  args.trim();
  String argsLower = args;
  argsLower.toLowerCase();

  auto defaultReason = String("Your session has been signed out by an administrator.");

  int revoked = 0;

  if (argsLower.startsWith("sid ")) {
    // Extract SID and optional reason
    String rest = args.substring(4);  // after "sid "
    rest.trim();
    int sp = rest.indexOf(' ');
    String sid = (sp < 0) ? rest : rest.substring(0, sp);
    String reason = (sp < 0) ? String() : rest.substring(sp + 1);
    reason.trim();
    if (!reason.length()) reason = defaultReason;
    int idx = findSessionIndexBySID(sid);
    if (idx < 0) return "Session not found for given SID.";
    if (gSessions[idx].ip.length() > 0) {
      storeLogoutReason(gSessions[idx].ip, reason);
    }
    enqueueTargetedRevokeForSessionIdx(idx, reason);
    // Admin audit broadcast
    {
      String who = gSessions[idx].user.length() ? gSessions[idx].user : String("(unknown)");
      if (ensureDebugBuffer()) {
        snprintf(getDebugBuffer(), 1024, "Admin audit: revoked session by SID for user '%s' reason='%s'", who.c_str(), reason.c_str());
        broadcastOutput(getDebugBuffer());
      }
    }
    if (!ensureDebugBuffer()) return "Revoked 1 session";
    snprintf(getDebugBuffer(), 1024, "Revoked 1 session (sid=%s)", sid.c_str());
    return getDebugBuffer();
  }

  if (argsLower.startsWith("user ")) {
    // Extract username and optional reason
    String rest = args.substring(5);  // after "user "
    rest.trim();
    int sp = rest.indexOf(' ');
    String username = (sp < 0) ? rest : rest.substring(0, sp);
    String reason = (sp < 0) ? String() : rest.substring(sp + 1);
    reason.trim();
    if (!reason.length()) reason = defaultReason;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
      if (!gSessions[i].sid.length()) continue;
      if (!gSessions[i].user.equalsIgnoreCase(username)) continue;
      if (gSessions[i].ip.length() > 0) {
        storeLogoutReason(gSessions[i].ip, reason);
      }
      enqueueTargetedRevokeForSessionIdx(i, reason);
      revoked++;
    }
    if (revoked > 0) {
      // Admin audit broadcast
      if (ensureDebugBuffer()) {
        snprintf(getDebugBuffer(), 1024, "Admin audit: revoked %d session(s) for user '%s' reason='%s'", revoked, username.c_str(), reason.c_str());
        broadcastOutput(getDebugBuffer());
      }
    }
    if (revoked == 0) {
      if (!ensureDebugBuffer()) return "No active sessions found";
      snprintf(getDebugBuffer(), 1024, "No active sessions found for user '%s'.", username.c_str());
      return getDebugBuffer();
    }
    if (!ensureDebugBuffer()) return "Revoked";
    snprintf(getDebugBuffer(), 1024, "Revoked %d session(s) for user '%s'.", revoked, username.c_str());
    return getDebugBuffer();
  }

  return "Usage:\n"
         "  session revoke sid <sid> [reason]\n"
         "  session revoke user <username> [reason]";
}
#else
// Stub implementations when HTTP server is disabled
const char* cmd_session_list(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Session management requires HTTP server to be enabled";
}
const char* cmd_session_revoke(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Session management requires HTTP server to be enabled";
}
#endif // ENABLE_HTTP_SERVER

const char* cmd_user_request(const String& args) {
  //RETURN_VALID_IF_VALIDATE_CSTR();
  broadcastOutput("[DEBUG] NEW cmd_user_request function called");
  if (!filesystemReady) return "Error: LittleFS not ready";
  // Syntax: args = "<username> <password> [confirmPassword]"
  String rest = args;
  rest.trim();
  if (rest.length() == 0) return "Usage: user request <username> <password> [confirmPassword]";
  int spU = rest.indexOf(' ');
  if (spU < 0) return "Usage: user request <username> <password> [confirmPassword]";
  String username = rest.substring(0, spU);
  username.trim();
  String rem = rest.substring(spU + 1);
  rem.trim();
  int spP = rem.indexOf(' ');
  String password = (spP >= 0) ? rem.substring(0, spP) : rem;
  password.trim();
  String confirm = (spP >= 0) ? rem.substring(spP + 1) : String();
  confirm.trim();
  if (username.length() == 0 || password.length() == 0) return "Error: username and password required";
  if (confirm.length() && confirm != password) return "Error: passwords do not match";
  // Add to pending_users.json in JSON format
  DEBUG_CMD_FLOWF("[users] Adding user to pending_users.json, filesystemReady=%d", filesystemReady ? 1 : 0);

  String json = "[]";
  if (LittleFS.exists("/system/pending_users.json")) {
    if (!readText("/system/pending_users.json", json)) {
      DEBUG_CMD_FLOWF("[users] ERROR: Failed to read existing /system/pending_users.json");
      return "Error: could not read pending list";
    }
  }

  // Parse existing JSON array or create new one
  if (json.length() < 2 || !json.startsWith("[")) json = "[]";

  // Create new user entry with hashed password
  String hashedPassword = hashUserPassword(password);
  String userEntry = "{\"username\":\"" + username + "\",\"password\":\"" + hashedPassword + "\",\"timestamp\":" + String(millis()) + "}";

  // Insert into array
  if (json == "[]") {
    json = "[" + userEntry + "]";
  } else {
    // Insert before closing bracket
    int lastBracket = json.lastIndexOf(']');
    if (lastBracket > 0) {
      String insert = json.substring(1, lastBracket).length() > 0 ? "," + userEntry : userEntry;
      json = json.substring(0, lastBracket) + insert + "]";
    }
  }

  // Attempt atomic write with debug details
  DEBUG_USERSF("[users] Attempting to write /system/pending_users.json (%d bytes)", (int)json.length());
  bool okWrite = writeText("/system/pending_users.json", json);
  if (!okWrite) {
    ERROR_STORAGEF("writeText failed when writing pending_users.json");
    broadcastOutput("[users] ERROR: writeText failed for /system/pending_users.json");
    return "Error: could not write pending list";
  }
  size_t fsz = 0;
  File dbgFile = LittleFS.open("/system/pending_users.json", "r");
  if (dbgFile) {
    fsz = dbgFile.size();
    dbgFile.close();
  }
  DEBUG_USERSF("[users] writeText success; file size=%d bytes", (int)fsz);

  DEBUG_CMD_FLOWF("[users] CLI request username=%s", username.c_str());
  BROADCAST_PRINTF("[register] New user request: %s", username.c_str());

  if (!ensureDebugBuffer()) return "Request submitted (buffer unavailable)";
  snprintf(getDebugBuffer(), 1024, "Request submitted for '%s' (JSON)", username.c_str());
  return getDebugBuffer();
}

// ============================================================================
// Command Registry
// ============================================================================

// CommandEntry struct is defined in system_utils.h (included via user_system.h)
// Note: userCommands array removed - use userSystemCommands instead

// ============================================================================
// User Filesystem Operations - MIGRATED from main .ino
// ============================================================================

// File path (exported for use by other modules)
const char* USERS_JSON_FILE = "/system/users/users.json";

// External dependencies for timestamp resolution
extern uint32_t gBootSeq;
extern uint32_t gBootCounter;
extern bool gTimeSyncedMarkerWritten;
extern void timeSyncUpdateBootEpoch();
extern void getTimestampPrefixMsCached(char* buf, size_t bufSize);
extern bool appendLineWithCap(const char* path, const String& line, size_t cap);

// Boot anchor structure - represents an NTP sync point
struct BootAnchor {
  uint32_t bootSeq;
  time_t epochAtSync;
  unsigned long millisAtSync;

  BootAnchor()
    : bootSeq(0), epochAtSync(0), millisAtSync(0) {}
  BootAnchor(uint32_t seq, time_t epoch, unsigned long ms)
    : bootSeq(seq), epochAtSync(epoch), millisAtSync(ms) {}
};

// User timestamp info - extracted from JSON for resolution
struct UserTimestampInfo {
  int jsonStartPos;
  int jsonEndPos;
  uint32_t bootSeq;
  unsigned long createdMs;
  int bootCount;
  bool needsResolution;

  UserTimestampInfo()
    : jsonStartPos(-1), jsonEndPos(-1),
      bootSeq(0), createdMs(0),
      bootCount(-1), needsResolution(false) {}
};

// Helper: Check if username exists in users.json content
bool usernameExistsInUsersJson(const String& json, const String& username) {
  String needle = String("\"username\": \"") + username + "\"";
  return json.indexOf(needle) >= 0;
}

// Helper: Load users from file (for first-time setup)
bool loadUsersFromFile(String& outUser, String& outPass) {
  if (!filesystemReady) return false;
  
  String usersJson;
  if (!readText(USERS_JSON_FILE, usersJson)) return false;
  
  // Simple parsing - find first user entry
  int userStart = usersJson.indexOf("\"username\":");
  if (userStart < 0) return false;
  
  int usernameStart = usersJson.indexOf("\"", userStart + 11);
  int usernameEnd = usersJson.indexOf("\"", usernameStart + 1);
  if (usernameStart < 0 || usernameEnd < 0) return false;
  
  outUser = usersJson.substring(usernameStart + 1, usernameEnd);
  
  // Find password hash
  int passStart = usersJson.indexOf("\"passwordHash\":", userStart);
  if (passStart < 0) return false;
  
  int passValueStart = usersJson.indexOf("\"", passStart + 15);
  int passValueEnd = usersJson.indexOf("\"", passValueStart + 1);
  if (passValueStart < 0 || passValueEnd < 0) return false;
  
  outPass = usersJson.substring(passValueStart + 1, passValueEnd);
  
  return true;
}

// Forward declarations for helper functions
static int parseBootAnchors(const String& usersJson, BootAnchor* anchors, int maxCount);
static bool parseUserTimestampInfo(const String& userObj, int userStart, UserTimestampInfo& info);
static BootAnchor* findMatchingAnchor(BootAnchor* anchors, int count, uint32_t bootSeq);
static bool replaceJsonField(String& json, const char* fieldName, const String& newValue, int searchStart);
static bool resolveUserTimestamp(String& usersJson, const UserTimestampInfo& info, const BootAnchor& anchor);
static bool approximateUserTimestamp(String& usersJson, const UserTimestampInfo& info, uint32_t ordinalNumber);
static bool extractJsonInt(const String& json, const char* fieldName, int& outValue, int searchStart);
static void buildOrdinal(uint32_t n, char* buf, size_t bufSize);
static bool formatEpochAsISO8601(time_t epoch, char* buf, size_t bufSize);

// Extract integer field from JSON substring
static bool extractJsonInt(const String& json, const char* fieldName,
                           int& outValue, int searchStart = 0) {
  String needle = String("\"") + fieldName + "\":";
  int idx = json.indexOf(needle, searchStart);
  if (idx < 0) return false;

  int valueStart = idx + needle.length();
  while (valueStart < json.length() && (json[valueStart] == ' ' || json[valueStart] == '\t')) {
    valueStart++;
  }

  int valueEnd = valueStart;
  bool isNegative = (json[valueStart] == '-');
  if (isNegative) valueEnd++;

  while (valueEnd < json.length() && json[valueEnd] >= '0' && json[valueEnd] <= '9') {
    valueEnd++;
  }

  if (valueEnd == valueStart || (isNegative && valueEnd == valueStart + 1)) {
    return false;
  }

  outValue = json.substring(valueStart, valueEnd).toInt();
  return true;
}

// Build ordinal string (1st, 2nd, 3rd, etc.)
static void buildOrdinal(uint32_t n, char* buf, size_t bufSize) {
  if (!buf || bufSize < 6) return;

  const char* suffix;
  uint32_t mod100 = n % 100;
  if (mod100 >= 11 && mod100 <= 13) {
    suffix = "th";
  } else {
    switch (n % 10) {
      case 1: suffix = "st"; break;
      case 2: suffix = "nd"; break;
      case 3: suffix = "rd"; break;
      default: suffix = "th"; break;
    }
  }
  snprintf(buf, bufSize, "%lu%s", (unsigned long)n, suffix);
}

// Format epoch as ISO 8601 string
static bool formatEpochAsISO8601(time_t epoch, char* buf, size_t bufSize) {
  if (!buf || bufSize < 21) return false;
  if (epoch <= 0) {
    strncpy(buf, "null", bufSize);
    return false;
  }
  struct tm tminfo;
  if (!gmtime_r(&epoch, &tminfo)) {
    strncpy(buf, "null", bufSize);
    return false;
  }
  if (tminfo.tm_year < 120) {
    strncpy(buf, "null", bufSize);
    return false;
  }

  strftime(buf, bufSize, "%Y-%m-%dT%H:%M:%SZ", &tminfo);
  return true;
}

// Parse boot anchors from users.json
static int parseBootAnchors(const String& usersJson, BootAnchor* anchors, int maxCount) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, usersJson);
  if (error) return 0;

  JsonArray bootAnchorsArray = doc["bootAnchors"];
  if (!bootAnchorsArray) return 0;

  int count = 0;
  for (JsonObject anchor : bootAnchorsArray) {
    if (count >= maxCount) break;
    
    int bootSeq = anchor["bootSeq"] | 0;
    int epochAtSync = anchor["epochAtSync"] | 0;
    int millisAtSync = anchor["millisAtSync"] | 0;
    
    if (bootSeq > 0 && epochAtSync > 0) {
      anchors[count++] = BootAnchor(bootSeq, epochAtSync, millisAtSync);
    }
  }

  return count;
}

// Parse user timestamp info from user object
static bool parseUserTimestampInfo(const String& userObj, int userStart,
                                   UserTimestampInfo& info) {
  info.jsonStartPos = userStart;
  // Check for null createdAt - handle both compact ("createdAt":null) and pretty ("createdAt": null) JSON
  info.needsResolution = (userObj.indexOf("\"createdAt\":null") >= 0) || 
                         (userObj.indexOf("\"createdAt\": null") >= 0);

  if (!info.needsResolution) return false;

  int bootSeq, msSinceBoot, bootCounter;
  if (!extractJsonInt(userObj, "bootSeq", bootSeq) || !extractJsonInt(userObj, "createdMs", msSinceBoot)) {
    return false;
  }

  info.bootSeq = bootSeq;
  info.createdMs = msSinceBoot;

  if (extractJsonInt(userObj, "bootCount", bootCounter)) {
    info.bootCount = bootCounter;
  }

  return true;
}

// Find matching anchor for boot sequence
static BootAnchor* findMatchingAnchor(BootAnchor* anchors, int count, uint32_t bootSeq) {
  for (int i = 0; i < count; i++) {
    if (anchors[i].bootSeq == bootSeq) {
      return &anchors[i];
    }
  }
  return nullptr;
}

// Replace JSON field value in string
static bool replaceJsonField(String& json, const char* fieldName,
                             const String& newValue, int searchStart = 0) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\":", fieldName);
  int needleLen = strlen(needle);

  const char* jsonStr = json.c_str();
  const char* found = strstr(jsonStr + searchStart, needle);
  if (!found) return false;

  int idx = found - jsonStr;
  int valueStart = idx + needleLen;

  while (valueStart < json.length() && (json[valueStart] == ' ' || json[valueStart] == '\t')) {
    valueStart++;
  }

  int valueEnd = valueStart;
  if (json[valueStart] == '"') {
    valueEnd++;
    while (valueEnd < json.length() && json[valueEnd] != '"') {
      if (json[valueEnd] == '\\') valueEnd++;
      valueEnd++;
    }
    valueEnd++;
  } else if (json[valueStart] == '[' || json[valueStart] == '{') {
    char open = json[valueStart];
    char close = (open == '[') ? ']' : '}';
    int depth = 1;
    valueEnd++;
    while (valueEnd < json.length() && depth > 0) {
      if (json[valueEnd] == open) depth++;
      else if (json[valueEnd] == close) depth--;
      valueEnd++;
    }
  } else {
    while (valueEnd < json.length() && json[valueEnd] != ',' && json[valueEnd] != '}' && json[valueEnd] != ']' && json[valueEnd] != '\n') {
      valueEnd++;
    }
  }

  int oldValueLen = valueEnd - valueStart;
  int newLen = json.length() - oldValueLen + newValue.length();

  String result;
  result.reserve(newLen + 16);
  result = json.substring(0, valueStart);
  result += newValue;
  result += json.substring(valueEnd);
  json = result;

  return true;
}

// Resolve user timestamp using NTP anchor
static bool resolveUserTimestamp(String& usersJson, const UserTimestampInfo& info,
                                 const BootAnchor& anchor) {
  long delta = (long)anchor.millisAtSync - (long)info.createdMs;
  time_t createdAtUtc = anchor.epochAtSync - (delta / 1000);

  if (createdAtUtc < 1577836800) return false;
  time_t now = time(nullptr);
  if (now > 0 && createdAtUtc > now + 60) return false;

  char isoTimestamp[24];
  if (!formatEpochAsISO8601(createdAtUtc, isoTimestamp, sizeof(isoTimestamp))) {
    return false;
  }

  char quotedValue[32];
  snprintf(quotedValue, sizeof(quotedValue), "\"%s\"", isoTimestamp);

  if (!replaceJsonField(usersJson, "createdAt", String(quotedValue), info.jsonStartPos)) {
    return false;
  }

  replaceJsonField(usersJson, "createdBy", "\"ntp_resolved\"", info.jsonStartPos);
  return true;
}

// Approximate user timestamp using power cycle label
static bool approximateUserTimestamp(String& usersJson, const UserTimestampInfo& info,
                                     uint32_t ordinalNumber) {
  char ordinal[16];
  buildOrdinal(ordinalNumber, ordinal, sizeof(ordinal));

  char approxLabel[48];
  char quotedValue[56];
  snprintf(approxLabel, sizeof(approxLabel), "%s Power Cycle", ordinal);
  snprintf(quotedValue, sizeof(quotedValue), "\"%s\"", approxLabel);

  if (!replaceJsonField(usersJson, "createdAt", String(quotedValue), info.jsonStartPos)) {
    return false;
  }

  replaceJsonField(usersJson, "createdBy", "\"approx_power_cycle\"", info.jsonStartPos);
  return true;
}

// Cleanup old boot anchors (keep only most recent)
void cleanupOldBootAnchors(void* docPtr) {
  if (!filesystemReady || !LittleFS.exists(USERS_JSON_FILE)) return;

  JsonDocument localDoc;
  JsonDocument* workingDoc = static_cast<JsonDocument*>(docPtr);
  
  if (!workingDoc) {
    static char* cleanupBuf = nullptr;
    static const size_t CLEANUP_BUF_SIZE = 8192;
    if (!cleanupBuf) {
      cleanupBuf = (char*)ps_alloc(CLEANUP_BUF_SIZE, AllocPref::PreferPSRAM, "cleanup.json.buf");
      if (!cleanupBuf) return;
    }

    size_t bytesRead = 0;
    {
      File f = LittleFS.open(USERS_JSON_FILE, "r");
      if (!f) return;
      bytesRead = f.readBytes(cleanupBuf, CLEANUP_BUF_SIZE - 1);
      cleanupBuf[bytesRead] = '\0';
      f.close();
    }

    if (bytesRead == 0) return;

    DeserializationError error = deserializeJson(localDoc, cleanupBuf);
    if (error) return;
    workingDoc = &localDoc;
  }

  JsonArray usersArray = (*workingDoc)["users"];
  if (usersArray) {
    for (JsonObject user : usersArray) {
      const char* createdBy = user["createdBy"] | "";
      if (strcmp(createdBy, "provisional") == 0) {
        return;
      }
    }
  }

  JsonArray bootAnchorsArray = (*workingDoc)["bootAnchors"];
  if (!bootAnchorsArray || bootAnchorsArray.size() == 0) return;

  uint32_t maxBootSeq = 0;
  JsonObject maxAnchor;
  
  for (JsonObject anchor : bootAnchorsArray) {
    uint32_t bootSeq = anchor["bootSeq"] | 0;
    if (bootSeq > maxBootSeq) {
      maxBootSeq = bootSeq;
      maxAnchor = anchor;
    }
  }

  if (maxBootSeq > 0 && !maxAnchor.isNull()) {
    uint32_t bootSeqVal = maxAnchor["bootSeq"] | 0;
    uint32_t epochVal = maxAnchor["epochAtSync"] | 0;
    uint32_t millisVal = maxAnchor["millisAtSync"] | 0;
    
    bootAnchorsArray.clear();
    JsonObject newAnchor = bootAnchorsArray.add<JsonObject>();
    newAnchor["bootSeq"] = (uint32_t)bootSeqVal;
    newAnchor["epochAtSync"] = (uint32_t)epochVal;
    newAnchor["millisAtSync"] = (uint32_t)millisVal;

    File file = LittleFS.open(USERS_JSON_FILE, "w");
    if (file) {
      serializeJson(*workingDoc, file);
      file.close();
    }
  }
}

// Resolve pending user creation timestamps
void resolvePendingUserCreationTimes() {
  DEBUG_USERSF("[resolve] Starting timestamp resolution");
  
  if (!filesystemReady || !LittleFS.exists(USERS_JSON_FILE)) {
    DEBUG_USERSF("[resolve] Skipping - FS not ready or file missing");
    return;
  }

  static char* usersJsonBuf = nullptr;
  static const size_t USERS_JSON_BUF_SIZE = 8192;
  if (!usersJsonBuf) {
    usersJsonBuf = (char*)ps_alloc(USERS_JSON_BUF_SIZE, AllocPref::PreferPSRAM, "users.json.buf");
    if (!usersJsonBuf) return;
  }

  size_t bytesRead = 0;
  {
    File f = LittleFS.open(USERS_JSON_FILE, "r");
    if (!f) return;
    bytesRead = f.readBytes(usersJsonBuf, USERS_JSON_BUF_SIZE - 1);
    usersJsonBuf[bytesRead] = '\0';
    f.close();
  }

  if (bytesRead == 0) return;

  String usersJson = usersJsonBuf;
  DEBUG_USERSF("[resolve] Read %d bytes from users.json", (int)bytesRead);

  const int MAX_ANCHORS = 16;
  BootAnchor anchors[MAX_ANCHORS];
  int anchorCount = parseBootAnchors(usersJson, anchors, MAX_ANCHORS);
  DEBUG_USERSF("[resolve] Found %d boot anchors", anchorCount);
  
  for (int i = 0; i < anchorCount; i++) {
    DEBUG_USERSF("[resolve] Anchor %d: bootSeq=%lu epochAtSync=%u millisAtSync=%lu",
                 i, (unsigned long)anchors[i].bootSeq, (unsigned)anchors[i].epochAtSync, (unsigned long)anchors[i].millisAtSync);
  }

  // Find the "users" array in the JSON - start searching after "users":
  int usersArrayStart = usersJson.indexOf("\"users\"");
  if (usersArrayStart < 0) {
    DEBUG_USERSF("[resolve] No 'users' array found");
    return;
  }
  int arrayBracket = usersJson.indexOf('[', usersArrayStart);
  if (arrayBracket < 0) {
    DEBUG_USERSF("[resolve] No '[' found after 'users'");
    return;
  }

  bool modified = false;
  int userPos = arrayBracket + 1;  // Start after the '[' of the users array

  while (true) {
    int userStart = usersJson.indexOf('{', userPos);
    if (userStart < 0) break;

    // Find matching closing brace (handle nested objects)
    int depth = 1;
    int userEnd = userStart + 1;
    while (userEnd < usersJson.length() && depth > 0) {
      if (usersJson[userEnd] == '{') depth++;
      else if (usersJson[userEnd] == '}') depth--;
      userEnd++;
    }
    userEnd--;  // Point to the closing brace

    if (depth != 0) break;

    String userObj = usersJson.substring(userStart, userEnd + 1);
    DEBUG_USERSF("[resolve] Checking user object at pos %d-%d", userStart, userEnd);

    UserTimestampInfo info;
    if (!parseUserTimestampInfo(userObj, userStart, info)) {
      DEBUG_USERSF("[resolve] User doesn't need resolution (createdAt not null or missing fields)");
      userPos = userEnd + 1;
      continue;
    }

    DEBUG_USERSF("[resolve] User needs resolution: bootSeq=%lu createdMs=%lu bootCount=%d",
                 (unsigned long)info.bootSeq, (unsigned long)info.createdMs, info.bootCount);

    BootAnchor* anchor = findMatchingAnchor(anchors, anchorCount, info.bootSeq);

    if (anchor) {
      DEBUG_USERSF("[resolve] Found matching anchor for bootSeq=%lu", (unsigned long)info.bootSeq);
      if (resolveUserTimestamp(usersJson, info, *anchor)) {
        INFO_SESSIONF("Successfully resolved timestamp");
        modified = true;
      } else {
        WARN_SESSIONF("Failed to resolve timestamp");
      }
    } else {
      DEBUG_USERSF("[resolve] No matching anchor for bootSeq=%lu", (unsigned long)info.bootSeq);
      bool shouldApprox = false;
      uint32_t ordinalN = info.bootSeq;

      if (info.bootCount > 0 && gBootCounter > 0) {
        if ((uint32_t)info.bootCount < gBootCounter) {
          shouldApprox = true;
          ordinalN = (uint32_t)info.bootCount;
        }
      } else if (info.bootSeq < gBootSeq) {
        shouldApprox = true;
      }

      if (shouldApprox) {
        DEBUG_USERSF("[resolve] Approximating timestamp with ordinal %lu", (unsigned long)ordinalN);
        if (approximateUserTimestamp(usersJson, info, ordinalN)) {
          modified = true;
        }
      }
    }

    userPos = userEnd + 1;
  }

  if (modified) {
    DEBUG_USERSF("[resolve] Writing modified users.json");
    if (writeText(USERS_JSON_FILE, usersJson)) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, usersJson);
      if (!error) {
        cleanupOldBootAnchors(&doc);
      } else {
        cleanupOldBootAnchors();
      }
    }
  } else {
    DEBUG_USERSF("[resolve] No modifications needed");
  }
}

// Write boot anchor for user creation timestamp resolution
void writeBootAnchor() {
  time_t now = time(nullptr);
  if (now <= 0 || gBootSeq == 0) return;
  if (!filesystemReady || !LittleFS.exists(USERS_JSON_FILE)) return;

  unsigned long currentMillis = millis();

  String usersJson;
  if (!readText(USERS_JSON_FILE, usersJson)) return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, usersJson);
  if (error) return;

  JsonArray bootAnchorsArray = doc["bootAnchors"].to<JsonArray>();

  int count = bootAnchorsArray.size();
  if (count >= 16) {
    bootAnchorsArray.remove(0);
  }

  JsonObject newAnchor = bootAnchorsArray.add<JsonObject>();
  newAnchor["bootSeq"] = (uint32_t)gBootSeq;
  newAnchor["epochAtSync"] = (uint32_t)now;
  newAnchor["millisAtSync"] = (uint32_t)currentMillis;

  String tempFile = String(USERS_JSON_FILE) + ".tmp";
  File file = LittleFS.open(tempFile.c_str(), "w");
  if (!file) return;
  
  size_t written = serializeJson(doc, file);
  file.close();
  
  if (written > 0) {
    LittleFS.remove(USERS_JSON_FILE);
    LittleFS.rename(tempFile.c_str(), USERS_JSON_FILE);
  }
}

// ============================================================================
// User System Command Registry (System-Specific)
// ============================================================================

const CommandEntry userSystemCommands[] = {
  // Authentication commands
  { "login", "Login to transport: login <user> <pass> [serial|display|bluetooth]", false, cmd_login, "Usage: login <username> <password> [transport]\nTransport: serial (default), display, bluetooth" },
  { "logout", "Logout from transport: logout [serial|display|bluetooth]", false, cmd_logout },
  
  // User management commands
  { "user approve", "Approve pending user request.", true, cmd_user_approve },
  { "user deny", "Deny pending user request.", true, cmd_user_deny },
  { "user promote", "Promote user to admin.", true, cmd_user_promote, "Usage: user promote <username>" },
  { "user demote", "Demote admin to user.", true, cmd_user_demote, "Usage: user demote <username>" },
  { "user delete", "Delete user account.", true, cmd_user_delete, "Usage: user delete <username>" },
  { "user list", "List all users.", true, cmd_user_list },
  { "user request", "Request new user account.", false, cmd_user_request, "Usage: user request <username> <password> [confirmPassword]" },
  { "user sync", "Sync user to ESP-NOW device.", true, cmd_user_sync },
  
  // Session management commands
  { "pending list", "List pending user requests.", true, cmd_pending_list },
  { "session list", "List active sessions.", true, cmd_session_list },
  { "session revoke", "Revoke user session.", true, cmd_session_revoke, "Usage:\n  session revoke sid <sid> [reason]\n  session revoke user <username> [reason]" }
};

const size_t userSystemCommandsCount = sizeof(userSystemCommands) / sizeof(userSystemCommands[0]);

// ============================================================================
// Command Registration (System-Specific)
// ============================================================================
// Direct static registration to avoid macro issues
static CommandModuleRegistrar _user_cmd_registrar(userSystemCommands, userSystemCommandsCount, "users");

// ============================================================================
// Boot Sequence Management
// ============================================================================

// Increment boot sequence counter (memory-only, resets on power cycle)
void loadAndIncrementBootSeq() {
  // Boot sequence is now memory-only and stored in bootAnchors in users.json
  // We derive it from the highest bootSeq in existing anchors
  gBootSeq = 0;
  gBootCounter = 0;
  // Temporarily enable DEBUG_SYSTEM for boot sequence initialization (runs before settings loaded)
  uint32_t _dbgSaved = getDebugFlags();
  setDebugFlag(DEBUG_SYSTEM);
  DEBUG_SYSTEMF("BootSeqInit: filesystemReady=%d, users.json exists=%d", (int)filesystemReady, (int)(filesystemReady && LittleFS.exists(USERS_JSON_FILE)));

  if (filesystemReady && LittleFS.exists(USERS_JSON_FILE)) {
    File file = LittleFS.open(USERS_JSON_FILE, "r");
    if (!file) {
      ERROR_SYSTEMF("BootSeqInit: Failed to open users.json");
    } else {
      // Parse with ArduinoJson
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, file);
      file.close();
      
      if (error) {
        ERROR_SYSTEMF("BootSeqInit: Failed to parse users.json");
      } else {
        DEBUG_SYSTEMF("BootSeqInit: Loaded and parsed users.json");
        
        // Find highest bootSeq in bootAnchors array
        JsonArray bootAnchorsArray = doc["bootAnchors"];
        if (bootAnchorsArray) {
          for (JsonObject anchor : bootAnchorsArray) {
            uint32_t seq = anchor["bootSeq"] | 0;
            if (seq > gBootSeq) {
              gBootSeq = seq;
            }
          }
          DEBUG_SYSTEMF("BootSeqInit: Highest bootSeq in anchors=%lu", (unsigned long)gBootSeq);
        }

        // Parse bootCounter if present
        gBootCounter = doc["bootCounter"] | 0;
        DEBUG_SYSTEMF("BootSeqInit: Parsed bootCounter=%lu", (unsigned long)gBootCounter);

        // Increment bootCounter and persist back
        uint32_t newCounter = gBootCounter + 1;
        doc["bootCounter"] = newCounter;
        
        DEBUG_SYSTEMF("BootSeqInit: Updating bootCounter -> %lu", (unsigned long)newCounter);

        // Write back to file
        file = LittleFS.open(USERS_JSON_FILE, "w");
        if (file) {
          size_t written = serializeJson(doc, file);
          file.close();
          if (written > 0) {
            gBootCounter = newCounter;
            INFO_SYSTEMF("BootSeqInit: Persisted users.json with bootCounter=%lu", (unsigned long)gBootCounter);
          } else {
            gBootCounter = newCounter;
            WARN_SYSTEMF("BootSeqInit: Write failed; bootCounter advanced in RAM to %lu", (unsigned long)gBootCounter);
          }
        } else {
          // Fallback: still advance in memory
          gBootCounter = newCounter;
          WARN_SYSTEMF("BootSeqInit: Persist failed; bootCounter advanced in RAM to %lu", (unsigned long)gBootCounter);
        }
      }
    }
  }

  gBootSeq++;
  // Restore debug flags
  setDebugFlags(_dbgSaved);
  // Use DEBUG macro - now safe since debug system is initialized
  DEBUG_SYSTEMF("[BOOT] Boot sequence: %lu (derived from bootAnchors)", (unsigned long)gBootSeq);
  DEBUG_SYSTEMF("[BOOT] Boot counter: %lu (stored in users.json)", (unsigned long)gBootCounter);
}

// ============================================================================
// User Sync Command (merged from System_User_Sync.cpp)
// ============================================================================
// ESP-NOW user credential propagation - allows admin to sync users across devices

#if ENABLE_ESPNOW

#include "System_ESPNow.h"

// Helper to build v2 envelope (from System_ESPNow.cpp)
extern void v2_init_envelope(JsonDocument& doc, const char* type, uint32_t id, const char* src, const char* dst, int ttl);
extern uint32_t generateMessageId();
extern bool routerSend(Message& msg);
extern String getEspNowDeviceName(const uint8_t* mac);

// Helper to parse MAC address or device name
static bool userSyncParseMacAddress(const String& s, uint8_t out[6]) {
  if (s.length() == 17 && s[2] == ':' && s[5] == ':') {
    // MAC format: XX:XX:XX:XX:XX:XX
    for (int i = 0; i < 6; i++) {
      char hex[3] = {s[i*3], s[i*3+1], '\0'};
      out[i] = strtol(hex, NULL, 16);
    }
    return true;
  }
  return false;
}

static bool userSyncResolveDeviceNameOrMac(const String& deviceStr, uint8_t outMac[6]) {
  // Try parsing as MAC first
  if (userSyncParseMacAddress(deviceStr, outMac)) {
    return true;
  }
  
  // Try resolving as device name
  extern EspNowState* gEspNow;
  if (!gEspNow) return false;
  
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (gEspNow->devices[i].name.equalsIgnoreCase(deviceStr)) {
      memcpy(outMac, gEspNow->devices[i].mac, 6);
      return true;
    }
  }
  
  return false;
}

/**
 * User sync command: user sync <username> <device> <password>
 * 
 * Sends user credentials to a paired ESP-NOW device.
 * Requires admin privileges and user sync to be enabled.
 */
const char* cmd_user_sync(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  // Check if ESP-NOW is enabled
  extern EspNowState* gEspNow;
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized";
  }
  
  // Check if user sync is enabled
  if (!gSettings.espnowUserSyncEnabled) {
    return "Error: User sync disabled - enable with 'espnow usersync on'";
  }
  
  // Parse command args: <username> <device> <password>
  String args = argsIn;
  args.trim();
  
  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) {
    return "Usage: user sync <username> <device> <password>";
  }
  
  String username = args.substring(0, firstSpace);
  String rest = args.substring(firstSpace + 1);
  rest.trim();
  
  int secondSpace = rest.indexOf(' ');
  if (secondSpace < 0) {
    return "Usage: user sync <username> <device> <password>";
  }
  
  String deviceStr = rest.substring(0, secondSpace);
  String password = rest.substring(secondSpace + 1);
  password.trim();
  
  if (username.length() == 0 || deviceStr.length() == 0 || password.length() == 0) {
    return "Usage: user sync <username> <device> <password>";
  }
  
  // Verify user exists locally
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId)) {
    snprintf(getDebugBuffer(), 1024, "Error: User '%s' not found", username.c_str());
    return getDebugBuffer();
  }
  
  // Get user role
  String role;
  if (!getUserRole(username, role)) {
    role = "user";  // Default if not found
  }
  
  // Resolve device MAC
  uint8_t targetMac[6];
  if (!userSyncResolveDeviceNameOrMac(deviceStr, targetMac)) {
    snprintf(getDebugBuffer(), 1024, "Error: Device '%s' not found in paired devices", deviceStr.c_str());
    return getDebugBuffer();
  }
  
  String deviceName = getEspNowDeviceName(targetMac);
  if (deviceName.length() == 0) {
    deviceName = deviceStr;
  }
  
  // Get admin credentials from current auth context
  extern AuthContext gExecAuthContext;
  String adminUser = gExecAuthContext.user;
  
  if (adminUser.length() == 0) {
    return "Error: Not authenticated - admin login required";
  }
  
  // Verify admin privileges
  if (!isAdminUser(adminUser)) {
    return "Error: Admin privileges required for user sync";
  }
  
  INFO_USERF("[USER_SYNC] Syncing user '%s' (role=%s) to device '%s'", 
             username.c_str(), role.c_str(), deviceName.c_str());
  
  // Build USER_SYNC message
  JsonDocument doc;
  uint32_t msgId = generateMessageId();
  
  // Get device name for source
  String myDeviceName = gSettings.espnowDeviceName;
  if (myDeviceName.length() == 0) {
    myDeviceName = "unknown";
  }
  
  v2_init_envelope(doc, MSG_TYPE_USER_SYNC, msgId, myDeviceName.c_str(), deviceName.c_str(), -1);
  
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["admin_user"] = adminUser;
  payload["admin_pass"] = password;  // Admin provides their own password for auth
  payload["target_user"] = username;
  payload["target_pass"] = password;  // Same password for the user being synced
  payload["role"] = role;
  
  // Serialize to string
  String envelope;
  serializeJson(doc, envelope);
  
  // Send via router
  Message msg;
  memcpy(msg.dstMac, targetMac, 6);
  msg.payload = envelope;
  msg.priority = PRIORITY_HIGH;
  msg.type = MSG_TYPE_COMMAND;
  msg.requiresAck = true;
  msg.msgId = msgId;
  msg.ttl = 3;
  msg.maxRetries = 2;
  
  if (!routerSend(msg)) {
    ERROR_USERF("[USER_SYNC] Failed to send sync message to %s", deviceName.c_str());
    snprintf(getDebugBuffer(), 1024, "Error: Failed to send user sync to '%s'", deviceName.c_str());
    return getDebugBuffer();
  }
  
  INFO_USERF("[USER_SYNC] ✓ Sent user '%s' to device '%s' (msgId=%lu)", 
             username.c_str(), deviceName.c_str(), (unsigned long)msgId);
  
  snprintf(getDebugBuffer(), 1024, "User sync sent: '%s' → '%s' (role=%s)", 
           username.c_str(), deviceName.c_str(), role.c_str());
  return getDebugBuffer();
}

#else
// Stub when ESP-NOW is disabled
const char* cmd_user_sync(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Error: ESP-NOW not enabled";
}
#endif // ENABLE_ESPNOW
