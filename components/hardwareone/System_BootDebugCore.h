// System_BootDebugCore.h - the boot debug capture's buffer, file text, file
// rotation and ESP-IDF level bookkeeping (owner decisions B13 and its
// follow-up, 2026-11-21/22). Pure: no Arduino, no ESP-IDF, no allocation, so
// the host test (test_boot_debug_core.cpp) runs it as it is. The device side
// (memory, locks, the force, the ESP-IDF hook, the write on cmd_exec) is
// System_BootDebug.cpp.
//
// While the capture runs (the top of setup() until setup() ends, unless the
// bootdebug setting is off) every debug output is on. Each line of that window
// is copied here as "[<uptime ms>] <line>\n"; a line the saved settings would
// not have shown goes nowhere else (not the console, not the normal logs). At
// the end of boot the buffer becomes boot-debug-1.log: on a writable SD card
// in /sd/system/sys_logs (the two boots before it move to -2 and -3), else in
// /system/sys_logs on flash (only the newest boot is kept there).
//
// The buffer keeps the START and the END of the boot: its first half fills
// from the start of boot; after that the lines go round its second half, a
// ring that always holds the newest lines. The file is the first half, a gap
// mark saying how much was dropped between them (when anything was), then the
// newest lines.
#ifndef SYSTEM_BOOT_DEBUG_CORE_H
#define SYSTEM_BOOT_DEBUG_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace bootdebug {

// Buffer sizes. A build with PSRAM takes the full buffer there (and has no
// capture when that allocation fails); a build without PSRAM takes a small one
// from the internal heap for the boot only (freed once the file is written).
constexpr size_t kPsramBufferBytes = 256 * 1024;
constexpr size_t kInternalBufferBytes = 4 * 1024;
// The header and the gap mark are formatted on the writer's stack.
constexpr size_t kHeaderMax = 1024;
constexpr size_t kGapMarkMax = 192;

// The kept boots, newest first: three on a card, one on flash.
constexpr int kCardKeptBoots = 3;
constexpr const char* kCardBootFiles[kCardKeptBoots] = {
    "/sd/system/sys_logs/boot-debug-1.log",
    "/sd/system/sys_logs/boot-debug-2.log",
    "/sd/system/sys_logs/boot-debug-3.log",
};
constexpr int kFlashKeptBoots = 1;
constexpr const char* kFlashBootFiles[kFlashKeptBoots] = {
    "/system/sys_logs/boot-debug-1.log",
};

// The longest "[<ms>] " prefix: ten digits, brackets and a space.
constexpr size_t kPrefixMax = 13;
// One captured line's text is cut here (a debug line's own limit).
constexpr size_t kLineMax = 255;

class LineBuffer {
 public:
  // The first half of `capacity` keeps the start of the boot, the rest is the
  // ring for its end.
  void attach(char* mem, size_t capacity) {
    mem_ = mem;
    const size_t cap = mem ? capacity : 0;
    headCap_ = cap / 2;
    tailCap_ = cap - headCap_;
    headUsed_ = tailStart_ = tailUsed_ = 0;
    headOpen_ = true;
    lines_ = forcedLines_ = droppedLines_ = 0;
    totalBytes_ = droppedBytes_ = 0;
  }

  // Appends "[ms] text\n", one line per '\n'-separated piece of `text` (a
  // trailing newline adds no empty line). `forced` marks a line only the boot
  // capture made (the saved settings would not have shown it).
  void append(uint32_t ms, const char* text, size_t len, bool forced) {
    char prefix[kPrefixMax + 1];
    const size_t plen = formatPrefix(prefix, sizeof(prefix), ms);
    appendPrefixed(prefix, plen, text, len, forced);
  }

  // "[ms] " for append; the device formats it before taking its spinlock.
  static size_t formatPrefix(char* out, size_t outSize, uint32_t ms) {
    const int p = snprintf(out, outSize, "[%lu] ", static_cast<unsigned long>(ms));
    if (p <= 0) return 0;
    return static_cast<size_t>(p) < outSize ? static_cast<size_t>(p) : outSize - 1;
  }

  // append with the prefix already made: memcpy and memchr only.
  void appendPrefixed(const char* prefix, size_t plen, const char* text, size_t len, bool forced) {
    if (!text) len = 0;
    if (len > kLineMax) len = kLineMax;
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    size_t start = 0;
    do {
      const char* nl = len > start ? static_cast<const char*>(memchr(text + start, '\n', len - start)) : nullptr;
      const size_t end = nl ? static_cast<size_t>(nl - text) : len;
      size_t pieceEnd = end;
      if (pieceEnd > start && text[pieceEnd - 1] == '\r') pieceEnd--;
      appendOne(prefix, plen, text ? text + start : "", pieceEnd - start, forced);
      start = end + 1;
    } while (start < len);
  }

  // The file's body, in order: head, (gap mark,) tail1, tail2.
  const char* head() const { return mem_; }
  size_t headUsed() const { return headUsed_; }
  // The ring from its oldest byte: up to two pieces (the second is empty when
  // the ring does not wrap).
  const char* tail1() const { return mem_ ? mem_ + headCap_ + tailStart_ : nullptr; }
  size_t tail1Len() const { return tailStart_ + tailUsed_ <= tailCap_ ? tailUsed_ : tailCap_ - tailStart_; }
  const char* tail2() const { return mem_ ? mem_ + headCap_ : nullptr; }
  size_t tail2Len() const { return tailUsed_ - tail1Len(); }

  size_t capacity() const { return headCap_ + tailCap_; }
  size_t headCapacity() const { return headCap_; }
  size_t tailCapacity() const { return tailCap_; }
  size_t used() const { return headUsed_ + tailUsed_; }
  bool gap() const { return droppedLines_ != 0; }
  uint32_t lines() const { return lines_; }              // every line of the capture
  uint32_t forcedLines() const { return forcedLines_; }  // of which only the capture made
  uint32_t keptLines() const { return lines_ - droppedLines_; }
  uint32_t droppedLines() const { return droppedLines_; }
  uint64_t totalBytes() const { return totalBytes_; }    // kept + dropped
  uint64_t droppedBytes() const { return droppedBytes_; }

 private:
  void appendOne(const char* prefix, size_t plen, const char* text, size_t len, bool forced) {
    const size_t need = plen + len + 1;
    lines_++;
    if (forced) forcedLines_++;
    totalBytes_ += need;
    if (headOpen_) {
      if (need <= headCap_ - headUsed_) {
        memcpy(mem_ + headUsed_, prefix, plen);
        if (len) memcpy(mem_ + headUsed_ + plen, text, len);
        mem_[headUsed_ + plen + len] = '\n';
        headUsed_ += need;
        return;
      }
      headOpen_ = false;  // the start is kept; every later line goes round the ring
    }
    if (need > tailCap_) {  // a ring smaller than one line has nowhere to keep it
      droppedLines_++;
      droppedBytes_ += need;
      return;
    }
    while (tailCap_ - tailUsed_ < need) evictOldest();
    size_t pos = (tailStart_ + tailUsed_) % tailCap_;
    pos = ringPut(pos, prefix, plen);
    pos = ringPut(pos, text, len);
    ringPut(pos, "\n", 1);
    tailUsed_ += need;
  }

  // Copies n bytes into the ring at pos, wrapping; returns the next position.
  size_t ringPut(size_t pos, const char* src, size_t n) {
    char* ring = mem_ + headCap_;
    while (n) {
      const size_t room = tailCap_ - pos;
      const size_t take = n < room ? n : room;
      memcpy(ring + pos, src, take);
      src += take;
      n -= take;
      pos = (pos + take) % tailCap_;
    }
    return pos;
  }

  // Drops the ring's oldest whole line (every line in it ends with '\n').
  void evictOldest() {
    const char* ring = mem_ + headCap_;
    size_t n = 0;
    size_t pos = tailStart_;
    while (n < tailUsed_) {
      const size_t run = tailCap_ - pos < tailUsed_ - n ? tailCap_ - pos : tailUsed_ - n;
      const char* nl = static_cast<const char*>(memchr(ring + pos, '\n', run));
      if (nl) {
        n += static_cast<size_t>(nl - (ring + pos)) + 1;
        break;
      }
      n += run;
      pos = 0;
    }
    tailStart_ = (tailStart_ + n) % tailCap_;
    tailUsed_ -= n;
    droppedLines_++;
    droppedBytes_ += n;
  }

  char* mem_ = nullptr;
  size_t headCap_ = 0;
  size_t tailCap_ = 0;
  size_t headUsed_ = 0;
  size_t tailStart_ = 0;
  size_t tailUsed_ = 0;
  bool headOpen_ = true;
  uint32_t lines_ = 0;
  uint32_t forcedLines_ = 0;
  uint32_t droppedLines_ = 0;
  uint64_t totalBytes_ = 0;
  uint64_t droppedBytes_ = 0;
};

// What the file's header records besides the buffer's own counts.
struct BootInfo {
  uint32_t bootNumber;      // the NVS boot counter (0 = unknown)
  const char* resetReason;  // resetReasonName()
  uint32_t startMs;         // uptime when the capture started (top of setup)
  uint32_t endMs;           // uptime when setup() ended
  uint64_t captureUs;       // wall-clock time spent in the capture path, all tasks together
  bool psram;               // the buffer is in PSRAM (false: internal heap)
};

// The header: '#' lines a reader can grep. Returns the length written (the
// text is cut to outSize - 1 like snprintf; kHeaderMax always holds it).
inline size_t formatHeader(char* out, size_t outSize, const BootInfo& info, const LineBuffer& buf) {
  if (!out || outSize == 0) return 0;
  const unsigned long bootMs = info.endMs >= info.startMs ? static_cast<unsigned long>(info.endMs - info.startMs) : 0UL;
  int n = snprintf(out, outSize,
                   "# HardwareOne boot debug capture (every debug output on during boot)\n"
                   "# boot: #%lu, reset reason %s\n"
                   "# capture: start %lu ms, end %lu ms, boot took %lu ms\n"
                   "# buffer: %lu B in %s (the first %lu B of the boot, then the newest %lu B)\n"
                   "# lines: %lu in all (%lu only because of the boot capture), %lu kept, %lu dropped\n"
                   "# output: %llu B in all (%lu B kept, %llu B dropped)\n"
                   "# capture time: %llu us of wall-clock time in the capture path (formatting the forced\n"
                   "#   lines, copying every line; not the callers' own work building a forced line's arguments)\n",
                   static_cast<unsigned long>(info.bootNumber), info.resetReason ? info.resetReason : "unknown",
                   static_cast<unsigned long>(info.startMs), static_cast<unsigned long>(info.endMs), bootMs,
                   static_cast<unsigned long>(buf.capacity()), info.psram ? "PSRAM" : "internal RAM",
                   static_cast<unsigned long>(buf.headCapacity()), static_cast<unsigned long>(buf.tailCapacity()),
                   static_cast<unsigned long>(buf.lines()), static_cast<unsigned long>(buf.forcedLines()),
                   static_cast<unsigned long>(buf.keptLines()), static_cast<unsigned long>(buf.droppedLines()),
                   static_cast<unsigned long long>(buf.totalBytes()), static_cast<unsigned long>(buf.used()),
                   static_cast<unsigned long long>(buf.droppedBytes()),
                   static_cast<unsigned long long>(info.captureUs));
  if (n < 0) return 0;
  size_t len = static_cast<size_t>(n) < outSize ? static_cast<size_t>(n) : outSize - 1;
  if (buf.gap()) {
    n = snprintf(out + len, outSize - len, "# GAP: the middle of the boot did not fit; a [GAP] line marks where\n");
    if (n > 0) len += static_cast<size_t>(n) < outSize - len ? static_cast<size_t>(n) : outSize - len - 1;
  }
  return len;
}

// The mark between the kept start and the kept end; empty (0) when nothing
// was dropped.
inline size_t formatGapMark(char* out, size_t outSize, const LineBuffer& buf) {
  if (!out || outSize == 0) return 0;
  out[0] = '\0';
  if (!buf.gap()) return 0;
  const int n = snprintf(out, outSize,
                         "[GAP] %lu line(s), %llu B of the boot dropped here (the buffer keeps the first %lu B "
                         "and the newest %lu B)\n",
                         static_cast<unsigned long>(buf.droppedLines()),
                         static_cast<unsigned long long>(buf.droppedBytes()),
                         static_cast<unsigned long>(buf.headCapacity()),
                         static_cast<unsigned long>(buf.tailCapacity()));
  if (n < 0) return 0;
  return static_cast<size_t>(n) < outSize ? static_cast<size_t>(n) : outSize - 1;
}

// Moves the kept boots down one place so files[0] is free for this boot: the
// oldest goes and every other one moves down (with one kept boot it is
// removed). Fs needs
//   bool exists(const char*), bool remove(const char*), bool rename(const char*, const char*).
// Returns false when a step failed; the caller still writes the new file
// (a failed rename at worst loses the older boots, never this one).
template <class Fs>
bool rotate(Fs& fs, const char* const* files, int kept) {
  if (!files || kept <= 0) return true;
  bool ok = true;
  if (fs.exists(files[kept - 1])) ok = fs.remove(files[kept - 1]) && ok;
  for (int i = kept - 2; i >= 0; --i) {
    if (!fs.exists(files[i])) continue;
    ok = fs.rename(files[i], files[i + 1]) && ok;
  }
  return ok;
}

// ---- ESP-IDF tag levels during the capture ---------------------------------
// While the capture runs, the firmware's ESP-IDF level calls are recorded here
// instead of lowering the tags, so every tag prints at the build's default
// level (its compiled maximum) and the vprintf hook sees every line. The hook
// asks levelFor() whether the saved levels would have shown a line; when the
// capture ends the recorded calls are replayed, which leaves ESP-IDF where
// those calls would have. A call that does not fit (a tag longer than kMaxTag,
// the table full) is not recorded: set() returns false and the caller applies
// it at once, so the worst case is a tag that is not forced.
class LevelShadow {
 public:
  static constexpr int kMaxEntries = 48;
  static constexpr size_t kMaxTag = 31;

  void reset(int baseDefault) {
    count_ = 0;
    default_ = baseDefault;
    wildcard_ = false;
  }

  // "*" sets the default and drops every tag's own level, as ESP-IDF does.
  bool set(const char* tag, int level) {
    if (!tag) return false;
    if (tag[0] == '*' && tag[1] == '\0') {
      default_ = level;
      wildcard_ = true;
      count_ = 0;
      return true;
    }
    const size_t len = strlen(tag);
    if (len == 0 || len > kMaxTag) return false;
    for (int i = 0; i < count_; ++i) {
      if (!strcmp(e_[i].tag, tag)) {
        e_[i].level = static_cast<int8_t>(level);
        return true;
      }
    }
    if (count_ >= kMaxEntries) return false;
    memcpy(e_[count_].tag, tag, len + 1);
    e_[count_].level = static_cast<int8_t>(level);
    count_++;
    return true;
  }

  // The level the recorded calls give `tag` (len bytes, not terminated).
  int levelFor(const char* tag, size_t len) const {
    for (int i = 0; i < count_; ++i) {
      if (strlen(e_[i].tag) == len && !memcmp(e_[i].tag, tag, len)) return e_[i].level;
    }
    return default_;
  }

  // Calls f(tag, level) for what the recorded calls leave: the wildcard (when
  // one was set) and then every tag, in the order they were first set.
  template <class F>
  void replay(F&& f) const {
    if (wildcard_) f("*", default_);
    for (int i = 0; i < count_; ++i) f(static_cast<const char*>(e_[i].tag), static_cast<int>(e_[i].level));
  }

  int count() const { return count_; }

 private:
  struct Entry {
    char tag[kMaxTag + 1];
    int8_t level;
  };
  Entry e_[kMaxEntries];
  int count_ = 0;
  int default_ = 3;
  bool wildcard_ = false;
};

// An ESP-IDF log line as the v1 logger prints it ("I (1234) tag: text", or
// with an hh:mm:ss.ms time). Gives its level (1 error .. 5 verbose, as
// esp_log_level_t) and its tag; false when the line has another shape (a
// continuation, a raw printf through the logger).
inline bool parseEspLogLine(const char* s, size_t len, int* level, const char** tag, size_t* tagLen) {
  if (!s || len < 6 || s[1] != ' ' || s[2] != '(') return false;
  int lv = 0;
  switch (s[0]) {
    case 'E': lv = 1; break;
    case 'W': lv = 2; break;
    case 'I': lv = 3; break;
    case 'D': lv = 4; break;
    case 'V': lv = 5; break;
    default: return false;
  }
  const char* close = static_cast<const char*>(memchr(s + 3, ')', len - 3));
  if (!close) return false;
  const size_t at = static_cast<size_t>(close - s);
  if (at + 2 >= len || close[1] != ' ') return false;
  const char* t = close + 2;
  const size_t rest = len - (at + 2);
  size_t i = 0;
  while (i < rest && t[i] != ':') ++i;
  if (i == 0 || i == rest) return false;
  if (level) *level = lv;
  if (tag) *tag = t;
  if (tagLen) *tagLen = i;
  return true;
}

}  // namespace bootdebug

#endif  // SYSTEM_BOOT_DEBUG_CORE_H
