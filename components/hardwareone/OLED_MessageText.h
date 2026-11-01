#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace oled_message {
constexpr uint32_t kErrorDurationMs = 8000;
constexpr size_t kMessageBytes = 192;
inline bool prefix(const char* text, const char* token) {
  while (*token) {
    char a = *text++, b = *token++;
    if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
    if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
    if (a != b) return false;
  }
  return true;
}
inline const char* trim(const char* text) {
  if (!text) return "";
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
  return text;
}
inline bool word(const char* text, const char* token) {
  if (!prefix(text, token)) return false;
  const char next = text[strlen(token)];
  return !next || next == ':' || next == ' ' || next == '\t' || next == '\r' || next == '\n';
}
inline bool isError(const char* text) {
  text = trim(text);
  return prefix(text, "[ERROR]") || word(text, "Error") || word(text, "Failed") ||
         word(text, "Failure") || prefix(text, "Usage:") || prefix(text, "Unknown command:");
}
inline const char* reason(const char* text) {
  text = trim(text);
  if (prefix(text, "[ERROR]")) text += 7;
  else if (word(text, "Error")) text += 5;
  while (*text == ':' || *text == ' ' || *text == '\t') ++text;
  return text;
}
struct Lines { char text[6][32]{}; int count = 0; };
// Word-wrap within the available toast rectangle; mark omitted text explicitly.
inline Lines wrap(const char* message, int columns, int maxLines) {
  Lines out;
  if (columns < 1 || maxLines < 1) return out;
  if (columns > 31) columns = 31;
  if (maxLines > 6) maxLines = 6;
  const char* p = trim(message);
  while (*p && out.count < maxLines) {
    size_t length = strcspn(p, "\r\n");
    size_t take = length < size_t(columns) ? length : size_t(columns);
    if (length > size_t(columns)) {
      for (size_t n = take; n > 0; --n)
        if (p[n] == ' ' || p[n] == '\t') { take = n; break; }
    }
    auto* line = out.text[out.count++];
    memcpy(line, p, take); line[take] = '\0';
    p = trim(p + take);
    if (out.count == maxLines && *p) {
      const size_t end = strlen(line);
      if (columns >= 3) {
        const size_t at = end >= 3 ? end - 3 : 0;
        memcpy(line + at, "...", 4);
      }
    }
  }
  return out;
}
} // namespace oled_message
