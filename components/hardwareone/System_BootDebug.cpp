// System_BootDebug.cpp - every debug flag on during boot, into a file of its
// own (owner decision B13). What and why: System_BootDebug.h. The buffer, the
// file text and the rotation are System_BootDebugCore.h (host-tested).
#include "System_BootDebug.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include "System_BootDebugCore.h"
#include "System_Debug.h"
#include "System_Mutex.h"
#include "System_Utils.h"  // submitDeferredToCmdExec, gBootCounter, resetReasonName
#include "System_VFS.h"

volatile bool gBootDebugCapturing = false;

namespace {

portMUX_TYPE sLock = portMUX_INITIALIZER_UNLOCKED;  // guards sBuf and sCaptureUs
bootdebug::LineBuffer sBuf;
char* sMem = nullptr;
bool sMemPsram = false;
uint64_t sCaptureUs = 0;
uint32_t sStartMs = 0;
uint32_t sEndMs = 0;
volatile bool sBootInProgress = true;
volatile bool sWritePending = false;  // the file write waits for a free executor slot

// The rotation's file operations, as the capped-log writer's identity
// confined to /system/sys_logs. The caller holds the filesystem lock.
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
  sBuf.attach(nullptr, 0);
  portEXIT_CRITICAL(&sLock);
  if (mem) heap_caps_free(mem);
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
  info.psram = sMemPsram;
  char header[768];  // kHeaderAndMarkMax
  const size_t headerLen = bootdebug::formatHeader(header, sizeof(header), info, sBuf);
  char mark[192];
  const size_t markLen = bootdebug::formatTruncationMark(mark, sizeof(mark), sBuf);
  const size_t total = headerLen + sBuf.used() + markLen;

  const char* failure = nullptr;
  if (!VFS::isLittleFsReady()) {
    failure = "LittleFS is not mounted";
  } else if (!VFS::admitBulkWrite(VFS::INTERNAL, total, "boot debug file")) {
    failure = "not enough free space (the log reserve stays free)";
  } else {
    FsLockGuard lock("bootdebug.write");
    // trusted: the boot debug writer, confined to the system log folder.
    const AuthContext ctx = VFS::systemAuth(VFS::Scopes::SYS_LOGS, "bootdebug.write");
    GuardedFs fs{ctx};
    if (!bootdebug::rotate(fs)) logSystemEvent("BOOT", "boot debug files: an older boot's file could not be moved");
    File f = VFS::openGuarded(String(bootdebug::kBootFiles[0]), "w", ctx, /*create=*/true);
    if (!f) {
      failure = "the file could not be created";
    } else {
      size_t written = f.write(reinterpret_cast<const uint8_t*>(header), headerLen);
      if (sBuf.used()) written += f.write(reinterpret_cast<const uint8_t*>(sBuf.data()), sBuf.used());
      if (markLen) written += f.write(reinterpret_cast<const uint8_t*>(mark), markLen);
      f.close();
      VFS::noteLittleFsBytesWritten(written);
      if (written != total) failure = "the write was short (volume full?)";
    }
  }
  const uint32_t tookMs = millis() - t0;
  if (failure) {
    logSystemEvent("BOOT", "boot debug file not written: %s (%u B)", failure, static_cast<unsigned>(total));
  } else {
    logSystemEvent("BOOT", "boot debug file %s: %u B, %lu lines kept, %lu dropped%s; written in %lu ms",
                   bootdebug::kBootFiles[0], static_cast<unsigned>(total),
                   static_cast<unsigned long>(sBuf.lines()), static_cast<unsigned long>(sBuf.droppedLines()),
                   sBuf.truncated() ? " (truncated)" : "", static_cast<unsigned long>(tookMs));
  }
  releaseBuffer();
}

void submitWrite() {
  sWritePending = !submitDeferredToCmdExec(writeBootFileProc, nullptr);
}

}  // namespace

void bootDebugBegin() {
  if (sMem || !sBootInProgress) return;
  // PSRAM first; a board without it gets a small internal buffer for the boot only.
  char* mem = static_cast<char*>(heap_caps_malloc(bootdebug::kPsramBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  bool psram = mem != nullptr;
  if (!mem) {
    mem = static_cast<char*>(heap_caps_malloc(bootdebug::kInternalBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (!mem) return;  // no capture this boot: the flags stay as saved, nothing is forced
  sStartMs = millis();
  portENTER_CRITICAL(&sLock);
  sMem = mem;
  sMemPsram = psram;
  sBuf.attach(mem, psram ? bootdebug::kPsramBufferBytes : bootdebug::kInternalBufferBytes);
  sCaptureUs = 0;
  portEXIT_CRITICAL(&sLock);
  gBootDebugCapturing = true;
  gDebugLineForce = true;
}

void bootDebugCaptureLine(const char* text, size_t len, bool shown, int64_t formatStartUs) {
  if (xPortInIsrContext()) return;
  const int64_t start = formatStartUs ? formatStartUs : esp_timer_get_time();
  const uint32_t ms = millis();
  char prefix[bootdebug::kPrefixMax + 1];
  const size_t plen = bootdebug::LineBuffer::formatPrefix(prefix, sizeof(prefix), ms);
  bool full = false;
  portENTER_CRITICAL(&sLock);
  if (gBootDebugCapturing && sMem) {
    sBuf.appendPrefixed(prefix, plen, ms, text, len, !shown);
    sCaptureUs += static_cast<uint64_t>(esp_timer_get_time() - start);
    full = sBuf.truncated();
  }
  portEXIT_CRITICAL(&sLock);
  // A full buffer keeps nothing more: stop forcing, so the rest of the boot
  // does not build and format every flag's lines only to count them. Lines
  // the saved flags show are still counted (the truncation mark says so).
  if (full) gDebugLineForce = gDebugVerbose;
}

bool bootDebugForceWanted() {
  portENTER_CRITICAL(&sLock);
  const bool wanted = gBootDebugCapturing && sMem && !sBuf.truncated();
  portEXIT_CRITICAL(&sLock);
  return wanted;
}

void bootDebugFinish() {
  if (!sBootInProgress) return;
  sBootInProgress = false;
  const bool captured = gBootDebugCapturing;
  // The saved flags rule again (debugverbose keeps its own force).
  gDebugLineForce = gDebugVerbose;
  portENTER_CRITICAL(&sLock);
  gBootDebugCapturing = false;
  portEXIT_CRITICAL(&sLock);
  if (!captured) {
    logSystemEvent("BOOT", "boot debug capture was off this boot (no memory for its buffer)");
    return;
  }
  sEndMs = millis();
  submitWrite();
}

void bootDebugLoopTick() {
  if (sWritePending) submitWrite();
}

bool bootDebugInProgress() { return sBootInProgress; }
