#ifndef SYSTEM_ESPNOW_MESHKEYS_H
#define SYSTEM_ESPNOW_MESHKEYS_H

// ============================================================================
// ESP-NOW V4 Phase 3.1 — per-mesh key derivation cache.
//
// Each mesh has a long-lived PBKDF2-stretched key (lives in
// gSettings.meshes[i].passphraseStretchedKey; persisted across boots). From
// that one stretched value, two purpose-specific subkeys are derived via
// Blake2b KDF and cached in RAM for the lifetime of the boot:
//
//   bootstrapKey  = KDF(stretchedKey, ctx="esp-boot")  — HMAC key for the
//                                                         KEY_EX_* handshake
//                                                         in Phase 3.3.
//   groupKey      = KDF(stretchedKey, ctx="esp-grup")  — AEAD key for
//                                                         BROADCAST_AUTH
//                                                         frames in Phase 3.5.
//
// Phase 3.1 only generates and caches; nothing on the wire consumes these
// yet. The values exist so that subsequent sub-phases can land smaller
// commits that just wire up the handshake / broadcast paths.
//
// Boot rule:
//   - If a mesh has a passphrase but no cached stretched key on disk, stretch
//     it at boot (~1-2 s blocking per mesh) and persist. One-time cost per
//     mesh per firmware version.
//   - Re-derive the in-RAM subkeys whenever the stretched key changes
//     (boot, passphrase set, mesh rename).
//
// Everything below requires espnowCryptoInit() to have completed.
// ============================================================================

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stddef.h>
#include <stdint.h>

struct MeshDerivedKeys {
  bool     valid;
  uint16_t fingerprint;        // mirrors gSettings.meshes[i].fingerprint at
                               //   the time the keys were derived; stale
                               //   if a rename happens without re-derivation
  uint8_t  bootstrapKey[32];   // KDF subkey for KEY_EX HMAC (Phase 3.3)
  uint8_t  groupKey[32];       // KDF subkey for mesh broadcast AEAD (Phase 3.5)
};

// One-shot stretcher: compute the PBKDF2-stretched key for mesh slot `meshIdx` from
// its current passphrase, write into gSettings.meshes[meshIdx], mark valid.
// Caller is responsible for persisting settings afterward.
//
// Cost: ~1-2 seconds on ESP32 @ 240 MHz with 100k iterations. Call from a
// non-realtime context (boot, CLI handler). Skips work if passphrase is
// empty (clears the key & marks invalid instead).
//
// Returns true if the key is valid after the call (including the empty-
// passphrase clear case).
bool meshKeysStretchPassphrase(uint8_t meshIdx);

// Derive bootstrapKey + groupKey from the cached stretched key into
// gMeshDerivedKeys[meshIdx]. No-op if the mesh has no valid key.
bool meshKeysDerive(uint8_t meshIdx);

// Convenience: stretch (if needed) AND derive for every configured mesh.
// Returns the number of meshes that ended up with valid derived keys.
// Called once at boot, after identity init.
uint8_t meshKeysInitAll();

// Invalidate the derived keys for a mesh — e.g. after passphrase change.
// Doesn't touch the persisted stretched key; caller is responsible for
// calling stretch + derive afterward.
void meshKeysInvalidate(uint8_t meshIdx);

// Read-only accessor. Returns nullptr if meshIdx out of range or no valid
// keys. Lifetime is global; safe to hold the pointer for the life of the
// process (but contents may be overwritten if meshKeysDerive runs again).
const MeshDerivedKeys* meshKeysGet(uint8_t meshIdx);

// Look up by fingerprint (V4 header field). Returns nullptr if no enabled
// mesh matches. Useful for RX-side dispatch in 3.3 / 3.5.
const MeshDerivedKeys* meshKeysFindByFingerprint(uint16_t fingerprint);

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_MESHKEYS_H
