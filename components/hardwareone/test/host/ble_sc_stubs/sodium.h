#pragma once
// Declarations only; ble_sc_directed_harness.cpp defines routing-neutral doubles.
#include <cstddef>
#include <cstdint>
struct crypto_auth_hmacsha256_state { int unused; };
int sodium_init();
void sodium_memzero(void* p, size_t n);
void randombytes_buf(void* p, size_t n);
int crypto_kx_keypair(unsigned char* pk, unsigned char* sk);
int crypto_scalarmult(unsigned char* q, const unsigned char* n, const unsigned char* p);
int crypto_auth_hmacsha256_init(crypto_auth_hmacsha256_state* st, const unsigned char* key, size_t keylen);
int crypto_auth_hmacsha256_update(crypto_auth_hmacsha256_state* st, const unsigned char* in,
                                  unsigned long long inlen);
int crypto_auth_hmacsha256_final(crypto_auth_hmacsha256_state* st, unsigned char* out);
int crypto_aead_chacha20poly1305_ietf_encrypt_detached(
    unsigned char* c, unsigned char* mac, unsigned long long* maclen_p, const unsigned char* m,
    unsigned long long mlen, const unsigned char* ad, unsigned long long adlen,
    const unsigned char* nsec, const unsigned char* npub, const unsigned char* k);
int crypto_aead_chacha20poly1305_ietf_decrypt_detached(
    unsigned char* m, unsigned char* nsec, const unsigned char* c, unsigned long long clen,
    const unsigned char* mac, const unsigned char* ad, unsigned long long adlen,
    const unsigned char* npub, const unsigned char* k);
