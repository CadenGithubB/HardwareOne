/**
 * System Utilities - Shared functions used across modules
 * 
 * This file contains common utility functions that are used by multiple
 * subsystems (automation, commands, etc.) to avoid circular dependencies
 * and work around Arduino IDE's 1MB .ino file symbol export limitation.
 * 
 * Also contains the centralized command registry system that collects
 * command tables from all modules.
 */

#include <Arduino.h>
#include "System_BuildConfig.h"
#if ENABLE_WIFI
  #include <WiFi.h>
#endif
#include <LittleFS.h>
#if ENABLE_HTTP_SERVER
  #include <esp_http_server.h>
#endif
#include <esp_timer.h>
#include <time.h>
#include "System_Utils.h"
#include "System_Debug.h"
#include "System_TaskUtils.h"
#include "System_I2C.h"
#include "System_User.h"
#include "System_Command.h"  // For CommandModuleRegistrar
#include "System_SensorStubs.h"  // Stubs for disabled sensors/modules
#include "System_MemoryMonitor.h"
#include "System_Notifications.h"
#include "i2csensor-ds3231.h"  // RTC for time functions
#include "System_Utils.h"
#include "System_ESPSR.h"

extern "C" {
  extern uint8_t _bss_start;
  extern uint8_t _bss_end;
  extern uint8_t _noinit_start;
  extern uint8_t _noinit_end;
  extern uint8_t _ext_ram_bss_start;
  extern uint8_t _ext_ram_bss_end;
  extern uint8_t _ext_ram_noinit_start;
  extern uint8_t _ext_ram_noinit_end;
}

#pragma weak _ext_ram_noinit_start
#pragma weak _ext_ram_noinit_end


// ============================================================================
// Task Execution Performance Monitoring Implementation
// ============================================================================

// Global task metrics instance
TaskExecutionMetrics gTaskMetrics = {0, 0, 0, 0, 0};

void taskOperationStart() {
  // Currently just tracking at completion - could add start timestamps here if needed
}

void taskOperationComplete(uint32_t elapsedMs, uint32_t timeoutThresholdMs) {
  gTaskMetrics.totalOperations++;
  
  // Update EWMA average (exponentially weighted moving average)
  if (gTaskMetrics.totalOperations > 1) {
    gTaskMetrics.avgExecutionMs = (gTaskMetrics.avgExecutionMs * 7 + elapsedMs) / 8;
  } else {
    gTaskMetrics.avgExecutionMs = elapsedMs;
  }
  
  // Update peak timing
  if (elapsedMs > gTaskMetrics.maxExecutionMs) {
    gTaskMetrics.maxExecutionMs = elapsedMs;
  }
  
  // Timeout detection
  if (elapsedMs > timeoutThresholdMs) {
    gTaskMetrics.timeoutCount++;
    
    // Log performance timeout
    char buf[96];
    snprintf(buf, sizeof(buf), "[TASK] TIMEOUT: elapsed=%lums, max=%lums (total_timeouts=%lu)",
             (unsigned long)elapsedMs, (unsigned long)timeoutThresholdMs, 
             (unsigned long)gTaskMetrics.timeoutCount);
    broadcastOutput(buf);
  }
}

void resetTaskMetrics() {
  gTaskMetrics.totalOperations = 0;
  gTaskMetrics.timeoutCount = 0;
  gTaskMetrics.avgExecutionMs = 0;
  gTaskMetrics.maxExecutionMs = 0;
  gTaskMetrics.lastResetMs = millis();
}

// ============================================================================
// Security Utilities
// ============================================================================

// Securely clear a String's internal buffer before releasing memory
// Uses volatile to prevent compiler from optimizing away the memset
void secureClearString(String& s) {
  if (s.length() == 0) return;
  
  // Get pointer to internal buffer and overwrite with zeros
  // Arduino String stores data in a char array accessible via c_str() but that's const
  // We need to use begin() which returns a non-const iterator on some platforms
  // For ESP32/Arduino, we can cast away const since we're about to clear it anyway
  char* buf = const_cast<char*>(s.c_str());
  size_t len = s.length();
  
  // Use volatile pointer to prevent optimizer from removing the memset
  volatile char* vbuf = buf;
  for (size_t i = 0; i < len; i++) {
    vbuf[i] = 0;
  }
  
  // Now actually clear the String
  s = "";
}

#include "System_Settings.h"
#include "System_Mutex.h"   // For FsLockGuard
#include "System_I2C.h"     // For I2CSensorEntry, ConnectedDevice, MAX_CONNECTED_DEVICES
#include "System_MemUtil.h"       // For AllocPref

// Extern declarations for logging functions (implemented in .ino)
extern bool appendLineWithCap(const char* path, const String& line, size_t capBytes);

// Extern declarations for command arrays moved to individual modules
#if ENABLE_THERMAL_SENSOR
extern const CommandEntry thermalCommands[];
extern const size_t thermalCommandsCount;
#endif
#if ENABLE_TOF_SENSOR
extern const CommandEntry tofCommands[];
extern const size_t tofCommandsCount;
#endif
#if ENABLE_IMU_SENSOR
extern const CommandEntry imuCommands[];
extern const size_t imuCommandsCount;
#endif
#if ENABLE_GAMEPAD_SENSOR
extern const CommandEntry gamepadCommands[];
extern const size_t gamepadCommandsCount;
#endif
#if ENABLE_APDS_SENSOR
extern const CommandEntry apdsCommands[];
extern const size_t apdsCommandsCount;
#endif
#if ENABLE_GPS_SENSOR
extern const CommandEntry gpsCommands[];
extern const size_t gpsCommandsCount;
#endif
#if ENABLE_FM_RADIO
extern const CommandEntry fmRadioCommands[];
extern const size_t fmRadioCommandsCount;
#endif
#if ENABLE_RTC_SENSOR
extern const CommandEntry rtcCommands[];
extern const size_t rtcCommandsCount;
#endif
#if ENABLE_PRESENCE_SENSOR
extern const CommandEntry presenceCommands[];
extern const size_t presenceCommandsCount;
#endif
#if ENABLE_CAMERA_SENSOR
extern const CommandEntry cameraCommands[];
extern const size_t cameraCommandsCount;
extern bool cameraConnected;
#endif
#if ENABLE_EDGE_IMPULSE
extern const CommandEntry edgeImpulseCommands[];
extern const size_t edgeImpulseCommandsCount;
#endif

 #if ENABLE_ESP_SR
 extern const CommandEntry espsrCommands[];
 extern const size_t espsrCommandsCount;
 #endif
#if ENABLE_MICROPHONE_SENSOR
extern const CommandEntry micCommands[];
extern const size_t micCommandsCount;
extern bool micConnected;
#endif
extern const CommandEntry userSystemCommands[];
extern const size_t userSystemCommandsCount;
extern const CommandEntry sensorLoggingCommands[];
extern const size_t sensorLoggingCommandsCount;

// Include module headers to access their command registries
#include "System_Filesystem.h"
#include "System_Debug.h"
#include "System_CLI.h"
#include "System_BuildConfig.h"   // Conditional sensor configuration - must be early
#if ENABLE_WIFI
  #include "System_WiFi.h"
#endif
#include "OLED_Display.h"
#include "System_NeoPixel.h"
#if ENABLE_SERVO
#include "i2csensor-pca9685.h"
#endif
#include "System_Automation.h"
#include "System_I2C.h"
#if ENABLE_ESPNOW
  #include "System_ESPNow.h"
#endif
#if ENABLE_BLUETOOTH
  #include "Optional_Bluetooth.h"
#endif
#include "System_Settings.h"
#include "System_User.h"
#include "System_VFS.h"  // For sdCommands (SD card management)
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor-mlx90640.h"  // For thermalCommands
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor-vl53l4cx.h"      // For tofCommands
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor-bno055.h"      // For imuCommands
#endif
#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor-seesaw.h"  // For gamepadCommands
#endif
#if ENABLE_APDS_SENSOR
  #include "i2csensor-apds9960.h"     // For apdsCommands
#endif
#include "System_SensorStubs.h" // Stubs for disabled sensors
#if ENABLE_GPS_SENSOR
  #include "i2csensor-pa1010d.h"      // For gpsCommands
#endif
#include "i2csensor-rda5807.h"        // For fmRadioCommands

// External dependencies from .ino
extern bool filesystemReady;
extern Settings gSettings;
extern bool gAutoLogActive;
extern String gAutoLogFile;
// ============================================================================
// Base64 Encoding (moved from .ino)
// ============================================================================

String base64Encode(const uint8_t* data, size_t len) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += table[(v >> 18) & 0x3F];
    out += table[(v >> 12) & 0x3F];
    out += table[(v >> 6) & 0x3F];
    out += table[v & 0x3F];
    i += 3;
  }
  // Handle padding
  if (i < len) {
    uint32_t v = data[i] << 16;
    if (i + 1 < len) {
      // 2 remaining bytes = 16 bits = 3 base64 chars + 1 padding
      v |= data[i + 1] << 8;
      out += table[(v >> 18) & 0x3F];
      out += table[(v >> 12) & 0x3F];
      out += table[(v >> 6) & 0x3F];
      out += '=';
    } else {
      // 1 remaining byte = 8 bits = 2 base64 chars + 2 padding
      out += table[(v >> 18) & 0x3F];
      out += table[(v >> 12) & 0x3F];
      out += '=';
      out += '=';
    }
  }
  return out;
}

String base64Decode(const String& input) {
  const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String output = "";
  size_t expectedSize = (input.length() * 3) / 4;
  output.reserve(expectedSize);
  
  int val = 0, valb = -8;
  for (unsigned char c : input) {
    if (c == '=') break;
    const char* p = strchr(base64_chars, c);
    if (!p) continue;
    val = (val << 6) + (p - base64_chars);
    valb += 6;
    if (valb >= 0) {
      output += char((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  return output;
}

// ============================================================================
// JSON Parsing Helpers (moved from .ino)
// ============================================================================

bool parseJsonBool(const String& src, const char* key, bool& out) {
  String k = String("\"") + key + "\":";
  int p = src.indexOf(k);
  if (p < 0) return false;
  p += k.length();
  while (p < (int)src.length() && src[p] == ' ') p++;
  if (p >= (int)src.length()) return false;
  if (src.startsWith("true", p)) {
    out = true;
    return true;
  }
  if (src.startsWith("false", p)) {
    out = false;
    return true;
  }
  if (src[p] == '1') {
    out = true;
    return true;
  }
  if (src[p] == '0') {
    out = false;
    return true;
  }
  return false;
}

bool parseJsonInt(const String& src, const char* key, int& out) {
  String k = String("\"") + key + "\":";
  int p = src.indexOf(k);
  if (p < 0) return false;
  p += k.length();
  while (p < (int)src.length() && src[p] == ' ') p++;
  if (p >= (int)src.length()) return false;
  int end = p;
  while (end < (int)src.length()) {
    char c = src[end];
    if ((c >= '0' && c <= '9') || c == '-') {
      end++;
      continue;
    }
    break;
  }
  if (end == p) return false;
  out = src.substring(p, end).toInt();
  return true;
}

bool parseJsonFloat(const String& src, const char* key, float& out) {
  String k = String("\"") + key + "\":";
  int p = src.indexOf(k);
  if (p < 0) return false;
  p += k.length();
  while (p < (int)src.length() && src[p] == ' ') p++;
  if (p >= (int)src.length()) return false;
  bool seenDigit = false, seenDot = false;
  int end = p;
  while (end < (int)src.length()) {
    char c = src[end];
    if (c >= '0' && c <= '9') {
      seenDigit = true;
      end++;
      continue;
    }
    if (c == '-' && end == p) {
      end++;
      continue;
    }
    if (c == '.' && !seenDot) {
      seenDot = true;
      end++;
      continue;
    }
    break;
  }
  if (!seenDigit) return false;
  out = src.substring(p, end).toFloat();
  return true;
}

bool parseJsonU16(const String& src, const char* key, uint16_t& out) {
  int tmp = 0;
  if (!parseJsonInt(src, key, tmp)) return false;
  out = (uint16_t)tmp;
  return true;
}

bool parseJsonString(const String& src, const char* key, String& out) {
  String k = String("\"") + key + "\":\"";
  int p = src.indexOf(k);
  if (p < 0) return false;
  p += k.length();
  int end = src.indexOf('\"', p);
  if (end < 0) return false;
  out = src.substring(p, end);
  return true;
}

bool extractObjectByKey(const String& src, const char* key, String& outObj) {
  String k = String("\"") + key + "\"";
  int keyPos = src.indexOf(k);
  if (keyPos < 0) return false;
  int colon = src.indexOf(':', keyPos + k.length());
  if (colon < 0) return false;
  int openBrace = src.indexOf('{', colon);
  if (openBrace < 0) return false;
  int depth = 1;
  int closeBrace = openBrace + 1;
  while (closeBrace < (int)src.length() && depth > 0) {
    char c = src[closeBrace];
    if (c == '{') depth++;
    else if (c == '}') depth--;
    closeBrace++;
  }
  if (depth != 0) return false;
  outObj = src.substring(openBrace, closeBrace);
  return true;
}

bool extractArrayByKey(const String& src, const char* key, String& outArray) {
  String k = String("\"") + key + "\"";
  int keyPos = src.indexOf(k);
  if (keyPos < 0) return false;
  int colon = src.indexOf(':', keyPos + k.length());
  if (colon < 0) return false;
  int lb = src.indexOf('[', colon);
  if (lb < 0) return false;
  int depth = 0;
  for (int i = lb; i < (int)src.length(); ++i) {
    char c = src[i];
    if (c == '[') depth++;
    else if (c == ']') {
      depth--;
      if (depth == 0) {
        outArray = src.substring(lb + 1, i);
        return true;
      }
    }
  }
  return false;
}

bool extractArrayItem(const String& arrayStr, int& pos, String& outItem) {
  while (pos < arrayStr.length() && (arrayStr[pos] == ' ' || arrayStr[pos] == '\t' || arrayStr[pos] == '\n' || arrayStr[pos] == ',')) {
    pos++;
  }
  if (pos >= arrayStr.length()) return false;
  if (arrayStr[pos] == '{') {
    int depth = 0;
    int start = pos;
    for (int i = pos; i < arrayStr.length(); ++i) {
      char c = arrayStr[i];
      if (c == '{') depth++;
      else if (c == '}') {
        depth--;
        if (depth == 0) {
          outItem = arrayStr.substring(start, i + 1);
          pos = i + 1;
          return true;
        }
      }
    }
  }
  return false;
}

// ============================================================================
// URL Encoding/Decoding Utilities (moved from .ino)
// ============================================================================

String urlEncode(const char* s) {
  String out;
  if (!s) return out;
  out.reserve(strlen(s) * 3);
  auto hex = [](uint8_t v) -> char {
    const char* H = "0123456789ABCDEF";
    return H[v & 0x0F];
  };
  for (const char* p = s; *p; ++p) {
    char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      uint8_t b = static_cast<uint8_t>(c);
      out += '%';
      out += hex((b >> 4) & 0x0F);
      out += hex(b & 0x0F);
    }
  }
  return out;
}

String urlDecode(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < s.length()) {
      auto hexv = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
        return -1;
      };
      int hi = hexv(s[i + 1]);
      int lo = hexv(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += char((hi << 4) | lo);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

String extractFormField(const String& body, const String& key) {
  String k = key + "=";
  int pos = 0;
  while (pos <= (int)body.length()) {
    int amp = body.indexOf('&', pos);
    int end = (amp < 0) ? body.length() : amp;
    String pair = body.substring(pos, end);
    int eq = pair.indexOf('=');
    if (eq > 0) {
      String pk = pair.substring(0, eq);
      if (pk == key) {
        return pair.substring(eq + 1);
      }
    }
    if (amp < 0) break;
    pos = amp + 1;
  }
  return String("");
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in.charAt(i);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

int serializeJsonArrayWithRepair(JsonArray& arr, char* buf, size_t bufSize, const char* context) {
  size_t len = serializeJson(arr, buf, bufSize);
  int removed = 0;
  while (len >= bufSize && arr.size() > 0) {
    arr.remove(0);  // Remove oldest (first) entry
    removed++;
    len = serializeJson(arr, buf, bufSize);
  }
  if (removed > 0) {
    WARN_MEMORYF("%s JSON overflow: removed %d oldest entries to fit %zu byte buffer", 
                 context, removed, bufSize);
  }
  return removed;
}

// ============================================================================
// Date/Time Formatting Utilities
// ============================================================================

String formatDateTime(time_t timestamp) {
  struct tm* timeinfo = localtime(&timestamp);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(buffer);
}

// ============================================================================
// Serial Input Helpers (moved from .ino)
// ============================================================================

String waitForSerialInput(unsigned long timeoutMs) {
  unsigned long start = millis();
  String input = "";
  while (millis() - start < timeoutMs) {
    if (Serial.available()) {
      input = Serial.readStringUntil('\n');
      input.trim();
      return input;
    }
    delay(10);
  }
  return "";
}

String waitForSerialInputBlocking() {
  for (;;) {
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      return input;
    }
    delay(10);
  }
}

// ============================================================================
// Time Sync Functions (moved from .ino)
// ============================================================================

// Maintains an offset to convert monotonic microseconds to epoch microseconds.
static int64_t gBootEpochUsOffset = 0;

void timeSyncUpdateBootEpoch() {
  time_t now = time(nullptr);
  if (now > 0) {
    gBootEpochUsOffset = (int64_t)now * 1000000LL - (int64_t)esp_timer_get_time();
  }
}

void getTimestampPrefixMsCached(char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (gBootEpochUsOffset == 0) {
    timeSyncUpdateBootEpoch();
  }
  int64_t epochUs = 0;
  if (gBootEpochUsOffset != 0) {
    epochUs = gBootEpochUsOffset + (int64_t)esp_timer_get_time();
  }
  if (epochUs <= 0) return;  // no valid time

  time_t sec = (time_t)(epochUs / 1000000LL);
  int ms = (int)((epochUs / 1000LL) % 1000LL);
  struct tm tminfo;
  if (!localtime_r(&sec, &tminfo)) return;
  // Sanity check: if RTC is unsynced, it may report 1970/1980 era time.
  // Require a reasonable year (>=2020) before we emit a formatted timestamp.
  if (tminfo.tm_year < 120) {  // tm_year is years since 1900
    return;
  }
  // Build prefix directly into caller's buffer
  char base[24];  // "[YYYY-MM-DD HH:MM:SS" (23 + NUL)
  if (strftime(base, sizeof(base), "[%Y-%m-%d %H:%M:%S", &tminfo) <= 0) return;
  snprintf(out, outSize, "%s.%03d] | ", base, ms);
}

// Sensor state externs for cmd_voltage
extern bool thermalConnected;
extern bool thermalEnabled;
extern bool imuConnected;
extern bool imuEnabled;
extern bool tofConnected;
extern bool tofEnabled;
extern bool apdsConnected;

// ============================================================================
// File I/O Functions
// ============================================================================

bool readText(const char* path, String& out) {
  out = "";
  
  // Pause sensor polling during file I/O to prevent I2C contention
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  
  FsLockGuard guard("readText");
  File f = LittleFS.open(path, "r");
  if (!f) {
    gSensorPollingPaused = wasPaused;
    return false;
  }
  out = f.readString();
  f.close();
  
  // Resume sensor polling
  gSensorPollingPaused = wasPaused;
  return true;
}

bool writeText(const char* path, const String& in) {
  // Pause sensor polling during file I/O to prevent I2C contention
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  
  FsLockGuard guard("writeText");
  File f = LittleFS.open(path, "w");
  if (!f) {
    gSensorPollingPaused = wasPaused;
    return false;
  }
  f.print(in);
  f.close();
  
  // Resume sensor polling
  gSensorPollingPaused = wasPaused;
  return true;
}

// ============================================================================
// Settings Persistence
// ============================================================================
// NOTE: saveUnifiedSettings() removed - use writeSettingsJson() from .ino instead

// ============================================================================
// Automation Logging
// ============================================================================
// ============================================================================
// Command Audit Logging (Always-On)
// ============================================================================

extern bool gCLIValidateOnly;

/**
 * Log command execution to audit file
 * Format: [timestamp] user@transport command -> result_status
 * Lightweight, always-enabled, no overhead when disabled
 */
void logCommandExecution(const AuthContext& ctx, const char* cmd, bool success, const char* result) {
  // Skip validation-only commands (dry-run checks)
  if (gCLIValidateOnly) return;
  
  // Build log entry
  char entry[512];
  unsigned long ts = millis() / 1000;  // Seconds since boot
  
  // Command source
  const char* source = "unknown";
  switch (ctx.transport) {
    case SOURCE_SERIAL: source = "serial"; break;
    case SOURCE_WEB: source = "web"; break;
    case SOURCE_ESPNOW: source = "espnow"; break;
    case SOURCE_INTERNAL: source = "internal"; break;
    case SOURCE_LOCAL_DISPLAY: source = "display"; break;
    case SOURCE_BLUETOOTH: source = "bluetooth"; break;
    case SOURCE_MQTT: source = "mqtt"; break;
    case SOURCE_VOICE: source = "voice"; break;
    default: source = "unknown"; break;
  }
  
  // Redact sensitive data from command
  String redactedCmd = redactCmdForAudit(String(cmd));
  
  // Result summary (first 40 chars, single line)
  String resultSummary = result ? String(result) : "OK";
  resultSummary.replace("\n", " ");
  resultSummary.replace("\r", " ");
  if (resultSummary.length() > 40) {
    resultSummary = resultSummary.substring(0, 37) + "...";
  }
  
  // Status indicator
  const char* status = success ? "OK" : "FAIL";
  
  // Format: [timestamp] user@source cmd -> status result
  snprintf(entry, sizeof(entry), "[%lu] %s@%s %s -> %s %s",
           ts,
           ctx.user.c_str(),
           source,
           redactedCmd.c_str(),
           status,
           resultSummary.c_str());
  
  // Append to audit log with 500KB cap (rotates automatically)
  appendLineWithCap("/system/logs/command-audit.log", String(entry), 500 * 1024);
}

// Automation logging
bool appendAutoLogEntry(const char* type, const String& message) {
  if (!gAutoLogActive || gAutoLogFile.length() == 0) return false;
  if (!filesystemReady) return false;

  // Get timestamp in same format as existing logs: [YYYY-MM-DD HH:MM:SS.mmm]
  char tsPrefix[32];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));

  // Format: [YYYY-MM-DD HH:MM:SS.mmm] | type | content
  String line;
  line.reserve(200);
  if (tsPrefix[0]) line += tsPrefix;  // already includes trailing " | "
  line += type;
  line += " | ";
  line += message;
  line += "\n";

  // Append to file (create if doesn't exist)
  File f = LittleFS.open(gAutoLogFile, "a");
  if (!f) {
    // Try to create directory if it doesn't exist
    int lastSlash = gAutoLogFile.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = gAutoLogFile.substring(0, lastSlash);
      if (!LittleFS.exists(dir)) {
        // Create directory recursively (simple approach for /logs)
        if (dir == "/logs" && !LittleFS.exists("/logs")) {
          LittleFS.mkdir("/logs");
        }
      }
    }

    // Try to open again after directory creation
    f = LittleFS.open(gAutoLogFile, "a");
    if (!f) return false;
  }

  size_t written = f.print(line);
  f.close();

  return written > 0;
}

// NOTE: processCommand() removed - use executeCommand() directly
// executeCommand() is defined in main .ino and requires AuthContext

// =========================================================================
// Audit / Redaction utilities
// =========================================================================

namespace {
  enum RedactType : uint8_t {
    MASK_TOKEN_AT_POS = 0,
    MASK_AFTER_TOKEN_POS = 1,
    CALL_HANDLER = 2,
  };

  struct RedactRule {
    const char* prefix;      // lowercase prefix to match, including trailing space when appropriate
    RedactType type;         // action type
    uint8_t param;           // token index (1-based) for MASK_* types
    String (*handler)(const String&); // optional specialized handler
  };

  static int indexOfNthSpace(const String& s, int n, int startIdx = 0) {
    int idx = startIdx - 1;
    for (int i = 0; i < n; i++) {
      idx = s.indexOf(' ', idx + 1);
      if (idx < 0) return -1;
    }
    return idx;
  }

  static String redactEspNowRemote(const String& in) {
    // Expect: "espnow remote <target> <username> <password> <command>..."
    String c = in;
    int base = c.indexOf(' ');                      // after "espnow"
    if (base > 0) base = c.indexOf(' ', base + 1);  // after "remote"
    if (base > 0) {
      int t1 = c.indexOf(' ', base + 1);                 // end of <target>
      int t2 = (t1 > 0) ? c.indexOf(' ', t1 + 1) : -1;   // end of <username>
      int t3 = (t2 > 0) ? c.indexOf(' ', t2 + 1) : -1;   // end of <password>
      if (t1 > 0 && t2 > 0) {
        String head = c.substring(0, t1 + 1);  // includes trailing space after <target>
        String afterUser = (t3 > 0) ? c.substring(t3) : String();
        return head + "***:***" + (afterUser.length() ? String(" ") + afterUser : String());
      }
    }
    return c;
  }

  // Rule table (extend here to add new redactions)
  static const RedactRule kRules[] = {
    { "wifiadd ", MASK_TOKEN_AT_POS, 3, nullptr },        // mask password token
    { "user request ", MASK_AFTER_TOKEN_POS, 3, nullptr }, // mask everything after username
    { "espnow remote ", CALL_HANDLER, 0, &redactEspNowRemote },
  };
}

String redactCmdForAudit(const String& cmd) {
  String c = cmd;
  String cl = c; cl.toLowerCase();

  for (size_t i = 0; i < (sizeof(kRules) / sizeof(kRules[0])); ++i) {
    const RedactRule& r = kRules[i];
    if (!cl.startsWith(r.prefix)) continue;

    if (r.type == CALL_HANDLER && r.handler) {
      return r.handler(c);
    }

    // Compute token boundaries (tokens separated by a single space in our CLI)
    // Token positions are 1-based over the entire line (including the command words)
    if (r.type == MASK_TOKEN_AT_POS) {
      int prevSpace = indexOfNthSpace(c, r.param - 1);
      if (prevSpace < 0) return c;
      int nextSpace = c.indexOf(' ', prevSpace + 1);
      String head = c.substring(0, prevSpace + 1);
      String tail = (nextSpace > 0) ? c.substring(nextSpace) : String();
      return head + "***" + tail;
    }

    if (r.type == MASK_AFTER_TOKEN_POS) {
      int endSpace = indexOfNthSpace(c, r.param);
      if (endSpace < 0) return c;
      String head = c.substring(0, endSpace + 1);
      return head + "***";
    }
  }

  return c;
}

// Redact sensitive data from command outputs (JSON responses, etc.)
String redactOutputForLog(const String& output) {
  String result = output;
  
  // Redact password hashes: "password":"HASH:xxxxx" -> "password":"***"
  // Also handles "password": "HASH:xxxxx" (with space)
  int pos = 0;
  while ((pos = result.indexOf("\"password\"", pos)) >= 0) {
    int colonPos = result.indexOf(':', pos + 10);
    if (colonPos < 0) break;
    
    // Skip optional space after colon
    int quoteStart = colonPos + 1;
    while (quoteStart < result.length() && result[quoteStart] == ' ') quoteStart++;
    
    // Find opening quote
    if (quoteStart >= result.length() || result[quoteStart] != '"') {
      pos = colonPos + 1;
      continue;
    }
    
    // Find closing quote
    int quoteEnd = result.indexOf('"', quoteStart + 1);
    if (quoteEnd < 0) break;
    
    // Replace the value with "***"
    result = result.substring(0, quoteStart + 1) + "***" + result.substring(quoteEnd);
    pos = quoteStart + 4;  // Move past the redacted value
  }
  
  // Redact session IDs: "sid":"long-hex-string" -> "sid":"***"
  pos = 0;
  while ((pos = result.indexOf("\"sid\"", pos)) >= 0) {
    int colonPos = result.indexOf(':', pos + 5);
    if (colonPos < 0) break;
    
    // Skip optional space after colon
    int quoteStart = colonPos + 1;
    while (quoteStart < result.length() && result[quoteStart] == ' ') quoteStart++;
    
    // Find opening quote
    if (quoteStart >= result.length() || result[quoteStart] != '"') {
      pos = colonPos + 1;
      continue;
    }
    
    // Find closing quote
    int quoteEnd = result.indexOf('"', quoteStart + 1);
    if (quoteEnd < 0) break;
    
    // Keep first 8 chars, redact the rest
    String sidValue = result.substring(quoteStart + 1, quoteEnd);
    String redacted;
    if (sidValue.length() > 8) {
      redacted = sidValue.substring(0, 8) + "...";
    } else {
      redacted = "***";
    }
    
    result = result.substring(0, quoteStart + 1) + redacted + result.substring(quoteEnd);
    pos = quoteStart + redacted.length() + 2;
  }
  
  return result;
}

// =========================================================================
// System Diagnostics Command Implementations
// =========================================================================

const char* cmd_temperature(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  // ESP32 internal temperature sensor
  float tempC = temperatureRead();
  float tempF = (tempC * 9.0 / 5.0) + 32.0;

  snprintf(getDebugBuffer(), 1024, "ESP32 Internal Temperature:\n  %.1f°C (%.1f°F)", tempC, tempF);
  return getDebugBuffer();
}

const char* cmd_voltage(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // ESP32 doesn't have built-in VCC measurement like ESP8266
  // We can read from an ADC pin if voltage divider is connected
  // For now, provide system power estimates based on operation

  broadcastOutput("Power Supply Information:");
  broadcastOutput("========================");

  // Estimate power consumption based on active components
  float estimatedCurrent = 80;  // Base ESP32 current in mA

  if (WiFi.isConnected()) {
    estimatedCurrent += 120;  // WiFi active
    broadcastOutput("WiFi: Active (+120mA)");
  } else {
    broadcastOutput("WiFi: Inactive");
  }

  if (thermalConnected && thermalEnabled) {
    estimatedCurrent += 23;  // MLX90640 typical
    broadcastOutput("Thermal Sensor: Active (+23mA)");
  }

  if (imuConnected && imuEnabled) {
    estimatedCurrent += 12;  // BNO055 typical
    broadcastOutput("IMU Sensor: Active (+12mA)");
  }

  if (tofConnected && tofEnabled) {
    estimatedCurrent += 20;  // VL53L4CX typical
    broadcastOutput("ToF Sensor: Active (+20mA)");
  }

  if (apdsConnected) {
    estimatedCurrent += 3;  // APDS9960 typical
    broadcastOutput("APDS Sensor: Active (+3mA)");
  }

  broadcastOutput("");
  BROADCAST_PRINTF("Estimated Current Draw: %.0fmA", estimatedCurrent);
  BROADCAST_PRINTF("Estimated Power (3.3V): %.2fW", (estimatedCurrent * 3.3) / 1000.0);
  broadcastOutput("");
  broadcastOutput("Note: Direct voltage measurement requires external ADC connection");

  return "[System] Voltage info displayed";
}

const char* cmd_cpufreq(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsIn;
  args.trim();

  uint32_t currentFreq = getCpuFrequencyMhz();

  if (args.length() == 0) {
    // Get current frequency
    broadcastOutput("CPU Frequency:");
    BROADCAST_PRINTF("  Current: %lu MHz", (unsigned long)currentFreq);
    BROADCAST_PRINTF("  XTAL: %lu MHz", (unsigned long)getXtalFrequencyMhz());
    BROADCAST_PRINTF("  APB: %lu MHz", (unsigned long)(getApbFrequency() / 1000000UL));
    return "[System] CPU frequency displayed";
  } else {
    // Set frequency (admin only for safety)

    uint32_t newFreq = args.toInt();
    if (newFreq != 80 && newFreq != 160 && newFreq != 240) {
      return "Error: Frequency must be 80, 160, or 240 MHz";
    }

    setCpuFrequencyMhz(newFreq);
    BROADCAST_PRINTF("CPU frequency set to %lu MHz", (unsigned long)newFreq);
    return "[System] CPU frequency updated";
  }
}

// =========================================================================
// Light Sleep Command
// =========================================================================

#include <esp_sleep.h>
#include "OLED_Display.h"

const char* cmd_lightsleep(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse optional duration (default 20 seconds)
  int seconds = 20;
  String arg = args;
  arg.trim();
  if (arg.length() > 0) {
    int val = arg.toInt();
    if (val > 0 && val <= 3600) {
      seconds = val;
    }
  }
  
  BROADCAST_PRINTF("Entering light sleep for %d seconds...", seconds);
  delay(100);  // Allow message to be sent
  
  // Show sleep message and turn off display (uses abstracted functions)
  oledShowSleepScreen(seconds);
  delay(500);
  oledDisplayOff();
  
  // Configure wake-up source: timer
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  
  // Enter light sleep (preserves RAM, resumes here when woken)
  esp_light_sleep_start();
  
  // Execution resumes here after wake-up
  DEBUG_SYSTEMF("Woke from light sleep!");
  
  // Turn display back on (uses abstracted function)
  oledDisplayOn();
  
  return "Woke from light sleep";
}

// =========================================================================
// Core System Commands (moved from .ino)
// =========================================================================

const char* cmd_status(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  broadcastOutput("System Status:");
#if ENABLE_WIFI
  BROADCAST_PRINTF("  WiFi: %s", WiFi.isConnected() ? "Connected" : "Disconnected");
  BROADCAST_PRINTF("  IP: %s", WiFi.localIP().toString().c_str());
#else
  BROADCAST_PRINTF("  WiFi: Disabled");
#endif
  BROADCAST_PRINTF("  Filesystem: %s", filesystemReady ? "Ready" : "Error");
  BROADCAST_PRINTF("  Free Heap: %lu bytes", (unsigned long)ESP.getFreeHeap());

  size_t psTot = ESP.getPsramSize();
  if (psTot > 0) {
    BROADCAST_PRINTF("  Free PSRAM: %lu bytes", (unsigned long)ESP.getFreePsram());
    BROADCAST_PRINTF("  Total PSRAM: %lu bytes", (unsigned long)psTot);
  }

  return "OK";
}

const char* cmd_uptime(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  unsigned long uptimeMs = millis();
  unsigned long seconds = uptimeMs / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  BROADCAST_PRINTF("Uptime: %luh %lum %lus", hours, minutes % 60, seconds % 60);
  return "[System] Uptime displayed";
}

const char* cmd_time(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Show uptime in milliseconds
  unsigned long uptimeMs = millis();
  BROADCAST_PRINTF("Uptime: %lu ms", uptimeMs);
  
  // Priority: RTC (primary) -> NTP (fallback)
#if ENABLE_RTC_SENSOR
  if (rtcEnabled && rtcConnected) {
    // RTC is primary time source
    RTCDateTime dt;
    if (rtcReadDateTime(&dt)) {
      char timeBuf[32];
      snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
               dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
      BROADCAST_PRINTF("Time: %s (RTC)", timeBuf);
      BROADCAST_PRINTF("Temp: %.1f C", rtcReadTemperature());
      return "OK";
    }
  }
#endif

  // Fallback to NTP/system time if RTC not available
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    BROADCAST_PRINTF("Time: %s (NTP)", timeBuf);
  } else {
    broadcastOutput("Time: Not synced (no RTC or NTP)");
  }
  
  return "OK";
}

const char* cmd_timeset(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = cmd;
  arg.trim();
  
  if (arg.length() == 0) {
    return "Usage: timeset YYYY-MM-DD HH:MM:SS  or  timeset <unix_timestamp>";
  }
  
  struct tm timeinfo = {0};
  time_t t;
  
  // Check if it's a unix timestamp (all digits)
  bool isUnix = true;
  for (size_t i = 0; i < arg.length(); i++) {
    if (!isDigit(arg[i])) {
      isUnix = false;
      break;
    }
  }
  
  if (isUnix) {
    t = (time_t)arg.toInt();
    localtime_r(&t, &timeinfo);
  } else {
    // Parse YYYY-MM-DD HH:MM:SS
    int year, month, day, hour, minute, second;
    if (sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", 
               &year, &month, &day, &hour, &minute, &second) != 6) {
      return "Invalid format. Use: YYYY-MM-DD HH:MM:SS or unix timestamp";
    }
    
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon = month - 1;
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;
    timeinfo.tm_isdst = -1;
    t = mktime(&timeinfo);
  }
  
  // Set system time
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  
  // Also update RTC if available
#if ENABLE_RTC_SENSOR
  if (rtcEnabled && rtcConnected) {
    rtcSyncFromSystem();
    broadcastOutput("System time and RTC updated");
    // Mark RTC as calibrated so future boots trust RTC first
    if (!gSettings.rtcTimeHasBeenSet) {
      setSetting(gSettings.rtcTimeHasBeenSet, true);
      broadcastOutput("RTC marked as calibrated for future boots");
    }
  } else {
    broadcastOutput("System time updated (RTC not available)");
  }
#else
  broadcastOutput("System time updated");
#endif
  
  char timeBuf[32];
  strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  BROADCAST_PRINTF("Time set to: %s", timeBuf);
  
  return "OK";
}

extern bool filesystemReady;  // From filesystem.cpp

const char* cmd_fsusage(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) {
    broadcastOutput("Error: LittleFS not ready");
    return "ERROR";
  }

  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;
  unsigned int usagePercent = (usedBytes * 100) / (totalBytes == 0 ? 1 : totalBytes);

  broadcastOutput("Filesystem Usage:");
  BROADCAST_PRINTF("  Total: %lu bytes", (unsigned long)totalBytes);
  BROADCAST_PRINTF("  Used:  %lu bytes", (unsigned long)usedBytes);
  BROADCAST_PRINTF("  Free:  %lu bytes", (unsigned long)freeBytes);
  BROADCAST_PRINTF("  Usage: %u%%", usagePercent);

  return "[System] Filesystem usage displayed";
}

// Forward declarations for encryption/hashing functions (in settings.cpp and user_system.cpp)
extern String encryptWifiPassword(const String& plaintext);
extern String decryptWifiPassword(const String& encrypted);
extern String hashUserPassword(const String& plaintext);
extern bool verifyUserPassword(const String& plaintext, const String& hash);

const char* cmd_testencryption(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsIn;
  args.trim();

  if (args.length() == 0) {
    return "Usage: testencryption <password_to_test>";
  }

  String encrypted = encryptWifiPassword(args);
  String decrypted = decryptWifiPassword(encrypted);

  broadcastOutput("WiFi Password Encryption Test:");
  BROADCAST_PRINTF("Original:  '%s'", args.c_str());
  BROADCAST_PRINTF("Encrypted: '%s'", encrypted.c_str());
  BROADCAST_PRINTF("Decrypted: '%s'", decrypted.c_str());
  BROADCAST_PRINTF("Match: %s", (args == decrypted) ? "YES" : "NO");

  return "[System] Encryption test complete";
}

const char* cmd_testpassword(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsIn;
  args.trim();

  if (args.length() == 0) {
    return "Usage: testpassword <password_to_test>";
  }

  String hashed = hashUserPassword(args);
  bool verified = verifyUserPassword(args, hashed);
  bool wrongVerified = verifyUserPassword("wrongpassword", hashed);

  broadcastOutput("Password Hashing Test:");
  BROADCAST_PRINTF("Original:  '%s'", args.c_str());
  BROADCAST_PRINTF("Hashed:    '%s'", hashed.c_str());
  BROADCAST_PRINTF("Verify Correct: %s", verified ? "YES" : "NO");
  BROADCAST_PRINTF("Verify Wrong:   %s", wrongVerified ? "YES" : "NO");
  BROADCAST_PRINTF("System Status: %s", (verified && !wrongVerified) ? "WORKING" : "ERROR");

  return "[System] Password test complete";
}

const char* cmd_reboot(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  broadcastOutput("Rebooting system...");
  delay(100);  // Allow message to be sent
  ESP.restart();
  return "[System] Rebooting";  // Won't actually return due to restart
}

const char* cmd_broadcast(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String msg = args;
  msg.trim();
  if (msg.length() == 0) return "Usage: broadcast <message>";
  broadcastOutput(msg);
  return "[System] Message broadcast";
}

const char* cmd_wait(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = args;
  val.trim();
  if (val.length() == 0) return "Usage: wait <ms>";
  int ms = val.toInt();
  if (ms > 0 && ms <= 60000) delay(ms);
  return "[System] Wait complete";
}

// =========================================================================
// NTP Time Synchronization (moved from .ino)
// =========================================================================

#if ENABLE_WIFI

extern Settings gSettings;
extern void resolvePendingUserCreationTimes();
extern void notifyAutomationScheduler();
extern void logTimeSyncedMarkerIfReady();

void setupNTP() {
  long gmtOffset = (long)gSettings.tzOffsetMinutes * 60;  // seconds
  DEBUG_DATETIMEF("[NTP Setup] Starting NTP configuration");
  DEBUG_DATETIMEF("[NTP Setup] Primary server: %s", gSettings.ntpServer.c_str());
  DEBUG_DATETIMEF("[NTP Setup] GMT offset: %ld seconds (%d minutes)", gmtOffset, gSettings.tzOffsetMinutes);
  DEBUG_DATETIMEF("[NTP Setup] WiFi status: %s", WiFi.isConnected() ? "CONNECTED" : "DISCONNECTED");
  if (WiFi.isConnected()) {
    DEBUG_DATETIMEF("[NTP Setup] WiFi IP: %s", WiFi.localIP().toString().c_str());
    DEBUG_DATETIMEF("[NTP Setup] WiFi gateway: %s", WiFi.gatewayIP().toString().c_str());
    DEBUG_DATETIMEF("[NTP Setup] WiFi DNS: %s", WiFi.dnsIP().toString().c_str());
    DEBUG_DATETIMEF("[NTP Setup] WiFi subnet: %s", WiFi.subnetMask().toString().c_str());
  }

  // Use standard configTime() with hostname-based NTP servers
  DEBUG_DATETIMEF("[NTP Setup] Configuring NTP with hostname-based servers");

  // Try multiple reliable NTP servers for redundancy
  configTime(gmtOffset, 0,
             gSettings.ntpServer.c_str(),  // Primary (usually pool.ntp.org)
             "time.google.com",            // Google
             "time.cloudflare.com");       // Cloudflare

  DEBUG_DATETIMEF("[NTP Setup] configTime() completed with servers:");
  DEBUG_DATETIMEF("[NTP Setup]   Primary: %s", gSettings.ntpServer.c_str());
  DEBUG_DATETIMEF("[NTP Setup]   Backup1: time.google.com");
  DEBUG_DATETIMEF("[NTP Setup]   Backup2: time.cloudflare.com");
}

bool syncNTPAndResolve() {
  DEBUG_DATETIMEF("[syncNTPAndResolve] Starting NTP sync process");

  if (!WiFi.isConnected()) {
    DEBUG_DATETIMEF("[syncNTPAndResolve] FAILED - WiFi not connected");
    broadcastOutput("NTP sync requires WiFi connection");
    return false;
  }

  DEBUG_DATETIMEF("[syncNTPAndResolve] WiFi connected, proceeding with NTP sync");

  // Wait for DNS to be ready after WiFi connection
  DEBUG_DATETIMEF("[syncNTPAndResolve] Waiting 500ms for DNS initialization...");
  delay(500);

  // Test DNS resolution before attempting NTP
  IPAddress testIP;
  bool dnsWorking = WiFi.hostByName("time.google.com", testIP);
  bool validIP = dnsWorking && testIP != IPAddress(0, 0, 0, 0);
  DEBUG_DATETIMEF("[syncNTPAndResolve] DNS test: hostByName('time.google.com') = %s, IP=%s",
                  validIP ? "SUCCESS" : "FAILED",
                  testIP.toString().c_str());

  if (!validIP) {
    DEBUG_DATETIMEF("[syncNTPAndResolve] WARNING: DNS resolution failed (returned %s), NTP may not work",
                    testIP.toString().c_str());
    broadcastOutput("⚠ DNS resolution failed - NTP may not work");
    broadcastOutput("  Waiting 2 more seconds for DNS to initialize...");
    delay(2000);
    dnsWorking = WiFi.hostByName("pool.ntp.org", testIP);
    validIP = dnsWorking && testIP != IPAddress(0, 0, 0, 0);
    DEBUG_DATETIMEF("[syncNTPAndResolve] DNS retry: hostByName('pool.ntp.org') = %s, IP=%s",
                    validIP ? "SUCCESS" : "FAILED",
                    testIP.toString().c_str());
    if (!validIP) {
      DEBUG_DATETIMEF("[syncNTPAndResolve] ERROR: DNS still not working after retry");
      broadcastOutput("[ERROR] DNS not working - NTP will fail");
      return false;
    }
  }

  broadcastOutput("Synchronizing time with NTP server...");
  setupNTP();
  broadcastOutput("  Contacting NTP server, please wait...");

  bool ntpSynced = false;
  // SNTP has a startup delay of up to 5 seconds (CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY)
  // plus network round-trip time, so we need to wait longer than that
  const int maxWaitSeconds = 15;
  const int iterationsPerSecond = 5;  // 200ms per iteration
  const int maxIterations = maxWaitSeconds * iterationsPerSecond;
  DEBUG_DATETIMEF("[syncNTPAndResolve] Starting %d-second wait loop for NTP response", maxWaitSeconds);

  for (int i = 0; i < maxIterations && !ntpSynced; i++) {
    delay(200);
    oledUpdate();  // Keep boot animation running during NTP wait

    if (i > 0 && i % iterationsPerSecond == 0) {
      char progressMsg[64];
      snprintf(progressMsg, sizeof(progressMsg), "  Looking for updates... %d/%d seconds", i / iterationsPerSecond, maxWaitSeconds);
      broadcastOutput(progressMsg);
      DEBUG_DATETIMEF("[syncNTPAndResolve] Waiting... %d/%d seconds elapsed", i / iterationsPerSecond, maxWaitSeconds);
    }

    time_t now = time(nullptr);
    DEBUG_DATETIMEF("[syncNTPAndResolve] time(nullptr) returned: %lu", (unsigned long)now);

    struct tm timeinfo;
    bool gotLocalTime = getLocalTime(&timeinfo, 10);  // 10ms timeout
    DEBUG_DATETIMEF("[syncNTPAndResolve] getLocalTime(10ms) returned: %s", gotLocalTime ? "true" : "false");

    if (gotLocalTime) {
      DEBUG_DATETIMEF("[syncNTPAndResolve] SUCCESS! Time synced: %04d-%02d-%02d %02d:%02d:%02d",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      logTimeSyncedMarkerIfReady();
      ntpSynced = true;
      break;
    }
  }

  if (ntpSynced) {
    DEBUG_DATETIMEF("[syncNTPAndResolve] NTP sync completed successfully");
    broadcastOutput("[OK] NTP time synchronized successfully");
    
    // Sync RTC from NTP time to keep RTC accurate
#if ENABLE_RTC_SENSOR
    if (rtcEnabled && rtcConnected) {
      if (rtcSyncFromSystem()) {
        broadcastOutput("[OK] RTC updated from NTP time");
        // Mark RTC as calibrated so future boots trust RTC first
        if (!gSettings.rtcTimeHasBeenSet) {
          setSetting(gSettings.rtcTimeHasBeenSet, true);
          broadcastOutput("[OK] RTC marked as calibrated for future boots");
        }
      }
    }
#endif
    
    DEBUG_SYSTEMF("About to call resolvePendingUserCreationTimes");
    resolvePendingUserCreationTimes();
    DEBUG_SYSTEMF("resolvePendingUserCreationTimes completed");
    DEBUG_SYSTEMF("About to call notifyAutomationScheduler");
    notifyAutomationScheduler();
    DEBUG_SYSTEMF("notifyAutomationScheduler completed");
    return true;
  } else {
    DEBUG_DATETIMEF("[syncNTPAndResolve] TIMEOUT - NTP sync failed after %d seconds", maxWaitSeconds);
    DEBUG_DATETIMEF("[syncNTPAndResolve] Check: WiFi=%s, DNS=%s, Gateway=%s",
                    WiFi.isConnected() ? "OK" : "FAIL",
                    WiFi.dnsIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str());
    
    // Try RTC as fallback time source
#if ENABLE_RTC_SENSOR
    if (rtcEnabled && rtcConnected) {
      if (rtcSyncToSystem()) {
        broadcastOutput("[OK] System time set from RTC (NTP unavailable)");
        resolvePendingUserCreationTimes();
        notifyAutomationScheduler();
        return true;
      }
    }
#endif
    
    broadcastOutput("[ERROR] NTP sync timeout - no RTC available");
    broadcastOutput("  Note: Your router may be blocking NTP (UDP port 123)");
    return false;
  }
}

#endif // ENABLE_WIFI

time_t nowEpoch() {
  return time(nullptr);
}

// =========================================================================
// Command Registry System
// =========================================================================

// Command handler forward declarations (implemented in .ino)
// cmd_status, cmd_uptime, cmd_fsusage now implemented above
// cmd_testencryption, cmd_testpassword now implemented above
// cmd_temperature, cmd_voltage, cmd_cpufreq now implemented above
// cmd_reboot, cmd_broadcast, cmd_wait now implemented above
extern const char* cmd_pending_list(const String& cmd);
// cmd_lightsleep now implemented above

// Main/Core command registry (commands that remain in main .ino file)
// Most commands have been modularized - this contains only core system commands
const CommandEntry commands[] = {
  // ---- Core / General ----
  { "status", "Show system status (WiFi, FS, memory).", false, cmd_status, nullptr, "system", "status" },
  { "uptime", "Show device uptime.", false, cmd_uptime },
  { "time", "Show device time (uptime + NTP if synced).", false, cmd_time },
  { "timeset", "Set time manually: timeset YYYY-MM-DD HH:MM:SS or <unix_timestamp>.", false, cmd_timeset },
  { "memsample", "Memory snapshot with component requirements. Use 'memsample track [on|off|reset|status]' for allocation tracking.", false, cmd_memsample },
  { "memreport", "Comprehensive memory report (Task Manager style).", false, cmd_memreport },
  { "fsusage", "Show filesystem usage.", false, cmd_fsusage },
  
  // ---- Testing Commands (Admin Only) ----
  { "testencryption", "Test WiFi password encryption (admin only).", true, cmd_testencryption },
  { "testpassword", "Test user password hashing (admin only).", true, cmd_testpassword },

  // ---- System Diagnostics ----
  { "temperature", "Read ESP32 internal temperature.", false, cmd_temperature },
  { "voltage", "Read supply voltage.", false, cmd_voltage },
  { "cpufreq", "Get/set CPU frequency.", false, cmd_cpufreq },
  { "taskstats", "Detailed task statistics.", false, cmd_taskstats },

  // ---- Misc ----
  { "reboot", "Reboot the system.", true, cmd_reboot, nullptr, "system", "reboot" },
  { "broadcast", "Send message to all or specific user.", true, cmd_broadcast },
  { "pending list", "List pending user requests.", true, cmd_pending_list },
  { "wait", "Delay execution for N milliseconds: wait <ms>.", false, cmd_wait },
  { "sleep", "Alias for wait: sleep <ms>.", false, cmd_wait },
  { "lightsleep", "Enter ESP32 light sleep: lightsleep [seconds] (default 20s).", false, cmd_lightsleep },
};

const size_t commandsCount = sizeof(commands) / sizeof(commands[0]);

// Auto-register with command system
static CommandModuleRegistrar _core_cmd_registrar(commands, commandsCount, "core");

// Battery commands (from System_Battery.cpp)
#if ENABLE_BATTERY_MONITOR
extern const char* cmd_battery_status(const String& args);
extern const char* cmd_battery_calibrate(const String& args);

const CommandEntry batteryCommands[] = {
  {"battery status", "Show battery voltage, charge level, and status", false, cmd_battery_status, nullptr, "battery", "status"},
  {"battery calibrate", "Recalibrate battery ADC readings", false, cmd_battery_calibrate}
};

const size_t batteryCommandsCount = sizeof(batteryCommands) / sizeof(batteryCommands[0]);
static CommandModuleRegistrar _battery_cmd_registrar(batteryCommands, batteryCommandsCount, "battery");
#endif

// MQTT commands (from System_MQTT.cpp)
#if ENABLE_MQTT
extern const CommandEntry mqttCommands[];
extern const size_t mqttCommandsCount;
#endif

// Module registry - collects all command tables from modules
// Now includes metadata: description, flags, isConnected callback
// Order matters for help display; longest-match search handles conflicts
static const CommandModule gCommandModules[] = {
  { "cli",        "Help and CLI navigation", cliCommands,          cliCommandsCount, CMD_MODULE_CORE, nullptr },
  { "core",       "Core system commands", commands,             commandsCount, CMD_MODULE_CORE, nullptr },
#if ENABLE_WIFI
  { "wifi",       "Network management (connect, scan, add/remove networks)", wifiCommands,         wifiCommandsCount, CMD_MODULE_NETWORK, nullptr },
#endif
#if ENABLE_ESPNOW
  { "espnow",     "ESP-NOW wireless communication (peer-to-peer, mesh)", espNowCommands,       espNowCommandsCount, CMD_MODULE_NETWORK, nullptr },
#endif
#if ENABLE_MQTT
  { "mqtt",       "MQTT broker connection for Home Assistant", mqttCommands,         mqttCommandsCount, CMD_MODULE_NETWORK, nullptr },
#endif
  #if ENABLE_BLUETOOTH
  { "bluetooth",  "Bluetooth LE control and status", bluetoothCommands, bluetoothCommandsCount, CMD_MODULE_NETWORK, nullptr },
  #endif
  { "filesystem", "File operations and storage management", filesystemCommands,   filesystemCommandsCount, 0, nullptr },
#if defined(SD_CS_PIN)
  { "sd",         "SD card mount, format, and info", sdCommands,           sdCommandsCount, 0, []() { return VFS::isSDAvailable(); } },
#endif
  { "oled",       "OLED display control and graphics", oledCommands,         oledCommandsCount, 0, nullptr },
  { "neopixel",   "RGB LED strip and effects", neopixelCommands,     neopixelCommandsCount, 0, nullptr },
#if ENABLE_SERVO
  { "servo",      "PCA9685 servo motor control", servoCommands,        servoCommandsCount, 0, nullptr },
#endif
#if ENABLE_THERMAL_SENSOR
  { "thermal",    "MLX90640 thermal camera (32x24)", thermalCommands,      thermalCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("thermal"); } },
#endif
#if ENABLE_TOF_SENSOR
  { "tof",        "VL53L4CX time-of-flight distance sensor", tofCommands,          tofCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("tof"); } },
#endif
#if ENABLE_IMU_SENSOR
  { "imu",        "BNO055 9-DOF orientation sensor", imuCommands,          imuCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("imu"); } },
#endif
#if ENABLE_GAMEPAD_SENSOR
  { "gamepad",    "Seesaw gamepad controller", gamepadCommands,      gamepadCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("gamepad"); } },
#endif
#if ENABLE_APDS_SENSOR
  { "apds",       "APDS9960 color, proximity, gesture sensor", apdsCommands,         apdsCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("apds"); } },
#endif
#if ENABLE_GPS_SENSOR
  { "gps",        "PA1010D GPS module", gpsCommands,          gpsCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("gps"); } },
#endif
#if ENABLE_FM_RADIO
  { "fmradio",    "RDA5807 FM radio receiver", fmRadioCommands,      fmRadioCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("fmradio"); } },
#endif
#if ENABLE_RTC_SENSOR
  { "rtc",        "DS3231 precision RTC", rtcCommands,          rtcCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("rtc"); } },
#endif
#if ENABLE_PRESENCE_SENSOR
  { "presence",   "STHS34PF80 IR presence/motion sensor", presenceCommands,     presenceCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("presence"); } },
#endif
#if ENABLE_CAMERA_SENSOR
  { "camera",     "ESP32-S3 DVP camera sensor", cameraCommands,       cameraCommandsCount, CMD_MODULE_SENSOR, []() { return cameraConnected; } },
#endif
#if ENABLE_MICROPHONE_SENSOR
  { "microphone", "PDM microphone audio sensor", micCommands,          micCommandsCount, CMD_MODULE_SENSOR, []() { return micConnected; } },
#endif
#if ENABLE_EDGE_IMPULSE
  { "edgeimpulse", "Edge Impulse ML inference", edgeImpulseCommands,  edgeImpulseCommandsCount, CMD_MODULE_SENSOR, nullptr },
#endif

 #if ENABLE_ESP_SR
   { "espsr", "ESP-SR speech recognition", espsrCommands,  espsrCommandsCount, CMD_MODULE_SENSOR, nullptr },
 #endif
  { "i2c",        "I2C bus diagnostics and scanning", i2cCommands,          i2cCommandsCount, 0, nullptr },
#if ENABLE_AUTOMATION
  { "automation", "Scheduled tasks and conditional commands", automationCommands,   automationCommandsCount, 0, nullptr },
#endif
#if ENABLE_BATTERY_MONITOR
  { "battery",    "Battery voltage and charge monitoring", batteryCommands,      batteryCommandsCount, 0, nullptr },
#endif
  { "debug",      "System debugging and diagnostics", debugCommands,        debugCommandsCount, 0, nullptr },
  { "settings",   "Device configuration and preferences", settingsCommands,     settingsCommandsCount, 0, nullptr },
  { "sensorlog", "Sensor data logging to files", sensorLoggingCommands, sensorLoggingCommandsCount, 0, nullptr },
  { "users",      "User authentication and management", userSystemCommands,         userSystemCommandsCount, CMD_MODULE_ADMIN, nullptr },
 };
static const size_t gCommandModulesCount = sizeof(gCommandModules) / sizeof(gCommandModules[0]);

const CommandModule* getCommandModules(size_t& count) {
  count = gCommandModulesCount;
  return gCommandModules;
}

// Note: findCommand() is now defined in command_system.cpp

// Check if command requires admin
bool commandRequiresAdmin(const String& cmdLine) {
  const CommandEntry* entry = findCommand(cmdLine);
  return entry ? entry->requiresAdmin : false;
}

// Dispatch command to handler (simple version without auth context)
// The full executeCommand() in .ino handles auth, logging, help mode, etc.
const char* dispatchCommand(const String& cmd) {
  const CommandEntry* entry = findCommand(cmd);
  if (!entry) {
    return "Unknown command";
  }
  return entry->handler(cmd);
}

// ============================================================================
// Memory Reporting Functions
// ============================================================================

// External dependencies for device registry
extern int connectedDeviceCount;
extern ConnectedDevice connectedDevices[];
extern const I2CSensorEntry i2cSensors[];
extern const size_t i2cSensorsCount;

// Print detailed breakdown of sensor libraries from device registry
// Returns total bytes via outTotal parameter
static bool isCompiledModuleName(const char* moduleName) {
  if (!moduleName) return true;

  if (strcmp(moduleName, "thermal") == 0) {
#if ENABLE_THERMAL_SENSOR
    return true;
#else
    return false;
#endif
  }
  if (strcmp(moduleName, "tof") == 0) {
#if ENABLE_TOF_SENSOR
    return true;
#else
    return false;
#endif
  }
  if (strcmp(moduleName, "imu") == 0) {
#if ENABLE_IMU_SENSOR
    return true;
#else
    return false;
#endif
  }
  if (strcmp(moduleName, "gamepad") == 0) {
#if ENABLE_GAMEPAD_SENSOR
    return true;
#else
    return false;
#endif
  }
  if (strcmp(moduleName, "apds") == 0) {
#if ENABLE_APDS_SENSOR
    return true;
#else
    return false;
#endif
  }
  if (strcmp(moduleName, "gps") == 0) {
#if ENABLE_GPS_SENSOR
    return true;
#else
    return false;
#endif
  }
  if (strcmp(moduleName, "oled") == 0) {
#if ENABLE_OLED_DISPLAY
    return true;
#else
    return false;
#endif
  }

  // Unknown module name: assume compiled (fail-open so report still works)
  return true;
}

void printConnectedDevicesLibraries(size_t& outTotal) {
  // Track which libraries we've printed
  const char* printedLibraries[50];  // MAX_CONNECTED_DEVICES from .ino
  int printedCount = 0;
  outTotal = 0;

  for (int i = 0; i < connectedDeviceCount; i++) {
    if (!connectedDevices[i].isConnected) continue;

    // Find device in known sensors database
    for (size_t j = 0; j < i2cSensorsCount; j++) {
      if (i2cSensors[j].address == connectedDevices[i].address && 
          strcmp(i2cSensors[j].name, connectedDevices[i].name) == 0) {

        // Skip if no library
        if (!i2cSensors[j].libraryName || i2cSensors[j].libraryHeapBytes == 0) break;

        // If the hardware is detected but the module is not compiled in, do not
        // attribute library memory. This keeps the report truthful under the
        // subdirectory-based modular build.
        if (!isCompiledModuleName(i2cSensors[j].moduleName)) {
          break;
        }

        // Check if already printed
        bool alreadyPrinted = false;
        for (int k = 0; k < printedCount; k++) {
          if (strcmp(printedLibraries[k], i2cSensors[j].libraryName) == 0) {
            alreadyPrinted = true;
            break;
          }
        }

        if (!alreadyPrinted) {
          BROADCAST_PRINTF("  - %-25s: %5u bytes",
                          i2cSensors[j].libraryName, (unsigned)i2cSensors[j].libraryHeapBytes);
          printedLibraries[printedCount++] = i2cSensors[j].libraryName;
          outTotal += i2cSensors[j].libraryHeapBytes;
        }
        break;
      }
    }
  }
}

// Calculate total estimated memory for sensor systems
size_t calculateSensorSystemMemory() {
  size_t total = 0;

    // Add up all known sensors heap usage (use libraryHeapBytes as estimate)
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (!connectedDevices[i].isConnected) continue;

    for (size_t j = 0; j < i2cSensorsCount; j++) {
      if (i2cSensors[j].address == connectedDevices[i].address && 
          strcmp(i2cSensors[j].name, connectedDevices[i].name) == 0) {
        if (isCompiledModuleName(i2cSensors[j].moduleName)) {
          total += i2cSensors[j].libraryHeapBytes;
        }
        break;
      }
    }
  }
  
  return total;
}

// ============================================================================
// System Diagnostic Commands
// ============================================================================

// Allocation tracking (struct defined here to avoid incomplete-type issues)
struct AllocEntry {
  char tag[24];
  size_t totalBytes;
  size_t psramBytes;
  size_t dramBytes;
  uint16_t count;
  bool isActive;
};
extern AllocEntry gAllocTracker[];
extern int gAllocTrackerCount;
extern bool gAllocTrackerEnabled;

// External task watermark globals
extern volatile UBaseType_t gTofWatermarkNow, gTofWatermarkMin;
extern volatile UBaseType_t gIMUWatermarkNow, gIMUWatermarkMin;
extern volatile UBaseType_t gThermalWatermarkNow, gThermalWatermarkMin;

// Command/context types (not in a shared header yet)
enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_AUTOMATION, ORIGIN_SYSTEM };
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

// Async callback type for fire-and-forget command execution
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);

// Exec request structure (same layout as in .ino)
struct ExecReq {
  char line[2048];  // Full size for ESP-NOW chunking
  CommandContext ctx;
  char out[2048];   // Result buffer (2KB)
  SemaphoreHandle_t done;  // NULL for async mode
  bool ok;
  
  // Async callback mode
  ExecAsyncCallback asyncCallback;
  void* asyncUserData;
};

// External memory allocation function
extern void* ps_alloc(size_t size, AllocPref pref, const char* tag);

// Comprehensive memory report - shows what's consuming memory (like Task Manager)
void printMemoryReport() {
  size_t dram_total = ESP.getHeapSize();
  size_t dram_free = ESP.getFreeHeap();
  size_t dram_used = dram_total - dram_free;
  size_t dram_min = ESP.getMinFreeHeap();
  size_t dram_peak_used = dram_total - dram_min;

  bool has_ps = psramFound();
  size_t ps_total = has_ps ? ESP.getPsramSize() : 0;
  size_t ps_free = has_ps ? ESP.getFreePsram() : 0;
  size_t ps_used = ps_total - ps_free;

  size_t bss_internal_bytes = (size_t)(&(_bss_end) - &(_bss_start));
  size_t bss_psram_bytes = (size_t)(&(_ext_ram_bss_end) - &(_ext_ram_bss_start));
  size_t noinit_internal_bytes = (size_t)(&(_noinit_end) - &(_noinit_start));
  size_t noinit_psram_bytes = 0;
  if (((uintptr_t)(&_ext_ram_noinit_start) != 0) && ((uintptr_t)(&_ext_ram_noinit_end) != 0)) {
    noinit_psram_bytes = (size_t)(&(_ext_ram_noinit_end) - &(_ext_ram_noinit_start));
  }

  bool useDynamicTracking = gAllocTrackerEnabled && gAllocTrackerCount > 0;

  broadcastOutput("");
  broadcastOutput("========== BOOT MEMORY REPORT (Task Manager) ==========");
  broadcastOutput("");

  // DRAM Summary
  broadcastOutput("-- DRAM (Internal Heap) --");
  BROADCAST_PRINTF("  Total:      %7lu bytes (%3lu KB)",
                (unsigned long)dram_total, (unsigned long)(dram_total / 1024));
  BROADCAST_PRINTF("  Used:       %7lu bytes (%3lu KB) [%2u%%]",
                (unsigned long)dram_used, (unsigned long)(dram_used / 1024),
                (unsigned)((dram_used * 100) / dram_total));
  BROADCAST_PRINTF("  Free:       %7lu bytes (%3lu KB) [%2u%%]",
                (unsigned long)dram_free, (unsigned long)(dram_free / 1024),
                (unsigned)((dram_free * 100) / dram_total));
  BROADCAST_PRINTF("  Peak Used:  %7lu bytes (%3lu KB) [%2u%%]",
                (unsigned long)dram_peak_used, (unsigned long)(dram_peak_used / 1024),
                (unsigned)((dram_peak_used * 100) / dram_total));

  // PSRAM Summary
  if (has_ps) {
    broadcastOutput("");
    broadcastOutput("-- PSRAM (External) --");
    BROADCAST_PRINTF("  Total:      %7lu bytes (%4lu KB)",
                  (unsigned long)ps_total, (unsigned long)(ps_total / 1024));
    BROADCAST_PRINTF("  Used:       %7lu bytes (%4lu KB) [%2u%%]",
                  (unsigned long)ps_used, (unsigned long)(ps_used / 1024),
                  (unsigned)((ps_used * 100) / ps_total));
    BROADCAST_PRINTF("  Free:       %7lu bytes (%4lu KB) [%2u%%]",
                  (unsigned long)ps_free, (unsigned long)(ps_free / 1024),
                  (unsigned)((ps_free * 100) / ps_total));
  } else {
    broadcastOutput("");
    broadcastOutput("-- PSRAM: Not available --");
  }

  broadcastOutput("");
  BROADCAST_PRINTF("  BSS (Internal): %7lu bytes (%3lu KB)",
                (unsigned long)bss_internal_bytes, (unsigned long)(bss_internal_bytes / 1024));
  BROADCAST_PRINTF("  BSS (PSRAM):    %7lu bytes (%3lu KB)",
                (unsigned long)bss_psram_bytes, (unsigned long)(bss_psram_bytes / 1024));
  BROADCAST_PRINTF("  NOINIT (Int):   %7lu bytes (%3lu KB)",
                (unsigned long)noinit_internal_bytes, (unsigned long)(noinit_internal_bytes / 1024));
  BROADCAST_PRINTF("  NOINIT (PSRAM): %7lu bytes (%3lu KB)",
                (unsigned long)noinit_psram_bytes, (unsigned long)(noinit_psram_bytes / 1024));

  broadcastOutput("");
  broadcastOutput("-- MEMORY BREAKDOWN (Hybrid Tracking) --");

  size_t total_known = 0;
  size_t tracked_total = 0;

  // ========== SECTION 1: Dynamic Allocations (ps_alloc tracked) ==========
  if (useDynamicTracking) {
    broadcastOutput("");
    broadcastOutput("[1] DYNAMIC ALLOCATIONS (ps_alloc tracked):");

    // Sort by size (descending)
    int sorted[64];  // MAX_ALLOC_ENTRIES
    for (int i = 0; i < gAllocTrackerCount; i++) sorted[i] = i;
    for (int i = 0; i < gAllocTrackerCount - 1; i++) {
      for (int j = i + 1; j < gAllocTrackerCount; j++) {
        if (gAllocTracker[sorted[j]].totalBytes > gAllocTracker[sorted[i]].totalBytes) {
          int temp = sorted[i];
          sorted[i] = sorted[j];
          sorted[j] = temp;
        }
      }
    }

    int displayed = 0;
    for (int i = 0; i < gAllocTrackerCount && displayed < 15; i++) {
      int idx = sorted[i];
      if (!gAllocTracker[idx].isActive) continue;

      tracked_total += gAllocTracker[idx].totalBytes;

      // Show actual memory type breakdown
      char location[12];
      if (gAllocTracker[idx].psramBytes > 0 && gAllocTracker[idx].dramBytes > 0) {
        snprintf(location, sizeof(location), "PS+DR");
      } else if (gAllocTracker[idx].psramBytes > 0) {
        snprintf(location, sizeof(location), "PSRAM");
      } else {
        snprintf(location, sizeof(location), "DRAM");
      }

      BROADCAST_PRINTF("  %-20s %6lu bytes (%2ux) %-5s",
                      gAllocTracker[idx].tag,
                      (unsigned long)gAllocTracker[idx].totalBytes,
                      (unsigned)gAllocTracker[idx].count,
                      location);
      displayed++;
    }

    if (displayed < gAllocTrackerCount) {
      BROADCAST_PRINTF("  ... and %d more allocations",
                      gAllocTrackerCount - displayed);
      // Add remaining to total
      for (int i = displayed; i < gAllocTrackerCount; i++) {
        int idx = sorted[i];
        if (gAllocTracker[idx].isActive) {
          tracked_total += gAllocTracker[idx].totalBytes;
        }
      }
    }

    BROADCAST_PRINTF("  Subtotal (tracked): %6lu bytes (%3lu KB)",
                    (unsigned long)tracked_total, (unsigned long)(tracked_total / 1024));
    total_known += tracked_total;
  }

  // ========== SECTION 2: System Components ==========
  broadcastOutput("");
  broadcastOutput("[2] SYSTEM COMPONENTS (not ps_alloc):");

  size_t static_total = 0;

  // Task Stacks
  UBaseType_t taskCount = uxTaskGetNumberOfTasks();
  static TaskStatus_t* taskStatusArray = nullptr;
  static UBaseType_t taskStatusCap = 0;
  if (taskCount > taskStatusCap) {
    if (taskStatusArray) {
      free(taskStatusArray);
      taskStatusArray = nullptr;
      taskStatusCap = 0;
    }
    taskStatusArray = (TaskStatus_t*)ps_alloc(taskCount * sizeof(TaskStatus_t), AllocPref::PreferPSRAM, "memreport.tasks");
    if (taskStatusArray) {
      taskStatusCap = taskCount;
    }
  }
  size_t app_tasks_total = 0;
  size_t system_tasks_total = 0;

  if (taskStatusArray) {
    UBaseType_t actualCount = uxTaskGetSystemState(taskStatusArray, taskCount, NULL);

    // Application tasks we created
    struct {
      const char* name;
      uint32_t words;
    } appTasks[] = {
      { "cmd_exec_task", CMD_EXEC_STACK_WORDS },
      { "sensor_queue_task", SENSOR_QUEUE_STACK_WORDS },
      { "espnow_task", ESPNOW_HB_STACK_WORDS },        // ESP-NOW heartbeat task (mesh processing)
      { "thermal_task", THERMAL_STACK_WORDS },
      { "imu_task", IMU_STACK_WORDS },
      { "tof_task", TOF_STACK_WORDS },
      { "gamepad_task", GAMEPAD_STACK_WORDS },
      { "debug_out", DEBUG_OUT_STACK_WORDS },        // Debug output queue processor
      { "apds_task", APDS_STACK_WORDS },             // APDS color/proximity/gesture sensor
      { "gps_task", GPS_STACK_WORDS },               // GPS polling task
    };

    broadcastOutput("  Application Task Stacks:");

    // First pass: show application tasks
    for (UBaseType_t i = 0; i < actualCount; i++) {
      bool isAppTask = false;
      uint32_t allocatedWords = 0;

      for (size_t j = 0; j < sizeof(appTasks) / sizeof(appTasks[0]); j++) {
        if (strcmp(taskStatusArray[i].pcTaskName, appTasks[j].name) == 0) {
          isAppTask = true;
          allocatedWords = appTasks[j].words;
          break;
        }
      }

      if (isAppTask) {
        size_t allocatedBytes = allocatedWords * 4;
        size_t freeBytes = taskStatusArray[i].usStackHighWaterMark * 4;
        size_t usedBytes = allocatedBytes - freeBytes;
        app_tasks_total += allocatedBytes;

        BROADCAST_PRINTF("    %-20s %5lu / %5lu bytes (%2u%% used)",
                        taskStatusArray[i].pcTaskName,
                        (unsigned long)usedBytes,
                        (unsigned long)allocatedBytes,
                        (unsigned)((usedBytes * 100) / allocatedBytes));
      }
    }

    BROADCAST_PRINTF("  Subtotal (app): %6lu bytes (%3lu KB)",
                    (unsigned long)app_tasks_total, (unsigned long)(app_tasks_total / 1024));
    static_total += app_tasks_total;

    // Second pass: show system tasks
    broadcastOutput("");
    broadcastOutput("  System Task Stacks:");

    for (UBaseType_t i = 0; i < actualCount; i++) {
      bool isSystemTask = true;

      // Check if it's an app task
      for (size_t j = 0; j < sizeof(appTasks) / sizeof(appTasks[0]); j++) {
        if (strcmp(taskStatusArray[i].pcTaskName, appTasks[j].name) == 0) {
          isSystemTask = false;
          break;
        }
      }

      if (isSystemTask) {
        size_t freeBytes = taskStatusArray[i].usStackHighWaterMark * 4;
        BROADCAST_PRINTF("    %-20s HWM: %5lu bytes",
                        taskStatusArray[i].pcTaskName,
                        (unsigned long)freeBytes);
      }
    }

  }

  // WiFi driver estimate
  size_t wifi_estimate = 32 * 1024;  // WiFi driver ~ 32KB
  BROADCAST_PRINTF("  WiFi Driver:   ~ %6lu bytes (%2lu KB)",
                  (unsigned long)wifi_estimate, (unsigned long)(wifi_estimate / 1024));
  static_total += wifi_estimate;

  // LVGL estimate
  size_t lvgl_estimate = 0;
  BROADCAST_PRINTF("  UI Framework:  ~ %6lu bytes (%2lu KB) (untracked)",
                  (unsigned long)lvgl_estimate, (unsigned long)(lvgl_estimate / 1024));

  // FreeRTOS estimate
  size_t freertos_estimate = 8 * 1024;  // FreeRTOS ~ 8KB
  BROADCAST_PRINTF("  FreeRTOS:      ~ %6lu bytes (%2lu KB)",
                  (unsigned long)freertos_estimate, (unsigned long)(freertos_estimate / 1024));
  static_total += freertos_estimate;

  BROADCAST_PRINTF("  Subtotal (static): %6lu bytes (%3lu KB)",
                  (unsigned long)static_total, (unsigned long)(static_total / 1024));
  total_known += static_total;

  // ========== SECTION 3: STATIC VARIABLES BY MODULE ==========
  broadcastOutput("");
  broadcastOutput("[3] STATIC VARIABLES BY MODULE:");
  
  size_t static_vars_total = 0;
  
  // First-Time Setup State Management
  broadcastOutput("  First-Time Setup State:");
  broadcastOutput("    gFirstTimeSetupState:        4 bytes");
  broadcastOutput("    gSetupProgressStage:         4 bytes");
  broadcastOutput("    gFirstTimeSetupPerformed:    1 bytes");
  static_vars_total += 9;
  
  // Sensor Module State Variables
  broadcastOutput("  Sensor Modules (Global State):");
  size_t thermal_state_bytes = sizeof(gThermalCache) + sizeof(thermalEnabled) + sizeof(thermalConnected) + sizeof(thermalTaskHandle);
  size_t imu_state_bytes = sizeof(gImuCache) + sizeof(imuEnabled) + sizeof(imuConnected) + sizeof(imuTaskHandle);
  size_t tof_state_bytes = sizeof(gTofCache) + sizeof(tofEnabled) + sizeof(tofConnected) + sizeof(tofTaskHandle);
  size_t gamepad_state_bytes = sizeof(gControlCache) + sizeof(gamepadEnabled) + sizeof(gamepadConnected) + sizeof(gamepadTaskHandle);
  size_t apds_state_bytes = sizeof(gPeripheralCache) + sizeof(apdsConnected) + sizeof(apdsColorEnabled) + sizeof(apdsProximityEnabled) + sizeof(apdsGestureEnabled);
  size_t gps_state_bytes = sizeof(gpsEnabled) + sizeof(gpsConnected);
  size_t oled_state_bytes = sizeof(oledEnabled) + sizeof(oledConnected);

#if ENABLE_THERMAL_SENSOR
  BROADCAST_PRINTF("    Thermal Module: %5lu bytes (enabled)", (unsigned long)thermal_state_bytes);
#else
  BROADCAST_PRINTF("    Thermal Module: %5lu bytes (disabled/stub)", (unsigned long)thermal_state_bytes);
#endif

#if ENABLE_TOF_SENSOR
  BROADCAST_PRINTF("    ToF Module:     %5lu bytes (enabled)", (unsigned long)tof_state_bytes);
#else
  BROADCAST_PRINTF("    ToF Module:     %5lu bytes (disabled/stub)", (unsigned long)tof_state_bytes);
#endif

#if ENABLE_IMU_SENSOR
  BROADCAST_PRINTF("    IMU Module:     %5lu bytes (enabled)", (unsigned long)imu_state_bytes);
#else
  BROADCAST_PRINTF("    IMU Module:     %5lu bytes (disabled/stub)", (unsigned long)imu_state_bytes);
#endif

#if ENABLE_GAMEPAD_SENSOR
  BROADCAST_PRINTF("    Gamepad Module: %5lu bytes (enabled)", (unsigned long)gamepad_state_bytes);
#else
  BROADCAST_PRINTF("    Gamepad Module: %5lu bytes (disabled/stub)", (unsigned long)gamepad_state_bytes);
#endif

#if ENABLE_APDS_SENSOR
  BROADCAST_PRINTF("    APDS Module:    %5lu bytes (enabled)", (unsigned long)apds_state_bytes);
#else
  BROADCAST_PRINTF("    APDS Module:    %5lu bytes (disabled/stub)", (unsigned long)apds_state_bytes);
#endif

#if ENABLE_GPS_SENSOR
  BROADCAST_PRINTF("    GPS Module:     %5lu bytes (enabled)", (unsigned long)gps_state_bytes);
#else
  BROADCAST_PRINTF("    GPS Module:     %5lu bytes (disabled/stub)", (unsigned long)gps_state_bytes);
#endif

#if ENABLE_OLED_DISPLAY
  BROADCAST_PRINTF("    OLED Module:    %5lu bytes (enabled)", (unsigned long)oled_state_bytes);
#else
  BROADCAST_PRINTF("    OLED Module:    %5lu bytes (disabled/stub)", (unsigned long)oled_state_bytes);
#endif

  static_vars_total += thermal_state_bytes + imu_state_bytes + tof_state_bytes + gamepad_state_bytes + apds_state_bytes + gps_state_bytes + oled_state_bytes;
  
  // I2C System
  broadcastOutput("  I2C System:");
  broadcastOutput("    Clock Stack:        32 bytes");  // Fixed 8-slot array inside I2CDeviceManager
  broadcastOutput("    Mutex Objects:     ~64 bytes");
  static_vars_total += 32 + 64;
  
  // Web System Arrays
#if ENABLE_HTTP_SERVER
  broadcastOutput("  Web System:");
  BROADCAST_PRINTF("    Sessions Array:   %4lu bytes",
                  (unsigned long)(MAX_SESSIONS * sizeof(SessionEntry)));
  BROADCAST_PRINTF("    Logout Reasons:   %4lu bytes",
                  (unsigned long)(MAX_LOGOUT_REASONS * sizeof(LogoutReason)));
  static_vars_total += (MAX_SESSIONS * sizeof(SessionEntry)) + (MAX_LOGOUT_REASONS * sizeof(LogoutReason));
#else
  broadcastOutput("  Web System: (disabled)");
#endif
  
  BROADCAST_PRINTF("  Subtotal (static vars): %6lu bytes (%3lu KB)",
                  (unsigned long)static_vars_total, (unsigned long)(static_vars_total / 1024));
  total_known += static_vars_total;

  // Connected devices
  size_t devices_lib_total = 0;
  printConnectedDevicesLibraries(devices_lib_total);
  BROADCAST_PRINTF("  Device Libraries: %6lu bytes (%3lu KB)",
                  (unsigned long)devices_lib_total, (unsigned long)(devices_lib_total / 1024));
  if (devices_lib_total > 0) {
    total_known += devices_lib_total;
  }

  // Tracked PSRAM usage
  size_t tracked_psram = 0;
  if (useDynamicTracking) {
    for (int i = 0; i < gAllocTrackerCount; i++) {
      if (gAllocTracker[i].isActive) {
        tracked_psram += gAllocTracker[i].psramBytes;
      }
    }
  }

  // ========== SECTION 4: MODULAR SENSOR BUILD CONFIGURATION ==========
  broadcastOutput("");
  broadcastOutput("[4] COMPILE-TIME I2C FEATURE LEVEL:");
#if I2C_FEATURE_LEVEL == I2C_LEVEL_DISABLED
  broadcastOutput("  Level: DISABLED (0) - No I2C code compiled");
#elif I2C_FEATURE_LEVEL == I2C_LEVEL_OLED_ONLY
  broadcastOutput("  Level: OLED_ONLY (1) - OLED only, sensors excluded");
#elif I2C_FEATURE_LEVEL == I2C_LEVEL_STANDALONE
  broadcastOutput("  Level: STANDALONE (2) - OLED + Gamepad");
#elif I2C_FEATURE_LEVEL == I2C_LEVEL_FULL
  broadcastOutput("  Level: FULL (3) - OLED + all sensors compiled in");
#elif I2C_FEATURE_LEVEL == I2C_LEVEL_CUSTOM
  broadcastOutput("  Level: CUSTOM (4) - Individual sensor selection");
#else
  broadcastOutput("  Level: UNKNOWN - Check I2C_FEATURE_LEVEL value");
#endif
  broadcastOutput("  (Change I2C_FEATURE_LEVEL in sensor_config.h to modify)");

  // Count enabled sensors
  int enabled_count = 0;
  int disabled_count = 0;

#if ENABLE_THERMAL_SENSOR
  broadcastOutput("  [Y] THERMAL  | thermalTask() in Sensor_Thermal_MLX90640.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] THERMAL  | Disabled (~20-25KB flash, ~15KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_TOF_SENSOR
  broadcastOutput("  [Y] TOF      | tofTask() in Sensor_ToF_VL53L4CX.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] TOF      | Disabled (~25-30KB flash, ~10KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_IMU_SENSOR
  broadcastOutput("  [Y] IMU      | imuTask() in Sensor_IMU_BNO055.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] IMU      | Disabled (~12-18KB flash, ~8KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_GAMEPAD_SENSOR
  broadcastOutput("  [Y] GAMEPAD  | gamepadTask() in Sensor_Gamepad_Seesaw.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] GAMEPAD  | Disabled (~8-12KB flash, ~6KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_APDS_SENSOR
  broadcastOutput("  [Y] APDS     | apdsTask() in Sensor_APDS_APDS9960.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] APDS     | Disabled (~6-10KB flash, ~4KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_GPS_SENSOR
  broadcastOutput("  [Y] GPS      | gpsTask() in Sensor_GPS_PA1010D.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] GPS      | Disabled (~5-8KB flash, ~4KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_OLED_DISPLAY
  broadcastOutput("  [Y] OLED     | Display driver enabled");
  enabled_count++;
#else
  broadcastOutput("  [N] OLED     | Disabled (~8-12KB flash, ~5KB RAM saved)");
  disabled_count++;
#endif

  BROADCAST_PRINTF("  Summary: %d sensors enabled, %d disabled",
                  enabled_count, disabled_count);

  // ========== TOTALS ==========
  broadcastOutput("");
  broadcastOutput("---------- TOTALS ----------");
  BROADCAST_PRINTF("  TOTAL ACCOUNTED:      %6lu bytes (%3lu KB)",
                  (unsigned long)total_known, (unsigned long)(total_known / 1024));
  BROADCAST_PRINTF("  ACTUAL DRAM USED:     %6lu bytes (%3lu KB)",
                  (unsigned long)dram_used, (unsigned long)(dram_used / 1024));

  if (dram_used > total_known) {
    size_t unaccounted = dram_used - total_known;
    BROADCAST_PRINTF("  Unaccounted DRAM:     %6lu bytes (%3lu KB)",
                    (unsigned long)unaccounted, (unsigned long)(unaccounted / 1024));
    size_t overestimate = 0;  
    if (static_total > unaccounted) {
      overestimate = static_total - unaccounted;
    }
    BROADCAST_PRINTF("  Static Over-Estimate: %6lu bytes (%3lu KB)",
                    (unsigned long)overestimate, (unsigned long)(overestimate / 1024));
    broadcastOutput("  (Static estimates are conservative upper bounds)");
  }

  // Show PSRAM accounting if available
  if (has_ps && useDynamicTracking) {
    broadcastOutput("");
    BROADCAST_PRINTF("  PSRAM ACCOUNTED:      %6lu bytes (%3lu KB)",
                    (unsigned long)tracked_psram, (unsigned long)(tracked_psram / 1024));
    BROADCAST_PRINTF("  ACTUAL PSRAM USED:    %6lu bytes (%3lu KB)",
                    (unsigned long)ps_used, (unsigned long)(ps_used / 1024));
    if (ps_used > tracked_psram) {
      size_t unaccounted_psram = ps_used - tracked_psram;
      BROADCAST_PRINTF("  Unaccounted PSRAM:    %6lu bytes (%3lu KB)",
                      (unsigned long)unaccounted_psram, (unsigned long)(unaccounted_psram / 1024));
    }
  }

  broadcastOutput("");
  broadcastOutput("========== END MEMORY REPORT ==========");
  broadcastOutput("");
}

// Command handlers
const char* cmd_memreport(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  printMemoryReport();
  return "Memory report printed to serial";
}



const char* cmd_taskstats(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  char* p = getDebugBuffer();
  size_t remaining = 1024;

  int n = snprintf(p, remaining, "Task Statistics:\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "=================\n");
  p += n;
  remaining -= n;

  // Get task count
  UBaseType_t taskCount = uxTaskGetNumberOfTasks();
  n = snprintf(p, remaining, "Total Tasks: %u\n\n", taskCount);
  p += n;
  remaining -= n;

  // Allocate memory for task status array
  static TaskStatus_t* taskArray = nullptr;
  static UBaseType_t taskCap = 0;
  if (taskCount > taskCap) {
    if (taskArray) {
      free(taskArray);
      taskArray = nullptr;
      taskCap = 0;
    }
    taskArray = (TaskStatus_t*)ps_alloc(taskCount * sizeof(TaskStatus_t), AllocPref::PreferPSRAM, "taskstats");
    if (taskArray) {
      taskCap = taskCount;
    }
  }
  if (!taskArray) {
    return "Error: Unable to allocate memory for task statistics";
  }

  // Get detailed task information
  UBaseType_t actualCount = uxTaskGetSystemState(taskArray, taskCount, nullptr);

  n = snprintf(p, remaining, "Task Name          State  Prio  Stack\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "================== ===== ===== ======\n");
  p += n;
  remaining -= n;

  for (UBaseType_t i = 0; i < actualCount && remaining > 50; i++) {
    const char* state;
    switch (taskArray[i].eCurrentState) {
      case eRunning: state = "RUN  "; break;
      case eReady: state = "READY"; break;
      case eBlocked: state = "BLOCK"; break;
      case eSuspended: state = "SUSP "; break;
      case eDeleted: state = "DEL  "; break;
      default: state = "UNK  "; break;
    }

    n = snprintf(p, remaining, "%-18.18s %s %4u %5u\n",
                 taskArray[i].pcTaskName, state,
                 (unsigned)taskArray[i].uxCurrentPriority,
                 (unsigned)taskArray[i].usStackHighWaterMark);
    p += n;
    remaining -= n;
  }

  return getDebugBuffer();
}



// ============================================================================
// Command Execution Functions - MIGRATED from main .ino
// ============================================================================
// External dependencies for command execution
extern String gExecUser;
extern bool gExecIsAdmin;
extern AuthContext gExecAuthContext;
extern bool gAutoLogActive;
extern bool gInAutomationContext;
extern String gAutoLogAutomationName;
extern CLIState gCLIState;
extern bool gCLIValidateOnly;
extern QueueHandle_t gCmdExecQ;

// External functions
extern bool handleHelpNavigation(const String& cmd, char* out, size_t outSize);
extern String exitToNormalBanner();
extern String redactCmdForAudit(const String& cmd);
extern bool hasAdminPrivilege(const AuthContext& ctx);
extern bool isAdminUser(const String& user);
extern void logAuthAttempt(bool success, const char* path, const String& user, const String& ip, const String& extra);
extern void setCurrentCommandContext(const CommandContext& ctx);

// Note: resolveRegistryCommandKey() is now defined in command_system.cpp

// Helper function: check if command requires admin privileges
bool adminRequiredForLine(const String& line) {
  // Handle help navigation commands specially (they don't require admin)
  String lc = line;
  lc.toLowerCase();
  lc.trim();
  if (gCLIState != CLI_NORMAL) {
    if (lc == "system" || lc == "wifi" || lc == "automations" || 
        lc == "espnow" || lc == "sensors" || lc == "settings") {
      return false;
    }
  }
  
  // Use centralized commandRequiresAdmin()
  return commandRequiresAdmin(line);
}

// Centralized authorization for a command line and context.
// Returns true if authorized, otherwise writes an error to 'out' and returns false.
static bool authorizeCommand(const AuthContext& ctx, const String& line, char* out, size_t outSize) {
  // Admin-only protection via registry
  if (commandRequiresAdmin(line) && !hasAdminPrivilege(ctx)) {
    // Extract command name for better error message (keep legacy format)
    String cmdStr = line;
    String cmdName = cmdStr;
    int spacePos = cmdStr.indexOf(' ');
    if (spacePos > 0) {
      cmdName = cmdStr.substring(0, spacePos);
    }
    snprintf(out, outSize, "Error: Admin access required for command '%s'. Contact an administrator.", cmdName.c_str());
    logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, String("cmd=") + redactCmdForAudit(line));
    return false;
  }
  return true;
}

// Core command execution with authentication and registry dispatch
bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize) {
  gExecUser = ctx.user;

  // Clear output buffer
  out[0] = '\0';
  gExecIsAdmin = isAdminUser(ctx.user);
  gExecAuthContext = ctx;  // Store full context (includes opaque for ESP-NOW streaming)
  DEBUG_CMD_FLOWF("[execCmd] user=%s ip=%s path=%s cmd=%s", ctx.user.c_str(), ctx.ip.c_str(), ctx.path.c_str(), redactCmdForAudit(String(cmd)).c_str());

  // Centralized authorization (admin-required and future policies)
  if (!authorizeCommand(ctx, String(cmd), out, outSize)) {
    return false;
  }

  // Log command execution if automation logging is active
  if (gAutoLogActive && gInAutomationContext) {
    String cmdMsg = cmd;
    if (gAutoLogAutomationName.length() > 0) {
      cmdMsg = "[" + gAutoLogAutomationName + "] " + String(cmd);
    }
    appendAutoLogEntry("COMMAND", cmdMsg);
  }

  // ===== INLINED REGISTRY LOGIC (eliminates 2 function calls) =====
  String command = cmd;
  command.trim();

  if (command.length() == 0) {
    strncpy(out, "Empty command", outSize - 1);
    out[outSize - 1] = '\0';
    return false;
  }

  // ===== REMOTE COMMAND ROUTING =====
  // Commands prefixed with "remote:" or "@" are sent to paired device
  // This works across ALL interfaces (OLED, web, serial, voice)
  bool isRemoteCommand = false;
  String actualCommand = command;
  
  if (command.startsWith("remote:") || command.startsWith("remote ")) {
    isRemoteCommand = true;
    actualCommand = command.substring(7);  // Remove "remote:" or "remote "
    actualCommand.trim();
  } else if (command.startsWith("@") && command.length() > 1) {
    isRemoteCommand = true;
    actualCommand = command.substring(1);  // Remove "@"
    actualCommand.trim();
  }
  
  if (isRemoteCommand) {
    #if ENABLE_ESPNOW
    extern bool isBondModeOnline();
    extern bool isBondSessionTokenValid();
    extern String buildBondedCommandPayload(const String& command);
    extern bool v3_send_frame(const uint8_t* dstMac, uint8_t type, uint8_t flags, 
                              uint32_t msgId, const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);
    extern uint32_t generateMessageId();
    extern bool parseMacAddress(const String& macStr, uint8_t mac[6]);
    extern Settings gSettings;
    
    if (!isBondModeOnline()) {
      strncpy(out, "Error: Bonded device not online", outSize - 1);
      out[outSize - 1] = '\0';
      return false;
    }
    
    if (!isBondSessionTokenValid()) {
      strncpy(out, "Error: No session token - set matching passphrase on both devices", outSize - 1);
      out[outSize - 1] = '\0';
      return false;
    }
    
    uint8_t peerMac[6];
    if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) {
      strncpy(out, "Error: Invalid bonded peer MAC", outSize - 1);
      out[outSize - 1] = '\0';
      return false;
    }
    
    // Build authenticated command payload: @BOND:<token>:<command>
    String payload = buildBondedCommandPayload(actualCommand);
    if (payload.length() == 0) {
      strncpy(out, "Error: Failed to build command payload", outSize - 1);
      out[outSize - 1] = '\0';
      return false;
    }
    
    uint32_t msgId = generateMessageId();
    bool sent = v3_send_frame(peerMac, 5 /* ESPNOW_V3_TYPE_CMD */, 0x01 /* ACK_REQ */, 
                              msgId, (const uint8_t*)payload.c_str(), payload.length(), 1);
    
    if (sent) {
      snprintf(out, outSize, "Remote command sent: %s", actualCommand.c_str());
      broadcastOutput("[REMOTE] Sent to paired device: " + actualCommand);
      return true;
    } else {
      strncpy(out, "Error: Failed to send remote command", outSize - 1);
      out[outSize - 1] = '\0';
      return false;
    }
    #else
    strncpy(out, "Error: ESP-NOW not enabled", outSize - 1);
    out[outSize - 1] = '\0';
    return false;
    #endif
  }
  
  // Continue with local command execution
  command = actualCommand;

  // Find command handler by case-insensitive prefix match
  String lc = command;
  lc.toLowerCase();

  const CommandEntry* found = nullptr;
  size_t foundLen = 0;

  // In help mode, handle navigation commands using cli_system module
  if (handleHelpNavigation(String(cmd), out, outSize)) {
    return true;
  }

  // Standard command lookup using centralized findCommand() from system_utils.cpp
  // This searches all module registries with longest-match semantics
  if (!found) {
    found = findCommand(command);
    if (found) {
      foundLen = strlen(found->name);
    }
  }

  if (found) {
    // Rebuild normalized command with canonical key + args
    String normalizedCmd = String(found->name);
    if (command.length() > foundLen) {
      String args = command.substring(foundLen);
      args.trim();
      if (args.length() > 0) {
        normalizedCmd += " ";
        normalizedCmd += args;
      }
    }

    // Handle help mode exit for non-help commands
    if (gCLIState != CLI_NORMAL) {
      String cmdName = String(found->name);
      bool isHelpCommand = false;
      
      // Check for core help navigation commands
      if (cmdName.startsWith("help") || cmdName == "back" || cmdName == "exit" || cmdName == "clear") {
        isHelpCommand = true;
      } else {
        // Dynamically check if command matches any registered module name
        size_t moduleCount;
        const CommandModule* modules = getCommandModules(moduleCount);
        for (size_t i = 0; i < moduleCount; i++) {
          if (cmdName == String(modules[i].name)) {
            isHelpCommand = true;
            break;
          }
        }
      }

      if (!isHelpCommand) {
        // Exit help mode first, then execute command
        String exitBanner = exitToNormalBanner();
        broadcastOutput(exitBanner);
        helpSuppressedPrintAndReset();
        // Extract args only (everything after command name)
        String args;
        if (command.length() > foundLen) {
          args = command.substring(foundLen);
          args.trim();
        }
        const char* commandResult = found->handler(args);
        snprintf(out, outSize, "%s", commandResult);

        // Log output if automation logging is active
        if (gAutoLogActive && gInAutomationContext) {
          String logOutput = out;
          if (logOutput.length() > 200) {
            logOutput = logOutput.substring(0, 197) + "...";
          }
          logOutput.replace("\n", " ");
          logOutput.replace("\r", " ");
          appendAutoLogEntry("OUTPUT", logOutput);
        }

        // Command audit logging (always-on)
        bool success = (strncmp(out, "Error", 5) != 0) && (strncmp(out, "ERROR", 5) != 0);
        logCommandExecution(ctx, cmd, success, out);

        logAuthAttempt(true, ctx.path.c_str(), ctx.user, ctx.ip, String("cmd=") + redactCmdForAudit(String(cmd)));
        DEBUG_CMD_FLOWF("[execCmd] out_len=%zu", strlen(out));
        return true;
      }
    }

    // Execute handler - pass only args, not full command
    String args;
    if (command.length() > foundLen) {
      args = command.substring(foundLen);
      args.trim();
    }
    DEBUG_CMD_FLOWF("[registry_exec] executing: %s (args: %s)", normalizedCmd.c_str(), args.c_str());
    DEBUGF(DEBUG_CLI, "[registry_exec] executing: %s (args: %s)", normalizedCmd.c_str(), args.c_str());
    const char* result = found->handler(args);
    strncpy(out, result, outSize - 1);
    out[outSize - 1] = '\0';
    
    // Command audit logging (always-on)
    bool success = (strncmp(out, "Error", 5) != 0) && (strncmp(out, "ERROR", 5) != 0);
    logCommandExecution(ctx, cmd, success, out);
  } else {
    // Command not found
    snprintf(out, outSize, "Unknown command: %s\nType 'help' for available commands", command.c_str());
    
    // Log failed command lookup
    logCommandExecution(ctx, cmd, false, out);
  }
  // ===== END INLINED REGISTRY LOGIC =====

  // Log command output if automation logging is active
  if (gAutoLogActive && gInAutomationContext) {
    // Truncate very long outputs for readability
    String logOutput = out;
    if (logOutput.length() > 200) {
      logOutput = logOutput.substring(0, 197) + "...";
    }
    // Replace newlines with spaces for single-line log format
    logOutput.replace("\n", " ");
    logOutput.replace("\r", " ");
    appendAutoLogEntry("OUTPUT", logOutput);
  }

  // We don't have structured success/failure from registry handlers; assume success for audit purposes
  logAuthAttempt(true, ctx.path.c_str(), ctx.user, ctx.ip, String("cmd=") + redactCmdForAudit(String(cmd)));
  DEBUG_CMD_FLOWF("[execCmd] out_len=%zu", strlen(out));
  return true;
}

// Queued command execution with deadlock avoidance
bool submitAndExecuteSync(const Command& cmd, String& out) {
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] enter: cmd.line.length()=%d", cmd.line.length());
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] cmd.line_first_80='%s'", cmd.line.substring(0, 80).c_str());

  // If executor queue isn't ready (very early boot) fallback to direct call
  if (gCmdExecQ == nullptr) {
    // Allocate output buffer from PSRAM (2KB matches ExecReq.out size)
    char* outBuf = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "cmd.out.direct");
    if (!outBuf) {
      out = "Error: Out of memory for command output";
      return false;
    }
    setCurrentCommandContext(cmd.ctx);
    bool ok = executeCommand((AuthContext&)cmd.ctx.auth, cmd.line.c_str(), outBuf, 2048);
    out = outBuf;
    free(outBuf);
    return ok;
  }

  // Package request
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] ENTRY: cmd.line='%s' len=%d", cmd.line.c_str(), cmd.line.length());
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] cmd.ctx.origin=%d validateOnly=%d", (int)cmd.ctx.origin, cmd.ctx.validateOnly ? 1 : 0);
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] cmd.ctx.auth.user='%s' path='%s'", cmd.ctx.auth.user.c_str(), cmd.ctx.auth.path.c_str());
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] Entered: r=%p cmd.line='%s' cmd.ctx.origin=%d", nullptr, cmd.line.c_str(), (int)cmd.ctx.origin);
  
  // Allocate ExecReq from PSRAM since it's large (8KB+)
  ExecReq* r = (ExecReq*)ps_alloc(sizeof(ExecReq), AllocPref::PreferPSRAM, "cmd.exec.req");
  if (!r) {
    DEBUG_CMD_FLOWF("[submitAndExecuteSync] FAILED to allocate ExecReq (heap=%lu psram=%lu)",
                    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
    Serial.printf("[ERROR] Out of memory - cannot create ExecReq: heap=%lu psram=%lu\n", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
    broadcastOutput("[ERROR] Out of memory - cannot create request");
    return false;
  }
  // Initialize the structure (placement new for C++ objects)
  new (r) ExecReq();
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] ExecReq allocated successfully: r=%p size=%d heap=%lu",
                  r, (int)sizeof(ExecReq), (unsigned long)ESP.getFreeHeap());
  
  // Validate cmd.line before proceeding
  if (cmd.line.length() == 0) {
    DEBUG_CMD_FLOWF("[submitAndExecuteSync] ERROR: Empty command line");
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Empty command");
    return false;
  }
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] Free heap after alloc: %lu bytes", (unsigned long)ESP.getFreeHeap());
  
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] Copying cmd.line to r->line (src='%s' len=%d dst_size=%d)", 
                  cmd.line.c_str(), cmd.line.length(), (int)sizeof(r->line));
  strncpy(r->line, cmd.line.c_str(), sizeof(r->line) - 1);
  r->line[sizeof(r->line) - 1] = '\0';
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] After strncpy: r=%p r->line='%s' len=%d", r, r->line, (int)strlen(r->line));
  
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] Copying cmd.ctx to r->ctx (origin=%d)", (int)cmd.ctx.origin);
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] Before ctx copy: r=%p heap=%lu", r, (unsigned long)ESP.getFreeHeap());
  r->ctx = cmd.ctx;
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] After ctx copy: r=%p heap=%lu", r, (unsigned long)ESP.getFreeHeap());
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] r->ctx.origin=%d r->ctx.auth.user='%s'", (int)r->ctx.origin, r->ctx.auth.user.c_str());
  
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] Creating semaphore for r=%p", r);
  r->done = xSemaphoreCreateBinary();
  if (!r->done) {
    DEBUG_CMD_FLOWF("[submitAndExecuteSync] FAILED to create semaphore (heap=%lu)", (unsigned long)ESP.getFreeHeap());
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Out of memory - cannot create semaphore");
    return false;
  }
  DEBUG_CMD_FLOWF("[submitAndExecuteSync] Semaphore created: r=%p r->done=%p heap=%lu",
                  r, r->done, (unsigned long)ESP.getFreeHeap());
  r->ok = false;

  DEBUG_CMD_FLOWF("[submit] Preparing to send: r=%p &r=%p", r, &r);
  DEBUG_CMD_FLOWF("[submit] Queue: gCmdExecQ=%p", gCmdExecQ);
  DEBUG_CMD_FLOWF("[submit] Request details: origin=%d user='%.32s' path='%.64s' cmd='%.128s'",
                  (int)r->ctx.origin,
                  r->ctx.auth.user.c_str(),
                  r->ctx.auth.path.c_str(),
                  r->line);

  // Enqueue and wait
  DEBUG_CMD_FLOWF("[submit] Calling xQueueSend(queue=%p, item_addr=%p, timeout=MAX)", gCmdExecQ, &r);
  DEBUG_CMD_FLOWF("[submit] Safety check: gCmdExecQ=%p r=%p &r=%p", gCmdExecQ, r, &r);
  if (!gCmdExecQ) {
    DEBUG_CMD_FLOWF("[submit] ERROR: gCmdExecQ is NULL!");
    vSemaphoreDelete(r->done);
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Command queue is NULL");
    return false;
  }
  if (!r) {
    DEBUG_CMD_FLOWF("[submit] ERROR: r is NULL!");
    broadcastOutput("[ERROR] Request pointer is NULL");
    return false;
  }
  BaseType_t queueResult = xQueueSend(gCmdExecQ, &r, pdMS_TO_TICKS(2000));
  DEBUG_CMD_FLOWF("[submit] xQueueSend returned: result=%d (1=success)", queueResult);
  
  if (queueResult != pdTRUE) {
    DEBUG_CMD_FLOWF("[submit] FAILED to send to queue! result=%d", queueResult);
    vSemaphoreDelete(r->done);
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Command queue full - try again");
    return false;
  }
  
  DEBUG_CMD_FLOWF("[submit] Waiting for semaphore: r->done=%p", r->done);
  if (xSemaphoreTake(r->done, pdMS_TO_TICKS(10000)) != pdTRUE) {
    DEBUG_CMD_FLOWF("[submit] Command execution timed out (10s)");
    vSemaphoreDelete(r->done);
    r->~ExecReq();
    free(r);
    out = "[ERROR] Command timed out";
    return false;
  }
  DEBUG_CMD_FLOWF("[submit] Semaphore taken - command completed");

  out = r->out;  // Copy from char array to String
  bool ok = r->ok;

  vSemaphoreDelete(r->done);
  // Call destructor and free PSRAM
  r->~ExecReq();
  free(r);

  DEBUG_CMD_FLOWF("[submit] done ok=%d len=%d", ok ? 1 : 0, out.length());
  return ok;
}

// Async command execution - fires and forgets, callback called on cmd_exec task
// Returns true if successfully queued, false on error
// Callback receives: (bool ok, const char* result, void* userData)
bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData) {
  DEBUG_CMD_FLOWF("[submitAsync] enter: cmd.line='%s'", cmd.line.c_str());
  
  if (gCmdExecQ == nullptr) {
    DEBUG_CMD_FLOWF("[submitAsync] ERROR: gCmdExecQ is NULL");
    return false;
  }
  
  if (cmd.line.length() == 0) {
    DEBUG_CMD_FLOWF("[submitAsync] ERROR: Empty command line");
    return false;
  }
  
  // Allocate ExecReq from PSRAM
  ExecReq* r = (ExecReq*)ps_alloc(sizeof(ExecReq), AllocPref::PreferPSRAM, "cmd.exec.async");
  if (!r) {
    DEBUG_CMD_FLOWF("[submitAsync] FAILED to allocate ExecReq");
    return false;
  }
  new (r) ExecReq();
  
  // Setup request
  strncpy(r->line, cmd.line.c_str(), sizeof(r->line) - 1);
  r->line[sizeof(r->line) - 1] = '\0';
  r->ctx = cmd.ctx;
  r->done = nullptr;  // No semaphore - async mode
  r->asyncCallback = callback;
  r->asyncUserData = userData;
  r->ok = false;
  
  // Queue for execution
  if (xQueueSend(gCmdExecQ, &r, 0) != pdTRUE) {
    DEBUG_CMD_FLOWF("[submitAsync] FAILED to queue command");
    r->~ExecReq();
    free(r);
    return false;
  }
  
  DEBUG_CMD_FLOWF("[submitAsync] Command queued successfully");
  return true;
}

// Convenience wrapper: execute a command with an existing context and return output
String execCommandUnified(const CommandContext& baseCtx, const String& line) {
  DEBUG_CMD_FLOWF("[exec] enter origin=%d user=%s path=%s cmd=%s", (int)baseCtx.origin, baseCtx.auth.user.c_str(), baseCtx.auth.path.c_str(), line.c_str());
  Command c;
  c.line = line;
  c.ctx = baseCtx;
  String out;
  (void)submitAndExecuteSync(c, out);
  DEBUG_CMD_FLOWF("[exec] exit len=%d", out.length());
  return out;
}

// Helper: run a command as SYSTEM origin with logging (used during first-time setup and automations)
void runUnifiedSystemCommand(const String& cmd) {
  AuthContext actx;
  actx.transport = SOURCE_INTERNAL;
  actx.user = "system";
  actx.ip = String();
  actx.path = "/system";
  actx.opaque = nullptr;
  Command uc;
  uc.line = cmd;
  uc.ctx.origin = ORIGIN_SYSTEM;
  uc.ctx.auth = actx;
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = (uint32_t)millis();
  uc.ctx.outputMask = CMD_OUT_LOG;
  uc.ctx.validateOnly = false;
  uc.ctx.replyHandle = nullptr;
  uc.ctx.httpReq = nullptr;
  String out;
  (void)submitAndExecuteSync(uc, out);
  broadcastOutput(out, uc.ctx);
}

// Helper used by web settings and other web endpoints to run a CLI-equivalent through unified path
bool executeUnifiedWebCommand(httpd_req_t* req, AuthContext& ctx, const String& cmd, String& out) {
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
  bool ok = submitAndExecuteSync(uc, out);
  broadcastOutput(out, uc.ctx);
  return ok;
}

// ============================================================================
// Icon System Implementation - Unified PNG-based icons for OLED/Web/TFT
// ============================================================================

#include <Adafruit_SSD1306.h>
#include "System_Icons.h"

bool initIconSystem() {
  DEBUG_STORAGEF("[Icons] Icon system initialized");
  return true;
}

String getIconPath(const char* name) {
  return String("/icons/") + name + ".png";
}

bool iconExists(const char* name) {
  // Check embedded icons first
  if (findEmbeddedIcon(name) != nullptr) {
    return true;
  }

  return false;
}

// Extension-to-icon mapping (shared logic)
static const struct {
  const char* ext;
  const char* icon;
} kExtIconMap[] = {
  {"json", "file_json"},
  {"txt", "file_text"},
  {"md", "file_text"},
  {"log", "file_text"},
  {"ino", "file_code"},
  {"cpp", "file_code"},
  {"h", "file_code"},
  {"hpp", "file_code"},
  {"c", "file_code"},
  {"py", "file_code"},
  {"js", "file_code"},
  {"html", "file_code"},
  {"css", "file_code"},
  {"xml", "file_code"},
  {"yaml", "file_code"},
  {"yml", "file_code"},
  {"jpg", "file_image"},
  {"jpeg", "file_image"},
  {"png", "file_image"},
  {"gif", "file_image"},
  {"bmp", "file_image"},
  {"svg", "file_image"},
  {"zip", "file_zip"},
  {"tar", "file_zip"},
  {"gz", "file_zip"},
  {"7z", "file_zip"},
  {"rar", "file_zip"},
  {"pdf", "file_pdf"},
  {"bin", "file_bin"},
  {"hex", "file_bin"},
  {"elf", "file_bin"},
  {nullptr, "file"}  // default fallback
};

const char* getIconNameForExtension(const char* ext) {
  if (!ext || !*ext) return "file";
  
  // Normalize: skip leading dot and lowercase
  const char* p = ext;
  if (*p == '.') p++;
  
  // Simple lowercase without locale
  char lower[16];
  size_t i = 0;
  for (; i < sizeof(lower)-1 && p[i]; ++i) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    lower[i] = c;
  }
  lower[i] = '\0';
  
  for (size_t idx = 0; kExtIconMap[idx].ext; ++idx) {
    if (strcmp(lower, kExtIconMap[idx].ext) == 0) {
      return kExtIconMap[idx].icon;
    }
  }
  return "file";  // default
}

bool loadIconData(const char* name, uint8_t* buffer, size_t bufferSize, uint8_t& width, uint8_t& height) {
  if (bufferSize < 128) {
    DEBUG_STORAGEF("[Icons] Buffer too small (need 128 bytes minimum)");
    return false;
  }

  // First, try embedded PROGMEM icons (zero heap, instant)
  const EmbeddedIcon* embedded = findEmbeddedIcon(name);
  if (embedded) {
    width = pgm_read_byte(&embedded->width);
    height = pgm_read_byte(&embedded->height);
    const uint8_t* bitmapPtr = (const uint8_t*)pgm_read_ptr(&embedded->bitmapData);
    memcpy_P(buffer, bitmapPtr, 128);
    return true;
  }

  return false;
}

bool drawIcon(Adafruit_SSD1306* display, const char* name, int x, int y, uint16_t color) {
  if (!display) {
    return false;
  }

  uint8_t buffer[128];
  uint8_t width, height;

  if (!loadIconData(name, buffer, sizeof(buffer), width, height)) {
    return false;
  }

  // Icons are stored LSB-first, but drawBitmap expects MSB-first.
  // Draw pixel-by-pixel with correct bit ordering.
  for (int py = 0; py < height; py++) {
    for (int px = 0; px < width; px++) {
      int byteIndex = py * (width / 8) + (px / 8);
      int bitIndex = px % 8;  // LSB-first: bit 0 = leftmost pixel
      if ((buffer[byteIndex] >> bitIndex) & 1) {
        display->drawPixel(x + px, y + py, color);
      }
    }
  }
  return true;
}

// Helper to get a bit from 32x32 1bpp bitmap (row-major, LSB first per byte)
static inline bool getBitmapBit(const uint8_t* buffer, int x, int y) {
  if (x < 0 || x >= 32 || y < 0 || y >= 32) return false;
  int byteIndex = y * 4 + (x / 8);  // 32 pixels / 8 = 4 bytes per row
  int bitIndex = x % 8;
  return (buffer[byteIndex] >> bitIndex) & 1;
}

bool drawIconScaled(Adafruit_SSD1306* display, const char* name, int x, int y, uint16_t color, float scale) {
  if (!display || scale <= 0) {
    return false;
  }

  uint8_t buffer[128];
  uint8_t width, height;

  if (!loadIconData(name, buffer, sizeof(buffer), width, height)) {
    return false;
  }

  // For scale == 1.0, use native bitmap draw (faster)
  if (scale >= 0.99f && scale <= 1.01f) {
    display->drawBitmap(x, y, buffer, width, height, color);
    return true;
  }

  // Calculate output dimensions
  int outWidth = (int)(width * scale);
  int outHeight = (int)(height * scale);

  // Handle 0.5x scale (32→16) with 2×2 block sampling
  if (scale >= 0.49f && scale <= 0.51f && width == 32 && height == 32) {
    for (int dy = 0; dy < 16; dy++) {
      for (int dx = 0; dx < 16; dx++) {
        // Sample 2×2 block from source
        int srcX = dx * 2;
        int srcY = dy * 2;
        bool p00 = getBitmapBit(buffer, srcX, srcY);
        bool p01 = getBitmapBit(buffer, srcX + 1, srcY);
        bool p10 = getBitmapBit(buffer, srcX, srcY + 1);
        bool p11 = getBitmapBit(buffer, srcX + 1, srcY + 1);

        // OR logic: draw pixel if any source pixel is set (preserves thin lines)
        if (p00 || p01 || p10 || p11) {
          display->drawPixel(x + dx, y + dy, color);
        }
      }
    }
    return true;
  }

  // Generic scaling for other factors (nearest neighbor)
  float invScale = 1.0f / scale;
  for (int dy = 0; dy < outHeight; dy++) {
    for (int dx = 0; dx < outWidth; dx++) {
      int srcX = (int)(dx * invScale);
      int srcY = (int)(dy * invScale);
      if (getBitmapBit(buffer, srcX, srcY)) {
        display->drawPixel(x + dx, y + dy, color);
      }
    }
  }

  return true;
}

// ============================================================================
// Authentication Commands (critical system functions)
// ============================================================================

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
    notifyLoginSuccess(username.c_str(), transportStr.c_str());
    static char buf[128];
    snprintf(buf, sizeof(buf), "Login successful for '%s' on %s%s",
             username.c_str(), transportStr.c_str(), isAdmin ? " (admin)" : "");
    return buf;
  } else {
    notifyLoginFailed(username.c_str(), transportStr.c_str());
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

// ============================================================================
// Input Abstraction Layer - MOVED TO Input_HAL.cpp
// ============================================================================
// Implementation is now in Input_HAL.cpp for better modularity.
