// Shipping definitions are inserted by test_g2_repair_schedule.py: the repair
// schedule (G2_RepairCore.h, included as is) and, from G2_Glasses.cpp, the
// heartbeat tick, the queue path, the worker's finish hook, the wear/gesture
// nudge, the status text, the ladder's admission and g2recover. The clock,
// locks, links, peer intent and the setup worker are host fakes.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "G2_RepairCore.h"

using String = std::string;
static unsigned assertions = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    ++assertions;                                                             \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

// Clock and the one lock. The repair lock nests nothing, and nothing logs,
// pushes or queues while it is held.
static uint32_t gNow = 0;
static uint32_t millis() { return gNow; }
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static int gLockDepth = 0;
static void portENTER_CRITICAL(portMUX_TYPE*) { CHECK(gLockDepth == 0); ++gLockDepth; }
static void portEXIT_CRITICAL(portMUX_TYPE*) { CHECK(gLockDepth == 1); --gLockDepth; }

static std::vector<std::string> gLines, gEvents;
static void logSystemEvent(const char*, const char* fmt, ...) {
  CHECK(gLockDepth == 0);
  char line[192];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  gLines.push_back(std::string("event: ") + line);
}
static void broadcastLine(const char* fmt, ...) {
  CHECK(gLockDepth == 0);
  char line[192];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  gLines.push_back(line);
}
#define BROADCAST_PRINTF(...) broadcastLine(__VA_ARGS__)
static const char* g2RepairDescribe(bool left, bool right, char (&buf)[8]);
struct FakeTemple { bool connected = false; };
static FakeTemple gL, gR;
// Each push records "reason:rp", the repair text the SSE would carry.
static void g2PushStatusEvent(const char* reason) {
  CHECK(gLockDepth == 0);
  char buf[8];
  gEvents.push_back(std::string(reason) + ":" + g2RepairDescribe(gL.connected, gR.connected, buf));
}
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)

// INSERT_TYPES

// Links: gL/gR.connected are the published topology.
static bool gClientUp = true, gPairPublished = false;
static G2LinkSnapshot g2LinkSnapshot() {
  G2LinkSnapshot s{};
  s.left = gL.connected;
  s.right = gR.connected;
  return s;
}
static bool isG2ClientInitialized() { return gClientUp; }
static bool g2RoleAdmissionOpen() { return true; }
static bool isBleServerInitialized() { return false; }
// The shared client stack and the glasses runtime it carries: the admission
// thunk opens the runtime on an up stack (both are the same flag here).
static bool bleStackReady() { return gClientUp; }
static bool initG2Client() { gClientUp = true; return true; }
static bool g2PublishedPairConnected() { return gPairPublished; }

// The glasses connect claim and the completion mutex.
static bool gCompletionOk = true, gClaimHeld = false;
static unsigned gClaimCalls = 0;
struct G2CompletionGuard {
  explicit operator bool() const { return gCompletionOk; }
};
static bool g2ConnectClaim(uint32_t* epoch) {
  CHECK(gLockDepth == 0);
  ++gClaimCalls;
  if (gClaimHeld) return false;
  gClaimHeld = true;
  if (epoch) *epoch = 77;
  return true;
}
static void g2ConnectRelease() {
  CHECK(gClaimHeld);
  gClaimHeld = false;
}

// Peer intent and the saved target.
static BlePeerReconnectSnapshot gIntent{};
static bool gManualLearnOk = true;
static unsigned gSavedMask = 3;  // bit 0: left (mac1), bit 1: right (mac2)
static uint32_t gTargetIdentity = 5;
static unsigned gManualLearns = 0;
static bool blePeerBeginManualLearn(BlePeerKind, uint32_t& intent, uint32_t& identity) {
  ++gManualLearns;
  if (!gManualLearnOk) return false;
  gIntent.userDisconnect = false;
  intent = 9;
  identity = 5;
  return true;
}
static bool blePeerReconnectSnapshot(BlePeerKind, BlePeerReconnectSnapshot& out) {
  out = gIntent;
  return true;
}
static bool blePeerSavedTargetSnapshot(BlePeerKind, BlePeerSavedTargetSnapshot& out) {
  out = BlePeerSavedTargetSnapshot{};  // like production, always a snapshot
  out.identityGeneration = gTargetIdentity;
  if (gSavedMask & 1) std::strcpy(out.target.mac1, "02:00:00:00:00:01");
  if (gSavedMask & 2) std::strcpy(out.target.mac2, "02:00:00:00:00:02");
  return true;
}
static bool blePeerConnectRequestIsCurrent(BlePeerKind, const BlePeerConnectRequest&) { return true; }
static void blePeerRequestToJob(const BlePeerConnectRequest&, BleConnectJob&) {}

// The setup worker: queued jobs, with the fast try they were queued as.
struct Queued { BleConnectJob job; uint8_t fastTry; uint32_t at; };
static bool gSubmitOk = true;
static std::vector<Queued> gQueue;
static std::vector<uint32_t> gTryTimes;
static bool g2SubmitBleConnect(const BleConnectJob& job) {
  CHECK(gLockDepth == 0 && gClaimHeld);
  if (!gSubmitOk) return false;
  gQueue.push_back({job, gRepair.fastTry(), gNow});
  return true;
}

// INSERT_PRODUCTION

// g2SetupFinish's order for a repair: the schedule hears the result, then the
// claim is released. ok connects the missing temple.
static Queued runTry(bool ok) {
  CHECK(gQueue.size() == 1);
  const Queued q = gQueue.back();
  gQueue.clear();
  CHECK(q.job.kind == BleConnectKind::G2_REPAIR && q.job.cancelEpoch == 77);
  CHECK(std::strcmp(q.job.mac, gSavedMask & 1 ? "02:00:00:00:00:01" : "") == 0 &&
        std::strcmp(q.job.mac2, gSavedMask & 2 ? "02:00:00:00:00:02" : "") == 0);
  gTryTimes.push_back(gNow);
  if (ok) {
    gL.connected = gR.connected = true;
  }
  missingArmRecoveryComplete(ok);
  g2ConnectRelease();
  return q;
}

static std::string status() {
  char buf[8];
  return g2RepairDescribe(gL.connected, gR.connected, buf);
}

static void world(uint32_t now) {
  gNow = now;
  gRepair = g2repair::Schedule{};
  gL.connected = true;
  gR.connected = false;
  gClientUp = true;
  gPairPublished = false;
  gCompletionOk = true;
  gClaimHeld = false;
  gIntent = BlePeerReconnectSnapshot{};
  gIntent.ownerAuthorityAvailable = true;
  gIntent.intentGeneration = 3;
  gIntent.identityGeneration = 5;
  gManualLearnOk = gSubmitOk = true;
  gSavedMask = 3;
  gTargetIdentity = 5;
  gQueue.clear();
  gTryTimes.clear();
  gLines.clear();
  gEvents.clear();
}

// Heartbeats every 5 s; a queued try fails 3.5 s after it was queued.
static void beatFor(uint32_t ms, bool failTries = true) {
  const uint32_t end = gNow + ms;
  while (int32_t(end - gNow) > 0) {
    gNow += 5000;
    recoveryHeartbeatTick();
    if (failTries && !gQueue.empty()) {
      gNow += 3500;
      (void)runTry(false);
      gNow -= 3500;
    }
  }
}

static std::vector<std::string> repairPushes() {
  std::vector<std::string> out;
  for (const auto& e : gEvents) if (e.rfind("repair:", 0) == 0) out.push_back(e.substr(7));
  return out;
}

static size_t count(const std::vector<std::string>& v, const char* needle) {
  size_t n = 0;
  for (const auto& s : v) n += s.find(needle) != std::string::npos;
  return n;
}

// The fast tries, then every 10 minutes with no restart at the cap (the
// v0.99.9 bug restarted at attempt 1 and never logged the give-up).
static void scheduleOverADay(uint32_t start) {
  world(start);
  recoveryHeartbeatTick();  // the drop is noticed: the episode starts
  CHECK(gQueue.empty() && status() == "1/7");
  std::vector<uint8_t> tries;
  for (uint32_t elapsed = 0; elapsed < 24u * 3600u * 1000u; elapsed += 5000) {
    gNow += 5000;
    recoveryHeartbeatTick();
    if (!gQueue.empty()) {
      tries.push_back(gQueue.back().fastTry);
      gNow += 3500;
      (void)runTry(false);
      gNow -= 3500;
    }
  }
  CHECK(tries.size() >= 7);
  for (uint8_t i = 0; i < 7; ++i) CHECK(tries[i] == i + 1);
  for (size_t i = 7; i < tries.size(); ++i) CHECK(tries[i] == 0);
  // Gaps from one try's end to the next queue: the backoff, plus up to a beat.
  for (size_t i = 1; i < gTryTimes.size(); ++i) {
    const uint32_t gap = gTryTimes[i] - gTryTimes[i - 1] - 3500;
    const uint32_t want = i < 7 ? g2repair::kFastBackoffMs[i] : g2repair::kSlowRetryMs;
    CHECK(gap >= want && gap <= want + 5000);
  }
  // About 13 minutes of fast tries, then 6 an hour for the rest of the day.
  CHECK(tries.size() >= 7 + 138 && tries.size() <= 7 + 144);
  CHECK(count(gLines, "still missing after 7 tries, retrying every 10 min") == 1);
  // The SSE follows every change once: 1/7 .. 7/7, then slow, then nothing.
  CHECK((repairPushes() == std::vector<std::string>{"1/7", "2/7", "3/7", "4/7", "5/7", "6/7", "7/7", "slow"}));
  CHECK(count(gLines, "attempt") == 0);  // the attempt line itself is in the worker
  CHECK(status() == "slow" && !gClaimHeld);
}

// A wear edge or a gesture sends a slow repair back to fast try 1, once;
// gestures at most once per 30 minutes in an episode.
static void toSlow() {
  for (int i = 0; i < 400 && status() != "slow"; ++i) beatFor(5000);
  CHECK(status() == "slow");
}

static void nudges() {
  world(1000);
  toSlow();
  const uint32_t slowDue = gRepair.nextMs;
  for (uint32_t etype : {4u, 5u, 6u, 7u, 8u, 10u, 11u}) {
    g2RepairNoteLensInput(etype);  // not the wearer's gestures
    CHECK(status() == "slow" && gRepair.nextMs == slowDue);
  }
  const uint32_t firstGesture = gNow;
  g2RepairNoteLensInput(0);  // CLICK
  CHECK(status() == "1/7" && gRepair.nextMs == gNow + 5000);
  g2RepairNoteLensInput(3);  // more gestures in the fast phase change nothing
  g2RepairNudge(false);
  CHECK(status() == "1/7" && gRepair.nextMs == gNow + 5000);
  gNow += 5000;
  recoveryHeartbeatTick();
  CHECK(count(gLines, "glasses worn or lens used, retrying the missing RIGHT temple") == 1);
  CHECK(gQueue.size() == 1 && gQueue.back().fastTry == 1 && repairPushes().back() == "1/7");
  (void)runTry(false);
  CHECK(status() == "2/7");
  const uint32_t fastDue = gRepair.nextMs;
  g2RepairNoteLensInput(0);  // a fast repair keeps its place
  g2RepairNudge(false);
  CHECK(status() == "2/7" && gRepair.nextMs == fastDue && !gRepair.restartNotice);
  // Back to slow inside 30 minutes of the first gesture restart: gestures wait,
  // the wear edge does not.
  toSlow();
  CHECK(uint32_t(gNow - firstGesture) < g2repair::kGestureRestartCooldownMs);
  g2RepairNoteLensInput(0);
  CHECK(status() == "slow");
  g2RepairNudge(false);
  CHECK(status() == "1/7");
  toSlow();
  while (uint32_t(gNow - firstGesture) < g2repair::kGestureRestartCooldownMs) {
    g2RepairNoteLensInput(1);
    CHECK(status() == "slow" || !gQueue.empty() || gRepair.queued);
    beatFor(5000);
  }
  g2RepairNoteLensInput(1);  // the cooldown is over
  CHECK(status() == "1/7");
  // A new episode allows a gesture restart at once.
  // Each gesture kind counts: scrolls, double click, long press.
  for (uint32_t etype : {1u, 2u, 3u, 9u}) {
    world(1000);
    toSlow();
    g2RepairNoteLensInput(etype);
    CHECK(status() == "1/7");
  }
  // A nudge while a slow try runs: that try counts as fast try 1.
  world(1000);
  toSlow();
  while (gQueue.empty()) {
    gNow += 5000;
    recoveryHeartbeatTick();
  }
  CHECK(gQueue.back().fastTry == 0);
  g2RepairNudge(false);
  (void)runTry(false);
  CHECK(status() == "2/7" && gRepair.nextMs == gNow + 10000);
  // Not while held, and not with both temples up or down.
  world(1000);
  toSlow();
  gIntent.userDisconnect = true;
  beatFor(5000);
  CHECK(status() == "held");
  g2RepairNudge(false);
  g2RepairNoteLensInput(0);
  CHECK(gRepair.failed == g2repair::kFastTries && status() == "held");
  world(1000);
  gR.connected = true;
  recoveryHeartbeatTick();
  g2RepairNudge(false);
  CHECK(!gRepair.active && status() == "none");
}

// A refused queue attempt uses no try: held (the user disconnected, owner
// authority gone, no target) shows as held; busy does not. Neither leaks the
// claim or the schedule's queued flag.
static void refusals() {
  world(1000);
  beatFor(10000);  // first try queued and failed: 2/7 waits 10 s
  CHECK(status() == "2/7");
  gIntent.userDisconnect = true;
  const unsigned claimsBefore = gClaimCalls;
  beatFor(5000);
  CHECK(status() == "held");  // at the next heartbeat, not the next due try
  beatFor(30000);
  CHECK(status() == "held" && gTryTimes.size() == 1 && !gClaimHeld && !gRepair.queued);
  CHECK(gClaimCalls == claimsBefore);  // a held repair does not even ask
  gIntent.userDisconnect = false;
  gNow += 5000;
  recoveryHeartbeatTick();
  CHECK(gQueue.size() == 1 && gQueue.back().fastTry == 2 && status() == "2/7");
  (void)runTry(false);
  // Owner authority gone, then the surviving (left) temple's address not
  // saved: held. Only the missing side unsaved is the name-based match: not held.
  gIntent.ownerAuthorityAvailable = false;
  beatFor(40000);
  CHECK(status() == "held" && !gClaimHeld);
  gIntent.ownerAuthorityAvailable = true;
  for (unsigned mask : {0u, 2u}) {
    gSavedMask = mask;
    beatFor(10000);
    CHECK(status() == "held" && !gClaimHeld && gQueue.empty());
  }
  gSavedMask = 1;
  for (int i = 0; i < 20 && gQueue.empty(); ++i) {
    gNow += 5000;
    recoveryHeartbeatTick();
  }
  CHECK(gQueue.size() == 1 && status() == "3/7");
  (void)runTry(false);
  gSavedMask = 3;
  // The queue path checks the same: a held tick's view can be a beat late.
  gRepair.observeIntent(false);
  gSavedMask = 2;
  CHECK(noteRepairQueued(queueMissingArmRecovery()) == RepairQueue::Held && status() == "held");
  gSavedMask = 3;
  // A late "held" from one caller never marks another caller's running try.
  gRepair.observeIntent(false);
  CHECK(noteRepairQueued(queueMissingArmRecovery()) == RepairQueue::Queued);
  CHECK(noteRepairQueued(RepairQueue::Held) == RepairQueue::Held && status() != "held");
  (void)runTry(false);
  CHECK(status() == "5/7" && !gRepair.held);
  // Busy: another glasses job holds the claim, the worker refuses, a stale
  // identity, the completion lock unavailable. Each is asked for a due try and
  // refused without holding, using no try and leaking nothing.
  const size_t tries = gTryTimes.size();
  for (int busy = 0; busy < 4; ++busy) {
    if (busy == 0) gClaimHeld = true;
    if (busy == 1) gSubmitOk = false;
    if (busy == 2) gTargetIdentity = 6;
    if (busy == 3) gCompletionOk = false;
    gRepair.nextMs = gNow;  // due at the next beat
    const unsigned claims = gClaimCalls;
    gNow += 5000;
    recoveryHeartbeatTick();
    CHECK(gClaimCalls == claims + (busy == 3 ? 0u : 1u));  // asked (the completion lock fails first)
    CHECK(status() == "5/7" && gQueue.empty() && !gRepair.queued && !gRepair.held);
    CHECK(gRepair.nextMs == gNow + g2repair::kRequeueMs);
    CHECK(gClaimHeld == (busy == 0));  // only the other job's claim remains
    gClaimHeld = false;
    gSubmitOk = gCompletionOk = true;
    gTargetIdentity = 5;
  }
  CHECK(gTryTimes.size() == tries);
  gNow += 5000;
  recoveryHeartbeatTick();
  CHECK(gQueue.size() == 1 && gQueue.back().fastTry == 5);
  // Disconnecting while a try runs shows held at once; the try ends at its own
  // intent fence and uses up a try; the repair stays held, asking nothing.
  gIntent.userDisconnect = true;
  gNow += 5000;
  recoveryHeartbeatTick();
  CHECK(status() == "held" && gRepair.queued);
  (void)runTry(false);
  const unsigned heldClaims = gClaimCalls;
  beatFor(20000);
  CHECK(status() == "held" && gRepair.failed == 5 && gClaimCalls == heldClaims);
  gIntent.userDisconnect = false;
  for (int i = 0; i < 60 && gQueue.empty(); ++i) {  // 240 s backoff after try 5
    gNow += 5000;
    recoveryHeartbeatTick();
  }
  CHECK(gQueue.size() == 1 && gQueue.back().fastTry == 6 && status() == "6/7");
  // One try at a time: the schedule's claim refuses a second while one runs.
  gClaimHeld = false;  // even if the connect claim were free
  CHECK(noteRepairQueued(queueMissingArmRecovery()) == RepairQueue::Busy);
  CHECK(gQueue.size() == 1 && !gClaimHeld);
  gClaimHeld = true;
  (void)runTry(false);
}

// The episode ends when both temples are up or down, or the client is off.
static void episodes() {
  world(1000);
  beatFor(60000);
  CHECK(status() == "4/7");
  gL.connected = false;  // both down: the ladder's business
  recoveryHeartbeatTick();
  CHECK(!gRepair.active && status() == "none");
  gL.connected = true;   // a new drop starts again at try 1
  recoveryHeartbeatTick();
  CHECK(status() == "1/7" && gRepair.nextMs == gNow + 5000);
  beatFor(30000);
  CHECK(status() == "3/7");
  gClientUp = false;
  recoveryHeartbeatTick();
  CHECK(!gRepair.active);
  gClientUp = true;
  // Success ends it.
  world(1000);
  beatFor(10000, false);  // the first beat starts the episode, the second queues
  CHECK(gQueue.size() == 1);
  (void)runTry(true);
  CHECK(!gRepair.active && status() == "none");
  recoveryHeartbeatTick();
  CHECK(!gRepair.active && gQueue.empty());
  // The repair's own connect reads as both temples up until its session is
  // promoted or refused: no end while the try runs, and a refused promotion
  // counts as one failed try, in the fast and in the slow phase.
  for (bool slow : {false, true}) {
    world(1000);
    if (slow) toSlow();
    for (int i = 0; i < 200 && gQueue.empty(); ++i) {
      gNow += 5000;
      recoveryHeartbeatTick();
    }
    CHECK(gQueue.size() == 1);
    const uint8_t failedBefore = gRepair.failed;
    gR.connected = true;  // linked, promoting
    const unsigned claims = gClaimCalls;
    for (int i = 0; i < 4; ++i) {
      gNow += 5000;
      recoveryHeartbeatTick();
    }
    CHECK(gRepair.active && gRepair.queued && gRepair.failed == failedBefore && gClaimCalls == claims);
    gR.connected = false;  // promotion refused: closed again
    (void)runTry(false);
    CHECK(gRepair.active && status() == (slow ? "slow" : "2/7"));
    CHECK(gRepair.nextMs == gNow + (slow ? g2repair::kSlowRetryMs : g2repair::kFastBackoffMs[1]));
  }
  // The survivor drops while a try runs: nothing changes until the try ends.
  world(1000);
  beatFor(10000, false);  // the first beat starts the episode, the second queues
  CHECK(gQueue.size() == 1);
  gL.connected = false;
  recoveryHeartbeatTick();
  CHECK(gRepair.active && gRepair.queued && status() == "none");
  gL.connected = true;   // and comes back: the try's failure is try 1
  const unsigned claims = gClaimCalls;
  for (int i = 0; i < 4; ++i) {
    gNow += 5000;
    recoveryHeartbeatTick();
  }
  CHECK(gQueue.size() == 1 && gRepair.queued && gClaimCalls == claims);  // not even asked
  (void)runTry(false);
  CHECK(status() == "2/7" && !gRepair.queued);
  // A try that fails after the survivor dropped too ends the episode.
  world(1000);
  beatFor(10000, false);
  gL.connected = false;
  recoveryHeartbeatTick();
  (void)runTry(false);
  CHECK(!gRepair.active && !gRepair.queued);
  // A try that did not run ends as failed (the worker passes ok=false).
  world(1000);
  beatFor(10000, false);  // the first beat starts the episode, the second queues
  gQueue.clear();
  missingArmRecoveryComplete(false);
  g2ConnectRelease();
  CHECK(status() == "2/7" && !gRepair.queued);
}

// g2recover: try now, back at try 1, clearing a user disconnect.
static void manual() {
  world(1000);
  beatFor(20 * 60 * 1000);
  gIntent.userDisconnect = true;
  beatFor(5000);
  CHECK(status() == "held");
  const unsigned learns = gManualLearns;
  CHECK(std::strstr(cmd_g2recover(""), "repair queued") != nullptr);
  CHECK(gManualLearns == learns + 1 && gQueue.size() == 1 && gQueue.back().fastTry == 1);
  CHECK(status() == "1/7");
  (void)runTry(false);
  CHECK(status() == "2/7" && gRepair.nextMs == gNow + 10000);
  // Busy keeps the retry 1 s out; the tick takes it.
  gClaimHeld = true;
  CHECK(std::strstr(cmd_g2recover(""), "retry retained") != nullptr);
  CHECK(status() == "1/7" && gRepair.nextMs == gNow + g2repair::kRequeueMs);
  gClaimHeld = false;
  gNow += 5000;
  recoveryHeartbeatTick();
  CHECK(gQueue.size() == 1 && gQueue.back().fastTry == 1);
  (void)runTry(false);
  // No owner authority: an error, and held.
  gManualLearnOk = false;
  CHECK(std::strncmp(cmd_g2recover(""), "Error: G2 recovery:", 19) == 0 && status() == "held");
  gManualLearnOk = true;
  gR.connected = true;
  CHECK(std::strcmp(cmd_g2recover(""), "G2: both temples already connected") == 0);
  gL.connected = gR.connected = false;
  CHECK(std::strncmp(cmd_g2recover(""), "Error: G2: both temples down", 28) == 0);
  gClientUp = false;
  CHECK(std::strcmp(cmd_g2recover(""), "Error: G2: client not initialized") == 0);
}

// The reconnect ladder defers while one temple is up.
static void ladder() {
  world(1000);
  BlePeerConnectRequest request{};
  std::strcpy(request.savedTarget.mac1, "02:00:00:00:00:01");
  for (int i = 0; i < 2; ++i) {
    gL.connected = i == 0;
    gR.connected = i == 1;
    CHECK(g2PeerConnectSavedAdmissionThunk(request) == BlePeerConnectAdmission::COALESCED);
    CHECK(!gClaimHeld && gQueue.empty());
  }
  gL.connected = gR.connected = false;
  CHECK(g2PeerConnectSavedAdmissionThunk(request) == BlePeerConnectAdmission::STARTED);
  CHECK(gClaimHeld && gQueue.size() == 1 && gQueue.back().job.kind == BleConnectKind::G2_SAVED);
  gQueue.clear();
  g2ConnectRelease();
  gL.connected = gR.connected = true;
  gPairPublished = true;
  CHECK(g2PeerConnectSavedAdmissionThunk(request) == BlePeerConnectAdmission::ALREADY_UP);
  BlePeerConnectRequest none{};
  gPairPublished = false;
  gR.connected = false;
  CHECK(g2PeerConnectSavedAdmissionThunk(none) == BlePeerConnectAdmission::NO_TARGET);
}

// Status text for each phase; "none" unless exactly one temple is up.
static void statusText() {
  world(1000);
  char buf[8];
  CHECK(std::strcmp(g2RepairDescribe(true, false, buf), "none") == 0);
  recoveryHeartbeatTick();
  CHECK(std::strcmp(g2RepairDescribe(true, false, buf), "1/7") == 0);
  CHECK(std::strcmp(g2RepairDescribe(false, true, buf), "1/7") == 0);
  CHECK(std::strcmp(g2RepairDescribe(true, true, buf), "none") == 0);
  CHECK(std::strcmp(g2RepairDescribe(false, false, buf), "none") == 0);
  for (uint8_t f = 0; f < g2repair::kFastTries; ++f) {
    gRepair.failed = f;
    const char want[] = {char('1' + f), '/', '7', '\0'};
    CHECK(std::strcmp(g2RepairDescribe(true, false, buf), want) == 0);
  }
  gRepair.failed = g2repair::kFastTries;
  CHECK(std::strcmp(g2RepairDescribe(true, false, buf), "slow") == 0);
  gRepair.held = true;
  CHECK(std::strcmp(g2RepairDescribe(true, false, buf), "held") == 0);
}

// Random operations on the schedule alone: bounded waits, saturating counts.
static void randomCore() {
  uint64_t seed = 0x9e3779b97f4a7c15ull;
  auto next = [&seed]() {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
  };
  for (int run = 0; run < 200; ++run) {
    g2repair::Schedule s;
    uint32_t now = uint32_t(next());
    bool claimed = false;
    for (int step = 0; step < 2000; ++step) {
      now += uint32_t(next() % 20000);
      switch (next() % 9) {
        case 0: (void)s.due(now); break;
        case 1: if (!claimed) claimed = s.claim(); break;
        case 2: if (claimed) { s.unclaim(); claimed = false; s.refused(next() & 1, now); } break;
        case 3: s.observeIntent(next() % 4 == 0); break;
        case 4: if (claimed) { s.finished(next() % 8 == 0, next() % 8 != 0, now); claimed = false; } break;
        case 5: (void)s.nudge(now, next() & 1); if (next() % 4 == 0) s.observeIntent(next() & 1); break;
        case 6: if (next() % 10 == 0) s.end(); break;
        case 7: if (next() % 20 == 0) s.restart(now, 0); break;
        case 8: s.slowNotice = s.restartNotice = false; break;
      }
      CHECK(s.queued == claimed);
      CHECK(s.failed <= g2repair::kFastTries);
      CHECK(!s.active || int32_t(s.nextMs - now) <= int32_t(g2repair::kSlowRetryMs));
      CHECK(!s.active ? s.phase() == g2repair::Phase::Off : s.phase() != g2repair::Phase::Off);
      CHECK(s.fastTry() == (s.active && s.failed < g2repair::kFastTries ? s.failed + 1 : 0));
      CHECK(!s.slowNotice || s.failed == g2repair::kFastTries);
      CHECK(!s.restartNotice || s.failed < g2repair::kFastTries);
    }
  }
}

int main() {
  scheduleOverADay(0);
  scheduleOverADay(0xFFFFFFFFu - 600000u);  // the clock wraps during the fast tries
  nudges();
  refusals();
  episodes();
  manual();
  ladder();
  statusText();
  randomCore();
  CHECK(gLockDepth == 0);
  std::printf("G2 repair schedule: %u checks\n", assertions);
}
