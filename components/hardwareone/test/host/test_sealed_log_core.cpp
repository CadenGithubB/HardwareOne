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

  std::fprintf(stderr, "checks=%d\n", checks);
  return 0;
}
