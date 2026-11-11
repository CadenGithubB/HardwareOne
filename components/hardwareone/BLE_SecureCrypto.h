#pragma once
// Shared wire-compatible cryptographic primitives. No GATT role or settings state.
#include <sodium.h>
#include <mbedtls/md.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdint>
namespace hw1sc {
// ----- HMAC-SHA256 (arbitrary key length via libsodium multipart) -----
inline void scHmac(const uint8_t* key, size_t keyLen,
                   const uint8_t* msg, size_t msgLen, uint8_t out[32]) {
  crypto_auth_hmacsha256_state st;
  crypto_auth_hmacsha256_init(&st, key, keyLen);
  crypto_auth_hmacsha256_update(&st, msg, msgLen);
  crypto_auth_hmacsha256_final(&st, out);
}

// ----- HKDF-SHA256 (RFC 5869), outLen <= 64 (we only need 64) -----
inline void scHkdf(const uint8_t* ikm, size_t ikmLen,
                   const uint8_t* salt, size_t saltLen,
                   const char* info, size_t infoLen,
                   uint8_t* out, size_t outLen) {
  uint8_t prk[32];
  scHmac(salt, saltLen, ikm, ikmLen, prk);            // Extract
  uint8_t t[32];
  size_t  tLen = 0;
  size_t  done = 0;
  uint8_t counter = 1;
  while (done < outLen) {
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, prk, sizeof(prk));
    if (tLen) crypto_auth_hmacsha256_update(&st, t, tLen);
    crypto_auth_hmacsha256_update(&st, (const uint8_t*)info, infoLen);
    crypto_auth_hmacsha256_update(&st, &counter, 1);
    crypto_auth_hmacsha256_final(&st, t);
    tLen = 32;
    size_t n = (outLen - done < 32) ? (outLen - done) : 32;
    memcpy(out + done, t, n);
    done += n;
    counter++;
  }
  sodium_memzero(prk, sizeof(prk));
  sodium_memzero(t, sizeof(t));
}

// ----- PBKDF2-HMAC-SHA256, 32-byte output -----
// The stock mbedtls_pkcs5_pbkdf2_hmac_ext call is one monolithic CPU stretch;
// on the S3, 100k rounds can starve IDLE0 long enough to trip Task WDT. This is
// the same RFC 2898 single-block calculation, but yields every 4096 rounds.
// The inner HMAC still uses ESP-IDF's mbedTLS SHA-256 implementation, so the
// Android PBKDF2 test vector and wire keys remain byte-for-byte unchanged.
inline bool scPbkdf2(const uint8_t* pw, size_t pwLen,
                     const uint8_t* salt, size_t saltLen,
                     uint32_t iters, uint8_t out[32]) {
  if (!pw || !salt || !out || iters == 0) return false;
  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!mdInfo) return false;

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, mdInfo, 1) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }

  uint8_t u[32];
  const uint8_t blockIndex[4] = {0, 0, 0, 1};
  bool ok = false;
  if (mbedtls_md_hmac_starts(&ctx, pw, pwLen) != 0) goto done;
  if (mbedtls_md_hmac_update(&ctx, salt, saltLen) != 0) goto done;
  if (mbedtls_md_hmac_update(&ctx, blockIndex, sizeof(blockIndex)) != 0) goto done;
  if (mbedtls_md_hmac_finish(&ctx, u) != 0) goto done;
  memcpy(out, u, sizeof(u));

  for (uint32_t i = 1; i < iters; ++i) {
    if (mbedtls_md_hmac_reset(&ctx) != 0) goto done;
    if (mbedtls_md_hmac_update(&ctx, u, sizeof(u)) != 0) goto done;
    if (mbedtls_md_hmac_finish(&ctx, u) != 0) goto done;
    for (size_t j = 0; j < sizeof(u); ++j) out[j] ^= u[j];
    if ((i & 0xFFFU) == 0) vTaskDelay(1);
  }
  ok = true;

done:
  mbedtls_md_free(&ctx);
  sodium_memzero(u, sizeof(u));
  if (!ok) sodium_memzero(out, 32);
  return ok;
}

inline void scNonce(uint8_t out[12], uint32_t dirTag, uint64_t ctr) {
  out[0] = (dirTag >> 24) & 0xFF; out[1] = (dirTag >> 16) & 0xFF;
  out[2] = (dirTag >> 8) & 0xFF;  out[3] = dirTag & 0xFF;
  for (int i = 0; i < 8; i++) out[4 + i] = (ctr >> (8 * (7 - i))) & 0xFF;
}

} // namespace hw1sc
