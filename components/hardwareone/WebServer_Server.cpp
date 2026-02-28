/**
 * Web Server - HTTP server session and authentication functions
 * 
 * This file contains HTTP session management functions used by web handlers.
 */

#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER

#include <Arduino.h>
#include <ArduinoJson.h>
#include <arpa/inet.h>
#include <LittleFS.h>
#include <mbedtls/base64.h>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>

#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Icons.h"
#include "System_Logging.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_UserSettings.h"
#include "System_Utils.h"
#include "WebServer_Utils.h"
#include "WebServer_Server.h"
#include "WebPage_Automations.h"
#include "WebPage_CLI.h"
#include "WebPage_Dashboard.h"
#include "WebPage_Files.h"
#include "WebPage_Login.h"
#include "WebPage_LoginSuccess.h"
#include "WebPage_LoginRequired.h"
#include "WebPage_Logging.h"
#include "WebPage_Settings.h"
#include "System_EdgeImpulse.h"
#include "System_ESPSR.h"
#include "System_Filesystem.h"
#include "System_VFS.h"
#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#endif
#if ENABLE_WIFI && ENABLE_MQTT
#include "System_MQTT.h"
#endif
#if ENABLE_BLUETOOTH
#include "Optional_Bluetooth.h"
#endif

#if ENABLE_WEB_BLUETOOTH
#include "WebPage_Bluetooth.h"
#endif
#if ENABLE_WEB_SPEECH
#include "WebPage_Speech.h"
#endif
#if ENABLE_WEB_ESPNOW
#include "WebPage_ESPNow.h"
#endif
#if ENABLE_WEB_BOND
#include "WebPage_Bond.h"
#endif
#if ENABLE_WEB_MQTT
#include "WebPage_MQTT.h"
#endif
#if ENABLE_WEB_GAMES
#include "WebPage_Games.h"
#endif
#if ENABLE_WEB_MAPS
#include "WebPage_Maps.h"
#endif
#if ENABLE_WEB_SENSORS
#include "WebPage_Sensors.h"
#endif

// Filesystem ready flag (defined in System_Filesystem.cpp)
extern bool filesystemReady;

// External dependencies from .ino
extern httpd_handle_t server;
extern void streamCommonCSS(httpd_req_t* req);
extern void streamDebugRecord(size_t bytes, size_t chunkSize);
extern void streamDebugFlush();

// Navigation generators moved to WebServer_Utils.cpp
#include "WebServer_Utils.h"

// ============================================================================
// SSE Helper Functions (moved from .ino)
// ============================================================================

void sseEnqueueNotice(SessionEntry& s, const String& msg);  // Forward declaration

void broadcastNoticeToAllSessions(const String& message) {
  if (!gSessions) return;
  DEBUG_SSEF("Broadcasting notice to all sessions: %s", message.c_str());
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (gSessions[i].sid.length() > 0) {
      sseEnqueueNotice(gSessions[i], message);
      DEBUG_SSEF("Enqueued notice for session %d (user: %s) qCount=%d", i, gSessions[i].user.c_str(), gSessions[i].nqCount);
    }
  }
}

void broadcastEventToAllSessions(const char* eventName, const char* jsonData) {
  if (!gSessions || !eventName || !jsonData) return;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (gSessions[i].sid.length() > 0) {
      sseEnqueueEvent(gSessions[i], eventName, jsonData);
    }
  }
}

void sendSSEBurstToSession(int sessionIndex, const String& eventData) {
  if (sessionIndex < 0 || sessionIndex >= MAX_SESSIONS) return;
  if (gSessions[sessionIndex].sid.length() == 0) return;

  DEBUG_SSEF("Would send SSE burst to session %d: %s...", sessionIndex, eventData.substring(0, 50).c_str());
  gSessions[sessionIndex].needsStatusUpdate = true;
}

// ============================================================================
// Global Variables - MUST BE FIRST, before any functions
// ============================================================================

// Session Management Globals
String gSessUser;
SessionEntry* gSessions = nullptr;

// Auth cache for high-frequency endpoints
struct AuthCache {
  String sessionId;
  String user;
  unsigned long validUntil;
  String ip;
};
AuthCache gAuthCache = { "", "", 0, "" };

// Logout reason tracking
LogoutReason* gLogoutReasons = nullptr;

// Boot ID for session versioning - changes on each reboot
String gBootId = "";

// Session TTL
const unsigned long SESSION_TTL_MS = 24UL * 60UL * 60UL * 1000UL;  // 24h

// Basic Auth Globals (Legacy - kept for compatibility)
String gAuthUser = "admin";
String gAuthPass = "admin";
String gExpectedAuthHeader = "";  // e.g., "Basic dXNlcjpwYXNz"

// JSON response buffer (shared across web modules)
char* gJsonResponseBuffer = nullptr;

// ============================================================================
// External Dependencies and Forward Declarations
// ============================================================================

// Memory allocation

// ============================================================================
// Session Management Functions
// ============================================================================

// getClientIP moved to WebCore_Utils.cpp

// Session lookup
int findSessionIndexBySID(const String& sid) {
  if (sid.length() == 0) return -1;
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    if (gSessions[i].sid == sid) return i;
  }
  return -1;
}

int findFreeSessionIndex() {
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    if (gSessions[i].sid.length() == 0) return i;
  }
  // No free slot: evict the oldest or expired
  int oldest = -1;
  unsigned long tOld = 0xFFFFFFFFUL;
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    if (gSessions[i].expiresAt == 0 || (long)(gSessions[i].expiresAt - tOld) < 0) {
      oldest = i;
      tOld = gSessions[i].expiresAt;
    }
  }
  return oldest;
}

void pruneExpiredSessions() {
  static unsigned long lastPrune = 0;
  unsigned long now = millis();

  // Only prune every 30 seconds to reduce overhead
  if (now - lastPrune < 30000) return;
  lastPrune = now;

  for (int i = 0; i < MAX_SESSIONS; ++i) {
    if (gSessions[i].sid.length() && gSessions[i].expiresAt > 0 && (long)(now - gSessions[i].expiresAt) >= 0) {
      gSessions[i] = SessionEntry();
    }
  }
}

// Cookie/token helpers and makeSessToken moved to WebCore_Utils.cpp

// Session creation
String setSession(httpd_req_t* req, const String& u) {
  pruneExpiredSessions();

  // Clear auth cache to prevent stale authentication
  gAuthCache = { "", "", 0, "" };

  // Get current client IP to avoid storing logout reason for same IP
  String currentIP;
  getClientIP(req, currentIP);
  unsigned long nowMs = millis();

  // If a valid session already exists for this user from the same IP, reuse it
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    if (gSessions[i].sid.length() > 0 && gSessions[i].user == u) {
      // Validate not expired and not revoked
      if (!(gSessions[i].expiresAt > 0 && (long)(nowMs - gSessions[i].expiresAt) >= 0) && !gSessions[i].revoked) {
        if (gSessions[i].ip == currentIP) {
          // Refresh and reuse existing session
          gSessions[i].lastSeen = nowMs;
          gSessions[i].expiresAt = nowMs + SESSION_TTL_MS;
          char cookieBuf[96];
          snprintf(cookieBuf, sizeof(cookieBuf), "session=%s; Path=/", gSessions[i].sid.c_str());
          esp_err_t sc = httpd_resp_set_hdr(req, "Set-Cookie", cookieBuf);
          DEBUG_AUTHF("Reusing existing session idx=%d user=%s sid=%s | refreshed", i, u.c_str(), gSessions[i].sid.c_str());
          BROADCAST_PRINTF("[auth] reusedSession user=%s, sid=%s, exp(ms)=%lu", u.c_str(), gSessions[i].sid.c_str(), gSessions[i].expiresAt);
          DEBUG_AUTHF("Set-Cookie (reuse) rc=%d: %s", (int)sc, cookieBuf);
          return gSessions[i].sid;
        }
      }
    }
  }

  // Enforce 1 session per user limit - immediately clear any existing sessions for this user
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    if (gSessions[i].sid.length() > 0 && gSessions[i].user == u) {
      // Found existing session for this user - only store logout reason if different IP
      if (gSessions[i].ip.length() > 0 && gSessions[i].ip != currentIP) {
        storeLogoutReason(gSessions[i].ip, "You were signed out because you logged in from another device.");
      }
      BROADCAST_PRINTF("[auth] Clearing existing session for user: %s (session limit enforcement)", u.c_str());
      if (gSessions[i].sockfd >= 0) {
        httpd_sess_trigger_close(server, gSessions[i].sockfd);
      }
      gSessions[i] = SessionEntry();  // Clear immediately
    }
  }

  int idx = findFreeSessionIndex();
  if (idx < 0) idx = 0;  // fallback
  SessionEntry s;
  s.sid = makeSessToken();
  s.user = u;
  s.bootId = gBootId;  // Store current boot ID for version checking
  s.createdAt = millis();
  s.lastSeen = s.createdAt;
  s.expiresAt = s.createdAt + SESSION_TTL_MS;

  // Debug: Log session creation with boot ID
  DEBUG_AUTHF("Creating session for user '%s' with bootId '%s' (current: '%s')",
              u.c_str(), s.bootId.c_str(), gBootId.c_str());
  String ip;
  getClientIP(req, ip);
  s.ip = ip;
  s.sockfd = httpd_req_to_sockfd(req);  // Store socket descriptor for force disconnect
  gSessions[idx] = s;
  // New session should reconcile UI immediately on next SSE ping
  gSessions[idx].needsStatusUpdate = true;
  gSessions[idx].lastSensorSeqSent = 0;
  DEBUG_AUTHF("New session created idx=%d user=%s sid=%s | needsStatusUpdate=1", idx, u.c_str(), s.sid.c_str());

  // Set new session cookie with minimal attributes for maximum compatibility
  char cookieBuf[96];
  snprintf(cookieBuf, sizeof(cookieBuf), "session=%s; Path=/", s.sid.c_str());
  esp_err_t sc = httpd_resp_set_hdr(req, "Set-Cookie", cookieBuf);
  DEBUG_AUTHF("Setting session cookie: %s", cookieBuf);
  DEBUG_AUTHF("Set-Cookie rc=%d", (int)sc);

  BROADCAST_PRINTF("[auth] setSession user=%s, sid=%s, exp(ms)=%lu", u.c_str(), s.sid.c_str(), s.expiresAt);
  return s.sid;
}

// ============================================================================
// Unified Authentication Success Handler (moved from .ino)
// TODO: This function handles both HTTP and Serial auth flows. Consider
// decoupling the Serial auth handling to a separate module (serial_auth.cpp)
// to better separate web concerns from serial CLI concerns.
// ============================================================================

// External dependencies for authSuccessUnified
extern bool gSerialAuthed;
extern String gSerialUser;
extern bool appendLineWithCap(const char* path, const String& line, size_t capBytes);
extern void streamLoginSuccessContent(httpd_req_t* req, const String& sid, const String& theme);
extern "C" void __attribute__((weak)) authSuccessDebug(const char* user,
                                                       const char* ip,
                                                       const char* path,
                                                       const char* sid,
                                                       const char* redirect,
                                                       bool reused) {
  // Default no-op; override in test harness or instrumentation module
}

esp_err_t authSuccessUnified(AuthContext& ctx, const char* redirectTo) {
  // Timestamp prefix with ms precision
  char tsPrefix[40];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
  char prefixFallback[48];
  if (!tsPrefix[0]) snprintf(prefixFallback, sizeof(prefixFallback), "[BOOT ms=%lu] | ", (unsigned long)millis());
  const char* prefix = tsPrefix[0] ? tsPrefix : prefixFallback;

  bool reused = false;
  String sidShort;

  if (ctx.transport == SOURCE_WEB) {
    httpd_req_t* req = reinterpret_cast<httpd_req_t*>(ctx.opaque);
    if (req) {
      // Create or reuse session (Safari-safe cookie path inside setSession)
      String sid = setSession(req, ctx.user);
      ctx.sid = sid;
      if (sid.length() > 8) sidShort = sid.substring(0, 8) + "...";
      else sidShort = sid;
      // Heuristic: if session exists for same user+IP, setSession reuses; we mark reused
      // We can't cheaply detect here without additional API; leave 'reused' default false.
      if (ctx.ip.length() == 0) getClientIP(req, ctx.ip);
    }
  } else if (ctx.transport == SOURCE_SERIAL) {
    // Establish serial-side session state
    gSerialAuthed = true;
    if (ctx.user.length()) gSerialUser = ctx.user;  // keep provided name
    if (!gSerialUser.length()) gSerialUser = "serial";
    // Admin decision left to existing logic; do not forcibly elevate here
    sidShort = "serial";
    if (ctx.ip.length() == 0) ctx.ip = "local";
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    // Establish local display session state
    extern bool gLocalDisplayAuthed;
    extern String gLocalDisplayUser;
    gLocalDisplayAuthed = true;
    if (ctx.user.length()) gLocalDisplayUser = ctx.user;
    if (!gLocalDisplayUser.length()) gLocalDisplayUser = "display";
    sidShort = "display";
    if (ctx.ip.length() == 0) ctx.ip = "local";
  } else {
    // Internal/system commands
    sidShort = "internal";
    if (ctx.ip.length() == 0) ctx.ip = "local";
  }

  // Log unified success entry
  String transportStr;
  switch (ctx.transport) {
    case SOURCE_WEB: transportStr = "http"; break;
    case SOURCE_SERIAL: transportStr = "serial"; break;
    case SOURCE_LOCAL_DISPLAY: transportStr = "display"; break;
    case SOURCE_ESPNOW: transportStr = "espnow"; break;
    case SOURCE_MQTT: transportStr = "mqtt"; break;
    default: transportStr = "internal"; break;
  }
  
  char logLine[384];
  snprintf(logLine, sizeof(logLine),
           "%sms=%lu event=auth_success user=%s ip=%s path=%s sid=%s transport=%s reused=%d redirect=%s",
           prefix, (unsigned long)millis(),
           ctx.user.length() ? ctx.user.c_str() : "<unknown>",
           ctx.ip.length() ? ctx.ip.c_str() : "<none>",
           ctx.path.length() ? ctx.path.c_str() : "<none>",
           sidShort.length() ? sidShort.c_str() : "<none>",
           transportStr.c_str(),
           reused ? 1 : 0,
           redirectTo ? redirectTo : "<none>");
  appendLineWithCap(LOG_OK_FILE, logLine, LOG_CAP_BYTES);

  // Weak hook for external instrumentation
  authSuccessDebug(ctx.user.c_str(), ctx.ip.c_str(), ctx.path.c_str(), ctx.sid.c_str(), redirectTo ? redirectTo : "", reused);

  // Emit transport-specific success UX
  if (ctx.transport == SOURCE_WEB) {
    httpd_req_t* req = reinterpret_cast<httpd_req_t*>(ctx.opaque);
    if (!req) return ESP_FAIL;
    // Use streaming success page (web_login_success.h), with meta refresh
    // Session ID passed for cookie setting; redirect handled inside the page implementation
    {
      String _theme = "light";
      uint32_t _uid = 0;
      if (getUserIdByUsername(ctx.user, _uid) && _uid > 0) {
        JsonDocument _s;
        if (loadUserSettings(_uid, _s)) { const char* _t = _s["theme"] | "light"; if (_t && strcmp(_t,"dark")==0) _theme = "dark"; }
      }
      streamLoginSuccessContent(req, ctx.sid, _theme);
    }
    return ESP_OK;
  } else if (ctx.transport == SOURCE_SERIAL) {
    DEBUG_HTTPF("OK: logged in (Serial transport)");
    return ESP_OK;
  } else {
    // Internal/ESP-NOW commands - no UI response needed
    return ESP_OK;
  }
}

// Session destruction
void clearSession(httpd_req_t* req, const char* logoutReason) {
  // Store logout reason for user's IP before clearing session
  if (logoutReason && logoutReason[0] != '\0') {
    char ipBuf[64];
    getClientIP(req, ipBuf, sizeof(ipBuf));
    if (ipBuf[0] != '-' && ipBuf[0] != '\0') {
      storeLogoutReason(ipBuf, logoutReason);
    }
  }

  // Revoke current session by cookie value
  String sid = getCookieSID(req);
  int idx = findSessionIndexBySID(sid);
  if (idx >= 0) { gSessions[idx] = SessionEntry(); }
  // Clear session cookie client-side
  httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  broadcastOutput("[auth] clearSession (revoked current if present)");
}

// Authentication check
bool isAuthed(httpd_req_t* req, String& outUser) {
  // httpd_req_t::uri is a fixed-size char array and never NULL; only guard req itself.
  const char* uri = req ? req->uri : "(null)";
  pruneExpiredSessions();
  String sid = getCookieSID(req);
  char ipBuf[64];
  getClientIP(req, ipBuf, sizeof(ipBuf));

  if (sid.length() == 0) {
    BROADCAST_PRINTF("[auth] no session cookie for uri=%.*s", 120, uri);
    return false;
  }

  int idx = findSessionIndexBySID(sid);
  if (idx < 0) {
    BROADCAST_PRINTF("[auth] unknown SID for uri=%.*s", 120, uri);

    // Rate limit session debug messages per IP
    static char lastDebugIP[64] = "";
    static unsigned long lastDebugTime = 0;
    unsigned long now = millis();

    if (strcmp(ipBuf, lastDebugIP) != 0 || (now - lastDebugTime) > 5000) {
      DEBUG_AUTHF("No session found for SID, current boot ID: %s", gBootId.c_str());

      // If someone has a session cookie but we have no sessions, it's likely due to a reboot
      // BUT: don't overwrite an existing logout reason (e.g., from intentional logout)
      if (sid.length() > 0 && !hasLogoutReason(ipBuf)) {
        DEBUG_AUTHF("Client has session cookie but no sessions exist - likely system restart");
        storeLogoutReason(ipBuf, "Your session expired due to a system restart. Please log in again.");
      }

      strncpy(lastDebugIP, ipBuf, sizeof(lastDebugIP) - 1);
      lastDebugIP[sizeof(lastDebugIP) - 1] = '\0';
      lastDebugTime = now;
    } else {
      // Still store logout reason but don't spam debug logs
      // BUT: don't overwrite an existing logout reason (e.g., from intentional logout)
      if (sid.length() > 0 && !hasLogoutReason(ipBuf)) {
        storeLogoutReason(ipBuf, "Your session expired due to a system restart. Please log in again.");
      }
    }

    return false;
  }

  // Check if session was cleared (sockfd = -1 indicates cleared session)
  if (gSessions[idx].sid.length() == 0) {
    BROADCAST_PRINTF("[auth] cleared session for uri=%.*s", 120, uri);
    return false;
  }

  // Check if session is from a previous boot (boot ID mismatch)
  // Rate limit boot validation debug messages per IP
  static char lastBootDebugIP[64] = "";
  static unsigned long lastBootDebugTime = 0;
  unsigned long bootNow = millis();

  if (strcmp(ipBuf, lastBootDebugIP) != 0 || (bootNow - lastBootDebugTime) > 5000) {
    DEBUG_AUTHF("Validating session: user='%s', sessionBootId='%s', currentBootId='%s'",
                gSessions[idx].user.c_str(), gSessions[idx].bootId.c_str(), gBootId.c_str());
    strncpy(lastBootDebugIP, ipBuf, sizeof(lastBootDebugIP) - 1);
    lastBootDebugIP[sizeof(lastBootDebugIP) - 1] = '\0';
    lastBootDebugTime = bootNow;
  }

  if (gSessions[idx].bootId != gBootId) {
    if (strcmp(ipBuf, lastBootDebugIP) == 0 && (bootNow - lastBootDebugTime) < 1000) {
      DEBUG_AUTHF("BOOT ID MISMATCH! Session from previous boot. Storing restart message.");
    }
    BROADCAST_PRINTF("[auth] session from previous boot for uri=%.*s", 120, uri);
    // Don't overwrite an existing logout reason (e.g., from intentional logout)
    if (!hasLogoutReason(ipBuf)) {
      storeLogoutReason(ipBuf, "Your session expired due to a system restart. Please log in again.");
    }
    // Clear the stale session
    gSessions[idx] = SessionEntry();
    return false;
  } else {
    if (strcmp(ipBuf, lastBootDebugIP) == 0 && (bootNow - lastBootDebugTime) < 1000) {
      DEBUG_AUTHF("Boot ID matches - session is valid for current boot");
    }
  }

  // Check if session was revoked
  if (gSessions[idx].revoked) {
    BROADCAST_PRINTF("[auth] revoked session for uri=%.*s", 120, uri);
    return false;
  }

  unsigned long now = millis();
  if (gSessions[idx].expiresAt > 0 && (long)(now - gSessions[idx].expiresAt) >= 0) {
    // expired
    gSessions[idx] = SessionEntry();
    BROADCAST_PRINTF("[auth] expired SID for uri=%.*s", 120, uri);
    return false;
  }

  // refresh
  gSessions[idx].lastSeen = now;
  gSessions[idx].expiresAt = now + SESSION_TTL_MS;
  outUser = gSessions[idx].user;
  return true;
}

// Cached auth check for high-frequency endpoints (sensors)
bool isAuthedCached(httpd_req_t* req, String& outUser) {
  String ip;
  getClientIP(req, ip);
  String sid = getCookieSID(req);
  unsigned long now = millis();

  // Check cache first (valid for 30 seconds)
  if (gAuthCache.sessionId == sid && gAuthCache.ip == ip && now < gAuthCache.validUntil && gAuthCache.sessionId.length() > 0) {
    outUser = gAuthCache.user;
    return true;
  }

  // Full auth check on cache miss
  bool result = isAuthed(req, outUser);
  if (result) {
    gAuthCache.sessionId = sid;
    gAuthCache.user = outUser;
    gAuthCache.validUntil = now + 30000;  // Cache for 30 seconds
    gAuthCache.ip = ip;
  } else {
    // Clear cache on auth failure
    gAuthCache = { "", "", 0, "" };
  }
  return result;
}

// isAdminUser moved to user_system.cpp

// Build JSON for all sessions (admin view)
void buildAllSessionsJson(const String& currentSid, JsonArray& sessions) {
  // Convert boot millis to epoch millis for display
  time_t now = time(nullptr);
  unsigned long currentMillis = millis();
  int64_t epochMillis = 0;
  if (now > 0) {
    epochMillis = (int64_t)now * 1000LL - (int64_t)currentMillis;
  }
  
  // Build JSON array directly (no String allocation)
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    const struct SessionEntry& s = gSessions[i];
    if (!s.sid.length()) continue;
    
    JsonObject session = sessions.add<JsonObject>();
    session["sid"] = s.sid;
    session["user"] = s.user;
    // Convert boot-relative millis to epoch millis for JavaScript Date()
    session["createdAt"] = (epochMillis > 0) ? (epochMillis + (int64_t)s.createdAt) : s.createdAt;
    session["lastSeen"] = (epochMillis > 0) ? (epochMillis + (int64_t)s.lastSeen) : s.lastSeen;
    session["expiresAt"] = (epochMillis > 0) ? (epochMillis + (int64_t)s.expiresAt) : s.expiresAt;
    session["ip"] = s.ip.length() ? s.ip : "-";
    session["current"] = (s.sid == currentSid);
  }
}

// Store logout reason for IP address (with rate limiting)
void storeLogoutReason(const String& ip, const String& reason) {
  if (ip.length() == 0 || reason.length() == 0) return;

  unsigned long now = millis();
  int idx = -1;

  // Find existing entry for this IP
  for (int i = 0; i < MAX_LOGOUT_REASONS; i++) {
    if (gLogoutReasons[i].ip == ip) {
      idx = i;
      // Rate limiting: don't store same reason within 5 seconds
      if (gLogoutReasons[i].reason == reason && (now - gLogoutReasons[i].timestamp) < 5000) {
        return;  // Skip storing duplicate reason too soon
      }
      break;
    }
  }

  // Find empty slot if no existing entry
  if (idx == -1) {
    for (int i = 0; i < MAX_LOGOUT_REASONS; i++) {
      if (gLogoutReasons[i].ip.length() == 0) {
        idx = i;
        break;
      }
    }
  }

  // If no slot found, use oldest entry
  if (idx == -1) {
    unsigned long oldest = now;
    for (int i = 0; i < MAX_LOGOUT_REASONS; i++) {
      if (gLogoutReasons[i].timestamp < oldest) {
        oldest = gLogoutReasons[i].timestamp;
        idx = i;
      }
    }
  }

  // Store the logout reason
  gLogoutReasons[idx].ip = ip;
  gLogoutReasons[idx].reason = reason;
  gLogoutReasons[idx].timestamp = now;

  // Debug: Log logout reason storage
  DEBUG_AUTHF("Stored logout reason for IP '%s': '%s'", ip.c_str(), reason.c_str());
}

// Check if a logout reason already exists for this IP (without clearing it)
bool hasLogoutReason(const char* ip) {
  if (!ip) return false;
  unsigned long now = millis();
  for (int i = 0; i < MAX_LOGOUT_REASONS; ++i) {
    if (gLogoutReasons[i].ip.length() > 0 && gLogoutReasons[i].ip.equals(ip)) {
      // Check if reason has expired (30 seconds)
      if (now - gLogoutReasons[i].timestamp > 30000) {
        continue;  // Expired, doesn't count
      }
      return true;  // Valid logout reason exists
    }
  }
  return false;
}

// Helper: enqueue a targeted revoke notice for a specific session index.
// Marks session for revocation and sends notice to client for popup/redirect.
// Session will be cleared on next auth check or after notice delivery.
void enqueueTargetedRevokeForSessionIdx(int idx, const String& reasonMsg) {
  if (idx < 0 || idx >= MAX_SESSIONS) return;
  if (gSessions[idx].sid.length() == 0) return;
  String msg = String("[revoke] ") + (reasonMsg.length() ? reasonMsg : String("Your session has been signed out by an administrator."));

  // Mark session as revoked but keep it alive for notice delivery
  gSessions[idx].revoked = true;
  // Set grace period for SSE delivery (30 seconds from now)
  gSessions[idx].expiresAt = millis() + 30000UL;

  // Send SSE notice while session still exists
  sseEnqueueNotice(gSessions[idx], msg);
}

// ============================================================================
// HTTP Page Handlers (moved from .ino to fix Arduino preprocessor issues)
// ============================================================================

// External dependencies from .ino
// tgRequireAuth is now in user_system.h (included above)
extern void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*));
extern void streamSensorsContent(httpd_req_t* req);
extern bool filesystemReady;
extern void* ps_alloc(size_t size, AllocPref pref, const char* tag);
#if ENABLE_AUTOMATION
extern bool sanitizeAutomationsJson(String& json);
extern void writeAutomationsJsonAtomic(const String& json);
extern const char* AUTOMATIONS_JSON_FILE;
extern bool gAutosDirty;
#endif
extern bool readText(const char* path, String& out);
extern const char* buildSensorStatusJson();
// gSensorStatusSeq declared below
extern void streamDashboardContent(httpd_req_t* req);
extern void streamSettingsContent(httpd_req_t* req);
// Note: gJsonResponseMutex is now in mutex_system.h
extern char* gJsonResponseBuffer;
extern void buildSettingsJsonDoc(JsonDocument& doc, bool excludeWifiPasswords);
extern bool isAdminUser(const String& username);
extern void ensureDeviceRegistryFile();
extern String getCookieSID(httpd_req_t* req);
extern void buildAllSessionsJson(const String& currentSid, JsonArray& sessions);
extern bool isAuthed(httpd_req_t* req, String& userOut);
extern volatile uint32_t gOutputFlags;

// Auth handler dependencies
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);
extern void streamChunk(httpd_req_t* req, const char* chunk);
extern String getLogoutReason(const String& ip);
extern String getLogoutReasonForAuthPage(httpd_req_t* req);
extern bool isValidUser(const String& username, const String& password);
extern String extractFormField(const String& body, const String& key);
extern String urlDecode(const String& str);
extern String gSessUser;
extern void streamLoginSuccessContent(httpd_req_t* req, const String& sid, const String& theme);
extern bool executeUnifiedWebCommand(httpd_req_t* req, AuthContext& ctx, const String& cmdline, String& out);
extern int findSessionIndexBySID(const String& sid);
extern bool sseDequeueNotice(SessionEntry& s, String& out);
extern String jsonEscape(const String& in);
// gWebMirror defined in WebServer_Utils.h
extern unsigned long gWebMirrorSeq;
extern void buildSystemInfoJson(JsonDocument& doc);
extern void appendCommandToFeed(const char* origin, const String& cmd, const String& user, const String& ip);
// Command forward declaration
enum CommandOrigin { ORIGIN_SERIAL,
                     ORIGIN_WEB,
                     ORIGIN_AUTOMATION,
                     ORIGIN_SYSTEM };
enum CmdOutputMask { CMD_OUT_SERIAL = 1 << 0,
                     CMD_OUT_WEB = 1 << 1,
                     CMD_OUT_LOG = 1 << 2,
                     CMD_OUT_BROADCAST = 1 << 3 };
struct CommandContext {
  CommandOrigin origin;
  AuthContext auth;
  uint32_t id;
  uint32_t timestampMs;
  uint32_t outputMask;
  bool validateOnly;
  void* replyHandle;
  httpd_req_t* httpReq;
};
struct Command {
  String line;
  CommandContext ctx;
};
extern bool submitAndExecuteSync(const Command& uc, String& out);
extern bool gMeshActivitySuspended;
// gBroadcastSkipSessionIdx declared in web_server.h
extern void streamCLIContent(httpd_req_t* req);
extern void streamAutomationsContent(httpd_req_t* req);
extern void streamFilesContent(httpd_req_t* req);
extern void streamLoggingContent(httpd_req_t* req);
// Streaming debug helpers from .ino
extern void streamDebugReset(const char* tag);
extern void streamDebugFlush();

// Universal page streaming function (moved from .ino)
void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*)) {
  httpd_resp_set_type(req, "text/html");

  // Suspend mesh activity during page generation to reduce CPU contention
  gMeshActivitySuspended = true;

  // Streaming debug: reset per-request counters
  streamDebugReset(activePage.c_str());

  // HTTP debug: page route entry
  DEBUG_HTTPF("page_enter tag=%s user=%s", activePage.c_str(), username.c_str());

  // Stream page-specific content only - no navigation wrapper
  if (contentStreamer) {
    contentStreamer(req);
  }

  // End chunked response
  httpd_resp_send_chunk(req, NULL, 0);

  // Streaming debug: print summary for this response
  streamDebugFlush();

  // HTTP debug: page route exit (totals captured by streamDebugFlush)
  DEBUG_HTTPF("page_exit tag=%s", activePage.c_str());

  // Resume mesh activity after page is sent
  gMeshActivitySuspended = false;
}

// Use DEBUG_*F macros from debug_system.h

// Output flags now centralized in debug_system.h (OUTPUT_SERIAL/OUTPUT_DISPLAY/OUTPUT_WEB)

// Command origin and output masks are now in enum above

// Settings now included from settings.h
// Note: gJsonResponseMutex and FsLockGuard are now in mutex_system.h

// Sensor status sequence declared in header (volatile)

// Utility functions (extractFormField and urlDecode declared above)
// Logging helpers (defined in .ino)
extern bool appendLineWithCap(const char* path, const String& line, size_t capBytes);
extern void getTimestampPrefixMsCached(char* out, size_t outSize);
extern void broadcastOutput(const String& s, const CommandContext& ctx);

// handleSensorsPage moved to web_sensors.cpp

// ============================================================================
// SSE helpers exported for use by other modules (moved from .ino)
// ============================================================================

bool sseSessionAliveAndRefresh(int sessIdx, const String& sid) {
  if (sessIdx < 0 || sessIdx >= MAX_SESSIONS) {
    DEBUG_SSEF("Invalid session index: %d", sessIdx);
    return false;
  }
  if (gSessions[sessIdx].sid != sid || sid.length() == 0) {
    DEBUG_SSEF("Session SID mismatch or empty - stored: %s... provided: %s",
               gSessions[sessIdx].sid.substring(0, 8).c_str(),
               (sid.length() ? (sid.substring(0, 8) + "...").c_str() : "<none>"));
    return false;
  }
  if (gSessions[sessIdx].sid.length() == 0) {
    DEBUG_SSEF("Session was revoked/cleared - terminating SSE");
    return false;
  }
  unsigned long now = millis();
  if (gSessions[sessIdx].revoked) {
    if (gSessions[sessIdx].expiresAt > 0 && (long)(now - gSessions[sessIdx].expiresAt) >= 0) {
      DEBUG_SSEF("Revoked session grace period expired - terminating SSE");
      return false;
    }
    return true;
  }
  if (gSessions[sessIdx].expiresAt > 0 && (long)(now - gSessions[sessIdx].expiresAt) >= 0) {
    DEBUG_SSEF("Session expired - terminating SSE");
    return false;
  }
  gSessions[sessIdx].lastSeen = now;
  gSessions[sessIdx].expiresAt = now + SESSION_TTL_MS;
  static unsigned long lastDbg = 0;
  if ((long)(now - lastDbg) >= 30000) {
    DEBUG_SSEF("session refreshed; next exp=%lu", gSessions[sessIdx].expiresAt);
    lastDbg = now;
  }
  return true;
}

bool sseSendLogs(httpd_req_t* req, unsigned long seq, const String& buf) {
  // Only send the last N lines to avoid huge payloads blocking the UI
  const int MAX_LINES = 200;
  int linesFound = 0;
  int startIdx = buf.length();
  while (startIdx > 0 && linesFound <= MAX_LINES) {
    int prev = buf.lastIndexOf('\n', startIdx - 1);
    if (prev < 0) { startIdx = 0; break; }
    startIdx = prev;
    linesFound++;
  }
  if (linesFound > MAX_LINES && startIdx < (int)buf.length()) {
    startIdx = startIdx + 1;
  } else if (startIdx < 0) {
    startIdx = 0;
  }

  String out;
  out.reserve(64 + (buf.length() - startIdx));
  out += "id: "; out += String(seq); out += "\n";
  out += "event: logs\n";
  int start = startIdx;
  int lines = 0;
  while (start < (int)buf.length()) {
    int nl = buf.indexOf('\n', start);
    if (nl < 0) nl = buf.length();
    out += "data: ";
    out += buf.substring(start, nl);
    out += "\n";
    lines++;
    start = nl + 1;
  }
  out += "\n";
  esp_err_t r = httpd_resp_send_chunk(req, out.c_str(), out.length());
  DEBUG_SSEF("sendLogs: seq=%lu, lines=%d %s", seq, lines, (r == ESP_OK ? "OK" : "FAIL"));
  return r == ESP_OK;
}

// ============================================================================
// Basic Auth Helper Functions
// ============================================================================

// Decode basic auth header and extract user/pass
bool decodeBasicAuth(httpd_req_t* req, String& userOut, String& passOut, bool& headerPresent) {
  headerPresent = false;
  size_t bufLen = httpd_req_get_hdr_value_len(req, "Authorization");
  if (bufLen == 0) return false;
  bufLen++;
  char* buf = (char*)malloc(bufLen);
  if (!buf) return false;
  if (httpd_req_get_hdr_value_str(req, "Authorization", buf, bufLen) != ESP_OK) {
    free(buf);
    return false;
  }
  headerPresent = true;
  String header(buf);
  free(buf);
  
  // Expect: "Basic base64"
  if (!header.startsWith("Basic ")) return false;
  
  // Fast path: compare raw header string to precomputed expected
  if (gExpectedAuthHeader.length() && header == gExpectedAuthHeader) {
    userOut = gAuthUser;
    passOut = gAuthPass;
    return true;
  }
  
  String b64 = header.substring(6);
  b64.trim();
  
  // Decode base64
  size_t out_len = 0;
  unsigned char out_buf[256];
  int ret = mbedtls_base64_decode(out_buf, sizeof(out_buf), &out_len,
                                  (const unsigned char*)b64.c_str(), b64.length());
  if (ret != 0 || out_len == 0) return false;
  
  String decoded = String((const char*)out_buf);
  int colon = decoded.indexOf(':');
  if (colon <= 0) return false;
  
  userOut = decoded.substring(0, colon);
  passOut = decoded.substring(colon + 1);
  return true;
}

// Build the expected Authorization header for fast comparisons
void rebuildExpectedAuthHeader() {
  String creds = gAuthUser + ":" + gAuthPass;
  size_t in_len = creds.length();
  size_t out_len = 0;
  unsigned char out_buf[256];
  
  if (in_len > 180) {
    gExpectedAuthHeader = "";
    return;
  }
  
  if (mbedtls_base64_encode(out_buf, sizeof(out_buf), &out_len,
                            (const unsigned char*)creds.c_str(), in_len) == 0
      && out_len > 0) {
    String b64 = String((const char*)out_buf);
    gExpectedAuthHeader = String("Basic ") + b64;
  } else {
    gExpectedAuthHeader = "";
  }
}

// Send basic auth required response
void sendAuthRequired(httpd_req_t* req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
  const char* msg = "Authentication required";
  httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

// Redirect helper for UI pages: send user to /login (no next param)
void redirectToLogin(httpd_req_t* req) {
  const char* uri = req ? req->uri : "/";
  const char* loc = "/login";
  // Limit URI length in logs to avoid format-truncation warnings
  DEBUG_AUTHF("[auth] redirectToLogin: uri=%.96s, loc=%s", uri, loc);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", loc);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

// ============================================================================
// Logout Reason Helpers
// ============================================================================

// Get and clear logout reason for IP address
String getLogoutReason(const String& ip) {
  unsigned long now = millis();
  for (int i = 0; i < MAX_LOGOUT_REASONS; ++i) {
    if (gLogoutReasons[i].ip == ip && gLogoutReasons[i].ip.length() > 0) {
      // Check if reason has expired (30 seconds)
      if (now - gLogoutReasons[i].timestamp > 30000) {
        gLogoutReasons[i] = LogoutReason();  // Clear expired reason
        continue;
      }

      String reason = gLogoutReasons[i].reason;
      DEBUG_AUTHF("Retrieved logout reason for IP '%s': '%s'", ip.c_str(), reason.c_str());
      return reason;
    }
  }

  DEBUG_AUTHF("No logout reason found for IP '%s'", ip.c_str());
  return String();
}

// Function called from auth required page to get logout reason
String getLogoutReasonForAuthPage(httpd_req_t* req) {
  String logoutReason = "";
  String clientIP;
  getClientIP(req, clientIP);
  if (clientIP.length() > 0) {
    logoutReason = getLogoutReason(clientIP);
    DEBUG_AUTHF("Login page for IP='%s' logout reason='%s'", clientIP.c_str(), logoutReason.c_str());
  }

  // Fallback to URL parameters if no stored reason
  if (logoutReason.length() == 0 && req) {
    String uri = String(req->uri);
    int reasonPos = uri.indexOf("reason=");
    if (reasonPos >= 0) {
      reasonPos += 7;  // Skip "reason="
      int endPos = uri.indexOf('&', reasonPos);
      if (endPos < 0) endPos = uri.length();
      logoutReason = uri.substring(reasonPos, endPos);
      // URL decode basic characters
      logoutReason.replace("%20", " ");
      logoutReason.replace("%21", "!");
      logoutReason.replace("%2E", ".");
    }
  }

  return logoutReason;
}

// ============================================================================
// System Info Builder
// ============================================================================

// Helper: Build system info JSON using ArduinoJson (eliminates String concatenation)
void buildSystemInfoJson(JsonDocument& doc) {
  // Uptime
  unsigned long uptimeMs = millis();
  unsigned long seconds = uptimeMs / 1000UL;
  unsigned long minutes = seconds / 60UL;
  unsigned long hours = minutes / 60UL;
  char uptimeHms[32];
  snprintf(uptimeHms, sizeof(uptimeHms), "%luh %lum %lus", hours, minutes % 60UL, seconds % 60UL);
  doc["uptime_hms"] = uptimeHms;
  
  // Network info
  JsonObject net = doc["net"].to<JsonObject>();
  if (WiFi.isConnected()) {
    net["ssid"] = WiFi.SSID();
    net["ip"] = WiFi.localIP().toString();
    net["rssi"] = WiFi.RSSI();
    net["channel"] = WiFi.channel();
    net["mac"] = WiFi.macAddress();
  } else {
    net["ssid"] = "";
    net["ip"] = "";
    net["rssi"] = 0;
    net["channel"] = 0;
    net["mac"] = WiFi.macAddress();
  }
  
  // Memory info (nested object with KB values)
  JsonObject mem = doc["mem"].to<JsonObject>();
  mem["heap_free_kb"] = (int)(ESP.getFreeHeap() / 1024);
  mem["heap_total_kb"] = (int)(ESP.getHeapSize() / 1024);
  mem["psram_total_kb"] = (int)(ESP.getPsramSize() / 1024);
  mem["psram_free_kb"] = (int)(ESP.getFreePsram() / 1024);
  
  // Storage info (nested object with KB values)
  JsonObject storage = doc["storage"].to<JsonObject>();
  {
    uint64_t totalBytes = 0, usedBytes = 0, freeBytes = 0;
    VFS::getStats(VFS::INTERNAL, totalBytes, usedBytes, freeBytes);
    storage["total_kb"] = (int)(totalBytes / 1024);
    storage["used_kb"] = (int)(usedBytes / 1024);
    storage["free_kb"] = (int)(freeBytes / 1024);
  }
  if (VFS::isSDAvailable()) {
    uint64_t sdTotal = 0, sdUsed = 0, sdFree = 0;
    if (VFS::getStats(VFS::SDCARD, sdTotal, sdUsed, sdFree)) {
      JsonObject sd = storage["sd"].to<JsonObject>();
      sd["total_mb"] = (int)(sdTotal / (1024 * 1024));
      sd["used_mb"] = (int)(sdUsed / (1024 * 1024));
      sd["free_mb"] = (int)(sdFree / (1024 * 1024));
    }
  }

  // Connectivity status
  JsonObject conn = doc["connectivity"].to<JsonObject>();

#if ENABLE_ESPNOW
  {
    JsonObject espnow = conn["espnow"].to<JsonObject>();
    espnow["enabled"] = gSettings.espnowenabled;
    espnow["running"] = (gEspNow && gEspNow->initialized);
    espnow["mesh"] = gSettings.espnowmesh;
    espnow["deviceName"] = gSettings.espnowDeviceName;
    espnow["encrypted"] = (gEspNow && gEspNow->encryptionEnabled);
    espnow["passphraseSet"] = (gSettings.espnowPassphrase.length() > 0);
#if ENABLE_BONDED_MODE
    JsonObject bond = conn["bond"].to<JsonObject>();
    bond["enabled"] = gSettings.bondModeEnabled;
    bond["role"] = (int)gSettings.bondRole;
    bond["online"] = isBondModeOnline();
    bond["synced"] = isBondSynced();
    bond["peer"] = gSettings.bondPeerMac;
#endif
  }
#endif

#if ENABLE_WIFI && ENABLE_MQTT
  {
    JsonObject mqtt = conn["mqtt"].to<JsonObject>();
    mqtt["enabled"] = gSettings.mqttAutoStart;
    mqtt["connected"] = isMqttConnected();
    mqtt["host"] = gSettings.mqttHost;
  }
#endif

#if ENABLE_BLUETOOTH
  {
    JsonObject bt = conn["bluetooth"].to<JsonObject>();
    bt["running"] = isBLERunning();
    bt["state"] = getBLEStateString();
  }
#endif
}

// ============================================================================
// Auth Logging
// ============================================================================

// Centralized auth attempt logging (moved from .ino)
// Only logs to file for actual login events, not all auth attempts
void logAuthAttempt(bool success, const char* path, const String& userTried, const String& ip, const String& reason) {
  // Normalize path for checking
  String cleanPath = String(path ? path : "");
  cleanPath.replace("%2F", "/");
  cleanPath.replace("%20", " ");
  
  // Only log to file if this is an actual login event
  // Login events: /login, serial/login, or reason contains "Login successful"
  bool isLoginEvent = (cleanPath.indexOf("/login") >= 0) || 
                      (cleanPath.indexOf("serial/login") >= 0) ||
                      (reason.indexOf("Login successful") >= 0);
  
  if (!isLoginEvent) {
    // Not a login event - skip file logging (command audit handles command tracking)
    return;
  }

  char tsPrefix[40];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
  String status = success ? "SUCCESS" : "FAILED";

  String cleanIP = ip;
  cleanIP.replace("::FFFF:", "");

  // Format: [ts] | STATUS | user=.. | ip=.. | /path [| reason=..]
  String line;
  line.reserve(160);
  if (tsPrefix[0]) line += tsPrefix;  // already includes trailing " | "
  line += status;
  line += " | user="; line += userTried;
  line += " | ip=";   line += cleanIP;
  line += " | ";      line += cleanPath;
  if (reason.length()) { line += " | reason="; line += reason; }

  const char* logFile = success ? LOG_OK_FILE : LOG_FAIL_FILE;
  appendLineWithCap(logFile, line, LOG_CAP_BYTES);
}

// Streaming content for Dashboard page (moved from .ino)
void streamDashboardContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  // Begin streamed HTML shell (nav + content wrapper)
  streamBeginHtml(req, "HardwareOne - Minimal", /*isPublic=*/false, u, "dashboard");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  // Delegate inner streaming to web_dashboard.h
  streamDashboardInner(req, u);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

// Streaming content for Settings page (moved from .ino)
void streamSettingsContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Settings", false, u, "settings");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamSettingsInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

void streamFilesContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Files", false, u, "files");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamFilesInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

void streamLoggingContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Logging", false, u, "logging");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamLoggingInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

void streamAutomationsContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Automations", false, u, "automations");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamAutomationsInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

void streamCLIContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "CLI", false, u, "cli");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamCLIInner(req, u);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

// Read raw file contents as text/plain
esp_err_t handleFileRead(httpd_req_t* req) {
  DEBUG_STORAGEF("[handleFileRead] START");
  
  // Pause sensor polling during file streaming to prevent I2C contention
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  
  AuthContext ctx = makeWebAuthCtx(req);
  DEBUG_STORAGEF("[handleFileRead] Auth check for user from IP: %s", ctx.ip.c_str());
  if (!tgRequireAuth(ctx)) {
    WARN_SESSIONF("File read auth failed");
    gSensorPollingPaused = wasPaused;
    return ESP_OK;
  }  DEBUG_STORAGEF("[handleFileRead] Auth SUCCESS for user: %s", ctx.user.c_str());

  if (!filesystemReady) {
    ERROR_STORAGEF("Filesystem not ready");
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Filesystem not initialized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    WARN_WEBF("No query string in file read request");
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "No filename specified", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  DEBUG_STORAGEF("[handleFileRead] Query string: %s", query);

  char name[160];
  if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
    WARN_WEBF("No 'name' parameter in file read query");
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Invalid filename", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  DEBUG_STORAGEF("[handleFileRead] Raw name parameter: %s", name);

  String path = String(name);
  path.replace("%2F", "/");
  path.replace("%20", " ");
  DEBUG_STORAGEF("[handleFileRead] Decoded path: %s", path.c_str());

  FsLockGuard fsGuard("handleFileRead");

  File f = VFS::open(path, "r");
  if (!f) {
    WARN_STORAGEF("File not found: %s", path.c_str());
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "File not found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  size_t fileSize = f.size();
  DEBUG_STORAGEF("[handleFileRead] File opened successfully, size: %d bytes", fileSize);

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  char buf[512];
  size_t totalSent = 0;
  int chunkCount = 0;
  while (true) {
    size_t n = f.readBytes(buf, sizeof(buf));
    if (n == 0) break;
    chunkCount++;
    totalSent += n;
    DEBUG_STORAGEF("[handleFileRead] Chunk %d: read %d bytes, total sent: %d", chunkCount, n, totalSent);
    httpd_resp_send_chunk(req, buf, n);
  }
  f.close();
  httpd_resp_send_chunk(req, NULL, 0);
  DEBUG_STORAGEF("[handleFileRead] COMPLETE: Sent %d bytes in %d chunks", totalSent, chunkCount);
  
  // Resume sensor polling
  gSensorPollingPaused = wasPaused;
  return ESP_OK;
}

// Write file via x-www-form-urlencoded: name=<path>&content=<urlencoded text>
esp_err_t handleFileWrite(httpd_req_t* req) {
  DEBUG_STORAGEF("[handleFileWrite] START");
  AuthContext ctx = makeWebAuthCtx(req);
  DEBUG_STORAGEF("[handleFileWrite] Auth check for user from IP: %s", ctx.ip.c_str());
  if (!tgRequireAuth(ctx)) {
    WARN_SESSIONF("File write auth failed");
    return ESP_OK;
  }  DEBUG_STORAGEF("[handleFileWrite] Auth SUCCESS for user: %s", ctx.user.c_str());

  if (!filesystemReady) {
    ERROR_STORAGEF("Filesystem not ready");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Filesystem not initialized\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Read body (form-urlencoded)
  size_t contentLen = req->content_len;
  DEBUG_STORAGEF("[handleFileWrite] Content-Length: %d bytes", contentLen);
  if (contentLen == 0 || contentLen > 150 * 1024) {
    ERROR_WEBF("Invalid content length: %d", contentLen);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid content length\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char* body = (char*)ps_alloc(contentLen + 1, AllocPref::PreferPSRAM, "http.upload.body");
  if (!body) {
    ERROR_MEMORYF("Failed to allocate %d bytes for upload", contentLen + 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"OOM\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  DEBUG_STORAGEF("[handleFileWrite] Allocated %d bytes for body", contentLen + 1);

  // Read the body in chunks (httpd_req_recv may not read everything at once)
  size_t totalReceived = 0;
  int recvAttempts = 0;
  while (totalReceived < contentLen) {
    recvAttempts++;
    int ret = httpd_req_recv(req, body + totalReceived, contentLen - totalReceived);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        DEBUG_STORAGEF("[handleFileWrite] Timeout on recv attempt %d, retrying...", recvAttempts);
        continue;  // Retry on timeout
      }
      ERROR_WEBF("recv failed with code %d after %d attempts", ret, recvAttempts);
      free(body);
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, "{\"success\":false,\"error\":\"Error receiving data\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    totalReceived += ret;
    DEBUG_STORAGEF("[handleFileWrite] Recv attempt %d: got %d bytes, total: %d/%d", recvAttempts, ret, totalReceived, contentLen);
  }
  body[totalReceived] = '\0';
  DEBUG_STORAGEF("[handleFileWrite] Received complete body: %d bytes in %d attempts", totalReceived, recvAttempts);

  String s = String(body);
  free(body);
  DEBUG_STORAGEF("[handleFileWrite] Converted to String, length: %d", s.length());

  auto getParam = [&](const char* key) -> String {
    String k = String(key) + "=";
    int p = s.indexOf(k);
    if (p < 0) return String("");
    p += k.length();
    int e = s.indexOf('&', p);
    if (e < 0) e = s.length();
    String v = s.substring(p, e);
    // URL decode common characters
    v.replace("+", " ");
    v.replace("%20", " ");
    v.replace("%0A", "\n");
    v.replace("%0D", "\r");
    v.replace("%2F", "/");
    v.replace("%3A", ":");
    v.replace("%2C", ",");
    v.replace("%7B", "{");
    v.replace("%7D", "}");
    v.replace("%22", "\"");
    v.replace("%5B", "[");
    v.replace("%5D", "]");
    v.replace("%25", "%");  // Keep this last to avoid double-decoding
    return v;
  };

  String name = getParam("name");
  String content = getParam("content");
  DEBUG_STORAGEF("[handleFileWrite] Parsed params - name: '%s', content length: %d", name.c_str(), content.length());

  if (name.length() == 0) {
    ERROR_WEBF("No name parameter in file write");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Name required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Use centralized permission check (also block .bin firmware files)
  if (name.endsWith(".bin") || !canEdit(name)) {
    WARN_STORAGEF("Protected path write attempt: %s", name.c_str());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Writes to this path are not allowed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  DEBUG_STORAGEF("[handleFileWrite] Opening file for write: %s", name.c_str());

  FsLockGuard fsGuard("handleFileWrite");

  File f = VFS::open(name, "w", true);
  if (!f) {
    ERROR_STORAGEF("Failed to open file for write: %s", name.c_str());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Open failed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  DEBUG_STORAGEF("[handleFileWrite] File opened successfully");

  size_t left = content.length();
  size_t pos = 0;
  int writeChunks = 0;
  while (left > 0) {
    size_t chunk = left > 512 ? 512 : left;
    size_t written = f.write((const uint8_t*)content.c_str() + pos, chunk);
    writeChunks++;
    DEBUG_STORAGEF("[handleFileWrite] Write chunk %d: %d bytes (requested %d)", writeChunks, written, chunk);
    pos += chunk;
    left -= chunk;
  }
  f.close();
  DEBUG_STORAGEF("[handleFileWrite] File closed, wrote %d bytes in %d chunks", content.length(), writeChunks);

#if ENABLE_AUTOMATION
  // Post-save hooks for specific files
  if (name == "/system/automations.json") {
    DEBUG_STORAGEF("[handleFileWrite] Automations.json detected, running post-save hooks");
    // Read back and sanitize duplicate IDs; persist atomically if changed
    String json;
    if (readText(AUTOMATIONS_JSON_FILE, json)) {
      DEBUG_STORAGEF("[handleFileWrite] Read back automations.json: %d bytes", json.length());
      if (sanitizeAutomationsJson(json)) {
        DEBUG_STORAGEF("[handleFileWrite] Sanitization needed, writing atomic");
        writeAutomationsJsonAtomic(json);  // best-effort atomic writeback
        gAutosDirty = true;                // ensure scheduler refreshes
        DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] Sanitized duplicate IDs after file write; scheduler refresh queued");
      } else {
        DEBUG_STORAGEF("[handleFileWrite] No sanitization needed");
      }
    } else {
      DEBUG_STORAGEF("[handleFileWrite] WARNING: Failed to read back automations.json");
    }
  }
#endif

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  DEBUG_STORAGEF("[handleFileWrite] COMPLETE: Success");
  return ESP_OK;
}

// Upload file via x-www-form-urlencoded (streamed): path=<fullpath>&binary=<0|1>&content=<urlencoded or base64-urlencoded>
esp_err_t handleFileUpload(httpd_req_t* req) {
  DEBUG_STORAGEF("[handleFileUpload] START (streaming)");
  uint32_t tStart = millis();
  uint32_t heapStart = ESP.getFreeHeap();
  
  // Pause sensor polling during file upload to prevent I2C timeouts
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  DEBUG_STORAGEF("[handleFileUpload] Sensor polling paused for upload");

  AuthContext ctx = makeWebAuthCtx(req);
  DEBUG_STORAGEF("[handleFileUpload] Auth check for user from IP: %s", ctx.ip.c_str());
  if (!tgRequireAuth(ctx)) {
    WARN_SESSIONF("File upload auth failed");
    gSensorPollingPaused = wasPaused;
    return ESP_OK;
  }
  DEBUG_STORAGEF("[handleFileUpload] Auth SUCCESS for user: %s", ctx.user.c_str());

  if (!filesystemReady) {
    DEBUG_STORAGEF("[handleFileUpload] ERROR: Filesystem not ready");
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Filesystem not initialized\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Base64 encoding adds ~33% overhead, plus URL encoding. Allow up to 3.5MB on wire for ~2.5MB actual files.
  // Maps are loaded fully into PSRAM, so limit size to leave room for other allocations.
  const size_t kMaxUpload = 3584 * 1024;  // 3.5MB on wire = ~2.5MB actual
  size_t contentLen = req->content_len;

  // Check available storage space FIRST - more helpful error message
  size_t totalBytes = 0;
  size_t usedBytes = 0;
  {
    FsLockGuard fsGuard("handleFileUpload.stats");
    totalBytes = LittleFS.totalBytes();
    usedBytes = LittleFS.usedBytes();
  }
  size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
  // Estimate actual file size from content length (remove ~33% base64 overhead)
  size_t estimatedFileSize = (contentLen * 3) / 4;
  
  DEBUG_STORAGEF("[handleFileUpload] Content-Length: %d bytes (est file: %d), free space: %d, heap=%u", 
                 contentLen, estimatedFileSize, freeBytes, (unsigned)ESP.getFreeHeap());
  
  // Check storage space first - give more specific error
  if (estimatedFileSize > freeBytes) {
    DEBUG_STORAGEF("[handleFileUpload] ERROR: Insufficient storage space");
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "application/json");
    char errBuf[128];
    snprintf(errBuf, sizeof(errBuf), 
             "{\"success\":false,\"error\":\"Not enough space. File ~%.1fMB, free %.1fMB\"}", 
             estimatedFileSize / (1024.0 * 1024.0), freeBytes / (1024.0 * 1024.0));
    httpd_resp_send(req, errBuf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  if (contentLen > kMaxUpload) {
    DEBUG_STORAGEF("[handleFileUpload] ERROR: Request too large");
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"File too large (max ~2.5MB)\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  enum Field { F_PATH,
               F_BINARY,
               F_CONTENT };
  Field field = F_PATH;

  // URL decode state
  int urlState = 0;  // 0=none, 1=after '%', waiting hex1
  char hexBuf[3] = { 0, 0, 0 };

  String path;
  path.reserve(64);
  bool isBinary = false;
  bool binaryKnown = false;

  // Base64 streaming decode state
  int b64val = 0;
  int b64valb = -8;
  bool b64pad = false;

  // Output buffer for writes - use PSRAM for larger buffer (better throughput)
  const size_t OUT_BUF_SIZE = 4096;  // 4KB instead of 256 bytes
  uint8_t* uploadOutBuf = (uint8_t*)ps_alloc(OUT_BUF_SIZE, AllocPref::PreferPSRAM);
  if (!uploadOutBuf) {
    DEBUG_STORAGEF("[handleFileUpload] ERROR: Failed to allocate output buffer");
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Memory allocation failed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  size_t outLen = 0;
  size_t totalWritten = 0;

  // Track free space to abort if we exceed available FS space (already computed above)
  size_t freeLimit = freeBytes;
  bool outOfSpace = false;

  File file;
  bool fileOpen = false;
  bool fsLockedForUpload = false;

  auto flushWrite = [&]() {
    if (outLen && fileOpen) {
      size_t w = file.write(uploadOutBuf, outLen);
      totalWritten += w;
      outLen = 0;
      if (totalWritten > freeLimit) {
        outOfSpace = true;
      }
    }
  };

  auto putByte = [&](uint8_t ch) {
    uploadOutBuf[outLen++] = ch;
    if (outLen >= OUT_BUF_SIZE) flushWrite();
  };

  auto urlEmit = [&](char ch) {
    if (field == F_PATH) {
      path += (char)ch;
    } else if (field == F_CONTENT) {
      // content emission depends on isBinary
      if (binaryKnown && !isBinary) {
        putByte((uint8_t)ch);
      }
    }
  };

  auto urlConsume = [&](char c) {
    if (urlState == 0) {
      if (c == '+') {
        urlEmit(' ');
      } else if (c == '%') {
        urlState = 1;
      } else {
        urlEmit(c);
      }
    } else if (urlState == 1) {
      hexBuf[0] = c;
      urlState = 2;
    } else {  // urlState == 2
      hexBuf[1] = c;
      hexBuf[2] = 0;
      char dc = (char)strtol(hexBuf, NULL, 16);
      urlEmit(dc);
      urlState = 0;
    }
  };

  auto openFileIfNeeded = [&]() -> bool {
    if (fileOpen) return true;
    // Validate path
    if (path.length() == 0) {
      DEBUG_STORAGEF("[handleFileUpload] ERROR: Empty path");
      return false;
    }
    // Block path traversal
    if (path.indexOf("..") >= 0) {
      WARN_SYSTEMF("[handleFileUpload] BLOCKED: Path traversal not allowed: %s", path.c_str());
      return false;
    }
    // Use centralized permission check
    if (!canCreate(path)) {
      WARN_SYSTEMF("[handleFileUpload] BLOCKED: Protected path: %s", path.c_str());
      return false;
    }
    // Auto-create parent directory if it doesn't exist
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
      String parentDir = path.substring(0, lastSlash);
      {
        FsLockGuard fsGuard("handleFileUpload.mkdir");
        if (!VFS::exists(parentDir)) {
          DEBUG_STORAGEF("[handleFileUpload] Creating parent directory: %s", parentDir.c_str());
          VFS::mkdir(parentDir);
        }
      }
    }
    DEBUG_STORAGEF("[handleFileUpload] Opening file for write: %s", path.c_str());

    fsLock("handleFileUpload.file");
    fsLockedForUpload = true;
    file = VFS::open(path, "w", true);
    if (!file) {
      ERROR_STORAGEF("Failed to open file for write: %s", path.c_str());
      fsUnlock();
      fsLockedForUpload = false;
      return false;
    }
    fileOpen = true;
    return true;
  };

  // Stream receive and parse - use PSRAM for larger buffer (better throughput)
  const size_t RECV_BUF_SIZE = 4096;  // 4KB instead of 512 bytes
  char* uploadRecvBuf = (char*)ps_alloc(RECV_BUF_SIZE, AllocPref::PreferPSRAM);
  if (!uploadRecvBuf) {
    free(uploadOutBuf);
    gSensorPollingPaused = wasPaused;
    DEBUG_STORAGEF("[handleFileUpload] ERROR: Failed to allocate recv buffer");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Memory allocation failed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  char* buf = uploadRecvBuf;
  size_t received = 0;
  int attempts = 0;
  size_t chunkIndex = 0;

  // Helper to consume literal prefix like "path=" / "binary=" / "content="
  const char* expect = "path=";
  size_t expectPos = 0;

  while (received < contentLen) {
    attempts++;
    int toRead = (int)min((size_t)(contentLen - received), RECV_BUF_SIZE);
    int ret = httpd_req_recv(req, buf, toRead);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
      DEBUG_STORAGEF("[handleFileUpload] Recv error %d", ret);
      if (fileOpen) file.close();
      if (fsLockedForUpload) {
        fsUnlock();
        fsLockedForUpload = false;
      }
      free(uploadOutBuf);
      free(uploadRecvBuf);
      gSensorPollingPaused = wasPaused;
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, "{\"success\":false,\"error\":\"Recv error\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    received += ret;
    chunkIndex++;
    DEBUG_STORAGEF("[handleFileUpload] Chunk %d: %d bytes (total %d/%d)", (int)chunkIndex, ret, (int)received, (int)contentLen);

    for (int i = 0; i < ret; i++) {
      // Yield every 64 bytes to prevent WDT timeout during base64 decoding
      // Use delay(0) instead of taskYIELD() to ensure IDLE task runs and feeds watchdog
      if ((i & 0x3F) == 0) {
        delay(0);
      }
      
      char c = buf[i];
      if (field != F_CONTENT) {
        // Matching expected literal until '=' consumed
        if (expect && expect[expectPos] != 0) {
          if (c == expect[expectPos]) {
            expectPos++;
            continue;
          }
        }
      }

      if (field == F_PATH) {
        if (c == '&' && urlState == 0) {
          DEBUG_STORAGEF("[handleFileUpload] PATH parsed: %s", path.c_str());
          field = F_BINARY;
          expect = "binary=";
          expectPos = 0;
          continue;
        }
        urlConsume(c);
      } else if (field == F_BINARY) {
        if (c == '=') { continue; }
        if (c == '&') {
          binaryKnown = true;
          DEBUG_STORAGEF("[handleFileUpload] BINARY parsed: %d", (int)isBinary);
          field = F_CONTENT;
          expect = "content=";
          expectPos = 0;
          continue;
        }
        if (c == '1') isBinary = true;
        else if (c == '0') isBinary = false;   // single digit expected
      } else {                                 // F_CONTENT
        if (!binaryKnown) binaryKnown = true;  // fallback
        if (!fileOpen) {
          if (!openFileIfNeeded()) {
            free(uploadOutBuf);
            free(uploadRecvBuf);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid path\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
          }
        }
        if (expect && expect[expectPos] != 0) {
          if (c == expect[expectPos]) {
            expectPos++;
            continue;
          }
          // If mismatch, assume content starts immediately after '=' in rare cases
          expect = nullptr;
        }

        if (!isBinary) {
          // URL-decode to bytes and write
          if (c == '&' && urlState == 0) {
            // End of content (unlikely, content is last). Flush and ignore rest
            flushWrite();
          } else {
            // Stream URL decoding for text
            if (urlState == 0 && c == '%') {
              urlState = 1;
            } else if (urlState == 1) {
              hexBuf[0] = c;
              urlState = 2;
            } else if (urlState == 2) {
              hexBuf[1] = c;
              hexBuf[2] = 0;
              putByte((uint8_t)strtol(hexBuf, NULL, 16));
              urlState = 0;
            } else if (c == '+') {
              putByte(' ');
            } else {
              putByte((uint8_t)c);
            }
          }
        } else {
          // URL-decode to base64 chars, then base64-decode to bytes
          char emitB64 = 0;
          bool haveEmit = false;
          if (urlState == 0) {
            if (c == '%') {
              urlState = 1;
            } else if (c == '+') {
              emitB64 = ' ';
              haveEmit = true;
            } else {
              emitB64 = c;
              haveEmit = true;
            }
          } else if (urlState == 1) {
            hexBuf[0] = c;
            urlState = 2;
          } else {  // urlState==2
            hexBuf[1] = c;
            hexBuf[2] = 0;
            emitB64 = (char)strtol(hexBuf, NULL, 16);
            haveEmit = true;
            urlState = 0;
          }
          if (haveEmit) {
            char ch = emitB64;
            if (ch == '=') { b64pad = true; }
            const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const char* p = strchr(table, ch);
            if (p || ch == '=') {
              if (ch != '=') {
                b64val = (b64val << 6) + (p - table);
                b64valb += 6;
                if (b64valb >= 0) {
                  putByte((uint8_t)((b64val >> b64valb) & 0xFF));
                  b64valb -= 8;
                }
              }
            }
          }
        }
      }
    }
    if ((chunkIndex % 8) == 0) {
      flushWrite();
      if (outOfSpace) break;
      delay(0);
    }
  }

  // Flush any remaining url-decode state for text content (nothing to emit for partial %)
  flushWrite();
  if (outOfSpace) {
    if (fileOpen) {
      file.close();
      fileOpen = false;
    }
    {
      FsLockGuard fsGuard("handleFileUpload.cleanup");
      LittleFS.remove(path);
    }
    if (fsLockedForUpload) {
      fsUnlock();
      fsLockedForUpload = false;
    }
    DEBUG_STORAGEF("[handleFileUpload] ERROR: Insufficient storage space during write (wrote %d / free %d)", (int)totalWritten, (int)freeLimit);
    free(uploadOutBuf);
    free(uploadRecvBuf);
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Insufficient storage space\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (fileOpen) file.close();
  if (fsLockedForUpload) {
    fsUnlock();
    fsLockedForUpload = false;
  }

  // Free PSRAM buffers
  free(uploadOutBuf);
  free(uploadRecvBuf);

  DEBUG_STORAGEF("[handleFileUpload] COMPLETE: wrote %d bytes to '%s' (binary=%s), heap delta=%d, dur=%u ms",
                 (int)totalWritten, path.c_str(), isBinary ? "true" : "false", (int)ESP.getFreeHeap() - (int)heapStart, (unsigned)(millis() - tStart));
  
  // Always print upload completion to serial for visibility
  Serial.printf("[UPLOAD] %s: %d bytes written in %u ms\n", path.c_str(), (int)totalWritten, (unsigned)(millis() - tStart));

#if ENABLE_AUTOMATION
  // Post-save hook: keep existing behavior for automations.json (read/validate)
  if (path == "/system/automations.json") {
    String json;
    if (readText(AUTOMATIONS_JSON_FILE, json)) {
      if (sanitizeAutomationsJson(json)) writeAutomationsJsonAtomic(json);
      else DEBUG_STORAGEF("[handleFileUpload] automations.json OK");
    }
  }
#endif

 #if ENABLE_WEB_MAPS
  // Post-save hook: auto-organize maps uploaded ANYWHERE on the filesystem
  // Detect maps by extension OR magic bytes (HWMP), move to /maps/<base>/<base>.hwmap
  // (stubs return false when ENABLE_MAPS=0)
  {
    bool isMapByExt = path.endsWith(".hwmap");
    bool isMapByMagic = false;
    if (!isMapByExt && !path.endsWith(".json")) {
      isMapByMagic = isMapFileByMagic(path);
    }
    if (isMapByExt || isMapByMagic) {
      String err;
      bool ok = organizeMapFromAnyPath(path, err);
      if (!ok) {
        DEBUG_STORAGEF("[handleFileUpload] Map organize failed for %s: %s", path.c_str(), err.c_str());
      } else {
        DEBUG_STORAGEF("[handleFileUpload] Map organized from %s", path.c_str());
      }
    }
  }

  // Post-save hook: if legacy waypoint JSON uploaded into /maps root, place it into the map folder if possible.
  // Expected legacy name: /maps/waypoints_<mapFileName>.json
  if (path.startsWith("/maps/waypoints_") && path.endsWith(".json")) {
    String fn = path;
    if (fn.startsWith("/maps/")) fn = fn.substring(6);
    if (fn.startsWith("/")) fn = fn.substring(1);
    if (fn.indexOf('/') == -1) {
      String err;
      bool ok = tryOrganizeLegacyWaypointsAtRoot(fn, err);
      if (!ok) {
        DEBUG_STORAGEF("[handleFileUpload] Waypoints organize skipped/failed for %s: %s", fn.c_str(), err.c_str());
      } else {
        DEBUG_STORAGEF("[handleFileUpload] Waypoints organized: %s", fn.c_str());
      }
    }
  }
 #endif

  // Resume sensor polling after upload completes
  gSensorPollingPaused = wasPaused;
  DEBUG_STORAGEF("[handleFileUpload] Sensor polling resumed");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// handleSensorsStatus moved to web_sensors.cpp

esp_err_t handleDashboard(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  DEBUG_HTTPF("handler enter uri=%s user=%s page=%s", ctx.path.c_str(), ctx.user.c_str(), "dashboard");
  streamPageWithContent(req, "dashboard", ctx.user, streamDashboardContent);
  return ESP_OK;
}

esp_err_t handleSettingsPage(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  DEBUG_HTTPF("handler enter uri=%s user=%s page=%s", ctx.path.c_str(), ctx.user.c_str(), "settings");
  streamPageWithContent(req, "settings", ctx.user, streamSettingsContent);
  return ESP_OK;
}

// Settings API (GET): return current settings as JSON
// OPTIMIZED: Uses static buffer with snprintf to eliminate String allocations
esp_err_t handleSettingsGet(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  // Lock shared JSON response buffer
  if (xSemaphoreTake(gJsonResponseMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  // Build response using ArduinoJson
  // Size hint: ~3584 for settings + 512 for wrapper
  JsonDocument response;
  
  // Build settings object using unified builder (passwords excluded for security)
  JsonObject settings = response["settings"].to<JsonObject>();
  JsonDocument settingsDoc;
  buildSettingsJsonDoc(settingsDoc, true);  // true = exclude WiFi passwords from web API
  settings.set(settingsDoc.as<JsonObject>());
  
  // Add response envelope
  response["success"] = true;
  
  // Add user info
  JsonObject user = response["user"].to<JsonObject>();
  user["username"] = ctx.user;
  user["isAdmin"] = isAdminUser(ctx.user);
  
  // Add features
  JsonObject features = response["features"].to<JsonObject>();
  features["adminSessions"] = isAdminUser(ctx.user);
  features["userApprovals"] = true;
  features["adminControls"] = true;
  features["sensorConfig"] = true;

  // Serialize to buffer
  size_t len = serializeJson(response, gJsonResponseBuffer, JSON_RESPONSE_SIZE);
  
  if (len == 0 || len >= JSON_RESPONSE_SIZE) {
    // Serialization failed or buffer too small
    xSemaphoreGive(gJsonResponseMutex);
    DEBUG_STORAGEF("[Settings API] JSON serialization failed or buffer overflow");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  // DEBUG: Log buffer usage
  int usagePct = (len * 100) / JSON_RESPONSE_SIZE;
  DEBUG_MEMORYF("[JSON_RESP_BUF] Settings JSON: %zu/%u bytes (%d%%)",
                len, (unsigned)JSON_RESPONSE_SIZE, usagePct);

  // Send response and release buffer
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, gJsonResponseBuffer, len);

  xSemaphoreGive(gJsonResponseMutex);
  return ESP_OK;
}

// Settings Schema API (GET): return settings metadata for dynamic UI rendering
esp_err_t handleSettingsSchema(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  // Check buffer is allocated
  if (!gJsonResponseBuffer) {
    WARN_WEBF("[handleSettingsSchema] JSON response buffer not allocated");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  // Lock shared JSON response buffer (longer timeout since this may race with settings fetch)
  if (xSemaphoreTake(gJsonResponseMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    WARN_WEBF("[handleSettingsSchema] Mutex timeout - another request holding buffer");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  // Build schema from registered settings modules
  PSRAM_JSON_DOC(doc);
  JsonArray modules = doc["modules"].to<JsonArray>();
  
  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  
  DEBUG_HTTPF("[Schema] Building schema for %zu modules", modCount);
  
  for (size_t m = 0; m < modCount; m++) {
    const SettingsModule* mod = mods[m];
    if (!mod) {
      WARN_WEBF("[Schema] Module %zu is null", m);
      continue;
    }
    
    DEBUG_HTTPF("[Schema] Processing module: %s (%zu entries)", mod->name, mod->count);
    
    JsonObject modObj = modules.add<JsonObject>();
    modObj["name"] = mod->name;
    modObj["section"] = mod->jsonSection ? mod->jsonSection : mod->name;
    modObj["description"] = mod->description ? mod->description : "";
    
    // Check connection status
    bool connected = true;
    if (mod->isConnected) {
      connected = mod->isConnected();
    }
    modObj["connected"] = connected;
    
    JsonArray entries = modObj["entries"].to<JsonArray>();
    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      if (!e || !e->jsonKey) {
        WARN_WEBF("[Schema] Module %s entry %zu has null jsonKey", mod->name, i);
        continue;
      }
      JsonObject entry = entries.add<JsonObject>();
      entry["key"] = e->jsonKey;
      entry["label"] = e->label ? e->label : e->jsonKey;
      
      // Type as string
      switch (e->type) {
        case SETTING_INT: entry["type"] = "int"; break;
        case SETTING_FLOAT: entry["type"] = "float"; break;
        case SETTING_BOOL: entry["type"] = "bool"; break;
        case SETTING_STRING: entry["type"] = "string"; break;
      }
      
      // Mark secret fields (passwords, etc.)
      if (e->isSecret) {
        entry["secret"] = true;
      }
      
      // Min/max for numeric types
      if (e->type == SETTING_INT || e->type == SETTING_FLOAT) {
        if (e->minVal != 0 || e->maxVal != 0) {
          entry["min"] = e->minVal;
          entry["max"] = e->maxVal;
        }
      }
      
      // Options for select fields
      if (e->options) {
        entry["options"] = e->options;
      }
      
      // Default value
      switch (e->type) {
        case SETTING_INT: entry["default"] = e->intDefault; break;
        case SETTING_FLOAT: entry["default"] = e->floatDefault; break;
        case SETTING_BOOL: entry["default"] = (bool)e->intDefault; break;
        case SETTING_STRING: entry["default"] = e->stringDefault ? e->stringDefault : ""; break;
      }
    }
  }
  
  doc["count"] = modCount;

  DEBUG_HTTPF("[Schema] Serializing schema JSON...");

  // Serialize to buffer
  size_t len = serializeJson(doc, gJsonResponseBuffer, JSON_RESPONSE_SIZE);
  
  DEBUG_HTTPF("[Schema] Serialized %zu bytes (buffer size: %u)", len, (unsigned)JSON_RESPONSE_SIZE);
  
  if (len == 0 || len >= JSON_RESPONSE_SIZE) {
    WARN_WEBF("[Schema] Serialization failed or buffer overflow: %zu >= %u", len, (unsigned)JSON_RESPONSE_SIZE);
    xSemaphoreGive(gJsonResponseMutex);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  DEBUG_HTTPF("[Schema] Sending schema response (%zu bytes)", len);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_send(req, gJsonResponseBuffer, len);

  xSemaphoreGive(gJsonResponseMutex);
  DEBUG_HTTPF("[Schema] Schema response sent successfully");
  return ESP_OK;
}

esp_err_t handleUserSettingsGet(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  DEBUG_HTTPF("[UserSettings] GET enter user=%s ip=%s", ctx.user.c_str(), ctx.ip.c_str());

  if (xSemaphoreTake(gJsonResponseMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  uint32_t userId = 0;
  if (!getUserIdByUsername(ctx.user, userId)) {
    DEBUG_HTTPF("[UserSettings] GET userId not found user=%s", ctx.user.c_str());
    JsonDocument response;
    response["success"] = false;
    response["error"] = "user_not_found";
    size_t len = serializeJson(response, gJsonResponseBuffer, JSON_RESPONSE_SIZE);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, gJsonResponseBuffer, len);
    xSemaphoreGive(gJsonResponseMutex);
    return ESP_OK;
  }

  JsonDocument settingsDoc;
  if (!loadUserSettings(userId, settingsDoc)) {
    DEBUG_HTTPF("[UserSettings] GET load failed user=%s userId=%u", ctx.user.c_str(), (unsigned)userId);
    JsonDocument response;
    response["success"] = false;
    response["error"] = "read_failed";
    size_t len = serializeJson(response, gJsonResponseBuffer, JSON_RESPONSE_SIZE);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, gJsonResponseBuffer, len);
    xSemaphoreGive(gJsonResponseMutex);
    return ESP_OK;
  }

  JsonDocument response;
  response["success"] = true;
  response["userId"] = userId;
  JsonObject settings = response["settings"].to<JsonObject>();
  settings.set(settingsDoc.as<JsonObject>());
  
  // Remove sensitive fields from response (passwords should never be exposed via API)
  settings.remove("password");
  settings.remove("gamepad_password");

  {
    const char* theme = settingsDoc["theme"] | "";
    DEBUG_HTTPF("[UserSettings] GET ok user=%s userId=%u theme=%s", ctx.user.c_str(), (unsigned)userId, theme);
  }

  size_t len = serializeJson(response, gJsonResponseBuffer, JSON_RESPONSE_SIZE);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_send(req, gJsonResponseBuffer, len);

  xSemaphoreGive(gJsonResponseMutex);
  return ESP_OK;
}

esp_err_t handleUserSettingsSet(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  DEBUG_HTTPF("[UserSettings] POST enter user=%s ip=%s content_len=%d", ctx.user.c_str(), ctx.ip.c_str(), (int)req->content_len);

  uint32_t userId = 0;
  if (!getUserIdByUsername(ctx.user, userId)) {
    DEBUG_HTTPF("[UserSettings] POST userId not found user=%s", ctx.user.c_str());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"user_not_found\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  int total_len = req->content_len;
  if (total_len <= 0 || total_len > 4096) {
    DEBUG_HTTPF("[UserSettings] POST invalid content_len=%d user=%s userId=%u", total_len, ctx.user.c_str(), (unsigned)userId);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"invalid_content_length\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  std::unique_ptr<char, void (*)(void*)> buf((char*)ps_alloc(total_len + 1, AllocPref::PreferPSRAM, "http.user.settings"), free);
  if (!buf) {
    ERROR_MEMORYF("OOM for user settings POST: user=%s userId=%u", ctx.user.c_str(), (unsigned)userId);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  int received = 0;
  while (received < total_len) {
    int r = httpd_req_recv(req, buf.get() + received, total_len - received);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      DEBUG_HTTPF("[UserSettings] POST recv_failed r=%d user=%s userId=%u", r, ctx.user.c_str(), (unsigned)userId);
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, "{\"success\":false,\"error\":\"recv_failed\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    received += r;
  }
  buf.get()[received] = '\0';

  JsonDocument patch;
  DeserializationError err = deserializeJson(patch, buf.get());
  if (err) {
    DEBUG_HTTPF("[UserSettings] POST invalid_json user=%s userId=%u err=%s", ctx.user.c_str(), (unsigned)userId, err.c_str());
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"invalid_json\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Reject attempts to set password fields via this API (use setUserPassword instead)
  // This prevents storing unhashed passwords
  if (patch.containsKey("password") || patch.containsKey("gamepad_password")) {
    DEBUG_HTTPF("[UserSettings] POST rejected password field user=%s userId=%u", ctx.user.c_str(), (unsigned)userId);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"password_not_allowed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  {
    const char* theme = patch["theme"] | "";
    if (theme && theme[0]) {
      DEBUG_HTTPF("[UserSettings] POST patch theme=%s user=%s userId=%u", theme, ctx.user.c_str(), (unsigned)userId);
    } else {
      DEBUG_HTTPF("[UserSettings] POST patch keys=%u user=%s userId=%u", (unsigned)patch.as<JsonObject>().size(), ctx.user.c_str(), (unsigned)userId);
    }
  }

  if (!mergeAndSaveUserSettings(userId, patch)) {
    DEBUG_HTTPF("[UserSettings] POST write_failed user=%s userId=%u", ctx.user.c_str(), (unsigned)userId);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"write_failed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  DEBUG_HTTPF("[UserSettings] POST ok user=%s userId=%u", ctx.user.c_str(), (unsigned)userId);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Device Registry API (GET): return device registry as JSON
esp_err_t handleDeviceRegistryGet(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  String regContent;
  {
    FsLockGuard guard("devices.read");
    
    // Ensure file exists (create if needed) - must be inside lock
    if (!LittleFS.exists("/system/devices.json")) {
      File f = LittleFS.open("/system/devices.json", "w");
      if (f) {
        f.println("{");
        f.println("  \"devices\": []");
        f.println("}");
        f.close();
      }
    }
    
    File file = LittleFS.open("/system/devices.json", "r");
    if (!file) {
      httpd_resp_send(req, "{\"error\":\"Could not read device registry\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    regContent = file.readString();
    file.close();
  }
  // Now send response without holding FS lock
  httpd_resp_set_type(req, "application/json");
  DEBUG_HTTPF("/api/devices len=%u", (unsigned)regContent.length());
  httpd_resp_send(req, regContent.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Build Configuration API (GET): return compile-time feature flags
esp_err_t handleBuildConfig(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600"); // Cache for 1 hour since build config doesn't change

  // Build JSON response with compile-time flags
  char json[512];
  snprintf(json, sizeof(json),
    "{\"camera\":%s,\"microphone\":%s,\"bluetooth\":%s,\"g2glasses\":%s,"
    "\"mqtt\":%s,\"espnow\":%s,\"edgeimpulse\":%s,\"espsr\":%s,"
    "\"automation\":%s,\"gps\":%s,\"imu\":%s,\"thermal\":%s,"
    "\"tof\":%s,\"gamepad\":%s,\"apds\":%s,\"fmradio\":%s,"
    "\"rtc\":%s,\"presence\":%s}",
    ENABLE_CAMERA_SENSOR    ? "true" : "false",
    ENABLE_MICROPHONE_SENSOR? "true" : "false",
    ENABLE_BLUETOOTH        ? "true" : "false",
    ENABLE_G2_GLASSES       ? "true" : "false",
    ENABLE_MQTT             ? "true" : "false",
    ENABLE_ESPNOW           ? "true" : "false",
    ENABLE_EDGE_IMPULSE     ? "true" : "false",
    ENABLE_ESP_SR           ? "true" : "false",
    ENABLE_AUTOMATION       ? "true" : "false",
    ENABLE_GPS_SENSOR       ? "true" : "false",
    ENABLE_IMU_SENSOR       ? "true" : "false",
    ENABLE_THERMAL_SENSOR   ? "true" : "false",
    ENABLE_TOF_SENSOR       ? "true" : "false",
    ENABLE_GAMEPAD_SENSOR   ? "true" : "false",
    ENABLE_APDS_SENSOR      ? "true" : "false",
    ENABLE_FM_RADIO         ? "true" : "false",
    ENABLE_RTC_SENSOR       ? "true" : "false",
    ENABLE_PRESENCE_SENSOR  ? "true" : "false");

  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t handleSessionsList(httpd_req_t* req) {
  // Admin-only: list all active sessions
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  if (!isAdminUser(ctx.user)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Admin access required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Build response using ArduinoJson (no String concatenation)
  PSRAM_JSON_DOC(doc);
  doc["success"] = true;
  JsonArray sessions = doc["sessions"].to<JsonArray>();
  
  String currentSid = getCookieSID(req);
  buildAllSessionsJson(currentSid, sessions);
  
  String response;
  serializeJson(doc, response);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Admin: list all sessions
esp_err_t handleAdminSessionsList(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  if (!isAdminUser(ctx.user)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Admin access required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  // Build response using ArduinoJson (no String concatenation)
  PSRAM_JSON_DOC(doc);
  doc["success"] = true;
  JsonArray sessions = doc["sessions"].to<JsonArray>();
  
  String currentSid = getCookieSID(req);
  buildAllSessionsJson(currentSid, sessions);
  
  String response;
  serializeJson(doc, response);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// GET /api/output -> returns persisted (gSettings) and runtime (gOutputFlags) for serial/web/display/g2
esp_err_t handleOutputGet(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  int rtSerial = (gOutputFlags & OUTPUT_SERIAL) ? 1 : 0;
  int rtWeb = (gOutputFlags & OUTPUT_WEB) ? 1 : 0;
  int rtDisplay = (gOutputFlags & OUTPUT_DISPLAY) ? 1 : 0;
  int rtG2 = (gOutputFlags & OUTPUT_G2) ? 1 : 0;
  
  char json[320];
  snprintf(json, sizeof(json),
           "{\"success\":true,\"persisted\":{\"serial\":%d,\"web\":%d,\"display\":%d,\"g2\":%d},\"runtime\":{\"serial\":%d,\"web\":%d,\"display\":%d,\"g2\":%d}}",
           gSettings.outSerial ? 1 : 0,
           gSettings.outWeb ? 1 : 0,
           gSettings.outDisplay ? 1 : 0,
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
           gSettings.outG2 ? 1 : 0,
#else
           0,
#endif
           rtSerial, rtWeb, rtDisplay, rtG2);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// POST /api/output/temp (x-www-form-urlencoded): serial=0/1&web=0/1&display=0/1
esp_err_t handleOutputTemp(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  // Read form body
  char buf[256];
  int total = 0;
  int remaining = req->content_len;
  while (remaining > 0 && total < (int)sizeof(buf) - 1) {
    int toRead = remaining;
    if (toRead > (int)sizeof(buf) - 1 - total) toRead = (int)sizeof(buf) - 1 - total;
    int r = httpd_req_recv(req, buf + total, toRead);
    if (r <= 0) break;
    total += r;
    remaining -= r;
  }
  buf[total] = '\0';

  // Parse values
  auto getVal = [&](const char* key) -> int {
    char val[8];
    if (httpd_query_key_value(buf, key, val, sizeof(val)) == ESP_OK) {
      return atoi(val);
    }
    return -1;  // not provided
  };
  int vSerial = getVal("serial");
  int vWeb = getVal("web");
  int vDisplay = getVal("display");

  // Apply to runtime flags only
  if (vSerial == 0) {
    gOutputFlags &= ~OUTPUT_SERIAL;
  } else if (vSerial == 1) {
    gOutputFlags |= OUTPUT_SERIAL;
  }

  if (vWeb == 0) {
    gOutputFlags &= ~OUTPUT_WEB;
  } else if (vWeb == 1) {
    gOutputFlags |= OUTPUT_WEB;
  }

  if (vDisplay == 0) {
    gOutputFlags &= ~OUTPUT_DISPLAY;
  } else if (vDisplay == 1) {
    gOutputFlags |= OUTPUT_DISPLAY;
  }

  // Respond with updated runtime snapshot
  int rtSerial = (gOutputFlags & OUTPUT_SERIAL) ? 1 : 0;
  int rtWeb = (gOutputFlags & OUTPUT_WEB) ? 1 : 0;
  int rtDisplay = (gOutputFlags & OUTPUT_DISPLAY) ? 1 : 0;
  
  char json[128];
  snprintf(json, sizeof(json),
           "{\"success\":true,\"runtime\":{\"serial\":%d,\"web\":%d,\"display\":%d}}",
           rtSerial, rtWeb, rtDisplay);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Notice endpoint: returns and clears per-session notice
esp_err_t handleNotice(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");

  // Check authentication manually to return JSON on failure
  String user;
  String ip;
  getClientIP(req, ip);

  if (!isAuthed(req, user)) {
    // Return 401 with JSON response instead of HTML
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  String sid = getCookieSID(req);
  int idx = findSessionIndexBySID(sid);
  String note = "";
  if (idx >= 0) {
    // Dequeue one notice from the ring if available
    String dequeued;
    if (sseDequeueNotice(gSessions[idx], dequeued)) {
      note = dequeued;
      // If this is a revoke notice, immediately clear the session and expire cookie
      if (note.startsWith("[revoke]")) {
        gSessions[idx] = SessionEntry();
        httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
      }
    }
  }
  String json = String("{\"success\":true,\"notice\":\"") + jsonEscape(note) + "\"}";
  httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Auth-protected text log endpoint returning mirrored output
esp_err_t handleLogs(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  DEBUG_HTTPF("[LOGS_DEBUG] Request from %s", ctx.ip.c_str());
  if (!tgRequireAuth(ctx)) {
    WARN_SESSIONF("Logs API auth failed");
    return ESP_OK;
  }
  DEBUG_HTTPF("[LOGS_DEBUG] Auth OK for user '%s'", ctx.user.c_str());
  httpd_resp_set_type(req, "text/plain");
  if (!gWebMirror.buf) {
    DEBUG_HTTPF("gWebMirror.buf is NULL, initializing...");
    gWebMirror.init(gWebMirrorCap);
    if (!gWebMirror.buf) {
      ERROR_WEBF("Failed to init gWebMirror!");
      httpd_resp_sendstr(req, "[ERROR] Web mirror buffer unavailable");
      return ESP_OK;
    }
  }
  // Use zero-copy snapshotTo() to avoid String allocation on every poll (500ms)
  // Allocate response buffer from PSRAM to avoid heap fragmentation
  char* responseBuf = (char*)ps_alloc(gWebMirrorCap, AllocPref::PreferPSRAM, "handleLogs.resp");
  if (!responseBuf) {
    ERROR_WEBF("Failed to allocate response buffer for logs");
    httpd_resp_sendstr(req, "[ERROR] Memory allocation failed");
    return ESP_OK;
  }
  size_t copied = gWebMirror.snapshotTo(responseBuf, gWebMirrorCap);
  DEBUG_HTTPF("[LOGS_DEBUG] Sending %zu bytes, seq=%lu", copied, gWebMirrorSeq);
  httpd_resp_send(req, responseBuf, copied);
  free(responseBuf);
  DEBUG_HTTPF("[LOGS_DEBUG] Response sent");
  return ESP_OK;
}

// Enhanced sensors status endpoint with session update checking
esp_err_t handleSensorsStatusWithUpdates(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  // Check if this session needs a status update notification
  int sessIdx = findSessionIndexBySID(getCookieSID(req));
  bool needsRefresh = false;
  if (sessIdx >= 0) {
    // Check if flagged for status update
    if (gSessions[sessIdx].needsStatusUpdate) {
      needsRefresh = true;
      DEBUG_SSEF("Session %d needs status update (reporting via /status); clearing flag", sessIdx);
    }
  }

  httpd_resp_set_type(req, "application/json");
  const char* baseJson = buildSensorStatusJson();

  size_t baseLen = strlen(baseJson);

  if (needsRefresh && baseLen > 0 && baseJson[baseLen - 1] == '}') {
    // Append needsRefresh flag using chunked transfer to avoid 1.5KB stack buffer.
    // Send everything except the closing '}', then the suffix.
    httpd_resp_send_chunk(req, baseJson, baseLen - 1);
    httpd_resp_send_chunk(req, ",\"needsRefresh\":true}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);  // End chunked response
    DEBUG_HTTPF("/api/sensors/status-updates by %s @ %s: json_len=%zu (with refresh)",
                ctx.user.c_str(), ctx.ip.c_str(), baseLen + 21);
    // Clear the flag after sending the refresh notification
    if (sessIdx >= 0) {
      gSessions[sessIdx].needsStatusUpdate = false;
    }
  } else {
    // No modification needed, send directly
    DEBUG_HTTPF("/api/sensors/status-updates by %s @ %s: json_len=%zu",
                ctx.user.c_str(), ctx.ip.c_str(), baseLen);
    httpd_resp_send(req, baseJson, baseLen);
  }
  return ESP_OK;
}

// System status endpoint for dashboard one-shot fetch
esp_err_t handleSystemStatus(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  httpd_resp_set_type(req, "application/json");

  // Build system info using ArduinoJson (eliminates String concatenation)
  PSRAM_JSON_DOC(doc);
  buildSystemInfoJson(doc);
  
  // Serialize to static buffer
  static char sysJsonBuf[1024];
  serializeJson(doc, sysJsonBuf, sizeof(sysJsonBuf));

  httpd_resp_send(req, sysJsonBuf, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t handleCLICommand(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  DEBUG_CMD_FLOWF("[web.cli] enter ip=%s content_len=%d", ctx.ip.c_str(), (int)req->content_len);
  if (!tgRequireAuth(ctx)) {
    // Security log: unauthorized CLI attempt
    logAuthAttempt(false, "/api/cli", String(), ctx.ip, "unauthorized");
    return ESP_OK;  // 401 already sent
  }

  // Rate limiting: prevent rapid command execution to avoid stack overflow
  static unsigned long lastCmdTime = 0;
  unsigned long now = millis();
  if (now - lastCmdTime < 50) {  // 50ms minimum between commands
    DEBUG_CMD_FLOWF("[web.cli] rate limited: %lums since last command", now - lastCmdTime);
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Rate limited - commands too frequent\",\"retry_after_ms\":50}");
    return ESP_OK;
  }
  lastCmdTime = now;
  // Read x-www-form-urlencoded body
  std::unique_ptr<char, void (*)(void*)> buf((char*)ps_alloc(req->content_len + 1, AllocPref::PreferPSRAM, "http.cli.exec"), free);
  int received = 0;
  String body;
  if (req->content_len > 0) {
    DEBUG_CMD_FLOWF("[web.cli] content_len=%d, starting recv loop", (int)req->content_len);
    while (received < (int)req->content_len) {
      int ret = httpd_req_recv(req, buf.get() + received, req->content_len - received);
      if (ret <= 0) break;
      received += ret;
    }
    buf.get()[received] = '\0';
    DEBUG_CMD_FLOWF("[web.cli] received=%d bytes, buf[0-79]='%.80s'", received, buf.get());
    body = String(buf.get());
    DEBUG_CMD_FLOWF("[web.cli] body.length()=%d after String conversion", body.length());
  }
  String cmdEncoded = extractFormField(body, "cmd");
  DEBUG_CMD_FLOWF("[web.cli] cmdEncoded.length()=%d after extractFormField", cmdEncoded.length());
  String cmd = urlDecode(cmdEncoded);
  DEBUG_CMD_FLOWF("[web.cli] cmd.length()=%d after urlDecode", cmd.length());
  String validateStr = extractFormField(body, "validate");
  bool doValidate = (validateStr == "1" || validateStr == "true");
  DEBUG_CMD_FLOWF("[web.cli] authed user=%s cmd_len=%d validate=%d", ctx.user.c_str(), cmd.length(), doValidate ? 1 : 0);
  DEBUG_CMD_FLOWF("[web.cli] cmd_first_80='%s'", cmd.substring(0, 80).c_str());

  // Record the command in the unified feed (skip if validation-only), then execute centrally
  if (!doValidate) {
    appendCommandToFeed("web", cmd, ctx.user, ctx.ip);
  }

  // Determine originating session index and set skip for SSE broadcast during this command
  String sidForCmd = getCookieSID(req);
  int originIdx = findSessionIndexBySID(sidForCmd);
  int prevSkip = gBroadcastSkipSessionIdx;
  gBroadcastSkipSessionIdx = originIdx;
  DEBUG_SSEF("CLI origin session idx=%d, sid=%s; will skip flagging this session on broadcast", originIdx, (sidForCmd.length() ? (sidForCmd.substring(0, 8) + "...").c_str() : "<none>"));
  DEBUG_CMD_FLOWF("[web.cli] build ctx user=%s originIdx=%d", ctx.user.c_str(), originIdx);

  // Build unified command and execute synchronously (queue to be added later)
  Command uc;
  uc.line = cmd;
  DEBUG_CMD_FLOWF("[web.cli] uc.line.length()=%d after assignment", uc.line.length());
  uc.ctx.origin = ORIGIN_WEB;
  uc.ctx.auth = ctx;
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = (uint32_t)millis();
  uc.ctx.outputMask = CMD_OUT_WEB | CMD_OUT_LOG;
  uc.ctx.validateOnly = doValidate;
  uc.ctx.replyHandle = nullptr;
  uc.ctx.httpReq = req;

  // Suspend mesh activity during command execution to reduce CPU contention
  gMeshActivitySuspended = true;
  String out;
  bool ok = submitAndExecuteSync(uc, out);
  gMeshActivitySuspended = false;
  
  // Redact sensitive data from output (passwords, session IDs)
  // This applies to CLI history, logs, and all broadcast sinks
  String redactedOut = redactOutputForLog(out);
  
  DEBUG_CMD_FLOWF("[web.cli] executed ok=%d out_len=%d", ok ? 1 : 0, out.length());
  DEBUG_CLIF("Command result: %s", redactedOut.c_str());
  if (!doValidate) {
    // Route output to configured sinks (web + log). HTTP response is sent below.
    DEBUG_CMD_FLOWF("[web.cli] routing output len=%d", redactedOut.length());
    broadcastOutput(redactedOut, uc.ctx);
  }

  // Restore skip index after broadcast side-effects complete
  gBroadcastSkipSessionIdx = prevSkip;
  DEBUG_SSEF("Restored gBroadcastSkipSessionIdx to %d", prevSkip);

  // For web CLI, return the redacted output directly for immediate display
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, redactedOut.c_str(), HTTPD_RESP_USE_STRLEN);
  DEBUG_CMD_FLOWF("[web.cli] exit");
  return ESP_OK;
}

esp_err_t handleCLIPage(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  DEBUG_HTTPF("handler enter uri=%s user=%s page=%s", ctx.path.c_str(), ctx.user.c_str(), "cli");
  streamPageWithContent(req, "cli", ctx.user, streamCLIContent);
  return ESP_OK;
}

#if ENABLE_AUTOMATION
// Automations page handler (authenticated for all users)
esp_err_t handleAutomationsPage(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  DEBUG_HTTPF("handler enter uri=%s user=%s page=%s", ctx.path.c_str(), ctx.user.c_str(), "automations");
  streamPageWithContent(req, "automations", ctx.user, streamAutomationsContent);
  return ESP_OK;
}

// GET /api/automations: return raw automations.json
esp_err_t handleAutomationsGet(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  httpd_resp_set_type(req, "application/json");
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to read automations.json\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  // Sanitize duplicate IDs if any and persist back
  if (sanitizeAutomationsJson(json)) {
    writeAutomationsJsonAtomic(json);  // best-effort writeback
  }
  httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}
#endif

esp_err_t handleFilesPage(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  streamPageWithContent(req, "files", ctx.user, streamFilesContent);
  return ESP_OK;
}

esp_err_t handleLoggingPage(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  streamPageWithContent(req, "logging", ctx.user, streamLoggingContent);
  return ESP_OK;
}

// ============================================================================
// Simple Handlers (Batch 1 - migrated from .ino)
// ============================================================================

// Root handler: redirect to dashboard
esp_err_t handleRoot(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/dashboard");
  httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Simple unauthenticated health check
esp_err_t handlePing(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Logout: clear session and redirect to login
esp_err_t handleLogout(httpd_req_t* req) {
  clearSession(req, "You have been logged out successfully.");
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/login");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "Logged out", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ============================================================================
// Auth Handlers (Batch 2 - migrated from .ino)
// ============================================================================

// Full-page Login: GET shows form, POST validates and sets cookie session
esp_err_t handleLogin(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    // If already authed, redirect directly to dashboard
    String u;
    if (isAuthed(req, u)) {
      httpd_resp_set_status(req, "303 See Other");
      httpd_resp_set_hdr(req, "Location", "/dashboard");
      httpd_resp_send(req, "", 0);
      return ESP_OK;
    }
    // Render login form (no next param) using streaming
    DEBUG_HTTPF("[LOGIN_DEBUG] Starting login page render");
    httpd_resp_set_type(req, "text/html");
    DEBUG_HTTPF("[LOGIN_DEBUG] Set content type");
    streamBeginHtml(req, "Sign In", /*isPublic=*/true, "", "login");
    DEBUG_HTTPF("[LOGIN_DEBUG] Sent HTML header");
    // Get logout reason
    String logoutReason = "";
    String clientIP;
    getClientIP(req, clientIP);
    if (clientIP.length() > 0) {
      logoutReason = getLogoutReason(clientIP);
      DEBUG_HTTPF("[LOGIN_PAGE_DEBUG] Direct login page access for IP '%s' - logout reason: '%s'", clientIP.c_str(), logoutReason.c_str());
    }
    DEBUG_HTTPF("[LOGIN_DEBUG] About to call streamLoginInner");
    streamLoginInner(req, "", "", logoutReason);
    DEBUG_HTTPF("[LOGIN_DEBUG] Called streamLoginInner");
    streamEndHtml(req);
    DEBUG_HTTPF("[LOGIN_DEBUG] Sent HTML footer, page complete");
    return ESP_OK;
  }
  // POST
  int total_len = req->content_len;
  if (total_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    return ESP_FAIL;
  }
  std::unique_ptr<char, void (*)(void*)> buf((char*)ps_alloc(total_len + 1, AllocPref::PreferPSRAM, "http.login"), free);
  int received = 0;
  while (received < total_len) {
    int r = httpd_req_recv(req, buf.get() + received, total_len - received);
    if (r <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read err");
      return ESP_FAIL;
    }
    received += r;
  }
  buf.get()[received] = '\0';
  String body(buf.get());
  String u = urlDecode(extractFormField(body, "username"));
  String p = urlDecode(extractFormField(body, "password"));
  BROADCAST_PRINTF("[login] POST attempt: username='%s', password_len=%u", u.c_str(), (unsigned)p.length());

  bool validUser = isValidUser(u, p);
  broadcastOutput(validUser ? "[login] isValidUser result: true" : "[login] isValidUser result: false");

  if (u.length() == 0 || p.length() == 0 || !validUser) {
    // Log failed authentication attempt
    String ip;
    getClientIP(req, ip);
    logAuthAttempt(false, req->uri, u, ip, "Invalid credentials");

    // Show login form with error
    httpd_resp_set_type(req, "text/html");
    streamBeginHtml(req, "Sign In", /*isPublic=*/true, "", "login");
    // Get logout reason
    String logoutReason = "";
    String clientIP2;
    getClientIP(req, clientIP2);
    if (clientIP2.length() > 0) {
      logoutReason = getLogoutReason(clientIP2);
    }
    streamLoginInner(req, u, "Invalid username or password", logoutReason);
    streamEndHtml(req);
    return ESP_OK;
  }
  // Success -> create session immediately and set cookie in this response
  broadcastOutput(String("[login] Login successful for user: ") + u);

  // Log successful authentication attempt
  String ip;
  getClientIP(req, ip);
  logAuthAttempt(true, req->uri, u, ip, "Login successful");

  // Clear auth cache immediately
  gAuthCache = { "", "", 0, "" };

  // Clear any existing logout reason for this IP to prevent false "signed out" messages
  String clientIP;
  getClientIP(req, clientIP);
  if (clientIP.length() > 0) {
    getLogoutReason(clientIP);  // This clears the reason by reading it
  }

  // Create session and capture SID for client-side fallback
  String sid = setSession(req, u);

  // Send styled login success page with Safari-compatible redirect
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");

  {
    String _theme = "light";
    uint32_t _uid = 0;
    if (getUserIdByUsername(u, _uid) && _uid > 0) {
      JsonDocument _s;
      if (loadUserSettings(_uid, _s)) { const char* _t = _s["theme"] | "light"; if (_t && strcmp(_t,"dark")==0) _theme = "dark"; }
    }
    streamLoginSuccessContent(req, sid, _theme);
  }

  broadcastOutput(String("[login] Safari-compatible session and cookie set for user: ") + u);
  return ESP_OK;
}

// Auth required page handler - shows 401 with login prompt
esp_err_t sendAuthRequiredResponse(httpd_req_t* req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  
  // Check if this is an API endpoint - send JSON for all /api/* paths
  String uri = String(req->uri);
  if (uri.startsWith("/api/")) {
    DEBUG_AUTHF("[401] API endpoint - sending JSON response");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"auth_required\",\"message\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Check if this is an AJAX/fetch request (Accept: application/json)
  char acceptBuf[128] = {0};
  if (httpd_req_get_hdr_value_str(req, "Accept", acceptBuf, sizeof(acceptBuf)) == ESP_OK) {
    String accept = String(acceptBuf);
    accept.toLowerCase();
    if (accept.indexOf("application/json") >= 0) {
      DEBUG_AUTHF("[401] Accept header requests JSON - sending JSON response");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, "{\"error\":\"auth_required\",\"reload\":true}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
  }
  
  DEBUG_AUTHF("[401] Sending HTML login page");
  httpd_resp_set_type(req, "text/html");

  // Get logout reason from main file function
  String logoutReason = getLogoutReasonForAuthPage(req);

  // Stream the page with public shell
  streamBeginHtml(req, "Authentication Required", /*isPublic=*/true, "", "auth");

  // Page content with card wrapper
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, R"AUTHREQ(
<div class='text-center pad-xl'>
  <h2>Authentication Required</h2>
)AUTHREQ",
                        HTTPD_RESP_USE_STRLEN);

  // Show logout reason if present
  if (logoutReason.length() > 0) {
    DEBUG_HTTPF("[AUTH_DEBUG] Including logout reason: %s", logoutReason.c_str());
    httpd_resp_send_chunk(req, R"LOGOUT(
  <div class='alert alert-warning mb-3' style='background:#fff3cd;border:1px solid #ffeaa7;color:#856404;padding:12px;border-radius:4px;'>
    <strong>Session Terminated:</strong> )LOGOUT",
                          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, logoutReason.c_str(), HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "\n  </div>\n", HTTPD_RESP_USE_STRLEN);
  }

  // Main content and closing
  httpd_resp_send_chunk(req, R"AUTHCONTENT(
  <p>You need to sign in to access this page.</p>
  <p class='text-sm' style='color:#fff'>Don't have an account? <a class='link-primary' href='/register' style='text-decoration:none'>Request Access</a></p>
</div>
</div>
<script>window.addEventListener('load', function(){ setTimeout(function(){ try{ var msg = sessionStorage.getItem('revokeMsg'); if(msg){ sessionStorage.removeItem('revokeMsg'); alert(msg); } }catch(_){} }, 500); });</script>
)AUTHCONTENT",
                        HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);  // Close card

  streamEndHtml(req);

  return ESP_OK;
}

// Session setter: Step 2 of login process - set fresh cookies after clearing old ones
esp_err_t handleLoginSetSession(httpd_req_t* req) {
  // Check if we have a pending session to set
  if (gSessUser.length() == 0) {
    broadcastOutput("[login] No pending session, redirecting to login");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
  }

  // Create a new session entry and set cookies
  String user = gSessUser;  // capture then clear
  setSession(req, user);
  gSessUser = "";
  broadcastOutput(String("[login] Session set for user: ") + user);

  // Send HTML page with JavaScript redirect and cookie verification
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  String html = "<!DOCTYPE html><html><head><title>Login Success</title></head><body>";
  html += "<script>";
  html += "console.log('Cookie verification page loaded');";
  html += "console.log('Document.cookie:', document.cookie);";
  html += "if(document.cookie.indexOf('session=') >= 0) {";
  html += "  console.log('Session cookie found, redirecting to dashboard');";
  html += "  window.location.href = '/dashboard';";
  html += "} else {";
  html += "  console.log('No session cookie found, waiting 1 second and retrying');";
  html += "  setTimeout(function() {";
  html += "    console.log('Retry - Document.cookie:', document.cookie);";
  html += "    if(document.cookie.indexOf('session=') >= 0) {";
  html += "      window.location.href = '/dashboard';";
  html += "    } else {";
  html += "      console.log('Cookie still not found, redirecting to login');";
  html += "      window.location.href = '/login';";
  html += "    }";
  html += "  }, 1000);";
  html += "}";
  html += "</script>";
  html += "<p>Login successful, checking session...</p>";
  html += "</body></html>";
  httpd_resp_send(req, html.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Registration page (guest)
esp_err_t handleRegisterPage(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  streamBeginHtml(req, "Request Account", /*isPublic=*/true, "", "register");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, R"REGISTER(
<div class='panel container-narrow space-top-md'>
  <div class='text-center space-bottom-sm'>
    <h2>Request Account</h2>
    <p class='text-muted' style='margin:0'>Submit your credentials for admin approval</p>
  </div>
  <form method='POST' action='/register/submit'>
    <div class='form-field'>
      <label for='username'>Username</label>
      <input type='text' id='username' name='username' class='form-input' required autofocus>
    </div>
    <div class='form-field'>
      <label for='password'>Password</label>
      <input type='password' id='password' name='password' class='form-input' required>
    </div>
    <div class='form-field'>
      <label for='confirm_password'>Confirm Password</label>
      <input type='password' id='confirm_password' name='confirm_password' class='form-input' required>
    </div>
    <div class='btn-row space-top-md'>
      <button class='btn btn-primary' type='submit'>Submit Request</button>
      <a class='btn btn-secondary' href='/login'>Back to Sign In</a>
    </div>
  </form>
</div>
)REGISTER",
                        HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
  return ESP_OK;
}

// Registration submit: use unified CLI command for consistency
esp_err_t handleRegisterSubmit(httpd_req_t* req) {
  // Parse form fields (reuse login parsing pattern)
  String body;
  int total_len = req->content_len;
  if (total_len > 0) {
    std::unique_ptr<char, void (*)(void*)> buf((char*)ps_alloc(total_len + 1, AllocPref::PreferPSRAM, "http.reg.post"), free);
    if (buf) {
      int received = 0;
      while (received < total_len) {
        int ret = httpd_req_recv(req, buf.get() + received, total_len - received);
        if (ret <= 0) break;
        received += ret;
      }
      buf.get()[received] = '\0';
      body = String(buf.get());
    }
  }
  String username = urlDecode(extractFormField(body, "username"));
  String password = extractFormField(body, "password");
  String confirmPassword = extractFormField(body, "confirm_password");

  if (username.length() == 0 || password.length() == 0 || confirmPassword.length() == 0) {
    httpd_resp_set_type(req, "text/html");
    streamBeginHtml(req, "Registration Failed", /*isPublic=*/true, "", "register");
    httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, R"REGERR(
<div class='panel container-narrow text-center pad-xl'>
  <h2 style='color:#dc3545'>Registration Failed</h2>
  <div class='form-error' style='background:#f8d7da;border:1px solid #f5c6cb;color:#721c24;padding:1rem;border-radius:8px;margin:1rem 0'>
    <p style='margin:0'>All fields are required.</p>
  </div>
  <div class='btn-row' style='justify-content:center'>
    <a class='btn' href='/register'>Try Again</a>
  </div>
</div>
)REGERR",
                          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    streamEndHtml(req);
    return ESP_OK;
  }

  if (password != confirmPassword) {
    httpd_resp_set_type(req, "text/html");
    streamBeginHtml(req, "Registration Failed", /*isPublic=*/true, "", "register");
    httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, R"REGERR(
<div class='panel container-narrow text-center pad-xl'>
  <h2 style='color:#dc3545'>Registration Failed</h2>
  <div class='form-error' style='background:#f8d7da;border:1px solid #f5c6cb;color:#721c24;padding:1rem;border-radius:8px;margin:1rem 0'>
    <p style='margin:0'>Passwords do not match. Please try again.</p>
  </div>
  <div class='btn-row' style='justify-content:center'>
    <a class='btn' href='/register'>Try Again</a>
  </div>
</div>
)REGERR",
                          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    streamEndHtml(req);
    return ESP_OK;
  }

  // Execute the built-in command via unified pipeline so it is logged/audited
  AuthContext ctx = makeWebAuthCtx(req);
  String cmdline = String("user request ") + username + " " + password + " " + confirmPassword;
  String out;
  bool ok = executeUnifiedWebCommand(req, ctx, cmdline, out);
  // Some commands always return true; also check textual success marker
  ok = ok || (out.indexOf("Request submitted for") >= 0);

  httpd_resp_set_type(req, "text/html");
  if (ok) {
    streamBeginHtml(req, "Request Submitted", /*isPublic=*/true, "", "register");
    httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, R"REGSUCCESS(
<div class='panel container-narrow text-center pad-xl'>
  <h2 style='color:#28a745'>Request Submitted</h2>
  <div style='background:#d4edda;border:1px solid #c3e6cb;border-radius:8px;padding:1.5rem;margin:1rem 0'>
    <p style='color:#155724;margin-bottom:1rem;font-weight:500'>Your account request has been submitted successfully!</p>
    <p style='color:#155724;font-size:0.9rem;margin:0'>An administrator will review your request and approve access to the system.</p>
  </div>
  <div class='btn-row' style='justify-content:center'>
    <a class='btn btn-primary' href='/login'>Return to Sign In</a>
  </div>
</div>
)REGSUCCESS",
                          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    streamEndHtml(req);
  } else {
    streamBeginHtml(req, "Registration Failed", /*isPublic=*/true, "", "register");
    httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, R"REGERR(
<div class='panel container-narrow text-center pad-xl'>
  <h2 style='color:#dc3545'>Registration Failed</h2>
  <div class='form-error' style='background:#f8d7da;border:1px solid #f5c6cb;color:#721c24;padding:1rem;border-radius:8px;margin:1rem 0'>
    <p style='margin:0'>)REGERR",
                          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, out.length() ? out.c_str() : "An error occurred.", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, R"REGERR2(</p>
  </div>
  <div class='btn-row' style='justify-content:center'>
    <a class='btn' href='/register'>Try Again</a>
  </div>
</div>
)REGERR2",
                          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    streamEndHtml(req);
  }
  return ESP_OK;
}

// ============================================================================
// File Handlers (Batch 3 - migrated from .ino)
// ============================================================================

// File handler dependencies
extern bool filesystemReady;
extern bool buildFilesListing(const String& inPath, String& out, bool asJson);
extern bool ensureFileViewBuffers();
extern char* gFileReadBuf;
extern char* gFileOutBuf;
extern const size_t kFileReadBufSize;
extern const size_t kFileOutBufSize;
extern bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);
extern void fsLock(const char* tag);
extern void fsUnlock();

esp_err_t handleFilesList(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  // Check if filesystem is ready
  if (!filesystemReady) {
    broadcastOutput("[files] ERROR: Filesystem not ready");
    String json = "{\"success\":false,\"error\":\"Filesystem not initialized\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Get path parameter
  char pathParam[256];
  String dirPath = "/";
  if (httpd_req_get_url_query_str(req, pathParam, sizeof(pathParam)) == ESP_OK) {
    char pathValue[256];
    if (httpd_query_key_value(pathParam, "path", pathValue, sizeof(pathValue)) == ESP_OK) {
      dirPath = String(pathValue);
      // URL decode the path
      dirPath.replace("%2F", "/");
      dirPath.replace("%20", " ");
      broadcastOutput(String("[files] Listing directory: ") + dirPath);
    }
  }
  String body;
  bool ok = buildFilesListing(dirPath, body, /*asJson=*/true);
  String json;
  if (ok) {
    json = String("{\"success\":true,\"files\":[") + body + "]}";
  } else {
    json = "{\"success\":false,\"error\":\"Directory not found or not accessible\"}";
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t handleFilesStats(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  if (!filesystemReady) {
    String json = "{\"success\":false,\"error\":\"Filesystem not initialized\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;
  int usagePercent = (totalBytes == 0) ? 0 : (usedBytes * 100) / totalBytes;

  String json = String("{\"success\":true,\"total\":") + String(totalBytes)
                + ",\"used\":" + String(usedBytes)
                + ",\"free\":" + String(freeBytes)
                + ",\"usagePercent\":" + String(usagePercent) + "}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t handleFilesCreate(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"No data received\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  buf[ret] = '\0';

  String body = String(buf);
  String name = "";
  String type = "";

  // Parse form data
  int nameStart = body.indexOf("name=");
  int typeStart = body.indexOf("type=");

  if (nameStart >= 0) {
    nameStart += 5;
    int nameEnd = body.indexOf("&", nameStart);
    if (nameEnd < 0) nameEnd = body.length();
    name = body.substring(nameStart, nameEnd);
    name.replace("%20", " ");
    name.replace("%2F", "/");
  }

  if (typeStart >= 0) {
    typeStart += 5;
    int typeEnd = body.indexOf("&", typeStart);
    if (typeEnd < 0) typeEnd = body.length();
    type = body.substring(typeStart, typeEnd);
  }

  if (name.length() == 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Name required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  // Normalize: strip leading '/' if present to prevent double slashes
  if (name.startsWith("/")) {
    name = name.substring(1);
  }
  String path = "/" + name;

  if (type == "folder") {
    // Use CLI command for consistent validation and error handling
    String cmd = "mkdir " + path;
    char* result = (char*)ps_alloc(512, AllocPref::PreferPSRAM, "http.mkdir.result");
    if (!result) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, "{\"success\":false,\"error\":\"Memory allocation failed\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    bool success = executeCommand(ctx, cmd.c_str(), result, 512);
    httpd_resp_set_type(req, "application/json");
    String resultStr = result;
    free(result);
    if (success && resultStr.startsWith("Created folder:")) {
      httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
      // Extract error message and return as JSON
      String errorMsg = resultStr;
      errorMsg.replace("\"", "\\\"");  // Escape quotes for JSON
      String jsonResponse = "{\"success\":false,\"error\":\"" + errorMsg + "\"}";
      httpd_resp_send(req, jsonResponse.c_str(), HTTPD_RESP_USE_STRLEN);
    }
  } else {
    // Normalize extension in path
    if (!name.endsWith("." + type)) {
      path = "/" + name + "." + type;
    }
    // Route through unified executor using CLI-equivalent command
    String cmd = String("filecreate ") + path;
    String out;
    bool ok = executeUnifiedWebCommand(req, ctx, cmd, out);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
      httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
      String resp = String("{\"success\":false,\"error\":\"") + out + String("\"}");
      httpd_resp_send(req, resp.c_str(), HTTPD_RESP_USE_STRLEN);
    }
  }

  return ESP_OK;
}

esp_err_t handleFileView(httpd_req_t* req) {
  DEBUG_STORAGEF("[handleFileView] ENTER heap=%u", (unsigned)ESP.getFreeHeap());

  // Pause sensor polling during file streaming to prevent I2C contention
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) {
    gSensorPollingPaused = wasPaused;
    return ESP_OK;
  }
  DEBUG_STORAGEF("[handleFileView] After auth heap=%u", (unsigned)ESP.getFreeHeap());

  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "No filename specified", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char name[128];
  if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Invalid filename", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  String path = String(name);
  // Full URL decode the path
  String decoded = "";
  decoded.reserve(path.length());
  for (unsigned int i = 0; i < path.length(); i++) {
    if (path[i] == '%' && i + 2 < path.length()) {
      char hex[3] = { path[i + 1], path[i + 2], 0 };
      decoded += (char)strtol(hex, NULL, 16);
      i += 2;
    } else if (path[i] == '+') {
      decoded += ' ';
    } else {
      decoded += path[i];
    }
  }
  path = decoded;

  broadcastOutput(String("[files] Viewing file: ") + path);

  DEBUG_STORAGEF("[handleFileView] File='%s' decoded='%s' heap=%u", name, path.c_str(), (unsigned)ESP.getFreeHeap());

  // Prefer special handling for JSON: pretty or raw streaming
  String filename = String(name);
  String displayName = filename;  // for titles
  displayName.replace("%2F", "/");
  displayName.replace("%20", " ");
  bool isJson = filename.endsWith(".json");
  if (isJson) {
    // Check view mode
    char mode[16];
    bool raw = false;
    if (httpd_query_key_value(query, "mode", mode, sizeof(mode)) == ESP_OK) {
      if (strcmp(mode, "raw") == 0) raw = true;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // Header and toolbar
    httpd_resp_send_chunk(req, "<!DOCTYPE html><html><head><title>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, filename.c_str(), filename.length());
    httpd_resp_send_chunk(req, "</title><style>"
      ":root{--bg:#f5f5f5;--fg:#333;--pre-bg:white;--pre-border:#ddd;--btn-bg:#fff;--btn-border:#ccc;--btn-active:#e9ecef}"
      "@media(prefers-color-scheme:dark){:root{--bg:#1a1a2e;--fg:#f0f0f0;--pre-bg:#16213e;--pre-border:#374151;--btn-bg:#374151;--btn-border:#4b5563;--btn-active:#4b5563}}"
      "body{font-family:monospace;margin:20px;background:var(--bg);color:var(--fg);font-size:14px}"
      "pre{background:var(--pre-bg);color:var(--fg);padding:15px;border-radius:5px;border:1px solid var(--pre-border);overflow-x:auto;font-size:14px;line-height:1.4}"
      ".bar{margin:8px 0 12px 0}"
      ".btn{display:inline-block;padding:4px 8px;border:1px solid var(--btn-border);border-radius:4px;background:var(--btn-bg);color:var(--fg);text-decoration:none;margin-right:6px}"
      ".btn.active{background:var(--btn-active)}"
      "</style></head><body><h2>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, displayName.c_str(), displayName.length());
    String base = String("/api/files/view?name=") + filename;
    String prettyHref = base + "&mode=pretty";
    String rawHref = base + "&mode=raw";
    httpd_resp_send_chunk(req, "</h2><div class='bar'>", HTTPD_RESP_USE_STRLEN);
    if (raw) {
      httpd_resp_send_chunk(req, "<a class='btn' href=\"", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, prettyHref.c_str(), prettyHref.length());
      httpd_resp_send_chunk(req, "\">Pretty</a><span class='btn active'>Raw</span>", HTTPD_RESP_USE_STRLEN);
    } else {
      httpd_resp_send_chunk(req, "<span class='btn active'>Pretty</span><a class='btn' href=\"", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, rawHref.c_str(), rawHref.length());
      httpd_resp_send_chunk(req, "\">Raw</a>", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</div><pre>", HTTPD_RESP_USE_STRLEN);

    // Ensure streaming buffers are allocated
    if (!ensureFileViewBuffers()) {
      httpd_resp_send_chunk(req, "Allocation failed", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, "</pre></body></html>", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, NULL, 0);
      gSensorPollingPaused = wasPaused;
      return ESP_OK;
    }

    // Open file via VFS (handles both LittleFS and SD)
    File file = VFS::open(path, "r");
    if (!file) {
      gSensorPollingPaused = wasPaused;
      httpd_resp_set_type(req, "text/plain");
      httpd_resp_send(req, "File not found", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    fsLock("file.view.json.open");

    if (raw) {
      // Stream raw JSON: lock only during reads
      while (true) {
        int bytesRead = file.readBytes(gFileReadBuf, kFileReadBufSize);
        if (bytesRead <= 0) break;
        fsUnlock();
        httpd_resp_send_chunk(req, gFileReadBuf, bytesRead);
        fsLock("file.view.json.loop");
      }
      file.close();
      fsUnlock();
      httpd_resp_send_chunk(req, "</pre></body></html>", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, NULL, 0);  // end chunked response
      gSensorPollingPaused = wasPaused;
      return ESP_OK;
    }

    // Pretty-print streaming
    int indent = 0;
    bool inString = false;
    bool escaped = false;
    size_t outLen = 0;
    auto flushOut = [&](bool force) {
      if (outLen && (force || outLen > (kFileOutBufSize - 64))) {
        httpd_resp_send_chunk(req, gFileOutBuf, outLen);
        outLen = 0;
      }
    };
    auto emit = [&](char ch) {
      if (outLen >= kFileOutBufSize - 1) flushOut(false);
      gFileOutBuf[outLen++] = ch;
    };
    auto emitIndent = [&]() {
      int spaces = indent * 2;
      for (int j = 0; j < spaces; j++) {
        if (outLen >= kFileOutBufSize - 1) flushOut(false);
        gFileOutBuf[outLen++] = ' ';
      }
    };
    while (true) {
      int bytesRead = file.readBytes(gFileReadBuf, kFileReadBufSize);
      if (bytesRead <= 0) break;
      fsUnlock();
      for (int i = 0; i < bytesRead; i++) {
        char c = gFileReadBuf[i];
        if (!inString) {
          if (c == '"' && !escaped) {
            inString = true;
            emit(c);
          } else if (c == '{' || c == '[') {
            emit(c);
            emit('\n');
            indent++;
            emitIndent();
          } else if (c == '}' || c == ']') {
            emit('\n');
            if (indent > 0) indent--;
            emitIndent();
            emit(c);
          } else if (c == ',') {
            emit(c);
            emit('\n');
            emitIndent();
          } else if (c == ':') {
            emit(c);
            emit(' ');
          } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            emit(c);
          }
        } else {
          emit(c);
          if (c == '"' && !escaped) { inString = false; }
        }
        escaped = (c == '\\' && !escaped);
        if (outLen >= kFileOutBufSize - 4) flushOut(false);
      }
      flushOut(false);
      fsLock("file.view.json.loop2");
    }
    file.close();
    fsUnlock();
    flushOut(true);
    httpd_resp_send_chunk(req, "</pre></body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);  // end chunked response
    gSensorPollingPaused = wasPaused;
    return ESP_OK;
  }

  // Default: non-JSON simple read-then-send
  uint32_t tVStart = millis();
  DEBUG_STORAGEF("[handleFileView] Non-JSON path=%s, heap=%u", path.c_str(), (unsigned)ESP.getFreeHeap());

  // Serialize file operations to prevent concurrent access crashes
  FsLockGuard fsLock("handleFileView");

  if (!VFS::exists(path)) {
    DEBUG_STORAGEF("[handleFileView] ERROR: File does not exist: %s", path.c_str());
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "File not found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  DEBUG_STORAGEF("[handleFileView] File exists, opening: %s (heap=%u)", path.c_str(), (unsigned)ESP.getFreeHeap());
  File file = VFS::open(path, "r");
  if (!file) {
    ERROR_STORAGEF("Failed to open file: %s", path.c_str());
    gSensorPollingPaused = wasPaused;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Failed to open file", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  size_t fileSize = file.size();
  DEBUG_STORAGEF("[handleFileView] File opened, size: %d bytes, heap=%u", fileSize, (unsigned)ESP.getFreeHeap());

  // Extract filename from path for Content-Disposition header
  String dispFilename = path;
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash >= 0) dispFilename = path.substring(lastSlash + 1);
  
  // Check if it's an image file
  bool isImage = path.endsWith(".jpg") || path.endsWith(".jpeg") || path.endsWith(".png") || path.endsWith(".gif") || path.endsWith(".bmp") || path.endsWith(".webp") || path.endsWith(".ico") || path.endsWith(".svg");
  
  // Check if it's a binary download file (e.g., .hwmap)
  bool isBinaryDownload = path.endsWith(".hwmap");

  // Allocate streaming buffer from PSRAM for better throughput
  const size_t VIEW_BUF_SIZE = 4096;  // 4KB instead of 512 bytes
  char* viewBuf = (char*)ps_alloc(VIEW_BUF_SIZE, AllocPref::PreferPSRAM);
  if (!viewBuf) {
    file.close();
    gSensorPollingPaused = wasPaused;
    DEBUG_STORAGEF("[handleFileView] ERROR: Failed to allocate view buffer");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Memory allocation failed", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (isImage) {
    DEBUG_STORAGEF("[handleFileView] Image file detected, setting content type");
    // Set appropriate content type for images
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) {
      httpd_resp_set_type(req, "image/jpeg");
    } else if (path.endsWith(".png")) {
      httpd_resp_set_type(req, "image/png");
    } else if (path.endsWith(".gif")) {
      httpd_resp_set_type(req, "image/gif");
    } else if (path.endsWith(".bmp")) {
      httpd_resp_set_type(req, "image/bmp");
    } else if (path.endsWith(".webp")) {
      httpd_resp_set_type(req, "image/webp");
    } else if (path.endsWith(".ico")) {
      httpd_resp_set_type(req, "image/x-icon");
    } else if (path.endsWith(".svg")) {
      httpd_resp_set_type(req, "image/svg+xml");
    }

    // Stream binary data in chunks
    size_t totalSent = 0;
    int chunkCount = 0;
    while (true) {
      size_t n = file.readBytes(viewBuf, VIEW_BUF_SIZE);
      if (n == 0) break;
      chunkCount++;
      totalSent += n;
      httpd_resp_send_chunk(req, viewBuf, n);
      if ((chunkCount % 8) == 0) {
        DEBUG_STORAGEF("[handleFileView] Streamed %d bytes in %d chunks (heap=%u)", totalSent, chunkCount, (unsigned)ESP.getFreeHeap());
        delay(0);  // avoid WDT while streaming
      }
    }
    file.close();
    free(viewBuf);
    httpd_resp_send_chunk(req, NULL, 0);
    DEBUG_STORAGEF("[handleFileView] Image sent: %d bytes in %d chunks, dur=%u ms", totalSent, chunkCount, (unsigned)(millis() - tVStart));
    gSensorPollingPaused = wasPaused;
    return ESP_OK;
  }

  // Binary download files (e.g., .hwmap) - serve with Content-Disposition for proper filename
  if (isBinaryDownload) {
    DEBUG_STORAGEF("[handleFileView] Binary download file detected: %s", dispFilename.c_str());
    httpd_resp_set_type(req, "application/octet-stream");
    String disposition = "attachment; filename=\"" + dispFilename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());

    // Stream binary data in chunks
    size_t totalSent = 0;
    int chunkCount = 0;
    while (true) {
      size_t n = file.readBytes(viewBuf, VIEW_BUF_SIZE);
      if (n == 0) break;
      chunkCount++;
      totalSent += n;
      httpd_resp_send_chunk(req, viewBuf, n);
      if ((chunkCount % 8) == 0) {
        DEBUG_STORAGEF("[handleFileView] Streamed %d bytes in %d chunks (heap=%u)", totalSent, chunkCount, (unsigned)ESP.getFreeHeap());
        delay(0);  // avoid WDT while streaming
      }
    }
    file.close();
    free(viewBuf);
    httpd_resp_send_chunk(req, NULL, 0);
    DEBUG_STORAGEF("[handleFileView] Binary file sent: %d bytes in %d chunks, dur=%u ms", totalSent, chunkCount, (unsigned)(millis() - tVStart));
    gSensorPollingPaused = wasPaused;
    return ESP_OK;
  }

  // Text file - stream it in chunks like images
  DEBUG_STORAGEF("[handleFileView] Text file path='%s' size=%d heap=%u", path.c_str(), fileSize, (unsigned)ESP.getFreeHeap());
  DEBUG_STORAGEF("[handleFileView] Setting content type...");
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  DEBUG_STORAGEF("[handleFileView] Starting streaming loop...");

  size_t totalSent = 0;
  int chunkCount = 0;
  while (true) {
    size_t n = file.readBytes(viewBuf, VIEW_BUF_SIZE);
    if (n == 0) break;
    DEBUG_STORAGEF("[handleFileView] Read chunk %d: %d bytes, heap=%u", chunkCount + 1, n, (unsigned)ESP.getFreeHeap());
    chunkCount++;
    totalSent += n;
    httpd_resp_send_chunk(req, viewBuf, n);
    if ((chunkCount % 8) == 0) {
      DEBUG_STORAGEF("[handleFileView] Streamed %d bytes in %d chunks (heap=%u)", totalSent, chunkCount, (unsigned)ESP.getFreeHeap());
      delay(0);  // avoid WDT while streaming
    }
  }
  file.close();
  free(viewBuf);
  httpd_resp_send_chunk(req, NULL, 0);
  DEBUG_STORAGEF("[handleFileView] COMPLETE: Text file sent %d bytes in %d chunks (dur=%u ms)", totalSent, chunkCount, (unsigned)(millis() - tVStart));
  
  // Resume sensor polling
  gSensorPollingPaused = wasPaused;
  return ESP_OK;
}

esp_err_t handleIconGet(httpd_req_t* req) {
  char query[128];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_set_status(req, "400");
    httpd_resp_send(req, "No icon name", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char name[64];
  if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
    httpd_resp_set_status(req, "400");
    httpd_resp_send(req, "Invalid icon name", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  bool debug = false;
  {
    char dbg[8];
    if (httpd_query_key_value(query, "debug", dbg, sizeof(dbg)) == ESP_OK) {
      debug = (dbg[0] == '1' || dbg[0] == 't' || dbg[0] == 'T' || dbg[0] == 'y' || dbg[0] == 'Y');
    }
  }

  DEBUG_HTTPF("[Icon] GET name='%s' debug=%d", name, debug ? 1 : 0);

  extern const EmbeddedIcon* findEmbeddedIcon(const char* name);
  const EmbeddedIcon* icon = findEmbeddedIcon(name);
  
  if (!icon) {
    if (debug) {
      httpd_resp_set_hdr(req, "X-Icon-Name", name);
      httpd_resp_set_hdr(req, "X-Icon-Status", "not_found");
    }
    DEBUG_HTTPF("[Icon] NOT FOUND name='%s'", name);
    httpd_resp_set_status(req, "404");
    httpd_resp_send(req, "Icon not found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  httpd_resp_set_type(req, "image/png");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
  
  const uint8_t* pngPtr = (const uint8_t*)pgm_read_ptr(&icon->pngData);
  size_t pngSize = (size_t)pgm_read_dword(&icon->pngSize);

  if (debug) {
    char sz[16];
    snprintf(sz, sizeof(sz), "%u", (unsigned)pngSize);
    httpd_resp_set_hdr(req, "X-Icon-Name", name);
    httpd_resp_set_hdr(req, "X-Icon-Size", sz);
    httpd_resp_set_hdr(req, "X-Icon-Status", "ok");
  }

  DEBUG_HTTPF("[Icon] SEND name='%s' pngSize=%u", name, (unsigned)pngSize);

  // Safari can be picky about chunked binary responses; these icons are tiny, so send in one response.
  uint8_t* tmp = (uint8_t*)malloc(pngSize);
  if (!tmp) {
    ERROR_MEMORYF("OOM for icon: name='%s' pngSize=%u", name, (unsigned)pngSize);
    httpd_resp_set_status(req, "500");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OOM", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  memcpy_P(tmp, pngPtr, pngSize);
  esp_err_t r = httpd_resp_send(req, (const char*)tmp, pngSize);
  free(tmp);
  if (r != ESP_OK) {
    DEBUG_HTTPF("[Icon] SEND FAIL name='%s' err=%d", name, (int)r);
    return ESP_OK;
  }

  DEBUG_HTTPF("[Icon] COMPLETE name='%s' sent=%u", name, (unsigned)pngSize);
  return ESP_OK;
}

esp_err_t handleIconTestPage(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  
  extern const size_t EMBEDDED_ICONS_COUNT;
  extern const EmbeddedIcon EMBEDDED_ICONS[];
  
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Icon Test</title>";
  html += "<style>body{font-family:sans-serif;max-width:1200px;margin:20px auto;padding:20px;background:#fafafa;}";
  html += ".controls{background:#fff;padding:12px;border-radius:8px;margin:16px 0;display:flex;gap:12px;align-items:center;box-shadow:0 1px 3px rgba(0,0,0,0.1);}";
  html += ".controls input{padding:6px 10px;border:1px solid #ccc;border-radius:4px;font-size:14px;flex:1;max-width:300px;}";
  html += ".controls button{padding:6px 14px;border:1px solid #ccc;border-radius:4px;cursor:pointer;background:#fff;font-size:13px;}";
  html += ".controls button.active{background:#333;color:#fff;border-color:#333;}";
  html += ".icon-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:12px;margin:20px 0;}";
  html += ".icon-item{border:1px solid #e0e0e0;padding:12px;text-align:center;border-radius:8px;background:#fff;transition:transform 0.1s;}";
  html += ".icon-item:hover{transform:scale(1.05);box-shadow:0 2px 8px rgba(0,0,0,0.12);}";
  html += ".icon-item img{image-rendering:pixelated;image-rendering:crisp-edges;background:#1a1a2e;border-radius:6px;padding:6px;box-sizing:border-box;display:block;margin:0 auto;}";
  html += ".icon-name{font-size:0.8em;color:#555;margin-top:8px;word-break:break-all;font-family:monospace;}";
  html += ".icon-info{font-size:0.7em;color:#999;margin-top:2px;}";
  html += "h1{color:#222;margin-bottom:4px;}";
  html += ".stats{color:#666;font-size:0.9em;}";
  html += "</style></head><body>";
  html += "<h1>Embedded Icons</h1>";
  html += "<div class='stats'>" + String(EMBEDDED_ICONS_COUNT) + " icons &middot; 32&times;32 native</div>";
  html += "<div class='controls'>";
  html += "<input type='text' id='filter' placeholder='Filter icons...' oninput='filterIcons()'>";
  html += "<button onclick='setSize(32)' id='b32'>32px</button>";
  html += "<button onclick='setSize(64)' id='b64' class='active'>64px</button>";
  html += "<button onclick='setSize(128)' id='b128'>128px</button>";
  html += "</div>";
  html += "<div class='icon-grid' id='grid'>";
  
  for (size_t i = 0; i < EMBEDDED_ICONS_COUNT; i++) {
    char iconName[32];
    strcpy_P(iconName, (PGM_P)pgm_read_ptr(&EMBEDDED_ICONS[i].name));
    size_t pngSize = (size_t)pgm_read_dword(&EMBEDDED_ICONS[i].pngSize);
    uint8_t width = pgm_read_byte(&EMBEDDED_ICONS[i].width);
    uint8_t height = pgm_read_byte(&EMBEDDED_ICONS[i].height);
    
    html += "<div class='icon-item'>";
    html += "<img src='/api/icon?name=" + String(iconName) + "' width='64' height='64'>";
    html += "<div class='icon-name'>" + String(iconName) + "</div>";
    html += "<div class='icon-info'>" + String(width) + "x" + String(height) + " (" + String(pngSize) + "B)</div>";
    html += "</div>";
  }
  
  html += "</div>";
  html += "<script>";
  html += "function setSize(s){document.querySelectorAll('.icon-item img').forEach(i=>{i.width=s;i.height=s;});";
  html += "document.querySelectorAll('.controls button').forEach(b=>b.classList.remove('active'));";
  html += "document.getElementById('b'+s).classList.add('active');}";
  html += "function filterIcons(){var f=document.getElementById('filter').value.toLowerCase();";
  html += "document.querySelectorAll('.icon-item').forEach(i=>{var n=i.querySelector('.icon-name').textContent;";
  html += "i.style.display=n.includes(f)?'':'none';});}";
  html += "</script>";
  html += "</body></html>";
  
  httpd_resp_send(req, html.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t handleFileDelete(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  // Accept name from POST body (x-www-form-urlencoded) or URL query as fallback
  String nameStr = "";
  {
    // Try to read POST body
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
      buf[ret] = '\0';
      String body = String(buf);
      int nameStart = body.indexOf("name=");
      if (nameStart >= 0) {
        nameStart += 5;
        int nameEnd = body.indexOf("&", nameStart);
        if (nameEnd < 0) nameEnd = body.length();
        nameStr = body.substring(nameStart, nameEnd);
      }
    }
  }
  if (nameStr.length() == 0) {
    // Fallback to query parameter
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      char name[128];
      if (httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_OK) {
        nameStr = String(name);
      }
    }
  }
  if (nameStr.length() == 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"No filename specified\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  // URL decode minimal subset
  nameStr.replace("%2F", "/");
  nameStr.replace("%20", " ");
  // Normalize: strip leading '/' if present to prevent double slashes
  if (nameStr.startsWith("/")) {
    nameStr = nameStr.substring(1);
  }
  String path = "/" + nameStr;

  // Use centralized permission check
  if (nameStr.length() == 0 || nameStr == "." || nameStr == ".." || !canDelete(path)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Deletion not allowed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Check if it's a directory
  bool isDir = false;
  {
    FsLockGuard guard("delete.probe");
    File file = VFS::open(path, "r");
    if (!file) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, "{\"success\":false,\"error\":\"File not found\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    isDir = file.isDirectory();
    file.close();
  }

  bool success = false;
  {
    FsLockGuard guard("web_files.delete");
    if (isDir) {
      success = VFS::rmdir(path);
    } else {
      success = VFS::remove(path);
    }
  }

  if (success) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to delete\"}", HTTPD_RESP_USE_STRLEN);
  }

  return ESP_OK;
}

// ============================================================================
// File Rename Handler
// ============================================================================

esp_err_t handleFileRename(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  // Read POST body
  char buf[512];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"No data received\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  buf[ret] = '\0';
  String body = String(buf);

  // Parse oldPath and newName from POST body
  String oldPath = "";
  String newName = "";
  {
    int idx = body.indexOf("oldPath=");
    if (idx >= 0) {
      idx += 8;
      int end = body.indexOf("&", idx);
      if (end < 0) end = body.length();
      oldPath = body.substring(idx, end);
    }
    idx = body.indexOf("newName=");
    if (idx >= 0) {
      idx += 8;
      int end = body.indexOf("&", idx);
      if (end < 0) end = body.length();
      newName = body.substring(idx, end);
    }
  }

  // URL decode
  oldPath.replace("%2F", "/");
  oldPath.replace("%20", " ");
  oldPath.replace("+", " ");
  newName.replace("%2F", "/");
  newName.replace("%20", " ");
  newName.replace("+", " ");

  if (oldPath.length() == 0 || newName.length() == 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"oldPath and newName required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Ensure oldPath starts with /
  if (!oldPath.startsWith("/")) oldPath = "/" + oldPath;

  // Build new path: same parent directory + new name
  int lastSlash = oldPath.lastIndexOf('/');
  String parentDir = (lastSlash > 0) ? oldPath.substring(0, lastSlash) : "";
  String newPath = parentDir + "/" + newName;

  // Use centralized permission check
  if (!canRename(oldPath)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Rename not allowed for this file\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  bool success = false;
  {
    FsLockGuard guard("web_files.rename");
    if (VFS::exists(oldPath)) {
      success = VFS::rename(oldPath, newPath);
    }
  }

  httpd_resp_set_type(req, "application/json");
  if (success) {
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Rename failed\"}", HTTPD_RESP_USE_STRLEN);
  }
  return ESP_OK;
}

// ============================================================================
// Admin Handlers (Batch 4 - migrated from .ino)
// ============================================================================

// Admin handler dependencies
extern bool readText(const char* path, String& out);
extern bool approvePendingUserInternal(const String& username, String& errorOut);
extern bool denyPendingUserInternal(const String& username, String& errorOut);

esp_err_t handleAdminPending(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  // Preserve JSON error contract for this endpoint
  if (!isAdminUser(ctx.user)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Admin access required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  JsonDocument response;
  response["success"] = true;
  JsonArray pendingArray = response["pending"].to<JsonArray>();

  if (LittleFS.exists("/system/users/pending_users.json")) {
    File file = LittleFS.open("/system/users/pending_users.json", "r");
    if (file) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, file);
      file.close();
      if (!err) {
        JsonArray pending = doc.as<JsonArray>();
        for (JsonObject user : pending) {
          JsonObject sanitizedUser = pendingArray.add<JsonObject>();
          sanitizedUser["username"] = user["username"];
          sanitizedUser["timestamp"] = user["timestamp"];
          // Explicitly exclude password field for security
        }
      }
    }
  }

  // Serialize to response buffer
  if (xSemaphoreTake(gJsonResponseMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    size_t len = serializeJson(response, gJsonResponseBuffer, JSON_RESPONSE_SIZE);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, gJsonResponseBuffer, len);
    xSemaphoreGive(gJsonResponseMutex);
  } else {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"pending\":[]}", HTTPD_RESP_USE_STRLEN);
  }
  return ESP_OK;
}

esp_err_t handleAdminApproveUser(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  if (!isAdminUser(ctx.user)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Admin access required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Read POST body inline (reuse pattern from other handlers)
  String body;
  int total_len = req->content_len;
  if (total_len > 0) {
    std::unique_ptr<char, void (*)(void*)> buf((char*)ps_alloc(total_len + 1, AllocPref::PreferPSRAM, "http.admin"), free);
    if (buf) {
      int received = 0;
      while (received < total_len) {
        int r = httpd_req_recv(req, buf.get() + received, total_len - received);
        if (r <= 0) break;
        received += r;
      }
      buf.get()[received] = '\0';
      body = String(buf.get());
    }
  }
  String username = urlDecode(extractFormField(body, "username"));
  if (username.length() == 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Username required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  String err;
  bool ok = approvePendingUserInternal(username, err);
  httpd_resp_set_type(req, "application/json");
  if (ok) {
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send(req, (String("{\"success\":false,\"error\":\"") + err + "\"}").c_str(), HTTPD_RESP_USE_STRLEN);
  }
  return ESP_OK;
}

esp_err_t handleAdminDenyUser(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  // Preserve JSON error contract for this endpoint
  if (!isAdminUser(ctx.user)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Admin access required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Read full body and decode
  int total_len = req->content_len;
  if (total_len <= 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"No data\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  std::unique_ptr<char, void (*)(void*)> buf((char*)ps_alloc(total_len + 1, AllocPref::PreferPSRAM, "http.passwd"), free);
  int received = 0;
  while (received < total_len) {
    int r = httpd_req_recv(req, buf.get() + received, total_len - received);
    if (r <= 0) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, "{\"success\":false,\"error\":\"Read error\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    received += r;
  }
  buf.get()[received] = '\0';
  String body = String(buf.get());
  String username = urlDecode(extractFormField(body, "username"));

  if (username.length() == 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Username required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  String err;
  if (!denyPendingUserInternal(username, err)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, (String("{\"success\":false,\"error\":\"") + err + "\"}").c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

#if ENABLE_AUTOMATION
// ============================================================================
// Automation Export Handler (Batch 5 - migrated from .ino)
// ============================================================================

// Automation export dependencies
extern bool extractArrayByKey(const String& json, const char* key, String& out);
extern bool extractArrayItem(const String& arrayStr, int& pos, String& out);
extern bool parseJsonInt(const String& json, const char* key, int& out);
extern bool parseJsonString(const String& json, const char* key, String& out);
extern const char* AUTOMATIONS_JSON_FILE;

esp_err_t handleAutomationsExport(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  // Parse query parameters
  char query[512] = { 0 };
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char idParam[32] = { 0 };
    if (httpd_query_key_value(query, "id", idParam, sizeof(idParam)) == ESP_OK) {
      // Export single automation
      String json;
      if (!readText(AUTOMATIONS_JSON_FILE, json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read automations");
        return ESP_OK;
      }

      // Find specific automation using string parsing
      int targetId = atoi(idParam);
      String automationsArray;
      if (!extractArrayByKey(json, "automations", automationsArray)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No automations array found");
        return ESP_OK;
      }

      // Parse automations array to find target
      String targetAuto;
      int pos = 0;
      while (pos < (int)automationsArray.length()) {
        String item;
        if (!extractArrayItem(automationsArray, pos, item)) break;

        // Check if this automation has the target ID
        int autoId = 0;
        if (parseJsonInt(item, "id", autoId) && autoId == targetId) {
          targetAuto = item;
          break;
        }
      }

      if (targetAuto.length() == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Automation not found");
        return ESP_OK;
      }

      // Generate filename from automation name
      String name;
      if (!parseJsonString(targetAuto, "name", name) || name.length() == 0) {
        name = "automation";
      }
      // Sanitize filename
      name.replace(" ", "_");
      name.replace("/", "_");
      name.replace("\\", "_");
      String filename = name + ".json";

      // Set download headers
      httpd_resp_set_type(req, "application/json");
      httpd_resp_set_hdr(req, "Content-Disposition", ("attachment; filename=\"" + filename + "\"").c_str());

      // Send single automation JSON (targetAuto already includes braces)
      httpd_resp_send(req, targetAuto.c_str(), HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
  }

  // Export all automations (bulk export)
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read automations");
    return ESP_OK;
  }

  // Generate timestamp for filename
  time_t now;
  time(&now);
  struct tm* timeinfo = localtime(&now);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", timeinfo);
  String filename = String("automations-backup-") + timestamp + ".json";

  // Set download headers
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Content-Disposition", ("attachment; filename=\"" + filename + "\"").c_str());

  httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}
#endif // ENABLE_AUTOMATION

// ============================================================================
// SSE Notice Queue Helpers (moved from HardwareOne.ino)
// ============================================================================

void sseEnqueueNotice(SessionEntry& s, const String& msg) {
  // Queue-only policy: if full, drop oldest then enqueue new
  const int cap = SessionEntry::NOTICE_QUEUE_SIZE;
  if (s.nqCount < cap) {
    // Copy message to buffer, truncating if necessary
    strncpy(s.noticeQueue[s.nqTail], msg.c_str(), SessionEntry::NOTICE_MAX_LEN - 1);
    s.noticeQueue[s.nqTail][SessionEntry::NOTICE_MAX_LEN - 1] = '\0';  // Ensure null termination
    s.nqTail = (s.nqTail + 1) % cap;
    s.nqCount++;
  } else {
    // Drop oldest
    s.nqHead = (s.nqHead + 1) % cap;
    // Enqueue new at tail
    strncpy(s.noticeQueue[s.nqTail], msg.c_str(), SessionEntry::NOTICE_MAX_LEN - 1);
    s.noticeQueue[s.nqTail][SessionEntry::NOTICE_MAX_LEN - 1] = '\0';
    s.nqTail = (s.nqTail + 1) % cap;
    // nqCount remains at capacity
  }
  // Enter burst mode for faster reconnects for a short period
  s.noticeBurstUntil = millis() + 15000UL;  // 15s burst window
  s.needsNotificationTick = true;
}

bool sseDequeueNotice(SessionEntry& s, String& out) {
  if (s.nqCount == 0) return false;
  const int cap = SessionEntry::NOTICE_QUEUE_SIZE;
  out = String(s.noticeQueue[s.nqHead]);
  s.nqHead = (s.nqHead + 1) % cap;
  s.nqCount--;
  return true;
}

// ============================================================================
// Sensor Status Broadcast (moved from HardwareOne.ino)
// ============================================================================

void broadcastSensorStatusToAllSessions() {
  DEBUG_SSEF("broadcastSensorStatusToAllSessions called - seq: %lu", (unsigned long)gSensorStatusSeq);
  int flagged = 0;

  // Flag all active sessions as needing status updates
  // They will receive the update when their background SSE connects
  // Pre-pass: dump session table for diagnostics
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (gSessions[i].sid.length() > 0) {
      DEBUG_SSEF("session[%d] sid=%s user=%s needsStatusUpdate=%d lastSeqSent=%lu",
                 i, gSessions[i].sid.c_str(), gSessions[i].user.c_str(),
                 gSessions[i].needsStatusUpdate ? 1 : 0,
                 (unsigned long)gSessions[i].lastSensorSeqSent);
    }
  }

  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (gSessions[i].sid.length() > 0) {
      // Do not skip originator; client-side seq handling de-duplicates UI work
      gSessions[i].needsStatusUpdate = true;
      flagged++;
      DEBUG_SSEF("Flagged session %d (SID: %s) for status update", i, gSessions[i].sid.c_str());
    }
  }

  DEBUG_SSEF("Flagging done; total flagged=%d, skipIdx=%d, cause=%s", flagged, gBroadcastSkipSessionIdx, gLastStatusCause);
  DEBUG_SSEF("All active sessions flagged for status updates - background SSE will deliver");
}

// ============================================================================
// HTTP Server Management
// ============================================================================

// Handler for browser icon requests (favicon, apple-touch-icon) - returns 204 No Content
static esp_err_t handleBrowserIcon(httpd_req_t* req) {
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

esp_err_t handleCliBatch(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) {
    logAuthAttempt(false, "/api/cli/batch", String(), ctx.ip, "unauthorized");
    return ESP_OK;
  }

  if (req->content_len == 0 || req->content_len > 32768) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid content length\"}");
    return ESP_OK;
  }

  std::unique_ptr<char, void(*)(void*)> buf(
    (char*)ps_alloc(req->content_len + 1, AllocPref::PreferPSRAM, "http.batch"), free);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    return ESP_FAIL;
  }
  int received = 0;
  while (received < (int)req->content_len) {
    int r = httpd_req_recv(req, buf.get() + received, req->content_len - received);
    if (r <= 0) break;
    received += r;
  }
  buf.get()[received] = '\0';

  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, buf.get(), received);
  if (jerr || !doc["commands"].is<JsonArray>()) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Expected {\\\"commands\\\":[...]} JSON body\"}");
    return ESP_OK;
  }

  String sidForCmd = getCookieSID(req);
  int originIdx = findSessionIndexBySID(sidForCmd);
  int prevSkip = gBroadcastSkipSessionIdx;
  gBroadcastSkipSessionIdx = originIdx;
  gMeshActivitySuspended = true;

  int count = 0;
  for (JsonVariant v : doc["commands"].as<JsonArray>()) {
    String cmd = v.as<String>();
    cmd.trim();
    if (cmd.length() == 0) continue;

    appendCommandToFeed("web", cmd, ctx.user, ctx.ip);

    Command uc;
    uc.line = cmd;
    uc.ctx.origin = ORIGIN_WEB;
    uc.ctx.auth = ctx;
    uc.ctx.id = (uint32_t)millis();
    uc.ctx.timestampMs = (uint32_t)millis();
    uc.ctx.outputMask = CMD_OUT_WEB | CMD_OUT_LOG;
    uc.ctx.validateOnly = false;
    uc.ctx.replyHandle = nullptr;
    uc.ctx.httpReq = req;

    String out;
    submitAndExecuteSync(uc, out);
    String redacted = redactOutputForLog(out);
    broadcastOutput(redacted, uc.ctx);

    count++;
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  gMeshActivitySuspended = false;
  gBroadcastSkipSessionIdx = prevSkip;

  httpd_resp_set_type(req, "application/json");
  String resp = "{\"ok\":true,\"count\":";
  resp += count;
  resp += "}";
  httpd_resp_sendstr(req, resp.c_str());
  return ESP_OK;
}

void startHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 100;
  config.lru_purge_enable = true;
  config.stack_size = 12288;  // 12KB – headroom for AuthContext Strings + chunked handlers
  config.recv_wait_timeout = 30;  // 30 second timeout for large uploads
  config.send_wait_timeout = 30;  // 30 second timeout for large responses
  // Note: max header length is set via CONFIG_HTTPD_MAX_REQ_HDR_LEN in sdkconfig
  if (httpd_start(&server, &config) != ESP_OK) {
    broadcastOutput("ERROR: Failed to start HTTP server");
    return;
  }

  // Define URIs
  static httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = handleRoot, .user_ctx = NULL };
  static httpd_uri_t loginGet = { .uri = "/login", .method = HTTP_GET, .handler = handleLogin, .user_ctx = NULL };
  static httpd_uri_t loginPost = { .uri = "/login", .method = HTTP_POST, .handler = handleLogin, .user_ctx = NULL };
  static httpd_uri_t loginSetSess = { .uri = "/login/setsession", .method = HTTP_GET, .handler = handleLoginSetSession, .user_ctx = NULL };
  static httpd_uri_t logout = { .uri = "/logout", .method = HTTP_GET, .handler = handleLogout, .user_ctx = NULL };
  static httpd_uri_t ping = { .uri = "/api/ping", .method = HTTP_GET, .handler = handlePing, .user_ctx = NULL };
  static httpd_uri_t dash = { .uri = "/dashboard", .method = HTTP_GET, .handler = handleDashboard, .user_ctx = NULL };
  static httpd_uri_t settingsPage = { .uri = "/settings", .method = HTTP_GET, .handler = handleSettingsPage, .user_ctx = NULL };
  static httpd_uri_t settingsGet = { .uri = "/api/settings", .method = HTTP_GET, .handler = handleSettingsGet, .user_ctx = NULL };
  static httpd_uri_t settingsSchema = { .uri = "/api/settings/schema", .method = HTTP_GET, .handler = handleSettingsSchema, .user_ctx = NULL };
  static httpd_uri_t userSettingsGet = { .uri = "/api/user/settings", .method = HTTP_GET, .handler = handleUserSettingsGet, .user_ctx = NULL };
  static httpd_uri_t userSettingsSet = { .uri = "/api/user/settings", .method = HTTP_POST, .handler = handleUserSettingsSet, .user_ctx = NULL };
  static httpd_uri_t devicesGet = { .uri = "/api/devices", .method = HTTP_GET, .handler = handleDeviceRegistryGet, .user_ctx = NULL };
  static httpd_uri_t buildConfig = { .uri = "/api/buildconfig", .method = HTTP_GET, .handler = handleBuildConfig, .user_ctx = NULL };
  static httpd_uri_t apiNotice = { .uri = "/api/notice", .method = HTTP_GET, .handler = handleNotice, .user_ctx = NULL };
  static httpd_uri_t apiEvents = { .uri = "/api/events", .method = HTTP_GET, .handler = handleEvents, .user_ctx = NULL };
  static httpd_uri_t filesPage = { .uri = "/files", .method = HTTP_GET, .handler = handleFilesPage, .user_ctx = NULL };
  static httpd_uri_t filesList = { .uri = "/api/files/list", .method = HTTP_GET, .handler = handleFilesList, .user_ctx = NULL };
  static httpd_uri_t filesStats = { .uri = "/api/files/stats", .method = HTTP_GET, .handler = handleFilesStats, .user_ctx = NULL };
  static httpd_uri_t filesCreate = { .uri = "/api/files/create", .method = HTTP_POST, .handler = handleFilesCreate, .user_ctx = NULL };
  static httpd_uri_t filesView = { .uri = "/api/files/view", .method = HTTP_GET, .handler = handleFileView, .user_ctx = NULL };
  static httpd_uri_t filesDelete = { .uri = "/api/files/delete", .method = HTTP_POST, .handler = handleFileDelete, .user_ctx = NULL };
  static httpd_uri_t filesRead = { .uri = "/api/files/read", .method = HTTP_GET, .handler = handleFileRead, .user_ctx = NULL };
  static httpd_uri_t filesWrite = { .uri = "/api/files/write", .method = HTTP_POST, .handler = handleFileWrite, .user_ctx = NULL };
  static httpd_uri_t filesUpload = { .uri = "/api/files/upload", .method = HTTP_POST, .handler = handleFileUpload, .user_ctx = NULL };
  static httpd_uri_t filesRename = { .uri = "/api/files/rename", .method = HTTP_POST, .handler = handleFileRename, .user_ctx = NULL };
  static httpd_uri_t iconGet = { .uri = "/api/icon", .method = HTTP_GET, .handler = handleIconGet, .user_ctx = NULL };
  static httpd_uri_t iconTestPage = { .uri = "/icons/test", .method = HTTP_GET, .handler = handleIconTestPage, .user_ctx = NULL };
  static httpd_uri_t loggingPage = { .uri = "/logging", .method = HTTP_GET, .handler = handleLoggingPage, .user_ctx = NULL };
  static httpd_uri_t cliPage = { .uri = "/cli", .method = HTTP_GET, .handler = handleCLIPage, .user_ctx = NULL };
  static httpd_uri_t cliCmd = { .uri = "/api/cli", .method = HTTP_POST, .handler = handleCLICommand, .user_ctx = NULL };
  static httpd_uri_t cliBatch = { .uri = "/api/cli/batch", .method = HTTP_POST, .handler = handleCliBatch, .user_ctx = NULL };
  static httpd_uri_t logsGet = { .uri = "/api/cli/logs", .method = HTTP_GET, .handler = handleLogs, .user_ctx = NULL };
#if ENABLE_AUTOMATION
  static httpd_uri_t automationsPage = { .uri = "/automations", .method = HTTP_GET, .handler = handleAutomationsPage, .user_ctx = NULL };
#endif
  static httpd_uri_t systemStatus = { .uri = "/api/system", .method = HTTP_GET, .handler = handleSystemStatus, .user_ctx = NULL };
  // Sessions (admin)
  static httpd_uri_t sessionsList = { .uri = "/api/sessions", .method = HTTP_GET, .handler = handleSessionsList, .user_ctx = NULL };
  static httpd_uri_t adminSessionsList = { .uri = "/api/admin/sessions", .method = HTTP_GET, .handler = handleAdminSessionsList, .user_ctx = NULL };
#if ENABLE_AUTOMATION
  static httpd_uri_t automationsGet = { .uri = "/api/automations", .method = HTTP_GET, .handler = handleAutomationsGet, .user_ctx = NULL };
  static httpd_uri_t automationsExport = { .uri = "/api/automations/export", .method = HTTP_GET, .handler = handleAutomationsExport, .user_ctx = NULL };
#endif
  static httpd_uri_t outputGet = { .uri = "/api/output", .method = HTTP_GET, .handler = handleOutputGet, .user_ctx = NULL };
  static httpd_uri_t outputTemp = { .uri = "/api/output/temp", .method = HTTP_POST, .handler = handleOutputTemp, .user_ctx = NULL };
  static httpd_uri_t regPage = { .uri = "/register", .method = HTTP_GET, .handler = handleRegisterPage, .user_ctx = NULL };
  static httpd_uri_t regSubmit = { .uri = "/register/submit", .method = HTTP_POST, .handler = handleRegisterSubmit, .user_ctx = NULL };
  static httpd_uri_t adminPending = { .uri = "/api/admin/pending", .method = HTTP_GET, .handler = handleAdminPending, .user_ctx = NULL };
  static httpd_uri_t adminApprove = { .uri = "/api/admin/approve", .method = HTTP_POST, .handler = handleAdminApproveUser, .user_ctx = NULL };
  static httpd_uri_t adminDeny = { .uri = "/api/admin/reject", .method = HTTP_POST, .handler = handleAdminDenyUser, .user_ctx = NULL };
  // Browser icon handlers (silence 404 warnings)
  static httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = handleBrowserIcon, .user_ctx = NULL };
  static httpd_uri_t appleTouchIcon = { .uri = "/apple-touch-icon.png", .method = HTTP_GET, .handler = handleBrowserIcon, .user_ctx = NULL };
  static httpd_uri_t appleTouchIconPre = { .uri = "/apple-touch-icon-precomposed.png", .method = HTTP_GET, .handler = handleBrowserIcon, .user_ctx = NULL };

  // Register
  httpd_register_uri_handler(server, &favicon);
  httpd_register_uri_handler(server, &appleTouchIcon);
  httpd_register_uri_handler(server, &appleTouchIconPre);
  httpd_register_uri_handler(server, &root);
  httpd_register_uri_handler(server, &loginGet);
  httpd_register_uri_handler(server, &loginPost);
  httpd_register_uri_handler(server, &loginSetSess);
  httpd_register_uri_handler(server, &logout);
  httpd_register_uri_handler(server, &ping);
  httpd_register_uri_handler(server, &dash);
  httpd_register_uri_handler(server, &settingsPage);
  httpd_register_uri_handler(server, &settingsGet);
  httpd_register_uri_handler(server, &settingsSchema);
  httpd_register_uri_handler(server, &userSettingsGet);
  httpd_register_uri_handler(server, &userSettingsSet);
  httpd_register_uri_handler(server, &devicesGet);
  httpd_register_uri_handler(server, &buildConfig);
  httpd_register_uri_handler(server, &apiNotice);
  httpd_register_uri_handler(server, &filesPage);
  httpd_register_uri_handler(server, &filesList);
  httpd_register_uri_handler(server, &filesStats);
  httpd_register_uri_handler(server, &filesCreate);
  httpd_register_uri_handler(server, &filesView);
  httpd_register_uri_handler(server, &filesDelete);
  httpd_register_uri_handler(server, &filesRead);
  httpd_register_uri_handler(server, &filesWrite);
  httpd_register_uri_handler(server, &filesUpload);
  httpd_register_uri_handler(server, &filesRename);
  httpd_register_uri_handler(server, &iconGet);
  httpd_register_uri_handler(server, &iconTestPage);
  httpd_register_uri_handler(server, &loggingPage);
 #if ENABLE_WEB_MAPS
  registerMapsHandlers(server);
 #endif
  registerEdgeImpulseHandlers(server);
  registerESPSRHandlers(server);
  httpd_register_uri_handler(server, &cliPage);
  httpd_register_uri_handler(server, &cliCmd);
  httpd_register_uri_handler(server, &logsGet);
  
  // Module registration functions (sensors, bluetooth, esp-now, games, speech)
 #if ENABLE_WEB_SENSORS
  registerSensorHandlers(server);
 #endif
 #if ENABLE_WEB_BLUETOOTH
  registerBluetoothHandlers(server);
 #endif
 #if ENABLE_WEB_SPEECH
  registerSpeechPageHandlers(server);
 #endif
 #if ENABLE_WEB_ESPNOW
  registerEspNowHandlers(server);
 #endif
 #if ENABLE_WEB_BOND
  registerBondHandlers(server);
 #endif
 #if ENABLE_WEB_MQTT
  registerMqttHandlers(server);
 #endif
 #if ENABLE_WEB_GAMES
  registerGamesHandlers(server);
 #endif
  
  // SSE events endpoint for server-driven notices
  httpd_register_uri_handler(server, &apiEvents);
  httpd_register_uri_handler(server, &systemStatus);
  // Sessions endpoints
  httpd_register_uri_handler(server, &sessionsList);
  httpd_register_uri_handler(server, &adminSessionsList);
#if ENABLE_AUTOMATION
  httpd_register_uri_handler(server, &automationsPage);
  httpd_register_uri_handler(server, &automationsGet);
  httpd_register_uri_handler(server, &automationsExport);
#endif
  httpd_register_uri_handler(server, &outputGet);
  httpd_register_uri_handler(server, &outputTemp);
  httpd_register_uri_handler(server, &regPage);
  httpd_register_uri_handler(server, &regSubmit);
  httpd_register_uri_handler(server, &adminPending);
  httpd_register_uri_handler(server, &adminApprove);
  httpd_register_uri_handler(server, &adminDeny);
  httpd_register_uri_handler(server, &cliBatch);
  
  // Enable web output when server starts
  gOutputFlags |= OUTPUT_WEB;
  
  broadcastOutput("HTTP server started");
}

// Web Mirror Buffer moved to WebCore_Utils.cpp

#endif // ENABLE_HTTP_SERVER
