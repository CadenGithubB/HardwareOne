#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace TranscriptionUI {
inline bool parseId(const char* text, uint64_t& out) {
  out = 0;
  if (!text || std::strlen(text) != 16) return false;
  for (size_t i = 0; i < 16; ++i) {
    const char c = text[i];
    unsigned digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return false;
    out = (out << 4) | digit;
  }
  return out != 0;
}
inline bool parseUnsigned(const char* text, uint32_t& out) {
  out = 0;
  if (!text || !*text) return false;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const unsigned digit = *text - '0';
    if (out > (UINT32_MAX - digit) / 10) return false;
    out = out * 10 + digit;
  }
  return true;
}
inline bool continuation(unsigned char c) { return (c & 0xc0) == 0x80; }
// Number of complete UTF-8 bytes at the end of a bounded non-final window.
// Malformed bytes remain visible to the presentation layer; this only avoids
// splitting a valid character between requests.
inline size_t completePrefix(const char* text, size_t length) {
  if (!length) return 0;
  size_t start = length - 1;
  while (start && continuation(static_cast<unsigned char>(text[start]))) --start;
  const auto lead = static_cast<unsigned char>(text[start]);
  const size_t width = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
      (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
  return length - start < width ? start : length;
}
inline bool safeFilename(const char* name) {
  if (!name || !*name || std::strlen(name) >= 64) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
    if (*p < 32 || *p == 127 || *p == '"' || *p == '/' || *p == '\\') return false;
  return true;
}
inline void clear(char* text, size_t capacity) {
  volatile char* out = text;
  while (capacity--) *out++ = 0;
}
// Bounded recent-text view. Callers commit only after accepting a receipt into
// this window; older preview text may scroll out while its saved file remains.
inline void appendRecent(char* out, size_t capacity, const char* text, size_t length) {
  if (!out || capacity < 2 || !text) return;
  const size_t limit = capacity - 1;
  size_t old = strnlen(out, limit);
  if (length >= limit) {
    text += length - limit; length = limit; old = 0;
    while (length && continuation(static_cast<unsigned char>(*text))) { ++text; --length; }
  } else if (old + length > limit) {
    size_t trim = old + length - limit;
    while (trim < old && continuation(static_cast<unsigned char>(out[trim]))) ++trim;
    std::memmove(out, out + trim, old - trim); old -= trim;
  }
  std::memcpy(out + old, text, length); out[old + length] = 0;
}
}
