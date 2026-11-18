// Rules of the sensor reading path (System_ESPNow_SensorCore.h, compiled as is).
//
// A sender numbers its readings per sensor and cuts a reading into 1 to 4
// frames; the receiver puts the parts together, checks the text and counts
// what was lost. These cases check:
//   * the one sensor table: a name per type, in order, both ways, and the
//     mask bit of a type;
//   * the 12-byte header: every field at its stated offset, round trip, and a
//     payload that is too short;
//   * planning: the part counts at the frame budgets of a direct and a routed
//     path, every part within its frame, 472 bytes on both paths, 473 refused;
//   * the round trip of a reading through the parts, with a part missing, out
//     of order, repeated, of another reading, left open too long, and hostile
//     counts and lengths (run under ASan);
//   * the counter rule: first, new with and without a gap, repeat, late, the
//     wrap, a restart by boot tag, eight late in a row, the stream-start flag,
//     silence longer than the late window, two receivers of one sender;
//   * the text check, with the text that would forge the receiver's own keys;
//   * where a reading goes (the bond partner, the controller of a running
//     lease, the named master for a stream switched on by hand), who sends
//     and who keeps by mesh role, and that every reason not to send has a name;
//   * the argument of espnowmasterfingerprint / espnowbackupfingerprint: none
//     shows, 'clear' empties, 64 hex characters set, anything else is
//     refused; what the reply to 'clear' says in each of the four cases.
#include "System_ESPNow_SensorCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
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

using namespace espnowsensor;

static bool is(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

static void testSensorTable() {
  static const char* const kNames[] = { "thermal", "tof", "imu", "gps", "input", "fmradio",
                                        "camera", "microphone", "rtc", "presence", "apds" };
  CHECK(REMOTE_SENSOR_MAX == 11);
  CHECK(sizeof(kNames) / sizeof(kNames[0]) == REMOTE_SENSOR_MAX);
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    CHECK(kSensorTable[i].id == i);
    CHECK(is(kSensorTable[i].name, kNames[i]));
    CHECK(is(sensorName(i), kNames[i]));
    CHECK(kSensorTable[i].shortName[0] != '\0' && kSensorTable[i].longName[0] != '\0');
    uint8_t id = 0xFF;
    CHECK(sensorIdFromName(kNames[i], id) && id == i);
    CHECK(sensorBit(i) == (1u << i));
  }
  // The numbers are on the wire.
  CHECK(REMOTE_SENSOR_THERMAL == 0 && REMOTE_SENSOR_TOF == 1 && REMOTE_SENSOR_IMU == 2 &&
        REMOTE_SENSOR_GPS == 3 && REMOTE_SENSOR_INPUT == 4 && REMOTE_SENSOR_FMRADIO == 5 &&
        REMOTE_SENSOR_CAMERA == 6 && REMOTE_SENSOR_MICROPHONE == 7 && REMOTE_SENSOR_RTC == 8 &&
        REMOTE_SENSOR_PRESENCE == 9 && REMOTE_SENSOR_APDS == 10);
  CHECK(kSensorMaskAll == 0x7FFu);
  uint8_t id = 7;
  CHECK(!sensorIdFromName("Thermal", id) && id == 7);  // names are exact
  CHECK(!sensorIdFromName("", id) && !sensorIdFromName(nullptr, id) && !sensorIdFromName("unknown", id));
  CHECK(is(sensorName(-1), "unknown") && is(sensorName(REMOTE_SENSOR_MAX), "unknown") && is(sensorName(255), "unknown"));

  char out[64];
  CHECK(sensorMaskNames(0, false, out, sizeof(out)) == 0 && is(out, ""));
  sensorMaskNames(sensorBit(REMOTE_SENSOR_GPS) | sensorBit(REMOTE_SENSOR_TOF), false, out, sizeof(out));
  CHECK(is(out, "ToF,GPS"));
  sensorMaskNames(sensorBit(REMOTE_SENSOR_INPUT) | sensorBit(REMOTE_SENSOR_APDS), true, out, sizeof(out));
  CHECK(is(out, "Input Device,Gesture/Color"));
  // Bits above the table name nothing; a short buffer is cut at a whole name.
  sensorMaskNames(0xFFFFF800u, false, out, sizeof(out));
  CHECK(is(out, ""));
  char small[8];
  sensorMaskNames(kSensorMaskAll, false, small, sizeof(small));
  CHECK(is(small, "Therm"));
  char one[1];
  CHECK(sensorMaskNames(kSensorMaskAll, false, one, sizeof(one)) == 0 && one[0] == '\0');
}

static void testHeader() {
  CHECK(kReadingHeaderLen == 12);
  CHECK(kPartMaxDirect == 190 && kPartMaxRouted == 118 && kReadingMaxBytes == 472 && kReadingMaxParts == 4);
  ReadingHeader h = {};
  h.sensorType = REMOTE_SENSOR_GPS;
  h.flags = kReadingFlagStreamStart;
  h.bootTag = 0xB1A2;
  h.counter = 0xC3D4;
  h.partIndex = 2;
  h.partCount = 3;
  h.ageMs = 0x11223344u;
  uint8_t raw[kReadingHeaderLen];
  std::memset(raw, 0xEE, sizeof(raw));
  packReadingHeader(raw, h);
  const uint8_t expect[kReadingHeaderLen] = { 3, 0x02, 0xA2, 0xB1, 0xD4, 0xC3, 0x32, 0x00, 0x44, 0x33, 0x22, 0x11 };
  CHECK(std::memcmp(raw, expect, sizeof(raw)) == 0);
  ReadingHeader back = {};
  CHECK(parseReadingHeader(raw, sizeof(raw), back));
  CHECK(back.sensorType == 3 && back.flags == 0x02 && back.bootTag == 0xB1A2 && back.counter == 0xC3D4 &&
        back.partIndex == 2 && back.partCount == 3 && back.ageMs == 0x11223344u);
  CHECK(!parseReadingHeader(raw, kReadingHeaderLen - 1, back));
  CHECK(!parseReadingHeader(nullptr, kReadingHeaderLen, back));
  CHECK(parseReadingHeader(raw, 250, back));
  CHECK(back.fwdLink == 0);
  // Byte 7 is the forwarding bond master's link number (bit 2 of the flags
  // says the reading is its bond partner's); every other byte is unchanged.
  h.flags = (uint8_t)(kReadingFlagStreamStart | kReadingFlagForwarded);
  h.fwdLink = 0xA7;
  packReadingHeader(raw, h);
  CHECK(raw[1] == 0x06 && raw[7] == 0xA7 && raw[6] == 0x32 && raw[8] == 0x44);
  CHECK(parseReadingHeader(raw, sizeof(raw), back) && back.fwdLink == 0xA7 && back.flags == 0x06 &&
        back.counter == 0xC3D4 && back.ageMs == 0x11223344u);
  CHECK(kReadingFlagForwarded == 0x04 && kReadingFlagAgeValid == 0x01 && kReadingFlagStreamStart == 0x02);
}

static void testPlanning() {
  struct Case { size_t len; uint16_t cap; uint8_t parts; };
  const Case cases[] = {
    { 1, kFrameCapDirect, 1 },   { 190, kFrameCapDirect, 1 }, { 191, kFrameCapDirect, 2 },
    { 380, kFrameCapDirect, 2 }, { 381, kFrameCapDirect, 3 }, { 472, kFrameCapDirect, 3 },
    { 1, kFrameCapRouted, 1 },   { 118, kFrameCapRouted, 1 }, { 119, kFrameCapRouted, 2 },
    { 236, kFrameCapRouted, 2 }, { 237, kFrameCapRouted, 3 }, { 354, kFrameCapRouted, 3 },
    { 355, kFrameCapRouted, 4 }, { 472, kFrameCapRouted, 4 },
  };
  for (const Case& c : cases) {
    PartPlan plan;
    CHECK(planParts(c.len, c.cap, plan));
    CHECK(plan.count == c.parts);
    size_t total = 0;
    for (uint8_t i = 0; i < plan.count; ++i) {
      size_t offset = 0, n = 0;
      partSpan(plan, c.len, i, offset, n);
      CHECK(offset == total && n > 0);
      CHECK(kReadingHeaderLen + n <= c.cap);  // every part fits its frame
      total += n;
    }
    CHECK(total == c.len);
    size_t offset = 0, n = 99;
    partSpan(plan, c.len, plan.count, offset, n);
    CHECK(n == 0);
  }
  // One maximum on both paths; over it nothing is planned.
  PartPlan plan;
  CHECK(!planParts(473, kFrameCapDirect, plan) && plan.count == 0);
  CHECK(!planParts(473, kFrameCapRouted, plan));
  CHECK(!planParts(0, kFrameCapDirect, plan));
  CHECK(!planParts(100, kReadingHeaderLen, plan));  // no room for data at all
  CHECK(!planParts(100, 0, plan));
  CHECK(!planParts(100, 20, plan));                 // would need more than four parts
  CHECK(kReadingHeaderLen + kPartMaxRouted == kFrameCapRouted);
}

// A sender for the assembly cases: the parts of `text` as (header, data).
struct Part {
  ReadingHeader h;
  std::vector<uint8_t> data;
};

static std::vector<Part> partsOf(const std::string& text, uint16_t cap, uint8_t sensor, uint16_t tag,
                                 uint16_t counter, uint8_t flags = 0) {
  std::vector<Part> parts;
  PartPlan plan;
  CHECK(planParts(text.size(), cap, plan));
  for (uint8_t i = 0; i < plan.count; ++i) {
    size_t offset = 0, n = 0;
    partSpan(plan, text.size(), i, offset, n);
    Part p;
    p.h = ReadingHeader{};
    p.h.sensorType = sensor;
    p.h.flags = flags;
    p.h.bootTag = tag;
    p.h.counter = counter;
    p.h.partIndex = i;
    p.h.partCount = plan.count;
    p.data.assign(text.begin() + (long)offset, text.begin() + (long)(offset + n));
    parts.push_back(p);
  }
  return parts;
}

static std::string jsonOf(size_t len) {
  std::string s = "{\"k\":\"";
  while (s.size() + 2 < len) s.push_back((char)('a' + (s.size() % 26)));
  s += "\"}";
  CHECK(s.size() == len);
  return s;
}

struct Receiver {
  PartAssembly a;
  // On the heap, exactly as long as stated: a write past it is an ASan error.
  std::vector<uint8_t> buf;
  Receiver() : a{}, buf(kReadingMaxBytes) {}
  PartResult feed(const Part& p, uint32_t nowMs, std::string* text = nullptr) {
    const uint8_t* out = nullptr;
    size_t outLen = 0;
    const PartResult r = partAccept(a, buf.data(), buf.size(), p.h, p.data.data(), p.data.size(), nowMs, &out, &outLen);
    if (r.verdict == PartVerdict::Complete) {
      CHECK(out != nullptr && outLen > 0);
      if (text) text->assign((const char*)out, outLen);
    } else {
      CHECK(out == nullptr && outLen == 0);
    }
    return r;
  }
};

static void testAssembly() {
  // The largest reading, through a relay (4 parts) and direct (3 parts).
  for (uint16_t cap : { kFrameCapRouted, kFrameCapDirect }) {
    const std::string text = jsonOf(kReadingMaxBytes);
    const std::vector<Part> parts = partsOf(text, cap, REMOTE_SENSOR_TOF, 7, 100);
    Receiver rx;
    std::string got;
    for (size_t i = 0; i < parts.size(); ++i) {
      const PartResult r = rx.feed(parts[i], 1000 + (uint32_t)i, &got);
      CHECK(r.incomplete == 0);
      CHECK(r.verdict == (i + 1 == parts.size() ? PartVerdict::Complete : PartVerdict::Stored));
    }
    CHECK(got == text && !rx.a.open);
  }
  // One part: no context is used.
  {
    Receiver rx;
    std::string got;
    const std::vector<Part> one = partsOf(jsonOf(60), kFrameCapDirect, REMOTE_SENSOR_GPS, 7, 1);
    CHECK(one.size() == 1);
    CHECK(rx.feed(one[0], 5, &got).verdict == PartVerdict::Complete && got == jsonOf(60) && !rx.a.open);
  }
  // A middle part is missing: nothing completes, and the reading is given up once.
  {
    Receiver rx;
    const std::vector<Part> parts = partsOf(jsonOf(472), kFrameCapRouted, REMOTE_SENSOR_TOF, 7, 100);
    CHECK(rx.feed(parts[0], 10).verdict == PartVerdict::Stored);
    PartResult r = rx.feed(parts[2], 11);
    CHECK(r.verdict == PartVerdict::Dropped && r.incomplete == 1 && !rx.a.open);
    r = rx.feed(parts[3], 12);
    CHECK(r.verdict == PartVerdict::Dropped && r.incomplete == 0);  // the same reading: counted once
    // The next reading is whole again.
    std::string got;
    const std::vector<Part> next = partsOf(jsonOf(300), kFrameCapRouted, REMOTE_SENSOR_TOF, 7, 101);
    unsigned incomplete = 0;
    for (const Part& p : next) incomplete += rx.feed(p, 20, &got).incomplete;
    CHECK(incomplete == 0 && got == jsonOf(300));
  }
  // The first part is missing: the later parts find no reading to join.
  {
    Receiver rx;
    const std::vector<Part> parts = partsOf(jsonOf(300), kFrameCapRouted, REMOTE_SENSOR_IMU, 7, 5);
    unsigned incomplete = 0;
    for (size_t i = 1; i < parts.size(); ++i) {
      const PartResult r = rx.feed(parts[i], 30);
      CHECK(r.verdict == PartVerdict::Dropped);
      incomplete += r.incomplete;
    }
    CHECK(incomplete == 1);
  }
  // A part repeated, a part out of order, a part of another reading.
  {
    Receiver rx;
    const std::vector<Part> parts = partsOf(jsonOf(472), kFrameCapRouted, REMOTE_SENSOR_TOF, 7, 100);
    CHECK(rx.feed(parts[0], 10).verdict == PartVerdict::Stored);
    CHECK(rx.feed(parts[1], 10).verdict == PartVerdict::Stored);
    PartResult r = rx.feed(parts[1], 11);  // repeated: the reading cannot be trusted any more
    CHECK(r.verdict == PartVerdict::Dropped && r.incomplete == 1 && !rx.a.open);
    Receiver rx2;
    CHECK(rx2.feed(parts[0], 10).verdict == PartVerdict::Stored);
    Part other = parts[1];
    other.h.counter = 101;  // the second part of another reading
    r = rx2.feed(other, 11);
    CHECK(r.verdict == PartVerdict::Dropped && r.incomplete == 1 && rx2.a.open);  // 100 is still open
    Part otherTag = parts[1];
    otherTag.h.bootTag = 8;
    r = rx2.feed(otherTag, 11);
    CHECK(r.verdict == PartVerdict::Dropped && rx2.a.open);
    Part otherCount = parts[1];
    otherCount.h.partCount = 3;  // same reading, another number of parts
    r = rx2.feed(otherCount, 12);
    CHECK(r.verdict == PartVerdict::Dropped && !rx2.a.open);
  }
  // A new reading starts while one is open: the open one is given up.
  {
    Receiver rx;
    const std::vector<Part> a = partsOf(jsonOf(300), kFrameCapRouted, REMOTE_SENSOR_TOF, 7, 1);
    const std::vector<Part> b = partsOf(jsonOf(300), kFrameCapRouted, REMOTE_SENSOR_TOF, 7, 2);
    CHECK(rx.feed(a[0], 10).verdict == PartVerdict::Stored);
    PartResult r = rx.feed(b[0], 11);
    CHECK(r.verdict == PartVerdict::Stored && r.incomplete == 1);
    r = rx.feed(a[1], 12);  // a late part of the one given up: not counted again
    CHECK(r.verdict == PartVerdict::Dropped && r.incomplete == 0);
    std::string got;
    CHECK(rx.feed(b[1], 13).verdict == PartVerdict::Stored);
    CHECK(rx.feed(b[2], 14, &got).verdict == PartVerdict::Complete && got == jsonOf(300));
    // A one-part reading of the same sensor closes an open one; of another sensor it does not.
    CHECK(rx.feed(a[0], 20).verdict == PartVerdict::Stored);
    const std::vector<Part> gps = partsOf(jsonOf(50), kFrameCapRouted, REMOTE_SENSOR_GPS, 7, 9);
    r = rx.feed(gps[0], 21);
    CHECK(r.verdict == PartVerdict::Complete && r.incomplete == 0 && rx.a.open);
    const std::vector<Part> tof = partsOf(jsonOf(50), kFrameCapRouted, REMOTE_SENSOR_TOF, 7, 3);
    r = rx.feed(tof[0], 22);
    CHECK(r.verdict == PartVerdict::Complete && r.incomplete == 1 && !rx.a.open);
  }
  // A reading left open for more than 2 s is given up when the next part of
  // anything arrives, also across the uptime wrap.
  {
    Receiver rx;
    const std::vector<Part> parts = partsOf(jsonOf(300), kFrameCapRouted, REMOTE_SENSOR_TOF, 7, 1);
    CHECK(rx.feed(parts[0], 0xFFFFFF00u).verdict == PartVerdict::Stored);
    PartResult r = rx.feed(parts[1], 0xFFFFFF00u + kAssemblyMaxAgeMs);  // exactly 2 s: still joins
    CHECK(r.verdict == PartVerdict::Stored && r.incomplete == 0);
    r = rx.feed(parts[2], 0xFFFFFF00u + kAssemblyMaxAgeMs + 1);  // wrapped past zero, too late
    CHECK(r.verdict == PartVerdict::Dropped && r.incomplete == 1 && !rx.a.open);
  }
  // Hostile headers and lengths: nothing is stored and nothing is written.
  {
    Receiver rx;
    const std::vector<Part> good = partsOf(jsonOf(300), kFrameCapRouted, REMOTE_SENSOR_TOF, 7, 1);
    auto malformed = [&](Part p) {
      const PartResult r = rx.feed(p, 50);
      return r.verdict == PartVerdict::Malformed && r.incomplete == 0 && !rx.a.open;
    };
    Part p = good[0];
    p.h.partCount = 0;
    CHECK(malformed(p));
    p = good[0];
    p.h.partCount = 5;
    CHECK(malformed(p));
    p = good[0];
    p.h.partCount = 15;
    p.h.partIndex = 14;
    CHECK(malformed(p));
    p = good[0];
    p.h.partIndex = 3;  // index == count
    CHECK(malformed(p));
    p = good[0];
    p.h.sensorType = REMOTE_SENSOR_MAX;
    CHECK(malformed(p));
    p = good[0];
    p.h.sensorType = 255;
    CHECK(malformed(p));
    p = good[0];
    p.data.clear();
    p.data.push_back('x');
    p.data.resize(kPartMaxDirect + 1, 'x');  // longer than any frame carries
    CHECK(malformed(p));
    // No data at all.
    const uint8_t* out = nullptr;
    size_t outLen = 0;
    const uint8_t nothing = 0;
    PartResult r = partAccept(rx.a, rx.buf.data(), rx.buf.size(), good[0].h, &nothing, 0, 50, &out, &outLen);
    CHECK(r.verdict == PartVerdict::Malformed);
    r = partAccept(rx.a, rx.buf.data(), rx.buf.size(), good[0].h, nullptr, 10, 50, &out, &outLen);
    CHECK(r.verdict == PartVerdict::Malformed);
    r = partAccept(rx.a, rx.buf.data(), kReadingMaxBytes - 1, good[0].h, good[0].data.data(), good[0].data.size(), 50,
                   &out, &outLen);
    CHECK(r.verdict == PartVerdict::Malformed);  // a buffer that is too small is never written
    // Parts that say "four of 190 bytes each": the fourth would pass 472 bytes.
    std::vector<uint8_t> big(kPartMaxDirect, 'y');
    Part q;
    q.h = ReadingHeader{};
    q.h.sensorType = REMOTE_SENSOR_TOF;
    q.h.bootTag = 7;
    q.h.counter = 40;
    q.h.partCount = 4;
    q.data = big;
    q.h.partIndex = 0;
    CHECK(rx.feed(q, 60).verdict == PartVerdict::Stored);
    q.h.partIndex = 1;
    CHECK(rx.feed(q, 60).verdict == PartVerdict::Stored);
    q.h.partIndex = 2;
    r = rx.feed(q, 60);  // 570 bytes would not fit: the reading is given up, nothing is written
    CHECK(r.verdict == PartVerdict::Dropped && r.incomplete == 1 && !rx.a.open);
  }
}

struct Counted {
  CounterTrack t;
  CounterCounts c;
  Counted() : t{}, c{} {}
  CounterVerdict see(uint16_t tag, uint16_t counter, uint32_t nowMs, bool start = false) {
    return counterJudge(t, c, tag, counter, start, nowMs);
  }
};

static void testCounter() {
  // First, new, a gap, a repeat.
  {
    Counted r;
    CHECK(r.see(5, 1, 1000) == CounterVerdict::First);
    CHECK(r.see(5, 2, 1500) == CounterVerdict::New && r.c.lost == 0);
    CHECK(r.see(5, 5, 2000) == CounterVerdict::New && r.c.lost == 2);
    CHECK(r.see(5, 5, 2100) == CounterVerdict::Repeat && r.c.repeated == 1 && r.c.lost == 2);
    CHECK(r.c.received == 3 && r.t.last == 5);
    CHECK(counterVerdictKeeps(CounterVerdict::First) && counterVerdictKeeps(CounterVerdict::New) &&
          counterVerdictKeeps(CounterVerdict::Restart) && counterVerdictKeeps(CounterVerdict::Resume) &&
          !counterVerdictKeeps(CounterVerdict::Repeat) && !counterVerdictKeeps(CounterVerdict::Late));
    // A receiver that starts in the middle of a stream counts nothing lost.
    Counted mid;
    CHECK(mid.see(5, 4000, 10) == CounterVerdict::First && mid.c.lost == 0);
  }
  // The wrap 65535 -> 0, and a gap across it.
  {
    Counted r;
    CHECK(r.see(5, 65534, 0) == CounterVerdict::First);
    CHECK(r.see(5, 65535, 500) == CounterVerdict::New);
    CHECK(r.see(5, 0, 1000) == CounterVerdict::New && r.c.lost == 0);
    CHECK(r.see(5, 3, 1500) == CounterVerdict::New && r.c.lost == 2);
    uint16_t state = 65534;
    CHECK(counterNext(state) == 65535 && counterNext(state) == 0 && counterNext(state) == 1);
    uint16_t fresh = 0;
    CHECK(counterNext(fresh) == 1);  // the first reading after boot carries 1
  }
  // An older reading that arrives right after a newer one is late: dropped,
  // and it was already counted lost.
  {
    Counted r;
    r.see(5, 10, 1000);
    CHECK(r.see(5, 12, 1500) == CounterVerdict::New && r.c.lost == 1);
    CHECK(r.see(5, 11, 1600) == CounterVerdict::Late && r.c.late == 1 && r.c.lost == 1 && r.t.last == 12);
    CHECK(r.c.received == 2);
    CHECK(r.see(5, 13, 2000) == CounterVerdict::New && r.c.lost == 1);
  }
  // Another boot tag is a restart: kept, no loss.
  {
    Counted r;
    r.see(5, 900, 1000);
    CHECK(r.see(6, 1, 1500) == CounterVerdict::Restart && r.c.restarts == 1 && r.c.lost == 0 && r.t.last == 1);
    CHECK(r.see(6, 2, 2000) == CounterVerdict::New);
  }
  // A restart that drew the same tag (it cannot with a boot counter, but the
  // fallback stays): eight late readings in a row, then it is a restart.
  {
    Counted r;
    r.see(5, 30000, 1000);
    for (uint16_t i = 1; i <= 7; ++i) {
      CHECK(r.see(5, i, 1000 + i * 100u) == CounterVerdict::Late);
    }
    CHECK(r.c.late == 7 && r.c.restarts == 0);
    CHECK(r.see(5, 8, 1800) == CounterVerdict::Restart && r.c.restarts == 1 && r.t.last == 8);
    CHECK(r.see(5, 9, 1900) == CounterVerdict::New && r.c.lost == 0);
    // "In a row": any other verdict starts the run again.
    Counted s;
    s.see(5, 30000, 1000);
    for (uint16_t i = 1; i <= 7; ++i) s.see(5, i, 1000 + i * 100u);
    CHECK(s.see(5, 30001, 1800) == CounterVerdict::New);
    for (uint16_t i = 1; i <= 7; ++i) {
      CHECK(s.see(5, i, 1900 + i * 100u) == CounterVerdict::Late);
    }
    CHECK(s.c.restarts == 0);
    Counted q;
    q.see(5, 30000, 1000);
    for (uint16_t i = 1; i <= 7; ++i) q.see(5, i, 1000 + i * 10u);
    CHECK(q.see(5, 30000, 1100) == CounterVerdict::Repeat);  // a repeat also ends the run
    CHECK(q.see(5, 8, 1200) == CounterVerdict::Late && q.c.restarts == 0);
  }
  // The stream-start flag: the first reading to this receiver after the
  // sender sent elsewhere is kept and counts no loss, whatever the counter.
  {
    Counted r;
    r.see(5, 100, 1000);
    CHECK(r.see(5, 5000, 2000, /*start=*/true) == CounterVerdict::Resume && r.c.resumes == 1 && r.c.lost == 0);
    CHECK(r.see(5, 5001, 2500) == CounterVerdict::New && r.c.lost == 0);
    CHECK(r.see(5, 40000, 3000, true) == CounterVerdict::Resume && r.c.lost == 0 && r.t.last == 40000);
    CHECK(r.see(5, 40000, 3100, true) == CounterVerdict::Repeat);  // the same reading again stays a repeat
  }
  // After more than half the counter range elsewhere (54 minutes at ten
  // readings a second), without the flag: silence over the late window makes
  // it a resume, not eight dropped readings.
  {
    Counted r;
    r.see(5, 100, 1000);
    CHECK(r.see(5, (uint16_t)(100 + 40000), 1000 + kLateWindowMs) == CounterVerdict::Resume);
    CHECK(r.c.resumes == 1 && r.c.late == 0 && r.c.lost == 0 && r.c.received == 2);
    // Inside the window the same counter is late.
    Counted s;
    s.see(5, 100, 1000);
    CHECK(s.see(5, (uint16_t)(100 + 40000), 1000 + kLateWindowMs - 1) == CounterVerdict::Late);
    // The window is measured on the receiver's uptime across its wrap.
    Counted w;
    w.see(5, 1000, 0xFFFFFFF0u);
    CHECK(w.see(5, 99, 0xFFFFFFF0u + 100) == CounterVerdict::Late);
    CHECK(w.see(5, 99, 0xFFFFFFF0u + kLateWindowMs) == CounterVerdict::Resume);
  }
  // An older frame never replaces a newer one, however long the silence: a
  // frame held back on the way (a relay; the replay window lets through no
  // more than 64 frames back) is at most 256 behind and stays late. Bug: a
  // battery frame 5 delivered 14 s after 6 replaced 6 with a fresh stamp.
  {
    Counted r;
    r.see(5, 5, 1000);
    CHECK(r.see(5, 6, 2000) == CounterVerdict::New);
    CHECK(r.see(5, 5, 2000 + 14000) == CounterVerdict::Late && r.t.last == 6 && r.c.late == 1 && r.c.resumes == 0);
    CHECK(r.see(5, 5, 2000 + 3600000u) == CounterVerdict::Late && r.t.last == 6);
    Counted b;
    b.see(5, 1000, 1000);
    CHECK(b.see(5, (uint16_t)(1000 - kLateMaxBack), 1000 + kLateWindowMs * 10) == CounterVerdict::Late);
    CHECK(b.see(5, (uint16_t)(1000 - kLateMaxBack - 1), 1000 + kLateWindowMs * 10) == CounterVerdict::Resume &&
          b.t.last == 1000 - kLateMaxBack - 1);
    // Across the counter's wrap: 65535 then 0, and 65535 again later.
    Counted z;
    z.see(5, 65535, 1000);
    CHECK(z.see(5, 0, 2000) == CounterVerdict::New && z.c.lost == 0);
    CHECK(z.see(5, 65535, 2000 + 60000) == CounterVerdict::Late && z.t.last == 0);
    // A sender that restarted without a new boot tag (its boot counter could
    // not be written) starts again at 1: eight in a row make it a restart.
    Counted q;
    q.see(5, 120, 1000);
    for (uint16_t i = 1; i <= 7; ++i) CHECK(q.see(5, i, 1000 + 60000u * i) == CounterVerdict::Late);
    CHECK(q.see(5, 8, 1000 + 60000u * 8) == CounterVerdict::Restart && q.t.last == 8);
  }
  // A gap under half the range is loss however long the silence was.
  {
    Counted r;
    r.see(5, 100, 1000);
    CHECK(r.see(5, 400, 1000 + 3600000u) == CounterVerdict::New && r.c.lost == 299);
  }
  // One sender, two receivers (battery and events go to the master and the
  // backup): the same numbers reach both, so neither counts loss; a receiver
  // that missed some counts exactly those.
  {
    uint16_t counter = 0;
    Counted primary, backup;
    for (int i = 0; i < 100; ++i) {
      const uint16_t value = counterNext(counter);
      primary.see(9, value, 1000 + (uint32_t)i * 1000);
      if (i < 40 || i >= 50) backup.see(9, value, 1000 + (uint32_t)i * 1000);
    }
    CHECK(primary.c.received == 100 && primary.c.lost == 0);
    CHECK(backup.c.received == 90 && backup.c.lost == 10);
  }
  // The sender's stream-start bookkeeping.
  {
    const uint8_t hub[6] = { 2, 0, 0, 0, 0, 1 };
    const uint8_t backup[6] = { 2, 0, 0, 0, 0, 2 };
    StreamTarget s = {};
    CHECK(streamStartWanted(s, hub));          // first reading after boot
    CHECK(streamStartWanted(s, hub));          // still wanted: nothing went out yet
    streamSentTo(s, hub);
    CHECK(!streamStartWanted(s, hub));
    CHECK(streamStartWanted(s, backup));       // the target changed
    streamSentTo(s, backup);
    CHECK(!streamStartWanted(s, backup) && streamStartWanted(s, hub));
    streamRestart(s);                          // the stream was switched on again
    CHECK(streamStartWanted(s, backup));
    streamSentTo(s, backup);
    CHECK(!streamStartWanted(s, backup));
  }
  CHECK(is(counterVerdictName(CounterVerdict::Late), "late") && is(counterVerdictName(CounterVerdict::Resume), "resume"));
  // How old a stamp is: one taken by another task after the loop read the
  // uptime is 0 ms old, not 49 days; the wrap is handled.
  CHECK(elapsedSinceMs(1000, 400) == 600 && elapsedSinceMs(1000, 1000) == 0 && elapsedSinceMs(1000, 1300) == 0);
  CHECK(elapsedSinceMs(1000, 1000 + kStampAheadSlackMs) == 0 && elapsedSinceMs(5, 0xFFFFFFF0u) == 21);
  CHECK(elapsedSinceMs(0xFFFFFFF0u, 5) == 0 && elapsedSinceMs(1000, 1000 + kStampAheadSlackMs + 1) > 0xF0000000u);
}

static bool object(const std::string& s) {
  return readingTextIsObject((const uint8_t*)s.data(), s.size());
}

static void testTextCheck() {
  CHECK(object("{}"));
  CHECK(object("{\"valid\":true,\"ts\":8123,\"lat\":37.1,\"objs\":[{\"d\":1},{\"d\":2}]}"));
  CHECK(object("{\"t\":\"a } b ] \\\" c {\"}"));           // brackets and an escaped quote inside a string
  CHECK(object("{\"t\":\"\\\\\"}"));                         // a string that ends in a backslash
  // The forgery: valid JSON after it is pasted behind "data":, with keys that
  // override the receiver's own.
  CHECK(!object("{},\"lost\":0,\"arrivalEpochMs\":1,\"x\":{}"));
  CHECK(!object("{},\"lost\":0,\"arrivalEpochMs\":1,\"x\":{"));
  CHECK(!object("{}{}"));
  CHECK(!object(""));
  CHECK(!object("{"));
  CHECK(!object("}"));
  CHECK(!object("[1,2]"));
  CHECK(!object("\"text\""));
  CHECK(!object("null"));
  CHECK(!object("{\"a\":1"));
  CHECK(!object("{\"a\":[1,2}"));
  CHECK(!object("{\"a\":[1,2}]"));
  CHECK(!object("{\"a\":\"open}"));                          // the string never ends
  CHECK(!object("{\"a\":\"x\\\"}"));                         // the closing quote is escaped
  CHECK(!object(std::string("{\"a\":1,\0\"b\":2}", 14)));    // a NUL would cut every C-string reader
  CHECK(!object("{\"a\":\"line\nbreak\"}"));
  CHECK(!object("{\"a\":\t1}"));
  CHECK(!object(" {\"a\":1}"));
  CHECK(!object("{\"a\":1} "));
  // Strict JSON: what is pasted behind "data": must keep the receiver's reply
  // valid JSON. Each of these passed the earlier structural check.
  for (const char* bad : { "{x}", "{\"a\":}", "{\"a\":1,}", "{\"a\":\"\\q\"}", "{\"a\" 1}", "{\"a\":1 \"b\":2}",
                           "{1:2}", "{\"a\":[1,]}", "{\"a\":[,1]}", "{\"a\":tru}", "{\"a\":nul}", "{\"a\":01}",
                           "{\"a\":1.}", "{\"a\":.5}", "{\"a\":-}", "{\"a\":1e}", "{\"a\":+1}", "{\"a\":\"\\u12G4\"}",
                           "{\"a\":\"\\u12\"}", "{\"a\":{\"b\":1}", "{\"a\":1}}", "{,}", "{\"a\":[1}}",
                           "{\"a\":1,,\"b\":2}", "{\"a\"::1}", "{\"a\":True}", "{\"a\":'x'}", "{\"a\":NaN}" }) {
    CHECK(!object(bad));
  }
  // Malformed UTF-8 inside a string: a lone continuation byte, a cut
  // character, an overlong form, a surrogate, over U+10FFFF, 0xFF.
  for (const char* bad : { "{\"a\":\"\x80\"}", "{\"a\":\"\xC3\"}", "{\"a\":\"\xC0\xAF\"}", "{\"a\":\"\xED\xA0\x80\"}",
                           "{\"a\":\"\xF4\x90\x80\x80\"}", "{\"a\":\"\xFF\"}", "{\"\xE2\x82\":1}" }) {
    CHECK(!object(bad));
  }
  // What strict JSON allows.
  for (const char* good : { "{\"a\":-0.5e+10,\"b\":1E3,\"c\":0,\"d\":-12.25}", "{\"a\":true,\"b\":false,\"c\":null}",
                            "{\"a\":\"\\u00e9\\n\\t\\/\\b\\f\\r\\\\\\\"\"}", "{\"a\": [ 1 , 2 ] , \"b\" : { } }",
                            "{\"\xC3\xA9t\xC3\xA9\":\"\xE2\x82\xAC \xF0\x9F\x98\x80\"}", "{\"a\":[[],{},[{}]]}", "{\"a\":\"\x7F\"}" }) {
    CHECK(object(good));
  }
  // Deeper than the check follows is refused, not misjudged.
  std::string deep = "{\"a\":";
  for (int i = 0; i < 20; ++i) deep += "[";
  for (int i = 0; i < 20; ++i) deep += "]";
  deep += "}";
  CHECK(!object(deep));
  std::string ok = "{\"a\":";
  for (int i = 0; i < 15; ++i) ok += "[";
  for (int i = 0; i < 15; ++i) ok += "]";
  ok += "}";
  CHECK(object(ok));
  CHECK(!readingTextIsObject(nullptr, 5));
  // The largest reading passes whole.
  CHECK(object(jsonOf(kReadingMaxBytes)));
}

static void testTargetAndReasons() {
  const uint8_t partner[6] = { 2, 0, 0, 0, 0, 9 };
  const uint8_t hub[6] = { 2, 0, 0, 0, 0, 1 };
  const uint8_t backup[6] = { 2, 0, 0, 0, 0, 2 };
  TargetInputs in = {};
  in.broadcastOn = true;
  in.meshOn = true;
  in.meshRole = kRoleWorker;
  in.leaseRunning = true;
  std::memcpy(in.controller, backup, 6);
  TargetChoice t = readingTarget(in);
  CHECK(t.why == DropReason::None && !t.viaBond && std::memcmp(t.mac, backup, 6) == 0);
  // Rule 0: a bond worker sends to its partner whatever the mesh says, or nowhere.
  in.bondWorker = true;
  in.bondOnline = true;
  std::memcpy(in.bondPartner, partner, 6);
  t = readingTarget(in);
  CHECK(t.why == DropReason::None && t.viaBond && std::memcmp(t.mac, partner, 6) == 0);
  in.broadcastOn = false;
  in.meshOn = false;
  CHECK(readingTarget(in).viaBond);
  in.bondOnline = false;
  CHECK(readingTarget(in).why == DropReason::BondOffline);
  // The mesh checks, in order.
  in.bondWorker = false;
  CHECK(readingTarget(in).why == DropReason::BroadcastOff);
  in.broadcastOn = true;
  CHECK(readingTarget(in).why == DropReason::MeshOff);
  in.meshOn = true;
  in.meshRole = kRoleMaster;
  CHECK(readingTarget(in).why == DropReason::IsMaster);
  // A backup master that has not taken over sends its own readings.
  in.meshRole = kRoleBackup;
  CHECK(readingTarget(in).why == DropReason::None);
  // Nobody asked and the stream was not switched on by hand: nowhere.
  in.leaseRunning = false;
  CHECK(readingTarget(in).why == DropReason::NoReceiver);
  in.masterNamed = true;
  in.havePrimary = true;
  std::memcpy(in.primary, hub, 6);
  CHECK(readingTarget(in).why == DropReason::NoReceiver);  // a named master alone starts no stream
  // Switched on by hand: to the primary master named on this device.
  in.byHand = true;
  t = readingTarget(in);
  CHECK(t.why == DropReason::None && !t.viaBond && std::memcmp(t.mac, hub, 6) == 0);
  // A running lease wins: its controller asked for exactly this sensor.
  in.leaseRunning = true;
  t = readingTarget(in);
  CHECK(t.why == DropReason::None && std::memcmp(t.mac, backup, 6) == 0);
  in.leaseRunning = false;
  // By hand with a master that is named but not paired, and with none named.
  in.havePrimary = false;
  CHECK(readingTarget(in).why == DropReason::MasterNotPaired);
  in.masterNamed = false;
  CHECK(readingTarget(in).why == DropReason::NoReceiver);
  // A lease that ends stops the stream it started; one switched on by hand goes on.
  CHECK(!streamSurvivesLeaseEnd(false) && streamSurvivesLeaseEnd(true));

  // Who sends and who keeps (S15): a worker sends and keeps nothing, a backup
  // master does both, the master (also a backup that took over, which
  // carries the master's number) only keeps.
  CHECK(roleSends(kRoleWorker) && roleSends(kRoleBackup) && !roleSends(kRoleMaster));
  CHECK(!roleReceives(kRoleWorker) && roleReceives(kRoleBackup) && roleReceives(kRoleMaster));
  CHECK(!roleReceives(3) && !roleReceives(255));  // a role number this firmware does not know keeps nothing
  CHECK(is(roleKey(kRoleWorker), "worker") && is(roleKey(kRoleBackup), "backup") && is(roleKey(kRoleMaster), "master"));

  // Every reason has a JSON name and a sentence of its own.
  for (size_t i = 0; i < kDropReasonCount; ++i) {
    const DropReason r = (DropReason)i;
    CHECK(!is(dropReasonKey(r), "?") && !is(dropReasonText(r), "?"));
    for (size_t k = 0; k < i; ++k) {
      CHECK(!is(dropReasonKey(r), dropReasonKey((DropReason)k)));
      CHECK(!is(dropReasonText(r), dropReasonText((DropReason)k)));
    }
  }
  CHECK(is(dropReasonKey(DropReason::TooLong), "too_long") && is(dropReasonKey(DropReason::NoSession), "no_session") &&
        is(dropReasonKey(DropReason::NotInBuild), "not_in_this_build") &&
        is(dropReasonKey(DropReason::MasterNotPaired), "master_not_paired"));
  // A drop the receiver must see as a gap takes a counter value first.
  CHECK(dropTakesCounter(DropReason::TooLong) && dropTakesCounter(DropReason::QueueFull));
  CHECK(!dropTakesCounter(DropReason::NoSession) && !dropTakesCounter(DropReason::NoReceiver) &&
        !dropTakesCounter(DropReason::BondOffline) && !dropTakesCounter(DropReason::MeshOff));
}

// A sender and a receiver end to end: 200 readings of mixed length over a
// routed path, every seventh one with a part lost on the air. The receiver
// stores only whole readings and its loss count is the number that did not
// arrive.
static void testEndToEnd() {
  uint16_t counter = 0;
  Receiver rx;
  Counted counted;
  unsigned whole = 0, incomplete = 0, damaged = 0;
  for (int i = 0; i < 200; ++i) {
    const std::string text = jsonOf(40 + (size_t)(i * 37 % 430));
    const std::vector<Part> parts = partsOf(text, kFrameCapRouted, REMOTE_SENSOR_TOF, 3, counterNext(counter));
    const bool lose = (i % 7 == 6) && parts.size() > 1;
    if (lose) ++damaged;
    for (size_t k = 0; k < parts.size(); ++k) {
      if (lose && k == parts.size() / 2) continue;
      std::string got;
      const PartResult r = rx.feed(parts[k], (uint32_t)i * 500, &got);
      incomplete += r.incomplete;
      if (r.verdict != PartVerdict::Complete) continue;
      CHECK(got == text);
      CHECK(readingTextIsObject((const uint8_t*)got.data(), got.size()));
      CHECK(counterVerdictKeeps(counted.see(3, parts[k].h.counter, (uint32_t)i * 500)));
      ++whole;
    }
  }
  CHECK(damaged > 10);
  CHECK(whole == 200 - damaged && counted.c.received == whole);
  CHECK(incomplete == damaged);
  CHECK(counted.c.lost == damaged);  // incomplete readings are part of lost
}

static void testFingerprintArg() {
  using espnowsensor::FingerprintArg;
  using espnowsensor::fingerprintArgKind;
  const std::string hex = "6e9c0f7a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6";
  const std::string upper = "6E9C0F7A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4E5F60718293A4B5C6";
  auto kind = [](const std::string& arg) { return fingerprintArgKind(arg.c_str(), arg.size()); };
  CHECK(kind("") == FingerprintArg::Show && fingerprintArgKind(nullptr, 0) == FingerprintArg::Show);
  for (const char* word : { "clear", "CLEAR", "Clear" }) CHECK(kind(word) == FingerprintArg::Clear);
  CHECK(kind(hex) == FingerprintArg::Set && kind(upper) == FingerprintArg::Set);
  CHECK(espnowsensor::fingerprintTextValid(hex.c_str(), hex.size()));
  // Refused: too short, too long, a letter that is not hex, a space inside,
  // other words (an empty value is never stored by the command).
  CHECK(kind(hex.substr(0, 63)) == FingerprintArg::Bad);
  CHECK(kind(hex + "0") == FingerprintArg::Bad);
  std::string notHex = hex;
  notHex[10] = 'g';
  CHECK(kind(notHex) == FingerprintArg::Bad);
  std::string spaced = hex;
  spaced[32] = ' ';
  CHECK(kind(spaced) == FingerprintArg::Bad);
  for (const char* word : { "none", "off", "0", "clear!", "clea", "\"\"" }) CHECK(kind(word) == FingerprintArg::Bad);
  CHECK(!espnowsensor::fingerprintTextValid(nullptr, 64) && !espnowsensor::fingerprintTextValid("", 0));
  CHECK(std::string(espnowsensor::kFingerprintBadReply).rfind("Error: ", 0) == 0);

  // A settings editor sends an emptied fingerprint field as 'clear', and
  // every other text (and every other setting) as it is.
  using espnowsensor::fingerprintEditorArg;
  CHECK(std::string(fingerprintEditorArg("espnowmasterfingerprint", "")) == "clear");
  CHECK(std::string(fingerprintEditorArg("espnowbackupfingerprint", " \t")) == "clear");
  CHECK(std::string(fingerprintEditorArg("EspNowMasterFingerprint", nullptr)) == "clear");
  CHECK(std::string(fingerprintEditorArg("espnowmasterfingerprint", hex.c_str())) == hex);
  CHECK(std::string(fingerprintEditorArg("ntpserver", "")).empty());
  CHECK(std::string(fingerprintEditorArg("espnowmasterfingerprintx", "")).empty());
  CHECK(std::string(fingerprintEditorArg("espnowmaster", "")).empty());
  CHECK(std::string(fingerprintEditorArg(nullptr, "x")) == "x");

  // The reply to 'clear' says what is true in each case.
  const std::string none = espnowsensor::fingerprintClearedReply(false, false);
  CHECK(none.rfind("OK: no master is named on this device now", 0) == 0);
  CHECK(none.find("takes no mesh time") != std::string::npos &&
        none.find("readings, battery state and events to no device") != std::string::npos &&
        none.find("refuses sensor requests") != std::string::npos);
  const std::string backupLeft = espnowsensor::fingerprintClearedReply(false, true);
  CHECK(backupLeft.rfind("OK: no master is named on this device now", 0) == 0);
  CHECK(backupLeft.find("Readings switched on by hand go to no device") != std::string::npos &&
        backupLeft.find("espnowbackupfingerprint") != std::string::npos &&
        backupLeft.find("battery state and events") != std::string::npos &&
        backupLeft.find("takes no mesh time") == std::string::npos);
  const std::string backupNone = espnowsensor::fingerprintClearedReply(true, false);
  CHECK(backupNone.rfind("OK: no backup master is named on this device now, and no master is either", 0) == 0 &&
        backupNone.find("takes no mesh time") != std::string::npos);
  const std::string masterLeft = espnowsensor::fingerprintClearedReply(true, true);
  CHECK(masterLeft.rfind("OK: no backup master is named on this device now", 0) == 0 &&
        masterLeft.find("espnowmasterfingerprint") != std::string::npos &&
        masterLeft.find("no mesh time") == std::string::npos);
}

// A bond worker's stream switches (BOND_STREAM_CTRL), kept per sensor until
// the task applies them: two frames in one loop both count (review
// 2026-11-18: a request for ToF and IMU used to start IMU only).
static void testStreamCtrlQueue() {
  StreamCtrlQueue q = {};
  uint16_t on = 0xFFFF, off = 0xFFFF;
  streamCtrlTake(q, &on, &off);
  CHECK(on == 0 && off == 0);
  streamCtrlPut(q, REMOTE_SENSOR_TOF, true);
  streamCtrlPut(q, REMOTE_SENSOR_IMU, true);
  streamCtrlPut(q, REMOTE_SENSOR_GPS, false);
  streamCtrlPut(q, REMOTE_SENSOR_MAX, true);  // not a sensor here: dropped
  streamCtrlPut(q, 200, false);
  streamCtrlTake(q, &on, &off);
  CHECK(on == (sensorBit(REMOTE_SENSOR_TOF) | sensorBit(REMOTE_SENSOR_IMU)) && off == sensorBit(REMOTE_SENSOR_GPS));
  CHECK(q.pending == 0 && q.on == 0);
  // For one sensor the last switch wins.
  streamCtrlPut(q, REMOTE_SENSOR_TOF, true);
  streamCtrlPut(q, REMOTE_SENSOR_TOF, false);
  streamCtrlPut(q, REMOTE_SENSOR_IMU, false);
  streamCtrlPut(q, REMOTE_SENSOR_IMU, true);
  streamCtrlTake(q, &on, &off);
  CHECK(on == sensorBit(REMOTE_SENSOR_IMU) && off == sensorBit(REMOTE_SENSOR_TOF));
}

// A master's request on this device: only the sensors it has running are
// leased (bench 2026-11-18: ToF was leased where there is none).
static void testReqSplit() {
  const uint32_t tof = sensorBit(REMOTE_SENSOR_TOF), imu = sensorBit(REMOTE_SENSOR_IMU);
  ReqSplit r = sensorReqSplit(0, tof | imu, imu);
  CHECK(r.apply == imu && r.lacking == tof);
  r = sensorReqSplit(2, tof, 0);
  CHECK(r.apply == 0 && r.lacking == tof);
  r = sensorReqSplit(0, tof | imu, tof | imu);
  CHECK(r.apply == (tof | imu) && r.lacking == 0);
  r = sensorReqSplit(1, tof | imu, 0);  // an unsubscribe ends whatever it names
  CHECK(r.apply == (tof | imu) && r.lacking == 0);
  r = sensorReqSplit(7, tof, 0);  // an unknown mode is passed on (the apply refuses it)
  CHECK(r.apply == tof && r.lacking == 0);
  r = sensorReqSplit(0, 0xFFFFFFFFu, 0xFFFFFFFFu);  // bits above the table are never applied
  CHECK(r.apply == kSensorMaskAll && r.lacking == 0);
}

// espnowsensorreq's words (bench 2026-11-18: only numbers were taken).
static void testRequestWords() {
  uint32_t m = 99;
  char bad[16];
  CHECK(parseSensorList("tof", m, bad, sizeof(bad)) && m == sensorBit(REMOTE_SENSOR_TOF));
  CHECK(parseSensorList("tof,IMU", m, bad, sizeof(bad)) && m == (sensorBit(REMOTE_SENSOR_TOF) | sensorBit(REMOTE_SENSOR_IMU)));
  CHECK(parseSensorList("all", m, bad, sizeof(bad)) && m == kSensorMaskAll);
  CHECK(parseSensorList("ALL", m, bad, sizeof(bad)) && m == kSensorMaskAll);
  CHECK(parseSensorList("0x6", m, bad, sizeof(bad)) && m == 6);
  CHECK(parseSensorList("6", m, bad, sizeof(bad)) && m == 6);
  CHECK(parseSensorList("0X7ff", m, bad, sizeof(bad)) && m == kSensorMaskAll);
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    CHECK(parseSensorList(kSensorTable[i].name, m, bad, sizeof(bad)) && m == sensorBit(i));
  }
  m = 99;
  CHECK(!parseSensorList("tof,lidar", m, bad, sizeof(bad)) && m == 99 && strcmp(bad, "lidar") == 0);
  CHECK(!parseSensorList("subscribe", m, bad, sizeof(bad)) && strcmp(bad, "subscribe") == 0);
  CHECK(!parseSensorList("tof,", m, bad, sizeof(bad)) && bad[0] == '\0');
  CHECK(!parseSensorList(",tof", m, bad, sizeof(bad)) && bad[0] == '\0');
  CHECK(!parseSensorList("", m, bad, sizeof(bad)) && bad[0] == '\0');
  CHECK(!parseSensorList("0x800", m, bad, sizeof(bad)) && strcmp(bad, "0x800") == 0);  // above the table
  CHECK(!parseSensorList("0", m, bad, sizeof(bad)) && strcmp(bad, "0") == 0);          // no sensor
  CHECK(!parseSensorList("0x", m, bad, sizeof(bad)) && !parseSensorList("6x", m, bad, sizeof(bad)) &&
        !parseSensorList("0xg", m, bad, sizeof(bad)) && !parseSensorList("99999999999", m, bad, sizeof(bad)));
  // Hex digits only after 0x: a decimal number with letters is junk, not read as hex or cut short.
  CHECK(!parseSensorList("1a", m, bad, sizeof(bad)) && strcmp(bad, "1a") == 0 && !parseSensorList("6f", m, bad, sizeof(bad)) &&
        !parseSensorList("2tof", m, bad, sizeof(bad)) && !parseSensorList("0x6,tof", m, bad, sizeof(bad)) &&
        !parseSensorList("6 ", m, bad, sizeof(bad)) && !parseSensorList("-1", m, bad, sizeof(bad)));
  CHECK(!parseSensorList("a-very-long-sensor-name", m, bad, sizeof(bad)) && strlen(bad) == sizeof(bad) - 1);
  CHECK(m == 99);
  uint8_t mode = 9;
  CHECK(parseRequestMode("subscribe", mode) && mode == 0 && parseRequestMode("sub", mode) && mode == 0 &&
        parseRequestMode("0", mode) && mode == 0);
  CHECK(parseRequestMode("unsubscribe", mode) && mode == 1 && parseRequestMode("UNSUB", mode) && mode == 1 &&
        parseRequestMode("1", mode) && mode == 1);
  CHECK(parseRequestMode("oneshot", mode) && mode == 2 && parseRequestMode("once", mode) && mode == 2 &&
        parseRequestMode("2", mode) && mode == 2);
  mode = 9;
  CHECK(!parseRequestMode("3", mode) && !parseRequestMode("subs", mode) && !parseRequestMode("", mode) &&
        !parseRequestMode(nullptr, mode) && mode == 9);
}

int main() {
  testRequestWords();
  testReqSplit();
  testFingerprintArg();
  testSensorTable();
  testHeader();
  testPlanning();
  testAssembly();
  testCounter();
  testTextCheck();
  testTargetAndReasons();
  testEndToEnd();
  testStreamCtrlQueue();
  std::printf("espnow_sensor_core_tests: %u checks passed\n", gChecks);
  return 0;
}
