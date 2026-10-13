#include <assert.h>
#include <stdio.h>
#include <vector>

using Bytes = std::vector<uint8_t>;

static Bytes joined(Bytes first, const Bytes& second) {
  first.insert(first.end(), second.begin(), second.end());
  return first;
}

static const Bytes kWearOffAck = {0x08,0x01,0x10,0xd1,0x01,0x1a,0x04,0x2a,0x02,0x08,0x00};
static const Bytes kWearOnAck  = {0x08,0x01,0x10,0xd1,0x01,0x1a,0x04,0x2a,0x02,0x08,0x01};
static const Bytes kBasicRead  = {0x08,0x02,0x10,0xd0,0x01,0x22,0x02,0x08,0x01};
// Synthetic schema fixture, not a claim of hardware qualification: both
// independently reported firmware strings, explicit wear-off, and battery.
static const Bytes kBasicState = {
  0x08,0x02,0x10,0xd0,0x01,0x22,0x1a,
  0x08,0x01,0x2a,0x08,'2','.','3','.','2','.','1','4',
  0x32,0x08,'2','.','3','.','2','.','1','4',0x50,0x00,0x60,0x18
};

static void rejectedEcho(const Bytes& input) {
  G2SettingsEcho echo;
  memset(&echo, 0xa5, sizeof(echo));
  uint32_t magic = 99, command = 99;
  assert(!g2ParseSettingEcho(input.data(), input.size(), &echo, &magic, &command));
  assert(echo.have == 0 && echo.wearDetect == 0 && magic == 0 && command == 0);
}

static void rejectedAck(const Bytes& input) {
  uint32_t magic = 99, value = 99;
  uint8_t inner = 99, leaf = 99;
  assert(!g2ParseSettingWriteAck(input.data(), input.size(), &magic, &inner, &leaf, &value));
  assert(magic == 0 && inner == 0 && leaf == 0 && value == 0);
}

static void checkBuilder(size_t size, const uint8_t* envelope, const Bytes& expected) {
  G2EnvelopeView parsed{};
  assert(size && g2ParseEnvelope(envelope, size, &parsed));
  assert(parsed.seq == 42 && parsed.sid == G2_SID_SETTINGS && parsed.flag == G2_FLAG_REQUEST);
  assert(parsed.totalFrags == 1 && parsed.fragIdx == 1);
  assert(parsed.payloadLen == expected.size());
  assert(memcmp(parsed.payload, expected.data(), expected.size()) == 0);
}

int main() {
  assert(g2ProtocolGoldenSelfTest());
  uint8_t wire[64];
  checkBuilder(g2BuildSettingBasicRequest(42, 208, wire, sizeof(wire)), wire, kBasicRead);
  for (uint32_t value : {0u, 1u}) {
    const Bytes& expected = value ? kWearOnAck : kWearOffAck;
    checkBuilder(g2BuildSettingInfoWrite(42, 209, G2_SETW_F_WEAR_DETECT,
        G2_SETW_WEAR_F_SWITCH, value, wire, sizeof(wire)), wire, expected);
    uint32_t magic = 0, actual = 9;
    uint8_t inner = 0, leaf = 0;
    assert(g2ParseSettingWriteAck(expected.data(), expected.size(), &magic, &inner, &leaf, &actual));
    assert(magic == 209 && inner == 5 && leaf == 1 && actual == value);
  }
  // Public builder limits and exact output bytes remain unchanged.
  for (uint8_t field : {uint8_t(7), uint8_t(8), uint8_t(10), uint8_t(11)})
    assert(!g2BuildSettingInfoWrite(42, 209, field, 1, 0, wire, sizeof(wire)));
  assert(!g2BuildSettingInfoWrite(42, 209, 5, 1, 0, wire, 4));
  checkBuilder(g2BuildHeadUpSwitch(42, 209, false, wire, sizeof(wire)), wire,
      {0x08,0x01,0x10,0xd1,0x01,0x1a,0x04,0x22,0x02,0x08,0x00});

  G2SettingsEcho echo{};
  uint32_t magic = 0, command = 0;
  assert(g2ParseSettingEcho(kBasicState.data(), kBasicState.size(), &echo, &magic, &command));
  assert(command == 2 && magic == 208 && echo.settingInfoType == 1);
  assert(G2_ECHO_HAS(echo, 10) && echo.wearDetect == 0 && echo.battery == 24);
  G2SettingVersions versions{};
  assert(g2ParseSettingVersions(kBasicState.data(), kBasicState.size(), &versions));
  assert(versions.hasLeft && versions.hasRight);
  assert(strcmp(versions.left, "2.3.2.14") == 0 && strcmp(versions.right, "2.3.2.14") == 0);
  // Exact profile admission must not accept a truncated string or a C-string
  // prefix that hides additional bytes after an embedded NUL.
  for (uint8_t field : {uint8_t(0x2a), uint8_t(0x32)}) {
    for (const Bytes& badVersion : {Bytes{'2','.','3','.','2','.','1','4',0,'x'},
                                   Bytes(32, 'x')}) {
      Bytes replacement = {0x22, uint8_t(badVersion.size() + 2), field,
                           uint8_t(badVersion.size())};
      replacement.insert(replacement.end(), badVersion.begin(), badVersion.end());
      const Bytes badState = joined(kBasicState, replacement);
      memset(&versions, 0xa5, sizeof(versions));
      assert(!g2ParseSettingVersions(badState.data(), badState.size(), &versions));
      assert(!versions.hasLeft && !versions.hasRight && versions.left[0] == 0 && versions.right[0] == 0);
    }
  }
  Bytes noWear = kBasicState;
  noWear.erase(noWear.end() - 4, noWear.end() - 2); noWear[6] -= 2;
  assert(g2ParseSettingEcho(noWear.data(), noWear.size(), &echo, &magic, &command));
  assert(!G2_ECHO_HAS(echo, 10) && echo.wearDetect == 0);

  // Correlated readback cannot use f4 if another known body is present,
  // even with exact pair versions and an omitted default wear scalar. Reject
  // either ordering and either requested correlation output; preserve the
  // historical three-argument accessor outside transaction qualification.
  for (uint8_t tag : {uint8_t(0x1a), uint8_t(0x2a), uint8_t(0x32), uint8_t(0x3a)}) {
    for (const Bytes& competing : {joined(noWear, {tag, 0x00}),
                                   joined({tag, 0x00}, noWear)}) {
      rejectedEcho(competing);
      magic = 99;
      assert(!g2ParseSettingEcho(competing.data(), competing.size(), &echo, &magic));
      assert(magic == 0 && echo.have == 0);
      command = 99;
      assert(!g2ParseSettingEcho(competing.data(), competing.size(), &echo, nullptr, &command));
      assert(command == 0 && echo.have == 0);
      assert(g2ParseSettingEcho(competing.data(), competing.size(), &echo));
      assert(echo.settingInfoType == 1 && !G2_ECHO_HAS(echo, 10));
    }
  }

  // Repeated f4 singular messages merge like the production version parser;
  // repeated scalar fields use their last value. The wrapper may be reordered.
  const Bytes merged = {0x22,0x04,0x50,0x00,0x10,0x64,0x10,0x00,
                        0x22,0x04,0x50,0x00,0x50,0x01,0x08,0x02};
  assert(g2ParseSettingEcho(merged.data(), merged.size(), &echo, &magic, &command));
  assert(magic == 0 && command == 2 && echo.wearDetect == 1 && echo.brightness == 100);
  const Bytes bodyOnly = {0x22,0x02,0x50,0x00};
  assert(g2ParseSettingEcho(bodyOnly.data(), bodyOnly.size(), &echo));
  rejectedEcho(bodyOnly); // correlation outputs require the corresponding fields
  const Bytes unknown = joined(kBasicState, {0xa0,0x01,0x01,0xaa,0x01,0x01,0xff});
  assert(g2ParseSettingEcho(unknown.data(), unknown.size(), &echo, &magic, &command));
  // Numeric echo saturation was pre-existing; no new firmware range policy.
  const Bytes saturated = joined(kBasicState, {0x22,0x03,0x10,0x80,0x02});
  assert(g2ParseSettingEcho(saturated.data(), saturated.size(), &echo, &magic, &command));
  assert(echo.brightness == 255);

  // Every truncated valid frame must fail without exposing a parsed prefix.
  for (size_t end = 0; end < kBasicState.size(); ++end)
    rejectedEcho(Bytes(kBasicState.begin(), kBasicState.begin() + end));
  for (size_t end = 0; end < kWearOffAck.size(); ++end)
    rejectedAck(Bytes(kWearOffAck.begin(), kWearOffAck.begin() + end));
  for (const Bytes& suffix : {Bytes{0xff}, Bytes{0x00}, Bytes{0x0f},
       Bytes{0xa2,0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x01},
       Bytes{0xa0,0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x02}}) {
    rejectedEcho(joined(kBasicState, suffix));
    rejectedAck(joined(kWearOffAck, suffix));
  }
  Bytes malformedBody = kBasicState;
  ++malformedBody[6]; malformedBody.push_back(0xff);
  rejectedEcho(malformedBody);
  rejectedEcho(joined(kBasicState, {0x22,0x03,0x52,0x01,0x00})); // numeric field wrong wire
  rejectedEcho(joined(kBasicState, {0x22,0x02,0x28,0x00})); // version wrong wire
  rejectedEcho(joined(kBasicState, {0x20,0x00})); // wrapper-f4 wrong wire
  rejectedEcho(joined(kBasicState, {0x08,0x02})); // duplicate command
  rejectedEcho(joined(kBasicState, {0x10,0xd0,0x01})); // duplicate magic
  rejectedEcho(joined(kBasicState, {0x22,0x0b,0x50,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x02}));
  rejectedEcho({0x08,0x02,0x10,0xd0,0x01,0x22,0x00}); // no reported numeric state
  rejectedEcho(kWearOffAck); // write body is not reported state

  // ACKs are unambiguous mirrors: command+magic+one f3 containing exactly
  // one setting sub-message and one varint. Do not take the first of many.
  rejectedAck(joined(kWearOffAck, {0x08,0x01}));
  rejectedAck(joined(kWearOffAck, {0x10,0xd1,0x01}));
  rejectedAck(joined(kWearOffAck, {0x1a,0x04,0x2a,0x02,0x08,0x00}));
  rejectedAck(joined(kWearOffAck, {0x22,0x02,0x50,0x00}));
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x08,0x2a,0x02,0x08,0x00,0x2a,0x02,0x08,0x01});
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x06,0x2a,0x04,0x08,0x00,0x10,0x01});
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x00});
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x02,0x0a,0x00}); // empty multi-leaf brightness
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x02,0x22,0x00}); // empty multi-leaf HeadUp
  // Static-derived fixture from the stock nanopb descriptor, not a captured
  // ACK: the single wear scalar default-elides. Its empty, present sub-message
  // confirms OFF; the absent f3/body above does not.
  const Bytes elidedOff = {0x08,0x01,0x10,0xd1,0x01,0x1a,0x02,0x2a,0x00};
  uint8_t elidedInner = 0, elidedLeaf = 0;
  uint32_t elidedValue = 99;
  assert(g2ParseSettingWriteAck(elidedOff.data(), elidedOff.size(), &magic,
      &elidedInner, &elidedLeaf, &elidedValue));
  assert(magic == 209 && elidedInner == 5 && elidedLeaf == 1 && elidedValue == 0);
  for (size_t end = 0; end < elidedOff.size(); ++end)
    rejectedAck(Bytes(elidedOff.begin(), elidedOff.begin() + end));
  rejectedAck(joined(elidedOff, {0xff}));
  rejectedAck(joined(elidedOff, {0x1a,0x02,0x2a,0x00}));
  rejectedAck({0x08,0x01,0x1a,0x04,0x2a,0x02,0x08,0x00}); // absent magic != explicit zero
  rejectedAck({0x10,0xd1,0x01,0x1a,0x04,0x2a,0x02,0x08,0x00});
  rejectedAck({0x08,0x02,0x10,0xd1,0x01,0x1a,0x04,0x2a,0x02,0x08,0x00});
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x05,0x2a,0x03,0x08,0x00,0xff});
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x06,0x82,0x10,0x03,0x08,0x00,0x00});
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x05,0x2a,0x03,0x80,0x10,0x00});
  rejectedAck({0x08,0x01,0x10,0xd1,0x01,0x1a,0x08,0x2a,0x06,0x08,0x80,0x80,0x80,0x80,0x10});
  // The codec reports a valid but different value; the transaction owner is
  // responsible for comparing it to what was requested.
  const Bytes zeroMagicAck = {0x1a,0x04,0x2a,0x02,0x08,0x01,0x10,0x00,0x08,0x01};
  uint32_t value = 0;
  assert(g2ParseSettingWriteAck(zeroMagicAck.data(), zeroMagicAck.size(), &magic, nullptr, nullptr, &value));
  assert(magic == 0 && value == 1);
  assert(!g2ParseSettingEcho(nullptr, 0, &echo, &magic, &command));
  assert(!g2ParseSettingWriteAck(nullptr, 0, &magic, nullptr, nullptr, &value));
  puts("G2 settings: golden codecs, exact read/write bytes, presence, strict ACKs, full-frame bounds and truncation passed");
}
