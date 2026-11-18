// Rules of the battery and event frames (System_ESPNow_PowerEventCore.h, compiled as is).
//
// A device that is not the mesh master sends its battery state (POWER_STATUS,
// 24 bytes) and its system events (EVENT_PUSH, 128 bytes) to the masters named
// on it. These cases check:
//   * the two frames: every field at its stated offset, reserved bytes 0, the
//     round trip, a payload that is too short, and that both fit one frame
//     through a relay;
//   * the battery values: every flag, nothing valid, a rate that is not a
//     number or beyond 16 bits, a voltage and a percentage out of range;
//   * when a battery frame is due: at once, every interval, on a change of
//     status, charging, USB or presence, never within 5 s, never with the
//     interval 0, and what the setting may hold;
//   * the event floor for each setting and tier, and the setting's words;
//   * the token bucket: the burst, the refill, time spent full earns nothing,
//     the uptime wrap;
//   * whether a device sends, and why not, in the order of the reasons;
//   * the text of a frame: a field with no terminator, control bytes, invalid
//     and cut UTF-8, 128 bytes of 0xFF (run under ASan), and the JSON escape;
//   * the sender's cut: flags, zero padding, never inside a UTF-8 character;
//   * the event list: a free entry first, then the oldest entry of the sender
//     that holds the most; walking it by receive number; a `since` from
//     before a restart.
#include "System_ESPNow_PowerEventCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using namespace espnowsensor;

static bool is(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

static void testPowerFrame() {
  PowerFrame f = {};
  f.counter = 0x1234;
  f.bootTag = 0xA2B3;
  f.status = 3;
  f.backend = kPowerBackendAdc;
  f.known = kPowerKnownAll;
  f.voltageMv = 3912;
  f.percentX10 = 715;
  f.rateX10 = -42;
  f.sampleAgeMs = 800;
  uint8_t wire[kPowerFrameLen];
  std::memset(wire, 0xEE, sizeof(wire));
  packPowerFrame(wire, f);
  const uint8_t expected[kPowerFrameLen] = {
    0x34, 0x12,  // counter
    0xB3, 0xA2,  // boot tag
    3,           // status
    1,           // backend
    0xFF, 0x1F,  // known
    0x48, 0x0F,  // 3912 mV
    0xCB, 0x02,  // 71.5 %
    0xD6, 0xFF,  // -4.2 %/h
    0x20, 0x03,  // 800 ms
    0, 0, 0, 0, 0, 0, 0, 0,
  };
  CHECK(std::memcmp(wire, expected, sizeof(wire)) == 0);
  PowerFrame back = {};
  CHECK(parsePowerFrame(wire, sizeof(wire), back));
  CHECK(back.counter == 0x1234 && back.bootTag == 0xA2B3 && back.status == 3 && back.backend == 1 &&
        back.known == kPowerKnownAll && back.voltageMv == 3912 && back.percentX10 == 715 && back.rateX10 == -42 &&
        back.sampleAgeMs == 800);
  // Bits the frame has no meaning for are not sent.
  f.known = 0xFFFF;
  packPowerFrame(wire, f);
  CHECK(wire[6] == 0xFF && wire[7] == 0x1F);
  // Too short; a longer payload is read up to the frame's length.
  CHECK(!parsePowerFrame(wire, kPowerFrameLen - 1, back) && !parsePowerFrame(nullptr, kPowerFrameLen, back));
  uint8_t longer[kPowerFrameLen + 8];
  std::memset(longer, 0x5A, sizeof(longer));
  std::memcpy(longer, wire, sizeof(wire));
  CHECK(parsePowerFrame(longer, sizeof(longer), back) && back.voltageMv == 3912);
  CHECK(kPowerFrameLen == 24 && kPowerFrameLen <= kFrameCapRouted);

  // A snapshot with every flag set.
  PowerInputs in = {};
  in.status = 1;
  in.backend = kPowerBackendFuelGauge;
  in.hasSample = true;
  in.sampleAgeMs = 1200;
  in.voltageValid = true;
  in.voltage = 4.1234f;
  in.percentageValid = in.percentageEstimated = true;
  in.percentage = 87.26f;
  in.presenceKnown = in.present = true;
  in.chargingKnown = in.charging = in.chargingEstimated = true;
  in.usbKnown = in.usbPresent = true;
  in.rateValid = true;
  in.ratePctPerHr = 12.34f;
  in.statusEstimated = in.stale = true;
  PowerFrame g = {};
  powerFrameFill(g, in);
  CHECK(g.status == 1 && g.backend == 2 && g.known == kPowerKnownAll);
  CHECK(g.voltageMv == 4123 && g.percentX10 == 873 && g.rateX10 == 123 && g.sampleAgeMs == 1200);
  // Nothing valid: every value 0, every bit clear, no sample.
  PowerInputs none = {};
  none.voltage = 3.7f;        // left over from an earlier sample: not valid, so not sent
  none.percentage = 50.0f;
  none.ratePctPerHr = -3.0f;
  none.present = none.charging = none.usbPresent = true;  // not known, so not sent
  powerFrameFill(g, none);
  CHECK(g.known == 0 && g.voltageMv == 0 && g.percentX10 == 0 && g.rateX10 == 0 && g.sampleAgeMs == 65535);
  packPowerFrame(wire, g);
  for (size_t i = 6; i < kPowerFrameLen - 10; ++i) CHECK(wire[i] == 0);
  // A rate that is not a number, and one beyond what 16 bits hold.
  PowerInputs rate = {};
  rate.rateValid = true;
  rate.ratePctPerHr = NAN;
  powerFrameFill(g, rate);
  CHECK(!(g.known & kPowerRateValid) && g.rateX10 == 0);
  rate.ratePctPerHr = INFINITY;
  powerFrameFill(g, rate);
  CHECK(!(g.known & kPowerRateValid) && g.rateX10 == 0);
  rate.ratePctPerHr = 99999.0f;
  powerFrameFill(g, rate);
  CHECK((g.known & kPowerRateValid) && g.rateX10 == 32767);
  rate.ratePctPerHr = -99999.0f;
  powerFrameFill(g, rate);
  CHECK((g.known & kPowerRateValid) && g.rateX10 == -32768);
  rate.ratePctPerHr = -4.26f;
  powerFrameFill(g, rate);
  CHECK(g.rateX10 == -43);
  // A voltage and a percentage that are not numbers or out of range.
  PowerInputs odd = {};
  odd.voltageValid = odd.percentageValid = true;
  odd.voltage = NAN;
  odd.percentage = NAN;
  powerFrameFill(g, odd);
  CHECK(g.known == 0);
  odd.voltage = -0.5f;
  odd.percentage = 130.0f;
  powerFrameFill(g, odd);
  CHECK(!(g.known & kPowerVoltageValid) && (g.known & kPowerPercentValid) && g.percentX10 == 1000);
  odd.voltage = 70.0f;
  odd.percentage = -5.0f;
  powerFrameFill(g, odd);
  CHECK(!(g.known & kPowerVoltageValid) && g.percentX10 == 0);
  // A state that is known and "no": the known bit without the value bit.
  PowerInputs no = {};
  no.presenceKnown = no.chargingKnown = no.usbKnown = true;
  powerFrameFill(g, no);
  CHECK(g.known == (kPowerPresenceKnown | kPowerChargingKnown | kPowerUsbKnown));
  // An old sample.
  PowerInputs old = {};
  old.hasSample = true;
  old.sampleAgeMs = 70000;
  powerFrameFill(g, old);
  CHECK(g.sampleAgeMs == 65535);

  // Names: a number this firmware does not know is "unknown".
  CHECK(is(powerStatusKey(0), "unknown") && is(powerStatusKey(3), "discharging") && is(powerStatusKey(6), "not_present") &&
        is(powerStatusKey(10), "empty") && is(powerStatusKey(11), "unknown") && is(powerStatusKey(255), "unknown"));
  CHECK(is(powerBackendKey(0), "none") && is(powerBackendKey(1), "adc") && is(powerBackendKey(2), "fuelgauge") &&
        is(powerBackendKey(3), "unknown") && is(powerBackendKey(255), "unknown"));
  CHECK(is(powerStatusText(6), "no battery") && is(powerStatusText(200), "status unknown"));
}

static void testPowerSendRule() {
  PowerSendState s = {};
  PowerFrame f = {};
  f.status = 3;
  f.known = kPowerUsbKnown;
  // The first frame goes out at once.
  CHECK(powerSendDue(s, f, 60, 1000));
  powerSent(s, f, 1000);
  // Then every interval.
  CHECK(!powerSendDue(s, f, 60, 1000 + 59999));
  CHECK(powerSendDue(s, f, 60, 1000 + 60000));
  // A change of status, charging, USB or presence sends at once, but never within 5 s.
  PowerFrame changed = f;
  changed.status = 4;
  CHECK(!powerSendDue(s, changed, 60, 1000 + 4999));
  CHECK(powerSendDue(s, changed, 60, 1000 + 5000));
  for (uint16_t bit : { kPowerPresenceKnown, kPowerPresent, kPowerChargingKnown, kPowerCharging, kPowerUsbKnown,
                        kPowerUsbPresent }) {
    changed = f;
    changed.known ^= bit;
    CHECK(powerSendDue(s, changed, 60, 1000 + 5000));
    CHECK(!powerSendDue(s, changed, 60, 1000 + 4000));
  }
  // What is not a change of state does not send early: the values and the other bits.
  changed = f;
  changed.voltageMv = 4000;
  changed.percentX10 = 900;
  changed.known ^= (kPowerVoltageValid | kPowerPercentValid | kPowerRateValid | kPowerStale | kPowerStatusEstimated);
  CHECK(!powerSendDue(s, changed, 60, 1000 + 30000));
  // Interval 0: nothing, not the first frame and not a change.
  PowerSendState fresh = {};
  CHECK(!powerSendDue(fresh, f, 0, 5));
  changed = f;
  changed.status = 5;
  CHECK(!powerSendDue(s, changed, 0, 1000 + 999999));
  // The uptime wraps.
  PowerSendState late = {};
  powerSent(late, f, 0xFFFFF000u);
  CHECK(!powerSendDue(late, f, 10, 0x00000100u));
  CHECK(powerSendDue(late, f, 10, 0xFFFFF000u + 10000u));
  // What the setting may hold, and what a stored value outside it becomes.
  CHECK(powerIntervalValid(0) && powerIntervalValid(10) && powerIntervalValid(60) && powerIntervalValid(3600));
  CHECK(!powerIntervalValid(1) && !powerIntervalValid(9) && !powerIntervalValid(3601) && !powerIntervalValid(65535));
  CHECK(powerIntervalMs(0) == 0 && powerIntervalMs(5) == 10000 && powerIntervalMs(60) == 60000 &&
        powerIntervalMs(9999) == 3600000);
  CHECK(kPowerIntervalDefaultS == 60);
}

static void testEventFrame() {
  EventFrame f = {};
  f.counter = 0x0102;
  f.bootTag = 0x0304;
  f.flags = kEventCutSubject | kEventCutWho;
  f.tier = kEventTierAlert;
  f.source = kEventSourceWeb;
  f.skipped = 0x0506;
  f.eventSeq = 0x0708090A;
  f.ageMs = 0x0B0C0D0E;
  std::memset(f.kind, 'k', sizeof(f.kind));
  std::memset(f.subject, 's', sizeof(f.subject));
  std::memset(f.detail, 'd', sizeof(f.detail));
  std::memset(f.who, 'w', sizeof(f.who));
  uint8_t wire[kEventFrameLen];
  std::memset(wire, 0xEE, sizeof(wire));
  packEventFrame(wire, f);
  const uint8_t head[20] = { 0x02, 0x01, 0x04, 0x03, 0x05, 2, 3, 0, 0x06, 0x05, 0, 0,
                             0x0A, 0x09, 0x08, 0x07, 0x0E, 0x0D, 0x0C, 0x0B };
  CHECK(std::memcmp(wire, head, sizeof(head)) == 0);
  for (size_t i = 20; i < 52; ++i) CHECK(wire[i] == 'k');
  for (size_t i = 52; i < 84; ++i) CHECK(wire[i] == 's');
  for (size_t i = 84; i < 116; ++i) CHECK(wire[i] == 'd');
  for (size_t i = 116; i < 128; ++i) CHECK(wire[i] == 'w');
  EventFrame back = {};
  CHECK(parseEventFrame(wire, sizeof(wire), back));
  CHECK(back.counter == 0x0102 && back.bootTag == 0x0304 && back.flags == 0x05 && back.tier == 2 && back.source == 3 &&
        back.skipped == 0x0506 && back.eventSeq == 0x0708090A && back.ageMs == 0x0B0C0D0E);
  CHECK(std::memcmp(back.kind, f.kind, sizeof(f.kind)) == 0 && std::memcmp(back.who, f.who, sizeof(f.who)) == 0);
  CHECK(!parseEventFrame(wire, kEventFrameLen - 1, back) && !parseEventFrame(nullptr, kEventFrameLen, back));
  // Flag bits the frame has no meaning for are not sent.
  f.flags = 0xFF;
  packEventFrame(wire, f);
  CHECK(wire[4] == 0x0F && wire[7] == 0 && wire[10] == 0 && wire[11] == 0);
  CHECK(kEventFrameLen == 128 && kEventFrameLen <= kFrameCapRouted);
  CHECK(eventSkippedField(0) == 0 && eventSkippedField(65535) == 65535 && eventSkippedField(65536) == 65535 &&
        eventSkippedField(0xFFFFFFFFu) == 65535);

  // The sender's cut.
  char field[8];
  CHECK(!eventFieldPut(field, sizeof(field), "abc", 4));
  CHECK(std::memcmp(field, "abc\0\0\0\0\0", 8) == 0);            // zero padded
  CHECK(!eventFieldPut(field, sizeof(field), "12345678", 9));
  CHECK(std::memcmp(field, "12345678", 8) == 0);                 // full: no terminator
  CHECK(eventFieldPut(field, sizeof(field), "123456789", 10));
  CHECK(std::memcmp(field, "12345678", 8) == 0);                 // cut
  CHECK(!eventFieldPut(field, sizeof(field), nullptr, 0) && field[0] == 0);
  // A source buffer with no terminator is read up to its size and no further.
  const char unterminated[4] = { 'w', 'x', 'y', 'z' };
  CHECK(!eventFieldPut(field, sizeof(field), unterminated, sizeof(unterminated)));
  CHECK(std::memcmp(field, "wxyz\0\0\0\0", 8) == 0);
  // A cut never ends inside a UTF-8 character: "1234567" + a 2-byte character.
  CHECK(eventFieldPut(field, sizeof(field), "1234567\xC3\xA9x", 11));
  CHECK(std::memcmp(field, "1234567\0", 8) == 0);
  CHECK(eventFieldPut(field, sizeof(field), "123456\xE2\x82\xACx", 11));  // a 3-byte character at 6
  CHECK(std::memcmp(field, "123456\0\0", 8) == 0);
  CHECK(eventFieldPut(field, sizeof(field), "12345\xE2\x82\xACx", 10));   // it fits exactly
  CHECK(std::memcmp(field, "12345\xE2\x82\xAC", 8) == 0);

  // Names.
  CHECK(is(eventTierKey(0), "verbose") && is(eventTierKey(1), "standard") && is(eventTierKey(2), "alert") &&
        is(eventTierKey(3), "unknown") && is(eventTierKey(255), "unknown"));
  CHECK(is(eventSourceKey(0), "unknown") && is(eventSourceKey(1), "cli") && is(eventSourceKey(3), "web") &&
        is(eventSourceKey(6), "system") && is(eventSourceKey(7), "g2") && is(eventSourceKey(8), "unknown") &&
        is(eventSourceKey(255), "unknown"));
}

static void testFloor() {
  // off: nothing. alert: alerts. standard: standard and alerts. verbose: all.
  for (uint8_t tier = 0; tier <= 2; ++tier) CHECK(!eventPassesFloor(kEventPushOff, tier));
  CHECK(!eventPassesFloor(kEventPushAlert, kEventTierVerbose) && !eventPassesFloor(kEventPushAlert, kEventTierStandard) &&
        eventPassesFloor(kEventPushAlert, kEventTierAlert));
  CHECK(!eventPassesFloor(kEventPushStandard, kEventTierVerbose) && eventPassesFloor(kEventPushStandard, kEventTierStandard) &&
        eventPassesFloor(kEventPushStandard, kEventTierAlert));
  for (uint8_t tier = 0; tier <= 2; ++tier) CHECK(eventPassesFloor(kEventPushVerbose, tier));
  // A stored value this firmware does not know sends nothing.
  for (uint8_t tier = 0; tier <= 2; ++tier) CHECK(!eventPassesFloor(4, tier) && !eventPassesFloor(255, tier));
  uint8_t setting = 99;
  CHECK(eventPushFromName("off", setting) && setting == kEventPushOff);
  CHECK(eventPushFromName("alert", setting) && setting == kEventPushAlert);
  CHECK(eventPushFromName("standard", setting) && setting == kEventPushStandard);
  CHECK(eventPushFromName("verbose", setting) && setting == kEventPushVerbose);
  setting = 99;
  CHECK(!eventPushFromName("loud", setting) && !eventPushFromName("", setting) && !eventPushFromName(nullptr, setting) &&
        setting == 99);
  CHECK(is(eventPushKey(1), "alert") && is(eventPushKey(0), "off") && is(eventPushKey(77), "off"));
}

static void testBucket() {
  TokenBucket b = {};
  uint32_t now = 5000;
  // Eight at once, the ninth waits.
  for (int i = 0; i < 8; ++i) CHECK(bucketTake(b, kEventTxBurst, kEventTxRefillMs, now));
  CHECK(!bucketTake(b, kEventTxBurst, kEventTxRefillMs, now));
  CHECK(!bucketHas(b, kEventTxBurst, kEventTxRefillMs, now + 1999));
  CHECK(bucketHas(b, kEventTxBurst, kEventTxRefillMs, now + 2000));
  CHECK(bucketTake(b, kEventTxBurst, kEventTxRefillMs, now + 2000));
  CHECK(!bucketTake(b, kEventTxBurst, kEventTxRefillMs, now + 2000));
  // One every 2 s: after 5 s two more (and the odd second is not lost).
  CHECK(bucketTake(b, kEventTxBurst, kEventTxRefillMs, now + 7000));
  CHECK(bucketTake(b, kEventTxBurst, kEventTxRefillMs, now + 7000));
  CHECK(!bucketTake(b, kEventTxBurst, kEventTxRefillMs, now + 7000));
  CHECK(bucketTake(b, kEventTxBurst, kEventTxRefillMs, now + 8000));
  // A long quiet time fills it to the burst and no further.
  now += 1000000;
  int taken = 0;
  while (bucketTake(b, kEventTxBurst, kEventTxRefillMs, now)) ++taken;
  CHECK(taken == 8);
  // Time spent full earns nothing: full at t, one taken at t + 1 h, the next
  // token comes 2 s after that, not at once.
  TokenBucket c = {};
  CHECK(bucketHas(c, 2, 1000, 100));
  CHECK(bucketTake(c, 2, 1000, 100 + 3600000));
  CHECK(bucketTake(c, 2, 1000, 100 + 3600000));
  CHECK(!bucketTake(c, 2, 1000, 100 + 3600000 + 999));
  CHECK(bucketTake(c, 2, 1000, 100 + 3600000 + 1000));
  // The uptime wraps.
  TokenBucket w = {};
  for (int i = 0; i < 16; ++i) CHECK(bucketTake(w, kEventRxBurst, kEventRxRefillMs, 0xFFFFFF00u));
  CHECK(!bucketTake(w, kEventRxBurst, kEventRxRefillMs, 0xFFFFFF00u + 999u));
  CHECK(bucketTake(w, kEventRxBurst, kEventRxRefillMs, 0x00000300u));
  // A state with more tokens than the burst (memory that was not this bucket's) is pulled back.
  TokenBucket odd = {};
  odd.primed = true;
  odd.tokens = 60000;
  odd.lastRefillMs = 0;
  bucketRefill(odd, 16, 1000, 10);
  CHECK(odd.tokens == 16);
}

static void testPushDecide() {
  PushInputs in = {};
  in.inBuild = true;
  in.settingOn = true;
  in.meshOn = true;
  in.meshRole = kRoleWorker;
  in.masterNamed = true;
  in.targets = 2;
  in.targetsReady = 1;
  CHECK(pushDecide(in) == PushWhyNot::Sends);
  in.meshRole = kRoleBackup;  // a backup master that has not taken over sends its own
  CHECK(pushDecide(in) == PushWhyNot::Sends);
  // The reasons, the first that holds.
  PushInputs all = {};  // nothing holds
  all.meshRole = kRoleMaster;
  CHECK(pushDecide(all) == PushWhyNot::NotInBuild);
  all.inBuild = true;
  CHECK(pushDecide(all) == PushWhyNot::Off);
  all.settingOn = true;
  CHECK(pushDecide(all) == PushWhyNot::MeshOff);
  all.meshOn = true;
  CHECK(pushDecide(all) == PushWhyNot::IsMaster);
  all.meshRole = kRoleWorker;
  CHECK(pushDecide(all) == PushWhyNot::NoMaster);
  all.masterNamed = true;
  CHECK(pushDecide(all) == PushWhyNot::MasterNotPaired);
  all.targets = 1;
  CHECK(pushDecide(all) == PushWhyNot::NoSession);
  all.targetsReady = 1;
  CHECK(pushDecide(all) == PushWhyNot::Sends);
  CHECK(pushWhyNotKey(PushWhyNot::Sends) == nullptr);
  for (uint8_t i = 1; i < (uint8_t)PushWhyNot::Count; ++i) {
    const PushWhyNot why = (PushWhyNot)i;
    CHECK(pushWhyNotKey(why) && !is(pushWhyNotKey(why), "?") && !is(pushWhyNotText(why), "?"));
    for (uint8_t k = 1; k < i; ++k) CHECK(!is(pushWhyNotKey(why), pushWhyNotKey((PushWhyNot)k)));
  }
  CHECK(is(pushWhyNotKey(PushWhyNot::NoMaster), "no_master") && is(pushWhyNotKey(PushWhyNot::IsMaster), "is_master"));
}

static std::string copied(const std::string& field, size_t fieldLen) {
  // A buffer of exactly the field plus one byte: a longer copy is an ASan error.
  std::vector<char> in(fieldLen, '\0');
  std::memcpy(in.data(), field.data(), field.size() < fieldLen ? field.size() : fieldLen);
  std::vector<char> out(fieldLen + 1, 'X');
  const size_t n = wireTextCopy(in.data(), fieldLen, out.data(), out.size());
  CHECK(out[n] == '\0' && n <= fieldLen && std::strlen(out.data()) == n);
  return std::string(out.data());
}

static void testWireText() {
  CHECK(copied("login_fail", 32) == "login_fail");
  CHECK(copied("", 32) == "");
  // A full field has no terminator: the receiver ends the copy itself.
  CHECK(copied(std::string(32, 'a'), 32) == std::string(32, 'a'));
  // The copy ends at the first NUL; what follows is not text.
  CHECK(copied(std::string("abc\0def", 7), 32) == "abc");
  // Control bytes and 0x7F.
  CHECK(copied("a\nb\tc\x01" "d\x7F" "e", 32) == "a?b?c?d?e");
  // Quotes and backslashes are text here (the JSON escape deals with them).
  CHECK(copied("say \"hi\" \\ now", 32) == "say \"hi\" \\ now");
  // Well-formed UTF-8 is kept: 2, 3 and 4 bytes.
  CHECK(copied("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80", 32) == "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
  // Not well formed: a lone continuation byte, a lead without its
  // continuation, an overlong form, a surrogate, a value over U+10FFFF, 0xFF.
  CHECK(copied("a\x80z", 32) == "a?z");
  CHECK(copied("a\xC3z", 32) == "a?z");
  CHECK(copied("a\xC0\x80z", 32) == "a??z");
  CHECK(copied("a\xE0\x80\x80z", 32) == "a???z");
  CHECK(copied("a\xED\xA0\x80z", 32) == "a???z");
  CHECK(copied("a\xF4\x90\x80\x80z", 32) == "a????z");
  CHECK(copied("a\xFFz", 32) == "a?z");
  // A character cut by the end of the field.
  CHECK(copied("1234567\xC3", 8) == "1234567?");
  CHECK(copied("123456\xE2\x82", 8) == "123456??");
  CHECK(copied("123456\xC3\xA9", 8) == "123456\xC3\xA9");
  // 0xFF everywhere: every byte becomes '?', and the copy is terminated.
  CHECK(copied(std::string(32, '\xFF'), 32) == std::string(32, '?'));
  CHECK(copied(std::string(12, '\xFF'), 12) == std::string(12, '?'));
  // A copy buffer smaller than the field takes what fits, and never half a character.
  char small[6];
  CHECK(wireTextCopy("abcdefgh", 8, small, sizeof(small)) == 5 && is(small, "abcde"));
  CHECK(wireTextCopy("abcd\xC3\xA9", 6, small, sizeof(small)) == 5 && is(small, "abcd?"));
  CHECK(wireTextCopy(nullptr, 8, small, sizeof(small)) == 0 && small[0] == '\0');
  CHECK(wireTextCopy("abc", 3, small, 0) == 0);

  // The JSON escape.
  char json[16];
  CHECK(jsonTextEscape("a\"b\\c", json, sizeof(json)) == 7 && is(json, "a\\\"b\\\\c"));
  CHECK(jsonTextEscape("x\ny", json, sizeof(json)) == 3 && is(json, "x?y"));
  CHECK(jsonTextEscape(nullptr, json, sizeof(json)) == 0 && json[0] == '\0');
  // It never ends inside an escape: 7 characters and a quote do not fit 9 bytes.
  char tight[9];
  CHECK(jsonTextEscape("1234567\"", tight, sizeof(tight)) == 7 && is(tight, "1234567"));
  CHECK(jsonTextEscape("123456\"", tight, sizeof(tight)) == 8 && is(tight, "123456\\\""));
  // The worst case of a field fits twice its size.
  char wide[2 * 33];
  const std::string quotes(32, '"');
  CHECK(jsonTextEscape(quotes.c_str(), wide, sizeof(wide)) == 64);

  // A whole frame of 0xFF: parsed, and every text field comes out as clean text.
  uint8_t hostile[kEventFrameLen];
  std::memset(hostile, 0xFF, sizeof(hostile));
  EventFrame f = {};
  CHECK(parseEventFrame(hostile, sizeof(hostile), f));
  char kind[kEventKindLen + 1], subject[kEventSubjectLen + 1], detail[kEventDetailLen + 1], who[kEventWhoLen + 1];
  CHECK(wireTextCopy(f.kind, sizeof(f.kind), kind, sizeof(kind)) == 32 && is(kind, std::string(32, '?').c_str()));
  CHECK(wireTextCopy(f.subject, sizeof(f.subject), subject, sizeof(subject)) == 32);
  CHECK(wireTextCopy(f.detail, sizeof(f.detail), detail, sizeof(detail)) == 32);
  CHECK(wireTextCopy(f.who, sizeof(f.who), who, sizeof(who)) == 12 && is(who, "????????????"));
  CHECK(is(eventTierKey(f.tier), "unknown") && is(eventSourceKey(f.source), "unknown"));
  PowerFrame p = {};
  CHECK(parsePowerFrame(hostile, sizeof(hostile), p));
  CHECK(is(powerStatusKey(p.status), "unknown") && is(powerBackendKey(p.backend), "unknown"));
}

static StoredEvent ev(uint32_t rx, uint8_t sender) {
  StoredEvent e = {};
  e.rx = rx;
  e.senderMac[5] = sender;
  return e;
}

static void testEventList() {
  StoredEvent list[kEventListLen] = {};
  // Free entries first, in order.
  for (uint8_t i = 0; i < kEventListLen; ++i) {
    CHECK(eventListSlot(list, kEventListLen) == i);
    list[i] = ev(i + 1, 1);
  }
  // Full of one sender: the 25th replaces the first.
  CHECK(eventListSlot(list, kEventListLen) == 0);
  list[0] = ev(25, 1);
  CHECK(eventListSlot(list, kEventListLen) == 1);  // then the second, which is now the oldest
  // Another sender's event takes from the sender that holds the most, not from itself.
  list[1] = ev(26, 2);
  CHECK(eventListSlot(list, kEventListLen) == 2);  // sender 1 still holds 23, its oldest is rx 3
  // Sender 2 sends a burst: it takes sender 1's entries until both hold 12,
  // then (as the one whose oldest entry is older) sender 1 gives one more,
  // and from then on sender 2 replaces its own oldest.
  uint32_t rx = 27;
  for (int n = 0; n < 40; ++n) {
    const uint8_t slot = eventListSlot(list, kEventListLen);
    list[slot] = ev(rx++, 2);
  }
  unsigned one = 0, two = 0;
  for (const StoredEvent& e : list) {
    if (e.senderMac[5] == 1) ++one;
    else ++two;
  }
  CHECK(one + two == kEventListLen && one >= 11 && one <= 12);  // one busy sender does not push the other out
  // Walking by receive number: oldest first across two lists, none twice.
  StoredEvent other[kEventListLen] = {};
  other[3] = ev(1000, 7);
  other[9] = ev(5, 7);  // older than what the first list holds now
  const StoredEvent* lists[kEventMeshes] = { list, nullptr, other, nullptr };
  uint32_t last = 0;
  unsigned walked = 0;
  for (const StoredEvent* e = eventListNext(lists, kEventMeshes, kEventListLen, 0); e;
       e = eventListNext(lists, kEventMeshes, kEventListLen, e->rx)) {
    CHECK(e->rx > last);
    last = e->rx;
    ++walked;
  }
  CHECK(walked == kEventListLen + 2 && last == 1000);
  CHECK(eventListNext(lists, kEventMeshes, kEventListLen, 1000) == nullptr);
  CHECK(eventListNext(lists, kEventMeshes, kEventListLen, 999)->rx == 1000);
  const StoredEvent* none[kEventMeshes] = { nullptr, nullptr, nullptr, nullptr };
  CHECK(eventListNext(none, kEventMeshes, kEventListLen, 0) == nullptr);
  // A `since` above the newest is from before a restart: read as 0.
  CHECK(eventSinceFor(0, 40) == 0 && eventSinceFor(40, 40) == 40 && eventSinceFor(41, 40) == 0 &&
        eventSinceFor(7, 0) == 0);
  // The oldest event a ring of 48 still holds: #1 until it has wrapped, then
  // the 48th from the newest.
  CHECK(eventOldestHeld(0, 48) == 1 && eventOldestHeld(1, 48) == 1 && eventOldestHeld(47, 48) == 1 &&
        eventOldestHeld(48, 48) == 1 && eventOldestHeld(49, 48) == 2 && eventOldestHeld(100, 48) == 53);
  CHECK(kEventListLen == 24 && kEventMeshes == 4);
}

int main() {
  testPowerFrame();
  testPowerSendRule();
  testEventFrame();
  testFloor();
  testBucket();
  testPushDecide();
  testWireText();
  testEventList();
  std::printf("espnow_power_event_core_tests: %u checks passed\n", gChecks);
  return 0;
}
