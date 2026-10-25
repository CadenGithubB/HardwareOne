#pragma once
// One BLE setup worker: an allocation-free turn scheduler for central setup.
// Glasses, ring retirement, ring and phone each own one slot; one task gives
// the turns, in that order, and a running setup is never cancelled for another
// client's. No BLE, OS or Arduino dependencies: the caller supplies the lock,
// clock, wake and client callbacks (Env), so the host tests run this file as is.
// See docs/BLE_SETUP_WORKER_PLAN.md.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hw1setup {
// The index is the turn order.
enum class Client : uint8_t { G2 = 0, RingRetire = 1, Ring = 2, Phone = 3 };
constexpr size_t kClients = 4;
constexpr uint8_t clientMask(Client c) { return uint8_t(1u << unsigned(c)); }
constexpr uint8_t kAccessories = clientMask(Client::G2) | clientMask(Client::RingRetire) | clientMask(Client::Ring);
constexpr uint8_t kAll = kAccessories | clientMask(Client::Phone);
constexpr uint32_t kRecheckMs = 100, kForever = UINT32_MAX;
constexpr uint32_t kTokenMask = 0x7fffffffu, kUsed = 0x80000000u;

enum class State : uint8_t { Empty, Filling, Queued, Running };
enum class Ready : uint8_t { Run, Wait, Drop };
enum class End : uint8_t { Ran, Dropped, Withdrawn, Overrun };
// Why a queued job waits or was dropped.
enum Reason : uint8_t {
  kNone = 0, kStale, kFault, kRecovery, kYield, kRole, kAudio, kAudioExpired, kInactive
};
struct Verdict { Ready ready = Ready::Wait; uint8_t reason = kNone; };
// queuedMs counts from publish; audioMs is the job's total wait for G2 audio,
// which other wait reasons pause but never reset.
struct WaitInfo { uint32_t now = 0, queuedMs = 0, audioMs = 0; uint8_t reason = kNone; };
// selfWaitMs is the time the job itself was not ready (any Wait verdict), so
// queuedMs - selfWaitMs is its wait for other clients' setups.
struct RunInfo { uint32_t queuedMs = 0, selfWaitMs = 0, audioMs = 0; };
struct FinishInfo { End end = End::Ran; uint8_t reason = kNone; uint32_t audioMs = 0; };

inline bool expired(uint32_t now, uint32_t since, uint32_t span) { return uint32_t(now - since) >= span; }

struct Slot {
  State state = State::Empty;
  bool passedOver = false;
  uint8_t waitReason = kNone;  // the Wait spell in progress, kNone when ready
  uint8_t finishing = 0;       // finishes decided but not yet returned
  uint32_t token = 0, submittedMs = 0, waitSinceMs = 0;
  uint32_t passedSeq = 0;      // when it was first passed over (orders those)
  uint32_t selfWaitMs = 0, audioMs = 0;  // closed Wait spells: all, and audio only
};
struct Ticket { Client client = Client::G2; uint32_t token = 0; RunInfo info; bool valid = false; };
struct Record { Client client = Client::G2; uint32_t token = 0; uint8_t reason = kNone; uint32_t audioMs = 0; };
// What one pick decided, for the caller to act on once the lock is released.
struct Applied {
  Ticket run;
  Record drops[kClients], waits[kClients];
  size_t dropCount = 0, waitCount = 0;
};

// Not thread-safe: every member runs under the caller's lock, which is a leaf
// (no callback, log or allocation while it is held).
class Core {
 public:
  // lastToken: where the token sequence starts (tests of the wrap).
  explicit Core(uint32_t lastToken = 0) : next_(lastToken & kTokenMask) {}
  bool claim(Client c, uint32_t* token) {  // Empty -> Filling with a new token
    Slot& s = slot(c);
    if (s.state != State::Empty) return false;
    do next_ = (next_ + 1) & kTokenMask; while (!next_);
    const uint8_t finishing = s.finishing;
    s = Slot{};
    s.state = State::Filling; s.token = next_; s.finishing = finishing;
    *token = next_;
    return true;
  }
  bool publish(Client c, uint32_t token, uint32_t now) {  // Filling -> Queued
    Slot& s = slot(c);
    if (s.state != State::Filling || s.token != token) return false;
    s.state = State::Queued; s.submittedMs = now;
    return true;
  }
  bool cancelClaim(Client c, uint32_t token) {  // Filling -> Empty, no finish
    Slot& s = slot(c);
    if (s.state != State::Filling || s.token != token) return false;
    s.state = State::Empty;
    return true;
  }
  enum class Withdraw : uint8_t { Withdrawn, Running, NotLive };
  // Queued -> Empty; the caller then runs finish once. token 0 = whatever is queued.
  Withdraw withdraw(Client c, uint32_t token, uint32_t now, Record* taken) {
    Slot& s = slot(c);
    if (token && s.token != token) return Withdraw::NotLive;
    if (s.state == State::Running) return Withdraw::Running;
    if (s.state != State::Queued) return Withdraw::NotLive;
    closeWait(s, now);
    *taken = Record{c, s.token, kNone, s.audioMs};
    leave(s);
    return Withdraw::Withdrawn;
  }
  bool live(Client c, uint32_t token) const {
    const Slot& s = slots_[size_t(c)];
    return token && s.token == token && s.state != State::Empty;
  }
  bool idle(uint8_t mask) const {
    for (size_t i = 0; i < kClients; ++i)
      if ((mask & (1u << i)) && (slots_[i].state != State::Empty || slots_[i].finishing)) return false;
    return true;
  }
  bool busy(uint8_t mask) const {  // Queued or Running
    for (size_t i = 0; i < kClients; ++i)
      if ((mask & (1u << i)) &&
          (slots_[i].state == State::Queued || slots_[i].state == State::Running)) return true;
    return false;
  }
  bool anyQueued() const { return busyState(0xff, State::Queued); }
  bool otherQueued(Client self) const { return busyState(uint8_t(0xff & ~clientMask(self)), State::Queued); }
  void snapshot(Slot (&out)[kClients]) const { for (size_t i = 0; i < kClients; ++i) out[i] = slots_[i]; }
  // The commit half of a pick. A verdict counts only for the token it was
  // given for; a slot that changed since the snapshot is left alone.
  Applied apply(const Slot (&view)[kClients], const Verdict (&v)[kClients], uint32_t now);
  bool finishRun(Client c, uint32_t token) {  // Running -> Empty
    Slot& s = slot(c);
    if (s.state != State::Running || s.token != token) return false;
    leave(s);
    return true;
  }
  void finished(Client c) { Slot& s = slot(c); if (s.finishing) --s.finishing; }
  // Another client ran from `from` to `to`. Queued jobs are not re-checked
  // during a run, so an open Wait spell counts only up to `from`; the run's
  // length is their wait for another setup. The reason stays open, so
  // waiting() does not report the same wait again.
  void pauseWaits(uint32_t from, uint32_t to) {
    for (auto& s : slots_) {
      if (s.state != State::Queued || s.waitReason == kNone) continue;
      if (int32_t(from - s.waitSinceMs) > 0) {
        const uint32_t spell = from - s.waitSinceMs;
        s.selfWaitMs += spell;
        if (s.waitReason == kAudio) s.audioMs += spell;
      }
      s.waitSinceMs = to;
    }
  }
  const Slot& peek(Client c) const { return slots_[size_t(c)]; }

 private:
  Slot& slot(Client c) { return slots_[size_t(c)]; }
  bool busyState(uint8_t mask, State st) const {
    for (size_t i = 0; i < kClients; ++i) if ((mask & (1u << i)) && slots_[i].state == st) return true;
    return false;
  }
  static void closeWait(Slot& s, uint32_t now) {
    if (s.waitReason == kNone) return;
    const uint32_t spell = now - s.waitSinceMs;
    s.selfWaitMs += spell;
    if (s.waitReason == kAudio) s.audioMs += spell;
    s.waitReason = kNone;
  }
  static void leave(Slot& s) { s.state = State::Empty; s.passedOver = false; ++s.finishing; }
  Slot slots_[kClients];
  uint32_t next_ = 0, passSeq_ = 0;
};

inline uint32_t audioSoFar(const Slot& s, uint32_t now) {
  return s.audioMs + (s.waitReason == kAudio ? now - s.waitSinceMs : 0);
}

// The client to run: of the Run-ready slots that were passed over, the one
// passed over first; otherwise the first Run-ready slot in turn order; -1 if
// none is ready. So once passed over, a job waits for at most one turn of
// each other client.
inline int decide(const Slot (&view)[kClients], const Verdict (&v)[kClients]) {
  int first = -1, passed = -1;
  for (size_t i = 0; i < kClients; ++i) {
    if (view[i].state != State::Queued || v[i].ready != Ready::Run) continue;
    if (view[i].passedOver &&
        (passed < 0 || int32_t(view[i].passedSeq - view[size_t(passed)].passedSeq) < 0)) passed = int(i);
    if (first < 0) first = int(i);
  }
  return passed >= 0 ? passed : first;
}

inline Applied Core::apply(const Slot (&view)[kClients], const Verdict (&v)[kClients], uint32_t now) {
  Applied out;
  const int run = decide(view, v);
  auto same = [&](size_t i) {
    return view[i].state == State::Queued && slots_[i].state == State::Queued &&
           slots_[i].token == view[i].token;
  };
  bool ran = false;
  if (run >= 0 && same(size_t(run))) {
    Slot& s = slots_[size_t(run)];
    closeWait(s, now);
    s.state = State::Running; s.passedOver = false;
    out.run = Ticket{Client(run), s.token, RunInfo{now - s.submittedMs, s.selfWaitMs, s.audioMs}, true};
    ran = true;
  }
  for (size_t i = 0; i < kClients; ++i) {
    if (int(i) == run || !same(i)) continue;
    Slot& s = slots_[i];
    switch (v[i].ready) {
      case Ready::Drop:
        closeWait(s, now);
        out.drops[out.dropCount++] = Record{Client(i), s.token, v[i].reason, s.audioMs};
        leave(s);
        break;
      case Ready::Wait:
        if (s.waitReason != v[i].reason) {
          closeWait(s, now);
          s.waitReason = v[i].reason; s.waitSinceMs = now;
          out.waits[out.waitCount++] = Record{Client(i), s.token, v[i].reason, s.audioMs};
        }
        break;
      case Ready::Run:  // ready, but another client took the turn
        closeWait(s, now);
        if (ran && !s.passedOver) { s.passedOver = true; s.passedSeq = ++passSeq_; }
        break;
    }
  }
  return out;
}

// Env supplies: lock(), unlock(), now(), waitWake(ms), and per client
// ready(c,t,WaitInfo), waiting(c,t,reason), run(c,t,RunInfo)->End,
// finish(c,t,FinishInfo). ready() runs without the lock and must be free of
// side effects; waiting() and finish() run without it too.
struct Pick { Ticket run; bool changed = false; };

template <class Env> Pick pickOnce(Core& core, Env& env) {
  Slot view[kClients];
  env.lock();
  core.snapshot(view);
  const uint32_t now = env.now();
  env.unlock();
  Verdict v[kClients];
  for (size_t i = 0; i < kClients; ++i) {
    if (view[i].state != State::Queued) continue;
    const WaitInfo info{now, now - view[i].submittedMs, audioSoFar(view[i], now), view[i].waitReason};
    v[i] = env.ready(Client(i), view[i].token, info);
  }
  env.lock();
  const Applied a = core.apply(view, v, now);
  env.unlock();
  for (size_t i = 0; i < a.dropCount; ++i) {
    const Record& r = a.drops[i];
    env.finish(r.client, r.token, FinishInfo{End::Dropped, r.reason, r.audioMs});
    env.lock(); core.finished(r.client); env.unlock();
  }
  for (size_t i = 0; i < a.waitCount; ++i) env.waiting(a.waits[i].client, a.waits[i].token, a.waits[i].reason);
  return Pick{a.run, a.dropCount != 0};
}

template <class Env> void runTicket(Core& core, Env& env, const Ticket& t) {
  const uint32_t began = env.now();
  const End end = env.run(t.client, t.token, t.info);
  const uint32_t ended = env.now();
  env.lock();
  const bool mine = core.finishRun(t.client, t.token);
  core.pauseWaits(began, ended);
  env.unlock();
  if (!mine) return;
  env.finish(t.client, t.token, FinishInfo{end, kNone, t.info.audioMs});
  env.lock(); core.finished(t.client); env.unlock();
}

// One wake cycle: sleep until woken (or the 100 ms recheck while anything is
// queued), then run turns until nothing is ready and nothing was dropped.
template <class Env> void serve(Core& core, Env& env) {
  env.lock(); const bool queued = core.anyQueued(); env.unlock();
  env.waitWake(queued ? kRecheckMs : kForever);
  for (;;) {
    const Pick p = pickOnce(core, env);
    if (p.run.valid) runTicket(core, env, p.run);
    else if (!p.changed) break;
  }
}

// Logs the job's wait for other setups, records it on the job's connect
// records while it runs, and clears it after. Hooks: logSetupWait(label, ms),
// setAccessoryWait(ms), clearAccessoryWait().
template <class Hooks, class Run>
End runRecorded(Hooks& hooks, const char* label, bool recordWait, const RunInfo& info, Run&& run) {
  const uint32_t waited = info.queuedMs > info.selfWaitMs ? info.queuedMs - info.selfWaitMs : 0;
  hooks.logSetupWait(label, waited);
  if (recordWait) hooks.setAccessoryWait(waited);
  const End end = run();
  if (recordWait) hooks.clearAccessoryWait();
  return end;
}

// The phone runs its own setup on its own task; the worker grants it the
// turn and waits for it to end. granted holds the token, with kUsed once the
// phone consumed it; ended holds the token of the turn the phone gave back.
struct PhoneTurn { std::atomic<uint32_t> granted{0}, ended{0}; };
constexpr uint32_t kPhoneTurnCeilingMs = 75000, kPhoneLocalStopMs = 10000;
inline bool consumeGrant(PhoneTurn& t, uint32_t token) {
  uint32_t expected = token;
  return token && t.granted.compare_exchange_strong(expected, token | kUsed);
}
inline bool grantHeld(const PhoneTurn& t, uint32_t token) {
  return token && t.granted.load() == (token | kUsed);
}

// TurnEnv supplies: now(), waitWake(ms), grant(token) (raise the phone task's
// priority, then publish the grant), release(token) (restore its priority),
// overran(token, used) (log), escalate(token) (recovery block, then
// recoveryNeeded) and settle(token) (undo the escalation if the phone ended
// after all). Every exit releases.
template <class TurnEnv> End runPhoneTurn(PhoneTurn& t, uint32_t token, TurnEnv& env) {
  env.grant(token);
  const uint32_t start = env.now();
  End end = End::Ran;
  while (t.ended.load() != token) {
    if (!expired(env.now(), start, kPhoneTurnCeilingMs)) { env.waitWake(kRecheckMs); continue; }
    end = End::Overrun;
    uint32_t expected = token;
    if (t.granted.compare_exchange_strong(expected, 0)) {  // never used: nothing to stop
      env.overran(token, false);
      break;
    }
    expected = token | kUsed;
    (void)t.granted.compare_exchange_strong(expected, 0);  // the phone's tick sees the revoke and stops
    env.overran(token, true);
    const uint32_t stopAt = env.now();
    while (t.ended.load() != token && !expired(env.now(), stopAt, kPhoneLocalStopMs)) env.waitWake(kRecheckMs);
    if (t.ended.load() != token) {
      env.escalate(token);
      if (t.ended.load() == token) env.settle(token);
    }
    break;
  }
  uint32_t expected = token | kUsed;
  if (!t.granted.compare_exchange_strong(expected, 0)) {
    expected = token;
    (void)t.granted.compare_exchange_strong(expected, 0);
  }
  env.release(token);
  return end;
}
}  // namespace hw1setup
