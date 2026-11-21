#pragma once

// Stock G2 Health panel, page 2 ("insights"): up to three lines of text, each
// under one of the glasses' health icons, sent on sid 0x0E to the right temple
// as the Even app's "mult highlight" message (cmd 4). Pure, host-testable, no
// Arduino/IDF, no allocation.
//
// Wire (glasses 2.3.2.14, captured from the phone and device-tested
// 2026-11-21; see docs/G2_HEALTH_PANEL.md):
//   {1 cmd=4, 2 magic, 6 {1 item {1 type, 2 text, 3 0} (1..3 times), 2 0, 3 1}}
//   no insights: {1 4, 2 magic, 6 {2 0, 3 3}}  -> the page shows "gathering insights"
//   glasses ACK: {1 4, 2 magic, 6 {1 {1 type} per stored item, 3 status}}
// Every length is a protobuf varint: a length of 128 or more takes two bytes
// (the glasses drop a message whose length is written as one byte). A message
// with four items is rejected (no ACK). Text is ASCII here, at most 255 bytes,
// no NUL; a newline starts a new row under the same icon.
//
// The adapter (G2_HealthInsights.cpp) aggregates the ring's daily history on
// the main loop into DaySummary values, builds the lines with buildInsights()
// and publishes encodeBody()'s bytes; the G2 control task only prefixes the
// cmd and magic (encodeMessage) and writes it after validateMessagePb().

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "G2_FeedCore.h"

namespace hw1_g2_health_insights {

static constexpr uint8_t kSid = 0x0E;
static constexpr uint8_t kCmdHighlight = 4;
static constexpr size_t kMaxItems = 3;
static constexpr size_t kMaxTextBytes = 255;
static constexpr uint8_t kStatusHasData = 1;
static constexpr uint8_t kStatusNone = 3;

// Icon types the 2.3.2.14 page draws. Any other type is not drawn.
enum Type : uint8_t {
  kSteps = 2,
  kCalories = 3,  // steps icon
  kSleep = 4,
  kHeartRate = 5,
  kSpo2 = 6,
  kTemperature = 7,
  kHrv = 8,
};
inline bool typeDrawn(uint32_t t) { return t >= kSteps && t <= kHrv; }

// Item: 08 t | 12 <len varint> text | 18 00
static constexpr size_t kMaxItemBytes = 2 + 1 + 2 + kMaxTextBytes + 2;
// Body (field 6 contents): items as 0a <len varint> item, then 10 00 18 01.
static constexpr size_t kMaxBodyBytes = kMaxItems * (1 + 2 + kMaxItemBytes) + 4;
// Message: 08 04 | 10 <magic varint, <= 2 bytes> | 32 <len varint> body.
static constexpr size_t kMaxPbBytes = 2 + 3 + 3 + kMaxBodyBytes;

struct Line {
  uint8_t type = 0;
  uint16_t len = 0;
  char text[kMaxTextBytes + 1] = {};
};

// count 0 = no insights (the "gathering insights" message).
struct Insights {
  uint8_t count = 0;
  Line lines[kMaxItems];
};

// Allowed text bytes: printable ASCII and the newline.
inline bool textByteOk(uint8_t c) { return c == '\n' || (c >= 0x20 && c <= 0x7E); }

// Copies `text` into `l`, clamped to kMaxTextBytes; any byte outside
// textByteOk becomes '?'. Returns false (and leaves `l` empty) for an empty
// text or a type the glasses do not draw.
inline bool setLine(Line& l, uint8_t type, const char* text) {
  l = Line();
  if (!typeDrawn(type) || !text || !text[0]) return false;
  size_t n = 0;
  for (; text[n] && n < kMaxTextBytes; n++) {
    const uint8_t c = (uint8_t)text[n];
    l.text[n] = textByteOk(c) ? (char)c : '?';
  }
  l.text[n] = '\0';
  l.len = (uint16_t)n;
  l.type = type;
  return true;
}

// Appends a line when there is room (at most kMaxItems; a fourth is never
// added). False when full or the line is invalid.
inline bool addLine(Insights& in, uint8_t type, const char* text) {
  if (in.count >= kMaxItems) return false;
  if (!setLine(in.lines[in.count], type, text)) return false;
  in.count++;
  return true;
}

// ---------------------------------------------------------------- encoding

struct Writer {
  uint8_t* buf;
  size_t cap;
  size_t len;
  bool ok;
  void byte(uint8_t b) {
    if (len >= cap) { ok = false; return; }
    buf[len++] = b;
  }
  void varint(uint32_t v) {
    do {
      uint8_t b = (uint8_t)(v & 0x7F);
      v >>= 7;
      if (v) b |= 0x80;
      byte(b);
    } while (v);
  }
  void bytes(const void* p, size_t n) {
    const uint8_t* s = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) byte(s[i]);
  }
};

inline size_t varintLen(uint32_t v) {
  size_t n = 1;
  while (v >= 0x80) { v >>= 7; n++; }
  return n;
}

inline bool lineValid(const Line& l) {
  if (!typeDrawn(l.type) || l.len == 0 || l.len > kMaxTextBytes) return false;
  for (size_t i = 0; i < l.len; i++)
    if (!textByteOk((uint8_t)l.text[i])) return false;
  return true;
}

inline size_t itemLen(const Line& l) { return 2 + 1 + varintLen(l.len) + l.len + 2; }

// Field 6 contents. Returns its length, or 0 if `in` is invalid (more than
// kMaxItems lines, a bad type or text) or it does not fit `cap`.
inline size_t encodeBody(const Insights& in, uint8_t* out, size_t cap) {
  if (!out || in.count > kMaxItems) return 0;
  Writer w{out, cap, 0, true};
  if (in.count == 0) {
    w.byte(0x10); w.byte(0x00);
    w.byte(0x18); w.byte(kStatusNone);
    return w.ok ? w.len : 0;
  }
  for (size_t i = 0; i < in.count; i++) {
    const Line& l = in.lines[i];
    if (!lineValid(l)) return 0;
    w.byte(0x0A);
    w.varint((uint32_t)itemLen(l));
    w.byte(0x08); w.byte(l.type);
    w.byte(0x12);
    w.varint(l.len);
    w.bytes(l.text, l.len);
    w.byte(0x18); w.byte(0x00);
  }
  w.byte(0x10); w.byte(0x00);
  w.byte(0x18); w.byte(kStatusHasData);
  return w.ok ? w.len : 0;
}

// Prefixes cmd 4 and `magic` (1..255) to a body. Returns the message length,
// or 0 if it does not fit `cap` or the arguments are invalid.
inline size_t encodeMessage(const uint8_t* body, size_t bodyLen, uint8_t magic,
                            uint8_t* out, size_t cap) {
  if (!body || !out || bodyLen == 0 || bodyLen > kMaxBodyBytes || magic == 0) return 0;
  Writer w{out, cap, 0, true};
  w.byte(0x08); w.byte(kCmdHighlight);
  w.byte(0x10); w.varint(magic);
  w.byte(0x32); w.varint((uint32_t)bodyLen);
  w.bytes(body, bodyLen);
  return w.ok ? w.len : 0;
}

inline size_t encode(const Insights& in, uint8_t magic, uint8_t* out, size_t cap) {
  uint8_t body[kMaxBodyBytes];
  const size_t n = encodeBody(in, body, sizeof body);
  return n ? encodeMessage(body, n, magic, out, cap) : 0;
}

// ---------------------------------------------------------------- parsing

struct Reader {
  const uint8_t* p;
  size_t len;
  size_t pos;
  bool varint(uint32_t& v) {
    v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
      if (pos >= len) return false;
      const uint8_t b = p[pos++];
      v |= (uint32_t)(b & 0x7F) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  }
  bool expectByte(uint8_t b) {
    if (pos >= len || p[pos] != b) return false;
    pos++;
    return true;
  }
  // Length-delimited field `tag`: on success `end` is the index after it.
  bool expectLength(uint8_t tag, size_t& end) {
    uint32_t n = 0;
    if (!expectByte(tag) || !varint(n) || n > len - pos) return false;
    end = pos + n;
    return true;
  }
};

// Strict whitelist: true only for exactly the shapes encodeMessage() builds
// from a valid encodeBody() (captured layout, 0..3 items, drawn types, ASCII
// text 1..255 bytes, minimal varints). Optionally returns magic and lines
// (`out` is written as it parses, so it is meaningful only on success; no
// Insights copy on the stack, as the G2 control task calls this per send).
inline bool validateMessagePb(const uint8_t* pb, size_t len, uint8_t* magicOut = nullptr,
                              Insights* out = nullptr) {
  if (!pb || len == 0 || len > kMaxPbBytes) return false;
  Reader r{pb, len, 0};
  uint32_t magic = 0;
  size_t bodyEnd = 0;
  if (!r.expectByte(0x08) || !r.expectByte(kCmdHighlight)) return false;
  if (!r.expectByte(0x10) || !r.varint(magic) || magic == 0 || magic > 0xFF ||
      varintLen(magic) != r.pos - 3)
    return false;
  const size_t lenStart = r.pos + 1;
  if (!r.expectLength(0x32, bodyEnd) || bodyEnd != len) return false;
  if (varintLen((uint32_t)(len - r.pos)) != r.pos - lenStart) return false;  // minimal
  uint8_t count = 0;
  if (r.pos + 4 == len && pb[r.pos] == 0x10 && pb[r.pos + 1] == 0x00 &&
      pb[r.pos + 2] == 0x18 && pb[r.pos + 3] == kStatusNone) {
    if (magicOut) *magicOut = (uint8_t)magic;
    if (out) out->count = 0;
    return true;
  }
  while (r.pos < len && pb[r.pos] == 0x0A) {
    if (count >= kMaxItems) return false;  // a fourth item is never sent
    size_t itemEnd = 0;
    const size_t lenAt = r.pos + 1;
    if (!r.expectLength(0x0A, itemEnd)) return false;
    if (varintLen((uint32_t)(itemEnd - r.pos)) != r.pos - lenAt) return false;
    // 08 <type, one byte> 12 <len> text 18 00, ending exactly at itemEnd.
    if (!r.expectByte(0x08) || r.pos >= itemEnd || !typeDrawn(pb[r.pos])) return false;
    const uint8_t type = pb[r.pos++];
    uint32_t textLen = 0;
    const size_t textLenAt = r.pos + 1;
    if (!r.expectByte(0x12) || !r.varint(textLen) || textLen == 0 ||
        textLen > kMaxTextBytes || varintLen(textLen) != r.pos - textLenAt ||
        textLen > itemEnd - r.pos)
      return false;
    for (size_t i = 0; i < textLen; i++)
      if (!textByteOk(pb[r.pos + i])) return false;
    if (out) {
      Line& l = out->lines[count];
      l.type = type;
      l.len = (uint16_t)textLen;
      memcpy(l.text, pb + r.pos, textLen);
      l.text[textLen] = '\0';
    }
    r.pos += textLen;
    if (!r.expectByte(0x18) || !r.expectByte(0x00) || r.pos != itemEnd) return false;
    count++;
  }
  if (count == 0) return false;
  if (!r.expectByte(0x10) || !r.expectByte(0x00) || !r.expectByte(0x18) ||
      !r.expectByte(kStatusHasData) || r.pos != len)
    return false;
  if (magicOut) *magicOut = (uint8_t)magic;
  if (out) out->count = count;
  return true;
}

// Glasses ACK: {1 4, 2 magic, 6 {1 {1 type} per stored item, 3 status}}.
// Fields inside 6 other than 1 and 3 are skipped when well-formed.
struct Ack {
  uint8_t magic = 0;
  uint8_t itemCount = 0;  // items echoed (all of them, even past the array)
  uint8_t types[8] = {};
  bool haveStatus = false;
  uint8_t status = 0;
};

inline bool skipField(Reader& r, uint32_t wire) {
  uint32_t v = 0;
  switch (wire) {
    case 0: return r.varint(v);
    case 2:
      if (!r.varint(v) || v > r.len - r.pos) return false;
      r.pos += v;
      return true;
    case 5:
      if (r.len - r.pos < 4) return false;
      r.pos += 4;
      return true;
    case 1:
      if (r.len - r.pos < 8) return false;
      r.pos += 8;
      return true;
    default: return false;
  }
}

inline bool parseAck(const uint8_t* pb, size_t len, Ack& out) {
  out = Ack();
  uint8_t cmd = 0, magic = 0;
  if (!hw1_g2_feed::parseCmdMagic(pb, len, cmd, magic) || cmd != kCmdHighlight) return false;
  out.magic = magic;
  Reader r{pb, len, 0};
  uint32_t v = 0;
  if (!r.expectByte(0x08) || !r.varint(v) || !r.expectByte(0x10) || !r.varint(v)) return false;
  while (r.pos < len) {
    uint32_t key = 0;
    if (!r.varint(key)) return false;
    if (key != 0x32) {
      if (!skipField(r, key & 7)) return false;
      continue;
    }
    uint32_t n = 0;
    if (!r.varint(n) || n > len - r.pos) return false;
    const size_t end = r.pos + n;
    Reader b{pb, end, r.pos};
    while (b.pos < end) {
      uint32_t k = 0;
      if (!b.varint(k)) return false;
      if (k == 0x0A) {
        uint32_t m = 0;
        if (!b.varint(m) || m > end - b.pos) return false;
        Reader it{pb, b.pos + m, b.pos};
        uint32_t type = 0;
        bool haveType = false;
        while (it.pos < it.len) {
          uint32_t ik = 0;
          if (!it.varint(ik)) return false;
          if (ik == 0x08) {
            if (!it.varint(type)) return false;
            haveType = true;
          } else if (!skipField(it, ik & 7)) {
            return false;
          }
        }
        if (out.itemCount < sizeof out.types) out.types[out.itemCount] = haveType && type <= 0xFF ? (uint8_t)type : 0;
        if (out.itemCount < 0xFF) out.itemCount++;
        b.pos += m;
      } else if (k == 0x18) {
        uint32_t s = 0;
        if (!b.varint(s)) return false;
        out.haveStatus = true;
        out.status = s > 0xFF ? 0xFF : (uint8_t)s;
      } else if (!skipField(b, k & 7)) {
        return false;
      }
    }
    r.pos = end;
  }
  return true;
}

// ---------------------------------------------------------------- aggregation
// One metric over hourly buckets {average, minimum, maximum}: lo = lowest
// hourly minimum, hi = highest hourly maximum, mean = mean of the hourly
// averages weighted by hours present. A bucket whose average is outside the
// metric's plausible range is skipped; a minimum/maximum that is missing (0)
// or on the wrong side of the average falls back to the average.

struct Range {
  uint16_t floor;
  uint16_t ceil;
};
static constexpr Range kHrRange = {30, 220};
static constexpr Range kHrvRange = {1, 300};
static constexpr Range kSpo2Range = {70, 100};

struct MetricAgg {
  uint32_t hours = 0;
  uint32_t sum = 0;
  uint16_t lo = 0;
  uint16_t hi = 0;
};

inline void addHour(MetricAgg& a, const Range& r, bool valid, uint16_t avg, uint16_t mn,
                    uint16_t mx) {
  if (!valid || avg < r.floor || avg > r.ceil) return;
  const uint16_t lo = (mn >= r.floor && mn <= avg) ? mn : avg;
  const uint16_t hi = (mx >= avg && mx <= r.ceil) ? mx : avg;
  if (a.hours == 0 || lo < a.lo) a.lo = lo;
  if (a.hours == 0 || hi > a.hi) a.hi = hi;
  a.hours++;
  a.sum += avg;
}

inline void mergeAgg(MetricAgg& into, const MetricAgg& from) {
  if (from.hours == 0) return;
  if (into.hours == 0 || from.lo < into.lo) into.lo = from.lo;
  if (into.hours == 0 || from.hi > into.hi) into.hi = from.hi;
  into.hours += from.hours;
  into.sum += from.sum;
}

inline uint32_t meanRounded(const MetricAgg& a) {
  return a.hours ? (a.sum + a.hours / 2) / a.hours : 0;
}

static constexpr uint32_t kStepsMax = 999999;
static constexpr uint32_t kSleepSecondsMax = 86400;

// One local day of ring history. Sleep is the night that ENDED on this day.
struct DaySummary {
  uint32_t dayStart = 0;
  bool isToday = false;  // the current, partial day
  MetricAgg hr, hrv, spo2;
  bool haveSteps = false;
  uint32_t steps = 0;
  bool haveSleep = false;
  uint32_t asleepSec = 0;
  bool hasData() const {
    return hr.hours || hrv.hours || spo2.hours || haveSteps || haveSleep;
  }
};

inline void setSteps(DaySummary& d, bool available, uint32_t steps) {
  d.haveSteps = available;
  d.steps = available ? (steps > kStepsMax ? kStepsMax : steps) : 0;
}

inline void setSleep(DaySummary& d, bool available, uint32_t asleepSec) {
  d.haveSleep = available && asleepSec > 0;
  d.asleepSec = d.haveSleep ? (asleepSec > kSleepSecondsMax ? kSleepSecondsMax : asleepSec) : 0;
}

// Window: the first `wantDays` (clamped 1..7) of `days` (newest first) that
// have any data. Returns the count copied to `out` (room for 7).
static constexpr uint8_t kMinDays = 1, kMaxDays = 7, kDefaultDays = 3;
inline uint8_t clampDays(int d) {
  return d < kMinDays ? kMinDays : (d > kMaxDays ? kMaxDays : (uint8_t)d);
}

inline size_t selectWindow(const DaySummary* days, size_t n, int wantDays, DaySummary* out) {
  const uint8_t want = clampDays(wantDays);
  size_t k = 0;
  for (size_t i = 0; i < n && k < want; i++)
    if (days[i].hasData()) out[k++] = days[i];
  return k;
}

// Two day keys name the same calendar day when they are at most an hour apart
// (a DST change moves a day's local midnight by the offset change; distinct
// calendar days are at least 23 h apart).
static constexpr uint32_t kDstSlackSec = 3600;
inline bool sameCalendarDay(uint32_t a, uint32_t b) {
  return (a > b ? a - b : b - a) <= kDstSlackSec;
}

// The adapter's day list: the RAM day (`ram`, may be null; summarized fresh)
// merged with the stored days of the last scan (`past`, any order), newest
// first, at most `cap`. A stored copy of the RAM day's calendar day is dropped
// (the RAM copy is the newer one). Every day on `todayStart`'s calendar day is
// marked isToday, whichever copy it came from, so today's partial steps never
// count as a complete day; every other day is marked complete. Days without
// data are dropped.
inline size_t mergeDays(const DaySummary* ram, const DaySummary* past, size_t pastCount,
                        uint32_t todayStart, DaySummary* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  auto insert = [&](const DaySummary& d) {
    if (!d.hasData()) return;
    size_t at = n;  // newest first: after every day that starts later
    while (at > 0 && out[at - 1].dayStart < d.dayStart) at--;
    if (at >= cap) return;  // full and older than everything kept
    const size_t last = n < cap ? n : cap - 1;
    for (size_t j = last; j > at; j--) out[j] = out[j - 1];
    out[at] = d;
    out[at].isToday = sameCalendarDay(d.dayStart, todayStart);
    if (n < cap) n++;
  };
  const bool ramHas = ram && ram->hasData();
  if (ramHas) insert(*ram);
  for (size_t i = 0; past && i < pastCount; i++) {
    if (ramHas && sameCalendarDay(past[i].dayStart, ram->dayStart)) continue;
    insert(past[i]);
  }
  return n;
}

// What a scan of the stored days depends on. Any change means the stored
// list is stale and the store is searched again. The RAM day is part of it:
// the scan skips the RAM day's calendar day (merged fresh), so when the RAM
// day moves (a new day's first sweep, a backlog sweep of an older day) the
// day it used to be must be read from the store.
struct ScanKey {
  char peer[18] = {};
  uint32_t todayStart = 0;
  uint32_t ramDayStart = 0;
  int16_t ramTz = 0;
  uint32_t committedGeneration = 0;
};
inline bool sameScanKey(const ScanKey& a, const ScanKey& b) {
  return strncmp(a.peer, b.peer, sizeof a.peer) == 0 && a.todayStart == b.todayStart &&
         a.ramDayStart == b.ramDayStart && a.ramTz == b.ramTz &&
         a.committedGeneration == b.committedGeneration;
}

// ---------------------------------------------------------------- formatting

// "6,400"
inline void formatThousands(char* out, size_t cap, uint32_t v) {
  char digits[12];
  const int n = snprintf(digits, sizeof digits, "%u", (unsigned)v);
  char tmp[16];
  size_t o = 0;
  for (int i = 0; i < n && o + 1 < sizeof tmp; i++) {
    if (i > 0 && (n - i) % 3 == 0) tmp[o++] = ',';
    tmp[o++] = digits[i];
  }
  tmp[o] = '\0';
  snprintf(out, cap, "%s", tmp);
}

// "7h05" from seconds, rounded to the minute.
inline void formatHm(char* out, size_t cap, uint32_t seconds) {
  const uint32_t m = (seconds + 30) / 60;
  snprintf(out, cap, "%uh%02u", (unsigned)(m / 60), (unsigned)(m % 60));
}

inline void formatRangeAvg(char* out, size_t cap, const char* label, const MetricAgg& a,
                           const char* unit) {
  snprintf(out, cap, "%s %u-%u%s, avg %u", label, (unsigned)a.lo, (unsigned)a.hi, unit,
           (unsigned)meanRounded(a));
}

// The gap row between two parts of one line ("\n \n": an empty row renders
// only with a space on it).
static constexpr char kGapRow[] = "\n \n";
// Two gap rows above the window note at the end of the last line (owner,
// 2026-11-21: the note sits apart from the data).
static constexpr char kTailGap[] = "\n \n \n";

// Appends kTailGap + `tail` to `l` when the result fits kMaxTextBytes; when it
// does not, `l` is left unchanged (the tail is dropped, the data is never cut).
inline bool appendTail(Line& l, const char* tail) {
  if (!tail || !tail[0] || l.len == 0) return false;
  const size_t gap = sizeof kTailGap - 1;
  const size_t t = strlen(tail);
  if (l.len + gap + t > kMaxTextBytes) return false;
  for (size_t i = 0; i < t; i++)
    if (!textByteOk((uint8_t)tail[i])) return false;
  memcpy(l.text + l.len, kTailGap, gap);
  memcpy(l.text + l.len + gap, tail, t);
  l.len = (uint16_t)(l.len + gap + t);
  l.text[l.len] = '\0';
  return true;
}

// The owner-approved layout (2026-11-21, window text moved the same day):
//   1 heart rate: "HR lo-hi bpm, avg a" "\n \n" "HRV lo-hi ms, avg a"
//     (HR only / HRV only when the other has no data; HRV alone uses the HRV icon)
//   2 steps: "Steps lo-hi/day, avg a" over complete days (today excluded),
//     "Steps today n" when only today has steps
//   3 sleep: "Sleep 6h10-7h40, avg 7h05"; without sleep "SpO2 lo-hi%, avg a"
// The window note ("Averages of the past 3 days", "... past day", "So far
// today") ends the last line after two gap rows:
// "Sleep 6h10-7h40, avg 7h05\n \n \nAverages of the past 3 days". It is
// dropped, not the data, when that line has no room for it.
// `window` is newest first, as selectWindow() returns it. No days -> count 0.
// Fills `out` in place (the adapter keeps it in PSRAM, off the main-loop stack).
inline void buildInsightsInto(const DaySummary* window, size_t n, Insights& out) {
  out.count = 0;  // lines are written by addLine (no 780-byte temporary)
  if (!window || n == 0) return;
  char windowText[40];
  if (n == 1)
    snprintf(windowText, sizeof windowText, "%s",
             window[0].isToday ? "So far today" : "Averages of the past day");
  else snprintf(windowText, sizeof windowText, "Averages of the past %u days", (unsigned)n);

  MetricAgg hr, hrv, spo2;
  uint32_t stepsLo = 0, stepsHi = 0, stepDays = 0, todaySteps = 0;
  uint64_t stepsSum = 0;
  bool todayHasSteps = false;
  uint32_t sleepLo = 0, sleepHi = 0, nights = 0;
  uint64_t sleepSum = 0;
  for (size_t i = 0; i < n; i++) {
    const DaySummary& d = window[i];
    mergeAgg(hr, d.hr);
    mergeAgg(hrv, d.hrv);
    mergeAgg(spo2, d.spo2);
    if (d.haveSteps) {
      if (d.isToday) {
        todayHasSteps = true;
        todaySteps = d.steps;
      } else {
        if (!stepDays || d.steps < stepsLo) stepsLo = d.steps;
        if (!stepDays || d.steps > stepsHi) stepsHi = d.steps;
        stepsSum += d.steps;
        stepDays++;
      }
    }
    if (d.haveSleep) {
      if (!nights || d.asleepSec < sleepLo) sleepLo = d.asleepSec;
      if (!nights || d.asleepSec > sleepHi) sleepHi = d.asleepSec;
      sleepSum += d.asleepSec;
      nights++;
    }
  }

  char text[160];  // the longest line is about 90 bytes; addLine clamps anyway
  char a[48], b[48];
  if (hr.hours) {
    formatRangeAvg(a, sizeof a, "HR", hr, " bpm");
    if (hrv.hours) {
      formatRangeAvg(b, sizeof b, "HRV", hrv, " ms");
      snprintf(text, sizeof text, "%s%s%s", a, kGapRow, b);
      addLine(out, kHeartRate, text);
    } else {
      addLine(out, kHeartRate, a);
    }
  } else if (hrv.hours) {
    formatRangeAvg(b, sizeof b, "HRV", hrv, " ms");
    addLine(out, kHrv, b);
  }

  if (stepDays) {
    char lo[16], hi[16], avg[16];
    formatThousands(lo, sizeof lo, stepsLo);
    formatThousands(hi, sizeof hi, stepsHi);
    formatThousands(avg, sizeof avg, (uint32_t)((stepsSum + stepDays / 2) / stepDays));
    if (stepsLo == stepsHi) snprintf(text, sizeof text, "Steps %s/day", lo);
    else snprintf(text, sizeof text, "Steps %s-%s/day, avg %s", lo, hi, avg);
    addLine(out, kSteps, text);
  } else if (todayHasSteps) {
    char t[16];
    formatThousands(t, sizeof t, todaySteps);
    snprintf(text, sizeof text, "Steps today %s", t);
    addLine(out, kSteps, text);
  }

  if (nights) {
    char lo[16], hi[16], avg[16];
    formatHm(lo, sizeof lo, sleepLo);
    formatHm(hi, sizeof hi, sleepHi);
    formatHm(avg, sizeof avg, (uint32_t)((sleepSum + nights / 2) / nights));
    if (strcmp(lo, hi) == 0) snprintf(text, sizeof text, "Sleep %s", lo);
    else snprintf(text, sizeof text, "Sleep %s-%s, avg %s", lo, hi, avg);
    addLine(out, kSleep, text);
  } else if (spo2.hours) {
    snprintf(text, sizeof text, "SpO2 %u-%u%%, avg %u", (unsigned)spo2.lo, (unsigned)spo2.hi,
             (unsigned)meanRounded(spo2));
    addLine(out, kSpo2, text);
  }

  // Line 3 when there is one, else the last line there is.
  if (out.count) appendTail(out.lines[out.count - 1], windowText);
}

inline Insights buildInsights(const DaySummary* window, size_t n) {
  Insights out;
  buildInsightsInto(window, n, out);
  return out;
}

// ---------------------------------------------------------------- policy
// The shared dashboard-feed policy (G2_FeedCore.h). Key = the published
// version, which the adapter bumps only when the body bytes change. The settle
// is longer than the MULT_DATA snapshot's (2 s), so after a connect the
// snapshot goes first; the adapter also holds this feed while a snapshot
// waits for its echo, and the reverse.

static constexpr uint32_t kConnectSettleMs = 5000;
static constexpr uint32_t kMinIntervalMs = 60000;
static constexpr uint32_t kAckTimeoutMs = 3000;
static constexpr uint8_t kMaxRetries = 1;
static constexpr hw1_g2_feed::Timing kTiming = {kConnectSettleMs, kMinIntervalMs, kAckTimeoutMs,
                                                kMaxRetries};

using Policy = hw1_g2_feed::Policy<uint32_t>;

// `haveMessage` = the main loop has computed a message (it may be the
// no-insights one: that is data too, the window simply has none).
inline hw1_g2_feed::Action policyTick(Policy& p, bool enabled, bool haveMessage, bool ready,
                                      uint32_t generation, uint32_t nowMs, uint32_t version) {
  return hw1_g2_feed::tick(p, kTiming, enabled, haveMessage, ready, generation, nowMs, version);
}

// The unit's echo: cmd 4 with the magic in flight.
inline bool policyAck(Policy& p, uint32_t rxGeneration, const Ack& a) {
  return hw1_g2_feed::onAck(p, rxGeneration, kCmdHighlight, kCmdHighlight, a.magic) ==
         hw1_g2_feed::AckResult::Complete;
}

// "1".."7" only.
inline bool parseDaysArg(const char* s, uint8_t& out) {
  if (!s || s[0] < '1' || s[0] > '7' || s[1] != '\0') return false;
  out = (uint8_t)(s[0] - '0');
  return true;
}

}  // namespace hw1_g2_health_insights
