// B11: which spellings of a card path the guarded VFS takes
// (System_SdPathCore.h, compiled as it is). FatFs ignores ASCII case, takes
// '\' as a separator, drops trailing dots and spaces from every segment and
// answers to 8.3 aliases; the path rules compare bytes. Every spelling that
// FatFs would resolve to a protected folder, other than the canonical one, is
// refused, for every protected folder.
#include "System_SdPathCore.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } \
  ++checks; } while (0)

using sdpath::Verdict;

static Verdict of(const std::string& path) { return sdpath::classify(path.c_str()); }

static std::string upper(std::string text) {
  for (char& c : text) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return text;
}

// The 8.3 alias FatFs gives a long name: six bytes of it and "~1".
static std::string alias(const std::string& name) {
  std::string base;
  for (char c : name) {
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) base += c;
    if (base.size() == 6) break;
  }
  return upper(base) + "~1";
}

int main() {
  // Not the card: LittleFS compares bytes, like the rules, so nothing is refused.
  for (const char* path : {"/", "/system/sys_logs/a.log", "/System/x", "/a\\b", "/x~1", "/x.", "/x ",
                           "/sdcard/System", "/sd.x/y", "/SD/System/x", "/sd\\system\\x"}) {
    CHECK(of(path) == Verdict::NotCard);
  }
  CHECK(sdpath::classify(nullptr) == Verdict::NotCard);

  // The card, in the one spelling the rules know.
  for (const char* path : {"/sd", "/sd/system", "/sd/system/sys_logs/failed_login.log",
                           "/sd/logging_captures/sensors/2026-11-22/health-2026-11-22.csv",
                           "/sd/stt/u7/20261122T101500Z-local-0000000100000002.txt", "/sd/stt/u7/notes/a.txt",
                           "/sd/g2/quicknotes/a.txt", "/sd/collect/aabbccddeeff/events.log", "/sd/espnow/cap.bin",
                           "/sd/g2_icon_animations/pack/a.bin", "/sd/VIDEOS/clip.avi", "/sd/STT Models/model.bin",
                           "/sd/STT Raw/a.wav", "/sd/PICTURES/IMG 0001.jpg", "/sd/recordings/take one.wav",
                           "/sd/logging_captures/sensors/.anchors.csv", "/sd/notes/a.b.c.txt",
                           "/sd/notes/~tilde.txt", "/sd/notes/a~z.txt", "/sd/notes/longer-name~1.txt",
                           "/sd/notes/x~1.backup", "/sd/Music/Song.mp3", "/sd/system/sys_logs/errors.log.foreign-1"}) {
    CHECK(of(path) == Verdict::Accepted);
  }

  // Every protected folder: its canonical spelling passes, every other
  // spelling FatFs resolves to it is refused, alone and with a tail.
  size_t folders = 0;
  for (const char* name : sdpath::kProtectedFolders) {
    ++folders;
    const std::string canonical = std::string("/sd/") + name;
    CHECK(of(canonical) == Verdict::Accepted);
    CHECK(of(canonical + "/x.log") == Verdict::Accepted);
    std::string mixed = name;
    mixed[0] = char(mixed[0] - 'a' + 'A');
    const std::vector<std::string> spellings = {
        "/sd/" + upper(name),           // other case
        "/sd/" + mixed,                 // one letter's case
        "/sd/" + std::string(name) + ".",   // trailing dot
        "/sd/" + std::string(name) + " ",   // trailing space
        "/sd/" + std::string(name) + ". .", // both
        "/sd/" + alias(name),           // the 8.3 alias
        "/sd/" + upper(name) + ".",     // case and a dot
    };
    for (const std::string& spelling : spellings) {
      CHECK(of(spelling) == Verdict::Refused);
      CHECK(of(spelling + "/x.log") == Verdict::Refused);
      CHECK(of(spelling + "/sys_logs/failed_login.log") == Verdict::Refused);
    }
    // '\' is a separator to FatFs: as the separator after the folder and
    // inside the tail.
    CHECK(of(canonical + "\\x.log") == Verdict::Refused);
    CHECK(of(canonical + "/sub\\x.log") == Verdict::Refused);
    CHECK(of("/sd/\\" + std::string(name) + "/x.log") == Verdict::Refused);
    // The same aliases below the folder are refused too (a second spelling of
    // any segment).
    CHECK(of(canonical + "/SYS_LO~1/x.log") == Verdict::Refused);
    CHECK(of(canonical + "/sys_logs./x.log") == Verdict::Refused);
    CHECK(of(canonical + "/sys_logs /x.log") == Verdict::Refused);
    CHECK(of(canonical + "/x.log.") == Verdict::Refused);
    CHECK(of(canonical + "/x.log ") == Verdict::Refused);
  }
  CHECK(folders == 7);

  // The design's four examples.
  CHECK(of("/sd/System/sys_logs/failed_login.log") == Verdict::Refused);
  CHECK(of("/sd/system\\sys_logs\\failed_login.log") == Verdict::Refused);
  CHECK(of("/sd/system./sys_logs/failed_login.log") == Verdict::Refused);
  CHECK(of("/sd/LOGGIN~1/sensors/a.csv") == Verdict::Refused);

  // The alias shape: a base of at most 8 bytes ending in '~' + hex digits
  // (FatFs writes a hash in hex after five collisions), an extension of at
  // most 3 bytes, at most one dot.
  for (const char* segment : {"LOGGIN~1", "LOGGIN~5", "LO3F2A~1", "LOGG~A3F", "SYSTEM~1", "A~1", "loggin~1",
                              "EVENTS~1.LOG", "X~1.A", "FAILED~2.LOG", "a~b", "~1"}) {
    CHECK(sdpath::shortNameAlias(segment, std::strlen(segment)));
    CHECK(of(std::string("/sd/") + segment) == Verdict::Refused);
    CHECK(of(std::string("/sd/notes/") + segment) == Verdict::Refused);
  }
  // Not the shape: a base over 8 bytes, no hex digit after the '~', text
  // after the digits, an extension over 3 bytes, a second dot.
  for (const char* segment : {"LOGGING~1", "a~", "~", "x~1.backup", "x~1.a.b", "name~1x", "tilde~g",
                              "plain", "a.b", "a~z"}) {
    CHECK(!sdpath::shortNameAlias(segment, std::strlen(segment)));
    CHECK(of(std::string("/sd/notes/") + segment) == Verdict::Accepted);
  }

  // Bytes of 0x80 and above. The build mounts the card with code page 437 and
  // ANSI/OEM paths: FatFs up-cases such a byte through the code page's table
  // for the short name, and several land on plain ASCII (0x82 -> E, 0x98 -> Y,
  // 0x93 0x95 0xA2 -> O, 0x83 0x85 0xA0 -> A, 0x88-0x8A -> E, 0x8B-0x8D 0xA1 ->
  // I, 0x96 0x97 0xA3 -> U), so "syst\x82m" opens the folder "system" by its
  // short name while the rules see other bytes. No card path carries one, in
  // any segment, protected folder or not.
  CHECK(of("/sd/syst\x82m/sys_logs/failed_login.log") == Verdict::Refused);
  CHECK(of("/sd/s\x98stem/sys_logs/failed_login.log") == Verdict::Refused);
  CHECK(of("/sd/c\x93llect/aabbccddeeff/events.log") == Verdict::Refused);
  CHECK(of("/sd/c\x95llect/x") == Verdict::Refused);
  CHECK(of("/sd/c\xa2llect/x") == Verdict::Refused);
  CHECK(of("/sd/\x82spnow/cap.bin") == Verdict::Refused);
  CHECK(of("/sd/espn\x93w/cap.bin") == Verdict::Refused);
  CHECK(of("/sd/system/s\x98s_logs/failed_login.log") == Verdict::Refused);
  CHECK(of("/sd/system/sys_logs/fail\x82" "d_login.log") == Verdict::Refused);
  for (const char* name : sdpath::kProtectedFolders) {
    // Every protected folder with each of its letters replaced by every byte
    // from 0x80 up: whatever a code page folds it to, it is not a card path.
    const std::string folder = name;
    for (size_t at = 0; at < folder.size(); ++at) {
      for (int byte = 0x80; byte <= 0xff; ++byte) {
        std::string spelled = folder;
        spelled[at] = char(byte);
        CHECK(of("/sd/" + spelled) == Verdict::Refused);
        CHECK(of("/sd/" + spelled + "/x.log") == Verdict::Refused);
      }
    }
  }
  for (int byte = 0x80; byte <= 0xff; ++byte) {
    const std::string c(1, char(byte));
    CHECK(!sdpath::segmentAccepted((std::string("a") + c).c_str(), 2));
    CHECK(of("/sd/notes/caf" + c + ".txt") == Verdict::Refused);     // an unprotected folder too
    CHECK(of("/sd/" + c) == Verdict::Refused);
    CHECK(of("/sd/stt/u7/" + c + "a.txt") == Verdict::Refused);
    CHECK(of("/notes/caf" + c + ".txt") == Verdict::NotCard);        // flash compares bytes: untouched
  }
  CHECK(of("/sd/Music/Caf\xc3\xa9.mp3") == Verdict::Refused);  // UTF-8 is such bytes too

  // Unprotected folders keep their own case (the names are not folded), but a
  // second spelling of a segment is refused there as well.
  CHECK(of("/sd/Videos/a.avi") == Verdict::Accepted);
  CHECK(of("/sd/VIDEOS./a.avi") == Verdict::Refused);
  CHECK(of("/sd/recordings/take.wav ") == Verdict::Refused);
  CHECK(of("/sd/recordings\\take.wav") == Verdict::Refused);

  std::fprintf(stderr, "checks=%d\n", checks);
  return 0;
}
