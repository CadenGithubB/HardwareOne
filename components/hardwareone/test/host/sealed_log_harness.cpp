// Executes the production System_SealedLog.cpp (B11: log files on the card are
// sealed) over the shared fake VFS. The device key and the cipher are the toy
// ones of sealed_log_host.h ("ENC1:" + base64, a fixed key id); everything
// else is production: the mark first in a new file, a file that is not this
// device's moved aside, one sealed row per line, lines put together across
// pieces, and whole rows read back by stored offset.
#include <Arduino.h>  // the web_batch_stubs String double

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#include "fake_vfs.h"
void webBatchTestStringAppend() {}

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } \
  ++checks; } while (0)

// Platform seams of the module.
#define EXT_RAM_BSS_ATTR
enum class AllocPref { PreferPsram, RequirePsram };
static std::vector<std::unique_ptr<char[]>> blocks;
static std::vector<std::string> allocTags;
static bool failAlloc = false;
static int frees = 0;
static void* ps_alloc(size_t size, AllocPref pref, const char* tag) {
  CHECK(pref == AllocPref::PreferPsram && tag && std::strncmp(tag, "sealedlog.", 10) == 0);  // PSRAM first, tagged
  CHECK(size <= 4097);
  allocTags.emplace_back(tag);
  if (failAlloc) return nullptr;
  blocks.push_back(std::make_unique<char[]>(size));
  return blocks.back().get();
}
static void ps_free(void* block) { if (block) ++frees; }
static std::vector<std::string> eventLines;
static void logSystemEvent(const char*, const char* format, ...) {
  char line[320];
  va_list args; va_start(args, format); std::vsnprintf(line, sizeof(line), format, args); va_end(args);
  eventLines.emplace_back(line);
}
namespace VFS {
static uint32_t mountGeneration = 1;
inline uint32_t sdMountGeneration() { return mountGeneration; }
}

// The device key and the cipher.
#define SEALED_LOG_HOST_CRYPTO_ONLY
#define SEALED_LOG_HOST_MARK_API
#define SEALED_LOG_HOST_CRYPTO_API
#include "sealed_log_host.h"
// INSERT_CAPCRYPT_MACROS

// INSERT_SEALED_LOG

using sealed_log_host::mark;
using sealed_log_host::reveal;
using sealed_log_host::sealRow;

static fake_vfs::Fs& fs() { return fake_vfs::fs(); }
static const char* const kLog = "/sd/system/sys_logs/errors.log";
static AuthContext sys() { return VFS::systemAuth("/sd/system/sys_logs", "unit"); }
static void reset() {
  CHECK(fs().liveHandles() == 0);
  fs().reset();
  fs().mkdirAll("/sd/system/sys_logs");
  fs().mkdirAll("/sd/stt/u7");
  eventLines.clear();
  sealed_log_host::keyAvailable = true;
  sealed_log_host::sealFailAt = 0; sealed_log_host::sealCalls = 0; sealed_log_host::movedAside = 0;
  failAlloc = false;
  ++VFS::mountGeneration;   // a new mount: no head remembered from the case before
}
static std::string rowsOf(const std::string& text) {  // the stored rows of plain lines
  std::string out;
  for (size_t at = 0; at < text.size();) {
    const size_t end = text.find('\n', at);
    out += sealRow(text.substr(at, end - at));
    at = end + 1;
  }
  return out;
}

static void appendAndForeign() {
  // Where it applies: the card, by the VFS's own routing.
  CHECK(SealedLog::appliesTo(String("/sd/system/sys_logs/errors.log")) && SealedLog::appliesTo(String("/sd/stt/u7/a.txt")));
  CHECK(!SealedLog::appliesTo(String("/system/sys_logs/errors.log")) && !SealedLog::appliesTo(String("/sdcard/x")) &&
        !SealedLog::appliesTo(String("/logging_captures/sd/x.csv")));

  // A new file: the mark, then rows.
  reset();
  size_t stored = 99;
  CHECK(SealedLog::inspect(kLog, sys()) == SealedLog::Head::Absent);
  File f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(f && stored == mark().size() && fs().text(kLog) == mark());
  CHECK(SealedLog::writeLines(f, "first line\r\nsecond line\n", 24, stored));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow("first line") + sealRow("second line") && stored == fs().text(kLog).size());
  CHECK(reveal(fs().text(kLog)) == "first line\nsecond line\n" && fs().text(kLog).find("first") == std::string::npos);
  CHECK(SealedLog::inspect(kLog, sys()) == SealedLog::Head::Ours);

  // The head of a file found ours is read once per mount; a later open
  // appends without a read, and a new mount reads it again.
  unsigned reads = fs().count("open", kLog);
  for (int i = 0; i < 3; ++i) {
    f = SealedLog::openAppend(kLog, sys(), stored);
    CHECK(f && stored == fs().text(kLog).size());
    CHECK(SealedLog::writeLines(f, "more", 4, stored));
    f.close();
  }
  CHECK(fs().count("open", kLog) == reads + 3);        // three append opens, no head read
  ++VFS::mountGeneration;
  f = SealedLog::openAppend(kLog, sys(), stored); f.close();
  CHECK(fs().count("open", kLog) == reads + 5);        // the head read, then the append open
  // A file removed meanwhile (inside the mount) is caught by its size: the mark again.
  CHECK(fs().remove(kLog, {}));
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(f && fs().text(kLog) == mark() && stored == mark().size());
  f.close();

  // Not this device's file: moved aside unchanged; nothing in it read beyond
  // its head, converted or appended to. One event line each.
  const std::string cases[] = {
      "#HW1ENC v1 k1 id=fedcba9876543210\n" + sealRow("their row"),   // another device's key id
      "[2026-11-22 10:00:00] | a plain log\r\n",                        // no mark
      "#HW1ENC v1 k1\n" + sealRow("old"),                              // a mark that names no key
      "#HW1ENC v1 k1 id=0123",                                         // a torn mark
  };
  for (const std::string& foreign : cases) {
    reset();
    fs().put(kLog, foreign);
    CHECK(SealedLog::inspect(kLog, sys()) == SealedLog::Head::NotOurs);
    f = SealedLog::openAppend(kLog, sys(), stored);
    CHECK(f && SealedLog::writeLines(f, "ours", 4, stored));
    f.close();
    CHECK(fs().text(std::string(kLog) + ".foreign-1") == foreign);
    CHECK(fs().text(kLog) == mark() + sealRow("ours"));
    CHECK(SealedLog::movedAsideCount() >= 1 && eventLines.size() == 1 &&
          eventLines[0].find("moved aside to /sd/system/sys_logs/errors.log.foreign-1") != std::string::npos);
  }
  // Names already taken are skipped, never replaced; with twenty taken the open fails.
  reset();
  fs().put(kLog, "plain\n");
  fs().put(std::string(kLog) + ".foreign-1", "one");
  fs().put(std::string(kLog) + ".foreign-2", "two");
  f = SealedLog::openAppend(kLog, sys(), stored); f.close();
  CHECK(fs().text(std::string(kLog) + ".foreign-3") == "plain\n" && fs().text(std::string(kLog) + ".foreign-1") == "one");
  reset();
  fs().put(kLog, "plain\n");
  for (unsigned n = 1; n <= sealedlog::kForeignNameTries; ++n) fs().put(std::string(kLog) + ".foreign-" + std::to_string(n), "x");
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(!f && fs().text(kLog) == "plain\n");            // not appended to, not removed
  // The rename fails: the same.
  reset();
  fs().put(kLog, "plain\n");
  fs().faults.rename.add(kLog);
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(!f && fs().text(kLog) == "plain\n" && !fs().exists(std::string(kLog) + ".foreign-1"));
  // An empty file is ours to start.
  reset();
  fs().put(kLog, "");
  CHECK(SealedLog::inspect(kLog, sys()) == SealedLog::Head::Empty);
  f = SealedLog::openAppend(kLog, sys(), stored); f.close();
  CHECK(fs().text(kLog) == mark());

  // No device key: nothing is created, moved or written.
  reset();
  sealed_log_host::keyAvailable = false;
  fs().put("/sd/system/sys_logs/other.log", "plain\n");
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(!f && !fs().exists(kLog));
  f = SealedLog::openAppend("/sd/system/sys_logs/other.log", sys(), stored);
  CHECK(!f && fs().text("/sd/system/sys_logs/other.log") == "plain\n" && eventLines.empty());
  f = SealedLog::openFresh(kLog, sys(), stored);
  CHECK(!f && !fs().exists(kLog));
  // The card does not open the file.
  reset();
  fs().faults.open.add(kLog);
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(!f);
  // The mark itself is cut short (a pull): the open fails, and what is left
  // is not taken for our file at the next mount.
  reset();
  fs().faults.shortWriteAfter[kLog] = 10;
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(!f && fs().text(kLog).size() == 10);
  fs().faults.clear(); ++VFS::mountGeneration;
  f = SealedLog::openAppend(kLog, sys(), stored); f.close();
  CHECK(fs().text(kLog) == mark() && fs().text(std::string(kLog) + ".foreign-1").size() == 10);
}

static void rowsAndFailures() {
  size_t stored = 0;
  // A line over 1023 bytes: rows of 1023 or less, nothing dropped; an empty
  // line is an empty row; text after the last line end is a row.
  reset();
  File f = SealedLog::openAppend(kLog, sys(), stored);
  std::string longLine(2100, 'x');
  for (size_t i = 0; i < longLine.size(); i += 61) longLine[i] = char('a' + i % 26);
  const std::string batch = "short\r\n\r\n" + longLine + "\r\ntail";
  CHECK(SealedLog::writeLines(f, batch.data(), batch.size(), stored));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow("short") + sealRow("") + sealRow(longLine.substr(0, 1023)) +
                               sealRow(longLine.substr(1023, 1023)) + sealRow(longLine.substr(2046)) + sealRow("tail"));
  CHECK(stored == fs().text(kLog).size());
  // A caller's smaller row limit (the transcript writer).
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(SealedLog::writeLines(f, longLine.data(), 512, stored, 480));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow(longLine.substr(0, 480)) + sealRow(longLine.substr(480, 32)));
  // Each row is one write: a card that takes only part of a write leaves the
  // rows before it whole and nothing plain.
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  const size_t afterTwo = (mark() + sealRow("row one") + sealRow("row two")).size();
  fs().faults.shortWriteAfter[kLog] = afterTwo + 6;
  CHECK(!SealedLog::writeLines(f, "row one\nrow two\nrow three\nrow four\n", 35, stored));
  f.close();
  CHECK(fs().text(kLog).size() == afterTwo + 6 && stored == afterTwo + 6);
  CHECK(reveal(fs().text(kLog)).rfind("row one\nrow two\n", 0) == 0 && fs().text(kLog).find("row three") == std::string::npos);
  CHECK(fs().count("write", kLog) == 4);                 // the mark, two rows, the torn third; the fourth is not tried
  // The seal fails: nothing of that line is written, plain or sealed.
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  sealed_log_host::sealFailAt = sealed_log_host::sealCalls + 2;
  CHECK(!SealedLog::writeLines(f, "kept\nlost\n", 10, stored));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow("kept"));
  // No row buffer: a failure, nothing written.
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(SealedLog::writeLines(f, nullptr, 0, stored));   // nothing to write is not a failure
  f.close();

  // openFresh: this device's own file starts anew; one that is not ours is
  // moved aside, never removed.
  reset();
  fs().put(kLog, mark() + sealRow("yesterday"));
  f = SealedLog::openFresh(kLog, sys(), stored);
  CHECK(f && fs().text(kLog) == mark() && stored == mark().size() && !fs().exists(std::string(kLog) + ".foreign-1"));
  f.close();
  reset();
  fs().put(kLog, "#HW1ENC v1 k1 id=fedcba9876543210\n" + sealRow("theirs"));
  f = SealedLog::openFresh(kLog, sys(), stored);
  CHECK(f && fs().text(kLog) == mark() && fs().exists(std::string(kLog) + ".foreign-1"));
  f.close();
  reset();
  f = SealedLog::openFresh(kLog, sys(), stored);
  CHECK(f && fs().text(kLog) == mark());
  f.close();

  // writeMark into a file the caller created (a rotation's copy, a staged header).
  reset();
  f = VFS::openGuarded("/sd/system/sys_logs/errors.log.tmp", "w", sys(), true);
  stored = 0;
  CHECK(SealedLog::writeMark(f, stored) && stored == mark().size());
  f.close();
  CHECK(fs().text("/sd/system/sys_logs/errors.log.tmp") == mark());
  sealed_log_host::keyAvailable = false;
  f = VFS::openGuarded("/sd/system/sys_logs/errors.log.tmp", "w", sys(), true);
  stored = 0;
  CHECK(!SealedLog::writeMark(f, stored) && stored == 0);
  f.close();
  CHECK(fs().text("/sd/system/sys_logs/errors.log.tmp").empty());
}

// Text that arrives in pieces which do not end at line ends (the boot debug
// buffer's chunks): a line is one row however the pieces fall.
static void lineStream() {
  std::string text;
  for (int i = 0; i < 60; ++i) text += "[" + std::to_string(i * 37) + "] line " + std::to_string(i) + std::string(i % 17, '.') + "\n";
  text += "\n";                                   // an empty line
  text += std::string(1500, 'L') + "\n";          // a line over one row
  text += "a line with CR\r\n";
  text += "the last line, unterminated";
  // Expected rows: as one writeLines call over the whole text would give.
  size_t stored = 0;
  reset();
  File whole = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(SealedLog::writeLines(whole, text.data(), text.size(), stored));
  whole.close();
  const std::string expected = fs().text(kLog);
  for (size_t piece : {size_t(1), size_t(2), size_t(7), size_t(64), size_t(1000), size_t(1023), size_t(1024), size_t(4096)}) {
    reset();
    File f = SealedLog::openAppend(kLog, sys(), stored);
    {
      SealedLog::LineStream lines(f);
      for (size_t at = 0; at < text.size(); at += piece) {
        CHECK(lines.write(text.data() + at, std::min(piece, text.size() - at)));
      }
      CHECK(lines.finish() && lines.stored() + mark().size() == expected.size());
    }
    f.close();
    CHECK(fs().text(kLog) == expected);
  }
  // A line that never ends within a piece and outgrows a row is still kept whole in order.
  reset();
  File f = SealedLog::openAppend(kLog, sys(), stored);
  {
    SealedLog::LineStream lines(f);
    const std::string endless(3000, 'e');
    for (size_t at = 0; at < endless.size(); at += 100) CHECK(lines.write(endless.data() + at, 100));
    CHECK(lines.finish());
    f.close();
    std::string joined = reveal(fs().text(kLog));
    joined.erase(std::remove(joined.begin(), joined.end(), '\n'), joined.end());
    CHECK(joined == endless);
  }
  // A failure stays a failure, and nothing more is written.
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  {
    SealedLog::LineStream lines(f);
    CHECK(lines.write("one\ntwo\n", 8));
    fs().faults.shortWriteAfter[kLog] = fs().text(kLog).size() + 3;
    CHECK(!lines.write("three\nfour\n", 11) && !lines.write("five\n", 5) && !lines.finish());
  }
  f.close();
  CHECK(fs().text(kLog).find("five") == std::string::npos && reveal(fs().text(kLog)).rfind("one\ntwo\n", 0) == 0);
  // The carry buffer is PSRAM and is freed.
  CHECK(std::count(allocTags.begin(), allocTags.end(), "sealedlog.carry") > 0 && frees > 0);
}

// What a reader pages through: whole rows from a stored offset.
static std::string readAll(const char* path, size_t window, size_t* windows = nullptr, size_t maxWindowUsed = 0) {
  (void)maxWindowUsed;
  File f = VFS::openGuarded(path, "r", sys());
  CHECK(bool(f));
  std::string shown;
  std::vector<char> out(window);
  size_t offset = 0, count = 0;
  const size_t size = f.size();
  for (;;) {
    size_t got = 0, next = 0;
    CHECK(SealedLog::readWindow(f, offset, out.data(), out.size(), got, next));
    CHECK(got < window && out[got] == '\0' && next >= offset && next <= size);
    shown.append(out.data(), got);
    ++count;
    if (next == size) break;
    CHECK(next > offset);                        // always forward
    CHECK(fs().text(path)[next - 1] == '\n');    // a row boundary of the stored file
    offset = next;
  }
  f.close();
  if (windows) *windows = count;
  return shown;
}
static void readBack() {
  // A sealed transcript: the header and the phrases, as its writer stores them.
  std::string plain = "HardwareOne transcript\nStarted: 20261122T101500Z\nBackend: local\nTitle: first words\n"
                      "An End marker below confirms finalization.\n\n";
  for (int i = 0; i < 40; ++i) plain += "phrase " + std::to_string(i) + " " + std::string(40 + (i * 13) % 400, char('a' + i % 26)) + "\n";
  plain += "\n[End: done]\n";
  reset();
  const char* path = "/sd/stt/u7/20261122T101500Z-local-0000000100000002.txt";
  fs().put(path, mark() + rowsOf(plain));
  File f = VFS::openGuarded(path, "r", sys());
  CHECK(SealedLog::looksSealed(f) && f.position() == 0);
  f.close();
  size_t windows = 0;
  CHECK(readAll(path, 513, &windows) == plain && windows > 10);    // the transcript reader's window
  CHECK(readAll(path, 4096) == plain);
  CHECK(readAll(path, 1100) == plain);
  // At and past the end.
  f = VFS::openGuarded(path, "r", sys());
  char out[64];
  size_t got = 9, next = 9;
  CHECK(SealedLog::readWindow(f, f.size(), out, sizeof(out), got, next) && got == 0 && next == f.size() && out[0] == '\0');
  CHECK(!SealedLog::readWindow(f, f.size() + 1, out, sizeof(out), got, next));
  CHECK(!SealedLog::readWindow(f, 0, out, 1, got, next) && !SealedLog::readWindow(f, 0, nullptr, 64, got, next));
  f.close();
  // A plain schema line passes through; a row that does not open says so;
  // a torn last row (no line end) too.
  reset();
  fs().put(kLog, mark() + "timestamp_ms,gps_fix\n" + sealRow("1,1") + "ENC1:AAA\n" + sealRow("2,0") + "ENC1:torn");
  CHECK(readAll(kLog, 513) == "timestamp_ms,gps_fix\n1,1\n[undecryptable row]\n2,0\n[undecryptable row]");
  // Another device's file: one line says whose it is, and the read ends.
  reset();
  fs().put(kLog, "#HW1ENC v1 k1 id=fedcba9876543210\n" + sealRow("their words") + sealRow("more of them"));
  CHECK(readAll(kLog, 513) == "[sealed by device fedcba9876543210: its rows can only be read on that device]\n");
  // Without this device's key every row reads as undecryptable (the id cannot be told from ours).
  reset();
  fs().put(kLog, mark() + sealRow("ours"));
  sealed_log_host::keyAvailable = false;
  CHECK(readAll(kLog, 513) == "[undecryptable row]\n");
  sealed_log_host::keyAvailable = true;
  // A row wider than the whole window is cut to it (the transcript writer
  // keeps its rows under the reader's window, so this is a foreign shape).
  reset();
  fs().put(kLog, mark() + sealRow(std::string(600, 'w')) + sealRow("after"));
  f = VFS::openGuarded(kLog, "r", sys());
  char narrow[101];
  CHECK(SealedLog::readWindow(f, 0, narrow, sizeof(narrow), got, next) && got == 100 && narrow[99] == '\n');
  CHECK(SealedLog::readWindow(f, next, narrow, sizeof(narrow), got, next) && std::string(narrow) == "after\n" && next == f.size());
  f.close();
  // A read the card fails is a failure with no text.
  reset();
  fs().put(kLog, mark() + sealRow("text"));
  fs().faults.seek.add(kLog);
  f = VFS::openGuarded(kLog, "r", sys());
  std::memset(out, 'x', sizeof(out));
  CHECK(!SealedLog::readWindow(f, mark().size(), out, sizeof(out), got, next) && got == 0 && out[0] == '\0' && next == mark().size());
  f.close();
  // The plain scratch and the read stage are PSRAM blocks; the stage is freed each call.
  CHECK(std::count(allocTags.begin(), allocTags.end(), "sealedlog.read") > 0 &&
        std::count(allocTags.begin(), allocTags.end(), "sealedlog.plain") == 1 &&
        std::count(allocTags.begin(), allocTags.end(), "sealedlog.row") == 1);
}

int main() {
  appendAndForeign();
  rowsAndFailures();
  lineStream();
  readBack();
  CHECK(fs().liveHandles() == 0);
  std::printf("sealed log: %d checks passed (production System_SealedLog.cpp over the fake VFS)\n", checks);
  return 0;
}
