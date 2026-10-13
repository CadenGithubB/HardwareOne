#pragma once

// Host-only Arduino String surface for the Edge Impulse suites
// (test_edge_impulse_jpeg.py, test_edge_impulse_lifecycle.py). It covers the
// String calls System_EdgeImpulse.cpp makes and the members ArduinoJson's
// Arduino String writer uses (operator=(const char*), concat, c_str, length).
// It does not model the ESP allocator or String's capacity policy.
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  String& operator=(const char* text) {
    text_ = text ? text : "";
    return *this;
  }
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.size(); }
  char operator[](size_t index) const { return index < text_.size() ? text_[index] : '\0'; }
  bool concat(const char* text) {
    text_ += text ? text : "";
    return true;
  }
  String& operator+=(const String& other) { text_ += other.text_; return *this; }
  String& operator+=(const char* other) { text_ += other ? other : ""; return *this; }
  void trim() {
    const auto first = text_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { text_.clear(); return; }
    text_ = text_.substr(first, text_.find_last_not_of(" \t\r\n") - first + 1);
  }
  int toInt() const { return std::atoi(text_.c_str()); }
  float toFloat() const { return static_cast<float>(std::atof(text_.c_str())); }
  int indexOf(char value) const {
    const auto pos = text_.find(value);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  int lastIndexOf(char value) const {
    const auto pos = text_.rfind(value);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  String substring(size_t from) const { return from < text_.size() ? text_.substr(from) : std::string(); }
  String substring(size_t from, size_t to) const {
    if (from >= text_.size() || to <= from) return String();
    return text_.substr(from, to - from);
  }
  bool startsWith(const char* prefix) const { return text_.rfind(prefix, 0) == 0; }
  bool endsWith(const char* suffix) const {
    const std::string end(suffix);
    return text_.size() >= end.size() &&
           text_.compare(text_.size() - end.size(), end.size(), end) == 0;
  }
  void remove(size_t index) { if (index < text_.size()) text_.erase(index); }
  void remove(size_t index, size_t count) { if (index < text_.size()) text_.erase(index, count); }
  bool operator==(const String& other) const { return text_ == other.text_; }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  bool operator!=(const String& other) const { return !(*this == other); }
  const std::string& str() const { return text_; }

 private:
  std::string text_;
};

inline String operator+(const String& left, const String& right) { return left.str() + right.str(); }
inline String operator+(const String& left, const char* right) { return left.str() + (right ? right : ""); }
inline String operator+(const char* left, const String& right) { return std::string(left ? left : "") + right.str(); }
