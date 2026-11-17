// Host harness for what 'espnowunpair' removes of the files kept about a peer.
//
// test_espnow_peer_files.py pastes production text in at the INSERT tags:
// peerCacheDir and peerCachePath from System_Utils.h; the PeerIdentity and
// PeerFilesRemoved records from System_ESPNow_Identity.h; from
// System_ESPNow_Identity.cpp findSlotByMac, buildPeerPath, peerIdentityForget,
// removeFolderContents and peerFilesRemove; from System_ESPNow.cpp the manifest
// reference (manifestHashToHex, manifestRefReadPath, manifestRefRead,
// manifestHashInUse, manifestDropIfUnused, manifestCacheRecordUse,
// manifestCachedFor, manifestCacheHit) and the reply line (appendUnpairText, formatStoredSize, formatUnpairFilesLine). The
// filesystem is the shared fake (fake_vfs.h). Doubles here: logging macros,
// the filesystem lock guard, filesystemReady and EXT_RAM_BSS_ATTR.
//
// A tree without one of the three pieces compiles with a stand-in for it that
// does what that tree's unpair did (forget the identity only, keep every
// manifest, no line about files), and fails the checks that need the piece.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#include <Arduino.h>  // web_batch_stubs String double, which fake_vfs.h needs
#include "fake_vfs.h"
#include "System_MacText.h"  // production MAC text helpers (copied beside this unit)

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
#define EXT_RAM_BSS_ATTR
static bool filesystemReady = true;

static int gFsLockDepth = 0;
struct FsLockGuard {
  explicit FsLockGuard(const char*) { ++gFsLockDepth; }
  ~FsLockGuard() { --gFsLockDepth; }
};

// ---- Production code -------------------------------------------------------
// INSERT_UTILS
// INSERT_TYPES
#if !HW1_HAVE_PEER_FILES
struct PeerFilesRemoved {
  uint16_t files;
  uint32_t bytes;
  uint8_t failed;
  bool folderLeft;
};
#endif

// gPeerIdentitiesLoaded is file-scope static in the source; the runner puts it
// here with the other lines that are not brace blocks.
// INSERT_GLOBALS
namespace {
// INSERT_IDENTITY
}  // namespace

#if HW1_HAVE_PEER_FILES
// INSERT_PEER_FILES
#else
// Before the change espnowunpair forgot the identity and nothing else.
bool peerFilesRemove(const uint8_t mac[6], PeerFilesRemoved& out) {
  out = PeerFilesRemoved{};
  peerIdentityForget(mac);
  return true;
}
#endif

#if HW1_HAVE_MANIFEST_REF
// INSERT_MANIFEST
#else
// Before the change no peer recorded its manifest, and none was removed.
static void manifestHashToHex(const uint8_t fwHash[16], char hex[33]) {
  for (int i = 0; i < 16; i++) std::snprintf(hex + (i * 2), 3, "%02x", fwHash[i]);
  hex[32] = '\0';
}
static bool manifestRefRead(const uint8_t*, char hex[33]) {
  hex[0] = '\0';
  return false;
}
static uint32_t manifestDropIfUnused(const char*) { return 0; }
static void manifestCacheRecordUse(const uint8_t*, const uint8_t*) {}
// The bond sync found a cached manifest and recorded nothing.
static bool manifestCacheHit(const uint8_t*, const uint8_t* fwHash) {
  char hex[33];
  manifestHashToHex(fwHash, hex);
  const std::string path = std::string("/system/manifests/") + hex + ".json";
  const std::string text = fake_vfs::fs().exists(path) ? fake_vfs::fs().text(path) : std::string();
  return text.size() >= 2 && text.front() == '{' && text.back() == '}';
}
#endif

#if HW1_HAVE_FILES_LINE
// INSERT_FILES_LINE
#else
// Before the change the unpair reply said nothing about files.
static void formatUnpairFilesLine(char* out, size_t outLen, bool, const PeerFilesRemoved&, uint32_t,
                                  const char*, const char*) {
  if (out && outLen) out[0] = '\0';
}
#endif

// ---- Checks ----------------------------------------------------------------
static int gChecks = 0, gFailures = 0;
static void check(bool ok, const std::string& what) {
  ++gChecks;
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  ++gFailures;
}

// ---- Fixtures --------------------------------------------------------------
using Mac = std::array<uint8_t, 6>;
static const Mac kPartner = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static const Mac kOther = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
static const Mac kThird = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};

static std::string token(const Mac& m) {
  char text[13];
  std::snprintf(text, sizeof(text), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return text;
}
static std::string folder(const Mac& m) { return "/system/espnow/peers/" + token(m); }
static std::string inFolder(const Mac& m, const char* leaf) { return folder(m) + "/" + leaf; }
static std::string receivedFolder(const Mac& m) { return "/espnow/received/" + token(m); }

using Hash = std::array<uint8_t, 16>;
static Hash hashOf(uint8_t first) {
  Hash h{};
  for (unsigned i = 0; i < 16; ++i) h[i] = static_cast<uint8_t>(first + i);
  return h;
}
static std::string hexOf(const Hash& h) {
  char hex[33];
  manifestHashToHex(h.data(), hex);
  return hex;
}
static std::string manifestPath(const Hash& h) { return "/system/manifests/" + hexOf(h) + ".json"; }

static void reset() {
  fake_vfs::Fs& fs = fake_vfs::fs();
  fs.reset();
  fs.lockHeld = [] { return gFsLockDepth > 0; };
  fs.mkdirAll("/system/espnow/peers");
  fs.mkdirAll("/system/manifests");
  gLog.clear();
  filesystemReady = true;
  for (PeerIdentity& p : gPeerIdentities) p = PeerIdentity{};
  gPeerIdentitiesLoaded = true;
}

// A bonded peer's folder: its identity (cached too) and the settings and
// schema caches.
static void storeFolder(const Mac& m) {
  fake_vfs::Fs& fs = fake_vfs::fs();
  fs.put(inFolder(m, "identity.json"), "{\"version\":2}");
  fs.put(inFolder(m, "settings.json"), std::string(7049, 's'));
  fs.put(inFolder(m, "schema.json"), std::string(1200, 'c'));
  for (PeerIdentity& p : gPeerIdentities) {
    if (p.valid) continue;
    std::memcpy(p.mac, m.data(), 6);
    p.valid = true;
    break;
  }
}

// What the bond sync leaves for a partner that sent its manifest: the folder,
// the manifest and the reference to it.
static void storePartner(const Mac& m, const Hash& h, size_t manifestBytes) {
  storeFolder(m);
  fake_vfs::fs().put(manifestPath(h), "{" + std::string(manifestBytes - 2, 'm') + "}");
  manifestCacheRecordUse(m.data(), h.data());
}

static bool cached(const Mac& m) {
  for (const PeerIdentity& p : gPeerIdentities) {
    if (p.valid && std::memcmp(p.mac, m.data(), 6) == 0) return true;
  }
  return false;
}

static unsigned warnings() {
  unsigned n = 0;
  for (const LogLine& line : gLog) n += line.level == 'W';
  return n;
}

// The unpair command's file steps, in its order (cmd_espnow_unpair). The
// identity flag is what espnowForgetPeerCrypto hands the reply: the cache
// slot or the file (its session part needs the radio and is not run here).
struct Unpaired {
  bool identity;
  PeerFilesRemoved files;
  uint32_t manifestBytes;
  bool ok;
};
static Unpaired unpairFiles(const Mac& m) {
  Unpaired u{};
  u.identity = cached(m);
  u.identity = peerIdentityForget(m.data()) || u.identity;
  char hex[33];
  const bool used = manifestRefRead(m.data(), hex);
  u.ok = peerFilesRemove(m.data(), u.files);
  if (used) u.manifestBytes = manifestDropIfUnused(hex);
  return u;
}

// ---- Cases: the peer folder ------------------------------------------------
static void unpairRemovesEverything() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  const Hash h = hashOf(0x10);
  storePartner(kPartner, h, 61903);
  fs.put(inFolder(kPartner, "identity.tmp"), "{\"vers");
  fs.put(inFolder(kPartner, "extra/note.txt"), "kept by a later feature");
  fs.put(receivedFolder(kPartner) + "/automations.json", "{\"automations\":[]}");
  fs.put(receivedFolder(kPartner) + "/photo.jpg", "jpeg");
  storePartner(kOther, hashOf(0x40), 500);

  check(fs.text(inFolder(kPartner, "manifest.hash")) == hexOf(h),
        "the bond sync records the partner's manifest hash in its folder");
  // Nothing is removed while a directory listing is open.
  unsigned removals = 0, removalsWithOpenHandle = 0;
  fs.onCall = [&](const fake_vfs::Call& call) {
    if (call.op != "remove" && call.op != "rmdir") return;
    ++removals;
    if (fs.liveHandles() != 0) ++removalsWithOpenHandle;
  };
  const Unpaired u = unpairFiles(kPartner);
  fs.onCall = nullptr;
  check(removals > 0 && removalsWithOpenHandle == 0, "unpair: no file or folder is removed while a listing is open");
  check(u.ok, "unpair: the peer folder is removed");
  check(!fs.exists(folder(kPartner)), "unpair: no folder left for the peer");
  check(!fs.exists(inFolder(kPartner, "settings.json")) && !fs.exists(inFolder(kPartner, "schema.json")),
        "unpair: the bond caches settings.json and schema.json are removed");
  check(!fs.exists(inFolder(kPartner, "identity.json")) && !fs.exists(inFolder(kPartner, "identity.tmp")),
        "unpair: the identity files are removed");
  check(!cached(kPartner), "unpair: the identity leaves the cache");
  check(u.files.files == 4 && u.files.bytes == 7049 + 1200 + 32 + 23,
        "unpair: the files after the identity are counted with their sizes (settings, schema, "
        "manifest.hash, the subfolder's file)");
  check(!u.files.folderLeft && u.files.failed == 0, "unpair: nothing reported as left");
  check(!fs.exists(manifestPath(h)), "unpair: the partner's cached manifest is removed");
  check(u.manifestBytes == 61903, "unpair: the removed manifest's size is reported");
  check(fs.exists(receivedFolder(kPartner) + "/automations.json") && fs.exists(receivedFolder(kPartner) + "/photo.jpg"),
        "unpair: files received from the peer stay (they are the user's)");
  check(fs.exists(inFolder(kOther, "settings.json")) && fs.exists(manifestPath(hashOf(0x40))) && cached(kOther),
        "unpair: another peer's folder, manifest and identity stay");
  check(gPeerIdentitiesLoaded && warnings() == 0, "unpair: a clean removal leaves the cache marked as read, no warning");
  bool listedLocked = true, listed = false;
  for (const fake_vfs::Call& call : fs.log) {
    if (call.op != "openNext") continue;
    listed = true;
    listedLocked = listedLocked && call.locked;
  }
  check(listed && listedLocked && gFsLockDepth == 0, "unpair: the folders are listed under the filesystem lock");
}

static void manyFiles() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  for (int i = 0; i < 11; ++i) fs.put(inFolder(kPartner, ("f" + std::to_string(i) + ".json").c_str()), "x");
  PeerFilesRemoved r{};
  check(peerFilesRemove(kPartner.data(), r), "eleven files: the folder is removed");
  check(r.files == 11 && r.bytes == 11 && !fs.exists(folder(kPartner)), "eleven files: all counted and gone");
}

static void nothingStored() {
  reset();
  PeerFilesRemoved r{};
  r.files = 9;
  check(peerFilesRemove(kThird.data(), r), "no folder: reports success");
  check(r.files == 0 && r.bytes == 0 && !r.folderLeft && r.failed == 0, "no folder: nothing counted");

  filesystemReady = false;
  fake_vfs::fs().put(inFolder(kThird, "settings.json"), "{}");
  check(!peerFilesRemove(kThird.data(), r), "filesystem not ready: reports failure");
  filesystemReady = true;
}

static void fileThatStays() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  storePartner(kPartner, hashOf(0x10), 100);
  fs.faults.remove.add(inFolder(kPartner, "schema.json"));
  PeerFilesRemoved r{};
  check(!peerFilesRemove(kPartner.data(), r), "a file that will not go: reports failure");
  check(r.folderLeft && r.failed == 1 && fs.exists(inFolder(kPartner, "schema.json")),
        "a file that will not go: the folder is reported as left, one entry failed");
  check(!fs.exists(inFolder(kPartner, "settings.json")) && !fs.exists(inFolder(kPartner, "manifest.hash")) && r.files == 2,
        "a file that will not go: the others are still removed");
  check(warnings() == 1 && gLog.back().text.find(folder(kPartner)) != std::string::npos,
        "a file that will not go: one warning naming the folder");

  reset();
  storePartner(kPartner, hashOf(0x10), 100);
  fs.faults.remove.add(inFolder(kPartner, "identity.json"));
  peerFilesRemove(kPartner.data(), r);
  check(fs.exists(inFolder(kPartner, "identity.json")) && !gPeerIdentitiesLoaded,
        "an identity.json that will not go leaves the cache marked incomplete");
}

// ---- Cases: the manifest cache ---------------------------------------------
static void manifestFollowsFirmware() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  const Hash h1 = hashOf(0x10), h2 = hashOf(0x20);
  fs.put(manifestPath(h1), "{1}");
  manifestCacheRecordUse(kPartner.data(), h1.data());
  check(fs.text(inFolder(kPartner, "manifest.hash")) == hexOf(h1), "a first manifest: recorded in a new peer folder");
  check(fs.exists(manifestPath(h1)), "a first manifest: kept");

  fs.put(manifestPath(h2), "{2}");
  manifestCacheRecordUse(kPartner.data(), h2.data());
  check(fs.text(inFolder(kPartner, "manifest.hash")) == hexOf(h2), "new firmware: the reference names the new manifest");
  check(!fs.exists(manifestPath(h1)) && fs.exists(manifestPath(h2)), "new firmware: the old manifest goes, the new one stays");

  manifestCacheRecordUse(kPartner.data(), h2.data());
  check(fs.exists(manifestPath(h2)) && fs.text(inFolder(kPartner, "manifest.hash")) == hexOf(h2),
        "the same firmware again: nothing removed");
  check(gFsLockDepth == 0, "the manifest steps release the filesystem lock");
}

static void sharedManifest() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  const Hash h = hashOf(0x10);
  storePartner(kPartner, h, 300);
  storePartner(kOther, h, 300);   // same firmware, one file
  Unpaired u = unpairFiles(kPartner);
  check(fs.exists(manifestPath(h)) && u.manifestBytes == 0,
        "two peers on one firmware: unpairing one keeps the manifest the other names");
  u = unpairFiles(kOther);
  check(!fs.exists(manifestPath(h)) && u.manifestBytes == 300, "unpairing the second removes it");

  // The other peer moves to new firmware while the first still names the old one.
  reset();
  storePartner(kPartner, h, 300);
  storePartner(kOther, h, 300);
  fs.put(manifestPath(hashOf(0x30)), "{3}");
  manifestCacheRecordUse(kOther.data(), hashOf(0x30).data());
  check(fs.exists(manifestPath(h)), "a peer moving to new firmware keeps the manifest another peer names");
}

static void unreadablePeers() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  const Hash h = hashOf(0x10);
  storePartner(kPartner, h, 300);
  char hex[33];
  check(manifestRefRead(kPartner.data(), hex) && hexOf(h) == hex, "the recorded hash reads back");
  fs.faults.open.add("/system/espnow/peers");
  check(manifestDropIfUnused(hex) == 0 && fs.exists(manifestPath(h)),
        "peer folders that cannot be listed: the manifest is kept");
  fs.faults.open.clear();

  // No peers folder at all: nobody names it.
  reset();
  fs.reset();
  fs.lockHeld = [] { return gFsLockDepth > 0; };
  fs.put(manifestPath(h), "{x}");
  check(manifestDropIfUnused(hexOf(h).c_str()) == 3 && !fs.exists(manifestPath(h)),
        "no peer folders: an unnamed manifest is removed");
  check(manifestDropIfUnused("not-a-hash") == 0, "a hash that is not 32 characters is refused");
}

static void badReferences() {
  const char* const bad[] = {"", "0123", "0123456789ABCDEF0123456789ABCDEF", "0123456789abcdef0123456789abcdeg",
                             "0123456789abcdef0123456789abcdef\n"};
  for (const char* text : bad) {
    reset();
    fake_vfs::fs().put(inFolder(kPartner, "manifest.hash"), text);
    char hex[33] = "unchanged";
    check(!manifestRefRead(kPartner.data(), hex) && hex[0] == '\0',
          std::string("a reference that is not 32 lowercase hex digits is not read: '") + text + "'");
  }
}

static void referenceWriteFails() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  const Hash h1 = hashOf(0x10), h2 = hashOf(0x20);
  storePartner(kPartner, h1, 100);
  fs.put(manifestPath(h2), "{2}");
  fs.faults.open.add(inFolder(kPartner, "manifest.hash"));
  gLog.clear();
  manifestCacheRecordUse(kPartner.data(), h2.data());
  fs.faults.open.clear();
  check(fs.exists(manifestPath(h1)) && fs.text(inFolder(kPartner, "manifest.hash")) == hexOf(h1),
        "a reference that cannot be written: the manifest the old one names is kept");
  check(!gLog.empty() && gLog.back().level == 'E', "a reference that cannot be written is logged as an error");
}

// A manifest the bond sync finds in the cache is recorded for that peer too.
static void cacheHit() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  const Hash h = hashOf(0x10);
  storePartner(kPartner, h, 300);
  storeFolder(kOther);   // same firmware: the sync finds the manifest cached
  check(manifestCacheHit(kOther.data(), h.data()), "cache hit: the cached manifest is found");
  check(fs.text(inFolder(kOther, "manifest.hash")) == hexOf(h), "cache hit: the peer records the manifest it uses");
  Unpaired u = unpairFiles(kPartner);
  check(fs.exists(manifestPath(h)) && u.manifestBytes == 0,
        "cache hit: unpairing the peer that sent the manifest keeps it for the peer that found it cached");
  u = unpairFiles(kOther);
  check(!fs.exists(manifestPath(h)) && u.manifestBytes == 300, "cache hit: unpairing the second peer removes it");

  // Cached before references existed: the first sync after the update names it.
  reset();
  storeFolder(kPartner);
  fs.put(manifestPath(h), "{" + std::string(61901, 'm') + "}");
  check(manifestCacheHit(kPartner.data(), h.data()) && fs.text(inFolder(kPartner, "manifest.hash")) == hexOf(h),
        "an older cached manifest: the sync records it for the partner");
  u = unpairFiles(kPartner);
  check(!fs.exists(manifestPath(h)) && u.manifestBytes == 61903,
        "an older cached manifest: unpairing the partner removes it");

  // A peer moving to firmware whose manifest is cached already drops its old one.
  reset();
  const Hash h2 = hashOf(0x20);
  storePartner(kPartner, h, 300);
  storePartner(kOther, h2, 400);
  check(manifestCacheHit(kPartner.data(), h2.data()), "new firmware already cached: found");
  check(fs.text(inFolder(kPartner, "manifest.hash")) == hexOf(h2) && !fs.exists(manifestPath(h)) &&
            fs.exists(manifestPath(h2)),
        "new firmware already cached: the reference follows and the old manifest goes");

  // A miss or an incomplete file records nothing.
  reset();
  storeFolder(kPartner);
  check(!manifestCacheHit(kPartner.data(), h.data()) && !fs.exists(inFolder(kPartner, "manifest.hash")),
        "no cached manifest: nothing recorded");
  fs.put(manifestPath(h), "{\"cut");
  check(!manifestCacheHit(kPartner.data(), h.data()) && !fs.exists(inFolder(kPartner, "manifest.hash")),
        "an incomplete cached manifest: not a hit, nothing recorded");
  check(gFsLockDepth == 0, "the cache lookup releases the filesystem lock");
}

// The reply's "crypto identity" covers an identity file the cache did not hold.
static void identityReported() {
  reset();
  fake_vfs::Fs& fs = fake_vfs::fs();
  fs.put(inFolder(kThird, "identity.json"), "{\"version\":2,\"meshId\":200}");   // left out at boot
  gPeerIdentitiesLoaded = false;
  Unpaired u = unpairFiles(kThird);
  check(u.identity && u.files.files == 0 && !fs.exists(folder(kThird)),
        "an identity file outside the cache: reported as removed");
  reset();
  u = unpairFiles(kThird);
  check(!u.identity && u.files.files == 0, "no identity at all: none reported");
  storeFolder(kPartner);
  u = unpairFiles(kPartner);
  check(u.identity, "a cached identity: reported as removed");
}

// ---- Cases: the reply line -------------------------------------------------
static std::string line(bool identity, uint16_t files, uint32_t bytes, uint32_t manifest, bool left,
                        const char* received, size_t size = 256) {
  PeerFilesRemoved r{};
  r.files = files;
  r.bytes = bytes;
  r.folderLeft = left;
  std::vector<char> out(size + 8, '#');
  formatUnpairFilesLine(out.data(), size, identity, r, manifest, "/system/espnow/peers/AABBCCDDEE01", received);
  if (out[size + 7] != '#') return "OVERRUN";
  return out.data();
}

static void replyLine() {
  check(line(true, 4, 8306, 61903, false, "/espnow/received/AABBCCDDEE01") ==
            "Removed: crypto identity, 4 cached files (8 kB), cached command manifest (60 kB). "
            "Received files stay in /espnow/received/AABBCCDDEE01.",
        "reply: identity, cached files and manifest in one line, with where received files stay");
  check(line(false, 1, 32, 0, false, nullptr) == "Removed: 1 cached file (32 bytes).", "reply: one small file");
  check(line(false, 0, 0, 0, false, nullptr) == "Nothing was stored about it.", "reply: nothing stored");
  check(line(true, 0, 0, 0, true, nullptr) ==
            "Removed: crypto identity. Not removed: some files in /system/espnow/peers/AABBCCDDEE01.",
        "reply: names the folder that stays");
  check(line(false, 0, 0, 0, true, nullptr) == "Not removed: some files in /system/espnow/peers/AABBCCDDEE01.",
        "reply: nothing removed but a file stayed: no 'nothing stored', the folder is named");
  check(line(false, 0, 0, 0, true, "/espnow/received/AABBCCDDEE01") ==
            "Not removed: some files in /system/espnow/peers/AABBCCDDEE01. "
            "Received files stay in /espnow/received/AABBCCDDEE01.",
        "reply: a folder that stays and received files, nothing removed");
  const std::string cut = line(true, 4, 8306, 61903, true, "/espnow/received/AABBCCDDEE01", 40);
  check(cut != "OVERRUN" && cut.size() == 39, "reply: a small buffer is filled, terminated and not overrun");
}

int main() {
  unpairRemovesEverything();
  manyFiles();
  nothingStored();
  fileThatStays();
  manifestFollowsFirmware();
  sharedManifest();
  unreadablePeers();
  badReferences();
  referenceWriteFails();
  cacheHit();
  identityReported();
  replyLine();
  if (gFailures) {
    std::fprintf(stderr, "espnow_peer_files: %d of %d checks failed\n", gFailures, gChecks);
    return 1;
  }
  std::printf("espnow_peer_files: %d checks passed\n", gChecks);
  return 0;
}
