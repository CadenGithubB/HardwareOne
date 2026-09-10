#pragma once

// Host-only Arduino String surface needed by the extracted web handlers and
// ArduinoJson's real Arduino String adapter. This is not the ESP allocator or
// a model of its SSO/capacity policy; allocation placement is tested elsewhere.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

void webBatchTestStringAppend();

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
  size_t length() const { return text_.length(); }
  bool concat(const char* text) {
    webBatchTestStringAppend();
    text_ += text ? text : "";
    return true;
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
  bool equalsIgnoreCase(const char* other) const {
    String left(*this), right(other);
    left.toLowerCase(); right.toLowerCase();
    return left == right;
  }
  bool startsWith(const char* prefix) const { return text_.rfind(prefix, 0) == 0; }
  bool endsWith(const char* suffix) const {
    const std::string end(suffix);
    return text_.size() >= end.size() &&
           text_.compare(text_.size() - end.size(), end.size(), end) == 0;
  }
  String& operator+=(const String& other) { text_ += other.text_; return *this; }
  bool operator==(const String& other) const { return text_ == other.text_; }
  bool operator!=(const String& other) const { return !(*this == other); }

 private:
  std::string text_;
};
