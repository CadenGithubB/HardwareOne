// Host harness for battery state and events over the mesh: the production
// code of System_ESPNow_PowerEvents.cpp between POWER_EVENTS_HOST_BEGIN and
// POWER_EVENTS_HOST_END (the sender step on espnow_task, the two receive
// paths, the stores, espnowbattery and espnowevents with their json forms),
// compiled as it is with the production rules header
// System_ESPNow_PowerEventCore.h, the production WarnLimiter
// (System_ESPNow_HandshakeBackoff.h), the MAC text helpers (System_MacText.h),
// SensorReceiverTargets of
// System_ESPNow_Sensors.h and PowerEventsSendView of System_ESPNow_PowerEvents.h.
//
// Host boundaries, all in this file (the pe* functions the production block
// reaches the device through): the uptime, the clock, the settings, the
// battery, the event ring, the paired devices and their mesh, the sessions,
// the radio queue with its acknowledgement watch, the allocator, the console
// and the reply buffer. The locks record whether anything that may block,
// print or allocate runs under them.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "System_ESPNow_HandshakeBackoff.h"
#include "System_ESPNow_PowerEventCore.h"
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
static const char kCommandReplyOutOfMemory[] = "Error: out of memory";

// ---- Locks ---------------------------------------------------------------------
static int gLockDepth = 0;
static unsigned gLockViolations = 0;
#define PE_LOCK()   (++gLockDepth)
#define PE_UNLOCK() (--gLockDepth)
static void notUnderLock() {
  if (gLockDepth != 0) ++gLockViolations;
}

static constexpr uint8_t kPePeerSlots = 16;

// INSERT_TARGETS_STRUCT

// INSERT_VIEW_STRUCT

// ---- The event ring of this device, as far as its reader is concerned -------------
#define SYSEVT_SUBJECT_LEN 48
#define SYSEVT_DETAIL_LEN 80
#define SYSEVT_WHO_LEN 24
struct SystemEvent {
  uint32_t seq;
  uint32_t tsMs;
  uint8_t kind;
  uint8_t source;
  char who[SYSEVT_WHO_LEN];
  char subject[SYSEVT_SUBJECT_LEN];
  char detail[SYSEVT_DETAIL_LEN];
};
static constexpr size_t kRingSize = 48;
static std::vector<SystemEvent> gRing;  // the live events, oldest first
static uint32_t gRingSeq = 0;
// Nothing here may post into this ring: a received event stays in the list
// of events from other devices. (The driver also requires that the production
// file never names this function.)
static unsigned gLocalEventPosts = 0;
[[maybe_unused]] static void systemEventPost(uint8_t, const char* = nullptr, const char* = nullptr, uint8_t = 0xFF,
                                             const char* = nullptr) {
  ++gLocalEventPosts;
}

// ---- What the production block asks of the device -----------------------------------
static uint32_t gNowMs = 0;
static uint32_t peUptimeMs() { return gNowMs; }
static bool gClockSet = false;
static int64_t gEpochMs = 0;
static uint8_t gClockSource = 0;  // 0 none, 2 ntp, 6 mesh
static bool peClockSet() { notUnderLock(); return gClockSet; }
static int64_t peEpochMs() { notUnderLock(); return gEpochMs; }
static uint8_t peClockSource() { return gClockSource; }
static const char* peClockKey(int64_t arrivalEpochMs, uint8_t source) {
  if (arrivalEpochMs == 0) return "none";
  switch (source) {
    case 0: return "carried";
    case 2: return "ntp";
    case 6: return "mesh";
    default: return "other";
  }
}
static void peFormatLocal(int64_t epochMs, char* out, size_t cap) {
  std::snprintf(out, cap, "local(%lld) UTC+1:00", (long long)(epochMs / 1000));
}
static uint8_t gRole = 0;
static bool gMeshOn = true;
static uint32_t gIntervalS = 60;
static uint8_t gFloor = espnowsensor::kEventPushAlert;
static uint32_t gBootCounterValue = 77;
static uint8_t peRole() { return gRole; }
static bool peMeshOn() { return gMeshOn; }
static uint32_t pePowerIntervalS() { return gIntervalS; }
static uint8_t peEventFloor() { return gFloor; }
static uint32_t peBootCounter() { return gBootCounterValue; }
static uint16_t peBootTag() { return 0xA2B3; }
static bool gBatteryInBuild = true;
static espnowsensor::PowerInputs gBattery = {};
static bool peBatteryInBuild() { return gBatteryInBuild; }
static void peBatteryInputs(espnowsensor::PowerInputs* in, uint32_t) { *in = gBattery; }
static SensorReceiverTargets gTargets = {};
static void peTargets(SensorReceiverTargets* out) { *out = gTargets; }

struct TestSession {
  uint8_t mac[6];
  bool up;
  uint32_t establishedAtMs;
  bool masterHasIt;       // the other end still has this session (it acknowledges)
  uint8_t unackedRun;     // watched frames in a row that nobody acknowledged
};
static std::vector<TestSession> gSessions;
static TestSession* sessionOf(const uint8_t* mac) {
  for (auto& s : gSessions) {
    if (std::memcmp(s.mac, mac, 6) == 0) return &s;
  }
  return nullptr;
}
static bool peSessionUp(const uint8_t mac[6]) {
  notUnderLock();
  const TestSession* s = sessionOf(mac);
  return s && s->up;
}
static bool peSessionSettled(const uint8_t mac[6], uint32_t nowMs) {
  notUnderLock();
  const TestSession* s = sessionOf(mac);
  return s && s->up && (uint32_t)(nowMs - s->establishedAtMs) >= 2000;
}
static std::vector<std::string> gHandshakes;
static bool gHandshakeBringsSession = false;
static std::string macText(const uint8_t* mac) {
  char text[18];
  macToDisplay(mac, text, sizeof(text));
  return text;
}
static bool peHandshake(const uint8_t mac[6]) {
  notUnderLock();
  gHandshakes.push_back(macText(mac));
  if (gHandshakeBringsSession) {
    TestSession* s = sessionOf(mac);
    if (s) {
      s->up = true;
      s->establishedAtMs = gNowMs;
      s->masterHasIt = true;
      s->unackedRun = 0;
    }
  }
  return true;
}

struct SentFrame {
  bool power;
  uint8_t dst[6];
  std::vector<uint8_t> bytes;
};
static std::vector<SentFrame> gSent;
static bool gQueueFull = false;
// The acknowledgement watch, as sendStatusRegister / sendStatusSweep do it: a
// watched frame that is not acknowledged within 10 s counts against the
// session, and two in a row clear it.
struct Watched {
  uint8_t mac[6];
  uint32_t atMs;
  bool acked;
  bool judged;
};
static std::vector<Watched> gWatched;
static void sweepWatch(uint32_t nowMs) {
  for (auto& w : gWatched) {
    if (w.judged) continue;
    TestSession* s = sessionOf(w.mac);
    if (w.acked) {
      w.judged = true;
      if (s) s->unackedRun = 0;
    } else if ((uint32_t)(nowMs - w.atMs) >= 10000) {
      w.judged = true;
      if (s && s->up && ++s->unackedRun >= 2) s->up = false;
    }
  }
}
static bool peSendPower(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  notUnderLock();
  if (gQueueFull) return false;
  SentFrame f{};
  f.power = true;
  std::memcpy(f.dst, mac, 6);
  f.bytes.assign(payload, payload + len);
  gSent.push_back(f);
  Watched w{};
  std::memcpy(w.mac, mac, 6);
  w.atMs = gNowMs;
  const TestSession* s = sessionOf(mac);
  w.acked = s && s->masterHasIt;
  gWatched.push_back(w);
  return true;
}
static bool peSendEvent(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  notUnderLock();
  if (gQueueFull) return false;
  SentFrame f{};
  f.power = false;
  std::memcpy(f.dst, mac, 6);
  f.bytes.assign(payload, payload + len);
  gSent.push_back(f);
  return true;
}
// One event after *cursor, oldest first; *skipped gains what the ring
// overwrote, and so does the ring's own count of silently lost events, as in
// systemEventFetchSince. gRingRace: that many more events are overwritten
// between the reader's look at the oldest number and its fetch.
static uint32_t gRingSilentLoss = 0;
static unsigned gRingRace = 0;
static int peEventFetch(uint32_t* cursor, SystemEvent* out, uint32_t* skipped) {
  notUnderLock();
  for (; gRingRace > 0 && gRing.size() > 1; --gRingRace) gRing.erase(gRing.begin());
  gRingRace = 0;
  if (gRing.empty()) return 0;
  if (*cursor == 0xFFFFFFFFu) *cursor = gRingSeq;
  const uint32_t oldest = gRing.front().seq;
  if (*cursor + 1 < oldest) {
    if (skipped) *skipped += oldest - *cursor - 1;
    gRingSilentLoss += oldest - *cursor - 1;
    *cursor = oldest - 1;
  }
  for (const SystemEvent& e : gRing) {
    if (e.seq > *cursor) {
      *out = e;
      *cursor = e.seq;
      return 1;
    }
  }
  return 0;
}
static uint32_t peEventLatestSeq() { return gRingSeq; }
static uint32_t peEventOldestSeq() { return gRing.empty() ? gRingSeq + 1 : gRing.front().seq; }
// Kinds of the harness: 1 alert, 2 standard, 3 verbose, 4 alert with the longest name.
static uint8_t peKindTier(uint8_t kind) {
  return kind == 2 ? espnowsensor::kEventTierStandard
       : (kind == 3 ? espnowsensor::kEventTierVerbose : espnowsensor::kEventTierAlert);
}
static const char* peKindName(uint8_t kind) {
  switch (kind) {
    case 1: return "login_fail";
    case 2: return "peer_online";
    case 3: return "setting_changed";
    case 4: return "ota_credential_changed_x25";
    case 5: return "a_kind_name_longer_than_the_32_byte_field";
    default: return "?";
  }
}
struct PairedPeer {
  uint8_t mac[6];
  uint8_t mesh;
  std::string name;
};
static std::vector<PairedPeer> gPaired;
static const PairedPeer* pairedOf(const uint8_t* mac) {
  for (const auto& p : gPaired) {
    if (std::memcmp(p.mac, mac, 6) == 0) return &p;
  }
  return nullptr;
}
static bool peIsPaired(const uint8_t mac[6]) { notUnderLock(); return pairedOf(mac) != nullptr; }
static uint8_t peMeshOf(const uint8_t mac[6]) {
  notUnderLock();
  const PairedPeer* p = pairedOf(mac);
  return p ? p->mesh : 0;
}
static unsigned gAllocs = 0;
static bool gAllocFails = false;
static void* peAlloc(size_t bytes, const char*) {
  notUnderLock();
  ++gAllocs;
  return gAllocFails ? nullptr : std::malloc(bytes);
}
static bool peResolve(const char* who, uint8_t mac[6]) {
  for (const auto& p : gPaired) {
    if (p.name == who || macText(p.mac) == who) {
      std::memcpy(mac, p.mac, 6);
      return true;
    }
  }
  return false;
}
static std::vector<std::string> gConsole;
static void peConsole(const char* fmt, ...) {
  notUnderLock();
  char line[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  gConsole.push_back(line);
}
// The user the command runs for (currentExecIsAdmin on the device).
static bool gCallerAdmin = true;
static bool peCallerIsAdmin() { return gCallerAdmin; }
static char gReply[4096];
static size_t gReplyCap = sizeof(gReply);  // a case may hand out less of it
static bool gReplyFails = false;
static char* peReply(size_t* cap) {
  if (gReplyFails) return nullptr;
  *cap = gReplyCap;
  // A reply that ran over its buffer would write here: ASan sees it.
  std::memset(gReply, 0x7E, sizeof(gReply));
  return gReply;
}

// INSERT_PRODUCTION

// ---- Cases -----------------------------------------------------------------------------
static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}
static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
static size_t count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++n;
  return n;
}

// A JSON syntax check: one value, nothing after it, no control byte in a string.
struct JsonCheck {
  const std::string& s;
  size_t i = 0;
  explicit JsonCheck(const std::string& text) : s(text) {}
  void space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) ++i; }
  bool literal(const char* word) {
    const size_t n = std::strlen(word);
    if (s.compare(i, n, word) != 0) return false;
    i += n;
    return true;
  }
  bool string() {
    if (i >= s.size() || s[i] != '"') return false;
    for (++i; i < s.size(); ++i) {
      const unsigned char ch = (unsigned char)s[i];
      if (ch < 0x20) return false;
      if (ch == '\\') {
        if (++i >= s.size() || !std::strchr("\"\\/bfnrtu", s[i])) return false;
      } else if (ch == '"') {
        ++i;
        return true;
      }
    }
    return false;
  }
  bool number() {
    const size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
    if (digits == 0) return false;
    if (i < s.size() && s[i] == '.') {
      ++i;
      digits = 0;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
      if (digits == 0) return false;
    }
    return i > start;
  }
  bool value() {
    space();
    if (i >= s.size()) return false;
    if (s[i] == '{') {
      ++i;
      space();
      if (i < s.size() && s[i] == '}') { ++i; return true; }
      for (;;) {
        space();
        if (!string()) return false;
        space();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        if (!value()) return false;
        space();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        return false;
      }
    }
    if (s[i] == '[') {
      ++i;
      space();
      if (i < s.size() && s[i] == ']') { ++i; return true; }
      for (;;) {
        if (!value()) return false;
        space();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        return false;
      }
    }
    if (s[i] == '"') return string();
    if (literal("true") || literal("false") || literal("null")) return true;
    return number();
  }
};
static bool jsonValid(const std::string& text) {
  JsonCheck c(text);
  if (!c.value()) return false;
  c.space();
  return c.i == text.size();
}

static const uint8_t kHub[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x10 };
static const uint8_t kBackup[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x11 };
static const uint8_t kPorch[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t kShed[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x02 };

static void pair(const uint8_t mac[6], const char* name, uint8_t mesh = 0) {
  PairedPeer p{};
  std::memcpy(p.mac, mac, 6);
  p.mesh = mesh;
  p.name = name;
  gPaired.push_back(p);
}
static void addSession(const uint8_t mac[6], bool up, uint32_t establishedAtMs) {
  TestSession s{};
  std::memcpy(s.mac, mac, 6);
  s.up = up;
  s.establishedAtMs = establishedAtMs;
  s.masterHasIt = up;
  gSessions.push_back(s);
}
static void nameMasters(bool primary, bool backup) {
  gTargets = SensorReceiverTargets{};
  gTargets.masterNamed = primary;
  gTargets.backupNamed = backup;
  gTargets.havePrimary = primary;
  gTargets.haveBackup = backup;
  std::memcpy(gTargets.primary, kHub, 6);
  std::memcpy(gTargets.backup, kBackup, 6);
}
static void resetAll() {
  for (auto& list : sPe.lists) std::free(list);
  std::free(sPe.peers);
  std::memset((void*)&sPe, 0, sizeof(sPe));
  gNowMs += 10000000;  // every case starts later than the last one ended
  gClockSet = false;
  gEpochMs = 86400000;  // what a clock that is not set reads (1970): never a stamp
  gReplyCap = sizeof(gReply);
  gClockSource = 0;
  gRole = 0;
  gMeshOn = true;
  gIntervalS = 60;
  gFloor = espnowsensor::kEventPushAlert;
  gBatteryInBuild = true;
  gBattery = espnowsensor::PowerInputs{};
  gBattery.status = 3;
  gBattery.backend = espnowsensor::kPowerBackendAdc;
  gBattery.hasSample = true;
  gBattery.sampleAgeMs = 800;
  gBattery.voltageValid = true;
  gBattery.voltage = 3.912f;
  gBattery.percentageValid = gBattery.percentageEstimated = true;
  gBattery.percentage = 71.5f;
  gBattery.usbKnown = true;
  gBattery.statusEstimated = true;
  nameMasters(true, false);
  gSessions.clear();
  gHandshakes.clear();
  gHandshakeBringsSession = false;
  gSent.clear();
  gWatched.clear();
  gQueueFull = false;
  gRing.clear();
  gRingSeq = 0;
  gRingSilentLoss = 0;
  gRingRace = 0;
  gPaired.clear();
  pair(kHub, "hub");
  pair(kBackup, "spare");
  pair(kPorch, "porch");
  pair(kShed, "shed");
  gAllocFails = false;
  gReplyFails = false;
  gConsole.clear();
}
// Runs the task step once a second for `seconds`.
static void run(uint32_t seconds) {
  for (uint32_t i = 0; i < seconds; ++i) {
    gNowMs += 1000;
    sweepWatch(gNowMs);
    espnowPowerEventsTick(gNowMs);
  }
}
static void post(uint8_t kind, const char* subject, const char* detail, const char* who, uint8_t source = 3,
                 uint32_t aheadMs = 0) {
  SystemEvent e{};
  e.seq = ++gRingSeq;
  e.tsMs = gNowMs + aheadMs;
  e.kind = kind;
  e.source = source;
  std::snprintf(e.subject, sizeof(e.subject), "%s", subject);
  std::snprintf(e.detail, sizeof(e.detail), "%s", detail);
  std::snprintf(e.who, sizeof(e.who), "%s", who);
  gRing.push_back(e);
  if (gRing.size() > kRingSize) gRing.erase(gRing.begin());
}
static espnowsensor::PowerFrame powerOf(const SentFrame& f) {
  espnowsensor::PowerFrame p{};
  espnowsensor::parsePowerFrame(f.bytes.data(), f.bytes.size(), p);
  return p;
}
static espnowsensor::EventFrame eventOf(const SentFrame& f) {
  espnowsensor::EventFrame e{};
  espnowsensor::parseEventFrame(f.bytes.data(), f.bytes.size(), e);
  return e;
}
static std::string field(const char* text, size_t len) {
  std::vector<char> out(len + 1);
  espnowsensor::wireTextCopy(text, len, out.data(), out.size());
  return out.data();
}
static std::string cmdBattery(const char* args) { return cmd_espnow_battery(String(args)); }
static std::string cmdEvents(const char* args) { return cmd_espnow_events(String(args)); }
static PeCollectPeer* peerOf(const uint8_t mac[6]) {
  for (uint8_t i = 0; sPe.peers && i < kPePeerSlots; ++i) {
    if (sPe.peers[i].inUse && std::memcmp(sPe.peers[i].mac, mac, 6) == 0) return &sPe.peers[i];
  }
  return nullptr;
}
static std::vector<uint8_t> powerBytes(uint16_t counter, uint16_t bootTag, uint16_t percentX10, uint16_t known,
                                       uint8_t status = 3) {
  espnowsensor::PowerFrame f{};
  f.counter = counter;
  f.bootTag = bootTag;
  f.status = status;
  f.backend = espnowsensor::kPowerBackendAdc;
  f.known = known;
  f.percentX10 = percentX10;
  f.voltageMv = 3912;
  f.sampleAgeMs = 800;
  std::vector<uint8_t> bytes(espnowsensor::kPowerFrameLen);
  espnowsensor::packPowerFrame(bytes.data(), f);
  return bytes;
}
static std::vector<uint8_t> eventBytes(uint16_t counter, uint16_t bootTag, const char* kind, const char* subject,
                                       const char* detail = "", const char* who = "", uint16_t skipped = 0,
                                       uint32_t eventSeq = 0) {
  espnowsensor::EventFrame f{};
  f.counter = counter;
  f.bootTag = bootTag;
  f.tier = espnowsensor::kEventTierAlert;
  f.source = espnowsensor::kEventSourceWeb;
  f.skipped = skipped;
  f.eventSeq = eventSeq ? eventSeq : counter;
  f.ageMs = 120;
  espnowsensor::eventFieldPut(f.kind, sizeof(f.kind), kind, std::strlen(kind));
  espnowsensor::eventFieldPut(f.subject, sizeof(f.subject), subject, std::strlen(subject));
  espnowsensor::eventFieldPut(f.detail, sizeof(f.detail), detail, std::strlen(detail));
  espnowsensor::eventFieldPut(f.who, sizeof(f.who), who, std::strlen(who));
  std::vector<uint8_t> bytes(espnowsensor::kEventFrameLen);
  espnowsensor::packEventFrame(bytes.data(), f);
  return bytes;
}
static void rxPower(const uint8_t mac[6], const char* name, const std::vector<uint8_t>& bytes) {
  powerEventsIngestPower(mac, name, bytes.data(), bytes.size());
}
static void rxEvent(const uint8_t mac[6], const char* name, const std::vector<uint8_t>& bytes) {
  powerEventsIngestEvent(mac, name, bytes.data(), bytes.size());
}

// ---- The battery sender -------------------------------------------------------------------
static void testPowerSend() {
  resetAll();
  nameMasters(true, true);
  addSession(kHub, true, gNowMs - 5000);
  addSession(kBackup, true, gNowMs - 5000);
  run(1);
  check(gSent.size() == 2 && gSent[0].power && gSent[1].power, "the first battery frame goes out at once, to both named masters");
  check(std::memcmp(gSent[0].dst, kHub, 6) == 0 && std::memcmp(gSent[1].dst, kBackup, 6) == 0, "the primary and the backup");
  check(gSent[0].bytes.size() == 24 && gSent[0].bytes == gSent[1].bytes, "24 bytes, the same frame (one counter value) to both");
  espnowsensor::PowerFrame p = powerOf(gSent[0]);
  check(p.counter == 1 && p.bootTag == 0xA2B3 && p.status == 3 && p.backend == 1 && p.voltageMv == 3912 &&
        p.percentX10 == 715 && p.sampleAgeMs == 800, "counter 1, the boot tag, and the battery values");
  check(p.known == (espnowsensor::kPowerVoltageValid | espnowsensor::kPowerPercentValid | espnowsensor::kPowerPercentEstimated |
                    espnowsensor::kPowerUsbKnown | espnowsensor::kPowerStatusEstimated), "the valid bits of that snapshot");
  check(gWatched.size() == 2, "each frame's acknowledgement is watched");
  // Nothing more until the interval has passed.
  run(58);
  check(gSent.size() == 2, "nothing within the interval");
  run(2);
  check(gSent.size() == 4 && powerOf(gSent[2]).counter == 2, "the next one after 60 s, counter 2");
  // A change of state sends at once, but not within 5 s of the last frame.
  run(1);
  gBattery.usbPresent = true;
  run(3);
  check(gSent.size() == 4, "a change within 5 s of the last frame waits");
  run(2);
  check(gSent.size() == 6 && (powerOf(gSent[4]).known & espnowsensor::kPowerUsbPresent), "and goes out when the 5 s have passed");
  gBattery.status = 1;
  gBattery.chargingKnown = gBattery.charging = true;
  run(5);
  check(gSent.size() == 8 && powerOf(gSent[6]).status == 1, "a change of status sends as well");
  // A value that only moves does not.
  gBattery.voltage = 4.0f;
  gBattery.percentage = 80.0f;
  run(30);
  check(gSent.size() == 8, "a voltage that moves is no reason to send early");
  // Interval 0: nothing.
  gIntervalS = 0;
  gBattery.status = 2;
  run(200);
  check(gSent.size() == 8, "interval 0: no battery frame, not even on a change");
  PowerEventsSendView view;
  espnowPowerEventsSendView(&view);
  check(view.batteryWhy == espnowsensor::PushWhyNot::Off && view.batterySent == 4 && view.batteryEverSent,
        "the status view: switched off, four samples sent");

  // A device with the battery monitor but no valid sample still sends, with the valid bits clear.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gBattery = espnowsensor::PowerInputs{};
  gBattery.backend = espnowsensor::kPowerBackendFuelGauge;
  run(1);
  check(gSent.size() == 1 && powerOf(gSent[0]).known == 0 && powerOf(gSent[0]).sampleAgeMs == 65535 &&
        powerOf(gSent[0]).backend == 2, "no sample yet: a frame with nothing valid");

  // One target without a session: the frame goes to the other, and the one
  // without sees the gap as loss when it is back.
  resetAll();
  nameMasters(true, true);
  addSession(kHub, true, gNowMs - 5000);
  addSession(kBackup, false, 0);
  run(1);
  check(gSent.size() == 1 && std::memcmp(gSent[0].dst, kHub, 6) == 0, "only the master with a session gets the frame");
  check(gHandshakes.size() == 1 && gHandshakes[0] == macText(kBackup), "a session with the other one is started");
  sessionOf(kBackup)->up = true;
  sessionOf(kBackup)->establishedAtMs = gNowMs;
  sessionOf(kBackup)->masterHasIt = true;
  run(60);
  check(gSent.size() == 3 && powerOf(gSent[1]).counter == 2 && powerOf(gSent[2]).counter == 2 &&
        std::memcmp(gSent[2].dst, kBackup, 6) == 0, "the next sample goes to both, with the same counter");

  // A session that is not 2 s old yet is not used.
  resetAll();
  addSession(kHub, true, gNowMs);
  run(1);
  check(gSent.empty() && gHandshakes.empty(), "a session under 2 s old: nothing sent, no handshake either");
  run(2);
  check(gSent.size() == 1, "sent once the session has settled");

  // The queue takes nothing: no frame, and the next look tries again.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gQueueFull = true;
  run(3);
  espnowPowerEventsSendView(&view);
  check(gSent.empty() && view.batterySent == 0 && !view.batteryEverSent, "queue full: nothing counted as sent");
  gQueueFull = false;
  run(1);
  check(gSent.size() == 1 && powerOf(gSent[0]).counter == 1, "the next look sends, and no counter value was used up meanwhile");

  // Who does not send, and why.
  struct Case { const char* what; espnowsensor::PushWhyNot why; };
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gRole = 1;
  run(5);
  espnowPowerEventsSendView(&view);
  check(gSent.empty() && view.batteryWhy == espnowsensor::PushWhyNot::IsMaster && gHandshakes.empty(),
        "the acting master sends no battery state and starts no handshake for it");
  gRole = 2;
  run(2);
  check(gSent.size() == 1, "a backup master that has not taken over sends its own");
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gMeshOn = false;
  run(5);
  espnowPowerEventsSendView(&view);
  check(gSent.empty() && view.batteryWhy == espnowsensor::PushWhyNot::MeshOff, "mesh mode off: nothing");
  gMeshOn = true;
  gBatteryInBuild = false;
  run(5);
  espnowPowerEventsSendView(&view);
  check(gSent.empty() && view.batteryWhy == espnowsensor::PushWhyNot::NotInBuild, "no battery monitor in this firmware: nothing");
  gBatteryInBuild = true;
  gTargets = SensorReceiverTargets{};
  run(5);
  espnowPowerEventsSendView(&view);
  check(gSent.empty() && view.batteryWhy == espnowsensor::PushWhyNot::NoMaster && gHandshakes.empty(),
        "no master named: nothing is sent to anybody");
  gTargets.masterNamed = true;
  run(5);
  espnowPowerEventsSendView(&view);
  check(gSent.empty() && view.batteryWhy == espnowsensor::PushWhyNot::MasterNotPaired, "a master that is named but not paired: nothing");
}

// The worker whose master restarted where the heartbeat does not reach: its
// session is stale. Two battery frames that nobody acknowledges clear it, the
// next look starts a new one, and the frame after that arrives.
static void testStaleSessionHeals() {
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gHandshakeBringsSession = true;
  run(1);
  check(gSent.size() == 1 && gWatched.back().acked, "the first frame is acknowledged");
  const uint32_t lostAt = gNowMs;
  sessionOf(kHub)->masterHasIt = false;  // the master restarted
  run(60);
  check(gSent.size() == 2 && sessionOf(kHub)->up, "the next frame goes out into a session the master no longer has");
  run(60);
  check(gSent.size() == 3, "and one more");
  run(11);
  check(!gHandshakes.empty() && gHandshakes[0] == macText(kHub), "two unacknowledged frames: the session was cleared and a new one started");
  check(sessionOf(kHub)->up && sessionOf(kHub)->masterHasIt, "the worker has a session again");
  check((uint32_t)(sessionOf(kHub)->establishedAtMs - lostAt) <= 3 * 60000, "within three battery intervals");
  const size_t before = gSent.size();
  run(60);
  check(gSent.size() == before + 1 && gWatched.back().acked, "and the next battery frame arrives");
}

// ---- The event sender -------------------------------------------------------------------------
static void testEventSend() {
  resetAll();
  nameMasters(true, true);
  addSession(kHub, true, gNowMs - 5000);
  addSession(kBackup, true, gNowMs - 5000);
  gIntervalS = 0;  // events only
  // The floor: alert (the default) sends alerts only.
  post(3, "debug", "on", "admin");
  post(2, "porch", "online", "");
  post(1, "admin", "3 attempts", "1.2.3.4");
  gNowMs += 250;
  run(1);
  check(gSent.size() == 2 && !gSent[0].power && gSent[0].bytes.size() == 128 && gSent[0].bytes == gSent[1].bytes,
        "the alert goes to both named masters as one 128-byte frame; the others stay");
  espnowsensor::EventFrame e = eventOf(gSent[0]);
  check(e.counter == 1 && e.bootTag == 0xA2B3 && e.tier == 2 && e.source == 3 && e.eventSeq == 3 && e.skipped == 0 && e.flags == 0,
        "counter 1, tier alert, the source, the event's own number; events under the floor are not 'skipped'");
  check(e.ageMs == 1250, "the age of the event when it was sent");
  check(field(e.kind, 32) == "login_fail" && field(e.subject, 32) == "admin" && field(e.detail, 32) == "3 attempts" &&
        field(e.who, 12) == "1.2.3.4", "the kind as its name, and the three texts");
  for (size_t i = 20 + 10; i < 52; ++i) check(gSent[0].bytes[i] == 0, "the kind field is zero padded");
  check(gSent[0].bytes[7] == 0 && gSent[0].bytes[10] == 0 && gSent[0].bytes[11] == 0, "reserved bytes are 0");
  // standard: standard and alert; verbose: all.
  gSent.clear();
  gFloor = espnowsensor::kEventPushStandard;
  post(3, "debug", "off", "admin");
  post(2, "shed", "online", "");
  post(1, "root", "locked", "");
  run(1);
  check(gSent.size() == 4 && eventOf(gSent[0]).tier == 1 && eventOf(gSent[2]).tier == 2 && eventOf(gSent[2]).counter == 3,
        "standard: the standard and the alert event go out, the verbose one does not");
  gSent.clear();
  gFloor = espnowsensor::kEventPushVerbose;
  post(3, "debug", "on", "admin");
  run(1);
  check(gSent.size() == 2 && eventOf(gSent[0]).tier == 0, "verbose: every event goes out");
  // off: nothing, and the cursor follows the head, so nothing old is sent later.
  gSent.clear();
  gFloor = espnowsensor::kEventPushOff;
  post(1, "a", "b", "");
  run(2);
  gFloor = espnowsensor::kEventPushAlert;
  run(2);
  check(gSent.empty(), "events posted while the setting was off are not sent later");
  post(1, "c", "d", "");
  run(1);
  check(gSent.size() == 2 && eventOf(gSent[0]).skipped == 0 && field(eventOf(gSent[0]).subject, 32) == "c",
        "the next one is, with nothing counted as skipped");
  // A stored setting this firmware does not know sends nothing.
  gSent.clear();
  gFloor = 9;
  post(1, "e", "f", "");
  run(2);
  PowerEventsSendView view;
  espnowPowerEventsSendView(&view);
  check(gSent.empty() && view.eventsWhy == espnowsensor::PushWhyNot::Off && view.eventFloor == espnowsensor::kEventPushOff,
        "an unknown setting value is 'off'");

  // Long texts are cut with their flags and stay text; a kind name longer than the field too.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  post(1, "a-subject-that-is-longer-than-thirty-two-bytes!", "a detail that is far longer than the thirty-two bytes a frame has room for",
       "a-long-user-name-here");
  run(1);
  e = eventOf(gSent[0]);
  check(e.flags == (espnowsensor::kEventCutSubject | espnowsensor::kEventCutDetail | espnowsensor::kEventCutWho),
        "subject, detail and who were cut, and say so");
  check(field(e.subject, 32) == "a-subject-that-is-longer-than-th" && field(e.detail, 32) == "a detail that is far longer than" &&
        field(e.who, 12) == "a-long-user-", "each is cut at its field's length");
  post(4, "x", "y", "");
  run(1);
  e = eventOf(gSent[1]);
  check(e.flags == 0 && field(e.kind, 32) == "ota_credential_changed_x25", "a 26-character kind name fits whole");

  // The rate limit: eight at once, then one every 2 s; what does not fit is
  // counted into `skipped` of the next frame that goes out.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  for (int i = 0; i < 20; ++i) post(1, "burst", "x", "");
  run(1);
  check(gSent.size() == 4, "four events a look");
  run(1);
  check(gSent.size() == 8, "eight pass at once");
  run(1);
  check(gSent.size() == 9 && eventOf(gSent[8]).skipped == 0, "the ninth has to wait for a token (one every 2 s)");
  // The events the limit has no room for are left out, not kept back: of the
  // twenty, the tenth frame (a token came after 2 s more) carries the
  // seventeenth event and says seven were left out before it.
  run(2);
  check(gSent.size() == 10 && eventOf(gSent[9]).eventSeq == 17 && eventOf(gSent[9]).skipped == 7 &&
        eventOf(gSent[9]).counter == 10, "the next frame says how many were left out; the counter counts frames, not events");
  espnowPowerEventsSendView(&view);
  check(view.eventsSent == 10 && view.eventsLeftOut == 10, "10 sent, 10 left out");
  gNowMs += 10000;
  post(1, "after", "x", "");
  run(1);
  check(gSent.size() == 11 && eventOf(gSent[10]).skipped == 3 && field(eventOf(gSent[10]).subject, 32) == "after",
        "the three left out after it are in the frame after that");
  post(1, "again", "x", "");
  run(1);
  check(gSent.size() == 12 && eventOf(gSent[11]).skipped == 0, "and then it starts at 0 again");
  // `skipped` stops at 65535.
  sPe.eventSkippedPending = 70000;
  gNowMs += 20000;
  post(1, "many", "x", "");
  run(1);
  check(eventOf(gSent.back()).skipped == 65535, "skipped stops at 65535");

  // No target has a session: the cursor waits, and what the ring overwrites
  // meanwhile is counted into `skipped`.
  resetAll();
  addSession(kHub, false, 0);
  gIntervalS = 0;
  for (int i = 0; i < 60; ++i) post(1, "waiting", "x", "");  // the ring holds 48: 12 are overwritten
  run(3);
  check(gSent.empty() && gHandshakes.size() == 3 && sPe.eventCursor == 0, "no session: nothing sent, the cursor waits, a handshake is started");
  sessionOf(kHub)->up = true;
  sessionOf(kHub)->establishedAtMs = gNowMs;
  run(3);
  check(!gSent.empty() && eventOf(gSent[0]).skipped == 12 && eventOf(gSent[0]).eventSeq == 13,
        "with the session up the oldest event still held goes out and says 12 were lost before it");
  check(gRingSilentLoss == 0, "the wait is reported by this reader and not charged to the ring's count of silent loss");
  PowerEventsSendView waited;
  espnowPowerEventsSendView(&waited);
  check(waited.eventsLeftOut == 12 && waited.eventsSent == 8, "the status counts the 12 as left out, and the 8 that went out so far as sent");
  // The same with a full queue on the first try: the gap is still counted once.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  gFloor = espnowsensor::kEventPushOff;
  run(1);
  gFloor = espnowsensor::kEventPushAlert;
  gTargets = SensorReceiverTargets{};  // (cursor follows the head: nothing held back)
  nameMasters(true, false);
  sPe.eventCursor = 0;
  for (int i = 0; i < 60; ++i) post(1, "behind", "x", "");
  gQueueFull = true;
  run(3);
  gQueueFull = false;
  run(1);
  check(!gSent.empty() && eventOf(gSent[0]).skipped == 12 && eventOf(gSent[0]).eventSeq == 13 && gRingSilentLoss == 0,
        "a full queue while the reader is behind: the gap is counted once, not on every try");
  // Events overwritten between the reader's look and its fetch are counted too.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  for (int i = 0; i < 10; ++i) post(1, "race", "x", "");
  gRingRace = 3;
  run(1);
  check(!gSent.empty() && eventOf(gSent[0]).skipped == 3 && eventOf(gSent[0]).eventSeq == 4,
        "events the ring overwrote between the look and the fetch are in 'skipped' as well");

  // The queue takes nothing: the event waits, with its number.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  post(1, "held", "x", "");
  gQueueFull = true;
  run(3);
  check(gSent.empty() && sPe.eventCounter == 0 && sPe.eventCursor == 0, "queue full: no counter value taken, the cursor stays");
  gQueueFull = false;
  run(1);
  check(gSent.size() == 1 && eventOf(gSent[0]).counter == 1 && field(eventOf(gSent[0]).subject, 32) == "held",
        "the same event goes out on the next look, as counter 1");

  // The acting master sends none of its own; without a named master the cursor follows the head.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  gRole = 1;
  post(1, "mine", "x", "");
  run(2);
  check(gSent.empty() && sPe.eventCursor == 1, "the master sends no events of its own");
  gRole = 0;
  gTargets = SensorReceiverTargets{};
  post(1, "nobody", "x", "");
  run(2);
  check(gSent.empty() && sPe.eventCursor == 2 && gHandshakes.empty(), "no master named: nothing is sent and nothing is kept back");
}

// ---- Receiving: roles, stores, texts --------------------------------------------------------------
static void testReceiveRolesAndBattery() {
  // A worker refuses both frames, counts them and says so once a minute.
  resetAll();
  gRole = 0;
  rxPower(kPorch, "porch", powerBytes(1, 7, 715, espnowsensor::kPowerPercentValid));
  rxEvent(kPorch, "porch", eventBytes(1, 7, "login_fail", "admin"));
  rxEvent(kPorch, "porch", eventBytes(2, 7, "login_fail", "admin"));
  check(sPe.peers == nullptr && gAllocs == 0 && sPe.refused.notReceiver == 3, "a worker keeps neither frame and allocates nothing");
  check(gConsole.size() == 1 && has(gConsole[0], "Battery state and events from porch refused: this device is not a mesh master or backup master"),
        "and says so once");
  check(cmdBattery("") == "OK: this device keeps no battery state of other devices (only a mesh master or backup master does)",
        "espnowbattery on a worker says who keeps it");
  check(has(cmdEvents(""), "this device keeps no events of other devices"), "espnowevents likewise");

  // A master and a backup master keep them. The receiver stamps with its own clock.
  for (uint8_t role : { (uint8_t)1, (uint8_t)2 }) {
    resetAll();
    gRole = role;
    const unsigned allocs = gAllocs;
    const uint32_t t0 = gNowMs;
    rxPower(kPorch, "porch", powerBytes(1, 7, 715, espnowsensor::kPowerPercentValid | espnowsensor::kPowerVoltageValid));
    check(peerOf(kPorch) && peerOf(kPorch)->havePower && gAllocs == allocs + 1, "kept; the first frame makes the records");
    check(peerOf(kPorch)->powerEpochMs == 0 && peerOf(kPorch)->powerUptimeMs == t0, "clock not set: epoch 0, the uptime is the stamp");
    gClockSet = true;
    gEpochMs = 1763388202417LL;
    gClockSource = 2;
    gNowMs += 60000;
    rxPower(kPorch, "porch", powerBytes(2, 7, 700, espnowsensor::kPowerPercentValid));
    check(peerOf(kPorch)->powerEpochMs == 1763388202417LL && peerOf(kPorch)->power.percentX10 == 700 && gAllocs == allocs + 1,
          "clock set: the receiver's UTC epoch in ms");
  }

  // The counter rule: a gap is loss, an older frame never replaces a newer one, a restart is kept.
  resetAll();
  gRole = 1;
  rxPower(kPorch, "porch", powerBytes(1, 7, 715, espnowsensor::kPowerPercentValid));
  gNowMs += 1000;
  rxPower(kPorch, "porch", powerBytes(4, 7, 650, espnowsensor::kPowerPercentValid));
  gNowMs += 1000;
  rxPower(kPorch, "porch", powerBytes(3, 7, 999, espnowsensor::kPowerPercentValid));  // late
  rxPower(kPorch, "porch", powerBytes(4, 7, 998, espnowsensor::kPowerPercentValid));  // repeat
  const PeCollectPeer* porch = peerOf(kPorch);
  check(porch->power.percentX10 == 650 && porch->powerCounts.received == 2 && porch->powerCounts.lost == 2 &&
        porch->powerCounts.late == 1 && porch->powerCounts.repeated == 1, "lost 2, late 1, repeated 1; the newer state stays");
  rxPower(kPorch, "porch", powerBytes(1, 8, 400, espnowsensor::kPowerPercentValid));
  check(porch->power.percentX10 == 400 && porch->powerCounts.restarts == 1 && porch->powerCounts.lost == 2, "a restart is kept, with no loss");

  // What the sender did not know is null in JSON; every flag of a full one is there.
  resetAll();
  gRole = 1;
  gClockSet = true;
  gEpochMs = 1763388202417LL;
  gClockSource = 6;
  rxPower(kPorch, "porch", powerBytes(1, 7, 0, 0, /*status=*/0));
  gNowMs += 12000;
  std::string j = cmdBattery("json");
  check(jsonValid(j), "espnowbattery json is JSON");
  check(has(j, "{\"schema\":1,\"role\":\"master\",\"sends\":{\"sends\":false,\"whyNot\":\"is_master\",\"intervalS\":60,\"sent\":0,\"lastSentAgoS\":null}"),
        "it starts with the role and what this device sends itself");
  check(has(j, "\"refused\":{\"notReceiver\":0,\"full\":0,\"noMemory\":0,\"malformed\":0}") && has(j, "\"from\":0,\"devices\":[") &&
        has(j, "],\"next\":null}"), "the refusals, the paging keys");
  check(has(j, "{\"mac\":\"02:A0:00:00:00:01\",\"name\":\"porch\",\"arrivalEpochMs\":1763388202417,") &&
        has(j, "\"agoMs\":12000,\"clock\":\"mesh\",\"bootTag\":7,\"counter\":1"), "the device, the receiver's stamp, the sender's tag and counter");
  check(has(j, "\"status\":\"unknown\",\"backend\":\"adc\",\"voltage\":null,\"percent\":null,\"percentEstimated\":false,"
               "\"present\":null,\"charging\":null,\"chargingEstimated\":false,\"usb\":null,\"ratePctPerHr\":null,"
               "\"statusEstimated\":false,\"stale\":false,\"sampleAgeMs\":800,"), "nothing known: null for every value");
  check(has(j, "\"received\":1,\"lost\":0,\"late\":0,\"restarts\":0,\"resumes\":0}"), "the counts");
  espnowsensor::PowerFrame full{};
  full.counter = 2;
  full.bootTag = 7;
  full.status = 1;
  full.backend = 2;
  full.known = espnowsensor::kPowerKnownAll;
  full.voltageMv = 4123;
  full.percentX10 = 873;
  full.rateX10 = -42;
  full.sampleAgeMs = 65535;
  std::vector<uint8_t> bytes(24);
  espnowsensor::packPowerFrame(bytes.data(), full);
  rxPower(kPorch, "porch", bytes);
  j = cmdBattery("json");
  check(jsonValid(j) && has(j, "\"status\":\"charging\",\"backend\":\"fuelgauge\",\"voltage\":4.123,\"percent\":87.3,\"percentEstimated\":true,"
               "\"present\":true,\"charging\":true,\"chargingEstimated\":true,\"usb\":true,\"ratePctPerHr\":-4.2,"
               "\"statusEstimated\":true,\"stale\":true,\"sampleAgeMs\":null,"), "everything known: every value and flag");
  // A status and a backend this firmware does not know are "unknown".
  std::vector<uint8_t> hostile(24, 0xFF);
  hostile[0] = 3;
  hostile[1] = 0;
  hostile[2] = 7;
  hostile[3] = 0;
  rxPower(kPorch, "porch", hostile);
  j = cmdBattery("json");
  check(jsonValid(j) && has(j, "\"status\":\"unknown\",\"backend\":\"unknown\",\"voltage\":65.535,\"percent\":100.0,") &&
        has(j, "\"ratePctPerHr\":-0.1,"), "24 bytes of 0xFF: valid JSON, unknown names, the percentage held at 100");
  // The text form, with the time shown in local time.
  gConsole.clear();
  rxPower(kPorch, "porch", powerBytes(4, 7, 715, espnowsensor::kPowerPercentValid | espnowsensor::kPowerPercentEstimated |
                                                    espnowsensor::kPowerVoltageValid | espnowsensor::kPowerUsbKnown));
  gNowMs += 12000;
  check(cmdBattery("") == "OK: battery state of 1 device", "the text form's reply");
  check(gConsole.size() == 2 && gConsole[0] == "[ESP-NOW] Battery state received from other devices" &&
        gConsole[1] == "  porch: 71.5 % (estimated), 3.912 V, discharging, USB power: no; 12 s ago (local(1763388202) UTC+1:00); 4 frames kept, 0 lost",
        "one line per device, the arrival shown in local time");
  // One device by name or address; an unknown one; bad arguments.
  rxPower(kShed, "shed", powerBytes(1, 3, 500, espnowsensor::kPowerPercentValid));
  j = cmdBattery("json shed");
  check(jsonValid(j) && count(j, "\"mac\":") == 1 && has(j, "\"name\":\"shed\""), "json <name>: that device only");
  j = cmdBattery("02:A0:00:00:00:01 json");
  check(jsonValid(j) && count(j, "\"mac\":") == 1 && has(j, "\"name\":\"porch\""), "by address, in either order");
  check(cmdBattery("json nobody") == "Error: no such device (use its name or its MAC address)", "an unknown device");
  check(cmdBattery("json a b") == "Usage: espnowbattery [json] [<name_or_mac>] [from <n>]" &&
        cmdBattery("from x") == "Usage: espnowbattery [json] [<name_or_mac>] [from <n>]" &&
        cmdBattery("since 3") == "Usage: espnowbattery [json] [<name_or_mac>] [from <n>]", "bad arguments give the usage");
  char one[640];
  check(espnowBatteryJsonFor(kShed, one, sizeof(one)) > 0 && jsonValid(one) && has(one, "\"name\":\"shed\""),
        "one device's battery object, for espnowsensorstatus json <device>");
  check(espnowBatteryJsonFor(kHub, one, sizeof(one)) == 0 && one[0] == '\0', "nothing kept of a device: empty");
  char tiny[40];
  check(espnowBatteryJsonFor(kShed, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0', "a buffer that is too small gets nothing, not half an object");
  char words[96];
  check(espnowPowerEventsPeerText(kShed, words, sizeof(words)) > 0 && std::string(words) == "battery 50.0 % 0 s ago",
        "the few words for the status line");
  gReplyFails = true;
  check(cmdBattery("json") == kCommandReplyOutOfMemory && cmdEvents("json") == kCommandReplyOutOfMemory, "no reply buffer: said");
  gReplyFails = false;

  // Sixteen senders fit; the seventeenth is refused and counted; all sixteen
  // are listed over two replies (`next`).
  resetAll();
  gRole = 1;
  gPaired.clear();
  uint8_t mac[6] = { 0x02, 0xB0, 0, 0, 0, 0 };
  for (uint8_t s = 0; s < 17; ++s) {
    mac[5] = s;
    pair(mac, ("node-with-a-long-name-" + std::to_string(s)).c_str());
    rxPower(mac, gPaired.back().name.c_str(), powerBytes(1, 1, (uint16_t)(s * 10), espnowsensor::kPowerKnownAll));
  }
  check(sPe.refused.noRecord == 1 && peerOf(mac) == nullptr, "the seventeenth sender is refused and counted");
  check(gConsole.size() == 1 && has(gConsole[0], "refused: this device already keeps those of 16 paired devices"), "and said");
  j = cmdBattery("json");
  check(jsonValid(j) && j.size() < sizeof(gReply), "sixteen devices: the reply is whole JSON within its buffer");
  const size_t firstPage = count(j, "\"mac\":");
  check(firstPage >= 8 && firstPage < 16 && has(j, "\"next\":" + std::to_string(firstPage) + "}"), "it holds what fits and says where to go on");
  j = cmdBattery(("json from " + std::to_string(firstPage)).c_str());
  check(jsonValid(j) && count(j, "\"mac\":") == 16 - firstPage && has(j, "\"next\":null}") &&
        has(j, "\"from\":" + std::to_string(firstPage) + ","), "the second reply has the rest");
  j = cmdBattery("json from 99");
  check(jsonValid(j) && count(j, "\"mac\":") == 0 && has(j, "\"devices\":[],\"next\":null}"), "a 'from' past the end: an empty list");
  // A sender that is no longer paired gives its record to a new one.
  gPaired.erase(gPaired.begin() + 3);
  rxPower(mac, "seventeen", powerBytes(5, 1, 123, espnowsensor::kPowerPercentValid));
  check(peerOf(mac) && peerOf(mac)->powerCounts.received == 1 && peerOf(mac)->powerCounts.lost == 0 && sPe.refused.noRecord == 1,
        "an unpaired sender's record is reused, with clean counts");

  // No memory: refused, counted, said; too short: counted.
  resetAll();
  gRole = 1;
  gAllocFails = true;
  rxPower(kPorch, "porch", powerBytes(1, 7, 715, espnowsensor::kPowerPercentValid));
  check(sPe.peers == nullptr && sPe.refused.noMemory == 1 && gConsole.size() == 1 && has(gConsole[0], "out of memory"),
        "no memory for the records: refused, counted, said");
  gAllocFails = false;
  std::vector<uint8_t> shortFrame(23, 0);
  rxPower(kPorch, "porch", shortFrame);
  powerEventsIngestPower(kPorch, "porch", nullptr, 24);
  rxEvent(kPorch, "porch", std::vector<uint8_t>(127, 0));
  check(sPe.refused.malformed == 3 && sPe.peers == nullptr, "frames that are too short are counted and make nothing");
}

static void testEventStore() {
  resetAll();
  gRole = 1;
  gClockSet = true;
  gEpochMs = 1763388202417LL;
  gClockSource = 2;
  const uint8_t far[6] = { 0x02, 0xC0, 0, 0, 0, 1 };
  pair(far, "far", 1);
  // One event, as JSON.
  const uint32_t t0 = gNowMs;
  rxEvent(kPorch, "porch", eventBytes(1, 7, "login_fail", "admin", "3 attempts", "1.2.3.4", 0, 41));
  std::string j = cmdEvents("json");
  check(jsonValid(j), "espnowevents json is JSON");
  check(has(j, "{\"schema\":1,\"role\":\"master\",\"boot\":77,\"latest\":1,\"since\":0,\"overwritten\":0,"
               "\"sends\":{\"sends\":false,\"whyNot\":\"is_master\",\"floor\":\"alert\",\"sent\":0,\"leftOut\":0}"),
        "boot, the newest receive number, and what this device sends itself");
  check(has(j, "\"senders\":[{\"mac\":\"02:A0:00:00:00:01\",\"name\":\"porch\",\"received\":1,\"stored\":1,\"lost\":0,\"late\":0,"
               "\"restarts\":0,\"resumes\":0,\"senderSkipped\":0,\"refusedRate\":0}]"), "the sender's counts, with late, resumes and refusedRate");
  check(has(j, "\"events\":[{\"rx\":1,\"mesh\":0,\"mac\":\"02:A0:00:00:00:01\",\"name\":\"porch\",\"arrivalEpochMs\":1763388202417,"
               "\"arrivalUptimeMs\":" + std::to_string(t0) + ",\"clock\":\"ntp\",\"ageMs\":120,\"eventSeq\":41,\"bootTag\":7,\"counter\":1,"
               "\"tier\":\"alert\",\"source\":\"web\",\"kind\":\"login_fail\",\"subject\":\"admin\",\"detail\":\"3 attempts\","
               "\"who\":\"1.2.3.4\",\"cut\":[]}],\"more\":false}"), "the event: the receiver's stamp, the sender's numbers, the texts");
  check(has(j, "],\"sendersMore\":false,\"events\":["), "every sender's counts fitted");
  check(gLocalEventPosts == 0, "a received event is not posted into this device's own event ring");
  // The text form.
  gConsole.clear();
  check(cmdEvents("") == "OK: 1 event, the newest is #1", "the text form's reply");
  check(gConsole.size() == 2 && gConsole[1] == "  #1 local(1763388202) UTC+1:00  porch  alert  login_fail: admin | 3 attempts (by web:1.2.3.4)",
        "one line per event, the arrival shown in local time");

  // A frame of 128 bytes of 0xFF: stored as clean text, and the JSON stays valid.
  std::vector<uint8_t> hostile(128, 0xFF);
  hostile[0] = 2;
  hostile[1] = 0;
  hostile[2] = 7;
  hostile[3] = 0;
  rxEvent(kPorch, "porch", hostile);
  j = cmdEvents("json since 1");
  check(jsonValid(j) && has(j, "\"tier\":\"unknown\",\"source\":\"unknown\",\"kind\":\"" + std::string(32, '?') + "\"") &&
        has(j, "\"who\":\"????????????\",\"cut\":[\"kind\",\"subject\",\"detail\",\"who\"]}"),
        "128 bytes of 0xFF: unknown tier and source, every text cleaned, valid JSON");
  check(peerOf(kPorch)->senderSkipped == 65535, "the sender's 'skipped' is added up");
  // Quotes, backslashes and control bytes in a text; a kind this firmware does not know.
  rxEvent(kPorch, "por\"ch", eventBytes(3, 7, "some_future_kind", "say \"hi\"", "a\\b\nc", "x\ty"));
  j = cmdEvents("json since 2");
  check(jsonValid(j) && has(j, "\"name\":\"por\\\"ch\"") && has(j, "\"kind\":\"some_future_kind\",\"subject\":\"say \\\"hi\\\"\","
               "\"detail\":\"a\\\\b?c\",\"who\":\"x?y\""), "texts are escaped; an unknown kind is kept as text");
  check(gLockViolations == 0, "nothing that may block ran under the lock so far");

  // A list holds 24: the 25th event of a mesh replaces the first. `since`
  // returns the right ones and `overwritten` counts what is gone.
  resetAll();
  gRole = 1;
  pair(far, "far", 1);
  for (uint16_t n = 1; n <= 25; ++n) {
    rxEvent(kPorch, "porch", eventBytes(n, 7, "login_fail", ("user" + std::to_string(n)).c_str()));
    gNowMs += 2000;  // within the receiver's rate limit for one sender
  }
  check(sPe.rxSerial == 25 && peerOf(kPorch)->eventsStored == 25 && peerOf(kPorch)->refusedRate == 0, "25 events stored one after the other");
  j = cmdEvents("json");
  check(jsonValid(j) && has(j, "\"latest\":25,\"since\":0,\"overwritten\":1,") && !has(j, "\"rx\":1,") && has(j, "\"events\":[{\"rx\":2,"),
        "the first one is gone and counted as overwritten; the list starts at #2");
  check(has(j, "\"more\":true}"), "24 events do not fit one reply: more");
  // Paging: ask again with since = the last rx, until more is false; every event once, in order.
  std::vector<uint32_t> seen;
  uint32_t since = 0;
  for (int page = 0; page < 12; ++page) {
    j = cmdEvents(("json since " + std::to_string(since)).c_str());
    check(jsonValid(j) && j.size() < sizeof(gReply), "each page is whole JSON within its buffer");
    for (size_t at = j.find("{\"rx\":"); at != std::string::npos; at = j.find("{\"rx\":", at + 1)) {
      seen.push_back((uint32_t)std::strtoul(j.c_str() + at + 6, nullptr, 10));
    }
    if (!seen.empty()) since = seen.back();
    if (has(j, "\"more\":false}")) break;
  }
  bool inOrder = seen.size() == 24;
  for (size_t i = 0; i < seen.size(); ++i) inOrder = inOrder && seen[i] == i + 2;
  check(inOrder, "paging returns events #2 to #25, each once, oldest first");
  j = cmdEvents("json since 20");
  check(jsonValid(j) && has(j, "\"since\":20,\"overwritten\":0,") && count(j, "{\"rx\":") == 5 && has(j, "\"events\":[{\"rx\":21,"),
        "since 20: the five after it, nothing overwritten");
  j = cmdEvents("json since 25");
  check(jsonValid(j) && has(j, "\"events\":[],\"more\":false}"), "since the newest: nothing");
  // A number from before this device restarted is read as 0.
  j = cmdEvents("json since 900");
  check(jsonValid(j) && has(j, "\"latest\":25,\"since\":0,\"overwritten\":1,") && has(j, "\"events\":[{\"rx\":2,"),
        "a 'since' above the newest is read as 0 (the receiver restarted)");
  // Events of mesh 1 do not push out those of mesh 0.
  for (uint16_t n = 1; n <= 30; ++n) {
    rxEvent(far, "far", eventBytes(n, 9, "login_fail", "x"));
    gNowMs += 2000;
  }
  unsigned mesh0 = 0, mesh1 = 0;
  for (uint8_t i = 0; i < espnowsensor::kEventListLen; ++i) {
    if (sPe.lists[0] && sPe.lists[0][i].rx) ++mesh0;
    if (sPe.lists[1] && sPe.lists[1][i].rx) ++mesh1;
  }
  check(mesh0 == 24 && mesh1 == 24 && sPe.lists[2] == nullptr, "each mesh has its own list of 24; a mesh without events has none");
  bool mesh0Kept = true;
  for (uint8_t i = 0; i < espnowsensor::kEventListLen; ++i) {
    mesh0Kept = mesh0Kept && std::memcmp(sPe.lists[0][i].senderMac, kPorch, 6) == 0 && sPe.lists[0][i].mesh == 0;
  }
  check(mesh0Kept, "30 events from mesh 1 pushed out none of mesh 0");
  j = cmdEvents("json since 54");
  check(jsonValid(j) && has(j, "\"mesh\":1,\"mac\":\"02:C0:00:00:00:01\",\"name\":\"far\""), "an event says which mesh it came from");

  // The receiver's rate limit per sender: sixteen at once, then one a second.
  resetAll();
  gRole = 1;
  for (uint16_t n = 1; n <= 20; ++n) rxEvent(kPorch, "porch", eventBytes(n, 7, "login_fail", "flood"));
  const PeCollectPeer* porch = peerOf(kPorch);
  check(porch->eventsStored == 16 && porch->refusedRate == 4 && porch->eventCounts.received == 20 && sPe.rxSerial == 16,
        "a flood from one sender: sixteen stored, four refused and counted, none of them 'lost'");
  rxEvent(kShed, "shed", eventBytes(1, 3, "login_fail", "quiet"));
  check(peerOf(kShed)->eventsStored == 1, "another sender is not held back by it");
  gNowMs += 1000;
  rxEvent(kPorch, "porch", eventBytes(21, 7, "login_fail", "flood"));
  check(porch->eventsStored == 17, "a second later one more is taken");
  // A busy sender cannot push out a quiet one's events: with the list full,
  // the oldest entry of the sender that holds the most is replaced.
  for (uint16_t n = 22; n <= 60; ++n) {
    gNowMs += 1000;
    rxEvent(kPorch, "porch", eventBytes(n, 7, "login_fail", "flood"));
  }
  bool quietStill = false;
  for (uint8_t i = 0; i < espnowsensor::kEventListLen; ++i) {
    if (std::memcmp(sPe.lists[0][i].senderMac, kShed, 6) == 0) quietStill = true;
  }
  check(quietStill, "the quiet sender's event is still held after 40 more from the busy one");
  j = cmdEvents("json");
  check(jsonValid(j) && has(j, "\"name\":\"porch\",\"received\":60,\"stored\":56,") && has(j, "\"refusedRate\":4}"),
        "the JSON has both senders' counts");
  char counts[320];
  check(espnowEventCountsJsonFor(kPorch, counts, sizeof(counts)) > 0 && jsonValid(counts) &&
        std::string(counts) == "{\"received\":60,\"stored\":56,\"lost\":0,\"late\":0,\"restarts\":0,\"resumes\":0,\"senderSkipped\":0,\"refusedRate\":4}",
        "one device's event counts, for espnowsensorstatus json <device>");
  check(espnowEventCountsJsonFor(kHub, counts, sizeof(counts)) == 0, "a device that sent no event: nothing");
  // Sixteen senders with events, each with a long name and large counts:
  // the senders' rows leave room for the events, so every page carries at
  // least one event and the paging ends.
  resetAll();
  gRole = 1;
  gPaired.clear();
  uint8_t node[6] = { 0x02, 0xB0, 0, 0, 0, 0 };
  for (uint8_t s = 0; s < 16; ++s) {
    node[5] = s;
    pair(node, ("a-sender-with-a-thirty-one-char-" + std::to_string(s)).substr(0, 31).c_str());
    rxEvent(node, gPaired.back().name.c_str(), eventBytes(1, 1, std::string(32, 'k').c_str(), std::string(32, '"').c_str(),
                                                          std::string(32, '\\').c_str(), std::string(12, '"').c_str(), 65535));
    PeCollectPeer* peer = peerOf(node);
    peer->eventCounts.received = peer->eventCounts.lost = peer->eventCounts.late = 4000000000u;
    peer->eventCounts.restarts = peer->eventCounts.resumes = peer->senderSkipped = peer->refusedRate = 4000000000u;
    peer->eventsStored = 4000000000u;
  }
  std::vector<uint32_t> pages;
  uint32_t after = 0;
  for (int page = 0; page < 40; ++page) {
    j = cmdEvents(("json since " + std::to_string(after)).c_str());
    check(jsonValid(j) && j.size() < sizeof(gReply), "sixteen busy senders: each page is whole JSON within its buffer");
    size_t inPage = 0;
    for (size_t at = j.find("{\"rx\":"); at != std::string::npos; at = j.find("{\"rx\":", at + 1)) {
      after = (uint32_t)std::strtoul(j.c_str() + at + 6, nullptr, 10);
      ++inPage;
    }
    pages.push_back((uint32_t)inPage);
    if (has(j, "\"more\":false}")) break;
    check(inPage >= 1, "a page that says 'more' carries at least one event");
  }
  uint32_t total = 0;
  for (uint32_t n : pages) total += n;
  check(total == 16 && after == 16 && pages.size() <= 16, "all sixteen events arrive over the pages, and the paging ends");
  check(has(j, "\"sendersMore\":true,"), "senders that did not fit are said to be left out");
  // Lost and late events are counted by the same rule as readings.
  resetAll();
  gRole = 1;
  rxEvent(kPorch, "porch", eventBytes(1, 7, "login_fail", "a"));
  gNowMs += 1000;
  rxEvent(kPorch, "porch", eventBytes(5, 7, "login_fail", "b", "", "", 3));
  rxEvent(kPorch, "porch", eventBytes(4, 7, "login_fail", "late"));
  rxEvent(kPorch, "porch", eventBytes(5, 7, "login_fail", "again"));
  porch = peerOf(kPorch);
  check(porch->eventCounts.lost == 3 && porch->eventCounts.late == 1 && porch->eventCounts.repeated == 1 && porch->eventsStored == 2 &&
        porch->senderSkipped == 3, "lost 3, late 1, repeated 1; a late or repeated event is not stored again");
  // Arguments.
  check(cmdEvents("json porch") == "Usage: espnowevents [json] [since <rx>]" && cmdEvents("since") == "Usage: espnowevents [json] [since <rx>]" &&
        cmdEvents("since -1") == "Usage: espnowevents [json] [since <rx>]" && cmdEvents("from 2") == "Usage: espnowevents [json] [since <rx>]",
        "bad arguments give the usage");
  // No memory for a mesh's list: refused and counted.
  resetAll();
  gRole = 1;
  rxPower(kPorch, "porch", powerBytes(1, 7, 715, espnowsensor::kPowerPercentValid));  // the records exist
  gAllocFails = true;
  rxEvent(kPorch, "porch", eventBytes(1, 7, "login_fail", "a"));
  check(sPe.refused.noMemory == 1 && sPe.lists[0] == nullptr && sPe.rxSerial == 0, "no memory for the list: refused and counted");
  check(cmdEvents("") == "OK: no events received from other devices", "and espnowevents has none");
}

// A worker's own view: what it sends and to whom shows in both commands.
static void testSenderView() {
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  post(1, "admin", "x", "");
  run(2);
  std::string j = cmdBattery("json");
  check(jsonValid(j) && has(j, "\"role\":\"worker\",\"sends\":{\"sends\":true,\"whyNot\":null,\"intervalS\":60,\"sent\":1,\"lastSentAgoS\":1}"),
        "a worker's espnowbattery json says it sends, how often and when last");
  j = cmdEvents("json");
  check(jsonValid(j) && has(j, "\"role\":\"worker\",\"boot\":77,\"latest\":0,") &&
        has(j, "\"sends\":{\"sends\":true,\"whyNot\":null,\"floor\":\"alert\",\"sent\":1,\"leftOut\":0}"), "and its espnowevents json likewise");
  gConsole.clear();
  cmdBattery("");
  check(gConsole.size() == 1 && gConsole[0] == "[ESP-NOW] This device sends its own battery state to the master named on it every 60 s (1 sent).",
        "the text form says so in words");
  gConsole.clear();
  cmdEvents("");
  check(gConsole.size() == 1 && gConsole[0] == "[ESP-NOW] This device sends its own events (alert and above) to the master named on it (1 sent, 0 left out).",
        "for events as well");
  gTargets = SensorReceiverTargets{};
  gConsole.clear();
  cmdBattery("");
  check(gConsole.size() == 1 && gConsole[0] == "[ESP-NOW] This device does not send its own battery state: no master is named on this device (espnowmasterfingerprint).",
        "and why not, when it does not");
  // The interval a stored value outside the range becomes.
  gIntervalS = 3;
  PowerEventsSendView view;
  espnowPowerEventsSendView(&view);
  check(view.batteryIntervalS == 10, "a stored interval under 10 s is used as 10 s");
}

// What the review of the A-F package found (2026-11-18).
static std::string batteryKeys(uint16_t known) {
  // The JSON of one battery frame from `known`, from "voltage" to "stale".
  resetAll();
  gRole = 1;
  espnowsensor::PowerFrame f{};
  f.counter = 1;
  f.bootTag = 7;
  f.status = 3;
  f.backend = 1;
  f.known = known;
  f.voltageMv = 3912;
  f.percentX10 = 715;
  f.rateX10 = 25;
  f.sampleAgeMs = 800;
  std::vector<uint8_t> bytes(24);
  espnowsensor::packPowerFrame(bytes.data(), f);
  rxPower(kPorch, "porch", bytes);
  const std::string j = cmdBattery("json");
  const size_t from = j.find("\"voltage\":");
  const size_t to = j.find(",\"sampleAgeMs\"");
  return (from == std::string::npos || to == std::string::npos) ? std::string("?") : j.substr(from, to - from);
}
// The same from the frame's specification (System_ESPNow_PowerEventCore.h), written out again.
static std::string batteryKeysSpec(uint16_t known) {
  auto bit = [&](int n) { return (known & (1u << n)) != 0; };
  auto tri = [&](int knownBit, int valueBit) { return bit(knownBit) ? (bit(valueBit) ? "true" : "false") : "null"; };
  std::string out = std::string("\"voltage\":") + (bit(0) ? "3.912" : "null") + ",\"percent\":" + (bit(1) ? "71.5" : "null") +
                    ",\"percentEstimated\":" + (bit(2) ? "true" : "false") + ",\"present\":" + tri(3, 4) +
                    ",\"charging\":" + tri(5, 6) + ",\"chargingEstimated\":" + (bit(12) ? "true" : "false") +
                    ",\"usb\":" + tri(7, 8) + ",\"ratePctPerHr\":" + (bit(9) ? "2.5" : "null") +
                    ",\"statusEstimated\":" + (bit(10) ? "true" : "false") + ",\"stale\":" + (bit(11) ? "true" : "false");
  return out;
}

static void testReviewFindings() {
  // Every bit of the battery frame's `known` reaches the JSON key it names,
  // alone and with its partner (a key fed by the wrong bit went unseen).
  for (int b = 0; b < 13; ++b) {
    const uint16_t known = (uint16_t)(1u << b);
    check(batteryKeys(known) == batteryKeysSpec(known), "battery JSON, bit " + std::to_string(b) + " alone: " + batteryKeys(known));
  }
  for (uint16_t pairBits : { (uint16_t)0x18, (uint16_t)0x60, (uint16_t)0x180, (uint16_t)0x1FFF, (uint16_t)0x0AAA, (uint16_t)0x1555 }) {
    check(batteryKeys(pairBits) == batteryKeysSpec(pairBits), "battery JSON, bits " + std::to_string(pairBits));
  }
  // The clock is not set: no arrival epoch is stored, whatever the clock reads.
  resetAll();
  gRole = 1;
  rxPower(kPorch, "porch", powerBytes(1, 7, 715, espnowsensor::kPowerPercentValid));
  rxEvent(kPorch, "porch", eventBytes(1, 7, "login_fail", "admin"));
  check(peerOf(kPorch)->powerEpochMs == 0 && sPe.lists[0] && sPe.lists[0][0].arrivalEpochMs == 0,
        "clock not set: battery and event arrival epochs are 0 (the 1970 reading is not taken)");
  check(has(cmdEvents("json"), "\"arrivalEpochMs\":null,") && has(cmdBattery("json"), "\"arrivalEpochMs\":null,"), "and null in JSON");
  // A sender's mesh id outside the lists' range is kept in list 0.
  resetAll();
  gRole = 1;
  const uint8_t odd[6] = { 0x02, 0xD0, 0, 0, 0, 1 };
  pair(odd, "odd", 7);
  rxEvent(odd, "odd", eventBytes(1, 7, "login_fail", "x"));
  check(sPe.lists[0] && sPe.lists[0][0].rx == 1 && sPe.lists[0][0].mesh == 0 && has(cmdEvents("json"), "\"mesh\":0,"),
        "mesh id 7 is kept in list 0 and shown as mesh 0");
  // The few words of the status line count the sender's events.
  rxEvent(odd, "odd", eventBytes(2, 7, "login_fail", "y"));
  rxEvent(odd, "odd", eventBytes(3, 7, "login_fail", "z"));
  char words[96];
  espnowPowerEventsPeerText(odd, words, sizeof(words));
  check(std::string(words) == "3 events", std::string("the status line counts 3 events: ") + words);
  // The text form marks a cut text.
  gConsole.clear();
  std::vector<uint8_t> cutFrame = eventBytes(4, 7, "login_fail", "a-subject-that-is-longer-than-thirty-two-bytes!");
  cutFrame[4] = espnowsensor::kEventCutSubject;  // the sender cut the subject
  rxEvent(odd, "odd", cutFrame);
  cmdEvents("since 3");
  check(gConsole.size() >= 2 && has(gConsole[1], "#4 ") && has(gConsole[1], " [text cut]"), "a cut text says so in the text form");

  // Sending: an event goes only to a named master whose session is ready.
  resetAll();
  nameMasters(true, true);
  addSession(kHub, true, gNowMs - 5000);
  addSession(kBackup, true, gNowMs + 500);  // came up after this pass read the uptime
  gIntervalS = 0;
  post(1, "admin", "x", "");
  run(1);
  check(gSent.size() == 1 && std::memcmp(gSent[0].dst, kHub, 6) == 0, "only the master with a ready session gets the event");
  // An event posted after the task read the uptime is 0 ms old, not 49 days.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  post(1, "late", "x", "", 3, /*aheadMs=*/300);
  gNowMs += 1000;
  sweepWatch(gNowMs);
  espnowPowerEventsTick(gNowMs - 1000 + 1000);
  check(gSent.size() == 1 && eventOf(gSent[0]).ageMs == 700, "an event 300 ms ahead of the pass is 700 ms old one second later");
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  gNowMs += 1000;
  post(1, "ahead", "x", "", 3, /*aheadMs=*/400);
  espnowPowerEventsTick(gNowMs);
  check(gSent.size() == 1 && eventOf(gSent[0]).ageMs == 0, "an event stamped after the pass read the uptime is 0 ms old");
  // A source number outside the table is sent as unknown.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  post(1, "src", "x", "", 200);
  run(1);
  check(gSent.size() == 1 && eventOf(gSent[0]).source == espnowsensor::kEventSourceUnknown, "source 200 goes out as 0 (unknown)");
  // A kind name longer than the field is cut, and says so.
  post(5, "kind", "x", "");
  run(1);
  check(gSent.size() == 2 && (eventOf(gSent[1]).flags & espnowsensor::kEventCutKind) &&
        field(eventOf(gSent[1]).kind, 32) == "a_kind_name_longer_than_the_32_b", "a long kind name is cut and flagged");
  // A full queue takes no token of the rate limit: eight wait and then all go.
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  for (int i = 0; i < 8; ++i) post(1, "queued", "x", "");
  gQueueFull = true;
  run(6);
  gQueueFull = false;
  run(2);
  PowerEventsSendView view;
  espnowPowerEventsSendView(&view);
  check(gSent.size() == 8 && view.eventsLeftOut == 0, "after a full queue all eight go out, none left out");
  // Events under the floor cost nothing of the four a look: an alert behind
  // ten lesser events goes out in the first look (it took three).
  resetAll();
  addSession(kHub, true, gNowMs - 5000);
  gIntervalS = 0;
  for (int i = 0; i < 10; ++i) post(3, "verbose", "x", "");
  post(1, "alert", "x", "");
  run(1);
  check(gSent.size() == 1 && field(eventOf(gSent[0]).subject, 32) == "alert", "the alert goes out in the first look");
  for (int i = 0; i < 47; ++i) post(3, "verbose", "x", "");
  post(1, "alert2", "x", "");
  run(1);
  check(gSent.size() == 2 && field(eventOf(gSent[1]).subject, 32) == "alert2" && eventOf(gSent[1]).skipped == 0,
        "also behind a whole ring of lesser events, with nothing counted as skipped");

  // Both JSON replies stay whole JSON inside any reply buffer, at every size
  // where a row is cut (paging without the room for the tail went unseen).
  resetAll();
  gRole = 1;
  gPaired.clear();
  uint8_t mac[6] = { 0x02, 0xB0, 0, 0, 0, 0 };
  for (uint8_t n = 0; n < 16; ++n) {
    mac[5] = n;
    pair(mac, ("node-" + std::to_string(n)).c_str());
    rxPower(mac, gPaired.back().name.c_str(), powerBytes(1, 1, 500, espnowsensor::kPowerKnownAll));
    for (uint16_t k = 1; k <= 3; ++k) rxEvent(mac, gPaired.back().name.c_str(), eventBytes(k, 1, "login_fail", "subject", "detail", "who"));
  }
  bool allWhole = true;
  std::string firstBad;
  for (size_t capSize = 900; capSize <= sizeof(gReply); capSize += 7) {
    gReplyCap = capSize;
    for (const char* args : { "json", "json from 3", "json since 5" }) {
      const std::string j = has(args, "since") ? cmdEvents(args) : cmdBattery(args);
      if (!jsonValid(j) || j.size() >= capSize) {
        if (allWhole) firstBad = std::to_string(capSize) + " " + args + ": " + j.substr(j.size() > 60 ? j.size() - 60 : 0);
        allWhole = false;
      }
    }
    const std::string e = cmdEvents("json");
    if (!jsonValid(e) || e.size() >= capSize) {
      if (allWhole) firstBad = std::to_string(capSize) + " events: " + e.substr(e.size() > 60 ? e.size() - 60 : 0);
      allWhole = false;
    }
  }
  gReplyCap = sizeof(gReply);
  check(allWhole, "every reply is whole JSON within buffers of 900 to 4096 bytes" + (firstBad.empty() ? "" : ": " + firstBad));
}

// The event counts of other devices are for an admin, as espnowevents is:
// espnowsensorstatus (its sender lines and its json for one device) leaves
// them out for anyone else; the battery state stays for every user.
static void testEventCountsAdminOnly() {
  resetAll();
  gRole = 1;
  pair(kPorch, "porch");
  rxPower(kPorch, "porch", powerBytes(1, 3, 715, espnowsensor::kPowerPercentValid));
  rxEvent(kPorch, "porch", eventBytes(1, 7, "login_fail", "admin", "3 attempts"));
  rxEvent(kPorch, "porch", eventBytes(2, 7, "login_fail", "admin", "4 attempts"));
  char words[96];
  char counts[320];
  gCallerAdmin = true;
  check(espnowEventCountsVisible(), "an admin sees the event counts");
  check(espnowPowerEventsPeerText(kPorch, words, sizeof(words)) > 0 &&
        std::string(words) == "battery 71.5 % 0 s ago, 2 events", "an admin's status line has the event count");
  check(espnowEventCountsJsonFor(kPorch, counts, sizeof(counts)) > 0 && has(counts, "\"received\":2,"),
        "an admin's json for the device has its event counts");
  gCallerAdmin = false;
  check(!espnowEventCountsVisible(), "a user who is not an admin does not");
  check(espnowPowerEventsPeerText(kPorch, words, sizeof(words)) > 0 &&
        std::string(words) == "battery 71.5 % 0 s ago", "the status line keeps the battery and leaves the events out");
  check(espnowEventCountsJsonFor(kPorch, counts, sizeof(counts)) == 0 && counts[0] == '\0',
        "the json for the device gets no event counts");
  // A sender that sent events only: no words at all for that user.
  pair(kShed, "shed");
  rxEvent(kShed, "shed", eventBytes(1, 9, "login_fail", "root"));
  check(espnowPowerEventsPeerText(kShed, words, sizeof(words)) == 0 && words[0] == '\0',
        "a sender with events and no battery state: nothing to say to that user");
  gCallerAdmin = true;
  check(espnowPowerEventsPeerText(kShed, words, sizeof(words)) > 0 && std::string(words) == "1 events",
        "and its count to an admin");
}

int main() {
  testPowerSend();
  testStaleSessionHeals();
  testEventSend();
  testReceiveRolesAndBattery();
  testEventStore();
  testSenderView();
  testReviewFindings();
  testEventCountsAdminOnly();
  check(gLockViolations == 0, "nothing that may block, print or allocate ran under the lock");
  check(gLockDepth == 0, "every lock was released");
  check(gLocalEventPosts == 0, "nothing was posted into this device's own event ring");
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d battery and event checks failed\n", failures, checks);
    return 1;
  }
  std::printf("ESP-NOW battery state and events: %d checks passed\n", checks);
  return 0;
}
