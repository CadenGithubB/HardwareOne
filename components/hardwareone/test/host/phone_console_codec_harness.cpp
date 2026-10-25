// Phone link v2 console records (BLE_PhonePipeConsoleCore.h) as shipped: header
// bytes, the P4 ingress validator's bounds and malformed cases, incremental
// scanning over arbitrary splits, the P4 egress chain machine (interleave, the
// 4608 B cap, ABORT), the phone-side receiver, the egress/receiver mirror over a
// random walk, and the INFO payload. Prints INFO lines for the driver's JSON check.
#include "BLE_PhonePipeConsoleCore.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
using namespace hw1rec;
using Bytes = std::vector<uint8_t>;

static unsigned long checks = 0;
#define REQUIRE(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "%s:%d: REQUIRE(%s)\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

struct Rng {
  uint64_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 32); }
  uint32_t below(uint32_t n) { return next() % n; }
};

static const uint8_t kKnown[] = {kInfo, kText, kBlob, kCard, kMirror, kAbort, kTicket, kCommand, kBinary, kResume};
static bool known(uint8_t type) { return std::find(std::begin(kKnown), std::end(kKnown), type) != std::end(kKnown); }

// One record; a BINARY (or any type, when asked) gets the OTA magic first.
static Bytes rec(uint8_t type, uint8_t flags, size_t len, uint8_t fill = 0x41, bool magic = true) {
  Bytes v(kHeader + len, fill);
  encodeHeader(v.data(), type, flags, uint16_t(len));
  if (type == kBinary && magic) std::memcpy(v.data() + kHeader, kOtaMagic, std::min(len, sizeof(kOtaMagic)));
  return v;
}
// scan() over exactly min(have, kScanPeek) bytes on the heap: ASan proves nothing past them is read.
static Scan scanAt(Side at, const uint8_t* stream, size_t have) {
  const size_t n = std::min(have, kScanPeek);
  std::unique_ptr<uint8_t[]> exact(new uint8_t[n ? n : 1]);
  if (n) std::memcpy(exact.get(), stream, n);
  return scan(at, exact.get(), have);
}
static Scan scanAt(Side at, const Bytes& stream) { return scanAt(at, stream.data(), stream.size()); }
static Scan nextAt(PhoneReceiver& r, const uint8_t* stream, size_t have) {
  const size_t n = std::min(have, kScanPeek);
  std::unique_ptr<uint8_t[]> exact(new uint8_t[n ? n : 1]);
  if (n) std::memcpy(exact.get(), stream, n);
  return r.next(exact.get(), have);
}
static Scan nextAt(PhoneReceiver& r, const Bytes& stream) { return nextAt(r, stream.data(), stream.size()); }
static bool is(const Scan& s, Verdict v, Fault f = Fault::None) { return s.verdict == v && s.fault == f; }
static Scan ingress(uint8_t type, uint8_t flags, size_t len) { return scanAt(Side::P4, rec(type, flags, len)); }

static void header() {
  REQUIRE(kHeader == 4 && kRecordMax == 1028 && kScanPeek == 12 && kChainMax == 4608 && kBinaryMax + kHeader == 493);
  const uint8_t ok[] = {'O', 'K'};
  uint8_t out[16];
  REQUIRE(encode(out, sizeof(out), kText, 0, ok, 2) == 6);
  const uint8_t text[] = {0x02, 0x00, 0x02, 0x00, 0x4f, 0x4b};
  REQUIRE(std::memcmp(out, text, 6) == 0);
  REQUIRE(encode(out, sizeof(out), kAbort, 0, nullptr, 0) == 4);
  const uint8_t abort[] = {0x06, 0x00, 0x00, 0x00};
  REQUIRE(std::memcmp(out, abort, 4) == 0);
  const uint8_t help[] = {'h', 'e', 'l', 'p'};
  REQUIRE(encode(out, sizeof(out), kCommand, 0, help, 4) == 8);
  const uint8_t command[] = {0x81, 0x00, 0x04, 0x00, 0x68, 0x65, 0x6c, 0x70};
  REQUIRE(std::memcmp(out, command, 8) == 0);
  encodeHeader(out, kText, kMore, 1024);
  REQUIRE(out[0] == 0x02 && out[1] == 0x01 && out[2] == 0x00 && out[3] == 0x04);
  encodeHeader(out, 0x90, 0, 0x1234);
  REQUIRE(out[2] == 0x34 && out[3] == 0x12);  // len little-endian
  REQUIRE(encode(out, 5, kText, 0, ok, 2) == 0 && encode(out, 6, kText, 0, ok, 2) == 6);
  static uint8_t big[kHeader + 0x10000], source[0x10000];
  REQUIRE(encode(big, sizeof(big), kText, 0, source, 0x10000) == 0 && encode(big, sizeof(big), kText, 0, source, 0xFFFF) == kHeader + 0xFFFF);
  for (unsigned type = 0; type < 256; ++type) {
    for (unsigned flags : {0u, 1u, 2u, 0x80u, 0xFFu}) {
      for (unsigned len : {0u, 1u, 255u, 256u, 511u, 4608u, 0xFFFFu}) {
        encodeHeader(out, uint8_t(type), uint8_t(flags), uint16_t(len));
        const Header h = parseHeader(out);
        REQUIRE(h.type == type && h.flags == flags && h.len == len && h.more() == ((flags & 1) != 0));
      }
    }
  }
}

static void ingressBounds() {
  // COMMAND 1..511, no MORE.
  REQUIRE(is(ingress(kCommand, 0, 0), Verdict::Malformed, Fault::Length));
  REQUIRE(is(ingress(kCommand, 0, 1), Verdict::Deliver) && is(ingress(kCommand, 0, 511), Verdict::Deliver));
  REQUIRE(is(ingress(kCommand, 0, 512), Verdict::Malformed, Fault::Length));
  REQUIRE(is(ingress(kCommand, kMore, 5), Verdict::Malformed, Fault::More));
  // BINARY 13..489 with the OTA magic.
  REQUIRE(is(ingress(kBinary, 0, 12), Verdict::Malformed, Fault::Length));
  REQUIRE(is(ingress(kBinary, 0, 13), Verdict::Deliver) && is(ingress(kBinary, 0, 489), Verdict::Deliver));
  REQUIRE(is(ingress(kBinary, 0, 490), Verdict::Malformed, Fault::Length));
  REQUIRE(is(ingress(kBinary, kMore, 100), Verdict::Malformed, Fault::More));
  REQUIRE(is(scanAt(Side::P4, rec(kBinary, 0, 100, 0x41, false)), Verdict::Malformed, Fault::Magic));
  for (size_t i = 0; i < sizeof(kOtaMagic); ++i) {
    for (uint8_t flip : {uint8_t(0x01), uint8_t(0x80), uint8_t(0xFF)}) {
      Bytes b = rec(kBinary, 0, 13);
      b[kHeader + i] ^= flip;
      REQUIRE(is(scanAt(Side::P4, b), Verdict::Malformed, Fault::Magic));
    }
  }
  // RESUME is reserved: skipped with the unknown bounds.
  REQUIRE(is(ingress(kResume, 0, 0), Verdict::Skip) && is(ingress(kResume, 0, 16), Verdict::Skip));
  REQUIRE(is(ingress(kResume, 0, 1024), Verdict::Skip) && is(ingress(kResume, 0, 1025), Verdict::Malformed, Fault::Length));
  REQUIRE(is(ingress(kResume, kMore, 16), Verdict::Malformed, Fault::More));
  // A P4 -> phone type arriving at the P4 is malformed, whatever its length.
  for (uint8_t type : {kInfo, kText, kBlob, kCard, kMirror, kAbort, kTicket}) {
    for (size_t len : {size_t(0), size_t(1), size_t(256), size_t(1024)}) REQUIRE(is(ingress(type, 0, len), Verdict::Malformed, Fault::Direction));
    REQUIRE(is(ingress(type, kMore, 10), Verdict::Malformed, Fault::Direction));
  }
  // Flag bits 1-7 are malformed on every type.
  for (unsigned type = 0; type < 256; ++type) {
    for (unsigned bit = 1; bit < 8; ++bit) {
      REQUIRE(is(ingress(uint8_t(type), uint8_t(1u << bit), 13), Verdict::Malformed, Fault::Flags));
      REQUIRE(is(ingress(uint8_t(type), uint8_t((1u << bit) | kMore), 13), Verdict::Malformed, Fault::Flags));
    }
  }
  // Every type, flag pattern and boundary length.
  unsigned unknownTypes = 0;
  for (unsigned t = 0; t < 256; ++t) {
    const uint8_t type = uint8_t(t);
    if (!known(type)) {
      ++unknownTypes;
      REQUIRE(is(ingress(type, 0, 0), Verdict::Skip) && is(ingress(type, 0, 1024), Verdict::Skip));
      REQUIRE(is(ingress(type, 0, 1025), Verdict::Malformed, Fault::Length));
      REQUIRE(is(ingress(type, kMore, 1), Verdict::Malformed, Fault::More));
    }
    for (unsigned flags : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 0xFFu}) {
      for (unsigned len : {0u, 1u, 12u, 13u, 16u, 256u, 257u, 488u, 489u, 490u, 511u, 512u, 513u, 1023u, 1024u, 1025u, 4096u, 0xFFFFu}) {
        const Scan s = ingress(type, uint8_t(flags), len);
        REQUIRE(s.size == kHeader + len && s.header.type == type && s.header.len == len);
        REQUIRE(s.verdict != Verdict::NeedMore && s.verdict != Verdict::Ignore);
        if (flags & 0xFE) { REQUIRE(is(s, Verdict::Malformed, Fault::Flags)); continue; }
        if (type >= kInfo && type <= kTicket) { REQUIRE(is(s, Verdict::Malformed, Fault::Direction)); continue; }
        if (s.verdict == Verdict::Deliver) {
          REQUIRE(flags == 0 && (type == kCommand || type == kBinary));
          REQUIRE(type == kCommand ? len >= 1 && len <= kCommandMax : len >= kBinaryMin && len <= kBinaryMax);
        } else if (s.verdict == Verdict::Skip) {
          REQUIRE(flags == 0 && type != kCommand && type != kBinary && len <= kUnknownMax);
        } else {
          REQUIRE(s.fault == (flags ? Fault::More : Fault::Length));
        }
      }
    }
  }
  REQUIRE(unknownTypes == 246);
}

// Every prefix of one record: NeedMore until `decideAt` bytes, then `final` for good.
static void prefixes(const Bytes& record, size_t decideAt, Verdict final, Fault fault = Fault::None) {
  Bytes stream = record;
  stream.insert(stream.end(), {0x81, 0x00, 0x01, 0x00, 'x'});  // a following record never changes the verdict
  for (size_t have = 0; have <= stream.size(); ++have) {
    const Scan s = scanAt(Side::P4, stream.data(), have);
    if (have < decideAt) {
      REQUIRE(is(s, Verdict::NeedMore));
      REQUIRE(s.size == (have < kHeader ? kHeader : record.size()) && s.size > have);
    } else {
      REQUIRE(is(s, final, fault));
      REQUIRE(s.size == record.size());
    }
  }
}

static Bytes randomIngress(Rng& rng, uint8_t* typeOut) {
  static const uint8_t unknown[] = {0x00, 0x08, 0x40, 0x7F, 0x80, 0x84, 0x90, 0xC3, 0xFE, 0xFF};
  const uint32_t pick = rng.below(10);
  Bytes b;
  if (pick < 5) {
    b = rec(kCommand, 0, 1 + rng.below(kCommandMax));
  } else if (pick < 8) {
    b = rec(kBinary, 0, kBinaryMin + rng.below(kBinaryMax - kBinaryMin + 1));
  } else if (pick < 9) {
    b = rec(kResume, 0, rng.below(kUnknownMax + 1));
  } else {
    b = rec(unknown[rng.below(sizeof(unknown))], 0, rng.below(kUnknownMax + 1));
  }
  for (size_t i = kHeader + (b[0] == kBinary ? sizeof(kOtaMagic) : 0); i < b.size(); ++i) b[i] = uint8_t(rng.next());
  *typeOut = b[0];
  return b;
}

static void incremental() {
  prefixes(rec(kCommand, 0, 1), 5, Verdict::Deliver);
  prefixes(rec(kCommand, 0, 511), 515, Verdict::Deliver);
  prefixes(rec(kBinary, 0, 13), 17, Verdict::Deliver);
  prefixes(rec(kBinary, 0, 489), 493, Verdict::Deliver);
  prefixes(rec(kResume, 0, 16), 20, Verdict::Skip);
  prefixes(rec(0x90, 0, 0), 4, Verdict::Skip);
  prefixes(rec(0x90, 0, 1024), 1028, Verdict::Skip);
  // Header faults show with the fourth byte; a bad magic byte as soon as it is there.
  prefixes(rec(kCommand, 0x02, 5), 4, Verdict::Malformed, Fault::Flags);
  prefixes(rec(kText, 0, 5), 4, Verdict::Malformed, Fault::Direction);
  prefixes(rec(kCommand, 0, 600), 4, Verdict::Malformed, Fault::Length);
  prefixes(rec(kCommand, kMore, 5), 4, Verdict::Malformed, Fault::More);
  for (size_t i = 0; i < sizeof(kOtaMagic); ++i) {
    Bytes b = rec(kBinary, 0, 40);
    b[kHeader + i] ^= 0x20;
    prefixes(b, kHeader + i + 1, Verdict::Malformed, Fault::Magic);
  }
  // A long stream split anywhere: the consumer sees exactly the records sent.
  for (uint64_t seed = 1; seed <= 12; ++seed) {
    Rng rng{seed * 0x9E3779B97F4A7C15ull};
    const size_t count = seed <= 2 ? 200 : 1500;
    Bytes stream;
    std::vector<std::pair<size_t, size_t>> records;  // offset, size
    for (size_t i = 0; i < count; ++i) {
      uint8_t type;
      const Bytes b = randomIngress(rng, &type);
      records.push_back({stream.size(), b.size()});
      stream.insert(stream.end(), b.begin(), b.end());
    }
    Bytes rx;
    size_t fed = 0, next = 0, delivered = 0, skipped = 0;
    while (fed < stream.size()) {
      size_t chunk = seed <= 2 ? 1 : 1 + (rng.below(4) == 0 ? rng.below(1600) : rng.below(24));
      chunk = std::min(chunk, stream.size() - fed);
      rx.insert(rx.end(), stream.begin() + long(fed), stream.begin() + long(fed + chunk));
      fed += chunk;
      for (;;) {
        const Scan s = scanAt(Side::P4, rx);
        if (s.verdict == Verdict::NeedMore) { REQUIRE(s.size > rx.size()); break; }
        REQUIRE(s.verdict == Verdict::Deliver || s.verdict == Verdict::Skip);
        REQUIRE(next < records.size() && s.size == records[next].second);
        REQUIRE(std::equal(rx.begin(), rx.begin() + long(s.size), stream.begin() + long(records[next].first)));
        const uint8_t type = stream[records[next].first];
        REQUIRE((s.verdict == Verdict::Deliver) == (type == kCommand || type == kBinary));
        (s.verdict == Verdict::Deliver ? delivered : skipped)++;
        rx.erase(rx.begin(), rx.begin() + long(s.size));
        ++next;
      }
    }
    REQUIRE(next == records.size() && rx.empty() && delivered > 0 && skipped > 0);
  }
  // A malformed record in the middle: everything before it is delivered, and the
  // verdict shows exactly when its offending byte arrives.
  for (uint64_t seed = 1; seed <= 200; ++seed) {
    Rng rng{seed * 0x2545F4914F6CDD1Dull};
    Bytes stream;
    const size_t before = rng.below(30);
    for (size_t i = 0; i < before; ++i) { uint8_t type; const Bytes b = randomIngress(rng, &type); stream.insert(stream.end(), b.begin(), b.end()); }
    const size_t badAt = stream.size();
    Bytes bad;
    size_t offending = kHeader;  // bytes of the bad record needed to see the fault
    switch (seed % 6) {
      case 0: bad = rec(kCommand, 0x04, 8); break;
      case 1: bad = rec(kMirror, 0, 8); break;
      case 2: bad = rec(kCommand, 0, 512); break;
      case 3: bad = rec(0x90, kMore, 8); break;
      case 4: bad = rec(kResume, 0, 2000); break;
      default: { const size_t i = rng.below(8); bad = rec(kBinary, 0, 100); bad[kHeader + i] ^= 0x10; offending = kHeader + i + 1; } break;
    }
    stream.insert(stream.end(), bad.begin(), bad.end());
    for (int i = 0; i < 3; ++i) { uint8_t type; const Bytes b = randomIngress(rng, &type); stream.insert(stream.end(), b.begin(), b.end()); }
    Bytes rx;
    size_t fed = 0, consumed = 0;
    bool malformed = false;
    while (!malformed && fed < stream.size()) {
      const size_t chunk = std::min<size_t>(1 + rng.below(seed % 2 ? 3 : 700), stream.size() - fed);
      rx.insert(rx.end(), stream.begin() + long(fed), stream.begin() + long(fed + chunk));
      fed += chunk;
      for (;;) {
        const Scan s = scanAt(Side::P4, rx);
        if (s.verdict == Verdict::NeedMore) {
          REQUIRE(consumed < badAt || rx.size() < offending);
          break;
        }
        if (s.verdict == Verdict::Malformed) {
          REQUIRE(consumed == badAt && rx.size() >= offending);
          malformed = true;
          break;
        }
        REQUIRE(consumed + s.size <= badAt);
        consumed += s.size;
        rx.erase(rx.begin(), rx.begin() + long(s.size));
      }
    }
    REQUIRE(malformed);
  }
}

static void egress() {
  Egress e;
  REQUIRE(!e.chainOpen() && e.chainType() == 0 && e.chainTotal() == 0);
  REQUIRE(e.check(kText, 0, 1) == Fault::None && e.commit(kText, 0, 1) && !e.chainOpen());
  REQUIRE(e.check(kAbort, 0, 0) == Fault::StrayAbort && !e.commit(kAbort, 0, 0));
  // Exactly 4608 in five records; then the chain is closed.
  for (int i = 0; i < 4; ++i) {
    REQUIRE(e.commit(kText, kMore, 1024) && e.chainOpen() && e.chainType() == kText && e.chainTotal() == size_t(1024 * (i + 1)));
  }
  REQUIRE(e.check(kText, 0, 513) == Fault::Cap && e.check(kText, kMore, 512) == Fault::Cap);
  REQUIRE(e.check(kText, kMore, 511) == Fault::None && e.check(kText, 0, 512) == Fault::None);
  REQUIRE(e.commit(kText, 0, 512) && !e.chainOpen() && e.chainTotal() == 0);
  // 4607 with MORE leaves room for one byte; MORE on the record reaching 4608 does not.
  for (int i = 0; i < 4; ++i) REQUIRE(e.commit(kBlob, kMore, 1024));
  REQUIRE(e.commit(kBlob, kMore, 511) && e.chainTotal() == 4607);
  REQUIRE(e.check(kBlob, 0, 2) == Fault::Cap && e.check(kBlob, kMore, 1) == Fault::Cap);
  REQUIRE(e.commit(kBlob, 0, 1) && !e.chainOpen());
  // Inside a chain: only the same type or ABORT.
  REQUIRE(e.commit(kText, kMore, 100));
  for (uint8_t other : {kBlob, kCard, kMirror}) REQUIRE(e.check(other, 0, 10) == Fault::Interleave);
  REQUIRE(e.check(kBlob, kMore, 10) == Fault::Interleave && !e.commit(kCard, 0, 10) && e.chainTotal() == 100);
  REQUIRE(e.check(kAbort, 0, 0) == Fault::None && e.commit(kAbort, 0, 0) && !e.chainOpen() && e.chainTotal() == 0);
  REQUIRE(e.commit(kCard, 0, 512) && e.commit(kMirror, 0, 512) && !e.chainOpen());
  // What the P4 never sends this way.
  for (unsigned t = 0; t < 256; ++t) {
    const uint8_t type = uint8_t(t);
    if (type == kText || type == kBlob || type == kCard || type == kMirror || type == kAbort) continue;
    REQUIRE(e.check(type, 0, 1) == Fault::Type && e.check(type, 0, 0) == Fault::Type);
  }
  // Per-record bounds and flags.
  REQUIRE(e.check(kText, 0, 0) == Fault::Length && e.check(kText, 0, 1025) == Fault::Length && e.check(kText, 0, 1024) == Fault::None);
  REQUIRE(e.check(kCard, 0, 0) == Fault::Length && e.check(kCard, 0, 513) == Fault::Length && e.check(kCard, kMore, 10) == Fault::More);
  REQUIRE(e.check(kMirror, 0, 0) == Fault::Length && e.check(kMirror, 0, 513) == Fault::Length && e.check(kMirror, kMore, 10) == Fault::More);
  REQUIRE(e.check(kText, 0, 70000) == Fault::Length && e.check(kText, 0x02, 10) == Fault::Flags && e.check(kCard, 0x81, 10) == Fault::Flags);
  REQUIRE(e.commit(kText, kMore, 10));
  REQUIRE(e.check(kAbort, 0, 1) == Fault::Length && e.check(kAbort, kMore, 0) == Fault::More && e.check(kAbort, 0x40, 0) == Fault::Flags);
  REQUIRE(e.check(kInfo, 0, 100) == Fault::Type && e.chainTotal() == 10);
  e.reset();
  REQUIRE(!e.chainOpen() && e.chainTotal() == 0);
  // The writer's slicing fills a chain of any size up to the cap and no further.
  for (size_t total : {size_t(1), size_t(511), size_t(1023), size_t(1024), size_t(1025), size_t(2048), size_t(4095), size_t(4096), size_t(4097), size_t(4607), size_t(4608), size_t(4609), size_t(6000)}) {
    for (uint8_t type : {kText, kBlob}) {
      Egress w;
      size_t remaining = total, sent = 0, records = 0;
      Fault fault = Fault::None;
      while (remaining) {
        const uint16_t len = sliceLen(remaining);
        const uint8_t flags = sliceFlags(remaining);
        REQUIRE(len >= 1 && len <= kTextMax && ((flags == kMore) == (remaining > len)));
        fault = w.check(type, flags, len);
        if (fault != Fault::None) break;
        REQUIRE(w.commit(type, flags, len));
        remaining -= len; sent += len; ++records;
      }
      REQUIRE(chainFits(total) == (total <= kChainMax));
      if (chainFits(total)) REQUIRE(fault == Fault::None && sent == total && records == (total + kTextMax - 1) / kTextMax && !w.chainOpen());
      else REQUIRE(fault == Fault::Cap && w.chainOpen() && w.check(kAbort, 0, 0) == Fault::None);
    }
  }
  REQUIRE(!chainFits(0));
}

static Bytes infoRecord(const char* fw = "1.0", const char* model = "Board") {
  char info[kInfoMax + 1];
  const size_t n = buildInfo(info, sizeof(info), fw, model);
  REQUIRE(n >= 1 && n <= kInfoMax);
  Bytes b(kHeader + n);
  encodeHeader(b.data(), kInfo, 0, uint16_t(n));
  std::memcpy(b.data() + kHeader, info, n);
  return b;
}

static void receiver() {
  PhoneReceiver r;
  REQUIRE(!r.infoSeen() && !r.failed());
  REQUIRE(is(nextAt(r, rec(kText, 0, 2)), Verdict::Malformed, Fault::InfoOrder) && r.failed());
  REQUIRE(is(nextAt(r, infoRecord()), Verdict::Malformed, Fault::InfoOrder));  // latched
  r.reset();
  REQUIRE(!r.failed() && is(nextAt(r, infoRecord()), Verdict::Deliver) && r.infoSeen());
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, infoRecord()), Verdict::Malformed, Fault::InfoOrder)); }
  // Phone -> P4 types here are malformed; TICKET (reserved) and unknown types are skipped.
  for (uint8_t type : {kCommand, kBinary, kResume}) { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(type, 0, 16)), Verdict::Malformed, Fault::Direction)); }
  REQUIRE(is(nextAt(r, rec(kTicket, 0, 18)), Verdict::Skip) && is(nextAt(r, rec(0x08, 0, 0)), Verdict::Skip) && is(nextAt(r, rec(0xF0, 0, 1024)), Verdict::Skip));
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(0x08, kMore, 3)), Verdict::Malformed, Fault::More)); }
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(kTicket, 0, 1025)), Verdict::Malformed, Fault::Length)); }
  // A stray ABORT is ignored; a CARD and MIRROR stand alone.
  REQUIRE(is(nextAt(r, rec(kAbort, 0, 0)), Verdict::Ignore) && !r.chainOpen() && !r.failed());
  REQUIRE(is(nextAt(r, rec(kCard, 0, 512)), Verdict::Deliver) && is(nextAt(r, rec(kMirror, 0, 1)), Verdict::Deliver));
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(kCard, 0, 513)), Verdict::Malformed, Fault::Length)); }
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(kInfo, 0, 257)), Verdict::Malformed, Fault::Length)); }
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(kAbort, 0, 1)), Verdict::Malformed, Fault::Length)); }
  // A one-record message and a chain of exactly 4608 B.
  Scan s = nextAt(r, rec(kText, 0, 7));
  REQUIRE(is(s, Verdict::Deliver) && s.chainEnd && s.chainTotal == 7 && !r.chainOpen());
  for (int i = 0; i < 4; ++i) {
    s = nextAt(r, rec(kBlob, kMore, 1024));
    REQUIRE(is(s, Verdict::Deliver) && !s.chainEnd && s.chainTotal == size_t(1024 * (i + 1)) && r.chainOpen() && r.chainType() == kBlob);
  }
  REQUIRE(is(nextAt(r, rec(0x55, 0, 9)), Verdict::Skip) && r.chainTotal() == 4096);  // unknown inside a chain: skipped
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(kBlob, 0, 513)), Verdict::Malformed, Fault::Cap)); }
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(kBlob, kMore, 512)), Verdict::Malformed, Fault::Cap)); }
  for (uint8_t other : {kText, kCard, kMirror}) { PhoneReceiver p = r; REQUIRE(is(nextAt(p, rec(other, 0, 5)), Verdict::Malformed, Fault::Interleave)); }
  { PhoneReceiver p = r; REQUIRE(is(nextAt(p, infoRecord()), Verdict::Malformed, Fault::InfoOrder)); }
  { PhoneReceiver p = r; s = nextAt(p, rec(kAbort, 0, 0)); REQUIRE(is(s, Verdict::Deliver) && s.chainEnd && s.chainTotal == 4096 && !p.chainOpen()); }
  s = nextAt(r, rec(kBlob, 0, 512));
  REQUIRE(is(s, Verdict::Deliver) && s.chainEnd && s.chainTotal == 4608 && !r.chainOpen());
  // Chain and INFO faults show with the header, before the payload is there; a
  // whole record is needed to deliver, and NeedMore never moves the state.
  REQUIRE(is(nextAt(r, rec(kText, kMore, 100)), Verdict::Deliver));
  {
    PhoneReceiver p = r;
    const Bytes card = rec(kCard, 0, 300);
    for (size_t have = 0; have < kHeader; ++have) REQUIRE(is(nextAt(p, card.data(), have), Verdict::NeedMore) && !p.failed());
    REQUIRE(is(nextAt(p, card.data(), kHeader), Verdict::Malformed, Fault::Interleave));
  }
  {
    PhoneReceiver p = r;
    const Bytes more = rec(kText, kMore, 1000);
    for (size_t have = 0; have < more.size(); ++have) {
      s = nextAt(p, more.data(), have);
      REQUIRE(is(s, Verdict::NeedMore) && s.size == (have < kHeader ? kHeader : more.size()) && p.chainTotal() == 100);
    }
    s = nextAt(p, more);
    REQUIRE(is(s, Verdict::Deliver) && s.chainTotal == 1100 && p.chainTotal() == 1100);
  }
  {
    PhoneReceiver fresh;
    REQUIRE(is(nextAt(fresh, rec(kMirror, 0, 300).data(), kHeader), Verdict::Malformed, Fault::InfoOrder));
  }
}

// The P4 egress machine and the phone receiver agree record by record: what the
// writer may send is delivered, and nothing it refuses is ever delivered.
static void mirror() {
  static const uint8_t types[] = {kText, kText, kText, kBlob, kBlob, kBlob, kCard, kMirror, kAbort, kAbort,
                                  kInfo, kTicket, kCommand, kBinary, kResume, 0x00, 0x08, 0x7F, 0x80, 0x84, 0xFF};
  static const uint8_t flagSet[] = {0, 0, 0, kMore, kMore, kMore, 0x02, 0x81};
  unsigned long accepted = 0, refused = 0, chains = 0, aborts = 0;
  for (uint64_t seed = 1; seed <= 300; ++seed) {
    Rng rng{seed * 0xD1B54A32D192ED03ull};
    Egress e;
    PhoneReceiver r;
    Bytes stream = infoRecord("0.99.98.4", "Espressif ESP32-P4X-EYE");
    REQUIRE(is(nextAt(r, stream), Verdict::Deliver));
    std::vector<Verdict> verdicts{Verdict::Deliver};
    for (int step = 0; step < 300; ++step) {
      const uint8_t type = types[rng.below(sizeof(types))];
      const uint8_t flags = flagSet[rng.below(sizeof(flagSet))];
      const size_t room = kChainMax - e.chainTotal();
      size_t len;
      switch (rng.below(6)) {
        case 0: len = rng.below(3); break;
        case 1: { static const size_t edge[] = {16, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025}; len = edge[rng.below(10)]; } break;
        case 2: len = room + rng.below(3) - (room ? 1 : 0); break;
        default: len = 1 + rng.below(1100); break;
      }
      const Fault f = e.check(type, flags, len);
      const Bytes record = rec(type, flags, len, uint8_t(rng.next()));
      PhoneReceiver probe = r;
      const Scan s = nextAt(probe, record);
      if (f == Fault::None) {
        REQUIRE(is(s, Verdict::Deliver) && s.size == record.size());
        REQUIRE(e.commit(type, flags, len));
        r = probe;
        REQUIRE(e.chainOpen() == r.chainOpen() && e.chainType() == r.chainType() && e.chainTotal() == r.chainTotal());
        if (s.chainEnd) (type == kAbort ? aborts : chains)++;
        stream.insert(stream.end(), record.begin(), record.end());
        verdicts.push_back(s.verdict);
        ++accepted;
      } else {
        REQUIRE(s.verdict != Verdict::Deliver);
        REQUIRE(f != Fault::StrayAbort || s.verdict == Verdict::Ignore);
        REQUIRE(f != Fault::Interleave || s.fault == Fault::Interleave);
        REQUIRE(f != Fault::Cap || s.fault == Fault::Cap);
        ++refused;
      }
    }
    // The same stream, split anywhere, reads back the same records.
    PhoneReceiver replay;
    Bytes rx;
    size_t fed = 0, index = 0;
    while (fed < stream.size()) {
      const size_t chunk = std::min<size_t>(1 + rng.below(seed % 3 ? 40 : 1500), stream.size() - fed);
      rx.insert(rx.end(), stream.begin() + long(fed), stream.begin() + long(fed + chunk));
      fed += chunk;
      for (;;) {
        const Scan s = nextAt(replay, rx);
        if (s.verdict == Verdict::NeedMore) break;
        REQUIRE(index < verdicts.size() && s.verdict == verdicts[index++]);
        rx.erase(rx.begin(), rx.begin() + long(s.size));
      }
    }
    REQUIRE(index == verdicts.size() && rx.empty() && !replay.failed());
  }
  std::printf("mirror accepted=%lu refused=%lu chains=%lu aborts=%lu\n", accepted, refused, chains, aborts);
  REQUIRE(accepted > 1000 && refused > 1000 && chains > 100 && aborts > 10);
}

static std::string info(const char* fw, const char* model) {
  char out[kInfoMax + 1];
  std::memset(out, 'Q', sizeof(out));
  const size_t n = buildInfo(out, sizeof(out), fw, model);
  REQUIRE(n >= 1 && n <= kInfoMax && out[n] == '\0' && std::strlen(out) == n);
  std::printf("INFO ");
  for (size_t i = 0; i < n; ++i) std::printf("%02x", static_cast<unsigned char>(out[i]));
  std::printf("\n");
  return std::string(out, n);
}

static void infoPayload() {
  REQUIRE(info("0.99.98.4", "Espressif ESP32-P4X-EYE") ==
          "{\"v\":1,\"fw\":\"0.99.98.4\",\"model\":\"Espressif ESP32-P4X-EYE\",\"line\":511,\"bin\":489,\"auth\":true,\"resume\":false}");
  REQUIRE(info("", "x") == "{\"v\":1,\"fw\":\"\",\"model\":\"x\",\"line\":511,\"bin\":489,\"auth\":true,\"resume\":false}");
  REQUIRE(info("", "x").size() == 75);  // the fixed part is 74 bytes
  REQUIRE(info(nullptr, nullptr) == "{\"v\":1,\"fw\":\"\",\"model\":\"HardwareOne\",\"line\":511,\"bin\":489,\"auth\":true,\"resume\":false}");
  REQUIRE(info("1", "") == info("1", "HardwareOne"));
  // Escapes: quote and backslash, control bytes and DEL as \u00XX, bytes >= 0x80 as '?'.
  REQUIRE(info("a\"b\\c\x01\x1f\x7f\xc3\xa9", "tab\there\n") ==
          "{\"v\":1,\"fw\":\"a\\\"b\\\\c\\u0001\\u001f\\u007f??\",\"model\":\"tab\\u0009here\\u000a\","
          "\"line\":511,\"bin\":489,\"auth\":true,\"resume\":false}");
  // Each string stops at 64 escaped bytes and never splits an escape.
  const std::string a100(100, 'a'), a63(63, 'a'), a62(62, 'a'), a59(59, 'a'), a58(58, 'a');
  auto fwOf = [](const std::string& payload) {
    const size_t start = payload.find("\"fw\":\"") + 6, end = payload.find("\",\"model\"");
    return payload.substr(start, end - start);
  };
  REQUIRE(fwOf(info(a100.c_str(), "m")) == std::string(64, 'a'));
  REQUIRE(fwOf(info((a63 + "\"").c_str(), "m")) == a63);
  REQUIRE(fwOf(info((a62 + "\"").c_str(), "m")) == a62 + "\\\"");
  REQUIRE(fwOf(info((a59 + "\x01").c_str(), "m")) == a59);
  REQUIRE(fwOf(info((a58 + "\x01").c_str(), "m")) == a58 + "\\u0001");
  // The worst case still fits the INFO record.
  const std::string quotes(200, '"'), controls(200, '\x02');
  const std::string worst = info(quotes.c_str(), controls.c_str());
  REQUIRE(worst.size() == 74 + 64 + 60 && worst.size() <= kInfoMax);
  // Too small a buffer writes nothing but a NUL.
  char small[kInfoMax];
  std::memset(small, 'Q', sizeof(small));
  REQUIRE(buildInfo(small, sizeof(small), "1", "m") == 0 && small[0] == '\0' && small[1] == 'Q');
  REQUIRE(buildInfo(small, 0, "1", "m") == 0 && small[0] == '\0');
  // The record: INFO header, flags 0, and the phone takes it as the first record.
  const Bytes record = infoRecord("0.99.98.4", "Espressif ESP32-P4X-EYE");
  REQUIRE(record[0] == 0x01 && record[1] == 0x00 && size_t(record[2]) == record.size() - kHeader && record[3] == 0x00);
  PhoneReceiver r;
  REQUIRE(is(nextAt(r, record), Verdict::Deliver) && r.infoSeen());
}

static void names() {
  REQUIRE(std::string(typeName(kInfo)) == "INFO" && std::string(typeName(kText)) == "TEXT" && std::string(typeName(kBlob)) == "BLOB");
  REQUIRE(std::string(typeName(kCard)) == "CARD" && std::string(typeName(kMirror)) == "MIRROR" && std::string(typeName(kAbort)) == "ABORT");
  REQUIRE(std::string(typeName(kTicket)) == "TICKET" && std::string(typeName(kCommand)) == "COMMAND" && std::string(typeName(kBinary)) == "BINARY");
  REQUIRE(std::string(typeName(kResume)) == "RESUME" && std::string(typeName(0x90)) == "UNKNOWN");
  const char* faults[] = {"none", "flags", "more", "length", "direction", "magic", "type", "interleave", "cap", "stray-abort", "info-order"};
  for (unsigned f = 0; f <= unsigned(Fault::InfoOrder); ++f) REQUIRE(std::string(faultName(Fault(f))) == faults[f]);
  const char* verdicts[] = {"need-more", "deliver", "skip", "ignore", "malformed"};
  for (unsigned v = 0; v <= unsigned(Verdict::Malformed); ++v) REQUIRE(std::string(verdictName(Verdict(v))) == verdicts[v]);
  static_assert(std::is_trivially_copyable<Egress>::value && std::is_trivially_copyable<PhoneReceiver>::value, "plain state");
  static_assert(sizeof(Egress) <= 16 && sizeof(PhoneReceiver) <= 24 && sizeof(Scan) <= 32, "small state");
}

int main() {
  header();
  ingressBounds();
  incremental();
  egress();
  receiver();
  mirror();
  infoPayload();
  names();
  std::printf("console codec: %lu checks passed\n", checks);
  return 0;
}
