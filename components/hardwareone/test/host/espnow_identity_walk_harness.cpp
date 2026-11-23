// Host harness for the boot-time peer identity walk of System_ESPNow_Identity.cpp.
//
// test_espnow_identity_walk.py pastes production text in at the INSERT tags:
// the PeerIdentity record and event bits from the header, and from the source
// hexToBytes, parseMacNoSep, findSlotByMac, buildPeerPath, readPeerIdentityFile,
// peerIdentitiesLoaded, peerIdentityForget, peerIdentityLoadAll, peerIdentityAt
// and peerIdentitySlotCount. The filesystem is the shared fake (fake_vfs.h),
// ArduinoJson is the vendored library. Doubles here: logging macros, the
// filesystem lock guard, filesystemReady, EXT_RAM_BSS_ATTR, PSRAM_JSON_DOC and
// Settings::N_MESHES (the value from System_Settings.h).
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#include <Arduino.h>  // web_batch_stubs String double, which fake_vfs.h needs
#include "fake_vfs.h"
#include <ArduinoJson.h>
#include "psram_json_doc_host.h"  // PSRAM_JSON_DOC
#include "System_MacText.h"       // production MAC text helpers (copied beside this unit)

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

// ---- Logging ---------------------------------------------------------------
struct LogLine {
  char level;
  std::string text;
};
static std::vector<LogLine> gLog;
__attribute__((format(printf, 2, 3)))
static void logLine(char level, const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  gLog.push_back({level, text});
}
#define ERROR_ESPNOWF(...) logLine('E', __VA_ARGS__)
#define WARN_ESPNOWF(...) logLine('W', __VA_ARGS__)
#define INFO_ESPNOWF(...) logLine('I', __VA_ARGS__)
#define DEBUG_ESPNOWF(...) logLine('D', __VA_ARGS__)

// ---- Production dependencies ----------------------------------------------
// The mesh slot count, from System_Settings.h through the runner.
struct Settings {
  static constexpr uint8_t N_MESHES = HW1_N_MESHES;
};
#define EXT_RAM_BSS_ATTR
static bool filesystemReady = true;

static int gFsLockDepth = 0;
struct FsLockGuard {
  explicit FsLockGuard(const char*) { ++gFsLockDepth; }
  ~FsLockGuard() { --gFsLockDepth; }
};

// ---- Production code -------------------------------------------------------
// INSERT_TYPES
namespace {
// kPeerSlots, kPeersDir, kPeerFileVersion, gPeerIdentities and
// gPeerIdentitiesLoaded are not brace blocks: the runner takes those lines
// from the source with a regex and puts them here.
// INSERT_GLOBALS
// INSERT_PRODUCTION
#if !HW1_HAVE_IDENTITIES_LOADED
// Trees before the flag had no way to say the cache was incomplete: every
// caller took it as complete. The checks that need "not read" fail on them.
bool peerIdentitiesLoaded() { return true; }
#endif
}  // namespace

// ---- Checks ----------------------------------------------------------------
static int gChecks = 0, gFailures = 0;
static void check(bool ok, const char* what) {
  ++gChecks;
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++gFailures;
}

// ---- Fixtures --------------------------------------------------------------
using Mac = std::array<uint8_t, 6>;
static const Mac kA = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static const Mac kB = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};  // sorts between kA and kC
static const Mac kC = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
static const Mac kElsewhere = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};  // never a folder
static const uint32_t kBondedAt = 1762560000u;

static std::string macText(const Mac& m, const char* format) {
  char text[18];
  std::snprintf(text, sizeof(text), format, m[0], m[1], m[2], m[3], m[4], m[5]);
  return text;
}
static std::string token(const Mac& m) { return macText(m, "%02X%02X%02X%02X%02X%02X"); }
static std::string display(const Mac& m) { return macText(m, "%02X:%02X:%02X:%02X:%02X:%02X"); }
static std::string folder(const Mac& m) { return "/system/espnow/peers/" + token(m); }
static std::string jsonPath(const Mac& m) { return folder(m) + "/identity.json"; }
static std::string tmpPath(const Mac& m) { return folder(m) + "/identity.tmp"; }

// The file a paired device leaves behind; the key bytes count up from mac[5].
static std::string identityText(const Mac& inside, unsigned meshId) {
  char pub[65];
  for (unsigned i = 0; i < 32; ++i) std::snprintf(pub + i * 2, 3, "%02x", (inside[5] + i) & 0xFFu);
  char text[320];
  std::snprintf(text, sizeof(text),
                "{\"version\":2,\"mac\":\"%s\",\"meshId\":%u,\"longTermPubEd25519_hex\":\"%s\","
                "\"bondedAtSec\":%lu,\"lastSeenSec\":%lu,\"subscribedEvents\":5}",
                display(inside).c_str(), meshId, pub, (unsigned long)kBondedAt, (unsigned long)kBondedAt + 60);
  return text;
}
static void store(const Mac& m, unsigned meshId) { fake_vfs::fs().put(jsonPath(m), identityText(m, meshId)); }

// The same file with the "meshId" member replaced by `member` ("" drops it).
static std::string identityWithMeshMember(const Mac& inside, const std::string& member) {
  std::string text = identityText(inside, 1);
  const std::string field = "\"meshId\":1,";
  const size_t at = text.find(field);
  return text.replace(at, field.size(), member.empty() ? std::string() : member + ",");
}

static void reset() {
  fake_vfs::Fs& fs = fake_vfs::fs();
  fs.reset();
  fs.lockHeld = [] { return gFsLockDepth > 0; };
  fs.mkdirAll("/system/espnow");
  gLog.clear();
  filesystemReady = true;
}

// The cache as the rest of the firmware sees it.
static const PeerIdentity* cached(const Mac& m) {
  for (uint8_t i = 0; i < peerIdentitySlotCount(); ++i) {
    const PeerIdentity* p = peerIdentityAt(i);
    if (p && std::memcmp(p->mac, m.data(), 6) == 0) return p;
  }
  return nullptr;
}
static unsigned cachedCount() {
  unsigned n = 0;
  for (uint8_t i = 0; i < peerIdentitySlotCount(); ++i) n += peerIdentityAt(i) != nullptr;
  return n;
}

static unsigned warnings() {
  unsigned n = 0;
  for (const LogLine& line : gLog) n += line.level == 'W';
  return n;
}
static bool warned(const std::string& a, const std::string& b) {
  for (const LogLine& line : gLog) {
    if (line.level == 'W' && line.text.find(a) != std::string::npos && line.text.find(b) != std::string::npos) return true;
  }
  return false;
}
// The warning for a stored identity that stayed out of the cache.
static bool warnedForget(const Mac& m) { return warned(display(m), "espnowforget"); }

// ---- Cases -----------------------------------------------------------------
static void neverRan() {
  check(!peerIdentitiesLoaded(), "before the first walk the identities are not marked as read");
}

static void loadsOne() {
  reset();
  store(kA, 3);
  check(peerIdentityLoadAll() == 1, "one stored identity: the walk reports 1");
  check(peerIdentitiesLoaded(), "one stored identity: marked as read");
  const PeerIdentity* p = cached(kA);
  check(p != nullptr, "one stored identity: in the cache under its address");
  check(p && p->meshId == 3, "one stored identity: cached with its mesh slot");
  check(p && p->valid && p->longTermPub[0] == kA[5] && p->longTermPub[31] == kA[5] + 31 &&
            p->bondedAtSec == kBondedAt,
        "one stored identity: cached with its key and pairing time (a leftover subscribedEvents key is ignored)");
  check(cachedCount() == 1, "one stored identity: nothing else in the cache");
  check(warnings() == 0, "one stored identity: no warning");
  check(peerIdentitySlotCount() == 16, "the cache has 16 slots");
  check(peerIdentityAt(peerIdentitySlotCount()) == nullptr && peerIdentityAt(255) == nullptr,
        "peerIdentityAt past the last slot is null");
  bool listed = false, locked = true;
  for (const fake_vfs::Call& call : fake_vfs::fs().log) {
    if (call.op != "open" && call.op != "openNext" && call.op != "read") continue;
    listed = listed || call.op == "openNext";
    locked = locked && call.locked;
  }
  check(listed && locked && gFsLockDepth == 0, "the directory and the files are read under the filesystem lock");
}

static void noPeersDirectory() {
  reset();  // /system/espnow exists, peers does not
  check(peerIdentityLoadAll() == 0, "no peers directory: the walk reports 0");
  check(peerIdentitiesLoaded(), "no peers directory: marked as read");
  check(cachedCount() == 0, "no peers directory: the cache of the previous walk is emptied");
}

static void filesystemNotReady() {
  reset();
  store(kA, 1);
  check(peerIdentityLoadAll() == 1 && peerIdentitiesLoaded(), "a walk with the filesystem ready marks the identities as read");
  // Unmounted: the flag is down and every path reads as absent.
  filesystemReady = false;
  fake_vfs::fs().littlefsReady() = false;
  check(peerIdentityLoadAll() == 0, "filesystem not ready: the walk reports 0");
  check(!peerIdentitiesLoaded(), "filesystem not ready: not marked as read, whatever the walk before said");
}

static void peersNotOpenable() {
  reset();
  store(kA, 1);
  fake_vfs::fs().faults.open.add("/system/espnow/peers");
  check(peerIdentityLoadAll() == 0, "peers directory will not open: the walk reports 0");
  check(!peerIdentitiesLoaded(), "peers directory will not open: not marked as read");

  reset();
  fake_vfs::fs().put("/system/espnow/peers", "not a directory");
  check(peerIdentityLoadAll() == 0, "peers is a file: the walk reports 0");
  check(!peerIdentitiesLoaded(), "peers is a file: not marked as read");
}

static void skippedEntries() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  store(kA, 1);
  fs.mkdirAll("/system/espnow/peers/112233445566");                                  // no identity.json
  fs.put("/system/espnow/peers/223344556677/identity.tmp", identityText(kElsewhere, 2));  // only a temp
  fs.put("/system/espnow/peers/notes/identity.json", identityText(kElsewhere, 2));        // not an address
  fs.mkdirAll("/system/espnow/peers/GGHHIIJJKKLL");                                  // 12 characters, not hex
  fs.mkdirAll("/system/espnow/peers/AABBCCDDEE7");                                   // 11 digits
  fs.mkdirAll("/system/espnow/peers/AABBCCDDEE777");                                 // 13 digits
  fs.put("/system/espnow/peers/334455667788", "a file named like an address");
  check(peerIdentityLoadAll() == 1, "folders without an identity are skipped: the walk reports the one identity");
  check(peerIdentitiesLoaded(), "folders without an identity do not leave the cache marked incomplete");
  check(cachedCount() == 1 && cached(kA) != nullptr, "folders without an identity put nothing in the cache");
  check(warnings() == 0, "folders without an identity are not warned about");

  // Each kind on its own, so one skipped kind cannot hide behind another.
  const char* const lone[] = {"/system/espnow/peers/112233445566", "/system/espnow/peers/GGHHIIJJKKLL"};
  for (const char* path : lone) {
    reset();
    fs.mkdirAll(path);
    check(peerIdentityLoadAll() == 0 && peerIdentitiesLoaded() && warnings() == 0, "a lone empty folder is skipped");
  }
  reset();
  fs.put("/system/espnow/peers/223344556677/identity.tmp", identityText(kElsewhere, 2));
  check(peerIdentityLoadAll() == 0 && peerIdentitiesLoaded() && warnings() == 0, "a lone folder with only identity.tmp is skipped");
  reset();
  fs.put("/system/espnow/peers/notes/identity.json", identityText(kElsewhere, 2));
  check(peerIdentityLoadAll() == 0 && peerIdentitiesLoaded() && warnings() == 0 && cachedCount() == 0,
        "a lone folder whose name is not an address is skipped");
}

// kB is stored but does not load; kA before it and kC after it do.
static void oneBadAmongGood(const std::string& badText, const char* what) {
  reset();
  store(kA, 1);
  fake_vfs::fs().put(jsonPath(kB), badText);
  store(kC, 2);
  std::string label = std::string(what) + ": ";
  check(peerIdentityLoadAll() == 2, (label + "the walk reports the two readable identities").c_str());
  check(!peerIdentitiesLoaded(), (label + "not marked as read").c_str());
  check(cached(kA) && cached(kA)->meshId == 1 && cached(kC) && cached(kC)->meshId == 2,
        (label + "the readable identities before and after it are cached").c_str());
  check(cachedCount() == 2 && !cached(kB) && !cached(kElsewhere), (label + "it is not in the cache").c_str());
  check(warnedForget(kB), (label + "a warning names the folder's address and espnowforget").c_str());
  check(warnings() == 1, (label + "one warning, none for the readable identities").c_str());
}

static void unreadableIdentity() {
  const std::string whole = identityText(kB, 2);
  oneBadAmongGood(whole.substr(0, whole.size() / 2), "truncated JSON");
  oneBadAmongGood("", "empty file");
  oneBadAmongGood(identityText(kElsewhere, 2), "another address inside");
}

// A meshId that is not a slot of this firmware: left out like an unreadable file.
static void meshIdOutOfRange() {
  check(Settings::N_MESHES == 4, "the settings header has 4 mesh slots (the cases below assume it)");
  oneBadAmongGood(identityText(kB, 200), "meshId 200");
  oneBadAmongGood(identityText(kB, Settings::N_MESHES), "meshId N_MESHES (first slot past the end)");
  oneBadAmongGood(identityText(kB, 300), "meshId 300 (does not fit a byte)");
  oneBadAmongGood(identityWithMeshMember(kB, "\"meshId\":-1"), "meshId -1");
  oneBadAmongGood(identityWithMeshMember(kB, "\"meshId\":\"1\""), "meshId as text");
  oneBadAmongGood(identityWithMeshMember(kB, "\"meshId\":1.5"), "meshId 1.5");
  oneBadAmongGood(identityWithMeshMember(kB, ""), "no meshId");

  reset();
  store(kA, Settings::N_MESHES - 1);
  check(peerIdentityLoadAll() == 1 && cached(kA) && cached(kA)->meshId == Settings::N_MESHES - 1 &&
            peerIdentitiesLoaded(),
        "the last mesh slot still loads");
}

static std::string lower(std::string text) {
  for (char& c : text) c = static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
  return text;
}
static std::string mixed(std::string text) {
  for (size_t i = 0; i < text.size(); i += 2) text[i] = static_cast<char>(text[i] >= 'A' && text[i] <= 'F' ? text[i] - 'A' + 'a' : text[i]);
  return text;
}

// Only the folder spelled as macToPathToken writes it is a peer folder.
static void canonicalFolders() {
  fake_vfs::Fs& fs = fake_vfs::fs();
  const std::string lowerFolder = "/system/espnow/peers/" + lower(token(kA));

  // An empty lower-case twin beside the real folder: one identity, one cache entry.
  reset();
  store(kA, 1);
  fs.mkdirAll(lowerFolder);
  check(peerIdentityLoadAll() == 1, "an empty lower-case twin folder: the walk reports 1");
  check(cachedCount() == 1 && cached(kA) && cached(kA)->meshId == 1, "an empty lower-case twin folder: one cache entry");
  check(peerIdentitiesLoaded() && warnings() == 0, "an empty lower-case twin folder: marked as read, no warning");
  peerIdentityForget(kA.data());
  check(!cached(kA) && cachedCount() == 0, "after a forget no copy of the identity stays in the cache");
  check(!fs.exists(folder(kA)), "the forget removes the real folder");

  // Mixed case is not the canonical spelling either.
  reset();
  store(kA, 1);
  fs.mkdirAll("/system/espnow/peers/" + mixed(token(kA)));
  check(peerIdentityLoadAll() == 1 && cachedCount() == 1 && peerIdentitiesLoaded() && warnings() == 0,
        "an empty mixed-case twin folder: one cache entry, marked as read");

  // A non-canonical folder with an identity of its own, alone: never loaded (no
  // command reads or removes it), named in a warning, cache still complete.
  reset();
  fs.put(lowerFolder + "/identity.json", identityText(kA, 2));
  check(peerIdentityLoadAll() == 0 && cachedCount() == 0, "a lower-case folder's own identity is not loaded");
  check(peerIdentitiesLoaded(), "a lower-case folder's own identity does not mark the cache incomplete");
  check(warnings() == 1 && warned(lower(token(kA)) + "/identity.json", "ignored"),
        "a lower-case folder's own identity: one warning naming the file");

  // Both folders hold an identity: the real one loads once, the twin is named.
  reset();
  store(kA, 1);
  fs.put(lowerFolder + "/identity.json", identityText(kA, 3));
  check(peerIdentityLoadAll() == 1 && cachedCount() == 1 && cached(kA) && cached(kA)->meshId == 1,
        "both spellings with an identity: the canonical folder's one is cached once");
  check(peerIdentitiesLoaded() && warnings() == 1, "both spellings with an identity: marked as read, the twin named");
}

static Mac numbered(unsigned i) { return Mac{0x02, 0x00, 0x00, 0x00, 0x10, static_cast<uint8_t>(i)}; }

static void fullCache() {
  reset();
  for (unsigned i = 0; i < 16; ++i) store(numbered(i), i % 4);
  check(peerIdentityLoadAll() == 16, "16 stored identities: the walk reports 16");
  check(peerIdentitiesLoaded() && cachedCount() == 16 && warnings() == 0, "16 stored identities: all cached, marked as read");

  store(numbered(16), 0);
  gLog.clear();
  check(peerIdentityLoadAll() == 16, "17 stored identities: the walk reports 16");
  check(cachedCount() == 16, "17 stored identities: 16 in the cache");
  check(!peerIdentitiesLoaded(), "17 stored identities: not marked as read");
  int missing = -1, missingCount = 0;
  for (unsigned i = 0; i < 17; ++i) {
    if (cached(numbered(i))) continue;
    missing = static_cast<int>(i);
    ++missingCount;
  }
  check(missingCount == 1, "17 stored identities: exactly one is left out");
  check(warnings() == 1 && missing >= 0 && warnedForget(numbered(static_cast<unsigned>(missing))),
        "17 stored identities: one warning, naming the address that is left out and espnowforget");
}

static void forget() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  store(kA, 1);
  fs.put(tmpPath(kA), "{\"version\":2");
  store(kC, 2);
  check(peerIdentityLoadAll() == 2 && peerIdentitiesLoaded(), "two stored identities are read before the forget");
  check(peerIdentityForget(kA.data()), "forget reports success");
  check(!fs.exists(jsonPath(kA)), "forget removes identity.json");
  check(!fs.exists(tmpPath(kA)), "forget removes identity.tmp");
  check(!fs.exists(folder(kA)), "forget removes the folder");
  check(!cached(kA), "forget clears the cache slot");
  check(cached(kC) && cachedCount() == 1 && fs.exists(jsonPath(kC)), "forget leaves the other identity alone");
  check(peerIdentitiesLoaded() && warnings() == 0, "a clean forget keeps the identities marked as read");
}

static void forgetCannotRemove() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  store(kA, 1);
  store(kC, 2);
  check(peerIdentityLoadAll() == 2 && peerIdentitiesLoaded(), "two stored identities are read before the failing forget");
  fs.faults.remove.add(jsonPath(kA));
  peerIdentityForget(kA.data());
  check(fs.exists(jsonPath(kA)) && !peerIdentitiesLoaded(), "an identity.json that stays after forget: no longer marked as read");
  check(warned(token(kA), "identity.json"), "an identity.json that stays after forget: a warning names the file");
  fs.faults.remove.clear();
  check(peerIdentityLoadAll() == 2 && cached(kA) && peerIdentitiesLoaded(), "the file that stayed is loaded again by the next walk");
}

static void rereadAfterRepair() {
  reset();
  store(kA, 1);
  fake_vfs::fs().put(jsonPath(kB), "{\"version\":2,\"mac\":");
  check(peerIdentityLoadAll() == 1 && !peerIdentitiesLoaded(), "a bad file leaves the identities not marked as read");
  peerIdentityForget(kB.data());  // the command the warning names
  check(!fake_vfs::fs().exists(folder(kB)), "forget removes a stored identity that never reached the cache");
  check(peerIdentityLoadAll() == 1 && cached(kA), "after the bad file is gone the readable identity still loads");
  check(peerIdentitiesLoaded(), "after the bad file is gone the next walk marks the identities as read");
}

int main() {
  neverRan();
  loadsOne();
  noPeersDirectory();
  filesystemNotReady();
  peersNotOpenable();
  skippedEntries();
  unreadableIdentity();
  meshIdOutOfRange();
  canonicalFolders();
  fullCache();
  forget();
  forgetCannotRemove();
  rereadAfterRepair();
  if (gFailures) {
    std::fprintf(stderr, "espnow_identity_walk: %d of %d checks failed\n", gFailures, gChecks);
    return 1;
  }
  std::printf("espnow_identity_walk: %d checks passed\n", gChecks);
  return 0;
}
