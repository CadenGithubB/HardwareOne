// Production G2_Ring.cpp RX/packetAck path, the G2_Health.cpp pending-session
// model and the System_R1_Protocol.cpp capability/packetAck factories are
// inserted by test_ring_daily_ack.py. Everything else is a boundary double.
#include "G2_Health.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define EXT_RAM_BSS_ATTR
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define DEBUG_RING_PROTOCOLF(...) ((void)0)
#define DEBUG_RING_DUMPF(...) ((void)0)
#define DEBUG_RING_HEALTHF(...) ((void)0)
enum { LOG_LEVEL_DEBUG = 4 };
enum : uint32_t { DEBUG_RING = 1u, DEBUG_RING_DUMP = 2u, DEBUG_RING_PROTOCOL = 4u };
static int getLogLevel() { return 0; }
static bool isDebugFlagSet(uint32_t) { return false; }

static unsigned checks = 0;
static std::vector<std::string> failures;
static void expect(bool ok, const char* what) {
  ++checks;
  if (!ok) failures.push_back(what);
}

// INSERT_TYPES

// ---- ring transport boundary -------------------------------------------------
static int sTransportMux = 0;
static uint32_t sLinkGeneration = 7;
static uint32_t sTelemetryGeneration = 7;
static bool sLinkOnline = true;
static R1ProtocolProfile sR1Profile = R1_PROFILE_FW_2_2_9_0003;
static struct { uint32_t packetsReceived; } gRing{};
static uint32_t fakeMillis = 1000;
static uint32_t millis() { return fakeMillis; }
static volatile uint32_t sRingTsSeen = 0;
static volatile uint32_t sRingTsSeenRxMs = 0;
static R1TelemetryCache gR1Cache;
static RingPacketAckPending sPacketAckQueue[RING_PACKET_ACK_DEPTH];
static uint8_t sPacketAckCount = 0;
static RingActivePacketAck sActivePacketAck;
static RingRxFingerprint sRxFingerprints[RING_RX_DUPLICATE_DEPTH];
static uint8_t sRxFingerprintCursor = 0;
static R1ActivityDailyResult sRingActivityScratch;
static RingReassembly sRingReasm;
static uint8_t sRingReasmBuf[R1_ACTIVITY_REASSEMBLED_MODEL_MAX];

class RingTelemetryGuard {
 public:
  bool locked() const { return true; }
};
static void ringTransitionTelemetrySourceLocked(RingTelemetrySource) {}
static bool ringHealthTimestampPlausible(uint32_t timestamp) { return timestamp != 0; }
static void ringNoteWear(uint8_t, uint32_t) {}
static bool ringSnapshotLink(uint32_t& generation) {
  generation = sLinkGeneration;
  return sLinkOnline;
}

struct HarnessSetupOwner { bool active; bool written; uint32_t generation; R1Frame frame; };
struct HarnessIntentHandle { uint32_t generation; };
struct HarnessIntent { HarnessIntentHandle handle; };
struct HarnessActiveTransaction { bool valid; bool written; HarnessIntent intent; R1Frame frame; };
static HarnessSetupOwner sSetupOwner{};
static HarnessActiveTransaction sActiveTransaction{};
static void ringFinishSetup(bool, uint8_t) {}
static void ringCompleteActive(G2RingTransactionState, uint8_t, uint8_t) {}
static bool ringRememberRejectedSleepCandidate(HarnessActiveTransaction&, uint32_t,
                                               const R1Decoded&) { return false; }
static bool ringReasmFrameContinues(const uint8_t*, size_t, uint32_t) { return false; }
static bool ringReasmConsume(const uint8_t*, size_t, uint32_t) { return false; }
static void ringHandleSetupRx(const R1Decoded&) {}
static bool ringApplyControlObservation(const R1Decoded&, RingControlTarget&,
                                        G2RingObservedState&) { return false; }
static void ringHandleActiveRx(const R1Decoded&, RingControlTarget, G2RingObservedState) {}
static unsigned reasmCompletions = 0;
static void ringCompleteReassembledDaily(uint8_t, uint8_t, uint8_t) { ++reasmCompletions; }

// ---- Health live-series boundary --------------------------------------------
static unsigned trendCalls = 0;
void g2HealthNoteSample(G2HealthMetric, int16_t, uint32_t) {}
void g2HealthApplyTrendDaily(G2HealthMetric, const uint8_t*, size_t, uint32_t, uint32_t) {
  ++trendCalls;
}
void g2HealthApplyDailyBackfill(G2HealthMetric, const uint8_t*, size_t, uint32_t, uint32_t) {}
class HistoryLock {
 public:
  explicit HistoryLock(bool) {}
  bool locked() const { return true; }
};
static void ensureHistoryInitializedLocked() {}

// INSERT_PROTOCOL

// ---- protocol decoder/parser doubles -----------------------------------------
size_t r1AnnotatePayload(R1ProtocolProfile, const R1Decoded&, char*, size_t) { return 0; }
uint32_t r1Crc32(const uint8_t*, size_t) { return 0; }
static R1Decoded gWire;
bool r1Decode(const uint8_t*, size_t, R1Decoded& out) {
  out = gWire;
  return true;
}
static R1CommonDailyResult gCommonPage;
static R1HrvDailyResult gHrvPage;
static R1ActivityDailyResult gActivityPage;
static uint16_t gReasmSerial = 0;
R1ParseError r1ParseCommonDaily(R1ProtocolProfile profile, const R1Decoded& d,
                                R1CommonDailyResult& out) {
  if ((d.cmd != R1_CMD_HEARTRATE && d.cmd != R1_CMD_SPO2) ||
      !r1ProfileSupportsSingleFrameDaily(profile, d.cmd)) return R1_PARSE_WRONG_PROFILE;
  out = gCommonPage;
  out.profile = profile;
  out.metric = d.cmd == R1_CMD_SPO2 ? R1_DAILY_METRIC_SPO2 : R1_DAILY_METRIC_HEART_RATE;
  out.sourceSerial = d.serial;
  out.sourceCrc32 = d.crc32Received;
  return R1_PARSE_OK;
}
R1ParseError r1ParseHrvDaily(R1ProtocolProfile profile, const R1Decoded& d,
                             R1HrvDailyResult& out) {
  if (d.cmd != R1_CMD_HRV || !r1ProfileSupportsSingleFrameDaily(profile, d.cmd))
    return R1_PARSE_WRONG_PROFILE;
  out = gHrvPage;
  out.profile = profile;
  out.metric = R1_DAILY_METRIC_HRV;
  out.sourceSerial = d.serial;
  out.sourceCrc32 = d.crc32Received;
  return R1_PARSE_OK;
}
R1ParseError r1ParseActivityDaily(R1ProtocolProfile profile, const R1Decoded& d,
                                  R1ActivityDailyResult& out) {
  if (d.cmd != R1_CMD_ACTIVITY || !r1ProfileSupportsSingleFrameDaily(profile, d.cmd))
    return R1_PARSE_WRONG_PROFILE;
  out = gActivityPage;
  out.profile = profile;
  out.metric = R1_DAILY_METRIC_ACTIVITY;
  out.sourceSerial = d.serial;
  out.sourceCrc32 = d.crc32Received;
  return R1_PARSE_OK;
}
R1ParseError r1ParseReassembledActivityDaily(R1ProtocolProfile profile, const uint8_t*,
                                             size_t, uint32_t crc32Whole,
                                             R1ActivityDailyResult& out) {
  if (!r1ProfileSupportsActivityReassembly(profile)) return R1_PARSE_WRONG_PROFILE;
  out = gActivityPage;
  out.profile = profile;
  out.metric = R1_DAILY_METRIC_ACTIVITY;
  out.sourceSerial = gReasmSerial;
  out.sourceCrc32 = crc32Whole;
  return R1_PARSE_OK;
}

// INSERT_PRODUCTION

// ---- test driver --------------------------------------------------------------
static constexpr uint32_t kDay = 1760313600u;  // a UTC midnight
static constexpr const char* kPeer = "02:00:00:00:00:01";  // synthetic, locally administered

static void resetLink(R1ProtocolProfile profile) {
  sR1Profile = profile;
  sLinkGeneration = sTelemetryGeneration = 7;
  sLinkOnline = true;
  sPacketAckCount = 0;
  sActivePacketAck = RingActivePacketAck{};
  std::memset(sRxFingerprints, 0, sizeof(sRxFingerprints));
  sRxFingerprintCursor = 0;
  gR1Cache = R1TelemetryCache{};
  sRingTsSeen = 0;
  sRingReasm = RingReassembly{};
}

// An idle model whose previous result is committed.
static void resetHistory() {
  std::memset(&sPendingHistory, 0, sizeof(sPendingHistory));
  sHistoryDirty.store(false);
  sTypedHistoryFetchActive.store(false);
  sDropCurrentHistoryFetch = false;
  std::memset(&sHistoryPeerCustody, 0, sizeof(sHistoryPeerCustody));
  std::strcpy(sHistoryPeerCustody.current, kPeer);
}

static R1Decoded dailyFrame(uint8_t cmd, uint16_t serial) {
  R1Decoded d{};
  d.crc32Valid = true;
  d.transferType = 0;
  d.crc32Received = 0x5A000000u | serial;
  d.version = 0x64;
  d.module = R1_MODULE_HEALTH;
  d.moduleVersion = 0x64;
  d.serial = serial;
  d.statusType = R1_STATUS_TYPE_NOTIFY;
  d.statusMethod = R1_STATUS_METHOD_SET;
  d.statusAck = R1_STATUS_ACK_OK;
  d.statusByte = static_cast<uint8_t>(d.statusType | (d.statusMethod << 1) |
                                      (d.statusAck << 2));
  d.cmd = cmd;
  d.subCmd = R1_SUB_DAILY;
  d.payloadLength = 20;
  d.modelLength = 32;
  d.modelLengthValid = true;
  return d;
}

static void deliver(const R1Decoded& frame) {
  gWire = frame;
  uint8_t bytes[32] = {};
  ringProcessRxFrame(bytes, sizeof(bytes), sLinkGeneration);
}

static bool ackedOnly(uint16_t serial, uint8_t cmd) {
  return sPacketAckCount == 1 && sPacketAckQueue[0].descriptor.receivedSerial() == serial &&
         sPacketAckQueue[0].descriptor.cmd() == cmd &&
         sPacketAckQueue[0].descriptor.module() == R1_MODULE_HEALTH &&
         sPacketAckQueue[0].descriptor.subCmd() == R1_SUB_DAILY &&
         sPacketAckQueue[0].generation == sLinkGeneration;
}

static void drainAckLane() { sPacketAckCount = 0; }

static void setCommonPage(R1DailyDayMode mode, uint32_t dayStart, uint8_t slot,
                          uint8_t average, uint32_t latest) {
  gCommonPage = R1CommonDailyResult{};
  gCommonPage.timezoneMinutes = 0;
  gCommonPage.dayMode = mode;
  gCommonPage.dayStart = dayStart;
  gCommonPage.latestTimestampMode = latest ? R1_DAILY_TIMESTAMP_EPOCH : R1_DAILY_TIMESTAMP_NONE;
  gCommonPage.latestTimestampRaw = latest;
  gCommonPage.latestTimestamp = latest;
  gCommonPage.latestValue = average;
  gCommonPage.count = 1;
  gCommonPage.records[0] = R1CommonDailyRecord{
      slot, average, static_cast<uint8_t>(average + 5), static_cast<uint8_t>(average - 5),
      dayStart + slot * 3600u};
}

static void setHrvPage(uint32_t dayStart, uint8_t slot, uint16_t average, uint32_t latest) {
  gHrvPage = R1HrvDailyResult{};
  gHrvPage.timezoneMinutes = 0;
  gHrvPage.dayMode = R1_DAILY_DAY_EPOCH;
  gHrvPage.dayStart = dayStart;
  gHrvPage.latestTimestampMode = R1_DAILY_TIMESTAMP_EPOCH;
  gHrvPage.latestTimestampRaw = latest;
  gHrvPage.latestTimestamp = latest;
  gHrvPage.latestValue = average;
  gHrvPage.count = 1;
  gHrvPage.records[0] = R1HrvDailyRecord{
      slot, average, static_cast<uint16_t>(average + 5), static_cast<uint16_t>(average - 5),
      dayStart + slot * 3600u};
}

static void setActivityPage(uint32_t dayStart, uint8_t slot, uint16_t steps) {
  gActivityPage = R1ActivityDailyResult{};
  gActivityPage.timezoneMinutes = 0;
  gActivityPage.dayMode = R1_DAILY_DAY_EPOCH;
  gActivityPage.dayStart = dayStart;
  gActivityPage.count = 1;
  gActivityPage.records[0] = R1ActivityDailyRecord{slot, steps, 10, 20, 30, dayStart + slot * 600u};
}

static void reassembledActivity(uint16_t serial) {
  gReasmSerial = serial;
  sRingReasm = RingReassembly{};
  sRingReasm.active = true;
  sRingReasm.crc32 = 0x0BADF00Du;
  sRingReasm.generation = sLinkGeneration;
  sRingReasm.len = 40;
  ringReasmFinalize(sLinkGeneration);
}

static void profile229() {
  // 1. A page outside any sweep (ringquery <metric> daily, dark-clock probe).
  resetLink(R1_PROFILE_FW_2_2_9_0003);
  resetHistory();
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 9, 70, kDay + 9 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 101));
  expect(sPacketAckCount == 0,
         "2.2.9: a daily page outside a sweep is refused by the Health model and must not be acknowledged");
  expect(gR1Cache.hrValid && gR1Cache.hr == 70,
         "2.2.9: a daily page outside a sweep still refreshes the live HR cache");
  expect(!sPendingHistory.heartRatePending, "no session: the model keeps nothing");

  // 2. A clean sweep accepts the page: exactly one ACK for it.
  drainAckLane();
  g2HealthHistoryFetchStarted();
  expect(sPendingHistory.active && !sDropCurrentHistoryFetch, "a clean sweep opens a session");
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 10, 72, kDay + 10 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 102));
  expect(ackedOnly(102, R1_CMD_HEARTRATE), "2.2.9: a page the model accepted gets exactly one ACK");
  expect(sPendingHistory.heartRatePending && sPendingHistory.keyKnown &&
         sPendingHistory.dayStart == kDay, "the accepted page is held by the session");

  // 3. Its retransmit after the ACK left the lane: ACKed again, not re-ingested.
  drainAckLane();
  const unsigned trendsBefore = trendCalls;
  deliver(dailyFrame(R1_CMD_HEARTRATE, 102));
  expect(ackedOnly(102, R1_CMD_HEARTRATE),
         "2.2.9: a retransmit of an accepted page is acknowledged again");
  expect(trendCalls == trendsBefore, "a retransmit is not ingested twice");

  // 4. Same sweep, another day key: refused, so no ACK (nor for its retransmit).
  drainAckLane();
  setHrvPage(kDay + 86400u, 1, 40, kDay + 86400u + 3600u + 60);
  deliver(dailyFrame(R1_CMD_HRV, 103));
  expect(sPacketAckCount == 0,
         "2.2.9: a page whose day key differs from the sweep's is refused and must not be acknowledged");
  expect(!sPendingHistory.hrvPending, "the other-day HRV page is not held");
  drainAckLane();
  deliver(dailyFrame(R1_CMD_HRV, 103));
  expect(sPacketAckCount == 0, "2.2.9: a retransmit of a refused page must not be acknowledged");

  // 5. Zero-base (unanchored) day: parses, but the model refuses it.
  drainAckLane();
  setCommonPage(R1_DAILY_DAY_ZERO_BASE, 0, 3, 96, 0);
  deliver(dailyFrame(R1_CMD_SPO2, 104));
  expect(sPacketAckCount == 0,
         "2.2.9: a zero-base daily page is refused by the model and must not be acknowledged");

  // 6. Single-frame activity in the sweep: accepted, one ACK.
  drainAckLane();
  setActivityPage(kDay, 60, 500);
  deliver(dailyFrame(R1_CMD_ACTIVITY, 105));
  expect(ackedOnly(105, R1_CMD_ACTIVITY), "2.2.9: an accepted activity page gets exactly one ACK");
  expect(sPendingHistory.activityPending, "the activity page is held by the session");
  g2HealthHistoryFetchFinished(true, 0);
  expect(sPendingHistory.terminalPending && !sPendingHistory.active,
         "the finished sweep waits for its commit");

  // 7. A second sweep while that result is uncommitted is dropped; its queries
  //    still run and refresh the live cache, but nothing is acknowledged.
  drainAckLane();
  g2HealthHistoryFetchStarted();
  expect(sDropCurrentHistoryFetch && !sPendingHistory.active,
         "a sweep behind an uncommitted result is dropped");
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 11, 74, kDay + 11 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 106));
  expect(sPacketAckCount == 0,
         "2.2.9: a page of a sweep dropped behind an uncommitted result must not be acknowledged");
  expect(gR1Cache.hrValid && gR1Cache.hr == 74 && gR1Cache.hrTs == kDay + 11 * 3600u + 60,
         "the dropped sweep's page still refreshes the live HR cache");
  g2HealthHistoryFetchFinished(true, 0);
  expect(!sDropCurrentHistoryFetch && sPendingHistory.terminalPending,
         "finishing a dropped sweep keeps the older generation");

  // The terminal was applied to the day but the store commit keeps failing.
  drainAckLane();
  std::memset(&sPendingHistory, 0, sizeof(sPendingHistory));
  sHistoryDirty.store(true);
  g2HealthHistoryFetchStarted();
  expect(sDropCurrentHistoryFetch, "a sweep behind a dirty (uncommitted) day is dropped");
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 12, 75, kDay + 12 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 107));
  expect(sPacketAckCount == 0,
         "2.2.9: a page of a sweep dropped behind a failing commit must not be acknowledged");
  g2HealthHistoryFetchFinished(true, 0);

  // 8. A pending peer switch refuses pages of an otherwise clean sweep.
  drainAckLane();
  resetHistory();
  sHistoryPeerCustody.requestPending = true;
  g2HealthHistoryFetchStarted();
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 13, 76, kDay + 13 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 108));
  expect(sPacketAckCount == 0,
         "2.2.9: a page received while a peer switch is pending must not be acknowledged");
  g2HealthHistoryFetchFinished(false, 0);
}

static void profile227() {
  // 9. 2.2.7 keeps its established flow control until a device check.
  resetLink(R1_PROFILE_FW_2_2_7_0005);
  resetHistory();
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 9, 70, kDay + 9 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 109));
  expect(ackedOnly(109, R1_CMD_HEARTRATE),
         "2.2.7: every eligible daily NOTIFY is still acknowledged, refused or not");
  drainAckLane();
  deliver(dailyFrame(R1_CMD_SLEEP, 110));
  expect(ackedOnly(110, R1_CMD_SLEEP), "2.2.7: a sleep page is still acknowledged");
  drainAckLane();
  setActivityPage(kDay, 70, 800);
  reassembledActivity(111);
  expect(ackedOnly(111, R1_CMD_ACTIVITY),
         "2.2.7: a reassembled activity day outside a sweep is still acknowledged");
  drainAckLane();
  g2HealthHistoryFetchStarted();
  const unsigned completionsBefore = reasmCompletions;
  reassembledActivity(112);
  expect(ackedOnly(112, R1_CMD_ACTIVITY) && sPendingHistory.activityPending,
         "2.2.7: an accepted reassembled activity day is acknowledged");
  expect(reasmCompletions == completionsBefore + 1, "reassembly still completes the transaction");
  g2HealthHistoryFetchFinished(true, 0);
}

int main() {
  profile229();
  profile227();
  for (const std::string& failure : failures) std::printf("FAIL: %s\n", failure.c_str());
  if (!failures.empty()) {
    std::printf("ring_daily_ack: %zu of %u checks failed\n", failures.size(), checks);
    return 1;
  }
  std::printf("ring_daily_ack: %u checks passed\n", checks);
  return 0;
}
