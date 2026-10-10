#include "ntp_transport_platform.h"
#include "System_NTP.h"
#include <cstdarg>

NtpFixture fixture;
FakeSettings gSettings;
FakeWiFi WiFi;
bool gRtcRunning = false, gRtcConnected = false;
static constexpr time_t VALID_TIME = 1700000000;
static constexpr time_t REPLY_TIME = VALID_TIME + 60;

static void reply(time_t epoch = REPLY_TIME) {
  assert(fixture.callback && !fixture.inCallback && !fixture.criticalDepth);
  const size_t logs = fixture.logs.size(), steps = fixture.steps.size();
  const int anchors = fixture.anchorCalls, resolves = fixture.resolveCalls;
  const size_t markers = fixture.markers.size();
  const int refreshes = fixture.refreshCalls;
  fixture.epoch = epoch;  // lwIP steps the clock before notifying the application.
  timeval value{epoch, 0};
  fixture.inCallback = true;
  fixture.callback(&value);
  fixture.inCallback = false;
  assert(fixture.logs.size() == logs && fixture.steps.size() == steps);
  assert(fixture.anchorCalls == anchors && fixture.resolveCalls == resolves);
  assert(fixture.markers.size() == markers && fixture.refreshCalls == refreshes + 1);
  assert(fixture.criticalDepth == 0);
}

namespace Clock {
int tzOffsetMinutes() { return fixture.timezoneMinutes; }
bool isValidEpoch(time_t epoch) { return epoch >= VALID_TIME; }
SyncSource syncSource() { return fixture.priorSource; }
const char* syncSourceName(SyncSource source) {
  assert(source == SYNC_RTC);
  return "rtc";
}
int64_t projectedEpochUs() { return fixture.projectionUs; }
void refreshProjection() {
  ++fixture.refreshCalls;
  fixture.projectionUs = static_cast<int64_t>(fixture.epoch) * 1000000;
  if (!fixture.inCallback) fixture.setupOrder.push_back("projection");
}
void applyTimezone() { fixture.setupOrder.push_back("timezone"); }
void clockStepped(SyncSource source, time_t before) {
  assert(!fixture.inCallback && !fixture.criticalDepth);
  fixture.steps.push_back({source, before});
  if (fixture.replyDuringDrain) {
    fixture.replyDuringDrain = false;
    reply(REPLY_TIME + 60);
  }
}
}

void broadcastOutput(const char* message) {
  assert(!fixture.inCallback && !fixture.criticalDepth);
  fixture.logs.emplace_back(message);
}
void ntpTestBroadcastPrintf(const char* format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  broadcastOutput(message);
}
void writeBootAnchor() { assert(!fixture.inCallback); ++fixture.anchorCalls; }
void resolvePendingUserCreationTimes() { assert(!fixture.inCallback); ++fixture.resolveCalls; }
void logTimeSyncedMarkerIfReady(const char* source) {
  assert(!fixture.inCallback);
  fixture.markers.emplace_back(source);
}
void oledUpdate() {}
void delay(unsigned long milliseconds) {
  assert(milliseconds == 100);
  fixture.nowUs += milliseconds * 1000;
  if (++fixture.delayCalls == fixture.replyAfterDelay) reply();
}
int64_t esp_timer_get_time() { return fixture.nowUs; }
bool esp_sntp_enabled() {
  if (fixture.replyAtDeadline) {
    fixture.replyAtDeadline = false;
    reply();  // Arrives after the last poll but before the atomic timeout mark.
  }
  return fixture.armed;
}
void sntp_set_time_sync_notification_cb(void (*callback)(timeval*)) {
  fixture.callback = callback;
  fixture.setupOrder.push_back("callback");
}
void configTime(long offset, int daylight, const char* primary, const char* secondary, const char* tertiary) {
  assert(fixture.callback && fixture.refreshCalls);
  assert(offset == fixture.timezoneMinutes * 60 && daylight == 0);
  assert(std::strcmp(secondary, "time.google.com") == 0);
  assert(std::strcmp(tertiary, "time.cloudflare.com") == 0);
  assert(primary != gSettings.ntpServer.c_str());
  fixture.retainedServer = primary;
  fixture.armed = fixture.armClient;
  ++fixture.configCalls;
  fixture.setupOrder.push_back("configTime");
  if (fixture.replyOnConfig) reply();
}
bool rtcSyncToSystem() {
  ++fixture.rtcCalls;
  if (fixture.rtcSuccess) Clock::clockStepped(Clock::SYNC_RTC, fixture.epoch);
  return fixture.rtcSuccess;
}
size_t ntpTestStrlcpy(char* destination, const char* source, size_t size) {
  const size_t length = std::strlen(source);
  if (size) {
    const size_t copied = length < size - 1 ? length : size - 1;
    std::memcpy(destination, source, copied);
    destination[copied] = '\0';
  }
  return length;
}
time_t ntpTestTime(time_t* output) {
  if (output) *output = fixture.epoch;
  return fixture.epoch;
}
static size_t messagesContaining(const char* text) {
  size_t count = 0;
  for (const auto& message : fixture.logs) if (message.find(text) != std::string::npos) ++count;
  return count;
}
static void drainOnce(time_t before) {
  assert(fixture.steps.empty());
  ntpSyncDrainTick();
  assert(fixture.steps.size() == 1);
  assert(fixture.steps[0].source == Clock::SYNC_NTP && fixture.steps[0].before == before);
  ntpSyncDrainTick();
  assert(fixture.steps.size() == 1);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  NtpSyncOutcome outcome = NtpSyncOutcome::Reply;
  if (scenario == "setup") {
    setupNTP();
    assert((fixture.setupOrder == std::vector<std::string>{"callback", "projection", "configTime", "timezone"}));
    const char* retained = fixture.retainedServer;
    gSettings.ntpServer = std::string(200, 'x');
    assert(std::strcmp(retained, "pool.example.test") == 0);
    setupNTP();
    assert(fixture.retainedServer == retained && std::strlen(retained) == 63);
    assert(std::string(retained) == std::string(63, 'x'));
    assert(getNtpRuntimeStatus().clientArmed && getNtpRuntimeStatus().replyCount == 0);
    ntpSyncDrainTick();
    assert(fixture.steps.empty());
  } else if (scenario == "disconnected") {
    fixture.connected = false;
    fixture.epoch = VALID_TIME;
    assert(!syncNTPAndResolve(&outcome) && outcome == NtpSyncOutcome::Failed);
    assert(!fixture.configCalls && !fixture.delayCalls && !fixture.anchorCalls);
    assert(getNtpRuntimeStatus().foregroundTimeoutCount == 0);
  } else if (scenario == "cold_reply" || scenario == "warm_reply" || scenario == "deadline_reply") {
    const bool warm = scenario == "warm_reply";
    fixture.epoch = warm ? VALID_TIME : 0;
    fixture.replyOnConfig = scenario == "cold_reply";
    fixture.replyAfterDelay = warm ? 3 : 0;
    fixture.replyAtDeadline = scenario == "deadline_reply";
    assert(syncNTPAndResolve(&outcome) && outcome == NtpSyncOutcome::Reply);
    assert(fixture.delayCalls == (warm ? 3 : scenario == "cold_reply" ? 1 : 150));
    assert(fixture.anchorCalls == int(warm) && fixture.resolveCalls == int(warm));
    assert(fixture.markers == (warm ? std::vector<std::string>{"ntp"} : std::vector<std::string>{}));
    const auto status = getNtpRuntimeStatus();
    assert(status.replyCount == 1 && status.foregroundTimeoutCount == 0 && status.lateReplyCount == 0);
    assert(status.lastReplyEpoch == REPLY_TIME && !status.awaitingLateReply);
    drainOnce(warm ? VALID_TIME : 0);
  } else if (scenario == "carryover" || scenario == "prior_rtc") {
    fixture.epoch = VALID_TIME;
    fixture.priorSource = scenario == "prior_rtc" ? Clock::SYNC_RTC : Clock::SYNC_NONE;
    assert(syncNTPAndResolve(&outcome) && outcome == NtpSyncOutcome::KeptPrior);
    assert(fixture.delayCalls == 70 && fixture.anchorCalls == 1 && fixture.resolveCalls == 1);
    assert(fixture.markers == std::vector<std::string>{scenario == "prior_rtc" ? "rtc" : "carryover"});
    const auto status = getNtpRuntimeStatus();
    assert(status.replyCount == 0 && status.foregroundTimeoutCount == 1 && status.awaitingLateReply);
    assert(!messagesContaining("NTP time synchronized"));
    ntpSyncDrainTick();
    assert(fixture.steps.empty());
  } else if (scenario == "rtc_fallback" || scenario == "rtc_failed") {
    gRtcRunning = gRtcConnected = true;
    fixture.rtcSuccess = scenario == "rtc_fallback";
    assert(syncNTPAndResolve(&outcome) == fixture.rtcSuccess);
    assert(outcome == (fixture.rtcSuccess ? NtpSyncOutcome::RtcFallback : NtpSyncOutcome::Failed));
    assert(fixture.rtcCalls == 1 && fixture.delayCalls == 150);
    assert(fixture.steps.size() == size_t(fixture.rtcSuccess));
    if (fixture.rtcSuccess) assert(fixture.steps[0].source == Clock::SYNC_RTC);
    ntpSyncDrainTick();
    assert(fixture.steps.size() == size_t(fixture.rtcSuccess));
    assert(!fixture.anchorCalls && !fixture.resolveCalls && fixture.markers.empty());
  } else if (scenario == "drain_interleaving" || scenario == "coalesced_replies") {
    fixture.epoch = VALID_TIME;
    setupNTP();
    reply();
    if (scenario == "coalesced_replies") {
      reply(REPLY_TIME + 60);
      drainOnce(REPLY_TIME);
    } else {
      fixture.replyDuringDrain = true;
      ntpSyncDrainTick();
      assert(fixture.steps.size() == 1 && fixture.steps[0].before == VALID_TIME);
      ntpSyncDrainTick();
      assert(fixture.steps.size() == 2 && fixture.steps[1].before == REPLY_TIME);
      ntpSyncDrainTick();
      assert(fixture.steps.size() == 2);
    }
    assert(getNtpRuntimeStatus().replyCount == 2);
  } else {
    assert(scenario == "dark_timeout" || scenario == "stopped_timeout" || scenario == "late_reply");
    fixture.armClient = scenario != "stopped_timeout";
    assert(!syncNTPAndResolve(&outcome) && outcome == NtpSyncOutcome::Failed);
    assert(fixture.delayCalls == 150 && !fixture.rtcCalls && !fixture.anchorCalls);
    auto status = getNtpRuntimeStatus();
    assert(status.replyCount == 0 && status.foregroundTimeoutCount == 1);
    assert(status.clientArmed == fixture.armClient && status.awaitingLateReply == fixture.armClient);
    if (scenario == "stopped_timeout") assert(messagesContaining("client is stopped") == 1);
    if (scenario == "late_reply") {
      reply();
      fixture.nowUs += 1234000;
      status = getNtpRuntimeStatus();
      assert(status.replyCount == 1 && status.lateReplyCount == 1 && !status.awaitingLateReply);
      assert(status.lastReplyEpoch == REPLY_TIME && status.lastReplyAgeMs == 1234);
      assert(messagesContaining("reply received after foreground timeout") == 0);
      drainOnce(0);
      assert(messagesContaining("reply received after foreground timeout") == 1);
      reply(REPLY_TIME + 60);
      ntpSyncDrainTick();
      assert(getNtpRuntimeStatus().lateReplyCount == 1 && fixture.steps.size() == 2);
      assert(messagesContaining("reply received after foreground timeout") == 1);
    } else {
      ntpSyncDrainTick();
      assert(fixture.steps.empty());
    }
  }
  assert(fixture.criticalDepth == 0);
}
