// Host harness for test_espnow_chunked_send.py: the production encrypted
// multi-frame sender (System_ESPNow.cpp v4_send_chunked_attempt and
// v4_send_encrypted_chunked, 1.0 step 5b) against a scripted receiver. The
// runner pastes the production code at the INSERT markers. Doubles: the clock
// (vTaskDelay advances it), one peer with one session, the radio (each emitted
// fragment is answered by the script through the fragment waiter, as the RX
// ACK handler would), the message-id counter, and the session-liveness and
// tracked-send hooks (recorded).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <functional>
#include <vector>

#include "System_ESPNow_AnswerCore.h"

#define EXT_RAM_BSS_ATTR
#define WARN_ESPNOWF(...) ((void)0)
#define DEBUG_ESPNOWF(...) ((void)0)
#define DEBUG_ESPNOW_ROUTERF(...) ((void)0)
#define V4_FRAG_ACK_WAIT_MAX 8

// INSERT_WIRE

static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }
#define pdMS_TO_TICKS(ms) (ms)
static std::function<void()> gOnTick;  // the script's clock hook (rekey applies, ...)
static void vTaskDelay(uint32_t ticks) {
  gNowMs += ticks ? ticks : 1;
  if (gOnTick) gOnTick();
}
static uint32_t esp_random() { return 0; }  // the shortest jitter: 300 ms

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
struct EspNowRuntimeOpGuard { bool held() const { return true; } };
struct EspNowTxGuard { explicit EspNowTxGuard(const char*) {} };
namespace espnowtx { static void drainAcksOnDispatcher() {} }

static const uint8_t kPeer[6] = {0x02, 0xB0, 0, 0, 0, 0x0B};
static void espnowSelfStaMac(uint8_t out[6]) { const uint8_t m[6] = {0x02, 0xA0, 0, 0, 0, 0x0A}; memcpy(out, m, 6); }
static void macToDisplay(const uint8_t*, char* out, size_t n) { snprintf(out, n, "peer"); }
static bool meshRouterIsRouted(const uint8_t*) { return false; }
static uint16_t v4_single_frame_cap(const uint8_t*) { return 202; }
static uint16_t fingerprintForPeer(const uint8_t*) { return 0x1234; }
static void captureEspNowFrame(const char*, const uint8_t*, int, const uint8_t*, int) {}

struct PeerIdentity { uint16_t meshId; };
static PeerIdentity gPid{1};
static const PeerIdentity* peerIdentityFindByMac(const uint8_t*) { return &gPid; }
enum SessionStateKind : uint8_t { SESSION_IDLE = 0, SESSION_ESTABLISHING, SESSION_ACTIVE, SESSION_REKEYING };
struct SessionState { uint8_t state; };
static SessionState gSess{SESSION_ACTIVE};
static SessionState* sessionFindByPeer(const uint8_t*, uint16_t) { return &gSess; }
// The script can start a rekey on this device after N seals.
static int gSealsBeforeRekey = -1;
static unsigned gSeals = 0;
static bool sessionWrapFrame(SessionState* s, EspNowV4Header*, const uint8_t* in, uint16_t len, uint8_t* out) {
  if (gSealsBeforeRekey >= 0 && (int)gSeals == gSealsBeforeRekey) s->state = SESSION_REKEYING;
  if (s->state != SESSION_ACTIVE) return false;
  ++gSeals;
  memcpy(out, in, len);
  return true;
}
static bool v4_send_session_wrapped(const uint8_t*, uint8_t, uint16_t, uint32_t, const uint8_t*, uint16_t, uint8_t,
                                    char*, size_t) { return true; }

struct RouterMetrics { uint32_t v4FragTx = 0; };
struct EspNowState { RouterMetrics routerMetrics; };
static EspNowState gState;
static EspNowState* gEspNow = &gState;

static uint32_t gNextId = 9000;
static uint32_t generateMessageId() { return gNextId++; }

// INSERT_OUTCOME

static std::vector<V4SendOutcome> gNoted;
static void sessionNoteSendOutcome(const uint8_t*, V4SendOutcome o) { gNoted.push_back(o); }
static std::vector<uint32_t> gDelivered, gRefused;
static void sendStatusMarkDelivered(uint32_t msgId, const uint8_t*) { gDelivered.push_back(msgId); }
static std::vector<uint8_t> gRefusedStatus;
static void sendStatusMarkRefused(uint32_t msgId, const uint8_t*, uint8_t status) {
  gRefused.push_back(msgId);
  gRefusedStatus.push_back(status);
}
// The remote-command requests follow a whole-message retry (1.0 step 6).
static std::vector<std::pair<uint32_t, uint32_t>> gRenamed;
static void remoteCmdRenamed(const uint8_t*, uint32_t oldId, uint32_t newId) { gRenamed.emplace_back(oldId, newId); }

// INSERT_ACK_WAIT

// The radio and the scripted receiver: each emitted fragment is recorded; the
// script answers it with a status (-1: no ACK), through the waiter as the RX
// ACK handler marks it.
struct Sent { uint32_t msgId; uint8_t fragIndex, fragCount; uint32_t atMs; };
static std::vector<Sent> gSent;
static std::function<int(const Sent&)> gReceiver;
static bool gRadioFails = false;  // every emit fails locally (nothing leaves)
static bool v4_radio_emit(const uint8_t*, const uint8_t* frame, size_t, bool) {
  if (gRadioFails) return false;
  EspNowV4Header h;
  memcpy(&h, frame, sizeof h);
  const Sent s{h.msgId, h.fragIndex, h.fragCount, gNowMs};
  gSent.push_back(s);
  const int status = gReceiver ? gReceiver(s) : 0;
  if (status >= 0) {
    for (auto& w : gV4FragAckWait) {
      if (w.active && w.msgId == s.msgId && w.fragIndex == s.fragIndex) {
        w.status = (uint8_t)status;
        w.acked = true;
      }
    }
  }
  return true;
}

V4SendResult v4_send_encrypted_chunked(const uint8_t dst[6], uint8_t type, uint16_t baseFlags, uint32_t msgId,
                                       const uint8_t* payload, uint16_t payloadLen, uint8_t ttl,
                                       bool ackSingle = false);
// INSERT_SENDER

static int gFailures = 0, gChecks = 0;
static void check(const char* what, bool ok) {
  ++gChecks;
  if (!ok) ++gFailures;
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
static void reset() {
  gSent.clear();
  gNoted.clear();
  gDelivered.clear();
  gRefused.clear();
  gRefusedStatus.clear();
  gRenamed.clear();
  gReceiver = nullptr;
  gOnTick = nullptr;
  gSess.state = SESSION_ACTIVE;
  gSealsBeforeRekey = -1;
  gSeals = 0;
  gRadioFails = false;
  memset(gV4FragAckWait, 0, sizeof gV4FragAckWait);
  gNowMs += 10000;
}
static unsigned attemptsOf(uint32_t msgId) {
  unsigned n = 0;
  for (const auto& s : gSent) n += (s.msgId == msgId && s.fragIndex == 0) ? 1 : 0;
  return n;
}
static const uint8_t kPayload[500] = {};  // three fragments: 200, 200, 100
static constexpr uint32_t kId = 500;

int main() {
  // K1: every fragment accepted.
  reset();
  V4SendResult r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K1 all fragments accepted: Delivered after one attempt of three fragments",
        r.outcome == V4SendOutcome::Delivered && gSent.size() == 3);
  check("K1 the session hears Delivered and the tracked send is marked by the caller's msgId",
        gNoted.size() == 1 && gNoted[0] == V4SendOutcome::Delivered && gDelivered == std::vector<uint32_t>{kId});

  // K2: busy on the last fragment, then the whole message again under a new msgId.
  reset();
  gReceiver = [](const Sent& s) {
    return (s.msgId == kId && s.fragIndex == 2) ? (int)espnowanswer::kBusy : 0;
  };
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K2 busy on the last fragment: the whole message is sent once more and Delivered",
        r.outcome == V4SendOutcome::Delivered && gSent.size() == 6 && attemptsOf(kId) == 1);
  check("K2 the second attempt has a new msgId and waits at least 300 ms",
        gSent[3].msgId != kId && gSent[3].fragIndex == 0 && gSent[3].atMs - gSent[2].atMs >= 300);
  check("K2 the tracked send is marked delivered by the caller's msgId",
        gDelivered == std::vector<uint32_t>{kId} && gNoted.size() == 1 && gNoted[0] == V4SendOutcome::Delivered);

  // K3: busy twice: Refused(busy), exactly two attempts; the session is alive.
  reset();
  gReceiver = [](const Sent& s) { return s.fragIndex == 0 ? (int)espnowanswer::kBusy : 0; };
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K3 busy twice: Refused(kBusy) after exactly two attempts",
        r.outcome == V4SendOutcome::Refused && r.status == espnowanswer::kBusy && gSent.size() == 2);
  check("K3 a busy refusal tells the session it is alive (Refused), and marks the tracked send busy (its status)",
        gNoted.size() == 1 && gNoted[0] == V4SendOutcome::Refused && gRefused == std::vector<uint32_t>{kId} &&
            gRefusedStatus == std::vector<uint8_t>{espnowanswer::kBusy});
  check("K3 a tracked request follows the retry to its new msgId (remote commands, 1.0 step 6)",
        gRenamed.size() == 1 && gRenamed[0].first == kId && gRenamed[0].second == gSent[1].msgId);

  // K4: a policy refusal is final: no second attempt.
  reset();
  gReceiver = [](const Sent& s) { return s.fragIndex == 1 ? (int)espnowanswer::kRefused : 0; };
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K4 kRefused stops the send at that fragment, with no second attempt",
        r.outcome == V4SendOutcome::Refused && r.status == espnowanswer::kRefused && gSent.size() == 2);

  // K5: no answer: three tries of the first fragment, NoAnswer, one strike.
  reset();
  gReceiver = [](const Sent&) { return -1; };
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K5 no ACK: NoAnswer after three tries of the first fragment, no whole-message retry",
        r.outcome == V4SendOutcome::NoAnswer && gSent.size() == 3 && attemptsOf(kId) == 3);
  check("K5 only silence counts toward the re-open: the session hears NoAnswer",
        gNoted.size() == 1 && gNoted[0] == V4SendOutcome::NoAnswer && gDelivered.empty() && gRefused.empty());

  // K6: this device starts a rekey after the second fragment; it applies 60 ms later.
  reset();
  gSealsBeforeRekey = 2;
  const uint32_t rekeyAt = gNowMs;
  gOnTick = [rekeyAt]() {
    if (gSess.state == SESSION_REKEYING && gNowMs - rekeyAt >= 60) {
      gSess.state = SESSION_ACTIVE;
      gSealsBeforeRekey = -1;
    }
  };
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K6 a rekey started mid-send: the message is sent again on the new keys under a new msgId (5a: aborted)",
        r.outcome == V4SendOutcome::Delivered && gSent.size() == 5 && gSent[2].msgId != kId &&
            gSent[2].fragIndex == 0);

  // K7: the rekey does not apply within the wait: NotSent, nothing counted.
  reset();
  gSealsBeforeRekey = 1;
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K7 a rekey that does not apply within 1 s: NotSent, counted toward nothing",
        r.outcome == V4SendOutcome::NotSent && gSent.size() == 1 && gNoted.size() == 1 &&
            gNoted[0] == V4SendOutcome::NotSent && gDelivered.empty() && gRefused.empty());

  // K8: no slot on the first fragment, then a slot: the second attempt is stored.
  reset();
  gReceiver = [](const Sent& s) { return s.msgId == kId ? (int)espnowanswer::kNoSlot : 0; };
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K8 kNoSlot: the message is sent once more (new msgId) and Delivered",
        r.outcome == V4SendOutcome::Delivered && gSent.size() == 4 && gSent[1].msgId != kId);

  // K9: no copy of the first fragment leaves the radio: a local failure,
  // NotSent, no strike on the session.
  reset();
  gRadioFails = true;
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kPayload, sizeof kPayload, 1);
  check("K9 the radio refuses every copy: NotSent, counted toward nothing (not a session strike)",
        r.outcome == V4SendOutcome::NotSent && gSent.empty() && gNoted.size() == 1 &&
            gNoted[0] == V4SendOutcome::NotSent);

  // K10: a long TEXT is stored per fragment on the receiver: busy on its last
  // fragment is final (no second copy under a new msgId).
  reset();
  gReceiver = [](const Sent& s) { return s.fragIndex == 2 ? (int)espnowanswer::kBusy : 0; };
  r = v4_send_encrypted_chunked(kPeer, ESPNOW_V4_TYPE_TEXT, 0, kId, kPayload, sizeof kPayload, 1);
  check("K10 a long TEXT refused busy is not sent again as a whole (Refused after one attempt)",
        r.outcome == V4SendOutcome::Refused && r.status == espnowanswer::kBusy && gSent.size() == 3 &&
            gRefused == std::vector<uint32_t>{kId});
  reset();
  gSealsBeforeRekey = 1;
  r = v4_send_encrypted_chunked(kPeer, ESPNOW_V4_TYPE_TEXT, 0, kId + 1, kPayload, sizeof kPayload, 1);
  check("K10 a long TEXT cut by this device's rekey is not sent again either (NotSent)",
        r.outcome == V4SendOutcome::NotSent && gSent.size() == 1);

  // K11 (1.0 step 6): a CMD answered busy ends Busy at once (DESIGN 3.5): no
  // second attempt, no request renamed.
  reset();
  gReceiver = [](const Sent& s) { return s.fragIndex == 0 ? (int)espnowanswer::kBusy : 0; };
  r = v4_send_encrypted_chunked(kPeer, ESPNOW_V4_TYPE_CMD, 0, kId, kPayload, sizeof kPayload, 1);
  check("K11 a long CMD refused busy is final: one attempt, Refused(busy)",
        r.outcome == V4SendOutcome::Refused && r.status == espnowanswer::kBusy && gSent.size() == 1 &&
            gRenamed.empty());

  // K12: a one-frame CMD_RESP with ackSingle: one frame (fragment 1 of 1) that
  // waits for its ACK; a requester that is busy gets the whole result once
  // more under a new msgId (its result queue was full).
  static const uint8_t kShort[40] = {};
  reset();
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kShort, sizeof kShort, 1, /*ackSingle=*/true);
  check("K12 ackSingle: one frame, fragment 1 of 1, Delivered on its ACK",
        r.outcome == V4SendOutcome::Delivered && gSent.size() == 1 && gSent[0].fragCount == 1 &&
            gSent[0].fragIndex == 0 && gSent[0].msgId == kId);
  reset();
  gReceiver = [](const Sent& s) { return s.msgId == kId ? (int)espnowanswer::kBusy : 0; };
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kShort, sizeof kShort, 1, true);
  check("K12 ackSingle busy: sent once more under a new msgId after 300-800 ms, Delivered",
        r.outcome == V4SendOutcome::Delivered && gSent.size() == 2 && gSent[1].msgId != kId &&
            gSent[1].atMs - gSent[0].atMs >= 300 && gSent[1].atMs - gSent[0].atMs <= 900);
  reset();
  gReceiver = [](const Sent&) { return -1; };
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kShort, sizeof kShort, 1, true);
  check("K12 ackSingle no ACK: three tries, NoAnswer (one session strike)",
        r.outcome == V4SendOutcome::NoAnswer && gSent.size() == 3 && gNoted.size() == 1 &&
            gNoted[0] == V4SendOutcome::NoAnswer);
  reset();
  r = v4_send_encrypted_chunked(kPeer, 51, 0, kId, kShort, sizeof kShort, 1);
  check("K12 without ackSingle a one-frame payload is not waited for (the tracked-send path)",
        r.outcome == V4SendOutcome::Delivered && gSent.empty());

  printf("espnow_chunked_send: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
