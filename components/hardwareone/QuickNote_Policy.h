// QuickNote_Policy.h — dependency-free QuickNote policy: the note lifecycle
// FSM, timing and size limits, the title and junk heuristics, the v1 file
// header, file names and the private notes path.
//
// Kept free of Arduino, ESP-IDF and FreeRTOS so the host tests
// (test/host/test_quicknote_policy.cpp) compile this exact header. The service
// owns the state and the clock; QuickNote_Store.cpp does the filesystem I/O.
// Nothing here allocates, blocks or logs, and no helper copies note text
// anywhere but the caller's own buffer.
//
// A note file is UTF-8 text, at most kHeaderMax + kMaxNoteBytes bytes:
//
//   HardwareOne note v1
//   Id: 6f1c9a2e00000007
//   Created: 20261004T153012Z          (UTC; "boot" while the clock is unsynced)
//   Boot: 76                           (HW1's boot counter when saved; 0 unknown)
//   Uptime-s: 1234                     (seconds since that boot when saved)
//   Trigger: hold                      (hold|lens|cli|web)
//   Mic: g2                            (g2|pdm)
//   Duration-ms: 8420
//   Words: 9
//   Title: Buy oat milk and call the dentist
//   <blank line>
//   Buy oat milk and call the dentist.
//
// It is named <Created>-<Id>.txt inside /<root>/u<accountId>/notes/, root
// /sd/stt or /stt. That folder is inside the private transcript tree, so
// Transcript_PathPolicy classifies it Owner(accountId) with no policy change.
// Boot and Uptime-s are optional (notes saved before they existed lack them):
// they let a list say how long ago a "boot" note was saved in this boot, and
// let the service give it its real time once the clock is set that boot.
#ifndef QUICKNOTE_POLICY_H
#define QUICKNOTE_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Transcript_PathPolicy.h"
#include "Transcript_TextPolicy.h"

namespace QuickNotePolicy {

// --- Lifecycle ----------------------------------------------------------------
// One note at a time: Idle → Starting → Recording → Finishing → Saving → Idle,
// leaving through Failed or Discarded, each cleared back to Idle once reported.
enum class Phase : uint8_t {
  Idle, Starting, Recording, Finishing, Saving, Failed, Discarded,
};
enum class Event : uint8_t {
  Start,          // a start request was admitted
  CaptureActive,  // the mic is delivering audio
  Stop,           // release / `note stop`: finish and keep what was said
  Finished,       // dictation delivered its final text (after Stop, or by
                  // itself: silence stop or the length limit)
  Saved,          // the store wrote the file
  Discard,        // cancel, too short, or junk: nothing is saved
  Fail,           // dictation, storage or session failure
  Clear,          // the outcome was reported
};

// The phase after `event`, or `phase` itself when the event does not apply
// there. Every applicable event changes the phase, so "unchanged" == "ignored".
inline Phase noteNext(Phase phase, Event event) {
  switch (phase) {
    case Phase::Idle:
      return event == Event::Start ? Phase::Starting : phase;
    case Phase::Starting:
      if (event == Event::CaptureActive) return Phase::Recording;
      // Stopped before any audio: a slip of the finger, not a note.
      if (event == Event::Stop || event == Event::Discard) return Phase::Discarded;
      return event == Event::Fail ? Phase::Failed : phase;
    case Phase::Recording:
      if (event == Event::Stop) return Phase::Finishing;
      if (event == Event::Finished) return Phase::Saving;
      if (event == Event::Discard) return Phase::Discarded;
      return event == Event::Fail ? Phase::Failed : phase;
    case Phase::Finishing:
      if (event == Event::Finished) return Phase::Saving;
      if (event == Event::Discard) return Phase::Discarded;
      return event == Event::Fail ? Phase::Failed : phase;
    case Phase::Saving:
      if (event == Event::Saved) return Phase::Idle;
      if (event == Event::Discard) return Phase::Discarded;  // junk
      return event == Event::Fail ? Phase::Failed : phase;
    case Phase::Failed:
    case Phase::Discarded:
      return event == Event::Clear ? Phase::Idle : phase;
  }
  return phase;
}
// Applies `event` to `phase`; false (phase untouched) when it does not apply.
inline bool noteTransition(Phase& phase, Event event) {
  const Phase next = noteNext(phase, event);
  if (next == phase) return false;
  phase = next;
  return true;
}
// A note holds the dictation lease (and the mic) in these phases.
constexpr bool noteActive(Phase phase) {
  return phase == Phase::Starting || phase == Phase::Recording ||
         phase == Phase::Finishing || phase == Phase::Saving;
}
inline const char* notePhaseName(Phase phase) {
  static const char* const kNames[] = {
    "idle", "starting", "recording", "finishing", "saving", "failed", "discarded",
  };
  const size_t i = static_cast<size_t>(phase);
  return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

// --- Limits -------------------------------------------------------------------
// A hold released before capture started, or with less audio than this, is
// discarded silently: no file and no failure.
constexpr uint32_t kMinHoldMs = 600;
constexpr bool noteHoldTooShort(bool captureStarted, uint32_t capturedMs) {
  return !captureStarted || capturedMs < kMinHoldMs;
}
// Stop → final text. Local STT finishes on the device; the CM5 path covers the
// framed upload plus kDictationHostResultTimeoutMs (90 s).
constexpr uint32_t kFinishTimeoutLocalMs = 30000;
constexpr uint32_t kFinishTimeoutCm5Ms = 95000;
constexpr uint32_t noteFinishTimeoutMs(bool localStt) {
  return localStt ? kFinishTimeoutLocalMs : kFinishTimeoutCm5Ms;
}
constexpr size_t kMaxNoteBytes = 4096;  // note text, header excluded
constexpr size_t kHeaderMax = 512;      // a whole header fits one list read
constexpr size_t kStampMax = 16;        // "yyyymmddThhmmssZ"
constexpr size_t kFileNameMax = kStampMax + 1 + 16 + 4;  // <stamp>-<id16>.txt
constexpr size_t kPathMax = 80;         // "/sd/stt/u4294967295/notes/" + name
constexpr uint32_t kJunkMaxMs = 2500;

// --- Trigger and mic ----------------------------------------------------------
enum class Trigger : uint8_t { Hold = 0, Lens = 1, Cli = 2, Web = 3 };
enum class Mic : uint8_t { G2 = 0, Pdm = 1 };

inline const char* noteTriggerName(Trigger trigger) {
  static const char* const kNames[] = {"hold", "lens", "cli", "web"};
  const size_t i = static_cast<size_t>(trigger);
  return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : nullptr;
}
inline const char* noteMicName(Mic mic) {
  return mic == Mic::G2 ? "g2" : mic == Mic::Pdm ? "pdm" : nullptr;
}
inline bool noteTokenIs(const char* text, size_t length, const char* word) {
  return text && strlen(word) == length && memcmp(text, word, length) == 0;
}
inline bool noteTriggerParse(const char* text, size_t length, Trigger* out) {
  for (uint8_t i = 0; i < 4; ++i) {
    if (noteTokenIs(text, length, noteTriggerName(static_cast<Trigger>(i)))) {
      if (out) *out = static_cast<Trigger>(i);
      return true;
    }
  }
  return false;
}
inline bool noteMicParse(const char* text, size_t length, Mic* out) {
  if (noteTokenIs(text, length, "g2")) { if (out) *out = Mic::G2; return true; }
  if (noteTokenIs(text, length, "pdm")) { if (out) *out = Mic::Pdm; return true; }
  return false;
}

// --- Words, title and junk ----------------------------------------------------
// Words and the title are the shared speech-text rules in
// Transcript_TextPolicy.h (transcripts use the same ones); these names keep
// QuickNote's callers and tests unchanged.
constexpr size_t kTitleMax = TranscriptText::kTitleMax;
inline bool noteGap(unsigned char c) { return TranscriptText::gap(c); }
inline bool noteWordByte(unsigned char c) { return TranscriptText::wordByte(c); }
inline uint32_t noteWordCount(const char* text, size_t length) {
  return TranscriptText::wordCount(text, length);
}
inline size_t noteTitle(const char* text, size_t length, char* out, size_t cap) {
  return TranscriptText::title(text, length, out, cap);
}

// True when the text is not worth a file: nothing but whitespace or
// punctuation, or a short (< kJunkMaxMs) take of at most two filler words,
// the recogniser's usual output for a breath or a click.
inline bool noteIsJunk(const char* text, size_t length, uint32_t durationMs) {
  static const char* const kFillers[] = {"yeah", "yes", "you", "uh", "um", "okay"};
  char words[3][8] = {};
  size_t count = 0;
  bool filler = true;  // every word so far fits a slot (<= 7 bytes)
  size_t i = 0;
  while (text && i < length) {
    while (i < length && noteGap(static_cast<unsigned char>(text[i]))) ++i;
    size_t start = i;
    while (i < length && !noteGap(static_cast<unsigned char>(text[i]))) ++i;
    size_t end = i;
    // Strip the punctuation around a word ("Okay." "uh,").
    while (start < end && !noteWordByte(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && !noteWordByte(static_cast<unsigned char>(text[end - 1]))) --end;
    if (start == end) continue;
    if (++count > 2) return false;
    if (end - start >= sizeof(words[0])) { filler = false; continue; }
    for (size_t k = 0; k < end - start; ++k) {
      const char c = text[start + k];
      words[count - 1][k] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    }
  }
  if (!count) return true;
  if (!filler || durationMs >= kJunkMaxMs) return false;
  if (count == 2 && !strcmp(words[0], "thank") && !strcmp(words[1], "you")) return true;
  for (size_t w = 0; w < count; ++w) {
    bool known = false;
    for (const char* f : kFillers) known = known || !strcmp(words[w], f);
    if (!known) return false;
  }
  return true;
}

// --- File names ---------------------------------------------------------------
// "yyyymmddThhmmssZ" (UTC) or "boot" when the clock was not synced.
inline bool noteStampValid(const char* stamp, size_t length) {
  if (noteTokenIs(stamp, length, "boot")) return true;
  if (!stamp || length != kStampMax || stamp[8] != 'T' || stamp[15] != 'Z') return false;
  for (size_t i = 0; i < kStampMax; ++i)
    if (i != 8 && i != 15 && (stamp[i] < '0' || stamp[i] > '9')) return false;
  return true;
}
// Exactly 16 hex digits; any case parses, the writer emits lowercase.
inline bool noteParseId(const char* text, size_t length, uint64_t* out) {
  if (!text || length != 16) return false;
  uint64_t id = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = text[i];
    unsigned digit;
    if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
    else return false;
    id = (id << 4) | digit;
  }
  if (!id) return false;
  if (out) *out = id;
  return true;
}
// "<created>-<id16>.txt" (or ".tmp", the store's in-progress name). Returns the
// length, or 0 for an invalid stamp/id or a buffer too small.
inline size_t noteFileName(const char* created, uint64_t id, bool tmp, char* out, size_t cap) {
  if (out && cap) out[0] = '\0';
  if (!out || !id || !created || !noteStampValid(created, strnlen(created, kStampMax + 1))) return 0;
  const int n = snprintf(out, cap, "%s-%08lx%08lx.%s", created,
                         static_cast<unsigned long>(id >> 32),
                         static_cast<unsigned long>(id & 0xffffffffu), tmp ? "tmp" : "txt");
  if (n <= 0 || static_cast<size_t>(n) >= cap) { out[0] = '\0'; return 0; }
  return static_cast<size_t>(n);
}
// Accepts only a name noteFileName(…, tmp, …) writes: a note, or with `tmp`
// the store's in-progress name. `created` (optional) needs kStampMax + 1 bytes.
inline bool noteFileNameParse(const char* name, char* created, uint64_t* id, bool tmp = false) {
  if (!name) return false;
  const size_t length = strnlen(name, kFileNameMax + 1);
  if (length < 4 + 1 + 16 + 4 || length > kFileNameMax) return false;
  const size_t stamp = length - (1 + 16 + 4);
  if (name[stamp] != '-' || memcmp(name + length - 4, tmp ? ".tmp" : ".txt", 4) != 0 ||
      !noteStampValid(name, stamp)) return false;
  // Lowercase only, so one note has exactly one spelling (FAT folds case).
  for (size_t i = stamp + 1; i < stamp + 17; ++i)
    if (name[i] >= 'A' && name[i] <= 'F') return false;
  if (!noteParseId(name + stamp + 1, 16, id)) return false;
  if (created) { memcpy(created, name, stamp); created[stamp] = '\0'; }
  return true;
}
// A new note's id, from 64 random `bits`. A list orders names newest first,
// and with the clock unset the note is named boot-<id>.txt, so its id is
// ordered instead: the boot counter (low 24 bits), the seconds since that
// boot (low 24 bits, ~194 days) and 16 of the random bits. Boot notes then
// list in the order they were started, a later boot's above an earlier one's.
// With the clock set, or no boot counter (0), it is `bits`. 0 only when every
// bit used is 0: the caller draws again.
inline uint64_t noteNewId(bool clockSet, uint32_t boot, uint32_t uptimeS, uint64_t bits) {
  if (clockSet || !boot) return bits;
  return (static_cast<uint64_t>(boot & 0xffffffu) << 40) |
         (static_cast<uint64_t>(uptimeS & 0xffffffu) << 16) | (bits & 0xffffu);
}

// --- Paths --------------------------------------------------------------------
// "<root>/u<owner>/notes", root "/sd/stt" or "/stt". Returns the length, 0 when
// owner is 0 or it does not fit.
inline size_t noteFolder(bool sd, uint32_t owner, char* out, size_t cap) {
  if (out && cap) out[0] = '\0';
  if (!out || !owner) return 0;
  const int n = snprintf(out, cap, "%s/u%lu/notes", sd ? "/sd/stt" : "/stt",
                         static_cast<unsigned long>(owner));
  if (n <= 0 || static_cast<size_t>(n) >= cap) { out[0] = '\0'; return 0; }
  return static_cast<size_t>(n);
}
// Device-wide native G2 notes. Deliberately not an account directory.
inline size_t g2NoteFolder(bool sd, char* out, size_t cap) {
  const int n = snprintf(out, cap, "%s/g2/quicknotes", sd ? "/sd" : "");
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}
inline bool g2NotePath(const char* path) {
  if (!path) return false;
  const char* prefix = !strncmp(path, "/sd/", 4) ? "/sd/g2/quicknotes/" : "/g2/quicknotes/";
  return !strncmp(path, prefix, strlen(prefix)) &&
         noteFileNameParse(path + strlen(prefix), nullptr, nullptr);
}
// True only for a canonical note file path, "<noteFolder>/<note file name>",
// as the guarded VFS's normalizer spells it. Backslashes, dot segments,
// alternate case, FAT 8.3 aliases and anything outside an owner's notes folder
// are rejected; Transcript_PathPolicy decides the owner. Outputs are optional.
inline bool notePathParse(const char* path, uint32_t* owner, bool* sd,
                          char* created, uint64_t* id) {
  const TranscriptPathPolicy::Path where = TranscriptPathPolicy::classify(path);
  if (where.kind != TranscriptPathPolicy::Kind::Owner) return false;
  const bool onCard = strncmp(path, "/sd/", 4) == 0;
  char folder[kPathMax];
  const size_t length = noteFolder(onCard, where.owner, folder, sizeof(folder));
  if (!length || strncmp(path, folder, length) != 0 || path[length] != '/') return false;
  if (!noteFileNameParse(path + length + 1, created, id)) return false;
  if (owner) *owner = where.owner;
  if (sd) *sd = onCard;
  return true;
}

// --- Header -------------------------------------------------------------------
struct NoteHeader {
  uint64_t id = 0;
  char created[kStampMax + 1] = {};
  uint32_t boot = 0;      // 0: unknown (no Boot line, or no boot counter)
  uint32_t uptimeS = 0;
  Trigger trigger = Trigger::Cli;
  Mic mic = Mic::Pdm;
  uint32_t durationMs = 0;
  uint32_t words = 0;
  char title[kTitleMax + 1] = {};
};
constexpr const char* kHeaderMagic = "HardwareOne note v1";

// Writes the header, blank separator line included. Returns its length, or 0
// for an invalid field (id 0, bad stamp/enum, a title with control bytes or
// over kTitleMax) or when it does not fit `cap`.
inline size_t noteFormatHeader(const NoteHeader& header, char* out, size_t cap) {
  if (out && cap) out[0] = '\0';
  const size_t title = strnlen(header.title, sizeof(header.title));
  const char* trigger = noteTriggerName(header.trigger);
  const char* mic = noteMicName(header.mic);
  if (!out || !header.id || !trigger || !mic || title > kTitleMax ||
      !noteStampValid(header.created, strnlen(header.created, sizeof(header.created))))
    return 0;
  for (size_t i = 0; i < title; ++i)
    if (static_cast<unsigned char>(header.title[i]) < ' ' || header.title[i] == 0x7f) return 0;
  const int n = snprintf(out, cap,
                         "%s\nId: %08lx%08lx\nCreated: %s\nBoot: %lu\nUptime-s: %lu\n"
                         "Trigger: %s\nMic: %s\nDuration-ms: %lu\nWords: %lu\nTitle: %.*s\n\n",
                         kHeaderMagic, static_cast<unsigned long>(header.id >> 32),
                         static_cast<unsigned long>(header.id & 0xffffffffu),
                         header.created, static_cast<unsigned long>(header.boot),
                         static_cast<unsigned long>(header.uptimeS), trigger, mic,
                         static_cast<unsigned long>(header.durationMs),
                         static_cast<unsigned long>(header.words),
                         static_cast<int>(title), header.title);
  if (n <= 0 || static_cast<size_t>(n) >= cap) { out[0] = '\0'; return 0; }
  return static_cast<size_t>(n);
}

inline bool noteParseU32(const char* text, size_t length, uint32_t* out) {
  if (!text || !length || length > 10) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + static_cast<unsigned>(text[i] - '0');
  }
  if (value > UINT32_MAX) return false;
  *out = static_cast<uint32_t>(value);
  return true;
}

// Parses the header at the start of `data` (a file's first `length` bytes,
// not NUL-terminated). Needs the magic line, each required v1 key exactly
// once, Boot and Uptime-s at most once, and the blank line inside the window;
// keys may come in any order and unknown keys are skipped. On success
// *bodyOffset (optional) is where the text begins.
inline bool noteParseHeader(const char* data, size_t length, NoteHeader* out,
                            size_t* bodyOffset) {
  if (!data || !out) return false;
  NoteHeader header;
  size_t pos = strlen(kHeaderMagic);
  if (length <= pos || memcmp(data, kHeaderMagic, pos) != 0) return false;
  if (data[pos] == '\r' && pos + 1 < length) ++pos;  // CRLF, as on every line
  if (data[pos++] != '\n') return false;
  enum : uint16_t { kId = 1, kCreated = 2, kTrigger = 4, kMic = 8, kDuration = 16,
                    kWords = 32, kTitle = 64, kAll = 127, kBoot = 128, kUptime = 256 };
  uint16_t seen = 0;
  while (pos < length) {
    const char* line = data + pos;
    const char* newline = static_cast<const char*>(memchr(line, '\n', length - pos));
    if (!newline) return false;  // the header must end inside the window
    size_t lineLength = static_cast<size_t>(newline - line);
    pos += lineLength + 1;
    if (lineLength && line[lineLength - 1] == '\r') --lineLength;
    if (!lineLength) {
      if ((seen & kAll) != kAll) return false;
      *out = header;
      if (bodyOffset) *bodyOffset = pos;
      return true;
    }
    // "Key: value"; the writer keeps the space even for an empty title.
    const char* colon = static_cast<const char*>(memchr(line, ':', lineLength));
    if (!colon || colon + 1 >= line + lineLength || colon[1] != ' ') return false;
    const size_t keyLength = static_cast<size_t>(colon - line);
    const char* value = colon + 2;
    const size_t valueLength = lineLength - keyLength - 2;
    uint16_t bit = 0;
    bool ok = true;
    if (noteTokenIs(line, keyLength, "Id")) {
      bit = kId; ok = noteParseId(value, valueLength, &header.id);
    } else if (noteTokenIs(line, keyLength, "Created")) {
      bit = kCreated; ok = noteStampValid(value, valueLength);
      if (ok) { memcpy(header.created, value, valueLength); header.created[valueLength] = '\0'; }
    } else if (noteTokenIs(line, keyLength, "Boot")) {
      bit = kBoot; ok = noteParseU32(value, valueLength, &header.boot);
    } else if (noteTokenIs(line, keyLength, "Uptime-s")) {
      bit = kUptime; ok = noteParseU32(value, valueLength, &header.uptimeS);
    } else if (noteTokenIs(line, keyLength, "Trigger")) {
      bit = kTrigger; ok = noteTriggerParse(value, valueLength, &header.trigger);
    } else if (noteTokenIs(line, keyLength, "Mic")) {
      bit = kMic; ok = noteMicParse(value, valueLength, &header.mic);
    } else if (noteTokenIs(line, keyLength, "Duration-ms")) {
      bit = kDuration; ok = noteParseU32(value, valueLength, &header.durationMs);
    } else if (noteTokenIs(line, keyLength, "Words")) {
      bit = kWords; ok = noteParseU32(value, valueLength, &header.words);
    } else if (noteTokenIs(line, keyLength, "Title")) {
      bit = kTitle; ok = valueLength <= kTitleMax;
      for (size_t i = 0; ok && i < valueLength; ++i)
        ok = static_cast<unsigned char>(value[i]) >= ' ' && value[i] != 0x7f;
      if (ok) { memcpy(header.title, value, valueLength); header.title[valueLength] = '\0'; }
    }
    if (!ok || (bit & seen)) return false;
    seen = static_cast<uint16_t>(seen | bit);
  }
  return false;
}

// --- List times ---------------------------------------------------------------
// A stamp's local "hh:mm" at a fixed UTC offset in minutes: HW1's timezone
// has no DST rule (System_Clock applies tzOffsetMinutes as it is), so this is
// what localtime_r gives, without the date arithmetic. False (`out` untouched)
// for "boot" or an invalid stamp.
inline bool noteLocalTime(const char* stamp, int tzOffsetMinutes, char* out, size_t cap) {
  const size_t length = stamp ? strnlen(stamp, kStampMax + 1) : 0;
  if (!out || !cap || length != kStampMax || !noteStampValid(stamp, length)) return false;
  const int hour = (stamp[9] - '0') * 10 + (stamp[10] - '0');
  const int minute = (stamp[11] - '0') * 10 + (stamp[12] - '0');
  if (hour > 23 || minute > 59) return false;
  const int local = ((hour * 60 + minute + tzOffsetMinutes) % 1440 + 1440) % 1440;
  snprintf(out, cap, "%02d:%02d", local / 60, local % 60);
  return true;
}

// "49710d ago" (the longest uptime label) or "hh:mm", plus the NUL.
constexpr size_t kWhenMax = 12;
// How a list shows the time of a note with no wall-clock stamp: its age when
// it was saved in the current boot ("just now", "5m ago", "2h ago", "3d ago"),
// else "earlier" (another boot, or a note without Boot/Uptime-s). `boot` and
// `uptimeS` are now. Returns the length.
inline size_t noteAgeLabel(uint32_t noteBoot, uint32_t noteUptimeS, uint32_t boot,
                           uint32_t uptimeS, char* out, size_t cap) {
  if (!out || !cap) return 0;
  const unsigned long age = uptimeS - noteUptimeS;
  const unsigned long unit = age < 3600 ? 60 : age < 86400 ? 3600 : 86400;
  const int n = !noteBoot || noteBoot != boot || uptimeS < noteUptimeS ? snprintf(out, cap, "earlier")
              : age < 60 ? snprintf(out, cap, "just now")
              : snprintf(out, cap, "%lu%c ago", age / unit, unit == 60 ? 'm' : unit == 3600 ? 'h' : 'd');
  if (n <= 0) { out[0] = '\0'; return 0; }
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

}  // namespace QuickNotePolicy

#endif  // QUICKNOTE_POLICY_H
