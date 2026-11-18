// Host harness for the sensor reading path: the production send and receive
// code of System_ESPNow_Sensors.cpp (the blocks between its SENSOR_*_HOST_BEGIN
// and _END marks, remoteSensorTableEnsure and getRemoteSensorDataJson),
// compiled as it is with the production rules header
// System_ESPNow_SensorCore.h, the production WarnLimiter
// (System_ESPNow_HandshakeBackoff.h), V4PayloadSensorStatus and the reading
// size constants of System_ESPNow_Wire.h and RemoteSensorInfo of
// System_ESPNow_Sensors.h.
//
// Host boundaries, all in this file: the 32-bit uptime, the clock module
// (set or not, its source), the paired-device lookup, the settings, the
// PSRAM allocator, the locks (they record whether anything that may block or
// print runs under them), the console, the sessions and the radio queue. The
// radio queue is a loopback: every part the production sender hands to it is
// delivered to the production receiver, unless a case says it is lost.
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_ESPNow_ForwardCore.h"
#include "System_ESPNow_HandshakeBackoff.h"
#include "System_ESPNow_PowerEventCore.h"  // jsonTextEscape (the via's name in a reply)
#include "System_ESPNow_SensorCore.h"

// ---- Arduino String, as far as the JSON reply uses it ---------------------------
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  void reserve(size_t n) { text_.reserve(n); }
  String& operator+=(const char* more) { text_ += more; return *this; }
  const char* c_str() const { return text_.c_str(); }
  const std::string& text() const { return text_; }
 private:
  std::string text_;
};

#define EXT_RAM_BSS_ATTR
#define MESH_PEER_MAX 16
#define MAC_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_ARGS(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

// ---- Locks: nothing that may block, print or allocate runs under one -------------
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
static int gLockDepth = 0;
static unsigned gLockViolations = 0;
#define taskENTER_CRITICAL(mux) ((void)(mux), ++gLockDepth)
#define taskEXIT_CRITICAL(mux)  ((void)(mux), --gLockDepth)
static void notUnderLock() {
  if (gLockDepth != 0) ++gLockViolations;
}
static portMUX_TYPE gSensorBcastTaskMux = 0;

// ---- Uptime, clock, settings, peers ------------------------------------------------
static uint32_t gNowMs = 0;
static uint32_t millis() { return gNowMs; }

static bool gClockSet = false;
static int64_t gEpochMs = 0;
namespace Clock {
enum SyncSource : uint8_t { SYNC_NONE = 0, SYNC_RTC, SYNC_NTP, SYNC_MANUAL, SYNC_RING, SYNC_CM5, SYNC_MESH };
static SyncSource gSource = SYNC_NONE;
static bool isSynced() { notUnderLock(); return gClockSet; }
static int64_t epochMillis() { notUnderLock(); return gEpochMs; }
static SyncSource syncSource() { return gSource; }
static const char* syncSourceName(SyncSource s) {
  switch (s) {
    case SYNC_RTC: return "rtc";
    case SYNC_NTP: return "ntp";
    case SYNC_MANUAL: return "manual";
    case SYNC_RING: return "ring";
    case SYNC_CM5: return "cm5";
    case SYNC_MESH: return "mesh";
    default: return "none";
  }
}
}  // namespace Clock

static struct { uint8_t meshRole = 0; } gSettings;
static uint32_t gBootCounter = 0x0001A2B3;
static uint32_t esp_random() { return 0x5EED5EED; }

struct PeerIdentity { uint8_t mac[6]; };
static std::vector<PeerIdentity> gPaired;
static const PeerIdentity* peerIdentityFindByMac(const uint8_t* mac) {
  notUnderLock();
  for (const auto& p : gPaired) {
    if (std::memcmp(p.mac, mac, 6) == 0) return &p;
  }
  return nullptr;
}

// ---- Allocator, console ---------------------------------------------------------------
enum class AllocPref { PreferPsram, RequirePsram };
static unsigned gAllocs = 0;
static bool gAllocFails = false;
static void* ps_alloc(size_t bytes, AllocPref, const char*) {
  notUnderLock();
  ++gAllocs;
  return gAllocFails ? nullptr : std::malloc(bytes);
}
static std::vector<std::string> gConsole;
static void consoleLine(const char* fmt, ...) {
  notUnderLock();
  char line[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  gConsole.push_back(line);
}
#define BROADCAST_PRINTF(...) consoleLine(__VA_ARGS__)
#define ERROR_SYSTEMF(...) consoleLine(__VA_ARGS__)
static void debugSink(const char*, ...) {}
#define DEBUG_ESPNOW_MESHF(...) debugSink(__VA_ARGS__)

// INSERT_WIRE

// INSERT_SENSORS_H

// INSERT_RX_STATE

// INSERT_TX_STATE

// ---- What the send path asks of the transport -----------------------------------------
struct TestSession {
  uint8_t mac[6];
  bool up;
  bool routed;
};
static std::vector<TestSession> gSessions;
static TestSession* sessionOf(const uint8_t* mac) {
  for (auto& s : gSessions) {
    if (std::memcmp(s.mac, mac, 6) == 0) return &s;
  }
  return nullptr;
}
static bool espnowSessionUp(const uint8_t* mac, uint32_t* establishedAtMs = nullptr) {
  (void)establishedAtMs;
  const TestSession* s = sessionOf(mac);
  return s && s->up;
}
static uint16_t v4_single_frame_cap(const uint8_t* dst) {
  const TestSession* s = sessionOf(dst);
  return (s && s->routed) ? 130 : 202;
}
static std::vector<std::string> gHandshakes;
static bool espnowHandshakeAutoStart(const uint8_t* mac) {
  char text[18];
  std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  gHandshakes.push_back(text);
  return true;
}

// The radio queue: a loopback into the production receiver.
struct SentPart {
  uint8_t dst[6];
  bool viaBond;
  std::vector<uint8_t> bytes;
};
static std::vector<SentPart> gSentParts;
static int gQueueFullAtPart = -1;    // the n-th part handed over from now on is refused (0-based)
static int gLosePart = -1;           // the n-th part handed over from now on is lost on the air
static int gPartsHandedOver = 0;
static uint8_t gSelfMac[6] = { 0x02, 0x5E, 0x00, 0x00, 0x00, 0x01 };
static bool gLoopback = true;

void remoteSensorIngestPart(const uint8_t* senderMac, const char* senderName,
                            const uint8_t* payload, size_t payloadLen, bool fromBond);

static bool v4_send_sensor_part(const uint8_t* dstMac, bool viaBond, const uint8_t* part, uint16_t len) {
  notUnderLock();
  const int index = gPartsHandedOver++;
  if (index == gQueueFullAtPart) return false;
  SentPart sent{};
  std::memcpy(sent.dst, dstMac, 6);
  sent.viaBond = viaBond;
  sent.bytes.assign(part, part + len);
  gSentParts.push_back(sent);
  if (index == gLosePart) return true;
  if (gLoopback) remoteSensorIngestPart(gSelfMac, "self", sent.bytes.data(), sent.bytes.size(), viaBond);
  return true;
}

// INSERT_HELPERS

// ---- What the receive path asks of the forwarding code -------------------------------
// A link table of one entry (System_ESPNow_ForwardRx.cpp keeps sixteen): the
// production rule espnowfwd::attribute decides, as on the device.
static espnowfwd::ForwardLink gLink = {};
static unsigned gResumeTaken = 0;
struct ForwardedReading {
  uint8_t src[6];
  espnowsensor::ReadingHeader h;
  std::string text;
};
static std::vector<ForwardedReading> gForwarded;
static std::vector<uint8_t> gNotKept;
static ForwardResolve sensorWhose(const uint8_t* src, const char* srcName, bool fromBond, bool bitSet, uint8_t fwdLink,
                                  uint8_t sensorType, ForwardOrigin* out) {
  notUnderLock();
  std::memset(out, 0, sizeof(*out));
  std::memcpy(out->mac, src, 6);
  out->link = fromBond ? espnowfwd::kLinkBond : espnowfwd::kLinkMesh;
  std::snprintf(out->name, sizeof(out->name), "%s", srcName ? srcName : "");
  const espnowfwd::ForwardLink* e = gLink.inUse && std::memcmp(gLink.via, src, 6) == 0 ? &gLink : nullptr;
  const espnowfwd::Attribution a = espnowfwd::attribute(e, fromBond, true, bitSet, fwdLink, sensorType);
  switch (a.verdict) {
    case espnowfwd::Verdict::Own: return ForwardResolve::Own;
    case espnowfwd::Verdict::Malformed: return ForwardResolve::Malformed;
    case espnowfwd::Verdict::Forwarded:
      std::memcpy(out->mac, e->partner, 6);
      std::memcpy(out->via, src, 6);
      out->link = espnowfwd::kLinkForwarded;
      out->resumePending = a.resumePending;
      std::memcpy(out->name, e->name, sizeof(out->name));
      return ForwardResolve::Forwarded;
    default:
      return ForwardResolve::Refused;
  }
}
static void sensorResumeTaken(const uint8_t via[6], uint8_t sensorType) {
  notUnderLock();
  if (gLink.inUse && std::memcmp(gLink.via, via, 6) == 0) gLink.resumeMask = (uint16_t)(gLink.resumeMask & ~(1u << sensorType));
  ++gResumeTaken;
}
static bool sensorLinkActive(const uint8_t via[6], const uint8_t origin[6]) {
  notUnderLock();
  return gLink.inUse && gLink.state == espnowfwd::LState::Active && std::memcmp(gLink.via, via, 6) == 0 &&
         std::memcmp(gLink.partner, origin, 6) == 0;
}
static void sensorForwardReading(const uint8_t src[6], const espnowsensor::ReadingHeader& h, const uint8_t* text,
                                 size_t len) {
  notUnderLock();
  ForwardedReading f{};
  std::memcpy(f.src, src, 6);
  f.h = h;
  f.text.assign((const char*)text, len);
  gForwarded.push_back(f);
}
static void sensorNotKept(uint8_t sensorType) {
  notUnderLock();
  gNotKept.push_back(sensorType);
}
// The status frame's question (the byte alone, the status stream id), by the
// same production rule.
static ForwardResolve sensorStatusWhose(const uint8_t* src, const char* srcName, bool fromBond, uint8_t fwdLink,
                                        ForwardOrigin* out) {
  notUnderLock();
  std::memset(out, 0, sizeof(*out));
  std::memcpy(out->mac, src, 6);
  out->link = fromBond ? espnowfwd::kLinkBond : espnowfwd::kLinkMesh;
  std::snprintf(out->name, sizeof(out->name), "%s", srcName ? srcName : "");
  const espnowfwd::ForwardLink* e = gLink.inUse && std::memcmp(gLink.via, src, 6) == 0 ? &gLink : nullptr;
  const espnowfwd::Attribution a = espnowfwd::attribute(e, fromBond, false, false, fwdLink, espnowfwd::kStreamIdStatus);
  switch (a.verdict) {
    case espnowfwd::Verdict::Own: return ForwardResolve::Own;
    case espnowfwd::Verdict::Malformed: return ForwardResolve::Malformed;
    case espnowfwd::Verdict::Forwarded:
      std::memcpy(out->mac, e->partner, 6);
      std::memcpy(out->via, src, 6);
      out->link = espnowfwd::kLinkForwarded;
      std::memcpy(out->name, e->name, sizeof(out->name));
      return ForwardResolve::Forwarded;
    default:
      return ForwardResolve::Refused;
  }
}
struct ForwardedStatus {
  uint8_t src[6];
  espnowfwd::StatusFrame f;
};
static std::vector<ForwardedStatus> gForwardedStatus;
static void sensorForwardStatus(const uint8_t src[6], const espnowfwd::StatusFrame& f) {
  notUnderLock();
  ForwardedStatus fs{};
  std::memcpy(fs.src, src, 6);
  fs.f = f;
  gForwardedStatus.push_back(fs);
}
static void sensorViaName(const uint8_t mac[6], char* out, size_t cap) {
  std::snprintf(out, cap, "%s", mac[5] == 0x20 ? "glasses" : "via?");
}

static espnowsensor::TargetChoice gTarget = {};
static int gTargetAskedFor = -99;
static espnowsensor::TargetChoice sensorReadingTarget(int sensorType) {
  gTargetAskedFor = sensorType;
  return gTarget;
}
// A bond worker's sensor status to its bond master once per bond session
// (System_ESPNow_Sensors.cpp, outside the host block).
static unsigned gBondStatusTicks = 0;
static void sensorBondStatusTick(uint32_t) {
  notUnderLock();
  ++gBondStatusTicks;
}
// The lookup of the named masters among the paired devices (espnow_task).
static unsigned gTargetRefreshes = 0;
static void sensorReceiverTargetsRefresh() {
  notUnderLock();
  ++gTargetRefreshes;
}

// INSERT_RX

// INSERT_TX

// INSERT_JSON

// ---- Cases --------------------------------------------------------------------------------
static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}
static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

static const uint8_t kPorch[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t kShed[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x02 };
static const uint8_t kHub[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x10 };
static const uint8_t kBackup[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x11 };

static void pair(const uint8_t mac[6]) {
  PeerIdentity p{};
  std::memcpy(p.mac, mac, 6);
  gPaired.push_back(p);
}
static void resetAll() {
  // The table stays (it is made once); every sender's slots are given back,
  // so each case makes its own.
  for (int i = 0; gSensorPeers && i < REMOTE_SENSOR_PEERS_MAX; ++i) std::free(gSensorPeers[i].slots);
  if (gSensorPeers) std::memset(gSensorPeers, 0, (size_t)REMOTE_SENSOR_PEERS_MAX * sizeof(SensorPeerRecord));
  gRemoteSensorRefusals = RemoteSensorRefusals{};
  gSensorRoleRefusalWarn = WarnLimiter{};
  gSensorFullWarn = WarnLimiter{};
  gAllocFails = false;
  std::memset(gSensorTxCounter, 0, sizeof(gSensorTxCounter));
  std::memset(gSensorTxTarget, 0, sizeof(gSensorTxTarget));
  std::memset(gSensorTxStats, 0, sizeof(gSensorTxStats));
  for (int i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    gSensorDropSinceLine[i].store(0);
    gSensorDropLastReason[i].store(0);
    gSensorDropWarn[i] = WarnLimiter{};
  }
  gSensorNeedSession = false;
  gNowMs += 1000000;  // every case starts later than the last one ended
  gClockSet = false;
  gEpochMs = 86400000;  // what a clock that is not set reads (1970): never a stamp
  Clock::gSource = Clock::SYNC_NONE;
  gSettings.meshRole = 1;
  gPaired.clear();
  pair(kPorch);
  pair(kShed);
  pair(gSelfMac);
  gSessions.clear();
  gHandshakes.clear();
  gSentParts.clear();
  gConsole.clear();
  gQueueFullAtPart = gLosePart = -1;
  gPartsHandedOver = 0;
  gLoopback = true;
  gTarget = espnowsensor::TargetChoice{};
}

static std::string jsonOf(size_t len) {
  std::string s = "{\"k\":\"";
  while (s.size() + 2 < len) s.push_back((char)('a' + (s.size() % 26)));
  s += "\"}";
  return s;
}

// One reading as its sender would cut it, delivered part by part.
static void deliver(const uint8_t sender[6], const char* name, uint8_t sensor, uint16_t tag, uint16_t counter,
                    const std::string& text, bool fromBond = false, uint8_t flags = 0, uint16_t cap = 202,
                    int losePart = -1) {
  espnowsensor::PartPlan plan;
  if (!espnowsensor::planParts(text.size(), cap, plan)) {
    std::fprintf(stderr, "bad test reading of %zu bytes\n", text.size());
    std::exit(2);
  }
  for (uint8_t i = 0; i < plan.count; ++i) {
    if ((int)i == losePart) continue;
    size_t offset = 0, n = 0;
    espnowsensor::partSpan(plan, text.size(), i, offset, n);
    espnowsensor::ReadingHeader h{};
    h.sensorType = sensor;
    h.flags = flags;
    h.bootTag = tag;
    h.counter = counter;
    h.partIndex = i;
    h.partCount = plan.count;
    std::vector<uint8_t> frame(espnowsensor::kReadingHeaderLen + n);
    espnowsensor::packReadingHeader(frame.data(), h);
    std::memcpy(frame.data() + espnowsensor::kReadingHeaderLen, text.data() + offset, n);
    remoteSensorIngestPart(sender, name, frame.data(), frame.size(), fromBond);
  }
}
static std::string textOf(const uint8_t mac[6], RemoteSensorType type) {
  // A buffer of exactly the stated size: a longer copy is an ASan error.
  std::vector<char> out(SENSOR_READING_MAX_BYTES + 1);
  remoteSensorCopyText(mac, nullptr, type, out.data(), out.size());
  return out.data();
}
static espnowsensor::CounterCounts countsOf(const uint8_t mac[6], int sensor) {
  for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; ++i) {
    if (gSensorPeers[i].inUse && std::memcmp(gSensorPeers[i].mac, mac, 6) == 0) return gSensorPeers[i].counts[sensor];
  }
  return espnowsensor::CounterCounts{};
}
static uint32_t incompleteOf(const uint8_t mac[6]) {
  for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; ++i) {
    if (gSensorPeers[i].inUse && std::memcmp(gSensorPeers[i].mac, mac, 6) == 0) return gSensorPeers[i].incomplete;
  }
  return 0;
}
static int usedSlots() {
  int n = 0;
  for (int i = 0; i < remoteSensorSlotCount(); ++i) {
    RemoteSensorInfo info;
    if (remoteSensorInfoAt(i, &info)) ++n;
  }
  return n;
}
static std::string json(const uint8_t mac[6], RemoteSensorType type) { return getRemoteSensorDataJson(mac, nullptr, type).text(); }

static void testBeforeTheTable() {
  // A worker keeps nothing: a status frame and a reading are refused and
  // counted, said once, and nothing is allocated (S14: only a device that is
  // sent readings and keeps them has a table).
  gSettings.meshRole = 0;
  gNowMs = 1000000;  // not at uptime 0, which the console limiter reads as "never printed"
  pair(kPorch);
  V4PayloadSensorStatus status{};
  status.presentMask = 0x7FF;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, jsonOf(60));
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 2, jsonOf(60));
  check(gSensorPeers == nullptr && gAllocs == 0, "a worker that is sent readings allocates nothing");
  check(gRemoteSensorRefusals.notReceiver == 3, "and counts each refused frame");
  check(gConsole.size() == 1 && has(gConsole[0], "Readings from porch refused: this device is not a mesh master or backup master"),
        "and says so once a minute");
  check(remoteSensorSlotCount() == 0, "no slots while this device keeps no readings");
  RemoteSensorInfo info;
  check(!remoteSensorInfoAt(0, &info) && !remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info), "readers find nothing");
  check(json(kPorch, REMOTE_SENSOR_GPS) == "{\"connected\":false,\"enabled\":false,\"fresh\":false,\"data\":null}",
        "the JSON reply of an unknown sensor");
  // A master, out of memory: a status frame is refused, counted and said as a
  // reading is; nothing is half made.
  gSettings.meshRole = 1;
  gConsole.clear();
  gAllocFails = true;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  check(gSensorPeers == nullptr && gRemoteSensorRefusals.noMemory == 1, "no memory: the status frame is refused and counted");
  check(gConsole.size() == 2 && has(gConsole[0], "Failed to allocate") &&
        has(gConsole[1], "Readings from porch refused: this device cannot keep readings, it is out of memory"),
        "and the failure is said");
  // ... and a reading the same way (the refusal line comes once a minute).
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 3, jsonOf(60));
  check(gSensorPeers == nullptr && gRemoteSensorRefusals.noMemory == 2, "no memory: the reading is refused and counted");
  check(gConsole.size() == 3 && has(gConsole[2], "Failed to allocate"), "and its refusal line waits for the minute");
  check(!remoteSensorTableEnsure(), "no table without memory");
  gAllocFails = false;
  // The first status frame a master keeps makes the table and that sender's
  // slots: its sensors are listed, present and not streaming, before any
  // reading arrives, and no reading is counted.
  const unsigned before = gAllocs;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  check(gSensorPeers != nullptr && gAllocs == before + 2 && usedSlots() == REMOTE_SENSOR_MAX,
        "the first status frame makes the table and the sender's slots, and lists its sensors");
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) && !info.enabled && !info.fresh && info.jsonLength == 0,
        "present, not streaming, no reading");
  check(countsOf(kPorch, REMOTE_SENSOR_GPS).received == 0 && countsOf(kPorch, REMOTE_SENSOR_GPS).lost == 0 &&
        textOf(kPorch, REMOTE_SENSOR_GPS).empty(), "nothing is counted or kept as a reading");
  // The first reading after it uses the same table and slots and is kept.
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 4, jsonOf(60));
  check(gAllocs == before + 2 && textOf(kPorch, REMOTE_SENSOR_GPS) == jsonOf(60) &&
        countsOf(kPorch, REMOTE_SENSOR_GPS).received == 1 && countsOf(kPorch, REMOTE_SENSOR_GPS).lost == 0,
        "the first reading makes nothing new, is kept and counted as the first");
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 7, 1, jsonOf(60));
  check(gAllocs == before + 2 && remoteSensorTableEnsure() && gAllocs == before + 2, "a second sensor of that sender makes nothing new");
  check(remoteSensorSlotCount() == REMOTE_SENSOR_SLOTS_MAX && REMOTE_SENSOR_SLOTS_MAX == 176, "one slot per sender and sensor type");
  // The slots of a sender are made by its first reading that is whole and well formed.
  pair(kShed);
  deliver(kShed, "shed", REMOTE_SENSOR_GPS, 7, 1, "not json");
  check(gAllocs == before + 2 && usedSlots() == REMOTE_SENSOR_MAX, "a malformed first reading makes no slots");
}

static void testRoles() {
  // S15: a worker keeps nothing that the mesh sends it; a backup master and
  // the master keep it. The bond frame is kept whatever the mesh role (the
  // receive table has already required the bond partner).
  for (uint8_t role : { (uint8_t)0, (uint8_t)2, (uint8_t)1 }) {
    resetAll();
    gSettings.meshRole = role;
    deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{\"lat\":1}");
    RemoteSensorInfo info;
    const bool kept = remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info);
    check(kept == (role != 0), role == 0 ? "a worker refuses a mesh reading" : "a master or backup master keeps a mesh reading");
    check(gRemoteSensorRefusals.notReceiver == (role == 0 ? 1u : 0u), "counted when refused");
    deliver(kShed, "shed", REMOTE_SENSOR_GPS, 7, 1, "{\"lat\":2}", /*fromBond=*/true);
    check(remoteSensorInfoFor(kShed, nullptr, REMOTE_SENSOR_GPS, &info), "the bond partner's reading is kept whatever the mesh role");
    V4PayloadSensorStatus status{};
    status.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
    remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
    check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_IMU, &info) == (role != 0), "the status frame follows the same role rule");
  }
}

static void testArrivalStamp() {
  resetAll();
  // The receiver's clock is not set: epoch 0 (null in JSON), uptime kept.
  const uint32_t t0 = gNowMs;
  const std::string t0Text = std::to_string(t0);
  const std::string t1Text = std::to_string(t0 + 1000);
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{\"lat\":1.5}");
  RemoteSensorInfo info;
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info), "the reading has a slot");
  check(info.arrivalEpochMs == 0 && info.lastUpdate == t0 && info.fresh && info.enabled && info.counter == 1,
        "clock not set: epoch 0, the uptime is the stamp");
  check(std::string(info.deviceName) == "porch" && info.jsonLength == 11, "name and length");
  check(textOf(kPorch, REMOTE_SENSOR_GPS) == "{\"lat\":1.5}", "the text is stored as it came");
  std::string j = json(kPorch, REMOTE_SENSOR_GPS);
  check(has(j, "\"arrivalEpochMs\":null,\"arrivalUptimeMs\":" + t0Text + ",\"agoMs\":0,\"clock\":\"none\",\"bootTag\":7,\"counter\":1"),
        "JSON: epoch null, clock none");
  check(has(j, "\"received\":1,\"lost\":0,\"late\":0,\"repeated\":0,\"restarts\":0,\"resumes\":0,\"incomplete\":0"),
        "JSON: the counts");
  check(has(j, ",\"data\":{\"lat\":1.5}}") && has(j, "{\"connected\":true,\"enabled\":true,\"fresh\":true"), "JSON: the data");
  // The clock is set from NTP: the stamp is the receiver's UTC epoch in ms.
  gClockSet = true;
  gEpochMs = 1763388202417LL;
  Clock::gSource = Clock::SYNC_NTP;
  gNowMs += 1000;
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 2, "{\"lat\":1.6}");
  remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info);
  check(info.arrivalEpochMs == 1763388202417LL && info.lastUpdate == t0 + 1000 && info.arrivalClock == Clock::SYNC_NTP,
        "clock set: the receiver's epoch in ms and its source");
  gNowMs += 310;
  j = json(kPorch, REMOTE_SENSOR_GPS);
  check(has(j, "\"arrivalEpochMs\":1763388202417,\"arrivalUptimeMs\":" + t1Text + ",\"agoMs\":310,\"clock\":\"ntp\",\"bootTag\":7,\"counter\":2"),
        "JSON: epoch, uptime, age, clock ntp");
  // A clock no source has set since boot.
  Clock::gSource = Clock::SYNC_NONE;
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 3, "{\"lat\":1.7}");
  check(has(json(kPorch, REMOTE_SENSOR_GPS), "\"clock\":\"carried\""), "JSON: a clock carried over a restart");
  Clock::gSource = Clock::SYNC_MESH;
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 4, "{\"lat\":1.8}");
  check(has(json(kPorch, REMOTE_SENSOR_GPS), "\"clock\":\"mesh\""), "JSON: a clock taken from the mesh says so");
  // After 30 s without a reading it is not fresh and the data is null; the stamp stays.
  gNowMs += REMOTE_SENSOR_TTL_MS + 1;
  j = json(kPorch, REMOTE_SENSOR_GPS);
  check(has(j, "\"fresh\":false") && has(j, "\"data\":null}") && has(j, "\"counter\":4"), "stale: no data, the stamp stays");
}

static void testCounterAndStore() {
  resetAll();
  gClockSet = true;
  gEpochMs = 1763388000000LL;
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 7, 10, "{\"a\":10}");
  gNowMs += 500;
  gEpochMs += 500;
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 7, 13, "{\"a\":13}");  // 11 and 12 never came
  check(countsOf(kPorch, REMOTE_SENSOR_IMU).lost == 2 && textOf(kPorch, REMOTE_SENSOR_IMU) == "{\"a\":13}", "a gap is counted lost");
  RemoteSensorInfo before;
  remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_IMU, &before);
  // A repeat and a late reading change neither the text nor the stamp.
  gNowMs += 200;
  gEpochMs += 200;
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 7, 13, "{\"a\":\"again\"}");
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 7, 12, "{\"a\":12}");
  RemoteSensorInfo after;
  remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_IMU, &after);
  check(textOf(kPorch, REMOTE_SENSOR_IMU) == "{\"a\":13}", "a repeat and a late reading do not replace the newer text");
  check(after.lastUpdate == before.lastUpdate && after.arrivalEpochMs == before.arrivalEpochMs && after.counter == 13,
        "nor its arrival stamp");
  const espnowsensor::CounterCounts c = countsOf(kPorch, REMOTE_SENSOR_IMU);
  check(c.received == 2 && c.repeated == 1 && c.late == 1 && c.lost == 2, "repeat and late are counted");
  // A restart of the sender (another boot tag): kept, no loss.
  gNowMs += 500;
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 8, 1, "{\"a\":1}");
  check(countsOf(kPorch, REMOTE_SENSOR_IMU).restarts == 1 && countsOf(kPorch, REMOTE_SENSOR_IMU).lost == 2 &&
        textOf(kPorch, REMOTE_SENSOR_IMU) == "{\"a\":1}", "a restart is kept and counts no loss");
  // Counters are per sensor and per sender.
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 8, 500, "{\"g\":1}");
  deliver(kShed, "shed", REMOTE_SENSOR_IMU, 3, 900, "{\"s\":1}");
  check(countsOf(kPorch, REMOTE_SENSOR_GPS).lost == 0 && countsOf(kShed, REMOTE_SENSOR_IMU).lost == 0 &&
        countsOf(kPorch, REMOTE_SENSOR_IMU).received == 3, "each sender and sensor has its own count");
  // The stream-start flag: the sender sent elsewhere meanwhile.
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 8, 9000, "{\"g\":2}", false, espnowsensor::kReadingFlagStreamStart);
  check(countsOf(kPorch, REMOTE_SENSOR_GPS).resumes == 1 && countsOf(kPorch, REMOTE_SENSOR_GPS).lost == 0,
        "a stream that starts again counts no loss");

  // A radio outage longer than the presence time: readers no longer list the
  // sensor, and listing it changes nothing. The next reading counts what was
  // lost meanwhile (the counter state is not in the slot).
  resetAll();
  for (uint16_t n = 1; n <= 5; ++n) {
    deliver(kPorch, "porch", REMOTE_SENSOR_TOF, 7, n, "{\"d\":1}");
    gNowMs += 500;
  }
  gNowMs += 120000;
  RemoteSensorInfo info;
  check(!remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info) && usedSlots() == 0, "after two minutes of silence it is not listed");
  check(json(kPorch, REMOTE_SENSOR_TOF) == "{\"connected\":false,\"enabled\":false,\"fresh\":false,\"data\":null}",
        "and the JSON reply says not connected");
  deliver(kPorch, "porch", REMOTE_SENSOR_TOF, 7, 20, "{\"d\":2}");
  check(countsOf(kPorch, REMOTE_SENSOR_TOF).lost == 14 && countsOf(kPorch, REMOTE_SENSOR_TOF).received == 6,
        "the outage is counted: 14 readings lost, the totals go on");
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info) && info.fresh, "and the sensor is listed again");
}

static void testParts() {
  resetAll();
  const std::string first = jsonOf(100);
  deliver(kPorch, "porch", REMOTE_SENSOR_TOF, 7, 1, first);
  // The largest reading through a relay: four parts. While they arrive,
  // readers keep the last complete reading.
  const std::string big = jsonOf(SENSOR_READING_MAX_BYTES);
  espnowsensor::PartPlan plan;
  espnowsensor::planParts(big.size(), 130, plan);
  check(plan.count == 4, "472 bytes are four routed parts");
  for (uint8_t i = 0; i < plan.count; ++i) {
    size_t offset = 0, n = 0;
    espnowsensor::partSpan(plan, big.size(), i, offset, n);
    espnowsensor::ReadingHeader h{};
    h.sensorType = REMOTE_SENSOR_TOF;
    h.bootTag = 7;
    h.counter = 2;
    h.partIndex = i;
    h.partCount = plan.count;
    std::vector<uint8_t> frame(espnowsensor::kReadingHeaderLen + n);
    espnowsensor::packReadingHeader(frame.data(), h);
    std::memcpy(frame.data() + espnowsensor::kReadingHeaderLen, big.data() + offset, n);
    check(frame.size() <= 130, "a routed part fits a routed frame");
    remoteSensorIngestPart(kPorch, "porch", frame.data(), frame.size(), false);
    if (i + 1 < plan.count) check(textOf(kPorch, REMOTE_SENSOR_TOF) == first, "the previous reading stays until the new one is whole");
  }
  check(textOf(kPorch, REMOTE_SENSOR_TOF) == big, "the whole reading is stored, byte for byte");
  RemoteSensorInfo info;
  remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info);
  check(info.jsonLength == SENSOR_READING_MAX_BYTES && info.counter == 2, "length 472, counter 2");
  check(has(json(kPorch, REMOTE_SENSOR_TOF), big), "and comes back whole in the JSON reply");
  // A middle part is lost: nothing is stored, it is counted incomplete, and
  // the next reading counts it lost.
  deliver(kPorch, "porch", REMOTE_SENSOR_TOF, 7, 3, jsonOf(400), false, 0, 130, /*losePart=*/1);
  check(textOf(kPorch, REMOTE_SENSOR_TOF) == big && incompleteOf(kPorch) == 1, "a reading with a lost part is not stored");
  deliver(kPorch, "porch", REMOTE_SENSOR_TOF, 7, 4, jsonOf(200), false, 0, 130);
  check(textOf(kPorch, REMOTE_SENSOR_TOF) == jsonOf(200) && countsOf(kPorch, REMOTE_SENSOR_TOF).lost == 1 &&
        incompleteOf(kPorch) == 1, "the next reading counts it lost, once");
  check(has(json(kPorch, REMOTE_SENSOR_TOF), "\"lost\":1,") && has(json(kPorch, REMOTE_SENSOR_TOF), "\"incomplete\":1"),
        "JSON: lost 1, incomplete 1");
  // Two senders send parts in turn: each has its own open reading.
  const std::string a = jsonOf(300), b = jsonOf(350);
  espnowsensor::PartPlan pa, pb;
  espnowsensor::planParts(a.size(), 130, pa);
  espnowsensor::planParts(b.size(), 130, pb);
  for (uint8_t i = 0; i < 3; ++i) {
    for (int who = 0; who < 2; ++who) {
      const std::string& text = who ? b : a;
      const espnowsensor::PartPlan& p = who ? pb : pa;
      size_t offset = 0, n = 0;
      espnowsensor::partSpan(p, text.size(), i, offset, n);
      espnowsensor::ReadingHeader h{};
      h.sensorType = REMOTE_SENSOR_IMU;
      h.bootTag = 9;
      h.counter = 1;
      h.partIndex = i;
      h.partCount = p.count;
      std::vector<uint8_t> frame(espnowsensor::kReadingHeaderLen + n);
      espnowsensor::packReadingHeader(frame.data(), h);
      std::memcpy(frame.data() + espnowsensor::kReadingHeaderLen, text.data() + offset, n);
      remoteSensorIngestPart(who ? kShed : kPorch, who ? "shed" : "porch", frame.data(), frame.size(), false);
    }
  }
  check(textOf(kPorch, REMOTE_SENSOR_IMU) == a && textOf(kShed, REMOTE_SENSOR_IMU) == b,
        "parts of two senders that arrive in turn give both readings");
}

static void testBondAndMeshTheSame() {
  // The bond frame and the mesh frame go through one ingest and give the same slot.
  resetAll();
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, jsonOf(300), /*fromBond=*/false);
  deliver(kShed, "porch", REMOTE_SENSOR_GPS, 7, 1, jsonOf(300), /*fromBond=*/true);
  RemoteSensorInfo mesh, bond;
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &mesh) && remoteSensorInfoFor(kShed, nullptr, REMOTE_SENSOR_GPS, &bond), "both stored");
  check(std::strcmp(mesh.deviceName, bond.deviceName) == 0 && mesh.sensorType == bond.sensorType &&
        mesh.enabled == bond.enabled && mesh.fresh == bond.fresh && mesh.jsonLength == bond.jsonLength &&
        mesh.lastUpdate == bond.lastUpdate && mesh.lastSeen == bond.lastSeen &&
        mesh.arrivalEpochMs == bond.arrivalEpochMs && mesh.arrivalClock == bond.arrivalClock &&
        mesh.counter == bond.counter, "the same slot content apart from the sender");
  check(countsOf(kPorch, REMOTE_SENSOR_GPS).received == 1 && countsOf(kShed, REMOTE_SENSOR_GPS).received == 1,
        "and the same counts");
  check(textOf(kPorch, REMOTE_SENSOR_GPS) == textOf(kShed, REMOTE_SENSOR_GPS), "the same text");
}

static void testRefusals() {
  resetAll();
  // S13: a sensor type this firmware does not know indexes nothing.
  for (uint8_t type : { (uint8_t)REMOTE_SENSOR_MAX, (uint8_t)12, (uint8_t)200, (uint8_t)255 }) {
    for (bool fromBond : { false, true }) {
      espnowsensor::ReadingHeader h{};
      h.sensorType = type;
      h.bootTag = 7;
      h.counter = 1;
      h.partCount = 1;
      std::vector<uint8_t> frame(espnowsensor::kReadingHeaderLen + 10, '{');
      espnowsensor::packReadingHeader(frame.data(), h);
      remoteSensorIngestPart(kPorch, "porch", frame.data(), frame.size(), fromBond);
    }
  }
  check(gRemoteSensorRefusals.badType == 8 && usedSlots() == 0, "types 11, 12, 200 and 255 are refused and counted; nothing is written");
  // Too short to be a reading; a header with no data; hostile part counts.
  const uint8_t shortFrame[11] = {};
  remoteSensorIngestPart(kPorch, "porch", shortFrame, sizeof(shortFrame), false);
  remoteSensorIngestPart(kPorch, "porch", nullptr, 40, false);
  check(gRemoteSensorRefusals.malformed == 2, "a payload shorter than the header is refused and counted");
  espnowsensor::ReadingHeader h{};
  h.sensorType = REMOTE_SENSOR_GPS;
  h.partCount = 1;
  uint8_t headerOnly[espnowsensor::kReadingHeaderLen];
  espnowsensor::packReadingHeader(headerOnly, h);
  remoteSensorIngestPart(kPorch, "porch", headerOnly, sizeof(headerOnly), false);
  uint8_t raw[espnowsensor::kReadingHeaderLen + 20];
  std::memset(raw, '{', sizeof(raw));
  espnowsensor::packReadingHeader(raw, h);
  raw[6] = 0x00;  // zero parts
  remoteSensorIngestPart(kPorch, "porch", raw, sizeof(raw), false);
  raw[6] = 0xF0;  // fifteen parts
  remoteSensorIngestPart(kPorch, "porch", raw, sizeof(raw), false);
  raw[6] = 0x33;  // part 3 of 3
  remoteSensorIngestPart(kPorch, "porch", raw, sizeof(raw), false);
  check(usedSlots() == 0 && gSensorPeers[0].malformed == 4, "no data, 0 parts, 15 parts and part 3 of 3 are refused and counted");
  // The text that would put keys of the sender's choosing beside the receiver's own.
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{},\"lost\":0,\"arrivalEpochMs\":1,\"x\":{}");
  check(usedSlots() == 0 && gSensorPeers[0].malformed == 5, "a text that is not one JSON object is refused and counted");
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 2, std::string("{\"a\":1,\0\"b\":2}", 14));
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 3, "[1,2,3]");
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 4, "not json at all");
  check(usedSlots() == 0 && gSensorPeers[0].malformed == 8, "a NUL, an array and plain text are refused");
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 5, "{\"ok\":true}");
  const std::string j = json(kPorch, REMOTE_SENSOR_GPS);
  check(has(j, "\"data\":{\"ok\":true}}") && j.find("\"lost\"") == j.rfind("\"lost\""), "a good reading is kept; the reply has one \"lost\"");
}

static void testStatusFrame() {
  resetAll();
  V4PayloadSensorStatus status{};
  status.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_GPS) | espnowsensor::sensorBit(REMOTE_SENSOR_IMU) |
                       espnowsensor::sensorBit(REMOTE_SENSOR_APDS) | 0xFFFFF800u;  // and bits no type has
  status.streamingMask = espnowsensor::sensorBit(REMOTE_SENSOR_GPS) | 0x80000000u;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  check(usedSlots() == 3, "a status frame lists the sensors the sender has; bits of unknown types are ignored");
  RemoteSensorInfo gps, imu;
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &gps) && remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_IMU, &imu), "both listed");
  check(gps.enabled && !imu.enabled && !gps.fresh && gps.jsonLength == 0, "streaming or not; a status frame is never data");
  check(json(kPorch, REMOTE_SENSOR_IMU) == "{\"connected\":true,\"enabled\":false,\"fresh\":false,\"link\":\"mesh\",\"via\":null,\"data\":null}",
        "JSON: present, not streaming, no data and no stamp");
  // A reading makes it fresh; a later status that says "not streaming" does
  // not take the reading away (freshness is the reading's age alone).
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{\"lat\":2}");
  status.streamingMask = 0;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &gps);
  check(!gps.enabled && gps.fresh && textOf(kPorch, REMOTE_SENSOR_GPS) == "{\"lat\":2}", "a status frame does not un-fresh a reading");
  // A sensor that is no longer present stops being marked streaming and ages out.
  status.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  gNowMs += REMOTE_SENSOR_PRESENCE_TTL_MS - 1000;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  gNowMs += 2000;
  check(!remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &gps) && remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_IMU, &imu),
        "a sensor the status no longer names ages out; one it names stays");
  // Too short.
  const uint32_t before = gRemoteSensorRefusals.malformed;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status) - 1);
  check(gRemoteSensorRefusals.malformed == before + 1, "a short status frame is refused and counted");
}

static void testStatusBeforeReadings() {
  // A status frame from a paired sender makes its entry, so the receiver
  // lists that device's sensors before any reading arrives. It never changes
  // a reading's freshness or its counts.
  resetAll();
  V4PayloadSensorStatus status{};
  status.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_TOF) | espnowsensor::sensorBit(REMOTE_SENSOR_GPS);
  status.streamingMask = 0;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  RemoteSensorInfo info;
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info) && !info.enabled && !info.fresh && info.jsonLength == 0 &&
        info.counter == 0 && info.arrivalEpochMs == 0 && std::string(info.deviceName) == "porch",
        "listed before any reading: present, not streaming, with the sender's name");
  check(json(kPorch, REMOTE_SENSOR_TOF) == "{\"connected\":true,\"enabled\":false,\"fresh\":false,\"link\":\"mesh\",\"via\":null,\"data\":null}",
        "its JSON: present, not streaming, no data");
  espnowsensor::CounterCounts c = countsOf(kPorch, REMOTE_SENSOR_TOF);
  check(c.received == 0 && c.lost == 0 && c.late == 0 && c.repeated == 0 && c.restarts == 0 && c.resumes == 0,
        "no reading is counted");
  // The stream starts: the status says so, still no reading and no freshness.
  status.streamingMask = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info) && info.enabled && !info.fresh && info.jsonLength == 0,
        "streaming, and still no reading");
  // The first reading: kept, fresh, counted as the first (nothing lost).
  deliver(kPorch, "porch", REMOTE_SENSOR_TOF, 7, 5, "{\"mm\":1}");
  c = countsOf(kPorch, REMOTE_SENSOR_TOF);
  check(c.received == 1 && c.lost == 0 && remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info) && info.fresh &&
        info.counter == 5, "the first reading after it is kept and counted as the first");
  // More status frames change no count, no counter and no freshness.
  for (int i = 0; i < 5; ++i) remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  c = countsOf(kPorch, REMOTE_SENSOR_TOF);
  check(c.received == 1 && c.lost == 0 && remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info) && info.fresh &&
        info.counter == 5 && textOf(kPorch, REMOTE_SENSOR_TOF) == "{\"mm\":1}", "status frames leave the reading as it was");
  gNowMs += REMOTE_SENSOR_TTL_MS + 1000;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info) && !info.fresh, "and never make an old reading fresh");
  // A worker still keeps nothing from a status frame.
  resetAll();
  gSettings.meshRole = 0;
  const uint32_t refusedBefore = gRemoteSensorRefusals.notReceiver;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  check(!remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_TOF, &info) && gRemoteSensorRefusals.notReceiver == refusedBefore + 1,
        "a worker refuses it and counts it");
  // Full through status frames alone: sixteen paired senders listed by their
  // status, the seventeenth refused, counted and said, as for a reading; its
  // reading is refused the same way, and nothing of the others changes.
  resetAll();
  gPaired.clear();
  gConsole.clear();
  uint8_t mac[6] = { 0x02, 0xC0, 0, 0, 0, 0 };
  for (uint8_t i = 0; i < 16; ++i) {
    mac[5] = i;
    pair(mac);
    remoteSensorIngestStatus(mac, "node", (const uint8_t*)&status, sizeof(status));
  }
  check(usedSlots() == 32 && gRemoteSensorRefusals.noRecord == 0, "sixteen senders listed by their status frames");
  mac[5] = 16;
  pair(mac);
  remoteSensorIngestStatus(mac, "late", (const uint8_t*)&status, sizeof(status));
  check(gRemoteSensorRefusals.noRecord == 1 && !remoteSensorInfoFor(mac, nullptr, REMOTE_SENSOR_TOF, &info) && usedSlots() == 32,
        "the seventeenth sender's status frame is refused and counted; the table is as it was");
  check(gConsole.size() == 1 &&
        has(gConsole[0], "Readings from late refused: this device already keeps the readings of 16 paired devices"),
        "and it is said");
  deliver(mac, "late", REMOTE_SENSOR_TOF, 1, 1, "{\"mm\":2}");
  check(gRemoteSensorRefusals.noRecord == 2 && !remoteSensorInfoFor(mac, nullptr, REMOTE_SENSOR_TOF, &info) && usedSlots() == 32 &&
        gConsole.size() == 1, "its reading is refused the same way (the line comes once a minute)");
}

static void testTableFull() {
  // S14: every sender has its own slots, one per sensor type. Sixteen senders
  // with every sensor: all 176 are kept and none is refused.
  resetAll();
  gPaired.clear();
  uint8_t mac[6] = { 0x02, 0xB0, 0, 0, 0, 0 };
  int stored = 0;
  for (uint8_t s = 0; s < 16; ++s) {
    mac[5] = s;
    pair(mac);
    for (uint8_t type = 0; type < REMOTE_SENSOR_MAX; ++type) {
      deliver(mac, "node", type, 1, 1, "{\"v\":1}");
      RemoteSensorInfo info;
      if (remoteSensorInfoFor(mac, nullptr, (RemoteSensorType)type, &info)) ++stored;
    }
  }
  check(stored == 176 && usedSlots() == 176, "sixteen senders with eleven sensors each: all 176 are kept");
  check(gRemoteSensorRefusals.noRecord == 0 && gRemoteSensorRefusals.noMemory == 0, "none is refused");
  // The table is full when sixteen paired devices send: the seventeenth is
  // refused, counted, and said once a minute. Nothing of the others changes.
  mac[5] = 16;
  pair(mac);
  gConsole.clear();
  deliver(mac, "late", REMOTE_SENSOR_GPS, 1, 1, "{\"v\":1}");
  deliver(mac, "late", REMOTE_SENSOR_GPS, 1, 2, "{\"v\":2}");
  RemoteSensorInfo info;
  check(gRemoteSensorRefusals.noRecord == 2 && !remoteSensorInfoFor(mac, nullptr, REMOTE_SENSOR_GPS, &info) && usedSlots() == 176,
        "the seventeenth sender is refused and counted; the table is as it was");
  check(gConsole.size() == 1 && has(gConsole[0], "Readings from late refused: this device already keeps the readings of 16 paired devices"),
        "and it is said, once");
  // Its status frame is refused the same way.
  V4PayloadSensorStatus status{};
  status.presentMask = 0x7FF;
  remoteSensorIngestStatus(mac, "late", (const uint8_t*)&status, sizeof(status));
  check(gRemoteSensorRefusals.noRecord == 3 && gConsole.size() == 1, "its status frame is refused and counted as well");
  // A sender that is no longer paired gives its record, and its slots, to the
  // next one: with clean counts and none of the old readings.
  uint8_t gone[6] = { 0x02, 0xB0, 0, 0, 0, 3 };
  for (size_t i = 0; i < gPaired.size(); ++i) {
    if (std::memcmp(gPaired[i].mac, gone, 6) == 0) { gPaired.erase(gPaired.begin() + (long)i); break; }
  }
  const unsigned allocs = gAllocs;
  deliver(mac, "late", REMOTE_SENSOR_GPS, 1, 9, "{\"v\":9}");
  check(remoteSensorInfoFor(mac, nullptr, REMOTE_SENSOR_GPS, &info) &&
        countsOf(mac, REMOTE_SENSOR_GPS).received == 1 && countsOf(mac, REMOTE_SENSOR_GPS).lost == 0,
        "an unpaired sender's record is reused, with clean counts");
  check(gRemoteSensorRefusals.noRecord == 3, "and nothing more was refused");
  check(gAllocs == allocs && std::string(info.deviceName) == "late", "its slots are reused, not made again");
  check(!remoteSensorInfoFor(gone, nullptr, REMOTE_SENSOR_GPS, &info) && !remoteSensorInfoFor(mac, nullptr, REMOTE_SENSOR_IMU, &info) &&
        textOf(mac, REMOTE_SENSOR_IMU).empty() && usedSlots() == 176 - REMOTE_SENSOR_MAX + 1,
        "and none of the old sender's readings are left in them");
  // Full again (sixteen paired senders): the next one is refused, and the
  // console line comes again a minute after the last, with what was held back.
  mac[5] = 17;
  pair(mac);
  deliver(mac, "later", REMOTE_SENSOR_GPS, 1, 1, "{\"v\":1}");
  check(gRemoteSensorRefusals.noRecord == 4 && gConsole.size() == 1, "refused again, not said again within the minute");
  gNowMs += 61000;
  deliver(mac, "later", REMOTE_SENSOR_GPS, 1, 2, "{\"v\":2}");
  check(gConsole.size() == 2 && has(gConsole[1], "Readings from later refused") && has(gConsole[1], "(3 more in the last minute)"),
        "again a minute later, with what was held back");
  // One sender's many sensors take nothing from another sender, and a sensor
  // that was silent for the presence time is listed again by its next reading.
  resetAll();
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{\"v\":1}");
  gNowMs += REMOTE_SENSOR_PRESENCE_TTL_MS + 1000;
  for (uint8_t type = 0; type < REMOTE_SENSOR_MAX; ++type) deliver(kShed, "shed", type, 7, 1, "{\"v\":1}");
  check(usedSlots() == REMOTE_SENSOR_MAX && !remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info), "a silent sensor is not listed");
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 2, "{\"v\":2}");
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) && info.fresh && textOf(kPorch, REMOTE_SENSOR_GPS) == "{\"v\":2}" &&
        usedSlots() == REMOTE_SENSOR_MAX + 1, "its slot was not taken meanwhile");
  // No memory for a new sender's slots: refused and counted, the record stays usable.
  resetAll();
  gAllocFails = true;
  gConsole.clear();
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{\"v\":1}");
  check(gRemoteSensorRefusals.noMemory == 1 && !remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) &&
        gConsole.size() == 1 && has(gConsole[0], "out of memory"), "no memory for a sender's slots: refused, counted, said");
  gAllocFails = false;
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 2, "{\"v\":2}");
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) && countsOf(kPorch, REMOTE_SENSOR_GPS).received == 1,
        "the next reading is kept once there is memory");
}

static void addSession(const uint8_t mac[6], bool up, bool routed = false) {
  TestSession s{};
  std::memcpy(s.mac, mac, 6);
  s.up = up;
  s.routed = routed;
  gSessions.push_back(s);
}
static espnowsensor::ReadingHeader headerOf(const SentPart& p) {
  espnowsensor::ReadingHeader h{};
  espnowsensor::parseReadingHeader(p.bytes.data(), p.bytes.size(), h);
  return h;
}

static void testSend() {
  resetAll();
  addSession(kHub, true);
  espnowsensor::DropReason why = espnowsensor::DropReason::QueueFull;
  // One frame to a direct receiver.
  const std::string small = jsonOf(150);
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_GPS, small.data(), small.size(), &why) &&
        why == espnowsensor::DropReason::None, "a reading of 150 bytes is sent");
  check(gSentParts.size() == 1 && gSentParts[0].bytes.size() == 162 && !gSentParts[0].viaBond &&
        std::memcmp(gSentParts[0].dst, kHub, 6) == 0, "in one frame of 12 + 150 bytes to the receiver");
  espnowsensor::ReadingHeader h = headerOf(gSentParts[0]);
  check(h.sensorType == REMOTE_SENSOR_GPS && h.counter == 1 && h.bootTag == 0xA2B3 && h.partIndex == 0 && h.partCount == 1 &&
        h.ageMs == 0, "the header: counter 1, the boot tag from the boot counter, one part, age 0");
  check((h.flags & espnowsensor::kReadingFlagStreamStart) != 0 && gSentParts[0].bytes[7] == 0, "the first reading says 'stream start'; reserved is 0");
  check(textOf(gSelfMac, REMOTE_SENSOR_GPS) == small, "and arrives as it was sent");
  // 190 bytes still fit one frame; 191 need two.
  gSentParts.clear();
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_GPS, jsonOf(190).data(), 190, &why) && gSentParts.size() == 1 &&
        gSentParts[0].bytes.size() == 202, "190 bytes: one full frame");
  check((headerOf(gSentParts[0]).flags & espnowsensor::kReadingFlagStreamStart) == 0 && headerOf(gSentParts[0]).counter == 2,
        "the second reading carries no stream start");
  gSentParts.clear();
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_GPS, jsonOf(191).data(), 191, &why) && gSentParts.size() == 2,
        "191 bytes: two frames");
  check(textOf(gSelfMac, REMOTE_SENSOR_GPS) == jsonOf(191), "received whole");
  // The largest reading: three frames direct, four through a relay, each within its frame.
  gSentParts.clear();
  const std::string big = jsonOf(SENSOR_READING_MAX_BYTES);
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_TOF, big.data(), big.size(), &why) && gSentParts.size() == 3,
        "472 bytes direct: three frames");
  for (const SentPart& p : gSentParts) check(p.bytes.size() <= 202, "each within a direct frame");
  check(textOf(gSelfMac, REMOTE_SENSOR_TOF) == big, "received whole");
  gSessions[0].routed = true;
  gSentParts.clear();
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_TOF, big.data(), big.size(), &why) && gSentParts.size() == 4,
        "472 bytes through a relay: four frames");
  for (const SentPart& p : gSentParts) check(p.bytes.size() <= 130, "each within a routed frame");
  check(headerOf(gSentParts[3]).partIndex == 3 && headerOf(gSentParts[3]).partCount == 4 &&
        headerOf(gSentParts[0]).counter == headerOf(gSentParts[3]).counter, "all parts carry the same counter");
  check(textOf(gSelfMac, REMOTE_SENSOR_TOF) == big && countsOf(gSelfMac, REMOTE_SENSOR_TOF).lost == 0, "received whole, nothing lost");
  gSentParts.clear();
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_IMU, jsonOf(130).data(), 130, &why) && gSentParts.size() == 2,
        "130 bytes through a relay: two frames (one direct)");

  // Too long: not sent, and the receiver sees the gap.
  gSentParts.clear();
  const std::string tooLong = jsonOf(SENSOR_READING_MAX_BYTES) + " ";
  check(!sensorSendReadingTo(kHub, false, REMOTE_SENSOR_TOF, tooLong.data(), tooLong.size(), &why) &&
        why == espnowsensor::DropReason::TooLong && gSentParts.empty(), "473 bytes: not sent, reason too_long");
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_TOF, jsonOf(50).data(), 50, &why), "the next one is sent");
  check(countsOf(gSelfMac, REMOTE_SENSOR_TOF).lost == 1, "and the receiver counts the dropped one as lost");

  // The queue refuses the second part: the reading is dropped and counted on both sides.
  gSentParts.clear();
  gPartsHandedOver = 0;
  gQueueFullAtPart = 1;
  check(!sensorSendReadingTo(kHub, false, REMOTE_SENSOR_TOF, big.data(), big.size(), &why) &&
        why == espnowsensor::DropReason::QueueFull && gSentParts.size() == 1, "queue full on the second part: stopped");
  gQueueFullAtPart = -1;
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_TOF, jsonOf(50).data(), 50, &why), "the next reading goes out");
  check(incompleteOf(gSelfMac) == 1 && countsOf(gSelfMac, REMOTE_SENSOR_TOF).lost == 2, "the receiver counts it incomplete and lost");
  // A part lost on the air.
  gPartsHandedOver = 0;
  gLosePart = 2;
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_TOF, big.data(), big.size(), &why), "the sender cannot know a part was lost");
  gLosePart = -1;
  sensorSendReadingTo(kHub, false, REMOTE_SENSOR_TOF, jsonOf(50).data(), 50, &why);
  check(incompleteOf(gSelfMac) == 2 && countsOf(gSelfMac, REMOTE_SENSOR_TOF).lost == 3, "the receiver counts it");

  // No session: nothing is sent, no counter value is taken, and the
  // handshake is started by the task step, not by the sender.
  resetAll();
  addSession(kHub, false);
  check(!sensorSendReadingTo(kHub, false, REMOTE_SENSOR_GPS, small.data(), small.size(), &why) &&
        why == espnowsensor::DropReason::NoSession && gSentParts.empty() && gHandshakes.empty(),
        "no session: not sent, and the sender starts no handshake itself");
  check(gSensorTxCounter[REMOTE_SENSOR_GPS] == 0, "no counter value is taken");
  gNowMs += 1000;
  sensorLinkTick(gNowMs);
  check(gHandshakes.size() == 1 && gHandshakes[0] == "02:A0:00:00:00:10", "the task step starts the handshake with the receiver");
  gNowMs += 1000;
  sensorLinkTick(gNowMs);
  check(gHandshakes.size() == 1, "once per reading that found no session, not on its own");
  gSessions[0].up = true;
  check(sensorSendReadingTo(kHub, false, REMOTE_SENSOR_GPS, small.data(), small.size(), &why) &&
        headerOf(gSentParts[0]).counter == 1 && (headerOf(gSentParts[0]).flags & espnowsensor::kReadingFlagStreamStart),
        "with the session up the first reading goes out as counter 1 with the stream start");
  gNowMs += 1000;
  sensorLinkTick(gNowMs);
  check(gHandshakes.size() == 1, "no handshake while the session is up");
  // A bond partner's session is the bond heartbeat's business.
  resetAll();
  addSession(kHub, false);
  check(!sensorSendReadingTo(kHub, true, REMOTE_SENSOR_GPS, small.data(), small.size(), &why) &&
        why == espnowsensor::DropReason::NoSession, "bond partner without a session: not sent");
  gNowMs += 1000;
  sensorLinkTick(gNowMs);
  check(gHandshakes.empty(), "and no handshake is started for it here");
  gSessions[0].up = true;
  check(sensorSendReadingTo(kHub, true, REMOTE_SENSOR_GPS, small.data(), small.size(), &why) && gSentParts.size() == 1 &&
        gSentParts[0].viaBond, "to the bond partner the same frame goes out as the bond opcode");

  // The target changes: the first reading to the new one, and the first back
  // to the old one, say 'stream start', so neither receiver counts the
  // other's readings as lost.
  resetAll();
  addSession(kHub, true);
  addSession(kBackup, true);
  gLoopback = false;
  for (int i = 0; i < 3; ++i) sensorSendReadingTo(kHub, false, REMOTE_SENSOR_IMU, small.data(), small.size(), &why);
  for (int i = 0; i < 3; ++i) sensorSendReadingTo(kBackup, false, REMOTE_SENSOR_IMU, small.data(), small.size(), &why);
  sensorSendReadingTo(kHub, false, REMOTE_SENSOR_IMU, small.data(), small.size(), &why);
  std::vector<bool> starts;
  std::vector<uint16_t> counters;
  for (const SentPart& p : gSentParts) {
    starts.push_back((headerOf(p).flags & espnowsensor::kReadingFlagStreamStart) != 0);
    counters.push_back(headerOf(p).counter);
  }
  check(starts == std::vector<bool>({ true, false, false, true, false, false, true }), "stream start on each change of target");
  check(counters == std::vector<uint16_t>({ 1, 2, 3, 4, 5, 6, 7 }), "one counter per sensor whoever it goes to");
  // A reading that is dropped before it goes out does not take the stream start with it.
  resetAll();
  addSession(kHub, true);
  gLoopback = false;
  gQueueFullAtPart = 0;
  check(!sensorSendReadingTo(kHub, false, REMOTE_SENSOR_IMU, small.data(), small.size(), &why), "dropped: queue full");
  gQueueFullAtPart = -1;
  sensorSendReadingTo(kHub, false, REMOTE_SENSOR_IMU, small.data(), small.size(), &why);
  check(gSentParts.size() == 1 && (headerOf(gSentParts[0]).flags & espnowsensor::kReadingFlagStreamStart) &&
        headerOf(gSentParts[0]).counter == 2, "the next reading still says 'stream start'");
}

static void testDropsAreSaid() {
  // Every reason is counted per sensor, and a console line says it once a minute.
  resetAll();
  gTarget.why = espnowsensor::DropReason::NoReceiver;
  const std::string small = jsonOf(100);
  for (int i = 0; i < 100; ++i) transmitSensorData(REMOTE_SENSOR_TOF, small.data(), small.size());
  check(gSensorTxStats[REMOTE_SENSOR_TOF].notSent[(size_t)espnowsensor::DropReason::NoReceiver] == 100 &&
        gSensorTxStats[REMOTE_SENSOR_TOF].sent == 0, "100 readings without a receiver are counted");
  check(gTargetAskedFor == REMOTE_SENSOR_TOF, "the target is asked for per sensor (a lease and a stream by hand are per sensor)");
  const unsigned refreshes = gTargetRefreshes;
  check(gConsole.empty(), "the sender itself prints nothing");
  gNowMs += 1000;
  sensorLinkTick(gNowMs);
  check(gTargetRefreshes == refreshes + 1, "the task step looks up the masters named on this device");
  check(gConsole.size() == 1 && has(gConsole[0], "tof readings not sent:") && has(gConsole[0], "(100 since the last such line)") &&
        has(gConsole[0], "no master is named on this device"), "one line says why and how many");
  for (int i = 0; i < 20; ++i) transmitSensorData(REMOTE_SENSOR_TOF, small.data(), small.size());
  gNowMs += 30000;
  sensorLinkTick(gNowMs);
  check(gConsole.size() == 1, "not again within the minute");
  gNowMs += 31000;
  sensorLinkTick(gNowMs);
  check(gConsole.size() == 2 && has(gConsole[1], "(20 since the last such line)"), "the next line a minute later carries the count since");
  gNowMs += 120000;
  sensorLinkTick(gNowMs);
  check(gConsole.size() == 2, "nothing dropped, nothing said");
  // Each reason the target rule gives is counted under its own name.
  const espnowsensor::DropReason reasons[] = { espnowsensor::DropReason::BondOffline, espnowsensor::DropReason::BroadcastOff,
                                               espnowsensor::DropReason::MeshOff, espnowsensor::DropReason::IsMaster,
                                               espnowsensor::DropReason::MasterNotPaired };
  for (espnowsensor::DropReason reason : reasons) {
    gTarget.why = reason;
    transmitSensorData(REMOTE_SENSOR_GPS, small.data(), small.size());
    check(gSensorTxStats[REMOTE_SENSOR_GPS].notSent[(size_t)reason] == 1, std::string("counted: ") + espnowsensor::dropReasonKey(reason));
  }
  // A reading that goes out is counted as sent; one without a session by its reason.
  gTarget = espnowsensor::TargetChoice{};
  std::memcpy(gTarget.mac, kHub, 6);
  addSession(kHub, true);
  transmitSensorData(REMOTE_SENSOR_GPS, small.data(), small.size());
  check(gSensorTxStats[REMOTE_SENSOR_GPS].sent == 1, "a reading that goes out is counted as sent");
  gSessions[0].up = false;
  transmitSensorData(REMOTE_SENSOR_GPS, small.data(), small.size());
  check(gSensorTxStats[REMOTE_SENSOR_GPS].notSent[(size_t)espnowsensor::DropReason::NoSession] == 1, "counted: no_session");
  // Two sensors, two lines.
  gConsole.clear();
  gNowMs += 61000;
  transmitSensorData(REMOTE_SENSOR_IMU, small.data(), small.size());
  sensorLinkTick(gNowMs);
  check(gConsole.size() == 2, "each sensor has its own line and its own minute");
}

// S21: what espnowsensorstream says after a stream was switched on.
static void testStreamReply() {
  char reply[200];
  auto say = [&](espnowsensor::DropReason why, bool viaBond, bool sessionUp, const char* name) {
    espnowsensor::TargetChoice choice{};
    choice.why = why;
    choice.viaBond = viaBond;
    std::memset(reply, 'X', sizeof(reply));
    sensorStreamOnReply(REMOTE_SENSOR_TOF, choice, sessionUp, name, reply, sizeof(reply));
    return std::string(reply);
  };
  using espnowsensor::DropReason;
  check(say(DropReason::None, false, true, "hub") == "OK: tof readings are sent to hub.", "sent: it names the receiver");
  check(say(DropReason::None, true, true, "glasses") == "OK: tof readings are sent to glasses (bond partner).", "sent to the bond partner");
  check(say(DropReason::None, false, false, "hub") ==
        "OK: tof streaming is on, but nothing is sent yet: hub has no session with this device.", "no session yet");
  check(say(DropReason::NoReceiver, false, false, "") ==
        "OK: tof streaming is on, but nothing is sent yet: no master is named on this device (espnowmasterfingerprint).",
        "no master named: it says which setting names one");
  check(say(DropReason::MasterNotPaired, false, false, "") ==
        "OK: tof streaming is on, but nothing is sent yet: the master named on this device is not paired with it.",
        "a master that is named but not paired");
  check(say(DropReason::MeshOff, false, false, "") ==
        "OK: tof streaming is on, but nothing is sent yet: mesh mode is off (espnowmode mesh).", "mesh mode off");
  check(say(DropReason::BondOffline, false, false, "") ==
        "OK: tof streaming is on, but nothing is sent yet: the bond partner is not connected.", "bond partner offline");
  check(say(DropReason::None, false, true, "") == "OK: tof readings are sent to the receiving device.", "a receiver without a name");
  // A long name does not run over the reply.
  char small[40];
  espnowsensor::TargetChoice choice{};
  sensorStreamOnReply(REMOTE_SENSOR_MICROPHONE, choice, false, "a-device-with-a-very-long-name-indeed", small, sizeof(small));
  check(std::strlen(small) == sizeof(small) - 1, "the reply is cut at its buffer");
}

// What the review of the A-F package found (2026-11-18).
static void testReviewFindings() {
  // A copy into a buffer with no room for the terminator copies nothing.
  resetAll();
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{\"lat\":1.5}");  // 11 bytes
  std::vector<char> exact(11, 'X');
  check(remoteSensorCopyText(kPorch, nullptr, REMOTE_SENSOR_GPS, exact.data(), exact.size()) == 0 && exact[0] == '\0',
        "a buffer of exactly the text's length gets nothing (it has no room for the terminator)");
  std::vector<char> room(12, 'X');
  check(remoteSensorCopyText(kPorch, nullptr, REMOTE_SENSOR_GPS, room.data(), room.size()) == 11 && room[11] == '\0',
        "one byte more: the whole text, terminated");
  // A status frame: a sensor it no longer names stops streaming at once; a
  // status frame never makes a reading fresh again.
  V4PayloadSensorStatus status{};
  status.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_GPS) | espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  status.streamingMask = status.presentMask;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  RemoteSensorInfo info;
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_IMU, &info) && info.enabled, "the status frame says imu streams");
  status.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_GPS);
  status.streamingMask = status.presentMask;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_IMU, &info) && !info.enabled, "a sensor it no longer names is no longer streaming");
  gNowMs += REMOTE_SENSOR_TTL_MS - 5000;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  gNowMs += 10000;
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) && info.enabled && !info.fresh,
        "a status frame keeps the sensor listed but does not make its old reading fresh");
  // The reply names restarts and resumes each by its own count.
  resetAll();
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 7, 1, "{\"a\":1}");
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 8, 1, "{\"a\":2}");   // a restart
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 9, 1, "{\"a\":3}");   // another
  deliver(kPorch, "porch", REMOTE_SENSOR_IMU, 9, 50, "{\"a\":4}", false, espnowsensor::kReadingFlagStreamStart);  // a resume
  check(has(json(kPorch, REMOTE_SENSOR_IMU), "\"restarts\":2,\"resumes\":1,"), "JSON: restarts 2, resumes 1");
  // 49.7 days: the uptime differences come round again. A sensor last heard
  // from exactly that long ago is not listed again, and its old reading is
  // not fresh again (the age step marked both while the differences were true).
  resetAll();
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{\"lat\":1}");
  gNowMs += REMOTE_SENSOR_PRESENCE_TTL_MS + 20000;
  sensorLinkTick(gNowMs);
  gNowMs += (uint32_t)(0x100000000ull - REMOTE_SENSOR_PRESENCE_TTL_MS - 20000 + 5000);  // 2^32 + 5 s after the reading
  check(!remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) && usedSlots() == 0,
        "49.7 days after its last frame a sensor is not listed again");
  check(textOf(kPorch, REMOTE_SENSOR_GPS) == "{\"lat\":1}", "(its last text is still kept)");
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 2, "{\"lat\":2}");
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) && info.fresh, "its next reading lists it again, fresh");
  // ... and a reading that aged while the sensor was still listed (status frames).
  resetAll();
  deliver(kPorch, "porch", REMOTE_SENSOR_GPS, 7, 1, "{\"lat\":1}");
  status = V4PayloadSensorStatus{};
  status.presentMask = status.streamingMask = espnowsensor::sensorBit(REMOTE_SENSOR_GPS);
  for (int i = 0; i < 4; ++i) {
    gNowMs += 15000;
    remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
    sensorLinkTick(gNowMs);
  }
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) && !info.fresh, "a minute without a reading: listed, not fresh");
  const uint32_t wrapTo = (uint32_t)(0x100000000ull - 60000 + 1000);  // 2^32 + 1 s after the reading
  gNowMs += wrapTo - 5000;
  remoteSensorIngestStatus(kPorch, "porch", (const uint8_t*)&status, sizeof(status));
  gNowMs += 5000;
  check(remoteSensorInfoFor(kPorch, nullptr, REMOTE_SENSOR_GPS, &info) && !info.fresh,
        "49.7 days after the reading it is not fresh again");
  check(json(kPorch, REMOTE_SENSOR_GPS).find("\"data\":null}") != std::string::npos, "and its old text is not sent as current");
}


// ---- Forwarding for a bond partner -------------------------------------------------------
static const uint8_t kGlasses[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x20 };  // a bond master (the via)
static const uint8_t kPartner[6] = { 0x02, 0xA0, 0x00, 0x00, 0x00, 0x30 };  // its bond partner, not paired here

static void linkUp(uint8_t gen) {
  gLink = espnowfwd::ForwardLink{};
  gLink.inUse = true;
  std::memcpy(gLink.via, kGlasses, 6);
  espnowfwd::LinkFrame f{};
  f.op = espnowfwd::kOpAnnounce;
  f.linkGen = gen;
  f.viaBootTag = 7;
  f.streams = espnowfwd::kStreamsAll;
  f.sensorMask = espnowfwd::kSensorMaskDefault;
  std::memcpy(f.partnerMac, kPartner, 6);
  espnowfwd::linkApplyAnnounce(gLink, f, "porch", false, 0);
}

// One reading forwarded by kGlasses for kPartner: X's sensor, boot tag and
// counter, bit 2 and the link number in byte 7.
static void deliverForwarded(uint8_t sensor, uint16_t counter, const std::string& text, uint8_t fwdLink,
                             uint8_t flags = espnowsensor::kReadingFlagForwarded, uint16_t cap = 130,
                             bool fromBond = false, const uint8_t* src = kGlasses) {
  espnowsensor::PartPlan plan;
  if (!espnowsensor::planParts(text.size(), cap, plan)) std::exit(2);
  for (uint8_t i = 0; i < plan.count; ++i) {
    size_t offset = 0, n = 0;
    espnowsensor::partSpan(plan, text.size(), i, offset, n);
    espnowsensor::ReadingHeader h{};
    h.sensorType = sensor;
    h.flags = flags;
    h.bootTag = 0xAAAA;
    h.counter = counter;
    h.partIndex = i;
    h.partCount = plan.count;
    h.fwdLink = fwdLink;
    std::vector<uint8_t> frame(espnowsensor::kReadingHeaderLen + n);
    espnowsensor::packReadingHeader(frame.data(), h);
    std::memcpy(frame.data() + espnowsensor::kReadingHeaderLen, text.data() + offset, n);
    remoteSensorIngestPart(src, "glasses", frame.data(), frame.size(), fromBond);
  }
}

static espnowsensor::CounterCounts countsVia(const uint8_t mac[6], const uint8_t* via, int sensor) {
  static const uint8_t none[6] = {};
  for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; ++i) {
    if (gSensorPeers[i].inUse && std::memcmp(gSensorPeers[i].mac, mac, 6) == 0 &&
        std::memcmp(gSensorPeers[i].via, via ? via : none, 6) == 0) {
      return gSensorPeers[i].counts[sensor];
    }
  }
  return espnowsensor::CounterCounts{};
}

static void testForwarded() {
  resetAll();
  pair(kGlasses);
  gLink = espnowfwd::ForwardLink{};
  gResumeTaken = 0;
  gSettings.meshRole = 1;
  // No link of glasses: refused, nothing filed under glasses or the partner.
  deliverForwarded(REMOTE_SENSOR_GPS, 1, "{\"v\":1}", 3);
  RemoteSensorInfo info;
  check(gRemoteSensorRefusals.forwardRefused == 1 && usedSlots() == 0, "forwarded data without a link: refused, nothing kept");
  // With the link: kept under (partner via glasses), named as the link shows.
  linkUp(3);
  deliverForwarded(REMOTE_SENSOR_GPS, 1, "{\"v\":1}", 3);
  check(remoteSensorInfoFor(kPartner, kGlasses, REMOTE_SENSOR_GPS, &info) && info.fresh &&
        info.link == espnowfwd::kLinkForwarded && std::memcmp(info.viaMac, kGlasses, 6) == 0 &&
        std::string(info.deviceName) == "porch" && remoteSensorInfoVia(info) != nullptr,
        "a forwarded reading is kept under (partner, via glasses) with the name the link shows");
  check(!remoteSensorInfoFor(kPartner, nullptr, REMOTE_SENSOR_GPS, &info) &&
        !remoteSensorInfoFor(kGlasses, nullptr, REMOTE_SENSOR_GPS, &info),
        "never under the partner's own record, never under glasses' own");
  std::vector<char> out(SENSOR_READING_MAX_BYTES + 1);
  check(remoteSensorCopyText(kPartner, kGlasses, REMOTE_SENSOR_GPS, out.data(), out.size()) == 7 &&
        remoteSensorCopyText(kPartner, nullptr, REMOTE_SENSOR_GPS, out.data(), out.size()) == 0,
        "the text is read with the via; without it there is none");
  const std::string fj = getRemoteSensorDataJson(kPartner, kGlasses, REMOTE_SENSOR_GPS).text();
  check(has(fj, "\"link\":\"forwarded\",\"via\":{\"mac\":\"02:A0:00:00:00:20\",\"name\":\"glasses\"}") &&
        has(fj, "\"data\":{\"v\":1}"), "GET /api/sensors/remote with via: the forwarded record, marked so");
  check(has(getRemoteSensorDataJson(kPartner, nullptr, REMOTE_SENSOR_GPS).text(), "\"connected\":false"),
        "without via: the partner's own record, which this device does not have");
  // The partner's own counters: X's numbers, end to end.
  deliverForwarded(REMOTE_SENSOR_GPS, 2, "{\"v\":2}", 3);
  deliverForwarded(REMOTE_SENSOR_GPS, 5, "{\"v\":5}", 3);  // 3 and 4 lost on one of the hops
  espnowsensor::CounterCounts c = countsVia(kPartner, kGlasses, REMOTE_SENSOR_GPS);
  check(c.received == 3 && c.lost == 2, "the partner's counter: lost readings show on the mesh master");
  // The same partner also sends directly: a separate record.
  pair(kPartner);
  deliver(kPartner, "porch-direct", REMOTE_SENSOR_GPS, 0xBBBB, 1, "{\"d\":1}");
  check(remoteSensorInfoFor(kPartner, nullptr, REMOTE_SENSOR_GPS, &info) && info.link == espnowfwd::kLinkMesh &&
        textOf(kPartner, REMOTE_SENSOR_GPS) == "{\"d\":1}" &&
        remoteSensorCopyText(kPartner, kGlasses, REMOTE_SENSOR_GPS, out.data(), out.size()) == 7,
        "a direct reading of the partner and the forwarded one are two records");
  // Another link number (an old partner's frame), a stream not announced
  // (input is left out by default), a bit without the byte: refused.
  const uint32_t refusedBefore = gRemoteSensorRefusals.forwardRefused;
  deliverForwarded(REMOTE_SENSOR_GPS, 6, "{\"v\":6}", 2);
  deliverForwarded(REMOTE_SENSOR_INPUT, 1, "{\"b\":1}", 3);
  check(gRemoteSensorRefusals.forwardRefused == refusedBefore + 2 &&
        countsVia(kPartner, kGlasses, REMOTE_SENSOR_GPS).received == 3 &&
        !remoteSensorInfoFor(kPartner, kGlasses, REMOTE_SENSOR_INPUT, &info),
        "an old link number and a stream that was not announced are refused");
  const uint32_t malformedBefore = gRemoteSensorRefusals.malformed;
  deliverForwarded(REMOTE_SENSOR_GPS, 7, "{\"v\":7}", 0);  // bit, no byte
  deliverForwarded(REMOTE_SENSOR_GPS, 7, "{\"v\":7}", 3, 0);  // byte, no bit
  check(gRemoteSensorRefusals.malformed == malformedBefore + 2, "a bit and a byte that disagree are malformed");

  // A new link (forwarding was off for a while): the first reading of each
  // stream is a resume, taken by the reading the counter rule judged: the
  // last part of a reading in parts, not the first.
  linkUp(4);
  gResumeTaken = 0;
  const std::string big = jsonOf(400);  // 4 routed parts
  espnowsensor::PartPlan plan;
  espnowsensor::planParts(big.size(), 130, plan);
  check(plan.count == 4, "a 400-byte reading is four routed parts");
  for (uint8_t i = 0; i < 3; ++i) {
    size_t offset = 0, n = 0;
    espnowsensor::partSpan(plan, big.size(), i, offset, n);
    espnowsensor::ReadingHeader h{};
    h.sensorType = REMOTE_SENSOR_GPS;
    h.flags = espnowsensor::kReadingFlagForwarded;
    h.bootTag = 0xAAAA;
    h.counter = 40000;
    h.partIndex = i;
    h.partCount = plan.count;
    h.fwdLink = 4;
    std::vector<uint8_t> frame(espnowsensor::kReadingHeaderLen + n);
    espnowsensor::packReadingHeader(frame.data(), h);
    std::memcpy(frame.data() + espnowsensor::kReadingHeaderLen, big.data() + offset, n);
    remoteSensorIngestPart(kGlasses, "glasses", frame.data(), frame.size(), false);
  }
  check(gResumeTaken == 0 && (gLink.resumeMask & (1u << REMOTE_SENSOR_GPS)), "the resume waits for the reading's last part");
  {
    size_t offset = 0, n = 0;
    espnowsensor::partSpan(plan, big.size(), 3, offset, n);
    espnowsensor::ReadingHeader h{};
    h.sensorType = REMOTE_SENSOR_GPS;
    h.flags = espnowsensor::kReadingFlagForwarded;
    h.bootTag = 0xAAAA;
    h.counter = 40000;
    h.partIndex = 3;
    h.partCount = plan.count;
    h.fwdLink = 4;
    std::vector<uint8_t> frame(espnowsensor::kReadingHeaderLen + n);
    espnowsensor::packReadingHeader(frame.data(), h);
    std::memcpy(frame.data() + espnowsensor::kReadingHeaderLen, big.data() + offset, n);
    remoteSensorIngestPart(kGlasses, "glasses", frame.data(), frame.size(), false);
  }
  c = countsVia(kPartner, kGlasses, REMOTE_SENSOR_GPS);
  check(gResumeTaken == 1 && !(gLink.resumeMask & (1u << REMOTE_SENSOR_GPS)) && c.resumes == 1 && c.lost == 2 &&
        c.received == 4, "the pause is a resume, not 39995 lost readings; the resume is taken once");
  deliverForwarded(REMOTE_SENSOR_GPS, 40002, "{\"v\":9}", 4);
  c = countsVia(kPartner, kGlasses, REMOTE_SENSOR_GPS);
  check(c.lost == 3 && c.resumes == 1, "after the resume the counter rule counts loss again");
  // An ended link frees its records for a seventeenth sender.
  gLink.state = espnowfwd::LState::Ended;
  deliverForwarded(REMOTE_SENSOR_GPS, 40003, "{\"v\":10}", 4);
  check(remoteSensorInfoFor(kPartner, kGlasses, REMOTE_SENSOR_GPS, &info), "the record stays until a sender needs it");
  for (uint8_t k = 0; k < REMOTE_SENSOR_PEERS_MAX; ++k) {
    uint8_t mac[6] = { 0x02, 0xD0, 0, 0, 0, k };
    pair(mac);
    deliver(mac, "node", REMOTE_SENSOR_TOF, 1, 1, "{\"t\":1}");
  }
  check(!remoteSensorInfoFor(kPartner, kGlasses, REMOTE_SENSOR_GPS, &info),
        "a forwarded record whose link ended was given to another sender");

  // The bond master's side: a reading of its partner it kept is handed to its
  // forwarder with the header and text as they came; a repeat is not.
  resetAll();
  gLink = espnowfwd::ForwardLink{};
  gForwarded.clear();
  gNotKept.clear();
  gSettings.meshRole = 0;  // a mesh worker that is a bond master
  deliver(kShed, "shed", REMOTE_SENSOR_IMU, 0x1111, 9, jsonOf(300), true, espnowsensor::kReadingFlagStreamStart);
  check(gForwarded.size() == 1 && std::memcmp(gForwarded[0].src, kShed, 6) == 0 &&
        gForwarded[0].h.counter == 9 && gForwarded[0].h.bootTag == 0x1111 &&
        gForwarded[0].h.flags == espnowsensor::kReadingFlagStreamStart && gForwarded[0].text == jsonOf(300),
        "the bond master hands its partner's reading to its forwarder, whole, with its own header");
  deliver(kShed, "shed", REMOTE_SENSOR_IMU, 0x1111, 9, jsonOf(300), true);
  check(gForwarded.size() == 1, "a repeat is not forwarded");
  deliver(kShed, "shed", REMOTE_SENSOR_IMU, 0x1111, 10, "{\"i\":1}");  // over the mesh: not a bond frame
  check(gForwarded.size() == 1, "only frames over the bond link are forwarded");
  // A forwarded mark on the bond link is malformed: the partner never forwards.
  const uint32_t mal = gRemoteSensorRefusals.malformed;
  deliverForwarded(REMOTE_SENSOR_IMU, 11, "{\"i\":2}", 3, espnowsensor::kReadingFlagForwarded, 202, true, kShed);
  check(gRemoteSensorRefusals.malformed == mal + 1 && gForwarded.size() == 1, "bit 2 on 179 is malformed");
  // What the bond master could not keep is counted for its forwarder.
  resetAll();
  gSettings.meshRole = 0;
  gNotKept.clear();
  for (int i = 0; gSensorPeers && i < REMOTE_SENSOR_PEERS_MAX; ++i) std::free(gSensorPeers[i].slots);
  std::free(gSensorPeers);
  gSensorPeers = nullptr;
  gAllocFails = true;
  deliver(kShed, "shed", REMOTE_SENSOR_IMU, 0x1111, 12, "{\"i\":3}", true);
  gAllocFails = false;
  check(gNotKept.size() == 1 && gNotKept[0] == REMOTE_SENSOR_IMU, "a partner's reading the bond master could not keep is counted");
  // No record free (paired senders hold every one): counted as well.
  resetAll();
  gSettings.meshRole = 1;
  for (uint8_t k = 0; k < REMOTE_SENSOR_PEERS_MAX; ++k) {
    uint8_t mac[6] = { 0x02, 0xD1, 0, 0, 0, k };
    pair(mac);
    deliver(mac, "node", REMOTE_SENSOR_TOF, 1, 1, "{\"t\":1}");
  }
  gNotKept.clear();
  gForwarded.clear();
  deliver(kShed, "shed", REMOTE_SENSOR_IMU, 0x1111, 13, "{\"i\":4}", true);
  check(gNotKept.size() == 1 && gNotKept[0] == REMOTE_SENSOR_IMU && gForwarded.empty(),
        "a partner's reading with no record left is counted not kept, and not handed on");
  gConsole.clear();
}


// The partner's sensor status: on the mesh master, forwarded by glasses (the
// link byte alone) it lists the partner's sensors under (partner via glasses)
// before any reading; on the bond master, over the bond link, it is kept under
// the partner whatever the mesh role and handed to the forwarder.
static void testForwardedStatus() {
  resetAll();
  pair(kGlasses);
  gSettings.meshRole = 1;
  gForwardedStatus.clear();
  linkUp(3);
  espnowfwd::StatusFrame f{};
  f.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_TOF) | espnowsensor::sensorBit(REMOTE_SENSOR_GPS);
  f.streamingMask = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  f.bootTag = 0xAAAA;
  f.fwdLink = 3;
  uint8_t frame[espnowfwd::kStatusFrameLen];
  espnowfwd::packStatusFrame(frame, f);
  remoteSensorIngestStatus(kGlasses, "glasses", frame, sizeof(frame));
  RemoteSensorInfo info;
  check(remoteSensorInfoFor(kPartner, kGlasses, REMOTE_SENSOR_TOF, &info) && info.enabled && !info.fresh &&
        info.jsonLength == 0 && info.link == espnowfwd::kLinkForwarded && std::string(info.deviceName) == "porch" &&
        remoteSensorInfoFor(kPartner, kGlasses, REMOTE_SENSOR_GPS, &info) && !info.enabled,
        "a forwarded status lists the partner's sensors via glasses before any reading");
  check(!remoteSensorInfoFor(kGlasses, nullptr, REMOTE_SENSOR_TOF, &info) &&
        !remoteSensorInfoFor(kPartner, nullptr, REMOTE_SENSOR_TOF, &info), "never under glasses' own record or the partner's own");
  check(gForwardedStatus.empty(), "the mesh master forwards nothing");
  // An old link number: refused, nothing kept.
  const uint32_t refusedBefore = gRemoteSensorRefusals.forwardRefused;
  f.fwdLink = 2;
  espnowfwd::packStatusFrame(frame, f);
  remoteSensorIngestStatus(kGlasses, "glasses", frame, sizeof(frame));
  check(gRemoteSensorRefusals.forwardRefused == refusedBefore + 1, "another link number: refused");
  // glasses' own status (byte 0) stays its own.
  f.fwdLink = 0;
  f.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  espnowfwd::packStatusFrame(frame, f);
  remoteSensorIngestStatus(kGlasses, "glasses", frame, sizeof(frame));
  check(remoteSensorInfoFor(kGlasses, nullptr, REMOTE_SENSOR_IMU, &info) &&
        !remoteSensorInfoFor(kPartner, kGlasses, REMOTE_SENSOR_IMU, &info), "glasses' own status is its own");

  // The bond master: over the bond link, whatever its mesh role.
  resetAll();
  gSettings.meshRole = 0;
  pair(kShed);
  gForwardedStatus.clear();
  f.fwdLink = 0;
  f.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  espnowfwd::packStatusFrame(frame, f);
  remoteSensorIngestBondStatus(kShed, "shed", frame, sizeof(frame));
  check(remoteSensorInfoFor(kShed, nullptr, REMOTE_SENSOR_TOF, &info) && info.link == espnowfwd::kLinkBond,
        "the bond partner's status is kept by a bond master that is a mesh worker");
  check(gForwardedStatus.size() == 1 && std::memcmp(gForwardedStatus[0].src, kShed, 6) == 0 &&
        gForwardedStatus[0].f.presentMask == f.presentMask && gForwardedStatus[0].f.bootTag == 0xAAAA,
        "and handed to the forwarder");
  // The forwarder's copy does not depend on this device's own table: with
  // no memory for the partner's slots (or a full table) the status is still
  // handed on, so a forwarded request is judged by the partner's real list
  // and not refused as "the partner has not said yet" forever.
  resetAll();
  gSettings.meshRole = 0;
  pair(kShed);
  gForwardedStatus.clear();
  f.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  espnowfwd::packStatusFrame(frame, f);
  gAllocFails = true;
  const uint32_t noMemoryBefore = gRemoteSensorRefusals.noMemory;
  remoteSensorIngestBondStatus(kShed, "shed", frame, sizeof(frame));
  gAllocFails = false;
  check(gRemoteSensorRefusals.noMemory == noMemoryBefore + 1 && !remoteSensorInfoFor(kShed, nullptr, REMOTE_SENSOR_IMU, &info) &&
            gForwardedStatus.size() == 1 && gForwardedStatus[0].f.presentMask == f.presentMask,
        "no memory for the partner's record here: refused for the table, still handed to the forwarder");
  // Back to the bond master's own frame for the cases below.
  gForwardedStatus.clear();
  f.presentMask = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  espnowfwd::packStatusFrame(frame, f);
  remoteSensorIngestBondStatus(kShed, "shed", frame, sizeof(frame));
  check(gForwardedStatus.size() == 1, "kept and handed on once memory is there");
  // A link byte over the bond link is malformed; a short frame too.
  const uint32_t malformedBefore = gRemoteSensorRefusals.malformed;
  f.fwdLink = 1;
  espnowfwd::packStatusFrame(frame, f);
  remoteSensorIngestBondStatus(kShed, "shed", frame, sizeof(frame));
  remoteSensorIngestBondStatus(kShed, "shed", frame, sizeof(frame) - 1);
  check(gRemoteSensorRefusals.malformed == malformedBefore + 2 && gForwardedStatus.size() == 1,
        "a link byte on the bond link, or a short frame: malformed, not forwarded");
  // The same frame from the mesh (not the bond link) on a worker: refused by the role rule.
  f.fwdLink = 0;
  espnowfwd::packStatusFrame(frame, f);
  const uint32_t notReceiverBefore = gRemoteSensorRefusals.notReceiver;
  remoteSensorIngestStatus(kShed, "shed", frame, sizeof(frame));
  check(gRemoteSensorRefusals.notReceiver == notReceiverBefore + 1 && gForwardedStatus.size() == 1,
        "a mesh status on a worker: refused, nothing forwarded");
}

int main() {
  testBeforeTheTable();
  testRoles();
  testArrivalStamp();
  testCounterAndStore();
  testParts();
  testBondAndMeshTheSame();
  testRefusals();
  testStatusFrame();
  testStatusBeforeReadings();
  testTableFull();
  testSend();
  testDropsAreSaid();
  testStreamReply();
  testReviewFindings();
  testForwarded();
  testForwardedStatus();
  check(gLockViolations == 0, "nothing that may block, print or allocate ran under a lock");
  check(gLockDepth == 0, "every lock was released");
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d sensor reading path checks failed\n", failures, checks);
    return 1;
  }
  std::printf("ESP-NOW sensor reading path: %d checks passed\n", checks);
  return 0;
}
