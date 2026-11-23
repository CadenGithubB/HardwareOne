#pragma once
// ESP-NOW revocation (SEC-6, 1.0 design section 5.1 A + B) without the radio,
// the filesystem or the console:
//   * the revoked list: up to kMax devices, each by its long-term Ed25519 key
//     (when this device knew it) and its MAC, with the device name and the
//     time of the revoke for messages;
//   * its stored form, /system/espnow/revoked.json ("schema":1, keys and MACs
//     in lowercase hex), and the fail-closed rule for a file that is there but
//     cannot be read;
//   * which key a key exchange may store (identity only for a registry entry);
//   * when the boot check may delete identity files that have no registry
//     entry;
//   * the first-key window of a registry entry made by espnowpairsecure.
//
// Pure on purpose: ArduinoJson and the C library only, no Arduino, no ESP-IDF,
// no statics. System_ESPNow_Revoked.cpp owns the table, its lock and the file;
// the host test (test_espnow_revoked_core.cpp) compiles this file as it is.
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace espnowrevoke {

constexpr uint8_t kMax = 32;                 // E23: a full list still unpairs, and says so
constexpr uint8_t kNameCap = 32;             // a device name (31 bytes) + NUL
constexpr uint8_t kSchema = 1;
constexpr uint8_t kFlagHasKey = 0x01;        // a revoke by MAC with no stored key stores "no key"
constexpr uint32_t kFirstKeyWindowMs = 10UL * 60UL * 1000UL;   // E31
constexpr size_t kKeyIdHexLen = 16;          // keyId = the first 8 key bytes in hex

struct Entry {
  uint8_t pub[32];
  uint8_t mac[6];
  uint8_t meshSlot;
  uint8_t flags;
  uint64_t revokedEpochMs;   // 0 = the clock was not set
  char name[kNameCap];
};

struct Table {
  Entry e[kMax];
  uint8_t count;
};

inline bool hasKey(const Entry& x) { return (x.flags & kFlagHasKey) != 0; }

inline int findMac(const Table& t, const uint8_t mac[6]) {
  if (!mac) return -1;
  for (uint8_t i = 0; i < t.count && i < kMax; i++)
    if (memcmp(t.e[i].mac, mac, 6) == 0) return i;
  return -1;
}

// Never matches an entry without a key: "no key" is not an all-zero key.
inline int findKey(const Table& t, const uint8_t pub[32]) {
  if (!pub) return -1;
  for (uint8_t i = 0; i < t.count && i < kMax; i++)
    if (hasKey(t.e[i]) && memcmp(t.e[i].pub, pub, 32) == 0) return i;
  return -1;
}

// A listed MAC with any key, or a listed key under any MAC (pub may be null).
inline int findMacOrKey(const Table& t, const uint8_t mac[6], const uint8_t pub[32]) {
  const int byMac = findMac(t, mac);
  return byMac >= 0 ? byMac : findKey(t, pub);
}

// A MAC that can name one device: not all zero, and not a group address
// (broadcast FF:FF:FF:FF:FF:FF included: the group bit of the first byte).
// Revoke and unpair refuse any other: tearing down the broadcast radio peer
// would make every broadcast send fail until a restart.
inline bool isUnicastMac(const uint8_t mac[6]) {
  if (!mac || (mac[0] & 0x01)) return false;
  for (int i = 0; i < 6; i++)
    if (mac[i]) return true;
  return false;
}

inline void formatKeyId(const uint8_t pub[32], char out[kKeyIdHexLen + 1]) {
  for (size_t i = 0; i < kKeyIdHexLen / 2; i++) snprintf(out + 2 * i, 3, "%02x", pub[i]);
  out[kKeyIdHexLen] = '\0';
}

enum class AddResult : uint8_t { Added, Updated, Full };

// Adds a revoke, or refreshes the entry already listed for this MAC. Only a
// MAC match is merged: the same key under another MAC is a second entry, so
// the first MAC stays listed (gate 1, relay, slots and heartbeats check by
// MAC). pub may be null (revoke by MAC with no stored key).
inline AddResult add(Table& t, const uint8_t mac[6], const uint8_t pub[32], uint8_t meshSlot,
                     uint64_t revokedEpochMs, const char* name) {
  int i = findMac(t, mac);
  AddResult r = AddResult::Updated;
  if (i < 0) {
    if (t.count >= kMax) return AddResult::Full;
    i = t.count++;
    r = AddResult::Added;
  }
  Entry& x = t.e[i];
  memset(&x, 0, sizeof(x));
  memcpy(x.mac, mac, 6);
  if (pub) { memcpy(x.pub, pub, 32); x.flags |= kFlagHasKey; }
  x.meshSlot = meshSlot;
  x.revokedEpochMs = revokedEpochMs;
  if (name) snprintf(x.name, sizeof(x.name), "%s", name);   // cut at 31 bytes (a device name fits)
  return r;
}

inline void removeAt(Table& t, int i) {
  if (i < 0 || i >= t.count) return;
  for (int j = i; j + 1 < t.count; j++) t.e[j] = t.e[j + 1];
  t.count--;
  memset(&t.e[t.count], 0, sizeof(Entry));
}

inline bool hexNibble(char c, uint8_t& v) {
  if (c >= '0' && c <= '9') { v = (uint8_t)(c - '0'); return true; }
  if (c >= 'a' && c <= 'f') { v = (uint8_t)(c - 'a' + 10); return true; }
  if (c >= 'A' && c <= 'F') { v = (uint8_t)(c - 'A' + 10); return true; }
  return false;
}

// Exactly 2*n hex digits into n bytes.
inline bool parseHex(const char* s, uint8_t* out, size_t n) {
  if (!s || strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    uint8_t hi, lo;
    if (!hexNibble(s[2 * i], hi) || !hexNibble(s[2 * i + 1], lo)) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

inline void formatHex(const uint8_t* in, size_t n, char* out) {
  for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", in[i]);
  out[2 * n] = '\0';
}

// The entry `text` names for espnowunrevoke: a listed name (exact), a MAC
// (12 hex digits, ':' or '-' separators allowed) or a keyId (4 to 64 hex
// digits, a prefix of a listed key; it must name one entry).
inline int findByText(const Table& t, const char* text) {
  if (!text || !*text) return -1;
  for (uint8_t i = 0; i < t.count; i++)
    if (t.e[i].name[0] && strcmp(t.e[i].name, text) == 0) return i;
  char hex[65];
  size_t n = 0;
  for (const char* p = text; *p; p++) {
    if (*p == ':' || *p == '-') continue;
    uint8_t v;
    if (!hexNibble(*p, v) || n >= sizeof(hex) - 1) return -1;
    hex[n++] = (char)(*p >= 'A' && *p <= 'F' ? *p - 'A' + 'a' : *p);
  }
  hex[n] = '\0';
  if (n == 12) {
    uint8_t mac[6];
    if (parseHex(hex, mac, 6)) {
      const int byMac = findMac(t, mac);
      if (byMac >= 0) return byMac;
    }
  }
  if (n < 4 || strchr(text, ':')) return -1;
  int hit = -1;
  for (uint8_t i = 0; i < t.count; i++) {
    if (!hasKey(t.e[i])) continue;
    char keyHex[65];
    formatHex(t.e[i].pub, 32, keyHex);
    if (strncmp(keyHex, hex, n) == 0) {
      if (hit >= 0) return -1;   // ambiguous
      hit = i;
    }
  }
  return hit;
}

// ---- revoked.json -------------------------------------------------------------

enum class Load : uint8_t {
  Ok,       // read; the table holds the file
  Absent,   // no file: the list is empty
  Locked,   // a file that is there but cannot be read: fail closed (no new keys, no pairing)
};

// The stored form of the file at `text` (len bytes). Anything that is not a
// whole, readable schema-1 document is Locked, never an empty list: an empty
// list would lift every revoke.
// `doc` is the caller's (the firmware passes a PSRAM-backed one).
inline Load parse(const char* text, size_t len, Table& out, JsonDocument& doc) {
  memset(&out, 0, sizeof(out));
  doc.clear();
  if (!text || deserializeJson(doc, text, len) != DeserializationError::Ok) return Load::Locked;
  if (!doc.is<JsonObject>() || (doc["schema"] | 0) != kSchema) return Load::Locked;
  JsonArrayConst arr = doc["revoked"].as<JsonArrayConst>();
  if (!doc["revoked"].is<JsonArrayConst>()) return Load::Locked;
  for (JsonObjectConst o : arr) {
    if (out.count >= kMax) return Load::Locked;
    Entry& x = out.e[out.count];
    if (!parseHex(o["mac"] | "", x.mac, 6)) return Load::Locked;
    const char* key = o["key"] | "";
    if (key[0]) {
      if (!parseHex(key, x.pub, 32)) return Load::Locked;
      x.flags |= kFlagHasKey;
    }
    const unsigned slot = o["meshSlot"] | 0u;
    if (slot > 255) return Load::Locked;
    x.meshSlot = (uint8_t)slot;
    x.revokedEpochMs = o["revokedEpochMs"] | (uint64_t)0;
    const char* name = o["name"] | "";
    if (strlen(name) >= kNameCap) return Load::Locked;
    snprintf(x.name, sizeof(x.name), "%s", name);   // fits: checked above
    out.count++;
  }
  return Load::Ok;
}

// The file text for `t` into `out` (cap bytes). 0 when it does not fit.
inline size_t serialize(const Table& t, char* out, size_t cap, JsonDocument& doc) {
  doc.clear();
  doc["schema"] = kSchema;
  JsonArray arr = doc["revoked"].to<JsonArray>();
  for (uint8_t i = 0; i < t.count; i++) {
    const Entry& x = t.e[i];
    JsonObject o = arr.add<JsonObject>();
    char mac[13], key[65];
    formatHex(x.mac, 6, mac);
    o["mac"] = mac;   // copied: the buffers are local
    if (hasKey(x)) { formatHex(x.pub, 32, key); o["key"] = key; }
    if (x.meshSlot) o["meshSlot"] = x.meshSlot;
    o["revokedEpochMs"] = x.revokedEpochMs;
    if (x.name[0]) o["name"] = x.name;
  }
  if (doc.overflowed()) return 0;
  const size_t need = measureJson(doc);
  if (!out || need + 1 > cap) return 0;
  return serializeJson(doc, out, cap);
}

// Room for a full list: 32 entries of about 190 bytes each.
constexpr size_t kFileCap = 7168;

// What `espnowunrevoke --all` does. A list locked because the file could not
// be read at all (no filesystem, no memory: loaded false) is read again first;
// only a file that was read and is unreadable is moved aside.
enum class ClearAction : uint8_t { WriteEmpty, RetryLoad, Quarantine };
inline ClearAction clearAction(bool locked, bool loaded) {
  if (!locked) return ClearAction::WriteEmpty;
  return loaded ? ClearAction::Quarantine : ClearAction::RetryLoad;
}

// ---- Which key a key exchange may store (rule A) --------------------------------

enum class Admit : uint8_t {
  Accept,        // go on: store (or confirm) the key; the key-change rule still applies
  NotAccepted,   // answer KEY_EX_CONFIRM status 3 ("not accepted"), store nothing
  Revoked,       // no answer at all (G3), store nothing
};

struct AdmitInputs {
  bool revoked;           // the MAC or the key is on the revoked list
  bool inRegistry;        // the MAC has a device-registry entry
  bool hasIdentity;       // an identity is stored for the MAC
  bool sameKey;           // ... and it is the key presented
  bool listLocked;        // revoked.json is there but unreadable (fail closed)
  bool firstKeyOpen;      // a local pairing action for the MAC is open (first-key window)
};

// An identity exists only for a registry entry: a first key needs an open
// pairing action for the MAC; a key already stored is confirmed; a changed
// key goes on to the key-change rule (the one-shot re-key window). While the
// revoked list cannot be read, nothing new is stored.
inline Admit admit(const AdmitInputs& in) {
  if (in.revoked) return Admit::Revoked;
  if (!in.inRegistry) return Admit::NotAccepted;
  if (in.hasIdentity && in.sameKey) return Admit::Accept;
  if (in.listLocked) return Admit::NotAccepted;
  if (in.hasIdentity) return Admit::Accept;   // a changed key: the re-key window decides
  return in.firstKeyOpen ? Admit::Accept : Admit::NotAccepted;
}

// KEY_EX_CONFIRM status for "not accepted" (not paired here, or revoked): one
// code for both (E24).
constexpr uint8_t kConfirmNotAccepted = 3;

// ---- First-key window (E31) ------------------------------------------------------

// A registry entry made by espnowpairsecure waits at most kFirstKeyWindowMs
// for the peer's first key. untilMs 0 = no window.
inline uint32_t firstKeyUntil(uint32_t nowMs) {
  const uint32_t until = nowMs + kFirstKeyWindowMs;
  return until ? until : 1;
}
inline bool firstKeyOpen(uint32_t untilMs, uint32_t nowMs) {
  return untilMs != 0 && (int32_t)(untilMs - nowMs) > 0;
}

// The window as stored (awaitingKeyUntilEpochMs) and read back at the next
// start: only with a set clock on both sides, never longer than the window.
// Returns the millis() deadline, or 0 when the window is over or not known.
inline uint32_t firstKeyFromStored(uint64_t storedEpochMs, uint64_t nowEpochMs, uint32_t nowMs) {
  if (storedEpochMs == 0 || nowEpochMs == 0 || storedEpochMs <= nowEpochMs) return 0;
  const uint64_t left = storedEpochMs - nowEpochMs;
  if (left > kFirstKeyWindowMs) return 0;   // a clock that moved back: not trusted
  const uint32_t until = nowMs + (uint32_t)left;
  return until ? until : 1;
}

// ---- Boot check -----------------------------------------------------------------

// The device registry's load result, as the boot check needs it.
enum class RegistryLoad : uint8_t { Ok, Absent, Quarantined, NotRead, Partial };

// An identity file with no registry entry is deleted only when the registry
// was read whole or there truly is no file. A quarantined (corrupt), unread
// (not mounted, open failed, no memory) or partial registry would mass-unpair.
inline bool sweepAllowed(RegistryLoad r) {
  return r == RegistryLoad::Ok || r == RegistryLoad::Absent;
}

}  // namespace espnowrevoke
