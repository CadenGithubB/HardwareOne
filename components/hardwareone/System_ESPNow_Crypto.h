#ifndef SYSTEM_ESPNOW_CRYPTO_H
#define SYSTEM_ESPNOW_CRYPTO_H

// ============================================================================
// ESP-NOW V4 Phase 3 — crypto primitive wrappers (thin libsodium adapters).
//
// 3.0 surface is intentionally narrow: init, randomness, Ed25519 keygen.
// The rest of the primitives (PBKDF2, KDF-Blake2b, X25519, AEAD) land in
// 3.1 / 3.4 / 3.5 as they're needed. Keeping declarations here so callers
// always include one header.
//
// All wrappers are thin: no logging, no caching, no error transforms beyond
// returning bool. The point of this layer is to keep #include <sodium.h>
// out of the rest of the codebase so libsodium's namespace doesn't leak.
// ============================================================================

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stddef.h>
#include <stdint.h>

// Initialize libsodium. Idempotent — safe to call multiple times. Must be
// called once at boot before any other espnowCrypto* call. Returns false
// only on hard libsodium init failure (vanishingly rare; effectively means
// the OS RNG is unavailable).
bool espnowCryptoInit();

// Whether sodium_init() has completed successfully. Other modules can
// gate work on this rather than tracking it themselves.
bool espnowCryptoReady();

// Fill `out` with `n` cryptographically secure random bytes. Wraps
// randombytes_buf. Always succeeds; on the rare RNG failure libsodium
// abort()s before returning.
void espnowCryptoRandomBytes(uint8_t* out, size_t n);

// Generate an Ed25519 long-term identity keypair.
//   pub: 32 bytes (crypto_sign_PUBLICKEYBYTES)
//   sec: 64 bytes (crypto_sign_SECRETKEYBYTES — includes pub in tail half)
// Returns false only if espnowCryptoInit hasn't been called or libsodium
// errors internally; callers should treat failure as fatal.
bool espnowCryptoEd25519Keygen(uint8_t pub[32], uint8_t sec[64]);

// ---------- Phase 3.1 primitives ----------

// PBKDF2-HMAC-SHA256, exactly one 32-byte output block. Hand-rolled loop on
// top of mbedtls's HMAC-SHA256 primitive — mbedtls is hooked into the ESP32
// hardware SHA accelerator (CONFIG_MBEDTLS_HARDWARE_SHA=y), which makes each
// HMAC ~10× faster than libsodium's software-only path. Hand-rolling rather
// than calling mbedtls_pkcs5_pbkdf2_hmac_ext lets us yield to FreeRTOS every
// 1024 iters so the Task WDT can't fire on long stretches.
//
// Empirical cost on ESP32 @ 240 MHz: ~10 µs/iter → 100 000 iters ≈ 1 s. Call
// once per mesh at boot (if no cached hash exists) and on every passphrase
// change. Result is the stretched value that downstream KDFs key off of —
// it must never be sent on the wire, but is safe to store at rest in
// plaintext (irreversible).
//
// Returns false only if espnowCryptoInit hasn't been called.
bool espnowCryptoPbkdf2HmacSha256(uint8_t out[32],
                                  const uint8_t* password, size_t passwordLen,
                                  const uint8_t* salt, size_t saltLen,
                                  uint32_t iters);

// One-shot HMAC-SHA256 over up to 3 input chunks (so callers can compute
// HMAC over concatenated fields without pre-allocating a contiguous buffer).
// Pass nullptr/0 for unused chunks. Uses mbedtls (hardware SHA accel).
// Returns false only if espnowCryptoInit hasn't been called.
bool espnowCryptoHmacSha256(uint8_t out[32],
                            const uint8_t* key, size_t keyLen,
                            const uint8_t* in1, size_t len1,
                            const uint8_t* in2, size_t len2,
                            const uint8_t* in3, size_t len3);

// ---------- Phase 3.4 primitives ----------

// Ed25519 signature operations against the long-term identity keypair.
// `sec` is the 64-byte libsodium secret-key blob (pub in tail half).
//   message:    arbitrary length, passed by pointer + length
//   sigOut:     64 bytes (crypto_sign_BYTES)
//   pub:        32 bytes — verifier-side key (typically a peer's stored pub)
// Returns false on init/library failure (sign) or signature mismatch (verify).
bool espnowCryptoEd25519Sign  (uint8_t sigOut[64],
                               const uint8_t* message, size_t messageLen,
                               const uint8_t sec[64]);
bool espnowCryptoEd25519Verify(const uint8_t sig[64],
                               const uint8_t* message, size_t messageLen,
                               const uint8_t pub[32]);

// X25519 ephemeral keypair generation. Each call produces a fresh keypair —
// the secret should be wiped immediately after the shared-secret derivation.
//   pub: 32 bytes
//   sec: 32 bytes
// Returns false if libsodium init incomplete.
bool espnowCryptoX25519Keygen(uint8_t pub[32], uint8_t sec[32]);

// X25519 ECDH: compute the 32-byte shared secret from our private + peer's
// public. Result is NOT a final session key; feed into HKDF / Blake2b KDF
// with distinct contexts to derive per-direction AEAD keys.
//   sharedOut:  32 bytes
//   ourSec:     our X25519 private (32 bytes)
//   theirPub:   their X25519 public (32 bytes)
// Returns false on libsodium error (e.g. small-subgroup / all-zero peer pub
// triggers libsodium's safety check).
bool espnowCryptoX25519Shared(uint8_t sharedOut[32],
                              const uint8_t ourSec[32],
                              const uint8_t theirPub[32]);

// ---------- Phase 3.5 primitives ----------

// ChaCha20-Poly1305 IETF AEAD seal/open. Tag is detached (16 bytes), kept
// separate from the ciphertext so callers can place it wherever the wire
// format dictates without an extra copy.
//
// Nonce: exactly 12 bytes (crypto_aead_chacha20poly1305_IETF_NPUBBYTES).
// Key:   exactly 32 bytes.
// AAD:   may be nullptr/0; included in the tag's MAC binding.
//
// Returns false on encrypt only if libsodium init failed; on decrypt false
// means tag verification failed (i.e. ciphertext tampered with or wrong key).
bool espnowCryptoAeadSeal(uint8_t* cipherOut, uint8_t tagOut[16],
                          const uint8_t* plaintext, size_t plaintextLen,
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t nonce[12],
                          const uint8_t key[32]);

bool espnowCryptoAeadOpen(uint8_t* plaintextOut,
                          const uint8_t* cipher, size_t cipherLen,
                          const uint8_t tag[16],
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t nonce[12],
                          const uint8_t key[32]);

// Blake2b KDF subkey derivation. Wraps crypto_kdf_blake2b_derive_from_key.
//   masterKey:  32-byte key (e.g. mesh stretched hash, or X25519 shared secret in 3.4)
//   subkeyId:   arbitrary uint64 — different IDs from the same master yield
//               independent subkeys (forward independent of each other)
//   context:    8-byte ASCII tag, e.g. "esp-boot"; differentiates KDF domains
//   out:        32-byte derived subkey
//
// The 8-byte context length is libsodium's crypto_kdf_blake2b_CONTEXTBYTES.
// Contexts shorter than 8 chars are zero-padded by libsodium; longer is
// rejected by this wrapper.
//
// Returns false only if espnowCryptoInit hasn't been called or `context` is
// longer than 8 chars.
bool espnowCryptoKdfSubkey(uint8_t out[32],
                           const uint8_t masterKey[32],
                           uint64_t subkeyId,
                           const char* context);

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_CRYPTO_H
