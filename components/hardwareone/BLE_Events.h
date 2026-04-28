#ifndef BLE_EVENTS_H
#define BLE_EVENTS_H

// =============================================================================
// BLE event helpers — compact JSON + SSE push
// =============================================================================
// Shared infrastructure for any BLE-peer module that wants to publish
// status events to logged-in browser sessions. Two pieces:
//
//   1. CompactJson — bounded JSON object builder. Tracks remaining bytes,
//      escapes string values, and stays brace-balanced even on overflow.
//      Designed for the SSE data-field 128-char cap (EVENT_DATA_MAX in
//      WebServer_Server.h); silently truncating into malformed JSON is the
//      footgun this exists to prevent.
//
//   2. blePushEvent — wraps broadcastEventToAllSessions with a logging
//      side-channel (writes [<EVENT>-SSE-TX] line) so live debugging
//      doesn't require sniffing the wire.
//
// Why exists: g2PushStatusEvent and ringPushStatusEvent grew side-by-side
// and each rolled its own snprintf. Adding a new BLE peer (bond device,
// second pair of glasses, future audio peripheral) would copy that pattern
// for the third time. Centralising removes the copy + adds escaping
// safety.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH

// -----------------------------------------------------------------------------
// CompactJson — bounded object builder
// -----------------------------------------------------------------------------
// Usage:
//   char buf[128];
//   CompactJson j(buf, sizeof(buf));
//   j.kv("s", "connected").kv("rssi", -54).kv("ok", true);
//   if (!j.ok()) { /* something didn't fit */ }
//   broadcastEventToAllSessions("my-event", j.c_str());
//
// Guarantees:
//   * buf is always null-terminated after construction.
//   * c_str() returns a brace-balanced JSON object ('{' ... '}') even
//     when truncation occurred — no half-finished key/value pairs.
//   * String values are escaped: '"' → \", '\' → \\, '\n' → \\n,
//     '\r' → \\r, '\t' → \\t, control chars < 0x20 → \\u00XX.
//   * No heap allocation. Caller owns the buffer.
//
// Limitations:
//   * Object-only (no nested arrays or sub-objects). Add when needed.
//   * Once a kv appends, the leading '{' is fixed. Re-using the buffer
//     for a different object means constructing a new CompactJson.

class CompactJson {
public:
  CompactJson(char* buf, size_t cap);

  // Append "key":value entries. Each returns *this for chaining. If the
  // append would exceed the buffer cap, the entry is skipped and the
  // truncated flag is set; subsequent kvs continue trying so the caller
  // gets as much data as fits.
  CompactJson& kv(const char* key, const char* value);
  CompactJson& kv(const char* key, int value);
  CompactJson& kv(const char* key, long value);
  CompactJson& kv(const char* key, unsigned value);
  CompactJson& kv(const char* key, unsigned long value);
  CompactJson& kv(const char* key, bool value);

  // True if every kv() so far fit cleanly. False if any append was
  // skipped due to overflow.
  bool ok() const { return !truncated_; }

  // Brace-balanced, null-terminated JSON object. Safe to call multiple
  // times — finalisation is idempotent.
  const char* c_str();

private:
  // Append raw bytes, no escaping. Returns false if it didn't fit (and
  // the truncated flag is set). Caller is expected to provide its own
  // escaping if needed.
  bool appendRaw(const char* src, size_t len);

  // Append an escaped JSON string (surrounding quotes included). Returns
  // false on overflow (sets truncated_).
  bool appendString(const char* s);

  // Add a leading comma if this isn't the first kv. Returns false on
  // overflow.
  bool appendSeparator();

  char*  buf_;
  size_t cap_;        // total buffer capacity, including the closing '}' + NUL
  size_t pos_;        // current write position (where '}' would go now)
  bool   first_;      // true until the first kv appends
  bool   truncated_;
  bool   finalised_;  // c_str() has been called; closing '}' is in place
};

// -----------------------------------------------------------------------------
// blePushEvent — fire an SSE event with the CompactJson payload
// -----------------------------------------------------------------------------
// Wraps broadcastEventToAllSessions(eventName, json.c_str()). Logs the
// payload to debug output so live debugging doesn't need a wire sniffer.
// Returns true if the JSON fit cleanly; false if any field was dropped
// due to the 128-char cap (caller can downgrade to a shorter reason
// string and retry, or just log the loss).
//
// `eventName` is the SSE event-type identifier — same string the browser
// listener uses with addEventListener(). Conventional shapes:
//   "g2-status", "ring-status", "phone-status", ...
bool blePushEvent(const char* eventName, CompactJson& json);

#else  // !ENABLE_BLUETOOTH

class CompactJson {
public:
  CompactJson(char*, size_t) {}
  CompactJson& kv(const char*, const char*) { return *this; }
  CompactJson& kv(const char*, int)         { return *this; }
  CompactJson& kv(const char*, bool)        { return *this; }
  bool ok() const                           { return true; }
  const char* c_str()                       { return ""; }
};
inline bool blePushEvent(const char*, CompactJson&) { return false; }

#endif  // ENABLE_BLUETOOTH
#endif  // BLE_EVENTS_H
