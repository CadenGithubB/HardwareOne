#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Identity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <time.h>

#include "System_Debug.h"
#include "System_ESPNow_Crypto.h"
#include "System_Filesystem.h"   // filesystemReady
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

bool ensureIdentityDirs() {
  auto auth = VFS::systemAuth("espnow.identity.mkdir");
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

  JsonDocument doc;
  doc["version"]                       = kIdentityFileVersion;
  doc["ed25519_pub_hex"]               = pubHex;
  doc["ed25519_secret_encrypted"]      = secEncrypted;
  doc["createdAtSec"]                  = id.createdAtSec;
  doc["regenCount"]                    = id.regenCount;

  FsLockGuard fsGuard("espnow.identity.write");

  File f = VFS::openGuarded(kIdentityTempPath, "w",
                            VFS::systemAuth("espnow.identity.write"), true);
  if (!f) {
    ERROR_ESPNOWF("identity write: cannot open %s", kIdentityTempPath);
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.flush();
  f.close();
  if (written == 0) {
    ERROR_ESPNOWF("identity write: serializeJson produced 0 bytes");
    VFS::removeGuarded(kIdentityTempPath, VFS::systemAuth("espnow.identity.write"));
    return false;
  }

  if (!VFS::renameGuarded(kIdentityTempPath, kIdentityPath,
                          VFS::systemAuth("espnow.identity.write"))) {
    ERROR_ESPNOWF("identity write: rename %s -> %s failed",
                  kIdentityTempPath, kIdentityPath);
    VFS::removeGuarded(kIdentityTempPath, VFS::systemAuth("espnow.identity.write"));
    return false;
  }
  return true;
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

  if (!VFS::existsGuarded(kIdentityPath, VFS::systemAuth("espnow.identity.read"))) {
    absent = true;
    return false;
  }

  FsLockGuard fsGuard("espnow.identity.read");

  File f = VFS::openGuarded(kIdentityPath, "r", VFS::systemAuth("espnow.identity.read"));
  if (!f) {
    ERROR_ESPNOWF("identity read: cannot open %s", kIdentityPath);
    return false;
  }

  JsonDocument doc;
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
  time_t now = time(nullptr);
  out.createdAtSec = (now > 0) ? (uint32_t)now : 0;
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

// Slot count: matches the existing peer-table cap. Each slot is ~50 B → tiny.
// Placed in BSS rather than PSRAM because access frequency is high (every
// RX dispatch could call peerIdentityFindByMac in 3.4+) and the table is
// small enough that DRAM cost is negligible.
constexpr uint8_t kPeerSlots = 16;
PeerIdentity gPeerIdentities[kPeerSlots] = {};

constexpr const char* kPeersDir = "/system/espnow/peers";
// File schema versions:
//   1 — initial (Phase 3.2). No subscribedEvents field.
//   2 — Phase 5: added subscribedEvents (uint32 bitmask). v1 files are
//       upgraded on load (subscribedEvents defaults to ALL = 0xFFFFFFFF);
//       the next write rewrites them as v2.
constexpr uint8_t  kPeerFileVersion = 2;

// Format MAC as 12-char uppercase hex without separators, matching the
// existing directory convention at /system/espnow/peers/AABBCCDDEEFF/.
void formatMacNoSep(const uint8_t mac[6], char* out13) {
  macToPathToken(mac, out13);  // canonical PATH TOKEN form (System_Utils.h)
}

bool parseMacNoSep(const char* in, uint8_t mac[6]) {
  if (!in || strlen(in) != 12) return false;
  auto nyb = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (int i = 0; i < 6; i++) {
    int hi = nyb(in[i * 2]);
    int lo = nyb(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    mac[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
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
  formatMacNoSep(mac, macHex);
  int n = snprintf(out, outLen, "%s/%s/%s", kPeersDir, macHex, leaf);
  return n > 0 && (size_t)n < outLen;
}

bool ensurePeerDir(const uint8_t mac[6]) {
  auto auth = VFS::systemAuth("espnow.peer_identity_mkdir");
  if (!VFS::existsGuarded("/system", auth) && !VFS::mkdirGuarded("/system", auth)) return false;
  if (!VFS::existsGuarded("/system/espnow", auth) && !VFS::mkdirGuarded("/system/espnow", auth)) return false;
  if (!VFS::existsGuarded(kPeersDir, auth) && !VFS::mkdirGuarded(kPeersDir, auth)) return false;

  char dir[64];
  char macHex[13];
  formatMacNoSep(mac, macHex);
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
  formatMacNoSep(p.mac, macHex);
  char macColons[18];
  macToDisplay(p.mac, macColons, sizeof(macColons));  // canonical DISPLAY form
  char pubHex[65];
  bytesToHex(p.longTermPub, 32, pubHex);

  JsonDocument doc;
  doc["version"]                  = kPeerFileVersion;
  doc["mac"]                      = String(macColons);
  doc["meshId"]                   = p.meshId;
  doc["longTermPubEd25519_hex"]   = String(pubHex);
  doc["bondedAtSec"]              = p.bondedAtSec;
  doc["lastSeenSec"]              = p.lastSeenSec;
  doc["subscribedEvents"]         = p.subscribedEvents;  // Phase 5

  FsLockGuard fsGuard("espnow.peer_identity_write");
  File f = VFS::openGuarded(tmp, "w",
                            VFS::systemAuth("espnow.peer_identity_write"), true);
  if (!f) {
    ERROR_ESPNOWF("peer identity write: open %s failed", tmp);
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.flush();
  f.close();
  if (written == 0) {
    VFS::removeGuarded(tmp, VFS::systemAuth("espnow.peer_identity_write"));
    return false;
  }
  if (!VFS::renameGuarded(tmp, path, VFS::systemAuth("espnow.peer_identity_write"))) {
    VFS::removeGuarded(tmp, VFS::systemAuth("espnow.peer_identity_write"));
    return false;
  }
  return true;
}

bool readPeerIdentityFile(const char* path, PeerIdentity& out) {
  if (!filesystemReady) return false;
  auto auth = VFS::systemAuth("espnow.peer_identity_read");
  if (!VFS::existsGuarded(path, auth)) return false;

  FsLockGuard fsGuard("espnow.peer_identity_read");
  File f = VFS::openGuarded(path, "r", auth);
  if (!f) return false;
  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, f);
  f.close();
  if (jerr) return false;

  // Phase 5: accept v1 (no subscribedEvents) and v2 (with). Anything else
  // is unknown / future — reject so we don't silently mis-parse.
  uint8_t ver = doc["version"] | (uint8_t)0;
  if (ver != 1 && ver != kPeerFileVersion) return false;

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

  out.meshId      = doc["meshId"] | (uint8_t)0;
  out.bondedAtSec = doc["bondedAtSec"] | (uint32_t)0;
  out.lastSeenSec = doc["lastSeenSec"] | (uint32_t)0;
  // Phase 5: v1 files don't carry subscribedEvents — default to ALL so they
  // keep receiving every broadcast until they opt out via SUBSCRIBE_UPDATE.
  out.subscribedEvents = doc["subscribedEvents"] | (uint32_t)ESPNOW_EVT_ALL;

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
  int idx = findSlotByMac(mac);
  if (idx < 0) idx = findFreeSlot();
  if (idx < 0) {
    ERROR_ESPNOWF("peer identity persist: cache full (%u slots)", (unsigned)kPeerSlots);
    return false;
  }
  PeerIdentity& p = gPeerIdentities[idx];
  bool isNewPeer = !p.valid;
  memcpy(p.mac, mac, 6);
  p.meshId       = meshId;
  memcpy(p.longTermPub, pub, 32);
  // Preserve bondedAtSec across re-pair (don't update if already valid),
  // overwrite if new. Always refresh lastSeenSec to "now".
  if (!p.valid || p.bondedAtSec == 0) p.bondedAtSec = bondedAtSec;
  time_t now = time(nullptr);
  p.lastSeenSec = (now > 0) ? (uint32_t)now : 0;
  // Phase 5: new peers default to ALL events. Existing peers' subscription
  // bitmap is preserved across re-pair (we don't want a KEY_EX redo to wipe
  // an explicit subscribe-narrowed bitmap).
  if (isNewPeer) p.subscribedEvents = ESPNOW_EVT_ALL;
  p.valid = true;

  if (!writePeerIdentityFile(p)) {
    ERROR_ESPNOWF("peer identity persist: disk write failed for %02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return false;
  }
  INFO_ESPNOWF("peer identity stored: %02X:%02X:%02X:%02X:%02X:%02X meshId=%u",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)meshId);
  return true;
}

void peerIdentityNoteSeen(const uint8_t mac[6], uint32_t nowSec) {
  int idx = findSlotByMac(mac);
  if (idx < 0) return;
  gPeerIdentities[idx].lastSeenSec = nowSec;
}

bool peerIdentitySetSubscriptions(const uint8_t mac[6], uint32_t subscribedEvents) {
  int idx = findSlotByMac(mac);
  if (idx < 0) {
    DEBUG_ESPNOWF("peer subscriptions: ignoring SUBSCRIBE_UPDATE from unknown peer "
                  "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return false;
  }
  uint32_t prev = gPeerIdentities[idx].subscribedEvents;
  if (prev == subscribedEvents) {
    DEBUG_ESPNOWF("peer subscriptions: no change for %02X:%02X:%02X:%02X:%02X:%02X (0x%08lX)",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  (unsigned long)subscribedEvents);
    return true;  // no-op success
  }
  gPeerIdentities[idx].subscribedEvents = subscribedEvents;
  if (!writePeerIdentityFile(gPeerIdentities[idx])) {
    // Roll back in-memory state so persistence + cache stay consistent.
    gPeerIdentities[idx].subscribedEvents = prev;
    ERROR_ESPNOWF("peer subscriptions: persist failed for %02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return false;
  }
  INFO_ESPNOWF("peer subscriptions: %02X:%02X:%02X:%02X:%02X:%02X now 0x%08lX (was 0x%08lX)",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
               (unsigned long)subscribedEvents, (unsigned long)prev);
  return true;
}

bool peerIdentityWantsEvent(const uint8_t mac[6], uint32_t category) {
  int idx = findSlotByMac(mac);
  if (idx < 0) {
    // Unknown peer — fall back to legacy behaviour (deliver everything).
    // Phase 5 only narrows for peers that have explicitly opted in.
    return true;
  }
  return (gPeerIdentities[idx].subscribedEvents & category) != 0;
}

bool peerIdentityForget(const uint8_t mac[6]) {
  int idx = findSlotByMac(mac);
  // Always try to remove the file even if we didn't have the slot cached,
  // so manual flash editing or a stale file gets cleaned up.
  char path[80];
  if (buildPeerPath(mac, "identity.json", path, sizeof(path))) {
    auto auth = VFS::systemAuth("espnow.peer_identity_forget");
    if (VFS::existsGuarded(path, auth)) {
      VFS::removeGuarded(path, auth);
    }
  }
  if (idx >= 0) {
    gPeerIdentities[idx] = PeerIdentity{};
  }
  return true;
}

uint8_t peerIdentityLoadAll() {
  if (!filesystemReady) return 0;
  // Reset cache.
  for (uint8_t i = 0; i < kPeerSlots; i++) gPeerIdentities[i] = PeerIdentity{};

  auto auth = VFS::systemAuth("espnow.peer_identity_scan");
  if (!VFS::existsGuarded(kPeersDir, auth)) return 0;

  FsLockGuard fsGuard("espnow.peer_identity_scan");
  File dir = VFS::openGuarded(kPeersDir, "r", auth);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  uint8_t loaded = 0;
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
      if (parseMacNoSep(base, mac) && loaded < kPeerSlots) {
        char path[80];
        if (buildPeerPath(mac, "identity.json", path, sizeof(path))) {
          PeerIdentity p = {};
          if (readPeerIdentityFile(path, p)) {
            // Verify mac matches dir name (paranoia — defends against a
            // moved/renamed file).
            if (memcmp(p.mac, mac, 6) == 0) {
              gPeerIdentities[loaded] = p;
              loaded++;
            }
          }
        }
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

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

#endif  // ENABLE_ESPNOW
