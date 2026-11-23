// B11: the format rules of a sealed log file on the card
// (System_SealedLogCore.h) and the card's caps and reserve
// (System_LogCapsCore.h), both compiled as they are.
#include "System_LogCapsCore.h"
#include "System_SealedLogCore.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } \
  ++checks; } while (0)

using sealedlog::Mark;

static Mark kindOf(const std::string& head, const char* own, std::string* id = nullptr) {
  const sealedlog::MarkInfo info = sealedlog::classifyMark(head.data(), head.size(), own);
  if (id) *id = info.keyId;
  return info.kind;
}

// Splits `text` the way the writer does: one row per line, a long line into
// several rows.
static std::vector<std::string> rowsOf(const std::string& text, size_t maxRow = sealedlog::kMaxRowBytes) {
  std::vector<std::string> rows;
  size_t at = 0;
  while (at < text.size()) {
    size_t lineLength = 0, consumed = 0;
    sealedlog::nextLine(text.data() + at, text.size() - at, lineLength, consumed);
    const char* line = text.data() + at;
    size_t left = lineLength;
    do {
      const size_t take = sealedlog::nextRowLength(line, left, maxRow);
      rows.emplace_back(line, take);
      line += take;
      left -= take;
    } while (left > 0);
    at += consumed;
  }
  return rows;
}

int main() {
  const char own[] = "0123456789abcdef";
  const char other[] = "fedcba9876543210";

  // The mark line: the stem every reader detects, then the key id.
  char mark[64];
  CHECK(sealedlog::formatMark(own, mark, sizeof(mark)) == sealedlog::kMarkLength);
  CHECK(std::string(mark) == "#HW1ENC v1 k1 id=0123456789abcdef");
  CHECK(sealedlog::kMarkLength == 33 && sealedlog::kMarkLength < sealedlog::kMarkProbeBytes);
  CHECK(std::string(mark).rfind(sealedlog::kMarkPrefix, 0) == 0);   // readers key off the prefix only
  CHECK(std::string(mark).rfind(sealedlog::kMarkStem, 0) == 0);
  // A malformed id or a small buffer writes no mark.
  CHECK(sealedlog::formatMark("0123456789ABCDEF", mark, sizeof(mark)) == 0);
  CHECK(sealedlog::formatMark("0123456789abcde", mark, sizeof(mark)) == 0);
  CHECK(sealedlog::formatMark("0123456789abcdef0", mark, sizeof(mark)) == 0);
  CHECK(sealedlog::formatMark(nullptr, mark, sizeof(mark)) == 0);
  CHECK(sealedlog::formatMark(own, mark, sealedlog::kMarkLength) == 0);

  // Whose file: this device's, another device's, no id, no mark.
  std::string id;
  const std::string ours = std::string("#HW1ENC v1 k1 id=") + own + "\nENC1:abc\n";
  CHECK(kindOf(ours, own, &id) == Mark::Own && id == own);
  CHECK(kindOf(std::string("#HW1ENC v1 k1 id=") + own + "\r\nENC1:abc\n", own) == Mark::Own);
  CHECK(kindOf(std::string("#HW1ENC v1 k1 id=") + other + "\nENC1:abc\n", own, &id) == Mark::Foreign && id == other);
  CHECK(kindOf(ours, nullptr, &id) == Mark::Foreign && id == own);   // no key here: every named id is another's
  CHECK(kindOf("#HW1ENC v1 k1\ntimestamp_ms\n", own, &id) == Mark::NoId && id.empty());   // the mark before B11
  CHECK(kindOf("#HW1ENC v2 k9 id=0123456789abcdef\n", own) == Mark::NoId);
  CHECK(kindOf(std::string("#HW1ENC v1 k1 id=") + own, own) == Mark::NoId);            // torn: no line end
  CHECK(kindOf("#HW1ENC v1 k1 id=0123456789abcde\n", own) == Mark::NoId);              // a short id
  CHECK(kindOf("#HW1ENC v1 k1 id=0123456789ABCDEF\n", own) == Mark::NoId);             // not lower-case hex
  CHECK(kindOf("#HW1ENC v1 k1 id=0123456789abcdef \n", own) == Mark::NoId);            // a trailing byte
  CHECK(kindOf("#HW1ENC", own) == Mark::NoId);
  CHECK(kindOf("[2026-11-22 10:00:00] plain log line\n", own) == Mark::Unmarked);
  CHECK(kindOf("timestamp_ms,gps_fix\n", own) == Mark::Unmarked);
  CHECK(kindOf("", own) == Mark::Unmarked && kindOf("#HW1EN", own) == Mark::Unmarked);
  CHECK(sealedlog::classifyMark(nullptr, 0, own).kind == Mark::Unmarked);

  // Lines of a batch: "\n" and "\r\n" end a line, the rest is a line too.
  {
    const std::vector<std::string> rows = rowsOf("first\r\nsecond\n\nfourth");
    CHECK(rows.size() == 4 && rows[0] == "first" && rows[1] == "second" && rows[2].empty() && rows[3] == "fourth");
    // A capped-log batch (CRLF after each line): one row per line.
    const std::vector<std::string> batch = rowsOf("[t] a\r\n[t] bb\r\n[t] ccc\r\n");
    CHECK(batch.size() == 3 && batch[0] == "[t] a" && batch[2] == "[t] ccc");
    CHECK(rowsOf("").empty());
    CHECK(rowsOf("\n").size() == 1 && rowsOf("\n")[0].empty());
    CHECK(rowsOf("\r\n").size() == 1 && rowsOf("\r\n")[0].empty());
  }

  // A line over 1023 bytes is split into rows of 1023 or less, never dropped.
  {
    const std::string longLine(2500, 'x');
    const std::vector<std::string> rows = rowsOf(longLine + "\nnext\n");
    CHECK(rows.size() == 4 && rows[0].size() == 1023 && rows[1].size() == 1023 && rows[2].size() == 454 &&
          rows[3] == "next");
    CHECK(rows[0] + rows[1] + rows[2] == longLine);
    CHECK(rowsOf(std::string(1023, 'y')).size() == 1 && rowsOf(std::string(1024, 'y')).size() == 2);
    // The split steps back out of a UTF-8 sequence: 1022 ASCII bytes, then
    // three-byte characters.
    std::string utf(1022, 'a');
    for (int i = 0; i < 4; ++i) utf += "\xE2\x82\xAC";
    const std::vector<std::string> split = rowsOf(utf);
    CHECK(split.size() == 2 && split[0].size() == 1022 && split[1].size() == 12 && split[0] + split[1] == utf);
    // A writer whose reader has a smaller window asks for shorter rows (the
    // transcript writer: 480).
    const std::vector<std::string> narrow = rowsOf(std::string(512, 'z'), 480);
    CHECK(narrow.size() == 2 && narrow[0].size() == 480 && narrow[1].size() == 32);
    CHECK(sealedlog::nextRowLength("abc", 3, 0) == 3 && sealedlog::nextRowLength("abc", 3, 5000) == 3);
  }

  // Stored size of a row: "ENC1:" + base64(12 + n + 16) + '\n'; the design's
  // 5 + 4 * ceil((n + 28) / 3) without the line end.
  CHECK(sealedlog::sealedRowBytes(0) == 5 + 40 + 1);
  CHECK(sealedlog::sealedRowBytes(60) == 5 + 4 * 30 + 1);     // 126: 2.0x of a 60-byte line and its CRLF
  CHECK(sealedlog::sealedRowBytes(120) == 5 + 4 * 50 + 1);    // 206: 1.7x
  CHECK(sealedlog::sealedRowBytes(300) == 5 + 4 * 110 + 1);   // 446: 1.5x
  CHECK(sealedlog::sealedRowBytes(1023) == 5 + 4 * 351 + 1 && sealedlog::sealedRowBytes(1023) < 1536);

  // Where a file that is not this device's is moved.
  {
    char name[64];
    CHECK(sealedlog::foreignName("/sd/system/sys_logs/errors.log", 1, name, sizeof(name)) &&
          std::string(name) == "/sd/system/sys_logs/errors.log.foreign-1");
    CHECK(sealedlog::foreignName("/sd/a.csv", 20, name, sizeof(name)) && std::string(name) == "/sd/a.csv.foreign-20");
    CHECK(!sealedlog::foreignName("/sd/a.csv", 1, name, 12));
    CHECK(!sealedlog::foreignName(nullptr, 1, name, sizeof(name)));
    CHECK(sealedlog::kForeignNameTries == 20);
  }

  // The card's caps: twice the nominal cap in stored bytes, about 5.6 MB for
  // the eight logs, whatever the LittleFS size.
  CHECK(logcaps::cardCap(logcaps::kEventsCap) == 512u * 1024u);
  CHECK(logcaps::cardCap(logcaps::kLoginCap) == 2u * 696969u);
  CHECK(logcaps::cardCap(logcaps::kNominalTotal) == 5646884u);
  CHECK(logcaps::generationBytes(logcaps::cardCap(logcaps::kAuditCap)) == 500u * 1024u);
  // A 256 KB log of 120-byte lines holds 2150 lines plain; its card cap holds
  // about the same number sealed.
  {
    const size_t plainLines = logcaps::kEventsCap / (120 + 2);
    const size_t sealedLines = logcaps::cardCap(logcaps::kEventsCap) / sealedlog::sealedRowBytes(120);
    CHECK(sealedLines > plainLines && sealedLines < plainLines * 13 / 10);
  }

  // The card reserve, with its hysteresis: full below 8 MiB, usable again at 16.
  {
    const uint64_t mib = 1024ull * 1024ull;
    CHECK(logcaps::kCardReserveBytes == 8 * mib && logcaps::kCardRearmBytes == 16 * mib);
    CHECK(!logcaps::cardFull(false, 8 * mib) && logcaps::cardFull(false, 8 * mib - 1));
    CHECK(logcaps::cardFull(true, 8 * mib) && logcaps::cardFull(true, 16 * mib - 1));
    CHECK(!logcaps::cardFull(true, 16 * mib) && !logcaps::cardFull(false, 64ull * 1024 * mib));
    CHECK(logcaps::cardFull(false, 0) && logcaps::cardFull(true, 0));
  }

  // The class a file's rows are bound to: a file and its card twin are one
  // class; the system logs, the boot debug files, the capture tree and each
  // account's transcript folder are classes of their own; anything else is its
  // exact path.
  {
    auto cls = [](const char* path) {
      char out[sealedlog::kClassMax];
      return std::string(sealedlog::classOf(path, out, sizeof(out)) ? out : "<none>");
    };
    CHECK(cls("/sd/system/sys_logs/command-audit.log") == "syslog");
    CHECK(cls("/system/sys_logs/command-audit.log") == "syslog");
    CHECK(cls("/sd/system/sys_logs/errors.log.1") == "syslog");
    CHECK(cls("/sd/system/sys_logs/errors.log.tmp") == "syslog");
    CHECK(cls("/sd/system/sys_logs/failed_login.log.foreign-3") == "syslog");
    CHECK(cls("/sd/system/sys_logs/boot-debug-1.log") == "bootdebug");
    CHECK(cls("/system/sys_logs/boot-debug-3.log") == "bootdebug");
    CHECK(cls("/sd/system/sys_logs/sub/boot-debug-1.log") == "syslog");
    CHECK(cls("/sd/logging_captures/sensors/2026-11-22/health-2026-11-22.csv") == "capture");
    CHECK(cls("/logging_captures/system/log.txt.2") == "capture");
    CHECK(cls("/sd/logging_captures/tracks/a.csv") == "capture");
    CHECK(cls("/logging_captures/r1-health-history") == "capture");  // the folder the health days are sealed for
    CHECK(cls("/logging_captures/r1-health-history/x.day") == "capture");
    CHECK(cls("/sd/stt/u1/20261122T101500Z-local-0000000100000002.txt") == "stt|u1");
    CHECK(cls("/stt/u1/a.txt") == "stt|u1");
    CHECK(cls("/sd/stt/u27/notes/a.txt") == "stt|u27");
    CHECK(cls("/sd/stt/u2/a.txt") != cls("/sd/stt/u1/a.txt"));
    CHECK(cls("/sd/stt/u4294967295/a.txt") == "stt|u4294967295");
    // Not an owner folder in the one spelling the transcript policy takes: its
    // exact path, so nothing sealed for an account opens there.
    CHECK(cls("/sd/stt/u01/a.txt") == "path|/sd/stt/u01/a.txt");
    CHECK(cls("/sd/stt/u/a.txt") == "path|/sd/stt/u/a.txt");
    CHECK(cls("/sd/stt/u1") == "path|/sd/stt/u1");
    CHECK(cls("/sd/stt/u1x/a.txt") == "path|/sd/stt/u1x/a.txt");
    CHECK(cls("/sd/stt/u12345678901/a.txt") == "path|/sd/stt/u12345678901/a.txt");
    CHECK(cls("/sd/x.txt") == "path|/sd/x.txt");
    CHECK(cls("/x.txt") == "path|/x.txt");
    CHECK(cls("/sd/x.txt") != cls("/sd/y.txt"));
    CHECK(cls("/sd/system/x.log") == "path|/sd/system/x.log");
    CHECK(cls("/sd/logging_captures") == "path|/sd/logging_captures");
    CHECK(cls("relative") == "<none>" && cls("") == "<none>");
    CHECK(!sealedlog::classOf(nullptr, nullptr, 0));
    {
      CHECK(cls(("/" + std::string(254, 'a')).c_str()).size() == 5 + 255);   // the longest path has a class
      const std::string longPath = "/" + std::string(300, 'a');
      CHECK(cls(longPath.c_str()) == "<none>");  // does not fit: such a file gets no rows
      char small[8];
      CHECK(!sealedlog::classOf("/sd/system/sys_logs/a.log", small, sizeof(small)));
    }
    // Only the log trees are a writer's to tidy.
    CHECK(sealedlog::inLogTree("/sd/system/sys_logs/errors.log"));
    CHECK(sealedlog::inLogTree("/sd/system/sys_logs/boot-debug-1.log"));
    CHECK(sealedlog::inLogTree("/sd/logging_captures/automation.log"));
    CHECK(sealedlog::inLogTree("/sd/stt/u1/a.txt"));
    CHECK(!sealedlog::inLogTree("/sd/notes.txt") && !sealedlog::inLogTree("/sd/Music/a.log"));
    CHECK(!sealedlog::inLogTree("/sd/system/users.json") && !sealedlog::inLogTree("/sd/stt/shared.txt"));
    // Which classes carry plain schema lines after the mark.
    CHECK(sealedlog::classHasSchema("/sd/logging_captures/sensors/a.csv"));
    CHECK(sealedlog::classHasSchema("/sd/my-capture.csv"));
    CHECK(!sealedlog::classHasSchema("/sd/system/sys_logs/errors.log"));
    CHECK(!sealedlog::classHasSchema("/sd/system/sys_logs/boot-debug-1.log"));
    CHECK(!sealedlog::classHasSchema("/sd/stt/u1/a.txt"));
    CHECK(!sealedlog::classHasSchema(nullptr));
  }

  // What a line of a file is, for a reader.
  {
    using sealedlog::Line;
    auto kinds = [](const char* path, const std::vector<std::string>& lines, bool midFile = false) {
      sealedlog::LineState state = sealedlog::stateFor(path, midFile);
      std::string out;
      for (const std::string& line : lines) {
        switch (sealedlog::classifyLine(state, line.data(), line.size())) {
          case Line::Plain:    out += 'p'; break;
          case Line::Mark:     out += 'M'; break;
          case Line::Schema:   out += 's'; break;
          case Line::Row:      out += 'R'; break;
          case Line::Blank:    out += '_'; break;
          case Line::Unsealed: out += 'U'; break;
        }
      }
      return out;
    };
    const char* capture = "/sd/logging_captures/sensors/a.csv";
    const char* syslog = "/sd/system/sys_logs/errors.log";
    const std::string mark = "#HW1ENC v1 k1 id=0123456789abcdef";
    // A file without the mark is plain, whatever its lines look like.
    CHECK(kinds(capture, {"timestamp_ms,a", "1,2", "ENC1:abcd", "#HW1ENC v1 k1"}) == "pppp");
    CHECK(kinds(syslog, {"[1] hello", ""}) == "pp");
    // A sensor capture: the mark, the column names, rows.
    CHECK(kinds(capture, {mark, "timestamp_ms,thermal_min", "ENC1:abcd", "ENC1:efgh"}) == "MsRR");
    // A track: the mark, '#' lines, rows; CR line ends read the same.
    CHECK(kinds(capture, {mark + "\r", "# GPS Track Log\r", "# timestamp_ms,lat,lon\r", "ENC1:abcd\r"}) == "MssR");
    // A plain line once the rows have begun was not written by a sealed
    // writer, and neither was one in the header that is no schema line.
    CHECK(kinds(capture, {mark, "timestamp_ms,a", "ENC1:abcd", "1700000000000,forged", "ENC1:efgh"}) == "MsRUR");
    CHECK(kinds(capture, {mark, "ENC1:abcd", "# late comment", "timestamp_ms,a"}) == "MRUU");
    CHECK(kinds(capture, {mark, "1700000000000,forged", "ENC1:abcd"}) == "MUR");
    CHECK(kinds(capture, {mark, "", "ENC1:abcd", ""}) == "M_R_");
    // The system logs, the boot debug files and transcripts have no schema
    // lines: every plain line after the mark is unsealed, '#' or not.
    CHECK(kinds(syslog, {mark, "# System log started", "[1] forged admin login", "ENC1:abcd"}) == "MUUR");
    CHECK(kinds("/sd/stt/u1/a.txt", {mark, "HardwareOne transcript", "ENC1:abcd", "Title: forged"}) == "MURU");
    CHECK(kinds("/sd/system/sys_logs/boot-debug-1.log", {mark, "# boot 7", "ENC1:abcd"}) == "MUR");
    // At most kSchemaLinesMax schema lines after a mark.
    {
      std::vector<std::string> lines = {mark};
      for (unsigned i = 0; i < sealedlog::kSchemaLinesMax + 2; ++i) lines.push_back("# comment");
      CHECK(kinds(capture, lines) == "M" + std::string(sealedlog::kSchemaLinesMax, 's') + "UU");
    }
    // A second mark (a stitched capture) opens a header again.
    CHECK(kinds(capture, {mark, "timestamp_ms,a", "ENC1:abcd", mark, "timestamp_ms,a", "ENC1:efgh", "x"}) == "MsRMsRU");
    // "ENC1:" alone is no row.
    CHECK(kinds(capture, {mark, "ENC1:", "ENC1:a"}) == "MUR");
    // A later window or page of a sealed file: past the header.
    CHECK(kinds(capture, {"ENC1:abcd", "timestamp_ms,forged", "# forged", "ENC1:efgh"}, true) == "RUUR");
    CHECK(kinds(syslog, {"[1] forged", "ENC1:abcd"}, true) == "UR");
    CHECK(std::string(sealedlog::kUnsealedLine) == "[unsealed line]");
  }

  // How much of a batch the card holds when only part of its stored bytes
  // arrived: whole leading lines, never a part of one.
  {
    const std::string batch = "first line\r\nsecond\r\n\r\nfourth and last\r\n";
    const size_t r1 = sealedlog::sealedRowBytes(10), r2 = sealedlog::sealedRowBytes(6),
                 r3 = sealedlog::sealedRowBytes(0), r4 = sealedlog::sealedRowBytes(15);
    auto landed = [&](size_t stored) {
      return sealedlog::linesLanded(batch.data(), batch.size(), sealedlog::kMaxRowBytes, stored);
    };
    CHECK(landed(0) == 0 && landed(r1 - 1) == 0);
    CHECK(landed(r1) == 12 && landed(r1 + r2 - 1) == 12);
    CHECK(landed(r1 + r2) == 20 && landed(r1 + r2 + r3) == 22);
    CHECK(landed(r1 + r2 + r3 + r4 - 1) == 22);
    CHECK(landed(r1 + r2 + r3 + r4) == batch.size());
    CHECK(landed(r1 + r2 + r3 + r4 + 100) == batch.size());
    // A line split into several rows counts once all its rows are there.
    const std::string longLine(2500, 'x');
    const size_t rows = sealedlog::sealedRowBytes(1023) * 2 + sealedlog::sealedRowBytes(2500 - 2046);
    CHECK(sealedlog::linesLanded(longLine.data(), longLine.size(), sealedlog::kMaxRowBytes, rows - 1) == 0);
    CHECK(sealedlog::linesLanded(longLine.data(), longLine.size(), sealedlog::kMaxRowBytes, rows) == 2500);
    CHECK(sealedlog::linesLanded("", 0, sealedlog::kMaxRowBytes, 0) == 0);
  }

  // One log's card file that cannot be written: 1, 4, 16, then 60 minutes
  // while the retries keep failing; a failure after a quiet spell starts over.
  {
    CHECK(logcaps::cardFileBackoffMs(1) == 60000u && logcaps::cardFileBackoffMs(2) == 240000u);
    CHECK(logcaps::cardFileBackoffMs(3) == 960000u && logcaps::cardFileBackoffMs(4) == 3600000u);
    CHECK(logcaps::cardFileBackoffMs(0) == 60000u && logcaps::cardFileBackoffMs(9) == 3600000u);
    uint8_t strikes = 0;
    uint32_t waited = 0;
    const uint32_t expected[] = {60000u, 240000u, 960000u, 3600000u, 3600000u, 3600000u};
    for (uint32_t want : expected) {
      strikes = logcaps::cardFileStrikes(strikes, waited);  // the retry right after the back-off failed
      CHECK(logcaps::cardFileBackoffMs(strikes) == want);
      waited = want + 1000;
    }
    CHECK(strikes == logcaps::kCardFileStrikesMax);
    CHECK(logcaps::cardFileStrikes(3, 2 * 960000u) == 4);       // still the same trouble
    CHECK(logcaps::cardFileStrikes(3, 2 * 960000u + 1) == 1);   // it had worked for a while: start over
    CHECK(logcaps::cardFileStrikes(4, 3 * 3600000u) == 1);
  }

  std::fprintf(stderr, "checks=%d\n", checks);
  return 0;
}
