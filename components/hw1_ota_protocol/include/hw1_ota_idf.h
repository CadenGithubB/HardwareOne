#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "hw1_ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The chip id an acceptable candidate image must carry.
 *
 * Both the main application (System_OTA.cpp) and the recovery updater
 * (updater_preflight.c) reject a staged image whose esp_image_header_t names a
 * different chip. That check used to be a literal ESP_CHIP_ID_ESP32S3 in each
 * file, which silently became wrong the moment a second chip family joined the
 * fleet: a classic-ESP32 build would have refused its OWN correctly-built
 * image, and the failure reads as a corrupt download rather than a mis-ported
 * constant.
 *
 * Deriving it from the build target keeps the two call sites honest and makes
 * adding a chip family a compile error here rather than a runtime rejection in
 * the field. HW1_OTA_EXPECTED_CHIP_NAME exists so the operator-facing message
 * names the chip the firmware actually wants.
 *
 * This lives in the IDF-side header on purpose: hw1_ota_protocol.h is
 * deliberately ESP-IDF-free so the wire format stays host-testable.
 */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define HW1_OTA_EXPECTED_CHIP_ID ESP_CHIP_ID_ESP32S3
#define HW1_OTA_EXPECTED_CHIP_NAME "ESP32-S3"
#elif defined(CONFIG_IDF_TARGET_ESP32)
#define HW1_OTA_EXPECTED_CHIP_ID ESP_CHIP_ID_ESP32
#define HW1_OTA_EXPECTED_CHIP_NAME "ESP32"
#else
#error "Recovery OTA has no expected chip id for this IDF target. Add it here, \
        add the board to the CMake registry, and add matching rows to \
        tools/ota/check_ota_builds.py before building an OTA image."
#endif

typedef struct {
    /* PEM buffer must include its terminating NUL byte. Ownership stays with caller. */
    const uint8_t *public_key_pem;
    size_t public_key_pem_size;
} hw1_ota_idf_rsa_public_key_t;

/*
 * Ready-to-pass hw1_ota_manifest_verify_fn implementation. It parses the
 * caller-owned PEM public key and enforces RSA-3072, PSS, SHA-256,
 * MGF1-SHA-256, a 32-byte salt, a 224-byte payload and a 384-byte signature.
 */
hw1_ota_status_t hw1_ota_idf_rsa3072_pss_sha256_verify(
    void *context,
    const hw1_ota_signature_spec_t *spec,
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *signature,
    size_t signature_size);

/* SHA-256 helpers use ESP-IDF's configured mbedTLS implementation. */
esp_err_t hw1_ota_idf_sha256_bytes(
    const void *data,
    size_t size,
    uint8_t digest[HW1_OTA_SHA256_SIZE]);
esp_err_t hw1_ota_idf_sha256_partition(
    const esp_partition_t *partition,
    size_t image_size,
    uint8_t digest[HW1_OTA_SHA256_SIZE]);

/*
 * Verifies the ESP image with esp_image_verify() (including its app signature
 * when signed-app verification is enabled), hashes exactly manifest.imageSize
 * plaintext bytes from the partition, then applies manifest identity policy.
 * Detached-manifest authentication must already have produced
 * verified_manifest; this function never treats parsed-only metadata as trusted.
 */
esp_err_t hw1_ota_idf_verify_partition(
    const esp_partition_t *partition,
    const hw1_ota_verified_manifest_t *verified_manifest,
    const hw1_ota_target_policy_t *policy,
    uint32_t *manifest_mismatches);

#ifdef __cplusplus
}
#endif
