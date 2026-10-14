#pragma once

// Production wire parser, also compiled by the host suite. No Arduino/IDF.
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "System_LLMReplyLimits.h"

namespace hw1http {
constexpr size_t kUrlMax = 384;
constexpr size_t kModelMax = 127;
constexpr size_t kTokenMax = 1024;
constexpr size_t kCaMax = 4096;
constexpr size_t kPromptMax = 2047;
constexpr size_t kEventMax = 8192;
constexpr size_t kResultCap = kLlmReplyCapacityBytes;
constexpr const char* kTruncated = "\n[Reply truncated on device]";
constexpr const char* kInterrupted = "\n[Response interrupted; check status]";

inline bool headerSafe(const char* value) {
  if (!value || !*value) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
    if (*p < 0x21 || *p > 0x7e) return false;
  return true;
}

// Intentionally accepts only a base URL ending /v1. No userinfo, query,
// fragments, whitespace, redirects, or protocol downgrade are inferred.
inline bool endpoint(const char* base, bool allowPlaintext, char* out, size_t cap) {
  if (!base || !out || !cap || std::strlen(base) > kUrlMax) return false;
  const size_t scheme = !std::strncmp(base, "https://", 8) ? 8 :
                       !std::strncmp(base, "http://", 7) && allowPlaintext ? 7 : 0;
  if (!scheme || !headerSafe(base)) return false;
  const char* path = std::strchr(base + scheme, '/');
  if (!path || path == base + scheme) return false;
  for (const char* p = base + scheme; *p; ++p)
    if (*p == '@' || *p == '?' || *p == '#' || *p == '\\') return false;
  size_t n = std::strlen(base);
  if (n && base[n - 1] == '/') --n;
  if (n < 3 || std::strncmp(base + n - 3, "/v1", 3)) return false;
  // Permit reverse-proxy path prefixes, but disallow dot segments.
  if (std::strstr(path, "/../") || std::strstr(path, "/./")) return false;
  int written = std::snprintf(out, cap, "%.*s/chat/completions", static_cast<int>(n), base);
  return written > 0 && static_cast<size_t>(written) < cap;
}

// Append-only output: readers may already have drained every published byte.
// Reserve terminal-marker room up front, never replace an already-served tail.
struct Text {
  char bytes[kResultCap]{};
  size_t size = 0;
  bool truncated = false;
  uint8_t pending[4]{};
  unsigned count = 0, need = 0;
  static constexpr size_t textLimit = kResultCap - 1 - 96;

  void literal(const char* p, size_t n) {
    if (n > kResultCap - 1 - size) n = kResultCap - 1 - size;
    std::memcpy(bytes + size, p, n); size += n; bytes[size] = 0;
  }
  void codepoint(const char* p, size_t n) {
    if (truncated) return;
    if (size + n > textLimit) {
      truncated = true; literal(kTruncated, std::strlen(kTruncated)); return;
    }
    literal(p, n);
  }
  void replacement() { codepoint("\xef\xbf\xbd", 3); }
  void append(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      uint8_t c = static_cast<uint8_t>(p[i]);
      if (count) {
        if ((c & 0xc0) != 0x80) {
          count = need = 0; replacement(); --i; continue;
        }
        pending[count++] = c;
        if (count != need) continue;
        const bool valid = !(need == 3 && ((pending[0] == 0xe0 && pending[1] < 0xa0) ||
                                           (pending[0] == 0xed && pending[1] >= 0xa0))) &&
                           !(need == 4 && ((pending[0] == 0xf0 && pending[1] < 0x90) ||
                                           (pending[0] == 0xf4 && pending[1] >= 0x90)));
        if (valid) codepoint(reinterpret_cast<const char*>(pending), count);
        else replacement();
        count = need = 0;
      } else if (c < 0x80) {
        char safe = c < 0x20 && c != '\n' && c != '\r' && c != '\t' ? ' ' : static_cast<char>(c);
        codepoint(&safe, 1);
      } else if (c >= 0xc2 && c <= 0xf4) {
        pending[0] = c; count = 1; need = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
      } else replacement();
    }
  }
  void finish(bool interrupted) {
    if (count) { count = need = 0; replacement(); }
    if (interrupted) literal(kInterrupted, std::strlen(kInterrupted));
  }
};

class Stream {
 public:
  Stream() = default;
  explicit Stream(ArduinoJson::Allocator* allocator) : doc_(allocator) {}
  Text text;
  bool done = false;
  const char* error = nullptr;  // static non-secret diagnostic; never server data
  bool feed(const char* p, size_t n) {
    for (size_t i = 0; i < n && !done && !error; ++i) {
      const char c = p[i];
      if (skipLf_) { skipLf_ = false; if (c == '\n') continue; }
      if (c == '\r' || c == '\n') {
        skipLf_ = c == '\r'; line_[lineLen_] = 0; line(); lineLen_ = 0;
      } else {
        if (!c) { error = "Invalid NUL in event stream"; break; }
        if (lineLen_ == kEventMax) { error = "HTTP event line exceeds limit"; break; }
        line_[lineLen_++] = c;
      }
    }
    return !error;
  }
  void eof() { if (!done && !error) error = "HTTP stream ended before [DONE]"; }
 private:
  JsonDocument doc_;
  char line_[kEventMax + 1]{};
  char data_[kEventMax + 1]{};
  size_t lineLen_ = 0, dataLen_ = 0;
  bool skipLf_ = false, hasData_ = false, errorEvent_ = false;
  void line() {
    if (!lineLen_) { dispatch(); return; }
    if (line_[0] == ':') return;
    const char* colon = std::strchr(line_, ':');
    size_t fieldLen = colon ? static_cast<size_t>(colon - line_) : lineLen_;
    const char* value = colon ? colon + 1 : line_ + lineLen_;
    if (*value == ' ') ++value;
    if (fieldLen == 5 && !std::memcmp(line_, "event", 5)) {
      errorEvent_ = !std::strcmp(value, "error"); return;
    }
    if (fieldLen != 4 || std::memcmp(line_, "data", 4)) return;
    const size_t n = std::strlen(value);
    if (dataLen_ + n + (hasData_ ? 1 : 0) > kEventMax) {
      error = "HTTP event exceeds limit"; return;
    }
    if (hasData_) data_[dataLen_++] = '\n';
    std::memcpy(data_ + dataLen_, value, n); dataLen_ += n;
    data_[dataLen_] = 0; hasData_ = true;
  }
  void dispatch() {
    if (errorEvent_) { error = "Agent reported a stream error"; return; }
    if (!hasData_) return;
    if (!std::strcmp(data_, "[DONE]")) { done = true; text.finish(false); return; }
    doc_.clear();
    if (deserializeJson(doc_, static_cast<const char*>(data_), dataLen_, DeserializationOption::NestingLimit(12))) {
      error = "Invalid JSON in HTTP event"; return;
    }
    if (!doc_["error"].isNull()) { error = "Agent reported a stream error"; return; }
    if (!doc_.is<JsonObject>() || !doc_["choices"].is<JsonArray>()) {
      error = "Invalid agent event shape"; return;
    }
    JsonVariantConst content = doc_["choices"][0]["delta"]["content"];
    if (!content.isNull()) {
      if (!content.is<const char*>()) { error = "Unsupported agent text content"; return; }
      JsonString s = content.as<JsonString>(); text.append(s.c_str(), s.size());
    }
    dataLen_ = 0; hasData_ = errorEvent_ = false;
  }
};
} // namespace hw1http
