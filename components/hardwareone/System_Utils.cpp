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
#include "System_RamFlush.h"
#include "System_Utils.h"
#include "System_Debug.h"
#include "System_TaskUtils.h"
#include "System_I2C.h"
#include "HAL_Input.h"          // For INPUT_TASK_NAME (gamepad vs ANO)
#include "System_User.h"
#include "System_AuthIdentity.h"  // ExecIdentityGuard (executeCommand + submitAndExecuteSync)
#include "System_BootState.h"     // bootStateGetBootCount / bootStateResetBootCount (NVS boot counter)
#include "System_CrashRecord.h"   // crashRecord* — RTC post-mortem record for `crashlog`
#include "System_SelfDevice.h"   // SelfDevice:: — local identity/heap/uptime/firmware (Stage 1 consolidation)
#include "System_Clock.h"        // Clock:: — epoch/sync/tz/format helpers (Stage 2)
#include <esp_sntp.h>            // sntp_set_time_sync_notification_cb — real-reply signal
                                 // (never also include lwip/apps/sntp.h in this TU)
#include "System_Command.h"
#include "System_SensorStubs.h"  // Stubs for disabled sensors/modules
#include "System_MemoryMonitor.h"
#include "System_Notifications.h"
#include "System_Events.h"  // cmd_events — system event register inspector

// Subsystem headers needed by buildSystemInfoJson() — the device-info JSON
// aggregator was moved here from WebServer_Server.cpp so the CLI `status json`
// no longer depends on the web server being compiled in. Each is feature-
// guarded to match the corresponding section of the builder. (ESP-NOW, BLE,
// SelfDevice, MemUtil, I2C, Clock are already included above.)
#if ENABLE_WIFI && ENABLE_MQTT
#include "System_MQTT.h"        // isMqttConnected()
#endif
#if ENABLE_G2_GLASSES
#include "G2_Glasses.h"         // isG2ClientInitialized(), isG2Connected()
#endif
#if ENABLE_ONDEVICE_LLM
#include "System_LLM.h"         // LLMStatus, llmGetStatus()
#endif
#if ENABLE_HTTP_SERVER
#include "WebServer_Server.h"   // server, gServerIsHttps, gSessions, MAX_SESSIONS
#endif
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
#if ENABLE_MICROPHONE
extern const CommandEntry micCommands[];
extern const size_t micCommandsCount;
extern bool micConnected;
bool audioAnySourceAvailable();   // HAL_Audio — a mic source (PDM or G2) is reachable
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
extern bool gAutomationLogActive;
extern String gAutomationLogFile;
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

// Reads the wall clock directly on every call. This used to project from a
// cached boot-us→epoch-us offset, but the cache latched a garbage offset on
// the first pre-sync log line (time() counts uptime, so `now > 0` passes at
// ~1 s) and only NTP and the ring path ever refreshed it — a `timeset`, an
// RTC sync, or the silent hourly SNTP correction left every subsequent
// prefix stamping the pre-step clock. One gettimeofday per log line makes
// every clock source self-heal on the very next line with zero per-source
// wiring, and removes a tearable shared int64 written from five tasks.
void getTimestampPrefixMsCached(char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  const int64_t nowMs = Clock::epochMillis();
  if (nowMs <= 0) return;  // no valid time
  const time_t sec = (time_t)(nowMs / 1000);
  // Pre-sync (or unsynced-RTC 1970/1980-era) clock → "" so callers keep
  // their existing [ms=...] / [BOOT ...] fallbacks. Contract unchanged.
  if (!Clock::isValidEpoch(sec)) return;
  const int ms = (int)(nowMs % 1000);
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

  // Pause sensor polling during file I/O to prevent I2C contention (RAII —
  // resumes on every return path; from System_PollPause.h via System_I2C.h).
  PollPauseGuard pollGuard;

  FsLockGuard guard("readText");
  File f = VFS::open(String(path), "r");
  if (!f) {
    return false;
  }
  out = f.readString();
  f.close();

  return true;
}

bool writeText(const char* path, const String& in) {
  PollPauseGuard pollGuard;   // pause sensor polling during file I/O (RAII)

  FsLockGuard guard("writeText");
  File f = VFS::open(String(path), "w", true);
  if (!f) {
    return false;
  }
  f.print(in);
  f.flush();
  f.close();

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

  // Rename failed: do NOT fall back to a truncate-in-place writeText(path,...).
  // "w" mode shreds the destination — the exact crash window this atomic write
  // exists to close (a power cut mid-write leaves an empty/half file). Drop the
  // tmp and fail loudly; the existing file is left fully intact and every caller
  // already checks the returned bool.
  VFS::remove(tmp);
  return false;
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
  
  // Status indicator
  const char* status = success ? "OK" : "FAIL";

  // Audit trail = WHO ran WHAT from WHERE + the OK/FAIL outcome — NOT the command
  // output. Result bodies (human text or JSON) are noise here: they already went
  // out the caller's own channel, and detailed failures live in the security /
  // error logs. So we record status only — the line stops at OK / FAIL.
  (void)result;

  // Format: [timestamp] user@source cmd -> status
  snprintf(entry, sizeof(entry), "[%lu] %s@%s %s -> %s",
           ts, ctx.user.c_str(), source, redactedCmd.c_str(), status);
  
  // Append to audit log with 500KB cap (rotates automatically)
  appendLineWithCap("/system/sys_logs/command-audit.log", entry, 500 * 1024);
  
  // Console echo of the "who did what from where" notice. This is OPERATIONAL
  // visibility (the broadcast domain) — always shown, like it has always been,
  // NOT a debug-gated item. (An earlier version gated it behind LOG_LEVEL_INFO +
  // a DEBUG_AUDIT flag; HW testing showed that hid it by default, which is wrong
  // for operational output — the audit trail is something you want to see, not a
  // debug trace. The durable copy is the file write above.)
  //
  // Console echo mirrors the file: status only — the line stops at OK / FAIL.
  // (Result bodies already went out the caller's own channel; echoing a preview
  // here was just noise.)
  char auditLine[384];
  snprintf(auditLine, sizeof(auditLine), "[CMD] %s@%s: %s -> %s",
           ctx.user.c_str(), source, redactedCmd.c_str(), status);
  extern void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route);
  // Exclude BLE: its notify characteristic is the command *response* channel
  // the app reads JSON replies from. Echoing the audit line onto it would
  // interleave with the reply. The echo still reaches serial / web / file /
  // OLED / G2.
  broadcastOutputCore_Routed(auditLine, strlen(auditLine), MSG_ROUTE_ALL & ~MSG_ROUTE_BLE);
}

// Automation logging
//
// Writes are gated by gAutomationLogOwnerCtx — the AuthContext captured when the
// user ran `autolog start`. See the comment on gAutomationLogOwnerCtx in
// System_Automation.cpp for the design rationale (captured identity vs.
// reading the current task's identity at write time, which fires from event
// triggers detached from any CLI session).
//
// If the captured ctx loses permission to the path mid-run (e.g. admin
// demoted to user), individual writes start failing — that's intentional.
bool appendAutoLogEntry(const char* type, const String& message) {
  if (!gAutomationLogActive || gAutomationLogFile.length() == 0) return false;
  if (!filesystemReady) return false;

  extern AuthContext gAutomationLogOwnerCtx;

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
  VFS::resolveOverflowPath(gAutomationLogFile.c_str(), line.length() + 512,
                           dest, sizeof(dest));

  // Ensure the parent directory exists on whichever FS we're writing to.
  // Both exists and mkdir go through the guarded path so the captured user
  // can't create directories they don't have CREATE perm on.
  String destStr(dest);
  int lastSlash = destStr.lastIndexOf('/');
  if (lastSlash > 0) {
    String dir = destStr.substring(0, lastSlash);
    if (!VFS::existsGuarded(dir, gAutomationLogOwnerCtx)) {
      if (!VFS::mkdirGuarded(dir, gAutomationLogOwnerCtx)) return false;
    }
  }

  File f = VFS::openGuarded(destStr, "a", gAutomationLogOwnerCtx);
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

  // Quote-aware token walk — mirrors CommandArgs: tokens split on spaces,
  // but a token opening with '"' runs to its closing quote (spaces inside
  // are token content, not separators). Redaction positions MUST agree with
  // the parser's argument positions: counting raw spaces let a quoted SSID
  // containing spaces shift the count, so the PASSWORD in
  // `wifiadd "My Net" "pass" 1 0` logged UNMASKED. Bare-token lines walk
  // identically to the old space count. Returns false if the line has fewer
  // than n tokens; on success start/end bound token n (end is one past).
  static bool nthTokenBounds(const String& s, int n, int& start, int& end) {
    int i = 0;
    const int len = (int)s.length();
    start = 0; end = 0;
    for (int t = 0; t < n; t++) {
      while (i < len && s[i] == ' ') i++;
      if (i >= len) return false;
      start = i;
      if (s[i] == '"') {
        int close = s.indexOf('"', i + 1);
        i = (close < 0) ? len : close + 1;
      } else {
        while (i < len && s[i] != ' ') i++;
      }
      end = i;
    }
    return true;
  }

  // Shared handler for peer-credential commands: "<cmd> <target> <username>
  // <password> <rest>..." (espnowremote / espnowbrowse / espnowfetch). Redact
  // ONLY the password — the username is audit-relevant (WHO ran it) and <rest>
  // (the remote command / path) stays visible (WHAT was run).
  static String redactPeerCredCmd(const String& in) {
    String c = in;
    int base = c.indexOf(' ');                      // after "espnowremote"
    if (base > 0) {
      int t1 = c.indexOf(' ', base + 1);                 // end of <target>
      int t2 = (t1 > 0) ? c.indexOf(' ', t1 + 1) : -1;   // end of <username>
      int t3 = (t2 > 0) ? c.indexOf(' ', t2 + 1) : -1;   // end of <password>
      if (t2 > 0) {
        String head = c.substring(0, t2 + 1);  // "espnowremote <target> <username> "
        String afterPass = (t3 > 0) ? c.substring(t3) : String();  // " <command>..."
        return head + "***" + afterPass;
      }
    }
    return c;
  }

  // usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>
  // Mask the THREE password tokens (positions 3, 6, 7); keep username(2),
  // device(4), and targetAdminUser(5) visible for the audit trail.
  static String redactUserSyncCmd(const String& in) {
    String c = in;
    const int kPwTokens[] = { 7, 6, 3 };  // high → low so edits don't shift earlier tokens
    for (int k = 0; k < 3; k++) {
      int pos = kPwTokens[k];
      int prevSpace = indexOfNthSpace(c, pos - 1);
      if (prevSpace < 0) continue;
      int nextSpace = c.indexOf(' ', prevSpace + 1);
      String head = c.substring(0, prevSpace + 1);
      String tail = (nextSpace > 0) ? c.substring(nextSpace) : String();
      c = head + "***" + tail;
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
    { "espnowremote ",     CALL_HANDLER,         0, &redactPeerCredCmd },  // <target> <user> <pass> <cmd>
    { "espnowbrowse ",     CALL_HANDLER,         0, &redactPeerCredCmd },  // <target> <user> <pass> [path]
    { "espnowfetch ",      CALL_HANDLER,         0, &redactPeerCredCmd },  // <target> <user> <pass> <path>
    { "blesecret ",        MASK_TOKEN_AT_POS,    2, nullptr },  // blesecret <passphrase>
    // Passphrase setters — MASK_AFTER_TOKEN_POS masks the whole rest of the line
    // (robust to quoted / spaced passphrases), keeping the mesh label visible.
    { "espnowsetpassphrase ",        MASK_AFTER_TOKEN_POS, 2, nullptr },  // espnowsetpassphrase <mesh> <passphrase>
    { "espnowmeshes setpassphrase ", MASK_AFTER_TOKEN_POS, 3, nullptr },  // espnowmeshes setpassphrase <label> <passphrase>
    // User credential commands — keep <username> visible where present, mask password(s).
    { "userchangepassword ", MASK_AFTER_TOKEN_POS, 1, nullptr },  // <curPass> <newPass> <confirmPass>
    { "userresetpassword ",  MASK_AFTER_TOKEN_POS, 2, nullptr },  // <username> <newPassword> [flag]
    // Mask ONLY the password token, not the rest of the line: useradd now grants
    // a ROLE, and masking everything after the username made "useradd bob *** 0 user"
    // and "... 0 superadmin" identical in the audit log — the one detail most worth
    // auditing was the one hidden. Same shape and reasoning as `login <user> <pass>`
    // above; CommandArgs splits on spaces, so a password can't contain one here.
    { "useradd ",            MASK_TOKEN_AT_POS,    3, nullptr },  // <username> <password> [0|1] [role]
    // usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>
    { "usersync ",           CALL_HANDLER,         0, &redactUserSyncCmd },  // mask pw tokens 3,6,7
  };
}

// =============================================================================
// settingBoolToggle — generic on/off CLI handler for persisted bools
// =============================================================================
// See System_Utils.h for the contract. Single static buffer because the
// CLI is single-threaded and result strings are consumed before the next
// command dispatches.

const char* settingBoolToggle(bool& field, const String& argsInput, const char* label) {
  EXT_RAM_BSS_ATTR static char buf[80];
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
  snprintf(buf, sizeof(buf), "Error: %s: invalid value (use on|off)", label);
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

    // Compute token boundaries QUOTE-AWARE (nthTokenBounds mirrors
    // CommandArgs) so a quoted argument containing spaces can't shift the
    // positions. Token positions are 1-based over the entire line
    // (including the command words).
    if (r.type == MASK_TOKEN_AT_POS) {
      int start = 0, end = 0;
      if (!nthTokenBounds(c, r.param, start, end)) return c;
      return c.substring(0, start) + "***" + c.substring(end);
    }

    if (r.type == MASK_AFTER_TOKEN_POS) {
      int start = 0, end = 0;
      if (!nthTokenBounds(c, r.param, start, end)) return c;
      return c.substring(0, end) + " ***";
    }
  }

  // No rule matched. If the VERB itself isn't a recognized command, mask
  // everything after it: a typo'd credential command ("logni <user> <pass>")
  // otherwise lands verbatim in the audit log — the rule table can never
  // enumerate misspellings, but "unknown verb ⇒ args are opaque" catches
  // them all. Recognized commands keep full args (that's the audit value);
  // an unknown verb's args have none.
  if (!findCommand(c)) {
    int sp = c.indexOf(' ');
    if (sp > 0) return c.substring(0, sp) + " ***";
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

  if (argWantsJson(originalCmd)) {
    snprintf(getDebugBuffer(), 1024, "{\"schema\":1,\"tempC\":%.1f,\"tempF\":%.1f}", tempC, tempF);
    return getDebugBuffer();
  }

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

  if (gThermalConnected && gThermalRunning) {
    estimatedCurrent += 23;  // MLX90640 typical
    broadcastOutput("Thermal Sensor: Active (+23mA)");
  }

  if (gImuConnected && gImuRunning) {
    estimatedCurrent += 12;  // BNO055 typical
    broadcastOutput("IMU Sensor: Active (+12mA)");
  }

  if (gTofConnected && gTofRunning) {
    estimatedCurrent += 20;  // VL53L4CX typical
    broadcastOutput("ToF Sensor: Active (+20mA)");
  }

  if (gApdsConnected) {
    estimatedCurrent += 3;  // APDS9960 typical
    broadcastOutput("APDS Sensor: Active (+3mA)");
  }

  if (argWantsJson(originalCmd)) {
    snprintf(getDebugBuffer(), 1024,
      "{\"schema\":1,\"measured\":false,\"estimatedCurrentMa\":%.0f,\"estimatedPowerW\":%.2f,"
      "\"note\":\"estimate only — see batterystatus json for measured\"}",
      estimatedCurrent, (estimatedCurrent * 3.3) / 1000.0);
    return getDebugBuffer();
  }

  broadcastOutput("");
  BROADCAST_PRINTF("Estimated Current Draw: %.0fmA", estimatedCurrent);
  BROADCAST_PRINTF("Estimated Power (3.3V): %.2fW", (estimatedCurrent * 3.3) / 1000.0);
  broadcastOutput("");
  broadcastOutput("Note: Direct voltage measurement requires external ADC connection");
  cliHint("these are estimates, not a reading — for measured battery volts and charge, run 'batterystatus'");

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

// Deep sleep with no wake source — closest ESP32 analogue to "power off".
// Wake only via the physical reset button. Used by G2 Power Off (and CLI).
const char* cmd_deepsleep(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  unsigned long cooldownRemain = 0;
  if (!powerSleepTransitionAllowed(&cooldownRemain)) {
    snprintf(getDebugBuffer(), 1024,
             "Deep sleep refused: cooldown active — try again in %lu ms (powercooldown to tune)",
             cooldownRemain);
    return getDebugBuffer();
  }

  BROADCAST_PRINTF("Entering deep sleep (wake via reset button)...");
  powerSleepTransitionMark();
  delay(100);
  // Let any pending UI (G2 lens banner / OLED) paint before the rails drop.
  delay(700);
  oledPrepareForSleep();
  {
    extern void batteryLogEvent(const char* event);
    batteryLogEvent("sleep:deep");
  }
  // No wake source: ~10 µA until physical reset.
  esp_deep_sleep_start();
  return "Deep sleep";  // unreachable
}

// =========================================================================
// Core System Commands (moved from .ino)
// =========================================================================

// ---------------------------------------------------------------------------
// Output contract — opt-in structured (JSON) output
// ---------------------------------------------------------------------------
// Per the firmware output contract (see executeCommand below), a command has
// two output channels with different shapes:
//   • broadcastOutput()  — human, line-oriented, <=255 B per call, live-
//                          streamed + decorated. The default for people.
//   • the return value    — one verbatim blob, <=4 KB, delivered once. The
//                          channel for byte-exact / machine-readable output.
// When the caller passes a standalone `json` token, the command must emit its
// payload ONLY via the return value (a single JSON document) and call NO
// broadcastOutput — otherwise the stream and the blob interleave and the JSON
// is unparseable.
//
// argWantsJson() detects the `json` token at a word boundary so it won't
// false-match an argument that merely contains the substring (e.g. a filename).
// Declared in System_Utils.h — shared by info commands across translation units
// (status, devices, …) as the JSON contract rolls out.
bool argWantsJson(const String& args) {
  String a = args;
  a.trim();
  if (a.length() == 0) return false;
  int idx = a.indexOf("json");
  while (idx >= 0) {
    bool leftOk  = (idx == 0) || (a[idx - 1] == ' ');
    bool rightOk = (idx + 4 >= (int)a.length()) || (a[idx + 4] == ' ');
    if (leftOk && rightOk) return true;
    idx = a.indexOf("json", idx + 4);
  }
  return false;
}

// argLeadingTokenIsJson() — true iff the FIRST space-delimited token is exactly
// "json". Use this (NOT argWantsJson) for commands where "json" must be
// positional because later tokens are real arguments that may themselves be or
// contain a "json" token — e.g. `llm json <prompt>` (a prompt may say "json")
// or `files json <path>` / `files /logs json` (a path may contain "json").
// argWantsJson would false-trigger on those; this only matches the lead token.
bool argLeadingTokenIsJson(const String& args) {
  String a = args;
  a.trim();
  return a == "json" || a.startsWith("json ");
}

// Reset reason labels matching esp_reset_reason_t (shared by both output paths).
//
// This table used to stop at "SDIO" (11 entries) while the enum runs to
// ESP_RST_CPU_LOCKUP = 15, and both consumers guarded with `reason < 11`. So
// USB (11), JTAG (12), EFUSE (13), PWR_GLITCH (14) and CPU_LOCKUP (15) all
// rendered as "Unknown" over web / BLE / MQTT — and USB is the reason you get
// from an ordinary reflash or replug, i.e. one of the most common of all.
// Indices must stay aligned with esp_reset_reason_t.
static const char* const kResetReasonLabels[] = {
  "Unknown", "Power-on", "External", "Software", "Panic",
  "Int WDT", "Task WDT", "WDT", "Deepsleep", "Brownout", "SDIO",
  "USB", "JTAG", "eFuse", "Power glitch", "CPU lockup"
};
static const size_t kResetReasonLabelCount = sizeof(kResetReasonLabels) / sizeof(kResetReasonLabels[0]);

// Single accessor so a future enum extension can't leave one consumer behind
// again (this is what let the two `reason < 11` sites drift out of sync).
static const char* resetReasonLabel(uint32_t reason) {
  return (reason < kResetReasonLabelCount) ? kResetReasonLabels[reason] : "Unknown";
}

// ---------------------------------------------------------------------------
// buildSystemInfoJson — single source of truth for device-info JSON
// ---------------------------------------------------------------------------
// Moved here from WebServer_Server.cpp (which is wholly #if ENABLE_HTTP_SERVER)
// so the CLI `status json` can use it WITHOUT depending on the web server being
// compiled in. The web / SSE / migration callers still reach it via the
// declaration in WebServer_Server.h — one schema, one definition, many
// consumers (CLI status json, /api/system, SSE, migration tool).
//
// includeDeviceList=false drops the only unbounded section (the I2C deviceList
// array) so the CLI/MQTT blob stays compact (~800 B) and fits even the 2 KB
// MQTT result buffer. Web callers pass the default (true) for the full nested
// view. NEVER emit secrets here — this is read over BLE / MQTT / web.
void buildSystemInfoJson(JsonDocument& doc, bool includeDeviceList) {
  // Schema version + identity + last-reset info. Added when the builder moved
  // to core so the CLI status view and the web dashboard share one schema.
  doc["schema"]     = 1;
  doc["fw"]    = SelfDevice::firmwareVersion();
  doc["board"] = BOARD_NAME;
  {
    uint32_t reason = gSettings.lastResetReason;
    doc["reset_reason"]      = resetReasonLabel(reason);
    doc["reset_reason_code"] = (unsigned long)reason;
    doc["crash_count"]       = (unsigned long)gSettings.crashCount;
  }

  // System time. "" if not yet synced — UI uses empty as the sentinel.
  // Format is "YYYY-MM-DD HH:MM:SS" (space separator, not 'T') to match
  // the historical public-API shape; this is the only Clock-using site
  // that emits this exact format so it stays inline.
  if (Clock::isSynced()) {
    time_t now = Clock::epochSeconds();
    struct tm tminfo;
    localtime_r(&now, &tminfo);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tminfo);
    doc["system_time"] = timeBuf;
  } else {
    doc["system_time"] = "";
  }

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
  // Two separate axes: WiFi CONNECTION (associated to an AP) and RADIO power
  // (the radio is up at all — WiFi OR ESP-NOW holding it). Emitted BEFORE the
  // connection branch so they are ALWAYS present — the radio can be ON while
  // WiFi is Disconnected (held for ESP-NOW), the case the two indicators exist
  // to show. Feeds the web dashboard, settings page, and the phone app.
  net["wifiConnected"] = WiFi.isConnected();
#if ENABLE_WIFI
  net["radioOn"] = wifiRadioOn();
  net["radioHeldForEspnow"] = (wifiRadioState() == WIFI_RADIO_UP_FOR_ESPNOW);
#else
  net["radioOn"] = false;
  net["radioHeldForEspnow"] = false;
#endif
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

  // Memory info (nested object with KB values). ESP.getHeapSize/getPsramSize
  // remain inline — those are constants for the build, not duplicated state.
  JsonObject mem = doc["mem"].to<JsonObject>();
  mem["heap_free_kb"] = (int)(SelfDevice::freeHeapBytes() / 1024);
  mem["heap_total_kb"] = (int)(ESP.getHeapSize() / 1024);
  mem["psram_total_kb"] = (int)(ESP.getPsramSize() / 1024);
  mem["psram_free_kb"] = (int)(SelfDevice::psramFreeBytes() / 1024);

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
    espnow["enabled"] = gSettings.espnowEnabled;
    espnow["running"] = (gEspNow && gEspNow->initialized);
    espnow["mesh"] = gSettings.espnowmesh;
    espnow["deviceName"] = gSettings.espnowDeviceName;
    espnow["encrypted"] = (gEspNow && gEspNow->encryptionEnabled);
    espnow["passphraseSet"] = (gSettings.meshes[0].passphrase.length() > 0);
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
    // Aggregate-status pattern (mirrors ESPNow mesh-or-direct): subsystem reports
    // running when EITHER the BLE server OR the G2 client is initialized.
    JsonObject bt = conn["bluetooth"].to<JsonObject>();
    bt["running"] = bleSubsystemActive();
    bt["state"]   = bleSubsystemStateString();
    bt["mode"]    = getBleModeString();   // "server" | "client"
    bt["server"]  = isBLERunning();
#if ENABLE_G2_GLASSES
    bt["client"]    = isG2ClientInitialized();
    bt["g2Connected"] = isG2Connected();
#else
    bt["client"]    = false;
    bt["g2Connected"] = false;
#endif
  }
#endif

#if ENABLE_HTTP_SERVER
  {
    JsonObject ws = conn["webserver"].to<JsonObject>();
    bool running = (server != nullptr);
    ws["running"] = running;
    ws["https"] = (running && gServerIsHttps);
    ws["port"] = (running && gServerIsHttps) ? 443 : 80;
    int active = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
      if (gSessions[i].sid.length() > 0) active++;
    }
    ws["sessions"] = active;
    ws["maxSessions"] = MAX_SESSIONS;
  }
#endif

#if ENABLE_I2C_SYSTEM
  {
    JsonObject i2c = conn["i2c"].to<JsonObject>();
    i2c["compiled"] = true;
    i2c["enabled"]  = gI2CBusRunning;
    // Honest, contradiction-proof counts (NOT the manager registry, which
    // drifts via phantom bus-0 pre-registrations + ever-talked accounting):
    //   devices       = compiled sensor TYPES (capability)
    //   activeDevices = devices physically connected NOW; same source as the
    //                   deviceList below, so it can never exceed/contradict it.
    extern int i2cCompiledSensorCount();
    extern int i2cConnectedDeviceCount();
    i2c["devices"]       = i2cCompiledSensorCount();
    i2c["activeDevices"] = i2cConnectedDeviceCount();
    i2c["sdaPin"]   = gSettings.i2cSdaPin;
    i2c["sclPin"]   = gSettings.i2cSclPin;
    // Detected-device list: everything the I2C discovery scan physically found
    // on the buses. Omitted in compact mode (CLI/MQTT) since it is the only
    // unbounded section. Built by the SAME helper that backs `devices json`
    // (defined in System_I2C.cpp) so the two views never diverge.
    if (includeDeviceList) {
      extern void buildI2cDeviceListJson(JsonArray& arr, bool verbose = false);
      JsonArray devs = i2c["deviceList"].to<JsonArray>();
      buildI2cDeviceListJson(devs);
    }
  }
#else
  {
    JsonObject i2c = conn["i2c"].to<JsonObject>();
    i2c["compiled"] = false;
    i2c["enabled"]  = false;
    i2c["devices"]  = 0;
  }
#endif

#if ENABLE_ONDEVICE_LLM
  {
    LLMStatus llmSt = llmGetStatus();
    const char* llmStateStr = "UNLOADED";
    switch (llmSt.state) {
      case LLMState::LOADING:    llmStateStr = "LOADING"; break;
      case LLMState::READY:      llmStateStr = "READY"; break;
      case LLMState::GENERATING: llmStateStr = "GENERATING"; break;
      case LLMState::ERROR:      llmStateStr = "ERROR"; break;
      default: break;
    }
    JsonObject llm = conn["llm"].to<JsonObject>();
    llm["state"] = llmStateStr;
    const char* slash = strrchr(llmSt.modelPath, '/');
    llm["model"] = slash ? (slash + 1) : llmSt.modelPath;
    llm["psramKB"] = (unsigned)((llmSt.totalPsramUsed + 512) / 1024);
    llm["tokPerSec"] = llmSt.lastTokensPerSec;
  }
#endif
}

const char* cmd_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  uint32_t reason = gSettings.lastResetReason;
  const char* reasonLabel = (reason < 11) ? kResetReasonLabels[reason] : "Unknown";
  size_t psTot = ESP.getPsramSize();

  // ---- Structured path: delegate to the shared buildSystemInfoJson() (the
  //      same builder /api/system uses) in COMPACT form, and serialize verbatim
  //      into a persistent PSRAM buffer. No broadcastOutput; no Arduino String
  //      (which would land in DRAM) — keeps the small-buffer / DRAM discipline.
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    buildSystemInfoJson(doc, /*includeDeviceList=*/false);
    static char* statusJsonBuf = nullptr;
    if (!statusJsonBuf) {
      statusJsonBuf = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "status.json");
    }
    if (!statusJsonBuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, statusJsonBuf, 2048);
    return statusJsonBuf;
  }

  // ---- Human path: live lines via broadcastOutput, short status return.
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

  if (psTot > 0) {
    BROADCAST_PRINTF("  Free PSRAM: %lu bytes", (unsigned long)SelfDevice::psramFreeBytes());
    BROADCAST_PRINTF("  Total PSRAM: %lu bytes", (unsigned long)psTot);
  }

  BROADCAST_PRINTF("  Last Reset: %s", reasonLabel);
  BROADCAST_PRINTF("  Crash Count: %lu", (unsigned long)gSettings.crashCount);

  return "OK";
}

const char* cmd_uptime(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  uint32_t seconds = SelfDevice::uptimeSeconds();
  uint32_t minutes = seconds / 60;
  uint32_t hours = minutes / 60;

  // Structured path: numbers + fixed-format string only (no escaping needed),
  // returned verbatim via a small PSRAM buffer. No broadcastOutput.
  if (argWantsJson(argsInput)) {
    static char* buf = nullptr;
    if (!buf) buf = (char*)ps_alloc(192, AllocPref::PreferPSRAM, "uptime.json");
    if (!buf) return "{\"error\":\"oom\"}";
    snprintf(buf, 192,
      "{\"schema\":1,\"uptime_s\":%lu,\"uptime_ms\":%lu,\"uptime_hms\":\"%luh %lum %lus\"}",
      (unsigned long)seconds, (unsigned long)millis(),
      (unsigned long)hours, (unsigned long)(minutes % 60), (unsigned long)(seconds % 60));
    return buf;
  }

  BROADCAST_PRINTF("Uptime: %luh %lum %lus",
                   (unsigned long)hours, (unsigned long)(minutes % 60), (unsigned long)(seconds % 60));
  return "[System] Uptime displayed";
}

const char* cmd_time(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Structured path: resolve the active time source (RTC primary, NTP fallback)
  // and emit one verbatim JSON blob via a small PSRAM buffer. No broadcastOutput.
  // All values are digits / a fixed time format, so manual building is safe.
  if (argWantsJson(argsInput)) {
    char timeBuf[40] = "";
    bool haveTime = false;
    bool haveTemp = false;
    float temp = 0.0f;
    // Displayed time: live RTC read when the hardware is present (deliberate
    // — do not replace with the system clock), else the system clock.
#if ENABLE_RTC_SENSOR
    if (gRtcRunning && gRtcConnected) {
      RTCDateTime dt;
      if (rtcReadDateTime(&dt)) {
        // RTC stores UTC; every human-facing surface shows local (ds3231.h
        // contract). This branch used to skip the conversion, so the same
        // "time" field was UTC on RTC boards but local on NTP-only boards.
        RTCDateTime local = rtcLocalTime(&dt);
        snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
                 local.year, local.month, local.day, local.hour, local.minute, local.second);
        haveTime = true; haveTemp = true; temp = rtcReadTemperature();
      }
    }
#endif
    if (!haveTime) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 0)) {
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
        haveTime = true;
      }
    }
    // "source" is the ledger: which custody chain actually stepped the
    // clock THIS boot (rtc/ntp/manual/ring). synced:true + source:"none"
    // means the time was carried across a soft reboot in the RTC domain.
    static char* buf = nullptr;
    if (!buf) buf = (char*)ps_alloc(256, AllocPref::PreferPSRAM, "time.json");
    if (!buf) return "{\"error\":\"oom\"}";
    int p = snprintf(buf, 256,
      "{\"schema\":1,\"synced\":%s,\"source\":\"%s\",\"time\":\"%s\",\"uptime_ms\":%lu",
      Clock::isSynced() ? "true" : "false",
      Clock::syncSourceName(Clock::syncSource()), timeBuf,
      (unsigned long)millis());
    if (haveTemp && p > 0 && p < 256) {
      p += snprintf(buf + p, 256 - p, ",\"rtc_temp_c\":%.1f", temp);
    }
    if (p > 0 && p < 256) snprintf(buf + p, 256 - p, "}");
    return buf;
  }

  // Show uptime in milliseconds
  unsigned long uptimeMs = millis();
  BROADCAST_PRINTF("Uptime: %lu ms", uptimeMs);
  
  // Priority: RTC (primary) -> NTP (fallback)
#if ENABLE_RTC_SENSOR
  if (gRtcRunning && gRtcConnected) {
    // RTC is primary display source (live register read — deliberate).
    RTCDateTime dt;
    if (rtcReadDateTime(&dt)) {
      RTCDateTime local = rtcLocalTime(&dt);  // RTC stores UTC; show local
      char timeBuf[32];
      snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
               local.year, local.month, local.day, local.hour, local.minute, local.second);
      BROADCAST_PRINTF("Time: %s (RTC)", timeBuf);
      BROADCAST_PRINTF("Temp: %.1f C", rtcReadTemperature());
      return "OK";
    }
  }
#endif

  // Fallback to system time if RTC not available
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    const Clock::SyncSource src = Clock::syncSource();
    // "none" while synced = time carried across a soft reboot in hardware.
    BROADCAST_PRINTF("Time: %s (source: %s)", timeBuf,
                     (src == Clock::SYNC_NONE && Clock::isSynced())
                         ? "carried over soft reboot"
                         : Clock::syncSourceName(src));
  } else {
    broadcastOutput("Time: Not synced (no time source yet)");
  }

  return "OK";
}

const char* cmd_timeset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    return "Error: invalid arguments — Usage: timeset YYYY-MM-DD HH:MM:SS  or  timeset <unix_timestamp>";
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
    // strtoll, not String::toInt (32-bit — silently wraps past 2038), and
    // range-checked before the time_t cast.
    char* endp = nullptr;
    const long long v = strtoll(arg.c_str(), &endp, 10);
    if (!endp || *endp != '\0') {
      return "Error: Invalid format. Use: YYYY-MM-DD HH:MM:SS or unix timestamp";
    }
    t = (time_t)v;
  } else {
    // Parse YYYY-MM-DD HH:MM:SS
    int year, month, day, hour, minute, second;
    if (sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d",
               &year, &month, &day, &hour, &minute, &second) != 6) {
      return "Error: Invalid format. Use: YYYY-MM-DD HH:MM:SS or unix timestamp";
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

  // Plausibility gate on BOTH branches: isValidEpoch alone has no upper
  // bound, and `timeset 100` used to silently un-sync a good clock.
  if (!Clock::isPlausibleEpoch(t)) {
    return "Error: timestamp must be within 2020-2099";
  }

  // Set system time, then run the standard post-step duties (event, boot
  // anchor, pending-user resolve, scheduler wake, RTC write-back) through
  // the shared chokepoint — this command used to skip all of them. Flush
  // synchronously so the side effects are visible before the prompt returns
  // (cmd_exec task has the stack for it).
  const time_t before = time(nullptr);
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  Clock::clockStepped(Clock::SYNC_MANUAL, before);
  Clock::clockDutiesFlush();

  char timeBuf[32];
  struct tm setTm;
  localtime_r(&t, &setTm);
  strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &setTm);
  BROADCAST_PRINTF("Time set to: %s", timeBuf);

  return "OK";
}

const char* cmd_fsusage(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool wantJson = argWantsJson(argsInput);
  if (!filesystemReady) {
    if (wantJson) return "{\"schema\":1,\"ready\":false}";
    broadcastOutput("Error: LittleFS not ready");
    return "ERROR";
  }

  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;
  unsigned int usagePercent = (usedBytes * 100) / (totalBytes == 0 ? 1 : totalBytes);

  // JSON to the caller only (no broadcastOutput); text path below unchanged.
  if (wantJson) {
    snprintf(getDebugBuffer(), 1024,
             "{\"schema\":1,\"ready\":true,\"totalBytes\":%lu,\"usedBytes\":%lu,\"freeBytes\":%lu,\"usagePercent\":%u}",
             (unsigned long)totalBytes, (unsigned long)usedBytes,
             (unsigned long)freeBytes, usagePercent);
    return getDebugBuffer();
  }

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
    return "Error: invalid arguments — Usage: testencryption <password_to_test>";
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
    return "Error: invalid arguments — Usage: testpassword <password_to_test>";
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

// Reboot helpers — see System_Utils.h. Every intentional restart routes through
// recordRebootIntent() so it (1) stashes a reason the NEXT boot turns into a typed
// SYSEVT_REBOOT (the in-RAM event ring can't survive the restart) and (2) writes the
// durable REBOOT audit line. rebootDevice() adds the inline flush + restart for
// simple sites; deferred sites (factoryreset's esp_timer) call recordRebootIntent()
// directly and restart on their own schedule.
void recordRebootIntent(const char* reason, const char* auditDetail) {
  // Capture WHO triggered the reboot now — we're still in their command scope.
  // The event posts on the next boot, where that identity is gone, so stash it
  // in RTC and replay it as the event's source/who.
  uint8_t src = transportToNotifSource(currentAuthContext().transport);
  if (src == NOTIF_SOURCE_UNKNOWN) src = NOTIF_SOURCE_SYSTEM;
  rebootStashReason(reason, currentExecUser().c_str(), src);
  logSystemEvent("REBOOT", "%s", (auditDetail && auditDetail[0]) ? auditDetail
                                 : (reason ? reason : "intentional restart"));
  // Push any pending typed events to events.log now (bypassing the 2s throttle)
  // so the last couple seconds of history isn't lost to the restart. The
  // caller's flush delay then lets the background writer drain to flash.
  systemEventLogFlush();
}

void rebootDevice(const char* reason, const char* auditDetail, uint32_t flushDelayMs) {
  recordRebootIntent(reason, auditDetail);
  delay(flushDelayMs);  // let the queued output + REBOOT audit line flush to disk
  ESP.restart();
}

const char* cmd_reboot(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  broadcastOutput("Rebooting system...");
  // A BOOT event with no preceding REBOOT event = power cut or crash, not a
  // commanded restart. rebootDevice() also stashes the reason so the next boot
  // posts SYSEVT_REBOOT (reason=command).
  char detail[96];
  snprintf(detail, sizeof(detail), "commanded restart (reboot) by '%s'", currentExecUser().c_str());
  rebootDevice("command", detail, 1000);
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
  EXT_RAM_BSS_ATTR static char respBuf[200];

  // Use the internal SYSTEM auth to delete users.json. The VFS path-rule
  // table grants admin only PERM_READ over /system/* -- only system auth
  // has PERM_DELETE. This is by design: an admin-credentials compromise
  // shouldn't be able to wipe accounts (which would lock everyone out
  // and trigger FTS the next boot). factoryreset is gated by the
  // requiresAdmin=true flag in the registry + the confirm prompt; system
  // auth is just the right capability to actually carry out the delete.
  // RTC survives a factory reset, so drop any stashed ramflush overlay — otherwise
  // a device that was reset could come back resuming the feature set it just wiped.
  ramFlushClearOverlay();

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

  // Record the imminent restart: stash the reason for the next boot's SYSEVT_REBOOT
  // and write the durable REBOOT audit line. Deferred via the esp_timer above, so we
  // record-only here rather than using rebootDevice()'s inline restart.
  recordRebootIntent("factory", "factory reset — rebooting to setup wizard");

  // users.json is gone and the reboot is scheduled — the reset is committed.
  systemEventPost(SYSEVT_FACTORY_RESET, currentExecUser().c_str());

  logSystemEvent("SETUP", "factory reset — %s deleted; rebooting to setup wizard", USERS_JSON_FILE);

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
  if (msg.length() == 0) return "Error: invalid arguments — Usage: broadcast <message>";
  broadcastOutput(msg);
  return "[System] Message broadcast";
}

const char* cmd_wait(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = argsInput;
  val.trim();
  if (val.length() == 0) return "Error: invalid arguments — Usage: wait <ms>";
  int ms = val.toInt();
  if (ms > 0 && ms <= 60000) delay(ms);
  return "[System] Wait complete";
}

// =========================================================================
// NTP Time Synchronization (moved from .ino)
// =========================================================================

#if ENABLE_WIFI

extern void resolvePendingUserCreationTimes();
extern void writeBootAnchor();
extern void logTimeSyncedMarkerIfReady(const char* source);

// ---------------------------------------------------------------------------
// SNTP sync notification — the only honest "an NTP server actually replied"
// signal. lwIP's daemon steps the clock itself (first request after a short
// startup delay, then hourly per CONFIG_LWIP_SNTP_UPDATE_DELAY), and before
// this callback existed those steps were completely invisible: no event, no
// log, and syncNTPAndResolve's old getLocalTime() "success" test couldn't
// tell a reply from a clock that was already set.
//
// The callback runs on lwIP's tcpip task (~3 KB stack): STORE-ONLY — no
// broadcast, no filesystem, no event post. ntpSyncDrainTick() (main loop,
// the ONLY caller) hands the stored facts to Clock::clockStepped(), which
// owns every post-step duty.
// ---------------------------------------------------------------------------
static volatile uint32_t gSntpSyncCount    = 0;
static volatile bool     gSntpDrainPending = false;
// 64-bit on a 32-bit core — written on lwIP's task, read on the main loop.
// Guarded by its own mux so the drain can never see a torn half-write.
static portMUX_TYPE      gSntpMux          = portMUX_INITIALIZER_UNLOCKED;
static int64_t           gSntpPrevProjUs   = 0;

static void onSntpTimeSync(struct timeval* tv) {
  (void)tv;
  // lwIP already stepped the real clock before invoking us; the projection
  // reference is the only remaining evidence of the PRE-step clock.
  const int64_t prev = Clock::projectedEpochUs();
  portENTER_CRITICAL(&gSntpMux);
  gSntpPrevProjUs = prev;
  portEXIT_CRITICAL(&gSntpMux);
  Clock::refreshProjection();
  gSntpSyncCount = gSntpSyncCount + 1;
  gSntpDrainPending = true;  // release: flag last
}

void ntpSyncDrainTick() {
  if (!gSntpDrainPending) return;
  gSntpDrainPending = false;
  portENTER_CRITICAL(&gSntpMux);
  const int64_t prevUs = gSntpPrevProjUs;
  portEXIT_CRITICAL(&gSntpMux);
  const time_t before = (time_t)(prevUs / 1000000LL);
  Clock::clockStepped(Clock::SYNC_NTP, before);
  // Duties (step log, edge event, marker, anchor, resolve, scheduler wake,
  // RTC write-back) drain via Clock::clockDutiesTick() on this same lap.
}

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

  // Register the reply callback BEFORE (re)starting SNTP. Registration is a
  // bare static-pointer store inside lwIP that survives sntp_stop/init
  // cycles; re-registering the same function is harmless.
  sntp_set_time_sync_notification_cb(&onSntpTimeSync);

  // Seed the projection so the FIRST background reply on a warm boot (clock
  // carried across a soft reboot) doesn't reconstruct a garbage "before".
  Clock::refreshProjection();

  // lwIP stores server-name POINTERS (sntp_setservername does not copy), so
  // the primary must live in storage that never moves. gSettings.ntpServer
  // is an Arduino String whose buffer reallocates on assignment — hand lwIP
  // a stable copy instead. Refreshed on every setupNTP(), which every
  // ntpserver-change path already calls.
  static char sNtpServer[64];
  strlcpy(sNtpServer, gSettings.ntpServer.c_str(), sizeof(sNtpServer));

  // NOTE: only server slot 0 is real until CONFIG_LWIP_SNTP_MAX_SERVERS>1
  // lands; the two extras below are silently ignored by lwIP when the slot
  // count is 1. Kept in the call so raising the config makes them live.
  configTime(gmtOffset, 0,
             sNtpServer,             // Primary (usually pool.ntp.org)
             "time.google.com",      // Backup (needs MAX_SERVERS >= 2)
             "time.cloudflare.com"); // Backup (needs MAX_SERVERS >= 3)

  // configTime()'s trailing setTimeZone() rewrites TZ from its numeric
  // offset (a rule-less "UTC<h>DST<h>" string). Re-assert the canonical
  // settings-derived TZ so this call can never clobber it — this is also
  // the hook that keeps a future DST rule alive across NTP restarts.
  Clock::applyTimezone();

  DEBUG_NTP_SETUPF("[NTP Setup] configTime() done; primary=%s (backups live once MAX_SERVERS>1)",
                   sNtpServer);
}

bool syncNTPAndResolve(NtpSyncOutcome* outcomeOut) {
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] Starting NTP sync process");
  if (outcomeOut) *outcomeOut = NtpSyncOutcome::Failed;

  if (!WiFi.isConnected()) {
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] FAILED - WiFi not connected");
    broadcastOutput("NTP sync requires WiFi connection");
    return false;
  }

  DEBUG_NTP_SYNCF("[syncNTPAndResolve] WiFi connected, proceeding with NTP sync");
  // No DNS pre-flight: lwIP's SNTP client resolves names itself, and the
  // wait loop below detects failure. (A synchronous hostByName probe here
  // used to add 5-8 s of dead wait on healthy networks.)

  // Capture the pre-attempt state BEFORE setupNTP restarts the daemon: the
  // success test below is "did the reply counter advance", and a reply can
  // land between sntp_init and our first poll.
  const time_t preSyncTime = time(nullptr);
  const bool preSet = Clock::isValidEpoch(preSyncTime);
  const uint32_t startCount = gSntpSyncCount;

  broadcastOutput("Synchronizing time with NTP server...");
  setupNTP();
  broadcastOutput("  Contacting NTP server, please wait...");

  // Success = an actual SNTP reply (the notification callback fired), NOT
  // "the clock looks set" — getLocalTime() is a clock-is-set test, and with
  // RTC/ring/warm-reboot sources able to pre-set the clock it reported
  // "NTP synchronized" without a single packet coming back.
  //
  // Wait budget: a dark boot has nothing better to do than wait the full
  // window; a boot whose clock is already valid shouldn't stall the caller —
  // the background daemon keeps retrying either way.
  bool ntpSynced = false;
  const int maxWaitSeconds = preSet ? 3 : 15;
  const int iterationsPerSecond = 10;  // 100ms per iteration
  const int maxIterations = maxWaitSeconds * iterationsPerSecond;
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] Starting %d-second wait loop for NTP reply", maxWaitSeconds);

  for (int i = 0; i < maxIterations; i++) {
    delay(100);
    oledUpdate();  // Keep boot animation running during NTP wait

    if (i > 0 && i % iterationsPerSecond == 0) {
      char progressMsg[64];
      snprintf(progressMsg, sizeof(progressMsg), "  Looking for updates... %d/%d seconds", i / iterationsPerSecond, maxWaitSeconds);
      broadcastOutput(progressMsg);
    }

    if (gSntpSyncCount != startCount) {
      DEBUG_NTP_SYNCF("[syncNTPAndResolve] SNTP reply received (count %lu)",
                      (unsigned long)gSntpSyncCount);
      ntpSynced = true;
      break;
    }
  }

  if (ntpSynced) {
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] NTP sync completed successfully");
    broadcastOutput("[OK] NTP time synchronized");
    // Post-step duties (edge event, marker, anchor, resolve, scheduler wake,
    // RTC write-back, step diagnostic) all arrive via ntpSyncDrainTick →
    // Clock::clockStepped on the next main-loop lap — do NOT duplicate here.
    //
    // One deliberate exception, the warm-boot baseline: when the clock was
    // ALREADY valid (carried across a soft reboot), the helper sees no
    // invalid→valid edge and a small correction pends nothing — but THIS
    // boot still needs its anchor (gNTPAnchorId bumps every boot, and
    // same-boot users can only resolve against a same-boot anchor). The
    // anchor write is an upsert, so this is idempotent with the drain.
    if (preSet) {
      writeBootAnchor();
      resolvePendingUserCreationTimes();
      logTimeSyncedMarkerIfReady("ntp");
    }
    if (outcomeOut) *outcomeOut = NtpSyncOutcome::Reply;
    return true;
  }

  DEBUG_NTP_SYNCF("[syncNTPAndResolve] no SNTP reply after %d seconds", maxWaitSeconds);
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] Check: WiFi=%s, DNS=%s, Gateway=%s",
                  WiFi.isConnected() ? "OK" : "FAIL",
                  WiFi.dnsIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str());

  // A reply can land in the milliseconds between the loop's final check and
  // here; without this re-check the dark path below would immediately
  // overwrite the just-corrected clock with drifted RTC time.
  if (gSntpSyncCount != startCount) {
    broadcastOutput("[OK] NTP time synchronized");
    if (preSet) {
      writeBootAnchor();
      resolvePendingUserCreationTimes();
      logTimeSyncedMarkerIfReady("ntp");
    }
    if (outcomeOut) *outcomeOut = NtpSyncOutcome::Reply;
    return true;
  }

  if (preSet) {
    // Honest version of what used to print "[OK] NTP time synchronized
    // successfully" on iteration 0: no reply yet, but the clock is valid
    // from an earlier source, so nothing is wrong — the daemon keeps
    // retrying in the background and the drain reports when it lands.
    broadcastOutput("[OK] Clock keeps its current time; NTP still trying in background");
    writeBootAnchor();
    resolvePendingUserCreationTimes();
    // Marker names WHO supplied this boot's clock: the ledger when a real
    // source stepped it (rtc/ring/manual), "carryover" only for the true
    // soft-reboot case where no settimeofday ever happened.
    logTimeSyncedMarkerIfReady(Clock::syncSource() != Clock::SYNC_NONE
                                   ? Clock::syncSourceName(Clock::syncSource())
                                   : "carryover");
    if (outcomeOut) *outcomeOut = NtpSyncOutcome::KeptPrior;
    return true;
  }

  // Dark timeout — try the RTC as a fallback source. rtcSyncToSystem() runs
  // its duties through Clock::clockStepped itself now.
#if ENABLE_RTC_SENSOR
  if (gRtcRunning && gRtcConnected) {
    if (rtcSyncToSystem()) {
      broadcastOutput("[OK] System time set from RTC (NTP unavailable)");
      if (outcomeOut) *outcomeOut = NtpSyncOutcome::RtcFallback;
      return true;
    }
  }
#endif

  broadcastOutput("[ERROR] NTP sync timeout - no other time source");
  broadcastOutput("  Note: Your router may be blocking NTP (UDP port 123)");
  return false;
}

#endif // ENABLE_WIFI


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
// Boot/reset counters. Boot count is persisted in NVS (its own flash
// partition), separate from settings.json / users.json so its every-boot bump
// can never corrupt those files. Crash count + last reset reason are RTC-backed
// RAM mirrors (System_BootState.h explains the split).
const char* cmd_bootcount(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String a = argsInput;
  a.trim();

  if (a.equalsIgnoreCase("reset")) {
    if (!currentExecIsAdmin()) return "Error: admin required to reset the boot counter";
    bootStateResetBootCount();
    logSystemEvent("BOOT", "boot counter reset to 0 by %s", currentExecUser().c_str());
    return "OK: boot counter reset to 0";
  }

  uint32_t bootCount = bootStateGetBootCount();
  uint32_t reason    = gSettings.lastResetReason;
  const char* reasonLabel = resetReasonLabel(reason);

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"]            = 1;
    doc["boot_count"]        = (unsigned long)bootCount;
    doc["boot_count_store"]  = "nvs";
    doc["crash_count"]       = (unsigned long)gSettings.crashCount;
    doc["reset_reason"]      = reasonLabel;
    doc["reset_reason_code"] = (unsigned long)reason;
    static char* buf = nullptr;
    if (!buf) buf = (char*)ps_alloc(256, AllocPref::PreferPSRAM, "bootcount.json");
    if (!buf) return "{\"error\":\"oom\"}";
    serializeJson(doc, buf, 256);
    return buf;
  }

  BROADCAST_PRINTF("Boot count:   %lu  (NVS — survives power loss & FS erase)", (unsigned long)bootCount);
  BROADCAST_PRINTF("Crash count:  %lu  (abnormal resets since last clean power-on)", (unsigned long)gSettings.crashCount);
  BROADCAST_PRINTF("Last reset:   %s (%lu)", reasonLabel, (unsigned long)reason);
  return "OK";
}

// Read surface for the RTC crash record. Without this the capture would be
// write-only: a field crash is diagnosed by someone with no serial cable, and
// registering here gets CLI + web + OLED + G2 + ESP-NOW for free through
// executeCommand(), with authorizeCommand() gating already applied.
const char* cmd_crashlog(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"]      = 1;
    doc["have_record"] = crashRecordHavePrevious();
    doc["consecutive"] = (unsigned long)crashRecordConsecutive();
    doc["signature"]   = (unsigned long)crashRecordSignature();
    doc["repeat"]      = (unsigned long)crashRecordRepeatCount();
    doc["boot_phase"]  = crashPhaseName(crashRecordPrevPhase());
    doc["detail"]      = crashRecordReasonText();
    static char* buf = nullptr;
    if (!buf) buf = (char*)ps_alloc(512, AllocPref::PreferPSRAM, "crashlog.json");
    if (!buf) return "{\"error\":\"oom\"}";
    serializeJson(doc, buf, 512);
    return buf;
  }

  // Static + PSRAM rather than a stack buffer: command handlers run on
  // cmd_exec_task and stack figures in this codebase are BYTES, so a 512 B frame
  // is not free.
  static char* text = nullptr;
  if (!text) text = (char*)ps_alloc(640, AllocPref::PreferPSRAM, "crashlog.text");
  if (!text) return "Error: out of memory";

  BROADCAST_PRINTF("Last crash");
  BROADCAST_PRINTF("  reset reason : %s (%lu)",
                   resetReasonLabel(gSettings.lastResetReason),
                   (unsigned long)gSettings.lastResetReason);
  crashRecordDetail(text, 640);

  // Emit line-by-line: broadcastOutput clamps at 255 B, so a multi-line blob
  // would be silently cut.
  char* line = text;
  while (line && *line) {
    char* nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    BROADCAST_PRINTF("%s", line);
    if (!nl) break;
    line = nl + 1;
  }
  if (VFS::exists("/system/sys_logs/crash-history.log")) {
    cliHint("full history (survives reboots): /system/sys_logs/crash-history.log");
  }
  return "OK";
}

const CommandEntry commands[] = {
  // ---- Core / General ----
  { "status", "Show system status (WiFi, FS, memory). (add 'json' for JSON output)", false, cmd_status, nullptr, "system", "status" },
  { "crashlog", "Show the last recorded crash (panic text, core/PC, boot phase, repeat count). (add 'json')", false, cmd_crashlog,
    "Usage: crashlog [json]" },
  { "bootcount", "Show boot count (NVS), crash count, last reset reason. 'bootcount reset' zeroes it (admin). (add 'json')", false, cmd_bootcount,
    "Usage: bootcount [reset|json]" },
  { "uptime", "Show device uptime. (add 'json' for JSON output)", false, cmd_uptime },
  { "time", "Show device time (uptime + NTP if synced). (add 'json' for JSON output)", false, cmd_time },
  { "timeset", "Set time manually: timeset YYYY-MM-DD HH:MM:SS or <unix_timestamp>.", true, cmd_timeset,
    "Usage: timeset <YYYY-MM-DD HH:MM:SS>|<unix_timestamp>" },
  { "memsample", "Memory snapshot with component requirements. Use 'memsample track [on|off|reset|status]' for allocation tracking.", false, cmd_memsample,
    "Usage: memsample [track <on|off|reset|status>]" },
  { "memreport", "Comprehensive memory report (Task Manager style). (add 'json' for JSON output)", false, cmd_memreport },
  { "fsusage", "Show filesystem usage. (add 'json' for JSON output)", false, cmd_fsusage },
  
  // ---- Testing Commands (Admin Only) ----
  { "testencryption", "Test WiFi password encryption (admin only).", true, cmd_testencryption },
  { "testpassword", "Test user password hashing (admin only).", true, cmd_testpassword },

  // ---- System Diagnostics ----
  { "temperature", "Read ESP32 internal temperature. (add 'json' for JSON output)", false, cmd_temperature },
  { "voltage", "Estimate power draw from active subsystems (not a real voltage measurement; use batterystatus for measured volts). (add 'json' for JSON output)", false, cmd_voltage },
  { "cpufreq", "Get/set CPU frequency (admin).", true, cmd_cpufreq,
    "Usage: cpufreq [80|160|240]" },
  { "taskstats", "Detailed task statistics (state/prio/stack min-free). (add 'json' for JSON output)", false, cmd_taskstats },
  { "perftop", "Live performance snapshot: loop laps/s, period, per-section timing, worst stalls + live task CPU%. (add 'json' for loop + per-task CPU% JSON)", false, cmd_perftop },
  { "events", "Show recent system events (the in-memory register that drives automation event triggers).", false, cmd_events,
    "Usage: events [kinds [json]]\n  (bare): show the recent-event ring\n  kinds: list every valid event-kind name (json = machine form)" },

  // ---- Misc ----
  { "reboot", "Reboot the system.", true, cmd_reboot, nullptr, "system", "reboot" },
  { "ramflush", "Reboot to reclaim RAM, restoring the features running right now.", true, cmd_ramflush,
    "Usage: ramflush [status]\n"
    "  (bare):  capture which features are running, reboot, and restore them\n"
    "  status:  show what the last boot restored (no reboot)\n"
    "\n"
    "Restores for that one boot only — a normal reboot returns to configured\n"
    "autostart. Never changes your autostart settings.",
    "system", "ramflush" },
  { "factoryreset", "Wipe user accounts and reboot to re-run setup wizard.", true,
    cmd_factoryreset,
    "Usage: factoryreset (no args, confirmation required)\n"
    "Deletes /system/users/users.json so the first-time setup wizard runs\n"
    "on next boot. WiFi credentials and other settings are preserved.",
    nullptr, nullptr, /*requiresSuperAdmin=*/true },
  { "broadcast", "Send a message to all connected output interfaces.", true, cmd_broadcast,
    "Usage: broadcast <message>" },
  { "pendinglist", "List pending user requests.", true, cmd_pending_list },
  { "wait", "Delay execution for N milliseconds: wait <ms>.", false, cmd_wait,
    "Usage: wait <ms>  (1..60000)" },
  { "sleep", "Alias for wait: sleep <ms>.", false, cmd_wait,
    "Usage: sleep <ms>  (1..60000)" },
  { "lightsleep", "Enter ESP32 light sleep: lightsleep [seconds] (default 20s).", true, cmd_lightsleep,
    "Usage: lightsleep [seconds]  (1..3600, default 20)" },
  { "deepsleep", "Power off via deep sleep (no wake source — reset button to wake).", true, cmd_deepsleep,
    "Usage: deepsleep  (admin; wakes only via physical reset)" },
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
  {"batterycalibrate", "Recalibrate/re-probe the battery sensor (ADC characterize or fuel-gauge re-probe)", true, cmd_battery_calibrate},
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
  { "cli",        "Help and CLI navigation", "The cli module is the on-device help and CLI navigation layer, not a feature "
    "subsystem. help opens a paged help browser: bare help shows the main menu listing "
    "every registered module, help <module> drills into one module command page (and "
    "prints that module subsystem overview at the top), and the special topics help "
    "sensors (aggregate view across all sensor modules), help all (show every command "
    "including hidden ones), and help tail (dump suppressed output) cover the rest. "
    "While the browser is open the CLI is in a help state, so back steps from a module "
    "page up to the main menu, exit leaves help mode entirely and returns to the normal "
    "prompt, and clear wipes the CLI scrollback/history.", cliCommands,          cliCommandsCount, CMD_MODULE_CORE, nullptr },
  { "system",     "Core system commands", "The system module holds core device commands that do not belong to any peripheral. "
    "Status and inspection: status (WiFi, filesystem, memory summary), uptime, time "
    "(uptime plus NTP wall-clock if synced), temperature and voltage (ESP32 internal "
    "die temp and supply rail), taskstats/perftop (FreeRTOS task and live loop/CPU "
    "profiling), fsusage, events (recent system events from the in-memory register "
    "that drives automation event triggers), and the memory tools memsample (snapshot, with memsample "
    "track on|off|reset|status for allocation tracking) and memreport. Control and "
    "power: reboot, ramflush, cpufreq [80|160|240] to read or set CPU clock, lightsleep "
    "[seconds] for ESP32 light sleep, deepsleep for power-off (reset to wake), and "
    "wait <ms>/sleep <ms> to pause command-script execution. "
    "timeset sets the clock manually. broadcast <message> pushes a line of text to all "
    "connected output interfaces, and factoryreset deletes the user-accounts file so "
    "the first-boot setup wizard re-runs on next reboot while deliberately preserving "
    "WiFi credentials and other settings. Most mutating commands (timeset, cpufreq, "
    "reboot, ramflush, factoryreset, broadcast, lightsleep, deepsleep) require admin.", commands,             commandsCount, 0, nullptr },
#if ENABLE_WIFI
  { "wifi",       "Network management (connect, scan, add/remove networks)", "The WiFi subsystem manages station-mode network connections plus the network "
    "services that ride on top of them: NTP time sync and the on-device HTTP/HTTPS "
    "server. Saved networks are stored as a prioritized list (wifilist, wifiadd, "
    "wifirm, wifipromote) and persist to flash; openwifi connects by best-priority "
    "(default) or by --index <N>, and a failed indexed attempt auto-rolls back to the "
    "previously connected network. Note two distinct disconnects: closewifi tears down "
    "the link AND stops the HTTP server and web output to free heap, while "
    "wifidisconnect (drop) leaves the radio and web server up so you can move to "
    "another network. wifiscan lists nearby APs, ntpsync/ntpstatus handle clock sync, "
    "and openhttp/closehttp/httpstatus run the web server (compiled in only when the "
    "HTTP server is enabled). certinfo and certgen (admin-only) manage the self-signed "
    "HTTPS certificate.", wifiCommands,         wifiCommandsCount, CMD_MODULE_NETWORK, nullptr },
#endif
#if ENABLE_ESPNOW
  { "espnow",     "ESP-NOW wireless communication (peer-to-peer, mesh)",
    "ESP-NOW links HardwareOne devices directly over the WiFi radio with no router or "
    "access point, as named peers that can also form a multi-hop mesh. Pair with "
    "espnowpair, then message (espnowsend/espnowbroadcast), push a file (espnowsendfile), "
    "pull a file (espnowfetch), browse a peer's files (espnowbrowse), or run a command on "
    "a peer (espnowremote). espnowremote, espnowfetch, espnowbrowse, espnowroomcmd and "
    "espnowtagcmd are ASYNCHRONOUS: they return OK on delivery; the real result arrives "
    "later in the message buffer, read with 'espnowmessages json [mac]'. Mesh mode "
    "(espnowmode mesh) adds routing with a TTL and master/worker/backup roles; each "
    "device carries identity metadata (name, friendly name, room, zone, tags) queried "
    "with espnowdeviceinfo locally or espnowrequestmeta for a peer.",
    espNowCommands,       espNowCommandsCount, CMD_MODULE_NETWORK, nullptr },
#endif
#if ENABLE_MQTT
  { "mqtt",       "MQTT broker connection for Home Assistant", "The MQTT subsystem connects the device to a broker, primarily to publish its "
    "sensor and system telemetry to Home Assistant via HA discovery. It is almost "
    "entirely configuration: broker host/port (mqttHost, mqttPort), credentials "
    "(mqttUser, mqttPassword), TLS mode and CA path, base/discovery topics, publish "
    "interval, and a long list of per-source publish toggles (mqttPublishThermal, "
    "mqttPublishIMU, and so on). These are persisted settings and most config commands "
    "are admin-only; after changing them, reconnect with closemqtt/openmqtt to apply to "
    "a live session. openmqtt and closemqtt start and stop the client, mqttstatus shows "
    "connection state, and mqttautostart controls whether it connects at boot. For "
    "inbound data, enable mqttSubscribeExternal with mqttSubscribeTopics; values "
    "received from those topics are cached and read back with mqttExternalSensors.", mqttCommands,         mqttCommandsCount, CMD_MODULE_NETWORK, nullptr },
#endif
  #if ENABLE_BLUETOOTH
  { "bluetooth",  "Bluetooth LE control and status", "The Bluetooth subsystem runs the device BLE stack in one of two mutually exclusive "
    "roles selected by blemode: server mode (the device advertises and a phone/app "
    "connects to it) or client mode (the device acts as a BLE central for Even G2 "
    "glasses; the even_g2 commands then apply). Switching modes tears down the other "
    "role automatically. In server mode, openble/closeble start and stop advertising, "
    "blesend pushes a one-off message and bleevent an event to the connected client, "
    "and blestream toggles periodic pushes as a bitmask of sensors/system/events "
    "(blestream on/off/sensors/system/events, plus interval) -- all of which require an "
    "active connection. An app-layer Secure Channel (X25519 + passphrase + "
    "ChaCha20-Poly1305, independent of BLE bonding) is configured with blesecret and "
    "required with blesecure; both are admin-only, as is blerequireauth. Boot "
    "reconnection to saved-MAC peers is per-peer via bleautoreconnect <name> [on|off] "
    "(see blepeers for names).", bluetoothCommands, bluetoothCommandsCount, CMD_MODULE_NETWORK, nullptr },
  #endif
  { "filesystem", "File operations and storage management", "Manages files and directories on the device internal LittleFS flash. Browse with "
    "files [\"/path\"] (add json for app/BLE, or files stats json for storage usage); "
    "create and remove with mkdir, rmdir, filecreate, and filedelete; view and rename "
    "with fileview and filerename. Critically, every path argument MUST be wrapped in "
    "double quotes, e.g. fileview \"/system/notes\" -- an unquoted or unmatched-quote "
    "path is rejected, and a leading slash is added automatically. For programmatic "
    "transfer, fileread and filewrite move data in chunks: fileread returns "
    "{success,size,offset,len,eof,enc,data} and you loop offset until eof, while "
    "filewrite is strictly sequential -- offset 0 truncates/creates the file, each "
    "later offset must equal the current file size, and passing final runs the "
    "post-save hooks. Access is permission-gated per path: system trees like /system "
    "are read-only (or browse-only) for admins, while user data is fully writable; "
    "logtier reports whether logs are writing to LittleFS or have spilled into SD "
    "overflow.", filesystemCommands,   filesystemCommandsCount, 0, nullptr },
#if defined(SD_CS_PIN)
  { "sd",         "SD card mount, format, and info", "Controls the optional microSD card, which mounts at /sd and serves as "
    "overflow/bulk storage (and is only compiled in on boards that wire a "
    "card-detect/CS pin). sdmount attempts to mount the card and sdunmount safely "
    "unmounts it; sdinfo shows the card type, size, and used/free space, and sddiag "
    "runs a raw-SPI hardware diagnostic to troubleshoot a card that will not mount. "
    "sdformat erases the entire card and reformats it as FAT32 and therefore requires "
    "sdformat confirm to proceed. Once mounted, file commands address the card through "
    "its /sd/... path prefix.", sdCommands,           sdCommandsCount, 0, []() { return VFS::isSDAvailable(); } },
#endif
  { "oled",       "OLED display control and graphics", "Drives the small SSD1306 OLED display: its lifecycle, the live screen contents, "
    "and persistent appearance settings. oledstart/oledstop (aliases "
    "openoled/closeoled) power the display task on and off, and oledstatus (alias "
    "oledread) reports its state. oledmode <mode> switches the live screen among the "
    "built-in views (menu, status, sensordata, thermal, network, mesh, gps, espnow, "
    "memory, off, and more); oledtext <message> shows custom text and oledanim "
    "<name>|fps <n> picks the animation -- both require the display to be running (run "
    "oledstart first) and neither persists across reboot. Separately, the oled* config "
    "commands write settings to flash immediately: oledbootmode and oleddefaultmode set "
    "the screen shown at boot and as the idle default, while oledbrightness <0-255>, "
    "oledflip, oledbootduration, oledupdateinterval, oledthermalscale, "
    "oledthermalcolormode, and oledenabled tune appearance and timing. oledrequireauth "
    "<0|1> (admin-only) controls whether a user must log in at the display before "
    "interacting with it.", oledCommands,         oledCommandsCount, 0, nullptr },
  { "neopixel",   "RGB LED strip and effects", "Controls the addressable RGB status LED (WS2812/NeoPixel). ledcolor <name> lights "
    "it a solid color from a fixed palette (red, green, blue, yellow, magenta, cyan, "
    "white, orange, purple, pink), and ledclear turns it off. ledeffect "
    "<fade|blink|pulse|strobe> [color] [color2] [duration 100-60000ms] runs an animated "
    "effect (defaults: red/blue, 3000 ms; ledeffect off clears it). These commands "
    "change the LED immediately and are not saved -- the persistent power-on brightness "
    "and startup animation live in the led settings module, not here. Note the effect "
    "call runs synchronously for its full duration before returning.", neopixelCommands,     neopixelCommandsCount, 0, nullptr },
  { "led",        "LED brightness and startup effects", "Configures the board onboard single LED -- its brightness and the one-shot effect "
    "played at startup. These are persistent settings written to flash, not live "
    "controls: ledbrightness <0-100> sets the global brightness, ledstartupenabled "
    "<0|1> toggles the boot effect, and ledstartupeffect "
    "<none|rainbow|pulse|fade|blink|strobe> with ledstartupcolor, ledstartupcolor2, and "
    "ledstartupduration <100-10000ms> define what plays on power-up. (The live, "
    "immediate RGB controls are the separate ledcolor/ledeffect commands in the "
    "neopixel module.)", ledCommands,           ledCommandsCount, 0, nullptr },
#if ENABLE_SERVO
  { "servo",      "PCA9685 servo motor control", "The PCA9685 is a 16-channel I2C PWM driver used to control hobby servos (and "
    "generic PWM outputs) without tying up the ESP32 own timers. servo <channel> "
    "<angle> moves the servo on a channel to an angle, while pwm <channel> <value> "
    "[freq] writes a raw PWM duty (and optional frequency) for non-servo loads like "
    "LEDs or motor drivers. Because different servos expect different pulse ranges, "
    "servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name> stores a per-channel "
    "calibration that maps angles to the correct pulse widths (servolist shows the "
    "saved profiles), and servocalibrate <channel> opens an interactive mode to find "
    "those pulse limits by hand.", servoCommands,        servoCommandsCount, 0, nullptr },
#endif
#if ENABLE_THERMAL_SENSOR
  { "thermal",    "MLX90640 thermal camera (32x24)", "The MLX90640 is a 32x24 (768-pixel) infrared thermal camera. openthermal starts "
    "it, thermalread reports the current frame min/max/avg temperature in Celsius, and "
    "closethermal stops it; thermalautostart [on|off] persists launching it at boot, "
    "and it runs on the fixed secondary I2C bus (Wire1). The sensor runs in "
    "chess-pattern mode at 16-bit ADC resolution, with thermaltargetfps <1..8> "
    "selecting the device refresh rate. Display tuning is extensive: "
    "thermalpalettedefault picks the color map (grayscale, iron, rainbow, hot, or "
    "coolwarm), thermalrotation <0..3> rotates the image 0/90/180/270 degrees, and "
    "thermalupscalefactor plus the thermalinterpolation* commands smooth and enlarge "
    "the 32x24 grid for the web/OLED view. Frame readings are stabilized by "
    "temporal/EWMA smoothing, per-pixel outlier rejection, and an optional rolling "
    "min/max auto-scale that keeps the color scale from flickering; thermaldiag prints "
    "a hardware self-check.", thermalCommands,      thermalCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("thermal"); } },
#endif
#if ENABLE_TOF_SENSOR
  { "tof",        "VL53L4CX time-of-flight distance sensor", "The VL53L4CX is a laser time-of-flight ranging sensor that measures distance to "
    "nearby objects. opentof starts it, tofread reports the closest valid distance in "
    "centimeters (or full object data as JSON), and closetof stops it; tofautostart "
    "[on|off] persists launching it at boot, and it runs on the fixed secondary I2C bus "
    "(Wire1). It is configured for LONG distance mode with a 200 ms timing budget, and "
    "is multi-target: each measurement can return up to four objects, which are "
    "signal-rate-gated and exponentially smoothed before the nearest valid one is "
    "reported. Most tunables are client-side visualization knobs rather than sensor "
    "settings: tofpollingms, toftransitionms, and tofmaxdistancemm shape the UI, "
    "tofstabilitythreshold sets how steady a reading must be, and tofdevicepollms "
    "controls how often the firmware reads the hardware.", tofCommands,          tofCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("tof"); } },
#endif
#if ENABLE_IMU_SENSOR
  { "imu",        "BNO055 9-DOF orientation sensor", "The BNO055 is a 9-DOF inertial measurement unit providing fused absolute "
    "orientation. openimu starts it, imuread reports yaw/pitch/roll (degrees) plus "
    "acceleration, gyroscope, and chip temperature, and closeimu stops it; imuautostart "
    "[on|off] persists launching it at boot, and it runs on the fixed secondary I2C bus "
    "(Wire1) using the board external crystal. Beyond raw orientation, imuactions runs "
    "gesture/event detection derived from the motion data: shake, tilt (with "
    "direction), tap/knock, rotation (with axis), freefall, a step counter with "
    "cadence, and screen-style orientation. Because the chip can be mounted in any "
    "pose, imuorientationmode <0..8> applies a fixed remap (flip pitch/roll/yaw, "
    "90-degree rotations, upside-down fixes), imuorientationcorrection <0|1> toggles "
    "that correction, and imupitchoffset/imurolloffset/imuyawoffset trim each axis in "
    "degrees.", imuCommands,          imuCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("imu"); } },
#endif
#if ENABLE_OLED_INPUT
  { "input",      "Input device (gamepad or ANO encoder)", "Device-agnostic abstraction for the OLED input controller, which is either the "
    "Seesaw gamepad or the ANO rotary encoder -- chosen at compile time via "
    "INPUT_DEVICE_TYPE and mutually exclusive, so exactly one driver is present per "
    "firmware. These commands operate on whichever driver was built in: openinput "
    "starts it, closeinput stops it, inputautostart [on|off] persists boot auto-start, "
    "and inputdevicepollms <10-1000> sets the polling interval in milliseconds (default "
    "90). Driver-specific debugging and tuning live in the gamepad and anoencoder "
    "modules; this module holds only the shared settings (poll interval and "
    "auto-start).", inputCommands,       inputCommandsCount,       CMD_MODULE_SENSOR, []() { return gInputConnected; } },
#endif
#if ENABLE_GAMEPAD_SENSOR
  { "gamepad",    "Seesaw gamepad — raw debug commands", "Adafruit Seesaw I2C gamepad (analog joystick plus buttons), exposed here as a "
    "low-level debug interface for the raw device. The driver-agnostic "
    "open/close/autostart/poll commands live under the input module; the only "
    "gamepad-specific command is gamepadread, which polls the Seesaw once and dumps raw "
    "state -- joystick X/Y and the button bitmask -- attempting an on-demand connect "
    "with backoff if the device is not yet initialized. A background task polls the "
    "gamepad at roughly 50 ms and caches the latest reading for the OLED UI and sensor "
    "JSON. This module is mutually exclusive at build time with anoencoder; only one "
    "input device is compiled in per firmware (see input).", gamepadCommands,      gamepadCommandsCount, CMD_MODULE_SENSOR, []() { return gInputConnected; } },
#endif
#if ENABLE_ANO_ENCODER
  { "anoencoder", "ANO rotary encoder — debug + driver-specific config", "Adafruit ANO directional navigation rotary encoder on Seesaw I2C: a click wheel "
    "with a center IN press and UP/DOWN/LEFT/RIGHT buttons, used as the OLED navigation "
    "input. This module provides debug and remap commands; the actual "
    "open/close/autostart/poll lifecycle lives under the input module. anoencoderread "
    "dumps raw state -- encoder position, the currently selected rotary axis, and the "
    "button bitmask. Remap commands persist to settings: anoencoderi2caddr <1-127> "
    "changes the device address (reboot required), anoencoderinvert [on|off] reverses "
    "rotation direction, and anoencoderswapud / anoencoderswaplr [on|off|toggle] swap "
    "the UP/DOWN and LEFT/RIGHT button pairs. A polling task accumulates encoder "
    "detents so fast spins do not drop clicks. Mutually exclusive at build time with "
    "the Seesaw gamepad.", anoEncoderCommands, anoEncoderCommandsCount, CMD_MODULE_SENSOR, []() { return gAnoEncoderConnected; } },
#endif
#if ENABLE_APDS_SENSOR
  { "apds",       "APDS9960 color, proximity, gesture sensor", "The APDS9960 is a combined RGB color, proximity, and gesture sensor. openapds "
    "starts it (color sensing enabled by default), apdsread shows which modes are "
    "active plus the latest RGBC and proximity values, and closeapds stops it; "
    "apdsautostart [on|off] persists launching it at boot, and it runs on the fixed "
    "secondary I2C bus (Wire1). Its three functions are toggled independently at "
    "runtime with apdsmode <color|proximity|gesture> [on|off] -- note that enabling "
    "gesture also turns proximity on, since the gesture engine needs it. Dedicated "
    "reads apdscolor, apdsproximity, and apdsgesture print a single sample on demand "
    "(gesture returns UP/DOWN/LEFT/RIGHT), and apdsdevicepollms sets how often the "
    "background task samples the hardware.", apdsCommands,         apdsCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("apds"); } },
#endif
#if ENABLE_GPS_SENSOR
  { "gps",        "PA1010D GPS module", "PA1010D I2C GPS receiver. Lifecycle: opengps starts the parser task, gpsread "
    "prints the current fix, and closegps stops it; gpsautostart [on|off] persists boot "
    "auto-start, and the module appears in help only when the chip is detected. gpsread "
    "reports fix yes/no, fix quality, satellite count, and (only when a fix is held) "
    "latitude/longitude in degrees, altitude in meters, speed in knots, heading angle, "
    "plus GPS UTC time and date; with no fix it shows just quality and satellites. The "
    "distinctive gpslog [interval_ms] command is a one-shot setup that turns on "
    "gpsAutoStart, configures sensorlog to format=track with sensors=gps, then "
    "immediately starts both the GPS sensor and the logger to record a track (default "
    "1000 ms, minimum 100 ms) that persists across reboots.", gpsCommands,          gpsCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("gps"); } },
#endif
#if ENABLE_FM_RADIO
  { "fmradio",    "RDA5807 FM radio receiver", "RDA5807M I2C FM radio receiver. Lifecycle: openfmradio starts it, fmradioread "
    "reports status, and closefmradio stops it; fmradioautostart [on|off] persists boot "
    "auto-start and the module shows in help only when detected. fmradiotune <freq> "
    "accepts either MHz (e.g. 103.9) or 10 kHz integer units (e.g. 10390) -- values "
    "under 200 are read as MHz, otherwise as raw units -- and rejects anything outside "
    "76.0-108.0 MHz; tuning clears any decoded RDS station name and text. fmradioseek "
    "[up|down] STARTS a hunt for the next station (no band wrap) and returns "
    "immediately -- the radio task finalizes the result within a few seconds; watch "
    "'fmradioread' (JSON field \"seeking\") or the OLED FM screen. fmradiovolume <0-15> "
    "sets output level, and fmradiomute / fmradiounmute toggle audio. The OLED FM "
    "screen drives all of this from the gamepad: L/R tune, Up/Down seek, A mute, "
    "Y volume, X power.", fmRadioCommands,      fmRadioCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("fmradio"); } },
#endif
#if ENABLE_RTC_SENSOR
  { "rtc",        "DS3231 precision RTC", "DS3231 precision I2C real-time clock with battery backup and an on-chip "
    "temperature sensor. Lifecycle: openrtc starts the RTC task, rtcread [status|temp] "
    "reads the clock (or die temperature), and closertc stops it; rtcautostart [on|off] "
    "persists boot auto-start and the module appears in help only when detected. rtcset "
    "accepts either \"YYYY-MM-DD HH:MM:SS\" or a bare Unix timestamp and writes it to "
    "the chip, computing day-of-week automatically. rtcsync [to|from] moves time "
    "between the RTC and the system clock: to (the default) copies RTC -> system, from "
    "copies system -> RTC (use this after an NTP sync to persist accurate time into the "
    "battery-backed chip). Setting the time via rtcset or rtcsync from also marks the "
    "RTC as calibrated so later boots trust it as a time source.", rtcCommands,          rtcCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("rtc"); } },
#endif
#if ENABLE_PRESENCE_SENSOR
  { "presence",   "STHS34PF80 IR presence/motion sensor", "The STHS34PF80 is an infrared presence and motion sensor that detects warm bodies "
    "without contact. openpresence starts it, presenceread reports ambient temperature "
    "plus presence, motion, and temperature-shock values (each with a DETECTED flag), "
    "and closepresence stops it; presenceautostart [on|off] persists launching it at "
    "boot. The sensor is initialized at an 8 Hz output data rate with block-data-update "
    "enabled, and its on-chip presence/motion/ambient-shock detection engines provide "
    "the detection flags directly; presencestatus prints connection and data-validity "
    "diagnostics, and presencedevicepollms controls the hardware read interval.", presenceCommands,     presenceCommandsCount, CMD_MODULE_SENSOR, []() { return isSensorConnected("presence"); } },
#endif
#if ENABLE_CAMERA_SENSOR
  { "camera",     "ESP32-S3 DVP camera sensor", "Driver and CLI for the attached DVP camera sensor (OV2640/OV3660 class). The "
    "sensor must be powered up first with opencamera before any capture or tuning "
    "command works (closecamera stops it); cameraread and cameradump report status and "
    "all current sensor register values. Three distinct capture paths exist: "
    "cameracapture grabs one JPEG frame into RAM and reports its size only, camerasave "
    "captures and writes a frame to storage (LittleFS, SD, or both, per "
    "camerastoragelocation, into cameracapturefolder), and cameratiny produces a "
    "160x120 frame small enough for a single ESP-NOW packet; camerarecord start|stop "
    "records MJPEG-AVI video and requires an SD card. Resolution and image controls "
    "(camerares/cameraframesize, cameraquality, camerafps) and a large set of "
    "sensor-tuning commands (brightness/contrast/saturation, white balance, "
    "exposure/AEC, gain/AGC, special effects, mirror/flip/rotate, plus raw camerareg "
    "register writes) adjust the live image. Automation settings (cameraautostart, "
    "cameraautocapture/cameraautocaptureinterval, "
    "camerasendaftercapture/cameratargetdevice) drive timed capture and optional "
    "ESP-NOW delivery to a named peer.", cameraCommands,       cameraCommandsCount, CMD_MODULE_SENSOR, []() { return cameraConnected; } },
#endif
#if ENABLE_MICROPHONE
  { "microphone", "Microphone (PDM or G2 glasses)", "Driver and CLI for the microphone — the on-board PDM mic and/or the G2 glasses mic, selected with micsource. The mic must be started with "
    "openmic before reads or recording (closemic stops it); commands that need the "
    "running mic return a use-openmic-first error otherwise. miclevel returns the "
    "current audio level (percent; add json for structured output) and micviz shows a "
    "live level meter until a key is pressed. micrecord start|stop records audio to a "
    "WAV file, miclist lists saved recordings, and micdelete removes one or all of "
    "them. Audio format is configured with micsamplerate (8000-48000), micgain (0-100), "
    "and micbitdepth (16 or 32), each usable as a getter with no argument; micautostart "
    "on|off persists whether the mic powers up automatically at boot.", micCommands,          micCommandsCount, CMD_MODULE_SENSOR, []() { return audioAnySourceAvailable(); } },
#endif
#if ENABLE_EDGE_IMPULSE
  { "edgeimpulse", "Edge Impulse ML inference", "On-device machine-learning image inference using TensorFlow Lite Micro models "
    "exported from Edge Impulse. Two things must be in place before inference: a model "
    "must be loaded with eimodelload \"<file>\" (models live in the models directory on "
    "LittleFS; eimodellist/eimodelinfo/eimodelunload manage them) and inference must be "
    "enabled with eienable 1 (which also initializes the inference buffers). eidetect "
    "runs a single detection on a live camera frame and so additionally requires the "
    "camera to be opened (see opencamera), returning detected objects with confidence "
    "and bounding boxes as JSON; eifile \"<path>\" runs the same inference against a "
    "stored JPEG instead of the camera. eicontinuous 1 runs detection repeatedly in the "
    "background, eiconfidence <0.0-1.0> sets the minimum confidence to report, and "
    "eistatus shows current state. The eitrack family (eitrackenable, eitrackstatus, "
    "eitrackclear) adds cross-frame state tracking of detected objects on top of raw "
    "detections.", edgeImpulseCommands,  edgeImpulseCommandsCount, CMD_MODULE_SENSOR, nullptr },
#endif

 #if ENABLE_ESP_SR
   { "espsr", "ESP-SR speech recognition", "Offline voice control built on Espressif ESP-SR: a WakeNet wake-word stage gates a "
    "MultiNet command-phrase recognizer, so the device waits for the wake word and then "
    "listens for a known command phrase. Note that srenable/sr enable only reports the "
    "compile-time build flag and cannot toggle the feature at runtime; the real "
    "lifecycle commands are opensr/srstart to start the recognition pipeline and "
    "closesr/srstop to stop it. Starting the pipeline also arms voice command execution "
    "as the current authenticated user (and stopping it disarms); arming can be managed "
    "directly with voicearm/voicedisarm/voicestatus, and recognized phrases only "
    "execute commands while armed. The command vocabulary is managed with the srcmds "
    "family (list/add/del/clear plus save/reload to an SD file and srcmdssync to import "
    "phrases from the CLI registry). Recognition is tuned through srconfidence, "
    "srtimeout, the srtuning* audio controls (gain, AGC, VAD, filters), and srdebug* "
    "telemetry; the mic feed follows the device-wide source (set with `micsource "
    "auto|pdm|g2` — onboard PDM or the G2 glasses left-temple mic), and the srsnip* commands capture audio "
    "snippets (by default on the wake word) for debugging.", espsrCommands,  espsrCommandsCount, CMD_MODULE_SENSOR, nullptr },
 #endif
#if ENABLE_I2C_SYSTEM
  { "i2c",        "I2C bus diagnostics and scanning", "The i2c module configures and diagnoses up to two I2C buses and the sensor device "
    "registry. There are two buses with a deliberate naming convention: bus 0 is I2C1 "
    "(Arduino Wire1, the primary STEMMA QT / sensor bus) and bus 1 is I2C2 (Wire, the "
    "optional secondary bus); each has its own enable flag and SDA/SCL pin settings, "
    "and bus/pin changes require a reboot. Each sensor can be routed to either bus with "
    "a per-device command (oledBus, gpsBus, rtcBus, imuBus, thermalBus, tofBus, etc.), "
    "all taking 0 or 1 and needing a reboot. Discovery and diagnostics: i2cscan dumps "
    "raw addresses found on each active bus; detect reports configured-vs-present "
    "hardware and detect apply (admin) auto-enables newly detected cheap devices; "
    "i2cmetrics/i2cstats/i2chealth show bus performance, error counters, and per-device "
    "health. Bus recovery: i2cpause/i2cresume stop and restart sensor polling, i2creset "
    "does a pause-recover-resume cycle, and i2crecover <address> clears a single device "
    "degraded state. The device registry is exposed via sensors [filter|json], "
    "sensorinfo <name>, devices, discover, and devicefile; sensorautostart [sensor] "
    "[on|off] controls which sensors start polling automatically at boot.", i2cCommands,          i2cCommandsCount, 0, nullptr },
#endif
#if ENABLE_AUTOMATION
  { "automation", "Scheduled tasks and conditional commands", "The automation module runs saved jobs (stored in automations.json) that execute "
    "one or more CLI commands on a schedule, condition, or system event. Every automation has one of "
    "four trigger types: atTime (fires daily at time=HH:MM, optionally limited to "
    "days=Mon,Tue,...), afterDelay (fires once after delayms milliseconds), interval "
    "(fires repeatedly every intervalms milliseconds), or event (fires when a system "
    "event occurs: on=<kind> with an optional match=<text> filter against the event's "
    "subject/detail — run 'events' to see recent kinds like peer_online, text_rx, "
    "battery_low, login_fail); jobs can also carry runatboot=1 "
    "to fire at startup. The primary entry point is automation <subcommand> (list, add, "
    "enable, disable, delete, run, trigger, sanitize, recompute) with single-word "
    "aliases automationlist, automationadd, automationrun, and automationtrigger. Note "
    "the important distinction: automationrun id=<id> executes a job commands "
    "immediately, whereas automationtrigger id=<id> only arms an afterDelay/manual "
    "timer so it fires after its delay; and automation system enable|disable|status is "
    "the global master switch that gates whether the scheduler runs at all, independent "
    "of each job own enabled flag. Jobs may also include an optional condition "
    "expression, and conditional commands use an IF <expr> THEN <command> [ELSE "
    "<command>] form (e.g. IF temp>75 THEN ledcolor red). Conditions can check sensors "
    "(temp, distance, light, motion), time (time, hour, day, ntp), system state "
    "(battery, heap, psram, fsfree, uptime, chiptemp), connectivity (wifi, rssi, peers, "
    "ble), ESP-NOW/bond (espnow, bond_mode, bond_paired, bond_online, bond_synced, "
    "bond_role, bond_rssi, bond_peer_heap, bond_peer_uptime, pairmode, pairmode_secs, "
    "peersknown, stalestpeerage), location (gps, speed, sats, wp_dist:<name>), the on-device model (llm), "
    "and ESP-NOW metadata "
    "(room, zone, tags); numeric variables use the > < = >= <= != operators and "
    "string/enum variables use = != CONTAINS. A true condition fires every time its "
    "trigger is due. Supporting commands: validate-conditions checks conditional syntax without "
    "running it, autolog records automation activity to a file, and print <message> "
    "broadcasts text to all outputs.", automationCommands,   automationCommandsCount, 0, nullptr },
#endif
#if ENABLE_BATTERY_MONITOR
  { "battery",    "Battery voltage and charge monitoring", "The battery module reports cell state and keeps a time-series log; it is only "
    "present when battery monitoring is compiled in. The backend is a MAX17048 fuel "
    "gauge over I2C (with an ADC or USB-only fallback on other boards), and charging "
    "detection cross-references the gauge CRATE register with a VBUS-present signal so "
    "the reported state distinguishes truly charging from merely USB-powered. "
    "batterystatus prints voltage, charge percentage, charging/USB state, and a coarse "
    "status label, or returns the same data as JSON. batterylog manages a CSV "
    "discharge/charge log written to the device for later graphing: with no args it "
    "shows status, and subcommands are on/off (enable/disable), interval <5..3600> "
    "seconds (sampling period), tail (show the most recent rows), and clear (erase the "
    "log); significant events such as sleep/wake are always recorded regardless of the "
    "interval. batterycalibrate (admin) re-calibrates the ADC-based readings.", batteryCommands,      batteryCommandsCount, 0, nullptr },
#endif
  { "debug",      "System debugging and diagnostics", "The debug subsystem controls diagnostic logging verbosity across every part of the "
    "firmware. Its core is a large set of per-subsystem debug-flag toggles (for example "
    "debugwifi, debughttprequests, debugespnowcore, debugcamera, debugimuvalues) that "
    "each follow a <0|1> [temp|runtime] model: with no mode the new state is persisted "
    "to flash, while temp or runtime flips only the live runtime flag and is NOT saved "
    "(it reverts on reboot). Many subsystems have a parent flag plus finer sub-flags "
    "(lifecycle/polling/values, or core/router/mesh/topo for ESP-NOW); the parent acts "
    "as a master switch and any sub-flag also lights its parent. Separate from the "
    "on/off flags, loglevel sets a severity threshold (error|warn|info|debug, "
    "persisted) and debugverbose is a global override. Related commands manage where "
    "output goes: outserial gates the UART lane (persisted), outg2/outble open "
    "runtime CLI streams to G2 glasses / BLE clients (session-only, reset on "
    "reboot), log starts/stops system-wide logging to a file, loglink routes ESP-IDF "
    "framework logs through the unified output queue, and debugstack/debugbuffer expose "
    "low-level trace and queue diagnostics.", debugCommands,        debugCommandsCount, 0, nullptr },
  { "settings",   "Device configuration and preferences", "The settings subsystem holds the device persisted configuration and the commands "
    "that change it. Each setting command (for example outserial, "
    "serialrequireauth, displayrequireauth, tzoffsetminutes, ntpserver, wifitxpower, "
    "webclihistorysize) sets one value; writes normally go to RAM and are flushed to "
    "the settings JSON on flash. Because flash writes are costly, you can batch them: "
    "beginwrite defers all subsequent writes, then savesettings flushes everything in a "
    "single write and ends the batch (savesettings is also the explicit flush-now "
    "command after individual changes). Most commands here are admin-gated. Some "
    "changes only take effect after a reboot (for example espnowenabled and "
    "httpsEnabled are marked reboot required). The controls command emits a "
    "machine-readable JSON descriptor of a module settable controls for UI use. Note "
    "that most subsystem settings (wifi, i2c, sensors, power, oled, bluetooth, espnow) "
    "are owned and registered by their own modules; this module hosts the cross-cutting "
    "CLI/output/auth/time settings plus the batch-write machinery.", settingsCommands,     settingsCommandsCount, 0, nullptr },
  { "sensorlog", "Sensor data logging to files", "Periodically samples the onboard sensors and appends readings to a file, driven by "
    "the single multiplexed sensorlog <subcommand> command. sensorlog start <filepath> "
    "[interval_ms] begins logging (default 5000 ms; the filepath must start with / and "
    "parent directories are created automatically) and sensorlog stop ends it; only one "
    "log can run at a time, so start refuses if logging is already active. sensorlog "
    "status reports the active file, interval, format, rotation settings, selected "
    "sensors, and last-write age. Configure behavior with format <text|csv|track> "
    "(track is a compact GPS-only format with signal-loss dedup), maxsize and rotations "
    "for log rotation, and sensors <thermal|tof|imu|gamepad|apds|gps|presence|r1|all|none> "
    "to choose which sensors are recorded. sensorlog interval <ms> sets the poll period "
    "(100-3600000, default 5000). NOTE: format track additionally REPLACES the sensor "
    "mask with GPS-only and persists it, so a prior selection is lost — and rotations 0 "
    "deletes the active file at the size cap rather than pruning older generations. "
    "sensorlog autostart [on|off] makes logging "
    "resume on the next boot using the last-used parameters; the "
    "format/maxsize/rotations/sensors/autostart choices are persisted.", sensorLoggingCommands, sensorLoggingCommandsCount, 0, nullptr },
  { "users",      "User authentication and management", "The users subsystem provides admin-gated account management, authentication, "
    "sessions, and bans. Accounts have two roles, admin and standard; the first account "
    "is the owner-admin, and userpromote/userdemote change roles while useradd creates "
    "an account directly (optionally forcing a password change on first login). New "
    "accounts can also come through an approval flow: userrequest files a pending "
    "request that an admin clears with userapprove or rejects with userdeny "
    "(pendinglist shows the queue). login and logout authenticate per transport "
    "(serial, display, bluetooth, g2), userlist enumerates accounts, and the password "
    "commands cover both self-service (userchangepassword) and admin reset "
    "(userresetpassword). Sessions are tracked per transport: sessionlist shows active "
    "sessions and sessionrevoke force-logs-out a session by SID or by username. Two "
    "independent ban mechanisms exist: ban/unban/banlist block an IP address, while "
    "banuser/unbanuser suspend a user account so it cannot log in until unbanned; the "
    "primary admin account cannot be banned. usersync pushes a user credentials to "
    "another device over ESP-NOW, authenticated by an admin account on the receiving "
    "device.", userSystemCommands,         userSystemCommandsCount, CMD_MODULE_ADMIN, nullptr },
  { "features",   "System feature management", "The features subsystem enables or disables compiled-in capabilities at runtime and "
    "reports their memory cost. features with no argument lists every feature grouped "
    "by category (Network, Display, Sensors, System) with an approximate heap estimate "
    "and a status of ON, OFF, or N/C (not compiled in this build); features <id> shows "
    "one feature details and features <id> <on|off> toggles it, persisting the change "
    "immediately. Only features that are compiled and marked runtime-toggleable can be "
    "changed; a few are compile-time only, and some (wifi, oled, i2c, https) are "
    "flagged reboot required so the toggle persists but the capability does not "
    "actually start or stop until the next restart. featuresetup launches an "
    "interactive, admin-only wizard that walks through the same toggles and works from "
    "any CLI transport.", featureCommands,      featureCommandsCount, 0, nullptr },
#if ENABLE_CAMERA_SENSOR
  { "image",      "Image capture and management", "Captures stills from the camera and manages the saved photo library. capture grabs "
    "a frame and saves it as a JPG (target storage chosen by argument: littlefs/lfs, "
    "sd, or both; default follows the cameraStorageLocation setting), and requires a "
    "camera sensor that is both compiled in and enabled -- on boards with no camera the "
    "capture simply fails. images lists saved photos with sizes and storage stats (add "
    "sd to list the card, json for app/BLE output); imagedelete \"<path>\" removes one "
    "(path must be quoted). imagesend transmits a photo to another device over ESP-NOW: "
    "imagesend <device> \"<path>\" resolves the device by name or MAC and sends the "
    "named file (path required).", imageCommands,        imageCommandsCount, 0, nullptr },
#endif
#if ENABLE_MAPS
  { "map",        "Map navigation and waypoints", "On-device offline map subsystem backed by region map files stored under /maps/ "
    "(custom HWMap tile format). A map must be loaded before any lookup works: mapload "
    "\"<path>\" loads a file into PSRAM, maplist shows what is available, map prints "
    "the current map region/feature-count/bounds (add json for structured output), and "
    "mapunload frees the PSRAM and tile cache. search <name> finds named features in "
    "the loaded map, while whereami reports the nearest road and area for the current "
    "GPS position and therefore needs both a loaded map and a live GPS fix. Waypoints "
    "are persistent user markers managed through waypoint "
    "(list/add/del/goto/clear/clearall/rename/notes) and can have files attached via "
    "waypointfile/waypointfiles; gpstrack loads, inspects, or clears a recorded GPS "
    "breadcrumb track (and rejects tracks that fall outside the loaded map bounds), and "
    "maporganize sorts loose files in /maps into subdirectories.", mapCommands,          mapCommandsCount, 0, nullptr },
  { "mapsettings","Maps app settings (zoom, layers, cache)", "Persisted rendering defaults for the maps app, stored under apps.maps and applied "
    "to the live map at boot. mapzoom <0.5..20.0> sets the initial zoom, maplayers "
    "<0..1023> sets a bitmask controlling which feature layers are drawn, and "
    "mapcachekb <256..4096> sizes the tile LRU cache pool. The zoom and layers setters "
    "also mirror immediately into the running renderer so changes take effect without a "
    "reboot, but the cache size only re-applies on the next map load (or reboot). All "
    "three are admin-only and, run from the CLI, write to flash immediately so they "
    "survive a reboot.", mapsSettingCommands, mapsSettingCommandsCount, 0, nullptr },
#endif
  { "power",      "Power management", "The power subsystem manages CPU frequency and battery-oriented power saving. The "
    "main command is power: power alone prints the current mode, CPU clock, display "
    "brightness, and auto-mode state; power mode <perf|balanced|saver|ultra|locked|0-4> "
    "selects one of five preset modes (Performance 240/80 MHz, Balanced 160/80 MHz, "
    "PowerSaver 80 MHz, UltraSaver 80 MHz interactive / 40 MHz idle, Locked 240 MHz always) "
    "which sets both the CPU frequency and the display brightness; the chosen mode is "
    "persisted. Locked alone holds 240 MHz through idle power-save (OLED blanks but the "
    "core does not downclock). UltraSaver's headline 40 MHz is idle-only — it is "
    "applied solely when idle power-save blanks the screen (40 MHz is too laggy for the "
    "live UI) and any input or command restores >=80 MHz; so UltraSaver only reaches "
    "40 MHz if powersave is enabled. power auto <on|off> enables an "
    "automatic low-battery downshift gated by power threshold <0-100>. Two related idle "
    "controls are separate commands: powersave <0..1440> sets an idle timeout (minutes; "
    "0 disables) after which the OLED blanks and the CPU may downclock (mode-dependent) "
    "while the radio stays up so the device remains reachable, and powercooldown "
    "<0..60000> sets an anti-flap cooldown (milliseconds) that prevents rapid "
    "back-to-back sleep transitions. All of these values persist.", powerCommands,        powerCommandsCount, 0, nullptr },
#if ENABLE_OLED_DISPLAY
  { "setpattern", "OLED gamepad password entry", "Provides the single admin-only command setgamepadpassword, which opens the "
    "gamepad-pattern password setup flow on the OLED screen. A pattern is a sequence of "
    "joystick directions that is hashed and stored as the logged-in user password, "
    "usable for on-device login. You must already be logged in at the OLED display "
    "first (the command errors otherwise); the guided on-screen flow then "
    "re-authenticates you, prompts you to enter the new pattern and confirm it, and "
    "saves it to your account. This command only launches the OLED mode -- the actual "
    "entry and confirmation happen on the device screen.", setPatternCommands,   setPatternCommandsCount, 0, nullptr },
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "even_g2",    "Even G2 smart glasses control", "This subsystem drives Even Reality G2 smart glasses while Bluetooth is in client "
    "mode (blemode client); the two temples are addressed as left/right/auto. openg2 "
    "starts scan-and-connect IN THE BACKGROUND and returns immediately -- it does not "
    "block, so poll g2status (or g2info for firmware/MAC/battery) to see when the link "
    "is up, and nearly every other command here requires that connection first. Display "
    "commands render to the lens: g2show prints text, g2ai/g2ai-noask/g2ai-direct push "
    "a front-pane AI answer card through the EvenAI pipeline, g2bmp shows a BMP file, "
    "and g2sensors/g2network/g2files/g2settingspage show built-in info pages; g2nav "
    "[on|off] enables menu-navigation mode and g2clear blanks the display. For audio, "
    "g2mic only sends the enable/disable control frame (LC3 decode is not yet wired, so "
    "no audio arrives); the working capture path is g2micrec (raw LC3 packets to SD) "
    "and g2micwav (decodes to a 16 kHz mono WAV on SD), each an SD-backed "
    "start/stop/status lifecycle that needs an SD card. closeg2 disconnects but keeps "
    "the GATT cache for fast reconnect; closeg2 full also frees the cache to recover "
    "about 30 KB. The remaining g2* commands are low-level protocol probes and "
    "diagnostics (g2probe, g2protostats, g2devcfg, g2dumpframes).", g2Commands,           g2CommandsCount, 0, nullptr },
  { "even_r1",    "Even R1 ring control (info-only)", "This subsystem talks to the Even R1 smart ring over BLE and is "
    "read-only/info-only: it queries the ring health and status data but does not "
    "control it. ringscan [seconds] discovers the ring and ringconnect [mac] connects "
    "(auto-scanning when no MAC is given, or connecting directly when one is), with "
    "ringstatus and ringdisconnect for state and teardown. ringquery is the main data "
    "command, requesting "
    "wear/health/heart-rate/HRV/SpO2/temperature/activity/sleep/report readings (or a "
    "raw module/cmd frame), and ringverbose toggles a full hex dump of the ring notify "
    "frames for debugging. Note that bridging ring data onto the G2 glasses is "
    "deliberately unavailable -- the commands exist in the code but are intentionally "
    "left unregistered because both approaches proved to be dead ends.", g2RingCommands,    g2RingCommandsCount, 0, nullptr },
#endif
#if ENABLE_ONDEVICE_LLM
  { "llm",        "On-device LLM text generation", "On-device large language model that runs a quantized model file entirely on the "
    "device (model weights held in PSRAM). A model must be loaded before generation: "
    "llmload [file.bin] loads one (bare filenames are looked up on the SD card under "
    "/sd/llm then internal /system/llm), llmmodels lists available files, llmunload "
    "frees the PSRAM, llmstatus shows engine state, and llmautostart 0|1 / "
    "llmdefaultmodel control boot-time loading. Two generate forms: bare 'llmgenerate "
    "<prompt>' BLOCKS and prints the whole reply, while 'llmgenerate json ...' starts "
    "async and returns a session id immediately — then poll llmresult json <offset> "
    "repeatedly (each call returns new text, the running total length, and a done flag) "
    "until done flips true; llmstop aborts an in-progress generation. The engine keeps a "
    "multi-turn conversation: llmclear resets it, llmretry regenerates the last reply "
    "(async), and llmturns json <index> reads back one turn at a time. The llm* setters "
    "(temperature, topp, minp, maxtokens, sentencelimit, hardcap, reppenalty/repwindow, "
    "maxcontext, kvprec, norepeatngram, confthreshold, contentboost) are admin-only "
    "sampler and KV-cache defaults that persist to flash; kvprec and maxcontext only take "
    "effect on the next model load.", llmCommands,          llmCommandsCount, 0, nullptr },
#endif
  { "settingsedit", "Per-field settings save commands",
    "Static CLI commands the web/OLED settings screen uses to persist individual settings fields that have no dedicated module command. Each writes one setting via handleSettingCommand; the value is read live or applied on next start. Fields that need a live apply action are routed to their module command instead of getting one of these.",
    settingEditorCommands, settingEditorCommandsCount, 0, nullptr },
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
  if (!entry) return false;
  // super implies admin: a super-only command is always at least admin-gated,
  // even if its row forgot requiresAdmin=true. Enforced here (the single reader
  // of the raw flag) so the invariant holds for every caller — the auth gate
  // and the help/visibility helper alike — with no per-row discipline. Mirrors
  // isAdminUser(), where a superadmin also satisfies the admin check.
  return entry->requiresAdmin || entry->requiresSuperAdmin;
}

// Top-tier gate, read straight from the command table (single source of truth,
// same as commandRequiresAdmin) — the requiresSuperAdmin flag lives with each
// command's own registration, so a command can never be dangerous-but-ungated
// through a stale parallel list.
bool commandRequiresSuperAdmin(const String& cmdLine) {
  const CommandEntry* entry = findCommand(cmdLine);
  return entry ? entry->requiresSuperAdmin : false;
}

// Dispatch command to handler (simple version without auth context)
// The full executeCommand() in .ino handles auth, logging, help mode, etc.
const char* dispatchCommand(const String& argsInput) {
  const CommandEntry* entry = findCommand(argsInput);
  if (!entry) {
    return "Error: Unknown command";
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

  // Header + DRAM summary packed into 2 envelopes (was 8). DRAM value group
  // worst case ~172 B (4 lines), safely under the 255 B slot.
  broadcastOutput("\n========== BOOT MEMORY REPORT (Task Manager) ==========\n\n-- DRAM (Internal Heap) --");
  BROADCAST_PRINTF(
    "  Total:      %7lu bytes (%3lu KB)\n"
    "  Used:       %7lu bytes (%3lu KB) [%2u%%]\n"
    "  Free:       %7lu bytes (%3lu KB) [%2u%%]\n"
    "  Peak Used:  %7lu bytes (%3lu KB) [%2u%%]",
    (unsigned long)dram_total,     (unsigned long)(dram_total / 1024),
    (unsigned long)dram_used,      (unsigned long)(dram_used / 1024),      (unsigned)((dram_used * 100) / dram_total),
    (unsigned long)dram_free,      (unsigned long)(dram_free / 1024),      (unsigned)((dram_free * 100) / dram_total),
    (unsigned long)dram_peak_used, (unsigned long)(dram_peak_used / 1024), (unsigned)((dram_peak_used * 100) / dram_total));

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

  // BSS/NOINIT packed into 1 envelope (was 5). Worst case ~150 B.
  BROADCAST_PRINTF(
    "\n  BSS (Internal): %7lu bytes (%3lu KB)\n"
    "  BSS (PSRAM):    %7lu bytes (%3lu KB)\n"
    "  NOINIT (Int):   %7lu bytes (%3lu KB)\n"
    "  NOINIT (PSRAM): %7lu bytes (%3lu KB)",
    (unsigned long)bss_internal_bytes,    (unsigned long)(bss_internal_bytes / 1024),
    (unsigned long)bss_psram_bytes,       (unsigned long)(bss_psram_bytes / 1024),
    (unsigned long)noinit_internal_bytes, (unsigned long)(noinit_internal_bytes / 1024),
    (unsigned long)noinit_psram_bytes,    (unsigned long)(noinit_psram_bytes / 1024));

  // heap_caps view — gives the truth about WHERE allocations land
  // (internal DRAM vs DMA-capable vs PSRAM) and how fragmented each
  // pool is. The "largest free block" is the indicator: when it
  // diverges from total free, the heap is fragmented and big allocs
  // (page-swap workers, image probes) will fail even with apparent
  // headroom.
  size_t cap_int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t cap_int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t cap_dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
  size_t cap_dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
  // Heap-caps header + INTERNAL + DMA packed into 1 envelope (was 4). ~161 B.
  BROADCAST_PRINTF(
    "\n-- HEAP CAPS (allocator-eye view) --\n"
    "  INTERNAL  free=%7lu B (%3lu KB)  largest=%7lu B  frag=%2u%%\n"
    "  DMA-able  free=%7lu B (%3lu KB)  largest=%7lu B",
                   (unsigned long)cap_int_free, (unsigned long)(cap_int_free / 1024),
                   (unsigned long)cap_int_largest,
                   cap_int_free ? (unsigned)(100 - (cap_int_largest * 100) / cap_int_free) : 0,
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
        // BYTES: the *_STACK_WORDS constants hold byte counts (ESP-IDF's
        // xTaskCreate takes usStackDepth in BYTES), and usStackHighWaterMark is
        // bytes too on this port. The old "* 4" printed 4x the real allocation
        // and was the bulk of this report's own "Static Over-Estimate".
        // See System_TaskUtils.h.
        size_t allocatedBytes = allocatedWords;
        size_t freeBytes = taskStatusArray[i].usStackHighWaterMark;
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
        // usStackHighWaterMark is in BYTES on this port — the old "* 4" made
        // these look 4x roomier than they are. See System_TaskUtils.h.
        size_t freeBytes = taskStatusArray[i].usStackHighWaterMark;
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
  broadcastOutput(
    "  First-Time Setup State:\n"
    "    gFirstTimeSetupState:        4 bytes\n"
    "    gSetupProgressStage:         4 bytes\n"
    "    gFirstTimeSetupPerformed:    1 bytes");
  static_vars_total += 9;
  
  // Sensor Module State Variables
  broadcastOutput("  Sensor Modules (Global State):");
  size_t thermal_state_bytes = sizeof(gThermalCache) + sizeof(gThermalRunning) + sizeof(gThermalConnected) + sizeof(gThermalTaskHandle);
  size_t imu_state_bytes = sizeof(gImuCache) + sizeof(gImuRunning) + sizeof(gImuConnected) + sizeof(gImuTaskHandle);
  size_t tof_state_bytes = sizeof(gTofCache) + sizeof(gTofRunning) + sizeof(gTofConnected) + sizeof(gTofTaskHandle);
  size_t gamepad_state_bytes = sizeof(gInputCache) + sizeof(gInputRunning) + sizeof(gInputConnected) + sizeof(gInputTaskHandle);
  size_t apds_state_bytes = sizeof(gApdsCache) + sizeof(gApdsConnected) + sizeof(gApdsColorRunning) + sizeof(gApdsProximityRunning) + sizeof(gApdsGestureRunning);
  size_t gps_state_bytes = sizeof(gGpsRunning) + sizeof(gGpsConnected);
  size_t oled_state_bytes = sizeof(gOledRunning) + sizeof(oledConnected);

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
  broadcastOutput(
    "  I2C System:\n"
    "    Clock Stack:        32 bytes\n"     // Fixed 8-slot array inside I2CDeviceManager
    "    Mutex Objects:     ~64 bytes");
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
  // TOTALS — grouped: header+DRAM (1 envelope), over-budget block (1), PSRAM (1+opt).
  BROADCAST_PRINTF(
    "\n---------- TOTALS ----------\n"
    "  TOTAL ACCOUNTED:      %6lu bytes (%3lu KB)\n"
    "  ACTUAL DRAM USED:     %6lu bytes (%3lu KB)",
                  (unsigned long)total_known, (unsigned long)(total_known / 1024),
                  (unsigned long)dram_used,   (unsigned long)(dram_used / 1024));

  if (dram_used > total_known) {
    size_t unaccounted = dram_used - total_known;
    size_t overestimate = (static_total > unaccounted) ? (static_total - unaccounted) : 0;
    BROADCAST_PRINTF(
      "  Unaccounted DRAM:     %6lu bytes (%3lu KB)\n"
      "  Static Over-Estimate: %6lu bytes (%3lu KB)\n"
      "  (Static estimates are conservative upper bounds)",
                    (unsigned long)unaccounted,  (unsigned long)(unaccounted / 1024),
                    (unsigned long)overestimate, (unsigned long)(overestimate / 1024));
  }

  // Show PSRAM accounting if available
  if (has_ps && useDynamicTracking) {
    BROADCAST_PRINTF(
      "\n  PSRAM ACCOUNTED:      %6lu bytes (%3lu KB)\n"
      "  ACTUAL PSRAM USED:    %6lu bytes (%3lu KB)",
                    (unsigned long)tracked_psram, (unsigned long)(tracked_psram / 1024),
                    (unsigned long)ps_used,       (unsigned long)(ps_used / 1024));
    if (ps_used > tracked_psram) {
      size_t unaccounted_psram = ps_used - tracked_psram;
      BROADCAST_PRINTF("  Unaccounted PSRAM:    %6lu bytes (%3lu KB)",
                      (unsigned long)unaccounted_psram, (unsigned long)(unaccounted_psram / 1024));
    }
  }

  broadcastOutput("\n========== END MEMORY REPORT ==========\n");
}

// Command handlers
const char* cmd_memreport(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    size_t dramTotal = ESP.getHeapSize();
    size_t dramFree  = ESP.getFreeHeap();
    size_t dramMin   = ESP.getMinFreeHeap();
    JsonObject dram = doc["dram"].to<JsonObject>();
    dram["total"]    = (unsigned long)dramTotal;
    dram["used"]     = (unsigned long)(dramTotal - dramFree);
    dram["free"]     = (unsigned long)dramFree;
    dram["minFree"]  = (unsigned long)dramMin;
    dram["peakUsed"] = (unsigned long)(dramTotal - dramMin);
    JsonObject ps = doc["psram"].to<JsonObject>();
    bool hasPs = psramFound();
    ps["available"] = hasPs;
    if (hasPs) {
      size_t psTotal = ESP.getPsramSize();
      size_t psFree  = ESP.getFreePsram();
      ps["total"] = (unsigned long)psTotal;
      ps["used"]  = (unsigned long)(psTotal - psFree);
      ps["free"]  = (unsigned long)psFree;
    }
    JsonObject hc = doc["heapCaps"].to<JsonObject>();
    hc["internalFree"]    = (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    hc["internalLargest"] = (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    hc["dmaFree"]         = (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA);
    hc["dmaLargest"]      = (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }
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

  // JSON form — a single object for the app/web (the OLED Perf "Stack" page
  // mirror). Serializes into the shared debug buffer (4 KB) like memreport json;
  // ~30 tasks land well under the ~4 KB BLE reply cap.
  if (argWantsJson(originalCmd)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["total"]  = (unsigned)taskCount;
    JsonArray arr = doc["tasks"].to<JsonArray>();
    for (UBaseType_t i = 0; i < actualCount; i++) {
      const char* state;
      switch (taskArray[i].eCurrentState) {
        case eRunning:   state = "RUN";   break;
        case eReady:     state = "READY"; break;
        case eBlocked:   state = "BLOCK"; break;
        case eSuspended: state = "SUSP";  break;
        case eDeleted:   state = "DEL";   break;
        default:         state = "UNK";   break;
      }
      JsonObject t = arr.add<JsonObject>();
      t["name"]  = taskArray[i].pcTaskName ? taskArray[i].pcTaskName : "?";
      t["state"] = state;
      t["prio"]  = (unsigned)taskArray[i].uxCurrentPriority;
      // usStackHighWaterMark is already BYTES under ESP-IDF — emit raw, never
      // *4 (see System_TaskUtils.h / feedback_task_stack_bytes_not_words).
      t["stackFree"] = (unsigned)taskArray[i].usStackHighWaterMark;
    }
    serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);
    return getDebugBuffer();
  }

  BROADCAST_PRINTF(
    "Task Statistics:\n"
    "=================\n"
    "Total Tasks: %u\n\n"
    "Task Name          State  Prio  Stack\n"
    "================== ===== ===== ======",
    (unsigned)taskCount);

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


// perftop — live performance snapshot. Prints the main-loop health (from the
// loopHealthTick profiler in HardwareOne.cpp) plus a fresh per-task CPU%
// sampled over ~0.75 s. Runs on the cmd_exec task, so the short sleep does NOT
// stall the main loop. uint64 math avoids the *100 overflow that corrupts the
// lifetime CPU% column.
const char* cmd_perftop(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // JSON form — the OLED Perf "CPU" page mirror for the app/web: loop-health
  // strip (struct-read, same source as the OLED) + a fresh per-task CPU% over a
  // 0.75 s sample. Returns one object; the text path below is unchanged.
  if (argWantsJson(originalCmd)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;

    uint32_t lapsPerSec = 0, avgMs = 0, maxMs = 0, stalls5s = 0, totalStalls = 0;
    bool loopValid = perfGetLoopSnapshot(lapsPerSec, avgMs, maxMs, stalls5s, totalStalls);
    JsonObject loop = doc["loop"].to<JsonObject>();
    loop["valid"]       = loopValid;   // false while the first 5 s window fills
    loop["lapsPerSec"]  = (unsigned long)lapsPerSec;
    loop["avgMs"]       = (unsigned long)avgMs;
    loop["maxMs"]       = (unsigned long)maxMs;
    loop["stalls5s"]    = (unsigned long)stalls5s;
    loop["totalStalls"] = (unsigned long)totalStalls;

    JsonArray tasks = doc["tasks"].to<JsonArray>();
    bool cpuValid = false;

    // Two run-time-stat snapshots ~750 ms apart, delta by handle (handle reuse
    // rejected), identical to the text path. Runs on cmd_exec so the short
    // sleep never stalls the main loop.
    UBaseType_t n = uxTaskGetNumberOfTasks() + 6;
    TaskStatus_t* a = (TaskStatus_t*)ps_alloc(n * sizeof(TaskStatus_t), AllocPref::PreferPSRAM, "perftop.ja");
    TaskStatus_t* b = (TaskStatus_t*)ps_alloc(n * sizeof(TaskStatus_t), AllocPref::PreferPSRAM, "perftop.jb");
    if (a && b) {
      uint32_t totA = 0, totB = 0;
      UBaseType_t na = uxTaskGetSystemState(a, n, &totA);
      vTaskDelay(pdMS_TO_TICKS(750));
      UBaseType_t nb = uxTaskGetSystemState(b, n, &totB);
      uint32_t totalDelta = totB - totA;
      if (totalDelta != 0 && na != 0 && nb != 0) {
        cpuValid = true;
        for (UBaseType_t i = 0; i < nb; i++) {
          const char* nm = b[i].pcTaskName;
          if (!nm) continue;
          uint32_t prev = 0; bool found = false;
          for (UBaseType_t j = 0; j < na; j++) {
            if (a[j].xHandle == b[i].xHandle) { prev = a[j].ulRunTimeCounter; found = true; break; }
          }
          if (!found) continue;                            // task new this interval
          uint32_t d = b[i].ulRunTimeCounter - prev;       // wrap-safe unsigned
          if (d > totalDelta + totalDelta / 4) continue;   // handle reuse / wrap → skip
          uint32_t pct = (uint32_t)((uint64_t)d * 100 / totalDelta);
          if (pct > 100) pct = 100;
          bool isIdle = (strncmp(nm, "IDLE", 4) == 0);
          // Keep IDLE rows (per-core headroom) + non-idle >= 1%, dropping the 0%
          // noise to bound the payload — same filter as the text/OLED CPU page.
          if (!isIdle && pct < 1) continue;
          JsonObject t = tasks.add<JsonObject>();
          t["name"]   = nm;   // TCB-owned pointer; serialized below while a/b alive
          t["cpuPct"] = (unsigned long)pct;
          t["idle"]   = isIdle;
        }
      }
    }
    doc["cpuValid"] = cpuValid;

    // Serialize BEFORE freeing a/b — the task-name pointers linked into `doc`
    // reference storage that outlives them (TCB), but a task deleted mid-sample
    // could dangle; serializing first closes that window entirely.
    serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);
    if (a) free(a);
    if (b) free(b);
    return getDebugBuffer();
  }

  broadcastOutput("");
  broadcastOutput("========== PERFTOP ==========");

  // 1) Main-loop health snapshot (laps/s, period, per-section avg, worst stalls)
  perfPrintLoopHealth();

  // 2) Live per-task CPU%: two run-time-stat snapshots ~750 ms apart, delta by
  //    handle (handles can be reused, so a delta > elapsed is rejected).
  broadcastOutput("");
  broadcastOutput("[PERFTOP] live task CPU (0.75s sample):");

  UBaseType_t n = uxTaskGetNumberOfTasks() + 6;  // headroom for tasks created mid-sample
  TaskStatus_t* a = (TaskStatus_t*)ps_alloc(n * sizeof(TaskStatus_t), AllocPref::PreferPSRAM, "perftop.a");
  TaskStatus_t* b = (TaskStatus_t*)ps_alloc(n * sizeof(TaskStatus_t), AllocPref::PreferPSRAM, "perftop.b");
  if (!a || !b) {
    if (a) free(a);
    if (b) free(b);
    broadcastOutput("  (cannot allocate task snapshot buffers)");
    broadcastOutput("========== END PERFTOP ==========");
    return "OK";
  }

  uint32_t totA = 0, totB = 0;
  UBaseType_t na = uxTaskGetSystemState(a, n, &totA);
  vTaskDelay(pdMS_TO_TICKS(750));
  UBaseType_t nb = uxTaskGetSystemState(b, n, &totB);
  uint32_t totalDelta = totB - totA;

  if (totalDelta == 0 || na == 0 || nb == 0) {
    broadcastOutput("  (run-time stats unavailable)");
  } else {
    // pass 0: IDLE0/IDLE1 (per-core headroom); pass 1: non-idle tasks >= 1%.
    int shown = 0;
    for (int pass = 0; pass < 2; pass++) {
      for (UBaseType_t i = 0; i < nb; i++) {
        const char* nm = b[i].pcTaskName;
        if (!nm) continue;
        bool isIdle = (strncmp(nm, "IDLE", 4) == 0);
        if (pass == 0 && !isIdle) continue;
        if (pass == 1 && isIdle) continue;
        uint32_t prev = 0; bool found = false;
        for (UBaseType_t j = 0; j < na; j++) {
          if (a[j].xHandle == b[i].xHandle) { prev = a[j].ulRunTimeCounter; found = true; break; }
        }
        if (!found) continue;                            // task new this interval
        uint32_t d = b[i].ulRunTimeCounter - prev;       // wrap-safe unsigned
        if (d > totalDelta + totalDelta / 4) continue;   // handle reuse / wrap → skip
        uint32_t pct = (uint32_t)((uint64_t)d * 100 / totalDelta);
        if (pct > 100) pct = 100;
        if (pass == 0) {
          BROADCAST_PRINTF("  %-16.16s idle %3lu%%", nm, (unsigned long)pct);
        } else if (pct >= 1) {
          BROADCAST_PRINTF("  %-16.16s %3lu%%", nm, (unsigned long)pct);
          shown++;
        }
      }
    }
    if (shown == 0) broadcastOutput("  (all non-idle tasks <1% this interval)");
  }

  free(a);
  free(b);
  broadcastOutput("========== END PERFTOP ==========");
  return "OK";
}


// ============================================================================
// Command Execution Functions - MIGRATED from main .ino
// ============================================================================
// External dependencies for command execution
extern bool gAutomationLogActive;
extern CLIState gCLIState;
extern bool gCLIValidateOnly;
extern QueueHandle_t gCmdExecQ;

// External functions
extern bool handleHelpNavigation(const String& cmd, char* out, size_t outSize);
extern String exitToNormalBanner();
extern String redactCmdForAudit(const String& argsInput);
extern bool hasAdminPrivilege(const AuthContext& ctx);
extern bool hasSuperAdminPrivilege(const AuthContext& ctx);
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

// Guest accounts may authenticate, but the command surface is login/logout
// only. View-only UX lives in web/OLED; this is the CLI chokepoint shared by
// every transport that funnels through executeCommand.
static bool commandAllowedForGuest(const String& line) {
  String lc = line;
  lc.toLowerCase();
  lc.trim();
  int sp = lc.indexOf(' ');
  String cmd = (sp > 0) ? lc.substring(0, sp) : lc;
  return cmd == "login" || cmd == "logout";
}

// Centralized authorization for a command line and context.
// Returns true if authorized, otherwise writes an error to 'out' and returns false.
static bool authorizeCommand(const AuthContext& ctx, const String& line, char* out, size_t outSize) {
  // Trusted internal identity (status REST facades, system FS). Same privilege
  // model as FsRole::SYSTEM — never a loginable account.
  if (ctx.transport == SOURCE_INTERNAL && ctx.user == "system") {
    return true;
  }
  // External transports must carry a real username (or AuthBypass when a
  // transport's require-auth toggle is off). Empty identity used to slip past
  // guest/admin gates. Exception: the login command itself is pre-auth and
  // may arrive with an empty user (BLE/web-style credential submit).
  // SOURCE_INTERNAL may still use an empty owner string for some automation
  // rows — those keep the old non-admin-only surface below.
  if (ctx.user.length() == 0 && ctx.transport != SOURCE_INTERNAL) {
    String cmdName = line;
    int spacePos = line.indexOf(' ');
    if (spacePos > 0) cmdName = line.substring(0, spacePos);
    cmdName.trim();
    if (!cmdName.equalsIgnoreCase("login")) {
      snprintf(out, outSize, "Error: Authentication required.");
      { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactCmdForAudit(line).c_str()); logAuthAttempt(false, ctx.path.c_str(), "(anonymous)", ctx.ip, auditBuf); }
      return false;
    }
  }
  // Guest: authenticated but view-only. Deny before admin/super checks so a
  // guest never reaches an admin-gated handler by accident.
  if (ctx.user.length() > 0 && isGuestUser(ctx.user) && !commandAllowedForGuest(line)) {
    String cmdName = line;
    int spacePos = line.indexOf(' ');
    if (spacePos > 0) cmdName = line.substring(0, spacePos);
    snprintf(out, outSize, "Error: Guest accounts are view-only. Only login/logout are allowed.");
    { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactCmdForAudit(line).c_str()); logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf); }
    systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(), cmdName.c_str());
    return false;
  }
  // Super-admin (top-tier) protection. Checked independently of the admin flag
  // so a super-only command is gated even if its table entry is not adminOnly.
  // A bonded session satisfies this (bond == super); a regular mesh/pair
  // account never does — the elevation lives entirely in isSuperAdminUser.
  if (commandRequiresSuperAdmin(line) && !hasSuperAdminPrivilege(ctx)) {
    String cmdName = line;
    int spacePos = line.indexOf(' ');
    if (spacePos > 0) cmdName = line.substring(0, spacePos);
    snprintf(out, outSize, "Error: Super-admin access required for command '%s'.", cmdName.c_str());
    { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactCmdForAudit(line).c_str()); logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf); }
    if (ctx.user.length() > 0) {
      systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(), cmdName.c_str());
    }
    return false;
  }
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
    // A privileged command was refused. Post only when a username is resolved
    // (a LOGGED-IN, non-admin user) — anonymous/unauthenticated denials have no
    // subject and are covered by the login-failure event elsewhere.
    if (ctx.user.length() > 0) {
      systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(), cmdName.c_str());
    }
    return false;
  }
  return true;
}

// Core command execution with authentication and registry dispatch
// ===========================================================================
// OUTPUT CONTRACT (read before adding command output)
// ===========================================================================
// Every command emits through two channels with DIFFERENT shapes — they are
// not interchangeable, and a command must not split one logical payload across
// both:
//
//   1. broadcastOutput()  — HUMAN, line-oriented. One call == one line,
//      clamped to ~255 B (longer is [CUT]). Live-streamed to every active sink
//      (serial / web / BLE-per-connection / log / OLED / G2) and decorated with
//      timestamps on the live path. This is the spine for people-facing output
//      and progress. A command that streams here should return a short status
//      string (e.g. "OK").
//
//   2. the return value (copied into `out`, <=4 KB) — the command's canonical
//      RESULT as a single verbatim blob. No 255 B line clamp, no decoration.
//      This is the channel for byte-exact / machine-readable output (JSON) and
//      is what MQTT publishes and what BLE/web deliver as the addressed reply.
//      Request/response callers may also FOLD the broadcast stream into this
//      blob via captureOutput (see cmd_exec worker) — that is how a human-
//      streaming command still yields an HTTP body.
//
// JSON / structured mode: when a command is asked for `json` (see
// argWantsJson), it must put the whole document in the return value and emit
// NOTHING via broadcastOutput — otherwise the live lines and the blob
// interleave and the JSON cannot be parsed.
//
// Rollout is incremental: `status` is the pilot; other info commands follow.
// ===========================================================================

// ---------------------------------------------------------------------------
// Uniform status-token stamp — the "OK:" half of the OK:/Error: return contract
// above. Applied to every SUCCESS result at the single executeCommand funnel, so
// the leading status word reaches ALL interfaces (serial/web/BLE/OLED/MQTT/
// ESP-NOW) at once. Failures already carry "Error"/"ERROR"; this gives success
// the symmetric token. Exemptions keep machine-read tokens and structured
// payloads byte-exact.
// ---------------------------------------------------------------------------

// Prepend "OK: " to a successful, human-facing result in place. No-op for
// failures (success==false), empty results, JSON/structured payloads ({,[),
// results that already lead with a status word (OK / OK: / SUCCESS), and
// validate-pass sentinels (gCLIValidateOnly -> "VALID"). Machine-readable
// results are protected STRUCTURALLY by the JSON ({,[) rule — there is no
// per-command allowlist to maintain.
static void stampOkStatus(char* out, size_t outSize, bool success) {
  if (!success || !out || outSize < 6) return;
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return;                          // validate sentinels (e.g. "VALID")
  size_t n = strlen(out);
  if (n == 0) return;
  if (out[0] == '{') return;                             // JSON object - structured payload
  if (out[0] == '[') {
    // Exempt a real JSON ARRAY ([{  ["  []  [<digit>  [-) but NOT a tag-first
    // result like "[Thermal] 23.5C" - those are successes and must get the OK:
    // stamp like every other success (a tag's 2nd char is a letter; a JSON-array
    // start is not). No command returns a literal [-array today, so this only
    // ever fires defensively for a future serialized JSON array.
    char c = out[1];  // safe: out[0] != '\0', so out[1] is a char or the terminator
    if (c == '{' || c == '"' || c == ']' || c == '-' || (c >= '0' && c <= '9')) return;
  }
  if (out[0] == 'O' && out[1] == 'K' &&
      (out[2] == '\0' || out[2] == ':')) return;         // bare "OK" sentinel or already "OK:"
  if (strncmp(out, "SUCCESS", 7) == 0) return;           // existing success token — no double-stamp
  const size_t pfx = 4;                                  // "OK: "
  size_t keep = (n + pfx <= outSize - 1) ? n : (outSize - 1 - pfx);
  memmove(out + pfx, out, keep);
  memcpy(out, "OK: ", pfx);
  out[pfx + keep] = '\0';
}

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
  if (gAutomationLogActive && cmdCtx && cmdCtx->automationName[0]) {
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
  String actualCommand;  // populated only on the remote: / @ path below

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
    // Reject NESTED remote wrappers first. findCommand() doesn't understand the
    // remote:/@ prefix, so "remote:remote:X" / "remote:@X" would classify as
    // "no command" and slip past the privilege gate below — then bounce to the
    // bonded peer (which runs it as bond-super) and get re-forwarded back to us,
    // executing X with super privilege for an unprivileged local caller. There
    // is no legitimate nesting (target the final peer directly).
    {
      String inner = actualCommand; inner.trim();
      if (inner.startsWith("remote:") || inner.startsWith("remote ") || inner.startsWith("@")) {
        strncpy(out, "Error: nested remote commands are not allowed.", outSize - 1);
        out[outSize - 1] = '\0';
        return false;
      }
    }
    // The forwarded command runs on the bonded peer as the bond identity
    // (kBondAdminUser = super). Bonding treats the two devices as ONE unit, so
    // the bond session token IS the trust — the local caller's role is NOT
    // re-checked here (the slave typically has no logged-in user at all; the
    // owner logs into the master and drives the pair). The nested-wrapper reject
    // above is the only guard, and it stays: it stops a command from looping
    // back (origin→peer→origin) to run as super on the ORIGIN, which would let a
    // local non-super escape the master's own super tier.
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
    // Opcode MUST be ESPNOW_V4_TYPE_CMD. This was hardcoded to a literal 5
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
      // SECURITY: echo the REDACTED form. actualCommand is the unwrapped inner
      // command, so `@login bob hunter2` used to be printed verbatim here — and
      // broadcastOutput fans out to every sink including the MSG_ROUTE_FILE tee,
      // writing the password to unencrypted flash. This branch returns before
      // executeCommand's logCommandExecution calls, so it never passed through
      // any redaction at all; it has to redact for itself.
      String safeCommand = redactCmdForAudit(actualCommand);
      snprintf(out, outSize, "Remote command sent: %s", safeCommand.c_str());
      broadcastOutput("[REMOTE] Sent to bonded device: " + safeCommand);
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
  
  // Continue with local command execution. Non-remote commands never touch
  // actualCommand, so `command` is already the line to run — no copy-back.

  // Find command handler (findCommand does its own case-insensitive matching).
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
        if (gAutomationLogActive && cmdCtx && cmdCtx->automationName[0]) {
          char logBuf[201];
          snprintf(logBuf, sizeof(logBuf), "%.197s%s", out, strlen(out) > 197 ? "..." : "");
          for (char* c = logBuf; *c; c++) { if (*c == '\n' || *c == '\r') *c = ' '; }
          appendAutoLogEntry("OUTPUT", logBuf);
        }

        // Command audit logging (always-on)
        bool success = (strncmp(out, "Error", 5) != 0) && (strncmp(out, "ERROR", 5) != 0);
        logCommandExecution(ctx, cmd, success, out);
        stampOkStatus(out, outSize, success);  // stamp AFTER audit so the log keeps the raw result

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
    DEBUG_CMD_FLOWF("[registry_exec] executing: %s (args: %s)", found->name, args.c_str());
    DEBUGF(DEBUG_CLI, "[registry_exec] executing: %s (args: %s)", found->name, args.c_str());

    // Capture CLIMode state BEFORE the handler runs. If the handler enters
    // a mode (e.g. cmd_filedelete -> cliRequestConfirm) the command hasn't
    // actually completed -- it just prompted the user. The real completion
    // (and the right moment to audit) is when the user resolves the prompt
    // and the mode's onInput composes the audit line with full context.
    // See System_CLIConfirm::confirm_onInput for the resolution audit.
    bool modeWasActiveBeforeHandler = cliInModeActive();

    const char* result = found->handler(args);
    // Delivery ceiling — see CMD_RESULT_MAX (System_CommandTypes.h). Every
    // transport hands us a differently-sized `out`, and this was a silent
    // strncpy: an over-long result was torn in half and delivered looking like
    // real data. That is how `events kinds json` reached MQTT as a truncated
    // fragment and how the web mute editor got a bare "OK". Report it instead.
    // The "Error" prefix is deliberate — the success test below and
    // stampOkStatus already treat it as a failure, so this needs no other
    // plumbing and reaches every transport through the normal path.
    const size_t resultLen = result ? strlen(result) : 0;
    if (outSize == 0) {
      // No buffer to answer into; nothing safe to do.
    } else if (resultLen >= outSize) {
      snprintf(out, outSize,
               "Error: result too large for this transport (%u B, limit %u B)"
               " - narrow the query or use a dedicated endpoint",
               (unsigned)resultLen, (unsigned)(outSize - 1));
    } else {
      memcpy(out, result ? result : "", resultLen + 1);
    }

    // Command audit logging (always-on, EXCEPT when the handler just
    // entered an interactive mode -- skip the prompt step so the audit
    // log doesn't claim a destructive action completed when it only asked
    // for confirmation. The mode is responsible for auditing the
    // resolution if it wants to (confirm mode does; help mode doesn't).
    const bool handlerEnteredMode = !modeWasActiveBeforeHandler && cliInModeActive();
    if (!handlerEnteredMode) {
      bool success = (strncmp(out, "Error", 5) != 0) && (strncmp(out, "ERROR", 5) != 0);
      logCommandExecution(ctx, cmd, success, out);
      stampOkStatus(out, outSize, success);  // stamp AFTER audit so the log keeps the raw result
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
  if (gAutomationLogActive && cmdCtx && cmdCtx->automationName[0]) {
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
    // Match the queued path's capacity. The old comment claimed "2KB matches
    // ExecReq.out size" — it never did (ExecReq::out is CMD_RESULT_MAX), so an
    // early-boot command silently got half the budget of the same command run
    // a moment later. Transient PSRAM, freed below.
    char* outBuf = (char*)ps_alloc(CMD_RESULT_MAX, AllocPref::PreferPSRAM, "cmd.out.direct");
    if (!outBuf) {
      out = "Error: Out of memory for command output";
      return false;
    }
    // executeCommand installs the command identity via ExecIdentityGuard, so
    // the early-boot direct path no longer needs an outer save/restore. The
    // current command context pointer is still wired explicitly for the
    // broadcast-output mask plumbing — per-task TLS now (Stage 3).
    setCurrentCommandContext((void*)&cmd.ctx);
    bool ok = executeCommand((AuthContext&)cmd.ctx.auth, cmd.line.c_str(), outBuf, CMD_RESULT_MAX);
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
  uc.ctx.outputMask = MSG_ROUTE_FILE;
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
  uc.ctx.outputMask = MSG_ROUTE_WEB | MSG_ROUTE_FILE;
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
    return "Error: invalid arguments — Usage: login <username> <password> [transport]\nTransport: serial (default), display, bluetooth";
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
    return "Error: Invalid transport. Use: serial, display, or bluetooth";
  }

  // Attempt login
  if (loginTransport(transport, username, password)) {
    bool isAdmin = isAdminUser(username);
    systemEventPost(SYSEVT_LOGIN_OK, username.c_str(), transportStr.c_str());
    EXT_RAM_BSS_ATTR static char buf[128];
    snprintf(buf, sizeof(buf), "Login successful for '%s' on %s%s",
             username.c_str(), transportStr.c_str(), isAdmin ? " (admin)" : "");
    return buf;
  } else {
    systemEventPost(SYSEVT_LOGIN_FAIL, username.c_str(), transportStr.c_str());
    return "Error: Authentication failed";
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
      // `bleautoreconnect g2-glasses on` from any authenticated session.
      transport = SOURCE_G2_GLASSES;
    } else {
      return "Error: Invalid transport. Use: serial, display, bluetooth, or g2";
    }
  }

  logoutTransport(transport);
  EXT_RAM_BSS_ATTR static char buf[64];
  snprintf(buf, sizeof(buf), "Logged out from %s", rest.length() > 0 ? rest.c_str() : "serial");
  return buf;
}

// ============================================================================
// Input Abstraction Layer - MOVED TO Input_HAL.cpp
// ============================================================================
// Implementation is now in Input_HAL.cpp for better modularity.
