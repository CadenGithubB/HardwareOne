// Host harness for test_espnow_msgid_dedup.py: this device's message numbers
// (System_ESPNow_MsgIdCore.h and the production generateMessageId) against the
// production receive accept memory (v4_accept_lookup / v4_accept_record, the
// classed duplicate ring with the verdict of 1.0 step 5b). The runner
// pastes the production code at the INSERT markers; the doubles are a clock and
// an EspNowState holding only the counter.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <set>
#include <vector>

static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }

// INSERT_MSGID_CORE

struct EspNowState { uint32_t nextMessageId = 1; };
static EspNowState gState;
static EspNowState* gEspNow = &gState;

// INSERT_GENERATE

// INSERT_DEDUP_TYPES

// INSERT_DEDUP_TTL

// INSERT_DEDUP_RING

static int gFailures = 0, gChecks = 0;
#define CHECK(cond, ...) do { ++gChecks; if (!(cond)) { ++gFailures; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
                              else { printf("PASS: " __VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t kSender[6] = {0x02, 0, 0, 0, 0, 0x11};
static constexpr int kRing = 128;
static V4DedupEntry gRing[kRing];
static int gRingIdx = 0;

static void ringReset() { memset(gRing, 0, sizeof(gRing)); gRingIdx = 0; }
static V4AcceptKey key(V4DedupClass cls = V4DedupClass::Sealed) {
  return V4AcceptKey{gRing, kRing, &gRingIdx, cls, kSender};
}
// The receive path for an accepted frame: a copy is known, a new one is
// processed and remembered as accepted.
static bool seen(uint32_t id, uint8_t frag = 0) {
  if (v4_accept_lookup(key(), id, frag) != V4AcceptMemo::None) return true;
  v4_accept_record(key(), id, frag, V4AcceptMemo::Accepted);
  return false;
}

int main() {
  // M1: the seed is any value but 0.
  CHECK(espnowmsgid::seed(0) == 1 && espnowmsgid::seed(1) == 1 && espnowmsgid::seed(0xDEADBEEFu) == 0xDEADBEEFu &&
        espnowmsgid::seed(0xFFFFFFFFu) == 0xFFFFFFFFu, "M1 seed: 0 becomes 1, any other value is kept");

  // M2: the production generateMessageId hands out the counter and skips 0 on wrap.
  gState.nextMessageId = espnowmsgid::seed(0xFFFFFFFDu);
  uint32_t got[5];
  for (uint32_t& g : got) g = generateMessageId();
  CHECK(got[0] == 0xFFFFFFFDu && got[1] == 0xFFFFFFFEu && got[2] == 0xFFFFFFFFu && got[3] == 1 && got[4] == 2,
        "M2 wrap: FFFFFFFD FFFFFFFE FFFFFFFF 1 2 (got %08X %08X %08X %08X %08X)",
        got[0], got[1], got[2], got[3], got[4]);
  gState.nextMessageId = 0;  // never seeded
  CHECK(generateMessageId() == 1 && generateMessageId() == 2, "M2 an unseeded counter (0) starts at 1");
  {
    EspNowState* saved = gEspNow;
    gEspNow = nullptr;
    CHECK(generateMessageId() == 0, "M2 without ESP-NOW state the answer is 0 (callers treat it as no id)");
    gEspNow = saved;
  }

  // D1: a random start near the top, 300 frames across the wrap: every frame is
  // new to the receiver, none is 0, and a radio re-delivery of each is caught.
  ringReset();
  gState.nextMessageId = espnowmsgid::seed(0xFFFFFF80u);
  int fresh = 0, zero = 0, dupCaught = 0;
  std::set<uint32_t> ids;
  for (int i = 0; i < 300; i++) {
    const uint32_t id = generateMessageId();
    ids.insert(id);
    if (id == 0) { zero++; continue; }
    if (!seen(id)) fresh++;
    if (seen(id)) dupCaught++;   // the same frame again, within the TTL
    gNowMs += 5;
  }
  CHECK(zero == 0 && fresh == 300 && dupCaught == 300 && ids.size() == 300,
        "D1 300 frames from a random start across the wrap: %d new, %d duplicates caught, %d zero, %zu distinct",
        fresh, dupCaught, zero, ids.size());

  // D2: fragments of one message share the msgId and are all new.
  ringReset();
  const uint32_t group = generateMessageId();
  bool allNew = true;
  for (uint8_t f = 0; f < 4; f++) allNew = allNew && !seen(group, f);
  CHECK(allNew && seen(group, 2), "D2 four fragments of one msgId are new, a repeated fragment is a duplicate");

  // D3: a sender that restarts within the TTL. Before 1.0 every start began at
  // 1, so the restarted sender's first frames carried numbers the receiver still
  // held and were dropped. With a random start they are new.
  ringReset();
  gState.nextMessageId = espnowmsgid::seed(1);  // the old fixed start
  for (int i = 0; i < 20; i++) seen(generateMessageId());
  gNowMs += 100;
  gState.nextMessageId = espnowmsgid::seed(1);  // restart, old behaviour
  int shadowed = 0;
  for (int i = 0; i < 20; i++) shadowed += seen(generateMessageId()) ? 1 : 0;
  CHECK(shadowed == 20, "D3 control: a restart from the same start loses its first 20 frames (%d)", shadowed);
  ringReset();
  gState.nextMessageId = espnowmsgid::seed(0x1A2B3C4Du);
  for (int i = 0; i < 20; i++) seen(generateMessageId());
  gNowMs += 100;
  gState.nextMessageId = espnowmsgid::seed(0x9E3779B9u);  // restart, new random start
  shadowed = 0;
  for (int i = 0; i < 20; i++) shadowed += seen(generateMessageId()) ? 1 : 0;
  CHECK(shadowed == 0, "D3 a restart from a new random start loses nothing (%d shadowed)", shadowed);

  // D4: after the TTL an old number is new again (expiry, unchanged).
  ringReset();
  CHECK(!seen(77) && seen(77), "D4 setup: 77 new, then a duplicate");
  gNowMs += V4_DEDUP_TTL_MS + 1;
  CHECK(!seen(77), "D4 after the TTL the same number is accepted again");

  // A1-A5: the accept memory (1.0 step 5b, DESIGN 3.1 G4).
  ringReset();
  v4_accept_record(key(), 500, 0, V4AcceptMemo::Accepted);
  v4_accept_record(key(), 501, 0, V4AcceptMemo::Silent);
  CHECK(v4_accept_lookup(key(), 500, 0) == V4AcceptMemo::Accepted &&
        v4_accept_lookup(key(), 501, 0) == V4AcceptMemo::Silent,
        "A1 an accepted frame is remembered as accepted (re-ACKed), a silent one as silent (a retry stays silent)");
  // A busy refusal is never recorded (the receive path records only Accepted
  // and Silent): the retry finds nothing and is processed, then remembered.
  CHECK(v4_accept_lookup(key(), 502, 0) == V4AcceptMemo::None, "A2 a refused frame leaves no entry");
  v4_accept_record(key(), 502, 0, V4AcceptMemo::Accepted);  // the retry, accepted
  CHECK(v4_accept_lookup(key(), 502, 0) == V4AcceptMemo::Accepted, "A2 its retry is processed and then remembered");
  CHECK(v4_accept_lookup(key(V4DedupClass::Group), 500, 0) == V4AcceptMemo::None &&
        v4_accept_lookup(key(), 500, 1) == V4AcceptMemo::None,
        "A3 the class and the fragment are part of the key");
  v4_accept_record(key(), 503, 2, V4AcceptMemo::Silent);
  gNowMs += V4_DEDUP_TTL_MS + 1;
  CHECK(v4_accept_lookup(key(), 503, 2) == V4AcceptMemo::None, "A4 an entry past the TTL is forgotten");
  const int before = gRingIdx;
  v4_accept_record(key(), 503, 2, V4AcceptMemo::Accepted);
  CHECK(gRingIdx == before && v4_accept_lookup(key(), 503, 2) == V4AcceptMemo::Accepted,
        "A4 recording it again refreshes the same entry with the new verdict");
  // A5: every stored fragment of a 21-fragment message is remembered, so a
  // fragment sent again after the message completed is answered from here.
  ringReset();
  for (uint8_t f = 0; f < 21; f++) v4_accept_record(key(), 600, f, V4AcceptMemo::Accepted);
  bool allKnown = true;
  for (uint8_t f = 0; f < 21; f++) allKnown = allKnown && v4_accept_lookup(key(), 600, f) == V4AcceptMemo::Accepted;
  CHECK(allKnown, "A5 the fragments of a completed 21-fragment message are all remembered");
  static_assert(sizeof(V4DedupEntry) == 20, "the verdict sits in the entry's tail padding");

  printf("espnow msgid dedup: %d/%d passed\n", gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
