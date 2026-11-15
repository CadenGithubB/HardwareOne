#pragma once

#include "System_MemUtil.h"  // ps_alloc, ps_free; ArduinoJson

// One JSON document's text in one PSRAM block of measureJson(doc) + 1 bytes,
// sized before anything is written. serializeJson(doc, String&) instead frees
// the String and grows it again for every 31 characters it writes, each time
// in internal RAM below 16 KB. The text is the same bytes.
//
// When the block cannot be had, ok() is false and the text is empty (c_str()
// is "", length() 0): no partial document is ever handed out. PreferPsram falls
// back to internal RAM before that happens.
class PsramJsonText final {
public:
  PsramJsonText(const JsonDocument& doc, const char* tag)
      : length_(measureJson(doc)),
        text_(static_cast<char*>(ps_alloc(length_ + 1, AllocPref::PreferPsram, tag))) {
    if (text_ && serializeJson(doc, text_, length_ + 1) != length_) release();
  }
  ~PsramJsonText() { release(); }
  PsramJsonText(const PsramJsonText&) = delete;
  PsramJsonText& operator=(const PsramJsonText&) = delete;

  bool ok() const { return text_ != nullptr; }
  const char* c_str() const { return text_ ? text_ : ""; }
  size_t length() const { return text_ ? length_ : 0; }

  // Overwrites the text, for a reply that carries private text, before the
  // block goes back to the heap.
  void wipe() {
    if (!text_) return;
    volatile char* bytes = text_;
    for (size_t i = 0; i <= length_; ++i) bytes[i] = 0;
  }

private:
  void release() {
    ps_free(text_);
    text_ = nullptr;
  }
  const size_t length_;
  char* text_;
};
