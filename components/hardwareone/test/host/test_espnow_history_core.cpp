// ESP-NOW message history byte ring (System_ESPNow_HistoryCore.h, compiled as is).
//
// One peer's history is two rings of packed records (20-byte header + text,
// padded to 4). These cases check, against an independent deque model of the
// placement rule and against ports of today's ring-of-structs algorithms:
//   1. the header round trip, and that any single changed header byte, a
//      reserved flag bit, a nonzero byte 7, type 8 and seq 0 are refused;
//   2. footprint, ringBudget, unattributedName, boundedCopy, displayName,
//      copyText (cap 0/1/2, null out, unterminated 32-byte names);
//   3. placement: exact fit at the end (wrap with no marker), a 4-byte gap
//      (marker), eviction across the wrap, the return to the unwrapped state,
//      empty, 255-byte and cut 300-byte texts, and the model on many sizes;
//   4. refusals leave the ring byte-identical;
//   5. the validated walk stops at the first changed byte of any record and
//      at a damaged wrap marker; buffers are exactly sized (ASan);
//   6. hostile text full of valid fake headers changes nothing; the one
//      residual case of DESIGN 6.3 (a hint the API never produced) is pinned;
//   7. findBySeq with every hint, hints at segLimit-4..16 of both segments
//      with the sent ring right after the received one, dead and future seqs;
//   8. stale references: after every append, every old ref resolves iff the
//      model says its record is live, even with a fake header carrying its
//      seq at its old offset;
//   9. setSendState changes only the flags and check bytes of its records;
//  10. reassemble, including a table test against today's algorithm;
//  11. collapseNewest against today's collapse + sort, the newest-maxN window,
//      the same-source look-back boundary and every argument refusal;
//  12. every corrupted RingState field is refused by every operation;
//  13. damage met while evicting clears the ring and the new record is kept;
//  14. decode budgets at the maximum configuration.
// The randomized stress test is a separate suite.
#include "System_ESPNow_HistoryCore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
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

using namespace espnowhist;

// ---------------------------------------------------------------------------------------------
// helpers

static uint32_t gSeq = 0;
static uint32_t nextSeq() { return ++gSeq; }
static uint32_t tsOf(uint32_t seq) { return seq * 2654435761u; }  // a bijection: unique, not monotonic

static uint32_t gRng = 12345;
static uint32_t rnd() {
  gRng ^= gRng << 13;
  gRng ^= gRng >> 17;
  gRng ^= gRng << 5;
  return gRng;
}
static uint32_t rndBelow(uint32_t n) { return n ? rnd() % n : 0; }

static Header mk(uint32_t seq, uint32_t reqId = 0, uint8_t piece = 1, uint8_t total = 1, uint8_t type = 0,
                 uint8_t sendState = 0, bool enc = false, bool unattr = false) {
  Header h{};
  h.seq = seq;
  h.reqId = reqId;
  h.timestamp = tsOf(seq);
  h.type = type;
  h.piece = piece;
  h.pieceTotal = total;
  h.sendState = sendState;
  h.encrypted = enc;
  h.unattributed = unattr;
  return h;
}

static std::string textOf(uint32_t len, uint32_t salt) {
  std::string s(len, 'a');
  for (uint32_t i = 0; i < len; ++i) s[i] = static_cast<char>('a' + (i * 7 + salt) % 26);
  return s;
}

static uint32_t lenForFootprint(uint32_t f) { return f - kHeaderBytes; }  // f multiple of 4, >= 20

// A ring with storage. Guarded: 64 canary bytes on both sides. Exact: an allocation of exactly `size` bytes, so
// ASan reports any read or write outside it.
struct Ring {
  static constexpr uint32_t kGuard = 64;
  RingState r{};
  uint8_t* mem = nullptr;
  uint8_t* buf = nullptr;
  uint32_t size = 0;
  bool guarded = true;
  Ring(uint32_t s, bool sent = false, bool guard = true) : size(s), guarded(guard) {
    if (guard) {
      mem = new uint8_t[s + 2 * kGuard];
      std::memset(mem, 0xCC, s + 2 * kGuard);
      buf = mem + kGuard;
    } else {
      mem = new uint8_t[s];
      buf = mem;
    }
    std::memset(buf, 0, s);
    CHECK(ringInit(r, s, sent));
  }
  ~Ring() { delete[] mem; }
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;
  bool guardsOk() const {
    if (!guarded) return true;
    for (uint32_t i = 0; i < kGuard; ++i)
      if (mem[i] != 0xCC || mem[kGuard + size + i] != 0xCC) return false;
    return true;
  }
};

static AppendResult put(RingState& r, uint8_t* buf, const Header& h, const std::string& text) {
  AppendResult a = append(r, buf, h, text.data(), text.size());
  CHECK(a.ok);
  CHECK(checkRing(r, buf).ok);
  return a;
}
static AppendResult put(Ring& g, const Header& h, const std::string& text) {
  AppendResult a = put(g.r, g.buf, h, text);
  CHECK(g.guardsOk());
  return a;
}

struct Rec {
  uint32_t off;
  Header h;
};
static std::vector<Rec> walk(const RingState& r, const uint8_t* buf, bool* bad = nullptr) {
  std::vector<Rec> v;
  Cursor c = begin(r);
  uint32_t off = 0;
  Header h;
  while (next(r, buf, c, off, h)) v.push_back({off, h});
  if (bad) *bad = c.bad != 0;
  return v;
}

static std::string textAt(const uint8_t* buf, const Rec& rec) {
  char t[kTextMax + 1];
  size_t n = copyText(buf, rec.off, rec.h, t, sizeof t);
  return std::string(t, n);
}

static bool sameHeader(const Header& a, const Header& b) {
  return a.seq == b.seq && a.reqId == b.reqId && a.timestamp == b.timestamp && a.length == b.length &&
         a.type == b.type && a.piece == b.piece && a.pieceTotal == b.pieceTotal && a.sendState == b.sendState &&
         a.encrypted == b.encrypted && a.sent == b.sent && a.unattributed == b.unattributed;
}

static bool scratchZero(const uint16_t* s) {
  for (int i = 0; i < 256; ++i)
    if (s[i]) return false;
  return true;
}

// Independent model of the placement rule (DESIGN 3.3), on a deque of records.
struct MRec {
  uint32_t seq, off, fp;
  Header h;
  std::string text;
};
struct ModelRing {
  uint32_t size = 0, head = 0, wrapAt = 0;
  bool wrapped = false;
  std::deque<MRec> q;
  void init(uint32_t s) {
    size = s;
    head = 0;
    wrapAt = s;
    wrapped = false;
    q.clear();
  }
  uint32_t place(uint32_t f, int& evicted) {
    evicted = 0;
    for (;;) {
      if (q.empty()) {
        wrapped = false;
        wrapAt = size;
        head = 0;
        return 0;
      }
      const uint32_t tail = q.front().off;
      if (!wrapped) {
        if (size - head >= f) return head;
        if (tail >= f) {
          wrapAt = head;
          wrapped = true;
          return 0;
        }
      } else if (tail - head >= f) {
        return head;
      }
      const MRec gone = q.front();
      q.pop_front();
      ++evicted;
      if (wrapped && gone.off + gone.fp == wrapAt) {
        wrapped = false;
        wrapAt = size;
      }
    }
  }
  void add(const Header& h, const std::string& text, int& evicted, uint32_t& pos) {
    const uint32_t len = text.size() > kTextMax ? kTextMax : static_cast<uint32_t>(text.size());
    const uint32_t f = footprint(len);
    pos = place(f, evicted);
    MRec m{h.seq, pos, f, h, text.substr(0, len)};
    m.h.length = static_cast<uint8_t>(len);
    q.push_back(m);
    head = pos + f;
  }
  uint32_t used() const {
    uint32_t u = 0;
    for (const MRec& m : q) u += m.fp;
    return u;
  }
  bool live(uint32_t seq) const {
    for (const MRec& m : q)
      if (m.seq == seq) return true;
    return false;
  }
};

// The core and the model agree on the whole state and every record.
static void checkAgainstModel(const RingState& r, const uint8_t* buf, const ModelRing& m) {
  CHECK(checkRing(r, buf).ok);
  CHECK(r.count == m.q.size());
  CHECK(r.used == m.used());
  CHECK(r.wrapped == (m.wrapped ? 1 : 0));
  if (r.count) {
    CHECK(r.head == m.head);
    CHECK(r.wrapAt == m.wrapAt);
    CHECK(r.tail == m.q.front().off);
    CHECK(r.oldestSeq == m.q.front().seq);
    CHECK(r.newestSeq == m.q.back().seq);
  }
  bool bad = true;
  std::vector<Rec> v = walk(r, buf, &bad);
  CHECK(!bad);
  CHECK(v.size() == m.q.size());
  for (size_t i = 0; i < v.size(); ++i) {
    CHECK(v[i].off == m.q[i].off);
    CHECK(v[i].h.seq == m.q[i].seq);
    CHECK(textAt(buf, v[i]) == m.q[i].text);
  }
}

// Fills g with records of varying length until it is wrapped with a marker gap and holds at least minCount.
static void fillWrapped(Ring& g, uint32_t minCount, uint32_t reqId = 0) {
  for (int i = 0; !(g.r.wrapped && g.r.wrapAt < g.r.size && g.r.count >= minCount); ++i) {
    CHECK(i < 1000);
    put(g, mk(nextSeq(), reqId), textOf(60 + 17 * static_cast<uint32_t>(i % 5), static_cast<uint32_t>(i)));
  }
}

// ---------------------------------------------------------------------------------------------
// 1. header

static void testHeader() {
  uint8_t p[kHeaderBytes];
  const uint32_t seqs[] = {1, 2, 0x7FFFFFFF, 0xFFFFFFFE, 0xFFFFFFFF};
  const uint32_t ids[] = {0, 1, 0xFFFFFFFF};
  for (uint32_t seq : seqs)
    for (uint32_t id : ids)
      for (uint8_t type = 0; type <= kTypeMax; ++type)
        for (uint8_t st = 0; st <= kSendStateMax; ++st)
          for (int flags = 0; flags < 8; ++flags) {
            Header h{};
            h.seq = seq;
            h.reqId = id;
            h.timestamp = ~id;
            h.length = static_cast<uint8_t>(type * 36);
            h.type = type;
            h.piece = static_cast<uint8_t>(flags * 36);
            h.pieceTotal = static_cast<uint8_t>(255 - flags);
            h.sendState = st;
            h.encrypted = flags & 1;
            h.sent = flags & 2;
            h.unattributed = flags & 4;
            encodeHeader(h, p);
            Header d{};
            CHECK(decodeHeader(p, d));
            CHECK(sameHeader(h, d));
            CHECK((p[kOffFlags] & kFlagsReserved) == 0);
          }

  // Extremes of the byte fields.
  Header h = mk(0xFFFFFFFF, 0xFFFFFFFF, 255, 255, kTypeMax, 3, true, true);
  h.timestamp = 0xFFFFFFFF;
  h.length = 255;
  h.sent = true;
  encodeHeader(h, p);
  Header d{};
  CHECK(decodeHeader(p, d));
  CHECK(sameHeader(h, d));
  h = mk(1, 0, 0, 0, 0);
  h.timestamp = 0;
  h.length = 0;
  encodeHeader(h, p);
  CHECK(decodeHeader(p, d));
  CHECK(sameHeader(h, d));

  // The check byte does not include itself.
  encodeHeader(mk(77, 5, 2, 3, 1), p);
  const uint8_t ck = headerCheck(p);
  p[kOffCheck] ^= 0x33;
  CHECK(headerCheck(p) == ck);

  // Every single-byte change of a valid header, each byte and each of the 255 deltas, is refused.
  const Header samples[] = {mk(77, 5, 2, 3, 1, 2, true), mk(1), mk(0xFFFFFFFF, 0xFFFFFFFF, 255, 255, 7, 3, true, true)};
  for (const Header& s : samples) {
    uint8_t good[kHeaderBytes];
    encodeHeader(s, good);
    for (uint32_t i = 0; i < kHeaderBytes; ++i) {
      for (int delta = 1; delta < 256; ++delta) {
        uint8_t q[kHeaderBytes];
        std::memcpy(q, good, sizeof q);
        q[i] = static_cast<uint8_t>(q[i] + delta);
        Header x{};
        CHECK(!decodeHeader(q, x));
      }
    }
  }

  // Each structural rule on its own, with the check recomputed so only that rule can refuse.
  auto refusedWith = [&](void (*mutate)(uint8_t*)) {
    uint8_t q[kHeaderBytes];
    encodeHeader(mk(9, 1, 1, 1, 3), q);
    mutate(q);
    q[kOffCheck] = headerCheck(q);
    Header x{};
    return !decodeHeader(q, x);
  };
  CHECK(refusedWith([](uint8_t* q) { q[kOffFlags] |= 0x40; }));
  CHECK(refusedWith([](uint8_t* q) { q[kOffFlags] |= 0x80; }));
  CHECK(refusedWith([](uint8_t* q) { q[kOffZero] = 1; }));
  CHECK(refusedWith([](uint8_t* q) { q[kOffZero] = 0xFF; }));
  CHECK(refusedWith([](uint8_t* q) { q[kOffType] = 8; }));
  CHECK(refusedWith([](uint8_t* q) { q[kOffType] = 255; }));
  CHECK(refusedWith([](uint8_t* q) { std::memset(q + kOffSeq, 0, 4); }));
  CHECK(refusedWith([](uint8_t* q) { q[kOffMarker] = kWrapMarker; }));
  CHECK(refusedWith([](uint8_t* q) { q[kOffMarker] = 0; }));
  CHECK(!refusedWith([](uint8_t* q) { q[kOffPiece] = 200; }));  // piece and pieceTotal are not validated

  // encodeHeader masks a sendState above 7 to its three bits (ESP-NOW 1.0 step 6)
  // and never sets a reserved bit (0xC0).
  for (int st = 0; st < 256; ++st) {
    Header s = mk(3);
    s.sendState = static_cast<uint8_t>(st);
    encodeHeader(s, p);
    CHECK((p[kOffFlags] & kFlagsReserved) == 0);
    CHECK(decodeHeader(p, d));
    CHECK(d.sendState == (st & 7));
  }

  // The status byte (ESP-NOW 1.0 step 6): kept only on a refused sent record (1-15).
  {
    Header s = mk(3);
    s.sent = true;
    s.sendState = kRefusedState;
    s.status = 4;
    encodeHeader(s, p);
    CHECK(decodeHeader(p, d) && d.status == 4 && d.sendState == kRefusedState);
    s.sendState = 4;
    encodeHeader(s, p);
    CHECK(decodeHeader(p, d) && d.status == 0);
    s.sent = false;
    s.sendState = kRefusedState;
    encodeHeader(s, p);
    CHECK(decodeHeader(p, d) && d.status == 0);
    s.sent = true;
    s.status = 16;
    encodeHeader(s, p);
    CHECK(decodeHeader(p, d) && d.status == 0);
    s.status = 4;
    encodeHeader(s, p);
    p[kOffZero] = 16;
    p[kOffCheck] = headerCheck(p);
    CHECK(!decodeHeader(p, d));
    s.sendState = 2;
    encodeHeader(s, p);
    p[kOffZero] = 4;
    p[kOffCheck] = headerCheck(p);
    CHECK(!decodeHeader(p, d));
  }

  // decodeCount counts every call, refused ones too.
  const uint32_t before = decodeCount();
  encodeHeader(mk(5), p);
  CHECK(decodeHeader(p, d));
  p[0] = 0;
  CHECK(!decodeHeader(p, d));
  CHECK(decodeCount() - before == 2);

  // The header is little-endian at the documented offsets.
  encodeHeader(mk(0x04030201, 0x08070605, 9, 10, 2), p);
  CHECK(p[kOffSeq] == 1 && p[kOffSeq + 3] == 4 && p[kOffReqId] == 5 && p[kOffReqId + 3] == 8);
  CHECK(p[kOffPiece] == 9 && p[kOffPieceTotal] == 10 && p[kOffType] == 2 && p[kOffMarker] == kRecordMarker);
}

// ---------------------------------------------------------------------------------------------
// 2. small helpers

static void testHelpers() {
  CHECK(footprint(0) == 20);
  CHECK(footprint(1) == 24);
  CHECK(footprint(4) == 24);
  CHECK(footprint(5) == 28);
  CHECK(footprint(200) == 220);
  CHECK(footprint(255) == 276);
  CHECK(kMaxFootprint == 276);

  // ringBudget
  Budget b = ringBudget(0, 16, 32, 1024 * 1024, 16);
  CHECK(b.ringBytes == 16384 && !b.clampedToBoardMax && !b.loweredForTotalCap);
  b = ringBudget(40, 16, 32, 1024 * 1024, 16);
  CHECK(b.ringBytes == 32768 && b.clampedToBoardMax && !b.loweredForTotalCap);
  b = ringBudget(32, 16, 32, 1024 * 1024, 16);  // exactly the share: not lowered
  CHECK(b.ringBytes == 32768 && !b.clampedToBoardMax && !b.loweredForTotalCap);
  b = ringBudget(32, 16, 32, 900 * 1024, 16);   // a quarter of a smaller PSRAM
  CHECK(b.ringBytes == 28800 && b.loweredForTotalCap);
  b = ringBudget(4, 2, 4, 64 * 1024, 16);       // no PSRAM, 16 slots
  CHECK(b.ringBytes == 2048 && b.loweredForTotalCap && !b.clampedToBoardMax);
  b = ringBudget(4, 2, 4, 64 * 1024, 8);
  CHECK(b.ringBytes == 4096 && !b.loweredForTotalCap);
  b = ringBudget(0, 2, 4, 64 * 1024, 16);
  CHECK(b.ringBytes == 2048 && !b.loweredForTotalCap);
  b = ringBudget(32, 2, 4, 64 * 1024, 8);       // setting above the no-PSRAM board maximum
  CHECK(b.ringBytes == 4096 && b.clampedToBoardMax);
  b = ringBudget(1, 16, 32, 1000, 16);          // never below kMinRingBytes
  CHECK(b.ringBytes == kMinRingBytes && b.loweredForTotalCap);
  b = ringBudget(16, 16, 32, 50010, 2);         // share 12502 -> floor4 12500
  CHECK(b.ringBytes == 12500 && b.ringBytes % 4 == 0 && b.loweredForTotalCap);
  b = ringBudget(16, 16, 32, 0, 0);             // capacity 0: no cap
  CHECK(b.ringBytes == 16384 && !b.loweredForTotalCap);
  b = ringBudget(16, 16, 32, 0, -3);            // negative capacity: no cap
  CHECK(b.ringBytes == 16384 && !b.loweredForTotalCap);
  b = ringBudget(0, 0, 32, 1024 * 1024, 4);     // default 0 -> 1 KB
  CHECK(b.ringBytes == 1024);
  b = ringBudget(100, 16, 100, 0xFFFFFFFF, 1);  // above the core maximum
  CHECK(b.ringBytes == kMaxRingBytes);
  b = ringBudget(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 1);
  CHECK(b.ringBytes == kMaxRingBytes);
  for (uint32_t cap = 0; cap < 200000; cap += 997) {
    for (int slots = 1; slots <= 16; ++slots) {
      b = ringBudget(32, 16, 32, cap, slots);
      CHECK(b.ringBytes % 4 == 0 && b.ringBytes >= kMinRingBytes && b.ringBytes <= 32768);
    }
  }

  // unattributedName
  CHECK(unattributedName(nullptr, "AA:BB:CC:DD:EE:FF"));
  CHECK(unattributedName("", "AA:BB:CC:DD:EE:FF"));
  CHECK(unattributedName("AA:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:FF"));
  CHECK(!unattributedName("aa:bb:cc:dd:ee:ff", "AA:BB:CC:DD:EE:FF"));
  CHECK(!unattributedName("Kitchen", "AA:BB:CC:DD:EE:FF"));
  CHECK(!unattributedName("Kitchen", nullptr));
  CHECK(unattributedName(nullptr, nullptr));

  // boundedCopy
  char out[64];
  std::memset(out, 'Z', sizeof out);
  CHECK(boundedCopy(nullptr, 10, "abc", 3) == 0);
  CHECK(boundedCopy(out, 0, "abc", 3) == 0 && out[0] == 'Z');
  CHECK(boundedCopy(out, 1, "abc", 3) == 0 && out[0] == '\0' && out[1] == 'Z');
  std::memset(out, 'Z', sizeof out);
  CHECK(boundedCopy(out, 2, "abc", 3) == 1 && std::strcmp(out, "a") == 0 && out[2] == 'Z');
  CHECK(boundedCopy(out, 64, nullptr, 10) == 0 && out[0] == '\0');
  CHECK(boundedCopy(out, 64, "abcdef", 3) == 3 && std::strcmp(out, "abc") == 0);
  CHECK(boundedCopy(out, 64, "ab", 32) == 2 && std::strcmp(out, "ab") == 0);
  CHECK(boundedCopy(out, 64, "abc", 0) == 0 && out[0] == '\0');
  char* raw = new char[32];  // an unterminated 32-byte name, sized exactly (ASan)
  std::memset(raw, 'n', 32);
  CHECK(boundedCopy(out, 64, raw, 32) == 32 && out[32] == '\0' && out[31] == 'n');
  CHECK(boundedCopy(out, 33, raw, 32) == 32 && out[32] == '\0');
  CHECK(boundedCopy(out, 32, raw, 32) == 31 && out[31] == '\0');
  CHECK(boundedCopy(out, 8, raw, 32) == 7 && std::strlen(out) == 7);

  // displayName
  const char* mac = "AA:BB:CC:DD:EE:FF";
  std::memset(out, 'Z', sizeof out);
  displayName(true, false, "Kitchen", 32, mac, out, sizeof out);
  CHECK(out[0] == '\0');
  displayName(true, true, "Kitchen", 32, mac, out, sizeof out);
  CHECK(out[0] == '\0');
  displayName(false, true, "Kitchen", 32, mac, out, sizeof out);
  CHECK(std::strcmp(out, mac) == 0);
  displayName(false, false, "Kitchen", 32, mac, out, sizeof out);
  CHECK(std::strcmp(out, "Kitchen") == 0);
  displayName(false, false, "", 32, mac, out, sizeof out);
  CHECK(std::strcmp(out, mac) == 0);
  displayName(false, false, nullptr, 32, mac, out, sizeof out);
  CHECK(std::strcmp(out, mac) == 0);
  displayName(false, false, "Kitchen", 0, mac, out, sizeof out);
  CHECK(std::strcmp(out, mac) == 0);
  displayName(false, true, "Kitchen", 32, nullptr, out, sizeof out);
  CHECK(out[0] == '\0');
  displayName(false, false, raw, 32, mac, out, 33);
  CHECK(std::strlen(out) == 32);
  displayName(false, false, raw, 32, mac, out, kNameCap);
  CHECK(std::strlen(out) == 31);
  displayName(false, false, "Kitchen", 32, mac, out, 4);
  CHECK(std::strcmp(out, "Kit") == 0);
  displayName(false, true, "Kitchen", 32, mac, out, 6);
  CHECK(std::strcmp(out, "AA:BB") == 0);
  std::memset(out, 'Z', sizeof out);
  displayName(false, false, "Kitchen", 32, mac, out, 0);
  CHECK(out[0] == 'Z');
  displayName(false, false, "Kitchen", 32, mac, out, 1);
  CHECK(out[0] == '\0' && out[1] == 'Z');
  displayName(false, false, "Kitchen", 32, mac, out, 2);
  CHECK(std::strcmp(out, "K") == 0 && out[2] == 'Z');
  displayName(false, false, "Kitchen", 32, mac, nullptr, 10);  // no crash
  delete[] raw;

  // copyText
  Ring g(1024);
  const uint32_t s = nextSeq();
  AppendResult a = put(g, mk(s), "hello world");
  Header h{};
  uint32_t off = 0;
  CHECK(findBySeq(g.r, g.buf, s, a.offset, off, h));
  std::memset(out, 'Z', sizeof out);
  CHECK(copyText(g.buf, off, h, nullptr, 10) == 0);
  CHECK(copyText(g.buf, off, h, out, 0) == 0 && out[0] == 'Z');
  CHECK(copyText(g.buf, off, h, out, 1) == 0 && out[0] == '\0' && out[1] == 'Z');
  std::memset(out, 'Z', sizeof out);
  CHECK(copyText(g.buf, off, h, out, 2) == 1 && std::strcmp(out, "h") == 0 && out[2] == 'Z');
  CHECK(copyText(g.buf, off, h, out, sizeof out) == 11 && std::strcmp(out, "hello world") == 0);
  CHECK(copyText(g.buf, off, h, out, 12) == 11 && std::strcmp(out, "hello world") == 0);
  CHECK(copyText(g.buf, off, h, out, 11) == 10 && std::strcmp(out, "hello worl") == 0);
}

// ---------------------------------------------------------------------------------------------
// 3. placement

static void testPlacement() {
  // ringInit / ringReset
  RingState r{};
  CHECK(!ringInit(r, 1023, false) && r.size == 0);
  CHECK(!ringInit(r, 1020, false) && r.size == 0);
  CHECK(!ringInit(r, 65540, true) && r.size == 0 && r.sent == 0);
  CHECK(!ringInit(r, 0, false));
  CHECK(ringInit(r, 1024, true) && r.size == 1024 && r.sent == 1 && r.wrapAt == 1024 && r.count == 0);
  CHECK(stateSane(r));
  CHECK(ringInit(r, 65536, false) && stateSane(r));
  CHECK(ringInit(r, 1100, false) && stateSane(r));

  // Exact fit at the end, then a wrap with no marker.
  {
    Ring g(1024);
    for (int i = 0; i < 4; ++i) put(g, mk(nextSeq()), textOf(lenForFootprint(256), i));
    CHECK(g.r.head == 1024 && g.r.tail == 0 && !g.r.wrapped && g.r.count == 4);
    AppendResult a = put(g, mk(nextSeq()), textOf(lenForFootprint(256), 9));
    CHECK(a.evicted == 1 && a.offset == 0 && !a.reset);
    CHECK(g.r.wrapped && g.r.wrapAt == 1024 && g.r.tail == 256 && g.r.head == 256);
    CHECK(g.guardsOk());  // no marker byte at buf[size]
  }
  // A 4-byte gap: the marker is written.
  {
    Ring g(1024);
    put(g, mk(nextSeq()), textOf(lenForFootprint(256), 1));  // 0
    put(g, mk(nextSeq()), textOf(lenForFootprint(256), 2));  // 256
    put(g, mk(nextSeq()), textOf(lenForFootprint(252), 3));  // 512
    put(g, mk(nextSeq()), textOf(lenForFootprint(256), 4));  // 764 -> 1020
    CHECK(g.r.head == 1020 && !g.r.wrapped);
    AppendResult a = put(g, mk(nextSeq()), textOf(lenForFootprint(256), 5));
    CHECK(a.evicted == 1 && a.offset == 0);
    CHECK(g.r.wrapped && g.r.wrapAt == 1020 && g.buf[1020] == kWrapMarker && g.r.tail == 256 && g.r.head == 256);
    // Eviction inside the upper segment.
    a = put(g, mk(nextSeq()), textOf(lenForFootprint(256), 6));
    CHECK(a.evicted == 1 && a.offset == 256 && g.r.tail == 512 && g.r.wrapped);
    // Eviction across the wrap: the last upper record goes, tail == wrapAt, and the ring is unwrapped again.
    a = put(g, mk(nextSeq()), textOf(lenForFootprint(256), 7));
    CHECK(a.evicted == 2 && a.offset == 512);
    CHECK(!g.r.wrapped && g.r.tail == 0 && g.r.wrapAt == 1024 && g.r.head == 768 && g.r.count == 3);
  }
  // Empty text, 255-byte text, 300-byte text cut to 255; padding is zero.
  {
    Ring g(2048);
    const uint32_t s0 = nextSeq();
    AppendResult a = put(g, mk(s0), "");
    CHECK(!a.truncated && g.r.used == 20);
    const uint32_t s1 = nextSeq();
    std::string t255 = textOf(255, 3);
    a = put(g, mk(s1), t255);
    CHECK(!a.truncated && g.r.used == 20 + 276);
    CHECK(g.buf[a.offset + 20 + 255] == 0);  // the one pad byte
    const uint32_t s2 = nextSeq();
    std::string t300 = textOf(300, 4);
    a = put(g, mk(s2), t300);
    CHECK(a.truncated);
    std::vector<Rec> v = walk(g.r, g.buf);
    CHECK(v.size() == 3);
    CHECK(v[0].h.length == 0 && textAt(g.buf, v[0]).empty());
    CHECK(v[1].h.length == 255 && textAt(g.buf, v[1]) == t255);
    CHECK(v[2].h.length == 255 && textAt(g.buf, v[2]) == t300.substr(0, 255));
    const uint32_t s3 = nextSeq();
    a = put(g, mk(s3), "abcde");  // 25 -> 28: three pad bytes
    CHECK(g.buf[a.offset + 25] == 0 && g.buf[a.offset + 26] == 0 && g.buf[a.offset + 27] == 0);
    // Embedded NULs are kept: the record has exactly `length` bytes.
    const char withNul[] = {'a', '\0', 'b'};
    AppendResult b = append(g.r, g.buf, mk(nextSeq()), withNul, 3);
    CHECK(b.ok);
    Header h{};
    uint32_t off = 0;
    CHECK(findBySeq(g.r, g.buf, g.r.newestSeq, b.offset, off, h) && h.length == 3);
    // A null text with length 0 is an empty record.
    CHECK(append(g.r, g.buf, mk(nextSeq()), nullptr, 0).ok);
    CHECK(checkRing(g.r, g.buf).ok && g.guardsOk());
  }
  // The new record is always kept, the stored fields round trip, and the sent bit follows the ring.
  {
    Ring g(1024, true);
    const uint32_t s = nextSeq();
    put(g, mk(s, 42, 3, 4, 5, 2, true, true), "x");
    std::vector<Rec> v = walk(g.r, g.buf);
    CHECK(v.size() == 1 && v[0].h.sent && v[0].h.reqId == 42 && v[0].h.piece == 3 && v[0].h.pieceTotal == 4 &&
          v[0].h.type == 5 && v[0].h.sendState == 2 && v[0].h.encrypted && v[0].h.unattributed &&
          v[0].h.timestamp == tsOf(s));
  }
  // The model on many sizes and lengths: state, offsets, texts; I9 after every evicting append.
  const uint32_t sizes[] = {1024, 1028, 1100, 2048, 4096};
  for (uint32_t size : sizes) {
    Ring g(size);
    ModelRing m;
    m.init(size);
    for (int i = 0; i < 1500; ++i) {
      uint32_t len;
      switch (rndBelow(5)) {
        case 0: len = 0; break;
        case 1: len = 255; break;
        case 2: len = 256 + rndBelow(60); break;
        default: len = rndBelow(256); break;
      }
      const Header h = mk(nextSeq(), rndBelow(4), static_cast<uint8_t>(rndBelow(4)), static_cast<uint8_t>(rndBelow(4)));
      const std::string t = textOf(len, static_cast<uint32_t>(i));
      int mev = 0;
      uint32_t mpos = 0;
      m.add(h, t, mev, mpos);
      AppendResult a = put(g, h, t);
      CHECK(a.offset == mpos);
      CHECK(a.evicted == mev);
      CHECK(a.truncated == (len > kTextMax));
      if (a.evicted) CHECK(g.r.used >= size - 2 * kMaxFootprint);  // I9
      checkAgainstModel(g.r, g.buf, m);
      CHECK(g.guardsOk());
    }
    CHECK(g.r.evicted > 0 && g.r.resets == 0);
  }
}

// ---------------------------------------------------------------------------------------------
// 4. refusals

static void testRefusals() {
  Ring g(1024);
  for (int i = 0; i < 11; ++i) put(g, mk(nextSeq()), textOf(100, i));
  CHECK(g.r.wrapped);
  std::vector<uint8_t> snap(g.buf, g.buf + g.size);
  const RingState rs = g.r;
  auto unchanged = [&]() {
    return std::memcmp(&rs, &g.r, sizeof rs) == 0 && std::memcmp(snap.data(), g.buf, g.size) == 0 && g.guardsOk();
  };
  const std::string t = "text";
  AppendResult a = append(g.r, g.buf, mk(0), t.data(), t.size());
  CHECK(!a.ok && !a.insane && unchanged());
  a = append(g.r, g.buf, mk(g.r.newestSeq), t.data(), t.size());
  CHECK(!a.ok && unchanged());
  a = append(g.r, g.buf, mk(g.r.newestSeq - 1), t.data(), t.size());
  CHECK(!a.ok && unchanged());
  a = append(g.r, g.buf, mk(gSeq + 1, 0, 1, 1, 8), t.data(), t.size());
  CHECK(!a.ok && unchanged());
  a = append(g.r, g.buf, mk(gSeq + 1, 0, 1, 1, 255), t.data(), t.size());
  CHECK(!a.ok && unchanged());
  a = append(g.r, g.buf, mk(gSeq + 1, 0, 1, 1, 0, 8), t.data(), t.size());  // a state above 7 (three bits)
  CHECK(!a.ok && unchanged());
  a = append(g.r, g.buf, mk(gSeq + 1), nullptr, 5);
  CHECK(!a.ok && unchanged());
  a = append(g.r, nullptr, mk(gSeq + 1), t.data(), t.size());
  CHECK(!a.ok && unchanged());
  // An empty ring takes any nonzero seq; it does not remember the last one (seqs are the caller's).
  Ring e(1024);
  CHECK(append(e.r, e.buf, mk(5), "a", 1).ok);
  CHECK(!append(e.r, e.buf, mk(5), "a", 1).ok);
  CHECK(!append(e.r, e.buf, mk(4), "a", 1).ok);
  CHECK(append(e.r, e.buf, mk(6), "a", 1).ok);
  gSeq = std::max<uint32_t>(gSeq, 6);
}

// ---------------------------------------------------------------------------------------------
// 5. validated iteration

static void testIteration() {
  Ring g(1024, false, /*guard=*/false);  // exact allocation
  fillWrapped(g, 4);
  CHECK(g.r.wrapped && g.r.wrapAt < g.r.size);
  std::vector<Rec> all = walk(g.r, g.buf);
  CHECK(all.size() == g.r.count && all.size() >= 4);

  // One changed byte anywhere in record k's header: the walk yields records 0..k-1, then stops as bad.
  for (size_t k = 0; k < all.size(); ++k) {
    for (uint32_t b = 0; b < kHeaderBytes; ++b) {
      const int deltas[] = {1, 0x80, 0xFF};
      for (int delta : deltas) {
        uint8_t& byte = g.buf[all[k].off + b];
        const uint8_t keep = byte;
        byte = static_cast<uint8_t>(byte + delta);
        bool bad = false;
        std::vector<Rec> v = walk(g.r, g.buf, &bad);
        CHECK(bad && v.size() == k);
        CHECK(!checkRing(g.r, g.buf).ok);
        byte = keep;
      }
    }
    // Text bytes are not validated: changing one changes nothing for the walk.
    if (all[k].h.length) {
      uint8_t& byte = g.buf[all[k].off + kHeaderBytes];
      const uint8_t keep = byte;
      byte ^= 0xFF;
      bool bad = true;
      CHECK(walk(g.r, g.buf, &bad).size() == all.size() && !bad);
      byte = keep;
    }
  }
  // A damaged wrap marker: the upper segment is read, then the walk stops.
  {
    size_t upper = 0;
    for (const Rec& r : all) upper += r.off >= g.r.tail;
    const uint8_t keep = g.buf[g.r.wrapAt];
    g.buf[g.r.wrapAt] = 0;
    bool bad = false;
    std::vector<Rec> v = walk(g.r, g.buf, &bad);
    CHECK(bad && v.size() == upper);
    CHECK(!checkRing(g.r, g.buf).ok);
    g.buf[g.r.wrapAt] = keep;
  }
  // Forged fields with the check recomputed: a longer length, a lower seq, the other direction.
  for (size_t k = 0; k < all.size(); ++k) {
    uint8_t* p = g.buf + all[k].off;
    uint8_t keep[kHeaderBytes];
    std::memcpy(keep, p, sizeof keep);
    p[kOffLength] = static_cast<uint8_t>(p[kOffLength] + 4);
    p[kOffCheck] = headerCheck(p);
    bool bad = false;
    std::vector<Rec> v = walk(g.r, g.buf, &bad);
    CHECK(bad && (v.size() == k || v.size() == k + 1));
    std::memcpy(p, keep, sizeof keep);

    p[kOffFlags] ^= kFlagSent;
    p[kOffCheck] = headerCheck(p);
    v = walk(g.r, g.buf, &bad);
    CHECK(bad && v.size() == k);
    std::memcpy(p, keep, sizeof keep);

    if (k > 0) {
      detail::put32(p + kOffSeq, all[k - 1].h.seq);
      p[kOffCheck] = headerCheck(p);
      v = walk(g.r, g.buf, &bad);
      CHECK(bad && v.size() == k);
      std::memcpy(p, keep, sizeof keep);
    }
  }
  bool bad = true;
  CHECK(walk(g.r, g.buf, &bad).size() == all.size() && !bad);
  CHECK(checkRing(g.r, g.buf).ok);

  // A state that is still sane but names other end seqs: the walk stops at the first or the last record.
  {
    RingState x = g.r;
    x.newestSeq += 1;
    CHECK(stateSane(x));
    std::vector<Rec> v = walk(x, g.buf, &bad);
    CHECK(bad && v.size() == all.size() - 1 && !checkRing(x, g.buf).ok);
    x = g.r;
    x.oldestSeq -= 1;
    CHECK(stateSane(x));
    v = walk(x, g.buf, &bad);
    CHECK(bad && v.empty() && !checkRing(x, g.buf).ok);
  }

  // An empty ring walks nothing and is not bad; a null buffer is bad.
  Ring e(1024);
  CHECK(walk(e.r, e.buf, &bad).empty() && !bad);
  Cursor c = begin(g.r);
  uint32_t off = 0;
  Header h{};
  CHECK(!next(g.r, nullptr, c, off, h) && c.bad);
}

// ---------------------------------------------------------------------------------------------
// 6. hostile text

static std::string hostileText(uint32_t len, uint32_t phase, const std::vector<uint32_t>& seqs, bool sent) {
  std::string t(len, static_cast<char>(kRecordMarker));
  for (uint32_t i = 0; i < len; i += 2) t[i] = static_cast<char>(kWrapMarker);
  size_t k = 0;
  for (uint32_t at = phase * 4; at + kHeaderBytes <= len; at += kHeaderBytes) {
    Header f = mk(seqs.empty() ? 1 : seqs[k++ % seqs.size()], 900, 1, 2);
    f.length = static_cast<uint8_t>(rndBelow(4) * 8);
    f.sent = sent;
    uint8_t p[kHeaderBytes];
    encodeHeader(f, p);
    std::memcpy(&t[at], p, kHeaderBytes);
  }
  return t;
}

static void testHostileText() {
  for (int sentRing = 0; sentRing < 2; ++sentRing) {
    Ring g(2048, sentRing != 0, /*guard=*/false);
    ModelRing m;
    m.init(2048);
    std::vector<uint32_t> liveSeqs;
    for (int i = 0; i < 40 || !g.r.wrapped; ++i) {
      std::vector<uint32_t> fakeSeqs = liveSeqs;
      for (uint32_t f = 1; f <= 5; ++f) fakeSeqs.push_back(gSeq + f);  // future seqs too
      const uint32_t s = nextSeq();
      const std::string t = hostileText(200 + rndBelow(56), static_cast<uint32_t>(i % 5), fakeSeqs, sentRing != 0);
      const Header h = mk(s, 500 + i / 2, static_cast<uint8_t>(1 + i % 2), 2);  // pairs: one message each
      int ev = 0;
      uint32_t pos = 0;
      m.add(h, t, ev, pos);
      put(g, h, t);
      checkAgainstModel(g.r, g.buf, m);
      liveSeqs.clear();
      for (const MRec& r : m.q) liveSeqs.push_back(r.seq);
    }
    CHECK(g.r.wrapped);
    // Find by the right hint and by the walk.
    for (const MRec& r : m.q) {
      uint32_t off = 0;
      Header h{};
      CHECK(findBySeq(g.r, g.buf, r.seq, r.off, off, h) && off == r.off);
      CHECK(findBySeq(g.r, g.buf, r.seq, 1, off, h) && off == r.off);
      CHECK(findBySeq(g.r, g.buf, r.seq, kNoEntry, off, h) && off == r.off);
    }
    // Future seqs carried by fakes are not found.
    uint32_t off = 0;
    Header h{};
    CHECK(!findBySeq(g.r, g.buf, gSeq + 1, 0, off, h));
    // Reassembly of each message is exactly its pieces.
    uint16_t scratch[256] = {};
    for (const MRec& r : m.q) {
      if (r.h.piece != 1) continue;
      std::string want;
      for (const MRec& q : m.q)
        if (q.h.reqId == r.h.reqId && q.h.piece == 1) want += q.text;
      for (const MRec& q : m.q)
        if (q.h.reqId == r.h.reqId && q.h.piece == 2) want += q.text;
      char out[600];
      bool complete = false, damaged = false;
      const int n = reassemble(g.r, g.buf, r.h.reqId, out, sizeof out, &complete, scratch, &damaged);
      CHECK(n == static_cast<int>(want.size()) && std::memcmp(out, want.data(), want.size()) == 0 && !damaged);
      CHECK(scratchZero(scratch));
    }
    // reqId 900 is only in the fakes.
    char out[64];
    bool complete = true;
    CHECK(reassemble(g.r, g.buf, 900, out, sizeof out, &complete, scratch) == 0 && !complete);
    // Collapse: one row per reqId present.
    MergeScratch ms;
    Source src{&g.r, g.buf, 3};
    Ref refs[64];
    uint32_t chain[64];
    const int w = collapseNewest(&src, 1, refs, 64, ms, chain, 64);
    std::vector<uint32_t> ids;
    for (const MRec& r : m.q)
      if (std::find(ids.begin(), ids.end(), r.h.reqId) == ids.end()) ids.push_back(r.h.reqId);
    CHECK(w == static_cast<int>(ids.size()));
    for (int i = 0; i < w; ++i) CHECK(refs[i].reqId == ids[i] && refs[i].slot == 3 && refs[i].isSent == (sentRing != 0));
    // setSendState for 900 changes nothing.
    std::vector<uint8_t> snap(g.buf, g.buf + g.size);
    CHECK(setSendState(g.r, g.buf, 900, 1) == 0);
    CHECK(std::memcmp(snap.data(), g.buf, g.size) == 0);

    // DESIGN 6.3, the documented residual: a hint the API never produced that points at a fake header in received
    // text carrying a live seq returns that place. It is still inside the ring, and the walk finds the true record.
    const MRec& a = m.q[m.q.size() - 2];
    const MRec& b = m.q[1];
    Header fake = mk(b.seq);
    fake.sent = sentRing != 0;
    fake.length = 0;
    const uint32_t at = a.off + kHeaderBytes + 8;
    uint8_t keep[kHeaderBytes];
    std::memcpy(keep, g.buf + at, kHeaderBytes);
    encodeHeader(fake, g.buf + at);
    CHECK(findBySeq(g.r, g.buf, b.seq, at, off, h) && off == at && at + kHeaderBytes <= g.size);
    CHECK(findBySeq(g.r, g.buf, b.seq, at + 1, off, h) && off == b.off);
    CHECK(checkRing(g.r, g.buf).ok);
    std::memcpy(g.buf + at, keep, kHeaderBytes);
  }
}

// ---------------------------------------------------------------------------------------------
// 7. findBySeq

static void testFindBySeq() {
  const uint32_t size = 1100;
  uint8_t* block = new uint8_t[2 * size];  // received ring, then the sent ring right after it; exact (ASan)
  std::memset(block, 0, 2 * size);
  RingState rr{}, rs{};
  CHECK(ringInit(rr, size, false) && ringInit(rs, size, true));
  uint8_t* bufs[2] = {block, block + size};
  RingState* rings[2] = {&rr, &rs};
  for (int d = 0; d < 2; ++d) {
    // Short records now and then, so that segLimit-4..16 falls inside a header as well as inside text.
    for (int i = 0; i < 14 || !rings[d]->wrapped || rings[d]->wrapAt == size; ++i) {
      CHECK(i < 200);
      const uint32_t len = (i % 4 == 3) ? 0 : 40 + 13 * static_cast<uint32_t>(i % 14);
      put(*rings[d], bufs[d], mk(nextSeq()), textOf(len, static_cast<uint32_t>(i)));
    }
    CHECK(rings[d]->wrapped && rings[d]->wrapAt < size);
  }
  for (int d = 0; d < 2; ++d) {
    const RingState& r = *rings[d];
    const uint8_t* buf = bufs[d];
    std::vector<Rec> all = walk(r, buf);
    for (const Rec& rec : all) {
      uint32_t off = 0;
      Header h{};
      const uint32_t before = decodeCount();
      CHECK(findBySeq(r, buf, rec.h.seq, rec.off, off, h) && off == rec.off && sameHeader(h, rec.h));
      CHECK(decodeCount() - before == 1);  // fast path: one decode
      for (uint32_t hint = 0; hint <= size + 8; ++hint) {
        CHECK(findBySeq(r, buf, rec.h.seq, hint, off, h) && off == rec.off && h.seq == rec.h.seq);
      }
      const uint32_t odd[] = {kNoEntry, 0xFFFFFFFC, 0x80000000, size - 4, size - 16};
      for (uint32_t hint : odd) CHECK(findBySeq(r, buf, rec.h.seq, hint, off, h) && off == rec.off);
      // Hints at segLimit - 4/8/12/16 of the upper and the lower segment.
      const uint32_t limits[] = {r.wrapAt, r.head};
      for (uint32_t lim : limits)
        for (uint32_t back = 4; back <= 16; back += 4) {
          bool damaged = false;
          CHECK(findBySeq(r, buf, rec.h.seq, lim - back, off, h, &damaged) && off == rec.off && !damaged);
        }
    }
    // Dead, never written, future and zero seqs; an empty ring.
    uint32_t off = 0;
    Header h{};
    bool damaged = false;
    CHECK(!findBySeq(r, buf, r.oldestSeq - 1, 0, off, h, &damaged) && !damaged);
    CHECK(!findBySeq(r, buf, r.newestSeq + 1, 0, off, h, &damaged) && !damaged);
    CHECK(!findBySeq(r, buf, 0, 0, off, h));
    CHECK(!findBySeq(r, nullptr, r.newestSeq, 0, off, h));
  }
  // A segment that ends at the very end of an exactly sized buffer, unwrapped (head == size) and then wrapped
  // (wrapAt == size): hints in the last 16 bytes land on 0xA5 text bytes, so a header read past the segment would
  // leave the allocation (ASan).
  {
    Ring x(1024, true, /*guard=*/false);
    const std::string a5(lenForFootprint(256), static_cast<char>(kRecordMarker));
    for (int i = 0; i < 4; ++i) put(x, mk(nextSeq()), a5);
    CHECK(!x.r.wrapped && x.r.head == x.size);
    for (int round = 0; round < 2; ++round) {
      for (const Rec& rec : walk(x.r, x.buf)) {
        uint32_t off = 0;
        Header h{};
        for (uint32_t hint = x.size - 16; hint < x.size + 4; ++hint)
          CHECK(findBySeq(x.r, x.buf, rec.h.seq, hint, off, h) && off == rec.off);
      }
      put(x, mk(nextSeq()), a5);
      CHECK(x.r.wrapped && x.r.wrapAt == x.size);
    }
  }
  // A seq of the other ring is never found here.
  uint32_t off = 0;
  Header h{};
  for (const Rec& rec : walk(rs, bufs[1])) CHECK(!findBySeq(rr, bufs[0], rec.h.seq, rec.off, off, h));
  Ring e(1024);
  CHECK(!findBySeq(e.r, e.buf, 1, 0, off, h));
  delete[] block;

  // An evicted seq is refused even when its old offset now holds a valid header carrying that very seq.
  Ring g(1024);
  const uint32_t first = nextSeq();
  const AppendResult a0 = put(g, mk(first), textOf(100, 1));
  while (g.r.oldestSeq == first) put(g, mk(nextSeq()), textOf(150, 2));
  Header fake = mk(first);
  fake.length = 0;
  // The old offset lies in free space or inside a live record's text; write the fake there directly.
  encodeHeader(fake, g.buf + a0.offset);
  bool damaged = false;
  CHECK(!findBySeq(g.r, g.buf, first, a0.offset, off, h, &damaged) && !damaged);
}

// ---------------------------------------------------------------------------------------------
// 8. stale references

static void staleRefs(uint32_t size) {
  Ring g(size);
  ModelRing m;
  m.init(size);
  for (int i = 0; i < 300 && !(m.wrapped && m.q.size() > 3); ++i) {
    const Header h = mk(nextSeq());
    const std::string t = textOf(rndBelow(120), static_cast<uint32_t>(i));
    int ev = 0;
    uint32_t pos = 0;
    m.add(h, t, ev, pos);
    put(g, h, t);
  }
  checkAgainstModel(g.r, g.buf, m);
  std::vector<Ref> refs(1024);
  std::vector<uint32_t> chain(1024);
  MergeScratch ms;
  Source src{&g.r, g.buf, 0};
  const int w = collapseNewest(&src, 1, refs.data(), 1024, ms, chain.data(), 1024);
  CHECK(w == static_cast<int>(m.q.size()));
  refs.resize(static_cast<size_t>(w));
  for (int i = 0; i < w; ++i) CHECK(refs[i].offset == m.q[i].off && refs[i].seqNum == m.q[i].seq);

  int deadSeen = 0;
  for (int step = 0; step < 3 * static_cast<int>(size / 200) + 10; ++step) {
    const uint32_t len = 100 + rndBelow(156);
    // Where will the record go? Ask a copy of the model, then put a fake header carrying each ref's seq at that
    // ref's old offset wherever the new text covers it.
    ModelRing probe = m;
    int ev = 0;
    const uint32_t pos = probe.place(footprint(len), ev);
    std::string t = textOf(len, static_cast<uint32_t>(step));
    for (const Ref& ref : refs) {
      const uint32_t textStart = pos + kHeaderBytes;
      if (ref.offset >= textStart && ref.offset + kHeaderBytes <= textStart + len) {
        Header f = mk(ref.seqNum);
        f.length = 0;
        uint8_t p[kHeaderBytes];
        encodeHeader(f, p);
        std::memcpy(&t[ref.offset - textStart], p, kHeaderBytes);
      }
    }
    const Header h = mk(nextSeq());
    uint32_t mpos = 0;
    m.add(h, t, ev, mpos);
    CHECK(mpos == pos);
    put(g, h, t);
    checkAgainstModel(g.r, g.buf, m);
    for (const Ref& ref : refs) {
      uint32_t off = 0;
      Header got{};
      bool damaged = false;
      const bool live = m.live(ref.seqNum);
      CHECK(findBySeq(g.r, g.buf, ref.seqNum, ref.offset, off, got, &damaged) == live);
      CHECK(!damaged);
      if (live) {
        CHECK(off == ref.offset);
        for (const MRec& q : m.q)
          if (q.seq == ref.seqNum) {
            CHECK(sameHeader(got, q.h));
            CHECK(textAt(g.buf, {off, got}) == q.text);
          }
      } else {
        ++deadSeen;
      }
      // Copying the head through a stale ref with the walk (no hint) agrees.
      CHECK(findBySeq(g.r, g.buf, ref.seqNum, kNoEntry, off, got) == live);
    }
  }
  CHECK(deadSeen > 0);
  CHECK(m.q.front().seq > refs.back().seqNum);  // every ref was evicted by the end
}

static void testStaleRefs() {
  staleRefs(1024);
  staleRefs(1100);
  staleRefs(16384);
}

// ---------------------------------------------------------------------------------------------
// 9. setSendState

static void testSendState() {
  Ring g(1024, true);
  // Fragments of reqId 77 in both segments.
  for (int i = 0; i < 60; ++i) {
    const bool frag = i % 3 == 0;
    put(g, mk(nextSeq(), frag ? 77 : 10 + static_cast<uint32_t>(i), static_cast<uint8_t>(1 + i / 3 % 9), frag ? 9 : 1),
        textOf(50 + rndBelow(40), static_cast<uint32_t>(i)));
    if (!g.r.wrapped) continue;
    bool upper = false, lower = false;
    for (const Rec& r : walk(g.r, g.buf))
      if (r.h.reqId == 77) (r.off >= g.r.tail ? upper : lower) = true;
    if (upper && lower && i > 20) break;
  }
  std::vector<Rec> all = walk(g.r, g.buf);
  int n77 = 0;
  bool upper = false, lower = false;
  for (const Rec& r : all)
    if (r.h.reqId == 77) {
      ++n77;
      (r.off >= g.r.tail ? upper : lower) = true;
    }
  CHECK(g.r.wrapped && upper && lower && n77 >= 2);

  for (uint8_t state = 0; state <= kSendStateMax; ++state) {
    std::vector<uint8_t> snap(g.buf, g.buf + g.size);
    const RingState rs = g.r;
    bool damaged = false;
    CHECK(setSendState(g.r, g.buf, 77, state, &damaged) == n77 && !damaged);
    CHECK(std::memcmp(&rs, &g.r, sizeof rs) == 0);
    for (uint32_t i = 0; i < g.size; ++i) {
      if (snap[i] == g.buf[i]) continue;
      bool allowed = false;
      for (const Rec& r : all)
        if (r.h.reqId == 77 && (i == r.off + kOffFlags || i == r.off + kOffCheck)) allowed = true;
      CHECK(allowed);
    }
    CHECK(checkRing(g.r, g.buf).ok && g.guardsOk());
    for (const Rec& r : walk(g.r, g.buf)) {
      for (const Rec& o : all)
        if (o.off == r.off) {
          Header want = o.h;
          if (o.h.reqId == 77) want.sendState = state;
          CHECK(sameHeader(r.h, want));
        }
    }
  }
  std::vector<uint8_t> snap(g.buf, g.buf + g.size);
  const RingState rs = g.r;
  for (int state = 8; state < 256; ++state) CHECK(setSendState(g.r, g.buf, 77, static_cast<uint8_t>(state)) == 0);
  CHECK(setSendState(g.r, g.buf, 0, 1) == 0);
  CHECK(setSendState(g.r, g.buf, 123456, 1) == 0);
  CHECK(setSendState(g.r, nullptr, 77, 1) == 0);
  CHECK(std::memcmp(&rs, &g.r, sizeof rs) == 0 && std::memcmp(snap.data(), g.buf, g.size) == 0);

  // A damaged record: the ones before it are stamped, *damaged is set.
  size_t k = all.size() - 1;
  while (all[k].h.reqId != 77) --k;
  int before = 0;
  for (size_t i = 0; i < k; ++i) before += all[i].h.reqId == 77;
  g.buf[all[k].off + kOffZero] = 9;
  bool damaged = false;
  CHECK(setSendState(g.r, g.buf, 77, 1, &damaged) == before && damaged);
}

// ---------------------------------------------------------------------------------------------
// 10. reassemble

// Today's espnowReassembleByReqId on a list of live records (oldest first), with an int loop counter.
static int todayReassemble(const std::vector<MRec>& ring, uint32_t reqId, char* out, size_t cap, bool* complete) {
  if (complete) *complete = false;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (reqId == 0) return 0;
  // Today's rule, with one change (review 2026-11-18): when the reqId has fragments (pieceTotal > 1), only they
  // make the message; single-frame records sharing the reqId (streamed command output) are left out.
  int total = 0;
  bool multi = false;
  for (const MRec& m : ring)
    if (m.h.reqId == reqId && m.h.pieceTotal > 1) {
      total = m.h.pieceTotal;
      multi = true;
      break;
    }
  if (!multi)
    for (const MRec& m : ring)
      if (m.h.reqId == reqId) {
        total = 1;
        break;
      }
  if (total == 0) return 0;
  size_t pos = 0;
  int found = 0;
  for (int p = 1; p <= total; p++) {
    for (const MRec& m : ring) {
      if (m.h.reqId != reqId || m.h.piece != p || (m.h.pieceTotal > 1) != multi) continue;
      size_t l = m.text.size();
      if (pos + l >= cap) l = (cap > pos + 1) ? (cap - 1 - pos) : 0;
      if (l) {
        std::memcpy(out + pos, m.text.data(), l);
        pos += l;
      }
      found++;
      break;
    }
    if (pos >= cap - 1) break;
  }
  out[pos] = '\0';
  if (complete) *complete = (found >= total);
  return static_cast<int>(pos);
}

static void testReassemble() {
  uint16_t scratch[256] = {};
  char out[1024];
  bool complete = false, damaged = false;
  {
    Ring g(2048);
    put(g, mk(nextSeq(), 5, 3, 3), "CC");
    put(g, mk(nextSeq(), 6, 1, 1), "other");
    put(g, mk(nextSeq(), 5, 1, 3), "AA");
    put(g, mk(nextSeq(), 5, 2, 3), "BB");
    put(g, mk(nextSeq(), 5, 2, 3), "XX");  // duplicate: the oldest wins
    CHECK(reassemble(g.r, g.buf, 5, out, sizeof out, &complete, scratch, &damaged) == 6);
    CHECK(std::strcmp(out, "AABBCC") == 0 && complete && !damaged && scratchZero(scratch));
    // cap truncation, cap 0/1/2, null out, null complete
    CHECK(reassemble(g.r, g.buf, 5, out, 5, &complete, scratch) == 4 && std::strcmp(out, "AABB") == 0);
    CHECK(!complete && scratchZero(scratch));  // as today: only the pieces reached before the stop count
    std::memset(out, 'Z', 8);
    CHECK(reassemble(g.r, g.buf, 5, out, 0, &complete, scratch) == 0 && out[0] == 'Z' && !complete);
    CHECK(reassemble(g.r, g.buf, 5, out, 1, &complete, scratch) == 0 && out[0] == '\0' && out[1] == 'Z');
    CHECK(scratchZero(scratch));
    std::memset(out, 'Z', 8);
    CHECK(reassemble(g.r, g.buf, 5, out, 2, &complete, scratch) == 1 && std::strcmp(out, "A") == 0 && out[2] == 'Z');
    CHECK(scratchZero(scratch));
    CHECK(reassemble(g.r, g.buf, 5, nullptr, 10, &complete, scratch) == 0 && !complete);
    CHECK(reassemble(g.r, g.buf, 5, out, sizeof out, nullptr, scratch) == 6);
    CHECK(reassemble(g.r, g.buf, 0, out, sizeof out, &complete, scratch) == 0 && out[0] == '\0' && !complete);
    CHECK(reassemble(g.r, g.buf, 99, out, sizeof out, &complete, scratch) == 0 && !complete);
    CHECK(reassemble(g.r, g.buf, 5, out, sizeof out, &complete, nullptr) == 0);
    // Stopped early by cap, then a call for a different reqId gives the right text.
    CHECK(reassemble(g.r, g.buf, 5, out, 3, &complete, scratch) == 2 && scratchZero(scratch));
    CHECK(reassemble(g.r, g.buf, 6, out, sizeof out, &complete, scratch) == 5 && std::strcmp(out, "other") == 0);
    CHECK(complete && scratchZero(scratch));
    // A dirty scratch from a caller bug does not leak into the result.
    for (int i = 0; i < 256; ++i) scratch[i] = static_cast<uint16_t>(i * 3 + 1);
    CHECK(reassemble(g.r, g.buf, 5, out, sizeof out, &complete, scratch) == 6 && std::strcmp(out, "AABBCC") == 0);
    CHECK(scratchZero(scratch));
  }
  {
    // Missing piece; total from the oldest match; piece 0 and piece > total ignored.
    Ring g(2048);
    put(g, mk(nextSeq(), 8, 1, 2), "one");
    put(g, mk(nextSeq(), 8, 3, 5), "three");
    put(g, mk(nextSeq(), 8, 0, 2), "zero");
    CHECK(reassemble(g.r, g.buf, 8, out, sizeof out, &complete, scratch) == 3 && std::strcmp(out, "one") == 0);
    CHECK(!complete && scratchZero(scratch));
    put(g, mk(nextSeq(), 9, 2, 3), "b");
    CHECK(reassemble(g.r, g.buf, 9, out, sizeof out, &complete, scratch) == 1 && !complete);
    // pieceTotal 0 counts as 1.
    put(g, mk(nextSeq(), 11, 1, 0), "solo");
    CHECK(reassemble(g.r, g.buf, 11, out, sizeof out, &complete, scratch) == 4 && complete);
  }
  {
    // A hostile pieceTotal 255 with one piece terminates, with at most count + total decodes.
    Ring g(4096);
    for (int i = 0; i < 30; ++i) put(g, mk(nextSeq(), 1000 + static_cast<uint32_t>(i)), "f");
    put(g, mk(nextSeq(), 42, 255, 255), "last");
    const uint32_t before = decodeCount();
    CHECK(reassemble(g.r, g.buf, 42, out, sizeof out, &complete, scratch) == 4 && !complete);
    CHECK(decodeCount() - before <= g.r.count + 255u);
    CHECK(scratchZero(scratch));
    // pieceTotal 255, every piece present, a cap that stops in the middle.
    Ring h(65536);
    std::string want;
    for (int p = 255; p >= 1; --p) put(h, mk(nextSeq(), 43, static_cast<uint8_t>(p), 255), textOf(3, static_cast<uint32_t>(p)));
    for (int p = 1; p <= 255; ++p) want += textOf(3, static_cast<uint32_t>(p));
    std::vector<char> big(1024);
    CHECK(reassemble(h.r, h.buf, 43, big.data(), big.size(), &complete, scratch) == 765 && complete);
    CHECK(std::string(big.data()) == want && scratchZero(scratch));
    CHECK(reassemble(h.r, h.buf, 43, big.data(), 100, &complete, scratch) == 99 && scratchZero(scratch));
    CHECK(std::string(big.data()) == want.substr(0, 99));
  }
  {
    // Damage after some pieces: the pieces before it are used and *damaged is set.
    Ring g(2048);
    put(g, mk(nextSeq(), 7, 1, 3), "x1");
    put(g, mk(nextSeq(), 7, 2, 3), "x2");
    const AppendResult a = put(g, mk(nextSeq(), 7, 3, 3), "x3");
    g.buf[a.offset + kOffSeq] ^= 1;
    damaged = false;
    CHECK(reassemble(g.r, g.buf, 7, out, sizeof out, &complete, scratch, &damaged) == 4);
    CHECK(std::strcmp(out, "x1x2") == 0 && !complete && damaged && scratchZero(scratch));
  }
  // Table test against today's algorithm on random rings with wraps and evictions.
  for (int round = 0; round < 300; ++round) {
    const uint32_t sizes[] = {1024, 1100, 2048, 4096};
    Ring g(sizes[round % 4]);
    ModelRing m;
    m.init(g.size);
    const int ops = 10 + static_cast<int>(rndBelow(120));
    for (int i = 0; i < ops; ++i) {
      const uint32_t id = rndBelow(6);
      const Header h = mk(nextSeq(), id, static_cast<uint8_t>(rndBelow(6)), static_cast<uint8_t>(rndBelow(6)));
      const std::string t = textOf(rndBelow(160), static_cast<uint32_t>(i));
      int ev = 0;
      uint32_t pos = 0;
      m.add(h, t, ev, pos);
      put(g, h, t);
    }
    std::vector<MRec> live(m.q.begin(), m.q.end());
    const size_t caps[] = {1, 2, 3, 17, 100, 1024};
    for (uint32_t id = 0; id < 6; ++id)
      for (size_t cap : caps) {
        char a[1024], b[1024];
        bool ca = false, cb = false;
        const int na = reassemble(g.r, g.buf, id, a, cap, &ca, scratch);
        const int nb = todayReassemble(live, id, b, cap, &cb);
        CHECK(na == nb && ca == cb && std::memcmp(a, b, static_cast<size_t>(na) + 1) == 0);
        CHECK(scratchZero(scratch));
      }
  }
}

// ---------------------------------------------------------------------------------------------
// 11. collapseNewest

struct TodayRef {
  uint32_t seqNum, reqId, timestamp;
  uint8_t slot;
  size_t index;
  uint8_t partsPresent, partsTotal;
  bool isSent;
};

// Today's espnowCollapseRing (startW / maxN as today).
static int todayCollapseRing(const std::vector<MRec>& ring, uint8_t slot, bool isSent, std::vector<TodayRef>& out,
                             int startW, int maxN) {
  int w = startW;
  for (size_t idx = 0; idx < ring.size(); ++idx) {
    const MRec& msg = ring[idx];
    if (msg.h.reqId != 0 && msg.h.pieceTotal > 1) {
      int g = -1;
      for (int k = startW; k < w; k++)
        if (out[k].reqId == msg.h.reqId && out[k].isSent == isSent && out[k].slot == slot &&
            out[k].partsTotal > 1) {  // review 2026-11-18: a single-frame entry is never joined
          g = k;
          break;
        }
      if (g >= 0) {
        if (out[g].partsPresent < 255) out[g].partsPresent++;
        if (msg.h.piece < ring[out[g].index].h.piece) {
          out[g].index = idx;
          out[g].seqNum = msg.seq;
          out[g].timestamp = msg.h.timestamp;
        }
        if (msg.h.pieceTotal > out[g].partsTotal) out[g].partsTotal = msg.h.pieceTotal;
        continue;
      }
    }
    if (w >= maxN) break;
    TodayRef& r = out[w];
    r.seqNum = msg.seq;
    r.reqId = msg.h.reqId;
    r.timestamp = msg.h.timestamp;
    r.slot = slot;
    r.index = idx;
    r.partsPresent = 1;
    r.partsTotal = (msg.h.pieceTotal > 1) ? msg.h.pieceTotal : 1;
    r.isSent = isSent;
    w++;
  }
  return w;
}
static void todaySort(std::vector<TodayRef>& out, int w) {
  for (int i = 0; i < w - 1; i++)
    for (int j = 0; j < w - i - 1; j++)
      if (out[j].timestamp > out[j + 1].timestamp) std::swap(out[j], out[j + 1]);
}

struct Peer {
  Ring recv{2048, false};
  Ring sent{2048, true};
  ModelRing mr, ms;
  std::vector<uint32_t> recentIds;
  Peer(uint32_t size) : recv(size, false), sent(size, true) {
    mr.init(size);
    ms.init(size);
  }
};

static uint32_t gReq = 100000;

// One logical message into a peer's ring: a single, or the fragments of a group (shuffled, maybe one missing or
// repeated); sometimes a recent reqId again, so groups re-merge as today.
static void addMessage(Peer& p, bool toSent) {
  Ring& g = toSent ? p.sent : p.recv;
  ModelRing& m = toSent ? p.ms : p.mr;
  auto one = [&](const Header& h, const std::string& t) {
    int ev = 0;
    uint32_t pos = 0;
    m.add(h, t, ev, pos);
    put(g, h, t);
  };
  const uint32_t kind = rndBelow(10);
  if (kind < 4) {
    one(mk(nextSeq(), rndBelow(2) ? 0 : ++gReq, 1, 1), textOf(rndBelow(80), kind));
    return;
  }
  uint32_t id = ++gReq;
  if (kind == 9 && !p.recentIds.empty()) id = p.recentIds[rndBelow(static_cast<uint32_t>(p.recentIds.size()))];
  p.recentIds.push_back(id);
  if (p.recentIds.size() > 6) p.recentIds.erase(p.recentIds.begin());
  const uint8_t total = static_cast<uint8_t>(2 + rndBelow(4));
  std::vector<uint8_t> pieces;
  for (uint8_t q = 1; q <= total; ++q) pieces.push_back(q);
  for (size_t i = pieces.size(); i > 1; --i) std::swap(pieces[i - 1], pieces[rndBelow(static_cast<uint32_t>(i))]);
  if (rndBelow(4) == 0) pieces.pop_back();
  if (rndBelow(4) == 0) pieces.push_back(pieces.front());
  for (uint8_t q : pieces) one(mk(nextSeq(), id, q, total), textOf(rndBelow(60), q));
}

static std::vector<Ref> collapse(const std::vector<Source>& src, int maxN, bool sort) {
  std::vector<Ref> out(static_cast<size_t>(maxN));
  std::vector<uint32_t> chain(static_cast<size_t>(maxN));
  MergeScratch ms;
  const int w = collapseNewest(src.data(), static_cast<int>(src.size()), out.data(), maxN, ms, chain.data(), maxN);
  out.resize(static_cast<size_t>(w));
  if (sort) sortByTime(out.data(), w);
  return out;
}

static bool sameAsToday(const std::vector<Ref>& got, const std::vector<TodayRef>& want, int w,
                        const std::vector<const std::vector<MRec>*>& ringsBySlotDir) {
  if (static_cast<int>(got.size()) != w) return false;
  for (int i = 0; i < w; ++i) {
    const TodayRef& t = want[static_cast<size_t>(i)];
    const Ref& g = got[static_cast<size_t>(i)];
    const MRec& head = (*ringsBySlotDir[t.slot * 2u + (t.isSent ? 1u : 0u)])[t.index];
    if (g.seqNum != t.seqNum || g.reqId != t.reqId || g.timestamp != t.timestamp || g.slot != t.slot ||
        g.partsPresent != t.partsPresent || g.partsTotal != t.partsTotal || g.isSent != t.isSent ||
        g.headPiece != head.h.piece || g.offset != head.off)
      return false;
  }
  return true;
}

static void testCollapseAgainstToday() {
  for (int round = 0; round < 120; ++round) {
    const uint32_t sizes[] = {1024, 2048, 4096};
    const int peers = 1 + static_cast<int>(rndBelow(4));
    std::vector<Peer*> ps;
    for (int i = 0; i < peers; ++i) ps.push_back(new Peer(sizes[rndBelow(3)]));
    const int msgs = 5 + static_cast<int>(rndBelow(150));
    for (int i = 0; i < msgs; ++i) addMessage(*ps[rndBelow(static_cast<uint32_t>(peers))], rndBelow(3) == 0);

    std::vector<std::vector<MRec>> lists;
    for (Peer* p : ps) {
      lists.emplace_back(p->mr.q.begin(), p->mr.q.end());
      lists.emplace_back(p->ms.q.begin(), p->ms.q.end());
    }
    std::vector<const std::vector<MRec>*> bySlotDir;
    for (const auto& l : lists) bySlotDir.push_back(&l);
    const int big = 2048;

    // Peer view: one source (received), chronological, no sort.
    {
      std::vector<TodayRef> t(big);
      const int w = todayCollapseRing(lists[0], 0, false, t, 0, big);
      std::vector<Source> src{{&ps[0]->recv.r, ps[0]->recv.buf, 0}};
      CHECK(sameAsToday(collapse(src, big, false), t, w, bySlotDir));
    }
    // Conversation: received + sent of one peer, then the time sort.
    {
      std::vector<TodayRef> t(big);
      int w = todayCollapseRing(lists[0], 0, false, t, 0, big);
      w = todayCollapseRing(lists[1], 0, true, t, w, big);
      todaySort(t, w);
      std::vector<Source> src{{&ps[0]->recv.r, ps[0]->recv.buf, 0}, {&ps[0]->sent.r, ps[0]->sent.buf, 0}};
      CHECK(sameAsToday(collapse(src, big, true), t, w, bySlotDir));
      // Exactly fitting maxN gives the same.
      if (w > 0) CHECK(sameAsToday(collapse(src, w, true), t, w, bySlotDir));
    }
    // All messages: both rings of every peer.
    {
      std::vector<TodayRef> t(big);
      int w = 0;
      std::vector<Source> src;
      for (int i = 0; i < peers; ++i) {
        w = todayCollapseRing(lists[2 * i], static_cast<uint8_t>(i), false, t, w, big);
        w = todayCollapseRing(lists[2 * i + 1], static_cast<uint8_t>(i), true, t, w, big);
        src.push_back({&ps[i]->recv.r, ps[i]->recv.buf, static_cast<uint8_t>(i)});
        src.push_back({&ps[i]->sent.r, ps[i]->sent.buf, static_cast<uint8_t>(i)});
      }
      todaySort(t, w);
      CHECK(sameAsToday(collapse(src, big, true), t, w, bySlotDir));
    }
    for (Peer* p : ps) delete p;
  }
}

static void testCollapseRules() {
  // Head = lowest piece, partsTotal = max, partsPresent counts every fragment.
  {
    Ring g(2048);
    put(g, mk(nextSeq(), 5, 2, 3), "b");
    const uint32_t s1 = nextSeq();
    const AppendResult a1 = put(g, mk(s1, 5, 1, 4), "a");
    put(g, mk(nextSeq(), 5, 3, 3), "c");
    std::vector<Source> src{{&g.r, g.buf, 2}};
    std::vector<Ref> v = collapse(src, 8, false);
    CHECK(v.size() == 1 && v[0].seqNum == s1 && v[0].offset == a1.offset && v[0].headPiece == 1 &&
          v[0].timestamp == tsOf(s1) && v[0].partsPresent == 3 && v[0].partsTotal == 4 && v[0].slot == 2);
  }
  // partsPresent caps at 255.
  {
    Ring g(32768);
    for (int i = 0; i < 300; ++i) put(g, mk(nextSeq(), 6, 2, 2), "");
    std::vector<Source> src{{&g.r, g.buf, 0}};
    std::vector<Ref> v = collapse(src, 8, false);
    CHECK(v.size() == 1 && v[0].partsPresent == 255 && v[0].partsTotal == 2);
  }
  // reqId 0 and pieceTotal 1 never join a group; sent and received never merge; different slots never merge.
  {
    Ring g(2048);
    put(g, mk(nextSeq(), 0, 1, 2), "a");
    put(g, mk(nextSeq(), 0, 2, 2), "b");
    put(g, mk(nextSeq(), 9, 1, 1), "c");
    put(g, mk(nextSeq(), 9, 1, 1), "d");
    std::vector<Source> src{{&g.r, g.buf, 0}};
    CHECK(collapse(src, 8, false).size() == 4);
    // A fragment never joins an entry a single-frame record with its reqId opened: it opens its own, and the
    // single entries keep their text (a remote command's streamed lines, then its chunked result).
    put(g, mk(nextSeq(), 9, 2, 2), "e");
    std::vector<Ref> v = collapse(src, 8, false);
    CHECK(v.size() == 5 && v[2].partsPresent == 1 && v[2].partsTotal == 1 && v[3].partsPresent == 1 &&
          v[3].partsTotal == 1 && v[4].partsPresent == 1 && v[4].partsTotal == 2 && v[4].headPiece == 2);
    Ring s(2048, true);
    put(s, mk(nextSeq(), 9, 2, 2), "f");
    Ring o(2048);
    put(o, mk(nextSeq(), 9, 2, 2), "g");
    std::vector<Source> three{{&g.r, g.buf, 0}, {&s.r, s.buf, 0}, {&o.r, o.buf, 1}};
    v = collapse(three, 16, false);
    CHECK(v.size() == 7 && v[5].isSent && v[5].reqId == 9 && v[6].slot == 1 && v[6].reqId == 9);
  }
  // Streamed output then a chunked result under one reqId (memreport from a peer): 90 single-frame lines, then
  // pieces 2, 1, 3 of 3. Every line keeps its own row and text; the result is one row of its own, headed by
  // piece 1, and reassembles to the result alone (review 2026-11-18: it used to join the last line's row, which
  // then showed the first line's text).
  {
    Ring g(16384);
    std::vector<uint32_t> lineSeq;
    for (int i = 0; i < 90; ++i) {
      lineSeq.push_back(nextSeq());
      put(g, mk(lineSeq.back(), 31, 1, 1), "line" + std::to_string(i));
    }
    put(g, mk(nextSeq(), 31, 2, 3), "RB");
    const uint32_t s1 = nextSeq();
    const AppendResult a1 = put(g, mk(s1, 31, 1, 3), "RA");
    put(g, mk(nextSeq(), 31, 3, 3), "RC");
    std::vector<Source> src{{&g.r, g.buf, 4}};
    std::vector<Ref> v = collapse(src, 512, false);
    CHECK(v.size() == 91);
    bool linesOk = v.size() == 91;
    for (int i = 0; linesOk && i < 90; ++i)
      linesOk = v[static_cast<size_t>(i)].seqNum == lineSeq[static_cast<size_t>(i)] &&
                v[static_cast<size_t>(i)].partsTotal == 1 && v[static_cast<size_t>(i)].partsPresent == 1;
    CHECK(linesOk);
    if (v.size() == 91) {
      const Ref& r = v[90];
      CHECK(r.seqNum == s1 && r.offset == a1.offset && r.headPiece == 1 && r.partsPresent == 3 && r.partsTotal == 3 &&
            r.reqId == 31 && r.slot == 4);
    }
    // The newest 8 only: the result is still its own row, the 7 lines before it keep theirs.
    v = collapse(src, 8, false);
    CHECK(v.size() == 8 && v[7].partsTotal == 3 && v[7].partsPresent == 3 && v[6].partsTotal == 1 &&
          v[6].seqNum == lineSeq[89]);
    uint16_t scratch[256] = {};
    char out[256];
    bool complete = false;
    CHECK(reassemble(g.r, g.buf, 31, out, sizeof out, &complete, scratch) == 6 && std::strcmp(out, "RARBRC") == 0 &&
          complete && scratchZero(scratch));
    // Only single-frame records with the reqId: the oldest one with piece 1, as today.
    Ring h(2048);
    put(h, mk(nextSeq(), 32, 1, 1), "first");
    put(h, mk(nextSeq(), 32, 1, 1), "second");
    CHECK(reassemble(h.r, h.buf, 32, out, sizeof out, &complete, scratch) == 5 && std::strcmp(out, "first") == 0 &&
          complete && scratchZero(scratch));
    // A missing piece of the result leaves it incomplete; the lines never stand in for it.
    Ring k(4096);
    put(k, mk(nextSeq(), 33, 1, 1), "L1");
    put(k, mk(nextSeq(), 33, 2, 2), "P2");
    CHECK(reassemble(k.r, k.buf, 33, out, sizeof out, &complete, scratch) == 2 && std::strcmp(out, "P2") == 0 &&
          !complete && scratchZero(scratch));
  }
  // The same-source window: a fragment joins its group across 63 newer same-source entries, not across 64.
  for (int between = 62; between <= 65; ++between) {
    Ring g(16384);
    put(g, mk(nextSeq(), 77, 1, 2), "p1");
    for (int i = 0; i < between; ++i) put(g, mk(nextSeq()), "x");
    put(g, mk(nextSeq(), 77, 2, 2), "p2");
    std::vector<Source> src{{&g.r, g.buf, 0}};
    const size_t rows = collapse(src, 512, false).size();
    CHECK(rows == static_cast<size_t>(between) + (between < kGroupWindow ? 1u : 2u));
  }
  // 200 entries of another source in between do not split a group.
  {
    Ring a(16384), b(16384);
    put(a, mk(nextSeq(), 78, 1, 2), "p1");
    for (int i = 0; i < 200; ++i) put(b, mk(nextSeq()), "y");
    put(a, mk(nextSeq(), 78, 2, 2), "p2");
    std::vector<Source> src{{&a.r, a.buf, 0}, {&b.r, b.buf, 1}};
    std::vector<Ref> v = collapse(src, 512, false);
    CHECK(v.size() == 201 && v[0].reqId == 78 && v[0].partsPresent == 2);
  }
  // The newest maxN are kept, chronologically, for every window start and maxN 1, 2, 7, 63, 64, 65.
  {
    const int maxNs[] = {1, 2, 7, 63, 64, 65};
    for (int maxN : maxNs) {
      for (int total = 0; total <= 2 * maxN + 3; total += (maxN > 10 ? 7 : 1)) {
        Ring g(8192);
        std::vector<uint32_t> seqs;
        for (int i = 0; i < total; ++i) {
          seqs.push_back(nextSeq());
          put(g, mk(seqs.back()), "");
        }
        std::vector<Source> src{{&g.r, g.buf, 0}};
        std::vector<Ref> v = collapse(src, maxN, false);
        const int keep = std::min(total, maxN);
        CHECK(static_cast<int>(v.size()) == keep);
        for (int i = 0; i < keep; ++i) CHECK(v[static_cast<size_t>(i)].seqNum == seqs[static_cast<size_t>(total - keep + i)]);
      }
    }
    // A group whose fragments are spread over the window boundary: the newest rows win, and a fragment whose
    // group was dropped opens a new row.
    Ring g(8192);
    put(g, mk(nextSeq(), 50, 1, 2), "a");
    for (int i = 0; i < 5; ++i) put(g, mk(nextSeq()), "x");
    put(g, mk(nextSeq(), 50, 2, 2), "b");
    std::vector<Source> src{{&g.r, g.buf, 0}};
    std::vector<Ref> v = collapse(src, 3, false);
    CHECK(v.size() == 3 && v[2].reqId == 50 && v[2].headPiece == 2 && v[2].partsPresent == 1);
    v = collapse(src, 6, false);
    CHECK(v.size() == 6 && v[0].reqId == 50 && v[0].partsPresent == 2);
  }
  // Argument refusals write nothing.
  {
    Ring g(2048);
    put(g, mk(nextSeq()), "a");
    Source src{&g.r, g.buf, 0};
    MergeScratch ms;
    Ref out[4];
    uint32_t chain[4];
    std::memset(out, 0xEE, sizeof out);
    std::memset(chain, 0xEE, sizeof chain);
    auto untouched = [&]() {
      for (size_t i = 0; i < sizeof out; ++i)
        if (reinterpret_cast<uint8_t*>(out)[i] != 0xEE) return false;
      for (size_t i = 0; i < sizeof chain; ++i)
        if (reinterpret_cast<uint8_t*>(chain)[i] != 0xEE) return false;
      return true;
    };
    CHECK(collapseNewest(&src, 1, nullptr, 4, ms, chain, 4) == 0 && untouched());
    CHECK(collapseNewest(&src, 1, out, 4, ms, nullptr, 4) == 0 && untouched());
    CHECK(collapseNewest(&src, 1, out, 0, ms, chain, 4) == 0 && untouched());
    CHECK(collapseNewest(&src, 1, out, -1, ms, chain, 4) == 0 && untouched());
    CHECK(collapseNewest(&src, 1, out, 4, ms, chain, 3) == 0 && untouched());
    CHECK(collapseNewest(nullptr, 1, out, 4, ms, chain, 4) == 0 && untouched());
    CHECK(collapseNewest(&src, -1, out, 4, ms, chain, 4) == 0 && untouched());
    std::vector<Source> many(33, src);
    CHECK(collapseNewest(many.data(), 33, out, 4, ms, chain, 4) == 0 && untouched());
    CHECK(collapseNewest(&src, 0, out, 4, ms, chain, 4) == 0 && untouched());
    CHECK(collapseNewest(many.data(), 32, out, 4, ms, chain, 4) == 4);  // 32 sources are allowed
  }
  // A damaged source ends at the damage and is marked; the others are complete.
  {
    Ring a(2048), b(2048);
    for (int i = 0; i < 5; ++i) {
      put(a, mk(nextSeq()), "a");
      put(b, mk(nextSeq()), "b");
    }
    std::vector<Rec> ra = walk(a.r, a.buf);
    a.buf[ra[2].off + kOffType] = 9;
    std::vector<Source> src{{&a.r, a.buf, 0}, {&b.r, b.buf, 1}};
    MergeScratch ms;
    Ref out[16];
    uint32_t chain[16];
    CHECK(collapseNewest(src.data(), 2, out, 16, ms, chain, 16) == 7);
    CHECK(ms.cur[0].bad && !ms.cur[1].bad);
  }
  // sortByTime: stable, no-op on null or n <= 0.
  {
    Ref v[6] = {};
    const uint32_t ts[] = {5, 3, 5, 1, 3, 5};
    for (int i = 0; i < 6; ++i) {
      v[i].timestamp = ts[i];
      v[i].seqNum = static_cast<uint32_t>(i);
    }
    sortByTime(v, 6);
    const uint32_t order[] = {3, 1, 4, 0, 2, 5};
    for (int i = 0; i < 6; ++i) CHECK(v[i].seqNum == order[i]);
    sortByTime(nullptr, 6);
    Ref w[2] = {};
    w[0].timestamp = 9;
    w[1].timestamp = 1;
    sortByTime(w, 0);
    sortByTime(w, -4);
    CHECK(w[0].timestamp == 9);
  }
}

static void testMerge() {
  Ring a(2048), b(2048, true), c(2048), e(2048);
  std::vector<uint32_t> order;
  for (int i = 0; i < 30; ++i) {
    Ring& g = (i % 3 == 0) ? a : (i % 3 == 1) ? b : c;
    order.push_back(nextSeq());
    put(g, mk(order.back()), textOf(30, static_cast<uint32_t>(i)));
  }
  std::vector<Source> src{{&a.r, a.buf, 0}, {&b.r, b.buf, 0}, {&c.r, c.buf, 1}, {&e.r, e.buf, 2}};
  MergeScratch ms;
  std::vector<uint32_t> seen;
  int n = mergeBySeq(src.data(), 4, 0, ms, [&](int, uint32_t, const Header& h) {
    seen.push_back(h.seq);
    return true;
  });
  CHECK(n == 30 && seen == order);
  // sinceSeq
  seen.clear();
  n = mergeBySeq(src.data(), 4, order[19], ms, [&](int i, uint32_t off, const Header& h) {
    const Source& s = src[static_cast<size_t>(i)];
    Header x{};
    uint32_t o = 0;
    CHECK(findBySeq(*s.ring, s.buf, h.seq, off, o, x) && o == off);
    seen.push_back(h.seq);
    return true;
  });
  CHECK(n == 10 && std::equal(seen.begin(), seen.end(), order.begin() + 20));
  // The visitor stops the merge; the stopping call counts.
  n = mergeBySeq(src.data(), 4, 0, ms, [&](int, uint32_t, const Header& h) { return h.seq < order[4]; });
  CHECK(n == 5);
  // Idle sources are skipped without a decode.
  uint32_t before = decodeCount();
  n = mergeBySeq(src.data(), 4, order.back(), ms, [&](int, uint32_t, const Header&) { return true; });
  CHECK(n == 0 && decodeCount() == before);
  // Only the sources with something newer are walked.
  before = decodeCount();
  n = mergeBySeq(src.data(), 4, order[28], ms, [&](int, uint32_t, const Header&) { return true; });
  CHECK(n == 1 && decodeCount() - before == c.r.count);  // the last seq is in c; a and b are skipped
  // Argument refusals.
  bool called = false;
  auto mark = [&](int, uint32_t, const Header&) {
    called = true;
    return true;
  };
  CHECK(mergeBySeq(nullptr, 1, 0, ms, mark) == 0);
  CHECK(mergeBySeq(src.data(), -1, 0, ms, mark) == 0);
  std::vector<Source> many(33, src[0]);
  CHECK(mergeBySeq(many.data(), 33, 0, ms, mark) == 0);
  CHECK(mergeBySeq(src.data(), 0, 0, ms, mark) == 0 && !called);
  // A null ring or buffer is skipped and not marked.
  std::vector<Source> odd{{nullptr, a.buf, 0}, {&a.r, nullptr, 0}, {&c.r, c.buf, 1}};
  CHECK(mergeBySeq(odd.data(), 3, 0, ms, mark) == static_cast<int>(c.r.count));
  CHECK(!ms.cur[0].bad && !ms.cur[1].bad && !ms.cur[2].bad);
}

// ---------------------------------------------------------------------------------------------
// 12. corrupted RingState

struct Mut {
  const char* what;
  void (*f)(RingState&);
};

static void expectRefused(RingState& bad, uint8_t* buf, uint32_t size, const char* what) {
  if (stateSane(bad)) std::fprintf(stderr, "mutation still sane: %s\n", what);
  CHECK(!stateSane(bad));
  std::vector<uint8_t> snap(buf, buf + size);
  const RingState keep = bad;
  const std::string t = "new";
  AppendResult a = append(bad, buf, mk(0xFFFFFFF0u), t.data(), t.size());
  CHECK(!a.ok && a.insane);
  CHECK(std::memcmp(&keep, &bad, sizeof keep) == 0);
  CHECK(begin(bad).bad == 1);
  uint32_t off = 0;
  Header h{};
  bool damaged = false;
  CHECK(!findBySeq(bad, buf, keep.newestSeq ? keep.newestSeq : 1, 0, off, h, &damaged) && damaged);
  damaged = false;
  CHECK(setSendState(bad, buf, 1, 1, &damaged) == 0 && damaged);
  uint16_t scratch[256] = {};
  char out[64];
  damaged = false;
  CHECK(reassemble(bad, buf, 1, out, sizeof out, nullptr, scratch, &damaged) == 0 && damaged && scratchZero(scratch));
  MergeScratch ms;
  Source src{&bad, buf, 0};
  CHECK(mergeBySeq(&src, 1, 0, ms, [](int, uint32_t, const Header&) { return true; }) == 0);
  CHECK(ms.cur[0].bad == 1);
  Ref refs[8];
  uint32_t chain[8];
  CHECK(collapseNewest(&src, 1, refs, 8, ms, chain, 8) == 0);
  CHECK(!checkRing(bad, buf).ok);
  CHECK(std::memcmp(&keep, &bad, sizeof keep) == 0);
  CHECK(std::memcmp(snap.data(), buf, size) == 0);
}

static void testCorruptState() {
  // Wrapped base with a marker gap, exact buffer.
  Ring g(1024, false, false);
  fillWrapped(g, 3, 1);
  CHECK(g.r.wrapped && g.r.wrapAt < g.r.size && g.r.count >= 3 && g.r.head < g.r.tail);
  const Mut wrappedMuts[] = {
      {"size 1023", [](RingState& r) { r.size = 1023; }},
      {"size 1020", [](RingState& r) { r.size = 1020; }},
      {"size 0", [](RingState& r) { r.size = 0; }},
      {"size 65540", [](RingState& r) { r.size = 65540; }},
      {"size huge", [](RingState& r) { r.size = 0xFFFFFFFC; }},
      {"tail unaligned", [](RingState& r) { r.tail += 2; }},
      {"tail past size", [](RingState& r) { r.tail = r.size + 4; }},
      {"tail == wrapAt", [](RingState& r) { r.tail = r.wrapAt; }},
      {"tail below head", [](RingState& r) { r.tail = r.head - 4; }},
      {"head unaligned", [](RingState& r) { r.head += 1; }},
      {"head 0", [](RingState& r) { r.head = 0; }},
      {"head past size", [](RingState& r) { r.head = r.size + 4; }},
      {"head above tail", [](RingState& r) { r.head = r.tail + 4; }},
      {"wrapAt unaligned", [](RingState& r) { r.wrapAt += 2; }},
      {"wrapAt past size", [](RingState& r) { r.wrapAt = r.size + 4; }},
      {"wrapAt == tail", [](RingState& r) { r.wrapAt = r.tail; }},
      {"used + 4", [](RingState& r) { r.used += 4; }},
      {"used - 4", [](RingState& r) { r.used -= 4; }},
      {"count 0", [](RingState& r) { r.count = 0; }},
      {"count above size/20", [](RingState& r) { r.count = static_cast<uint16_t>(r.size / kHeaderBytes + 1); }},
      {"count*20 > used", [](RingState& r) { r.count = static_cast<uint16_t>(r.used / kHeaderBytes + 1); }},
      {"count*276 < used", [](RingState& r) { r.count = 1; }},
      {"oldestSeq 0", [](RingState& r) { r.oldestSeq = 0; }},
      {"oldestSeq > newestSeq", [](RingState& r) { r.oldestSeq = r.newestSeq + 1; }},
      {"newestSeq < oldestSeq", [](RingState& r) { r.newestSeq = r.oldestSeq - 1; }},
      {"seq span < count-1", [](RingState& r) { r.newestSeq = r.oldestSeq + r.count - 2; }},
      {"sent 2", [](RingState& r) { r.sent = 2; }},
      {"wrapped 2", [](RingState& r) { r.wrapped = 2; }},
      {"wrapped 0", [](RingState& r) { r.wrapped = 0; }},
  };
  for (const Mut& m : wrappedMuts) {
    RingState bad = g.r;
    m.f(bad);
    expectRefused(bad, g.buf, g.size, m.what);
  }
  // Unwrapped base.
  Ring u(1024, false, false);
  for (int i = 0; i < 4; ++i) put(u, mk(nextSeq()), textOf(40, i));
  CHECK(!u.r.wrapped && u.r.count == 4);
  const Mut flatMuts[] = {
      {"wrapAt below size", [](RingState& r) { r.wrapAt = r.size - 4; }},
      {"tail == head", [](RingState& r) { r.tail = r.head; }},
      {"used + 4", [](RingState& r) { r.used += 4; }},
      {"wrapped 1", [](RingState& r) { r.wrapped = 1; }},
  };
  for (const Mut& m : flatMuts) {
    RingState bad = u.r;
    m.f(bad);
    expectRefused(bad, u.buf, u.size, m.what);
  }
  // Coordinated damage past the end: head, tail or wrapAt beyond size with used, count, the order and the seqs
  // kept consistent, so only the bounds line (I3) refuses it. Unrefused, append would write a header at
  // head = 1040 of a 1024-byte buffer (exact buffer: ASan reports it). Review 2026-11-18, surviving mutant M09.
  {
    Ring c(1024, false, false);
    const Mut pastEnd[] = {
        {"flat: tail 1000, head 1040", [](RingState& r) {
           r.tail = 1000; r.head = 1040; r.wrapAt = r.size; r.wrapped = 0; r.used = 40; r.count = 1;
           r.oldestSeq = r.newestSeq = 5;
         }},
        {"flat: tail 1024, head 1064", [](RingState& r) {
           r.tail = 1024; r.head = 1064; r.wrapAt = r.size; r.wrapped = 0; r.used = 40; r.count = 1;
           r.oldestSeq = r.newestSeq = 5;
         }},
        {"wrapped: wrapAt 1048, tail 1028", [](RingState& r) {
           r.wrapped = 1; r.head = 40; r.tail = 1028; r.wrapAt = 1048; r.used = (1048 - 1028) + 40; r.count = 2;
           r.oldestSeq = 5; r.newestSeq = 6;
         }},
        {"wrapped: wrapAt 1028, tail 1000", [](RingState& r) {
           r.wrapped = 1; r.head = 20; r.tail = 1000; r.wrapAt = 1028; r.used = (1028 - 1000) + 20; r.count = 2;
           r.oldestSeq = 5; r.newestSeq = 6;
         }},
    };
    for (const Mut& m : pastEnd) {
      RingState bad = c.r;
      m.f(bad);
      expectRefused(bad, c.buf, c.size, m.what);
    }
  }
  // One record: oldestSeq must equal newestSeq.
  Ring one(1024, false, false);
  put(one, mk(nextSeq()), "a");
  {
    RingState bad = one.r;
    bad.oldestSeq = bad.newestSeq - 1;
    expectRefused(bad, one.buf, one.size, "one record, two seqs");
  }
  // Empty base: anything off the normal form.
  Ring e(1024, false, false);
  const Mut emptyMuts[] = {
      {"empty tail 4", [](RingState& r) { r.tail = 4; }},
      {"empty head 4", [](RingState& r) { r.head = 4; }},
      {"empty used 4", [](RingState& r) { r.used = 4; }},
      {"empty oldestSeq", [](RingState& r) { r.oldestSeq = 1; }},
      {"empty newestSeq", [](RingState& r) { r.newestSeq = 1; }},
      {"empty wrapAt", [](RingState& r) { r.wrapAt = r.size - 4; }},
      {"empty wrapped", [](RingState& r) { r.wrapped = 1; }},
      {"empty sent 3", [](RingState& r) { r.sent = 3; }},
  };
  for (const Mut& m : emptyMuts) {
    RingState bad = e.r;
    m.f(bad);
    expectRefused(bad, e.buf, e.size, m.what);
  }
  // The statistics do not take part.
  RingState stats = g.r;
  stats.evicted = 0xFFFFFFFF;
  stats.resets = 12345;
  CHECK(stateSane(stats) && checkRing(stats, g.buf).ok);
  // ringReset keeps size, sent and evicted, counts the reset, and gives the empty normal form.
  RingState rr = g.r;
  rr.sent = 1;
  rr.evicted = 77;
  rr.resets = 4;
  ringReset(rr);
  CHECK(rr.size == 1024 && rr.sent == 1 && rr.evicted == 77 && rr.resets == 5 && rr.count == 0 && stateSane(rr));
  // checkRing catches what stateSane cannot: a header that does not tile.
  RingState wrongUsed = u.r;
  CHECK(checkRing(wrongUsed, u.buf).ok);
  std::vector<Rec> v = walk(u.r, u.buf);
  u.buf[v[1].off + kOffLength] = 0;
  u.buf[v[1].off + kOffCheck] = headerCheck(u.buf + v[1].off);
  CHECK(stateSane(u.r) && !checkRing(u.r, u.buf).ok);
}

// ---------------------------------------------------------------------------------------------
// 13. damage met while evicting

static void expectResetAndKept(Ring& g) {
  const uint32_t s = nextSeq();
  const std::string t = textOf(255, 5);
  AppendResult a = append(g.r, g.buf, mk(s), t.data(), t.size());
  CHECK(a.ok && a.reset && !a.insane);
  CHECK(g.r.count == 1 && g.r.newestSeq == s && g.r.oldestSeq == s && g.r.resets == 1);
  CHECK(checkRing(g.r, g.buf).ok && g.guardsOk());
  std::vector<Rec> v = walk(g.r, g.buf);
  CHECK(v.size() == 1 && textAt(g.buf, v[0]) == t);
}

// X(276) Y(276) Z(276) W(196) fill [0,1024); V U T wrap over them; W is alone in the upper segment at 828.
static void buildEvictBase(Ring& g, std::vector<uint32_t>* offs = nullptr) {
  for (int i = 0; i < 3; ++i) put(g, mk(nextSeq()), textOf(255, i));
  put(g, mk(nextSeq()), textOf(lenForFootprint(196), 3));
  for (int i = 0; i < 3; ++i) put(g, mk(nextSeq()), textOf(255, 10 + i));
  CHECK(g.r.wrapped && g.r.tail == 828 && g.r.head == 828 && g.r.wrapAt == 1024 && g.r.count == 4);
  if (offs) {
    offs->clear();
    for (const Rec& r : walk(g.r, g.buf)) offs->push_back(r.off);
  }
}

static void testEvictDamage() {
  {  // forged length on the tail record: its footprint passes wrapAt
    Ring g(1024);
    buildEvictBase(g);
    uint8_t* p = g.buf + g.r.tail;
    p[kOffLength] = 255;
    p[kOffCheck] = headerCheck(p);
    expectResetAndKept(g);
  }
  {  // forged shorter length: tail would land inside the record
    Ring g(1024);
    for (int i = 0; i < 4; ++i) put(g, mk(nextSeq()), textOf(lenForFootprint(256), i));
    uint8_t* p = g.buf + g.r.tail;
    p[kOffLength] = 0;
    p[kOffCheck] = headerCheck(p);
    expectResetAndKept(g);
  }
  {  // forged seq at the tail
    Ring g(1024);
    buildEvictBase(g);
    uint8_t* p = g.buf + g.r.tail;
    detail::put32(p + kOffSeq, g.r.oldestSeq + 1);
    p[kOffCheck] = headerCheck(p);
    expectResetAndKept(g);
  }
  {  // the other direction at the tail
    Ring g(1024);
    buildEvictBase(g);
    uint8_t* p = g.buf + g.r.tail;
    p[kOffFlags] ^= kFlagSent;
    p[kOffCheck] = headerCheck(p);
    expectResetAndKept(g);
  }
  {  // damaged marker at the tail
    Ring g(1024);
    buildEvictBase(g);
    g.buf[g.r.tail] = 0;
    expectResetAndKept(g);
  }
  {  // the record after the evicted one is damaged
    Ring g(1024);
    std::vector<uint32_t> offs;
    buildEvictBase(g, &offs);
    // The base walk is W(828), V(0), U(276), T(552); appending 276 evicts W and then needs V (at 0).
    g.buf[offs[1] + kOffZero] = 1;
    expectResetAndKept(g);
    // W was evicted before the damage was met and nothing overwrote it: its marker was killed on eviction, so it
    // can never be read as a header again. (Elsewhere the new record always covers an evicted record's first byte.)
    Header old{};
    CHECK(g.buf[offs[0]] == 0 && !decodeHeader(g.buf + offs[0], old));
  }
  {  // the next record carries a seq not above the evicted one
    Ring g(1024);
    for (int i = 0; i < 4; ++i) put(g, mk(nextSeq()), textOf(lenForFootprint(256), i));
    std::vector<Rec> v = walk(g.r, g.buf);
    uint8_t* p = g.buf + v[1].off;
    detail::put32(p + kOffSeq, v[0].h.seq);
    p[kOffCheck] = headerCheck(p);
    expectResetAndKept(g);
  }
  // The same base without damage evicts normally.
  Ring g(1024);
  buildEvictBase(g);
  AppendResult a = put(g, mk(nextSeq()), textOf(255, 1));
  CHECK(a.evicted == 2 && !a.reset && g.r.resets == 0);
}

// ---------------------------------------------------------------------------------------------
// 14. decode budgets at the maximum configuration

static void testHoldBudget() {
  const int n = kMaxSources;
  const uint32_t size = 32768;
  std::vector<uint8_t> mem(static_cast<size_t>(n) * size, 0);
  std::vector<RingState> rings(static_cast<size_t>(n));
  std::vector<Source> src;
  for (int i = 0; i < n; ++i) {
    CHECK(ringInit(rings[static_cast<size_t>(i)], size, i % 2 != 0));
    src.push_back({&rings[static_cast<size_t>(i)], mem.data() + static_cast<size_t>(i) * size, static_cast<uint8_t>(i / 2)});
  }
  uint32_t fragments = 0;
  for (int round = 0; round < 1700; ++round)
    for (int i = 0; i < n; ++i) {
      const bool frag = round % 10 == 0;
      fragments += frag;
      const Header h = mk(nextSeq(), frag ? 1 + static_cast<uint32_t>(round) : 0, 1, frag ? 2 : 1);
      CHECK(append(rings[static_cast<size_t>(i)], mem.data() + static_cast<size_t>(i) * size, h, "", 0).ok);
    }
  uint32_t records = 0, newest = 0;
  for (const RingState& r : rings) {
    CHECK(r.count == size / kHeaderBytes && r.wrapped);  // 1638 records each
    records += r.count;
    newest = std::max(newest, r.newestSeq);
  }
  CHECK(records == 1638u * 32u);
  MergeScratch* ms = new MergeScratch;
  // Idle poll: nothing newer than the newest -> no decode.
  uint32_t before = decodeCount();
  CHECK(mergeBySeq(src.data(), n, newest, *ms, [](int, uint32_t, const Header&) { return true; }) == 0);
  CHECK(decodeCount() == before);
  // Full merge: one decode per record.
  before = decodeCount();
  CHECK(mergeBySeq(src.data(), n, 0, *ms, [](int, uint32_t, const Header&) { return true; }) ==
        static_cast<int>(records));
  CHECK(decodeCount() - before == records);
  // A page of 50 stops early.
  before = decodeCount();
  int seen = 0;
  CHECK(mergeBySeq(src.data(), n, 0, *ms, [&](int, uint32_t, const Header&) { return ++seen < 50; }) == 50);
  CHECK(decodeCount() - before <= 50u + static_cast<uint32_t>(n));
  // The global inbox collapse: at most records + 64 x fragments decodes (chain steps decode nothing).
  std::vector<Ref> out(512);
  std::vector<uint32_t> chain(512);
  before = decodeCount();
  const int w = collapseNewest(src.data(), n, out.data(), 512, *ms, chain.data(), 512);
  CHECK(w == 512);
  const uint32_t used = decodeCount() - before;
  CHECK(used <= records + 64u * fragments);
  CHECK(used == records);
  for (int i = 1; i < w; ++i) CHECK(out[static_cast<size_t>(i)].seqNum > out[static_cast<size_t>(i - 1)].seqNum);
  CHECK(out.back().seqNum == newest);
  // findBySeq: the fast path decodes once; the walk at most count.
  const RingState& r0 = rings[0];
  uint32_t off = 0;
  Header h{};
  CHECK(findBySeq(r0, src[0].buf, r0.newestSeq, kNoEntry, off, h));
  before = decodeCount();
  CHECK(findBySeq(r0, src[0].buf, r0.newestSeq, off, off, h));
  CHECK(decodeCount() - before == 1);
  before = decodeCount();
  CHECK(findBySeq(r0, src[0].buf, r0.newestSeq, 2, off, h));
  CHECK(decodeCount() - before <= r0.count);
  for (int i = 0; i < n; ++i) CHECK(checkRing(rings[static_cast<size_t>(i)], src[static_cast<size_t>(i)].buf).ok);
  delete ms;
}

int main() {
  testHeader();
  testHelpers();
  testPlacement();
  testRefusals();
  testIteration();
  testHostileText();
  testFindBySeq();
  testStaleRefs();
  testSendState();
  testReassemble();
  testCollapseAgainstToday();
  testCollapseRules();
  testMerge();
  testCorruptState();
  testEvictDamage();
  testHoldBudget();
  std::printf("espnow_history_core_tests: %u checks passed\n", gChecks);
  return 0;
}
