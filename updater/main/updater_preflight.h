#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "hw1_ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HW1_UPDATER_STAGED_IMAGE_PATH "/littlefs/system/ota/candidate.bin"
#define HW1_UPDATER_STAGED_MANIFEST_PATH "/littlefs/system/ota/manifest.json"
#define HW1_UPDATER_OTA0_SIZE 0x5A0000u
#define HW1_UPDATER_DATA_SCHEMA 1u
#define HW1_UPDATER_IMAGE_PREFIX_SIZE (24u + 8u + sizeof(esp_app_desc_t))

typedef struct {
    hw1_ota_verified_manifest_t verified;
    esp_app_desc_t app_desc;
    uint8_t image_sha256[HW1_OTA_SHA256_SIZE];
    uint32_t image_size;
    FILE *image;
} updater_candidate_t;

esp_err_t updater_validate_layout(const esp_partition_t **ota0,
                                  char *reason, size_t reason_size);
esp_err_t updater_validate_existing_ota0(const esp_partition_t *ota0,
                                         bool *valid,
                                         esp_ota_img_states_t *state);

esp_err_t updater_verify_manifest_envelope(
    const uint8_t *json, size_t json_size,
    hw1_ota_verified_manifest_t *verified,
    char *reason, size_t reason_size);
esp_err_t updater_validate_manifest_contract(
    const hw1_ota_verified_manifest_t *verified,
    uint32_t observed_size,
    const uint8_t observed_sha256[HW1_OTA_SHA256_SIZE],
    char *reason, size_t reason_size);
esp_err_t updater_validate_image_prefix(
    const uint8_t *prefix, size_t prefix_size,
    const hw1_ota_verified_manifest_t *verified,
    esp_app_desc_t *app_desc,
    char *reason, size_t reason_size);
esp_err_t updater_validate_not_downgrade(
    const esp_partition_t *ota0,
    const hw1_ota_verified_manifest_t *verified,
    bool allow_downgrade,
    char *reason, size_t reason_size);

esp_err_t updater_open_staged_candidate(
    const esp_partition_t *ota0,
    bool allow_downgrade,
    updater_candidate_t *candidate,
    char *reason, size_t reason_size);
void updater_close_candidate(updater_candidate_t *candidate);

esp_err_t updater_verify_written_candidate(
    const esp_partition_t *ota0,
    const hw1_ota_verified_manifest_t *verified,
    char *reason, size_t reason_size);

#ifdef __cplusplus
}
#endif
