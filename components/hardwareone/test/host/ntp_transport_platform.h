#pragma once
// Deterministic platform boundaries; the NTP implementation and API are real.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>
#include <vector>

namespace Clock {
enum SyncSource : uint8_t { SYNC_NONE, SYNC_RTC, SYNC_NTP, SYNC_MANUAL, SYNC_RING, SYNC_CM5 };
int tzOffsetMinutes();
bool isValidEpoch(time_t value);
SyncSource syncSource();
const char* syncSourceName(SyncSource source);
int64_t projectedEpochUs();
void refreshProjection();
void applyTimezone();
void clockStepped(SyncSource source, time_t before);
}

struct NtpFixture {
  bool connected = true, armClient = true, armed = false;
  bool inCallback = false, replyOnConfig = false, replyAtDeadline = false;
  bool replyDuringDrain = false, rtcSuccess = false;
  int delayCalls = 0, replyAfterDelay = 0, configCalls = 0, criticalDepth = 0;
  int anchorCalls = 0, resolveCalls = 0, rtcCalls = 0, refreshCalls = 0;
  int timezoneMinutes = -330;
  int64_t nowUs = 1000000, projectionUs = 0;
  time_t epoch = 0;
  Clock::SyncSource priorSource = Clock::SYNC_NONE;
  void (*callback)(timeval*) = nullptr;
  const char* retainedServer = nullptr;
  std::vector<std::string> logs, markers, setupOrder;
  struct Step { Clock::SyncSource source; time_t before; };
  std::vector<Step> steps;
};
extern NtpFixture fixture;
struct FakeSettings { std::string ntpServer = "pool.example.test"; };
extern FakeSettings gSettings;
struct FakeWiFi { bool isConnected() const { return fixture.connected; } };
extern FakeWiFi WiFi;
#if ENABLE_RTC_SENSOR
extern bool gRtcRunning, gRtcConnected;
#endif

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE*) { assert(fixture.criticalDepth++ == 0); }
inline void portEXIT_CRITICAL(portMUX_TYPE*) { assert(--fixture.criticalDepth == 0); }
#define DEBUG_NTP_SETUPF(...) ((void)0)
#define DEBUG_NTP_SYNCF(...) ((void)0)
#define BROADCAST_PRINTF(...) ntpTestBroadcastPrintf(__VA_ARGS__)
void ntpTestBroadcastPrintf(const char* format, ...);
void broadcastOutput(const char* message);
void oledUpdate();
void delay(unsigned long milliseconds);
int64_t esp_timer_get_time();
bool esp_sntp_enabled();
void sntp_set_time_sync_notification_cb(void (*callback)(timeval*));
void configTime(long offset, int daylight, const char* primary, const char* secondary, const char* tertiary);
#if ENABLE_RTC_SENSOR
bool rtcSyncToSystem();
#endif
size_t ntpTestStrlcpy(char* destination, const char* source, size_t size);
time_t ntpTestTime(time_t* output);
// Headers above are loaded before these local replacements; host wall time and
// libc availability must not affect wait budgets or server-copy checks.
#define time ntpTestTime
#define strlcpy ntpTestStrlcpy
