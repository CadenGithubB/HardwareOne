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
#include "System_Command.h"
#include "System_Clock.h"  // Clock::isValidEpoch / isSynced — replaces tm_year>=120 magic
#include "System_BootState.h"  // bootStateIncrementBootCount — boot counter now lives in NVS
#include "System_Mutex.h"  // For FsLockGuard
#include "System_Debug.h"  // For DEBUG_AUTHF, DEBUG_USERF
#include "System_Logging.h" // For log file paths and constants
#include "System_Filesystem.h"    // For writeText, readText
#include "System_VFS.h"    // For VFS::*Guarded + VFS::systemAuth
#include "System_AuthIdentity.h"   // currentAuthContext (createdBy stamp, admin sync)
#include "System_CLIConfirm.h"     // cliRequestConfirm — yes/no gate for destructive userdelete
#include "System_Settings.h"
#include "Bluetooth.h"
#include "BLE_Peers.h"            // gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser
                                  // — G2 identity source for SOURCE_G2_GLASSES branches
                                  // (declarations are #if ENABLE_BLUETOOTH-gated inside)

// ----------------------------------------------------------------------------
// G2 identity accessors — wrap gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser
// behind the BT compile gate so SOURCE_G2_GLASSES switch branches below
// compile cleanly on BT-off builds. On BT-off the lens can't connect anyway,
// so the readers return empty/false and the mutators are no-ops; nothing
// downstream relies on a real value.
// ----------------------------------------------------------------------------
static inline String g2PairedUserGet() {
#if ENABLE_BLUETOOTH
  return gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser;
#else
  return String();
#endif
}
static inline void g2PairedUserClear() {
#if ENABLE_BLUETOOTH
  gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser = String();
#endif
}
static inline bool g2PairedUserMatches(const String& username) {
#if ENABLE_BLUETOOTH
  return gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser.equalsIgnoreCase(username);
#else
  (void)username;
  return false;
#endif
}
#include "OLED_Display.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "System_UserSettings.h"
#include "System_MemUtil.h"
#include <Arduino.h>
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

// ============================================================================
// External Dependencies from .ino
// ============================================================================

// filesystemReady is provided by System_Filesystem.h (included above).

// Session management now in web_server.h (included above)

// ----------------------------------------------------------------------------
// Shared session idle-timeout policy (Step 0). See System_User.h for the
// contract. Pure read of gSettings + millis(); safe to call from any task.
// ----------------------------------------------------------------------------
unsigned long sessionStampNow() {
  unsigned long t = millis();
  return (t == 0) ? 1UL : t;                 // reserve 0 to mean "never stamped"
}

unsigned long sessionIdleWindowMs(CommandSource transport) {
  uint32_t mins = 0;                                    // unknown/unwired → disabled
  switch (transport) {
    case SOURCE_WEB:       mins = gSettings.sessionIdleWeb;    break;
    case SOURCE_SERIAL:    mins = gSettings.sessionIdleSerial; break;
    case SOURCE_BLUETOOTH: mins = gSettings.sessionIdleBle;     break;
    case SOURCE_LOCAL_DISPLAY: mins = gSettings.sessionIdleDisplay; break;
    // SOURCE_ESPNOW (bond) is intentionally excluded: it's a device-to-device
    // trust channel, not a human session, so idle-logout doesn't apply. Any
    // other transport reads as disabled.
    default:               mins = 0;                            break;
  }
  return (mins == 0) ? 0UL : (unsigned long)mins * 60000UL;   // 0 = disabled
}

bool sessionIdleExpired(unsigned long lastInteractionMs, unsigned long nowMs, unsigned long windowMs) {
  if (windowMs == 0)          return false;  // feature off → never expire
  if (lastInteractionMs == 0) return false;  // never stamped → treat as fresh
  // Wrap-safe: the window (<= 24h) is far below the ~49.7-day millis() rollover,
  // and an idle session would have expired long before wrap could matter.
  return (unsigned long)(nowMs - lastInteractionMs) > windowMs;
}

bool sessionIdleExpired(CommandSource transport, unsigned long lastInteractionMs, unsigned long nowMs) {
  return sessionIdleExpired(lastInteractionMs, nowMs, sessionIdleWindowMs(transport));
}

// File paths
#define PENDING_USERS_FILE "/system/users/pending_users.json"

// Memory allocation
// Utility functions from .ino
extern String getDeviceEncryptionKey();
extern String jsonEscape(const String& in);
extern void fsLock(const char* reason);
extern void fsUnlock();
extern void resolvePendingUserCreationTimes();

// Boot tracking globals
extern uint32_t gNTPAnchorId;
extern uint32_t gBootCounter;

// Serial authentication globals
extern bool gSerialAuthed;
extern String gSerialUser;

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
    logAuthAttempt(true, ctx.path.c_str(), ctx.user, ctx.ip, "");
    return true;
  } else if (ctx.transport == SOURCE_SERIAL) {
    // Serial console auth state
    if (gSettings.serialRequireAuth && !gSerialAuthed) {
      broadcastOutput("ERROR: auth required");
      return false;
    }
    ctx.user = gSerialUser;
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    // Local display auth state - check if auth is required via settings
    // Allow commands during boot phase (before auth is enforced)
    if (shouldBlockForDisplayAuth()) {
      broadcastOutput("ERROR: auth required (display)");
      return false;
    }
    ctx.user = gLocalDisplayUser;
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_G2_GLASSES) {
    // G2 lens — auth IS pairing. pairedByUser is the captured identity from
    // pair-time (set by bleStampPairedByIfBlank when an authenticated CLI
    // ran `bleautoconnect g2-glasses on`). Blank means "paired-but-stamp-lost"
    // — refuse the command and tell the user how to recover (re-stamp via
    // bleautoconnect from an authenticated CLI session).
    String g2User = g2PairedUserGet();
    if (g2User.length() == 0) {
      broadcastOutput("ERROR: G2 pairedByUser blank — run 'bleautoconnect g2-glasses on' from authenticated CLI to re-stamp");
      return false;
    }
    ctx.user = g2User;
    if (ctx.ip.length() == 0) ctx.ip = "g2.local";
    return true;
  } else {
    // SOURCE_BLUETOOTH and SOURCE_ESPNOW are expected to validate auth in
    // their command-receive handlers BEFORE submitting the command to
    // executeCommand:
    //   * BLE  — per-conn session token check in Bluetooth.cpp before submit
    //   * ESP-NOW — v3 protocol signed msgId verification before submit
    // SOURCE_MQTT currently has NO such upstream check: MQTT-received
    // commands are administrator-trusted, equivalent to SOURCE_INTERNAL.
    // If/when MQTT is opened to untrusted clients, split this branch and
    // wire MQTT through a proper session/token check.
    // SOURCE_INTERNAL is firmware-generated and trusted by design (used by
    // automation rules, boot sequences, scheduled tasks).
    return true;
  }
}

#else
// Stub implementations when HTTP server is disabled
bool tgRequireAuth(AuthContext& ctx) {
  // Serial auth only when HTTP server disabled
  if (ctx.transport == SOURCE_SERIAL) {
    if (gSettings.serialRequireAuth && !gSerialAuthed) {
      broadcastOutput("ERROR: auth required");
      return false;
    }
    ctx.user = gSerialUser;
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    if (shouldBlockForDisplayAuth()) {
      broadcastOutput("ERROR: auth required (display)");
      return false;
    }
    ctx.user = gLocalDisplayUser;
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_G2_GLASSES) {
    String g2User = g2PairedUserGet();
    if (g2User.length() == 0) {
      broadcastOutput("ERROR: G2 pairedByUser blank — run 'bleautoconnect g2-glasses on' from authenticated CLI to re-stamp");
      return false;
    }
    ctx.user = g2User;
    if (ctx.ip.length() == 0) ctx.ip = "g2.local";
    return true;
  }
  return true; // Internal commands pass through
}
#endif // ENABLE_HTTP_SERVER

// Determine if the given username is admin (any user with role == admin)
bool isAdminUser(const String& who) {
#if ENABLE_BONDED_MODE
  // A bonded master that presented a valid bond session token runs commands under
  // kBondAdminUser. Grant admin for the lifetime of the live bond session only.
  // Checked before the filesystem/JSON path — this identity is never a real
  // users.json account, and ctx.user is only ever set to it by the token-validated
  // bonded-command path (v4_handle_cmd), so the session check is defense-in-depth.
  if (who == kBondAdminUser) {
    extern bool isBondSessionTokenValid();
    return isBondSessionTokenValid();
  }
#endif
  if (!filesystemReady) return false;
  // Prefer JSON
  if (VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.isAdmin"))) {
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
  // serial / display / bluetooth are the credential-login transports that funnel
  // through here (web uses cookie sessions; G2 uses pair-time identity). Audit
  // each attempt once via the shared recordLoginAttempt() front-door.
  const bool credentialTransport = (transport == SOURCE_SERIAL ||
                                    transport == SOURCE_LOCAL_DISPLAY ||
                                    transport == SOURCE_BLUETOOTH);

  // Validate credentials first
  if (!isValidUser(username, password)) {
#if ENABLE_HTTP_SERVER
    if (credentialTransport)
      recordLoginAttempt(transport, username, String(), false, "Invalid credentials");
#endif
    return false;
  }

#if ENABLE_HTTP_SERVER
  if (credentialTransport)
    recordLoginAttempt(transport, username, String(), true, "Login successful");
#endif

  // Set auth state based on transport
  switch (transport) {
    case SOURCE_SERIAL:
      gSerialAuthed = true;
      gSerialUser = username;
      updateUserLastSeen(username);
      return true;

    case SOURCE_LOCAL_DISPLAY:
      gLocalDisplayAuthed = true;
      gLocalDisplayUser = username;
      // Start the idle clock now: login is a real interaction, and without this
      // a user who logs in then walks away would read as never-stamped (0) and
      // never time out. localDisplaySessionTick() refreshes it on later input.
      gLocalDisplayLastInteractionMs = sessionStampNow();
      oledNotifyLocalDisplayAuthChanged();
      updateUserLastSeen(username);
      return true;

    case SOURCE_BLUETOOTH:
      updateUserLastSeen(username);
      return true;
      
    case SOURCE_WEB:
      // Web auth is handled separately via session cookies
      // This function doesn't apply to web transport
      return false;

    case SOURCE_G2_GLASSES:
      // G2 doesn't use credential login. Identity is captured at pair time
      // by bleStampPairedByIfBlank, called from `bleautoconnect g2-glasses on`
      // executed under an already-authenticated CLI session. No code path
      // should be calling loginTransport(SOURCE_G2_GLASSES) — return false
      // so it surfaces clearly if one ever does.
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

    case SOURCE_G2_GLASSES:
      // Clear pair-time stamp. The G2 stays paired (MAC + autoConnect
      // preserved) but commands from the lens will be denied until
      // someone runs `bleautoconnect g2-glasses on` from an authenticated
      // CLI session to re-stamp pairedByUser.
      g2PairedUserClear();
      break;

    case SOURCE_BLUETOOTH:
      bleRevokeAllSessions();
      break;

    case SOURCE_WEB:
      // Web logout is handled separately via session management
      break;
      
    default:
      break;
  }
}

// ============================================================================
// revokeUserSessions — force-logout helper used by user-mutation paths
// ============================================================================
//
// Walks every transport's session state and clears any session belonging to
// `username`. Optionally skips a single web SID (for self-password-change
// where the user just authenticated with the new password and would be
// annoyed to be kicked out of the session they're typing into) and/or skips
// one transport entirely (same reason for serial/oled/bluetooth — those
// transports use a single session-per-transport model, no SID).
//
// Callers:
//   * cmd_user_delete  → revoke everywhere (account is gone)
//   * cmd_user_demote  → revoke everywhere (was admin, now isn't —
//                        existing session was running with admin perms)
//   * cmd_user_changepassword → revoke everywhere EXCEPT the calling
//                        session (self-service: credential rotated,
//                        kick other devices but keep current alive)
//   * cmd_user_resetpassword  → revoke everywhere (admin reset target's
//                        password; admin's own session is a different
//                        user, so no exception needed)
//
// This is intentionally NOT wired to gIdentityGeneration. The clock is for
// permission-topology changes that invalidate auth-derived caches like
// FileManager. Session revocation is a separate concern with its own
// trigger points — calling it from mutators directly is cleaner than
// bumping the clock and having a generic listener trawl sessions on
// every cache-relevant event.
int revokeUserSessions(const String& username,
                       const String& reason,
                       const String& exceptSid,
                       CommandSource exceptTransport) {
  if (username.length() == 0) return 0;
  int revoked = 0;

  // Web: walk gSessions, skip exceptSid if set.
  if (gSessions) {
    for (int i = 0; i < MAX_SESSIONS; ++i) {
      if (!gSessions[i].sid.length()) continue;
      if (!gSessions[i].user.equalsIgnoreCase(username)) continue;
      if (exceptSid.length() > 0 && gSessions[i].sid == exceptSid) continue;
      if (gSessions[i].ip.length() > 0) {
        storeLogoutReason(gSessions[i].ip, reason);
      }
      enqueueTargetedRevokeForSessionIdx(i, reason);
      revoked++;
    }
  }

  // Serial transport: single per-device session, skipped if this is the
  // calling transport (self-modify case).
  if (exceptTransport != SOURCE_SERIAL
      && gSerialAuthed
      && gSerialUser.equalsIgnoreCase(username)) {
    gSerialAuthed = false;
    gSerialUser   = String();
    revoked++;
  }

  // Local display (OLED).
  if (exceptTransport != SOURCE_LOCAL_DISPLAY
      && gLocalDisplayAuthed
      && gLocalDisplayUser.equalsIgnoreCase(username)) {
    gLocalDisplayAuthed = false;
    gLocalDisplayUser   = String();
    oledNotifyLocalDisplayAuthChanged();
    revoked++;
  }

  // G2 glasses (BLE-attached lens; pair-time identity).
  // The pair stays valid (MAC + autoConnect kept) but the lens stops being
  // able to act as this user until re-stamp via `bleautoconnect g2-glasses on`.
  if (exceptTransport != SOURCE_G2_GLASSES && g2PairedUserMatches(username)) {
    g2PairedUserClear();
    revoked++;
  }

  // Bluetooth (its own session table inside the BT module).
  if (exceptTransport != SOURCE_BLUETOOTH) {
    revoked += bleRevokeUserSessions(username);
  }

  if (revoked > 0) {
    WARN_USERF("[SESSION-REVOKE] user='%s' count=%d reason='%s'",
               username.c_str(), revoked, reason.c_str());
  }
  return revoked;
}

bool isTransportAuthenticated(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL:
      return gSerialAuthed;
      
    case SOURCE_LOCAL_DISPLAY:
      return gLocalDisplayAuthed;

    case SOURCE_G2_GLASSES:
      // G2 is "authenticated" iff the lens is paired AND pairedByUser is
      // a non-empty captured username. Pairing without pairedByUser counts
      // as not-authenticated (stuck-stamp state — see G2_HijackCmd.cpp).
      return g2PairedUserGet().length() > 0;

    case SOURCE_BLUETOOTH:
      return bleHasAuthenticatedSession();

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

    case SOURCE_G2_GLASSES:
      // Pair-time stamp. Empty string when not yet stamped / cleared by
      // revoke. Callers (G2 page handlers etc.) get the username string
      // identical to what gLocalDisplayUser would have returned before
      // the OLED/G2 split.
      return g2PairedUserGet();

    case SOURCE_BLUETOOTH:
      {
        uint16_t connId = 0;
        String user;
        if (bleGetAuthenticatedSessionInfo(0, connId, user)) {
          return user;
        }
        return String();
      }

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

  // Use PBKDF2-HMAC-SHA256 for strong password hashing
  String salt = getDeviceEncryptionKey();
  const int iterations = 10000;  // NIST minimum recommendation
  uint8_t hash[32];  // 256-bit output

  int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
    MBEDTLS_MD_SHA256,
    (const uint8_t*)password.c_str(), password.length(),
    (const uint8_t*)salt.c_str(), salt.length(),
    iterations,
    32,  // Output 32 bytes (256 bits)
    hash
  );

  secureClearString(salt);

  if (ret != 0) {
    DEBUG_AUTHF("[PBKDF2] Hash generation failed: %d", ret);
    return "";
  }

  // Encode as hex
  char hashStr[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hashStr + (i * 2), 3, "%02x", hash[i]);
  }
  hashStr[64] = '\0';

  // Format: PBKDF2:iterations:hex
  char hashFmt[80];
  snprintf(hashFmt, sizeof(hashFmt), "PBKDF2:%d:%s", iterations, hashStr);
  return String(hashFmt);
}

bool verifyUserPassword(const String& inputPassword, const String& storedHash) {
  if (inputPassword.length() == 0 || storedHash.length() == 0) return false;

  // Verify PBKDF2 format
  if (!storedHash.startsWith("PBKDF2:")) {
    DEBUG_AUTHF("[PBKDF2] Invalid hash format detected");
    return false;
  }

  // Hash the input password and compare
  String inputHash = hashUserPassword(inputPassword);
  return (inputHash == storedHash);
}

// Update a user's text password in per-user settings file
bool setUserPassword(const String& username, const String& newPasswordRaw, bool requireChangeOnNextLogin) {
  if (!filesystemReady || username.length() == 0 || newPasswordRaw.length() == 0) return false;
  
  // Get user ID from username
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId) || userId == 0) return false;
  
  // Hash the password
  String hashed = hashUserPassword(newPasswordRaw);
  
  // Load existing user settings
  PSRAM_JSON_DOC(settings);
  loadUserSettings(userId, settings);  // OK if doesn't exist yet
  
  // Set the password field
  settings["password"] = hashed;
  settings["mustChangePassword"] = requireChangeOnNextLogin;

  // Save back to user settings file
  return saveUserSettings(userId, settings);
}

bool userMustChangePassword(const String& username) {
  if (!filesystemReady || username.length() == 0) return false;
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId) || userId == 0) return false;
  PSRAM_JSON_DOC(settings);
  if (!loadUserSettings(userId, settings)) return false;
  return settings["mustChangePassword"] == true;
}

bool adminCreateUser(const String& username, const String& plainPassword, bool mustChangeOnLogin,
                     const String& createdBy, String& errorOut) {
  errorOut = "";
  if (!filesystemReady) {
    errorOut = "LittleFS not ready";
    return false;
  }
  String u = username;
  u.trim();
  if (u.length() == 0 || u.length() > 64) {
    errorOut = "Invalid username";
    return false;
  }
  for (size_t i = 0; i < u.length(); ++i) {
    if (u[i] <= ' ') {
      errorOut = "Username may not contain whitespace";
      return false;
    }
  }
  // Reserved names — used by the audit-log/dispatch layer as identity
  // sentinels. Allowing a user to claim one would let their commands be
  // mistaken for system or anonymous-local activity in the audit trail.
  // Case-insensitive so "authbypass" / "AUTHBYPASS" don't sneak through.
  {
    static const char* kReserved[] = { "system", "AuthBypass", kBondAdminUser };
    for (const char* r : kReserved) {
      if (strcasecmp(u.c_str(), r) == 0) {
        errorOut = String("Username \"") + r + "\" is reserved";
        return false;
      }
    }
  }
  if (plainPassword.length() < 6) {
    errorOut = "Password must be at least 6 characters";
    return false;
  }

  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.admin.create"))) {
    errorOut = "users.json not found";
    return false;
  }

  // Reject if already pending registration
  if (VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.admin.create"))) {
    File pf = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.admin.create"));
    if (pf) {
      PSRAM_JSON_DOC(pdoc);
      if (!deserializeJson(pdoc, pf)) {
        JsonArray parr = pdoc.as<JsonArray>();
        if (parr) {
          for (JsonObject pu : parr) {
            const char* pun = pu["username"] | "";
            if (pun && u == pun) {
              pf.close();
              errorOut = "Username already pending approval";
              return false;
            }
          }
        }
      }
      pf.close();
    }
  }

  int nextIdForSettings = 0;
  {
    FsLockGuard guard("users.admin_create");
    File file = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.admin.create"));
    if (!file) {
      errorOut = "Failed to read users.json";
      return false;
    }
    PSRAM_JSON_DOC(doc);
    DeserializationError jerr = deserializeJson(doc, file);
    file.close();
    if (jerr) {
      errorOut = "Malformed users.json";
      return false;
    }
    JsonArray users = doc["users"];
    if (!users) {
      errorOut = "Malformed users.json - missing users array";
      return false;
    }
    for (JsonObject user : users) {
      const char* existingUsername = user["username"];
      if (existingUsername && u == existingUsername) {
        errorOut = "Username already exists";
        return false;
      }
    }

    int nextId = doc["nextId"] | 2;
    JsonObject newUser = users.add<JsonObject>();
    newUser["id"] = nextId;
    newUser["username"] = u;
    newUser["role"] = "user";
    newUser["createdAt"] = (const char*)nullptr;
    newUser["createdBy"] = createdBy.length() ? createdBy.c_str() : "admin";
    newUser["createdAtSource"] = "pending";  // createdAt resolved lazily via boot anchor
    newUser["createdMs"] = millis();
    newUser["ntpAnchorId"] = gNTPAnchorId;
    newUser["bootCount"] = gBootCounter;
    doc["nextId"] = nextId + 1;

    {
      // Atomic write (tmp + rename) — never truncate the live auth DB in place.
      String json;
      serializeJson(doc, json);
      if (json.length() == 0 || !writeTextAtomic(USERS_JSON_FILE, json)) {
        errorOut = "Failed to write users.json";
        return false;
      }
    }
    nextIdForSettings = nextId;
  }

  const uint32_t createdUserId = (uint32_t)nextIdForSettings;
  {
    FsLockGuard sguard("user_settings.admin_create");
    PSRAM_JSON_DOC(defaults);
    defaults["theme"] = "light";
    defaults["password"] = hashUserPassword(plainPassword);
    defaults["mustChangePassword"] = mustChangeOnLogin;
    if (!saveUserSettings(createdUserId, defaults)) {
      errorOut = "User created but failed to write settings file";
      return false;
    }
  }

  // Identity topology changed (a new account is now usable). Invalidate
  // any auth-dependent caches — see System_AuthIdentity.h for the protocol.
  bumpIdentityGeneration("user.add");
  DEBUG_USERSF("[users] admin created user '%s' id=%u mustChange=%d", u.c_str(), (unsigned)createdUserId,
               mustChangeOnLogin ? 1 : 0);
  return true;
}

// Update a user's gamepad pattern password in per-user settings file
bool setUserGamepadPassword(const String& username, const String& newPatternRaw) {
  if (!filesystemReady || username.length() == 0 || newPatternRaw.length() == 0) return false;
  
  // Get user ID from username
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId) || userId == 0) return false;
  
  // Hash the pattern (same as text password)
  String hashed = hashUserPassword(newPatternRaw);
  
  // Load existing user settings
  PSRAM_JSON_DOC(settings);
  loadUserSettings(userId, settings);  // OK if doesn't exist yet
  
  // Set the gamepad password field
  settings["gamepad_password"] = hashed;
  
  // Save back to user settings file
  return saveUserSettings(userId, settings);
}

// Check if a user has a gamepad password set (in per-user settings file)
bool hasUserGamepadPassword(const String& username) {
  if (!filesystemReady || username.length() == 0) return false;
  
  // Get user ID from username
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId) || userId == 0) return false;
  
  // Load user settings
  PSRAM_JSON_DOC(settings);
  if (!loadUserSettings(userId, settings)) return false;
  
  // Check if gamepad_password field exists and is non-empty
  const char* gamepadPass = settings["gamepad_password"];
  return (gamepadPass && strlen(gamepadPass) > 0);
}

// ============================================================================
// User Account Ban
// ============================================================================

// Returns true if the given username has "banned": true in users.json.
bool isUserBanned(const String& username) {
  if (!filesystemReady || username.length() == 0) return false;
  FsLockGuard guard("users.is_banned");
  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.isBanned"))) return false;
  File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.isBanned"));
  if (!f) return false;
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  for (JsonObject u : doc["users"].as<JsonArray>()) {
    const char* uname = u["username"] | "";
    if (username == uname) return u["banned"] | false;
  }
  return false;
}

// Sets (or clears) the "banned" flag on a user entry in users.json.
// Also stores optional reason. Kicks active sessions when banning.
// Returns error string on failure, nullptr on success.
static const char* setUserBanInternal(const String& username, bool ban, const String& reason) {
  if (username.length() == 0) return "Error: Username required";
  if (!filesystemReady)       return "Error: Filesystem not ready";

  uint32_t userId = 0;
  getUserIdByUsername(username, userId);
  if (userId == 0) return "Error: User not found";
  if (userId == 1) return "Error: Cannot ban the primary admin account";

  {
    FsLockGuard guard("users.set_ban");
    if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.setBan"))) return "Error: users.json not found";
    File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.setBan"));
    if (!f) return "Error: Failed to read users.json";
    PSRAM_JSON_DOC(doc);
    if (deserializeJson(doc, f)) { f.close(); return "Error: Malformed users.json"; }
    f.close();

    bool found = false;
    for (JsonObject u : doc["users"].as<JsonArray>()) {
      const char* uname = u["username"] | "";
      if (username == uname) {
        if (ban) {
          u["banned"] = true;
          if (reason.length()) u["banReason"] = reason;
          else                 u.remove("banReason");
        } else {
          u.remove("banned");
          u.remove("banReason");
        }
        found = true;
        break;
      }
    }
    if (!found) return "Error: User not found";

    {
      // Atomic write (tmp + rename) — never truncate the live auth DB in place.
      String json;
      serializeJson(doc, json);
      if (json.length() == 0 || !writeTextAtomic(USERS_JSON_FILE, json)) {
        logSystemEvent("USERS", "users.json REWRITE FAILED during %s of '%s' — auth database may be inconsistent", ban ? "ban" : "unban", username.c_str());
        return "Error: Failed to write users.json";
      }
    }
  }

  if (ban) {
    // Revoke serial transport session if active for this user
    if (gSerialAuthed && gSerialUser.equalsIgnoreCase(username)) {
      gSerialAuthed = false;
      gSerialUser   = String();
    }
    // Revoke OLED/local display session if active for this user
    if (gLocalDisplayAuthed && gLocalDisplayUser.equalsIgnoreCase(username)) {
      gLocalDisplayAuthed = false;
      gLocalDisplayUser   = String();
    }
    // Clear G2 pair-time stamp if the deleted user paired the lens.
    // Same recovery path as the admin force-logout: re-stamp via
    // `bleautoconnect g2-glasses on` from another authenticated CLI.
    if (g2PairedUserMatches(username)) {
      g2PairedUserClear();
    }
    // Revoke Bluetooth sessions for this user
    (void)bleRevokeUserSessions(username);
#if ENABLE_HTTP_SERVER
    // Revoke all web sessions for this user
    if (gSessions) {
      for (int i = 0; i < MAX_SESSIONS; i++) {
        if (gSessions[i].sid.length() > 0 && gSessions[i].user.equalsIgnoreCase(username)) {
          enqueueTargetedRevokeForSessionIdx(i, "Your account has been suspended by an administrator.");
        }
      }
    }
#endif
  }

  DEBUG_USERSF("[users] %s user ban for '%s'", ban ? "set" : "cleared", username.c_str());
  return nullptr;
}

// Updates the "lastSeen" field in users.json for the given username.
// Stores an ISO-8601 timestamp (same format as createdAt).
// Only writes if the system clock appears valid (epoch > Jan 1, 2021).
void updateUserLastSeen(const String& username) {
  if (username.length() == 0 || !filesystemReady) return;
  time_t now = Clock::epochSeconds();
  if (!Clock::isValidEpoch(now)) return;  // Clock not set yet — skip write

  char isoTimestamp[25];
  struct tm tminfo;
  if (!gmtime_r(&now, &tminfo)) return;
  strftime(isoTimestamp, sizeof(isoTimestamp), "%Y-%m-%dT%H:%M:%SZ", &tminfo);

  FsLockGuard guard("users.last_seen");
  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.lastSeen"))) return;
  File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.lastSeen"));
  if (!f) return;
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();

  for (JsonObject u : doc["users"].as<JsonArray>()) {
    const char* uname = u["username"] | "";
    if (username == uname) {
      u.remove("lastSeenSec");  // Remove legacy field if present
      u["lastSeen"] = isoTimestamp;
      break;
    }
  }

  {
    // Atomic write (tmp + rename). This runs on every login — a raw truncate
    // here was the highest-frequency chance to shred the auth DB on a power cut.
    String json;
    serializeJson(doc, json);
    if (json.length() == 0 || !writeTextAtomic(USERS_JSON_FILE, json)) return;
  }
  DEBUG_USERSF("[users] lastSeen updated for '%s' -> %s", username.c_str(), isoTimestamp);
}

// Validate a username/password against per-user settings file
// Checks both 'password' (text) and 'gamepad_password' (pattern) fields
bool isValidUser(const String& u, const String& p) {
  if (!filesystemReady) return false;
  if (u.length() == 0 || p.length() == 0) return false;

  // Reject banned accounts before touching credentials
  if (isUserBanned(u)) return false;

  // Get user ID from username (verifies user exists in users.json)
  uint32_t userId = 0;
  if (!getUserIdByUsername(u, userId) || userId == 0) return false;
  
  // Load user settings containing passwords
  PSRAM_JSON_DOC(settings);
  if (!loadUserSettings(userId, settings)) return false;
  
  // Check text password
  const char* textPass = settings["password"];
  if (textPass && verifyUserPassword(p, String(textPass))) {
    return true;
  }
  
  // Check gamepad pattern password (if set)
  const char* gamepadPass = settings["gamepad_password"];
  if (gamepadPass && verifyUserPassword(p, String(gamepadPass))) {
    return true;
  }
  
  return false;
}

bool getUserIdByUsername(const String& username, uint32_t& outUserId) {
  outUserId = 0;
  if (!filesystemReady) return false;
  if (username.length() == 0) return false;

  {
    FsLockGuard guard("users.get_id");
    if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.getId"))) return false;
    File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.getId"));
    if (!f) return false;

    PSRAM_JSON_DOC(doc);
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
  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.getRole"))) return false;
  File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.getRole"));
  if (!f) return false;

  PSRAM_JSON_DOC(doc);
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

  if (!VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.approve"))) {
    errorOut = "User not found in pending list";
    return false;
  }

  // Parse pending_users.json with ArduinoJson
  File file = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.approve"));
  if (!file) {
    errorOut = "Could not read pending list";
    return false;
  }

  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    errorOut = "Malformed pending_users.json";
    return false;
  }

  // Build new array without the approved user
  PSRAM_JSON_DOC(newDoc);
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
    VFS::removeGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.approve"));
    fsUnlock();
  } else {
    // Write updated list
    file = VFS::openGuarded(PENDING_USERS_FILE, "w", VFS::systemAuth("user.approve"));
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
  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.approve"))) {
    // Create users.json with the first user (ID 1) using ArduinoJson
    PSRAM_JSON_DOC(doc);
    doc["version"] = 1;
    doc["nextId"] = 2;
    
    JsonArray users = doc["users"].to<JsonArray>();
    JsonObject user = users.add<JsonObject>();
    user["id"] = 1;
    user["username"] = username;
    // Password now stored in per-user settings file, not here
    user["role"] = "admin";
    user["createdAt"] = (const char*)nullptr;  // resolved lazily via boot anchor
    user["createdBy"] = "firstsetup";          // first admin = device owner (not "approved")
    user["createdAtSource"] = "pending";       // time-derivation status
    user["createdMs"] = millis();
    user["ntpAnchorId"] = gNTPAnchorId;
    user["bootCount"] = gBootCounter;
    
    DEBUG_SYSTEMF("ApproveInit: Creating users.json with admin.bootCount=%lu, gNTPAnchorId=%lu", (unsigned long)gBootCounter, (unsigned long)gNTPAnchorId);

    // Serialize atomically (tmp + rename). Boot anchors live in their own file now.
    {
      String json;
      serializeJson(doc, json);
      if (json.length() == 0 || !writeTextAtomic(USERS_JSON_FILE, json)) {
        errorOut = "Failed to create users.json";
        return false;
      }
    }

    createdUserId = 1;
  } else {
    // Parse existing users.json with ArduinoJson
    File file = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.approve"));
    if (!file) {
      errorOut = "Failed to open users.json";
      return false;
    }
    
    PSRAM_JSON_DOC(doc);  // Use dynamic document for users.json
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
    
    // Record WHO approved this pending request (provenance), distinct from a
    // first-setup owner admin. Falls back to "admin" if there is no auth context.
    char approvedByBuf[48];
    { String ap = currentAuthContext().user;
      snprintf(approvedByBuf, sizeof(approvedByBuf), "approved:%s", ap.length() ? ap.c_str() : "admin"); }
    JsonObject newUser = users.add<JsonObject>();
    newUser["id"] = nextId;
    newUser["username"] = username;
    // Password now stored in per-user settings file, not here
    newUser["role"] = "user";
    newUser["createdAt"] = (const char*)nullptr;  // resolved lazily via boot anchor
    newUser["createdBy"] = approvedByBuf;         // provenance: approved by an admin
    newUser["createdAtSource"] = "pending";       // time-derivation status
    newUser["createdMs"] = millis();
    newUser["ntpAnchorId"] = gNTPAnchorId;
    newUser["bootCount"] = gBootCounter;
    
    DEBUG_SYSTEMF("ApproveAppend: New user id=%d with bootCount=%lu, gNTPAnchorId=%lu", nextId, (unsigned long)gBootCounter, (unsigned long)gNTPAnchorId);
    
    // Update nextId
    doc["nextId"] = nextId + 1;
    
    // Write back atomically (tmp + rename).
    {
      String json;
      serializeJson(doc, json);
      if (json.length() == 0 || !writeTextAtomic(USERS_JSON_FILE, json)) {
        logSystemEvent("USERS", "users.json REWRITE FAILED during approve of '%s' — auth database may be inconsistent", username.c_str());
        errorOut = "Failed to write users.json";
        return false;
      }
    }

    createdUserId = (uint32_t)nextId;
  }

  if (createdUserId > 0 && filesystemReady) {
    String settingsPath = getUserSettingsPath(createdUserId);
    FsLockGuard guard("user_settings.default");
    // Create user settings with password and defaults
    PSRAM_JSON_DOC(defaults);
    defaults["theme"] = "light";
    defaults["password"] = userPassword;  // Store hashed password in user settings
    if (!saveUserSettings(createdUserId, defaults)) {
      WARN_SESSIONF("Failed to create user settings for userId=%u", (unsigned)createdUserId);
    }
  }

  // Identity topology changed (pending account is now real and usable).
  // Note: userrequest/userdeny do NOT bump — pending accounts can't
  // authenticate, so their existence/removal doesn't change "who can read
  // what." Approval is when the account first matters. See
  // System_AuthIdentity.h for the full bump-site list.
  bumpIdentityGeneration("user.approve");
  // [EVENT] Covers BOTH the CLI and the web approve path (the web POST
  // bypasses the command audit, so this is the single source of truth).
  logSystemEvent("USERS", "user '%s' approved (id=%lu)", username.c_str(), (unsigned long)createdUserId);
  BROADCAST_PRINTF("[admin] Approved user: %s", username.c_str());

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
  
  if (!VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.deny"))) {
    errorOut = "User not found in pending list";
    return false;
  }

  // Parse pending_users.json with ArduinoJson
  File file = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.deny"));
  if (!file) {
    errorOut = "Could not read pending list";
    return false;
  }

  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    errorOut = "Malformed pending_users.json";
    return false;
  }

  // Build new array without the denied user
  PSRAM_JSON_DOC(newDoc);
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
    VFS::removeGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.deny"));
  } else {
    // Write updated list
    file = VFS::openGuarded(PENDING_USERS_FILE, "w", VFS::systemAuth("user.deny"));
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
  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.promote"))) {
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
              json = before + "admin" + after;
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
  if (!writeTextAtomic(USERS_JSON_FILE, json)) {
    errorOut = "Failed to write users.json";
    return false;
  }
  // Identity topology changed (user gained admin permissions). Invalidate
  // any auth-dependent caches — see System_AuthIdentity.h for the protocol.
  bumpIdentityGeneration("user.promote");
  BROADCAST_PRINTF("[admin] Promoted user to admin: %s", username.c_str());

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
  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.demote"))) {
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
              json = before + "user" + after;
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
  if (!writeTextAtomic(USERS_JSON_FILE, json)) {
    errorOut = "Failed to write users.json";
    return false;
  }
  // Identity topology changed (user lost admin permissions). Invalidate
  // any auth-dependent caches — see System_AuthIdentity.h for the protocol.
  bumpIdentityGeneration("user.demote");
  // Force-logout the demoted user across all transports. Per the design:
  // a session that was running with admin privileges should NOT silently
  // continue with reduced privileges — the admin who clicked "demote"
  // expects immediate effect. The user re-authenticates and resumes
  // with their now-correct non-admin permissions.
  //
  // Self-demote case: if the calling admin is demoting themselves, they
  // get kicked too. That's intentional — they explicitly asked for the
  // change and the principle of least surprise is "demote takes effect
  // immediately, no exceptions."
  if (gSerialAuthed && gSerialUser == username) {
    broadcastOutput("[serial] Your admin privileges have been revoked");
  }
  revokeUserSessions(username, "Your admin privileges have been revoked. Please log in again.");
  BROADCAST_PRINTF("[admin] Demoted user from admin: %s", username.c_str());

  return true;
}

// Delete an existing user from users.json and their settings file
static bool deleteUserInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] delete internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  
  // Get userId first so we can delete their settings file
  uint32_t userId = 0;
  getUserIdByUsername(username, userId);

  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.delete"))) {
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
  if (!writeTextAtomic(USERS_JSON_FILE, json)) {
    logSystemEvent("USERS", "users.json REWRITE FAILED during delete of '%s' — auth database may be inconsistent", username.c_str());
    errorOut = "Failed to write users.json";
    return false;
  }
  // Identity topology changed (user removed — their permission set
  // evaporates). Invalidate any auth-dependent caches — see
  // System_AuthIdentity.h for the protocol.
  bumpIdentityGeneration("user.delete");
  // Forcibly log out every active session for the deleted user across
  // all transports (web/serial/oled/bluetooth). No exception filter:
  // the account is gone, so even if the calling admin is themselves
  // the user being deleted, kick them too. revokeUserSessions returns
  // the count and emits [SESSION-REVOKE] to the audit log; we just
  // need a per-transport user-facing notice on serial in addition.
  if (gSerialAuthed && gSerialUser.equalsIgnoreCase(username)) {
    broadcastOutput("[serial] Your account has been deleted. You have been logged out.");
  }
  int revokedSessions =
      revokeUserSessions(username, "Account deleted by administrator");

  // Delete user settings file (contains password and preferences)
  if (userId > 0) {
    String settingsPath = getUserSettingsPath(userId);
    if (VFS::existsGuarded(settingsPath.c_str(), VFS::systemAuth("user.settings.remove"))) {
      VFS::removeGuarded(settingsPath.c_str(), VFS::systemAuth("user.settings.remove"));
      DEBUG_USERSF("[users] Deleted settings file for userId=%u", (unsigned)userId);
    }
  }

  if (revokedSessions > 0) {
    BROADCAST_PRINTF("[admin] Deleted user: %s (%d active session(s) terminated)", username.c_str(), revokedSessions);
  } else {
    BROADCAST_PRINTF("[admin] Deleted user: %s", username.c_str());
  }
  return true;
}

// ============================================================================
// User Command Handlers
// ============================================================================

const char* cmd_user_approve(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsInput;
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

const char* cmd_user_deny(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsInput;
  username.trim();
  DEBUG_USERSF("[users] CLI deny username=%s", username.c_str());
  String err;
  if (!denyPendingUserInternal(username, err)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Error: Denied";
  snprintf(getDebugBuffer(), 1024, "Denied user '%s'", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_promote(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsInput;
  username.trim();
  if (username.length() == 0) return "Error: invalid arguments — Usage: user promote <username>";
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

const char* cmd_user_demote(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsInput;
  username.trim();
  if (username.length() == 0) return "Error: invalid arguments — Usage: user demote <username>";
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

// userdelete is two-step like filedelete: prompt + yes/no confirm via the
// CLIMode framework. Static state survives the gap between the prompting
// command and the user's yes/no reply (cmd_exec runs both, sequentially,
// with the framework holding sActiveMode in between).
static String s_pendingUserDeleteName;

static const char* user_delete_confirmed(void* /*userData*/) {
  static char respBuf[160];
  if (!filesystemReady) return "Error: LittleFS not ready";

  DEBUG_USERSF("[users] CLI delete (confirmed) username=%s",
               s_pendingUserDeleteName.c_str());
  String err;
  if (!deleteUserInternal(s_pendingUserDeleteName, err)) {
    snprintf(respBuf, sizeof(respBuf), "Error: %s", err.c_str());
    return respBuf;
  }
  snprintf(respBuf, sizeof(respBuf), "Deleted user '%s'",
           s_pendingUserDeleteName.c_str());
  return respBuf;
}

static const char* user_delete_cancelled(void* /*userData*/) {
  static char respBuf[120];
  snprintf(respBuf, sizeof(respBuf), "Cancelled. User '%s' not deleted.",
           s_pendingUserDeleteName.c_str());
  return respBuf;
}

const char* cmd_user_delete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsInput;
  username.trim();
  if (username.length() == 0) return "Error: invalid arguments — Usage: user delete <username>";
  DEBUG_USERSF("[users] CLI delete (prompt) username=%s", username.c_str());

  // Stash the target name for the confirm callbacks. We do NOT capture
  // the AuthContext here because deleteUserInternal doesn't take one --
  // it has internal permission logic. (Compare to filedelete which uses
  // VFS::removeGuarded(path, ctx).)
  s_pendingUserDeleteName = username;

  String prompt = "Confirm delete of user '" + username +
                  "'? All sessions for this user will be revoked.";
  // Originating command line stored for the resolution audit -- shows up
  // in [CMD] log as "userdelete bob (confirm: yes) -> Deleted user 'bob'"
  // (or "(confirm: no) -> Cancelled. User 'bob' not deleted." on cancel).
  String origCmd = "userdelete " + username;
  if (!cliRequestConfirm(prompt, origCmd, user_delete_confirmed, user_delete_cancelled, nullptr)) {
    return "Error: cannot request confirm (another interactive mode is active)";
  }
  return "Type 'yes' to confirm or anything else to cancel.";
}

// Shared password-change implementation. Both the CLI command wrapper below
// and the web POST handler call this. Resolves the caller via the per-task
// TLS identity (ExecIdentityGuard installed upstream by executeCommand for
// command-pipeline callers, or by the web handler before invocation).
//
// Previously the body of cmd_user_changepassword hard-coded
// getTransportUser(SOURCE_LOCAL_DISPLAY) for user resolution, which only
// worked from the OLED. Every non-OLED transport got the OLED user's name
// back regardless of who actually called. Now currentExecUser() returns the
// right user for every transport because each dispatch path installs its own
// AuthContext into the TLS slot.
const char* userChangePasswordCore(const String& currentPassword,
                                   const String& newPassword,
                                   const String& confirmPassword) {
  if (!filesystemReady) return "Error: LittleFS not ready";

  if (newPassword != confirmPassword) {
    return "Error: New passwords do not match";
  }

  if (newPassword.length() < 6) {
    return "Error: Password must be at least 6 characters";
  }

  if (newPassword == currentPassword) {
    return "Error: New password must differ from current password";
  }

  // Resolve calling user from per-task TLS identity. Works for every
  // transport (web / CLI / OLED / BLE / ESP-NOW / serial) because each
  // dispatch path installs its own AuthContext via ExecIdentityGuard.
  String username = currentExecUser();
  if (username.length() == 0) return "Error: Not authenticated";
  const AuthContext& caller = currentAuthContext();

  // Verify current password
  if (!isValidUser(username, currentPassword)) {
    // Security event — log alongside login attempts so the auth-events
    // file shows both authentication AND credential-rotation activity.
    logAuthAttempt(false, caller.path.c_str(), username, caller.ip, "Current password incorrect");
    return "Error: Current password incorrect";
  }

  if (!setUserPassword(username, newPassword)) {
    logAuthAttempt(false, caller.path.c_str(), username, caller.ip, "Password storage failed");
    if (!ensureDebugBuffer()) return "Error: Failed to change password";
    snprintf(getDebugBuffer(), 1024, "Error: Failed to change password for user '%s'", username.c_str());
    return getDebugBuffer();
  }

  // Force-logout the user's OTHER sessions across all transports —
  // their credentials just rotated. Keep the calling session alive
  // (they just authenticated with the new password; kicking them
  // immediately would be bad UX). No clock bump: password change
  // does not change permission topology, so auth-derived caches
  // (FileManager etc.) don't need to invalidate. See
  // System_AuthIdentity.h "SISTER PROTOCOL: SESSION REVOCATION"
  // for the bump-vs-revoke decision matrix.
  revokeUserSessions(username,
                     "Your password was changed. Please log in again.",
                     caller.sid,         // skip current web SID (if any)
                     caller.transport);  // skip current transport's session

  // Audit-log the successful rotation. The reason string "Password changed"
  // is what logAuthAttempt's filter matches on to write this to the
  // security-events file (rather than the verbose command-audit log).
  logAuthAttempt(true, caller.path.c_str(), username, caller.ip, "Password changed");

  if (!ensureDebugBuffer()) return "Password changed successfully";
  snprintf(getDebugBuffer(), 1024, "Password changed successfully for user '%s'", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_changepassword(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Parse: "currentPassword newPassword confirmPassword"
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(3)) return "Error: invalid arguments — Usage: user changepassword <currentPassword> <newPassword> <confirmPassword>";

  return userChangePasswordCore(a.arg(0), a.arg(1), a.arg(2));
}

const char* cmd_user_resetpassword(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  
  // Parse: "<username> <newPassword> [0|1]" — optional 1 = require new password on next login
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: user resetpassword <username> <newPassword> [0|1]\nOptional: 1 = require password change on next login, 0 = omit";
  }

  String username = a.arg(0);
  String rest = a.arg(1);
  bool mustChange = a.argBool(2, false);

  if (rest.length() < 6) {
    return "Error: Password must be at least 6 characters";
  }

  if (!setUserPassword(username, rest, mustChange)) {
    if (!ensureDebugBuffer()) return "Error: Failed to reset password";
    snprintf(getDebugBuffer(), 1024, "Error: Failed to reset password for user '%s'", username.c_str());
    return getDebugBuffer();
  }

  // Force-logout target user's sessions everywhere. Admin's own session
  // is a different user, so no exception filter — the target gets
  // kicked from every transport and must log in with the new password.
  // No clock bump: password reset does not change permission topology.
  revokeUserSessions(username,
                     "Your password was reset by an administrator. Please log in again.");

  if (!ensureDebugBuffer()) return "Password reset successfully";
  snprintf(getDebugBuffer(), 1024, "Password reset successfully for user '%s'%s", username.c_str(),
           mustChange ? " (must change password on next login)" : "");
  return getDebugBuffer();
}

// Create user immediately (admin). Args: "<username> <password> [0|1]" — optional trailing 1 = must change password on next login.
const char* cmd_user_add(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: user add <username> <password> [0|1]\n(optional last arg: 1 = require new password on next login)";
  }

  String username = a.arg(0);
  String rest = a.arg(1);
  bool mustChange = a.argBool(2, false);

  if (rest.length() < 6) {
    return "Error: Password must be at least 6 characters";
  }

  String createdBy = currentAuthContext().user;
  if (createdBy.length() == 0) {
    createdBy = "cli";
  }

  String err;
  if (!adminCreateUser(username, rest, mustChange, createdBy, err)) {
    if (!ensureDebugBuffer()) return "Error: Failed to create user";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }

  if (!ensureDebugBuffer()) return "User created";
  snprintf(getDebugBuffer(), 1024, "Created user '%s'%s", username.c_str(),
           mustChange ? " (must change password on next login)" : "");
  return getDebugBuffer();
}

const char* cmd_user_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  // Check if JSON output is requested (word-boundary "json" token via argWantsJson)
  bool jsonOutput = argWantsJson(argsInput);
  
  DEBUG_USERSF("[USER_LIST_DEBUG] Called with args='%s', jsonOutput=%d", argsInput.c_str(), jsonOutput);

  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.list"))) {
    DEBUG_USERSF("[USER_LIST_DEBUG] File not found: %s", USERS_JSON_FILE);
    return jsonOutput ? "{\"schema\":1,\"users\":[]}" : "No users found";
  }

  // Open and parse users file with ArduinoJson
  File file = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.list"));
  if (!file) {
    ERROR_SESSIONF("Failed to open users file");
    if (jsonOutput) return "{\"schema\":1,\"users\":[]}";
    broadcastOutput("Error: Failed to read users file");
    return "ERROR";
  }

  // Parse JSON document (2KB should be enough for users file)
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    ERROR_SESSIONF("JSON parse error: %s", error.c_str());
    if (jsonOutput) return "{\"schema\":1,\"users\":[]}";
    broadcastOutput("Error: Malformed users file");
    return "ERROR";
  }

  JsonArray users = doc["users"];
  if (!users) {
    DEBUG_USERSF("[USER_LIST_DEBUG] No users array found");
    return jsonOutput ? "{\"schema\":1,\"users\":[]}" : "No users found";
  }

  if (jsonOutput) {
    // Use static PSRAM buffer - 2KB sufficient for user list
    static char* jsonBuf = nullptr;
    static const size_t kBufSize = 2048;
    if (!jsonBuf) {
      jsonBuf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "user.list.json");
      if (!jsonBuf) return "{\"schema\":1,\"users\":[]}";
    }
    // Wrap under {"v":1,"users":[...]} for the object-only JSON contract.
    PSRAM_JSON_DOC(out);
    out["schema"] = 1;
    out["users"].set(users);
    size_t len = serializeJson(out, jsonBuf, kBufSize);
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

const char* cmd_pending_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  // Check if JSON output is requested
  bool jsonOutput = argWantsJson(argsInput);

  if (!VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.pending.list"))) {
    if (jsonOutput) return "{\"schema\":1,\"pending\":[]}";
    broadcastOutput("No pending users");
    return "OK";
  }

  // Open and parse pending users file with ArduinoJson
  File file = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.pending.list"));
  if (!file) {
    if (jsonOutput) return "{\"schema\":1,\"pending\":[]}";
    ERROR_SESSIONF("Failed to read pending users file");
    broadcastOutput("Error: Failed to read pending users file");
    return "ERROR";
  }

  // Parse JSON array (1KB should be enough for pending users)
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    if (jsonOutput) return "{\"schema\":1,\"pending\":[]}";
    ERROR_SESSIONF("Malformed pending users file");
    broadcastOutput("Error: Malformed pending users file");
    return "ERROR";
  }

  JsonArray pending = doc.as<JsonArray>();
  if (!pending) {
    return jsonOutput ? "{\"schema\":1,\"pending\":[]}" : "No pending users";
  }

  if (jsonOutput) {
    // Use static PSRAM buffer - 2KB sufficient for pending list
    static char* jsonBuf = nullptr;
    static const size_t kBufSize = 2048;
    if (!jsonBuf) {
      jsonBuf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "pending.list.json");
      if (!jsonBuf) return "{\"schema\":1,\"pending\":[]}";
    }
    // Build sanitized {"v":1,"pending":[...]} object without password hashes.
    PSRAM_JSON_DOC(sanitized);
    sanitized["schema"] = 1;
    JsonArray sanitizedArray = sanitized["pending"].to<JsonArray>();
    for (JsonObject user : pending) {
      JsonObject sanitizedUser = sanitizedArray.add<JsonObject>();
      sanitizedUser["username"] = user["username"];
      sanitizedUser["timestamp"] = user["timestamp"];
      // Explicitly exclude password field for security
    }
    size_t len = serializeJson(sanitized, jsonBuf, kBufSize);
    if (len >= kBufSize) {
      ERROR_MEMORYF("pending list JSON truncated: %zu >= %zu", len, kBufSize);
    }
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
const char* cmd_session_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Check if JSON output is requested
  bool jsonOutput = argWantsJson(argsInput);

  if (jsonOutput) {
    // Use static PSRAM buffer - 2KB sufficient for session list
    static char* jsonBuf = nullptr;
    static const size_t kBufSize = 2048;
    if (!jsonBuf) {
      jsonBuf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "session.list.json");
      if (!jsonBuf) return "{\"schema\":1,\"sessions\":[]}";
    }
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray sessions = doc["sessions"].to<JsonArray>();
    buildAllSessionsJson("", sessions);
    // Append active transport (non-web) sessions as synthetic entries
    if (gSerialAuthed && gSerialUser.length()) {
      JsonObject t = sessions.add<JsonObject>();
      t["user"]      = gSerialUser;
      t["transport"] = "serial";
    }
    if (gLocalDisplayAuthed && gLocalDisplayUser.length()) {
      JsonObject t = sessions.add<JsonObject>();
      t["user"]      = gLocalDisplayUser;
      t["transport"] = "oled";
    }
    {
      String g2User = g2PairedUserGet();
      if (g2User.length()) {
        JsonObject t = sessions.add<JsonObject>();
        t["user"]      = g2User;
        t["transport"] = "g2";
      }
    }
    for (int i = 0;; ++i) {
      uint16_t connId = 0;
      String user;
      if (!bleGetAuthenticatedSessionInfo(i, connId, user)) break;
      JsonObject t = sessions.add<JsonObject>();
      t["user"]      = user;
      t["transport"] = "bluetooth";
      t["sid"]       = String(connId);
    }
    size_t len = serializeJson(doc, jsonBuf, kBufSize);
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

// cmd_login and cmd_logout moved to System_Utils.cpp (critical system functions)

const char* cmd_session_revoke(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String subcmd = a.arg(0);
  subcmd.toLowerCase();

  auto defaultReason = String("Your session has been signed out by an administrator.");

  int revoked = 0;

  if (subcmd == "sid") {
    String sid = a.arg(1);
    String reason = a.has(2) ? a.remaining(1) : String();
    if (!reason.length()) reason = defaultReason;
    int idx = findSessionIndexBySID(sid);
    if (idx < 0) return "Error: Session not found for given SID.";
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

  if (subcmd == "user") {
    String username = a.arg(1);
    String reason = a.has(2) ? a.remaining(1) : String();
    if (!reason.length()) reason = defaultReason;

    // Delegate fan-out to revokeUserSessions — single source of truth for
    // "kick user X out of every transport's session table." The previous
    // inline implementation duplicated this loop and drifted: it was
    // missing the oledNotifyLocalDisplayAuthChanged() call that
    // revokeUserSessions emits when clearing the OLED session, so the
    // OLED UI didn't refresh after an admin-revoke. Routing through the
    // shared helper picks that up automatically, and any future revoke-
    // protocol additions land in one place. We do NOT pass exceptSid /
    // exceptTransport because this command is admin-driven and revokes
    // someone else's sessions (not a self-modify).
    revoked = revokeUserSessions(username, reason);

    if (revoked > 0) {
      // Admin audit broadcast — emitted at the command layer (not inside
      // revokeUserSessions, which only WARN_USERFs internally) so the
      // operator running the CLI sees confirmation in their terminal.
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

  return "Error: invalid arguments — Usage:\n"
         "  sessionrevoke sid <sid> [reason]\n"
         "  sessionrevoke user <username> [reason]";
}
const char* cmd_ban(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: ban <ip> [reason]";

  String ip = a.arg(0);
  String reason = a.has(1) ? a.remaining(0) : String();
  // Basic format check — must contain a dot (IPv4) or colon (IPv6)
  if (ip.indexOf('.') < 0 && ip.indexOf(':') < 0) {
    return "Error: invalid IP address format (expected e.g. 192.168.1.100)";
  }

  if (banIp(ip.c_str(), reason.length() ? reason.c_str() : nullptr)) {
    static char buf[140];
    if (reason.length()) snprintf(buf, sizeof(buf), "Banned %s — %s", ip.c_str(), reason.c_str());
    else                 snprintf(buf, sizeof(buf), "Banned %s", ip.c_str());
    return buf;
  }
  return "Error: could not ban IP (list may be full or save failed)";
}

const char* cmd_unban(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String ip = argsInput;
  ip.trim();
  if (ip.length() == 0) return "Error: invalid arguments — Usage: unban <ip>";

  if (unbanIp(ip.c_str())) {
    static char buf[80];
    snprintf(buf, sizeof(buf), "Unbanned %s", ip.c_str());
    return buf;
  }
  return "Error: IP not found in ban list";
}

const char* cmd_banlist(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) return banListJson();  // built in WebServer_Server.cpp where sIpBans lives
  broadcastBanList();
  return "";
}

const char* cmd_banuser(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: banuser <username> [reason]";

  String username = a.arg(0);
  String reason = a.has(1) ? a.remaining(0) : String();

  const char* err = setUserBanInternal(username, true, reason);
  if (err) return err;

  static char buf[160];
  if (reason.length()) snprintf(buf, sizeof(buf), "Banned user '%s' — %s", username.c_str(), reason.c_str());
  else                 snprintf(buf, sizeof(buf), "Banned user '%s'", username.c_str());
  return buf;
}

const char* cmd_unbanuser(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String username = argsInput;
  username.trim();
  if (username.length() == 0) return "Error: invalid arguments — Usage: unbanuser <username>";

  const char* err = setUserBanInternal(username, false, "");
  if (err) return err;

  static char buf[80];
  snprintf(buf, sizeof(buf), "Unbanned user '%s'", username.c_str());
  return buf;
}

#else
// Stub implementations when HTTP server is disabled
// Note: cmd_login and cmd_logout are in System_Utils.cpp (always available)
const char* cmd_session_list(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(originalCmd)) return "{\"schema\":1,\"available\":false,\"reason\":\"http server disabled\"}";
  return "Session management requires HTTP server to be enabled";
}
const char* cmd_session_revoke(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Session management requires HTTP server to be enabled";
}
const char* cmd_ban(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Ban management requires HTTP server to be enabled";
}
const char* cmd_unban(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Ban management requires HTTP server to be enabled";
}
const char* cmd_banlist(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(originalCmd)) return "{\"schema\":1,\"available\":false,\"reason\":\"http server disabled\"}";
  return "Ban management requires HTTP server to be enabled";
}
const char* cmd_banuser(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Ban management requires HTTP server to be enabled";
}
const char* cmd_unbanuser(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Ban management requires HTTP server to be enabled";
}
#endif // ENABLE_HTTP_SERVER

const char* cmd_user_request(const String& argsInput) {
  //RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  // Syntax: args = "<username> <password> [confirmPassword]"
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: user request <username> <password> [confirmPassword]";

  String username = a.arg(0);
  String password = a.arg(1);
  String confirm = a.arg(2);  // empty if not provided
  if (confirm.length() && confirm != password) return "Error: passwords do not match";
  // Add to pending_users.json in JSON format
  DEBUG_CMD_FLOWF("[users] Adding user to pending_users.json, filesystemReady=%d", filesystemReady ? 1 : 0);

  String json = "[]";
  if (VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.request"))) {
    if (!readText(PENDING_USERS_FILE, json)) {
      DEBUG_CMD_FLOWF("[users] ERROR: Failed to read existing /system/pending_users.json");
      return "Error: could not read pending list";
    }
  }

  // Parse existing JSON array or create new one
  if (json.length() < 2 || !json.startsWith("[")) json = "[]";

  // Create new user entry with hashed password
  String hashedPassword = hashUserPassword(password);
  char entryBuf[256];
  snprintf(entryBuf, sizeof(entryBuf), "{\"username\":\"%s\",\"password\":\"%s\",\"timestamp\":%lu}",
           username.c_str(), hashedPassword.c_str(), (unsigned long)millis());
  String userEntry = entryBuf;

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
  bool okWrite = writeTextAtomic(PENDING_USERS_FILE, json);
  if (!okWrite) {
    ERROR_STORAGEF("writeText failed when writing pending_users.json");
    broadcastOutput("[users] ERROR: writeText failed for /system/pending_users.json");
    return "Error: could not write pending list";
  }
  size_t fsz = 0;
  File dbgFile = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.request"));
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

// Boot anchors (NTP time-sync scaffolding) live in their OWN file, decoupled
// from the credential database. They are written once per boot on NTP sync;
// keeping them out of users.json means users.json only changes on real account
// edits, and an anchor write can never touch the auth DB. Structure:
//   {"bootAnchors":[{"ntpAnchorId":N,"epochAtSync":E,"millisAtSync":M}, ...]}
const char* BOOT_ANCHORS_FILE = "/system/boot_anchors.json";

// External dependencies for timestamp resolution
extern uint32_t gNTPAnchorId;
extern uint32_t gBootCounter;
extern bool gTimeSyncedMarkerWritten;
extern void timeSyncUpdateBootEpoch();
extern void getTimestampPrefixMsCached(char* buf, size_t bufSize);
extern bool appendLineWithCap(const char* path, const String& line, size_t cap);

// Boot anchor structure - represents an NTP sync point
struct BootAnchor {
  uint32_t ntpAnchorId;
  time_t epochAtSync;
  unsigned long millisAtSync;

  BootAnchor()
    : ntpAnchorId(0), epochAtSync(0), millisAtSync(0) {}
  BootAnchor(uint32_t seq, time_t epoch, unsigned long ms)
    : ntpAnchorId(seq), epochAtSync(epoch), millisAtSync(ms) {}
};

// User timestamp info - extracted from JSON for resolution
struct UserTimestampInfo {
  int jsonStartPos;
  int jsonEndPos;
  uint32_t ntpAnchorId;
  unsigned long createdMs;
  int bootCount;
  bool needsResolution;

  UserTimestampInfo()
    : jsonStartPos(-1), jsonEndPos(-1),
      ntpAnchorId(0), createdMs(0),
      bootCount(-1), needsResolution(false) {}
};

// Helper: Check if username exists in users.json content
bool usernameExistsInUsersJson(const String& json, const String& username) {
  char needleBuf[80];
  snprintf(needleBuf, sizeof(needleBuf), "\"username\": \"%s\"", username.c_str());
  String needle = needleBuf;
  return json.indexOf(needle) >= 0;
}

// (Removed: loadUsersFromFile — read a `passwordHash` field that no longer
// exists in users.json since the User struct moved to PBKDF2 with per-device
// salt. The function returned false every time, so its only caller in
// HardwareOne.cpp boot left the legacy gAuthUser/gAuthPass globals at their
// "admin"/"admin" defaults. Both the function and its callers + the Basic-
// Auth fast path that consumed those globals were deleted together. See
// AUTH_ASSESSMENT_REPORT.md §4 #6.)

// Forward declarations for helper functions
static int parseBootAnchors(const String& usersJson, BootAnchor* anchors, int maxCount);
static bool parseUserTimestampInfo(const String& userObj, int userStart, UserTimestampInfo& info);
static BootAnchor* findMatchingAnchor(BootAnchor* anchors, int count, uint32_t ntpAnchorId);
static bool replaceJsonField(String& json, const char* fieldName, const String& newValue, int searchStart);
static bool resolveUserTimestamp(String& usersJson, const UserTimestampInfo& info, const BootAnchor& anchor);
static bool approximateUserTimestamp(String& usersJson, const UserTimestampInfo& info, uint32_t ordinalNumber);
static bool extractJsonInt(const String& json, const char* fieldName, int& outValue, int searchStart);
static void buildOrdinal(uint32_t n, char* buf, size_t bufSize);
static bool formatEpochAsISO8601(time_t epoch, char* buf, size_t bufSize);

// Extract integer field from JSON substring
static bool extractJsonInt(const String& json, const char* fieldName,
                           int& outValue, int searchStart = 0) {
  char needleBuf[64];
  snprintf(needleBuf, sizeof(needleBuf), "\"%s\":", fieldName);
  int idx = json.indexOf(needleBuf, searchStart);
  if (idx < 0) return false;

  int valueStart = idx + strlen(needleBuf);
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
  if (!Clock::isValidEpoch(epoch)) {
    strncpy(buf, "null", bufSize);
    return false;
  }
  struct tm tminfo;
  if (!gmtime_r(&epoch, &tminfo)) {
    strncpy(buf, "null", bufSize);
    return false;
  }
  strftime(buf, bufSize, "%Y-%m-%dT%H:%M:%SZ", &tminfo);
  return true;
}

// Parse boot anchors from users.json
static int parseBootAnchors(const String& usersJson, BootAnchor* anchors, int maxCount) {
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, usersJson);
  if (error) return 0;

  JsonArray bootAnchorsArray = doc["bootAnchors"];
  if (!bootAnchorsArray) return 0;

  int count = 0;
  for (JsonObject anchor : bootAnchorsArray) {
    if (count >= maxCount) break;
    
    int ntpAnchorId = anchor["ntpAnchorId"] | 0;
    int epochAtSync = anchor["epochAtSync"] | 0;
    int millisAtSync = anchor["millisAtSync"] | 0;
    
    if (ntpAnchorId > 0 && epochAtSync > 0) {
      anchors[count++] = BootAnchor(ntpAnchorId, epochAtSync, millisAtSync);
    }
  }

  return count;
}

// Read boot anchors from their dedicated file. Returns count (0 if the file is
// absent, empty, or corrupt — all benign: anchors are regenerated on NTP sync).
static int readBootAnchors(BootAnchor* anchors, int maxCount) {
  if (!filesystemReady) return 0;
  String json;
  if (!readText(BOOT_ANCHORS_FILE, json) || json.length() == 0) return 0;
  return parseBootAnchors(json, anchors, maxCount);
}

// Highest ntpAnchorId currently persisted (0 if none). Seeds gNTPAnchorId at boot.
static uint32_t highestBootAnchorId() {
  BootAnchor anchors[16];
  int n = readBootAnchors(anchors, 16);
  uint32_t maxId = 0;
  for (int i = 0; i < n; i++) {
    if (anchors[i].ntpAnchorId > maxId) maxId = anchors[i].ntpAnchorId;
  }
  return maxId;
}

// Parse user timestamp info from user object
static bool parseUserTimestampInfo(const String& userObj, int userStart,
                                   UserTimestampInfo& info) {
  info.jsonStartPos = userStart;
  // Needs resolution if createdAt is:
  //   1. JSON null (NTP wasn't synced at creation time)
  //   2. Missing entirely (ArduinoJson may drop null values on re-serialize)
  //   3. An approximate sentinel like "1st Power Cycle" written by approximateUserTimestamp()
  //      — happens when gBootCounter advanced past bootCount before an anchor was available;
  //      now that NTP has synced we can replace the approximation with the real timestamp.
  bool hasNullCreatedAt    = (userObj.indexOf("\"createdAt\":null") >= 0) ||
                             (userObj.indexOf("\"createdAt\": null") >= 0);
  bool hasMissingCreatedAt = (userObj.indexOf("\"createdAt\"") < 0);
  // The approximate sentinel now lives in createdAtSource; also accept the
  // legacy createdBy location so pre-split users are still re-resolved.
  bool isApproximate       = (userObj.indexOf("\"createdAtSource\":\"approx_power_cycle\"") >= 0) ||
                             (userObj.indexOf("\"createdAtSource\": \"approx_power_cycle\"") >= 0) ||
                             (userObj.indexOf("\"createdBy\":\"approx_power_cycle\"") >= 0) ||
                             (userObj.indexOf("\"createdBy\": \"approx_power_cycle\"") >= 0);
  info.needsResolution = hasNullCreatedAt || hasMissingCreatedAt || isApproximate;

  if (!info.needsResolution) return false;

  int ntpAnchorId, msSinceBoot, bootCounter;
  if (!extractJsonInt(userObj, "ntpAnchorId", ntpAnchorId) || !extractJsonInt(userObj, "createdMs", msSinceBoot)) {
    return false;
  }

  info.ntpAnchorId = ntpAnchorId;
  info.createdMs = msSinceBoot;

  if (extractJsonInt(userObj, "bootCount", bootCounter)) {
    info.bootCount = bootCounter;
  }

  return true;
}

// Find matching anchor for boot sequence
static BootAnchor* findMatchingAnchor(BootAnchor* anchors, int count, uint32_t ntpAnchorId) {
  for (int i = 0; i < count; i++) {
    if (anchors[i].ntpAnchorId == ntpAnchorId) {
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

  time_t createdAtUtc;
  bool crossBoot = false;

  if (delta >= 0) {
    // Same-boot: user was created before NTP synced in the same boot cycle.
    // Exact calculation: walk back from the sync epoch by the elapsed ms difference.
    createdAtUtc = anchor.epochAtSync - (delta / 1000);
  } else {
    // Cross-boot: user was created in a previous boot (createdMs > millisAtSync).
    // We can't calculate the exact time without knowing when the prior boot started,
    // so use epochAtSync as an upper bound (user was created no later than this).
    createdAtUtc = anchor.epochAtSync;
    crossBoot = true;
  }

  if (createdAtUtc < 1577836800) return false;  // sanity: before Jan 2020
  time_t now = time(nullptr);
  if (now > 0 && createdAtUtc > now + 60) return false;  // sanity: not in the future

  char isoTimestamp[24];
  if (!formatEpochAsISO8601(createdAtUtc, isoTimestamp, sizeof(isoTimestamp))) {
    return false;
  }

  char quotedValue[32];
  snprintf(quotedValue, sizeof(quotedValue), "\"%s\"", isoTimestamp);

  if (!replaceJsonField(usersJson, "createdAt", String(quotedValue), info.jsonStartPos)) {
    return false;
  }

  // Mark whether this was an exact resolution or a cross-boot approximation.
  // Record HOW createdAt was derived in createdAtSource; leave createdBy
  // (provenance: who created the account) untouched.
  replaceJsonField(usersJson, "createdAtSource",
                   crossBoot ? "\"approx_ntp\"" : "\"ntp_resolved\"",
                   info.jsonStartPos);
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

  replaceJsonField(usersJson, "createdAtSource", "\"approx_power_cycle\"", info.jsonStartPos);
  return true;
}

// Prune boot anchors down to the single most-recent one — but only once NO user
// is still awaiting timestamp resolution (a pending user needs its anchor kept).
// Anchors now live in their own file (BOOT_ANCHORS_FILE); the pending-user guard
// still reads users.json, optionally reusing a users doc the caller already has.
// The usersDocPtr param is a JsonDocument* of parsed users.json, or nullptr.
void cleanupOldBootAnchors(void* usersDocPtr) {
  if (!filesystemReady) return;

  // 1) Don't prune if any user still has an unresolved (null) createdAt — that
  //    user's anchor must survive until it can be resolved.
  bool anyPending = false;
  {
    PSRAM_JSON_DOC(localUsers);
    JsonDocument* usersDoc = static_cast<JsonDocument*>(usersDocPtr);
    if (!usersDoc) {
      String usersJson;
      if (readText(USERS_JSON_FILE, usersJson) && usersJson.length() > 0) {
        DeserializationError err = deserializeJson(localUsers, usersJson);
        if (!err) usersDoc = &localUsers;
      }
    }
    if (usersDoc) {
      JsonArray usersArray = (*usersDoc)["users"];
      if (usersArray) {
        for (JsonObject user : usersArray) {
          if (user["createdAt"].isNull()) { anyPending = true; break; }
        }
      }
    }
  }
  if (anyPending) return;

  // 2) Prune the anchors file to just the most recent anchor.
  BootAnchor anchors[16];
  int n = readBootAnchors(anchors, 16);
  if (n <= 1) return;  // nothing to prune

  int maxIdx = 0;
  for (int i = 1; i < n; i++) {
    if (anchors[i].ntpAnchorId > anchors[maxIdx].ntpAnchorId) maxIdx = i;
  }

  PSRAM_JSON_DOC(doc);
  JsonArray arr = doc["bootAnchors"].to<JsonArray>();
  JsonObject a = arr.add<JsonObject>();
  a["ntpAnchorId"]  = (uint32_t)anchors[maxIdx].ntpAnchorId;
  a["epochAtSync"]  = (uint32_t)anchors[maxIdx].epochAtSync;
  a["millisAtSync"] = (uint32_t)anchors[maxIdx].millisAtSync;

  String out;
  serializeJson(doc, out);
  writeTextAtomic(BOOT_ANCHORS_FILE, out);
}

// Resolve pending user creation timestamps
void resolvePendingUserCreationTimes() {
  DEBUG_NTP_RESOLVEF("[resolve] Starting timestamp resolution");
  
  if (!filesystemReady || !VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.resolveCreated"))) {
    DEBUG_NTP_RESOLVEF("[resolve] Skipping - FS not ready or file missing");
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
    File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.resolveCreated"));
    if (!f) return;
    bytesRead = f.readBytes(usersJsonBuf, USERS_JSON_BUF_SIZE - 1);
    usersJsonBuf[bytesRead] = '\0';
    f.close();
  }

  if (bytesRead == 0) return;

  String usersJson = usersJsonBuf;
  DEBUG_NTP_RESOLVEF("[resolve] Read %d bytes from users.json", (int)bytesRead);

  const int MAX_ANCHORS = 16;
  BootAnchor anchors[MAX_ANCHORS];
  int anchorCount = readBootAnchors(anchors, MAX_ANCHORS);  // from BOOT_ANCHORS_FILE
  DEBUG_NTP_RESOLVEF("[resolve] Found %d boot anchors", anchorCount);
  
  for (int i = 0; i < anchorCount; i++) {
    DEBUG_NTP_RESOLVEF("[resolve] Anchor %d: ntpAnchorId=%lu epochAtSync=%u millisAtSync=%lu",
                 i, (unsigned long)anchors[i].ntpAnchorId, (unsigned)anchors[i].epochAtSync, (unsigned long)anchors[i].millisAtSync);
  }

  // Find the "users" array in the JSON - start searching after "users":
  int usersArrayStart = usersJson.indexOf("\"users\"");
  if (usersArrayStart < 0) {
    DEBUG_NTP_RESOLVEF("[resolve] No 'users' array found");
    return;
  }
  int arrayBracket = usersJson.indexOf('[', usersArrayStart);
  if (arrayBracket < 0) {
    DEBUG_NTP_RESOLVEF("[resolve] No '[' found after 'users'");
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
    DEBUG_NTP_RESOLVEF("[resolve] Checking user object at pos %d-%d", userStart, userEnd);

    UserTimestampInfo info;
    if (!parseUserTimestampInfo(userObj, userStart, info)) {
      DEBUG_NTP_RESOLVEF("[resolve] User doesn't need resolution (createdAt not null or missing fields)");
      userPos = userEnd + 1;
      continue;
    }

    DEBUG_NTP_RESOLVEF("[resolve] User needs resolution: ntpAnchorId=%lu createdMs=%lu bootCount=%d",
                 (unsigned long)info.ntpAnchorId, (unsigned long)info.createdMs, info.bootCount);

    BootAnchor* anchor = findMatchingAnchor(anchors, anchorCount, info.ntpAnchorId);

    if (anchor) {
      DEBUG_NTP_RESOLVEF("[resolve] Found matching anchor for ntpAnchorId=%lu", (unsigned long)info.ntpAnchorId);
      if (resolveUserTimestamp(usersJson, info, *anchor)) {
        INFO_SESSIONF("Successfully resolved timestamp");
        modified = true;
      } else {
        WARN_SESSIONF("Failed to resolve timestamp");
      }
    } else {
      DEBUG_NTP_RESOLVEF("[resolve] No matching anchor for ntpAnchorId=%lu", (unsigned long)info.ntpAnchorId);
      bool shouldApprox = false;
      uint32_t ordinalN = info.ntpAnchorId;

      if (info.bootCount > 0 && gBootCounter > 0) {
        if ((uint32_t)info.bootCount < gBootCounter) {
          shouldApprox = true;
          ordinalN = (uint32_t)info.bootCount;
        }
      } else if (info.ntpAnchorId < gNTPAnchorId) {
        shouldApprox = true;
      }

      if (shouldApprox) {
        DEBUG_NTP_RESOLVEF("[resolve] Approximating timestamp with ordinal %lu", (unsigned long)ordinalN);
        if (approximateUserTimestamp(usersJson, info, ordinalN)) {
          modified = true;
        }
      }
    }

    userPos = userEnd + 1;
  }

  if (modified) {
    DEBUG_NTP_RESOLVEF("[resolve] Writing modified users.json");
    if (writeTextAtomic(USERS_JSON_FILE, usersJson)) {
      cleanupOldBootAnchors();  // prunes BOOT_ANCHORS_FILE, guarded by pending-user check
    }
  } else {
    DEBUG_NTP_RESOLVEF("[resolve] No modifications needed");
  }
}

// Append a boot anchor (NTP just synced) to the dedicated anchors file. Written
// atomically via writeTextAtomic; users.json is NOT touched.
void writeBootAnchor() {
  time_t now = time(nullptr);
  if (now <= 0 || gNTPAnchorId == 0) return;
  if (!filesystemReady) return;

  unsigned long currentMillis = millis();

  // Load existing anchors (start empty if the file is absent or corrupt).
  PSRAM_JSON_DOC(doc);
  String existing;
  if (readText(BOOT_ANCHORS_FILE, existing) && existing.length() > 0) {
    DeserializationError error = deserializeJson(doc, existing);
    if (error) doc.clear();
  }

  JsonArray bootAnchorsArray = doc["bootAnchors"];
  if (bootAnchorsArray.isNull()) bootAnchorsArray = doc["bootAnchors"].to<JsonArray>();

  if ((int)bootAnchorsArray.size() >= 16) {
    bootAnchorsArray.remove(0);
  }

  JsonObject newAnchor = bootAnchorsArray.add<JsonObject>();
  newAnchor["ntpAnchorId"] = (uint32_t)gNTPAnchorId;
  newAnchor["epochAtSync"] = (uint32_t)now;
  newAnchor["millisAtSync"] = (uint32_t)currentMillis;

  String out;
  serializeJson(doc, out);
  writeTextAtomic(BOOT_ANCHORS_FILE, out);
}

// ============================================================================
// User System Command Registry (System-Specific)
// ============================================================================

static const char* cmd_serialrequireauth(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.serialRequireAuth ? "[Serial] Require auth: enabled" : "[Serial] Require auth: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.serialRequireAuth, true);
    return "[Serial] Require auth enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.serialRequireAuth, false);
    return "[Serial] Require auth disabled - serial commands bypass login";
  }
  return "Error: invalid arguments — Usage: serialrequireauth [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry userSystemCommands[] = {
  // Authentication commands
  { "login", "Login: <user> <pass> [transport]", false, cmd_login, "Usage: login <username> <password> [transport]\nTransport: serial (default), display, bluetooth" },
  { "logout", "Logout [transport]", false, cmd_logout, "Usage: logout [transport]\nTransport: serial (default), display, bluetooth, g2" },
  { "serialrequireauth", "Enable/disable serial auth requirement [on|off].", true, cmd_serialrequireauth, "Usage: serialrequireauth [on|off]" },
  
  // User management commands
  { "userapprove", "Approve pending request: <username>", true, cmd_user_approve, "Usage: userapprove <username>" },
  { "userdeny", "Deny pending request: <username>", true, cmd_user_deny, "Usage: userdeny <username>" },
  { "userpromote", "Promote to admin: <username>", true, cmd_user_promote, "Usage: userpromote <username>" },
  { "userdemote", "Demote from admin: <username>", true, cmd_user_demote, "Usage: userdemote <username>" },
  { "userdelete", "Delete user: <username>", true, cmd_user_delete, "Usage: userdelete <username>" },
  { "userchangepassword", "Change own password: <currentPass> <newPass> <confirmPass>", false, cmd_user_changepassword, "Usage: userchangepassword <currentPassword> <newPassword> <confirmPassword>" },
  { "userresetpassword", "Reset user password: <username> <newPassword> [0|1]", true, cmd_user_resetpassword,
    "Usage: userresetpassword <username> <newPassword> [0|1]\nOptional: 1 = require password change on next login" },
  { "useradd", "Create user: <username> <password> [0|1]", true, cmd_user_add, "Usage: useradd <username> <password> [0|1]\nOptional: 1 = require new password on next login, 0 = omit" },
  { "userlist", "List all users. (add 'json' for JSON output)", true, cmd_user_list },
  { "userrequest", "Request account: <user> <pass> [confirm]", false, cmd_user_request, "Usage: userrequest <username> <password> [confirmPassword]" },
  { "usersync", "Sync a user to another device over ESP-NOW. (async; result only on the target device - check its userlist)", true, cmd_user_sync,
    "Usage: usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>\n\n       Returns OK on delivery; the user is created on the TARGET device (no confirmation returns here) - verify on that device's userlist."
    "  targetAdminUser/targetAdminPass = an admin account on the RECEIVING device (validated there).\n"
    "  yourAdminPass = your admin password on THIS device; userPass = the synced user's password." },

  // Session management commands
  { "pendinglist", "List pending user requests. (add 'json' for JSON output)", true, cmd_pending_list },
  { "sessionlist", "List active sessions. (add 'json' for JSON output)", true, cmd_session_list },
  { "sessionrevoke", "Revoke session: <sid|user> [reason]", true, cmd_session_revoke, "Usage:\n  sessionrevoke sid <sid> [reason]\n  sessionrevoke user <username> [reason]" },

  // IP ban management
  { "ban",      "Permanently ban an IP: <ip> [reason]",      true, cmd_ban,      "Usage: ban <ip> [reason]\nBlocks all access from the IP until manually unbanned." },
  { "unban",    "Remove an IP ban: <ip>",                    true, cmd_unban,    "Usage: unban <ip>" },
  { "banlist",  "List all banned IPs. (add 'json' for JSON output)",                      true, cmd_banlist },
  { "banuser",  "Permanently ban a user account: <username> [reason]", true, cmd_banuser,  "Usage: banuser <username> [reason]\nPrevents the account from logging in until manually unbanned." },
  { "unbanuser","Remove a user account ban: <username>",     true, cmd_unbanuser,"Usage: unbanuser <username>" }
};

const size_t userSystemCommandsCount = sizeof(userSystemCommands) / sizeof(userSystemCommands[0]);

// ============================================================================
// Command Registration (System-Specific)
// ============================================================================
// Direct static registration to avoid macro issues
// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Boot Sequence Management
// ============================================================================

// Increment NTP anchor ID counter (memory-only, resets on power cycle)
void loadAndIncrementBootSeq() {
  // Boot counter now lives in NVS (its own flash partition), NOT users.json.
  // This was the ONLY reason users.json used to be rewritten every boot; moving
  // it here makes users.json a write-rarely file (real account edits only), so a
  // power cut can no longer destroy the auth database via the counter bump.
  gBootCounter = bootStateIncrementBootCount();

  // NTP anchor id is derived (read-only) from the highest id in the dedicated
  // anchors file — also decoupled from users.json.
  gNTPAnchorId = highestBootAnchorId();

  // Temporarily enable DEBUG_SYSTEM for boot sequence initialization (runs before settings loaded)
  DebugFlagMask _dbgSaved = getDebugFlags();
  setDebugFlag(DEBUG_SYSTEM);
  DEBUG_SYSTEMF("BootSeqInit: filesystemReady=%d, users.json exists=%d, bootCounter=%lu, maxAnchorId=%lu",
                (int)filesystemReady,
                (int)(filesystemReady && VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.bootSeq"))),
                (unsigned long)gBootCounter, (unsigned long)gNTPAnchorId);

  // Read-only integrity pass over users.json: if it's corrupt, quarantine it so
  // first-time setup re-arms cleanly (otherwise every login fails silently with
  // no recovery path, since serial itself requires auth). No write-back on the
  // healthy path — the file is never modified here.
  if (filesystemReady && VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.bootSeq"))) {
    File file = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.bootSeq"));
    if (!file) {
      ERROR_SYSTEMF("BootSeqInit: Failed to open users.json");
    } else {
      PSRAM_JSON_DOC(doc);
      DeserializationError error = deserializeJson(doc, file);
      file.close();

      if (error) {
        ERROR_SYSTEMF("BootSeqInit: Failed to parse users.json");
        String bad = String(USERS_JSON_FILE) + ".bad";
        VFS::removeGuarded(bad.c_str(), VFS::systemAuth("user.bootSeq"));
        bool q = VFS::renameGuarded(USERS_JSON_FILE, bad.c_str(), VFS::systemAuth("user.bootSeq"));
        logSystemEvent("USERS", "users.json corrupt at boot (%s) — %s; first-time setup will re-arm",
                       error.c_str(), q ? "quarantined to users.json.bad" : "quarantine rename FAILED");
      } else {
        DEBUG_NTP_ANCHORF("BootSeqInit: users.json parsed OK");
      }
    }
  }

  gNTPAnchorId++;
  // Restore debug flags
  setDebugFlags(_dbgSaved);
  // Use DEBUG macro - now safe since debug system is initialized
  DEBUG_NTP_ANCHORF("[BOOT] NTP anchor ID: %lu (from %s) | Boot counter: %lu (NVS)",
                    (unsigned long)gNTPAnchorId, BOOT_ANCHORS_FILE, (unsigned long)gBootCounter);
}

// ============================================================================
// User Sync Command (merged from System_User_Sync.cpp)
// ============================================================================
// ESP-NOW user credential propagation - allows admin to sync users across devices

#if ENABLE_ESPNOW

#include "System_ESPNow.h"

extern uint32_t generateMessageId();
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
const char* cmd_user_sync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  // Check if ESP-NOW is enabled
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized";
  }
  
  // Check if user sync is enabled
  if (!gSettings.espnowUserSyncEnabled) {
    return "Error: User sync disabled - enable with 'espnowusersync on'";
  }
  
  // Parse command args
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(6)) {
    return "Error: invalid arguments — Usage: usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>";
  }

  // Auth mirrors remote command execution (espnow remote): the RECEIVING device
  // authenticates an admin from ITS OWN user store, so we transmit the TARGET
  // device's admin username+password. Fields are grouped logically — the synced
  // account + its password, then the device, then the target-admin + its
  // password, then your re-auth; the audit redactor (redactUserSyncCmd) masks
  // the three password tokens by position.
  String username      = a.arg(0);  // the user being synced
  String userPass      = a.arg(1);  // the synced user's password
  String deviceStr     = a.arg(2);  // target device (name or MAC)
  String recvAdminUser = a.arg(3);  // admin that exists on the TARGET device
  String recvAdminPass = a.arg(4);  // the TARGET admin's password
  String myAdminPass   = a.arg(5);  // this device's admin password (local re-auth)
  
  // Verify user exists locally
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId)) {
    snprintf(getDebugBuffer(), 1024, "Error: User '%s' not found", username.c_str());
    return getDebugBuffer();
  }

  // Verify provided password matches the selected user locally
  if (!isValidUser(username, userPass)) {
    snprintf(getDebugBuffer(), 1024, "Error: Password incorrect for user '%s'", username.c_str());
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
  String adminUser = currentAuthContext().user;
  
  if (adminUser.length() == 0) {
    return "Error: Not authenticated - admin login required";
  }
  
  // Verify admin privileges
  if (!isAdminUser(adminUser)) {
    return "Error: Admin privileges required for user sync";
  }
  // Re-confirm the admin password was re-entered (sensitive cross-device push of
  // a credential), and that the target device's admin credentials were provided.
  if (!isValidUser(adminUser, myAdminPass)) {
    return "Error: Your admin password is incorrect";
  }
  if (recvAdminUser.length() == 0 || recvAdminPass.length() == 0) {
    return "Error: Target device admin username and password required";
  }
  
  INFO_USERF("[USER_SYNC] Syncing user '%s' (role=%s) to device '%s'", 
             username.c_str(), role.c_str(), deviceName.c_str());
  
  // Build USER_SYNC message
  PSRAM_JSON_DOC(doc);
  uint32_t msgId = generateMessageId();
  
  // Get device name for source
  String myDeviceName = gSettings.espnowDeviceName;
  if (myDeviceName.length() == 0) {
    myDeviceName = "unknown";
  }
  
  // Build JSON payload (no V2 envelope, just the sync data)
  JsonObject payload = doc.to<JsonObject>();
  payload["recv_admin_user"] = recvAdminUser;
  payload["recv_admin_pass"] = recvAdminPass;
  payload["target_user"] = username;
  payload["target_pass"] = userPass;
  payload["role"] = role;
  payload["src_device"] = myDeviceName;
  payload["dst_device"] = deviceName;
  
  // Serialize to string
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  // Send via V4 binary protocol
  extern bool v4_send_user_sync(const uint8_t* dst, const char* jsonPayload, uint16_t jsonLen);
  if (!v4_send_user_sync(targetMac, jsonStr.c_str(), jsonStr.length())) {
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
