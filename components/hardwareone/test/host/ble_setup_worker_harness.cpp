// The one BLE setup worker's scheduler (BLE_SetupWorkerCore.h), as is.
#include "BLE_SetupWorkerCore.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <random>
#include <thread>
#include <string>
#include <vector>

using namespace hw1setup;

// Allocation counter for the "never allocates" checks.
static std::atomic<long> gAllocs{0};
void* operator new(std::size_t n) {
  ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int gChecks = 0;
#define CHECK(x) do { ++gChecks; if (!(x)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

struct Event { char kind; Client client; uint32_t token; uint8_t reason; End end; uint32_t value; };

// A deterministic, single-threaded Env: a fake clock, a verdict table and an
// event log. It never allocates once its log is reserved.
struct Env {
  Core* core = nullptr;
  std::mutex m;
  uint32_t clock = 1000;
  Verdict verdict[kClients];
  End endFor[kClients] = {End::Ran, End::Ran, End::Ran, End::Ran};
  uint32_t runMs[kClients] = {0, 0, 0, 0};  // how long each client's run takes
  WaitInfo lastWait[kClients];
  RunInfo lastRun;
  std::vector<Event> log;
  std::vector<uint32_t> sleeps;
  void (*duringReady)(Env&, Client, uint32_t) = nullptr;
  void (*duringFinish)(Env&, Client, uint32_t) = nullptr;
  bool idleDuringFinish = true;
  Env() { log.reserve(4096); sleeps.reserve(4096); for (auto& v : verdict) v = {Ready::Run, kNone}; }
  void lock() { m.lock(); }
  void unlock() { m.unlock(); }
  uint32_t now() { return clock; }
  void waitWake(uint32_t ms) { sleeps.push_back(ms); if (ms != kForever) clock += ms; }
  Verdict ready(Client c, uint32_t t, const WaitInfo& w) {
    lastWait[size_t(c)] = w;
    if (duringReady) duringReady(*this, c, t);
    return verdict[size_t(c)];
  }
  void waiting(Client c, uint32_t t, uint8_t reason) { log.push_back({'w', c, t, reason, End::Ran, 0}); }
  End run(Client c, uint32_t t, const RunInfo& info) {
    lastRun = info;
    log.push_back({'r', c, t, kNone, End::Ran, info.queuedMs});
    clock += runMs[size_t(c)];
    return endFor[size_t(c)];
  }
  void finish(Client c, uint32_t t, const FinishInfo& f) {
    lock(); idleDuringFinish = core->idle(clientMask(c)); unlock();
    log.push_back({'f', c, t, f.reason, f.end, f.audioMs});
    if (duringFinish) duringFinish(*this, c, t);
  }
  size_t count(char kind, Client c) const {
    size_t n = 0;
    for (const auto& e : log) n += e.kind == kind && e.client == c;
    return n;
  }
};

static uint32_t submit(Core& core, Env& env, Client c) {
  uint32_t t = 0;
  CHECK(core.claim(c, &t));
  CHECK(core.publish(c, t, env.clock));
  return t;
}

static void basics() {
  Core core;
  Env env; env.core = &core;
  uint32_t a = 0, b = 0;
  CHECK(core.claim(Client::Ring, &a) && a);
  CHECK(!core.claim(Client::Ring, &b));                 // one job per client
  CHECK(!core.idle(clientMask(Client::Ring)) && core.idle(clientMask(Client::G2)));
  CHECK(!pickOnce(core, env).run.valid);                // Filling is not picked
  CHECK(!core.publish(Client::Ring, a + 1, 0));         // wrong token
  CHECK(core.cancelClaim(Client::Ring, a) && core.idle(kAll));
  CHECK(env.log.empty());                               // a cancelled claim never finishes
  const uint32_t t = submit(core, env, Client::Ring);
  CHECK(t != a && core.live(Client::Ring, t) && !core.live(Client::Ring, a));
  CHECK(core.busy(kAccessories) && !core.busy(clientMask(Client::Phone)));
  // Withdraw: a wrong token touches nothing, token 0 takes whatever is queued.
  Record taken;
  CHECK(core.withdraw(Client::Ring, t + 1, env.clock, &taken) == Core::Withdraw::NotLive);
  CHECK(core.withdraw(Client::Ring, 0, env.clock, &taken) == Core::Withdraw::Withdrawn && taken.token == t);
  CHECK(!core.idle(clientMask(Client::Ring)));          // finishing until finished()
  uint32_t again = 0;                                   // a new claim meanwhile keeps it so
  CHECK(core.claim(Client::Ring, &again) && core.cancelClaim(Client::Ring, again));
  CHECK(!core.idle(clientMask(Client::Ring)));
  core.finished(Client::Ring);
  CHECK(core.idle(kAll) && !core.live(Client::Ring, t));
  const uint32_t phone = submit(core, env, Client::Phone);  // the phone is never an accessory
  CHECK(!core.busy(kAccessories) && core.busy(kAll));
  CHECK(core.withdraw(Client::Phone, phone, env.clock, &taken) == Core::Withdraw::Withdrawn);
  core.finished(Client::Phone);
  CHECK(core.withdraw(Client::Ring, 0, env.clock, &taken) == Core::Withdraw::NotLive);
}

static void runAndFinishOnce() {
  Core core;
  Env env; env.core = &core;
  const uint32_t t = submit(core, env, Client::G2);
  env.clock += 250;
  const Pick p = pickOnce(core, env);
  CHECK(p.run.valid && p.run.client == Client::G2 && p.run.token == t && p.run.info.queuedMs == 250);
  CHECK(!core.idle(clientMask(Client::G2)) && core.busy(clientMask(Client::G2)));
  // A running job is never withdrawn and gets no finish from the attempt.
  Record taken;
  CHECK(core.withdraw(Client::G2, 0, env.clock, &taken) == Core::Withdraw::Running);
  CHECK(core.withdraw(Client::G2, t, env.clock, &taken) == Core::Withdraw::Running);
  CHECK(env.count('f', Client::G2) == 0);
  runTicket(core, env, p.run);
  CHECK(env.count('r', Client::G2) == 1 && env.count('f', Client::G2) == 1);
  CHECK(!env.idleDuringFinish);                         // finish runs while the slot is not idle
  CHECK(core.idle(kAll));
  CHECK(env.log.back().end == End::Ran);
}

static void order() {
  Core core;
  Env env; env.core = &core;
  submit(core, env, Client::Phone);
  submit(core, env, Client::Ring);
  submit(core, env, Client::RingRetire);
  submit(core, env, Client::G2);
  Client seen[kClients];
  for (size_t i = 0; i < kClients; ++i) {
    const Pick p = pickOnce(core, env);
    CHECK(p.run.valid);
    seen[i] = p.run.client;
    runTicket(core, env, p.run);
  }
  CHECK(seen[0] == Client::G2 && seen[1] == Client::RingRetire && seen[2] == Client::Ring && seen[3] == Client::Phone);
  CHECK(core.idle(kAll));
}

static void passOver() {
  Core core;
  Env env; env.core = &core;
  submit(core, env, Client::Phone);
  submit(core, env, Client::G2);
  Pick p = pickOnce(core, env);                          // G2 first, phone passed over
  CHECK(p.run.client == Client::G2 && core.peek(Client::Phone).passedOver);
  runTicket(core, env, p.run);
  submit(core, env, Client::G2);                         // a new glasses job
  p = pickOnce(core, env);
  CHECK(p.run.client == Client::Phone);                  // the passed-over phone goes next
  CHECK(core.peek(Client::G2).passedOver);
  runTicket(core, env, p.run);
  p = pickOnce(core, env);
  CHECK(p.run.client == Client::G2 && !core.peek(Client::G2).passedOver);
  runTicket(core, env, p.run);
  // A job that was not ready is never marked passed over.
  submit(core, env, Client::Ring);
  submit(core, env, Client::G2);
  env.verdict[size_t(Client::Ring)] = {Ready::Wait, kRole};
  p = pickOnce(core, env);
  CHECK(p.run.client == Client::G2 && !core.peek(Client::Ring).passedOver);
  runTicket(core, env, p.run);
  env.verdict[size_t(Client::Ring)] = {Ready::Run, kNone};
  p = pickOnce(core, env);
  CHECK(p.run.client == Client::Ring);
  runTicket(core, env, p.run);

  // The bound: with every client ready, whenever a client is passed over,
  // at most one turn per other client goes before it.
  std::mt19937 rng(7);
  Core c2;
  Env e2; e2.core = &c2;
  int since[kClients] = {-1, -1, -1, -1};
  for (int round = 0; round < 4000; ++round) {
    for (size_t i = 0; i < kClients; ++i) {
      uint32_t t = 0;
      if ((rng() & 3) == 0 && c2.claim(Client(i), &t)) { c2.publish(Client(i), t, e2.clock); since[i] = 0; }
    }
    const Pick q = pickOnce(c2, e2);
    if (!q.run.valid) continue;
    for (size_t i = 0; i < kClients; ++i) {
      if (Client(i) == q.run.client) { since[i] = -1; continue; }
      if (c2.peek(Client(i)).state == State::Queued) { ++since[i]; CHECK(since[i] <= int(kClients) - 1); }
    }
    runTicket(c2, e2, q.run);
  }
}

static void dropsAndWaits() {
  Core core;
  Env env; env.core = &core;
  const uint32_t ring = submit(core, env, Client::Ring);
  const uint32_t g2 = submit(core, env, Client::G2);
  env.verdict[size_t(Client::Ring)] = {Ready::Wait, kAudio};
  env.verdict[size_t(Client::G2)] = {Ready::Drop, kStale};
  Pick p = pickOnce(core, env);
  CHECK(!p.run.valid && p.changed);
  CHECK(env.count('f', Client::G2) == 1 && env.log[0].reason == kStale && env.log[0].end == End::Dropped);
  CHECK(!core.live(Client::G2, g2) && core.idle(clientMask(Client::G2)));
  CHECK(env.count('w', Client::Ring) == 1);
  // The same reason does not log again; a new reason does.
  p = pickOnce(core, env);
  CHECK(!p.run.valid && !p.changed && env.count('w', Client::Ring) == 1);
  // Audio time accumulates across a role wait, and the role wait pauses it.
  env.clock += 60000;
  env.verdict[size_t(Client::Ring)] = {Ready::Wait, kRole};
  pickOnce(core, env);
  CHECK(env.lastWait[size_t(Client::Ring)].audioMs == 60000);
  CHECK(env.count('w', Client::Ring) == 2);
  env.clock += 5000;
  env.verdict[size_t(Client::Ring)] = {Ready::Wait, kAudio};
  pickOnce(core, env);
  CHECK(env.lastWait[size_t(Client::Ring)].audioMs == 60000);   // the role spell did not count
  env.clock += 30000;
  pickOnce(core, env);
  CHECK(env.lastWait[size_t(Client::Ring)].audioMs == 90000);   // and did not reset it
  CHECK(env.lastWait[size_t(Client::Ring)].queuedMs == 95000);
  // Ready again: the run reports the wait for other setups apart from its own.
  env.verdict[size_t(Client::Ring)] = {Ready::Run, kNone};
  env.clock += 1000;
  p = pickOnce(core, env);
  CHECK(p.run.valid && p.run.token == ring);
  // The audio spell lasts until the pick that finds it ready.
  CHECK(p.run.info.queuedMs == 96000 && p.run.info.selfWaitMs == 96000 && p.run.info.audioMs == 91000);
  runTicket(core, env, p.run);
  CHECK(env.log.back().kind == 'f' && env.log.back().value == 91000);
  // A dropped job's finish carries its audio time.
  submit(core, env, Client::Ring);
  env.verdict[size_t(Client::Ring)] = {Ready::Wait, kAudio};
  pickOnce(core, env);
  env.clock += 1234;
  env.verdict[size_t(Client::Ring)] = {Ready::Drop, kAudioExpired};
  pickOnce(core, env);
  CHECK(env.log.back().kind == 'f' && env.log.back().reason == kAudioExpired && env.log.back().value == 1234);
  CHECK(core.idle(kAll));
}

// Queued jobs are not re-checked during another client's run, so their own
// wait spell stops counting when that run starts: the run is time spent
// waiting for another setup, not for the job's own reason (G2 audio).
static void foreignRunPausesWaits() {
  Core core;
  Env env; env.core = &core;
  submit(core, env, Client::Ring);
  env.verdict[size_t(Client::Ring)] = {Ready::Wait, kAudio};
  Pick p = pickOnce(core, env);          // the ring starts waiting for audio
  CHECK(!p.run.valid);
  env.clock += 2000;                     // 2 s of its own audio wait
  submit(core, env, Client::G2);
  env.runMs[size_t(Client::G2)] = 30000;
  p = pickOnce(core, env);               // the glasses take the turn
  CHECK(p.run.valid && p.run.client == Client::G2);
  runTicket(core, env, p.run);           // 30 s of glasses setup
  CHECK(core.peek(Client::Ring).waitReason == kAudio);  // still waiting; not logged again
  env.clock += 500;                      // then 500 ms more of its own audio wait
  pickOnce(core, env);
  CHECK(env.lastWait[size_t(Client::Ring)].audioMs == 2500);
  CHECK(env.count('w', Client::Ring) == 1);
  env.verdict[size_t(Client::Ring)] = {Ready::Run, kNone};
  p = pickOnce(core, env);
  CHECK(p.run.valid && p.run.client == Client::Ring);
  CHECK(p.run.info.queuedMs == 32500 && p.run.info.selfWaitMs == 2500 && p.run.info.audioMs == 2500);
  runTicket(core, env, p.run);
  CHECK(core.idle(kAll));
}

// A withdraw and a new submit while ready() runs: the verdict for the old
// token is never applied to the new job.
static void resubmitDuringReady() {
  Core core;
  Env env; env.core = &core;
  submit(core, env, Client::Ring);
  static uint32_t fresh;
  env.duringReady = [](Env& e, Client c, uint32_t t) {
    if (c != Client::Ring) return;
    Record taken;
    e.lock();
    CHECK(e.core->withdraw(c, t, e.clock, &taken) == Core::Withdraw::Withdrawn);
    e.unlock();
    e.finish(c, taken.token, FinishInfo{End::Withdrawn, kNone, 0});
    e.lock();
    e.core->finished(c);
    CHECK(e.core->claim(c, &fresh) && e.core->publish(c, fresh, e.clock));
    e.unlock();
    e.duringReady = nullptr;
  };
  env.verdict[size_t(Client::Ring)] = {Ready::Drop, kStale};
  const Pick p = pickOnce(core, env);
  CHECK(!p.run.valid && !p.changed);
  CHECK(env.count('f', Client::Ring) == 1 && env.log.back().end == End::Withdrawn);
  CHECK(core.live(Client::Ring, fresh) && core.peek(Client::Ring).state == State::Queued);
  // The same for a Run verdict: the stale run is refused.
  env.verdict[size_t(Client::Ring)] = {Ready::Run, kNone};
  env.duringReady = [](Env& e, Client c, uint32_t t) {
    Record taken;
    e.lock();
    CHECK(e.core->withdraw(c, t, e.clock, &taken) == Core::Withdraw::Withdrawn);
    e.unlock();
    e.finish(c, taken.token, FinishInfo{End::Withdrawn, kNone, 0});
    e.lock();
    e.core->finished(c);
    CHECK(e.core->claim(c, &fresh) && e.core->publish(c, fresh, e.clock));
    e.unlock();
    e.duringReady = nullptr;
  };
  const uint32_t waitingPhone = submit(core, env, Client::Phone);
  const Pick q = pickOnce(core, env);
  CHECK(!q.run.valid);
  CHECK(core.live(Client::Ring, fresh) && core.peek(Client::Ring).state == State::Queued);
  // Nobody ran, so the ready phone was not passed over.
  CHECK(!core.peek(Client::Phone).passedOver);
  Record phoneTaken;
  CHECK(core.withdraw(Client::Phone, waitingPhone, env.clock, &phoneTaken) == Core::Withdraw::Withdrawn);
  core.finished(Client::Phone);
  const Pick r = pickOnce(core, env);
  CHECK(r.run.valid && r.run.token == fresh);
  runTicket(core, env, r.run);
  CHECK(env.count('f', Client::Ring) == 3 && core.idle(kAll));
}

static void serveRechecks() {
  Core core;
  Env env; env.core = &core;
  serve(core, env);                                      // nothing queued: sleeps until woken
  CHECK(env.sleeps.back() == kForever);
  submit(core, env, Client::Ring);
  env.verdict[size_t(Client::Ring)] = {Ready::Wait, kRole};
  serve(core, env);                                      // queued: the 100 ms recheck
  CHECK(env.sleeps.back() == kRecheckMs && core.peek(Client::Ring).state == State::Queued);
  env.verdict[size_t(Client::Ring)] = {Ready::Drop, kStale};
  serve(core, env);                                      // a stale job ends within one recheck
  CHECK(env.sleeps.back() == kRecheckMs && core.idle(kAll) && env.count('f', Client::Ring) == 1);
  // One wake runs every ready job, glasses first.
  submit(core, env, Client::Phone);
  submit(core, env, Client::G2);
  env.verdict[size_t(Client::Ring)] = {Ready::Run, kNone};
  submit(core, env, Client::Ring);
  const size_t before = env.log.size();
  serve(core, env);
  CHECK(core.idle(kAll));
  CHECK(env.log[before].kind == 'r' && env.log[before].client == Client::G2);
}

static void tokens() {
  Core core(kTokenMask - 1);
  uint32_t a = 0, b = 0, c = 0;
  CHECK(core.claim(Client::G2, &a) && a == kTokenMask);
  core.cancelClaim(Client::G2, a);
  CHECK(core.claim(Client::G2, &b) && b == 1);           // skips 0 on wrap
  core.cancelClaim(Client::G2, b);
  CHECK(core.claim(Client::G2, &c) && c == 2 && !(c & kUsed));
}

static void noAllocations() {
  Core core;
  Env env; env.core = &core;
  const long before = gAllocs.load();
  for (int i = 0; i < 1000; ++i) {
    uint32_t t = 0;
    core.claim(Client::Ring, &t);
    core.publish(Client::Ring, t, env.clock);
    if (i & 1) {
      Record taken;
      core.withdraw(Client::Ring, t, env.clock, &taken);
      core.finished(Client::Ring);
    } else {
      const Pick p = pickOnce(core, env);
      runTicket(core, env, p.run);
    }
    env.log.clear();
  }
  CHECK(gAllocs.load() == before);
}

// Two threads: a submitter withdrawing at random against the worker's picks.
// Every published job finishes exactly once.
static void stress() {
  Core core;
  std::mutex m;
  std::atomic<uint32_t> clock{0};
  std::atomic<long> finishes{0}, published{0};
  std::atomic<bool> stop{false};
  struct ThreadEnv {
    Core& core; std::mutex& m; std::atomic<uint32_t>& clock; std::atomic<long>& finishes;
    void lock() { m.lock(); }
    void unlock() { m.unlock(); }
    uint32_t now() { return clock.load(); }
    void waitWake(uint32_t) { std::this_thread::yield(); }
    Verdict ready(Client, uint32_t t, const WaitInfo&) {
      return t % 7 == 0 ? Verdict{Ready::Drop, kStale} : t % 5 == 0 ? Verdict{Ready::Wait, kRole} : Verdict{Ready::Run, kNone};
    }
    void waiting(Client, uint32_t, uint8_t) {}
    End run(Client, uint32_t, const RunInfo&) { std::this_thread::yield(); return End::Ran; }
    void finish(Client, uint32_t, const FinishInfo&) { ++finishes; }
  };
  ThreadEnv workerEnv{core, m, clock, finishes};
  std::thread worker([&] { while (!stop.load()) { serve(core, workerEnv); } });
  std::mt19937 rng(11);
  for (int i = 0; i < 20000; ++i) {
    const Client c = Client(rng() % kClients);
    uint32_t t = 0;
    m.lock();
    const bool claimed = core.claim(c, &t);
    if (claimed && core.publish(c, t, clock.load())) ++published;
    m.unlock();
    if (rng() & 1) {
      Record taken;
      m.lock();
      const auto w = core.withdraw(c, 0, clock.load(), &taken);
      m.unlock();
      if (w == Core::Withdraw::Withdrawn) {
        ++finishes;
        m.lock(); core.finished(c); m.unlock();
      }
    }
    clock += 10;
  }
  // Drain: withdraw the slots that wait forever (Wait verdicts).
  for (int spin = 0; spin < 200000; ++spin) {
    for (size_t i = 0; i < kClients; ++i) {
      Record taken;
      m.lock();
      const auto w = core.withdraw(Client(i), 0, clock.load(), &taken);
      m.unlock();
      if (w == Core::Withdraw::Withdrawn) { ++finishes; m.lock(); core.finished(Client(i)); m.unlock(); }
    }
    m.lock(); const bool idle = core.idle(kAll); m.unlock();
    if (idle) break;
    std::this_thread::yield();
  }
  stop = true;
  worker.join();
  m.lock(); CHECK(core.idle(kAll)); m.unlock();
  CHECK(finishes.load() == published.load());
}

static void recorded() {
  struct Hooks {
    char seq[8] = {}; size_t n = 0; uint32_t logged = 0, set = 0;
    void logSetupWait(const char*, uint32_t ms) { seq[n++] = 'l'; logged = ms; }
    void setAccessoryWait(uint32_t ms) { seq[n++] = 's'; set = ms; }
    void clearAccessoryWait() { seq[n++] = 'c'; }
  } h;
  const End e = runRecorded(h, "ring saved", true, RunInfo{9000, 4000, 4000}, [&] { h.seq[h.n++] = 'r'; return End::Ran; });
  CHECK(e == End::Ran && std::strcmp(h.seq, "lsrc") == 0 && h.logged == 5000 && h.set == 5000);
  Hooks q;
  runRecorded(q, "phone", false, RunInfo{300, 0, 0}, [&] { q.seq[q.n++] = 'r'; return End::Ran; });
  CHECK(std::strcmp(q.seq, "lr") == 0 && q.logged == 300);
}

struct TurnLog {
  PhoneTurn* turn = nullptr;
  uint32_t clock = 0;
  std::vector<char> seq;
  uint32_t endAtMs = 0, endToken = 0;
  bool consumeAt = false; uint32_t consumeMs = 0;
  bool granted = false, raisedBeforeGrant = false;
  int priority = 1;
  bool blocked = false, needed = false;
  void tick() {
    if (consumeAt && clock >= consumeMs) { consumeAt = false; CHECK(consumeGrant(*turn, endToken)); }
    if (endAtMs && clock >= endAtMs) { turn->ended.store(endToken); endAtMs = 0; }
  }
};
struct TurnEnv {
  TurnLog& l;
  uint32_t now() { return l.clock; }
  void waitWake(uint32_t ms) { l.clock += ms; l.tick(); }
  void grant(uint32_t t) { l.priority = 5; l.raisedBeforeGrant = l.turn->granted.load() == 0; l.turn->granted.store(t); l.seq.push_back('g'); l.tick(); }
  void release(uint32_t) { l.priority = 1; l.seq.push_back('x'); }
  void overran(uint32_t, bool used) { l.seq.push_back(used ? 'O' : 'o'); }
  void escalate(uint32_t) { l.blocked = true; l.seq.push_back('b'); l.needed = true; l.seq.push_back('n'); }
  void settle(uint32_t) { if (l.needed) { l.needed = false; l.blocked = false; } l.seq.push_back('s'); }
};

static void phoneTurn() {
  {  // A normal turn: consumed, ended, released.
    PhoneTurn turn; TurnLog l; l.turn = &turn; l.endToken = 9;
    l.consumeAt = true; l.consumeMs = 300; l.endAtMs = 8000;
    TurnEnv env{l};
    CHECK(runPhoneTurn(turn, 9, env) == End::Ran);
    CHECK(l.raisedBeforeGrant && l.priority == 1 && turn.granted.load() == 0);
    CHECK(std::string(l.seq.begin(), l.seq.end()) == "gx" && !l.blocked);
  }
  {  // Never consumed: revoked at 75 s, no stop wait and no escalation.
    PhoneTurn turn; TurnLog l; l.turn = &turn; l.endToken = 9;
    TurnEnv env{l};
    CHECK(runPhoneTurn(turn, 9, env) == End::Overrun);
    CHECK(std::string(l.seq.begin(), l.seq.end()) == "gox" && l.clock >= kPhoneTurnCeilingMs && l.clock < kPhoneTurnCeilingMs + 200);
    CHECK(!consumeGrant(turn, 9) && l.priority == 1 && !l.blocked);
  }
  {  // Consumed, then overran: revoked; the phone stops within the bound.
    PhoneTurn turn; TurnLog l; l.turn = &turn; l.endToken = 9;
    l.consumeAt = true; l.consumeMs = 100; l.endAtMs = kPhoneTurnCeilingMs + 4000;
    TurnEnv env{l};
    CHECK(runPhoneTurn(turn, 9, env) == End::Overrun);
    CHECK(std::string(l.seq.begin(), l.seq.end()) == "gOx" && !l.blocked && !grantHeld(turn, 9));
  }
  {  // Consumed and never stopped: the block, then recoveryNeeded.
    PhoneTurn turn; TurnLog l; l.turn = &turn; l.endToken = 9;
    l.consumeAt = true; l.consumeMs = 100;
    TurnEnv env{l};
    CHECK(runPhoneTurn(turn, 9, env) == End::Overrun);
    CHECK(std::string(l.seq.begin(), l.seq.end()) == "gObnx" && l.blocked && l.needed);
    CHECK(l.clock >= kPhoneTurnCeilingMs + kPhoneLocalStopMs);
  }
  {  // The phone ends just as the worker escalates: the escalation is settled.
    PhoneTurn turn; TurnLog l; l.turn = &turn; l.endToken = 9;
    l.consumeAt = true; l.consumeMs = 100;
    struct LateEnv : TurnEnv {
      void escalate(uint32_t t) { TurnEnv::escalate(t); l.turn->ended.store(9); }
    } env{{l}};
    CHECK(runPhoneTurn(turn, 9, env) == End::Overrun);
    CHECK(std::string(l.seq.begin(), l.seq.end()) == "gObnsx" && !l.blocked && !l.needed);
  }
  {  // A turn that ends after another token's grant is not confused with it.
    PhoneTurn turn; TurnLog l; l.turn = &turn; l.endToken = 8;
    l.consumeAt = false; l.endAtMs = 500;
    TurnEnv env{l};
    turn.ended.store(0);
    l.endToken = 8;
    CHECK(runPhoneTurn(turn, 9, env) == End::Overrun);     // ended(8) never ends token 9
  }
  CHECK(!grantHeld(PhoneTurn{}, 0));
}

int main() {
  basics();
  runAndFinishOnce();
  order();
  passOver();
  dropsAndWaits();
  resubmitDuringReady();
  foreignRunPausesWaits();
  serveRechecks();
  tokens();
  noAllocations();
  recorded();
  phoneTurn();
  stress();
  std::printf("ble_setup_worker: %d checks passed\n", gChecks);
  return 0;
}
