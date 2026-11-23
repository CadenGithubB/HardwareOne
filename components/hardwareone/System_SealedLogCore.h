// System_SealedLogCore.h - the format rules of a sealed log file on the card
// (B11, 2026-11-22). Pure: no Arduino, no ESP-IDF, no allocation, so the host
// tests run it as it is (test/host/test_sealed_log_core.cpp). The device side
// (the files, the key, the seal itself) is System_SealedLog.cpp over
// System_CaptureCrypto.
//
//   #HW1ENC v1 k1 id=<16 hex>            plain first line: the mark, and which
//                                        device key sealed the file
//   <plain schema line(s)>               sensor CSV / track header only
//   ENC1:<base64(nonce12 | ct | tag16)>  one per data row
//
// The id is captureCryptoKeyId: a keyed one-way hash of the device key, the
// same id `espnowcatchup` shows. It is never the key. Readers detect the
// "#HW1ENC" prefix only, so a mark with or without the id reads the same. A
// writer uses the id to tell this device's file from another device's, and a
// viewer to say whose file it is.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace sealedlog {

constexpr char kMarkPrefix[] = "#HW1ENC";       // CAPCRYPT_MAGIC_PREFIX
constexpr char kMarkStem[] = "#HW1ENC v1 k1";   // CAPCRYPT_MAGIC_LINE
constexpr char kMarkIdField[] = " id=";
constexpr size_t kKeyIdHexLength = 16;          // 8 bytes of key id
// "#HW1ENC v1 k1 id=" + 16 hex, without the line end.
constexpr size_t kMarkLength = sizeof(kMarkStem) - 1 + sizeof(kMarkIdField) - 1 + kKeyIdHexLength;
// What a writer reads from a file's start to judge its mark (the mark, a CR
// and the line end, with room to spare).
constexpr size_t kMarkProbeBytes = 48;
// The longest plaintext row one seal takes (CAPCRYPT_MAX_ROW).
constexpr size_t kMaxRowBytes = 1023;

inline bool lowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// The mark line for the key id `keyIdHex` (16 lower-case hex digits), without
// a line end. Returns its length, 0 when the id is malformed or `out` small.
inline size_t formatMark(const char* keyIdHex, char* out, size_t capacity) {
  if (!keyIdHex || !out || capacity < kMarkLength + 1) return 0;
  for (size_t i = 0; i < kKeyIdHexLength; ++i) {
    if (!lowerHex(keyIdHex[i])) return 0;
  }
  if (keyIdHex[kKeyIdHexLength] != '\0') return 0;
  const int n = snprintf(out, capacity, "%s%s%s", kMarkStem, kMarkIdField, keyIdHex);
  return n == static_cast<int>(kMarkLength) ? kMarkLength : 0;
}

enum class Mark : uint8_t {
  Unmarked,  // does not start with the mark: a plain file
  Own,       // sealed with this device's key
  Foreign,   // sealed with another device's key (its id is given)
  NoId,      // the mark without a key id, or a damaged mark line: whose key is unknown
};

struct MarkInfo {
  Mark kind = Mark::Unmarked;
  char keyId[kKeyIdHexLength + 1] = {};  // the file's key id when it names one
};

// Judges the first bytes of a file (`head`, `length` bytes; the first line or
// more). `ownKeyIdHex` may be nullptr when this device has no key: a named id
// is then Foreign.
inline MarkInfo classifyMark(const char* head, size_t length, const char* ownKeyIdHex) {
  MarkInfo info;
  const size_t prefix = sizeof(kMarkPrefix) - 1;
  if (!head || length < prefix || memcmp(head, kMarkPrefix, prefix) != 0) return info;
  info.kind = Mark::NoId;
  size_t line = 0;
  while (line < length && head[line] != '\n') ++line;
  if (line == length) return info;  // no line end within the probe: not a whole mark
  if (line > 0 && head[line - 1] == '\r') --line;
  if (line != kMarkLength) return info;
  const size_t stem = sizeof(kMarkStem) - 1, field = sizeof(kMarkIdField) - 1;
  if (memcmp(head, kMarkStem, stem) != 0 || memcmp(head + stem, kMarkIdField, field) != 0) return info;
  const char* id = head + stem + field;
  for (size_t i = 0; i < kKeyIdHexLength; ++i) {
    if (!lowerHex(id[i])) return info;
  }
  memcpy(info.keyId, id, kKeyIdHexLength);
  info.keyId[kKeyIdHexLength] = '\0';
  info.kind = (ownKeyIdHex && memcmp(id, ownKeyIdHex, kKeyIdHexLength) == 0 &&
               ownKeyIdHex[kKeyIdHexLength] == '\0')
                  ? Mark::Own : Mark::Foreign;
  return info;
}

// The next line of `data` (`length` bytes left, length > 0): `lineLength` is
// the text without its line end ("\n" or "\r\n"), `consumed` the bytes to step
// over. Text after the last line end is a line too.
inline void nextLine(const char* data, size_t length, size_t& lineLength, size_t& consumed) {
  const char* end = static_cast<const char*>(memchr(data, '\n', length));
  if (!end) {
    lineLength = consumed = length;
    return;
  }
  consumed = static_cast<size_t>(end - data) + 1;
  lineLength = consumed - 1;
  if (lineLength > 0 && data[lineLength - 1] == '\r') --lineLength;
}

// How much of a line (`length` bytes left) goes into the next sealed row: all
// of it up to `maxRowBytes` (at most kMaxRowBytes; a writer whose reader has a
// smaller window asks for less). A longer line is split, never dropped; the
// split steps back out of a UTF-8 sequence so each row is valid text on its
// own.
inline size_t nextRowLength(const char* line, size_t length, size_t maxRowBytes = kMaxRowBytes) {
  if (maxRowBytes == 0 || maxRowBytes > kMaxRowBytes) maxRowBytes = kMaxRowBytes;
  if (length <= maxRowBytes) return length;
  size_t n = maxRowBytes;
  for (int back = 0; back < 3 && n > 1 && (static_cast<unsigned char>(line[n]) & 0xC0) == 0x80; ++back) --n;
  return n;
}

// Stored bytes of one sealed row of `plainLength` plaintext bytes, with its
// '\n': "ENC1:" + base64(12 + n + 16) + '\n'.
constexpr size_t sealedRowBytes(size_t plainLength) {
  return 5 + 4 * ((plainLength + 28 + 2) / 3) + 1;
}

// ---------------------------------------------------------------------------
// The class of a sealed file: what its rows are bound to.
//
// Every row is sealed with additional data that names the class of the file
// it was written into, and a reader opens a row with the class of the path it
// is reading. A row moved to a place of another class does not open, so the
// device cannot be made to reveal one file's rows through another path's
// permissions (a transcript copied into another account's folder, the command
// audit copied to a user-readable place on a pulled card).
//
//   syslog        /system/sys_logs/... and its card twin
//   bootdebug     the boot debug files there (boot-debug-<n>.log)
//   capture       /logging_captures/... and its card twin (sensor, track and
//                 automation captures, the system log, health days)
//   stt|u<id>     one account's transcript folder, /stt/u<id>/... and its twin
//   path|<path>   anything else: the exact path
//
// Within a class the same accounts read every file (admins the first three,
// the owner the fourth), so a move inside a class reveals nothing new. `path`
// is a normalized path (normalizeFsPath); "/sd" in front is not part of the
// class, so a file and its card twin are one class.
// ---------------------------------------------------------------------------
constexpr size_t kClassMax = 272;  // "path|" + the longest path (255, with "/sd" in front), with its NUL

inline bool startsWith(const char* text, const char* prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

enum class Class : uint8_t { None, SysLog, BootDebug, Capture, Transcript, Path };

// Which class `path` is in. For Transcript, `owner` and `ownerLength` (when
// given) receive the account id's digits. None: not an absolute path.
inline Class classKind(const char* path, const char** owner = nullptr, size_t* ownerLength = nullptr) {
  if (!path || path[0] != '/') return Class::None;
  const char* t = (strncmp(path, "/sd/", 4) == 0) ? path + 3 : path;  // the tier is not part of the class
  static const char kSysLogs[] = "/system/sys_logs/";
  static const char kCaptures[] = "/logging_captures/";
  static const char kStt[] = "/stt/u";
  if (startsWith(t, kSysLogs)) {
    const char* name = t + sizeof(kSysLogs) - 1;
    return startsWith(name, "boot-debug-") && !strchr(name, '/') ? Class::BootDebug : Class::SysLog;
  }
  if (startsWith(t, kCaptures)) return Class::Capture;
  if (startsWith(t, kStt)) {
    // "u" and a decimal id without a leading zero, then '/': the one spelling
    // TranscriptPathPolicy takes for an owner folder.
    const char* digits = t + sizeof(kStt) - 1;
    size_t count = 0;
    while (digits[count] >= '0' && digits[count] <= '9') ++count;
    if (count > 0 && count <= 10 && digits[0] != '0' && digits[count] == '/') {
      if (owner) *owner = digits;
      if (ownerLength) *ownerLength = count;
      return Class::Transcript;
    }
  }
  return Class::Path;
}

// Writes the class of `path` into `out` (NUL-terminated). False when `path`
// is not an absolute path or the class does not fit: such a file gets no rows.
inline bool classOf(const char* path, char* out, size_t capacity) {
  if (!out || capacity < 16) return false;
  const char* owner = nullptr;
  size_t ownerLength = 0;
  int n = -1;
  switch (classKind(path, &owner, &ownerLength)) {
    case Class::None:       return false;
    case Class::SysLog:     n = snprintf(out, capacity, "syslog"); break;
    case Class::BootDebug:  n = snprintf(out, capacity, "bootdebug"); break;
    case Class::Capture:    n = snprintf(out, capacity, "capture"); break;
    case Class::Transcript: n = snprintf(out, capacity, "stt|u%.*s", static_cast<int>(ownerLength), owner); break;
    case Class::Path:       n = snprintf(out, capacity, "path|%s", path); break;
  }
  return n > 0 && static_cast<size_t>(n) < capacity;
}

// True for a path in one of the log trees a card log writer owns (the first
// four classes). Only there is a file that is not this device's moved aside;
// anywhere else a writer leaves a file it did not make alone.
inline bool inLogTree(const char* path) {
  const Class kind = classKind(path);
  return kind != Class::None && kind != Class::Path;
}

// True for a path whose sealed files carry plain schema lines after the mark:
// the capture tree (a sensor CSV's column names, the '#' lines of a track or
// a health day) and a capture the user put at a path of its own. The system
// logs, the boot debug files and transcripts have none: every line after
// their mark is a sealed row.
inline bool classHasSchema(const char* path) {
  const Class kind = classKind(path);
  return kind == Class::Capture || kind == Class::Path;
}

// How much of `data` (lines as writeLines takes them, split into rows of at
// most `maxRowBytes`) is on the card when `landedStored` of the stored bytes
// its rows make arrived there: the bytes of the leading lines whose rows are
// all within that. A writer that could not verify a write uses it to write
// only the rest elsewhere.
inline size_t linesLanded(const char* data, size_t length, size_t maxRowBytes, size_t landedStored) {
  size_t at = 0, stored = 0;
  while (at < length) {
    size_t lineLength = 0, lineBytes = 0;
    nextLine(data + at, length - at, lineLength, lineBytes);
    const char* line = data + at;
    size_t left = lineLength;
    do {
      const size_t take = nextRowLength(line, left, maxRowBytes);
      stored += sealedRowBytes(take);
      line += take;
      left -= take;
    } while (left > 0);
    if (stored > landedStored) break;
    at += lineBytes;
  }
  return at;
}

// ---------------------------------------------------------------------------
// What a line of a marked file is, for a reader. Only sealed rows carry data.
// Plain lines are taken as genuine only where a writer puts them: the mark,
// and, in a file of a class that has them (classHasSchema), the schema lines
// directly after it (a CSV column header, '#' comment lines of a track or
// health file), at most kSchemaLinesMax of them. Any other plain line was not
// written by a sealed writer; a reader shows it as "[unsealed line]"
// (kUnsealedLine) and a parser ignores it.
// ---------------------------------------------------------------------------
constexpr char kRowPrefix[] = "ENC1:";          // CAPCRYPT_ROW_PREFIX
constexpr char kUnsealedLine[] = "[unsealed line]";
constexpr unsigned kSchemaLinesMax = 8;

enum class Line : uint8_t {
  Plain,     // the text is not marked: every line is as stored
  Mark,      // "#HW1ENC ..."
  Schema,    // a plain schema line directly after a mark
  Row,       // a sealed row
  Blank,     // an empty line
  Unsealed,  // a plain line where only sealed rows belong
};

struct LineState {
  bool started = false;   // a first line was seen
  bool marked = false;    // the text is a sealed file
  bool inHeader = false;  // between a mark and the first row after it
  bool schema = true;     // the file's class has schema lines (classHasSchema)
  uint8_t schemaLines = 0;
};
// The state for reading the file at `path`: from its start, or (`midFile`)
// from inside a marked file (a later window or page), which is past the
// header, so only rows and marks are genuine there.
inline LineState stateFor(const char* path, bool midFile = false) {
  LineState state;
  state.schema = classHasSchema(path);
  if (midFile) state.started = state.marked = true;
  return state;
}

// `line` is one line without its line end (a trailing CR is ignored).
inline Line classifyLine(LineState& state, const char* line, size_t length) {
  if (length > 0 && line[length - 1] == '\r') --length;
  const size_t markLength = sizeof(kMarkPrefix) - 1, rowLength = sizeof(kRowPrefix) - 1;
  const bool mark = length >= markLength && memcmp(line, kMarkPrefix, markLength) == 0;
  if (!state.started) {
    state.started = true;
    state.marked = mark;
  }
  if (!state.marked) return Line::Plain;
  if (mark) {
    state.inHeader = true;
    state.schemaLines = 0;
    return Line::Mark;
  }
  if (length > rowLength && memcmp(line, kRowPrefix, rowLength) == 0) {
    state.inHeader = false;
    return Line::Row;
  }
  if (length == 0) return Line::Blank;
  if (state.schema && state.inHeader && state.schemaLines < kSchemaLinesMax &&
      (line[0] == '#' || (length >= 12 && memcmp(line, "timestamp_ms", 12) == 0))) {
    ++state.schemaLines;
    return Line::Schema;
  }
  return Line::Unsealed;
}

// "<path>.foreign-<n>": where a card file this device may not append to is
// moved (no mark, or another device's key id). Returns false when it does not
// fit.
inline bool foreignName(const char* path, unsigned n, char* out, size_t capacity) {
  if (!path || !out || capacity == 0) return false;
  const int written = snprintf(out, capacity, "%s.foreign-%u", path, n);
  return written > 0 && static_cast<size_t>(written) < capacity;
}
// How many such names a writer tries before it gives the card up for this file.
constexpr unsigned kForeignNameTries = 20;

}  // namespace sealedlog
