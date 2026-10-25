// Production G2_Ring.cpp RX/packetAck path, the G2_Health.cpp pending-session
// model and the System_R1_Protocol.cpp capability/packetAck factories are
// inserted by test_ring_daily_ack.py. Everything else is a boundary double.
#include "G2_Health.h"

#include <atomic>
#include <cctype>
#include <ctime>
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
static std::string sTag;  // profile under test, prefixed to failures
static void expect(bool ok, const char* what) {
  ++checks;
  if (!ok) failures.push_back(sTag + what);
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
static unsigned wearNotes = 0;
static uint8_t lastWear = 0xFF;
static void ringNoteWear(uint8_t wear, uint32_t) { ++wearNotes; lastWear = wear; }
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
static unsigned sleepCandidateHeaders = 0;
static bool ringRememberSleepCandidateHeader(HarnessActiveTransaction&, uint32_t,
                                             uint8_t module, uint8_t cmd, uint8_t subCmd,
                                             uint8_t statusType, uint8_t statusMethod,
                                             uint8_t statusAck, size_t payloadLength) {
  if (module == R1_MODULE_HEALTH && cmd == R1_CMD_SLEEP && subCmd == R1_SUB_DAILY &&
      statusType == R1_STATUS_TYPE_NOTIFY && statusMethod == R1_STATUS_METHOD_SET &&
      statusAck == R1_STATUS_ACK_OK && payloadLength != 0) ++sleepCandidateHeaders;
  return false;
}
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
static uint32_t lastTrendStart = 0;
static uint32_t lastTrendEnd = 0;
void g2HealthNoteSample(G2HealthMetric, int16_t, uint32_t) {}
void g2HealthApplyTrendDaily(G2HealthMetric, const uint8_t*, size_t, uint32_t startTs,
                             uint32_t endTs) {
  ++trendCalls;
  lastTrendStart = startTs;
  lastTrendEnd = endTs;
}
static unsigned backfillCalls = 0;
static uint32_t lastBackfillStart = 0;
void g2HealthApplyDailyBackfill(G2HealthMetric, const uint8_t*, size_t, uint32_t startTs, uint32_t) {
  ++backfillCalls;
  lastBackfillStart = startTs;
}
class HistoryLock {
 public:
  explicit HistoryLock(bool) {}
  bool locked() const { return true; }
};
static void ensureHistoryInitializedLocked() {}

namespace Clock {
static bool isValidEpoch(time_t t) { return t >= 1577836800; }
}

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
static R1SleepDailyResult gSleepPage;
static R1ParseError gSleepReasmError = R1_PARSE_OK;
R1ParseError r1ParseSleepDaily(R1ProtocolProfile profile, const R1Decoded& d,
                               R1SleepDailyResult& out) {
  if (d.cmd != R1_CMD_SLEEP || !r1ProfileSupportsSleepRecords(profile))
    return R1_PARSE_WRONG_PROFILE;
  out = gSleepPage;
  out.profile = profile;
  out.sourceSerial = d.serial;
  out.sourceCrc32 = d.crc32Received;
  return R1_PARSE_OK;
}
R1ParseError r1ParseReassembledSleepDaily(R1ProtocolProfile profile, const uint8_t*,
                                          size_t, uint32_t crc32Whole,
                                          R1SleepDailyResult& out) {
  if (!r1ProfileSupportsSleepRecords(profile)) return R1_PARSE_WRONG_PROFILE;
  if (gSleepReasmError != R1_PARSE_OK) return gSleepReasmError;
  out = gSleepPage;
  out.profile = profile;
  out.sourceSerial = gReasmSerial;
  out.sourceCrc32 = crc32Whole;
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

// A 7 h night ending `endOffset` seconds after `dayStart` (UTC, tz 0):
// 40 awake + 200 REM + 400 light + 200 deep epochs x 30 s.
static void setSleepPage(uint32_t dayStart, uint32_t endOffset, bool anchored) {
  gSleepPage = R1SleepDailyResult{};
  gSleepPage.type = R1_SLEEP_TYPE_LONG;
  gSleepPage.timezoneMinutes = 0;
  gSleepPage.endEpoch = dayStart + endOffset;
  gSleepPage.startEpoch = gSleepPage.endEpoch - 25200u;
  gSleepPage.anchored = anchored;
  gSleepPage.stageCount = 4;
  gSleepPage.stageEpochs[R1_SLEEP_STAGE_AWAKE] = 40;
  gSleepPage.stageEpochs[R1_SLEEP_STAGE_REM] = 200;
  gSleepPage.stageEpochs[R1_SLEEP_STAGE_LIGHT] = 400;
  gSleepPage.stageEpochs[R1_SLEEP_STAGE_DEEP] = 200;
}

static void reassembledSleep(uint16_t serial) {
  gReasmSerial = serial;
  sRingReasm = RingReassembly{};
  sRingReasm.active = true;
  sRingReasm.crc32 = 0x0BADF00Du;
  sRingReasm.generation = sLinkGeneration;
  sRingReasm.len = 300;
  std::memset(sRingReasmBuf, 0, 32);
  sRingReasmBuf[1] = R1_MODULE_HEALTH;
  sRingReasmBuf[5] = 0x02;  // notify/set/ok
  sRingReasmBuf[6] = R1_CMD_SLEEP;
  sRingReasmBuf[7] = R1_SUB_DAILY;
  sRingReasmBuf[8] = 0x2C;  // declared model length 300
  sRingReasmBuf[9] = 0x01;
  ringReasmFinalize(sLinkGeneration);
}

static void reassembledActivity(uint16_t serial) {
  gReasmSerial = serial;
  sRingReasm = RingReassembly{};
  std::memset(sRingReasmBuf, 0, 32);
  sRingReasmBuf[1] = R1_MODULE_HEALTH;
  sRingReasmBuf[6] = R1_CMD_ACTIVITY;
  sRingReasm.active = true;
  sRingReasm.crc32 = 0x0BADF00Du;
  sRingReasm.generation = sLinkGeneration;
  sRingReasm.len = 40;
  ringReasmFinalize(sLinkGeneration);
}

// 2.2.9 and both 2.3.2 profiles share the history-gated packetAck rule.
static void historyGatedProfile(R1ProtocolProfile profile) {
  // 1. A page outside any sweep (ring query <metric> daily, dark-clock probe).
  resetLink(profile);
  resetHistory();
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 9, 70, kDay + 9 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 101));
  expect(sPacketAckCount == 0,
         "a daily page outside a sweep is refused by the Health model and must not be acknowledged");
  expect(gR1Cache.hrValid && gR1Cache.hr == 70,
         "a daily page outside a sweep still refreshes the live HR cache");
  expect(!sPendingHistory.heartRatePending, "no session: the model keeps nothing");

  // 2. A clean sweep accepts the page: exactly one ACK for it.
  drainAckLane();
  g2HealthHistoryFetchStarted();
  expect(sPendingHistory.active && !sDropCurrentHistoryFetch, "a clean sweep opens a session");
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 10, 72, kDay + 10 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 102));
  expect(ackedOnly(102, R1_CMD_HEARTRATE), "a page the model accepted gets exactly one ACK");
  expect(sPendingHistory.heartRatePending && sPendingHistory.keyKnown &&
         sPendingHistory.dayStart == kDay, "the accepted page is held by the session");

  // 3. Its retransmit after the ACK left the lane: ACKed again, not re-ingested.
  drainAckLane();
  const unsigned trendsBefore = trendCalls;
  deliver(dailyFrame(R1_CMD_HEARTRATE, 102));
  expect(ackedOnly(102, R1_CMD_HEARTRATE),
         "a retransmit of an accepted page is acknowledged again");
  expect(trendCalls == trendsBefore, "a retransmit is not ingested twice");

  // 4. Same sweep, another day key: refused, so no ACK (nor for its retransmit).
  drainAckLane();
  setHrvPage(kDay + 86400u, 1, 40, kDay + 86400u + 3600u + 60);
  deliver(dailyFrame(R1_CMD_HRV, 103));
  expect(sPacketAckCount == 0,
         "a page whose day key differs from the sweep's is refused and must not be acknowledged");
  expect(!sPendingHistory.hrvPending, "the other-day HRV page is not held");
  drainAckLane();
  deliver(dailyFrame(R1_CMD_HRV, 103));
  expect(sPacketAckCount == 0, "a retransmit of a refused page must not be acknowledged");

  // 5. Zero-base (unanchored) day: parses, but the model refuses it.
  drainAckLane();
  setCommonPage(R1_DAILY_DAY_ZERO_BASE, 0, 3, 96, 0);
  deliver(dailyFrame(R1_CMD_SPO2, 104));
  expect(sPacketAckCount == 0,
         "a zero-base daily page is refused by the model and must not be acknowledged");

  // 6. Single-frame activity in the sweep: accepted, one ACK.
  drainAckLane();
  setActivityPage(kDay, 60, 500);
  deliver(dailyFrame(R1_CMD_ACTIVITY, 105));
  expect(ackedOnly(105, R1_CMD_ACTIVITY), "an accepted activity page gets exactly one ACK");
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
         "a page of a sweep dropped behind an uncommitted result must not be acknowledged");
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
         "a page of a sweep dropped behind a failing commit must not be acknowledged");
  g2HealthHistoryFetchFinished(true, 0);

  // 8. A pending peer switch refuses pages of an otherwise clean sweep.
  drainAckLane();
  resetHistory();
  sHistoryPeerCustody.requestPending = true;
  g2HealthHistoryFetchStarted();
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 13, 76, kDay + 13 * 3600u + 60);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 108));
  expect(sPacketAckCount == 0,
         "a page received while a peer switch is pending must not be acknowledged");
  g2HealthHistoryFetchFinished(false, 0);
}

static R1Decoded deviceStatusFrame(uint16_t serial, uint8_t battery, uint8_t byte1) {
  R1Decoded d{};
  d.crc32Valid = true;
  d.crc32Received = 0x5B000000u | serial;
  d.version = 0x64;
  d.module = R1_MODULE_SYSTEM;
  d.moduleVersion = 0x64;
  d.serial = serial;
  d.statusType = R1_STATUS_TYPE_ACK;
  d.statusMethod = R1_STATUS_METHOD_SET;
  d.statusAck = R1_STATUS_ACK_OK;
  d.statusByte = static_cast<uint8_t>(d.statusType | (d.statusMethod << 1) |
                                      (d.statusAck << 2));
  d.cmd = R1_CMD_SYSTEM;
  d.subCmd = R1_SUB_DEVICE_STATUS;
  d.payloadLength = 7;
  d.payload[0] = battery;
  d.payload[1] = byte1;
  d.payload[2] = 1;
  d.modelLength = 19;
  d.modelLengthValid = true;
  return d;
}

static R1Decoded wearStatusPush(uint16_t serial, uint8_t wear) {
  R1Decoded d = deviceStatusFrame(serial, 0, 0);
  d.statusType = R1_STATUS_TYPE_NOTIFY;
  d.statusByte = static_cast<uint8_t>(d.statusType | (d.statusMethod << 1) |
                                      (d.statusAck << 2));
  d.subCmd = R1_SUB_WEAR_STATUS;
  d.crc32Received = 0x5C000000u | serial;
  d.payloadLength = 1;
  d.payload[0] = wear;
  d.payload[1] = d.payload[2] = 0;
  d.modelLength = 13;
  return d;
}

// Behaviour the 2.3.2 captures add on top of the shared history-gated rule.
static void profile232(R1ProtocolProfile profile) {
  resetLink(profile);
  resetHistory();
  g2HealthHistoryFetchStarted();

  // A night that ends on the sweep's day is stored, then acknowledged.
  setSleepPage(kDay, 6 * 3600u, true);
  deliver(dailyFrame(R1_CMD_SLEEP, 201));
  expect(ackedOnly(201, R1_CMD_SLEEP), "a stored sleep record gets exactly one ACK");
  expect(sPendingHistory.sleepRecordsPending && sPendingHistory.sleepRecords.count == 1 &&
         sPendingHistory.dayStart == kDay, "the night is filed under the day it ends on");
  // A night recorded before the ring knew the time is never stored or ACKed.
  drainAckLane();
  setSleepPage(kDay, 7 * 3600u, false);
  deliver(dailyFrame(R1_CMD_SLEEP, 211));
  expect(sPacketAckCount == 0, "an unanchored sleep record is not acknowledged");
  // A night ending on a newer day stays on the ring and marks a backlog.
  setSleepPage(kDay + 86400u, 6 * 3600u, true);
  deliver(dailyFrame(R1_CMD_SLEEP, 212));
  expect(sPacketAckCount == 0, "a newer day's sleep record is not acknowledged");
  // A long night stitched from several notifications is stored and ACKed.
  setSleepPage(kDay, 9 * 3600u, true);
  reassembledSleep(213);
  expect(ackedOnly(213, R1_CMD_SLEEP) && sPendingHistory.sleepRecords.count == 2,
         "a reassembled sleep record is stored and acknowledged once");

  // Full activity days arrive as 3-part frames; an accepted one is ACKed once.
  drainAckLane();
  setActivityPage(kDay, 70, 800);
  const unsigned completionsBefore = reasmCompletions;
  reassembledActivity(202);
  expect(ackedOnly(202, R1_CMD_ACTIVITY) && sPendingHistory.activityPending,
         "an accepted reassembled activity day gets exactly one ACK");
  expect(reasmCompletions == completionsBefore + 1, "reassembly completes the transaction");
  g2HealthHistoryFetchFinished(true, 0);
  expect(g2HealthHistoryBacklogPending(),
         "a sweep that refused a newer day reports a backlog for the next round");

  // Outside a sweep the model refuses a reassembled day, so it is not ACKed.
  drainAckLane();
  resetHistory();
  setActivityPage(kDay + 86400u, 80, 900);
  reassembledActivity(203);
  expect(sPacketAckCount == 0, "a reassembled activity day outside a sweep is not acknowledged");

  // A backlog page repeats the ring-wide newest sample time (three days later
  // here). Clock custody still sees that stamp, but the page's own trend span
  // must end inside its own day.
  drainAckLane();
  const uint32_t newest = kDay + 3 * 86400u + 11 * 3600u + 763u;
  const unsigned backfillBefore = backfillCalls;
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 22, 66, newest);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 204));
  expect(lastTrendStart == kDay + 22 * 3600u, "a backlog page's trend starts at its first bucket");
  expect(lastTrendEnd == kDay + 22 * 3600u + 3599u,
         "a backlog page's trend ends inside its own last bucket, not at the ring-wide newest sample");
  expect(sRingTsSeen == newest, "clock custody still adopts the ring-wide newest sample");
  expect(backfillCalls == backfillBefore,
         "an older backlog page never seeds the now-anchored live backfill");
  // Page-end edges: slot 23 ends at the day's last second; an older page ends
  // in its last bucket, not at day end.
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 23, 66, newest);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 302));
  expect(lastTrendEnd == kDay + 86399u, "slot-23 page ends at its day's last second");
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 10, 66, newest);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 301));
  expect(lastTrendEnd == kDay + 10 * 3600u + 3599u,
         "an older page ends inside its last bucket, not at day end");
  // A next-morning newest sample, or one at next midnight, does not end
  // yesterday's page; one at the last bucket's start ends the page there.
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 22, 66, kDay + 86400u + 1800u);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 307));
  expect(lastTrendEnd == kDay + 22 * 3600u + 3599u,
         "a next-morning newest sample does not end yesterday's page");
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 22, 66, kDay + 86400u);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 308));
  expect(lastTrendEnd == kDay + 22 * 3600u + 3599u,
         "a newest sample at next midnight does not end the older page");
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 22, 66, kDay + 22 * 3600u);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 309));
  expect(lastTrendEnd == kDay + 22 * 3600u,
         "a newest sample at the last bucket's start ends the page there");
  // Multi-record pages end in their LAST bucket (HR and HRV branches).
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 9, 66, newest);
  gCommonPage.count = 2;
  gCommonPage.records[1] = R1CommonDailyRecord{14, 70, 75, 65, kDay + 14 * 3600u};
  deliver(dailyFrame(R1_CMD_HEARTRATE, 305));
  expect(lastTrendStart == kDay + 9 * 3600u, "multi-record page starts at its first record");
  expect(lastTrendEnd == kDay + 14 * 3600u + 3599u, "multi-record page ends at its last record");
  setHrvPage(kDay, 22, 40, newest);
  deliver(dailyFrame(R1_CMD_HRV, 304));
  expect(lastTrendEnd == kDay + 22 * 3600u + 3599u, "an HRV backlog page ends inside its day");
  setHrvPage(kDay, 9, 40, newest);
  gHrvPage.count = 2;
  gHrvPage.records[1] = R1HrvDailyRecord{14, 42, 47, 37, kDay + 14 * 3600u};
  deliver(dailyFrame(R1_CMD_HRV, 306));
  expect(lastTrendEnd == kDay + 14 * 3600u + 3599u, "a multi-record HRV page ends at its last record");
  // A 0007 page: the parser leaves a 1970-range latest at 0.
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay, 9, 66, 0);
  gCommonPage.latestTimestampMode = R1_DAILY_TIMESTAMP_UNKNOWN;
  gCommonPage.latestTimestampRaw = 39270u;
  const unsigned unanchoredBackfill = backfillCalls;
  deliver(dailyFrame(R1_CMD_HEARTRATE, 303));
  expect(lastTrendStart == kDay + 9 * 3600u && lastTrendEnd == kDay + 9 * 3600u + 3599u,
         "an unanchored (0007) latest ends the page inside its last bucket");
  expect(backfillCalls == unanchoredBackfill + 1, "an unanchored page may still backfill");

  // The newest day's page keeps the newest sample as its end, and seeds the
  // live backfill.
  const unsigned newestBackfill = backfillCalls;
  setCommonPage(R1_DAILY_DAY_EPOCH, kDay + 3 * 86400u, 10, 68, newest);
  deliver(dailyFrame(R1_CMD_HEARTRATE, 205));
  expect(lastTrendEnd == newest, "the newest day's trend ends at its newest sample");
  expect(backfillCalls == newestBackfill + 1 &&
             lastBackfillStart == kDay + 3 * 86400u + 10 * 3600u,
         "the page holding the newest sample seeds the live backfill");

  // A multi-part sleep frame that fails to parse still proves sleep data was
  // sent, so the stage can never conclude EMPTY; nothing is ACKed.
  drainAckLane();
  const unsigned candidatesBefore = sleepCandidateHeaders;
  gSleepReasmError = R1_PARSE_LENGTH;
  reassembledSleep(310);
  gSleepReasmError = R1_PARSE_OK;
  expect(sleepCandidateHeaders == candidatesBefore + 1 && sPacketAckCount == 0,
         "a rejected multi-part sleep frame records presence, not EMPTY, and no ACK");

  // The sweep stays open until the ring has been quiet after its last stage.
  expect(!ringHistorySweepSettled(1000, 1000, 0), "a sweep does not close at once");
  expect(ringHistorySweepSettled(1000 + RING_HISTORY_SETTLE_QUIET_MS, 1000, 0),
         "a quiet ring closes the sweep after the quiet window");
  // 900 ms after the last stage but only 400 ms after a trailing page: still open.
  expect(!ringHistorySweepSettled(1900, 1000, 1500),
         "a page after the last stage restarts the quiet window");
  expect(ringHistorySweepSettled(1500 + RING_HISTORY_SETTLE_QUIET_MS, 1000, 1500),
         "the sweep closes a quiet window after the trailing page");
  expect(ringHistorySweepSettled(1000 + RING_HISTORY_SETTLE_MAX_MS, 1000,
                                 1000 + RING_HISTORY_SETTLE_MAX_MS),
         "a ring that keeps talking still closes the sweep at the cap");
  // Last stage done 256 ms before millis() wraps; last page 256 ms earlier.
  expect(!ringHistorySweepSettled(0x10u, 0xFFFFFF00u, 0xFFFFFE00u) &&
             ringHistorySweepSettled(0xFFFFFF00u + RING_HISTORY_SETTLE_QUIET_MS,
                                     0xFFFFFF00u, 0xFFFFFE00u),
         "the settle window survives millis() wrap");

  // Build 243 reads deviceStatus byte 1 as charge status: battery only.
  const unsigned wearBefore = wearNotes;
  deliver(deviceStatusFrame(206, 45, 2));
  expect(gR1Cache.batteryValid && gR1Cache.battery == 45, "deviceStatus battery is ingested");
  expect(wearNotes == wearBefore, "deviceStatus byte 1 is not read as wear");
  expect(gR1Cache.chargeValid && gR1Cache.charge == 2, "deviceStatus byte 1 is the charge status");
  // On the charger the ring pushes the same status as NOTIFY/SET/OK, "full" = 3.
  R1Decoded push = deviceStatusFrame(214, 99, 3);
  push.statusType = R1_STATUS_TYPE_NOTIFY;
  push.statusByte = static_cast<uint8_t>(push.statusType | (push.statusMethod << 1) |
                                         (push.statusAck << 2));
  deliver(push);
  expect(gR1Cache.battery == 99 && gR1Cache.charge == 3,
         "a pushed charging status refreshes battery and charge");
  // A GET reply from a full ring (byte 1 = 3) is valid on 2.3.2, so Poll Now
  // does not end partial while the ring sits full on its charger.
  deliver(deviceStatusFrame(216, 100, 3));
  expect(gR1Cache.battery == 100 && gR1Cache.charge == 3,
         "a 'full' deviceStatus reply is accepted");

  // The ring's own wearStatus push is the 2.3.2 wear source.
  deliver(wearStatusPush(208, 2));
  expect(wearNotes == wearBefore + 1 && lastWear == 2, "a pushed wearStatus is recorded");
  // Lifted off the charger the ring sends notWear, then possiblyWear while it
  // decides; keeping the earlier on/off would show a stale state.
  deliver(wearStatusPush(209, 3));
  expect(wearNotes == wearBefore + 2 && lastWear == 3,
         "a pushed possiblyWear is recorded as its own state");
  deliver(wearStatusPush(217, 4));
  expect(wearNotes == wearBefore + 2 && lastWear == 3, "an undefined wear code is not recorded");
}

static std::string profileTag(R1ProtocolProfile profile);

// What the Health UI and `ring health` read: wear for every profile with a wear
// source, charge only where byte 1 is charge status, nothing for unknown.
static G2RingTelemetry publishTelemetry(R1ProtocolProfile profile,
                                        const R1TelemetryCache& cache) {
  RingTelemetrySnapshot snapshot{};
  snapshot.cache = cache;
  snapshot.source = RING_TELEMETRY_DIRECT;
  snapshot.telemetryGeneration = snapshot.linkGeneration = 7;
  snapshot.online = true;
  snapshot.profile = profile;
  G2RingTelemetry out{};
  ringPopulateTelemetryOutput(&snapshot, out);
  return out;
}

static void telemetryPublication() {
  R1TelemetryCache cache{};
  cache.wearValid = true;
  cache.wear = 2;
  cache.wearRxMs = fakeMillis;
  cache.chargeValid = true;
  cache.charge = 1;
  for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_2_7_0005, R1_PROFILE_FW_2_2_9_0003,
                                          R1_PROFILE_FW_2_3_2_0007, R1_PROFILE_FW_2_3_2_0008}) {
    sTag = profileTag(profile);
    const G2RingTelemetry out = publishTelemetry(profile, cache);
    expect(out.wearValid && out.wear == 2 && out.wearAgeSec == 0, "wear is published");
    const bool chargeProfile = profile == R1_PROFILE_FW_2_3_2_0007 ||
                               profile == R1_PROFILE_FW_2_3_2_0008;
    expect(out.chargeValid == chargeProfile && (!chargeProfile || out.charge == 1),
           "charge status is published only where byte 1 is charge status");
  }
  sTag.clear();
  R1TelemetryCache maybe = cache;
  maybe.wear = 3;
  const G2RingTelemetry possibly = publishTelemetry(R1_PROFILE_FW_2_3_2_0008, maybe);
  expect(possibly.wearValid && possibly.wear == 3, "possiblyWear is published as 3");
  const G2RingTelemetry unknown = publishTelemetry(R1_PROFILE_UNKNOWN, cache);
  expect(!unknown.wearValid && unknown.wearAgeSec == -1 && !unknown.chargeValid,
         "an unknown profile publishes no wear or charge");
}

// One wording for every wear display: the G2 overview (wearLabel), and the
// OLED, `ring health` and web page through g2RingWearName.
static void wearNames() {
  struct Case { bool valid; uint8_t wear; const char* name; };
  for (const Case c : {Case{false, 2, "--"}, Case{true, 0, "?"}, Case{true, 1, "off"},
                       Case{true, 2, "on"}, Case{true, 3, "maybe"}, Case{true, 4, "?"}}) {
    expect(std::strcmp(g2RingWearName(c.valid, c.wear), c.name) == 0,
           (std::string("wear name for ") + std::to_string(c.wear) + " is " + c.name).c_str());
    G2RingTelemetry t{};
    t.wearValid = c.valid;
    t.wear = c.wear;
    char label[16];
    expect(std::string(wearLabel(t, label, sizeof(label))) == std::string("Wear ") + c.name,
           (std::string("G2 overview shows Wear ") + c.name).c_str());
  }
}

// The 2.3.2 battery poll: once per link right after setup, then after five
// minutes without a battery sample. Pushes and Poll Now's read defer it, a
// running Poll Now suppresses it, and only the 2.3.2 profiles poll at all.
static void deviceStatusPollCadence() {
  const uint32_t poll = RING_DEVICE_STATUS_POLL_MS;
  const uint32_t retry = RING_DEVICE_STATUS_RETRY_MS;
  const uint32_t now = 10u * 60u * 1000u;
  const RingDeviceStatusPollState fresh{0, 0, 0};
  auto due = [&](R1ProtocolProfile profile, bool setup, bool refresh, uint32_t gen,
                 bool heard, uint32_t rxMs, RingDeviceStatusPollState state, uint32_t at) {
    return ringDeviceStatusPollDue(profile, setup, refresh, gen, heard, rxMs, state, at);
  };
  for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_3_2_0007, R1_PROFILE_FW_2_3_2_0008}) {
    sTag = profileTag(profile);
    expect(due(profile, true, false, 7, false, 0, fresh, now), "asks as soon as setup is done");
    expect(!due(profile, false, false, 7, false, 0, fresh, now), "no ask before setup is done");
    expect(!due(profile, true, true, 7, false, 0, fresh, now), "no ask while Poll Now runs");
    expect(!due(profile, true, false, 0, false, 0, fresh, now), "no ask without a live link");
  }
  sTag.clear();
  for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_2_7_0005, R1_PROFILE_FW_2_2_9_0003,
                                          R1_PROFILE_UNKNOWN}) {
    sTag = profileTag(profile);
    expect(!due(profile, true, false, 7, false, 0, fresh, now), "only 2.3.2 profiles poll");
  }
  sTag = profileTag(R1_PROFILE_FW_2_3_2_0008);
  const R1ProtocolProfile p = R1_PROFILE_FW_2_3_2_0008;
  const RingDeviceStatusPollState asked{7, now - 1000, 0};
  expect(!due(p, true, false, 7, false, 0, asked, now), "an unanswered ask waits");
  expect(!due(p, true, false, 7, false, 0, RingDeviceStatusPollState{7, now - (poll - 1), 0}, now),
         "an ask just under five minutes old still waits");
  expect(due(p, true, false, 7, false, 0, RingDeviceStatusPollState{7, now - poll, 0}, now),
         "asks again five minutes after the last ask");
  expect(due(p, true, false, 8, false, 0, asked, now), "a new link asks at once");
  const RingDeviceStatusPollState oldAsk{7, now - 2 * poll, 0};
  expect(!due(p, true, false, 7, true, now - 10000, oldAsk, now),
         "a fresh battery sample (charger push, Poll Now) defers the ask");
  expect(due(p, true, false, 7, true, now - poll, oldAsk, now),
         "five quiet minutes after the last sample brings the next ask");
  expect(!due(p, true, false, 7, true, now - 2 * poll, RingDeviceStatusPollState{7, now - 60000, 0}, now),
         "a recent ask counts even when the last sample is older");
  expect(!due(p, true, false, 7, false, 0, RingDeviceStatusPollState{7, 0, now - (retry - 1)}, now),
         "a refused enqueue waits for the retry gap");
  expect(due(p, true, false, 7, false, 0, RingDeviceStatusPollState{7, 0, now - retry}, now),
         "a refused enqueue retries after the gap");
  expect(due(p, true, false, 8, false, 0, RingDeviceStatusPollState{7, 0, now - 1000}, now),
         "a refusal on the old link does not hold back a new one");
  const uint32_t beforeWrap = 0xFFFFF000u;
  const RingDeviceStatusPollState wrapped{7, beforeWrap, 0};
  expect(!due(p, true, false, 7, false, 0, wrapped, beforeWrap + 0x2000u),
         "an ask just before millis() wraps still waits after it");
  expect(due(p, true, false, 7, false, 0, wrapped, beforeWrap + poll),
         "the cadence survives millis() wrap");
  sTag.clear();
}

static void profile229DeviceStatusWear() {
  resetLink(R1_PROFILE_FW_2_2_9_0003);
  const unsigned wearBefore = wearNotes;
  deliver(deviceStatusFrame(207, 45, 2));
  expect(gR1Cache.batteryValid && wearNotes == wearBefore + 1,
         "2.2.9: deviceStatus keeps its established battery and wear reading");
  deliver(wearStatusPush(210, 2));
  expect(wearNotes == wearBefore + 1, "2.2.9: a wearStatus frame is still not ingested");
  expect(!gR1Cache.chargeValid, "2.2.9: no charge status is derived");
  R1Decoded push = deviceStatusFrame(215, 50, 1);
  push.statusType = R1_STATUS_TYPE_NOTIFY;
  push.statusByte = static_cast<uint8_t>(push.statusType | (push.statusMethod << 1) |
                                         (push.statusAck << 2));
  deliver(push);
  expect(gR1Cache.battery == 45, "2.2.9: a pushed status is not trusted as battery");
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

// r1ProtocolProfileName lives outside the inserted protocol subset.
static std::string profileTag(R1ProtocolProfile profile) {
  switch (profile) {
    case R1_PROFILE_FW_2_2_9_0003: return "2.2.9.0003: ";
    case R1_PROFILE_FW_2_3_2_0007: return "2.3.2.0007: ";
    case R1_PROFILE_FW_2_3_2_0008: return "2.3.2.0008: ";
    default: return "?: ";
  }
}

int main() {
  for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_2_9_0003, R1_PROFILE_FW_2_3_2_0007,
                                          R1_PROFILE_FW_2_3_2_0008}) {
    sTag = profileTag(profile);
    historyGatedProfile(profile);
  }
  for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_3_2_0007, R1_PROFILE_FW_2_3_2_0008}) {
    sTag = profileTag(profile);
    profile232(profile);
  }
  telemetryPublication();
  sTag.clear();
  wearNames();
  deviceStatusPollCadence();
  profile229DeviceStatusWear();
  profile227();
  for (const std::string& failure : failures) std::printf("FAIL: %s\n", failure.c_str());
  if (!failures.empty()) {
    std::printf("ring_daily_ack: %zu of %u checks failed\n", failures.size(), checks);
    return 1;
  }
  std::printf("ring_daily_ack: %u checks passed\n", checks);
  return 0;
}
