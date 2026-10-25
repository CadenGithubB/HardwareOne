// BLE idle logout, as shipped: test_ble_session_idle.py inserts Bluetooth.cpp's
// lifecycle guard, slot and session-epoch helpers, bleNewState, the phone-link
// console slot maker, bleBindSession (login), bleMarkActivity (the stamp every
// command and OTA frame makes) and bleSessionTick (the main loop's idle-logout
// pass), with System_User.cpp's shared idle policy. The clock, the lifecycle
// mutex, transport sessions, the directed sender and OTA are host boundaries.
//
// Another task can win the lifecycle lock while the main loop waits for it:
// cmd_exec_task stamps a command (bleMarkActivity) and the main loop's tick
// then reads that slot. The contender double models exactly that interleaving,
// one millisecond later, at each lock take the tick makes.
#include "Arduino.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
using TransportSessionEpoch = uint32_t;

// INSERT_TYPES

// ---------------------------------------------------------------------------
// Host boundaries
// ---------------------------------------------------------------------------
static uint32_t gClockMs = 1000;
static uint32_t millis() { return gClockMs; }

using TickType_t = uint32_t;
using BaseType_t = int;
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1

// The recursive lifecycle mutex. While a contender is armed it runs before
// every outermost take: another task got the lock first and did its work.
struct HostMutex { int depth = 0; };
using SemaphoreHandle_t = HostMutex*;
static HostMutex gLifecycle;
static SemaphoreHandle_t sBleLifecycleMutex = &gLifecycle;
static std::function<void()> gContender;
static BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t) {
  if (mutex->depth == 0 && gContender) {
    std::function<void()> run = std::move(gContender);
    gContender = nullptr;  // the contender's own lock takes do not contend
    run();
    gContender = std::move(run);
  }
  ++mutex->depth;
  return pdTRUE;
}
static BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
  if (mutex->depth <= 0) {
    std::fprintf(stderr, "FAIL: lifecycle mutex given while not held\n");
    std::exit(1);
  }
  --mutex->depth;
  return pdTRUE;
}

enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t bytes, AllocPref, const char*) { return std::malloc(bytes); }

#define DEBUG_BLE_CORE 1
#define DEBUG_BLE_GATT 2
#define DEBUG_BLE_DATA 3
static void hostDebug(int, const char*, ...) {}
#define BLE_DEBUGF(flag, ...) hostDebug(flag, __VA_ARGS__)
constexpr uint32_t MSG_ROUTE_BLE = 0x10;
static volatile uint32_t gOutputFlags = 0xFF;

static struct Settings {
  uint32_t sessionIdleWeb = 0, sessionIdleSerial = 0, sessionIdleBle = 15,
           sessionIdleDisplay = 0, sessionIdleUart = 0;
} gSettings;

// Transport sessions: a fresh epoch per open.
static TransportSessionEpoch gNextEpoch = 100;
static unsigned gSessionCloses = 0;
static TransportSessionEpoch transportSessionOpen(CommandSource) { return gNextEpoch++; }
static void transportSessionClose(CommandSource, TransportSessionEpoch epoch) {
  if (epoch != 0) ++gSessionCloses;
}

static unsigned gScResets = 0;
static void bleScReset(uint16_t) { ++gScResets; }
static bool gOtaUploadActive = false;
static bool otaBleUploadActiveOnConnection(uint16_t) { return gOtaUploadActive; }

// The directed sender: records each line, its fence and whether it may wait. A
// paced send takes time; a hook runs meanwhile (another task works while this
// one sends). gFullSends: that many next offers that may not wait find no room
// (the phone link's console stream full) and are refused.
struct Sent {
  uint16_t connId; TransportSessionEpoch epoch; std::string text; uint32_t atMs;
  bool blocking = true, accepted = true;
};
static std::vector<Sent> gSent;
static std::function<void()> gDuringSend;
static unsigned gFullSends = 0;
bool sendBLEResponseToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking = true) {
  const bool accepted = blocking || gFullSends == 0;
  if (!accepted) --gFullSends;
  gSent.push_back(Sent{connId, expectedEpoch, std::string(data, len), gClockMs, blocking, accepted});
  if (gDuringSend) {
    std::function<void()> run = std::move(gDuringSend);
    gDuringSend = nullptr;
    run();
  }
  return accepted;
}

// A pipe build (the phone link's console row exists), and the leaf lock of the
// row's snapshot (one task here).
#define ENABLE_BLE_PHONE_PIPE 1
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))

BLESystemState* gBLEState = nullptr;

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static int gFailures = 0;
#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                       \
      std::fprintf(stderr, "\n");                              \
      ++gFailures;                                             \
    }                                                          \
  } while (0)

constexpr uint32_t kSecond = 1000;
constexpr uint32_t kMinute = 60 * kSecond;
constexpr uint32_t kWindow = 15 * kMinute;  // sessionIdleBle = 15 (minutes)

static void resetWorld(uint32_t startMs) {
  if (gBLEState) {
    gBLEState->~BLESystemState();
    std::free(gBLEState);
  }
  gBLEState = bleNewState();
  for (auto& epoch : sBleSessionEpoch) epoch.store(0);
  gClockMs = startMs;
  gContender = nullptr;
  gDuringSend = nullptr;
  gSent.clear();
  gOtaUploadActive = false;
  gOutputFlags = 0xFF;
  gFullSends = 0;
  sBlePipeRowSlot = -1;
  sBlePipeIdentity = BlePipeConsoleIdentity{};
}

static int slotOf(uint16_t connId) { return findConnectionSlotByConnIdLocked(connId); }
static const BLEConnection& conn(uint16_t connId) { return gBLEState->connections[slotOf(connId)]; }
static TransportSessionEpoch epochOf(uint16_t connId) { return bleSessionEpochForConnection(connId); }

// Login as the BLE login job binds it: the credential check passed elsewhere.
static void login(uint16_t connId, const char* user) {
  TransportSessionEpoch bound = 0;
  CHECK(bleBindSession(connId, epochOf(connId), String(user), true, bound) && bound != 0,
        "login binds conn %u", (unsigned)connId);
}

// The main loop's tick, with a command from connId landing while it waits for
// the lifecycle lock (cmd_exec_task stamps it one millisecond later each time).
static void tickWhileCommandLands(uint16_t connId) {
  gContender = [connId] {
    gClockMs += 1;
    bleMarkActivity(connId);
  };
  bleSessionTick();
  gContender = nullptr;
}

static unsigned expiriesFor(uint16_t connId) {
  unsigned n = 0;
  for (const Sent& s : gSent) {
    if (s.connId == connId && s.text.find("Session expired") != std::string::npos) ++n;
  }
  return n;
}

// Runs the main loop once a second from now through untilMs (inclusive).
static void tickEverySecondUntil(uint32_t untilMs) {
  while (static_cast<int32_t>(untilMs - gClockMs) >= static_cast<int32_t>(kSecond)) {
    gClockMs += kSecond;
    bleSessionTick();
  }
}

// The app console on the phone pipe's link: the slot is made when the pipe goes
// READY (stamped then), the app logs in 12 minutes later, runs commands at
// +60 s and +120 s, and `phone status` lands at +180 s while the main loop's
// tick waits for the lock. The session stays; it expires only after 15 minutes
// without a command, with the notice on the new (logged-out) session.
static void phoneLinkSessionLivesWhileUsed() {
  resetWorld(5000);
  const uint16_t phone = 3;
  {
    BleLifecycleGuard lifecycle;
    CHECK(bleConsoleMakeSlotLocked(phone), "the phone-link console slot is made");
  }
  tickEverySecondUntil(gClockMs + 12 * kMinute);
  const uint32_t t0 = gClockMs;
  login(phone, "admin");
  CHECK(conn(phone).authed, "logged in");

  tickEverySecondUntil(t0 + 60 * kSecond);
  bleMarkActivity(phone);                       // command at +60 s
  tickEverySecondUntil(t0 + 120 * kSecond);
  bleMarkActivity(phone);                       // command at +120 s
  tickEverySecondUntil(t0 + 180 * kSecond - kSecond);
  gClockMs = t0 + 180 * kSecond;
  tickWhileCommandLands(phone);                 // `phone status` at +180 s
  CHECK(conn(phone).authed && expiriesFor(phone) == 0,
        "a command landing during the tick at +180 s does not expire the session "
        "(idle stamp %u, now %u)", (unsigned)conn(phone).lastActivityMs, (unsigned)gClockMs);

  // More commands landing on later ticks keep it alive too.
  for (int i = 0; i < 5; ++i) {
    gClockMs += 7;
    tickWhileCommandLands(phone);
  }
  CHECK(conn(phone).authed && expiriesFor(phone) == 0,
        "commands landing during later ticks keep the session");

  // Then nothing: it lives through exactly 15 minutes of idle and expires after.
  const uint32_t last = conn(phone).lastActivityMs;
  CHECK(last >= t0 + 180 * kSecond, "the last command stamped the slot");
  tickEverySecondUntil(last + kWindow);
  gClockMs = last + kWindow;
  bleSessionTick();
  CHECK(conn(phone).authed && expiriesFor(phone) == 0,
        "still signed in at exactly 15 minutes idle");
  gClockMs = last + kWindow + 1;
  bleSessionTick();
  CHECK(!conn(phone).authed && conn(phone).user.length() == 0,
        "signed out after 15 minutes idle");
  CHECK(expiriesFor(phone) == 1, "one expiry notice (got %u)", expiriesFor(phone));
  CHECK(!gSent.empty() && gSent.back().text == "[ble] Session expired for user 'admin'" &&
            gSent.back().epoch == epochOf(phone) && gSent.back().epoch != 0,
        "the notice goes to the link's new logged-out session");
  CHECK(conn(phone).active, "the link and its slot stay");
  CHECK((gOutputFlags & MSG_ROUTE_BLE) == 0, "no signed-in BLE session is left for output");
  bleSessionTick();
  CHECK(expiriesFor(phone) == 1, "an expired session is not expired twice");
}

// Without a command after login the session ends 15 minutes after login, not
// 15 minutes after the slot was made.
static void idleAfterLoginExpiresFromLogin() {
  resetWorld(9000);
  const uint16_t phone = 3;
  {
    BleLifecycleGuard lifecycle;
    CHECK(bleConsoleMakeSlotLocked(phone), "the phone-link console slot is made");
  }
  tickEverySecondUntil(gClockMs + 12 * kMinute);
  const uint32_t t0 = gClockMs;
  login(phone, "admin");
  tickEverySecondUntil(t0 + kWindow);
  CHECK(conn(phone).authed && expiriesFor(phone) == 0, "signed in through 15 minutes after login");
  gClockMs = t0 + kWindow + 1;
  bleSessionTick();
  CHECK(!conn(phone).authed && expiriesFor(phone) == 1, "signed out 15 minutes after login");
}

// Server mode: two app sessions in the same loop. A is idle and expires; its
// notice is a paced send, during which B's command lands. B stays signed in.
static void serverModeSendDoesNotExpireTheNextSession() {
  resetWorld(20000);
  uint16_t a = 0, b = 1;
  {
    BleLifecycleGuard lifecycle;
    CHECK(bleConsoleMakeSlotLocked(a) && bleConsoleMakeSlotLocked(b), "two app links");
  }
  CHECK(slotOf(a) < slotOf(b), "A's slot is checked before B's");
  login(a, "alice");
  login(b, "bob");
  const uint32_t t0 = gClockMs;
  for (uint32_t t = t0 + 30 * kSecond; t <= t0 + kWindow; t += 30 * kSecond) {
    tickEverySecondUntil(t);
    bleMarkActivity(b);
  }
  gClockMs = t0 + kWindow + 1;
  gDuringSend = [b] {
    gClockMs += 25;            // the paced notice to A
    bleMarkActivity(b);        // B's command lands meanwhile
  };
  bleSessionTick();
  CHECK(!conn(a).authed && expiriesFor(a) == 1, "the idle session A expires");
  CHECK(conn(b).authed && expiriesFor(b) == 0,
        "the active session B is kept (idle stamp %u, now %u)",
        (unsigned)conn(b).lastActivityMs, (unsigned)gClockMs);
  CHECK((gOutputFlags & MSG_ROUTE_BLE) != 0, "B still receives BLE output");
}

// A bulk OTA upload in flight on the session outranks the idle window.
static void otaUploadInFlightOutranksIdle() {
  resetWorld(30000);
  const uint16_t phone = 3;
  {
    BleLifecycleGuard lifecycle;
    CHECK(bleConsoleMakeSlotLocked(phone), "the phone-link console slot is made");
  }
  login(phone, "admin");
  gOtaUploadActive = true;
  gClockMs += kWindow + 5 * kSecond;
  bleSessionTick();
  CHECK(conn(phone).authed && expiriesFor(phone) == 0, "an upload in flight keeps the session");
  gOtaUploadActive = false;
  bleSessionTick();
  CHECK(!conn(phone).authed && expiriesFor(phone) == 1, "once it ends the idle window applies");
}

// sessionIdleBle = 0 disables the BLE idle logout.
static void zeroWindowNeverExpires() {
  resetWorld(40000);
  gSettings.sessionIdleBle = 0;
  const uint16_t phone = 3;
  {
    BleLifecycleGuard lifecycle;
    CHECK(bleConsoleMakeSlotLocked(phone), "the phone-link console slot is made");
  }
  login(phone, "admin");
  gClockMs += 24 * 60 * kMinute;
  bleSessionTick();
  CHECK(conn(phone).authed && expiriesFor(phone) == 0, "0 disables the idle logout");
  gSettings.sessionIdleBle = 15;
}

// The phone link's console row (phone link v2), as blePipeConsoleBind leaves
// it: its own id, the pipe's generation and stream, the snapshot published.
static uint16_t bindPipeRow(uint32_t generation, uint16_t stream) {
  const uint16_t row = blePipeConsoleRowId(generation);
  BleLifecycleGuard lifecycle;
  CHECK(bleConsoleMakeSlotLocked(row), "the console row's slot is made");
  BLEConnection& c = gBLEState->connections[findConnectionSlotByConnIdLocked(row)];
  c.pipeConsole = true;
  c.pipeGeneration = generation;
  c.pipeStream = stream;
  c.pipeLinkConnId = 3;
  sBlePipeRowSlot = findConnectionSlotByConnIdLocked(row);
  blePipeIdentitySyncLocked();
  return row;
}

// The console row's idle expiry: the notice leaves the main loop as one record
// offered now (TryNow, never a blocking send). A full stream keeps it pending,
// offered again on each tick while that signed-out session lasts, and sent once;
// a new session (login) drops it. Design A's slot beside it keeps its waiting
// notice.
static void pipeRowNoticeIsTryNowAndRetried() {
  resetWorld(50000);
  const uint16_t row = bindPipeRow(12, 4);
  const uint16_t slotA = 3;  // design A's slot on the same link
  {
    BleLifecycleGuard lifecycle;
    CHECK(bleConsoleMakeSlotLocked(slotA), "design A's slot is made beside the row");
  }
  login(row, "admin");
  login(slotA, "alice");
  CHECK(sBlePipeIdentity.bound && sBlePipeIdentity.authed && sBlePipeIdentity.epoch == epochOf(row),
        "the snapshot follows the console row's login");
  const uint32_t t0 = gClockMs;
  gFullSends = 2;  // the console stream is full for the first two offers
  gClockMs = t0 + kWindow + 1;
  bleSessionTick();
  CHECK(!conn(row).authed && !sBlePipeIdentity.authed && sBlePipeIdentity.epoch == epochOf(row),
        "the console row expired and the snapshot follows its new session");
  CHECK(gSent.size() == 2 && gSent[1].connId == slotA && gSent[1].blocking && gSent[1].accepted,
        "design A's slot beside the row did not expire with its waiting notice");
  const Sent first = gSent[0];
  CHECK(first.connId == row && !first.blocking && !first.accepted && first.epoch == epochOf(row) &&
            first.text == "[ble] Session expired for user 'admin'",
        "the console row's notice was not one TryNow offer to its new session");
  CHECK(conn(row).noticePending, "a notice the stream refused is not kept");
  gClockMs += kSecond;
  bleSessionTick();  // refused again
  gClockMs += kSecond;
  bleSessionTick();  // fits
  CHECK(gSent.size() == 4 && gSent[3].connId == row && !gSent[3].blocking && gSent[3].accepted &&
            gSent[3].text == first.text && gSent[3].epoch == first.epoch,
        "the pending notice was not offered again each tick until it fit (%zu sends)", gSent.size());
  CHECK(!conn(row).noticePending, "a delivered notice stays pending");
  gClockMs += kSecond;
  bleSessionTick();
  CHECK(gSent.size() == 4, "a delivered notice was sent again");

  // A new session (login) drops a pending notice.
  login(row, "admin");
  gFullSends = 100;
  gClockMs += kWindow + 1;
  bleSessionTick();
  CHECK(conn(row).noticePending && gSent.size() == 5, "test setup: a second notice pending");
  gFullSends = 0;
  login(row, "bob");
  gClockMs += kSecond;
  bleSessionTick();
  CHECK(gSent.size() == 5 && !conn(row).noticePending,
        "a notice pending for an ended session was offered to the next one");
}

int main() {
  pipeRowNoticeIsTryNowAndRetried();
  phoneLinkSessionLivesWhileUsed();
  idleAfterLoginExpiresFromLogin();
  serverModeSendDoesNotExpireTheNextSession();
  otaUploadInFlightOutranksIdle();
  zeroWindowNeverExpires();
  CHECK(gLifecycle.depth == 0, "every lifecycle lock was released");
  if (gFailures) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("BLE idle logout: a command landing during the tick never expires its session; "
              "15 minutes idle expires it once\n");
  return 0;
}
