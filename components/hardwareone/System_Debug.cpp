#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <freertos/semphr.h>

#include "System_BuildConfig.h"
#include "System_Clock.h"  // Clock::isValidEpoch — one epoch-validity vocabulary
#include "System_Filesystem.h"
#include "System_VFS.h"
#include "OLED_ConsoleBuffer.h"
#include "Bluetooth.h"
#include "G2_Glasses.h"
#include "System_CLI.h"
#include "System_CLIMode.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Logging.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_User.h"  // CommandSource — recordLoginAttempt maps it to an audit path
#include "System_Utils.h"
#include "System_AuthIdentity.h"  // currentCommandContext / currentCaptureState
#include "WebServer_Utils.h"
#include "System_ESPSR.h"  // srSyncDebugLevel()

// External dependencies from .ino

// ============================================================================
// Debug System Implementation
// ============================================================================

// Debug system globals - single source of truth
// Early-boot default: all core/parent flags on for maximum verbosity until
// settings are loaded and applied (same set the pre-256-bit mask enabled as
// its "lower 32 bits"). Sensor and feature sub-flags stay off for cleaner
// output.
//
// Deliberately HAND-WRITTEN, not generated (C2 defaults audit): this mask is
// a SECOND default policy, intentionally different from the registry's — 29
// of these flags have registry intDefault 0, and the registry default is
// what persists once applyRegisteredDefaults() runs. No per-row dflt column
// could express both without inventing a bootDefault column nothing else
// reads; one expression over the frozen DEBUG_* spellings is cheaper and
// cannot drift structurally (an unknown symbol is a compile error).
static constexpr DebugFlagMask kBootDefaultDebugFlags =
    DEBUG_AUTH | DEBUG_HTTP | DEBUG_SSE | DEBUG_CLI |
    DEBUG_CMD_FLOW | DEBUG_COMMAND_SYSTEM | DEBUG_USERS | DEBUG_SYSTEM |
    DEBUG_STORAGE | DEBUG_LOGGER | DEBUG_PERFORMANCE | DEBUG_WIFI |
    DEBUG_MEMORY | DEBUG_G2 | DEBUG_RING | DEBUG_MQTT | DEBUG_FMRADIO | DEBUG_I2C |
    DEBUG_MICROPHONE | DEBUG_CAMERA |
    DEBUG_ESPNOW_CORE | DEBUG_ESPNOW_ROUTER | DEBUG_ESPNOW_MESH |
    DEBUG_ESPNOW_TOPO | DEBUG_ESPNOW_STREAM |
    DEBUG_AUTOMATIONS | DEBUG_AUTO_EXEC | DEBUG_AUTO_CONDITION |
    DEBUG_AUTO_TIMING | DEBUG_AUTO_SCHEDULER;
DebugFlagMask gDebugFlags = kBootDefaultDebugFlags;
DebugSubFlags gDebugSubFlags = {}; // All sub-flags initialized to false
char* gDebugBuffer = nullptr;
QueueHandle_t gDebugOutputQueue = nullptr;
QueueHandle_t gDebugFreeQueue = nullptr;
volatile unsigned long gDebugDropped = 0;
int gDebugQueueSize = DEBUG_QUEUE_SIZE_MIN; // Runtime queue size (set in initDebugSystem)
int gDebugPoolSize = 0;                     // Allocated DebugMessage slots

static DebugMessage* gDebugPoolPrimary = nullptr;
static DebugMessage* gDebugPoolExtra = nullptr;
static portMUX_TYPE gDebugPoolMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool gDebugPoolGrowthRequested = false;
enum DebugPoolGrowthState : uint8_t {
  DEBUG_POOL_SMALL = 0,
  DEBUG_POOL_GROWING,
  DEBUG_POOL_FULL,
  DEBUG_POOL_GROW_FAILED,
};
static DebugPoolGrowthState gDebugPoolGrowthState = DEBUG_POOL_FULL;
static constexpr int DEBUG_POOL_GROW_THRESHOLD = 24;

volatile bool gDebugVerbose = false;

// Low-level stack/heap trace toggle — see STACK_TRACEF in System_Debug.h.
// Runtime-only: not persisted, resets to false on reboot.
volatile bool gDebugStackTraceEnabled = false;

// Severity-based logging level (default: show everything)
uint8_t gLogLevel = LOG_LEVEL_DEBUG;

// System logging state
String gSystemLogPath = "";
bool gSystemLogRunning = false;
unsigned long gSystemLogLastWrite = 0;
bool gSystemLogCategoryTags = true;  // Default: enabled

// Persistent file handle for efficient logging (avoids open/close per message)
static File gSystemLogFile;
static unsigned long gSystemLogLastFlush = 0;
static uint16_t gSystemLogUnflushedCount = 0;
static const uint16_t LOG_FLUSH_MESSAGE_COUNT = 20;      // Flush every 20 messages
static const uint32_t LOG_FLUSH_INTERVAL_MS = 5000;      // Or every 5 seconds

// BLE broadcast output buffer (accumulates messages for periodic send to authenticated BLE clients)
#if ENABLE_BLUETOOTH
static String gBLEOutputBuffer;
static bool gBLEOutputBufferReserved = false;
static unsigned long gBLELastFlush = 0;
static const uint32_t BLE_OUTPUT_FLUSH_INTERVAL_MS = 150;   // Flush to BLE every 150ms (snappier; sendBLEResponse fragments)
// MUST stay <= ESP_GATT_MAX_ATTR_LEN (512). BLECharacteristic::setValue REJECTS
// any value longer than that — it logs at a level compiled out by default and
// returns WITHOUT updating the characteristic, after which the caller's notify()
// re-sends the PREVIOUS payload. So an oversize flush is not truncated, it is
// silently duplicated.
//
// This was 1024, which made every size-triggered flush 768..1023 B (the flush
// threshold is MAX - DEBUG_MSG_SIZE) — i.e. ALL of them over the limit, so on the
// plaintext path only the 150 ms timer flushes were getting through. 512 restores
// the invariant the MTU setup is written against (Bluetooth.cpp: MTU 517 => 514
// usable "fully covers the 512-byte gBLEOutputBuffer flush"). With 512 the append
// guard caps length at 511 and flushes run 256..511 B — always deliverable.
//
// The Secure-Channel path (bleScSendEncrypted) fragments at 195 B and was never
// affected; this only ever bit plaintext links (bleRawNotify).
static const size_t BLE_OUTPUT_BUFFER_MAX = 512;           // Coalesce per flush; hard-capped by setValue's 512 B limit
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
EXT_RAM_BSS_ATTR static char gHelpTailSnapshot[kHelpTailLines][kHelpTailCols];
static size_t gHelpTailCount = 0;
static size_t gHelpTailIndex = 0;
static unsigned long gHelpSuppressedCount = 0;
static StaticSemaphore_t gHelpTailMutexStorage;
static SemaphoreHandle_t gHelpTailMutex = nullptr;
static portMUX_TYPE gHelpTailInitMux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t helpTailMutex() {
  if (gHelpTailMutex) return gHelpTailMutex;
  portENTER_CRITICAL(&gHelpTailInitMux);
  if (!gHelpTailMutex)
    gHelpTailMutex = xSemaphoreCreateMutexStatic(&gHelpTailMutexStorage);
  portEXIT_CRITICAL(&gHelpTailInitMux);
  return gHelpTailMutex;
}

static void pushHelpSuppressed(const char* t) {
  if (!t) return;
  SemaphoreHandle_t mutex = helpTailMutex();
  if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
  ++gHelpSuppressedCount;
  size_t i = gHelpTailIndex % kHelpTailLines;
  strncpy(gHelpTail[i], t, kHelpTailCols - 1);
  gHelpTail[i][kHelpTailCols - 1] = '\0';
  gHelpTailIndex++;
  if (gHelpTailCount < kHelpTailLines) gHelpTailCount++;
  xSemaphoreGive(mutex);
}

void helpSuppressedTailDump() {
  unsigned long totalSuppressed = 0;
  size_t tailCount = 0;
  size_t tailIndex = 0;
  SemaphoreHandle_t mutex = helpTailMutex();
  if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    totalSuppressed = gHelpSuppressedCount;
    tailCount = gHelpTailCount;
    tailIndex = gHelpTailIndex;
    memcpy(gHelpTailSnapshot, gHelpTail, sizeof(gHelpTailSnapshot));
    xSemaphoreGive(mutex);
  }
  
  if (tailCount == 0) {
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
             tailCount, totalSuppressed);
  } else {
    snprintf(header, sizeof(header), 
             "════════ Suppressed Output Tail (%zu lines) ════════",
             tailCount);
  }
  broadcastOutput(header);
  
  // Dump tail buffer
  size_t start = (tailIndex >= tailCount) ? (tailIndex - tailCount) : 0;
  for (size_t n = 0; n < tailCount; n++) {
    size_t idx = (start + n) % kHelpTailLines;
    broadcastOutput(gHelpTailSnapshot[idx]);
  }
  
  broadcastOutput("═══════════════════════════════════════════════════════════════");
}

// ============================================================================
// Initialization
// ============================================================================

// Forward declaration (definition with I2C helpers below)
static String buildTimestampPrefix();

// Forward declaration (definition with logSystemEvent below)
static void flushPreInitEvents();
static bool growDebugPoolIfNeeded(bool force);

// Debug output task - single writer for all debug messages
static TaskHandle_t gDebugOutputTaskHandle = nullptr;

void debugOutputTask(void* parameter) {
  while (true) {
    DebugMessage* msg = nullptr;
    if (xQueueReceive(gDebugOutputQueue, &msg, portMAX_DELAY) == pdTRUE && msg) {
      if (gDebugPoolGrowthRequested) {
        (void)growDebugPoolIfNeeded(true);
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
              if (gOutputFlags & MSG_ROUTE_SERIAL) Serial.printf("%s\n", mark);
              if (gWebMirror.buf) gWebMirror.appendDirect(mark, (size_t)mw, true);
            }
            sDropSeen   = dropped;
            sDropMarkMs = nowMs;
          }
        }
      }

      // --- Per-sink output gated by msg->routing AND hardware availability ---

      // Serial ambient output is suppressed only for a serial-owned mode. Do
      // not revive the former process-global help gate: web/file/BLE/OLED/G2
      // sinks continue normally, and another transport's mode never silences
      // this console. Explicit mode renders and urgent notices still pass.
      const bool suppressAmbientSerial =
          (msg->routing & MSG_ROUTE_SERIAL) &&
          !(msg->routing & MSG_ROUTE_ALLOW_IN_HELP) &&
          strncmp(msg->text, "[SECURITY]", 10) != 0 &&
          strncmp(msg->text, "[AUTH]", 6) != 0 &&
          strncmp(msg->text, "[ERROR]", 7) != 0 &&
          cliModeSuppressesAmbientSerial();
      if (suppressAmbientSerial) {
        pushHelpSuppressed(msg->text);
      }
      if (!suppressAmbientSerial &&
          (msg->routing & MSG_ROUTE_SERIAL) &&
          (gOutputFlags & MSG_ROUTE_SERIAL)) {
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
      if ((msg->routing & MSG_ROUTE_FILE) && (gOutputFlags & MSG_ROUTE_FILE) &&
          gSystemLogRunning && gSystemLogPath.length() > 0) {
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

      // System-event ring (always, independent of routing — [EVENT] lines are
      // the always-on lifecycle record; see logSystemEvent()). Low volume by
      // design (discrete decisions, not polling), so no dedupe window needed.
      if (filesystemReady && strncmp(msg->text, "[EVENT]", 7) == 0) {
        String line = buildTimestampPrefix();
        line += msg->text;
        appendLineWithCap(LOG_EVENTS_FILE, line, LOG_EVENTS_CAP);
      }

      // Structured event-stream tee (always, independent of routing) — one
      // line per event-ring entry, produced by systemEventLogTick() with the
      // [EVLOG] prefix and no display sinks in its route mask, so this file
      // append is the line's only destination.
      if (filesystemReady && strncmp(msg->text, "[EVLOG]", 7) == 0) {
        String line = buildTimestampPrefix();
        line += msg->text;
        appendLineWithCap(LOG_EVENT_STREAM_FILE, line, LOG_EVENT_STREAM_CAP);
      }

      // OLED console
      #if ENABLE_OLED_DISPLAY
      if ((msg->routing & MSG_ROUTE_OLED) && gOledConsole.mutex) {
        gOledConsole.append(msg->text, msg->timestamp);
      }
      #endif

      // BLE broadcast output
      #if ENABLE_BLUETOOTH
      if ((msg->routing & MSG_ROUTE_BLE) && (gOutputFlags & MSG_ROUTE_BLE) &&
          isBLEConnected() && bleHasAuthenticatedSession()) {
        if (!gBLEOutputBufferReserved) {
          gBLEOutputBufferReserved = gBLEOutputBuffer.reserve(BLE_OUTPUT_BUFFER_MAX);
        }
        size_t msgLen = strlen(msg->text);   // <= DEBUG_MSG_SIZE (256) < BLE_OUTPUT_BUFFER_MAX
        if (gBLEOutputBuffer.length() + msgLen + 2 < BLE_OUTPUT_BUFFER_MAX) {
          gBLEOutputBuffer += msg->text;
          gBLEOutputBuffer += "\n";
        }
        // NON-BLOCKING flush: never stall this single debugOutputTask on the secure-channel tx
        // mutex. A paced multi-fragment command result can hold that mutex for hundreds of ms;
        // blocking here would stop draining gDebugOutputQueue and starve serial/web/OLED/file.
        // If the tx is busy we keep buffering and retry on the next message/interval; only when
        // the buffer is also full do we drop (best-effort console mirror — history is
        // authoritative). The "[output] dropped" marker still surfaces any loss.
        unsigned long now = millis();
        bool full = gBLEOutputBuffer.length() >= BLE_OUTPUT_BUFFER_MAX - DEBUG_MSG_SIZE;
        bool due  = (now - gBLELastFlush >= BLE_OUTPUT_FLUSH_INTERVAL_MS);
        if (gBLEOutputBuffer.length() > 0 && (full || due)) {
          if (sendBLEResponse(gBLEOutputBuffer.c_str(), gBLEOutputBuffer.length(), /*blocking=*/false)) {
            gBLEOutputBuffer = "";
            gBLELastFlush = now;
          } else if (full) {
            gBLEOutputBuffer = "";   // tx busy and buffer full → drop to bound growth
            gBLELastFlush = now;
          }
        }
      }
      #endif

      // G2 glasses output
      #if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
      if ((msg->routing & MSG_ROUTE_G2) && (gOutputFlags & MSG_ROUTE_G2) && isG2Connected()) {
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

// Add the second half of the PSRAM-backed message pool before the initial
// 96 slots are exhausted. Queue capacities are fixed at 192 from boot, so
// growth only allocates messages and seeds their pointers; no FreeRTOS queue
// replacement or consumer handoff is required.
static bool growDebugPoolIfNeeded(bool force) {
  if (gDebugPoolGrowthState == DEBUG_POOL_GROW_FAILED) {
    gDebugPoolGrowthRequested = false;
    return false;
  }
  if (!gDebugFreeQueue || gDebugQueueSize <= gDebugPoolSize ||
      gDebugPoolGrowthState == DEBUG_POOL_FULL) {
    gDebugPoolGrowthRequested = false;
    return true;
  }

  if (xPortInIsrContext()) {
    // Heap allocation is forbidden in ISR context. The consumer task sees this
    // request while draining the message that caused the low-water condition.
    gDebugPoolGrowthRequested = true;
    return false;
  }

  int freeSlots = (int)uxQueueMessagesWaiting(gDebugFreeQueue);
  if (!force && freeSlots > DEBUG_POOL_GROW_THRESHOLD) return true;

  taskENTER_CRITICAL(&gDebugPoolMux);
  if (gDebugPoolGrowthState != DEBUG_POOL_SMALL) {
    bool ready = (gDebugPoolGrowthState == DEBUG_POOL_FULL);
    taskEXIT_CRITICAL(&gDebugPoolMux);
    return ready;
  }
  gDebugPoolGrowthState = DEBUG_POOL_GROWING;
  taskEXIT_CRITICAL(&gDebugPoolMux);

  const int addCount = gDebugQueueSize - gDebugPoolSize;
  // Expansion is optional and must never spill ~28 KB into scarce internal
  // DRAM if PSRAM is fragmented. In that case retain the 96-slot pool and let
  // the existing drop accounting surface any later saturation.
  DebugMessage* extra = (DebugMessage*)ps_alloc(
      (size_t)addCount * sizeof(DebugMessage), AllocPref::RequirePSRAM, "debug.pool.growth");
  if (!extra) {
    taskENTER_CRITICAL(&gDebugPoolMux);
    gDebugPoolGrowthState = DEBUG_POOL_GROW_FAILED;
    gDebugPoolGrowthRequested = false;
    taskEXIT_CRITICAL(&gDebugPoolMux);
    if (gOutputFlags & MSG_ROUTE_SERIAL) {
      Serial.printf("[DEBUG] Pool growth failed; remaining at %d slots\n",
                    gDebugPoolSize);
    }
    return false;
  }

  int seeded = 0;
  for (; seeded < addCount; seeded++) {
    DebugMessage* p = &extra[seeded];
    if (xQueueSend(gDebugFreeQueue, &p, 0) != pdTRUE) break;
  }

  taskENTER_CRITICAL(&gDebugPoolMux);
  gDebugPoolExtra = extra;
  gDebugPoolSize += seeded;
  gDebugPoolGrowthState =
      (gDebugPoolSize >= gDebugQueueSize) ? DEBUG_POOL_FULL : DEBUG_POOL_GROW_FAILED;
  gDebugPoolGrowthRequested = false;
  taskEXIT_CRITICAL(&gDebugPoolMux);

  if (gOutputFlags & MSG_ROUTE_SERIAL) {
    Serial.printf("[DEBUG] Pool grew to %d slots before saturation\n",
                  gDebugPoolSize);
  }
  return gDebugPoolGrowthState == DEBUG_POOL_FULL;
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

// Bounded wait for the line queue to fully drain. Used by the serial result
// writer (deliverCommandResult) so a directly-written result blob doesn't
// overtake lines the command streamed through the queue during execution.
// Two caveats, both accepted: (1) queue-empty is not a hard "last byte
// reached the UART" guarantee — the drain may still be printing the message
// it just popped — but the UART/CDC driver's per-write lock keeps that
// residual race whole-line, never a mid-line splice. (2) On the TIMEOUT
// exit (queue still non-empty after maxWaitMs) the remaining lines print
// after the blob instead of before it — bounded delay is deliberately
// chosen over blocking the caller on a wedged drain.
void debugWaitOutputDrained(uint32_t maxWaitMs) {
  if (!gDebugOutputQueue) return;
  uint32_t start = millis();
  while (uxQueueMessagesWaiting(gDebugOutputQueue) > 0) {
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
    return "Error: Invalid level. Use: error(0), warn(1), info(2), or debug(3)";
  }

  if (newLevel < LOG_LEVEL_ERROR) newLevel = LOG_LEVEL_ERROR;
  if (newLevel > LOG_LEVEL_DEBUG) newLevel = LOG_LEVEL_DEBUG;

  setDebugSetting(gSettings.logLevel, newLevel);
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

  // Keep queue capacity at the proven 192-slot burst ceiling on PSRAM builds,
  // but initially allocate only 96 expensive DebugMessage objects. The pool
  // grows once, before exhaustion, when producers cross the low-water mark.
  size_t psramSize = ESP.getPsramSize();
  bool hasPsram = (psramSize > 0);
  gDebugQueueSize = hasPsram ? DEBUG_QUEUE_SIZE_MAX : DEBUG_QUEUE_SIZE_MIN;
  const int initialPoolSize =
      hasPsram ? DEBUG_POOL_SIZE_INITIAL_PSRAM : DEBUG_QUEUE_SIZE_MIN;
  
  if (gOutputFlags & MSG_ROUTE_SERIAL) {
    Serial.printf("[DEBUG] Queue capacity: %d, initial pool: %d slots (%s)\n",
                  gDebugQueueSize, initialPoolSize,
                  hasPsram ? "PSRAM, adaptive" : "internal RAM");
  }

  // Allocate debug buffer in PSRAM
  if (!gDebugBuffer) {
    gDebugBuffer = (char*)ps_alloc(GLOBAL_DEBUG_BUFFER_SIZE, AllocPref::PreferPSRAM, "debug.buf");
    if (!gDebugBuffer) {
      if (gOutputFlags & MSG_ROUTE_SERIAL) {
        Serial.println("FATAL: Failed to allocate debug buffer");
      }
      while (1) delay(1000);
    }
  }

  // Create debug free queue (pool of reusable DebugMessage pointers)
  if (!gDebugFreeQueue) {
    gDebugFreeQueue = xQueueCreate(gDebugQueueSize, sizeof(DebugMessage*));
    if (!gDebugFreeQueue) {
      if (gOutputFlags & MSG_ROUTE_SERIAL) {
        Serial.println("FATAL: Failed to create debug free queue");
      }
      while (1) delay(1000);
    }

    // Pre-allocate the pool itself (prefer PSRAM if available)
    AllocPref allocPref = hasPsram ? AllocPref::PreferPSRAM : AllocPref::DefaultHeap;
    gDebugPoolPrimary = (DebugMessage*)ps_alloc(
        (size_t)initialPoolSize * sizeof(DebugMessage), allocPref, "debug.pool");
    if (!gDebugPoolPrimary) {
      if (gOutputFlags & MSG_ROUTE_SERIAL) {
        Serial.println("FATAL: Failed to allocate debug message pool");
      }
      while (1) delay(1000);
    }
    gDebugPoolSize = initialPoolSize;
    gDebugPoolGrowthState =
        hasPsram ? DEBUG_POOL_SMALL : DEBUG_POOL_FULL;
    gDebugPoolGrowthRequested = false;

    // Seed free queue with pointers into the pool
    for (int i = 0; i < initialPoolSize; ++i) {
      DebugMessage* p = &gDebugPoolPrimary[i];
      xQueueSend(gDebugFreeQueue, &p, 0);
    }
  }

  // Create debug output queue (stores pointers to heap-allocated messages)
  if (!gDebugOutputQueue) {
    gDebugOutputQueue = xQueueCreate(gDebugQueueSize, sizeof(DebugMessage*));
    if (!gDebugOutputQueue) {
      if (gOutputFlags & MSG_ROUTE_SERIAL) {
        Serial.println("FATAL: Failed to create debug output queue");
      }
      while (1) delay(1000);
    }
    DEBUG_SYSTEMF("Debug output queue created (capacity=%d, pool=%d in %s)",
                  gDebugQueueSize, gDebugPoolSize,
                  hasPsram ? "PSRAM" : "internal RAM");
  }

  // Create debug output task
  if (!gDebugOutputTaskHandle) {
    taskStackRecord("debug_out", DEBUG_OUT_STACK_WORDS);
    BaseType_t result = xTaskCreatePinnedToCore(
      debugOutputTask,
      "debug_out",
      DEBUG_OUT_STACK_WORDS,  // ~14KB stack — see System_TaskUtils.h for sizing rationale
      nullptr,
      TASK_PRIORITY_LOW,
      &gDebugOutputTaskHandle,
      PRO_CORE  // serial/log I/O drain — belongs with the I/O sinks on Core 0
    );
    if (result != pdPASS) {
      if (gOutputFlags & MSG_ROUTE_SERIAL) {
        Serial.println("FATAL: Failed to create debug output task");
      }
      while (1) delay(1000);
    }
    DEBUG_SYSTEMF("Debug output task created");
  }

  // NOTE: Do NOT reset gDebugFlags here - applySettings() may have already set them
  // The flags are managed by applySettings() in settings.cpp
  
  // BLE and OLED output buffers are lazy. They are claimed only once the
  // corresponding runtime feature is active, so compiled-but-disabled
  // Bluetooth/OLED builds do not pay their idle heap/DRAM cost.
  
  // The web/CLI mirror is intentionally lazy. startHttpServer() claims it for
  // the web console, while the CLI help/clear paths claim it on first use.
  
  // Flush system events recorded before the queue existed (early boot:
  // filesystem init, settings load) so they reach LOG_EVENTS_FILE.
  flushPreInitEvents();

  DEBUG_SYSTEMF("Debug system initialized");
}

// ============================================================================
// Buffer Management
// ============================================================================

bool ensureDebugBuffer() {
  if (!gDebugBuffer) {
    gDebugBuffer = (char*)ps_alloc(GLOBAL_DEBUG_BUFFER_SIZE, AllocPref::PreferPSRAM, "debug.buf");
    if (!gDebugBuffer) {
      if (gOutputFlags & MSG_ROUTE_SERIAL) {
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

  // Grow while there is still headroom, not after a frame has already been
  // lost. Normal tasks perform the one-time allocation synchronously; ISR
  // producers only request it for the consumer task.
  int freeSlots = isr
      ? (int)uxQueueMessagesWaitingFromISR(gDebugFreeQueue)
      : (int)uxQueueMessagesWaiting(gDebugFreeQueue);
  if (freeSlots <= DEBUG_POOL_GROW_THRESHOLD) {
    if (isr) gDebugPoolGrowthRequested = true;
    else     (void)growDebugPoolIfNeeded(false);
  }

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
  extern bool gThermalRunning, gImuRunning, gTofRunning, gFmRadioRunning;
  extern TaskHandle_t gThermalTaskHandle, gImuTaskHandle, gTofTaskHandle, gFmRadioTaskHandle;
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  if (currentTask == gThermalTaskHandle && !gThermalRunning) return;
  if (currentTask == gImuTaskHandle && !gImuRunning) return;
  if (currentTask == gTofTaskHandle && !gTofRunning) return;
  if (currentTask == gFmRadioTaskHandle && !gFmRadioRunning) return;

  // Format into a stack line, then hand to the shared enqueue primitive
  // (ISR-safe; centralizes slot grab / send / drop + [CUT]). There is no split
  // layer — by design, see the note at the enqueueChunk call in
  // broadcastOutputCore: callers prove their line fits 255 B and pack multiple
  // lines with '\n' themselves. Nothing splits at runtime.
  char line[DEBUG_MSG_SIZE];
  va_list args;
  va_start(args, fmt);
  int wn = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (wn < 0) return;
  size_t llen = (wn < (int)sizeof(line)) ? (size_t)wn : (sizeof(line) - 1);
  enqueueChunk(line, llen, MSG_ROUTE_ALL, flag, wn >= (int)DEBUG_MSG_SIZE);
}

// ── System event log (always-on) ─────────────────────────────────────────────
// logSystemEvent() records low-volume lifecycle decisions (boot, FS format,
// settings load/save failures, WiFi connects) as "[EVENT][CAT] ..." lines,
// independent of debug flags and log level. The output task tees every
// [EVENT] line durably to LOG_EVENTS_FILE (see debugOutputTask). Events
// emitted before initDebugSystem() — filesystem init, settings load — echo
// to Serial immediately and are held here until the queue exists; the ring
// keeps the FIRST N events (chronologically the boot record that matters).
static const int    kPreInitEventSlots = 12;
static const size_t kPreInitEventLen   = 192;
EXT_RAM_BSS_ATTR static char gPreInitEvents[kPreInitEventSlots][kPreInitEventLen];
static int gPreInitEventCount   = 0;
static int gPreInitEventDropped = 0;

void logSystemEvent(const char* category, const char* fmt, ...) {
  if (!fmt) return;
  char line[DEBUG_MSG_SIZE];
  int hdr = snprintf(line, sizeof(line), "[EVENT][%s] ", category ? category : "SYS");
  if (hdr < 0 || hdr >= (int)sizeof(line)) return;
  va_list args;
  va_start(args, fmt);
  int wn = vsnprintf(line + hdr, sizeof(line) - hdr, fmt, args);
  va_end(args);
  if (wn < 0) return;
  size_t llen = strnlen(line, sizeof(line) - 1);

  if (gDebugOutputQueue && gDebugFreeQueue) {
    enqueueChunk(line, llen, MSG_ROUTE_ALL | MSG_ROUTE_ALLOW_IN_HELP, 0,
                 wn >= (int)(sizeof(line) - hdr));
    return;
  }

  // Debug system not up yet (early boot): show it on serial now, keep a copy
  // (with its true uptime) for the durable log once the queue exists.
  Serial.printf("[%lu] %s\n", millis(), line);
  if (gPreInitEventCount < kPreInitEventSlots) {
    snprintf(gPreInitEvents[gPreInitEventCount], kPreInitEventLen,
             "%s (+%lums)", line, millis());
    gPreInitEventCount++;
  } else {
    gPreInitEventDropped++;
  }
}

bool debugQueueLine(const char* text, uint8_t routing) {
  if (!text || !gDebugOutputQueue || !gDebugFreeQueue) return false;
  // Preserve the enqueue path's "truncation is never silent" invariant for
  // future callers: an over-length line is clamped AND [CUT]-marked.
  size_t len = strnlen(text, DEBUG_MSG_SIZE);
  bool over = len > DEBUG_MSG_SIZE - 1;
  return enqueueChunk(text, over ? DEBUG_MSG_SIZE - 1 : len, routing, 0, over);
}

// Called at the end of initDebugSystem(): move early-boot events into the
// queue so they reach LOG_EVENTS_FILE. They already printed to Serial at
// emit time, so the flush routes everywhere except Serial (no duplicates).
static void flushPreInitEvents() {
  const uint8_t route = (MSG_ROUTE_ALL & ~MSG_ROUTE_SERIAL) | MSG_ROUTE_ALLOW_IN_HELP;
  for (int i = 0; i < gPreInitEventCount; i++) {
    enqueueChunk(gPreInitEvents[i], strnlen(gPreInitEvents[i], kPreInitEventLen - 1),
                 route, 0, false);
  }
  if (gPreInitEventDropped > 0) {
    char note[80];
    int nn = snprintf(note, sizeof(note),
                      "[EVENT][SYS] %d early-boot event(s) dropped (ring full)",
                      gPreInitEventDropped);
    if (nn > 0) enqueueChunk(note, (size_t)nn, route, 0, false);
  }
  gPreInitEventCount = 0;
  gPreInitEventDropped = 0;
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

  // 3. Skip output if current task is a sensor task that's been disabled
  extern bool gThermalRunning, gImuRunning, gTofRunning;
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  extern TaskHandle_t gThermalTaskHandle, gImuTaskHandle, gTofTaskHandle;
  if (currentTask == gThermalTaskHandle && !gThermalRunning) return;
  if (currentTask == gImuTaskHandle && !gImuRunning) return;
  if (currentTask == gTofTaskHandle && !gTofRunning) return;

  // 5. Compute per-message route mask
  uint8_t route;
  if (routeOverride) {
    route = routeOverride;
  } else if (currentCommandContext()) {
    // outputMask holds MSG_ROUTE_* bits directly; keep only the sinks a
    // command may address (OLED/G2 are appended unconditionally below).
    // Reads the calling task's slot; broadcasts from non-cmd_exec tasks
    // see nullptr and fall through to MSG_ROUTE_ALL below.
    uint32_t mask = getCurrentCommandOutputMask();
    route = (uint8_t)(mask & (MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE))
          | MSG_ROUTE_OLED | MSG_ROUTE_G2;  // OLED and G2 always receive command output
  } else {
    route = MSG_ROUTE_ALL;  // non-command output goes to all sinks
  }
  // gInHelpRender is legacy process-global render state, but only cmd_exec's
  // current command is allowed to mark its own help pages as explicit output.
  // Background tasks racing a render have no CommandContext and remain
  // ambient, so they cannot punch through a serial-owned help session.
  if (gInHelpRender && currentCommandContext())
    route |= MSG_ROUTE_ALLOW_IN_HELP;

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

// ---------------------------------------------------------------------------
// CLI next-step hints
// ---------------------------------------------------------------------------
// One understated "what to do next" line for command output. It helps a human
// discover the right follow-up command, and lets tools/automation recover when
// a command's output isn't the value they were after. The format lives in ONE
// place — restyle it here and every hint in the project updates. Use sparingly:
// only where the output is a genuine dead end or could be misread, never where
// the output already answers the question. For JSON command output, set a
// top-level "hint" string field instead of calling these.
// Log de-spam: returns true at most once per windowMs (first call always true).
// Generalized from notifyCooldownOk / logBondAuthFailure so flapping-peer WARNs
// share one throttle primitive instead of re-rolling the millis() guard.
bool logCooldownOk(uint32_t& lastMs, uint32_t windowMs) {
  uint32_t now = millis();
  if (lastMs != 0 && (now - lastMs) < windowMs) return false;
  lastMs = now;
  return true;
}

void cliHint(const char* text) {
  if (!text || !text[0]) return;
  BROADCAST_PRINTF("Hint: %s", text);
}

void cliHintf(const char* fmt, ...) {
  char buf[224];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  cliHint(buf);
}

// Convenience for the most common case: the output is a listing/catalog, not a
// reading. `readHint` names the read command(s); `extraNote` (optional) is a
// short caveat (e.g. how a filter matches). The hint leads with the action.
void emitListingTrailer(const char* what, const char* readHint, const char* extraNote) {
  (void)what;
  broadcastOutput("");
  if (extraNote && extraNote[0]) cliHint(extraNote);
  cliHintf("to read a value — %s", readHint);
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
  unsigned long n = 0;
  SemaphoreHandle_t mutex = helpTailMutex();
  if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    n = gHelpSuppressedCount;
    gHelpSuppressedCount = 0;
    gHelpTailCount = 0;
    gHelpTailIndex = 0;
    xSemaphoreGive(mutex);
  }
  if (n > 0) {
    // Minimal one-line notice to keep UI clean
    char msg[96];
    snprintf(msg, sizeof(msg), "(Note) Suppressed %lu lines during help.", (unsigned long)n);
    broadcastOutput(msg);
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
    bool enabled = (gOutputFlags & MSG_ROUTE_G2) != 0;
    bool connected = isG2Connected();
    EXT_RAM_BSS_ATTR static char buf[128];
    snprintf(buf, sizeof(buf), "G2 output: %s, G2 connected: %s",
             enabled ? "yes" : "no", connected ? "yes" : "no");
    return buf;
  }
  if (v) {
    gOutputFlags |= MSG_ROUTE_G2;
    return "G2 output enabled (messages will stream to glasses when connected)";
  } else {
    gOutputFlags &= ~MSG_ROUTE_G2;
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
    bool enabled = (gOutputFlags & MSG_ROUTE_BLE) != 0;
    bool connected = isBLEConnected();
    bool authed = bleHasAuthenticatedSession();
    EXT_RAM_BSS_ATTR static char buf[160];
    snprintf(buf, sizeof(buf), "BLE output: %s, BLE connected: %s, authenticated: %s",
             enabled ? "yes" : "no", connected ? "yes" : "no", authed ? "yes" : "no");
    return buf;
  }
  if (v) {
    gOutputFlags |= MSG_ROUTE_BLE;
    return "BLE broadcast output enabled (messages will stream to authenticated BLE clients)";
  } else {
    gOutputFlags &= ~MSG_ROUTE_BLE;
    return "BLE broadcast output disabled";
  }
  #else
  return "Bluetooth not compiled (ENABLE_BLUETOOTH=0)";
  #endif
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

const char* cmd_debugverbose(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  if (ca.count() == 0) return gDebugVerbose ? "debugVerbose is ON" : "debugVerbose is OFF";
  int v = ca.argInt(0, -1);
  if (v != 0 && v != 1) return "Error: invalid arguments — Usage: debugverbose <0|1>";
  gDebugVerbose = (v == 1);
  return gDebugVerbose ? "debugVerbose enabled" : "debugVerbose disabled";
}

const char* cmd_webconsole(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  int v = valStr.toInt();
  setDebugSetting(gSettings.webConsoleDebug, (bool)(v != 0));
  return gSettings.webConsoleDebug ? "webConsole enabled (persistent)" : "webConsole disabled (persistent)";
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
  setDebugSetting(gSettings.memorySampleIntervalSec, v);
  snprintf(gDebugBuffer, 1024, "Memory sample interval set to %d sec%s", v, v == 0 ? " (disabled)" : "");
  return gDebugBuffer;
}

const char* cmd_debugbuffer(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gDebugOutputQueue) {
    return "Error: Debug output queue is not initialized";
  }

  int depth = gDebugOutputQueue ? uxQueueMessagesWaiting(gDebugOutputQueue) : 0;
  int free = gDebugFreeQueue ? uxQueueMessagesWaiting(gDebugFreeQueue) : 0;
  int total = gDebugPoolSize;
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
  BROADCAST_PRINTF("  Pool: %d/%d messages", total, gDebugQueueSize);
  BROADCAST_PRINTF("  Queued: %d (%d%%)", depth, pct);
  BROADCAST_PRINTF("  Free: %d messages", free);
  BROADCAST_PRINTF("  Dropped: %lu (queue full)", dropped);
  BROADCAST_PRINTF("  Status: %s", status);

  return "OK";
}

const char* cmd_debugflags(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  // gDebugVerbose short-circuits every gate (see isDebugFlagSet), so the mask
  // below does not describe what is actually emitting while it is on.
  if (gDebugVerbose) {
    BROADCAST_PRINTF("debugVerbose: ON — all debug output is forced regardless of the mask below");
  }

  const DebugFlagMask flags = getDebugFlags();
  BROADCAST_PRINTF("Debug flags: 0x%016llX:%016llX:%016llX:%016llX",
                   (unsigned long long)(uint64_t)(flags >> 192),
                   (unsigned long long)(uint64_t)(flags >> 128),
                   (unsigned long long)(uint64_t)(flags >> 64),
                   (unsigned long long)(uint64_t)flags);

  // Resolve each bit on its own so a set sub-flag reports its own name rather
  // than the parent's. Bits with no entry in getDebugCategoryName are counted
  // instead of listed — the hex words above are where to find them.
  int named = 0;
  int unnamed = 0;
  char line[128] = "";
  size_t used = 0;
  for (int bit = 0; bit < 256; bit++) {
    const DebugFlagMask one = DEBUG_BIT(bit);
    if (!(flags & one)) continue;
    const char* name = getDebugCategoryName(one);
    if (strcmp(name, "UNKNOWN") == 0) { unnamed++; continue; }
    if (used + strlen(name) + 2 > sizeof(line)) {
      BROADCAST_PRINTF("  %s", line);
      used = 0;
    }
    used += snprintf(line + used, sizeof(line) - used, "%s%s", used ? " " : "", name);
    named++;
  }
  if (used > 0) BROADCAST_PRINTF("  %s", line);

  snprintf(gDebugBuffer, 1024, "%d named flag%s set, %d unnamed bit%s",
           named, named == 1 ? "" : "s", unnamed, unnamed == 1 ? "" : "s");
  return gDebugBuffer;
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
  if (!on && !off) return "Error: invalid arguments — Usage: loglink <0|1|on|off>";
  setIdfLogBridge(on);
  return on
    ? "loglink ON — ESP-IDF logs now share the firmware output queue (no more UART interleave)"
    : "loglink OFF — ESP-IDF logs restored to direct UART";
}

// ============================================================================
// Aggregated-parent recompute — ONE mechanism generated from the tables in
// System_DebugFlags.h (DBG_AGG_FAMILY_LIST / DBG_SUBBOOL_LIST / the
// settingsField column), replacing the per-family sync helpers with the
// same per-family terms. Only the 14 listed families are ever recomputed;
// every other parent is an explicit master switch owned by its toggle.
// ============================================================================

// Persistent-layer column: each flag row's gSettings bool, nullptr for the
// DBG_NO_SETTING control row. Indexed by DbgFlagIdx.
#define DBG_SETPTR_0(field) &gSettings.field,
#define DBG_SETPTR_1(field) nullptr,
#define DBG_X(SYM, bit, BANK, parentBit, tag, field, ...) \
  DBG_PP_CAT(DBG_SETPTR_, DBG_SF_IS_NONE(field))(field)
static constexpr bool* kDbgSettingPtr[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };
#undef DBG_X
#undef DBG_SETPTR_1
#undef DBG_SETPTR_0

// Runtime-layer column: each bitless sub's gDebugSubFlags member. Indexed by
// DbgSubIdx, parallel to kDbgSubParentIdx.
#define DBG_S(SYM, subField, settingsField, PARENT_SYM, ...) &gDebugSubFlags.subField,
static constexpr bool* kDbgSubRuntimePtr[DBG_SUBBOOL_COUNT] = { DBG_SUBBOOL_LIST(DBG_S) };
#undef DBG_S

// Every aggregated root must have a persisted bool — the SUBBOOLS expression
// dereferences it unconditionally.
static constexpr bool dbgAggRootsHaveSettings() {
  for (int i = 0; i < DBG_FLAG_COUNT; ++i)
    if (kDbgAggMode.m[i] != DBG_AGG_NONE && kDbgSettingPtr[i] == nullptr) return false;
  return true;
}
static_assert(dbgAggRootsHaveSettings(), "aggregated family root lacks a settingsField");

// True when any of the family's bitless runtime subs is enabled (the layer
// `temp` toggles write).
static bool dbgAnyRuntimeSub(DbgFlagIdx root) {
  for (int i = 0; i < DBG_SUBBOOL_COUNT; ++i)
    if (kDbgSubParentIdx[i] == root && *kDbgSubRuntimePtr[i]) return true;
  return false;
}

// True when any of the family's persisted booleans is set — every flag row
// in the family's bank, the root's own field included.
static bool dbgAnyFamilySetting(DbgFlagIdx root) {
  for (int i = 0; i < DBG_FLAG_COUNT; ++i)
    if (kDbgBank[i] == kDbgBank[root] && kDbgSettingPtr[i] && *kDbgSettingPtr[i]) return true;
  return false;
}

// True when any family-bank bit OTHER than the root's own is up in the live
// mask — temp-toggled children. Bank membership, not kDbgParentBit: BT/SR
// sub rows carry no parent link (their macros pass bare sub-bits), yet their
// temp-set bits must still raise the parent, exactly as the old helpers'
// hand-written child masks did.
static bool dbgAnyRuntimeChildBit(DbgFlagIdx root) {
  const DebugFlagMask flags = getDebugFlags();
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    if (i == (int)root || kDbgBank[i] != kDbgBank[root]) continue;
    if ((flags & kDbgMask[i]) != (DebugFlagMask)0) return true;
  }
  return false;
}

void dbgRecomputeParent(DbgFlagIdx root) {
  bool any = false;
  switch (kDbgAggMode.m[root]) {
    case DBG_AGG_NONE:
      return;  // not an aggregated family — its bit belongs to its toggle alone
    case DBG_AGG_SUBBOOLS:       // root setting OR bitless runtime subs
      any = *kDbgSettingPtr[root] || dbgAnyRuntimeSub(root);
      break;
    case DBG_AGG_SETTINGS:       // family settings only (LLM)
      any = dbgAnyFamilySetting(root);
      break;
    case DBG_AGG_SETTINGS_BITS:  // family settings OR temp-set child bits (BT, SR)
      any = dbgAnyFamilySetting(root) || dbgAnyRuntimeChildBit(root);
      break;
  }
  updateParentDebugFlag(kDbgMask[root], any);
}

void dbgRecomputeAllParents() {
  for (int i = 0; i < DBG_FLAG_COUNT; ++i)
    if (kDbgAggMode.m[i] != DBG_AGG_NONE) dbgRecomputeParent((DbgFlagIdx)i);
}

// ============================================================================
// Generated CLI debug-command dispatch (C3).
//
// The ~159 mechanical flag/sub command handlers are no longer hand-written:
// each cmd_<name> is a generated thunk (below) forwarding to one of two shared
// dispatchers. dbgFlagCmd()/dbgSubCmd() reproduce the pre-C3 handler bodies
// exactly (temp = live mask bit only; persist = gSettings + bit; bitless subs
// always write their gDebugSubFlags runtime bool and recompute the family
// parent) with ONE deliberate change: a STRICT 0|1 contract — any other int is
// rejected with the same error shape the already-strict handlers used. The
// non-uniform side effects (HTTPS TLS log levels; the SR level-sync; the
// LLM/SR/Bluetooth family recomputes) live in dbgApplyHook(), keyed by
// DbgFlagIdx. Bespoke handlers that do something non-mechanical stay
// hand-written and keep their debugCommands[] rows verbatim.
// ============================================================================

// Per-flag-row columns, indexed by DbgFlagIdx (parallel to kDbgSettingPtr).
//   kDbgCmdRetName — camelCase settingsField, used verbatim in the
//                    "<name> enabled/disabled (…)" reply (e.g. "debugHttp").
//   kDbgCmdIdent   — CLI token for the strict-usage error (e.g. "debughttp").
// The ALWAYS control row's entries are never read — it carries no command.
#define DBG_X(SYM, bit, BANK, parentBit, tag, field, cmdIdent, ...) #field,
static constexpr const char* kDbgCmdRetName[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };
#undef DBG_X
#define DBG_X(SYM, bit, BANK, parentBit, tag, field, cmdIdent, ...) #cmdIdent,
static constexpr const char* kDbgCmdIdent[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };
#undef DBG_X

// Per-bitless-sub columns, indexed by DbgSubIdx (parallel to kDbgSubRuntimePtr
// / kDbgSubParentIdx). kDbgSubSettingPtr is the persistent gSettings bool.
#define DBG_S(SYM, subField, settingsField, PARENT_SYM, cmdIdent, ...) &gSettings.settingsField,
static constexpr bool* kDbgSubSettingPtr[DBG_SUBBOOL_COUNT] = { DBG_SUBBOOL_LIST(DBG_S) };
#undef DBG_S
#define DBG_S(SYM, subField, settingsField, PARENT_SYM, cmdIdent, ...) #settingsField,
static constexpr const char* kDbgSubRetName[DBG_SUBBOOL_COUNT] = { DBG_SUBBOOL_LIST(DBG_S) };
#undef DBG_S
#define DBG_S(SYM, subField, settingsField, PARENT_SYM, cmdIdent, ...) #cmdIdent,
static constexpr const char* kDbgSubCmdIdent[DBG_SUBBOOL_COUNT] = { DBG_SUBBOOL_LIST(DBG_S) };
#undef DBG_S

// Non-mechanical per-command side effects, keyed by DbgFlagIdx. Fires on the
// SAME paths as the pre-C3 handlers: after the mask bit is toggled (temp) or
// after the bit + gSettings are written (persist). Unlisted rows do nothing.
static void dbgApplyHook(DbgFlagIdx idx, bool modeTemp, int v, bool* settingPtr) {
  (void)modeTemp; (void)v; (void)settingPtr;
  switch (idx) {
    case DBG_HTTPS: {
      // Drive ESP-IDF TLS/HTTPS tag verbosity. Skipped during the CLI
      // validation pass (dbgFlagCmd short-circuits before the hook, so
      // gCLIValidateOnly is false here — guarded anyway to match pre-C3).
      extern bool gCLIValidateOnly;
      if (!gCLIValidateOnly) applyHttpsLogLevels(isDebugFlagSet(DEBUG_HTTPS));
      break;
    }
    case DBG_LLM:
    case DBG_LLM_LOAD:
    case DBG_LLM_TOKENIZER:
    case DBG_LLM_FORWARD:
    case DBG_LLM_GENERATE:
    case DBG_LLM_MEMORY:
      // Persistent path only — temp LLM sub toggles never raise the parent
      // (SETTINGS family; the DEBUG_LLM_*F macros pass the bare sub-bit).
      if (!modeTemp) dbgRecomputeParent(DBG_LLM);
      break;
    case DBG_SR:
      // Parent: sync the legacy gSrDebugLevel; NO recompute (owns its bit).
#if ENABLE_ESP_SR
      if (modeTemp) { bool prev = *settingPtr; *settingPtr = (v != 0); srSyncDebugLevel(); *settingPtr = prev; }
      else          { srSyncDebugLevel(); }
#endif
      break;
    case DBG_SR_WAKE:
    case DBG_SR_COMMAND:
    case DBG_SR_AFE:
    case DBG_SR_LIFECYCLE:
    case DBG_SR_TUNING:
      // SR subs: level-sync (flip/restore on temp), then recompute DBG_SR on
      // BOTH paths (the SR output gates test DEBUG_SR alongside the sub-bit).
#if ENABLE_ESP_SR
      if (modeTemp) { bool prev = *settingPtr; *settingPtr = (v != 0); srSyncDebugLevel(); *settingPtr = prev; }
#endif
      dbgRecomputeParent(DBG_SR);
#if ENABLE_ESP_SR
      if (!modeTemp) srSyncDebugLevel();
#endif
      break;
    case DBG_BLUETOOTH_CORE:
    case DBG_BLUETOOTH_GATT:
    case DBG_BLUETOOTH_DATA:
      // Recompute the Bluetooth parent on BOTH paths (SETTINGS_BITS family).
      dbgRecomputeParent(DBG_BLUETOOTH);
      break;
    default:
      break;
  }
}

// Shared dispatcher for every flag-backed debug command (parent or bit-sub).
// Reproduces cmd_debugsubflag_impl exactly, plus the strict 0|1 contract and
// the dbgApplyHook side effect.
static const char* dbgFlagCmd(DbgFlagIdx idx, const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[96];
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (v != 0 && v != 1) {
    snprintf(buf, sizeof(buf), "Error: invalid arguments — Usage: %s <0|1> [temp|runtime]", kDbgCmdIdent[idx]);
    return buf;
  }
  const DebugFlagMask flagBit = kDbgMask[idx];
  bool* settingPtr = kDbgSettingPtr[idx];
  const char* name = kDbgCmdRetName[idx];
  if (modeTemp) {
    if (v) setDebugFlag(flagBit); else clearDebugFlag(flagBit);
    dbgApplyHook(idx, true, v, settingPtr);
    snprintf(buf, sizeof(buf), "%s %s (runtime only)", name, v ? "enabled" : "disabled");
    return buf;
  }
  setDebugSetting(*settingPtr, (bool)(v != 0));
  if (v) setDebugFlag(flagBit); else clearDebugFlag(flagBit);
  dbgApplyHook(idx, false, v, settingPtr);
  snprintf(buf, sizeof(buf), "%s %s (persistent)", name, *settingPtr ? "enabled" : "disabled");
  return buf;
}

// Shared dispatcher for the 40 bitless subs. The runtime layer
// (gDebugSubFlags.<subField>) is written UNCONDITIONALLY on both paths — the
// invariant that lets `temp` diverge runtime from persistent — while gSettings
// is written on persist only. The family parent recomputes on both paths.
static const char* dbgSubCmd(DbgSubIdx sidx, const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[96];
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  int v = ca.argInt(0, 0);
  if (v != 0 && v != 1) {
    snprintf(buf, sizeof(buf), "Error: invalid arguments — Usage: %s <0|1> [temp|runtime]", kDbgSubCmdIdent[sidx]);
    return buf;
  }
  *kDbgSubRuntimePtr[sidx] = (v != 0);
  if (!modeTemp) setDebugSetting(*kDbgSubSettingPtr[sidx], (bool)(v != 0));
  dbgRecomputeParent(kDbgSubParentIdx[sidx]);
  if (modeTemp) {
    snprintf(buf, sizeof(buf), "%s %s (runtime only)", kDbgSubRetName[sidx], v ? "enabled" : "disabled");
    return buf;
  }
  snprintf(buf, sizeof(buf), "%s %s (persistent)", kDbgSubRetName[sidx], *kDbgSubSettingPtr[sidx] ? "enabled" : "disabled");
  return buf;
}

// --- Generated flag thunks: cmd_<cmdIdent> -> dbgFlagCmd(DBG_<SYM>) ----------
// The ALWAYS control row (cmdIdent == DBG_NO_CMD) emits no thunk. cmdIdent is
// re-passed through the skip dispatch (benign token, never an Arduino macro);
// the index is pasted as DBG_##SYM at the first macro level (C1 paste rule) and
// forwarded as an already-resolved enumerator token.
#define DBG_THUNK_EMIT(IDXTOK, cmdIdent) \
  const char* cmd_##cmdIdent(const String& a) { return dbgFlagCmd(IDXTOK, a); }
#define DBG_THUNK_0(IDXTOK, cmdIdent) DBG_THUNK_EMIT(IDXTOK, cmdIdent)
#define DBG_THUNK_1(IDXTOK, cmdIdent) /* control row — no command */
#define DBG_X(SYM, bit, BANK, parentBit, tag, field, cmdIdent, ...) \
  DBG_PP_CAT(DBG_THUNK_, DBG_CMD_IS_NONE(cmdIdent))(DBG_##SYM, cmdIdent)
DBG_FLAG_LIST(DBG_X)
#undef DBG_X
#undef DBG_THUNK_1
#undef DBG_THUNK_0
#undef DBG_THUNK_EMIT

// --- Generated bitless-sub thunks: cmd_<cmdIdent> -> dbgSubCmd(DBG_SUB_<SYM>) -
#define DBG_S(SYM, subField, settingsField, PARENT_SYM, cmdIdent, ...) \
  const char* cmd_##cmdIdent(const String& a) { return dbgSubCmd(DBG_SUB_##SYM, a); }
DBG_SUBBOOL_LIST(DBG_S)
#undef DBG_S

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
  // Table walk over the generated rows in System_DebugFlags.h. The DEBUG_*F
  // macros enqueue `PARENT | SUB`, so pass 1 tests every sub-flag row
  // (parentBit != 255) and pass 2 every root row — most-specific wins, and a
  // sub's tag always beats its parent's. Rows with an empty tag are control
  // bits (DEBUG_ALWAYS): they ride along with the producer's flag and must
  // never shadow it, so both passes skip them.
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {  // pass 1: sub-flags
    if (kDbgParentBit[i] == 255) continue;
    if (kDbgTag[i][0] == '\0') continue;
    if ((flag & kDbgMask[i]) != (DebugFlagMask)0) return kDbgTag[i];
  }
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {  // pass 2: root flags
    if (kDbgParentBit[i] != 255) continue;
    if (kDbgTag[i][0] == '\0') continue;
    if ((flag & kDbgMask[i]) != (DebugFlagMask)0) return kDbgTag[i];
  }
  return "UNKNOWN";
}

// ============================================================================
// System Logging Commands
// ============================================================================

// Helper: Generate timestamped filename for system log
static String generateSystemLogFilename() {
  String filename = CAPTURE_DIR_SYSTEM "/system-";
  
  // Try to get epoch time
  time_t now = time(nullptr);
  if (Clock::isValidEpoch(now)) {  // was a hand-rolled year-2001 threshold
    struct tm tmLocal;
    localtime_r(&now, &tmLocal);
    char timestamp[32];
    // Format: YYYY-MM-DDTHH-MM-SS
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H-%M-%S", &tmLocal);
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

// Parse a flags= mask string into a DebugFlagMask. Accepts either a single hex
// string (up to 64 digits) or colon-separated 64-bit words high-to-low.
bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out) {
  auto stripHexPrefix = [](const String& s) -> String {
    return (s.startsWith("0x") || s.startsWith("0X")) ? s.substring(2) : s;
  };
  auto parseHex64 = [](const String& s) -> uint64_t {
    return strtoull(s.c_str(), nullptr, 16);
  };
  uint64_t w[4] = {0, 0, 0, 0};  // w[0] = bits 0-63 ... w[3] = bits 192-255
  if (flagsStr.indexOf(':') >= 0) {
    String rest = flagsStr;
    for (int wi = 0; wi < 4 && rest.length() > 0; ++wi) {
      int colon = rest.lastIndexOf(':');
      w[wi] = parseHex64(stripHexPrefix(colon >= 0 ? rest.substring(colon + 1) : rest));
      if (colon < 0) break;
      rest = rest.substring(0, colon);
    }
  } else {
    String hex = stripHexPrefix(flagsStr);
    if (hex.length() == 0) return false;
    for (int wi = 0; wi < 4 && hex.length() > 0; ++wi) {
      unsigned cut = hex.length() > 16 ? hex.length() - 16 : 0;
      w[wi] = parseHex64(hex.substring(cut));
      hex = hex.substring(0, cut);
    }
  }
  out = ((DebugFlagMask)w[3] << 192) | ((DebugFlagMask)w[2] << 128) |
        ((DebugFlagMask)w[1] << 64)  | (DebugFlagMask)w[0];
  return true;
}

String formatSystemLogFlags(DebugFlagMask mask) {
  char buf[80];
  snprintf(buf, sizeof(buf), "0x%016llX:%016llX:%016llX:%016llX",
           (unsigned long long)(uint64_t)(mask >> 192),
           (unsigned long long)(uint64_t)(mask >> 128),
           (unsigned long long)(uint64_t)(mask >> 64),
           (unsigned long long)(uint64_t)mask);
  return String(buf);
}

bool systemLogApplyPersistedFlags() {
  if (gSettings.systemLogFlags.length() == 0) return false;
  DebugFlagMask restored = (DebugFlagMask)0;
  if (!parseSystemLogFlags(gSettings.systemLogFlags, restored)) return false;
  gDebugFlags = restored;
  return true;
}

const char* cmd_log(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CommandArgs ca(argsInput);
  if (ca.count() == 0) {
    return "Error: invalid arguments — Usage: log <start|stop|status|autostart>\n"
           "  start [\"filepath\"] [flags=0x...] [tags=0|1]: Begin system logging\n"
           "    filepath: Log file path (auto-generated if omitted)\n"
           "    flags: Debug flag mask, up to 64 hex digits (bit map in System_Debug.h),\n"
           "           or colon-separated 64-bit words high to low: 0xW3:0xW2:0xW1:0xW0\n"
           "    tags: Enable category tags (default: 1)\n"
           "  stop: Stop system logging\n"
           "  status: Show current logging status\n"
           "  autostart: Toggle auto-start system logging on boot\n"
           "Examples:\n"
           "  log start\n"
           "  log start /logging_captures/system/debug.log\n"
           "  log start flags=0x15 tags=1\n"
           "  log start /logging_captures/system/debug.log flags=0x3F00000000 tags=0\n"
           "  log autostart";
  }
  String subCmd = ca.arg(0);
  subCmd.toLowerCase();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  // Handle 'status' subcommand
  if (subCmd == "status") {
    if (gSystemLogRunning && (gOutputFlags & MSG_ROUTE_FILE)) {
      unsigned long ageSeconds = (millis() - gSystemLogLastWrite) / 1000;
      snprintf(gDebugBuffer, 1024,
               "System logging ACTIVE\n"
               "  File: %s\n"
               "  Last write: %lus ago\n"
               "  Output flags: 0x%02X\n"
               "  Auto-start: %s",
               gSystemLogPath.c_str(), ageSeconds, (unsigned)gOutputFlags,
               gSettings.systemLogAutoStart ? "ON" : "OFF");
    } else if (gSystemLogRunning) {
      snprintf(gDebugBuffer, 1024,
               "System logging CONFIGURED but MSG_ROUTE_FILE flag not set\n"
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
    if (!gSystemLogRunning) {
      return "Error: System logging is not running";
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
    
    gSystemLogRunning = false;
    gOutputFlags &= ~MSG_ROUTE_FILE;
    String msg = "System logging stopped. Log saved to: " + gSystemLogPath;
    gSystemLogPath = "";
    snprintf(gDebugBuffer, 1024, "%s", msg.c_str());
    return gDebugBuffer;
  }
  
  // Handle 'start' subcommand
  if (subCmd == "start") {
    // Master switch - see the matching gate in cmd_sensorlog.
    if (!gSettings.systemLogEnabled) {
      return "Error: system logging is disabled - run 'systemlogenabled 1' first";
    }
    if (gSystemLogRunning) {
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
    
    // Parse arguments: log start [filepath] [flags=0x...] [tags=0|1]
    // Flag arg accepts up to 256 bits as either:
    //   - a single hex string up to 64 digits (what the web logging page sends)
    //   - colon-separated 64-bit words, highest first: 0xW3:0xW2:0xW1:0xW0
    //     (fewer words fill the low end)
    String filepath;
    bool flagsSet = false;
    DebugFlagMask debugFlags = (DebugFlagMask)0;
    int categoryTags = -1; // Sentinel: don't change if not specified

    // Find filepath (first non-key=value arg after "start")
    bool hasFilepath = false;
    for (int i = 1; i < ca.count(); i++) {
      String token = ca.arg(i);
      if (token.startsWith("flags=")) {
        if (!parseSystemLogFlags(token.substring(6), debugFlags)) {
          return "Error: invalid flags= mask";
        }
        flagsSet = true;
      } else if (token.startsWith("tags=")) {
        String tagsStr = token.substring(5);
        categoryTags = tagsStr.toInt();
      } else if (token.length() > 0 && !hasFilepath) {
        // The log path must be a quoted token (uniform quoted-path rule); a bare
        // token here is almost certainly an unquoted path.
        if (!ca.argWasQuoted(i))
          return "Error: log path must be in quotes, e.g. log start \"/logging_captures/system/sys.txt\"";
        filepath = token;
        hasFilepath = true;
      }
    }

    if (!hasFilepath) {
      // Use persisted path if set, otherwise auto-generate
      filepath = (gSettings.systemLogPath.length() > 0) ? gSettings.systemLogPath : generateSystemLogFilename();
    }
    
    if (filepath.length() == 0 || filepath.charAt(0) != '/') {
      return "Error: Filepath must start with / (e.g., /logging_captures/system/system.log)";
    }
    
    // Apply debug flags if specified; otherwise restore last-used system-log mask
    if (flagsSet) {
      gDebugFlags = debugFlags;
      setSetting(gSettings.systemLogFlags, formatSystemLogFlags(debugFlags));
      char flagsMsg[160];
      snprintf(flagsMsg, sizeof(flagsMsg),
               "Debug flags set to: 0x%016llX:%016llX:%016llX:%016llX",
               (unsigned long long)(uint64_t)(gDebugFlags >> 192),
               (unsigned long long)(uint64_t)(gDebugFlags >> 128),
               (unsigned long long)(uint64_t)(gDebugFlags >> 64),
               (unsigned long long)(uint64_t)gDebugFlags);
      broadcastOutput(flagsMsg);
    } else if (gSettings.systemLogFlags.length() > 0) {
      DebugFlagMask restored = (DebugFlagMask)0;
      if (parseSystemLogFlags(gSettings.systemLogFlags, restored)) {
        gDebugFlags = restored;
      }
    }
    
    // Apply category tags setting (arg overrides, otherwise use persisted setting)
    if (categoryTags >= 0) {
      gSystemLogCategoryTags = (categoryTags != 0);
      setSetting(gSettings.systemLogCategoryTags, gSystemLogCategoryTags);
    } else {
      gSystemLogCategoryTags = gSettings.systemLogCategoryTags;
    }
    
    // Ensure directory exists. RECURSIVE (matching sensorlog's setup path):
    // mkdir is single-level, so a nested target like
    // /logging_captures/system/foo.log — or an SD-overflow mirror at
    // /sd/logging_captures/system/ — needs each level created in turn. This
    // used to be one mkdir, which worked only while the parent happened to
    // exist and would have failed outright once the overflow latch fired.
    int lastSlash = filepath.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = filepath.substring(0, lastSlash);
      if (!VFS::existsGuarded(dir, VFS::systemAuth("debug.log_setup_mkdir"))) {
        fsLock("log.mkdir");
        for (int i = 1; i <= (int)dir.length(); i++) {
          if (i == (int)dir.length() || dir.charAt(i) == '/') {
            String parent = dir.substring(0, i);
            if (parent.length() > 0 && !VFS::existsGuarded(parent, VFS::systemAuth("debug.log_setup_mkdir"))) {
              VFS::mkdirGuarded(parent, VFS::systemAuth("debug.log_setup_mkdir"));
            }
          }
        }
        fsUnlock();
        if (!VFS::existsGuarded(dir, VFS::systemAuth("debug.log_setup_mkdir"))) {
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
    gSystemLogRunning = true;
    gSystemLogLastWrite = millis();
    gOutputFlags |= MSG_ROUTE_FILE;
    setSetting(gSettings.systemLogPath, filepath);

    snprintf(gDebugBuffer, 1024, "System logging started\n  File: %s", filepath.c_str());
    broadcastOutput(gDebugBuffer);
    return gDebugBuffer;
  }
  
  // Handle 'autostart' subcommand
  if (subCmd == "autostart") {
    String mode = ca.arg(1);
    mode.toLowerCase();
    bool newValue;
    if (mode.length() == 0) {
      newValue = !gSettings.systemLogAutoStart;  // bare = toggle
    } else if (mode == "on" || mode == "1" || mode == "true" || mode == "enable") {
      newValue = true;
    } else if (mode == "off" || mode == "0" || mode == "false" || mode == "disable") {
      newValue = false;
    } else {
      return "Error: invalid arguments — Usage: log autostart [on|off]  (bare = toggle)";
    }
    setSetting(gSettings.systemLogAutoStart, newValue);
    snprintf(gDebugBuffer, 1024, "System log auto-start %s", newValue ? "ENABLED" : "DISABLED");
    return gDebugBuffer;
  }
  
  return "Error: Unknown subcommand. Use: start, stop, status, or autostart";
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

// ============================================================================
// Debug Command Registry
// ============================================================================

// Implemented in System_Notifications.cpp (owns the pipeline counters).
extern const char* cmd_notifstats(const String& argsInput);

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry debugCommands[] = {
  { "debughttp", "Debug HTTP requests.", true, cmd_debughttp, "Usage: debughttp <0|1> [temp|runtime]" },
  { "debughttps", "Debug HTTPS/TLS handshake + connection errors (ESP-IDF logs).", true, cmd_debughttps, "Usage: debughttps <0|1> [temp|runtime]" },
  { "debugsse", "Debug Server-Sent Events.", true, cmd_debugsse, "Usage: debugsse <0|1> [temp|runtime]" },
  { "debugcli", "Debug CLI processing.", true, cmd_debugcli, "Usage: debugcli <0|1> [temp|runtime]" },
  { "debugauth", "Debug authentication (parent flag).", true, cmd_debugauth, "Usage: debugauth <0|1> [temp|runtime]" },
  { "debugespnow", "Debug ESP-NOW core messages (alias of debugespnowcore).", true, cmd_debugespnowcore, "Usage: debugespnow <0|1> [temp|runtime]" },
  { "debugbluetooth", "Debug Bluetooth (parent flag).", true, cmd_debugbluetooth, "Usage: debugbluetooth <0|1> [temp|runtime]" },
  { "debugbluetoothcore", "Debug Bluetooth core lifecycle.", true, cmd_debugbluetoothcore, "Usage: debugbluetoothcore <0|1> [temp|runtime]" },
  { "debugbluetoothgatt", "Debug Bluetooth GATT operations.", true, cmd_debugbluetoothgatt, "Usage: debugbluetoothgatt <0|1> [temp|runtime]" },
  { "debugbluetoothdata", "Debug Bluetooth command/data path.", true, cmd_debugbluetoothdata, "Usage: debugbluetoothdata <0|1> [temp|runtime]" },
  { "debuguart", "Debug UART host link (parent flag).", true, cmd_debuguart, "Usage: debuguart <0|1> [temp|runtime]" },
  { "debuguartlifecycle", "Debug UART link/session lifecycle.", true, cmd_debuguartlifecycle, "Usage: debuguartlifecycle <0|1> [temp|runtime]" },
  { "debuguartcontrol", "Debug UART CM5/liveaudio control intrinsics.", true, cmd_debuguartcontrol, "Usage: debuguartcontrol <0|1> [temp|runtime]" },
  { "debugcamera",          "Debug camera (parent flag).",                              true, cmd_debugcamera,          "Usage: debugcamera <0|1> [temp|runtime]" },
  { "debugcameralifecycle", "Debug camera init/stop/PWDN-RESET/GPIO state.",            true, cmd_debugcameralifecycle, "Usage: debugcameralifecycle <0|1> [temp|runtime]" },
  { "debugcameracapture",   "Debug captureFrame, JPEG validation, fb buffer, recovery.",true, cmd_debugcameracapture,   "Usage: debugcameracapture <0|1> [temp|runtime]" },
  { "debugcamerasettings",  "Debug runtime camera resolution/quality changes.",         true, cmd_debugcamerasettings,  "Usage: debugcamerasettings <0|1> [temp|runtime]" },
  { "debugcameravideo",     "Debug video recording start/finalize, frame writing.",     true, cmd_debugcameravideo,     "Usage: debugcameravideo <0|1> [temp|runtime]" },
  { "debugdisplay",         "Debug OLED init/probe/boot-animation/mode-transitions.",   true, cmd_debugdisplay,         "Usage: debugdisplay <0|1> [temp|runtime]" },
  { "debugnotifications",   "Debug notification pipeline: ring lag/skips, stale/cooldown drops, SSE saturation.", true, cmd_debugnotifications, "Usage: debugnotifications <0|1> [temp|runtime]" },
  { "notifstats",           "Notification pipeline counters: loss, suppression, ring lag, SSE drops.", true, cmd_notifstats, "Usage: notifstats [reset]\n  (bare): print pipeline counters\n  reset: zero them" },
  { "debugmicrophone", "Debug microphone operations.", true, cmd_debugmicrophone, "Usage: debugmicrophone <0|1> [temp|runtime]" },
  { "debuggps", "Debug GPS sensor (PA1010D).", true, cmd_debuggps, "Usage: debuggps <0|1> [temp|runtime]" },
  { "debugrtc", "Debug RTC sensor (DS3231).", true, cmd_debugrtc, "Usage: debugrtc <0|1> [temp|runtime]" },
  { "debugimu", "Debug IMU sensor (BNO055).", true, cmd_debugimu, "Usage: debugimu <0|1> [temp|runtime]" },
  { "debugthermal", "Debug thermal sensor (MLX90640).", true, cmd_debugthermal, "Usage: debugthermal <0|1> [temp|runtime]" },
  { "debugtof", "Debug ToF sensor (VL53L4CX).", true, cmd_debugtof, "Usage: debugtof <0|1> [temp|runtime]" },
  { "debuginput",      "Debug input abstraction layer (HAL_Input + OLED dispatch).", true, cmd_debuginput,      "Usage: debuginput <0|1> [temp|runtime]" },
  { "debuganoencoder", "Debug ANO rotary encoder driver internals.",                 true, cmd_debuganoencoder, "Usage: debuganoencoder <0|1> [temp|runtime]" },
  { "debugapds", "Debug APDS sensor (APDS9960).", true, cmd_debugapds, "Usage: debugapds <0|1> [temp|runtime]" },
  { "debugpresence", "Debug presence sensor (STHS34PF80).", true, cmd_debugpresence, "Usage: debugpresence <0|1> [temp|runtime]" },
  // Per-sensor sub-flag setters (Lifecycle / Polling / Values)
  { "debugthermallifecycle",  "Debug thermal init/connect/recovery.",         true, cmd_debugthermallifecycle,  "Usage: debugthermallifecycle <0|1> [temp|runtime]" },
  { "debugthermalpolling",    "Debug thermal poll cadence/FPS/capture.",      true, cmd_debugthermalpolling,    "Usage: debugthermalpolling <0|1> [temp|runtime]" },
  { "debugthermalvalues",     "Debug thermal value updates/interpolation.",   true, cmd_debugthermalvalues,     "Usage: debugthermalvalues <0|1> [temp|runtime]" },
  { "debugtoflifecycle",      "Debug ToF init/connect/recovery.",             true, cmd_debugtoflifecycle,      "Usage: debugtoflifecycle <0|1> [temp|runtime]" },
  { "debugtofpolling",        "Debug ToF poll cadence/capture.",              true, cmd_debugtofpolling,        "Usage: debugtofpolling <0|1> [temp|runtime]" },
  { "debugtofvalues",         "Debug ToF range/object detection values.",     true, cmd_debugtofvalues,         "Usage: debugtofvalues <0|1> [temp|runtime]" },
  { "debuginputlifecycle",    "Debug input abstraction layer lifecycle.",     true, cmd_debuginputlifecycle,  "Usage: debuginputlifecycle <0|1> [temp|runtime]" },
  { "debuginputpolling",      "Debug input abstraction layer poll/dispatch.", true, cmd_debuginputpolling,    "Usage: debuginputpolling <0|1> [temp|runtime]" },
  { "debuginputvalues",       "Debug input abstraction layer event values.",  true, cmd_debuginputvalues,     "Usage: debuginputvalues <0|1> [temp|runtime]" },
  { "debuganoencoderlifecycle", "Debug ANO encoder init/connect/recovery.",   true, cmd_debuganoencoderlifecycle, "Usage: debuganoencoderlifecycle <0|1> [temp|runtime]" },
  { "debuganoencoderpolling",   "Debug ANO encoder poll/encoder reads.",      true, cmd_debuganoencoderpolling,   "Usage: debuganoencoderpolling <0|1> [temp|runtime]" },
  { "debuganoencodervalues",    "Debug ANO encoder rotation/button events.",  true, cmd_debuganoencodervalues,    "Usage: debuganoencodervalues <0|1> [temp|runtime]" },
  { "debugimulifecycle",      "Debug IMU init/connect/recovery.",             true, cmd_debugimulifecycle,      "Usage: debugimulifecycle <0|1> [temp|runtime]" },
  { "debugimupolling",        "Debug IMU poll cadence.",                      true, cmd_debugimupolling,        "Usage: debugimupolling <0|1> [temp|runtime]" },
  { "debugimuvalues",         "Debug IMU orientation/acceleration values.",   true, cmd_debugimuvalues,         "Usage: debugimuvalues <0|1> [temp|runtime]" },
  { "debugapdslifecycle",     "Debug APDS init/connect/recovery.",            true, cmd_debugapdslifecycle,     "Usage: debugapdslifecycle <0|1> [temp|runtime]" },
  { "debugapdspolling",       "Debug APDS poll cadence.",                     true, cmd_debugapdspolling,       "Usage: debugapdspolling <0|1> [temp|runtime]" },
  { "debugapdsvalues",        "Debug APDS color/proximity/gesture values.",   true, cmd_debugapdsvalues,        "Usage: debugapdsvalues <0|1> [temp|runtime]" },
  { "debuggpslifecycle",      "Debug GPS init/connect/recovery.",             true, cmd_debuggpslifecycle,      "Usage: debuggpslifecycle <0|1> [temp|runtime]" },
  { "debuggpspolling",        "Debug GPS poll cadence.",                      true, cmd_debuggpspolling,        "Usage: debuggpspolling <0|1> [temp|runtime]" },
  { "debuggpsvalues",         "Debug GPS NMEA/fix/coordinate values.",        true, cmd_debuggpsvalues,         "Usage: debuggpsvalues <0|1> [temp|runtime]" },
  { "debugrtclifecycle",      "Debug RTC init/connect/recovery.",             true, cmd_debugrtclifecycle,      "Usage: debugrtclifecycle <0|1> [temp|runtime]" },
  { "debugrtcpolling",        "Debug RTC poll cadence.",                      true, cmd_debugrtcpolling,        "Usage: debugrtcpolling <0|1> [temp|runtime]" },
  { "debugrtcvalues",         "Debug RTC time-read values.",                  true, cmd_debugrtcvalues,         "Usage: debugrtcvalues <0|1> [temp|runtime]" },
  { "debugfmradiolifecycle",  "Debug FM radio init/tune/recovery.",           true, cmd_debugfmradiolifecycle,  "Usage: debugfmradiolifecycle <0|1> [temp|runtime]" },
  { "debugfmradiopolling",    "Debug FM radio poll cadence.",                 true, cmd_debugfmradiopolling,    "Usage: debugfmradiopolling <0|1> [temp|runtime]" },
  { "debugfmradiovalues",     "Debug FM radio RDS/RSSI/state values.",        true, cmd_debugfmradiovalues,     "Usage: debugfmradiovalues <0|1> [temp|runtime]" },
  { "debugmiclifecycle",      "Debug microphone init/start/stop.",            true, cmd_debugmiclifecycle,      "Usage: debugmiclifecycle <0|1> [temp|runtime]" },
  { "debugmicpolling",        "Debug microphone capture cadence.",            true, cmd_debugmicpolling,        "Usage: debugmicpolling <0|1> [temp|runtime]" },
  { "debugmicvalues",         "Debug microphone level/sample values.",        true, cmd_debugmicvalues,         "Usage: debugmicvalues <0|1> [temp|runtime]" },
  { "debugpresencelifecycle", "Debug presence sensor init/connect/recovery.", true, cmd_debugpresencelifecycle, "Usage: debugpresencelifecycle <0|1> [temp|runtime]" },
  { "debugpresencepolling",   "Debug presence sensor poll cadence.",          true, cmd_debugpresencepolling,   "Usage: debugpresencepolling <0|1> [temp|runtime]" },
  { "debugpresencevalues",    "Debug presence detection values.",             true, cmd_debugpresencevalues,    "Usage: debugpresencevalues <0|1> [temp|runtime]" },
  { "debugmaps", "Debug maps (parent flag).", true, cmd_debugmaps, "Usage: debugmaps <0|1> [temp|runtime]" },
  { "debugmapsloading", "Debug map file loading and tile directory.", true, cmd_debugmapsloading, "Usage: debugmapsloading <0|1> [temp|runtime]" },
  { "debugmapsrendering", "Debug map render pipeline and feature drawing.", true, cmd_debugmapsrendering, "Usage: debugmapsrendering <0|1> [temp|runtime]" },
  { "debugmapsperf", "Debug map performance timing (render ms, tile I/O, cache, FPS).", true, cmd_debugmapsperf, "Usage: debugmapsperf <0|1> [temp|runtime]" },
#if ENABLE_LLM_SOURCE_ONBOARD
  { "debugllm", "Debug on-device LLM (parent flag).", true, cmd_debugllm, "Usage: debugllm <0|1> [temp|runtime]" },
  { "debugllmload", "Debug LLM checkpoint load and validation.", true, cmd_debugllmload, "Usage: debugllmload <0|1> [temp|runtime]" },
  { "debugllmtokenizer", "Debug LLM tokenizer / BPE.", true, cmd_debugllmtokenizer, "Usage: debugllmtokenizer <0|1> [temp|runtime]" },
  { "debugllmforward", "Debug LLM transformer forward (verbose).", true, cmd_debugllmforward, "Usage: debugllmforward <0|1> [temp|runtime]" },
  { "debugllmgenerate", "Debug LLM generation loop and sampling.", true, cmd_debugllmgenerate, "Usage: debugllmgenerate <0|1> [temp|runtime]" },
  { "debugllmmemory", "Debug LLM PSRAM budget and context cap.", true, cmd_debugllmmemory, "Usage: debugllmmemory <0|1> [temp|runtime]" },
#endif
  { "debugi2c",          "Debug I2C bus (parent flag).",                                true, cmd_debugi2c,          "Usage: debugi2c <0|1> [temp|runtime]" },
  { "debugi2cbus",       "Debug I2C bus lifecycle, polling pause/resume, status bumps.", true, cmd_debugi2cbus,       "Usage: debugi2cbus <0|1> [temp|runtime]" },
  { "debugi2cdiscovery", "Debug I2C device probing, registry, scan results.",            true, cmd_debugi2cdiscovery, "Usage: debugi2cdiscovery <0|1> [temp|runtime]" },
  { "debugi2cautostart", "Debug I2C sensor auto-start orchestration + init results.",    true, cmd_debugi2cautostart, "Usage: debugi2cautostart <0|1> [temp|runtime]" },
  { "debugwifi", "Debug WiFi operations.", true, cmd_debugwifi, "Usage: debugwifi <0|1> [temp|runtime]" },
  { "debugstorage", "Debug storage operations.", true, cmd_debugstorage, "Usage: debugstorage <0|1> [temp|runtime]" },
  { "debugperformance", "Debug performance metrics.", true, cmd_debugperformance, "Usage: debugperformance <0|1> [temp|runtime]" },
  { "debugdatetime",        "Debug NTP/date-time (parent flag).",        true, cmd_debugdatetime, "Usage: debugdatetime <0|1> [temp|runtime]" },
  { "debugdatetimesync",    "Debug NTP sync loop (DNS, wait, result).",  true, cmd_debugdatetimesync,    "Usage: debugdatetimesync <0|1> [temp|runtime]" },
  { "debugdatetimesetup",   "Debug NTP setup / configTime calls.",       true, cmd_debugdatetimesetup,   "Usage: debugdatetimesetup <0|1> [temp|runtime]" },
  { "debugdatetimeanchor",  "Debug NTP boot anchor write/read.",         true, cmd_debugdatetimeanchor,  "Usage: debugdatetimeanchor <0|1> [temp|runtime]" },
  { "debugdatetimeresolve", "Debug NTP timestamp resolution for users.", true, cmd_debugdatetimeresolve, "Usage: debugdatetimeresolve <0|1> [temp|runtime]" },
  { "debugverbose", "Global debug verbosity override (forces all debug + loglevel=DEBUG).", true, cmd_debugverbose, "Usage: debugverbose <0|1>" },
  { "debugbuffer", "Show debug ring buffer status.", true, cmd_debugbuffer },
  { "debugflags", "Show the live debug mask (4 hex words) and the flags it has set.", true, cmd_debugflags, "Usage: debugflags" },
  { "debugcommandflow", "Debug command flow.", true, cmd_debugcommandflow, "Usage: debugcommandflow <0|1> [temp|runtime]" },
  { "debugusers", "Debug user management.", true, cmd_debugusers, "Usage: debugusers <0|1> [temp|runtime]" },
  { "debugsystem", "Debug system/boot operations.", true, cmd_debugsystem, "Usage: debugsystem <0|1> [temp|runtime]" },
  { "debugespnowstream", "Debug ESP-NOW streaming output.", true, cmd_debugespnowstream, "Usage: debugespnowstream <0|1> [temp|runtime]" },
  { "debugespnowcore", "Debug ESP-NOW core operations.", true, cmd_debugespnowcore, "Usage: debugespnowcore <0|1> [temp|runtime]" },
  { "debugespnowrouter", "Debug ESP-NOW router operations.", true, cmd_debugespnowrouter, "Usage: debugespnowrouter <0|1> [temp|runtime]" },
  { "debugespnowmesh", "Debug ESP-NOW mesh operations.", true, cmd_debugespnowmesh, "Usage: debugespnowmesh <0|1> [temp|runtime]" },
  { "debugespnowtopo", "Debug ESP-NOW topology discovery.", true, cmd_debugespnowtopo, "Usage: debugespnowtopo <0|1> [temp|runtime]" },
  { "debugespnowmetadata", "Debug ESP-NOW metadata exchange (REQ/RESP/PUSH).", true, cmd_debugespnowmetadata, "Usage: debugespnowmetadata <0|1> [temp|runtime]" },
  { "debugautoscheduler", "Debug automations scheduler.", true, cmd_debugautoscheduler, "Usage: debugautoscheduler <0|1> [temp|runtime]" },
  { "debugautoexec", "Debug automations execution.", true, cmd_debugautoexec, "Usage: debugautoexec <0|1> [temp|runtime]" },
  { "debugautocondition", "Debug automations conditions.", true, cmd_debugautocondition, "Usage: debugautocondition <0|1> [temp|runtime]" },
  { "debugautotiming", "Debug automations timing.", true, cmd_debugautotiming, "Usage: debugautotiming <0|1> [temp|runtime]" },
  { "debugmemory",         "Debug memory (parent flag).",                              true, cmd_debugmemory,         "Usage: debugmemory <0|1> [temp|runtime]" },
  { "loglink",             "Route ESP-IDF logs through the unified output queue (stops UART interleave).", true, cmd_loglink, "Usage: loglink [<0|1|on|off>]  (bare = show status)" },
  { "debugmemoryheap",     "Debug per-task heap (free/min/largest), DRAM low watermark.", true, cmd_debugmemoryheap,    "Usage: debugmemoryheap <0|1> [temp|runtime]" },
  { "debugmemorystack",    "Debug per-task stack watermarks + peak reports.",          true, cmd_debugmemorystack,    "Usage: debugmemorystack <0|1> [temp|runtime]" },
  { "debugmemorybuffers",  "Debug response/cookie buffer sizing diagnostics.",         true, cmd_debugmemorybuffers,  "Usage: debugmemorybuffers <0|1> [temp|runtime]" },
  { "debugmqtt",           "Debug MQTT (parent flag).",                                true, cmd_debugmqtt,           "Usage: debugmqtt <0|1> [temp|runtime]" },
  { "debugmqttconnection", "Debug MQTT connect/disconnect/TLS/init.",                  true, cmd_debugmqttconnection, "Usage: debugmqttconnection <0|1> [temp|runtime]" },
  { "debugmqttpubsub",     "Debug MQTT publish/subscribe + received messages.",        true, cmd_debugmqttpubsub,     "Usage: debugmqttpubsub <0|1> [temp|runtime]" },
  { "debugmqttdiscovery",  "Debug MQTT Home Assistant auto-discovery.",                true, cmd_debugmqttdiscovery,  "Usage: debugmqttdiscovery <0|1> [temp|runtime]" },
  { "debugmqttcommands",   "Debug MQTT inbound commands + auth.",                      true, cmd_debugmqttcommands,   "Usage: debugmqttcommands <0|1> [temp|runtime]" },
  { "debugauthsessions", "Debug auth sessions.", true, cmd_debugauthsessions, "Usage: debugauthsessions <0|1> [temp|runtime]" },
  { "debugauthcookies", "Debug auth cookies.", true, cmd_debugauthcookies, "Usage: debugauthcookies <0|1> [temp|runtime]" },
  { "debugauthlogin", "Debug auth login.", true, cmd_debugauthlogin, "Usage: debugauthlogin <0|1> [temp|runtime]" },
  { "debugauthbootid", "Debug auth boot ID.", true, cmd_debugauthbootid, "Usage: debugauthbootid <0|1> [temp|runtime]" },
  { "debughttphandlers", "Debug HTTP handlers.", true, cmd_debughttphandlers, "Usage: debughttphandlers <0|1> [temp|runtime]" },
  { "debughttprequests", "Debug HTTP requests.", true, cmd_debughttprequests, "Usage: debughttprequests <0|1> [temp|runtime]" },
  { "debughttpresponses", "Debug HTTP responses.", true, cmd_debughttpresponses, "Usage: debughttpresponses <0|1> [temp|runtime]" },
  { "debughttpstreaming", "Debug HTTP streaming.", true, cmd_debughttpstreaming, "Usage: debughttpstreaming <0|1> [temp|runtime]" },
  { "debugwificonnection", "Debug WiFi connection.", true, cmd_debugwificonnection, "Usage: debugwificonnection <0|1> [temp|runtime]" },
  { "debugwificonfig", "Debug WiFi config.", true, cmd_debugwificonfig, "Usage: debugwificonfig <0|1> [temp|runtime]" },
  { "debugwifiscanning", "Debug WiFi scanning.", true, cmd_debugwifiscanning, "Usage: debugwifiscanning <0|1> [temp|runtime]" },
  { "debugwifidriver", "Debug WiFi driver.", true, cmd_debugwifidriver, "Usage: debugwifidriver <0|1> [temp|runtime]" },
  { "debugstoragefiles", "Debug storage files.", true, cmd_debugstoragefiles, "Usage: debugstoragefiles <0|1> [temp|runtime]" },
  { "debugstoragejson", "Debug storage JSON.", true, cmd_debugstoragejson, "Usage: debugstoragejson <0|1> [temp|runtime]" },
  { "debugstoragesettings", "Debug storage settings.", true, cmd_debugstoragesettings, "Usage: debugstoragesettings <0|1> [temp|runtime]" },
  { "debugstoragemigration", "Debug storage migration.", true, cmd_debugstoragemigration, "Usage: debugstoragemigration <0|1> [temp|runtime]" },
  { "debugstoragepermissions", "Debug storage [PERM] DENY audit.", true, cmd_debugstoragepermissions, "Usage: debugstoragepermissions <0|1> [temp|runtime]" },
  { "debugstack", "Low-level stack/heap trace to Serial: <on|off>.", true, cmd_debugstack, "Usage: debugstack <0|1|on|off>" },
  { "debugsystemboot", "Debug system boot.", true, cmd_debugsystemboot, "Usage: debugsystemboot <0|1> [temp|runtime]" },
  { "debugsystemconfig", "Debug system config.", true, cmd_debugsystemconfig, "Usage: debugsystemconfig <0|1> [temp|runtime]" },
  { "debugsystemtasks", "Debug system tasks.", true, cmd_debugsystemtasks, "Usage: debugsystemtasks <0|1> [temp|runtime]" },
  { "debugsystemhardware", "Debug system hardware.", true, cmd_debugsystemhardware, "Usage: debugsystemhardware <0|1> [temp|runtime]" },
  { "debugusersmgmt", "Debug users management.", true, cmd_debugusersmgmt, "Usage: debugusersmgmt <0|1> [temp|runtime]" },
  { "debugusersregister", "Debug users registration.", true, cmd_debugusersregister, "Usage: debugusersregister <0|1> [temp|runtime]" },
  { "debugusersquery", "Debug users query.", true, cmd_debugusersquery, "Usage: debugusersquery <0|1> [temp|runtime]" },
  { "debugcliexecution", "Debug CLI execution.", true, cmd_debugcliexecution, "Usage: debugcliexecution <0|1> [temp|runtime]" },
  { "debugcliqueue", "Debug CLI queue.", true, cmd_debugcliqueue, "Usage: debugcliqueue <0|1> [temp|runtime]" },
  { "debugclivalidation", "Debug CLI validation.", true, cmd_debugclivalidation, "Usage: debugclivalidation <0|1> [temp|runtime]" },
  { "debugperfstack", "Debug performance stack.", true, cmd_debugperfstack, "Usage: debugperfstack <0|1> [temp|runtime]" },
  { "debugperfheap", "Debug performance heap.", true, cmd_debugperfheap, "Usage: debugperfheap <0|1> [temp|runtime]" },
  { "debugperftiming", "Debug performance timing.", true, cmd_debugperftiming, "Usage: debugperftiming <0|1> [temp|runtime]" },
  { "debugsseconnection", "Debug SSE connection.", true, cmd_debugsseconnection, "Usage: debugsseconnection <0|1> [temp|runtime]" },
  { "debugsseevents", "Debug SSE events.", true, cmd_debugsseevents, "Usage: debugsseevents <0|1> [temp|runtime]" },
  { "debugssebroadcast", "Debug SSE broadcast.", true, cmd_debugssebroadcast, "Usage: debugssebroadcast <0|1> [temp|runtime]" },
  { "debugcmdflowrouting", "Debug command flow routing.", true, cmd_debugcmdflowrouting, "Usage: debugcmdflowrouting <0|1> [temp|runtime]" },
  { "debugcmdflowqueue", "Debug command flow queue.", true, cmd_debugcmdflowqueue, "Usage: debugcmdflowqueue <0|1> [temp|runtime]" },
  { "debugcmdflowcontext", "Debug command flow context.", true, cmd_debugcmdflowcontext, "Usage: debugcmdflowcontext <0|1> [temp|runtime]" },
  { "debugcommandsystem", "Debug modular command registry operations.", true, cmd_debugcommandsystem, "Usage: debugcommandsystem <0|1> [temp|runtime]" },
  { "debugautomations", "Debug automations scheduler and actions.", true, cmd_debugautomations, "Usage: debugautomations <0|1> [temp|runtime]" },
  { "debuglogger", "Debug sensor logger internals.", true, cmd_debuglogger, "Usage: debuglogger <0|1> [temp|runtime]" },
  { "commandmodulesummary", "Show command module summary.", true, cmd_commandmodulesummary },
  { "settingsmodulesummary", "Show settings module summary.", true, cmd_settingsmodulesummary },
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "outg2", "Enable/disable G2 glasses output.", false, cmd_outg2, "Usage: outg2 <0|1> - streams CLI output to G2 glasses" },
  { "debugg2", "Debug G2 smart glasses BLE operations.", true, cmd_debugg2, "Usage: debugg2 <0|1> [temp|runtime]" },
  { "debugg2lifecycle", "Debug G2 BLE lifecycle (scan/connect/MTU).", true, cmd_debugg2lifecycle, "Usage: debugg2lifecycle <0|1> [temp|runtime]" },
  { "debugg2protocol",  "Debug G2 envelope TX/RX, CRC, fragmentation.", true, cmd_debugg2protocol,  "Usage: debugg2protocol <0|1> [temp|runtime]" },
  { "debugg2events",    "Debug G2 DevEvents/SysEvents/gestures.",       true, cmd_debugg2events,    "Usage: debugg2events <0|1> [temp|runtime]" },
  { "debugg2pages",     "Debug G2 page-swap worker / hijack / lens state.", true, cmd_debugg2pages, "Usage: debugg2pages <0|1> [temp|runtime]" },
  { "debugg2heartbeat", "Debug G2 heartbeat TX + acks (loud).",         true, cmd_debugg2heartbeat, "Usage: debugg2heartbeat <0|1> [temp|runtime]" },
  { "debugg2dump",      "Debug G2 ring-buffer dumps on errors.",        true, cmd_debugg2dump,      "Usage: debugg2dump <0|1> [temp|runtime]" },
  { "debugring",          "Debug R1 health ring (parent flag).",             true, cmd_debugring,          "Usage: debugring <0|1> [temp|runtime]" },
  { "debugringlifecycle", "Debug R1 scan/connect/GATT/disconnect.",          true, cmd_debugringlifecycle, "Usage: debugringlifecycle <0|1> [temp|runtime]" },
  { "debugringsetup",     "Debug R1 setup ritual + clock custody.",          true, cmd_debugringsetup,     "Usage: debugringsetup <0|1> [temp|runtime]" },
  { "debugringprotocol",  "Debug R1 per-frame decode/reassembly (loud).",    true, cmd_debugringprotocol,  "Usage: debugringprotocol <0|1> [temp|runtime]" },
  { "debugringtxn",       "Debug R1 transactions + packetAck (loud).",       true, cmd_debugringtxn,       "Usage: debugringtxn <0|1> [temp|runtime]" },
  { "debugringhealth",    "Debug R1 telemetry cache + history sweep.",       true, cmd_debugringhealth,    "Usage: debugringhealth <0|1> [temp|runtime]" },
  { "debugringbridge",    "Debug R1→G2 spoof bridge push.",                  true, cmd_debugringbridge,    "Usage: debugringbridge <0|1> [temp|runtime]" },
  { "debugringdump",      "Debug R1 raw hex frame/payload dumps (loud).",    true, cmd_debugringdump,      "Usage: debugringdump <0|1> [temp|runtime]" },
#endif
  { "outble", "Enable/disable BLE broadcast output.", false, cmd_outble, "Usage: outble <0|1> - streams broadcast output to authenticated BLE clients" },
  { "debugsr",          "Debug ESP-SR speech recognition (parent flag).", true, cmd_debugsr,          "Usage: debugsr <0|1> [temp|runtime]" },
  { "debugsrwake",      "Debug SR wake word detection events.",            true, cmd_debugsrwake,      "Usage: debugsrwake <0|1> [temp|runtime]" },
  { "debugsrcommand",   "Debug SR MultiNet command recognition.",          true, cmd_debugsrcommand,   "Usage: debugsrcommand <0|1> [temp|runtime]" },
  { "debugsrafe",       "Debug SR AFE chain (VAD/noise/gain).",            true, cmd_debugsrafe,       "Usage: debugsrafe <0|1> [temp|runtime]" },
  { "debugsrlifecycle", "Debug SR init/start/stop verbose.",                true, cmd_debugsrlifecycle, "Usage: debugsrlifecycle <0|1> [temp|runtime]" },
  { "debugsrtuning",    "Debug SR auto-tune sweeps + threshold.",           true, cmd_debugsrtuning,    "Usage: debugsrtuning <0|1> [temp|runtime]" },
  { "debugfmradio", "Debug FM Radio operations.", true, cmd_debugfmradio, "Usage: debugfmradio <0|1> [temp|runtime]" },
  { "memorysampleintervalsec", "Set memory sampling interval in seconds (0=disabled).", true, cmd_memorysampleintervalsec, "Usage: memorysampleintervalsec <0-300>" },
  { "loglevel", "Set log level (error|warn|info|debug).", true, cmd_loglevel, "Usage: loglevel <error|warn|info|debug>" },
  { "log", "System-wide logging to file.", true, cmd_log, "Usage: log <start|stop|status|autostart>\n  start [\"filepath\"] [flags=0x...] [tags=0|1]: Begin system logging\n    filepath: Log file path, quoted (auto-generated if omitted)\n    flags: Debug flag mask, up to 64 hex digits (bit map in System_Debug.h)\n    tags: Prefix lines with category tags (0|1, default 1)\n  stop / status: Stop logging / show logging status\n  autostart [on|off]: Toggle logging auto-start on boot (bare = toggle)" },
  { "webconsole", "Enable/disable browser-side debug console output in the web UI.", true, cmd_webconsole, "Usage: webconsole <0|1>" },
};

const size_t debugCommandsCount = sizeof(debugCommands) / sizeof(debugCommands[0]);

// C3 drift tripwire. The flag/sub command handlers are generated thunks: 119
// flag rows (DBG_FLAG_COUNT minus the ALWAYS control row that carries
// DBG_NO_CMD) + 40 bitless subs = 159. The rows enumerated below stay
// hand-written. Adding a flag/sub row to the tables, or a hand row here,
// without reconciling the counts fails this assert. The subtractions track the
// two feature-gated table spans: 6 LLM flag rows, 7 debugg2* + 8 debugring*
// flag rows (both under ENABLE_BLUETOOTH && ENABLE_G2_GLASSES), and the
// outg2 hand row.
static constexpr size_t kDbgHandCmdRows = 15;  // debugespnow(alias), notifstats,
  // debugverbose, debugbuffer, debugflags, loglink, debugstack,
  // commandmodulesummary, settingsmodulesummary, outg2, outble,
  // memorysampleintervalsec, loglevel, log, webconsole
static constexpr size_t kDbgGenCmdRowsInTable =
    (size_t)(DBG_FLAG_COUNT - 1) + (size_t)DBG_SUBBOOL_COUNT
#if !ENABLE_LLM_SOURCE_ONBOARD
    - 6
#endif
#if !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)
    - 15  // 7 debugg2* + 8 debugring*
#endif
    ;
static constexpr size_t kDbgHandCmdRowsInTable = kDbgHandCmdRows
#if !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)
    - 1  // outg2 is G2-gated
#endif
    ;
static_assert(sizeof(debugCommands) / sizeof(debugCommands[0])
                  == kDbgGenCmdRowsInTable + kDbgHandCmdRowsInTable,
              "debugCommands row count drifted from generated(thunk)+hand expectation");

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

void DebugManager::setSystemLogEnabled(bool enabled) { gSystemLogRunning = enabled; }
bool DebugManager::isSystemLogEnabled() const { return gSystemLogRunning; }

void DebugManager::setLogCategoryTags(bool enabled) { gSystemLogCategoryTags = enabled; }
bool DebugManager::getLogCategoryTags() const { return gSystemLogCategoryTags; }

// ============================================================================
// Logging System (merged from System_Logging.cpp)
// ============================================================================
// Handles structured logging to LittleFS files with automatic cap enforcement.

// External dependencies
extern bool appendLineWithCap(const char* path, const String& line, size_t capBytes);
extern void getTimestampPrefixMsCached(char* buf, size_t bufSize);

// Time sync marker flag
bool gTimeSyncedMarkerWritten = false;

// Log File Path Definitions
extern const char* const LOG_OK_FILE = "/system/sys_logs/successful_login.log";              // ~680KB cap
extern const char* const LOG_FAIL_FILE = "/system/sys_logs/failed_login.log";                // ~680KB cap
extern const char* const LOG_I2C_FILE = "/system/sys_logs/i2c_errors.log";                   // 64KB cap
extern const char* const LOG_ERROR_FILE = "/system/sys_logs/errors.log";                      // LOG_ERROR_CAP
extern const char* const LOG_EVENTS_FILE = "/system/sys_logs/system-events.log";              // LOG_EVENTS_CAP
extern const char* const LOG_EVENT_STREAM_FILE = "/system/sys_logs/events.log";               // LOG_EVENT_STREAM_CAP

void logToFile(const char* path, const String& line, size_t capBytes) {
  appendLineWithCap(path, line, capBytes);
}

// ============================================================================
// Auth Logging
// ============================================================================
// Lives here, not in WebServer_Server.cpp, because the security audit trail
// must not depend on the web server being compiled in: that whole file is
// wrapped in `#if ENABLE_HTTP_SERVER`, so a headless build used to lose login
// auditing on EVERY transport (serial and UART included), silently.

// Only logs to file for actual login events, not all auth attempts.
void logAuthAttempt(bool success, const char* path, const String& userTried, const String& ip, const String& reason) {
  // Normalize path for checking
  String cleanPath = String(path ? path : "");
  cleanPath.replace("%2F", "/");
  cleanPath.replace("%20", " ");

  // Only log to file if this is an actual security event (login or credential
  // rotation). Path checks use exact equality to avoid spurious matches like
  // "/configure-login-page" or "/login-help" triggering on the substring
  // "/login". The reason-based matches catch events whose path varies by
  // transport (e.g. password change is "/account/password-change" from web
  // but "/oled/command" from OLED).
  // Any "<transport>/login" path (web "/login", serial/login, bluetooth/login,
  // display/login) plus the G2 pair event and credential-rotation reasons.
  // endsWith avoids spurious matches like "/login-help" / "/configure-login-page".
  bool isSecurityAuditEvent =
      cleanPath.endsWith("/login") ||
      (cleanPath == "g2/pair") ||
      (cleanPath == "espnow/bond") ||
      (reason.indexOf("Login successful") >= 0) ||
      (reason.indexOf("Password changed") >= 0) ||
      (reason.indexOf("Current password incorrect") >= 0) ||
      (reason.indexOf("Password storage failed") >= 0);

  if (!isSecurityAuditEvent) {
    // Not a security event - skip file logging (command audit handles command tracking)
    return;
  }

  char tsPrefix[40];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
  String status = success ? "SUCCESS" : "FAILED";

  String cleanIP = ip;
  cleanIP.replace("::FFFF:", "");

  // Format: [ts] | STATUS | user=.. | ip=.. | /path [| reason=..]
  String line;
  line.reserve(160);
  if (tsPrefix[0]) line += tsPrefix;  // already includes trailing " | "
  line += status;
  line += " | user="; line += userTried;
  line += " | ip=";   line += cleanIP;
  line += " | ";      line += cleanPath;
  if (reason.length()) { line += " | reason="; line += reason; }

  const char* logFile = success ? LOG_OK_FILE : LOG_FAIL_FILE;
  appendLineWithCap(logFile, line, LOG_CAP_BYTES);
}

// Single audit front-door for all credential logins (web, serial, UART, BLE,
// OLED). G2 is excluded — it has no credential login (its pair-time identity
// is logged separately with path "g2/pair"). Maps the transport to a canonical
// "<x>/login" path and fills a synthetic IP when the caller has none, so every
// login path records consistently through one place.
void recordLoginAttempt(CommandSource transport, const String& user,
                        const String& ip, bool success, const char* reason) {
  const char* path;
  const char* defIp;
  switch (transport) {
    case SOURCE_WEB:           path = "web/login";       defIp = "web";   break;
    case SOURCE_SERIAL:        path = "serial/login";    defIp = "local"; break;
    case SOURCE_UART:          path = "uart/login";      defIp = "uart";  break;
    case SOURCE_BLUETOOTH:     path = "bluetooth/login"; defIp = "ble";   break;
    case SOURCE_LOCAL_DISPLAY: path = "display/login";   defIp = "local"; break;
    default:                   path = "login";           defIp = "local"; break;
  }
  logAuthAttempt(success, path, user, ip.length() ? ip : String(defIp), reason ? reason : "");
}

// Log a one-time marker when the clock first becomes valid; safe to call
// anytime. `source` names who supplied the time ("ntp", "rtc", "ring",
// "manual", "carryover") — the marker line is a forensic breadcrumb, so it
// should say which custody chain produced this boot's clock.
void logTimeSyncedMarkerIfReady(const char* source) {
  if (gTimeSyncedMarkerWritten) {
    return;
  }

  time_t t = time(nullptr);
  if (t <= 0) {
    return;
  }

  static char* bootTsPrefix = nullptr;
  if (!bootTsPrefix) {
    bootTsPrefix = (char*)ps_alloc(48, AllocPref::PreferPSRAM, "boot.ts");
    if (!bootTsPrefix) return;
  }
  
  getTimestampPrefixMsCached(bootTsPrefix, 48);
  char fallbackPrefix[48];
  if (!bootTsPrefix[0]) { snprintf(fallbackPrefix, sizeof(fallbackPrefix), "[BOOT ms=%lu] | ", millis()); }
  String prefix = bootTsPrefix[0] ? String(bootTsPrefix) : String(fallbackPrefix);
  String line = prefix + "Time Synced via " +
                (source && source[0] ? source : "?");  // distinct from the "Device Powered On" boot anchor
  
  appendLineWithCap(LOG_OK_FILE, line, LOG_CAP_BYTES);
  appendLineWithCap(LOG_FAIL_FILE, line, LOG_CAP_BYTES);
  appendLineWithCap(LOG_I2C_FILE, line, LOG_I2C_CAP);
  appendLineWithCap(LOG_ERROR_FILE, line, LOG_ERROR_CAP);
  appendLineWithCap(LOG_EVENTS_FILE, line, LOG_EVENTS_CAP);
  // events.log is opt-out (eventlog 0) — don't re-create a disabled log.
  if (gSettings.eventLogEnabled) {
    appendLineWithCap(LOG_EVENT_STREAM_FILE, line, LOG_EVENT_STREAM_CAP);
  }

  gTimeSyncedMarkerWritten = true;
  // NOTE: writeBootAnchor()/resolvePendingUserCreationTimes() used to hang
  // off this one-shot — which meant a boot whose clock arrived via a
  // non-NTP source (or improved after the marker fired) never re-anchored.
  // Those duties now flow through Clock::clockStepped()'s pend flags and
  // run (repeatably, upsert-safe) from Clock::clockDutiesTick().
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

// Always-on per-boot orientation divider. Unlike logTimeSyncedMarkerIfReady()
// (which is gated on NTP/RTC producing a valid wall-clock time), this fires
// unconditionally early in boot so EVERY log carries a "which boot am I looking
// at" marker — even on an offline boot that never syncs time. Written to the
// login / i2c / error logs; system-events.log is intentionally skipped because
// it already gets the richer "[EVENT][BOOT] boot #N | reset=… | fw v…" line.
// The timestamp is millis-based here (time isn't synced yet); the later
// time-synced marker ties millis→wall-clock once NTP lands.
void logBootAnchorToLogs(const char* resetReason, const char* detail) {
  if (!filesystemReady) return;
  extern uint32_t gBootCounter;

  String line = buildTimestampPrefix();
  line += "Device Powered On | boot #";
  line += String((unsigned long)gBootCounter);
  line += " | reset=";
  line += resetReason ? resetReason : "?";
  if (detail && detail[0]) { line += " | "; line += detail; }  // e.g. "reboot: command by web:red"

  appendLineWithCap(LOG_OK_FILE, line, LOG_CAP_BYTES);
  appendLineWithCap(LOG_FAIL_FILE, line, LOG_CAP_BYTES);
  appendLineWithCap(LOG_I2C_FILE, line, LOG_I2C_CAP);
  appendLineWithCap(LOG_ERROR_FILE, line, LOG_ERROR_CAP);
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
  if (gSystemLogRunning) return;  // Already running
  
  // Use persisted path if set, otherwise auto-generate
  String filepath = (gSettings.systemLogPath.length() > 0) ? gSettings.systemLogPath : generateSystemLogFilename();
  gSystemLogCategoryTags = gSettings.systemLogCategoryTags;

  // Restore last-used category mask so autostart logs the same categories
  // that were selected when system logging was last started.
  systemLogApplyPersistedFlags();
  
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
    logSystemEvent("LOG", "debug-capture log autostart FAILED — could not create %s", filepath.c_str());
    return;
  }
  f.printf("# System log auto-started at %lu ms\n", millis());
  f.close();
  fsUnlock();
  
  gSystemLogPath = filepath;
  gSystemLogRunning = true;
  gSystemLogLastWrite = millis();
  gOutputFlags |= MSG_ROUTE_FILE;

  broadcastOutput("[SYSTEM_LOG] Auto-start enabled, logging to: " + filepath);
  logSystemEvent("LOG", "debug-capture log autostart OK → %s", filepath.c_str());
}

// ============================================================================
// System Log Settings Module
// ============================================================================

// Settings-page BitmaskField options for `systemLogFlags`, generated from
// DBG_FLAG_LIST so EVERY debug flag gets a checkbox (this used to be a
// hand-kept subset that drifted ~71 flags behind the table). Format is
// UNCHANGED — the same "bitmask:0x<mask>|Label,..." dialect
// BitmaskField.parseBits already reads, with "#|<Bank>" separators at each
// family boundary. Persistence is untouched: the stored systemLogFlags value
// stays a hex mask string; only this display vocabulary is regenerated.
//
// Baked at COMPILE TIME into flash .rodata (2026-08-19). The old runtime
// builder assembled this into a static Arduino String at static-init because
// the preprocessor cannot turn bit 128 into its 33-hex-digit mask — but
// constexpr can. The runtime version cost ~5.5 KB of internal heap forever
// (a reserve(5000) buffer the ~5.5 KB string then outgrew and reallocated)
// and its runtime-initialized pointer dragged the whole otherwise-const
// SettingEntry table below into .data. Mask digits are built the same way:
// lead hex digit "1248"[bit&3], then (bit>>2) zeros (1<<0=0x1, 1<<4=0x10,
// 1<<32=0x100000000, ...). Output is byte-identical to the old builder.
namespace {

#define DBG_X(SYM, bit, BANK, parentBit, tag, settingsField, cmdIdent, group, jsonKey, label) label,
constexpr const char* kDbgOptLabel[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };
#undef DBG_X

// Emit the BitmaskField options dialect into `out` (nullptr = dry run),
// returning the emitted length (excluding the NUL). Same algorithm as the
// old runtime builder; iterates the generated kDbgBank/kDbgBit columns,
// which share DBG_FLAG_LIST's order by construction.
constexpr size_t dbgEmitLogFlagOptions(char* out) {
  size_t n = 0;
  auto put = [&](char c) { if (out) out[n] = c; n++; };
  auto puts_ = [&](const char* str) { for (size_t i = 0; str[i]; i++) put(str[i]); };
  puts_("bitmask:");
  bool first = true;
  DbgBank lastBank = DBG_BANK_COUNT;   // sentinel: no bank emitted yet
  for (size_t i = 0; i < DBG_FLAG_COUNT; i++) {
    const DbgBank bank = kDbgBank[i];
    if (bank == DBG_BANK_CONTROL) continue;   // control bits: no checkbox, no header
    if (bank != lastBank) {                   // family boundary -> "#|<Bank>" separator
      if (!first) put(',');
      puts_("#|");
      puts_(kDbgBankLabel[bank]);
      first = false;
      lastBank = bank;
    }
    if (!first) put(',');
    first = false;
    const int bit = kDbgBit[i];
    puts_("0x");
    put("1248"[bit & 3]);
    for (int z = bit >> 2; z > 0; --z) put('0');
    put('|');
    puts_(kDbgOptLabel[i]);
  }
  if (out) out[n] = '\0';
  return n;
}

constexpr size_t kSystemLogFlagsOptionsLen = dbgEmitLogFlagOptions(nullptr);

struct SystemLogFlagsOptionsBuf { char s[kSystemLogFlagsOptionsLen + 1]; };

constexpr SystemLogFlagsOptionsBuf dbgBuildLogFlagOptions() {
  SystemLogFlagsOptionsBuf b{};
  dbgEmitLogFlagOptions(b.s);
  return b;
}

// Flash-resident (.rodata) options string; the SettingEntry below points its
// `options` at kSystemLogFlagsOptions.s. Regenerated at every build from
// DBG_FLAG_LIST — drift is impossible. It is LIVE (drives the systemLogFlags
// BitmaskField grid on the web Settings page).
constexpr SystemLogFlagsOptionsBuf kSystemLogFlagsOptions = dbgBuildLogFlagOptions();

}  // namespace

static const SettingEntry systemLogSettingEntries[] = {
  { "systemLogEnabled", SETTING_BOOL, &gSettings.systemLogEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "systemlogenabled" },
  { "systemLogAutoStart",    SETTING_BOOL,   &gSettings.systemLogAutoStart,    0, 0, nullptr, 0, 1, "Auto-start logging after boot", nullptr, false, nullptr, "log autostart" },
  { "systemLogPath", SETTING_STRING, &gSettings.systemLogPath, 0, 0, "", 0, 0, "Log file path (empty = auto-generate)", nullptr, false, nullptr, nullptr },
  { "systemLogCategoryTags", SETTING_BOOL, &gSettings.systemLogCategoryTags, 1, 0, nullptr, 0, 1, "Include category tags", nullptr, false, nullptr, "logcategorytags" },
  { "systemLogFlags", SETTING_STRING, &gSettings.systemLogFlags, 0, 0, "", 0, 0, "Debug message categories",
    kSystemLogFlagsOptions.s, false, nullptr, "systemlogflags" },
  { "eventLog", SETTING_BOOL, &gSettings.eventLogEnabled, 1, 0, nullptr, 0, 1, "Structured event history (events.log)", nullptr, false, nullptr, "eventlog" },
};

extern const SettingsModule systemLogSettingsModule = {
  "systemlog",
  "logging.systemlog",
  systemLogSettingEntries,
  sizeof(systemLogSettingEntries) / sizeof(systemLogSettingEntries[0]),
  nullptr,
  "System debug logging to file"
};
