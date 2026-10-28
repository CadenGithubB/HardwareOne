/**
 * System Utilities - Shared functions used across modules
 * 
 * Common utilities and command execution shared by subsystems.
 * 
 * Command-module composition lives in System_CommandRegistry.cpp.
 */

#include <Arduino.h>
#include <atomic>
#include <ctype.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#if CONFIG_HEAP_TASK_TRACKING
#include <esp_heap_task_info.h>   // heap_task_totals_t, heap_caps_get_per_task_info
#endif
#include "System_BuildConfig.h"

// ---------------------------------------------------------------------------
// ESP-NOW-off stub
// ---------------------------------------------------------------------------
// isEspNowInitialized() is DEFINED in System_ESPNow.cpp, which CMake drops when
// ENABLE_ESPNOW is 0 — but three always-compiled TUs call it unconditionally:
// System_WiFi.cpp (radio-ownership decisions, via a local `extern` declaration),
// System_RamFlush.cpp (RF_ESPNOW resume) and WebPage_Sensors.cpp. So the
// ESPNOW=0 configuration has never actually linked; the flag existed but the
// build was never tried.
//
// Answering false here is correct, not merely convenient: with ESP-NOW compiled
// out nothing ever claims the radio for it, so every caller's "is ESP-NOW
// holding the radio / does it need resuming / is it active" question has the
// same true answer.
#if !ENABLE_ESPNOW
bool isEspNowInitialized() { return false; }
#endif
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
#include "System_CommandTypes.h"
#include "System_Debug.h"
#include "System_TaskUtils.h"
#include "System_I2C.h"
#include "System_User.h"
#include "System_UartLink.h"
#include "System_AuthIdentity.h"  // ExecIdentityGuard (executeCommand + submitAndExecuteSync)
#include "System_BootState.h"     // bootStateGetBootCount / bootStateResetBootCount (NVS boot counter)
#include "System_CrashRecord.h"   // crashRecord* — RTC post-mortem record for `crashlog`
#include "System_OTASafety.h"     // pending trial images must finish probation before power-off
#include "System_SelfDevice.h"   // SelfDevice:: — local identity/heap/uptime/firmware (Stage 1 consolidation)
#include "System_Clock.h"        // Clock:: — epoch/sync/tz/format helpers (Stage 2)
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
// Unconditional: G2_Glasses.h is self-guarding (it supplies inline no-op stubs
// when BT/G2 are compiled out), and this TU calls into it from ungated code
// (e.g. the EvenAI authority fence in the command executor). Gating the include
// on a bare ENABLE_G2_GLASSES left those calls undeclared when G2 was off.
#include "G2_Glasses.h"         // isG2ClientInitialized(), isG2Connected()
#if ENABLE_LLM_BACKEND
#include "System_LLMBackend.h"         // LLMStatus, llmBackendStatus()
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
#include "HAL_Input.h"
#include "System_Mutex.h"   // For FsLockGuard
#include "System_I2C.h"     // For I2CSensorEntry, ConnectedDevice, MAX_CONNECTED_DEVICES
#include "System_MemUtil.h"       // For AllocPref
#include "System_MemTracker.h"

// Extern declarations for logging functions (implemented in .ino)
extern bool appendLineWithCap(const char* path, const String& line, size_t capBytes);

// Runtime subsystem interfaces used by the utilities and executor below
#include "System_Filesystem.h"
#include "System_Debug.h"
#include "System_CLI.h"
#include "System_CLIMode.h"
#include "System_BuildConfig.h"   // Conditional sensor configuration - must be early
#if ENABLE_WIFI
  #include "System_WiFi.h"
#endif
#include "OLED_Display.h"
#include "System_Automation.h"
#include "System_I2C.h"
#if ENABLE_ESPNOW
  #include "System_ESPNow.h"
  #include "System_ESPNow_Wire.h"   // ESPNOW_V4_TYPE_* / ESPNOW_V4_FLAG_* opcode + flag enums
#endif
#if ENABLE_BLUETOOTH
  #include "Bluetooth.h"
  #include "BLE_PhonePipe.h"
  #include "BLE_LinkStatus.h"  // status json: role, relay pipe, app console
  #include "BLE_Peers.h"
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
#include "System_RadioCompanion.h"  // companionCommands, sleep hooks, status (P4X-EYE); no-ops elsewhere
#if ENABLE_APDS_SENSOR
  #include "i2csensor_apds9960.h"     // For apdsCommands
#endif
#include "System_SensorStubs.h" // Stubs for disabled sensors
#if ENABLE_GPS_SENSOR
  #include "i2csensor_pa1010d.h"      // For gpsCommands
#endif

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
// FTS uses timeout=0 so a new owner has time to read instructions. Blocking
// wizard callers may opt into an idle timeout. The interactive CLIMode uses
// its own session/inactivity policy and leaves this timer disabled.
// Wizard input loops refresh activity and poll cancellation, including local
// display loops that do not call the blocking serial reader.
static volatile unsigned long sSerialWaitTimeoutMs = 0;
static volatile unsigned long sSerialWaitLastActivityMs = 0;
static volatile bool sWizardCancelRequested = false;

void setSerialWaitTimeout(unsigned long timeoutMs) {
  sSerialWaitTimeoutMs = timeoutMs;
  sSerialWaitLastActivityMs = millis();
  sWizardCancelRequested = false;
}

void requestWizardCancel() { sWizardCancelRequested = true; }
void noteWizardInputActivity() { sSerialWaitLastActivityMs = millis(); }

bool isWizardCancelRequested() {
  if (sSerialWaitTimeoutMs > 0 &&
      (millis() - sSerialWaitLastActivityMs) > sSerialWaitTimeoutMs) {
    sWizardCancelRequested = true;
  }
  return sWizardCancelRequested;
}

String waitForSerialInputBlocking() {
  for (;;) {
    if (isWizardCancelRequested()) return String();
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

bool readText(const char* path, String& out, const AuthContext& ctx) {
  out = "";

  // Pause sensor polling during file I/O to prevent I2C contention (RAII —
  // resumes on every return path; from System_PollPause.h via System_I2C.h).
  PollPauseGuard pollGuard;

  FsLockGuard guard("readText");
  File f = VFS::openGuarded(String(path), "r", ctx);
  if (!f) {
    return false;
  }
  out = f.readString();
  f.close();

  return true;
}

bool writeText(const char* path, const String& in, const AuthContext& ctx) {
  if (!path || !*path) return false;
  String destination;
  if (!destination.concat(path)) return false;

  PollPauseGuard pollGuard;   // pause sensor polling during file I/O (RAII)

  FsLockGuard guard("writeText");
  File f = VFS::openGuarded(destination, "w", ctx, true);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }
  const size_t written = f.print(in);
  f.flush();
  // flush() has no result in Arduino File. Check every completion signal the
  // API exposes before a caller may promote this file over its live state.
  const bool complete = written == in.length() && !f.getWriteError() &&
                        f.size() == in.length();
  f.close();

  return complete;
}

bool writeTextAtomic(const char* path, const String& content, const AuthContext& ctx) {
  if (!path || !*path) return false;
  String destination;
  String tmp;
  if (!destination.concat(path) || !tmp.concat(destination) || !tmp.concat(".tmp")) {
    return false;  // No I/O until both complete paths are allocated.
  }

  // The fixed sibling temp belongs to this whole operation. Keep competing
  // saves out until promotion or cleanup finishes; both guards allow nesting.
  PollPauseGuard pollGuard;
  FsLockGuard guard("writeTextAtomic");

  if (writeText(tmp.c_str(), content, ctx) && VFS::renameGuarded(tmp, destination, ctx)) {
    return true;
  }

  // Failed write or rename: discard only the temp, never truncate or delete
  // the destination. Cleanup is best-effort and cannot turn failure into success.
  VFS::removeGuarded(tmp, ctx);
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
// (g2status, ring status, blestatus are each fetched every 1.5-15 s by
// the Bluetooth panel), and the audit file would grow without bound
// for traffic that has zero security relevance (pure reads, no state
// change). Keeping the list deliberately tight: only commands that
// (a) cannot mutate state and (b) are called automatically by the UI
// belong here. User-typed `g2status` from a CLI prompt also passes
// through this path and gets suppressed too — that's fine; it's
// visible in the prompt response anyway.
// Display clients poll and acknowledge accepted text automatically. These
// passive exchanges must neither wake the device nor fill the audit log.
static bool isPassiveTranscriptionCommand(const char* cmd) {
  if (!cmd) return false;
  static const char* const verbs[] = {
    "transcription status", "transcription next", "transcription ack"
  };
  for (const char* verb : verbs) {
    const size_t n = strlen(verb);
    if (!strncmp(cmd, verb, n) &&
        (!cmd[n] || cmd[n] == ' ' || cmd[n] == '\t')) return true;
  }
  return false;
}

static bool isQuietPollCommand(const char* cmd) {
  if (isPassiveTranscriptionCommand(cmd)) return true;
  if (!cmd || !cmd[0]) return false;
  // Match the leading verb; ignore any trailing args (json modifiers, etc.)
  static const char* const kQuiet[] = {
    "g2status", "ring status", "blestatus",
    "cm5 status", "cm5 capabilities",
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
static bool commandExecutionLogSuppressed(const char* cmd) {
  // Skip validation-only commands (dry-run checks) of this task's command
  if (currentCommandValidateOnly()) return true;

  // Skip the audit log + broadcast for noisy read-only polls.
  return isQuietPollCommand(cmd);
}

static void logCommandExecutionPrepared(const AuthContext& ctx,
                                        const char* originalCmd,
                                        const String& redactedCmd,
                                        bool success,
                                        const char* result) {
  if (commandExecutionLogSuppressed(originalCmd)) return;

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
    case SOURCE_UART: source = "uart"; break;
    default: source = "unknown"; break;
  }
  
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

void logCommandExecution(const AuthContext& ctx, const char* cmd, bool success,
                         const char* result) {
  // Preserve the old cheap exit: quiet/validation-only commands never pay for
  // command recognition or redaction.
  if (commandExecutionLogSuppressed(cmd)) return;
  const String command = cmd ? String(cmd) : String();
  const CommandResolution resolution = resolveCommand(command);
  const String redactedCmd = redactCmdForAudit(command, resolution);
  logCommandExecutionPrepared(ctx, cmd, redactedCmd, success, result);
}

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
      while (i < len && isspace(static_cast<unsigned char>(s[i]))) i++;
      if (i >= len) return false;
      start = i;
      if (s[i] == '"') {
        int close = s.indexOf('"', i + 1);
        i = (close < 0) ? len : close + 1;
      } else {
        while (i < len && !isspace(static_cast<unsigned char>(s[i]))) i++;
      }
      end = i;
    }
    return true;
  }

  // True when tokenization up to `uptoToken` cannot be trusted to put the secret
  // where the rule says it is. EVERY token up to the masked one is checked, not
  // just the masked one: the split that shifts the secret happens EARLIER in the
  // line. In `wifiadd "My"Net" pass123` token 3 ends at a space quite happily; it
  // is token 2 (`"My"` butted straight against `Net`) that did the damage.
  static bool redactTokensAmbiguous(const String& c, int uptoToken) {
    CommandArgs parsed(c);
    if (parsed.unterminatedQuote()) return true;
    for (int t = 1; t <= uptoToken; ++t) {
      int ts = 0, te = 0;
      if (!nthTokenBounds(c, t, ts, te)) return true;
      // must be followed by whitespace or end-of-line
      if (te < (int)c.length() &&
          !isspace(static_cast<unsigned char>(c[te]))) return true;
      // a bare token may contain no quote at all; a quoted token may contain
      // exactly its two delimiters and nothing else
      const bool isQuoted = (c[ts] == '"');
      for (int k = ts; k < te; ++k) {
        if (c[k] != '"') continue;
        if (isQuoted && (k == ts || k == te - 1)) continue;
        return true;
      }
    }
    return false;
  }

  // Ambiguous tokenization => keep the verb only and mask the whole tail. A rule
  // that matched a credential command must NEVER fall back to returning the raw
  // line; "we could not find the secret" and "there is no secret" are different.
  static String redactFailClosed(const String& c) {
    int vs = 0, ve = 0;
    if (nthTokenBounds(c, 1, vs, ve)) return c.substring(0, ve) + " ***";
    return String("***");
  }

  // Shared handler for peer-credential commands: "<cmd> <target> <username>
  // <password> <rest>..." (espnowremote / roomcmd / tagcmd / browse / fetch).
  // Mask the peer password. For command-forwarding forms, recursively redact
  // the nested command too: it may itself be `login ...` or another credential
  // verb.
  static String redactPeerCredCmd(const String& in) {
    String c = in;
    CommandArgs parsed(c);
    String safe;
    for (int i = 0; i < 3 && i < parsed.count(); ++i) {
      if (i) safe += " ";
      safe += parsed.arg(i);
    }
    safe += " ***";

    const bool forwarding = parsed.count() > 0 &&
        (parsed.arg(0).equalsIgnoreCase("espnowremote") ||
         parsed.arg(0).equalsIgnoreCase("espnowroomcmd") ||
         parsed.arg(0).equalsIgnoreCase("espnowtagcmd"));
    // Preserve a nested command only when tokenization is unambiguous. Any
    // quote adjacency/unterminated form is treated as secret tail in full.
    int passwordStart = 0, passwordEnd = 0;
    if (!forwarding || parsed.unterminatedQuote() ||
        !nthTokenBounds(c, 4, passwordStart, passwordEnd)) {
      return safe;
    }
    if (passwordEnd < c.length() &&
        !isspace(static_cast<unsigned char>(c[passwordEnd]))) {
      return safe;
    }
    String tail = c.substring(passwordEnd);
    tail.trim();
    if (tail.length()) safe += String(" ") + redactCmdForAudit(tail);
    return safe;
  }

  // usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>
  // Mask the THREE password tokens (positions 3, 6, 7); keep username(2),
  // device(4), and targetAdminUser(5) visible for the audit trail.
  static String redactUserSyncCmd(const String& in) {
    String c = in;
    const int kPwTokens[] = { 7, 6, 3 };  // high → low so edits don't shift earlier tokens
    // CALL_HANDLER rules run BEFORE the generic dispatch guard, so this needs its
    // own. It also used to `continue` when a token could not be bounded — leaving
    // that password in the clear. Three secrets on one line: if the positions are
    // not trustworthy, none of them are.
    if (redactTokensAmbiguous(c, kPwTokens[0])) return redactFailClosed(c);
    for (int k = 0; k < 3; k++) {
      int pos = kPwTokens[k];
      int start = 0, end = 0;
      if (!nthTokenBounds(c, pos, start, end)) continue;  // line simply ends early
      c = c.substring(0, start) + "***" + c.substring(end);
    }
    return c;
  }

  // `dictate result` carries user-authored field text and `dictate fail`
  // carries a potentially sensitive host diagnosis. Keep the operation and a
  // syntactically valid correlation ID useful to the audit trail, but never
  // retain the free-form tail. A malformed ID is masked too so invalid command
  // attempts cannot repurpose the nominal ID position as an unredacted field.
  static String redactDictateCmd(const String& in) {
    CommandArgs parsed(in);
    String safe = "dictate";
    if (parsed.count() > 1) {
      safe += " ";
      safe += parsed.arg(1);
    }
    if (parsed.count() > 2) {
      const String& id = parsed.arg(2);
      bool validId = id.length() == 16;
      for (size_t i = 0; validId && i < id.length(); ++i) {
        validId = isxdigit(static_cast<unsigned char>(id[i]));
      }
      if (validId) {
        safe += " ";
        safe += id;
      }
    }
    safe += " ***";
    return safe;
  }

  // `phone pair new` (phone link v2) takes no argument and its code is never on
  // the line; the audit line, the [CMD] echo and the feeds say so all the same.
  static String redactPhonePairNew(const String&) {
    return String("phone pair new (code redacted)");
  }

  static bool redactRuleMatches(const String& command,
                                const char* rulePrefix) {
    CommandArgs actual(command);
    String expectedLine(rulePrefix ? rulePrefix : "");
    expectedLine.trim();
    CommandArgs expected(expectedLine);
    if (expected.count() == 0 || actual.count() < expected.count())
      return false;
    for (int i = 0; i < expected.count(); ++i) {
      if (!actual.arg(i).equalsIgnoreCase(expected.arg(i))) return false;
    }
    return true;
  }

  // Rule table (extend here to add new redactions)
  // Columns: prefix (lowercase cmd prefix), type (MASK_TOKEN_AT_POS|MASK_AFTER_TOKEN_POS|CALL_HANDLER), param (1-based token index), handler (custom fn or nullptr)
  static const RedactRule kRules[] = {
    { "wifiadd ",          MASK_TOKEN_AT_POS,    3, nullptr },  // wifiadd <ssid> <password>
    { "llmhttptoken ",     MASK_AFTER_TOKEN_POS, 1, nullptr },  // gateway credential
    { "llmhttpurl ",       MASK_AFTER_TOKEN_POS, 1, nullptr },  // redact even a rejected URL containing userinfo
    { "mqttpassword ",     MASK_AFTER_TOKEN_POS, 1, nullptr },  // mqttpassword <password>
    { "login ",            MASK_TOKEN_AT_POS,    3, nullptr },  // login <user> <password>
    { "testencryption ",   MASK_AFTER_TOKEN_POS, 1, nullptr },  // testencryption <secret>
    { "testpassword ",     MASK_AFTER_TOKEN_POS, 1, nullptr },  // testpassword <secret>
    { "otapin ",           MASK_AFTER_TOKEN_POS, 1, nullptr },  // otapin <recovery passphrase>
    { "userrequest ",      MASK_AFTER_TOKEN_POS, 2, nullptr },  // userrequest <name> <pass> ...
    { "espnowremote ",     CALL_HANDLER,         0, &redactPeerCredCmd },  // <target> <user> <pass> <cmd>
    { "espnowroomcmd ",    CALL_HANDLER,         0, &redactPeerCredCmd },  // <room> <user> <pass> <cmd>
    { "espnowtagcmd ",     CALL_HANDLER,         0, &redactPeerCredCmd },  // <tag> <user> <pass> <cmd>
    { "espnowbrowse ",     CALL_HANDLER,         0, &redactPeerCredCmd },  // <target> <user> <pass> [path]
    { "espnowfetch ",      CALL_HANDLER,         0, &redactPeerCredCmd },  // <target> <user> <pass> <path>
    { "phone secret ",     MASK_AFTER_TOKEN_POS, 2, nullptr },  // phone secret <passphrase>
    { "phone pair new",    CALL_HANDLER,         0, &redactPhonePairNew },  // its code is shown once, never logged
    { "blesecret ",        MASK_AFTER_TOKEN_POS, 1, nullptr },  // blesecret <passphrase>
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
    { "dictate result ",     CALL_HANDLER,         0, &redactDictateCmd },  // preserve exact id; mask transcript
    { "dictate fail ",       CALL_HANDLER,         0, &redactDictateCmd },  // preserve exact id; mask reason
    // Automation definitions may embed arbitrary future command lines and are
    // copied into several durable/debug sinks before execution. Preserve only
    // the wrapper identity; the nested payload is opaque secret material here.
    { "automation add ",     MASK_AFTER_TOKEN_POS, 2, nullptr },
    { "automationadd ",      MASK_AFTER_TOKEN_POS, 1, nullptr },
    { "validate-conditions ", MASK_AFTER_TOKEN_POS, 1, nullptr },
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

static bool macHexDigit(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}
// A MAC's neighbour: more hex or another separator means a longer token.
static bool macGlued(char ch) { return macHexDigit(ch) || ch == ':' || ch == '-'; }

String maskMacTokens(const String& text) {
  const char* s = text.c_str();
  const size_t n = text.length();
  String out;
  out.reserve(n);
  size_t i = 0;
  while (i < n) {
    // Six pairs and five separators of one kind, not glued to more hex.
    bool mac = i + 17 <= n && (i == 0 || !macGlued(s[i - 1])) &&
               (s[i + 2] == ':' || s[i + 2] == '-');
    for (size_t k = 0; mac && k < 17; ++k) {
      mac = (k % 3 == 2) ? s[i + k] == s[i + 2] : macHexDigit(s[i + k]);
    }
    if (mac && i + 17 < n && macGlued(s[i + 17])) mac = false;
    if (mac) {
      out += "<mac>";
      i += 17;
    } else {
      out += s[i++];
    }
  }
  return out;
}

String redactCmdForAudit(const String& argsInput) {
  const CommandResolution resolution = resolveCommand(argsInput);
  return redactCmdForAudit(argsInput, resolution);
}

String redactCmdForAudit(const String& argsInput,
                         const CommandResolution& resolution) {
  // Addresses first: masking one keeps it a single token, so the rules below
  // still find their positions.
  String c = maskMacTokens(argsInput);
  String cl = c; cl.toLowerCase();

  // Credential verbs accept the command registry's full whitespace grammar.
  // Mask login's password before the legacy literal-prefix rule table so a
  // tab/CR/VT/FF separator cannot bypass redaction.
  int verbStart = 0;
  while (verbStart < cl.length() &&
         isspace(static_cast<unsigned char>(cl[verbStart]))) verbStart++;
  int verbEnd = verbStart;
  while (verbEnd < cl.length() &&
         !isspace(static_cast<unsigned char>(cl[verbEnd]))) verbEnd++;
  if (cl.substring(verbStart, verbEnd) == "login") {
    CommandArgs loginArgs(c);
    // Login grammar has no escaping. Treat malformed or surprising input as
    // entirely secret after the username instead of trying to preserve an
    // ambiguous tail (e.g. `login bob "pa"ss word`). For an exact targeted
    // form retain only the non-secret target label.
    String safe = "login";
    if (loginArgs.count() > 1) {
      safe += " ";
      safe += loginArgs.arg(1);
    }
    safe += " ***";
    if (!loginArgs.unterminatedQuote() && loginArgs.count() == 4) {
      safe += " ";
      safe += loginArgs.arg(3);
    }
    return safe;
  }

  for (size_t i = 0; i < (sizeof(kRules) / sizeof(kRules[0])); ++i) {
    const RedactRule& r = kRules[i];
    if (!redactRuleMatches(c, r.prefix)) continue;

    if (r.type == CALL_HANDLER && r.handler) {
      return r.handler(c);
    }

    // Compute token boundaries QUOTE-AWARE (nthTokenBounds mirrors
    // CommandArgs) so a quoted argument containing spaces can't shift the
    // positions. Token positions are 1-based over the entire line
    // (including the command words).
    // FAIL CLOSED on any tokenization ambiguity. Both branches below used to
    // `return c` -- the RAW line -- whenever nthTokenBounds failed, so
    // `wifiadd "My Net pass123` (unterminated quote) consumed the rest of the
    // line as one token and logged the password verbatim.
    //
    // And a quote INSIDE a value splits it in two, shifting the secret PAST the
    // masked index: `wifiadd "My"Net" pass123` tokenizes as
    // wifiadd / My / Net" / pass123, so masking token 3 hit an SSID fragment and
    // left the PSK in the clear. The parser and nthTokenBounds agree here -- it
    // is the RULE's "the secret is always token N" assumption that breaks.
    //
    // Same three-part guard redactPeerCredCmd already applies above.
    if (r.type == MASK_TOKEN_AT_POS || r.type == MASK_AFTER_TOKEN_POS) {
      int start = 0, end = 0;
      if (redactTokensAmbiguous(c, (int)r.param) ||
          !nthTokenBounds(c, r.param, start, end)) {
        return redactFailClosed(c);
      }
      if (r.type == MASK_TOKEN_AT_POS) {
        return c.substring(0, start) + "***" + c.substring(end);
      }
      return c.substring(0, end) + " ***";
    }
  }

  // No rule matched. If the VERB itself isn't a recognized command, mask
  // everything after it: a typo'd credential command ("logni <user> <pass>")
  // otherwise lands verbatim in the audit log — the rule table can never
  // enumerate misspellings, but "unknown verb ⇒ args are opaque" catches
  // them all. Recognized commands keep full args (that's the audit value);
  // an unknown verb's args have none.
  if (!resolution.entry) {
    if (verbEnd > verbStart && verbEnd < c.length()) {
      return c.substring(0, verbEnd) + " ***";
    }
  }

  return c;
}

// Redact sensitive data from command outputs (JSON responses, etc.)
String redactOutputForLog(const String& output) {
  // Private STT results are tagged by the producer. Replace the WHOLE reply
  // rather than parsing quoted/escaped text or risking a partial redaction.
  if (output.indexOf("\"sttText\"") >= 0 ||
      output.indexOf("\"transcriptPath\"") >= 0) return "[private STT result]";
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

#include "System_Power.h"  // shared target CPU clock choices

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
    broadcastOutput("  Supported interactive frequencies:");
    for (size_t i = 0; i < getPowerCpuFrequencyCount(); ++i) {
      BROADCAST_PRINTF("    %lu MHz", (unsigned long)getPowerCpuFrequencyMhz(i));
    }
    return "[System] CPU frequency displayed";
  } else {
    // Set frequency (admin only for safety)

    for (size_t i = 0; i < args.length(); ++i) {
      if (args[i] < '0' || args[i] > '9') {
        return "Error: Enter one CPU frequency in MHz; run cpufreq for supported values";
      }
    }
    uint32_t newFreq = args.toInt();
    if (!isPowerCpuFrequencySupported(newFreq)) {
      return "Error: Unsupported CPU frequency; run cpufreq for supported values";
    }
    if (!powerSetCpuFrequencyMhz(newFreq)) {
      return "Error: CPU frequency change failed";
    }
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
  if (otaSafetyIsPendingVerification()) {
    return "Error: Light sleep refused while the new firmware is in OTA health probation";
  }

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

  // The radio companion follows the main processor: its heartbeat watchdog
  // pauses so the silence of sleep is not mistaken for an outage.
  radioCompanionBeforeLightSleep();
  // Enter light sleep (preserves RAM, resumes here when woken)
  esp_light_sleep_start();
  radioCompanionAfterLightSleep();

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
  if (otaSafetyIsPendingVerification()) {
    return "Error: Deep sleep refused while the new firmware is in OTA health probation";
  }

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
  // The radio companion follows: stop what uses it and hold the C6 in reset
  // through deep sleep (no-op with an on-chip radio).
  radioCompanionBeforeDeepSleep();
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
// Declared in System_Utils.h for CLI, HTTP, SSE and migration callers,
// independently of whether the HTTP server is compiled in.
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
  mem["heap_total_kb"] = (int)(hw1InternalTotalBytes() / 1024);
  mem["psram_total_kb"] = (int)(ESP.getPsramSize() / 1024);
  mem["psram_free_kb"] = (int)(SelfDevice::psramFreeBytes() / 1024);

  // Storage info (nested object with KB values). This builder feeds periodic
  // web/SSE telemetry as well as explicit status responses, so repeated calls
  // use the presentation-only capacity snapshot. Admission paths (uploads,
  // image saves, sensor logging) continue to call fresh VFS::getStats().
  static constexpr uint32_t kSystemInfoStatsMaxAgeMs = 5000;
  JsonObject storage = doc["storage"].to<JsonObject>();
  {
    VFS::CapacitySnapshot stats = {};
    (void)VFS::getStatsSnapshot(VFS::INTERNAL, stats,
                                kSystemInfoStatsMaxAgeMs);
    storage["total_kb"] = (int)(stats.totalBytes / 1024);
    storage["used_kb"] = (int)(stats.usedBytes / 1024);
    storage["free_kb"] = (int)(stats.freeBytes / 1024);
  }
  if (VFS::isSDAvailable()) {
    VFS::CapacitySnapshot stats = {};
    if (VFS::getStatsSnapshot(VFS::SDCARD, stats,
                              kSystemInfoStatsMaxAgeMs)) {
      JsonObject sd = storage["sd"].to<JsonObject>();
      sd["total_mb"] = (int)(stats.totalBytes / (1024 * 1024));
      sd["used_mb"] = (int)(stats.usedBytes / (1024 * 1024));
      sd["free_mb"] = (int)(stats.freeBytes / (1024 * 1024));
    }
  }

#if HW1_RADIO_COMPANION
  // ESP32-C6 radio companion (P4X-EYE): state, firmware, bridge, heartbeat.
  radioCompanionJson(doc["companion"].to<JsonObject>());
#endif

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
    bt["mode"]    = getBleModeString();   // "server" | "client" (the saved boot role)
    bt["server"]  = isBleServerInitialized();
#if ENABLE_G2_GLASSES
    bt["client"]    = isG2ClientInitialized();
    bt["g2Connected"] = isG2Connected();
#else
    bt["client"]    = false;
    bt["g2Connected"] = false;
#endif
    // One link reading: the role running now (+ the saved mode), the relay
    // pipe and the app console in the words every surface shows
    // (BLE_LinkStatus.h). None of its keys collide with the ones above.
    const BleLinkStatus link = bleLinkStatus();
#if ENABLE_BLE_PHONE_PIPE
    // Android phone relay pipe: "ready", "fail: not found" (the lens/OLED words).
    char pipeBuf[24];  // copied into the document
    bt["phoneConnected"] = link.pipe.phase == hw1link::Pipe::Ready;
    bt["phoneState"]     = hw1link::pipeLine(link.pipe, pipeBuf, sizeof(pipeBuf));
    // The phone link block, compact (no texts, no end-reason counts).
    bleLinkPhoneJson(bt["phoneLink"].to<JsonObject>(), link, false);
#endif
    bleLinkStatusJson(bt, link);
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

#if ENABLE_LLM_BACKEND
  {
    LLMStatus llmSt = llmBackendStatus();
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
    // A P4 build (C6 companion, LLM, the Bluetooth link words) reaches about
    // 2 KB; a document that does not fit is refused, never cut into bad JSON.
    static constexpr size_t kStatusJsonCap = 3072;
    PSRAM_JSON_DOC(doc);
    buildSystemInfoJson(doc, /*includeDeviceList=*/false);
    if (doc.overflowed()) return "{\"error\":\"oom\"}";
    if (measureJson(doc) >= kStatusJsonCap) return "{\"error\":\"status too large\"}";
    static char* statusJsonBuf = nullptr;
    if (!statusJsonBuf) {
      statusJsonBuf = (char*)ps_alloc(kStatusJsonCap, AllocPref::PreferPSRAM, "status.json");
    }
    if (!statusJsonBuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, statusJsonBuf, kStatusJsonCap);
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

  uint64_t total = 0, used = 0, free = 0;
  if (!VFS::getStats(VFS::INTERNAL, total, used, free)) {
    return wantJson ? "{\"schema\":1,\"ready\":false}" : "Error: filesystem stats unavailable";
  }
  size_t totalBytes = (size_t)total;
  size_t usedBytes = (size_t)used;
  size_t freeBytes = (size_t)free;
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

  broadcastOutput("AES String Encryption Test (secret material hidden):");
  BROADCAST_PRINTF("Input length: %u", (unsigned)args.length());
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

  broadcastOutput("Password Hashing Test (secret material hidden):");
  BROADCAST_PRINTF("Input length: %u", (unsigned)args.length());
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

static bool finalizeMicRecordingForRestart(const char* reason) {
  if (!micRecordingBusy()) return true;
  WARN_SYSTEMF("[REBOOT] stopping %s microphone recording before %s",
               micRecordingStateName(getMicRecordingState()),
               reason ? reason : "restart");
  if (stopRecording(3000)) return true;
  WARN_SYSTEMF("[REBOOT] recorder still %s after bounded finalization wait",
               micRecordingStateName(getMicRecordingState()));
  return false;
}

void rebootDevice(const char* reason, const char* auditDetail, uint32_t flushDelayMs) {
  // Normal reboot/OTA/ramflush paths must not cut power while a WAV still owns
  // its placeholder header or is in FINALIZING. This is deliberately bounded:
  // a broken recorder must not make an intentional recovery reboot impossible.
  finalizeMicRecordingForRestart(reason);
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
// factoryreset -- wipe the accounts and their data + reboot to first-time setup
// ============================================================================
//
// Phase 4 of the CLIMode framework rollout: a real user-facing destructive
// command that runs through the same yes/no confirm flow as filedelete and
// userdelete (Phase 3). What it does:
//
//   1. Prompt: lists what is deleted; settings (WiFi, debug flags) are
//      PRESERVED.
//   2. If user says yes, under one filesystem lock and as VFS::systemAuth
//      (admin can't delete /system/* under normal permissions -- only the
//      internal "system" auth has PERM_DELETE in the path table):
//      a. Record the roster's issued account ids in the id high-water mark
//         (System_User), so later accounts never reuse them.
//      b. Delete pending_users.json and every account folder (u<id>) under
//         /stt and, when the card is mounted, /sd/stt. A failure here stops
//         the reset with the accounts untouched.
//      c. Delete /system/users/users.json (the commit), then every file in
//         /system/users/user_settings/.
//      d. Schedule a reboot 1 second in the future via esp_timer.
//      e. Return "Factory reset complete. ... Rebooting in 1 second..."
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

// Per-account data factoryreset removes besides users.json. All of it is keyed
// by account id, and the transcript and note views authorize by id alone.
static constexpr const char* kFactoryResetPendingUsers = "/system/users/pending_users.json";
static constexpr const char* kFactoryResetUserSettingsDir = "/system/users/user_settings";
static constexpr uint8_t kFactoryResetMaxDepth = 8;

// "u<digits>": one account's folder under /stt or /sd/stt.
static bool factoryResetIsAccountFolder(const VFS::DirectoryEntry& entry) {
  if (!entry.isFolder || entry.name.length() < 2 || entry.name[0] != 'u') return false;
  for (size_t i = 1; i < entry.name.length(); ++i) {
    if (entry.name[i] < '0' || entry.name[i] > '9') return false;
  }
  return true;
}

// Removes the entries of dir that select accepts (all of them when select is
// null), folders with everything below them; dir itself stays. The directory
// is re-read after each removal, so no iterator is used across a removal. A
// missing dir counts as empty. On failure failedPath names what was left.
// The caller holds the filesystem lock.
static bool factoryResetClearDirectory(const String& dir, bool (*select)(const VFS::DirectoryEntry&),
                                       const AuthContext& ctx, uint8_t depth, String& failedPath) {
  if (depth > kFactoryResetMaxDepth) {
    failedPath = dir;
    return false;
  }
  String previous;
  for (;;) {
    VFS::DirectoryEntry victim;
    bool found = false;
    {
      File directory = VFS::openGuarded(dir, "r", ctx);
      if (!directory) {
        if (!VFS::existsGuarded(dir, ctx)) return true;
        failedPath = dir;
        return false;
      }
      if (!directory.isDirectory()) {
        directory.close();
        failedPath = dir;
        return false;
      }
      VFS::DirectoryReader reader(directory, dir);
      VFS::DirectoryEntry entry;
      while (reader.next(entry)) {
        if (entry.isVirtual || (select && !select(entry))) continue;
        victim = entry;
        found = true;
        break;
      }
      directory.close();
    }
    if (!found) return true;
    // A removal that reported success but left the entry would loop forever.
    if (victim.path == previous) {
      failedPath = victim.path;
      return false;
    }
    previous = victim.path;
    if (victim.isFolder) {
      if (!factoryResetClearDirectory(victim.path, nullptr, ctx, depth + 1, failedPath)) return false;
      if (!VFS::rmdirGuarded(victim.path, ctx)) {
        failedPath = victim.path;
        return false;
      }
    } else if (!VFS::removeGuarded(victim.path, ctx)) {
      failedPath = victim.path;
      return false;
    }
  }
}

static const char* factoryreset_confirmed(void* /*userData*/) {
  EXT_RAM_BSS_ATTR static char respBuf[320];

  // Factory reset uses a deferred timer rather than rebootDevice(), so drain
  // the recorder before committing the destructive delete/scheduled restart.
  if (!finalizeMicRecordingForRestart("factory reset")) {
    return "Error: factory reset aborted — microphone recording did not finalize";
  }

  {
#if ENABLE_BLUETOOTH
    // The BLE role is held from the pipe's stop through the secret's clear, as
    // `phone stop` holds it: no link start (boot, the radio companion, stuck-
    // phone recovery) may derive its key from the secret being cleared. It is
    // this task's (re-entrant).
    struct PhoneStopClaim {
      const bool held=bleRoleTransitionBegin(BleRoleTransition::PHONE_STOP);
      ~PhoneStopClaim(){if(held)bleRoleTransitionEnd();}
    } role;
    if(!role.held)return "Error: factory reset stopped; BLE role transition busy";
    // Revoke the pipe before deleting account authority. A failed durable clear
    // stops reset while the roster still exists; unrelated bond policy is unchanged.
    if(!phonePipeStop())return "Error: factory reset stopped; phone pipe cleanup incomplete";
    if(!phonePipeInvalidateSecret())return "Error: factory reset stopped; pipe key invalidation busy";
#endif
    if(!setInternalSecretSetting(gSettings.phonePipeSecret,String("")))
      return "Error: factory reset stopped; pipe secret could not be cleared";
    gSettings.phonePipeSecretGenerated=false;  // no device code is left to offer the console
  }
#if ENABLE_BLUETOOTH
  if(!blePeerClearPhonePipeEnrollment())return "Error: factory reset stopped; pipe enrollment cleanup busy";
#endif
  // Persist even when Bluetooth is compiled out: a later firmware must not
  // recover a pipe credential from the retained settings file after reset.
  if(!writeSettingsJsonForFactoryReset())return "Error: factory reset stopped; pipe secret clear was not persisted";

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

  // trusted, deliberately unconfined: a factory reset wipes every tree.
  AuthContext sysCtx = VFS::systemAuth("factory.reset");
  bool sdSkipped = false;
  String settingsLeft;
  {
    // One lock over the whole wipe: no account is created or requested
    // between recording the ids and removing the roster.
    FsLockGuard guard("factory.reset");

    // The ids users.json has issued stay reserved after it is gone.
    String idError;
    if (!recordAccountIdHighWater(idError)) {
      logSystemEvent("SETUP", "factory reset — account id mark not updated (%s)", idError.c_str());
    }

    // Data that is useless without its account goes first. A failure here
    // stops the reset while every account still exists.
    String failed;
    if (VFS::existsGuarded(kFactoryResetPendingUsers, sysCtx) &&
        !VFS::removeGuarded(kFactoryResetPendingUsers, sysCtx)) {
      failed = kFactoryResetPendingUsers;
    }
    if (failed.length() == 0) {
      (void)factoryResetClearDirectory("/stt", factoryResetIsAccountFolder, sysCtx, 0, failed);
    }
    if (failed.length() == 0) {
      if (VFS::isSDAvailable()) {
        (void)factoryResetClearDirectory("/sd/stt", factoryResetIsAccountFolder, sysCtx, 0, failed);
      } else {
        sdSkipped = true;
      }
    }
    if (failed.length() != 0) {
      snprintf(respBuf, sizeof(respBuf),
               "Error: factory reset stopped -- could not remove %s. Accounts are unchanged; no reboot.",
               failed.c_str());
      return respBuf;
    }

    if (!VFS::removeGuarded(USERS_JSON_FILE, sysCtx)) {
      snprintf(respBuf, sizeof(respBuf),
               "Error: factory reset failed -- could not delete %s",
               USERS_JSON_FILE);
      return respBuf;
    }

    // users.json is gone, so no account references these credentials any
    // more; one left behind is overwritten whole when its id is next written.
    if (!factoryResetClearDirectory(kFactoryResetUserSettingsDir, nullptr, sysCtx, 0, settingsLeft)) {
      logSystemEvent("SETUP", "factory reset — could not remove %s", settingsLeft.c_str());
    }
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

  logSystemEvent("SETUP", "factory reset — accounts, pending requests, user settings and account "
                 "transcripts deleted%s; rebooting to setup wizard",
                 sdSkipped ? " (SD card not mounted: /sd/stt kept)" : "");

  snprintf(respBuf, sizeof(respBuf),
           "Factory reset complete. Accounts, pending requests, user settings and account "
           "transcripts/notes deleted.%s%s Rebooting in 1 second to start setup wizard...",
           sdSkipped ? " SD card not mounted: its /sd/stt folders were kept." : "",
           settingsLeft.length() ? " Some user settings files could not be removed." : "");
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
    "  - Delete pending account requests and every user's settings\n"
    "  - Delete every account's transcripts and notes (/stt/u*, /sd/stt/u*)\n"
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
  return cliConfirmPromptResponse();
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
// Command Registry System
// =========================================================================

// Command handler forward declarations (implemented in .ino)
// cmd_status, cmd_uptime, cmd_fsusage now implemented above
// cmd_testencryption, cmd_testpassword now implemented above
// cmd_temperature, cmd_voltage, cmd_cpufreq now implemented above
// cmd_reboot, cmd_broadcast, cmd_wait now implemented above
// cmd_lightsleep now implemented above

// Main/Core command registry (commands that remain in main .ino file)
// Most commands have been modularized - this contains only core system commands
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
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
  if (VFS::existsGuarded("/system/sys_logs/crash-history.log", currentAuthContext())) {
    cliHint("full history (survives reboots): /system/sys_logs/crash-history.log");
  }
  return "OK";
}

extern const CommandEntry commands[] = {
  // ---- Core / General ----
  { "status", "Show system status (WiFi, FS, memory). (add 'json' for JSON output)", false, cmd_status },
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
    "Usage: cpufreq [MHz] (run without arguments to list supported values)" },
  { "taskstats", "Detailed task statistics (state/prio/stack min-free). (add 'json' for JSON output)", false, cmd_taskstats },
  { "perftop", "Live performance snapshot: loop laps/s, period, per-section timing, worst stalls + live task CPU%. (add 'json' for loop + per-task CPU% JSON)", false, cmd_perftop },
  { "events", "Show recent system events (the in-memory register that drives automation event triggers).", false, cmd_events,
    "Usage: events [kinds [json]]\n  (bare): show the recent-event ring\n  kinds: list every valid event-kind name (json = machine form)" },

  // ---- Misc ----
  { "reboot", "Reboot the system.", true, cmd_reboot },
  { "ramflush", "Reboot to reclaim RAM, restoring the features running right now.", true, cmd_ramflush,
    "Usage: ramflush [status]\n"
    "  (bare):  capture which features are running, reboot, and restore them\n"
    "  status:  show what the last boot restored (no reboot)\n"
    "\n"
    "Restores for that one boot only — a normal reboot returns to configured\n"
    "autostart. Never changes your autostart settings." },
  { "factoryreset", "Wipe user accounts and their data and reboot to re-run setup wizard.", true,
    cmd_factoryreset,
    "Usage: factoryreset (no args, confirmation required)\n"
    "Deletes /system/users/users.json, pending account requests, every user's\n"
    "settings file and every account's transcripts and notes (/stt/u*, /sd/stt/u*),\n"
    "so the first-time setup wizard runs on next boot. WiFi credentials and\n"
    "other settings are preserved.",
    /*requiresSuperAdmin=*/true },
  { "broadcast", "Send a message to all connected output interfaces.", true, cmd_broadcast,
    "Usage: broadcast <message>" },
  // pendinglist is registered by the users module (System_User.cpp).
  { "wait", "Delay execution for N milliseconds: wait <ms>.", false, cmd_wait,
    "Usage: wait <ms>  (1..60000)" },
  { "sleep", "Alias for wait: sleep <ms>.", false, cmd_wait,
    "Usage: sleep <ms>  (1..60000)" },
  { "lightsleep", "Enter ESP32 light sleep: lightsleep [seconds] (default 20s).", true, cmd_lightsleep,
    "Usage: lightsleep [seconds]  (1..3600, default 20)" },
  { "deepsleep", "Power off via deep sleep (no wake source — reset button to wake).", true, cmd_deepsleep,
    "Usage: deepsleep  (admin; wakes only via physical reset)" },
};

extern const size_t commandsCount = sizeof(commands) / sizeof(commands[0]);

// Registered by System_CommandRegistry.cpp

// Battery commands (from System_Battery.cpp)
#if ENABLE_BATTERY_MONITOR
extern const char* cmd_battery_status(const String& argsInput);
extern const char* cmd_battery_calibrate(const String& argsInput);
extern const char* cmd_batterylog(const String& argsInput);

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
extern const CommandEntry batteryCommands[] = {
  {"batterystatus", "Show battery voltage, charge level, and status", false, cmd_battery_status},
  {"batterycalibrate", "Reinitialize ADC calibration or re-probe the fuel gauge", true, cmd_battery_calibrate},
  {"batterylog", "Battery time-series CSV log (on/off/interval/tail/clear)", false, cmd_batterylog, "Usage: batterylog [on|off|interval <s>|tail|clear]"}
};

extern const size_t batteryCommandsCount = sizeof(batteryCommands) / sizeof(batteryCommands[0]);
// Registered by System_CommandRegistry.cpp
#endif

// Command lookup is implemented in System_Command.cpp.

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

// External task watermark globals
extern volatile UBaseType_t gTofWatermarkNow, gTofWatermarkMin;
extern volatile UBaseType_t gImuWatermarkNow, gImuWatermarkMin;
extern volatile UBaseType_t gThermalWatermarkNow, gThermalWatermarkMin;

// External memory allocation function
extern void* ps_alloc(size_t size, AllocPref pref, const char* tag);

// Comprehensive memory report - shows what's consuming memory (like Task Manager)
void printMemoryReport() {
  static constexpr size_t kMemReportTopCount = 15;
  MemTrackerSnapshot allocationTraffic{};
  MemTrackerEntry allocationTop[kMemReportTopCount]{};
  const bool haveAllocationTraffic = memTrackerSnapshot(
      allocationTraffic, allocationTop, kMemReportTopCount);

  // 8-bit-capable internal RAM only. ESP.getHeapSize()/getFreeHeap() are
  // MALLOC_CAP_INTERNAL without MALLOC_CAP_8BIT, so they fold in the IRAM-only
  // heap that no malloc/new/task stack can use, and getHeapSize() additionally
  // double-counts the DMA reserve pool. See hw1InternalFreeBytes() in
  // System_MemUtil.h. On feather_esp32_v2 the old calls overstated free by
  // ~25.8 KB and total by ~59.4 KB.
  size_t dram_total = hw1InternalTotalBytes();
  size_t dram_free = hw1InternalFreeBytes();
  size_t dram_used = (dram_total > dram_free) ? (dram_total - dram_free) : 0;
  size_t dram_min = hw1InternalMinFreeBytes();
  size_t dram_peak_used = (dram_total > dram_min) ? (dram_total - dram_min) : 0;

  // The allocator's own view of the SAME heap set, sampled here beside
  // dram_total/dram_free so every figure in TOTALS comes from one moment.
  //
  // RAW heap_caps_get_total_size(), NOT hw1InternalTotalBytes(): the helper
  // subtracts CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL to cancel the reserve
  // pool's double registration (System_MemUtil.h). Allocator metadata is a
  // property of the physical spans, so the helper would understate it by
  // exactly the reserve size.
  //
  // Per heap, span = heap_t + TLSF control_t + allocated + free + 4 B per
  // live block (multi_heap.c, multi_heap_get_info_impl), so
  // span - free - allocated IS the bookkeeping, over the identical cap
  // predicate heap_caps_get_total_size/get_info both apply. This is memory
  // nobody allocated that still counts as "used": an empty heap reports its
  // own control structures as consumed. dram_used already nets the reserve
  // pool out, so the residual in TOTALS cancels it without naming it — exact
  // to within one byte per reserve chunk.
  multi_heap_info_t dram_info{};
  heap_caps_get_info(&dram_info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t dram_span_raw  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t dram_span_used = dram_info.total_free_bytes + dram_info.total_allocated_bytes;
  const size_t alloc_meta =
      (dram_span_raw > dram_span_used) ? (dram_span_raw - dram_span_used) : 0;

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
  // Derived from the single sample above, not re-queried: get_free_size sums
  // the same per-heap free_bytes get_info reports, and get_largest_free_block
  // IS get_info(...).largest_free_block. One fewer full TLSF block walk.
  size_t cap_int_free = dram_info.total_free_bytes;
  size_t cap_int_largest = dram_info.largest_free_block;
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
  broadcastOutput("-- MEMORY BREAKDOWN (measured live heap + cumulative allocation traffic) --");

  // ========== SECTION 1: Cumulative allocation traffic ==========
  // This registry has no free hook and is deliberately not a live-ownership
  // ledger.  Never add these values to the live heap accounting below.
  if (haveAllocationTraffic &&
      (allocationTraffic.enabled || allocationTraffic.entryCount > 0 ||
       allocationTraffic.contentionDrops > 0 ||
       allocationTraffic.invalidTagEvents > 0)) {
    broadcastOutput("");
    broadcastOutput("[1] ALLOCATION TRAFFIC (cumulative since tracker reset; not live):");
    BROADCAST_PRINTF("  State: %s  tags=%u/%u attempts=%llu successes=%llu failures=%llu fallbacks=%llu",
                     allocationTraffic.enabled ? "ON" : "OFF",
                     (unsigned)allocationTraffic.entryCount,
                     (unsigned)allocationTraffic.capacity,
                     (unsigned long long)(allocationTraffic.successCount +
                                          allocationTraffic.failureCount),
                     (unsigned long long)allocationTraffic.successCount,
                     (unsigned long long)allocationTraffic.failureCount,
                     (unsigned long long)allocationTraffic.fallbackCount);

    for (size_t i = 0; i < allocationTraffic.topCount; ++i) {
      char location[12];
      if (allocationTop[i].psramBytes > 0 && allocationTop[i].dramBytes > 0) {
        snprintf(location, sizeof(location), "PS+DR");
      } else if (allocationTop[i].psramBytes > 0) {
        snprintf(location, sizeof(location), "PSRAM");
      } else {
        snprintf(location, sizeof(location), "DRAM");
      }

      BROADCAST_PRINTF("  %-31.31s %8llu B ok:%llu fail:%llu fb:%llu %-5s",
                       allocationTop[i].tag,
                       (unsigned long long)(allocationTop[i].dramBytes +
                                            allocationTop[i].psramBytes),
                       (unsigned long long)allocationTop[i].successCount,
                       (unsigned long long)allocationTop[i].failureCount,
                       (unsigned long long)allocationTop[i].fallbackCount,
                       location);
    }

    if (allocationTraffic.topCount < allocationTraffic.entryCount) {
      BROADCAST_PRINTF("  ... and %u more tags",
                       (unsigned)(allocationTraffic.entryCount -
                                  allocationTraffic.topCount));
    }

    BROADCAST_PRINTF("  Cumulative requested: %llu B [DRAM %llu B, PSRAM %llu B]",
                     (unsigned long long)(allocationTraffic.dramBytes +
                                          allocationTraffic.psramBytes),
                     (unsigned long long)allocationTraffic.dramBytes,
                     (unsigned long long)allocationTraffic.psramBytes);
    BROADCAST_PRINTF("  Drops: lock=%llu invalid-tag=%llu registry-full=%llu",
                     (unsigned long long)allocationTraffic.contentionDrops,
                     (unsigned long long)allocationTraffic.invalidTagEvents,
                     (unsigned long long)allocationTraffic.overflowEvents);
  }

  // ========== SECTION 2: LIVE HEAP ATTRIBUTION ==========
  // Everything summed here is MEASURED off the live allocator. Lines marked
  // '~' are estimates and are deliberately NOT summed — an estimate folded
  // into a measured total corrupts the measurement, which is exactly how the
  // old hardcoded "FreeRTOS ~8192" credit hid a 24 KB shortfall
  // (docs/MEMORY_REPORT_FIX_PLAN.md, defects 1, 2 and 4).
  broadcastOutput("");
  broadcastOutput("[2] LIVE HEAP ATTRIBUTION (measured; '~' lines are estimates, not summed):");

  // Task snapshot buffer plus a parallel measurement buffer. Both persist
  // across calls (the report runs at boot and from `memreport`) and grow
  // only when the task count does.
  // +4 headroom, as reportAllTaskStacks() does: uxTaskGetSystemState() returns
  // 0 — not a partial fill — when the array is smaller than the live task
  // count, and at boot the radio stacks are still spawning tasks on the other
  // core while this runs.
  UBaseType_t taskCount = uxTaskGetNumberOfTasks() + 4;
  static TaskStatus_t*    taskStatusArray = nullptr;
  static TaskHeapMeasure* taskMeasure     = nullptr;
  static UBaseType_t      taskStatusCap   = 0;
  if (taskCount > taskStatusCap) {
    if (taskStatusArray) { free(taskStatusArray); taskStatusArray = nullptr; }
    if (taskMeasure)     { free(taskMeasure);     taskMeasure     = nullptr; }
    taskStatusCap = 0;
    taskStatusArray = (TaskStatus_t*)ps_alloc(taskCount * sizeof(TaskStatus_t),
                                              AllocPref::PreferPSRAM, "memreport.tasks");
    taskMeasure     = (TaskHeapMeasure*)ps_alloc(taskCount * sizeof(TaskHeapMeasure),
                                                 AllocPref::PreferPSRAM, "memreport.taskmeas");
    if (taskStatusArray && taskMeasure) {
      taskStatusCap = taskCount;
    } else {                       // partial failure must not leave a half-armed pair
      if (taskStatusArray) { free(taskStatusArray); taskStatusArray = nullptr; }
      if (taskMeasure)     { free(taskMeasure);     taskMeasure     = nullptr; }
    }
  }

  size_t   app_stacks_total   = 0;
  size_t   other_stacks_total = 0;
  size_t   tcb_total          = 0;
  unsigned app_task_count     = 0;
  unsigned other_task_count   = 0;
  unsigned tcb_count          = 0;
  unsigned unmeasured_tasks   = 0;
  bool     tasks_measured     = false;   // TOTALS must not print 0 as a measurement

  if (taskStatusArray && taskMeasure) {
    // taskStatusCap, not taskCount: cap is only ever set on a successful
    // pair-alloc, so it is always the true capacity of both arrays.
    UBaseType_t actualCount = uxTaskGetSystemState(taskStatusArray, taskStatusCap, NULL);
    tasks_measured = (actualCount > 0);

    // MEASURE FIRST, PRINT SECOND. The snapshot's pxStackBase / xHandle are
    // raw pointers into memory the IDLE task frees asynchronously after
    // vTaskDelete(NULL). One allocator walk here, before ~80 queue messages
    // of printing, keeps the snapshot-to-measure window at microseconds; the
    // walk itself only reports a LIVE block that starts exactly at the
    // pointer (System_TaskUtils.h, taskHeapMeasureSnapshot).
    taskHeapMeasureSnapshot(taskStatusArray, taskMeasure, actualCount);

    // First pass: tasks this firmware created (present in the creation
    // registry). Both columns are measured; the registry only classifies.
    broadcastOutput("  Application task stacks (used / allocated):");

    for (UBaseType_t i = 0; i < actualCount; i++) {
      if (taskMeasure[i].regBytes == 0) continue;   // not in our creation registry
      const char*  name      = taskStatusArray[i].pcTaskName;
      const size_t freeBytes = taskStatusArray[i].usStackHighWaterMark;  // BYTES on this port

      if (taskMeasure[i].tcbBytes) { tcb_total += taskMeasure[i].tcbBytes; tcb_count++; }

      if (taskMeasure[i].stackBytes == 0) {
        // Registry knows the requested size, but the block is not in internal
        // heap (PSRAM stack, or a caller-owned buffer), so it is NOT part of
        // dram_used. Shown, never summed.
        unmeasured_tasks++;
        BROADCAST_PRINTF("    %-20s     ? / ~%5lu bytes (internal allocation unmeasured; free %lu)",
                         name, (unsigned long)taskMeasure[i].regBytes,
                         (unsigned long)freeBytes);
        continue;
      }
      const size_t allocatedBytes = taskMeasure[i].stackBytes;
      const size_t usedBytes = (allocatedBytes > freeBytes) ? (allocatedBytes - freeBytes) : 0;
      app_stacks_total += allocatedBytes;
      app_task_count++;
      BROADCAST_PRINTF("    %-20s %5lu / %5lu bytes (%2u%% used)",
                       name, (unsigned long)usedBytes, (unsigned long)allocatedBytes,
                       (unsigned)((usedBytes * 100) / allocatedBytes));
    }

    // Second pass: everything not in our creation registry. The header does
    // NOT claim kernel ownership — the registry is a membership test, and a
    // task created without taskStackRecord (e.g. "llm_gen") or dropped by
    // registry overflow lands here too. These stacks AND TCBs are heap, not
    // .bss: ESP-IDF pvPortMallocs both even for the "static" IDLE and Tmr Svc
    // tasks (port_common.c). The old report printed their HWM and summed
    // NOTHING — 20,992 B of live heap silently dropped on the minimal build,
    // more on every feature-enabled one.
    broadcastOutput("");
    broadcastOutput("  Other task stacks (kernel / unregistered, used / allocated):");

    for (UBaseType_t i = 0; i < actualCount; i++) {
      if (taskMeasure[i].regBytes != 0) continue;   // shown above
      const char*  name      = taskStatusArray[i].pcTaskName;
      const size_t freeBytes = taskStatusArray[i].usStackHighWaterMark;

      if (taskMeasure[i].tcbBytes) { tcb_total += taskMeasure[i].tcbBytes; tcb_count++; }

      if (taskMeasure[i].stackBytes == 0) {
        unmeasured_tasks++;
        BROADCAST_PRINTF("    %-20s     ? /     ? bytes (internal allocation unmeasured; free %lu)",
                         name, (unsigned long)freeBytes);
        continue;
      }
      const size_t allocatedBytes = taskMeasure[i].stackBytes;
      const size_t usedBytes = (allocatedBytes > freeBytes) ? (allocatedBytes - freeBytes) : 0;
      other_stacks_total += allocatedBytes;
      other_task_count++;
      BROADCAST_PRINTF("    %-20s %5lu / %5lu bytes (%2u%% used)",
                       name, (unsigned long)usedBytes, (unsigned long)allocatedBytes,
                       (unsigned)((usedBytes * 100) / allocatedBytes));
    }

    // 3 lines, ~150 B expanded against the 256 B BROADCAST_PRINTF stack
    // buffer (System_Debug.h, where overflow truncates SILENTLY).
    BROADCAST_PRINTF(
      "  Stacks (application): %6lu bytes  (%2u tasks)\n"
      "  Stacks (other):       %6lu bytes  (%2u tasks)\n"
      "  Task control blocks:  %6lu bytes  (%2u of %2u tasks measured)",
      (unsigned long)app_stacks_total,   app_task_count,
      (unsigned long)other_stacks_total, other_task_count,
      (unsigned long)tcb_total,          tcb_count, (unsigned)actualCount);

    if (unmeasured_tasks > 0) {
      BROADCAST_PRINTF("  (%u task stack(s) unmeasured in internal heap - NOT attributed)",
                       unmeasured_tasks);
    }
    if (actualCount == 0) {
      // uxTaskGetSystemState also has a weak stub in this component that
      // returns 0 unconditionally (System_TaskUtils.cpp).
      broadcastOutput("  !! uxTaskGetSystemState returned 0 - task memory NOT attributed this run");
    }
  } else {
    broadcastOutput("  !! task snapshot allocation failed - task memory NOT attributed this run");
  }

  // WiFi driver estimate. Omitted entirely when WiFi is compiled out — the
  // driver is not linked, so reporting 32 KB of it was a flat lie on a
  // WiFi-less build. Gated rather than zeroed so the line does not read as
  // "the WiFi driver is using nothing", which is a different claim. Print-only:
  // an estimate is never folded into the measured totals.
  //
  // Runtime-gated too: compiled-in is not started. On a boot where WiFi stays
  // off (no wifi/tiT/sys_evt task exists) the driver holds nothing, and the
  // HW-observed residual with WiFi up (~34.7 KB minus the ~10.4 KB base) puts
  // the driver's INTERNAL heap nearer 24 KB than 32 KB — WiFi/LWIP prefer
  // SPIRAM. So the figure is an upper bound, and it is only claimed when the
  // radio is actually up.
#if ENABLE_WIFI
  if (wifiRadioState() != WIFI_RADIO_OFF) {
    size_t wifi_estimate = 32 * 1024;  // upper bound; see comment above
    BROADCAST_PRINTF("  WiFi Driver:   ~ %6lu bytes (%2lu KB) (upper-bound estimate; inside UNATTRIBUTED)",
                    (unsigned long)wifi_estimate, (unsigned long)(wifi_estimate / 1024));
  } else {
    broadcastOutput("  WiFi Driver:   (compiled in, radio off - holds nothing)");
  }
#endif

  // ========== SECTION 3: STATIC VARIABLES BY MODULE ==========
  // .bss/.data, NOT heap. .dram0.bss ends exactly where .dram0.heap_start
  // begins (the base of heap region 1), so no static byte can ever be inside
  // ACTUAL DRAM USED. Printed for scale, excluded from the totals — adding
  // these to a heap-only figure was a category error (defect 3 in
  // docs/MEMORY_REPORT_FIX_PLAN.md).
  broadcastOutput("");
  broadcastOutput("[3] STATIC VARIABLES BY MODULE (.bss/.data - NOT heap, excluded from totals):");
  
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
  size_t thermal_state_bytes = thermalCacheStorageBytes() + sizeof(gThermalRunning) + sizeof(gThermalConnected) + sizeof(gThermalTaskHandle);
  size_t imu_state_bytes = imuCacheStorageBytes() + sizeof(gImuRunning) + sizeof(gImuConnected) + sizeof(gImuTaskHandle);
  size_t tof_state_bytes = tofCacheStorageBytes() + sizeof(gTofRunning) + sizeof(gTofConnected) + sizeof(gTofTaskHandle);
  size_t gamepad_state_bytes = inputCacheStorageBytes() + sizeof(gInputRunning) + sizeof(gInputConnected) + sizeof(gInputTaskHandle);
  size_t apds_state_bytes = apdsCacheStorageBytes() + sizeof(gApdsConnected) + sizeof(gApdsColorRunning) + sizeof(gApdsProximityRunning) + sizeof(gApdsGestureRunning);
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

  // Connected devices. printConnectedDevicesLibraries() fills an ESTIMATE
  // table (i2cSensors[].libraryHeapBytes), not a measurement — shown, never
  // summed into the measured totals below.
  size_t devices_lib_total = 0;
  printConnectedDevicesLibraries(devices_lib_total);
  BROADCAST_PRINTF("  Device Libraries: ~%6lu bytes (%3lu KB) (estimate; inside UNATTRIBUTED)",
                  (unsigned long)devices_lib_total, (unsigned long)(devices_lib_total / 1024));

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
#elif ENABLE_GPIO_ENCODER
  broadcastOutput("  [Y] GPIO_ENC | Input_GPIOEncoder task");
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
  // Only MEASURED quantities are summed here. dram_used and alloc_meta were
  // sampled together at the top of this function, and the task rows come from
  // one allocator walk, so this is one snapshot — not figures taken ~500
  // lines of queue traffic apart.
  const size_t tasks_total = app_stacks_total + other_stacks_total + tcb_total;
  const size_t attributed  = tasks_total + alloc_meta;

  if (!tasks_measured) {
    // No snapshot (allocation failed, or the kernel returned 0 because the
    // task count outran the array). Printing tasks_total = 0 here would read
    // as "tasks cost nothing, measured" — the exact lie this report is meant
    // not to tell. State the gap and stop.
    BROADCAST_PRINTF(
      "\n---------- TOTALS ----------\n"
      "  ACTUAL DRAM USED:     %6lu bytes (%4lu KB)\n"
      "  Allocator metadata:   %6lu bytes (%4lu KB)  measured\n"
      "  Task stacks + TCBs:   NOT MEASURED this run (no task snapshot) - rerun memreport",
      (unsigned long)dram_used,  (unsigned long)(dram_used / 1024),
      (unsigned long)alloc_meta, (unsigned long)(alloc_meta / 1024));
    broadcastOutput("\n========== END MEMORY REPORT ==========\n");
    return;
  }

  // SIGNED on purpose. The old block lived inside `if (dram_used >
  // total_known)`, so an over-count printed NOTHING; and its "Static
  // Over-Estimate" line subtracted two unrelated quantities and read 0 only
  // because it clamped. ATTRIBUTED + UNATTRIBUTED == ACTUAL DRAM USED always
  // holds, because the residual is a difference, never assembled from parts.
  // The residual is NOT padded to zero: on the minimal build ~3 KB of it is
  // genuinely unexplained (docs/DRAM_UNACCOUNTED_CENSUS_2026-08-23.md) and an
  // honest "unattributed" beats a figure engineered to close.
  const long residual = (long)dram_used - (long)attributed;
  const unsigned attr_pct =
      dram_used ? (unsigned)((attributed * 100 + dram_used / 2) / dram_used) : 0;

  // 2 envelopes, ~180 B and ~150 B expanded, against BROADCAST_PRINTF's
  // 256 B stack buffer where overflow truncates silently.
  BROADCAST_PRINTF(
    "\n---------- TOTALS ----------\n"
    "  ACTUAL DRAM USED:     %6lu bytes (%4lu KB)\n"
    "  Task stacks + TCBs:   %6lu bytes (%4lu KB)  measured\n"
    "  Allocator metadata:   %6lu bytes (%4lu KB)  measured",
    (unsigned long)dram_used,   (unsigned long)(dram_used / 1024),
    (unsigned long)tasks_total, (unsigned long)(tasks_total / 1024),
    (unsigned long)alloc_meta,  (unsigned long)(alloc_meta / 1024));

  BROADCAST_PRINTF(
    "  ATTRIBUTED:           %6lu bytes (%4lu KB) [%2u%%]\n"
    "  UNATTRIBUTED:       %8ld bytes  (kernel objs, libc/VFS/FS/NVS, C++ ctors, driver heap)",
    (unsigned long)attributed, (unsigned long)(attributed / 1024), attr_pct,
    residual);

  broadcastOutput("\n========== END MEMORY REPORT ==========\n");
}

// Command handlers
const char* cmd_memreport(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    // 8-bit-capable internal RAM only (see hw1InternalFreeBytes, System_MemUtil.h).
    size_t dramTotal = hw1InternalTotalBytes();
    size_t dramFree  = hw1InternalFreeBytes();
    size_t dramMin   = hw1InternalMinFreeBytes();
    JsonObject dram = doc["dram"].to<JsonObject>();
    dram["total"]    = (unsigned long)dramTotal;
    dram["used"]     = (unsigned long)(dramTotal > dramFree ? dramTotal - dramFree : 0);
    dram["free"]     = (unsigned long)dramFree;
    dram["minFree"]  = (unsigned long)dramMin;
    dram["peakUsed"] = (unsigned long)(dramTotal > dramMin ? dramTotal - dramMin : 0);
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
    MemTrackerSnapshot tracker{};
    if (memTrackerSnapshot(tracker)) {
      JsonObject at = doc["allocationTraffic"].to<JsonObject>();
      at["semantics"] = "cumulative_requested_since_reset";
      at["enabled"] = tracker.enabled;
      at["generation"] = tracker.generation;
      at["tags"] = (unsigned)tracker.entryCount;
      at["capacity"] = (unsigned)tracker.capacity;
      at["attempts"] =
          (unsigned long long)(tracker.successCount + tracker.failureCount);
      at["successes"] = (unsigned long long)tracker.successCount;
      at["failures"] = (unsigned long long)tracker.failureCount;
      at["fallbacks"] = (unsigned long long)tracker.fallbackCount;
      at["dramBytes"] = (unsigned long long)tracker.dramBytes;
      at["psramBytes"] = (unsigned long long)tracker.psramBytes;
      at["contentionDrops"] = (unsigned long long)tracker.contentionDrops;
      at["invalidTagEvents"] = (unsigned long long)tracker.invalidTagEvents;
      at["overflowEvents"] = (unsigned long long)tracker.overflowEvents;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }
  printMemoryReport();
  return "Memory report printed to serial";
}



namespace {

const char* taskStateLabel(eTaskState state) {
  switch (state) {
    case eRunning:   return "RUN";
    case eReady:     return "READY";
    case eBlocked:   return "BLOCK";
    case eSuspended: return "SUSP";
    case eDeleted:   return "DEL";
    default:         return "UNK";
  }
}

// Per-command samples shared by perftop's text and JSON renderers. Owns the
// arrays only; task-name pointers retain FreeRTOS's existing lifetime contract.
// OLED and periodic stack reports keep their own sampling intervals/baselines.
struct TaskCpuSample {
  TaskStatus_t* before = nullptr;
  TaskStatus_t* after = nullptr;
  UBaseType_t beforeCount = 0;
  UBaseType_t afterCount = 0;
  uint32_t totalDelta = 0;

  TaskCpuSample(const char* beforeTag, const char* afterTag) {
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 6;
    before = (TaskStatus_t*)ps_alloc(capacity * sizeof(TaskStatus_t),
                                   AllocPref::PreferPSRAM, beforeTag);
    after = (TaskStatus_t*)ps_alloc(capacity * sizeof(TaskStatus_t),
                                  AllocPref::PreferPSRAM, afterTag);
    if (!allocated()) return;

    uint32_t beforeTotal = 0, afterTotal = 0;
    beforeCount = uxTaskGetSystemState(before, capacity, &beforeTotal);
    vTaskDelay(pdMS_TO_TICKS(750));
    afterCount = uxTaskGetSystemState(after, capacity, &afterTotal);
    totalDelta = afterTotal - beforeTotal;
  }

  ~TaskCpuSample() {
    if (before) free(before);
    if (after) free(after);
  }
  TaskCpuSample(const TaskCpuSample&) = delete;
  TaskCpuSample& operator=(const TaskCpuSample&) = delete;

  bool allocated() const { return before && after; }
  bool available() const {
    return beforeCount != 0 && afterCount != 0 && totalDelta != 0;
  }

  // -1 means no usable baseline (new task or suspected handle reuse).
  int cpuPercent(UBaseType_t index) const {
    if (!available() || index >= afterCount) return -1;
    for (UBaseType_t i = 0; i < beforeCount; ++i) {
      if (before[i].xHandle != after[index].xHandle) continue;
      const uint32_t delta = after[index].ulRunTimeCounter - before[i].ulRunTimeCounter;
      if (delta > totalDelta + totalDelta / 4) return -1;
      const uint32_t percent = (uint32_t)((uint64_t)delta * 100 / totalDelta);
      return percent > 100 ? 100 : (int)percent;
    }
    return -1;
  }
};

}  // namespace

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
      JsonObject t = arr.add<JsonObject>();
      t["name"]  = taskArray[i].pcTaskName ? taskArray[i].pcTaskName : "?";
      t["state"] = taskStateLabel(taskArray[i].eCurrentState);
      t["prio"]  = (unsigned)taskArray[i].uxCurrentPriority;
      // usStackHighWaterMark is already BYTES under ESP-IDF — emit raw, never
      // *4 (see System_TaskUtils.h / feedback_task_stack_bytes_not_words).
      t["stackFree"] = (unsigned)taskArray[i].usStackHighWaterMark;
      // Total allocated stack, also BYTES. FreeRTOS can't report it (pxEndOfStack
      // is compiled out on this down-growing port), so it comes from the
      // creation-time registry and only covers tasks HardwareOne creates.
      // Emitted only when known — the key is ABSENT for IDF-owned tasks
      // (ipc0/ipc1, esp_timer, IDLEn, wifi, BT), which consumers render as
      // headroom-only rather than assuming a total.
      uint32_t stackTotal = taskStackLookup(taskArray[i].pcTaskName);
      if (stackTotal > 0) t["stackSize"] = (unsigned)stackTotal;
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
    BROADCAST_PRINTF("%-18.18s %-5s %4u %5u",
                     taskArray[i].pcTaskName,
                     taskStateLabel(taskArray[i].eCurrentState),
                     (unsigned)taskArray[i].uxCurrentPriority,
                     (unsigned)taskArray[i].usStackHighWaterMark);
  }

  return "OK";
}


// perftop — live performance snapshot. Prints the main-loop health (from the
// loopHealthTick profiler in HardwareOne.cpp) plus a fresh per-task CPU%
// sampled over ~0.75 s on cmd_exec. Synchronous callers wait for this sample.
// uint64 math avoids the *100 overflow in run-time percentage calculations.
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
    TaskCpuSample sample("perftop.ja", "perftop.jb");
    if (sample.available()) {
      for (UBaseType_t i = 0; i < sample.afterCount; i++) {
        const char* nm = sample.after[i].pcTaskName;
        if (!nm) continue;
        const int pct = sample.cpuPercent(i);
        if (pct < 0) continue;
        const bool isIdle = (strncmp(nm, "IDLE", 4) == 0);
        // Keep IDLE rows (per-core headroom) + non-idle >= 1%.
        if (!isIdle && pct < 1) continue;
        JsonObject t = tasks.add<JsonObject>();
        t["name"]   = nm;
        t["cpuPct"] = (unsigned long)pct;
        t["idle"]   = isIdle;
      }
    }
    doc["cpuValid"] = sample.available();

    // Retain the sample arrays through serialization. Task names are borrowed
    // from FreeRTOS; retaining the arrays does not pin a task's TCB.
    serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);
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

  bool allocationFailed = false;
  {
    TaskCpuSample sample("perftop.a", "perftop.b");
    if (!sample.allocated()) {
      allocationFailed = true;
    } else if (!sample.available()) {
      broadcastOutput("  (run-time stats unavailable)");
    } else {
      // pass 0: IDLE0/IDLE1 (per-core headroom); pass 1: non-idle tasks >= 1%.
      int shown = 0;
      for (int pass = 0; pass < 2; pass++) {
        for (UBaseType_t i = 0; i < sample.afterCount; i++) {
          const char* nm = sample.after[i].pcTaskName;
          if (!nm) continue;
          bool isIdle = (strncmp(nm, "IDLE", 4) == 0);
          if (pass == 0 && !isIdle) continue;
          if (pass == 1 && isIdle) continue;
          const int pct = sample.cpuPercent(i);
          if (pct < 0) continue;
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
  }
  if (allocationFailed) broadcastOutput("  (cannot allocate task snapshot buffers)");
  broadcastOutput("========== END PERFTOP ==========");
  return "OK";
}


// ============================================================================
// Command Execution Functions - MIGRATED from main .ino
// ============================================================================
// External dependencies for command execution
extern bool gCLIValidateOnly;
extern QueueHandle_t gCmdExecQ;
extern TaskHandle_t gCmdExecTaskHandle;

// External functions
extern bool handleHelpNavigation(const String& cmd, char* out, size_t outSize);
extern String exitToNormalBanner();
extern bool isAdminUser(const String& user);

// Note: resolveRegistryCommandKey() is now defined in command_system.cpp

// Guest accounts may authenticate, but the command surface is caller-local
// login/logout plus whoami only. View-only UX lives in web/OLED; this is the
// CLI chokepoint shared by every transport that funnels through executeCommand.
static bool commandAllowedForGuest(const String& line) {
  CommandArgs args(line);
  if (args.unterminatedQuote() || args.count() == 0) return false;

  // Guest is a real authenticated role, not anonymous/AuthBypass. It may
  // replace or end only its own submitting transport session. Any explicit
  // target is session administration and is deliberately outside the
  // view-only Guest surface.
  if (args.arg(0).equalsIgnoreCase("login")) {
    return args.count() == 3;  // login <user> <pass>
  }
  if (args.arg(0).equalsIgnoreCase("logout")) {
    return args.count() == 1;  // logout
  }
  if (args.arg(0).equalsIgnoreCase("whoami")) {
    return args.count() == 1;
  }
  return false;
}

// While the account must change its password (userMustChangePassword), the
// command surface is the change itself plus the caller-local session verbs:
// the command-line form of the web gate (webPasswordChangeAllows).
static bool commandAllowedDuringPasswordChange(const String& line) {
  CommandArgs args(line);
  if (args.unterminatedQuote() || args.count() == 0) return false;
  if (args.arg(0).equalsIgnoreCase("userchangepassword")) return true;
  if (args.arg(0).equalsIgnoreCase("login")) return args.count() == 3;
  if (args.arg(0).equalsIgnoreCase("logout")) return args.count() == 1;
  if (args.arg(0).equalsIgnoreCase("whoami")) return args.count() == 1;
  return false;
}

// The bonded peer runs every remote:/@ command as bond-admin, which is super
// for the live bond session (v4_handle_cmd, System_ESPNow.cpp). Here the
// unwrapped line can be checked only against this build's registry flags.
// Handlers that decide access at run time from the caller (file path rules,
// the owner of notes and transcripts, user settings, sessions, voice arming)
// would decide it for super on the peer, and a command this build does not
// compile has no row here at all. So a remote command needs a local
// administrator, except the lines below: the bond page and the OLED bond
// screen send them for any signed-in user, and their handlers check no
// identity. Each starts or stops one of the peer's sensors and takes no
// arguments. Anything not listed, including a command added later, needs an
// administrator.
static bool remoteCommandAllowedWithoutAdmin(const String& line) {
  static const char* const kSensors[] = {
      "thermal", "tof", "imu", "input", "apds", "gps", "rtc", "presence", "fmradio"};
  CommandArgs args(line);
  if (args.unterminatedQuote() || args.count() != 1 || args.argWasQuoted(0)) return false;
  String verb = args.arg(0);
  verb.toLowerCase();  // the registry matches names without case
  const char* sensor = nullptr;
  if (verb.startsWith("open")) sensor = verb.c_str() + 4;
  else if (verb.startsWith("close")) sensor = verb.c_str() + 5;
  if (!sensor) return false;
  for (const char* known : kSensors) {
    if (strcmp(sensor, known) == 0) return true;
  }
  return false;
}

enum class SessionControlForm : uint8_t {
  Other,
  BareLogin,
  TargetedLogin,
  BareLogout,
  TargetedLogout,
  MalformedLogin,
  MalformedLogout,
};

// Classify the complete command line once so authorization and the handlers
// cannot disagree about whether a target transport was supplied.
static SessionControlForm classifySessionControlLine(const String& line) {
  CommandArgs args(line);
  if (args.count() == 0) return SessionControlForm::Other;
  if (args.arg(0).equalsIgnoreCase("login")) {
    if (args.unterminatedQuote() || args.count() < 3 || args.count() > 4) {
      return SessionControlForm::MalformedLogin;
    }
    return args.count() == 4 ? SessionControlForm::TargetedLogin
                             : SessionControlForm::BareLogin;
  }
  if (args.arg(0).equalsIgnoreCase("logout")) {
    if (args.unterminatedQuote() || args.count() > 2) {
      return SessionControlForm::MalformedLogout;
    }
    return args.count() == 2 ? SessionControlForm::TargetedLogout
                             : SessionControlForm::BareLogout;
  }
  return SessionControlForm::Other;
}

static const char* commandSourceSessionName(CommandSource source) {
  switch (source) {
    case SOURCE_SERIAL: return "serial";
    case SOURCE_UART: return "uart";
    case SOURCE_LOCAL_DISPLAY: return "display";
    default: return nullptr;
  }
}

// Explicitly controlling another transport is available only to a real,
// named, still-live Serial/UART/OLED session. This deliberately distinguishes
// a stored Guest role from the anonymous AuthBypass identity used when a
// transport's require-auth toggle is off.
static bool callerMayTargetAnotherSession(const AuthContext& auth,
                                          const char** denialOut) {
  auto deny = [&](const char* reason) {
    if (denialOut) *denialOut = reason;
    return false;
  };

  if (auth.user.length() == 0 || auth.user == "AuthBypass") {
    return deny("Error: sign in on this interface before targeting another session.");
  }
  if (!userMayControlOtherSessions(auth.user)) {
    return deny("Error: a known, unbanned, non-Guest account is required to target another session.");
  }
  if (!commandSourceSessionName(auth.transport)) {
    return deny("Error: targeted login/logout is available only from Serial, UART, or the local display.");
  }

  const CommandContext* commandCtx =
      static_cast<const CommandContext*>(currentCommandContext());
  if (!commandCtx ||
      !(commandCtx->behaviorFlags & COMMAND_CONTEXT_REQUIRE_LIVE_SESSION) ||
      commandCtx->transportSessionEpoch == 0 ||
      commandCtx->auth.transport != auth.transport ||
      commandCtx->auth.user != auth.user ||
      !transportSessionEpochIsLive(auth.transport,
                                   commandCtx->transportSessionEpoch)) {
    return deny("Error: the submitting session is no longer current.");
  }
  const bool originMatches =
      (auth.transport == SOURCE_SERIAL &&
       commandCtx->origin == ORIGIN_SERIAL) ||
      (auth.transport == SOURCE_UART &&
       commandCtx->origin == ORIGIN_UART) ||
      (auth.transport == SOURCE_LOCAL_DISPLAY &&
       commandCtx->origin == ORIGIN_LOCAL_DISPLAY);
  if (!originMatches) {
    return deny("Error: command origin does not match the submitting session.");
  }

  String liveUser;
  bool liveAuthed = false;
  TransportSessionEpoch liveEpoch = 0;
  if (auth.transport == SOURCE_SERIAL) {
    liveEpoch = serialTransportSessionSnapshot(liveUser, liveAuthed);
  } else if (auth.transport == SOURCE_LOCAL_DISPLAY) {
    liveEpoch = localDisplayTransportSessionSnapshot(liveUser, liveAuthed);
  } else {
    char uartUser[65] = {};
    uint32_t namedEpoch = 0;
    uint32_t transportEpoch = 0;
    liveAuthed = uartLinkSessionSnapshot(
        uartUser, sizeof(uartUser), &namedEpoch, &transportEpoch);
    liveUser = uartUser;
    liveEpoch = transportEpoch;
    liveAuthed = liveAuthed && namedEpoch != 0;
  }

  if (!liveAuthed || liveEpoch != commandCtx->transportSessionEpoch ||
      liveUser != auth.user) {
    return deny("Error: a live named login on the submitting interface is required.");
  }
  return true;
}

// Centralized authorization for a command line and context.
// Returns true if authorized, otherwise writes an error to 'out' and returns false.
static bool authorizeCommand(const AuthContext& ctx, const String& line,
                             const CommandResolution& resolution,
                             const String& redactedLine,
                             char* out, size_t outSize) {
  const SessionControlForm sessionForm = classifySessionControlLine(line);
  if (sessionForm == SessionControlForm::TargetedLogin ||
      sessionForm == SessionControlForm::TargetedLogout) {
    const char* denial = nullptr;
    if (!callerMayTargetAnotherSession(ctx, &denial)) {
      snprintf(out, outSize, "%s", denial ? denial : "Error: targeted session control denied.");
      char auditBuf[180];
      snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s",
               redactedLine.c_str());
      logAuthAttempt(false, ctx.path.c_str(),
                     ctx.user.length() ? ctx.user : String("(anonymous)"),
                     ctx.ip, auditBuf);
      systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(),
                      sessionForm == SessionControlForm::TargetedLogin
                          ? "login" : "logout");
      return false;
    }
  }

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
      { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactedLine.c_str()); logAuthAttempt(false, ctx.path.c_str(), "(anonymous)", ctx.ip, auditBuf); }
      return false;
    }
  }
  // Named account role resolution is fail-closed. A missing/corrupt role store
  // must not temporarily turn an already-live Guest into an ordinary User.
  String liveRole;
  const bool liveBondIdentity =
      ctx.transport == SOURCE_ESPNOW && ctx.user == kBondAdminUser &&
      isSuperAdminUser(ctx.user);
  const bool liveG2UartSessionIdentity =
      ctx.transport == SOURCE_INTERNAL && ctx.user == "uart-session" &&
      ctx.path == "/g2evenai";
  if (ctx.user.length() > 0 && ctx.user != "AuthBypass" &&
      !liveBondIdentity &&
      !liveG2UartSessionIdentity &&
      !getUserAuthorizationRole(ctx.user, liveRole)) {
    snprintf(out, outSize, "Error: unable to verify the current account role.");
    logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip,
                   "account_role_unavailable");
    systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(), "role_unavailable");
    return false;
  }
  // Guest: authenticated but view-only. Deny before admin/super checks so a
  // guest never reaches an admin-gated handler by accident.
  if (liveRole == "guest" && !commandAllowedForGuest(line)) {
    String cmdName = line;
    int spacePos = line.indexOf(' ');
    if (spacePos > 0) cmdName = line.substring(0, spacePos);
    snprintf(out, outSize, "Error: Guest accounts are view-only. Only local login/logout and whoami are allowed.");
    { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactedLine.c_str()); logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf); }
    systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(), cmdName.c_str());
    return false;
  }
  // Forced password change: every external transport (the web refuses the
  // same account in tgRequireAuth). Internal callers run stored work such as
  // automations, not a signed-in user, and are not gated here.
  if (ctx.transport != SOURCE_INTERNAL && ctx.user.length() > 0 &&
      ctx.user != "AuthBypass" && !liveBondIdentity &&
      !commandAllowedDuringPasswordChange(line) &&
      userMustChangePassword(ctx.user)) {
    String cmdName = line;
    int spacePos = line.indexOf(' ');
    if (spacePos > 0) cmdName = line.substring(0, spacePos);
    snprintf(out, outSize, "Error: change your password first (userchangepassword <current> <new> <confirm>).");
    { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactedLine.c_str()); logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf); }
    systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(), cmdName.c_str());
    return false;
  }
  // Super-admin (top-tier) protection. Checked independently of the admin flag
  // so a super-only command is gated even if its table entry is not adminOnly.
  // A bonded session satisfies this (bond == super); a regular mesh/pair
  // account never does — the elevation lives entirely in isSuperAdminUser.
  const bool requiresSuperAdmin =
      resolution.entry && resolution.entry->requiresSuperAdmin;
  if (requiresSuperAdmin && !hasSuperAdminPrivilege(ctx)) {
    // The row's own name, so a multi-word row reads `phone secret`, not `phone`.
    const String cmdName = resolution.entry->name;
    snprintf(out, outSize, "Error: Super-admin access required for command '%s'.", cmdName.c_str());
    { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactedLine.c_str()); logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf); }
    if (ctx.user.length() > 0) {
      systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(), cmdName.c_str());
    }
    return false;
  }
  // Admin-only protection via registry
  const bool requiresAdmin =
      resolution.entry &&
      (resolution.entry->requiresAdmin ||
       resolution.entry->requiresSuperAdmin);
  if (requiresAdmin && !hasAdminPrivilege(ctx)) {
    // The row's own name: `ring control`, not `ring` (whose status is open).
    const String cmdName = resolution.entry->name;
    snprintf(out, outSize, "Error: Admin access required for command '%s'. Contact an administrator.", cmdName.c_str());
    { char auditBuf[180]; snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", redactedLine.c_str()); logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf); }
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

// The remote:/@ gate, applied after authorizeCommand has passed the unwrapped
// line: see remoteCommandAllowedWithoutAdmin for why the peer's authority
// needs a local administrator.
static bool authorizeRemoteCommand(const AuthContext& ctx, const String& line,
                                   const String& redactedLine,
                                   char* out, size_t outSize) {
  // The trusted internal identity passes authorizeCommand unchecked; same here.
  if (ctx.transport == SOURCE_INTERNAL && ctx.user == "system") return true;
  if (remoteCommandAllowedWithoutAdmin(line) || hasAdminPrivilege(ctx)) return true;
  snprintf(out, outSize,
           "Error: Admin access required to send this command to the bonded device; "
           "it runs there as administrator.");
  char auditBuf[180];
  snprintf(auditBuf, sizeof(auditBuf), "remote cmd=%.160s", redactedLine.c_str());
  logAuthAttempt(false, ctx.path.c_str(),
                 ctx.user.length() ? ctx.user : String("(anonymous)"), ctx.ip,
                 auditBuf);
  if (ctx.user.length() > 0) {
    systemEventPost(SYSEVT_COMMAND_DENIED, ctx.user.c_str(), "remote");
  }
  return false;
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
  if (currentCommandValidateOnly()) return;              // validate sentinels (e.g. "VALID")
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

#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
// `remote:` reply owner (declared in System_Utils.h).
static constexpr char kRemoteSentPrefix[] = "Remote command sent: ";
static constexpr char kRemoteSentReqId[] = " (reqId ";

void formatRemoteCommandSent(char* out, size_t outSize, const char* safeCommand,
                             uint32_t msgId) {
  if (!out || outSize == 0) return;
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "%s%lu)", kRemoteSentReqId, (unsigned long)msgId);
  const size_t fixed = (sizeof(kRemoteSentPrefix) - 1) + strlen(suffix);
  const char* echo = safeCommand ? safeCommand : "";
  size_t echoLen = strlen(echo);
  // Shorten the echo, never the request id.
  const size_t room = outSize > fixed ? outSize - 1 - fixed : 0;
  if (echoLen > room) echoLen = room;
  snprintf(out, outSize, "%s%.*s%s", kRemoteSentPrefix, (int)echoLen, echo, suffix);
}

bool parseRemoteCommandSentReqId(const char* reply, uint32_t* msgIdOut) {
  const size_t prefixLen = sizeof(kRemoteSentPrefix) - 1;
  if (!reply || !msgIdOut || strncmp(reply, kRemoteSentPrefix, prefixLen) != 0) return false;
  // The id is the last tag and ends the reply; an echoed command cannot fake it.
  const char* tag = nullptr;
  for (const char* p = strstr(reply + prefixLen, kRemoteSentReqId); p;
       p = strstr(p + 1, kRemoteSentReqId)) {
    tag = p;
  }
  if (!tag) return false;
  const char* digits = tag + sizeof(kRemoteSentReqId) - 1;
  const char* p = digits;
  uint64_t value = 0;
  while (*p >= '0' && *p <= '9' && p - digits < 10) value = value * 10 + (uint64_t)(*p++ - '0');
  if (p == digits || p[0] != ')' || p[1] != '\0' || value == 0 || value > 0xFFFFFFFFull) {
    return false;
  }
  *msgIdOut = (uint32_t)value;
  return true;
}
#endif  // ENABLE_ESPNOW && ENABLE_BONDED_MODE

// Handlers share the executor's task state: the validate mirror
// gCLIValidateOnly, the debug and capture buffers and their static result
// buffers. Only cmd_exec runs them, plus the early-boot direct path in
// submitAndExecuteSync before the executor queue exists. Every other task
// submits instead (submitAndExecuteSync, submitCommandAsync,
// executeInternalStatusCommand).
static bool executeCommandCallerAdmitted() {
  if (gCmdExecQ == nullptr) return true;
  return gCmdExecTaskHandle != nullptr &&
         xTaskGetCurrentTaskHandle() == gCmdExecTaskHandle;
}

bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize) {
  // Clear output buffer
  out[0] = '\0';

  // Refuse before any side effect (activity, identity, audit). The command
  // text is not logged: it may carry a credential.
  if (!executeCommandCallerAdmitted()) {
    const char* task = pcTaskGetName(nullptr);
    ERROR_SYSTEMF("[execCmd] refused a direct call from task '%s'; submit it to cmd_exec",
                  task ? task : "?");
    snprintf(out, outSize, "Error: commands run on the command executor only.");
    return false;
  }

  // Resolve the immutable queue-admission metadata before any side effect.
  // A logout, reconnect or same-user re-login invalidates the captured
  // generation, so stale queued work cannot inherit the replacement session.
  CommandContext* cmdCtx = static_cast<CommandContext*>(currentCommandContext());

  // A real user/peer command counts as activity for the power policy: it keeps
  // the clock at the mode's in-use clock and, only when someone is at the
  // device, wakes the local panel. Local means the local display's own
  // commands and on-device voice; a command from the glasses, phone, web,
  // serial, ESP-NOW or MQTT is remote and never lights the panel. This is what
  // lets a headless box with no input device still drop its clock yet come
  // back up on a serial / web / G2 / ESP-NOW command.
  // SOURCE_INTERNAL (automations, scheduler, internal helpers) is excluded so
  // background command execution can't pin the device out of idle.
  // SOURCE_UART is excluded for the same reason: the UART host link is a
  // machine daemon that may poll sensors continuously — a human isn't there,
  // and its traffic must not hold the device out of power-save forever.
  if (ctx.transport != SOURCE_INTERNAL && ctx.transport != SOURCE_UART &&
      !isPassiveTranscriptionCommand(cmd)) {
    powerSaveNoteActivity(ctx.transport == SOURCE_LOCAL_DISPLAY ||
                          ctx.transport == SOURCE_VOICE);
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

  // The full CommandContext also carries automationName.
  // cmd_exec_task always calls setCurrentCommandContext(&r->ctx) before
  // invoking executeCommand, so this returns the correct ctx for queued
  // automation sub-commands. Direct callers that don't set it get nullptr,
  // which correctly suppresses autolog (non-automation paths).
  // Create command String once — reuse everywhere (avoids 5+ String(cmd) temporaries)
  String command = cmd;
  command.trim();

  const CommandResolution commandResolution = resolveCommand(command);
  const String safeCommandForTrace =
      redactCmdForAudit(command, commandResolution);
  DEBUG_CMD_FLOWF("[execCmd] user=%s ip=%s path=%s cmd=%.80s", ctx.user.c_str(), ctx.ip.c_str(), ctx.path.c_str(), safeCommandForTrace.c_str());

  const bool requiresLiveSession =
      cmdCtx && (cmdCtx->behaviorFlags &
                 COMMAND_CONTEXT_REQUIRE_LIVE_SESSION);
  const bool staleTransportSession = cmdCtx &&
      ((requiresLiveSession && cmdCtx->transportSessionEpoch == 0) ||
       (cmdCtx->transportSessionEpoch != 0 &&
        (cmdCtx->auth.transport != ctx.transport ||
         !transportSessionEpochIsLive(ctx.transport,
                                      cmdCtx->transportSessionEpoch))));
  if (staleTransportSession) {
    snprintf(out, outSize,
             "Error: transport session changed before command execution.");
    char auditBuf[180];
    snprintf(auditBuf, sizeof(auditBuf), "stale_session cmd=%.150s",
             safeCommandForTrace.c_str());
    logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf);
    return false;
  }

  // G2 EvenAI's internal capture commands are not generic SOURCE_INTERNAL
  // work. They carry a one-shot capability bound to the active exchange and
  // the named UART login that admitted it. Revalidate at executor admission so
  // queued microphone work cannot survive host logout/re-login.
  if (cmdCtx &&
      (cmdCtx->behaviorFlags &
       COMMAND_CONTEXT_REQUIRE_G2_EVENAI_AUTHORITY)) {
    const bool exactSyntheticIdentity =
        ctx.transport == SOURCE_INTERNAL && ctx.user == "uart-session" &&
        ctx.path == "/g2evenai";
    if (!exactSyntheticIdentity || cmdCtx->authorityId == 0 ||
        cmdCtx->authoritySessionEpoch == 0 ||
        !g2EvenAiExchangeBoundToUartSession(
            cmdCtx->authorityId, cmdCtx->authoritySessionEpoch)) {
      snprintf(out, outSize,
               "Error: G2 EvenAI UART authority changed before command execution.");
      logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip,
                     "stale_g2_evenai_authority");
      return false;
    }
  }

  // AuthBypass is an execution-time policy, not a durable permission. A
  // command admitted while authentication was disabled must not run after an
  // administrator enables it ahead of the command in the shared executor.
  if (ctx.user == "AuthBypass") {
    const bool authNowRequired =
        (ctx.transport == SOURCE_SERIAL && gSettings.serialRequireAuth) ||
        (ctx.transport == SOURCE_UART && gSettings.uartRequireAuth) ||
        (ctx.transport == SOURCE_LOCAL_DISPLAY &&
         gSettings.localDisplayRequireAuth) ||
        (ctx.transport == SOURCE_BLUETOOTH && gSettings.bleRequireAuth);
    if (authNowRequired) {
      snprintf(out, outSize,
               "Error: authentication policy changed before command execution.");
      logAuthAttempt(false, ctx.path.c_str(), ctx.user, ctx.ip,
                     "stale_auth_bypass");
      return false;
    }
  }

  // Centralized authorization (admin-required and future policies)
  if (!authorizeCommand(ctx, command, commandResolution,
                        safeCommandForTrace, out, outSize)) {
    return false;
  }

  // Log command execution if automation logging is active and this command
  // carries an automationName (i.e. it was queued as an automation sub-command).
  if (automationLogActive() && cmdCtx && cmdCtx->automationName[0]) {
    char logBuf[300];
    snprintf(logBuf, sizeof(logBuf), "[%s] %s", cmdCtx->automationName,
             safeCommandForTrace.c_str());
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

    // The peer necessarily authenticates the wire envelope as the bonded
    // device identity, which is super-admin. That transport identity must not
    // become a privilege elevator for the human who submitted this wrapper.
    // Apply the ordinary command policy to the unwrapped line *locally* before
    // placing it in the bond envelope. This includes admin/super flags and the
    // exact-session policy for targeted login/logout. Checks a handler makes
    // at run time against the caller would see bond-admin on the peer, so
    // authorizeRemoteCommand then requires a local administrator for anything
    // but the peer sensor toggles. The peer still validates the authenticated
    // envelope and runs its own authorization as defense in depth.
    const CommandResolution remoteResolution = resolveCommand(actualCommand);
    const String safeRemoteCommand =
        redactCmdForAudit(actualCommand, remoteResolution);
    if (!authorizeCommand(ctx, actualCommand, remoteResolution,
                          safeRemoteCommand, out, outSize) ||
        !authorizeRemoteCommand(ctx, actualCommand, safeRemoteCommand,
                                out, outSize)) {
      return false;
    }

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
      // any redaction at all; it has to redact for itself. The reply carries
      // msgId, which the peer's CMD_RESP echoes as its reqId.
      formatRemoteCommandSent(out, outSize, safeRemoteCommand.c_str(), msgId);
      broadcastOutput("[REMOTE] Sent to bonded device: " + safeRemoteCommand);
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

  // Reuse the immutable registry row resolved before authorization. Interactive
  // modes still get first crack below, including arbitrary non-registry input.
  const CommandEntry* found = commandResolution.entry;
  const size_t foundLen = commandResolution.matchedLength;
  bool registrySuccess = false;

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
    // Consumption is distinct from command success. Keep the same Error:
    // contract used by registry handlers so transports can return a failure
    // status without forcing the interactive mode to exit.
    return !(out && (strncmp(out, "Error", 5) == 0 ||
                     strncmp(out, "ERROR", 5) == 0));
  }

  if (found) {
    // Execute handler - pass only args, not full command
    String args;
    if (command.length() > foundLen) {
      args = command.substring(foundLen);
      args.trim();
    }
    DEBUG_CMD_FLOWF("[registry_exec] executing: %s", found->name);
    DEBUGF(DEBUG_CLI, "[registry_exec] executing: %s", found->name);

    // Capture CLIMode state BEFORE the handler runs. If the handler enters
    // a mode (e.g. cmd_filedelete -> cliRequestConfirm) the command hasn't
    // actually completed -- it just prompted the user. The real completion
    // (and the right moment to audit) is when the user resolves the prompt
    // and the mode's onInput composes the audit line with full context.
    // See System_CLIConfirm::confirm_onInput for the resolution audit.
    const uint32_t modeInstanceBeforeHandler = cliModeInstanceId();

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
    const uint32_t modeInstanceAfterHandler = cliModeInstanceId();
    const bool handlerEnteredMode =
        modeInstanceAfterHandler != 0 &&
        modeInstanceAfterHandler != modeInstanceBeforeHandler &&
        cliModeCurrentCommandOwns();
    if (!handlerEnteredMode) {
      registrySuccess = (strncmp(out, "Error", 5) != 0) &&
                        (strncmp(out, "ERROR", 5) != 0);
      logCommandExecutionPrepared(ctx, cmd, safeCommandForTrace,
                                  registrySuccess, out);
      stampOkStatus(out, outSize, registrySuccess);  // stamp AFTER audit so the log keeps the raw result
    } else {
      registrySuccess = true;  // accepted prompt; resolution is audited later
      DEBUG_CMD_FLOWF("[execCmd] suppressing audit -- handler entered mode '%s'",
                      cliCurrentMode() && cliCurrentMode()->name
                        ? cliCurrentMode()->name : "(unnamed)");
    }
  } else {
    // Command not found
    snprintf(out, outSize, "Unknown command: %s\nType 'help' for available commands",
             safeCommandForTrace.c_str());
    
    // Log failed command lookup
    logCommandExecutionPrepared(ctx, cmd, safeCommandForTrace, false, out);
  }
  // ===== END INLINED REGISTRY LOGIC =====

  // Log command output if this is an automation sub-command with logging active.
  if (automationLogActive() && cmdCtx && cmdCtx->automationName[0]) {
    const String safeAutomationOutput = redactOutputForLog(String(out));
    char logBuf[201];
    snprintf(logBuf, sizeof(logBuf), "%.197s%s",
             safeAutomationOutput.c_str(),
             safeAutomationOutput.length() > 197 ? "..." : "");
    for (char* c = logBuf; *c; c++) { if (*c == '\n' || *c == '\r') *c = ' '; }
    appendAutoLogEntry("OUTPUT", logBuf);
  }

  // Registry handlers use the uniform Error:/ERROR prefix contract. Reuse the
  // same result that drives command audit and status stamping so a denied
  // targeted login/logout is never recorded as an authorization success.
  {
    char auditBuf[180];
    snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s",
             safeCommandForTrace.c_str());
    logAuthAttempt(registrySuccess, ctx.path.c_str(), ctx.user, ctx.ip, auditBuf);
  }
  DEBUG_CMD_FLOWF("[execCmd] out_len=%zu", strlen(out));
  return registrySuccess;
}

// Queued command execution with deadlock avoidance
void releaseSyncExecReqOwner(ExecReq* request) {
  if (!request) return;

  const uint32_t previous =
      request->syncOwnerRefs.fetch_sub(1, std::memory_order_acq_rel);
  configASSERT(previous > 0);
  if (previous != 1) return;

  // The final acq_rel decrement observes the other owner's pre-release writes,
  // including syncTimedOut. Capture the diagnostic state, then finish cleanup
  // before logging; never expose the command text (it may contain credentials).
  const bool timedOut = request->syncTimedOut.load(std::memory_order_acquire);

  SemaphoreHandle_t done = request->done;
  request->done = nullptr;
  if (done) vSemaphoreDelete(done);
  request->~ExecReq();
  free(request);

  if (timedOut) {
    DEBUG_CMD_FLOWF("[submitSync] timed-out request cleanup complete");
  }
}

bool submitAndExecuteSync(const Command& cmd, String& out,
                          bool* completedOut) {
  if (completedOut) *completedOut = false;
  const size_t inputLength = cmd.line.length();
  if (!commandInputLengthAccepted(inputLength)) {
    if (inputLength == 0) {
      out = "Error: Empty command";
      broadcastOutput("[ERROR] Empty command");
    } else {
      out = "Error: Command exceeds input limit";
      DEBUG_CMD_FLOWF("[submitSync] rejected %lu-byte command (max=%lu)",
                      (unsigned long)inputLength,
                      (unsigned long)CMD_INPUT_MAX);
      broadcastOutput("[ERROR] Command exceeds input limit");
    }
    return false;
  }

  const String safeLineForTrace = redactCmdForAudit(cmd.line);
  DEBUG_CMD_FLOWF("[submitSync] cmd='%.80s' origin=%d user='%s'",
                  safeLineForTrace.c_str(), (int)cmd.ctx.origin, cmd.ctx.auth.user.c_str());

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
    // broadcast-output mask plumbing — per-task TLS now (Stage 3). Before the
    // executor exists this task is the executor, so it also sets the handler
    // validate mirror the way commandExecTask does.
    setCurrentCommandContext((void*)&cmd.ctx);
    const bool prevValidate = gCLIValidateOnly;
    gCLIValidateOnly = cmd.ctx.validateOnly;
    bool ok = executeCommand((AuthContext&)cmd.ctx.auth, cmd.line.c_str(), outBuf, CMD_RESULT_MAX);
    gCLIValidateOnly = prevValidate;
    clearCurrentCommandContext();
    out = outBuf;
    free(outBuf);
    if (completedOut) *completedOut = true;
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
  
  memcpy(r->line, cmd.line.c_str(), inputLength + 1);
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

  // Publish "the httpd task is blocked on cmd_exec_task" before the request
  // can become visible. cmd_exec may receive on another core immediately after
  // xQueueSend(), and a shutdown command must not observe a transient zero and
  // enter httpd_stop() while this handler is waiting (including for queue room).
#if ENABLE_HTTP_SERVER
  const char* selfTaskName = pcTaskGetName(nullptr);
  const bool waiterIsHttpd = (selfTaskName && strcmp(selfTaskName, "httpd") == 0);
  if (waiterIsHttpd) gWebCmdWaiters.fetch_add(1, std::memory_order_release);
#endif

  // Reserve both owners before publishing the pointer. cmd_exec_task can run
  // and complete as soon as xQueueSend succeeds, even before this task starts
  // waiting on the semaphore.
  r->syncOwnerRefs.store(2, std::memory_order_relaxed);
  CmdExecItem syncItem;
  syncItem.req = r;
  BaseType_t queueResult = xQueueSend(gCmdExecQ, &syncItem, pdMS_TO_TICKS(2000));
  
  if (queueResult != pdTRUE) {
#if ENABLE_HTTP_SERVER
    if (waiterIsHttpd) gWebCmdWaiters.fetch_sub(1, std::memory_order_acq_rel);
#endif
    DEBUG_CMD_FLOWF("[submitSync] queue full for '%.40s'", safeLineForTrace.c_str());
    vSemaphoreDelete(r->done);
    r->~ExecReq();
    free(r);
    broadcastOutput("[ERROR] Command queue full - try again");
    return false;
  }
  
  DEBUG_CMD_FLOWF("[submitSync] queued '%.40s' waiting...", safeLineForTrace.c_str());

  // Timeout bumped from 10s → 60s to cover PBKDF2 (~12 s) and any other
  // long-running synchronous command. Caller and executor retain independent
  // references, so either side may win at the exact timeout boundary without
  // orphaning or prematurely freeing the request/semaphore.
  const BaseType_t syncTaken = xSemaphoreTake(r->done, pdMS_TO_TICKS(60000));

#if ENABLE_HTTP_SERVER
  if (waiterIsHttpd) gWebCmdWaiters.fetch_sub(1, std::memory_order_acq_rel);
#endif

  if (syncTaken != pdTRUE) {
    r->syncTimedOut.store(true, std::memory_order_release);
    releaseSyncExecReqOwner(r);
    ERROR_COMMANDF("sync command timed out after 60000 ms; command execution may continue; line='%.60s'",
                   safeLineForTrace.c_str());
    out = "[ERROR] Command timed out";
    return false;
  }

  out = r->out;  // Copy from char array to String
  bool ok = r->ok;
  if (completedOut) *completedOut = true;

  releaseSyncExecReqOwner(r);

  DEBUG_CMD_FLOWF("[submitSync] done ok=%d len=%d", ok ? 1 : 0, out.length());
  return ok;
}

bool submitAndExecuteSync(const Command& cmd, String& out) {
  return submitAndExecuteSync(cmd, out, nullptr);
}

// Async command execution - fires and forgets, callback called on cmd_exec task
// Returns true if successfully queued, false on error
// Callback receives: (bool ok, const char* result, void* userData)
bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData) {
  const size_t inputLength = cmd.line.length();
  if (!commandInputLengthAccepted(inputLength)) {
    DEBUG_CMD_FLOWF("[submitAsync] ERROR: invalid command length=%lu max=%lu",
                    (unsigned long)inputLength,
                    (unsigned long)CMD_INPUT_MAX);
    return false;
  }

  const String safeLineForTrace = redactCmdForAudit(cmd.line);
  DEBUG_CMD_FLOWF("[submitAsync] enter: cmd.line='%s'", safeLineForTrace.c_str());
  
  if (gCmdExecQ == nullptr) {
    DEBUG_CMD_FLOWF("[submitAsync] ERROR: gCmdExecQ is NULL");
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
  memcpy(r->line, cmd.line.c_str(), inputLength + 1);
  r->ctx = cmd.ctx;
  r->done = nullptr;  // No semaphore - async mode
  r->asyncCallback = callback;
  r->asyncUserData = userData;
  r->ok = false;
  
  // Queue for execution
  CmdExecItem asyncItem;
  asyncItem.req = r;
  if (xQueueSend(gCmdExecQ, &asyncItem, 0) != pdTRUE) {
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
// Counts submissions lost because the command queue was full or memory was
// unavailable. This path drops silently by design (the caller is a time-critical
// radio callback that must not block), which made a dropped BLE OTA frame or
// checkpoint indistinguishable from a dead link. Surfaced via `otawrite status`.
namespace {
std::atomic<uint32_t> sCmdExecDropCount{0};
}

extern "C" void cmdExecDropCountIncrement(void) {
  // This counter carries no state; atomicity is the only ordering requirement.
  sCmdExecDropCount.fetch_add(1, std::memory_order_relaxed);
}

extern "C" uint32_t cmdExecDropCountLoad(void) {
  return sCmdExecDropCount.load(std::memory_order_relaxed);
}

// Returns true if successfully queued.
bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg) {
  if (!fn) return false;
  if (gCmdExecQ == nullptr) return false;

  // Allocates nothing. This used to take a whole ExecReq (6,384 B, from PSRAM)
  // to carry two pointers, on a path that reads neither line[] nor ctx nor
  // out[] -- ~8,700 times per BLE OTA image. The item now travels by value, so
  // this path cannot fail for want of memory and cannot fragment the heap,
  // which is also what lets it work on a board with no PSRAM at all.
  CmdExecItem item;
  item.deferredFn  = fn;
  item.deferredArg = arg;

  if (xQueueSend(gCmdExecQ, &item, 0) != pdTRUE) {
    cmdExecDropCountIncrement();
    return false;
  }
  return true;
}

// Run trusted firmware/boot actions as SYSTEM origin with logging.
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

bool currentCommandValidateOnly() {
  const CommandContext* ctx =
      static_cast<const CommandContext*>(currentCommandContext());
  return ctx && ctx->validateOnly;
}

// Status endpoints used to call executeCommand on the httpd task, where the
// handler's debug-buffer or static result could be overwritten by (or
// overwrite) the command cmd_exec was running. Submitting keeps one task
// running handlers; the result is copied out of the request, not a shared
// buffer.
bool executeInternalStatusCommand(const char* cmd, char* out, size_t outSize) {
  if (!cmd || !out || outSize == 0) return false;
  out[0] = '\0';
  Command uc;
  uc.line = cmd;
  uc.ctx.origin = ORIGIN_SYSTEM;
  uc.ctx.auth.transport = SOURCE_INTERNAL;
  uc.ctx.auth.user = "system";
  uc.ctx.auth.ip = "local";
  uc.ctx.auth.path = cmd;
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = uc.ctx.id;
  uc.ctx.outputMask = MSG_ROUTE_FILE;
  // A page poll is not an answer to anyone's interactive prompt.
  uc.ctx.behaviorFlags |= COMMAND_CONTEXT_MODE_INDEPENDENT;
  String result;
  const bool ok = submitAndExecuteSync(uc, result);
  snprintf(out, outSize, "%s", result.c_str());
  return ok;
}

CommandContext makeWebCommandContext(httpd_req_t* req, const AuthContext& auth) {
  CommandContext ctx;
  ctx.origin = ORIGIN_WEB;
  ctx.auth = auth;
  ctx.id = (uint32_t)millis();
  ctx.timestampMs = (uint32_t)millis();
  ctx.outputMask = MSG_ROUTE_WEB | MSG_ROUTE_FILE;
  // Cookie requests require a live session even if the caller's epoch capture
  // fails. Leave capture to the caller: a batch binds once for all its items.
  if (auth.sid.length()) {
    ctx.behaviorFlags |= COMMAND_CONTEXT_REQUIRE_LIVE_SESSION;
  }
  ctx.httpReq = req;
  return ctx;
}

// Helper used by web settings and other web endpoints to run a CLI-equivalent through unified path
bool executeUnifiedWebCommand(httpd_req_t* req, AuthContext& ctx, const String& cmd, String& out) {
  Command uc;
  uc.line = cmd;
  uc.ctx = makeWebCommandContext(req, ctx);
  uc.ctx.transportSessionEpoch = captureTransportSessionEpoch(ctx);
  // REST/settings facades are request/response machine calls, not keystrokes.
  // They must never be consumed as an answer to an outstanding prompt or
  // leave an interactive mode behind.
  uc.ctx.behaviorFlags |= COMMAND_CONTEXT_MODE_INDEPENDENT;
  bool ok = submitAndExecuteSync(uc, out);
  const bool webSessionStillLive =
      !ctx.sid.length() ||
      transportSessionEpochIsLive(SOURCE_WEB,
                                  uc.ctx.transportSessionEpoch);
  if (!webSessionStillLive) {
    out = "Error: web session changed before command result delivery.";
    return false;
  }
  if (!cliModeOwnedBySession(ctx.transport, uc.ctx.transportSessionEpoch)) {
    broadcastOutput(redactOutputForLog(out), uc.ctx);
  }
  return ok;
}

// ============================================================================
// Authentication Commands (critical system functions)
// ============================================================================

static bool parseSessionCommandTarget(const String& raw,
                                      CommandSource& sourceOut,
                                      String& canonicalOut) {
  canonicalOut = raw;
  canonicalOut.toLowerCase();
  if (canonicalOut == "serial") {
    sourceOut = SOURCE_SERIAL;
    return true;
  }
  if (canonicalOut == "uart") {
    sourceOut = SOURCE_UART;
    return true;
  }
  if (canonicalOut == "display") {
    sourceOut = SOURCE_LOCAL_DISPLAY;
    return true;
  }
  return false;
}

static bool sessionCommandTargetAvailable(CommandSource source) {
  if (source == SOURCE_SERIAL) return true;
  if (source == SOURCE_UART) return uartLinkIsRunning();
  if (source == SOURCE_LOCAL_DISPLAY) {
#if ENABLE_OLED_DISPLAY
    extern bool oledConnected;
    // gOledRunning is deliberately false while idle power-save has blanked a
    // still-connected panel.  Sleeping is not a transport teardown and must
    // not make its live session impossible to administer.
    return oledConnected;
#else
    return false;
#endif
  }
  return false;
}

const char* cmd_login(const String& originalCmd) {
  // Bare login is caller-relative. A supplied target is a distinct,
  // privileged operation and is authorized against the exact live source
  // session both in authorizeCommand() and again here for direct callers.
  CommandArgs a(originalCmd);
  if (a.unterminatedQuote() || a.count() < 2 || a.count() > 3) {
    return "Error: invalid arguments — Usage: login <username> <password> [serial|uart|display]";
  }

  String username = a.arg(0);
  String password = a.arg(1);
  const AuthContext& caller = currentAuthContext();
  const bool targeted = a.count() == 3;
  CommandSource transport = caller.transport;
  String transportStr;

  if (targeted) {
    const char* denial = nullptr;
    if (!callerMayTargetAnotherSession(caller, &denial)) {
      return denial ? denial : "Error: targeted session control denied.";
    }
    if (!parseSessionCommandTarget(a.arg(2), transport, transportStr)) {
      return "Error: Invalid transport. Use: serial, uart, or display";
    }
    if (transport == caller.transport) {
      return "Error: use bare login <username> <password> to replace this interface's own session.";
    }
    if (!sessionCommandTargetAvailable(transport)) {
      return "Error: target transport is not available.";
    }
  } else {
    const char* callerName = commandSourceSessionName(caller.transport);
    if (!callerName) {
      return "Error: use this interface's native login flow.";
    }
    transportStr = callerName;
  }

  // Validation must agree with execution about syntax and target policy, but
  // it must stop before lockout counters, password verification, or mutation.
  RETURN_VALID_IF_VALIDATE_CSTR();

  // A targeted login guesses from the caller's own console, so it counts in
  // that console's entry of the credential attempt table. A bare login is
  // throttled under the same entry inside loginTransport.
  const char* callerPeer = credentialAttemptConsolePeer(caller.transport);
  if (targeted) {
    unsigned long retryMs = 0;
    if (!credentialAttemptAllowed(caller.transport, callerPeer, &retryMs)) {
      EXT_RAM_BSS_ATTR static char throttleBuf[96];
      snprintf(throttleBuf, sizeof(throttleBuf),
               "Error: targeted login locked out. Retry in %lu seconds.",
               (retryMs + 999UL) / 1000UL);
      return throttleBuf;
    }
  }

  bool loginOk = false;
  if (targeted) {
    const CommandContext* commandCtx =
        static_cast<const CommandContext*>(currentCommandContext());
    loginOk = commandCtx && loginTransportFromNamedSession(
        transport, username, password, caller.transport, caller.user,
        commandCtx->transportSessionEpoch);
    credentialAttemptRecord(caller.transport, callerPeer, username.c_str(),
                            loginOk);
  } else {
    unsigned long lockoutRetryMs = 0;
    loginOk = loginTransport(transport, username, password, &lockoutRetryMs);
    if (!loginOk && lockoutRetryMs > 0) {
      EXT_RAM_BSS_ATTR static char lockedBuf[96];
      snprintf(lockedBuf, sizeof(lockedBuf),
               "Error: login locked out. Retry in %lu seconds.",
               (lockoutRetryMs + 999UL) / 1000UL);
      return lockedBuf;
    }
  }

  if (loginOk) {
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
  CommandArgs a(originalCmd);
  if (a.unterminatedQuote() || a.count() > 1) {
    return "Error: invalid arguments — Usage: logout [serial|uart|display]";
  }

  const AuthContext& caller = currentAuthContext();
  const bool targeted = a.count() == 1;
  CommandSource transport = caller.transport;
  String transportStr;
  if (targeted) {
    const char* denial = nullptr;
    if (!callerMayTargetAnotherSession(caller, &denial)) {
      return denial ? denial : "Error: targeted session control denied.";
    }
    if (!parseSessionCommandTarget(a.arg(0), transport, transportStr)) {
      return "Error: Invalid transport. Use: serial, uart, or display";
    }
    if (transport == caller.transport) {
      return "Error: use bare logout to end this interface's own session.";
    }
    if (!sessionCommandTargetAvailable(transport)) {
      return "Error: target transport is not available.";
    }
  } else {
    const char* callerName = commandSourceSessionName(caller.transport);
    if (!callerName) return "Error: use this interface's native logout flow.";
    transportStr = callerName;
  }

  RETURN_VALID_IF_VALIDATE_CSTR();

  if (targeted) {
    const CommandContext* commandCtx =
        static_cast<const CommandContext*>(currentCommandContext());
    const char* denial = nullptr;
    if (!commandCtx || !logoutTransportFromNamedSession(
            transport, caller.transport, caller.user,
            commandCtx->transportSessionEpoch, &denial)) {
      return denial ? denial : "Error: submitting session changed before logout.";
    }
  } else {
    logoutTransport(transport);
  }
  EXT_RAM_BSS_ATTR static char buf[64];
  snprintf(buf, sizeof(buf), "Logged out from %s", transportStr.c_str());
  return buf;
}

// ============================================================================
// Input Abstraction Layer - MOVED TO Input_HAL.cpp
// ============================================================================
// Implementation is now in Input_HAL.cpp for better modularity.
