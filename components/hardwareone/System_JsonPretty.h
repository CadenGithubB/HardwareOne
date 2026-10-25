// =============================================================================
// System_JsonPretty — streaming JSON re-indenter shared by the file viewers
// =============================================================================
// Every viewer that shows a .json file "pretty" goes through this one
// formatter: the web file viewer (streamed per read buffer), the G2 lens Files
// page and the OLED file browser (both into a TextWrapStream). It re-indents
// the text a byte at a time instead of parsing it into a document, so it
// allocates nothing, handles any file size, and still lays out malformed or
// truncated JSON as far as its brackets go. String contents pass through
// untouched: the viewer shows the file's own text (its escapes and number
// spelling), not a re-serialization of it.
//
// Layout matches ArduinoJson's serializeJsonPretty for valid input: two-space
// indent, one member or element per line, `"key": value`, and an empty {} or
// [] kept on one line; lines end in '\n' where ArduinoJson writes "\r\n".
// Whitespace outside strings is the file's own and is dropped. Indentation stops growing past kMaxIndentLevels so pathological
// nesting can't turn each byte of input into a line of spaces.
//
// Pure byte logic with no Arduino or display dependency (host tests compile it
// directly). Output goes to any callable taking a char; state carries across
// calls, so a caller may feed the file in pieces:
//   JsonPrettyFormatter pretty;
//   pretty.write(buf, n, [&](char c) { sink.write(static_cast<uint8_t>(c)); });

#pragma once

#include <stddef.h>

class JsonPrettyFormatter {
 public:
  static constexpr size_t kMaxIndentLevels = 16;

  template <typename Emit>
  void put(char c, Emit&& emit) {
    if (inString_) {
      emit(c);
      if (escaped_) escaped_ = false;
      else if (c == '\\') escaped_ = true;
      else if (c == '"') inString_ = false;
      return;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return;
    if (openPending_) {
      openPending_ = false;
      if (c == '}' || c == ']') {        // empty container: close on the same line
        if (depth_ > 0) --depth_;
        emit(c);
        return;
      }
      newLine(emit);
    }
    switch (c) {
      case '"':
        inString_ = true;
        emit(c);
        break;
      case '{':
      case '[':
        emit(c);
        ++depth_;
        openPending_ = true;             // the newline waits: it may be empty
        break;
      case '}':
      case ']':
        if (depth_ > 0) --depth_;
        newLine(emit);
        emit(c);
        break;
      case ',':
        emit(c);
        newLine(emit);
        break;
      case ':':
        emit(c);
        emit(' ');
        break;
      default:
        emit(c);
        break;
    }
  }

  template <typename Emit>
  void write(const char* data, size_t length, Emit&& emit) {
    if (!data) return;
    for (size_t i = 0; i < length; ++i) put(data[i], emit);
  }

 private:
  template <typename Emit>
  void newLine(Emit& emit) {
    emit('\n');
    const size_t levels = depth_ < kMaxIndentLevels ? depth_ : kMaxIndentLevels;
    for (size_t i = 0; i < levels * 2; ++i) emit(' ');
  }

  size_t depth_ = 0;
  bool inString_ = false, escaped_ = false, openPending_ = false;
};
