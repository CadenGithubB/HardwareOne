// Production ringResolveAdvIdentity (G2_Ring.cpp) and macParse
// (System_Utils.h) feeding the real advStart encoder. test_r1_protocol.py
// inserts both functions; the addresses are synthetic.
#include "System_R1_Protocol.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#ifdef NDEBUG
#error "Host checks require assertions"
#endif

// INSERT_MACPARSE

// INSERT_RESOLVE

static const uint8_t kRight[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01};
static const uint8_t kLeft[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x02};
static const uint8_t kOtherPair[6] = {0x02, 0x99, 0x88, 0x77, 0x66, 0x01};
static const char* const kSavedRight = "02:11:22:33:44:01";
static const char* const kSavedLeft = "02:11:22:33:44:02";

struct Resolved {
  bool ok;
  bool usedSaved;
  uint8_t right[6];
  uint8_t left[6];
};

// nullptr live address = that temple is not connected.
static Resolved resolve(const uint8_t* liveRight, const uint8_t* liveLeft,
                        const char* savedRight, const char* savedLeft) {
  static const uint8_t none[6] = {};
  Resolved out{};
  out.usedSaved = true;  // the resolver must clear it
  out.ok = ringResolveAdvIdentity(liveRight != nullptr, liveRight ? liveRight : none,
                                  liveLeft != nullptr, liveLeft ? liveLeft : none,
                                  savedRight, savedLeft, out.right, out.left,
                                  &out.usedSaved);
  return out;
}

static void expectPair(const Resolved& r, bool usedSaved) {
  assert(r.ok && r.usedSaved == usedSaved);
  assert(memcmp(r.right, kRight, 6) == 0 && memcmp(r.left, kLeft, 6) == 0);
}

int main() {
  // Both temples connected: the saved pair is not consulted, so a missing or
  // different saved pair changes nothing.
  expectPair(resolve(kRight, kLeft, "", ""), false);
  expectPair(resolve(kRight, kLeft, "02:99:88:77:66:01", "02:99:88:77:66:02"), false);
  expectPair(resolve(kRight, kLeft, kSavedRight, kSavedLeft), false);

  // A temple that is down takes its saved address.
  expectPair(resolve(kRight, nullptr, kSavedRight, kSavedLeft), true);
  expectPair(resolve(nullptr, kLeft, kSavedRight, kSavedLeft), true);
  expectPair(resolve(nullptr, nullptr, kSavedRight, kSavedLeft), true);
  // The registry's text form is parsed leniently, as macParse documents.
  expectPair(resolve(kRight, nullptr, "02-11-22-33-44-01", "021122334402"), true);

  // Fail closed: nothing to vouch for the missing temple.
  assert(!resolve(kRight, nullptr, kSavedRight, "").ok);
  assert(!resolve(kRight, nullptr, kSavedRight, nullptr).ok);
  assert(!resolve(kRight, nullptr, kSavedRight, "00:00:00:00:00:00").ok);
  assert(!resolve(kRight, nullptr, kSavedRight, "02:11:22:33:44").ok);
  assert(!resolve(kRight, nullptr, kSavedRight, "02:11:22:33:44:zz").ok);
  // A partly parsed address is never sent for the temple that is down.
  assert(!resolve(nullptr, kLeft, "02:11:22:33:44:zz", kSavedLeft).ok);
  assert(!resolve(nullptr, nullptr, "", "").ok);
  // A half-saved pair cannot vouch for the other half.
  assert(!resolve(kRight, nullptr, "", kSavedLeft).ok);
  assert(!resolve(nullptr, kLeft, kSavedRight, "").ok);
  // A connected temple from a different pair never mixes with the saved half.
  assert(!resolve(kOtherPair, nullptr, kSavedRight, kSavedLeft).ok);
  assert(!resolve(nullptr, kOtherPair, kSavedRight, kSavedLeft).ok);
  {
    Resolved refused = resolve(kOtherPair, nullptr, kSavedRight, kSavedLeft);
    assert(!refused.usedSaved);
  }

  // The resolved pair reaches the wire as advStart: reversed right, then
  // reversed left, on both 2.3.2 profiles.
  for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_3_2_0007, R1_PROFILE_FW_2_3_2_0008}) {
    const Resolved r = resolve(kRight, nullptr, kSavedRight, kSavedLeft);
    R1Encoder encoder;
    const R1Frame frame = encoder.buildAdvStart(profile, r.right, r.left);
    R1Decoded d{};
    assert(frame.length > 0 && r1Decode(frame.bytes, frame.length, d) &&
           r1DecodedIsTrusted(d));
    assert(d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM &&
           d.subCmd == R1_SUB_ADV_START && d.payloadLength == 12);
    for (size_t i = 0; i < 6; ++i) {
      assert(d.payload[i] == kRight[5 - i]);
      assert(d.payload[6 + i] == kLeft[5 - i]);
    }
  }

  std::puts("ring advStart identity: ok");
  return 0;
}
