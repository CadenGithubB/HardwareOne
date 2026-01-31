#include "Arduino.h"
// Forward declarations to satisfy Arduino's auto-generated prototypes
struct AuthContext;
struct CommandContext;
struct Command;
struct MeshPeerHealth;
struct TopologyStream;
struct Message;
struct RouterMetrics;
struct ChunkBuffer;
struct ReceivedMessage;

#include "System_Utils.h"

extern const CommandEntry commands[];
extern const size_t commandsCount;

static String originPrefix(const char* source, const String& user, const String& ip);
void runUnifiedSystemCommand(const String& cmd);

extern uint32_t gLastHeartbeatSentMs;
extern const uint32_t MESH_HEARTBEAT_INTERVAL_MS;

// Web server functions (implemented in web_server.cpp) - declared here to prevent Arduino preprocessor from creating static versions
struct httpd_req;
typedef struct httpd_req httpd_req_t;
void getClientIP(httpd_req_t* req, String& ipOut);
void getClientIP(httpd_req_t* req, char* ipBuf, size_t bufSize);

#include "System_BuildConfig.h"  // Must be early for ENABLE_* and NETWORK_FEATURE_LEVEL

#if ENABLE_WIFI
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#endif
// WiFi stub is provided by sensor_stubs_minimal.h when ENABLE_WIFI=0

#if ENABLE_HTTP_SERVER
  #include <esp_http_server.h>
#endif
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include "mbedtls/base64.h"
#include <lwip/sockets.h>
#include <ArduinoJson.h>
#if ENABLE_HTTP_SERVER
  #include "WebServer_Utils.h"
  #include "WebPage_Login.h"
  #include "WebPage_LoginSuccess.h"
  #include "WebPage_LoginRequired.h"
  #include "WebPage_Dashboard.h"
  #include "WebPage_CLI.h"
  #include "WebPage_Files.h"
  #include "WebPage_Logging.h"
  #include "WebPage_Settings.h"
  #include "WebPage_Sensors.h"
  #if ENABLE_AUTOMATION
    #include "WebPage_Automations.h"
  #endif
  #include "WebPage_ESPNow.h"
#endif

#if ENABLE_ESPNOW
  #include "System_ESPNow.h"
#endif
#include "Optional_Bluetooth.h"
#include "System_Automation.h"
#include "System_Utils.h"
#include "System_User.h"
#include "System_Filesystem.h"
#include "System_CLI.h"
#include "System_I2C.h"
#include "System_Logging.h"
#include "System_Debug.h"
#if ENABLE_WIFI
  #include "System_WiFi.h"
#endif
#if ENABLE_MQTT
  #include "System_MQTT.h"
#endif
#include "System_Command.h"
#if ENABLE_HTTP_SERVER
  #include "WebServer_Server.h"
#endif
#include "System_Battery.h"
#include "System_FirstTimeSetup.h"
#include "System_TaskUtils.h"
// sensor_config.h included early (before WiFi)
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor-mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor-vl53l4cx.h"
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor-bno055.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor-seesaw.h"
#endif
#if ENABLE_APDS_SENSOR
  #include "i2csensor-apds9960.h"
#endif
#if ENABLE_GPS_SENSOR
  #include "i2csensor-pa1010d.h"
#endif
#if ENABLE_RTC_SENSOR
  #include "i2csensor-ds3231.h"
#endif
#include "System_SensorStubs.h"  // Minimal stubs only where required
#include "i2csensor-rda5807.h"
#include "OLED_Display.h"  // Always include - wrapper functions are safe to call when disabled
#include "System_NeoPixel.h"
#include "System_MemoryMonitor.h"
#if ENABLE_HTTP_SERVER && ENABLE_GAMES
  #include "WebPage_Games.h"
#endif
#if ENABLE_WIFI
  #include <lwip/netdb.h>
  #include <arpa/inet.h>
  #include <esp_wifi.h>
#endif
#if ENABLE_ESPNOW
  #include <esp_now.h>
#endif
#include <memory>
#include <ctype.h>
#include <Wire.h>
#include <string.h>
#if ENABLE_GAMEPAD_SENSOR
#include "Adafruit_seesaw.h"
#endif
#if ENABLE_IMU_SENSOR
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <utility/imumaths.h>
#endif
#include <Adafruit_NeoPixel.h>
#if ENABLE_APDS_SENSOR
#include "Adafruit_APDS9960.h"
#endif
#if ENABLE_TOF_SENSOR
#include "vl53l4cx_class.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include <Adafruit_MLX90640.h>
#endif
#if ENABLE_OLED_DISPLAY
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif
#include <Adafruit_PWMServoDriver.h>
#if ENABLE_GPS_SENSOR
#include <Adafruit_GPS.h>
#endif
#include <vector>
#include <functional>
#include "System_MemUtil.h"

bool createGamepadTask();
void ensureDeviceRegistryFile();
bool isSensorConnected(const char* moduleName);
void setCurrentCommandContext(const CommandContext& ctx);
bool initGamepad();


// Pre-allocation snapshots (used by mem_util.h)
size_t gAllocHeapBefore = 0;
size_t gAllocPsBefore = 0;

// Dynamic allocation tracker - aggregates allocations by tag
struct AllocEntry {
  char tag[24];
  size_t totalBytes;
  size_t psramBytes;  // How much went to PSRAM
  size_t dramBytes;   // How much went to DRAM
  uint16_t count;
  bool isActive;
};
extern const int MAX_ALLOC_ENTRIES = 64;
AllocEntry gAllocTracker[MAX_ALLOC_ENTRIES];
int gAllocTrackerCount = 0;
bool gAllocTrackerEnabled = false;

// Global flag to indicate CLI dry-run validation mode (no side effects)
bool gCLIValidateOnly = false;

// Helper: early-return for validate-only mode inside command branches
#define RETURN_VALID_IF_VALIDATE() \
  do { \
    if (gCLIValidateOnly) return String("VALID"); \
  } while (0)

// Forward declarations for debug output sink used in macros
void broadcastOutput(const String& s);
void broadcastOutput(const char* s);
void broadcastOutput(const String& s, const CommandContext& ctx);

// MEMORY OPTIMIZATION: Printf-style broadcastOutput using stack-local buffer
// Thread-safe: each caller uses its own stack for formatting (no shared gDebugBuffer)
#ifndef BROADCAST_PRINTF
#define BROADCAST_PRINTF(fmt, ...) \
  do { \
    char _bpBuf[256]; \
    snprintf(_bpBuf, sizeof(_bpBuf), fmt, ##__VA_ARGS__); \
    broadcastOutput(_bpBuf); \
  } while (0)
#endif

// Context-aware version for commands that need user/source attribution
#ifndef BROADCAST_PRINTF_CTX
#define BROADCAST_PRINTF_CTX(ctx, fmt, ...) \
  do { \
    char _bpBuf[256]; \
    snprintf(_bpBuf, sizeof(_bpBuf), fmt, ##__VA_ARGS__); \
    broadcastOutput(_bpBuf, ctx); \
  } while (0)
#endif


#ifndef DEBUG_MEM_SUMMARY
#define DEBUG_MEM_SUMMARY 0
#endif

// File paths (LittleFS)
const char* SETTINGS_JSON_FILE = "/system/settings.json";  // Non-static for settings.cpp
#if ENABLE_AUTOMATION
const char* AUTOMATIONS_JSON_FILE = "/system/automations.json";
#endif

#include "System_Mutex.h"

// Forward declarations
class VL53L4CX;
class Adafruit_BNO055;
extern VL53L4CX* gVL53L4CX;
bool appendLineWithCap(const char* path, const String& line, size_t capBytes);
String setSession(httpd_req_t* req, const String& u);
String getCookieSID(httpd_req_t* req);
void buildAllSessionsJson(const String& currentSid, JsonArray& sessions);
void broadcastWithOrigin(const String& channel, const String& user, const String& origin, const String& message);

#ifndef DEBUGF
// Debug macros - only emit if flag is set (uses accessor functions)
#define DEBUGF(flag, fmt, ...) \
  do { \
    if (isDebugFlagSet(flag)) { \
      if (ensureDebugBuffer()) { \
        snprintf(getDebugBuffer(), 1024, fmt, ##__VA_ARGS__); \
        /* Use Serial direct to avoid stressing HTTP task stack via web history writes */ \
        Serial.printf("[" #flag "] %s\n", getDebugBuffer()); \
      } \
    } \
  } while (0)
#endif

// NOTE: Debug macros (DEBUGF_RING, DEBUGF_BROADCAST, DEBUG_*F) now defined in debug_system.h
// All debug functionality moved to debug_system.cpp for proper encapsulation
// Security debug always on - now also using broadcastOutput
// OPTIMIZED: Uses const char* overload to avoid String allocation
#ifndef DEBUG_SECURITYF
#define DEBUG_SECURITYF(fmt, ...) \
  do { \
    if (ensureDebugBuffer()) { \
      snprintf(getDebugBuffer(), 1024, "[SECURITY] " fmt, ##__VA_ARGS__); \
      broadcastOutput(getDebugBuffer()); \
    } \
  } while (0)
#endif

void setupWiFi();

// WiFi global flags (defined in wifi_system.cpp)
extern volatile bool gWifiUserCancelled;
extern bool gSkipNTPInWifiConnect;

// Output/logging used widely before definition

// Global variables forward declarations
extern bool filesystemReady;

// Centralized command execution with AuthContext (transport-agnostic)
static const char* originFrom(const AuthContext& ctx);
bool hasAdminPrivilege(const AuthContext& ctx);

struct ExecReq;
QueueHandle_t gCmdExecQ = nullptr;
static void commandExecTask(void* pv);
bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);
bool submitAndExecuteSync(const Command& cmd, String& out);
String execCommandUnified(const CommandContext& baseCtx, const String& line);
bool executeUnifiedWebCommand(httpd_req_t* req, AuthContext& ctx, const String& cmd, String& out);
String resolveRegistryCommandKey(const String& line);
bool adminRequiredForLine(const String& line);
void broadcastOutput(const String& s);
void broadcastOutput(const char* s);
void broadcastOutput(const String& s, const CommandContext& ctx);
// Auth/session helpers used early
bool isAuthed(httpd_req_t* req, String& outUser);
void logAuthAttempt(bool success, const char* path, const String& userTried, const String& ip, const String& reason);
extern String gBootId;
extern String gAuthUser;
extern String gAuthPass;
extern String gExpectedAuthHeader;
void rebuildExpectedAuthHeader();

// ---------------------------------------------------------------------------
// Serial auth globals
// ---------------------------------------------------------------------------
// Boot sequence tracking for user creation timestamp resolution
// Exported to user_system.cpp
uint32_t gBootSeq = 0;
uint32_t gBootCounter = 0;

bool gSerialAuthed = false;
String gSerialUser = String();

bool gLocalDisplayAuthed = false;
String gLocalDisplayUser = String();

// Bluetooth authentication (per-connection, separate from other transports)
// Exported to user_system.cpp and bluetooth modules
bool gBluetoothAuthed = false;
String gBluetoothUser = String();

esp_err_t handleSensorsStatus(httpd_req_t* req);

volatile bool gSensorPollingPaused = false;

#include "System_SensorLogging.h"

// Global sensor-status sequence for SSE fanout
volatile unsigned long gSensorStatusSeq = 1;
// Forward declaration for SSE broadcast
void broadcastSensorStatusToAllSessions();
// Index of a session to skip when flagging updates (set around command handling)
volatile int gBroadcastSkipSessionIdx = -1;
// Last known cause for a sensor status bump (for diagnostics)
const char* gLastStatusCause = "";
// Debounced SSE broadcast state
static volatile bool gSensorStatusDirty = false;
static volatile unsigned long gNextSensorStatusBroadcastDue = 0;
static const unsigned long kSensorStatusDebounceMs = 150;  // 100–200ms window

void sensorStatusBump() {
  uint32_t s = gSensorStatusSeq + 1;
  if (s == 0) s = 1;
  gSensorStatusSeq = s;
  DEBUG_SENSORSF("[STATUS_BUMP] seq=%lu cause='%s' | thermal=%d tof=%d imu=%d gamepad=%d",
                 (unsigned long)gSensorStatusSeq, gLastStatusCause,
                 thermalEnabled ? 1 : 0, tofEnabled ? 1 : 0, imuEnabled ? 1 : 0, gamepadEnabled ? 1 : 0);
  DEBUG_SSEF("sensorStatusBump: seq now %lu | cause=%s (debounced)", (unsigned long)gSensorStatusSeq, gLastStatusCause);
  // Mark dirty and schedule debounced broadcast
  gSensorStatusDirty = true;
  unsigned long nowMs = millis();
  if (gNextSensorStatusBroadcastDue == 0 || (long)(nowMs - gNextSensorStatusBroadcastDue) > 0) {
    gNextSensorStatusBroadcastDue = nowMs + kSensorStatusDebounceMs;
    DEBUG_SENSORSF("[STATUS_BUMP] Broadcast scheduled for %lu ms from now", kSensorStatusDebounceMs);
  } else {
    DEBUG_SENSORSF("[STATUS_BUMP] Broadcast already scheduled (due in %ld ms)", (long)(gNextSensorStatusBroadcastDue - nowMs));
  }
}

extern Adafruit_NeoPixel pixels;
extern BatteryState gBatteryState;

// Legacy auth defaults (still used by loadUsersFromFile)
static String DEFAULT_AUTH_USER = "admin";
static String DEFAULT_AUTH_PASS = "admin";

// Globals
#if ENABLE_HTTP_SERVER
httpd_handle_t server = NULL;
#endif
Preferences prefs;

// Response buffer sizes for web handlers
static const size_t TOF_RESPONSE_SIZE = 1024;      // 1KB sufficient for 4 ToF objects
static const size_t IMU_RESPONSE_SIZE = 512;       // 512 bytes sufficient for IMU data (accel, gyro, ori, temp)
static const size_t THERMAL_RESPONSE_SIZE = 8192;  // 8KB typically fits 32x24 frame; larger interpolated frames will fallback

volatile unsigned long gWebMirrorSeq = 0;
String gLastTFTLine;

String gExecUser = "";
bool gExecIsAdmin = false;
AuthContext gExecAuthContext;

#include "System_Settings.h"
Settings gSettings;

String gSerialCLI = "";

#if ENABLE_WIFI
WifiNetwork* gWifiNetworks = nullptr;
int gWifiNetworkCount = 0;
#endif

extern "C" void __attribute__((weak)) memAllocDebug(const char* op, void* ptr, size_t size,
                                                    bool requestedPS, bool usedPS, const char* tag) {
  // Update allocation tracker if enabled (lightweight, no FS access)
  if (gAllocTrackerEnabled && tag && ptr) {
    // Find or create entry for this tag
    int idx = -1;
    for (int i = 0; i < gAllocTrackerCount; i++) {
      if (strcmp(gAllocTracker[i].tag, tag) == 0) {
        idx = i;
        break;
      }
    }
    if (idx == -1 && gAllocTrackerCount < MAX_ALLOC_ENTRIES) {
      idx = gAllocTrackerCount++;
      strncpy(gAllocTracker[idx].tag, tag, sizeof(gAllocTracker[idx].tag) - 1);
      gAllocTracker[idx].tag[sizeof(gAllocTracker[idx].tag) - 1] = '\0';
      gAllocTracker[idx].totalBytes = 0;
      gAllocTracker[idx].psramBytes = 0;
      gAllocTracker[idx].dramBytes = 0;
      gAllocTracker[idx].count = 0;
      gAllocTracker[idx].isActive = true;
    }
    if (idx >= 0) {
      gAllocTracker[idx].totalBytes += size;
      gAllocTracker[idx].count++;
      // Track actual memory type used (usedPS tells us where it ended up)
      if (usedPS) {
        gAllocTracker[idx].psramBytes += size;
      } else {
        gAllocTracker[idx].dramBytes += size;
      }
    }
  }

  // Avoid work if filesystem not ready
  if (!filesystemReady) return;
  // Reentrancy guard to prevent recursion when logging triggers allocations
  static volatile bool s_inMemLog = false;
  if (s_inMemLog) return;
  // Deadlock safeguard: if current task already holds fsMutex, skip FS logging
  if (isFsLockedByCurrentTask()) return;
  s_inMemLog = true;
  // Ensure /logs exists (best-effort)
  {
    FsLockGuard guard("alloclog.ensure_logs");
    if (!LittleFS.exists("/logs")) {
      LittleFS.mkdir("/logs");
    }
  }
  // Timestamp prefix with ms precision, via boot-epoch offset and esp_timer
  char tsPrefix[40];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));

  // After values
  size_t heapAfter = ESP.getFreeHeap();
  size_t psTot = ESP.getPsramSize();
  size_t psAfter = (psTot > 0) ? ESP.getFreePsram() : 0;

  // Deltas (positive means memory consumed)
  long heapDelta = (long)gAllocHeapBefore - (long)heapAfter;
  long psDelta = (long)gAllocPsBefore - (long)psAfter;

  // Build one-line entry with before/after info
  // Format: [YYYY-mm-dd HH:MM:SS] | ms=<millis> op=... size=... reqPS=0|1 usedPS=0|1 ptr=0x... tag=...
  //         heapBefore=... heapAfter=... heapDelta=... [psBefore=... psAfter=... psDelta=...]
  String line;
  line.reserve(200);
  // Always include a prefix; if NTP time isn't ready yet or invalid, use a stable fallback
  bool ok = (tsPrefix[0] == '[');
  if (ok) {
    for (size_t i = 1; tsPrefix[i] && i < sizeof(tsPrefix); ++i) {
      if (tsPrefix[i] == ']') {
        ok = true;
        break;
      }
      if (i == sizeof(tsPrefix) - 1) ok = false;
    }
  }
  String prefix = ok ? String(tsPrefix) : String("[BOOTING] | ");
  line += prefix;
  line += "ms=";
  line += String(millis());
  line += " op=";
  line += (op ? op : "?");
  line += " size=";
  line += String((unsigned long)size);
  line += " reqPS=";
  line += (requestedPS ? "1" : "0");
  line += " usedPS=";
  line += (usedPS ? "1" : "0");
  line += " ptr=0x";
  line += String((uint32_t)ptr, HEX);
  if (tag && tag[0]) {
    line += " tag=";
    line += tag;
  }

  line += " heapBefore=";
  line += String(gAllocHeapBefore);
  line += " heapAfter=";
  line += String(heapAfter);
  line += " heapDelta=";
  line += String(heapDelta);

  if (psTot > 0) {
    line += " psBefore=";
    line += String(gAllocPsBefore);
    line += " psAfter=";
    line += String(psAfter);
    line += " psDelta=";
    line += String(psDelta);
  }

  // Memory allocation logging removed - LOG_ALLOC_FILE is obsolete
  s_inMemLog = false;
}

#if ENABLE_HTTP_SERVER
void sseEnqueueNotice(SessionEntry& s, const String& msg);
bool sseDequeueNotice(SessionEntry& s, String& out);
#endif

volatile uint32_t gOutputFlags = OUTPUT_SERIAL;

// Remove ANSI CSI escape sequences (e.g., ESC[2J, ESC[H, ESC[1;32m) for serial cleanliness
static String stripANSICSI(const String& in) {
  String out;
  out.reserve(in.length());
  size_t i = 0, n = in.length();
  while (i < n) {
    char c = in.charAt(i);
    if (c == 0x1B) {  // ESC
      // Handle CSI sequences that start with ESC '[' and end with a final byte @..~
      if (i + 1 < n && in.charAt(i + 1) == '[') {
        i += 2;  // skip ESC[
        while (i < n) {
          char d = in.charAt(i);
          // Final byte in CSI is in range @ (0x40) to ~ (0x7E)
          if (d >= '@' && d <= '~') {
            i++;
            break;
          }
          i++;
        }
        continue;  // skip entire CSI
      } else {
        // Skip solitary ESC or non-CSI sequences conservatively
        i++;
        continue;
      }
    }
    out += c;
    i++;
  }
  return out;
}

static inline void printToSerial(const String& s) {
  Serial.println(stripANSICSI(s));
}

static inline void printToTFT(const String& s) {
  gLastTFTLine = s;
}

void appendCommandToFeed(const char* source, const String& cmd, const String& user = String(), const String& ip = String()) {
  String line = "[";
  line += source;
  if (user.length() || ip.length()) {
    line += " ";
    if (user.length()) { line += user; }
    if (ip.length()) {
      line += "@";
      line += ip;
    }
  }
  line += "] $ ";
  line += cmd;
  printToWeb(line);
}

static String originPrefix(const char* source, const String& user, const String& ip) {
  String p = "[";
  p += source;
  if (user.length() || ip.length()) {
    p += " ";
    if (user.length()) { p += user; }
    if (ip.length()) {
      p += "@";
      p += ip;
    }
  }
  p += "] ";
  return p;
}

#if ENABLE_HTTP_SERVER
static inline void broadcastWithOrigin(const char* source, const String& user, const String& ip, const String& msg) {
  DEBUG_SSEF("broadcastWithOrigin called: source='%s' user='%s' ip='%s' msg='%s'",
             source ? source : "NULL", user.c_str(), ip.c_str(), msg.c_str());

  // Debug: Show all active sessions
  DEBUG_SSEF("Active sessions count: %d", MAX_SESSIONS);
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (gSessions[i].user.length() > 0) {
      DEBUG_SSEF("  [%d] user='%s' sid='%s' sockfd=%d expires=%lu ip='%s'",
                 i, gSessions[i].user.c_str(), gSessions[i].sid.c_str(),
                 gSessions[i].sockfd, gSessions[i].expiresAt, gSessions[i].ip.c_str());
    }
  }

  // Check if this is a targeted message (ip parameter contains username instead of IP)
  bool isTargetedMessage = false;
  String targetUser = "";

  // If ip doesn't contain ":" or "." it's likely a username, not an IP
  if (ip.length() > 0 && ip.indexOf(':') == -1 && ip.indexOf('.') == -1) {
    isTargetedMessage = true;
    targetUser = ip;
    DEBUG_SSEF("Detected targeted message to user: '%s'", targetUser.c_str());
  }

  if (isTargetedMessage) {
    // Find the target user's session
    bool userFound = false;
    for (int i = 0; i < MAX_SESSIONS; i++) {
      if (gSessions[i].user.length() > 0 && gSessions[i].user == targetUser) {
        DEBUG_SSEF("Found target user session [%d] - sending targeted message", i);

        // Create the message with proper prefix
        String targetedMsg = originPrefix(source ? source : "system", user, targetUser) + msg;

        // Send message directly to this specific session's notice queue
        DEBUG_SSEF("Sending to session: sockfd=%d sid='%s'", gSessions[i].sockfd, gSessions[i].sid.c_str());
        sseEnqueueNotice(gSessions[i], targetedMsg);
        DEBUG_SSEF("Message queued for user '%s' (qCount=%d)", targetUser.c_str(), gSessions[i].nqCount);

        userFound = true;
        break;
      }
    }

    if (!userFound) {
      DEBUG_SSEF("Target user '%s' not found in active sessions", targetUser.c_str());
      broadcastOutput("[ERROR] User '" + targetUser + "' not found or not logged in");
    }
  } else {
    // Regular broadcast to all users
    DEBUG_SSEF("Regular broadcast to all users");

    // Session-only: if origin is serial and serial sink is disabled, enable for this session
    if (source && strcmp(source, "serial") == 0) {
      if (!(gOutputFlags & OUTPUT_SERIAL)) {
        gOutputFlags |= OUTPUT_SERIAL;  // session-only; do not modify persisted settings
      }
    }
    // Prefix and broadcast via simple sinks
    broadcastOutput(originPrefix(source ? source : "system", user, ip) + msg);
  }
}
#endif // ENABLE_HTTP_SERVER

// applySettings() moved to settings.cpp

// ==========================
// URL query helpers
// ==========================

#if ENABLE_HTTP_SERVER
static bool getQueryParam(httpd_req_t* req, const char* key, String& out) {
  out = "";
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0) return false;
  std::unique_ptr<char, void (*)(void*)> qbuf((char*)ps_alloc(qlen + 1, AllocPref::PreferPSRAM, "http.query"), free);
  if (httpd_req_get_url_query_str(req, qbuf.get(), qlen + 1) != ESP_OK) return false;
  char val[256];
  if (httpd_query_key_value(qbuf.get(), key, val, sizeof(val)) == ESP_OK) {
    out = String(val);
    return true;
  }
  return false;
}
#endif // ENABLE_HTTP_SERVER

// jsonEscape moved to system_utils.cpp

// Output flags API
// ==========================

// ==========================
// HTTP handlers
// ==========================
// Note: handleRoot, handlePing, handleLogout, handleLogin, sendAuthRequiredResponse,
//       handleLoginSetSession, handleRegisterPage, handleRegisterSubmit moved to web_server.cpp
// (declarations now in web_server.h)

// Protected dashboard moved to web_server.cpp

// Sensor JSON building functions moved to respective sensor files (thermal_sensor.cpp, tof_sensor.cpp, imu_sensor.cpp)

// Note: handlePing moved to web_server.cpp

// ==========================
// Sessions API (list + revoke)
// ==========================
#if ENABLE_HTTP_SERVER
static void buildUserSessionsJson(const String& user, const String& currentSid, JsonArray& sessions) {
  // Build JSON array directly (no String allocation)
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    const SessionEntry& s = gSessions[i];
    if (!s.sid.length()) continue;
    if (s.user != user) continue;
    
    JsonObject session = sessions.add<JsonObject>();
    session["sid"] = s.sid;
    session["createdAt"] = s.createdAt;
    session["lastSeen"] = s.lastSeen;
    session["expiresAt"] = s.expiresAt;
    session["ip"] = s.ip.length() ? s.ip : "-";
    session["current"] = (s.sid == currentSid);
  }
}
#endif // ENABLE_HTTP_SERVER

static const char* originFrom(const AuthContext& ctx) {
  // Only map known transports to stable strings; avoid assuming future ones exist
  switch (ctx.transport) {
    case SOURCE_WEB: return "web";
    case SOURCE_SERIAL: return "serial";
    case SOURCE_ESPNOW: return "espnow";
    case SOURCE_INTERNAL: return "internal";
    case SOURCE_MQTT: return "mqtt";
    case SOURCE_VOICE: return "voice";
    default: return "unknown";
  }
}

extern bool isAdminUser(const String& who);

bool hasAdminPrivilege(const AuthContext& ctx) {
  // SOURCE_INTERNAL transport grants automatic admin privileges for system-level operations
  // (e.g., scheduled automations, system boot commands)
  // User-originated commands (Web, Serial, ESP-NOW) must check actual user admin status
  if (ctx.transport == SOURCE_INTERNAL) return true;
  return isAdminUser(ctx.user);
}

enum CommandOrigin { ORIGIN_SERIAL,
                     ORIGIN_WEB,
                     ORIGIN_AUTOMATION,
                     ORIGIN_SYSTEM };
// Note: avoid name collision with existing OUTPUT_* macros used for device output flags
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
  void* replyHandle;     // placeholder for future sync replies
  httpd_req_t* httpReq;  // used by web origin if needed
};
struct Command {
  String line;
  CommandContext ctx;
};

void setCurrentCommandContext(const CommandContext& ctx) {
  gExecUser = ctx.auth.user;
  gExecAuthContext = ctx.auth;
}

// -------- Command Executor Task (definition) --------
struct ExecReq {
  char line[2048];         // Command string (full size for ESP-NOW chunking)
  CommandContext ctx;      // Full execution context
  char out[2048];          // Result buffer (2KB)
  SemaphoreHandle_t done;  // Signals completion
  bool ok;                 // Success flag from executeCommand()
};

// Now that ExecReq is fully defined we can implement the task
static void commandExecTask(void* pv) {
  DEBUG_CMD_FLOWF("[cmd_exec] task started");
  static unsigned long lastStackCheck = 0;
  for (;;) {
    // Periodic stack watermark check (every 30 seconds)
    unsigned long now = millis();
    if (now - lastStackCheck > 30000) {
      // Get current stack pointer position
      void* sp;
      asm volatile("mov %0, sp"
                   : "=r"(sp));
      UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(NULL);
      uint32_t stackPeak = (4096 * 4) - (stackHighWater * 4);  // Peak usage in bytes
      int peakPct = (stackPeak * 100) / (4096 * 4);

      // Estimate current usage (approximate, stack grows down)
      void* stackBase = (void*)((uint32_t)sp + (stackHighWater * 4));
      uint32_t stackCurrent = (uint32_t)stackBase - (uint32_t)sp;
      int currentPct = (stackCurrent * 100) / (4096 * 4);

      DEBUG_MEMORYF("[STACK] cmd_exec: current=%lu bytes (%d%%), peak=%lu bytes (%d%%), free_min=%lu bytes",
                    (unsigned long)stackCurrent, currentPct,
                    (unsigned long)stackPeak, peakPct,
                    (unsigned long)(stackHighWater * 4));
      DEBUG_MEMORYF("[HEAP] cmd_exec: free=%lu min=%lu",
                    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
      lastStackCheck = now;
    }

    ExecReq* r = nullptr;
    DEBUG_CMD_FLOWF("[cmd_exec] waiting for command... (queue=%p heap=%lu)", gCmdExecQ, (unsigned long)ESP.getFreeHeap());
    
    BaseType_t receiveResult = xQueueReceive(gCmdExecQ, &r, portMAX_DELAY);
    DEBUG_CMD_FLOWF("[cmd_exec] xQueueReceive returned: result=%d r=%p", receiveResult, r);
    
    if (receiveResult == pdTRUE) {
      if (!r) {
        DEBUG_CMD_FLOWF("[cmd_exec] ERROR: Received NULL pointer from queue!");
        continue;
      }
      DEBUG_CMD_FLOWF("[cmd_exec] Received request at %p (PSRAM-allocated)", r);
      DEBUG_CMD_FLOWF("[cmd_exec] r->line='%.200s' len=%zu", r->line, strlen(r->line));
      DEBUG_CMD_FLOWF("[cmd_exec] r->ctx.origin=%d r->ctx.validateOnly=%d", (int)r->ctx.origin, r->ctx.validateOnly ? 1 : 0);
      DEBUG_CMD_FLOWF("[cmd_exec] r->ctx.auth.user='%s' path='%s'", r->ctx.auth.user.c_str(), r->ctx.auth.path.c_str());
      DEBUG_CMD_FLOWF("[cmd_exec] r->done=%p heap=%lu psram=%lu", r->done,
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
      
      DEBUG_CMD_FLOWF("[cmd_exec] Setting command context");
      setCurrentCommandContext(r->ctx);
      DEBUG_CMD_FLOWF("[cmd_exec] Executing command: '%.200s'", r->line);
      bool prevValidate = gCLIValidateOnly;
      gCLIValidateOnly = r->ctx.validateOnly;
      r->ok = executeCommand((AuthContext&)r->ctx.auth, r->line, r->out, sizeof(r->out));
      gCLIValidateOnly = prevValidate;
      DEBUG_CMD_FLOWF("[cmd_exec] Command executed: ok=%d out_len=%zu heap=%lu",
                  r->ok ? 1 : 0, strlen(r->out), (unsigned long)ESP.getFreeHeap());
      DEBUG_CMD_FLOWF("[cmd_exec] Giving semaphore: r->done=%p", r->done);
      
      xSemaphoreGive(r->done);
      DEBUG_CMD_FLOWF("[cmd_exec] Semaphore given, command complete");
    } else {
      DEBUG_CMD_FLOWF("[cmd_exec] xQueueReceive failed: result=%d", receiveResult);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// Context-aware broadcastOutput that includes origin/user/path metadata
void broadcastOutput(const String& s, const CommandContext& ctx) {
  const char* source;
  switch (ctx.origin) {
    case ORIGIN_SERIAL: source = "serial"; break;
    case ORIGIN_WEB: source = "web"; break;
    case ORIGIN_AUTOMATION: source = "auto"; break;
    case ORIGIN_SYSTEM:
    default: source = "system"; break;
  }

  String prefixed = originPrefix(source, ctx.auth.user, ctx.auth.ip) + s;
  DEBUG_CMD_FLOWF("[BROADCAST_CTX_DEBUG] origin=%s user=%s mask=0x%02lX flags=0x%02lX msg='%.50s'",
                  source, ctx.auth.user.c_str(),
                  (unsigned long)ctx.outputMask,
                  (unsigned long)gOutputFlags,
                  s.substring(0, 50).c_str());

  // Centralized sinks: route via debug_system (respects help gating and flags)
  broadcastOutput(prefixed);
  // Preserve prior behavior: ensure web history is appended even if OUTPUT_WEB is disabled
  if (!(gOutputFlags & OUTPUT_WEB)) {
    printToWeb(prefixed);
  }

  // ESP-NOW streaming: Send to remote device if active (uses prefixed version for context)
#if ENABLE_ESPNOW
  if (gEspNow && gEspNow->streamActive && gEspNow->streamTarget) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] broadcastOutput(ctx) calling sendEspNowStreamMessage | msg: %.50s", prefixed.c_str());
    sendEspNowStreamMessage(prefixed);
  } else if (gEspNow && (gEspNow->streamActive || gEspNow->streamTarget)) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] broadcastOutput(ctx) NOT streaming - active=%d target=%s | msg: %.50s",
           gEspNow->streamActive, gEspNow->streamTarget ? "SET" : "NULL", prefixed.c_str());
  }
#endif

  DEBUG_CMD_FLOWF("[broadcast] sinks: serial=%d web=%d log=%d len=%d",
                  (ctx.outputMask & CMD_OUT_SERIAL) ? 1 : 0,
                  (ctx.outputMask & CMD_OUT_WEB) ? 1 : 0,
                  (ctx.outputMask & CMD_OUT_LOG) ? 1 : 0,
                  s.length());
}

char* gFileReadBuf = nullptr;
char* gFileOutBuf = nullptr;
size_t kFileReadBufSize = 2048;
size_t kFileOutBufSize = 2048;

bool ensureFileViewBuffers() {
  if (!gFileReadBuf) {
    gFileReadBuf = (char*)ps_alloc(kFileReadBufSize, AllocPref::PreferPSRAM, "http.file.read");
  }
  if (!gFileOutBuf) {
    gFileOutBuf = (char*)ps_alloc(kFileOutBufSize, AllocPref::PreferPSRAM, "http.file.out");
  }
  return gFileReadBuf && gFileOutBuf;
}

static void performanceCounter() {
  static unsigned long perfCounter = 0;
  static unsigned long lastPerfReport = 0;
  perfCounter++;

  // Report performance every 5 seconds
  if (millis() - lastPerfReport > 5000) {
    unsigned long loopsPerSec = perfCounter / 5;
    DEBUG_PERFORMANCEF("Performance: %lu loops/sec", loopsPerSec);
    perfCounter = 0;
    lastPerfReport = millis();
  }
}

static String exitHelpAndExecute(const String& originalCmd) {
  String banner = exitToNormalBanner() + "\n";
  AuthContext ctx = gExecAuthContext;
  ctx.path = "/help/exit";
  char out[2048];
  (void)executeCommand(ctx, originalCmd.c_str(), out, sizeof(out));
  return banner + String(out);
}

extern int connectedDeviceCount;
extern struct ConnectedDevice connectedDevices[];

static bool gDebugMemSummary = false;

static void heapLogSummary(const char* tag) {
  size_t dram_free = ESP.getFreeHeap();
  size_t dram_min = ESP.getMinFreeHeap();
  size_t dram_maxalloc = ESP.getMaxAllocHeap();
  size_t dram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  bool has_ps = psramFound();
  size_t ps_total = has_ps ? ESP.getPsramSize() : 0;
  size_t ps_free = has_ps ? ESP.getFreePsram() : 0;
  size_t ps_largest = has_ps ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) : 0;
  UBaseType_t main_hwm = uxTaskGetStackHighWaterMark(NULL);
  BROADCAST_PRINTF("[HEAP] %s | dram_free=%u dram_largest=%u dram_maxalloc=%u dram_min=%u | psram=%s total=%u free=%u largest=%u | stack_main=%u",
                   tag ? tag : "?",
                   (unsigned)dram_free,
                   (unsigned)dram_largest,
                   (unsigned)dram_maxalloc,
                   (unsigned)dram_min,
                   has_ps ? "yes" : "no",
                   (unsigned)ps_total,
                   (unsigned)ps_free,
                   (unsigned)ps_largest,
                   (unsigned)main_hwm);
}

extern void ensureDeviceRegistryFile();
extern void discoverI2CDevices();

#if ENABLE_AUTOMATION
const char* cmd_downloadautomation(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  broadcastOutput("Download automation from GitHub not yet implemented");
  return "ERROR";
}
const char* cmd_conditional(const String& cmd) {
  return executeConditionalCommand(cmd.c_str());
}
#endif

void hardwareone_setup() {
  // --- Initialise Serial early ---
  Serial.begin(115200);
  delay(500);  // Longer delay for serial connection

  // Enable allocation tracking BEFORE any allocations
  gAllocTrackerEnabled = true;
  gAllocTrackerCount = 0;
  memset(gAllocTracker, 0, sizeof(gAllocTracker));

  // Filesystem FIRST to enable early allocation logging
  if (!initFilesystem()) {
    Serial.println("FATAL: Filesystem initialization failed");
    while (1) delay(1000);
  }
#if DEBUG_MEM_SUMMARY
  heapLogSummary("boot.after_fs");
#endif

  // NEW: Detect first-time setup state IMMEDIATELY after filesystem init
  // This ensures OLED shows correct message from the first frame
  detectFirstTimeSetupState();

  // Allocate WiFi networks array BEFORE loading settings (needed for readSettingsJson)
#if ENABLE_WIFI
  if (!gWifiNetworks) {
    gWifiNetworks = (WifiNetwork*)ps_alloc(MAX_WIFI_NETWORKS * sizeof(WifiNetwork), AllocPref::PreferPSRAM, "wifi.networks");
    if (!gWifiNetworks) {
      Serial.println("FATAL: Failed to allocate WiFi networks array");
      while (1) delay(1000);
    }
    // Initialize with placement new to call constructors
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
      new (&gWifiNetworks[i]) WifiNetwork();
    }
    Serial.println("[DEBUG] WiFi networks array allocated");
  }
#endif

  // Load settings EARLY (before allocations) so conditional allocations can use setting values
  Serial.println("[DEBUG] About to call settingsDefaults()");
  Serial.flush();

  // Initialize settings with defaults FIRST to ensure String members are constructed
  settingsDefaults();

  Serial.println("[DEBUG] settingsDefaults() completed");
  Serial.flush();

  // Note: All settings modules now auto-register via static constructors
  // No manual registration needed

  // Load settings from file if it exists (will overwrite defaults)
  bool haveSettings = false;
  if (filesystemReady) {
    FsLockGuard guard("settings.exists");
    haveSettings = LittleFS.exists(SETTINGS_JSON_FILE);
  }

  if (filesystemReady && haveSettings) {
    Serial.println("[DEBUG] Settings file exists, about to read");
    Serial.flush();

    bool settingsLoaded = readSettingsJson();
    if (settingsLoaded) {
      Serial.println("[DEBUG] Settings loaded successfully");
      Serial.flush();
      // NOTE: applySettings() moved to after initDebugSystem() so debug queue exists
      
      // Initialize modular command system early
      Serial.println("[DEBUG] About to initialize command system");
      Serial.flush();
      initializeCommandSystem();
      
      // Print debug summary of auto-registered command modules
      printCommandModuleSummary();
      Serial.println("[DEBUG] Command system initialization completed");
      Serial.flush();
    }
  } else {
    if (!filesystemReady) {
      Serial.println("[DEBUG] Filesystem not ready - using defaults");
    } else {
      Serial.println("[DEBUG] No settings file, writing defaults");
    }
    Serial.flush();
    // No settings file exists or filesystem not ready, write defaults if possible
    if (filesystemReady) {
      writeSettingsJson();
      // NOTE: applySettings() moved to after initDebugSystem() so debug queue exists
      
      // Initialize modular command system early
      Serial.println("[DEBUG] About to initialize command system (default settings)");
      Serial.flush();
      initializeCommandSystem();
      
      // Print debug summary of auto-registered command modules
      printCommandModuleSummary();
      Serial.println("[DEBUG] Command system initialization completed (default settings)");
      Serial.flush();
    }
  }

  // FALLBACK: Ensure command system is always initialized
  // This handles the case where filesystem failed and no settings were applied
  if (!gCommands || gCommandsCount == 0) {
    Serial.println("[DEBUG] Initializing command system (fallback - no settings loaded)");
    Serial.flush();
    initializeCommandSystem();
    
    // Print debug summary of auto-registered command modules
    printCommandModuleSummary();
    Serial.println("[DEBUG] Command system initialization completed (fallback)");
    Serial.flush();
  }

  // If time is already valid (warm boot, retained RTC), resolve user creation times early
  if (time(nullptr) > 0) {
    resolvePendingUserCreationTimes();
  }

  // Generate unique boot ID for session versioning
  Serial.println("[DEBUG] About to generate boot ID");
  Serial.flush();

  uint64_t chipId = ESP.getEfuseMac();
  Serial.printf("[DEBUG] Got chip ID: %llx\n", chipId);
  Serial.flush();

  Serial.println("[DEBUG] Creating boot ID string parts");
  Serial.flush();

  String part1 = String((uint32_t)(chipId >> 32), HEX);
  Serial.printf("[DEBUG] part1 created, len=%d\n", part1.length());
  Serial.flush();

  String part2 = String((uint32_t)chipId, HEX);
  Serial.printf("[DEBUG] part2 created, len=%d\n", part2.length());
  Serial.flush();

  String part3 = String(millis());
  Serial.printf("[DEBUG] part3 created, len=%d\n", part3.length());
  Serial.flush();

  Serial.println("[DEBUG] Concatenating boot ID parts");
  Serial.flush();

  gBootId = part1 + part2 + "_" + part3;

  Serial.printf("[DEBUG] Boot ID created: len=%d\n", gBootId.length());
  Serial.flush();

  // Debug: Log the boot ID generation
  DEBUG_SYSTEMF("Generated new boot ID: %s", gBootId.c_str());
  Serial.flush();  // Ensure debug message is sent immediately

  // Build identifier banner
  broadcastOutput("[build] Firmware: reg-json-debug-1");
  DEBUG_SYSTEMF("Setup continuing after banner");

  // ========================================
  // CRITICAL: Create ALL mutexes and semaphores FIRST before any tasks or I2C operations
  // ========================================

  // Initialize sensor cache mutexes conditionally (only for enabled sensors)
#if ENABLE_THERMAL_SENSOR
  if (!gThermalCache.mutex) {
    gThermalCache.mutex = xSemaphoreCreateMutex();
    if (!gThermalCache.mutex) {
      Serial.println("FATAL: Failed to create thermal cache mutex");
      while (1) delay(1000);
    }
  }
#endif

#if ENABLE_IMU_SENSOR
  if (!gImuCache.mutex) {
    gImuCache.mutex = xSemaphoreCreateMutex();
    if (!gImuCache.mutex) {
      Serial.println("FATAL: Failed to create IMU cache mutex");
      while (1) delay(1000);
    }
  }
#endif

#if ENABLE_TOF_SENSOR
  if (!gTofCache.mutex) {
    gTofCache.mutex = xSemaphoreCreateMutex();
    if (!gTofCache.mutex) {
      Serial.println("FATAL: Failed to create ToF cache mutex");
      while (1) delay(1000);
    }
  }
#endif

#if ENABLE_GAMEPAD_SENSOR
  Serial.println("[GAMEPAD_INIT] Creating gControlCache.mutex...");
  if (!gControlCache.mutex) {
    gControlCache.mutex = xSemaphoreCreateMutex();
    if (!gControlCache.mutex) {
      Serial.println("FATAL: Failed to create gamepad cache mutex");
      while (1) delay(1000);
    }
    Serial.printf("[GAMEPAD_INIT] gControlCache.mutex created: %p\n", (void*)gControlCache.mutex);
  } else {
    Serial.printf("[GAMEPAD_INIT] gControlCache.mutex already exists: %p\n", (void*)gControlCache.mutex);
  }
#endif

  // Legacy cache removed - each sensor now manages its own cache mutex

  Serial.println("[CACHE_INIT] Sensor cache mutexes created (conditional compilation)");

  // Initialize all global mutexes (fsMutex, i2cMutex, gJsonResponseMutex, gMeshRetryMutex)
  // Centralized in mutex_system.cpp
  initMutexes();

  // Initialize sensor startup queue mutex (in i2c_system.cpp) - only if runtime enabled
  if (gSettings.i2cSensorsEnabled) {
    initSensorQueue();
    Serial.println("[I2C_SENSORS] Runtime enabled - sensor queue initialized");
  } else {
    Serial.println("[I2C_SENSORS] Runtime disabled - skipping sensor queue initialization");
  }

  Serial.println("[DEBUG] All mutexes created successfully");
  Serial.flush();

  // ========================================
  // Allocate buffers and resources (no tasks yet)
  // ========================================

  // (Removed) gStreamBuffer allocation - was never used, saving 4.5KB

  // Initialize debug system (buffer + ring buffer)
  initDebugSystem();

  // CRITICAL: Apply debug flags AFTER debug system is initialized
  // This ensures the debug queue/task exists when flags are set
  Serial.println("[DEBUG] Applying settings (debug flags, output routing, etc.)");
  Serial.flush();
  applySettings();
  Serial.println("[DEBUG] Settings applied - debug flags now active");
  Serial.flush();

  // Memory baseline right after debug buffer is ready
  heapLogSummary("boot.after_debugbuf");

  // Initialize shared JSON response buffer for handlers
#if ENABLE_HTTP_SERVER
  if (!gJsonResponseBuffer) {
    gJsonResponseBuffer = (char*)ps_alloc(JSON_RESPONSE_SIZE, AllocPref::PreferPSRAM, "json.resp.buf");
    if (!gJsonResponseBuffer) {
      Serial.println("FATAL: Failed to allocate JSON response buffer");
      while (1) delay(1000);
    }
  }
#endif

#if ENABLE_AUTOMATION
  // Initialize automation system at boot (only if enabled in settings)
  if (gSettings.automationsEnabled) {
    if (!initAutomationSystem()) {
      Serial.println("FATAL: Failed to initialize automation system");
      while (1) delay(1000);
    }
    DEBUG_SYSTEMF("Automation system initialized at boot");
  } else {
    DEBUG_SYSTEMF("Automation system disabled - skipping initialization");
  }
#endif

  // ========================================
  // Now safe to create command executor queue and task
  // ========================================
  if (!gCmdExecQ) {
    gCmdExecQ = xQueueCreate(6, sizeof(ExecReq*));
    if (!gCmdExecQ) {
      Serial.println("FATAL: Failed to create command exec queue");
      while (1) delay(1000);
    }
    const uint32_t cmdExecStackWords = 5120;  // words (≈20 KB) - includes NTP sync with DNS/file I/O
    if (xTaskCreateLogged(commandExecTask, "cmd_exec", cmdExecStackWords, nullptr, 1, nullptr, "cmd.exec") != pdPASS) {
      Serial.println("FATAL: Failed to create command exec task");
      while (1) delay(1000);
    }
    Serial.println("[DEBUG] Command executor task created");
#if DEBUG_MEM_SUMMARY
    heapLogSummary("boot.after_task.cmd_exec");
#endif
  }

  // NTP sync runs synchronously in cmd_exec task (no dedicated NTP task needed)

  // Initialize battery monitoring (Feather ESP32 battery on A13/GPIO35)
  initBattery();
  
  // Initialize NeoPixel LED (also enables NEOPIXEL_I2C_POWER on Feather V2)
  // CRITICAL: Must be called BEFORE initI2CBuses() to power STEMMA QT connector
  initNeoPixelLED();
  
  // Initialize I2C buses early for OLED boot animation
  // Centralized initialization in i2c_system.cpp
 #if ENABLE_I2C_SYSTEM
  initI2CBuses();
  
  // Suppress ESP-IDF I2C driver NACK spam (legitimately occurs during FM radio RDS polling)
  // The RDA5807M FM radio chip returns NACK when polled for RDS data that isn't ready yet
  // This is intentional protocol behavior, not an error - suppress routine I2C logs
  esp_log_level_set("i2c.master", ESP_LOG_WARN);  // Only show WARN and ERROR, suppress INFO/DEBUG
  DEBUG_SENSORSF("[I2C] ESP-IDF I2C driver log level set to WARN (suppresses routine NACK messages)");
 #endif

  // Show modular sensor configuration (always visible during boot)
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║          MODULAR SENSOR BUILD CONFIGURATION                  ║");
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
#if ENABLE_THERMAL_SENSOR
  Serial.println("║ ✓ THERMAL  │ MLX90640 thermal camera                         ║");
  Serial.println("║            │ Task: thermalTask() in Sensor_Thermal_MLX90640  ║");
#else
  Serial.println("║ ✗ THERMAL  │ Disabled (~20-25KB flash, ~15KB RAM saved)      ║");
#endif
#if ENABLE_TOF_SENSOR
  Serial.println("║ ✓ TOF      │ VL53L4CX distance sensor                        ║");
  Serial.println("║            │ Task: tofTask() in Sensor_ToF_VL53L4CX          ║");
#else
  Serial.println("║ ✗ TOF      │ Disabled (~25-30KB flash, ~10KB RAM saved)      ║");
#endif
#if ENABLE_IMU_SENSOR
  Serial.println("║ ✓ IMU      │ BNO055 9-DOF orientation sensor                 ║");
  Serial.println("║            │ Task: imuTask() in Sensor_IMU_BNO055            ║");
#else
  Serial.println("║ ✗ IMU      │ Disabled (~12-18KB flash, ~8KB RAM saved)       ║");
#endif
#if ENABLE_GAMEPAD_SENSOR
  Serial.println("║ ✓ GAMEPAD  │ Seesaw gamepad controller                       ║");
  Serial.println("║            │ Task: gamepadTask() in Sensor_Gamepad_Seesaw    ║");
#else
  Serial.println("║ ✗ GAMEPAD  │ Disabled (~8-12KB flash, ~6KB RAM saved)        ║");
#endif
#if ENABLE_APDS_SENSOR
  Serial.println("║ ✓ APDS     │ APDS9960 color/proximity/gesture                ║");
  Serial.println("║            │ Task: apdsTask() in Sensor_APDS_APDS9960        ║");
#else
  Serial.println("║ ✗ APDS     │ Disabled (~6-10KB flash, ~4KB RAM saved)        ║");
#endif
#if ENABLE_GPS_SENSOR
  Serial.println("║ ✓ GPS      │ PA1010D mini GPS module                         ║");
  Serial.println("║            │ Task: gpsTask() in Sensor_GPS_PA1010D           ║");
#else
  Serial.println("║ ✗ GPS      │ Disabled (~5-8KB flash, ~4KB RAM saved)         ║");
#endif
#if ENABLE_PRESENCE_SENSOR
  Serial.println("║ ✓ PRESENCE │ STHS34PF80 IR presence/motion sensor            ║");
  Serial.println("║            │ Task: presenceTask() in i2csensor-sths34pf80    ║");
#else
  Serial.println("║ ✗ PRESENCE │ Disabled (~4-6KB flash, ~2KB RAM saved)         ║");
#endif
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
#if ENABLE_OLED_DISPLAY
  Serial.println("║ ✓ OLED     │ SSD1306 128x64 display enabled                  ║");
#else
  Serial.println("║ ✗ OLED     │ Disabled (~8-12KB flash, ~5KB RAM saved)        ║");
#endif
  Serial.println("╚══════════════════════════════════════════════════════════════╝");
  Serial.println();

  // Servo profiles initialization moved to i2csensor-pca9685.cpp (initialized when first servo command is used)

  // Quick OLED detection and initialization for boot animation
  // This happens BEFORE WiFi/NTP so animation shows during slow setup operations
  oledEarlyInit();

  // Mutexes already created earlier in setup() - safe to create tasks now

  // Create sensor queue processor task only if I2C sensors are runtime enabled
 #if ENABLE_I2C_SYSTEM
  if (gSettings.i2cSensorsEnabled && !queueProcessorTask) {
    const uint32_t queueStackWords = 3072;  // ~12KB (measured min free during IMU start: ~1408 bytes)
    if (xTaskCreateLogged(sensorQueueProcessorTask, "sensor_queue", queueStackWords, nullptr, 1, &queueProcessorTask, "sensor.queue") != pdPASS) {
      Serial.println("FATAL: Failed to create sensor queue processor task");
      while (1) delay(1000);
    }
    DEBUG_SYSTEMF("Sensor queue processor task created successfully");
    Serial.println("[I2C_SENSORS] Queue processor task created (runtime enabled)");
 #if DEBUG_MEM_SUMMARY
    heapLogSummary("boot.after_task.sensor_queue");
 #endif
  } else if (!gSettings.i2cSensorsEnabled) {
    Serial.println("[I2C_SENSORS] Queue processor task skipped (runtime disabled - saves ~12KB RAM)");
  }
 #endif

  // Per-sensor tasks will be created lazily on first start to conserve RAM

  // Initialize I2C clock stack only if I2C sensors are runtime enabled  
 #if ENABLE_I2C_SYSTEM
  if (gSettings.i2cSensorsEnabled && !gI2CClockStack) {
    gI2CClockStack = (uint32_t*)ps_alloc(kI2CClockStackMax * sizeof(uint32_t), AllocPref::PreferPSRAM, "i2c.stack");
    if (!gI2CClockStack) {
      Serial.println("FATAL: Failed to allocate I2C clock stack");
      while (1) delay(1000);
    }
    Serial.println("[I2C_SENSORS] Clock stack allocated (runtime enabled)");
  } else if (!gSettings.i2cSensorsEnabled) {
    Serial.println("[I2C_SENSORS] Clock stack skipped (runtime disabled - saves I2C memory)");
  } 
  if (gI2CClockStack) {
    memset(gI2CClockStack, 0, kI2CClockStackMax * sizeof(uint32_t));
  }
 #endif

  // WiFi networks array already allocated early (before settings load)
  // See allocation near top of setup() before readSettingsJson()

  // Initialize session entries array
#if ENABLE_HTTP_SERVER
  if (!gSessions) {
    gSessions = (SessionEntry*)ps_alloc(MAX_SESSIONS * sizeof(SessionEntry), AllocPref::PreferPSRAM, "sessions");
    if (!gSessions) {
      Serial.println("FATAL: Failed to allocate sessions array");
      while (1) delay(1000);
    }
    // Initialize with placement new to call constructors
    for (int i = 0; i < MAX_SESSIONS; i++) {
      new (&gSessions[i]) SessionEntry();
    }
  }

  // Initialize logout reasons array
  if (!gLogoutReasons) {
    gLogoutReasons = (LogoutReason*)ps_alloc(MAX_LOGOUT_REASONS * sizeof(LogoutReason), AllocPref::PreferPSRAM, "logout.reasons");
    if (!gLogoutReasons) {
      Serial.println("FATAL: Failed to allocate logout reasons array");
      while (1) delay(1000);
    }
    // Initialize with placement new to call constructors
    for (int i = 0; i < MAX_LOGOUT_REASONS; i++) {
      new (&gLogoutReasons[i]) LogoutReason();
    }
  }
#endif



  // Now safe to emit output (may allocate and will be logged)
  broadcastOutput("");
  broadcastOutput("Booting ESP32 Minimal Auth");

  // Settings already loaded early (before allocations) for conditional resource allocation

#if ENABLE_WIFI
  // WiFi initialization deferred to first use (lazy init saves ~32KB at boot)
  // WiFi will be initialized when user calls wificonnect or enables via quick settings
  DEBUG_WIFIF("[Boot] WiFi initialization deferred (lazy init)");
#endif

  // First-time setup if needed (prompts on Serial, adds WiFi credentials)
  if (gFirstTimeSetupState == SETUP_NOT_NEEDED) {
    oledSetBootProgress(10, "Setup check...");
  } else {
    oledUpdate();  // Force OLED to show first-time setup prompt before blocking
    
#if ENABLE_OLED_DISPLAY && ENABLE_GAMEPAD_SENSOR
    // Start gamepad sensor before first-time setup so OLED keyboard can receive input
    if (oledConnected && oledEnabled) {
      DEBUG_SYSTEMF("[Boot] Starting gamepad sensor for OLED first-time setup");
      const char* result = startGamepadInternal();  // Properly initializes hardware and creates task
      Serial.printf("[Boot] Gamepad init result: %s\n", result);
      delay(100);  // Give gamepad task time to start polling
    }
#endif
  }
  firstTimeSetupIfNeeded();
  oledUpdate();  // Update OLED animation during boot

  // Load user credentials
  String fu, fp;
  if (loadUsersFromFile(fu, fp)) {
    gAuthUser = fu;
    gAuthPass = fp;
  }
  rebuildExpectedAuthHeader();

  // RTC early boot sync - only if RTC time has been previously set
  // If rtcTimeHasBeenSet is false, we'll prioritize NTP at boot to get accurate time first
#if ENABLE_RTC_SENSOR
  if (gSettings.rtcTimeHasBeenSet) {
    oledSetBootProgress(28, "RTC sync...");
    if (rtcEarlyBootSync()) {
      broadcastOutput("[Boot] System time set from RTC (previously calibrated)");
    }
  } else {
    oledSetBootProgress(28, "RTC uncalibrated");
    broadcastOutput("[Boot] RTC time not yet set - will sync from NTP if available");
  }
#endif

  // Network - WiFi auto-start enabled by default
#if ENABLE_WIFI
  oledSetBootProgress(30, "WiFi ready...");

  bool wifiConnected = false;
  // Always attempt WiFi connection if credentials exist (controlled by wifiAutoReconnect setting)
  if (gSettings.wifiAutoReconnect) {  // Controlled by first-time setup or settings
    // Skip NTP sync in wificonnect so we can show it separately in boot progress
    gSkipNTPInWifiConnect = true;
    setupWiFi();
    gSkipNTPInWifiConnect = false;  // Reset for future manual connections
    wifiConnected = WiFi.isConnected();
#if DEBUG_MEM_SUMMARY
    if (wifiConnected) { heapLogSummary("boot.after_wifi"); }
#endif
  } else {
    // WiFi initialization deferred - will initialize on first use
    broadcastOutput("WiFi disabled by default. Use quick settings (SELECT button) or 'wificonnect' to connect.");
  }

  // Update OLED animation after WiFi attempt
  oledSetBootProgress(40, wifiConnected ? "WiFi connected" : "WiFi skipped");

  // NTP sync phase - runs synchronously during boot
  if (wifiConnected) {
    oledSetBootProgress(45, "Syncing time...");
    Serial.println("[DEBUG] Starting NTP sync");
    Serial.flush();
    bool ntpOk = syncNTPAndResolve();
    Serial.println(ntpOk ? "[DEBUG] NTP sync complete" : "[DEBUG] NTP sync failed");
    Serial.flush();
    oledSetBootProgress(50, ntpOk ? "Time synced" : "Time sync failed");
  } else {
    oledSetBootProgress(50, "Network offline");
  }
#else
  // WiFi disabled at compile time
  oledSetBootProgress(30, "WiFi disabled");
  bool wifiConnected = false;
  oledSetBootProgress(50, "Network offline");
#endif

  Serial.println("[DEBUG] About to start device discovery");
  Serial.flush();

  // Initialize device registry (after I2C system is ready)
  oledSetBootProgress(60, "Scanning devices...");

  DEBUG_SYSTEMF("Starting device discovery");
  ensureDeviceRegistryFile();
  
  // Give slower I2C devices (GPS, FM Radio, Gamepad) extra time to initialize after power-on
  // Some sensors need 1-2 seconds to become responsive on I2C bus
  delay(2000);
  
 #if ENABLE_I2C_SYSTEM
  discoverI2CDevices();
  DEBUG_SYSTEMF("Device discovery completed");
 #else
  DEBUG_SYSTEMF("I2C system disabled at compile time - skipping I2C device discovery");
 #endif

  oledSetBootProgress(80, "Devices found");

  // Apply OLED settings if display was initialized early
  oledApplySettings();

  // Gamepad auto-initialization removed: use opengamepad (queued) to start

  // Bluetooth - auto-start if enabled in settings
#if ENABLE_BLUETOOTH
  if (gSettings.bluetoothAutoStart) {
    oledSetBootProgress(85, "BLE init...");
    
    // Pause sensor polling during BLE init to avoid interrupt contention
    bool wasPaused = gSensorPollingPaused;
    gSensorPollingPaused = true;
    vTaskDelay(pdMS_TO_TICKS(50));  // Let pending I2C ops complete
    
    extern bool initBluetooth();
    extern bool startBLEAdvertising();
    if (initBluetooth()) {
      if (startBLEAdvertising()) {
        broadcastOutput("Bluetooth initialized and advertising");
      } else {
        broadcastOutput("Bluetooth initialized but advertising failed");
      }
    } else {
      broadcastOutput("Bluetooth initialization failed");
    }
    
    gSensorPollingPaused = wasPaused;
  } else {
    broadcastOutput("Bluetooth disabled by default. Use quick settings (SELECT button) or 'openble' to enable.");
  }
#endif

  // Sensor auto-start - process settings for all I2C sensors
  oledSetBootProgress(87, "Sensors...");
  processAutoStartSensors();

#if ENABLE_CAMERA_SENSOR
  // Camera auto-start (independent of I2C sensor queue)
  if (gSettings.cameraAutoStart) {
    runUnifiedSystemCommand("opencamera");
  }
#endif

#if ENABLE_MICROPHONE_SENSOR
  // Microphone / ESP-SR auto-start
  // If ESP-SR is enabled, it takes over the microphone - don't start mic separately
  #if ENABLE_ESP_SR
  if (gSettings.srAutoStart) {
    broadcastOutput("Starting ESP-SR speech recognition...");
    runUnifiedSystemCommand("sr start");
  } else if (gSettings.microphoneAutoStart) {
    broadcastOutput("Starting microphone sensor...");
    runUnifiedSystemCommand("openmic");
  }
  #else
  if (gSettings.microphoneAutoStart) {
    broadcastOutput("Starting microphone sensor...");
    runUnifiedSystemCommand("openmic");
  }
  #endif
#endif

  // HTTP server - auto-start if enabled in settings and WiFi is connected
#if ENABLE_HTTP_SERVER
  oledSetBootProgress(90, "HTTP ready...");

  if (gSettings.httpAutoStart && WiFi.isConnected()) {
    runUnifiedSystemCommand("openhttp");
    broadcastOutput("HTTP server started. Try: http://" + WiFi.localIP().toString());
  } else if (!gSettings.httpAutoStart) {
    broadcastOutput("HTTP server available. Use 'openhttp' or quick settings (SELECT button) to start.");
  } else {
    broadcastOutput("HTTP server not started (WiFi offline). Use quick settings (SELECT button) or 'openhttp' to start manually.");
  }
#else
  oledSetBootProgress(90, "HTTP disabled");
#endif

  // MQTT client - auto-start if enabled in settings and WiFi is connected
#if ENABLE_MQTT
  oledSetBootProgress(92, "MQTT ready...");

  if (gSettings.mqttAutoStart && WiFi.isConnected()) {
    runUnifiedSystemCommand("openmqtt");
    broadcastOutput("[MQTT] Auto-start enabled, connecting to broker...");
  } else if (!gSettings.mqttAutoStart) {
    broadcastOutput("[MQTT] Available. Use 'openmqtt' to connect.");
  } else {
    broadcastOutput("[MQTT] Not started (WiFi offline). Use 'openmqtt' to start manually.");
  }
#endif

  oledSetBootProgress(100, "Boot complete!");

  // Run LED startup effect if enabled (only on boards with NeoPixel hardware)
#if defined(NEOPIXEL_PIN_DEFAULT) && NEOPIXEL_PIN_DEFAULT >= 0
  if (gSettings.ledStartupEnabled && gSettings.ledStartupEffect != "none") {
    RGB color1, color2;
    if (!getRGBFromName(gSettings.ledStartupColor, color1)) {
      color1 = { 0, 255, 255 };  // Default cyan
    }
    if (!getRGBFromName(gSettings.ledStartupColor2, color2)) {
      color2 = { 255, 0, 255 };  // Default magenta
    }

    unsigned long duration = gSettings.ledStartupDuration;
    if (duration < 100) duration = 100;
    if (duration > 10000) duration = 10000;

    String effect = gSettings.ledStartupEffect;
    effect.toLowerCase();

    if (effect == "rainbow") {
      runLEDEffect(EFFECT_RAINBOW, color1, color1, duration);
    } else if (effect == "pulse" || effect == "breathe") {
      runLEDEffect(EFFECT_PULSE, color1, color1, duration);
    } else if (effect == "fade") {
      runLEDEffect(EFFECT_FADE, color1, color2, duration);
    } else if (effect == "blink") {
      unsigned long startTime = millis();
      while (millis() - startTime < duration) {
        setLEDColor(color1);
        delay(250);
        setLEDColor({ 0, 0, 0 });
        delay(250);
      }
      setLEDColor({ 0, 0, 0 });
    } else if (effect == "strobe") {
      unsigned long startTime = millis();
      while (millis() - startTime < duration) {
        setLEDColor(color1);
        delay(50);
        setLEDColor({ 0, 0, 0 });
        delay(50);
      }
      setLEDColor({ 0, 0, 0 });
    }
    broadcastOutput("✨ Startup effect completed: " + effect);
  }
#endif

#if ENABLE_AUTOMATION
  // Finally, run boot automations if configured
  Serial.println("[DEBUG] About to run boot automations");
  Serial.flush();
  runAutomationsOnBoot();
  Serial.println("[DEBUG] Boot automations completed");
  Serial.flush();
#endif

  // ESP-NOW auto-initialization (if enabled in settings) - moved to end of boot
  // This ensures all systems (WiFi, filesystem, settings) are fully initialized
#if ENABLE_ESPNOW
  Serial.println("[DEBUG] Checking ESP-NOW settings");
  Serial.flush();
  if (gSettings.espnowenabled) {
    broadcastOutput("[ESP-NOW] Auto-initialization enabled in settings");
    
    // Check first-time setup before attempting init
    const char* setupError = checkEspNowFirstTimeSetup();
    if (setupError && strlen(setupError) > 0) {
      broadcastOutput("[ESP-NOW] Auto-init skipped - first-time setup required:");
      broadcastOutput(setupError);
      broadcastOutput("[ESP-NOW] Set device name with: espnow setname <name>");
    } else {
      broadcastOutput("[ESP-NOW] Initializing...");
      const char* result = cmd_espnow_init("");  // Empty string is fine - validation happens in function
      broadcastOutput(result);
#if DEBUG_MEM_SUMMARY
      heapLogSummary("boot.after_espnow_init");
#endif
    }
  } else {
    DEBUG_SYSTEMF("ESP-NOW Auto-init: Disabled by setting (enable in web settings)");
  }
  Serial.println("[DEBUG] ESP-NOW check completed");
  Serial.flush();
#endif

  // Boot mode transition will be handled in loop() based on oledBootDuration setting

  // Print command/settings module summaries, then comprehensive memory report
  Serial.println("[DEBUG] About to print command module summary");
  Serial.flush();
  printCommandModuleSummary();
  Serial.println("[DEBUG] About to print settings module summary");
  Serial.flush();
  printSettingsModuleSummary();
  Serial.println("[DEBUG] About to print boot memory report");
  Serial.flush();
  printMemoryReport();
  Serial.println("[DEBUG] Setup() completed!");
  Serial.flush();
}


void hardwareone_loop() {
  // Drain debug ring buffer (safe from main loop context)
  drainDebugRing();

  // Periodic memory sampling (gated by DEBUG_MEMORY flag, runs every 2 seconds)
  periodicMemorySample();

  // Periodic battery monitoring (every 10 seconds)
#if ENABLE_BATTERY_MONITOR
  static unsigned long lastBatteryUpdate = 0;
  if (millis() - lastBatteryUpdate >= 10000) {
    lastBatteryUpdate = millis();
    updateBattery();
  }
#endif

  // Heap pressure monitoring - CONSOLIDATED into periodicMemorySample()
  // No longer needed here - heap warnings now triggered during memory sampling

  // Task pressure monitoring - comprehensive report every 1 minute
  // Only runs when DEBUG_MEMORY is enabled (gated to reduce overhead)
  if (isDebugFlagSet(DEBUG_MEMORY)) {
    static unsigned long lastTaskReport = 0;
    unsigned long now = millis();
    if (now - lastTaskReport >= 60000) {  // 1 minute
      lastTaskReport = now;
      reportAllTaskStacks();
    }
  }

#if ENABLE_AUTOMATION
  // Automation scheduler - runs when dirty flag set OR every 60 seconds
  if (gSettings.automationsEnabled) {
    static unsigned long lastAutoCheck = 0;
    unsigned long nowAuto = millis();
    if (gAutosDirty || (nowAuto - lastAutoCheck >= 60000)) {
      gAutosDirty = false;
      schedulerTickMinute();
      lastAutoCheck = nowAuto;
    }
  }
#endif

  // Performance monitoring (gated by debug flag)
  if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
    performanceCounter();
  }

  // I2C bus health monitoring - REMOVED: Now event-driven
  // Devices automatically trigger checkBusRecoveryNeeded() when they become degraded
  // No periodic polling needed in main loop

  // Process ESP-NOW message retry queue
#if ENABLE_ESPNOW
  if (gEspNow && gEspNow->initialized) {
    processMessageQueue();
  }
#endif

  // OLED boot sequence - handled by processOLEDBootSequence() in oled_display.cpp
#if ENABLE_OLED_DISPLAY
  processOLEDBootSequence();
#endif

  // ESP-NOW chunked message timeout cleanup
#if ENABLE_ESPNOW
  cleanupExpiredChunkedMessage();
  // Cleanup expired buffered PEER messages (topology discovery)
  cleanupExpiredBufferedPeers();
  // Topology collection window check now runs in ESP-NOW FreeRTOS task (espnowHeartbeatTask)
  // Cleanup timed-out chunk buffers (router reassembly)
  if (gEspNow && gEspNow->initialized) {
    cleanupTimedOutChunks();
  }
#endif
  // Debounced SSE sensor-status broadcast
  if (gSensorStatusDirty) {
    unsigned long nowMs = millis();
    DEBUG_SENSORSF("[SSE_BROADCAST_CHECK] dirty=true, due=%lu, now=%lu, ready=%d",
                   gNextSensorStatusBroadcastDue, nowMs,
                   (gNextSensorStatusBroadcastDue != 0 && (long)(nowMs - gNextSensorStatusBroadcastDue) >= 0) ? 1 : 0);
    if (gNextSensorStatusBroadcastDue != 0 && (long)(nowMs - gNextSensorStatusBroadcastDue) >= 0) {
      // Dump quick snapshot for diagnostics
      DEBUG_SENSORSF("[SSE_BROADCAST] SENDING | seq=%lu thermal=%d tof=%d imu=%d gamepad=%d apdsColor=%d apdsProx=%d apdsGest=%d",
                     (unsigned long)gSensorStatusSeq,
                     thermalEnabled ? 1 : 0, tofEnabled ? 1 : 0, imuEnabled ? 1 : 0, gamepadEnabled ? 1 : 0,
                     apdsColorEnabled ? 1 : 0, apdsProximityEnabled ? 1 : 0, apdsGestureEnabled ? 1 : 0);
      broadcastSensorStatusToAllSessions();
      DEBUG_SENSORSF("[SSE_BROADCAST] SENT successfully");
      gSensorStatusDirty = false;
      gNextSensorStatusBroadcastDue = 0;
    }
  }

  // BLE data streaming updates (auto-push sensor/system data at configured intervals)
#if ENABLE_BLUETOOTH
  bleUpdateStreams();
#endif

  // MQTT periodic publishing and reconnect handling
#if ENABLE_MQTT
  mqttTick();
#endif

  // Non-blocking Serial CLI
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String cmd = gSerialCLI;
      cmd.trim();

      // Serial auth gate: require login before executing any commands
      if (!gSerialAuthed) {
        if (cmd.startsWith("login ")) {
          // Parse: login <user> <pass>
          String rest = cmd.substring(6);
          rest.trim();
          int sp = rest.indexOf(' ');
          if (sp <= 0) {
            broadcastOutput("Usage: login <username> <password>");
          } else {
            String u = rest.substring(0, sp);
            String p = rest.substring(sp + 1);
            if (isValidUser(u, p)) {
              // Unified auth success flow for Serial transport
              AuthContext ctx;
              ctx.transport = SOURCE_SERIAL;
              ctx.user = u;
              ctx.ip = "local";
              ctx.path = "serial/login";
              ctx.sid = String();
#if ENABLE_HTTP_SERVER
              authSuccessUnified(ctx, nullptr);
#endif
              gSerialAuthed = true;
              gSerialUser = u;
              // Check admin status in real-time
              bool isCurrentlyAdmin = isAdminUser(u);
              broadcastOutput(String("[serial] Login successful. User: ") + u + (isCurrentlyAdmin ? " (admin)" : ""));
            } else {
              broadcastOutput("[serial] Authentication failed.");
            }
          }
        } else if (cmd.length() > 0) {
          // Block everything else (including 'clear') until login
          broadcastOutput("Serial - Authentication required. Use: login <username> <password>");
        }
      } else {
        // Authenticated: handle local session commands first
        if (cmd == "logout") {
          gSerialAuthed = false;
          gSerialUser = String();
          // gSerialIsAdmin no longer needed - using real-time checks
          broadcastOutput("Logged out.");
        } else if (cmd == "whoami") {
          bool isCurrentlyAdmin = gSerialUser.length() ? isAdminUser(gSerialUser) : false;
          broadcastOutput(String("You are ") + (gSerialUser.length() ? gSerialUser : String("(unknown)")) + (isCurrentlyAdmin ? " (admin)" : ""));
        } else {
          // Record the entered command into the unified feed with source tag (only after auth)
          appendCommandToFeed("serial", cmd);

          // Build unified command context for Serial origin
          AuthContext actx;
          actx.transport = SOURCE_SERIAL;
          actx.user = gSerialUser;
          actx.ip = "local";
          actx.path = "serial";
          Command uc;
          uc.line = cmd;
          uc.ctx.origin = ORIGIN_SERIAL;
          uc.ctx.auth = actx;
          uc.ctx.id = (uint32_t)millis();
          uc.ctx.timestampMs = (uint32_t)millis();
          uc.ctx.outputMask = CMD_OUT_SERIAL | CMD_OUT_LOG;
          uc.ctx.validateOnly = false;
          uc.ctx.replyHandle = nullptr;
          uc.ctx.httpReq = nullptr;

          String out;
          (void)submitAndExecuteSync(uc, out);
          broadcastOutput(out, uc.ctx);
        }
      }
      gSerialCLI = "";
      Serial.print("$ ");
    } else {
      gSerialCLI += c;
      // Optional: echo
      // Serial.print(c);
    }
  }

  // All sensor polling now handled by unified sensor polling task - no loop processing needed

  // Update OLED display
  oledUpdate();

  // Mesh heartbeat processing now runs in separate FreeRTOS task (espnowHeartbeatTask)
  // Started by initEspNow() -> startEspNowTask()

  // esp_http_server handles requests internally
  delay(1);
} 