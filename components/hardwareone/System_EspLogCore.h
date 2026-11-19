// ESP-IDF log levels by tag, saved as one settings string (`esplog`).
//
// The string is "tag:L,tag:L" with L one of N E W I D V (ESP_LOG_NONE ..
// ESP_LOG_VERBOSE); "*" is the tag for every tag. Dependency-free so the host
// tests run it as shipped; System_Debug.cpp applies the entries and owns the
// command. Readers skip a malformed entry (the setting can also be written by
// the generic settings paths), editors drop it.
#ifndef SYSTEM_ESP_LOG_CORE_H
#define SYSTEM_ESP_LOG_CORE_H

#include <cstddef>
#include <cstring>

namespace esplog {

constexpr size_t kMaxTagLen = 31;      // ESP-IDF tags are short ("BT_BTM", "rpc_utils")
constexpr size_t kMaxEntries = 16;
constexpr size_t kMaxStoredLen = 240;  // the saved string, without its terminator
constexpr int kLevelCount = 6;
constexpr char kLetters[] = "NEWIDV";
constexpr const char* kNames[kLevelCount] = {"none", "error", "warn", "info", "debug", "verbose"};

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// A level name, any case: none, error, warn (or warning), info, debug,
// verbose. -1 for anything else.
inline int parseLevel(const char* text) {
  if (!text) return -1;
  char word[8];
  size_t n = 0;
  for (; text[n]; ++n) {
    if (n + 1 >= sizeof(word)) return -1;
    word[n] = lower(text[n]);
  }
  word[n] = '\0';
  if (!std::strcmp(word, "warning")) return 2;
  for (int i = 0; i < kLevelCount; ++i)
    if (!std::strcmp(word, kNames[i])) return i;
  return -1;
}

inline int levelFromLetter(char c) {
  for (int i = 0; i < kLevelCount; ++i)
    if (kLetters[i] == c) return i;
  return -1;
}

// Printable ASCII without the separators, quotes, backslash and '*'.
inline bool tagChar(char c) {
  return c > ' ' && c < 0x7f && c != ',' && c != ':' && c != '*' && c != '"' && c != '\\';
}

// "*" alone, or 1..kMaxTagLen tag characters.
inline bool validTag(const char* tag, size_t len) {
  if (!tag || len == 0 || len > kMaxTagLen) return false;
  if (len == 1 && tag[0] == '*') return true;
  for (size_t i = 0; i < len; ++i)
    if (!tagChar(tag[i])) return false;
  return true;
}
inline bool validTag(const char* tag) { return tag && validTag(tag, std::strlen(tag)); }

inline bool isWildcard(const char* tag, size_t len) { return len == 1 && tag[0] == '*'; }

// Calls f(tag, tagLen, level) for each well-formed entry, at most kMaxEntries,
// skipping malformed ones and later repeats of a tag. Returns the count.
template <typename F>
size_t forEach(const char* stored, F&& f) {
  if (!stored) return 0;
  struct Seen { const char* tag; size_t len; } seen[kMaxEntries];
  size_t count = 0;
  const char* p = stored;
  while (*p && count < kMaxEntries) {
    const char* end = std::strchr(p, ',');
    const size_t len = end ? size_t(end - p) : std::strlen(p);
    // tag ':' letter
    if (len >= 3 && p[len - 2] == ':') {
      const size_t tagLen = len - 2;
      const int level = levelFromLetter(p[len - 1]);
      bool repeat = false;
      for (size_t i = 0; i < count; ++i)
        if (seen[i].len == tagLen && !std::memcmp(seen[i].tag, p, tagLen)) repeat = true;
      if (level >= 0 && validTag(p, tagLen) && !repeat) {
        seen[count++] = {p, tagLen};
        f(p, tagLen, level);
      }
    }
    if (!end) break;
    p = end + 1;
  }
  return count;
}

// The saved level of `tag`, or -1.
inline int levelOf(const char* stored, const char* tag) {
  int found = -1;
  const size_t want = tag ? std::strlen(tag) : 0;
  forEach(stored, [&](const char* t, size_t len, int level) {
    if (len == want && !std::memcmp(t, tag, len)) found = level;
  });
  return found;
}

enum class Edit { Ok, Unchanged, BadTag, BadLevel, TooMany, TooLong, NotFound, Duplicate };

namespace detail {
// Rebuilds the string into out: the wildcard first, then the other entries in
// their order, without `skip`; `add` (when given) replaces its tag's entry in
// place or is appended (a wildcard goes first). Fails with TooMany/TooLong; out
// is untouched on failure.
inline Edit rebuild(const char* stored, const char* skip, const char* add, int addLevel, char* out,
                    size_t cap) {
  char buf[kMaxStoredLen + 1];
  size_t used = 0, entries = 0;
  bool overflow = false;
  const size_t skipLen = skip ? std::strlen(skip) : 0;
  auto put = [&](const char* tag, size_t len, int level) {
    if (used + (used ? 1 : 0) + len + 2 > kMaxStoredLen) { overflow = true; return; }
    if (used) buf[used++] = ',';
    std::memcpy(buf + used, tag, len);
    used += len;
    buf[used++] = ':';
    buf[used++] = kLetters[level];
    ++entries;
  };
  const bool skipWildcard = skip && isWildcard(skip, skipLen);
  const bool addWildcard = add && isWildcard(add, std::strlen(add));
  // The wildcard first (the new one, or the kept one), then the tags in their
  // order, then a new tag.
  if (addWildcard) {
    put(add, 1, addLevel);
  } else if (!skipWildcard) {
    const int w = levelOf(stored, "*");
    if (w >= 0) put("*", 1, w);
  }
  const size_t addLen = add ? std::strlen(add) : 0;
  bool placed = addWildcard;
  forEach(stored, [&](const char* tag, size_t len, int level) {
    if (isWildcard(tag, len)) return;
    if (len == skipLen && !std::memcmp(tag, skip, len)) return;
    if (add && len == addLen && !std::memcmp(tag, add, len)) {  // replaced in its place
      put(tag, len, addLevel);
      placed = true;
      return;
    }
    put(tag, len, level);
  });
  if (add && !placed) put(add, addLen, addLevel);
  if (entries > kMaxEntries) return Edit::TooMany;
  if (overflow) return Edit::TooLong;
  if (!out || cap <= used) return Edit::TooLong;
  std::memcpy(out, buf, used);
  out[used] = '\0';
  return Edit::Ok;
}
}  // namespace detail

// Sets tag to level (adds or replaces), rebuilding the canonical string into
// out. Unchanged when the tag already has that level (out still filled).
inline Edit set(const char* stored, const char* tag, int level, char* out, size_t cap) {
  if (!validTag(tag)) return Edit::BadTag;
  if (level < 0 || level >= kLevelCount) return Edit::BadLevel;
  const int before = levelOf(stored, tag);
  if (before < 0) {
    size_t n = forEach(stored, [](const char*, size_t, int) {});
    if (n >= kMaxEntries) return Edit::TooMany;
  }
  const Edit e = detail::rebuild(stored, nullptr, tag, level, out, cap);
  if (e != Edit::Ok) return e;
  return before == level ? Edit::Unchanged : Edit::Ok;
}

// Removes tag's entry, rebuilding the canonical string into out.
inline Edit remove(const char* stored, const char* tag, char* out, size_t cap) {
  if (!validTag(tag)) return Edit::BadTag;
  if (levelOf(stored, tag) < 0) return Edit::NotFound;
  return detail::rebuild(stored, tag, nullptr, 0, out, cap);
}

// Replaces every entry with `text` ("tag:L,tag:L", L a letter or a level
// name), as the settings editors send it. Strict: any malformed entry or a
// repeated tag refuses the whole text. An empty text clears.
inline Edit replaceAll(const char* text, char* out, size_t cap) {
  if (!out || cap == 0) return Edit::TooLong;
  char canon[kMaxStoredLen + 1];
  size_t used = 0, entries = 0;
  const char* p = text ? text : "";
  while (*p == ' ') ++p;
  while (*p) {
    const char* end = std::strchr(p, ',');
    size_t len = end ? size_t(end - p) : std::strlen(p);
    const char* q = p;
    while (len && *q == ' ') { ++q; --len; }
    while (len && q[len - 1] == ' ') --len;
    const char* colon = static_cast<const char*>(std::memchr(q, ':', len));
    if (!colon) return Edit::BadTag;
    const size_t tagLen = size_t(colon - q);
    if (!validTag(q, tagLen)) return Edit::BadTag;
    char levelText[8];
    const size_t levelLen = len - tagLen - 1;
    if (levelLen == 0 || levelLen >= sizeof(levelText)) return Edit::BadLevel;
    std::memcpy(levelText, colon + 1, levelLen);
    levelText[levelLen] = '\0';
    int level = levelLen == 1 ? levelFromLetter(levelText[0] >= 'a' && levelText[0] <= 'z'
                                                    ? char(levelText[0] - 'a' + 'A') : levelText[0])
                              : parseLevel(levelText);
    if (level < 0) return Edit::BadLevel;
    if (++entries > kMaxEntries) return Edit::TooMany;
    if (used + (used ? 1 : 0) + tagLen + 2 > kMaxStoredLen) return Edit::TooLong;
    // A repeated tag is refused rather than silently merged.
    char tag[kMaxTagLen + 1];
    std::memcpy(tag, q, tagLen);
    tag[tagLen] = '\0';
    canon[used] = '\0';
    if (levelOf(canon, tag) >= 0) return Edit::Duplicate;
    if (used) canon[used++] = ',';
    std::memcpy(canon + used, q, tagLen);
    used += tagLen;
    canon[used++] = ':';
    canon[used++] = kLetters[level];
    canon[used] = '\0';
    if (!end) break;
    p = end + 1;
  }
  canon[used] = '\0';
  // Canonical order: the wildcard first.
  return detail::rebuild(canon, nullptr, nullptr, 0, out, cap);
}

}  // namespace esplog

#endif  // SYSTEM_ESP_LOG_CORE_H
