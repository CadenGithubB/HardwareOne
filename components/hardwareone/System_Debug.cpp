#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>

#include "System_BuildConfig.h"
#include "OLED_ConsoleBuffer.h"
#include "Optional_EvenG2.h"
#include "System_CLI.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Logging.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"
#include "WebServer_Utils.h"

// External dependencies from .ino
extern Settings gSettings;

// ============================================================================
// Debug System Implementation
// ============================================================================

// Debug system globals - single source of truth
// All debug flags enabled by default for maximum verbosity (lower 32 bits)
// This includes ALL ESP-NOW debug flags:
//   - DEBUG_ESPNOW_CORE (0x10000)
//   - DEBUG_ESPNOW_ROUTER (0x80000)
//   - DEBUG_ESPNOW_MESH (0x100000)
//   - DEBUG_ESPNOW_TOPO (0x200000)
//   - DEBUG_ESPNOW_STREAM (0x400000)
//   - DEBUG_ESPNOW_ENCRYPTION (0x80000000)
// Individual sensor flags (upper 32 bits) disabled by default for cleaner output
uint64_t gDebugFlags = 0x00000000FFFFFFFFULL;
DebugSubFlags gDebugSubFlags = {}; // All sub-flags initialized to false
char* gDebugBuffer = nullptr;
QueueHandle_t gDebugOutputQueue = nullptr;
QueueHandle_t gDebugFreeQueue = nullptr;
volatile unsigned long gDebugDropped = 0;
int gDebugQueueSize = DEBUG_QUEUE_SIZE_MIN; // Runtime queue size (set in initDebugSystem)

volatile bool gDebugVerbose = false;

// Severity-based logging level (default: show everything)
uint8_t gLogLevel = LOG_LEVEL_DEBUG;

// System logging state
String gSystemLogPath = "";
bool gSystemLogEnabled = false;
unsigned long gSystemLogLastWrite = 0;
bool gSystemLogCategoryTags = true;  // Default: enabled

// Persistent file handle for efficient logging (avoids open/close per message)
static File gSystemLogFile;
static unsigned long gSystemLogLastFlush = 0;
static uint16_t gSystemLogUnflushedCount = 0;
static const uint16_t LOG_FLUSH_MESSAGE_COUNT = 20;      // Flush every 20 messages
static const uint32_t LOG_FLUSH_INTERVAL_MS = 5000;      // Or every 5 seconds

// Suppressed output during help (summary only)
static volatile unsigned long gHelpSuppressedCount = 0;

// G2 glasses output buffer (accumulates messages for periodic display)
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
static String gG2OutputBuffer;
static unsigned long gG2LastFlush = 0;
static const uint32_t G2_FLUSH_INTERVAL_MS = 2000;  // Send to glasses every 2 seconds
static const size_t G2_BUFFER_MAX = 500;            // Max chars to buffer
#endif


// External dependencies from main .ino
extern bool gCLIValidateOnly;
extern volatile bool gInHelpRender;

// Settings and persistence (defined in settings.h and main .ino)
extern Settings gSettings;

// Web mirror buffer access - defined in WebServer_Utils.h

// Suppressed tail ring buffer
static const size_t kHelpTailLines = 32;
static const size_t kHelpTailCols = 120;
static char gHelpTail[kHelpTailLines][kHelpTailCols];
static size_t gHelpTailCount = 0;
static size_t gHelpTailIndex = 0;

static void pushHelpSuppressed(const char* t) {
  if (!t) return;
  size_t i = gHelpTailIndex % kHelpTailLines;
  strncpy(gHelpTail[i], t, kHelpTailCols - 1);
  gHelpTail[i][kHelpTailCols - 1] = '\0';
  gHelpTailIndex++;
  if (gHelpTailCount < kHelpTailLines) gHelpTailCount++;
}

void helpSuppressedTailDump() {
  unsigned long totalSuppressed = gHelpSuppressedCount;
  
  if (gHelpTailCount == 0) {
    if (totalSuppressed > 0) {
      broadcastOutput("(Note) Suppressed output tail is empty (tail buffer overflow or no recent messages).");
    } else {
      broadcastOutput("(Note) No suppressed output during this help session.");
    }
    return;
  }
  
  // Show header with count info
  char header[128];
  if (totalSuppressed > kHelpTailLines) {
    snprintf(header, sizeof(header), 
             "════════ Suppressed Output Tail (showing last %zu of %lu lines) ════════",
             gHelpTailCount, totalSuppressed);
  } else {
    snprintf(header, sizeof(header), 
             "════════ Suppressed Output Tail (%zu lines) ════════",
             gHelpTailCount);
  }
  broadcastOutput(header);
  
  // Dump tail buffer
  size_t start = (gHelpTailIndex >= gHelpTailCount) ? (gHelpTailIndex - gHelpTailCount) : 0;
  for (size_t n = 0; n < gHelpTailCount; n++) {
    size_t idx = (start + n) % kHelpTailLines;
    broadcastOutput(gHelpTail[idx]);
  }
  
  broadcastOutput("═══════════════════════════════════════════════════════════════");
}

// ============================================================================
// Initialization
// ============================================================================

// Debug output task - single writer for all debug messages
static TaskHandle_t gDebugOutputTaskHandle = nullptr;

void debugOutputTask(void* parameter) {
  while (true) {
    DebugMessage* msg = nullptr;
    if (xQueueReceive(gDebugOutputQueue, &msg, portMAX_DELAY) == pdTRUE && msg) {
      // Help-mode gating for queued debug messages (allow security/auth)
      if (gCLIState != CLI_NORMAL && !gInHelpRender) {
        if ((msg->flags & DEBUG_MSG_FLAG_ALLOW_IN_HELP) == 0 &&
            !(strncmp(msg->text, "[SECURITY]", 10) == 0 || strncmp(msg->text, "[AUTH]", 6) == 0)) {
          gHelpSuppressedCount++;
          pushHelpSuppressed(msg->text);
          if (gDebugFreeQueue) {
            xQueueSend(gDebugFreeQueue, &msg, 0);
          }
          continue; // Drop from sinks to avoid overwriting help UI
        }
      }
      // Single point of output - no concurrency issues
      if (gOutputFlags & OUTPUT_SERIAL) {
        Serial.printf("[%lu] %s\n", msg->timestamp, msg->text);
      }
      // Append to web mirror buffer using circular buffer logic (only if web output enabled)
      if ((gOutputFlags & OUTPUT_WEB) && gWebMirror.buf) {
        // Format message with timestamp (stack buffer - zero heap allocation)
        char formattedMsg[DEBUG_MSG_SIZE + 32];
        int written = snprintf(formattedMsg, sizeof(formattedMsg), "[%lu] %s", msg->timestamp, msg->text);
        if (written > 0) {
          // Use appendDirect() with pre-calculated length - zero String churn
          gWebMirror.appendDirect(formattedMsg, (size_t)written, true);
        }
      }
      // File output (system log) - optimized with persistent file handle
      if ((gOutputFlags & OUTPUT_FILE) && gSystemLogEnabled && gSystemLogPath.length() > 0) {
        fsLock("debug.log");
        
        // Open file if not already open (once per logging session)
        if (!gSystemLogFile) {
          gSystemLogFile = LittleFS.open(gSystemLogPath.c_str(), "a");
          if (gSystemLogFile) {
            gSystemLogLastFlush = millis();
            gSystemLogUnflushedCount = 0;
          }
        }
        
        if (gSystemLogFile) {
          // Write directly to file (no intermediate buffer needed)
          if (gSystemLogCategoryTags && msg->flags != 0) {
            const char* category = getDebugCategoryName(msg->flags);
            gSystemLogFile.printf("[%lu] [%s] %s\n", msg->timestamp, category, msg->text);
          } else {
            gSystemLogFile.printf("[%lu] %s\n", msg->timestamp, msg->text);
          }
          
          gSystemLogLastWrite = millis();
          gSystemLogUnflushedCount++;
          
          // Periodic flush (balances performance vs data safety)
          bool shouldFlush = 
            (gSystemLogUnflushedCount >= LOG_FLUSH_MESSAGE_COUNT) ||
            ((millis() - gSystemLogLastFlush) >= LOG_FLUSH_INTERVAL_MS);
          
          if (shouldFlush) {
            gSystemLogFile.flush();
            gSystemLogLastFlush = millis();
            gSystemLogUnflushedCount = 0;
          }
        }
        
        fsUnlock();
      }
      
      // Append to OLED console buffer (always, independent of OUTPUT_* flags)
      #if ENABLE_OLED_DISPLAY
      if (gOLEDConsole.mutex) {
        gOLEDConsole.append(msg->text, msg->timestamp);
      }
      #endif
      
      // G2 glasses output - buffer messages and flush periodically
      #if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
      if ((gOutputFlags & OUTPUT_G2) && isG2Connected()) {
        // Append to buffer (with newline)
        if (gG2OutputBuffer.length() + strlen(msg->text) + 2 < G2_BUFFER_MAX) {
          gG2OutputBuffer += msg->text;
          gG2OutputBuffer += "\n";
        }
        // Flush if buffer full or interval elapsed
        unsigned long now = millis();
        if (gG2OutputBuffer.length() >= G2_BUFFER_MAX - 50 || 
            (now - gG2LastFlush >= G2_FLUSH_INTERVAL_MS && gG2OutputBuffer.length() > 0)) {
          g2ShowText(gG2OutputBuffer.c_str());
          gG2OutputBuffer = "";
          gG2LastFlush = now;
        }
      }
      #endif
      
      // Return message to pool
      if (gDebugFreeQueue) {
        xQueueSend(gDebugFreeQueue, &msg, 0);
      }
    }
  }
}

const char* cmd_loglevel(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  String valStr = args;
  valStr.trim();
  valStr.toLowerCase();

  if (valStr.length() == 0) {
    const char* levelName = "unknown";
    int cur = gSettings.logLevel;
    if (cur < LOG_LEVEL_ERROR) cur = LOG_LEVEL_ERROR;
    if (cur > LOG_LEVEL_DEBUG) cur = LOG_LEVEL_DEBUG;
    switch (cur) {
      case LOG_LEVEL_ERROR: levelName = "error"; break;
      case LOG_LEVEL_WARN: levelName = "warn"; break;
      case LOG_LEVEL_INFO: levelName = "info"; break;
      case LOG_LEVEL_DEBUG: levelName = "debug"; break;
    }
    snprintf(gDebugBuffer, 1024, "Current log level: %s (%d) (0=error, 1=warn, 2=info, 3=debug)", levelName, cur);
    return gDebugBuffer;
  }

  int newLevel = gSettings.logLevel;
  if (valStr == "error" || valStr == "e" || valStr == "0") {
    newLevel = LOG_LEVEL_ERROR;
  } else if (valStr == "warn" || valStr == "warning" || valStr == "w" || valStr == "1") {
    newLevel = LOG_LEVEL_WARN;
  } else if (valStr == "info" || valStr == "i" || valStr == "2") {
    newLevel = LOG_LEVEL_INFO;
  } else if (valStr == "debug" || valStr == "d" || valStr == "3") {
    newLevel = LOG_LEVEL_DEBUG;
  } else {
    return "Invalid level. Use: error(0), warn(1), info(2), or debug(3)";
  }

  if (newLevel < LOG_LEVEL_ERROR) newLevel = LOG_LEVEL_ERROR;
  if (newLevel > LOG_LEVEL_DEBUG) newLevel = LOG_LEVEL_DEBUG;

  setSetting(gSettings.logLevel, newLevel);
  DEBUG_MANAGER.setLogLevel((uint8_t)newLevel);

  const char* levelName = "unknown";
  switch (gSettings.logLevel) {
    case LOG_LEVEL_ERROR: levelName = "error"; break;
    case LOG_LEVEL_WARN: levelName = "warn"; break;
    case LOG_LEVEL_INFO: levelName = "info"; break;
    case LOG_LEVEL_DEBUG: levelName = "debug"; break;
  }

  snprintf(gDebugBuffer, 1024, "Log level set to: %s (%d) and saved", levelName, (int)gSettings.logLevel);
  return gDebugBuffer;
}

void initDebugSystem() {
  // Ensure the DebugManager singleton is constructed early.
  // IMPORTANT: Do not call DebugManager::initialize() here (it delegates back to initDebugSystem()).
  (void)DebugManager::getInstance();

  // Determine queue size based on PSRAM availability
  // PSRAM available: 128 slots in PSRAM, otherwise 64 slots in internal RAM
  size_t psramSize = ESP.getPsramSize();
  bool hasPsram = (psramSize > 0);
  gDebugQueueSize = hasPsram ? DEBUG_QUEUE_SIZE_MAX : DEBUG_QUEUE_SIZE_MIN;
  
  if (gOutputFlags & OUTPUT_SERIAL) {
    Serial.printf("[DEBUG] Queue size: %d slots (%s)\n", gDebugQueueSize, 
                  hasPsram ? "PSRAM" : "internal RAM");
  }

  // Allocate debug buffer in PSRAM
  if (!gDebugBuffer) {
    gDebugBuffer = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "debug.buf");
    if (!gDebugBuffer) {
      if (gOutputFlags & OUTPUT_SERIAL) {
        Serial.println("FATAL: Failed to allocate debug buffer");
      }
      while (1) delay(1000);
    }
  }

  // Create debug free queue (pool of reusable DebugMessage pointers)
  if (!gDebugFreeQueue) {
    gDebugFreeQueue = xQueueCreate(gDebugQueueSize, sizeof(DebugMessage*));
    if (!gDebugFreeQueue) {
      if (gOutputFlags & OUTPUT_SERIAL) {
        Serial.println("FATAL: Failed to create debug free queue");
      }
      while (1) delay(1000);
    }

    // Pre-allocate the pool itself (prefer PSRAM if available)
    AllocPref allocPref = hasPsram ? AllocPref::PreferPSRAM : AllocPref::PreferInternal;
    DebugMessage* pool = (DebugMessage*)ps_alloc(gDebugQueueSize * sizeof(DebugMessage), allocPref, "debug.pool");
    if (!pool) {
      if (gOutputFlags & OUTPUT_SERIAL) {
        Serial.println("FATAL: Failed to allocate debug message pool");
      }
      while (1) delay(1000);
    }

    // Seed free queue with pointers into the pool
    for (int i = 0; i < gDebugQueueSize; ++i) {
      DebugMessage* p = &pool[i];
      xQueueSend(gDebugFreeQueue, &p, 0);
    }
  }

  // Create debug output queue (stores pointers to heap-allocated messages)
  if (!gDebugOutputQueue) {
    gDebugOutputQueue = xQueueCreate(gDebugQueueSize, sizeof(DebugMessage*));
    if (!gDebugOutputQueue) {
      if (gOutputFlags & OUTPUT_SERIAL) {
        Serial.println("FATAL: Failed to create debug output queue");
      }
      while (1) delay(1000);
    }
    DEBUG_SYSTEMF("Debug output queue created (%d messages in %s)", gDebugQueueSize,
                  hasPsram ? "PSRAM" : "internal RAM");
  }

  // Create debug output task
  if (!gDebugOutputTaskHandle) {
    BaseType_t result = xTaskCreate(
      debugOutputTask,
      "debug_out",
      DEBUG_OUT_STACK_WORDS,  // ~12KB stack (reduced from 16KB - peak usage 8KB)
      nullptr,
      1,     // Low priority
      &gDebugOutputTaskHandle
    );
    if (result != pdPASS) {
      if (gOutputFlags & OUTPUT_SERIAL) {
        Serial.println("FATAL: Failed to create debug output task");
      }
      while (1) delay(1000);
    }
    DEBUG_SYSTEMF("Debug output task created");
  }

  // NOTE: Do NOT reset gDebugFlags here - applySettings() may have already set them
  // The flags are managed by applySettings() in settings.cpp
  
  // Initialize OLED console buffer
  #if ENABLE_OLED_DISPLAY
  gOLEDConsole.init();
  #endif
  
  // Initialize web mirror buffer for CLI history
  if (!gWebMirror.buf && gWebMirrorCap > 0) {
    gWebMirror.init(gWebMirrorCap);
    if (gWebMirror.buf) {
      DEBUG_SYSTEMF("Web mirror buffer allocated (%u bytes)", (unsigned)gWebMirrorCap);
    } else {
      if (gOutputFlags & OUTPUT_SERIAL) {
        Serial.println("WARNING: Failed to allocate web mirror buffer - web CLI will be empty");
      }
    }
  }
  
  DEBUG_SYSTEMF("Debug system initialized");
}

// ============================================================================
// Buffer Management
// ============================================================================

bool ensureDebugBuffer() {
  if (!gDebugBuffer) {
    gDebugBuffer = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "debug.buf");
    if (!gDebugBuffer) {
      if (gOutputFlags & OUTPUT_SERIAL) {
        Serial.println("ERROR: Failed to allocate debug buffer");
      }
      return false;
    }
  }
  return true;
}

// ============================================================================
// Legacy function - no longer needed with queue-based system
// ============================================================================

void drainDebugRing() {
  // No-op: Debug output task handles all output automatically
}

void debugQueuePrintf(uint32_t flag, const char* fmt, ...) {
  if (!fmt) return;
  if (!getDebugQueue() || !getDebugFreeQueue()) return;

  // CRITICAL: Check if we're in a sensor task that's shutting down
  extern bool thermalEnabled, imuEnabled, tofEnabled, fmRadioEnabled;
  extern TaskHandle_t thermalTaskHandle, imuTaskHandle, tofTaskHandle, fmRadioTaskHandle;
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  if (currentTask == thermalTaskHandle && !thermalEnabled) return;
  if (currentTask == imuTaskHandle && !imuEnabled) return;
  if (currentTask == tofTaskHandle && !tofEnabled) return;
  if (currentTask == fmRadioTaskHandle && !fmRadioEnabled) return;

  DebugMessage* msg = nullptr;
  BaseType_t got = xPortInIsrContext() ?
    xQueueReceiveFromISR(getDebugFreeQueue(), &msg, NULL) :
    xQueueReceive(getDebugFreeQueue(), &msg, 0);

  if (got != pdTRUE || !msg) {
    incrementDebugDropped();
    return;
  }

  msg->timestamp = millis();
  msg->flags = flag;

  va_list args;
  va_start(args, fmt);
  vsnprintf(msg->text, DEBUG_MSG_SIZE, fmt, args);
  va_end(args);
  msg->text[DEBUG_MSG_SIZE - 1] = '\0';

  BaseType_t result = xPortInIsrContext() ?
    xQueueSendFromISR(getDebugQueue(), &msg, NULL) :
    xQueueSend(getDebugQueue(), &msg, 0);

  if (result != pdTRUE) {
    if (xPortInIsrContext()) {
      xQueueSendFromISR(getDebugFreeQueue(), &msg, NULL);
    } else {
      xQueueSend(getDebugFreeQueue(), &msg, 0);
    }
    incrementDebugDropped();
  }
}

// ============================================================================
// Broadcast Output Functions
// ============================================================================

// Helper: Print to web buffer
void printToWeb(const String& s) {
  if (!gWebMirror.buf) return;
  if (gWebMirror.len + s.length() + 1 >= gWebMirror.cap) return;
  strcpy(gWebMirror.buf + gWebMirror.len, s.c_str());
  gWebMirror.len += s.length();
  gWebMirror.buf[gWebMirror.len++] = '\n';
  gWebMirror.buf[gWebMirror.len] = '\0';
}

// Broadcast output - String overload (now uses queue)
void broadcastOutput(const String& s) {
  // Suppress output in validation mode
  if (gCLIValidateOnly) {
    return;
  }
  
  // Help-mode gating: drop non-help-render output while help UI is active,
  // but allow security/auth notices to pass through
  if (gCLIState != CLI_NORMAL && !gInHelpRender) {
    if (!(s.startsWith("[SECURITY]") || s.startsWith("[AUTH]"))) {
      gHelpSuppressedCount++;
      pushHelpSuppressed(s.c_str());
      return;
    }
  }
  
  // CRITICAL: Check if we're in a sensor task that's shutting down
  extern bool thermalEnabled, imuEnabled, tofEnabled;
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  extern TaskHandle_t thermalTaskHandle, imuTaskHandle, tofTaskHandle;
  
  // Skip output if current task is a sensor task that's been disabled
  if (currentTask == thermalTaskHandle && !thermalEnabled) return;
  if (currentTask == imuTaskHandle && !imuEnabled) return;
  if (currentTask == tofTaskHandle && !tofEnabled) return;
  
  if (gDebugOutputQueue) {
    DebugMessage* msg = nullptr;
    if (gDebugFreeQueue && xQueueReceive(gDebugFreeQueue, &msg, 0) == pdTRUE && msg) {
      msg->timestamp = millis();
      msg->flags = gInHelpRender ? DEBUG_MSG_FLAG_ALLOW_IN_HELP : 0;
      strncpy(msg->text, s.c_str(), DEBUG_MSG_SIZE - 1);
      msg->text[DEBUG_MSG_SIZE - 1] = '\0';
      if (xQueueSend(gDebugOutputQueue, &msg, 0) != pdTRUE) {
        xQueueSend(gDebugFreeQueue, &msg, 0);
        gDebugDropped++;
      }
    } else {
      gDebugDropped++;
    }
  }
  

  // ESP-NOW V3 session streaming (extern to avoid circular deps)
#if ENABLE_ESPNOW
  extern uint32_t gCurrentStreamCmdId;
  extern void sendEspNowStreamMessage(const String& message);
  if (gCurrentStreamCmdId != 0) {
    sendEspNowStreamMessage(s);
  }
#endif
}

// Broadcast output - const char* overload (now uses queue)
void broadcastOutput(const char* s) {
  if (!s) return;
  
  // Suppress output in validation mode
  if (gCLIValidateOnly) {
    return;
  }

  // Help-mode gating: drop non-help-render output while help UI is active,
  // but allow security/auth notices to pass through
  if (gCLIState != CLI_NORMAL && !gInHelpRender) {
    if (!(strncmp(s, "[SECURITY]", 10) == 0 || strncmp(s, "[AUTH]", 6) == 0)) {
      gHelpSuppressedCount++;
      pushHelpSuppressed(s);
      return;
    }
  }

  // CRITICAL: Check if we're in a sensor task that's shutting down
  extern bool thermalEnabled, imuEnabled, tofEnabled;
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  extern TaskHandle_t thermalTaskHandle, imuTaskHandle, tofTaskHandle;
  
  // Skip output if current task is a sensor task that's been disabled
  if (currentTask == thermalTaskHandle && !thermalEnabled) return;
  if (currentTask == imuTaskHandle && !imuEnabled) return;
  if (currentTask == tofTaskHandle && !tofEnabled) return;

  if (gDebugOutputQueue) {
    DebugMessage* msg = nullptr;
    if (gDebugFreeQueue && xQueueReceive(gDebugFreeQueue, &msg, 0) == pdTRUE && msg) {
      msg->timestamp = millis();
      msg->flags = gInHelpRender ? DEBUG_MSG_FLAG_ALLOW_IN_HELP : 0;
      strncpy(msg->text, s, DEBUG_MSG_SIZE - 1);
      msg->text[DEBUG_MSG_SIZE - 1] = '\0';
      if (xQueueSend(gDebugOutputQueue, &msg, 0) != pdTRUE) {
        xQueueSend(gDebugFreeQueue, &msg, 0);
        gDebugDropped++;
      }
    } else {
      gDebugDropped++;
    }
  }
  

  // ESP-NOW V3 session streaming (extern to avoid circular deps)
#if ENABLE_ESPNOW
  extern uint32_t gCurrentStreamCmdId;
  extern void sendEspNowStreamMessage(const String& message);
  if (gCurrentStreamCmdId != 0) {
    sendEspNowStreamMessage(String(s));
  }
#endif
}

// Print summary (and tail) of output suppressed during help; resets counters
void helpSuppressedPrintAndReset() {
  unsigned long n = gHelpSuppressedCount;
  if (n > 0) {
    // Minimal one-line notice to keep UI clean
    char msg[96];
    snprintf(msg, sizeof(msg), "(Note) Suppressed %lu lines during help.", (unsigned long)n);
    broadcastOutput(msg);
    gHelpSuppressedCount = 0;
  }
}

// ==========================================================================
// Streaming Debug Instrumentation (centralized implementation)
// ==========================================================================
static bool gStreamHitMaxChunk = false;
static size_t gStreamMaxChunk = 0;
static size_t gStreamTotalBytes = 0;
static String gStreamTag = "";

void streamDebugReset(const char* tag) {
  gStreamHitMaxChunk = false;
  gStreamMaxChunk = 0;
  gStreamTotalBytes = 0;
  gStreamTag = tag ? String(tag) : String("");
}

void streamDebugRecord(size_t sz, size_t chunkLimit) {
  if (sz > gStreamMaxChunk) gStreamMaxChunk = sz;
  gStreamTotalBytes += sz;
  if (sz >= chunkLimit) gStreamHitMaxChunk = true;
}

void streamDebugFlush() {
  // One-line summary per response
  DEBUG_HTTPF("page=%s total=%uB maxChunk=%uB hitMax=%s buf=5119B",
              gStreamTag.c_str(), (unsigned)gStreamTotalBytes, (unsigned)gStreamMaxChunk,
              gStreamHitMaxChunk ? "yes" : "no");
}

// ============================================================================
// Debug Command Handlers
// ============================================================================

const char* cmd_outdisplay(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Syntax:
  //   outdisplay <0|1> [persist|temp]
  //   outdisplay [persist|temp] <0|1>
  String a = args;
  a.trim();
  String t1, t2;
  int sp2 = a.indexOf(' ');
  if (sp2 >= 0) {
    t1 = a.substring(0, sp2);
    t2 = a.substring(sp2 + 1);
    t2.trim();
  } else {
    t1 = a;
  }
  t1.trim();
  bool modeTemp = false;  // default persist
  int v = -1;
  auto parse01 = [](const String& s) -> int {
    String ss = s;
    ss.trim();
    return ss.toInt();
  };
  if (t1.length() && (t1 == "temp" || t1 == "persist")) {
    modeTemp = (t1 == "temp");
    if (t2.length()) v = parse01(t2);
  } else {
    if (t1.length()) v = parse01(t1);
    if (t2.length()) { modeTemp = (t2 == "temp"); }
  }
  if (v != 0) v = 1;
  if (v < 0) return "Usage: outdisplay <0|1> [persist|temp]";
  if (modeTemp) {
    if (v) gOutputFlags |= OUTPUT_DISPLAY;
    else gOutputFlags &= ~OUTPUT_DISPLAY;
    return v ? "outDisplay (runtime) set to 1" : "outDisplay (runtime) set to 0";
  } else {
    setSetting(gSettings.outDisplay, (bool)(v != 0));
    if (v) gOutputFlags |= OUTPUT_DISPLAY;
    else gOutputFlags &= ~OUTPUT_DISPLAY;
    return gSettings.outDisplay ? "outDisplay (persisted) set to 1" : "outDisplay (persisted) set to 0";
  }
}

const char* cmd_outg2(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  #if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  // Syntax: outg2 <0|1>
  String a = args;
  a.trim();
  int v = -1;
  if (a.length()) {
    v = a.toInt();
    if (a != "0" && v == 0) v = -1;  // Invalid input
  }
  if (v < 0) {
    // Show current status
    bool enabled = (gOutputFlags & OUTPUT_G2) != 0;
    bool connected = isG2Connected();
    static char buf[128];
    snprintf(buf, sizeof(buf), "G2 output: %s, G2 connected: %s", 
             enabled ? "ON" : "OFF", connected ? "yes" : "no");
    return buf;
  }
  if (v) {
    gOutputFlags |= OUTPUT_G2;
    return "G2 output enabled (messages will stream to glasses when connected)";
  } else {
    gOutputFlags &= ~OUTPUT_G2;
    return "G2 output disabled";
  }
  #else
  return "G2 glasses support not compiled (ENABLE_G2_GLASSES=0)";
  #endif
}

const char* cmd_debughttp(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_HTTP);
    else clearDebugFlag(DEBUG_HTTP);
    return v ? "debugHttp enabled (runtime only)" : "debugHttp disabled (runtime only)";
  } else {
    setSetting(gSettings.debugHttp, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_HTTP);
    else clearDebugFlag(DEBUG_HTTP);
    return gSettings.debugHttp ? "debugHttp enabled (persistent)" : "debugHttp disabled (persistent)";
  }
}

const char* cmd_debugsse(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_SSE);
    else clearDebugFlag(DEBUG_SSE);
    return v ? "debugSse enabled (runtime only)" : "debugSse disabled (runtime only)";
  } else {
    setSetting(gSettings.debugSse, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_SSE);
    else clearDebugFlag(DEBUG_SSE);
    return gSettings.debugSse ? "debugSse enabled (persistent)" : "debugSse disabled (persistent)";
  }
}

const char* cmd_debugcli(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_CLI);
    else clearDebugFlag(DEBUG_CLI);
    return v ? "debugCli enabled (runtime only)" : "debugCli disabled (runtime only)";
  } else {
    setSetting(gSettings.debugCli, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_CLI);
    else clearDebugFlag(DEBUG_CLI);
    return gSettings.debugCli ? "debugCli enabled (persistent)" : "debugCli disabled (persistent)";
  }
}

const char* cmd_debugsensorsgeneral(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_SENSORS);
    else clearDebugFlag(DEBUG_SENSORS);
    return v ? "debugSensorsGeneral enabled (runtime only)" : "debugSensorsGeneral disabled (runtime only)";
  } else {
    setSetting(gSettings.debugSensorsGeneral, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_SENSORS);
    else clearDebugFlag(DEBUG_SENSORS);
    return gSettings.debugSensorsGeneral ? "debugSensorsGeneral enabled (persistent)" : "debugSensorsGeneral disabled (persistent)";
  }
}

const char* cmd_debugcamera(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_CAMERA);
    else clearDebugFlag(DEBUG_CAMERA);
    return v ? "debugCamera enabled (runtime only)" : "debugCamera disabled (runtime only)";
  } else {
    setSetting(gSettings.debugCamera, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_CAMERA);
    else clearDebugFlag(DEBUG_CAMERA);
    return gSettings.debugCamera ? "debugCamera enabled (persistent)" : "debugCamera disabled (persistent)";
  }
}

const char* cmd_debugmicrophone(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_MICROPHONE);
    else clearDebugFlag(DEBUG_MICROPHONE);
    return v ? "debugMicrophone enabled (runtime only)" : "debugMicrophone disabled (runtime only)";
  } else {
    setSetting(gSettings.debugMicrophone, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_MICROPHONE);
    else clearDebugFlag(DEBUG_MICROPHONE);
    return gSettings.debugMicrophone ? "debugMicrophone enabled (persistent)" : "debugMicrophone disabled (persistent)";
  }
}

const char* cmd_debugi2c(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_I2C);
    else clearDebugFlag(DEBUG_I2C);
    return v ? "debugI2C enabled (runtime only)" : "debugI2C disabled (runtime only)";
  } else {
    setSetting(gSettings.debugI2C, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_I2C);
    else clearDebugFlag(DEBUG_I2C);
    return gSettings.debugI2C ? "debugI2C enabled (persistent)" : "debugI2C disabled (persistent)";
  }
}

const char* cmd_debugwifi(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_WIFI);
    else clearDebugFlag(DEBUG_WIFI);
    return v ? "debugWifi enabled (runtime only)" : "debugWifi disabled (runtime only)";
  } else {
    setSetting(gSettings.debugWifi, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_WIFI);
    else clearDebugFlag(DEBUG_WIFI);
    return gSettings.debugWifi ? "debugWifi enabled (persistent)" : "debugWifi disabled (persistent)";
  }
}

const char* cmd_debugstorage(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_STORAGE);
    else clearDebugFlag(DEBUG_STORAGE);
    return v ? "debugStorage enabled (runtime only)" : "debugStorage disabled (runtime only)";
  } else {
    setSetting(gSettings.debugStorage, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_STORAGE);
    else clearDebugFlag(DEBUG_STORAGE);
    return gSettings.debugStorage ? "debugStorage enabled (persistent)" : "debugStorage disabled (persistent)";
  }
}

const char* cmd_debuglogger(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_LOGGER);
    else clearDebugFlag(DEBUG_LOGGER);
    return v ? "debugLogger enabled (runtime only)" : "debugLogger disabled (runtime only)";
  } else {
    setSetting(gSettings.debugLogger, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_LOGGER);
    else clearDebugFlag(DEBUG_LOGGER);
    return gSettings.debugLogger ? "debugLogger enabled (persistent)" : "debugLogger disabled (persistent)";
  }
}

const char* cmd_debugautomations(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTOMATIONS);
    else clearDebugFlag(DEBUG_AUTOMATIONS);
    return v ? "debugAutomations enabled (runtime only)" : "debugAutomations disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAutomations, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_AUTOMATIONS);
    else clearDebugFlag(DEBUG_AUTOMATIONS);
    return gSettings.debugAutomations ? "debugAutomations enabled (persistent)" : "debugAutomations disabled (persistent)";
  }
}

const char* cmd_debugperformance(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_PERFORMANCE);
    else clearDebugFlag(DEBUG_PERFORMANCE);
    return v ? "debugPerformance enabled (runtime only)" : "debugPerformance disabled (runtime only)";
  } else {
    setSetting(gSettings.debugPerformance, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_PERFORMANCE);
    else clearDebugFlag(DEBUG_PERFORMANCE);
    return gSettings.debugPerformance ? "debugPerformance enabled (persistent)" : "debugPerformance disabled (persistent)";
  }
}

const char* cmd_debugauth(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTH);
    else clearDebugFlag(DEBUG_AUTH);
    return v ? "debugAuth enabled (runtime only)" : "debugAuth disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAuth, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_AUTH);
    else clearDebugFlag(DEBUG_AUTH);
    return gSettings.debugAuth ? "debugAuth enabled (persistent)" : "debugAuth disabled (persistent)";
  }
}

const char* cmd_debugsensors(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_SENSORS);
    else clearDebugFlag(DEBUG_SENSORS);
    return v ? "debugSensors enabled (runtime only)" : "debugSensors disabled (runtime only)";
  } else {
    setSetting(gSettings.debugSensors, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_SENSORS);
    else clearDebugFlag(DEBUG_SENSORS);
    return gSettings.debugSensors ? "debugSensors enabled (persistent)" : "debugSensors disabled (persistent)";
  }
}

const char* cmd_debugespnow(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_CORE);
    else clearDebugFlag(DEBUG_ESPNOW_CORE);
    return v ? "debugEspNow enabled (runtime only)" : "debugEspNow disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNow, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_ESPNOW_CORE);
    else clearDebugFlag(DEBUG_ESPNOW_CORE);
    return gSettings.debugEspNow ? "debugEspNow enabled (persistent)" : "debugEspNow disabled (persistent)";
  }
}

const char* cmd_debugdatetime(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_SYSTEM);  // DateTime maps to SYSTEM flag
    else clearDebugFlag(DEBUG_SYSTEM);
    return v ? "debugDateTime enabled (runtime only)" : "debugDateTime disabled (runtime only)";
  } else {
    setSetting(gSettings.debugDateTime, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_SYSTEM);
    else clearDebugFlag(DEBUG_SYSTEM);
    return gSettings.debugDateTime ? "debugDateTime enabled (persistent)" : "debugDateTime disabled (persistent)";
  }
}

const char* cmd_debugverbose(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return gDebugVerbose ? "debugVerbose is ON" : "debugVerbose is OFF";
  int v = valStr.toInt();
  if (v != 0 && v != 1) return "Usage: debugverbose <0|1>";
  gDebugVerbose = (v == 1);
  return gDebugVerbose ? "debugVerbose enabled" : "debugVerbose disabled";
}

const char* cmd_debugcommandsystem(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (v != 0 && v != 1) {
    return "Usage: debugcommandsystem <0|1> [temp|runtime]";
  }
  if (!modeTemp) {
    setSetting(gSettings.debugCommandSystem, (bool)(v != 0));
  }
  if (v) setDebugFlag(DEBUG_COMMAND_SYSTEM);
  else clearDebugFlag(DEBUG_COMMAND_SYSTEM);
  if (modeTemp) {
    return v ? "debugCommandSystem enabled (runtime only)" : "debugCommandSystem disabled (runtime only)";
  } else {
    return gSettings.debugCommandSystem ? "debugCommandSystem enabled (persistent)" : "debugCommandSystem disabled (persistent)";
  }
}

const char* cmd_debugsettingssystem(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  // Only "temp" or "runtime" triggers temporary mode; anything else (including empty) persists
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (v != 0 && v != 1) {
    return "Usage: debugsettingssystem <0|1> [temp|runtime]";
  }
  if (!modeTemp) {
    setSetting(gSettings.debugSettingsSystem, (bool)(v != 0));
  }
  if (v) setDebugFlag(DEBUG_SETTINGS_SYSTEM);
  else clearDebugFlag(DEBUG_SETTINGS_SYSTEM);
  if (modeTemp) {
    return v ? "debugSettingsSystem enabled (runtime only)" : "debugSettingsSystem disabled (runtime only)";
  } else {
    return gSettings.debugSettingsSystem ? "debugSettingsSystem enabled (persistent)" : "debugSettingsSystem disabled (persistent)";
  }
}

// Individual I2C sensor debug command handlers
const char* cmd_debuggps(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_GPS);
    else clearDebugFlag(DEBUG_GPS);
    return v ? "debugGps enabled (runtime only)" : "debugGps disabled (runtime only)";
  } else {
    setSetting(gSettings.debugGps, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_GPS);
    else clearDebugFlag(DEBUG_GPS);
    return gSettings.debugGps ? "debugGps enabled (persistent)" : "debugGps disabled (persistent)";
  }
}

const char* cmd_debugrtc(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_RTC);
    else clearDebugFlag(DEBUG_RTC);
    return v ? "debugRtc enabled (runtime only)" : "debugRtc disabled (runtime only)";
  } else {
    setSetting(gSettings.debugRtc, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_RTC);
    else clearDebugFlag(DEBUG_RTC);
    return gSettings.debugRtc ? "debugRtc enabled (persistent)" : "debugRtc disabled (persistent)";
  }
}

const char* cmd_debugimu(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_IMU);
    else clearDebugFlag(DEBUG_IMU);
    return v ? "debugImu enabled (runtime only)" : "debugImu disabled (runtime only)";
  } else {
    setSetting(gSettings.debugImu, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_IMU);
    else clearDebugFlag(DEBUG_IMU);
    return gSettings.debugImu ? "debugImu enabled (persistent)" : "debugImu disabled (persistent)";
  }
}

const char* cmd_debugthermal(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_THERMAL);
    else clearDebugFlag(DEBUG_THERMAL);
    return v ? "debugThermal enabled (runtime only)" : "debugThermal disabled (runtime only)";
  } else {
    setSetting(gSettings.debugThermal, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_THERMAL);
    else clearDebugFlag(DEBUG_THERMAL);
    return gSettings.debugThermal ? "debugThermal enabled (persistent)" : "debugThermal disabled (persistent)";
  }
}

const char* cmd_debugtof(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_TOF);
    else clearDebugFlag(DEBUG_TOF);
    return v ? "debugTof enabled (runtime only)" : "debugTof disabled (runtime only)";
  } else {
    setSetting(gSettings.debugTof, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_TOF);
    else clearDebugFlag(DEBUG_TOF);
    return gSettings.debugTof ? "debugTof enabled (persistent)" : "debugTof disabled (persistent)";
  }
}

const char* cmd_debuggamepad(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_GAMEPAD);
    else clearDebugFlag(DEBUG_GAMEPAD);
    return v ? "debugGamepad enabled (runtime only)" : "debugGamepad disabled (runtime only)";
  } else {
    setSetting(gSettings.debugGamepad, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_GAMEPAD);
    else clearDebugFlag(DEBUG_GAMEPAD);
    return gSettings.debugGamepad ? "debugGamepad enabled (persistent)" : "debugGamepad disabled (persistent)";
  }
}

const char* cmd_debugapds(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_APDS);
    else clearDebugFlag(DEBUG_APDS);
    return v ? "debugApds enabled (runtime only)" : "debugApds disabled (runtime only)";
  } else {
    setSetting(gSettings.debugApds, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_APDS);
    else clearDebugFlag(DEBUG_APDS);
    return gSettings.debugApds ? "debugApds enabled (persistent)" : "debugApds disabled (persistent)";
  }
}

const char* cmd_debugpresence(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_PRESENCE);
    else clearDebugFlag(DEBUG_PRESENCE);
    return v ? "debugPresence enabled (runtime only)" : "debugPresence disabled (runtime only)";
  } else {
    setSetting(gSettings.debugPresence, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_PRESENCE);
    else clearDebugFlag(DEBUG_PRESENCE);
    return gSettings.debugPresence ? "debugPresence enabled (persistent)" : "debugPresence disabled (persistent)";
  }
}

// Per-sensor frame/data debug command handlers
const char* cmd_debugthermalframe(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_THERMAL_FRAME);
    else clearDebugFlag(DEBUG_THERMAL_FRAME);
    return v ? "debugThermalFrame enabled (runtime only)" : "debugThermalFrame disabled (runtime only)";
  } else {
    setSetting(gSettings.debugThermalFrame, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_THERMAL_FRAME);
    else clearDebugFlag(DEBUG_THERMAL_FRAME);
    return gSettings.debugThermalFrame ? "debugThermalFrame enabled (persistent)" : "debugThermalFrame disabled (persistent)";
  }
}

const char* cmd_debugthermaldata(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_THERMAL_DATA);
    else clearDebugFlag(DEBUG_THERMAL_DATA);
    return v ? "debugThermalData enabled (runtime only)" : "debugThermalData disabled (runtime only)";
  } else {
    setSetting(gSettings.debugThermalData, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_THERMAL_DATA);
    else clearDebugFlag(DEBUG_THERMAL_DATA);
    return gSettings.debugThermalData ? "debugThermalData enabled (persistent)" : "debugThermalData disabled (persistent)";
  }
}

const char* cmd_debugtofframe(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_TOF_FRAME);
    else clearDebugFlag(DEBUG_TOF_FRAME);
    return v ? "debugTofFrame enabled (runtime only)" : "debugTofFrame disabled (runtime only)";
  } else {
    setSetting(gSettings.debugTofFrame, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_TOF_FRAME);
    else clearDebugFlag(DEBUG_TOF_FRAME);
    return gSettings.debugTofFrame ? "debugTofFrame enabled (persistent)" : "debugTofFrame disabled (persistent)";
  }
}

const char* cmd_debuggamepadframe(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_GAMEPAD_FRAME);
    else clearDebugFlag(DEBUG_GAMEPAD_FRAME);
    return v ? "debugGamepadFrame enabled (runtime only)" : "debugGamepadFrame disabled (runtime only)";
  } else {
    setSetting(gSettings.debugGamepadFrame, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_GAMEPAD_FRAME);
    else clearDebugFlag(DEBUG_GAMEPAD_FRAME);
    return gSettings.debugGamepadFrame ? "debugGamepadFrame enabled (persistent)" : "debugGamepadFrame disabled (persistent)";
  }
}

const char* cmd_debuggamepaddata(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_GAMEPAD_DATA);
    else clearDebugFlag(DEBUG_GAMEPAD_DATA);
    return v ? "debugGamepadData enabled (runtime only)" : "debugGamepadData disabled (runtime only)";
  } else {
    setSetting(gSettings.debugGamepadData, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_GAMEPAD_DATA);
    else clearDebugFlag(DEBUG_GAMEPAD_DATA);
    return gSettings.debugGamepadData ? "debugGamepadData enabled (persistent)" : "debugGamepadData disabled (persistent)";
  }
}

const char* cmd_debugimuframe(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_IMU_FRAME);
    else clearDebugFlag(DEBUG_IMU_FRAME);
    return v ? "debugImuFrame enabled (runtime only)" : "debugImuFrame disabled (runtime only)";
  } else {
    setSetting(gSettings.debugImuFrame, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_IMU_FRAME);
    else clearDebugFlag(DEBUG_IMU_FRAME);
    return gSettings.debugImuFrame ? "debugImuFrame enabled (persistent)" : "debugImuFrame disabled (persistent)";
  }
}

const char* cmd_debugimudata(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_IMU_DATA);
    else clearDebugFlag(DEBUG_IMU_DATA);
    return v ? "debugImuData enabled (runtime only)" : "debugImuData disabled (runtime only)";
  } else {
    setSetting(gSettings.debugImuData, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_IMU_DATA);
    else clearDebugFlag(DEBUG_IMU_DATA);
    return gSettings.debugImuData ? "debugImuData enabled (persistent)" : "debugImuData disabled (persistent)";
  }
}

const char* cmd_debugapdsframe(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) {
    mode = valStr.substring(sp2 + 1);
    valStr = valStr.substring(0, sp2);
    mode.trim();
  }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_APDS_FRAME);
    else clearDebugFlag(DEBUG_APDS_FRAME);
    return v ? "debugApdsFrame enabled (runtime only)" : "debugApdsFrame disabled (runtime only)";
  } else {
    setSetting(gSettings.debugApdsFrame, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_APDS_FRAME);
    else clearDebugFlag(DEBUG_APDS_FRAME);
    return gSettings.debugApdsFrame ? "debugApdsFrame enabled (persistent)" : "debugApdsFrame disabled (persistent)";
  }
}

const char* cmd_debugbuffer(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gDebugOutputQueue) {
    return "Debug output queue is not initialized";
  }

  int depth = gDebugOutputQueue ? uxQueueMessagesWaiting(gDebugOutputQueue) : 0;
  int free = gDebugOutputQueue ? uxQueueSpacesAvailable(gDebugOutputQueue) : 0;
  int total = gDebugQueueSize;
  int pct = (depth * 100) / total;
  unsigned long dropped = gDebugDropped;

  const char* status;
  if (pct > 90) {
    status = "CRITICAL - buffer near full!";
  } else if (pct > 75) {
    status = "WARNING - buffer filling up";
  } else if (pct > 50) {
    status = "Busy - moderate usage";
  } else {
    status = "OK - healthy";
  }

  // Output each line separately to avoid DEBUG_MSG_SIZE (256 byte) truncation
  broadcastOutput("Debug Output Queue Status:");
  BROADCAST_PRINTF("  Size: %d messages", total);
  BROADCAST_PRINTF("  Queued: %d (%d%%)", depth, pct);
  BROADCAST_PRINTF("  Free: %d messages", free);
  BROADCAST_PRINTF("  Dropped: %lu (queue full)", dropped);
  BROADCAST_PRINTF("  Status: %s", status);

  return "OK";
}

const char* cmd_debugcommandflow(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_CMD_FLOW); else clearDebugFlag(DEBUG_CMD_FLOW);
    return v ? "debugCommandFlow enabled (runtime only)" : "debugCommandFlow disabled (runtime only)";
  } else {
    setSetting(gSettings.debugCommandFlow, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_CMD_FLOW); else clearDebugFlag(DEBUG_CMD_FLOW);
    return gSettings.debugCommandFlow ? "debugCommandFlow enabled (persistent)" : "debugCommandFlow disabled (persistent)";
  }
}

const char* cmd_debugusers(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_USERS); else clearDebugFlag(DEBUG_USERS);
    return v ? "debugUsers enabled (runtime only)" : "debugUsers disabled (runtime only)";
  } else {
    setSetting(gSettings.debugUsers, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_USERS); else clearDebugFlag(DEBUG_USERS);
    return gSettings.debugUsers ? "debugUsers enabled (persistent)" : "debugUsers disabled (persistent)";
  }
}

const char* cmd_debugsystem(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_SYSTEM); else clearDebugFlag(DEBUG_SYSTEM);
    return v ? "debugSystem enabled (runtime only)" : "debugSystem disabled (runtime only)";
  } else {
    setSetting(gSettings.debugSystem, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_SYSTEM); else clearDebugFlag(DEBUG_SYSTEM);
    return gSettings.debugSystem ? "debugSystem enabled (persistent)" : "debugSystem disabled (persistent)";
  }
}

const char* cmd_debugespnowstream(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_STREAM); else clearDebugFlag(DEBUG_ESPNOW_STREAM);
    return v ? "debugEspNowStream enabled (runtime only)" : "debugEspNowStream disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowStream, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_STREAM); else clearDebugFlag(DEBUG_ESPNOW_STREAM);
    return gSettings.debugEspNowStream ? "debugEspNowStream enabled (persistent)" : "debugEspNowStream disabled (persistent)";
  }
}

const char* cmd_debugespnowcore(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_CORE); else clearDebugFlag(DEBUG_ESPNOW_CORE);
    return v ? "debugEspNowCore enabled (runtime only)" : "debugEspNowCore disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowCore, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_CORE); else clearDebugFlag(DEBUG_ESPNOW_CORE);
    return gSettings.debugEspNowCore ? "debugEspNowCore enabled (persistent)" : "debugEspNowCore disabled (persistent)";
  }
}

const char* cmd_debugespnowrouter(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_ROUTER); else clearDebugFlag(DEBUG_ESPNOW_ROUTER);
    return v ? "debugEspNowRouter enabled (runtime only)" : "debugEspNowRouter disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowRouter, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_ROUTER); else clearDebugFlag(DEBUG_ESPNOW_ROUTER);
    return gSettings.debugEspNowRouter ? "debugEspNowRouter enabled (persistent)" : "debugEspNowRouter disabled (persistent)";
  }
}

const char* cmd_debugmemory(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_MEMORY); else clearDebugFlag(DEBUG_MEMORY);
    return v ? "debugMemory enabled (runtime only)" : "debugMemory disabled (runtime only)";
  } else {
    setSetting(gSettings.debugMemory, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_MEMORY); else clearDebugFlag(DEBUG_MEMORY);
    return gSettings.debugMemory ? "debugMemory enabled (persistent)" : "debugMemory disabled (persistent)";
  }
}

const char* cmd_debugespnowmesh(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_MESH); else clearDebugFlag(DEBUG_ESPNOW_MESH);
    return v ? "debugEspNowMesh enabled (runtime only)" : "debugEspNowMesh disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowMesh, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_MESH); else clearDebugFlag(DEBUG_ESPNOW_MESH);
    return gSettings.debugEspNowMesh ? "debugEspNowMesh enabled (persistent)" : "debugEspNowMesh disabled (persistent)";
  }
}

const char* cmd_debugespnowtopo(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_TOPO); else clearDebugFlag(DEBUG_ESPNOW_TOPO);
    return v ? "debugEspNowTopo enabled (runtime only)" : "debugEspNowTopo disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowTopo, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_TOPO); else clearDebugFlag(DEBUG_ESPNOW_TOPO);
    return gSettings.debugEspNowTopo ? "debugEspNowTopo enabled (persistent)" : "debugEspNowTopo disabled (persistent)";
  }
}

const char* cmd_debugespnowencryption(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_ENCRYPTION); else clearDebugFlag(DEBUG_ESPNOW_ENCRYPTION);
    return v ? "debugEspNowEncryption enabled (runtime only)" : "debugEspNowEncryption disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowEncryption, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_ENCRYPTION); else clearDebugFlag(DEBUG_ESPNOW_ENCRYPTION);
    return gSettings.debugEspNowEncryption ? "debugEspNowEncryption enabled (persistent)" : "debugEspNowEncryption disabled (persistent)";
  }
}

const char* cmd_debugespnowmetadata(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_METADATA); else clearDebugFlag(DEBUG_ESPNOW_METADATA);
    return v ? "debugEspNowMetadata enabled (runtime only)" : "debugEspNowMetadata disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowMetadata, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_METADATA); else clearDebugFlag(DEBUG_ESPNOW_METADATA);
    return gSettings.debugEspNowMetadata ? "debugEspNowMetadata enabled (persistent)" : "debugEspNowMetadata disabled (persistent)";
  }
}

const char* cmd_debugautoscheduler(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTO_SCHEDULER); else clearDebugFlag(DEBUG_AUTO_SCHEDULER);
    return v ? "debugAutoScheduler enabled (runtime only)" : "debugAutoScheduler disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAutoScheduler, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_AUTO_SCHEDULER); else clearDebugFlag(DEBUG_AUTO_SCHEDULER);
    return gSettings.debugAutoScheduler ? "debugAutoScheduler enabled (persistent)" : "debugAutoScheduler disabled (persistent)";
  }
}

const char* cmd_debugautoexec(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTO_EXEC); else clearDebugFlag(DEBUG_AUTO_EXEC);
    return v ? "debugAutoExec enabled (runtime only)" : "debugAutoExec disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAutoExec, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_AUTO_EXEC); else clearDebugFlag(DEBUG_AUTO_EXEC);
    return gSettings.debugAutoExec ? "debugAutoExec enabled (persistent)" : "debugAutoExec disabled (persistent)";
  }
}

const char* cmd_debugautocondition(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTO_CONDITION); else clearDebugFlag(DEBUG_AUTO_CONDITION);
    return v ? "debugAutoCondition enabled (runtime only)" : "debugAutoCondition disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAutoCondition, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_AUTO_CONDITION); else clearDebugFlag(DEBUG_AUTO_CONDITION);
    return gSettings.debugAutoCondition ? "debugAutoCondition enabled (persistent)" : "debugAutoCondition disabled (persistent)";
  }
}

const char* cmd_debugautotiming(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTO_TIMING); else clearDebugFlag(DEBUG_AUTO_TIMING);
    return v ? "debugAutoTiming enabled (runtime only)" : "debugAutoTiming disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAutoTiming, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_AUTO_TIMING); else clearDebugFlag(DEBUG_AUTO_TIMING);
    return gSettings.debugAutoTiming ? "debugAutoTiming enabled (persistent)" : "debugAutoTiming disabled (persistent)";
  }
}

// ============================================================================
// Sub-flag parent sync helpers — one per group, called after any sub-flag change
// ============================================================================
static inline void syncAuthParent()    { updateParentDebugFlag(DEBUG_AUTH,        gSettings.debugAuth        || gDebugSubFlags.authSessions   || gDebugSubFlags.authCookies    || gDebugSubFlags.authLogin      || gDebugSubFlags.authBootId); }
static inline void syncHttpParent()    { updateParentDebugFlag(DEBUG_HTTP,        gSettings.debugHttp        || gDebugSubFlags.httpHandlers   || gDebugSubFlags.httpRequests   || gDebugSubFlags.httpResponses  || gDebugSubFlags.httpStreaming); }
static inline void syncWifiParent()    { updateParentDebugFlag(DEBUG_WIFI,        gSettings.debugWifi        || gDebugSubFlags.wifiConnection || gDebugSubFlags.wifiConfig     || gDebugSubFlags.wifiScanning   || gDebugSubFlags.wifiDriver); }
static inline void syncStorageParent() { updateParentDebugFlag(DEBUG_STORAGE,     gSettings.debugStorage     || gDebugSubFlags.storageFiles   || gDebugSubFlags.storageJson    || gDebugSubFlags.storageSettings || gDebugSubFlags.storageMigration); }
static inline void syncSystemParent()  { updateParentDebugFlag(DEBUG_SYSTEM,      gSettings.debugSystem      || gDebugSubFlags.systemBoot     || gDebugSubFlags.systemConfig   || gDebugSubFlags.systemTasks    || gDebugSubFlags.systemHardware); }
static inline void syncUsersParent()   { updateParentDebugFlag(DEBUG_USERS,       gSettings.debugUsers       || gDebugSubFlags.usersMgmt      || gDebugSubFlags.usersRegister  || gDebugSubFlags.usersQuery); }
static inline void syncCliParent()     { updateParentDebugFlag(DEBUG_CLI,         gSettings.debugCli         || gDebugSubFlags.cliExecution   || gDebugSubFlags.cliQueue       || gDebugSubFlags.cliValidation); }
static inline void syncPerfParent()    { updateParentDebugFlag(DEBUG_PERFORMANCE, gSettings.debugPerformance || gDebugSubFlags.perfStack       || gDebugSubFlags.perfHeap       || gDebugSubFlags.perfTiming); }
static inline void syncSseParent()     { updateParentDebugFlag(DEBUG_SSE,         gSettings.debugSse         || gDebugSubFlags.sseConnection  || gDebugSubFlags.sseEvents      || gDebugSubFlags.sseBroadcast); }
static inline void syncCmdFlowParent() { updateParentDebugFlag(DEBUG_CMD_FLOW,    gSettings.debugCommandFlow || gDebugSubFlags.cmdflowRouting  || gDebugSubFlags.cmdflowQueue   || gDebugSubFlags.cmdflowContext); }

const char* cmd_debugauthsessions(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.authSessions = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugAuthSessions, (bool)(v != 0));
  syncAuthParent();
  if (modeTemp) return v ? "debugAuthSessions enabled (runtime only)" : "debugAuthSessions disabled (runtime only)";
  return gSettings.debugAuthSessions ? "debugAuthSessions enabled (persistent)" : "debugAuthSessions disabled (persistent)";
}

const char* cmd_debugauthcookies(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.authCookies = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugAuthCookies, (bool)(v != 0));
  syncAuthParent();
  if (modeTemp) return v ? "debugAuthCookies enabled (runtime only)" : "debugAuthCookies disabled (runtime only)";
  return gSettings.debugAuthCookies ? "debugAuthCookies enabled (persistent)" : "debugAuthCookies disabled (persistent)";
}

const char* cmd_debugauthlogin(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.authLogin = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugAuthLogin, (bool)(v != 0));
  syncAuthParent();
  if (modeTemp) return v ? "debugAuthLogin enabled (runtime only)" : "debugAuthLogin disabled (runtime only)";
  return gSettings.debugAuthLogin ? "debugAuthLogin enabled (persistent)" : "debugAuthLogin disabled (persistent)";
}

const char* cmd_debugauthbootid(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  int sp2 = valStr.indexOf(' ');
  String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2 + 1); valStr = valStr.substring(0, sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.authBootId = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugAuthBootId, (bool)(v != 0));
  syncAuthParent();
  if (modeTemp) return v ? "debugAuthBootId enabled (runtime only)" : "debugAuthBootId disabled (runtime only)";
  return gSettings.debugAuthBootId ? "debugAuthBootId enabled (persistent)" : "debugAuthBootId disabled (persistent)";
}

const char* cmd_commandmodulesummary(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t moduleCount = 0;
  getCommandModules(moduleCount);
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  printCommandModuleSummary();
  snprintf(gDebugBuffer, 1024, "Command modules: %zu modules, %zu commands", moduleCount, gCommandsCount);
  return gDebugBuffer;
}

const char* cmd_settingsmodulesummary(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  size_t totalEntries = 0;
  for (size_t i = 0; i < modCount; ++i) {
    if (mods && mods[i]) totalEntries += mods[i]->count;
  }
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  printSettingsModuleSummary();
  snprintf(gDebugBuffer, 1024, "Settings modules: %zu modules, %zu total entries", modCount, totalEntries);
  return gDebugBuffer;
}

// ============================================================================
// Debug Category Name Mapping
// ============================================================================

const char* getDebugCategoryName(uint32_t flag) {
  // Return the first matching flag name
  if (flag & DEBUG_AUTH) return "AUTH";
  if (flag & DEBUG_HTTP) return "HTTP";
  if (flag & DEBUG_SSE) return "SSE";
  if (flag & DEBUG_CLI) return "CLI";
  if (flag & DEBUG_SENSORS) return "SENSORS";
  if (flag & DEBUG_FMRADIO) return "FMRADIO";
  if (flag & DEBUG_WIFI) return "WIFI";
  if (flag & DEBUG_STORAGE) return "STORAGE";
  if (flag & DEBUG_PERFORMANCE) return "PERF";
  if (flag & DEBUG_SYSTEM) return "SYSTEM";
  if (flag & DEBUG_USERS) return "USERS";
  if (flag & DEBUG_AUTOMATIONS) return "AUTO";
  if (flag & DEBUG_LOGGER) return "LOGGER";
  if (flag & DEBUG_ESPNOW_STREAM) return "ESPNOW_STREAM";
  if (flag & DEBUG_MEMORY) return "MEMORY";
  if (flag & DEBUG_CMD_FLOW) return "CMD_FLOW";
  if (flag & DEBUG_COMMAND_SYSTEM) return "CMD_SYS";
  if (flag & DEBUG_SETTINGS_SYSTEM) return "SETTINGS_SYS";
  if (flag & DEBUG_SECURITY) return "SECURITY";
  if (flag & DEBUG_ESPNOW_CORE) return "ESPNOW";
  if (flag & DEBUG_ESPNOW_ROUTER) return "ESPNOW_ROUTER";
  if (flag & DEBUG_ESPNOW_MESH) return "ESPNOW_MESH";
  if (flag & DEBUG_ESPNOW_TOPO) return "ESPNOW_TOPO";
  if (flag & DEBUG_AUTO_SCHEDULER) return "AUTO_SCHED";
  if (flag & DEBUG_AUTO_EXEC) return "AUTO_EXEC";
  if (flag & DEBUG_AUTO_CONDITION) return "AUTO_COND";
  if (flag & DEBUG_AUTO_TIMING) return "AUTO_TIME";
  if (flag & DEBUG_ESPNOW_ENCRYPTION) return "ESPNOW_ENCRYPTION";
  if (flag & DEBUG_ESPNOW_METADATA) return "ESPNOW_METADATA";
  return "UNKNOWN";
}

// ============================================================================
// System Logging Commands
// ============================================================================

// Helper: Generate timestamped filename for system log
static String generateSystemLogFilename() {
  String filename = "/logs/system-";
  
  // Try to get epoch time
  time_t now = time(nullptr);
  if (now > 0 && now > 1000000000) {  // Valid epoch time (after year 2001)
    struct tm* timeinfo = localtime(&now);
    char timestamp[32];
    // Format: YYYY-MM-DDTHH-MM-SS
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H-%M-%S", timeinfo);
    filename += String(timestamp);
  } else {
    // Fallback to uptime
    filename += "uptime-" + String(millis());
  }
  
  filename += ".log";
  return filename;
}

const char* cmd_log(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String action = args;
  action.trim();
  if (action.length() == 0) {
    return "Usage: log <start|stop|status|autostart>\n"
           "  start [filepath] [flags=0xXXXX] [tags=0|1]: Begin system logging\n"
           "    filepath: Log file path (auto-generated if omitted)\n"
           "    flags: Debug flags to enable (e.g., flags=0x0203)\n"
           "    tags: Enable category tags (default: 1)\n"
           "  stop: Stop system logging\n"
           "  status: Show current logging status\n"
           "  autostart: Toggle auto-start system logging on boot\n"
           "Examples:\n"
           "  log start\n"
           "  log start /logs/debug.log\n"
           "  log start flags=0x0203 tags=1\n"
           "  log start /logs/debug.log flags=0x4603 tags=0\n"
           "  log autostart";
  }
  int sp2 = action.indexOf(' ');
  String subCmd = (sp2 >= 0) ? action.substring(0, sp2) : action;
  subCmd.toLowerCase();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  // Handle 'status' subcommand
  if (subCmd == "status") {
    if (gSystemLogEnabled && (gOutputFlags & OUTPUT_FILE)) {
      unsigned long ageSeconds = (millis() - gSystemLogLastWrite) / 1000;
      snprintf(gDebugBuffer, 1024,
               "System logging ACTIVE\n"
               "  File: %s\n"
               "  Last write: %lus ago\n"
               "  Output flags: 0x%02X\n"
               "  Auto-start: %s",
               gSystemLogPath.c_str(), ageSeconds, (unsigned)gOutputFlags,
               gSettings.systemLogAutoStart ? "ON" : "OFF");
    } else if (gSystemLogEnabled) {
      snprintf(gDebugBuffer, 1024,
               "System logging CONFIGURED but OUTPUT_FILE flag not set\n"
               "  File: %s\n"
               "  Use 'log start' to enable\n"
               "  Auto-start: %s",
               gSystemLogPath.c_str(),
               gSettings.systemLogAutoStart ? "ON" : "OFF");
    } else {
      snprintf(gDebugBuffer, 1024, "System logging is INACTIVE\n  Auto-start: %s",
               gSettings.systemLogAutoStart ? "ON" : "OFF");
    }
    return gDebugBuffer;
  }
  
  // Handle 'stop' subcommand
  if (subCmd == "stop") {
    if (!gSystemLogEnabled) {
      return "System logging is not running";
    }
    
    // Flush and close persistent file handle if open
    if (gSystemLogFile) {
      fsLock("debug.log");
      gSystemLogFile.flush();
      gSystemLogFile.close();
      // Note: close() resets the handle internally
      gSystemLogUnflushedCount = 0;
      fsUnlock();
    }
    
    gSystemLogEnabled = false;
    gOutputFlags &= ~OUTPUT_FILE;
    String msg = "System logging stopped. Log saved to: " + gSystemLogPath;
    gSystemLogPath = "";
    snprintf(gDebugBuffer, 1024, "%s", msg.c_str());
    return gDebugBuffer;
  }
  
  // Handle 'start' subcommand
  if (subCmd == "start") {
    if (gSystemLogEnabled) {
      return "System logging already running. Use 'log stop' first.";
    }
    
    // Ensure any previous file handle is closed (safety check)
    if (gSystemLogFile) {
      fsLock("log.create");
      gSystemLogFile.flush();
      gSystemLogFile.close();
      // Note: close() resets the handle internally
      fsUnlock();
    }
    
    // Parse arguments: log start [filepath] [flags=0xXXXX] [tags=0|1]
    String filepath;
    uint64_t debugFlags = 0xFFFFFFFFFFFFFFFFULL; // Sentinel: don't change if not specified
    int categoryTags = -1; // Sentinel: don't change if not specified
    
    if (sp2 >= 0) {
      String args = action.substring(sp2 + 1);
      args.trim();
      
      // Parse all key=value pairs
      bool hasFilepath = false;
      int idx = 0;
      while (idx < args.length()) {
        int spacePos = args.indexOf(' ', idx);
        String token = (spacePos >= 0) ? args.substring(idx, spacePos) : args.substring(idx);
        token.trim();
        
        if (token.startsWith("flags=")) {
          String flagsStr = token.substring(6);
          flagsStr.trim();
          if (flagsStr.startsWith("0x") || flagsStr.startsWith("0X")) {
            debugFlags = strtoull(flagsStr.c_str() + 2, nullptr, 16);
          } else {
            debugFlags = strtoull(flagsStr.c_str(), nullptr, 16);
          }
        } else if (token.startsWith("tags=")) {
          String tagsStr = token.substring(5);
          tagsStr.trim();
          categoryTags = tagsStr.toInt();
        } else if (token.length() > 0 && !hasFilepath) {
          // First non-key=value token is the filepath
          filepath = token;
          hasFilepath = true;
        }
        
        if (spacePos < 0) break;
        idx = spacePos + 1;
      }
      
      // Generate filename if not specified
      if (!hasFilepath) {
        filepath = generateSystemLogFilename();
      }
    } else {
      // No arguments - auto-generate filename
      filepath = generateSystemLogFilename();
    }
    
    if (filepath.length() == 0 || filepath.charAt(0) != '/') {
      return "Error: Filepath must start with / (e.g., /logs/system.log)";
    }
    
    // Apply debug flags if specified
    if (debugFlags != 0xFFFFFFFFFFFFFFFFULL) {
      gDebugFlags = debugFlags;
      char flagsMsg[128];
      snprintf(flagsMsg, sizeof(flagsMsg), "Debug flags set to: 0x%016llX", (unsigned long long)gDebugFlags);
      broadcastOutput(flagsMsg);
    }
    
    // Apply category tags setting if specified
    if (categoryTags >= 0) {
      gSystemLogCategoryTags = (categoryTags != 0);
      broadcastOutput(gSystemLogCategoryTags ? "Category tags: ENABLED" : "Category tags: DISABLED");
    }
    
    // Ensure directory exists
    int lastSlash = filepath.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = filepath.substring(0, lastSlash);
      if (!LittleFS.exists(dir)) {
        fsLock("log.mkdir");
        bool created = LittleFS.mkdir(dir);
        fsUnlock();
        if (!created) {
          snprintf(gDebugBuffer, 1024, "Error: Failed to create directory: %s", dir.c_str());
          return gDebugBuffer;
        }
        broadcastOutput("Created directory: " + dir);
      }
    }
    
    // Create file if needed
    fsLock("log.create");
    if (!LittleFS.exists(filepath)) {
      File f = LittleFS.open(filepath, "w");
      if (!f) {
        fsUnlock();
        snprintf(gDebugBuffer, 1024, "Error: Failed to create file: %s", filepath.c_str());
        return gDebugBuffer;
      }
      f.printf("# System log started at %lu ms\n", millis());
      f.close();
    }
    fsUnlock();
    
    gSystemLogPath = filepath;
    gSystemLogEnabled = true;
    gSystemLogLastWrite = millis();
    gOutputFlags |= OUTPUT_FILE;
    
    snprintf(gDebugBuffer, 1024, "System logging started\n  File: %s", filepath.c_str());
    broadcastOutput(gDebugBuffer);
    return gDebugBuffer;
  }
  
  // Handle 'autostart' subcommand
  if (subCmd == "autostart") {
    bool newValue = !gSettings.systemLogAutoStart;
    setSetting(gSettings.systemLogAutoStart, newValue);
    snprintf(gDebugBuffer, 1024, "System log auto-start %s", newValue ? "ENABLED" : "DISABLED");
    return gDebugBuffer;
  }
  
  return "Error: Unknown subcommand. Use: start, stop, status, or autostart";
}

// ============================================================================
// Debug Sub-Flag Commands (merged from System_Debug_SubCommands.cpp)
// ============================================================================

// HTTP sub-flag commands
const char* cmd_debughttphandlers(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.httpHandlers = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugHttpHandlers, (bool)(v!=0));
  syncHttpParent();
  if (modeTemp) return v ? "debugHttpHandlers enabled (runtime only)" : "debugHttpHandlers disabled (runtime only)";
  return gSettings.debugHttpHandlers ? "debugHttpHandlers enabled (persistent)" : "debugHttpHandlers disabled (persistent)";
}

const char* cmd_debughttprequests(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.httpRequests = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugHttpRequests, (bool)(v!=0));
  syncHttpParent();
  if (modeTemp) return v ? "debugHttpRequests enabled (runtime only)" : "debugHttpRequests disabled (runtime only)";
  return gSettings.debugHttpRequests ? "debugHttpRequests enabled (persistent)" : "debugHttpRequests disabled (persistent)";
}

const char* cmd_debughttpresponses(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.httpResponses = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugHttpResponses, (bool)(v!=0));
  syncHttpParent();
  if (modeTemp) return v ? "debugHttpResponses enabled (runtime only)" : "debugHttpResponses disabled (runtime only)";
  return gSettings.debugHttpResponses ? "debugHttpResponses enabled (persistent)" : "debugHttpResponses disabled (persistent)";
}

const char* cmd_debughttpstreaming(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.httpStreaming = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugHttpStreaming, (bool)(v!=0));
  syncHttpParent();
  if (modeTemp) return v ? "debugHttpStreaming enabled (runtime only)" : "debugHttpStreaming disabled (runtime only)";
  return gSettings.debugHttpStreaming ? "debugHttpStreaming enabled (persistent)" : "debugHttpStreaming disabled (persistent)";
}

// WiFi sub-flag commands
const char* cmd_debugwificonnection(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.wifiConnection = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugWifiConnection, (bool)(v!=0));
  syncWifiParent();
  if (modeTemp) return v ? "debugWifiConnection enabled (runtime only)" : "debugWifiConnection disabled (runtime only)";
  return gSettings.debugWifiConnection ? "debugWifiConnection enabled (persistent)" : "debugWifiConnection disabled (persistent)";
}

const char* cmd_debugwificonfig(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.wifiConfig = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugWifiConfig, (bool)(v!=0));
  syncWifiParent();
  if (modeTemp) return v ? "debugWifiConfig enabled (runtime only)" : "debugWifiConfig disabled (runtime only)";
  return gSettings.debugWifiConfig ? "debugWifiConfig enabled (persistent)" : "debugWifiConfig disabled (persistent)";
}

const char* cmd_debugwifiscanning(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.wifiScanning = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugWifiScanning, (bool)(v!=0));
  syncWifiParent();
  if (modeTemp) return v ? "debugWifiScanning enabled (runtime only)" : "debugWifiScanning disabled (runtime only)";
  return gSettings.debugWifiScanning ? "debugWifiScanning enabled (persistent)" : "debugWifiScanning disabled (persistent)";
}

const char* cmd_debugwifidriver(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.wifiDriver = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugWifiDriver, (bool)(v!=0));
  syncWifiParent();
  if (modeTemp) return v ? "debugWifiDriver enabled (runtime only)" : "debugWifiDriver disabled (runtime only)";
  return gSettings.debugWifiDriver ? "debugWifiDriver enabled (persistent)" : "debugWifiDriver disabled (persistent)";
}

// Storage sub-flag commands
const char* cmd_debugstoragefiles(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.storageFiles = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStorageFiles, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStorageFiles enabled (runtime only)" : "debugStorageFiles disabled (runtime only)";
  return gSettings.debugStorageFiles ? "debugStorageFiles enabled (persistent)" : "debugStorageFiles disabled (persistent)";
}

const char* cmd_debugstoragejson(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.storageJson = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStorageJson, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStorageJson enabled (runtime only)" : "debugStorageJson disabled (runtime only)";
  return gSettings.debugStorageJson ? "debugStorageJson enabled (persistent)" : "debugStorageJson disabled (persistent)";
}

const char* cmd_debugstoragesettings(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.storageSettings = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStorageSettings, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStorageSettings enabled (runtime only)" : "debugStorageSettings disabled (runtime only)";
  return gSettings.debugStorageSettings ? "debugStorageSettings enabled (persistent)" : "debugStorageSettings disabled (persistent)";
}

const char* cmd_debugstoragemigration(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.storageMigration = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStorageMigration, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStorageMigration enabled (runtime only)" : "debugStorageMigration disabled (runtime only)";
  return gSettings.debugStorageMigration ? "debugStorageMigration enabled (persistent)" : "debugStorageMigration disabled (persistent)";
}

// System sub-flag commands
const char* cmd_debugsystemboot(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.systemBoot = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSystemBoot, (bool)(v!=0));
  syncSystemParent();
  if (modeTemp) return v ? "debugSystemBoot enabled (runtime only)" : "debugSystemBoot disabled (runtime only)";
  return gSettings.debugSystemBoot ? "debugSystemBoot enabled (persistent)" : "debugSystemBoot disabled (persistent)";
}

const char* cmd_debugsystemconfig(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.systemConfig = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSystemConfig, (bool)(v!=0));
  syncSystemParent();
  if (modeTemp) return v ? "debugSystemConfig enabled (runtime only)" : "debugSystemConfig disabled (runtime only)";
  return gSettings.debugSystemConfig ? "debugSystemConfig enabled (persistent)" : "debugSystemConfig disabled (persistent)";
}

const char* cmd_debugsystemtasks(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.systemTasks = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSystemTasks, (bool)(v!=0));
  syncSystemParent();
  if (modeTemp) return v ? "debugSystemTasks enabled (runtime only)" : "debugSystemTasks disabled (runtime only)";
  return gSettings.debugSystemTasks ? "debugSystemTasks enabled (persistent)" : "debugSystemTasks disabled (persistent)";
}

const char* cmd_debugsystemhardware(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.systemHardware = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSystemHardware, (bool)(v!=0));
  syncSystemParent();
  if (modeTemp) return v ? "debugSystemHardware enabled (runtime only)" : "debugSystemHardware disabled (runtime only)";
  return gSettings.debugSystemHardware ? "debugSystemHardware enabled (persistent)" : "debugSystemHardware disabled (persistent)";
}

// Users sub-flag commands
const char* cmd_debugusersmgmt(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.usersMgmt = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugUsersMgmt, (bool)(v!=0));
  syncUsersParent();
  if (modeTemp) return v ? "debugUsersMgmt enabled (runtime only)" : "debugUsersMgmt disabled (runtime only)";
  return gSettings.debugUsersMgmt ? "debugUsersMgmt enabled (persistent)" : "debugUsersMgmt disabled (persistent)";
}

const char* cmd_debugusersregister(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.usersRegister = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugUsersRegister, (bool)(v!=0));
  syncUsersParent();
  if (modeTemp) return v ? "debugUsersRegister enabled (runtime only)" : "debugUsersRegister disabled (runtime only)";
  return gSettings.debugUsersRegister ? "debugUsersRegister enabled (persistent)" : "debugUsersRegister disabled (persistent)";
}

const char* cmd_debugusersquery(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.usersQuery = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugUsersQuery, (bool)(v!=0));
  syncUsersParent();
  if (modeTemp) return v ? "debugUsersQuery enabled (runtime only)" : "debugUsersQuery disabled (runtime only)";
  return gSettings.debugUsersQuery ? "debugUsersQuery enabled (persistent)" : "debugUsersQuery disabled (persistent)";
}

// CLI sub-flag commands
const char* cmd_debugcliexecution(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.cliExecution = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCliExecution, (bool)(v!=0));
  syncCliParent();
  if (modeTemp) return v ? "debugCliExecution enabled (runtime only)" : "debugCliExecution disabled (runtime only)";
  return gSettings.debugCliExecution ? "debugCliExecution enabled (persistent)" : "debugCliExecution disabled (persistent)";
}

const char* cmd_debugcliqueue(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.cliQueue = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCliQueue, (bool)(v!=0));
  syncCliParent();
  if (modeTemp) return v ? "debugCliQueue enabled (runtime only)" : "debugCliQueue disabled (runtime only)";
  return gSettings.debugCliQueue ? "debugCliQueue enabled (persistent)" : "debugCliQueue disabled (persistent)";
}

const char* cmd_debugclivalidation(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.cliValidation = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCliValidation, (bool)(v!=0));
  syncCliParent();
  if (modeTemp) return v ? "debugCliValidation enabled (runtime only)" : "debugCliValidation disabled (runtime only)";
  return gSettings.debugCliValidation ? "debugCliValidation enabled (persistent)" : "debugCliValidation disabled (persistent)";
}

// Performance sub-flag commands
const char* cmd_debugperfstack(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.perfStack = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugPerfStack, (bool)(v!=0));
  syncPerfParent();
  if (modeTemp) return v ? "debugPerfStack enabled (runtime only)" : "debugPerfStack disabled (runtime only)";
  return gSettings.debugPerfStack ? "debugPerfStack enabled (persistent)" : "debugPerfStack disabled (persistent)";
}

const char* cmd_debugperfheap(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.perfHeap = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugPerfHeap, (bool)(v!=0));
  syncPerfParent();
  if (modeTemp) return v ? "debugPerfHeap enabled (runtime only)" : "debugPerfHeap disabled (runtime only)";
  return gSettings.debugPerfHeap ? "debugPerfHeap enabled (persistent)" : "debugPerfHeap disabled (persistent)";
}

const char* cmd_debugperftiming(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.perfTiming = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugPerfTiming, (bool)(v!=0));
  syncPerfParent();
  if (modeTemp) return v ? "debugPerfTiming enabled (runtime only)" : "debugPerfTiming disabled (runtime only)";
  return gSettings.debugPerfTiming ? "debugPerfTiming enabled (persistent)" : "debugPerfTiming disabled (persistent)";
}

// SSE sub-flag commands
const char* cmd_debugsseconnection(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.sseConnection = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSseConnection, (bool)(v!=0));
  syncSseParent();
  if (modeTemp) return v ? "debugSseConnection enabled (runtime only)" : "debugSseConnection disabled (runtime only)";
  return gSettings.debugSseConnection ? "debugSseConnection enabled (persistent)" : "debugSseConnection disabled (persistent)";
}

const char* cmd_debugsseevents(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.sseEvents = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSseEvents, (bool)(v!=0));
  syncSseParent();
  if (modeTemp) return v ? "debugSseEvents enabled (runtime only)" : "debugSseEvents disabled (runtime only)";
  return gSettings.debugSseEvents ? "debugSseEvents enabled (persistent)" : "debugSseEvents disabled (persistent)";
}

const char* cmd_debugssebroadcast(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.sseBroadcast = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSseBroadcast, (bool)(v!=0));
  syncSseParent();
  if (modeTemp) return v ? "debugSseBroadcast enabled (runtime only)" : "debugSseBroadcast disabled (runtime only)";
  return gSettings.debugSseBroadcast ? "debugSseBroadcast enabled (persistent)" : "debugSseBroadcast disabled (persistent)";
}

// Command Flow sub-flag commands
const char* cmd_debugcmdflowrouting(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.cmdflowRouting = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCmdflowRouting, (bool)(v!=0));
  syncCmdFlowParent();
  if (modeTemp) return v ? "debugCmdflowRouting enabled (runtime only)" : "debugCmdflowRouting disabled (runtime only)";
  return gSettings.debugCmdflowRouting ? "debugCmdflowRouting enabled (persistent)" : "debugCmdflowRouting disabled (persistent)";
}

const char* cmd_debugcmdflowqueue(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.cmdflowQueue = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCmdflowQueue, (bool)(v!=0));
  syncCmdFlowParent();
  if (modeTemp) return v ? "debugCmdflowQueue enabled (runtime only)" : "debugCmdflowQueue disabled (runtime only)";
  return gSettings.debugCmdflowQueue ? "debugCmdflowQueue enabled (persistent)" : "debugCmdflowQueue disabled (persistent)";
}

const char* cmd_debugcmdflowcontext(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args; valStr.trim();
  int sp2 = valStr.indexOf(' '); String mode = "";
  if (sp2 >= 0) { mode = valStr.substring(sp2+1); valStr = valStr.substring(0,sp2); mode.trim(); }
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = valStr.toInt();
  gDebugSubFlags.cmdflowContext = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCmdflowContext, (bool)(v!=0));
  syncCmdFlowParent();
  if (modeTemp) return v ? "debugCmdflowContext enabled (runtime only)" : "debugCmdflowContext disabled (runtime only)";
  return gSettings.debugCmdflowContext ? "debugCmdflowContext enabled (persistent)" : "debugCmdflowContext disabled (persistent)";
}

// ============================================================================
// Debug Command Registry
// ============================================================================

const CommandEntry debugCommands[] = {
  { "debugauthcookies", "Debug authentication cookies.", true, cmd_debugauthcookies },
  { "debughttp", "Debug HTTP requests.", true, cmd_debughttp },
  { "debugsse", "Debug Server-Sent Events.", true, cmd_debugsse },
  { "debugcli", "Debug CLI processing.", true, cmd_debugcli },
  { "debugauth", "Debug authentication (parent flag).", true, cmd_debugauth, "Usage: debugauth <0|1>" },
  { "debugsensors", "Debug sensors (parent flag).", true, cmd_debugsensors, "Usage: debugsensors <0|1>" },
  { "debugespnow", "Debug ESP-NOW (parent flag).", true, cmd_debugespnow, "Usage: debugespnow <0|1>" },
  { "debugsensorsgeneral", "Debug general sensor operations.", true, cmd_debugsensorsgeneral },
  { "debugcamera", "Debug camera operations.", true, cmd_debugcamera },
  { "debugmicrophone", "Debug microphone operations.", true, cmd_debugmicrophone },
  { "debuggps", "Debug GPS sensor (PA1010D).", true, cmd_debuggps, "Usage: debuggps <0|1>" },
  { "debugrtc", "Debug RTC sensor (DS3231).", true, cmd_debugrtc, "Usage: debugrtc <0|1>" },
  { "debugimu", "Debug IMU sensor (BNO055).", true, cmd_debugimu, "Usage: debugimu <0|1>" },
  { "debugthermal", "Debug thermal sensor (MLX90640).", true, cmd_debugthermal, "Usage: debugthermal <0|1>" },
  { "debugtof", "Debug ToF sensor (VL53L4CX).", true, cmd_debugtof, "Usage: debugtof <0|1>" },
  { "debuggamepad", "Debug gamepad (Seesaw).", true, cmd_debuggamepad, "Usage: debuggamepad <0|1>" },
  { "debugapds", "Debug APDS sensor (APDS9960).", true, cmd_debugapds, "Usage: debugapds <0|1>" },
  { "debugpresence", "Debug presence sensor (STHS34PF80).", true, cmd_debugpresence, "Usage: debugpresence <0|1>" },
  { "debugthermalframe", "Debug thermal frame timing/capture.", true, cmd_debugthermalframe, "Usage: debugthermalframe <0|1>" },
  { "debugthermaldata", "Debug thermal data processing.", true, cmd_debugthermaldata, "Usage: debugthermaldata <0|1>" },
  { "debugtofframe", "Debug ToF frame capture.", true, cmd_debugtofframe, "Usage: debugtofframe <0|1>" },
  { "debuggamepadframe", "Debug gamepad frame timing.", true, cmd_debuggamepadframe, "Usage: debuggamepadframe <0|1>" },
  { "debuggamepaddata", "Debug gamepad button events.", true, cmd_debuggamepaddata, "Usage: debuggamepaddata <0|1>" },
  { "debugimuframe", "Debug IMU frame timing.", true, cmd_debugimuframe, "Usage: debugimuframe <0|1>" },
  { "debugimudata", "Debug IMU data updates.", true, cmd_debugimudata, "Usage: debugimudata <0|1>" },
  { "debugapdsframe", "Debug APDS frame timing.", true, cmd_debugapdsframe, "Usage: debugapdsframe <0|1>" },
  { "debugi2c", "Debug I2C bus transactions, mutex, clock changes.", true, cmd_debugi2c },
  { "debugwifi", "Debug WiFi operations.", true, cmd_debugwifi },
  { "debugstorage", "Debug storage operations.", true, cmd_debugstorage },
  { "debugperformance", "Debug performance metrics.", true, cmd_debugperformance },
  { "debugdatetime", "Debug date/time operations.", true, cmd_debugdatetime },
  { "debugverbose", "Global debug verbosity override (forces all debug + loglevel=DEBUG).", true, cmd_debugverbose, "Usage: debugverbose <0|1>" },
  { "debugbuffer", "Show debug ring buffer status.", false, cmd_debugbuffer },
  { "debugcommandflow", "Debug command flow.", true, cmd_debugcommandflow, "Usage: debugcommandflow <0|1>" },
  { "debugusers", "Debug user management.", true, cmd_debugusers, "Usage: debugusers <0|1>" },
  { "debugsystem", "Debug system/boot operations.", true, cmd_debugsystem, "Usage: debugsystem <0|1>" },
  { "debugespnowstream", "Debug ESP-NOW streaming output.", true, cmd_debugespnowstream, "Usage: debugespnowstream <0|1>" },
  { "debugespnowcore", "Debug ESP-NOW core operations.", true, cmd_debugespnowcore, "Usage: debugespnowcore <0|1>" },
  { "debugespnowrouter", "Debug ESP-NOW router operations.", true, cmd_debugespnowrouter, "Usage: debugespnowrouter <0|1>" },
  { "debugespnowmesh", "Debug ESP-NOW mesh operations.", true, cmd_debugespnowmesh, "Usage: debugespnowmesh <0|1>" },
  { "debugespnowtopo", "Debug ESP-NOW topology discovery.", true, cmd_debugespnowtopo, "Usage: debugespnowtopo <0|1>" },
  { "debugespnowencryption", "Debug ESP-NOW encryption.", true, cmd_debugespnowencryption, "Usage: debugespnowencryption <0|1>" },
  { "debugespnowmetadata", "Debug ESP-NOW metadata exchange (REQ/RESP/PUSH).", true, cmd_debugespnowmetadata, "Usage: debugespnowmetadata <0|1>" },
  { "debugautoscheduler", "Debug automations scheduler.", true, cmd_debugautoscheduler, "Usage: debugautoscheduler <0|1>" },
  { "debugautoexec", "Debug automations execution.", true, cmd_debugautoexec, "Usage: debugautoexec <0|1>" },
  { "debugautocondition", "Debug automations conditions.", true, cmd_debugautocondition, "Usage: debugautocondition <0|1>" },
  { "debugautotiming", "Debug automations timing.", true, cmd_debugautotiming, "Usage: debugautotiming <0|1>" },
  { "debugmemory", "Debug memory buffer usage instrumentation.", true, cmd_debugmemory, "Usage: debugmemory <0|1>" },
  { "debugauthsessions", "Debug auth sessions.", true, cmd_debugauthsessions, "Usage: debugauthsessions <0|1>" },
  { "debugauthcookies", "Debug auth cookies.", true, cmd_debugauthcookies, "Usage: debugauthcookies <0|1>" },
  { "debugauthlogin", "Debug auth login.", true, cmd_debugauthlogin, "Usage: debugauthlogin <0|1>" },
  { "debugauthbootid", "Debug auth boot ID.", true, cmd_debugauthbootid, "Usage: debugauthbootid <0|1>" },
  { "debughttphandlers", "Debug HTTP handlers.", true, cmd_debughttphandlers },
  { "debughttprequests", "Debug HTTP requests.", true, cmd_debughttprequests },
  { "debughttpresponses", "Debug HTTP responses.", true, cmd_debughttpresponses },
  { "debughttpstreaming", "Debug HTTP streaming.", true, cmd_debughttpstreaming },
  { "debugwificonnection", "Debug WiFi connection.", true, cmd_debugwificonnection },
  { "debugwificonfig", "Debug WiFi config.", true, cmd_debugwificonfig },
  { "debugwifiscanning", "Debug WiFi scanning.", true, cmd_debugwifiscanning },
  { "debugwifidriver", "Debug WiFi driver.", true, cmd_debugwifidriver },
  { "debugstoragefiles", "Debug storage files.", true, cmd_debugstoragefiles },
  { "debugstoragejson", "Debug storage JSON.", true, cmd_debugstoragejson },
  { "debugstoragesettings", "Debug storage settings.", true, cmd_debugstoragesettings },
  { "debugstoragemigration", "Debug storage migration.", true, cmd_debugstoragemigration },
  { "debugsystemboot", "Debug system boot.", true, cmd_debugsystemboot },
  { "debugsystemconfig", "Debug system config.", true, cmd_debugsystemconfig },
  { "debugsystemtasks", "Debug system tasks.", true, cmd_debugsystemtasks },
  { "debugsystemhardware", "Debug system hardware.", true, cmd_debugsystemhardware },
  { "debugusersmgmt", "Debug users management.", true, cmd_debugusersmgmt },
  { "debugusersregister", "Debug users registration.", true, cmd_debugusersregister },
  { "debugusersquery", "Debug users query.", true, cmd_debugusersquery },
  { "debugcliexecution", "Debug CLI execution.", true, cmd_debugcliexecution },
  { "debugcliqueue", "Debug CLI queue.", true, cmd_debugcliqueue },
  { "debugclivalidation", "Debug CLI validation.", true, cmd_debugclivalidation },
  { "debugperfstack", "Debug performance stack.", true, cmd_debugperfstack },
  { "debugperfheap", "Debug performance heap.", true, cmd_debugperfheap },
  { "debugperftiming", "Debug performance timing.", true, cmd_debugperftiming },
  { "debugsseconnection", "Debug SSE connection.", true, cmd_debugsseconnection },
  { "debugsseevents", "Debug SSE events.", true, cmd_debugsseevents },
  { "debugssebroadcast", "Debug SSE broadcast.", true, cmd_debugssebroadcast },
  { "debugcmdflowrouting", "Debug command flow routing.", true, cmd_debugcmdflowrouting },
  { "debugcmdflowqueue", "Debug command flow queue.", true, cmd_debugcmdflowqueue },
  { "debugcmdflowcontext", "Debug command flow context.", true, cmd_debugcmdflowcontext },
  { "debugcommandsystem", "Debug modular command registry operations.", true, cmd_debugcommandsystem, "Usage: debugcommandsystem <0|1> [temp|runtime]" },
  { "debugsettingssystem", "Debug settings module registration and validation.", true, cmd_debugsettingssystem, "Usage: debugsettingssystem <0|1> [temp|runtime]" },
  { "debugautomations", "Debug automations scheduler and actions.", true, cmd_debugautomations },
  { "debuglogger", "Debug sensor logger internals.", true, cmd_debuglogger },
  { "commandmodulesummary", "Show command module summary.", true, cmd_commandmodulesummary },
  { "settingsmodulesummary", "Show settings module summary.", true, cmd_settingsmodulesummary },
  { "outdisplay", "Enable/disable display output.", true, cmd_outdisplay, "Usage: outdisplay <0|1> [persist|temp]" },
  { "outg2", "Enable/disable G2 glasses output.", false, cmd_outg2, "Usage: outg2 <0|1> - streams CLI output to G2 glasses" },
  { "loglevel", "Set log level (error|warn|info|debug).", true, cmd_loglevel },
  { "log", "System-wide logging to file.", false, cmd_log, "Usage: log <start|stop|status>\n  start [filepath] [flags=0xXXXX] [tags=0|1]: Begin system logging\n    filepath: Log file path (auto-generated if omitted)\n    flags: Debug flags to enable (e.g., flags=0x0203)" },
};

const size_t debugCommandsCount = sizeof(debugCommands) / sizeof(debugCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _debug_cmd_registrar(debugCommands, debugCommandsCount, "debug");

// ============================================================================
// DebugManager Class Implementation (merged from System_Debug_Manager.cpp)
// ============================================================================

DebugManager::DebugManager() {}

DebugManager& DebugManager::getInstance() {
    static DebugManager instance;
    return instance;
}

bool DebugManager::initialize() {
    // Delegate to existing debug system init to avoid duplicated queues/tasks
    initDebugSystem();
    return true;
}

void DebugManager::queueDebugMessage(uint64_t flag, const char* message) {
    if (!message) return;
    DEBUGF_QUEUE(flag, "%s", message);
}

QueueHandle_t DebugManager::getDebugQueue() const {
    return gDebugOutputQueue;
}

QueueHandle_t DebugManager::getDebugFreeQueue() const {
    return gDebugFreeQueue;
}

void DebugManager::incrementDebugDropped() {
    gDebugDropped = gDebugDropped + 1;
}

char* DebugManager::getDebugBuffer() {
    return gDebugBuffer;
}

bool DebugManager::ensureDebugBuffer() {
    return ::ensureDebugBuffer();
}

void DebugManager::shutdown() {
    // Intentionally no-op for now: existing debug system owns queues/tasks.
}

void DebugManager::setDebugFlags(uint64_t flags) { gDebugFlags = flags; }
uint64_t DebugManager::getDebugFlags() const { return gDebugFlags; }

void DebugManager::setLogLevel(uint8_t level) { gLogLevel = level; }
uint8_t DebugManager::getLogLevel() const { return gLogLevel; }

void DebugManager::setSystemLogEnabled(bool enabled) { gSystemLogEnabled = enabled; }
bool DebugManager::isSystemLogEnabled() const { return gSystemLogEnabled; }

void DebugManager::setLogCategoryTags(bool enabled) { gSystemLogCategoryTags = enabled; }
bool DebugManager::getLogCategoryTags() const { return gSystemLogCategoryTags; }

// ============================================================================
// Logging System (merged from System_Logging.cpp)
// ============================================================================
// Handles structured logging to LittleFS files with automatic cap enforcement.

// External dependencies
extern bool appendLineWithCap(const char* path, const String& line, size_t capBytes);
extern void getTimestampPrefixMsCached(char* buf, size_t bufSize);
extern void timeSyncUpdateBootEpoch();
extern void writeBootAnchor();
extern void resolvePendingUserCreationTimes();

// Time sync marker flag
bool gTimeSyncedMarkerWritten = false;

// Log File Path Definitions
const char* LOG_OK_FILE = "/system/logs/successful_login.log";              // ~680KB cap
const char* LOG_FAIL_FILE = "/system/logs/failed_login.log";                // ~680KB cap
const char* LOG_I2C_FILE = "/system/logs/i2c_errors.log";                   // 64KB cap

void logToFile(const char* path, const String& line, size_t capBytes) {
  appendLineWithCap(path, line, capBytes);
}

// Log a one-time marker when NTP/RTC becomes valid; safe to call anytime.
void logTimeSyncedMarkerIfReady() {
  if (gTimeSyncedMarkerWritten) {
    return;
  }
  
  time_t t = time(nullptr);
  if (t <= 0) {
    return;
  }
  
  timeSyncUpdateBootEpoch();
  
  static char* bootTsPrefix = nullptr;
  if (!bootTsPrefix) {
    bootTsPrefix = (char*)ps_alloc(48, AllocPref::PreferPSRAM, "boot.ts");
    if (!bootTsPrefix) return;
  }
  
  getTimestampPrefixMsCached(bootTsPrefix, 48);
  String prefix = bootTsPrefix[0] ? String(bootTsPrefix) : String("[BOOT ms=") + String(millis()) + "] | ";
  String line = prefix + "Device Powered On | Time Synced via NTP";
  
  appendLineWithCap(LOG_OK_FILE, line, LOG_CAP_BYTES);
  appendLineWithCap(LOG_FAIL_FILE, line, LOG_CAP_BYTES);
  appendLineWithCap(LOG_I2C_FILE, line, LOG_I2C_CAP);
  
  gTimeSyncedMarkerWritten = true;

  // Write boot anchor and resolve pending user creation timestamps
  writeBootAnchor();
  resolvePendingUserCreationTimes();
}

static String buildTimestampPrefix() {
  char tsPrefix[48];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
  if (tsPrefix[0]) {
    return String(tsPrefix);
  }
  return String("[ms=") + String(millis()) + "] ";
}

void logI2CError(uint8_t address, const char* deviceName, int consecutiveErrors, int totalErrors, bool nowDegraded) {
  String line = buildTimestampPrefix();
  line += "I2C ERROR | addr=0x";
  if (address < 0x10) line += "0";
  line += String(address, HEX);
  line += " | device=";
  line += deviceName ? deviceName : "?";
  line += " | consec=";
  line += String(consecutiveErrors);
  line += " | total=";
  line += String(totalErrors);
  
  if (nowDegraded) {
    line += " | STATUS=DEGRADED";
  }
  
  appendLineWithCap(LOG_I2C_FILE, line, LOG_I2C_CAP);
}

void logI2CRecovery(uint8_t address, const char* deviceName, int totalErrors) {
  String line = buildTimestampPrefix();
  line += "I2C RECOVERED | addr=0x";
  if (address < 0x10) line += "0";
  line += String(address, HEX);
  line += " | device=";
  line += deviceName ? deviceName : "?";
  line += " | total_errors=";
  line += String(totalErrors);
  
  appendLineWithCap(LOG_I2C_FILE, line, LOG_I2C_CAP);
}

// ============================================================================
// System Log Auto-Start (called from boot)
// ============================================================================

void systemLogAutoStart() {
  if (!gSettings.systemLogAutoStart) return;
  if (gSystemLogEnabled) return;  // Already running
  
  // Auto-generate log filename with timestamp
  String filepath = generateSystemLogFilename();
  
  // Ensure any previous file handle is closed (safety check)
  if (gSystemLogFile) {
    fsLock("debug.log");
    gSystemLogFile.flush();
    gSystemLogFile.close();
    fsUnlock();
  }
  
  // Create the log file
  fsLock("debug.log");
  File f = LittleFS.open(filepath.c_str(), "w");
  if (!f) {
    fsUnlock();
    broadcastOutput("[SYSTEM_LOG] Auto-start failed: Could not create file: " + filepath);
    return;
  }
  f.printf("# System log auto-started at %lu ms\n", millis());
  f.close();
  fsUnlock();
  
  gSystemLogPath = filepath;
  gSystemLogEnabled = true;
  gSystemLogLastWrite = millis();
  gOutputFlags |= OUTPUT_FILE;
  
  broadcastOutput("[SYSTEM_LOG] Auto-start enabled, logging to: " + filepath);
}
