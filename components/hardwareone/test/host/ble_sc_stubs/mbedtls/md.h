#pragma once
// Declarations only; ble_sc_directed_harness.cpp defines the doubles.
#include <cstddef>
struct mbedtls_md_info_t { int unused; };
struct mbedtls_md_context_t { int unused; };
enum mbedtls_md_type_t { MBEDTLS_MD_SHA256 = 1 };
const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t type);
void mbedtls_md_init(mbedtls_md_context_t* ctx);
int mbedtls_md_setup(mbedtls_md_context_t* ctx, const mbedtls_md_info_t* info, int hmac);
void mbedtls_md_free(mbedtls_md_context_t* ctx);
int mbedtls_md_hmac_starts(mbedtls_md_context_t* ctx, const unsigned char* key, size_t keylen);
int mbedtls_md_hmac_update(mbedtls_md_context_t* ctx, const unsigned char* in, size_t ilen);
int mbedtls_md_hmac_finish(mbedtls_md_context_t* ctx, unsigned char* out);
int mbedtls_md_hmac_reset(mbedtls_md_context_t* ctx);
