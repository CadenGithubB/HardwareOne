// System_BootDebug.cpp - every debug output on during boot, into a file of its
// own (owner decision B13 and its follow-up). What and why:
// System_BootDebug.h. The buffer, the file text, the rotation and the ESP-IDF
// level record are System_BootDebugCore.h (host-tested).
#include "System_BootDebug.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <sdkconfig.h>
#include <stdarg.h>
#include <new>

#include "System_BootDebugCore.h"
#include "System_Command.h"       // CommandArgs
#include "System_CommandReply.h"  // kCommandReplyNotOnOff
#include "System_Debug.h"
#include "System_Mutex.h"
#include "System_Settings.h"      // gSettings.bootDebugCapture, setDebugSetting
#include "System_Utils.h"         // submitDeferredToCmdExec, commandQueueFull, gBootCounter, resetReasonName
#include "System_VFS.h"

volatile bool gBootDebugCapturing = false;

namespace {

// The buffer: PSRAM on a build with it (no capture when that allocation
// fails), a small internal one on a build without.
#if CONFIG_SPIRAM
constexpr size_t kBufferBytes = bootdebug::kPsramBufferBytes;
constexpr uint32_t kBufferCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr bool kBufferPsram = true;
// The ESP-IDF level record shares the PSRAM allocation. A build without PSRAM
// does not force the ESP-IDF tags (the record would be more internal memory;
// its 4 KB buffer keeps only what ESP-IDF prints at the saved levels).
constexpr bool kForceEspTags = true;
#else
constexpr size_t kBufferBytes = bootdebug::kInternalBufferBytes;
constexpr uint32_t kBufferCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr bool kBufferPsram = false;
constexpr bool kForceEspTags = false;
#endif
constexpr size_t kShadowOffset = (kBufferBytes + 7) & ~static_cast<size_t>(7);

// The setting's NVS copy, read before any file system is mounted.
constexpr const char* kNvsNamespace = "hw1bootdbg";
constexpr const char* kNvsKeyOff = "off";

// The write's hand-off retry when cmd_exec's queue is full.
constexpr uint32_t kSubmitRetryMs = 250;
// The file is written in pieces, the filesystem lock taken for each.
constexpr size_t kWriteChunk = 8 * 1024;

portMUX_TYPE sLock = portMUX_INITIALIZER_UNLOCKED;  // guards sBuf, sCaptureUs, sShadow
bootdebug::LineBuffer sBuf;
char* sMem = nullptr;
bootdebug::LevelShadow* sShadow = nullptr;
uint64_t sCaptureUs = 0;
uint32_t sStartMs = 0;
uint32_t sEndMs = 0;
volatile bool sBootInProgress = true;
// The ESP-IDF record is in use: from bootDebugBegin until its replay ends.
volatile bool sEspForcing = false;
// Orders a set against the replay (a set made while the replay runs waits,
// then goes to ESP-IDF).
StaticSemaphore_t sEspMutexStorage;
SemaphoreHandle_t sEspMutex = nullptr;
volatile bool sWritePending = false;
uint32_t sNextSubmitMs = 0;

// What this boot's capture did, for `bootdebug`.
enum class Outcome : uint8_t { Off, NoMemory, Capturing, Writing, Written, Failed };
volatile Outcome sOutcome = Outcome::Capturing;
const char* sOutcomeDetail = nullptr;  // the failure, or the file written
uint32_t sOutcomeBytes = 0;
uint32_t sOutcomeMs = 0;

bool nvsCaptureOff() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;  // never written: on
  uint8_t off = 0;
  const esp_err_t err = nvs_get_u8(h, kNvsKeyOff, &off);
  nvs_close(h);
  return err == ESP_OK && off != 0;
}

void nvsSetCaptureOff(bool off) {
  if (nvsCaptureOff() == off) return;
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  if (nvs_set_u8(h, kNvsKeyOff, off ? 1 : 0) == ESP_OK) nvs_commit(h);
  nvs_close(h);
}

// The rotation's file operations, as the boot debug writer's identity
// confined to the system log folder of its tier. The caller holds the
// filesystem lock.
struct GuardedFs {
  const AuthContext& ctx;
  bool exists(const char* p) { return VFS::existsGuarded(String(p), ctx); }
  bool remove(const char* p) { return VFS::removeGuarded(String(p), ctx); }
  bool rename(const char* from, const char* to) { return VFS::renameGuarded(String(from), String(to), ctx); }
};

void releaseBuffer() {
  portENTER_CRITICAL(&sLock);
  char* mem = sMem;
  sMem = nullptr;
  sShadow = nullptr;
  sBuf.attach(nullptr, 0);
  portEXIT_CRITICAL(&sLock);
  if (mem) heap_caps_free(mem);
}

struct Piece {
  const char* data;
  size_t len;
};

// Writes the pieces to files[0] on one tier, the older boots rotated first.
// Returns nullptr when the whole file was written, else why not.
const char* writeTier(bool card, const Piece* pieces, size_t count, size_t total) {
  const char* const* files = card ? bootdebug::kCardBootFiles : bootdebug::kFlashBootFiles;
  const int kept = card ? bootdebug::kCardKeptBoots : bootdebug::kFlashKeptBoots;
  if (!card) {
    if (!VFS::isLittleFsReady()) return "LittleFS is not mounted";
    if (!VFS::admitBulkWrite(VFS::INTERNAL, total, "boot debug file")) {
      return "not enough free space (the log reserve stays free)";
    }
  }
  // trusted: the boot debug writer, confined to the system log folder of its tier.
  const AuthContext ctx = VFS::systemAuth(card ? VFS::Scopes::SYS_LOGS_SD : VFS::Scopes::SYS_LOGS, "bootdebug.write");
  File f;
  {
    FsLockGuard lock("bootdebug.write");
    GuardedFs fs{ctx};
    if (!bootdebug::rotate(fs, files, kept)) logSystemEvent("BOOT", "boot debug files: an older boot's file could not be moved");
    f = VFS::openGuarded(String(files[0]), "w", ctx, /*create=*/true);
  }
  if (!f) return "the file could not be created";
  size_t written = 0;
  bool shortWrite = false;
  for (size_t i = 0; i < count && !shortWrite; ++i) {
    for (size_t off = 0; off < pieces[i].len && !shortWrite; off += kWriteChunk) {
      const size_t n = pieces[i].len - off < kWriteChunk ? pieces[i].len - off : kWriteChunk;
      FsLockGuard lock("bootdebug.write");
      const size_t w = f.write(reinterpret_cast<const uint8_t*>(pieces[i].data + off), n);
      written += w;
      shortWrite = w != n;
    }
  }
  {
    FsLockGuard lock("bootdebug.write");
    f.close();
  }
  if (!card) VFS::noteLittleFsBytesWritten(written);
  if (written != total) return "the write was short (volume full?)";
  return nullptr;
}

// cmd_exec: write the boot file (the capture has stopped, so the buffer no
// longer changes), then free the buffer.
void writeBootFileProc(void*) {
  const uint32_t t0 = millis();
  bootdebug::BootInfo info = {};
  info.bootNumber = gBootCounter;
  info.resetReason = resetReasonName(static_cast<uint32_t>(esp_reset_reason()));
  info.startMs = sStartMs;
  info.endMs = sEndMs;
  info.captureUs = sCaptureUs;
  info.psram = kBufferPsram;
  char header[bootdebug::kHeaderMax];
  const size_t headerLen = bootdebug::formatHeader(header, sizeof(header), info, sBuf);
  char gap[bootdebug::kGapMarkMax];
  const size_t gapLen = bootdebug::formatGapMark(gap, sizeof(gap), sBuf);
  const Piece pieces[] = {
      {header, headerLen}, {sBuf.head(), sBuf.headUsed()}, {gap, gapLen},
      {sBuf.tail1(), sBuf.tail1Len()}, {sBuf.tail2(), sBuf.tail2Len()},
  };
  size_t total = 0;
  for (const Piece& p : pieces) total += p.len;

  // A writable card takes it (three boots kept), else flash (the newest only).
  const bool card = VFS::isSdWritable();
  const char* failure = writeTier(card, pieces, sizeof(pieces) / sizeof(pieces[0]), total);
  bool onCard = card;
  if (failure && card) {
    VFS::noteSdWriteFailure("bootdebug.write");
    logSystemEvent("BOOT", "boot debug file not written to the card: %s; writing it to flash", failure);
    failure = writeTier(false, pieces, sizeof(pieces) / sizeof(pieces[0]), total);
    onCard = false;
  }
  const char* path = onCard ? bootdebug::kCardBootFiles[0] : bootdebug::kFlashBootFiles[0];
  const uint32_t tookMs = millis() - t0;
  sOutcomeBytes = static_cast<uint32_t>(total);
  sOutcomeMs = tookMs;
  if (failure) {
    sOutcomeDetail = failure;
    sOutcome = Outcome::Failed;
    logSystemEvent("BOOT", "boot debug file not written: %s (%u B)", failure, static_cast<unsigned>(total));
  } else {
    sOutcomeDetail = path;
    sOutcome = Outcome::Written;
    logSystemEvent("BOOT", "boot debug file %s: %u B, %lu of %lu lines kept%s; written in %lu ms", path,
                   static_cast<unsigned>(total), static_cast<unsigned long>(sBuf.keptLines()),
                   static_cast<unsigned long>(sBuf.lines()), sBuf.gap() ? " (gap in the middle)" : "",
                   static_cast<unsigned long>(tookMs));
  }
  releaseBuffer();
}

// Hands the write to cmd_exec. A full queue is not a drop: the hand-off waits
// kSubmitRetryMs and the main loop tries again (bootDebugLoopTick).
void submitWrite() {
  if (!commandQueueFull() && submitDeferredToCmdExec(writeBootFileProc, nullptr)) {
    sWritePending = false;
    return;
  }
  sNextSubmitMs = millis() + kSubmitRetryMs;
  sWritePending = true;
}

}  // namespace

void bootDebugBegin() {
  if (sMem || !sBootInProgress) return;
  if (nvsCaptureOff()) {  // the setting is off: no force, no capture, no buffer
    sOutcome = Outcome::Off;
    return;
  }
  const size_t bytes = kForceEspTags ? kShadowOffset + sizeof(bootdebug::LevelShadow) : kBufferBytes;
  char* mem = static_cast<char*>(heap_caps_malloc(bytes, kBufferCaps));
  if (!mem) {  // no capture this boot: the flags stay as saved, nothing is forced
    sOutcome = Outcome::NoMemory;
    return;
  }
  bootdebug::LevelShadow* shadow = nullptr;
  if (kForceEspTags) {
    shadow = new (mem + kShadowOffset) bootdebug::LevelShadow();
    shadow->reset(CONFIG_LOG_DEFAULT_LEVEL);
    if (!sEspMutex) sEspMutex = xSemaphoreCreateMutexStatic(&sEspMutexStorage);
  }
  sStartMs = millis();
  portENTER_CRITICAL(&sLock);
  sMem = mem;
  sShadow = shadow;
  sBuf.attach(mem, kBufferBytes);
  sCaptureUs = 0;
  portEXIT_CRITICAL(&sLock);
  sEspForcing = shadow != nullptr;
  gBootDebugCapturing = true;
  gDebugLineForce = true;
  idfLogHookRefresh();
}

void bootDebugCaptureLine(const char* text, size_t len, bool shown, int64_t formatStartUs) {
  if (xPortInIsrContext() || !text) return;
  const int64_t start = formatStartUs ? formatStartUs : esp_timer_get_time();
  const uint32_t ms = millis();
  char prefix[bootdebug::kPrefixMax + 1];
  const size_t plen = bootdebug::LineBuffer::formatPrefix(prefix, sizeof(prefix), ms);
  portENTER_CRITICAL(&sLock);
  if (gBootDebugCapturing && sMem) {
    sBuf.appendPrefixed(prefix, plen, text, len, !shown);
    sCaptureUs += static_cast<uint64_t>(esp_timer_get_time() - start);
  }
  portEXIT_CRITICAL(&sLock);
}

void bootDebugCaptureText(const char* text) {
  if (!gBootDebugCapturing || !text) return;
  bootDebugCaptureLine(text, strlen(text), /*shown=*/false, esp_timer_get_time());
}

void bootDebugCaptureF(const char* fmt, ...) {
  if (!gBootDebugCapturing || !fmt) return;
  const int64_t start = esp_timer_get_time();
  char line[DEBUG_MSG_SIZE];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  bootDebugCaptureLine(line, static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1,
                       /*shown=*/false, start);
}

void bootDebugEspLogSet(const char* tag, esp_log_level_t level) {
  if (!tag) return;
  if (!sEspForcing || !sEspMutex) {
    esp_log_level_set(tag, level);
    return;
  }
  xSemaphoreTake(sEspMutex, portMAX_DELAY);
  bool recorded = false;
  if (sEspForcing) {
    portENTER_CRITICAL(&sLock);
    recorded = sShadow && sShadow->set(tag, static_cast<int>(level));
    portEXIT_CRITICAL(&sLock);
  }
  if (!recorded) esp_log_level_set(tag, level);  // after the replay, or a tag the record cannot hold
  xSemaphoreGive(sEspMutex);
}

esp_log_level_t bootDebugEspLogGet(const char* tag) {
  if (sEspForcing && tag) {
    int level = -1;
    portENTER_CRITICAL(&sLock);
    if (sEspForcing && sShadow) level = sShadow->levelFor(tag, strlen(tag));
    portEXIT_CRITICAL(&sLock);
    if (level >= 0) return static_cast<esp_log_level_t>(level);
  }
  return esp_log_level_get(tag);
}

bool bootDebugEspLogWanted() { return sEspForcing || gBootDebugCapturing; }

bool bootDebugEspLogLine(const char* text, size_t len, int64_t formatStartUs) {
  bool shown = true;
  int level = 0;
  const char* tag = nullptr;
  size_t tagLen = 0;
  if (sEspForcing && bootdebug::parseEspLogLine(text, len, &level, &tag, &tagLen)) {
    portENTER_CRITICAL(&sLock);
    if (sEspForcing && sShadow) shown = level <= sShadow->levelFor(tag, tagLen);
    portEXIT_CRITICAL(&sLock);
  }
  if (gBootDebugCapturing) bootDebugCaptureLine(text, len, shown, formatStartUs);
  return shown;
}

bool bootDebugForceWanted() { return gBootDebugCapturing; }

void bootDebugFinish() {
  if (!sBootInProgress) return;
  sBootInProgress = false;
  const bool captured = gBootDebugCapturing;
  // The saved settings rule again (debugverbose keeps its own force).
  gDebugLineForce = gDebugVerbose;
  portENTER_CRITICAL(&sLock);
  gBootDebugCapturing = false;
  portEXIT_CRITICAL(&sLock);
  // ESP-IDF's tag levels go where the firmware's calls put them. Until the
  // replay is done the hook still keeps the lines those levels would not show
  // off the console.
  if (sEspForcing && sEspMutex) {
    xSemaphoreTake(sEspMutex, portMAX_DELAY);
    if (sShadow) {
      sShadow->replay([](const char* tag, int level) { esp_log_level_set(tag, static_cast<esp_log_level_t>(level)); });
    }
    sEspForcing = false;
    xSemaphoreGive(sEspMutex);
  }
  sEspForcing = false;
  idfLogHookRefresh();
  // The NVS copy follows the saved setting (a restore or an import may have
  // changed debug.json without the command).
  nvsSetCaptureOff(!gSettings.bootDebugCapture);
  if (!captured) {
    if (sOutcome == Outcome::NoMemory) logSystemEvent("BOOT", "boot debug capture was off this boot (no memory for its buffer)");
    releaseBuffer();
    return;
  }
  sEndMs = millis();
  sOutcome = Outcome::Writing;
  submitWrite();
}

void bootDebugLoopTick() {
  if (!sWritePending) return;
  if (static_cast<int32_t>(millis() - sNextSubmitMs) < 0) return;
  submitWrite();
}

bool bootDebugInProgress() { return sBootInProgress; }

const char* cmd_bootdebug(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  if (ca.count() > 1) return "Usage: bootdebug [on|off]";
  if (ca.count() == 1) {
    const int v = parseBoolArg(ca.arg(0));
    if (v < 0) return kCommandReplyNotOnOff;
    setDebugSetting(gSettings.bootDebugCapture, v == 1);
    nvsSetCaptureOff(v != 1);
    return v == 1 ? "Boot debug capture on (saved; from the next boot)"
                  : "Boot debug capture off (saved; from the next boot: no forcing, no capture)";
  }
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  char thisBoot[200];
  switch (sOutcome) {
    case Outcome::Off:
      snprintf(thisBoot, sizeof(thisBoot), "not captured (the setting was off at boot)");
      break;
    case Outcome::NoMemory:
      snprintf(thisBoot, sizeof(thisBoot), "not captured (no memory for its buffer)");
      break;
    case Outcome::Capturing:
    case Outcome::Writing:
      snprintf(thisBoot, sizeof(thisBoot), "captured; the file is still being written");
      break;
    case Outcome::Written:
      snprintf(thisBoot, sizeof(thisBoot), "%s (%lu B, written in %lu ms)", sOutcomeDetail ? sOutcomeDetail : "?",
               static_cast<unsigned long>(sOutcomeBytes), static_cast<unsigned long>(sOutcomeMs));
      break;
    case Outcome::Failed:
      snprintf(thisBoot, sizeof(thisBoot), "not written: %s (%lu B)", sOutcomeDetail ? sOutcomeDetail : "?",
               static_cast<unsigned long>(sOutcomeBytes));
      break;
  }
  snprintf(getDebugBuffer(), 1024,
           "Boot debug capture: %s (saved; a change applies from the next boot)\n"
           "This boot: %s\n"
           "Files: %s (newest of %d) on a writable SD card, else %s (newest only)",
           gSettings.bootDebugCapture ? "on" : "off", thisBoot, bootdebug::kCardBootFiles[0],
           bootdebug::kCardKeptBoots, bootdebug::kFlashBootFiles[0]);
  return getDebugBuffer();
}
