#ifndef WEBSERVER_SERVER_H
#define WEBSERVER_SERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>

#include "System_User.h"
 
// Shared JSON response buffer size (available to all modules)
// Increased to 12KB to accommodate 64x48 thermal data (~9KB with integers)
#define JSON_RESPONSE_SIZE 12288

// Shared JSON response buffer for web handlers (size defined per TU)
// Buffer is defined in web_server.cpp; other modules use this extern.
extern char* gJsonResponseBuffer;

// ============================================================================
// Web Server - HTTP server session and authentication functions
// ============================================================================

// Session constants
#define MAX_SESSIONS 5
#define MAX_LOGOUT_REASONS 8

// Multi-session support structure
struct SessionEntry {
  String sid;     // session id (cookie value)
  String user;    // username
  String bootId;  // boot ID when session was created (for detecting restarts)
  unsigned long createdAt = 0;
  unsigned long lastSeen = 0;
  unsigned long expiresAt = 0;
  String ip;
  // Small ring buffer for notices to avoid drops during reconnects
  // Using fixed-size char buffers instead of String to save memory and reduce fragmentation
  static const int NOTICE_QUEUE_SIZE = 3;  // Reduced from 8
  static const int NOTICE_MAX_LEN = 128;   // Max notice length
  char noticeQueue[3][128];                // 3 notices × 128 bytes = 384 bytes (vs 8 Strings × 24 bytes = 192 bytes base + heap)
  int nqHead = 0;
  int nqTail = 0;
  int nqCount = 0;
  // Small ring buffer for typed SSE events (event name + JSON data)
  static const int EVENT_QUEUE_SIZE = 3;
  static const int EVENT_NAME_MAX = 16;
  static const int EVENT_DATA_MAX = 192;
  char eventNameQ[EVENT_QUEUE_SIZE][EVENT_NAME_MAX];
  char eventDataQ[EVENT_QUEUE_SIZE][EVENT_DATA_MAX];
  int eqHead = 0;
  int eqTail = 0;
  int eqCount = 0;
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
extern volatile unsigned long gSensorStatusSeq;
extern const char* gLastStatusCause;
extern volatile int gBroadcastSkipSessionIdx;

// Sensor status broadcast (SSE)
void broadcastSensorStatusToAllSessions();

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
bool isAuthedCached(httpd_req_t* req, String& outUser);
bool isAdminUser(const String& who);

// Cookie and header helpers
String getCookieSID(httpd_req_t* req);
bool getHeaderValue(httpd_req_t* req, const char* name, String& out);
bool getCookieValue(httpd_req_t* req, const char* key, String& out);
String makeSessToken();

// Client IP extraction
void getClientIP(httpd_req_t* req, char* ipBuf, size_t bufSize);
void getClientIP(httpd_req_t* req, String& ipOut);

// Session JSON building
void buildAllSessionsJson(const String& currentSid, JsonArray& sessions);
void buildSystemInfoJson(JsonDocument& doc);

// Logout reason management
void storeLogoutReason(const String& ip, const String& reason);
bool hasLogoutReason(const char* ip);

// Session revocation
void enqueueTargetedRevokeForSessionIdx(int idx, const String& reasonMsg);
void sseEnqueueNotice(SessionEntry& s, const String& msg);
bool sseDequeueNotice(SessionEntry& s, String& out);
// Custom SSE event queue helpers
void sseEnqueueEvent(SessionEntry& s, const char* eventName, const String& data);
bool sseDequeueEvent(SessionEntry& s, String& outEventName, String& outData);

// HTTP page handlers
esp_err_t handleSensorsPage(httpd_req_t* req);
esp_err_t handleEspNowPage(httpd_req_t* req);
esp_err_t handleGamesPage(httpd_req_t* req);
esp_err_t handleBluetoothPage(httpd_req_t* req);
esp_err_t handleFileRead(httpd_req_t* req);
esp_err_t handleFileWrite(httpd_req_t* req);
esp_err_t handleFileUpload(httpd_req_t* req);
esp_err_t handleSensorsStatus(httpd_req_t* req);
esp_err_t handleDashboard(httpd_req_t* req);
esp_err_t handleSettingsPage(httpd_req_t* req);
esp_err_t handleSettingsGet(httpd_req_t* req);
esp_err_t handleUserSettingsGet(httpd_req_t* req);
esp_err_t handleUserSettingsSet(httpd_req_t* req);
esp_err_t handleDeviceRegistryGet(httpd_req_t* req);
esp_err_t handleSessionsList(httpd_req_t* req);
esp_err_t handleAdminSessionsList(httpd_req_t* req);
esp_err_t handleOutputGet(httpd_req_t* req);
esp_err_t handleOutputTemp(httpd_req_t* req);
esp_err_t handleNotice(httpd_req_t* req);
esp_err_t handleLogs(httpd_req_t* req);
esp_err_t handleSensorsStatusWithUpdates(httpd_req_t* req);
esp_err_t handleSystemStatus(httpd_req_t* req);
esp_err_t handleCLICommand(httpd_req_t* req);
esp_err_t handleCLIPage(httpd_req_t* req);
esp_err_t handleAutomationsPage(httpd_req_t* req);
esp_err_t handleAutomationsGet(httpd_req_t* req);
esp_err_t handleFilesPage(httpd_req_t* req);
esp_err_t handleLoggingPage(httpd_req_t* req);

// Shared page streaming helper (implemented in main .ino)
void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*));

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

// ESP-NOW API handlers
esp_err_t handleEspNowMessages(httpd_req_t* req);

// Auth logging helper (implemented in main .ino)
void logAuthAttempt(bool success, const char* path, const String& userTried, const String& ip, const String& reason);

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

#endif // WEBSERVER_SERVER_H
