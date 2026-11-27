#pragma once
// ESP-NOW message history: the per-peer byte ring, without the radio, the
// registry, the lock or the allocator.
//
// Each peer that has a message owns one block with a received ring and a sent
// ring of the same size (System_ESPNow.h, PeerMessageHistory). A ring holds
// packed records: a 20-byte header followed by exactly `length` text bytes,
// padded to a multiple of 4. The record carries no MAC and no name; the ring
// belongs to one peer, and the name is looked up when a record is shown.
//
//   header  0 marker 0xA5   1 length   2 type   3 flags   4 piece   5 pieceTotal
//           6 check   7 status   8 seq (LE)   12 reqId (LE)   16 timestamp (LE)
//   flags   bit0 encrypted, bit1 sent, bit2 unattributed, bits3-5 sendState,
//           bits6-7 zero. sendState: a sent record's delivery state
//           (SendStatusState); a received remote-command result row's outcome
//           (espnowcmd::Outcome, 1.0 step 6); 0 otherwise.
//   status  a refused sent record's ACK status (sendState kRefusedState, 1-15:
//           the reason "Refused: <reason>" names); 0 on every other record.
//   check   0x5A ^ sum p[i]*(2i+1) mod 256 over bytes 0..19 except 6: odd
//           weights, so any single changed header byte is detected.
//
// New records go at the head. When one does not fit, whole oldest records are
// dropped at the tail. A record never splits across the end: the writer
// leaves a wrap marker (0x5A) where the upper segment ends, when there is a
// gap, and continues at offset 0. Records are dropped in seq order (FIFO), so
// a record with seq s is live in a ring iff oldestSeq <= s <= newestSeq and s
// was written there; that is what makes a reference safe without trusting
// its offset (findBySeq decides liveness before it looks at the hint).
//
// The RingState lives in the same memory as the records, so it is checked,
// never trusted: every operation starts with the O(1) stateSane(), and every
// walk validates each header (marker, check, zero bits, type, seq, direction,
// footprint inside its segment, ascending seq) before it uses it and stops at
// the first bad one. A writer that meets damage while evicting clears the
// ring; a reader reports it (`damaged`, Cursor::bad) and the store clears it.
//
// No allocation, no locking, no logging: the caller owns the storage and
// serialises the calls (System_ESPNow.cpp, under sPeerHistoryLock). The host
// test compiles this file as is (espnow_history_core_tests). Design:
// DESIGN.md sections 2-4 and 10.1 of the 2026-11-18 history work.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace espnowhist {

// ---- geometry ----------------------------------------------------------------
constexpr uint32_t kHeaderBytes    = 20;
constexpr uint32_t kTextMax        = 255;   // text bytes in one record (today's char[256] slot)
constexpr uint32_t kChunkText      = 200;   // piece size of the chunked stores
constexpr uint32_t kChunkPiecesMax = 255;   // a chunked store keeps at most this many pieces
constexpr uint32_t kAlign          = 4;
constexpr uint32_t footprint(uint32_t textLen) { return (kHeaderBytes + textLen + (kAlign - 1)) & ~(kAlign - 1); }
constexpr uint32_t kMaxFootprint   = footprint(kTextMax);   // 276
constexpr uint32_t kMinRingBytes   = 1024;
constexpr uint32_t kMaxRingBytes   = 65536;
constexpr uint8_t  kTypeMax        = 7;
constexpr uint8_t  kSendStateMax   = 7;
constexpr uint8_t  kRecordMarker   = 0xA5;
constexpr uint8_t  kWrapMarker     = 0x5A;
constexpr int      kMaxSources     = 32;    // 2 rings x MESH_PEER_MAX (asserted in System_ESPNow.h)
constexpr int      kGroupWindow    = 64;    // collapse: same-source entries a fragment looks back over
constexpr size_t   kNameCap        = 32;    // display name with NUL (the old senderName field)
constexpr bool     kHintFastPath   = true;  // 6.3
constexpr uint32_t kNoEntry        = 0xFFFFFFFFu;
static_assert(kMinRingBytes >= 2 * kMaxFootprint + kAlign, "a ring holds two largest records");
static_assert(kMinRingBytes % kAlign == 0 && kMaxRingBytes % kAlign == 0, "aligned ring sizes");
static_assert(kMaxRingBytes / kAlign + 1 <= 0xFFFF, "piece scratch stores offset/4+1 in uint16_t");
static_assert(kMaxRingBytes / kHeaderBytes <= 0xFFFF, "RingState::count and Cursor::left are uint16_t");
static_assert(kMaxFootprint == 276, "largest record");

// ---- header layout -----------------------------------------------------------
enum : uint32_t { kOffMarker = 0, kOffLength = 1, kOffType = 2, kOffFlags = 3, kOffPiece = 4,
                  kOffPieceTotal = 5, kOffCheck = 6, kOffZero = 7, kOffSeq = 8, kOffReqId = 12,
                  kOffTimestamp = 16 };
constexpr uint8_t kFlagEncrypted    = 0x01;
constexpr uint8_t kFlagSent         = 0x02;
constexpr uint8_t kFlagUnattributed = 0x04;
constexpr uint8_t kSendStateShift   = 3;
constexpr uint8_t kSendStateMask    = 0x38;
constexpr uint8_t kFlagsReserved    = 0xC0;
constexpr uint8_t kRefusedState     = 5;    // SEND_STATUS_REFUSED: the one state with a status byte
constexpr uint8_t kStatusMax        = 15;   // the shared ACK statuses (espnowanswer)

struct Header {
  uint32_t seq;
  uint32_t reqId;
  uint32_t timestamp;
  uint8_t  length;       // set by append from the text; ignored on input
  uint8_t  type;         // LogMessageType
  uint8_t  piece;
  uint8_t  pieceTotal;
  uint8_t  sendState;    // 0..7
  uint8_t  status;       // a refused sent record's ACK status (1-15), else 0
  bool     encrypted;
  bool     sent;         // set by append from the ring; ignored on input
  bool     unattributed;
};

namespace detail {
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace detail

// Decodes so far (tests, hold-time line). Touched only under the caller's lock.
inline uint32_t& decodeCount() {
  static uint32_t n = 0;
  return n;
}

// 0x5A ^ sum p[i]*(2i+1) over p[0..19] except p[6].
inline uint8_t headerCheck(const uint8_t* p) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < kHeaderBytes; ++i) {
    if (i == kOffCheck) continue;
    sum += static_cast<uint32_t>(p[i]) * (2 * i + 1);
  }
  return static_cast<uint8_t>(0x5A ^ (sum & 0xFF));
}

// Writes 20 bytes incl. the check. sendState is masked to its three bits.
inline void encodeHeader(const Header& h, uint8_t* p) {
  p[kOffMarker] = kRecordMarker;
  p[kOffLength] = h.length;
  p[kOffType] = h.type;
  p[kOffFlags] = static_cast<uint8_t>((h.encrypted ? kFlagEncrypted : 0) | (h.sent ? kFlagSent : 0) |
                                      (h.unattributed ? kFlagUnattributed : 0) |
                                      ((static_cast<uint32_t>(h.sendState) << kSendStateShift) & kSendStateMask));
  p[kOffPiece] = h.piece;
  p[kOffPieceTotal] = h.pieceTotal;
  // The status byte is kept only where it means something (a refused sent record).
  p[kOffZero] = (h.sent && h.sendState == kRefusedState && h.status <= kStatusMax) ? h.status : 0;
  detail::put32(p + kOffSeq, h.seq);
  detail::put32(p + kOffReqId, h.reqId);
  detail::put32(p + kOffTimestamp, h.timestamp);
  p[kOffCheck] = headerCheck(p);
}

// False unless marker, status byte (0, or 1-15 on a refused sent record), reserved bits, type <= kTypeMax,
// seq != 0 and check all hold.
// h is written only on success. Every call counts in decodeCount().
inline bool decodeHeader(const uint8_t* p, Header& h) {
  ++decodeCount();
  if (p[kOffMarker] != kRecordMarker) return false;
  const uint8_t flags = p[kOffFlags];
  if (flags & kFlagsReserved) return false;
  // Byte 7 is 0, except the status (1-15) of a refused sent record.
  const uint8_t st = static_cast<uint8_t>((flags & kSendStateMask) >> kSendStateShift);
  if (p[kOffZero] != 0 &&
      !((flags & kFlagSent) && st == kRefusedState && p[kOffZero] <= kStatusMax)) return false;
  if (p[kOffType] > kTypeMax) return false;
  const uint32_t seq = detail::get32(p + kOffSeq);
  if (seq == 0) return false;
  if (p[kOffCheck] != headerCheck(p)) return false;
  h.seq = seq;
  h.reqId = detail::get32(p + kOffReqId);
  h.timestamp = detail::get32(p + kOffTimestamp);
  h.length = p[kOffLength];
  h.type = p[kOffType];
  h.piece = p[kOffPiece];
  h.pieceTotal = p[kOffPieceTotal];
  h.sendState = st;
  h.status = p[kOffZero];
  h.encrypted = (flags & kFlagEncrypted) != 0;
  h.sent = (flags & kFlagSent) != 0;
  h.unattributed = (flags & kFlagUnattributed) != 0;
  return true;
}

// ---- budget, attribution, names ----------------------------------------------
struct Budget { uint32_t ringBytes; bool clampedToBoardMax; bool loweredForTotalCap; };

// kb 0 -> kbDefault; clamp [1, kbMax]; x1024; if capacity > 0: min(.., floor4(totalCapBytes / (2*capacity)));
// clamp [kMinRingBytes, kMaxRingBytes]; result is a multiple of 4.
inline Budget ringBudget(uint32_t kbSetting, uint32_t kbDefault, uint32_t kbMax, uint32_t totalCapBytes, int capacity) {
  Budget b{0, false, false};
  uint64_t kb = kbSetting == 0 ? kbDefault : kbSetting;
  if (kb > kbMax) {
    kb = kbMax;
    b.clampedToBoardMax = true;
  }
  if (kb < 1) kb = 1;
  uint64_t bytes = kb * 1024u;
  if (capacity > 0) {
    const uint64_t share = (static_cast<uint64_t>(totalCapBytes) / (2u * static_cast<uint64_t>(capacity))) &
                           ~static_cast<uint64_t>(kAlign - 1);
    if (bytes > share) {
      bytes = share;
      b.loweredForTotalCap = true;
    }
  }
  if (bytes < kMinRingBytes) bytes = kMinRingBytes;
  if (bytes > kMaxRingBytes) bytes = kMaxRingBytes;
  b.ringBytes = static_cast<uint32_t>(bytes) & ~(kAlign - 1);
  return b;
}

// True when the given name proves nothing: null, empty, or equal to macText.
inline bool unattributedName(const char* givenName, const char* macText) {
  if (!givenName || givenName[0] == '\0') return true;
  return macText && std::strcmp(givenName, macText) == 0;
}

// Copies at most min(cap-1, srcCap) bytes of src, stopping at its NUL; always writes a NUL; src need not be
// terminated within srcCap. out == nullptr or cap == 0 -> returns 0, writes nothing. src == nullptr -> "".
inline size_t boundedCopy(char* out, size_t cap, const char* src, size_t srcCap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  if (src) {
    const size_t limit = (cap - 1) < srcCap ? (cap - 1) : srcCap;
    while (n < limit && src[n] != '\0') {
      out[n] = src[n];
      ++n;
    }
  }
  out[n] = '\0';
  return n;
}

// sent -> ""; unattributed -> macText; else currentName (read at most currentCap bytes) if non-empty, else macText.
// out == nullptr or cap == 0 -> nothing written. Otherwise always NUL-terminated.
inline void displayName(bool sent, bool unattributed, const char* currentName, size_t currentCap,
                        const char* macText, char* out, size_t cap) {
  if (!out || cap == 0) return;
  if (sent) {
    out[0] = '\0';
    return;
  }
  if (!unattributed && currentName && currentCap > 0 && currentName[0] != '\0') {
    boundedCopy(out, cap, currentName, currentCap);
    return;
  }
  boundedCopy(out, cap, macText, cap);  // macText is NUL-terminated; at most cap-1 bytes are read
}

// ---- ring ----------------------------------------------------------------------
struct RingState {
  uint32_t size, tail, head, wrapAt, oldestSeq, newestSeq, used, evicted, resets;
  uint16_t count;
  uint8_t  wrapped;
  uint8_t  sent;
};
static_assert(sizeof(RingState) == 40, "RingState layout");

// O(1): I1-I3, I4/I5 orderings, used/count arithmetic (DESIGN 3.2). Reads no record byte.
inline bool stateSane(const RingState& r) {
  if (r.size % kAlign != 0 || r.size < kMinRingBytes || r.size > kMaxRingBytes) return false;
  if (r.sent > 1 || r.wrapped > 1) return false;
  if (r.tail % kAlign != 0 || r.head % kAlign != 0 || r.wrapAt % kAlign != 0) return false;
  if (r.tail > r.size || r.head > r.size || r.wrapAt > r.size) return false;
  if (r.count == 0) {
    return r.tail == 0 && r.head == 0 && r.wrapped == 0 && r.wrapAt == r.size && r.used == 0 &&
           r.oldestSeq == 0 && r.newestSeq == 0;
  }
  if (r.count > r.size / kHeaderBytes) return false;
  if (r.wrapped == 0) {
    if (!(r.tail < r.head) || r.wrapAt != r.size || r.used != r.head - r.tail) return false;
  } else {
    if (!(0 < r.head && r.head <= r.tail && r.tail < r.wrapAt)) return false;
    if (r.used != (r.wrapAt - r.tail) + r.head) return false;
  }
  const uint32_t n = r.count;
  if (r.used < n * kHeaderBytes || r.used > n * kMaxFootprint) return false;
  if (r.oldestSeq == 0 || r.oldestSeq > r.newestSeq) return false;
  if (n == 1 && r.oldestSeq != r.newestSeq) return false;
  if (r.newestSeq - r.oldestSeq < n - 1) return false;  // n strictly ascending seqs
  return true;
}

namespace detail {
inline void normaliseEmpty(RingState& r) {
  r.tail = 0;
  r.head = 0;
  r.wrapAt = r.size;
  r.wrapped = 0;
  r.used = 0;
  r.count = 0;
  r.oldestSeq = 0;
  r.newestSeq = 0;
}
}  // namespace detail

// Empty normal form; false (r zeroed, size 0) unless size meets I1.
inline bool ringInit(RingState& r, uint32_t size, bool sent) {
  r = RingState{};
  if (size % kAlign != 0 || size < kMinRingBytes || size > kMaxRingBytes) return false;
  r.size = size;
  r.wrapAt = size;
  r.sent = sent ? 1 : 0;
  return true;
}

// Empty normal form; keeps size, sent, evicted; resets++. Only on a state whose size passed stateSane.
inline void ringReset(RingState& r) {
  const uint32_t size = r.size, evicted = r.evicted, resets = r.resets + 1;
  const uint8_t sent = r.sent ? 1 : 0;
  r = RingState{};
  r.size = size;
  r.evicted = evicted;
  r.resets = resets;
  r.sent = sent;
  detail::normaliseEmpty(r);
}

namespace detail {
// A record at pos inside the segment [.., segLimit): bounds are checked before any byte is read, then the header is
// decoded, its footprint must lie inside the segment and its direction must be the ring's.
inline bool recordAt(const RingState& r, const uint8_t* buf, uint32_t pos, uint32_t segLimit, Header& h) {
  if (!buf || pos % kAlign != 0 || segLimit > r.size || segLimit < kHeaderBytes || pos > segLimit - kHeaderBytes)
    return false;
  if (!decodeHeader(buf + pos, h)) return false;
  if (footprint(h.length) > segLimit - pos) return false;
  if (h.sent != (r.sent != 0)) return false;
  return true;
}

// Drops the record at tail (DESIGN 3.4). Any failed check clears the ring (reset = true). Always makes progress.
inline void evictOldest(RingState& r, uint8_t* buf, bool& reset) {
  Header h;
  const uint32_t seg = r.wrapped ? r.wrapAt : r.head;
  if (r.count == 0 || !recordAt(r, buf, r.tail, seg, h) || h.seq != r.oldestSeq) {
    ringReset(r);
    reset = true;
    return;
  }
  const uint32_t f = footprint(h.length);
  if (r.used < f) {
    ringReset(r);
    reset = true;
    return;
  }
  const uint32_t gone = h.seq;
  buf[r.tail] = 0;  // kill the marker: a stale pointer to this place can never validate
  r.tail += f;
  r.used -= f;
  r.count--;
  r.evicted++;
  if (r.wrapped && r.tail == r.wrapAt) {
    r.tail = 0;
    r.wrapped = 0;
    r.wrapAt = r.size;
  }
  if (r.count == 0) {
    if (r.tail == r.head && r.used == 0) {
      normaliseEmpty(r);
    } else {
      ringReset(r);
      reset = true;
    }
    return;
  }
  Header n;
  const uint32_t seg2 = r.wrapped ? r.wrapAt : r.head;
  if (!recordAt(r, buf, r.tail, seg2, n) || n.seq <= gone || n.seq > r.newestSeq ||
      (r.count == 1 && n.seq != r.newestSeq)) {
    ringReset(r);
    reset = true;
    return;
  }
  r.oldestSeq = n.seq;
  if (!stateSane(r)) {
    ringReset(r);
    reset = true;
  }
}
}  // namespace detail

struct AppendResult {
  bool     ok;
  bool     insane;      // stateSane failed on entry; nothing touched (the store resets the ring, 3.10)
  bool     truncated;   // text was longer than kTextMax
  bool     reset;       // a damaged record was met while evicting; the ring was cleared first
  uint16_t evicted;     // records dropped to make room
  uint32_t offset;      // where the record starts
};

// Appends one record (DESIGN 3.3). Refused, ring unchanged: insane state, null buf, seq 0, seq not above newestSeq,
// type > kTypeMax, sendState > kSendStateMax, null text with textLen > 0. Text beyond kTextMax is cut (truncated).
// The new record is always kept: whole oldest records are dropped until it fits.
inline AppendResult append(RingState& r, uint8_t* buf, const Header& h, const char* text, size_t textLen) {
  AppendResult res{false, false, false, false, 0, 0};
  if (!stateSane(r)) {
    res.insane = true;
    return res;
  }
  if (!buf || h.seq == 0 || (r.count > 0 && h.seq <= r.newestSeq) || h.type > kTypeMax ||
      h.sendState > kSendStateMax || (!text && textLen > 0))
    return res;
  const uint32_t len = textLen > kTextMax ? kTextMax : static_cast<uint32_t>(textLen);
  res.truncated = textLen > kTextMax;
  const uint32_t f = footprint(len);
  uint32_t pos = 0;
  for (;;) {
    if (r.count == 0) {
      detail::normaliseEmpty(r);
      pos = 0;
      break;
    }
    if (r.wrapped == 0) {
      if (r.size - r.head >= f) {
        pos = r.head;
        break;
      }
      if (r.tail >= f) {
        if (r.head < r.size) buf[r.head] = kWrapMarker;
        r.wrapAt = r.head;
        r.wrapped = 1;
        pos = 0;
        break;
      }
    } else if (r.tail - r.head >= f) {
      pos = r.head;
      break;
    }
    bool wasReset = false;
    detail::evictOldest(r, buf, wasReset);
    if (wasReset) {
      res.reset = true;
    } else if (res.evicted < 0xFFFF) {
      res.evicted++;
    }
  }
  Header w = h;
  w.length = static_cast<uint8_t>(len);
  w.sent = r.sent != 0;
  uint8_t* p = buf + pos;
  encodeHeader(w, p);
  if (len) std::memcpy(p + kHeaderBytes, text, len);
  for (uint32_t i = kHeaderBytes + len; i < f; ++i) p[i] = 0;
  r.head = pos + f;
  r.count++;
  r.used += f;
  r.newestSeq = h.seq;
  if (r.count == 1) r.oldestSeq = h.seq;
  res.ok = true;
  res.offset = pos;
  return res;
}

// ---- iteration: the one validated walk every reader uses (DESIGN 3.5) -------------------
struct Cursor { uint32_t pos; uint32_t lastSeq; uint16_t left; uint8_t upper; uint8_t bad; };

// bad = 1 when !stateSane(r)
inline Cursor begin(const RingState& r) {
  Cursor c{0, 0, 0, 0, 0};
  if (!stateSane(r)) {
    c.bad = 1;
    return c;
  }
  c.pos = r.tail;
  c.left = r.count;
  c.upper = r.wrapped;
  return c;
}

// Next record oldest -> newest. False at the end or at the first bad header (then c.bad = 1); nothing after a bad
// header is read.
inline bool next(const RingState& r, const uint8_t* buf, Cursor& c, uint32_t& offset, Header& h) {
  if (c.bad) return false;
  if (c.left == 0) {
    if (c.pos != r.head) c.bad = 1;
    return false;
  }
  if (!buf) {
    c.bad = 1;
    return false;
  }
  if (c.upper && c.pos == r.wrapAt) {
    if (r.wrapAt < r.size && buf[r.wrapAt] != kWrapMarker) {
      c.bad = 1;
      return false;
    }
    c.pos = 0;
    c.upper = 0;
  }
  const uint32_t seg = c.upper ? r.wrapAt : r.head;
  Header d;
  if (!detail::recordAt(r, buf, c.pos, seg, d) || d.seq <= c.lastSeq ||
      (c.left == r.count && d.seq != r.oldestSeq) || (c.left == 1 && d.seq != r.newestSeq)) {
    c.bad = 1;
    return false;
  }
  offset = c.pos;
  h = d;
  c.pos += footprint(d.length);
  c.left--;
  c.lastSeq = d.seq;
  return true;
}

// Copies min(h.length, cap-1) text bytes of the record at offset and a NUL. cap 0 / null out -> 0, nothing written.
inline size_t copyText(const uint8_t* buf, uint32_t offset, const Header& h, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  if (buf) {
    n = h.length < cap - 1 ? h.length : cap - 1;
    if (n) std::memcpy(out, buf + offset + kHeaderBytes, n);
  }
  out[n] = '\0';
  return n;
}

// Record with this seq (DESIGN 3.6). Liveness (oldestSeq <= seq <= newestSeq) is decided before the hint is looked
// at; the hint is bounds-checked against its segment before any byte is read and fully validated after; otherwise
// the ring is walked. *damaged is set (never cleared) when the state is insane or the walk met a bad header.
inline bool findBySeq(const RingState& r, const uint8_t* buf, uint32_t seq, uint32_t hint,
                      uint32_t& offset, Header& h, bool* damaged = nullptr) {
  if (!stateSane(r)) {
    if (damaged) *damaged = true;
    return false;
  }
  if (!buf || r.count == 0 || seq == 0 || seq < r.oldestSeq || seq > r.newestSeq) return false;
  if (kHintFastPath && hint % kAlign == 0) {
    uint32_t seg = 0;
    if (r.wrapped == 0) {
      if (r.tail <= hint && hint < r.head) seg = r.head;
    } else if (r.tail <= hint && hint < r.wrapAt) {
      seg = r.wrapAt;
    } else if (hint < r.head) {
      seg = r.head;
    }
    Header d;
    if (seg != 0 && detail::recordAt(r, buf, hint, seg, d) && d.seq == seq) {
      offset = hint;
      h = d;
      return true;
    }
  }
  Cursor c = begin(r);
  uint32_t off = 0;
  Header d;
  while (next(r, buf, c, off, d)) {
    if (d.seq == seq) {
      offset = off;
      h = d;
      return true;
    }
    if (d.seq > seq) return false;
  }
  if (c.bad && damaged) *damaged = true;
  return false;
}

// Stamps sendState on every record with this reqId (DESIGN 3.8): only the flags, status and check bytes of matching headers
// change. state > kSendStateMax or reqId == 0 -> 0, nothing written. Returns records changed. *damaged as findBySeq.
// `status`: the ACK status kept with kRefusedState on a sent record (1-15); 0 (and any other state) clears it.
inline int setSendState(RingState& r, uint8_t* buf, uint32_t reqId, uint8_t state, bool* damaged = nullptr,
                        uint8_t status = 0) {
  if (state > kSendStateMax || reqId == 0) return 0;
  if (!stateSane(r)) {
    if (damaged) *damaged = true;
    return 0;
  }
  if (!buf) return 0;
  int changed = 0;
  Cursor c = begin(r);
  uint32_t off = 0;
  Header d;
  while (next(r, buf, c, off, d)) {
    if (d.reqId != reqId) continue;
    uint8_t* p = buf + off;
    p[kOffFlags] = static_cast<uint8_t>((p[kOffFlags] & ~kSendStateMask) |
                                        ((static_cast<uint32_t>(state) << kSendStateShift) & kSendStateMask));
    p[kOffZero] = (d.sent && state == kRefusedState && status <= kStatusMax) ? status : 0;
    p[kOffCheck] = headerCheck(p);
    ++changed;
  }
  if (c.bad && damaged) *damaged = true;
  return changed;
}

// Text of the message with this reqId, pieces 1..total in order (DESIGN 3.7). The fragments are the records with
// the reqId and pieceTotal > 1; total comes from the first (oldest) of them. Single-frame records (pieceTotal <= 1)
// that share the reqId, such as a remote command's streamed output lines before its chunked result, are not part
// of it; only when the reqId has no fragment at all is the oldest single record with piece 1 the text (total 1).
// For a repeated piece the oldest wins; a missing piece leaves *complete false. One walk plus at most total decodes. out == nullptr or cap == 0 -> 0, nothing written. reqId 0 -> 0. pieceScratch[256] is all 0
// on return (it is cleared on entry too). *damaged as findBySeq; the pieces found before damage are still used.
// Like today's code, the copy stops once the output is full; pieces counted as found are those reached before that.
inline int reassemble(const RingState& r, const uint8_t* buf, uint32_t reqId, char* out, size_t cap,
                      bool* complete, uint16_t* pieceScratch, bool* damaged = nullptr) {
  if (complete) *complete = false;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (reqId == 0 || !pieceScratch) return 0;
  if (!stateSane(r)) {
    if (damaged) *damaged = true;
    return 0;
  }
  if (!buf) return 0;
  std::memset(pieceScratch, 0, 256 * sizeof(uint16_t));
  unsigned total = 0;
  bool anySingle = false;
  uint16_t single = 0;  // oldest single record with piece 1 (offset / kAlign + 1; 0: none)
  Cursor c = begin(r);
  uint32_t off = 0;
  Header d;
  while (next(r, buf, c, off, d)) {
    if (d.reqId != reqId) continue;
    if (d.pieceTotal > 1) {
      if (total == 0) total = d.pieceTotal;
      if (d.piece >= 1 && d.piece <= total && pieceScratch[d.piece] == 0)
        pieceScratch[d.piece] = static_cast<uint16_t>(off / kAlign + 1);
    } else {
      anySingle = true;
      if (d.piece == 1 && single == 0) single = static_cast<uint16_t>(off / kAlign + 1);
    }
  }
  if (c.bad && damaged) *damaged = true;
  if (total == 0) {
    if (!anySingle) return 0;
    total = 1;
    pieceScratch[1] = single;  // no fragment was recorded, so the scratch is all 0 here
  }
  size_t pos = 0;
  unsigned found = 0;
  bool stopped = false;
  for (unsigned p = 1; p <= total; ++p) {
    const uint16_t v = pieceScratch[p];
    pieceScratch[p] = 0;  // cleared for every p, also after the cap stop
    if (stopped) continue;
    if (v != 0) {
      const uint32_t o = (static_cast<uint32_t>(v) - 1) * kAlign;
      Header e;
      if (o <= r.size - kHeaderBytes && decodeHeader(buf + o, e) && e.reqId == reqId && e.piece == p &&
          footprint(e.length) <= r.size - o) {
        const size_t room = cap - 1 - pos;
        const size_t l = e.length < room ? e.length : room;
        if (l) std::memcpy(out + pos, buf + o + kHeaderBytes, l);
        pos += l;
        ++found;
      } else if (damaged) {
        *damaged = true;
      }
    }
    if (pos >= cap - 1) stopped = true;
  }
  out[pos] = '\0';
  if (complete) *complete = found >= total;
  return static_cast<int>(pos);
}

// ---- full check (tests; optional debug use) ---------------------------------------------
struct CheckResult { bool ok; const char* what; uint32_t at; };

// I1..I8, written independently of next(): geometry, empty normal form, orderings, exact tiling of each segment,
// every header valid with the ring's direction, the wrap marker, strictly ascending seqs from oldestSeq to newestSeq,
// count and used.
inline CheckResult checkRing(const RingState& r, const uint8_t* buf) {
  if (r.size % kAlign != 0 || r.size < kMinRingBytes || r.size > kMaxRingBytes) return {false, "I1 size", r.size};
  if (r.sent > 1 || r.wrapped > 1) return {false, "I1 sent/wrapped", 0};
  if (r.tail % kAlign || r.head % kAlign || r.wrapAt % kAlign) return {false, "I3 alignment", 0};
  if (r.tail > r.size || r.head > r.size || r.wrapAt > r.size) return {false, "I3 bounds", 0};
  if (r.count == 0) {
    if (r.tail || r.head || r.wrapped || r.wrapAt != r.size || r.used || r.oldestSeq || r.newestSeq)
      return {false, "I2 empty normal form", 0};
    return {true, "ok", 0};
  }
  if (r.tail == 0 && r.head == 0 && !r.wrapped) return {false, "I2 empty form with records", 0};
  if (!buf) return {false, "no buffer", 0};
  uint32_t segStart[2], segEnd[2];
  int segs = 0;
  if (!r.wrapped) {
    if (!(r.tail < r.head) || r.wrapAt != r.size) return {false, "I4 order", r.tail};
    if (r.used != r.head - r.tail) return {false, "I4 used", r.used};
    segStart[0] = r.tail;
    segEnd[0] = r.head;
    segs = 1;
  } else {
    if (!(0 < r.head && r.head <= r.tail && r.tail < r.wrapAt && r.wrapAt <= r.size))
      return {false, "I5 order", r.tail};
    if (r.used != (r.wrapAt - r.tail) + r.head) return {false, "I5 used", r.used};
    if (r.wrapAt < r.size && buf[r.wrapAt] != kWrapMarker) return {false, "I5 wrap marker", r.wrapAt};
    segStart[0] = r.tail;
    segEnd[0] = r.wrapAt;
    segStart[1] = 0;
    segEnd[1] = r.head;
    segs = 2;
  }
  uint32_t n = 0, sum = 0, last = 0, first = 0;
  for (int s = 0; s < segs; ++s) {
    uint32_t pos = segStart[s];
    while (pos != segEnd[s]) {
      if (pos > segEnd[s] || segEnd[s] - pos < kHeaderBytes) return {false, "I6 header past segment", pos};
      Header h;
      if (!decodeHeader(buf + pos, h)) return {false, "I6 header invalid", pos};
      const uint32_t f = footprint(h.length);
      if (f > segEnd[s] - pos) return {false, "I6 record past segment", pos};
      if (h.sent != (r.sent != 0)) return {false, "I6 direction", pos};
      if (h.seq <= last) return {false, "I7 seq order", pos};
      if (n == 0) first = h.seq;
      last = h.seq;
      ++n;
      sum += f;
      pos += f;
    }
  }
  if (n != r.count) return {false, "I8 count", n};
  if (sum != r.used) return {false, "I8 used", sum};
  if (first != r.oldestSeq) return {false, "I7 oldestSeq", first};
  if (last != r.newestSeq) return {false, "I7 newestSeq", last};
  if (r.used < n * kHeaderBytes || r.used > n * kMaxFootprint) return {false, "I8 bounds", r.used};
  if (!stateSane(r)) return {false, "stateSane", 0};
  return {true, "ok", 0};
}

// ---- merge and collapse ----------------------------------------------------------
struct Source { const RingState* ring; const uint8_t* buf; uint8_t slot; };   // direction = ring->sent
struct MergeScratch {
  Cursor   cur[kMaxSources];      // after a call, cur[i].bad marks a source that ended at damage (3.10)
  Header   hdr[kMaxSources];
  uint32_t off[kMaxSources];
  uint8_t  have[kMaxSources];
  uint32_t srcLast[kMaxSources];  // collapse: newest window entry (absolute number) of each source
};

namespace detail {
// Advances source i to its next record with seq > sinceSeq; have[i] = 0 at its end (or at damage: cur[i].bad).
inline void mergeAdvance(const Source& src, int i, uint32_t sinceSeq, MergeScratch& s) {
  s.have[i] = 0;
  uint32_t off = 0;
  Header h;
  while (next(*src.ring, src.buf, s.cur[i], off, h)) {
    if (h.seq > sinceSeq) {
      s.off[i] = off;
      s.hdr[i] = h;
      s.have[i] = 1;
      return;
    }
  }
}
}  // namespace detail

// visit(int sourceIndex, uint32_t offset, const Header& h) -> bool (false stops). Ascending seq, only
// seq > sinceSeq. A source with count == 0 or newestSeq <= sinceSeq is skipped without a decode.
// src == nullptr, n < 0 or n > kMaxSources -> 0, visit never called. Returns records visited (the number of
// visit calls, including the one that returned false).
// A source whose state is insane, or whose walk meets a bad header, ends there with s.cur[i].bad = 1; a source
// with a null ring or buffer is skipped (not marked).
template <class Visit>
int mergeBySeq(const Source* src, int n, uint32_t sinceSeq, MergeScratch& s, Visit&& visit) {
  if (!src || n < 0 || n > kMaxSources) return 0;
  for (int i = 0; i < n; ++i) {
    s.have[i] = 0;
    s.off[i] = 0;
    s.cur[i] = Cursor{0, 0, 0, 0, 0};
    const RingState* r = src[i].ring;
    if (!r) continue;
    if (!stateSane(*r)) {
      s.cur[i].bad = 1;
      continue;
    }
    if (r->count == 0 || r->newestSeq <= sinceSeq || !src[i].buf) continue;
    s.cur[i] = begin(*r);
    detail::mergeAdvance(src[i], i, sinceSeq, s);
  }
  int visited = 0;
  for (;;) {
    int best = -1;
    for (int i = 0; i < n; ++i) {
      if (s.have[i] && (best < 0 || s.hdr[i].seq < s.hdr[best].seq)) best = i;
    }
    if (best < 0) break;
    ++visited;
    const bool go = visit(best, s.off[best], static_cast<const Header&>(s.hdr[best]));
    if (!go) break;
    detail::mergeAdvance(src[best], best, s.hdr[best].seq, s);
  }
  return visited;
}

struct Ref {
  uint32_t seqNum;        // head record's seq (identity)
  uint32_t reqId;         // group id (0 = single / unsolicited)
  uint32_t timestamp;     // head record's millis (ordering key)
  uint32_t offset;        // head record's offset when the ref was made (hint only)
  uint8_t  slot;          // peer history slot
  uint8_t  headPiece;     // piece number of the head record
  uint8_t  partsPresent;  // fragments of the message in the ring
  uint8_t  partsTotal;    // fragments the message was split into (1 = single-frame)
  bool     isSent;        // ring: false received, true sent
};
static_assert(sizeof(Ref) == 24, "Ref layout");

namespace detail {
inline void reverseRefs(Ref* a, uint32_t lo, uint32_t hi) {  // [lo, hi)
  while (lo + 1 < hi) {
    --hi;
    const Ref t = a[lo];
    a[lo] = a[hi];
    a[hi] = t;
    ++lo;
  }
}
}  // namespace detail

// Newest maxN logical messages over the merged sources, chronological by merge order. A fragment joins
// a group among the last kGroupWindow entries of its own source. chain[chainCap] is caller scratch.
// out/chain null, maxN <= 0, maxN > chainCap, src null, n < 0 or n > kMaxSources -> 0, nothing written.
//
// Grouping rules (today's espnowCollapseRing): a record with reqId != 0 and pieceTotal > 1 joins the newest
// same-source entry with its reqId that a fragment opened (partsTotal > 1) within the window (partsPresent++
// capped at 255, partsTotal = max, and a lower piece becomes the head: seqNum, offset, timestamp, headPiece); any
// other record opens an entry. An entry a single-frame record opened (a remote command's streamed output line
// shares the command's reqId with its chunked result) is never joined, so it keeps its own text and the result
// gets its own entry, matching reassemble. Entry
// numbers are absolute (0, 1, 2, ...): entry a lives at out[a % maxN], chain[a % maxN] holds the previous entry
// of the same source (kNoEntry: none), and an entry below opened - maxN has been dropped (the oldest goes first).
inline int collapseNewest(const Source* src, int n, Ref* out, int maxN, MergeScratch& s,
                          uint32_t* chain, int chainCap) {
  if (!out || !chain || maxN <= 0 || maxN > chainCap || !src || n < 0 || n > kMaxSources) return 0;
  for (int i = 0; i < n; ++i) s.srcLast[i] = kNoEntry;
  const uint32_t m = static_cast<uint32_t>(maxN);
  uint32_t opened = 0;
  mergeBySeq(src, n, 0, s, [&](int i, uint32_t off, const Header& h) -> bool {
    const uint32_t live = opened < m ? opened : m;
    const uint32_t firstLive = opened - live;
    if (h.reqId != 0 && h.pieceTotal > 1) {
      uint32_t e = s.srcLast[i];
      for (int step = 0; step < kGroupWindow && e != kNoEntry && e >= firstLive; ++step) {
        Ref& g = out[e % m];
        if (g.reqId == h.reqId && g.partsTotal > 1) {
          if (g.partsPresent < 255) g.partsPresent++;
          if (h.pieceTotal > g.partsTotal) g.partsTotal = h.pieceTotal;
          if (h.piece < g.headPiece) {
            g.seqNum = h.seq;
            g.offset = off;
            g.timestamp = h.timestamp;
            g.headPiece = h.piece;
          }
          return true;
        }
        e = chain[e % m];
      }
    }
    const uint32_t a = opened++;
    Ref& g = out[a % m];
    g.seqNum = h.seq;
    g.reqId = h.reqId;
    g.timestamp = h.timestamp;
    g.offset = off;
    g.slot = src[i].slot;
    g.headPiece = h.piece;
    g.partsPresent = 1;
    g.partsTotal = h.pieceTotal > 1 ? h.pieceTotal : 1;
    g.isSent = h.sent;
    chain[a % m] = s.srcLast[i];
    s.srcLast[i] = a;
    return true;
  });
  const uint32_t w = opened < m ? opened : m;
  if (opened > m) {  // rotate out[0..m) so the oldest live entry comes first
    const uint32_t start = opened % m;
    if (start != 0) {
      detail::reverseRefs(out, 0, start);
      detail::reverseRefs(out, start, m);
      detail::reverseRefs(out, 0, m);
    }
  }
  return static_cast<int>(w);
}

// Stable, by timestamp (insertion sort); out null or n <= 0 -> no-op.
inline void sortByTime(Ref* out, int n) {
  if (!out || n <= 0) return;
  for (int i = 1; i < n; ++i) {
    const Ref x = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].timestamp > x.timestamp) {
      out[j + 1] = out[j];
      --j;
    }
    out[j + 1] = x;
  }
}

}  // namespace espnowhist
