#include <Arduino.h>
#include <FS.h>
#include <stdarg.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <freertos/semphr.h>

#include "System_BuildConfig.h"
#include "System_Clock.h"  // Clock::isValidEpoch — one epoch-validity vocabulary
#include "System_Filesystem.h"
#include "System_VFS.h"
#include "OLED_ConsoleBuffer.h"
#include "BLE_Server.h"
#include "G2_Glasses.h"
#include "System_CLI.h"
#include "System_CLIMode.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_EspLogCore.h"
#include "System_RadioLogCore.h"
#include "HAL_Radio.h"  // HW1_RADIO_COMPANION: the ESP-Hosted tags exist
#include "System_Logging.h"
#include "System_LogCapsCore.h"  // kBatchTickMs: this task's idle wake
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_User.h"  // CommandSource — recordLoginAttempt maps it to an audit path
#include "System_Utils.h"
#include "System_JsonText.h"  // jsonEscapeInto: the log path in the status json
#include "System_AuthIdentity.h"  // currentCommandContext / currentCaptureState
#include "WebServer_Utils.h"
#include "System_ESPSR.h"  // srSyncDebugLevel()
#include "System_CommandRegistry.h"
#include "System_Notifications.h"
#include "System_SealedLog.h"      // B11: the system log on the card is sealed
#include "System_SealedLogCore.h"  // kMarkLength: a sealed file that holds only its mark is new
#include "HAL_Motion.h"
#include "i2csensor_mlx90640.h"
#include "i2csensor_rda5807.h"
#include "i2csensor_vl53l4cx.h"
#include "System_ESPNow.h"
#include "System_BootDebug.h"
#include <esp_timer.h>

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
#if ENABLE_DEBUG_ALL_BY_DEFAULT
// Test image (System_BuildConfig.h §6): every flag row, so the early-boot
// window logs everything too. The ALWAYS control bit names no producer and
// is left clear; DEBUG_##SYM is pasted at the first macro level (see DBG_MAP).
#define DBG_ALL_OR(SYM, ...) | DEBUG_##SYM
static constexpr DebugFlagMask kBootDefaultDebugFlags =
    (((DebugFlagMask)0) DBG_FLAG_LIST(DBG_ALL_OR)) & ~DEBUG_ALWAYS;
#undef DBG_ALL_OR
#else
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
#endif
DebugFlagMask gDebugFlags = kBootDefaultDebugFlags;
#if ENABLE_DEBUG_ALL_BY_DEFAULT
// Bitless sub-flags on from static init as well; applySettings() re-mirrors
// them from gSettings (same value) once debug.json has loaded.
static constexpr DebugSubFlags dbgAllSubFlagsOn() {
  DebugSubFlags s = {};
#define DBG_SUB_ON(SYM, subField, ...) s.subField = true;
  DBG_SUBBOOL_LIST(DBG_SUB_ON)
#undef DBG_SUB_ON
  return s;
}
DebugSubFlags gDebugSubFlags = dbgAllSubFlagsOn();
#else
DebugSubFlags gDebugSubFlags = {}; // All sub-flags initialized to false
#endif
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
volatile bool gDebugLineForce = false;  // debugverbose, or the boot capture (System_BootDebug.cpp)

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
// B11: the file that handle is (or was last) open on. gSystemLogPath is the
// path the session was started with; while a card takes log lines the file is
// its card twin, sealed. Resolved when the file is opened (the start, the
// first line after a rotation, a card that failed) and looked at again once a
// minute while the file is on flash, so a card that comes back takes the log
// again. Written under the filesystem lock.
static String gSystemLogActivePath;
static uint32_t gSystemLogTierCheckedMs = 0;
static constexpr uint32_t kSystemLogTierCheckMs = 60000;
// B11: what the open card file is known to hold. A row the handle took is on
// the card only once a flush was followed by the file having the size the
// rows make (SealedLog::settle): the file layer buffers, and a pulled card
// fails neither write() nor flush(). Until then the plain text of the rows
// written since the last verified flush is kept (PSRAM), so that a card that
// went away loses nothing: those lines are written to flash instead
// (systemLogCardFailoverLocked). Written under the filesystem lock.
static constexpr size_t kSystemLogPendingBytes = 8192;  // 20 lines between flushes, 320 bytes at most each
struct SystemLogCard {
  size_t stored;        // bytes the handle has taken: the size at the open and every row since
  size_t settled;       // the size at the last flush that verified
  size_t pendingBytes;  // of `pending`
  char*  pending;       // the lines written since, each with its '\n'
};
EXT_RAM_BSS_ATTR static SystemLogCard sSystemLogCard;
// The line being sealed and the path being resolved: not locals, since both
// are used on the log task's 6 KB stack above the seal and the file layer.
// Used under the filesystem lock only.
EXT_RAM_BSS_ATTR static char sSystemLogResolved[160];
static void systemLogCardFailoverLocked();
static bool systemLogCardSettleLocked();
static unsigned long gSystemLogLastFlush = 0;
static uint16_t gSystemLogUnflushedCount = 0;
static const uint16_t LOG_FLUSH_MESSAGE_COUNT = 20;      // Flush every 20 messages
static const uint32_t LOG_FLUSH_INTERVAL_MS = 5000;      // Or every 5 seconds

// Size bound for that file, held to the sensor logger's standard: a size
// limit and a count of old generations, both settings (`log maxsize`,
// `log rotations`). Until 2026-11-06 it grew without limit (3.5 MB in ten
// hours with ring and glasses debug on). At the limit the file becomes
// <path>.1 behind the shifted older generations, and a fresh file continues:
// renames only, under the lock the writer already holds, no copy.
static uint32_t gSystemLogRotations = 0;
static uint32_t gSystemLogRotateRetryAtMs = 0;  // set after a failed rotation; 0 = no backoff
static uint32_t gSystemLogReserveSkips = 0;     // lines not written: LittleFS below the reserve

// G-39: the system log's default budget (1 MB x 4 generations) exceeds the
// LittleFS partition of the 8 MB layouts. On LittleFS it must not eat the
// reserve the flash latch keeps for settings and state saves, so lines are
// skipped and counted while flash is below it. The free figure is the VFS
// cache the capped logs already refresh, so most lines cost no traverse.
// `path` is the file the handle is on: since B11 that is the card twin while
// a card takes log lines, and the card is not held to the flash reserve.
static bool systemLogBlockedByReserve(const String& path) {
  if (VFS::getStorageType(path) == VFS::SDCARD) return false;
  return VFS::getCachedLittleFsFree() < VFS::logReserveBytes();
}

// The sensor logger's bounds; an out-of-range setting reads as the default.
static size_t systemLogMaxSizeBytes() {
  const int v = gSettings.systemLogMaxSize;
  return (v >= 10240 && v <= 10485760) ? (size_t)v : 1048576u;
}
static int systemLogMaxRotations() {
  const int v = gSettings.systemLogMaxRotations;
  return (v >= 0 && v <= 9) ? v : 3;
}

// Caller holds the FS lock with gSystemLogFile open. Closes the file, shifts
// the kept generations (.i -> .i+1, oldest removed first: FAT refuses to
// rename over an existing file), moves the file to .1 — or removes it when
// no generation is kept — and leaves the handle closed: the next line reopens
// the path for append exactly as a run's first write does. No log line from
// here — this runs inside the log writer itself.
//
// B11: the file rotated is the one the handle was on (gSystemLogActivePath),
// so a log on the card rotates on the card; its generations are sealed files
// with their own mark, and the next open starts a new marked file.
static void systemLogRotateLocked() {
  // A card file is rotated only once the card verifiably holds its rows; one
  // that does not is given up for flash and nothing is rotated.
  if (SealedLog::appliesTo(gSystemLogActivePath) && !systemLogCardSettleLocked()) {
    systemLogCardFailoverLocked();
    return;
  }
  gSystemLogFile.flush();
  gSystemLogFile.close();
  gSystemLogUnflushedCount = 0;
  const String& path = gSystemLogActivePath.length() ? gSystemLogActivePath : gSystemLogPath;
  const AuthContext ctx =
      VFS::systemAuth(VFS::captureScopeFor(path), "debug.system_log_rotate");
  const int keep = systemLogMaxRotations();
  bool ok;
  if (keep > 0) {
    const String oldest = path + "." + String(keep);
    if (VFS::existsGuarded(oldest, ctx)) VFS::removeGuarded(oldest, ctx);
    for (int i = keep - 1; i >= 1; --i) {
      const String from = path + "." + String(i);
      if (VFS::existsGuarded(from, ctx)) {
        VFS::renameGuarded(from, path + "." + String(i + 1), ctx);
      }
    }
    ok = VFS::renameGuarded(path, path + ".1", ctx);
  } else {
    ok = VFS::removeGuarded(path, ctx);  // start over, keep nothing
  }
  if (ok) {
    ++gSystemLogRotations;
    gSystemLogRotateRetryAtMs = 0;
  } else {
    // The file keeps growing past the limit; retry in a minute, not per line.
    gSystemLogRotateRetryAtMs = millis() + 60000u;
  }
}

// BLE broadcast output buffer (accumulates messages for periodic send to authenticated BLE clients)
#if ENABLE_BLUETOOTH
static String gBleOutputBuffer;
static bool gBleOutputBufferReserved = false;
static unsigned long gBleLastFlush = 0;
static const uint32_t BLE_OUTPUT_FLUSH_INTERVAL_MS = 150;   // Flush to BLE every 150ms (snappier; sendBleResponse fragments)
// MUST stay <= ESP_GATT_MAX_ATTR_LEN (512). A plaintext flush leaves as ONE
// directed notification per session (sendBleResponse -> bleRawNotifyToSession
// -> esp_ble_gatts_send_indicate, never fragmented), and one ATT value cannot
// be longer than that. The characteristic-wide sender this replaced
// (BLECharacteristic::setValue + notify(), since deleted) did not even fail on
// an oversize value: setValue dropped it and notify() re-sent the PREVIOUS
// payload, so an oversize flush was silently duplicated.
//
// This was 1024, which made every size-triggered flush 768..1023 B (the flush
// threshold is MAX - DEBUG_MSG_SIZE) — i.e. ALL of them over the limit, so on the
// plaintext path only the 150 ms timer flushes were getting through. 512 restores
// the invariant the MTU setup is written against (BLE_Server.cpp: MTU 517 => 514
// usable "fully covers the 512-byte gBleOutputBuffer flush"). With 512 the append
// guard caps length at 511 and flushes run 256..511 B — always deliverable.
//
// The Secure-Channel path (bleScSendEncryptedBound) fragments at 195 B and was
// never affected; the cap only matters for plaintext sessions.
static const size_t BLE_OUTPUT_BUFFER_MAX = 512;           // Coalesce per flush; one plaintext notification carries at most 512 B
#endif

// G2 glasses output buffer (accumulates messages for periodic display)
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
static String gG2OutputBuffer;
static unsigned long gG2LastFlush = 0;
static const uint32_t G2_FLUSH_INTERVAL_MS = 2000;  // Send to glasses every 2 seconds
static const size_t G2_BUFFER_MAX = 500;            // Max chars to buffer
#endif


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
  strlcpy(gHelpTail[i], t, sizeof(gHelpTail[i]));
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

// Forward declarations (definitions with I2C helpers below)
static String buildTimestampPrefix();
static void appendTimestampedLine(const char* path, const char* text, size_t capBytes);

// Forward declaration (definition with logSystemEvent below)
static void flushPreInitEvents();
static bool growDebugPoolIfNeeded(bool force);

// Writes one log line, "[<timestamp>] <text>\n" or with the category tag
// "[<timestamp>] [<tag>] <text>\n", to the serial port or the system log file
// in one write, the bytes Print::printf would write. printf formats into 64
// bytes on its stack and mallocs a block for a longer line, which is most of
// them; this formats into its own buffer, sized for a 255-character message
// with its prefix, and falls back to printf only for a line that does not fit
// (a category tag over 47 characters). Not inlined, so the buffer lives in
// this frame only for the write and the log task's own frame stays as it was.
static constexpr size_t kLogLineCapacity = DEBUG_MSG_SIZE + 64;
static __attribute__((noinline)) void writeLogLine(Print& out, unsigned long timestamp,
                                                   const char* tag, const char* text) {
  char line[kLogLineCapacity];
  const int length = tag ? snprintf(line, sizeof(line), "[%lu] [%s] %s\n", timestamp, tag, text)
                         : snprintf(line, sizeof(line), "[%lu] %s\n", timestamp, text);
  if (length < 0) return;  // printf writes nothing for a format error either
  if (static_cast<size_t>(length) < sizeof(line)) {
    out.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(length));
  } else if (tag) {
    out.printf("[%lu] [%s] %s\n", timestamp, tag, text);
  } else {
    out.printf("[%lu] %s\n", timestamp, text);
  }
}

// B11: opens the system log `path` for append where its next line goes: the
// card twin while a card takes log lines (sealed: SealedLog writes the mark
// into a new file, moves a file that is not this device's aside and ends a
// torn last row), else the path itself. The open counts only when the card
// verifiably holds the file as opened (SealedLog::settle). A card file that
// does not take the open is reported (which keeps this log, or with a failed
// card every log, off the card for a while) and the flash path is opened
// instead; a path the user put on the card has no flash twin. `active`
// receives the file's path, `isNew` whether it holds no line yet and
// `storedOut` a card file's size. `startFresh` (the boot auto-start, which
// begins its file anew on every boot) empties an existing file first; on the
// card that is this device's own file only, since one that is not is moved
// aside, never removed. The caller holds the filesystem lock.
static File systemLogOpenLocked(const String& path, String& active, bool* isNew = nullptr,
                                bool startFresh = false, size_t* storedOut = nullptr) {
  if (isNew) *isNew = false;
  if (storedOut) *storedOut = 0;
  char* resolved = sSystemLogResolved;
  active = path;
  if (path.length() + 4 < sizeof(sSystemLogResolved)) {
    (void)VFS::resolveLogPath(path.c_str(), 0, resolved, sizeof(sSystemLogResolved));
    active = resolved;
  }
  File file;
  if (SealedLog::appliesTo(active)) {
    const AuthContext cardCtx = VFS::systemAuth(VFS::captureScopeFor(active), "debug.system_log_append");
    size_t stored = 0;
    file = startFresh ? SealedLog::openFresh(active, cardCtx, stored)
                      : SealedLog::openAppend(active, cardCtx, stored);
    if (file && !SealedLog::settle(file, active, stored)) file.close();
    if (file) {
      if (isNew) *isNew = stored == sealedlog::kMarkLength + 1;
      if (storedOut) *storedOut = stored;
      return file;
    }
    if (SealedLog::appliesTo(path)) return file;
    VFS::noteLogCardFileFailure(path.c_str(), "debug.log.open");
    active = path;
  }
  const AuthContext ctx = VFS::systemAuth(VFS::captureScopeFor(active), "debug.system_log_append");
  const bool existed = isNew && !startFresh && VFS::existsGuarded(active, ctx);
  file = VFS::openGuarded(active, startFresh ? "w" : "a", ctx, true);
  if (file && isNew) *isNew = !existed || file.size() == 0;
  return file;
}

// The "# System log ..." line a session's file starts with: plain on flash,
// one sealed row on the card (`stored`: the card file's size before it).
static void systemLogWriteStartLine(File& file, const String& active, const char* what, size_t stored = 0) {
  char line[56];
  const int n = snprintf(line, sizeof(line), "# System log %s at %lu ms", what, millis());
  if (n <= 0) return;
  if (SealedLog::appliesTo(active)) {
    (void)SealedLog::writeLines(file, active, line, static_cast<size_t>(n), stored);
    (void)SealedLog::settle(file, active, stored);  // a row that did not land is mended at the next open
  } else {
    file.printf("%s\n", line);
  }
}

// Opens the log task's handle where the next line goes (the caller holds the
// filesystem lock, and the session is running).
static void systemLogOpenHandleLocked() {
  size_t stored = 0;
  gSystemLogFile = systemLogOpenLocked(gSystemLogPath, gSystemLogActivePath, nullptr, false, &stored);
  gSystemLogTierCheckedMs = millis();
  sSystemLogCard.stored = sSystemLogCard.settled = stored;
  sSystemLogCard.pendingBytes = 0;
  if (gSystemLogFile) {
    gSystemLogLastFlush = millis();
    gSystemLogUnflushedCount = 0;
  }
}

// Flushes the card file and verifies its size. True: the card holds every
// row written, and the kept lines are let go.
static bool systemLogCardSettleLocked() {
  SystemLogCard& card = sSystemLogCard;
  if (!SealedLog::settle(gSystemLogFile, gSystemLogActivePath, card.stored)) return false;
  card.settled = card.stored;
  card.pendingBytes = 0;
  return true;
}

// The card file did not take what was written (a row, or a flush that did not
// verify). Closes it, reports it, opens the log where its lines go now (flash)
// and writes there the kept lines the card does not verifiably hold. A log
// the user put on the card itself has no flash twin: its handle stays closed
// and the next line tries the card again. Leaves the handle open on flash, or
// closed.
static void systemLogCardFailoverLocked() {
  SystemLogCard& card = sSystemLogCard;
  size_t keptBytes = card.pending ? card.pendingBytes : 0;
  size_t taken = 0;
  if (gSystemLogFile) {
    const size_t have = gSystemLogFile.size();
    if (keptBytes && have > card.settled) {
      taken = sealedlog::linesLanded(card.pending, keptBytes, sealedlog::kMaxRowBytes, have - card.settled);
    }
    gSystemLogFile.close();
  }
  gSystemLogUnflushedCount = 0;
  card.pendingBytes = 0;
  if (SealedLog::appliesTo(gSystemLogPath)) return;
  VFS::noteLogCardFileFailure(gSystemLogPath.c_str(), "debug.log");
  systemLogOpenHandleLocked();
  if (gSystemLogFile && SealedLog::appliesTo(gSystemLogActivePath)) {
    gSystemLogFile.close();  // still the card: nothing plain goes there
    return;
  }
  if (!gSystemLogFile || taken >= keptBytes) return;
  if (systemLogBlockedByReserve(gSystemLogActivePath)) {
    ++gSystemLogReserveSkips;
    return;
  }
  gSystemLogFile.write(reinterpret_cast<const uint8_t*>(card.pending) + taken, keptBytes - taken);
}

// B11: one line for the system log on the card, as one sealed row (the row's
// '\n' ends it; the text is the line without one). A line that does not fit
// the buffer is cut to it. The line is formatted in external RAM, not on this
// task's stack, and its text is kept until a flush has verified the row.
// False when the card did not take the row: the caller gives the card file up
// (systemLogCardFailoverLocked) and writes this line on flash.
static __attribute__((noinline)) bool systemLogCardWriteLocked(unsigned long timestamp, const char* tag,
                                                               const char* text) {
  EXT_RAM_BSS_ATTR static char line[kLogLineCapacity];
  int length = tag ? snprintf(line, sizeof(line), "[%lu] [%s] %s", timestamp, tag, text)
                   : snprintf(line, sizeof(line), "[%lu] %s", timestamp, text);
  if (length < 0) return true;  // a format error writes nothing, as in writeLogLine
  if (static_cast<size_t>(length) >= sizeof(line)) length = static_cast<int>(sizeof(line)) - 1;
  SystemLogCard& card = sSystemLogCard;
  if (!card.pending) {
    card.pending = static_cast<char*>(ps_alloc(kSystemLogPendingBytes, AllocPref::PreferPsram, "syslog.pending"));
  }
  // No room to keep this line beside the unverified ones: verify those first.
  if (card.pending && card.pendingBytes + static_cast<size_t>(length) + 1 > kSystemLogPendingBytes &&
      !systemLogCardSettleLocked()) {
    return false;
  }
  if (!SealedLog::writeLines(gSystemLogFile, gSystemLogActivePath, line, static_cast<size_t>(length), card.stored)) {
    return false;
  }
  if (!card.pending) return systemLogCardSettleLocked();  // nothing can be kept: verify each row
  memcpy(card.pending + card.pendingBytes, line, static_cast<size_t>(length));
  card.pendingBytes += static_cast<size_t>(length);
  card.pending[card.pendingBytes++] = '\n';
  return true;
}

// Closes the log task's handle (the caller holds the filesystem lock). A card
// file is verified first; lines it did not take go to flash before the close.
static void systemLogCloseHandleLocked() {
  if (!gSystemLogFile) return;
  if (SealedLog::appliesTo(gSystemLogActivePath) && !systemLogCardSettleLocked()) systemLogCardFailoverLocked();
  if (gSystemLogFile) {
    gSystemLogFile.flush();
    gSystemLogFile.close();  // File::close() also drops the handle
  }
  gSystemLogUnflushedCount = 0;
}

// UP-06: the system log's file stays open between lines. When it is on the
// card it has to be closed before the card's mount ends (`sdunmount`,
// `sdmount`, `sdformat`, the card watch's remount): after that its descriptor
// number can belong to another file. The log task opens it again on its next
// line (and fails quietly while no card is mounted, as an open always did).
//
// Its callers hold the filesystem lock, so this must be the guard and never
// the fsLock()/fsUnlock() pair: fsUnlock() gives the mutex whenever this task
// holds it, which would hand the caller's lock away in the middle of the
// unmount (SD.end() and the format would then run unlocked, and the caller's
// own release would give a mutex another task may hold by then). The guard
// takes nothing when the task already holds the lock and releases only what
// it took.
void systemLogReleaseSdHandle() {
  FsLockGuard guard("debug.log.sd_release");
  if (gSystemLogFile && gSystemLogActivePath.startsWith("/sd/")) systemLogCloseHandleLocked();
}

// Debug output task - single writer for all debug messages
static TaskHandle_t gDebugOutputTaskHandle = nullptr;

void debugOutputTask(void* parameter) {
  while (true) {
    DebugMessage* msg = nullptr;
    // A timed wait, so the capped logs' batched lines are written within 2 s
    // of being logged even when no further message arrives (S-8; see
    // cappedLogsFlushDue below). The wake costs two reads while nothing is
    // pending.
    if (xQueueReceive(gDebugOutputQueue, &msg, pdMS_TO_TICKS(logcaps::kBatchTickMs)) == pdTRUE && msg) {
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
            // Not a local: this frame is under every sealed card write.
            EXT_RAM_BSS_ATTR static char mark[96];
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
        writeLogLine(Serial, msg->timestamp, nullptr, msg->text);
      }
      // Web mirror (circular buffer for /api/cli/logs polling)
      if ((msg->routing & MSG_ROUTE_WEB) && gWebMirror.buf) {
        // External RAM, not this task's frame, which stays on the stack under
        // the file sinks below (the sealed card writes). log_out is the only
        // consumer task, so one buffer serves every line.
        EXT_RAM_BSS_ATTR static char formattedMsg[DEBUG_MSG_SIZE + 32];
        int written = snprintf(formattedMsg, sizeof(formattedMsg), "[%lu] %s", msg->timestamp, msg->text);
        if (written > 0) {
          gWebMirror.appendDirect(formattedMsg, (size_t)written, true);
        }
      }
      // File output (system log)
      if ((msg->routing & MSG_ROUTE_FILE) && (gOutputFlags & MSG_ROUTE_FILE) &&
          gSystemLogRunning && gSystemLogPath.length() > 0) {
        fsLock("debug.log");
        // B11: a log that is on flash while its session could be on the card
        // is looked at again once a minute: when a card takes log lines by
        // then, the flash file is closed and the next open goes to the card.
        if (gSystemLogFile && !SealedLog::appliesTo(gSystemLogActivePath) &&
            (uint32_t)(millis() - gSystemLogTierCheckedMs) >= kSystemLogTierCheckMs) {
          gSystemLogTierCheckedMs = millis();
          if (gSystemLogPath.length() + 4 < sizeof(sSystemLogResolved) &&
              VFS::resolveLogPath(gSystemLogPath.c_str(), 0, sSystemLogResolved, sizeof(sSystemLogResolved)) ==
                  VFS::LogTier::Overflow &&
              gSystemLogActivePath != sSystemLogResolved) {
            systemLogCloseHandleLocked();
          }
        }
        // Re-check under the lock: `log stop` closes the file and clears the
        // path in one lock hold, so a stale pre-lock check must not reopen.
        if (!gSystemLogFile && gSystemLogRunning && gSystemLogPath.length() > 0) systemLogOpenHandleLocked();
        bool wrote = false;
        if (gSystemLogFile && systemLogBlockedByReserve(gSystemLogActivePath)) {
          ++gSystemLogReserveSkips;  // shown by `log status`
        } else if (gSystemLogFile) {
          const char* cat = (gSystemLogCategoryTags && msg->category != 0)
                                ? getDebugCategoryName(msg->category) : nullptr;
          if (SealedLog::appliesTo(gSystemLogActivePath)) {
            // On the card: one sealed row. A card file that does not take it
            // (pulled mid-run) is given up: the lines it does not verifiably
            // hold and this one are written on flash. A log the user put on
            // the card itself has no flash twin.
            wrote = systemLogCardWriteLocked(msg->timestamp, cat, msg->text);
            if (!wrote) {
              systemLogCardFailoverLocked();
              if (gSystemLogFile && systemLogBlockedByReserve(gSystemLogActivePath)) {
                ++gSystemLogReserveSkips;
              } else if (gSystemLogFile) {
                writeLogLine(gSystemLogFile, msg->timestamp, cat, msg->text);
                wrote = true;
              }
            }
          } else {
            writeLogLine(gSystemLogFile, msg->timestamp, cat, msg->text);
            wrote = true;
          }
        }
        if (wrote && gSystemLogFile) {
          gSystemLogLastWrite = millis();
          gSystemLogUnflushedCount++;
          bool shouldFlush =
            (gSystemLogUnflushedCount >= LOG_FLUSH_MESSAGE_COUNT) ||
            ((millis() - gSystemLogLastFlush) >= LOG_FLUSH_INTERVAL_MS);
          if (shouldFlush) {
            // A card file's flush counts only when its size then matches what
            // was written; otherwise the card is given up for this log.
            if (!SealedLog::appliesTo(gSystemLogActivePath)) gSystemLogFile.flush();
            else if (!systemLogCardSettleLocked()) systemLogCardFailoverLocked();
            gSystemLogLastFlush = millis();
            gSystemLogUnflushedCount = 0;
          }
        }
        if (wrote && gSystemLogFile) {
          // Append mode: position() is the file size, buffered bytes included.
          if (gSystemLogFile.position() >= systemLogMaxSizeBytes() &&
              (gSystemLogRotateRetryAtMs == 0 ||
               (int32_t)(millis() - gSystemLogRotateRetryAtMs) >= 0)) {
            systemLogRotateLocked();
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

        STACK_TRACEF("log_out.err_entry dup=%d text=%.40s", isDup ? 1 : 0, msg->text);

        if (!isDup) {
          STACK_TRACEF("log_out.before_appendLineWithCap");
          appendTimestampedLine(LOG_ERROR_FILE, msg->text, LOG_ERROR_CAP);
          STACK_TRACEF("log_out.after_appendLineWithCap");
          strlcpy(lastErrText, msg->text, sizeof(lastErrText));
          lastErrTimeMs = now;
        }
      }

      // System-event ring (always, independent of routing — [EVENT] lines are
      // the always-on lifecycle record; see logSystemEvent()). Low volume by
      // design (discrete decisions, not polling), so no dedupe window needed.
      if (filesystemReady && strncmp(msg->text, "[EVENT]", 7) == 0) {
        appendTimestampedLine(LOG_EVENTS_FILE, msg->text, LOG_EVENTS_CAP);
        // A restart was announced (recordRebootIntent's line, every planned
        // restart writes one): put every batched line on disk now and write
        // the rest of this boot's lines at once. rebootDevice waits only
        // 250-1000 ms for this task before it restarts, less than a batch may
        // wait.
        if (strncmp(msg->text, "[EVENT][REBOOT]", 15) == 0) cappedLogsFlush(true);
      }

      // Structured event-stream tee (always, independent of routing) — one
      // line per event-ring entry, produced by systemEventLogTick() with the
      // [EVLOG] prefix and no display sinks in its route mask, so this file
      // append is the line's only destination.
      if (filesystemReady && strncmp(msg->text, "[EVLOG]", 7) == 0) {
        appendTimestampedLine(LOG_EVENT_STREAM_FILE, msg->text, LOG_EVENT_STREAM_CAP);
      }

      // OLED console
      #if ENABLE_OLED_DISPLAY
      if (msg->routing & MSG_ROUTE_OLED) {
        // A stale generation skips only this sink: the other branches above
        // and below still ran or run for the same envelope.
        gOledConsole.append(msg->text, (uint32_t)msg->timestamp, msg->oledGeneration);  // no-op until the ring exists
      }
      #endif

      // BLE broadcast output: buffered only while a session could receive a
      // flush (sendBleResponse's recipients: logged in, non-guest).
      #if ENABLE_BLUETOOTH
      if ((msg->routing & MSG_ROUTE_BLE) && (gOutputFlags & MSG_ROUTE_BLE) &&
          isBleConnected() && bleMirrorRecipientCount() > 0) {
        if (!gBleOutputBufferReserved) {
          gBleOutputBufferReserved = gBleOutputBuffer.reserve(BLE_OUTPUT_BUFFER_MAX);
        }
        size_t msgLen = strlen(msg->text);   // <= DEBUG_MSG_SIZE (256) < BLE_OUTPUT_BUFFER_MAX
        if (gBleOutputBuffer.length() + msgLen + 2 < BLE_OUTPUT_BUFFER_MAX) {
          gBleOutputBuffer += msg->text;
          gBleOutputBuffer += "\n";
        }
        // NON-BLOCKING flush: never stall this single debugOutputTask on the secure-channel tx
        // mutex. A paced multi-fragment command result can hold that mutex for hundreds of ms;
        // blocking here would stop draining gDebugOutputQueue and starve serial/web/OLED/file.
        // If the tx is busy we keep buffering and retry on the next message/interval; only when
        // the buffer is also full do we drop (best-effort console mirror — history is
        // authoritative). The "[output] dropped" marker still surfaces any loss.
        unsigned long now = millis();
        bool full = gBleOutputBuffer.length() >= BLE_OUTPUT_BUFFER_MAX - DEBUG_MSG_SIZE;
        bool due  = (now - gBleLastFlush >= BLE_OUTPUT_FLUSH_INTERVAL_MS);
        if (gBleOutputBuffer.length() > 0 && (full || due)) {
          if (sendBleResponse(gBleOutputBuffer.c_str(), gBleOutputBuffer.length(), /*blocking=*/false)) {
            gBleOutputBuffer = "";
            gBleLastFlush = now;
          } else if (full) {
            gBleOutputBuffer = "";   // tx busy and buffer full → drop to bound growth
            gBleLastFlush = now;
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
    // Capped logs: write the batches whose oldest line is 1.5 s old. Runs
    // after every message and on every idle wake of this task.
    if (filesystemReady) cappedLogsFlushDue();
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
      (size_t)addCount * sizeof(DebugMessage), AllocPref::RequirePsram, "debug.pool.growth");
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
  applyLogLevel((uint8_t)newLevel);

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
    gDebugBuffer = (char*)ps_alloc(GLOBAL_DEBUG_BUFFER_SIZE, AllocPref::PreferPsram, "debug.buf");
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
    AllocPref allocPref = hasPsram ? AllocPref::PreferPsram : AllocPref::DefaultHeap;
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
    taskStackRecord("log_out", DEBUG_OUT_STACK_WORDS);
    BaseType_t result = xTaskCreatePinnedToCore(
      debugOutputTask,
      "log_out",
      DEBUG_OUT_STACK_WORDS,  // 6 KB, internal RAM — see System_TaskUtils.h for sizing rationale
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
    gDebugBuffer = (char*)ps_alloc(GLOBAL_DEBUG_BUFFER_SIZE, AllocPref::PreferPsram, "debug.buf");
    if (!gDebugBuffer) {
      if (gOutputFlags & MSG_ROUTE_SERIAL) {
        Serial.println("Error: Failed to allocate debug buffer");
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
  // The OLED sink's ownership token for this envelope: a relaxed atomic load,
  // so it is safe from interrupt context and costs no lock.
#if ENABLE_OLED_DISPLAY
  msg->oledGeneration = gOledConsole.deliveryGeneration();
#else
  msg->oledGeneration = 0;
#endif
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

// The line path of every entry point. `shownWithoutCapture` matters only
// during the boot debug capture (System_BootDebug.h): the saved flags and log
// level would have shown this line. Every line of the boot
// is copied into the boot buffer; a line only the capture's force made goes
// nowhere else, so the console and the normal logs see what they always saw.
// Inlined into each, so a line costs the stack one frame, as before.
static inline __attribute__((always_inline)) void debugQueueVPrintf(DebugFlagMask flag, bool shownWithoutCapture,
                                                                    const char* fmt, va_list args) {
  if (!fmt) return;
  const bool capturing = gBootDebugCapturing;
  if (!capturing && (!getDebugQueue() || !getDebugFreeQueue())) return;

  // CRITICAL: Check if we're in a sensor task that's shutting down
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  if (currentTask == gThermalTaskHandle && !gThermalRunning) return;
  if (currentTask == gImuTaskHandle && !gImuRunning) return;
  if (currentTask == gTofTaskHandle && !gTofRunning) return;
  if (currentTask == gFmRadioTaskHandle && !gFmRadioRunning) return;

  // A forced line that passed its gate just before the capture ended: nothing
  // would keep it, so it is not formatted.
  if (capturing && !shownWithoutCapture && !gDebugLineForce) return;

  // Format into a stack line, then hand to the shared enqueue primitive
  // (ISR-safe; centralizes slot grab / send / drop + [CUT]). There is no split
  // layer — by design, see the note at the enqueueChunk call in
  // broadcastOutputCore: callers prove their line fits 255 B and pack multiple
  // lines with '\n' themselves. Nothing splits at runtime.
  const int64_t formatStartUs = (capturing && !shownWithoutCapture) ? esp_timer_get_time() : 0;
  char line[DEBUG_MSG_SIZE];
  int wn = vsnprintf(line, sizeof(line), fmt, args);
  if (wn < 0) return;
  size_t llen = (wn < (int)sizeof(line)) ? (size_t)wn : (sizeof(line) - 1);
  if (capturing) {
    bootDebugCaptureLine(line, llen, shownWithoutCapture, formatStartUs);
    if (!shownWithoutCapture) return;
    if (!getDebugQueue() || !getDebugFreeQueue()) return;
  }
  enqueueChunk(line, llen, MSG_ROUTE_ALL, flag, wn >= (int)DEBUG_MSG_SIZE);
}

// Whether the saved flags and log level (or debugverbose) would show a line
// of this flag at this level; always true outside the boot capture.
static inline bool lineShownWithoutCapture(DebugFlagMask flag, uint8_t level) {
  return !gBootDebugCapturing || gDebugVerbose ||
         (gLogLevel >= level &&
          ((flag & DEBUG_ALWAYS) != (DebugFlagMask)0 || (gDebugFlags & flag) != (DebugFlagMask)0));
}

// DEBUGF_QUEUE and the ERROR macros: shown by the flag (or ALWAYS).
void debugQueuePrintf(DebugFlagMask flag, const char* fmt, ...) {
  const bool shown = lineShownWithoutCapture(flag, LOG_LEVEL_ERROR);
  va_list args;
  va_start(args, fmt);
  debugQueueVPrintf(flag, shown, fmt, args);
  va_end(args);
}

// DEBUGF_QUEUE_DEBUG: shown by the flag and the DEBUG log level.
void debugQueuePrintfDebug(DebugFlagMask flag, const char* fmt, ...) {
  const bool shown = lineShownWithoutCapture(flag, LOG_LEVEL_DEBUG);
  va_list args;
  va_start(args, fmt);
  debugQueueVPrintf(flag, shown, fmt, args);
  va_end(args);
}

// DEBUGF_QUEUE_LEVEL (the INFO and WARN macros): shown by the flag and `level`.
void debugQueuePrintfLevel(DebugFlagMask flag, uint8_t level, const char* fmt, ...) {
  const bool shown = lineShownWithoutCapture(flag, level);
  va_list args;
  va_start(args, fmt);
  debugQueueVPrintf(flag, shown, fmt, args);
  va_end(args);
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
  // The boot debug capture keeps the boot's events (they are shown anyway).
  if (gBootDebugCapturing) bootDebugCaptureLine(line, llen, /*shown=*/true, 0);

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
  // 1. Suppress output of a validation pass, only on the task running it:
  //    a validate request on cmd_exec must not silence other tasks.
  if (currentCommandValidateOnly()) return;

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
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  if (currentTask == gThermalTaskHandle && !gThermalRunning) return;
  if (currentTask == gImuTaskHandle && !gImuRunning) return;
  if (currentTask == gTofTaskHandle && !gTofRunning) return;

  // 4. The boot debug capture keeps every broadcast line of the boot (all of
  //    them are shown, so they go on below as well).
  if (gBootDebugCapturing) bootDebugCaptureLine(text, len, /*shown=*/true, 0);

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

  // 7. ESP-NOW V3 session streaming (extern to avoid circular deps). The
  //    owner decides from the calling task's command context whether, and to
  //    which requester, this line streams.
#if ENABLE_ESPNOW
  if (espnowStreamOutputWanted()) {
    sendEspNowStreamMessage(String(text));
  }
#endif

  // 8. A Bluetooth command's own lines also go to the session that sent it
  //    (BLE_Server.cpp batches them before its result). Lines kept off
  //    Bluetooth, such as the BLE send traces, never reach it.
#if ENABLE_BLUETOOTH
  if (route & MSG_ROUTE_BLE) bleCommandOutputLine(text, len);
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
// share ONE UART writer (log_out). Without it they are two unsynchronized
// writers to the same UART and interleave mid-byte (e.g. "PSRAM" -> "PS7391").
//
// Deliberately dumb: format -> strip ANSI color -> trim trailing newline ->
// direct free-list enqueue. It does NOT go through broadcastOutputCore (so it
// skips the capture buffer, help-gating, and especially the SYNCHRONOUS
// ESP-NOW stream send — none of which are safe to run from the wifi task), and
// it makes NO ESP_LOG calls, so it cannot recurse. ISR / queue-not-ready
// callers fall back to the previous (default) handler. Default OFF; armed via
// the `loglink` command so it can be A/B'd on hardware without reflashing.
//
// The same hook serves the boot debug capture (System_BootDebug.h): while it
// runs every ESP-IDF line is formatted once more here, copied into the boot
// buffer, and printed only when the saved tag levels would have shown it. The
// hook is installed while either wants it (idfLogHookRefresh).
// ============================================================================
static vprintf_like_t s_prevLogVprintf = nullptr;
static volatile bool  s_idfBridgeOn    = false;
static bool           s_idfHookInstalled = false;
static portMUX_TYPE   s_idfHookMux = portMUX_INITIALIZER_UNLOCKED;

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

// The boot debug capture's copy of one line, formatted into `line`. Returns
// false when only the capture's force made the line (do not print it).
static bool idfLogCaptureInto(char* line, size_t cap, const char* fmt, va_list ap, int* cn) {
  const int64_t startUs = esp_timer_get_time();
  va_list copy;
  va_copy(copy, ap);
  *cn = vsnprintf(line, cap, fmt, copy);
  va_end(copy);
  if (*cn < 0) return true;
  size_t len = strnlen(line, cap - 1);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
  stripAnsiInPlace(line);
  len = strlen(line);
  return len == 0 || bootDebugEspLogLine(line, len, startUs);
}

// Only when another task holds the capture's line: format on this stack. Its
// own frame, gone before the previous handler prints.
static bool __attribute__((noinline)) idfLogCaptureOnStack(const char* fmt, va_list ap, int* cn) {
  char line[DEBUG_MSG_SIZE];
  return idfLogCaptureInto(line, sizeof(line), fmt, ap, cn);
}

// ESP-IDF logs from tasks with small stacks (sys_evt, esp_timer): the capture
// formats into the line of its own allocation (bootDebugScratchTake), so the
// frame left under the previous handler is only this one's few words.
static bool __attribute__((noinline)) idfLogCapture(const char* fmt, va_list ap, int* cn) {
  size_t cap = 0;
  char* line = bootDebugScratchTake(&cap);
  if (!line) return idfLogCaptureOnStack(fmt, ap, cn);
  const bool shown = idfLogCaptureInto(line, cap, fmt, ap, cn);
  bootDebugScratchGive();
  return shown;
}

// `loglink`: one ESP-IDF line into the output queue. Its own frame, so the
// line buffer is not on the stack of the plain path.
static int __attribute__((noinline)) idfLogBridgeLine(const char* fmt, va_list ap) {
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

static int idfLogVprintf(const char* fmt, va_list ap) {
  // The boot debug capture: format a copy (ap stays unread for the paths
  // below), keep it, and stop here when only the capture's force made it.
  if (bootDebugEspLogWanted() && !xPortInIsrContext()) {
    int cn = 0;
    if (!idfLogCapture(fmt, ap, &cn)) return cn;
  }
  // Decide BEFORE consuming ap (a va_list can only be walked once). In ISR
  // context, or before the queue exists, behave exactly like the old handler.
  if (!s_idfBridgeOn || xPortInIsrContext() || !gDebugOutputQueue || !gDebugFreeQueue) {
    return s_prevLogVprintf ? s_prevLogVprintf(fmt, ap) : vprintf(fmt, ap);
  }
  return idfLogBridgeLine(fmt, ap);
}

// Installs the hook while the bridge or the boot debug capture wants it and
// puts the previous handler back when neither does.
void idfLogHookRefresh() {
  portENTER_CRITICAL(&s_idfHookMux);
  const bool want = s_idfBridgeOn || bootDebugEspLogWanted();
  const bool install = want && !s_idfHookInstalled;
  const bool uninstall = !want && s_idfHookInstalled;
  if (install || uninstall) s_idfHookInstalled = want;
  portEXIT_CRITICAL(&s_idfHookMux);
  if (install) {
    s_prevLogVprintf = esp_log_set_vprintf(idfLogVprintf);
  } else if (uninstall) {
    esp_log_set_vprintf(s_prevLogVprintf ? s_prevLogVprintf : vprintf);
  }
}

// Install (or restore) the bridge. Arms the flag BEFORE swapping the handler
// so a log racing the install never sees a half-armed state.
static void setIdfLogBridge(bool enable) {
  if (enable && !s_idfBridgeOn) {
    s_idfBridgeOn = true;
    idfLogHookRefresh();
  } else if (!enable && s_idfBridgeOn) {
    s_idfBridgeOn = false;
    idfLogHookRefresh();
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
// Nothing records chunk sizes any more, so the summary names the page only.
static String gStreamTag = "";

void streamDebugReset(const char* tag) {
  gStreamTag = tag ? String(tag) : String("");
}

void streamDebugFlush() {
  // One line per streamed response
  DEBUG_HTTPF("page=%s streamed", gStreamTag.c_str());
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
    v = parseBoolArg(a);
    if (v < 0) return kCommandReplyNotOnOff;
  }
  if (v < 0) {
    // Bare command: show current status
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
    v = parseBoolArg(a);
    if (v < 0) return kCommandReplyNotOnOff;
  }
  if (v < 0) {
    bool enabled = (gOutputFlags & MSG_ROUTE_BLE) != 0;
    bool connected = isBleConnected();
    bool authed = bleHasAuthenticatedSession();
    int recipients = bleMirrorRecipientCount();
    EXT_RAM_BSS_ATTR static char buf[160];
    snprintf(buf, sizeof(buf), "BLE output: %s, BLE connected: %s, authenticated: %s, mirror recipients: %d",
             enabled ? "yes" : "no", connected ? "yes" : "no", authed ? "yes" : "no", recipients);
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
  return "Error: Bluetooth is not in this build. Use a build with ENABLE_BLUETOOTH.";
  #endif
}

void applyLogLevel(uint8_t level) {
  DEBUG_MANAGER.setLogLevel(level);

  // App: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG.
  esp_log_level_t espLevel;
  switch (level) {
    case LOG_LEVEL_ERROR: espLevel = ESP_LOG_ERROR; break;
    case LOG_LEVEL_WARN:  espLevel = ESP_LOG_WARN;  break;
    case LOG_LEVEL_INFO:  espLevel = ESP_LOG_INFO;  break;
    default:             espLevel = ESP_LOG_DEBUG; break;
  }
  bootDebugEspLogSet("wifi", espLevel);
  bootDebugEspLogSet("wifi_init", espLevel);
  bootDebugEspLogSet("phy_init", espLevel);
  applyEspLogOverrides(false);  // a saved esplog entry for these tags wins
}

// Set the ESP-IDF log level for the TLS/HTTPS framework tags. These emit a
// flood of benign noise when a browser hits a self-signed cert (handshake
// rejects, connection resets, per-chunk write errors) — much of it at ERROR
// level, so the global loglevel can't suppress it. verbose=false → NONE,
// verbose=true → DEBUG. Owned by DEBUG_HTTPS.
void applyHttpsLogLevels(bool verbose) {
  esp_log_level_t lvl = verbose ? ESP_LOG_DEBUG : ESP_LOG_NONE;
  bootDebugEspLogSet("esp-tls-mbedtls", lvl);
  bootDebugEspLogSet("esp_https_server", lvl);
  bootDebugEspLogSet("httpd", lvl);
  bootDebugEspLogSet("httpd_txrx", lvl);
  applyEspLogOverrides(false);  // a saved esplog entry for these tags wins
}

// ---- radio stack logs: which tags exist, which flags are on ------------------
// The tag table and its levels live in System_RadioLogCore.h; the apply is
// after cmd_esplog (applyRadioStackLogLevels).
static bool radioLogStackPresent(radiolog::Stack stack) {
  switch (stack) {
    case radiolog::Stack::Hosted:
      return HW1_RADIO_COMPANION;
    case radiolog::Stack::Bluedroid:
#if ENABLE_BLUETOOTH && defined(CONFIG_BT_BLUEDROID_ENABLED) && CONFIG_BT_BLUEDROID_ENABLED
      return true;
#else
      return false;
#endif
    case radiolog::Stack::BleController:
#if ENABLE_BLUETOOTH && defined(CONFIG_BT_CONTROLLER_ENABLED) && CONFIG_BT_CONTROLLER_ENABLED && !CONFIG_IDF_TARGET_ESP32
      return true;
#else
      return false;
#endif
    case radiolog::Stack::BtdmController:
#if ENABLE_BLUETOOTH && defined(CONFIG_BT_CONTROLLER_ENABLED) && CONFIG_BT_CONTROLLER_ENABLED && CONFIG_IDF_TARGET_ESP32
      return true;
#else
      return false;
#endif
    case radiolog::Stack::CoexRecords:
      return ENABLE_BLE_COEX_RECORDS;
  }
  return false;
}
// The runtime layers (`temp` toggles write them); debugverbose forces both,
// as it forces every debug output. The parents (debugwifi, debugbluetooth)
// do not: as for the rest of their families, a parent does not switch its
// children's output on.
static bool radioLogWifiDriverOn() { return gDebugVerbose || gDebugSubFlags.wifiDriver; }
static bool radioLogBluetoothCoreOn() { return isDebugFlagSaved(DEBUG_BLUETOOTH_CORE); }

// ---- esplog: the saved string's lock -----------------------------------------
// gSettings.espLogLevels is written by esplog on the command task and read by
// applyRadioStackLogLevels on the loop task too (each companion TRANSPORT_UP).
// The lock covers each save's reserve and assignment and each radio apply
// whole: an apply never parses a string being reallocated, and two applies
// never interleave, so the last one sets the latest string's and flags' levels.
// The debug.json write after a save runs outside it.
static StaticSemaphore_t gEspLogMutexStorage;
static SemaphoreHandle_t gEspLogMutex = nullptr;
static portMUX_TYPE gEspLogInitMux = portMUX_INITIALIZER_UNLOCKED;

static void espLogLock() {
  if (!gEspLogMutex) {
    portENTER_CRITICAL(&gEspLogInitMux);
    if (!gEspLogMutex) gEspLogMutex = xSemaphoreCreateMutexStatic(&gEspLogMutexStorage);
    portEXIT_CRITICAL(&gEspLogInitMux);
  }
  xSemaphoreTake(gEspLogMutex, portMAX_DELAY);
}
static void espLogUnlock() { xSemaphoreGive(gEspLogMutex); }

// ---- esplog: ESP-IDF log levels by tag, saved -------------------------------
// HTTP_CLIENT's DEBUG lines print request headers, Authorization included;
// System_LLMHttp pins the tag to WARN for each request and refuses one above
// it. An override never raises it past WARN.
static constexpr const char* kEspLogCredentialTag = "HTTP_CLIENT";

static esp_log_level_t espLogLevelFor(const char* tag, int level) {
  if (!strcmp(tag, kEspLogCredentialTag) && level > ESP_LOG_WARN) level = ESP_LOG_WARN;
  return static_cast<esp_log_level_t>(level);
}

// Arduino String leaves a null buffer after a failed allocation.
static const char* espLogStored() {
  const char* stored = gSettings.espLogLevels.c_str();
  return stored ? stored : "";
}

// Saves `next`, or keeps the old value when memory is short: the copy is made
// and the field's room reserved before the assignment, which would otherwise
// invalidate the old value on a failed allocation. Both run under the lock
// (espLogLock); the persistence request after it.
static bool espLogSave(const char* next) {
  const String value(next);
  if (!value.c_str()) return false;
  espLogLock();
  const bool changed = gSettings.espLogLevels != value;
  const bool saved = !changed || gSettings.espLogLevels.reserve(value.length());
  if (changed && saved) gSettings.espLogLevels = value;
  espLogUnlock();
  if (changed && saved) (void)requestDebugSettingsPersist();
  return saved;
}
static constexpr const char* kEspLogNoMemory = "Error: Not enough memory to save tag levels";

void applyEspLogOverrides(bool includeWildcard) {
  const char* stored = espLogStored();
  if (!stored[0]) return;
  if (includeWildcard) {
    const int all = esplog::levelOf(stored, "*");
    if (all >= 0) {
      // Resets every tag's level, the firmware's quieted ones too; the tag
      // entries below go on top. HTTP_CLIENT keeps its ceiling.
      bootDebugEspLogSet("*", static_cast<esp_log_level_t>(all));
      bootDebugEspLogSet(kEspLogCredentialTag, espLogLevelFor(kEspLogCredentialTag, all));
      // "*" dropped every tag's own level: a radio stack tag whose debug flag
      // is on gets its stock level back (the flag wins over the wildcard).
      applyRadioStackLogLevels();
    }
  }
  esplog::forEach(stored, [](const char* tag, size_t len, int level) {
    if (esplog::isWildcard(tag, len)) return;
    char name[esplog::kMaxTagLen + 1];
    memcpy(name, tag, len);
    name[len] = '\0';
    bootDebugEspLogSet(name, espLogLevelFor(name, level));
  });
}

static const char* espLogEditError(esplog::Edit e) {
  switch (e) {
    case esplog::Edit::BadTag:
      return "Error: a tag is 1-31 characters without spaces, commas, colons or quotes (or * for every tag)";
    case esplog::Edit::BadLevel:
      return "Error: level is none, error, warn, info, debug or verbose";
    case esplog::Edit::TooMany:
      return "Error: at most 16 tag levels are saved; remove one with esplog <tag> default";
    case esplog::Edit::TooLong:
      return "Error: the saved tag levels would pass 240 characters; remove one with esplog <tag> default";
    case esplog::Edit::NotFound:
      return "No saved level for that tag";
    case esplog::Edit::Duplicate:
      return "Error: a tag appears twice";
    default:
      return "Error: tag levels not changed";
  }
}

// Debug and verbose lines exist only up to the build's maximum level.
static const char* espLogBuildNote(int level) {
#if defined(CONFIG_LOG_MAXIMUM_LEVEL) && CONFIG_LOG_MAXIMUM_LEVEL >= 0 && CONFIG_LOG_MAXIMUM_LEVEL < 5
  static char note[64];
  if (level > CONFIG_LOG_MAXIMUM_LEVEL) {
    snprintf(note, sizeof(note), " (this build compiles ESP-IDF lines only up to %s)",
             esplog::kNames[CONFIG_LOG_MAXIMUM_LEVEL]);
    return note;
  }
#else
  (void)level;
#endif
  return "";
}

const char* cmd_esplog(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  CommandArgs ca(argsInput);
  const char* stored = espLogStored();

  if (ca.count() == 0) {
    size_t used = (size_t)snprintf(gDebugBuffer, 1024, "Saved ESP-IDF log levels (esplog):");
    const size_t n = esplog::forEach(stored, [&](const char* tag, size_t len, int level) {
      if (used >= 1024) return;
      const int w = snprintf(gDebugBuffer + used, 1024 - used, "\n  %.*s %s", (int)len, tag,
                             esplog::kNames[level]);
      used += w > 0 ? (size_t)w : 0;
      if (used > 1023) used = 1023;  // truncated: the rest is dropped
    });
    if (n == 0) snprintf(gDebugBuffer + used, 1024 - used, " none (the firmware's own levels apply)");
    return gDebugBuffer;
  }

  static constexpr const char* kUsage =
      "Usage: esplog [<tag|*> <none|error|warn|info|debug|verbose|default>] | esplog clear";
  char next[esplog::kMaxStoredLen + 1];
  const String tag = ca.arg(0);
  if (ca.count() == 1 && tag.equalsIgnoreCase("clear")) {
    if (!stored[0]) return "No saved ESP-IDF log levels";
    if (!espLogSave("")) return kEspLogNoMemory;
    applyRadioStackLogLevels();
    return "Saved ESP-IDF log levels cleared; the radio stack tags follow their debug flags again, the "
           "firmware's other levels return at the next reboot";
  }
  // The settings editors send the whole "tag:L,tag:L" string; it replaces
  // every entry.
  if (tag.indexOf(':') >= 0) {
    const esplog::Edit e = esplog::replaceAll(ca.raw().c_str(), next, sizeof(next));
    if (e != esplog::Edit::Ok) return espLogEditError(e);
    if (!strcmp(next, stored)) return "ESP-IDF log levels unchanged";
    if (esplog::levelOf(next, kEspLogCredentialTag) > ESP_LOG_WARN) {
      return "Error: HTTP_CLIENT stays at warn or below (its debug lines print request headers)";
    }
    if (!espLogSave(next)) return kEspLogNoMemory;
    applyRadioStackLogLevels();  // a removed radio stack tag follows its debug flag again
    applyEspLogOverrides(true);
    snprintf(gDebugBuffer, 1024, "ESP-IDF log levels saved: %s; removed entries return to the "
             "firmware's level at the next reboot", next[0] ? next : "(none)");
    return gDebugBuffer;
  }
  if (ca.count() != 2) return kUsage;

  const String levelText = ca.arg(1);
  if (levelText.equalsIgnoreCase("default")) {
    const esplog::Edit e = esplog::remove(stored, tag.c_str(), next, sizeof(next));
    if (e != esplog::Edit::Ok) return espLogEditError(e);
    if (!espLogSave(next)) return kEspLogNoMemory;
    if (radiolog::find(tag.c_str())) {
      applyRadioStackLogLevels();
      snprintf(gDebugBuffer, 1024, "%s: saved level removed; it follows its debug flag again", tag.c_str());
    } else if (esplog::isWildcard(tag.c_str(), tag.length())) {
      applyRadioStackLogLevels();
      snprintf(gDebugBuffer, 1024, "*: saved level removed; the radio stack tags follow their debug flags "
               "again, the other tags keep it until the next reboot");
    } else {
      snprintf(gDebugBuffer, 1024, "%s: saved level removed; its firmware level returns at the next reboot",
               tag.c_str());
    }
    return gDebugBuffer;
  }
  const int level = esplog::parseLevel(levelText.c_str());
  if (level < 0) return espLogEditError(esplog::Edit::BadLevel);
  if (!strcmp(tag.c_str(), kEspLogCredentialTag) && level > ESP_LOG_WARN) {
    return "Error: HTTP_CLIENT stays at warn or below (its debug lines print request headers)";
  }
  const esplog::Edit e = esplog::set(stored, tag.c_str(), level, next, sizeof(next));
  if (e != esplog::Edit::Ok && e != esplog::Edit::Unchanged) return espLogEditError(e);
  if (!espLogSave(next)) return kEspLogNoMemory;
  if (esplog::isWildcard(tag.c_str(), tag.length())) {
    applyEspLogOverrides(true);
    snprintf(gDebugBuffer, 1024,
             "*: %s (saved). Every tag was set, the firmware's quieted ones too; saved tag levels "
             "were applied again%s", esplog::kNames[level], espLogBuildNote(level));
  } else {
    bootDebugEspLogSet(tag.c_str(), espLogLevelFor(tag.c_str(), level));
    snprintf(gDebugBuffer, 1024, "%s: %s (saved)%s", tag.c_str(), esplog::kNames[level],
             espLogBuildNote(level));
  }
  return gDebugBuffer;
}

// ---- radio stack logs: apply --------------------------------------------------
// Sets every radio stack tag of this build (System_RadioLogCore.h): its saved
// esplog level, else its stock level while its debug flag is on, else the
// saved wildcard, else ERROR. Called at boot (applySettings), on
// debugwifidriver, debugbluetoothcore and debugverbose, after the wildcard is
// applied, when an esplog entry is removed, and on each companion
// TRANSPORT_UP and failure (esp_hosted_init sets rpc_core/rpc_rsp/rpc_evt to
// WARN for every new Hosted session).
void applyRadioStackLogLevels() {
  espLogLock();  // the loop task applies too: see espLogLock
  const char* stored = espLogStored();
  const bool wifiDriver = radioLogWifiDriverOn();
  const bool bluetoothCore = radioLogBluetoothCoreOn();
  for (const radiolog::TagRow& row : radiolog::kTags) {
    if (!radioLogStackPresent(row.stack)) continue;
    bootDebugEspLogSet(row.tag, espLogLevelFor(row.tag, radiolog::levelFor(row, wifiDriver, bluetoothCore, stored)));
  }
  espLogUnlock();
}

// A path that replaces the whole live mask (`log start flags=`, the saved
// system-log mask at `log start`, `log autostart` and `systemlogflags`) runs
// what the per-flag commands run: the radio stack tags follow
// debugbluetoothcore, and the HTTPS tags debughttps when its bit changed (an
// unchanged bit leaves a saved esplog wildcard's level on them).
void applyDebugMaskLogLevels(DebugFlagMask before) {
  applyRadioStackLogLevels();
  if (((before ^ gDebugFlags) & DEBUG_HTTPS) != (DebugFlagMask)0) applyHttpsLogLevels(isDebugFlagSaved(DEBUG_HTTPS));
}

const char* cmd_debugverbose(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  if (ca.count() == 0) return gDebugVerbose ? "debugVerbose is ON" : "debugVerbose is OFF";
  const int v = parseBoolArg(ca.arg(0));
  if (v < 0) return kCommandReplyNotOnOff;
  gDebugVerbose = (v == 1);
  gDebugLineForce = gDebugVerbose || bootDebugForceWanted();
  applyRadioStackLogLevels();  // verbose shows the radio stacks' routine lines too
  return gDebugVerbose ? "debugVerbose enabled" : "debugVerbose disabled";
}

const char* cmd_webconsole(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  const int v = parseBoolArg(valStr);
  if (v < 0) return kCommandReplyNotOnOff;
  setDebugSetting(gSettings.webConsoleDebug, v == 1);
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
  int v = 0;
  if (!parseIntArg(valStr, v)) return kCommandReplyNotANumber;
  if (v < 0 || v > 300) return "Error: Interval must be 0 to 300 seconds. Use 0 to turn sampling off.";
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
  char flagsText[80];
  formatSystemLogFlags(flags, flagsText, sizeof(flagsText));
  BROADCAST_PRINTF("Debug flags: %s", flagsText);

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
  bool on  = argIsOn(a);
  bool off = argIsOff(a);
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
// same per-family terms. Only families in that list are ever recomputed; every
// other parent is an explicit master switch owned by its toggle.
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
    case DBG_AGG_SETTINGS_BITS:  // family settings OR temp-set child bits (BT, SR)
      any = dbgAnyFamilySetting(root) || dbgAnyRuntimeChildBit(root);
      break;
  }
  updateParentDebugFlag(kDbgMask[root], any);
}

// The bits of the aggregated family roots in a bank: a persistent toggle of
// any family member may have recomputed them.
static DebugFlagMask dbgFamilyRootMask(DbgBank bank) {
  DebugFlagMask roots = (DebugFlagMask)0;
  for (int i = 0; i < DBG_FLAG_COUNT; ++i)
    if (kDbgBank[i] == bank && kDbgAggMode.m[i] != DBG_AGG_NONE) roots = roots | kDbgMask[i];
  return roots;
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
// parent) with ONE deliberate change: a STRICT 0|1 contract — a missing value
// or anything that is not 0/1 (on/off) is rejected with the same error shape
// the already-strict handlers used. The
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
      // gCliValidateOnly is false here — guarded anyway to match pre-C3).
      if (!gCliValidateOnly) applyHttpsLogLevels(isDebugFlagSaved(DEBUG_HTTPS));
      break;
    }
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
      // debugbluetoothcore also owns the Bluetooth stack's own log tags
      // (Bluedroid, BLE_COEX, the C6 console): stock level on, ERROR off.
      if (idx == DBG_BLUETOOTH_CORE && !gCliValidateOnly) applyRadioStackLogLevels();
      break;
    default:
      break;
  }
}

// A second word other than temp or runtime is refused; it is not read as "save".
static constexpr char kDbgUnknownMode[] =
    "Error: Unknown mode. Use temp or runtime, or leave it out to save the change.";

// Shared dispatcher for every flag-backed debug command (parent or bit-sub).
// Reproduces cmd_debugsubflag_impl exactly, plus the strict 0|1 contract and
// the dbgApplyHook side effect.
static const char* dbgFlagCmd(DbgFlagIdx idx, const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[96];
  CommandArgs ca(argsInput);
  String mode = ca.arg(1);
  bool modeTemp = (mode.equalsIgnoreCase("temp") || mode.equalsIgnoreCase("runtime"));
  // A missing value or one that is not 0/1 (on/off) is refused: it never
  // switches the flag off.
  const int v = ca.has(0) ? parseBoolArg(ca.arg(0)) : -1;
  if (v < 0) {
    snprintf(buf, sizeof(buf), "Error: invalid arguments — Usage: %s <0|1> [temp|runtime]", kDbgCmdIdent[idx]);
    return buf;
  }
  if (ca.count() > 2 || (ca.has(1) && !modeTemp)) return kDbgUnknownMode;
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
  systemLogFollowFlagChange(flagBit | dbgFamilyRootMask(kDbgBank[idx]));
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
  const int v = ca.has(0) ? parseBoolArg(ca.arg(0)) : -1;
  if (v < 0) {
    snprintf(buf, sizeof(buf), "Error: invalid arguments — Usage: %s <0|1> [temp|runtime]", kDbgSubCmdIdent[sidx]);
    return buf;
  }
  if (ca.count() > 2 || (ca.has(1) && !modeTemp)) return kDbgUnknownMode;
  *kDbgSubRuntimePtr[sidx] = (v != 0);
  if (!modeTemp) setDebugSetting(*kDbgSubSettingPtr[sidx], (bool)(v != 0));
  dbgRecomputeParent(kDbgSubParentIdx[sidx]);
  // debugwifidriver also owns ESP-Hosted's and the C6 console's log tags
  // (System_RadioLogCore.h): stock level on, ERROR off, on both paths.
  if (sidx == DBG_SUB_WIFI_DRIVER) applyRadioStackLogLevels();
  if (modeTemp) {
    snprintf(buf, sizeof(buf), "%s %s (runtime only)", kDbgSubRetName[sidx], v ? "enabled" : "disabled");
    return buf;
  }
  systemLogFollowFlagChange(kDbgMask[kDbgSubParentIdx[sidx]]);
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
// string (up to 64 digits) or one to four colon-separated 64-bit words
// high-to-low (up to 16 digits each). A word may lead with 0x. Anything else
// (an empty word, a character that is not a hex digit, too many digits or
// words) fails and leaves `out` untouched.
bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out) {
  auto hexValue = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  const char* begin = flagsStr.c_str();
  const char* end = begin + strlen(begin);
  while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
  const bool colonForm = memchr(begin, ':', (size_t)(end - begin)) != nullptr;
  const size_t maxDigits = colonForm ? 16 : 64;
  uint64_t w[4] = {0, 0, 0, 0};  // w[0] = bits 0-63 ... w[3] = bits 192-255
  // Walk the words from the last (lowest) to the first.
  const char* wordEnd = end;
  for (int word = 0;; ++word) {
    if (word >= 4) return false;
    const char* wordBegin = begin;
    if (colonForm) {
      wordBegin = wordEnd;
      while (wordBegin > begin && wordBegin[-1] != ':') --wordBegin;
    }
    const char* digits = wordBegin;
    if (wordEnd - digits >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits += 2;
    const size_t count = (size_t)(wordEnd - digits);
    if (count == 0 || count > maxDigits) return false;
    for (size_t i = 0; i < count; ++i) {
      const int value = hexValue(*(wordEnd - 1 - i));
      if (value < 0) return false;
      const size_t nibble = (size_t)word * 16 + i;
      w[nibble / 16] |= (uint64_t)value << (4 * (nibble % 16));
    }
    if (wordBegin == begin) break;
    wordEnd = wordBegin - 1;  // step over the colon
  }
  out = ((DebugFlagMask)w[3] << 192) | ((DebugFlagMask)w[2] << 128) |
        ((DebugFlagMask)w[1] << 64)  | (DebugFlagMask)w[0];
  return true;
}

void formatSystemLogFlags(DebugFlagMask mask, char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  snprintf(out, outSize, "0x%016llX:%016llX:%016llX:%016llX",
           (unsigned long long)(uint64_t)(mask >> 192),
           (unsigned long long)(uint64_t)(mask >> 128),
           (unsigned long long)(uint64_t)(mask >> 64),
           (unsigned long long)(uint64_t)mask);
}

String formatSystemLogFlags(DebugFlagMask mask) {
  char buf[80];
  formatSystemLogFlags(mask, buf, sizeof(buf));
  return String(buf);
}

bool systemLogApplyPersistedFlags() {
  if (gSettings.systemLogFlags.length() == 0) return false;
  DebugFlagMask restored = (DebugFlagMask)0;
  if (!parseSystemLogFlags(gSettings.systemLogFlags, restored)) return false;
  const DebugFlagMask before = gDebugFlags;
  gDebugFlags = restored;
  applyDebugMaskLogLevels(before);
  return true;
}

// A persistent per-flag change (`debug<x> 0|1`) also lands in the saved
// system-log mask, when one is saved. `log autostart` and a plain `log start`
// apply that mask as a whole, so a category turned off for good came back at
// the next boot. `touched` names the bits the command owns or may have
// recomputed; their live values are copied. Runtime-only (`temp`) changes
// never reach here.
bool systemLogFollowFlagChange(DebugFlagMask touched) {
  if (gSettings.systemLogFlags.length() == 0) return false;  // nothing saved: the per-flag settings stand alone
  DebugFlagMask saved = (DebugFlagMask)0;
  if (!parseSystemLogFlags(gSettings.systemLogFlags, saved)) return false;
  const DebugFlagMask merged = (saved & ~touched) | (gDebugFlags & touched);
  if (merged == saved) return false;
  setSetting(gSettings.systemLogFlags, formatSystemLogFlags(merged));
  return true;
}

DebugFlagMask systemLogConfiguredFlags() {
  DebugFlagMask saved = (DebugFlagMask)0;
  if (gSettings.systemLogFlags.length() > 0 &&
      parseSystemLogFlags(gSettings.systemLogFlags, saved)) {
    return saved;
  }
  return gDebugFlags;  // nothing saved: `log start` keeps the live mask
}

SystemLogState systemLogState() {
  if (!gSystemLogRunning) return SystemLogState::Inactive;
  return (gOutputFlags & MSG_ROUTE_FILE) ? SystemLogState::Active : SystemLogState::Configured;
}

// One reading of the system-log status; the text and json forms print from it.
// `path` is borrowed from the logger and valid only inside the call that read
// it (the formatters below read and print in one call).
struct SystemLogStatus {
  SystemLogState state;
  bool running;              // the flag `log start` / `log stop` check
  const char* path;
  unsigned long lastWriteAgoSec;
  unsigned outputFlags;
  bool autoStart;
  unsigned maxSizeKb;
  int keep;                  // old files kept at rotation
  unsigned long rotations;   // this boot
  unsigned long reserveSkips;
  unsigned reserveKb;
};

static void systemLogReadStatus(SystemLogStatus& st) {
  st.state = systemLogState();
  st.running = gSystemLogRunning;
  // B11: the file the lines go to (the card twin while a card takes them).
  st.path = gSystemLogActivePath.length() ? gSystemLogActivePath.c_str() : gSystemLogPath.c_str();
  st.lastWriteAgoSec = (millis() - gSystemLogLastWrite) / 1000;
  st.outputFlags = (unsigned)gOutputFlags;
  st.autoStart = gSettings.systemLogAutoStart;
  st.maxSizeKb = (unsigned)(systemLogMaxSizeBytes() / 1024u);
  st.keep = systemLogMaxRotations();
  st.rotations = (unsigned long)gSystemLogRotations;
  st.reserveSkips = (unsigned long)gSystemLogReserveSkips;
  st.reserveKb = (unsigned)(VFS::logReserveBytes() / 1024u);
}

static void systemLogFormatStatusText(const SystemLogStatus& st, char* out, size_t outSize) {
  if (st.state == SystemLogState::Active) {
    snprintf(out, outSize,
             "System logging ACTIVE\n"
             "  File: %s\n"
             "  Last write: %lus ago\n"
             "  Output flags: 0x%02X\n"
             "  Auto-start: %s\n"
             "  Rotation: at %u KB, keep %d old file(s); rotated %lu time(s) this boot",
             st.path, st.lastWriteAgoSec, st.outputFlags,
             st.autoStart ? "ON" : "OFF",
             st.maxSizeKb, st.keep, st.rotations);
    if (st.reserveSkips > 0) {
      const size_t used = strnlen(out, outSize);
      snprintf(out + used, outSize - used,
               "\n  Skipped: %lu line(s) while flash was below its %u KB reserve",
               st.reserveSkips, st.reserveKb);
    }
  } else if (st.state == SystemLogState::Configured) {
    snprintf(out, outSize,
             "System logging CONFIGURED but MSG_ROUTE_FILE flag not set\n"
             "  File: %s\n"
             "  Use 'log start' to enable\n"
             "  Auto-start: %s",
             st.path,
             st.autoStart ? "ON" : "OFF");
  } else {
    snprintf(out, outSize, "System logging is INACTIVE\n  Auto-start: %s",
             st.autoStart ? "ON" : "OFF");
  }
}

void formatSystemLogStatus(char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  SystemLogStatus st;
  systemLogReadStatus(st);
  systemLogFormatStatusText(st, out, outSize);
}

static bool systemLogFormatStatusJson(const SystemLogStatus& st, char* out, size_t outSize) {
  size_t pos = 0;
  bool fits = true;
  // Accounts one snprintf result; a write that does not fit ends the document.
  auto wrote = [&](int n) {
    if (!fits) return;
    if (n < 0 || (size_t)n >= outSize - pos) fits = false;
    else pos += (size_t)n;
  };
  wrote(snprintf(out + pos, outSize - pos, "{\"schema\":1,\"state\":\"%s\",\"running\":%s",
                 systemLogStateKey(st.state), st.running ? "true" : "false"));
  if (fits && st.running) {
    wrote(snprintf(out + pos, outSize - pos, ",\"file\":\""));
    if (fits) {
      // jsonEscapeInto stops at what fits; a shortened path is refused here.
      if (jsonEscapedLength(st.path, strlen(st.path)) >= outSize - pos) fits = false;
      else pos += jsonEscapeInto(out + pos, outSize - pos, st.path);
    }
    if (fits) wrote(snprintf(out + pos, outSize - pos, "\""));
  }
  if (fits && st.state == SystemLogState::Active) {
    wrote(snprintf(out + pos, outSize - pos, ",\"lastWriteAgoSec\":%lu,\"outputFlags\":%u",
                   st.lastWriteAgoSec, st.outputFlags));
  }
  if (fits) {
    wrote(snprintf(out + pos, outSize - pos,
                   ",\"autoStart\":%s,\"maxSizeKb\":%u,\"keep\":%d,\"rotations\":%lu,"
                   "\"reserveSkips\":%lu,\"reserveKb\":%u}",
                   st.autoStart ? "true" : "false", st.maxSizeKb, st.keep, st.rotations,
                   st.reserveSkips, st.reserveKb));
  }
  if (!fits) snprintf(out, outSize, "{\"schema\":1,\"ok\":false,\"error\":\"response too large\"}");
  return fits;
}

bool formatSystemLogStatusJson(char* out, size_t outSize) {
  if (!out || outSize == 0) return false;
  SystemLogStatus st;
  systemLogReadStatus(st);
  return systemLogFormatStatusJson(st, out, outSize);
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
           "  status [json]: Show current logging status (json: one JSON object)\n"
           "  maxsize <bytes> | rotations <0-9>: Rotate the file at this size, keeping that many old files\n"
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
  
  // Handle 'status' subcommand: text, or one JSON object with 'status json'.
  if (subCmd == "status") {
    String form = ca.arg(1);
    form.toLowerCase();
    if (form == "json") {
      (void)formatSystemLogStatusJson(gDebugBuffer, 1024);
      return gDebugBuffer;
    }
    formatSystemLogStatus(gDebugBuffer, 1024);
    return gDebugBuffer;
  }
  
  // Handle 'stop' subcommand
  if (subCmd == "stop") {
    if (!gSystemLogRunning) {
      return "Error: System logging is not running. Start it with 'log start'.";
    }
    
    // Close the handle and retire the path under ONE lock hold. The debug
    // task re-checks the running flag and the path under the same lock before
    // it reopens, so it can never see the file closed but the path not yet
    // cleared (which used to reopen an empty path and log a [PERM] denial).
    String msg;
    fsLock("debug.log");
    systemLogCloseHandleLocked();  // a card file is verified first (B11)
    gSystemLogRunning = false;
    gOutputFlags &= ~MSG_ROUTE_FILE;
    msg = "System logging stopped. Log saved to: " +
          (gSystemLogActivePath.length() ? gSystemLogActivePath : gSystemLogPath);
    gSystemLogPath = "";
    gSystemLogActivePath = "";
    fsUnlock();
    snprintf(gDebugBuffer, 1024, "%s", msg.c_str());
    return gDebugBuffer;
  }
  
  // Handle 'start' subcommand
  if (subCmd == "start") {
    if (gSystemLogRunning) {
      return "Error: System logging is already running. Run log stop first.";
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
          return "Error: flags= is not a hex mask. Give up to 64 hex digits, for example flags=0x15.";
        }
        flagsSet = true;
      } else if (token.startsWith("tags=")) {
        const int tags = parseBoolArg(token.substring(5));
        if (tags < 0) return kCommandReplyNotOnOff;
        categoryTags = tags;
      } else if (token.length() > 0 && !hasFilepath) {
        // The log path must be a quoted token (uniform quoted-path rule); a bare
        // token here is almost certainly an unquoted path.
        if (!ca.argWasQuoted(i))
          return "Error: log path must be in quotes, e.g. log start \"/logging_captures/system/sys.txt\"";
        filepath = token;
        hasFilepath = true;
      } else {
        return "Error: Unknown log start argument. Use a quoted path, flags=0x... or tags=0|1.";
      }
    }

    if (!hasFilepath) {
      // Use persisted path if set, otherwise auto-generate
      filepath = (gSettings.systemLogPath.length() > 0) ? gSettings.systemLogPath : generateSystemLogFilename();
    }
    
    if (filepath.length() == 0 || filepath.charAt(0) != '/') {
      return "Error: Filepath must start with / (e.g., /logging_captures/system/system.log)";
    }
    // The system log is written by the system identity; confine it to the
    // capture tree (either tier) so an admin-typed path can never point that
    // identity at the roster, settings or the audit logs.
    if (!VFS::pathInCaptureTree(filepath)) {
      return "Error: Filepath must be inside " CAPTURE_DIR " or /sd" CAPTURE_DIR
             " (e.g., " CAPTURE_DIR_SYSTEM "/system.log)";
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
      if (!VFS::existsGuarded(dir, VFS::systemAuth(VFS::captureScopeFor(filepath), "debug.log_setup_mkdir"))) {
        fsLock("log.mkdir");
        for (int i = 1; i <= (int)dir.length(); i++) {
          if (i == (int)dir.length() || dir.charAt(i) == '/') {
            String parent = dir.substring(0, i);
            if (parent.length() > 0 && !VFS::existsGuarded(parent, VFS::systemAuth(VFS::captureScopeFor(filepath), "debug.log_setup_mkdir"))) {
              VFS::mkdirGuarded(parent, VFS::systemAuth(VFS::captureScopeFor(filepath), "debug.log_setup_mkdir"));
            }
          }
        }
        fsUnlock();
        if (!VFS::existsGuarded(dir, VFS::systemAuth(VFS::captureScopeFor(filepath), "debug.log_setup_mkdir"))) {
          snprintf(gDebugBuffer, 1024, "Error: Failed to create directory: %s", dir.c_str());
          return gDebugBuffer;
        }
        broadcastOutput("Created directory: " + dir);
      }
    }

    // Create the file if needed, where its first line will go (B11): the
    // card twin, sealed, while a card takes log lines, else the path itself.
    String startedFile;
    fsLock("log.create");
    {
      bool isNew = false;
      size_t stored = 0;
      File f = systemLogOpenLocked(filepath, startedFile, &isNew, false, &stored);
      if (!f) {
        fsUnlock();
        snprintf(gDebugBuffer, 1024, "Error: Failed to create file: %s", startedFile.c_str());
        return gDebugBuffer;
      }
      if (isNew) systemLogWriteStartLine(f, startedFile, "started", stored);
      f.close();
    }
    fsUnlock();

    // The file exists: only now change the live and saved mask and the tags
    // setting, so a start that failed above leaves them as they were.
    // Apply debug flags if specified; otherwise restore last-used system-log mask
    if (flagsSet) {
      const DebugFlagMask before = gDebugFlags;
      gDebugFlags = debugFlags;
      applyDebugMaskLogLevels(before);
      setSetting(gSettings.systemLogFlags, formatSystemLogFlags(debugFlags));
      char flagsText[80];
      formatSystemLogFlags(gDebugFlags, flagsText, sizeof(flagsText));
      char flagsMsg[160];
      snprintf(flagsMsg, sizeof(flagsMsg), "Debug flags set to: %s", flagsText);
      broadcastOutput(flagsMsg);
    } else {
      systemLogApplyPersistedFlags();
    }
    
    // Apply category tags setting (arg overrides, otherwise use persisted setting)
    if (categoryTags >= 0) {
      gSystemLogCategoryTags = (categoryTags != 0);
      setSetting(gSettings.systemLogCategoryTags, gSystemLogCategoryTags);
    } else {
      gSystemLogCategoryTags = gSettings.systemLogCategoryTags;
    }
    
    gSystemLogPath = filepath;
    gSystemLogActivePath = startedFile;
    gSystemLogRunning = true;
    gSystemLogLastWrite = millis();
    gOutputFlags |= MSG_ROUTE_FILE;
    setSetting(gSettings.systemLogPath, filepath);

    snprintf(gDebugBuffer, 1024, "System logging started\n  File: %s%s", startedFile.c_str(),
             SealedLog::appliesTo(startedFile) ? " (on the card, sealed)" : "");
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
    } else if (argIsOn(mode)) {
      newValue = true;
    } else if (argIsOff(mode)) {
      newValue = false;
    } else {
      return "Error: invalid arguments — Usage: log autostart [on|off]  (bare = toggle)";
    }
    setSetting(gSettings.systemLogAutoStart, newValue);
    snprintf(gDebugBuffer, 1024, "System log auto-start %s", newValue ? "ENABLED" : "DISABLED");
    return gDebugBuffer;
  }
  
  // Size limit and kept generations: the sensor logger's `sensorlog maxsize`
  // and `sensorlog rotations`, with the same bounds, applied by the writer.
  if (subCmd == "maxsize") {
    if (!ca.has(1)) {
      snprintf(gDebugBuffer, 1024, "Current max size: %u bytes\nUsage: log maxsize <bytes> (10240-10485760)",
               (unsigned)systemLogMaxSizeBytes());
      return gDebugBuffer;
    }
    long newSize = 0;
    if (!parseIntArg(ca.arg(1), newSize)) return kCommandReplyNotANumber;
    if (newSize < 10240) return "Error: Max size must be at least 10240 bytes (10KB)";
    if (newSize > 10485760) return "Error: Max size cannot exceed 10485760 bytes (10MB)";
    setSetting(gSettings.systemLogMaxSize, (int)newSize);
    snprintf(gDebugBuffer, 1024, "System log rotates at %ld bytes", newSize);
    return gDebugBuffer;
  }
  if (subCmd == "rotations") {
    if (!ca.has(1)) {
      snprintf(gDebugBuffer, 1024, "Current rotations: %d\nUsage: log rotations <count> (0-9)\n"
                                   "Set to 0 to start over at the limit and keep no old log",
               systemLogMaxRotations());
      return gDebugBuffer;
    }
    int count = 0;
    if (!parseIntArg(ca.arg(1), count)) return kCommandReplyNotANumber;
    if (count < 0 || count > 9) return "Error: Rotation count must be 0-9";
    setSetting(gSettings.systemLogMaxRotations, count);
    if (count == 0) return "Rotation disabled - the log starts over at the size limit";
    snprintf(gDebugBuffer, 1024, "Will keep up to %d old log file%s (.1, .2, etc.)",
             count, count > 1 ? "s" : "");
    return gDebugBuffer;
  }

  return "Error: Unknown subcommand. Use: start, stop, status, autostart, maxsize or rotations";
}

// Low-level stack/heap trace toggle (bypasses debug queue, writes directly
// to Serial). Use only while diagnosing stack-overflow / queue-saturation
// issues — Serial.printf is synchronous and slows heavy paths if left on.
const char* cmd_debugstack(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String s = ca.arg(0);
  if (argIsOn(s)) {
    gDebugStackTraceEnabled = true;
    return "debugstack ON — STACK_TRACEF messages will print directly to Serial";
  }
  if (argIsOff(s)) {
    gDebugStackTraceEnabled = false;
    return "debugstack OFF";
  }
  if (ca.count() > 0) return kCommandReplyNotOnOff;
  return gDebugStackTraceEnabled ? "debugstack is currently ON" : "debugstack is currently OFF";
}

// ============================================================================
// Debug Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry debugCommands[] = {
  { "debughttp", "Debug HTTP requests.", true, cmd_debughttp, "Usage: debughttp <0|1> [temp|runtime]" },
  { "debughttps", "Debug HTTPS/TLS handshake + connection errors (ESP-IDF logs).", true, cmd_debughttps, "Usage: debughttps <0|1> [temp|runtime]" },
  { "debugsse", "Debug Server-Sent Events.", true, cmd_debugsse, "Usage: debugsse <0|1> [temp|runtime]" },
  { "debugcli", "Debug CLI processing.", true, cmd_debugcli, "Usage: debugcli <0|1> [temp|runtime]" },
  { "debugauth", "Debug authentication (parent flag).", true, cmd_debugauth, "Usage: debugauth <0|1> [temp|runtime]" },
  { "debugbluetooth", "Debug Bluetooth (parent flag).", true, cmd_debugbluetooth, "Usage: debugbluetooth <0|1> [temp|runtime]" },
  { "debugbluetoothcore", "Debug Bluetooth core lifecycle; also shows the Bluetooth stack's own routine log lines (Bluedroid BT_*, BLE_COEX records, the P4X-EYE's C6 console), which print only errors while it is off.", true, cmd_debugbluetoothcore,
    "Usage: debugbluetoothcore <0|1> [temp|runtime]\n"
    "  On: BT_APPL, BT_BTM, BT_HCI, BT_L2CAP, BT_GATT, BT_SMP, BT_BTC, BLE_INIT, BTDM_INIT, BLE_COEX and C6> at their stock level (C6> while the console mirror runs, see c6console).\n"
    "  Off (default): errors only. debugbluetooth alone does not change them; a saved esplog level wins." },
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
  { "debugimu", "Debug the IMU (motion sensor).", true, cmd_debugimu, "Usage: debugimu <0|1> [temp|runtime]" },
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
  { "debugwifidriver", "Debug WiFi driver; on the P4X-EYE also shows ESP-Hosted's and the C6 console's routine log lines, which print only errors while it is off.", true, cmd_debugwifidriver,
    "Usage: debugwifidriver <0|1> [temp|runtime]\n"
    "  On: transport, H_SDIO_DRV, sdio_wrapper, H_API, RPC_WRAP, rpc_*, vhci_drv, os_wrapper_esp, esp_cli, esp_wifi_remote\n"
    "  and C6> at their stock level (C6> while the console mirror runs, see c6console). Off (default): errors only. debugwifi alone does not change them; a saved esplog level wins." },
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
  { "esplog", "ESP-IDF log levels by tag, saved across reboots (loglevel covers only HardwareOne's own log).", true, cmd_esplog,
    "Usage: esplog [<tag|*> <none|error|warn|info|debug|verbose|default>] | esplog clear\n"
    "  (bare): list the saved tag levels\n"
    "  <tag> <level>: set and save, e.g. esplog BT_BTM error, esplog rpc_utils warn\n"
    "  <tag> default: remove the saved level (a radio stack tag follows its debug flag again at once,\n"
    "    any other tag gets the firmware's level back at the next reboot)\n"
    "  * <level>: every tag, the firmware's quieted ones too; tag entries go on top\n"
    "  clear: remove every saved level" },
  { "log", "System-wide logging to file.", true, cmd_log, "Usage: log <start|stop|status|autostart>\n  start [\"filepath\"] [flags=0x...] [tags=0|1]: Begin system logging\n    filepath: Log file path, quoted (auto-generated if omitted)\n    flags: Debug flag mask, up to 64 hex digits (bit map in System_Debug.h)\n    tags: Prefix lines with category tags (0|1, default 1)\n  stop / status [json]: Stop logging / show logging status (json: one JSON object)\n  maxsize <bytes> / rotations <0-9>: Rotate the file at this size, keeping that many old files (.1, .2, ...)\n  autostart [on|off]: Toggle logging auto-start on boot (bare = toggle)" },
  { "webconsole", "Enable/disable browser-side debug console output in the web UI.", true, cmd_webconsole, "Usage: webconsole <0|1>" },
  { "bootdebug", "Boot debug file: every debug output on during boot, saved as boot-debug-1.log (on by default).", true, cmd_bootdebug,
    "Usage: bootdebug [on|off]\n"
    "  (bare): the setting, and where this boot's file went\n"
    "  on|off: save the setting; it applies from the next boot (off: no forcing, no capture, fastest boot)" },
};

const size_t debugCommandsCount = sizeof(debugCommands) / sizeof(debugCommands[0]);

// C3 drift tripwire. The flag/sub command handlers are generated thunks: 126
// flag rows (DBG_FLAG_COUNT minus the ALWAYS control row that carries
// DBG_NO_CMD) + 40 bitless subs = 166. The rows enumerated below stay
// hand-written. Adding a flag/sub row to the tables, or a hand row here,
// without reconciling the counts fails this assert. The subtractions track the
// two feature-gated table spans: 6 LLM flag rows, 7 debugg2* + 7 debugring*
// flag rows (both under ENABLE_BLUETOOTH && ENABLE_G2_GLASSES), and the
// outg2 hand row.
static constexpr size_t kDbgHandCmdRows = 16;  // notifstats,
  // debugverbose, debugbuffer, debugflags, loglink, debugstack,
  // commandmodulesummary, settingsmodulesummary, outg2, outble,
  // memorysampleintervalsec, loglevel, esplog, log, webconsole, bootdebug
static constexpr size_t kDbgGenCmdRowsInTable =
    (size_t)(DBG_FLAG_COUNT - 1) + (size_t)DBG_SUBBOOL_COUNT
#if !ENABLE_LLM_SOURCE_ONBOARD
    - 6
#endif
#if !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)
    - 14  // 7 debugg2* + 7 debugring*
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
// DebugManager Class Implementation (merged from System_DebugManager.cpp)
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

// Time sync marker flag
bool gTimeSyncedMarkerWritten = false;

// Log File Path Definitions
extern const char* const LOG_OK_FILE = "/system/sys_logs/successful_login.log";              // ~680KB cap
extern const char* const LOG_FAIL_FILE = "/system/sys_logs/failed_login.log";                // ~680KB cap
extern const char* const LOG_I2C_FILE = "/system/sys_logs/i2c_errors.log";                   // 64KB cap
extern const char* const LOG_ERROR_FILE = "/system/sys_logs/errors.log";                      // LOG_ERROR_CAP
extern const char* const LOG_EVENTS_FILE = "/system/sys_logs/system-events.log";              // LOG_EVENTS_CAP
extern const char* const LOG_EVENT_STREAM_FILE = "/system/sys_logs/events.log";               // LOG_EVENT_STREAM_CAP

// ============================================================================
// Auth Logging
// ============================================================================
// Lives here, not in WebServer_Server.cpp, because the security audit trail
// must not depend on the web server being compiled in: that whole file is
// wrapped in `#if ENABLE_HTTP_SERVER`, so a headless build used to lose login
// auditing on EVERY transport (serial and UART included), silently.

// The path of an auth log line: the request path with its URL escapes undone.
static String authLogPath(const char* path) {
  String cleanPath = String(path ? path : "");
  cleanPath.replace("%2F", "/");
  cleanPath.replace("%20", " ");
  return cleanPath;
}

// Appends one line to the auth log: accepted events to LOG_OK_FILE, refused
// ones to LOG_FAIL_FILE.
static void appendAuthLogLine(bool success, const String& cleanPath, const String& userTried,
                              const String& ip, const String& reason) {
  char tsPrefix[40];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
  String status = success ? "SUCCESS" : "FAILED";

  String cleanIp = ip;
  cleanIp.replace("::FFFF:", "");

  // Format: [ts] | STATUS | user=.. | ip=.. | /path [| reason=..]
  String line;
  line.reserve(160);
  if (tsPrefix[0]) line += tsPrefix;  // already includes trailing " | "
  line += status;
  line += " | user="; line += userTried;
  line += " | ip=";   line += cleanIp;
  line += " | ";      line += cleanPath;
  if (reason.length()) { line += " | reason="; line += reason; }

  const char* logFile = success ? LOG_OK_FILE : LOG_FAIL_FILE;
  appendLineWithCap(logFile, line, LOG_CAP_BYTES);
}

// A security event by its (cleaned) path: ends in "/login" (so "/login-help"
// and "/configure-login-page" do not match), or is the G2 pair event or the
// ESP-NOW bond.
static bool isSecurityAuditPath(const char* cleanPath) {
  if (!cleanPath) return false;
  static const char kLoginTail[] = "/login";
  const size_t length = strlen(cleanPath);
  const size_t tail = sizeof(kLoginTail) - 1;
  return (length >= tail && strcmp(cleanPath + length - tail, kLoginTail) == 0) ||
         strcmp(cleanPath, "g2/pair") == 0 ||
         strcmp(cleanPath, "espnow/bond") == 0;
}

// A request or a command, admitted or refused. Only a security event reaches
// the auth log file; the command audit tracks the rest. A request is one by
// its path (isSecurityAuditPath): a "<transport>/login" context, the G2 pair
// event or the ESP-NOW bond. The reason is free text for the reader and
// decides nothing: a credential login comes in through recordLoginAttempt()
// and a password change through logCredentialChange(), and both always file.
// Nearly every call is not a security event (each web request, each command),
// so the path is tested before any String is made: a path without a '%' is
// its own cleaned path.
void logAuthAttempt(bool success, const char* path, const String& userTried, const String& ip, const char* reason) {
  if (!path || !strchr(path, '%')) {
    if (!isSecurityAuditPath(path)) return;
  }
  const String cleanPath = authLogPath(path);
  if (!isSecurityAuditPath(cleanPath.c_str())) return;
  appendAuthLogLine(success, cleanPath, userTried, ip, String(reason ? reason : ""));
}

// A password change, accepted or refused: always a security audit event,
// whatever path its transport gave the request ("/account/password-change"
// from the web, "/oled/command" from the local display).
void logCredentialChange(bool success, const char* path, const String& user, const String& ip,
                         const String& reason) {
  appendAuthLogLine(success, authLogPath(path), user, ip, reason);
}

// Single audit front-door for all credential logins (web, serial, UART, BLE,
// OLED, MQTT command envelopes). G2 is excluded — it has no credential login
// (its pair-time identity is logged separately with path "g2/pair"). Maps the
// transport to a canonical "<x>/login" path and fills a synthetic IP when the
// caller has none, so every login path records consistently through one place.
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
    case SOURCE_MQTT:          path = "mqtt/login";      defIp = "mqtt";  break;
    default:                   path = "login";           defIp = "local"; break;
  }
  appendAuthLogLine(success, String(path), user, ip.length() ? ip : String(defIp),
                    String(reason ? reason : ""));
}

// Log a one-time marker when the clock first becomes valid; safe to call
// anytime. `source` names who supplied the time ("ntp", "rtc", "ring",
// "manual", "carryover") — the marker line is a forensic breadcrumb, so it
// should say which custody chain produced this boot's clock.
void logTimeSyncedMarkerIfReady(const char* source) {
  if (gTimeSyncedMarkerWritten) {
    return;
  }

  if (!Clock::isSynced()) {
    return;
  }

  static char* bootTsPrefix = nullptr;
  if (!bootTsPrefix) {
    bootTsPrefix = (char*)ps_alloc(48, AllocPref::PreferPsram, "boot.ts");
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

// The prefix of a capped log line, into `out` (kTimestampPrefixSize bytes,
// NUL-terminated): the wall-clock "[YYYY-MM-DD HH:MM:SS.mmm] | ", or
// "[ms=<uptime>] " while the clock is not set. Not inlined, so its scratch
// buffer is not on the stack during the append that follows.
static constexpr size_t kTimestampPrefixSize = 48;
static __attribute__((noinline)) void formatTimestampPrefix(char* out) {
  getTimestampPrefixMsCached(out, kTimestampPrefixSize);
  if (out[0]) return;
  char msBuf[24];
  snprintf(msBuf, sizeof(msBuf), "[ms=%lu] ", millis());
  memcpy(out, msBuf, sizeof(msBuf));
}

static String buildTimestampPrefix() {
  char tsPrefix[kTimestampPrefixSize];
  formatTimestampPrefix(tsPrefix);
  return String(tsPrefix);
}

// Appends buildTimestampPrefix() + text to a capped log (log_out's [ERROR],
// [EVENT] and [EVLOG] lines). The line is formatted into one PSRAM block this
// function keeps: log_out is its only caller, one line at a time. While PSRAM
// refuses the block, or for a text that does not end within it (a queued text
// is NUL-terminated within DEBUG_MSG_SIZE, so only a broken one), the line is
// made as a String as before. The file receives the same bytes either way.
static void appendTimestampedLine(const char* path, const char* text, size_t capBytes) {
  static constexpr size_t kLineCapacity = kTimestampPrefixSize + DEBUG_MSG_SIZE;
  static char* sLine = nullptr;
  if (!sLine) sLine = static_cast<char*>(ps_alloc(kLineCapacity, AllocPref::RequirePsram, "log.capped_line"));
  if (sLine) {
    formatTimestampPrefix(sLine);
    const size_t prefixLength = strlen(sLine);
    const size_t room = kLineCapacity - 1 - prefixLength;
    const size_t textLength = strnlen(text, room);
    if (textLength < room) {
      memcpy(sLine + prefixLength, text, textLength);
      sLine[prefixLength + textLength] = '\0';
      appendLineWithCap(path, sLine, prefixLength + textLength, capBytes);
      return;
    }
  }
  String line = buildTimestampPrefix();
  line += text;
  appendLineWithCap(path, line, capBytes);
}

void logI2cError(uint8_t address, const char* deviceName, int consecutiveErrors, int totalErrors, bool nowDegraded) {
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

void logI2cRecovery(uint8_t address, const char* deviceName, int totalErrors) {
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
  // Same confinement as `log start`: the persisted path is only a setting and
  // could have been edited to point outside the capture tree.
  if (!VFS::pathInCaptureTree(filepath)) {
    broadcastOutput("[SYSTEM_LOG] Auto-start refused: " + filepath + " is outside " CAPTURE_DIR);
    logSystemEvent("LOG", "debug-capture log autostart REFUSED — %s is outside " CAPTURE_DIR, filepath.c_str());
    return;
  }
  gSystemLogCategoryTags = gSettings.systemLogCategoryTags;

  // Restore last-used category mask so autostart logs the same categories
  // that were selected when system logging was last started. Persistent
  // per-flag commands keep that mask current (systemLogFollowFlagChange).
  systemLogApplyPersistedFlags();
  
  // Ensure any previous file handle is closed (safety check)
  if (gSystemLogFile) {
    fsLock("debug.log");
    gSystemLogFile.flush();
    gSystemLogFile.close();
    fsUnlock();
  }
  
  // Create the log file, where its lines will go (B11): the card twin, sealed,
  // while a card takes log lines, else the path itself. It starts anew on
  // every boot, as before.
  String startedFile;
  fsLock("debug.log");
  size_t startedStored = 0;
  File f = systemLogOpenLocked(filepath, startedFile, nullptr, /*startFresh=*/true, &startedStored);
  if (!f) {
    fsUnlock();
    broadcastOutput("[SYSTEM_LOG] Auto-start failed: Could not create file: " + startedFile);
    logSystemEvent("LOG", "debug-capture log autostart FAILED — could not create %s", startedFile.c_str());
    return;
  }
  systemLogWriteStartLine(f, startedFile, "auto-started", startedStored);
  f.close();
  fsUnlock();
  
  gSystemLogPath = filepath;
  gSystemLogActivePath = startedFile;
  gSystemLogRunning = true;
  gSystemLogLastWrite = millis();
  gOutputFlags |= MSG_ROUTE_FILE;

  broadcastOutput("[SYSTEM_LOG] Auto-start enabled, logging to: " + startedFile);
  logSystemEvent("LOG", "debug-capture log autostart OK → %s", startedFile.c_str());
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
    puts_(kDbgLabel[i]);
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
  { "systemLogAutoStart",    SETTING_BOOL,   &gSettings.systemLogAutoStart,    0, 0, nullptr, 0, 1, "Auto-start logging after boot", nullptr, false, nullptr, "log autostart" },
  { "systemLogPath", SETTING_STRING, &gSettings.systemLogPath, 0, 0, "", 0, 0, "Log file path (empty = auto-generate)", nullptr, false, nullptr, nullptr },
  { "systemLogCategoryTags", SETTING_BOOL, &gSettings.systemLogCategoryTags, 1, 0, nullptr, 0, 1, "Include category tags", nullptr, false, nullptr, "logcategorytags" },
  { "systemLogMaxSize", SETTING_INT, &gSettings.systemLogMaxSize, 1048576, 0, nullptr, 10240, 10485760, "Max file size (bytes)", nullptr, false, nullptr, "log maxsize" },
  { "systemLogMaxRotations", SETTING_INT, &gSettings.systemLogMaxRotations, 3, 0, nullptr, 0, 9, "Rotations (old logs to keep)", nullptr, false, nullptr, "log rotations" },
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
