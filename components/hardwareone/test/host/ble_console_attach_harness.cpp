// Runs BLE_ConsoleAttachCore.h as shipped: every input combination, the named
// sequences, and a random walk over a model of the links and the pipe.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "BLE_ConsoleAttachCore.h"

using namespace hw1console;
static unsigned checks = 0;
#define CHECK(c)                                                                \
  do {                                                                          \
    ++checks;                                                                   \
    if (!(c)) {                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                             \
    }                                                                           \
  } while (0)

static_assert(std::is_trivially_copyable<Attach>::value, "copied under a spinlock on BTC");
static_assert(std::is_trivially_copyable<PipeLink>::value, "copied under a spinlock on BTC");

static const Mode kModes[] = {Mode::Off, Mode::Server, Mode::ClientRegistered};

static bool closedFor(const Attach& a, uint32_t generation) {
  return a.closed && a.closedGeneration == generation;
}
static bool slotIs(const Slot& s, uint32_t generation, uint16_t connId) {
  return s.present && s.generation == generation && s.connId == connId;
}
static Attach applied(const Plan& p) { return p.next; }

// Every pipe record, slot and closed memory over two generations and two ids.
static std::vector<PipeLink> allPipes() {
  std::vector<PipeLink> out;
  for (int m = 0; m < 8; ++m) out.push_back(PipeLink{bool(m & 1), (m & 2) ? 2u : 1u, uint16_t((m & 4) ? 1 : 0)});
  return out;
}
static std::vector<Attach> allAttach() {
  std::vector<Attach> out;
  for (int m = 0; m < 64; ++m) {
    if ((m & 32) && !(m & 1)) continue;  // only a slot can have been joined
    Attach a;
    a.slot = Slot{bool(m & 1), (m & 2) ? 2u : 1u, uint16_t((m & 4) ? 1 : 0), bool(m & 32)};
    a.closed = m & 8;
    a.closedGeneration = (m & 16) ? 2u : 1u;
    out.push_back(a);
  }
  return out;
}

static void attachableTable() {
  for (Mode mode : kModes)
    for (bool on : {false, true}) CHECK(attachable(mode, on) == (mode == Mode::ClientRegistered && on));
}

static void reconcileTable() {
  const auto pipes = allPipes();
  const auto attaches = allAttach();
  unsigned created = 0, cleared = 0, kept = 0;
  for (Mode mode : kModes)
    for (int flags = 0; flags < 4; ++flags)
      for (const PipeLink& pipe : pipes)
        for (const Attach& now : attaches) {
          Inputs in;
          in.mode = mode;
          in.enabled = flags & 1;
          in.secret = flags & 2;
          in.pipe = pipe;
          const Plan p = reconcile(in, now);
          const bool live = mode == Mode::ClientRegistered && in.enabled && in.secret && pipe.valid &&
                            !closedFor(now, pipe.generation);
          CHECK(!p.server);
          // Only the slot moves; the closed memory is the link events' alone.
          CHECK(p.next.closed == now.closed && p.next.closedGeneration == now.closedGeneration);
          // Safe: a slot exists only for the pipe's live record, in client mode,
          // enabled, with a secret, and never for a closed generation.
          if (p.next.slot.present) {
            CHECK(live);
            CHECK(slotIs(p.next.slot, pipe.generation, pipe.connId));
          }
          // Live: every such record gets its slot.
          if (live) CHECK(slotIs(p.next.slot, pipe.generation, pipe.connId));
          else CHECK(!p.next.slot.present);
          // A kept slot is the same slot; anything else ends the old session
          // first and makes a new one (a new epoch), never a renamed old one.
          const bool same = sameSlot(now.slot, p.next.slot);
          CHECK(p.clear == (now.slot.present && !same));
          CHECK(p.create == (p.next.slot.present && !same));
          if (p.clear) CHECK(now.slot.present);
          // Only the app's write joins: a kept slot keeps its state, a new one
          // waits for the app, and no slot is joined.
          CHECK(!p.join);
          if (p.next.slot.present) CHECK(p.next.slot.joined == (same && now.slot.joined));
          else CHECK(!p.next.slot.joined || same);
          if (mode != Mode::ClientRegistered) CHECK(!p.create);
          // A fixpoint: a second pass changes nothing.
          const Plan again = reconcile(in, applied(p));
          CHECK(!again.clear && !again.create && sameSlot(again.next.slot, p.next.slot));
          created += p.create;
          cleared += p.clear;
          kept += now.slot.present && same;
        }
  CHECK(created && cleared && kept);  // the table reaches all three outcomes
}

static void linkEventTable() {
  const auto pipes = allPipes();
  const auto attaches = allAttach();
  for (Mode mode : kModes)
    for (uint16_t connId = 0; connId < 3; ++connId)
      for (const PipeLink& pipe : pipes)
        for (const Attach& now : attaches)
          for (int which = 0; which < 2; ++which) {
            const Plan p = which ? onDisconnect(mode, connId, pipe, now) : onConnect(mode, connId, pipe, now);
            const Plan q = linkChanged(mode, connId, pipe, now);
            CHECK(p.server == q.server && p.clear == q.clear && p.create == q.create &&
                  sameSlot(p.next.slot, q.next.slot) && p.next.closed == q.next.closed &&
                  p.next.closedGeneration == q.next.closedGeneration);
            // Server mode keeps its path; client mode never admits on a link event.
            CHECK(p.server == (mode == Mode::Server));
            CHECK(!p.create && !p.join);
            // Only a slot on that very id ends; any other link leaves it alone.
            const bool onThisId = now.slot.present && now.slot.connId == connId;
            CHECK(p.clear == onThisId);
            if (onThisId) CHECK(!p.next.slot.present && !p.next.slot.joined);
            else CHECK(sameSlot(p.next.slot, now.slot) && p.next.slot.joined == now.slot.joined);
            // The generation that rode this id is closed: the pipe's (its record
            // can outlive the link) and the slot's.
            if (pipe.valid && pipe.connId == connId) CHECK(closedFor(p.next, pipe.generation));
            else if (onThisId) CHECK(closedFor(p.next, now.slot.generation));
            else CHECK(p.next.closed == now.closed && p.next.closedGeneration == now.closedGeneration);
            // connId reuse never inherits: after any event on an id, the record
            // that named it never gets a slot again, whatever else is on.
            if (pipe.valid && pipe.connId == connId) {
              Inputs in;
              in.mode = Mode::ClientRegistered;
              in.enabled = in.secret = true;
              in.pipe = pipe;
              const Plan r = reconcile(in, p.next);
              CHECK(!r.create && !r.next.slot.present);
            }
          }
}

// The app's write: the first one on the slot's link joins it, once; any other
// id, no slot, or a joined slot changes nothing. Nothing else moves.
static void joinTable() {
  const auto attaches = allAttach();
  unsigned joined = 0;
  for (uint16_t connId = 0; connId < 3; ++connId)
    for (const Attach& now : attaches) {
      const Plan p = join(now, connId);
      const bool onThisId = now.slot.present && now.slot.connId == connId;
      CHECK(!p.server && !p.clear && !p.create);
      CHECK(p.join == (onThisId && !now.slot.joined));
      CHECK(sameSlot(p.next.slot, now.slot));
      CHECK(p.next.slot.joined == (now.slot.joined || onThisId));
      CHECK(p.next.closed == now.closed && p.next.closedGeneration == now.closedGeneration);
      CHECK(!join(applied(p), connId).join);  // idempotent
      joined += p.join;
    }
  CHECK(joined > 0);
}

// bledisconnect: only a joined slot on that id ends, nothing is closed (the
// pipe keeps the link), and the next reconcile makes a fresh slot there, waiting
// for the app again. A slot no app joined has no session to end.
static void endSessionTable() {
  const auto pipes = allPipes();
  const auto attaches = allAttach();
  for (uint16_t connId = 0; connId < 3; ++connId)
    for (const Attach& now : attaches) {
      const Plan p = endSession(now, connId);
      const bool onThisId = now.slot.present && now.slot.connId == connId && now.slot.joined;
      CHECK(!p.server && !p.create && !p.join && p.clear == onThisId);
      CHECK(onThisId ? !p.next.slot.present
                     : sameSlot(p.next.slot, now.slot) && p.next.slot.joined == now.slot.joined);
      CHECK(p.next.closed == now.closed && p.next.closedGeneration == now.closedGeneration);
      if (!onThisId) continue;
      for (const PipeLink& pipe : pipes) {
        Inputs in;
        in.mode = Mode::ClientRegistered;
        in.enabled = in.secret = true;
        in.pipe = pipe;
        const Plan r = reconcile(in, p.next);
        // The same live record attaches again, as a new slot (a new session)
        // the app has not joined.
        const bool live = pipe.valid && !closedFor(now, pipe.generation);
        CHECK(r.create == live && !r.clear && !r.next.slot.joined);
      }
    }
}

static Inputs client(PipeLink pipe, bool enabled = true, bool secret = true) {
  Inputs in;
  in.mode = Mode::ClientRegistered;
  in.enabled = enabled;
  in.secret = secret;
  in.pipe = pipe;
  return in;
}

// The named sequences. Ids: temples 0 and 1, the ring 2, the phone 3.
static void sequences() {
  const PipeLink none;
  const PipeLink ready5{true, 5, 3};
  // The phone link comes up before the console is enabled.
  {
    Attach a;
    Plan p = onConnect(Mode::ClientRegistered, 3, none, a);  // the link itself: never admitted
    CHECK(!p.server && !p.clear && !p.create && !p.next.slot.present);
    a = p.next;
    p = reconcile(client(ready5, false), a);
    CHECK(!p.create && !p.clear);
    a = p.next;
    p = reconcile(client(ready5), a);
    CHECK(p.create && !p.clear && slotIs(p.next.slot, 5, 3));
  }
  // Enabled first; then the temples, ring and phone link up, then the pipe is ready.
  {
    Attach a;
    for (uint16_t id : {0, 1, 2, 3}) {
      const Plan p = onConnect(Mode::ClientRegistered, id, none, a);
      CHECK(!p.server && !p.create && !p.clear);
      a = p.next;
      CHECK(!reconcile(client(none), a).create);  // no record: no slot
    }
    Plan p = reconcile(client(ready5), a);
    CHECK(p.create && slotIs(p.next.slot, 5, 3));
    a = p.next;
    // Temple, ring connect and drop on their own ids: the slot is untouched.
    for (uint16_t id : {0, 1, 2}) {
      p = onDisconnect(Mode::ClientRegistered, id, ready5, a);
      CHECK(!p.clear && slotIs(p.next.slot, 5, 3) && !p.next.closed);
      a = p.next;
      p = onConnect(Mode::ClientRegistered, id, ready5, a);
      CHECK(!p.clear && slotIs(p.next.slot, 5, 3));
      a = p.next;
    }
    CHECK(!reconcile(client(ready5), a).clear);
    // The phone link drops; the pipe has not noticed yet. No slot comes back.
    p = onDisconnect(Mode::ClientRegistered, 3, ready5, a);
    CHECK(p.clear && !p.next.slot.present && closedFor(p.next, 5));
    a = p.next;
    p = reconcile(client(ready5), a);
    CHECK(!p.create && !p.next.slot.present);
    // A temple reconnects on the phone's old id while the stale record stands.
    p = onConnect(Mode::ClientRegistered, 3, ready5, a);
    CHECK(!p.create && !p.clear);
    a = p.next;
    CHECK(!reconcile(client(ready5), a).create);
    // The pipe clears its record, then comes back on another id as generation 6.
    p = reconcile(client(none), a);
    CHECK(!p.create && !p.clear);
    a = p.next;
    p = reconcile(client(PipeLink{true, 6, 1}), a);
    CHECK(p.create && slotIs(p.next.slot, 6, 1));
  }
  // A new generation on the same id, or a changed id: a new slot, the old session ended.
  {
    Attach a = reconcile(client(ready5), Attach{}).next;
    Plan p = reconcile(client(PipeLink{true, 6, 3}), a);
    CHECK(p.clear && p.create && slotIs(p.next.slot, 6, 3));
    p = reconcile(client(PipeLink{true, 5, 2}), a);
    CHECK(p.clear && p.create && slotIs(p.next.slot, 5, 2));
  }
  // Detach (setting off), re-attach on the same pipe link: a fresh slot.
  {
    Attach a = reconcile(client(ready5), Attach{}).next;
    Plan p = reconcile(client(ready5, false), a);
    CHECK(p.clear && !p.create && !p.next.slot.present && !p.next.closed);
    a = p.next;
    p = reconcile(client(ready5), a);
    CHECK(!p.clear && p.create && slotIs(p.next.slot, 5, 3));
  }
  // The app joins with its first write; bledisconnect ends only a joined slot,
  // and the next reconcile makes a fresh one that waits for the app again.
  {
    Attach a = reconcile(client(ready5), Attach{}).next;
    CHECK(!a.slot.joined);
    Plan p = endSession(a, 3);
    CHECK(!p.clear && sameSlot(p.next.slot, a.slot));  // nothing joined: nothing to end
    CHECK(!join(a, 2).join);                            // a temple's write joins nothing
    p = join(a, 3);
    CHECK(p.join && p.next.slot.joined);
    a = p.next;
    p = reconcile(client(ready5), a);
    CHECK(!p.clear && !p.create && p.next.slot.joined);  // a pass keeps the joined slot
    p = endSession(a, 3);
    CHECK(p.clear && !p.next.slot.present && !p.next.closed);
    a = p.next;
    p = reconcile(client(ready5), a);
    CHECK(p.create && slotIs(p.next.slot, 5, 3) && !p.next.slot.joined);
  }
  // The secret removed: the slot ends; restored: back on the same link.
  {
    Attach a = reconcile(client(ready5), Attach{}).next;
    Plan p = reconcile(client(ready5, true, false), a);
    CHECK(p.clear && !p.create);
    a = p.next;
    CHECK(!reconcile(client(ready5, true, false), a).create);
    CHECK(reconcile(client(ready5), a).create);
  }
  // The pipe fails (record cleared) while the link stays up for a moment.
  {
    Attach a = reconcile(client(ready5), Attach{}).next;
    Plan p = reconcile(client(none), a);
    CHECK(p.clear && !p.create && !p.next.slot.present);
  }
  // A missed disconnect: the next link on that id ends the slot it would inherit.
  {
    Attach a = reconcile(client(ready5), Attach{}).next;
    Plan p = onConnect(Mode::ClientRegistered, 3, none, a);  // the pipe already cleared its record
    CHECK(p.clear && !p.next.slot.present && closedFor(p.next, 5));
    a = p.next;
    CHECK(!reconcile(client(ready5), a).create);  // a stale record of generation 5 too
  }
  // The console host torn down: Off ends the slot; server mode never makes one.
  {
    Attach a = reconcile(client(ready5), Attach{}).next;
    Inputs in = client(ready5);
    in.mode = Mode::Off;
    Plan p = reconcile(in, a);
    CHECK(p.clear && !p.create);
    in.mode = Mode::Server;
    CHECK(!reconcile(in, Attach{}).create);
    p = onConnect(Mode::Server, 3, none, Attach{});
    CHECK(p.server && !p.clear && !p.create);
    p = onDisconnect(Mode::Server, 3, none, Attach{});
    CHECK(p.server && !p.clear && !p.create);
  }
}

// A model of the four C6 links and the pipe, driven by random events. The
// pipe publishes its record while its link is up, or just after a drop that
// races READY but never once the id is reused (the pipe holds its setup turn
// until READY), and may clear it late. After every step the slot is on the
// very link the pipe ran that generation on, or on its dead and unused id;
// after every reconcile a live record has its slot.
struct Rng {
  uint64_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); }
  uint32_t below(uint32_t n) { return next() % n; }
};

static void walk(uint64_t seed, unsigned steps) {
  constexpr uint16_t kIds = 4;
  Rng rng{seed};
  uint32_t holder[kIds] = {};  // link serial on each id, 0 when free
  uint32_t serial = 0, gens = 0;
  std::vector<uint32_t> genSerial(1, 0);  // generation -> the pipe's link serial
  uint32_t pipeSerial = 0, pipeGen = 0;
  uint16_t pipeConn = 0;
  bool pipeUp = false, published = false;
  PipeLink record;
  bool enabled = true, secret = true;
  Attach a;
  unsigned made = 0, ended = 0, refused = 0, raced = 0, joins = 0;
  auto freeId = [&](uint16_t* id) {
    uint16_t ids[kIds];
    unsigned n = 0;
    for (uint16_t i = 0; i < kIds; ++i) if (!holder[i]) ids[n++] = i;
    if (!n) return false;
    *id = ids[rng.below(n)];
    return true;
  };
  auto apply = [&](const Plan& p) {
    CHECK(!p.server && !(p.clear && !a.slot.present));
    ended += p.clear;
    made += p.create;
    a = p.next;
  };
  for (unsigned step = 0; step < steps; ++step) {
    const uint32_t e = rng.below(14);
    uint16_t id = 0;
    bool reconciled = false;
    if (e == 0 && freeId(&id)) {  // a temple or the ring links up
      holder[id] = ++serial;
      apply(onConnect(Mode::ClientRegistered, id, record, a));
    } else if (e == 1 && !pipeSerial && freeId(&id)) {  // the pipe's next link
      holder[id] = ++serial;
      pipeSerial = serial;
      pipeConn = id;
      pipeGen = ++gens;
      genSerial.push_back(serial);
      pipeUp = true;
      published = false;
      apply(onConnect(Mode::ClientRegistered, id, record, a));
    } else if (e == 2) {  // any link drops
      id = uint16_t(rng.below(kIds));
      if (!holder[id]) continue;
      if (holder[id] == pipeSerial) pipeUp = false;
      holder[id] = 0;
      apply(onDisconnect(Mode::ClientRegistered, id, record, a));
    } else if (e == 3 && pipeSerial && !published && (pipeUp || !holder[pipeConn])) {
      // READY: while its link is up, or racing its drop before the id is reused.
      record = PipeLink{true, pipeGen, pipeConn};
      published = true;
      raced += !pipeUp;
    } else if (e == 4 && pipeSerial && !pipeUp) {  // the pipe notices, late
      record = PipeLink{};
      pipeSerial = 0;
    } else if (e == 5 && pipeUp && record.valid) {  // the pipe fails; the link drops later
      record = PipeLink{};
    } else if (e == 6) {
      enabled = rng.below(4) != 0;
    } else if (e == 7) {
      secret = rng.below(8) != 0;
    } else if (e == 8 || e == 9) {  // a write from a link that is up: it may join
      id = uint16_t(rng.below(kIds));
      if (!holder[id]) continue;
      const Plan p = join(a, id);
      CHECK(!p.clear && !p.create);
      CHECK(!p.join || (a.slot.present && a.slot.connId == id));
      joins += p.join;
      a = p.next;
    } else if (e == 10 && rng.below(4) == 0) {  // bledisconnect
      const Plan p = endSession(a, a.slot.connId);
      CHECK(p.clear == (a.slot.present && a.slot.joined));
      apply(p);
    } else {
      const Plan p = reconcile(client(record, enabled, secret), a);
      refused += record.valid && !p.next.slot.present;
      apply(p);
      reconciled = true;
    }
    // Safe: the slot's id is held by the link its generation ran on, or by
    // nothing (a raced READY); never by another link.
    if (a.slot.present) {
      CHECK(a.slot.generation < genSerial.size());
      CHECK(holder[a.slot.connId] == genSerial[a.slot.generation] || holder[a.slot.connId] == 0);
    } else {
      CHECK(!a.slot.joined);  // joined is a slot's state only
    }
    if (reconciled) {
      if (a.slot.present) CHECK(enabled && secret && record.valid);
      // Live: a published record whose link is up has its slot.
      const bool liveRecord = record.valid && pipeUp && holder[record.connId] == genSerial[record.generation];
      if (enabled && secret && liveRecord) CHECK(slotIs(a.slot, record.generation, record.connId));
    }
  }
  CHECK(made > 20 && ended > 20 && refused > 0 && raced > 0 && joins > 20);  // the walk reached the interesting paths
}

int main() {
  attachableTable();
  reconcileTable();
  linkEventTable();
  joinTable();
  endSessionTable();
  sequences();
  for (uint64_t seed : {0x9e3779b97f4a7c15ull, 0x2545f4914f6cdd1dull, 0xd1b54a32d192ed03ull, 42ull})
    walk(seed, 200000);
  std::printf("BLE console attach: %u checks\n", checks);
}
