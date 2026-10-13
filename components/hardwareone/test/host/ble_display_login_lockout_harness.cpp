// Host harness for the Bluetooth and display login lockout (C8/D3).
// test_ble_display_login_lockout.py inserts, unchanged, the credential attempt
// table and loginTransport from System_User.cpp, the System_User.h table
// declarations, and BleLoginAsyncJob, bleLoginJobFree and bleLoginDeferred
// from Bluetooth.cpp. Everything they reach outside themselves is a double
// here: the clock, the critical section, the filesystem lock, the password
// check, the BLE binder and sender, the console session publishers, the audit
// line and the event register.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// INSERT_TYPES

using TransportSessionEpoch = uint32_t;

class String {
 public:
  String() = default;
  String(const char* value) : text_(value ? value : "") {}
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.size(); }
  bool operator==(const String& other) const { return text_ == other.text_; }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  bool operator!=(const String& other) const { return !(*this == other); }

 private:
  std::string text_;
};

// ---------------------------------------------------------------------------
// Table doubles: clock, critical section, diagnostics, events.
// ---------------------------------------------------------------------------
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
static uint64_t gNow = 1000;  // milliseconds
static int64_t esp_timer_get_time() { return static_cast<int64_t>(gNow * 1000ULL); }
#define DEBUG_AUTHF(...) ((void)0)
#define BROADCAST_PRINTF(...) ((void)0)
#define BLE_DEBUGF(...) ((void)0)
#define DEBUG_BLE_CORE 0

enum : uint8_t { SYSEVT_LOGIN_OK = 1, SYSEVT_LOGIN_FAIL, SYSEVT_LOGIN_LOCKED };
struct Event {
  int kind;
  std::string subject;
  std::string detail;
};
static std::vector<Event> gEvents;
void systemEventPost(uint8_t kind, const char* subject, const char* detail) {
  gEvents.push_back({kind, subject ? subject : "", detail ? detail : ""});
}

// INSERT_DECLARATIONS

// INSERT_TABLE

// ---------------------------------------------------------------------------
// Credential store, audit and sessions.
// ---------------------------------------------------------------------------
static bool gStoreAvailable = true;  // the filesystem lock can be taken
struct FsLockGuard {
  bool held;
  explicit FsLockGuard(const char*) : held(gStoreAvailable) {}
};
static bool isFsLockedByCurrentTask() { return false; }

static int gPasswordChecks = 0;
static bool passwordMatches(const String& user, const String& password) {
  ++gPasswordChecks;
  return user == "alice" && password == "right-pass";
}
bool validateTransportCredentials(CommandSource, const String& user, const String& password) {
  return passwordMatches(user, password);
}
bool isValidUser(const String& user, const String& password) {
  return passwordMatches(user, password);
}

struct Audit {
  CommandSource transport;
  std::string user;
  bool success;
  std::string reason;
};
static std::vector<Audit> gAudit;
void recordTransportLoginResult(CommandSource transport, const String& user, bool success,
                                const char* reason) {
  gAudit.push_back({transport, user.c_str(), success, reason ? reason : ""});
}
void recordLoginAttempt(CommandSource transport, const String& user, const String&,
                        bool success, const char* reason) {
  gAudit.push_back({transport, user.c_str(), success, reason ? reason : ""});
}
void updateUserLastSeen(const String&) {}

// The local display publishes the session unless a test refuses it.
static bool gDisplayPublishes = true;
static std::string gDisplayUser;
void localDisplayTransportSessionAuthenticated(const String& user) {
  if (gDisplayPublishes) gDisplayUser = user.c_str();
}
TransportSessionEpoch localDisplayTransportSessionSnapshot(String& userOut, bool& authed) {
  userOut = String(gDisplayUser.c_str());
  authed = !gDisplayUser.empty();
  return authed ? 1 : 0;
}
TransportSessionEpoch serialTransportSessionAuthenticated(const String&) { return 1; }
TransportSessionEpoch serialTransportSessionSnapshot(String& userOut, bool& authed) {
  userOut = String("");
  authed = false;
  return 0;
}
bool uartLinkIsRunning() { return false; }
uint32_t publishUartAccountSession(const String&, uint32_t* = nullptr) { return 0; }
bool uartLinkSessionSnapshot(char* userOut, size_t size, uint32_t* = nullptr,
                             uint32_t* = nullptr, bool* = nullptr) {
  if (size) userOut[0] = '\0';
  return false;
}

// INSERT_LOGIN_TRANSPORT

// ---------------------------------------------------------------------------
// Bluetooth doubles: the session fence, the binder and the directed sender.
// ---------------------------------------------------------------------------
static bool gEpochMatches = true;
bool bleSessionEpochMatches(uint16_t, TransportSessionEpoch) { return gEpochMatches; }
static bool gBindOk = true;
static int gBinds = 0;
static bool bleBindSession(uint16_t, TransportSessionEpoch expectedEpoch, const String&, bool,
                           TransportSessionEpoch& boundEpoch) {
  ++gBinds;
  if (!gBindOk) return false;
  boundEpoch = expectedEpoch + 100;
  return true;
}
bool userMayControlOtherSessions(const String&) { return false; }
bool isAdminUser(const String&) { return false; }
struct Reply {
  uint16_t connId;
  TransportSessionEpoch epoch;
  std::string text;
};
static std::vector<Reply> gReplies;
bool sendBLEResponseToSession(uint16_t connId, TransportSessionEpoch epoch, const char* data,
                              size_t len, bool = true) {
  gReplies.push_back({connId, epoch, std::string(data, len)});
  return true;
}

// INSERT_BLE_JOB

// Every job release goes through this: the password must already be wiped.
static int gJobsFreed = 0;
static int gJobsFreedUnwiped = 0;
static void harnessFree(void* pointer) {
  if (!pointer) return;
  const BleLoginAsyncJob* job = static_cast<const BleLoginAsyncJob*>(pointer);
  for (size_t i = 0; i < sizeof(job->password); ++i) {
    if (job->password[i] != 0) {
      ++gJobsFreedUnwiped;
      break;
    }
  }
  ++gJobsFreed;
  std::free(pointer);
}
#define free harnessFree
// INSERT_BLE_LOGIN
#undef free

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------
static int gChecks = 0;
static int gFailures = 0;
static bool gScenarioOk = true;
static void check(bool condition, const char* scenario, const char* what) {
  ++gChecks;
  if (!condition) {
    gScenarioOk = false;
    std::printf("  FAIL %s: %s\n", scenario, what);
  }
}

static int gJobs = 0;
static void begin() {
  std::memset(credential_attempts::sEntries, 0, sizeof(credential_attempts::sEntries));
  credential_attempts::sTotalFailures = 0;
  gNow = 1000;
  gEvents.clear();
  gAudit.clear();
  gReplies.clear();
  gPasswordChecks = 0;
  gBinds = 0;
  gJobs = gJobsFreed = gJobsFreedUnwiped = 0;
  gStoreAvailable = gEpochMatches = gBindOk = gDisplayPublishes = true;
  gDisplayUser.clear();
  gScenarioOk = true;
}

static void finish(const char* scenario) {
  check(gJobsFreed == gJobs, scenario, "every BLE login job is freed once");
  check(gJobsFreedUnwiped == 0, scenario, "every BLE login job's password is wiped before free");
  check(gCriticalDepth == 0, scenario, "every critical section is closed");
  if (gScenarioOk) {
    std::printf("ok   %s\n", scenario);
  } else {
    std::printf("FAIL %s\n", scenario);
    ++gFailures;
  }
}

static void bleLogin(uint16_t connId, TransportSessionEpoch epoch, const char* user,
                     const char* password) {
  auto* job = static_cast<BleLoginAsyncJob*>(std::calloc(1, sizeof(BleLoginAsyncJob)));
  job->connId = connId;
  job->sessionEpoch = epoch;
  std::snprintf(job->user, sizeof(job->user), "%s", user);
  std::snprintf(job->password, sizeof(job->password), "%s", password);
  ++gJobs;
  bleLoginDeferred(job);
}

static int countEvents(int kind, const char* detail) {
  int count = 0;
  for (const Event& e : gEvents) {
    if (e.kind == kind && (!detail || e.detail == detail)) ++count;
  }
  return count;
}

static bool lastReplyIs(uint16_t connId, TransportSessionEpoch epoch, const char* text) {
  return !gReplies.empty() && gReplies.back().connId == connId &&
         gReplies.back().epoch == epoch && gReplies.back().text == text;
}

static bool displayLogin(const char* user, const char* password, unsigned long* retryMs) {
  *retryMs = 0;
#if HAS_LOCKOUT_OUT
  return loginTransport(SOURCE_LOCAL_DISPLAY, String(user), String(password), retryMs);
#else
  return loginTransport(SOURCE_LOCAL_DISPLAY, String(user), String(password));
#endif
}

static void scenarioBleSixthRefused() {
  const char* name = "BLE: the sixth login is refused before the password check";
  begin();
  for (int i = 0; i < 5; ++i) {
    bleLogin(7, 70, "alice", "wrong");
    check(lastReplyIs(7, 70, "[ble] Authentication failed."), name,
          "a wrong password answers the requesting session");
  }
  check(gPasswordChecks == 5, name, "five passwords were checked");
  check(countEvents(SYSEVT_LOGIN_FAIL, "ble") == 5, name, "each failure posts SYSEVT_LOGIN_FAIL");
  check(countEvents(SYSEVT_LOGIN_LOCKED, nullptr) == 1, name, "the fifth sets the lock");
  bleLogin(7, 70, "alice", "right-pass");
  check(gPasswordChecks == 5, name, "the locked attempt checks no password");
  check(gBinds == 0, name, "and binds no session");
  check(lastReplyIs(7, 70, "[ble] Login locked out. Retry in 30 seconds."), name,
        "the lockout text goes to the requesting session only");
  check(!gAudit.empty() && gAudit.back().reason == "Locked out" && !gAudit.back().success,
        name, "the refusal is audited as Locked out");
  gNow += LOGIN_LOCKOUT_TIER1_MS;
  bleLogin(7, 70, "alice", "right-pass");
  check(gBinds == 1 && lastReplyIs(7, 170, "[ble] Login successful. User: alice"), name,
        "the right password binds once the lock ends");
  finish(name);
}

static void scenarioBleSharedKey() {
  const char* name = "BLE: every connection counts under Bluetooth's one key";
  begin();
  for (uint16_t conn = 1; conn <= 5; ++conn) bleLogin(conn, 10 + conn, "alice", "wrong");
  bleLogin(9, 99, "alice", "right-pass");
  check(gPasswordChecks == 5 && gBinds == 0, name,
        "a new connection (or a rotated address) is still locked");
  check(lastReplyIs(9, 99, "[ble] Login locked out. Retry in 30 seconds."), name,
        "and is told so on its own session");
  check(credentialAttemptFailures(SOURCE_BLUETOOTH, "ble") == 5, name,
        "one entry counts all five");
  check(credentialAttemptAllowed(SOURCE_SERIAL, "local") &&
            credentialAttemptAllowed(SOURCE_LOCAL_DISPLAY, "display"),
        name, "the consoles stay open");
  finish(name);
}

static void scenarioBleNotGuesses() {
  const char* name = "BLE: an unavailable store or a lost bind is not a guess";
  begin();
  gStoreAvailable = false;
  for (int i = 0; i < 6; ++i) bleLogin(3, 30, "alice", "wrong");
  check(gPasswordChecks == 0, name, "no password is checked without the store");
  check(!gAudit.empty() && gAudit.back().reason == "Authentication store unavailable", name,
        "the audit names the unavailable store");
  check(countEvents(SYSEVT_LOGIN_FAIL, nullptr) == 0, name, "and posts no login failure");
  gStoreAvailable = true;
  gBindOk = false;
  for (int i = 0; i < 6; ++i) bleLogin(3, 30, "alice", "right-pass");
  check(gPasswordChecks == 6 && gBinds == 6, name,
        "a right password whose connection changed is never refused");
  check(!gAudit.empty() && gAudit.back().reason == "Connection changed before session bind",
        name, "the audit names the lost bind");
  gBindOk = true;
  bleLogin(3, 30, "alice", "right-pass");
  check(lastReplyIs(3, 130, "[ble] Login successful. User: alice"), name,
        "after twelve non-guesses a login still binds");
  check(countEvents(SYSEVT_LOGIN_OK, "ble") == 1, name, "and posts SYSEVT_LOGIN_OK");
  finish(name);
}

static void scenarioBleSuccessClears() {
  const char* name = "BLE: a bound login clears its account's failures";
  begin();
  for (int i = 0; i < 4; ++i) bleLogin(4, 40, "alice", "wrong");
  bleLogin(4, 40, "alice", "right-pass");
  check(gBinds == 1, name, "four failures do not lock");
  for (int i = 0; i < 4; ++i) bleLogin(4, 40, "alice", "wrong");
  bleLogin(4, 40, "alice", "right-pass");
  check(gBinds == 2, name, "the success cleared the earlier four");
  check(countEvents(SYSEVT_LOGIN_OK, "ble") == 2 && countEvents(SYSEVT_LOGIN_FAIL, "ble") == 8,
        name, "login events name the ble transport");
  finish(name);
}

static void scenarioBleStaleEpoch() {
  const char* name = "BLE: a stale session epoch touches nothing";
  begin();
  gEpochMatches = false;
  for (int i = 0; i < 6; ++i) bleLogin(5, 50, "alice", "wrong");
  check(gPasswordChecks == 0 && gReplies.empty() && gAudit.empty() && gEvents.empty(), name,
        "no check, reply, audit line or event");
  gEpochMatches = true;
  bleLogin(5, 50, "alice", "right-pass");
  check(gBinds == 1, name, "and nothing was counted");
  finish(name);
}

static void scenarioDisplaySixthRefused() {
  const char* name = "display: the sixth login is refused before the password check";
  begin();
  unsigned long retry = 0;
  for (int i = 0; i < 5; ++i) {
    check(!displayLogin("alice", "wrong", &retry) && retry == 0, name,
          "a wrong password fails without a lockout time");
  }
  check(gPasswordChecks == 5, name, "five passwords were checked");
  check(!displayLogin("alice", "right-pass", &retry), name, "the sixth login is refused");
  check(gPasswordChecks == 5, name, "without checking the password");
  check(retry == LOGIN_LOCKOUT_TIER1_MS, name, "loginTransport reports the time left");
  check(gDisplayUser.empty(), name, "no display session is published");
  check(!gAudit.empty() && gAudit.back().reason == "Locked out", name,
        "the refusal is audited as Locked out");
  check(countEvents(SYSEVT_LOGIN_LOCKED, nullptr) == 1 && gEvents.back().subject == "display",
        name, "the lock event names the display key");
  bleLogin(6, 60, "alice", "right-pass");
  check(gBinds == 1, name, "a display lock leaves Bluetooth open");
  gNow += retry;
  check(displayLogin("alice", "right-pass", &retry) && gDisplayUser == "alice", name,
        "the right password publishes once the lock ends");
  check(credentialAttemptFailures(SOURCE_LOCAL_DISPLAY, "display") == 0, name,
        "and clears the account's failures");
  finish(name);
}

static void scenarioDisplayNotGuesses() {
  const char* name = "display: an unpublished login or an unavailable store is not a guess";
  begin();
  unsigned long retry = 0;
  gDisplayPublishes = false;
  for (int i = 0; i < 6; ++i) check(!displayLogin("alice", "right-pass", &retry), name,
                                    "an unpublished login fails");
  check(gPasswordChecks == 6, name, "every attempt was checked, none refused");
  check(!gAudit.empty() && gAudit.back().reason == "Session publication failed", name,
        "the audit names the publication failure");
  gStoreAvailable = false;
  for (int i = 0; i < 6; ++i) displayLogin("alice", "wrong", &retry);
  check(gPasswordChecks == 6, name, "no password is checked without the store");
  gStoreAvailable = gDisplayPublishes = true;
  check(displayLogin("alice", "right-pass", &retry) && retry == 0, name,
        "after twelve non-guesses the display login still publishes");
  finish(name);
}

int main() {
  scenarioBleSixthRefused();
  scenarioBleSharedKey();
  scenarioBleNotGuesses();
  scenarioBleSuccessClears();
  scenarioBleStaleEpoch();
  scenarioDisplaySixthRefused();
  scenarioDisplayNotGuesses();
  std::fprintf(stderr, "checks=%d\n", gChecks);
  std::printf("%d scenario(s) failed\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
