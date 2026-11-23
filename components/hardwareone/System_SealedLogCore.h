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
