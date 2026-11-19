// System_ESPNow_PowerEventCore.h - rules of the battery and event frames over ESP-NOW
//
// A device that is not the mesh master tells the masters named on it how its
// battery stands (POWER_STATUS, opcode 152) and which system events happened
// on it (EVENT_PUSH, opcode 132). This header has the two frames byte by
// byte, when a battery frame is due, which events pass the sender's floor,
// the rate limit on both ends, how a receiver copies text it cannot trust,
// and which entry a new event replaces in a full list. Pure logic on
// caller-owned state: no heap, no OS, no logging, so the rules run on a host
// as they run on the device (test/host/test_espnow_power_event_core.cpp).
// The owner (System_ESPNow_PowerEvents.cpp) holds the state and the locks.
//
// Both frames are one ordinary session frame, also through a relay (24 and
// 128 bytes of the 146 a routed frame carries). Each carries the sender's
// boot tag and a counter of its own, judged by the receiver with the counter
// rule of the readings (System_ESPNow_SensorCore.h): the same value goes to
// the primary and the backup master, so what two masters kept can be merged
// by (sender, boot tag, counter).
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "System_ESPNow_SensorCore.h"  // CounterTrack, kFrameCapRouted, the role rules

namespace espnowsensor {

// ---------------------------------------------------------------------------
// A token bucket: `cap` at once, then one every `refillMs`. Used by the event
// sender (8, one every 2 s) and by the receiver per sender (16, one a second).
// ---------------------------------------------------------------------------
struct TokenBucket {
  bool     primed;
  uint16_t tokens;
  uint32_t lastRefillMs;
};

inline void bucketRefill(TokenBucket& b, uint16_t cap, uint32_t refillMs, uint32_t nowMs) {
  if (!b.primed) {
    b.primed = true;
    b.tokens = cap;
    b.lastRefillMs = nowMs;
    return;
  }
  if (b.tokens > cap) b.tokens = cap;
  const uint32_t add = refillMs ? (uint32_t)(nowMs - b.lastRefillMs) / refillMs : 0;
  if (add == 0) return;
  if (add >= (uint32_t)(cap - b.tokens)) {
    // Full: the time it stays full earns nothing.
    b.tokens = cap;
    b.lastRefillMs = nowMs;
  } else {
    b.tokens = (uint16_t)(b.tokens + add);
    b.lastRefillMs += add * refillMs;
  }
}

inline bool bucketHas(TokenBucket& b, uint16_t cap, uint32_t refillMs, uint32_t nowMs) {
  bucketRefill(b, cap, refillMs, nowMs);
  return b.tokens > 0;
}

inline bool bucketTake(TokenBucket& b, uint16_t cap, uint32_t refillMs, uint32_t nowMs) {
  bucketRefill(b, cap, refillMs, nowMs);
  if (b.tokens == 0) return false;
  b.tokens--;
  return true;
}

// ---------------------------------------------------------------------------
// POWER_STATUS (24 bytes, little endian), as V4PayloadPowerStatus in
// System_ESPNow_Wire.h lays it out:
//
//   0  counter      16 bit: counts this sender's battery frames (1 first, 0
//                   after 65535); the same value to every target
//   2  bootTag      16 bit: as in the reading header
//   4  status       BatteryStatus (System_BatteryPolicy.h; its ordinals are
//                   stable). A value the receiver does not know is shown as
//                   "unknown"
//   5  backend      0 none, 1 ADC, 2 fuel gauge
//   6  known        16 bit: what the sender knows (kPower* below); a value
//                   whose bit is clear is 0 and means nothing
//   8  voltageMv    millivolts
//  10  percentX10   0 to 1000
//  12  rateX10      signed: percent per hour x 10, clamped to the field
//  14  sampleAgeMs  how old the battery sample was when sent; 65535 = that
//                   or more, or no sample yet
//  16  fwdLink      0, or the bond master's link number: the frame is the
//                   sender's bond partner's (System_ESPNow_ForwardCore.h)
//  17  reserved     7 bytes, 0
// ---------------------------------------------------------------------------
constexpr size_t kPowerFrameLen = 24;

constexpr uint16_t kPowerVoltageValid      = 1u << 0;
constexpr uint16_t kPowerPercentValid      = 1u << 1;
constexpr uint16_t kPowerPercentEstimated  = 1u << 2;
constexpr uint16_t kPowerPresenceKnown     = 1u << 3;
constexpr uint16_t kPowerPresent           = 1u << 4;
constexpr uint16_t kPowerChargingKnown     = 1u << 5;
constexpr uint16_t kPowerCharging          = 1u << 6;
constexpr uint16_t kPowerUsbKnown          = 1u << 7;
constexpr uint16_t kPowerUsbPresent        = 1u << 8;
constexpr uint16_t kPowerRateValid         = 1u << 9;
constexpr uint16_t kPowerStatusEstimated   = 1u << 10;
constexpr uint16_t kPowerStale             = 1u << 11;
constexpr uint16_t kPowerChargingEstimated = 1u << 12;
constexpr uint16_t kPowerKnownAll          = 0x1FFF;

constexpr uint8_t kPowerBackendNone      = 0;
constexpr uint8_t kPowerBackendAdc       = 1;
constexpr uint8_t kPowerBackendFuelGauge = 2;

// The BatteryStatus ordinals (System_BatteryPolicy.h asserts nothing about
// them, so the owner of the frame does: System_ESPNow_PowerEvents.cpp).
constexpr uint8_t kPowerStatusCount = 11;

struct PowerFrame {
  uint16_t counter;
  uint16_t bootTag;
  uint8_t  status;
  uint8_t  backend;
  uint16_t known;
  uint16_t voltageMv;
  uint16_t percentX10;
  int16_t  rateX10;
  uint16_t sampleAgeMs;
  uint8_t  fwdLink;  // byte 16: 0, or the link number of a forwarded frame
};

inline void packPowerFrame(uint8_t out[kPowerFrameLen], const PowerFrame& f) {
  auto put16 = [&](size_t at, uint16_t v) {
    out[at] = (uint8_t)(v & 0xFF);
    out[at + 1] = (uint8_t)(v >> 8);
  };
  put16(0, f.counter);
  put16(2, f.bootTag);
  out[4] = f.status;
  out[5] = f.backend;
  put16(6, (uint16_t)(f.known & kPowerKnownAll));
  put16(8, f.voltageMv);
  put16(10, f.percentX10);
  put16(12, (uint16_t)f.rateX10);
  put16(14, f.sampleAgeMs);
  out[16] = f.fwdLink;
  memset(out + 17, 0, 7);
}

// False when the payload is shorter than the frame. A longer one is read up
// to the frame's length (a later firmware may add fields behind it).
inline bool parsePowerFrame(const uint8_t* in, size_t len, PowerFrame& f) {
  if (!in || len < kPowerFrameLen) return false;
  auto get16 = [&](size_t at) { return (uint16_t)(in[at] | ((uint16_t)in[at + 1] << 8)); };
  f.counter = get16(0);
  f.bootTag = get16(2);
  f.status = in[4];
  f.backend = in[5];
  f.known = get16(6);
  f.voltageMv = get16(8);
  f.percentX10 = get16(10);
  f.rateX10 = (int16_t)get16(12);
  f.sampleAgeMs = get16(14);
  f.fwdLink = in[16];
  return true;
}

// What the sender knows of its battery (the fields of BatteryState that the
// frame carries; the owner copies them in).
struct PowerInputs {
  uint8_t  status;
  uint8_t  backend;
  bool     hasSample;
  uint32_t sampleAgeMs;
  bool     voltageValid;
  float    voltage;            // volts
  bool     percentageValid;
  bool     percentageEstimated;
  float    percentage;         // 0 to 100
  bool     presenceKnown;
  bool     present;
  bool     chargingKnown;
  bool     charging;
  bool     chargingEstimated;
  bool     usbKnown;
  bool     usbPresent;
  bool     rateValid;
  float    ratePctPerHr;
  bool     statusEstimated;
  bool     stale;
};

// Fills everything but counter and bootTag. A value that is not valid, not a
// number or outside its field is sent as 0 with its bit clear; the rate is
// clamped to what 16 bits hold.
inline void powerFrameFill(PowerFrame& f, const PowerInputs& in) {
  f.status = in.status;
  f.backend = in.backend;
  f.known = 0;
  f.voltageMv = 0;
  f.percentX10 = 0;
  f.rateX10 = 0;
  if (in.voltageValid && isfinite(in.voltage) && in.voltage >= 0.0f && in.voltage <= 65.0f) {
    f.voltageMv = (uint16_t)(in.voltage * 1000.0f + 0.5f);
    f.known |= kPowerVoltageValid;
  }
  if (in.percentageValid && isfinite(in.percentage)) {
    const float p = in.percentage < 0.0f ? 0.0f : (in.percentage > 100.0f ? 100.0f : in.percentage);
    f.percentX10 = (uint16_t)(p * 10.0f + 0.5f);
    f.known |= kPowerPercentValid;
    if (in.percentageEstimated) f.known |= kPowerPercentEstimated;
  }
  if (in.rateValid && isfinite(in.ratePctPerHr)) {
    float r = in.ratePctPerHr * 10.0f;
    if (r > 32767.0f) r = 32767.0f;
    if (r < -32768.0f) r = -32768.0f;
    f.rateX10 = (int16_t)(r < 0.0f ? r - 0.5f : r + 0.5f);
    f.known |= kPowerRateValid;
  }
  if (in.presenceKnown) f.known |= (uint16_t)(kPowerPresenceKnown | (in.present ? kPowerPresent : 0));
  if (in.chargingKnown) {
    f.known |= (uint16_t)(kPowerChargingKnown | (in.charging ? kPowerCharging : 0));
    if (in.chargingEstimated) f.known |= kPowerChargingEstimated;
  }
  if (in.usbKnown) f.known |= (uint16_t)(kPowerUsbKnown | (in.usbPresent ? kPowerUsbPresent : 0));
  if (in.statusEstimated) f.known |= kPowerStatusEstimated;
  if (in.stale) f.known |= kPowerStale;
  f.sampleAgeMs = (!in.hasSample || in.sampleAgeMs > 65535u) ? (uint16_t)65535 : (uint16_t)in.sampleAgeMs;
}

// The status and the backend in JSON. A number from the frame that this
// firmware does not know is "unknown".
inline const char* powerStatusKey(uint8_t status) {
  static const char* const kNames[kPowerStatusCount] = {
    "unknown", "charging", "full", "discharging", "low", "critical", "not_present", "high", "good", "medium", "empty",
  };
  return status < kPowerStatusCount ? kNames[status] : "unknown";
}

// The status in words, for a console line.
inline const char* powerStatusText(uint8_t status) {
  static const char* const kNames[kPowerStatusCount] = {
    "status unknown", "charging", "full", "discharging", "low", "critical", "no battery", "high", "good", "medium",
    "empty",
  };
  return status < kPowerStatusCount ? kNames[status] : "status unknown";
}

inline const char* powerBackendKey(uint8_t backend) {
  switch (backend) {
    case kPowerBackendNone:      return "none";
    case kPowerBackendAdc:       return "adc";
    case kPowerBackendFuelGauge: return "fuelgauge";
    default:                     return "unknown";
  }
}

// ---- When a battery frame is due --------------------------------------------
// Every `interval` (the setting powerStatusInterval in seconds: 0 = never,
// else 10 to 3600), and at once when the status, charging, USB or presence
// changed since the last frame, but never within 5 s of the last frame. The
// first frame goes out as soon as there is a receiver.
constexpr uint32_t kPowerMinGapMs      = 5000;
constexpr uint16_t kPowerIntervalMinS  = 10;
constexpr uint16_t kPowerIntervalMaxS  = 3600;
constexpr uint16_t kPowerIntervalDefaultS = 60;
constexpr uint16_t kPowerChangeMask = kPowerPresenceKnown | kPowerPresent | kPowerChargingKnown | kPowerCharging |
                                      kPowerUsbKnown | kPowerUsbPresent;

// True for a value the setting may hold.
constexpr bool powerIntervalValid(uint32_t seconds) {
  return seconds == 0 || (seconds >= kPowerIntervalMinS && seconds <= kPowerIntervalMaxS);
}

// The interval in ms; 0 = off. A stored value outside the range (a settings
// file written by hand) is pulled into it.
constexpr uint32_t powerIntervalMs(uint32_t seconds) {
  return seconds == 0 ? 0u
       : (seconds < kPowerIntervalMinS ? kPowerIntervalMinS * 1000u
       : (seconds > kPowerIntervalMaxS ? kPowerIntervalMaxS * 1000u : seconds * 1000u));
}

struct PowerSendState {
  bool     sentOnce;
  uint32_t lastSentMs;
  uint8_t  lastStatus;
  uint16_t lastKnown;
};

inline bool powerSendDue(const PowerSendState& s, const PowerFrame& now, uint32_t intervalSeconds, uint32_t nowMs) {
  const uint32_t interval = powerIntervalMs(intervalSeconds);
  if (interval == 0) return false;
  if (!s.sentOnce) return true;
  const uint32_t since = (uint32_t)(nowMs - s.lastSentMs);
  if (since < kPowerMinGapMs) return false;
  if (since >= interval) return true;
  return now.status != s.lastStatus || ((now.known ^ s.lastKnown) & kPowerChangeMask) != 0;
}

inline void powerSent(PowerSendState& s, const PowerFrame& f, uint32_t nowMs) {
  s.sentOnce = true;
  s.lastSentMs = nowMs;
  s.lastStatus = f.status;
  s.lastKnown = f.known;
}

// ---------------------------------------------------------------------------
// EVENT_PUSH (128 bytes, little endian), as V4PayloadEventPush in
// System_ESPNow_Wire.h lays it out:
//
//   0  counter    16 bit: counts this sender's event frames, as above
//   2  bootTag    16 bit
//   4  flags      bit 0 subject was cut, bit 1 detail was cut, bit 2 who was
//                 cut, bit 3 kind was cut; bit 4 forwarded (the event is the
//                 sender's bond partner's); other bits 0
//   5  tier       0 verbose, 1 standard, 2 alert: how the sender rates the kind
//   6  source     which interface caused the event (kEventSource* below)
//   7  fwdLink    the bond master's link number when bit 4 is set, else 0
//   8  skipped    16 bit: events the sender left out since the last one it
//                 sent (its rate limit, or overwritten in its ring before
//                 they could be sent); 65535 = that or more
//  10  reserved1  16 bit, 0
//  12  eventSeq   32 bit: the event's number on the sender, the #N of its
//                 events.log
//  16  ageMs      32 bit: how old the event was when sent
//  20  kind       32 bytes: the kind's name (the catalog forbids persisting
//                 its number), zero padded
//  52  subject    32 bytes, zero padded
//  84  detail     32 bytes, zero padded
// 116  who        12 bytes, zero padded
//
// A text field that is full has no terminator. Nothing in the frame is
// trusted to be text: the receiver copies each field with its length
// (wireTextCopy).
// ---------------------------------------------------------------------------
constexpr size_t kEventFrameLen   = 128;
constexpr size_t kEventKindLen    = 32;
constexpr size_t kEventSubjectLen = 32;
constexpr size_t kEventDetailLen  = 32;
constexpr size_t kEventWhoLen     = 12;
static_assert(20 + kEventKindLen + kEventSubjectLen + kEventDetailLen + kEventWhoLen == kEventFrameLen,
              "the event frame's fields fill it");
static_assert(kEventFrameLen <= kFrameCapRouted && kPowerFrameLen <= kFrameCapRouted,
              "both frames fit one frame through a relay");

constexpr uint8_t kEventCutSubject = 0x01;
constexpr uint8_t kEventCutDetail  = 0x02;
constexpr uint8_t kEventCutWho     = 0x04;
constexpr uint8_t kEventCutKind    = 0x08;
constexpr uint8_t kEventCutMask    = 0x0F;  // the cut bits a receiver stores
constexpr uint8_t kEventFlagForwarded = 0x10;
constexpr uint8_t kEventFlagsSent  = 0x1F;  // what a sender may set

constexpr uint8_t kEventTierVerbose  = 0;
constexpr uint8_t kEventTierStandard = 1;
constexpr uint8_t kEventTierAlert    = 2;

// The source numbers on the wire (NotificationSource of System_Events.h has
// the same values; System_ESPNow_PowerEvents.cpp asserts it).
constexpr uint8_t kEventSourceUnknown = 0;
constexpr uint8_t kEventSourceCli     = 1;
constexpr uint8_t kEventSourceOled    = 2;
constexpr uint8_t kEventSourceWeb     = 3;
constexpr uint8_t kEventSourceVoice   = 4;
constexpr uint8_t kEventSourceRemote  = 5;
constexpr uint8_t kEventSourceSystem  = 6;
constexpr uint8_t kEventSourceG2      = 7;
constexpr uint8_t kEventSourceCount   = 8;

struct EventFrame {
  uint16_t counter;
  uint16_t bootTag;
  uint8_t  flags;
  uint8_t  tier;
  uint8_t  source;
  uint16_t skipped;
  uint32_t eventSeq;
  uint32_t ageMs;
  uint8_t  fwdLink;                    // byte 7: 0, or the link number of a forwarded event
  char     kind[kEventKindLen];        // as on the wire: zero padded, not terminated when full
  char     subject[kEventSubjectLen];
  char     detail[kEventDetailLen];
  char     who[kEventWhoLen];
};

inline void packEventFrame(uint8_t out[kEventFrameLen], const EventFrame& f) {
  auto put16 = [&](size_t at, uint16_t v) {
    out[at] = (uint8_t)(v & 0xFF);
    out[at + 1] = (uint8_t)(v >> 8);
  };
  auto put32 = [&](size_t at, uint32_t v) {
    put16(at, (uint16_t)(v & 0xFFFF));
    put16(at + 2, (uint16_t)(v >> 16));
  };
  put16(0, f.counter);
  put16(2, f.bootTag);
  out[4] = (uint8_t)(f.flags & kEventFlagsSent);
  out[5] = f.tier;
  out[6] = f.source;
  out[7] = f.fwdLink;
  put16(8, f.skipped);
  put16(10, 0);
  put32(12, f.eventSeq);
  put32(16, f.ageMs);
  memcpy(out + 20, f.kind, kEventKindLen);
  memcpy(out + 52, f.subject, kEventSubjectLen);
  memcpy(out + 84, f.detail, kEventDetailLen);
  memcpy(out + 116, f.who, kEventWhoLen);
}

// False when the payload is shorter than the frame.
inline bool parseEventFrame(const uint8_t* in, size_t len, EventFrame& f) {
  if (!in || len < kEventFrameLen) return false;
  auto get16 = [&](size_t at) { return (uint16_t)(in[at] | ((uint16_t)in[at + 1] << 8)); };
  auto get32 = [&](size_t at) { return (uint32_t)get16(at) | ((uint32_t)get16(at + 2) << 16); };
  f.counter = get16(0);
  f.bootTag = get16(2);
  f.flags = in[4];
  f.tier = in[5];
  f.source = in[6];
  f.fwdLink = in[7];
  f.skipped = get16(8);
  f.eventSeq = get32(12);
  f.ageMs = get32(16);
  memcpy(f.kind, in + 20, kEventKindLen);
  memcpy(f.subject, in + 52, kEventSubjectLen);
  memcpy(f.detail, in + 84, kEventDetailLen);
  memcpy(f.who, in + 116, kEventWhoLen);
  return true;
}

// Sender: `text` (in a buffer of `textCap` bytes, read up to its terminator
// or that many bytes) into a zero-padded field of `fieldLen` bytes. True when
// the text did not fit and was cut; a cut never ends inside a UTF-8 character.
inline bool eventFieldPut(char* field, size_t fieldLen, const char* text, size_t textCap) {
  memset(field, 0, fieldLen);
  if (!text) return false;
  size_t len = 0;
  while (len < textCap && text[len] != '\0') len++;
  if (len <= fieldLen) {
    memcpy(field, text, len);
    return false;
  }
  size_t n = fieldLen;
  // Step back over the bytes of a character that would be cut in two.
  while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80) n--;
  memcpy(field, text, n);
  return true;
}

// The `skipped` field for a count.
constexpr uint16_t eventSkippedField(uint32_t count) { return count > 65535u ? (uint16_t)65535 : (uint16_t)count; }

inline const char* eventTierKey(uint8_t tier) {
  switch (tier) {
    case kEventTierVerbose:  return "verbose";
    case kEventTierStandard: return "standard";
    case kEventTierAlert:    return "alert";
    default:                 return "unknown";
  }
}

inline const char* eventSourceKey(uint8_t source) {
  static const char* const kNames[kEventSourceCount] = {
    "unknown", "cli", "oled", "web", "voice", "remote", "system", "g2",
  };
  return source < kEventSourceCount ? kNames[source] : "unknown";
}

// ---- Which events a device sends: the setting eventPush ----------------------
constexpr uint8_t kEventPushOff      = 0;
constexpr uint8_t kEventPushAlert    = 1;  // the default
constexpr uint8_t kEventPushStandard = 2;
constexpr uint8_t kEventPushVerbose  = 3;
constexpr uint8_t kEventPushCount    = 4;

inline const char* eventPushKey(uint8_t setting) {
  switch (setting) {
    case kEventPushAlert:    return "alert";
    case kEventPushStandard: return "standard";
    case kEventPushVerbose:  return "verbose";
    default:                 return "off";
  }
}

// False for a word that is not one of off, alert, standard, verbose.
inline bool eventPushFromName(const char* name, uint8_t& out) {
  if (!name) return false;
  for (uint8_t i = 0; i < kEventPushCount; ++i) {
    if (strcmp(name, eventPushKey(i)) == 0) { out = i; return true; }
  }
  return false;
}

// True when an event of `tier` is sent under `setting`. A stored value this
// firmware does not know sends nothing.
constexpr bool eventPassesFloor(uint8_t setting, uint8_t tier) {
  return setting == kEventPushVerbose ? true
       : setting == kEventPushStandard ? tier >= kEventTierStandard
       : setting == kEventPushAlert ? tier >= kEventTierAlert
       : false;
}

// The sender's and the receiver's rate limits.
constexpr uint16_t kEventTxBurst      = 8;
constexpr uint32_t kEventTxRefillMs   = 2000;
constexpr uint16_t kEventRxBurst      = 16;
constexpr uint32_t kEventRxRefillMs   = 1000;

// ---------------------------------------------------------------------------
// Whether this device sends its battery state, or its events, and why not.
// First match. Naming the master on a device is the consent to send to it;
// the receiver needs no bond and no login. A bond worker is the exception:
// it sends to its bond partner only (BOND_POWER_STATUS 180, BOND_EVENT_PUSH
// 182) and never to a mesh master, whatever is named on it; the bond master
// may forward them (System_ESPNow_ForwardCore.h).
// ---------------------------------------------------------------------------
enum class PushWhyNot : uint8_t {
  Sends = 0,
  NotInBuild,       // battery: this firmware has no battery monitor
  Off,              // the setting is off
  MeshOff,
  IsMaster,         // the acting master receives
  NoMaster,         // no master is named on this device
  MasterNotPaired,  // one is named, none of them is paired
  NoSession,        // none of them has a session yet (one is being started)
  BondOffline,      // a bond worker whose bond partner is not connected
  Count
};

struct PushInputs {
  bool    inBuild;
  bool    settingOn;
  bool    meshOn;
  uint8_t meshRole;
  bool    masterNamed;     // a master or backup master fingerprint is set
  uint8_t targets;         // named masters that are paired (0 to 2)
  uint8_t targetsReady;    // ... with a session a frame can use now
  bool    bondWorker;      // bond mode on and this device is the bond worker
  bool    bondOnline;      // ... and its bond partner is connected
};

inline PushWhyNot pushDecide(const PushInputs& in) {
  if (!in.inBuild) return PushWhyNot::NotInBuild;
  if (!in.settingOn) return PushWhyNot::Off;
  if (in.bondWorker) return in.bondOnline ? PushWhyNot::Sends : PushWhyNot::BondOffline;
  if (!in.meshOn) return PushWhyNot::MeshOff;
  if (!roleSends(in.meshRole)) return PushWhyNot::IsMaster;
  if (!in.masterNamed) return PushWhyNot::NoMaster;
  if (in.targets == 0) return PushWhyNot::MasterNotPaired;
  if (in.targetsReady == 0) return PushWhyNot::NoSession;
  return PushWhyNot::Sends;
}

// nullptr for Sends.
inline const char* pushWhyNotKey(PushWhyNot why) {
  switch (why) {
    case PushWhyNot::Sends:           return nullptr;
    case PushWhyNot::NotInBuild:      return "not_in_this_build";
    case PushWhyNot::Off:             return "off";
    case PushWhyNot::MeshOff:         return "mesh_off";
    case PushWhyNot::IsMaster:        return "is_master";
    case PushWhyNot::NoMaster:        return "no_master";
    case PushWhyNot::MasterNotPaired: return "master_not_paired";
    case PushWhyNot::NoSession:       return "no_session";
    case PushWhyNot::BondOffline:     return "bond_offline";
    case PushWhyNot::Count:           break;
  }
  return "?";
}

inline const char* pushWhyNotText(PushWhyNot why) {
  switch (why) {
    case PushWhyNot::Sends:           return "sent";
    case PushWhyNot::NotInBuild:      return "this firmware has no battery monitor";
    case PushWhyNot::Off:             return "switched off";
    case PushWhyNot::MeshOff:         return "mesh mode is off";
    case PushWhyNot::IsMaster:        return "this device is the mesh master, which receives";
    case PushWhyNot::NoMaster:        return "no master is named on this device (espnowmasterfingerprint)";
    case PushWhyNot::MasterNotPaired: return "the master named on this device is not paired with it";
    case PushWhyNot::NoSession:       return "the master has no session with this device yet";
    case PushWhyNot::BondOffline:     return "the bond partner is not connected";
    case PushWhyNot::Count:           break;
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Text from a frame. The receiver copies a field with its length, ends the
// copy at the first NUL and terminates it itself. A control byte, 0x7F and
// every byte that is not part of a well-formed UTF-8 character become '?',
// so the copy can be shown and put into JSON whatever the sender wrote.
// `out` holds at least fieldLen + 1 bytes. Returns the copy's length.
// ---------------------------------------------------------------------------
inline size_t wireTextCopy(const char* field, size_t fieldLen, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t pos = 0;
  if (!field) { out[0] = '\0'; return 0; }
  size_t len = 0;
  while (len < fieldLen && field[len] != '\0') len++;
  size_t i = 0;
  while (i < len && pos + 1 < cap) {
    const uint8_t ch = (uint8_t)field[i];
    if (ch < 0x80) {
      out[pos++] = (ch < 0x20 || ch == 0x7F) ? '?' : (char)ch;
      i++;
      continue;
    }
    // A UTF-8 character of 2 to 4 bytes: its lead byte, then continuation
    // bytes; overlong forms, surrogates and values over U+10FFFF are refused.
    size_t need = 0;
    uint32_t cp = 0;
    if (ch >= 0xC2 && ch <= 0xDF) { need = 1; cp = ch & 0x1Fu; }
    else if (ch >= 0xE0 && ch <= 0xEF) { need = 2; cp = ch & 0x0Fu; }
    else if (ch >= 0xF0 && ch <= 0xF4) { need = 3; cp = ch & 0x07u; }
    // All of it must be inside the field and fit the copy.
    bool good = need != 0 && i + need < len && pos + need + 1 < cap;
    for (size_t k = 1; good && k <= need; ++k) {
      const uint8_t next = (uint8_t)field[i + k];
      if ((next & 0xC0) != 0x80) good = false;
      else cp = (cp << 6) | (next & 0x3Fu);
    }
    if (good) {
      const uint32_t least = need == 1 ? 0x80u : (need == 2 ? 0x800u : 0x10000u);
      if (cp < least || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) good = false;
    }
    if (!good) {
      out[pos++] = '?';
      i++;
      continue;
    }
    for (size_t k = 0; k <= need; ++k) out[pos++] = field[i + k];
    i += need + 1;
  }
  out[pos] = '\0';
  return pos;
}

// `text` (already clean: wireTextCopy, or a name this device made) as the
// inside of a JSON string: quotes and backslashes are escaped, a control byte
// becomes '?'. The result is terminated and never ends inside an escape.
// Returns its length.
inline size_t jsonTextEscape(const char* text, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t pos = 0;
  for (const char* p = text ? text : ""; *p; ++p) {
    const uint8_t ch = (uint8_t)*p;
    if (ch == '"' || ch == '\\') {
      if (pos + 3 > cap) break;
      out[pos++] = '\\';
      out[pos++] = (char)ch;
    } else {
      if (pos + 2 > cap) break;
      out[pos++] = (ch < 0x20 || ch == 0x7F) ? '?' : (char)ch;
    }
  }
  out[pos] = '\0';
  return pos;
}

// ---------------------------------------------------------------------------
// The receiver's recent events: one list of kEventListLen entries per mesh.
// A new event takes a free entry; in a full list it replaces the oldest entry
// of the sender that holds the most (a sender is its origin and the device
// that forwarded it, if any) (of two that hold the same, the one whose
// oldest entry is older), so one busy sender cannot push out what the others
// sent. Every stored event has a receive number `rx` that counts up from 1
// since the receiver's boot, across all lists; 0 marks a free entry.
// ---------------------------------------------------------------------------
constexpr uint8_t kEventListLen = 24;
constexpr uint8_t kEventMeshes  = 4;

struct StoredEvent {
  uint32_t rx;               // 0: free
  uint8_t  senderMac[6];     // the event's origin
  uint8_t  via[6];           // all zero, or the bond master that forwarded it
  uint8_t  link;             // 0 mesh, 1 bond link, 2 forwarded (espnowfwd::kLink*)
  uint8_t  tier;
  uint8_t  source;
  uint8_t  flags;
  uint8_t  arrivalClock;     // the receiver's clock source at arrival
  uint8_t  mesh;             // the mesh slot of the sender (which list holds it)
  uint16_t counter;          // the sender's
  uint16_t bootTag;          // the sender's
  uint32_t eventSeq;         // the sender's
  uint32_t ageMs;            // how old the event was when the sender sent it
  uint32_t arrivalUptimeMs;  // the receiver's uptime at arrival
  int64_t  arrivalEpochMs;   // the receiver's UTC clock at arrival; 0: it was not set
  char     kind[kEventKindLen + 1];       // cleaned and terminated by the receiver
  char     subject[kEventSubjectLen + 1];
  char     detail[kEventDetailLen + 1];
  char     who[kEventWhoLen + 1];
};

// The index of the entry a new event takes.
inline uint8_t eventListSlot(const StoredEvent* list, uint8_t n) {
  for (uint8_t i = 0; i < n; ++i) {
    if (list[i].rx == 0) return i;
  }
  uint8_t best = 0;
  uint8_t bestCount = 0;
  for (uint8_t i = 0; i < n; ++i) {
    // The oldest entry of list[i]'s sender, and how many that sender holds.
    uint8_t count = 0;
    uint8_t oldest = i;
    for (uint8_t k = 0; k < n; ++k) {
      if (memcmp(list[k].senderMac, list[i].senderMac, 6) != 0 || memcmp(list[k].via, list[i].via, 6) != 0) continue;
      count++;
      if (list[k].rx < list[oldest].rx) oldest = k;
    }
    if (count > bestCount || (count == bestCount && list[oldest].rx < list[best].rx)) {
      bestCount = count;
      best = oldest;
    }
  }
  return best;
}

// The stored events after receive number `since`, oldest first: the entry
// with the smallest rx above `after` in the lists, or nullptr. A caller walks
// with after = since, then after = the entry's rx.
inline const StoredEvent* eventListNext(const StoredEvent* const* lists, uint8_t listCount, uint8_t n,
                                        uint32_t after) {
  const StoredEvent* next = nullptr;
  for (uint8_t m = 0; m < listCount; ++m) {
    if (!lists[m]) continue;
    for (uint8_t i = 0; i < n; ++i) {
      const StoredEvent& e = lists[m][i];
      if (e.rx == 0 || e.rx <= after) continue;
      if (!next || e.rx < next->rx) next = &e;
    }
  }
  return next;
}

// The number of the oldest event a ring of `ringSize` still holds when the
// newest is `latest` (the ring keeps the last ringSize posts; numbers start
// at 1). latest + 1 would be "none": a ring that holds none has latest 0.
constexpr uint32_t eventOldestHeld(uint32_t latest, uint32_t ringSize) {
  return latest >= ringSize ? latest - ringSize + 1 : 1u;
}

// A `since` above the newest receive number means the reader's number is
// from before this receiver restarted: it is read as 0 (everything held).
constexpr uint32_t eventSinceFor(uint32_t since, uint32_t latest) { return since > latest ? 0u : since; }

}  // namespace espnowsensor
