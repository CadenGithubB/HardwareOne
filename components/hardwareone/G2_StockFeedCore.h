#ifndef G2_STOCK_FEED_CORE_H
#define G2_STOCK_FEED_CORE_H

// Stock G2 stock widget as a system-stats display: the sid 0x01
// DashboardDataPackage stock message the Even app sends to the right temple,
// rebuilt for HardwareOne's own metrics. Pure, host-testable, no Arduino/IDF,
// no allocation.
//
// Wire (decoded from an Even app 2.3.x -> G2 2.3.2.14 capture, 2026-11-20;
// every captured stock message re-encodes byte for byte with encodeMessage):
//   DashboardDataPackage { 1 command_id = 2, 2 magic,
//     4 Dashboard_Receive { 3 DashboardData { 2 WidgetData {
//       2 StockData { 1 stock_total, 2 stock_num, 3 StockInfo } } } } }
//   StockInfo, ascending tags, every set field written (zeros too):
//     1 code (row 1 left)          2 exchange (not drawn)
//     4 change % f32 (row 2 right) 5 price f32 (row 1 right, 0 shows "-")
//     6 name (row 2 left)          7 / 8 day high / low f32 (chart range)
//     9 open f32, 10 volume varint, 11 turnover f64 (none drawn)
//     12 copy of tag 4             13 trend (1 up, 2 down, other flat)
//     14 point_total               15 previous close f32 (dashed line)
//     16 points, packed f32, 4 x point_total bytes, <= 185
//   The glasses draw point i at a fixed step from the left edge, sized for
//   185 points, so the feed always sends a full 185-point window: the newest
//   point at the right edge, padded on the left until an hour of history
//   exists (Pad below).
//   One stock per message, stock_total first, stock_num 0..total-1, the next
//   one only after the previous ack. stock_total 0 clears the widget.
//   Ack (glasses -> phone, right temple): {1 command_id = 1, 2 magic echoed,
//   3 Dashboard_Respond {}}; a non-zero Dashboard_Respond field is an error.
//   Expanded stock page: the master temple pushes {1 command_id = 3,
//   2 0x12345678, 6 {1 package_id, 5 PageState {1 type, ...}}}; type 3 =
//   stock page open, any other type = left it.
//
// The feed (G2_StockFeed.cpp) only ever builds two shapes: one stock slot of
// a metric (makeSlot -> encodeSlot) and the clear message (encodeClear).
// validateMessagePb() re-parses a payload and refuses anything outside the
// whitelisted StockData shape before it may go on the wire.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "G2_FeedCore.h"
#include "System_MetricsCore.h"

namespace hw1_g2_stock_feed {

static constexpr uint8_t kSid = 0x01;          // dashboard service
static constexpr uint8_t kCmdDashboard = 2;    // phone -> glasses data
static constexpr uint8_t kCmdAck = 1;          // glasses -> phone reply
static constexpr uint8_t kCmdPush = 3;         // glasses -> phone page state
static constexpr uint32_t kPushMagic = 0x12345678u;
static constexpr uint32_t kPageTypeStockExpanded = 3;

// Firmware rules (decompiled update_stock, confirmed by the capture).
static constexpr uint8_t kMaxSlots = 9;        // stock_num >= 9 is ignored
static constexpr uint16_t kMaxPoints = 185;    // > 185 fails decode
static constexpr uint8_t kMaxTrend = 2;
// Whitelist limits (the firmware's own are 63 / 127 bytes).
static constexpr size_t kMaxCodeBytes = 15;
static constexpr size_t kMaxExchangeBytes = 8;
static constexpr size_t kMaxNameBytes = 63;
static constexpr size_t kMaxPbBytes = 1024;    // whitelist ceiling (185 points ~ 860 B)

// The feed's own, tighter shape.
static constexpr size_t kFeedCodeBytes = 4;    // owner: labels of 4 characters at most
static constexpr size_t kFeedNameBytes = 31;
// The chart window: always the glasses' full 185 points (one per history
// point of the sampler, 20 s apart, about an hour).
static constexpr uint8_t kFeedPoints = system_metrics_core::kHistoryLen;  // 185
static_assert(kFeedPoints == kMaxPoints, "the window is the glasses' full chart");
// One slot with a 185-point window: at most 837 B (a 31-byte name, magic >=
// 128), 4 fragments of 232 B.
static constexpr size_t kFeedPbBytes = 848;
static_assert(kFeedPbBytes <= kMaxPbBytes, "inside the whitelist ceiling");
static constexpr float kMinShownValue = 0.01f; // exactly 0 shows "-"
static constexpr float kMaxShownValue = 999999.0f;
static constexpr float kMaxChange = 9999.0f;

// Fixed magics of our other sid 0x01 messages; stock units never use them, so
// their echoes are never mistaken for a stock ack. The dashboard layout
// message (g2BuildDashboardDisplayJuly31) uses 240; the AppLaunch session
// prelude (a stock clear written at every connect) uses 156.
static constexpr uint8_t kLayoutMagic = 240;
static constexpr uint8_t kPreludeMagic = 156;

inline bool reservedMagic(uint8_t m) {
  return m == 0 || m == kLayoutMagic || m == kPreludeMagic;
}

// Glasses firmware versions with a captured, byte-exact stock fixture. The
// feed sends nothing to any other version.
static constexpr const char* kFixtureFirmware[] = {"2.3.2.14"};

inline bool firmwareSupported(const char* version) {
  if (!version || !*version) return false;
  for (const char* v : kFixtureFirmware)
    if (strcmp(v, version) == 0) return true;
  return false;
}

// ------------------------------------------------------------------ message

struct StockInfo {
  const char* code = nullptr;      // tag 1 (required)
  const char* exchange = nullptr;  // tag 2, omitted when null
  float changePercent = 0.0f;      // tag 4
  float price = 0.0f;              // tag 5
  const char* name = nullptr;      // tag 6, omitted when null
  float high = 0.0f;               // tag 7
  float low = 0.0f;                // tag 8
  bool hasOpen = false;            // tag 9
  float open = 0.0f;
  bool hasVolume = false;          // tag 10
  uint64_t volume = 0;
  bool hasTurnover = false;        // tag 11
  double turnover = 0.0;
  bool hasChangeCopy = false;      // tag 12 = tag 4, as the app writes it
  uint32_t trend = 0;              // tag 13
  bool hasPreviousClose = false;   // tag 15
  float previousClose = 0.0f;
  const float* points = nullptr;   // tags 14 + 16
  uint16_t pointCount = 0;
};

// Byte writer. With buf == nullptr it only counts.
struct Writer {
  uint8_t* buf;
  size_t cap;
  size_t len;
  bool ok;
  void byte(uint8_t b) {
    if (buf) {
      if (len >= cap) { ok = false; return; }
      buf[len] = b;
    }
    len++;
  }
  void varint(uint64_t v) {
    do {
      uint8_t b = (uint8_t)(v & 0x7F);
      v >>= 7;
      if (v) b |= 0x80;
      byte(b);
    } while (v);
  }
  void key(uint32_t field, uint8_t wire) { varint(((uint64_t)field << 3) | wire); }
  void fieldVarint(uint32_t field, uint64_t v) { key(field, 0); varint(v); }
  void f32(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    for (int i = 0; i < 4; i++) byte((uint8_t)(bits >> (8 * i)));
  }
  void fieldFloat(uint32_t field, float f) { key(field, 5); f32(f); }
  void fieldDouble(uint32_t field, double d) {
    key(field, 1);
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    for (int i = 0; i < 8; i++) byte((uint8_t)(bits >> (8 * i)));
  }
  void fieldString(uint32_t field, const char* s) {
    const size_t n = strlen(s);
    key(field, 2);
    varint(n);
    for (size_t i = 0; i < n; i++) byte((uint8_t)s[i]);
  }
};

inline void writeStockInfo(Writer& w, const StockInfo& s) {
  w.fieldString(1, s.code ? s.code : "");
  if (s.exchange) w.fieldString(2, s.exchange);
  w.fieldFloat(4, s.changePercent);
  w.fieldFloat(5, s.price);
  if (s.name) w.fieldString(6, s.name);
  w.fieldFloat(7, s.high);
  w.fieldFloat(8, s.low);
  if (s.hasOpen) w.fieldFloat(9, s.open);
  if (s.hasVolume) w.fieldVarint(10, s.volume);
  if (s.hasTurnover) w.fieldDouble(11, s.turnover);
  if (s.hasChangeCopy) w.fieldFloat(12, s.changePercent);
  w.fieldVarint(13, s.trend);
  w.fieldVarint(14, s.pointCount);
  if (s.hasPreviousClose) w.fieldFloat(15, s.previousClose);
  if (s.pointCount && s.points) {
    w.key(16, 2);
    w.varint((uint64_t)s.pointCount * 4u);
    for (uint16_t i = 0; i < s.pointCount; i++) w.f32(s.points[i]);
  }
}

inline size_t varintLen(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) { v >>= 7; n++; }
  return n;
}

// Writes the DashboardDataPackage around a StockData body of `bodyLen` bytes
// produced by `body(w)`.
template <typename Body>
inline size_t encodeEnvelope(uint32_t magic, size_t bodyLen, Body body,
                             uint8_t* out, size_t cap) {
  if (!out || cap == 0) return 0;
  if (cap > kMaxPbBytes) cap = kMaxPbBytes;
  const size_t widget = 1 + varintLen(bodyLen) + bodyLen;      // 12 StockData
  const size_t dashboard = 1 + varintLen(widget) + widget;      // 12 WidgetData
  const size_t receive = 1 + varintLen(dashboard) + dashboard;  // 1a DashboardData
  Writer w{out, cap, 0, true};
  w.fieldVarint(1, kCmdDashboard);
  w.fieldVarint(2, magic);
  w.key(4, 2);
  w.varint(receive);
  w.key(3, 2);
  w.varint(dashboard);
  w.key(2, 2);
  w.varint(widget);
  w.key(2, 2);
  w.varint(bodyLen);
  const size_t start = w.len;
  body(w);
  if (!w.ok || w.len - start != bodyLen) return 0;
  return w.len;
}

// One stock message: slot `num` of `total`. Returns the payload length, or 0
// when the arguments are out of range or it does not fit `cap`.
inline size_t encodeMessage(const StockInfo& s, uint8_t total, uint8_t num,
                            uint32_t magic, uint8_t* out, size_t cap) {
  if (total == 0 || total > kMaxSlots || num >= total || !s.code) return 0;
  if (s.pointCount > kMaxPoints || (s.pointCount && !s.points)) return 0;
  Writer count{nullptr, 0, 0, true};
  writeStockInfo(count, s);
  const size_t infoLen = count.len;
  const size_t bodyLen = 2 + 2 + 1 + varintLen(infoLen) + infoLen;
  return encodeEnvelope(
      magic, bodyLen,
      [&](Writer& w) {
        w.fieldVarint(1, total);
        w.fieldVarint(2, num);
        w.key(3, 2);
        w.varint(infoLen);
        writeStockInfo(w, s);
      },
      out, cap);
}

// stock_total = 0: the firmware clears the widget and resets its received
// count. StockData {1 stock_total 0, 2 stock_num 0}: the same body as the
// AppLaunch session prelude (G2_Protocol.cpp PRELUDE_F5872, magic 156) that
// HardwareOne already writes to each temple on every connect, so the feed's
// clear matches one the glasses take at every connect.
inline size_t encodeClear(uint32_t magic, uint8_t* out, size_t cap) {
  return encodeEnvelope(
      magic, 4,
      [](Writer& w) {
        w.fieldVarint(1, 0);
        w.fieldVarint(2, 0);
      },
      out, cap);
}

// ------------------------------------------------------------------ parsing

struct Reader {
  const uint8_t* p;
  size_t end;
  size_t pos;
  bool varint(uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (pos >= end) return false;
      const uint8_t b = p[pos++];
      v |= (uint64_t)(b & 0x7F) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  }
  bool key(uint32_t& field, uint8_t& wire) {
    uint64_t k = 0;
    if (!varint(k) || k > 0xFFFFFFFFull) return false;
    field = (uint32_t)(k >> 3);
    wire = (uint8_t)(k & 7);
    return field != 0;
  }
  bool expectKey(uint32_t field, uint8_t wire) {
    uint32_t f = 0;
    uint8_t w = 0;
    return key(f, w) && f == field && w == wire;
  }
  bool expectVarint(uint32_t field, uint64_t& v) { return expectKey(field, 0) && varint(v); }
  // Length-delimited field whose bytes must run to `end` exactly.
  bool expectNested(uint32_t field) {
    uint64_t n = 0;
    return expectKey(field, 2) && varint(n) && n == end - pos;
  }
  bool length(size_t& n) {
    uint64_t v = 0;
    if (!varint(v) || v > end - pos) return false;
    n = (size_t)v;
    return true;
  }
  bool f32(float& f) {
    if (end - pos < 4) return false;
    uint32_t bits = 0;
    for (int i = 0; i < 4; i++) bits |= (uint32_t)p[pos + i] << (8 * i);
    pos += 4;
    memcpy(&f, &bits, sizeof f);
    return true;
  }
  bool f64(double& d) {
    if (end - pos < 8) return false;
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) bits |= (uint64_t)p[pos + i] << (8 * i);
    pos += 8;
    memcpy(&d, &bits, sizeof d);
    return true;
  }
  bool skip(uint8_t wire) {
    uint64_t v = 0;
    size_t n = 0;
    switch (wire) {
      case 0: return varint(v);
      case 1: if (end - pos < 8) return false; pos += 8; return true;
      case 2: if (!length(n)) return false; pos += n; return true;
      case 5: if (end - pos < 4) return false; pos += 4; return true;
      default: return false;
    }
  }
};

// ------------------------------------------------------------------ padding

// How the left of the chart window is filled before kFeedPoints real points
// exist (`g2 stats pad`, saved): Gap = NaN points, which pass the glasses'
// negative-point check (how they draw is unverified until it is seen on the
// lens); Flat = the oldest real point repeated.
enum class Pad : uint8_t { Gap = 0, Flat = 1 };

// The one NaN the feed writes (canonical quiet NaN); the whitelist accepts no
// other.
static constexpr uint32_t kGapBits = 0x7FC00000u;

inline float gapPoint() {
  float f;
  memcpy(&f, &kGapBits, sizeof f);
  return f;
}

inline bool isGapPoint(float f) {
  uint32_t bits;
  memcpy(&bits, &f, sizeof bits);
  return bits == kGapBits;
}

inline Pad padFromSetting(int v) { return v == 1 ? Pad::Flat : Pad::Gap; }
inline const char* padName(Pad p) { return p == Pad::Flat ? "flat" : "gap"; }

// "gap" or "flat" (any case, surrounding spaces), or the setting values "0"
// / "1" the settings editors send for the g2StockPad row. False otherwise.
inline bool parsePad(const char* text, Pad& out) {
  if (!text) return false;
  while (*text == ' ') text++;
  char word[6];
  size_t n = 0;
  for (; *text && *text != ' '; text++) {
    if (n + 1 >= sizeof word) return false;
    char c = *text;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    word[n++] = c;
  }
  word[n] = '\0';
  while (*text == ' ') text++;
  if (*text) return false;
  if (strcmp(word, "gap") == 0 || strcmp(word, "0") == 0) { out = Pad::Gap; return true; }
  if (strcmp(word, "1") == 0) { out = Pad::Flat; return true; }
  if (strcmp(word, "flat") == 0) { out = Pad::Flat; return true; }
  return false;
}

inline bool printableAscii(const uint8_t* s, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (s[i] < 0x20 || s[i] > 0x7E) return false;
  return true;
}

struct Parsed {
  uint32_t magic = 0;
  uint8_t total = 0;
  uint8_t num = 0;
  bool clear = false;
  float price = 0.0f, changePercent = 0.0f, high = 0.0f, low = 0.0f;
  uint32_t trend = 0;
  uint16_t pointCount = 0;
  uint16_t padCount = 0;  // leading gap (NaN) points
  char code[kMaxCodeBytes + 1] = {};
};

// Strict whitelist check of one encoded stock or clear message: the exact
// nesting above, StockInfo tags ascending from the known set with their wire
// types, printable strings within the limits, finite numbers, price > 0,
// 0 <= low < high, trend <= 2, point_total <= 185 and matching the packed
// points, every point inside [low, high]. Fields 1, 5, 7, 8, 13 and 14 are
// required. `gapPrefix` (the feed's Pad::Gap): the window may start with
// gap points (exactly kGapBits) when it is the full 185 points and at least
// its last point is real; a gap point anywhere after a real one, or any
// other non-finite point, is refused either way.
inline bool validateMessagePb(const uint8_t* pb, size_t len, Parsed* out = nullptr,
                              bool gapPrefix = false) {
  if (!pb || len == 0 || len > kMaxPbBytes) return false;
  Reader r{pb, len, 0};
  Parsed res;
  uint64_t v = 0;
  if (!r.expectVarint(1, v) || v != kCmdDashboard) return false;
  if (!r.expectVarint(2, v) || v > 0xFFFFFFFFull) return false;
  res.magic = (uint32_t)v;
  if (!r.expectNested(4) || !r.expectNested(3) || !r.expectNested(2) || !r.expectNested(2))
    return false;
  if (!r.expectVarint(1, v) || v > kMaxSlots) return false;
  res.total = (uint8_t)v;
  if (res.total == 0) {
    // The clear: exactly {1: 0, 2: 0}, as encodeClear and the prelude write it.
    if (!r.expectVarint(2, v) || v != 0 || r.pos != len) return false;
    res.clear = true;
    if (out) *out = res;
    return true;
  }
  if (!r.expectVarint(2, v) || v >= res.total) return false;
  res.num = (uint8_t)v;
  if (!r.expectNested(3)) return false;

  uint32_t lastField = 0;
  uint32_t seen = 0;  // bit per field
  bool haveCopy = false;
  float copy = 0.0f;
  size_t pointsAt = 0, pointsBytes = 0;
  while (r.pos < len) {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!r.key(field, wire) || field <= lastField || field > 16) return false;
    lastField = field;
    seen |= 1u << field;
    size_t n = 0;
    float f = 0.0f;
    double d = 0.0;
    switch (field) {
      case 1:
      case 2:
      case 6: {
        const size_t limit = field == 1 ? kMaxCodeBytes : field == 2 ? kMaxExchangeBytes : kMaxNameBytes;
        if (wire != 2 || !r.length(n) || n > limit || !printableAscii(pb + r.pos, n)) return false;
        if (field == 1) {
          if (n == 0) return false;
          memcpy(res.code, pb + r.pos, n);
          res.code[n] = '\0';
        }
        r.pos += n;
        break;
      }
      case 4: case 5: case 7: case 8: case 9: case 12: case 15:
        if (wire != 5 || !r.f32(f) || !isfinite(f) || fabsf(f) > 1.0e9f) return false;
        if (field == 4) res.changePercent = f;
        if (field == 5) res.price = f;
        if (field == 7) res.high = f;
        if (field == 8) res.low = f;
        if (field == 12) { haveCopy = true; copy = f; }
        break;
      case 10:
        if (wire != 0 || !r.varint(v)) return false;
        break;
      case 11:
        if (wire != 1 || !r.f64(d) || !isfinite(d)) return false;
        break;
      case 13:
        if (wire != 0 || !r.varint(v) || v > kMaxTrend) return false;
        res.trend = (uint32_t)v;
        break;
      case 14:
        if (wire != 0 || !r.varint(v) || v > kMaxPoints) return false;
        res.pointCount = (uint16_t)v;
        break;
      case 16:
        if (wire != 2 || !r.length(n) || n == 0 || n % 4) return false;
        pointsAt = r.pos;
        pointsBytes = n;
        r.pos += n;
        break;
      default:
        return false;  // 3 and anything unknown
    }
  }
  const uint32_t required = (1u << 1) | (1u << 5) | (1u << 7) | (1u << 8) | (1u << 13) | (1u << 14);
  if ((seen & required) != required) return false;
  if (!(res.price > 0.0f) || !(res.low >= 0.0f) || !(res.high > res.low)) return false;
  if (haveCopy && memcmp(&copy, &res.changePercent, sizeof copy) != 0) return false;
  if (pointsBytes / 4 != res.pointCount) return false;
  Reader pr{pb, pointsAt + pointsBytes, pointsAt};
  bool inPrefix = true;
  for (uint16_t i = 0; i < res.pointCount; i++) {
    float p = 0.0f;
    if (!pr.f32(p)) return false;
    if (isGapPoint(p)) {
      if (!gapPrefix || !inPrefix) return false;
      res.padCount++;
      continue;
    }
    inPrefix = false;
    if (!isfinite(p) || p < res.low || p > res.high) return false;
  }
  if (res.padCount && (res.pointCount != kMaxPoints || res.padCount >= res.pointCount))
    return false;
  if (out) *out = res;
  return true;
}

// Ack for one of our messages: {1 = 1, 2 magic, 3 Dashboard_Respond}. Returns
// true when `pb` is an ack at all (magic set); `accepted` is false when the
// Dashboard_Respond carries any non-zero varint (settings_id, result).
inline bool parseAck(const uint8_t* pb, size_t len, uint8_t& magic, bool& accepted) {
  uint8_t cmd = 0;
  if (!hw1_g2_feed::parseCmdMagic(pb, len, cmd, magic) || cmd != kCmdAck) return false;
  Reader r{pb, len, 0};
  uint64_t v = 0;
  r.expectVarint(1, v);
  r.expectVarint(2, v);
  accepted = true;
  while (r.pos < len) {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!r.key(field, wire)) { accepted = false; break; }
    if (field == 3 && wire == 2) {
      size_t n = 0;
      if (!r.length(n)) { accepted = false; break; }
      Reader in{pb, r.pos + n, r.pos};
      while (in.pos < in.end) {
        uint32_t f2 = 0;
        uint8_t w2 = 0;
        if (!in.key(f2, w2)) { accepted = false; break; }
        if (w2 == 0) {
          if (!in.varint(v)) { accepted = false; break; }
          if (v != 0) accepted = false;
        } else if (!in.skip(w2)) {
          accepted = false;
          break;
        }
      }
      r.pos += n;
    } else if (!r.skip(wire)) {
      accepted = false;
      break;
    }
  }
  return true;
}

// Page-state push from the glasses: true when `pb` is one, with its
// PageState.type in `type`.
inline bool parsePageState(const uint8_t* pb, size_t len, uint32_t& type) {
  if (!pb || len == 0 || len > kMaxPbBytes) return false;
  Reader r{pb, len, 0};
  uint64_t v = 0;
  if (!r.expectVarint(1, v) || v != kCmdPush) return false;
  if (!r.expectVarint(2, v)) return false;
  while (r.pos < len) {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!r.key(field, wire)) return false;
    if (field != 6 || wire != 2) {
      if (!r.skip(wire)) return false;
      continue;
    }
    size_t n = 0;
    if (!r.length(n)) return false;
    Reader body{pb, r.pos + n, r.pos};
    while (body.pos < body.end) {
      if (!body.key(field, wire)) return false;
      if (field != 5 || wire != 2) {
        if (!body.skip(wire)) return false;
        continue;
      }
      size_t m = 0;
      if (!body.length(m)) return false;
      Reader state{pb, body.pos + m, body.pos};
      if (!state.expectVarint(1, v) || v > 0xFF) return false;
      type = (uint32_t)v;
      return true;
    }
    return false;
  }
  return false;
}

// ------------------------------------------------------------------ metrics

// Default order (owner, 2026-11-19): CPU, internal RAM, PSRAM, then the rest;
// the CPU clock in the chip temperature's place (owner, 2026-11-21; TEMP can
// still be chosen).
static constexpr SystemMetricId kDefaultMetrics[kMaxSlots] = {
    SystemMetricId::Cpu,  SystemMetricId::Heap,  SystemMetricId::Psram,
    SystemMetricId::Sd,   SystemMetricId::Lfs,   SystemMetricId::Clk,
    SystemMetricId::Links, SystemMetricId::Mesh, SystemMetricId::Ring};
static constexpr const char* kDefaultMetricsText = "CPU,HEAP,PSRM,SD,LFS,CLK,LINK,MESH,RING";
// The default before CLK. The setting holds a label list, never "default"
// (`g2 stats metrics default` saves the list it stands for), and every
// settings save writes it out, so a device that never chose its own list has
// this text saved: savedSelection reads it as the current default.
static constexpr const char* kPreviousDefaultMetricsText =
    "CPU,HEAP,PSRM,SD,LFS,TEMP,LINK,MESH,RING";

struct Selection {
  SystemMetricId ids[kMaxSlots];
  uint8_t count = 0;
};

inline Selection defaultSelection() {
  Selection s;
  for (uint8_t i = 0; i < kMaxSlots; i++) s.ids[i] = kDefaultMetrics[i];
  s.count = kMaxSlots;
  return s;
}

// "CPU,HEAP psrm" (commas and/or spaces, any case) or "default". On failure
// `error` names the problem and `out` is left unchanged.
inline bool parseMetricList(const char* text, Selection& out, char* error, size_t errorCap) {
  auto fail = [&](const char* msg, const char* token, size_t tokenLen) {
    if (error && errorCap) {
      size_t n = 0;
      for (const char* m = msg; *m && n + 1 < errorCap; m++) error[n++] = *m;
      for (size_t i = 0; token && i < tokenLen && n + 1 < errorCap; i++) error[n++] = token[i];
      error[n] = '\0';
    }
    return false;
  };
  if (!text) return fail("no metrics given", nullptr, 0);
  while (*text == ' ' || *text == ',') text++;
  if (!*text) return fail("no metrics given", nullptr, 0);
  {
    const char* d = "default";
    const char* t = text;
    while (*d && (*t == *d || *t == *d - 32)) { d++; t++; }
    while (*t == ' ') t++;
    if (!*d && !*t) {
      out = defaultSelection();
      return true;
    }
  }
  Selection s;
  const char* p = text;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ' && *p != ',') p++;
    const size_t n = (size_t)(p - start);
    char token[8];
    if (n >= sizeof token) return fail("unknown metric: ", start, n);
    memcpy(token, start, n);
    token[n] = '\0';
    SystemMetricId id;
    if (!system_metrics_core::idFromLabel(token, id)) return fail("unknown metric: ", start, n);
    for (uint8_t i = 0; i < s.count; i++)
      if (s.ids[i] == id) return fail("listed twice: ", start, n);
    if (s.count >= kMaxSlots) return fail("more than 9 metrics", nullptr, 0);
    s.ids[s.count++] = id;
  }
  if (!s.count) return fail("no metrics given", nullptr, 0);
  out = s;
  return true;
}

// The saved setting (g2StockMetrics) as a selection: the previous default's
// exact text is the current default, and so is anything parseMetricList
// refuses (a hand edit, an empty string). Any other list is kept as saved.
inline Selection savedSelection(const char* saved) {
  if (!saved || strcmp(saved, kPreviousDefaultMetricsText) == 0) return defaultSelection();
  Selection s;
  char err[48];
  if (!parseMetricList(saved, s, err, sizeof err)) return defaultSelection();
  return s;
}

// "CPU,HEAP,PSRM" (labels joined by commas). Returns the length written.
inline size_t formatMetricList(const Selection& s, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < s.count; i++) {
    const system_metrics_core::MetricInfo* m = system_metrics_core::info(s.ids[i]);
    if (!m) continue;
    const size_t l = strlen(m->label);
    if (n + l + (n ? 1 : 0) + 1 > cap) break;
    if (n) out[n++] = ',';
    memcpy(out + n, m->label, l);
    n += l;
    out[n] = '\0';
  }
  return n;
}

// The list as `g2 stats metrics` saves it: formatMetricList, except that the
// one list savedSelection would read as the current default (the previous
// default, chosen again on purpose) is saved space-separated, so it stays as
// chosen after a reboot. Returns the length written.
inline size_t formatSavedMetricList(const Selection& s, char* out, size_t cap) {
  const size_t n = formatMetricList(s, out, cap);
  if (n && strcmp(out, kPreviousDefaultMetricsText) == 0)
    for (size_t i = 0; i < n; i++)
      if (out[i] == ',') out[i] = ' ';
  return n;
}

// Feed interval in seconds: digits only, kMinIntervalSec..kMaxIntervalSec.
static constexpr uint32_t kDefaultIntervalSec = 60;
static constexpr uint32_t kMinIntervalSec = 30;
static constexpr uint32_t kMaxIntervalSec = 3600;
// While the expanded stock page is open (the glasses report it). 10 s, not
// 5: a full batch is 9 slots of about 840 B (4 fragments each), written one
// at a time with an ack round trip in between, a few seconds on the air; at
// 5 s batches could run back to back. Sending only the 3 visible slots would
// be lighter but cannot work: the glasses redraw only after stock_total
// messages and a different stock_total restarts their count, and the page
// state does not say which page is shown.
static constexpr uint32_t kPageOpenIntervalMs = system_metrics_core::kFastIntervalMs;
static_assert(kPageOpenIntervalMs >= 10000, "page-open cadence >= 10 s (airtime)");

inline bool parseIntervalSec(const char* text, uint32_t& out) {
  if (!text) return false;
  while (*text == ' ') text++;
  uint32_t v = 0;
  size_t digits = 0;
  for (; *text >= '0' && *text <= '9'; text++) {
    if (++digits > 5) return false;
    v = v * 10 + (uint32_t)(*text - '0');
  }
  while (*text == ' ') text++;
  if (!digits || *text || v < kMinIntervalSec || v > kMaxIntervalSec) return false;
  out = v;
  return true;
}

inline uint32_t clampIntervalSec(int v) {
  if (v < (int)kMinIntervalSec) return kDefaultIntervalSec;
  if (v > (int)kMaxIntervalSec) return kMaxIntervalSec;
  return (uint32_t)v;
}

// Chart step in seconds (`g2 stats step`, saved): one history point per step,
// 185 points, so the window is 185 x step. Only these three; the setting row
// lists the same values for the settings editors.
static constexpr uint32_t kDefaultStepSec = system_metrics_core::kHistoryStepMs / 1000u;  // 20
static constexpr uint32_t kStepChoicesSec[] = {10, 20, 60};
static constexpr char kStepChoicesText[] = "10, 20 or 60";
static_assert(sizeof kStepChoicesSec / sizeof kStepChoicesSec[0] ==
                  sizeof system_metrics_core::kHistoryStepChoicesMs /
                      sizeof system_metrics_core::kHistoryStepChoicesMs[0],
              "the command's steps are the sampler's");

inline bool stepAllowedSec(uint32_t sec) {
  return sec <= UINT32_MAX / 1000u && system_metrics_core::historyStepAllowed(sec * 1000u);
}

// "10", "20" or "60" (digits only, surrounding spaces). False otherwise.
inline bool parseStepSec(const char* text, uint32_t& out) {
  if (!text) return false;
  while (*text == ' ') text++;
  uint32_t v = 0;
  size_t digits = 0;
  for (; *text >= '0' && *text <= '9'; text++) {
    if (++digits > 3) return false;
    v = v * 10 + (uint32_t)(*text - '0');
  }
  while (*text == ' ') text++;
  if (!digits || *text || !stepAllowedSec(v)) return false;
  out = v;
  return true;
}

// The saved setting as a step: an allowed value as is, anything else (a hand
// edit, an old file) the default.
inline uint32_t stepFromSetting(int v) {
  return v > 0 && stepAllowedSec((uint32_t)v) ? (uint32_t)v : kDefaultStepSec;
}

// The time the full chart covers at `stepSec`, to the nearest minute:
// "31 min" (10 s), "1 h 2 min" (20 s), "3 h 5 min" (60 s).
inline size_t formatWindow(uint32_t stepSec, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  const uint32_t min = (system_metrics_core::historyWindowMs(stepSec * 1000u) / 1000u + 30u) / 60u;
  int n;
  if (min < 60) n = snprintf(out, cap, "%u min", (unsigned)min);
  else if (min % 60 == 0) n = snprintf(out, cap, "%u h", (unsigned)(min / 60));
  else n = snprintf(out, cap, "%u h %u min", (unsigned)(min / 60), (unsigned)(min % 60));
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

// The chart line of `g2 stats status`: step, points, the window they cover,
// and the padding.
inline size_t formatChartLine(uint32_t stepSec, const char* padName, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  char window[24];
  formatWindow(stepSec, window, sizeof window);
  const int n = snprintf(out, cap,
                         "Chart: a point every %u s (saved), %u points = the last %s, left "
                         "padding %s (saved)",
                         (unsigned)stepSec, (unsigned)kFeedPoints, window,
                         padName ? padName : "?");
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

// Send policy: settle a moment after connect (the app's first batch went out
// 3.7 s after the link came up), ack timeout 3 s from the last write of a
// unit (acks took 34-546 ms in the capture), one retry.
static constexpr uint32_t kConnectSettleMs = 3000;
static constexpr uint32_t kAckTimeoutMs = 3000;
static constexpr uint8_t kMaxRetries = 1;

inline hw1_g2_feed::Timing timing(bool pageOpen, uint32_t intervalSec) {
  const uint32_t ms = pageOpen ? kPageOpenIntervalMs : clampIntervalSec((int)intervalSec) * 1000u;
  return hw1_g2_feed::Timing{kConnectSettleMs, ms, kAckTimeoutMs, kMaxRetries};
}

// The feed's sampler consumer. Wanted whenever the feed is on (saved),
// whatever the glasses link does: a disconnect must not end the sampler run,
// which would clear the history, so a reconnect sends the last hour. Only
// feed off (and no preview/send warm-up) lets the run end. The link state is
// deliberately not an input.
inline bool samplerWanted(bool feedOn, bool warm) { return feedOn || warm; }

// Its refresh interval: the page-open cadence while the expanded page is open
// on a connected temple or a preview/send warms it, the feed interval
// otherwise. The history clock (20 s) runs independently of it.
inline uint32_t samplerIntervalMs(bool pageOpen, bool warm, uint32_t intervalSec) {
  return (pageOpen || warm) ? kPageOpenIntervalMs : clampIntervalSec((int)intervalSec) * 1000u;
}

// First magic of a unit of `count` messages after `prev`: consecutive by
// nextMagic, never a reserved magic (0, the layout's, the prelude's). A unit
// that would run over one starts right after it instead.
inline uint8_t unitMagic(uint8_t prev, uint8_t count) {
  uint8_t first = hw1_g2_feed::nextMagic(prev);
  for (int attempt = 0; attempt < 4; attempt++) {
    uint8_t m = first;
    bool clash = false;
    for (uint8_t i = 0; i < count; i++) {
      if (reservedMagic(m)) {
        clash = true;
        break;
      }
      m = hw1_g2_feed::nextMagic(m);
    }
    if (!clash) return first;
    first = hw1_g2_feed::nextMagic(m);
  }
  return first;
}

inline uint8_t magicAt(uint8_t first, uint8_t index) {
  for (uint8_t i = 0; i < index; i++) first = hw1_g2_feed::nextMagic(first);
  return first;
}

// One metric as the sampler reports it (System_Metrics.h reading + history).
struct MetricInput {
  SystemMetricId id = SystemMetricId::Cpu;
  bool available = false;
  float value = 0.0f;
  float delta = 0.0f;
  float rangeLow = 0.0f;
  float rangeHigh = 0.0f;
  int8_t trend = 0;
  const float* history = nullptr;  // oldest first
  uint8_t historyCount = 0;
};

// One stock slot exactly as it will be encoded.
struct Slot {
  char code[kFeedCodeBytes + 1] = {};
  char name[kFeedNameBytes + 1] = {};
  float value = kMinShownValue;
  float change = 0.0f;
  float low = 0.0f;
  float high = 100.0f;
  uint8_t trend = 0;
  uint8_t pointCount = 0;  // kFeedPoints, or 1 with no history yet
  uint8_t realPoints = 0;  // history points in the window (the rest is padding)
  float points[kFeedPoints] = {};
};

inline void copyText(char* dst, size_t cap, const char* src) {
  size_t n = 0;
  for (; src && src[n] && n + 1 < cap; n++) {
    const char c = src[n];
    dst[n] = (c >= 0x20 && c <= 0x7E) ? c : '?';
  }
  dst[n] = '\0';
}

inline float finiteOr(float f, float fallback) { return isfinite(f) ? f : fallback; }

// Clamps for the glasses: price >= 0.01 (exactly 0 shows "-"), a fixed 0-100
// chart for the percentage metrics, 0 <= low < high, each real point inside
// [low, high] (a negative point or one outside the range would drop the
// chart), the last point = the value shown, change finite and bounded, trend
// 1 up / 2 down / 0 flat, label <= 4 characters.
//
// The chart is the full kFeedPoints window, right-anchored: the newest
// min(historyCount, kFeedPoints) history points at the right end (the last
// one replaced by the value shown), the left padded per `pad`. With no
// history yet the chart is the value alone (one point, no padding).
//
// `in.history` may point into `out.points` (the feed reads the sampler's
// history straight into the slot): the points are moved in place, and no
// field is reset before they are read.
inline bool makeSlot(const MetricInput& in, Pad pad, Slot& out) {
  const system_metrics_core::MetricInfo* m = system_metrics_core::info(in.id);
  if (!m || !in.available) return false;
  const bool percent = m->unit[0] == '%' && m->unit[1] == '\0';
  float low = percent ? 0.0f : finiteOr(in.rangeLow, 0.0f);
  float high = percent ? 100.0f : finiteOr(in.rangeHigh, 0.0f);
  if (low < 0.0f) low = 0.0f;
  float value = finiteOr(in.value, 0.0f);
  if (value < kMinShownValue) value = kMinShownValue;
  if (value > kMaxShownValue) value = kMaxShownValue;
  if (percent && value > 100.0f) value = 100.0f;
  if (high > kMaxShownValue) high = kMaxShownValue;
  if (value > high) high = value;
  if (low > value) low = 0.0f;
  if (!(high > low)) high = low + 1.0f;

  // Points first: in.history may alias out.points.
  const uint16_t count = in.history ? in.historyCount : 0;
  const uint8_t n = (uint8_t)(count < kFeedPoints ? count : kFeedPoints);
  if (n == 0) {
    out.points[0] = value;
    out.pointCount = 1;
    out.realPoints = 0;
  } else {
    float* dst = out.points + (kFeedPoints - n);
    memmove(dst, in.history + (count - n), (size_t)n * sizeof(float));
    for (uint8_t i = 0; i < n; i++) {
      float p = finiteOr(dst[i], low);
      if (p < low) p = low;
      if (p > high) p = high;
      dst[i] = p;
    }
    dst[n - 1] = value;
    const float fill = pad == Pad::Flat ? dst[0] : gapPoint();
    for (uint8_t i = 0; i < kFeedPoints - n; i++) out.points[i] = fill;
    out.pointCount = kFeedPoints;
    out.realPoints = n;
  }

  copyText(out.code, sizeof out.code, m->label);
  copyText(out.name, sizeof out.name, m->longName);
  out.value = value;
  out.low = low;
  out.high = high;
  float change = finiteOr(in.delta, 0.0f);
  if (change > kMaxChange) change = kMaxChange;
  if (change < -kMaxChange) change = -kMaxChange;
  out.change = change;
  out.trend = in.trend > 0 ? 1 : in.trend < 0 ? 2 : 0;
  return true;
}

inline StockInfo slotInfo(const Slot& s) {
  StockInfo info;
  info.code = s.code;
  info.changePercent = s.change;
  info.price = s.value;
  info.name = s.name;
  info.high = s.high;
  info.low = s.low;
  info.hasChangeCopy = true;  // the app always writes tag 12 = tag 4
  info.trend = s.trend;
  info.pointCount = s.pointCount;
  info.points = s.points;
  return info;
}

inline size_t encodeSlot(const Slot& s, uint8_t total, uint8_t num, uint32_t magic,
                         uint8_t* out, size_t cap) {
  return encodeMessage(slotInfo(s), total, num, magic, out, cap);
}

// Send-on-change key: the batch's metric list, the values the tile shows
// (value and change to two decimals, trend) and the sampler's history-point
// count (digestPoints), so the chart scrolls left with each 20 s point even
// while every shown value is steady. The send interval still caps how often a
// batch goes (one per `g2 stats interval`, 10 s with the page open).
inline uint32_t digestStart() { return 2166136261u; }
inline uint32_t digestAdd(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    h ^= (v >> (8 * i)) & 0xFFu;
    h *= 16777619u;
  }
  return h;
}
inline int32_t hundredths(float f) {
  const float x = f * 100.0f;
  if (x > 2.0e9f) return 2000000000;
  if (x < -2.0e9f) return -2000000000;
  return (int32_t)lroundf(x);
}
inline uint32_t digestSlot(uint32_t h, SystemMetricId id, const Slot& s) {
  h = digestAdd(h, (uint32_t)id);
  h = digestAdd(h, (uint32_t)hundredths(s.value));
  h = digestAdd(h, (uint32_t)hundredths(s.change));
  return digestAdd(h, s.trend);
}
// The current run's history-point count (systemMetricsPointGeneration).
inline uint32_t digestPoints(uint32_t h, uint32_t pointGeneration) {
  return digestAdd(h, pointGeneration);
}


// ------------------------------------------------------------------ page and version

// The expanded stock page counts as open this long after the last type-3
// page-state push. The glasses repeat type 3 after each scroll and report
// leaving only when it is closed by a tap; a time-out of the display or a
// head-down does not report it, and the 10 s cadence must not outlive that.
static constexpr uint32_t kPageOpenHoldMs = 90000;

inline bool pageOpenCurrent(bool open, uint32_t openedAtMs, uint32_t nowMs) {
  return open && (uint32_t)(nowMs - openedAtMs) < kPageOpenHoldMs;
}

// The version gate needs the right temple's firmware string, which a sid
// 0x09 settings push or reply fills. If none has come this long after the
// link came up, the feed asks once per link (a BASIC settings request).
static constexpr uint32_t kFirmwareQueryDelayMs = 5000;

// ------------------------------------------------------------------ batch

// One batch on the wire: slots 0..count-1, one message each, the next only
// after the previous one's ack (the app's own pacing). The glasses count
// stock messages and redraw when the count reaches stock_total; update_stock
// resets the count only on a different stock_total, an overflow or a full
// count. A batch cut short (lost ack, failed write, abort) leaves the count
// part-way, and a restart with the same total would redraw early and keep
// that offset in every later batch. So a batch that follows an unfinished one
// on the same link starts with a clear (stock_total 0 resets the count; the
// tile stays blank until that batch completes, a second or two) and writes
// slot 0 only after the clear's ack. A new link needs none: the session
// prelude written at every connect is already a clear.
enum class Step : uint8_t { Wait, Clear, Slot, Stop };

struct Batch {
  uint8_t count = 0;
  uint8_t next = 0;           // next slot to write
  uint8_t acked = 0;          // slots 0..acked-1 echoed
  uint8_t first = 0;          // magic of slot 0
  uint8_t clearMagic = 0;     // magic of the leading clear
  bool clearFirst = false;    // the leading clear is still to be written
  bool clearAwaited = false;  // it was written; slot 0 waits for its ack
  bool wrote = false;         // a message of this batch reached the radio
  bool active = false;
  uint32_t generation = 0;
};

inline void batchBegin(Batch& b, uint8_t count, uint8_t first, uint32_t generation,
                       bool clearFirst, uint8_t clearMagic) {
  b = Batch{};
  b.count = count;
  b.first = first;
  b.generation = generation;
  b.clearFirst = clearFirst;
  b.clearMagic = clearMagic;
  b.active = count > 0 && count <= kMaxSlots && generation != 0;
}

// What to write now. `unitInFlight`: the send policy still waits for this
// batch's acks (false once it completed, timed out or was given up).
inline Step batchStep(const Batch& b, uint32_t generation, bool unitInFlight) {
  if (!b.active) return Step::Wait;
  if (b.generation != generation) return Step::Stop;
  if (b.wrote && !unitInFlight) return Step::Stop;
  if (b.clearFirst) return Step::Clear;
  if (b.clearAwaited || b.next >= b.count || b.next > b.acked) return Step::Wait;
  return Step::Slot;
}

// The write for `step` succeeded.
inline void batchWrote(Batch& b, Step step) {
  b.wrote = true;
  if (step == Step::Clear) {
    b.clearFirst = false;
    b.clearAwaited = true;
  } else if (step == Step::Slot) {
    b.next++;
  }
}

inline void batchStop(Batch& b) { b.active = false; }

// The leading clear's ack: slot 0 may go. False if it is not that ack.
inline bool batchClearAcked(Batch& b, uint32_t generation, uint8_t magic) {
  if (!b.active || !b.clearAwaited || generation != b.generation || magic != b.clearMagic)
    return false;
  b.clearAwaited = false;
  return true;
}

// Slot `index` (0-based, of the unit in flight) was echoed.
inline void batchSlotAcked(Batch& b, int index) {
  if (!b.active || index < 0 || index >= b.count) return;
  if ((uint8_t)(index + 1) > b.acked) b.acked = (uint8_t)(index + 1);
}

// The glasses' received count on one link: part-way from the first slot
// write of a batch until that batch is fully acked or a clear is acked.
struct GlassesCount {
  uint32_t partwayGeneration = 0;  // 0 = at rest
};

inline void countSlotWritten(GlassesCount& c, uint32_t generation) { c.partwayGeneration = generation; }
inline void countReset(GlassesCount& c) { c.partwayGeneration = 0; }
inline bool countPartway(const GlassesCount& c, uint32_t generation) {
  return generation != 0 && c.partwayGeneration == generation;
}

}  // namespace hw1_g2_stock_feed

#endif  // G2_STOCK_FEED_CORE_H
