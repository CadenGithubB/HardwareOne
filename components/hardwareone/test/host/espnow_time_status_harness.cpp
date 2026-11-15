// Host harness for the mesh time-sync stamp behind `espnowtimestatus`.
//
// test_espnow_time_status.py inserts, from the production sources, the time
// sync state of System_ESPNow.cpp, V4PayloadTimeSync (System_ESPNow_Wire.h),
// V4RxCtx (System_ESPNow_RxCtx.h), the TIME_SYNC handler v4h_time_sync and
// cmd_espnow_timestatus. Host boundaries: String, the 32-bit millis() clock
// (unsigned long is 64 bits here, so the double returns uint32_t as the device
// does), the reply buffer and the log macros.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
 private:
  std::string text_;
};

static uint32_t gNowMs = 0;
static uint32_t millis() { return gNowMs; }

#define DEBUG_ESPNOW_MESHF(...) ((void)0)
#define WARN_ESPNOWF(...) ((void)0)
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)argsInput)  // validate-only mode is off here
[[maybe_unused]] static const char kCommandReplyOutOfMemory[] = "Error: Out of memory";
static char gReply[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gReply; }

struct esp_now_recv_info;
struct EspNowV4Header;

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

static void deliver(int64_t timeOffset, uint16_t payloadLen = sizeof(V4PayloadTimeSync)) {
  V4PayloadTimeSync payload{};
  payload.epochTime = 1763000000u;
  payload.timeOffset = timeOffset;
  V4RxCtx ctx{};
  ctx.payload = reinterpret_cast<const uint8_t*>(&payload);
  ctx.payloadLen = payloadLen;
  ctx.isAuthenticated = true;
  ctx.deviceName = "master";
  v4h_time_sync(ctx);
}

// "Last sync: N seconds ago" from the reply, or -1 when the line is absent.
static long lastSyncSeconds() {
  const std::string reply = cmd_espnow_timestatus(String(""));
  const char kLine[] = "Last sync: ";
  const size_t at = reply.find(kLine);
  if (at == std::string::npos) return -1;
  unsigned long seconds = 0;
  if (std::sscanf(reply.c_str() + at + sizeof(kLine) - 1, "%lu seconds ago", &seconds) != 1) return -2;
  return static_cast<long>(seconds);
}

int main() {
  const int64_t kOffset = 1763000000000LL;

  // No sync yet: the reply says so and names no sync time.
  gNowMs = 90000;
  check(lastSyncSeconds() == -1, "no 'Last sync' line before a sync was applied");
  check(std::string(gReply).find("Synced: No") != std::string::npos, "reply says not synced");

  // A payload that is too short applies nothing.
  deliver(kOffset, sizeof(V4PayloadTimeSync) - 1);
  check(lastSyncSeconds() == -1, "a short TIME_SYNC payload does not count as a sync");

  // The device had been up 100 s when the sync was applied; 60 s later the
  // reply says 60, not the 160 s of uptime.
  gNowMs = 100000;
  deliver(kOffset);
  check(std::string(cmd_espnow_timestatus(String(""))).find("Synced: Yes") != std::string::npos,
        "reply says synced after a TIME_SYNC");
  check(lastSyncSeconds() == 0, "0 s right after the sync");
  gNowMs = 160000;
  check(lastSyncSeconds() == 60, "60 s after the sync the reply says 60 (uptime is 160 s)");

  // A later sync moves the stamp.
  gNowMs = 400000;
  deliver(kOffset + 250);
  gNowMs = 403000;
  check(lastSyncSeconds() == 3, "a second sync restarts the count");

  // A correction of more than an hour is refused once synced: the stamp of
  // the last applied sync stays.
  gNowMs = 500000;
  deliver(kOffset + 250 + 2LL * 3600 * 1000);
  gNowMs = 503000;
  check(lastSyncSeconds() == 103, "a refused TIME_SYNC does not move the stamp");
  check(gTimeOffset == kOffset + 250, "a refused TIME_SYNC does not move the offset");

  // millis() wraps at 2^32: 4096 ms before the wrap to 3000 ms after it.
  gNowMs = 0xFFFFF000u;
  deliver(kOffset + 500);
  gNowMs = 3000;
  check(lastSyncSeconds() == 7, "elapsed time is right across the millis() wrap");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d ESP-NOW time status checks failed\n", failures, checks);
    return 1;
  }
  std::printf("ESP-NOW time status: %d checks passed\n", checks);
  return 0;
}
