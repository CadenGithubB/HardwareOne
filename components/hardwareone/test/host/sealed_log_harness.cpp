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
  fs().mkdirAll("/sd/stt/u8");
  fs().mkdirAll("/sd/logging_captures/sensors");
  fs().mkdirAll("/sd/notes");
  eventLines.clear();
  sealed_log_host::keyAvailable = true;
  sealed_log_host::sealFailAt = 0; sealed_log_host::sealCalls = 0; sealed_log_host::movedAside = 0;
  failAlloc = false;
  ++VFS::mountGeneration;   // a new mount: no head remembered from the case before
}
static std::string rowsOf(const std::string& text, const char* path) {  // the stored rows of plain lines, sealed for `path`
  std::string out;
  for (size_t at = 0; at < text.size();) {
    const size_t end = text.find('\n', at);
    out += sealRow(text.substr(at, end - at), path);
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
  CHECK(SealedLog::writeLines(f, kLog, "first line\r\nsecond line\n", 24, stored));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow("first line") + sealRow("second line") && stored == fs().text(kLog).size());
  CHECK(reveal(fs().text(kLog)) == "first line\nsecond line\n" && fs().text(kLog).find("first") == std::string::npos);
  CHECK(SealedLog::inspect(kLog, sys()) == SealedLog::Head::Ours);

  // The head and the tail of a file found ours are read once per mount; a
  // later open after a settled write appends without a read, and a new mount
  // reads them again.
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(f && SealedLog::writeLines(f, kLog, "settled", 7, stored) && SealedLog::settle(f, kLog, stored));
  f.close();
  unsigned reads = fs().count("open", kLog);
  for (int i = 0; i < 3; ++i) {
    f = SealedLog::openAppend(kLog, sys(), stored);
    CHECK(f && stored == fs().text(kLog).size());
    CHECK(SealedLog::writeLines(f, kLog, "more", 4, stored) && SealedLog::settle(f, kLog, stored));
    f.close();
  }
  CHECK(fs().count("open", kLog) == reads + 3);        // three append opens, no head or tail read
  ++VFS::mountGeneration;
  f = SealedLog::openAppend(kLog, sys(), stored); f.close();
  CHECK(fs().count("open", kLog) == reads + 6);        // the head read, the tail read, then the append open
  // A write that was not settled leaves the tail unknown: the next open looks.
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(f && SealedLog::writeLines(f, kLog, "unsettled", 9, stored));
  f.close();
  reads = fs().count("open", kLog);
  f = SealedLog::openAppend(kLog, sys(), stored); f.close();
  CHECK(fs().count("open", kLog) == reads + 2);        // the tail read, then the append open
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
    CHECK(f && SealedLog::writeLines(f, kLog, "ours", 4, stored));
    f.close();
    CHECK(fs().text(std::string(kLog) + ".foreign-1") == foreign);
    CHECK(fs().text(kLog) == mark() + sealRow("ours"));
    CHECK(SealedLog::movedAsideCount() >= 1 && eventLines.size() == 1 &&
          eventLines[0].find("moved aside to /sd/system/sys_logs/errors.log.foreign-1") != std::string::npos);
  }
  // Names already taken are skipped while a free one is left.
  reset();
  fs().put(kLog, "plain\n");
  fs().put(std::string(kLog) + ".foreign-1", "one");
  fs().put(std::string(kLog) + ".foreign-2", "two");
  f = SealedLog::openAppend(kLog, sys(), stored); f.close();
  CHECK(fs().text(std::string(kLog) + ".foreign-3") == "plain\n" && fs().text(std::string(kLog) + ".foreign-1") == "one");
  // With all twenty taken the one written longest ago makes room, so a file
  // that keeps turning up cannot keep its log off the card for good.
  reset();
  for (unsigned n = 1; n <= sealedlog::kForeignNameTries; ++n) {
    fs().now = 1700000000 + (n == 7 ? 0 : 100 + n);    // number 7 is the oldest
    fs().put(std::string(kLog) + ".foreign-" + std::to_string(n), "old " + std::to_string(n));
  }
  fs().now = 1700009000;
  fs().put(kLog, "plain\n");
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(f && fs().text(kLog) == mark());
  f.close();
  CHECK(fs().text(std::string(kLog) + ".foreign-7") == "plain\n");
  for (unsigned n = 1; n <= sealedlog::kForeignNameTries; ++n) {
    if (n != 7) CHECK(fs().text(std::string(kLog) + ".foreign-" + std::to_string(n)) == "old " + std::to_string(n));
  }
  CHECK(eventLines.size() == 1 && eventLines[0].find("foreign-7 (the oldest set-aside file was replaced)") != std::string::npos);
  // The oldest cannot be removed: the open fails; nothing appended to, nothing lost.
  reset();
  for (unsigned n = 1; n <= sealedlog::kForeignNameTries; ++n) fs().put(std::string(kLog) + ".foreign-" + std::to_string(n), "x");
  fs().put(kLog, "plain\n");
  for (unsigned n = 1; n <= sealedlog::kForeignNameTries; ++n) fs().faults.remove.add(std::string(kLog) + ".foreign-" + std::to_string(n));
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(!f && fs().text(kLog) == "plain\n");

  // Outside the log trees a file this device did not make is left alone: not
  // moved, not appended to (an automation log started on some card path).
  reset();
  const char* const elsewhere = "/sd/notes/shopping.txt";
  for (const std::string& theirs : {std::string("milk\neggs\n"), "#HW1ENC v1 k1 id=fedcba9876543210\n" + sealRow("x", elsewhere)}) {
    fs().put(elsewhere, theirs);
    CHECK(SealedLog::inspect(elsewhere, VFS::systemAuth("/sd/notes", "unit")) == SealedLog::Head::NotOurs);
    CHECK(!SealedLog::moveAside(elsewhere, VFS::systemAuth("/sd/notes", "unit")));
    f = SealedLog::openAppend(elsewhere, VFS::systemAuth("/sd/notes", "unit"), stored);
    CHECK(!f && fs().text(elsewhere) == theirs && !fs().exists(std::string(elsewhere) + ".foreign-1") && eventLines.empty());
    f = SealedLog::openFresh(elsewhere, VFS::systemAuth("/sd/notes", "unit"), stored);
    CHECK(!f && fs().text(elsewhere) == theirs);
  }
  // A new file there, or this device's own, is written as anywhere.
  CHECK(fs().remove(elsewhere, {}));
  f = SealedLog::openAppend(elsewhere, VFS::systemAuth("/sd/notes", "unit"), stored);
  CHECK(f && SealedLog::writeLines(f, elsewhere, "ours", 4, stored) && SealedLog::settle(f, elsewhere, stored));
  f.close();
  CHECK(fs().text(elsewhere) == mark() + sealRow("ours", elsewhere));
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

  // No device key: nothing is created, judged, moved or written. A file that
  // is another device's, or plain, is "unreadable" then, never "not ours":
  // prepare (the sensor log's way in) moves nothing aside either.
  reset();
  sealed_log_host::keyAvailable = false;
  fs().put("/sd/system/sys_logs/other.log", "plain\n");
  fs().put("/sd/system/sys_logs/theirs.log", "#HW1ENC v1 k1 id=fedcba9876543210\n");
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(!f && !fs().exists(kLog));
  f = SealedLog::openAppend("/sd/system/sys_logs/other.log", sys(), stored);
  CHECK(!f && fs().text("/sd/system/sys_logs/other.log") == "plain\n" && eventLines.empty());
  for (const char* name : {"/sd/system/sys_logs/other.log", "/sd/system/sys_logs/theirs.log", kLog}) {
    bool ours = true;
    CHECK(SealedLog::inspect(name, sys()) == SealedLog::Head::Unreadable);
    CHECK(!SealedLog::prepare(name, sys(), ours) && !ours);
    CHECK(!fs().exists(std::string(name) + ".foreign-1"));
  }
  const uint32_t movedBefore = SealedLog::movedAsideCount();
  CHECK(eventLines.empty());
  // With the key back the same files are judged, and moved, as ever.
  sealed_log_host::keyAvailable = true;
  {
    bool ours = true;
    CHECK(SealedLog::prepare("/sd/system/sys_logs/theirs.log", sys(), ours) && !ours);
    CHECK(fs().exists("/sd/system/sys_logs/theirs.log.foreign-1") && SealedLog::movedAsideCount() == movedBefore + 1);
  }
  sealed_log_host::keyAvailable = false;
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
  CHECK(SealedLog::writeLines(f, kLog, batch.data(), batch.size(), stored));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow("short") + sealRow("") + sealRow(longLine.substr(0, 1023)) +
                               sealRow(longLine.substr(1023, 1023)) + sealRow(longLine.substr(2046)) + sealRow("tail"));
  CHECK(stored == fs().text(kLog).size());
  // A caller's smaller row limit (the transcript writer).
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(SealedLog::writeLines(f, kLog, longLine.data(), 512, stored, 480));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow(longLine.substr(0, 480)) + sealRow(longLine.substr(480, 32)));
  // Each row is one write: a card that takes only part of a write leaves the
  // rows before it whole and nothing plain.
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  const size_t afterTwo = (mark() + sealRow("row one") + sealRow("row two")).size();
  fs().faults.shortWriteAfter[kLog] = afterTwo + 6;
  CHECK(!SealedLog::writeLines(f, kLog, "row one\nrow two\nrow three\nrow four\n", 35, stored));
  f.close();
  CHECK(fs().text(kLog).size() == afterTwo + 6 && stored == afterTwo + 6);
  CHECK(reveal(fs().text(kLog)).rfind("row one\nrow two\n", 0) == 0 && fs().text(kLog).find("row three") == std::string::npos);
  CHECK(fs().count("write", kLog) == 4);                 // the mark, two rows, the torn third; the fourth is not tried
  // The seal fails: nothing of that line is written, plain or sealed.
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  sealed_log_host::sealFailAt = sealed_log_host::sealCalls + 2;
  CHECK(!SealedLog::writeLines(f, kLog, "kept\nlost\n", 10, stored));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow("kept"));
  // No row buffer: a failure, nothing written.
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(SealedLog::writeLines(f, kLog, nullptr, 0, stored));   // nothing to write is not a failure
  f.close();

  // A write is on the card only when the flushed file has the size the rows
  // make. A card pulled under the handle fails neither write() nor flush():
  // every row is "written", the file never grows. settle tells, and
  // linesLanded says how many whole lines the card did take.
  reset();
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(f && SealedLog::settle(f, kLog, stored));                  // the mark is there
  {
    const size_t before = stored;
    const std::string batch = "line one\r\nline two\r\nline three\r\n";
    const size_t oneRow = sealRow("line one").size();
    fs().faults.lostAfter[kLog] = before + oneRow + 9;             // the first row, and a piece of the second
    CHECK(SealedLog::writeLines(f, kLog, batch.data(), batch.size(), stored));   // nothing reported
    CHECK(stored == before + oneRow + sealRow("line two").size() + sealRow("line three").size());
    CHECK(!f.getWriteError());
    CHECK(!SealedLog::settle(f, kLog, stored));                    // the size tells
    const size_t have = f.size();
    CHECK(have == before + oneRow + 9);
    CHECK(sealedlog::linesLanded(batch.data(), batch.size(), sealedlog::kMaxRowBytes, have - before) == 10);  // "line one\r\n"
    f.close();
    fs().faults.clear();
    // The next open ends the torn row first, so the next good row is not
    // glued to it and lost with it.
    f = SealedLog::openAppend(kLog, sys(), stored);
    CHECK(f && stored == have + 1 && fs().text(kLog).back() == '\n');
    CHECK(SealedLog::writeLines(f, kLog, "after the pull", 14, stored) && SealedLog::settle(f, kLog, stored));
    f.close();
    CHECK(sealed_log_host::view(fs().text(kLog), kLog) == mark() + "line one\n[undecryptable row]\nafter the pull\n");
  }
  // The same for a file found torn at a new mount (a power cut mid-row), and
  // a settled file is not touched.
  reset();
  fs().put(kLog, mark() + sealRow("whole") + "ENC1:dG9yb");
  f = SealedLog::openAppend(kLog, sys(), stored);
  CHECK(f && SealedLog::writeLines(f, kLog, "next", 4, stored) && SealedLog::settle(f, kLog, stored));
  f.close();
  CHECK(fs().text(kLog) == mark() + sealRow("whole") + "ENC1:dG9yb\n" + sealRow("next"));
  CHECK(sealed_log_host::view(fs().text(kLog), kLog) == mark() + "whole\n[undecryptable row]\nnext\n");
  {
    const unsigned writes = fs().count("write", kLog);
    f = SealedLog::openAppend(kLog, sys(), stored); f.close();
    CHECK(fs().count("write", kLog) == writes);                    // nothing to mend
  }
  // prepare alone (the sensor log's way in) mends too.
  reset();
  fs().put(kLog, mark() + sealRow("whole") + "ENC1:dG9yb");
  {
    bool ours = false;
    CHECK(SealedLog::prepare(kLog, sys(), ours) && ours && fs().text(kLog) == mark() + sealRow("whole") + "ENC1:dG9yb\n");
    // The tail cannot be mended (the card refuses the write): not ready.
    reset();
    fs().put(kLog, mark() + "ENC1:dG9yb");
    fs().faults.shortWriteAfter[kLog] = 0;
    CHECK(!SealedLog::prepare(kLog, sys(), ours));
    File g = SealedLog::openAppend(kLog, sys(), stored);
    CHECK(!g && fs().text(kLog) == mark() + "ENC1:dG9yb");
    fs().faults.clear();
    // noteTail: a writer of its own rows says how its last write ended.
    reset();
    fs().put(kLog, mark() + sealRow("row"));
    CHECK(SealedLog::prepare(kLog, sys(), ours) && ours);
    unsigned opens = fs().count("open", kLog);
    CHECK(SealedLog::prepare(kLog, sys(), ours) && fs().count("open", kLog) == opens);   // known clean: no read
    SealedLog::noteTail(kLog, false);
    CHECK(SealedLog::prepare(kLog, sys(), ours) && fs().count("open", kLog) == opens + 1);  // looked at again
  }

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
  CHECK(SealedLog::writeLines(whole, kLog, text.data(), text.size(), stored));
  whole.close();
  const std::string expected = fs().text(kLog);
  for (size_t piece : {size_t(1), size_t(2), size_t(7), size_t(64), size_t(1000), size_t(1023), size_t(1024), size_t(4096)}) {
    reset();
    File f = SealedLog::openAppend(kLog, sys(), stored);
    {
      SealedLog::LineStream lines(f, kLog);
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
    SealedLog::LineStream lines(f, kLog);
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
    SealedLog::LineStream lines(f, kLog);
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
    CHECK(SealedLog::readWindow(f, path, offset, out.data(), out.size(), got, next));
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
  fs().put(path, mark() + rowsOf(plain, path));
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
  CHECK(SealedLog::readWindow(f, path, f.size(), out, sizeof(out), got, next) && got == 0 && next == f.size() && out[0] == '\0');
  CHECK(!SealedLog::readWindow(f, path, f.size() + 1, out, sizeof(out), got, next));
  CHECK(!SealedLog::readWindow(f, path, 0, out, 1, got, next) && !SealedLog::readWindow(f, path, 0, nullptr, 64, got, next));
  f.close();
  // A plain schema line directly after the mark of a capture passes through;
  // a row that does not open says so; a torn last row (no line end) too.
  reset();
  const char* const capture = "/sd/logging_captures/sensors/a.csv";
  fs().put(capture, mark() + "timestamp_ms,gps_fix\n" + sealRow("1,1", capture) + "ENC1:AAA\n" + sealRow("2,0", capture) + "ENC1:torn");
  CHECK(readAll(capture, 513) == "timestamp_ms,gps_fix\n1,1\n[undecryptable row]\n2,0\n[undecryptable row]");
  // A plain line where only sealed rows belong was not written by this
  // device's writer and is never shown as if it were: "[unsealed line]". In a
  // capture that is any plain line after the rows began (or one in the header
  // that is no schema line); in a system log or a transcript, which have no
  // schema lines, every plain line.
  reset();
  fs().put(capture, mark() + "timestamp_ms,gps_fix\n" + sealRow("1,1", capture) + "1700000000000,1\n" +
                        "# a late comment\n" + sealRow("2,0", capture) + "forged tail");
  CHECK(readAll(capture, 513) == "timestamp_ms,gps_fix\n1,1\n[unsealed line]\n[unsealed line]\n2,0\n[unsealed line]");
  CHECK(readAll(capture, 64) == readAll(capture, 513));     // window by window the same
  reset();
  fs().put(kLog, mark() + "[1] [AUTH] admin logged in from 10.0.0.9\n" + sealRow("[2] real") + "# System log started\n" +
                     sealRow("[3] real") + "\n" + "[4] forged\n");
  CHECK(readAll(kLog, 513) == "[unsealed line]\n[2] real\n[unsealed line]\n[3] real\n\n[unsealed line]\n");
  CHECK(readAll(kLog, 40) == readAll(kLog, 513));
  {
    // A later window starts past the header: a line that looks like a schema
    // line is not one there, in a capture either.
    const std::string stored = mark() + "timestamp_ms,a\n" + sealRow("1,1", capture) + "timestamp_ms,forged\n" + sealRow("2,2", capture);
    reset();
    fs().put(capture, stored);
    File g = VFS::openGuarded(capture, "r", sys());
    char window[128];
    size_t n = 0, after = 0;
    const size_t second = (mark() + "timestamp_ms,a\n" + sealRow("1,1", capture)).size();
    CHECK(SealedLog::readWindow(g, capture, second, window, sizeof(window), n, after) &&
          std::string(window) == "[unsealed line]\n2,2\n" && after == stored.size());
    g.close();
  }

  // A row opens only at a path of the class it was sealed for. A transcript
  // copied into another account's folder, out of the transcript tree, or into
  // the capture tree stays closed; so does a system log copied anywhere else.
  {
    const char* const mine = "/sd/stt/u7/20261122T101500Z-local-0000000100000002.txt";
    const std::string transcript = mark() + rowsOf("HardwareOne transcript\nTitle: private words\nsecret phrase\n", mine);
    reset();
    fs().put(mine, transcript);
    CHECK(readAll(mine, 513) == "HardwareOne transcript\nTitle: private words\nsecret phrase\n");
    for (const char* moved : {"/sd/stt/u8/20261122T101500Z-local-0000000100000002.txt", "/sd/stt/u8/copy.txt",
                              "/sd/notes/x.txt", "/sd/logging_captures/sensors/x.csv", "/sd/system/sys_logs/errors.log"}) {
      fs().put(moved, transcript);
      const std::string shown = readAll(moved, 513);
      CHECK(shown == "[undecryptable row]\n[undecryptable row]\n[undecryptable row]\n");
      CHECK(shown.find("secret") == std::string::npos && shown.find("private") == std::string::npos);
    }
    // Inside the owner's folder a copy still opens (the same account reads both).
    fs().put("/sd/stt/u7/copy.txt", transcript);
    CHECK(readAll("/sd/stt/u7/copy.txt", 513).find("secret phrase") != std::string::npos);
    // The command audit, copied to where other accounts read.
    const char* const audit = "/sd/system/sys_logs/command-audit.log";
    const std::string auditFile = mark() + rowsOf("[1] admin: user add bob\n[2] admin: wifi psk ...\n", audit);
    fs().put(audit, auditFile);
    CHECK(readAll(audit, 513).find("user add bob") != std::string::npos);
    for (const char* moved : {"/sd/notes/x.txt", "/sd/stt/u8/x.txt", "/sd/logging_captures/sensors/x.csv",
                              "/sd/system/sys_logs/boot-debug-1.log"}) {
      fs().put(moved, auditFile);
      CHECK(readAll(moved, 513) == "[undecryptable row]\n[undecryptable row]\n");
    }
    // A rotated generation, a staged copy and a set-aside file of the same log
    // are the same class: they open.
    fs().put("/sd/system/sys_logs/command-audit.log.1", auditFile);
    CHECK(readAll("/sd/system/sys_logs/command-audit.log.1", 513).find("user add bob") != std::string::npos);
    // One row moved between classes inside otherwise good files fails alone.
    fs().put(mine, mark() + sealRow("mine", mine) + sealRow("theirs", "/sd/stt/u8/a.txt") + sealRow("mine again", mine));
    CHECK(readAll(mine, 513) == "mine\n[undecryptable row]\nmine again\n");
    // What the writer puts at a path is sealed for that path's class.
    reset();
    size_t stored = 0;
    File w = SealedLog::openAppend(mine, VFS::systemAuth("/sd/stt/u7", "unit"), stored);
    CHECK(w && SealedLog::writeLines(w, mine, "spoken", 6, stored) && SealedLog::settle(w, mine, stored));
    w.close();
    CHECK(fs().text(mine) == mark() + sealRow("spoken", mine) && fs().text(mine) != mark() + sealRow("spoken", "/sd/stt/u8/a.txt"));
  }
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
  CHECK(SealedLog::readWindow(f, kLog, 0, narrow, sizeof(narrow), got, next) && got == 100 && narrow[99] == '\n');
  CHECK(SealedLog::readWindow(f, kLog, next, narrow, sizeof(narrow), got, next) && std::string(narrow) == "after\n" && next == f.size());
  f.close();
  // A read the card fails is a failure with no text.
  reset();
  fs().put(kLog, mark() + sealRow("text"));
  fs().faults.seek.add(kLog);
  f = VFS::openGuarded(kLog, "r", sys());
  std::memset(out, 'x', sizeof(out));
  CHECK(!SealedLog::readWindow(f, kLog, mark().size(), out, sizeof(out), got, next) && got == 0 && out[0] == '\0' && next == mark().size());
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
