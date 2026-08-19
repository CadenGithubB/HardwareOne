#include "Arduino.h"
#include <atomic>
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_system.h>
// Forward declarations to satisfy Arduino's auto-generated prototypes
#include "System_CommandTypes.h"
struct MeshPeerHealth;
struct TopologyStream;
struct RouterMetrics;

#include "System_Utils.h"

extern const CommandEntry commands[];
extern const size_t commandsCount;

static String originPrefix(const char* source, const String& user, const String& ip);
void runUnifiedSystemCommand(const String& argsInput);

extern uint32_t gLastHeartbeatSentMs;
extern const uint32_t MESH_HEARTBEAT_INTERVAL_MS;

// Web server functions (implemented in web_server.cpp) - declared here to prevent Arduino preprocessor from creating static versions
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
#include "System_VFS.h"
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
  #if ENABLE_WEB_SENSORS
    #include "WebPage_Sensors.h"
  #endif
  #if ENABLE_AUTOMATION
    #include "WebPage_Automations.h"
  #endif
  #if ENABLE_WEB_ESPNOW
    #include "WebPage_ESPNow.h"
  #endif
#endif

#if ENABLE_ESPNOW
  #include "System_ESPNow.h"
  #include "System_ESPNow_Crypto.h"
  #include "System_ESPNow_Identity.h"
  #include "System_ESPNow_Files.h"
  #include "System_ESPNow_MeshKeys.h"
  #include "System_ESPNow_Sessions.h"
  #include "System_ESPNow_Tx.h"
#endif
#include "Bluetooth.h"
#include "G2_Glasses.h"
#include "G2_Ring.h"
#include "BLE_Peers.h"
#include "System_Automation.h"
#include "System_Utils.h"
#include "System_User.h"
#include "System_AuthIdentity.h"
#include "System_Filesystem.h"
#include "System_SelfDevice.h"
#include "System_Clock.h"
#include "System_CLI.h"
#include "System_Cm5Presence.h"
#include "System_LiveAudio.h"
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
#include "System_CrashRecord.h"
#include "System_OTASafety.h"
#include "System_OTA.h"
#if ENABLE_HTTP_SERVER
  #include "WebServer_Server.h"
#endif
#include "System_Battery.h"
#include "System_FirstTimeSetup.h"
#include "System_SetupWizard.h"  // gWizardOwnsSerial — main loop yields Serial while legacy wizard is running
#include "System_UartLink.h"     // UART host link (CM5 command channel) — drain ticked from loop()
#include "System_LLMBackend.h"   // llmBackendTick() — remote-generation stall timeout
#if ENABLE_RASPBERRY_PI_HOST_POWER || ENABLE_RASPBERRY_PI_HOST_FAN
  #include "System_Cm5HostControl.h"  // finite CM5 host-power/host-fan request/ACK/report state machines
#endif
#include "System_CLIMode.h"      // cliModeTick — periodic tick for active CLIMode (Phase 5 wizard)
#include "System_TaskUtils.h"
#include "System_Notifications.h"  // systemEventsNotifyTick — main-loop toast render for bus events
#include "System_Events.h"         // systemEventPost — power-save enter/exit events
// sensor_config.h included early (before WiFi)
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor_bno055.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor_seesaw.h"
#endif
#if ENABLE_APDS_SENSOR
  #include "i2csensor_apds9960.h"
#endif
#if ENABLE_GPS_SENSOR
  #include "i2csensor_pa1010d.h"
#endif
#if ENABLE_RTC_SENSOR
  #include "i2csensor_ds3231.h"
#endif
#include "System_SensorStubs.h"  // Minimal stubs only where required
#include "i2csensor_rda5807.h"
#include "OLED_Display.h"  // Always include - wrapper functions are safe to call when disabled
#include "System_NeoPixel.h"
#include "System_MemoryMonitor.h"
#if ENABLE_WEB_GAMES
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
#if ENABLE_SERVO
#include <Adafruit_PWMServoDriver.h>
#endif
#if ENABLE_GPS_SENSOR
#include <Adafruit_GPS.h>
#endif
#include <vector>
#include <functional>
#include "System_MemUtil.h"

bool createInputTask();
bool isSensorConnected(const char* moduleName);
bool gamepadInit();


// Pre-allocation snapshots (used by mem_util.h)
size_t gAllocHeapBefore = 0;
size_t gAllocPsBefore = 0;

// AllocEntry struct defined in System_MemUtil.h
extern const int MAX_ALLOC_ENTRIES = 64;
EXT_RAM_BSS_ATTR AllocEntry gAllocTracker[MAX_ALLOC_ENTRIES];
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
#define DEBUG_MEM_SUMMARY 1
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
void setupWiFi();

// WiFi global flags (defined in wifi_system.cpp)
extern volatile bool gWifiUserCancelled;
extern bool gSkipNTPInWifiConnect;

// Output/logging used widely before definition

// Global variables forward declarations
// filesystemReady is provided by System_Filesystem.h (included above).

// Centralized command execution with AuthContext (transport-agnostic)
static const char* originFrom(const AuthContext& ctx);
bool hasAdminPrivilege(const AuthContext& ctx);
bool hasSuperAdminPrivilege(const AuthContext& ctx);

struct ExecReq;
QueueHandle_t gCmdExecQ = nullptr;
TaskHandle_t gCmdExecTaskHandle = nullptr;
static std::atomic<uint32_t> sCmdExecHeartbeatMs{0};
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
extern String gBootId;

// ---------------------------------------------------------------------------
// Serial auth globals
// ---------------------------------------------------------------------------
// NTP anchor ID tracking for user creation timestamp resolution
// Exported to user_system.cpp
uint32_t gNTPAnchorId = 0;
uint32_t gBootCounter = 0;

bool gSerialAuthed = false;
String gSerialUser = String();
// Last REAL serial interaction (millis timebase, 0 = never). Serial has no
// passive traffic — every completed command line is a human keystroke — so
// there's no interaction classifier here (unlike web). One session only, so
// this is a flat global alongside gSerialAuthed/gSerialUser rather than a
// struct field. Drives the shared idle-logout policy (sessionIdleExpired).
std::atomic<unsigned long> gSerialLastInteractionMs{0};

bool gLocalDisplayAuthed = false;
String gLocalDisplayUser = String();
// Last REAL physical interaction with the OLED session (millis, 0 = never).
// Stamped on local-display login and whenever gInputCache.seq advances (a
// gamepad/ANO button or encoder edge). Network commands never touch it, so an
// OLED login still goes idle while the box is busy serving web/ESP-NOW. Drives
// the shared per-transport idle-logout policy (sessionIdleExpired).
std::atomic<unsigned long> gLocalDisplayLastInteractionMs{0};

esp_err_t handleSensorsStatus(httpd_req_t* req);

// gSensorPollingPaused now lives in System_PollPause.cpp (its own module);
// HardwareOne sees the declaration via System_I2C.h.

#include "System_SensorLogging.h"
#include "System_TimeAnchors.h"

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
  DEBUG_ESPNOW_METADATAF("[STATUS_BUMP] seq=%lu cause='%s' | thermal=%d tof=%d imu=%d gamepad=%d",
                 (unsigned long)gSensorStatusSeq, gLastStatusCause,
                 gThermalRunning ? 1 : 0, gTofRunning ? 1 : 0, gImuRunning ? 1 : 0, gInputRunning ? 1 : 0);
  DEBUG_SSEF("sensorStatusBump: seq now %lu | cause=%s (debounced)", (unsigned long)gSensorStatusSeq, gLastStatusCause);
  // Mark dirty and schedule debounced broadcast
  gSensorStatusDirty = true;
  unsigned long nowMs = millis();
  if (gNextSensorStatusBroadcastDue == 0 || (long)(nowMs - gNextSensorStatusBroadcastDue) > 0) {
    gNextSensorStatusBroadcastDue = nowMs + kSensorStatusDebounceMs;
    DEBUG_ESPNOW_METADATAF("[STATUS_BUMP] Broadcast scheduled for %lu ms from now", kSensorStatusDebounceMs);
  } else {
    DEBUG_ESPNOW_METADATAF("[STATUS_BUMP] Broadcast already scheduled (due in %ld ms)", (long)(gNextSensorStatusBroadcastDue - nowMs));
  }
#if ENABLE_BONDED_MODE
  // Proactively push status to bonded peer so master sees changes immediately
  if (gEspNow && gEspNow->initialized && isBondSynced()) {
    gEspNow->bondNeedsProactiveStatus = true;
  }
#endif
}

extern Adafruit_NeoPixel pixels;
extern BatteryState gBatteryState;

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

// Per-task identity (formerly gExecUser / gExecIsAdmin / gExecAuthContext) now
// lives in each task's TLS slot via System_AuthIdentity. Use currentAuthContext()
// / currentExecUser() / currentExecIsAdmin() for reads, ExecIdentityGuard or
// SYSTEM_IDENTITY_SCOPE for writes.

#include "System_RamFlush.h"
#include "System_Settings.h"
#include "System_Power.h"  // powerSleepTransitionAllowed/Mark — power-save anti-flap guard
Settings gSettings;

String gSerialCLI = "";
static TransportSessionEpoch gSerialCLIEpoch = 0;

#if ENABLE_WIFI
WifiNetwork* gWifiNetworks = nullptr;
int gWifiNetworkCount = 0;
#endif

extern "C" void __attribute__((weak)) memAllocDebug(const char* op, void* ptr, size_t size,
                                                    bool requestedPS, bool usedPS, const char* tag) {
  (void)op;
  (void)requestedPS;
  if (!gAllocTrackerEnabled || !tag || !ptr) return;

  // Find-or-insert by tag. No mutex: tracker is a diagnostic accumulator and
  // races at worst produce slightly wrong byte totals — never crashes.
  int idx = -1;
  for (int i = 0; i < gAllocTrackerCount; i++) {
    if (strcmp(gAllocTracker[i].tag, tag) == 0) {
      idx = i;
      break;
    }
  }
  if (idx == -1) {
    if (gAllocTrackerCount >= MAX_ALLOC_ENTRIES) return;
    idx = gAllocTrackerCount++;
    strncpy(gAllocTracker[idx].tag, tag, sizeof(gAllocTracker[idx].tag) - 1);
    gAllocTracker[idx].tag[sizeof(gAllocTracker[idx].tag) - 1] = '\0';
    gAllocTracker[idx].totalBytes = 0;
    gAllocTracker[idx].psramBytes = 0;
    gAllocTracker[idx].dramBytes = 0;
    gAllocTracker[idx].count = 0;
    gAllocTracker[idx].isActive = true;
  }
  gAllocTracker[idx].totalBytes += size;
  gAllocTracker[idx].count++;
  if (usedPS) {
    gAllocTracker[idx].psramBytes += size;
  } else {
    gAllocTracker[idx].dramBytes += size;
  }
}

#if ENABLE_HTTP_SERVER
void sseEnqueueNotice(SessionEntry& s, const String& msg);
bool sseDequeueNotice(SessionEntry& s, String& out);
#endif

volatile uint32_t gOutputFlags = MSG_ROUTE_SERIAL;

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
  broadcastOutput(stripANSICSI(s));
}

void appendCommandToFeed(const char* source, const String& cmd, const String& user = String(), const String& ip = String()) {
  // Machine CM5/live-audio inspection may be polled from UART, web, serial,
  // or a future interface. Keep this policy at the shared feed boundary so no
  // transport can evict useful human history with status/capability polls.
  if (cm5PresenceIsProtocolCommand(cmd.c_str()) ||
      liveAudioIsHousekeepingCommand(cmd.c_str())) return;
  char prefix[128];
  if (user.length() || ip.length()) {
    snprintf(prefix, sizeof(prefix), "[%s %s%s%s] $ ",
             source,
             user.length() ? user.c_str() : "",
             ip.length() ? "@" : "",
             ip.length() ? ip.c_str() : "");
  } else {
    snprintf(prefix, sizeof(prefix), "[%s] $ ", source);
  }
  String line = String(prefix) + redactCmdForAudit(cmd);
  // Write directly to web mirror (command feed, not via debug queue)
  if (gWebMirror.buf) {
    gWebMirror.appendDirect(line.c_str(), line.length(), true);
  }
}

static String originPrefix(const char* source, const String& user, const String& ip) {
  char buf[128];
  if (user.length() || ip.length()) {
    snprintf(buf, sizeof(buf), "[%s %s%s%s] ",
             source,
             user.length() ? user.c_str() : "",
             ip.length() ? "@" : "",
             ip.length() ? ip.c_str() : "");
  } else {
    snprintf(buf, sizeof(buf), "[%s] ", source);
  }
  return String(buf);
}

static void broadcastCommandResultQueued(const String& prefix, const String& body, uint8_t route) {
  extern void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route);

  const size_t maxFrameLen = DEBUG_MSG_SIZE - 1;  // leave room for NUL in DebugMessage::text
  const size_t prefixLen = prefix.length();
  const size_t bodyLen = body.length();

  if (prefixLen + bodyLen <= maxFrameLen) {
    String line = prefix;
    line += body;
    broadcastOutputCore_Routed(line.c_str(), line.length(), route);
    return;
  }

  // Command return values may be up to CMD_RESULT_MAX (4096), but the debug
  // fan-out queue is intentionally line-sized. Split only the queued display
  // copy; the caller's return buffer and targeted BLE reply stay intact.
  char frame[DEBUG_MSG_SIZE];
  const char* prefixC = prefix.c_str();
  const char* bodyC = body.c_str();

  size_t prefixCopy = prefixLen < maxFrameLen ? prefixLen : maxFrameLen;
  size_t payloadCap = maxFrameLen - prefixCopy;
  if (payloadCap == 0) {
    // Pathological username/IP/source prefix: emit the clipped prefix first,
    // then continue with body-only chunks rather than dropping the result.
    memcpy(frame, prefixC, maxFrameLen);
    frame[maxFrameLen] = '\0';
    broadcastOutputCore_Routed(frame, maxFrameLen, route);
    prefixCopy = 0;
    payloadCap = maxFrameLen;
  }

  const uint32_t paceStartMs = millis();
  for (size_t off = 0; off < bodyLen; off += payloadCap) {
    // Pace the burst against the single drain task: a 4 KB result is ~17
    // frames against a 96-slot pool, and enqueueChunk drops (never blocks)
    // when the free list is exhausted — without pacing, a busy queue turns
    // the tail of the result into silent "[output] N line(s) dropped".
    // The TOTAL pacing cost is budgeted: this runs on loopTask for serial
    // results and on httpd tasks for web, and a wedged drain must not hold
    // the caller for 17 × 50 ms. Past the budget, frames fall back to
    // drop-with-marker — the pre-pacing behavior.
    if ((uint32_t)(millis() - paceStartMs) < 250) debugQueueBackpressure(8, 50);

    size_t n = bodyLen - off;
    if (n > payloadCap) n = payloadCap;

    size_t pos = 0;
    if (prefixCopy > 0) {
      memcpy(frame, prefixC, prefixCopy);
      pos = prefixCopy;
    }
    memcpy(frame + pos, bodyC + off, n);
    pos += n;
    frame[pos] = '\0';
    broadcastOutputCore_Routed(frame, pos, route);
  }
}

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
  return isAdminUser(ctx.user);
}

bool hasSuperAdminPrivilege(const AuthContext& ctx) {
  return isSuperAdminUser(ctx.user);
}

// CommandOrigin, CommandContext, Command, ExecReq, ExecAsyncCallback
// are defined in System_CommandTypes.h (included at top of file)

// Per-task current command context lives in the calling task's TLS slot
// (Stage 3) — was a shared void* global, which routed background-task
// broadcasts to whichever WebSocket session cmd_exec was currently serving.
// Accessor lives in System_AuthIdentity; getCurrentCommandOutputMask() is
// the convenience wrapper for the broadcastOutput route-mask computation.
uint32_t getCurrentCommandOutputMask() {
  void* ctx = currentCommandContext();
  if (!ctx) return 0xFFFFFFFF;
  return ((CommandContext*)ctx)->outputMask;
}

// Per-command identity install lives inside executeCommand via
// ExecIdentityGuard, which writes the calling task's TLS slot. There is no
// global save/restore here — the prior pattern is structurally obsolete.

// Now that ExecReq is fully defined we can implement the task
static void commandExecTask(void* pv) {
  DEBUG_CMD_FLOWF("[cmd_exec] task started");
  static unsigned long lastStackCheck = 0;
  // BYTES: the constant is the byte count passed to xTaskCreate and the HWM is
  // returned in bytes on this port — no word->byte scaling. See System_TaskUtils.h.
  constexpr uint32_t stackBytes = CMD_EXEC_STACK_WORDS;
  for (;;) {
    // Mode cleanup is executor-affine: transport callbacks and read-only
    // queries only request cancellation. Drain before receiving new work so
    // owner loss/idle timeout is handled even while the command queue is idle.
    (void)cliModeExecutorDrainPending();
    // The OTA probation gate uses this as proof that the command worker is
    // scheduled and able to return to its receive loop. A bounded receive wait
    // keeps the heartbeat fresh while the queue is idle; a wedged command leaves
    // it stale and prevents the trial image from becoming permanent.
    sCmdExecHeartbeatMs.store(millis(), std::memory_order_release);
    // Periodic stack watermark check (every 30 seconds)
    unsigned long now = millis();
    if (now - lastStackCheck > 30000) {
      UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(NULL);
      uint32_t stackPeak = stackBytes - (uint32_t)stackHighWater;
      int peakPct = (stackPeak * 100) / stackBytes;

      DEBUG_MEMORY_STACKF("[STACK] cmd_exec: peak=%lu bytes (%d%%), free_min=%lu bytes, total=%lu",
                    (unsigned long)stackPeak, peakPct,
                    (unsigned long)stackHighWater, (unsigned long)stackBytes);
      lastStackCheck = now;
    }

    ExecReq* r = nullptr;
    BaseType_t receiveResult = xQueueReceive(gCmdExecQ, &r, pdMS_TO_TICKS(1000));
    
    if (receiveResult == pdTRUE) {
      if (!r) continue;

      // Deferred-work fast path — used by espnow_task to run heavy crypto
      // (Ed25519 sign/verify in SESSION_OPEN/CONFIRM) on cmd_exec_task's
      // deeper stack. Skip the entire CLI execution pipeline: no auth
      // context push, no capture buffer, no output formatting. The deferred
      // function owns its arg's lifetime.
      if (r->deferredFn) {
        r->deferredFn(r->deferredArg);
        r->~ExecReq();
        free(r);
        continue;
      }

      const String safeExecLine = redactCmdForAudit(String(r->line));
      DEBUG_CMD_FLOWF("[cmd_exec] exec '%.80s' user='%s' heap=%lu",
                  safeExecLine.c_str(), r->ctx.auth.user.c_str(), (unsigned long)ESP.getFreeHeap());

      setCurrentCommandContext(&r->ctx);
      bool prevValidate = gCLIValidateOnly;
      gCLIValidateOnly = r->ctx.validateOnly;

      // Set up output capture if requested (for HTTP responses that need
      // the actual broadcast output, not just the return code).
      // Heap-allocate the capture buffer to avoid blowing cmd_exec_task stack.
      // Per-task TLS now — broadcastOutput on other tasks won't see this
      // buffer and won't append into it.
      static constexpr size_t CAPTURE_BUF_SIZE = 4096;
      char* captureBuf = nullptr;
      if (r->ctx.captureOutput) {
        captureBuf = (char*)ps_alloc(CAPTURE_BUF_SIZE, AllocPref::PreferPSRAM,
                                     "cmd.capture");
        if (captureBuf) {
          captureBuf[0] = '\0';
          setCaptureBuffer(captureBuf, CAPTURE_BUF_SIZE);
        }
      }

      r->ok = executeCommand((AuthContext&)r->ctx.auth, r->line, r->out, sizeof(r->out));

      // Tear down capture and merge captured output into r->out
      if (captureBuf) {
        CaptureBufState* capState = currentCaptureState();
        size_t capturedLen = capState ? capState->len : 0;
        clearCaptureBuffer();
        if (capturedLen > 0) {
          bool trivialReturn = (strcmp(r->out, "OK") == 0);
          if (trivialReturn) {
            // Replace "OK" with the captured output
            size_t copyLen = capturedLen < sizeof(r->out) - 1 ? capturedLen : sizeof(r->out) - 1;
            memcpy(r->out, captureBuf, copyLen);
            r->out[copyLen] = '\0';
          } else {
            // Prepend captured output to the return value.
            // Shift existing r->out right to make room, then copy capture in front.
            size_t capLen = capturedLen;
            size_t retLen = strlen(r->out);
            size_t maxOut = sizeof(r->out) - 1;
            if (capLen + retLen > maxOut) {
              // Truncate return portion to fit
              retLen = (capLen < maxOut) ? maxOut - capLen : 0;
            }
            memmove(r->out + capLen, r->out, retLen);
            memcpy(r->out, captureBuf, capLen);
            r->out[capLen + retLen] = '\0';
          }
        }
        free(captureBuf);
      }

      gCLIValidateOnly = prevValidate;
      clearCurrentCommandContext();
      (void)cliModeExecutorDrainPending();
      DEBUG_CMD_FLOWF("[cmd_exec] done ok=%d out_len=%zu heap=%lu",
                  r->ok ? 1 : 0, strlen(r->out), (unsigned long)ESP.getFreeHeap());
      
      // 2026-05-18 — if submitSync gave up waiting (>60 s) it sets
      // r->abandoned = true and returns without freeing. We MUST clean up
      // here instead, otherwise the ExecReq + its `done` semaphore leak
      // forever (and ps_alloc would eventually exhaust PSRAM).
      if (r->abandoned) {
        Serial.printf("[DBG_CMD] cmd_exec: r=%p was abandoned by caller — cleaning up\n", r);
        if (r->done) {
          vSemaphoreDelete(r->done);
          r->done = nullptr;
        }
        r->~ExecReq();
        free(r);
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      // Handle completion: async callback OR semaphore
      if (r->asyncCallback) {
        r->asyncCallback(r->ok, r->out, r->asyncUserData);
        // Async mode: we own the ExecReq, free it
        r->~ExecReq();
        free(r);
      } else if (r->done) {
        xSemaphoreGive(r->done);
      } else {
        DEBUG_CMD_FLOWF("[cmd_exec] WARNING: No callback and no semaphore!");
        r->~ExecReq();
        free(r);
      }
      // Yield between commands to prevent starving Core 0 ISRs (I2C, UART, WiFi)
      // Without this, back-to-back commands can trigger Interrupt WDT
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// Context-aware broadcastOutput that includes origin/user/path metadata.
// Used to broadcast a command's return value after execution completes.
// Routing is derived from ctx.outputMask (MSG_ROUTE_* sink bits).
void broadcastOutput(const String& s, const CommandContext& ctx) {
  const char* source;
  switch (ctx.origin) {
    case ORIGIN_SERIAL: source = "serial"; break;
    case ORIGIN_WEB: source = "web"; break;
    case ORIGIN_AUTOMATION: source = "auto"; break;
    case ORIGIN_BLUETOOTH: source = "bluetooth"; break;
    case ORIGIN_G2_HIJACK: source = "g2"; break;
    case ORIGIN_ESPNOW: source = "espnow"; break;
    case ORIGIN_LOCAL_DISPLAY: source = "oled"; break;
    case ORIGIN_MQTT: source = "mqtt"; break;
    case ORIGIN_VOICE: source = "voice"; break;
    case ORIGIN_UART: source = "uart"; break;
    case ORIGIN_SYSTEM:
    default: source = "system"; break;
  }

  String prefix = originPrefix(source, ctx.auth.user, ctx.auth.ip);
  const String safeOutputForTrace = redactOutputForLog(s);
  DEBUG_CMD_FLOWF("[BROADCAST_CTX] origin=%s user=%s mask=0x%02lX msg='%.50s'",
                  source, ctx.auth.user.c_str(),
                  (unsigned long)ctx.outputMask, safeOutputForTrace.c_str());

  // outputMask holds MSG_ROUTE_* bits directly; keep only the sinks a
  // command may address. OLED and G2 always included for command return values.
  uint8_t route = (uint8_t)(ctx.outputMask & (MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE))
                | MSG_ROUTE_OLED | MSG_ROUTE_G2;

  // Pass explicit route to the queued command-result fan-out. This runs AFTER
  // the command's per-task currentCommandContext has been cleared, so the
  // implicit fallback would be MSG_ROUTE_ALL. Use the ctx we already have to
  // compute the correct route instead. Long results are split here because
  // command handlers can return up to CMD_RESULT_MAX while DebugMessage::text
  // remains a deliberately small line-sized envelope.
  broadcastCommandResultQueued(prefix, s, route);
  // (No web-mirror backfill here: the drain appends WEB-routed messages to
  // the mirror unconditionally, so a backfill would double-append.)

  // Targeted BLE response (direct send to originating connection, not via queue)
  if (ctx.outputMask & MSG_ROUTE_BLE) {
    String prefixed = prefix;
    prefixed += s;
    uint16_t targetConnId = 0;
    if (ctx.auth.sid.length() > 0) {
      targetConnId = (uint16_t)ctx.auth.sid.toInt();
    }
    if (targetConnId > 0) {
      sendBLEResponseToConn(targetConnId, prefixed.c_str(), prefixed.length());
    } else {
      sendBLEResponse(prefixed.c_str(), prefixed.length());
    }
  }

  DEBUG_CMD_FLOWF("[broadcast] route=0x%02X len=%d", route, s.length());
}

// Transport-completion delivery for the command return value (OUTPUT CONTRACT
// channel 2). Serial is the one origin with no reply channel of its own — web
// answers in the HTTP body, BLE in a targeted notify — so its result used to
// be pushed through the 256 B line queue in ~17 decorated chunks. Here the
// serial-origin blob is written to the console directly instead: byte-exact,
// copy-pasteable, prompt printed after it by the caller. All other sinks
// (file log, OLED console, web mirror) keep the queued chunked copy they get
// today, so nothing else changes shape. Non-serial origins pass through to
// broadcastOutput(s, ctx) untouched.
void deliverCommandResult(const String& result, const CommandContext& ctx) {
  // A queued command may finish after logout/re-login/revocation. Execution
  // has its own admission fence, but result delivery is a separate authority
  // boundary: never hand an old session's result to the replacement console or
  // to shared mirrors. Stateless producers do not set REQUIRE_LIVE_SESSION.
  const bool requiresLiveSession =
      (ctx.behaviorFlags & COMMAND_CONTEXT_REQUIRE_LIVE_SESSION) != 0;
  if (requiresLiveSession &&
      (ctx.transportSessionEpoch == 0 ||
       !transportSessionEpochIsLive(ctx.auth.transport,
                                    ctx.transportSessionEpoch))) {
    DEBUG_CMD_FLOWF(
        "[deliver] dropping stale result origin=%d transport=%d epoch=%lu",
        static_cast<int>(ctx.origin), static_cast<int>(ctx.auth.transport),
        static_cast<unsigned long>(ctx.transportSessionEpoch));
    return;
  }

  const bool serialDirect = (ctx.origin == ORIGIN_SERIAL) && (ctx.outputMask & MSG_ROUTE_SERIAL);
  if (!serialDirect) {
    broadcastOutput(result, ctx);
    return;
  }

  // The direct write bypasses the queue pipeline, so it must re-ask the
  // pipeline's own gates: validate-only suppression (per-command
  // ctx.validateOnly — the race-free equivalent of the gCLIValidateOnly
  // global broadcastOutputCore checks, which cmd_exec may already have
  // re-set for its NEXT queued command by the time we run), and the outSerial
  // kill-switch the drain applies per line (System_Debug.cpp serial sink).
  const bool writeSerial = result.length() > 0
                        && !ctx.validateOnly
                        && (gOutputFlags & MSG_ROUTE_SERIAL);

  String framed;

  if (writeSerial) {
    // Let lines the command streamed during execution drain first, so the
    // result can't overtake them on the console. Bounded: a jammed queue
    // delays the reply, never blocks it. Queue-empty still leaves at most
    // one in-flight line racing us, and the CDC/UART driver's per-write
    // lock keeps that whole-line — it can never splice inside the blob.
    debugWaitOutputDrained(120);

    // Blob + trailing newline in ONE write() call == one locked CDC/UART
    // transaction, atomic against debug_out's per-line printf: a debug line
    // can land before or after this write, never inside it. (Two separate
    // writes would let a line glue itself between the blob and its newline.)
    // USB-CDC short-writes when the host stops draining (100 ms window) —
    // surface that rather than lose the tail silently. Classic-UART boards
    // (QT Py / Feather V2) instead block until fully sent (~350 ms for 4 KB
    // at 115200) while holding the UART lock, pausing the drain for that
    // window — accepted cost on those secondary targets.
    framed.reserve(result.length() + 1);
    framed = result;
    framed += '\n';
  }

  // Final admission and delivery are one serial-session transaction. A web
  // revocation or live auth-policy change that wins before this point drops
  // everything; one that arrives later waits for the bounded physical write
  // and mirror enqueue, so a result can never cross into its replacement
  // session between a check and the actual output.
  if (requiresLiveSession &&
      !serialTransportSessionBeginDelivery(ctx.transportSessionEpoch)) {
    return;
  }

  if (writeSerial) {
    if (framed.length() == result.length() + 1) {
      const size_t wrote = Serial.write((const uint8_t*)framed.c_str(), framed.length());
      if (wrote < framed.length()) {
        BROADCAST_PRINTF("[serial] result short-write %u/%u B (host stopped draining?)",
                         (unsigned)wrote, (unsigned)framed.length());
      }
    } else {
      // OOM building the framed copy — fall back to two writes. Worst case a
      // racing debug line lands between blob and newline; the bytes still
      // arrive intact.
      size_t wrote = Serial.write((const uint8_t*)result.c_str(), result.length());
      wrote += Serial.write((uint8_t)'\n');
      if (wrote < result.length() + 1) {
        BROADCAST_PRINTF("[serial] result short-write %u/%u B (host stopped draining?)",
                         (unsigned)wrote, (unsigned)(result.length() + 1));
      }
    }
  }

  // Mirror to the remaining sinks (file/OLED/G2, plus web when masked)
  // exactly as before; the queued serial copy is replaced by the direct
  // write above.
  CommandContext mirrorCtx = ctx;
  mirrorCtx.outputMask &= ~(uint32_t)MSG_ROUTE_SERIAL;
  if (!cliModeOwnedBySession(ctx.auth.transport,
                             ctx.transportSessionEpoch)) {
    broadcastOutput(result, mirrorCtx);
  }
  if (requiresLiveSession) serialTransportSessionEndDelivery();
}

char* gFileReadBuf = nullptr;
char* gFileOutBuf = nullptr;
// `extern const`: WebServer_Server.cpp declares these `extern const size_t`,
// and until 2026-08-19 they were DEFINED here as plain `size_t` — two
// declarations of one variable with different types, which is an ODR/type
// violation the compiler cannot diagnose across TUs. They are written exactly
// once (here) and only read, so const is the truth. The explicit `extern` is
// REQUIRED: a namespace-scope const has internal linkage by default, and
// without it WebServer_Server.cpp would fail to link. Side benefit: both move
// from .data (DRAM) to .rodata (flash).
extern const size_t kFileReadBufSize = 2048;
extern const size_t kFileOutBufSize = 2048;

bool ensureFileViewBuffers() {
  if (!gFileReadBuf) {
    gFileReadBuf = (char*)ps_alloc(kFileReadBufSize, AllocPref::PreferPSRAM, "http.file.read");
  }
  if (!gFileOutBuf) {
    gFileOutBuf = (char*)ps_alloc(kFileOutBufSize, AllocPref::PreferPSRAM, "http.file.out");
  }
  return gFileReadBuf && gFileOutBuf;
}

// ---------------------------------------------------------------------------
// Loop-health instrument + per-section profiler (replaces performanceCounter).
//
// loopHealthTick() runs FIRST each lap and is the lap-timing anchor; the loop
// calls perfMarkSection(i) after each of its 6 sections so we know where the
// lap's time went. Two tiers:
//   • Tier 1 (always-on, ~free): time each lap via esp_timer and emit a
//     rate-limited [LOOPHEALTH] WARN whenever a lap exceeds LOOP_STALL_US,
//     ATTRIBUTED via the profiler — either to a dominant loop section
//     ("loop-bound: INPUT 1510 ms", e.g. a synchronous serial command) or to
//     time spent outside any section ("840 ms outside sections" = the loop was
//     blocked/preempted, e.g. another task hogging the core). A worst-N ring +
//     total count are kept for the `perftop` command. Always on, no flag.
//   • Tier 2 (DEBUG_PERFORMANCE): every 5 s print laps/s, period min/avg/max,
//     a latency histogram, and the per-section average breakdown. The window
//     rolls every 5 s regardless of the flag (stats stay bounded); the snapshot
//     is what `perftop` reads on demand.
//
// Caveat: section timing is wall-clock between marks, so a preemption that
// lands mid-section inflates that section. The inLoop-vs-outside split (and
// `perftop`'s live task CPU%) disambiguate. Tunable: LOOP_STALL_US.
// ---------------------------------------------------------------------------
static const uint32_t LOOP_STALL_US = 200000;  // 200 ms stall threshold
static const char* const kLoopSectionNames[6] = {
  "DIAG", "IO", "EVENT", "NET", "DISPLAY", "INPUT"
};

struct LoopPerfState {
  // per-lap section scratch: filled by perfMarkSection() during the lap,
  // consumed by loopHealthTick() at the top of the NEXT lap.
  int64_t  lastMarkUs;
  uint32_t lapSectionUs[6];

  // current 5 s window accumulators
  int64_t  lastLapUs;
  uint32_t laps;
  uint32_t minUs, maxUs;
  uint64_t sumUs;
  uint32_t stallsWindow;
  uint32_t buckets[6];                 // <16,<32,<64,<128,<256,>=256 ms
  uint64_t sectionSumUs[6];
  unsigned long lastReportMs;
  unsigned long lastStallWarnMs;
  uint32_t stallSuppressed;

  // snapshot of the last completed window (stable read for perftop)
  bool     snapValid;
  uint32_t snapLapsPerSec, snapMinMs, snapAvgMs, snapMaxMs, snapStalls;
  uint32_t snapBuckets[6];
  uint32_t snapSectionAvgUs[6];

  // all-time
  uint32_t totalStalls;
  struct WorstStall { uint32_t ms; uint32_t atMs; uint8_t dom; bool inLoop; } worst[5];
  uint8_t  worstCount;
};
static LoopPerfState gLoopPerf;   // zero-initialised (static storage)

// Unlike millis()-since-power-on, this timestamp starts only after setup has
// actually reached CRASH_PHASE_RUNNING. It owns the existing crash-counter
// healthy marker; OTA validation has its stricter completed-loop gate below.
static bool sHardwareOneRunning = false;
static unsigned long sHardwareOneRunningSinceMs = 0;

// Called after each loop section: record that section's wall-clock duration and
// advance the mark. Cheap — one esp_timer read + a store.
static inline void perfMarkSection(uint8_t idx) {
  if (idx >= 6) return;
  int64_t now = esp_timer_get_time();
  gLoopPerf.lapSectionUs[idx] = (uint32_t)(now - gLoopPerf.lastMarkUs);
  gLoopPerf.lastMarkUs = now;
}

static void loopHealthTick() {
  const int64_t       nowUs = esp_timer_get_time();
  const unsigned long nowMs = millis();

  if (gLoopPerf.lastLapUs != 0) {
    uint32_t dtUs = (uint32_t)(nowUs - gLoopPerf.lastLapUs);
    uint32_t dtMs = dtUs / 1000;

    // period distribution (laps==0 → first lap of the window seeds min/max)
    if (gLoopPerf.laps == 0) { gLoopPerf.minUs = dtUs; gLoopPerf.maxUs = dtUs; }
    else { if (dtUs < gLoopPerf.minUs) gLoopPerf.minUs = dtUs;
           if (dtUs > gLoopPerf.maxUs) gLoopPerf.maxUs = dtUs; }
    gLoopPerf.laps++;
    gLoopPerf.sumUs += dtUs;
    uint8_t b = dtMs < 16 ? 0 : dtMs < 32 ? 1 : dtMs < 64 ? 2 : dtMs < 128 ? 3 : dtMs < 256 ? 4 : 5;
    gLoopPerf.buckets[b]++;

    // accumulate the just-finished lap's per-section times + find the hotspot
    uint32_t inLoopUs = 0;
    uint8_t  dom = 0;
    for (uint8_t i = 0; i < 6; i++) {
      gLoopPerf.sectionSumUs[i] += gLoopPerf.lapSectionUs[i];
      inLoopUs += gLoopPerf.lapSectionUs[i];
      if (gLoopPerf.lapSectionUs[i] > gLoopPerf.lapSectionUs[dom]) dom = i;
    }

    // Tier 1: stall detection + attribution
    if (dtUs >= LOOP_STALL_US) {
      bool inLoop = inLoopUs >= (dtUs / 2);   // ≥half the lap was inside sections
      gLoopPerf.stallsWindow++;
      gLoopPerf.totalStalls++;

      // worst-N ring (keep the 5 largest by duration)
      if (gLoopPerf.worstCount < 5) {
        gLoopPerf.worst[gLoopPerf.worstCount++] = { dtMs, (uint32_t)nowMs, dom, inLoop };
      } else {
        uint8_t smallest = 0;
        for (uint8_t i = 1; i < 5; i++)
          if (gLoopPerf.worst[i].ms < gLoopPerf.worst[smallest].ms) smallest = i;
        if (dtMs > gLoopPerf.worst[smallest].ms)
          gLoopPerf.worst[smallest] = { dtMs, (uint32_t)nowMs, dom, inLoop };
      }

      if (gLoopPerf.lastStallWarnMs == 0 || (nowMs - gLoopPerf.lastStallWarnMs) >= 5000UL) {
        const char* more = gLoopPerf.stallSuppressed ? " [+more suppressed]" : "";
        if (inLoop) {
          BROADCAST_PRINTF("[LOOPHEALTH] stall: lap took %lu ms | loop-bound: %s %lu ms%s",
                           (unsigned long)dtMs, kLoopSectionNames[dom],
                           (unsigned long)(gLoopPerf.lapSectionUs[dom] / 1000UL), more);
        } else {
          BROADCAST_PRINTF("[LOOPHEALTH] stall: lap took %lu ms | %lu ms outside sections (blocked/preempted — run perftop)%s",
                           (unsigned long)dtMs, (unsigned long)((dtUs - inLoopUs) / 1000UL), more);
        }
        gLoopPerf.lastStallWarnMs = nowMs;
        gLoopPerf.stallSuppressed = 0;
      } else {
        gLoopPerf.stallSuppressed++;
      }
    }
  }
  gLoopPerf.lastLapUs  = nowUs;
  gLoopPerf.lastMarkUs = nowUs;   // section[0] timing starts here

  // 5 s window roll: snapshot (for perftop) + print (DEBUG_PERFORMANCE) + reset.
  if (gLoopPerf.lastReportMs == 0) gLoopPerf.lastReportMs = nowMs;
  if ((nowMs - gLoopPerf.lastReportMs) >= 5000UL) {
    if (gLoopPerf.laps > 0) {
      gLoopPerf.snapValid      = true;
      gLoopPerf.snapLapsPerSec = gLoopPerf.laps / 5UL;
      gLoopPerf.snapMinMs      = gLoopPerf.minUs / 1000UL;
      gLoopPerf.snapAvgMs      = (uint32_t)(gLoopPerf.sumUs / gLoopPerf.laps) / 1000UL;
      gLoopPerf.snapMaxMs      = gLoopPerf.maxUs / 1000UL;
      gLoopPerf.snapStalls     = gLoopPerf.stallsWindow;
      for (uint8_t i = 0; i < 6; i++) {
        gLoopPerf.snapBuckets[i]      = gLoopPerf.buckets[i];
        gLoopPerf.snapSectionAvgUs[i] = (uint32_t)(gLoopPerf.sectionSumUs[i] / gLoopPerf.laps);
      }
      if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
        DEBUG_PERFORMANCEF("[LOOPHEALTH] %lu laps/s | period min/avg/max=%lu/%lu/%lu ms | stalls=%lu | dist ms <16:%lu <32:%lu <64:%lu <128:%lu <256:%lu >=256:%lu",
                           (unsigned long)gLoopPerf.snapLapsPerSec,
                           (unsigned long)gLoopPerf.snapMinMs, (unsigned long)gLoopPerf.snapAvgMs, (unsigned long)gLoopPerf.snapMaxMs,
                           (unsigned long)gLoopPerf.snapStalls,
                           (unsigned long)gLoopPerf.snapBuckets[0], (unsigned long)gLoopPerf.snapBuckets[1], (unsigned long)gLoopPerf.snapBuckets[2],
                           (unsigned long)gLoopPerf.snapBuckets[3], (unsigned long)gLoopPerf.snapBuckets[4], (unsigned long)gLoopPerf.snapBuckets[5]);
        DEBUG_PERFORMANCEF("[LOOPHEALTH] section avg us/lap: DIAG=%lu IO=%lu EVENT=%lu NET=%lu DISPLAY=%lu INPUT=%lu",
                           (unsigned long)gLoopPerf.snapSectionAvgUs[0], (unsigned long)gLoopPerf.snapSectionAvgUs[1],
                           (unsigned long)gLoopPerf.snapSectionAvgUs[2], (unsigned long)gLoopPerf.snapSectionAvgUs[3],
                           (unsigned long)gLoopPerf.snapSectionAvgUs[4], (unsigned long)gLoopPerf.snapSectionAvgUs[5]);
      }
    }
    gLoopPerf.lastReportMs = nowMs;
    gLoopPerf.laps = 0; gLoopPerf.sumUs = 0; gLoopPerf.minUs = 0; gLoopPerf.maxUs = 0; gLoopPerf.stallsWindow = 0;
    memset(gLoopPerf.buckets, 0, sizeof(gLoopPerf.buckets));
    memset(gLoopPerf.sectionSumUs, 0, sizeof(gLoopPerf.sectionSumUs));
  }
}

// Dump the loop-health snapshot for the `perftop` command (prints via
// broadcastOutput). Declared in System_TaskUtils.h.
void perfPrintLoopHealth() {
  if (!gLoopPerf.snapValid) {
    broadcastOutput("[PERFTOP] loop: warming up (no completed 5s window yet)");
    return;
  }
  BROADCAST_PRINTF("[PERFTOP] loop: %lu laps/s | period min/avg/max = %lu/%lu/%lu ms | stalls(last 5s)=%lu",
                   (unsigned long)gLoopPerf.snapLapsPerSec,
                   (unsigned long)gLoopPerf.snapMinMs, (unsigned long)gLoopPerf.snapAvgMs, (unsigned long)gLoopPerf.snapMaxMs,
                   (unsigned long)gLoopPerf.snapStalls);
  BROADCAST_PRINTF("[PERFTOP] section avg us/lap: DIAG=%lu IO=%lu EVENT=%lu NET=%lu DISPLAY=%lu INPUT=%lu",
                   (unsigned long)gLoopPerf.snapSectionAvgUs[0], (unsigned long)gLoopPerf.snapSectionAvgUs[1],
                   (unsigned long)gLoopPerf.snapSectionAvgUs[2], (unsigned long)gLoopPerf.snapSectionAvgUs[3],
                   (unsigned long)gLoopPerf.snapSectionAvgUs[4], (unsigned long)gLoopPerf.snapSectionAvgUs[5]);
  if (gLoopPerf.totalStalls == 0) {
    BROADCAST_PRINTF("[PERFTOP] no loop stalls (>%lu ms) since boot", (unsigned long)(LOOP_STALL_US / 1000UL));
  } else {
    BROADCAST_PRINTF("[PERFTOP] worst loop stalls since boot (total %lu):", (unsigned long)gLoopPerf.totalStalls);
    unsigned long nowMs = millis();
    for (uint8_t i = 0; i < gLoopPerf.worstCount; i++) {
      const LoopPerfState::WorstStall& w = gLoopPerf.worst[i];
      BROADCAST_PRINTF("   %4lu ms  %lus ago  %s",
                       (unsigned long)w.ms,
                       (unsigned long)((nowMs - w.atMs) / 1000UL),
                       w.inLoop ? kLoopSectionNames[w.dom] : "outside sections (blocked/preempted)");
    }
  }
}

// Struct-read accessor for the loop-health snapshot — the OLED perf screen
// renders from this instead of parsing perfPrintLoopHealth's text. Returns
// false while the first 5 s window is still accumulating.
bool perfGetLoopSnapshot(uint32_t& lapsPerSec, uint32_t& avgMs, uint32_t& maxMs,
                         uint32_t& stalls5s, uint32_t& totalStalls) {
  if (!gLoopPerf.snapValid) return false;
  lapsPerSec  = gLoopPerf.snapLapsPerSec;
  avgMs       = gLoopPerf.snapAvgMs;
  maxMs       = gLoopPerf.snapMaxMs;
  stalls5s    = gLoopPerf.snapStalls;
  totalStalls = gLoopPerf.totalStalls;
  return true;
}

static String exitHelpAndExecute(const String& originalCmd) {
  String banner = exitToNormalBanner() + "\n";
  AuthContext ctx = currentAuthContext();
  ctx.path = "/help/exit";
  // Deliberately BELOW CMD_RESULT_MAX: this is a stack array, and internal
  // stack is the scarce resource here — 4 KB in this frame is not worth it for
  // the help-exit path. A result that doesn't fit now reports that explicitly
  // (executeCommand's ceiling check) instead of being silently halved.
  char out[2048];
  (void)executeCommand(ctx, originalCmd.c_str(), out, sizeof(out));
  banner += out;
  return banner;
}

extern int connectedDeviceCount;
extern struct ConnectedDevice connectedDevices[];

static bool gDebugMemSummary = false;

static void heapLogSummary(const char* tag) {
  // All four must carry MALLOC_CAP_8BIT. dram_largest was already fixed once
  // (MALLOC_CAP_8BIT alone matches PSRAM too, so it must be INTERNAL|8BIT), but
  // free/min/maxalloc were left on ESP.* which is MALLOC_CAP_INTERNAL with no
  // 8BIT — that sweeps in the IRAM-only heap and read ~25.8 KB high. See
  // hw1InternalFreeBytes() in System_MemUtil.h.
  size_t dram_free = hw1InternalFreeBytes();
  size_t dram_min = hw1InternalMinFreeBytes();
  // What a plain malloc()/new could actually get right now: stricter than
  // dram_largest because the DMA reserve pool carries no MALLOC_CAP_DEFAULT.
  size_t dram_maxalloc = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
  size_t dram_largest = hw1InternalLargestBlock();
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

extern void discoverI2CDevices();

#if ENABLE_AUTOMATION
const char* cmd_downloadautomation(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  broadcastOutput("Download automation from GitHub not yet implemented");
  return "ERROR";
}
const char* cmd_conditional(const String& argsInput) {
  return executeConditionalCommand(argsInput.c_str(), currentExecUser().c_str());
}
#endif

// RTC SLOW memory (.rtc_noinit -> rtc_slow_seg @ 0x50000000, 8 KB): survives
// soft reset / WDT / panic / deep sleep, but NOT power-off and NOT an RTC-domain
// reset. NOTE: the "7 KiB RTCRAM" line in the boot log is RTC *FAST* heap, a
// different pool these never touch.
RTC_NOINIT_ATTR static uint32_t rtcCrashCount;
RTC_NOINIT_ATTR static uint32_t rtcLastResetReason;
RTC_NOINIT_ATTR static uint32_t rtcMagic;
// Reboot reason stashed just before an intentional esp_restart(), consumed on the
// next boot to post SYSEVT_REBOOT. Guarded by its own magic because RTC_NOINIT is
// garbage on a cold power-on. Written via rebootStashReason() from the reboot helpers.
RTC_NOINIT_ATTR static char     rtcRebootReason[24];
RTC_NOINIT_ATTR static char     rtcRebootWho[24];       // who triggered it (username) — for the reboot event's attribution
RTC_NOINIT_ATTR static uint8_t  rtcRebootSource;        // NotificationSource of the actor
RTC_NOINIT_ATTR static uint32_t rtcRebootReasonMagic;
static const uint32_t REBOOT_REASON_MAGIC = 0x5245424F;  // 'REBO'

void rebootStashReason(const char* reason, const char* who, uint8_t source) {
  if (!reason) reason = "";
  strncpy(rtcRebootReason, reason, sizeof(rtcRebootReason) - 1);
  rtcRebootReason[sizeof(rtcRebootReason) - 1] = '\0';
  if (!who) who = "";
  strncpy(rtcRebootWho, who, sizeof(rtcRebootWho) - 1);
  rtcRebootWho[sizeof(rtcRebootWho) - 1] = '\0';
  rtcRebootSource = source;
  rtcRebootReasonMagic = REBOOT_REASON_MAGIC;
}
#define RTC_CRASH_MAGIC 0xC0FFEE42u

static const char* resetReasonName(uint32_t r) {
  switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";         // reset over the USB-Serial-JTAG peripheral (flash / replug)
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    case ESP_RST_UNKNOWN:   return "unknown";
    default:                return "other";
  }
}

void hardwareone_setup() {

  // ========================================================================
  // 1. PRE-SERIAL — crash tracking from RTC memory (no Serial yet)
  // ========================================================================
  {
    esp_reset_reason_t reason = esp_reset_reason();
    if (rtcMagic != RTC_CRASH_MAGIC) {
      // First ever cold boot — initialise RTC counters
      rtcCrashCount = 0;
      rtcMagic = RTC_CRASH_MAGIC;
    } else if (reason == ESP_RST_POWERON) {
      // Clean power cycle — reset accumulated crash count
      rtcCrashCount = 0;
    } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
               reason == ESP_RST_PANIC   || reason == ESP_RST_BROWNOUT) {
      // Keep ESP_RST_TASK_WDT: although CONFIG_ESP_TASK_WDT_PANIC is unset (so a
      // TWDT trip only prints and keeps running), the TWDT hardware still arms
      // STAGE1 = RESET_SYSTEM at 2x the timeout. That stage is dodged only
      // because the ISR feeds the timer — if interrupts are starved for 10 s the
      // reset is real, and it is the highest-signal reason in this list.
      rtcCrashCount++;
    }
    rtcLastResetReason = (uint32_t)reason;

    // First typed lifecycle edge. The event register is static and usable this
    // early, before the filesystem, settings, queues, or worker tasks exist.
    // gBootCounter is deliberately absent: its NVS increment happens later
    // inside initFilesystem(), so stamping it here would report a false #0.
    systemEventPost(SYSEVT_BOOT_STARTED, resetReasonName(rtcLastResetReason));

    // Decode anything __wrap_esp_panic_handler left in RTC, latch the previous
    // boot's phase before this boot overwrites it, and maintain the CONSECUTIVE
    // crash counter (rtcCrashCount above is cumulative-since-poweron, which is a
    // different question). Pure RTC/RAM work — no I/O, safe this early.
    crashRecordBootConsume((uint32_t)reason);

    // If an accepted trial image repeatedly dies during setup, persist the
    // rollback result before selecting the immutable factory recovery image.
    // No-op for ordinary partition layouts and healthy boots.
    otaSystemCrashLoopEscapeEarly();

    // Arm the panic-time capture as early as possible — a crash before this
    // point leaves no detail, so every line of setup after it is covered.
    crashRecordInstallPanicHook();

    // Render over esp_rom_printf while we are still pre-Serial. This is the only
    // path that survives a setup()-phase boot loop: if a crash happens before
    // setup() returns, the device never reaches loop() and therefore never
    // reaches any normal log sink, so every other render point would print
    // nothing on every iteration, forever.
    crashRecordEmitEarly();
  }

  // ========================================================================
  // 2. SERIAL + FILESYSTEM + SETTINGS
  // ========================================================================
  crashRecordSetPhase(CRASH_PHASE_FS_SETTINGS);
  Serial.begin(115200);
  delay(500);  // Longer delay for serial connection

  // Filesystem and settings code below already uses FsLockGuard. Create the
  // global mutexes before the first such use so those guards never silently
  // degrade to no-ops, even if boot sequencing gains another task later.
  initMutexes();

  // Enable allocation tracking BEFORE any allocations
  gAllocTrackerEnabled = true;
  gAllocTrackerCount = 0;
  memset(gAllocTracker, 0, sizeof(gAllocTracker));

  // Filesystem FIRST to enable early allocation logging
  if (!initFilesystem()) {
    Serial.println("FATAL: Filesystem initialization failed");
    // Never auto-format retained data. On the OTA layout, a committed main
    // image that cannot mount LittleFS parks in the factory updater so the
    // operator can recover over serial/authenticated SoftAP.
    (void)otaSystemRecoverFromStorageFailure();
    while (1) delay(1000);
  }
#if DEBUG_MEM_SUMMARY
  heapLogSummary("boot.after_fs");
#endif

  // Detect FTS state early so OLED shows correct message from the first frame
  detectFirstTimeSetupState();

#if ENABLE_WIFI
  // WiFi networks array must exist before readSettingsJson deserializes into it
  if (!gWifiNetworks) {
    gWifiNetworks = (WifiNetwork*)ps_alloc(MAX_WIFI_NETWORKS * sizeof(WifiNetwork), AllocPref::PreferPSRAM, "wifi.networks");
    if (!gWifiNetworks) {
      Serial.println("FATAL: Failed to allocate WiFi networks array");
      while (1) delay(1000);
    }
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
      new (&gWifiNetworks[i]) WifiNetwork();
    }
  }
#endif

  // Settings: defaults → load from file (or write defaults) → command system
  settingsDefaults();

  bool haveSettings = false;
  if (filesystemReady) {
    FsLockGuard guard("settings.exists");
    haveSettings = VFS::existsGuarded(SETTINGS_JSON_FILE, VFS::systemAuth("hwone.settings_load_check"));
  }

  if (filesystemReady && haveSettings) {
    if (readSettingsJson()) {
      Serial.println("[Settings] Loaded from file");
      // Loading is complete, so the stale firmwareVersion can be corrected.
      // This rewrites one key of the parsed file - it does NOT serialise RAM,
      // which is why it is safe here and why writeSettingsJson() would not be.
      // Without it the "settings carried over from prior firmware" event
      // re-fires on every boot after an update rather than once.
      (void)settingsRestampFirmwareVersion();
    } else {
      // Parse/open failure: the file is left on disk; RAM runs on defaults.
      logSystemEvent("SETTINGS", "settings.json load FAILED — running on defaults (file left on disk)");
    }
  } else if (filesystemReady) {
    Serial.println("[Settings] No file found, writing defaults");
    logSystemEvent("SETTINGS", "no settings.json — writing defaults");
    settingsMarkLoadedOk();  // nothing on disk to protect — RAM is the truth
    writeSettingsJson();
  }

  // Debug flags persist in their own DEBUG_JSON_FILE (split out of settings.json).
  // Load them into the gSettings.debug* shadow bools BEFORE applySettings() builds
  // the runtime gDebugFlags mask below. Seed the file if absent (fresh device, or a
  // first boot after upgrading onto a device whose settings.json predates the split).
  if (filesystemReady) {
    readDebugJson();
    if (!VFS::existsGuarded(DEBUG_JSON_FILE, VFS::systemAuth("hwone.debug_seed_check"))) {
      writeDebugJson();
    }
  }

  // Push the configured offset into libc TZ the moment settings are readable,
  // NOT at applySettings() below. Everything between here and there that
  // formats a wall-clock time calls localtime_r, and until tzset() runs libc
  // is in UTC — so those stamps silently came out UTC-shifted while every log
  // written after applySettings() was correct. That split-brain is visible in
  // the shipped logs: a v0.99.5 crash is recorded as 21:29:28 in
  // crash-history.log and 16:29:29 in system-events.log — same event, same
  // "+1262ms" marker, five hours apart, because crashRecordPersistToFile()
  // runs ~60 lines below and applySettings() runs ~60 lines below THAT.
  // (system-events escapes it only because its pre-init events are buffered
  // and stamped at flush.) applyTimezone is just setenv+tzset off
  // gSettings.tzOffsetMinutes — idempotent, no I/O — so the later call in
  // applySettings() stays exactly as it is.
  Clock::applyTimezone();

  // TEMP DEBUG (2026-04-03): force debug flags on AFTER file load to diagnose
  // Command system init — single call after settings are resolved
  // NOTE: applySettings() deferred until after initDebugSystem() so debug queue exists
  initializeCommandSystem();

  // Reconcile the durable OTA transaction once NVS, LittleFS, events, and the
  // command system are usable. This also reports any result left by recovery.
  otaSystemInitAfterStorage();

  // Reflect the RTC-derived crash counters into the RAM mirror for status/UI
  // reads. Plain assignment — NOT setSetting — so this no longer triggers a
  // settings.json write on every reset/reflash. That write, firing after a
  // failed load (when RAM is all defaults), was the trigger that could
  // overwrite settings.json with defaults. crashCount is RTC-backed and resets
  // on clean power-on by design, so it needs no file/NVS persistence of its own.
  gSettings.crashCount = rtcCrashCount;
  gSettings.lastResetReason = rtcLastResetReason;

  // Decode an intentional reboot from the RTC stash (set just before the restart)
  // so the boot record self-explains it. Only ESP_RST_SW is a deliberate
  // esp_restart(); a crash/watchdog/brownout/power-loss has a different reset
  // reason and no stash, so it can't masquerade as a reboot.
  bool swReset   = ((esp_reset_reason_t)rtcLastResetReason == ESP_RST_SW);
  bool haveStash = (rtcRebootReasonMagic == REBOOT_REASON_MAGIC);
  char rebootDetail[80] = "";   // e.g. "reboot: command by web:red" (empty unless a stashed reboot)
  if (swReset && haveStash) {
    const char* why  = rtcRebootReason[0] ? rtcRebootReason : "software";
    const char* rsrc = systemEventSourceName(rtcRebootSource);
    if (rtcRebootWho[0]) snprintf(rebootDetail, sizeof(rebootDetail), "reboot: %s by %s:%s", why, rsrc, rtcRebootWho);
    else                 snprintf(rebootDetail, sizeof(rebootDetail), "reboot: %s", why);
  }

  logSystemEvent("BOOT", "boot #%lu | reset=%s(%lu)%s%s | crashCount=%lu | fw v%s",
                 (unsigned long)gBootCounter, resetReasonName(rtcLastResetReason),
                 (unsigned long)rtcLastResetReason,
                 rebootDetail[0] ? " | " : "", rebootDetail,
                 (unsigned long)rtcCrashCount, SelfDevice::firmwareVersion());

  // Typed bus event for the intentional reboot — posted here on the next boot
  // because the in-RAM event ring can't survive the restart. Fires on any
  // software reset; the stash enriches reason/actor when the reboot went through
  // rebootDevice()/recordRebootIntent(). (Drains to events.log + automations once
  // loop() starts; boot posts far fewer than the ring's 48 slots before then.)
  if (swReset) {
    const char* why = (haveStash && rtcRebootReason[0]) ? rtcRebootReason : "software";
    uint8_t  src    = haveStash ? rtcRebootSource : 0xFF;                        // stashed actor source, or TLS default
    const char* who = (haveStash && rtcRebootWho[0]) ? rtcRebootWho : nullptr;   // stashed actor username
    systemEventPost(SYSEVT_REBOOT, why, resetReasonName(rtcLastResetReason), src, who);
  }

  // Unexpected-reset (crash) event — a fault reset (panic/watchdog/brownout/lockup/
  // glitch) is posted on the next boot as a NOTIFICATION so a crash is visible to
  // automations + the event log. SW resets are commanded reboots, handled above.
  {
    esp_reset_reason_t rr = (esp_reset_reason_t)rtcLastResetReason;
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_WDT   || rr == ESP_RST_BROWNOUT || rr == ESP_RST_CPU_LOCKUP ||
        rr == ESP_RST_PWR_GLITCH) {
      // Carry the panic detail (abort/assert text, faulting core + PC, the boot
      // phase it died in) into the event, not just the reset reason — "panic" on
      // its own says a fault happened, not what faulted. Bounded well under the
      // pre-init event path's ~165-char silent truncation limit.
      char cd[150];
      size_t w = (size_t)snprintf(cd, sizeof(cd), "boot #%lu crashCount=%lu",
                                  (unsigned long)gBootCounter, (unsigned long)rtcCrashCount);
      if (w < sizeof(cd)) {
        char det[110];
        if (crashRecordSummary(det, sizeof(det)) > 0) {
          snprintf(cd + w, sizeof(cd) - w, " | %s", det);
        }
      }
      systemEventPost(SYSEVT_CRASH, resetReasonName(rtcLastResetReason), cd);
      // Durable copy of the full record (assert text, core/PC, backtrace, build
      // sha). The RTC copy was invalidated at consume and now exists only in
      // this boot's RAM — one more reboot/reflash and the backtrace is gone.
      // Written here, at boot with the FS up, because panic context can't
      // safely touch flash.
      crashRecordPersistToFile(resetReasonName(rtcLastResetReason),
                               (uint32_t)rtcLastResetReason,
                               (unsigned long)gBootCounter);
    }
  }
  rtcRebootReasonMagic = 0;   // consume — a later spontaneous SW reset won't reuse a stale reason
  rtcRebootReason[0] = '\0';
  rtcRebootWho[0] = '\0';

  // Consume the ramflush overlay alongside the reason stash, so both RTC records
  // are invalidated at one point. Must precede every apply site — the earliest is
  // the automation init below — because it invalidates before applying, so a panic
  // while restoring can't replay the same state into a boot loop.
  ramFlushConsumeOverlay();

  // cameraAutoStart is not stable across this boot: a failed initCamera() clears it
  // (System_Camera_DVP.cpp:941) during the very replay that reads it. Snapshot the
  // intent now, before any replay runs, so the camera's resolve can't be swayed by
  // a write the replay itself caused.
  [[maybe_unused]] const bool snapCameraAutoStart = gSettings.cameraAutoStart;

  // Unconditional per-boot orientation divider into the login/i2c/error logs so
  // every log is attributable to a boot even when NTP never syncs (offline) — now
  // carrying the reboot detail so those sparse logs self-explain a commanded restart.
  // The "clock now accurate" line is still added later by logTimeSyncedMarkerIfReady().
  logBootAnchorToLogs(resetReasonName(rtcLastResetReason), rebootDetail[0] ? rebootDetail : nullptr);

  // If time is already valid (warm soft-reboot carried it in the RTC
  // domain), resolve user creation times early. This is the one case the
  // clockStepped chokepoint can't cover — no settimeofday happens on a
  // carryover, so nothing pends the resolve. The old gate here was
  // `time(nullptr) > 0`, which is an uptime test (true ~1 s after ANY
  // boot), so this ran a pointless users.json read on every dark boot.
  if (Clock::isSynced()) {
    resolvePendingUserCreationTimes();
  }

  // Generate unique boot ID for session versioning
  uint64_t chipId = ESP.getEfuseMac();
  gBootId = String((uint32_t)(chipId >> 32), HEX) + String((uint32_t)chipId, HEX) + "_" + String(millis());

  broadcastOutput("[build] Firmware: reg-json-debug-1");

  // ========================================================================
  // 3. MUTEXES + DEBUG SYSTEM + BUFFERS
  // ========================================================================
  crashRecordSetPhase(CRASH_PHASE_DEBUG_BUF);

  // Sensor cache mutexes are now created lazily in each *StartInternal() function
  // This saves memory for disabled sensors and allows better error handling

  if (gSettings.i2cEnabled) {
    initSensorQueue();
  }

  // Debug system must be up before applySettings() (debug queue/task needed for flag writes)
  initDebugSystem();
  applySettings();
  // Notification device policy (per-kind levels) — name-keyed JSON, loaded
  // after the filesystem + settings are up, before render tasks start.
  notifPolicyLoad();
  heapLogSummary("boot.after_debugbuf");

#if ENABLE_AUTOMATION
  // Both axes: automationEnabled is permission, automationAutoStart is boot
  // intent. The scheduler can still be brought up later with
  // `automation system enable`, which sets gAutomationSchedulerRunning itself.
  if (gSettings.automationEnabled && ramFlushResolve(RF_AUTOMATION, gSettings.automationAutoStart)) {
    if (!initAutomationSystem()) {
      ERROR_SYSTEMF("FATAL: Failed to initialize automation system");
      while (1) delay(1000);
    }
    DEBUG_SYSTEMF("Automation system initialized at boot");
  } else if (!gSettings.automationEnabled) {
    DEBUG_SYSTEMF("Automation system disabled - skipping initialization");
  } else {
    DEBUG_SYSTEMF("Automation autostart off - scheduler idle until 'automation system enable'");
  }
#endif

  // Command executor task (mutexes + debug system must be ready)
    if (!gCmdExecQ) {
      // Depth 8, not 6: every inbound BLE secure-channel frame takes a slot via
      // submitDeferredToCmdExec(), whose enqueue DROPS on full rather than
      // blocking — so a burst of OTA staging chunks can starve the `otawrite
      // status` checkpoint that rides the same queue, and a lost checkpoint
      // fails the whole transfer. The queue holds pointers (4 B/slot), so the
      // widening itself costs 8 bytes of internal DRAM; the real cost is up to
      // two more in-flight ExecReq (~6.3 KB each) which ps_alloc takes from
      // PSRAM, where there is multiple MB spare.
      gCmdExecQ = xQueueCreate(8, sizeof(ExecReq*));
      if (!gCmdExecQ) {
        ERROR_SYSTEMF("FATAL: Failed to create command exec queue");
        while (1) delay(1000);
      }
    const uint32_t cmdExecStackWords = CMD_EXEC_STACK_WORDS;  // BYTES (8192 = 8 KB; the old "≈24 KB" was the 4x myth) - automation run + debug vsnprintf frames need deep stack
    // Pin to core 0 (PRO_CPU), alongside the BLE host (BTC_TASK) and web server.
    // The on-device LLM task (llm_gen) is pinned to core 1 at a higher priority; if
    // cmd_exec is left unpinned it can sit on core 1 and be preempted by a 15 s
    // generation, so BLE/serial command replies (e.g. the llmgenerate {session})
    // arrive seconds late and IDLE1 starves → task-WDT. Keeping command I/O on core 0
    // gives the BLE path the same isolation the web path already enjoys.
    if (xTaskCreateLogged(commandExecTask, "cmd_exec_task", cmdExecStackWords, nullptr, TASK_PRIORITY_LOW, &gCmdExecTaskHandle, "cmd.exec", 0) != pdPASS) {
      ERROR_SYSTEMF("FATAL: Failed to create command exec task");
      while (1) delay(1000);
    }
    DEBUG_SYSTEMF("Command executor task created");
#if DEBUG_MEM_SUMMARY
    heapLogSummary("boot.after_task.cmd_exec");
#endif
  }

  // ========================================================================
  // 4. HARDWARE INIT — battery, LED, I2C buses
  // ========================================================================
  crashRecordSetPhase(CRASH_PHASE_HARDWARE);
  initBattery();
  boardPowerRailInit();  // Must precede I2C — powers STEMMA QT connector on Feather V2 (independent of ENABLE_NEOPIXEL)
  initNeoPixelLED();     // no-op stub when ENABLE_NEOPIXEL=0

#if ENABLE_I2C_SYSTEM
  initI2CBuses();
  esp_log_level_set("i2c.master", ESP_LOG_WARN);  // Suppress routine NACK spam from FM radio RDS polling

  // Early silent RTC sync (zero-alloc I2C read + settimeofday) — verbose sync happens later
  #if ENABLE_RTC_SENSOR
  rtcEarlyBootSync();
  #endif
#endif

  // ========================================================================
  // 5. BUILD CONFIG BANNER + OLED EARLY INIT
  // ========================================================================
  crashRecordSetPhase(CRASH_PHASE_BANNER_OLED);
  {
    char bannerLine[96];
    broadcastOutput("");
    snprintf(bannerLine, sizeof(bannerLine), "========== HARDWAREONE v%s BUILD CONFIGURATION ==========", SelfDevice::firmwareVersion());
    broadcastOutput(bannerLine);
#if ENABLE_THERMAL_SENSOR
    broadcastOutput("  [Y] THERMAL  | MLX90640 thermal camera");
#else
    broadcastOutput("  [N] THERMAL  | Disabled (~20-25KB flash, ~15KB RAM saved)");
#endif
#if ENABLE_TOF_SENSOR
    broadcastOutput("  [Y] TOF      | VL53L4CX distance sensor");
#else
    broadcastOutput("  [N] TOF      | Disabled (~25-30KB flash, ~10KB RAM saved)");
#endif
#if ENABLE_IMU_SENSOR
    broadcastOutput("  [Y] IMU      | BNO055 9-DOF orientation sensor");
#else
    broadcastOutput("  [N] IMU      | Disabled (~12-18KB flash, ~8KB RAM saved)");
#endif
#if ENABLE_GAMEPAD_SENSOR
    broadcastOutput("  [Y] GAMEPAD  | Seesaw gamepad controller");
#else
    broadcastOutput("  [N] GAMEPAD  | Disabled (~8-12KB flash, ~6KB RAM saved)");
#endif
#if ENABLE_APDS_SENSOR
    broadcastOutput("  [Y] APDS     | APDS9960 color/proximity/gesture");
#else
    broadcastOutput("  [N] APDS     | Disabled (~6-10KB flash, ~4KB RAM saved)");
#endif
#if ENABLE_GPS_SENSOR
    broadcastOutput("  [Y] GPS      | PA1010D mini GPS module");
#else
    broadcastOutput("  [N] GPS      | Disabled (~5-8KB flash, ~4KB RAM saved)");
#endif
#if ENABLE_RTC_SENSOR
    broadcastOutput("  [Y] RTC      | DS3231 precision real-time clock");
#else
    broadcastOutput("  [N] RTC      | Disabled (~3-5KB flash, ~1KB RAM saved)");
#endif
#if ENABLE_FM_RADIO
    broadcastOutput("  [Y] FM RADIO | RDA5807 FM radio receiver");
#else
    broadcastOutput("  [N] FM RADIO | Disabled (~3-5KB flash, ~1KB RAM saved)");
#endif
#if ENABLE_PRESENCE_SENSOR
    broadcastOutput("  [Y] PRESENCE | STHS34PF80 IR presence/motion sensor");
#else
    broadcastOutput("  [N] PRESENCE | Disabled (~4-6KB flash, ~2KB RAM saved)");
#endif
#if ENABLE_OLED_DISPLAY
    broadcastOutput("  [Y] OLED     | SSD1306 128x64 display enabled");
#else
    broadcastOutput("  [N] OLED     | Disabled (~8-12KB flash, ~5KB RAM saved)");
#endif
    broadcastOutput("========================================================");
    broadcastOutput("");
  }

  // OLED early init — boot animation runs during slow WiFi/NTP phases below
  oledEarlyInit();

#if ENABLE_I2C_SYSTEM
  if (gSettings.i2cEnabled && !queueProcessorTask) {
    const uint32_t queueStackWords = SENSOR_QUEUE_STACK_WORDS;
    // Pin to Core 1 (I2C_SENSOR_CORE): this task runs the I2C device-init
    // transactions, so it carries the same starve-mid-transaction → bus-storm →
    // panic(4) hazard as the sensor pollers. Must not float onto Wi-Fi-saturated Core 0.
    if (xTaskCreateLogged(sensorQueueProcessorTask, "sensor_queue_task", queueStackWords, nullptr, TASK_PRIORITY_LOW, &queueProcessorTask, "sensor.queue", I2C_SENSOR_CORE) != pdPASS) {
      ERROR_SYSTEMF("FATAL: Failed to create sensor queue processor task");
      while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
#if DEBUG_MEM_SUMMARY
    heapLogSummary("boot.after_task.sensor_queue");
#endif
  }
#endif

#if ENABLE_HTTP_SERVER
    if (!gSessions) {
      gSessions = (SessionEntry*)ps_alloc(MAX_SESSIONS * sizeof(SessionEntry), AllocPref::PreferPSRAM, "sessions");
      if (!gSessions) {
        ERROR_SYSTEMF("FATAL: Failed to allocate sessions array");
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
        ERROR_SYSTEMF("FATAL: Failed to allocate logout reasons array");
        while (1) delay(1000);
      }
    // Initialize with placement new to call constructors
    for (int i = 0; i < MAX_LOGOUT_REASONS; i++) {
      new (&gLogoutReasons[i]) LogoutReason();
    }
  }
#endif
  // ========================================================================
  // 6. FIRST-TIME SETUP + CREDENTIALS
  // ========================================================================
  crashRecordSetPhase(CRASH_PHASE_FIRST_TIME);
  broadcastOutput("");
  broadcastOutput("Booting ESP32 Minimal Auth");
  if (gFirstTimeSetupState == SETUP_NOT_NEEDED) {
    oledSetBootProgress(10, "Checking setup");
  } else {
    oledUpdate();  // Force OLED to show first-time setup prompt before blocking
    
#if ENABLE_OLED_DISPLAY && ENABLE_OLED_INPUT
    // Start the input device (gamepad or ANO encoder) before first-time setup
    // so the OLED keyboard can receive input.
    if (oledConnected && gOledRunning) {
      DEBUG_SYSTEMF("[Boot] Starting input device for OLED first-time setup");
      bool ok = inputStartInternal();  // Properly initializes hardware and creates task
      DEBUG_INPUTF("[Boot] Input device init result: %s", ok ? "SUCCESS" : "FAILED");
      delay(100);  // Give input task time to start polling
    }
#endif
  }
  // firstTimeSetupIfNeeded() BLOCKS here waiting for a human to type. An
  // unverified OTA image cannot survive that: the loop heartbeat lives in
  // hardwareone_loop() and otaSafetySetupReachedRunning() is further down this
  // function, so the probation supervisor's 5-minute setup deadline fires on an
  // operator who merely reads the menu slowly, rolls the image back, and marks
  // it ABORTED - which then needs a full re-provision to clear.
  //
  // Reaching this point is better evidence than the probation it replaces, and
  // filesystemReady is the load-bearing part of the condition: a broken image
  // that failed to mount storage ALSO reports "setup required", and that case
  // must keep its probation rather than being waved through.
  if (filesystemReady && isFirstTimeSetup()) {
    (void)otaSafetyAcceptProvisioningBoot();
  }
  firstTimeSetupIfNeeded();
  oledUpdate();  // Update OLED animation during boot

  // UART host link (CM5 command channel) — starts only if uartLinkEnabled.
  // Deliberately after firstTimeSetupIfNeeded(): the link never runs on an
  // unprovisioned device (no users.json → nothing to authenticate against).
  uartLinkInitFromSettings();
#if ENABLE_RASPBERRY_PI_HOST_POWER
  cm5HostPowerInit();
#endif
#if ENABLE_RASPBERRY_PI_HOST_FAN
  cm5HostFanInit();
#endif

  // (Removed: legacy Basic-Auth gAuthUser/gAuthPass priming via
  // loadUsersFromFile. The function read a passwordHash field that has
  // long been gone from users.json, so this sequence was a no-op that
  // left the literal admin/admin defaults in place. decodeBasicAuth now
  // always parses the actual Authorization header and lets isValidUser
  // decide. See AUTH_ASSESSMENT_REPORT.md §4 #6.)

  // RTC early boot sync - only if RTC time has been previously set
  // If rtcTimeHasBeenSet is false, we'll prioritize NTP at boot to get accurate time first
#if ENABLE_RTC_SENSOR
  if (gSettings.rtcTimeHasBeenSet) {
    oledSetBootProgress(28, "Syncing RTC");
    if (rtcEarlyBootSync()) {
      broadcastOutput("[Boot] System time set from RTC (previously calibrated)");
    }
  } else {
    oledSetBootProgress(28, "Skipping RTC");
    broadcastOutput("[Boot] RTC time not yet set - will sync from NTP if available");
  }
#endif

  // ========================================================================
  // 7. NETWORK — WiFi + NTP
  // ========================================================================
  crashRecordSetPhase(CRASH_PHASE_NETWORK);
#if ENABLE_WIFI
  oledSetBootProgress(30, "Connecting WiFi");

  bool wifiConnected = false;
  // Always attempt WiFi connection if credentials exist (controlled by wifiAutoStart setting)
  if (gSettings.wifiEnabled && ramFlushResolve(RF_WIFI, gSettings.wifiAutoStart)) {  // Controlled by first-time setup or settings
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
  oledSetBootProgress(40, wifiConnected ? "WiFi connected" : "Skipping WiFi");

  // NTP sync phase - only needed if RTC didn't already provide valid time
  // RTC is the primary time source; NTP is secondary (for initial calibration or manual refresh)
  if (wifiConnected) {
    bool rtcProvidedTime = false;
#if ENABLE_RTC_SENSOR
    // Check if RTC already set valid system time during early boot.
    // getLocalTime(&t, 10) blocks up to 10ms for the time to become valid
    // and fills timeinfo on success. Validate against the same year>=2020
    // threshold Clock::isSynced uses, but read the JUST-populated tm_year
    // directly — calling Clock::isSynced here would do a fresh time()/
    // localtime_r and could race against the 10ms-window state we just
    // sampled.
    if (gSettings.rtcTimeHasBeenSet) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10) && timeinfo.tm_year >= 120) {
        rtcProvidedTime = true;
      }
    }
#endif
    if (rtcProvidedTime) {
      // RTC already provided valid time - skip blocking NTP sync at boot
      // NTP can still be triggered manually via 'ntpsync' command
      oledSetBootProgress(50, "Time from RTC");
      DEBUG_NTPF("[DEBUG] Skipping NTP sync - RTC already provided valid time");
      // Still set up NTP for background sync (non-blocking)
      setupNTP();
    } else {
      oledSetBootProgress(45, "Syncing NTP");
      DEBUG_NTPF("[DEBUG] Starting NTP sync");
      NtpSyncOutcome ntpOutcome = NtpSyncOutcome::Failed;
      bool ntpOk = syncNTPAndResolve(&ntpOutcome);
      DEBUG_NTPF("%s", ntpOk ? "[DEBUG] NTP sync complete" : "[DEBUG] NTP sync failed");
      const char* ntpMsg = "Time unavailable";
      switch (ntpOutcome) {
        case NtpSyncOutcome::Reply:       ntpMsg = "Time synced"; break;
        case NtpSyncOutcome::KeptPrior:   ntpMsg = "Time kept, NTP pending"; break;
        case NtpSyncOutcome::RtcFallback: ntpMsg = "Time from RTC"; break;
        default: break;
      }
      oledSetBootProgress(50, ntpMsg);
    }
  } else {
    oledSetBootProgress(50, "Continuing offline");
  }
#else
  // WiFi disabled at compile time
  oledSetBootProgress(30, "Skipping WiFi");
  bool wifiConnected = false;
  oledSetBootProgress(50, "Continuing offline");
#endif

  // ========================================================================
  // 8. DEVICE DISCOVERY + SERVICE AUTO-START
  // ========================================================================
  crashRecordSetPhase(CRASH_PHASE_AUTOSTART);
  oledSetBootProgress(60, "Scanning devices");

  DEBUG_SYSTEMF("Starting device discovery");
  
  // Give slower I2C devices (GPS, FM Radio, Gamepad) extra time to initialize after power-on
  // Some sensors need 1-2 seconds to become responsive on I2C bus
  // Use loop instead of blocking delay to keep boot animation running
  for (int i = 0; i < 20; i++) {
    delay(80);
    oledUpdate();
  }
  
 #if ENABLE_I2C_SYSTEM
  discoverI2CDevices();
  DEBUG_SYSTEMF("Device discovery completed");
 #else
  DEBUG_SYSTEMF("I2C system disabled at compile time - skipping I2C device discovery");
 #endif

  oledSetBootProgress(80, "Starting services");

  // Apply OLED settings if display was initialized early
  oledApplySettings();

  // Bluetooth - auto-start if enabled in settings.
  // Two triggers can bring up BT at boot:
  //   1. bleAutoStart=true   → start whichever mode bleMode says
  //   2. Any registered peer wants auto-reconnect AND has a saved MAC
  //      → start client mode (even if (1) is off and bleMode=server)
  // Trigger 2 only ever starts the *client* role; if you want server mode
  // at boot, flip bleAutoStart explicitly. Boot reconnect itself is
  // delegated to BLE_Peers' bleBootReconnect() which iterates the peer
  // registry — adding a new peer doesn't require touching this block.
#if ENABLE_BLUETOOTH
  // Make sure built-in metadata-only peers (phone) are registered so
  // bleAnyPeerWantsAutoReconnect can see them. Real peer modules register
  // themselves from initG2Client / g2RingInit further down.
  bleRegisterBuiltinPeers();

#if ENABLE_G2_GLASSES
  const bool wantClientForAutoReconnect = bleAnyPeerWantsAutoReconnect();
#else
  const bool wantClientForAutoReconnect = false;
#endif

  // The *effective* mode for boot. If a peer wants auto-reconnect we
  // coerce to client mode so the BLE stack comes up the right way.
  const int bootBleMode = wantClientForAutoReconnect
      ? (int)BLE_MODE_G2_CLIENT
      : gSettings.bleMode;

  // Substitute only the setting read, not the whole condition: a G2/ring peer
  // brings the radio up in client mode independently of bleAutoStart.
  // NOTE the parentheses: bleEnabled gates BOTH reasons to come up. Without
  // them this reads `(enabled && autostart) || wantClient`, so a peer set to
  // auto-reconnect would start the radio on a device where Bluetooth is
  // explicitly disabled. Disabled has to mean disabled.
  if (gSettings.bleEnabled &&
      (ramFlushResolve(RF_BLUETOOTH, gSettings.bleAutoStart) || wantClientForAutoReconnect)) {
    oledSetBootProgress(85, "Starting Bluetooth");

    // Pause sensor polling during BLE init to avoid interrupt contention
    pollPause();
    vTaskDelay(pdMS_TO_TICKS(50));  // Let pending I2C ops complete

#if ENABLE_G2_GLASSES
    if (bootBleMode == BLE_MODE_G2_CLIENT) {
      // initG2Client also registers the G2 peer. If wantClientForAutoReconnect
      // is true, bleBootReconnect will iterate the registry and kick off
      // saved-MAC connects (with paced 2s gaps). The boot block is now
      // peer-agnostic — adding a peer means adding a registry entry, not
      // touching this code.
      if (initG2Client()) {
        broadcastOutput("G2 client initialized (run 'openg2' to connect)");
        if (wantClientForAutoReconnect) {
          // Give the BLE stack ~2 s to settle before kicking off scans.
          vTaskDelay(pdMS_TO_TICKS(2000));
          broadcastOutput("[BLE] Auto-reconnect: iterating peer registry");
          bleBootReconnect();
        }
      } else {
        broadcastOutput("G2 client initialization failed");
      }
    } else
#endif
    if (ramFlushResolve(RF_BLUETOOTH, gSettings.bleAutoStart)) {
      // Server-mode path only runs when the user *explicitly* asked for BT
      // at boot. We never coerce to server from auto-reconnect flags.
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
    }

    pollResume();
  } else {
    broadcastOutput("Bluetooth disabled by default. Use quick settings (SELECT button) or 'openble' to enable.");
  }
#endif

  // Sensor auto-start - process settings for all I2C sensors
  oledSetBootProgress(87, "Starting sensors");
#if ENABLE_I2C_SYSTEM
  processAutoStartSensors();
#endif

#if ENABLE_CAMERA_SENSOR
  // Camera auto-start (independent of I2C sensor queue)
  if (gSettings.cameraEnabled && ramFlushResolve(RF_CAMERA, snapCameraAutoStart)) {
    runUnifiedSystemCommand("opencamera");
  }
#endif

#if ENABLE_MICROPHONE
  // Microphone / ESP-SR auto-start. openmic self-guards on availability
  // (initMicrophone refuses when no source is present), so on a PDM-less board
  // with the glasses not yet connected this is a graceful no-op; the wearer can
  // start the mic once the glasses are up (or leave micautostart off).
  // If ESP-SR is enabled, it takes over the microphone - don't start mic separately
  #if ENABLE_ESP_SR
  // Resolve both through this same if/else — SR takes the mic, so resolving them
  // independently would double-claim the I2S channel.
  if (gSettings.srEnabled && ramFlushResolve(RF_SR, gSettings.srAutoStart)) {
    broadcastOutput("Starting ESP-SR speech recognition...");
    runUnifiedSystemCommand("srstart");
  } else if (gSettings.micEnabled && ramFlushResolve(RF_MICROPHONE, gSettings.micAutoStart)) {
    broadcastOutput("Starting microphone sensor...");
    runUnifiedSystemCommand("openmic");
  }
  #else
  if (gSettings.micEnabled && ramFlushResolve(RF_MICROPHONE, gSettings.micAutoStart)) {
    broadcastOutput("Starting microphone sensor...");
    runUnifiedSystemCommand("openmic");
  }
  #endif
#endif

  // HTTP server - auto-start if enabled in settings and WiFi is connected
#if ENABLE_HTTP_SERVER
  oledSetBootProgress(90, "Starting HTTP");

  const bool httpWanted = gSettings.httpEnabled && ramFlushResolve(RF_HTTP, gSettings.httpAutoStart);
  if (httpWanted && WiFi.isConnected()) {
    runUnifiedSystemCommand("openhttp");
    BROADCAST_PRINTF("%s%s", gServerIsHttps ? "HTTPS server started. Try: https://" : "HTTP server started. Try: http://", WiFi.localIP().toString().c_str());
  } else if (!httpWanted) {
    broadcastOutput("HTTP server available. Use 'openhttp' or quick settings (SELECT button) to start.");
  } else {
    broadcastOutput("HTTP server not started (WiFi offline). Use quick settings (SELECT button) or 'openhttp' to start manually.");
  }
#else
  oledSetBootProgress(90, "Skipping HTTP");
#endif

  // On-device LLM - auto-load the default model at boot if enabled in settings.
  // Mirrors the sensor/SR auto-start pattern (gSettings.<x>AutoStart -> runtime
  // start command). Loading is heavy (PSRAM + time), so it's opt-in (default off).
#if ENABLE_LLM_SOURCE_ONBOARD
  if (gSettings.llmEnabled && ramFlushResolve(RF_LLM, gSettings.llmAutoStart)) {
    broadcastOutput("Auto-loading on-device LLM model...");
    String llmAutoCmd = "llmload " + gSettings.llmDefaultModel;
    runUnifiedSystemCommand(llmAutoCmd);
  }
#endif

  // MQTT client - auto-start if enabled in settings and WiFi is connected
#if ENABLE_MQTT
  // mqttEnabled stays a hard master gate — the overlay resolves the
  // autostart intent, it does not override whether MQTT is configured at all.
  if (gSettings.mqttEnabled && ramFlushResolve(RF_MQTT, gSettings.mqttAutoStart)) {
    oledSetBootProgress(92, "Starting MQTT");
    if (WiFi.isConnected()) {
      runUnifiedSystemCommand("openmqtt");
      broadcastOutput("[MQTT] Auto-start enabled, connecting to broker...");
    } else {
      broadcastOutput("[MQTT] Not started (WiFi offline). Use 'openmqtt' to start manually.");
    }
  } else {
    oledSetBootProgress(92, "Skipping MQTT");
  }
#else
  oledSetBootProgress(92, "Skipping MQTT");
#endif

  oledSetBootProgress(100, "Boot complete!");

  // Run LED startup effect if enabled (compiled only when the NeoPixel feature
  // is — the pin test alone would break user-override ENABLE_NEOPIXEL=0 builds
  // against the gated ledStartup* settings fields)
#if ENABLE_NEOPIXEL
  if (gSettings.ledStartupEnabled && gSettings.ledStartupEffect.length() > 0 && gSettings.ledStartupEffect != "none") {
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
    } else if (effect == "pulse") {
      runLEDEffect(EFFECT_PULSE, color1, color1, duration);
    } else if (effect == "fade") {
      runLEDEffect(EFFECT_FADE, color1, color2, duration);
    } else if (effect == "blink") {
      runLEDEffect(EFFECT_BLINK, color1, color1, duration);
    } else if (effect == "strobe") {
      runLEDEffect(EFFECT_STROBE, color1, color1, duration);
    } else if (effect == "solid") {
      setLEDColor(color1);
      delay(duration);
      setLEDColor({ 0, 0, 0 });
    }
    broadcastOutput("Startup effect completed: " + effect);
  }
#endif

#if ENABLE_AUTOMATION
  // Gate on the scheduler actually being up. Previously unconditional, so
  // BOOT-trigger automations fired even with automations switched off —
  // runAutomationsOnBoot() itself only checks a once-per-boot latch and the
  // filesystem, never the setting.
  if (gAutomationSchedulerRunning) {
    runAutomationsOnBoot();
  }
#endif

#if ENABLE_ESPNOW
  // Phase 3.0 — initialize libsodium and load (or first-boot-generate) the
  // long-term Ed25519 identity BEFORE ESPNOW init. This must happen on every
  // boot (even when ESPNOW init is gated off by settings) so the identity
  // file exists for later phases (3.3+) to consume and so the on-disk format
  // is exercised early. If identity init fails fatally, ESPNOW does not
  // initialize regardless of the user setting; the operator can recover via
  // the `espnowregenidentity --confirm-wipe-all-bonds` CLI.
  bool cryptoOk = espnowCryptoInit();
  if (!cryptoOk) {
    broadcastOutput("[ESP-NOW] FATAL: libsodium init failed — ESPNOW disabled this boot");
  }
  bool identityOk = false;
  if (cryptoOk) {
    EspNowIdentity bootIdentity = {};
    identityOk = espnowIdentityLoadOrGenerate(bootIdentity);
    if (!identityOk) {
      broadcastOutput("[ESP-NOW] FATAL: long-term identity load failed — ESPNOW disabled this boot");
      broadcastOutput("[ESP-NOW] Recovery: 'espnowregenidentity --confirm-wipe-all-bonds'");
    }
  }

  // Phase 3.1 — stretch each mesh's passphrase to PBKDF2 once (cached on disk),
  // then derive bootstrap + group subkeys into RAM. No on-wire effect yet; the
  // 3.3 KEY_EX handshake and 3.5 BROADCAST_AUTH path will consume these.
  if (identityOk) {
    // Walk meshes, stretch any that have a passphrase but no cached hash.
    // Persist once at the end if anything changed (avoids N writes).
    bool meshesDirty = false;
    for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
      Settings::MeshIdentity& m = gSettings.meshes[i];
      if (!m.enabled || m.label.length() == 0) continue;
      if (!m.passphraseStretchedKeyValid && m.passphrase.length() > 0) {
        if (meshKeysStretchPassphrase(i)) meshesDirty = true;
      }
    }
    if (meshesDirty && filesystemReady) {
      writeSettingsJson();
    }
    uint8_t derivedCount = meshKeysInitAll();
    (void)derivedCount;  // logged inside meshKeysInitAll on a per-mesh basis

    // Phase 3.2 — walk /system/espnow/peers/*/identity.json and populate the
    // in-memory peer identity cache. Empty on a fresh boot; populates once
    // KEY_EX (3.3) has paired peers. No-op + 0-line log if the directory
    // doesn't exist yet, which it won't until the first pair completes.
    peerIdentityLoadAll();

    // Phase 3.4 — allocate the in-RAM SessionState table. Sessions vanish
    // on reboot by design (forward secrecy); just need the slot table ready.
    sessionsInit();

    // Phase 4 — file-transfer slot table + boot-time cleanup of stale
    // .part files from any previous crashed-mid-transfer boot.
    fileSlotsInit();
    fileSlotsBootCleanup();

    // Phase 4b — remote-directory-listing protocol (FS_LIST_REQ/REPLY).
    // Allocates the pending-request table mutex; safe to call repeatedly.
    extern void fsListInit();
    fsListInit();
  }

  // espnowEnabled used to serve as BOTH the master switch and the boot flag.
  // It is now the master switch only; espnowAutoStart is the boot flag.
  if (gSettings.espnowEnabled &&
      ramFlushResolve(RF_ESPNOW, gSettings.espnowAutoStart) && identityOk) {
    broadcastOutput("[ESP-NOW] Auto-initialization enabled in settings");
    const char* setupError = checkEspNowFirstTimeSetup();
    if (setupError && strlen(setupError) > 0) {
      broadcastOutput("[ESP-NOW] Auto-init skipped - first-time setup required:");
      broadcastOutput(setupError);
      broadcastOutput("[ESP-NOW] Set device name with: espnow setname <name>");
    } else {
      broadcastOutput("[ESP-NOW] Initializing...");
      const char* result = cmd_espnow_init("");
      broadcastOutput(result);
#if DEBUG_MEM_SUMMARY
      heapLogSummary("boot.after_espnow_init");
#endif
      // NOTE: the espnow_tx dispatcher is started inside initEspNow() so it
      // comes up on every init path (boot + manual `openespnow`), not just here.
    }
  }

  // Boot notification is sent by initEspNow() itself (System_ESPNow.cpp),
  // which covers every init path — boot autostart AND manual `openespnow`.
  // A near-verbatim copy here used to send a SECOND notification with a
  // fresh message id on every autostart boot (mesh dedup couldn't collapse
  // them), so peers saw each boot twice.
#endif

  // ========================================================================
  // 9. BOOT-COMPLETE DIAGNOSTICS
  // ========================================================================
  crashRecordSetPhase(CRASH_PHASE_BOOT_DIAG);
  printCommandModuleSummary();
  printSettingsModuleSummary();
  printMemoryReport();
  // Gated at the call site: both helpers read their own autostart flag internally,
  // so the overlay has to decide here rather than inside them.
  if (gSettings.sensorLogEnabled && ramFlushResolve(RF_SENSORLOG, gSettings.sensorLogAutoStart)) sensorLogAutoStart();
  if (gSettings.systemLogEnabled && ramFlushResolve(RF_SYSTEMLOG, gSettings.systemLogAutoStart)) systemLogAutoStart();

  broadcastOutput("[Boot] Setup complete");

  // setup() survived — any crash from here on is a RUNTIME fault, not a boot
  // fault, which is the single most useful bit for triaging one.
  crashRecordSetPhase(CRASH_PHASE_RUNNING);
  sHardwareOneRunningSinceMs = millis();
  sHardwareOneRunning = true;
  otaSafetySetupReachedRunning();

  // Last lines of boot: nudge to provision the BLE passphrase if encryption is wanted
  // but unset (so the operator can't miss that Bluetooth is currently plaintext).
  bleSecurityBootNotice();

  // Final typed lifecycle edge. Keep this as the last action before setup()
  // returns so the pair brackets LED effects, boot automations, ESP-NOW setup,
  // diagnostics, and the transition to CRASH_PHASE_RUNNING. An intentional
  // restart also has the richer SYSEVT_REBOOT event posted earlier in setup.
  {
    char bootDetail[24];
    snprintf(bootDetail, sizeof(bootDetail), "boot #%lu", (unsigned long)gBootCounter);
    systemEventPost(SYSEVT_BOOT_FINISHED, resetReasonName(rtcLastResetReason), bootDetail);
  }
}


#if ENABLE_OLED_DISPLAY
// ---------------------------------------------------------------------------
// Power saving (Tier 1: display off + CPU downclock)
//
// After gSettings.powerSaveTimeoutMinutes with no activity, blank and (on
// power-gated boards like the FeatherS3) power down the OLED, stop refreshing
// it, and drop the CPU to the 80 MHz WiFi floor (restored on wake). The radio
// stays up, so HTTP/ESP-NOW keep working with the screen dark. "Activity" is
// source-agnostic: any input-device event (gamepad OR ANO) and any real
// user/peer command (serial, web, G2, ESP-NOW RCE, ...) — routed through
// powerSaveNoteActivity() — resets the idle timer and wakes it. So a headless
// box with no input device benefits too: a command wakes it. Held off only
// while genuinely busy with local capture (camera or mic). A bonded peer being
// online no longer holds it awake — real peer commands wake it via the hook.
// 0 minutes = disabled.
// ---------------------------------------------------------------------------
static bool powerSaveInhibited() {
  // Only genuinely heavy / screen-bound local work blocks power-save. ESP-NOW is
  // intentionally NOT here: a bonded peer being *online* is mere presence (the
  // 5 s heartbeat), not use, so it no longer pins the device awake. Real peer
  // activity — an @BOND/RCE command — flows through executeCommand and resets
  // the idle timer via the activity hook instead, so the device wakes on actual
  // use while an idle-but-bonded link is free to power-save.
  extern bool cameraStreaming;
  return micRecordingBusy() || cameraStreaming;
}

// Clock switch with an ACTIVE I2C drain. pausePolling() only stops NEW poll
// cycles — the flag is checked at top-of-loop, so an in-flight transaction
// (a thermal frame read can hold its bus up to its 1500 ms timeout) runs to
// completion regardless; a fixed post-pause delay is a guess, not a guarantee.
// Holding every bus mutex across the switch proves the buses are idle AND
// blocks any new transaction from starting (including cmd_exec-driven reads,
// which never consult the pause flag) for the microseconds the switch takes.
// The 2 s take outlasts the worst legitimate holder; on timeout the bus is
// already wedged (bus-recovery territory), so switch anyway rather than pin
// the power state forever.
static void setCpuFrequencyDrained(uint32_t mhz) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (mgr) mgr->pausePolling();
  SemaphoreHandle_t held[I2CDeviceManager::NUM_BUSES] = {};
  if (mgr) {
    for (uint8_t b = 0; b < I2CDeviceManager::NUM_BUSES; b++) {
      SemaphoreHandle_t m = mgr->getBusMutex(b);
      if (m && xSemaphoreTake(m, pdMS_TO_TICKS(2000)) == pdTRUE) held[b] = m;
    }
  }
  setCpuFrequencyMhz(mhz);
  if (mgr) {
    for (uint8_t b = 0; b < I2CDeviceManager::NUM_BUSES; b++) {
      if (held[b]) xSemaphoreGive(held[b]);
    }
    mgr->resumePolling();
  }
}

static void powerSaveTick() {
  static bool          asleep = false;
  static uint32_t      lastSeq = 0;
  static uint32_t      savedCpuMhz = 0;
  static unsigned long lastSeenActivityMs = 0;
  static bool          inited = false;

  // Wake: restore the CPU clock (only if we lowered it), then bring the panel
  // back. Centralized so every wake path stays in sync.
  auto wake = [&]() {
    // Restore the interactive clock iff idle-enter actually lowered it. Compare
    // to the LIVE clock rather than a fixed ">80" test: UltraSaver's interactive
    // floor is exactly 80 MHz yet it sinks to 40 MHz while asleep, so a ">80"
    // test would strand it at 40 after wake. savedCpuMhz holds the pre-idle
    // (interactive) clock; 0 means "we never lowered".
    if (savedCpuMhz != 0 && getCpuFrequencyMhz() < savedCpuMhz) {
      // Same hazard as the enter path below: switching the CPU clock while
      // any I2C task is mid-transaction panics the chip (reset=panic(4)).
      setCpuFrequencyDrained(savedCpuMhz);
    }
    savedCpuMhz = 0;
    gOledRunning = true;
    oledResumeFromSleep();
    oledMarkDirty();
    asleep = false;
    batteryLogEvent("powersave:wake");
    // Automation commands run as SOURCE_INTERNAL and skip the activity
    // stamp, so an automation reacting to this can't re-trigger the wake.
    systemEventPost(SYSEVT_POWER_SAVE_EXIT);
  };

  const unsigned long now = millis();
  if (!inited) {
    inited = true;
    lastSeq = gInputCache.seq;
    powerSaveNoteActivity();
    lastSeenActivityMs = powerSaveLastActivityMs();
  }

  // Feature disabled — keep the panel up and the idle clock fresh.
  if (gSettings.powerSaveTimeoutMinutes == 0) {
    if (asleep) wake();
    lastSeq = gInputCache.seq;
    powerSaveNoteActivity();
    lastSeenActivityMs = powerSaveLastActivityMs();
    return;
  }

  // Any input device (gamepad or ANO) advancing seq counts as activity.
  const uint32_t seq = gInputCache.seq;
  if (seq != lastSeq) { lastSeq = seq; powerSaveNoteActivity(); }

  // Any noted activity (input OR a user/peer command) wakes it / resets idle.
  // Sources only stamp the timestamp from their own task; the wake runs here
  // on the main loop, so OLED/CPU work never happens off-task.
  const unsigned long act = powerSaveLastActivityMs();
  if (act != lastSeenActivityMs) {
    lastSeenActivityMs = act;
    if (asleep) wake();
    return;
  }

  if (asleep) return;  // already dark, waiting for activity

  // Defer the countdown while genuinely busy (camera/mic, or a bonded peer).
  if (powerSaveInhibited()) {
    powerSaveNoteActivity();
    lastSeenActivityMs = powerSaveLastActivityMs();
    return;
  }

  const unsigned long timeoutMs = (unsigned long)gSettings.powerSaveTimeoutMinutes * 60000UL;
  if ((now - act) < timeoutMs) return;

  // Idle long enough — enter power-save, respecting the anti-flap cooldown.
  if (!powerSleepTransitionAllowed(nullptr)) return;
  powerSleepTransitionMark();
  oledPrepareForSleep();   // DISPLAYOFF (+ LDO2 cut on FeatherS3); board-aware
  gOledRunning = false;    // updateOLEDDisplay() now early-returns → refresh stops
  // Downclock to the current mode's IDLE floor. Locked keeps 240 through idle
  // (OLED blanks, core stays hot). Performance/Balanced/PowerSaver hold the
  // 80 MHz Wi-Fi floor while asleep — radio stays up so HTTP/ESP-NOW remain
  // reachable; we only shed dynamic core power. UltraSaver is the exception:
  // it sinks all the way to 40 MHz here (its headline deep-save clock), which
  // is safe ONLY because the panel is blanked and idle — 40 MHz makes the
  // interactive UI unusably laggy, so it's confined to this asleep state and
  // any input (gInputCache.seq) or command wakes it back to >=80 via wake()
  // above. Only switch if the floor is actually below the live clock (never
  // raise here).
  savedCpuMhz = getCpuFrequencyMhz();
  uint32_t idleFloorMhz = getPowerModeIdleCpuFreq(gSettings.powerMode);

  // A UART host link running above REF_TICK is clocked from APB, and on chips
  // whose HAL has that fallback (ESP32 / ESP32-S2) APB follows the CPU clock
  // once it sinks below 80 MHz. UltraSaver's 40 MHz idle floor would therefore
  // halve the link's effective baud and corrupt every byte — including the
  // CM5's heartbeat, so freshness would lapse with no way to recover: UART
  // traffic deliberately does not wake the device (see the SOURCE_UART
  // exclusion in executeCommand), and a garbled command cannot be parsed into
  // a wake in the first place. Clamp to the interactive floor instead; 240/160/
  // 80 are all PLL-derived and pin APB at 80 MHz, so 80 is enough — the other
  // three power modes already idle there. Costs UltraSaver some deep-idle
  // saving only while a fast link is actually up.
#if SOC_UART_SUPPORT_REF_TICK
  if (idleFloorMhz < POWER_INTERACTIVE_FLOOR_MHZ && uartLinkIsRunning() &&
      uartLinkEffectiveBaud() > UART_LINK_REF_TICK_BAUD_LIMIT) {
    DEBUG_SYSTEMF("[POWER] idle floor %lu -> %lu MHz: UART link at %d baud is "
                  "APB-clocked and cannot survive a sub-80 MHz CPU",
                  (unsigned long)idleFloorMhz,
                  (unsigned long)POWER_INTERACTIVE_FLOOR_MHZ,
                  uartLinkEffectiveBaud());
    idleFloorMhz = POWER_INTERACTIVE_FLOOR_MHZ;
  }
#endif

  if (idleFloorMhz < savedCpuMhz) {
    // Guard the clock switch. setCpuFrequencyMhz() swaps the CPU clock and
    // fires clock-change callbacks; unguarded, doing that while gps_task was
    // mid-I2C transaction panic-rebooted the device on every idle power-save
    // entry (reset=panic(4) every ~10 min whenever it was left alone with a
    // GPS fix — the whole 2026-07-17 crash-loop day).
    setCpuFrequencyDrained(idleFloorMhz);
  } else {
    savedCpuMhz = 0;  // nothing lowered → wake() must not "restore" a raise
  }
  asleep = true;
  batteryLogEvent("powersave:enter");
  {
    char det[20];
    snprintf(det, sizeof(det), "idle %dmin", (int)gSettings.powerSaveTimeoutMinutes);
    systemEventPost(SYSEVT_POWER_SAVE_ENTER, det);
  }
}
#endif // ENABLE_OLED_DISPLAY


void hardwareone_loop() {

  // ========================================================================
  // 1. DIAGNOSTICS — tiered: always-on lightweight health watches
  //    (loop-stall + heap-pressure WARN, ~free) plus debug-gated verbose
  //    dumps (memory sample, task table, loop-period distribution).
  // ========================================================================

  // Loop-timing anchor + per-section profiler. MUST run first so lap period and
  // section[0] timing start here. Always-on stall WARN (with section attribution);
  // verbose period + section distribution self-gate on DEBUG_PERFORMANCE inside.
  loopHealthTick();

  // periodicMemorySample() now does an always-on DRAM-pressure check before
  // its DEBUG_MEMORY-gated verbose sample, so a shipping device warns before
  // it OOMs even with memory debugging off.
  periodicMemorySample();

  // Declare this boot healthy once it has run for a while, which clears the
  // CONSECUTIVE crash counter. Without this the counter would only ever clear on
  // a power cycle, i.e. it would silently degrade into "crashes since poweron" —
  // the exact conflation it exists to avoid — and a device that crashed a few
  // times over weeks of healthy uptime would read as if it were mid-crash-loop.
  if (sHardwareOneRunning &&
      (unsigned long)(millis() - sHardwareOneRunningSinceMs) >= 60000UL) {
    crashRecordMarkBootHealthy();
  }

  // Finish an HTTP server teardown that a command had to defer because a web
  // request was mid-flight (see WebServer_Handle.h). This task is neither the
  // httpd task nor cmd_exec_task, so it can wait on httpd_stop safely; it is a
  // flag test in the overwhelmingly common case where nothing is pending.
#if ENABLE_HTTP_SERVER
  httpServerStopPendingTick();
#endif

  if (isDebugFlagSet(DEBUG_MEMORY)) {
    static unsigned long lastTaskReport = 0;
    unsigned long now = millis();
    if (now - lastTaskReport >= 60000) {
      lastTaskReport = now;
      reportAllTaskStacks();
    }
  }
  perfMarkSection(0);  // section 1: DIAGNOSTICS

  // ========================================================================
  // 2. PERIODIC I/O — timer-gated sampling and publishing
  // ========================================================================

  sensorLogTick();
  timeAnchorsTick();   // retro-date boot-named captures once the clock syncs
  g2RingTimeSyncTick(); // ring-clock custody: adopt when dark / correct ring after sync
  cm5TimeSyncTick();    // CM5 carrier RTC: adopt when dark / correct after sync (authoritative)
  g2Tick();             // glasses: overlay auto-dismiss + adaptive clock/tz re-push (self-throttled)
  ntpSyncDrainTick();   // hand real SNTP replies to the clock-step chokepoint
  Clock::clockDutiesTick(); // drain filesystem-touching clock-step chores
#if ENABLE_BLUETOOTH
  // Before scheduling another reseek: if the host stack is wedged, no number
  // of retries can clear it — recycle instead. Self-gating (worker idle,
  // nothing linked, rate-limited), so this is a cheap no-op the rest of the
  // time. Runs HERE rather than inside the connect worker because the recycle
  // vTaskDeletes that worker.
  bleStackRecycleIfWedged();
  bleAutoReconnectTick();
#endif

#if ENABLE_BATTERY_MONITOR
  {
    static unsigned long lastBatteryUpdate = 0;
    if (millis() - lastBatteryUpdate >= 10000) {
      lastBatteryUpdate = millis();
      updateBattery();
    }
  }
  batteryLogTick();  // self-gates on gSettings.batteryLogEnabled + interval
#endif

#if ENABLE_MQTT
  mqttTick();
#endif

#if ENABLE_BLUETOOTH
  bleUpdateStreams();
#endif

  perfMarkSection(1);  // section 2: PERIODIC I/O

  // ========================================================================
  // 3. EVENT-DRIVEN — only runs when dirty/triggered
  // ========================================================================

  if (gSensorStatusDirty) {
    unsigned long nowMs = millis();
    DEBUG_SSEF("[SSE_BROADCAST_CHECK] dirty=true, due=%lu, now=%lu, ready=%d",
                   gNextSensorStatusBroadcastDue, nowMs,
                   (gNextSensorStatusBroadcastDue != 0 && (long)(nowMs - gNextSensorStatusBroadcastDue) >= 0) ? 1 : 0);
    if (gNextSensorStatusBroadcastDue != 0 && (long)(nowMs - gNextSensorStatusBroadcastDue) >= 0) {
      DEBUG_SSEF("[SSE_BROADCAST] SENDING | seq=%lu thermal=%d tof=%d imu=%d gamepad=%d apdsColor=%d apdsProx=%d apdsGest=%d",
                     (unsigned long)gSensorStatusSeq,
                     gThermalRunning ? 1 : 0, gTofRunning ? 1 : 0, gImuRunning ? 1 : 0, gInputRunning ? 1 : 0,
                     gApdsColorRunning ? 1 : 0, gApdsProximityRunning ? 1 : 0, gApdsGestureRunning ? 1 : 0);
      broadcastSensorStatusToAllSessions();
      DEBUG_SSEF("[SSE_BROADCAST] SENT successfully");
      gSensorStatusDirty = false;
      gNextSensorStatusBroadcastDue = 0;
    }
  }

#if ENABLE_AUTOMATION
  // gAutomationSchedulerRunning, not automationAutoStart: autostart is boot
  // intent only, and gating the tick on it would leave a runtime
  // `automation system enable` enabled-but-never-ticking. The flag is also what
  // makes the boot gate hold — without it, automationsAnyDue() would report due
  // on a null cache and schedulerTickMinute() would rebuild and run anyway.
  if (gSettings.automationEnabled && gAutomationSchedulerRunning) {
    static unsigned long lastAutoCheck = 0;
    unsigned long nowAuto = millis();
    time_t nowT = time(nullptr);
    // Fast in-RAM due check: just an array scan of cached nextAt values, no
    // I/O. The expensive schedulerTickMinute only runs when something is
    // actually due, a subscribed bus event arrived, the cache is stale, an
    // edit occurred, or the 60s safety interval elapses. Event-driven ticks
    // are rate-limited to one per 250ms so a chatty subscribed source
    // (gesture waving, text spam) can't degenerate the loop into a full
    // file-read+parse every pass — events buffer in the ring meanwhile.
    bool needFullTick = gAutomationsDirty ||
                        automationsAnyDue(nowT) ||
                        (automationEventsPending() && (nowAuto - lastAutoCheck >= 250)) ||
                        (nowAuto - lastAutoCheck >= 60000);
    if (needFullTick) {
      gAutomationsDirty = false;
      schedulerTickMinute();
      lastAutoCheck = nowAuto;
    }
  }
#endif

  // Render human-facing notifications for mesh-origin bus events (peer
  // online/offline, text/file received). The producers run on espnow_task,
  // which must stay bounded — so the OLED banner + web toast happen here,
  // on the task every other notification renders from.
  systemEventsNotifyTick();

  // Structured event-history file sink (throttled internally; hands lines to
  // the debug output task, which does the actual file I/O).
  systemEventLogTick();

  perfMarkSection(2);  // section 3: EVENT-DRIVEN

  // ========================================================================
  // 4. NETWORK MAINTENANCE
  // ========================================================================

  // Emit the [EVENT][WIFI] connection-lost line + SYSEVT for a disconnect
  // snapshotted on the arduino_events task (kept tiny-frame there; the
  // 256 B log line + SystemEvent local stack HERE instead).
  wifiEventLogDrain();

  perfMarkSection(3);  // section 4: NETWORK MAINTENANCE

  // ========================================================================
  // 5. DISPLAY — OLED boot sequence and periodic refresh
  // ========================================================================

#if ENABLE_OLED_DISPLAY
  processOLEDBootSequence();
#endif

  oledUpdate();

  // Advance a running (non-blocking) LED effect one frame. Cheap no-op while
  // idle; ~20 ms pacing while active. See ledEffectStart in System_NeoPixel.
  ledEffectTick();

#if ENABLE_OLED_DISPLAY
  powerSaveTick();
  // Idle-logout the local-display (OLED) session after sessionIdleDisplay min of
  // no physical input. Separate from power-save: revokes the login, not pixels.
  localDisplaySessionTick();
#endif

  // Periodic tick for an active CLIMode (no-op when no mode is active or
  // when the mode doesn't define onTick). Used by the Phase 5 wizard's
  // future OLED-joystick polling; help and confirm modes don't define
  // onTick so this is a branch-and-return for them.
  cliModeTick();

  perfMarkSection(4);  // section 5: DISPLAY

  // ========================================================================
  // 6. USER INPUT — Serial CLI (always last before yield)
  // ========================================================================

  // Yield Serial to the setup wizard while it's running. The wizard reads
  // bytes directly via waitForSerialInputBlocking() on the cmd_exec task.
  // If this main-loop drain also reads from Serial, the two consumers race
  // for each byte — half the user's keystrokes land in the wizard (advances
  // pages), the other half get parsed as CLI commands here and submitted
  // to the cmd_exec queue. cmd_exec is busy with the wizard, so those
  // submissions sit in the queue for 10 s and print "[ERROR] Command
  // timed out". The `!gWizardOwnsSerial` guard on the while condition
  // makes this drain a no-op while the wizard is active, so every byte
  // reaches waitForSerialInputBlocking() instead. See declaration in
  // System_SetupWizard.h.
  while (!gWizardOwnsSerial && Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    const TransportSessionEpoch liveSerialInputEpoch =
        serialTransportInputEpoch();
    if (gSerialCLIEpoch != 0 &&
        gSerialCLIEpoch != liveSerialInputEpoch) {
      // A remote login/logout replaced the identity while this line was only
      // partially typed. Drop it instead of completing it as the successor.
      gSerialCLI = "";
      gSerialCLIEpoch = 0;
    }
    if (c == '\n') {
      if (gSerialCLIEpoch == 0 ||
          gSerialCLIEpoch != liveSerialInputEpoch) {
        gSerialCLI = "";
        gSerialCLIEpoch = 0;
        continue;
      }
      const TransportSessionEpoch admittedSerialEpoch = gSerialCLIEpoch;
      String cmd = gSerialCLI;
      cmd.trim();
      String admittedSerialUser;
      bool admittedSerialAuthed = false;
      if (serialTransportSessionSnapshot(admittedSerialUser,
                                         admittedSerialAuthed) !=
          admittedSerialEpoch) {
        // The identity changed after newline admission but before parsing.
        // Treat the whole line as belonging to the old incarnation.
        gSerialCLI = "";
        gSerialCLIEpoch = 0;
        continue;
      }

      // Serial idle-logout: drop an idle session before processing this line.
      // Serial has no passive traffic, so this only fires when the user types
      // after being away — exactly when we'd want to force re-login. The line
      // they just typed then falls through to the login gate and is rejected.
      // No-op when auth is off (gSerialAuthed false) or window=0 (see
      // sessionIdleExpired); never-stamped (0) is treated as fresh.
      if (admittedSerialAuthed && sessionIdleExpired(
              SOURCE_SERIAL,
              gSerialLastInteractionMs.load(std::memory_order_acquire))) {
        if (serialTransportSessionClearAndBeginDelivery(
                admittedSerialEpoch)) {
          Serial.println(
              "[serial] Signed out due to inactivity. Please log in again.");
          serialTransportSessionEndDelivery();
        }
        gSerialCLI = "";
        gSerialCLIEpoch = 0;
        Serial.print("$ ");
        break;
      }

      CommandArgs serialLineArgs(cmd);
      const bool serialLoginVerb =
          serialLineArgs.count() > 0 &&
          serialLineArgs.arg(0).equalsIgnoreCase("login");
      const bool serialLocalLogin =
          serialLoginVerb && !serialLineArgs.unterminatedQuote() &&
          serialLineArgs.count() == 3;

      // A bare login always belongs to this physical Serial endpoint, both
      // before authentication and when replacing an existing/AuthBypass
      // session. Supplying a fourth token is an explicit target operation;
      // it is allowed to reach the registry only after this source has a
      // named, non-Guest login.
      auto handleSerialLocalLogin = [&]() {
        const String u = serialLineArgs.arg(1);
        const String p = serialLineArgs.arg(2);
        const char* serialIp = "local";

        // Serialize credential validation through publication so a password
        // reset cannot revoke an empty slot and then have the old credential
        // publish a replacement session afterward.
        FsLockGuard authGuard("serial.login");
        if (!authGuard.held) {
          broadcastOutput("[serial] Authentication temporarily unavailable.");
          return;
        }

#if ENABLE_HTTP_SERVER
        unsigned long lockoutRemainingMs = 0;
        bool locked = isLoginLocked(serialIp, &lockoutRemainingMs);
#else
        bool locked = false;
#endif
        if (locked) {
#if ENABLE_HTTP_SERVER
          BROADCAST_PRINTF("[serial] Login locked out. Retry in %lu seconds.",
                           lockoutRemainingMs / 1000UL);
          recordLoginAttempt(SOURCE_SERIAL, u, serialIp, false, "Locked out");
#endif
        } else if (isValidUser(u, p)) {
#if ENABLE_HTTP_SERVER
          clearLoginAttempts(serialIp);
#endif
          const TransportSessionEpoch newEpoch =
              serialTransportSessionAuthenticatedIfEpoch(
                  admittedSerialEpoch, u);
          if (newEpoch != 0 && serialTransportSessionBeginDelivery(newEpoch)) {
            recordLoginAttempt(SOURCE_SERIAL, u, serialIp, true,
                               "Login successful");
            const bool isCurrentlyAdmin = isAdminUser(u);
            Serial.printf("[serial] Login successful. User: %s%s\n", u.c_str(),
                          isCurrentlyAdmin ? " (admin)" : "");
            serialTransportSessionEndDelivery();
          } else {
            Serial.println("[serial] Session changed before login completed.");
          }
        } else {
#if ENABLE_HTTP_SERVER
          recordFailedLogin(serialIp);
#endif
          recordLoginAttempt(SOURCE_SERIAL, u, serialIp, false,
                             "Invalid credentials");
          broadcastOutput("[serial] Authentication failed.");
        }
      };

      // Serial auth gate: require login before executing any commands (if enabled)
      if (gSettings.serialRequireAuth && !admittedSerialAuthed) {
        if (serialLocalLogin) {
          handleSerialLocalLogin();
        } else if (serialLoginVerb) {
          broadcastOutput("Serial - Sign in first with bare: login <username> <password>");
        } else if (cmd.length() > 0) {
          broadcastOutput("Serial - Authentication required. Use: login <username> <password>");
        }
      } else {
        if (serialLocalLogin) {
          handleSerialLocalLogin();
        } else if (serialLineArgs.count() == 1 &&
                   serialLineArgs.arg(0).equalsIgnoreCase("logout")) {
          if (serialTransportSessionClearAndBeginDelivery(
                  admittedSerialEpoch)) {
            Serial.println("Logged out.");
            serialTransportSessionEndDelivery();
          }
        } else if (serialLineArgs.count() == 1 &&
                   serialLineArgs.arg(0).equalsIgnoreCase("whoami")) {
          String liveSerialUser;
          bool liveSerialAuthed = false;
          const TransportSessionEpoch whoamiEpoch =
              serialTransportSessionSnapshot(liveSerialUser,
                                             liveSerialAuthed);
          const bool isCurrentlyAdmin =
              liveSerialAuthed && isAdminUser(liveSerialUser);
          if (whoamiEpoch == admittedSerialEpoch &&
              serialTransportSessionBeginDelivery(whoamiEpoch)) {
            Serial.printf("You are %s%s\n",
                          liveSerialAuthed && liveSerialUser.length()
                              ? liveSerialUser.c_str() : "AuthBypass",
                          isCurrentlyAdmin ? " (admin)" : "");
            serialTransportSessionEndDelivery();
          }
        } else {
          appendCommandToFeed("serial", cmd);

          AuthContext actx;
          actx.transport = SOURCE_SERIAL;
          // When serialRequireAuth is off and no one has explicitly logged in,
          // stamp the audit log with "AuthBypass" instead of an empty user so
          // log lines read `[CMD] AuthBypass@serial: ...` (clear physical-user
          // origin) instead of `[CMD] @serial: ...` (ambiguous). Reserved
          // username; see adminCreateUser. Matches the OLED sentinel pattern
          // in buildOLEDCommand.
          const TransportSessionEpoch commandSessionEpoch =
              admittedSerialEpoch;
          actx.user = admittedSerialAuthed && admittedSerialUser.length()
                          ? admittedSerialUser : String("AuthBypass");
          actx.ip = "local";
          actx.path = "serial";
          Command uc;
          uc.line = cmd;
          uc.ctx.origin = ORIGIN_SERIAL;
          uc.ctx.auth = actx;
          uc.ctx.id = (uint32_t)millis();
          uc.ctx.timestampMs = (uint32_t)millis();
          uc.ctx.transportSessionEpoch = commandSessionEpoch;
          // Serial is a stateful physical endpoint even when authentication is
          // disabled. A failed epoch capture must therefore fail closed rather
          // than silently turning this queued command into an unbound caller.
          uc.ctx.behaviorFlags |= COMMAND_CONTEXT_REQUIRE_LIVE_SESSION;
          uc.ctx.outputMask = MSG_ROUTE_SERIAL | MSG_ROUTE_FILE;
          uc.ctx.validateOnly = false;
          uc.ctx.replyHandle = nullptr;
          uc.ctx.httpReq = nullptr;

          String out;
          (void)submitAndExecuteSync(uc, out);
          deliverCommandResult(out, uc.ctx);
        }
      }
      // Every serial line is a real keystroke-driven interaction; refresh the
      // idle clock whenever we end this line authenticated. One stamp covers
      // both a fresh successful login and any subsequent command. Skipped when
      // not authed (failed login / logout / auth disabled) — nothing to age.
      String completedSerialUser;
      bool completedSerialAuthed = false;
      const TransportSessionEpoch completedSerialEpoch =
          serialTransportSessionSnapshot(completedSerialUser,
                                         completedSerialAuthed);
      if (completedSerialAuthed &&
          completedSerialEpoch == admittedSerialEpoch)
        gSerialLastInteractionMs.store(sessionStampNow(),
                                       std::memory_order_release);

      gSerialCLI = "";
      gSerialCLIEpoch = 0;
      Serial.print("$ ");
      break;  // Process at most one command per loop() iteration to avoid starving WDT
    } else {
      if (gSerialCLIEpoch == 0) gSerialCLIEpoch = liveSerialInputEpoch;
      gSerialCLI += c;
    }
  }

  // UART host link drain — the CM5-facing machine channel. Same one-command-
  // per-lap discipline as the serial drain above; no-op unless the link was
  // started (uartLinkEnabled). Parked while the wizard owns the CLI so link
  // commands don't queue behind a wizard-occupied cmd_exec and time out.
  uartLinkTick();
#if ENABLE_RASPBERRY_PI_HOST_POWER
  cm5HostPowerTick();
#endif
#if ENABLE_RASPBERRY_PI_HOST_FAN
  cm5HostFanTick();
#endif
#if ENABLE_LLM_BACKEND
  // Times out a remote generation whose host went silent. Without it one lost
  // `cm5 llm end` line strands the chat layer's streaming turn on EVERY surface.
  llmBackendTick();
#endif

  perfMarkSection(5);  // section 6: USER INPUT

  // ========================================================================
  // YIELD — give scheduler time to run lower-priority tasks and service ISRs
  // ========================================================================

  delay(2);

  // A heartbeat means every HardwareOne loop section completed. The OTA image
  // is eligible for validation only while these arrive continuously and the
  // two core boot services remain usable for the full probation interval.
  const uint32_t cmdHeartbeatAge =
      millis() - sCmdExecHeartbeatMs.load(std::memory_order_acquire);
  otaSafetyLoopHeartbeat(filesystemReady && gCmdExecQ != nullptr &&
                         gCmdExecTaskHandle != nullptr && cmdHeartbeatAge <= 5000U);
}
