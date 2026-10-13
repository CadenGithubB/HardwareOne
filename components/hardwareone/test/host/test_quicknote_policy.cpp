// Host-side tests for the exact production QuickNote policy header: lifecycle
// FSM, limits, title and junk heuristics, the v1 header, file names and the
// private notes path.
#include <cstdio>
#include <cstring>
#include <string>

#include "../../QuickNote_Policy.h"

using namespace QuickNotePolicy;

static int failures = 0;

#define CHECK(condition, message)                                            \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static std::string title(const char* text, size_t cap = kTitleMax + 1) {
  char out[128];
  std::memset(out, 'x', sizeof(out));
  const size_t n = noteTitle(text, text ? std::strlen(text) : 0, out, cap);
  if (n != std::strlen(out)) return "<length mismatch>";
  return out;
}

static bool junk(const char* text, uint32_t ms) {
  return noteIsJunk(text, std::strlen(text), ms);
}

static void test_fsm_table() {
  constexpr int kPhases = 7, kEvents = 8;
  // expected[phase][event]; -1 = the event does not apply (phase unchanged).
  using P = Phase;
  const int I = int(P::Idle), S = int(P::Starting), R = int(P::Recording),
            Fi = int(P::Finishing), Sa = int(P::Saving), F = int(P::Failed),
            D = int(P::Discarded), X = -1;
  //                         Start CapAct Stop Finished Saved Discard Fail Clear
  const int expected[kPhases][kEvents] = {
      /* Idle      */ {S, X, X, X, X, X, X, X},
      /* Starting  */ {X, R, D, X, X, D, F, X},
      /* Recording */ {X, X, Fi, Sa, X, D, F, X},
      /* Finishing */ {X, X, X, Sa, X, D, F, X},
      /* Saving    */ {X, X, X, X, I, D, F, X},
      /* Failed    */ {X, X, X, X, X, X, X, I},
      /* Discarded */ {X, X, X, X, X, X, X, I},
  };
  for (int p = 0; p < kPhases; ++p) {
    for (int e = 0; e < kEvents; ++e) {
      const Phase from = Phase(p);
      const int want = expected[p][e] < 0 ? p : expected[p][e];
      CHECK(int(noteNext(from, Event(e))) == want, "FSM transition table");
      Phase moved = from;
      const bool applied = noteTransition(moved, Event(e));
      CHECK(applied == (expected[p][e] >= 0), "noteTransition reports applicability");
      CHECK(int(moved) == want, "noteTransition applies exactly noteNext");
    }
  }
  // The happy path, and every exit is cleared back to Idle.
  Phase phase = Phase::Idle;
  for (Event e : {Event::Start, Event::CaptureActive, Event::Stop, Event::Finished, Event::Saved})
    CHECK(noteTransition(phase, e), "happy path step");
  CHECK(phase == Phase::Idle, "a saved note returns to Idle");
  CHECK(!noteActive(Phase::Idle) && noteActive(Phase::Starting) &&
            noteActive(Phase::Recording) && noteActive(Phase::Finishing) &&
            noteActive(Phase::Saving) && !noteActive(Phase::Failed) &&
            !noteActive(Phase::Discarded),
        "only Starting..Saving hold the lease");
  CHECK(!std::strcmp(notePhaseName(Phase::Recording), "recording") &&
            !std::strcmp(notePhaseName(Phase::Discarded), "discarded") &&
            !std::strcmp(notePhaseName(Phase(42)), "?"),
        "phase names");
}

static void test_limits() {
  CHECK(kMinHoldMs == 600 && kMaxNoteBytes == 4096 && kTitleMax == 48, "spec constants");
  CHECK(noteHoldTooShort(false, 5000), "no capture is always too short");
  CHECK(noteHoldTooShort(true, 599) && !noteHoldTooShort(true, 600),
        "600 ms of audio is the minimum");
  CHECK(noteFinishTimeoutMs(true) == 30000 && noteFinishTimeoutMs(false) == 95000,
        "finish timeout local / CM5");
  CHECK(kFinishTimeoutCm5Ms > 90000, "CM5 finish covers the 90 s host result timeout");
}

static void test_words_and_title() {
  CHECK(noteWordCount("", 0) == 0 && noteWordCount(" \n\t ", 4) == 0, "no words");
  CHECK(noteWordCount("Buy oat milk, and call the dentist.", 35) == 7, "seven words");
  CHECK(noteWordCount("... - !", 7) == 0, "punctuation is not a word");
  CHECK(noteWordCount("caf\xc3\xa9 ok", 8) == 2, "non-ASCII bytes form words");

  CHECK(title("Buy oat milk") == "Buy oat milk", "short title is the text");
  CHECK(title("  Buy\n\noat \t milk  ") == "Buy oat milk", "whitespace collapses");
  CHECK(title("a\x01" "b") == "a b", "control bytes separate words");
  CHECK(title("") == "" && title(nullptr) == "" && title(" \n ") == "", "empty title");
  const char* longText =
      "Remember to buy oat milk and call the dentist about the appointment tomorrow";
  const std::string cut = title(longText);
  CHECK(cut == "Remember to buy oat milk and call the dentist", "cut at a word boundary");
  CHECK(cut.size() <= kTitleMax, "title fits 48 bytes");
  std::string exact(48, 'a');
  CHECK(title((exact + " more").c_str()) == exact, "a 48-byte first word is kept whole");
  CHECK(title((std::string(47, 'a') + " b").c_str()) == std::string(47, 'a'),
        "no room for a space and a one-byte word");
  CHECK(title((std::string(46, 'a') + " b c").c_str()) == std::string(46, 'a') + " b",
        "a word ending exactly at 48 bytes fits");
  CHECK(title(std::string(60, 'z').c_str()) == std::string(48, 'z'),
        "one oversized word is hard-cut at 48 bytes");
  // 47 ASCII bytes then "é" (2 bytes): the character would straddle byte 48.
  CHECK(title((std::string(47, 'a') + "\xc3\xa9").c_str()) == std::string(47, 'a'),
        "a hard cut never splits a UTF-8 character");
  CHECK(title((std::string(46, 'a') + "\xe2\x82\xac" "x").c_str()) == std::string(46, 'a'),
        "a 3-byte character straddling the limit is dropped whole");
  CHECK(title((std::string(46, 'a') + "\xc3\xa9" "x").c_str()) ==
            std::string(46, 'a') + "\xc3\xa9",
        "a character ending exactly at the limit is kept");
  CHECK(title("hello world", 6) == "hello" && title("hello world", 4) == "hel" &&
            title("hello", 1) == "",
        "a smaller buffer bounds the title");
  char tiny[1] = {'x'};
  CHECK(noteTitle("abc", 3, tiny, 0) == 0 && tiny[0] == 'x', "cap 0 writes nothing");
  CHECK(noteTitle("abc def", 7, tiny, 1) == 0 && tiny[0] == 0, "cap 1 is an empty title");
  char some[16];
  CHECK(noteTitle("abc def", 3, some, sizeof(some)) == 3 && !std::strcmp(some, "abc"),
        "length, not the NUL, bounds the input");
}

static void test_junk() {
  CHECK(junk("", 100) && junk("   \n", 9000) && junk("...", 9000), "empty is junk at any length");
  CHECK(junk("yeah", 800) && junk("Yes.", 2499) && junk("you", 100) && junk("uh", 100) &&
            junk("um,", 100) && junk("Okay!", 100),
        "one short filler word is junk");
  CHECK(junk("um uh", 1000) && junk("Okay okay", 2000) && junk("Thank you.", 1500),
        "two short filler words, or thank you, are junk");
  CHECK(!junk("yeah", 2500) && !junk("Thank you.", 3000), "2.5 s or longer is kept");
  CHECK(!junk("um uh um", 1000), "three words are kept");
  CHECK(!junk("thank", 1000) && !junk("you thank", 1000), "thank alone is not a filler");
  CHECK(!junk("milk", 900) && !junk("buy milk", 900), "real words are kept");
  CHECK(!junk("yeahyeahyeah", 900), "a long word is not a filler");
  CHECK(!junk("ok", 900), "only the listed fillers count");
}

static NoteHeader sample() {
  NoteHeader h;
  h.id = 0x6f1c9a2e00000007ull;
  std::strcpy(h.created, "20261004T153012Z");
  h.boot = 76;
  h.uptimeS = 1234;
  h.trigger = Trigger::Hold;
  h.mic = Mic::G2;
  h.durationMs = 8420;
  h.words = 9;
  std::strcpy(h.title, "Buy oat milk and call the dentist");
  return h;
}

static bool sameHeader(const NoteHeader& a, const NoteHeader& b) {
  return a.id == b.id && !std::strcmp(a.created, b.created) && a.boot == b.boot &&
         a.uptimeS == b.uptimeS && a.trigger == b.trigger &&
         a.mic == b.mic && a.durationMs == b.durationMs && a.words == b.words &&
         !std::strcmp(a.title, b.title);
}

static bool parses(const std::string& text, NoteHeader* out = nullptr, size_t* body = nullptr) {
  NoteHeader scratch;
  return noteParseHeader(text.data(), text.size(), out ? out : &scratch, body);
}

static void test_header() {
  char buffer[kHeaderMax];
  NoteHeader h = sample();
  const size_t n = noteFormatHeader(h, buffer, sizeof(buffer));
  const std::string expect =
      "HardwareOne note v1\nId: 6f1c9a2e00000007\nCreated: 20261004T153012Z\n"
      "Boot: 76\nUptime-s: 1234\nTrigger: hold\nMic: g2\nDuration-ms: 8420\nWords: 9\n"
      "Title: Buy oat milk and call the dentist\n\n";
  CHECK(n == expect.size() && buffer == expect, "v1 header layout");
  NoteHeader back;
  size_t body = 0;
  const std::string file = expect + "Buy oat milk and call the dentist.";
  CHECK(parses(file, &back, &body) && sameHeader(back, h) && body == expect.size(),
        "header round-trips and finds the text");
  // Every trigger / mic / boot stamp / empty title round-trips.
  for (int t = 0; t < 4; ++t) {
    for (int m = 0; m < 2; ++m) {
      NoteHeader v = sample();
      v.trigger = Trigger(t);
      v.mic = Mic(m);
      std::strcpy(v.created, "boot");
      v.title[0] = 0;
      v.durationMs = UINT32_MAX;
      v.words = 0;
      v.boot = UINT32_MAX;
      v.uptimeS = 0;
      const size_t len = noteFormatHeader(v, buffer, sizeof(buffer));
      CHECK(len && parses(std::string(buffer, len), &back) && sameHeader(back, v),
            "variant round-trips");
    }
  }
  NoteHeader full = sample();
  std::memset(full.title, 'T', kTitleMax);
  full.title[kTitleMax] = 0;
  full.boot = full.uptimeS = full.durationMs = full.words = UINT32_MAX;
  const size_t fullLength = noteFormatHeader(full, buffer, sizeof(buffer));
  CHECK(fullLength && fullLength < kHeaderMax && parses(std::string(buffer, fullLength), &back) &&
            sameHeader(back, full),
        "a maximal header fits and round-trips");
  CHECK(!noteFormatHeader(full, buffer, fullLength) && !buffer[0], "a short buffer refuses");
  // Invalid fields are refused, never written.
  NoteHeader bad = sample();
  bad.id = 0;
  CHECK(!noteFormatHeader(bad, buffer, sizeof(buffer)), "id 0 refused");
  bad = sample();
  std::strcpy(bad.created, "2026-10-04");
  CHECK(!noteFormatHeader(bad, buffer, sizeof(buffer)), "bad stamp refused");
  bad = sample();
  bad.trigger = Trigger(9);
  CHECK(!noteFormatHeader(bad, buffer, sizeof(buffer)), "bad trigger refused");
  bad = sample();
  bad.mic = Mic(7);
  CHECK(!noteFormatHeader(bad, buffer, sizeof(buffer)), "bad mic refused");
  bad = sample();
  std::strcpy(bad.title, "two\nlines");
  CHECK(!noteFormatHeader(bad, buffer, sizeof(buffer)), "a newline cannot enter the header");
  bad = sample();
  std::memset(bad.title, 'T', sizeof(bad.title));  // unterminated
  CHECK(!noteFormatHeader(bad, buffer, sizeof(buffer)), "unterminated title refused");

  // Parser strictness.
  for (size_t cut = 0; cut < expect.size(); ++cut)
    CHECK(!parses(expect.substr(0, cut)), "a header cut short of its blank line fails");
  CHECK(!parses("HardwareOne note v2\n" + expect.substr(20)), "wrong magic");
  CHECK(!parses("hardwareone note v1\n" + expect.substr(20)), "magic is exact");
  std::string reordered =
      "HardwareOne note v1\nTitle: x\nWords: 1\nDuration-ms: 1\nMic: pdm\n"
      "Trigger: cli\nCreated: boot\nId: 0000000000000001\nUid: 77\n\nx";
  CHECK(parses(reordered, &back) && back.id == 1 && back.mic == Mic::Pdm &&
            back.trigger == Trigger::Cli && !std::strcmp(back.title, "x"),
        "any key order; unknown keys skipped");
  CHECK(back.boot == 0 && back.uptimeS == 0, "a note without Boot/Uptime-s (older firmware) parses as unknown");
  std::string old = expect;
  old.erase(old.find("Boot: 76\n"), std::strlen("Boot: 76\nUptime-s: 1234\n"));
  NoteHeader legacy = sample();
  legacy.boot = legacy.uptimeS = 0;
  CHECK(parses(old, &back, &body) && sameHeader(back, legacy) && body == old.size(),
        "a v1 header written before Boot/Uptime-s still parses");
  CHECK(parses("HardwareOne note v1\nUptime-s: 9\nId: 0000000000000001\nCreated: boot\nTrigger: cli\n"
               "Mic: pdm\nDuration-ms: 1\nWords: 1\nTitle: x\n\nx", &back) &&
            back.boot == 0 && back.uptimeS == 9,
        "Boot and Uptime-s are independent and optional");
  std::string crlf = expect;
  for (size_t at = crlf.find('\n'); at != std::string::npos; at = crlf.find('\n', at + 2))
    crlf.insert(at, "\r");
  CHECK(parses(crlf, &back) && sameHeader(back, h), "CRLF lines parse");
  const struct { const char* from; const char* to; const char* why; } mutations[] = {
      {"Id: 6f1c9a2e00000007\n", "", "missing Id"},
      {"Words: 9\n", "", "missing Words"},
      {"Title: Buy oat milk and call the dentist\n", "", "missing Title"},
      {"Words: 9\n", "Words: 9\nWords: 9\n", "duplicate key"},
      {"Id: 6f1c9a2e00000007", "Id: 0000000000000000", "id 0"},
      {"Id: 6f1c9a2e00000007", "Id: 6f1c9a2e0000007", "short id"},
      {"Created: 20261004T153012Z", "Created: 20261004 153012Z", "bad stamp"},
      {"Trigger: hold", "Trigger: ring-hold", "unknown trigger"},
      {"Mic: g2", "Mic: g2-native", "unknown mic"},
      {"Duration-ms: 8420", "Duration-ms: 4294967296", "duration overflow"},
      {"Words: 9", "Words: nine", "non-numeric words"},
      {"Words: 9", "Words:9", "key without the space"},
      {"Words: 9", "Words", "line without a colon"},
      {"Boot: 76\n", "Boot: 76\nBoot: 77\n", "duplicate Boot"},
      {"Uptime-s: 1234\n", "Uptime-s: 1234\nUptime-s: 1\n", "duplicate Uptime-s"},
      {"Boot: 76", "Boot: seventy", "non-numeric Boot"},
      {"Uptime-s: 1234", "Uptime-s: 4294967296", "Uptime-s overflow"},
      {"Uptime-s: 1234", "Uptime-s: ", "empty Uptime-s"},
      {"Title: Buy", "Title: " "0123456789012345678901234567890123456789012345678" " Buy",
       "title over 48 bytes"},
  };
  for (const auto& m : mutations) {
    std::string text = expect;
    const size_t at = text.find(m.from);
    if (at == std::string::npos) { CHECK(false, m.why); continue; }
    text.replace(at, std::strlen(m.from), m.to);
    CHECK(!parses(text), m.why);
  }
  CHECK(!noteParseHeader(nullptr, 0, &back, nullptr) &&
            !noteParseHeader(expect.data(), expect.size(), nullptr, nullptr),
        "null arguments");
}

static void test_file_names() {
  char name[64];
  CHECK(noteFileName("20261004T153012Z", 0x6f1c9a2e00000007ull, false, name, sizeof(name)) == 37 &&
            !std::strcmp(name, "20261004T153012Z-6f1c9a2e00000007.txt"),
        "UTC name");
  CHECK(noteFileName("boot", 0xABCull, false, name, sizeof(name)) == 25 &&
            !std::strcmp(name, "boot-0000000000000abc.txt"),
        "boot name, lowercase hex");
  CHECK(noteFileName("boot", 1, true, name, sizeof(name)) &&
            !std::strcmp(name, "boot-0000000000000001.tmp"),
        "in-progress name");
  CHECK(!noteFileName("boot", 0, false, name, sizeof(name)) && !name[0], "id 0 refused");
  CHECK(!noteFileName("20261004T15301Z", 1, false, name, sizeof(name)), "short stamp refused");
  CHECK(!noteFileName("20261004T153012Z", 1, false, name, 37), "no room for the NUL");
  CHECK(!noteFileName(nullptr, 1, false, name, sizeof(name)), "null stamp");
  CHECK(kFileNameMax == 37, "longest name");

  char created[kStampMax + 1];
  uint64_t id = 0;
  CHECK(noteFileNameParse("20261004T153012Z-6f1c9a2e00000007.txt", created, &id) &&
            !std::strcmp(created, "20261004T153012Z") && id == 0x6f1c9a2e00000007ull,
        "name parses");
  CHECK(noteFileNameParse("boot-0000000000000abc.txt", created, &id) &&
            !std::strcmp(created, "boot") && id == 0xabc,
        "boot name parses");
  for (const char* bad : {"boot-0000000000000ABC.txt", "boot-0000000000000000.txt",
                          "boot-0000000000000abc.tmp", "boot-0000000000000abc.TXT",
                          "boot-000000000000abc.txt", "boot-00000000000000abc.txt",
                          "Boot-0000000000000abc.txt", "boot_0000000000000abc.txt",
                          "20261004T153012-6f1c9a2e00000007.txt", "x.txt", "",
                          "20261004T153012Z-6f1c9a2e00000007.txt.", "BOOT~1.TXT",
                          "boot-0000000000000abc.txt ", "../boot-0000000000000abc.txt",
                          "boot-0000000000000ab\\.txt", "2026100AT153012Z-6f1c9a2e00000007.txt"})
    CHECK(!noteFileNameParse(bad, nullptr, nullptr), bad);
  CHECK(!noteFileNameParse(nullptr, nullptr, nullptr), "null name");
  // The store's in-progress spelling, only when asked for (the sweep).
  CHECK(noteFileNameParse("20261004T153012Z-6f1c9a2e00000007.tmp", created, &id, true) &&
            !std::strcmp(created, "20261004T153012Z") && id == 0x6f1c9a2e00000007ull,
        "in-progress name parses as one");
  CHECK(noteFileNameParse("boot-0000000000000abc.tmp", nullptr, nullptr, true), "boot in-progress name");
  for (const char* bad : {"boot-0000000000000abc.txt", "boot-0000000000000ABC.tmp", "notes.tmp",
                          "boot-0000000000000abc.TMP", "boot-0000000000000000.tmp",
                          "boot-0000000000000abc.header.tmp", "x.tmp"})
    CHECK(!noteFileNameParse(bad, nullptr, nullptr, true), bad);
}

static void test_ids() {
  const uint64_t bits = 0x0123456789abcdefull;
  CHECK(noteNewId(true, 76, 1000, bits) == bits, "clock set: random");
  CHECK(noteNewId(false, 0, 1000, bits) == bits, "no boot counter: random");
  CHECK(noteNewId(false, 76, 1000, bits) == 0x00004c0003e8cdefull, "boot, uptime, 16 random bits");
  CHECK(noteNewId(false, 0x1234567, 0x1abcdef, 0) == 0x234567abcdef0000ull, "24-bit fields");
  CHECK(!noteNewId(false, 1u << 24, 1u << 24, 0x10000), "every bit used 0: the caller draws again");
  // boot-<id> names sort as a list orders them: by boot, then by uptime,
  // whatever the random bits.
  char first[64], second[64], third[64];
  CHECK(noteFileName("boot", noteNewId(false, 76, 59, ~0ull), false, first, sizeof(first)) &&
            noteFileName("boot", noteNewId(false, 76, 60, 0x10000), false, second, sizeof(second)) &&
            noteFileName("boot", noteNewId(false, 77, 1, 1), false, third, sizeof(third)),
        "boot names");
  CHECK(std::strcmp(first, second) < 0 && std::strcmp(second, third) < 0, "later boot names sort higher");
}

static void test_paths() {
  char folder[kPathMax];
  CHECK(noteFolder(false, 7, folder, sizeof(folder)) == 13 && !std::strcmp(folder, "/stt/u7/notes"),
        "internal folder");
  CHECK(noteFolder(true, 4294967295u, folder, sizeof(folder)) &&
            !std::strcmp(folder, "/sd/stt/u4294967295/notes"),
        "SD folder, largest id");
  CHECK(!noteFolder(false, 0, folder, sizeof(folder)) && !folder[0], "owner 0 refused");
  CHECK(!noteFolder(false, 7, folder, 13), "no room");
  const std::string longest = std::string("/sd/stt/u4294967295/notes/") +
                              "20261004T153012Z-6f1c9a2e00000007.txt";
  CHECK(longest.size() < kPathMax, "the longest note path fits kPathMax");

  uint32_t owner = 0;
  bool sd = true;
  char created[kStampMax + 1];
  uint64_t id = 0;
  CHECK(notePathParse("/stt/u7/notes/boot-0000000000000abc.txt", &owner, &sd, created, &id) &&
            owner == 7 && !sd && id == 0xabc && !std::strcmp(created, "boot"),
        "internal note path");
  CHECK(notePathParse(longest.c_str(), &owner, &sd, nullptr, nullptr) && owner == 4294967295u && sd,
        "SD note path");
  // Transcript_PathPolicy classifies the folder as the owner's private tree.
  const auto where = TranscriptPathPolicy::classify("/stt/u7/notes/x.txt");
  CHECK(where.kind == TranscriptPathPolicy::Kind::Owner && where.owner == 7,
        "notes folder is Owner(7) with no policy change");
  CHECK(TranscriptPathPolicy::classify("/sd/stt/u7/notes/x.txt").kind ==
            TranscriptPathPolicy::Kind::Owner,
        "SD notes folder is Owner too");
  const char* name = "boot-0000000000000abc.txt";
  for (const std::string& bad : {
           std::string("/stt/u7/") + name,                 // a transcript, not a note
           std::string("/stt/u7/notes/sub/") + name,       // nested
           std::string("/stt/u7/Notes/") + name,           // case
           std::string("/stt/u7/notes//") + name,          // not canonical
           std::string("/stt/u7/notes/./") + name,
           std::string("/stt/u7/notes\\") + name,          // LittleFS literal backslash
           std::string("/sd/stt/u7/notes\\") + name,       // FAT separator
           std::string("/sd/stt\\u7/notes/") + name,
           std::string("/sd/STT/u7/notes/") + name,        // FAT case alias
           std::string("/sd/stt./u7/notes/") + name,       // FAT trailing dot
           std::string("/sd/stt /u7/notes/") + name,
           std::string("/sd/./stt/u7/notes/") + name,
           std::string("/sd//stt/u7/notes/") + name,
           std::string("/stt/u07/notes/") + name,          // owner spellings
           std::string("/stt/U7/notes/") + name,
           std::string("/stt/u7~1/notes/") + name,
           std::string("/stt/u0/notes/") + name,
           std::string("/stt2/u7/notes/") + name,          // outside the tree
           std::string("/notes/") + name,
           std::string("stt/u7/notes/") + name,
           std::string("/stt/u7/notes/boot-0000000000000abc.tmp"),
           std::string("/stt/u7/notes/BOOT-0000000000000ABC.TXT"),
           std::string("/stt/u7/notes"),
           std::string("/stt/u7/notes/"),
       })
    CHECK(!notePathParse(bad.c_str(), nullptr, nullptr, nullptr, nullptr), bad.c_str());
  CHECK(!notePathParse(nullptr, nullptr, nullptr, nullptr, nullptr), "null path");
}

static std::string localTime(const char* stamp, int offset) {
  char out[16] = "untouched";
  return noteLocalTime(stamp, offset, out, sizeof(out)) ? out : std::string("!") + out;
}
static std::string age(uint32_t noteBoot, uint32_t noteUptime, uint32_t boot, uint32_t uptime,
                       size_t cap = kWhenMax) {
  char out[64];
  std::memset(out, 'x', sizeof(out));
  const size_t n = noteAgeLabel(noteBoot, noteUptime, boot, uptime, out, cap);
  return n == std::strlen(out) ? out : "<length mismatch>";
}

static void test_list_times() {
  // A real stamp at HW1's fixed offset (minutes east of UTC; no DST rule).
  CHECK(localTime("20261004T153012Z", 0) == "15:30", "UTC");
  CHECK(localTime("20261004T153012Z", -300) == "10:30", "UTC-5");
  CHECK(localTime("20261004T153012Z", 330) == "21:00", "UTC+5:30");
  CHECK(localTime("20261004T013012Z", -480) == "17:30", "back across midnight");
  CHECK(localTime("20261004T233012Z", 840) == "13:30", "forward across midnight (UTC+14)");
  CHECK(localTime("20261004T000000Z", -720) == "12:00" && localTime("20261004T235959Z", 0) == "23:59",
        "the ends of the day and of the offset range");
  for (const char* bad : {"boot", "", "20261004T243012Z", "20261004T156012Z", "2026-10-04T15:30Z",
                          "20261004T153012", "20261004T153012ZZ"})
    CHECK(localTime(bad, 0) == "!untouched", bad);
  char small[4] = "abc";
  CHECK(!noteLocalTime(nullptr, 0, small, sizeof(small)) && !noteLocalTime("20261004T153012Z", 0, nullptr, 8) &&
            !noteLocalTime("20261004T153012Z", 0, small, 0) && !std::strcmp(small, "abc"),
        "no stamp or no buffer: nothing written");
  CHECK(noteLocalTime("20261004T153012Z", 0, small, sizeof(small)) && !std::strcmp(small, "15:"),
        "a short buffer truncates");

  // Saved this boot: its age; another boot or unknown: "earlier".
  CHECK(age(76, 100, 76, 100) == "just now", "same second");
  CHECK(age(76, 100, 76, 159) == "just now", "under a minute");
  CHECK(age(76, 100, 76, 160) == "1m ago", "a minute");
  CHECK(age(76, 100, 76, 100 + 59 * 60 + 59) == "59m ago", "under an hour");
  CHECK(age(76, 100, 76, 100 + 3600) == "1h ago", "an hour");
  CHECK(age(76, 0, 76, 86399) == "23h ago", "under a day");
  CHECK(age(76, 0, 76, 3 * 86400 + 5) == "3d ago", "days");
  CHECK(age(76, 0, 76, UINT32_MAX) == "49710d ago" && std::strlen("49710d ago") < kWhenMax,
        "the longest label fits kWhenMax");
  CHECK(age(75, 100, 76, 500) == "earlier", "another boot");
  CHECK(age(0, 100, 0, 500) == "earlier", "no boot counter: never claims this boot");
  CHECK(age(0, 0, 76, 500) == "earlier", "a note without Boot/Uptime-s");
  CHECK(age(76, 600, 76, 500) == "earlier", "saved later than now: not this boot");
  CHECK(age(76, 100, 76, 160, 4) == "1m " && age(76, 100, 76, 160, 1).empty(), "truncates to the buffer");
  char out[4] = "zz";
  CHECK(!noteAgeLabel(76, 1, 76, 2, out, 0) && !std::strcmp(out, "zz") &&
            !noteAgeLabel(76, 1, 76, 2, nullptr, 4),
        "no buffer, no write");
}

int main() {
  test_fsm_table();
  test_limits();
  test_words_and_title();
  test_junk();
  test_header();
  test_file_names();
  test_ids();
  test_paths();
  test_list_times();
  if (failures) {
    std::fprintf(stderr, "%d QuickNote policy check(s) failed\n", failures);
    return 1;
  }
  std::puts("QuickNote policy: FSM, limits, title, junk, header, names, ids, paths and list times passed");
  return 0;
}
