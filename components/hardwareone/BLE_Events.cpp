// =============================================================================
// BLE_Events.cpp — CompactJson + blePushEvent
// =============================================================================
// Implementation notes:
//   * The buffer is reserved with one byte for the closing '}' and one for
//     NUL termination — so writable space is cap_ - 2.
//   * pos_ tracks the next free byte. It always points to where the '}'
//     would land if c_str() were called right now. We don't physically
//     write the '}' until c_str() is called (idempotent finalisation).
//   * Each kv() call either fits entirely or is skipped — never a partial
//     write that would leave dangling syntax in the buffer.

#include "BLE_Events.h"

#if ENABLE_BLUETOOTH

#include "WebServer_Server.h"   // broadcastEventToAllSessions
#include "System_SensorStubs.h"  // ...whose no-op stub lives here when ENABLE_HTTP_SERVER=0 (BT-on/web-off builds)
#include "System_Debug.h"

#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// CompactJson
// -----------------------------------------------------------------------------

CompactJson::CompactJson(char* buf, size_t cap)
    : buf_(buf), cap_(cap), pos_(0),
      first_(true), truncated_(false), finalised_(false) {
  // Need room for at least '{', '}', '\0'. Anything smaller is a caller bug;
  // make it safe rather than crash.
  if (!buf_ || cap_ < 3) {
    if (buf_ && cap_ > 0) buf_[0] = '\0';
    truncated_ = true;
    cap_ = 0;
    return;
  }
  buf_[0]    = '{';
  pos_       = 1;
  buf_[pos_] = '\0';   // keep buffer null-terminated even before any kv
}

bool CompactJson::appendRaw(const char* src, size_t len) {
  if (cap_ == 0) { truncated_ = true; return false; }
  // Reserve 2 bytes for the closing '}' and the NUL.
  if (pos_ + len + 2 > cap_) {
    truncated_ = true;
    return false;
  }
  memcpy(buf_ + pos_, src, len);
  pos_ += len;
  buf_[pos_] = '\0';
  return true;
}

bool CompactJson::appendSeparator() {
  if (first_) { first_ = false; return true; }
  return appendRaw(",", 1);
}

bool CompactJson::appendString(const char* s) {
  if (!s) s = "";
  // Conservative pre-flight: most strings have no chars needing escapes,
  // so cost out the no-escape case first. If anything in the string would
  // need escaping, fall through to the per-char loop.
  size_t n = strlen(s);
  bool needsEscape = false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\' || c < 0x20) { needsEscape = true; break; }
  }
  if (!needsEscape) {
    if (!appendRaw("\"", 1))   return false;
    if (!appendRaw(s, n))       return false;
    if (!appendRaw("\"", 1))   return false;
    return true;
  }
  // Slow path — escape per char. Each control char can expand to 6 bytes
  // (\uXXXX), which is why we guard each write individually.
  if (!appendRaw("\"", 1)) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"')  { if (!appendRaw("\\\"", 2)) return false; }
    else if (c == '\\') { if (!appendRaw("\\\\", 2)) return false; }
    else if (c == '\n') { if (!appendRaw("\\n",  2)) return false; }
    else if (c == '\r') { if (!appendRaw("\\r",  2)) return false; }
    else if (c == '\t') { if (!appendRaw("\\t",  2)) return false; }
    else if (c < 0x20) {
      char esc[7];
      // 6 bytes + NUL — snprintf takes care of the format
      int w = snprintf(esc, sizeof(esc), "\\u%04x", c);
      if (w != 6 || !appendRaw(esc, 6)) return false;
    } else {
      char one = (char)c;
      if (!appendRaw(&one, 1)) return false;
    }
  }
  if (!appendRaw("\"", 1)) return false;
  return true;
}

CompactJson& CompactJson::kv(const char* key, const char* value) {
  // Snapshot pos_ so we can roll back on partial-fit failure — keeps the
  // buffer in a consistent state even when a kv overflows mid-write.
  size_t saved = pos_;
  bool   savedFirst = first_;
  if (!appendSeparator())     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendString(key))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(":", 1))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendString(value))   { pos_ = saved; first_ = savedFirst; return *this; }
  return *this;
}

CompactJson& CompactJson::kv(const char* key, int value) {
  char num[16];
  int w = snprintf(num, sizeof(num), "%d", value);
  if (w <= 0) { truncated_ = true; return *this; }
  size_t saved = pos_; bool savedFirst = first_;
  if (!appendSeparator())     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendString(key))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(":", 1))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(num, (size_t)w)) { pos_ = saved; first_ = savedFirst; return *this; }
  return *this;
}

CompactJson& CompactJson::kv(const char* key, long value) {
  char num[24];
  int w = snprintf(num, sizeof(num), "%ld", value);
  if (w <= 0) { truncated_ = true; return *this; }
  size_t saved = pos_; bool savedFirst = first_;
  if (!appendSeparator())     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendString(key))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(":", 1))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(num, (size_t)w)) { pos_ = saved; first_ = savedFirst; return *this; }
  return *this;
}

CompactJson& CompactJson::kv(const char* key, unsigned value) {
  char num[16];
  int w = snprintf(num, sizeof(num), "%u", value);
  if (w <= 0) { truncated_ = true; return *this; }
  size_t saved = pos_; bool savedFirst = first_;
  if (!appendSeparator())     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendString(key))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(":", 1))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(num, (size_t)w)) { pos_ = saved; first_ = savedFirst; return *this; }
  return *this;
}

CompactJson& CompactJson::kv(const char* key, unsigned long value) {
  char num[24];
  int w = snprintf(num, sizeof(num), "%lu", value);
  if (w <= 0) { truncated_ = true; return *this; }
  size_t saved = pos_; bool savedFirst = first_;
  if (!appendSeparator())     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendString(key))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(":", 1))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(num, (size_t)w)) { pos_ = saved; first_ = savedFirst; return *this; }
  return *this;
}

CompactJson& CompactJson::kv(const char* key, bool value) {
  const char* lit = value ? "true" : "false";
  size_t litLen   = value ? 4 : 5;
  size_t saved = pos_; bool savedFirst = first_;
  if (!appendSeparator())     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendString(key))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(":", 1))     { pos_ = saved; first_ = savedFirst; return *this; }
  if (!appendRaw(lit, litLen)){ pos_ = saved; first_ = savedFirst; return *this; }
  return *this;
}

const char* CompactJson::c_str() {
  if (cap_ == 0 || !buf_) return "";
  if (!finalised_) {
    // Always have room for '}' + '\0' because every appendRaw reserved it.
    buf_[pos_++] = '}';
    buf_[pos_]   = '\0';
    finalised_   = true;
  }
  return buf_;
}

// -----------------------------------------------------------------------------
// blePushEvent
// -----------------------------------------------------------------------------

bool blePushEvent(const char* eventName, CompactJson& json) {
  if (!eventName) return false;
  const char* s = json.c_str();
  const bool clean = json.ok();
  // Mirror the prior g2PushStatusEvent / ringPushStatusEvent log line so
  // existing log-watching habits keep working. The TRUNCATED suffix is
  // the cheap signal that 128 bytes wasn't enough — fix the caller, not
  // the cap.
  DEBUG_G2F("[%s-TX] %s%s",
            eventName, s,
            clean ? "" : "  (TRUNCATED)");
  broadcastEventToAllSessions(eventName, s);
  return clean;
}

#endif  // ENABLE_BLUETOOTH
