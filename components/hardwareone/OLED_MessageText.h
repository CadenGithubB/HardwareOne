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
// Length of the first sentence of a reason, for a toast: up to the first line
// break, and up to and including a '.', '!' or '?' that a space and a capital
// letter follow. "This recording belongs to alice. Only alice or an
// administrator can stop it." keeps "This recording belongs to alice.".
inline size_t firstSentenceLength(const char* text) {
  if (!text) return 0;
  const size_t line = strcspn(text, "\r\n");
  for (size_t i = 0; i + 2 < line; ++i) {
    const char c = text[i];
    if ((c == '.' || c == '!' || c == '?') && text[i + 1] == ' ' &&
        text[i + 2] >= 'A' && text[i + 2] <= 'Z')
      return i + 1;
  }
  return line;
}
// `Cap` is the caller's storage, sized to the rows that caller asked to keep.
// The six-line Lines result stays for callers that still pass a small count.
template <int Cap>
struct LineStore {
  char text[Cap][32]{};
  int count = 0;
};
struct Lines { char text[6][32]{}; int count = 0; };

// Word-wrap on spaces. Stores at most maxLines, and at most Cap. A caller that
// needs the content band passes that band's row count and a Cap large enough
// to hold it. Mark omitted text explicitly.
template <int Cap>
inline LineStore<Cap> wrap(const char* message, int columns, int maxLines) {
  LineStore<Cap> out;
  if (columns < 1 || maxLines < 1 || Cap < 1) return out;
  if (columns > 31) columns = 31;
  if (maxLines > Cap) maxLines = Cap;
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
inline Lines wrap(const char* message, int columns, int maxLines) {
  const auto stored = wrap<6>(message, columns, maxLines);
  Lines out;
  out.count = stored.count;
  for (int i = 0; i < stored.count; ++i) memcpy(out.text[i], stored.text[i], 32);
  return out;
}
} // namespace oled_message
