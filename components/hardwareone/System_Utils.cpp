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
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#if CONFIG_HEAP_TASK_TRACKING
#include <esp_heap_task_info.h>   // heap_task_totals_t, heap_caps_get_per_task_info
#endif
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
#include "HAL_Input.h"          // For INPUT_TASK_NAME (gamepad vs ANO)
#include "System_User.h"
#include "System_AuthIdentity.h"  // ExecIdentityGuard (executeCommand + submitAndExecuteSync)
#include "System_SelfDevice.h"   // SelfDevice:: — local identity/heap/uptime/firmware (Stage 1 consolidation)
#include "System_Clock.h"        // Clock:: — epoch/sync/tz/format helpers (Stage 2)
#include "System_Command.h"
#include "System_SensorStubs.h"  // Stubs for disabled sensors/modules
#include "System_MemoryMonitor.h"
#include "System_Notifications.h"
#include "i2csensor_ds3231.h"  // RTC for time functions
// Additional sensor headers for the gXxxEnabled/gXxxConnected externs used by cmd_voltage.
#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_APDS_SENSOR
#include "i2csensor_apds9960.h"
#endif
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
#if ENABLE_ANO_ENCODER
extern const CommandEntry anoEncoderCommands[];
extern const size_t anoEncoderCommandsCount;
#endif
#if ENABLE_OLED_INPUT
extern const CommandEntry inputCommands[];
extern const size_t inputCommandsCount;
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
extern const CommandEntry ledCommands[];
extern const size_t ledCommandsCount;
extern const CommandEntry featureCommands[];
extern const size_t featureCommandsCount;
#if ENABLE_CAMERA_SENSOR
extern const CommandEntry imageCommands[];
extern const size_t imageCommandsCount;
#endif
extern const CommandEntry mapCommands[];
extern const size_t mapCommandsCount;
extern const CommandEntry mapsSettingCommands[];
extern const size_t mapsSettingCommandsCount;
extern const CommandEntry powerCommands[];
extern const size_t powerCommandsCount;
#if ENABLE_OLED_DISPLAY
extern const CommandEntry setPatternCommands[];
extern const size_t setPatternCommandsCount;
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
extern const CommandEntry g2Commands[];
extern const size_t g2CommandsCount;
extern const CommandEntry g2RingCommands[];
extern const size_t g2RingCommandsCount;
#endif

// Include module headers to access their command registries
#include "System_Filesystem.h"
#include "System_Debug.h"
#include "System_CLI.h"
#include "System_CLIMode.h"
#include "System_BuildConfig.h"   // Conditional sensor configuration - must be early
#if ENABLE_WIFI
  #include "System_WiFi.h"
#endif
#include "OLED_Display.h"
#include "System_NeoPixel.h"
#if ENABLE_SERVO
#include "i2csensor_pca9685.h"
#endif
#include "System_Automation.h"
#include "System_I2C.h"
#if ENABLE_ESPNOW
  #include "System_ESPNow.h"
  #include "System_ESPNow_Wire.h"   // ESPNOW_V4_TYPE_* / ESPNOW_V4_FLAG_* opcode + flag enums
#endif
#if ENABLE_BLUETOOTH
  #include "Bluetooth.h"
#endif
#include "System_Settings.h"
#include "System_User.h"
#include "System_VFS.h"  // For sdCommands (SD card management)
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor_mlx90640.h"  // For thermalCommands
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor_vl53l4cx.h"      // For tofCommands
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor_bno055.h"      // For imuCommands
#endif
#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor_seesaw.h"  // For gamepadCommands
#endif
#if ENABLE_ANO_ENCODER
  #include "i2csensor_ano_encoder.h"  // For anoEncoderCommands
#endif
#if ENABLE_OLED_INPUT
  #include "HAL_Input.h"  // For inputCommands
#endif
#if ENABLE_APDS_SENSOR
  #include "i2csensor_apds9960.h"     // For apdsCommands
#endif
#include "System_SensorStubs.h" // Stubs for disabled sensors
#if ENABLE_GPS_SENSOR
  #include "i2csensor_pa1010d.h"      // For gpsCommands
#endif
#include "i2csensor_rda5807.h"        // For fmRadioCommands

// External dependencies from .ino
// (filesystemReady is provided by System_Filesystem.h, included above)
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
  char kBuf[64];
  snprintf(kBuf, sizeof(kBuf), "\"%s\":", key);
  int p = src.indexOf(kBuf);
  if (p < 0) return false;
  p += strlen(kBuf);
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
  char kBuf[64];
  snprintf(kBuf, sizeof(kBuf), "\"%s\":", key);
  int p = src.indexOf(kBuf);
  if (p < 0) return false;
  p += strlen(kBuf);
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
  char kBuf[64];
  snprintf(kBuf, sizeof(kBuf), "\"%s\":", key);
  int p = src.indexOf(kBuf);
  if (p < 0) return false;
  p += strlen(kBuf);
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
  char kBuf[64];
  snprintf(kBuf, sizeof(kBuf), "\"%s\":\"", key);
  int p = src.indexOf(kBuf);
  if (p < 0) return false;
  p += strlen(kBuf);
  int end = src.indexOf('\"', p);
  if (end < 0) return false;
  out = src.substring(p, end);
  return true;
}

bool extractObjectByKey(const String& src, const char* key, String& outObj) {
  char kBuf[64];
  snprintf(kBuf, sizeof(kBuf), "\"%s\"", key);
  int kLen = strlen(kBuf);
  int keyPos = src.indexOf(kBuf);
  if (keyPos < 0) return false;
  int colon = src.indexOf(':', keyPos + kLen);
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
  char kBuf[64];
  snprintf(kBuf, sizeof(kBuf), "\"%s\"", key);
  int kLen = strlen(kBuf);
  int keyPos = src.indexOf(kBuf);
  if (keyPos < 0) return false;
  int colon = src.indexOf(':', keyPos + kLen);
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

// Wizard-input timeout state. When timeoutMs > 0, waitForSerialInputBlocking
// gives up after `timeoutMs` of no input and sets the cancel flag. Callers
// (the setup wizard, first-time-setup) can poll isWizardCancelRequested() to
// short-circuit out of the wizard cleanly.
//
// FTS at boot uses timeout=0 (wait forever — fresh-device owner may need
// time to read instructions). cmd_featuresetup uses timeout=60s so a CLI
// invocation can't park the cmd_exec task indefinitely if the user walks
// away or invokes from a transport that can't actually deliver input.
//
// Single-writer single-reader semantics: only the wizard entry points
// (setSerialWaitTimeout) write the timeout, only waitForSerialInputBlocking
// reads + writes the activity timestamp. The cancel flag is set by the
// timeout path and read by wizard code on the same task.
static volatile unsigned long sSerialWaitTimeoutMs = 0;
static volatile unsigned long sSerialWaitLastActivityMs = 0;
static volatile bool sWizardCancelRequested = false;

void setSerialWaitTimeout(unsigned long timeoutMs) {
  sSerialWaitTimeoutMs = timeoutMs;
  sSerialWaitLastActivityMs = millis();
  sWizardCancelRequested = false;
}

bool isWizardCancelRequested() {
  return sWizardCancelRequested;
}

String waitForSerialInputBlocking() {
  for (;;) {
    if (Serial.available()) {
      sSerialWaitLastActivityMs = millis();
      String input = Serial.readStringUntil('\n');
      input.trim();
      return input;
    }
    if (sSerialWaitTimeoutMs > 0 &&
        (millis() - sSerialWaitLastActivityMs) > sSerialWaitTimeoutMs) {
      sWizardCancelRequested = true;
      return String();
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
  // Sanity check: if RTC is unsynced, it may report 1970/1980 era time.
  // Clock::isValidEpoch enforces the year >= 2020 threshold uniformly.
  if (!Clock::isValidEpoch(sec)) return;
  struct tm tminfo;
  if (!localtime_r(&sec, &tminfo)) return;
  // Build prefix directly into caller's buffer
  char base[24];  // "[YYYY-MM-DD HH:MM:SS" (23 + NUL)
  if (strftime(base, sizeof(base), "[%Y-%m-%d %H:%M:%S", &tminfo) <= 0) return;
  snprintf(out, outSize, "%s.%03d] | ", base, ms);
}

// Sensor state externs for cmd_voltage come from the sensor headers (included above).

// ============================================================================
// File I/O Functions
// ============================================================================

// NOTE: These helpers route through VFS::open, which dispatches to LittleFS
// for any path without a "/sd/" prefix. In the current codebase that means
// EVERY caller (state files: settings.json, users.json, automations.json,
// etc.) goes to LittleFS. Using VFS here is about API consistency, not
// behavior change — these state files should never overflow to SD (see the
// design contract in System_VFS.h).

bool readText(const char* path, String& out) {
  out = "";

  // Pause sensor polling during file I/O to prevent I2C contention
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard guard("readText");
  File f = VFS::open(String(path), "r");
  if (!f) {
    gSensorPollingPaused = wasPaused;
    return false;
  }
  out = f.readString();
  f.close();

  gSensorPollingPaused = wasPaused;
  return true;
}

bool writeText(const char* path, const String& in) {
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard guard("writeText");
  File f = VFS::open(String(path), "w", true);
  if (!f) {
    gSensorPollingPaused = wasPaused;
    return false;
  }
  f.print(in);
  f.flush();
  f.close();

  gSensorPollingPaused = wasPaused;
  return true;
}

bool writeTextAtomic(const char* path, const String& content) {
  String tmp = String(path) + ".tmp";

  // Write to temp file first
  if (!writeText(tmp.c_str(), content)) return false;

  // Atomic rename — VFS dispatches to the correct filesystem based on path.
  {
    FsLockGuard guard("atomicRename");
    if (VFS::rename(tmp, String(path))) {
      return true;
    }
  }

  // Fallback: direct write if rename fails
  VFS::remove(tmp);
  return writeText(path, content);
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

// Read-only status commands that the web UI polls on a tick. Both the
// serial broadcast and the audit-file write are skipped for these —
// the broadcast was filling the serial log with one line per poll
// (g2status, ringstatus, blestatus are each fetched every 1.5-15 s by
// the Bluetooth panel), and the audit file would grow without bound
// for traffic that has zero security relevance (pure reads, no state
// change). Keeping the list deliberately tight: only commands that
// (a) cannot mutate state and (b) are called automatically by the UI
// belong here. User-typed `g2status` from a CLI prompt also passes
// through this path and gets suppressed too — that's fine; it's
// visible in the prompt response anyway.
static bool isQuietPollCommand(const char* cmd) {
  if (!cmd || !cmd[0]) return false;
  // Match the leading verb; ignore any trailing args (json modifiers, etc.)
  static const char* const kQuiet[] = {
    "g2status", "ringstatus", "blestatus",
  };
  for (size_t i = 0; i < sizeof(kQuiet) / sizeof(kQuiet[0]); i++) {
    size_t n = strlen(kQuiet[i]);
    if (strncmp(cmd, kQuiet[i], n) == 0 &&
        (cmd[n] == '\0' || cmd[n] == ' ' || cmd[n] == '\t')) {
      return true;
    }
  }
  return false;
}

/**
 * Log command execution to audit file
 * Format: [timestamp] user@transport command -> result_status
 * Lightweight, always-enabled, no overhead when disabled
 */
void logCommandExecution(const AuthContext& ctx, const char* cmd, bool success, const char* result) {
  // Skip validation-only commands (dry-run checks)
  if (gCLIValidateOnly) return;

  // Skip the audit log + broadcast for noisy read-only polls.
  if (isQuietPollCommand(cmd)) return;

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
    case SOURCE_G2_GLASSES: source = "g2"; break;
    default: source = "unknown"; break;
  }
  
  // Redact sensitive data from command
  String redactedCmd = redactCmdForAudit(cmd);
  
  // Result summary (first 40 chars, single line, no heap alloc)
  char resultBuf[44];
  if (result && result[0] != '\0') {
    size_t len = strlen(result);
    if (len > 40) {
      memcpy(resultBuf, result, 37);
      resultBuf[37] = '.'; resultBuf[38] = '.'; resultBuf[39] = '.';
      resultBuf[40] = '\0';
    } else {
      strncpy(resultBuf, result, sizeof(resultBuf) - 1);
      resultBuf[sizeof(resultBuf) - 1] = '\0';
    }
    // Replace newlines with spaces
    for (char* p = resultBuf; *p; p++) {
      if (*p == '\n' || *p == '\r') *p = ' ';
    }
  } else {
    strcpy(resultBuf, "OK");
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
           resultBuf);
  
  // Append to audit log with 500KB cap (rotates automatically)
  appendLineWithCap("/system/sys_logs/command-audit.log", entry, 500 * 1024);
  
  // Broadcast command execution notice to all interfaces (serial, web, OLED, etc.)
  // This is the "who did what from where" audit trail visible everywhere.
  // Uses explicit MSG_ROUTE_ALL to bypass context-based serial suppression —
  // the audit line should ALWAYS appear on all interfaces regardless of command origin.
  char auditLine[384];
  // Include resultBuf (already capped to 40 chars, newlines neutralized) so
  // serial logs surface the actual command response — otherwise paths that
  // return non-empty text (e.g. "Not detected on I2C bus", "already running")
  // are indistinguishable from a literal "OK" in the audit stream.
  snprintf(auditLine, sizeof(auditLine), "[CMD] %s@%s: %s -> %s %s",
           ctx.user.c_str(), source, redactedCmd.c_str(), status, resultBuf);
  extern void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route);
  broadcastOutputCore_Routed(auditLine, strlen(auditLine), MSG_ROUTE_ALL);
}

// Automation logging
//
// Writes are gated by gAutoLogOwnerCtx — the AuthContext captured when the
// user ran `autolog start`. See the comment on gAutoLogOwnerCtx in
// System_Automation.cpp for the design rationale (captured identity vs.
// reading the current task's identity at write time, which fires from event
// triggers detached from any CLI session).
//
// If the captured ctx loses permission to the path mid-run (e.g. admin
// demoted to user), individual writes start failing — that's intentional.
bool appendAutoLogEntry(const char* type, const String& message) {
  if (!gAutoLogActive || gAutoLogFile.length() == 0) return false;
  if (!filesystemReady) return false;

  extern AuthContext gAutoLogOwnerCtx;

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

  // Resolve destination — routes to /sd mirror when LittleFS is full.
  char dest[128];
  VFS::resolveOverflowPath(gAutoLogFile.c_str(), line.length() + 512,
                           dest, sizeof(dest));

  // Ensure the parent directory exists on whichever FS we're writing to.
  // Both exists and mkdir go through the guarded path so the captured user
  // can't create directories they don't have CREATE perm on.
  String destStr(dest);
  int lastSlash = destStr.lastIndexOf('/');
  if (lastSlash > 0) {
    String dir = destStr.substring(0, lastSlash);
    if (!VFS::existsGuarded(dir, gAutoLogOwnerCtx)) {
      if (!VFS::mkdirGuarded(dir, gAutoLogOwnerCtx)) return false;
    }
  }

  File f = VFS::openGuarded(destStr, "a", gAutoLogOwnerCtx);
  if (!f) return false;

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
    // Expect: "espnowremote <target> <username> <password> <command>..."
    String c = in;
    int base = c.indexOf(' ');                      // after "espnowremote"
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
  // Columns: prefix (lowercase cmd prefix), type (MASK_TOKEN_AT_POS|MASK_AFTER_TOKEN_POS|CALL_HANDLER), param (1-based token index), handler (custom fn or nullptr)
  static const RedactRule kRules[] = {
    { "wifiadd ",          MASK_TOKEN_AT_POS,    3, nullptr },  // wifiadd <ssid> <password>
    { "mqttpassword ",     MASK_TOKEN_AT_POS,    2, nullptr },  // mqttpassword <password>
    { "login ",            MASK_TOKEN_AT_POS,    3, nullptr },  // login <user> <password>
    { "testencryption ",   MASK_TOKEN_AT_POS,    2, nullptr },  // testencryption <secret>
    { "testpassword ",     MASK_TOKEN_AT_POS,    2, nullptr },  // testpassword <secret>
    { "userrequest ",      MASK_AFTER_TOKEN_POS, 2, nullptr },  // userrequest <name> <pass> ...
    { "espnowremote ",     CALL_HANDLER,         0, &redactEspNowRemote },
  };
}

// =============================================================================
// settingBoolToggle — generic on/off CLI handler for persisted bools
// =============================================================================
// See System_Utils.h for the contract. Single static buffer because the
// CLI is single-threaded and result strings are consumed before the next
// command dispatches.

const char* settingBoolToggle(bool& field, const String& argsInput, const char* label) {
  static char buf[80];
  if (!label) label = "Setting";

  String arg = argsInput;
  arg.trim();

  // Empty arg → report current state.
  if (arg.length() == 0) {
    snprintf(buf, sizeof(buf), "%s: %s", label,
             field ? "enabled" : "disabled");
    return buf;
  }

  int parsed = parseBoolArg(arg);
  if (parsed == 1) {
    setSetting(field, true);
    snprintf(buf, sizeof(buf), "%s enabled", label);
    return buf;
  }
  if (parsed == 0) {
    setSetting(field, false);
    snprintf(buf, sizeof(buf), "%s disabled", label);
    return buf;
  }
  snprintf(buf, sizeof(buf), "%s: invalid value (use on|off)", label);
  return buf;
}

String redactCmdForAudit(const String& argsInput) {
  String c = argsInput;
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

  if (gThermalConnected && gThermalEnabled) {
    estimatedCurrent += 23;  // MLX90640 typical
    broadcastOutput("Thermal Sensor: Active (+23mA)");
  }

  if (gImuConnected && gImuEnabled) {
    estimatedCurrent += 12;  // BNO055 typical
    broadcastOutput("IMU Sensor: Active (+12mA)");
  }

  if (gTofConnected && gTofEnabled) {
    estimatedCurrent += 20;  // VL53L4CX typical
    broadcastOutput("ToF Sensor: Active (+20mA)");
  }

  if (gApdsConnected) {
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

const char* cmd_cpufreq(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsInput;
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
    {
      extern void batteryLogEvent(const char* event);
      char ev[24];
      snprintf(ev, sizeof(ev), "cpufreq:%luMHz", (unsigned long)newFreq);
      batteryLogEvent(ev);
    }
    BROADCAST_PRINTF("CPU frequency set to %lu MHz", (unsigned long)newFreq);
    return "[System] CPU frequency updated";
  }
}

// =========================================================================
// Light Sleep Command
// =========================================================================

#include <esp_sleep.h>
#include "OLED_Display.h"
#include "System_Power.h"  // powerSleepTransitionAllowed/Mark — anti-flap guard

const char* cmd_lightsleep(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  // Anti-flap: refuse if a previous sleep entry was too recent. Without this,
  // a glitched trigger (stuck button, MQTT spam, a future "sleep on idle"
  // racing "wake on activity") could thrash the LDO + WiFi/BLE reconnect
  // path every few hundred ms and chew the battery in minutes. Cooldown is
  // gSettings.powerTransitionCooldownMs (default 5000, 0 disables).
  unsigned long cooldownRemain = 0;
  if (!powerSleepTransitionAllowed(&cooldownRemain)) {
    snprintf(getDebugBuffer(), 1024,
             "Sleep refused: cooldown active — try again in %lu ms (powercooldown to tune)",
             cooldownRemain);
    return getDebugBuffer();
  }

  // Parse optional duration (default 20 seconds)
  int seconds = 20;
  String arg = argsInput;
  arg.trim();
  if (arg.length() > 0) {
    int val = arg.toInt();
    if (val > 0 && val <= 3600) {
      seconds = val;
    }
  }

  BROADCAST_PRINTF("Entering light sleep for %d seconds...", seconds);
  // Stamp the cooldown NOW (before the actual sleep call) so that a
  // concurrent task or a wake-then-immediate-resleep loop sees a fresh
  // last-transition time. Wake doesn't re-stamp; the clock keeps running.
  powerSleepTransitionMark();
  delay(100);  // Allow message to be sent
  
  // Show sleep message and turn off display (uses abstracted functions)
  oledShowSleepScreen(seconds);
  delay(500);
  // oledPrepareForSleep supersedes the old oledDisplayOff: on bus-1 + power-
  // gated builds it ALSO drops LDO2 so the OLED truly loses power, not just
  // its panel pixels. Falls back to plain SSD1306-DISPLAYOFF everywhere else.
  oledPrepareForSleep();

  // Annotate the battery log so the discharge curve shows the sleep window.
  extern void batteryLogEvent(const char* event);
  {
    char ev[24];
    snprintf(ev, sizeof(ev), "sleep:light:%ds", seconds);
    batteryLogEvent(ev);
  }

  // Configure wake-up source: timer
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);

  // Enter light sleep (preserves RAM, resumes here when woken)
  esp_light_sleep_start();

  // Execution resumes here after wake-up
  DEBUG_SYSTEMF("Woke from light sleep!");
  batteryLogEvent("wake:light");

  // oledResumeFromSleep supersedes oledDisplayOn: on bus-1 + power-gated
  // builds it raises LDO2, waits for the rail to stabilise, re-runs the
  // SSD1306 init (registers were lost on the power cycle), and reapplies
  // rotation + brightness so the panel comes back exactly as it was.
  oledResumeFromSleep();
  
  return "Woke from light sleep";
}

// =========================================================================
// Core System Commands (moved from .ino)
// =========================================================================

const char* cmd_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  BROADCAST_PRINTF("HardwareOne v%s (%s)", SelfDevice::firmwareVersion(), BOARD_NAME);
  broadcastOutput("System Status:");
#if ENABLE_WIFI
  BROADCAST_PRINTF("  WiFi: %s", WiFi.isConnected() ? "Connected" : "Disconnected");
  BROADCAST_PRINTF("  IP: %s", WiFi.localIP().toString().c_str());
#else
  BROADCAST_PRINTF("  WiFi: Disabled");
#endif
  BROADCAST_PRINTF("  Filesystem: %s", filesystemReady ? "Ready" : "Error");
  BROADCAST_PRINTF("  Free Heap: %lu bytes", (unsigned long)SelfDevice::freeHeapBytes());

  size_t psTot = ESP.getPsramSize();
  if (psTot > 0) {
    BROADCAST_PRINTF("  Free PSRAM: %lu bytes", (unsigned long)SelfDevice::psramFreeBytes());
    BROADCAST_PRINTF("  Total PSRAM: %lu bytes", (unsigned long)psTot);
  }

  // Reset reason labels matching esp_reset_reason_t
  static const char* const kResetReasonLabels[] = {
    "Unknown", "Power-on", "External", "Software", "Panic",
    "Int WDT", "Task WDT", "WDT", "Deepsleep", "Brownout", "SDIO"
  };
  uint32_t reason = gSettings.lastResetReason;
  const char* reasonLabel = (reason < 11) ? kResetReasonLabels[reason] : "Unknown";
  BROADCAST_PRINTF("  Last Reset: %s", reasonLabel);
  BROADCAST_PRINTF("  Crash Count: %lu", (unsigned long)gSettings.crashCount);

  return "OK";
}

const char* cmd_uptime(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  uint32_t seconds = SelfDevice::uptimeSeconds();
  uint32_t minutes = seconds / 60;
  uint32_t hours = minutes / 60;
  BROADCAST_PRINTF("Uptime: %luh %lum %lus",
                   (unsigned long)hours, (unsigned long)(minutes % 60), (unsigned long)(seconds % 60));
  return "[System] Uptime displayed";
}

const char* cmd_time(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Show uptime in milliseconds
  unsigned long uptimeMs = millis();
  BROADCAST_PRINTF("Uptime: %lu ms", uptimeMs);
  
  // Priority: RTC (primary) -> NTP (fallback)
#if ENABLE_RTC_SENSOR
  if (gRtcEnabled && gRtcConnected) {
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

const char* cmd_timeset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
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
  if (gRtcEnabled && gRtcConnected) {
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

const char* cmd_fsusage(const String& argsInput) {
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
extern String encryptString(const String& plaintext);
extern String decryptString(const String& encrypted);
extern String hashUserPassword(const String& plaintext);
extern bool verifyUserPassword(const String& plaintext, const String& hash);

const char* cmd_testencryption(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsInput;
  args.trim();

  if (args.length() == 0) {
    return "Usage: testencryption <password_to_test>";
  }

  String encrypted = encryptString(args);
  String decrypted = decryptString(encrypted);

  broadcastOutput("AES String Encryption Test:");
  BROADCAST_PRINTF("Original:  '%s'", args.c_str());
  BROADCAST_PRINTF("Encrypted: '%s'", encrypted.c_str());
  BROADCAST_PRINTF("Decrypted: '%s'", decrypted.c_str());
  BROADCAST_PRINTF("Match: %s", (args == decrypted) ? "YES" : "NO");

  return "[System] Encryption test complete";
}

const char* cmd_testpassword(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsInput;
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

// ============================================================================
// factoryreset -- wipe users.json + reboot to re-trigger first-time setup
// ============================================================================
//
// Phase 4 of the CLIMode framework rollout: a real user-facing destructive
// command that runs through the same yes/no confirm flow as filedelete and
// userdelete (Phase 3). What it does:
//
//   1. Prompt: "Confirm FACTORY RESET? Wipes user accounts -- next boot
//      runs first-time setup wizard. Settings (WiFi, debug flags) are
//      PRESERVED."
//   2. If user says yes:
//      a. Delete /system/users/users.json via VFS::systemAuth (admin
//         can't delete /system/* under normal permissions -- only the
//         internal "system" auth has PERM_DELETE in the path table).
//      b. Schedule a reboot 1 second in the future via esp_timer.
//      c. Return "Factory reset complete. Rebooting in 1 second..."
//         -- this lets confirm_onInput audit the resolution + the
//         dispatcher flush the response to the user before the device
//         restarts.
//   3. If user says no: "Cancelled. No changes."
//
// Why esp_timer instead of inline delay+ESP.restart() like cmd_reboot:
// the confirm framework's audit hook fires AFTER the callback returns.
// If the callback never returns (because of ESP.restart()), the audit
// log entry for "factoryreset (confirm: yes) -> Factory reset complete"
// is lost. The esp_timer one-shot defers the actual restart until after
// the callback has returned, the audit has fired, and the response has
// flushed to the user. Without this, forensic auditors would see the
// prompt step but no resolution -- the device just reboots into FTS
// with no log trail of who triggered it.

#include "System_CLIConfirm.h"
#include "System_AuthIdentity.h"
#include "System_User.h"   // USERS_JSON_FILE
#include "System_VFS.h"

extern const char* USERS_JSON_FILE;  // defined in System_User.cpp

// One-shot timer callback: actually performs the reboot 1 second after
// scheduling. Runs on the esp_timer task, not cmd_exec, so it can call
// ESP.restart() without disrupting any in-flight audit writes.
static void factoryreset_doRestart(void* /*arg*/) {
  ESP.restart();
}

static const char* factoryreset_confirmed(void* /*userData*/) {
  static char respBuf[200];

  // Use the internal SYSTEM auth to delete users.json. The VFS path-rule
  // table grants admin only PERM_READ over /system/* -- only system auth
  // has PERM_DELETE. This is by design: an admin-credentials compromise
  // shouldn't be able to wipe accounts (which would lock everyone out
  // and trigger FTS the next boot). factoryreset is gated by the
  // requiresAdmin=true flag in the registry + the confirm prompt; system
  // auth is just the right capability to actually carry out the delete.
  AuthContext sysCtx = VFS::systemAuth("factory.reset");
  if (!VFS::removeGuarded(USERS_JSON_FILE, sysCtx)) {
    snprintf(respBuf, sizeof(respBuf),
             "Error: factory reset failed -- could not delete %s",
             USERS_JSON_FILE);
    return respBuf;
  }

  // Schedule the reboot for 1 second from now. This gives the confirm
  // framework time to audit the resolution and the dispatcher time to
  // flush "Factory reset complete..." to the user's serial / web client
  // before the device restarts. Memory note: the timer struct is
  // intentionally leaked (we're rebooting in 1 second; nothing else
  // will run) -- no esp_timer_delete needed.
  esp_timer_handle_t timer = nullptr;
  const esp_timer_create_args_t timerArgs = {
    .callback = factoryreset_doRestart,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "factoryreset_reboot",
    .skip_unhandled_events = false,
  };
  esp_timer_create(&timerArgs, &timer);
  esp_timer_start_once(timer, 1000ULL * 1000ULL);  // 1 s expressed in microseconds

  snprintf(respBuf, sizeof(respBuf),
           "Factory reset complete. %s deleted. Rebooting in 1 second to start setup wizard...",
           USERS_JSON_FILE);
  return respBuf;
}

static const char* factoryreset_cancelled(void* /*userData*/) {
  return "Cancelled. No changes made.";
}

const char* cmd_factoryreset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;  // factoryreset takes no arguments

  String prompt =
    "Confirm FACTORY RESET? This will:\n"
    "  - Delete all user accounts (/system/users/users.json)\n"
    "  - Reboot the device\n"
    "  - First-time setup wizard runs on next boot\n"
    "Settings (WiFi credentials, debug flags) are PRESERVED.\n"
    "This cannot be undone.";

  // Originating cmd line for the resolution audit -- mirrors how
  // filedelete and userdelete pass theirs. The audit log will read:
  //   [CMD] asd@serial: factoryreset (confirm: yes) -> OK   Factory reset complete...
  //   [CMD] asd@serial: factoryreset (confirm: no)  -> OK   Cancelled. No changes made.
  if (!cliRequestConfirm(prompt, "factoryreset",
                         factoryreset_confirmed,
                         factoryreset_cancelled,
                         nullptr)) {
    return "Error: cannot request confirm (another interactive mode is active)";
  }
  return "Type 'yes' to confirm or anything else to cancel.";
}

const char* cmd_broadcast(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String msg = argsInput;
  msg.trim();
  if (msg.length() == 0) return "Usage: broadcast <message>";
  broadcastOutput(msg);
  return "[System] Message broadcast";
}

const char* cmd_wait(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = argsInput;
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

extern void resolvePendingUserCreationTimes();
extern void notifyAutomationScheduler();
extern void logTimeSyncedMarkerIfReady();

void setupNTP() {
  long gmtOffset = (long)Clock::tzOffsetMinutes() * 60;  // seconds
  DEBUG_NTP_SETUPF("[NTP Setup] Starting NTP configuration");
  DEBUG_NTP_SETUPF("[NTP Setup] Primary server: %s", gSettings.ntpServer.c_str());
  DEBUG_NTP_SETUPF("[NTP Setup] GMT offset: %ld seconds (%d minutes)", gmtOffset, Clock::tzOffsetMinutes());
  DEBUG_NTP_SETUPF("[NTP Setup] WiFi status: %s", WiFi.isConnected() ? "CONNECTED" : "DISCONNECTED");
  if (WiFi.isConnected()) {
    DEBUG_NTP_SETUPF("[NTP Setup] WiFi IP: %s", WiFi.localIP().toString().c_str());
    DEBUG_NTP_SETUPF("[NTP Setup] WiFi gateway: %s", WiFi.gatewayIP().toString().c_str());
    DEBUG_NTP_SETUPF("[NTP Setup] WiFi DNS: %s", WiFi.dnsIP().toString().c_str());
    DEBUG_NTP_SETUPF("[NTP Setup] WiFi subnet: %s", WiFi.subnetMask().toString().c_str());
  }

  // Use standard configTime() with hostname-based NTP servers
  DEBUG_NTP_SETUPF("[NTP Setup] Configuring NTP with hostname-based servers");

  // Try multiple reliable NTP servers for redundancy
  configTime(gmtOffset, 0,
             gSettings.ntpServer.c_str(),  // Primary (usually pool.ntp.org)
             "time.google.com",            // Google
             "time.cloudflare.com");       // Cloudflare

  DEBUG_NTP_SETUPF("[NTP Setup] configTime() completed with servers:");
  DEBUG_NTP_SETUPF("[NTP Setup]   Primary: %s", gSettings.ntpServer.c_str());
  DEBUG_NTP_SETUPF("[NTP Setup]   Backup1: time.google.com");
  DEBUG_NTP_SETUPF("[NTP Setup]   Backup2: time.cloudflare.com");
}

bool syncNTPAndResolve() {
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] Starting NTP sync process");

  if (!WiFi.isConnected()) {
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] FAILED - WiFi not connected");
    broadcastOutput("NTP sync requires WiFi connection");
    return false;
  }

  DEBUG_NTP_SYNCF("[syncNTPAndResolve] WiFi connected, proceeding with NTP sync");

  // DNS is ready by the time WiFi reports an assigned IP — no separate
  // initialization phase to wait on. (Previously: hardcoded delay(500).)

  // Pre-flight DNS check — disabled.
  //
  // The synchronous WiFi.hostByName() call below was adding 5-8 s of dead
  // wait to every boot on otherwise-healthy networks (lwIP's DNS retry
  // strategy + IPv6/IPv4 dual-stack timeouts). It was originally there to
  // fail fast when DNS was broken, but lwIP's SNTP client does its own
  // resolution and the 15 s polling loop below already detects sync failure.
  // With MAX_SERVERS=3 plus DHCP-provided NTP, SNTP can succeed even when
  // one server's DNS is unhealthy — making this check actively misleading.
  //
  // To re-enable: change `#if 0` to `#if 1`.
#if 0
  // Test DNS resolution before attempting NTP
  IPAddress testIP;
  bool dnsWorking = WiFi.hostByName("time.google.com", testIP);
  bool validIP = dnsWorking && testIP != IPAddress(0, 0, 0, 0);
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] DNS test: hostByName('time.google.com') = %s, IP=%s",
                  validIP ? "SUCCESS" : "FAILED",
                  testIP.toString().c_str());

  if (!validIP) {
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] WARNING: DNS resolution failed (returned %s), NTP may not work",
                    testIP.toString().c_str());
    broadcastOutput("⚠ DNS resolution failed - NTP may not work");
    broadcastOutput("  Waiting 2 more seconds for DNS to initialize...");
    delay(2000);
    dnsWorking = WiFi.hostByName("pool.ntp.org", testIP);
    validIP = dnsWorking && testIP != IPAddress(0, 0, 0, 0);
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] DNS retry: hostByName('pool.ntp.org') = %s, IP=%s",
                    validIP ? "SUCCESS" : "FAILED",
                    testIP.toString().c_str());
    if (!validIP) {
      DEBUG_NTP_SYNCF("[syncNTPAndResolve] ERROR: DNS still not working after retry");
      broadcastOutput("[ERROR] DNS not working - NTP will fail");
      return false;
    }
  }
#endif

  broadcastOutput("Synchronizing time with NTP server...");
  setupNTP();
  broadcastOutput("  Contacting NTP server, please wait...");

  bool ntpSynced = false;
  // SNTP startup delay is now CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY=100 ms,
  // and DHCP-provided NTP servers (router-local, RTT ~1 ms) win when present.
  // 100 ms polling cadence catches the response within one iteration on a
  // healthy network. 15 s window kept as a safety net for offline / blocked-NTP
  // boots; typical sync exits in 1-3 iterations.
  const int maxWaitSeconds = 15;
  const int iterationsPerSecond = 10;  // 100ms per iteration
  const int maxIterations = maxWaitSeconds * iterationsPerSecond;
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] Starting %d-second wait loop for NTP response", maxWaitSeconds);

  for (int i = 0; i < maxIterations && !ntpSynced; i++) {
    delay(100);
    oledUpdate();  // Keep boot animation running during NTP wait

    if (i > 0 && i % iterationsPerSecond == 0) {
      char progressMsg[64];
      snprintf(progressMsg, sizeof(progressMsg), "  Looking for updates... %d/%d seconds", i / iterationsPerSecond, maxWaitSeconds);
      broadcastOutput(progressMsg);
      DEBUG_NTP_SYNCF("[syncNTPAndResolve] Waiting... %d/%d seconds elapsed", i / iterationsPerSecond, maxWaitSeconds);
    }

    time_t now = time(nullptr);
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] time(nullptr) returned: %lu", (unsigned long)now);

    struct tm timeinfo;
    bool gotLocalTime = getLocalTime(&timeinfo, 10);  // 10ms timeout
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] getLocalTime(10ms) returned: %s", gotLocalTime ? "true" : "false");

    if (gotLocalTime) {
      DEBUG_NTP_SYNCF("[syncNTPAndResolve] SUCCESS! Time synced: %04d-%02d-%02d %02d:%02d:%02d",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      logTimeSyncedMarkerIfReady();
      ntpSynced = true;
      break;
    }
  }

  if (ntpSynced) {
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] NTP sync completed successfully");
    broadcastOutput("[OK] NTP time synchronized successfully");
    
    // Sync RTC from NTP time to keep RTC accurate
#if ENABLE_RTC_SENSOR
    if (gRtcEnabled && gRtcConnected) {
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
    
    DEBUG_NTP_SYNCF("About to call resolvePendingUserCreationTimes");
    resolvePendingUserCreationTimes();
    DEBUG_NTP_SYNCF("resolvePendingUserCreationTimes completed");
    DEBUG_NTP_SYNCF("About to call notifyAutomationScheduler");
    notifyAutomationScheduler();
    DEBUG_NTP_SYNCF("notifyAutomationScheduler completed");
    return true;
  } else {
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] TIMEOUT - NTP sync failed after %d seconds", maxWaitSeconds);
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] Check: WiFi=%s, DNS=%s, Gateway=%s",
                    WiFi.isConnected() ? "OK" : "FAIL",
                    WiFi.dnsIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str());
    
    // Try RTC as fallback time source
#if ENABLE_RTC_SENSOR
    if (gRtcEnabled && gRtcConnected) {
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
extern const char* cmd_pending_list(const String& argsInput);
// cmd_lightsleep now implemented above

// Main/Core command registry (commands that remain in main .ino file)
// Most commands have been modularized - this contains only core system commands
// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry commands[] = {
  // ---- Core / General ----
  { "status", "Show system status (WiFi, FS, memory).", false, cmd_status, nullptr, "system", "status" },
  { "uptime", "Show device uptime.", false, cmd_uptime },
  { "time", "Show device time (uptime + NTP if synced).", false, cmd_time },
  { "timeset", "Set time manually: timeset YYYY-MM-DD HH:MM:SS or <unix_timestamp>.", true, cmd_timeset },
  { "memsample", "Memory snapshot with component requirements. Use 'memsample track [on|off|reset|status]' for allocation tracking.", false, cmd_memsample },
  { "memreport", "Comprehensive memory report (Task Manager style).", false, cmd_memreport },
  { "fsusage", "Show filesystem usage.", false, cmd_fsusage },
  
  // ---- Testing Commands (Admin Only) ----
  { "testencryption", "Test WiFi password encryption (admin only).", true, cmd_testencryption },
  { "testpassword", "Test user password hashing (admin only).", true, cmd_testpassword },

  // ---- System Diagnostics ----
  { "temperature", "Read ESP32 internal temperature.", false, cmd_temperature },
  { "voltage", "Read supply voltage.", false, cmd_voltage },
  { "cpufreq", "Get/set CPU frequency.", true, cmd_cpufreq },
  { "taskstats", "Detailed task statistics.", false, cmd_taskstats },

  // ---- Misc ----
  { "reboot", "Reboot the system.", true, cmd_reboot, nullptr, "system", "reboot" },
  { "factoryreset", "Wipe user accounts and reboot to re-run setup wizard.", true,
    cmd_factoryreset,
    "Usage: factoryreset (no args, confirmation required)\n"
    "Deletes /system/users/users.json so the first-time setup wizard runs\n"
    "on next boot. WiFi credentials and other settings are preserved." },
  { "broadcast", "Send message to all or specific user.", true, cmd_broadcast },
  { "pendinglist", "List pending user requests.", true, cmd_pending_list },
  { "wait", "Delay execution for N milliseconds: wait <ms>.", false, cmd_wait },
  { "sleep", "Alias for wait: sleep <ms>.", false, cmd_wait },
  { "lightsleep", "Enter ESP32 light sleep: lightsleep [seconds] (default 20s).", true, cmd_lightsleep },
};

const size_t commandsCount = sizeof(commands) / sizeof(commands[0]);

// Registration handled by gCommandModules[] below

// Battery commands (from System_Battery.cpp)
#if ENABLE_BATTERY_MONITOR
extern const char* cmd_battery_status(const String& argsInput);
extern const char* cmd_battery_calibrate(const String& argsInput);
extern const char* cmd_batterylog(const String& argsInput);

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry batteryCommands[] = {
  {"batterystatus", "Show battery voltage, charge level, and status", false, cmd_battery_status, nullptr, "battery", "status"},
  {"batterycalibrate", "Recalibrate battery ADC readings", true, cmd_battery_calibrate},
  {"batterylog", "Battery time-series CSV log (on/off/interval/tail/clear)", false, cmd_batterylog, "Usage: batterylog [on|off|interval <s>|tail|clear]"}
};

const size_t batteryCommandsCount = sizeof(batteryCommands) / sizeof(batteryCommands[0]);
// Registration handled by gCommandModules[] below
#endif

// MQTT commands (from System_MQTT.cpp)
#if ENABLE_MQTT
extern const CommandEntry mqttCommands[];
extern const size_t mqttCommandsCount;
#endif

// LLM commands (from System_LLM.cpp)
#if ENABLE_ONDEVICE_LLM
extern const CommandEntry llmCommands[];
extern const size_t llmCommandsCount;
#endif

// Module registry - collects all command tables from modules
// Now includes metadata: description, flags, isConnected callback
// Order matters for help display; longest-match search handles conflicts
// Columns: name, description, commands, count, flags, isConnected
static const CommandModule gCommandModules[] = {
  { "cli",        "Help and CLI navigation", cliCommands,          cliCommandsCount, CMD_MODULE_CORE, nullptr },
  { "system",     "Core system commands", commands,             commandsCount, 0, nullptr },
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
  { "led",        "LED brightness and startup effects", ledCommands,           ledCommandsCount, 0, nullptr },
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
#if ENABLE_OLED_INPUT
  { "input",      "Input device (gamepad or ANO encoder)", inputCommands,       inputCommandsCount,       CMD_MODULE_SENSOR, []() { return gInputConnected; } },
#endif
#if ENABLE_GAMEPAD_SENSOR
  { "gamepad",    "Seesaw gamepad — raw debug commands", gamepadCommands,      gamepadCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("gamepad"); } },
#endif
#if ENABLE_ANO_ENCODER
  { "anoencoder", "ANO rotary encoder — debug + driver-specific config", anoEncoderCommands, anoEncoderCommandsCount, CMD_MODULE_SENSOR, []() { return gAnoEncoderConnected; } },
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
#if ENABLE_I2C_SYSTEM
  { "i2c",        "I2C bus diagnostics and scanning", i2cCommands,          i2cCommandsCount, 0, nullptr },
#endif
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
  { "features",   "System feature management", featureCommands,      featureCommandsCount, 0, nullptr },
#if ENABLE_CAMERA_SENSOR
  { "image",      "Image capture and management", imageCommands,        imageCommandsCount, 0, nullptr },
#endif
#if ENABLE_MAPS
  { "map",        "Map navigation and waypoints", mapCommands,          mapCommandsCount, 0, nullptr },
  { "mapsettings","Maps app settings (zoom, layers, cache)", mapsSettingCommands, mapsSettingCommandsCount, 0, nullptr },
#endif
  { "power",      "Power management", powerCommands,        powerCommandsCount, 0, nullptr },
#if ENABLE_OLED_DISPLAY
  { "setpattern", "OLED gamepad password entry", setPatternCommands,   setPatternCommandsCount, 0, nullptr },
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "even_g2",    "Even G2 smart glasses control", g2Commands,           g2CommandsCount, 0, nullptr },
  { "even_r1",    "Even R1 ring control (info-only)", g2RingCommands,    g2RingCommandsCount, 0, nullptr },
#endif
#if ENABLE_ONDEVICE_LLM
  { "llm",        "On-device LLM text generation", llmCommands,          llmCommandsCount, 0, nullptr },
#endif
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
const char* dispatchCommand(const String& argsInput) {
  const CommandEntry* entry = findCommand(argsInput);
  if (!entry) {
    return "Unknown command";
  }
  return entry->handler(argsInput);
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

// AllocEntry struct + gAllocTracker declared in System_MemUtil.h
extern int gAllocTrackerCount;
extern bool gAllocTrackerEnabled;

// External task watermark globals
extern volatile UBaseType_t gTofWatermarkNow, gTofWatermarkMin;
extern volatile UBaseType_t gImuWatermarkNow, gImuWatermarkMin;
extern volatile UBaseType_t gThermalWatermarkNow, gThermalWatermarkMin;

// Command/context types - shared header eliminates duplication
#include "System_CommandTypes.h"

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

  // heap_caps view — gives the truth about WHERE allocations land
  // (internal DRAM vs DMA-capable vs PSRAM) and how fragmented each
  // pool is. The "largest free block" is the indicator: when it
  // diverges from total free, the heap is fragmented and big allocs
  // (page-swap workers, image probes) will fail even with apparent
  // headroom.
  broadcastOutput("");
  broadcastOutput("-- HEAP CAPS (allocator-eye view) --");
  size_t cap_int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t cap_int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t cap_dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
  size_t cap_dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
  BROADCAST_PRINTF("  INTERNAL  free=%7lu B (%3lu KB)  largest=%7lu B  frag=%2u%%",
                   (unsigned long)cap_int_free, (unsigned long)(cap_int_free / 1024),
                   (unsigned long)cap_int_largest,
                   cap_int_free ? (unsigned)(100 - (cap_int_largest * 100) / cap_int_free) : 0);
  BROADCAST_PRINTF("  DMA-able  free=%7lu B (%3lu KB)  largest=%7lu B",
                   (unsigned long)cap_dma_free, (unsigned long)(cap_dma_free / 1024),
                   (unsigned long)cap_dma_largest);
  if (has_ps) {
    size_t cap_ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t cap_ps_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    BROADCAST_PRINTF("  SPIRAM    free=%7lu B (%4lu KB)  largest=%7lu B",
                     (unsigned long)cap_ps_free, (unsigned long)(cap_ps_free / 1024),
                     (unsigned long)cap_ps_largest);
  }

  broadcastOutput("");
  broadcastOutput("-- MEMORY BREAKDOWN (Hybrid Tracking) --");

  size_t total_known = 0;
  size_t tracked_total = 0;
  size_t tracked_dram = 0;  // DRAM-only subset — only this rolls into total_known

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
      tracked_dram += gAllocTracker[idx].dramBytes;

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
      // Add remaining to totals
      for (int i = displayed; i < gAllocTrackerCount; i++) {
        int idx = sorted[i];
        if (gAllocTracker[idx].isActive) {
          tracked_total += gAllocTracker[idx].totalBytes;
          tracked_dram += gAllocTracker[idx].dramBytes;
        }
      }
    }

    BROADCAST_PRINTF("  Subtotal (tracked): %6lu bytes (%3lu KB)  [DRAM %lu B, PSRAM %lu B]",
                    (unsigned long)tracked_total, (unsigned long)(tracked_total / 1024),
                    (unsigned long)tracked_dram, (unsigned long)(tracked_total - tracked_dram));
    // Only the DRAM portion contributes to the DRAM "TOTAL ACCOUNTED" roll-up.
    // PSRAM is accounted separately further down in this report.
    total_known += tracked_dram;
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
      { INPUT_TASK_NAME, INPUT_STACK_WORDS },
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
  size_t thermal_state_bytes = sizeof(gThermalCache) + sizeof(gThermalEnabled) + sizeof(gThermalConnected) + sizeof(gThermalTaskHandle);
  size_t imu_state_bytes = sizeof(gImuCache) + sizeof(gImuEnabled) + sizeof(gImuConnected) + sizeof(gImuTaskHandle);
  size_t tof_state_bytes = sizeof(gTofCache) + sizeof(gTofEnabled) + sizeof(gTofConnected) + sizeof(gTofTaskHandle);
  size_t gamepad_state_bytes = sizeof(gInputCache) + sizeof(gInputEnabled) + sizeof(gInputConnected) + sizeof(gInputTaskHandle);
  size_t apds_state_bytes = sizeof(gApdsCache) + sizeof(gApdsConnected) + sizeof(gApdsColorEnabled) + sizeof(gApdsProximityEnabled) + sizeof(gApdsGestureEnabled);
  size_t gps_state_bytes = sizeof(gGpsEnabled) + sizeof(gGpsConnected);
  size_t oled_state_bytes = sizeof(gOledEnabled) + sizeof(oledConnected);

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
  broadcastOutput("  [Y] THERMAL  | thermalTask() in i2csensor_mlx90640.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] THERMAL  | Disabled (~20-25KB flash, ~15KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_TOF_SENSOR
  broadcastOutput("  [Y] TOF      | tofTask() in i2csensor_vl53l4cx.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] TOF      | Disabled (~25-30KB flash, ~10KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_IMU_SENSOR
  broadcastOutput("  [Y] IMU      | imuTask() in i2csensor_bno055.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] IMU      | Disabled (~12-18KB flash, ~8KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_GAMEPAD_SENSOR
  broadcastOutput("  [Y] GAMEPAD  | inputTask() in i2csensor_seesaw.cpp");
  enabled_count++;
#elif ENABLE_ANO_ENCODER
  broadcastOutput("  [Y] ANO_ENC  | inputTask() in i2csensor_ano_encoder.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] INPUT    | Disabled (no input device built in)");
  disabled_count++;
#endif

#if ENABLE_APDS_SENSOR
  broadcastOutput("  [Y] APDS     | apdsTask() in i2csensor_apds9960.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] APDS     | Disabled (~6-10KB flash, ~4KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_GPS_SENSOR
  broadcastOutput("  [Y] GPS      | gpsTask() in i2csensor_pa1010d.cpp");
  enabled_count++;
#else
  broadcastOutput("  [N] GPS      | Disabled (~5-8KB flash, ~4KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_RTC_SENSOR
  broadcastOutput("  [Y] RTC      | DS3231 precision real-time clock");
  enabled_count++;
#else
  broadcastOutput("  [N] RTC      | Disabled (~3-5KB flash, ~1KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_FM_RADIO
  broadcastOutput("  [Y] FM RADIO | RDA5807 FM radio receiver");
  enabled_count++;
#else
  broadcastOutput("  [N] FM RADIO | Disabled (~3-5KB flash, ~1KB RAM saved)");
  disabled_count++;
#endif

#if ENABLE_PRESENCE_SENSOR
  broadcastOutput("  [Y] PRESENCE | STHS34PF80 IR presence/motion sensor");
  enabled_count++;
#else
  broadcastOutput("  [N] PRESENCE | Disabled (~3-5KB flash, ~1KB RAM saved)");
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
const char* cmd_memreport(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  printMemoryReport();
  return "Memory report printed to serial";
}



const char* cmd_taskstats(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Stream output line-by-line via broadcastOutput. Each broadcastOutput
  // call becomes one queue message capped at DEBUG_MSG_SIZE (256 B), so
  // we MUST emit each row individually — concatenating into one return
  // string would silently truncate at the third or fourth task.

  UBaseType_t taskCount = uxTaskGetNumberOfTasks();

  // Allocate task array in PSRAM, cached across invocations so repeated
  // taskstats calls don't churn the heap. Re-allocates only when the
  // task count grows.
  static TaskStatus_t* taskArray = nullptr;
  static UBaseType_t taskCap = 0;
  if (taskCount > taskCap) {
    if (taskArray) {
      free(taskArray);
      taskArray = nullptr;
      taskCap = 0;
    }
    taskArray = (TaskStatus_t*)ps_alloc(taskCount * sizeof(TaskStatus_t),
                                        AllocPref::PreferPSRAM, "taskstats");
    if (taskArray) taskCap = taskCount;
  }
  if (!taskArray) {
    return "Error: Unable to allocate memory for task statistics";
  }

  UBaseType_t actualCount = uxTaskGetSystemState(taskArray, taskCount, nullptr);

  broadcastOutput("Task Statistics:");
  broadcastOutput("=================");
  BROADCAST_PRINTF("Total Tasks: %u", (unsigned)taskCount);
  broadcastOutput("");
  broadcastOutput("Task Name          State  Prio  Stack");
  broadcastOutput("================== ===== ===== ======");

  for (UBaseType_t i = 0; i < actualCount; i++) {
    const char* state;
    switch (taskArray[i].eCurrentState) {
      case eRunning:   state = "RUN  "; break;
      case eReady:     state = "READY"; break;
      case eBlocked:   state = "BLOCK"; break;
      case eSuspended: state = "SUSP "; break;
      case eDeleted:   state = "DEL  "; break;
      default:         state = "UNK  "; break;
    }
    BROADCAST_PRINTF("%-18.18s %s %4u %5u",
                     taskArray[i].pcTaskName, state,
                     (unsigned)taskArray[i].uxCurrentPriority,
                     (unsigned)taskArray[i].usStackHighWaterMark);
  }

  return "OK";
}


// ============================================================================
// Command Execution Functions - MIGRATED from main .ino
// ============================================================================
// External dependencies for command execution
extern bool gAutoLogActive;
extern CLIState gCLIState;
extern bool gCLIValidateOnly;
extern QueueHandle_t gCmdExecQ;

// External functions
extern bool handleHelpNavigation(const String& cmd, char* out, size_t outSize);
extern String exitToNormalBanner();
extern String redactCmdForAudit(const String& argsInput);
extern bool hasAdminPrivilege(const AuthContext& ctx);
extern bool isAdminUser(const String& user);
extern void logAuthAttempt(bool success, const char* path, const String& user, const String& ip, const String& extra);

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
    { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactCmdForAudit(line).c_str()); logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf); }
    return false;
  }
  return true;
}

// Core command execution with authentication and registry dispatch
bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize) {
  // Clear output buffer
  out[0] = '\0';

  // A real user/peer command resets the power-save idle timer (and wakes the
  // device if it's dark). SOURCE_INTERNAL (automations, scheduler, internal
  // helpers) is excluded so background command execution can't pin the device
  // awake. This is what lets a headless box with no input device still power-
  // save yet wake on a serial / web / G2 / ESP-NOW command.
  if (ctx.transport != SOURCE_INTERNAL) {
    powerSaveNoteActivity();
  }

  // Install the command's identity + notification source into the calling
  // task's TLS slots for the duration of this call. CommandIdentityScope
  // composes ExecIdentityGuard + NotificationContextGuard and auto-derives
  // the NotificationSource from ctx.transport via a 1:1 mapping (see
  // System_AuthIdentity.cpp::transportToNotifSource — the single source of
  // truth for that mapping across the firmware). RAII restores on every
  // return path: success, auth failure, validation, internal error.
  // Cross-task identity isolation is structural — no other task can observe
  // this command's identity or notification context.
  CommandIdentityScope scope(ctx);

  // Resolve the full CommandContext so we can read automationName.
  // cmd_exec_task always calls setCurrentCommandContext(&r->ctx) before
  // invoking executeCommand, so this returns the correct ctx for queued
  // automation sub-commands. Direct callers that don't set it get nullptr,
  // which correctly suppresses autolog (non-automation paths).
  CommandContext* cmdCtx = static_cast<CommandContext*>(currentCommandContext());

  // Create command String once — reuse everywhere (avoids 5+ String(cmd) temporaries)
  String command = cmd;
  command.trim();

  DEBUG_CMD_FLOWF("[execCmd] user=%s ip=%s path=%s cmd=%.80s", ctx.user.c_str(), ctx.ip.c_str(), ctx.path.c_str(), cmd);

  // Centralized authorization (admin-required and future policies)
  if (!authorizeCommand(ctx, command, out, outSize)) {
    return false;
  }

  // Log command execution if automation logging is active and this command
  // carries an automationName (i.e. it was queued as an automation sub-command).
  if (gAutoLogActive && cmdCtx && cmdCtx->automationName[0]) {
    char logBuf[300];
    snprintf(logBuf, sizeof(logBuf), "[%s] %s", cmdCtx->automationName, cmd);
    appendAutoLogEntry("COMMAND", logBuf);
  }

  if (command.length() == 0) {
    strncpy(out, "Empty command", outSize - 1);
    out[outSize - 1] = '\0';
    return false;
  }

  // ===== REMOTE COMMAND ROUTING =====
  // Commands prefixed with "remote:" or "@" are sent to bonded device
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
    #if ENABLE_ESPNOW && ENABLE_BONDED_MODE
    extern bool isBondSynced();
    extern bool isBondSessionTokenValid();
    extern String buildBondedCommandPayload(const String& command);
    extern bool v4_send_frame(const uint8_t* dstMac, uint8_t type, uint16_t flags,
                              uint32_t msgId, const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);
    // Session-aware sender used by every other CMD send site (e.g. System_ESPNow.cpp
    // lines 9175/11249). AEAD-wraps in a SESSION_FRAME when a session is active;
    // otherwise queues the frame and kicks SESSION_OPEN (Phase 1 pending-frame path).
    // Used here so the bonded command — which carries the static @BOND token — is
    // never sent in cleartext on the air (a sniffed token = arbitrary RCE).
    extern bool v4_send_encrypted_or_queue(const uint8_t dst[6], uint8_t type, uint16_t baseFlags,
                                           uint32_t msgId, const uint8_t* plaintext, uint16_t plaintextLen,
                                           uint8_t ttl, char* outStatus = nullptr, size_t outStatusLen = 0);
    extern uint32_t generateMessageId();
    extern bool parseMacAddress(const String& macStr, uint8_t mac[6]);
    extern Settings gSettings;
    
    // Gate on valid session token — this is the actual auth credential needed
    // to send remote commands. Implies bond is synced on both roles.
    if (!isBondSessionTokenValid()) {
      strncpy(out, "Error: No session token - bond not ready or passphrase mismatch", outSize - 1);
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
    // Opcode MUST be ESPNOW_V4_TYPE_CMD (30). This was hardcoded to a literal 5
    // with a "/* ESPNOW_V4_TYPE_CMD */" comment that rotted when the opcode enum
    // was renumbered (CMD: 5 → 30). The result: every remote/bond command went
    // out as dead opcode 5, the peer logged "Unknown type 5" and dropped it, so
    // bond sensor toggles and all remote: commands silently did nothing. Use the
    // named constant so it can never drift from the handler registration again.
    //
    // SECURITY: send via v4_send_encrypted_or_queue (NOT raw v4_send_frame). The
    // payload is "@BOND:<32-hex static token>:<command>"; sent in cleartext, a
    // passive sniffer captures the token and can forge arbitrary commands (RCE on
    // the bonded peer). The session-aware sender AEAD-wraps the frame when a
    // session is active (synced bond mode always has one) and otherwise queues +
    // kicks SESSION_OPEN — so the token is never on-air in plaintext. This is the
    // same path every other CMD send already uses; bond was the lone exception.
    bool sent = v4_send_encrypted_or_queue(peerMac, ESPNOW_V4_TYPE_CMD, ESPNOW_V4_FLAG_ACK_REQ,
                                           msgId, (const uint8_t*)payload.c_str(), payload.length(), 1);
    
    if (sent) {
      snprintf(out, outSize, "Remote command sent: %s", actualCommand.c_str());
      broadcastOutput("[REMOTE] Sent to bonded device: " + actualCommand);
      return true;
    } else {
      strncpy(out, "Error: Failed to send remote command", outSize - 1);
      out[outSize - 1] = '\0';
      return false;
    }
    #else
    strncpy(out, "Error: Bond mode not available", outSize - 1);
    out[outSize - 1] = '\0';
    return false;
    #endif // ENABLE_ESPNOW && ENABLE_BONDED_MODE
  }
  
  // Continue with local command execution
  command = actualCommand;

  // Find command handler by case-insensitive prefix match
  String lc = command;
  lc.toLowerCase();

  const CommandEntry* found = nullptr;
  size_t foundLen = 0;

  // Interactive CLI modes (help today; wizard/confirm in the future) get
  // first crack at the input. The dispatch function returns true when the
  // active mode consumed the command -- response was written into `out`,
  // we return immediately. Returns false when no mode is active or the
  // mode wants normal command lookup to proceed.
  //
  // Concretely for help today: the active helpMode's onInput delegates to
  // the same handleHelpNavigation() the old code called directly. The new
  // call shape lets other modes register without each one growing its own
  // dispatcher hook line here.
  if (cliModeDispatchInput(command, out, outSize)) {
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
      if (!isHelpModeCommand(found->name)) {
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

        // Log output if this is an automation sub-command with logging active.
        if (gAutoLogActive && cmdCtx && cmdCtx->automationName[0]) {
          char logBuf[201];
          snprintf(logBuf, sizeof(logBuf), "%.197s%s", out, strlen(out) > 197 ? "..." : "");
          for (char* c = logBuf; *c; c++) { if (*c == '\n' || *c == '\r') *c = ' '; }
          appendAutoLogEntry("OUTPUT", logBuf);
        }

        // Command audit logging (always-on)
        bool success = (strncmp(out, "Error", 5) != 0) && (strncmp(out, "ERROR", 5) != 0);
        logCommandExecution(ctx, cmd, success, out);

        {
          char auditBuf[180];
          snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactCmdForAudit(command).c_str());
          logAuthAttempt(true, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf);
        }
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

    // Capture CLIMode state BEFORE the handler runs. If the handler enters
    // a mode (e.g. cmd_filedelete -> cliRequestConfirm) the command hasn't
    // actually completed -- it just prompted the user. The real completion
    // (and the right moment to audit) is when the user resolves the prompt
    // and the mode's onInput composes the audit line with full context.
    // See System_CLIConfirm::confirm_onInput for the resolution audit.
    bool modeWasActiveBeforeHandler = cliInModeActive();

    const char* result = found->handler(args);
    strncpy(out, result, outSize - 1);
    out[outSize - 1] = '\0';

    // Command audit logging (always-on, EXCEPT when the handler just
    // entered an interactive mode -- skip the prompt step so the audit
    // log doesn't claim a destructive action completed when it only asked
    // for confirmation. The mode is responsible for auditing the
    // resolution if it wants to (confirm mode does; help mode doesn't).
    const bool handlerEnteredMode = !modeWasActiveBeforeHandler && cliInModeActive();
    if (!handlerEnteredMode) {
      bool success = (strncmp(out, "Error", 5) != 0) && (strncmp(out, "ERROR", 5) != 0);
      logCommandExecution(ctx, cmd, success, out);
    } else {
      DEBUG_CMD_FLOWF("[execCmd] suppressing audit -- handler entered mode '%s'",
                      cliCurrentMode() && cliCurrentMode()->name
                        ? cliCurrentMode()->name : "(unnamed)");
    }
  } else {
    // Command not found
    snprintf(out, outSize, "Unknown command: %s\nType 'help' for available commands", command.c_str());
    
    // Log failed command lookup
    logCommandExecution(ctx, cmd, false, out);
  }
  // ===== END INLINED REGISTRY LOGIC =====

  // Log command output if this is an automation sub-command with logging active.
  if (gAutoLogActive && cmdCtx && cmdCtx->automationName[0]) {
    char logBuf[201];
    snprintf(logBuf, sizeof(logBuf), "%.197s%s", out, strlen(out) > 197 ? "..." : "");
    for (char* c = logBuf; *c; c++) { if (*c == '\n' || *c == '\r') *c = ' '; }
    appendAutoLogEntry("OUTPUT", logBuf);
  }

  // We don't have structured success/failure from registry handlers; assume success for audit purposes
  {
    char auditBuf[180];
    snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactCmdForAudit(command).c_str());
    logAuthAttempt(true, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf);
  }
  DEBUG_CMD_FLOWF("[execCmd] out_len=%zu", strlen(out));
  return true;
}

// Queued command execution with deadlock avoidance
bool submitAndExecuteSync(const Command& cmd, String& out) {
  DEBUG_CMD_FLOWF("[submitSync] cmd='%.80s' origin=%d user='%s'",
                  cmd.line.c_str(), (int)cmd.ctx.origin, cmd.ctx.auth.user.c_str());

  // If executor queue isn't ready (very early boot) fallback to direct call
  if (gCmdExecQ == nullptr) {
    // Allocate output buffer from PSRAM (2KB matches ExecReq.out size)
    char* outBuf = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "cmd.out.direct");
    if (!outBuf) {
      out = "Error: Out of memory for command output";
      return false;
    }
    // executeCommand installs the command identity via ExecIdentityGuard, so
    // the early-boot direct path no longer needs an outer save/restore. The
    // current command context pointer is still wired explicitly for the
    // broadcast-output mask plumbing — per-task TLS now (Stage 3).
    setCurrentCommandContext((void*)&cmd.ctx);
    bool ok = executeCommand((AuthContext&)cmd.ctx.auth, cmd.line.c_str(), outBuf, 2048);
    clearCurrentCommandContext();
    out = outBuf;
    free(outBuf);
    return ok;
  }

  // Allocate ExecReq from PSRAM since it's large (8KB+)
  ExecReq* r = (ExecReq*)ps_alloc(sizeof(ExecReq), AllocPref::PreferPSRAM, "cmd.exec.req");
  if (!r) {
    DEBUG_CMD_FLOWF("[submitSync] FAILED alloc ExecReq heap=%lu psram=%lu",
                    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
    broadcastOutput("[ERROR] Out of memory - cannot create request");
    return false;
  }
  // Initialize the structure (placement new for C++ objects)
  new (r) ExecReq();
  
  // Validate cmd.line before proceeding
  if (cmd.line.length() == 0) {
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Empty command");
    return false;
  }
  
  strncpy(r->line, cmd.line.c_str(), sizeof(r->line) - 1);
  r->line[sizeof(r->line) - 1] = '\0';
  r->ctx = cmd.ctx;
  
  r->done = xSemaphoreCreateBinary();
  if (!r->done) {
    DEBUG_CMD_FLOWF("[submitSync] FAILED semaphore heap=%lu", (unsigned long)ESP.getFreeHeap());
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Out of memory - cannot create semaphore");
    return false;
  }
  r->ok = false;

  // Enqueue and wait
  if (!gCmdExecQ) {
    vSemaphoreDelete(r->done);
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Command queue is NULL");
    return false;
  }
  BaseType_t queueResult = xQueueSend(gCmdExecQ, &r, pdMS_TO_TICKS(2000));
  
  if (queueResult != pdTRUE) {
    DEBUG_CMD_FLOWF("[submitSync] queue full for '%.40s'", r->line);
    vSemaphoreDelete(r->done);
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Command queue full - try again");
    return false;
  }
  
  DEBUG_CMD_FLOWF("[submitSync] queued '%.40s' waiting...", r->line);
  // Timeout bumped from 10s → 60s to cover PBKDF2 (~12 s) and any other
  // long-running synchronous command. The bigger fix is the `abandoned`
  // flag below — even if a future command runs longer than 60 s, we no
  // longer free `r` from under cmd_exec_task. See ExecReq::abandoned.
  if (xSemaphoreTake(r->done, pdMS_TO_TICKS(60000)) != pdTRUE) {
    Serial.printf("[DBG_CMD] [submitSync] TIMEOUT — abandoning r=%p line='%.60s'\n",
                  r, r->line);
    DEBUG_CMD_FLOWF("[submitSync] TIMEOUT for '%.40s' — handing ownership to cmd_exec_task",
                    r->line);
    // CRITICAL: cmd_exec_task may still be inside executeCommand at this
    // point. We MUST NOT free `r` or delete the semaphore — that would
    // produce a use-after-free or double-free. Instead, mark `r` as
    // abandoned; cmd_exec_task sees the flag after executeCommand returns
    // and takes over the cleanup (vSemaphoreDelete + ~ExecReq + free).
    r->abandoned = true;
    out = "[ERROR] Command timed out";
    return false;
  }

  out = r->out;  // Copy from char array to String
  bool ok = r->ok;

  vSemaphoreDelete(r->done);
  // Call destructor and free PSRAM
  r->~ExecReq();
  free(r);

  DEBUG_CMD_FLOWF("[submitSync] done ok=%d len=%d", ok ? 1 : 0, out.length());
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

// Deferred work submission — runs an arbitrary callback on cmd_exec_task,
// bypassing the CLI execution pipeline. Used by espnow_task to push heavy
// Ed25519 / X25519 work onto cmd_exec_task's deeper stack without bloating
// espnow_task's budget. The callback owns its arg's lifetime (must free it).
// Returns true if successfully queued.
bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg) {
  if (!fn) return false;
  if (gCmdExecQ == nullptr) return false;

  ExecReq* r = (ExecReq*)ps_alloc(sizeof(ExecReq), AllocPref::PreferPSRAM, "cmd.exec.deferred");
  if (!r) return false;
  new (r) ExecReq();

  r->deferredFn  = fn;
  r->deferredArg = arg;

  if (xQueueSend(gCmdExecQ, &r, 0) != pdTRUE) {
    r->~ExecReq();
    free(r);
    return false;
  }
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
void runUnifiedSystemCommand(const String& argsInput) {
  AuthContext actx;
  actx.transport = SOURCE_INTERNAL;
  actx.user = "system";
  actx.ip = String();
  actx.path = "/system";
  actx.opaque = nullptr;
  Command uc;
  uc.line = argsInput;
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
  char pathBuf[64];
  snprintf(pathBuf, sizeof(pathBuf), "/icons/%s.png", name);
  return String(pathBuf);
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

  // Parse: <username> <password> [transport]
  // transport can be: serial, display, bluetooth
  CommandArgs a(originalCmd);
  if (!a.hasMinArgs(2)) {
    return "Usage: login <username> <password> [transport]\nTransport: serial (default), display, bluetooth";
  }

  String username = a.arg(0);
  String password = a.arg(1);
  String transportStr = a.has(2) ? a.arg(2) : String("serial");
  transportStr.toLowerCase();

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
    } else if (rest == "g2") {
      // G2 has no credential login (pairing IS auth), but admins may want
      // to clear pairedByUser without un-pairing the lens. logoutTransport
      // calls g2PairedUserClear(); recovery is a fresh stamp via
      // `bleautoconnect g2-glasses on` from any authenticated session.
      transport = SOURCE_G2_GLASSES;
    } else {
      return "Invalid transport. Use: serial, display, bluetooth, or g2";
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
