// Host harness for the credential attempt table (System_User.cpp).
// test_credential_attempts.py inserts the production table unchanged, or
// with --baseline-web the old per-IP web lockout behind an adapter, and the
// scenarios below run against whichever was inserted. Only the clock, the
// critical section, diagnostics and the event register are replaced.
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// INSERT_TYPES

#define EXT_RAM_BSS_ATTR
struct portMUX_TYPE {
  int owner;
};
#define portMUX_INITIALIZER_UNLOCKED {0}
static int gCriticalDepth = 0;
#define portENTER_CRITICAL(mux) \
  do {                          \
    (void)(mux);                \
    ++gCriticalDepth;           \
  } while (0)
#define portEXIT_CRITICAL(mux) \
  do {                         \
    (void)(mux);               \
    --gCriticalDepth;          \
  } while (0)

// One host clock in milliseconds. The table reads it through
// esp_timer_get_time() (64-bit microseconds, as on the device); the old web
// functions used by --baseline-web read it through millis().
static uint64_t gNow = 1000;
static int64_t esp_timer_get_time() { return static_cast<int64_t>(gNow * 1000ULL); }
static unsigned long millis() { return static_cast<unsigned long>(gNow); }

static int gChecks = 0;
static int gFailures = 0;
static int gOutputInsideLock = 0;

static void harnessOutput(const char* fmt, ...) {
  if (gCriticalDepth != 0) ++gOutputInsideLock;
  char line[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
}
#define DEBUG_AUTHF(...) harnessOutput(__VA_ARGS__)
#define BROADCAST_PRINTF(...) harnessOutput(__VA_ARGS__)

enum : uint8_t { SYSEVT_LOGIN_LOCKED = 7 };
static int gLockEvents = 0;
static std::string gLastLockSubject;
static std::string gLastLockDetail;
void systemEventPost(uint8_t kind, const char* subject, const char* detail) {
  if (gCriticalDepth != 0) ++gOutputInsideLock;
  if (kind == SYSEVT_LOGIN_LOCKED) {
    ++gLockEvents;
    gLastLockSubject = subject ? subject : "";
    gLastLockDetail = detail ? detail : "";
  }
}

// INSERT_DECLARATIONS

// INSERT_PRODUCTION

// INSERT_ADAPTER

static bool gScenarioOk = true;
static void check(bool condition, const char* scenario, const char* what) {
  ++gChecks;
  if (!condition) {
    gScenarioOk = false;
    std::printf("  FAIL %s: %s\n", scenario, what);
  }
}

static void begin() {
  harnessReset();
  gNow = 1000;
  gLockEvents = 0;
  gScenarioOk = true;
}

static void finish(const char* scenario) {
  if (gScenarioOk) {
    std::printf("ok   %s\n", scenario);
  } else {
    std::printf("FAIL %s\n", scenario);
    ++gFailures;
  }
}

static void failTimes(CommandSource transport, const char* peer,
                      const char* user, int count) {
  for (int i = 0; i < count; ++i) {
    credentialAttemptRecord(transport, peer, user, false);
  }
}

static bool allowed(CommandSource transport, const char* peer,
                    unsigned long* retry = nullptr) {
  return credentialAttemptAllowed(transport, peer, retry);
}

// Tiers 5/10/20 are all reachable by a peer that waits out every lock.
static void scenarioTiers() {
  const char* name = "tiers 5/10/20 reachable when every lock is waited out";
  begin();
  const char* ip = "192.0.2.10";
  bool tier1 = false, tier2 = false, tier3 = false;
  for (int failure = 1; failure <= 20; ++failure) {
    unsigned long retry = 0;
    if (!allowed(SOURCE_WEB, ip, &retry)) gNow += retry;
    check(allowed(SOURCE_WEB, ip), name, "allowed once the lock ended");
    credentialAttemptRecord(SOURCE_WEB, ip, "admin", false);
    retry = 0;
    const bool locked = !allowed(SOURCE_WEB, ip, &retry);
    if (failure < 5) check(!locked, name, "no lock below five failures");
    if (failure == 5) tier1 = locked && retry == LOGIN_LOCKOUT_TIER1_MS;
    if (failure == 10) tier2 = locked && retry == LOGIN_LOCKOUT_TIER2_MS;
    if (failure == 20) tier3 = locked && retry == LOGIN_LOCKOUT_TIER3_MS;
  }
  check(tier1, name, "five failures lock for 30 s");
  check(tier2, name, "ten failures lock for 5 min");
  check(tier3, name, "twenty failures lock for 30 min");
  check(credentialAttemptFailures(SOURCE_WEB, ip) == 20, name,
        "the entry counts all twenty failures");
  finish(name);
}

// Keys are transport plus peer; one peer's lock leaves the others alone.
static void scenarioIsolation() {
  const char* name = "per-key isolation (peer and transport)";
  begin();
  failTimes(SOURCE_WEB, "192.0.2.1", "admin", 5);
  check(!allowed(SOURCE_WEB, "192.0.2.1"), name, "the guessing IP is locked");
  check(allowed(SOURCE_WEB, "192.0.2.2"), name, "another IP is not");
  check(allowed(SOURCE_SERIAL, "local"), name, "serial is not");
  check(allowed(SOURCE_UART, "uart"), name, "the UART link is not");
  check(allowed(SOURCE_MQTT, "mqtt:broker.example"), name, "MQTT is not");
  check(allowed(SOURCE_MQTT, "192.0.2.1"), name,
        "the same peer text under another transport is a different key");
  failTimes(SOURCE_SERIAL, "local", "admin", 5);
  check(!allowed(SOURCE_SERIAL, "local"), name, "serial locks on its own");
  check(allowed(SOURCE_UART, "uart"), name, "UART stays open");
  finish(name);
}

// A success for one account does not clear failures that named another.
static void scenarioOtherAccountSuccess() {
  const char* name = "success for another account does not clear";
  begin();
  const char* ip = "192.0.2.20";
  failTimes(SOURCE_WEB, ip, "admin", 4);
  credentialAttemptRecord(SOURCE_WEB, ip, "guest", true);
  check(credentialAttemptFailures(SOURCE_WEB, ip) == 4, name,
        "the four admin failures survive a guest login");
  credentialAttemptRecord(SOURCE_WEB, ip, "admin", false);
  check(!allowed(SOURCE_WEB, ip), name, "the fifth admin failure locks");
  finish(name);
}

static void scenarioSameAccountSuccess() {
  const char* name = "success for the same account clears";
  begin();
  const char* ip = "192.0.2.21";
  failTimes(SOURCE_WEB, ip, "alice", 4);
  credentialAttemptRecord(SOURCE_WEB, ip, "alice", true);
  check(credentialAttemptFailures(SOURCE_WEB, ip) == 0, name, "record cleared");
  credentialAttemptRecord(SOURCE_WEB, ip, "alice", false);
  check(allowed(SOURCE_WEB, ip), name, "one new failure does not lock");
  finish(name);
}

static void scenarioMixedAccounts() {
  const char* name = "failures naming several accounts are not cleared";
  begin();
  const char* ip = "192.0.2.22";
  failTimes(SOURCE_WEB, ip, "admin", 2);
  failTimes(SOURCE_WEB, ip, "guest", 2);
  credentialAttemptRecord(SOURCE_WEB, ip, "guest", true);
  check(credentialAttemptFailures(SOURCE_WEB, ip) == 4, name,
        "a guest login keeps the admin failures");
  // A name longer than the stored tag cannot be matched exactly.
  const std::string longName(kPublicUsernameMaxLen + 4, 'x');
  const std::string prefix(kPublicUsernameMaxLen, 'x');
  failTimes(SOURCE_WEB, "192.0.2.23", longName.c_str(), 3);
  credentialAttemptRecord(SOURCE_WEB, "192.0.2.23", prefix.c_str(), true);
  check(credentialAttemptFailures(SOURCE_WEB, "192.0.2.23") == 3, name,
        "a prefix of an over-long name does not clear it");
  finish(name);
}

// The window ending must not cancel a running lock.
static void scenarioWindowDuringLock() {
  const char* name = "window expiry during a lock stays locked";
  begin();
  const char* ip = "192.0.2.30";
  failTimes(SOURCE_WEB, ip, "admin", 4);
  gNow += LOGIN_ATTEMPT_WINDOW_MS - 5000UL;
  credentialAttemptRecord(SOURCE_WEB, ip, "admin", false);  // 30 s lock
  gNow += 10000UL;  // window over, lock still running
  unsigned long retry = 0;
  check(!allowed(SOURCE_WEB, ip, &retry), name, "still locked after the window");
  check(retry == 20000UL, name, "the lock keeps its own end time");
  gNow += retry;
  check(allowed(SOURCE_WEB, ip), name, "open when the lock ends");
  check(credentialAttemptFailures(SOURCE_WEB, ip) == 5, name,
        "the count survives into the next window");
  credentialAttemptRecord(SOURCE_WEB, ip, "admin", false);
  check(!allowed(SOURCE_WEB, ip), name, "the next failure locks again");
  finish(name);
}

static void scenarioExpiry() {
  const char* name = "unlocked entries expire after the window";
  begin();
  const char* ip = "192.0.2.31";
  failTimes(SOURCE_WEB, ip, "admin", 4);
  gNow += LOGIN_ATTEMPT_WINDOW_MS + 1UL;
  check(credentialAttemptFailures(SOURCE_WEB, ip) == 0, name, "count expired");
  credentialAttemptRecord(SOURCE_WEB, ip, "admin", false);
  check(allowed(SOURCE_WEB, ip), name, "a fresh failure does not lock");
  // After a lock the entry is kept for one window from the lock's end.
  failTimes(SOURCE_SERIAL, "local", "admin", 5);
  gNow += LOGIN_LOCKOUT_TIER1_MS + LOGIN_ATTEMPT_WINDOW_MS - 1000UL;
  check(credentialAttemptFailures(SOURCE_SERIAL, "local") == 5, name,
        "kept for a window after the lock ended");
  gNow += 2000UL;
  check(credentialAttemptFailures(SOURCE_SERIAL, "local") == 0, name,
        "then expires");
  finish(name);
}

// A locked entry is never evicted by new peers.
static void scenarioNoEvictionOfLocked() {
  const char* name = "no eviction of a locked entry";
  begin();
  failTimes(SOURCE_WEB, "192.0.2.40", "admin", 5);
  for (size_t i = 0; i < kHarnessPoolSlots + 4; ++i) {
    char peer[32];
    std::snprintf(peer, sizeof(peer), "198.51.100.%u", static_cast<unsigned>(i));
    gNow += 100UL;
    credentialAttemptRecord(SOURCE_WEB, peer, "admin", false);
  }
  check(!allowed(SOURCE_WEB, "192.0.2.40"), name,
        "the locked IP is still locked after the pool churned");
  finish(name);
}

// When every pool entry is locked, a new peer is refused (fail closed) but
// the console transports and MQTT keep their reserved slots.
static void scenarioFailClosed() {
  const char* name = "full pool of locks refuses new peers, consoles and MQTT stay open";
  begin();
  for (size_t i = 0; i < kHarnessPoolSlots; ++i) {
    char peer[32];
    std::snprintf(peer, sizeof(peer), "203.0.113.%u", static_cast<unsigned>(i));
    failTimes(SOURCE_WEB, peer, "admin", 5);
  }
  unsigned long retry = 0;
  check(!allowed(SOURCE_WEB, "203.0.113.200", &retry), name,
        "a new IP is refused while every slot is locked");
  check(retry > 0 && retry <= LOGIN_LOCKOUT_TIER1_MS, name,
        "the refusal reports the earliest lock end");
  check(allowed(SOURCE_SERIAL, "local"), name, "serial still has its slot");
  check(allowed(SOURCE_MQTT, "mqtt:broker.example"), name,
        "web locks do not reach MQTT logins");
  failTimes(SOURCE_MQTT, "mqtt:broker.example", "admin", 4);
  check(credentialAttemptFailures(SOURCE_MQTT, "mqtt:broker.example") == 4, name,
        "MQTT counts in its own slot");
  credentialAttemptRecord(SOURCE_MQTT, "mqtt:broker.example", "admin", true);
  check(allowed(SOURCE_MQTT, "mqtt:broker.example") &&
            credentialAttemptFailures(SOURCE_MQTT, "mqtt:broker.example") == 0,
        name, "and a correct MQTT login succeeds and clears it");
  failTimes(SOURCE_SERIAL, "local", "admin", 5);
  check(!allowed(SOURCE_SERIAL, "local"), name, "and counts in it");
  gNow += LOGIN_LOCKOUT_TIER1_MS;
  check(allowed(SOURCE_WEB, "203.0.113.200"), name,
        "a new IP is admitted once a lock ends");
  finish(name);
}

static void scenarioEventsAndTotals() {
  const char* name = "lock event, totals and lock discipline";
  begin();
  const uint32_t before = getTotalFailedLoginCount();
  failTimes(SOURCE_MQTT, "mqtt:broker.example", "admin", 5);
  check(gLockEvents == 1, name, "one SYSEVT_LOGIN_LOCKED when the lock is set");
  check(gLastLockSubject == "mqtt:broker.example", name, "subject is the peer");
  check(gLastLockDetail == "30s", name, "detail is the lock length");
  credentialAttemptRecord(SOURCE_MQTT, "mqtt:broker.example", "admin", true);
  check(getTotalFailedLoginCount() - before == 5, name,
        "the since-boot total counts failures and is not reset by a success");
  check(gOutputInsideLock == 0, name, "no diagnostics or events inside the lock");
  check(gCriticalDepth == 0, name, "every critical section is closed");
  check(allowed(SOURCE_WEB, nullptr) && allowed(SOURCE_WEB, ""), name,
        "a missing peer is handled");
  credentialAttemptRecord(SOURCE_WEB, nullptr, nullptr, false);
  credentialAttemptForget(SOURCE_WEB, "unknown");
  finish(name);
}

// The device's millis() is 32 bits wide: a lock that spans its wrap still
// runs, and an ended lock does not come back weeks later (a signed 32-bit
// difference against a stored end time flips after ~24.8 days).
static void scenarioLongIdle() {
  const char* name = "locks across the 32-bit millis() wrap and long idle";
  begin();
  const uint64_t kDay = 24ULL * 3600ULL * 1000ULL;
  gNow = 0xFFFFFFFFULL - 10000ULL;
  failTimes(SOURCE_UART, "uart", "admin", 5);
  gNow += 20000UL;  // past the 32-bit wrap
  unsigned long retry = 0;
  check(!allowed(SOURCE_UART, "uart", &retry), name, "locked across the wrap");
  check(retry == 10000UL, name, "remaining time is wrap-safe");
  gNow += retry;
  check(allowed(SOURCE_UART, "uart"), name, "open after the lock");

  begin();
  failTimes(SOURCE_SERIAL, "local", "admin", 5);
  failTimes(SOURCE_MQTT, "mqtt:broker.example", "admin", 5);
  for (size_t i = 0; i < kHarnessPoolSlots; ++i) {
    char peer[32];
    std::snprintf(peer, sizeof(peer), "203.0.113.%u", static_cast<unsigned>(i));
    failTimes(SOURCE_WEB, peer, "admin", 5);
  }
  gNow += LOGIN_LOCKOUT_TIER1_MS + LOGIN_ATTEMPT_WINDOW_MS + 1000UL;
  check(allowed(SOURCE_SERIAL, "local"), name, "serial open after lock and window");
  for (int step = 1; step <= 3; ++step) {  // 25, 50 and 75 days idle
    gNow += 25ULL * kDay;
    retry = 0;
    check(allowed(SOURCE_SERIAL, "local", &retry) && retry == 0, name,
          "serial stays open with no activity for weeks");
    check(credentialAttemptFailures(SOURCE_SERIAL, "local") == 0, name,
          "the ended serial entry does not come back");
    check(allowed(SOURCE_MQTT, "mqtt:broker.example"), name,
          "MQTT stays open with no activity for weeks");
    check(allowed(SOURCE_WEB, "198.51.100.1"), name,
          "stale pool locks do not refuse a new IP weeks later");
  }
  credentialAttemptRecord(SOURCE_SERIAL, "local", "admin", false);
  check(credentialAttemptFailures(SOURCE_SERIAL, "local") == 1, name,
        "a new failure starts a fresh count");
  finish(name);
}

static void scenarioForget() {
  const char* name = "forget drops only that peer";
  begin();
  failTimes(SOURCE_WEB, "192.0.2.50", "admin", 5);
  failTimes(SOURCE_WEB, "192.0.2.51", "admin", 5);
  credentialAttemptForget(SOURCE_WEB, "192.0.2.50");
  check(allowed(SOURCE_WEB, "192.0.2.50"), name, "forgotten peer is open");
  check(!allowed(SOURCE_WEB, "192.0.2.51"), name, "other peer stays locked");
  finish(name);
}

int main() {
  scenarioTiers();
  scenarioIsolation();
  scenarioOtherAccountSuccess();
  scenarioSameAccountSuccess();
  scenarioMixedAccounts();
  scenarioWindowDuringLock();
  scenarioExpiry();
  scenarioNoEvictionOfLocked();
  scenarioFailClosed();
  scenarioEventsAndTotals();
  scenarioLongIdle();
  scenarioForget();
  std::fprintf(stderr, "checks=%d\n", gChecks);
  std::printf("%d scenario(s) failed\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
