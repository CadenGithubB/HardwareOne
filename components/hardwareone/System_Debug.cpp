#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_attr.h>
#include <esp_log.h>

#include "System_BuildConfig.h"
#include "System_Filesystem.h"
#include "System_VFS.h"
#include "OLED_ConsoleBuffer.h"
#include "Bluetooth.h"
#include "G2_Glasses.h"
#include "System_CLI.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Logging.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"
#include "System_AuthIdentity.h"  // currentCommandContext / currentCaptureState
#include "WebServer_Utils.h"
#include "System_ESPSR.h"  // srSyncDebugLevel()

// External dependencies from .ino

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
DebugFlagMask gDebugFlags = (DebugFlagMask)0x00000000FFFFFFFFULL;
DebugSubFlags gDebugSubFlags = {}; // All sub-flags initialized to false
char* gDebugBuffer = nullptr;
QueueHandle_t gDebugOutputQueue = nullptr;
QueueHandle_t gDebugFreeQueue = nullptr;
volatile unsigned long gDebugDropped = 0;
int gDebugQueueSize = DEBUG_QUEUE_SIZE_MIN; // Runtime queue size (set in initDebugSystem)

volatile bool gDebugVerbose = false;

// Low-level stack/heap trace toggle — see STACK_TRACEF in System_Debug.h.
// Runtime-only: not persisted, resets to false on reboot.
volatile bool gDebugStackTraceEnabled = false;

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

// BLE broadcast output buffer (accumulates messages for periodic send to authenticated BLE clients)
#if ENABLE_BLUETOOTH
static String gBLEOutputBuffer;
static unsigned long gBLELastFlush = 0;
static const uint32_t BLE_OUTPUT_FLUSH_INTERVAL_MS = 500;  // Send to BLE every 500ms
static const size_t BLE_OUTPUT_BUFFER_MAX = 512;            // Max chars to buffer (BLE MTU limited)
#endif

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

// Web mirror buffer access - defined in WebServer_Utils.h

// Suppressed tail ring buffer
static const size_t kHelpTailLines = 32;
static const size_t kHelpTailCols = 120;
EXT_RAM_BSS_ATTR static char gHelpTail[kHelpTailLines][kHelpTailCols];
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

// Forward declaration (definition with I2C helpers below)
static String buildTimestampPrefix();

// Debug output task - single writer for all debug messages
static TaskHandle_t gDebugOutputTaskHandle = nullptr;

void debugOutputTask(void* parameter) {
  while (true) {
    DebugMessage* msg = nullptr;
    if (xQueueReceive(gDebugOutputQueue, &msg, portMAX_DELAY) == pdTRUE && msg) {
      // Help-mode gating for queued messages (allow security/auth/error)
      if (gCLIState != CLI_NORMAL && !gInHelpRender) {
        if ((msg->routing & MSG_ROUTE_ALLOW_IN_HELP) == 0 &&
            !(strncmp(msg->text, "[SECURITY]", 10) == 0 || strncmp(msg->text, "[AUTH]", 6) == 0 ||
              strncmp(msg->text, "[ERROR]", 7) == 0)) {
          gHelpSuppressedCount = gHelpSuppressedCount + 1;  // (= x+1: ++ on volatile is deprecated in C++20)
          pushHelpSuppressed(msg->text);
          if (gDebugFreeQueue) {
            xQueueSend(gDebugFreeQueue, &msg, 0);
          }
          continue; // Drop from sinks to avoid overwriting help UI
        }
      }

      // Surface dropped-message bursts so queue saturation is never silent.
      // Rate-limited to one marker per second; a burst coalesces into one line.
      // Emitted straight to Serial + web mirror from this (single) consumer task,
      // so the marker itself can't be dropped or race another UART writer.
      {
        static unsigned long sDropSeen   = 0;
        static unsigned long sDropMarkMs = 0;
        unsigned long dropped = gDebugDropped;          // volatile read
        if (dropped != sDropSeen) {
          unsigned long nowMs = millis();
          if (sDropMarkMs == 0 || (nowMs - sDropMarkMs) >= 1000UL) {
            char mark[96];
            int mw = snprintf(mark, sizeof(mark),
                              "[%lu] [output] %lu line(s) dropped (queue saturated)",
                              nowMs, dropped - sDropSeen);
            if (mw > 0) {
              if (gOutputFlags & OUTPUT_SERIAL) Serial.printf("%s\n", mark);
              if (gWebMirror.buf) gWebMirror.appendDirect(mark, (size_t)mw, true);
            }
            sDropSeen   = dropped;
            sDropMarkMs = nowMs;
          }
        }
      }

      // --- Per-sink output gated by msg->routing AND hardware availability ---

      // Serial
      if ((msg->routing & MSG_ROUTE_SERIAL) && (gOutputFlags & OUTPUT_SERIAL)) {
        Serial.printf("[%lu] %s\n", msg->timestamp, msg->text);
      }
      // Web mirror (circular buffer for /api/cli/logs polling)
      if ((msg->routing & MSG_ROUTE_WEB) && gWebMirror.buf) {
        char formattedMsg[DEBUG_MSG_SIZE + 32];
        int written = snprintf(formattedMsg, sizeof(formattedMsg), "[%lu] %s", msg->timestamp, msg->text);
        if (written > 0) {
          gWebMirror.appendDirect(formattedMsg, (size_t)written, true);
        }
      }
      // File output (system log)
      if ((msg->routing & MSG_ROUTE_FILE) && (gOutputFlags & OUTPUT_FILE) &&
          gSystemLogEnabled && gSystemLogPath.length() > 0) {
        fsLock("debug.log");
        if (!gSystemLogFile) {
          gSystemLogFile = VFS::openGuarded(gSystemLogPath, "a", VFS::systemAuth("debug.system_log_append"), true);
          if (gSystemLogFile) {
            gSystemLogLastFlush = millis();
            gSystemLogUnflushedCount = 0;
          }
        }
        if (gSystemLogFile) {
          if (gSystemLogCategoryTags && msg->category != 0) {
            const char* cat = getDebugCategoryName(msg->category);
            gSystemLogFile.printf("[%lu] [%s] %s\n", msg->timestamp, cat, msg->text);
          } else {
            gSystemLogFile.printf("[%lu] %s\n", msg->timestamp, msg->text);
          }
          gSystemLogLastWrite = millis();
          gSystemLogUnflushedCount++;
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

      // Error ring buffer (always, independent of routing — errors are always logged).
      //
      // Dedupe: suppress file writes for identical [ERROR] lines that arrive
      // within a short window. A flood of repeating messages (e.g. cam_hal
      // FB-OVF bursts, empty-command spam) would otherwise trigger a heavy
      // log rotation on every single occurrence, which once caused a stack
      // overflow in this task. Serial + web mirror already ran above, so
      // skipping the file append only suppresses the on-disk duplicate.
      if (filesystemReady && strncmp(msg->text, "[ERROR]", 7) == 0) {
        static char     lastErrText[64] = {0};
        static uint32_t lastErrTimeMs   = 0;
        const uint32_t  now             = millis();
        const uint32_t  DEDUPE_WINDOW_MS = 2000;

        bool isDup = (now - lastErrTimeMs < DEDUPE_WINDOW_MS) &&
                     (strncmp(lastErrText, msg->text, sizeof(lastErrText) - 1) == 0);

        STACK_TRACEF("debug_out.err_entry dup=%d text=%.40s", isDup ? 1 : 0, msg->text);

        if (!isDup) {
          String line = buildTimestampPrefix();
          line += msg->text;
          STACK_TRACEF("debug_out.before_appendLineWithCap");
          appendLineWithCap(LOG_ERROR_FILE, line, LOG_ERROR_CAP);
          STACK_TRACEF("debug_out.after_appendLineWithCap");
          strncpy(lastErrText, msg->text, sizeof(lastErrText) - 1);
          lastErrText[sizeof(lastErrText) - 1] = '\0';
          lastErrTimeMs = now;
        }
      }

      // OLED console
      #if ENABLE_OLED_DISPLAY
      if ((msg->routing & MSG_ROUTE_OLED) && gOledConsole.mutex) {
        gOledConsole.append(msg->text, msg->timestamp);
      }
      #endif

      // BLE broadcast output
      #if ENABLE_BLUETOOTH
      if ((msg->routing & MSG_ROUTE_BLE) && (gOutputFlags & OUTPUT_BLE) &&
          isBLEConnected() && bleHasAuthenticatedSession()) {
        size_t msgLen = strlen(msg->text);
        if (gBLEOutputBuffer.length() + msgLen + 2 < BLE_OUTPUT_BUFFER_MAX) {
          gBLEOutputBuffer += msg->text;
          gBLEOutputBuffer += "\n";
        }
        unsigned long now = millis();
        if (gBLEOutputBuffer.length() >= BLE_OUTPUT_BUFFER_MAX - 50 ||
            (now - gBLELastFlush >= BLE_OUTPUT_FLUSH_INTERVAL_MS && gBLEOutputBuffer.length() > 0)) {
          sendBLEResponse(gBLEOutputBuffer.c_str(), gBLEOutputBuffer.length());
          gBLEOutputBuffer = "";
          gBLELastFlush = now;
        }
      }
      #endif

      // G2 glasses output
      #if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
      if ((msg->routing & MSG_ROUTE_G2) && (gOutputFlags & OUTPUT_G2) && isG2Connected()) {
        if (gG2OutputBuffer.length() + strlen(msg->text) + 2 < G2_BUFFER_MAX) {
          gG2OutputBuffer += msg->text;
          gG2OutputBuffer += "\n";
        }
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

// Boot-time backpressure for bursty debug output. When a tight loop emits a
// long run of lines (e.g. the command-registry dump of 500+ commands), the
// producer can outrun the single debugOutputTask and overflow the free-list
// pool, silently dropping the tail. Call this periodically inside such a loop:
// it yields until the free pool recovers above `minFree` slots, bounded by
// `maxWaitMs` so it can never hang boot. It's a no-op once the pool is healthy
// or before the queue is up — so it costs nothing when the relevant debug flag
// is off (no lines are produced, the pool stays full).
void debugQueueBackpressure(int minFree, uint32_t maxWaitMs) {
  if (!gDebugFreeQueue) return;
  uint32_t start = millis();
  while ((int)uxQueueMessagesWaiting(gDebugFreeQueue) < minFree) {
    if ((uint32_t)(millis() - start) >= maxWaitMs) break;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

const char* cmd_loglevel(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  String valStr = argsInput;
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

  // Also update ESP-IDF framework component log levels
  {
    esp_log_level_t espLevel;
    switch (newLevel) {
      case LOG_LEVEL_ERROR: espLevel = ESP_LOG_ERROR; break;
      case LOG_LEVEL_WARN:  espLevel = ESP_LOG_WARN;  break;
      case LOG_LEVEL_INFO:  espLevel = ESP_LOG_INFO;  break;
      default:              espLevel = ESP_LOG_DEBUG; break;
    }
    // TLS/HTTPS tags (esp-tls-mbedtls, esp_https_server, httpd, httpd_txrx)
    // are owned by the DEBUG_HTTPS flag (applyHttpsLogLevels), not the global
    // log level — so `loglevel debug` can't re-enable the disconnect flood.
    esp_log_level_set("wifi", espLevel);
    esp_log_level_set("wifi_init", espLevel);
    esp_log_level_set("phy_init", espLevel);
  }

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
    gDebugBuffer = (char*)ps_alloc(GLOBAL_DEBUG_BUFFER_SIZE, AllocPref::PreferPSRAM, "debug.buf");
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
      DEBUG_OUT_STACK_WORDS,  // ~14KB stack — see System_TaskUtils.h for sizing rationale
      nullptr,
      TASK_PRIORITY_LOW,
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
  
  // Preallocate BLE output buffer to prevent incremental reallocation
  #if ENABLE_BLUETOOTH
  gBLEOutputBuffer.reserve(BLE_OUTPUT_BUFFER_MAX);
  #endif
  
  // Initialize OLED console buffer
  #if ENABLE_OLED_DISPLAY
  gOledConsole.init();
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
    gDebugBuffer = (char*)ps_alloc(GLOBAL_DEBUG_BUFFER_SIZE, AllocPref::PreferPSRAM, "debug.buf");
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

// Stamp a visible "[CUT]" marker on a line that didn't fit in DEBUG_MSG_SIZE,
// so truncation is never silent. Caller must have NUL-terminated text[] first;
// this overwrites the tail [250..254] and leaves the NUL at [255] intact.
static inline void markTrunc(char* text, bool truncated) {
  if (truncated) memcpy(&text[DEBUG_MSG_SIZE - 6], "[CUT]", 5);
}

// ── Unified message enqueue (the single producer primitive) ──────────────────
// THE one place a DebugMessage is grabbed, filled, queued, and drop-accounted.
// ISR-safe (uses the *FromISR APIs in interrupt context). Copies up to `len`
// bytes of `text` (a slice need not be NUL-terminated); over-long input is
// clamped and marked [CUT]. Returns false if the message was dropped (no free
// slot / queue full). Every output path funnels through here so they all behave
// identically — and drops go through incrementDebugDropped() (the volatile-safe
// counter the consumer's drop-marker reads).
static bool enqueueChunk(const char* text, size_t len, uint8_t routing,
                         DebugFlagMask category, bool truncated) {
  if (!gDebugOutputQueue || !gDebugFreeQueue) return false;
  const bool isr = xPortInIsrContext();

  DebugMessage* msg = nullptr;
  BaseType_t got = isr ? xQueueReceiveFromISR(gDebugFreeQueue, &msg, NULL)
                       : xQueueReceive(gDebugFreeQueue, &msg, 0);
  if (got != pdTRUE || !msg) { incrementDebugDropped(); return false; }

  size_t copy = len < (size_t)(DEBUG_MSG_SIZE - 1) ? len : (size_t)(DEBUG_MSG_SIZE - 1);
  msg->timestamp = millis();
  msg->category  = category;
  msg->routing   = routing;
  memcpy(msg->text, text, copy);
  msg->text[copy] = '\0';
  markTrunc(msg->text, truncated || len > (size_t)(DEBUG_MSG_SIZE - 1));

  BaseType_t sent = isr ? xQueueSendFromISR(gDebugOutputQueue, &msg, NULL)
                        : xQueueSend(gDebugOutputQueue, &msg, 0);
  if (sent != pdTRUE) {
    if (isr) xQueueSendFromISR(gDebugFreeQueue, &msg, NULL);
    else     xQueueSend(gDebugFreeQueue, &msg, 0);
    incrementDebugDropped();
    return false;
  }
  return true;
}

void debugQueuePrintf(DebugFlagMask flag, const char* fmt, ...) {
  if (!fmt) return;
  if (!getDebugQueue() || !getDebugFreeQueue()) return;

  // CRITICAL: Check if we're in a sensor task that's shutting down
  extern bool gThermalEnabled, gImuEnabled, gTofEnabled, gFmRadioEnabled;
  extern TaskHandle_t gThermalTaskHandle, gImuTaskHandle, gTofTaskHandle, gFmRadioTaskHandle;
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  if (currentTask == gThermalTaskHandle && !gThermalEnabled) return;
  if (currentTask == gImuTaskHandle && !gImuEnabled) return;
  if (currentTask == gTofTaskHandle && !gTofEnabled) return;
  if (currentTask == gFmRadioTaskHandle && !gFmRadioEnabled) return;

  // Format into a stack line, then hand to the shared enqueue primitive
  // (ISR-safe; centralizes slot grab / send / drop + [CUT]). DEBUG_*F is
  // single-line, so it uses enqueueChunk directly rather than the split layer.
  char line[DEBUG_MSG_SIZE];
  va_list args;
  va_start(args, fmt);
  int wn = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (wn < 0) return;
  size_t llen = (wn < (int)sizeof(line)) ? (size_t)wn : (sizeof(line) - 1);
  enqueueChunk(line, llen, MSG_ROUTE_ALL, flag, wn >= (int)DEBUG_MSG_SIZE);
}

// ============================================================================
// Broadcast Output Functions
// ============================================================================

// Per-task current command context lives in System_AuthIdentity's TLS slot.
// Read via currentCommandContext() (returns void*); the outputMask helper
// is getCurrentCommandOutputMask() in HardwareOne.cpp.
extern uint32_t getCurrentCommandOutputMask();

// Output capture state lives in the calling task's TLS slot too (Stage 3).
// Access via currentCaptureState() — returns nullptr for tasks that never
// set a capture buffer, in which case broadcastOutput skips capture.

// ============================================================================
// broadcastOutputCore — single implementation for all broadcast overloads
// ============================================================================
//
// All broadcast output funnels through here. The public overloads
// (String, const char*) are thin wrappers that call this.
//
// routeOverride: when non-zero, used as the MSG_ROUTE_* mask directly.
//   When zero, route is computed from currentCommandContext()->outputMask
//   for the calling task (or MSG_ROUTE_ALL if no command context is active
//   on this task — most non-cmd_exec tasks).
//
static void broadcastOutputCore(const char* text, size_t len, uint8_t routeOverride) {
  // 1. Suppress output in validation mode
  if (gCLIValidateOnly) return;

  // 2. Capture output if active (used for HTTP response with capture=1).
  //    Per-task state — only the task that ran setCaptureBuffer() captures
  //    its own broadcasts. Other tasks' broadcasts are not appended into
  //    this buffer, fixing the cross-task contamination bug.
  if (CaptureBufState* cap = currentCaptureState();
      cap && cap->buf && cap->len < cap->cap) {
    size_t avail = cap->cap - cap->len - 1;  // leave room for NUL
    size_t copyLen = len < avail ? len : avail;
    if (copyLen > 0) {
      memcpy(cap->buf + cap->len, text, copyLen);
      cap->len += copyLen;
      if (cap->len < cap->cap - 1) {
        cap->buf[cap->len++] = '\n';
      }
      cap->buf[cap->len] = '\0';
    }
  }

  // 3. Help-mode gating: drop non-help-render output while help UI is active,
  //    but allow security/auth notices to pass through
  if (gCLIState != CLI_NORMAL && !gInHelpRender) {
    if (!(strncmp(text, "[SECURITY]", 10) == 0 || strncmp(text, "[AUTH]", 6) == 0)) {
      gHelpSuppressedCount = gHelpSuppressedCount + 1;  // (= x+1: ++ on volatile is deprecated in C++20)
      pushHelpSuppressed(text);
      return;
    }
  }

  // 4. Skip output if current task is a sensor task that's been disabled
  extern bool gThermalEnabled, gImuEnabled, gTofEnabled;
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  extern TaskHandle_t gThermalTaskHandle, gImuTaskHandle, gTofTaskHandle;
  if (currentTask == gThermalTaskHandle && !gThermalEnabled) return;
  if (currentTask == gImuTaskHandle && !gImuEnabled) return;
  if (currentTask == gTofTaskHandle && !gTofEnabled) return;

  // 5. Compute per-message route mask
  uint8_t route;
  if (routeOverride) {
    route = routeOverride;
  } else if (currentCommandContext()) {
    // Map CMD_OUT_* to MSG_ROUTE_* (bits 0-2 and 4 are aligned by design).
    // Reads the calling task's slot; broadcasts from non-cmd_exec tasks
    // see nullptr and fall through to MSG_ROUTE_ALL below.
    uint32_t mask = getCurrentCommandOutputMask();
    route = (uint8_t)(mask & (MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE))
          | MSG_ROUTE_OLED | MSG_ROUTE_G2;  // OLED and G2 always receive command output
  } else {
    route = MSG_ROUTE_ALL;  // non-command output goes to all sinks
  }
  if (gInHelpRender) route |= MSG_ROUTE_ALLOW_IN_HELP;

  // 6. Enqueue — one broadcastOutput() call == one queue message, verbatim.
  //    No splitting/packing: text goes in as sent. Over-long text (> 255 B) is
  //    clamped and marked [CUT] — a noted platform limit; callers that want
  //    several lines in one envelope pack them with '\n' at the call site.
  enqueueChunk(text, len, route, /*category=*/0, len > (size_t)(DEBUG_MSG_SIZE - 1));

  // 7. ESP-NOW V3 session streaming (extern to avoid circular deps)
#if ENABLE_ESPNOW
  extern uint32_t gCurrentStreamCmdId;
  extern void sendEspNowStreamMessage(const String& message);
  if (gCurrentStreamCmdId != 0) {
    sendEspNowStreamMessage(String(text));
  }
#endif
}

// --- Public overloads: thin wrappers around broadcastOutputCore ---

void broadcastOutput(const String& s) {
  broadcastOutputCore(s.c_str(), s.length(), 0);
}

void broadcastOutput(const char* s) {
  if (!s) return;
  broadcastOutputCore(s, strlen(s), 0);
}

// Explicit-route entry point for context-aware overload (HardwareOne.cpp)
// Called when the caller already knows the route (e.g. after
// clearCurrentCommandContext, where falling back to MSG_ROUTE_ALL wouldn't
// match the just-finished command).
void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route) {
  broadcastOutputCore(text, len, route);
}

// ============================================================================
// ESP-IDF log bridge — route ESP_LOGx output (wifi:/cam_hal:/i2c: ...) through
// the firmware's single output queue so the IDF logger and broadcastOutput
// share ONE UART writer (debug_out). Without it they are two unsynchronized
// writers to the same UART and interleave mid-byte (e.g. "PSRAM" -> "PS7391").
//
// Deliberately dumb: format -> strip ANSI color -> trim trailing newline ->
// direct free-list enqueue. It does NOT go through broadcastOutputCore (so it
// skips the capture buffer, help-gating, and especially the SYNCHRONOUS
// ESP-NOW stream send — none of which are safe to run from the wifi task), and
// it makes NO ESP_LOG calls, so it cannot recurse. ISR / queue-not-ready
// callers fall back to the previous (default) handler. Default OFF; armed via
// the `loglink` command so it can be A/B'd on hardware without reflashing.
// ============================================================================
static vprintf_like_t s_prevLogVprintf = nullptr;
static volatile bool  s_idfBridgeOn    = false;

// Strip ANSI CSI (color) escape sequences in place: ESC '[' params final-byte.
static void stripAnsiInPlace(char* s) {
  char* w = s;
  for (char* r = s; *r; ) {
    if (*r == '\033') {                              // ESC
      r++;
      if (*r == '[') {                               // CSI introducer
        r++;
        while (*r && !(*r >= '@' && *r <= '~')) r++; // skip params/intermediates
        if (*r) r++;                                 // skip the final byte
      }
      continue;                                      // drop the sequence
    }
    *w++ = *r++;
  }
  *w = '\0';
}

static int idfLogVprintf(const char* fmt, va_list ap) {
  // Decide BEFORE consuming ap (a va_list can only be walked once). In ISR
  // context, or before the queue exists, behave exactly like the old handler.
  if (!s_idfBridgeOn || xPortInIsrContext() || !gDebugOutputQueue || !gDebugFreeQueue) {
    return s_prevLogVprintf ? s_prevLogVprintf(fmt, ap) : vprintf(fmt, ap);
  }
  char buf[DEBUG_MSG_SIZE];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  int len = (n > 0 && n < (int)sizeof(buf)) ? n : (int)strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
  stripAnsiInPlace(buf);
  if (buf[0] == '\0') return n;                       // nothing left (e.g. a bare "\n")

  // One IDF log line == one queue message, routed to Serial+web+file only
  // (not OLED/G2/BLE). strlen(buf) is the post-strip length (always <= 255).
  enqueueChunk(buf, strlen(buf), MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE, /*category=*/0, false);
  return n;
}

// Install (or restore) the bridge. Arms the flag BEFORE swapping the handler
// so a log racing the install never sees a half-armed state.
static void setIdfLogBridge(bool enable) {
  if (enable && !s_idfBridgeOn) {
    s_idfBridgeOn = true;
    s_prevLogVprintf = esp_log_set_vprintf(idfLogVprintf);
  } else if (!enable && s_idfBridgeOn) {
    esp_log_set_vprintf(s_prevLogVprintf ? s_prevLogVprintf : vprintf);
    s_idfBridgeOn = false;
  }
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

const char* cmd_outdisplay(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Syntax:
  //   outdisplay <0|1> [persist|temp]
  //   outdisplay [persist|temp] <0|1>
  CommandArgs ca(argsInput);
  String t1 = ca.arg(0);
  String t2 = ca.arg(1);
  bool modeTemp = false;  // default persist
  int v = -1;
  if (t1.length() && (t1 == "temp" || t1 == "persist")) {
    modeTemp = (t1 == "temp");
    if (t2.length()) v = t2.toInt();
  } else {
    if (t1.length()) v = t1.toInt();
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

const char* cmd_outg2(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  #if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  // Syntax: outg2 <0|1>
  String a = argsInput;
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
             enabled ? "yes" : "no", connected ? "yes" : "no");
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

const char* cmd_outble(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  #if ENABLE_BLUETOOTH
  String a = argsInput;
  a.trim();
  int v = -1;
  if (a.length()) {
    v = a.toInt();
    if (a != "0" && v == 0) v = -1;
  }
  if (v < 0) {
    bool enabled = (gOutputFlags & OUTPUT_BLE) != 0;
    bool connected = isBLEConnected();
    bool authed = bleHasAuthenticatedSession();
    static char buf[160];
    snprintf(buf, sizeof(buf), "BLE output: %s, BLE connected: %s, authenticated: %s",
             enabled ? "yes" : "no", connected ? "yes" : "no", authed ? "yes" : "no");
    return buf;
  }
  if (v) {
    gOutputFlags |= OUTPUT_BLE;
    return "BLE broadcast output enabled (messages will stream to authenticated BLE clients)";
  } else {
    gOutputFlags &= ~OUTPUT_BLE;
    return "BLE broadcast output disabled";
  }
  #else
  return "Bluetooth not compiled (ENABLE_BLUETOOTH=0)";
  #endif
}

// Forward decl: helper defined below. Used by parent-flag handlers (here) and
// sub-flag handlers (further down).
static const char* cmd_debugsubflag_impl(const String& argsInput, bool* settingPtr,
                                         DebugFlagMask flagBit, const char* name);

const char* cmd_debughttp(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugHttp, DEBUG_HTTP, "debugHttp");
}

// Set the ESP-IDF log level for the TLS/HTTPS framework tags. These emit a
// flood of benign noise when a browser hits a self-signed cert (handshake
// rejects, connection resets, per-chunk write errors) — much of it at ERROR
// level, so the global loglevel can't suppress it. verbose=false → NONE,
// verbose=true → DEBUG. Owned by DEBUG_HTTPS.
void applyHttpsLogLevels(bool verbose) {
  esp_log_level_t lvl = verbose ? ESP_LOG_DEBUG : ESP_LOG_NONE;
  esp_log_level_set("esp-tls-mbedtls", lvl);
  esp_log_level_set("esp_https_server", lvl);
  esp_log_level_set("httpd", lvl);
  esp_log_level_set("httpd_txrx", lvl);
}

const char* cmd_debughttps(const String& a) {
  const char* r = cmd_debugsubflag_impl(a, &gSettings.debugHttps, DEBUG_HTTPS, "debugHttps");
  // Skip the side effect during the CLI validation pass (cmd_debugsubflag_impl
  // returns "VALID" without mutating state when gCLIValidateOnly is set).
  extern bool gCLIValidateOnly;
  if (!gCLIValidateOnly) applyHttpsLogLevels(isDebugFlagSet(DEBUG_HTTPS));
  return r;
}

const char* cmd_debugsse(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugSse, DEBUG_SSE, "debugSse");
}

const char* cmd_debugcli(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugCli, DEBUG_CLI, "debugCli");
}

// Generic single-flag handler shared by parent and sub-flag debug commands.
// Toggles the bool setting + the bit, optional persist/temp mode. Sub-flag
// handlers pass child DEBUG_* bits; parent handlers pass parent DEBUG_*.
// Does NOT aggregate to a parent flag -- granular sub-toggles let users
// light up "Capture only" without dragging in unrelated noise. The
// DEBUG_<feature>_*F macros gate on `DEBUG_<parent> | DEBUG_<sub>`, so
// the parent toggle still acts as a master switch when desired.
static const char* cmd_debugsubflag_impl(const String& argsInput, bool* settingPtr,
                                         DebugFlagMask flagBit, const char* name) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[96];
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(flagBit);
    else clearDebugFlag(flagBit);
    snprintf(buf, sizeof(buf), "%s %s (runtime only)", name, v ? "enabled" : "disabled");
    return buf;
  }
  setSetting(*settingPtr, (bool)(v != 0));
  if (v) setDebugFlag(flagBit);
  else clearDebugFlag(flagBit);
  snprintf(buf, sizeof(buf), "%s %s (persistent)", name, *settingPtr ? "enabled" : "disabled");
  return buf;
}

const char* cmd_debugcamera(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugCamera, DEBUG_CAMERA, "debugCamera");
}

const char* cmd_debugcameralifecycle(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugCameraLifecycle, DEBUG_CAMERA_LIFECYCLE, "debugCameraLifecycle");
}
const char* cmd_debugcameracapture(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugCameraCapture, DEBUG_CAMERA_CAPTURE, "debugCameraCapture");
}
const char* cmd_debugcamerasettings(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugCameraSettings, DEBUG_CAMERA_SETTINGS, "debugCameraSettings");
}
const char* cmd_debugcameravideo(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugCameraVideo, DEBUG_CAMERA_VIDEO, "debugCameraVideo");
}

const char* cmd_debugmqtt(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMqtt, DEBUG_MQTT, "debugMqtt");
}
const char* cmd_debugmqttconnection(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMqttConnection, DEBUG_MQTT_CONNECTION, "debugMqttConnection");
}
const char* cmd_debugmqttpubsub(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMqttPubsub, DEBUG_MQTT_PUBSUB, "debugMqttPubsub");
}
const char* cmd_debugmqttdiscovery(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMqttDiscovery, DEBUG_MQTT_DISCOVERY, "debugMqttDiscovery");
}
const char* cmd_debugmqttcommands(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMqttCommands, DEBUG_MQTT_COMMANDS, "debugMqttCommands");
}

const char* cmd_debugdisplay(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugDisplay, DEBUG_DISPLAY, "debugDisplay");
}

const char* cmd_debugmicrophone(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMicrophone, DEBUG_MICROPHONE, "debugMicrophone");
}

const char* cmd_debugi2c(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugI2C, DEBUG_I2C, "debugI2C");
}

const char* cmd_debugi2cbus(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugI2CBus, DEBUG_I2C_BUS, "debugI2CBus");
}
const char* cmd_debugi2cdiscovery(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugI2CDiscovery, DEBUG_I2C_DISCOVERY, "debugI2CDiscovery");
}
const char* cmd_debugi2cautostart(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugI2CAutoStart, DEBUG_I2C_AUTOSTART, "debugI2CAutoStart");
}

const char* cmd_debugwifi(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugWifi, DEBUG_WIFI, "debugWifi");
}

const char* cmd_debugstorage(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugStorage, DEBUG_STORAGE, "debugStorage");
}

const char* cmd_debuglogger(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugLogger, DEBUG_LOGGER, "debugLogger");
}

const char* cmd_debugautomations(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugAutomations, DEBUG_AUTOMATIONS, "debugAutomations");
}

const char* cmd_debugperformance(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugPerformance, DEBUG_PERFORMANCE, "debugPerformance");
}

const char* cmd_debugauth(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugAuth, DEBUG_AUTH, "debugAuth");
}

const char* cmd_debugespnow(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugEspNow, DEBUG_ESPNOW_CORE, "debugEspNow");
}

static void syncBluetoothParentFlag() {
  const DebugFlagMask runtime = getDebugFlags();
  const bool runtimeChild = (runtime & (DEBUG_BLUETOOTH_CORE | DEBUG_BLUETOOTH_GATT | DEBUG_BLUETOOTH_DATA)) != (DebugFlagMask)0;
  const bool any = gSettings.debugBluetooth ||
                   gSettings.debugBluetoothCore ||
                   gSettings.debugBluetoothGatt ||
                   gSettings.debugBluetoothData ||
                   runtimeChild;
  updateParentDebugFlag(DEBUG_BLUETOOTH, any);
}

const char* cmd_debugbluetooth(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (v != 0 && v != 1) return "Usage: debugbluetooth <0|1> [temp|runtime]";
  if (!modeTemp) setSetting(gSettings.debugBluetooth, (bool)(v == 1));
  if (v) setDebugFlag(DEBUG_BLUETOOTH);
  else clearDebugFlag(DEBUG_BLUETOOTH);
  syncBluetoothParentFlag();
  if (modeTemp) return v ? "debugBluetooth enabled (runtime only)" : "debugBluetooth disabled (runtime only)";
  return gSettings.debugBluetooth ? "debugBluetooth enabled (persistent)" : "debugBluetooth disabled (persistent)";
}

const char* cmd_debugbluetoothcore(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (v != 0 && v != 1) return "Usage: debugbluetoothcore <0|1> [temp|runtime]";
  if (!modeTemp) setSetting(gSettings.debugBluetoothCore, (bool)(v == 1));
  if (v) setDebugFlag(DEBUG_BLUETOOTH_CORE);
  else clearDebugFlag(DEBUG_BLUETOOTH_CORE);
  syncBluetoothParentFlag();
  if (modeTemp) return v ? "debugBluetoothCore enabled (runtime only)" : "debugBluetoothCore disabled (runtime only)";
  return gSettings.debugBluetoothCore ? "debugBluetoothCore enabled (persistent)" : "debugBluetoothCore disabled (persistent)";
}

const char* cmd_debugbluetoothgatt(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (v != 0 && v != 1) return "Usage: debugbluetoothgatt <0|1> [temp|runtime]";
  if (!modeTemp) setSetting(gSettings.debugBluetoothGatt, (bool)(v == 1));
  if (v) setDebugFlag(DEBUG_BLUETOOTH_GATT);
  else clearDebugFlag(DEBUG_BLUETOOTH_GATT);
  syncBluetoothParentFlag();
  if (modeTemp) return v ? "debugBluetoothGatt enabled (runtime only)" : "debugBluetoothGatt disabled (runtime only)";
  return gSettings.debugBluetoothGatt ? "debugBluetoothGatt enabled (persistent)" : "debugBluetoothGatt disabled (persistent)";
}

const char* cmd_debugbluetoothdata(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (v != 0 && v != 1) return "Usage: debugbluetoothdata <0|1> [temp|runtime]";
  if (!modeTemp) setSetting(gSettings.debugBluetoothData, (bool)(v == 1));
  if (v) setDebugFlag(DEBUG_BLUETOOTH_DATA);
  else clearDebugFlag(DEBUG_BLUETOOTH_DATA);
  syncBluetoothParentFlag();
  if (modeTemp) return v ? "debugBluetoothData enabled (runtime only)" : "debugBluetoothData disabled (runtime only)";
  return gSettings.debugBluetoothData ? "debugBluetoothData enabled (persistent)" : "debugBluetoothData disabled (persistent)";
}

const char* cmd_debugdatetime(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_NTP); else clearDebugFlag(DEBUG_NTP);
    return v ? "debugDateTime enabled (runtime only)" : "debugDateTime disabled (runtime only)";
  } else {
    setSetting(gSettings.debugDateTime, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_NTP); else clearDebugFlag(DEBUG_NTP);
    return gSettings.debugDateTime ? "debugDateTime enabled (persistent)" : "debugDateTime disabled (persistent)";
  }
}

const char* cmd_debugverbose(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  if (ca.count() == 0) return gDebugVerbose ? "debugVerbose is ON" : "debugVerbose is OFF";
  int v = ca.argInt(0, -1);
  if (v != 0 && v != 1) return "Usage: debugverbose <0|1>";
  gDebugVerbose = (v == 1);
  return gDebugVerbose ? "debugVerbose enabled" : "debugVerbose disabled";
}

const char* cmd_debugcommandsystem(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
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

const char* cmd_webconsole(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  int v = valStr.toInt();
  setSetting(gSettings.webConsoleDebug, (bool)(v != 0));
  return gSettings.webConsoleDebug ? "webConsole enabled (persistent)" : "webConsole disabled (persistent)";
}

// Individual I2C sensor debug command handlers
const char* cmd_debuggps(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugGps, DEBUG_GPS, "debugGps");
}

const char* cmd_debugrtc(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugRtc, DEBUG_RTC, "debugRtc");
}

const char* cmd_debugimu(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugImu, DEBUG_IMU, "debugImu");
}

const char* cmd_debugthermal(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugThermal, DEBUG_THERMAL, "debugThermal");
}

const char* cmd_debugtof(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugTof, DEBUG_TOF, "debugTof");
}

const char* cmd_debuginput(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugInput, DEBUG_INPUT, "debugInput");
}

const char* cmd_debuganoencoder(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugAnoEncoder, DEBUG_ANO_ENCODER, "debugAnoEncoder");
}

const char* cmd_debugapds(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugApds, DEBUG_APDS, "debugApds");
}

const char* cmd_debugpresence(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugPresence, DEBUG_PRESENCE, "debugPresence");
}

// Per-sensor sub-flag handlers (Lifecycle / Polling / Values).
// All use the shared cmd_debugsubflag_impl helper.
const char* cmd_debugthermallifecycle(const String& a)  { return cmd_debugsubflag_impl(a, &gSettings.debugThermalLifecycle,  DEBUG_THERMAL_LIFECYCLE,  "debugThermalLifecycle"); }
const char* cmd_debugthermalpolling(const String& a)    { return cmd_debugsubflag_impl(a, &gSettings.debugThermalPolling,    DEBUG_THERMAL_POLLING,    "debugThermalPolling"); }
const char* cmd_debugthermalvalues(const String& a)     { return cmd_debugsubflag_impl(a, &gSettings.debugThermalValues,     DEBUG_THERMAL_VALUES,     "debugThermalValues"); }
const char* cmd_debugtoflifecycle(const String& a)      { return cmd_debugsubflag_impl(a, &gSettings.debugTofLifecycle,      DEBUG_TOF_LIFECYCLE,      "debugTofLifecycle"); }
const char* cmd_debugtofpolling(const String& a)        { return cmd_debugsubflag_impl(a, &gSettings.debugTofPolling,        DEBUG_TOF_POLLING,        "debugTofPolling"); }
const char* cmd_debugtofvalues(const String& a)         { return cmd_debugsubflag_impl(a, &gSettings.debugTofValues,         DEBUG_TOF_VALUES,         "debugTofValues"); }
const char* cmd_debuginputlifecycle(const String& a)  { return cmd_debugsubflag_impl(a, &gSettings.debugInputLifecycle,  DEBUG_INPUT_LIFECYCLE,  "debugInputLifecycle"); }
const char* cmd_debuginputpolling(const String& a)    { return cmd_debugsubflag_impl(a, &gSettings.debugInputPolling,    DEBUG_INPUT_POLLING,    "debugInputPolling"); }
const char* cmd_debuginputvalues(const String& a)     { return cmd_debugsubflag_impl(a, &gSettings.debugInputValues,     DEBUG_INPUT_VALUES,     "debugInputValues"); }
const char* cmd_debuganoencoderlifecycle(const String& a) { return cmd_debugsubflag_impl(a, &gSettings.debugAnoEncoderLifecycle, DEBUG_ANO_ENCODER_LIFECYCLE, "debugAnoEncoderLifecycle"); }
const char* cmd_debuganoencoderpolling(const String& a)   { return cmd_debugsubflag_impl(a, &gSettings.debugAnoEncoderPolling,   DEBUG_ANO_ENCODER_POLLING,   "debugAnoEncoderPolling"); }
const char* cmd_debuganoencodervalues(const String& a)    { return cmd_debugsubflag_impl(a, &gSettings.debugAnoEncoderValues,    DEBUG_ANO_ENCODER_VALUES,    "debugAnoEncoderValues"); }
const char* cmd_debugimulifecycle(const String& a)      { return cmd_debugsubflag_impl(a, &gSettings.debugImuLifecycle,      DEBUG_IMU_LIFECYCLE,      "debugImuLifecycle"); }
const char* cmd_debugimupolling(const String& a)        { return cmd_debugsubflag_impl(a, &gSettings.debugImuPolling,        DEBUG_IMU_POLLING,        "debugImuPolling"); }
const char* cmd_debugimuvalues(const String& a)         { return cmd_debugsubflag_impl(a, &gSettings.debugImuValues,         DEBUG_IMU_VALUES,         "debugImuValues"); }
const char* cmd_debugapdslifecycle(const String& a)     { return cmd_debugsubflag_impl(a, &gSettings.debugApdsLifecycle,     DEBUG_APDS_LIFECYCLE,     "debugApdsLifecycle"); }
const char* cmd_debugapdspolling(const String& a)       { return cmd_debugsubflag_impl(a, &gSettings.debugApdsPolling,       DEBUG_APDS_POLLING,       "debugApdsPolling"); }
const char* cmd_debugapdsvalues(const String& a)        { return cmd_debugsubflag_impl(a, &gSettings.debugApdsValues,        DEBUG_APDS_VALUES,        "debugApdsValues"); }
const char* cmd_debuggpslifecycle(const String& a)      { return cmd_debugsubflag_impl(a, &gSettings.debugGpsLifecycle,      DEBUG_GPS_LIFECYCLE,      "debugGpsLifecycle"); }
const char* cmd_debuggpspolling(const String& a)        { return cmd_debugsubflag_impl(a, &gSettings.debugGpsPolling,        DEBUG_GPS_POLLING,        "debugGpsPolling"); }
const char* cmd_debuggpsvalues(const String& a)         { return cmd_debugsubflag_impl(a, &gSettings.debugGpsValues,         DEBUG_GPS_VALUES,         "debugGpsValues"); }
const char* cmd_debugrtclifecycle(const String& a)      { return cmd_debugsubflag_impl(a, &gSettings.debugRtcLifecycle,      DEBUG_RTC_LIFECYCLE,      "debugRtcLifecycle"); }
const char* cmd_debugrtcpolling(const String& a)        { return cmd_debugsubflag_impl(a, &gSettings.debugRtcPolling,        DEBUG_RTC_POLLING,        "debugRtcPolling"); }
const char* cmd_debugrtcvalues(const String& a)         { return cmd_debugsubflag_impl(a, &gSettings.debugRtcValues,         DEBUG_RTC_VALUES,         "debugRtcValues"); }
const char* cmd_debugfmradiolifecycle(const String& a)  { return cmd_debugsubflag_impl(a, &gSettings.debugFmRadioLifecycle,  DEBUG_FMRADIO_LIFECYCLE,  "debugFmRadioLifecycle"); }
const char* cmd_debugfmradiopolling(const String& a)    { return cmd_debugsubflag_impl(a, &gSettings.debugFmRadioPolling,    DEBUG_FMRADIO_POLLING,    "debugFmRadioPolling"); }
const char* cmd_debugfmradiovalues(const String& a)     { return cmd_debugsubflag_impl(a, &gSettings.debugFmRadioValues,     DEBUG_FMRADIO_VALUES,     "debugFmRadioValues"); }
const char* cmd_debugmiclifecycle(const String& a)      { return cmd_debugsubflag_impl(a, &gSettings.debugMicLifecycle,      DEBUG_MIC_LIFECYCLE,      "debugMicLifecycle"); }
const char* cmd_debugmicpolling(const String& a)        { return cmd_debugsubflag_impl(a, &gSettings.debugMicPolling,        DEBUG_MIC_POLLING,        "debugMicPolling"); }
const char* cmd_debugmicvalues(const String& a)         { return cmd_debugsubflag_impl(a, &gSettings.debugMicValues,         DEBUG_MIC_VALUES,         "debugMicValues"); }
const char* cmd_debugpresencelifecycle(const String& a) { return cmd_debugsubflag_impl(a, &gSettings.debugPresenceLifecycle, DEBUG_PRESENCE_LIFECYCLE, "debugPresenceLifecycle"); }
const char* cmd_debugpresencepolling(const String& a)   { return cmd_debugsubflag_impl(a, &gSettings.debugPresencePolling,   DEBUG_PRESENCE_POLLING,   "debugPresencePolling"); }
const char* cmd_debugpresencevalues(const String& a)    { return cmd_debugsubflag_impl(a, &gSettings.debugPresenceValues,    DEBUG_PRESENCE_VALUES,    "debugPresenceValues"); }

const char* cmd_debugmaps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_MAPS); else clearDebugFlag(DEBUG_MAPS);
    return v ? "debugMaps enabled (runtime only)" : "debugMaps disabled (runtime only)";
  } else {
    setSetting(gSettings.debugMaps, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_MAPS); else clearDebugFlag(DEBUG_MAPS);
    return gSettings.debugMaps ? "debugMaps enabled (persistent)" : "debugMaps disabled (persistent)";
  }
}

const char* cmd_debugmapsloading(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_MAPS_LOADING); else clearDebugFlag(DEBUG_MAPS_LOADING);
    return v ? "debugMapsLoading enabled (runtime only)" : "debugMapsLoading disabled (runtime only)";
  } else {
    setSetting(gSettings.debugMapsLoading, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_MAPS_LOADING); else clearDebugFlag(DEBUG_MAPS_LOADING);
    return gSettings.debugMapsLoading ? "debugMapsLoading enabled (persistent)" : "debugMapsLoading disabled (persistent)";
  }
}

const char* cmd_debugmapsrendering(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_MAPS_RENDERING); else clearDebugFlag(DEBUG_MAPS_RENDERING);
    return v ? "debugMapsRendering enabled (runtime only)" : "debugMapsRendering disabled (runtime only)";
  } else {
    setSetting(gSettings.debugMapsRendering, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_MAPS_RENDERING); else clearDebugFlag(DEBUG_MAPS_RENDERING);
    return gSettings.debugMapsRendering ? "debugMapsRendering enabled (persistent)" : "debugMapsRendering disabled (persistent)";
  }
}

const char* cmd_debugmapsperf(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_MAPS_PERF); else clearDebugFlag(DEBUG_MAPS_PERF);
    return v ? "debugMapsPerf enabled (runtime only)" : "debugMapsPerf disabled (runtime only)";
  } else {
    setSetting(gSettings.debugMapsPerf, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_MAPS_PERF); else clearDebugFlag(DEBUG_MAPS_PERF);
    return gSettings.debugMapsPerf ? "debugMapsPerf enabled (persistent)" : "debugMapsPerf disabled (persistent)";
  }
}

#if ENABLE_ONDEVICE_LLM
static inline void syncLlmParent() {
  updateParentDebugFlag(DEBUG_LLM,
                        gSettings.debugLlm ||
                        gSettings.debugLlmLoad ||
                        gSettings.debugLlmTokenizer ||
                        gSettings.debugLlmForward ||
                        gSettings.debugLlmGenerate ||
                        gSettings.debugLlmMemory);
}

static const char* cmd_debugllm_impl(const String& argsInput, bool* settingPtr, DebugFlagMask flagBit, const char* name) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[96];
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(flagBit);
    else clearDebugFlag(flagBit);
    snprintf(buf, sizeof(buf), "%s %s (runtime only)", name, v ? "enabled" : "disabled");
    return buf;
  }
  setSetting(*settingPtr, (bool)(v != 0));
  if (v) setDebugFlag(flagBit);
  else clearDebugFlag(flagBit);
  syncLlmParent();
  snprintf(buf, sizeof(buf), "%s %s (persistent)", name, *settingPtr ? "enabled" : "disabled");
  return buf;
}

const char* cmd_debugllm(const String& argsInput) {
  return cmd_debugllm_impl(argsInput, &gSettings.debugLlm, DEBUG_LLM, "debugLlm");
}
const char* cmd_debugllmload(const String& argsInput) {
  return cmd_debugllm_impl(argsInput, &gSettings.debugLlmLoad, DEBUG_LLM_LOAD, "debugLlmLoad");
}
const char* cmd_debugllmtokenizer(const String& argsInput) {
  return cmd_debugllm_impl(argsInput, &gSettings.debugLlmTokenizer, DEBUG_LLM_TOKENIZER, "debugLlmTokenizer");
}
const char* cmd_debugllmforward(const String& argsInput) {
  return cmd_debugllm_impl(argsInput, &gSettings.debugLlmForward, DEBUG_LLM_FORWARD, "debugLlmForward");
}
const char* cmd_debugllmgenerate(const String& argsInput) {
  return cmd_debugllm_impl(argsInput, &gSettings.debugLlmGenerate, DEBUG_LLM_GENERATE, "debugLlmGenerate");
}
const char* cmd_debugllmmemory(const String& argsInput) {
  return cmd_debugllm_impl(argsInput, &gSettings.debugLlmMemory, DEBUG_LLM_MEMORY, "debugLlmMemory");
}
#endif

const char* cmd_debugfmradio(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugFmRadio, DEBUG_FMRADIO, "debugFmRadio");
}

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
const char* cmd_debugg2(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugG2, DEBUG_G2, "debugG2");
}

const char* cmd_debugg2lifecycle(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugG2Lifecycle, DEBUG_G2_LIFECYCLE, "debugG2Lifecycle");
}
const char* cmd_debugg2protocol(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugG2Protocol, DEBUG_G2_PROTOCOL, "debugG2Protocol");
}
const char* cmd_debugg2events(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugG2Events, DEBUG_G2_EVENTS, "debugG2Events");
}
const char* cmd_debugg2pages(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugG2Pages, DEBUG_G2_PAGES, "debugG2Pages");
}
const char* cmd_debugg2heartbeat(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugG2Heartbeat, DEBUG_G2_HEARTBEAT, "debugG2Heartbeat");
}
const char* cmd_debugg2dump(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugG2Dump, DEBUG_G2_DUMP, "debugG2Dump");
}
#endif

// ESP-SR debug handlers. Parent (debugSr) is the explicit master switch.
// Sub-flags are independent — they don't aggregate up. Old log sites that
// check gSrDebugLevel still work because srSyncDebugLevel() derives that
// integer from these bools after every change.
const char* cmd_debugsr(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_SR);
    else clearDebugFlag(DEBUG_SR);
#if ENABLE_ESP_SR
    // Runtime-only path: temporarily flip gSettings just long enough for the
    // sync to see it, then restore. Cheaper than a parallel "effective" mask.
    bool prev = gSettings.debugSr;
    gSettings.debugSr = (v != 0);
    srSyncDebugLevel();
    gSettings.debugSr = prev;
#endif
    return v ? "debugSr enabled (runtime only)" : "debugSr disabled (runtime only)";
  } else {
    setSetting(gSettings.debugSr, (bool)(v != 0));
    if (v) setDebugFlag(DEBUG_SR);
    else clearDebugFlag(DEBUG_SR);
#if ENABLE_ESP_SR
    srSyncDebugLevel();
#endif
    return gSettings.debugSr ? "debugSr enabled (persistent)" : "debugSr disabled (persistent)";
  }
}

static const char* cmd_debugsrsub_impl(const String& argsInput, bool* settingPtr,
                                        DebugFlagMask flagBit, const char* name) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[96];
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(flagBit);
    else clearDebugFlag(flagBit);
#if ENABLE_ESP_SR
    bool prev = *settingPtr;
    *settingPtr = (v != 0);
    srSyncDebugLevel();
    *settingPtr = prev;
#endif
    snprintf(buf, sizeof(buf), "%s %s (runtime only)", name, v ? "enabled" : "disabled");
    return buf;
  }
  setSetting(*settingPtr, (bool)(v != 0));
  if (v) setDebugFlag(flagBit);
  else clearDebugFlag(flagBit);
#if ENABLE_ESP_SR
  srSyncDebugLevel();
#endif
  snprintf(buf, sizeof(buf), "%s %s (persistent)", name, *settingPtr ? "enabled" : "disabled");
  return buf;
}

const char* cmd_debugsrwake(const String& a) {
  return cmd_debugsrsub_impl(a, &gSettings.debugSrWake, DEBUG_SR_WAKE, "debugSrWake");
}
const char* cmd_debugsrcommand(const String& a) {
  return cmd_debugsrsub_impl(a, &gSettings.debugSrCommand, DEBUG_SR_COMMAND, "debugSrCommand");
}
const char* cmd_debugsrafe(const String& a) {
  return cmd_debugsrsub_impl(a, &gSettings.debugSrAfe, DEBUG_SR_AFE, "debugSrAfe");
}
const char* cmd_debugsrlifecycle(const String& a) {
  return cmd_debugsrsub_impl(a, &gSettings.debugSrLifecycle, DEBUG_SR_LIFECYCLE, "debugSrLifecycle");
}
const char* cmd_debugsrtuning(const String& a) {
  return cmd_debugsrsub_impl(a, &gSettings.debugSrTuning, DEBUG_SR_TUNING, "debugSrTuning");
}

const char* cmd_memorysampleintervalsec(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) {
    snprintf(gDebugBuffer, 1024, "Memory sample interval: %d sec (0=disabled)", gSettings.memorySampleIntervalSec);
    return gDebugBuffer;
  }
  int v = valStr.toInt();
  if (v < 0 || v > 300) return "Interval must be 0-300 seconds (0=disabled)";
  setSetting(gSettings.memorySampleIntervalSec, v);
  snprintf(gDebugBuffer, 1024, "Memory sample interval set to %d sec%s", v, v == 0 ? " (disabled)" : "");
  return gDebugBuffer;
}

const char* cmd_debugbuffer(const String& argsInput) {
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

const char* cmd_debugcommandflow(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_CMD_FLOW); else clearDebugFlag(DEBUG_CMD_FLOW);
    return v ? "debugCommandFlow enabled (runtime only)" : "debugCommandFlow disabled (runtime only)";
  } else {
    setSetting(gSettings.debugCommandFlow, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_CMD_FLOW); else clearDebugFlag(DEBUG_CMD_FLOW);
    return gSettings.debugCommandFlow ? "debugCommandFlow enabled (persistent)" : "debugCommandFlow disabled (persistent)";
  }
}

const char* cmd_debugusers(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_USERS); else clearDebugFlag(DEBUG_USERS);
    return v ? "debugUsers enabled (runtime only)" : "debugUsers disabled (runtime only)";
  } else {
    setSetting(gSettings.debugUsers, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_USERS); else clearDebugFlag(DEBUG_USERS);
    return gSettings.debugUsers ? "debugUsers enabled (persistent)" : "debugUsers disabled (persistent)";
  }
}

const char* cmd_debugsystem(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_SYSTEM); else clearDebugFlag(DEBUG_SYSTEM);
    return v ? "debugSystem enabled (runtime only)" : "debugSystem disabled (runtime only)";
  } else {
    setSetting(gSettings.debugSystem, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_SYSTEM); else clearDebugFlag(DEBUG_SYSTEM);
    return gSettings.debugSystem ? "debugSystem enabled (persistent)" : "debugSystem disabled (persistent)";
  }
}

const char* cmd_debugespnowstream(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_STREAM); else clearDebugFlag(DEBUG_ESPNOW_STREAM);
    return v ? "debugEspNowStream enabled (runtime only)" : "debugEspNowStream disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowStream, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_STREAM); else clearDebugFlag(DEBUG_ESPNOW_STREAM);
    return gSettings.debugEspNowStream ? "debugEspNowStream enabled (persistent)" : "debugEspNowStream disabled (persistent)";
  }
}

const char* cmd_debugespnowcore(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_CORE); else clearDebugFlag(DEBUG_ESPNOW_CORE);
    return v ? "debugEspNowCore enabled (runtime only)" : "debugEspNowCore disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowCore, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_CORE); else clearDebugFlag(DEBUG_ESPNOW_CORE);
    return gSettings.debugEspNowCore ? "debugEspNowCore enabled (persistent)" : "debugEspNowCore disabled (persistent)";
  }
}

const char* cmd_debugespnowrouter(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_ROUTER); else clearDebugFlag(DEBUG_ESPNOW_ROUTER);
    return v ? "debugEspNowRouter enabled (runtime only)" : "debugEspNowRouter disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowRouter, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_ROUTER); else clearDebugFlag(DEBUG_ESPNOW_ROUTER);
    return gSettings.debugEspNowRouter ? "debugEspNowRouter enabled (persistent)" : "debugEspNowRouter disabled (persistent)";
  }
}

const char* cmd_debugmemory(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_MEMORY); else clearDebugFlag(DEBUG_MEMORY);
    return v ? "debugMemory enabled (runtime only)" : "debugMemory disabled (runtime only)";
  } else {
    setSetting(gSettings.debugMemory, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_MEMORY); else clearDebugFlag(DEBUG_MEMORY);
    return gSettings.debugMemory ? "debugMemory enabled (persistent)" : "debugMemory disabled (persistent)";
  }
}

// Runtime toggle for the ESP-IDF log bridge (setIdfLogBridge above). Default OFF;
// flip ON to route IDF logs through the firmware's single output queue so they
// stop interleaving with our lines on the UART. Runtime-only (not persisted yet)
// so it can be A/B'd live without reflashing.
const char* cmd_loglink(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = argsInput; a.trim();
  if (a.length() == 0) {
    return s_idfBridgeOn
      ? "loglink: ON — ESP-IDF logs routed through the unified output queue"
      : "loglink: OFF — ESP-IDF logs write the UART directly (default)";
  }
  bool on  = (a == "1" || a.equalsIgnoreCase("on")  || a.equalsIgnoreCase("true"));
  bool off = (a == "0" || a.equalsIgnoreCase("off") || a.equalsIgnoreCase("false"));
  if (!on && !off) return "Usage: loglink <0|1|on|off>";
  setIdfLogBridge(on);
  return on
    ? "loglink ON — ESP-IDF logs now share the firmware output queue (no more UART interleave)"
    : "loglink OFF — ESP-IDF logs restored to direct UART";
}

const char* cmd_debugmemoryheap(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMemoryHeap, DEBUG_MEMORY_HEAP, "debugMemoryHeap");
}
const char* cmd_debugmemorystack(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMemoryStack, DEBUG_MEMORY_STACK, "debugMemoryStack");
}
const char* cmd_debugmemorybuffers(const String& a) {
  return cmd_debugsubflag_impl(a, &gSettings.debugMemoryBuffers, DEBUG_MEMORY_BUFFERS, "debugMemoryBuffers");
}

const char* cmd_debugespnowmesh(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_MESH); else clearDebugFlag(DEBUG_ESPNOW_MESH);
    return v ? "debugEspNowMesh enabled (runtime only)" : "debugEspNowMesh disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowMesh, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_MESH); else clearDebugFlag(DEBUG_ESPNOW_MESH);
    return gSettings.debugEspNowMesh ? "debugEspNowMesh enabled (persistent)" : "debugEspNowMesh disabled (persistent)";
  }
}

const char* cmd_debugespnowtopo(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_TOPO); else clearDebugFlag(DEBUG_ESPNOW_TOPO);
    return v ? "debugEspNowTopo enabled (runtime only)" : "debugEspNowTopo disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowTopo, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_TOPO); else clearDebugFlag(DEBUG_ESPNOW_TOPO);
    return gSettings.debugEspNowTopo ? "debugEspNowTopo enabled (persistent)" : "debugEspNowTopo disabled (persistent)";
  }
}

const char* cmd_debugespnowencryption(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_ENCRYPTION); else clearDebugFlag(DEBUG_ESPNOW_ENCRYPTION);
    return v ? "debugEspNowEncryption enabled (runtime only)" : "debugEspNowEncryption disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowEncryption, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_ENCRYPTION); else clearDebugFlag(DEBUG_ESPNOW_ENCRYPTION);
    return gSettings.debugEspNowEncryption ? "debugEspNowEncryption enabled (persistent)" : "debugEspNowEncryption disabled (persistent)";
  }
}

const char* cmd_debugespnowmetadata(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_ESPNOW_METADATA); else clearDebugFlag(DEBUG_ESPNOW_METADATA);
    return v ? "debugEspNowMetadata enabled (runtime only)" : "debugEspNowMetadata disabled (runtime only)";
  } else {
    setSetting(gSettings.debugEspNowMetadata, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_ESPNOW_METADATA); else clearDebugFlag(DEBUG_ESPNOW_METADATA);
    return gSettings.debugEspNowMetadata ? "debugEspNowMetadata enabled (persistent)" : "debugEspNowMetadata disabled (persistent)";
  }
}

const char* cmd_debugautoscheduler(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTO_SCHEDULER); else clearDebugFlag(DEBUG_AUTO_SCHEDULER);
    return v ? "debugAutoScheduler enabled (runtime only)" : "debugAutoScheduler disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAutoScheduler, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_AUTO_SCHEDULER); else clearDebugFlag(DEBUG_AUTO_SCHEDULER);
    return gSettings.debugAutoScheduler ? "debugAutoScheduler enabled (persistent)" : "debugAutoScheduler disabled (persistent)";
  }
}

const char* cmd_debugautoexec(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTO_EXEC); else clearDebugFlag(DEBUG_AUTO_EXEC);
    return v ? "debugAutoExec enabled (runtime only)" : "debugAutoExec disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAutoExec, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_AUTO_EXEC); else clearDebugFlag(DEBUG_AUTO_EXEC);
    return gSettings.debugAutoExec ? "debugAutoExec enabled (persistent)" : "debugAutoExec disabled (persistent)";
  }
}

const char* cmd_debugautocondition(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (modeTemp) {
    if (v) setDebugFlag(DEBUG_AUTO_CONDITION); else clearDebugFlag(DEBUG_AUTO_CONDITION);
    return v ? "debugAutoCondition enabled (runtime only)" : "debugAutoCondition disabled (runtime only)";
  } else {
    setSetting(gSettings.debugAutoCondition, (bool)(v == 1));
    if (v) setDebugFlag(DEBUG_AUTO_CONDITION); else clearDebugFlag(DEBUG_AUTO_CONDITION);
    return gSettings.debugAutoCondition ? "debugAutoCondition enabled (persistent)" : "debugAutoCondition disabled (persistent)";
  }
}

const char* cmd_debugautotiming(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
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
static inline void syncStorageParent() { updateParentDebugFlag(DEBUG_STORAGE,     gSettings.debugStorage     || gDebugSubFlags.storageFiles   || gDebugSubFlags.storageJson    || gDebugSubFlags.storageSettings || gDebugSubFlags.storageMigration || gDebugSubFlags.storagePermissions); }
static inline void syncSystemParent()  { updateParentDebugFlag(DEBUG_SYSTEM,      gSettings.debugSystem      || gDebugSubFlags.systemBoot     || gDebugSubFlags.systemConfig   || gDebugSubFlags.systemTasks    || gDebugSubFlags.systemHardware); }
static inline void syncUsersParent()   { updateParentDebugFlag(DEBUG_USERS,       gSettings.debugUsers       || gDebugSubFlags.usersMgmt      || gDebugSubFlags.usersRegister  || gDebugSubFlags.usersQuery); }
static inline void syncCliParent()     { updateParentDebugFlag(DEBUG_CLI,         gSettings.debugCli         || gDebugSubFlags.cliExecution   || gDebugSubFlags.cliQueue       || gDebugSubFlags.cliValidation); }
static inline void syncPerfParent()    { updateParentDebugFlag(DEBUG_PERFORMANCE, gSettings.debugPerformance || gDebugSubFlags.perfStack       || gDebugSubFlags.perfHeap       || gDebugSubFlags.perfTiming); }
static inline void syncSseParent()     { updateParentDebugFlag(DEBUG_SSE,         gSettings.debugSse         || gDebugSubFlags.sseConnection  || gDebugSubFlags.sseEvents      || gDebugSubFlags.sseBroadcast); }
static inline void syncCmdFlowParent() { updateParentDebugFlag(DEBUG_CMD_FLOW,    gSettings.debugCommandFlow || gDebugSubFlags.cmdflowRouting  || gDebugSubFlags.cmdflowQueue   || gDebugSubFlags.cmdflowContext); }
static inline void syncNtpParent()     { updateParentDebugFlag(DEBUG_NTP,         gSettings.debugDateTime    || gDebugSubFlags.ntpSync        || gDebugSubFlags.ntpSetup       || gDebugSubFlags.ntpAnchor      || gDebugSubFlags.ntpResolve); }

const char* cmd_debugdatetimesync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.ntpSync = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugDatetimeSync, (bool)(v != 0));
  syncNtpParent();
  if (modeTemp) return v ? "debugDatetimeSync enabled (runtime only)" : "debugDatetimeSync disabled (runtime only)";
  return gSettings.debugDatetimeSync ? "debugDatetimeSync enabled (persistent)" : "debugDatetimeSync disabled (persistent)";
}

const char* cmd_debugdatetimesetup(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.ntpSetup = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugDatetimeSetup, (bool)(v != 0));
  syncNtpParent();
  if (modeTemp) return v ? "debugDatetimeSetup enabled (runtime only)" : "debugDatetimeSetup disabled (runtime only)";
  return gSettings.debugDatetimeSetup ? "debugDatetimeSetup enabled (persistent)" : "debugDatetimeSetup disabled (persistent)";
}

const char* cmd_debugdatetimeanchor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.ntpAnchor = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugDatetimeAnchor, (bool)(v != 0));
  syncNtpParent();
  if (modeTemp) return v ? "debugDatetimeAnchor enabled (runtime only)" : "debugDatetimeAnchor disabled (runtime only)";
  return gSettings.debugDatetimeAnchor ? "debugDatetimeAnchor enabled (persistent)" : "debugDatetimeAnchor disabled (persistent)";
}

const char* cmd_debugdatetimeresolve(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.ntpResolve = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugDatetimeResolve, (bool)(v != 0));
  syncNtpParent();
  if (modeTemp) return v ? "debugDatetimeResolve enabled (runtime only)" : "debugDatetimeResolve disabled (runtime only)";
  return gSettings.debugDatetimeResolve ? "debugDatetimeResolve enabled (persistent)" : "debugDatetimeResolve disabled (persistent)";
}

const char* cmd_debugauthsessions(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.authSessions = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugAuthSessions, (bool)(v != 0));
  syncAuthParent();
  if (modeTemp) return v ? "debugAuthSessions enabled (runtime only)" : "debugAuthSessions disabled (runtime only)";
  return gSettings.debugAuthSessions ? "debugAuthSessions enabled (persistent)" : "debugAuthSessions disabled (persistent)";
}

const char* cmd_debugauthcookies(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.authCookies = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugAuthCookies, (bool)(v != 0));
  syncAuthParent();
  if (modeTemp) return v ? "debugAuthCookies enabled (runtime only)" : "debugAuthCookies disabled (runtime only)";
  return gSettings.debugAuthCookies ? "debugAuthCookies enabled (persistent)" : "debugAuthCookies disabled (persistent)";
}

const char* cmd_debugauthlogin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.authLogin = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugAuthLogin, (bool)(v != 0));
  syncAuthParent();
  if (modeTemp) return v ? "debugAuthLogin enabled (runtime only)" : "debugAuthLogin disabled (runtime only)";
  return gSettings.debugAuthLogin ? "debugAuthLogin enabled (persistent)" : "debugAuthLogin disabled (persistent)";
}

const char* cmd_debugauthbootid(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.authBootId = (v != 0);
  if (!modeTemp) setSetting(gSettings.debugAuthBootId, (bool)(v != 0));
  syncAuthParent();
  if (modeTemp) return v ? "debugAuthBootId enabled (runtime only)" : "debugAuthBootId disabled (runtime only)";
  return gSettings.debugAuthBootId ? "debugAuthBootId enabled (persistent)" : "debugAuthBootId disabled (persistent)";
}

const char* cmd_commandmodulesummary(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t moduleCount = 0;
  getCommandModules(moduleCount);
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  printCommandModuleSummary();
  snprintf(gDebugBuffer, 1024, "Command modules: %zu modules, %zu commands", moduleCount, gCommandsCount);
  return gDebugBuffer;
}

const char* cmd_settingsmodulesummary(const String& argsInput) {
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

const char* getDebugCategoryName(DebugFlagMask flag) {
  // Return the first matching flag name (checked in bit order, low to high)
  if (flag & DEBUG_AUTH) return "AUTH";
  if (flag & DEBUG_HTTP) return "HTTP";
  if (flag & DEBUG_HTTPS) return "HTTPS";
  if (flag & DEBUG_SSE) return "SSE";
  if (flag & DEBUG_CLI) return "CLI";
  if (flag & DEBUG_FMRADIO) return "FMRADIO";
  if (flag & DEBUG_I2C) return "I2C";
  if (flag & DEBUG_I2C_BUS)       return "I2C_BUS";
  if (flag & DEBUG_I2C_DISCOVERY) return "I2C_DISCOVERY";
  if (flag & DEBUG_I2C_AUTOSTART) return "I2C_AUTOSTART";
  if (flag & DEBUG_MQTT)            return "MQTT";
  if (flag & DEBUG_MQTT_CONNECTION) return "MQTT_CONN";
  if (flag & DEBUG_MQTT_PUBSUB)     return "MQTT_PUBSUB";
  if (flag & DEBUG_MQTT_DISCOVERY)  return "MQTT_DISCOVERY";
  if (flag & DEBUG_MQTT_COMMANDS)   return "MQTT_CMD";
  if (flag & DEBUG_WIFI) return "WIFI";
  if (flag & DEBUG_PERFORMANCE) return "PERF";
  if (flag & DEBUG_MICROPHONE) return "MIC";
  if (flag & DEBUG_CMD_FLOW) return "CMD_FLOW";
  if (flag & DEBUG_USERS) return "USERS";
  if (flag & DEBUG_SYSTEM) return "SYSTEM";
  if (flag & DEBUG_STORAGE) return "STORAGE";
  if (flag & DEBUG_ESPNOW_CORE) return "ESP-NOW";
  if (flag & DEBUG_LOGGER) return "LOGGER";
  if (flag & DEBUG_MEMORY) return "MEMORY";
  if (flag & DEBUG_MEMORY_HEAP)    return "MEMORY_HEAP";
  if (flag & DEBUG_MEMORY_STACK)   return "MEMORY_STACK";
  if (flag & DEBUG_MEMORY_BUFFERS) return "MEMORY_BUFFERS";
  if (flag & DEBUG_ESPNOW_ROUTER) return "ESPNOW_ROUTER";
  if (flag & DEBUG_ESPNOW_MESH) return "ESPNOW_MESH";
  if (flag & DEBUG_ESPNOW_TOPO) return "ESPNOW_TOPO";
  if (flag & DEBUG_ESPNOW_STREAM) return "ESPNOW_STREAM";
  if (flag & DEBUG_COMMAND_SYSTEM) return "CMD_SYS";
  if (flag & DEBUG_AUTO_EXEC) return "AUTO_EXEC";
  if (flag & DEBUG_AUTO_CONDITION) return "AUTO_COND";
  if (flag & DEBUG_AUTO_TIMING) return "AUTO_TIME";
  if (flag & DEBUG_AUTOMATIONS) return "AUTO";
  if (flag & DEBUG_CAMERA) return "CAMERA";
  if (flag & DEBUG_CAMERA_LIFECYCLE) return "CAMERA_LIFECYCLE";
  if (flag & DEBUG_CAMERA_CAPTURE)   return "CAMERA_CAPTURE";
  if (flag & DEBUG_CAMERA_SETTINGS)  return "CAMERA_SETTINGS";
  if (flag & DEBUG_CAMERA_VIDEO)     return "CAMERA_VIDEO";
  if (flag & DEBUG_DISPLAY)          return "DISPLAY";
  if (flag & DEBUG_AUTO_SCHEDULER) return "AUTO_SCHED";
  if (flag & DEBUG_ESPNOW_ENCRYPTION) return "ESPNOW_ENC";
  // Bits 32-39: per-sensor device flags
  if (flag & DEBUG_GPS) return "GPS";
  if (flag & DEBUG_RTC) return "RTC";
  if (flag & DEBUG_IMU) return "IMU";
  if (flag & DEBUG_THERMAL) return "THERMAL";
  if (flag & DEBUG_TOF) return "TOF";
  if (flag & DEBUG_INPUT) return "INPUT";
  if (flag & DEBUG_ANO_ENCODER) return "ANO";
  if (flag & DEBUG_APDS) return "APDS";
  if (flag & DEBUG_PRESENCE) return "PRESENCE";
  // Bits 40-47: per-sensor frame/data flags
  // Per-sensor sub-flags (Lifecycle / Polling / Values)
  if (flag & DEBUG_THERMAL_LIFECYCLE)  return "THERMAL_LIFE";
  if (flag & DEBUG_THERMAL_POLLING)    return "THERMAL_POLL";
  if (flag & DEBUG_THERMAL_VALUES)     return "THERMAL_VAL";
  if (flag & DEBUG_TOF_LIFECYCLE)      return "TOF_LIFE";
  if (flag & DEBUG_TOF_POLLING)        return "TOF_POLL";
  if (flag & DEBUG_TOF_VALUES)         return "TOF_VAL";
  if (flag & DEBUG_INPUT_LIFECYCLE)  return "INPUT_LIFE";
  if (flag & DEBUG_INPUT_POLLING)    return "INPUT_POLL";
  if (flag & DEBUG_INPUT_VALUES)     return "INPUT_VAL";
  if (flag & DEBUG_ANO_ENCODER_LIFECYCLE) return "ANO_LIFE";
  if (flag & DEBUG_ANO_ENCODER_POLLING)   return "ANO_POLL";
  if (flag & DEBUG_ANO_ENCODER_VALUES)    return "ANO_VAL";
  if (flag & DEBUG_IMU_LIFECYCLE)      return "IMU_LIFE";
  if (flag & DEBUG_IMU_POLLING)        return "IMU_POLL";
  if (flag & DEBUG_IMU_VALUES)         return "IMU_VAL";
  if (flag & DEBUG_APDS_LIFECYCLE)     return "APDS_LIFE";
  if (flag & DEBUG_APDS_POLLING)       return "APDS_POLL";
  if (flag & DEBUG_APDS_VALUES)        return "APDS_VAL";
  if (flag & DEBUG_GPS_LIFECYCLE)      return "GPS_LIFE";
  if (flag & DEBUG_GPS_POLLING)        return "GPS_POLL";
  if (flag & DEBUG_GPS_VALUES)         return "GPS_VAL";
  if (flag & DEBUG_RTC_LIFECYCLE)      return "RTC_LIFE";
  if (flag & DEBUG_RTC_POLLING)        return "RTC_POLL";
  if (flag & DEBUG_RTC_VALUES)         return "RTC_VAL";
  if (flag & DEBUG_FMRADIO_LIFECYCLE)  return "FMRADIO_LIFE";
  if (flag & DEBUG_FMRADIO_POLLING)    return "FMRADIO_POLL";
  if (flag & DEBUG_FMRADIO_VALUES)     return "FMRADIO_VAL";
  if (flag & DEBUG_MIC_LIFECYCLE)      return "MIC_LIFE";
  if (flag & DEBUG_MIC_POLLING)        return "MIC_POLL";
  if (flag & DEBUG_MIC_VALUES)         return "MIC_VAL";
  if (flag & DEBUG_PRESENCE_LIFECYCLE) return "PRESENCE_LIFE";
  if (flag & DEBUG_PRESENCE_POLLING)   return "PRESENCE_POLL";
  if (flag & DEBUG_PRESENCE_VALUES)    return "PRESENCE_VAL";
  // Bit 48
  if (flag & DEBUG_ESPNOW_METADATA) return "ESPNOW_META";
  // Bits 49-52: Maps
  if (flag & DEBUG_MAPS) return "MAPS";
  if (flag & DEBUG_MAPS_LOADING) return "MAPS_LOAD";
  if (flag & DEBUG_MAPS_RENDERING) return "MAPS_RENDER";
  if (flag & DEBUG_MAPS_PERF) return "MAPS_PERF";
#if ENABLE_ONDEVICE_LLM
  // Bits 57-62: On-device LLM
  if (flag & DEBUG_LLM) return "LLM";
  if (flag & DEBUG_LLM_LOAD) return "LLM_LOAD";
  if (flag & DEBUG_LLM_TOKENIZER) return "LLM_TOK";
  if (flag & DEBUG_LLM_FORWARD) return "LLM_FWD";
  if (flag & DEBUG_LLM_GENERATE) return "LLM_GEN";
  if (flag & DEBUG_LLM_MEMORY) return "LLM_MEM";
#endif
  // Bit 63: NTP / DateTime
  if (flag & DEBUG_NTP) return "NTP";
  // Bits 64-69: G2 sub-flags. Return the sub name when the sub-bit is
  // set; the parent DEBUG_G2 alone (no sub) still maps to "G2".
  if (flag & DEBUG_G2_LIFECYCLE) return "G2_LIFE";
  if (flag & DEBUG_G2_PROTOCOL)  return "G2_PROTO";
  if (flag & DEBUG_G2_EVENTS)    return "G2_EVT";
  if (flag & DEBUG_G2_PAGES)     return "G2_PAGE";
  if (flag & DEBUG_G2_HEARTBEAT) return "G2_HB";
  if (flag & DEBUG_G2_DUMP)      return "G2_DUMP";
  if (flag & DEBUG_G2)           return "G2";
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
    char uptimeBuf[32];
    snprintf(uptimeBuf, sizeof(uptimeBuf), "uptime-%lu", millis());
    filename += uptimeBuf;
  }
  
  filename += ".log";
  return filename;
}

const char* cmd_log(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CommandArgs ca(argsInput);
  if (ca.count() == 0) {
    return "Usage: log <start|stop|status|autostart>\n"
           "  start [\"filepath\"] [flags=0xXXXX] [tags=0|1]: Begin system logging\n"
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
  String subCmd = ca.arg(0);
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
    
    // Parse arguments: log start [filepath] [flags=0xXXXX[:0xYYYY]] [tags=0|1]
    // Flag arg accepts up to 128 bits as either:
    //   - single hex (interpreted as low 64 bits, high stays 0)
    //   - "0xHIGH:0xLOW" pair (each up to 16 hex chars / 64 bits)
    String filepath;
    bool flagsSet = false;
    DebugFlagMask debugFlags = (DebugFlagMask)0;
    int categoryTags = -1; // Sentinel: don't change if not specified

    // Find filepath (first non-key=value arg after "start")
    bool hasFilepath = false;
    for (int i = 1; i < ca.count(); i++) {
      String token = ca.arg(i);
      if (token.startsWith("flags=")) {
        String flagsStr = token.substring(6);
        int colon = flagsStr.indexOf(':');
        auto parseHex64 = [](const String& s) -> uint64_t {
          const char* p = s.c_str();
          if (s.startsWith("0x") || s.startsWith("0X")) p += 2;
          return strtoull(p, nullptr, 16);
        };
        if (colon >= 0) {
          uint64_t hi = parseHex64(flagsStr.substring(0, colon));
          uint64_t lo = parseHex64(flagsStr.substring(colon + 1));
          debugFlags = ((DebugFlagMask)hi << 64) | (DebugFlagMask)lo;
        } else {
          debugFlags = (DebugFlagMask)parseHex64(flagsStr);
        }
        flagsSet = true;
      } else if (token.startsWith("tags=")) {
        String tagsStr = token.substring(5);
        categoryTags = tagsStr.toInt();
      } else if (token.length() > 0 && !hasFilepath) {
        // The log path must be a quoted token (uniform quoted-path rule); a bare
        // token here is almost certainly an unquoted path.
        if (!ca.argWasQuoted(i))
          return "Error: log path must be in quotes, e.g. log start \"/logs/sys.txt\"";
        filepath = token;
        hasFilepath = true;
      }
    }

    if (!hasFilepath) {
      // Use persisted path if set, otherwise auto-generate
      filepath = (gSettings.systemLogPath.length() > 0) ? gSettings.systemLogPath : generateSystemLogFilename();
    }
    
    if (filepath.length() == 0 || filepath.charAt(0) != '/') {
      return "Error: Filepath must start with / (e.g., /logs/system.log)";
    }
    
    // Apply debug flags if specified
    if (flagsSet) {
      gDebugFlags = debugFlags;
      const uint64_t hi = (uint64_t)(gDebugFlags >> 64);
      const uint64_t lo = (uint64_t)gDebugFlags;
      char flagsMsg[128];
      snprintf(flagsMsg, sizeof(flagsMsg),
               "Debug flags set to: 0x%016llX:%016llX",
               (unsigned long long)hi, (unsigned long long)lo);
      broadcastOutput(flagsMsg);
    }
    
    // Apply category tags setting (arg overrides, otherwise use persisted setting)
    if (categoryTags >= 0) {
      gSystemLogCategoryTags = (categoryTags != 0);
    } else {
      gSystemLogCategoryTags = gSettings.systemLogCategoryTags;
    }
    
    // Ensure directory exists
    int lastSlash = filepath.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = filepath.substring(0, lastSlash);
      if (!VFS::existsGuarded(dir, VFS::systemAuth("debug.log_setup_mkdir"))) {
        fsLock("log.mkdir");
        bool created = VFS::mkdirGuarded(dir, VFS::systemAuth("debug.log_setup_mkdir"));
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
    if (!VFS::existsGuarded(filepath, VFS::systemAuth("debug.log_setup_create"))) {
      File f = VFS::openGuarded(filepath, "w", VFS::systemAuth("debug.log_setup_create"), true);
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
    setSetting(gSettings.systemLogPath, filepath);

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
const char* cmd_debughttphandlers(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.httpHandlers = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugHttpHandlers, (bool)(v!=0));
  syncHttpParent();
  if (modeTemp) return v ? "debugHttpHandlers enabled (runtime only)" : "debugHttpHandlers disabled (runtime only)";
  return gSettings.debugHttpHandlers ? "debugHttpHandlers enabled (persistent)" : "debugHttpHandlers disabled (persistent)";
}

const char* cmd_debughttprequests(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.httpRequests = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugHttpRequests, (bool)(v!=0));
  syncHttpParent();
  if (modeTemp) return v ? "debugHttpRequests enabled (runtime only)" : "debugHttpRequests disabled (runtime only)";
  return gSettings.debugHttpRequests ? "debugHttpRequests enabled (persistent)" : "debugHttpRequests disabled (persistent)";
}

const char* cmd_debughttpresponses(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.httpResponses = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugHttpResponses, (bool)(v!=0));
  syncHttpParent();
  if (modeTemp) return v ? "debugHttpResponses enabled (runtime only)" : "debugHttpResponses disabled (runtime only)";
  return gSettings.debugHttpResponses ? "debugHttpResponses enabled (persistent)" : "debugHttpResponses disabled (persistent)";
}

const char* cmd_debughttpstreaming(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.httpStreaming = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugHttpStreaming, (bool)(v!=0));
  syncHttpParent();
  if (modeTemp) return v ? "debugHttpStreaming enabled (runtime only)" : "debugHttpStreaming disabled (runtime only)";
  return gSettings.debugHttpStreaming ? "debugHttpStreaming enabled (persistent)" : "debugHttpStreaming disabled (persistent)";
}

// WiFi sub-flag commands
const char* cmd_debugwificonnection(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.wifiConnection = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugWifiConnection, (bool)(v!=0));
  syncWifiParent();
  if (modeTemp) return v ? "debugWifiConnection enabled (runtime only)" : "debugWifiConnection disabled (runtime only)";
  return gSettings.debugWifiConnection ? "debugWifiConnection enabled (persistent)" : "debugWifiConnection disabled (persistent)";
}

const char* cmd_debugwificonfig(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.wifiConfig = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugWifiConfig, (bool)(v!=0));
  syncWifiParent();
  if (modeTemp) return v ? "debugWifiConfig enabled (runtime only)" : "debugWifiConfig disabled (runtime only)";
  return gSettings.debugWifiConfig ? "debugWifiConfig enabled (persistent)" : "debugWifiConfig disabled (persistent)";
}

const char* cmd_debugwifiscanning(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.wifiScanning = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugWifiScanning, (bool)(v!=0));
  syncWifiParent();
  if (modeTemp) return v ? "debugWifiScanning enabled (runtime only)" : "debugWifiScanning disabled (runtime only)";
  return gSettings.debugWifiScanning ? "debugWifiScanning enabled (persistent)" : "debugWifiScanning disabled (persistent)";
}

const char* cmd_debugwifidriver(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.wifiDriver = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugWifiDriver, (bool)(v!=0));
  syncWifiParent();
  if (modeTemp) return v ? "debugWifiDriver enabled (runtime only)" : "debugWifiDriver disabled (runtime only)";
  return gSettings.debugWifiDriver ? "debugWifiDriver enabled (persistent)" : "debugWifiDriver disabled (persistent)";
}

// Storage sub-flag commands
const char* cmd_debugstoragefiles(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.storageFiles = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStorageFiles, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStorageFiles enabled (runtime only)" : "debugStorageFiles disabled (runtime only)";
  return gSettings.debugStorageFiles ? "debugStorageFiles enabled (persistent)" : "debugStorageFiles disabled (persistent)";
}

const char* cmd_debugstoragejson(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.storageJson = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStorageJson, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStorageJson enabled (runtime only)" : "debugStorageJson disabled (runtime only)";
  return gSettings.debugStorageJson ? "debugStorageJson enabled (persistent)" : "debugStorageJson disabled (persistent)";
}

const char* cmd_debugstoragesettings(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.storageSettings = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStorageSettings, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStorageSettings enabled (runtime only)" : "debugStorageSettings disabled (runtime only)";
  return gSettings.debugStorageSettings ? "debugStorageSettings enabled (persistent)" : "debugStorageSettings disabled (persistent)";
}

const char* cmd_debugstoragemigration(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.storageMigration = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStorageMigration, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStorageMigration enabled (runtime only)" : "debugStorageMigration disabled (runtime only)";
  return gSettings.debugStorageMigration ? "debugStorageMigration enabled (persistent)" : "debugStorageMigration disabled (persistent)";
}

// [PERM] DENY audit toggle. Unlike the other Storage subflags, this one
// gates an actual log line per call (logFsAccessDeny in System_Filesystem.cpp
// checks gDebugSubFlags.storagePermissions before emitting). Defaults to
// true. Mute with `debugstoragepermissions 0` if you want other Storage
// debug on but don't want the denial audit trail.
const char* cmd_debugstoragepermissions(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.storagePermissions = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugStoragePermissions, (bool)(v!=0));
  syncStorageParent();
  if (modeTemp) return v ? "debugStoragePermissions enabled (runtime only)" : "debugStoragePermissions disabled (runtime only)";
  return gSettings.debugStoragePermissions ? "debugStoragePermissions enabled (persistent)" : "debugStoragePermissions disabled (persistent)";
}

// Low-level stack/heap trace toggle (bypasses debug queue, writes directly
// to Serial). Use only while diagnosing stack-overflow / queue-saturation
// issues — Serial.printf is synchronous and slows heavy paths if left on.
const char* cmd_debugstack(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  int v = ca.argInt(0, -1);
  String s = ca.arg(0);
  if (s.equalsIgnoreCase("on") || s.equalsIgnoreCase("true") || v == 1) {
    gDebugStackTraceEnabled = true;
    return "debugstack ON — STACK_TRACEF messages will print directly to Serial";
  }
  if (s.equalsIgnoreCase("off") || s.equalsIgnoreCase("false") || v == 0) {
    gDebugStackTraceEnabled = false;
    return "debugstack OFF";
  }
  return gDebugStackTraceEnabled ? "debugstack is currently ON" : "debugstack is currently OFF";
}

// System sub-flag commands
const char* cmd_debugsystemboot(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.systemBoot = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSystemBoot, (bool)(v!=0));
  syncSystemParent();
  if (modeTemp) return v ? "debugSystemBoot enabled (runtime only)" : "debugSystemBoot disabled (runtime only)";
  return gSettings.debugSystemBoot ? "debugSystemBoot enabled (persistent)" : "debugSystemBoot disabled (persistent)";
}

const char* cmd_debugsystemconfig(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.systemConfig = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSystemConfig, (bool)(v!=0));
  syncSystemParent();
  if (modeTemp) return v ? "debugSystemConfig enabled (runtime only)" : "debugSystemConfig disabled (runtime only)";
  return gSettings.debugSystemConfig ? "debugSystemConfig enabled (persistent)" : "debugSystemConfig disabled (persistent)";
}

const char* cmd_debugsystemtasks(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.systemTasks = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSystemTasks, (bool)(v!=0));
  syncSystemParent();
  if (modeTemp) return v ? "debugSystemTasks enabled (runtime only)" : "debugSystemTasks disabled (runtime only)";
  return gSettings.debugSystemTasks ? "debugSystemTasks enabled (persistent)" : "debugSystemTasks disabled (persistent)";
}

const char* cmd_debugsystemhardware(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.systemHardware = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSystemHardware, (bool)(v!=0));
  syncSystemParent();
  if (modeTemp) return v ? "debugSystemHardware enabled (runtime only)" : "debugSystemHardware disabled (runtime only)";
  return gSettings.debugSystemHardware ? "debugSystemHardware enabled (persistent)" : "debugSystemHardware disabled (persistent)";
}

// Users sub-flag commands
const char* cmd_debugusersmgmt(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.usersMgmt = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugUsersMgmt, (bool)(v!=0));
  syncUsersParent();
  if (modeTemp) return v ? "debugUsersMgmt enabled (runtime only)" : "debugUsersMgmt disabled (runtime only)";
  return gSettings.debugUsersMgmt ? "debugUsersMgmt enabled (persistent)" : "debugUsersMgmt disabled (persistent)";
}

const char* cmd_debugusersregister(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.usersRegister = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugUsersRegister, (bool)(v!=0));
  syncUsersParent();
  if (modeTemp) return v ? "debugUsersRegister enabled (runtime only)" : "debugUsersRegister disabled (runtime only)";
  return gSettings.debugUsersRegister ? "debugUsersRegister enabled (persistent)" : "debugUsersRegister disabled (persistent)";
}

const char* cmd_debugusersquery(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.usersQuery = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugUsersQuery, (bool)(v!=0));
  syncUsersParent();
  if (modeTemp) return v ? "debugUsersQuery enabled (runtime only)" : "debugUsersQuery disabled (runtime only)";
  return gSettings.debugUsersQuery ? "debugUsersQuery enabled (persistent)" : "debugUsersQuery disabled (persistent)";
}

// CLI sub-flag commands
const char* cmd_debugcliexecution(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.cliExecution = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCliExecution, (bool)(v!=0));
  syncCliParent();
  if (modeTemp) return v ? "debugCliExecution enabled (runtime only)" : "debugCliExecution disabled (runtime only)";
  return gSettings.debugCliExecution ? "debugCliExecution enabled (persistent)" : "debugCliExecution disabled (persistent)";
}

const char* cmd_debugcliqueue(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.cliQueue = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCliQueue, (bool)(v!=0));
  syncCliParent();
  if (modeTemp) return v ? "debugCliQueue enabled (runtime only)" : "debugCliQueue disabled (runtime only)";
  return gSettings.debugCliQueue ? "debugCliQueue enabled (persistent)" : "debugCliQueue disabled (persistent)";
}

const char* cmd_debugclivalidation(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.cliValidation = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCliValidation, (bool)(v!=0));
  syncCliParent();
  if (modeTemp) return v ? "debugCliValidation enabled (runtime only)" : "debugCliValidation disabled (runtime only)";
  return gSettings.debugCliValidation ? "debugCliValidation enabled (persistent)" : "debugCliValidation disabled (persistent)";
}

// Performance sub-flag commands
const char* cmd_debugperfstack(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.perfStack = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugPerfStack, (bool)(v!=0));
  syncPerfParent();
  if (modeTemp) return v ? "debugPerfStack enabled (runtime only)" : "debugPerfStack disabled (runtime only)";
  return gSettings.debugPerfStack ? "debugPerfStack enabled (persistent)" : "debugPerfStack disabled (persistent)";
}

const char* cmd_debugperfheap(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.perfHeap = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugPerfHeap, (bool)(v!=0));
  syncPerfParent();
  if (modeTemp) return v ? "debugPerfHeap enabled (runtime only)" : "debugPerfHeap disabled (runtime only)";
  return gSettings.debugPerfHeap ? "debugPerfHeap enabled (persistent)" : "debugPerfHeap disabled (persistent)";
}

const char* cmd_debugperftiming(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.perfTiming = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugPerfTiming, (bool)(v!=0));
  syncPerfParent();
  if (modeTemp) return v ? "debugPerfTiming enabled (runtime only)" : "debugPerfTiming disabled (runtime only)";
  return gSettings.debugPerfTiming ? "debugPerfTiming enabled (persistent)" : "debugPerfTiming disabled (persistent)";
}

// SSE sub-flag commands
const char* cmd_debugsseconnection(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.sseConnection = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSseConnection, (bool)(v!=0));
  syncSseParent();
  if (modeTemp) return v ? "debugSseConnection enabled (runtime only)" : "debugSseConnection disabled (runtime only)";
  return gSettings.debugSseConnection ? "debugSseConnection enabled (persistent)" : "debugSseConnection disabled (persistent)";
}

const char* cmd_debugsseevents(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.sseEvents = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSseEvents, (bool)(v!=0));
  syncSseParent();
  if (modeTemp) return v ? "debugSseEvents enabled (runtime only)" : "debugSseEvents disabled (runtime only)";
  return gSettings.debugSseEvents ? "debugSseEvents enabled (persistent)" : "debugSseEvents disabled (persistent)";
}

const char* cmd_debugssebroadcast(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.sseBroadcast = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugSseBroadcast, (bool)(v!=0));
  syncSseParent();
  if (modeTemp) return v ? "debugSseBroadcast enabled (runtime only)" : "debugSseBroadcast disabled (runtime only)";
  return gSettings.debugSseBroadcast ? "debugSseBroadcast enabled (persistent)" : "debugSseBroadcast disabled (persistent)";
}

// Command Flow sub-flag commands
const char* cmd_debugcmdflowrouting(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.cmdflowRouting = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCmdflowRouting, (bool)(v!=0));
  syncCmdFlowParent();
  if (modeTemp) return v ? "debugCmdflowRouting enabled (runtime only)" : "debugCmdflowRouting disabled (runtime only)";
  return gSettings.debugCmdflowRouting ? "debugCmdflowRouting enabled (persistent)" : "debugCmdflowRouting disabled (persistent)";
}

const char* cmd_debugcmdflowqueue(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.cmdflowQueue = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCmdflowQueue, (bool)(v!=0));
  syncCmdFlowParent();
  if (modeTemp) return v ? "debugCmdflowQueue enabled (runtime only)" : "debugCmdflowQueue disabled (runtime only)";
  return gSettings.debugCmdflowQueue ? "debugCmdflowQueue enabled (persistent)" : "debugCmdflowQueue disabled (persistent)";
}

const char* cmd_debugcmdflowcontext(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp")||mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  gDebugSubFlags.cmdflowContext = (v!=0);
  if (!modeTemp) setSetting(gSettings.debugCmdflowContext, (bool)(v!=0));
  syncCmdFlowParent();
  if (modeTemp) return v ? "debugCmdflowContext enabled (runtime only)" : "debugCmdflowContext disabled (runtime only)";
  return gSettings.debugCmdflowContext ? "debugCmdflowContext enabled (persistent)" : "debugCmdflowContext disabled (persistent)";
}

// ============================================================================
// Debug Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry debugCommands[] = {
  { "debughttp", "Debug HTTP requests.", true, cmd_debughttp },
  { "debughttps", "Debug HTTPS/TLS handshake + connection errors (ESP-IDF logs).", true, cmd_debughttps },
  { "debugsse", "Debug Server-Sent Events.", true, cmd_debugsse },
  { "debugcli", "Debug CLI processing.", true, cmd_debugcli },
  { "debugauth", "Debug authentication (parent flag).", true, cmd_debugauth, "Usage: debugauth <0|1>" },
  { "debugespnow", "Debug ESP-NOW (parent flag).", true, cmd_debugespnow, "Usage: debugespnow <0|1>" },
  { "debugbluetooth", "Debug Bluetooth (parent flag).", true, cmd_debugbluetooth, "Usage: debugbluetooth <0|1> [temp|runtime]" },
  { "debugbluetoothcore", "Debug Bluetooth core lifecycle.", true, cmd_debugbluetoothcore, "Usage: debugbluetoothcore <0|1> [temp|runtime]" },
  { "debugbluetoothgatt", "Debug Bluetooth GATT operations.", true, cmd_debugbluetoothgatt, "Usage: debugbluetoothgatt <0|1> [temp|runtime]" },
  { "debugbluetoothdata", "Debug Bluetooth command/data path.", true, cmd_debugbluetoothdata, "Usage: debugbluetoothdata <0|1> [temp|runtime]" },
  { "debugcamera",          "Debug camera (parent flag).",                              true, cmd_debugcamera,          "Usage: debugcamera <0|1> [temp|runtime]" },
  { "debugcameralifecycle", "Debug camera init/stop/PWDN-RESET/GPIO state.",            true, cmd_debugcameralifecycle, "Usage: debugcameralifecycle <0|1>" },
  { "debugcameracapture",   "Debug captureFrame, JPEG validation, fb buffer, recovery.",true, cmd_debugcameracapture,   "Usage: debugcameracapture <0|1>" },
  { "debugcamerasettings",  "Debug runtime camera resolution/quality changes.",         true, cmd_debugcamerasettings,  "Usage: debugcamerasettings <0|1>" },
  { "debugcameravideo",     "Debug video recording start/finalize, frame writing.",     true, cmd_debugcameravideo,     "Usage: debugcameravideo <0|1>" },
  { "debugdisplay",         "Debug OLED init/probe/boot-animation/mode-transitions.",   true, cmd_debugdisplay,         "Usage: debugdisplay <0|1>" },
  { "debugmicrophone", "Debug microphone operations.", true, cmd_debugmicrophone },
  { "debuggps", "Debug GPS sensor (PA1010D).", true, cmd_debuggps, "Usage: debuggps <0|1>" },
  { "debugrtc", "Debug RTC sensor (DS3231).", true, cmd_debugrtc, "Usage: debugrtc <0|1>" },
  { "debugimu", "Debug IMU sensor (BNO055).", true, cmd_debugimu, "Usage: debugimu <0|1>" },
  { "debugthermal", "Debug thermal sensor (MLX90640).", true, cmd_debugthermal, "Usage: debugthermal <0|1>" },
  { "debugtof", "Debug ToF sensor (VL53L4CX).", true, cmd_debugtof, "Usage: debugtof <0|1>" },
  { "debuginput",      "Debug input abstraction layer (HAL_Input + OLED dispatch).", true, cmd_debuginput,      "Usage: debuginput <0|1>" },
  { "debuganoencoder", "Debug ANO rotary encoder driver internals.",                 true, cmd_debuganoencoder, "Usage: debuganoencoder <0|1>" },
  { "debugapds", "Debug APDS sensor (APDS9960).", true, cmd_debugapds, "Usage: debugapds <0|1>" },
  { "debugpresence", "Debug presence sensor (STHS34PF80).", true, cmd_debugpresence, "Usage: debugpresence <0|1>" },
  // Per-sensor sub-flag setters (Lifecycle / Polling / Values)
  { "debugthermallifecycle",  "Debug thermal init/connect/recovery.",         true, cmd_debugthermallifecycle,  "Usage: debugthermallifecycle <0|1>" },
  { "debugthermalpolling",    "Debug thermal poll cadence/FPS/capture.",      true, cmd_debugthermalpolling,    "Usage: debugthermalpolling <0|1>" },
  { "debugthermalvalues",     "Debug thermal value updates/interpolation.",   true, cmd_debugthermalvalues,     "Usage: debugthermalvalues <0|1>" },
  { "debugtoflifecycle",      "Debug ToF init/connect/recovery.",             true, cmd_debugtoflifecycle,      "Usage: debugtoflifecycle <0|1>" },
  { "debugtofpolling",        "Debug ToF poll cadence/capture.",              true, cmd_debugtofpolling,        "Usage: debugtofpolling <0|1>" },
  { "debugtofvalues",         "Debug ToF range/object detection values.",     true, cmd_debugtofvalues,         "Usage: debugtofvalues <0|1>" },
  { "debuginputlifecycle",    "Debug input abstraction layer lifecycle.",     true, cmd_debuginputlifecycle,  "Usage: debuginputlifecycle <0|1>" },
  { "debuginputpolling",      "Debug input abstraction layer poll/dispatch.", true, cmd_debuginputpolling,    "Usage: debuginputpolling <0|1>" },
  { "debuginputvalues",       "Debug input abstraction layer event values.",  true, cmd_debuginputvalues,     "Usage: debuginputvalues <0|1>" },
  { "debuganoencoderlifecycle", "Debug ANO encoder init/connect/recovery.",   true, cmd_debuganoencoderlifecycle, "Usage: debuganoencoderlifecycle <0|1>" },
  { "debuganoencoderpolling",   "Debug ANO encoder poll/encoder reads.",      true, cmd_debuganoencoderpolling,   "Usage: debuganoencoderpolling <0|1>" },
  { "debuganoencodervalues",    "Debug ANO encoder rotation/button events.",  true, cmd_debuganoencodervalues,    "Usage: debuganoencodervalues <0|1>" },
  { "debugimulifecycle",      "Debug IMU init/connect/recovery.",             true, cmd_debugimulifecycle,      "Usage: debugimulifecycle <0|1>" },
  { "debugimupolling",        "Debug IMU poll cadence.",                      true, cmd_debugimupolling,        "Usage: debugimupolling <0|1>" },
  { "debugimuvalues",         "Debug IMU orientation/acceleration values.",   true, cmd_debugimuvalues,         "Usage: debugimuvalues <0|1>" },
  { "debugapdslifecycle",     "Debug APDS init/connect/recovery.",            true, cmd_debugapdslifecycle,     "Usage: debugapdslifecycle <0|1>" },
  { "debugapdspolling",       "Debug APDS poll cadence.",                     true, cmd_debugapdspolling,       "Usage: debugapdspolling <0|1>" },
  { "debugapdsvalues",        "Debug APDS color/proximity/gesture values.",   true, cmd_debugapdsvalues,        "Usage: debugapdsvalues <0|1>" },
  { "debuggpslifecycle",      "Debug GPS init/connect/recovery.",             true, cmd_debuggpslifecycle,      "Usage: debuggpslifecycle <0|1>" },
  { "debuggpspolling",        "Debug GPS poll cadence.",                      true, cmd_debuggpspolling,        "Usage: debuggpspolling <0|1>" },
  { "debuggpsvalues",         "Debug GPS NMEA/fix/coordinate values.",        true, cmd_debuggpsvalues,         "Usage: debuggpsvalues <0|1>" },
  { "debugrtclifecycle",      "Debug RTC init/connect/recovery.",             true, cmd_debugrtclifecycle,      "Usage: debugrtclifecycle <0|1>" },
  { "debugrtcpolling",        "Debug RTC poll cadence.",                      true, cmd_debugrtcpolling,        "Usage: debugrtcpolling <0|1>" },
  { "debugrtcvalues",         "Debug RTC time-read values.",                  true, cmd_debugrtcvalues,         "Usage: debugrtcvalues <0|1>" },
  { "debugfmradiolifecycle",  "Debug FM radio init/tune/recovery.",           true, cmd_debugfmradiolifecycle,  "Usage: debugfmradiolifecycle <0|1>" },
  { "debugfmradiopolling",    "Debug FM radio poll cadence.",                 true, cmd_debugfmradiopolling,    "Usage: debugfmradiopolling <0|1>" },
  { "debugfmradiovalues",     "Debug FM radio RDS/RSSI/state values.",        true, cmd_debugfmradiovalues,     "Usage: debugfmradiovalues <0|1>" },
  { "debugmiclifecycle",      "Debug microphone init/start/stop.",            true, cmd_debugmiclifecycle,      "Usage: debugmiclifecycle <0|1>" },
  { "debugmicpolling",        "Debug microphone capture cadence.",            true, cmd_debugmicpolling,        "Usage: debugmicpolling <0|1>" },
  { "debugmicvalues",         "Debug microphone level/sample values.",        true, cmd_debugmicvalues,         "Usage: debugmicvalues <0|1>" },
  { "debugpresencelifecycle", "Debug presence sensor init/connect/recovery.", true, cmd_debugpresencelifecycle, "Usage: debugpresencelifecycle <0|1>" },
  { "debugpresencepolling",   "Debug presence sensor poll cadence.",          true, cmd_debugpresencepolling,   "Usage: debugpresencepolling <0|1>" },
  { "debugpresencevalues",    "Debug presence detection values.",             true, cmd_debugpresencevalues,    "Usage: debugpresencevalues <0|1>" },
  { "debugmaps", "Debug maps (parent flag).", true, cmd_debugmaps, "Usage: debugmaps <0|1>" },
  { "debugmapsloading", "Debug map file loading and tile directory.", true, cmd_debugmapsloading, "Usage: debugmapsloading <0|1>" },
  { "debugmapsrendering", "Debug map render pipeline and feature drawing.", true, cmd_debugmapsrendering, "Usage: debugmapsrendering <0|1>" },
  { "debugmapsperf", "Debug map performance timing (render ms, tile I/O, cache, FPS).", true, cmd_debugmapsperf, "Usage: debugmapsperf <0|1>" },
#if ENABLE_ONDEVICE_LLM
  { "debugllm", "Debug on-device LLM (parent flag).", true, cmd_debugllm, "Usage: debugllm <0|1> [temp|runtime]" },
  { "debugllmload", "Debug LLM checkpoint load and validation.", true, cmd_debugllmload, "Usage: debugllmload <0|1> [temp|runtime]" },
  { "debugllmtokenizer", "Debug LLM tokenizer / BPE.", true, cmd_debugllmtokenizer, "Usage: debugllmtokenizer <0|1> [temp|runtime]" },
  { "debugllmforward", "Debug LLM transformer forward (verbose).", true, cmd_debugllmforward, "Usage: debugllmforward <0|1> [temp|runtime]" },
  { "debugllmgenerate", "Debug LLM generation loop and sampling.", true, cmd_debugllmgenerate, "Usage: debugllmgenerate <0|1> [temp|runtime]" },
  { "debugllmmemory", "Debug LLM PSRAM budget and context cap.", true, cmd_debugllmmemory, "Usage: debugllmmemory <0|1> [temp|runtime]" },
#endif
  { "debugi2c",          "Debug I2C bus (parent flag).",                                true, cmd_debugi2c,          "Usage: debugi2c <0|1>" },
  { "debugi2cbus",       "Debug I2C bus lifecycle, polling pause/resume, status bumps.", true, cmd_debugi2cbus,       "Usage: debugi2cbus <0|1>" },
  { "debugi2cdiscovery", "Debug I2C device probing, registry, scan results.",            true, cmd_debugi2cdiscovery, "Usage: debugi2cdiscovery <0|1>" },
  { "debugi2cautostart", "Debug I2C sensor auto-start orchestration + init results.",    true, cmd_debugi2cautostart, "Usage: debugi2cautostart <0|1>" },
  { "debugwifi", "Debug WiFi operations.", true, cmd_debugwifi },
  { "debugstorage", "Debug storage operations.", true, cmd_debugstorage },
  { "debugperformance", "Debug performance metrics.", true, cmd_debugperformance },
  { "debugdatetime",        "Debug NTP/date-time (parent flag).",        true, cmd_debugdatetime },
  { "debugdatetimesync",    "Debug NTP sync loop (DNS, wait, result).",  true, cmd_debugdatetimesync,    "Usage: debugdatetimesync <0|1> [temp|runtime]" },
  { "debugdatetimesetup",   "Debug NTP setup / configTime calls.",       true, cmd_debugdatetimesetup,   "Usage: debugdatetimesetup <0|1> [temp|runtime]" },
  { "debugdatetimeanchor",  "Debug NTP boot anchor write/read.",         true, cmd_debugdatetimeanchor,  "Usage: debugdatetimeanchor <0|1> [temp|runtime]" },
  { "debugdatetimeresolve", "Debug NTP timestamp resolution for users.", true, cmd_debugdatetimeresolve, "Usage: debugdatetimeresolve <0|1> [temp|runtime]" },
  { "debugverbose", "Global debug verbosity override (forces all debug + loglevel=DEBUG).", true, cmd_debugverbose, "Usage: debugverbose <0|1>" },
  { "debugbuffer", "Show debug ring buffer status.", true, cmd_debugbuffer },
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
  { "debugmemory",         "Debug memory (parent flag).",                              true, cmd_debugmemory,         "Usage: debugmemory <0|1>" },
  { "loglink",             "Route ESP-IDF logs through the unified output queue (stops UART interleave).", true, cmd_loglink, "Usage: loglink <0|1>" },
  { "debugmemoryheap",     "Debug per-task heap (free/min/largest), DRAM low watermark.", true, cmd_debugmemoryheap,    "Usage: debugmemoryheap <0|1>" },
  { "debugmemorystack",    "Debug per-task stack watermarks + peak reports.",          true, cmd_debugmemorystack,    "Usage: debugmemorystack <0|1>" },
  { "debugmemorybuffers",  "Debug response/cookie buffer sizing diagnostics.",         true, cmd_debugmemorybuffers,  "Usage: debugmemorybuffers <0|1>" },
  { "debugmqtt",           "Debug MQTT (parent flag).",                                true, cmd_debugmqtt,           "Usage: debugmqtt <0|1> [temp|runtime]" },
  { "debugmqttconnection", "Debug MQTT connect/disconnect/TLS/init.",                  true, cmd_debugmqttconnection, "Usage: debugmqttconnection <0|1>" },
  { "debugmqttpubsub",     "Debug MQTT publish/subscribe + received messages.",        true, cmd_debugmqttpubsub,     "Usage: debugmqttpubsub <0|1>" },
  { "debugmqttdiscovery",  "Debug MQTT Home Assistant auto-discovery.",                true, cmd_debugmqttdiscovery,  "Usage: debugmqttdiscovery <0|1>" },
  { "debugmqttcommands",   "Debug MQTT inbound commands + auth.",                      true, cmd_debugmqttcommands,   "Usage: debugmqttcommands <0|1>" },
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
  { "debugstoragepermissions", "Debug storage [PERM] DENY audit.", true, cmd_debugstoragepermissions },
  { "debugstack", "Low-level stack/heap trace to Serial: <on|off>.", true, cmd_debugstack },
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
  { "debugautomations", "Debug automations scheduler and actions.", true, cmd_debugautomations },
  { "debuglogger", "Debug sensor logger internals.", true, cmd_debuglogger },
  { "commandmodulesummary", "Show command module summary.", true, cmd_commandmodulesummary },
  { "settingsmodulesummary", "Show settings module summary.", true, cmd_settingsmodulesummary },
  { "outdisplay", "Enable/disable display output.", true, cmd_outdisplay, "Usage: outdisplay <0|1> [persist|temp]" },
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "outg2", "Enable/disable G2 glasses output.", false, cmd_outg2, "Usage: outg2 <0|1> - streams CLI output to G2 glasses" },
  { "debugg2", "Debug G2 smart glasses BLE operations.", true, cmd_debugg2, "Usage: debugg2 <0|1>" },
  { "debugg2lifecycle", "Debug G2 BLE lifecycle (scan/connect/MTU).", true, cmd_debugg2lifecycle, "Usage: debugg2lifecycle <0|1>" },
  { "debugg2protocol",  "Debug G2 envelope TX/RX, CRC, fragmentation.", true, cmd_debugg2protocol,  "Usage: debugg2protocol <0|1>" },
  { "debugg2events",    "Debug G2 DevEvents/SysEvents/gestures.",       true, cmd_debugg2events,    "Usage: debugg2events <0|1>" },
  { "debugg2pages",     "Debug G2 page-swap worker / hijack / lens state.", true, cmd_debugg2pages, "Usage: debugg2pages <0|1>" },
  { "debugg2heartbeat", "Debug G2 heartbeat TX + acks (loud).",         true, cmd_debugg2heartbeat, "Usage: debugg2heartbeat <0|1>" },
  { "debugg2dump",      "Debug G2 ring-buffer dumps on errors.",        true, cmd_debugg2dump,      "Usage: debugg2dump <0|1>" },
#endif
  { "outble", "Enable/disable BLE broadcast output.", false, cmd_outble, "Usage: outble <0|1> - streams broadcast output to authenticated BLE clients" },
  { "debugsr",          "Debug ESP-SR speech recognition (parent flag).", true, cmd_debugsr,          "Usage: debugsr <0|1> [temp|runtime]" },
  { "debugsrwake",      "Debug SR wake word detection events.",            true, cmd_debugsrwake,      "Usage: debugsrwake <0|1>" },
  { "debugsrcommand",   "Debug SR MultiNet command recognition.",          true, cmd_debugsrcommand,   "Usage: debugsrcommand <0|1>" },
  { "debugsrafe",       "Debug SR AFE chain (VAD/noise/gain).",            true, cmd_debugsrafe,       "Usage: debugsrafe <0|1>" },
  { "debugsrlifecycle", "Debug SR init/start/stop verbose.",                true, cmd_debugsrlifecycle, "Usage: debugsrlifecycle <0|1>" },
  { "debugsrtuning",    "Debug SR auto-tune sweeps + threshold.",           true, cmd_debugsrtuning,    "Usage: debugsrtuning <0|1>" },
  { "debugfmradio", "Debug FM Radio operations.", true, cmd_debugfmradio, "Usage: debugfmradio <0|1>" },
  { "memorysampleintervalsec", "Set memory sampling interval in seconds (0=disabled).", true, cmd_memorysampleintervalsec, "Usage: memorysampleintervalsec <0-300>" },
  { "loglevel", "Set log level (error|warn|info|debug).", true, cmd_loglevel },
  { "log", "System-wide logging to file.", false, cmd_log, "Usage: log <start|stop|status>\n  start [\"filepath\"] [flags=0xXXXX] [tags=0|1]: Begin system logging\n    filepath: Log file path, quoted (auto-generated if omitted)\n    flags: Debug flags to enable (e.g., flags=0x0203)" },
  { "webconsole", "Enable/disable browser-side debug console output in the web UI.", true, cmd_webconsole, "Usage: webconsole <0|1>" },
};

const size_t debugCommandsCount = sizeof(debugCommands) / sizeof(debugCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

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

void DebugManager::queueDebugMessage(DebugFlagMask flag, const char* message) {
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

void DebugManager::setDebugFlags(DebugFlagMask flags) { gDebugFlags = flags; }
DebugFlagMask DebugManager::getDebugFlags() const { return gDebugFlags; }

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
const char* LOG_OK_FILE = "/system/sys_logs/successful_login.log";              // ~680KB cap
const char* LOG_FAIL_FILE = "/system/sys_logs/failed_login.log";                // ~680KB cap
const char* LOG_I2C_FILE = "/system/sys_logs/i2c_errors.log";                   // 64KB cap
const char* LOG_ERROR_FILE = "/system/sys_logs/errors.log";                      // LOG_ERROR_CAP

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
  char fallbackPrefix[48];
  if (!bootTsPrefix[0]) { snprintf(fallbackPrefix, sizeof(fallbackPrefix), "[BOOT ms=%lu] | ", millis()); }
  String prefix = bootTsPrefix[0] ? String(bootTsPrefix) : String(fallbackPrefix);
  String line = prefix + "Device Powered On | Time Synced via NTP";
  
  appendLineWithCap(LOG_OK_FILE, line, LOG_CAP_BYTES);
  appendLineWithCap(LOG_FAIL_FILE, line, LOG_CAP_BYTES);
  appendLineWithCap(LOG_I2C_FILE, line, LOG_I2C_CAP);
  appendLineWithCap(LOG_ERROR_FILE, line, LOG_ERROR_CAP);
  
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
  char msBuf[24];
  snprintf(msBuf, sizeof(msBuf), "[ms=%lu] ", millis());
  return String(msBuf);
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
  
  // Use persisted path if set, otherwise auto-generate
  String filepath = (gSettings.systemLogPath.length() > 0) ? gSettings.systemLogPath : generateSystemLogFilename();
  gSystemLogCategoryTags = gSettings.systemLogCategoryTags;
  
  // Ensure any previous file handle is closed (safety check)
  if (gSystemLogFile) {
    fsLock("debug.log");
    gSystemLogFile.flush();
    gSystemLogFile.close();
    fsUnlock();
  }
  
  // Create the log file
  fsLock("debug.log");
  File f = VFS::openGuarded(filepath, "w", VFS::systemAuth("debug.log_autostart"), true);
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

// ============================================================================
// System Log Settings Module
// ============================================================================

static const SettingEntry systemLogSettingEntries[] = {
  { "systemLogAutoStart",    SETTING_BOOL,   &gSettings.systemLogAutoStart,    0, 0, nullptr, 0, 1, "Auto-start logging after boot", nullptr, false, nullptr, "log autostart" },
  { "systemLogPath", SETTING_STRING, &gSettings.systemLogPath, 0, 0, "", 0, 0, "Log file path (empty = auto-generate)", nullptr, false, nullptr, nullptr },
  { "systemLogCategoryTags", SETTING_BOOL, &gSettings.systemLogCategoryTags, 1, 0, nullptr, 0, 1, "Include category tags", nullptr, false, nullptr, nullptr },
};

extern const SettingsModule systemLogSettingsModule = {
  "systemlog",
  "logging.systemlog",
  systemLogSettingEntries,
  sizeof(systemLogSettingEntries) / sizeof(systemLogSettingEntries[0]),
  nullptr,
  "System debug logging to file"
};
