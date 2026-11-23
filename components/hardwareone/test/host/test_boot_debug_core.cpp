// The boot debug capture's buffer, file text, rotation and ESP-IDF level
// record (owner decision B13 and its follow-up), from the production header
// System_BootDebugCore.h: whole lines with their uptime, the start of the boot
// kept in the first half and the newest lines going round the second, the gap
// between them counted and marked, the header's measurements, three kept boots
// on a card and one on flash, the recorded tag levels and the ESP-IDF line
// shape. Also the byte-range page boundaries fileview uses for a file of any
// size (System_TextPager.h), the boot file being the largest it pages.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../System_BootDebugCore.h"
#include "../../System_TextPager.h"

using bootdebug::LineBuffer;

#define CHECK(x)                                                                 \
  do {                                                                           \
    if (!(x)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
      return 1;                                                                  \
    }                                                                            \
  } while (0)

// The file's body as the writer puts it together.
static std::string body(const LineBuffer& buf) {
  std::string out(buf.head(), buf.headUsed());
  char gap[bootdebug::kGapMarkMax];
  out.append(gap, bootdebug::formatGapMark(gap, sizeof(gap), buf));
  if (buf.tail1Len()) out.append(buf.tail1(), buf.tail1Len());
  if (buf.tail2Len()) out.append(buf.tail2(), buf.tail2Len());
  return out;
}

static int lines_are_whole_and_stamped() {
  char mem[64];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  buf.append(12, "hello", 5, false);
  buf.append(345, "world", 5, true);
  CHECK(body(buf) == "[12] hello\n[345] world\n");
  CHECK(buf.lines() == 2 && buf.forcedLines() == 1 && buf.keptLines() == 2 && !buf.gap());
  CHECK(buf.totalBytes() == 23 && buf.droppedBytes() == 0 && buf.used() == 23);
  char mark[64];
  CHECK(bootdebug::formatGapMark(mark, sizeof(mark), buf) == 0 && mark[0] == '\0');
  return 0;
}

static int a_multi_line_text_is_one_stamped_line_per_piece() {
  char mem[128];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  buf.append(7, "one\r\ntwo\n", 9, false);  // CRLF inside, a trailing newline adds no line
  buf.append(8, "", 0, false);              // an empty line is still a line
  CHECK(body(buf) == "[7] one\n[7] two\n[8] \n");
  CHECK(buf.lines() == 3);
  // A line's text is cut at the debug line limit.
  std::string longText(400, 'x');
  char big[2048];
  LineBuffer cut;
  cut.attach(big, sizeof(big));
  cut.append(1, longText.c_str(), longText.size(), false);
  CHECK(body(cut) == "[1] " + std::string(bootdebug::kLineMax, 'x') + "\n");
  return 0;
}

// 64 B: the first 32 keep the start, the other 32 go round. Each line here is
// "[n] Lnnnnnnnnn\n" = 15 B.
static int the_start_and_the_newest_lines_are_kept_with_a_gap_between() {
  char mem[64];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  CHECK(buf.headCapacity() == 32 && buf.tailCapacity() == 32);
  const char* text[] = {"L111111111", "L222222222", "L333333333", "L444444444",
                        "L555555555", "L666666666", "L777777777"};
  for (int i = 0; i < 7; ++i) buf.append(static_cast<uint32_t>(i + 1), text[i], 10, i % 2 == 1);
  // Lines 1-2 are the start; 3 and 4 went round first and were dropped as 5,
  // 6 and 7 came (5 landed across the ring's end, 7 evicted it).
  CHECK(buf.lines() == 7 && buf.droppedLines() == 3 && buf.keptLines() == 4 && buf.gap());
  CHECK(buf.totalBytes() == 105 && buf.droppedBytes() == 45 && buf.used() == 60);
  CHECK(buf.forcedLines() == 3);
  const std::string expected =
      "[1] L111111111\n[2] L222222222\n"
      "[GAP] 3 line(s), 45 B of the boot dropped here (the buffer keeps the first 32 B and the newest 32 B)\n"
      "[6] L666666666\n[7] L777777777\n";
  CHECK(body(buf) == expected);
  CHECK(buf.tail2Len() > 0);  // the ring wrapped: two pieces
  return 0;
}

static int a_short_boot_has_no_gap() {
  char mem[64];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  buf.append(1, "L111111111", 10, false);
  buf.append(2, "L222222222", 10, false);
  buf.append(3, "L333333333", 10, false);  // the start is full: this one starts the ring
  CHECK(!buf.gap() && body(buf) == "[1] L111111111\n[2] L222222222\n[3] L333333333\n");
  return 0;
}

static int a_line_larger_than_the_ring_and_no_memory_are_counted() {
  char mem[16];
  LineBuffer tiny;
  tiny.attach(mem, sizeof(mem));
  tiny.append(1, "0123456789", 10, false);  // 15 B: fits neither half
  CHECK(tiny.used() == 0 && tiny.gap() && tiny.droppedLines() == 1 && tiny.droppedBytes() == 15);
  LineBuffer none;
  none.attach(nullptr, 4096);
  CHECK(none.capacity() == 0);
  none.append(1, "a", 1, false);
  CHECK(none.used() == 0 && none.lines() == 1 && none.droppedLines() == 1);
  return 0;
}

static int a_long_boot_keeps_every_kept_line_whole() {
  // Many lines of varying length through a small buffer: the head is the
  // first lines exactly, the tail the newest lines exactly, nothing torn.
  char mem[1000];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  std::vector<std::string> all;
  for (int i = 0; i < 500; ++i) {
    std::string t = "line " + std::to_string(i) + std::string(static_cast<size_t>(i % 37), '.');
    buf.append(static_cast<uint32_t>(i), t.c_str(), t.size(), false);
    all.push_back("[" + std::to_string(i) + "] " + t + "\n");
  }
  std::string head(buf.head(), buf.headUsed());
  std::string expectHead;
  size_t n = 0;
  while (n < all.size() && expectHead.size() + all[n].size() <= buf.headCapacity()) expectHead += all[n++];
  CHECK(head == expectHead);
  std::string tail;
  tail.append(buf.tail1(), buf.tail1Len());
  tail.append(buf.tail2(), buf.tail2Len());
  std::string expectTail;
  for (size_t i = all.size() - (buf.keptLines() - n); i < all.size(); ++i) expectTail += all[i];
  CHECK(tail == expectTail);
  CHECK(tail.size() <= buf.tailCapacity());
  uint64_t total = 0;
  for (const auto& l : all) total += l.size();
  CHECK(buf.totalBytes() == total && buf.totalBytes() == buf.used() + buf.droppedBytes());
  return 0;
}

static int the_header_records_the_measurements() {
  char mem[64];
  LineBuffer buf;
  buf.attach(mem, sizeof(mem));
  for (int i = 1; i <= 7; ++i) buf.append(static_cast<uint32_t>(i * 100), "L123456789", 10, true);  // "[100] ..." = 17 B
  bootdebug::BootInfo info = {};
  info.bootNumber = 42;
  info.resetReason = "power-on";
  info.startMs = 510;
  info.endMs = 9510;
  info.captureUs = 123456;
  info.psram = true;
  char header[bootdebug::kHeaderMax];
  const size_t n = bootdebug::formatHeader(header, sizeof(header), info, buf);
  CHECK(n == std::strlen(header) && n < sizeof(header) - 1);
  CHECK(std::strstr(header, "# boot: #42, reset reason power-on\n"));
  CHECK(std::strstr(header, "# capture: start 510 ms, end 9510 ms, boot took 9000 ms\n"));
  CHECK(std::strstr(header, "# buffer: 64 B in PSRAM (the first 32 B of the boot, then the newest 32 B)\n"));
  CHECK(std::strstr(header, "# lines: 7 in all (7 only because of the boot capture), 2 kept, 5 dropped\n"));
  // The total counts every byte the boot produced, the dropped ones included.
  CHECK(std::strstr(header, "# output: 119 B in all (34 B kept, 85 B dropped)\n"));
  CHECK(std::strstr(header, "# capture time: 123456 us of wall-clock time in the capture path"));
  CHECK(!std::strstr(header, "CPU"));
  CHECK(std::strstr(header, "# GAP: the middle of the boot did not fit"));
  // Every header line is a '#' line.
  for (const char* p = header; *p; p = std::strchr(p, '\n') + 1) CHECK(*p == '#');
  // A small output buffer is cut like snprintf, never overrun.
  char small[40];
  const size_t m = bootdebug::formatHeader(small, sizeof(small), info, buf);
  CHECK(m == sizeof(small) - 1 && std::strlen(small) == m);
  // Without a gap there is no GAP line, and internal RAM is named.
  char mem2[64];
  LineBuffer whole;
  whole.attach(mem2, sizeof(mem2));
  whole.append(1, "x", 1, false);
  info.psram = false;
  bootdebug::formatHeader(header, sizeof(header), info, whole);
  CHECK(!std::strstr(header, "GAP") && std::strstr(header, "in internal RAM"));
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
};

static int three_boots_on_the_card_one_on_flash() {
  using bootdebug::kCardBootFiles;
  using bootdebug::kFlashBootFiles;
  CHECK(bootdebug::kCardKeptBoots == 3 && bootdebug::kFlashKeptBoots == 1);
  CHECK(std::strcmp(kCardBootFiles[0], "/sd/system/sys_logs/boot-debug-1.log") == 0);
  CHECK(std::strcmp(kCardBootFiles[2], "/sd/system/sys_logs/boot-debug-3.log") == 0);
  CHECK(std::strcmp(kFlashBootFiles[0], "/system/sys_logs/boot-debug-1.log") == 0);
  FakeFs card;
  CHECK(bootdebug::rotate(card, kCardBootFiles, bootdebug::kCardKeptBoots));
  CHECK(card.ops.empty());  // first boot: nothing to move
  card.files[kCardBootFiles[0]] = "boot1";
  for (int boot = 2; boot <= 5; ++boot) {
    CHECK(bootdebug::rotate(card, kCardBootFiles, bootdebug::kCardKeptBoots));
    card.files[kCardBootFiles[0]] = "boot" + std::to_string(boot);
  }
  CHECK(card.files.size() == 3);
  CHECK(card.files[kCardBootFiles[0]] == "boot5" && card.files[kCardBootFiles[1]] == "boot4" &&
        card.files[kCardBootFiles[2]] == "boot3");
  card.ops.clear();
  CHECK(bootdebug::rotate(card, kCardBootFiles, bootdebug::kCardKeptBoots));
  CHECK((card.ops == std::vector<std::string>{
      "rm /sd/system/sys_logs/boot-debug-3.log",
      "mv /sd/system/sys_logs/boot-debug-2.log /sd/system/sys_logs/boot-debug-3.log",
      "mv /sd/system/sys_logs/boot-debug-1.log /sd/system/sys_logs/boot-debug-2.log"}));
  // Flash keeps the newest boot only: the old file goes, nothing moves.
  FakeFs flash;
  flash.files[kFlashBootFiles[0]] = "older";
  CHECK(bootdebug::rotate(flash, kFlashBootFiles, bootdebug::kFlashKeptBoots));
  CHECK((flash.ops == std::vector<std::string>{"rm /system/sys_logs/boot-debug-1.log"}) && flash.files.empty());
  // A failed step is reported; the caller still writes this boot's file.
  FakeFs broken;
  broken.files[kCardBootFiles[0]] = "old";
  broken.failRename = true;
  CHECK(!bootdebug::rotate(broken, kCardBootFiles, bootdebug::kCardKeptBoots));
  return 0;
}

static int the_level_record_answers_and_replays_like_esp_idf() {
  static bootdebug::LevelShadow shadow;
  shadow.reset(3);
  CHECK(shadow.levelFor("wifi", 4) == 3);  // unset: the default
  CHECK(shadow.set("wifi", 2) && shadow.set("i2c.master", 0) && shadow.set("wifi", 1));
  CHECK(shadow.levelFor("wifi", 4) == 1 && shadow.levelFor("i2c.master", 10) == 0);
  CHECK(shadow.levelFor("wif", 3) == 3);  // length matters
  std::vector<std::string> replayed;
  auto record = [&](const char* tag, int level) { replayed.push_back(std::string(tag) + "=" + std::to_string(level)); };
  shadow.replay(record);
  CHECK((replayed == std::vector<std::string>{"wifi=1", "i2c.master=0"}));
  // "*" sets the default and drops every tag's own level, as ESP-IDF does.
  CHECK(shadow.set("*", 4) && shadow.set("HTTP_CLIENT", 2));
  CHECK(shadow.levelFor("wifi", 4) == 4 && shadow.levelFor("HTTP_CLIENT", 11) == 2);
  replayed.clear();
  shadow.replay(record);
  CHECK((replayed == std::vector<std::string>{"*=4", "HTTP_CLIENT=2"}));
  // What does not fit is refused, for the caller to apply at once.
  CHECK(!shadow.set("a-tag-that-is-longer-than-thirty-one", 1) && !shadow.set(nullptr, 1) && !shadow.set("", 1));
  shadow.reset(3);
  for (int i = 0; i < bootdebug::LevelShadow::kMaxEntries; ++i) CHECK(shadow.set(("t" + std::to_string(i)).c_str(), 1));
  CHECK(!shadow.set("one-more", 1) && shadow.set("t0", 2) && shadow.count() == bootdebug::LevelShadow::kMaxEntries);
  return 0;
}

static int esp_idf_lines_are_read_for_level_and_tag() {
  struct Case {
    const char* line;
    bool ok;
    int level;
    const char* tag;
  } cases[] = {
      {"I (1234) wifi: wifi driver task: 3fca, prio:23", true, 3, "wifi"},
      {"E (12:34:56.789) i2c.master: I2C transaction unexpected nack detected", true, 1, "i2c.master"},
      {"W (5) C6>: I (310) boot: chip revision", true, 2, "C6>"},
      {"D (99) esp_https_server: x", true, 4, "esp_https_server"},
      {"V (1) t:", true, 5, "t"},
      {"hello world", false, 0, ""},
      {"I(1) tag: x", false, 0, ""},
      {"X (1) tag: x", false, 0, ""},
      {"I (1) : no tag", false, 0, ""},
      {"I (1) no colon at all", false, 0, ""},
  };
  for (const Case& c : cases) {
    int level = 0;
    const char* tag = nullptr;
    size_t tagLen = 0;
    const bool ok = bootdebug::parseEspLogLine(c.line, std::strlen(c.line), &level, &tag, &tagLen);
    CHECK(ok == c.ok);
    if (ok) CHECK(level == c.level && std::string(tag, tagLen) == c.tag);
  }
  return 0;
}

static int file_pages_split_at_line_ends_and_cover_the_file() {
  CHECK(filePageCount(0, 100) == 1 && filePageCount(100, 100) == 1 && filePageCount(101, 100) == 2);
  std::string file;
  for (int i = 0; file.size() < 5000; ++i) file += "row " + std::to_string(i) + std::string(size_t(i % 50), '-') + "\n";
  const size_t stride = 700, search = 120;
  const size_t pages = filePageCount(file.size(), stride);
  size_t prev = 0;
  std::string rebuilt;
  for (size_t k = 0; k < pages; ++k) {
    const size_t start = filePageBoundary(k, stride, file.size(), file.data() + k * stride,
                                          file.size() - k * stride, search);
    const size_t end = k + 1 < pages ? filePageBoundary(k + 1, stride, file.size(), file.data() + (k + 1) * stride,
                                                        file.size() - (k + 1) * stride, search)
                                     : file.size();
    CHECK(start == prev && end > start);
    CHECK(start == 0 || file[start - 1] == '\n');  // every page starts a line
    rebuilt += file.substr(start, end - start);
    prev = end;
  }
  CHECK(rebuilt == file);
  // A line longer than the search window is cut at the nominal start.
  const std::string longLine(1000, 'x');
  CHECK(filePageBoundary(1, 300, longLine.size(), longLine.data() + 300, 700, 100) == 300);
  CHECK(filePageBoundary(5, 300, longLine.size(), nullptr, 0, 100) == longLine.size());
  return 0;
}

int main() {
  if (lines_are_whole_and_stamped()) return 1;
  if (a_multi_line_text_is_one_stamped_line_per_piece()) return 1;
  if (the_start_and_the_newest_lines_are_kept_with_a_gap_between()) return 1;
  if (a_short_boot_has_no_gap()) return 1;
  if (a_line_larger_than_the_ring_and_no_memory_are_counted()) return 1;
  if (a_long_boot_keeps_every_kept_line_whole()) return 1;
  if (the_header_records_the_measurements()) return 1;
  if (three_boots_on_the_card_one_on_flash()) return 1;
  if (the_level_record_answers_and_replays_like_esp_idf()) return 1;
  if (esp_idf_lines_are_read_for_level_and_tag()) return 1;
  if (file_pages_split_at_line_ends_and_cover_the_file()) return 1;
  std::puts("boot debug core: all cases passed");
  return 0;
}
