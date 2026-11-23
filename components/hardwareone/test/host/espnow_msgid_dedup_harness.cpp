// Host harness for test_espnow_msgid_dedup.py: this device's message numbers
// (System_ESPNow_MsgIdCore.h and the production generateMessageId) against the
// production receive duplicate ring (v4_dedup_ring_seen_and_insert). The runner
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
static constexpr int kRing = 64;
static V4DedupEntry gRing[kRing];
static int gRingIdx = 0;

static void ringReset() { memset(gRing, 0, sizeof(gRing)); gRingIdx = 0; }
static bool seen(uint32_t id, uint8_t frag = 0) {
  return v4_dedup_ring_seen_and_insert(gRing, kRing, gRingIdx, V4DedupClass::Sealed, kSender, id, frag);
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

  printf("espnow msgid dedup: %d/%d passed\n", gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
