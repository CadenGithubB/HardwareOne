// System_VersionsCore.h — the `versions` report: list, text and JSON forms.
//
// Dependency-free (no Arduino, ESP-IDF or ArduinoJson) so the host suite
// compiles this exact file. System_Versions.cpp collects the values from their
// owners; this file only holds them and formats them.
//
// Keys are the stable `versions json` / `status json` `versions` names: add a
// key, never rename one. Every value is a string, also the numeric ones, so a
// reader never has to guess a type per key.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace hw1versions {

constexpr size_t kMaxItems = 32;
constexpr size_t kValueMax = 64;  // including the terminator; longer values are cut

struct Item {
  const char* key;    // stable JSON key (string literal)
  const char* label;  // human label (string literal); nullptr: JSON only
  char value[kValueMax];
};

struct List {
  Item items[kMaxItems];
  size_t count = 0;
  bool overflowed = false;  // an add() found the list full; that item is missing

  void clear() { count = 0; overflowed = false; }

  void add(const char* key, const char* label, const char* fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 4, 5)))
#endif
  {
    if (count >= kMaxItems) { overflowed = true; return; }
    Item& item = items[count++];
    item.key = key;
    item.label = label;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(item.value, sizeof(item.value), fmt, args);
    va_end(args);
    if (n < 0) item.value[0] = '\0';
  }

  const char* find(const char* key) const {
    for (size_t i = 0; i < count; ++i)
      if (std::strcmp(items[i].key, key) == 0) return items[i].value;
    return nullptr;
  }
};

namespace detail {
struct Writer {
  char* out;
  size_t cap;
  size_t len = 0;
  bool ok = true;
  void put(const char* s, size_t n) {
    if (!ok) return;
    if (len + n >= cap) { ok = false; return; }
    std::memcpy(out + len, s, n);
    len += n;
    out[len] = '\0';
  }
  void put(const char* s) { put(s, std::strlen(s)); }
  void pad(size_t n) { while (n--) put(" ", 1); }
  void jsonString(const char* s) {
    put("\"", 1);
    for (; *s; ++s) {
      const unsigned char c = static_cast<unsigned char>(*s);
      if (c == '"' || c == '\\') {
        const char esc[2] = {'\\', static_cast<char>(c)};
        put(esc, 2);
      } else if (c < 0x20) {
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04x", c);
        put(esc);
      } else {
        put(s, 1);
      }
    }
    put("\"", 1);
  }
};
}  // namespace detail

// One line per labelled item: "<label padded>  <value>". Returns the length
// written, or 0 (out = "") when the text does not fit in cap bytes.
inline size_t formatText(const List& list, char* out, size_t cap) {
  if (!out || !cap) return 0;
  out[0] = '\0';
  size_t width = 0;
  for (size_t i = 0; i < list.count; ++i) {
    if (!list.items[i].label) continue;
    const size_t n = std::strlen(list.items[i].label);
    if (n > width) width = n;
  }
  detail::Writer w{out, cap};
  bool first = true;
  for (size_t i = 0; i < list.count; ++i) {
    const Item& item = list.items[i];
    if (!item.label) continue;
    if (!first) w.put("\n", 1);
    first = false;
    w.put(item.label);
    w.pad(width - std::strlen(item.label) + 2);
    w.put(item.value);
  }
  if (!w.ok) { out[0] = '\0'; return 0; }
  return w.len;
}

// {"schema":1,"<key>":"<value>",...}. Returns the length written, or 0
// (out = "") when the document does not fit in cap bytes.
inline size_t formatJson(const List& list, char* out, size_t cap) {
  if (!out || !cap) return 0;
  out[0] = '\0';
  detail::Writer w{out, cap};
  w.put("{\"schema\":1");
  for (size_t i = 0; i < list.count; ++i) {
    w.put(",", 1);
    w.jsonString(list.items[i].key);
    w.put(":", 1);
    w.jsonString(list.items[i].value);
  }
  w.put("}", 1);
  if (!w.ok) { out[0] = '\0'; return 0; }
  return w.len;
}

}  // namespace hw1versions
