#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_MeshKeys.h"

#include <Arduino.h>
#include <string.h>
#include <sodium.h>

#include "System_Debug.h"
#include "System_ESPNow_Crypto.h"
#include "System_Settings.h"

namespace {

// One key cache slot per mesh. Tiny (~70 B × 4 = 280 B) so just static BSS.
MeshDerivedKeys gMeshDerivedKeys[Settings::N_MESHES] = {};

// 100000 iters via mbedtls HMAC-SHA256 backed by the ESP32 hardware SHA
// accelerator. Empirically ~1 s on ESP32 @ 240 MHz (10× faster than
// libsodium's software-only HMAC, which previously needed ~10 s for the
// same count). The PBKDF2 loop yields every 1024 iters so the Task WDT
// stays quiet even if the iteration count gets bumped further.
constexpr uint32_t kPbkdf2Iters    = 100000;
constexpr const char* kSaltPrefix  = "espnow-v4-mesh-salt:";
constexpr uint64_t  kSubkeyBootstrap = 1;
constexpr uint64_t  kSubkeyGroup     = 2;
constexpr const char* kCtxBootstrap  = "esp-boot";  // exactly 8 bytes
constexpr const char* kCtxGroup      = "esp-grup";  // exactly 8 bytes

// Salt = SHA256("espnow-v4-mesh-salt:" || meshLabel). Stable per label,
// distinct per mesh. Rename → salt changes → key must be recomputed.
void computeMeshSalt(const String& label, uint8_t out[32]) {
  String input = String(kSaltPrefix) + label;
  crypto_hash_sha256(out,
                     reinterpret_cast<const uint8_t*>(input.c_str()),
                     input.length());
}

}  // namespace

bool meshKeysStretchPassphrase(uint8_t meshIdx) {
  if (meshIdx >= Settings::N_MESHES) return false;
  if (!espnowCryptoReady()) return false;

  Settings::MeshIdentity& m = gSettings.meshes[meshIdx];

  if (m.passphrase.length() == 0) {
    // No passphrase — clear the cached key. Persisting an all-zero key
    // would be ambiguous, so we explicitly mark it invalid instead.
    memset(m.passphraseStretchedKey, 0, sizeof(m.passphraseStretchedKey));
    m.passphraseStretchedKeyValid = false;
    return false;
  }

  uint8_t salt[32];
  computeMeshSalt(m.label, salt);

  uint32_t startMs = millis();
  bool ok = espnowCryptoPbkdf2HmacSha256(
      m.passphraseStretchedKey,
      reinterpret_cast<const uint8_t*>(m.passphrase.c_str()), m.passphrase.length(),
      salt, sizeof(salt),
      kPbkdf2Iters);
  uint32_t elapsedMs = millis() - startMs;

  sodium_memzero(salt, sizeof(salt));

  if (!ok) {
    memset(m.passphraseStretchedKey, 0, sizeof(m.passphraseStretchedKey));
    m.passphraseStretchedKeyValid = false;
    ERROR_ESPNOWF("mesh '%s' passphrase stretch failed", m.label.c_str());
    return false;
  }
  m.passphraseStretchedKeyValid = true;
  INFO_ESPNOWF("mesh '%s' passphrase stretched (%lu iters in %u ms)",
               m.label.c_str(),
               (unsigned long)kPbkdf2Iters, (unsigned)elapsedMs);
  return true;
}

bool meshKeysDerive(uint8_t meshIdx) {
  if (meshIdx >= Settings::N_MESHES) return false;

  Settings::MeshIdentity& m = gSettings.meshes[meshIdx];
  MeshDerivedKeys& dk = gMeshDerivedKeys[meshIdx];

  if (!m.passphraseStretchedKeyValid) {
    dk.valid = false;
    memset(&dk, 0, sizeof(dk));
    return false;
  }

  bool ok1 = espnowCryptoKdfSubkey(dk.bootstrapKey, m.passphraseStretchedKey,
                                   kSubkeyBootstrap, kCtxBootstrap);
  bool ok2 = espnowCryptoKdfSubkey(dk.groupKey,     m.passphraseStretchedKey,
                                   kSubkeyGroup,    kCtxGroup);
  if (!ok1 || !ok2) {
    memset(&dk, 0, sizeof(dk));
    dk.valid = false;
    ERROR_ESPNOWF("mesh '%s' subkey derivation failed (boot=%d group=%d)",
                  m.label.c_str(), ok1 ? 1 : 0, ok2 ? 1 : 0);
    return false;
  }
  dk.fingerprint = m.fingerprint;
  dk.valid = true;
  return true;
}

uint8_t meshKeysInitAll() {
  uint8_t validCount = 0;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    Settings::MeshIdentity& m = gSettings.meshes[i];
    if (!m.enabled || m.label.length() == 0) continue;

    // Stretch lazily — only if we haven't cached a stretched key yet.
    if (!m.passphraseStretchedKeyValid && m.passphrase.length() > 0) {
      meshKeysStretchPassphrase(i);
    }
    if (meshKeysDerive(i)) validCount++;
  }
  return validCount;
}

void meshKeysInvalidate(uint8_t meshIdx) {
  if (meshIdx >= Settings::N_MESHES) return;
  MeshDerivedKeys& dk = gMeshDerivedKeys[meshIdx];
  sodium_memzero(&dk, sizeof(dk));
}

const MeshDerivedKeys* meshKeysGet(uint8_t meshIdx) {
  if (meshIdx >= Settings::N_MESHES) return nullptr;
  const MeshDerivedKeys& dk = gMeshDerivedKeys[meshIdx];
  return dk.valid ? &dk : nullptr;
}

const MeshDerivedKeys* meshKeysFindByFingerprint(uint16_t fingerprint) {
  if (fingerprint == 0) return nullptr;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    const Settings::MeshIdentity& m = gSettings.meshes[i];
    if (!m.enabled) continue;
    const MeshDerivedKeys& dk = gMeshDerivedKeys[i];
    if (dk.valid && dk.fingerprint == fingerprint) return &dk;
  }
  return nullptr;
}

#endif  // ENABLE_ESPNOW
