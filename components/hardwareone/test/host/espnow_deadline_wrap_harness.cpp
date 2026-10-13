// Host harness: millis()-wrap-safe deadlines in the ESP-NOW module, driven by an
// injectable clock placed on both sides of the 32-bit rollover.
//   A16_UNIT=1  production prev-key unwrap, rekey apply and prev-key sweep from
//               System_ESPNow_Sessions.cpp (the 5 s previous-key window).
//   A16_UNIT=2  production sensor-subscription apply and lease-expiry owner from
//               System_ESPNow_Sensors.cpp, and the session pre-warm backoff owners
//               from System_ESPNow.cpp (the 60 s cooldown).
// Doubles: the clock, counting log macros, a key-matching AEAD open, a counting
// TX guard and pending-frame drain, and minimal FreeRTOS/sensor-task stand-ins.
// No radio, no hardware.
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef A16_UNIT
#error "compile with -DA16_UNIT=1 (sessions) or -DA16_UNIT=2 (leases and pre-warm)"
#endif

// ---- Injectable clock ---------------------------------------------------------
static uint32_t gNowMs = 0;
static unsigned long millis() { return gNowMs; }

// ---- Deadline owner (System_ESPNow_Deadline.h, when the tree has it) ----------
// INSERT_DEADLINE

static unsigned gChecks = 0, gFailures = 0;
static void check(bool ok, const char* id, const char* what) {
  ++gChecks;
  if (!ok) {
    ++gFailures;
    std::fprintf(stderr, "FAIL %s %s\n", id, what);
  }
}

static unsigned gLogs = 0;
__attribute__((format(printf, 1, 2)))
static void noteLog(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  ++gLogs;
}

#if A16_UNIT == 1
// =============================================================================
// Unit 1: previous-key window (System_ESPNow_Sessions.cpp)
// =============================================================================
#define INFO_ESPNOWF(...)  noteLog(__VA_ARGS__)
#define WARN_ESPNOWF(...)  noteLog(__VA_ARGS__)
#define ERROR_ESPNOWF(...) noteLog(__VA_ARGS__)
static void sodium_memzero(void* p, size_t n) { std::memset(p, 0, n); }

// ---- Production wire header and session record --------------------------------
// INSERT_WIRE
// INSERT_SESSION_TYPES

static unsigned gGuards = 0, gDrains = 0;
struct EspNowTxGuard {
  explicit EspNowTxGuard(const char*) { ++gGuards; }
  EspNowTxGuard(const EspNowTxGuard&) = delete;
  EspNowTxGuard& operator=(const EspNowTxGuard&) = delete;
};
void pendingFrameDrainForPeer(const uint8_t peerMac[6]) { (void)peerMac; ++gDrains; }

// AEAD open double: succeeds only under the key the scripted peer seals with.
static uint8_t gPeerSealKey[32];
bool espnowCryptoAeadOpen(uint8_t* out, const uint8_t* cipher, size_t cipherLen, const uint8_t tag[16],
                          const uint8_t* aad, size_t aadLen, const uint8_t nonce[12], const uint8_t key[32]) {
  (void)tag; (void)nonce;
  if (!aad || aadLen != 30) return false;
  if (std::memcmp(key, gPeerSealKey, 32) != 0) return false;
  std::memcpy(out, cipher, cipherLen);
  return true;
}

// ---- Production internals (anonymous namespace in the .cpp) -------------------
namespace {
// INSERT_SESSION_INTERNAL
}  // namespace

// ---- Production entry points ---------------------------------------------------
// INSERT_SESSION_PRODUCTION

// A fresh ACTIVE session whose peer still seals under the old RX key (0xAA),
// rekeyed at time t to TX 0xBB / RX 0xCC by the real sessionApplyRekeyedKeys.
static void rekeyAt(SessionState& s, uint32_t t) {
  std::memset(&s, 0, sizeof s);
  s.state = SESSION_ACTIVE;
  s.sessionId = 7;
  std::memset(s.aeadKeyRx, 0xAA, 32);
  uint8_t tx[32], rx[32];
  std::memset(tx, 0xBB, 32);
  std::memset(rx, 0xCC, 32);
  gNowMs = t;
  sessionApplyRekeyedKeys(&s, tx, rx);
  std::memset(gPeerSealKey, 0xAA, 32);
}

// A frame the peer sealed under the previous key, opened by the real unwrap.
static bool unwrapUnderPrev(SessionState& s, uint32_t frameSeq) {
  EspNowV4Header h;
  std::memset(&h, 0, sizeof h);
  h.frameSeq = frameSeq;
  uint8_t cipher[4 + kAeadTagBytes] = {1, 2, 3, 4};
  uint8_t plain[sizeof cipher];
  uint16_t plainLen = 0;
  return sessionUnwrapFrame(&s, &h, cipher, sizeof cipher, plain, &plainLen) && plainLen == 4;
}

int main() {
  static SessionState table[kSessionSlots];
  std::memset(table, 0, sizeof table);
  gSessions = table;
  SessionState& s = table[0];

  // Plain clock, far from the wrap.
  rekeyAt(s, 1000);
  gNowMs = 3000;
  check(unwrapUnderPrev(s, 1), "S1", "prev key accepted 2 s after a rekey at 1000");
  gNowMs = 8000;
  check(!unwrapUnderPrev(s, 2), "S2", "prev key rejected 7 s after a rekey at 1000");
  sessionRekeyPrevKeysSweep(8000);
  check(s.prevKeysValidUntilMs == 0, "S2b", "prev key swept 7 s after a rekey at 1000");

  // Rekey 8 s before the wrap (deadline 0xFFFFF388 does not wrap), checked 12.3 s later.
  rekeyAt(s, 0xFFFFE000u);
  check(s.prevKeysValidUntilMs == 0xFFFFF388u, "S3a", "rekey at 0xFFFFE000 stores the deadline 0xFFFFF388");
  gNowMs = 0x00001000u;
  check(!unwrapUnderPrev(s, 1), "S3",
        "prev key rejected 12.3 s after a rekey at 0xFFFFE000 (now 0x00001000 after the wrap; "
        "an absolute 'now <= until' keeps it for ~49.7 days)");
  sessionRekeyPrevKeysSweep(0x00001000u);
  check(s.prevKeysValidUntilMs == 0, "S4",
        "prev key swept 12.3 s after a rekey at 0xFFFFE000 (an absolute 'now > until' never sweeps it)");

  // Rekey 4 s before the wrap (deadline wraps to 0x00000388), checked 2 s later.
  rekeyAt(s, 0xFFFFF000u);
  check(s.prevKeysValidUntilMs == 0x00000388u, "S5a", "rekey at 0xFFFFF000 stores the wrapped deadline 0x00000388");
  gNowMs = 0xFFFFF7D0u;
  check(unwrapUnderPrev(s, 1), "S5",
        "prev key accepted 2 s after a rekey at 0xFFFFF000 (an absolute 'now <= until' rejects it at once)");
  sessionRekeyPrevKeysSweep(0xFFFFF7D0u);
  check(s.prevKeysValidUntilMs == 0x00000388u, "S6",
        "prev key not swept 2 s after a rekey at 0xFFFFF000 (an absolute 'now > until' wipes it at once)");
  gNowMs = 0x00000388u + 3000;
  check(!unwrapUnderPrev(s, 2), "S7", "prev key rejected 8 s after a rekey at 0xFFFFF000");
  sessionRekeyPrevKeysSweep(0x00000388u + 3000);
  check(s.prevKeysValidUntilMs == 0, "S7b", "prev key swept 8 s after a rekey at 0xFFFFF000");

  // A deadline that lands exactly on 0 must not read as "no prev keys".
  rekeyAt(s, 0xFFFFFFFFu - 4999u);
  check(s.prevKeysValidUntilMs != 0, "S8",
        "rekey at 0xFFFFEC79 keeps a prev-key window (deadline 0 would mean 'no prev keys')");
  gNowMs = 0xFFFFFFFFu - 4999u + 2000;
  check(unwrapUnderPrev(s, 1), "S8b", "prev key accepted 2 s after a rekey at 0xFFFFEC79");

  check(gGuards == 4 && gDrains == 4, "S9", "every rekey took the TX guard and drained the pending frames once");
  std::printf("espnow_deadline_wrap unit 1 (prev-key window): %u checks, %u failures\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}

#elif A16_UNIT == 2
// =============================================================================
// Unit 2: sensor leases (System_ESPNow_Sensors.cpp) and the session pre-warm
// cooldown (System_ESPNow.cpp)
// =============================================================================
// ---- Production sensor enums ----------------------------------------------------
// INSERT_SENSOR_TYPES

#define DEBUG_ESPNOW_STREAMF(...) noteLog(__VA_ARGS__)
using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
#define pdTRUE 1
static int gSensorBcastTaskMux = 0;
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
static int gLifecycleStorage = 0;
static SemaphoreHandle_t gSensorBroadcasterLifecycleMutex = &gLifecycleStorage;
static bool gLifecycleHeld = false;
static int xSemaphoreTake(SemaphoreHandle_t, int) {
  if (gLifecycleHeld) return 0;
  gLifecycleHeld = true;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t) { gLifecycleHeld = false; }
static std::atomic<bool> gSensorStreamingShutdown{false};
struct EspNowState { bool meshSensorReqPending = false; };
static EspNowState gEspNowState;
static EspNowState* gEspNow = &gEspNowState;
static uint8_t gSensorControllerMac[6] = {0};
static bool gSensorBroadcastEnabled = false;
static bool gSensorStreamingEnabled[REMOTE_SENSOR_MAX] = {false};
static unsigned long gLastTxMs[REMOTE_SENSOR_MAX] = {0};
static uint32_t gSensorReqIntervalMs[REMOTE_SENSOR_MAX] = {0};
// ---- Production lease state -----------------------------------------------------
// INSERT_LEASE_STATE
static TaskHandle_t gSensorBroadcasterTask = nullptr;
static unsigned gStarts = 0, gStatusFrames = 0;
static bool startSensorBroadcasterLocked() {
  ++gStarts;
  gSensorBroadcasterTask = &gStarts;
  return true;
}
void broadcastSensorStatus(RemoteSensorType, bool) { ++gStatusFrames; }

// ---- Production: lease owner and subscription apply --------------------------------
// INSERT_SENSOR_PRODUCTION

// ---- Production: session pre-warm table and backoff owners -----------------------
#define EXT_RAM_BSS_ATTR
// INSERT_PREWARM_PRODUCTION

static const uint8_t kController[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0C};
static void subscribe(uint32_t now, uint8_t mode, uint32_t intervalMs, uint32_t leaseMs) {
  gNowMs = now;
  espnowApplySensorSubscription(mode, 1u << REMOTE_SENSOR_IMU, intervalMs, leaseMs, kController);
}

int main() {
  const int imu = REMOTE_SENSOR_IMU;

  // L1/L2 — lease of 5 s stamped 4 s before the wrap: the deadline wraps to 0x388.
  subscribe(0xFFFFF000u, SENSOR_REQ_SUBSCRIBE, 1000, 5000);
  check(gSensorLeaseExpiresAt[imu] == 0x00000388u && gSensorStreamingEnabled[imu] && gStarts == 1, "L1",
        "SUBSCRIBE at 0xFFFFF000 for 5 s stamps 0x00000388 and starts streaming");
  check(!sensorLeaseLapsed(imu, 0xFFFFF7D0u), "L2",
        "lease alive 2 s later, before the wrap (an absolute 'now >= exp' lapses it at once)");
  check(sensorLeaseLapsed(imu, 0x00000388u + 3000), "L2b", "lease lapsed 8 s later, after the wrap");

  // L3 — lease of 5 s stamped 8 s before the wrap: the deadline does not wrap.
  subscribe(0xFFFFE000u, SENSOR_REQ_SUBSCRIBE, 1000, 5000);
  check(gSensorLeaseExpiresAt[imu] == 0xFFFFF388u, "L3", "SUBSCRIBE at 0xFFFFE000 for 5 s stamps 0xFFFFF388");
  check(!sensorLeaseLapsed(imu, 0xFFFFE7D0u), "L3b", "lease alive 2 s later");
  check(sensorLeaseLapsed(imu, 0x00001000u), "L3c",
        "lease lapsed 12.3 s later, across the wrap (an absolute 'now >= exp' keeps it ~49.7 days)");

  // L4 — one-shot lease (500 ms default) whose deadline lands exactly on 0.
  subscribe(0xFFFFFFFFu - 499u, SENSOR_REQ_ONESHOT, 0, 0);
  check(gSensorLeaseExpiresAt[imu] == 1u, "L4", "one-shot lease landing on 0 is stored as 1, never as 'no lease'");
  check(!sensorLeaseLapsed(imu, 0xFFFFFFFFu - 499u) && sensorLeaseLapsed(imu, 1u), "L4b",
        "one-shot lease alive when stamped and lapsed ~500 ms later");

  // L5 — a lease longer than INT32_MAX ms is capped so it reads as far ahead.
  subscribe(1000, SENSOR_REQ_SUBSCRIBE, 1000, 0xFFFFFFFFu);
  check(gSensorLeaseExpiresAt[imu] == 1000u + 0x7FFFFFFFu, "L5", "oversized lease capped at INT32_MAX ms");
  check(!sensorLeaseLapsed(imu, 1000u) && !sensorLeaseLapsed(imu, 1000u + 0x7FFFFFFEu) &&
            sensorLeaseLapsed(imu, 1000u + 0x7FFFFFFFu), "L5b",
        "capped lease alive when stamped and until the cap, lapsed at the cap");

  // L6 — 0 is manual streaming: it never lapses.
  gSensorLeaseExpiresAt[REMOTE_SENSOR_TOF] = 0;
  check(!sensorLeaseLapsed(REMOTE_SENSOR_TOF, 0) && !sensorLeaseLapsed(REMOTE_SENSOR_TOF, 0x90000000u), "L6",
        "a slot without a lease (0) never lapses");

  // L7 — UNSUBSCRIBE clears the lease.
  subscribe(5, SENSOR_REQ_UNSUBSCRIBE, 0, 0);
  check(gSensorLeaseExpiresAt[imu] == 0 && !gSensorStreamingEnabled[imu], "L7", "UNSUBSCRIBE clears the lease");
  check(gStatusFrames == 5 && !gEspNow->meshSensorReqPending && !gLifecycleHeld, "L7b",
        "one status frame per apply, no deferred retry, lifecycle lock released");

  // P1-P3 — pre-warm backoff armed 4 s before the wrap (60 s deadline wraps to 0xDA60).
  const uint8_t peer[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  SessionPrewarm* pw = sessionPrewarmSlot(peer);
  check(pw && !sessionPrewarmInCooldown(*pw, 0xFFFFF000u), "P1", "fresh pre-warm slot is not backing off");
  check(!sessionPrewarmNoteAttempt(pw, 0xFFFFF000u) && !sessionPrewarmNoteAttempt(pw, 0xFFFFF000u), "P1b",
        "attempts 1 and 2 do not start the backoff");
  check(sessionPrewarmNoteAttempt(pw, 0xFFFFF000u) && pw->cooldownUntilMs == 0x0000DA60u && pw->fails == 0, "P2",
        "attempt 3 at 0xFFFFF000 starts a 60 s backoff ending 0x0000DA60 and resets the count");
  check(sessionPrewarmInCooldown(*pw, 0xFFFFF800u), "P3",
        "backing off 2 s later, before the wrap (an absolute 'now >= until' re-kicks SESSION_OPEN)");
  check(!sessionPrewarmInCooldown(*pw, 0x0000DA60u), "P3b", "backoff over 60 s later, after the wrap");

  // P4 — reset clears the backoff.
  sessionPrewarmReset(peer);
  check(pw->cooldownUntilMs == 0 && !sessionPrewarmInCooldown(*pw, 0x0000DA60u), "P4", "reset clears the backoff");

  // P5 — backoff armed 65.5 s before the wrap (deadline 0xFFFFEA60 does not wrap).
  for (int i = 0; i < 3; ++i) sessionPrewarmNoteAttempt(pw, 0xFFFF0000u);
  check(pw->cooldownUntilMs == 0xFFFFEA60u && sessionPrewarmInCooldown(*pw, 0xFFFF0001u), "P5",
        "backoff armed at 0xFFFF0000 ends 0xFFFFEA60 and is active 1 ms later");
  check(!sessionPrewarmInCooldown(*pw, 0x00001000u), "P5b",
        "backoff over 69.6 s later, across the wrap (an absolute 'now >= until' stays silent ~49.7 days)");

  // P6 — a backoff deadline that lands exactly on 0 is not "no backoff".
  sessionPrewarmReset(peer);
  for (int i = 0; i < 3; ++i) sessionPrewarmNoteAttempt(pw, 0xFFFF15A0u);
  check(pw->cooldownUntilMs == 1u && sessionPrewarmInCooldown(*pw, 0xFFFF15A1u), "P6",
        "backoff landing on 0 is stored as 1 and active 1 ms later");

  // P7 — the slot lookup is stable for a known peer.
  check(sessionPrewarmSlot(peer) == pw, "P7", "slot lookup returns the same entry for the same peer");

  std::printf("espnow_deadline_wrap unit 2 (sensor leases, pre-warm cooldown): %u checks, %u failures (logs=%u)\n",
              gChecks, gFailures, gLogs);
  return gFailures ? 1 : 0;
}
#else
#error "A16_UNIT must be 1 or 2"
#endif
