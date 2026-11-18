// System_ESPNow_SensorCore.h - rules of the sensor reading path over ESP-NOW
//
// One numbering of the sensor types, the reading frame's header, how a
// reading is cut into frames and put together again, how a receiver counts
// lost readings, and where a reading goes. Pure logic on caller-owned state:
// no heap, no OS, no logging, so the rules run on a host as they run on the
// device (test/host/test_espnow_sensor_core.cpp). The owner
// (System_ESPNow_Sensors.cpp) holds the state and the locks.
//
// A reading is the sender's JSON text of one sensor. It travels in 1 to 4
// frames ("parts") of opcode SENSOR_ENVELOPE (154), or BOND_SENSOR_DATA (179)
// to a bond partner; each part starts with the 12-byte header below. A part
// is an ordinary single frame, also through a relay, where a frame carries
// less. The largest reading is 472 bytes on every path.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The sensor types. This order is the one numbering: the reading frame, the
// status frame's masks, the SENSOR_REQ mask, the capability and bond status
// masks (CAP_SENSOR_* in System_ESPNow.h are sensorBit() of these) and the
// receiver's table all use it. The numbers are on the wire: add at the end,
// never renumber.
// ---------------------------------------------------------------------------
enum RemoteSensorType {
  REMOTE_SENSOR_THERMAL = 0,
  REMOTE_SENSOR_TOF = 1,
  REMOTE_SENSOR_IMU = 2,
  REMOTE_SENSOR_GPS = 3,
  REMOTE_SENSOR_INPUT = 4,
  REMOTE_SENSOR_FMRADIO = 5,
  REMOTE_SENSOR_CAMERA = 6,
  REMOTE_SENSOR_MICROPHONE = 7,
  REMOTE_SENSOR_RTC = 8,
  REMOTE_SENSOR_PRESENCE = 9,
  REMOTE_SENSOR_APDS = 10,
  REMOTE_SENSOR_MAX
};

namespace espnowsensor {

struct SensorRow {
  uint8_t id;             // RemoteSensorType
  const char* name;       // on the wire and in commands
  const char* shortName;  // OLED
  const char* longName;   // web and CLI lists
};

// One row per type, in RemoteSensorType order.
inline constexpr SensorRow kSensorTable[REMOTE_SENSOR_MAX] = {
  { REMOTE_SENSOR_THERMAL,    "thermal",    "Therm", "Thermal Camera" },
  { REMOTE_SENSOR_TOF,        "tof",        "ToF",   "Time-of-Flight" },
  { REMOTE_SENSOR_IMU,        "imu",        "IMU",   "IMU/Accelerometer" },
  { REMOTE_SENSOR_GPS,        "gps",        "GPS",   "GPS" },
  { REMOTE_SENSOR_INPUT,      "input",      "Input", "Input Device" },
  { REMOTE_SENSOR_FMRADIO,    "fmradio",    "FM",    "FM Radio" },
  { REMOTE_SENSOR_CAMERA,     "camera",     "Cam",   "Camera" },
  { REMOTE_SENSOR_MICROPHONE, "microphone", "Mic",   "Microphone" },
  { REMOTE_SENSOR_RTC,        "rtc",        "RTC",   "Real-Time Clock" },
  { REMOTE_SENSOR_PRESENCE,   "presence",   "Pres",  "Presence Sensor" },
  { REMOTE_SENSOR_APDS,       "apds",       "APDS",  "Gesture/Color" },
};

// The mask bit of a sensor type, in every mask that names sensors.
constexpr uint32_t sensorBit(uint8_t id) { return (uint32_t)1u << id; }
constexpr uint32_t kSensorMaskAll = ((uint32_t)1u << REMOTE_SENSOR_MAX) - 1u;

inline const char* sensorName(int id) {
  return (id >= 0 && id < REMOTE_SENSOR_MAX) ? kSensorTable[id].name : "unknown";
}

// False for a name that is not a sensor type; `out` is then left alone.
inline bool sensorIdFromName(const char* name, uint8_t& out) {
  if (!name) return false;
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    if (strcmp(name, kSensorTable[i].name) == 0) { out = i; return true; }
  }
  return false;
}

// The names of the types in `mask`, comma separated (short or long form),
// in table order. Bits above the table are left out. Returns the length
// written; a list that does not fit is cut at a whole name.
inline size_t sensorMaskNames(uint32_t mask, bool longForm, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t pos = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    if (!(mask & sensorBit(i))) continue;
    const char* name = longForm ? kSensorTable[i].longName : kSensorTable[i].shortName;
    const size_t nameLen = strlen(name);
    if (pos + nameLen + (pos ? 1 : 0) >= cap) break;
    if (pos) out[pos++] = ',';
    memcpy(out + pos, name, nameLen);
    pos += nameLen;
    out[pos] = '\0';
  }
  return pos;
}

// ---------------------------------------------------------------------------
// How long ago a millis() stamp was. A task loop reads the uptime once and
// hands it to its steps, so a stamp another task took after that read is a
// little ahead of `nowMs`: such a stamp (up to a minute ahead) is 0 ms old,
// not 49 days. Wrap-safe otherwise.
// ---------------------------------------------------------------------------
constexpr uint32_t kStampAheadSlackMs = 60000;
constexpr uint32_t elapsedSinceMs(uint32_t nowMs, uint32_t stampMs) {
  return (uint32_t)(stampMs - nowMs) <= kStampAheadSlackMs ? 0u : (uint32_t)(nowMs - stampMs);
}

// ---------------------------------------------------------------------------
// The reading header (12 bytes, little endian), as V4PayloadSensorReading in
// System_ESPNow_Wire.h lays it out:
//
//   0  sensorType   RemoteSensorType
//   1  flags        bit 0: ageMs is filled; bit 1: first reading of a stream
//                   to this target; bit 2: forwarded (the reading is the
//                   sender's bond partner's, System_ESPNow_ForwardCore.h);
//                   other bits 0, ignored on receive
//   2  bootTag      16 bit: the same for every frame the sender sends in one boot
//   4  counter      16 bit: counts this sender's readings of this sensor. The
//                   first after boot is 1; 65535 is followed by 0. All parts
//                   of one reading carry the same value
//   6  parts        low nibble: this part's index (0-based);
//                   high nibble: number of parts (1 to 4)
//   7  fwdLink      the bond master's link number when bit 2 is set, else 0
//                   (bit 2 set if and only if this byte is not 0)
//   8  ageMs        32 bit: how old the reading already was when it was sent.
//                   0 today (readings are built when they are sent); for a
//                   later sender that keeps readings and sends them afterwards
//  12  data         this part's bytes of the reading's text, no terminator;
//                   the length is the frame's payload length minus 12
// ---------------------------------------------------------------------------
constexpr size_t   kReadingHeaderLen  = 12;
constexpr uint8_t  kReadingMaxParts   = 4;
constexpr uint16_t kFrameCapDirect    = 202;  // ESPNOW_V4_MAX_PLAINTEXT
constexpr uint16_t kFrameCapRouted    = 130;  // ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT
constexpr uint16_t kPartMaxDirect     = kFrameCapDirect - kReadingHeaderLen;   // 190
constexpr uint16_t kPartMaxRouted     = kFrameCapRouted - kReadingHeaderLen;   // 118
// The stated maximum of one reading: four parts through a relay.
constexpr uint16_t kReadingMaxBytes   = kReadingMaxParts * kPartMaxRouted;      // 472

constexpr uint8_t kReadingFlagAgeValid    = 0x01;
constexpr uint8_t kReadingFlagStreamStart = 0x02;
constexpr uint8_t kReadingFlagForwarded   = 0x04;

struct ReadingHeader {
  uint8_t  sensorType;
  uint8_t  flags;
  uint16_t bootTag;
  uint16_t counter;
  uint8_t  partIndex;
  uint8_t  partCount;
  uint32_t ageMs;
  uint8_t  fwdLink;  // byte 7: 0, or the link number of a forwarded reading
};

inline void packReadingHeader(uint8_t out[kReadingHeaderLen], const ReadingHeader& h) {
  out[0] = h.sensorType;
  out[1] = h.flags;
  out[2] = (uint8_t)(h.bootTag & 0xFF);
  out[3] = (uint8_t)(h.bootTag >> 8);
  out[4] = (uint8_t)(h.counter & 0xFF);
  out[5] = (uint8_t)(h.counter >> 8);
  out[6] = (uint8_t)((h.partIndex & 0x0F) | ((h.partCount & 0x0F) << 4));
  out[7] = h.fwdLink;
  out[8]  = (uint8_t)(h.ageMs & 0xFF);
  out[9]  = (uint8_t)((h.ageMs >> 8) & 0xFF);
  out[10] = (uint8_t)((h.ageMs >> 16) & 0xFF);
  out[11] = (uint8_t)((h.ageMs >> 24) & 0xFF);
}

// False when the payload is shorter than a header. Nothing else is judged
// here; partAccept() refuses a header that makes no sense.
inline bool parseReadingHeader(const uint8_t* in, size_t len, ReadingHeader& h) {
  if (!in || len < kReadingHeaderLen) return false;
  h.sensorType = in[0];
  h.flags      = in[1];
  h.bootTag    = (uint16_t)(in[2] | ((uint16_t)in[3] << 8));
  h.counter    = (uint16_t)(in[4] | ((uint16_t)in[5] << 8));
  h.partIndex  = (uint8_t)(in[6] & 0x0F);
  h.partCount  = (uint8_t)(in[6] >> 4);
  h.fwdLink    = in[7];
  h.ageMs      = (uint32_t)in[8] | ((uint32_t)in[9] << 8) | ((uint32_t)in[10] << 16) |
                 ((uint32_t)in[11] << 24);
  return true;
}

// ---------------------------------------------------------------------------
// Cutting a reading into parts (sender).
// ---------------------------------------------------------------------------
struct PartPlan {
  uint8_t  count;     // 1 to kReadingMaxParts
  uint16_t partSize;  // data bytes of every part but the last
};

// `frameCap` is what one sealed frame to the target carries (202 direct, 130
// through a relay). False for an empty reading, one over kReadingMaxBytes, or
// a frame too small to carry it in four parts.
inline bool planParts(size_t len, uint16_t frameCap, PartPlan& plan) {
  plan.count = 0;
  plan.partSize = 0;
  if (len == 0 || len > kReadingMaxBytes || frameCap <= kReadingHeaderLen) return false;
  const uint16_t partSize = (uint16_t)(frameCap - kReadingHeaderLen);
  const size_t count = (len + partSize - 1) / partSize;
  if (count > kReadingMaxParts) return false;
  plan.count = (uint8_t)count;
  plan.partSize = partSize;
  return true;
}

// Where part `index` of a `len`-byte reading starts and how long it is.
inline void partSpan(const PartPlan& plan, size_t len, uint8_t index, size_t& offset, size_t& n) {
  offset = (size_t)index * plan.partSize;
  n = (offset >= len) ? 0 : ((len - offset < plan.partSize) ? len - offset : plan.partSize);
}

// ---------------------------------------------------------------------------
// The sender's counter and "first reading of a stream" flag.
// ---------------------------------------------------------------------------

// The next counter value: 1 after boot, 0 after 65535.
inline uint16_t counterNext(uint16_t& state) { return ++state; }

// Per sensor on the sender: who the last reading went to. The stream-start
// flag goes on the first reading to a target: after boot, after the target
// changed, and after the stream was switched on (streamRestart).
struct StreamTarget {
  bool    have;
  bool    startPending;
  uint8_t mac[6];
};

inline void streamRestart(StreamTarget& s) { s.startPending = true; }

// True when the next reading to `mac` must carry kReadingFlagStreamStart.
inline bool streamStartWanted(const StreamTarget& s, const uint8_t mac[6]) {
  return !s.have || s.startPending || memcmp(s.mac, mac, 6) != 0;
}

// A reading went out to `mac` (at least its first part was handed to the
// radio queue). Until then the flag stays wanted, so a reading that was
// dropped does not take the stream start with it.
inline void streamSentTo(StreamTarget& s, const uint8_t mac[6]) {
  s.have = true;
  s.startPending = false;
  memcpy(s.mac, mac, 6);
}

// ---------------------------------------------------------------------------
// The receiver's counter rule. One CounterTrack and one CounterCounts per
// sender and counted stream (a sensor type; the battery frame; the event
// frame). Repeated frames never get here in practice (the session's replay
// window drops them first); the Repeat verdict is a backstop.
//
//   nothing seen yet                          First    kept
//   another boot tag                          Restart  kept, restarts +1
//   same counter as the last kept             Repeat   dropped, repeated +1
//   stream-start flag                         Resume   kept, resumes +1, no loss
//   1 <= d < 32768 (d = counter - last)       New      kept, lost += d - 1
//   older by 256 or less (d >= 65280)         Late     dropped, late +1
//   older by more, last kept under 10 s ago   Late     dropped, late +1
//   ... the eighth Late in a row              Restart  kept, restarts +1
//   older by more, last kept 10 s ago or more Resume   kept, resumes +1
//
// `lost` counts readings the sender numbered and this receiver did not get
// in order; a Late one was counted lost before it arrived, so lost - late is
// what never arrived. A paused stream is not loss: the counter only moves
// when the sender sends.
//
// "Older by 256 or less" is what a frame held back on the way can be (a
// relay that holds it; the session's replay window drops anything older):
// it never replaces a newer one, however long the silence was. A counter
// "older" by more than that after a silence is a stream that went on while
// this receiver heard nothing (more than 32768 readings) and is kept.
// ---------------------------------------------------------------------------
constexpr uint32_t kLateWindowMs   = 10000;
constexpr uint8_t  kLateRunRestart = 8;
constexpr uint16_t kLateMaxBack    = 256;

struct CounterTrack {
  bool     have;
  uint16_t bootTag;
  uint16_t last;
  uint8_t  lateRun;
  uint32_t lastKeptMs;
};

struct CounterCounts {
  uint32_t received;  // kept
  uint32_t lost;
  uint32_t late;
  uint32_t repeated;
  uint32_t restarts;
  uint32_t resumes;
};

enum class CounterVerdict : uint8_t { First, New, Repeat, Late, Restart, Resume };

inline bool counterVerdictKeeps(CounterVerdict v) {
  return v != CounterVerdict::Repeat && v != CounterVerdict::Late;
}

inline const char* counterVerdictName(CounterVerdict v) {
  switch (v) {
    case CounterVerdict::First:   return "first";
    case CounterVerdict::New:     return "new";
    case CounterVerdict::Repeat:  return "repeat";
    case CounterVerdict::Late:    return "late";
    case CounterVerdict::Restart: return "restart";
    case CounterVerdict::Resume:  return "resume";
  }
  return "?";
}

inline CounterVerdict counterJudge(CounterTrack& t, CounterCounts& c, uint16_t bootTag,
                                   uint16_t counter, bool streamStart, uint32_t nowMs) {
  auto keep = [&](CounterVerdict v) {
    t.have = true;
    t.bootTag = bootTag;
    t.last = counter;
    t.lateRun = 0;
    t.lastKeptMs = nowMs;
    c.received++;
    return v;
  };
  if (!t.have) return keep(CounterVerdict::First);
  if (t.bootTag != bootTag) {
    c.restarts++;
    return keep(CounterVerdict::Restart);
  }
  const uint16_t d = (uint16_t)(counter - t.last);
  if (d == 0) {
    c.repeated++;
    t.lateRun = 0;
    return CounterVerdict::Repeat;
  }
  if (streamStart) {
    c.resumes++;
    return keep(CounterVerdict::Resume);
  }
  if (d < 32768u) {
    c.lost += (uint32_t)(d - 1);
    return keep(CounterVerdict::New);
  }
  const uint16_t back = (uint16_t)(t.last - counter);  // how much older, 1 to 32768
  if (back > kLateMaxBack && (uint32_t)(nowMs - t.lastKeptMs) >= kLateWindowMs) {
    c.resumes++;
    return keep(CounterVerdict::Resume);
  }
  c.late++;
  if (++t.lateRun >= kLateRunRestart) {
    c.restarts++;
    return keep(CounterVerdict::Restart);
  }
  return CounterVerdict::Late;
}

// ---------------------------------------------------------------------------
// Putting the parts together (receiver). One PartAssembly and one text
// buffer of kReadingMaxBytes per sender: a sender's parts of one reading go
// out back to back from one task, so one open reading per sender is enough.
// Readers keep the last complete reading while a new one arrives.
// ---------------------------------------------------------------------------
constexpr uint32_t kAssemblyMaxAgeMs = 2000;

struct PartAssembly {
  bool     open;
  uint8_t  sensorType;
  uint8_t  flags;      // of part 0
  uint8_t  next;       // index of the part expected next
  uint8_t  count;
  uint16_t bootTag;
  uint16_t counter;
  uint16_t len;        // bytes gathered so far
  uint32_t startedMs;
  // The reading whose later parts found no open context: counted once.
  bool     orphanHave;
  uint8_t  orphanSensor;
  uint16_t orphanBootTag;
  uint16_t orphanCounter;
};

enum class PartVerdict : uint8_t {
  Malformed,  // the header or the length makes no sense; nothing was changed
  Stored,     // kept; more parts are expected
  Complete,   // *text / *textLen hold the whole reading
  Dropped,    // a later part with no open reading to join
};

struct PartResult {
  PartVerdict verdict;
  uint8_t     incomplete;  // readings given up because of this part (0 to 2)
};

// `buf` (of `bufCap` >= kReadingMaxBytes bytes) belongs to this sender's
// PartAssembly. On Complete, *text points into `data` (one part) or `buf`.
inline PartResult partAccept(PartAssembly& a, uint8_t* buf, size_t bufCap, const ReadingHeader& h,
                             const uint8_t* data, size_t n, uint32_t nowMs,
                             const uint8_t** text, size_t* textLen) {
  PartResult r = { PartVerdict::Malformed, 0 };
  if (text) *text = nullptr;
  if (textLen) *textLen = 0;
  if (!buf || bufCap < kReadingMaxBytes || !data || n == 0 || n > kPartMaxDirect ||
      h.partCount == 0 || h.partCount > kReadingMaxParts || h.partIndex >= h.partCount ||
      h.sensorType >= REMOTE_SENSOR_MAX) {
    return r;
  }
  // Gives up the open reading: counted once, and remembered so that a part of
  // it that still arrives is not counted again.
  auto giveUpOpen = [&]() {
    a.open = false;
    a.orphanHave = true;
    a.orphanSensor = a.sensorType;
    a.orphanBootTag = a.bootTag;
    a.orphanCounter = a.counter;
    r.incomplete++;
  };
  // A reading left open too long is given up before anything else.
  if (a.open && (uint32_t)(nowMs - a.startedMs) > kAssemblyMaxAgeMs) giveUpOpen();
  auto sameReading = [&](uint8_t sensor, uint16_t tag, uint16_t counter) {
    return sensor == h.sensorType && tag == h.bootTag && counter == h.counter;
  };
  if (h.partCount == 1) {
    // An open reading of the same sensor is older than this one.
    if (a.open && a.sensorType == h.sensorType) giveUpOpen();
    if (text) *text = data;
    if (textLen) *textLen = n;
    r.verdict = PartVerdict::Complete;
    return r;
  }
  if (h.partIndex == 0) {
    if (a.open) giveUpOpen();  // the reading it replaces never finished
    a.open = true;
    a.sensorType = h.sensorType;
    a.flags = h.flags;
    a.next = 1;
    a.count = h.partCount;
    a.bootTag = h.bootTag;
    a.counter = h.counter;
    a.len = (uint16_t)n;
    a.startedMs = nowMs;
    memcpy(buf, data, n);
    r.verdict = PartVerdict::Stored;
    return r;
  }
  const bool joins = a.open && sameReading(a.sensorType, a.bootTag, a.counter) &&
                     a.count == h.partCount && a.next == h.partIndex &&
                     (size_t)a.len + n <= kReadingMaxBytes;
  if (!joins) {
    // Its reading cannot be finished: a part was lost, or it came out of order.
    if (a.open && sameReading(a.sensorType, a.bootTag, a.counter)) {
      giveUpOpen();
    } else if (!(a.orphanHave && sameReading(a.orphanSensor, a.orphanBootTag, a.orphanCounter))) {
      r.incomplete++;
      a.orphanHave = true;
      a.orphanSensor = h.sensorType;
      a.orphanBootTag = h.bootTag;
      a.orphanCounter = h.counter;
    }
    r.verdict = PartVerdict::Dropped;
    return r;
  }
  memcpy(buf + a.len, data, n);
  a.len = (uint16_t)(a.len + n);
  a.next++;
  if (a.next < a.count) {
    r.verdict = PartVerdict::Stored;
    return r;
  }
  a.open = false;
  if (text) *text = buf;
  if (textLen) *textLen = a.len;
  r.verdict = PartVerdict::Complete;
  return r;
}

// ---------------------------------------------------------------------------
// A reading's text is put into the receiver's own JSON replies as it came.
// So it must be one JSON object (RFC 8259) and nothing else: `{` first, the
// object closing at the last byte, every key a string, every value a string,
// number, true, false, null, object or array, brackets of the right kind, no
// trailing comma, strings with valid escapes, no control byte anywhere (no
// tab or line break between tokens either), no malformed UTF-8, at most 16
// levels deep. `{},"lost":0,"x":{` is the kind of text this
// refuses: it would add keys of the sender's choosing beside the receiver's
// own; `{"a":}` or `{"a":"\q"}` would make the receiver's reply invalid JSON.
// No recursion: the check runs on espnow_task's stack.
// ---------------------------------------------------------------------------

// The length of the well-formed UTF-8 character that starts at s[0] (2 to 4
// bytes, no overlong form, no surrogate, nothing over U+10FFFF), or 0.
inline size_t utf8CharLen(const uint8_t* s, size_t avail) {
  if (avail == 0) return 0;
  const uint8_t lead = s[0];
  size_t need = 0;
  uint32_t cp = 0;
  if (lead >= 0xC2 && lead <= 0xDF) { need = 1; cp = lead & 0x1Fu; }
  else if (lead >= 0xE0 && lead <= 0xEF) { need = 2; cp = lead & 0x0Fu; }
  else if (lead >= 0xF0 && lead <= 0xF4) { need = 3; cp = lead & 0x07u; }
  else return 0;
  if (need >= avail) return 0;
  for (size_t k = 1; k <= need; ++k) {
    if ((s[k] & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (s[k] & 0x3Fu);
  }
  const uint32_t least = need == 1 ? 0x80u : (need == 2 ? 0x800u : 0x10000u);
  if (cp < least || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
  return need + 1;
}

inline bool readingTextIsObject(const uint8_t* s, size_t n) {
  if (!s || n < 2 || s[0] != '{') return false;
  constexpr size_t kMaxDepth = 16;
  char stack[kMaxDepth];  // '{' or '[' per open level
  size_t depth = 0;
  size_t i = 0;
  auto isDigit = [&](size_t at) { return at < n && s[at] >= '0' && s[at] <= '9'; };
  auto isHex = [&](size_t at) {
    return at < n && ((s[at] >= '0' && s[at] <= '9') || (s[at] >= 'a' && s[at] <= 'f') || (s[at] >= 'A' && s[at] <= 'F'));
  };
  // Spaces only: a tab or a line break (JSON allows them) is a control byte
  // in a line-based reply, and no sender of this firmware writes one.
  auto skipSpace = [&]() {
    while (i < n && s[i] == ' ') ++i;
  };
  // A string at s[i]; i ends behind its closing quote.
  auto string = [&]() {
    if (i >= n || s[i] != '"') return false;
    for (++i; i < n;) {
      const uint8_t ch = s[i];
      if (ch == '"') { ++i; return true; }
      if (ch < 0x20) return false;
      if (ch == '\\') {
        if (i + 1 >= n) return false;
        const uint8_t e = s[i + 1];
        if (e == 'u') {
          if (!isHex(i + 2) || !isHex(i + 3) || !isHex(i + 4) || !isHex(i + 5)) return false;
          i += 6;
        } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
          i += 2;
        } else {
          return false;
        }
        continue;
      }
      if (ch >= 0x80) {
        const size_t len = utf8CharLen(s + i, n - i);
        if (len == 0) return false;
        i += len;
        continue;
      }
      ++i;
    }
    return false;
  };
  // A number, true, false or null at s[i].
  auto scalar = [&]() {
    const uint8_t ch = s[i];
    if (ch == 't' || ch == 'f' || ch == 'n') {
      const char* word = ch == 't' ? "true" : (ch == 'f' ? "false" : "null");
      const size_t len = strlen(word);
      if (n - i < len || memcmp(s + i, word, len) != 0) return false;
      i += len;
      return true;
    }
    if (ch == '-') ++i;
    if (!isDigit(i)) return false;
    if (s[i] == '0') ++i;
    else while (isDigit(i)) ++i;
    if (i < n && s[i] == '.') {
      ++i;
      if (!isDigit(i)) return false;
      while (isDigit(i)) ++i;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
      ++i;
      if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
      if (!isDigit(i)) return false;
      while (isDigit(i)) ++i;
    }
    return true;
  };
  // What may come next.
  enum class Want : uint8_t { Value, KeyOrClose, Key, Colon, ValueOrClose, CommaOrClose };
  Want want = Want::Value;
  for (;;) {
    skipSpace();
    if (i >= n) return false;
    const uint8_t ch = s[i];
    bool closed = false;
    switch (want) {
      case Want::KeyOrClose:
        if (ch == '}') { closed = true; break; }
        want = Want::Key;
        continue;
      case Want::Key:
        if (!string()) return false;
        want = Want::Colon;
        continue;
      case Want::Colon:
        if (ch != ':') return false;
        ++i;
        want = Want::Value;
        continue;
      case Want::ValueOrClose:
        if (ch == ']') { closed = true; break; }
        want = Want::Value;
        continue;
      case Want::Value:
        if (ch == '{' || ch == '[') {
          if (depth >= kMaxDepth) return false;
          stack[depth++] = (char)ch;
          ++i;
          want = ch == '{' ? Want::KeyOrClose : Want::ValueOrClose;
          continue;
        }
        if (depth == 0) return false;  // the outer value must be the object
        if (!(ch == '"' ? string() : scalar())) return false;
        want = Want::CommaOrClose;
        continue;
      case Want::CommaOrClose: {
        const char open = stack[depth - 1];
        if (ch == ',') {
          ++i;
          want = open == '{' ? Want::Key : Want::Value;
          continue;
        }
        if (ch != (open == '{' ? '}' : ']')) return false;
        closed = true;
        break;
      }
    }
    if (!closed) return false;
    ++i;
    --depth;
    if (depth == 0) return i == n;  // the object ends at the last byte
    want = Want::CommaOrClose;
  }
}

// ---------------------------------------------------------------------------
// Why a reading was not sent. A reading is never dropped silently: each
// reason is counted per sensor and reported (espnowsensorstatus, and one
// console line a minute while it goes on).
// ---------------------------------------------------------------------------
enum class DropReason : uint8_t {
  None = 0,
  TooLong,        // over kReadingMaxBytes
  NoReceiver,     // nobody to send to
  NoSession,      // the receiver has no session with this device yet
  QueueFull,      // the send queue took no more
  IsMaster,       // this device is the mesh master: it receives
  BondOffline,    // bond worker, partner not there
  BroadcastOff,   // espnowsensorbroadcast is off
  MeshOff,        // mesh mode is off
  NotInBuild,     // this firmware cannot read that sensor
  MasterNotPaired,  // the master named on this device is not paired with it
  Count
};
constexpr size_t kDropReasonCount = (size_t)DropReason::Count;

// The reason's name in JSON.
inline const char* dropReasonKey(DropReason r) {
  switch (r) {
    case DropReason::None:         return "none";
    case DropReason::TooLong:      return "too_long";
    case DropReason::NoReceiver:   return "no_receiver";
    case DropReason::NoSession:    return "no_session";
    case DropReason::QueueFull:    return "queue_full";
    case DropReason::IsMaster:     return "is_master";
    case DropReason::BondOffline:  return "bond_offline";
    case DropReason::BroadcastOff: return "broadcast_off";
    case DropReason::MeshOff:      return "mesh_off";
    case DropReason::NotInBuild:   return "not_in_this_build";
    case DropReason::MasterNotPaired: return "master_not_paired";
    case DropReason::Count:        break;
  }
  return "?";
}

// The reason in plain words, for "<sensor> readings not sent: <text>".
inline const char* dropReasonText(DropReason r) {
  switch (r) {
    case DropReason::None:         return "sent";
    case DropReason::TooLong:      return "the reading is over the 472-byte limit";
    case DropReason::NoReceiver:   return "no device has asked for them and no master is named on this device";
    case DropReason::NoSession:    return "the receiving device has no session with this device yet";
    case DropReason::QueueFull:    return "the send queue is full";
    case DropReason::IsMaster:     return "this device is the mesh master, which receives readings";
    case DropReason::BondOffline:  return "the bond partner is not connected";
    case DropReason::BroadcastOff: return "sensor sending is switched off (espnowsensorbroadcast on)";
    case DropReason::MeshOff:      return "mesh mode is off";
    case DropReason::NotInBuild:   return "this firmware cannot read that sensor";
    case DropReason::MasterNotPaired: return "the master named on this device is not paired with it";
    case DropReason::Count:        break;
  }
  return "?";
}

// True for the reasons that take a counter value first, so the receiver sees
// the gap. The others mean there was nobody to count for.
constexpr bool dropTakesCounter(DropReason r) {
  return r == DropReason::TooLong || r == DropReason::QueueFull;
}

// ---------------------------------------------------------------------------
// Who sends and who keeps, by mesh role (0 worker, 1 master, 2 backup master;
// a backup that took over carries 1). The same two rules for readings, the
// status frame, battery state and events:
//
//   role                     sends its own   keeps what it is sent
//   worker                   yes             no
//   backup master            yes             yes
//   master (also a backup    no              yes
//   that took over)
//
// A worker that is sent a reading refuses it and counts it. A bond master
// keeps its partner's readings whatever its mesh role: the owner of the table
// does not ask this rule for a bond frame.
// ---------------------------------------------------------------------------
constexpr uint8_t kRoleWorker = 0;
constexpr uint8_t kRoleMaster = 1;
constexpr uint8_t kRoleBackup = 2;

constexpr bool roleSends(uint8_t role) { return role != kRoleMaster; }
constexpr bool roleReceives(uint8_t role) { return role == kRoleMaster || role == kRoleBackup; }

// The role's name in JSON.
inline const char* roleKey(uint8_t role) {
  return role == kRoleMaster ? "master" : (role == kRoleBackup ? "backup" : "worker");
}

// ---------------------------------------------------------------------------
// Where a reading of one sensor goes. First match:
//   0. a bond worker with bond mode on: the bond partner (opcode 179), or
//      nowhere (BondOffline) while the partner is not connected;
//   then the mesh checks: sending switched off, mesh off, a role that only
//      receives;
//   1. a device asked for this sensor (SENSOR_REQ) and its lease runs: to
//      that device (the controller);
//   2. the stream was switched on by hand on this device: to the primary
//      master named on this device, when it is paired;
//   3. else nowhere: NoReceiver when no master is named (or nobody asked),
//      MasterNotPaired when one is named and is not paired.
// The status frame goes where a reading switched on by hand would go, or to
// the controller while one is set (the caller fills the inputs so).
// ---------------------------------------------------------------------------
struct TargetInputs {
  bool    bondWorker;      // bond mode on and this device is the bond worker
  bool    bondOnline;
  uint8_t bondPartner[6];
  bool    broadcastOn;     // espnowsensorbroadcast
  bool    meshOn;
  uint8_t meshRole;
  bool    leaseRunning;    // a controller asked for this sensor and its lease runs
  uint8_t controller[6];
  bool    byHand;          // the stream was switched on by hand on this device
  bool    masterNamed;     // a primary master is named on this device
  bool    havePrimary;     // ... and it is paired: `primary` is its address
  uint8_t primary[6];
};

struct TargetChoice {
  DropReason why;      // None: `mac` is the target
  bool       viaBond;  // send as BOND_SENSOR_DATA to the bond partner
  uint8_t    mac[6];
};

inline TargetChoice readingTarget(const TargetInputs& in) {
  TargetChoice out = {};
  if (in.bondWorker) {
    if (!in.bondOnline) { out.why = DropReason::BondOffline; return out; }
    out.viaBond = true;
    memcpy(out.mac, in.bondPartner, 6);
    return out;
  }
  if (!in.broadcastOn) { out.why = DropReason::BroadcastOff; return out; }
  if (!in.meshOn) { out.why = DropReason::MeshOff; return out; }
  if (!roleSends(in.meshRole)) { out.why = DropReason::IsMaster; return out; }
  if (in.leaseRunning) {
    memcpy(out.mac, in.controller, 6);
    return out;
  }
  if (in.byHand && in.havePrimary) {
    memcpy(out.mac, in.primary, 6);
    return out;
  }
  out.why = (in.byHand && in.masterNamed) ? DropReason::MasterNotPaired : DropReason::NoReceiver;
  return out;
}

// A lease that ends (it lapsed, or its controller unsubscribed) stops the
// stream it started. A stream that was switched on by hand goes on without
// the lease, to the named master.
constexpr bool streamSurvivesLeaseEnd(bool byHand) { return byHand; }

// ---------------------------------------------------------------------------
// The masters named on this device: espnowmasterfingerprint and
// espnowbackupfingerprint <64-hex | clear>. Each holds the device key that
// 'espnowidentity' shows on that master, exactly 64 hexadecimal characters
// (either case), or nothing: an empty setting names no device (also when it
// comes so from the web page or a settings file). No argument shows the
// value; "clear" (either case) empties it; anything else is refused.
// ---------------------------------------------------------------------------
constexpr size_t kFingerprintHexLen = 64;

inline bool fingerprintTextValid(const char* text, size_t len) {
  if (!text || len != kFingerprintHexLen) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

enum class FingerprintArg : uint8_t { Show, Clear, Set, Bad };

// `arg` is the command's argument with the spaces around it removed.
inline FingerprintArg fingerprintArgKind(const char* arg, size_t len) {
  if (!arg || len == 0) return FingerprintArg::Show;
  if (len == 5) {
    static const char kClear[] = "clear";
    bool same = true;
    for (size_t i = 0; i < 5 && same; ++i) {
      char c = arg[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      same = c == kClear[i];
    }
    if (same) return FingerprintArg::Clear;
  }
  return fingerprintTextValid(arg, len) ? FingerprintArg::Set : FingerprintArg::Bad;
}

// What a settings editor (the web page, the OLED editor) sends as the
// argument of `cmd` for the field text `value`: an emptied fingerprint field
// is 'clear', since an empty argument only shows the value; anything else as
// it is. `cmd` is the command name the setting declares (either case).
inline const char* fingerprintEditorArg(const char* cmd, const char* value) {
  if (!value) value = "";
  if (!cmd) return value;
  static const char* const kNames[2] = { "espnowmasterfingerprint", "espnowbackupfingerprint" };
  bool isFingerprint = false;
  for (const char* name : kNames) {
    size_t i = 0;
    for (; name[i] && cmd[i]; ++i) {
      char c = cmd[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != name[i]) break;
    }
    if (!name[i] && !cmd[i]) isFingerprint = true;
  }
  if (!isFingerprint) return value;
  for (const char* c = value; *c; ++c) {
    if (*c != ' ' && *c != '\t' && *c != '\r' && *c != '\n') return value;
  }
  return "clear";
}

// The stream switches a bond worker received from its bond master
// (BOND_STREAM_CTRL, one sensor each), kept per sensor until espnow_task
// applies them after the receive ring is drained. Several switches that
// arrive in one loop all count (a bond master switching two sensors sends two
// frames back to back); for one sensor the last one wins. Sensor numbers this
// firmware does not know are dropped.
struct StreamCtrlQueue {
  uint16_t pending;  // sensors with a switch to apply
  uint16_t on;       // of those: on (else off)
};

inline void streamCtrlPut(StreamCtrlQueue& q, uint8_t sensor, bool on) {
  if (sensor >= REMOTE_SENSOR_MAX) return;
  const uint16_t bit = (uint16_t)sensorBit(sensor);
  q.pending = (uint16_t)(q.pending | bit);
  q.on = on ? (uint16_t)(q.on | bit) : (uint16_t)(q.on & ~bit);
}

// Takes every pending switch: `on`, `off` the sensors to switch on and off.
inline void streamCtrlTake(StreamCtrlQueue& q, uint16_t* on, uint16_t* off) {
  *on = (uint16_t)(q.pending & q.on);
  *off = (uint16_t)(q.pending & ~q.on);
  q.pending = 0;
  q.on = 0;
}

constexpr const char* kFingerprintBadReply =
    "Error: a fingerprint is exactly 64 hexadecimal characters (the key 'espnowidentity' shows on that device), "
    "or 'clear' to name none";

// What "clear" did. `backup`: the backup master's fingerprint was cleared;
// `otherNamed`: the other one is still set (well formed). The effects are
// those of the rules this device follows: mesh time only from a named master
// (System_ESPNow_TimeCore.h), readings switched on by hand only to the named
// primary (readingTarget above), battery state and events to the named
// primary and backup (System_ESPNow_PowerEventCore.h), and sensor requests
// only from a named master (a lease already running runs out on its own).
inline const char* fingerprintClearedReply(bool backup, bool otherNamed) {
  if (!backup && !otherNamed) {
    return "OK: no master is named on this device now: it takes no mesh time, sends its readings, battery state "
           "and events to no device, and refuses sensor requests (a lease already running runs out).";
  }
  if (!backup) {
    return "OK: no master is named on this device now. Readings switched on by hand go to no device. The backup "
           "master named here (espnowbackupfingerprint) still gets its battery state and events, may ask it for "
           "readings, and gives it mesh time while it acts as master.";
  }
  if (!otherNamed) {
    return "OK: no backup master is named on this device now, and no master is either: it takes no mesh time, "
           "sends its readings, battery state and events to no device, and refuses sensor requests (a lease "
           "already running runs out).";
  }
  return "OK: no backup master is named on this device now: battery state and events go to the master named here "
         "(espnowmasterfingerprint) alone, and only that master gives it mesh time and may ask it for readings.";
}

}  // namespace espnowsensor
