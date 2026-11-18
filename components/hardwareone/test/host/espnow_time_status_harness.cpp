// Host harness for mesh time (TIME_SYNC): the production code of
// System_ESPNow_MeshTime.cpp between MESH_TIME_HOST_BEGIN and
// MESH_TIME_HOST_END, compiled as it is with the production rules header
// System_ESPNow_TimeCore.h.
//
// test_espnow_time_status.py inserts that block and the two clock questions of
// System_Clock.cpp (isSynced, isValidEpoch). Host boundaries, all in this
// file: a settable system clock with settimeofday, the clock module's source,
// step counter and chokepoint (a recording double), a monotonic timer and the
// 32-bit uptime, the settings, the paired devices with their sessions, the
// radio queue (it records what was sent), the handshake start, the console
// and the reply buffer.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>
#include <vector>

#include "System_ESPNow_TimeCore.h"

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  const std::string& text() const { return text_; }
 private:
  std::string text_;
};

#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)argsInput)  // validate-only mode is off here
[[maybe_unused]] static const char kCommandReplyOutOfMemory[] = "Error: Out of memory";

// ---- The system clock ---------------------------------------------------------
static int64_t gClockMs = 0;  // what the system clock reads; under 2020 = not set
static time_t testTime(time_t*) { return (time_t)(gClockMs / 1000); }

namespace Clock {
#define time testTime
// INSERT_CLOCK
#undef time
}  // namespace Clock

// ---- The clock module (recording double) --------------------------------------
static meshtime::OwnSource gSource = meshtime::OwnSource::None;
static bool gClockHeld = false;
static uint32_t gStepSerial = 0;
static uint32_t gLastStepUptimeMs = 0;
static unsigned gMeshSteps = 0;          // clockStepped(SYNC_MESH, ...) calls
static int64_t gMeshStepBeforeMs = 0;    // the clock right before the last of them
static bool gRaceOnNextRead = false;     // another source sets the clock between decision and step
static int gRaceReads = 0;

static uint32_t gUptimeMs = 0;
static int64_t gMonoMs = 0;
static void pass(int64_t ms) {
  gUptimeMs += (uint32_t)ms;
  gMonoMs += ms;
  gClockMs += ms;
}
// Another source (timeset, NTP) sets the clock.
static void otherSourceSets(meshtime::OwnSource source, int64_t epochMs) {
  gClockMs = epochMs;
  gSource = source;
  gStepSerial++;
  gLastStepUptimeMs = gUptimeMs;
}

// ---- Settings and peers ---------------------------------------------------------
static bool gSettingOn = true;
static bool gMeshOn = true;
static uint8_t gRole = 0;
static bool gBackupPromoted = false;
static uint8_t gNamedPrimary[6] = {};
static uint8_t gNamedBackup[6] = {};
static bool gHaveNamedPrimary = false;
static bool gHaveNamedBackup = false;

struct TestPeer {
  uint8_t mac[6];
  bool paired;
  bool session;
  uint32_t establishedAtMs;
  const char* name;
};
static std::vector<TestPeer> gPeers;

struct SentFrame {
  uint8_t mac[6];
  meshtime::Frame frame;
  uint32_t atMs;
};
static std::vector<SentFrame> gSent;
static std::vector<std::string> gHandshakes;
static std::vector<std::string> gConsole;
static bool gQueueFull = false;
static char gReply[4096];

static std::string macText(const uint8_t mac[6]) {
  char out[18];
  std::snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return out;
}
static void macToDisplay(const uint8_t* mac, char* out, size_t cap) { std::snprintf(out, cap, "%s", macText(mac).c_str()); }

// ---- What the production block asks of the device --------------------------------
#define MESH_TIME_LOCK()   ((void)0)
#define MESH_TIME_UNLOCK() ((void)0)
static constexpr uint8_t kMeshTimePeerSlots = 16;

static uint32_t mtUptimeMs() { return gUptimeMs; }
static int64_t mtMonoMs() { return gMonoMs; }
static bool mtClockSet() { return Clock::isSynced(); }
static int64_t mtEpochMs() { return gClockMs; }
static meshtime::OwnSource mtOwnSource() { return gSource; }
static bool mtClockHeld() { return gClockHeld; }
static uint32_t mtStepSerial() {
  if (gRaceOnNextRead && ++gRaceReads == 2) {
    // The decision has read the counter once; before the second read, timeset
    // sets the clock on another task.
    gRaceReads = 0;
    gRaceOnNextRead = false;
    otherSourceSets(meshtime::OwnSource::Manual, gClockMs + 500000);
  }
  return gStepSerial;
}
static bool mtLastSetAgo(uint32_t* agoSeconds) {
  if (gStepSerial == 0) return false;
  if (agoSeconds) *agoSeconds = (gUptimeMs - gLastStepUptimeMs) / 1000;
  return true;
}
static void mtSetClock(int64_t epochMs) {
  // settimeofday, then Clock::clockStepped(Clock::SYNC_MESH, before).
  gMeshStepBeforeMs = gClockMs;
  gClockMs = epochMs;
  gSource = meshtime::OwnSource::Mesh;
  gStepSerial++;
  gLastStepUptimeMs = gUptimeMs;
  gMeshSteps++;
}
static void mtFormatLocal(int64_t epochMs, char* out, size_t cap) {
  std::snprintf(out, cap, "epoch %lld", (long long)(epochMs / 1000));
}
static bool mtSettingOn() { return gSettingOn; }
static bool mtMeshOn() { return gMeshOn; }
static uint8_t mtRole() { return gRole; }
static bool mtBackupPromoted() { return gBackupPromoted; }
static bool mtHasNamedMaster() { return gHaveNamedPrimary || gHaveNamedBackup; }
// The fingerprints the harness names are the addresses as text: a change of
// either named master changes the tag, as a change of a fingerprint does.
static uint64_t mtNamedTag() {
  const std::string primary = gHaveNamedPrimary ? macText(gNamedPrimary) : std::string();
  const std::string backup = gHaveNamedBackup ? macText(gNamedBackup) : std::string();
  return meshtime::namedMastersTag(primary.c_str(), backup.c_str());
}
static int mtNamedKind(const uint8_t mac[6]) {
  if (gHaveNamedPrimary && std::memcmp(mac, gNamedPrimary, 6) == 0) return 1;
  if (gHaveNamedBackup && std::memcmp(mac, gNamedBackup, 6) == 0) return 2;
  return 0;
}
static uint8_t mtPeerSlots() { return (uint8_t)gPeers.size(); }
static bool mtPeerAt(uint8_t i, uint8_t mac[6]) {
  if (i >= gPeers.size() || !gPeers[i].paired) return false;
  std::memcpy(mac, gPeers[i].mac, 6);
  return true;
}
static TestPeer* peerOf(const uint8_t mac[6]) {
  for (auto& p : gPeers) {
    if (std::memcmp(p.mac, mac, 6) == 0) return &p;
  }
  return nullptr;
}
static bool mtIsPaired(const uint8_t mac[6]) {
  const TestPeer* p = peerOf(mac);
  return p && p->paired;
}
static bool mtSession(const uint8_t mac[6], uint32_t* establishedAtMs) {
  const TestPeer* p = peerOf(mac);
  if (!p || !p->paired || !p->session) return false;
  if (establishedAtMs) *establishedAtMs = p->establishedAtMs;
  return true;
}
static bool mtSend(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  if (gQueueFull) return false;
  SentFrame s{};
  std::memcpy(s.mac, mac, 6);
  if (!meshtime::parseFrame(payload, len, s.frame) || len != meshtime::kFrameLen) return false;
  for (int i = 12; i < 16; ++i) {
    if (payload[i] != 0) return false;  // reserved bytes are 0 on the wire
  }
  s.atMs = gUptimeMs;
  gSent.push_back(s);
  return true;
}
static bool mtHandshake(const uint8_t mac[6]) {
  gHandshakes.push_back(macText(mac));
  return true;
}
static void mtPeerName(const uint8_t mac[6], char* out, size_t cap) {
  const TestPeer* p = peerOf(mac);
  std::snprintf(out, cap, "%s", p && p->name ? p->name : macText(mac).c_str());
}
static void mtConsole(const char* fmt, ...) {
  char line[200];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  gConsole.push_back(line);
}
static char* mtReply(size_t* cap) {
  *cap = sizeof(gReply);
  return gReply;
}
static bool mtWantsJson(const String& args) { return args.text().find("json") != std::string::npos; }

// INSERT_PRODUCTION

// ---- Cases ------------------------------------------------------------------------
static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}
static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

static const int64_t kT0 = 1763388000000LL;  // 2026-11-17 UTC
static const uint8_t kHub[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01 };
static const uint8_t kPorch[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x02 };
static const uint8_t kShed[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x03 };

static void resetAll() {
  sMeshTime = MeshTimeState{};
  gClockMs = 0;
  gSource = meshtime::OwnSource::None;
  gClockHeld = false;
  gStepSerial = 0;
  gLastStepUptimeMs = 0;
  gMeshSteps = 0;
  gRaceOnNextRead = false;
  gRaceReads = 0;
  gUptimeMs = 100000;
  gMonoMs = 100000;
  gSettingOn = true;
  gMeshOn = true;
  gRole = 0;
  gBackupPromoted = false;
  // The usual set-up: hub is the master named on this device.
  gHaveNamedPrimary = true;
  std::memcpy(gNamedPrimary, kHub, 6);
  gHaveNamedBackup = false;
  gPeers.clear();
  gSent.clear();
  gHandshakes.clear();
  gConsole.clear();
  gQueueFull = false;
}
static void addPeer(const uint8_t mac[6], const char* name, bool session, uint32_t establishedAtMs = 0) {
  TestPeer p{};
  std::memcpy(p.mac, mac, 6);
  p.paired = true;
  p.session = session;
  p.establishedAtMs = establishedAtMs;
  p.name = name;
  gPeers.push_back(p);
}
// A TIME_SYNC payload arrives from `sender`, as the frame handler hands it on.
static void receive(const uint8_t sender[6], int64_t epochMs, uint8_t source = meshtime::kSrcNtp, uint8_t flags = 0,
                    size_t len = meshtime::kFrameLen) {
  meshtime::Frame f{};
  f.epochMs = epochMs;
  f.source = source;
  f.flags = flags;
  f.sinceSetMin = 3;
  uint8_t payload[meshtime::kFrameLen];
  meshtime::packFrame(payload, f);
  meshTimeKeepFrame(sender, payload, len);
}
static std::string status(const char* args = "") { return cmd_espnow_timestatus(String(args)); }
static std::string sync() { return cmd_espnow_timesync(String("")); }
// Runs the sender step for `ms` of uptime, one look per 100 ms.
static void runSender(int64_t ms) {
  for (int64_t t = 0; t < ms; t += 100) {
    pass(100);
    espnowMeshTimeSendTick(gUptimeMs);
  }
}

static void testFollower() {
  // A device with no clock takes the master's time, through the chokepoint, once.
  resetAll();
  addPeer(kHub, "hub", true);
  check(has(status(), "This device's clock: not set"), "status says the clock is not set");
  check(has(status(), "No time frame received yet."), "status says no frame was received");
  check(has(status("json"), "\"clockSet\":false,\"clockSource\":\"none\""), "json: clock not set, source none");
  receive(kHub, kT0);
  check(gMeshSteps == 0 && gClockMs < 1000000, "the handler sets nothing itself");
  pass(40);  // the frame waits 40 ms for the main loop
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1, "one step through the chokepoint (clockStepped with the mesh source)");
  check(gClockMs == kT0 + 40, "the clock is the frame's time plus its wait");
  check(gMeshStepBeforeMs < 1000000, "the step was told the clock from before it");
  check(gSource == meshtime::OwnSource::Mesh, "the source is mesh");
  check(gConsole.size() == 1 && has(gConsole[0], "Clock set from mesh time") && has(gConsole[0], "from hub") &&
        has(gConsole[0], "its source: ntp"), "one console line names the sender and its source");
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1, "a frame is applied once");
  std::string s = status();
  check(has(s, "This device's clock: set, from mesh, last set 0 s ago"), "status: set, from mesh");
  check(has(s, "Role: worker. Takes time from the master named on this device when it has no better source"),
        "status: role worker");
  check(has(s, "Last time frame: 0 s ago from hub (its source: ntp). Applied."), "status: last frame applied");
  check(has(s, "Frames: 1 applied, 0 agreed, 0 held, 0 not used, 0 refused"), "status: counts");
  pass(42000);
  s = status();
  check(has(s, "last set 42 s ago") && has(s, "Last time frame: 42 s ago"), "status counts from the step, not from boot");
  std::string j = status("json");
  check(has(j, "\"schema\":1,\"meshTime\":true,\"role\":\"worker\",\"masterNamed\":true,\"clockSet\":true,"
               "\"clockSource\":\"mesh\",\"clockSetAgoS\":42"),
        "json: clock set from mesh, 42 s ago");
  check(has(j, "\"sends\":false,\"sendsWhyNot\":\"not_master\""), "json: a worker does not send");
  check(has(j, "\"lastFrame\":{\"mac\":\"AA:BB:CC:00:00:01\",\"name\":\"hub\",\"agoS\":42,\"source\":\"ntp\",\"sinceSetMin\":3,"
               "\"result\":\"applied\",\"differenceS\":0}"), "json: the last frame");
  check(has(j, "\"counts\":{\"applied\":1,\"agreed\":0,\"held\":0,\"ignored\":0,\"refused\":0}}"), "json: counts");

  // The next round agrees: nothing is set.
  receive(kHub, gClockMs + 300);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1 && has(status(), "It agrees with this device's clock; nothing was changed."),
        "a frame within 5 s changes nothing");
  // The master is a minute ahead: applied.
  pass(600000);
  receive(kHub, gClockMs + 60000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 2, "a difference of a minute is applied");

  // A short payload is counted and dropped; the kept state stays.
  const unsigned before = gMeshSteps;
  receive(kHub, gClockMs + 90000, meshtime::kSrcNtp, 0, meshtime::kFrameLen - 1);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == before, "a short TIME_SYNC payload sets nothing");
  check(has(status(), "1 refused"), "and is counted as refused");

  // A better source of its own: the frame is not used.
  for (meshtime::OwnSource source : { meshtime::OwnSource::Rtc, meshtime::OwnSource::Ntp, meshtime::OwnSource::Manual,
                                      meshtime::OwnSource::Cm5 }) {
    resetAll();
    addPeer(kHub, "hub", true);
    otherSourceSets(source, kT0);
    receive(kHub, kT0 + 120000);
    espnowMeshTimeApplyTick();
    check(gMeshSteps == 0 && gClockMs == kT0, "a device with a better source ignores mesh time");
    check(has(status(), "Not used: this device has a better source ("), "status says which source is better");
    check(has(status("json"), "\"result\":\"ignored_better_source\",\"differenceS\":120"), "json: ignored_better_source");
  }
  // Ring time and a clock carried over a restart are replaced.
  resetAll();
  addPeer(kHub, "hub", true);
  otherSourceSets(meshtime::OwnSource::Ring, kT0);
  receive(kHub, kT0 + 120000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1 && gClockMs == kT0 + 120000, "ring time is replaced by mesh time");
  resetAll();
  addPeer(kHub, "hub", true);
  gClockMs = kT0;  // set, but by no source this boot
  check(has(status(), "This device's clock: set, carried over a restart"), "status: carried over a restart");
  check(has(status("json"), "\"clockSet\":true,\"clockSource\":\"carried\",\"clockSetAgoS\":null"), "json: carried");
  receive(kHub, kT0 + 120000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1, "a clock carried over a restart is replaced by mesh time");

  // Switched off, a sender that is not the named master, this device the master.
  resetAll();
  addPeer(kHub, "hub", true);
  gSettingOn = false;
  receive(kHub, kT0);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0 && has(status(), "Not used: mesh time is switched off.") &&
        has(status(), "Mesh time is switched off (espnowmeshtime on)."), "switched off: nothing is taken");
  resetAll();
  addPeer(kHub, "hub", true);
  addPeer(kShed, "shed", true);
  gHaveNamedPrimary = true;
  std::memcpy(gNamedPrimary, kHub, 6);
  receive(kShed, kT0);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0 && has(status(), "Not used: shed is not the master named on this device."),
        "a paired device that is not the named master cannot set the clock");
  receive(kHub, kT0);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1, "the named master can");
  resetAll();
  addPeer(kShed, "shed", true);
  gRole = 1;
  otherSourceSets(meshtime::OwnSource::Ring, kT0);
  receive(kShed, kT0 + 120000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0 && has(status("json"), "\"result\":\"ignored_master\""), "the master takes no mesh time");

  // A time that is not a real date.
  resetAll();
  addPeer(kHub, "hub", true);
  receive(kHub, 86400000);  // 1970
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0 && has(status(), "Refused: the time in the frame is not a real date."), "1970 is refused");
  receive(kHub, kT0, /*source=*/0);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0, "source 0 is refused");

  // More than an hour: held, said on the console once, applied when repeated.
  resetAll();
  addPeer(kHub, "hub", true);
  otherSourceSets(meshtime::OwnSource::Ring, kT0);
  receive(kHub, gClockMs + 7200000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0 && gClockMs == kT0, "a change of two hours is not applied at once");
  check(has(status(), "Held: it differs from this device's clock by more than an hour (+7200 s). "
                      "It is applied when hub says it again."), "status: held");
  check(gConsole.size() == 1 && has(gConsole[0], "held until hub says it again"), "one console line for the hold");
  pass(5000);
  receive(kHub, gClockMs + 7200000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0 && gConsole.size() == 1, "repeated within 10 s: still held, no second console line");
  pass(10000);
  receive(kHub, gClockMs + 7200000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1 && gClockMs == kT0 + 15000 + 7200000, "repeated after 10 s by the same sender: applied");

  // timeset on another task between the decision and the step: the frame is
  // judged again and loses to the better source; the clock is not touched.
  resetAll();
  addPeer(kHub, "hub", true);
  otherSourceSets(meshtime::OwnSource::Ring, kT0);
  receive(kHub, kT0 + 120000);
  gRaceOnNextRead = true;
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0, "a clock set by another source meanwhile is not overwritten");
  check(gSource == meshtime::OwnSource::Manual && gClockMs == kT0 + 500000, "the other source's time stands");
  check(has(status("json"), "\"result\":\"ignored_better_source\""), "and the frame is recorded as not used");

  // The primary and the backup both send: the second cannot move the clock back.
  resetAll();
  addPeer(kHub, "hub", true);
  addPeer(kShed, "shed", true);
  gHaveNamedBackup = true;
  std::memcpy(gNamedBackup, kShed, 6);
  receive(kHub, kT0);
  espnowMeshTimeApplyTick();
  pass(30000);
  receive(kShed, gClockMs - 600000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1 && has(status(), "Not used: this device follows the time of another sender."),
        "a second sender is not followed while the first is");

  // A name with a quote in it cannot break the JSON reply.
  resetAll();
  addPeer(kHub, "h\"ub\\", true);
  receive(kHub, kT0);
  espnowMeshTimeApplyTick();
  check(has(status("json"), "\"name\":\"h\\\"ub\\\\\""), "the sender's name is escaped in the JSON reply");
}

static void testSender() {
  // The command on a worker, without a clock, with a clock that is not from a real source.
  resetAll();
  addPeer(kPorch, "porch", true);
  check(sync() == "Error: only the mesh master sends time. This device's role is worker.", "a worker does not send");
  gRole = 2;
  check(sync() == "Error: only the mesh master sends time. This device's role is backup master.",
        "a backup that is not promoted does not send");
  gRole = 1;
  // 55 hours after a boot without a time source the clock reads 1970-01-03:
  // still not set (the old `epoch < 100000` test let it through).
  for (int64_t clockMs : { int64_t{0}, int64_t{5000}, int64_t{99999000}, int64_t{100001000}, int64_t{200000000},
                           int64_t{946684800000LL}, int64_t{1577836799000LL} }) {
    gClockMs = clockMs;
    check(sync() == "Error: this device's clock is not set. Nothing sent.", "no time is sent from a clock that is not set");
    runSender(3000);
    check(gSent.empty(), "and the sender step sends nothing");
    gClockMs = 0;
  }
  check(has(status(), "Role: master. Not sending: this device's clock is not set."), "status: master, clock not set");
  gClockMs = kT0;  // carried over a restart
  check(has(sync(), "Error: this device's clock is carried over a restart, which is not a real source"),
        "a clock carried over a restart is not passed on");
  check(has(status("json"), "\"sends\":false,\"sendsWhyNot\":\"source_not_real\""), "json: source_not_real");
  otherSourceSets(meshtime::OwnSource::Ring, kT0);
  check(has(sync(), "Error: this device's clock is ring, which is not a real source (rtc, ntp, manual, cm5)"),
        "ring time is not passed on");
  otherSourceSets(meshtime::OwnSource::Mesh, kT0);
  check(has(sync(), "this device's clock is mesh, which is not a real source"), "mesh time is not passed on");
  runSender(3000);
  check(gSent.empty(), "nothing went out for any of these");
  otherSourceSets(meshtime::OwnSource::Ntp, kT0);
  gClockHeld = true;
  check(has(sync(), "Error: an NTP reply moved this device's clock by more than a day"), "an unconfirmed NTP jump is not sent");
  check(has(status("json"), "\"sendsWhyNot\":\"clock_unconfirmed\""), "json: clock_unconfirmed");
  gClockHeld = false;
  gSettingOn = false;
  check(sync() == "Error: mesh time is switched off (espnowmeshtime on).", "switched off: nothing is sent");
  gSettingOn = true;
  gMeshOn = false;
  check(sync() == "Error: Mesh mode not enabled. Use 'espnowmode mesh' first.", "without mesh mode nothing is sent");
  gHandshakes.clear();
  runSender(3000);
  check(gSent.empty() && gHandshakes.empty(), "and the sender step does nothing at all");

  // A master with NTP time and three paired devices: two with a session.
  resetAll();
  gRole = 1;
  otherSourceSets(meshtime::OwnSource::Ntp, kT0);
  addPeer(kPorch, "porch", true, gUptimeMs - 60000);
  addPeer(kShed, "shed", true, gUptimeMs - 60000);
  addPeer(kHub, "far", false);
  check(has(status("json"), "\"sends\":true,\"sendsWhyNot\":null,\"lastSentAgoS\":null"), "json: sends, nothing sent yet");
  runSender(400);
  check(gSent.size() == 1, "the first round starts at once, one device per half second");
  runSender(500);
  check(gSent.size() == 2 && std::memcmp(gSent[0].mac, gSent[1].mac, 6) != 0, "then the other device");
  check(gSent[0].frame.source == meshtime::kSrcNtp && gSent[0].frame.flags == 0, "the frame names the source ntp");
  check(gSent[0].frame.epochMs >= kT0 && gSent[0].frame.epochMs <= gClockMs, "and carries the master's clock in UTC ms");
  runSender(5000);
  check(gSent.size() == 2, "nothing more until the next round");
  check(!gHandshakes.empty() && gHandshakes[0] == "AA:BB:CC:00:00:01",
        "the master starts a session with the paired device that has none");
  check(has(status(), "Role: master. Sends its clock to paired devices when their session starts and every 10 min.") &&
        has(status(), "This round has reached 2 of 3 paired devices (2 with a session)."), "status: master, 2 of 3");
  // The far device's session comes up: one frame to it, once it has settled.
  gPeers[2].session = true;
  gPeers[2].establishedAtMs = gUptimeMs;
  runSender(1500);
  check(gSent.size() == 2, "a session under 2 s old is not used yet");
  runSender(1500);
  check(gSent.size() == 3 && std::memcmp(gSent[2].mac, kHub, 6) == 0, "then that device gets its frame");
  // The next round after 10 minutes reaches all three.
  runSender(600000);
  check(gSent.size() == 6, "a round every 10 minutes");
  // The master's clock is set (an NTP reply): a round at once and one more 15 s later.
  otherSourceSets(meshtime::OwnSource::Ntp, gClockMs + 4000);
  runSender(2000);
  check(gSent.size() == 9, "a clock step on the master starts a round");
  runSender(15000);
  check(gSent.size() == 12, "and one repeat 15 s later");
  runSender(60000);
  check(gSent.size() == 12, "then quiet again");
  // By hand.
  check(sync() == "OK: time is being sent to 3 of 3 paired devices", "espnowtimesync asks for a round");
  runSender(2000);
  check(gSent.size() == 15, "which the sender step sends");
  gPeers[2].session = false;
  check(sync() == "OK: time is being sent to 2 of 3 paired devices (1 has no session yet)", "the reply counts sessions");
  runSender(2000);
  check(gSent.size() == 17, "the round reaches the two devices with a session");
  // A full queue is tried again at the next look.
  gQueueFull = true;
  check(sync() == "OK: time is being sent to 2 of 3 paired devices (1 has no session yet)", "asked again");
  runSender(1000);
  check(gSent.size() == 17, "nothing while the queue is full");
  gQueueFull = false;
  runSender(2000);
  check(gSent.size() == 19, "sent once the queue takes frames again");
  // A promoted backup says so in the frame.
  gBackupPromoted = true;
  sync();
  runSender(2000);
  check(gSent.back().frame.flags == meshtime::kFlagActingBackup, "a backup acting as master sets flag bit 0");
  // No paired devices.
  resetAll();
  gRole = 1;
  otherSourceSets(meshtime::OwnSource::Rtc, kT0);
  check(sync() == "OK: no paired devices to send time to.", "no paired devices");
  // The step is driven by the 32-bit uptime across its wrap.
  resetAll();
  gRole = 1;
  gUptimeMs = 0xFFFFFE00u;
  otherSourceSets(meshtime::OwnSource::Ntp, kT0);
  addPeer(kPorch, "porch", true, gUptimeMs - 60000);
  runSender(1000);
  check(gSent.size() == 1, "a round before the uptime wrap");
  runSender(600000);
  check(gSent.size() == 2, "the next round 10 minutes later, after the wrap");
}

// What the review of the A-F package found (2026-11-18).
static void testHardening() {
  // A paired device that floods TIME_SYNC: the named master's frame that
  // arrived first is still judged and applied (it was pushed out of a
  // one-frame stash and never judged).
  resetAll();
  addPeer(kHub, "hub", true);
  addPeer(kShed, "shed", true);
  gHaveNamedPrimary = true;
  std::memcpy(gNamedPrimary, kHub, 6);
  receive(kHub, kT0);
  pass(2);
  for (int i = 0; i < 20; ++i) receive(kShed, kT0 + 900000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1 && gClockMs == kT0 + 2, "the named master's frame is applied despite the flood after it");
  check(has(status("json"), "\"ignored\":1,"), "the flooder's newest frame is judged once (not named), its older ones replaced");
  // ... also when the flood comes from four devices: the named master's frame takes the place of one of theirs.
  resetAll();
  addPeer(kHub, "hub", true);
  gHaveNamedPrimary = true;
  std::memcpy(gNamedPrimary, kHub, 6);
  for (uint8_t i = 0; i < 6; ++i) {
    const uint8_t other[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x01, i };
    addPeer(other, "noise", true);
    receive(other, kT0 + 900000);
  }
  receive(kHub, kT0);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1 && gClockMs == kT0, "four kept frames of others do not keep the named master's frame out");
  check(has(status("json"), "\"ignored\":6,"), "every frame that was not judged is counted as not used");
  // The primary's first and the backup's 2 ms later: the backup is not followed.
  resetAll();
  addPeer(kHub, "hub", true);
  addPeer(kShed, "shed", true);
  gHaveNamedBackup = true;
  std::memcpy(gNamedBackup, kShed, 6);
  receive(kHub, kT0);
  pass(2);
  receive(kShed, kT0 + 900000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1 && gClockMs == kT0 + 2, "the first sender's frame is applied");
  check(has(status("json"), "\"result\":\"ignored_other_master\""), "and the second is judged after it");

  // Mesh mode off: no time is taken, as the status says.
  resetAll();
  addPeer(kHub, "hub", true);
  gMeshOn = false;
  receive(kHub, kT0);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0 && gClockMs < 1000000, "mesh mode off: the clock is not set");
  std::string s = status();
  check(has(s, "Mesh mode is off: no time is sent or taken.") && has(s, "Not used: mesh mode is off."),
        "and the status says so, truly");
  check(has(status("json"), "\"result\":\"ignored_mesh_off\""), "json: ignored_mesh_off");

  // No master named: no mesh time at all, from any paired device, also while
  // the clock is not set; the status says so and names the command.
  for (bool clockSet : { false, true }) {
    resetAll();
    gHaveNamedPrimary = false;
    addPeer(kHub, "hub", true);
    addPeer(kShed, "shed", true);
    if (clockSet) otherSourceSets(meshtime::OwnSource::Ring, kT0);
    const int64_t clockBefore = gClockMs;
    check(has(status(), "Role: worker. Takes no mesh time: no master is named on this device. "
                        "Name it with 'espnowmasterfingerprint <64-hex-pubkey>'."),
          "status: no master named, and the command that names it");
    check(has(status("json"), "\"role\":\"worker\",\"masterNamed\":false,"), "json: masterNamed false");
    for (int i = 0; i < 60; ++i) {  // ten minutes, a frame every 10 s, 59 minutes off each time
      receive((i % 2) ? kShed : kHub, (clockSet ? gClockMs : kT0) + 59LL * 60 * 1000, meshtime::kSrcNtp,
              (i % 3) ? 0 : meshtime::kFlagActingBackup);
      espnowMeshTimeApplyTick();
      pass(10000);
    }
    check(gMeshSteps == 0 && (clockSet ? gClockMs == clockBefore + 600000 : gClockMs < 1000000),
          "no frame sets the clock");
    check(has(status(), "Not used: no master is named on this device (espnowmasterfingerprint)."),
          "the last frame says why");
    check(has(status("json"), "\"result\":\"ignored_no_master\"") && has(status("json"), "\"ignored\":60,"),
          "json: ignored_no_master, every frame counted as not used");
    check(gConsole.empty(), "and nothing is said on the console");
  }
  // gRole 2 (backup, not promoted) with no master named likewise.
  resetAll();
  gHaveNamedPrimary = false;
  gRole = 2;
  check(has(status(), "Role: backup master. Takes no mesh time: no master is named on this device."), "a backup likewise");
  // Only the backup master named: its frames are taken.
  resetAll();
  gHaveNamedPrimary = false;
  gHaveNamedBackup = true;
  std::memcpy(gNamedBackup, kShed, 6);
  addPeer(kShed, "shed", true);
  check(has(status(), "Role: worker. Takes time from the master named on this device"), "a named backup is a named master");
  receive(kShed, kT0, meshtime::kSrcNtp, meshtime::kFlagActingBackup);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1 && gClockMs == kT0, "the named backup acting as master sets the clock");
  // A named master's frame steps after 10 s, every time.
  resetAll();
  addPeer(kHub, "hub", true);
  otherSourceSets(meshtime::OwnSource::Ring, kT0);
  receive(kHub, gClockMs + 120000);
  espnowMeshTimeApplyTick();
  pass(11000);
  receive(kHub, gClockMs + 120000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 2, "the named master corrects again after 10 s");
  pass(1000);
  receive(kHub, gClockMs + 120000);
  espnowMeshTimeApplyTick();
  check(has(status(), "Not used: this device's clock was set a moment before."), "within 10 s: the short text");

  // A hostile time: refused, no difference computed from it.
  resetAll();
  addPeer(kHub, "hub", true);
  otherSourceSets(meshtime::OwnSource::Ring, kT0);
  receive(kHub, INT64_MAX);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 0 && has(status("json"), "\"result\":\"refused_bad_time\",\"differenceS\":0}"),
        "INT64_MAX is refused and shows a difference of 0");

  // The "held" line: a sender that takes turns between two times prints at
  // most one line a minute (it printed every second frame).
  resetAll();
  addPeer(kHub, "hub", true);
  otherSourceSets(meshtime::OwnSource::Ring, kT0);
  for (int i = 0; i < 200; ++i) {
    receive(kHub, (i % 2) ? gClockMs : gClockMs + 7200000);
    espnowMeshTimeApplyTick();
    pass(5);
  }
  check(gConsole.size() == 1, "200 frames in a second: one console line, not 100");
  pass(60000);
  receive(kHub, gClockMs + 7200000);
  espnowMeshTimeApplyTick();
  check(gConsole.size() == 2, "and the next a minute later");

  // The frame names the master's own source and the minutes since it was set.
  for (meshtime::OwnSource source : { meshtime::OwnSource::Rtc, meshtime::OwnSource::Manual, meshtime::OwnSource::Cm5,
                                      meshtime::OwnSource::Ntp }) {
    resetAll();
    gRole = 1;
    otherSourceSets(source, kT0);
    pass(185000);  // set 3 minutes and 5 s ago
    addPeer(kPorch, "porch", true, gUptimeMs - 60000);
    runSender(400);
    check(gSent.size() == 1 && gSent[0].frame.source == meshtime::wireSourceFor(source) &&
          gSent[0].frame.source != 0, "the frame carries the master's own source");
    check(gSent.size() == 1 && gSent[0].frame.sinceSetMin == 3, "and the whole minutes since it was set");
  }
  // A master with mesh mode off starts no handshake with a device without a session.
  resetAll();
  gRole = 1;
  gMeshOn = false;
  otherSourceSets(meshtime::OwnSource::Ntp, kT0);
  addPeer(kPorch, "porch", false);
  runSender(3000);
  check(gHandshakes.empty() && gSent.empty(), "mesh mode off: no handshake, no frame");
  gMeshOn = true;
  runSender(1000);
  check(!gHandshakes.empty(), "(with mesh mode on it starts one)");
  // The JSON names the backup master's role.
  resetAll();
  gRole = 2;
  check(has(status("json"), "\"role\":\"backup\""), "json: a backup master is 'backup'");
  gRole = 0;
  check(has(status("json"), "\"role\":\"worker\""), "json: a worker is 'worker'");

  // Which sources may be written into the RTC chip: not the chip itself, not
  // mesh time, not a clock carried over a restart.
  check(Clock::sourceWritesRtc(Clock::SYNC_NTP) && Clock::sourceWritesRtc(Clock::SYNC_MANUAL) &&
        Clock::sourceWritesRtc(Clock::SYNC_RING) && Clock::sourceWritesRtc(Clock::SYNC_CM5),
        "ntp, timeset, the ring and cm5 are written into the RTC chip");
  check(!Clock::sourceWritesRtc(Clock::SYNC_MESH) && !Clock::sourceWritesRtc(Clock::SYNC_NONE) &&
        !Clock::sourceWritesRtc(Clock::SYNC_RTC), "mesh time, a carried clock and the chip's own are not");
}

// A master named in place of the one followed is followed with its first
// frame (it used to wait out the 25 minutes the old one was followed for).
static void testRenamedMaster() {
  resetAll();
  addPeer(kHub, "hub", true);
  addPeer(kShed, "shed", true);
  receive(kHub, kT0);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 1, "hub, the master named here, sets the clock and is followed");
  pass(60000);
  std::memcpy(gNamedPrimary, kShed, 6);  // espnowmasterfingerprint <shed's key>
  receive(kShed, gClockMs + 120000);
  espnowMeshTimeApplyTick();
  check(gMeshSteps == 2 && has(status("json"), "\"result\":\"applied\""),
        "shed, named in hub's place, sets the clock with its first frame");
}

int main() {
  testFollower();
  testSender();
  testHardening();
  testRenamedMaster();
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d mesh time checks failed\n", failures, checks);
    return 1;
  }
  std::printf("ESP-NOW mesh time: %d checks passed\n", checks);
  return 0;
}
