#ifndef SYSTEM_DEVICE_NAME_H
#define SYSTEM_DEVICE_NAME_H

// ============================================================================
// The device name: one rule, checked here and nowhere else.
//
// One stored name (gSettings.espnowDeviceName, set with 'devicename') is this
// device's ESP-NOW name, its Bluetooth (GAP) name and its Wi-Fi hostname.
//
// Rule: 1-31 bytes of ASCII letters, digits and '-'; it does not start or end
// with '-' (an RFC 1123 hostname label); it is not one of the command words
// clear, all, json or none; and it does not read as a MAC address (twelve hex
// digits, with or without separators, or hex digits in six groups). Names are
// stored as typed and compared ignoring case.
//
// On the wire every frame that carries the name has a 32-byte field
// (kFieldLen). The name is at most 31 bytes, so a NUL is always present; the
// bytes after it are sent as 0 and ignored. A receiver whose getField() fails
// treats the name as absent and shows the address: it never cuts or cleans a
// received name.
//
// The free-text display label (espnowFriendlyName, up to 47 bytes) is separate
// and is not checked here.
//
// Pure header (no Arduino, no ESP-IDF): the host tests compile it directly.
// ============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace devicename {

constexpr size_t kMaxLen = 31;    // bytes of name
constexpr size_t kFieldLen = 32;  // bytes of every wire field that carries it (name + NUL)
// A BLE advertisement's name element holds 29 bytes (31-byte scan response
// minus the 2-byte AD header); a longer name is advertised shortened.
constexpr size_t kAdvertMax = 29;
// The fallback when no name is stored: "HardwareOne-" + the last three
// address bytes in hex (18 bytes, valid under the rule).
constexpr const char* kFallbackPrefix = "HardwareOne";
// What a receiver shows for a sender whose name field is absent or invalid,
// where it needs a name rather than the address ("peer-A1B2C3").
constexpr const char* kPeerPrefix = "peer";

enum class Fault : uint8_t {
  None = 0,
  Empty,
  TooLong,
  BadChar,
  LeadingHyphen,
  TrailingHyphen,
  Reserved,
  ReadsAsAddress,
};

inline bool isAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAsciiHex(char c) {
  return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// Equal ignoring ASCII case. Null reads as "".
inline bool sameIgnoringCase(const char* a, const char* b) {
  if (!a) a = "";
  if (!b) b = "";
  while (*a && *b) {
    if (asciiLower(*a) != asciiLower(*b)) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

// Reads as a MAC address the way the commands parse one (macParse): exactly
// twelve hex digits with any ':' '-' or ' ' between them, or hex digits in
// groups with at least five separators. Such a name could not be told from an
// address by the commands that take "<name|mac>".
inline bool readsAsAddress(const char* s, size_t len) {
  if (!s || len == 0) return false;
  size_t hex = 0, seps = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = s[i];
    if (c == ':' || c == '-' || c == ' ') {
      seps++;
    } else if (isAsciiHex(c)) {
      hex++;
    } else {
      return false;
    }
  }
  return hex == 12 || (hex > 0 && seps >= 5);
}

// The command words a name may not be: 'clear' is the setter's keyword, 'all'
// and 'json' are what the peer commands take for every peer and the JSON form,
// and 'none' is the empty value of the settings pickers.
inline bool isReservedWord(const char* s, size_t len) {
  static const char* const kWords[] = {"clear", "all", "json", "none"};
  for (const char* w : kWords) {
    const size_t wl = strlen(w);
    if (wl != len) continue;
    bool same = true;
    for (size_t i = 0; i < len && same; i++) same = asciiLower(s[i]) == w[i];
    if (same) return true;
  }
  return false;
}

inline Fault check(const char* s, size_t len) {
  if (!s || len == 0) return Fault::Empty;
  if (len > kMaxLen) return Fault::TooLong;
  for (size_t i = 0; i < len; i++) {
    const char c = s[i];
    if (!isAsciiAlpha(c) && !isAsciiDigit(c) && c != '-') return Fault::BadChar;
  }
  if (s[0] == '-') return Fault::LeadingHyphen;
  if (s[len - 1] == '-') return Fault::TrailingHyphen;
  if (isReservedWord(s, len)) return Fault::Reserved;
  if (readsAsAddress(s, len)) return Fault::ReadsAsAddress;
  return Fault::None;
}

inline Fault check(const char* s) { return check(s, s ? strlen(s) : 0); }
inline bool valid(const char* s) { return check(s) == Fault::None; }

// The rule in one sentence, for help text and every refusal.
constexpr const char* kRuleText =
    "A device name is 1-31 letters, digits and '-', does not start or end with '-', "
    "is not clear, all, json or none, and does not read as a MAC address.";

// The one wording every setter returns for a refused name (without "Error: ").
inline const char* faultText(Fault f) {
  switch (f) {
    case Fault::None: return "";
    case Fault::Empty: return "The device name is empty.";
    case Fault::TooLong: return "The device name is longer than 31 bytes.";
    case Fault::BadChar:
      return "The device name may contain only letters, digits and '-' (no spaces, '_' or other characters).";
    case Fault::LeadingHyphen: return "The device name may not start with '-'.";
    case Fault::TrailingHyphen: return "The device name may not end with '-'.";
    case Fault::Reserved: return "The device name may not be clear, all, json or none.";
    case Fault::ReadsAsAddress: return "The device name may not read as a MAC address.";
  }
  return "The device name is not valid.";
}

// Write `name` into a wire field: all kFieldLen bytes are zeroed first, then a
// valid name is copied (it always leaves a NUL). An invalid or null name
// leaves the field empty, which receivers read as "no name". Returns whether
// the name was written.
inline bool putField(char (&field)[kFieldLen], const char* name) {
  memset(field, 0, sizeof(field));
  if (!name) return false;
  const size_t n = strlen(name);
  if (check(name, n) != Fault::None) return false;
  memcpy(field, name, n);
  return true;
}

// Read a received name field of `len` bytes (normally kFieldLen). The name
// must end with a NUL inside the field and pass check(); bytes after the NUL
// are ignored. On success `out` holds the name; on failure `out` is "" and the
// caller shows the sender's address instead. Never cuts, never cleans.
inline bool getField(const char* field, size_t len, char (&out)[kFieldLen]) {
  out[0] = '\0';
  if (!field) return false;
  if (len > kFieldLen) len = kFieldLen;
  const void* nul = memchr(field, 0, len);
  if (!nul) return false;
  const size_t n = (size_t)((const char*)nul - field);
  if (check(field, n) != Fault::None) return false;
  memcpy(out, field, n);
  out[n] = '\0';
  return true;
}

// "<prefix>-A1B2C3" from the last three bytes of `mac`. With no address the
// suffix is "000000".
inline void formatWithMacSuffix(const char* prefix, const uint8_t* mac, char (&out)[kFieldLen]) {
  const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
  const uint8_t* m = mac ? mac : zero;
  snprintf(out, sizeof(out), "%s-%02X%02X%02X", prefix ? prefix : kFallbackPrefix, m[3], m[4], m[5]);
}

// Index of the first of `names` equal to `name` ignoring case, or -1. Null and
// empty entries never match; an empty `name` matches nothing.
inline int clashIndex(const char* name, const char* const names[], size_t n) {
  if (!name || !name[0] || !names) return -1;
  for (size_t i = 0; i < n; i++) {
    if (names[i] && names[i][0] && sameIgnoringCase(name, names[i])) return (int)i;
  }
  return -1;
}

// The name a BLE advertisement carries: the whole name when it fits
// kAdvertMax bytes (*complete = true), else its first kAdvertMax bytes
// (*complete = false, advertised as the shortened name). The GAP device name
// characteristic always holds the whole name. Returns the bytes written.
inline size_t advertisedName(const char* name, char* out, size_t cap, bool* complete) {
  if (complete) *complete = true;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!name) return 0;
  size_t n = strlen(name);
  if (n > kAdvertMax) {
    n = kAdvertMax;
    if (complete) *complete = false;
  }
  if (n >= cap) n = cap - 1;
  memcpy(out, name, n);
  out[n] = '\0';
  return n;
}

// A device name suggested from a free-text label (the setup wizard asks the
// label first and offers this as the name): spaces and '_' become '-', other
// characters that are not allowed are dropped, runs of '-' collapse, the
// result is cut to kMaxLen and '-' at either end is removed. `out` is "" when
// nothing valid is left (the caller then offers the fallback).
inline void suggestFromLabel(const char* label, char (&out)[kFieldLen]) {
  out[0] = '\0';
  if (!label) return;
  size_t n = 0;
  for (const char* p = label; *p && n < kMaxLen; ++p) {
    char c = *p;
    if (c == ' ' || c == '_') c = '-';
    if (!isAsciiAlpha(c) && !isAsciiDigit(c) && c != '-') continue;
    if (c == '-' && (n == 0 || out[n - 1] == '-')) continue;  // no leading '-', no runs
    out[n++] = c;
  }
  while (n > 0 && out[n - 1] == '-') n--;
  out[n] = '\0';
  if (check(out, n) != Fault::None) out[0] = '\0';
}

// ----------------------------------------------------------------------------
// The names stored before the one device name (System_DeviceNameLegacy.h reads
// them from settings.json): the ESP-NOW name (up to 19 bytes of letters,
// digits, '-' and '_') and the Bluetooth-only name (free text, "HardwareOne"
// unless changed). A device updated from such a build has no device name, so
// the settings load takes one of them once:
//   - never when a device name is stored (any value, valid or not);
//   - the ESP-NOW name first, then the Bluetooth name unless it is the old
//     default "HardwareOne" (any case), which every device had and which no
//     one chose;
//   - a value that passes the rule is taken as it is; one that does not is
//     converted as suggestFromLabel does (spaces and '_' to '-', other
//     characters dropped, runs of '-' collapsed, cut to 31 bytes, '-' trimmed
//     at both ends); when nothing valid is left the next one is tried.
// ----------------------------------------------------------------------------

enum class LegacySource : uint8_t { None = 0, EspNow, Bluetooth };

// The old Bluetooth-only name's compiled default.
constexpr const char* kLegacyBluetoothDefault = "HardwareOne";

struct LegacyPick {
  LegacySource source = LegacySource::None;  // None: nothing taken
  bool converted = false;                     // the old value failed the rule and was converted
  char name[kFieldLen] = {};                  // the name taken ("" when source is None)
  // The first old value that was there but gave no valid name, if any.
  LegacySource unusable = LegacySource::None;
};

inline LegacyPick pickLegacy(const char* stored, const char* espnowOld, const char* bluetoothOld) {
  LegacyPick pick;
  if (stored && stored[0]) return pick;  // a device name is stored: never overwritten
  struct Candidate {
    LegacySource source;
    const char* value;
  };
  const Candidate candidates[] = {
      {LegacySource::EspNow, espnowOld},
      {LegacySource::Bluetooth,
       (bluetoothOld && !sameIgnoringCase(bluetoothOld, kLegacyBluetoothDefault)) ? bluetoothOld : nullptr},
  };
  for (const Candidate& c : candidates) {
    if (!c.value || !c.value[0]) continue;
    const size_t n = strlen(c.value);
    if (check(c.value, n) == Fault::None) {
      memcpy(pick.name, c.value, n);
      pick.name[n] = '\0';
      pick.source = c.source;
      pick.converted = false;
      return pick;
    }
    suggestFromLabel(c.value, pick.name);
    if (pick.name[0]) {
      pick.source = c.source;
      pick.converted = true;
      return pick;
    }
    if (pick.unusable == LegacySource::None) pick.unusable = c.source;
  }
  return pick;
}

inline const char* legacySourceText(LegacySource s) {
  switch (s) {
    case LegacySource::EspNow: return "ESP-NOW name";
    case LegacySource::Bluetooth: return "Bluetooth name";
    case LegacySource::None: break;
  }
  return "";
}

}  // namespace devicename

#endif  // SYSTEM_DEVICE_NAME_H
