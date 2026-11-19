// The session capability word (System_ESPNow_SessionCapsCore.h, compiled as is).
//
// These cases check:
//   * the bit values and the known mask stay what 1.0 put on the air;
//   * buildCaps: the build-dependent bits (bond, sensor_send, collect) follow
//     their arguments and every other 1.0 bit is always set;
//   * capsText: names in bit order, reserved bits as "+0x...", "none" for 0,
//     a short buffer cut and NUL-terminated, the needed length returned.
#include "System_ESPNow_SessionCapsCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using namespace sessioncaps;

static bool is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

int main() {
  // Values on the air: never renumbered.
  CHECK(kBond == 0x01 && kRemoteFs == 0x02 && kFileReceive == 0x04 && kSensorSend == 0x08);
  CHECK(kCollect == 0x10 && kRelayForward == 0x20 && kTimeMaster == 0x40 && kSessionUnknown == 0x80);
  CHECK(kKnownMask == 0xFFu);
  CHECK(kBitCount == 8);
  CHECK(is(bitName(0), "bond") && is(bitName(3), "sensor_send") && is(bitName(7), "session_unknown"));
  CHECK(bitName(8) == nullptr);

  // buildCaps.
  static_assert(buildCaps(true, true, true) == kKnownMask, "every 1.0 bit");
  CHECK(!has(buildCaps(false, true, false), kBond));
  CHECK(has(buildCaps(true, false, false), kBond));
  CHECK(!has(buildCaps(true, false, false), kSensorSend));
  CHECK(has(buildCaps(false, true, false), kSensorSend));
  CHECK(!has(buildCaps(true, true, false), kCollect));
  const uint32_t always = kRemoteFs | kFileReceive | kRelayForward | kTimeMaster | kSessionUnknown;
  CHECK((buildCaps(false, false, false) & always) == always);
  CHECK(buildCaps(false, false, false) == always);
  CHECK((buildCaps(true, true, true) & ~kKnownMask) == 0);   // reserved bits are sent 0

  // capsText.
  char buf[128];
  size_t n = capsText(kBond | kSensorSend | kSessionUnknown, buf, sizeof buf);
  CHECK(is(buf, "bond,sensor_send,session_unknown") && n == std::strlen(buf));
  n = capsText(0, buf, sizeof buf);
  CHECK(is(buf, "none") && n == 4);
  n = capsText(kRemoteFs | 0x100u | 0x80000000u, buf, sizeof buf);
  CHECK(is(buf, "remote_fs,+0x80000100"));
  n = capsText(0x200u, buf, sizeof buf);
  CHECK(is(buf, "+0x200"));
  n = capsText(kKnownMask, buf, sizeof buf);
  CHECK(is(buf, "bond,remote_fs,file_receive,sensor_send,collect,relay_forward,time_master,session_unknown"));
  const size_t full = n;
  char small[8];
  std::memset(small, 'X', sizeof small);
  n = capsText(kKnownMask, small, sizeof small);
  CHECK(n == full && small[7] == '\0' && is(small, "bond,re"));
  char one[1] = {'X'};
  n = capsText(kBond, one, sizeof one);
  CHECK(one[0] == '\0' && n == 4);
  n = capsText(kBond, nullptr, 0);   // length only
  CHECK(n == 4);

  // espnowsensorreq's gate: the device itself needs sensor_send; through a
  // bond master ('via') only bond counts; an unknown or unconfirmed word
  // refuses nothing.
  using R = SensorReqRefusal;
  const uint32_t sensorsOnly = buildCaps(false, true, false), bondOnly = buildCaps(true, false, false);
  CHECK(sensorRequestRefusal(true, true, sensorsOnly, false) == R::None);
  CHECK(sensorRequestRefusal(true, true, bondOnly, false) == R::NoSensors);
  CHECK(sensorRequestRefusal(true, true, bondOnly, true) == R::None);
  CHECK(sensorRequestRefusal(true, true, sensorsOnly, true) == R::NoBond);
  CHECK(sensorRequestRefusal(true, false, bondOnly, false) == R::None);
  CHECK(sensorRequestRefusal(true, false, sensorsOnly, true) == R::None);
  CHECK(sensorRequestRefusal(false, true, 0, false) == R::None);
  CHECK(sensorRequestRefusal(false, true, 0, true) == R::None);
  CHECK(sensorRequestRefusal(true, true, 0, false) == R::NoSensors);

  std::printf("espnow_session_caps_core: %u checks passed\n", gChecks);
  return 0;
}
