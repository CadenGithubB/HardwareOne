#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Stateful counterpart to textWrapInto, for serializers that emit small
// chunks. Existing textWrapAppend deliberately starts at column zero on each
// call; leave that API unchanged for chat/events/sidebar callers.
//
// This is a bounded presentation sink, not a byte-preserving file writer:
// NUL ends input just like textWrapInto; CR/control/tab and soft-wrap handling
// are identical. Writes report consumed input even after display truncation,
// allowing a serializer to finish without allocating an intermediate String.
class TextWrapStream {
 public:
  TextWrapStream(char* dst, size_t capacity, size_t cols,
                 size_t continuationIndent = 0, bool stripControl = true)
      : dst_(dst), capacity_(capacity), cols_(cols ? cols : 1),
        indent_(continuationIndent), stripControl_(stripControl) {
    if (indent_ >= cols_) indent_ = cols_ - 1;
    if (dst_ && capacity_) dst_[0] = '\0';
  }

  size_t write(uint8_t c) {
    ++sourceBytes_;
    if (ended_ || truncated_) return 1;
    if (!c) { ended_ = true; return 1; }
    if (c == '\r') return 1;
    if (c == '\n') {
      if (put('\n')) col_ = 0;
      return 1;
    }
    if (c == '\t') c = ' ';
    else if (stripControl_ && (c < 0x20 || c == 0x7f)) return 1;
    if (col_ >= cols_) {
      if (!put('\n')) return 1;
      col_ = 0;
      for (size_t i = 0; i < indent_; ++i) {
        if (!put(' ')) return 1;
        ++col_;
      }
    }
    if (put(static_cast<char>(c))) ++col_;
    return 1;
  }

  size_t write(const uint8_t* data, size_t length) {
    if (!data) return 0;
    for (size_t i = 0; i < length; ++i) write(data[i]);
    return length;
  }
  size_t write(const char* data, size_t length) {
    return write(reinterpret_cast<const uint8_t*>(data), length);
  }
  size_t write(const char* text) { return text ? write(text, strlen(text)) : 0; }
  size_t size() const { return size_; }
  size_t sourceBytes() const { return sourceBytes_; }
  bool truncated() const { return truncated_; }

 private:
  bool put(char c) {
    if (!dst_ || !capacity_ || size_ >= capacity_ - 1) {
      truncated_ = true;
      return false;
    }
    dst_[size_++] = c;
    dst_[size_] = '\0';
    return true;
  }

  char* dst_;
  size_t capacity_, cols_, indent_;
  bool stripControl_;
  size_t size_ = 0, col_ = 0, sourceBytes_ = 0;
  bool ended_ = false, truncated_ = false;
};
