#ifndef G2_QUICKLIST_CORE_H
#define G2_QUICKLIST_CORE_H

// Control rows in the G2 glasses' own QuickList (sid 0x0C): "Wear
// Detection: On/Off" (index 0), "Silent Mode: On/Off" (index 1), "Auto
// Brightness: On/Off" (index 2), "Head-Up Display: On/Off" (index 3) and
// "Brightness: Auto|25%|50%|75%|100%|<level>%" (index 4), which HardwareOne
// adds and which switch (or, for Brightness, step) that setting when the
// wearer ticks them.
// Codec and policy only. Pure, host-testable, no Arduino/IDF, no allocation.
// The adapter is G2_QuickList.cpp.
//
// Wire (Even app 2.3.x <-> G2 firmware 2.3.2.14 captures, right temple):
//   Phone list update, flag 0x20, cmd 2 MULT_ITEMS FULL_UPDATE (the app's own
//   add/delete/complete message). It replaces the whole list on the glasses:
//     08 02  10 <magic>  22 <len> {
//       08 01 (data_type FULL_UPDATE)  10 <total_count>
//       1a <len> { item } ... }      one item per row, index 0..n-1
//   Each item writes every field, even when 0, in this order:
//       08 <uid>  10 <index>  18 00 (is_completed)  20 00 (timestamp)
//       2a <len> <title> 00      38 02 (ts_type)
//     The title's length counts one trailing NUL. Captured app item (uid
//     766822758, index 0, "line a"):
//       08 e6 92 d3 ed 02 10 00 18 00 20 00 2a 07 6c 69 6e 65 20 61 00 38 02
//   Lengths of 128 and more are two-byte varints: HardwareOne's five-row
//   update is about 190 bytes, so its f4 length always is.
//   Glasses ACK, flag 0x00: 08 02  10 <magic>  22 04 { 08 01  10 <total_count> }
//     e.g. 08 02 10 0c 22 04 08 01 10 04 (app, 4 items)
//   Glasses tick report, flag 0x01, cmd 2 MULT_ITEMS:
//     08 02  10 <glasses counter>  22 <len> {
//       08 <data_type 3>  10 <total_count>
//       1a <len> { 08 <uid>  18 01 (is_completed)  20 <timestamp> } ... }
//     captured: 08 02 10 04 22 14 08 03 10 01 1a 0e 08 81 80 c4 c1 04 18 01
//               20 a9 b0 84 d8 06
//   No phone reply to a tick report was captured.
//   The app renames one item with cmd 1 ITEM (08 01 10 <magic> 1a <len>
//   { item }), which HardwareOne no longer sends: on the 2.3.2.14 lens an
//   ITEM whose title changed moves that row to the top (device test
//   2026-11-21).
//
// encodeRows() builds the only message this feature sends: all five rows,
// index 0..4, in the fixed order Wear Detection, Silent Mode, Auto
// Brightness, Head-Up Display, Brightness. validateFullUpdatePb() re-parses a
// payload and refuses anything else before the radio.
//
// Row order on the lens: a FULL_UPDATE shows its items in index order (device
// test 2026-11-21: the five rows at index 0..4 showed exactly that order, and
// ticks on them still reported). Every push is one FULL_UPDATE of all five
// rows; items added in the Even app are removed from the lens by it.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace hw1_g2_quicklist {

static constexpr uint8_t  kSid            = 0x0C;
static constexpr uint8_t  kCmdItem        = 1;   // the app's one-item rename (a protocol record; not sent)
static constexpr uint8_t  kCmdMultItems   = 2;   // FULL_UPDATE, its ACK, and the glasses' tick report
static constexpr uint32_t kDataTypeFullUpdate = 1;
static constexpr uint32_t kDataTypeModify = 3;
static constexpr uint32_t kTsType         = 2;
static constexpr uint32_t kUidBase        = 0x48310000u;
static constexpr uint8_t  kWearControlId  = 1;
static constexpr uint32_t kWearUid        = kUidBase | kWearControlId;   // 0x48310001
static constexpr uint8_t  kSilentControlId = 2;
static constexpr uint32_t kSilentUid      = kUidBase | kSilentControlId; // 0x48310002
static constexpr uint8_t  kAutoBrightControlId = 3;
static constexpr uint32_t kAutoBrightUid  = kUidBase | kAutoBrightControlId; // 0x48310003
static constexpr uint8_t  kHeadUpControlId = 4;
static constexpr uint32_t kHeadUpUid      = kUidBase | kHeadUpControlId;     // 0x48310004
static constexpr uint8_t  kBrightnessControlId = 5;
static constexpr uint32_t kBrightnessUid  = kUidBase | kBrightnessControlId; // 0x48310005
static constexpr uint8_t  kRowCount       = 5;
static constexpr char     kFirmware[]     = "2.3.2.14";
static constexpr size_t   kTitleMax       = 32;   // bytes incl. the one NUL
// The longest item body: 08 + uid (5) + 10 + index (1) + 18 00 + 20 00 +
// 2a + len (1) + title with NUL (kTitleMax) + 38 02.
static constexpr size_t   kItemBodyMax    = 6 + 2 + 2 + 2 + 2 + kTitleMax + 2;
// f4 body: data_type, total_count, then per row 1a + len (1) + item.
static constexpr size_t   kListBodyMax    = 2 + 2 + kRowCount * (2 + kItemBodyMax);
// 08 02, 10 + magic (up to 2), 22 + len (2), body.
static constexpr size_t   kPbMax          = 2 + 3 + 3 + kListBodyMax;
static_assert(kItemBodyMax == 48 && kListBodyMax == 254 && kPbMax == 262, "the longest FULL_UPDATE");
static constexpr size_t   kReportItemsMax = 10;
static constexpr size_t   kDedupSlots     = 16;
static constexpr uint32_t kSettleMs           = 3000;
static constexpr uint32_t kJobGapMs           = 5000;  // one tick-triggered job per 5 s
static constexpr uint32_t kJobStartDeadlineMs = 3000;  // a write job that starts later than this does nothing
                                                       // (an old tick is never applied); a read still runs
static constexpr uint32_t kJobTimeoutMs       = 6000;  // owner gives up on a job that never started
// A switch write reads the switch back after a pause, not at once, and never
// polls (owner decision 2026-11-21). Device test 2026-11-21: a silent ON write
// was ACKed, but the read-back answered 139 ms after the write still said off;
// the glasses' own change push then said on. Phone capture: the Even app's
// silent ON write at 5.6 s, the glasses' change push at 5.9 s (0.3 s); silent
// OFF write at 23.1 s, push at 26.6 s (3.5 s). Wear and auto brightness matched
// at once in every device test, but no change push would fix a stale read-back
// of theirs. So: after the ACK wait, kReadbackDelayMs, read back once; only if
// that does not show the written state (or goes unanswered), wait
// kReadbackRecheckMs and read back once more. At most two read-backs per write
// (each one may still resend an unanswered BASIC read, as every settings read
// does). The waits run in kReadbackSliceMs slices that check the right temple.
static constexpr uint32_t kReadbackDelayMs    = 500;   // ACK wait -> first read-back (the push came 0.3 s after a write)
static constexpr uint32_t kReadbackRecheckMs  = 1500;  // first read-back -> the one recheck
static constexpr uint32_t kReadbackSliceMs    = 50;    // readiness check period while waiting
// The recheck starts only while the write has run at most this long (pre-read
// to the end of the first read-back: about 1-1.5 s normally, about 4.5 s with
// an unanswered first read-back), so a slow write still ends inside
// kJobBackstopMs: start 3 s + 9 s + recheck wait 1.5 s + read 6.75 s = 20.25 s.
static constexpr uint32_t kReadbackRecheckBudgetMs = 9000;
// Worst-case pieces of one job (G2_Glasses.cpp, the QuickList boundary). One
// settings read: up to 3 attempts of one BASIC read with up to 1 s wait each,
// no new attempt later than 2.5 s into the read, each send up to 1.5 s central
// TX gate + 1.5 s write mutex + 0.25 s write/cool-down: 2.5 + 3.25 + 1.0 =
// 6.75 s. One settings write: the same 3.25 s send, then up to 0.5 s ACK wait.
static constexpr uint32_t kReadWorstMs        = 6750;
static constexpr uint32_t kWriteSendWorstMs   = 3250;
static constexpr uint32_t kAckWaitMs          = 500;   // 50 x 10 ms, as cmd_g2glasses
static constexpr uint32_t kVersionWaitMs      = 50;    // the cold-link version-string wait after the pre-read
// A switch write without the recheck: start 3 s + pre-read 6.75 s + version
// wait 0.05 s + write send 3.25 s + ACK wait 0.5 s + read-back delay 0.5 s +
// read-back 6.75 s = 20.8 s.
static constexpr uint32_t kSwitchWriteWorstMs = kJobStartDeadlineMs + kReadWorstMs + kVersionWaitMs +
                                                kWriteSendWorstMs + kAckWaitMs + kReadbackDelayMs + kReadWorstMs;
// A Brightness tick with auto brightness on writes twice in the same job
// (auto OFF, its ACK, then the level): + 3.25 s + 0.5 s = 24.55 s.
static constexpr uint32_t kBrightnessWriteWorstMs = kSwitchWriteWorstMs + kWriteSendWorstMs + kAckWaitMs;
// Any write with the recheck (kReadbackRecheckBudgetMs above): 3 + 9 + 1.5 +
// 6.75 = 20.25 s. The budget is measured from the job's own start, so it
// covers the two-write Brightness job unchanged (its recheck is skipped once
// the job ran 9 s).
static constexpr uint32_t kRecheckWriteWorstMs = kJobStartDeadlineMs + kReadbackRecheckBudgetMs +
                                                 kReadbackRecheckMs + kReadWorstMs;
// Without TX gate contention a switch write whose pre-read is answered only at
// its third attempt and whose two read-backs go unanswered ends after about
// 3 + 3 + 0.05 + 0.5 + 0.5 + 3 + 1.5 + 3 = 14.6 s (Brightness, two writes:
// + 0.5 s ACK wait = 15.1 s). A read job may start late (up to kJobTimeoutMs,
// 6 s) and still runs: 6 + 6.75 = about 12.8 s.
// A started job is waited for. The backstop was 25 s while the worst job was a
// one-write switch (20.8 s); the two-write Brightness job (24.55 s) left only
// 0.45 s, so it is 30 s now: at least 3 s (5.45 s) above every worst case, so
// no row push goes out while a write still runs. Cost: a job that never reports
// holds the rows 5 s longer before the owner gives up on it.
static constexpr uint32_t kJobBackstopMs      = 30000; // owner gives up on a started job that never reports
static_assert(kSwitchWriteWorstMs == 20800 && kBrightnessWriteWorstMs == 24550 && kRecheckWriteWorstMs == 20250,
              "the worst cases the comments above add up");
static_assert(kRecheckWriteWorstMs + 3000 <= kJobBackstopMs,
              "a write with the read-back recheck ends at least 3 s inside the job backstop");
static_assert(kBrightnessWriteWorstMs + 3000 <= kJobBackstopMs,
              "the two-write Brightness job ends at least 3 s inside the job backstop");
static_assert(kJobTimeoutMs + kReadWorstMs + 3000 <= kJobBackstopMs, "a late read job ends inside the backstop");

// ---- the settings writes the rows send (sid 0x09, cmd 1, flag 0x20) ---------
// Field numbers of G2_Protocol.h (G2_SETW_*; G2_Glasses.cpp static_asserts
// that they match), so this header stands alone. Captured on 2.3.2.14:
//   wear       08 01 10 mm 1a 04 2a 02 08 0v      f3{f5{f1: 0|1}}
//   silent     08 01 10 mm 1a 04 32 02 08 0v      f3{f6{f1: 0|1}}
//   auto       08 01 10 mm 1a 04 0a 02 08 0v      f3{f1{f1: 0|1}}
//   head-up    08 01 10 mm 1a 04 22 02 08 0v      f3{f4{f1: 0|1}}
//   level      08 01 10 mm 1a 04 0a 02 10 <lvl>   f3{f1{f2: 0..100}} (app: 2..100)
static constexpr uint8_t kSetwBrightness = 1, kSetwHeadUp = 4, kSetwWear = 5, kSetwSilent = 6;
static constexpr uint8_t kSetwLeafSwitch = 1;   // auto / head-up / wear / silent switch
static constexpr uint8_t kSetwLeafLevel  = 2;   // brightness level
// The Brightness row's steps. HardwareOne writes only these levels.
static constexpr uint8_t kBrightnessSteps[] = {25, 50, 75, 100};
static constexpr uint8_t kBrightnessStepCount = sizeof kBrightnessSteps;
// The settings writes a QuickList tick may send: one of the four switches
// (0 or 1), or the brightness level at one of kBrightnessSteps. Re-checked in
// G2_Glasses.cpp before each write.
inline bool rowWriteAllowed(uint8_t inner, uint8_t leaf, uint32_t value) {
  if (leaf == kSetwLeafSwitch &&
      (inner == kSetwWear || inner == kSetwSilent || inner == kSetwBrightness || inner == kSetwHeadUp))
    return value <= 1;
  if (inner == kSetwBrightness && leaf == kSetwLeafLevel) {
    for (uint8_t s : kBrightnessSteps)
      if (value == s) return true;
  }
  return false;
}
// Whether a read-back brightness level `read` matches the level `written` on
// the 2.3.2.14 glasses: equal, or `written` is odd and `read` is one less.
// The glasses keep only even levels. Device test 2026-11-21 (A7:73, glasses
// 2.3.2.14), written -> read back, every write ACKed with the written value:
// 25 -> 24, 75 -> 74, 50 -> 50, 100 -> 100. The phone app's captured levels
// were all even (2, 20, 60, 80, 100) and read back exactly. Nothing wider:
// 24 written never matches 25 read, and 50 written never matches 49 read.
inline bool brightnessLevelMatches(uint8_t written, uint8_t read) {
  return read == written || ((written & 1u) != 0 && read == (uint8_t)(written - 1));
}
// The index in kBrightnessSteps of the step `level` matches
// (brightnessLevelMatches: 24 -> 25's, 74 -> 75's), or -1 for none.
inline int brightnessStepIndex(uint8_t level) {
  for (uint8_t i = 0; i < kBrightnessStepCount; ++i)
    if (brightnessLevelMatches(kBrightnessSteps[i], level)) return i;
  return -1;
}
// The level the Brightness row shows for `level`: the step it matches (24 ->
// 25, 74 -> 75), otherwise the level as read (e.g. 60).
inline uint8_t brightnessShownLevel(uint8_t level) {
  const int i = brightnessStepIndex(level);
  return i < 0 ? level : kBrightnessSteps[i];
}
// The level after `level` (auto brightness off): when `level` matches a step,
// the following step (24 -> 50, 74 -> 100, 100 -> 25); otherwise the smallest
// step strictly above it, and at 100 or above, back to 25.
inline uint8_t brightnessStep(uint8_t level) {
  const int i = brightnessStepIndex(level);
  if (i >= 0) return kBrightnessSteps[(i + 1) % kBrightnessStepCount];
  for (uint8_t s : kBrightnessSteps)
    if (s > level) return s;
  return kBrightnessSteps[0];
}
// What a Brightness tick writes: with auto brightness on, the first step
// (after turning auto off); otherwise the step after the current level.
inline uint8_t brightnessTarget(bool autoOn, uint8_t level) {
  return autoOn ? kBrightnessSteps[0] : brightnessStep(level);
}

// ---- protobuf helpers (local) ----------------------------------------------

namespace detail {

// Writes into p[0..cap), or, with cmp set, compares against cmp[0..cap)
// instead (the validator re-encodes over the received bytes; no scratch).
struct Writer {
  uint8_t* p;
  size_t cap, n;
  bool ok;
  const uint8_t* cmp;
  void byte(uint8_t b) {
    if (!ok || n >= cap) { ok = false; return; }
    if (cmp) { if (cmp[n] != b) { ok = false; return; } ++n; return; }
    p[n++] = b;
  }
  void varint(uint64_t v) {
    do {
      uint8_t b = (uint8_t)(v & 0x7F);
      v >>= 7;
      if (v) b |= 0x80;
      byte(b);
    } while (v && ok);
  }
  void tag(uint32_t field, uint8_t wt) { varint(((uint64_t)field << 3) | wt); }
  void bytes(const void* src, size_t len) {
    if (!ok || len > cap - n) { ok = false; return; }
    if (cmp) { if (len && memcmp(cmp + n, src, len) != 0) { ok = false; return; } }
    else if (len) memcpy(p + n, src, len);
    n += len;
  }
};
inline size_t varintLen(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) { v >>= 7; ++n; }
  return n;
}

struct Reader {
  const uint8_t* p;
  size_t len, pos;
  bool atEnd() const { return pos >= len; }
  // Any varint up to 10 bytes (uint64). False on truncation or overlong.
  bool varint64(uint64_t* out) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 10; ++i) {
      if (pos >= len) return false;
      const uint8_t b = p[pos++];
      if (i == 9 && (b & 0xFE)) return false;
      v |= (uint64_t)(b & 0x7F) << (7 * i);
      if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;
  }
  // A uint32 varint: at most 5 bytes, anything that overflows is rejected.
  bool varint32(uint32_t* out) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 5; ++i) {
      if (pos >= len) return false;
      const uint8_t b = p[pos++];
      if (i == 4 && (b & 0xF0)) return false;
      v |= (uint32_t)(b & 0x7F) << (7 * i);
      if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;
  }
  bool tag(uint32_t* field, uint8_t* wt) {
    uint32_t t = 0;
    if (!varint32(&t)) return false;
    *field = t >> 3;
    *wt = (uint8_t)(t & 7);
    return *field != 0;
  }
  // Length-delimited body: [*start, *start + *n).
  bool lenDelim(const uint8_t** start, size_t* n) {
    uint32_t l = 0;
    if (!varint32(&l)) return false;
    if (l > len - pos) return false;
    *start = p + pos;
    *n = l;
    pos += l;
    return true;
  }
  // Skips wire types 0/1/2/5; anything else (groups, 6, 7) is rejected.
  bool skip(uint8_t wt) {
    switch (wt) {
      case 0: { uint64_t v; return varint64(&v); }
      case 1: if (len - pos < 8) return false; pos += 8; return true;
      case 2: { const uint8_t* s; size_t n; return lenDelim(&s, &n); }
      case 5: if (len - pos < 4) return false; pos += 4; return true;
      default: return false;
    }
  }
};

inline bool printableTitle(const char* title, size_t* outLen) {
  if (!title || !title[0]) return false;
  size_t n = 0;
  for (; title[n]; ++n) {
    const unsigned char c = (unsigned char)title[n];
    if (c < 0x20 || c > 0x7E) return false;
    if (n + 2 > kTitleMax) return false;  // title + NUL must fit kTitleMax
  }
  *outLen = n;
  return true;
}

}  // namespace detail

// ---- codec ----------------------------------------------------------------

enum class Shown : uint8_t { Unknown = 0, Off = 1, On = 2 };      // all-zero = Unknown

inline const char* wearTitle(Shown s) {
  switch (s) {
    case Shown::On:  return "Wear Detection: On";
    case Shown::Off: return "Wear Detection: Off";
    default:         return "Wear Detection: ?";
  }
}
inline const char* silentTitle(Shown s) {
  switch (s) {
    case Shown::On:  return "Silent Mode: On";
    case Shown::Off: return "Silent Mode: Off";
    default:         return "Silent Mode: ?";
  }
}
inline const char* autoBrightTitle(Shown s) {
  switch (s) {
    case Shown::On:  return "Auto Brightness: On";
    case Shown::Off: return "Auto Brightness: Off";
    default:         return "Auto Brightness: ?";
  }
}
inline const char* headUpTitle(Shown s) {
  switch (s) {
    case Shown::On:  return "Head-Up Display: On";
    case Shown::Off: return "Head-Up Display: Off";
    default:         return "Head-Up Display: ?";
  }
}
// A row title by value (the Brightness row's carries a number).
struct Title { char text[kTitleMax]; };
// The Brightness row: Shown::On = auto brightness on ("Auto"), Shown::Off =
// auto brightness off at `level` ("25%" ... "100%", the step the level
// matches, so 24 shows "25%"; or whatever level the glasses hold, e.g.
// "60%"), Unknown = "?".
inline Title brightnessTitle(Shown s, uint8_t level) {
  Title t{};
  if (s == Shown::On) snprintf(t.text, sizeof t.text, "Brightness: Auto");
  else if (s == Shown::Off)
    snprintf(t.text, sizeof t.text, "Brightness: %u%%", (unsigned)brightnessShownLevel(level));
  else snprintf(t.text, sizeof t.text, "Brightness: ?");
  return t;
}
inline Shown shownFrom(bool known, bool on) { return !known ? Shown::Unknown : on ? Shown::On : Shown::Off; }
// Copy of hw1_g2_feed::nextMagic (G2_FeedCore.h), so this header stands alone.
inline uint8_t nextMagic(uint8_t prev) { return (uint8_t)(prev == 0xFF ? 1 : prev + 1); }  // 1..255, never 0

// The five control rows. The enum value is the row's slot and its list index.
enum class Control : uint8_t { Wear = 0, Silent = 1, AutoBright = 2, HeadUp = 3, Brightness = 4 };
inline uint8_t rowSlot(Control c) { return (uint8_t)c < kRowCount ? (uint8_t)c : 0; }
inline uint32_t rowUid(Control c) {
  switch (c) {
    case Control::Silent:     return kSilentUid;
    case Control::AutoBright: return kAutoBrightUid;
    case Control::HeadUp:     return kHeadUpUid;
    case Control::Brightness: return kBrightnessUid;
    default:                  return kWearUid;
  }
}
// Wear 0, silent 1, auto brightness 2, head-up 3, brightness 4 (the owner's
// order): the item index in the FULL_UPDATE, which the lens shows top to
// bottom.
inline uint32_t rowIndex(Control c) { return rowSlot(c); }
// `level`: the Brightness row's level (Shown::Off); ignored by the others.
inline Title rowTitle(Control c, Shown s, uint8_t level = 0) {
  if (c == Control::Brightness) return brightnessTitle(s, level);
  const char* text = wearTitle(s);
  switch (c) {
    case Control::Silent:     text = silentTitle(s); break;
    case Control::AutoBright: text = autoBrightTitle(s); break;
    case Control::HeadUp:     text = headUpTitle(s); break;
    default:                  break;
  }
  Title t{};
  snprintf(t.text, sizeof t.text, "%s", text);
  return t;
}
inline const char* controlName(Control c) {
  switch (c) {
    case Control::Silent:     return "silent mode";
    case Control::AutoBright: return "auto brightness";
    case Control::HeadUp:     return "head-up display";
    case Control::Brightness: return "brightness";
    default:                  return "wear detection";
  }
}
inline bool controlForUid(uint32_t uid, Control* out) {
  if (uid == kWearUid)       { if (out) *out = Control::Wear;       return true; }
  if (uid == kSilentUid)     { if (out) *out = Control::Silent;     return true; }
  if (uid == kAutoBrightUid) { if (out) *out = Control::AutoBright; return true; }
  if (uid == kHeadUpUid)     { if (out) *out = Control::HeadUp;     return true; }
  if (uid == kBrightnessUid) { if (out) *out = Control::Brightness; return true; }
  return false;
}

// One list item, as the app writes it (every field, even when 0):
// 08 <uid> 10 <index> 18 00 20 00 2a <len> <title> 00 38 02
struct ListItem { uint32_t uid; uint32_t index; const char* title; };

namespace detail {
inline size_t itemBodyLen(const ListItem& it, size_t titleLen) {
  return 1 + varintLen(it.uid) + 1 + varintLen(it.index) + 2 + 2 + 1 + varintLen(titleLen + 1) + titleLen + 1 +
         1 + varintLen(kTsType);
}
inline void writeItem(Writer& w, const ListItem& it, size_t titleLen) {
  w.tag(1, 0); w.varint(it.uid);
  w.tag(2, 0); w.varint(it.index);
  w.tag(3, 0); w.varint(0);          // is_completed
  w.tag(4, 0); w.varint(0);          // timestamp
  w.tag(5, 2); w.varint(titleLen + 1); w.bytes(it.title, titleLen); w.byte(0);
  w.tag(7, 0); w.varint(kTsType);
}
// cmd 2 FULL_UPDATE of `count` items (1..kRowCount). Lengths are computed
// first, so the bytes go straight into `w` (no scratch).
inline bool writeFullUpdate(Writer& w, uint8_t magic, const ListItem* items, size_t count) {
  if (!magic || !items || !count || count > kRowCount) return false;
  size_t titleLen[kRowCount], itemLen[kRowCount];
  size_t body = 1 + varintLen(kDataTypeFullUpdate) + 1 + varintLen(count);
  for (size_t i = 0; i < count; ++i) {
    if (!printableTitle(items[i].title, &titleLen[i])) return false;
    itemLen[i] = itemBodyLen(items[i], titleLen[i]);
    body += 1 + varintLen(itemLen[i]) + itemLen[i];
  }
  w.tag(1, 0); w.varint(kCmdMultItems);
  w.tag(2, 0); w.varint(magic);
  w.tag(4, 2); w.varint(body);
  w.tag(1, 0); w.varint(kDataTypeFullUpdate);
  w.tag(2, 0); w.varint(count);
  for (size_t i = 0; i < count; ++i) {
    w.tag(3, 2); w.varint(itemLen[i]);
    writeItem(w, items[i], titleLen[i]);
  }
  return w.ok;
}
}  // namespace detail

// One item body alone (the captured app item is the golden). Returns bytes
// written or 0 (null/empty title, a byte outside 0x20..0x7E, title+NUL >
// kTitleMax, cap too small).
inline size_t encodeListItem(const ListItem& it, uint8_t* out, size_t cap) {
  size_t titleLen = 0;
  if (!out || !detail::printableTitle(it.title, &titleLen)) return 0;
  detail::Writer w{out, cap, 0, true, nullptr};
  detail::writeItem(w, it, titleLen);
  return w.ok ? w.n : 0;
}
// cmd 2 FULL_UPDATE: 08 02 10 <magic> 22 <len> { 08 01 10 <count> (1a <len> { item })... }.
// Returns bytes written or 0 (magic 0, no items, more than kRowCount, a bad
// title, cap too small).
inline size_t encodeFullUpdate(uint8_t magic, const ListItem* items, size_t count, uint8_t* out, size_t cap) {
  if (!out) return 0;
  detail::Writer w{out, cap, 0, true, nullptr};
  return detail::writeFullUpdate(w, magic, items, count) ? w.n : 0;
}
// HardwareOne's update: the five rows, row i (Control i) at index i with its
// uid and titles[i].
inline size_t encodeRows(uint8_t magic, const char* const titles[kRowCount], uint8_t* out, size_t cap) {
  if (!titles) return 0;
  ListItem items[kRowCount];
  for (uint8_t i = 0; i < kRowCount; ++i) items[i] = ListItem{rowUid((Control)i), i, titles[i]};
  return encodeFullUpdate(magic, items, kRowCount, out, cap);
}

// Wire whitelist, re-checked in G2_Glasses.cpp before the radio. Accepts only
// encodeRows' exact bytes: cmd 2, magic 1..255, FULL_UPDATE, total_count 5,
// then the five rows in index order 0..4, each with its own uid (kWearUid ..
// kBrightnessUid), is_completed/timestamp == 0, ts_type == 2 and a title of
// printable ASCII with exactly one trailing NUL counted in its length;
// minimal varints, no extra field. The titles are located, then the message
// is re-encoded over the received bytes (compare mode, no scratch).
inline bool validateFullUpdatePb(const uint8_t* pb, size_t len, uint8_t* magic = nullptr) {
  if (!pb || !len || len > kPbMax) return false;
  detail::Reader r{pb, len, 0};
  uint32_t f = 0, v = 0;
  uint8_t wt = 0;
  if (!r.tag(&f, &wt) || f != 1 || wt != 0 || !r.varint32(&v) || v != kCmdMultItems) return false;
  uint32_t m = 0;
  if (!r.tag(&f, &wt) || f != 2 || wt != 0 || !r.varint32(&m) || m < 1 || m > 0xFF) return false;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0;
  if (!r.tag(&f, &wt) || f != 4 || wt != 2 || !r.lenDelim(&body, &bodyLen) || !r.atEnd()) return false;
  detail::Reader in{body, bodyLen, 0};
  if (!in.tag(&f, &wt) || f != 1 || wt != 0 || !in.varint32(&v) || v != kDataTypeFullUpdate) return false;
  if (!in.tag(&f, &wt) || f != 2 || wt != 0 || !in.varint32(&v) || v != kRowCount) return false;
  const char* titles[kRowCount];
  for (uint8_t i = 0; i < kRowCount; ++i) {
    const uint8_t* item = nullptr;
    size_t itemLen = 0;
    if (!in.tag(&f, &wt) || f != 3 || wt != 2 || !in.lenDelim(&item, &itemLen)) return false;
    detail::Reader it{item, itemLen, 0};
    // Fields 1..4 are varints; the uid, index and zeros are checked by the re-encode.
    for (uint32_t field = 1; field <= 4; ++field)
      if (!it.tag(&f, &wt) || f != field || wt != 0 || !it.varint32(&v)) return false;
    const uint8_t* title = nullptr;
    size_t titleLen = 0;
    if (!it.tag(&f, &wt) || f != 5 || wt != 2 || !it.lenDelim(&title, &titleLen)) return false;
    if (titleLen < 2 || titleLen > kTitleMax || title[titleLen - 1] != 0) return false;
    titles[i] = (const char*)title;   // NUL-terminated in place; printable checked by the re-encode
  }
  if (!in.atEnd()) return false;
  ListItem items[kRowCount];
  for (uint8_t i = 0; i < kRowCount; ++i) items[i] = ListItem{rowUid((Control)i), i, titles[i]};
  detail::Writer w{nullptr, len, 0, true, pb};
  if (!detail::writeFullUpdate(w, (uint8_t)m, items, kRowCount) || w.n != len) return false;
  if (magic) *magic = (uint8_t)m;
  return true;
}

// The glasses' ACK of a FULL_UPDATE (flag 0x00): f1 == 2, f2 magic 1..255,
// optional f4 {f1 data_type, f2 total_count}. A data_type other than
// FULL_UPDATE is refused; total_count is returned when present (noteAck wants
// kRowCount). Tolerates unknown fields. Captured: 08 02 10 0c 22 04 08 01 10 04.
struct Ack { uint8_t magic; bool haveTotal; uint32_t totalCount; };
inline bool parseAck(const uint8_t* pb, size_t len, Ack& out) {
  out = Ack{};
  if (!pb || !len) return false;
  detail::Reader r{pb, len, 0};
  bool haveCmd = false, haveMagic = false;
  uint32_t cmd = 0, magic = 0;
  while (!r.atEnd()) {
    uint32_t f = 0;
    uint8_t wt = 0;
    if (!r.tag(&f, &wt)) return false;
    if (f == 1 && wt == 0) { if (!r.varint32(&cmd)) return false; haveCmd = true; }
    else if (f == 2 && wt == 0) { if (!r.varint32(&magic)) return false; haveMagic = true; }
    else if (f == 4 && wt == 2) {
      const uint8_t* s = nullptr;
      size_t n = 0;
      if (!r.lenDelim(&s, &n)) return false;
      detail::Reader in{s, n, 0};
      while (!in.atEnd()) {
        uint32_t g = 0, gv = 0;
        uint8_t gw = 0;
        if (!in.tag(&g, &gw)) return false;
        if (g == 1 && gw == 0) {
          if (!in.varint32(&gv) || gv != kDataTypeFullUpdate) return false;
        } else if (g == 2 && gw == 0) {
          if (!in.varint32(&out.totalCount)) return false;
          out.haveTotal = true;
        } else if (!in.skip(gw)) return false;
      }
    } else if (!r.skip(wt)) return false;
  }
  if (!haveCmd || cmd != kCmdMultItems || !haveMagic || magic < 1 || magic > 0xFF) { out = Ack{}; return false; }
  out.magic = (uint8_t)magic;
  return true;
}

struct ReportItem { uint32_t uid; bool completed; uint32_t timestamp; };
struct TickReport {
  uint32_t glassesMagic;     // f2; 0 when omitted
  uint32_t dataType, totalCount;
  uint8_t  count; bool truncated;
  ReportItem items[kReportItemsMax];
};

namespace detail {
inline bool parseReportItem(const uint8_t* pb, size_t len, ReportItem& it) {
  it = ReportItem{};
  Reader r{pb, len, 0};
  while (!r.atEnd()) {
    uint32_t f = 0, v = 0;
    uint8_t wt = 0;
    if (!r.tag(&f, &wt)) return false;
    if (wt == 0 && (f == 1 || f == 3 || f == 4)) {
      if (!r.varint32(&v)) return false;
      if (f == 1) it.uid = v;
      else if (f == 3) it.completed = v != 0;
      else it.timestamp = v;
    } else if (!r.skip(wt)) return false;
  }
  return true;
}
}  // namespace detail

// Flag-0x01 report: f1 == 2, optional f2, f4 submessage {f1 data_type, f2 total_count,
// repeated f3 item {f1 uid, f3 is_completed, f4 timestamp; others skipped}}.
// True only for data_type 3 with >= 1 item; > 10 items -> first 10, truncated = true.
inline bool parseTickReport(const uint8_t* pb, size_t len, TickReport& out) {
  memset(&out, 0, sizeof out);
  if (!pb || !len) return false;
  detail::Reader r{pb, len, 0};
  bool haveCmd = false, haveData = false;
  uint32_t cmd = 0;
  while (!r.atEnd()) {
    uint32_t f = 0;
    uint8_t wt = 0;
    if (!r.tag(&f, &wt)) return false;
    if (f == 1 && wt == 0) { if (!r.varint32(&cmd)) return false; haveCmd = true; }
    else if (f == 2 && wt == 0) { if (!r.varint32(&out.glassesMagic)) return false; }
    else if (f == 4 && wt == 2) {
      const uint8_t* s = nullptr;
      size_t n = 0;
      if (!r.lenDelim(&s, &n)) return false;
      haveData = true;
      detail::Reader in{s, n, 0};
      while (!in.atEnd()) {
        uint32_t g = 0;
        uint8_t gw = 0;
        if (!in.tag(&g, &gw)) return false;
        if (g == 1 && gw == 0) { if (!in.varint32(&out.dataType)) return false; }
        else if (g == 2 && gw == 0) { if (!in.varint32(&out.totalCount)) return false; }
        else if (g == 3 && gw == 2) {
          const uint8_t* is = nullptr;
          size_t in_n = 0;
          if (!in.lenDelim(&is, &in_n)) return false;
          ReportItem it;
          if (!detail::parseReportItem(is, in_n, it)) return false;
          if (out.count < kReportItemsMax) out.items[out.count++] = it;
          else out.truncated = true;
        } else if (!in.skip(gw)) return false;
      }
    } else if (!r.skip(wt)) return false;
  }
  return haveCmd && cmd == kCmdMultItems && haveData &&
         out.dataType == kDataTypeModify && out.count >= 1;
}

// Upper-case hex, NUL-terminated; stops early if `out` is too small. Returns
// the characters written (without the NUL).
inline size_t formatHex(const uint8_t* pb, size_t len, char* out, size_t cap) {
  if (!out || !cap) return 0;
  static const char kDigits[] = "0123456789ABCDEF";
  size_t n = 0;
  for (size_t i = 0; pb && i < len && n + 2 < cap; ++i) {
    out[n++] = kDigits[pb[i] >> 4];
    out[n++] = kDigits[pb[i] & 0x0F];
  }
  out[n] = '\0';
  return n;
}

// ---- policy ---------------------------------------------------------------
// Every struct below is valid when all its bytes are zero, so it can live in
// EXT_RAM_BSS_ATTR.

struct Dedup { uint32_t magic[kDedupSlots]; uint32_t uid[kDedupSlots]; uint8_t next, used; };
inline void dedupClear(Dedup& d) { d = Dedup{}; }
// true = already seen; otherwise inserts (oldest evicted) and returns false.
inline bool dedupSeen(Dedup& d, uint32_t glassesMagic, uint32_t uid) {
  for (uint8_t i = 0; i < d.used && i < kDedupSlots; ++i)
    if (d.magic[i] == glassesMagic && d.uid[i] == uid) return true;
  const uint8_t slot = (uint8_t)(d.next % kDedupSlots);
  d.magic[slot] = glassesMagic;
  d.uid[slot] = uid;
  d.next = (uint8_t)((slot + 1) % kDedupSlots);
  if (d.used < kDedupSlots) d.used++;
  return false;
}

enum class TickAction : uint8_t { Foreign, NotCompleted, Disabled, Duplicate, Busy, RateLimited, Refresh, Apply };
struct Decision { TickAction action; bool targetOn; Control control; };

inline const char* tickActionName(TickAction a) {
  switch (a) {
    case TickAction::Foreign:      return "foreign";
    case TickAction::NotCompleted: return "not completed";
    case TickAction::Disabled:     return "ignored (feed off)";
    case TickAction::Duplicate:    return "duplicate";
    case TickAction::Busy:         return "while busy";
    case TickAction::RateLimited:  return "rate-limited";
    case TickAction::Refresh:      return "refresh";
    case TickAction::Apply:        return "apply";
  }
  return "?";
}

// One row's state on this link.
struct Row {
  Shown shown;      // title the glasses show now (set on a successful push only)
  Shown next;       // title the next push will carry (set from reads, applies, change pushes)
  uint8_t magic;    // magic of the last FULL_UPDATE sent on this link (0 = none; the same in every row)
  bool acked;       // that FULL_UPDATE was ACKed (one ACK covers all five rows)
  bool pending;     // this row's title wants a push (any pending row sends all five)
  // The Brightness row's level while its state is Off (auto brightness off);
  // 0 in every other row and state, so a title compares as (state, shown
  // level): 24 and 25 both show "25%" (brightnessShownLevel).
  uint8_t shownLevel, nextLevel;
};
// The title of `next` differs from the one the glasses show.
inline bool rowChanged(const Row& r) {
  return r.next != r.shown || brightnessShownLevel(r.nextLevel) != brightnessShownLevel(r.shownLevel);
}
// Re-read after "?": when a read fails and rows show (or stay) unknown, the
// owner reads again after 2 s, 5 s, 15 s, 30 s, then every 60 s, until a read
// gives every row a state. Reset on success and on link reset (linkReset).
static constexpr uint32_t kRecoveryDelaysMs[] = {2000, 5000, 15000, 30000, 60000};
static constexpr uint8_t kRecoveryDelayCount = sizeof kRecoveryDelaysMs / sizeof kRecoveryDelaysMs[0];
struct Recovery {
  bool armed;          // a re-read is scheduled
  uint8_t step;        // re-reads scheduled since the last reset (picks the delay)
  uint32_t fromMs, delayMs;
};

// ---- the push: one FULL_UPDATE of all five rows -------------------------------
// Device test 2026-11-21 (owner's glasses 2.3.2.14): a cmd 1 ITEM whose title
// changed moves that row to the top of the lens, so per-row updates cannot keep
// the order. A cmd 2 FULL_UPDATE with the five rows at index 0..4 was ACKed and
// the lens showed exactly that order; it replaces the whole list (items added
// in the Even app go). So whenever a row wants a push, the owner sends ONE
// FULL_UPDATE carrying all five rows with their current titles ("?"
// included), and one ACK covers them all:
//  - a row that changes while the ACK is awaited goes out in the next
//    FULL_UPDATE, after that ACK (or its timeout);
//  - a send that fails, or an ACK that does not come within kPushAckWaitMs
//    (counted from when the send returned), sends the list again after the
//    owner's 1 s retry gate (resendDue); an ACK of that list that still comes
//    before the resend counts (the lens shows it) and cancels the resend, so a
//    late ACK never starts another round; kPushTriesMax such failures in a
//    row and the owner gives up for now: the lens may show an
//    older list (staleLens), so the re-read schedule (2/5/15/30/60 s, same
//    gates) reads the settings and, when the read answers, sends the list
//    again. Any later ACKed FULL_UPDATE restores everything and clears it.
// Phone capture 2026-11-20: the glasses ACKed the app's FULL_UPDATE at once.
static constexpr uint32_t kPushAckWaitMs = 1000;
static constexpr uint8_t kPushTriesMax = 3;   // failed sends or missing ACKs in a row before the owner gives up

// What asked for the push (`g2 list status`).
enum class PushReason : uint8_t { None, Connect, Manual, Tick, Change, Recovery, Repush, Timeout, Resend };
inline const char* pushReasonName(PushReason r) {
  switch (r) {
    case PushReason::None:     return "none";
    case PushReason::Connect:  return "connect read";
    case PushReason::Manual:   return "g2 list push";
    case PushReason::Tick:     return "tick";
    case PushReason::Change:   return "silent change push";
    case PushReason::Recovery: return "re-read after \"?\"";
    case PushReason::Repush:   return "tick re-push";
    case PushReason::Timeout:  return "job timeout";
    case PushReason::Resend:   return "re-read after a give-up";
  }
  return "?";
}

// The list push on this link (all-zero = none).
struct ListPush {
  bool awaitingAck;     // a FULL_UPDATE was sent with `magic` at sentMs
  uint8_t magic;
  uint32_t sentMs;
  uint8_t failRun;      // failed sends / missing ACKs in a row (reset by an ACK)
  bool resendDue;       // the last send failed or its ACK timed out: send the list again
  PushReason reason;    // what the last FULL_UPDATE was sent for
  PushReason want;      // the latest request, taken by the next FULL_UPDATE
};

// One job slot, one rate limit and one dedup for all rows.
struct Policy {
  Row row[kRowCount];   // [rowSlot(Control)]
  bool jobInFlight, everJob;
  uint32_t lastJobMs;
  Dedup dedup;
  Recovery recovery;
  ListPush push;
  // A push was given up (kPushTriesMax failures): the lens may show an older
  // list. The re-read schedule sends the list again (see kPushAckWaitMs).
  bool staleLens;
};
inline void linkReset(Policy& p) { p = Policy{}; }   // every row Unknown, nothing pending, dedup and recovery cleared
inline Row& rowOf(Policy& p, Control c) { return p.row[rowSlot(c)]; }
inline const Row& rowOf(const Policy& p, Control c) { return p.row[rowSlot(c)]; }

// Re-push a row with the title it shows now: clears a tick that switched
// nothing. A newer state already waiting for a push is kept.
inline void noteRepush(Policy& p, Control c) {
  Row& r = rowOf(p, c);
  if (!r.pending) { r.next = r.shown; r.nextLevel = r.shownLevel; r.pending = true; }
}
// A row whose state could not be read: "?" on the next push.
inline void markUnknown(Policy& p, Control c) {
  Row& r = rowOf(p, c);
  r.next = Shown::Unknown;
  r.nextLevel = 0;
  r.pending = true;
}
inline bool anyPending(const Policy& p) {
  for (uint8_t i = 0; i < kRowCount; ++i)
    if (p.row[i].pending) return true;
  return false;
}
// The list wants a FULL_UPDATE: a row is pending, or a failed one is resent.
inline bool pushWanted(const Policy& p) { return anyPending(p) || p.push.resendDue; }
inline void clearPending(Policy& p) {
  for (uint8_t i = 0; i < kRowCount; ++i) p.row[i].pending = false;
}
// A row shows, or will show with its next push, "?".
inline bool anyUnknown(const Policy& p) {
  for (uint8_t i = 0; i < kRowCount; ++i)
    if (p.row[i].next == Shown::Unknown) return true;
  return false;
}

// ---- re-read after "?" (pure; the owner calls these) ----
inline uint32_t recoveryDelayMs(uint8_t step) {
  return kRecoveryDelaysMs[step < kRecoveryDelayCount ? step : kRecoveryDelayCount - 1];
}
inline void recoveryReset(Recovery& r) { r = Recovery{}; }
// Schedules the next re-read. Already scheduled: unchanged (a failure while one
// waits neither moves it nor speeds the backoff up).
inline void recoveryArm(Recovery& r, uint32_t nowMs) {
  if (r.armed) return;
  r.armed = true;
  r.fromMs = nowMs;
  r.delayMs = recoveryDelayMs(r.step);
  if (r.step < 0xFF) r.step++;
}
inline bool recoveryDue(const Recovery& r, uint32_t nowMs) {
  return r.armed && (uint32_t)(nowMs - r.fromMs) >= r.delayMs;   // wrap-safe
}
// ms until the re-read is due: 0 = due, UINT32_MAX = none scheduled.
inline uint32_t recoveryWaitMs(const Recovery& r, uint32_t nowMs) {
  if (!r.armed) return UINT32_MAX;
  const uint32_t elapsed = nowMs - r.fromMs;
  return elapsed >= r.delayMs ? 0 : r.delayMs - elapsed;
}
// The re-read job was submitted; its result arms the next step or resets.
inline void recoveryTaken(Recovery& r) { r.armed = false; }
// After a read job's result was applied to the rows (afterRead): every row
// known = success, reset; any row "?" = schedule a re-read. A stale lens (a
// given-up push): the next step when the read pushes nothing (it did not
// answer), and never a reset, so the backoff keeps growing while pushes keep
// failing (an ACKed push resets it, noteAck).
inline void recoveryAfterRead(Policy& p, uint32_t nowMs) {
  if (anyUnknown(p) || (p.staleLens && !pushWanted(p))) recoveryArm(p.recovery, nowMs);
  else if (!p.staleLens) recoveryReset(p.recovery);
}
// After a job that left rows "?" without a read (a timeout, a write): schedule
// a re-read; never a reset.
inline void recoveryArmIfUnknown(Policy& p, uint32_t nowMs) {
  if (anyUnknown(p)) recoveryArm(p.recovery, nowMs);
}

// The owner's gate for a scheduled re-read.
struct RecoveryGate {
  bool phaseIdle;       // link settled, not Unsupported
  bool enabled;         // feed on, or a manual push on this link
  bool firmwareExact;   // the 2.3.2.14 pair
  bool busy;            // a voice or QuickNote session
  bool otherWork;       // an action, a stashed tick or a held change waits (it goes first)
};
enum class RecoveryAction : uint8_t { None, Cleared, Wait, Blocked, Read };
//  not scheduled                              -> None
//  no row "?" any more (a change push, a tick) and the lens not stale -> Cleared (reset)
//  job in flight, not due yet                 -> Wait
//  due, but not idle / not enabled / not the exact pair / busy -> Blocked
//  other work (it goes first)                 -> Wait
//  else                                       -> Read (submit a read job with
//                                                noteBackgroundJobStarted, then recoveryTaken)
// Blocked comes before other work: work that waits on the same gate (a push in
// a voice session, a row pending while Unsupported) must not make the owner
// lap fast for a re-read that cannot run.
inline RecoveryAction decideRecovery(Policy& p, const RecoveryGate& g, uint32_t nowMs) {
  Recovery& r = p.recovery;
  if (!r.armed) return RecoveryAction::None;
  if (!anyUnknown(p) && !p.staleLens) { recoveryReset(r); return RecoveryAction::Cleared; }
  if (p.jobInFlight || !recoveryDue(r, nowMs)) return RecoveryAction::Wait;
  if (!g.phaseIdle || !g.enabled || !g.firmwareExact || g.busy) return RecoveryAction::Blocked;
  if (g.otherWork) return RecoveryAction::Wait;
  return RecoveryAction::Read;
}

// Order matters (host-tested). Callers pass `enabled` = feedOn || manual push this link.
//  uid not one of our rows                 -> Foreign (not inserted into dedup)
//  !it.completed                           -> NotCompleted
//  !enabled                                -> Disabled
//  dedupSeen(glassesMagic, uid)            -> Duplicate
//  p.jobInFlight                           -> Busy        (row re-pushed when the job ends)
//  p.everJob && now - lastJobMs < kJobGapMs (uint32 wrap-safe) -> RateLimited (row re-pushed unchanged)
//  that row's shown == Unknown             -> Refresh
//  else                                    -> Apply, targetOn = (that row's shown == Shown::Off)
//                                             (Brightness: targetOn false and unused; the job
//                                             picks the step from its own pre-read)
inline Decision decideItem(Policy& p, bool enabled, uint32_t glassesMagic, const ReportItem& it, uint32_t nowMs) {
  Control c = Control::Wear;
  if (!controlForUid(it.uid, &c)) return {TickAction::Foreign, false, Control::Wear};
  if (!it.completed) return {TickAction::NotCompleted, false, c};
  if (!enabled) return {TickAction::Disabled, false, c};
  if (dedupSeen(p.dedup, glassesMagic, it.uid)) return {TickAction::Duplicate, false, c};
  if (p.jobInFlight) { noteRepush(p, c); return {TickAction::Busy, false, c}; }
  if (p.everJob && (uint32_t)(nowMs - p.lastJobMs) < kJobGapMs) {
    noteRepush(p, c);
    return {TickAction::RateLimited, false, c};
  }
  const Row& r = rowOf(p, c);
  if (r.shown == Shown::Unknown) return {TickAction::Refresh, false, c};
  return {TickAction::Apply, c != Control::Brightness && r.shown == Shown::Off, c};
}
inline void noteJobStarted(Policy& p, uint32_t nowMs) {
  p.jobInFlight = true;
  p.everJob = true;
  p.lastJobMs = nowMs;
}
// A job the wearer did not ask for (the re-read after "?"): one job at a time
// still holds, but it does not use up the 5 s tick budget.
inline void noteBackgroundJobStarted(Policy& p) { p.jobInFlight = true; }

// The typed result of one row write (wear, silent, auto brightness, head-up or
// brightness; the names predate the other rows). known/on describe the written
// row's read-back: for a switch, the switch; for Brightness, that row's state
// (on = auto brightness on, "Auto"; off = manual at `level`).
// AutoOffUnacked: Brightness only, auto brightness was on and its OFF write
// got no ACK, so the level was not written (the read-back still ran, with its
// one recheck when it still showed auto on).
enum class WearStage : uint8_t { NotReady, Stale, FwRejected, PreReadFailed, AlreadySet,
                                 BuilderRejected, SendFailed, LostAfterSend, LostDuringReadback, Done,
                                 AutoOffUnacked };

// One settings read's numbers (G2_Glasses.cpp fills them). attempts = BASIC
// reads sent (0 = no read was made); answeredAttempt = the 1-based attempt
// whose magic the correlated reply carried (0 = no reply; a late reply to an
// earlier attempt still counts as that attempt's); replyMs = from that
// attempt's send to the reply; totalMs = the whole read. otherReplies:
// settings replies during the wait that carried another magic; otherFrames:
// other right-temple settings frames during the wait (the glasses' own
// updates, without settingInfoType 1).
struct ReadDiag {
  uint8_t attempts, answeredAttempt;
  uint16_t otherReplies, otherFrames;
  uint32_t replyMs, totalMs;
};
inline bool readMade(const ReadDiag& d) { return d.attempts != 0; }
inline bool readAnswered(const ReadDiag& d) { return d.answeredAttempt != 0; }
// Answered, but only after a resend (the first attempt's wait ran out).
inline bool readNeededResend(const ReadDiag& d) { return d.answeredAttempt != 0 && d.attempts > 1; }

// readOk/known/on/fullProfile: the read-back the row goes by. readBack: the
// first read-back (kReadbackDelayMs after the ACK wait). rechecked: it did not
// show the written state, so a second one was made kReadbackRecheckMs later
// (recheck); an answered recheck replaces the first read-back's state, an
// unanswered one keeps it. recheckSkipped: a recheck was due but the write had
// run past kReadbackRecheckBudgetMs.
// levelKnown/level: the read-back's brightness level (BASIC f2, the level in
// effect; with auto brightness on, the one it chose), filled by every write.
// autoKnown/autoOn: the read-back's auto brightness (f18), every write.
// Brightness only: target = the step chosen from the pre-read (0 = none);
// autoWasOn = the pre-read showed auto brightness on; autoWritten/autoAcked =
// its OFF write was sent / ACKed; levelWritten = the level write was sent
// (acked = that write's ACK).
struct WearResult {
  WearStage stage; bool acked, readOk, known, on, fullProfile, rechecked, recheckSkipped;
  bool levelKnown, autoKnown, autoOn;
  uint8_t level, target;
  bool autoWasOn, autoWritten, autoAcked, levelWritten;
  ReadDiag preRead, readBack, recheck;
};
// The recheck's answer replaces the first read-back's (the read-back fields only).
inline void takeReadBack(WearResult& dst, const WearResult& src) {
  dst.readOk = src.readOk;
  dst.fullProfile = src.fullProfile;
  dst.known = src.known;
  dst.on = src.on;
  dst.levelKnown = src.levelKnown;
  dst.level = src.level;
  dst.autoKnown = src.autoKnown;
  dst.autoOn = src.autoOn;
}
// A Brightness write's read-back shows what it wrote: the level matches the
// target (brightnessLevelMatches: 25 read back as 24 is confirmed on the first
// read-back) and, when it turned auto brightness off, auto brightness is off.
inline bool brightnessConfirmed(const WearResult& r) {
  if (!r.readOk || !r.levelKnown || !brightnessLevelMatches(r.target, r.level)) return false;
  return !r.autoWritten || (r.autoKnown && !r.autoOn);
}
// How the read-back ended, for `g2 list status` and the log.
enum class Recheck : uint8_t { None, Answered, Unanswered, Skipped };
inline Recheck recheckOf(const WearResult& r) {
  if (r.recheckSkipped) return Recheck::Skipped;
  if (!r.rechecked) return Recheck::None;
  return readAnswered(r.recheck) ? Recheck::Answered : Recheck::Unanswered;
}
// Appended to "readback on|off|unknown".
inline const char* recheckSuffix(Recheck k) {
  switch (k) {
    case Recheck::None:       return "";
    case Recheck::Answered:   return " after recheck";
    case Recheck::Unanswered: return " (recheck unanswered)";
    case Recheck::Skipped:    return " (no time left to recheck)";
  }
  return "";
}
inline const char* stageName(WearStage s) {
  switch (s) {
    case WearStage::NotReady:           return "not ready";
    case WearStage::Stale:              return "stale (started too late)";
    case WearStage::FwRejected:         return "firmware rejected";
    case WearStage::PreReadFailed:      return "pre-read failed";
    case WearStage::AlreadySet:         return "already set";
    case WearStage::BuilderRejected:    return "builder rejected";
    case WearStage::SendFailed:         return "send failed";
    case WearStage::LostAfterSend:      return "link lost after send";
    case WearStage::LostDuringReadback: return "link lost during readback";
    case WearStage::Done:               return "done";
    case WearStage::AutoOffUnacked:     return "auto-brightness off not acknowledged, level not written";
  }
  return "?";
}

// One fresh correlated BASIC read gives every row (wear f10, silent f14, auto
// brightness f18, head-up f7; Brightness from f18 and the level f2). A switch
// is known when its field is present, or when it is absent from a full
// correlated 2.3.2.14 reply (the glasses leave out a zero; for f18 and f7 that
// is still to confirm on the device test). Absent anywhere else = unknown.
// The Brightness row's state: known/on as auto brightness, and level (f2) when
// auto brightness is off (brightnessRowState).
struct SwitchState { bool known, on; uint8_t level; };
struct ReadResult { bool readOk, fullProfile; SwitchState sw[kRowCount]; ReadDiag diag; };   // sw[rowSlot(Control)]

// The Brightness row from auto brightness (f18) and the level (f2): "Auto"
// when auto brightness is on (whatever level it chose), the level when it is
// off, "?" when auto brightness is unknown or it is off and the level unknown.
inline SwitchState brightnessRowState(SwitchState autoBright, bool levelKnown, uint8_t level) {
  if (!autoBright.known) return SwitchState{false, false, 0};
  if (autoBright.on) return SwitchState{true, true, 0};
  if (!levelKnown) return SwitchState{false, false, 0};
  return SwitchState{true, false, level};
}

// Sets a row's next title from a state (unknown -> "?"). The level is kept
// only for the Brightness row at Off.
inline void setNext(Row& row, Control c, SwitchState s) {
  row.next = shownFrom(s.known, s.on);
  row.nextLevel = (c == Control::Brightness && row.next == Shown::Off) ? s.level : 0;
}

enum class After : uint8_t { None, Push, ReadThenPush, Unsupported, Drop };
// jobInFlight = false. firmwareExact: stored pair exact OR the read's fullProfile.
//  !firmwareExact -> Unsupported; else every row: next = shownFrom(readOk && known, on)
//  (Brightness: and its level), pending -> Push
inline After afterRead(Policy& p, const ReadResult& r, bool firmwareExact) {
  p.jobInFlight = false;
  if (!firmwareExact) return After::Unsupported;
  for (uint8_t i = 0; i < kRowCount; ++i) {
    SwitchState s = r.sw[i];
    if (!r.readOk) s.known = false;
    setNext(p.row[i], (Control)i, s);
    p.row[i].pending = true;
  }
  return After::Push;
}
// jobInFlight = false. The re-read after "?" (nobody asked for it):
//  !firmwareExact -> Unsupported
//  !readOk (unanswered, or the job started too late) -> rows unchanged, nothing pushed
//  readOk: a row the read knows takes that state; a row it does not know keeps
//  the state it has (a re-read never turns a known row into "?"). A push
//  (always all five rows) goes out only when a title changes or the last
//  FULL_UPDATE was not ACKed; a re-read that changes nothing pushes nothing.
//  readOk and staleLens (a push was given up): the list is sent again; a read
//  that does not answer pushes nothing.
//  -> Push when a row wants a push, else None
inline After afterRecoveryRead(Policy& p, const ReadResult& r, bool firmwareExact) {
  p.jobInFlight = false;
  if (!firmwareExact) return After::Unsupported;
  if (r.readOk) {
    for (uint8_t i = 0; i < kRowCount; ++i) {
      Row& row = p.row[i];
      if (r.sw[i].known) setNext(row, (Control)i, r.sw[i]);
      if (p.staleLens || (r.sw[i].known && (rowChanged(row) || !row.acked))) row.pending = true;
    }
  }
  return anyPending(p) ? After::Push : After::None;
}
// The other brightness row after a write's answered read-back (Auto Brightness
// after a Brightness tick, Brightness after an Auto Brightness tick): it takes
// the read state ("?" when the read cannot tell) and is pushed only when its
// title changes or its last push was not ACKed (as after a re-read).
inline void noteLinkedRow(Policy& p, Control c, SwitchState s) {
  Row& row = rowOf(p, c);
  setNext(row, c, s);
  if (rowChanged(row) || !row.acked) row.pending = true;
}
// jobInFlight = false. For the written row `c`:
//  NotReady/LostAfterSend/LostDuringReadback -> Drop (the link is changing; linkReset follows)
//  FwRejected -> Unsupported
//  (Done|AlreadySet|AutoOffUnacked) && known -> next = shownFrom(true, on) (Brightness: and
//    its level), pending -> Push; the linked brightness row follows the same read-back
//    (noteLinkedRow): Auto Brightness after Brightness, Brightness after Auto Brightness
//  everything else (Stale, PreReadFailed, BuilderRejected, SendFailed, Done without known) -> ReadThenPush
inline After afterApply(Policy& p, Control c, const WearResult& r) {
  p.jobInFlight = false;
  switch (r.stage) {
    case WearStage::NotReady:
    case WearStage::LostAfterSend:
    case WearStage::LostDuringReadback:
      return After::Drop;
    case WearStage::FwRejected:
      return After::Unsupported;
    case WearStage::Done:
    case WearStage::AlreadySet:
    case WearStage::AutoOffUnacked:
      if (r.known) {
        Row& row = rowOf(p, c);
        setNext(row, c, SwitchState{true, r.on, r.level});
        row.pending = true;
        if (c == Control::Brightness) {
          noteLinkedRow(p, Control::AutoBright, SwitchState{r.autoKnown, r.autoOn, 0});
        } else if (c == Control::AutoBright) {
          noteLinkedRow(p, Control::Brightness,
                        brightnessRowState(SwitchState{true, r.on, 0}, r.levelKnown, r.level));
        }
        return After::Push;
      }
      return After::ReadThenPush;
    default:
      return After::ReadThenPush;
  }
}
// After a successful send of row `c` in a FULL_UPDATE: shown = next, magic,
// not acked, not pending.
inline void notePushed(Policy& p, Control c, uint8_t magic) {
  Row& r = rowOf(p, c);
  r.shown = r.next;
  r.shownLevel = r.nextLevel;
  r.magic = magic;
  r.acked = false;
  r.pending = false;
}
// An ACK of our last FULL_UPDATE (its magic; total_count kRowCount when
// present): every row is acked, the wait ends, the failure run and a stale
// lens clear, and the re-read schedule resets when no row is "?". An ACK that
// comes after its timeout but before the resend still counts: the lens shows
// that list, so the resend is cancelled (resendDue) and only rows that
// changed since (pending) go out. False for any other ACK (another magic, an
// older list, a second ACK of the same one).
inline bool noteAck(Policy& p, const Ack& a) {
  const uint8_t m = p.row[0].magic;
  if (!m || a.magic != m || p.row[0].acked) return false;
  if (a.haveTotal && a.totalCount != kRowCount) return false;
  for (uint8_t i = 0; i < kRowCount; ++i)
    if (p.row[i].magic == m) p.row[i].acked = true;
  if (p.push.awaitingAck && p.push.magic == m) p.push.awaitingAck = false;
  p.push.resendDue = false;
  p.push.failRun = 0;
  p.staleLens = false;
  if (!anyUnknown(p)) recoveryReset(p.recovery);
  return true;
}

// ---- the push (see kPushAckWaitMs; the owner calls these) ----
// Records what asks for the next push (the latest request wins).
inline void pushWant(Policy& p, PushReason r) { p.push.want = r; }
enum class PushStep : uint8_t {
  Idle,        // nothing awaited, no row pending
  Send,        // send one FULL_UPDATE of all five rows now, then pushSent or pushFailed
  WaitAck,     // the last FULL_UPDATE waits for its ACK
  AckTimeout,  // its ACK did not come in kPushAckWaitMs: the owner calls pushFailed
};
inline const char* pushStepName(PushStep s) {
  switch (s) {
    case PushStep::Idle:       return "idle";
    case PushStep::Send:       return "send";
    case PushStep::WaitAck:    return "wait for ACK";
    case PushStep::AckTimeout: return "ACK timeout";
  }
  return "?";
}
// One owner lap's push decision (the owner calls it only while no job runs,
// outside a voice/QuickNote session and with its retry gate open):
//  awaiting an ACK, within kPushAckWaitMs   -> WaitAck (noteAck ends the wait)
//  awaiting an ACK, past kPushAckWaitMs     -> AckTimeout (awaiting cleared)
//  a row pending or a resend due            -> Send
//  else                                     -> Idle
// sentMs is taken when the send returned, so a lap `now` read before it is
// earlier than sentMs: the signed difference keeps that a WaitAck (wrap-safe).
inline PushStep pushStep(Policy& p, uint32_t nowMs) {
  ListPush& s = p.push;
  if (s.awaitingAck) {
    if ((int32_t)(nowMs - s.sentMs) < (int32_t)kPushAckWaitMs) return PushStep::WaitAck;
    s.awaitingAck = false;
    return PushStep::AckTimeout;
  }
  return pushWanted(p) ? PushStep::Send : PushStep::Idle;
}
// The FULL_UPDATE was sent with `magic` (nowMs: when the send returned):
// every row shows its next title now (notePushed) and the push waits for the
// ACK. The pending request becomes its reason (a resend keeps the reason it
// had).
inline void pushSent(Policy& p, uint8_t magic, uint32_t nowMs) {
  for (uint8_t i = 0; i < kRowCount; ++i) notePushed(p, (Control)i, magic);
  ListPush& s = p.push;
  s.awaitingAck = true;
  s.resendDue = false;
  s.magic = magic;
  s.sentMs = nowMs;
  if (s.want != PushReason::None) { s.reason = s.want; s.want = PushReason::None; }
}
// A send failed or an ACK did not come: the list is resent (resendDue) after
// the owner's retry gate, unless the late ACK of that list comes first
// (noteAck cancels the resend; rows are not marked pending, so only rows that
// changed meanwhile still go out). Returns true when that was the
// kPushTriesMax-th failure in a row: given up, nothing stays pending or due,
// the lens is marked stale and the re-read schedule armed (its answered read
// sends the list again; so does the next change, tick, read or
// `g2 list push`).
inline bool pushFailed(Policy& p, uint32_t nowMs) {
  ListPush& s = p.push;
  s.awaitingAck = false;
  if (++s.failRun < kPushTriesMax) {
    s.resendDue = true;
    return false;
  }
  s.failRun = 0;
  s.resendDue = false;
  s.want = PushReason::None;
  clearPending(p);
  p.staleLens = true;
  recoveryArm(p.recovery, nowMs);
  return true;
}
// The owner stops pushing (not the exact pair, firmware rejected): nothing
// stays pending or awaited and no resend is scheduled.
inline void pushAbort(Policy& p) {
  p.push.awaitingAck = false;
  p.push.resendDue = false;
  p.push.failRun = 0;
  p.push.want = PushReason::None;
  p.staleLens = false;
  clearPending(p);
}
// ms until an awaited ACK times out: 0 = now, UINT32_MAX = no ACK awaited.
inline uint32_t pushAckWaitMs(const Policy& p, uint32_t nowMs) {
  const ListPush& s = p.push;
  if (!s.awaitingAck) return UINT32_MAX;
  const int32_t elapsed = (int32_t)(nowMs - s.sentMs);   // < 0: a `now` read before the send returned
  if (elapsed < 0) return kPushAckWaitMs;
  return (uint32_t)elapsed >= kPushAckWaitMs ? 0 : kPushAckWaitMs - (uint32_t)elapsed;
}

// Glasses change pushes (silent: sid 0x09 cmd 3 f5{f2}, sometimes 2-3 times
// per change). The newest one waits in HeldChange while a job runs.
enum class ChangeAction : uint8_t { Duplicate, Noted, Push };
inline const char* changeActionName(ChangeAction a) {
  switch (a) {
    case ChangeAction::Duplicate: return "duplicate";
    case ChangeAction::Noted:     return "noted";
    case ChangeAction::Push:      return "push";
  }
  return "?";
}
// `live`: the row was added on this link and the feed (or a manual push) is on.
//  same state as next, and (not live, or already pending, or already shown) -> Duplicate
//  !live -> next = state, Noted (no push: the row is not on the lens from this link)
//  else  -> next = state, pending -> Push
inline ChangeAction noteChangePush(Policy& p, Control c, bool on, bool live) {
  Row& r = rowOf(p, c);
  const Shown s = on ? Shown::On : Shown::Off;
  if (r.next == s && (!live || r.pending || r.shown == s)) return ChangeAction::Duplicate;
  r.next = s;
  if (!live) return ChangeAction::Noted;
  r.pending = true;
  return ChangeAction::Push;
}
struct HeldChange { bool pending, on; };
inline void holdChange(HeldChange& h, bool on) { h.pending = true; h.on = on; }   // the newest value wins
// Applies the held change unless a job runs (then it stays held). False = nothing applied.
inline bool takeChange(Policy& p, HeldChange& h, Control c, bool live, ChangeAction* out) {
  if (!h.pending || p.jobInFlight) return false;
  h.pending = false;
  *out = noteChangePush(p, c, h.on, live);
  return true;
}

}  // namespace hw1_g2_quicklist

#endif  // G2_QUICKLIST_CORE_H
