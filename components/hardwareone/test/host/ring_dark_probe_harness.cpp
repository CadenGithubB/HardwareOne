// Production ringSetupServiceDarkProbe (G2_Ring.cpp) against the real R1
// codec. test_r1_protocol.py inserts the function; everything else is a double.
#include "System_R1_Protocol.h"
#include "System_Debug.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#ifdef NDEBUG
#error "Host checks require assertions"
#endif

enum RingWriteResult : uint8_t {
  RING_WRITE_OK,
  RING_WRITE_BUSY,
  RING_WRITE_DISCONNECTED,
  RING_WRITE_FAILED,
};
enum : uint8_t { G2_RING_ERR_DISCONNECTED = 7 };
struct HarnessSetupOwner { uint8_t darkProbesSent; uint32_t darkProbeLastMs; };
static HarnessSetupOwner sSetupOwner{};
static R1ProtocolProfile sR1Profile = R1_PROFILE_UNKNOWN;
static R1Encoder gR1Encoder;
static uint32_t fakeMillis = 5000;
static uint32_t millis() { return fakeMillis; }
static unsigned writes = 0, finishes = 0;
static uint8_t written[R1_MAX_FRAME];
static size_t writtenLength = 0;
static RingWriteResult ringOwnerWrite(const uint8_t* bytes, size_t length) {
  ++writes;
  memcpy(written, bytes, length);
  writtenLength = length;
  return RING_WRITE_OK;
}
static void ringFinishSetup(bool, uint8_t) { ++finishes; }

// INSERT_PROBE

int main() {
  struct Case { R1ProtocolProfile profile; uint8_t subCmd; };
  // DAILY profiles ask for an HR page; 2.2.7 keeps its legacy POINT probe. A
  // probe that builds nothing would hold ring setup (and, under the shared
  // BLE setup gate, the phone) for the whole TIME-stage deadline.
  for (const Case c : {Case{R1_PROFILE_FW_2_2_7_0005, R1_SUB_POINT},
                       Case{R1_PROFILE_FW_2_2_9_0003, R1_SUB_DAILY},
                       Case{R1_PROFILE_FW_2_3_2_0007, R1_SUB_DAILY},
                       Case{R1_PROFILE_FW_2_3_2_0008, R1_SUB_DAILY}}) {
    sSetupOwner = HarnessSetupOwner{};
    sR1Profile = c.profile;
    gR1Encoder.resetSerial();
    writes = finishes = 0;
    ringSetupServiceDarkProbe();
    R1Decoded d{};
    assert(writes == 1 && finishes == 0 && sSetupOwner.darkProbesSent == 1);
    assert(r1Decode(written, writtenLength, d) && r1DecodedIsTrusted(d));
    assert(d.module == R1_MODULE_HEALTH && d.cmd == R1_CMD_HEARTRATE &&
           d.subCmd == c.subCmd && d.statusType == R1_STATUS_TYPE_NOTIFY &&
           d.statusMethod == R1_STATUS_METHOD_GET && d.payloadLength == 0);
    // Rate limit, then the three-probe cap.
    ringSetupServiceDarkProbe();
    assert(writes == 1);
    for (int i = 0; i < 4; ++i) {
      fakeMillis += 1500;
      ringSetupServiceDarkProbe();
    }
    assert(writes == 3 && sSetupOwner.darkProbesSent == 3);
  }
  // An unknown profile builds no probe and writes nothing.
  sSetupOwner = HarnessSetupOwner{};
  sR1Profile = R1_PROFILE_UNKNOWN;
  writes = 0;
  ringSetupServiceDarkProbe();
  assert(writes == 0 && sSetupOwner.darkProbesSent == 0);
  puts("ring setup dark-clock probe: DAILY on 2.2.9/2.3.2, POINT on 2.2.7, capped");
}
