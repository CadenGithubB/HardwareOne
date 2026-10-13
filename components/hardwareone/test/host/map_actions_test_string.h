#pragma once

// Host Arduino String surface for the OLED Map action and waypoint save
// harnesses (test_oled_map_actions.py). Only the members the extracted Map
// code, CommandArgs and AuthContext use; not a model of the ESP allocator.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
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
  char operator[](size_t index) const { return index < text_.size() ? text_[index] : '\0'; }
  char charAt(size_t index) const { return (*this)[index]; }
  size_t length() const { return text_.size(); }
  const char* c_str() const { return text_.c_str(); }
  int indexOf(char value, size_t from = 0) const {
    const auto pos = text_.find(value, from);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  int indexOf(const String& value, size_t from = 0) const {
    const auto pos = text_.find(value.text_, from);
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
  void trim() {
    const auto first = text_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { text_.clear(); return; }
    text_ = text_.substr(first, text_.find_last_not_of(" \t\r\n") - first + 1);
  }
  void toLowerCase() {
    std::transform(text_.begin(), text_.end(), text_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }
  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }
  float toFloat() const { return std::strtof(text_.c_str(), nullptr); }
  bool startsWith(const String& prefix) const { return text_.rfind(prefix.text_, 0) == 0; }
  bool endsWith(const String& suffix) const {
    return text_.size() >= suffix.text_.size() &&
           text_.compare(text_.size() - suffix.text_.size(), suffix.text_.size(), suffix.text_) == 0;
  }
  String operator+(const String& other) const { return text_ + other.text_; }
  String& operator+=(const String& other) { text_ += other.text_; return *this; }
  bool operator==(const String& other) const { return text_ == other.text_; }
  bool operator!=(const String& other) const { return text_ != other.text_; }
  const std::string& str() const { return text_; }

 private:
  std::string text_;
};
