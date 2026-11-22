// System_BootDebugCore.h - the boot debug capture's buffer, file text and
// file rotation (owner decision B13, 2026-11-21). Pure: no Arduino, no
// ESP-IDF, no allocation, so the host test (test_boot_debug_core.cpp) runs it
// as it is. The device side (memory, lock, flag force, the write on cmd_exec)
// is System_BootDebug.cpp.
//
// Every boot runs with every debug flag forced on from the start of setup()
// until setup() ends. Each debug line of that window is copied here as
// "[<uptime ms>] <line>\n"; lines the saved flags would not have shown go
// nowhere else (not the console, not the normal logs). At the end of boot the
// buffer becomes /system/sys_logs/boot-debug-1.log and the two boots before it
// move to -2 and -3.
//
// The buffer keeps the FIRST lines of the boot: once a line does not fit, it
// and every later line are counted, not kept, so the file is one unbroken
// stretch from the start of boot plus a truncation mark that says how much
// was left out. The force stops when the buffer fills (System_BootDebug.cpp),
// so after that only the lines the saved flags show are made and counted.
#ifndef SYSTEM_BOOT_DEBUG_CORE_H
#define SYSTEM_BOOT_DEBUG_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace bootdebug {

// Buffer sizes. A board with PSRAM takes the full buffer there; a board with
// none takes a small one from the internal heap for the boot only (freed once
// the file is written).
constexpr size_t kPsramBufferBytes = 56 * 1024;
constexpr size_t kInternalBufferBytes = 4 * 1024;
// The whole file (header, buffer, truncation mark) stays under the 60000 B
// that `fileview` reads, so its pages show the file to the end.
constexpr size_t kHeaderAndMarkMax = 768 + 192;
static_assert(kPsramBufferBytes + kHeaderAndMarkMax < 60000, "a full boot file must fit fileview's read");

// The kept boots, newest first.
constexpr int kKeptBoots = 3;
constexpr const char* kBootFiles[kKeptBoots] = {
    "/system/sys_logs/boot-debug-1.log",
    "/system/sys_logs/boot-debug-2.log",
    "/system/sys_logs/boot-debug-3.log",
};

// The longest "[<ms>] " prefix: ten digits, brackets and a space.
constexpr size_t kPrefixMax = 13;

class LineBuffer {
 public:
  void attach(char* mem, size_t capacity) {
    mem_ = mem;
    cap_ = mem ? capacity : 0;
    used_ = 0;
    lines_ = forcedLines_ = droppedLines_ = 0;
    droppedBytes_ = 0;
    fullAtMs_ = 0;
    truncated_ = false;
  }

  // Appends "[ms] text\n" as one whole line, or counts it as dropped. `forced`
  // marks a line only the boot capture made (the saved flags would not have
  // shown it). Returns true when the line was kept.
  bool append(uint32_t ms, const char* text, size_t len, bool forced) {
    char prefix[kPrefixMax + 1];
    const size_t plen = formatPrefix(prefix, sizeof(prefix), ms);
    return appendPrefixed(prefix, plen, ms, text, len, forced);
  }

  // "[ms] " for append; the device formats it before taking its spinlock.
  static size_t formatPrefix(char* out, size_t outSize, uint32_t ms) {
    const int p = snprintf(out, outSize, "[%lu] ", static_cast<unsigned long>(ms));
    if (p <= 0) return 0;
    return static_cast<size_t>(p) < outSize ? static_cast<size_t>(p) : outSize - 1;
  }

  // append with the prefix already made: memcpy only, no library call.
  bool appendPrefixed(const char* prefix, size_t plen, uint32_t ms, const char* text, size_t len, bool forced) {
    const size_t need = plen + len + 1;
    if (truncated_ || !mem_ || need > cap_ - used_) {
      if (!truncated_) {
        truncated_ = true;
        fullAtMs_ = ms;
      }
      droppedLines_++;
      droppedBytes_ += need;
      return false;
    }
    memcpy(mem_ + used_, prefix, plen);
    if (len) memcpy(mem_ + used_ + plen, text, len);
    mem_[used_ + plen + len] = '\n';
    used_ += need;
    lines_++;
    if (forced) forcedLines_++;
    return true;
  }

  const char* data() const { return mem_; }
  size_t used() const { return used_; }
  size_t capacity() const { return cap_; }
  bool truncated() const { return truncated_; }
  uint32_t lines() const { return lines_; }
  uint32_t forcedLines() const { return forcedLines_; }
  uint32_t droppedLines() const { return droppedLines_; }
  size_t droppedBytes() const { return droppedBytes_; }
  uint32_t fullAtMs() const { return fullAtMs_; }

 private:
  char* mem_ = nullptr;
  size_t cap_ = 0;
  size_t used_ = 0;
  uint32_t lines_ = 0;
  uint32_t forcedLines_ = 0;
  uint32_t droppedLines_ = 0;
  size_t droppedBytes_ = 0;
  uint32_t fullAtMs_ = 0;
  bool truncated_ = false;
};

// What the file's header records besides the buffer's own counts.
struct BootInfo {
  uint32_t bootNumber;      // the NVS boot counter (0 = unknown)
  const char* resetReason;  // resetReasonName()
  uint32_t startMs;         // uptime when the capture started (top of setup)
  uint32_t endMs;           // uptime when setup() ended
  uint64_t captureUs;       // CPU time spent in the capture path, all tasks
  bool psram;               // the buffer is in PSRAM (false: internal heap)
};

// The header: '#' lines a reader can grep. Returns the length written (the
// text is cut to outSize - 1 like snprintf; 768 bytes always hold it).
inline size_t formatHeader(char* out, size_t outSize, const BootInfo& info, const LineBuffer& buf) {
  if (!out || outSize == 0) return 0;
  const unsigned long bootMs = info.endMs >= info.startMs ? static_cast<unsigned long>(info.endMs - info.startMs) : 0UL;
  int n = snprintf(out, outSize,
                   "# HardwareOne boot debug capture (every debug flag on during boot)\n"
                   "# boot: #%lu, reset reason %s\n"
                   "# capture: start %lu ms, end %lu ms, boot took %lu ms\n"
                   "# buffer: %lu B in %s, %lu B used\n"
                   "# lines: %lu kept (%lu shown only because of the boot capture), %lu dropped\n"
                   "# output: %lu B in all (%lu B kept, %lu B dropped)\n"
                   "# capture cost: %llu us of CPU in the capture path (formatting the forced lines, copying every line;\n"
                   "#   not the callers' own work building a forced line's arguments)\n",
                   static_cast<unsigned long>(info.bootNumber), info.resetReason ? info.resetReason : "unknown",
                   static_cast<unsigned long>(info.startMs), static_cast<unsigned long>(info.endMs), bootMs,
                   static_cast<unsigned long>(buf.capacity()), info.psram ? "PSRAM" : "internal RAM",
                   static_cast<unsigned long>(buf.used()), static_cast<unsigned long>(buf.lines()),
                   static_cast<unsigned long>(buf.forcedLines()), static_cast<unsigned long>(buf.droppedLines()),
                   static_cast<unsigned long>(buf.used() + buf.droppedBytes()), static_cast<unsigned long>(buf.used()),
                   static_cast<unsigned long>(buf.droppedBytes()), static_cast<unsigned long long>(info.captureUs));
  if (n < 0) return 0;
  size_t len = static_cast<size_t>(n) < outSize ? static_cast<size_t>(n) : outSize - 1;
  if (buf.truncated()) {
    n = snprintf(out + len, outSize - len, "# TRUNCATED: the buffer filled at %lu ms; the end of this file says what is missing\n",
                 static_cast<unsigned long>(buf.fullAtMs()));
    if (n > 0) len += static_cast<size_t>(n) < outSize - len ? static_cast<size_t>(n) : outSize - len - 1;
  }
  return len;
}

// The mark after the last kept line, empty (0) when nothing was dropped.
inline size_t formatTruncationMark(char* out, size_t outSize, const LineBuffer& buf) {
  if (!out || outSize == 0) return 0;
  out[0] = '\0';
  if (!buf.truncated()) return 0;
  const int n = snprintf(out, outSize,
                         "[TRUNCATED] %lu more line(s), %lu B, did not fit in the %lu B buffer (full at %lu ms); "
                         "later lines counted only where the saved flags show them\n",
                         static_cast<unsigned long>(buf.droppedLines()), static_cast<unsigned long>(buf.droppedBytes()),
                         static_cast<unsigned long>(buf.capacity()), static_cast<unsigned long>(buf.fullAtMs()));
  if (n < 0) return 0;
  return static_cast<size_t>(n) < outSize ? static_cast<size_t>(n) : outSize - 1;
}

// Moves the kept boots down one place so kBootFiles[0] is free for this boot:
// the oldest goes, -2 becomes -3, -1 becomes -2. Fs needs
//   bool exists(const char*), bool remove(const char*), bool rename(const char*, const char*).
// Returns false when a step failed; the caller still writes the new file
// (a failed rename at worst loses the older boots, never this one).
template <class Fs>
bool rotate(Fs& fs) {
  bool ok = true;
  if (fs.exists(kBootFiles[kKeptBoots - 1])) ok = fs.remove(kBootFiles[kKeptBoots - 1]) && ok;
  for (int i = kKeptBoots - 2; i >= 0; --i) {
    if (!fs.exists(kBootFiles[i])) continue;
    ok = fs.rename(kBootFiles[i], kBootFiles[i + 1]) && ok;
  }
  return ok;
}

}  // namespace bootdebug

#endif  // SYSTEM_BOOT_DEBUG_CORE_H
