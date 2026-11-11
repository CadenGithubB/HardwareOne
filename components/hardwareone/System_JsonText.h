#pragma once
// ============================================================================
// JSON string text
// ============================================================================
// The one escaping rule for text that hand-built JSON places inside a string
// value. No Arduino types, so every module and host test can use it; the String
// forms (jsonEscape, jsonAppendEscaped, jsonQuote) are in System_Utils.h.
//
// The rule: `"` and `\` get a backslash; backspace, form feed, newline,
// carriage return and tab get their short form; every other byte below 0x20
// becomes \u00xx in lower-case hex. Bytes from 0x20 up pass through unchanged,
// so UTF-8 text stays as it is. This is what a standard encoder writes, which
// keeps the G2 notification file byte-identical to the reference encoder.

#include <stddef.h>
#include <string.h>

// Writes the string-content form of one byte into `seq` (NUL-terminated) and
// returns its length: 1 for a byte that passes through, 2 for a short escape,
// 6 for \u00xx.
inline size_t jsonEscapeChar(unsigned char c, char seq[7]) {
  char shortForm = 0;
  switch (c) {
    case '"':  shortForm = '"';  break;
    case '\\': shortForm = '\\'; break;
    case '\b': shortForm = 'b';  break;
    case '\f': shortForm = 'f';  break;
    case '\n': shortForm = 'n';  break;
    case '\r': shortForm = 'r';  break;
    case '\t': shortForm = 't';  break;
    default: break;
  }
  if (shortForm) {
    seq[0] = '\\'; seq[1] = shortForm; seq[2] = '\0';
    return 2;
  }
  if (c < 0x20) {
    static const char hex[] = "0123456789abcdef";
    seq[0] = '\\'; seq[1] = 'u'; seq[2] = '0'; seq[3] = '0';
    seq[4] = hex[c >> 4]; seq[5] = hex[c & 0x0f]; seq[6] = '\0';
    return 6;
  }
  seq[0] = (char)c; seq[1] = '\0';
  return 1;
}

// Length of `len` bytes once escaped (no quotes, no NUL).
inline size_t jsonEscapedLength(const char* data, size_t len) {
  size_t result = 0;
  char seq[7];
  for (size_t i = 0; data && i < len; ++i) result += jsonEscapeChar((unsigned char)data[i], seq);
  return result;
}

// Escapes `src` into a fixed buffer and returns the bytes written (without the
// NUL). When the next character's escape does not fit, the copy stops on a
// character boundary: the result is a shortened text, never half an escape.
inline size_t jsonEscapeInto(char* dst, size_t dstCap, const char* src) {
  if (!dst || dstCap == 0) return 0;
  size_t o = 0;
  char seq[7];
  for (size_t i = 0; src && src[i]; ++i) {
    const size_t need = jsonEscapeChar((unsigned char)src[i], seq);
    if (o + need >= dstCap) break;
    memcpy(dst + o, seq, need);
    o += need;
  }
  dst[o] = '\0';
  return o;
}
