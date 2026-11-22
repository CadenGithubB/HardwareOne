// The boot debug capture's buffer, file text and rotation (owner decision
// B13), from the production header System_BootDebugCore.h: whole lines with
// their uptime, the first lines kept once the buffer is full, the truncation
// counted and marked, the header's measurements, and three kept boots.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../System_BootDebugCore.h"

using bootdebug::LineBuffer;

#define CHECK(x)                                                                 \
  do {                                                                           \
    if (!(x)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
      return 1;                                                                  \
    }                                                                            \
  } while (0)

static int lines_are_whole_and_stamped() {
  char mem[64];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  CHECK(buf.append(12, "hello", 5, false));
  CHECK(buf.append(345, "world", 5, true));
  const std::string text(buf.data(), buf.used());
  CHECK(text == "[12] hello\n[345] world\n");
  CHECK(buf.lines() == 2 && buf.forcedLines() == 1 && !buf.truncated());
  char mark[64];
  CHECK(bootdebug::formatTruncationMark(mark, sizeof(mark), buf) == 0 && mark[0] == '\0');
  return 0;
}

static int a_full_buffer_keeps_the_first_lines_and_counts_the_rest() {
  char mem[40];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  CHECK(buf.append(1, "0123456789", 10, false));   // "[1] 0123456789\n" = 15
  CHECK(buf.append(2, "0123456789", 10, true));    // 30
  CHECK(!buf.append(3, "0123456789", 10, true));   // 45 > 40: dropped
  // A later line that would still fit is dropped too: the file stays one
  // unbroken stretch from the start of boot.
  CHECK(!buf.append(4, "x", 1, false));
  CHECK(buf.used() == 30 && buf.lines() == 2 && buf.truncated());
  CHECK(buf.droppedLines() == 2 && buf.droppedBytes() == 15 + 6 && buf.fullAtMs() == 3);
  char mark[160];
  const size_t n = bootdebug::formatTruncationMark(mark, sizeof(mark), buf);
  CHECK(n == std::strlen(mark));
  CHECK(std::strstr(mark, "[TRUNCATED] 2 more line(s), 21 B, did not fit in the 40 B buffer (full at 3 ms)") == mark);
  CHECK(mark[n - 1] == '\n');
  return 0;
}

static int an_exact_fit_is_kept() {
  char mem[15];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  CHECK(buf.append(1, "0123456789", 10, false));  // exactly 15
  CHECK(buf.used() == 15 && !buf.truncated());
  CHECK(!buf.append(2, "", 0, false));
  CHECK(buf.truncated() && buf.droppedLines() == 1);
  return 0;
}

static int no_memory_captures_nothing() {
  LineBuffer buf;
  buf.attach(nullptr, 4096);
  CHECK(buf.capacity() == 0);
  CHECK(!buf.append(1, "a", 1, false));
  CHECK(buf.used() == 0 && buf.truncated());
  return 0;
}

static int the_header_records_the_measurements() {
  char mem[32];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  buf.append(100, "kept", 4, true);         // "[100] kept\n" = 11
  buf.append(200, "a longer line that will not fit", 31, true);
  bootdebug::BootInfo info = {};
  info.bootNumber = 42;
  info.resetReason = "power-on";
  info.startMs = 510;
  info.endMs = 9510;
  info.captureUs = 123456;
  info.psram = true;
  char header[768];
  const size_t n = bootdebug::formatHeader(header, sizeof(header), info, buf);
  CHECK(n == std::strlen(header) && n < sizeof(header) - 1);
  CHECK(std::strstr(header, "# boot: #42, reset reason power-on\n"));
  CHECK(std::strstr(header, "# capture: start 510 ms, end 9510 ms, boot took 9000 ms\n"));
  CHECK(std::strstr(header, "# buffer: 32 B in PSRAM, 11 B used\n"));
  CHECK(std::strstr(header, "# lines: 1 kept (1 shown only because of the boot capture), 1 dropped\n"));
  CHECK(std::strstr(header, "# output: 49 B in all (11 B kept, 38 B dropped)\n"));
  CHECK(std::strstr(header, "# capture cost: 123456 us of CPU"));
  CHECK(std::strstr(header, "# TRUNCATED: the buffer filled at 200 ms"));
  // Every header line is a '#' line.
  for (const char* p = header; *p; p = std::strchr(p, '\n') + 1) CHECK(*p == '#');
  // A small output buffer is cut like snprintf, never overrun.
  char small[40];
  const size_t m = bootdebug::formatHeader(small, sizeof(small), info, buf);
  CHECK(m == sizeof(small) - 1 && std::strlen(small) == m);
  // Without truncation there is no TRUNCATED line, and internal RAM is named.
  char mem2[64];
  LineBuffer whole;
  whole.attach(mem2, sizeof(mem2));
  whole.append(1, "x", 1, false);
  info.psram = false;
  bootdebug::formatHeader(header, sizeof(header), info, whole);
  CHECK(!std::strstr(header, "TRUNCATED") && std::strstr(header, "in internal RAM"));
  return 0;
}

// A fake volume: name -> content.
struct FakeFs {
  std::map<std::string, std::string> files;
  std::vector<std::string> ops;
  bool failRename = false;
  bool exists(const char* p) { return files.count(p) != 0; }
  bool remove(const char* p) {
    ops.push_back(std::string("rm ") + p);
    return files.erase(p) == 1;
  }
  bool rename(const char* from, const char* to) {
    ops.push_back(std::string("mv ") + from + " " + to);
    if (failRename) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  // What the writer does after the rotation.
  void write(const std::string& content) { files[bootdebug::kBootFiles[0]] = content; }
};

static int three_boots_are_kept_newest_first() {
  CHECK(bootdebug::kKeptBoots == 3);
  CHECK(std::strcmp(bootdebug::kBootFiles[0], "/system/sys_logs/boot-debug-1.log") == 0);
  CHECK(std::strcmp(bootdebug::kBootFiles[2], "/system/sys_logs/boot-debug-3.log") == 0);
  FakeFs fs;
  // First boot: nothing to move.
  CHECK(bootdebug::rotate(fs));
  CHECK(fs.ops.empty());
  fs.write("boot1");
  for (int boot = 2; boot <= 5; ++boot) {
    CHECK(bootdebug::rotate(fs));
    fs.write("boot" + std::to_string(boot));
  }
  CHECK(fs.files.size() == 3);
  CHECK(fs.files["/system/sys_logs/boot-debug-1.log"] == "boot5");
  CHECK(fs.files["/system/sys_logs/boot-debug-2.log"] == "boot4");
  CHECK(fs.files["/system/sys_logs/boot-debug-3.log"] == "boot3");
  // The oldest goes first, then each moves down one place.
  fs.ops.clear();
  CHECK(bootdebug::rotate(fs));
  CHECK((fs.ops == std::vector<std::string>{
      "rm /system/sys_logs/boot-debug-3.log",
      "mv /system/sys_logs/boot-debug-2.log /system/sys_logs/boot-debug-3.log",
      "mv /system/sys_logs/boot-debug-1.log /system/sys_logs/boot-debug-2.log"}));
  // A failed step is reported; the caller still writes this boot's file.
  FakeFs broken;
  broken.write("old");
  broken.failRename = true;
  CHECK(!bootdebug::rotate(broken));
  return 0;
}

int main() {
  if (lines_are_whole_and_stamped()) return 1;
  if (a_full_buffer_keeps_the_first_lines_and_counts_the_rest()) return 1;
  if (an_exact_fit_is_kept()) return 1;
  if (no_memory_captures_nothing()) return 1;
  if (the_header_records_the_measurements()) return 1;
  if (three_boots_are_kept_newest_first()) return 1;
  std::puts("boot debug core: all cases passed");
  return 0;
}
