// Host harness for elapsed-time tests that used a signed difference.
//
// test_elapsed_time.py inserts, from the production sources:
//   * G2_Health.cpp: the sync stamp, the refresh-status and logging-status
//     state, g2HealthTick, healthPollStatusLine and g2HealthLoggingStatusLine;
//   * G2_Ring.h / G2_Ring.cpp: G2_RING_POLL_VITAL_COUNT and
//     g2RingPollVitalForLogging.
// Host boundaries: a settable 32-bit millis() (unsigned long is 64 bits here,
// so the double returns uint32_t as the device does), the ring link state and
// counting stand-ins for the ring poll and the telemetry sync. The driver
// writes `long` in the inserted text as the 32-bit type it is on the device.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static uint32_t gNowMs = 0;
static uint32_t millis() { return gNowMs; }

// ---- G2_Health.cpp boundaries -------------------------------------------------
static bool gRefreshSupported = true;
static bool g2RingHealthPageRefreshSupported() { return gRefreshSupported; }
static unsigned gMenuBumps = 0, gSyncs = 0;
static void bumpMenuGen() { ++gMenuBumps; }
static void syncFromTelemetry() { ++gSyncs; }

// ---- G2_Ring.cpp boundaries ---------------------------------------------------
static struct { bool connected = true; bool writeChar = true; } gRing;
static bool g2RingPointPollingSupported() { return true; }
static unsigned gVitalPolls = 0;
static uint8_t gLastVital = 0xFF;
static bool g2RingPollVital(uint8_t which) { ++gVitalPolls; gLastVital = which; return true; }

// INSERT_HEALTH_STATE

// INSERT_HEALTH_FUNCTIONS


// INSERT_RING

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

int main() {
  constexpr uint32_t kDay = 24u * 60u * 60u * 1000u;
  // A stamp this old is where a signed 32-bit difference turns negative: more
  // than 24.9 days, less than 49.7.
  const uint32_t kIdle[] = {25u * kDay, 30u * kDay, 49u * kDay};

  // ---- g2HealthTick: sync from telemetry at most once a second ----------------
  gNowMs = 5000;
  g2HealthTick();
  check(gSyncs == 1, "the first tick syncs");
  gNowMs = 5999;
  g2HealthTick();
  check(gSyncs == 1, "no second sync inside one second");
  gNowMs = 6000;
  g2HealthTick();
  check(gSyncs == 2, "a sync after one second");
  for (uint32_t idle : kIdle) {
    const unsigned before = gSyncs;
    gNowMs += idle;  // the Health page was closed for that long
    g2HealthTick();
    check(gSyncs == before + 1, "g2HealthTick syncs after " + std::to_string(idle / kDay) + " idle days");
  }
  // Across the wrap.
  gNowMs = 0xFFFFFE00u;
  g2HealthTick();
  const unsigned atWrap = gSyncs;
  gNowMs = 0x00000100u;  // 768 ms later
  g2HealthTick();
  check(gSyncs == atWrap, "768 ms across the wrap is inside the second");
  gNowMs = 0x00000300u;  // 1280 ms later
  g2HealthTick();
  check(gSyncs == atWrap + 1, "1280 ms across the wrap is a new second");

  // ---- healthPollStatusLine: a refresh result shows for four seconds ----------
  auto finishPoll = [](uint32_t atMs) { gNowMs = atMs; sPollUi = POLL_UI_DONE; sPollUiMs = atMs; };
  finishPoll(100000);
  check(std::strcmp(healthPollStatusLine("idle"), "Refreshed") == 0, "the result shows at once");
  gNowMs = 103999;
  check(std::strcmp(healthPollStatusLine("idle"), "Refreshed") == 0, "the result shows for 4 s");
  gNowMs = 104000;
  check(std::strcmp(healthPollStatusLine("idle"), "idle") == 0, "after 4 s the fallback shows");
  check(sPollUi == POLL_UI_IDLE, "and the result is retired");
  for (uint32_t idle : kIdle) {
    finishPoll(100000);
    gNowMs = 100000u + idle;  // nobody looked at the page for that long
    check(std::strcmp(healthPollStatusLine("idle"), "idle") == 0,
          "a refresh result is not shown as fresh " + std::to_string(idle / kDay) + " days later");
    check(sPollUi == POLL_UI_IDLE, "and it is retired");
  }
  finishPoll(0xFFFFFF00u);
  gNowMs = 0x00000800u;  // 2304 ms later, across the wrap
  check(std::strcmp(healthPollStatusLine("idle"), "Refreshed") == 0, "the result shows across the wrap");
  gNowMs = 0x00001000u;  // 4352 ms later
  check(std::strcmp(healthPollStatusLine("idle"), "idle") == 0, "and ends after 4 s across the wrap");

  // ---- g2HealthLoggingStatusLine: a refusal shows for kLoggingUiHoldMs --------
  auto refuse = [](uint32_t atMs) {
    gNowMs = atMs;
    std::snprintf(sLoggingUiMsg, sizeof(sLoggingUiMsg), "Not paired");
    sLoggingUiMs = atMs;
  };
  refuse(200000);
  check(g2HealthLoggingStatusLine() != nullptr, "the refusal shows at once");
  gNowMs = 200000u + kLoggingUiHoldMs - 1;
  check(g2HealthLoggingStatusLine() != nullptr, "the refusal shows for its hold time");
  gNowMs = 200000u + kLoggingUiHoldMs;
  check(g2HealthLoggingStatusLine() == nullptr && sLoggingUiMsg[0] == '\0', "then it is cleared");
  for (uint32_t idle : kIdle) {
    refuse(200000);
    gNowMs = 200000u + idle;
    check(g2HealthLoggingStatusLine() == nullptr,
          "a refusal is not shown again " + std::to_string(idle / kDay) + " days later");
    check(sLoggingUiMsg[0] == '\0', "and it is cleared");
  }

  // ---- g2RingPollVitalForLogging: one vital at most every 700 ms --------------
  gNowMs = 300000;
  g2RingPollVitalForLogging();
  check(gVitalPolls == 1 && gLastVital == 0, "the first call polls vital 0");
  gNowMs = 300699;
  g2RingPollVitalForLogging();
  check(gVitalPolls == 1, "no second poll inside 700 ms");
  gNowMs = 300700;
  g2RingPollVitalForLogging();
  check(gVitalPolls == 2 && gLastVital == 1, "the next vital after 700 ms");
  for (uint32_t idle : kIdle) {
    const unsigned before = gVitalPolls;
    gNowMs += idle;  // the ring was away, or logging was off, for that long
    g2RingPollVitalForLogging();
    check(gVitalPolls == before + 1, "the ring is polled again after " + std::to_string(idle / kDay) + " idle days");
  }
  check(gLastVital < G2_RING_POLL_VITAL_COUNT, "the vital cursor stays in range");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d elapsed-time checks failed\n", failures, checks);
    return 1;
  }
  std::printf("elapsed time: %d checks passed\n", checks);
  return 0;
}
