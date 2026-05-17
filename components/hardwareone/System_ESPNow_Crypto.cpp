#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Crypto.h"

#include <string.h>
#include <sodium.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// mbedtls — used specifically for PBKDF2's inner HMAC-SHA256. ESP-IDF's
// mbedtls port hooks SHA-256 into the ESP32's hardware SHA accelerator
// (CONFIG_MBEDTLS_HARDWARE_SHA=y in sdkconfig), which is ~10× faster than
// libsodium's pure-software impl. Libsodium is still the right choice for
// Ed25519 / X25519 / AEAD where mbedtls's coverage is weaker.
#include <mbedtls/md.h>

namespace {
bool gSodiumReady = false;
}

bool espnowCryptoInit() {
  if (gSodiumReady) return true;
  // sodium_init returns:
  //    0  — first-time success
  //    1  — already initialized (treat as success)
  //   -1  — fatal (e.g., RNG unavailable)
  int rc = sodium_init();
  if (rc < 0) return false;
  gSodiumReady = true;
  return true;
}

bool espnowCryptoReady() {
  return gSodiumReady;
}

void espnowCryptoRandomBytes(uint8_t* out, size_t n) {
  randombytes_buf(out, n);
}

bool espnowCryptoEd25519Keygen(uint8_t pub[32], uint8_t sec[64]) {
  if (!gSodiumReady) return false;
  return crypto_sign_keypair(pub, sec) == 0;
}

// ---------- Phase 3.1 ----------

bool espnowCryptoPbkdf2HmacSha256(uint8_t out[32],
                                  const uint8_t* password, size_t passwordLen,
                                  const uint8_t* salt, size_t saltLen,
                                  uint32_t iters) {
  if (iters == 0) return false;

  // PBKDF2 RFC 2898, single output block (32 bytes = SHA-256 output size).
  //   U_1 = HMAC(P, S || INT32_BE(1))
  //   U_i = HMAC(P, U_{i-1})
  //   T   = U_1 XOR U_2 XOR ... XOR U_iters
  //
  // We hand-roll the loop rather than calling mbedtls_pkcs5_pbkdf2_hmac_ext
  // so we can yield to FreeRTOS periodically and prevent the Task WDT firing
  // on long stretches. The inner HMAC primitive is mbedtls's, which on ESP-IDF
  // is hooked into the ESP32 hardware SHA-256 accelerator — ~10× faster than
  // libsodium's pure-software HMAC.

  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!mdInfo) return false;

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  // setup(..., is_hmac=1) allocates the inner+outer pad keys for HMAC.
  if (mbedtls_md_setup(&ctx, mdInfo, 1) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }

  uint8_t u[32];
  const uint8_t blockIdx[4] = { 0, 0, 0, 1 };
  bool ok = false;

  // Iteration 1: HMAC over salt || INT32_BE(1).
  if (mbedtls_md_hmac_starts(&ctx, password, passwordLen) != 0) goto done;
  if (mbedtls_md_hmac_update(&ctx, salt, saltLen) != 0)         goto done;
  if (mbedtls_md_hmac_update(&ctx, blockIdx, 4) != 0)           goto done;
  if (mbedtls_md_hmac_finish(&ctx, u) != 0)                     goto done;
  memcpy(out, u, 32);

  // Iterations 2..N: HMAC over previous U, XOR into accumulator. hmac_reset
  // reuses the already-keyed context — no re-keying cost per iteration.
  for (uint32_t i = 1; i < iters; i++) {
    if (mbedtls_md_hmac_reset(&ctx) != 0)            goto done;
    if (mbedtls_md_hmac_update(&ctx, u, 32) != 0)    goto done;
    if (mbedtls_md_hmac_finish(&ctx, u) != 0)        goto done;
    for (int j = 0; j < 32; j++) out[j] ^= u[j];
    // Yield every 4096 iters (≈ 24 yields × ~10ms tick in a 100k stretch =
    // ~240ms overhead). Frequent enough to keep IDLE0 happy and prevent
    // Task WDT, sparse enough that vTaskDelay overhead doesn't dominate.
    if ((i & 0xFFF) == 0) vTaskDelay(1);
  }
  ok = true;

done:
  mbedtls_md_free(&ctx);
  sodium_memzero(u, sizeof(u));
  return ok;
}

bool espnowCryptoHmacSha256(uint8_t out[32],
                            const uint8_t* key, size_t keyLen,
                            const uint8_t* in1, size_t len1,
                            const uint8_t* in2, size_t len2,
                            const uint8_t* in3, size_t len3) {
  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!mdInfo) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = false;
  if (mbedtls_md_setup(&ctx, mdInfo, 1) != 0)               goto done;
  if (mbedtls_md_hmac_starts(&ctx, key, keyLen) != 0)       goto done;
  if (in1 && len1 && mbedtls_md_hmac_update(&ctx, in1, len1) != 0) goto done;
  if (in2 && len2 && mbedtls_md_hmac_update(&ctx, in2, len2) != 0) goto done;
  if (in3 && len3 && mbedtls_md_hmac_update(&ctx, in3, len3) != 0) goto done;
  if (mbedtls_md_hmac_finish(&ctx, out) != 0)               goto done;
  ok = true;
done:
  mbedtls_md_free(&ctx);
  return ok;
}

bool espnowCryptoEd25519Sign(uint8_t sigOut[64],
                             const uint8_t* message, size_t messageLen,
                             const uint8_t sec[64]) {
  if (!gSodiumReady) return false;
  unsigned long long sigLen = 0;
  int rc = crypto_sign_detached(sigOut, &sigLen, message, messageLen, sec);
  return rc == 0 && sigLen == 64;
}

bool espnowCryptoEd25519Verify(const uint8_t sig[64],
                               const uint8_t* message, size_t messageLen,
                               const uint8_t pub[32]) {
  if (!gSodiumReady) return false;
  return crypto_sign_verify_detached(sig, message, messageLen, pub) == 0;
}

bool espnowCryptoX25519Keygen(uint8_t pub[32], uint8_t sec[32]) {
  if (!gSodiumReady) return false;
  // libsodium has crypto_kx_keypair which generates X25519 keys for the
  // crypto_kx KX API; the resulting keys are valid Curve25519 keys we can
  // use directly with crypto_scalarmult.
  return crypto_kx_keypair(pub, sec) == 0;
}

bool espnowCryptoX25519Shared(uint8_t sharedOut[32],
                              const uint8_t ourSec[32],
                              const uint8_t theirPub[32]) {
  if (!gSodiumReady) return false;
  // crypto_scalarmult rejects all-zero / small-subgroup pubkeys; treat any
  // non-zero return as failure (caller should abandon the session).
  return crypto_scalarmult(sharedOut, ourSec, theirPub) == 0;
}

bool espnowCryptoAeadSeal(uint8_t* cipherOut, uint8_t tagOut[16],
                          const uint8_t* plaintext, size_t plaintextLen,
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t nonce[12],
                          const uint8_t key[32]) {
  if (!gSodiumReady) return false;
  unsigned long long tagLen = 0;
  int rc = crypto_aead_chacha20poly1305_ietf_encrypt_detached(
      cipherOut, tagOut, &tagLen,
      plaintext, plaintextLen,
      aad, aadLen,
      nullptr,  // unused (NSEC)
      nonce, key);
  return rc == 0 && tagLen == 16;
}

bool espnowCryptoAeadOpen(uint8_t* plaintextOut,
                          const uint8_t* cipher, size_t cipherLen,
                          const uint8_t tag[16],
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t nonce[12],
                          const uint8_t key[32]) {
  if (!gSodiumReady) return false;
  int rc = crypto_aead_chacha20poly1305_ietf_decrypt_detached(
      plaintextOut, nullptr /* unused NSEC */,
      cipher, cipherLen,
      tag,
      aad, aadLen,
      nonce, key);
  return rc == 0;
}

bool espnowCryptoKdfSubkey(uint8_t out[32],
                           const uint8_t masterKey[32],
                           uint64_t subkeyId,
                           const char* context) {
  if (!gSodiumReady) return false;
  if (!context) return false;

  // libsodium's crypto_kdf_blake2b context is exactly 8 bytes; shorter inputs
  // we zero-pad ourselves to make the boundary explicit. Longer = rejected.
  size_t ctxLen = strlen(context);
  if (ctxLen > crypto_kdf_blake2b_CONTEXTBYTES) return false;

  char ctxBuf[crypto_kdf_blake2b_CONTEXTBYTES] = {0};
  memcpy(ctxBuf, context, ctxLen);

  int rc = crypto_kdf_blake2b_derive_from_key(out, 32, subkeyId, ctxBuf, masterKey);
  return rc == 0;
}

#endif  // ENABLE_ESPNOW
