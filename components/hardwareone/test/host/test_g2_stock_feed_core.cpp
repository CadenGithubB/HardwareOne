// Host tests for G2_StockFeedCore.h: the glasses' stock widget fed with
// HardwareOne's system metrics (sid 0x01 StockData).
//
// The fixtures (g2_stock_feed_fixture.h) are the Even app's own stock
// messages from a 2.3.2.14 capture, rebuilt with fixed TEST values; the
// encoder must reproduce them byte for byte, envelope frames included.
#include "../../G2_StockFeedCore.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "g2_stock_feed_fixture.h"

using namespace hw1_g2_stock_feed;
namespace feed = hw1_g2_feed;

static int gFailures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                  \
      gFailures++;                                                          \
    }                                                                       \
  } while (0)

using Bytes = std::vector<uint8_t>;

static Bytes hex(const char* h) {
  Bytes out;
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
    unsigned v = 0;
    std::sscanf(h + i, "%2x", &v);
    out.push_back((uint8_t)v);
  }
  return out;
}

static float bitsToFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

// The production envelope (sendPbFragmented, G2_Glasses.cpp): 232-byte pb
// chunks, the same seq on every fragment, CRC-16/CCITT-FALSE of the whole pb
// little-endian on the last one (an extra CRC-only fragment when pb % 232 == 0).
static uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)(d[i] << 8);
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static std::vector<Bytes> frames(const Bytes& pb, uint8_t seq, uint8_t sid, uint8_t flag) {
  std::vector<Bytes> out;
  const size_t chunk = 232;
  size_t count = (pb.size() + chunk - 1) / chunk;
  if (pb.size() % chunk == 0) count++;
  const uint16_t crc = crc16(pb.data(), pb.size());
  size_t off = 0;
  for (size_t i = 0; i < count; i++) {
    const bool last = i + 1 == count;
    const size_t n = std::min(chunk, pb.size() - off);
    Bytes f = {0xAA, 0x21, seq, (uint8_t)(n + (last ? 2 : 0)), (uint8_t)count, (uint8_t)(i + 1), sid, flag};
    f.insert(f.end(), pb.begin() + off, pb.begin() + off + n);
    if (last) {
      f.push_back((uint8_t)(crc & 0xFF));
      f.push_back((uint8_t)(crc >> 8));
    }
    off += n;
    out.push_back(f);
  }
  return out;
}

static Bytes encode(const StockInfo& s, uint8_t total, uint8_t num, uint32_t magic) {
  uint8_t buf[kMaxPbBytes];
  const size_t n = encodeMessage(s, total, num, magic, buf, sizeof buf);
  return Bytes(buf, buf + n);
}

static bool valid(const Bytes& b, Parsed* out = nullptr) {
  return validateMessagePb(b.data(), b.size(), out);
}

// ------------------------------------------------------------- fixtures

static StockInfo testCompany(const char* code, const char* name, float pct, float price,
                             float high, float low, float open, uint32_t trend, float prevClose,
                             const float* points, uint16_t n) {
  StockInfo s;
  s.code = code;
  s.exchange = "XTST";
  s.changePercent = pct;
  s.price = price;
  s.name = name;
  s.high = high;
  s.low = low;
  s.hasOpen = true;
  s.open = open;
  s.hasVolume = true;
  s.volume = 123456;
  s.hasTurnover = true;
  s.turnover = 12345678.5;
  s.hasChangeCopy = true;
  s.trend = trend;
  s.hasPreviousClose = true;
  s.previousClose = prevClose;
  s.points = points;
  s.pointCount = n;
  return s;
}

static void testFixtureSlot0() {
  float pts[184];
  for (int i = 0; i < 184; i++) pts[i] = 100.0f + 0.25f * (float)((7 * i) % 40);
  const StockInfo s = testCompany("TEST.XTST", "Test Company One", 0.25f, 100.25f, 110.0f,
                                  99.5f, 100.5f, 1, 100.0f, pts, 184);
  const Bytes pb = encode(s, 3, 0, 21);
  CHECK(pb == hex(kSlot0Pb));
  CHECK(pb.size() == 851);
  const std::vector<Bytes> f = frames(pb, 21, kSid, 0x20);
  CHECK(f.size() == 4);
  const char* expected[] = {kSlot0Frame1, kSlot0Frame2, kSlot0Frame3, kSlot0Frame4};
  for (size_t i = 0; i < f.size() && i < 4; i++) CHECK(f[i] == hex(expected[i]));
  Parsed p;
  CHECK(valid(pb, &p));
  CHECK(p.magic == 21 && p.total == 3 && p.num == 0 && !p.clear);
  CHECK(std::strcmp(p.code, "TEST.XTST") == 0);
  CHECK(p.price == 100.25f && p.changePercent == 0.25f && p.trend == 1 && p.pointCount == 184);
}

static void testFixtureSmall() {
  const float up[] = {100.0f, 101.5f, 103.0f, 102.25f, 104.0f, 103.5f, 105.0f, 104.5f};
  const StockInfo su = testCompany("TEST.XTST", "Test Company One", bitsToFloat(0x3fba6864u),
                                   104.5f, 106.0f, 99.5f, 100.5f, 1, 103.0f, up, 8);
  const Bytes a = encode(su, 2, 0, 1);
  CHECK(a == hex(kSmallUpPb));
  const std::vector<Bytes> fa = frames(a, 1, kSid, 0x20);
  CHECK(fa.size() == 1 && fa[0] == hex(kSmallUpFrame));

  const float down[] = {102.5f, 102.0f, 101.25f, 101.5f, 100.75f, 100.25f, 100.5f, 100.0f};
  const StockInfo sd = testCompany("TST2.XTST", "Test Company Two", bitsToFloat(0xc01c18fau),
                                   100.0f, 103.0f, 99.5f, 102.25f, 2, 102.5f, down, 8);
  const Bytes b = encode(sd, 2, 1, 2);
  CHECK(b == hex(kSmallDownPb));
  const std::vector<Bytes> fb = frames(b, 2, kSid, 0x20);
  CHECK(fb.size() == 1 && fb[0] == hex(kSmallDownFrame));

  Parsed p;
  CHECK(valid(a, &p) && p.trend == 1 && p.total == 2 && p.num == 0 && p.magic == 1);
  CHECK(valid(b, &p) && p.trend == 2 && p.total == 2 && p.num == 1 && p.magic == 2);
  CHECK(p.changePercent < 0.0f);
}

// ------------------------------------------------------------- whitelist

static Bytes smallMessage(float price = 50.0f, float low = 0.0f, float high = 100.0f,
                          uint32_t trend = 1) {
  static float pts[4];
  pts[0] = 10.0f; pts[1] = 20.0f; pts[2] = 30.0f; pts[3] = price;
  StockInfo s;
  s.code = "CPU";
  s.name = "CPU load";
  s.changePercent = 1.5f;
  s.price = price;
  s.high = high;
  s.low = low;
  s.hasChangeCopy = true;
  s.trend = trend;
  s.points = pts;
  s.pointCount = 4;
  return encode(s, 1, 0, 7);
}

static void testWhitelist() {
  const Bytes good = smallMessage();
  CHECK(!good.empty() && valid(good));
  // Every truncation and one trailing byte are refused.
  for (size_t n = 0; n < good.size(); n++) {
    Bytes t(good.begin(), good.begin() + n);
    CHECK(!valid(t));
  }
  Bytes extra = good;
  extra.push_back(0x00);
  CHECK(!valid(extra));
  // Wrong command, total/num out of range.
  Bytes cmd = good;
  cmd[1] = 0x03;
  CHECK(!valid(cmd));
  // Field values the glasses would mis-draw.
  CHECK(smallMessage(0.0f).empty() || !valid(smallMessage(0.0f)));      // price 0
  CHECK(!valid(smallMessage(-1.0f, -5.0f, 100.0f)));                     // negative
  CHECK(!valid(smallMessage(50.0f, 60.0f, 100.0f)));                     // point below low
  CHECK(!valid(smallMessage(50.0f, 0.0f, 0.0f)));                        // high <= low
  CHECK(!valid(smallMessage(NAN)));
  CHECK(!valid(smallMessage(50.0f, 0.0f, 100.0f, 3)));                   // trend 3

  // stock_total / stock_num limits in the encoder.
  StockInfo s;
  float one = 5.0f;
  s.code = "X";
  s.price = 5.0f;
  s.high = 10.0f;
  s.points = &one;
  s.pointCount = 1;
  uint8_t buf[kMaxPbBytes];
  CHECK(encodeMessage(s, 0, 0, 1, buf, sizeof buf) == 0);
  CHECK(encodeMessage(s, 10, 0, 1, buf, sizeof buf) == 0);
  CHECK(encodeMessage(s, 3, 3, 1, buf, sizeof buf) == 0);
  CHECK(encodeMessage(s, 9, 8, 1, buf, sizeof buf) > 0);
  CHECK(encodeMessage(s, 1, 0, 1, buf, 10) == 0);  // does not fit
  // 185 points pass, 186 are refused.
  float many[186];
  for (float& f : many) f = 5.0f;
  s.points = many;
  s.pointCount = 185;
  const size_t n185 = encodeMessage(s, 1, 0, 1, buf, sizeof buf);
  CHECK(n185 > 0 && validateMessagePb(buf, n185));
  s.pointCount = 186;
  CHECK(encodeMessage(s, 1, 0, 1, buf, sizeof buf) == 0);

  // point_total that disagrees with the packed points.
  Bytes mismatch = good;
  for (size_t i = 0; i + 1 < mismatch.size(); i++) {
    if (mismatch[i] == 0x70 && mismatch[i + 1] == 0x04) {  // 14: point_total = 4
      mismatch[i + 1] = 0x03;
      break;
    }
  }
  CHECK(!valid(mismatch));

  // Strings: control characters and an over-long code.
  StockInfo t;
  float p = 5.0f;
  t.price = 5.0f;
  t.high = 10.0f;
  t.points = &p;
  t.pointCount = 1;
  t.code = "AB\x01";
  CHECK(!valid(encode(t, 1, 0, 1)));
  t.code = "ABCDEFGHIJKLMNOP";  // 16 > 15
  CHECK(!valid(encode(t, 1, 0, 1)));
  t.code = "";
  CHECK(!valid(encode(t, 1, 0, 1)));
  t.code = "OK";
  CHECK(valid(encode(t, 1, 0, 1)));

  // The clear message: StockData {1: 0, 2: 0}.
  uint8_t c[32];
  const size_t cn = encodeClear(5, c, sizeof c);
  CHECK(Bytes(c, c + cn) == hex("08021005220a1a08120612040800" "1000"));
  Parsed parsed;
  CHECK(validateMessagePb(c, cn, &parsed) && parsed.clear && parsed.total == 0 && parsed.magic == 5);
  // With the prelude's magic it is the AppLaunch session prelude payload
  // (G2_Protocol.cpp PRELUDE_F5872), which HardwareOne writes at every connect.
  const size_t pn = encodeClear(kPreludeMagic, c, sizeof c);
  CHECK(Bytes(c, c + pn) == hex("0802109c01220a1a08120612040800" "1000"));
  // Other clear shapes are refused: total only, a non-zero stock_num, a body.
  CHECK(!valid(hex("0802100522081a06120412020800")));
  CHECK(!valid(hex("08021005220a1a08120612040800" "1001")));
  CHECK(!valid(hex("08021005220c1a0a120812060800" "10001a00")));
  // The dashboard layout message (Dashboard_Receive.2) is not a stock message.
  CHECK(!valid(hex("080210f0012217121508041003"
                   "1a0301020320042a040103020230013802")));
}

// ------------------------------------------------------------- echoes

static void testAckAndPageState() {
  const Bytes ack = hex(kAckPb);
  uint8_t magic = 0;
  bool accepted = false;
  CHECK(parseAck(ack.data(), ack.size(), magic, accepted) && magic == 21 && accepted);
  const Bytes ackFrame = hex(kAckFrame);
  CHECK(ackFrame.size() == 8 + ack.size() + 2);
  CHECK(Bytes(ackFrame.begin() + 8, ackFrame.end() - 2) == ack);
  CHECK(crc16(ack.data(), ack.size()) == (uint16_t)(ackFrame[ackFrame.size() - 2] | (ackFrame.back() << 8)));
  // An error result in Dashboard_Respond is not an accepted ack.
  const Bytes err = hex("080110151a021001");
  CHECK(parseAck(err.data(), err.size(), magic, accepted) && magic == 21 && !accepted);
  // Bare {1, 2} counts as an ack too.
  const Bytes bare = hex("08011015");
  CHECK(parseAck(bare.data(), bare.size(), magic, accepted) && accepted);
  // Not acks: our own send, a glasses push (magic 0x12345678).
  const Bytes own = hex(kSmallUpPb);
  CHECK(!parseAck(own.data(), own.size(), magic, accepted));
  const Bytes open = hex(kPageStockOpenPb);
  CHECK(!parseAck(open.data(), open.size(), magic, accepted));

  uint32_t type = 0;
  CHECK(parsePageState(open.data(), open.size(), type) && type == kPageTypeStockExpanded);
  const Bytes dash = hex(kPageDashboardPb);
  CHECK(parsePageState(dash.data(), dash.size(), type) && type == 1);
  CHECK(!parsePageState(ack.data(), ack.size(), type));
  CHECK(!parsePageState(own.data(), own.size(), type));
  for (size_t n = 0; n < open.size(); n++) {
    uint32_t t = 0;
    CHECK(!parsePageState(open.data(), n, t));
  }
  for (const char* fr : {kPageStockOpenFrame, kPageDashboardFrame}) {
    const Bytes f = hex(fr);
    const Bytes pb(f.begin() + 8, f.end() - 2);
    CHECK(f[6] == kSid && f[7] == 0x01);
    CHECK(crc16(pb.data(), pb.size()) == (uint16_t)(f[f.size() - 2] | (f.back() << 8)));
  }
}

// ------------------------------------------------------------- clamps

static MetricInput metric(SystemMetricId id, float value, float delta, int8_t trend,
                          const float* hist, uint8_t n, float lo = 0.0f, float hi = 100.0f) {
  MetricInput in;
  in.id = id;
  in.available = true;
  in.value = value;
  in.delta = delta;
  in.trend = trend;
  in.rangeLow = lo;
  in.rangeHigh = hi;
  in.history = hist;
  in.historyCount = n;
  return in;
}

static constexpr int kLast = kFeedPoints - 1;  // the window's newest point

static void testClamps() {
  Slot s;
  float hist[200];
  for (int i = 0; i < 200; i++) hist[i] = (float)(i % 100);
  // The window is the newest 185; the last point is the value shown.
  CHECK(makeSlot(metric(SystemMetricId::Cpu, 42.5f, 1.25f, 1, hist, 200), Pad::Gap, s));
  CHECK(s.pointCount == kFeedPoints && s.realPoints == kFeedPoints);
  CHECK(s.points[0] == 15.0f && s.points[kLast - 1] == 98.0f && s.points[kLast] == 42.5f);
  CHECK(std::strcmp(s.code, "CPU") == 0 && std::strcmp(s.name, "CPU load") == 0);
  CHECK(s.low == 0.0f && s.high == 100.0f && s.trend == 1 && s.change == 1.25f);
  // 0 -> 0.01 (the glasses draw exactly 0 as "-"), NaN and negatives too.
  CHECK(makeSlot(metric(SystemMetricId::Heap, 0.0f, -0.5f, -1, hist, 3), Pad::Gap, s));
  CHECK(s.value == kMinShownValue && s.trend == 2 && s.points[kLast] == kMinShownValue);
  CHECK(makeSlot(metric(SystemMetricId::Heap, NAN, NAN, 0, hist, 3), Pad::Gap, s));
  CHECK(s.value == kMinShownValue && s.change == 0.0f && s.trend == 0);
  CHECK(makeSlot(metric(SystemMetricId::Temp, -12.0f, 0.0f, 0, hist, 3, -40.0f, 125.0f), Pad::Gap, s));
  CHECK(s.value == kMinShownValue && s.low == 0.0f);
  // Percentages: fixed 0-100 whatever the sampler says, value capped at 100.
  // No history yet: the chart is the value alone, not padded.
  CHECK(makeSlot(metric(SystemMetricId::Psram, 130.0f, 0.0f, 0, hist, 0, 5.0f, 50.0f), Pad::Gap, s));
  CHECK(s.value == 100.0f && s.low == 0.0f && s.high == 100.0f);
  CHECK(s.pointCount == 1 && s.realPoints == 0 && s.points[0] == 100.0f);
  // Negative and out-of-range points are pulled into the chart range.
  const float wild[] = {-5.0f, 250.0f, NAN, 50.0f};
  CHECK(makeSlot(metric(SystemMetricId::Sd, 50.0f, 0.0f, 0, wild, 4), Pad::Gap, s));
  CHECK(s.points[kLast - 3] == 0.0f && s.points[kLast - 2] == 100.0f &&
        s.points[kLast - 1] == 0.0f && s.points[kLast] == 50.0f);
  // Count metrics: range from the sampler, grown to fit the value.
  CHECK(makeSlot(metric(SystemMetricId::Links, 3.0f, 1.0f, 1, nullptr, 0, 0.0f, 0.0f), Pad::Gap, s));
  CHECK(s.low == 0.0f && s.high == 3.0f && s.value == 3.0f);
  CHECK(makeSlot(metric(SystemMetricId::Mesh, 0.0f, 0.0f, 0, nullptr, 0, 0.0f, 0.0f), Pad::Gap, s));
  CHECK(s.high > s.low && s.value == kMinShownValue);
  // CPU clock: MHz, the chart 0..the chip's top clock from the sampler (not
  // the fixed 0-100 of the percentages), grown if the clock is above it.
  const float clkHist[] = {400.0f, 360.0f, 360.0f};
  CHECK(makeSlot(metric(SystemMetricId::Clk, 360.0f, -40.0f, -1, clkHist, 3, 0.0f, 400.0f), Pad::Gap, s));
  CHECK(std::strcmp(s.code, "CLK") == 0 && std::strcmp(s.name, "CPU clock") == 0);
  CHECK(s.value == 360.0f && s.low == 0.0f && s.high == 400.0f);
  CHECK(s.change == -40.0f && s.trend == 2 && s.points[kLast - 2] == 400.0f && s.points[kLast] == 360.0f);
  CHECK(makeSlot(metric(SystemMetricId::Clk, 240.0f, 0.0f, 0, nullptr, 0, 0.0f, 160.0f), Pad::Gap, s));
  CHECK(s.value == 240.0f && s.high == 240.0f);
  // Change bounded.
  CHECK(makeSlot(metric(SystemMetricId::HeapBlock, 10.0f, 1.0e7f, 1, nullptr, 0, 0.0f, 512.0f), Pad::Gap, s));
  CHECK(s.change == kMaxChange && s.high == 512.0f);
  // Unavailable metrics are never shown.
  MetricInput off = metric(SystemMetricId::Cpu1, 5.0f, 0.0f, 0, nullptr, 0);
  off.available = false;
  CHECK(!makeSlot(off, Pad::Gap, s));
  // Every metric's slot fits the feed buffer and passes the whitelist, labels
  // <= 4: a part-filled window in either pad mode, and a full one.
  for (const auto& m : system_metrics_core::kMetrics) {
    float h[kFeedPoints];
    for (int i = 0; i < kFeedPoints; i++) h[i] = (float)(i % 7) * 3.0f;
    for (uint8_t real : {(uint8_t)60, (uint8_t)kFeedPoints}) {
      for (Pad pad : {Pad::Gap, Pad::Flat}) {
        Slot slot;
        CHECK(makeSlot(metric(m.id, 12.345f, -0.5f, -1, h, real, 0.0f, 64.0f), pad, slot));
        CHECK(std::strlen(slot.code) <= kFeedCodeBytes && std::strlen(slot.code) > 0);
        uint8_t buf[kFeedPbBytes];
        const size_t n = encodeSlot(slot, kMaxSlots, kMaxSlots - 1, 255, buf, sizeof buf);
        CHECK(n > 0 && n <= kFeedPbBytes);
        CHECK(n > 3 * 232 && n < 4 * 232);  // 4 fragments
        Parsed p;
        CHECK(validateMessagePb(buf, n, &p, pad == Pad::Gap) && p.pointCount == kFeedPoints &&
              p.total == 9 && p.num == 8);
        CHECK(p.padCount == (pad == Pad::Gap ? kFeedPoints - real : 0));
        CHECK(std::strcmp(p.code, m.label) == 0);
        // A gap-padded window passes only with the gap flag.
        CHECK(validateMessagePb(buf, n) == (pad == Pad::Flat || real == kFeedPoints));
      }
    }
  }
  // The largest slot the feed can build (31-byte name, magic >= 128) fits.
  Slot big;
  float h[kFeedPoints];
  for (int i = 0; i < kFeedPoints; i++) h[i] = 1.0f;
  CHECK(makeSlot(metric(SystemMetricId::Cpu, 50.0f, 0.0f, 0, h, kFeedPoints), Pad::Gap, big));
  std::memcpy(big.code, "ABCD", 5);
  std::memset(big.name, 'N', kFeedNameBytes);
  big.name[kFeedNameBytes] = '\0';
  uint8_t buf[kFeedPbBytes];
  const size_t bigLen = encodeSlot(big, kMaxSlots, 0, 200, buf, sizeof buf);
  CHECK(bigLen == 837 && bigLen <= kFeedPbBytes && validateMessagePb(buf, bigLen));
}

// ------------------------------------------------------------- window

static Slot slotOf(const float* hist, uint8_t n, float value, Pad pad) {
  Slot s;
  CHECK(makeSlot(metric(SystemMetricId::Cpu, value, 0.0f, 0, hist, n), pad, s));
  return s;
}

static Bytes encoded(const Slot& s, uint8_t magic = 7) {
  uint8_t buf[kFeedPbBytes];
  const size_t n = encodeSlot(s, 3, 0, magic, buf, sizeof buf);
  return Bytes(buf, buf + n);
}

static void putPoint(Bytes& b, int index, uint32_t bits) {
  const size_t at = b.size() - 4u * kFeedPoints + 4u * (size_t)index;
  for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(bits >> (8 * i));
}

static void testWindow() {
  // Gap: one real point, 184 canonical NaN on the left, the value at the right.
  const float one[] = {40.0f};
  Slot s = slotOf(one, 1, 42.0f, Pad::Gap);
  CHECK(s.pointCount == kFeedPoints && s.realPoints == 1);
  for (int i = 0; i < kLast; i++) CHECK(isGapPoint(s.points[i]));
  CHECK(s.points[kLast] == 42.0f);
  Bytes pb = encoded(s);
  CHECK(pb == hex(kWindowGap1Pb));  // pinned: this encoder, 2026-11-20
  Parsed p;
  CHECK(valid(pb) == false);
  CHECK(validateMessagePb(pb.data(), pb.size(), &p, true) && p.padCount == kLast &&
        p.pointCount == kFeedPoints && p.price == 42.0f);
  // The packed points are the window's last bytes: 184 x 0000c07f, then 42.0.
  for (int i = 0; i < kLast; i++) {
    const size_t at = pb.size() - 4u * kFeedPoints + 4u * (size_t)i;
    CHECK(pb[at] == 0x00 && pb[at + 1] == 0x00 && pb[at + 2] == 0xC0 && pb[at + 3] == 0x7F);
  }
  CHECK(frames(pb, 7, kSid, 0x20).size() == 4);

  // Flat: the oldest real point repeated on the left; no NaN anywhere.
  const float three[] = {10.0f, 20.0f, 30.0f};
  s = slotOf(three, 3, 35.0f, Pad::Flat);
  CHECK(s.pointCount == kFeedPoints && s.realPoints == 3);
  for (int i = 0; i < kFeedPoints - 3; i++) CHECK(s.points[i] == 10.0f);
  CHECK(s.points[kLast - 2] == 10.0f && s.points[kLast - 1] == 20.0f && s.points[kLast] == 35.0f);
  pb = encoded(s);
  CHECK(pb == hex(kWindowFlat3Pb));  // pinned: this encoder, 2026-11-20
  CHECK(valid(pb, &p) && p.padCount == 0 && p.pointCount == kFeedPoints);
  // Gap with the same history: same bytes up to the points.
  const Bytes gap = encoded(slotOf(three, 3, 35.0f, Pad::Gap));
  CHECK(gap.size() == pb.size());
  CHECK(Bytes(gap.begin(), gap.end() - 4 * kFeedPoints) == Bytes(pb.begin(), pb.end() - 4 * kFeedPoints));
  CHECK(validateMessagePb(gap.data(), gap.size(), &p, true) && p.padCount == kFeedPoints - 3);

  // A full window is what encodeMessage (byte-exact against the capture)
  // writes for those 185 points: no padding, no NaN, either mode.
  float full[kFeedPoints];
  for (int i = 0; i < kFeedPoints; i++) full[i] = (float)(i % 50) + 0.5f;
  const Slot f = slotOf(full, kFeedPoints, 77.0f, Pad::Gap);
  CHECK(f.realPoints == kFeedPoints);
  StockInfo info = slotInfo(f);
  float expectPts[kFeedPoints];
  std::memcpy(expectPts, full, sizeof full);
  expectPts[kLast] = 77.0f;
  info.points = expectPts;
  CHECK(encoded(f) == encode(info, 3, 0, 7));
  CHECK(encoded(f) == encoded(slotOf(full, kFeedPoints, 77.0f, Pad::Flat)));

  // In place: the history read straight into slot.points gives the same slot.
  for (uint8_t n : {(uint8_t)1, (uint8_t)3, (uint8_t)100, (uint8_t)kFeedPoints}) {
    for (Pad pad : {Pad::Gap, Pad::Flat}) {
      const Slot ref = slotOf(full, n, 33.0f, pad);
      Slot in;
      std::memcpy(in.points, full, sizeof(float) * n);
      CHECK(makeSlot(metric(SystemMetricId::Cpu, 33.0f, 0.0f, 0, in.points, n), pad, in));
      CHECK(encoded(in) == encoded(ref));
      CHECK(in.realPoints == n && in.pointCount == kFeedPoints);
    }
  }
}

static void testGapWhitelist() {
  const float three[] = {10.0f, 20.0f, 30.0f};
  const Bytes good = encoded(slotOf(three, 3, 35.0f, Pad::Gap));
  CHECK(validateMessagePb(good.data(), good.size(), nullptr, true));
  CHECK(!valid(good));                                       // NaN prefix needs the gap flag
  auto ok = [](const Bytes& b) { return validateMessagePb(b.data(), b.size(), nullptr, true); };
  // A gap after a real point.
  Bytes b = good;
  putPoint(b, kLast - 1, kGapBits);
  CHECK(!ok(b));
  // Another NaN, a negative NaN, infinity in the prefix.
  b = good;
  putPoint(b, 0, 0x7FC00001u);
  CHECK(!ok(b));
  b = good;
  putPoint(b, 5, 0xFFC00000u);
  CHECK(!ok(b));
  b = good;
  putPoint(b, 0, 0x7F800000u);
  CHECK(!ok(b));
  // A real point inside the prefix ends it: a gap after it is refused.
  b = good;
  putPoint(b, 10, 0x41200000u);  // 10.0
  CHECK(!ok(b));
  b = good;
  putPoint(b, 0, 0x41200000u);   // a real first point, then gaps
  CHECK(!ok(b));
  // Every point a gap: nothing real to draw.
  b = good;
  for (int i = kLast - 2; i <= kLast; i++) putPoint(b, i, kGapBits);
  CHECK(!ok(b));
  // A gap prefix on a window shorter than 185.
  StockInfo t;
  float pts[4] = {gapPoint(), gapPoint(), 5.0f, 6.0f};
  t.code = "CPU";
  t.price = 6.0f;
  t.high = 100.0f;
  t.points = pts;
  t.pointCount = 4;
  const Bytes shortGap = encode(t, 1, 0, 1);
  CHECK(!shortGap.empty() && !ok(shortGap) && !valid(shortGap));
  // Flat windows and the fixtures still pass without the flag.
  CHECK(valid(encoded(slotOf(three, 3, 35.0f, Pad::Flat))));
  CHECK(valid(hex(kSlot0Pb)) && valid(hex(kSmallUpPb)) && valid(hex(kSmallDownPb)));
  CHECK(ok(hex(kSlot0Pb)) && ok(hex(kSmallUpPb)));

  // `g2 stats pad`.
  Pad pad = Pad::Flat;
  CHECK(parsePad("gap", pad) && pad == Pad::Gap);
  CHECK(parsePad(" FLAT ", pad) && pad == Pad::Flat);
  CHECK(!parsePad("", pad) && !parsePad("gaps", pad) && !parsePad("flat x", pad));
  CHECK(!parsePad(nullptr, pad) && !parsePad("2", pad) && !parsePad("01", pad));
  // The settings editors send the row's option value.
  CHECK(parsePad("1", pad) && pad == Pad::Flat);
  CHECK(parsePad(" 0 ", pad) && pad == Pad::Gap);
  CHECK(padFromSetting(0) == Pad::Gap && padFromSetting(1) == Pad::Flat &&
        padFromSetting(7) == Pad::Gap);
  CHECK(std::strcmp(padName(Pad::Gap), "gap") == 0 && std::strcmp(padName(Pad::Flat), "flat") == 0);
}

// ------------------------------------------------------------- history across links

// The feed's sampler consumer (sf::samplerWanted) against the sampler core's
// run and history clock: a glasses disconnect keeps the run (and the hour of
// history the reconnect sends); feed off ends it, and the next start is empty.
struct FeedSampler {
  system_metrics_core::Consumers consumers{};
  system_metrics_core::Sampler sampler{};
  system_metrics_core::Series series{};
  bool running = false;
  uint8_t handle = 0;
  float v = 1.0f;

  void lap(uint32_t now, bool feedOn, bool warm, bool pageOpen, bool linkOpen) {
    namespace smc = system_metrics_core;
    // updateConsumer (G2_StockFeed.cpp): the link is not an input to wanting.
    if (!samplerWanted(feedOn, warm)) {
      if (handle) smc::consumerClose(consumers, handle);
      handle = 0;
    } else {
      const uint32_t ms = samplerIntervalMs(pageOpen && linkOpen, warm, 60);
      if (!handle) handle = smc::consumerOpen(consumers, ms);
      else smc::consumerSetInterval(consumers, handle, ms);
    }
    // systemMetricsTick (CPU warm-up left out).
    const uint32_t refresh = smc::effectiveInterval(consumers);
    const smc::RunStep step = smc::runStep(running, refresh != 0);
    if (step == smc::RunStep::Idle) return;
    if (step == smc::RunStep::Start) {
      smc::seriesClear(series);
      sampler = smc::Sampler{};
    }
    const smc::Plan p = smc::plan(sampler, now, refresh);
    if (!p.sample) return;
    smc::sampleTaken(sampler, p, now);
    smc::seriesSample(series, true, v, p.point, p.refresh, 0.0f);
    v += 1.0f;
  }
};

static void testHistoryAcrossLinks() {
  auto* f = new FeedSampler();
  uint32_t t = 0;
  // 30 min on and connected: 90 points.
  for (; t < 30u * 60000u; t += 500) f->lap(t, true, false, false, true);
  CHECK(f->series.history.count == 90);
  // The glasses go away for 20 min (gate Down): the sampler keeps going.
  for (; t < 50u * 60000u; t += 500) f->lap(t, true, false, false, false);
  CHECK(f->running && f->series.history.count == 150);
  // Back: the window the reconnect sends holds all 150, padded on the left.
  float pts[kFeedPoints];
  uint8_t n = system_metrics_core::historyCopy(f->series.history, pts, kFeedPoints);
  Slot s;
  CHECK(makeSlot(metric(SystemMetricId::Links, 500.0f, 0.0f, 0, pts, n, 0.0f, 1000.0f), Pad::Gap, s));
  CHECK(s.realPoints == 150 && s.pointCount == kFeedPoints && isGapPoint(s.points[kFeedPoints - 151]));
  // A page open while disconnected does not speed it up; connected it does
  // (the refresh only: still a point per 20 s).
  f->lap(t, true, false, true, false);
  CHECK(system_metrics_core::effectiveInterval(f->consumers) == 60000);
  for (; t < 60u * 60000u; t += 500) f->lap(t, true, false, true, true);
  CHECK(system_metrics_core::effectiveInterval(f->consumers) == kPageOpenIntervalMs);
  CHECK(f->series.history.count == 180);
  // Past the hour the ring wraps: always the newest 185.
  for (; t < 90u * 60000u; t += 500) f->lap(t, true, false, false, true);
  CHECK(f->series.history.count == kFeedPoints);
  n = system_metrics_core::historyCopy(f->series.history, pts, kFeedPoints);
  CHECK(makeSlot(metric(SystemMetricId::Links, 5000.0f, 0.0f, 0, pts, n, 0.0f, 10000.0f), Pad::Gap, s));
  CHECK(s.realPoints == kFeedPoints);
  for (int i = 1; i < kLast; i++) CHECK(s.points[i] > s.points[i - 1]);
  // Feed off: the consumer closes and the run ends; on again starts empty.
  f->lap(t, false, false, false, true);
  CHECK(!f->running && f->handle == 0);
  t += 500;
  f->lap(t, true, false, false, true);
  CHECK(f->running && f->series.history.count == 1);
  // A preview's warm-up alone keeps a run too; feed off with no warm-up ends it.
  CHECK(samplerWanted(false, true) && samplerWanted(true, false) && !samplerWanted(false, false));
  CHECK(samplerIntervalMs(false, true, 60) == kPageOpenIntervalMs);
  CHECK(samplerIntervalMs(false, false, 120) == 120000 && samplerIntervalMs(false, false, 1) == 60000);
  delete f;
}

// ------------------------------------------------------------- helpers

static void testHelpers() {
  CHECK(firmwareSupported("2.3.2.14"));
  CHECK(!firmwareSupported("2.2.6.10") && !firmwareSupported("") && !firmwareSupported(nullptr));
  CHECK(!firmwareSupported("2.3.2.140"));

  // Unit magics: consecutive, never 0, never the layout message's magic.
  CHECK(unitMagic(0, 1) == 1);
  CHECK(unitMagic(255, 3) == 1);
  CHECK(unitMagic(235, 9) == 241);
  CHECK(unitMagic(239, 1) == 241);
  CHECK(unitMagic(238, 1) == 239);
  CHECK(unitMagic(150, 9) == 157);  // runs over the prelude's magic
  CHECK(unitMagic(155, 1) == 157);
  for (int prev = 0; prev < 256; prev++) {
    for (uint8_t count = 1; count <= kMaxSlots; count++) {
      const uint8_t first = unitMagic((uint8_t)prev, count);
      for (uint8_t i = 0; i < count; i++) {
        const uint8_t m = magicAt(first, i);
        CHECK(m != 0 && m != kLayoutMagic && m != kPreludeMagic);
      }
    }
  }

  // The expanded page counts as open for a while after its last push.
  CHECK(pageOpenCurrent(true, 1000, 1000 + kPageOpenHoldMs - 1));
  CHECK(!pageOpenCurrent(true, 1000, 1000 + kPageOpenHoldMs));
  CHECK(!pageOpenCurrent(false, 1000, 1001));
  CHECK(pageOpenCurrent(true, 0xFFFFFF00u, 0x100u));  // wraps
  CHECK(magicAt(254, 3) == 2);

  // Metric list parsing (the `g2 stats metrics` argument).
  Selection s;
  char err[64];
  CHECK(parseMetricList("cpu,Heap psrm", s, err, sizeof err) && s.count == 3);
  CHECK(s.ids[0] == SystemMetricId::Cpu && s.ids[1] == SystemMetricId::Heap &&
        s.ids[2] == SystemMetricId::Psram);
  CHECK(parseMetricList(" , TEMP ,, ring ", s, err, sizeof err) && s.count == 2);
  CHECK(parseMetricList("DEFAULT", s, err, sizeof err) && s.count == kMaxSlots);
  char text[64];
  formatMetricList(s, text, sizeof text);
  CHECK(std::strcmp(text, kDefaultMetricsText) == 0);
  Selection fromText;
  CHECK(parseMetricList(kDefaultMetricsText, fromText, err, sizeof err) && fromText.count == 9);
  for (uint8_t i = 0; i < 9; i++) CHECK(fromText.ids[i] == kDefaultMetrics[i]);
  // The default (owner, 2026-11-21): the CPU clock in the temperature's place.
  CHECK(std::strcmp(kDefaultMetricsText, "CPU,HEAP,PSRM,SD,LFS,CLK,LINK,MESH,RING") == 0);
  CHECK(kDefaultMetrics[5] == SystemMetricId::Clk);
  for (uint8_t i = 0; i < kMaxSlots; i++) CHECK(kDefaultMetrics[i] != SystemMetricId::Temp);
  CHECK(parseMetricList("clk,temp", fromText, err, sizeof err) && fromText.count == 2 &&
        fromText.ids[0] == SystemMetricId::Clk && fromText.ids[1] == SystemMetricId::Temp);
  // The saved setting: the earlier default's exact text (what a device that
  // never chose a list has saved) reads as the new default; any other list is
  // kept, also one with TEMP; nothing usable is the default.
  auto sameAsDefault = [](const Selection& sel) {
    if (sel.count != kMaxSlots) return false;
    for (uint8_t i = 0; i < kMaxSlots; i++)
      if (sel.ids[i] != kDefaultMetrics[i]) return false;
    return true;
  };
  CHECK(std::strcmp(kPreviousDefaultMetricsText, "CPU,HEAP,PSRM,SD,LFS,TEMP,LINK,MESH,RING") == 0);
  CHECK(sameAsDefault(savedSelection(kPreviousDefaultMetricsText)));
  CHECK(sameAsDefault(savedSelection(kDefaultMetricsText)));
  CHECK(sameAsDefault(savedSelection(nullptr)));
  CHECK(sameAsDefault(savedSelection("")));
  CHECK(sameAsDefault(savedSelection("cpu,foo")));
  CHECK(sameAsDefault(savedSelection("default")));
  Selection custom = savedSelection("CPU,HEAP,PSRM,SD,LFS,TEMP,LINK,MESH");  // 8: the owner's own
  CHECK(custom.count == 8 && custom.ids[5] == SystemMetricId::Temp);
  custom = savedSelection("TEMP,CPU");
  CHECK(custom.count == 2 && custom.ids[0] == SystemMetricId::Temp && custom.ids[1] == SystemMetricId::Cpu);
  custom = savedSelection("CPU,HEAP,PSRM,SD,LFS,TEMP,LINK,RING,MESH");  // reordered: kept
  CHECK(custom.count == 9 && custom.ids[5] == SystemMetricId::Temp && custom.ids[8] == SystemMetricId::Mesh);
  // Saving: any list as formatMetricList writes it, except the previous
  // default chosen again on purpose, saved so that it reads back as chosen.
  char saved[64];
  CHECK(formatSavedMetricList(defaultSelection(), saved, sizeof saved) > 0 &&
        std::strcmp(saved, kDefaultMetricsText) == 0);
  CHECK(sameAsDefault(savedSelection(saved)));
  CHECK(parseMetricList(kPreviousDefaultMetricsText, fromText, err, sizeof err) && fromText.count == 9);
  CHECK(formatSavedMetricList(fromText, saved, sizeof saved) > 0 &&
        std::strcmp(saved, kPreviousDefaultMetricsText) != 0);
  custom = savedSelection(saved);
  CHECK(custom.count == 9 && custom.ids[5] == SystemMetricId::Temp);
  for (uint8_t i = 0; i < 9; i++) CHECK(custom.ids[i] == fromText.ids[i]);
  CHECK(parseMetricList("temp,cpu", fromText, err, sizeof err));
  CHECK(formatSavedMetricList(fromText, saved, sizeof saved) == 8 && std::strcmp(saved, "TEMP,CPU") == 0);
  Selection keep = s;
  CHECK(!parseMetricList("cpu,foo", keep, err, sizeof err) && std::strstr(err, "foo"));
  CHECK(keep.count == kMaxSlots);  // unchanged on failure
  CHECK(!parseMetricList("cpu,CPU", keep, err, sizeof err) && std::strstr(err, "twice"));
  CHECK(!parseMetricList("", keep, err, sizeof err));
  CHECK(!parseMetricList(" , ", keep, err, sizeof err));
  CHECK(!parseMetricList("cpu,cpu0,cpu1,heap,hblk,psrm,sd,lfs,temp,link", keep, err, sizeof err));
  CHECK(std::strstr(err, "9"));
  CHECK(!parseMetricList("defaults", keep, err, sizeof err));
  CHECK(!parseMetricList("cpuheapxx", keep, err, sizeof err));

  // Interval (`g2 stats interval`): seconds, 30..3600.
  uint32_t v = 0;
  CHECK(parseIntervalSec("60", v) && v == 60);
  CHECK(parseIntervalSec(" 30 ", v) && v == 30);
  CHECK(parseIntervalSec("3600", v) && v == 3600);
  CHECK(!parseIntervalSec("29", v) && !parseIntervalSec("3601", v));
  CHECK(!parseIntervalSec("60s", v) && !parseIntervalSec("", v) && !parseIntervalSec("-60", v));
  CHECK(!parseIntervalSec("000000060", v));
  CHECK(clampIntervalSec(0) == kDefaultIntervalSec && clampIntervalSec(90) == 90 &&
        clampIntervalSec(99999) == kMaxIntervalSec);
  CHECK(timing(false, 60).minIntervalMs == 60000 && timing(true, 60).minIntervalMs == 10000);
  CHECK(timing(false, 120).minIntervalMs == 120000 && timing(false, 1).minIntervalMs == 60000);

  // Chart step (`g2 stats step`): 10, 20 or 60 s only.
  uint32_t step = 0;
  for (uint32_t ok : kStepChoicesSec) CHECK(parseStepSec(std::to_string(ok).c_str(), step) && step == ok);
  CHECK(parseStepSec(" 60 ", step) && step == 60);
  step = 7;
  CHECK(!parseStepSec("0", step) && !parseStepSec("15", step) && !parseStepSec("30", step));
  CHECK(!parseStepSec("abc", step) && !parseStepSec("", step) && !parseStepSec(nullptr, step));
  CHECK(!parseStepSec("10s", step) && !parseStepSec("-10", step) && !parseStepSec("600", step));
  CHECK(!parseStepSec("20 60", step) && !parseStepSec("99999999999", step) && step == 7);
  CHECK(kDefaultStepSec == 20);
  CHECK(stepFromSetting(10) == 10 && stepFromSetting(20) == 20 && stepFromSetting(60) == 60);
  CHECK(stepFromSetting(0) == 20 && stepFromSetting(30) == 20 && stepFromSetting(-10) == 20 &&
        stepFromSetting(600) == 20);
  CHECK(std::strstr(kStepChoicesText, "10") && std::strstr(kStepChoicesText, "20") &&
        std::strstr(kStepChoicesText, "60"));
  // The window each step covers (185 points), as status prints it.
  char window[24];
  CHECK(formatWindow(10, window, sizeof window) == 6 && std::strcmp(window, "31 min") == 0);
  formatWindow(20, window, sizeof window);
  CHECK(std::strcmp(window, "1 h 2 min") == 0);
  formatWindow(60, window, sizeof window);
  CHECK(std::strcmp(window, "3 h 5 min") == 0);
  char tiny[4];
  CHECK(formatWindow(60, tiny, sizeof tiny) == 3 && std::strcmp(tiny, "3 h") == 0);  // truncated
  CHECK(formatWindow(60, nullptr, 8) == 0);
  // `g2 stats status`'s chart line: the step, the points and the window.
  char line[112];
  formatChartLine(20, padName(Pad::Gap), line, sizeof line);
  CHECK(std::strcmp(line, "Chart: a point every 20 s (saved), 185 points = the last 1 h 2 min, "
                          "left padding gap (saved)") == 0);
  formatChartLine(60, padName(Pad::Flat), line, sizeof line);
  CHECK(std::strcmp(line, "Chart: a point every 60 s (saved), 185 points = the last 3 h 5 min, "
                          "left padding flat (saved)") == 0);
  formatChartLine(10, padName(Pad::Gap), line, sizeof line);
  CHECK(std::strstr(line, "every 10 s") && std::strstr(line, "the last 31 min"));

  // The digest follows what the tile shows, not the chart.
  Slot a, b;
  const float h1[] = {1.0f, 2.0f}, h2[] = {7.0f, 2.0f};
  makeSlot(metric(SystemMetricId::Cpu, 2.0f, 1.0f, 1, h1, 2), Pad::Gap, a);
  makeSlot(metric(SystemMetricId::Cpu, 2.0f, 1.0f, 1, h2, 2), Pad::Gap, b);
  CHECK(digestSlot(digestStart(), SystemMetricId::Cpu, a) ==
        digestSlot(digestStart(), SystemMetricId::Cpu, b));
  makeSlot(metric(SystemMetricId::Cpu, 2.01f, 1.0f, 1, h1, 2), Pad::Gap, b);
  CHECK(digestSlot(digestStart(), SystemMetricId::Cpu, a) !=
        digestSlot(digestStart(), SystemMetricId::Cpu, b));
  CHECK(digestSlot(digestStart(), SystemMetricId::Cpu, a) !=
        digestSlot(digestStart(), SystemMetricId::Heap, a));
  // ... plus the history-point count: with every shown value steady, a new
  // 20 s point still changes the digest, so the chart scrolls.
  const uint32_t steady = digestAdd(digestSlot(digestStart(), SystemMetricId::Cpu, a), 1);
  CHECK(digestPoints(steady, 41) != digestPoints(steady, 42));
  CHECK(digestPoints(steady, 42) == digestPoints(steady, 42));
}

// ------------------------------------------------------------- gating

// The feed policy as G2_StockFeed.cpp drives it: a batch of N slots is one
// unit, written one message per ack.
static void testPolicy() {
  feed::Policy<uint32_t> p;
  const uint32_t gen = 7;
  uint32_t now = 1000;
  feed::Timing t = timing(false, 60);
  // Settle after connect, then the connect push.
  CHECK(feed::tick(p, t, true, true, true, gen, now, 0xAAu) == feed::Action::None);
  now += kConnectSettleMs;
  CHECK(feed::tick(p, t, true, true, true, gen, now, 0xAAu) == feed::Action::Send);
  uint8_t first = unitMagic(0, 3);
  feed::sent(p, now, first, 3);
  // Serial: ack slot 0, write slot 1 (extend), ack, write slot 2, ack.
  CHECK(feed::onAck(p, gen, kCmdAck, kCmdAck, magicAt(first, 0)) == feed::AckResult::Partial);
  now += 60;
  feed::extend(p, now);
  CHECK(feed::onAck(p, gen, kCmdAck, kCmdAck, magicAt(first, 0)) == feed::AckResult::Ignored);
  CHECK(feed::onAck(p, gen, kCmdAck, kCmdAck, magicAt(first, 1)) == feed::AckResult::Partial);
  now += 60;
  feed::extend(p, now);
  CHECK(feed::onAck(p, gen + 1, kCmdAck, kCmdAck, magicAt(first, 2)) == feed::AckResult::Ignored);
  CHECK(feed::onAck(p, gen, kCmdDashboard, kCmdAck, magicAt(first, 2)) == feed::AckResult::Ignored);
  CHECK(feed::onAck(p, gen, kCmdAck, kCmdAck, magicAt(first, 2)) == feed::AckResult::Complete);
  // Unchanged data is not re-sent; changed data waits for the interval.
  now += 70000;
  CHECK(feed::tick(p, t, true, true, true, gen, now, 0xAAu) == feed::Action::None);
  const uint32_t sentAt = now;
  CHECK(feed::tick(p, t, true, true, true, gen, now, 0xBBu) == feed::Action::Send);
  first = unitMagic(magicAt(first, 2), 3);
  feed::sent(p, sentAt, first, 3);
  CHECK(feed::onAck(p, gen, kCmdAck, kCmdAck, first) == feed::AckResult::Partial);
  // No further ack: one retry after the ack timeout, then wait for a change.
  now += kAckTimeoutMs;
  CHECK(feed::tick(p, t, true, true, true, gen, now, 0xBBu) == feed::Action::Send);
  feed::sent(p, now, unitMagic(magicAt(first, 2), 3), 3);
  now += kAckTimeoutMs;
  CHECK(feed::tick(p, t, true, true, true, gen, now, 0xBBu) == feed::Action::None);
  now += 70000;
  CHECK(feed::tick(p, t, true, true, true, gen, now, 0xBBu) == feed::Action::None);
  // Expanded page open: 10 s cadence for changed data.
  const feed::Timing fast = timing(true, 60);
  CHECK(feed::tick(p, fast, true, true, true, gen, now, 0xCCu) == feed::Action::Send);
  feed::sent(p, now, 9, 1);
  CHECK(feed::onAck(p, gen, kCmdAck, kCmdAck, 9) == feed::AckResult::Complete);
  now += 9000;
  CHECK(feed::tick(p, fast, true, true, true, gen, now, 0xDDu) == feed::Action::None);
  CHECK(feed::tick(p, t, true, true, true, gen, now + 1000, 0xDDu) == feed::Action::None);
  CHECK(feed::tick(p, fast, true, true, true, gen, now + 1000, 0xDDu) == feed::Action::Send);
  // Paused while voice/QuickNote run; a new link generation resends in full.
  CHECK(feed::gate(true, gen, true) == feed::Gate::Paused);
  CHECK(feed::gate(true, 0, false) == feed::Gate::Down);
  feed::linkDown(p);
  now += 1000;
  CHECK(feed::tick(p, t, true, true, true, gen + 1, now, 0xDDu) == feed::Action::None);
  now += kConnectSettleMs;
  CHECK(feed::tick(p, t, true, true, true, gen + 1, now, 0xDDu) == feed::Action::Send);
  // Off or no data: nothing is sent.
  feed::Policy<uint32_t> q;
  CHECK(feed::tick(q, t, false, true, true, 3, 10, 1u) == feed::Action::None);
  CHECK(feed::tick(q, t, false, true, true, 3, 10 + kConnectSettleMs, 1u) == feed::Action::None);
  CHECK(feed::tick(q, t, true, false, true, 3, 20 + kConnectSettleMs, 1u) == feed::Action::None);
}

// ------------------------------------------------------------- batch

// The adapter's sequencing (G2_StockFeed.cpp runBatch/startBatch/OnRx) driven
// against the shared policy: serial slots, a clear ahead of a batch that
// follows an unfinished one, stop on a lost ack, a failed write or a new link.
struct Sim {
  feed::Policy<uint32_t> policy;
  Batch batch;
  GlassesCount count;
  uint8_t magic = 0;
  std::vector<std::string> wire;  // "C<magic>" or "S<slot>"
  bool failNext = false;
  uint32_t gen = 3;

  void start(uint8_t n, uint32_t now) {
    const bool clear = countPartway(count, gen);
    uint8_t clearMagic = 0;
    if (clear) {
      clearMagic = unitMagic(magic, 1);
      magic = clearMagic;
    }
    const uint8_t first = unitMagic(magic, n);
    magic = magicAt(first, (uint8_t)(n - 1));
    batchBegin(batch, n, first, gen, clear, clearMagic);
    run(now);
  }
  // One runBatch step; returns what it did.
  Step run(uint32_t now) {
    const Step step = batchStep(batch, gen, policy.awaitingAck);
    if (step == Step::Wait) return step;
    if (step == Step::Stop) {
      batchStop(batch);
      return step;
    }
    if (failNext) {
      failNext = false;
      feed::unitFailed(policy, now);
      batchStop(batch);
      return Step::Stop;
    }
    wire.push_back(step == Step::Clear ? "C" + std::to_string(batch.clearMagic)
                                       : "S" + std::to_string(batch.next));
    const bool firstWrite = !batch.wrote;
    if (step == Step::Slot) countSlotWritten(count, gen);
    batchWrote(batch, step);
    if (firstWrite) feed::sent(policy, now, batch.first, batch.count);
    else feed::extend(policy, now);
    return step;
  }
  void ackSlot(uint8_t slot) {
    const uint8_t m = magicAt(batch.first, slot);
    const int index = feed::unitIndex(policy, m);
    const feed::AckResult r = feed::onAck(policy, gen, kCmdAck, kCmdAck, m);
    if (r == feed::AckResult::Ignored) return;
    batchSlotAcked(batch, index);
    if (r == feed::AckResult::Complete) {
      batchStop(batch);
      countReset(count);
    }
  }
  void ackClear() {
    if (batchClearAcked(batch, gen, batch.clearMagic)) countReset(count);
  }
};

static void testBatch() {
  // Serial: one slot per ack; a full ack leaves the glasses' count at rest.
  Sim s;
  s.policy.generation = s.gen;
  s.start(3, 100);
  CHECK(s.wire == std::vector<std::string>({"S0"}));
  CHECK(s.run(110) == Step::Wait);  // slot 1 waits for slot 0's ack
  CHECK(s.policy.awaitingAck && s.policy.awaitingCount == 3);
  s.ackSlot(0);
  CHECK(s.run(120) == Step::Slot);
  CHECK(s.run(130) == Step::Wait);
  s.ackSlot(1);
  CHECK(s.run(140) == Step::Slot);
  CHECK(s.run(150) == Step::Wait);
  CHECK(countPartway(s.count, s.gen));
  s.ackSlot(2);
  CHECK(!s.batch.active && !s.policy.awaitingAck && !countPartway(s.count, s.gen));
  CHECK(s.wire == std::vector<std::string>({"S0", "S1", "S2"}));
  CHECK(s.run(160) == Step::Wait);  // nothing after the batch

  // The next batch on a count at rest needs no clear.
  s.wire.clear();
  s.start(3, 70000);
  CHECK(s.wire == std::vector<std::string>({"S0"}));

  // Slot 1's ack is lost: the policy times out, the batch stops, and the
  // retry starts with a clear; slot 0 waits for the clear's ack.
  s.ackSlot(0);
  s.run(70010);
  CHECK(s.wire.back() == "S1");
  const feed::Timing t = timing(false, 60);
  CHECK(feed::tick(s.policy, t, true, true, true, s.gen, 70010 + kAckTimeoutMs, 1u) ==
        feed::Action::Send);
  CHECK(s.run(70010 + kAckTimeoutMs) == Step::Stop);
  s.wire.clear();
  s.start(3, 70010 + kAckTimeoutMs);
  CHECK(s.wire.size() == 1 && s.wire[0][0] == 'C');
  CHECK(s.batch.clearMagic != 0 && s.batch.clearMagic != s.batch.first);
  CHECK(s.run(73100) == Step::Wait);
  s.ackClear();
  CHECK(!countPartway(s.count, s.gen));
  CHECK(s.run(73120) == Step::Slot);
  s.ackSlot(0);
  s.run(73130);
  s.ackSlot(1);
  s.run(73140);
  s.ackSlot(2);
  CHECK(s.wire.size() == 4 && s.wire[1] == "S0" && s.wire[3] == "S2");
  CHECK(!s.batch.active && !countPartway(s.count, s.gen));

  // A later write fails: the batch stops, the same data goes again after the
  // interval, and that batch starts with a clear.
  s.wire.clear();
  s.policy.pending = 7u;
  s.start(2, 200000);
  s.ackSlot(0);
  s.failNext = true;
  CHECK(s.run(200010) == Step::Stop);
  CHECK(!s.batch.active && !s.policy.awaitingAck && !s.policy.haveAttempted);
  CHECK(feed::tick(s.policy, t, true, true, true, s.gen, 200010 + 60000, 7u) ==
        feed::Action::Send);
  s.wire.clear();
  s.start(2, 260010);
  CHECK(s.wire.size() == 1 && s.wire[0][0] == 'C');

  // A new link: the batch stops, and the prelude already reset the count.
  s.gen = 4;
  CHECK(s.run(260020) == Step::Stop);
  CHECK(!countPartway(s.count, s.gen));
  s.policy = feed::Policy<uint32_t>{};
  s.policy.generation = s.gen;
  s.wire.clear();
  s.start(2, 270000);
  CHECK(s.wire == std::vector<std::string>({"S0"}));

  // The first write fails: nothing is registered, nothing waits.
  Sim f;
  f.policy.generation = f.gen;
  f.failNext = true;
  f.start(3, 10);
  CHECK(f.wire.empty() && !f.batch.active && !f.policy.awaitingAck && !countPartway(f.count, f.gen));

  // An echo from another link or with another magic never releases a clear.
  Batch b;
  batchBegin(b, 2, 50, 9, true, 49);
  batchWrote(b, Step::Clear);
  CHECK(!batchClearAcked(b, 8, 49) && !batchClearAcked(b, 9, 50));
  CHECK(batchClearAcked(b, 9, 49) && !batchClearAcked(b, 9, 49));
  // Out-of-range slot acks are ignored; a bad count never starts a batch.
  batchSlotAcked(b, 5);
  batchSlotAcked(b, -1);
  CHECK(b.acked == 0);
  batchBegin(b, 0, 1, 9, false, 0);
  CHECK(!b.active);
  batchBegin(b, kMaxSlots + 1, 1, 9, false, 0);
  CHECK(!b.active);
  batchBegin(b, 2, 1, 0, false, 0);
  CHECK(!b.active);
}

int main() {
  testFixtureSlot0();
  testFixtureSmall();
  testWhitelist();
  testAckAndPageState();
  testClamps();
  testWindow();
  testGapWhitelist();
  testHistoryAcrossLinks();
  testHelpers();
  testPolicy();
  testBatch();
  if (gFailures) {
    std::fprintf(stderr, "g2_stock_feed_core_tests: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("g2_stock_feed_core_tests: all passed\n");
  return 0;
}
