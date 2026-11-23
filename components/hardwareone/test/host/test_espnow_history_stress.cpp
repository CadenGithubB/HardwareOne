// Randomized stress and adversarial test for the ESP-NOW message history core
// (System_ESPNow_HistoryCore.h, the production dependency-free header).
//
// A reference model (a std::deque of records per ring, with its own placement
// rules written from DESIGN 2.1 / 3.3 / 3.4, and its own header encoder) is
// driven in lockstep with the core: appends of random length (0..300, so also
// truncated ones), refusals, wraps and exact fits, find by seq with good, stale,
// random and hostile hints, delivery-state stamps by reqId, reassembly, the
// k-way merge and the collapse over up to 32 rings, and references kept from
// long ago. After every step the touched ring is compared with the model: the
// RingState fields, every record byte (header layout, check byte, text, zero
// padding, wrap marker), the validated walk and checkRing.
//
// The corruption part flips bytes in a copy of a ring (one byte at a known
// place, many bytes anywhere, RingState fields, random rings) and runs every
// reader and writer on it. Buffers are allocated at exactly their size, so ASan
// reports any byte read or written outside a ring; with one flip the exact
// outcome (records before the damage, then stop) is checked too.
//
// Usage: espnow_history_stress_tests [--ops N] [--seed S] [--long]
//                                    [--episode-seed X] [--max-fail N] [--verbose]
// Each failure prints the episode seed; --episode-seed X replays that episode.

#include "System_ESPNow_HistoryCore.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace eh = espnowhist;

// ---------------------------------------------------------------------------
// Random numbers (splitmix64: tiny, seedable, the same on every host)
// ---------------------------------------------------------------------------
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t u32() { return (uint32_t)next(); }
  uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1); }  // inclusive
  bool pct(uint32_t p) { return below(100) < p; }
};

static uint64_t mix64(uint64_t a, uint64_t b) {
  Rng r(a ^ (b * 0xD1342543DE82EF95ull));
  r.next();
  return r.next();
}

// ---------------------------------------------------------------------------
// Failure reporting
// ---------------------------------------------------------------------------
struct Failure {
  int line;
  std::string what;
};

struct Ctx {
  uint64_t episodeSeed = 0;
  uint64_t step = 0;
  const char* op = "";
  bool verbose = false;
} g;

[[noreturn]] static void failAt(int line, const char* expr, const char* detail) {
  char b[1024];
  snprintf(b, sizeof b, "%s%s%s [op %s, step %" PRIu64 "]", expr, detail[0] ? " -- " : "", detail, g.op,
           g.step);
  throw Failure{line, b};
}

#define CHECK(c)                                   \
  do {                                             \
    if (!(c)) failAt(__LINE__, #c, "");            \
  } while (0)
#define CHECKM(c, ...)                             \
  do {                                             \
    if (!(c)) {                                    \
      char _b[512];                                \
      snprintf(_b, sizeof _b, __VA_ARGS__);        \
      failAt(__LINE__, #c, _b);                    \
    }                                              \
  } while (0)

// ---------------------------------------------------------------------------
// Spec encoder / decoder, written from DESIGN 2.1 (independent of the core)
// ---------------------------------------------------------------------------
static uint32_t fp(uint32_t len) { return (20u + len + 3u) & ~3u; }

static uint8_t specCheck(const uint8_t* p) {
  uint32_t s = 0;
  for (uint32_t i = 0; i < 20; i++) {
    if (i == 6) continue;
    s += (uint32_t)p[i] * (2u * i + 1u);
  }
  return (uint8_t)(0x5A ^ (s & 0xFFu));
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t specFlags(bool enc, bool sent, bool unattr, uint8_t sendState) {
  return (uint8_t)((enc ? 1 : 0) | (sent ? 2 : 0) | (unattr ? 4 : 0) | ((sendState & 7) << 3));  // three state bits (ESP-NOW 1.0 step 6)
}

static void specEncode(uint8_t* p, uint32_t seq, uint32_t reqId, uint32_t ts, uint8_t len, uint8_t type,
                       uint8_t flags, uint8_t piece, uint8_t pieceTotal) {
  p[0] = 0xA5;
  p[1] = len;
  p[2] = type;
  p[3] = flags;
  p[4] = piece;
  p[5] = pieceTotal;
  p[6] = 0;
  p[7] = 0;
  put32(p + 8, seq);
  put32(p + 12, reqId);
  put32(p + 16, ts);
  p[6] = specCheck(p);
}

// ---------------------------------------------------------------------------
// Reference model
// ---------------------------------------------------------------------------
struct MRec {
  uint32_t seq, reqId, ts, off;
  uint8_t len, type, piece, pieceTotal, sendState;
  bool enc, unattr;
  std::string text;  // exactly len bytes (may contain any byte)
};

struct MRing {
  uint32_t size = 0;
  bool sent = false;
  std::deque<MRec> q;
  bool wrapped = false;
  uint32_t wrapAt = 0;
  uint32_t upper = 0;  // records in [tail, wrapAt) while wrapped
  uint32_t used = 0;
  uint32_t evicted = 0;
  uint32_t resets = 0;
  std::vector<uint32_t> evictedSeqs;  // a bounded sample of seqs that left this ring

  uint32_t tail() const { return q.empty() ? 0 : q.front().off; }
  uint32_t head() const { return q.empty() ? 0 : q.back().off + fp(q.back().len); }

  void init(uint32_t sz, bool isSent) {
    size = sz;
    sent = isSent;
    q.clear();
    wrapped = false;
    wrapAt = sz;
    upper = 0;
    used = 0;
  }

  void evictFront() {
    used -= fp(q.front().len);
    if (evictedSeqs.size() < 512) evictedSeqs.push_back(q.front().seq);
    else evictedSeqs[q.front().seq % 512] = q.front().seq;
    q.pop_front();
    evicted++;
    if (wrapped) {
      upper--;
      if (upper == 0) {
        wrapped = false;
        wrapAt = size;
      }
    }
    if (q.empty()) {
      wrapped = false;
      wrapAt = size;
      upper = 0;
    }
  }

  // Where DESIGN 3.3 places a record of footprint F; evicts as it goes.
  uint32_t place(uint32_t F, uint32_t& ev) {
    ev = 0;
    for (;;) {
      if (q.empty()) {
        wrapped = false;
        wrapAt = size;
        upper = 0;
        return 0;
      }
      if (!wrapped) {
        if (size - head() >= F) return head();
        if (tail() >= F) {
          wrapAt = head();
          wrapped = true;
          upper = (uint32_t)q.size();
          return 0;
        }
      } else {
        if (tail() - head() >= F) return head();
      }
      evictFront();
      ev++;
    }
  }

  const MRec* find(uint32_t seq) const {
    auto it = std::lower_bound(q.begin(), q.end(), seq, [](const MRec& r, uint32_t s) { return r.seq < s; });
    if (it == q.end() || it->seq != seq) return nullptr;
    return &*it;
  }
};

// ---------------------------------------------------------------------------
// One ring under test
// ---------------------------------------------------------------------------
struct TRing {
  eh::RingState st{};
  uint8_t* buf = nullptr;
  MRing m;
  uint8_t slot = 0;
  uint32_t hostileSeq = 0;  // newest record whose text carries fake headers with live/foreign seqs
};

struct StaleRef {
  int ring;
  uint32_t seq;
  uint32_t hint;
};

struct Stats {
  uint64_t appends = 0, refusals = 0, evictions = 0, wraps = 0, truncs = 0, finds = 0, staleChecks = 0,
           stamps = 0, reassembles = 0, merges = 0, collapses = 0, corruptions = 0, singleFlips = 0, fuzz = 0,
           forgeries = 0, resetsExpected = 0, resetsAtNext = 0, freePlants = 0,
           episodes = 0, steps = 0;
} S;

static void expectHeaderEq(const eh::Header& h, const MRec& r, bool sent) {
  CHECKM(h.seq == r.seq, "seq %u vs model %u", h.seq, r.seq);
  CHECKM(h.reqId == r.reqId, "reqId %u vs %u (seq %u)", h.reqId, r.reqId, r.seq);
  CHECKM(h.timestamp == r.ts, "ts %u vs %u", h.timestamp, r.ts);
  CHECKM(h.length == r.len, "length %u vs %u (seq %u)", h.length, r.len, r.seq);
  CHECKM(h.type == r.type, "type %u vs %u", h.type, r.type);
  CHECKM(h.piece == r.piece, "piece %u vs %u", h.piece, r.piece);
  CHECKM(h.pieceTotal == r.pieceTotal, "pieceTotal %u vs %u", h.pieceTotal, r.pieceTotal);
  CHECKM(h.sendState == r.sendState, "sendState %u vs %u (seq %u)", h.sendState, r.sendState, r.seq);
  CHECKM(h.encrypted == r.enc, "encrypted %d vs %d", h.encrypted, r.enc);
  CHECKM(h.sent == sent, "sent %d vs %d", h.sent, sent);
  CHECKM(h.unattributed == r.unattr, "unattributed %d vs %d", h.unattributed, r.unattr);
}

// Record bytes exactly as DESIGN 2.1 says, checked with the spec decoder.
static void expectRecordBytes(const TRing& t, const MRec& r) {
  const uint8_t* p = t.buf + r.off;
  CHECKM(r.off % 4 == 0 && r.off + fp(r.len) <= t.m.size, "model offset %u", r.off);
  CHECKM(p[0] == 0xA5, "marker %02x at %u", p[0], r.off);
  CHECKM(p[1] == r.len, "length byte %u vs %u at %u", p[1], r.len, r.off);
  CHECKM(p[2] == r.type, "type byte at %u", r.off);
  uint8_t fl = specFlags(r.enc, t.m.sent, r.unattr, r.sendState);
  CHECKM(p[3] == fl, "flags byte %02x vs %02x at %u", p[3], fl, r.off);
  CHECKM(p[4] == r.piece && p[5] == r.pieceTotal, "piece bytes at %u", r.off);
  CHECKM(p[6] == specCheck(p), "check byte %02x vs spec %02x at %u", p[6], specCheck(p), r.off);
  CHECKM(p[7] == 0, "zero byte at %u", r.off);
  CHECKM(get32(p + 8) == r.seq && get32(p + 12) == r.reqId && get32(p + 16) == r.ts,
         "little-endian seq/reqId/ts at %u", r.off);
  CHECKM(memcmp(p + 20, r.text.data(), r.len) == 0, "text bytes of seq %u at %u", r.seq, r.off);
  for (uint32_t i = 20u + r.len; i < fp(r.len); i++) CHECKM(p[i] == 0, "padding byte %u of seq %u not 0", i, r.seq);
}

static void expectStateEq(const TRing& t) {
  const eh::RingState& s = t.st;
  const MRing& m = t.m;
  CHECKM(s.size == m.size, "size %u vs %u", s.size, m.size);
  CHECKM(s.sent == (m.sent ? 1 : 0), "sent %u", s.sent);
  CHECKM(s.count == m.q.size(), "count %u vs model %zu", s.count, m.q.size());
  CHECKM(s.used == m.used, "used %u vs model %u", s.used, m.used);
  CHECKM(s.tail == m.tail(), "tail %u vs model %u", s.tail, m.tail());
  CHECKM(s.head == m.head(), "head %u vs model %u", s.head, m.head());
  CHECKM(s.wrapped == (m.wrapped ? 1 : 0), "wrapped %u vs model %d", s.wrapped, m.wrapped);
  CHECKM(s.wrapAt == m.wrapAt, "wrapAt %u vs model %u", s.wrapAt, m.wrapAt);
  uint32_t os = m.q.empty() ? 0 : m.q.front().seq, ns = m.q.empty() ? 0 : m.q.back().seq;
  CHECKM(s.oldestSeq == os, "oldestSeq %u vs %u", s.oldestSeq, os);
  CHECKM(s.newestSeq == ns, "newestSeq %u vs %u", s.newestSeq, ns);
  CHECKM(s.evicted == m.evicted, "evicted %u vs %u", s.evicted, m.evicted);
  CHECKM(s.resets == m.resets, "resets %u vs %u", s.resets, m.resets);
  CHECK(eh::stateSane(s));
  if (m.wrapped && m.wrapAt < m.size) CHECKM(t.buf[m.wrapAt] == 0x5A, "wrap marker at %u is %02x", m.wrapAt, t.buf[m.wrapAt]);
}

static void verifyRing(TRing& t, bool deep, Rng& rng) {
  expectStateEq(t);
  if (!deep) return;
  eh::CheckResult cr = eh::checkRing(t.st, t.buf);
  CHECKM(cr.ok, "checkRing: %s at %u", cr.what ? cr.what : "?", cr.at);
  eh::Cursor c = eh::begin(t.st);
  CHECK(c.bad == 0);
  size_t i = 0;
  uint32_t off = 0;
  eh::Header h{};
  while (eh::next(t.st, t.buf, c, off, h)) {
    CHECKM(i < t.m.q.size(), "walk yields more than %zu records", t.m.q.size());
    const MRec& r = t.m.q[i];
    CHECKM(off == r.off, "walk offset %u vs model %u (record %zu)", off, r.off, i);
    expectHeaderEq(h, r, t.m.sent);
    expectRecordBytes(t, r);
    // copyText at an exact-size buffer; sometimes a short cap
    size_t cap = rng.pct(70) ? (size_t)r.len + 1 : (size_t)rng.range(1, r.len + 2);
    char* out = new char[cap];
    size_t n = eh::copyText(t.buf, off, h, out, cap);
    size_t want = std::min<size_t>(r.len, cap - 1);
    CHECKM(n == want, "copyText returned %zu, want %zu", n, want);
    CHECK(memcmp(out, r.text.data(), want) == 0);
    CHECK(out[want] == 0);
    delete[] out;
    i++;
  }
  CHECKM(c.bad == 0, "walk went bad after %zu records", i);
  CHECKM(i == t.m.q.size(), "walk yielded %zu, model %zu", i, t.m.q.size());
  CHECK(!eh::next(t.st, t.buf, c, off, h));  // stays at the end
}

// ---------------------------------------------------------------------------
// Episode
// ---------------------------------------------------------------------------
static const uint32_t kSizes[] = {1024, 1028, 1100, 1104, 2048, 4096, 16384, 32768, 65536};

struct Episode {
  Rng rng;
  std::vector<TRing> rings;
  std::vector<uint8_t*> owned;  // allocations to free
  uint32_t seq = 0;
  std::vector<uint32_t> reqPool;
  std::vector<StaleRef> stale;
  bool hostileLive = false;  // fakes may carry live and foreign seqs (then only API hints are used)
  int lenMode = 0;
  uint16_t scratch[256] = {};
  std::vector<uint32_t> chain;

  explicit Episode(uint64_t seed) : rng(seed) {}
  ~Episode() {
    for (uint8_t* p : owned) delete[] p;
  }

  void fillGarbage(uint8_t* p, uint32_t n) {
    uint32_t mode = rng.below(4);
    if (mode == 0) memset(p, 0xA5, n);  // what the store's allocation double fills
    else if (mode == 1) memset(p, 0, n);
    else {
      for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)rng.u32();
      if (mode == 3) {  // plant valid-looking headers everywhere
        for (uint32_t o = 0; o + 20 <= n; o += 4 * rng.range(1, 16))
          specEncode(p + o, rng.range(1, 0xFFFFFFFFu), rng.u32(), rng.u32(), (uint8_t)rng.u32(),
                     (uint8_t)rng.below(8), (uint8_t)rng.below(32), (uint8_t)rng.u32(), (uint8_t)rng.u32());
      }
    }
  }

  void setup() {
    int peers;
    uint32_t r = rng.below(100);
    if (r < 55) peers = 1;
    else if (r < 85) peers = (int)rng.range(2, 4);
    else peers = 16;  // 32 sources: the merge maximum
    uint32_t size;
    uint32_t sr = rng.below(100);
    if (sr < 45) size = kSizes[rng.below(4)];
    else if (sr < 75) size = kSizes[rng.below(7)];
    else if (sr < 85) size = kSizes[rng.below(9)];
    else size = 4 * rng.range(256, 2048);  // any multiple of 4 up to 8 KB
    if (peers == 16 && size > 4096) size = kSizes[rng.below(4)];
    bool perRingSize = rng.pct(15);
    bool blockLayout = rng.pct(40);  // [received][sent] back to back, as in the peer block
    hostileLive = rng.pct(20);
    lenMode = (int)rng.below(5);
    seq = rng.pct(85) ? rng.range(1, 1000) : rng.range(0xFFFF0000u, 0xFFFFF000u);
    for (int i = 0; i < 8; i++) reqPool.push_back(rng.range(1, 0xFFFFFFFFu));
    reqPool.push_back(1);
    rings.resize((size_t)peers * 2);
    for (int p = 0; p < peers; p++) {
      uint32_t s0 = size, s1 = size;
      if (perRingSize) {
        s0 = kSizes[rng.below(5)];
        s1 = kSizes[rng.below(5)];
      }
      uint8_t* b0;
      uint8_t* b1;
      if (blockLayout && s0 == s1) {
        uint8_t* blk = new uint8_t[(size_t)s0 * 2];
        owned.push_back(blk);
        b0 = blk;
        b1 = blk + s0;
        fillGarbage(blk, s0 * 2);
      } else {
        b0 = new uint8_t[s0];
        b1 = new uint8_t[s1];
        owned.push_back(b0);
        owned.push_back(b1);
        fillGarbage(b0, s0);
        fillGarbage(b1, s1);
      }
      for (int d = 0; d < 2; d++) {
        TRing& t = rings[(size_t)p * 2 + d];
        t.buf = d ? b1 : b0;
        t.slot = (uint8_t)p;
        uint32_t sz = d ? s1 : s0;
        memset(&t.st, rng.pct(50) ? 0xEE : 0, sizeof t.st);  // ringInit must set every field
        CHECK(eh::ringInit(t.st, sz, d == 1));
        t.m.init(sz, d == 1);
        verifyRing(t, true, rng);
      }
    }
  }

  uint32_t pickLen(const TRing& t) {
    uint32_t mode = (uint32_t)lenMode;
    if (rng.pct(30)) mode = rng.below(5);
    switch (mode) {
      case 0: return rng.below(301);
      case 1: return rng.pct(20) ? 0 : rng.range(1, 24);
      case 2: return rng.range(240, 300);
      case 3: {  // aim at an exact fit or a small gap at the end of the ring
        uint32_t room = t.m.wrapped ? t.m.tail() - t.m.head() : t.m.size - t.m.head();
        uint32_t want = room - 4 * rng.below(3);
        if (want >= 20 && want <= eh::kMaxFootprint) {
          uint32_t len = want - 20, back = rng.pct(50) ? rng.below(4) : 0;
          return len >= back ? len - back : len;
        }
        return rng.below(256);
      }
      default: return rng.pct(50) ? 0 : 255;
    }
  }

  uint32_t nextSeq() {
    uint32_t gap = 1;
    if (rng.pct(10)) gap = rng.range(2, 50);
    if (rng.pct(1)) gap = rng.range(1000, 1000000);
    if (seq > 0xFFFFFFFFu - gap) return 0;
    seq += gap;
    return seq;
  }

  std::string makeText(TRing& t, uint32_t len, uint32_t pos, bool& hostile) {
    std::string s(len, '\0');
    uint32_t mode = rng.below(10);
    hostile = false;
    if (mode < 4) {
      for (auto& ch : s) ch = (char)rng.range(32, 126);
    } else if (mode < 6) {
      for (auto& ch : s) {
        uint32_t k = rng.below(6);
        ch = (char)(k == 0 ? 0xA5 : k == 1 ? 0x5A : k == 2 ? 0 : (uint8_t)rng.u32());
      }
    } else {
      // fake headers at every place that is 4-aligned in the ring
      for (auto& ch : s) ch = (char)(uint8_t)rng.u32();
      uint32_t first = (4 - ((pos + 20) % 4)) % 4;
      for (uint32_t k = first; k + 20 <= len; k += 4 * rng.range(1, 3)) {
        uint32_t fseq;
        bool liveish = hostileLive && rng.pct(60);
        if (liveish) {
          // a live seq of this ring, a seq of another ring, or the next seq
          uint32_t c = rng.below(3);
          if (c == 0 && !t.m.q.empty()) fseq = t.m.q[rng.below((uint32_t)t.m.q.size())].seq;
          else if (c == 1) fseq = seq + rng.range(1, 3);
          else fseq = rng.range(1, seq + 1);
          hostile = true;
        } else if (!t.m.evictedSeqs.empty()) {
          fseq = t.m.evictedSeqs[rng.below((uint32_t)t.m.evictedSeqs.size())];  // dead for good
        } else {
          continue;
        }
        uint8_t flen = (uint8_t)rng.below(256);
        uint8_t fl = specFlags(rng.pct(50), rng.pct(90) ? t.m.sent : !t.m.sent, rng.pct(30), (uint8_t)rng.below(4));
        uint8_t tmp[20];
        specEncode(tmp, fseq, reqPool[rng.below((uint32_t)reqPool.size())], rng.u32(), flen, (uint8_t)rng.below(8),
                   fl, (uint8_t)rng.below(4), (uint8_t)rng.below(4));
        memcpy(&s[k], tmp, 20);
      }
      if (len > 0 && rng.pct(30)) s[rng.below(len)] = (char)0x5A;
    }
    // Stale-reference attack: where an old ref of this ring points into the new text, plant a valid header
    // carrying that ref's seq (it is dead: a live record never lies inside a new one). The ref must be refused.
    if (rng.pct(60)) {
      int me = (int)(&t - rings.data());
      for (const StaleRef& r : stale) {
        if (r.ring != me || r.hint % 4 != 0 || r.hint < pos + 20 || r.hint + 20 > pos + 20 + len) continue;
        if (t.m.find(r.seq)) continue;  // cannot happen; keep the attack honest
        uint32_t k = r.hint - pos - 20;
        uint32_t room = pos + 20 + len - r.hint;  // bytes up to the end of the new record's text
        uint8_t flen = (uint8_t)(room >= 20 ? std::min<uint32_t>(room - 20, rng.below(64)) : 0);
        uint8_t tmp[20];
        specEncode(tmp, r.seq, reqPool[rng.below((uint32_t)reqPool.size())], rng.u32(), flen, (uint8_t)rng.below(8),
                   specFlags(rng.pct(50), t.m.sent, false, (uint8_t)rng.below(4)), 1, 1);
        memcpy(&s[k], tmp, 20);
      }
    }
    return s;
  }

  // ------------------------------------------------------------------- ops
  void opAppend(TRing& t) {
    g.op = "append";
    S.appends++;
    // refusals first (sometimes)
    if (rng.pct(6)) {
      eh::Header h{};
      uint32_t kind = rng.below(5);
      h.seq = seq + 1;
      h.type = (uint8_t)rng.below(8);
      h.sendState = (uint8_t)rng.below(8);
      const char* text = "x";
      size_t tl = 1;
      if (kind == 0) h.seq = 0;
      else if (kind == 1) {
        if (t.m.q.empty()) return;
        h.seq = rng.pct(50) ? t.m.q.back().seq : rng.range(1, t.m.q.back().seq);
      } else if (kind == 2) h.type = (uint8_t)rng.range(8, 255);
      else if (kind == 3) h.sendState = (uint8_t)rng.range(8, 255);
      else {
        text = nullptr;
        tl = rng.range(1, 300);
      }
      std::vector<uint8_t> snap(t.buf, t.buf + t.st.size);
      eh::RingState ss = t.st;
      eh::AppendResult ar = eh::append(t.st, t.buf, h, text, tl);
      CHECKM(!ar.ok, "refusal kind %u accepted", kind);
      CHECK(!ar.insane);
      CHECKM(memcmp(&ss, &t.st, sizeof ss) == 0, "refusal kind %u changed RingState", kind);
      CHECKM(memcmp(snap.data(), t.buf, snap.size()) == 0, "refusal kind %u changed ring bytes", kind);
      S.refusals++;
      return;
    }
    uint32_t s = nextSeq();
    if (s == 0) return;
    uint32_t textLen = pickLen(t);
    uint32_t len = std::min<uint32_t>(textLen, 255);
    MRec r{};
    r.seq = s;
    r.reqId = rng.pct(40) ? 0 : reqPool[rng.below((uint32_t)reqPool.size())];
    r.ts = rng.pct(90) ? s * 7u + rng.below(5) : rng.u32();
    r.len = (uint8_t)len;
    r.type = (uint8_t)rng.below(8);
    uint32_t pr = rng.below(10);
    if (pr < 4) {
      r.piece = 1;
      r.pieceTotal = 1;
    } else if (pr < 8) {
      r.pieceTotal = (uint8_t)rng.range(2, 5);
      r.piece = (uint8_t)rng.range(1, r.pieceTotal);
    } else {
      r.piece = (uint8_t)rng.u32();
      r.pieceTotal = (uint8_t)rng.u32();
    }
    r.sendState = (uint8_t)rng.below(8);
    r.enc = rng.pct(50);
    r.unattr = rng.pct(20);
    // model first: it says where the record goes, so fakes can be aligned
    uint32_t ev = 0;
    const uint32_t bHead = t.m.head(), bTail = t.m.tail(), bSize = t.m.size;
    const bool bWrapped = t.m.wrapped;
    uint32_t pos = t.m.place(fp(len), ev);
    bool hostile = false;
    std::string full = makeText(t, textLen, pos, hostile);
    r.text = full.substr(0, len);
    r.off = pos;
    bool wrappedNow = t.m.wrapped && !bWrapped;
    t.m.q.push_back(r);
    t.m.used += fp(len);
    if (hostile) t.hostileSeq = s;

    eh::Header h{};
    h.seq = r.seq;
    h.reqId = r.reqId;
    h.timestamp = r.ts;
    h.length = (uint8_t)rng.u32();  // ignored on input
    h.type = r.type;
    h.piece = r.piece;
    h.pieceTotal = r.pieceTotal;
    h.sendState = r.sendState;
    h.encrypted = r.enc;
    h.sent = rng.pct(50);  // ignored on input
    h.unattributed = r.unattr;
    const char* tp = textLen ? full.data() : (rng.pct(50) ? nullptr : full.data());
    // the text is given in an exact-size heap buffer so an over-read shows
    char* exact = nullptr;
    if (tp && textLen) {
      exact = new char[textLen];
      memcpy(exact, full.data(), textLen);
      tp = exact;
    }
    eh::AppendResult ar = eh::append(t.st, t.buf, h, tp, textLen);
    delete[] exact;
    CHECKM(ar.ok, "append refused (len %u, seq %u)", textLen, s);
    CHECK(!ar.insane);
    CHECKM(!ar.reset, "append reset a healthy ring");
    CHECKM(ar.truncated == (textLen > 255), "truncated flag %d for len %u", ar.truncated, textLen);
    CHECKM(ar.evicted == ev, "evicted %u, model %u (len %u)", ar.evicted, ev, textLen);
    CHECKM(ar.offset == pos, "record at %u, model %u (len %u, head %u tail %u wrapped %d size %u)", ar.offset, pos,
           len, bHead, bTail, bWrapped, bSize);
    if (ev) S.evictions += ev;
    if (wrappedNow) S.wraps++;
    if (textLen > 255) S.truncs++;
    // I9: an append that evicted leaves at most the wrap gap plus one record unused
    if (ev) CHECKM(t.st.used + 2 * eh::kMaxFootprint >= t.st.size, "I9: used %u of %u after evicting", t.st.used, t.st.size);
    expectRecordBytes(t, t.m.q.back());
    if (rng.pct(15)) {
      StaleRef sr{(int)(&t - rings.data()), s, pos};
      if (stale.size() < 256) stale.push_back(sr);
      else stale[rng.below(256)] = sr;
    }
  }

  bool randomHintsOk(const TRing& t) const {
    return t.hostileSeq == 0 || t.m.q.empty() || t.m.q.front().seq > t.hostileSeq;
  }

  void expectFind(TRing& t, uint32_t sq, uint32_t hint) {
    eh::Header h{};
    uint32_t off = 0xDEADBEEF;
    bool damaged = false;
    bool found = eh::findBySeq(t.st, t.buf, sq, hint, off, h, rng.pct(50) ? &damaged : nullptr);
    const MRec* r = t.m.find(sq);
    CHECKM(found == (r != nullptr), "findBySeq(seq %u, hint %u) = %d, model live %d", sq, hint, found, r != nullptr);
    CHECK(!damaged);
    if (r) {
      CHECKM(off == r->off, "findBySeq offset %u, model %u (hint %u)", off, r->off, hint);
      expectHeaderEq(h, *r, t.m.sent);
    }
  }

  void opFind(TRing& t) {
    g.op = "findBySeq";
    S.finds++;
    bool rh = randomHintsOk(t);
    uint32_t k = rng.below(10);
    const MRing& m = t.m;
    uint32_t sq = 0, hint = 0;
    if (k == 9) {
      plantInFreeSpace(t);
      return;
    }
    if (!m.q.empty() && k < 4) {
      const MRec& r = m.q[rng.below((uint32_t)m.q.size())];
      sq = r.seq;
      if (k == 0) hint = r.off;
      else if (k == 1) hint = rng.pct(50) ? eh::kNoEntry : r.off + rng.range(1, 3);
      else if (k == 2) hint = rh ? 4 * rng.below(m.size / 4 + 2) : r.off;
      else hint = rh ? rng.u32() : eh::kNoEntry;
    } else if (k == 4 && !m.evictedSeqs.empty()) {
      sq = m.evictedSeqs[rng.below((uint32_t)m.evictedSeqs.size())];
      hint = 4 * rng.below(m.size / 4);
    } else if (k == 5) {
      sq = rng.pct(50) ? 0 : (m.q.empty() ? seq + 1 : m.q.back().seq + rng.range(1, 100));
      hint = 4 * rng.below(m.size / 4);
    } else if (k == 6 && rh) {
      // a seq somewhere in the global range: usually another ring's
      sq = rng.range(1, seq ? seq : 1);
      hint = 4 * rng.below(m.size / 4 + 2);
    } else {
      // hints at the ends of each segment (no read past it)
      if (m.q.empty()) return;
      const MRec& r = m.q[rng.below((uint32_t)m.q.size())];
      sq = r.seq;
      uint32_t ends[] = {m.head(), m.wrapAt, m.size, m.tail()};
      uint32_t e = ends[rng.below(4)];
      uint32_t back = 4 * rng.below(6);
      hint = e >= back ? e - back : e;
      if (!rh && hint != r.off) hint = eh::kNoEntry;
      if (rng.pct(10)) hint = 0xFFFFFFFCu;
    }
    expectFind(t, sq, hint);
  }

  // Free space may hold anything, also a valid header that carries a live seq of this ring (old text, old
  // garbage). A hint into free space must never resolve there: the segment check comes first (DESIGN 3.6).
  void plantInFreeSpace(TRing& t) {
    const MRing& m = t.m;
    if (m.q.empty()) return;
    std::vector<std::pair<uint32_t, uint32_t>> free;
    if (!m.wrapped) {
      free.push_back({m.head(), m.size});
      free.push_back({0, m.tail()});
    } else {
      free.push_back({m.head(), m.tail()});
      if (m.wrapAt < m.size) free.push_back({m.wrapAt + 4, m.size});
    }
    std::vector<std::pair<uint32_t, uint32_t>> ok;
    for (auto& f : free)
      if (f.second >= f.first + 20) ok.push_back(f);
    if (ok.empty()) return;
    auto f = ok[rng.below((uint32_t)ok.size())];
    uint32_t hint = f.first + 4 * rng.below((f.second - f.first - 20) / 4 + 1);
    const MRec& r = m.q[rng.below((uint32_t)m.q.size())];
    uint32_t room = f.second - hint;
    uint8_t flen = (uint8_t)std::min<uint32_t>(room - 20, rng.below(256));
    specEncode(t.buf + hint, r.seq, r.reqId, r.ts, flen, r.type, specFlags(r.enc, m.sent, r.unattr, r.sendState),
               r.piece, r.pieceTotal);
    S.freePlants++;
    expectFind(t, r.seq, hint);
  }

  void opStale() {
    g.op = "staleRefs";
    for (const StaleRef& s : stale) {
      S.staleChecks++;
      TRing& t = rings[(size_t)s.ring];
      eh::Header h{};
      uint32_t off = 0;
      bool found = eh::findBySeq(t.st, t.buf, s.seq, s.hint, off, h);
      const MRec* r = t.m.find(s.seq);
      CHECKM(found == (r != nullptr), "stale ref seq %u hint %u: found %d, model live %d (oldest %u)", s.seq, s.hint,
             found, r != nullptr, t.m.q.empty() ? 0 : t.m.q.front().seq);
      if (r) {
        CHECKM(off == r->off, "stale ref resolved to %u, model %u", off, r->off);
        expectHeaderEq(h, *r, t.m.sent);
      }
    }
  }

  void opStamp(TRing& t) {
    g.op = "setSendState";
    S.stamps++;
    uint32_t reqId;
    uint32_t k = rng.below(10);
    if (k < 6 && !t.m.q.empty()) reqId = t.m.q[rng.below((uint32_t)t.m.q.size())].reqId;
    else if (k < 9) reqId = reqPool[rng.below((uint32_t)reqPool.size())];
    else reqId = 0;
    uint8_t state = rng.pct(85) ? (uint8_t)rng.below(8) : (uint8_t)rng.range(8, 255);
    std::vector<uint8_t> snap(t.buf, t.buf + t.st.size);
    eh::RingState ss = t.st;
    bool damaged = false;
    int n = eh::setSendState(t.st, t.buf, reqId, state, rng.pct(50) ? &damaged : nullptr);
    std::vector<uint8_t> allowed(t.st.size, 0);
    int want = 0;
    if (state <= 7 && reqId != 0) {
      for (MRec& r : t.m.q)
        if (r.reqId == reqId) {
          r.sendState = state;
          want++;
          allowed[r.off + 3] = allowed[r.off + 6] = 1;
        }
    }
    CHECKM(n == want, "setSendState(reqId %u, state %u) = %d, model %d", reqId, state, n, want);
    CHECK(!damaged);
    CHECKM(memcmp(&ss, &t.st, sizeof ss) == 0, "setSendState changed RingState");
    for (uint32_t i = 0; i < t.st.size; i++)
      CHECKM(snap[i] == t.buf[i] || allowed[i], "setSendState changed byte %u (not a flags/check byte of a match)", i);
  }

  void opReassemble(TRing& t) {
    g.op = "reassemble";
    S.reassembles++;
    uint32_t reqId;
    uint32_t k = rng.below(10);
    if (k < 7 && !t.m.q.empty()) reqId = t.m.q[rng.below((uint32_t)t.m.q.size())].reqId;
    else if (k < 9) reqId = reqPool[rng.below((uint32_t)reqPool.size())];
    else reqId = 0;
    size_t cap;
    uint32_t c = rng.below(10);
    if (c == 0) cap = rng.below(3);
    else if (c < 5) cap = rng.range(1, 64);
    else if (c < 9) cap = rng.range(64, 1500);
    else cap = 70000;
    bool nullOut = rng.pct(3);
    char* out = (cap && !nullOut) ? new char[cap] : nullptr;
    if (out) memset(out, 0x77, cap);
    bool complete = true;
    bool* cp = rng.pct(80) ? &complete : nullptr;
    bool damaged = false;
    int n = eh::reassemble(t.st, t.buf, reqId, out, cap, cp, scratch, rng.pct(50) ? &damaged : nullptr);
    for (int i = 0; i < 256; i++) CHECKM(scratch[i] == 0, "piece scratch[%d] = %u after reassemble", i, scratch[i]);
    CHECK(!damaged);
    // model: today's espnowReassembleByReqId (S.cpp) on the model ring
    std::string want;
    bool wantComplete = false;
    if (out && cap > 0 && reqId != 0) {
      unsigned total = 0;
      int idx[256];
      for (int& v : idx) v = -1;
      bool anySingle = false;
      for (size_t i = 0; i < t.m.q.size(); i++) {
        const MRec& r = t.m.q[i];
        if (r.reqId != reqId) continue;
        if (r.pieceTotal <= 1) {  // single-frame records are not fragments of the message
          anySingle = true;
          continue;
        }
        if (total == 0) total = r.pieceTotal;
        if (r.piece >= 1 && r.piece <= total && idx[r.piece] < 0) idx[r.piece] = (int)i;
      }
      if (total == 0 && anySingle) {  // no fragment: the oldest single record with piece 1
        total = 1;
        for (size_t i = 0; i < t.m.q.size(); i++) {
          const MRec& r = t.m.q[i];
          if (r.reqId == reqId && r.pieceTotal <= 1 && r.piece == 1) {
            idx[1] = (int)i;
            break;
          }
        }
      }
      unsigned found = 0;
      bool stopped = false;
      for (unsigned p = 1; p <= total; p++) {
        if (idx[p] < 0 || stopped) continue;
        const MRec& r = t.m.q[(size_t)idx[p]];
        size_t take = std::min<size_t>(r.len, cap - 1 - want.size());
        want.append(r.text.data(), take);
        found++;
        if (want.size() >= cap - 1) stopped = true;
      }
      wantComplete = total > 0 && found >= total;
    }
    if (!out || cap == 0) {
      CHECKM(n == 0, "reassemble with no output returned %d", n);
    } else {
      CHECKM(n == (int)want.size(), "reassemble(reqId %u, cap %zu) = %d, model %zu", reqId, cap, n, want.size());
      CHECK(memcmp(out, want.data(), want.size()) == 0);
      CHECK(out[want.size()] == 0);
    }
    if (cp) CHECKM(complete == wantComplete, "complete %d, model %d (reqId %u, cap %zu, n %d)", complete,
                   wantComplete, reqId, cap, n);
    delete[] out;
  }

  // sources: a random subset of the rings in random order
  std::vector<int> pickSources(int maxCount) {
    std::vector<int> idx(rings.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
    for (size_t i = idx.size(); i > 1; i--) std::swap(idx[i - 1], idx[rng.below((uint32_t)i)]);
    int n = (int)rng.range(0, (uint32_t)std::min<int>(maxCount, (int)idx.size()));
    if (rng.pct(40)) n = (int)std::min<size_t>(idx.size(), (size_t)maxCount);
    idx.resize((size_t)n);
    return idx;
  }

  struct MItem {
    int src;
    const MRec* r;
  };
  std::vector<MItem> mergedModel(const std::vector<int>& srcs, uint32_t since) {
    std::vector<MItem> v;
    for (size_t i = 0; i < srcs.size(); i++)
      for (const MRec& r : rings[(size_t)srcs[i]].m.q)
        if (r.seq > since) v.push_back({(int)i, &r});
    std::sort(v.begin(), v.end(), [](const MItem& a, const MItem& b) { return a.r->seq < b.r->seq; });
    return v;
  }

  void opBadArgs() {
    g.op = "badArgs";
    std::vector<eh::Source> src;
    for (TRing& t : rings) src.push_back({&t.st, t.buf, t.slot});
    static eh::MergeScratch ms;
    int calls = 0;
    auto v = [&](int, uint32_t, const eh::Header&) {
      calls++;
      return true;
    };
    int bad[] = {-1, eh::kMaxSources + 1, -1000};
    for (int n : bad) CHECKM(eh::mergeBySeq(src.data(), n, 0, ms, v) == 0 && calls == 0, "merge with n %d", n);
    CHECK(eh::mergeBySeq(nullptr, (int)src.size(), 0, ms, v) == 0 && calls == 0);
    eh::Ref out[8];
    uint32_t ch[8];
    memset(out, 0xAB, sizeof out);
    eh::Ref before[8];
    memcpy(before, out, sizeof out);
    int s = (int)src.size();
    CHECK(eh::collapseNewest(src.data(), s, out, 8, ms, ch, 7) == 0);    // maxN > chainCap
    CHECK(eh::collapseNewest(src.data(), s, nullptr, 8, ms, ch, 8) == 0);
    CHECK(eh::collapseNewest(src.data(), s, out, 8, ms, nullptr, 8) == 0);
    CHECK(eh::collapseNewest(src.data(), s, out, 0, ms, ch, 8) == 0);
    CHECK(eh::collapseNewest(src.data(), s, out, -3, ms, ch, 8) == 0);
    CHECK(eh::collapseNewest(src.data(), -1, out, 8, ms, ch, 8) == 0);
    CHECK(eh::collapseNewest(src.data(), eh::kMaxSources + 1, out, 8, ms, ch, 8) == 0);
    CHECK(eh::collapseNewest(nullptr, s, out, 8, ms, ch, 8) == 0);
    CHECKM(memcmp(out, before, sizeof out) == 0, "collapse with bad arguments wrote its output");
    eh::sortByTime(nullptr, 5);
    eh::sortByTime(out, 0);
    eh::sortByTime(out, -1);
    CHECK(memcmp(out, before, sizeof out) == 0);
    // copyText / reassemble with no room
    TRing& t = rings[0];
    if (!t.m.q.empty()) {
      eh::Header h{};
      uint32_t off = 0;
      CHECK(eh::findBySeq(t.st, t.buf, t.m.q.front().seq, t.m.q.front().off, off, h));
      CHECK(eh::copyText(t.buf, off, h, nullptr, 10) == 0);
      char one = 'x';
      CHECK(eh::copyText(t.buf, off, h, &one, 0) == 0 && one == 'x');
      bool complete = true;
      CHECK(eh::reassemble(t.st, t.buf, t.m.q.front().reqId, &one, 0, &complete, scratch) == 0 && one == 'x' &&
            !complete);
    }
  }

  // sinceSeq paging: repeated calls from the last seq seen visit every record once, ascending
  void opPage() {
    g.op = "paging";
    std::vector<int> srcIdx = pickSources(eh::kMaxSources);
    std::vector<eh::Source> src;
    for (int i : srcIdx) src.push_back({&rings[(size_t)i].st, rings[(size_t)i].buf, rings[(size_t)i].slot});
    std::vector<MItem> want = mergedModel(srcIdx, 0);
    uint32_t page = rng.range(1, 50);
    uint32_t since = 0;
    std::vector<uint32_t> got;
    static eh::MergeScratch ms;
    for (int guard = 0; guard < 100000; guard++) {
      uint32_t n = 0;
      eh::mergeBySeq(src.data(), (int)src.size(), since, ms, [&](int, uint32_t, const eh::Header& h) {
        got.push_back(h.seq);
        n++;
        return n < page;
      });
      if (n == 0) break;
      since = got.back();
    }
    CHECKM(got.size() == want.size(), "paging visited %zu, model %zu (page %u)", got.size(), want.size(), page);
    for (size_t i = 0; i < want.size(); i++) CHECK(got[i] == want[i].r->seq);
  }

  void opMerge() {
    g.op = "mergeBySeq";
    S.merges++;
    if (rng.pct(5)) {
      opBadArgs();
      return;
    }
    if (rng.pct(10)) {
      opPage();
      return;
    }
    std::vector<int> srcIdx = pickSources(eh::kMaxSources);
    std::vector<eh::Source> src;
    for (int i : srcIdx) src.push_back({&rings[(size_t)i].st, rings[(size_t)i].buf, rings[(size_t)i].slot});
    uint32_t since = 0;
    uint32_t k = rng.below(5);
    if (k == 1) since = rng.range(0, seq);
    else if (k == 2) since = seq;
    else if (k == 3) since = rng.u32();
    else if (k == 4) {
      // the newest seq of a random source (an idle poll for that one)
      if (!srcIdx.empty()) since = rings[(size_t)srcIdx[rng.below((uint32_t)srcIdx.size())]].st.newestSeq;
    }
    std::vector<MItem> want = mergedModel(srcIdx, since);
    uint32_t limit = rng.pct(50) ? 0xFFFFFFFFu : rng.range(1, 40);
    static eh::MergeScratch ms;
    memset(&ms, rng.pct(50) ? 0 : 0xCD, sizeof ms);
    std::vector<MItem> got;
    std::vector<uint32_t> gotOff;
    std::vector<eh::Header> gotH;
    uint32_t calls = 0;
    uint32_t dc0 = eh::decodeCount();
    int ret = eh::mergeBySeq(src.empty() ? (rng.pct(50) ? nullptr : src.data()) : src.data(), (int)src.size(), since,
                             ms, [&](int si, uint32_t off, const eh::Header& h) {
                               calls++;
                               got.push_back({si, nullptr});
                               gotOff.push_back(off);
                               gotH.push_back(h);
                               return calls < limit;
                             });
    uint32_t decodes = eh::decodeCount() - dc0;
    size_t wantN = std::min<size_t>(want.size(), limit);
    CHECKM(got.size() == wantN, "merge visited %zu, model %zu (since %u, %zu sources)", got.size(), wantN, since,
           src.size());
    CHECKM(ret == (int)calls, "merge returned %d after %u visits", ret, calls);
    for (size_t i = 0; i < wantN; i++) {
      CHECKM(got[i].src == want[i].src, "merge item %zu from source %d, model %d", i, got[i].src, want[i].src);
      CHECKM(gotOff[i] == want[i].r->off, "merge item %zu offset", i);
      expectHeaderEq(gotH[i], *want[i].r, rings[(size_t)srcIdx[(size_t)want[i].src]].m.sent);
    }
    if (limit == 0xFFFFFFFFu)
      for (size_t i = 0; i < src.size(); i++) CHECKM(ms.cur[i].bad == 0, "merge marked healthy source %zu bad", i);
    // an idle poll decodes nothing
    if (want.empty()) CHECKM(decodes == 0, "idle merge (since %u) decoded %u headers", since, decodes);
  }

  // Model of collapseNewest (DESIGN 3.9 over today's espnowCollapseRing rules).
  std::vector<eh::Ref> collapseModel(const std::vector<int>& srcIdx, int maxN) {
    std::vector<MItem> all = mergedModel(srcIdx, 0);
    std::vector<eh::Ref> e;
    std::vector<long> prevSame;
    std::vector<int> srcOf;
    std::vector<long> srcLast(srcIdx.size(), -1);
    for (const MItem& it : all) {
      const MRec& r = *it.r;
      const TRing& t = rings[(size_t)srcIdx[(size_t)it.src]];
      long opened = (long)e.size();
      long w = std::min<long>(opened, maxN);
      long firstLive = opened - w;
      if (r.reqId != 0 && r.pieceTotal > 1) {
        long a = srcLast[(size_t)it.src];
        int steps = 0;
        long hit = -1;
        while (a >= 0 && a >= firstLive && steps < eh::kGroupWindow) {
          if (e[(size_t)a].reqId == r.reqId && e[(size_t)a].partsTotal > 1) {  // only a fragment-opened entry
            hit = a;
            break;
          }
          a = prevSame[(size_t)a];
          steps++;
        }
        if (hit >= 0) {
          eh::Ref& g2 = e[(size_t)hit];
          if (g2.partsPresent < 255) g2.partsPresent++;
          if (r.piece < g2.headPiece) {
            g2.seqNum = r.seq;
            g2.offset = r.off;
            g2.timestamp = r.ts;
            g2.headPiece = r.piece;
          }
          if (r.pieceTotal > g2.partsTotal) g2.partsTotal = r.pieceTotal;
          continue;
        }
      }
      eh::Ref nr{};
      nr.seqNum = r.seq;
      nr.reqId = r.reqId;
      nr.timestamp = r.ts;
      nr.offset = r.off;
      nr.slot = t.slot;
      nr.headPiece = r.piece;
      nr.partsPresent = 1;
      nr.partsTotal = r.pieceTotal > 1 ? r.pieceTotal : 1;
      nr.isSent = t.m.sent;
      e.push_back(nr);
      prevSame.push_back(srcLast[(size_t)it.src]);
      srcOf.push_back(it.src);
      srcLast[(size_t)it.src] = opened;
    }
    long w = std::min<long>((long)e.size(), maxN);
    return std::vector<eh::Ref>(e.end() - w, e.end());
  }

  void opCollapse() {
    g.op = "collapseNewest";
    S.collapses++;
    std::vector<int> srcIdx = pickSources(eh::kMaxSources);
    std::vector<eh::Source> src;
    for (int i : srcIdx) src.push_back({&rings[(size_t)i].st, rings[(size_t)i].buf, rings[(size_t)i].slot});
    int maxN;
    uint32_t k = rng.below(8);
    static const int kN[] = {1, 2, 3, 63, 64, 65, 512};
    if (k < 4) maxN = kN[rng.below(7)];
    else maxN = (int)rng.range(1, 600);
    int chainCap = maxN + (int)rng.below(4);
    std::vector<eh::Ref> want = collapseModel(srcIdx, maxN);
    eh::Ref* out = new eh::Ref[(size_t)maxN];
    uint32_t* ch = new uint32_t[(size_t)chainCap];
    memset(out, 0xAB, sizeof(eh::Ref) * (size_t)maxN);
    static eh::MergeScratch ms;
    int w = eh::collapseNewest(src.data(), (int)src.size(), out, maxN, ms, ch, chainCap);
    CHECKM(w == (int)want.size(), "collapse returned %d, model %zu (maxN %d, %zu sources)", w, want.size(), maxN,
           src.size());
    for (int i = 0; i < w; i++) {
      const eh::Ref& a = out[i];
      const eh::Ref& b = want[(size_t)i];
      CHECKM(a.seqNum == b.seqNum && a.reqId == b.reqId && a.timestamp == b.timestamp && a.offset == b.offset &&
                 a.slot == b.slot && a.headPiece == b.headPiece && a.partsPresent == b.partsPresent &&
                 a.partsTotal == b.partsTotal && a.isSent == b.isSent,
             "collapse row %d: got seq %u req %u off %u slot %u head %u parts %u/%u sent %d; model seq %u req %u "
             "off %u slot %u head %u parts %u/%u sent %d",
             i, a.seqNum, a.reqId, a.offset, a.slot, a.headPiece, a.partsPresent, a.partsTotal, a.isSent, b.seqNum,
             b.reqId, b.offset, b.slot, b.headPiece, b.partsPresent, b.partsTotal, b.isSent);
    }
    for (size_t i = 0; i < src.size(); i++) CHECKM(ms.cur[i].bad == 0, "collapse marked healthy source %zu bad", i);
    // sortByTime = a stable sort by timestamp
    std::vector<eh::Ref> sorted(out, out + w);
    std::vector<eh::Ref> ref = sorted;
    std::stable_sort(ref.begin(), ref.end(), [](const eh::Ref& a, const eh::Ref& b) { return a.timestamp < b.timestamp; });
    eh::sortByTime(sorted.data(), w);
    for (int i = 0; i < w; i++)
      CHECKM(sorted[(size_t)i].seqNum == ref[(size_t)i].seqNum && sorted[(size_t)i].isSent == ref[(size_t)i].isSent,
             "sortByTime row %d differs from a stable sort", i);
    // keep some refs for the stale-reference checks
    for (int i = 0; i < w; i++) {
      if (!rng.pct(20)) continue;
      int ring = -1;
      for (int si : srcIdx)
        if (rings[(size_t)si].slot == out[i].slot && rings[(size_t)si].m.sent == out[i].isSent) ring = si;
      if (ring < 0) continue;
      StaleRef s{ring, out[i].seqNum, out[i].offset};
      if (stale.size() < 256) stale.push_back(s);
      else stale[rng.below(256)] = s;
    }
    delete[] out;
    delete[] ch;
  }

  // ------------------------------------------------------------ corruption
  // A copy of ring t at exact size. Flip bytes / fields, then run everything.
  void opCorrupt(TRing& t) {
    g.op = "corrupt";
    S.corruptions++;
    if (t.m.q.empty()) return;
    const uint32_t size = t.st.size;
    uint8_t* cb = new uint8_t[size];
    memcpy(cb, t.buf, size);
    eh::RingState cs = t.st;
    uint32_t mode = rng.below(10);
    if (mode < 5) singleFlip(t, cs, cb);
    else blindCorrupt(t, cs, cb, mode);
    delete[] cb;
  }

  // Exactly one byte changed; the outcome is known.
  void singleFlip(TRing& t, eh::RingState& cs, uint8_t* cb) {
    S.singleFlips++;
    const MRing& m = t.m;
    // where: a header byte of record k, a text byte, the wrap marker, or anywhere
    uint32_t at;
    uint8_t delta = 0;
    size_t k = rng.below((uint32_t)m.q.size());
    if (rng.pct(35)) {
      // A consistent forgery of record k's header: the check byte is right, one rule is broken (wrong direction,
      // seq out of order, type above 7, a reserved flag bit, seq 0). The walk must stop at k all the same.
      S.forgeries++;
      const MRec& r = m.q[k];
      uint32_t fseq = r.seq;
      uint8_t type = r.type;
      uint8_t flags = specFlags(r.enc, m.sent, r.unattr, r.sendState);
      uint32_t kind = rng.below(5);
      if (kind == 0) flags ^= 0x02;
      else if (kind == 1) {
        if (k > 0) fseq = rng.range(1, m.q[k - 1].seq);
        else fseq = (r.seq > 1 && (rng.pct(50) || r.seq > 0xFFFFFF00u)) ? rng.range(1, r.seq - 1) : r.seq + rng.range(1, 5);
      } else if (kind == 2) type = (uint8_t)rng.range(8, 255);
      else if (kind == 3) flags = (uint8_t)(flags | (0x40u << rng.below(2)));
      else fseq = 0;
      specEncode(cb + r.off, fseq, r.reqId, r.ts, r.len, type, flags, r.piece, r.pieceTotal);
      at = r.off;
    } else {
      uint32_t w = rng.below(4);
      if (w == 0) at = m.q[k].off + rng.below(20);
      else if (w == 1 && m.q[k].len) at = m.q[k].off + 20 + rng.below(m.q[k].len);
      else if (w == 2 && m.wrapped && m.wrapAt < m.size) at = m.wrapAt;
      else at = rng.below(m.size);
      delta = (uint8_t)rng.range(1, 255);
      cb[at] ^= delta;
    }
    // what the flip hit
    size_t damagedHeader = SIZE_MAX;  // record whose header broke
    bool markerHit = m.wrapped && m.wrapAt < m.size && at == m.wrapAt;
    size_t textHit = SIZE_MAX;
    for (size_t i = 0; i < m.q.size(); i++) {
      uint32_t o = m.q[i].off;
      if (at >= o && at < o + 20) damagedHeader = i;
      else if (at >= o + 20 && at < o + 20 + m.q[i].len) textHit = i;
    }
    // records the walk must yield before it stops
    size_t prefix = m.q.size();
    if (damagedHeader != SIZE_MAX) prefix = damagedHeader;
    if (markerHit) prefix = m.upper;  // the upper segment, then the bad marker
    bool bad = prefix < m.q.size();
    auto textOf = [&](size_t i) {
      std::string s = m.q[i].text;
      if (i == textHit) s[at - m.q[i].off - 20] = (char)(s[at - m.q[i].off - 20] ^ (char)delta);
      return s;
    };
    // walk
    {
      eh::Cursor c = eh::begin(cs);
      uint32_t off;
      eh::Header h{};
      size_t i = 0;
      while (eh::next(cs, cb, c, off, h)) {
        CHECKM(i < prefix, "walk read past damage (flip at %u, record %zu of %zu yielded, prefix %zu)", at, i,
               m.q.size(), prefix);
        CHECK(off == m.q[i].off);
        expectHeaderEq(h, m.q[i], m.sent);
        char tb[256];
        size_t n = eh::copyText(cb, off, h, tb, sizeof tb);
        std::string want = textOf(i);
        CHECK(n == want.size() && memcmp(tb, want.data(), n) == 0);
        i++;
      }
      CHECKM(i == prefix, "walk yielded %zu, expected %zu (flip at %u)", i, prefix, at);
      CHECKM((c.bad != 0) == bad, "cursor bad %d, expected %d (flip at %u)", c.bad, bad, at);
    }
    // finds: right hint (fast path) and walk
    for (size_t i = 0; i < m.q.size(); i++) {
      if (m.q.size() > 64 && !rng.pct(10)) continue;
      const MRec& r = m.q[i];
      eh::Header h{};
      uint32_t off = 0;
      bool damaged = false;
      bool f = eh::findBySeq(cs, cb, r.seq, r.off, off, h, &damaged);
      bool wantF = (i != damagedHeader) || i < prefix;
      if (eh::kHintFastPath) {
        CHECKM(f == wantF, "findBySeq(right hint) of record %zu = %d, expected %d (flip at %u)", i, f, wantF, at);
      }
      if (f) {
        CHECK(off == r.off);
        expectHeaderEq(h, r, m.sent);
      } else {
        CHECKM(damaged, "findBySeq failed on a damaged ring without reporting damage (record %zu)", i);
      }
      damaged = false;
      f = eh::findBySeq(cs, cb, r.seq, eh::kNoEntry, off, h, &damaged);
      CHECKM(f == (i < prefix), "findBySeq(walk) of record %zu = %d, prefix %zu (flip at %u)", i, f, prefix, at);
      if (!f) CHECKM(damaged, "walk find stopped at damage without reporting it");
    }
    // merge over the copy and a healthy ring
    {
      TRing* other = &rings[(size_t)rng.below((uint32_t)rings.size())];
      if (other == &t) other = nullptr;
      eh::Source src[2] = {{&cs, cb, t.slot}, {other ? &other->st : nullptr, other ? other->buf : nullptr, 1}};
      int n = other ? 2 : 1;
      std::vector<std::pair<uint32_t, int>> want;
      for (size_t i = 0; i < prefix; i++) want.push_back({m.q[i].seq, 0});
      if (other)
        for (const MRec& r : other->m.q) want.push_back({r.seq, 1});
      std::sort(want.begin(), want.end());
      static eh::MergeScratch ms;
      std::vector<std::pair<uint32_t, int>> got;
      eh::mergeBySeq(src, n, 0, ms, [&](int si, uint32_t, const eh::Header& h) {
        got.push_back({h.seq, si});
        return true;
      });
      CHECKM(got == want, "merge over a damaged ring: %zu items, expected %zu (flip at %u)", got.size(), want.size(), at);
      CHECKM((ms.cur[0].bad != 0) == bad, "merge bad mark %d, expected %d", ms.cur[0].bad, bad);
      if (other) CHECK(ms.cur[1].bad == 0);
    }
    // reassemble on the copy
    {
      uint32_t reqId = m.q[rng.below((uint32_t)m.q.size())].reqId;
      if (reqId) {
        char out[2048];
        bool complete = false, damaged = false;
        int n = eh::reassemble(cs, cb, reqId, out, sizeof out, &complete, scratch, &damaged);
        for (int i = 0; i < 256; i++) CHECK(scratch[i] == 0);
        unsigned total = 0;
        int idx[256];
        for (int& v : idx) v = -1;
        bool anySingle = false;
        for (size_t i = 0; i < prefix; i++) {
          const MRec& r = m.q[i];
          if (r.reqId != reqId) continue;
          if (r.pieceTotal <= 1) {  // single-frame records are not fragments of the message
            anySingle = true;
            continue;
          }
          if (total == 0) total = r.pieceTotal;
          if (r.piece >= 1 && r.piece <= total && idx[r.piece] < 0) idx[r.piece] = (int)i;
        }
        if (total == 0 && anySingle) {  // no fragment: the oldest single record with piece 1
          total = 1;
          for (size_t i = 0; i < prefix; i++) {
            const MRec& r = m.q[i];
            if (r.reqId == reqId && r.pieceTotal <= 1 && r.piece == 1) {
              idx[1] = (int)i;
              break;
            }
          }
        }
        std::string want;
        for (unsigned p = 1; p <= total; p++)
          if (idx[p] >= 0) want += textOf((size_t)idx[p]);
        if (want.size() > sizeof out - 1) want.resize(sizeof out - 1);
        CHECKM(n == (int)want.size() && memcmp(out, want.data(), want.size()) == 0 && out[n] == 0,
               "reassemble over a damaged ring: %d bytes, expected %zu (flip at %u)", n, want.size(), at);
        CHECKM(damaged == bad, "reassemble damaged %d, expected %d", damaged, bad);
      }
    }
    // setSendState on the copy: only the matching headers before the damage change
    {
      uint32_t reqId = m.q[rng.below((uint32_t)m.q.size())].reqId;
      uint8_t state = (uint8_t)rng.below(8);
      std::vector<uint8_t> snap(cb, cb + cs.size);
      bool damaged = false;
      int n = eh::setSendState(cs, cb, reqId, state, &damaged);
      int want = 0;
      std::vector<uint8_t> allowed(cs.size, 0);
      if (reqId)
        for (size_t i = 0; i < prefix; i++)
          if (m.q[i].reqId == reqId) {
            want++;
            allowed[m.q[i].off + 3] = allowed[m.q[i].off + 6] = 1;
          }
      CHECKM(n == want, "setSendState over a damaged ring = %d, expected %d (flip at %u)", n, want, at);
      if (reqId) CHECKM(damaged == bad, "setSendState damaged %d, expected %d", damaged, bad);
      for (uint32_t i = 0; i < cs.size; i++)
        CHECKM(snap[i] == cb[i] || allowed[i], "setSendState over a damaged ring changed byte %u", i);
    }
    // append on the copy: evicting into the damage resets the ring; never a write outside
    if (seq < 0xFFFFFFFFu) {
      MRing mm = m;
      uint32_t ev = 0;
      uint32_t len = rng.below(256);
      mm.place(fp(len), ev);
      eh::Header h{};
      h.seq = seq + 1;
      h.type = 1;
      std::string txt(len, 'z');
      eh::AppendResult ar = eh::append(cs, cb, h, txt.data(), len);
      CHECKM(ar.ok && !ar.insane, "append on a damaged ring refused");
      CHECK(ar.offset % 4 == 0 && ar.offset + fp(len) <= cs.size);
      bool wantReset = damagedHeader != SIZE_MAX && ev > 0 && damagedHeader <= ev;
      if (wantReset) S.resetsExpected++;
      if (wantReset && damagedHeader == ev) S.resetsAtNext++;
      CHECKM(ar.reset == wantReset, "append reset %d, expected %d (damaged record %zu, model evicts %u, flip at %u)",
             ar.reset, wantReset, damagedHeader == SIZE_MAX ? (size_t)0 : damagedHeader, ev, at);
      if (ar.reset) {
        CHECK(cs.count == 1 && cs.resets == t.st.resets + 1 && cs.newestSeq == h.seq && cs.oldestSeq == h.seq);
        eh::CheckResult cr = eh::checkRing(cs, cb);
        CHECKM(cr.ok, "after the reset: %s at %u", cr.what ? cr.what : "?", cr.at);
      }
      eh::Header hh{};
      uint32_t off = 0;
      CHECK(eh::findBySeq(cs, cb, h.seq, ar.offset, off, hh) && off == ar.offset);
    }
  }

  // Many bytes, RingState fields, or everything: only safety and bounds.
  void blindCorrupt(TRing& t, eh::RingState& cs, uint8_t* cb, uint32_t mode) {
    const uint32_t size = cs.size;
    if (mode < 7) {
      uint32_t n = rng.range(2, 64);
      for (uint32_t i = 0; i < n; i++) {
        uint32_t at = rng.pct(60) && !t.m.q.empty() ? t.m.q[rng.below((uint32_t)t.m.q.size())].off + rng.below(24)
                                                    : rng.below(size);
        if (at < size) cb[at] ^= (uint8_t)rng.range(1, 255);
      }
      if (rng.pct(30)) {
        // forge a header that passes its check byte (a sound check cannot catch this)
        const MRec& r = t.m.q[rng.below((uint32_t)t.m.q.size())];
        specEncode(cb + r.off, rng.pct(50) ? r.seq : rng.u32(), r.reqId, r.ts, (uint8_t)rng.u32(),
                   (uint8_t)rng.below(8), (uint8_t)(rng.u32() & 0x1F), r.piece, r.pieceTotal);
      }
    }
    if (mode >= 7) {
      // RingState fields (size kept: the store checks it against the block, 3.10)
      uint32_t* f[] = {&cs.tail, &cs.head, &cs.wrapAt, &cs.oldestSeq, &cs.newestSeq, &cs.used};
      uint32_t nf = rng.range(1, 3);
      for (uint32_t i = 0; i < nf; i++) {
        uint32_t which = rng.below(9);
        uint32_t v;
        uint32_t vk = rng.below(4);
        if (vk == 0) v = rng.u32();
        else if (vk == 1) v = 4 * rng.below(size / 4 + 2);
        else if (vk == 2) v = rng.below(size + 8);
        else v = rng.pct(50) ? size : 0;
        if (which < 6) *f[which] = (which == 3 || which == 4) && rng.pct(50) ? (*f[which] + rng.range(1, 3)) : v;
        else if (which == 6) cs.count = (uint16_t)(rng.pct(50) ? cs.count + rng.range(1, 2) - 0 : rng.u32());
        else if (which == 7) cs.wrapped = (uint8_t)rng.below(3);
        else cs.sent = (uint8_t)rng.below(3);
      }
      if (mode == 9) {
        // a fully made-up but sane-looking state over random bytes with fake headers
        for (uint32_t o = 0; o + 20 <= size; o += 4) {
          if (rng.pct(30))
            specEncode(cb + o, rng.range(1, 2000), reqPool[rng.below((uint32_t)reqPool.size())], rng.u32(),
                       (uint8_t)rng.u32(), (uint8_t)rng.below(8), (uint8_t)(rng.u32() & 0x1F), (uint8_t)rng.below(4),
                       (uint8_t)rng.below(5));
        }
        cs.tail = 4 * rng.below(size / 4);
        cs.head = 4 * rng.below(size / 4 + 1);
        cs.wrapped = cs.head <= cs.tail ? 1 : 0;
        cs.wrapAt = cs.wrapped ? cs.tail + 4 * rng.below((size - cs.tail) / 4 + 1) : size;
        if (cs.wrapped && cs.wrapAt <= cs.tail) cs.wrapAt = size;
        cs.used = cs.wrapped ? (cs.wrapAt - cs.tail) + cs.head : cs.head - cs.tail;
        cs.count = (uint16_t)std::max<uint32_t>(1, cs.used / rng.range(20, 276));
        cs.oldestSeq = rng.range(1, 1000);
        cs.newestSeq = cs.count == 1 ? cs.oldestSeq : cs.oldestSeq + rng.range(1, 1000);
      }
    }
    exerciseBlind(t, cs, cb);
  }

  void exerciseBlind(TRing& t, eh::RingState& cs, uint8_t* cb) {
    const uint32_t size = t.st.size;
    bool sane = eh::stateSane(cs);
    // walk
    {
      eh::Cursor c = eh::begin(cs);
      if (!sane) CHECK(c.bad != 0);
      uint32_t off = 0, last = 0;
      eh::Header h{};
      int n = 0;
      while (eh::next(cs, cb, c, off, h)) {
        CHECKM(off % 4 == 0 && off + fp(h.length) <= size, "walk yielded a record at %u len %u", off, h.length);
        CHECK(h.seq > last);
        CHECK(h.sendState <= 7 && h.type <= 7);
        last = h.seq;
        char tb[300];
        size_t cap = rng.range(1, sizeof tb);
        size_t k = eh::copyText(cb, off, h, tb, cap);
        CHECK(k < cap && tb[k] == 0);
        CHECKM(++n <= (int)cs.count, "walk yielded more than count %u", cs.count);
      }
    }
    // checkRing must not crash; on a broken state it must say so
    eh::CheckResult cr = eh::checkRing(cs, cb);
    if (!sane) CHECK(!cr.ok);
    // finds with any hint
    for (int i = 0; i < 8; i++) {
      eh::Header h{};
      uint32_t off = 0;
      bool damaged = false;
      uint32_t sq = rng.pct(50) && !t.m.q.empty() ? t.m.q[rng.below((uint32_t)t.m.q.size())].seq : rng.range(1, 3000);
      uint32_t hint = rng.pct(70) ? 4 * rng.below(size / 4 + 4) : rng.u32();
      bool f = eh::findBySeq(cs, cb, sq, hint, off, h, &damaged);
      if (f) {
        CHECKM(off % 4 == 0 && off + fp(h.length) <= size, "find returned a record at %u len %u", off, h.length);
        CHECK(h.seq == sq);
      }
      if (!sane) CHECK(!f);
    }
    // reassemble
    {
      uint32_t reqId = reqPool[rng.below((uint32_t)reqPool.size())];
      size_t cap = rng.range(1, 3000);
      char* out = new char[cap];
      bool complete = false, damaged = false;
      int n = eh::reassemble(cs, cb, reqId, out, cap, &complete, scratch, &damaged);
      for (int i = 0; i < 256; i++) CHECK(scratch[i] == 0);
      CHECK(n >= 0 && (size_t)n < cap && out[n] == 0);
      if (!sane) CHECK(n == 0);
      delete[] out;
    }
    // merge and collapse with the copy among healthy sources
    {
      eh::Source src[3];
      int n = 0;
      src[n++] = {&cs, cb, t.slot};
      TRing& o = rings[(size_t)rng.below((uint32_t)rings.size())];
      if (&o != &t) src[n++] = {&o.st, o.buf, o.slot};
      static eh::MergeScratch ms;
      int visits = 0;
      eh::mergeBySeq(src, n, 0, ms, [&](int si, uint32_t off, const eh::Header& h) {
        CHECK(si >= 0 && si < n);
        if (si == 0) CHECK(off % 4 == 0 && off + fp(h.length) <= size);
        visits++;
        return visits < 100000;
      });
      int maxN = (int)rng.range(1, 100);
      std::vector<eh::Ref> out((size_t)maxN);
      std::vector<uint32_t> ch((size_t)maxN);
      int w = eh::collapseNewest(src, n, out.data(), maxN, ms, ch.data(), maxN);
      CHECK(w >= 0 && w <= maxN);
      for (int i = 0; i < w; i++)
        if (out[(size_t)i].slot == t.slot && out[(size_t)i].isSent == (cs.sent == 1)) {
          eh::Header h{};
          uint32_t off = 0;
          (void)eh::findBySeq(cs, cb, out[(size_t)i].seqNum, out[(size_t)i].offset, off, h);
        }
    }
    // setSendState: on a broken state nothing is written
    {
      std::vector<uint8_t> snap(cb, cb + size);
      eh::RingState ss = cs;
      bool damaged = false;
      int n = eh::setSendState(cs, cb, reqPool[rng.below((uint32_t)reqPool.size())], (uint8_t)rng.below(8), &damaged);
      CHECK(n >= 0);
      CHECKM(memcmp(&ss, &cs, sizeof ss) == 0, "setSendState changed RingState");
      if (!sane) CHECK(n == 0 && memcmp(snap.data(), cb, size) == 0);
    }
    // append: insane → untouched; otherwise it stores (possibly after a reset)
    {
      std::vector<uint8_t> snap(cb, cb + size);
      eh::RingState ss = cs;
      bool saneNow = eh::stateSane(cs);
      eh::Header h{};
      uint64_t ns = (uint64_t)std::max<uint32_t>(cs.newestSeq, seq) + 1;
      if (ns > 0xFFFFFFFFull) return;  // no larger seq exists; nothing to store
      h.seq = (uint32_t)ns;
      uint32_t len = rng.below(300);
      std::string txt(len, 'q');
      eh::AppendResult ar = eh::append(cs, cb, h, txt.data(), len);
      if (!saneNow) {
        CHECK(!ar.ok && ar.insane);
        CHECK(memcmp(&ss, &cs, sizeof ss) == 0 && memcmp(snap.data(), cb, size) == 0);
        // the store resets an insane ring (3.10); then it works again
        eh::RingState fresh{};
        CHECK(eh::ringInit(fresh, size, cs.sent == 1));
        eh::AppendResult a2 = eh::append(fresh, cb, h, txt.data(), len);
        CHECK(a2.ok && fresh.count == 1);
        CHECK(eh::checkRing(fresh, cb).ok);
      } else {
        CHECKM(ar.ok, "append on a sane-looking damaged ring refused");
        CHECK(ar.offset % 4 == 0 && ar.offset + fp(std::min<uint32_t>(len, 255)) <= size);
        eh::Header hh{};
        uint32_t off = 0;
        CHECKM(eh::findBySeq(cs, cb, h.seq, ar.offset, off, hh) && off == ar.offset,
               "the record just stored on a damaged ring cannot be found");
        CHECK(eh::stateSane(cs));
      }
    }
    // ringReset always brings a ring with a sane size back
    if (cs.size == size) {
      eh::RingState r = cs;
      r.sent = r.sent > 1 ? 1 : r.sent;
      uint32_t ev = r.evicted, rs = r.resets;
      eh::ringReset(r);
      CHECK(r.size == size && r.count == 0 && r.used == 0 && r.tail == 0 && r.head == 0 && r.wrapped == 0 &&
            r.wrapAt == size && r.oldestSeq == 0 && r.newestSeq == 0 && r.evicted == ev && r.resets == rs + 1);
      CHECK(eh::stateSane(r) && eh::checkRing(r, cb).ok);
    }
  }

  // ------------------------------------------------------------- run
  void run(uint64_t ops) {
    setup();
    S.episodes++;
    for (uint64_t i = 0; i < ops; i++) {
      g.step = i;
      S.steps++;
      TRing& t = rings[rng.below((uint32_t)rings.size())];
      uint32_t r = rng.below(1000);
      bool mutated = false;
      if (r < 520) {
        opAppend(t);
        mutated = true;
      } else if (r < 640) {
        opFind(t);
      } else if (r < 700) {
        opStamp(t);
        mutated = true;
      } else if (r < 760) {
        opReassemble(t);
      } else if (r < 810) {
        opMerge();
      } else if (r < 850) {
        opCollapse();
      } else if (r < 870) {
        opStale();
      } else if (r < 900) {
        opCorrupt(t);
      } else {
        // a burst of appends to one ring: drives wraps and long eviction runs
        uint32_t n = rng.range(1, 40);
        for (uint32_t k = 0; k < n; k++) opAppend(t);
        mutated = true;
      }
      g.op = "verify";
      bool deep = t.m.q.size() <= 160 || (i % 16) == 0;
      if (mutated) verifyRing(t, deep, rng);
      if ((i % 512) == 0)
        for (TRing& x : rings) verifyRing(x, true, rng);
    }
    g.op = "final";
    for (TRing& x : rings) verifyRing(x, true, rng);
    opStale();
  }
};

// ---------------------------------------------------------------------------
// Small adversarial checks of the name helpers and the budget, and ring init
// ---------------------------------------------------------------------------
static void fuzzHelpers(Rng& rng, uint64_t n) {
  g.op = "helpers";
  for (uint64_t i = 0; i < n; i++) {
    g.step = i;
    S.fuzz++;
    // boundedCopy: src exactly srcCap bytes, maybe with no NUL
    size_t srcCap = rng.below(40);
    char* src = srcCap ? new char[srcCap] : nullptr;
    for (size_t k = 0; k < srcCap; k++) src[k] = (char)rng.range(rng.pct(10) ? 0 : 1, 127);
    size_t cap = rng.below(40);
    char* out = cap ? new char[cap] : nullptr;
    if (out) memset(out, 0x55, cap);
    const char* sp = (srcCap || rng.pct(50)) ? src : nullptr;
    size_t got = eh::boundedCopy(out, cap, sp, srcCap);
    if (!out || cap == 0) {
      CHECK(got == 0);
    } else {
      size_t want = 0;
      size_t lim = std::min(cap - 1, srcCap);
      if (sp)
        while (want < lim && sp[want]) want++;
      CHECKM(got == want, "boundedCopy %zu, want %zu", got, want);
      CHECK(memcmp(out, sp ? sp : "", want) == 0 && out[want] == 0);
    }
    // displayName
    const char* mac = "AA:BB:CC:DD:EE:FF";
    bool sent = rng.pct(20), un = rng.pct(20);
    size_t nameCap = rng.below(33);
    char* name = nameCap ? new char[nameCap] : nullptr;
    for (size_t k = 0; k < nameCap; k++) name[k] = (char)rng.range(rng.pct(15) ? 0 : 33, 126);
    const char* np = rng.pct(10) ? nullptr : name;
    if (!name) np = nullptr;
    char* out2 = cap ? new char[cap] : nullptr;
    eh::displayName(sent, un, np, nameCap, mac, out2, cap);
    if (out2) {
      std::string want;
      if (sent) want = "";
      else if (un) want = mac;
      else {
        std::string cur;
        if (np)
          for (size_t k = 0; k < nameCap && np[k]; k++) cur += np[k];
        want = cur.empty() ? mac : cur;
      }
      if (want.size() > cap - 1) want.resize(cap - 1);
      CHECKM(strlen(out2) == want.size() && memcmp(out2, want.data(), want.size()) == 0,
             "displayName gave '%s', want '%s'", out2, want.c_str());
    }
    delete[] src;
    delete[] out;
    delete[] name;
    delete[] out2;
    // ringBudget: properties of the result
    uint32_t kb = rng.below(300), def = rng.range(1, 32), mx = rng.range(1, 64);
    uint32_t total = rng.pct(20) ? 0 : rng.range(0, 4u << 20);
    int capacity = (int)rng.below(20);
    eh::Budget b = eh::ringBudget(kb, def, mx, total, capacity);
    CHECKM(b.ringBytes % 4 == 0 && b.ringBytes >= eh::kMinRingBytes && b.ringBytes <= eh::kMaxRingBytes,
           "ringBudget(%u,%u,%u,%u,%d) = %u", kb, def, mx, total, capacity, b.ringBytes);
    uint32_t k2 = kb ? kb : def;
    k2 = std::max<uint32_t>(1, std::min(k2, mx));
    uint32_t want = k2 * 1024;
    if (capacity > 0) want = std::min<uint32_t>(want, (total / (2u * (uint32_t)capacity)) & ~3u);
    want = std::max(eh::kMinRingBytes, std::min(eh::kMaxRingBytes, want));
    CHECKM(b.ringBytes == want, "ringBudget(%u,%u,%u,%u,%d) = %u, formula %u", kb, def, mx, total, capacity,
           b.ringBytes, want);
    // ringInit on any size
    eh::RingState r;
    memset(&r, 0xEE, sizeof r);
    uint32_t sz = rng.pct(50) ? rng.u32() % 70000 : 4 * rng.range(250, 16400);
    bool okSize = sz % 4 == 0 && sz >= eh::kMinRingBytes && sz <= eh::kMaxRingBytes;
    bool ok = eh::ringInit(r, sz, rng.pct(50));
    CHECKM(ok == okSize, "ringInit(%u) = %d", sz, ok);
    if (!ok) {
      CHECK(r.size == 0 && !eh::stateSane(r));
      eh::Cursor c = eh::begin(r);
      CHECK(c.bad != 0);
    } else {
      CHECK(eh::stateSane(r) && r.count == 0 && r.wrapAt == sz && r.evicted == 0 && r.resets == 0);
    }
  }
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  uint64_t ops = 1000000;
  uint64_t seed = 0x5EED0001ull;
  uint64_t onlyEpisode = 0;
  bool haveOnly = false;
  int maxFail = 10;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto val = [&]() -> uint64_t { return i + 1 < argc ? strtoull(argv[++i], nullptr, 0) : 0; };
    if (a == "--ops") ops = val();
    else if (a == "--seed") seed = val();
    else if (a == "--long") ops = 20000000;
    else if (a == "--episode-seed") {
      onlyEpisode = val();
      haveOnly = true;
    } else if (a == "--max-fail") maxFail = (int)val();
    else if (a == "--verbose") g.verbose = true;
    else {
      fprintf(stderr, "usage: %s [--ops N] [--seed S] [--long] [--episode-seed X] [--max-fail N] [--verbose]\n",
              argv[0]);
      return 2;
    }
  }
  auto t0 = std::chrono::steady_clock::now();
  std::map<int, std::pair<int, std::string>> fails;  // line -> (count, first message)
  int failures = 0;
  {
    Rng hr(mix64(seed, 0xF00D));
    try {
      fuzzHelpers(hr, haveOnly ? 0 : 200000);
    } catch (const Failure& f) {
      failures++;
      fails[f.line] = {1, "helpers (seed " + std::to_string(seed) + "): " + f.what};
    }
  }
  uint64_t done = 0;
  for (uint64_t e = 0; done < ops || haveOnly; e++) {
    uint64_t es = haveOnly ? onlyEpisode : mix64(seed, e + 1);
    Rng er(es);
    uint64_t epOps = er.pct(80) ? er.range(500, 6000) : er.range(6000, 40000);
    if (!haveOnly && epOps > ops - done) epOps = ops - done;
    g.episodeSeed = es;
    try {
      Episode ep(mix64(es, 7));
      ep.run(epOps);
    } catch (const Failure& f) {
      failures++;
      auto& slot = fails[f.line];
      slot.first++;
      if (slot.first == 1) {
        char b[96];
        snprintf(b, sizeof b, "--episode-seed 0x%016" PRIx64 ": ", es);
        slot.second = b + f.what;
        fprintf(stderr, "FAIL line %d: %s\n", f.line, slot.second.c_str());
      }
      if (failures >= maxFail) break;
    }
    done += epOps;
    if (haveOnly) break;
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("history stress: seed 0x%" PRIx64 ", %" PRIu64 " episodes, %" PRIu64 " steps in %.1f s\n", seed, S.episodes,
         S.steps, secs);
  printf("  appends %" PRIu64 " (refusals %" PRIu64 ", evictions %" PRIu64 ", wraps %" PRIu64 ", truncated %" PRIu64
         ")\n",
         S.appends, S.refusals, S.evictions, S.wraps, S.truncs);
  printf("  finds %" PRIu64 ", stale-ref resolves %" PRIu64 ", stamps %" PRIu64 ", reassembles %" PRIu64
         ", merges %" PRIu64 ", collapses %" PRIu64 "\n",
         S.finds, S.staleChecks, S.stamps, S.reassembles, S.merges, S.collapses);
  printf("  corruptions %" PRIu64 " (single flips %" PRIu64 ", of them forgeries %" PRIu64
         "; evicting into damage %" PRIu64 ", at the next header %" PRIu64 "), free-space plants %" PRIu64
         ", helper cases %" PRIu64 "\n",
         S.corruptions, S.singleFlips, S.forgeries, S.resetsExpected, S.resetsAtNext, S.freePlants, S.fuzz);
  if (failures) {
    printf("FAILED: %d failing episodes, %zu distinct checks\n", failures, fails.size());
    for (auto& kv : fails) printf("  line %d (x%d): %s\n", kv.first, kv.second.first, kv.second.second.c_str());
    return 1;
  }
  printf("PASS\n");
  return 0;
}
