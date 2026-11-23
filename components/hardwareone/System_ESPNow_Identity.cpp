#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Identity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR
#include <time.h>

#include "System_ClockCore.h"    // clockcore::isValidEpoch: a time is stored only when the clock is set
#include "System_Debug.h"
#include "System_ESPNow_Crypto.h"
#include "System_Filesystem.h"   // filesystemReady
#include "System_MemUtil.h"      // PSRAM_JSON_DOC
#include "System_Mutex.h"        // FsLockGuard
#include "System_Settings.h"     // encryptString / decryptString
#include "System_VFS.h"          // openGuarded / mkdirGuarded / renameGuarded / systemAuth
#include "System_Utils.h"        // macToPathToken / macToDisplay / macParse

namespace {

constexpr const char* kIdentityDir       = "/system/espnow";
constexpr const char* kIdentityPath      = "/system/espnow/identity.json";
constexpr const char* kIdentityTempPath  = "/system/espnow/identity.tmp";
constexpr uint8_t     kIdentityFileVersion = 1;

EspNowIdentity gIdentity = {};

// 64 bytes → 128 hex chars + NUL. Used for the AES-CBC encrypt wrapper
// because encryptString operates on Arduino String (printable text); we
// hex-encode the raw secret first so null bytes don't truncate.
void bytesToHex(const uint8_t* in, size_t inLen, char* out) {
  static const char* k = "0123456789abcdef";
  for (size_t i = 0; i < inLen; i++) {
    out[i * 2]     = k[(in[i] >> 4) & 0xF];
    out[i * 2 + 1] = k[in[i] & 0xF];
  }
  out[inLen * 2] = '\0';
}

bool hexToBytes(const char* in, size_t inLen, uint8_t* out, size_t outLen) {
  if (inLen != outLen * 2) return false;
  auto nyb = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < outLen; i++) {
    int hi = nyb(in[i * 2]);
    int lo = nyb(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// Stream `doc` into the sibling `tmp` and promote it over `dest` only when
// the file holds every byte measureJson() promised. On a nearly full LittleFS
// serializeJson stops short but still returns a positive count; promoting that
// would leave a truncated identity which boot refuses to load, and only a
// bond-wiping regeneration repairs it. On any failure the temp is removed and
// the previous file is left exactly as it was. Caller holds the filesystem lock.
bool writeJsonFileComplete(const JsonDocument& doc, const char* tmp, const char* dest,
                           const char* owner) {
  const auto auth = VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, owner);
  const size_t expected = doc.overflowed() ? 0 : measureJson(doc);
  if (expected == 0) {
    ERROR_ESPNOWF("%s: document could not be serialized; %s kept", owner, dest);
    return false;
  }
  File f = VFS::openGuarded(tmp, "w", auth, true);
  if (!f) {
    ERROR_ESPNOWF("%s: cannot open %s", owner, tmp);
    return false;
  }
  const size_t written = serializeJson(doc, f);
  f.flush();
  const size_t stored = f.size();
  f.close();
  if (written != expected || stored != expected) {
    ERROR_ESPNOWF("%s: incomplete write to %s (%u/%u bytes, %u stored); %s kept",
                  owner, tmp, (unsigned)written, (unsigned)expected, (unsigned)stored, dest);
    VFS::removeGuarded(tmp, auth);
    return false;
  }
  if (!VFS::renameGuarded(tmp, dest, auth)) {
    ERROR_ESPNOWF("%s: rename %s -> %s failed", owner, tmp, dest);
    VFS::removeGuarded(tmp, auth);
    return false;
  }
  return true;
}

bool ensureIdentityDirs() {
  auto auth = VFS::systemAuth(VFS::Scopes::SYSTEM_STATE, "espnow.identity.mkdir");
  if (!VFS::existsGuarded("/system", auth)) {
    if (!VFS::mkdirGuarded("/system", auth)) return false;
  }
  if (!VFS::existsGuarded(kIdentityDir, auth)) {
    if (!VFS::mkdirGuarded(kIdentityDir, auth)) return false;
  }
  return true;
}

bool writeIdentityFile(const EspNowIdentity& id) {
  if (!filesystemReady) {
    ERROR_ESPNOWF("identity write: filesystem not ready");
    return false;
  }
  if (!ensureIdentityDirs()) {
    ERROR_ESPNOWF("identity write: failed to create %s", kIdentityDir);
    return false;
  }

  // Encrypt the 64-byte Ed25519 secret. Hex-encode first so binary bytes
  // survive the String-based encryptString path. Result is "AES:<iv>:<ct>".
  char secHex[129];
  bytesToHex(id.sec, sizeof(id.sec), secHex);
  String secEncrypted = encryptString(String(secHex));
  // Wipe the hex buffer immediately — it's the raw secret in plaintext.
  memset(secHex, 0, sizeof(secHex));
  if (secEncrypted.length() == 0) {
    ERROR_ESPNOWF("identity write: secret encryption failed");
    return false;
  }

  char pubHex[65];
  bytesToHex(id.pub, sizeof(id.pub), pubHex);

  PSRAM_JSON_DOC(doc);
  doc["version"]                       = kIdentityFileVersion;
  doc["ed25519_pub_hex"]               = pubHex;
  doc["ed25519_secret_encrypted"]      = secEncrypted;
  doc["createdAtSec"]                  = id.createdAtSec;
  doc["regenCount"]                    = id.regenCount;

  FsLockGuard fsGuard("espnow.identity.write");
  return writeJsonFileComplete(doc, kIdentityTempPath, kIdentityPath, "espnow.identity.write");
}

// Read on success returns true; out is populated. On absence returns false
// with `out.valid=false` and absent flag set so the caller can branch. On
// presence-but-corrupt returns false WITHOUT setting absent — that's the
// "refuse to start, recovery via CLI" path.
bool readIdentityFile(EspNowIdentity& out, bool& absent) {
  absent = false;
  out.valid = false;

  if (!filesystemReady) {
    ERROR_ESPNOWF("identity read: filesystem not ready");
    return false;
  }

  if (!VFS::existsGuarded(kIdentityPath, VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, "espnow.identity.read"))) {
    absent = true;
    return false;
  }

  FsLockGuard fsGuard("espnow.identity.read");

  File f = VFS::openGuarded(kIdentityPath, "r", VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, "espnow.identity.read"));
  if (!f) {
    ERROR_ESPNOWF("identity read: cannot open %s", kIdentityPath);
    return false;
  }

  PSRAM_JSON_DOC(doc);
  DeserializationError jerr = deserializeJson(doc, f);
  f.close();
  if (jerr) {
    ERROR_ESPNOWF("identity read: JSON parse failed: %s", jerr.c_str());
    return false;
  }

  if (!doc["version"].is<uint8_t>() ||
      doc["version"].as<uint8_t>() != kIdentityFileVersion) {
    ERROR_ESPNOWF("identity read: unknown file version");
    return false;
  }

  const char* pubHex = doc["ed25519_pub_hex"] | (const char*)nullptr;
  const char* secEnc = doc["ed25519_secret_encrypted"] | (const char*)nullptr;
  if (!pubHex || !secEnc) {
    ERROR_ESPNOWF("identity read: missing pub or encrypted secret field");
    return false;
  }
  if (!hexToBytes(pubHex, strlen(pubHex), out.pub, sizeof(out.pub))) {
    ERROR_ESPNOWF("identity read: pub hex malformed");
    return false;
  }

  String secHex = decryptString(String(secEnc));
  if (secHex.length() != sizeof(out.sec) * 2) {
    ERROR_ESPNOWF("identity read: secret decrypt yielded %d hex chars, expected %d",
                  (int)secHex.length(), (int)(sizeof(out.sec) * 2));
    return false;
  }
  if (!hexToBytes(secHex.c_str(), secHex.length(), out.sec, sizeof(out.sec))) {
    ERROR_ESPNOWF("identity read: secret hex malformed");
    // Best-effort wipe of the recovered hex String. Arduino String has no
    // public wipe primitive — we overwrite via index assignment then let
    // it free.
    for (size_t i = 0; i < secHex.length(); i++) secHex.setCharAt(i, '0');
    return false;
  }
  // Wipe the decrypted hex now that bytes are extracted.
  for (size_t i = 0; i < secHex.length(); i++) secHex.setCharAt(i, '0');

  // Sanity check: the trailing 32 bytes of an Ed25519 secret-key blob equal
  // the public key. If they don't match, the file has been tampered with
  // or paired the wrong way during write.
  if (memcmp(out.sec + 32, out.pub, 32) != 0) {
    ERROR_ESPNOWF("identity read: pub/sec halves do not match — file corrupt");
    return false;
  }

  out.createdAtSec = doc["createdAtSec"] | (uint32_t)0;
  out.regenCount   = doc["regenCount"]   | (uint32_t)0;
  out.valid        = true;
  return true;
}

bool generateFreshIdentity(EspNowIdentity& out, uint32_t regenCount) {
  if (!espnowCryptoReady()) {
    ERROR_ESPNOWF("identity gen: crypto not initialized");
    return false;
  }
  if (!espnowCryptoEd25519Keygen(out.pub, out.sec)) {
    ERROR_ESPNOWF("identity gen: keypair generation failed");
    return false;
  }
  // 0 while the clock is not set: a clock that was never set counts up from
  // 1970, and that number is not a creation date.
  time_t now = time(nullptr);
  out.createdAtSec = clockcore::isValidEpoch((int64_t)now) ? (uint32_t)now : 0;
  out.regenCount   = regenCount;
  out.valid        = true;
  return true;
}

}  // namespace

bool espnowIdentityLoadOrGenerate(EspNowIdentity& out) {
  bool absent = false;
  if (readIdentityFile(out, absent)) {
    gIdentity = out;
    INFO_ESPNOWF("identity loaded, createdAtSec=%u regenCount=%u",
                 out.createdAtSec, out.regenCount);
    return true;
  }
  if (!absent) {
    // File present but corrupt — refuse to auto-regen. Operator must run
    // `espnowregenidentity --confirm-wipe-all-bonds` to recover.
    ERROR_ESPNOWF("identity present but unreadable — refusing to auto-regenerate. "
                  "Run 'espnowregenidentity --confirm-wipe-all-bonds' to recover.");
    return false;
  }

  // First-boot path: generate and persist.
  if (!generateFreshIdentity(out, /*regenCount=*/0)) return false;
  if (!writeIdentityFile(out)) {
    ERROR_ESPNOWF("identity gen: persistence failed; aborting");
    out.valid = false;
    return false;
  }
  gIdentity = out;
  INFO_ESPNOWF("identity generated, createdAtSec=%u regenCount=0",
               out.createdAtSec);
  return true;
}

bool espnowIdentityRegenerate(EspNowIdentity& out) {
  uint32_t nextRegen = gIdentity.valid ? (gIdentity.regenCount + 1) : 1;
  if (!generateFreshIdentity(out, nextRegen)) return false;
  if (!writeIdentityFile(out)) {
    out.valid = false;
    return false;
  }
  gIdentity = out;
  WARN_ESPNOWF("identity regenerated, regenCount=%u — all existing peer trust must be re-paired",
               out.regenCount);
  return true;
}

const EspNowIdentity& espnowIdentityGet() {
  return gIdentity;
}

void espnowIdentityFormatPubHex(const uint8_t pub[32], char* out, size_t outLen) {
  if (outLen < 65) {
    if (outLen > 0) out[0] = '\0';
    return;
  }
  bytesToHex(pub, 32, out);
}

// ============================================================================
// Phase 3.2 — peer identity persistence
// ============================================================================

namespace {

// Slot count: matches the existing peer-table cap. Each slot is ~56 B → ~900 B.
// PSRAM .bss, parallel to gEspNow->devices[16]: peerIdentityLoadAll runs at boot
// unconditionally (the identity block deliberately does not wait for ESP-NOW
// to start, for recovery), so this table cannot be made lazy — devices with ESP-NOW
// switched off would still pay for it in internal DRAM. The tradeoff is a
// 16-slot linear scan of PSRAM per RX dispatch, which is noise next to the
// Ed25519/AEAD work already on that path. Contents are peer PUBLIC keys and
// timestamps — no secret material, so plaintext-PSRAM exposure is a non-issue.
constexpr uint8_t kPeerSlots = 16;
EXT_RAM_BSS_ATTR PeerIdentity gPeerIdentities[kPeerSlots] = {};

constexpr const char* kPeersDir = "/system/espnow/peers";
// File schema version 2 (1.0 boards start erased; a file of another version is
// not read). It no longer carries subscribedEvents (SUBSCRIBE_UPDATE was removed
// in 1.0); a file that still has the key reads the same.
constexpr uint8_t  kPeerFileVersion = 2;

// peerIdentityPersist rewrites an unchanged peer's file only to move the
// stored lastSeenSec forward, and only once it is this far behind.
constexpr uint32_t kPeerLastSeenRefreshSec = 3600;

// 12 hex digits, either case. A peer folder must also be spelled exactly as
// macToPathToken() writes it (upper case); peerIdentityLoadAll checks that.
bool parseMacNoSep(const char* in, uint8_t mac[6]) {
  return in && strlen(in) == 12 && macParse(in, mac);
}

int findSlotByMac(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < kPeerSlots; i++) {
    if (gPeerIdentities[i].valid && memcmp(gPeerIdentities[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

int findFreeSlot() {
  for (uint8_t i = 0; i < kPeerSlots; i++) {
    if (!gPeerIdentities[i].valid) return i;
  }
  return -1;
}

bool buildPeerPath(const uint8_t mac[6], const char* leaf,
                   char* out, size_t outLen) {
  char macHex[13];
  macToPathToken(mac, macHex);
  int n = snprintf(out, outLen, "%s/%s/%s", kPeersDir, macHex, leaf);
  return n > 0 && (size_t)n < outLen;
}

bool ensurePeerDir(const uint8_t mac[6]) {
  auto auth = VFS::systemAuth(VFS::Scopes::SYSTEM_STATE, "espnow.peer_identity_mkdir");
  if (!VFS::existsGuarded("/system", auth) && !VFS::mkdirGuarded("/system", auth)) return false;
  if (!VFS::existsGuarded("/system/espnow", auth) && !VFS::mkdirGuarded("/system/espnow", auth)) return false;
  if (!VFS::existsGuarded(kPeersDir, auth) && !VFS::mkdirGuarded(kPeersDir, auth)) return false;

  char dir[64];
  char macHex[13];
  macToPathToken(mac, macHex);
  snprintf(dir, sizeof(dir), "%s/%s", kPeersDir, macHex);
  if (!VFS::existsGuarded(dir, auth) && !VFS::mkdirGuarded(dir, auth)) return false;
  return true;
}

bool writePeerIdentityFile(const PeerIdentity& p) {
  if (!filesystemReady) return false;
  if (!ensurePeerDir(p.mac)) {
    ERROR_ESPNOWF("peer identity write: mkdir failed");
    return false;
  }

  char path[80], tmp[80];
  if (!buildPeerPath(p.mac, "identity.json", path, sizeof(path))) return false;
  if (!buildPeerPath(p.mac, "identity.tmp",  tmp,  sizeof(tmp)))  return false;

  char macHex[13];
  macToPathToken(p.mac, macHex);
  char macColons[18];
  macToDisplay(p.mac, macColons, sizeof(macColons));  // canonical DISPLAY form
  char pubHex[65];
  bytesToHex(p.longTermPub, 32, pubHex);

  PSRAM_JSON_DOC(doc);
  doc["version"]                  = kPeerFileVersion;
  doc["mac"]                      = String(macColons);
  doc["meshId"]                   = p.meshId;
  doc["longTermPubEd25519_hex"]   = String(pubHex);
  doc["bondedAtSec"]              = p.bondedAtSec;
  doc["lastSeenSec"]              = p.lastSeenSec;

  FsLockGuard fsGuard("espnow.peer_identity_write");
  return writeJsonFileComplete(doc, tmp, path, "espnow.peer_identity_write");
}

bool readPeerIdentityFile(const char* path, PeerIdentity& out) {
  if (!filesystemReady) return false;
  auto auth = VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, "espnow.peer_identity_read");
  if (!VFS::existsGuarded(path, auth)) return false;

  FsLockGuard fsGuard("espnow.peer_identity_read");
  File f = VFS::openGuarded(path, "r", auth);
  if (!f) return false;
  PSRAM_JSON_DOC(doc);
  DeserializationError jerr = deserializeJson(doc, f);
  f.close();
  if (jerr) return false;

  // Only the version this firmware writes; any other is not one it wrote
  // (1.0 boards start erased) and is rejected rather than mis-parsed.
  uint8_t ver = doc["version"] | (uint8_t)0;
  if (ver != kPeerFileVersion) return false;

  const char* macStr = doc["mac"] | (const char*)nullptr;
  if (!macStr) return false;
  // Parse colon-MAC ("AA:BB:..."): strip colons then hex-decode.
  char macFlat[13] = {};
  size_t pos = 0;
  for (size_t i = 0; macStr[i] && pos < 12; i++) {
    if (macStr[i] != ':') macFlat[pos++] = macStr[i];
  }
  if (pos != 12) return false;
  if (!parseMacNoSep(macFlat, out.mac)) return false;

  // The mesh slot indexes per-mesh tables elsewhere, and a file that names a
  // slot this firmware does not have is not one it wrote: leave it out, so the
  // walk marks the cache incomplete and names it. ArduinoJson turns a value
  // that does not fit a uint8_t into the default 0, so the range is checked on
  // the unsigned value; a missing, negative or non-integer meshId fails it too.
  JsonVariantConst meshVal = doc["meshId"];
  if (!meshVal.is<unsigned>() || meshVal.as<unsigned>() >= Settings::N_MESHES) return false;
  out.meshId      = (uint8_t)meshVal.as<unsigned>();
  out.bondedAtSec = doc["bondedAtSec"] | (uint32_t)0;
  out.lastSeenSec = doc["lastSeenSec"] | (uint32_t)0;

  const char* pubHex = doc["longTermPubEd25519_hex"] | (const char*)nullptr;
  if (!pubHex || strlen(pubHex) != 64) return false;
  if (!hexToBytes(pubHex, 64, out.longTermPub, 32)) return false;

  out.valid = true;
  return true;
}

}  // namespace

const PeerIdentity* peerIdentityFindByMac(const uint8_t mac[6]) {
  int idx = findSlotByMac(mac);
  return idx < 0 ? nullptr : &gPeerIdentities[idx];
}

bool peerIdentityPersist(const uint8_t mac[6], uint8_t meshId,
                         const uint8_t pub[32], uint32_t bondedAtSec) {
  // The KEY_EX handlers pass the slot their fingerprint matched; a file with
  // anything else would not load at the next boot.
  if (meshId >= Settings::N_MESHES) {
    ERROR_ESPNOWF("peer identity persist: mesh slot %u out of range", (unsigned)meshId);
    return false;
  }
  int idx = findSlotByMac(mac);
  if (idx < 0) idx = findFreeSlot();
  if (idx < 0) {
    ERROR_ESPNOWF("peer identity persist: cache full (%u slots)", (unsigned)kPeerSlots);
    return false;
  }
  PeerIdentity& p = gPeerIdentities[idx];
  const PeerIdentity before = p;  // put back if the write fails
  time_t now = time(nullptr);
  const uint32_t nowSec = clockcore::isValidEpoch((int64_t)now) ? (uint32_t)now : 0;  // 0 while the clock is not set

  // A KEY_EX that changes nothing we store (same key, same mesh, bondedAtSec
  // already set) is not worth a flash write. Every KEY_EX_HELLO and REPLY
  // lands here, a recorded HELLO can be replayed without the passphrase, and
  // the HELLO handler runs on the receive task. The one thing such a call
  // would change is lastSeenSec, and that is rewritten at most once per
  // kPeerLastSeenRefreshSec, and only forwards (a clock that is not set yet
  // never replaces a real timestamp). The cached record always equals the
  // stored one.
  const bool unchanged = p.valid && p.meshId == meshId &&
                         memcmp(p.longTermPub, pub, 32) == 0 &&
                         (p.bondedAtSec != 0 || bondedAtSec == 0);
  const bool lastSeenDue = nowSec > p.lastSeenSec &&
                           (nowSec - p.lastSeenSec) >= kPeerLastSeenRefreshSec;
  if (unchanged && !lastSeenDue) return true;

  memcpy(p.mac, mac, 6);
  p.meshId       = meshId;
  memcpy(p.longTermPub, pub, 32);
  // Preserve bondedAtSec across re-pair (don't update if already valid),
  // overwrite if new. lastSeenSec becomes "now" with every write.
  if (!p.valid || p.bondedAtSec == 0) p.bondedAtSec = bondedAtSec;
  p.lastSeenSec = nowSec;
  p.valid = true;

  if (!writePeerIdentityFile(p)) {
    // Keep the cache equal to the file: a record that exists only in RAM would
    // read as "unchanged" on the next KEY_EX and never reach the flash.
    p = before;
    ERROR_ESPNOWF("peer identity persist: disk write failed for " MAC_FMT,
                  MAC_ARGS(mac));
    return false;
  }
  INFO_ESPNOWF("peer identity stored: " MAC_FMT " meshId=%u",
               MAC_ARGS(mac), (unsigned)meshId);
  return true;
}



// True once peerIdentityLoadAll has walked the peers directory this boot (or
// found there is none) and every stored identity went into the cache. While
// false the cache does not say which mesh slots the stored identities use.
static bool gPeerIdentitiesLoaded = false;

bool peerIdentitiesLoaded() { return gPeerIdentitiesLoaded; }

bool peerIdentityForget(const uint8_t mac[6]) {
  int idx = findSlotByMac(mac);
  auto auth = VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, "espnow.peer_identity_forget");
  // Remove the identity file(s) even if the slot wasn't cached, so manual flash
  // editing or a stale file gets cleaned up.
  char path[80];
  bool fileRemoved = false;
  if (buildPeerPath(mac, "identity.json", path, sizeof(path)) && VFS::existsGuarded(path, auth)) {
    VFS::removeGuarded(path, auth);
    // A file that would not go away is loaded again at the next boot with its
    // mesh slot; until then the cache no longer shows it.
    if (VFS::existsGuarded(path, auth)) {
      gPeerIdentitiesLoaded = false;
      WARN_ESPNOWF("stored peer identity %s could not be removed", path);
    } else {
      fileRemoved = true;
    }
  }
  if (buildPeerPath(mac, "identity.tmp", path, sizeof(path)) && VFS::existsGuarded(path, auth)) {
    VFS::removeGuarded(path, auth);   // stray temp from an interrupted write
  }
  // Remove the now-empty per-peer directory so it doesn't linger in `files` listings.
  char dir[64], macHex[13];
  macToPathToken(mac, macHex);
  snprintf(dir, sizeof(dir), "%s/%s", kPeersDir, macHex);
  if (VFS::existsGuarded(dir, auth)) {
    VFS::rmdirGuarded(dir, auth);
  }
  if (idx >= 0) {
    gPeerIdentities[idx] = PeerIdentity{};
  }
  // A file the boot walk left out of the cache (a bad meshId) counts too.
  return idx >= 0 || fileRemoved;
}

bool peerIdentityFileStored(const uint8_t mac[6]) {
  auto auth = VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, "espnow.peer_identity_stored");
  char path[80];
  return buildPeerPath(mac, "identity.json", path, sizeof(path)) && VFS::existsGuarded(path, auth);
}

namespace {

// Removes what is in `dir`, counting the files into `out`; subfolders `depth`
// levels down are emptied and removed too. Entries are taken a few at a time
// with the directory closed before anything is removed, so the removal never
// runs under an open directory handle. The caller holds the filesystem lock.
void removeFolderContents(const char* dir, uint8_t depth, const AuthContext& auth,
                          PeerFilesRemoved& out) {
  constexpr uint8_t kBatch = 4;
  constexpr uint8_t kMaxPasses = 32;   // bounds a folder whose entries keep failing
  struct Entry {
    char name[64];
    bool folder;
    uint32_t size;
  };
  uint8_t failedHere = 0;   // entries of this folder that would not go
  for (uint8_t pass = 0; pass < kMaxPasses; ++pass) {
    Entry batch[kBatch];
    uint8_t n = 0;
    uint8_t skip = 0;   // the entries that failed are listed again: step over them
    File d = VFS::openGuarded(dir, "r", auth);
    if (!d || !d.isDirectory()) {
      if (d) d.close();
      break;
    }
    File e = d.openNextFile();
    while (e && n < kBatch) {
      const char* name = e.name();
      const char* leaf = strrchr(name, '/');
      leaf = leaf ? leaf + 1 : name;
      const size_t len = strlen(leaf);
      if (skip < failedHere) {
        ++skip;
      } else if (len < sizeof(batch[n].name)) {   // LittleFS names are at most 63 characters
        memcpy(batch[n].name, leaf, len + 1);
        batch[n].folder = e.isDirectory();
        batch[n].size = batch[n].folder ? 0 : (uint32_t)e.size();
        ++n;
      }
      e.close();
      e = d.openNextFile();
    }
    if (e) e.close();
    d.close();
    if (n == 0) break;

    for (uint8_t i = 0; i < n; ++i) {
      char path[136];
      const int w = snprintf(path, sizeof(path), "%s/%s", dir, batch[i].name);
      if (w < 0 || (size_t)w >= sizeof(path)) {
        // A path this long is not one this firmware writes: leave it, count it.
        if (failedHere < UINT8_MAX) failedHere++;
        continue;
      }
      bool gone;
      if (batch[i].folder) {
        if (depth > 0) removeFolderContents(path, depth - 1, auth, out);
        gone = VFS::rmdirGuarded(path, auth) || !VFS::existsGuarded(path, auth);
      } else {
        gone = VFS::removeGuarded(path, auth) || !VFS::existsGuarded(path, auth);
        if (gone) {
          if (out.files < UINT16_MAX) out.files++;
          out.bytes += batch[i].size;
        }
      }
      if (!gone && failedHere < UINT8_MAX) failedHere++;
    }
  }
  out.failed = (uint8_t)((out.failed + failedHere) > UINT8_MAX ? UINT8_MAX : out.failed + failedHere);
}

}  // namespace

bool peerFilesRemove(const uint8_t mac[6], PeerFilesRemoved& out) {
  out = PeerFilesRemoved{};
  // The cache slot and the identity files first, through the one function that
  // keeps the "identities read" mark honest.
  peerIdentityForget(mac);
  if (!filesystemReady) return false;

  char dir[64], token[13];
  macToPathToken(mac, token);
  snprintf(dir, sizeof(dir), "%s/%s", kPeersDir, token);
  auto auth = VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, "espnow.peer_files_remove");
  FsLockGuard fsGuard("espnow.peer_files_remove");
  if (!VFS::existsGuarded(dir, auth)) return true;
  removeFolderContents(dir, 1, auth, out);
  VFS::rmdirGuarded(dir, auth);
  out.folderLeft = VFS::existsGuarded(dir, auth);
  if (out.folderLeft) {
    WARN_ESPNOWF("%s could not be removed completely", dir);
  }
  return !out.folderLeft;
}

uint8_t peerIdentityLoadAll() {
  gPeerIdentitiesLoaded = false;
  if (!filesystemReady) return 0;
  // Reset cache.
  for (uint8_t i = 0; i < kPeerSlots; i++) gPeerIdentities[i] = PeerIdentity{};

  auto auth = VFS::systemAuth(VFS::Scopes::ESPNOW_STATE, "espnow.peer_identity_scan");
  if (!VFS::existsGuarded(kPeersDir, auth)) {
    gPeerIdentitiesLoaded = true;  // nothing stored yet
    return 0;
  }

  FsLockGuard fsGuard("espnow.peer_identity_scan");
  File dir = VFS::openGuarded(kPeersDir, "r", auth);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  uint8_t loaded = 0;
  uint8_t notLoaded = 0;
  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      // Build the candidate identity.json path. The entry name should be a
      // 12-hex-char MAC without separators (matches the existing convention).
      const char* name = entry.name();
      // entry.name() returns the basename only for LittleFS in recent Arduino
      // cores, but to be defensive accept either basename or full path.
      const char* base = strrchr(name, '/');
      base = base ? base + 1 : name;
      uint8_t mac[6];
      char path[80];
      // Only a folder spelled exactly as macToPathToken writes it is a peer
      // folder: every reader and writer (persist, forget, the bond caches)
      // builds that spelling, so "aabbccddee01" beside "AABBCCDDEE01" must not
      // load the upper-case folder's identity a second time.
      char canonical[13] = {};
      const bool isAddress = parseMacNoSep(base, mac);
      if (isAddress) macToPathToken(mac, canonical);
      if (isAddress && strcmp(base, canonical) != 0) {
        // Never written by this firmware, never read or removed by it either,
        // so it holds no mesh slot: skipped without marking the cache
        // incomplete, but named, since espnowunpair cannot remove it.
        char odd[96];
        snprintf(odd, sizeof(odd), "%s/%s/identity.json", kPeersDir, base);
        if (VFS::existsGuarded(odd, auth)) {
          WARN_ESPNOWF("stored peer identity %s is ignored: peer folders are named %s; remove that folder",
                       odd, canonical);
        }
      } else if (isAddress && findSlotByMac(mac) >= 0) {
        // Already cached under this address: one cache entry per device.
      } else if (isAddress && buildPeerPath(mac, "identity.json", path, sizeof(path)) &&
                 VFS::existsGuarded(path, auth)) {
        PeerIdentity p = {};
        // The mac must match the directory name (paranoia — defends against a
        // moved/renamed file).
        if (loaded < kPeerSlots && readPeerIdentityFile(path, p) && memcmp(p.mac, mac, 6) == 0) {
          gPeerIdentities[loaded] = p;
          loaded++;
        } else {
          // Stored but not in the cache (unreadable now, another address
          // inside, or no room): which mesh slot it uses is not known.
          if (notLoaded < 255) notLoaded++;
          char shown[18];
          macToDisplay(mac, shown, sizeof(shown));  // the form espnowunpair accepts
          WARN_ESPNOWF("stored peer identity %s/%s was not loaded; 'espnowunpair %s' removes it",
                       kPeersDir, base, shown);
        }
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  gPeerIdentitiesLoaded = notLoaded == 0;

  if (loaded > 0) {
    INFO_ESPNOWF("loaded %u peer identities from %s", (unsigned)loaded, kPeersDir);
  }
  return loaded;
}

const PeerIdentity* peerIdentityAt(uint8_t i) {
  if (i >= kPeerSlots) return nullptr;
  return gPeerIdentities[i].valid ? &gPeerIdentities[i] : nullptr;
}

uint8_t peerIdentitySlotCount() {
  return kPeerSlots;
}

uint8_t espnowIdentityFileVersion() { return kIdentityFileVersion; }

uint8_t peerIdentityFileVersion() { return kPeerFileVersion; }

#endif  // ENABLE_ESPNOW
