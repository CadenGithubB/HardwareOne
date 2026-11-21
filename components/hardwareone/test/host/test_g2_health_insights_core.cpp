// Host tests for G2_HealthInsightsCore.h: the stock G2 Health panel's insights
// page (sid 0x0E, cmd 4 "mult highlight") fed from ring history.
//
// The one-item golden vector is the phone's own message captured on glasses
// 2.3.2.14 (the Even app's generic insight text, magic 0x3f). The ACK vector
// is the glasses' answer to a three-item device test (magic 253).
#include "../../G2_HealthInsightsCore.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace hw1_g2_health_insights;

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    gChecks++;                                                              \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                  \
      gFailures++;                                                          \
    }                                                                       \
  } while (0)

static std::vector<uint8_t> hex(const char* h) {
  std::vector<uint8_t> out;
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
    unsigned v = 0;
    std::sscanf(h + i, "%2x", &v);
    out.push_back((uint8_t)v);
  }
  return out;
}

static std::string toHex(const std::vector<uint8_t>& v) {
  std::string s;
  char b[3];
  for (uint8_t c : v) {
    std::snprintf(b, sizeof b, "%02x", c);
    s += b;
  }
  return s;
}

static std::vector<uint8_t> enc(const Insights& in, uint8_t magic) {
  uint8_t buf[kMaxPbBytes];
  const size_t n = encode(in, magic, buf, sizeof buf);
  return std::vector<uint8_t>(buf, buf + n);
}

static bool valid(const std::vector<uint8_t>& v, uint8_t* magic = nullptr,
                  Insights* out = nullptr) {
  return validateMessagePb(v.data(), v.size(), magic, out);
}

static const char* kCapturedText =
    "Wide variations appeared in HR; choose smoother activity and reduce stress today";
static const char* kCapturedHex =
    "0804103f325c0a56080512505769646520766172696174696f6e7320617070656172656420696e"
    "2048523b2063686f6f736520736d6f6f7468657220616374697669747920616e64207265647563"
    "652073747265737320746f646179180010001801";

static void testGoldenOneItem() {
  Insights in;
  CHECK(addLine(in, kHeartRate, kCapturedText));
  CHECK(std::strlen(kCapturedText) == 80);
  const std::vector<uint8_t> got = enc(in, 0x3f);
  CHECK(toHex(got) == kCapturedHex);
  uint8_t magic = 0;
  Insights back;
  CHECK(valid(got, &magic, &back));
  CHECK(magic == 0x3f);
  CHECK(back.count == 1 && back.lines[0].type == kHeartRate &&
        std::strcmp(back.lines[0].text, kCapturedText) == 0);
  // The captured bytes themselves pass the whitelist.
  CHECK(valid(hex(kCapturedHex)));
}

static void testNoInsights() {
  Insights none;
  CHECK(toHex(enc(none, 0x41)) == "080410413204100018" "03");
  uint8_t magic = 0;
  Insights back;
  back.count = 9;
  CHECK(valid(enc(none, 0x41), &magic, &back));
  CHECK(magic == 0x41 && back.count == 0);
  // A two-byte magic still gives the same body.
  CHECK(toHex(enc(none, 200)) == "080410c801320410001803");
  // Zero magic is never encoded.
  uint8_t buf[16];
  CHECK(encode(none, 0, buf, sizeof buf) == 0);
}

static std::string repeatChar(char c, size_t n) { return std::string(n, c); }

// Three items whose body is 130 bytes: the whole message is 137 bytes and the
// field 6 length needs two varint bytes (82 01). The earlier test builder wrote
// that as one byte and the glasses dropped the message.
static void testVarintLength137() {
  Insights in;
  // Each entry is 0a <len> {08 t 12 <len> text 18 00} = 8 + text bytes.
  const std::string a = repeatChar('a', 34), b = repeatChar('b', 34), c = repeatChar('c', 34);
  CHECK(addLine(in, kHeartRate, a.c_str()));
  CHECK(addLine(in, kSteps, b.c_str()));
  CHECK(addLine(in, kSleep, c.c_str()));
  const std::vector<uint8_t> m = enc(in, 0x10);
  CHECK(m.size() == 137);
  CHECK(m.size() >= 7 && m[4] == 0x32 && m[5] == 0x82 && m[6] == 0x01);
  CHECK(valid(m));
  // The one-byte form (length truncated to 0x02 and the body reinterpreted)
  // must not pass: drop the continuation byte and write the low 7 bits only.
  std::vector<uint8_t> bad = m;
  bad[5] = 0x82 & 0x7F;
  bad.erase(bad.begin() + 6);
  CHECK(!valid(bad));
  // Non-minimal varints are refused (82 01 written as 82 81 00).
  std::vector<uint8_t> padded = m;
  padded[6] = 0x81;
  padded.insert(padded.begin() + 7, 0x00);
  CHECK(!valid(padded));
}

static void testLongItemVarints() {
  Insights in;
  const std::string t = repeatChar('x', 200);
  CHECK(addLine(in, kHrv, t.c_str()));
  const std::vector<uint8_t> m = enc(in, 253);
  // 08 04 10 fd 01 32 <len 2 bytes> 0a <item len 2 bytes> 08 08 12 c8 01 ...
  CHECK(m.size() > 14);
  CHECK(m[2] == 0x10 && m[3] == 0xFD && m[4] == 0x01);
  CHECK(m[5] == 0x32);
  const uint32_t itemLen = 2 + 1 + 2 + 200 + 2;  // 207
  const uint32_t bodyLen = 1 + 2 + itemLen + 4;   // 214
  CHECK(m[6] == (uint8_t)(0x80 | (bodyLen & 0x7F)) && m[7] == (uint8_t)(bodyLen >> 7));
  CHECK(m[8] == 0x0A && m[9] == (uint8_t)(0x80 | (itemLen & 0x7F)) && m[10] == (uint8_t)(itemLen >> 7));
  CHECK(m[11] == 0x08 && m[12] == kHrv && m[13] == 0x12 && m[14] == 0xC8 && m[15] == 0x01);
  CHECK(m.size() == 8 + bodyLen);
  Insights back;
  uint8_t magic = 0;
  CHECK(valid(m, &magic, &back));
  CHECK(magic == 253 && back.count == 1 && back.lines[0].len == 200);
}

static void testThreeItemCap() {
  Insights in;
  CHECK(addLine(in, kHeartRate, "one"));
  CHECK(addLine(in, kSteps, "two"));
  CHECK(addLine(in, kSleep, "three"));
  CHECK(!addLine(in, kSpo2, "four"));
  CHECK(in.count == 3);
  // A caller that forces a fourth line gets nothing to send.
  Insights four = in;
  four.count = 4;
  uint8_t buf[kMaxPbBytes];
  CHECK(encodeBody(four, buf, sizeof buf) == 0);
  CHECK(encode(four, 5, buf, sizeof buf) == 0);
  // A hand-built four-item message (the rejected device test) fails the
  // whitelist.
  std::vector<uint8_t> m = enc(in, 5);
  CHECK(valid(m));
  const std::vector<uint8_t> item = hex("0a0a08061204666f75721800");  // type 6 "four"
  std::vector<uint8_t> body(m.begin() + 6, m.end());
  body.insert(body.end() - 4, item.begin(), item.end());
  std::vector<uint8_t> m4 = {0x08, 0x04, 0x10, 0x05, 0x32, (uint8_t)body.size()};
  CHECK(body.size() < 128);
  m4.insert(m4.end(), body.begin(), body.end());
  CHECK(!valid(m4));
  // The same construction with three items does pass (the check above is
  // about the count, not the splice).
  std::vector<uint8_t> body3(m.begin() + 6, m.end());
  std::vector<uint8_t> m3 = {0x08, 0x04, 0x10, 0x05, 0x32, (uint8_t)body3.size()};
  m3.insert(m3.end(), body3.begin(), body3.end());
  CHECK(valid(m3));
}

static void testWhitelistRejects() {
  Insights in;
  CHECK(addLine(in, kSteps, "Steps today 3,100"));
  const std::vector<uint8_t> good = enc(in, 7);
  CHECK(valid(good));
  auto mutate = [&](size_t i, uint8_t v) {
    std::vector<uint8_t> m = good;
    m[i] = v;
    return m;
  };
  CHECK(!valid(mutate(1, 0x02)));     // cmd 2
  CHECK(!valid(mutate(3, 0x00)));     // magic 0
  CHECK(!valid(mutate(9, 0x09)));     // type 9 is not drawn
  CHECK(!valid(mutate(9, 0x01)));     // type 1 neither
  CHECK(!valid(mutate(12, 0x00)));    // NUL in the text
  CHECK(!valid(mutate(12, 0xC3)));    // non-ASCII
  CHECK(!valid(mutate(good.size() - 1, 0x03)));  // status 3 with items
  CHECK(!valid(mutate(good.size() - 3, 0x01)));  // field 2 not 0
  std::vector<uint8_t> trailing = good;
  trailing.push_back(0x00);
  CHECK(!valid(trailing));
  std::vector<uint8_t> shortMsg(good.begin(), good.end() - 1);
  CHECK(!valid(shortMsg));
  // An item with f3 != 0.
  std::vector<uint8_t> f3 = good;
  f3[good.size() - 5] = 0x01;
  CHECK(!valid(f3));
  // Empty text is never built.
  Line l;
  CHECK(!setLine(l, kSteps, ""));
  CHECK(!setLine(l, 9, "x"));
  CHECK(!valid(std::vector<uint8_t>()));
}

static void testTextClamp() {
  Insights in;
  const std::string t = repeatChar('y', 300);
  CHECK(addLine(in, kSleep, t.c_str()));
  CHECK(in.lines[0].len == 255 && std::strlen(in.lines[0].text) == 255);
  Line l;
  CHECK(setLine(l, kSteps, "a\xe2\x80\x93z\tb\n c"));
  CHECK(std::string(l.text) == "a???z?b\n c");
  // 3 x 255 bytes still encodes, validates and fits kMaxPbBytes.
  Insights big;
  CHECK(addLine(big, kHeartRate, t.c_str()));
  CHECK(addLine(big, kSteps, t.c_str()));
  CHECK(addLine(big, kSleep, t.c_str()));
  const std::vector<uint8_t> m = enc(big, 255);
  CHECK(m.size() == kMaxPbBytes);
  CHECK(valid(m));
}

static void testAckParse() {
  Ack a;
  const std::vector<uint8_t> ack = hex("080410FD01320E0A0208050A0208020A0208041801");
  CHECK(parseAck(ack.data(), ack.size(), a));
  CHECK(a.magic == 253 && a.itemCount == 3 && a.types[0] == 5 && a.types[1] == 2 &&
        a.types[2] == 4 && a.haveStatus && a.status == 1);
  const std::vector<uint8_t> none = hex("0804104132021803");
  CHECK(parseAck(none.data(), none.size(), a));
  CHECK(a.magic == 0x41 && a.itemCount == 0 && a.haveStatus && a.status == 3);
  // The one-item ACK from the capture: 32 06 0a 02 08 05 18 01.
  const std::vector<uint8_t> one = hex("0804103f32060a0208051801");
  CHECK(parseAck(one.data(), one.size(), a));
  CHECK(a.magic == 0x3f && a.itemCount == 1 && a.types[0] == 5 && a.status == 1);
  // MULT_DATA echoes (cmd 2) and truncated bytes are not insight ACKs.
  const std::vector<uint8_t> mult = hex("0802102a220408011800");
  CHECK(!parseAck(mult.data(), mult.size(), a));
  const std::vector<uint8_t> cut = hex("080410FD01320E0A0208");
  CHECK(!parseAck(cut.data(), cut.size(), a));

  Policy p;
  CHECK(policyTick(p, true, true, true, 7, 0, 1) == hw1_g2_feed::Action::None);
  CHECK(policyTick(p, true, true, true, 7, kConnectSettleMs, 1) == hw1_g2_feed::Action::Send);
  hw1_g2_feed::sent(p, kConnectSettleMs, 253);
  Ack other;
  other.magic = 252;
  CHECK(!policyAck(p, 7, other));
  CHECK(!policyAck(p, 8, a));  // wrong link generation
  parseAck(ack.data(), ack.size(), a);
  CHECK(policyAck(p, 7, a));
  CHECK(!p.awaitingAck && p.haveAcked && p.lastAcked == 1);
}

static void testPolicyTiming() {
  Policy p;
  const uint32_t g = 3;
  // Nothing computed yet: no send, not even after the settle.
  CHECK(policyTick(p, true, false, true, g, 10000, 0) == hw1_g2_feed::Action::None);
  CHECK(policyTick(p, true, true, true, g, 14000, 1) == hw1_g2_feed::Action::None);  // settle from 10000
  CHECK(policyTick(p, true, true, true, g, 15000, 1) == hw1_g2_feed::Action::Send);
  hw1_g2_feed::sent(p, 15000, 9);
  // No echo: one retry after the ack timeout, then nothing for unchanged data.
  CHECK(policyTick(p, true, true, true, g, 15000 + kAckTimeoutMs, 1) == hw1_g2_feed::Action::Send);
  hw1_g2_feed::sent(p, 18000, 10);
  CHECK(policyTick(p, true, true, true, g, 21000, 1) == hw1_g2_feed::Action::None);
  CHECK(policyTick(p, true, true, true, g, 200000, 1) == hw1_g2_feed::Action::None);
  // A new version goes out, but not before the minimum interval.
  Policy q;
  CHECK(policyTick(q, true, true, true, g, 0, 1) == hw1_g2_feed::Action::None);
  CHECK(policyTick(q, true, true, true, g, kConnectSettleMs, 1) == hw1_g2_feed::Action::Send);
  hw1_g2_feed::sent(q, kConnectSettleMs, 11);
  Ack a;
  a.magic = 11;
  CHECK(policyAck(q, g, a));
  CHECK(policyTick(q, true, true, true, g, kConnectSettleMs + 1000, 2) == hw1_g2_feed::Action::None);
  CHECK(policyTick(q, true, true, true, g, kConnectSettleMs + kMinIntervalMs, 2) ==
        hw1_g2_feed::Action::Send);
  // Feed off: nothing automatic.
  Policy r;
  CHECK(policyTick(r, false, true, true, g, 0, 1) == hw1_g2_feed::Action::None);
  CHECK(policyTick(r, false, true, true, g, 60000, 1) == hw1_g2_feed::Action::None);
}

// ------------------------------------------------------------ aggregation

static void testAggregation() {
  MetricAgg a;
  addHour(a, kHrRange, true, 70, 55, 90);
  addHour(a, kHrRange, true, 80, 60, 120);
  addHour(a, kHrRange, false, 200, 30, 220);  // invalid bucket
  addHour(a, kHrRange, true, 250, 40, 250);   // average out of range
  addHour(a, kHrRange, true, 65, 0, 0);       // no min/max: average both ways
  CHECK(a.hours == 3 && a.lo == 55 && a.hi == 120);
  CHECK(meanRounded(a) == 72);  // (70+80+65)/3 = 71.67
  MetricAgg b;
  addHour(b, kHrRange, true, 60, 70, 50);  // min/max on the wrong side
  CHECK(b.lo == 60 && b.hi == 60);
  // Hour-weighted mean across days: 3 hours at 72-ish + 1 hour at 100.
  MetricAgg c;
  addHour(c, kHrRange, true, 100, 95, 130);
  MetricAgg all;
  mergeAgg(all, a);
  mergeAgg(all, c);
  mergeAgg(all, MetricAgg());
  CHECK(all.hours == 4 && all.lo == 55 && all.hi == 130);
  CHECK(meanRounded(all) == 79);  // (70+80+65+100)/4 = 78.75 (a day mean would be 85.8)
  MetricAgg s;
  addHour(s, kSpo2Range, true, 69, 60, 99);  // below 70
  addHour(s, kSpo2Range, true, 97, 92, 101);  // max out of range -> average
  CHECK(s.hours == 1 && s.lo == 92 && s.hi == 97);
  CHECK(meanRounded(MetricAgg()) == 0);
}

static DaySummary day(uint32_t start, bool today) {
  DaySummary d;
  d.dayStart = start;
  d.isToday = today;
  return d;
}

static void hrDay(DaySummary& d, uint16_t avg, uint16_t mn, uint16_t mx, int hours = 1) {
  for (int i = 0; i < hours; i++) addHour(d.hr, kHrRange, true, avg, mn, mx);
}

static void testFormatting() {
  char b[32];
  formatThousands(b, sizeof b, 0);
  CHECK(std::string(b) == "0");
  formatThousands(b, sizeof b, 999);
  CHECK(std::string(b) == "999");
  formatThousands(b, sizeof b, 1000);
  CHECK(std::string(b) == "1,000");
  formatThousands(b, sizeof b, 6400);
  CHECK(std::string(b) == "6,400");
  formatThousands(b, sizeof b, 999999);
  CHECK(std::string(b) == "999,999");
  formatThousands(b, sizeof b, 1234567);
  CHECK(std::string(b) == "1,234,567");
  formatHm(b, sizeof b, 6 * 3600 + 10 * 60);
  CHECK(std::string(b) == "6h10");
  formatHm(b, sizeof b, 7 * 3600 + 4 * 60 + 30);  // rounds up
  CHECK(std::string(b) == "7h05");
  formatHm(b, sizeof b, 59 * 60 + 40);
  CHECK(std::string(b) == "1h00");
}

static void testOwnerLayout() {
  const uint32_t D = 86400;
  DaySummary days[3] = {day(3 * D, true), day(2 * D, false), day(D, false)};
  hrDay(days[0], 70, 52, 120, 2);
  hrDay(days[1], 66, 48, 110, 2);
  hrDay(days[2], 72, 55, 142, 1);
  addHour(days[0].hrv, kHrvRange, true, 40, 22, 80);
  addHour(days[1].hrv, kHrvRange, true, 50, 30, 95);
  setSteps(days[0], true, 3100);   // today: partial, excluded from the range
  setSteps(days[1], true, 4200);
  setSteps(days[2], true, 9800);
  setSleep(days[0], true, 7 * 3600 + 40 * 60);
  setSleep(days[1], true, 6 * 3600 + 10 * 60);
  setSleep(days[2], true, 7 * 3600 + 5 * 60);
  // SpO2 has data too: sleep still wins line 3 (SpO2 is only the fallback).
  addHour(days[0].spo2, kSpo2Range, true, 96, 93, 99);
  addHour(days[1].spo2, kSpo2Range, true, 95, 91, 98);
  const Insights in = buildInsights(days, 3);
  CHECK(in.count == 3);
  CHECK(in.lines[0].type == kHeartRate);
  // HR mean = (70+70+66+66+72)/5 = 68.8 -> 69
  CHECK(std::string(in.lines[0].text) ==
        "HR 48-142 bpm, avg 69\n \nHRV 22-95 ms, avg 45");
  CHECK(in.lines[1].type == kSteps);
  CHECK(std::string(in.lines[1].text) == "Steps 4,200-9,800/day, avg 7,000");
  CHECK(in.lines[2].type == kSleep);
  // (7h40 + 6h10 + 7h05) / 3 = 6h58m20s -> 6h58
  CHECK(std::string(in.lines[2].text) == "Sleep 6h10-7h40, avg 6h58\n \n \nAverages of the past 3 days");
  // Encodes as one fragment-sized message and passes the whitelist.
  const std::vector<uint8_t> m = enc(in, 1);
  CHECK(!m.empty() && m.size() < 232 && valid(m));
}

static void testFallbacks() {
  const uint32_t D = 86400;
  // HR only.
  {
    DaySummary d[2] = {day(2 * D, true), day(D, false)};
    hrDay(d[0], 70, 60, 80);
    hrDay(d[1], 75, 65, 90);
    const Insights in = buildInsights(d, 2);
    CHECK(in.count == 1 && in.lines[0].type == kHeartRate);
    CHECK(std::string(in.lines[0].text) == "HR 60-90 bpm, avg 73\n \n \nAverages of the past 2 days");
  }
  // HRV only: the HRV icon.
  {
    DaySummary d[1] = {day(D, false)};
    addHour(d[0].hrv, kHrvRange, true, 41, 20, 70);
    const Insights in = buildInsights(d, 1);
    CHECK(in.count == 1 && in.lines[0].type == kHrv);
    CHECK(std::string(in.lines[0].text) == "HRV 20-70 ms, avg 41\n \n \nAverages of the past day");
  }
  // No sleep in the window: SpO2 instead; only today has steps.
  {
    DaySummary d[2] = {day(2 * D, true), day(D, false)};
    hrDay(d[0], 70, 60, 80);
    addHour(d[0].spo2, kSpo2Range, true, 97, 94, 99);
    addHour(d[1].spo2, kSpo2Range, true, 95, 92, 98);
    addHour(d[1].spo2, kSpo2Range, true, 96, 93, 99);
    setSteps(d[0], true, 3100);
    setSleep(d[1], true, 0);  // a zero-length night is no night
    const Insights in = buildInsights(d, 2);
    CHECK(in.count == 3);
    CHECK(std::string(in.lines[1].text) == "Steps today 3,100");
    CHECK(in.lines[2].type == kSpo2);
    CHECK(std::string(in.lines[0].text) == "HR 60-80 bpm, avg 70");
    CHECK(std::string(in.lines[2].text) == "SpO2 92-99%, avg 96\n \n \nAverages of the past 2 days");
  }
  // Today only.
  {
    DaySummary d[1] = {day(D, true)};
    hrDay(d[0], 64, 50, 99);
    setSteps(d[0], true, 12345);
    const Insights in = buildInsights(d, 1);
    CHECK(in.count == 2);
    CHECK(std::string(in.lines[0].text) == "HR 50-99 bpm, avg 64");
    CHECK(std::string(in.lines[1].text) == "Steps today 12,345\n \n \nSo far today");
  }
  // One complete day of steps and one night: no range.
  {
    DaySummary d[2] = {day(2 * D, true), day(D, false)};
    setSteps(d[1], true, 6400);
    setSleep(d[0], true, 7 * 3600 + 5 * 60);
    const Insights in = buildInsights(d, 2);
    CHECK(in.count == 2);
    CHECK(in.lines[0].type == kSteps && std::string(in.lines[0].text) == "Steps 6,400/day");
    CHECK(in.lines[1].type == kSleep && std::string(in.lines[1].text) == "Sleep 7h05\n \n \nAverages of the past 2 days");
  }
}

// The window text ends the last line after a gap row only when the whole line
// still fits 255 bytes; otherwise it is dropped and the data is kept whole.
static void testWindowTextLimit() {
  Line l;
  std::string body(244, 'x');
  CHECK(setLine(l, kSleep, body.c_str()));
  CHECK(appendTail(l, "3 days"));  // 244 + 5 (two gap rows) + 6 = 255
  CHECK(l.len == 255 && std::strlen(l.text) == 255 && lineValid(l));
  CHECK(std::string(l.text) == body + "\n \n \n3 days");
  body.assign(245, 'y');
  CHECK(setLine(l, kSleep, body.c_str()));
  CHECK(!appendTail(l, "3 days"));  // 245 + 5 + 6 = 256: dropped
  CHECK(l.len == 245 && std::string(l.text) == body);
  CHECK(!appendTail(l, "") && !appendTail(l, nullptr) && l.len == 245);
  Line empty;
  CHECK(!appendTail(empty, "3 days") && empty.len == 0);
}

static void testWindow() {
  const uint32_t D = 86400;
  DaySummary all[5] = {day(10 * D, true), day(9 * D, false), day(8 * D, false),
                       day(7 * D, false), day(6 * D, false)};
  // Today and day 9 have no data; 8, 7, 6 do.
  hrDay(all[2], 70, 60, 80);
  setSteps(all[3], true, 5000);
  setSleep(all[4], true, 25000);
  DaySummary w[7];
  CHECK(selectWindow(all, 5, 3, w) == 3);
  CHECK(w[0].dayStart == 8 * D && w[1].dayStart == 7 * D && w[2].dayStart == 6 * D);
  CHECK(selectWindow(all, 5, 2, w) == 2 && w[1].dayStart == 7 * D);
  // Shorter than N: only what exists, and the text says so.
  CHECK(selectWindow(all, 5, 7, w) == 3);
  CHECK(selectWindow(all, 3, 3, w) == 1);
  const Insights in = buildInsights(w, 1);
  CHECK(in.count == 1 && std::string(in.lines[0].text) == "HR 60-80 bpm, avg 70\n \n \nAverages of the past day");
  // N is clamped to 1..7.
  CHECK(clampDays(0) == 1 && clampDays(-4) == 1 && clampDays(9) == 7 && clampDays(3) == 3);
  CHECK(selectWindow(all, 5, 0, w) == 1);
  uint8_t n = 0;
  CHECK(parseDaysArg("1", n) && n == 1);
  CHECK(parseDaysArg("7", n) && n == 7);
  CHECK(!parseDaysArg("0", n) && !parseDaysArg("8", n) && !parseDaysArg("12", n) &&
        !parseDaysArg("", n) && !parseDaysArg("x", n) && !parseDaysArg(nullptr, n));
}

// The adapter's merge of the RAM day into the stored days (review findings:
// a stored copy of the RAM day doubled, a stored copy of today counted as a
// complete day, the RAM day moving between scans).
static void testMergeDays() {
  const uint32_t D = 86400;
  const uint32_t today = 10 * D;
  DaySummary out[kMaxDays + 1];
  // RAM day = today, stored = older days (any order): newest first.
  {
    DaySummary ram = day(today, false);
    setSteps(ram, true, 2000);
    DaySummary past[3] = {day(8 * D, false), day(9 * D, true), day(7 * D, false)};
    setSteps(past[0], true, 8000);
    setSteps(past[1], true, 9000);
    setSteps(past[2], true, 7000);
    const size_t n = mergeDays(&ram, past, 3, today, out, kMaxDays + 1);
    CHECK(n == 4);
    CHECK(out[0].dayStart == today && out[0].isToday && out[0].steps == 2000);
    CHECK(out[1].dayStart == 9 * D && !out[1].isToday);  // stale flag cleared
    CHECK(out[2].dayStart == 8 * D && out[3].dayStart == 7 * D);
  }
  // A stored copy of the RAM day (same key, or an hour off for DST) is
  // dropped: the day counts once, with the RAM copy's data.
  {
    DaySummary ram = day(9 * D, false);
    setSteps(ram, true, 5000);
    DaySummary past[3] = {day(9 * D, false), day(9 * D + 3600, false), day(8 * D, false)};
    setSteps(past[0], true, 4000);
    setSteps(past[1], true, 4000);
    setSteps(past[2], true, 6000);
    const size_t n = mergeDays(&ram, past, 3, today, out, kMaxDays + 1);
    CHECK(n == 2);
    CHECK(out[0].dayStart == 9 * D && out[0].steps == 5000 && !out[0].isToday);
    CHECK(out[1].dayStart == 8 * D);
    DaySummary w[kMaxDays];
    const Insights in = buildInsights(w, selectWindow(out, n, 3, w));
    CHECK(in.count == 1 &&
          std::string(in.lines[0].text) == "Steps 5,000-6,000/day, avg 5,500\n \n \nAverages of the past 2 days");
  }
  // The RAM day is older than today and the store holds today's partial day
  // (a backlog sweep, or a reboot): the stored today is the partial day, so
  // its steps are not a complete day, and alone it reads "Today".
  {
    DaySummary ram = day(8 * D, false);
    setSteps(ram, true, 6000);
    DaySummary past[2] = {day(today, false), day(9 * D, false)};
    setSteps(past[0], true, 1200);
    setSteps(past[1], true, 9800);
    const size_t n = mergeDays(&ram, past, 2, today, out, kMaxDays + 1);
    CHECK(n == 3);
    CHECK(out[0].dayStart == today && out[0].isToday);
    CHECK(out[1].dayStart == 9 * D && !out[1].isToday);
    CHECK(out[2].dayStart == 8 * D && !out[2].isToday);
    DaySummary w[kMaxDays];
    Insights in = buildInsights(w, selectWindow(out, n, 3, w));
    CHECK(in.count == 1 &&
          std::string(in.lines[0].text) == "Steps 6,000-9,800/day, avg 7,900\n \n \nAverages of the past 3 days");
    // Today stored under the other DST offset is still today.
    DaySummary dst[1] = {day(today - 3600, false)};
    hrDay(dst[0], 70, 60, 80);
    CHECK(mergeDays(nullptr, dst, 1, today, out, kMaxDays + 1) == 1 && out[0].isToday);
    in = buildInsights(out, 1);
    CHECK(in.count == 1 && std::string(in.lines[0].text) == "HR 60-80 bpm, avg 70\n \n \nSo far today");
  }
  // Days without data are dropped; a RAM day without data does not hide a
  // stored copy; the list is capped, keeping the newest.
  {
    DaySummary ram = day(today, false);  // cleared new day, no data yet
    DaySummary past[9];
    for (int i = 0; i < 9; i++) {
      past[i] = day((9 - i) * D, false);
      if (i != 4) setSteps(past[i], true, 1000u * (i + 1));
    }
    const size_t n = mergeDays(&ram, past, 9, today, out, 3);
    CHECK(n == 3);
    CHECK(out[0].dayStart == 9 * D && out[1].dayStart == 8 * D && out[2].dayStart == 7 * D);
    CHECK(mergeDays(&ram, nullptr, 0, today, out, kMaxDays + 1) == 0);
    CHECK(mergeDays(nullptr, past, 9, today, nullptr, 3) == 0);
  }
  // Scan key: the RAM day moving (or any other input) asks for a rescan.
  {
    ScanKey a;
    std::snprintf(a.peer, sizeof a.peer, "AA:BB:CC:DD:EE:FF");
    a.todayStart = today;
    a.ramDayStart = 9 * D;
    a.ramTz = 60;
    a.committedGeneration = 4;
    ScanKey b = a;
    CHECK(sameScanKey(a, b));
    b.ramDayStart = today;
    CHECK(!sameScanKey(a, b));
    b = a; b.ramTz = 120;
    CHECK(!sameScanKey(a, b));
    b = a; b.todayStart += D;
    CHECK(!sameScanKey(a, b));
    b = a; b.committedGeneration++;
    CHECK(!sameScanKey(a, b));
    b = a; b.peer[0] = 'B';
    CHECK(!sameScanKey(a, b));
  }
}

static void testNoData() {
  const uint32_t D = 86400;
  DaySummary d[2] = {day(2 * D, true), day(D, false)};
  setSteps(d[1], false, 0);
  setSleep(d[1], false, 0);
  addHour(d[1].hr, kHrRange, true, 10, 5, 12);  // implausible: dropped
  CHECK(!d[0].hasData() && !d[1].hasData());
  DaySummary w[7];
  const size_t n = selectWindow(d, 2, 3, w);
  CHECK(n == 0);
  const Insights in = buildInsights(w, n);
  CHECK(in.count == 0);
  CHECK(toHex(enc(in, 0x22)) == "080410223204100018" "03");
  CHECK(buildInsights(nullptr, 3).count == 0);
}

int main() {
  testGoldenOneItem();
  testNoInsights();
  testVarintLength137();
  testLongItemVarints();
  testThreeItemCap();
  testWhitelistRejects();
  testTextClamp();
  testAckParse();
  testPolicyTiming();
  testAggregation();
  testFormatting();
  testOwnerLayout();
  testFallbacks();
  testWindowTextLimit();
  testWindow();
  testMergeDays();
  testNoData();
  if (gFailures) {
    std::fprintf(stderr, "%d of %d checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  std::printf("g2_health_insights_core_tests: %d checks passed\n", gChecks);
  return 0;
}
