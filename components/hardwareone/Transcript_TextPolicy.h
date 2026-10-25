#pragma once
#include <stddef.h>
#include <stdint.h>

// Word and title rules shared by every saved speech text: QuickNote files and
// transcripts take their "Title:" header line and word counts from here, so a
// note and a transcript of the same speech get the same title. Dependency-free
// (no Arduino, ESP-IDF or project headers) so the host suites compile this
// exact header; nothing here allocates, blocks or logs.
namespace TranscriptText {
constexpr size_t kTitleMax = 48;

// Whitespace and control bytes separate words. A word is a token holding at
// least one letter, digit or non-ASCII byte, so stray punctuation ("...", "-")
// from the recogniser is not one.
inline bool gap(unsigned char c) { return c <= ' ' || c == 0x7f; }
inline bool wordByte(unsigned char c) {
  return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z');
}
inline uint32_t wordCount(const char* text, size_t length) {
  uint32_t words = 0;
  bool inToken = false, counted = false;
  for (size_t i = 0; text && i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (gap(c)) { inToken = false; continue; }
    if (!inToken) { inToken = true; counted = false; }
    if (!counted && wordByte(c)) { counted = true; ++words; }
  }
  return words;
}

// The title: the text's leading words, whitespace collapsed, at most
// kTitleMax bytes (and cap - 1), cut at a word boundary. A first word longer
// than that is cut at a UTF-8 character boundary instead. Returns the length.
inline size_t title(const char* text, size_t length, char* out, size_t cap) {
  if (!out || !cap) return 0;
  const size_t limit = cap - 1 < kTitleMax ? cap - 1 : kTitleMax;
  size_t len = 0;       // bytes in out
  size_t complete = 0;  // length through the last complete word
  bool pending = false; // whitespace since the last word byte
  for (size_t i = 0; text && i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (gap(c)) { pending = len != 0; continue; }
    if (pending) {        // a new word starts; the previous one is complete
      complete = len;
      if (len + 1 >= limit) break;  // no room for " x"
      out[len++] = ' ';
      pending = false;
    }
    if (len == limit) {   // this word does not fit
      if (complete) len = complete;
      else if ((c & 0xc0) == 0x80) {  // mid-character: drop the partial one
        while (len && (static_cast<unsigned char>(out[len - 1]) & 0xc0) == 0x80) --len;
        if (len) --len;
      }
      break;
    }
    out[len++] = static_cast<char>(c);
  }
  out[len] = '\0';
  return len;
}
}  // namespace TranscriptText
