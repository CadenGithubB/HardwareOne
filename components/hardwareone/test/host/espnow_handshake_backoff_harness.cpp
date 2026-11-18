// Host harness for test_espnow_handshake_backoff.py.
//
// The runner pastes, from the production sources: the pending-frame queue and
// the ESTABLISHING timeout sweep (System_ESPNow_Sessions.cpp); the KEY_EX
// in-flight table, its arm / clear / query / retry sweep, the key-exchange
// start and, when present, the handshake back-off table and entry points
// (System_ESPNow_Handlers_Crypto.cpp); v4_send_encrypted_or_queue and
// bondSendEncryptedAsync (System_ESPNow.cpp), each at its INSERT tag below.
// System_ESPNow_HandshakeBackoff.h is included whole when the
// tree has it (HW1_HAVE_BACKOFF).
//
// Doubles: the clock, the log (WARN lines are recorded), peer identities,
// session slots, espnow_tx (runs the job at once, as v4_send_payload_smart does
// for one frame), the HELLO sender and SESSION_OPEN. A peer that is there
// answers 100 ms later the way v4hKeyExReply and runDeferredSessionConfirm
// do (the back-off calls in those two are pinned in the source by the runner).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_MacText.h"
#include "System_Deadline.h"
#if HW1_HAVE_BACKOFF
#include "System_ESPNow_HandshakeBackoff.h"
#endif

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++gChecks;                                                        \
    if (!(cond)) {                                                    \
      ++gFailures;                                                    \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

// ---- Clock and log ---------------------------------------------------------------
static uint32_t gNowMs = 1;
static uint32_t millis() { return gNowMs; }

static std::vector<std::string> gWarnLines;
template <class... A>
static void recordWarn(const char* fmt, A... args) {
  char line[512];
  if constexpr (sizeof...(A) == 0) {
    std::snprintf(line, sizeof line, "%s", fmt);
  } else {
    std::snprintf(line, sizeof line, fmt, args...);
  }
  gWarnLines.push_back(line);
}
template <class... A> static void sink(A&&...) {}
#define WARN_ESPNOWF(...) recordWarn(__VA_ARGS__)
#define INFO_ESPNOWF(...) sink(__VA_ARGS__)
#define ERROR_ESPNOWF(...) sink(__VA_ARGS__)
#define DEBUG_ESPNOWF(...) sink(__VA_ARGS__)

static bool logCooldownOk(uint32_t& lastMs, uint32_t windowMs) {   // System_Debug.cpp
  uint32_t now = millis();
  if (lastMs != 0 && (now - lastMs) < windowMs) return false;
  lastMs = now;
  return true;
}

static size_t warnsSince(size_t from, const char* prefix = nullptr) {
  size_t n = 0;
  for (size_t i = from; i < gWarnLines.size(); ++i) {
    if (!prefix || gWarnLines[i].rfind(prefix, 0) == 0) ++n;
  }
  return n;
}

// ---- Platform doubles ------------------------------------------------------------
struct portMUX_TYPE { int unused; };
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
enum class AllocPref { PreferPsram };
static void* ps_alloc(size_t n, AllocPref, const char*) { return std::malloc(n); }
static void sodium_memzero(void* p, size_t n) { std::memset(p, 0, n); }
struct EspNowTxGuard { explicit EspNowTxGuard(const char*) {} };
class EspNowRuntimeOpGuard {
 public:
  bool held() const { return true; }
};
static int gMarkedFailed = 0;
static void sendStatusMarkFailed(uint32_t, const uint8_t*) { ++gMarkedFailed; }

struct EspNowState { bool initialized = true; };
static EspNowState gState;
static EspNowState* gEspNow = &gState;

#define ESPNOW_V4_MAX_PAYLOAD (250 - 32)
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_HEARTBEAT = 170;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_CAP_REQ = 171;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_SENSOR_DATA = 179;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_POWER_STATUS = 180;
static constexpr uint8_t ESPNOW_V4_TYPE_BOND_EVENT_PUSH = 182;

// Our address and the partner's.
static uint8_t gSelf[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x09};      // higher: initiator
static const uint8_t kPartner[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01};
static void espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, gSelf, 6); }
static bool sessionIsASide(const uint8_t me[6], const uint8_t peer[6]) { return std::memcmp(me, peer, 6) < 0; }

// Identities (KEY_EX done) and session slots.
struct PeerIdentity { uint8_t mac[6]; uint8_t meshId; bool inUse; };
static PeerIdentity gIds[8];
static const PeerIdentity* peerIdentityFindByMac(const uint8_t mac[6]) {
  for (auto& p : gIds) if (p.inUse && std::memcmp(p.mac, mac, 6) == 0) return &p;
  return nullptr;
}
static void addIdentity(const uint8_t mac[6]) {
  if (peerIdentityFindByMac(mac)) return;
  for (auto& p : gIds) if (!p.inUse) { std::memcpy(p.mac, mac, 6); p.meshId = 0; p.inUse = true; return; }
}

enum : uint8_t { SESSION_FREE = 0, SESSION_ESTABLISHING = 1, SESSION_ACTIVE = 2, SESSION_REKEYING = 3,
                 SESSION_CLOSED = 4 };
struct SessionState {
  uint8_t peerMac[6]; uint8_t meshId; uint8_t state; uint32_t lastUseMs; bool inUse; uint8_t userStarted;
};
constexpr uint8_t kSessionSlots = 8;
static SessionState gSessionTable[kSessionSlots];
static SessionState* gSessions = gSessionTable;
static SessionState* sessionFindByPeer(const uint8_t mac[6], uint8_t) {
  for (auto& s : gSessionTable) if (s.inUse && std::memcmp(s.peerMac, mac, 6) == 0) return &s;
  return nullptr;
}
static void sessionClear(SessionState* s) { std::memset(s, 0, sizeof(*s)); }

// The far end: is it there, does it answer.
static bool gPeerPresent = false;
struct Event { uint32_t atMs; int kind; uint8_t mac[6]; };   // 0 = KEY_EX_REPLY, 1 = SESSION_CONFIRM
static std::vector<Event> gEvents;
static int gHellos = 0;
static int gPartnerHellos = 0;
static int gSessionOpens = 0;
static int gSessionsUp = 0;
static uint32_t gLastSessionUpMs = 0;

static int resolveMeshSlot(const char*) { return 0; }
static bool keyExSendHello(const uint8_t peerMac[6], int) {
  ++gHellos;
  if (std::memcmp(peerMac, kPartner, 6) == 0) ++gPartnerHellos;
  if (gPeerPresent && std::memcmp(peerMac, kPartner, 6) == 0) {
    Event e{gNowMs + 100, 0, {}};
    std::memcpy(e.mac, peerMac, 6);
    gEvents.push_back(e);
  }
  return true;
}
static bool espnowSessionOpenInitiate(const uint8_t peerMac[6], const char*, bool* outAlreadyInFlight = nullptr) {
  if (outAlreadyInFlight) *outAlreadyInFlight = false;
  if (!peerIdentityFindByMac(peerMac)) return false;
  SessionState* s = sessionFindByPeer(peerMac, 0);
  if (s && (s->state == SESSION_ESTABLISHING || s->state == SESSION_REKEYING)) {
    if (outAlreadyInFlight) *outAlreadyInFlight = true;
    return true;
  }
  if (!s) {
    for (auto& c : gSessionTable) if (!c.inUse) { s = &c; break; }
  }
  std::memset(s, 0, sizeof(*s));
  std::memcpy(s->peerMac, peerMac, 6);
  s->inUse = true;
  s->state = SESSION_ESTABLISHING;
  s->lastUseMs = gNowMs;
  ++gSessionOpens;
  if (gPeerPresent) {
    Event e{gNowMs + 100, 1, {}};
    std::memcpy(e.mac, peerMac, 6);
    gEvents.push_back(e);
  }
  return true;
}

// Frames that went out over an ACTIVE session.
static std::vector<uint8_t> gDelivered;
bool v4_send_session_wrapped(const uint8_t dst[6], uint8_t type, uint16_t, uint32_t, const uint8_t*, uint16_t,
                             uint8_t, char* errOut, size_t errOutLen) {
  SessionState* s = sessionFindByPeer(dst, 0);
  if (!s || s->state != SESSION_ACTIVE) {
    if (errOut && errOutLen) std::snprintf(errOut, errOutLen, "no session");
    return false;
  }
  gDelivered.push_back(type);
  return true;
}

// espnow_tx: the job runs at once (single frame: v4_send_payload_smart ->
// v4_send_encrypted_or_queue).
namespace espnowtx {
enum JobKind : uint8_t { JOB_AEAD_SMART, JOB_RAW };
struct Job {
  JobKind kind;
  uint8_t peerMac[6];
  uint8_t type;
  uint16_t flags;
  uint32_t msgId;
  uint8_t ttl;
  uint16_t payloadLen;
  uint8_t* payload;
  void* completion;
};
static bool submit(const Job& job);
}  // namespace espnowtx

// Declarations the sources take from System_ESPNow_Handlers_Crypto.h.
bool espnowKeyExInitiate(const uint8_t peerMac[6], const char* meshLabel);
bool keyExIsInFlight(const uint8_t peerMac[6]);
#if HW1_HAVE_BACKOFF
bool espnowKeyExInitiateManual(const uint8_t peerMac[6], const char* meshLabel);
bool espnowHandshakeAutoStart(const uint8_t peerMac[6]);
void espnowHandshakeBackoffClear(const uint8_t peerMac[6]);
void espnowHandshakeHeard(const uint8_t peerMac[6]);
#endif
#if HW1_HAVE_START_NOW
bool espnowHandshakeStartNow(const uint8_t peerMac[6]);
void sessionMarkUserStarted(const uint8_t peerMac[6]);
#endif

// INSERT_PENDING

// INSERT_KEYEX

// INSERT_SEND

// A tree without the unpair call-off: the unpair did nothing of the kind.
#if !HW1_HAVE_CANCEL
static void espnowKeyExCancel(const uint8_t*) {}
#endif
#if !HW1_HAVE_PENDING_DROP
static uint8_t pendingFrameDropForPeer(const uint8_t*) { return 0; }
#endif

namespace espnowtx {
static bool submit(const Job& job) {
  v4_send_encrypted_or_queue(job.peerMac, job.type, job.flags, job.msgId, job.payload, job.payloadLen,
                             job.ttl, nullptr, 0);
  if (job.payload) std::free(job.payload);
  return true;
}
}  // namespace espnowtx

// ---- The far end answering (v4hKeyExReply / runDeferredSessionConfirm) ---------
static void peerAnswered(const Event& e) {
  if (e.kind == 0) {          // KEY_EX_REPLY
    addIdentity(e.mac);
    keyExClearInFlight(e.mac);
#if HW1_HAVE_BACKOFF
    espnowHandshakeBackoffClear(e.mac);
#endif
    SessionState* live = sessionFindByPeer(e.mac, 0);
    if (live && live->state == SESSION_ACTIVE) {
      pendingFrameDrainForPeer(e.mac);
    } else if (pendingFrameHasForPeer(e.mac)) {
      bool already = false;
      espnowSessionOpenInitiate(e.mac, nullptr, &already);
    }
  } else {                    // SESSION_CONFIRM: ACTIVE, drain, bondNotifySessionEstablished
    SessionState* s = sessionFindByPeer(e.mac, 0);
    if (!s || s->state != SESSION_ESTABLISHING) return;
    s->state = SESSION_ACTIVE;
    s->lastUseMs = gNowMs;
    ++gSessionsUp;
    gLastSessionUpMs = gNowMs;
    pendingFrameDrainForPeer(e.mac);
#if HW1_HAVE_BACKOFF
    espnowHandshakeBackoffClear(e.mac);
#endif
  }
}

// ---- Simulation ------------------------------------------------------------------
static uint32_t gNextHeartbeatMs = 0;
static uint32_t gMsgId = 1;
static size_t gMaxPending = 0;
static uint32_t gHeardEveryMs = 0;     // >0: the partner is heard this often (never answering)
static uint32_t gNextHeardMs = 0;

static void sendHeartbeat() {
  uint8_t payload[24] = {};
  bondSendEncryptedAsync(kPartner, ESPNOW_V4_TYPE_BOND_HEARTBEAT, 0, ++gMsgId, payload, sizeof payload);
  if (pendingFrameCount() > gMaxPending) gMaxPending = pendingFrameCount();
}

static void runFor(uint32_t ms) {
  const uint32_t end = gNowMs + ms;
  while (gNowMs < end) {
    gNowMs += 100;
    for (size_t i = 0; i < gEvents.size();) {
      if ((int32_t)(gNowMs - gEvents[i].atMs) >= 0) {
        Event e = gEvents[i];
        gEvents.erase(gEvents.begin() + (long)i);
        peerAnswered(e);
      } else {
        ++i;
      }
    }
    keyExRetrySweep(gNowMs);
    pendingFrameTimeoutSweep(gNowMs);
    sessionEstablishingTimeoutSweep(gNowMs);
    if ((int32_t)(gNowMs - gNextHeartbeatMs) >= 0) {
      gNextHeartbeatMs = gNowMs + 5000;
      sendHeartbeat();
    }
#if HW1_HAVE_BACKOFF
    if (gHeardEveryMs && (int32_t)(gNowMs - gNextHeardMs) >= 0) {
      gNextHeardMs = gNowMs + gHeardEveryMs;
      espnowHandshakeHeard(kPartner);
    }
#endif
  }
}

static void resetWorld() {
  gNowMs = 1;
  gNextHeartbeatMs = 0;
  gNextHeardMs = 0;
  gHeardEveryMs = 0;
  gPeerPresent = false;
  gEvents.clear();
  gHellos = gPartnerHellos = gSessionOpens = gSessionsUp = 0;
  gMarkedFailed = 0;
  gLastSessionUpMs = 0;
  gDelivered.clear();
  gMaxPending = 0;
  std::memset(gIds, 0, sizeof gIds);
  std::memset(gSessionTable, 0, sizeof gSessionTable);
  for (auto& k : gKeyExInFlight) k.inUse = false;
  if (gPending) {
    for (uint8_t i = 0; i < kPendingFrameSlots; ++i) std::memset(&gPending[i], 0, sizeof(gPending[i]));
  }
#if HW1_HAVE_BACKOFF
  gHandshakeBackoff = HandshakeBackoffTable{};
  gKeyExGiveUpWarn = WarnLimiter{};
  gPendingEvictWarn = WarnLimiter{};
  gPendingTimeoutWarn = WarnLimiter{};
#endif
  const uint8_t initiator[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x09};
  std::memcpy(gSelf, initiator, 6);
}

static size_t heartbeatsDelivered() {
  size_t n = 0;
  for (uint8_t t : gDelivered) n += (t == ESPNOW_V4_TYPE_BOND_HEARTBEAT);
  return n;
}

// ---- Cases -----------------------------------------------------------------------

static void caseSchedule() {
#if HW1_HAVE_BACKOFF
  const uint32_t want[] = {5000, 10000, 20000, 40000, 80000, 160000, 300000, 300000, 300000};
  bool ok = true;
  for (uint8_t n = 1; n <= 9; ++n) ok = ok && handshakeBackoffDelayMs(n) == want[n - 1];
  ok = ok && handshakeBackoffDelayMs(255) == 300000;
  CHECK(ok, "back-off delays are not 5 s doubling to 5 min");

  HandshakeBackoffTable t = {};
  uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
  CHECK(handshakeBackoffTake(t, mac, 1000), "a first start is not due at once");
  CHECK(!handshakeBackoffTake(t, mac, 5999) && handshakeBackoffTake(t, mac, 6000),
        "the second start is not due exactly 5 s after the first");
  CHECK(!handshakeBackoffTake(t, mac, 15999) && handshakeBackoffTake(t, mac, 16000),
        "the third start is not due 10 s after the second");
  handshakeBackoffClear(t, mac);
  CHECK(handshakeBackoffTake(t, mac, 16001), "a cleared peer is not due at once");
  // Wrap: a start just before the 49.7-day wrap.
  HandshakeBackoffTable w = {};
  CHECK(handshakeBackoffTake(w, mac, 0xFFFFF000u) && !handshakeBackoffTake(w, mac, 0x00000100u) &&
        handshakeBackoffTake(w, mac, 0x00000388u), "the back-off does not survive the millis() wrap");
#else
  CHECK(false, "no handshake back-off in this tree (System_ESPNow_HandshakeBackoff.h)");
#endif
}

static void caseTableAndHeard() {
#if HW1_HAVE_BACKOFF
  HandshakeBackoffTable t = {};
  uint8_t mac[6] = {2, 0, 0, 0, 0, 0};
  for (uint8_t i = 0; i < kHandshakeBackoffSlots + 3; ++i) {
    mac[5] = i;
    handshakeBackoffTake(t, mac, 1000u + i * 10u);
  }
  int used = 0;
  for (auto& e : t.e) used += e.inUse;
  CHECK(used == kHandshakeBackoffSlots, "the table holds %d entries, want %d (fixed size)", used,
        (int)kHandshakeBackoffSlots);
  mac[5] = 0;   // the oldest was given up for a newer peer
  CHECK(handshakeBackoffFind(t, mac) == nullptr, "a full table did not give up the oldest entry");

  // One entry per paired device (MESH_PEER_MAX, 16): the mesh master offers
  // a start to every paired device without a session, one start per 500 ms
  // look. With sixteen silent devices every one of them still reaches its
  // long wait; with fewer entries than devices the table gave one up every
  // look and the master started 1.5 to 2 handshakes a second for good.
  CHECK(kHandshakeBackoffSlots >= 16, "the table has %d entries, fewer than the 16 paired devices",
        (int)kHandshakeBackoffSlots);
  HandshakeBackoffTable silent = {};
  unsigned starts = 0, lastTenMinutes = 0;
  const uint32_t hourMs = 3600000u;
  for (uint32_t at = 1000; at < 1000 + hourMs; at += 500) {
    for (uint8_t i = 0; i < 16; ++i) {
      mac[5] = (uint8_t)(0x40 + i);
      if (!handshakeBackoffTake(silent, mac, at)) continue;
      ++starts;
      if (at >= 1000 + hourMs - 600000u) ++lastTenMinutes;
      break;  // at most one start per look
    }
  }
  CHECK(starts <= 16 * 20 && lastTenMinutes <= 16 * 3,
        "sixteen silent paired devices cost %u starts in an hour (%u in the last ten minutes)", starts,
        lastTenMinutes);

  // Heard: ends the wait only after a quiet spell.
  HandshakeBackoffTable h = {};
  uint8_t p[6] = {2, 0, 0, 0, 0, 7};
  handshakeBackoffTake(h, p, 1000);                        // start 1
  CHECK(handshakeBackoffHeard(h, p, 1500), "first hearing after a start did not end the wait");
  handshakeBackoffTake(h, p, 2000);                        // start 1 again
  CHECK(!handshakeBackoffHeard(h, p, 3000), "a peer heard 1.5 s ago ended the wait again");
  CHECK(!handshakeBackoffTake(h, p, 4000), "the wait ended for a peer heard all along");
  CHECK(handshakeBackoffHeard(h, p, 3000 + kHandshakeBackoffQuietMs), "a peer back after a quiet minute "
        "did not end the wait");
  uint8_t other[6] = {2, 0, 0, 0, 0, 8};
  CHECK(!handshakeBackoffHeard(h, other, 5000), "hearing a peer with no entry made one");

  WarnLimiter wl = {};
  uint32_t held = 99;
  CHECK(warnLimiterTake(wl, 10, 60000, &held) && held == 0, "the first warning is held");
  for (int i = 0; i < 9; ++i) warnLimiterTake(wl, 100u + (uint32_t)i, 60000, &held);
  CHECK(warnLimiterTake(wl, 60010, 60000, &held) && held == 9, "the next warning does not carry the 9 held (%u)",
        (unsigned)held);
#else
  CHECK(false, "no fixed per-peer back-off table in this tree");
#endif
}

// Bench step d2: the partner is an address no device has.
static void caseAbsentPartner() {
  resetWorld();
  size_t w0 = gWarnLines.size();
  int h0 = gHellos;
  runFor(3600000);
  const size_t hour1Warns = warnsSince(w0);
  const int hour1Hellos = gHellos - h0;
  std::printf("absent partner, hour 1: %zu warning lines, %d HELLOs, max %zu pending\n", hour1Warns,
              hour1Hellos, gMaxPending);
  CHECK(hour1Warns <= 8, "an absent partner cost %zu warning lines in the first hour", hour1Warns);
  CHECK(hour1Hellos <= 25 * 3, "an absent partner cost %d HELLOs in the first hour", hour1Hellos);
  CHECK(gMaxPending == 0, "a heartbeat was queued behind the missing session (max %zu pending)", gMaxPending);
  w0 = gWarnLines.size();
  h0 = gHellos;
  runFor(3600000);
  const size_t hour2Warns = warnsSince(w0);
  std::printf("absent partner, hour 2: %zu warning lines, %d HELLOs\n", hour2Warns, gHellos - h0);
  CHECK(hour2Warns <= 5, "an absent partner cost %zu warning lines in the second hour", hour2Warns);
  CHECK(gHellos - h0 <= 13 * 3, "an absent partner cost %d HELLOs in the second hour", gHellos - h0);
  bool counted = false;
  for (size_t i = w0; i < gWarnLines.size(); ++i) {
    counted = counted || gWarnLines[i].find("more not shown") != std::string::npos;
  }
  CHECK(counted || hour2Warns == 0, "the give-up warning does not say how many it held back");

  // A manual key exchange starts at once, its give-up is always reported, and
  // it ends the back-off: the next automatic start is due at once. It is run
  // right after an automatic start, so without that the next one would be
  // 5 minutes away.
#if HW1_HAVE_BACKOFF
  for (int i = 0; i < 4000 && keyExIsInFlight(kPartner); ++i) runFor(100);
  for (int i = 0; i < 4000 && !keyExIsInFlight(kPartner); ++i) runFor(100);
  CHECK(keyExIsInFlight(kPartner), "no automatic key exchange within 400 s in the second hour");
  const int hellosBefore = gHellos;
  const size_t warnsBefore = gWarnLines.size();
  espnowKeyExInitiateManual(kPartner, nullptr);
  CHECK(gHellos == hellosBefore + 1, "espnowkeyex did not send a HELLO at once");
  bool manualReported = false;
  for (int i = 0; i < 100 && !manualReported; ++i) {
    runFor(100);
    for (size_t j = warnsBefore; j < gWarnLines.size(); ++j) {
      manualReported = manualReported ||
                       gWarnLines[j].find("giving up (run espnowkeyex to retry)") != std::string::npos;
    }
  }
  CHECK(manualReported, "a manual key exchange that got no answer was not reported");
  // Its HELLO and two re-sends, then an automatic start at the next heartbeat.
  runFor(5100);
  CHECK(gHellos > hellosBefore + 1 + (int)kKeyExMaxRetries,
        "espnowkeyex did not end the back-off: no automatic start at the next heartbeat after it gave up "
        "(%d HELLOs since)", gHellos - hellosBefore);
#else
  CHECK(false, "no manual key-exchange start in this tree");
#endif

  // The partner comes back and is heard: the bond re-forms within 15 s.
  runFor(1234);
  gPeerPresent = true;
  const uint32_t heardAt = gNowMs;
#if HW1_HAVE_BACKOFF
  espnowHandshakeHeard(kPartner);
#endif
  runFor(20000);
  CHECK(gSessionsUp >= 1 && gLastSessionUpMs - heardAt <= 15000,
        "a partner that came back was not bonded within 15 s (sessions up %d, after %lu ms)", gSessionsUp,
        (unsigned long)(gLastSessionUpMs - heardAt));
  CHECK(heartbeatsDelivered() >= 1, "no heartbeat reached the partner after it came back");
}

// A peer that is heard all the time but never answers the key exchange
// (another passphrase) keeps its back-off.
static void caseHeardNeverAnswers() {
  resetWorld();
  gHeardEveryMs = 5000;
  gNextHeardMs = 1000;
  runFor(3600000);
  std::printf("heard, never answering, 1 hour: %d HELLOs\n", gHellos);
  CHECK(gHellos <= 25 * 3, "a peer heard all along but never answering cost %d HELLOs in an hour", gHellos);
}

// The partner is there and answers: as before, one handshake, heartbeats flow,
// a lost session is re-opened at the next heartbeat.
static void caseAnsweringPartner() {
  resetWorld();
  const size_t w0 = gWarnLines.size();
  gPeerPresent = true;
  addIdentity(kPartner);
  runFor(60000);
  CHECK(gSessionOpens == 1 && gSessionsUp == 1, "answering partner: %d SESSION_OPENs, %d up (want 1, 1)",
        gSessionOpens, gSessionsUp);
  CHECK(heartbeatsDelivered() >= 11, "answering partner: %zu heartbeats delivered in 60 s", heartbeatsDelivered());
  SessionState* s = sessionFindByPeer(kPartner, 0);
  if (s) sessionClear(s);   // the session is lost (heartbeat timeout, peer reboot)
  const uint32_t lostAt = gNowMs;
  const size_t before = heartbeatsDelivered();
  runFor(12000);
  CHECK(gSessionOpens == 2 && gLastSessionUpMs - lostAt <= 5200,
        "a lost session was not re-opened at the next heartbeat (%d opens, after %lu ms)", gSessionOpens,
        (unsigned long)(gLastSessionUpMs - lostAt));
  CHECK(heartbeatsDelivered() > before, "no heartbeat after the session came back");
  CHECK(warnsSince(w0) == 0, "an answering partner logged %zu warnings", warnsSince(w0));
}

// A known partner (identity on file) that is switched off.
static void caseKnownPartnerOff() {
  resetWorld();
  addIdentity(kPartner);
  const size_t w0 = gWarnLines.size();
  runFor(3600000);
  const size_t warns = warnsSince(w0);
  std::printf("known partner switched off, 1 hour: %zu warning lines, %d SESSION_OPENs\n", warns, gSessionOpens);
  CHECK(warns <= 8, "a known partner that is off cost %zu warning lines in an hour", warns);
  CHECK(gSessionOpens <= 25, "a known partner that is off cost %d SESSION_OPENs in an hour", gSessionOpens);
}

// Frames that are not periodic still wait for the handshake as before.
static void caseRequestStillQueued() {
  resetWorld();
  bondSendEncryptedAsync(kPartner, ESPNOW_V4_TYPE_BOND_CAP_REQ, 0, ++gMsgId, nullptr, 0);
  CHECK(pendingFrameCount() == 1 && gHellos == 1,
        "a CAP_REQ without a session was not queued with a key exchange (pending %u, HELLOs %d)",
        (unsigned)pendingFrameCount(), gHellos);
  // The responder (lower address) never starts anything.
  resetWorld();
  const uint8_t lower[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x00};
  std::memcpy(gSelf, lower, 6);
  runFor(60000);
  CHECK(gHellos == 0 && gSessionOpens == 0 && pendingFrameCount() == 0,
        "the responder started a handshake (%d HELLOs, %d opens)", gHellos, gSessionOpens);
}

// Bond sensor data is periodic like the heartbeat: without a session it is
// dropped quietly, neither queued nor starting a handshake.
static void caseSensorDataDropped() {
  resetWorld();
  const uint8_t reading[16] = {};
  bondSendEncryptedAsync(kPartner, ESPNOW_V4_TYPE_BOND_SENSOR_DATA, 0, ++gMsgId, reading, sizeof reading);
  CHECK(pendingFrameCount() == 0 && gHellos == 0,
        "sensor data without a session was queued or started a key exchange (pending %u, HELLOs %d)",
        (unsigned)pendingFrameCount(), gHellos);
  resetWorld();
  addIdentity(kPartner);
  bondSendEncryptedAsync(kPartner, ESPNOW_V4_TYPE_BOND_SENSOR_DATA, 0, ++gMsgId, reading, sizeof reading);
  CHECK(pendingFrameCount() == 0 && gSessionOpens == 0,
        "sensor data to a known partner without a session was queued or opened a session (pending %u, opens %d)",
        (unsigned)pendingFrameCount(), gSessionOpens);
}

// A bond worker's battery state and events (180/182) are periodic too: the
// sender waits and sends again; nothing is queued and nothing is started.
static void caseBondPowerEventsDropped() {
  const uint8_t frame[24] = {};
  for (uint8_t type : { ESPNOW_V4_TYPE_BOND_POWER_STATUS, ESPNOW_V4_TYPE_BOND_EVENT_PUSH }) {
    resetWorld();
    addIdentity(kPartner);
    bondSendEncryptedAsync(kPartner, type, 0, ++gMsgId, frame, sizeof frame);
    CHECK(pendingFrameCount() == 0 && gSessionOpens == 0 && gHellos == 0,
          "bond frame %u without a session was queued or started a handshake (pending %u, opens %d, HELLOs %d)",
          (unsigned)type, (unsigned)pendingFrameCount(), gSessionOpens, gHellos);
  }
}

static bool warnedSince(size_t from, const char* text) {
  for (size_t i = from; i < gWarnLines.size(); ++i) {
    if (gWarnLines[i].find(text) != std::string::npos) return true;
  }
  return false;
}

// bondconnect / bondresync (espnowHandshakeStartNow) and espnowsessionopen
// after the partner has been away for 10 minutes: they start at once and,
// unlike the automatic tries, their failure is printed.
static void caseUserStartsReported() {
#if HW1_HAVE_START_NOW
  resetWorld();                       // no identity: a key exchange
  runFor(600000);
  int hellos = gHellos;
  size_t w = gWarnLines.size();
  CHECK(espnowHandshakeStartNow(kPartner) && gHellos == hellos + 1,
        "bondconnect without an identity did not send a HELLO at once");
  runFor(6000);
  CHECK(warnedSince(w, "giving up (run espnowkeyex to retry)"),
        "bondconnect's key exchange got no answer and nothing was printed");

  resetWorld();                       // identity on file: SESSION_OPEN
  addIdentity(kPartner);
  runFor(600000);
  w = gWarnLines.size();
  CHECK(espnowHandshakeStartNow(kPartner), "bondconnect with an identity did not start SESSION_OPEN");
  const SessionState* s = sessionFindByPeer(kPartner, 0);
  CHECK(s && s->state == SESSION_ESTABLISHING, "no SESSION_OPEN under way after bondconnect");
  runFor(7000);
  CHECK(warnedSince(w, "session: ESTABLISHING timeout"),
        "bondconnect's SESSION_OPEN got no answer and nothing was printed");

  resetWorld();                       // espnowsessionopen (its calls, in its order)
  addIdentity(kPartner);
  runFor(600000);
  w = gWarnLines.size();
  espnowHandshakeBackoffClear(kPartner);
  bool already = false;
  CHECK(espnowSessionOpenInitiate(kPartner, nullptr, &already), "espnowsessionopen did not start");
  sessionMarkUserStarted(kPartner);
  runFor(7000);
  CHECK(warnedSince(w, "session: ESTABLISHING timeout"),
        "espnowsessionopen got no answer and nothing was printed");
  // The automatic tries after it are limited again.
  const size_t w2 = gWarnLines.size();
  runFor(600000);
  CHECK(warnsSince(w2, "session: ESTABLISHING timeout") <= 1,
        "automatic tries after espnowsessionopen logged %zu timeouts in 10 minutes",
        warnsSince(w2, "session: ESTABLISHING timeout"));

  resetWorld();                       // a session that is up is left alone
  addIdentity(kPartner);
  gPeerPresent = true;
  runFor(10000);
  const int opens = gSessionOpens;
  CHECK(!espnowHandshakeStartNow(kPartner) && gSessionOpens == opens,
        "bondconnect replaced an ACTIVE session");
#else
  CHECK(false, "no manual handshake start for bond commands in this tree");
#endif
}

// The pending-frame warnings: one a minute, with the count held back.
static void casePendingWarnings() {
  resetWorld();
  const uint8_t other[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x05};
  const size_t w0 = gWarnLines.size();
  for (int i = 0; i < 50; ++i) {
    v4_send_encrypted_or_queue(other, 1, 0, ++gMsgId, nullptr, 0, 1, nullptr, 0);
    gNowMs += 200;
  }
  const size_t evicts = warnsSince(w0, "pending: evicting");
  CHECK(evicts <= 1, "50 frames to one silent peer in 10 s logged %zu evictions", evicts);
  // Four silent peers time out together.
  for (uint8_t i = 0; i < 4; ++i) {
    uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x55, i};
    if (i == 0) std::memcpy(mac, other, 6);
    v4_send_encrypted_or_queue(mac, 1, 0, ++gMsgId, nullptr, 0, 1, nullptr, 0);
  }
  const size_t w1 = gWarnLines.size();
  gNowMs += 6000;
  pendingFrameTimeoutSweep(gNowMs);
  CHECK(warnsSince(w1, "pending: timeout") <= 1, "4 timeouts at once logged %zu lines",
        warnsSince(w1, "pending: timeout"));
  gNowMs += 61000;
  v4_send_encrypted_or_queue(other, 1, 0, ++gMsgId, nullptr, 0, 1, nullptr, 0);
  const size_t w2 = gWarnLines.size();
  gNowMs += 6000;
  pendingFrameTimeoutSweep(gNowMs);
  bool carries = false;
  for (size_t i = w2; i < gWarnLines.size(); ++i) {
    carries = carries || gWarnLines[i].find("(3 more in the last minute)") != std::string::npos;
  }
  CHECK(carries, "the next timeout warning does not carry the 3 held back");
}

// Unpair (finding X4, round 3): cmd_espnow_unpair calls off the key exchange
// this device started with the peer and drops the frames queued for its
// session before it forgets the identity. A HELLO retry would otherwise bring
// the identity back after the unpair. No heartbeats run here: the bond with
// the peer ended first.
static void caseUnpairCallsOff() {
  resetWorld();
  const uint8_t other[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x05};
  gNextHeartbeatMs = gNowMs + 0x40000000u;   // no heartbeats
  v4_send_encrypted_or_queue(kPartner, 1, 0, ++gMsgId, nullptr, 0, 1, nullptr, 0);
  v4_send_encrypted_or_queue(other, 1, 0, ++gMsgId, nullptr, 0, 1, nullptr, 0);
  CHECK(keyExIsInFlight(kPartner) && pendingFrameHasForPeer(kPartner) && pendingFrameHasForPeer(other) &&
        gPartnerHellos == 1, "setup: a key exchange and a queued frame for the partner and another peer");
  const size_t w0 = gWarnLines.size();
  espnowKeyExCancel(kPartner);
  const uint8_t dropped = pendingFrameDropForPeer(kPartner);
  CHECK(dropped == 1 && !pendingFrameHasForPeer(kPartner) && pendingFrameHasForPeer(other) && gMarkedFailed == 1,
        "X4 unpair: the partner's queued frame was not dropped and reported failed alone "
        "(dropped %u, failed %d)", (unsigned)dropped, gMarkedFailed);
  CHECK(!keyExIsInFlight(kPartner), "X4 a key exchange called off by an unpair still counts as in flight");
  runFor(1600);
  CHECK(gPartnerHellos == 1, "X4 the HELLO to the unpaired peer was re-sent %d times", gPartnerHellos - 1);
  bool entryLeft = false;
  for (const auto& k : gKeyExInFlight) entryLeft = entryLeft || (k.inUse && std::memcmp(k.peerMac, kPartner, 6) == 0);
  CHECK(!entryLeft, "X4 the called-off key exchange still holds its table entry one retry interval later");
  runFor(6000);
  CHECK(gPartnerHellos == 1 && warnsSince(w0, "KEY_EX: no REPLY") == 1,
        "X4 after the unpair: %d HELLO retries to the peer and %zu give-up warnings (want 0 and 1, the other peer's)",
        gPartnerHellos - 1, warnsSince(w0, "KEY_EX: no REPLY"));
  // A new key exchange right after the unpair (pairing again) starts at once
  // and is retried as usual.
  resetWorld();
  gNextHeartbeatMs = gNowMs + 0x40000000u;   // no heartbeats
  espnowKeyExInitiate(kPartner, nullptr);
  espnowKeyExCancel(kPartner);
  CHECK(espnowKeyExInitiate(kPartner, nullptr) && keyExIsInFlight(kPartner) && gPartnerHellos == 2,
        "a key exchange started after an unpair did not start at once");
  runFor(1600);
  CHECK(gPartnerHellos == 3, "a key exchange started after an unpair is not retried (%d HELLOs)", gPartnerHellos);
  // Nothing to call off: no effect.
  resetWorld();
  espnowKeyExCancel(kPartner);
  CHECK(pendingFrameDropForPeer(kPartner) == 0 && gMarkedFailed == 0, "calling off nothing changed something");
}

int main() {
  caseSchedule();
  caseTableAndHeard();
  caseAbsentPartner();
  caseHeardNeverAnswers();
  caseAnsweringPartner();
  caseKnownPartnerOff();
  caseRequestStillQueued();
  casePendingWarnings();
  caseSensorDataDropped();
  caseBondPowerEventsDropped();
  caseUserStartsReported();
  caseUnpairCallsOff();
  std::printf("espnow_handshake_backoff: %d checks, %d failed\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
