// The ESP-NOW revocation rules (System_ESPNow_RevokedCore.h, 1.0 design 5.1
// A + B) as they are, with the vendored ArduinoJson:
//   R1 the revoked list: by MAC, by key under any MAC, "no key" never matches
//      an all-zero key, a full list (32) refuses, removal by name, MAC or keyId
//   R2 revoked.json: a written list reads back the same; a file that is there
//      but cannot be read (truncated, not JSON, wrong schema, a bad MAC or key,
//      too many entries) locks the list (fail closed), never reads as empty
//   R3 rule A: an identity only for a registry entry, a first key only inside
//      the first-key window, a revoked MAC or key gets no answer, nothing new
//      while the list is locked
//   R4 the boot identity check runs only after an Ok load or a truly absent
//      registry: a quarantined, partial or unread one skips it
//   R5 the first-key window (E31): 10 minutes, across the millis() rollover,
//      and as stored across a restart
#include "System_ESPNow_RevokedCore.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace espnowrevoke;

static int gFailures = 0;
static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) gFailures++;
}

static void mac(uint8_t out[6], uint8_t n) {
  const uint8_t m[6] = {0x02, 0, 0, 0, 0x10, n};
  memcpy(out, m, 6);
}
static void key(uint8_t out[32], uint8_t k) { memset(out, k, 32); }

static std::string save(const Table& t) {
  static char buf[kFileCap];
  JsonDocument doc;
  const size_t n = serialize(t, buf, sizeof buf, doc);
  return std::string(buf, n);
}
static Load load(const std::string& text, Table& out) {
  JsonDocument doc;
  return parse(text.data(), text.size(), out, doc);
}

int main() {
  static Table t;   // 2.5 KB: off the stack, as in the firmware
  uint8_t m1[6], m2[6], m3[6], k1[32], k2[32], zero[32];
  mac(m1, 1); mac(m2, 2); mac(m3, 3);
  key(k1, 0xA1); key(k2, 0xB2); memset(zero, 0, 32);

  // R1
  memset(&t, 0, sizeof t);
  check(add(t, m1, k1, 0, 1800000000000ULL, "porch") == AddResult::Added, "R1 a revoke is added");
  check(add(t, m2, nullptr, 1, 0, "lost-one") == AddResult::Added, "R1 a revoke by MAC with no stored key is added");
  check(findMac(t, m1) == 0 && findKey(t, k1) == 0, "R1 found by its MAC and by its key");
  check(findMacOrKey(t, m3, k1) == 0, "R1 the revoked key under another MAC is found (any MAC)");
  check(findMacOrKey(t, m2, k2) == 1, "R1 the revoked MAC with another key is found (any key)");
  check(findKey(t, zero) < 0, "R1 'no key' never matches an all-zero key");
  check(add(t, m3, k1, 0, 5, "porch2") == AddResult::Added && t.count == 3 && findMac(t, m1) == 0 &&
            findMac(t, m3) == 2,
        "R1 the same key under another MAC is a second entry: the first MAC stays listed (review 1)");
  check(add(t, m1, k2, 0, 6, "porch") == AddResult::Updated && t.count == 3 && findMac(t, m3) == 2,
        "R1 a second revoke of a listed MAC refreshes its own entry only");
  memset(&t, 0, sizeof t);
  for (uint8_t i = 0; i < kMax; i++) {
    uint8_t m[6], k[32];
    mac(m, (uint8_t)(0x40 + i));
    key(k, (uint8_t)(0x40 + i));
    add(t, m, k, 0, 1, "x");
  }
  check(t.count == kMax && add(t, m1, k1, 0, 1, "full") == AddResult::Full && findMac(t, m1) < 0,
        "R1 a full list (32) refuses the 33rd and lists nothing new");
  memset(&t, 0, sizeof t);
  add(t, m1, k1, 0, 1, "porch");
  add(t, m2, nullptr, 0, 1, "garage");
  add(t, m3, k2, 0, 1, "attic");
  check(findByText(t, "garage") == 1, "R1 unrevoke by name");
  check(findByText(t, "02:00:00:00:10:03") == 2 && findByText(t, "020000001003") == 2, "R1 unrevoke by MAC");
  check(findByText(t, "a1a1a1a1a1a1a1a1") == 0 && findByText(t, "B2B2") == 2, "R1 unrevoke by keyId (prefix)");
  check(findByText(t, "c3c3") < 0 && findByText(t, "zz") < 0 && findByText(t, "") < 0,
        "R1 an unknown name, key or text finds nothing");
  removeAt(t, 1);
  check(t.count == 2 && findMac(t, m2) < 0 && findMac(t, m3) == 1, "R1 a removed entry is gone, the rest kept");
  char id[kKeyIdHexLen + 1];
  formatKeyId(k1, id);
  check(std::string(id) == "a1a1a1a1a1a1a1a1", "R1 the keyId is the first 16 hex digits of the key, lowercase");

  // R6 (review 6) — only a unicast MAC can be revoked or unpaired by address.
  {
    const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t mcast[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
    const uint8_t zeroMac[6] = {0, 0, 0, 0, 0, 0};
    check(isUnicastMac(m1) && !isUnicastMac(bcast) && !isUnicastMac(mcast) && !isUnicastMac(zeroMac) &&
              !isUnicastMac(nullptr),
          "R6 broadcast, multicast and all-zero MACs are refused; a device MAC is taken");
  }
  // R7 (review 7) — espnowunrevoke --all moves aside only a file that was read.
  check(clearAction(false, true) == ClearAction::WriteEmpty && clearAction(true, false) == ClearAction::RetryLoad &&
            clearAction(true, true) == ClearAction::Quarantine,
        "R7 a list locked because nothing could be read is read again first; only an unreadable file moves");

  // R2
  memset(&t, 0, sizeof t);
  add(t, m1, k1, 2, 1800000000123ULL, "porch");
  add(t, m2, nullptr, 0, 0, "");
  const std::string text = save(t);
  Table back;
  check(load(text, back) == Load::Ok && back.count == 2 && memcmp(back.e[0].pub, k1, 32) == 0 &&
            back.e[0].meshSlot == 2 && back.e[0].revokedEpochMs == 1800000000123ULL &&
            std::string(back.e[0].name) == "porch" && !hasKey(back.e[1]) && memcmp(back.e[1].mac, m2, 6) == 0,
        "R2 a written list reads back the same (key, MAC, mesh, time, name, no key)");
  check(text.find("\"schema\":1") != std::string::npos && text.find("a1a1a1a1") != std::string::npos &&
            text.find("A1A1") == std::string::npos,
        "R2 the file has schema 1 and keys in lowercase hex");
  check(load("{\"schema\":1,\"revoked\":[]}", back) == Load::Ok && back.count == 0, "R2 an empty list reads as empty");
  const char* bad[] = {
      "",                                              // empty file
      "{\"schema\":1,\"revoked\":[{\"mac\":\"0200",    // truncated
      "not json at all",
      "{\"schema\":2,\"revoked\":[]}",                 // another schema
      "{\"revoked\":[]}",                              // no schema
      "{\"schema\":1}",                                // no list
      "{\"schema\":1,\"revoked\":[{\"mac\":\"zz0000001001\"}]}",
      "{\"schema\":1,\"revoked\":[{\"mac\":\"020000001001\",\"key\":\"a1\"}]}",
      "[1,2,3]",
  };
  bool allLocked = true;
  for (const char* b : bad) {
    Table x;
    memset(&x, 0xEE, sizeof x);
    allLocked = allLocked && load(b, x) == Load::Locked && x.count == 0;
  }
  check(allLocked, "R2 an unreadable revoked.json (9 kinds) locks the list and lists nothing: fail closed");
  {
    std::string many = "{\"schema\":1,\"revoked\":[";
    for (int i = 0; i < kMax + 1; i++) {
      char e[64];
      std::snprintf(e, sizeof e, "%s{\"mac\":\"0200000010%02x\"}", i ? "," : "", i);
      many += e;
    }
    many += "]}";
    check(load(many, back) == Load::Locked, "R2 more than 32 entries locks the list");
  }

  // R3
  AdmitInputs in = {};
  in.inRegistry = true;
  in.firstKeyOpen = true;
  check(admit(in) == Admit::Accept, "R3 a registry entry inside its first-key window stores its first key");
  in.firstKeyOpen = false;
  check(admit(in) == Admit::NotAccepted, "R3 a registry entry whose window is over stores no first key");
  in = {};
  in.firstKeyOpen = true;
  check(admit(in) == Admit::NotAccepted, "R3 a MAC with no registry entry stores nothing (status 3)");
  in = {};
  in.inRegistry = true; in.hasIdentity = true; in.sameKey = true;
  check(admit(in) == Admit::Accept, "R3 the key already stored is confirmed (no window needed)");
  in.sameKey = false;
  check(admit(in) == Admit::Accept, "R3 a changed key goes on to the re-key window rule");
  in = {};
  in.revoked = true; in.inRegistry = true; in.hasIdentity = true; in.sameKey = true; in.firstKeyOpen = true;
  check(admit(in) == Admit::Revoked, "R3 a revoked MAC or key gets no answer, whatever else holds");
  in = {};
  in.inRegistry = true; in.firstKeyOpen = true; in.listLocked = true;
  check(admit(in) == Admit::NotAccepted, "R3 nothing new is stored while the list is locked");
  in.hasIdentity = true; in.sameKey = true;
  check(admit(in) == Admit::Accept, "R3 a key already stored is still confirmed while the list is locked");
  in.sameKey = false;
  check(admit(in) == Admit::NotAccepted, "R3 no key replacement while the list is locked");

  // R4
  check(sweepAllowed(RegistryLoad::Ok) && sweepAllowed(RegistryLoad::Absent),
        "R4 the boot check runs after an Ok load or with no registry file");
  check(!sweepAllowed(RegistryLoad::Quarantined), "R4 a quarantined (corrupt) registry skips the boot check");
  check(!sweepAllowed(RegistryLoad::NotRead) && !sweepAllowed(RegistryLoad::Partial),
        "R4 an unread or partly read registry skips it too");

  // R5
  const uint32_t t0 = 0xFFFFF000u;
  const uint32_t until = firstKeyUntil(t0);
  check(firstKeyOpen(until, t0 + kFirstKeyWindowMs - 1) && !firstKeyOpen(until, t0 + kFirstKeyWindowMs),
        "R5 the first-key window lasts 10 minutes, across the millis() rollover");
  check(!firstKeyOpen(0, 5), "R5 no window: closed");
  check(firstKeyFromStored(0, 1000, 5) == 0 && firstKeyFromStored(5000, 0, 5) == 0,
        "R5 a stored window is not used without a set clock");
  check(firstKeyFromStored(1000 + 60000, 1000, 5) == 60005, "R5 a restart inside the window keeps the rest of it");
  check(firstKeyFromStored(1000, 2000, 5) == 0, "R5 a stored window that ended stays ended");
  check(firstKeyFromStored(1000 + kFirstKeyWindowMs + 1, 1000, 5) == 0,
        "R5 a window longer than 10 minutes (clock moved back) is not trusted");

  std::printf("%d failure(s)\n", gFailures);
  return gFailures ? 1 : 0;
}
