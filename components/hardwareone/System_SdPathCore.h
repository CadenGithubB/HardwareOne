// System_SdPathCore.h - which spellings of a card path the guarded VFS takes
// (B11, 2026-11-22). Pure: no Arduino, no ESP-IDF, no allocation, so the host
// tests run it as it is (test/host/test_sd_path_core.cpp).
//
// The path rules (System_Filesystem.cpp, sPathRules) and the capability
// scopes compare bytes. FatFs does not: it ignores ASCII case, takes '\' as a
// separator, drops trailing dots and spaces from every path segment and also
// answers to the 8.3 alias of a long name ("LOGGIN~1" for "logging_captures").
// A spelling the rules do not recognise fell to the catch-all row (accounts
// below admin read, create and upload) while the card opened the real file,
// so "/sd/System/sys_logs/failed_login.log" showed a user the login log.
//
// The rule here: for a path on the card ("/sd/..."), every segment has one
// accepted spelling, and the first-level folders the rules protect are
// accepted in their canonical spelling only. Anything else is refused where
// ".." is refused (normalizeFsPath), before any rule is looked up, so it can
// neither read a protected file nor plant one under an aliased folder.
//
// LittleFS compares bytes, like the rules do: a path that is not on the card
// is never refused here.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace sdpath {

inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }

// First-level card folders with a rule row, a private-tree policy or a system
// writer of their own: the card twins of /system and /logging_captures, the
// transcript and note trees, what a collector pulled, the ESP-NOW captures and
// the icon packs. test_sd_path_core.py fails when sPathRules names a "/sd/..."
// folder that is missing here.
inline constexpr const char* kProtectedFolders[] = {
    "system", "logging_captures", "stt", "g2", "collect", "espnow", "g2_icon_animations",
};

inline bool hexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// The shape of a FAT 8.3 alias: a base of at most 8 bytes that ends in '~'
// and one or more hexadecimal digits (FatFs numbers a collision ~1 to ~5 and
// then writes a hash in hex), with at most one dot and an extension of at most
// 3 bytes. A long name of that shape is refused too: nothing the firmware
// writes has one.
inline bool shortNameAlias(const char* segment, size_t length) {
  const char* dot = static_cast<const char*>(memchr(segment, '.', length));
  const size_t base = dot ? static_cast<size_t>(dot - segment) : length;
  if (base < 2 || base > 8) return false;
  if (dot) {
    const size_t extension = length - base - 1;
    if (extension > 3 || memchr(dot + 1, '.', extension)) return false;
  }
  size_t digits = 0;
  while (digits < base && hexDigit(segment[base - 1 - digits])) ++digits;
  return digits > 0 && digits < base && segment[base - 1 - digits] == '~';
}

// One segment of a card path as FatFs would resolve it: false for a segment
// with a second spelling (a backslash, a trailing dot or space, an 8.3 alias).
inline bool segmentAccepted(const char* segment, size_t length) {
  if (length == 0) return false;
  if (memchr(segment, '\\', length)) return false;
  const char last = segment[length - 1];
  if (last == '.' || last == ' ') return false;
  return !shortNameAlias(segment, length);
}

// True when `segment` names a protected folder in any case; `canonical` then
// says whether it is the one accepted spelling.
inline bool protectedFolder(const char* segment, size_t length, bool& canonical) {
  for (const char* name : kProtectedFolders) {
    if (strlen(name) != length) continue;
    size_t i = 0;
    while (i < length && lower(segment[i]) == name[i]) ++i;
    if (i != length) continue;
    canonical = memcmp(segment, name, length) == 0;
    return true;
  }
  canonical = true;
  return false;
}

enum class Verdict : uint8_t {
  NotCard,   // not under /sd: LittleFS, compared byte for byte
  Accepted,  // on the card, in the one spelling the rules know
  Refused,   // on the card, in a spelling FatFs would resolve to another name
};

// `path` is a normalized path (normalizeFsPath: leading '/', no "//", no "."
// or ".." segment, no trailing '/'). A '\' anywhere after "/sd" is refused,
// including directly after it ("/sd\system" is the card to FatFs once the
// mount prefix is stripped; to the VFS router it is a LittleFS name, which
// LittleFS stores literally, so it is left alone as NotCard).
inline Verdict classify(const char* path) {
  if (!path || strncmp(path, "/sd", 3) != 0) return Verdict::NotCard;
  if (path[3] == '\0') return Verdict::Accepted;  // the card's root
  if (path[3] != '/') return Verdict::NotCard;    // "/sdcard", "/sd.x": a LittleFS name
  const char* segment = path + 4;
  bool first = true;
  while (*segment) {
    const char* end = strchr(segment, '/');
    const size_t length = end ? static_cast<size_t>(end - segment) : strlen(segment);
    if (!segmentAccepted(segment, length)) return Verdict::Refused;
    if (first) {
      bool canonical = true;
      if (protectedFolder(segment, length, canonical) && !canonical) return Verdict::Refused;
      first = false;
    }
    if (!end) break;
    segment = end + 1;
  }
  return Verdict::Accepted;
}

}  // namespace sdpath
