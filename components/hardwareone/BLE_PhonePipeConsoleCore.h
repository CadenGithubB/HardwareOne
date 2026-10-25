#pragma once
// Phone link v2 console records: the byte layer inside the console stream (pipe
// slot 2). Pure and allocation-free (no BLE, OS or Arduino): the driver
// (BLE_PhonePipeConsole.cpp), the fixtures and the host tests share it.
//
// Record = type u8 | flags u8 (bit0 MORE, bits 1-7 zero) | len u16 LE | payload.
//   P4 -> phone: INFO 0x01 (the first record, once), TEXT 0x02 / BLOB 0x03 (one
//   message per MORE chain, <= 4608 B), CARD 0x04, MIRROR 0x05, ABORT 0x06 (cuts
//   the open chain); TICKET 0x07 is reserved (P3).
//   Phone -> P4: COMMAND 0x81 (one CLI line), BINARY 0x82 (one OtaBleWire
//   frame, 00 'HW1OTA' 01 offset_be32 chunk); RESUME 0x83 is reserved (P3).
// A receiver skips (and counts) a type it does not know and the reserved type
// addressed to it: 0..1024 B, no MORE. Malformed: flag bits 1-7, MORE outside
// TEXT/BLOB, a length out of bounds, a known type sent the wrong way, a BINARY
// without the OTA magic, and on the phone the chain/INFO rules below.
// Chains: TEXT or BLOB records with MORE, ended by one without MORE (or cut by
// ABORT). Inside an open chain only the same type or ABORT may follow; a record
// with MORE must leave room for at least one more byte under the cap. ABORT is
// sent only while a chain is open; a receiver ignores a stray one.
//
// Use:
//  - P4 ingress (driver): peek min(rxAvailable, kScanPeek) bytes, then
//    scanIngress(head, rxAvailable): NeedMore (size = bytes the record needs),
//    Deliver (COMMAND/BINARY: payload at offset kHeader, consume size), Skip
//    (consume size, count unknown) or Malformed (fault says why).
//  - P4 egress (writer, under the chain mutex): Egress::check() before each
//    push, commit() after it was accepted whole; ABORT while chainOpen() when a
//    chain fails. INFO is not a writer record: buildInfo() + encodeHeader().
//  - Phone side (fixtures, tests): PhoneReceiver::next() checks the P4's output.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hw1rec {
constexpr uint8_t kInfo = 0x01, kText = 0x02, kBlob = 0x03, kCard = 0x04, kMirror = 0x05,
  kAbort = 0x06, kTicket = 0x07, kCommand = 0x81, kBinary = 0x82, kResume = 0x83;
constexpr uint8_t kMore = 0x01;
constexpr size_t kHeader = 4, kPayloadMax = 1024, kRecordMax = kHeader + kPayloadMax;
constexpr size_t kInfoMax = 256, kTextMax = 1024, kChainMax = 4608, kCardMax = 512, kMirrorMax = 512,
  kCommandMax = 511, kBinaryMin = 13, kBinaryMax = 489, kUnknownMax = 1024;
inline constexpr uint8_t kOtaMagic[8] = {0x00, 'H', 'W', '1', 'O', 'T', 'A', 0x01};
constexpr size_t kScanPeek = kHeader + sizeof(kOtaMagic);  // scan() reads at most this many bytes
constexpr unsigned kInfoVersion = 1;                        // INFO "v": the record format
constexpr size_t kInfoField = 64;                           // escaped bytes per INFO string
static_assert(kBinaryMin == sizeof(kOtaMagic) + 4 + 1 && kBinaryMax + kHeader == 493, "OtaBleWire 12 B head + 1..477");
static_assert(kChainMax == 4096 + 512, "CMD_RESULT_MAX (System_CommandLimits.h) plus headroom");

enum class Side : uint8_t { P4, Phone };  // the receiving side
enum class Verdict : uint8_t { NeedMore, Deliver, Skip, Ignore, Malformed };
enum class Fault : uint8_t { None, Flags, More, Length, Direction, Magic, Type, Interleave, Cap, StrayAbort, InfoOrder };

struct Header {
  uint8_t type = 0, flags = 0;
  uint16_t len = 0;
  bool more() const { return (flags & kMore) != 0; }
};
inline void encodeHeader(uint8_t out[kHeader], uint8_t type, uint8_t flags, uint16_t len) {
  out[0] = type; out[1] = flags; out[2] = uint8_t(len); out[3] = uint8_t(len >> 8);
}
inline Header parseHeader(const uint8_t in[kHeader]) {
  Header h; h.type = in[0]; h.flags = in[1]; h.len = uint16_t(in[2] | uint16_t(in[3]) << 8); return h;
}
// Header + payload into out; 0 when it does not fit or len > 65535 (no bounds check).
inline size_t encode(uint8_t* out, size_t cap, uint8_t type, uint8_t flags, const uint8_t* payload, size_t len) {
  if (len > 0xFFFF || cap < kHeader + len) return 0;
  encodeHeader(out, type, flags, uint16_t(len));
  if (len) std::memcpy(out + kHeader, payload, len);
  return kHeader + len;
}

// What a type is. dir: 0 unknown, 1 P4 -> phone, 2 phone -> P4. Reserved types
// carry the unknown bounds until their phase defines them.
struct TypeSpec { uint8_t dir; bool reserved, more; uint16_t min, max; };
inline TypeSpec typeSpec(uint8_t type) {
  switch (type) {
    case kInfo: return {1, false, false, 1, kInfoMax};
    case kText: case kBlob: return {1, false, true, 1, kTextMax};
    case kCard: return {1, false, false, 1, kCardMax};
    case kMirror: return {1, false, false, 1, kMirrorMax};
    case kAbort: return {1, false, false, 0, 0};
    case kTicket: return {1, true, false, 0, kUnknownMax};
    case kCommand: return {2, false, false, 1, kCommandMax};
    case kBinary: return {2, false, false, kBinaryMin, kBinaryMax};
    case kResume: return {2, true, false, 0, kUnknownMax};
    default: return {0, false, false, 0, kUnknownMax};
  }
}
// A record of this type is skipped (not delivered) when it reaches `at`.
inline bool skipped(Side at, uint8_t type) {
  const TypeSpec s = typeSpec(type);
  return s.dir == 0 || (s.reserved && s.dir == (at == Side::P4 ? 2 : 1));
}
// Stateless header rules at the receiving side.
inline Fault headerFault(Side at, const Header& h) {
  if (h.flags & ~kMore) return Fault::Flags;
  const TypeSpec s = typeSpec(h.type);
  if (s.dir && s.dir != (at == Side::P4 ? 2 : 1)) return Fault::Direction;
  if (h.more() && !s.more) return Fault::More;
  if (h.len < s.min || h.len > s.max) return Fault::Length;
  return Fault::None;
}

struct Scan {
  Verdict verdict = Verdict::NeedMore;
  Fault fault = Fault::None;
  Header header;         // valid once kHeader bytes were there
  size_t size = kHeader;  // the record's bytes (kHeader + len); before the header, kHeader
  bool chainEnd = false;  // PhoneReceiver: this record ended a chain (last record or ABORT)
  size_t chainTotal = 0;  // PhoneReceiver: the chain's payload bytes so far / at its end
};
// One record at the start of a byte stream holding `have` bytes. bytes must hold
// min(have, kScanPeek) of them; nothing beyond is read. Malformed shows as soon
// as the offending byte is there (header, or a BINARY's magic); Deliver/Skip need
// the whole record. The verdict for a growing prefix never goes back.
inline Scan scan(Side at, const uint8_t* bytes, size_t have) {
  Scan r;
  if (have < kHeader) return r;
  r.header = parseHeader(bytes);
  r.size = kHeader + r.header.len;
  r.fault = headerFault(at, r.header);
  if (r.fault == Fault::None && at == Side::P4 && r.header.type == kBinary) {
    const size_t n = have - kHeader < sizeof(kOtaMagic) ? have - kHeader : sizeof(kOtaMagic);
    if (std::memcmp(bytes + kHeader, kOtaMagic, n) != 0) r.fault = Fault::Magic;
  }
  if (r.fault != Fault::None) r.verdict = Verdict::Malformed;
  else if (have >= r.size) r.verdict = skipped(at, r.header.type) ? Verdict::Skip : Verdict::Deliver;
  return r;
}
inline Scan scanIngress(const uint8_t* bytes, size_t have) { return scan(Side::P4, bytes, have); }

// The P4's chain state on one console stream, for the writer: which record may
// go next. check() never changes state; commit() applies a record that passed
// (after its push was accepted whole). The P4 sends TEXT, BLOB, CARD, MIRROR and
// ABORT this way; INFO, TICKET and anything else are Fault::Type.
class Egress {
 public:
  void reset() { type_ = 0; total_ = 0; }
  bool chainOpen() const { return type_ != 0; }
  uint8_t chainType() const { return type_; }
  size_t chainTotal() const { return total_; }
  Fault check(uint8_t type, uint8_t flags, size_t len) const {
    if (flags & ~kMore) return Fault::Flags;
    if (type != kText && type != kBlob && type != kCard && type != kMirror && type != kAbort) return Fault::Type;
    const TypeSpec s = typeSpec(type);
    if ((flags & kMore) && !s.more) return Fault::More;
    if (len < s.min || len > s.max) return Fault::Length;
    if (type == kAbort) return chainOpen() ? Fault::None : Fault::StrayAbort;
    if (!chainOpen()) return Fault::None;
    if (type != type_) return Fault::Interleave;
    const size_t total = total_ + len;
    return total > kChainMax || ((flags & kMore) && total >= kChainMax) ? Fault::Cap : Fault::None;
  }
  bool commit(uint8_t type, uint8_t flags, size_t len) {
    if (check(type, flags, len) != Fault::None) return false;
    if (type == kAbort) reset();
    else if (type == kText || type == kBlob) {
      if (flags & kMore) { total_ = (chainOpen() ? total_ : 0) + len; type_ = type; }
      else reset();
    }
    return true;
  }
 private:
  uint8_t type_ = 0;  // the open chain's type, 0 when none
  size_t total_ = 0;  // its payload bytes so far
};
// Chain slicing for a writer with `remaining` bytes left (>= 1): the next
// record's length and flags. A whole chain is 1..kChainMax bytes.
inline uint16_t sliceLen(size_t remaining) { return uint16_t(remaining > kTextMax ? kTextMax : remaining); }
inline uint8_t sliceFlags(size_t remaining) { return remaining > kTextMax ? kMore : 0; }
inline bool chainFits(size_t total) { return total >= 1 && total <= kChainMax; }

// The phone's view of the P4's records: scan(Side::Phone) plus INFO first and
// once, the chain rules, and a stray ABORT ignored. State advances only on a
// whole record (Deliver/Ignore); Skip leaves it; Malformed latches.
class PhoneReceiver {
 public:
  void reset() { *this = PhoneReceiver(); }
  bool failed() const { return failed_ != Fault::None; }
  bool infoSeen() const { return info_; }
  bool chainOpen() const { return type_ != 0; }
  uint8_t chainType() const { return type_; }
  size_t chainTotal() const { return total_; }
  Scan next(const uint8_t* bytes, size_t have) {
    Scan r = scan(Side::Phone, bytes, have);
    if (failed()) { r.verdict = Verdict::Malformed; r.fault = failed_; return r; }
    if (r.verdict == Verdict::Malformed) { failed_ = r.fault; return r; }
    if (have < kHeader || skipped(Side::Phone, r.header.type)) return r;
    const Fault f = stateFault(r.header);
    if (f != Fault::None) { failed_ = f; r.verdict = Verdict::Malformed; r.fault = f; return r; }
    if (r.verdict == Verdict::NeedMore) return r;
    const Header& h = r.header;
    if (h.type == kInfo) info_ = true;
    else if (h.type == kAbort) {
      if (!chainOpen()) r.verdict = Verdict::Ignore;
      else { r.chainEnd = true; r.chainTotal = total_; type_ = 0; total_ = 0; }
    } else if (h.type == kText || h.type == kBlob) {
      total_ = (chainOpen() ? total_ : 0) + h.len;
      r.chainTotal = total_;
      if (h.more()) type_ = h.type;
      else { r.chainEnd = true; type_ = 0; total_ = 0; }
    }
    return r;
  }
 private:
  Fault stateFault(const Header& h) const {
    if (h.type == kInfo) return info_ ? Fault::InfoOrder : Fault::None;
    if (!info_) return Fault::InfoOrder;
    if (!chainOpen() || h.type == kAbort) return Fault::None;
    if (h.type != type_) return Fault::Interleave;
    const size_t total = total_ + h.len;
    return total > kChainMax || (h.more() && total >= kChainMax) ? Fault::Cap : Fault::None;
  }
  bool info_ = false;
  uint8_t type_ = 0;
  size_t total_ = 0;
  Fault failed_ = Fault::None;
};

// INFO payload: {"v":1,"fw":"<fw>","model":"<model>","line":511,"bin":489,"auth":true,"resume":false}
// compact, in this key order. fw and model are JSON-escaped (" and \ escaped,
// control bytes and DEL as \u00XX, bytes >= 0x80 as '?'), each cut at
// kInfoField escaped bytes without splitting an escape; an empty or null model
// is "HardwareOne". Writes the payload plus NUL; returns its length (<=
// kInfoMax), or 0 (out[0] = NUL when cap > 0) when cap < kInfoMax + 1.
namespace info_detail {
inline void put(char* out, size_t& o, const char* s) { while (*s) out[o++] = *s++; }
inline void field(char* out, size_t& o, const char* s) {
  static const char hex[] = "0123456789abcdef";
  size_t used = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : ""); *p; ++p) {
    char e[6]; size_t n = 0; const unsigned char c = *p;
    if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = char(c); n = 2; }
    else if (c < 0x20 || c == 0x7F) { e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = hex[c >> 4]; e[5] = hex[c & 15]; n = 6; }
    else { e[0] = c >= 0x80 ? '?' : char(c); n = 1; }
    if (used + n > kInfoField) break;
    for (size_t i = 0; i < n; ++i) out[o++] = e[i];
    used += n;
  }
}
inline void number(char* out, size_t& o, unsigned v) {
  char d[10]; size_t n = 0;
  do { d[n++] = char('0' + v % 10); v /= 10; } while (v);
  while (n) out[o++] = d[--n];
}
}  // namespace info_detail
inline size_t buildInfo(char* out, size_t cap, const char* fw, const char* model) {
  if (cap < kInfoMax + 1) { if (cap) out[0] = '\0'; return 0; }
  using namespace info_detail;
  size_t o = 0;
  put(out, o, "{\"v\":"); number(out, o, kInfoVersion);
  put(out, o, ",\"fw\":\""); field(out, o, fw);
  put(out, o, "\",\"model\":\""); field(out, o, model && *model ? model : "HardwareOne");
  put(out, o, "\",\"line\":"); number(out, o, unsigned(kCommandMax));
  put(out, o, ",\"bin\":"); number(out, o, unsigned(kBinaryMax));
  put(out, o, ",\"auth\":true,\"resume\":false}");
  out[o] = '\0';
  return o;
}
static_assert(74 + 2 * kInfoField <= kInfoMax, "INFO always fits its record");

inline const char* typeName(uint8_t type) {
  switch (type) {
    case kInfo: return "INFO"; case kText: return "TEXT"; case kBlob: return "BLOB"; case kCard: return "CARD";
    case kMirror: return "MIRROR"; case kAbort: return "ABORT"; case kTicket: return "TICKET";
    case kCommand: return "COMMAND"; case kBinary: return "BINARY"; case kResume: return "RESUME";
    default: return "UNKNOWN";
  }
}
inline const char* faultName(Fault f) {
  switch (f) {
    case Fault::None: return "none"; case Fault::Flags: return "flags"; case Fault::More: return "more";
    case Fault::Length: return "length"; case Fault::Direction: return "direction"; case Fault::Magic: return "magic";
    case Fault::Type: return "type"; case Fault::Interleave: return "interleave"; case Fault::Cap: return "cap";
    case Fault::StrayAbort: return "stray-abort"; case Fault::InfoOrder: return "info-order";
  }
  return "?";
}
inline const char* verdictName(Verdict v) {
  switch (v) {
    case Verdict::NeedMore: return "need-more"; case Verdict::Deliver: return "deliver"; case Verdict::Skip: return "skip";
    case Verdict::Ignore: return "ignore"; case Verdict::Malformed: return "malformed";
  }
  return "?";
}
}  // namespace hw1rec
