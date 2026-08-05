#pragma once

#include "esp_err.h"
#include "hw1_ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HW1_OTA_NVS_NAMESPACE "hw1up"
#define HW1_OTA_NVS_SLOT_A_KEY "tx_a"
#define HW1_OTA_NVS_SLOT_B_KEY "tx_b"

typedef struct {
    uint8_t valid_slots;   /* bit 0 = A, bit 1 = B */
    uint8_t corrupt_slots; /* bit 0 = A, bit 1 = B */
    uint8_t selected_slot; /* 0 = A, 1 = B */
    uint32_t sequence;
} hw1_ota_nvs_info_t;

/* These functions never initialize or erase NVS. The application owns that policy. */
esp_err_t hw1_ota_nvs_load(
    hw1_ota_record_t *record,
    hw1_ota_nvs_info_t *info);
esp_err_t hw1_ota_nvs_load_from_partition(
    const char *partition_name,
    hw1_ota_record_t *record,
    hw1_ota_nvs_info_t *info);

/*
 * Atomically writes the inactive slot and leaves the previous valid slot as a
 * fallback. expected_sequence is normally the sequence that was loaded (zero
 * for a new store), or HW1_OTA_SEQUENCE_ANY for an explicitly unconditional
 * write. stored receives the committed sequence.
 */
esp_err_t hw1_ota_nvs_commit(
    const hw1_ota_record_t *desired,
    uint32_t expected_sequence,
    hw1_ota_record_t *stored,
    hw1_ota_nvs_info_t *info);
esp_err_t hw1_ota_nvs_commit_to_partition(
    const char *partition_name,
    const hw1_ota_record_t *desired,
    uint32_t expected_sequence,
    hw1_ota_record_t *stored,
    hw1_ota_nvs_info_t *info);

#ifdef __cplusplus
}
#endif
