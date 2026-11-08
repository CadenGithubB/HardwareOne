// Discover-then-confirm ESP-NOW pairing (System_ESPNow_PairCore.h, compiled as
// is) between two modelled devices over a lossy broadcast channel. The device
// glue mirrors System_ESPNow.cpp: v4h_pair_request / pairAnswerReceived /
// cmd_espnow_accept / cmd_espnow_reject / the heartbeat sweep (step 2c);
// cmd_espnow_pairsecure is modelled as "add the peer to the registry and send
// KEY_EX_HELLO". A KEY_EX_HELLO is not a pair answer (the target also sends one
// for unrelated encrypted traffic). A source contract pins those call sites.
//
// Cases: happy path, lost first REQUEST, lost ACCEPT, every answer lost until
// timeout then the half-pair repair, duplicate REQUEST while pending and after
// an answer, the discovery window closing while the prompt is open, an
// unrelated HELLO while waiting, lost REJECT, timeout, an initiator that sends
// once (older firmware), and the millis() wrap.
#include "System_ESPNow_PairCore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using namespace espnowpair;

enum class Kind { Request, Accept, Reject, Hello };
struct Frame { Kind kind; int from; int to; };

struct Device {
  uint8_t mac[6];
  const char* name;
  State st{};
  std::set<int> registry;    // devices[] (by device index)
  bool windowOpen = true;
  bool resends = true;       // false: an initiator that sends its REQUEST once
  int prompts = 0;           // accept prompts shown
  int completions = 0;       // PairCompleteJob runs (initiator)
  int timeouts = 0;
  uint32_t lastPromptAtMs = 0;
};

struct World {
  uint32_t now = 0;
  Device dev[2];
  std::vector<Frame> air;
  int sent[4] = {};
  std::function<bool(const Frame&, int nth)> drop = [](const Frame&, int) { return false; };
  int nthOfKind[4] = {};

  World(uint32_t start = 1000) : now(start) {
    const uint8_t a[6] = {0xAA, 1, 2, 3, 4, 5}, b[6] = {0xBB, 6, 7, 8, 9, 10};
    std::memcpy(dev[0].mac, a, 6); dev[0].name = "alpha";
    std::memcpy(dev[1].mac, b, 6); dev[1].name = "bravo";
  }
  int idx(const uint8_t mac[6]) const { return std::memcmp(mac, dev[0].mac, 6) == 0 ? 0 : 1; }

  void send(Kind k, int from, int to) {
    const int kk = (int)k;
    sent[kk]++;
    const Frame f{k, from, to};
    if (drop(f, ++nthOfKind[kk])) return;
    air.push_back(f);
  }
  // cmd_espnow_pairsecure, modelled: registry write + KEY_EX_HELLO to the peer.
  void pairsecure(int self, int peer) {
    dev[self].registry.insert(peer);
    send(Kind::Hello, self, peer);
  }
  // pairAnswerReceived
  void answerReceived(int self, int from, Answer a) {
    Request ended{};
    if (!onAnswer(dev[self].st, dev[from].mac, a, now, &ended)) return;
    if (a == Answer::Accept) {
      dev[self].completions++;
      pairsecure(self, from);
    }
  }
  void deliver(const Frame& f) {
    Device& d = dev[f.to];
    switch (f.kind) {
      case Kind::Request: {   // v4h_pair_request
        const Reply r = onRequest(d.st, dev[f.from].mac, dev[f.from].name, now,
                                  d.registry.count(f.from) != 0, d.windowOpen);
        if (r == Reply::Accept) send(Kind::Accept, f.to, f.from);
        if (r == Reply::Reject) send(Kind::Reject, f.to, f.from);
        if (r == Reply::Prompt) { d.prompts++; d.lastPromptAtMs = now; }
        break;
      }
      case Kind::Accept: answerReceived(f.to, f.from, Answer::Accept); break;
      case Kind::Reject: answerReceived(f.to, f.from, Answer::Reject); break;
      case Kind::Hello: break;   // key exchange only: not a pair answer
    }
  }
  // The heartbeat sweep, step 2c.
  void sweep(int self) {
    Device& d = dev[self];
    const Tick t = tick(d.st, now);
    if (t.resendRequest && d.resends) send(Kind::Request, self, idx(t.requestMac));
    if (t.resendAnswer) send(t.answer == Answer::Accept ? Kind::Accept : Kind::Reject, self, idx(t.answerMac));
    if (t.outTimedOut) d.timeouts++;
  }
  // cmd_espnow_pairrequest
  void request(int self, int peer) {
    startRequest(dev[self].st, dev[peer].mac, dev[peer].name, now);
    send(Kind::Request, self, peer);
  }
  // cmd_espnow_accept / cmd_espnow_reject
  bool userAnswers(int self, Answer a) {
    Request taken{};
    if (!answerIncoming(dev[self].st, a, now, &taken)) return false;
    const int peer = idx(taken.mac);
    send(a == Answer::Accept ? Kind::Accept : Kind::Reject, self, peer);
    if (a == Answer::Accept) pairsecure(self, peer);
    return true;
  }
  // Advance in 10 ms steps (the heartbeat task's period); frames land 2 ms after sending.
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      now += 10;
      std::vector<Frame> landing;
      landing.swap(air);
      for (const Frame& f : landing) deliver(f);
      sweep(0);
      sweep(1);
    }
  }
  bool bothPaired() const { return dev[0].registry.count(1) && dev[1].registry.count(0); }
  Outcome outcomeOf(int self) { char n[kNameCap]; return outcome(dev[self].st, now, n, sizeof(n)); }
};

static void happyPath() {
  World w;
  w.request(0, 1);
  w.run(100);
  CHECK(w.dev[1].prompts == 1);
  CHECK(w.outcomeOf(0) == Outcome::Waiting);
  w.run(3000);   // the user takes a few seconds; the REQUEST repeats meanwhile
  CHECK(w.dev[1].prompts == 1);   // the repeats are the same request: no new prompt
  CHECK(w.userAnswers(1, Answer::Accept));
  w.run(5000);
  CHECK(w.bothPaired());
  CHECK(w.dev[0].completions == 1);
  CHECK(w.outcomeOf(0) == Outcome::Paired);
  CHECK(!w.dev[0].st.out.active && !w.dev[1].st.in.active);
  w.run(kAnswerMemoryMs + 2000);
  CHECK(w.sent[(int)Kind::Accept] == 1 + kAnswerResends);   // bounded: no more after the memory
  CHECK(w.outcomeOf(0) == Outcome::None);                    // the result is shown for a while only
  CHECK(w.dev[0].timeouts == 0);
}

static void lostFirstRequest() {
  World w;
  w.drop = [](const Frame& f, int nth) { return f.kind == Kind::Request && nth == 1; };
  w.request(0, 1);
  w.run(100);
  CHECK(w.dev[1].prompts == 0);
  w.run(kResendMs + 100);   // the retransmit reaches the target
  CHECK(w.dev[1].prompts == 1);
  CHECK(w.userAnswers(1, Answer::Accept));
  w.run(1000);
  CHECK(w.bothPaired() && w.dev[0].completions == 1);
}

// The target accepts, the first ACCEPT is lost. The initiator's next REQUEST
// gets the remembered answer (and the unsolicited copy also covers it).
static void lostAcceptRetransmit() {
  World w;
  w.drop = [](const Frame& f, int nth) { return f.kind == Kind::Accept && nth == 1; };
  w.request(0, 1);
  w.run(500);
  CHECK(w.userAnswers(1, Answer::Accept));
  w.run(100);
  CHECK(w.dev[1].registry.count(0));      // the target paired at once
  w.run(kResendMs * 3);
  CHECK(w.bothPaired());                  // ... and the initiator caught up
  CHECK(w.dev[0].completions == 1);
  CHECK(w.dev[0].timeouts == 0);
  CHECK(w.dev[1].prompts == 1);
}

// The target's discovery window closes (expiry, 'espnowpairmode off', the
// OLED's B button) while its accept prompt is open. The initiator's retransmits
// are the same pending request: no REJECT, and the user's late accept pairs
// both sides.
static void windowClosesWhilePromptOpen() {
  World w;
  w.request(0, 1);
  w.run(100);
  CHECK(w.dev[1].prompts == 1);
  w.dev[1].windowOpen = false;
  w.run(kResendMs * 4);   // several retransmits land with the window closed
  CHECK(w.sent[(int)Kind::Reject] == 0);
  CHECK(w.dev[0].st.out.active && w.outcomeOf(0) == Outcome::Waiting);
  CHECK(w.dev[1].st.in.active && w.dev[1].prompts == 1);
  CHECK(w.userAnswers(1, Answer::Accept));
  w.run(1000);
  CHECK(w.bothPaired() && w.dev[0].completions == 1);
  CHECK(w.sent[(int)Kind::Reject] == 0);
}

// The target sends a KEY_EX_HELLO for unrelated traffic (an encrypted send to a
// peer without identity, espnowkeyex, espnowprobe) while the initiator waits,
// then its user rejects. The HELLO must not count as an accept.
static void unrelatedHelloIsNotAnAccept() {
  World w;
  w.request(0, 1);
  w.run(100);
  w.send(Kind::Hello, 1, 0);
  w.run(100);
  CHECK(w.dev[0].st.out.active && w.dev[0].completions == 0);
  CHECK(w.outcomeOf(0) == Outcome::Waiting && w.dev[0].registry.empty());
  CHECK(w.userAnswers(1, Answer::Reject));
  w.run(1000);
  CHECK(w.outcomeOf(0) == Outcome::Declined);
  CHECK(w.dev[0].registry.empty() && w.dev[1].registry.empty());
}

// Everything the target sends is lost for longer than the TTL: the initiator
// times out once, stops sending, and is told how to repair. A request made
// again after the target's answer memory expired is answered ACCEPT because
// the requester is already in the target's registry (window closed by then).
static void timeoutThenHalfPairRepair() {
  World w;
  bool targetMute = true;
  w.drop = [&targetMute](const Frame& f, int) { return targetMute && f.from == 1; };
  w.request(0, 1);
  w.run(1000);
  CHECK(w.userAnswers(1, Answer::Accept));
  w.run(kRequestTtlMs + 1000);
  CHECK(w.dev[1].registry.count(0) && !w.dev[0].registry.count(1));   // the half pair
  CHECK(w.dev[0].timeouts == 1);
  CHECK(w.outcomeOf(0) == Outcome::TimedOut);
  const int requests = w.sent[(int)Kind::Request];
  CHECK(requests <= 1 + (int)(kRequestTtlMs / kResendMs) + 1);   // bounded retransmit
  w.run(10000);
  CHECK(w.sent[(int)Kind::Request] == requests);                 // nothing after the timeout
  CHECK(w.dev[0].timeouts == 1);

  w.run(kAnswerMemoryMs);   // the target's answer memory is gone too
  CHECK(!w.dev[1].st.answered.active);
  targetMute = false;
  w.dev[1].windowOpen = false;
  w.request(0, 1);          // the repair the timeout text and the OLED suggest
  w.run(200);
  CHECK(w.bothPaired());
  CHECK(w.dev[0].completions == 1);
  CHECK(w.dev[1].prompts == 1);   // no second prompt on the target
}

// A REQUEST repeated while it waits for the user is the same request: the
// prompt's identity (sender, atMs) is kept, so the OLED does not re-open it.
static void duplicateRequestWhilePending() {
  World w;
  w.request(0, 1);
  w.run(50);
  const uint32_t firstAt = w.dev[1].st.in.atMs;
  CHECK(w.dev[1].prompts == 1);
  for (int i = 0; i < 3; i++) {
    w.send(Kind::Request, 0, 1);
    w.run(500);
  }
  CHECK(w.dev[1].prompts == 1);
  CHECK(w.dev[1].st.in.active && w.dev[1].st.in.atMs == firstAt);
  CHECK(w.sent[(int)Kind::Accept] == 0 && w.sent[(int)Kind::Reject] == 0);
}

// A REQUEST repeated after the user answered gets the same answer, no prompt.
static void duplicateRequestAfterAnswer() {
  for (Answer a : {Answer::Accept, Answer::Reject}) {
    World w;
    w.request(0, 1);
    w.run(50);
    CHECK(w.userAnswers(1, a));
    w.run(5000);   // initiator done; the unsolicited copies are spent
    const int acc = w.sent[(int)Kind::Accept], rej = w.sent[(int)Kind::Reject];
    w.send(Kind::Request, 0, 1);
    w.run(50);
    CHECK(w.dev[1].prompts == 1);
    CHECK(w.sent[(int)Kind::Accept] == acc + (a == Answer::Accept ? 1 : 0));
    CHECK(w.sent[(int)Kind::Reject] == rej + (a == Answer::Reject ? 1 : 0));
    CHECK(w.dev[0].completions == (a == Answer::Accept ? 1 : 0));   // repeats are ignored
  }
}

// The user rejects and the REJECT is lost: the initiator's retransmit gets the
// REJECT again and the target's user is not asked twice.
static void lostReject() {
  World w;
  w.drop = [](const Frame& f, int nth) { return f.kind == Kind::Reject && nth <= 2; };
  w.request(0, 1);
  w.run(50);
  CHECK(w.userAnswers(1, Answer::Reject));
  w.run(kResendMs * 4);
  CHECK(w.outcomeOf(0) == Outcome::Declined);
  CHECK(!w.dev[0].st.out.active && w.dev[0].timeouts == 0);
  CHECK(w.dev[1].prompts == 1);
  CHECK(!w.bothPaired() && w.dev[1].registry.empty() && w.dev[0].registry.empty());
}

// Nobody answers: both sides expire their request after the TTL, the
// initiator reports one timeout, and a request made later is new.
static void timeoutNoAnswer() {
  World w;
  w.request(0, 1);
  w.run(kRequestTtlMs - 100);
  CHECK(w.dev[0].st.out.active && w.dev[1].st.in.active && w.dev[0].timeouts == 0);
  w.run(1000);
  CHECK(!w.dev[0].st.out.active && !w.dev[1].st.in.active);
  CHECK(w.dev[0].timeouts == 1 && w.outcomeOf(0) == Outcome::TimedOut);
  CHECK(!w.userAnswers(1, Answer::Accept));   // the prompt is gone
  CHECK(w.sent[(int)Kind::Request] <= 1 + (int)(kRequestTtlMs / kResendMs) + 1);
  w.request(0, 1);
  w.run(50);
  CHECK(w.dev[1].prompts == 2);   // a new request after the old one ended
  CHECK(w.outcomeOf(0) == Outcome::Waiting);
}

// The target's discovery window is closed: REJECT, and the initiator stops.
static void windowClosedRejects() {
  World w;
  w.dev[1].windowOpen = false;
  w.request(0, 1);
  w.run(kResendMs * 3);
  CHECK(w.outcomeOf(0) == Outcome::Declined && w.dev[1].prompts == 0);
  CHECK(w.sent[(int)Kind::Request] == 1);
}

// An initiator on older firmware sends its REQUEST once. The target's
// unsolicited ACCEPT copies still reach it when the first ACCEPT is lost.
static void singleShotInitiator() {
  World w;
  w.dev[0].resends = false;
  w.drop = [](const Frame& f, int nth) { return f.kind == Kind::Accept && nth == 1; };
  w.request(0, 1);
  w.run(50);
  CHECK(w.userAnswers(1, Answer::Accept));
  w.run(kResendMs * 2 + 100);
  CHECK(w.bothPaired() && w.dev[0].completions == 1);
  CHECK(w.sent[(int)Kind::Request] == 1);
}

// The same exchange across the 32-bit millis() rollover.
static void acrossMillisWrap() {
  World w(0xFFFFFFFFu - 2000u);
  w.drop = [](const Frame& f, int nth) { return f.kind == Kind::Accept && nth == 1; };
  w.request(0, 1);
  w.run(1500);
  CHECK(w.userAnswers(1, Answer::Accept));
  w.run(kResendMs * 2);
  CHECK(w.bothPaired() && w.dev[0].completions == 1 && w.dev[0].timeouts == 0);
  World t(0xFFFFFFFFu - 2000u);
  t.request(0, 1);
  t.run(kRequestTtlMs - 100);
  CHECK(t.dev[0].timeouts == 0);
  t.run(1000);
  CHECK(t.dev[0].timeouts == 1);
}

static std::string readFile(const std::string& path) {
  std::ifstream f(path);
  CHECK(f.good());
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The firmware glue the model mirrors.
static void sourceContract() {
  const std::string dir = HW1_COMPONENT_DIR;
  const std::string owner = readFile(dir + "/System_ESPNow.cpp");
  const std::string crypto = readFile(dir + "/System_ESPNow_Handlers_Crypto.cpp");
  const std::string oled = readFile(dir + "/OLED_ESPNow.cpp");
  auto has = [](const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; };
  CHECK(has(owner, "#include \"System_ESPNow_PairCore.h\""));
  CHECK(has(owner, "espnowpair::onRequest(gPair, mac, name, (uint32_t)millis(), paired, windowOpen)"));
  CHECK(has(owner, "case espnowpair::Reply::Accept: sendPairCtrl(mac, ESPNOW_V4_TYPE_PAIR_ACCEPT); return;"));
  CHECK(has(owner, "espnowpair::onAnswer(gPair, mac, answer, (uint32_t)millis(), &ended)"));
  CHECK(has(owner, "const espnowpair::Tick t = espnowpair::tick(gPair, now);"));
  CHECK(has(owner, "if (t.resendRequest) sendPairCtrl(t.requestMac, ESPNOW_V4_TYPE_PAIR_REQUEST);"));
  CHECK(has(owner, "if (t.resendAnswer) {"));
  CHECK(has(owner, "'espnowpairsecure %s %s' here finishes the pair."));
  CHECK(has(owner, "espnowpair::startRequest(gPair, mac, name, now);"));
  CHECK(has(owner, "espnowpair::answerIncoming(gPair, answer, (uint32_t)millis(), &taken)"));
  CHECK(!has(owner, "gPendingPairIn") && !has(owner, "gPendingPairOut"));
  // The sweep is in processMeshHeartbeats.
  const size_t hb = owner.find("void processMeshHeartbeats() {");
  CHECK(hb != std::string::npos && owner.find("espnowpair::tick(gPair, now)", hb) != std::string::npos);
  // No KEY_EX_HELLO-as-accept hook: HELLO is also sent for unrelated traffic.
  CHECK(!has(crypto, "espnowPairNoteKeyExHello") && !has(owner, "espnowPairNoteKeyExHello"));
  CHECK(has(oled, "espnowGetPairRequestOutcome(outName, sizeof(outName))"));
}

int main() {
  happyPath();
  lostFirstRequest();
  lostAcceptRetransmit();
  timeoutThenHalfPairRepair();
  duplicateRequestWhilePending();
  duplicateRequestAfterAnswer();
  windowClosesWhilePromptOpen();
  unrelatedHelloIsNotAnAccept();
  lostReject();
  timeoutNoAnswer();
  windowClosedRejects();
  singleShotInitiator();
  acrossMillisWrap();
  sourceContract();
  std::printf("espnow pair core: %u checks passed\n", gChecks);
  return 0;
}
