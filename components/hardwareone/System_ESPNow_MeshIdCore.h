#ifndef SYSTEM_ESPNOW_MESH_ID_CORE_H
#define SYSTEM_ESPNOW_MESH_ID_CORE_H

// ============================================================================
// The mesh id: the 16-bit value every frame carries in the V4 header's
// meshFingerprint field (bytes 22-23), which the receiver's mesh gate matches
// against its own meshes.
//
// Derivation (a cross-device contract; docs/COMPATIBILITY.md states it too):
//   stretched = PBKDF2-HMAC-SHA256(passphrase, SHA256(kSaltPrefix || label), kPbkdf2Iters)
//               with kSaltPrefix "hw1-espnow-1-mesh-salt:" and 100000 rounds (kdf record 1)
//   sub       = crypto_kdf_blake2b_derive_from_key(32 bytes, subkey id 3, context "esp-mid1", stretched)
//   id        = pick(sub): the first nonzero little-endian u16 of sub's 16 byte pairs
// All 32 bytes zero gives 0, "no id" (probability 2^-256; logged).
//
// Rules:
//   - A mesh that never had a passphrase has no id (0) and sends nothing.
//   - The id is never 0 on the air; the receiver drops a frame carrying 0.
//   - Clearing a mesh's passphrase disables the mesh and drops its id (1.0,
//     E2): a mesh without a passphrase sends and takes nothing. A new
//     passphrase or label gives a new id.
//   - Ids are unique among this device's meshes: a command whose new id another
//     mesh here already has is refused and changes nothing (clashSlot).
//
// Pure header: the host tests compile it directly.
// ============================================================================

#include <stddef.h>
#include <stdint.h>

namespace meshid {

// The stretch (a cross-device contract, like the id). kKdfRecord is stored as
// "kdf" next to the cached stretched key in the settings file; a cached key
// with another record (or none) is not used: the passphrase is stretched again.
constexpr const char* kSaltPrefix = "hw1-espnow-1-mesh-salt:";
constexpr uint32_t kPbkdf2Iters = 100000;
constexpr int kKdfRecord = 1;

constexpr uint64_t kSubkeyId = 3;
constexpr const char* kKdfContext = "esp-mid1";  // exactly 8 bytes (libsodium KDF context)

// The first nonzero little-endian u16 among the 16 byte pairs of `sub`; 0 when
// all 32 bytes are zero.
inline uint16_t pick(const uint8_t sub[32]) {
  if (!sub) return 0;
  for (int i = 0; i < 16; i++) {
    const uint16_t v = (uint16_t)(sub[2 * i] | ((uint16_t)sub[2 * i + 1] << 8));
    if (v != 0) return v;
  }
  return 0;
}

// The slot among `ids[0..n)` other than `self` that already has `cand`, or -1.
// 0 ("no id") never clashes.
inline int clashSlot(const uint16_t ids[], uint8_t n, uint8_t self, uint16_t cand) {
  if (cand == 0 || !ids) return -1;
  for (uint8_t i = 0; i < n; i++) {
    if (i != self && ids[i] == cand) return (int)i;
  }
  return -1;
}

}  // namespace meshid

#endif  // SYSTEM_ESPNOW_MESH_ID_CORE_H
