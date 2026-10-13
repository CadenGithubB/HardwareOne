#ifndef WEBSERVER_SERVER_H
#define WEBSERVER_SERVER_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

#include "System_User.h"

// Shared JSON response buffer size (available to all modules).
//
// Sized for the LARGEST response served from this buffer, currently the
// /api/settings/schema endpoint -- it emits every registered SettingEntry
// across every module (debug flags, output channels, wifi, mqtt, espnow,
// camera, all per-sensor modules, etc.). Each entry serializes to ~120-180
// bytes (key, label, type, default, group, cmdKey, +optional min/max/
// options). The codebase currently registers ~330 entries.
//
// History: 32KB was sufficient before commit 13a0629 fixed 7 silent ENABLE_*
// typos (which had been quietly excluding settings modules from the build).
// Re-including those modules pushed the schema serialization to exactly
// 32768 bytes -- the buffer-full check `len >= JSON_RESPONSE_SIZE` then
// fired with `[Schema] Serialization failed or buffer overflow: 32768 >=
// 32768`, returning HTTP 500.
//
// 64KB doubles the headroom. The buffer lives in PSRAM (ps_alloc) so the
// extra 32KB costs essentially nothing on this 8MB-PSRAM target.
//
// If the schema continues to grow past 64KB in the future, options include:
//  (a) bump this further (cheap in PSRAM)
//  (b) emit schema per-module on demand instead of all-at-once
//  (c) use ArduinoJson's measureJson() to size-and-allocate per request
#define JSON_RESPONSE_SIZE 65536

// Shared JSON response buffer for web handlers (size defined per TU)
// Buffer is defined in web_server.cpp; other modules use this extern.
extern char* gJsonResponseBuffer;

// Track whether the running server is HTTPS (true) or HTTP (false)
extern bool gServerIsHttps;

// Global HTTP server handle (defined in HardwareOne.cpp). Declaration lives in
// the lightweight WebServer_Handle.h so non-web modules can get it without this
// heavy header; re-exported here for back-compat.
#include "WebServer_Handle.h"

// ============================================================================
// Web Server - HTTP server session and authentication functions
// ============================================================================

// Session constants
#define MAX_SESSIONS 2
#define MAX_LOGOUT_REASONS 8

// Multi-session support structure
struct SessionEntry {
  String sid;     // session id (cookie value)
  String user;    // username
  String bootId;  // boot ID when session was created (for detecting restarts)
  unsigned long createdAt = 0;
  unsigned long lastSeen = 0;
  unsigned long expiresAt = 0;
  // Last REAL user interaction (millis timebase, 0 = never stamped). Distinct
  // from lastSeen, which is bumped by ALL traffic incl. passive SSE/polls.
  // This is stamped only on genuine user actions (see requestIsInteraction in
  // WebServer_Server.cpp) and drives the shared idle-logout policy
  // (sessionIdleExpired) so automatic chatter never keeps a session alive.
  unsigned long lastInteractionMs = 0;
  String ip;
  // Small ring buffer for notices to avoid drops during reconnects
  // Using fixed-size char buffers instead of String to save memory and reduce fragmentation
  static const int NOTICE_QUEUE_SIZE = 2;
  static const int NOTICE_MAX_LEN = 96;
  char noticeQueue[NOTICE_QUEUE_SIZE][NOTICE_MAX_LEN];  // 2 × 96 = 192 bytes
  int nqHead = 0;
  int nqTail = 0;
  int nqCount = 0;
  // Small ring buffer for typed SSE events (event name + JSON data)
  static const int EVENT_QUEUE_SIZE = 4;
  static const int EVENT_NAME_MAX = 16;
  static const int EVENT_DATA_MAX = 128;
  char eventNameQ[EVENT_QUEUE_SIZE][EVENT_NAME_MAX];  // 4 × 16 = 64 bytes
  char eventDataQ[EVENT_QUEUE_SIZE][EVENT_DATA_MAX];  // 4 × 128 = 512 bytes
  int eqHead = 0;
  int eqTail = 0;
  int eqCount = 0;
  // Drop-oldest counters for the two rings above (diagnostics; reset with the
  // session entry). Surfaced via sseEventDropsTotal() / notifstats.
  uint16_t eqDropped = 0;
  uint16_t nqDropped = 0;
  // When a notice is queued, enter a short 'burst' window to reconnect faster
  unsigned long noticeBurstUntil = 0;  // millis() timestamp; 0 means idle
  bool needsNotificationTick = false;  // set when there are pending notices to deliver
  uint32_t lastSensorSeqSent = 0;      // last sensor-status sequence sent over SSE
  bool needsStatusUpdate = false;      // flag to trigger status refresh on next request
  int sockfd = -1;                     // socket file descriptor for force disconnect
  bool revoked = false;                // session has been revoked but kept alive for notice delivery
};

// Session array (defined in main .ino)
extern SessionEntry* gSessions;

// Logout reason structure (defined in main .ino)
struct LogoutReason {
  String ip;
  String reason;
  unsigned long timestamp;
};
extern LogoutReason* gLogoutReasons;

// Boot-local session-generation lookup used to fence queued commands and
// interactive CLI ownership. The value is not wall-clock/NTP time.
TransportSessionEpoch webSessionEpochForSID(const String& sid);
bool webSessionEpochIsLive(TransportSessionEpoch epoch);

#if ENABLE_HTTP_SERVER

// ============================================================================
// Brute-Force / Login Rate Limiting (web adapters)
// ============================================================================
// The lockout table, its tiers and getTotalFailedLoginCount() live in
// System_User (credentialAttempt*), keyed by transport plus peer; these
// adapters key the web by client IP. /login and Basic Auth call the owner
// directly because they know the account that was tried.

// True if the given IP may not try a credential now (remainingMs, optional,
// receives the time left).
bool isLoginLocked(const char* ip, unsigned long* remainingMs = nullptr);

// Count a failed attempt that names no account (registration intake).
void recordFailedLogin(const char* ip);

// Drop the IP's attempt record (an admin ban replaces it).
void clearLoginAttempts(const char* ip);

// ============================================================================
// IP Ban List (permanent, admin-managed, persisted to flash)
// ============================================================================

#define MAX_IP_BANS   16
#define IP_BANS_FILE  "/system/users/ip_bans.json"

// A single ban entry. Fixed char arrays — no String, no heap fragmentation.
struct IpBanEntry {
  char ip[40];             // IPv4/IPv6 as C-string; ip[0]=='\0' means unused
  char reason[80];         // optional admin note
  unsigned long bannedAt;  // millis() when ban was created (for display only)
};

// Load ban list from flash into memory (called once at server start).
void loadIpBans();

// Returns true if the given IP is in the permanent ban list.
bool isIpBanned(const char* ip);

// Add an IP to the ban list, kick any active sessions from that IP, and persist.
// Returns false if the list is full or the save fails. A ban whose save fails
// stays in force in memory until reboot (a not-saved notice is broadcast);
// banning the same IP again retries the save.
bool banIp(const char* ip, const char* reason = nullptr);

// Remove an IP from the ban list and persist.
// Returns false if the IP was not found in the list, or if the save fails; a
// failed save keeps the ban in force so memory matches the saved list.
bool unbanIp(const char* ip);

// Broadcast the current ban list to all connected clients (used by cmd_banlist).
void broadcastBanList();

// JSON form of the IP ban list (used by `banlist json`). Defined alongside the
// ban data in WebServer_Server.cpp; only linked in HTTP-enabled builds.
// Returns the whole document or a one-field {"error":...} object.
const char* banListJson();

extern volatile unsigned long gSensorStatusSeq;
extern const char* gLastStatusCause;
extern volatile int gBroadcastSkipSessionIdx;

// Sensor status broadcast (SSE)
void broadcastSensorStatusToAllSessions();

// Broadcast a typed SSE event to all active sessions (used by System_Notifications)
void broadcastEventToAllSessions(const char* eventName, const char* jsonData);

// Predicate-filtered variant: only sessions whose authenticated username
// passes `allow` receive the event (per-user notification visibility).
// `allow` runs without the session lock on a copy of the username and may
// read flash; the event is dropped if the slot changed hands meanwhile.
void broadcastEventToSessionsIf(const char* eventName, const char* jsonData,
                                bool (*allow)(const char* username, void* arg), void* arg);

// Sum of typed-event drop-oldest counts across CURRENT sessions (counters
// reset when a session entry is recycled). Used by `notifstats`.
uint32_t sseEventDropsTotal();

// Locked queue access for the SSE handler: slot `idx` must still hold the
// session `sid`, otherwise nothing is dequeued. The settle call clears the
// pending-delivery flag when both queues are empty. The status call records a
// delivered sensor-status snapshot taken at `seqSent` and clears the update
// flag only if no newer status was published since.
bool webSessionDequeueSseEvent(int idx, const String& sid,
                               String& outEventName, String& outData);
bool webSessionDequeueSseNotice(int idx, const String& sid, String& out);
void webSessionSettleSseQueues(int idx, const String& sid);
void webSessionMarkStatusSent(int idx, const String& sid, unsigned long seqSent);

// ============================================================================
// Session Management Functions
// ============================================================================

// Session lookup and management
int findSessionIndexBySID(const String& sid);
int findFreeSessionIndex();
void pruneExpiredSessions();
int sseBindSession(httpd_req_t* req, String& outSid);

// Session creation and destruction
String setSession(httpd_req_t* req, const String& u);
void clearSession(httpd_req_t* req, const char* logoutReason = nullptr);

// Unified authentication success handler (handles HTTP, Serial, TFT transports)
// TODO: Consider decoupling Serial auth handling to separate module
esp_err_t authSuccessUnified(AuthContext& ctx, const char* redirectTo);

// Authentication
bool isAuthed(httpd_req_t* req, String& outUser);
bool isAdminUser(const String& who);

// Cookie and header helpers
String getCookieSID(httpd_req_t* req);
bool getHeaderValue(httpd_req_t* req, const char* name, String& out);
bool getCookieValue(httpd_req_t* req, const char* key, String& out);
String makeSessToken();

// Client IP extraction
void getClientIP(httpd_req_t* req, char* ipBuf, size_t bufSize);
void getClientIP(httpd_req_t* req, String& ipOut);

// Construct a SOURCE_WEB AuthContext from an HTTP request. The SID is routing
// metadata; queued command authority is captured separately as a numeric
// transport-session epoch.
inline AuthContext makeWebAuthCtx(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = req ? req->uri : "";
  getClientIP(req, ctx.ip);
  ctx.sid = getCookieSID(req);
  return ctx;
}

// Build the admin-facing session array across web, Serial, UART, OLED, G2,
// and all authenticated BLE connections. Web entries carry their richer
// cookie/IP/timestamp metadata; native transport entries carry user+transport.
void buildAllSessionsJson(const String& currentSid, JsonArray& sessions);

// Logout reason management
void storeLogoutReason(const String& ip, const String& reason);
bool hasLogoutReason(const char* ip);

// Session revocation
void enqueueTargetedRevokeForSessionIdx(int idx, const String& reasonMsg);
// Exact, table-locked web-session helpers for callers outside the HTTP TU.
// They avoid racing slot reuse while inspecting Arduino String fields.
int webRevokeSessionsForUser(const String& username,
                             const String& reason,
                             const String& exceptSid = String());
bool webRevokeSessionBySid(const String& sid,
                           const String& reason,
                           String* outUser = nullptr);
int webBroadcastSessionsHuman();
void sseEnqueueNotice(SessionEntry& s, const String& msg);
bool sseDequeueNotice(SessionEntry& s, String& out);
// Custom SSE event queue helpers
void sseEnqueueEvent(SessionEntry& s, const char* eventName, const char* data);
bool sseDequeueEvent(SessionEntry& s, String& outEventName, String& outData);

// HTTP page handlers
esp_err_t handleSensorsPage(httpd_req_t* req);
esp_err_t handleBluetoothPage(httpd_req_t* req);
esp_err_t handleFileRead(httpd_req_t* req);
esp_err_t handleFileWrite(httpd_req_t* req);
esp_err_t handleFileUpload(httpd_req_t* req);
esp_err_t handleSensorsStatus(httpd_req_t* req);
esp_err_t handleDashboard(httpd_req_t* req);
esp_err_t handlePasswordChangePage(httpd_req_t* req);
esp_err_t handleSettingsPage(httpd_req_t* req);
esp_err_t handleSettingsGet(httpd_req_t* req);
esp_err_t handleUserSettingsGet(httpd_req_t* req);
esp_err_t handleUserSettingsSet(httpd_req_t* req);
esp_err_t handleDeviceRegistryGet(httpd_req_t* req);
esp_err_t handleSessionsList(httpd_req_t* req);
esp_err_t handleAdminSessionsList(httpd_req_t* req);
esp_err_t handleNotice(httpd_req_t* req);
esp_err_t handleLogs(httpd_req_t* req);
esp_err_t handleSensorsStatusWithUpdates(httpd_req_t* req);
esp_err_t handleSystemStatus(httpd_req_t* req);
esp_err_t handleCLICommand(httpd_req_t* req);
esp_err_t handleCliBatch(httpd_req_t* req);
esp_err_t handleCLIPage(httpd_req_t* req);
esp_err_t handleAutomationsPage(httpd_req_t* req);
esp_err_t handleAutomationsGet(httpd_req_t* req);
esp_err_t handleFilesPage(httpd_req_t* req);
esp_err_t handleLoggingPage(httpd_req_t* req);

// Shared page streaming helper (implemented in main .ino)
void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*, const String&));

// Simple handlers (Batch 1)
esp_err_t handleRoot(httpd_req_t* req);
esp_err_t handlePing(httpd_req_t* req);
esp_err_t handleLogout(httpd_req_t* req);

// Auth handlers (Batch 2)
esp_err_t handleLogin(httpd_req_t* req);
esp_err_t sendAuthRequiredResponse(httpd_req_t* req);
esp_err_t handleLoginSetSession(httpd_req_t* req);
esp_err_t handleRegisterPage(httpd_req_t* req);
esp_err_t handleRegisterSubmit(httpd_req_t* req);

// File handlers (Batch 3)
esp_err_t handleFilesList(httpd_req_t* req);
esp_err_t handleFilesStats(httpd_req_t* req);
esp_err_t handleFilesCreate(httpd_req_t* req);
esp_err_t handleFileView(httpd_req_t* req);
esp_err_t handleFileDelete(httpd_req_t* req);

// Admin handlers (Batch 4)
esp_err_t handleAdminPending(httpd_req_t* req);
esp_err_t handleAdminApproveUser(httpd_req_t* req);
esp_err_t handleAdminDenyUser(httpd_req_t* req);

// Automation handlers (Batch 5)
esp_err_t handleAutomationsExport(httpd_req_t* req);

// Sensor handlers (Batch 5)
esp_err_t handleSensorData(httpd_req_t* req);
esp_err_t handleRemoteSensors(httpd_req_t* req);
esp_err_t handleCameraStatus(httpd_req_t* req);
esp_err_t handleCameraFrame(httpd_req_t* req);
esp_err_t handleMicRecordingsList(httpd_req_t* req);
esp_err_t handleMicRecordingFile(httpd_req_t* req);
esp_err_t handleMicRecordingDelete(httpd_req_t* req);

// logAuthAttempt / recordLoginAttempt now live in System_User.h — they are
// transport-neutral and must stay available when ENABLE_HTTP_SERVER=0.

// SSE helpers moved from .ino
bool sseSessionAliveAndRefresh(int sessIdx, const String& sid);
bool sseSendLogs(httpd_req_t* req, unsigned long seq, const String& buf);
bool sseWrite(httpd_req_t* req, const char* chunk);
void sseDebug(const String& msg);
bool sseHeartbeat(httpd_req_t* req);
bool sseSendNotice(httpd_req_t* req, const String& note);
esp_err_t handleEvents(httpd_req_t* req);

// HTTP server management
void startHttpServer();

// HTTP streaming helpers
esp_err_t streamChunkC(httpd_req_t* req, const char* s);
esp_err_t streamChunkBuf(httpd_req_t* req, const char* buf, size_t len);
void streamBeginHtml(httpd_req_t* req, const String& title, const String& username = "");
void streamEndHtml(httpd_req_t* req);
void streamNav(httpd_req_t* req, const String& username, const String& activePage);
void streamContentGeneric(httpd_req_t* req, const String& content);
void streamChunk(httpd_req_t* req, const String& str);
void streamChunk(httpd_req_t* req, const char* str);

#endif // ENABLE_HTTP_SERVER

#endif // WEBSERVER_SERVER_H
