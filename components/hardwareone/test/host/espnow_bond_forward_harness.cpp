// Host harness for the bond master's side of forwarding: the production code
// of System_ESPNow_BondForward.cpp between BOND_FORWARD_HOST_BEGIN and
// BOND_FORWARD_HOST_END (the tick, the answers and queries, the forward calls
// with the partner's sensor status, the mesh master's sensor requests for the
// partner and their leases, 'bondforward', the status lines and JSON),
// compiled as it is with the
// production rules headers System_ESPNow_ForwardCore.h,
// System_ESPNow_SensorCore.h and System_ESPNow_PowerEventCore.h and the
// production WarnLimiter.
//
// Host boundaries, all in this file: the bond (mode, partner, role, its key,
// the agreement, the hold epoch), the settings, the mesh role, the named
// masters and which mesh they are paired in, the sessions (with their session
// ids), the radio (a recording queue that can refuse), the bond-link stream
// control of the partner's sensors, the console, the reply buffer and the lock.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "System_ESPNow_ForwardCore.h"
#include "System_ESPNow_HandshakeBackoff.h"
#include "System_ESPNow_PowerEventCore.h"
#include "System_ESPNow_SensorCore.h"
#include "System_MacText.h"

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  const char* c_str() const { return text_.c_str(); }
 private:
  std::string text_;
};

#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR()

static int gLockDepth = 0;
static unsigned gLockViolations = 0;
#define BF_LOCK()   (++gLockDepth)
#define BF_UNLOCK() (--gLockDepth)
static void notUnderLock() {
  if (gLockDepth != 0) ++gLockViolations;
}

// INSERT_TARGETS_STRUCT

// INSERT_BOND_STRUCT

// ---- The device -------------------------------------------------------------------------------
static uint32_t gNowMs = 5000;
static BfBond gBond = {};
static bool gHeld = false;
static bool gSetting = false;
static uint16_t gMask = espnowfwd::kSensorMaskDefault;
static bool gMeshOn = true;
static uint8_t gRole = 0;
static SensorReceiverTargets gTargets = {};
struct TestSession {
  uint8_t mac[6];
  uint16_t id;          // 0: none
  uint32_t upAtMs;
  bool routed;
};
static std::vector<TestSession> gSessions;
static TestSession* sessionOf(const uint8_t* mac) {
  for (auto& s : gSessions) {
    if (std::memcmp(s.mac, mac, 6) == 0) return &s;
  }
  return nullptr;
}
struct Sent {
  uint8_t kind;   // 155, 154, 152, 132
  uint8_t dst[6];
  std::vector<uint8_t> bytes;
};
static std::vector<Sent> gSent;
static int gQueueRoom = -1;  // frames the queue still takes (-1: no limit)
static std::vector<std::string> gHandshakes;
static std::vector<std::string> gConsole;
static char gReply[4096];
static unsigned gWatched = 0;

static std::string macText(const uint8_t* mac) {
  char text[18];
  macToDisplay(mac, text, sizeof(text));
  return text;
}
static uint32_t bfNow() { return gNowMs; }
// A partner change that begins while the tick reads the bond (cmd_exec runs
// between two reads on the device).
static bool gHoldDuringRead = false;
static void bfBond(BfBond* out) {
  notUnderLock();
  *out = gBond;
  if (gHoldDuringRead) {
    gHoldDuringRead = false;
    gHeld = true;
    ++gBond.epoch;
  }
}
static bool bfDeclined() { return gBond.agreeState == espnowfwd::kAgreeDeclined; }
static uint32_t bfEpoch() { return gBond.epoch; }
// A partner change runs on cmd_exec (bondWithFramesHeld): the epoch moved
// first, the settings and the agreement follow one by one.
static bool bfHeld(uint32_t epoch) { return gHeld || gBond.epoch != epoch; }
static bool bfSettingOn() { return gSetting; }
static uint16_t bfSensorMask() { return gMask; }
static void bfSetSettingOn(bool on) {
  notUnderLock();
  gSetting = on;
}
static void bfSetSensorMask(uint16_t mask) {
  notUnderLock();
  gMask = mask;
}
static bool bfMeshOn() { return gMeshOn; }
static int bfParseBool(const char* word) {
  const std::string w = word;
  if (w == "on" || w == "true" || w == "1") return 1;
  if (w == "off" || w == "false" || w == "0") return 0;
  return -1;
}
static uint8_t bfRole() { return gRole; }
static void bfTargets(SensorReceiverTargets* out) { *out = gTargets; }
// The named masters paired in another mesh than this device's primary one.
static std::vector<std::string> gOtherMesh;
static bool bfInPrimaryMesh(const uint8_t mac[6]) {
  notUnderLock();
  char text[18];
  macToDisplay(mac, text, sizeof(text));
  return std::find(gOtherMesh.begin(), gOtherMesh.end(), std::string(text)) == gOtherMesh.end();
}
// The bond-link control of the partner's sensors (BOND_STREAM_CTRL).
struct Ctrl {
  uint8_t sensor;
  bool on;
};
static std::vector<Ctrl> gCtrl;
static bool gCtrlFails = false;
static bool bfStreamCtrl(uint8_t sensor, bool on) {
  notUnderLock();
  if (gCtrlFails) return false;
  gCtrl.push_back(Ctrl{sensor, on});
  return true;
}
static bool gPartnerOnline = true;
static bool bfPartnerOnline() {
  notUnderLock();
  return gPartnerOnline;
}
// The saved 'bondstream' settings (what this device's user chose to stream
// at the partner on every bond session).
static uint16_t gSavedStreams = 0;
static uint16_t bfSavedStreams() {
  notUnderLock();
  return gSavedStreams;
}
static uint16_t bfSessionId(const uint8_t mac[6]) {
  notUnderLock();
  const TestSession* s = sessionOf(mac);
  return s ? s->id : 0;
}
static bool bfSettled(const uint8_t mac[6], uint32_t nowMs) {
  const TestSession* s = sessionOf(mac);
  return s && s->id && (uint32_t)(nowMs - s->upAtMs) >= 2000;
}
static bool bfHandshake(const uint8_t mac[6]) {
  notUnderLock();
  gHandshakes.push_back(macText(mac));
  return true;
}
static uint16_t bfBootTag() { return 0x0042; }
static uint16_t bfFrameCap(const uint8_t mac[6]) {
  const TestSession* s = sessionOf(mac);
  return (s && s->routed) ? 130 : 202;
}
static bool bfQueue(uint8_t kind, const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  notUnderLock();
  if (gQueueRoom == 0) return false;
  if (gQueueRoom > 0) --gQueueRoom;
  Sent s{};
  s.kind = kind;
  std::memcpy(s.dst, mac, 6);
  s.bytes.assign(payload, payload + len);
  gSent.push_back(s);
  return true;
}
static bool bfSendLink(const uint8_t mac[6], const uint8_t* payload, uint16_t len) { return bfQueue(155, mac, payload, len); }
static bool bfSendPart(const uint8_t mac[6], const uint8_t* part, uint16_t len) { return bfQueue(154, mac, part, len); }
static bool bfSendPower(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  if (!bfQueue(152, mac, payload, len)) return false;
  ++gWatched;
  return true;
}
static bool bfSendEvent(const uint8_t mac[6], const uint8_t* payload, uint16_t len) { return bfQueue(132, mac, payload, len); }
static bool bfSendStatus(const uint8_t mac[6], const uint8_t* payload, uint16_t len) { return bfQueue(151, mac, payload, len); }
// The longest names a device can show (31 bytes, as bfName's callers keep them).
static bool gLongNames = false;
static void bfName(const uint8_t mac[6], char* out, size_t cap) {
  notUnderLock();
  if (gLongNames) {
    std::snprintf(out, cap, "%c\"long-device-name-%02X-abcdefghij", mac[1] == 0xA0 ? 'P' : 'M', mac[5]);
    return;
  }
  const char* name = mac[5] == 0x01 && mac[1] == 0xA0 ? "porch"
                   : mac[5] == 0x02 && mac[1] == 0xA0 ? "shed"
                   : mac[5] == 0x10 ? "hub" : mac[5] == 0x11 ? "spare" : mac[5] == 0x12 ? "hub2" : nullptr;
  if (name) std::snprintf(out, cap, "%s", name);
  else macToDisplay(mac, out, cap);
}
static void bfConsole(const char* fmt, ...) {
  notUnderLock();
  char line[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  gConsole.push_back(line);
}
static char* bfReply(size_t* cap) {
  *cap = sizeof(gReply);
  std::memset(gReply, 0x7E, sizeof(gReply));
  return gReply;
}

// INSERT_PRODUCTION

// ---- Cases -------------------------------------------------------------------------------------
static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}
static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

static const uint8_t kPorch[6] = { 0x02, 0xA0, 0, 0, 0, 0x01 };   // the partner
static const uint8_t kShed[6] = { 0x02, 0xA0, 0, 0, 0, 0x02 };    // another partner later
static const uint8_t kHub[6] = { 0x02, 0xC0, 0, 0, 0, 0x10 };     // the named master
static const uint8_t kSpare[6] = { 0x02, 0xC0, 0, 0, 0, 0x11 };   // the named backup master
static const uint8_t kHub2[6] = { 0x02, 0xC0, 0, 0, 0, 0x12 };    // another master named later

static void sessionUp(const uint8_t mac[6], uint16_t id, bool routed = false) {
  TestSession* s = sessionOf(mac);
  if (!s) {
    gSessions.push_back(TestSession{});
    s = &gSessions.back();
    std::memcpy(s->mac, mac, 6);
  }
  s->id = id;
  s->upAtMs = gNowMs;
  s->routed = routed;
}
static void run(uint32_t seconds) {
  for (uint32_t i = 0; i < seconds; ++i) {
    gNowMs += 1000;
    bondForwardTick(gNowMs);
  }
}
static std::vector<espnowfwd::LinkFrame> linkFrames(const uint8_t* to = nullptr) {
  std::vector<espnowfwd::LinkFrame> out;
  for (const Sent& s : gSent) {
    if (s.kind != 155 || (to && std::memcmp(s.dst, to, 6) != 0)) continue;
    espnowfwd::LinkFrame f{};
    if (!espnowfwd::parseLinkFrame(s.bytes.data(), s.bytes.size(), f)) std::abort();
    out.push_back(f);
  }
  return out;
}
static size_t count(uint8_t kind, const uint8_t* to = nullptr) {
  size_t n = 0;
  for (const Sent& s : gSent) n += (s.kind == kind && (!to || std::memcmp(s.dst, to, 6) == 0)) ? 1 : 0;
  return n;
}
static void answer(const uint8_t from[6], uint8_t answers, uint8_t result, uint8_t gen, const uint8_t* partner = kPorch,
                   uint8_t streams = espnowfwd::kStreamsAll, uint16_t mask = espnowfwd::kSensorMaskDefault) {
  espnowfwd::LinkFrame a{};
  a.op = espnowfwd::kOpAnswer;
  a.answers = answers;
  a.result = result;
  a.linkGen = gen;
  a.viaBootTag = 0x0042;
  a.streams = streams;
  a.sensorMask = mask;
  if (partner) std::memcpy(a.partnerMac, partner, 6);
  bondForwardOnLinkFrame(from, a);
}
static std::string describe(const char* args) { return cmd_bond_forward(String(args)); }
static void reading(const uint8_t src[6], uint8_t sensor, uint16_t counter, const std::string& text, uint8_t flags = 0) {
  espnowsensor::ReadingHeader h{};
  h.sensorType = sensor;
  h.flags = flags;
  h.bootTag = 0xAAAA;
  h.counter = counter;
  h.partCount = 1;
  h.ageMs = 0;
  bondForwardReading(src, h, (const uint8_t*)text.data(), text.size());
}
static std::string jsonOf(size_t len) {
  std::string s = "{\"k\":\"";
  while (s.size() + 2 < len) s.push_back((char)('a' + (s.size() % 26)));
  s += "\"}";
  return s;
}

static void resetAll() {
  std::memset((void*)&sBf, 0, sizeof(sBf));
  gNowMs += 1000000;
  gBond = BfBond{};
  gBond.modeOn = true;
  gBond.havePartner = true;
  std::memcpy(gBond.partner, kPorch, 6);
  gBond.isMaster = true;
  gBond.partnerPaired = true;
  std::memset(gBond.partnerKey, 0x5A, sizeof(gBond.partnerKey));
  gBond.agreeState = espnowfwd::kAgreeAgreed;
  gBond.epoch = 1;
  gSetting = false;
  gMask = espnowfwd::kSensorMaskDefault;
  gMeshOn = true;
  gRole = 0;
  gTargets = SensorReceiverTargets{};
  gTargets.masterNamed = gTargets.backupNamed = true;
  gTargets.havePrimary = gTargets.haveBackup = true;
  std::memcpy(gTargets.primary, kHub, 6);
  std::memcpy(gTargets.backup, kSpare, 6);
  gSessions.clear();
  sessionUp(kHub, 0x1111);
  sessionUp(kSpare, 0x2222);
  gNowMs += 5000;  // both sessions settled
  gSent.clear();
  gQueueRoom = -1;
  gHandshakes.clear();
  gConsole.clear();
  gWatched = 0;
  gHeld = false;
  gLongNames = false;
  gOtherMesh.clear();
  gCtrl.clear();
  gCtrlFails = false;
  gPartnerOnline = true;
  gSavedStreams = 0;
}

// Forwarding on, both masters accepted.
static void upAndAccepted() {
  gSetting = true;
  run(1);
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);
  answer(kSpare, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1, kPorch,
         espnowfwd::kStreamBattery | espnowfwd::kStreamEvents, 0);
  // The partner's sensor list: every sensor there, none streaming.
  espnowfwd::StatusFrame st{};
  st.presentMask = espnowsensor::kSensorMaskAll;
  bondForwardStatus(kPorch, st);
  gSent.clear();
}

static void testOffAndReasons() {
  resetAll();
  // Off: every master named here hears once per session that this device
  // forwards for nobody, and nothing else.
  run(3);
  std::vector<espnowfwd::LinkFrame> f = linkFrames();
  check(f.size() == 2 && f[0].op == espnowfwd::kOpWithdraw && espnowfwd::macIsZero(f[0].partnerMac) && f[0].linkGen == 0 &&
        f[0].viaBootTag == 0x0042, "off: one withdrawal of everything per master and session, and nothing more");
  check(describe("") == "OK: forwarding is off.", "bondforward: off");
  sessionUp(kHub, 0x1112);  // the master restarted
  gNowMs += 2000;
  run(1);
  check(linkFrames().size() == 3, "a new session hears it again");
  // The reasons, first match.
  gSetting = true;
  gBond.isMaster = false;
  check(describe("") == "OK: stored, but this device is the bond worker: only a bond master forwards. Its own data goes to its "
                        "bond master only.", "a bond worker is told it forwards nothing");
  gBond.isMaster = true;
  gBond.partnerPaired = false;
  check(describe("") == "OK: forwarding is on, but nothing is forwarded yet: only a bond master with a paired partner forwards.",
        "a partner that is not paired");
  gBond.partnerPaired = true;
  gBond.agreeState = espnowfwd::kAgreeUnknown;
  gBond.epoch = 2;  // another partner: what the last one said no longer counts
  check(describe("") == "OK: forwarding is on, but nothing is forwarded yet: the partner has not agreed to the bond.",
        "never agreed: nothing is announced");
  run(2);
  check(linkFrames().size() == 3, "nothing announced before the partner agreed");
  gBond.agreeState = espnowfwd::kAgreeAgreed;
  gMeshOn = false;
  check(has(describe(""), "mesh mode is off"), "mesh off");
  gMeshOn = true;
  gRole = 1;
  check(describe("") == "OK: forwarding is on. This device is the mesh master: porch's readings, battery state and events are "
                        "kept here; nothing goes over the air.", "the acting master keeps its partner's data");
  gRole = 2;  // a backup master that has not taken over forwards
  gTargets.havePrimary = gTargets.haveBackup = false;
  check(has(describe(""), "the master named on this device is not paired with it"), "no named master is paired");
  gTargets.masterNamed = gTargets.backupNamed = false;
  check(has(describe(""), "no master is named on this device (espnowmasterfingerprint)"), "no master named");
  // bondforward sensors.
  check(describe("sensors tof,gps") == "OK: forwarded sensors: tof, gps (input is left out: bondforward sensors all)" &&
        gMask == (espnowsensor::sensorBit(REMOTE_SENSOR_TOF) | espnowsensor::sensorBit(REMOTE_SENSOR_GPS)), "by name");
  check(describe("sensors all") == "OK: forwarded sensors: thermal, tof, imu, gps, input, fmradio, camera, microphone, rtc, "
                                   "presence, apds" && gMask == espnowsensor::kSensorMaskAll, "all");
  check(describe("sensors default") == "OK: forwarded sensors: thermal, tof, imu, gps, fmradio, camera, microphone, rtc, "
                                       "presence, apds (input is left out: bondforward sensors all)" &&
        gMask == espnowfwd::kSensorMaskDefault, "default");
  check(has(describe("sensors tof,warp"), "Error: 'warp' is not a sensor") && gMask == espnowfwd::kSensorMaskDefault,
        "an unknown name changes nothing");
  check(describe("sideways") == "Usage: bondforward [on | off | sensors <all | default | name,name,...>]" &&
        describe("on now") == "Usage: bondforward [on | off | sensors <all | default | name,name,...>]", "usage");
  check(describe("0") == "OK: forwarding is off." && !gSetting, "a settings editor's 0");
}

static void testAnnounceAndForward() {
  resetAll();
  check(describe("on").rfind("OK: forwarding is on, but nothing is forwarded yet: waiting for hub to accept.", 0) == 0 ||
        describe("").rfind("OK: forwarding is on, but nothing is forwarded yet: waiting for hub to accept.", 0) == 0,
        "on: waiting for the master");
  run(1);
  std::vector<espnowfwd::LinkFrame> hub = linkFrames(kHub);
  std::vector<espnowfwd::LinkFrame> spare = linkFrames(kSpare);
  check(hub.size() == 1 && hub[0].op == espnowfwd::kOpAnnounce && hub[0].linkGen == 1 && hub[0].viaBootTag == 0x0042 &&
        hub[0].streams == espnowfwd::kStreamsAll && hub[0].sensorMask == espnowfwd::kSensorMaskDefault &&
        std::memcmp(hub[0].partnerMac, kPorch, 6) == 0 && hub[0].partnerKey[0] == 0x5A && hub[0].partnerKey[31] == 0x5A &&
        std::string(hub[0].partnerName, 5) == "porch" && hub[0].partnerName[5] == 0,
        "the primary hears the link: link 1, readings, battery, events, the default sensors, the partner and its key");
  check(spare.size() == 1 && spare[0].streams == (espnowfwd::kStreamBattery | espnowfwd::kStreamEvents) &&
        spare[0].sensorMask == 0, "the backup master: battery state and events only");
  // Before an answer nothing is forwarded.
  reading(kPorch, REMOTE_SENSOR_GPS, 1, "{\"v\":1}");
  check(count(154) == 0, "nothing before the master accepted");
  // Back-off: again after 2 s, then 4 s.
  run(1);
  check(linkFrames(kHub).size() == 1, "not within 2 s");
  run(1);
  check(linkFrames(kHub).size() == 2 && linkFrames(kHub)[1].linkGen == 1, "again after 2 s, the same number");
  // A wrong answer is ignored; the right one accepts.
  gConsole.clear();
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 2);
  check(sBf.counts.answersIgnored == 1, "an answer to another number is ignored and counted");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);
  answer(kSpare, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1, kPorch,
         espnowfwd::kStreamBattery | espnowfwd::kStreamEvents, 0);
  check(gConsole.size() == 2 && gConsole[0] == "[ESP-NOW] hub accepted: this device forwards porch's readings, battery state and "
                                               "events to it" &&
        gConsole[1] == "[ESP-NOW] spare accepted: this device forwards porch's battery state and events to it",
        "each acceptance is said once");
  run(1);
  check(describe("") == "OK: forwarding is on: porch's readings, battery state and events go to hub (accepted); porch's "
                        "battery state and events go also to spare (accepted).", "bondforward says where it goes");
  gSent.clear();

  // A reading: X's header, the bit and the link number, the parts planned for the hub's path.
  reading(kPorch, REMOTE_SENSOR_GPS, 7, "{\"v\":7}", espnowsensor::kReadingFlagStreamStart);
  check(count(154, kHub) == 1 && count(154, kSpare) == 0, "readings go to the primary only");
  espnowsensor::ReadingHeader h{};
  espnowsensor::parseReadingHeader(gSent[0].bytes.data(), gSent[0].bytes.size(), h);
  check(h.sensorType == REMOTE_SENSOR_GPS && h.counter == 7 && h.bootTag == 0xAAAA &&
        h.flags == (espnowsensor::kReadingFlagStreamStart | espnowsensor::kReadingFlagForwarded) && h.fwdLink == 1 &&
        h.partCount == 1 && std::string((const char*)gSent[0].bytes.data() + 12, gSent[0].bytes.size() - 12) == "{\"v\":7}",
        "X's sensor, counter, boot tag and stream start, with the forwarded bit and link 1");
  // 472 bytes through a relay: four parts of 12 + 118, all with the same number.
  gSent.clear();
  sessionOf(kHub)->routed = true;
  reading(kPorch, REMOTE_SENSOR_IMU, 8, jsonOf(472));
  check(count(154) == 4 && gSent[0].bytes.size() == 130 && gSent[3].bytes.size() == 12 + 472 - 3 * 118,
        "472 bytes routed: four parts, each one routed frame");
  bool same = true;
  std::string joined;
  for (uint8_t i = 0; i < 4; ++i) {
    espnowsensor::ReadingHeader p{};
    espnowsensor::parseReadingHeader(gSent[i].bytes.data(), gSent[i].bytes.size(), p);
    same = same && p.partIndex == i && p.partCount == 4 && p.counter == 8 && p.fwdLink == 1 &&
           (p.flags & espnowsensor::kReadingFlagForwarded);
    joined.append((const char*)gSent[i].bytes.data() + 12, gSent[i].bytes.size() - 12);
  }
  check(same && joined == jsonOf(472), "every part carries X's numbers and the link number; the text is whole");
  sessionOf(kHub)->routed = false;
  // A sensor that is not forwarded (input, by default).
  gSent.clear();
  reading(kPorch, REMOTE_SENSOR_INPUT, 1, "{\"b\":1}");
  check(count(154) == 0 && sBf.counts.notForwarded[0][(size_t)espnowfwd::WhyNot::SensorOff] == 1, "input: sensor_off");
  // Battery state: to both masters, the link number in byte 16, X's values, acknowledged.
  espnowsensor::PowerFrame pf{};
  pf.counter = 3;
  pf.bootTag = 0xAAAA;
  pf.known = espnowsensor::kPowerPercentValid;
  pf.percentX10 = 715;
  bondForwardPower(kPorch, pf);
  check(count(152, kHub) == 1 && count(152, kSpare) == 1 && gWatched == 2, "battery state to both, acknowledged");
  espnowsensor::PowerFrame back{};
  espnowsensor::parsePowerFrame(gSent.back().bytes.data(), gSent.back().bytes.size(), back);
  check(back.fwdLink == 1 && back.counter == 3 && back.bootTag == 0xAAAA && back.known == espnowsensor::kPowerPercentValid &&
        back.percentX10 == 715, "X's battery frame with the link number");
  // Events: to both, bit 4 and the link number, X's cut bits kept.
  espnowsensor::EventFrame ef{};
  ef.counter = 4;
  ef.bootTag = 0xAAAA;
  ef.flags = espnowsensor::kEventCutDetail;
  ef.eventSeq = 99;
  espnowsensor::eventFieldPut(ef.kind, sizeof(ef.kind), "login_fail", 11);
  bondForwardEvent(kPorch, ef);
  check(count(132, kHub) == 1 && count(132, kSpare) == 1, "an event to both");
  espnowsensor::EventFrame eb{};
  espnowsensor::parseEventFrame(gSent.back().bytes.data(), gSent.back().bytes.size(), eb);
  check(eb.flags == (espnowsensor::kEventCutDetail | espnowsensor::kEventFlagForwarded) && eb.fwdLink == 1 &&
        eb.eventSeq == 99 && eb.counter == 4, "X's event with bit 4 and the link number");
  check(sBf.counts.forwarded[0] == 2 && sBf.counts.forwarded[1] == 1 && sBf.counts.forwarded[2] == 1, "counted forwarded");

  // A frame from another device than the link's partner, or after a partner
  // change began (the epoch moved), is not filed under the link.
  gSent.clear();
  reading(kShed, REMOTE_SENSOR_GPS, 9, "{\"v\":9}");
  gBond.epoch = 2;
  reading(kPorch, REMOTE_SENSOR_GPS, 10, "{\"v\":10}");
  gBond.epoch = 1;
  check(count(154) == 0 && sBf.counts.notForwarded[0][(size_t)espnowfwd::WhyNot::PartnerChanged] == 2,
        "another source or another epoch: partner_changed, nothing sent");
  // 'bondforward off' stops at once, before the tick.
  gSetting = false;
  reading(kPorch, REMOTE_SENSOR_GPS, 11, "{\"v\":11}");
  check(count(154) == 0 && sBf.counts.notForwarded[0][(size_t)espnowfwd::WhyNot::Off] >= 1, "off: stops at once");
  gSetting = true;
  // The queue full: the reading stops, counted.
  gQueueRoom = 2;
  reading(kPorch, REMOTE_SENSOR_IMU, 12, jsonOf(400));
  check(count(154) == 2 && sBf.counts.notForwarded[0][(size_t)espnowfwd::WhyNot::QueueFull] == 1,
        "a full queue stops the reading and is counted");
  gQueueRoom = -1;
  bondForwardNotKept(espnowfwd::kStreamIdEvents);
  check(sBf.counts.notForwarded[2][(size_t)espnowfwd::WhyNot::NotKeptHere] == 1, "what this device could not keep is counted");
  // JSON.
  char json[espnowfwd::kStatusJsonCap];
  check(bondForwardStatusJson(json, sizeof(json)) > 0 && has(json, "{\"setting\":true,\"sensors\":[\"thermal\"") &&
        has(json, "\"forwards\":true,\"whyNot\":null,\"linkGen\":1,\"partner\":{\"mac\":\"02:A0:00:00:00:01\",\"name\":\"porch\"}") &&
        has(json, "{\"kind\":\"primary\",\"mac\":\"02:C0:00:00:00:10\",\"name\":\"hub\",\"isPartner\":false,"
                  "\"inPrimaryMesh\":true,\"state\":\"accepted\","
                  "\"result\":\"accepted\",\"streams\":[\"readings\",\"battery\",\"events\"],\"session\":true}") &&
        has(json, "\"forwarded\":{\"readings\":2,\"battery\":1,\"events\":1}") &&
        has(json, "\"partner_changed\":2,") && has(json, "\"sensor_off\":1") && has(json, "\"not_accepted\":1") &&
        has(json, "\"events\":{\"not_kept_here\":1}"), std::string("the forward object of bondstatus json: ") + json);
  char small[40];
  check(bondForwardStatusJson(small, sizeof(small)) == 0 && small[0] == '\0', "a buffer too small gets nothing, not half");
  char line[espnowfwd::kStatusLinesCap];
  bondForwardStatusLines(line, sizeof(line));
  check(std::string(line) == "Forwarding to the mesh: on: porch's readings, battery state and events go to hub (accepted);\n"
                             "  porch's battery state and events go also to spare (accepted).\n"
                             "  Forwarded 2 readings, 1 battery frames, 1 events; 7 not forwarded, mostly: it came from "
                             "another device than the partner the link names.",
        std::string("the bondstatus lines: ") + line);
  // One console line a minute while frames are not forwarded.
  gConsole.clear();
  run(61);
  check(gConsole.size() == 1 && has(gConsole[0], "[ESP-NOW] porch's"), "the not-forwarded line");
  reading(kPorch, REMOTE_SENSOR_INPUT, 2, "{}");
  run(30);
  check(gConsole.size() == 1, "at most once a minute");
}

static void testSessionsAndChanges() {
  resetAll();
  upAndAccepted();
  // A rekey keeps the session id: nothing is announced, data keeps flowing.
  sessionOf(kHub)->upAtMs = gNowMs;  // the rekey reset the age
  run(5);
  reading(kPorch, REMOTE_SENSOR_GPS, 1, "{}");
  check(linkFrames().empty() && count(154) == 1, "a rekey: still accepted, no announcement, data flows");
  // A new session (the master restarted): announced again on it, the same
  // number; nothing forwarded until it answers.
  gSent.clear();
  sessionUp(kHub, 0x1199);
  reading(kPorch, REMOTE_SENSOR_GPS, 2, "{}");
  check(count(154) == 0, "nothing on a new session before the answer");
  run(1);
  check(linkFrames(kHub).empty(), "not before the session settled");
  run(2);
  check(linkFrames(kHub).size() == 1 && linkFrames(kHub)[0].linkGen == 1, "announced on the new session, same number");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);
  reading(kPorch, REMOTE_SENSOR_GPS, 3, "{}");
  check(count(154) == 1, "and forwarded again once answered");
  // A query: announced again at once.
  gSent.clear();
  espnowfwd::LinkFrame q{};
  q.op = espnowfwd::kOpQuery;
  bondForwardOnLinkFrame(kHub, q);
  run(1);
  check(linkFrames(kHub).size() == 1 && linkFrames(kHub)[0].op == espnowfwd::kOpAnnounce, "a query: announced again");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);
  // The 10-minute refresh.
  gSent.clear();
  run(600);
  check(linkFrames(kHub).size() == 1, "refreshed every 10 minutes");
  // The partner out of range (agreement unknown after it agreed): the link stands.
  gSent.clear();
  gBond.agreeState = espnowfwd::kAgreeUnknown;
  run(5);
  check(linkFrames().empty(), "a partner out of range: no withdrawal");
  gBond.agreeState = espnowfwd::kAgreeAgreed;
  // Declined: withdrawn (three tries until answered), a new number once agreed again.
  gBond.agreeState = espnowfwd::kAgreeDeclined;
  run(1);
  std::vector<espnowfwd::LinkFrame> w = linkFrames(kHub);
  check(w.size() == 1 && w[0].op == espnowfwd::kOpWithdraw && w[0].linkGen == 1 && std::memcmp(w[0].partnerMac, kPorch, 6) == 0,
        "declined: the link is withdrawn");
  run(10);
  check(linkFrames(kHub).size() == 3, "three tries without an answer, then no more");
  answer(kSpare, espnowfwd::kOpWithdraw, espnowfwd::kResultWithdrawn, 1);
  const size_t spareBefore = linkFrames(kSpare).size();
  run(10);
  check(linkFrames(kSpare).size() == spareBefore, "an answered withdrawal is not repeated");
  gSent.clear();
  gBond.agreeState = espnowfwd::kAgreeAgreed;
  run(1);
  check(linkFrames(kHub).size() == 1 && linkFrames(kHub)[0].op == espnowfwd::kOpAnnounce && linkFrames(kHub)[0].linkGen == 2,
        "agreed again: a new link number");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 2);
  // A partner change: the new partner's frame before the tick is not filed
  // under the old link; the tick withdraws link 2 and announces link 3 once
  // the new partner agreed.
  gSent.clear();
  gBond.epoch = 7;
  std::memcpy(gBond.partner, kShed, 6);
  gBond.agreeState = espnowfwd::kAgreeUnknown;
  reading(kShed, REMOTE_SENSOR_GPS, 1, "{}");
  check(count(154) == 0, "the new partner's reading before the tick: not forwarded");
  run(1);
  w = linkFrames(kHub);
  check(w.size() == 1 && w[0].op == espnowfwd::kOpWithdraw && w[0].linkGen == 2 && std::memcmp(w[0].partnerMac, kPorch, 6) == 0,
        "the old link is withdrawn with its number and partner");
  answer(kHub, espnowfwd::kOpWithdraw, espnowfwd::kResultWithdrawn, 2, kPorch);
  answer(kSpare, espnowfwd::kOpWithdraw, espnowfwd::kResultWithdrawn, 2, kPorch);
  gBond.agreeState = espnowfwd::kAgreeAgreed;
  run(1);
  w = linkFrames(kHub);
  check(w.back().op == espnowfwd::kOpAnnounce && w.back().linkGen == 3 && std::memcmp(w.back().partnerMac, kShed, 6) == 0,
        "the new partner is announced with a new number");
  // Stale: the master holds a newer number: a new one.
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultStale, 3, kShed);
  gSent.clear();
  run(1);
  check(linkFrames(kHub).size() == 1 && linkFrames(kHub)[0].linkGen == 4,
        "stale: announced again with a new number (" + std::to_string(linkFrames(kHub).size()) + " frames, link " +
            (linkFrames(kHub).empty() ? std::string("-") : std::to_string(linkFrames(kHub)[0].linkGen)) + ")");
  // Refused: said, retried after 10 minutes.
  gConsole.clear();
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultNameTaken, 4, kShed);
  check(gConsole.size() == 1 && gConsole[0] == "[ESP-NOW] hub refused forwarding for shed: the partner's name is taken by "
                                               "another device there", "a refusal is said");
  run(1);
  check(has(describe(""), "hub refused: the partner's name is taken by another device there"), "bondforward says why");
  gSent.clear();
  run(598);
  check(linkFrames(kHub).empty(), "not retried within 10 minutes");
  run(1);
  check(linkFrames(kHub).size() == 1 && linkFrames(kHub)[0].op == espnowfwd::kOpAnnounce && linkFrames(kHub)[0].linkGen == 4,
        "retried after 10 minutes");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 4, kShed);
  // Another master named: the old one gets one withdrawal, the new one the announcement.
  gSent.clear();
  std::memcpy(gTargets.primary, kHub2, 6);
  sessionUp(kHub2, 0x3333);
  gNowMs += 3000;
  run(1);
  check(linkFrames(kHub).size() == 1 && linkFrames(kHub)[0].op == espnowfwd::kOpWithdraw &&
        linkFrames(kHub2).size() == 1 && linkFrames(kHub2)[0].op == espnowfwd::kOpAnnounce && linkFrames(kHub2)[0].linkGen == 4,
        "the old master hears the withdrawal, the new one the announcement");
  // No session with a master that is wanted: a handshake is started.
  gHandshakes.clear();
  sessionOf(kHub2)->id = 0;
  run(1);
  check(gHandshakes.size() == 1 && gHandshakes[0] == "02:C0:00:00:00:12", "a session is started with the master");
  // Becoming the acting master: withdrawn, kept locally.
  sessionUp(kHub2, 0x3334);
  gNowMs += 3000;
  run(1);
  answer(kHub2, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 4, kShed);
  gSent.clear();
  gRole = 1;
  run(1);
  check(linkFrames(kHub2).size() == 1 && linkFrames(kHub2)[0].op == espnowfwd::kOpWithdraw, "promoted: withdrawn");
  reading(kShed, REMOTE_SENSOR_GPS, 2, "{}");
  check(count(154) == 0 && sBf.counts.notForwarded[0][(size_t)espnowfwd::WhyNot::IsMaster] == 1,
        "the acting master forwards nothing: is_master");
  gRole = 0;
  gSent.clear();
  run(1);
  check(linkFrames(kHub2).size() >= 1 && linkFrames(kHub2).back().op == espnowfwd::kOpAnnounce &&
        linkFrames(kHub2).back().linkGen == 5, "demoted: announced with a new number");
  // The named master is the bond partner itself: it gets nothing.
  resetAll();
  gSetting = true;
  std::memcpy(gTargets.primary, kPorch, 6);
  sessionUp(kPorch, 0x4444);
  gNowMs += 3000;
  run(2);
  std::vector<espnowfwd::LinkFrame> toPartner = linkFrames(kPorch);
  check(toPartner.size() == 1 && toPartner[0].op == espnowfwd::kOpWithdraw && espnowfwd::macIsZero(toPartner[0].partnerMac) &&
        linkFrames(kSpare).size() == 1 && linkFrames(kSpare)[0].op == espnowfwd::kOpAnnounce && gHandshakes.empty(),
        "the partner as the named master is never announced to: it hears once per session that this device forwards "
        "for nobody, and no session is started with it");
  reading(kPorch, REMOTE_SENSOR_GPS, 1, "{}");
  check(count(154) == 0, "and gets no data");
  char json[espnowfwd::kStatusJsonCap];
  check(bondForwardStatusJson(json, sizeof(json)) > 0 && has(json, "\"isPartner\":true"), "and the JSON says so");
}

// Review findings of 2026-11-18.
static void testReviewCases() {
  // 1. A partner change runs on cmd_exec while the tick runs: the epoch moved
  // first, the new partner is written, and the old partner's agreement is
  // reset last. The tick waits for the change to finish and never announces
  // a partner that has not agreed.
  resetAll();
  upAndAccepted();
  gHeld = true;
  gBond.epoch = 2;
  std::memcpy(gBond.partner, kShed, 6);  // the old partner's "agreed" still stands
  run(3);
  check(gSent.empty(), "nothing is withdrawn or announced while a partner change runs");
  gBond.agreeState = espnowfwd::kAgreeUnknown;  // bondAgreementReset, the last step of the change
  gHeld = false;
  run(1);
  std::vector<espnowfwd::LinkFrame> hub = linkFrames(kHub);
  check(hub.size() == 1 && hub[0].op == espnowfwd::kOpWithdraw && hub[0].linkGen == 1 &&
        std::memcmp(hub[0].partnerMac, kPorch, 6) == 0, "after the change the old link is withdrawn");
  run(20);
  bool announcedShed = false;
  for (const auto& f : linkFrames()) announcedShed = announcedShed || f.op == espnowfwd::kOpAnnounce;
  check(!announcedShed, "and the new partner, which has not agreed, is not announced");
  gBond.agreeState = espnowfwd::kAgreeAgreed;
  gSent.clear();
  run(1);
  hub = linkFrames(kHub);
  check(!hub.empty() && hub.back().op == espnowfwd::kOpAnnounce && hub.back().linkGen == 2 &&
        std::memcmp(hub.back().partnerMac, kShed, 6) == 0, "it is announced once it agreed");
  // A change that begins while the tick reads the bond: that tick does nothing.
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 2, kShed);
  gSent.clear();
  gBond.agreeState = espnowfwd::kAgreeAgreed;
  std::memcpy(gBond.partner, kPorch, 6);  // what the reader sees: the new partner, the old answer
  gHoldDuringRead = true;
  run(1);
  check(gSent.empty() && sBf.link.linkGen == 2, "a change that began during the read: the tick acts on nothing it read");
  gHeld = false;

  // 2. The bond partner becomes the master named here: the link it was told
  // of is withdrawn there (and nothing else is sent to it).
  resetAll();
  upAndAccepted();
  std::memcpy(gBond.partner, kHub, 6);  // bondconnect hub
  gBond.epoch = 2;
  gBond.agreeState = espnowfwd::kAgreeUnknown;
  run(1);
  hub = linkFrames(kHub);
  check(hub.size() == 1 && hub[0].op == espnowfwd::kOpWithdraw && hub[0].linkGen == 1 &&
        std::memcmp(hub[0].partnerMac, kPorch, 6) == 0, "the new partner, the old master, hears the old link's withdrawal");
  run(10);
  check(linkFrames(kHub).size() == 3, "tried three times until answered");
  gSent.clear();
  gBond.agreeState = espnowfwd::kAgreeAgreed;
  run(5);
  for (const auto& f : linkFrames(kHub)) check(f.op != espnowfwd::kOpAnnounce, "the partner is never announced to");
  check(!linkFrames(kSpare).empty() && linkFrames(kSpare).back().op == espnowfwd::kOpAnnounce &&
        std::memcmp(linkFrames(kSpare).back().partnerMac, kHub, 6) == 0, "the backup master hears the new link");

  // 3. The hold epoch moved without another partner (bondconnect to the same
  // device again): the old link is withdrawn and a new one announced.
  resetAll();
  upAndAccepted();
  gBond.epoch = 5;
  run(1);
  hub = linkFrames(kHub);
  check(hub.size() == 2 && hub[0].op == espnowfwd::kOpWithdraw && hub[0].linkGen == 1 &&
        hub[1].op == espnowfwd::kOpAnnounce && hub[1].linkGen == 2 && std::memcmp(hub[1].partnerMac, kPorch, 6) == 0,
        "another epoch, the same partner: link 1 withdrawn, link 2 announced");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 2);
  gSent.clear();
  reading(kPorch, REMOTE_SENSOR_GPS, 1, "{}");
  check(count(154, kHub) == 1, "and the partner's readings flow on the new link");

  // 4. The forwarded sensors change while an announcement is out: announced
  // again at once, and the answer to the older one does not count.
  resetAll();
  gSetting = true;
  run(1);
  check(linkFrames(kHub).size() == 1 && linkFrames(kHub)[0].sensorMask == espnowfwd::kSensorMaskDefault, "announced");
  check(describe("sensors all").rfind("OK: forwarded sensors: thermal", 0) == 0, "bondforward sensors all");
  run(1);
  hub = linkFrames(kHub);
  check(hub.size() == 2 && hub[1].sensorMask == espnowsensor::kSensorMaskAll, "announced again at once with every sensor");
  const uint32_t ignoredBefore = sBf.counts.answersIgnored;
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);  // echoes the default sensors
  check(sBf.targets[espnowfwd::kTargetPrimary].state == espnowfwd::TState::Announcing &&
        sBf.counts.answersIgnored == ignoredBefore + 1, "the answer to the older announcement is ignored");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1, kPorch, espnowfwd::kStreamsAll,
         espnowsensor::kSensorMaskAll);
  gSent.clear();
  reading(kPorch, REMOTE_SENSOR_INPUT, 1, "{\"b\":1}");
  check(count(154, kHub) == 1, "the answer to the newer one: input is forwarded at once");

  // 5. The longest status there can be: 31-byte names that need escaping,
  // the longest refusal, every count at its largest.
  resetAll();
  upAndAccepted();
  answer(kSpare, espnowfwd::kOpAnnounce, espnowfwd::kResultNameTaken, 1, kPorch,
         espnowfwd::kStreamBattery | espnowfwd::kStreamEvents, 0);
  run(1);
  for (auto& stream : sBf.counts.notForwarded) {
    for (auto& n : stream) n = 0xFFFFFFFFu;
  }
  for (auto& n : sBf.counts.forwarded) n = 0xFFFFFFFFu;
  sBf.counts.answersIgnored = 0xFFFFFFFFu;
  // ... and the mesh master's requests for the partner: every sensor leased,
  // the longest reason, every count at its largest.
  sBf.req.applied = sBf.req.refused = 0xFFFFFFFFu;
  sBf.req.lastRefused = (uint8_t)espnowfwd::ReqVerdict::NotBondMaster;
  sBf.req.leases.leased = (uint16_t)espnowsensor::kSensorMaskAll;
  for (auto& n : sBf.req.refusedBy) n = 0xFFFFFFFFu;
  sBf.req.lacking = 0xFFFFFFFFu;
  sBf.req.lastLacking = (uint16_t)espnowsensor::kSensorMaskAll;
  gLongNames = true;
  char lines[espnowfwd::kStatusLinesCap];
  bondForwardStatusLines(lines, sizeof(lines));
  std::vector<std::string> split;
  for (const char* p = lines; *p;) {
    const char* nl = std::strchr(p, '\n');
    split.emplace_back(p, nl ? (size_t)(nl - p) : std::strlen(p));
    p = nl ? nl + 1 : p + std::strlen(p);
  }
  bool fit = split.size() == 5;
  size_t longest = 0;
  for (const auto& l : split) {
    fit = fit && l.size() <= espnowfwd::kStatusLineMax && !l.empty() && l.back() != ' ';
    longest = std::max(longest, l.size());
  }
  check(fit && split[1].find("(refused: the partner's name is taken by another device there).") != std::string::npos &&
            split[2].rfind("  Forwarded 4294967295 readings, 4294967295 battery frames, 4294967295 events; ", 0) == 0 &&
            split[2].back() == '.' &&
            split[3] == "  Sensor requests for the partner: 4294967295 applied, 4294967295 refused "
                        "(last: only a bond master with a paired partner takes requests for it); leased now: thermal, "
                        "tof, imu, gps, input, fmradio, camera, microphone, rtc, presence, apds." &&
            split[4] == "  Sensors the partner does not have running, asked for 4294967295 times (last: thermal, tof, "
                        "imu, gps, input, fmradio, camera, microphone, rtc, presence, apds): never leased; 4294967295 "
                        "requests came before its sensor list was known.",
        std::string("every bondstatus line is whole and fits one console message: ") + lines);
  char json[espnowfwd::kStatusJsonCap];
  const size_t len = bondForwardStatusJson(json, sizeof(json));
  check(len > 0 && len < sizeof(json) && has(json, "\"answersIgnored\":4294967295,") && has(json, "\\\"long-device") &&
            has(json, "\"sensorRequests\":{\"applied\":4294967295,\"refused\":4294967295,\"lastRefused\":\"not_bond_master\","
                      "\"refusedBy\":{\"off\":4294967295,") &&
            has(json, "\"partner_lacks_sensor\":4294967295},\"partnerLacks\":{\"count\":4294967295,\"last\":[\"thermal\",") &&
            has(json, "\"leased\":[\"thermal\",") && has(json, "\"apds\"]}}"),
        "the longest forward object fits espnowfwd::kStatusJsonCap (" + std::to_string(len) + " bytes)");
  std::printf("longest forward object: %zu of %zu bytes; longest bondstatus line: %zu of %zu\n", len, sizeof(json),
              longest, espnowfwd::kStatusLineMax);
  gLongNames = false;
}


// Owner decisions of 2026-11-18, the bond master's side.
static size_t ctrlCount(uint8_t sensor, bool on) {
  size_t n = 0;
  for (const Ctrl& c : gCtrl) n += (c.sensor == sensor && c.on == on) ? 1 : 0;
  return n;
}
static void request(const uint8_t from[6], uint8_t mode, uint32_t mask, uint32_t leaseMs, uint8_t fwdLink = 1,
                    uint32_t intervalMs = 0) {
  bondForwardSensorRequest(from, mode, mask, intervalMs, leaseMs, fwdLink);
}
static espnowfwd::StatusFrame statusFrame(const std::vector<uint8_t>& bytes) {
  espnowfwd::StatusFrame f{};
  if (!espnowfwd::parseStatusFrame(bytes.data(), bytes.size(), f)) std::abort();
  return f;
}

static void testPrimaryMesh() {
  // The backup master is paired in another mesh: it gets nothing, the master
  // of the primary mesh gets everything.
  resetAll();
  gOtherMesh.push_back("02:C0:00:00:00:11");
  gSetting = true;
  run(3);
  check(!linkFrames(kHub).empty() && linkFrames(kHub)[0].op == espnowfwd::kOpAnnounce, "the primary mesh's master hears the link");
  for (const auto& f : linkFrames(kSpare)) check(f.op != espnowfwd::kOpAnnounce, "a master in another mesh is never announced to");
  check(linkFrames(kSpare).size() == 1 && espnowfwd::macIsZero(linkFrames(kSpare)[0].partnerMac),
        "it hears once per session that this device forwards nothing to it");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);
  answer(kSpare, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1, kPorch,
         espnowfwd::kStreamBattery | espnowfwd::kStreamEvents, 0);  // an answer it was never asked for
  run(1);
  gSent.clear();
  espnowsensor::PowerFrame pf{};
  pf.counter = 1;
  bondForwardPower(kPorch, pf);
  check(count(152, kHub) == 1 && count(152, kSpare) == 0, "battery state only to the primary mesh's master");
  check(describe("") == "OK: forwarding is on: porch's readings, battery state and events go to hub (accepted).",
        "bondforward names only the primary mesh's master");
  char json[espnowfwd::kStatusJsonCap];
  check(bondForwardStatusJson(json, sizeof(json)) > 0 && has(json, "\"kind\":\"backup\"") &&
        has(json, "\"isPartner\":false,\"inPrimaryMesh\":false"), "the JSON says the backup is in another mesh");
  // The master moves to another mesh while accepted: the link is withdrawn
  // there and nothing more goes to it.
  gOtherMesh.push_back("02:C0:00:00:00:10");
  gSent.clear();
  run(1);
  std::vector<espnowfwd::LinkFrame> hub = linkFrames(kHub);
  check(hub.size() == 1 && hub[0].op == espnowfwd::kOpWithdraw && hub[0].linkGen == 1 &&
        std::memcmp(hub[0].partnerMac, kPorch, 6) == 0, "a master that left the primary mesh hears the withdrawal");
  reading(kPorch, REMOTE_SENSOR_GPS, 5, "{}");
  check(count(154) == 0, "and gets no more readings");
  check(describe("") == "OK: forwarding is on, but nothing is forwarded yet: the master named here is paired in another "
                        "mesh than this device's primary mesh.", "with no master in the primary mesh, bondforward says why");
  // Back in the primary mesh: announced again, with a new link number.
  gOtherMesh.clear();
  gSent.clear();
  run(1);
  hub = linkFrames(kHub);
  check(!hub.empty() && hub.back().op == espnowfwd::kOpAnnounce && hub.back().linkGen == 2, "back: a new link");
}

static void testStatusAndRequests() {
  const uint32_t tof = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  const uint32_t imu = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  const uint32_t input = espnowsensor::sensorBit(REMOTE_SENSOR_INPUT);
  // The partner's sensor status before the master accepted: kept, sent on
  // acceptance with the forwarded sensors only, the link byte and X's boot tag.
  resetAll();
  gSetting = true;
  run(1);
  espnowfwd::StatusFrame st{};
  st.presentMask = tof | imu | input;
  st.streamingMask = imu | input;
  st.bootTag = 0xAAAA;
  bondForwardStatus(kPorch, st);
  check(count(151) == 0, "no status before the master accepted");
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);
  check(count(151, kHub) == 1, "the partner's status goes to the master on acceptance");
  espnowfwd::StatusFrame got = statusFrame(gSent.back().bytes);
  check(got.presentMask == (tof | imu) && got.streamingMask == imu && got.bootTag == 0xAAAA && got.fwdLink == 1,
        "with the forwarded sensors only (input left out), X's boot tag and the link number");
  answer(kSpare, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1, kPorch,
         espnowfwd::kStreamBattery | espnowfwd::kStreamEvents, 0);
  check(count(151, kSpare) == 0, "never to the backup master (no readings there)");
  // A refresh acceptance does not send it again; a new status does.
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);
  check(count(151) == 1, "an acceptance of a refresh sends nothing more");
  st.streamingMask = tof;
  bondForwardStatus(kPorch, st);
  check(count(151, kHub) == 2 && statusFrame(gSent.back().bytes).streamingMask == tof, "a new status goes on at once");
  // From another device than the link's partner: not sent on.
  bondForwardStatus(kShed, st);
  check(count(151) == 2, "another device's status is not filed under the link");

  // Sensor requests. The accepted primary asks for ToF with a lease: this
  // device switches ToF on at the partner over the bond link.
  gCtrl.clear();
  st.streamingMask = 0;
  bondForwardStatus(kPorch, st);
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000);
  check(gCtrl.size() == 1 && ctrlCount(REMOTE_SENSOR_TOF, true) == 1 && sBf.req.applied == 1,
        "subscribe: ToF switched on at the partner");
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000);
  check(gCtrl.size() == 1 && sBf.req.applied == 2, "a renewal switches nothing again");
  // Its readings come back through the forwarding.
  gSent.clear();
  reading(kPorch, REMOTE_SENSOR_TOF, 1, "{\"mm\":250}");
  check(count(154, kHub) == 1, "the partner's ToF readings are forwarded to the master");
  // The lease ends: ToF off.
  run(31);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 1, "the lease ran out: ToF switched off at the partner");
  // Refusals, each counted and said once a minute.
  gConsole.clear();
  request(kSpare, espnowfwd::kReqSubscribe, tof, 30000);
  check(sBf.req.refused == 1 && sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::NotPrimaryMaster &&
        gConsole.size() == 1 && has(gConsole[0], "Sensor request from spare for the bond partner refused: only the master "
                                                 "this device forwards readings to may ask"),
        "the backup master may not ask");
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000, 2);
  check(sBf.req.refused == 2 && sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::OldLink && gConsole.size() == 1,
        "another link number: refused, said no more within the minute");
  request(kHub, espnowfwd::kReqSubscribe, input, 30000);
  check(sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::NotForwarded, "input is not forwarded: refused");
  request(kHub, 7, tof, 30000);
  check(sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::BadMode, "an unknown mode: refused");
  gPartnerOnline = false;
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000);
  check(sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::PartnerOffline, "the partner offline: refused");
  gPartnerOnline = true;
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 1, "no refused request switched anything");
  // A stream the partner runs already (its status says so) is not started by
  // the lease and goes on after it.
  st.streamingMask = imu;
  bondForwardStatus(kPorch, st);
  gCtrl.clear();
  request(kHub, espnowfwd::kReqSubscribe, imu, 5000);
  run(6);
  check(gCtrl.empty(), "IMU streams already: neither on nor off");
  // This device's user streams ToF (bondstream, saved): the lease's end leaves it on.
  gSavedStreams = (uint16_t)tof;
  request(kHub, espnowfwd::kReqSubscribe, tof, 5000);
  run(6);
  check(gCtrl.empty(), "the user's own stream: not touched by the lease");
  gSavedStreams = 0;
  // By hand during a lease: on makes it the user's; off ends the lease.
  request(kHub, espnowfwd::kReqSubscribe, tof, 5000);
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 1, "leased and switched on");
  bondForwardStreamByHand(REMOTE_SENSOR_TOF, true);
  run(6);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 0, "switched on by hand meanwhile: the lease's end leaves it on");
  gCtrl.clear();
  request(kHub, espnowfwd::kReqSubscribe, tof, 60000);
  bondForwardStreamByHand(REMOTE_SENSOR_TOF, false);
  check(sBf.req.leases.leased == 0, "stopped by hand: the lease ends with it");
  // Unsubscribe; a one-shot.
  gCtrl.clear();
  request(kHub, espnowfwd::kReqSubscribe, tof, 60000);
  request(kHub, espnowfwd::kReqUnsubscribe, tof, 0);
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 1 && ctrlCount(REMOTE_SENSOR_TOF, false) == 1 && sBf.req.leases.leased == 0,
        "unsubscribe: switched off at once");
  gCtrl.clear();
  request(kHub, espnowfwd::kReqOneshot, tof, 0, 1, 500);
  run(2);
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 1 && ctrlCount(REMOTE_SENSOR_TOF, false) == 0, "a one-shot holds 3 s");
  run(2);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 1, "then switches off");
  // The partner restarted (a new bond session): what a lease started is
  // switched on again; an "on" that could not be sent is sent again.
  gCtrl.clear();
  sessionUp(kPorch, 0x7001);
  request(kHub, espnowfwd::kReqSubscribe, tof, 60000);
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 1, "on");
  run(1);
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 1, "not again on the same session");
  sessionUp(kPorch, 0x7002);
  run(1);
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 2, "again on the partner's new session");
  st.streamingMask = 0;
  bondForwardStatus(kPorch, st);  // IMU no longer streams there
  gCtrlFails = true;              // the queue refuses the "on"
  request(kHub, espnowfwd::kReqSubscribe, imu, 60000);
  gCtrlFails = false;
  check(ctrlCount(REMOTE_SENSOR_IMU, true) == 0, "the 'on' could not be sent");
  run(1);
  check(ctrlCount(REMOTE_SENSOR_IMU, true) == 1, "it is sent again on the next tick");
  // Forwarding switched off: every lease ends, what they started is switched off.
  gCtrl.clear();
  gSetting = false;
  run(1);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 1 && sBf.req.leases.leased == 0, "forwarding off ends every lease");
  request(kHub, espnowfwd::kReqSubscribe, tof, 60000);
  check(sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::Off, "and refuses new requests");
  // A partner change ends the leases without telling the new partner anything.
  resetAll();
  upAndAccepted();
  sessionUp(kPorch, 0x7101);
  request(kHub, espnowfwd::kReqSubscribe, tof, 60000);
  gCtrl.clear();
  gBond.epoch = 9;
  std::memcpy(gBond.partner, kShed, 6);
  gBond.agreeState = espnowfwd::kAgreeUnknown;
  run(1);
  check(gCtrl.empty() && sBf.req.leases.leased == 0, "a new partner: the old leases end, nothing is sent to the new one");
  // The JSON and the bondstatus line.
  resetAll();
  upAndAccepted();
  request(kHub, espnowfwd::kReqSubscribe, tof, 60000);
  request(kSpare, espnowfwd::kReqSubscribe, tof, 60000);
  char json[espnowfwd::kStatusJsonCap];
  check(bondForwardStatusJson(json, sizeof(json)) > 0 &&
        has(json, "\"sensorRequests\":{\"applied\":1,\"refused\":1,\"lastRefused\":\"not_primary_master\","
                  "\"refusedBy\":{\"not_primary_master\":1},\"partnerLacks\":{\"count\":0,\"last\":[]},"
                  "\"leased\":[\"tof\"]}}"),
        std::string("the JSON: ") + json);
  char lines[espnowfwd::kStatusLinesCap];
  bondForwardStatusLines(lines, sizeof(lines));
  check(has(lines, "\n  Sensor requests for the partner: 1 applied, 1 refused (last: only the master "
                   "this device forwards readings to may ask); leased now: tof."), std::string("the lines: ") + lines);
}

// Review of 2026-11-18 (second look at decision 2): the "off" a lease owes
// is sent again until it went out; the user's streams by hand; leases end
// when their master leaves the primary mesh, another master is named, or
// their sensor is no longer forwarded; bondforward names a master of the
// primary mesh only.
static void testLeaseReview() {
  const uint32_t tof = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  const uint32_t imu = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  // A failed "off" (the partner out of range when the lease ran out) is sent
  // again once the partner is back, also after the link ended.
  resetAll();
  upAndAccepted();
  sessionUp(kPorch, 0x7201);
  request(kHub, espnowfwd::kReqSubscribe, (uint32_t)(tof | imu), 5000);
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 1 && ctrlCount(REMOTE_SENSOR_IMU, true) == 1,
        "two sensors asked for: both switched on (one control each)");
  gCtrl.clear();
  gCtrlFails = true;
  run(6);
  check(gCtrl.empty() && sBf.req.leases.leased == 0 && sBf.req.leases.offOwed == (tof | imu),
        "the lease ran out and the 'off' could not be sent: still owed");
  gCtrlFails = false;
  gPartnerOnline = false;
  run(1);
  check(gCtrl.empty(), "nothing is sent while the partner is away");
  gPartnerOnline = true;
  run(1);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 1 && ctrlCount(REMOTE_SENSOR_IMU, false) == 1 &&
        sBf.req.leases.offOwed == 0, "the partner is back: the 'off' goes out");
  run(2);
  check(gCtrl.size() == 2, "once");
  // After the link ended (forwarding off) the owed "off" still goes out.
  request(kHub, espnowfwd::kReqSubscribe, tof, 60000);
  gCtrl.clear();
  gCtrlFails = true;
  gSetting = false;
  run(1);
  check(gCtrl.empty() && sBf.req.leases.offOwed == tof, "forwarding off: the 'off' could not be sent");
  gCtrlFails = false;
  run(1);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 1 && sBf.req.leases.offOwed == 0, "sent on the next tick");
  // A new partner, or this device no longer the bond master: an owed "off"
  // is dropped, nothing goes to anyone.
  gSetting = true;
  run(1);
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, sBf.link.linkGen);
  request(kHub, espnowfwd::kReqSubscribe, tof, 60000, sBf.link.linkGen);
  check(sBf.req.leases.leased == tof, "leased again on the new link");
  gCtrl.clear();
  gCtrlFails = true;
  gSetting = false;
  run(1);
  check(sBf.req.leases.offOwed == tof, "owed");
  gCtrlFails = false;
  gBond.isMaster = false;
  run(3);
  check(gCtrl.empty() && sBf.req.leases.offOwed == 0, "no longer the bond master: forgotten, nothing sent");

  // The user's stream by hand is never started or stopped by a lease, also
  // when nothing is saved and the partner's status does not show it yet.
  resetAll();
  upAndAccepted();
  bondForwardStreamByHand(REMOTE_SENSOR_IMU, true);
  request(kHub, espnowfwd::kReqSubscribe, imu, 5000);
  run(6);
  check(gCtrl.empty(), "IMU switched on by hand here: the lease neither starts nor stops it");
  bondForwardStreamByHand(REMOTE_SENSOR_IMU, false);
  request(kHub, espnowfwd::kReqSubscribe, imu, 5000);
  check(ctrlCount(REMOTE_SENSOR_IMU, true) == 1, "stopped by hand: the next lease starts it");

  // The master that asked leaves the primary mesh: its leases end at once.
  resetAll();
  upAndAccepted();
  request(kHub, espnowfwd::kReqSubscribe, (uint32_t)(tof | imu), 600000);
  gCtrl.clear();
  gOtherMesh.push_back("02:C0:00:00:00:10");
  run(1);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 1 && ctrlCount(REMOTE_SENSOR_IMU, false) == 1 &&
        sBf.req.leases.leased == 0, "the master left the primary mesh: its leases end, the sensors are switched off");
  // 'bondforward sensors' no longer forwards ToF: its lease ends, IMU's not.
  resetAll();
  upAndAccepted();
  request(kHub, espnowfwd::kReqSubscribe, (uint32_t)(tof | imu), 600000);
  gCtrl.clear();
  gMask = (uint16_t)(espnowfwd::kSensorMaskDefault & ~tof);
  run(1);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 1 && ctrlCount(REMOTE_SENSOR_IMU, false) == 0 &&
        sBf.req.leases.leased == imu, "ToF no longer forwarded: its lease ends");
  // Another master is named as primary: the leases of the old one end.
  resetAll();
  upAndAccepted();
  request(kHub, espnowfwd::kReqSubscribe, tof, 600000);
  gCtrl.clear();
  std::memcpy(gTargets.primary, kHub2, 6);
  sessionUp(kHub2, 0x3333);
  run(1);
  check(ctrlCount(REMOTE_SENSOR_TOF, false) == 1 && sBf.req.leases.leased == 0,
        "another master named: the old one's leases end");

  // bondforward: a primary named in another mesh is not the one waited for.
  resetAll();
  gOtherMesh.push_back("02:C0:00:00:00:10");
  gSetting = true;
  run(1);
  check(describe("") == "OK: forwarding is on, but nothing is forwarded yet: waiting for spare to accept.",
        "bondforward waits for the backup in the primary mesh, not the master in another mesh: " + describe(""));
}

// Bench 2026-11-18: a forwarded request leased ToF at a worker whose build
// has none. The partner's own sensor list decides: a sensor it does not list
// is never leased, counted as partner_lacks_sensor; before the list came
// nothing is guessed (partner_status_unknown).
static void testPartnerSensorList() {
  const uint32_t tof = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  const uint32_t imu = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  // No list from the partner yet.
  resetAll();
  gSetting = true;
  run(1);
  answer(kHub, espnowfwd::kOpAnnounce, espnowfwd::kResultAccepted, 1);
  gCtrl.clear();
  gConsole.clear();
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000);
  check(gCtrl.empty() && sBf.req.leases.leased == 0 && sBf.req.refused == 1 &&
            sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::PartnerStatusUnknown &&
            sBf.req.refusedBy[(size_t)espnowfwd::ReqVerdict::PartnerStatusUnknown] == 1,
        "no sensor list from the partner yet: refused, nothing switched");
  check(gConsole.size() == 1 &&
            has(gConsole[0], "Sensor request from hub for the bond partner refused: the partner has not said yet which "
                             "sensors it has (0 more in the last minute)"),
        "said on the console: " + (gConsole.empty() ? std::string() : gConsole[0]));
  // The bench worker: it lists no sensor at all.
  espnowfwd::StatusFrame st{};
  bondForwardStatus(kPorch, st);
  gConsole.clear();
  gNowMs += 61000;  // a new minute for the console line
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000);
  check(gCtrl.empty() && sBf.req.leases.leased == 0 &&
            sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::PartnerLacksSensor &&
            sBf.req.refusedBy[(size_t)espnowfwd::ReqVerdict::PartnerLacksSensor] == 1 && sBf.req.lacking == 1 &&
            sBf.req.lastLacking == tof && sBf.req.applied == 0,
        "the partner lists no ToF: refused partner_lacks_sensor, no lease");
  check(gConsole.size() == 1 &&
            has(gConsole[0], "refused: the partner does not have that sensor running (tof) (0 more in the last minute)"),
        "the console names the sensor: " + (gConsole.empty() ? std::string() : gConsole[0]));
  // The partner has IMU only: of ToF and IMU, IMU is leased, ToF left out.
  st.presentMask = imu;
  bondForwardStatus(kPorch, st);
  gConsole.clear();
  request(kHub, espnowfwd::kReqSubscribe, tof | imu, 30000);
  check(ctrlCount(REMOTE_SENSOR_IMU, true) == 1 && ctrlCount(REMOTE_SENSOR_TOF, true) == 0 &&
            sBf.req.leases.leased == imu && sBf.req.applied == 1 && sBf.req.lacking == 2 && sBf.req.lastLacking == tof,
        "IMU leased, ToF left out and counted");
  check(gConsole.size() == 1 &&
            has(gConsole[0], "Sensor request from hub for the bond partner: tof left out: the partner does not have that "
                             "sensor running (0 more in the last minute)"),
        "the left-out sensor is said: " + (gConsole.empty() ? std::string() : gConsole[0]));
  // An unsubscribe needs no list.
  request(kHub, espnowfwd::kReqUnsubscribe, tof | imu, 0);
  check(ctrlCount(REMOTE_SENSOR_IMU, false) == 1 && sBf.req.leases.leased == 0 && sBf.req.lacking == 2,
        "unsubscribe: IMU off, nothing counted as lacking");
  // bondstatus, bondforward and the JSON say it.
  char lines[espnowfwd::kStatusLinesCap];
  bondForwardStatusLines(lines, sizeof(lines));
  check(has(lines, "\n  Sensor requests for the partner: 2 applied, 2 refused (last: the partner does not have that "
                   "sensor running); leased now: none.\n  Sensors the partner does not have running, asked for 2 times "
                   "(last: tof): never leased; 1 requests came before its sensor list was known."),
        std::string("bondstatus: ") + lines);
  const std::string fwd = describe("");
  check(fwd.rfind("OK: forwarding is on", 0) == 0 && has(fwd, ".\n  Sensor requests for the partner: 2 applied, 2 refused") &&
            has(fwd, "\n  Sensors the partner does not have running, asked for 2 times (last: tof)"),
        "bondforward: " + fwd);
  char json[espnowfwd::kStatusJsonCap];
  check(bondForwardStatusJson(json, sizeof(json)) > 0 &&
            has(json, "\"refusedBy\":{\"partner_status_unknown\":1,\"partner_lacks_sensor\":1},"
                      "\"partnerLacks\":{\"count\":2,\"last\":[\"tof\"]}"),
        std::string("the JSON: ") + json);
  // The partner starts its ToF later and says so (its next list): the next
  // request for it is leased; nothing of the earlier refusals sticks.
  st.presentMask = tof | imu;
  bondForwardStatus(kPorch, st);
  gCtrl.clear();
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000);
  check(ctrlCount(REMOTE_SENSOR_TOF, true) == 1 && sBf.req.leases.leased == tof && sBf.req.applied == 3 &&
            sBf.req.lacking == 2,
        "a sensor the partner lists later is leased");
  // ... and stops it again: the next request is refused once more.
  request(kHub, espnowfwd::kReqUnsubscribe, tof, 0);
  st.presentMask = imu;
  bondForwardStatus(kPorch, st);
  gCtrl.clear();
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000);
  check(gCtrl.empty() && sBf.req.leases.leased == 0 && sBf.req.lacking == 3 &&
            sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::PartnerLacksSensor,
        "and one it no longer lists is refused again");
  // A list from another device (not the partner) is not the partner's.
  static const uint8_t kStranger[6] = { 0x02, 0x99, 0, 0, 0, 1 };
  st.presentMask = tof | imu;
  bondForwardStatus(kStranger, st);
  request(kHub, espnowfwd::kReqSubscribe, tof, 30000);
  check(gCtrl.empty() && sBf.req.lastRefused == (uint8_t)espnowfwd::ReqVerdict::PartnerStatusUnknown,
        "a list from another device does not count as the partner's");
}

int main() {
  testOffAndReasons();
  testAnnounceAndForward();
  testSessionsAndChanges();
  testReviewCases();
  testPrimaryMesh();
  testStatusAndRequests();
  testLeaseReview();
  testPartnerSensorList();
  check(gLockViolations == 0, "nothing that may block, print or send ran under the lock");
  check(gLockDepth == 0, "every lock was released");
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d bond forward checks failed\n", failures, checks);
    return 1;
  }
  std::printf("ESP-NOW forwarding, bond master side: %d checks passed\n", checks);
  return 0;
}
