#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#ifdef NDEBUG
#error "Host checks require assertions"
#endif
using String = std::string;
// INSERT_TYPES
struct G2Temple {
  bool connected = false;
  String deviceAddress;
  uint8_t deviceAddressType = 0;
  bool deviceAddressTypeKnown = false;
};
static G2Temple gL, gR;
static BlePeerData gBlePeerData[BLE_PEER_MAX];
static PeerReconnectRuntime sReconnect[BLE_PEER_MAX];
static bool dataGuardAvailable = true;
static unsigned dataDepth = 0, muxDepth = 0;
static int sReconnectMux = 0;
static uint32_t clockMs = 100;
static uint32_t millis() { return clockMs; }
struct PeerDataGuard {
  bool held = dataGuardAvailable;
  PeerDataGuard() { if (held) ++dataDepth; }
  ~PeerDataGuard() { if (held) --dataDepth; }
  explicit operator bool() const { return held; }
};
#define portENTER_CRITICAL(mux) do { (void)(mux); assert(dataDepth == 1 && muxDepth == 0); ++muxDepth; } while (0)
#define portEXIT_CRITICAL(mux) do { (void)(mux); assert(muxDepth == 1); --muxDepth; } while (0)
static size_t hostStrlcpy(char* out, const char* text, size_t cap) {
  const size_t n = strlen(text), used = cap ? (n < cap ? n : cap - 1) : 0;
  if (cap) { memcpy(out, text, used); out[used] = 0; }
  return n;
}
#define strlcpy hostStrlcpy
// INSERT_PRODUCTION

static BlePeerSavedTarget savedPair() {
  BlePeerSavedTarget saved{};
  strcpy(saved.mac1, "c0:11:22:33:44:01");
  strcpy(saved.mac2, "c0:11:22:33:44:02");
  return saved;
}

static void seedRegistry(const BlePeerSavedTarget& saved) {
  auto& data = gBlePeerData[BLE_PEER_G2_GLASSES];
  data = {}; data.mac1 = saved.mac1; data.mac2 = saved.mac2;
  auto& state = sReconnect[BLE_PEER_G2_GLASSES];
  state = {}; state.savedTarget = saved;
  state.intentGeneration = 17; state.identityGeneration = 23;
  state.ownerAuthorityAvailable = true; state.autoReconnect = true;
  state.hasSavedTarget = saved.mac1[0] || saved.mac2[0];
}

static uint8_t applyLearned(G2Eye eye, BlePeerSavedTarget& saved) {
  seedRegistry(saved);
  G2LinkSnapshot after{};
  after.left = gL.connected;
  after.right = gR.connected;
  const uint8_t mask = g2LearnedTargetReplaceMask(eye, after, saved);
  bool persist = false;
  assert(blePeerCommitLearnedTargetIfCurrent(BLE_PEER_G2_GLASSES, 17, 23,
      (mask & 1) && after.left ? gL.deviceAddress : String(),
      (mask & 2) && after.right ? gR.deviceAddress : String(), mask,
      after.left && after.right, &persist));
  const auto& state = sReconnect[BLE_PEER_G2_GLASSES];
  const auto& data = gBlePeerData[BLE_PEER_G2_GLASSES];
  assert(data.mac1 == state.savedTarget.mac1 && data.mac2 == state.savedTarget.mac2);
  assert(state.wantReconnect == !(after.left && after.right));
  assert(state.dueMs == (after.left && after.right ? 0 : clockMs + 5000));
  assert(!dataDepth && !muxDepth);
  saved = state.savedTarget;
  return mask;
}

static bool ringIdentity(const BlePeerSavedTarget& saved) {
  uint8_t liveL[6]{}, liveR[6]{}, right[6]{}, left[6]{};
  if (gL.connected) assert(macParse(gL.deviceAddress.c_str(), liveL));
  if (gR.connected) assert(macParse(gR.deviceAddress.c_str(), liveR));
  return ringResolveAdvIdentity(gR.connected, liveR, gL.connected, liveL,
                                 saved.mac2, saved.mac1, right, left, nullptr);
}

int main() {
  // Both one-arm return paths retain the admitted mate and satisfy R1 setup.
  for (bool left : {false, true}) {
    auto saved = savedPair();
    gL = {}; gR = {};
    G2Temple& live = left ? gL : gR;
    live.connected = true;
    live.deviceAddress = left ? "C0:11:22:33:44:01" : saved.mac2;
    assert(applyLearned(G2_EYE_AUTO, saved) == (left ? 1 : 2));
    assert(saved.mac1[0] && saved.mac2[0] && ringIdentity(saved));
    // Another failed/partial scan must not gradually erase the pair.
    assert(applyLearned(G2_EYE_AUTO, saved) == (left ? 1 : 2));
    assert(ringIdentity(saved));
  }
  // A new/different arm cannot inherit the previous pair's unseen mate.
  for (bool left : {false, true}) {
    auto saved = savedPair();
    gL = {}; gR = {};
    G2Temple& live = left ? gL : gR;
    live.connected = true; live.deviceAddress = "c0:99:88:77:66:55";
    assert(applyLearned(G2_EYE_AUTO, saved) == 3);
    assert(left ? !saved.mac2[0] : !saved.mac1[0]);
    assert(!ringIdentity(saved));
  }
  // Known address-type mismatch, or an unobserved required type, is not proof
  // that the arm belongs to the admitted identity, even if text MACs match.
  for (bool typeKnown : {false, true}) {
    auto saved = savedPair();
    saved.addressType1Known = true; saved.addressType1 = 1;
    gR = {}; gL = {}; gL.connected = true; gL.deviceAddress = saved.mac1;
    gL.deviceAddressTypeKnown = typeKnown; gL.deviceAddressType = 0;
    assert(applyLearned(G2_EYE_AUTO, saved) == 3 && !saved.mac2[0]);
  }
  auto saved = savedPair();
  saved.addressType1Known = true; saved.addressType1 = 1;
  gL = {}; gR = {}; gL.connected = true; gL.deviceAddress = saved.mac1;
  gL.deviceAddressTypeKnown = true; gL.deviceAddressType = 1;
  assert(applyLearned(G2_EYE_AUTO, saved) == 1 && ringIdentity(saved));
  // Both observed arms replace both slots; no old address survives.
  gR.connected = true; gR.deviceAddress = "c0:99:88:77:66:22";
  gL.deviceAddress = "c0:99:88:77:66:11";
  assert(applyLearned(G2_EYE_AUTO, saved) == 3 && ringIdentity(saved));
  // Explicit single-side pairing keeps its existing authority semantics.
  assert(applyLearned(G2_EYE_LEFT, saved) == 1);
  assert(applyLearned(G2_EYE_RIGHT, saved) == 2);
  // No prior pair is invented when only one side has ever been learned.
  saved = {}; gR = {};
  assert(applyLearned(G2_EYE_AUTO, saved) == 3 && !ringIdentity(saved));
  // The registry's actual rollback path refuses stale or revoked completions;
  // neither persisted Strings nor the fixed-size reconnect identity may change.
  for (unsigned refusal = 0; refusal < 5; ++refusal) {
    saved = savedPair(); seedRegistry(saved);
    auto& state = sReconnect[BLE_PEER_G2_GLASSES];
    if (refusal == 0) ++state.intentGeneration;
    if (refusal == 1) ++state.identityGeneration;
    if (refusal == 2) state.userDisconnect = true;
    if (refusal == 3) state.ownerAuthorityAvailable = false;
    if (refusal == 4) dataGuardAvailable = false;
    bool persist = true;
    assert(!blePeerCommitLearnedTargetIfCurrent(BLE_PEER_G2_GLASSES, 17, 23,
        "c0:99:88:77:66:55", "", 3, false, &persist));
    assert(!persist && savedTargetMatches(saved, state.savedTarget));
    assert(gBlePeerData[BLE_PEER_G2_GLASSES].mac1 == saved.mac1);
    assert(gBlePeerData[BLE_PEER_G2_GLASSES].mac2 == saved.mac2);
    assert(!dataDepth && !muxDepth); dataGuardAvailable = true;
  }
  // Representability failures also roll the settings Strings back atomically.
  saved = savedPair(); seedRegistry(saved);
  bool persist = true;
  assert(!blePeerCommitLearnedTargetIfCurrent(BLE_PEER_G2_GLASSES, 17, 23,
      String(40, 'a'), "", 3, false, &persist));
  assert(!persist && savedTargetMatches(saved, sReconnect[BLE_PEER_G2_GLASSES].savedTarget));
  assert(gBlePeerData[BLE_PEER_G2_GLASSES].mac1 == saved.mac1);
  assert(gBlePeerData[BLE_PEER_G2_GLASSES].mac2 == saved.mac2);
  // Preserve both saved address types when only the admitted left is observed.
  saved = savedPair(); saved.addressType1Known = saved.addressType2Known = true;
  saved.addressType1 = saved.addressType2 = 1;
  gL = {}; gR = {}; gL.connected = true; gL.deviceAddress = saved.mac1;
  gL.deviceAddressTypeKnown = true; gL.deviceAddressType = 1;
  assert(applyLearned(G2_EYE_AUTO, saved) == 1);
  assert(saved.addressType2Known && saved.addressType2 == 1 && ringIdentity(saved));
  assert(!dataDepth && !muxDepth);
  puts("G2 saved pair: production AUTO policy + registry commit preserve R1 identity; stale/unauthorized/oversized commits roll back; new pairs fail closed");
}
