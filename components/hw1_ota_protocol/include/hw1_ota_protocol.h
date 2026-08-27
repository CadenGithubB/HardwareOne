/*
 * HardwareOne OTA protocol core.
 *
 * This header is deliberately ESP-IDF- and Arduino-free. Both the main app and
 * the recovery updater can share the exact wire format and state machine, and
 * the deterministic parts can be exercised by a host compiler.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HW1_OTA_RECORD_FORMAT_VERSION 1u
#define HW1_OTA_MANIFEST_FORMAT_VERSION 1u
#define HW1_OTA_MANIFEST_ENVELOPE_FORMAT "hardwareone-ota-envelope"
#define HW1_OTA_MANIFEST_SIGNATURE_ALGORITHM "rsa-pss-sha256"

#define HW1_OTA_BOARD_ID_SIZE 24u
#define HW1_OTA_LAYOUT_ID_SIZE 24u
#define HW1_OTA_PROJECT_NAME_SIZE 32u
#define HW1_OTA_VERSION_SIZE 48u
#define HW1_OTA_MIN_UPDATER_VERSION_SIZE 32u
#define HW1_OTA_SHA256_SIZE 32u
#define HW1_OTA_RESULT_DETAIL_SIZE 96u

#define HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE 224u
#define HW1_OTA_RECORD_WIRE_SIZE 384u
#define HW1_OTA_RSA3072_SIGNATURE_SIZE 384u
#define HW1_OTA_PSS_SALT_LENGTH 32u

/*
 * Required HardwareOne build-metadata identities, one row per recovery-OTA
 * board. The board id, layout id and version suffix travel together: the
 * manifest names all three and the device refuses an image that disagrees
 * about any of them, which is what stops a FeatherS3 image from being staged
 * onto a Feather V2 (or an 8 MB layout image onto a 16 MB one).
 *
 * Every id must fit HW1_OTA_BOARD_ID_SIZE / HW1_OTA_LAYOUT_ID_SIZE including
 * its NUL. Keep the CMake registry in the root CMakeLists.txt, the board
 * tables under tools/ota and these constants in step.
 */
#define HW1_OTA_BOARD_ID_FEATHERS3 "feathers3"
#define HW1_OTA_BOARD_ID_FEATHERS3_FLASH_ENCRYPTED "feathers3_fe"
#define HW1_OTA_LAYOUT_ID_FEATHERS3_OTA_V1 "hw1-f3-ota-v1"
#define HW1_OTA_LAYOUT_ID_FEATHERS3_FLASH_ENCRYPTED_OTA_V1 "hw1-f3fe-ota-v1"
#define HW1_OTA_VERSION_SUFFIX_FEATHERS3_PLAIN "+f3o1"
#define HW1_OTA_VERSION_SUFFIX_FEATHERS3_FLASH_ENCRYPTED "+f3feo1"

/* Adafruit Feather ESP32 V2 -- classic ESP32, 8 MB flash, no flash encryption. */
#define HW1_OTA_BOARD_ID_FEATHER_ESP32_V2 "feather_esp32_v2"
#define HW1_OTA_LAYOUT_ID_FEATHER_ESP32_V2_OTA_V1 "hw1-fv2-ota-v1"
#define HW1_OTA_VERSION_SUFFIX_FEATHER_ESP32_V2 "+fv2o1"

/* Adafruit QT Py ESP32 -- same die and flash size as the Feather V2, but no
 * Bluetooth and no battery hardware. Distinct ids all the same: the layout is
 * byte-identical, yet the IMAGES are not interchangeable, and the manifest
 * check is what stops one being staged onto the other. */
#define HW1_OTA_BOARD_ID_QTPY_ESP32 "qtpy_esp32"
#define HW1_OTA_LAYOUT_ID_QTPY_ESP32_OTA_V1 "hw1-qtpy-ota-v1"
#define HW1_OTA_VERSION_SUFFIX_QTPY_ESP32 "+qtpyo1"

/* Pass this expected sequence only when optimistic conflict checking is unwanted. */
#define HW1_OTA_SEQUENCE_ANY UINT32_MAX

typedef enum {
    HW1_OTA_OK = 0,
    HW1_OTA_ERR_INVALID_ARG = -1,
    HW1_OTA_ERR_CORRUPT = -2,
    HW1_OTA_ERR_UNSUPPORTED_VERSION = -3,
    HW1_OTA_ERR_INVALID_STATE = -4,
    HW1_OTA_ERR_INCOMPATIBLE = -5,
    HW1_OTA_ERR_AUTH_REQUIRED = -6,
    HW1_OTA_ERR_SIGNATURE_INVALID = -7,
    HW1_OTA_ERR_CONFLICT = -8,
    HW1_OTA_ERR_NOT_FOUND = -9,
    HW1_OTA_ERR_OVERFLOW = -10,
} hw1_ota_status_t;

/* The v1 detached signature contract is intentionally singular. */
typedef enum {
    HW1_OTA_SIGNATURE_NONE = 0,
    HW1_OTA_SIGNATURE_RSA_PSS_SHA256 = 1,
} hw1_ota_signature_algorithm_t;

typedef struct {
    hw1_ota_signature_algorithm_t algorithm;
    uint16_t rsa_key_bits;
    uint8_t digest_size;
    uint8_t pss_salt_length;
} hw1_ota_signature_spec_t;

/*
 * Canonical decoded v1 manifest payload. The signed payload is the exact
 * HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE bytes produced by
 * hw1_ota_manifest_encode(). Field names correspond to the release manifest's
 * formatVersion, boardId, layoutId, projectName, version, imageSize,
 * imageSha256, minUpdaterVersion, and dataSchema fields.
 */
typedef struct {
    uint16_t format_version;
    char board_id[HW1_OTA_BOARD_ID_SIZE];
    char layout_id[HW1_OTA_LAYOUT_ID_SIZE];
    char project_name[HW1_OTA_PROJECT_NAME_SIZE];
    char version[HW1_OTA_VERSION_SIZE];
    uint32_t image_size;
    uint8_t image_sha256[HW1_OTA_SHA256_SIZE];
    char min_updater_version[HW1_OTA_MIN_UPDATER_VERSION_SIZE];
    uint32_t data_schema;
} hw1_ota_manifest_t;

/*
 * A verifier must authenticate the exact payload bytes using RSA-3072-PSS,
 * SHA-256, MGF1-SHA-256 and a 32-byte salt. Public-key ownership and loading
 * are application policy, so the protocol component never embeds a key.
 */
typedef hw1_ota_status_t (*hw1_ota_manifest_verify_fn)(
    void *context,
    const hw1_ota_signature_spec_t *spec,
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *signature,
    size_t signature_size);

/* Only hw1_ota_manifest_verify() should create this value from external data. */
typedef struct {
    hw1_ota_manifest_t manifest;
    hw1_ota_signature_algorithm_t signature_algorithm;
} hw1_ota_verified_manifest_t;

typedef struct {
    const char *board_id;
    const char *layout_id;
    const char *project_name;
    const char *required_version_suffix;
    const char *current_updater_version;
    uint32_t maximum_image_size;
    uint32_t minimum_data_schema;
    uint32_t maximum_data_schema;
} hw1_ota_target_policy_t;

/* Convenience initializer; it fails at compile time if the build IDs are absent. */
#define HW1_OTA_TARGET_POLICY_BUILD_INIT(project_, suffix_, updater_, max_size_, min_schema_, max_schema_) \
    {                                                                                                      \
        .board_id = HW1_OTA_BOARD_ID,                                                                      \
        .layout_id = HW1_OTA_LAYOUT_ID,                                                                    \
        .project_name = (project_),                                                                        \
        .required_version_suffix = (suffix_),                                                              \
        .current_updater_version = (updater_),                                                             \
        .maximum_image_size = (max_size_),                                                                 \
        .minimum_data_schema = (min_schema_),                                                              \
        .maximum_data_schema = (max_schema_),                                                              \
    }

typedef enum {
    HW1_OTA_MANIFEST_MISMATCH_NONE = 0,
    HW1_OTA_MANIFEST_MISMATCH_FORMAT = 1u << 0,
    HW1_OTA_MANIFEST_MISMATCH_BOARD = 1u << 1,
    HW1_OTA_MANIFEST_MISMATCH_LAYOUT = 1u << 2,
    HW1_OTA_MANIFEST_MISMATCH_PROJECT = 1u << 3,
    HW1_OTA_MANIFEST_MISMATCH_VERSION_SUFFIX = 1u << 4,
    HW1_OTA_MANIFEST_MISMATCH_IMAGE_TOO_LARGE = 1u << 5,
    HW1_OTA_MANIFEST_MISMATCH_IMAGE_SIZE = 1u << 6,
    HW1_OTA_MANIFEST_MISMATCH_IMAGE_SHA256 = 1u << 7,
    HW1_OTA_MANIFEST_MISMATCH_UPDATER_VERSION = 1u << 8,
    HW1_OTA_MANIFEST_MISMATCH_DATA_SCHEMA = 1u << 9,
} hw1_ota_manifest_mismatch_t;

typedef enum {
    HW1_OTA_PHASE_IDLE = 0,
    HW1_OTA_PHASE_REQUESTED = 1,
    HW1_OTA_PHASE_RECOVERY_BOOT_ARMED = 2,
    HW1_OTA_PHASE_RECOVERY_RUNNING = 3,
    HW1_OTA_PHASE_APPLYING = 4,
    HW1_OTA_PHASE_IMAGE_VERIFIED = 5,
    HW1_OTA_PHASE_TRIAL_BOOT_ARMED = 6,
    HW1_OTA_PHASE_TRIAL_RUNNING = 7,
    HW1_OTA_PHASE_SUCCEEDED = 8,
    HW1_OTA_PHASE_FAILED = 9,
    HW1_OTA_PHASE_CANCELED = 10,
} hw1_ota_phase_t;

typedef enum {
    HW1_OTA_SOURCE_NONE = 0,
    HW1_OTA_SOURCE_STAGED_FILE = 1,
    HW1_OTA_SOURCE_RECOVERY_UPLOAD = 2,
    HW1_OTA_SOURCE_SERIAL = 3,
} hw1_ota_source_t;

typedef enum {
    HW1_OTA_REQUEST_NONE = 0,
    HW1_OTA_REQUEST_ALLOW_DOWNGRADE = 1u << 0,
    HW1_OTA_REQUEST_FORCED_POWER_OVERRIDE = 1u << 1,
    /* Emergency recovery may prioritize restoring service over retaining the
     * sole pending result slot. Ordinary operator flows must acknowledge the
     * pending result instead of setting this flag. */
    HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT = 1u << 2,
} hw1_ota_request_flag_t;

typedef enum {
    HW1_OTA_RESULT_NONE = 0,
    HW1_OTA_RESULT_SUCCESS = 1,
    HW1_OTA_RESULT_CANCELED = 2,
    HW1_OTA_RESULT_MANIFEST_INVALID = 10,
    HW1_OTA_RESULT_SIGNATURE_INVALID = 11,
    HW1_OTA_RESULT_INCOMPATIBLE_IMAGE = 12,
    HW1_OTA_RESULT_DIGEST_MISMATCH = 13,
    HW1_OTA_RESULT_POWER_UNSAFE = 14,
    HW1_OTA_RESULT_STORAGE_ERROR = 20,
    HW1_OTA_RESULT_FLASH_ERROR = 21,
    HW1_OTA_RESULT_BOOT_SWITCH_ERROR = 22,
    HW1_OTA_RESULT_HEALTH_TIMEOUT = 30,
    HW1_OTA_RESULT_ROLLBACK_DETECTED = 31,
    HW1_OTA_RESULT_INTERNAL_ERROR = 255,
} hw1_ota_result_code_t;

typedef struct {
    uint32_t sequence;
    uint64_t operation_id;
    hw1_ota_result_code_t code;
    hw1_ota_phase_t terminal_phase;
    int32_t native_error;
    char detail[HW1_OTA_RESULT_DETAIL_SIZE];
} hw1_ota_result_t;

typedef struct {
    uint32_t sequence; /* assigned by the transactional store */
    uint64_t operation_id;
    hw1_ota_phase_t phase;
    hw1_ota_source_t source;
    uint16_t request_flags;
    uint32_t trial_boot_count;
    bool candidate_present;
    /*
     * Durable comparison metadata, not a durable authentication token. NVS
     * CRC protects against torn/corrupt writes but cannot prove who wrote the
     * record. After every load, re-verify the signed envelope and use
     * hw1_ota_record_candidate_matches_verified() before trusting this value.
     */
    hw1_ota_verified_manifest_t candidate;
    hw1_ota_result_t last_result;
    uint32_t acknowledged_result_sequence;
} hw1_ota_record_t;

typedef enum {
    HW1_OTA_EVENT_ARM_RECOVERY_BOOT = 1,
    HW1_OTA_EVENT_RECOVERY_STARTED = 2,
    HW1_OTA_EVENT_APPLY_STARTED = 3,
    HW1_OTA_EVENT_IMAGE_VERIFIED = 4,
    HW1_OTA_EVENT_ARM_TRIAL_BOOT = 5,
    HW1_OTA_EVENT_TRIAL_STARTED = 6,
    HW1_OTA_EVENT_MARK_VALID = 7,
    HW1_OTA_EVENT_FAIL = 8,
    HW1_OTA_EVENT_CANCEL = 9,
    HW1_OTA_EVENT_ROLLBACK_OBSERVED = 10,
    HW1_OTA_EVENT_CLEAR_TERMINAL = 11,
} hw1_ota_event_t;

/*
 * CANCEL is structurally valid through APPLYING so recovery can abandon a
 * direct-upload transaction before a writer owns the flash operation. The
 * recovery caller must first prove that ota_0 is still a complete, bootable
 * image; the protocol core deliberately has no partition access with which to
 * enforce that environmental precondition.
 */

typedef struct {
    const hw1_ota_verified_manifest_t *verified_manifest;
    hw1_ota_result_code_t result_code;
    int32_t native_error;
    const char *detail;
} hw1_ota_transition_args_t;

typedef enum {
    HW1_OTA_ROLE_UNKNOWN = 0,
    HW1_OTA_ROLE_MAIN = 1,
    HW1_OTA_ROLE_RECOVERY = 2,
} hw1_ota_role_t;

typedef enum {
    HW1_OTA_BOOT_TARGET_UNKNOWN = 0,
    HW1_OTA_BOOT_TARGET_MAIN = 1,
    HW1_OTA_BOOT_TARGET_RECOVERY = 2,
} hw1_ota_boot_target_t;

typedef enum {
    HW1_OTA_IMAGE_UNKNOWN = 0,
    HW1_OTA_IMAGE_ABSENT = 1,
    HW1_OTA_IMAGE_INVALID = 2,
    HW1_OTA_IMAGE_VALID = 3,
    HW1_OTA_IMAGE_PENDING_VERIFY = 4,
    HW1_OTA_IMAGE_ACCEPTED = 5,
} hw1_ota_image_state_t;

typedef struct {
    hw1_ota_role_t running_role;
    hw1_ota_boot_target_t configured_boot_target;
    hw1_ota_image_state_t main_image_state;
    bool staged_candidate_available;
} hw1_ota_observation_t;

typedef enum {
    HW1_OTA_ACTION_NONE = 0,
    HW1_OTA_ACTION_COMMIT_EVENT = 1u << 0,
    HW1_OTA_ACTION_SET_BOOT_RECOVERY = 1u << 1,
    HW1_OTA_ACTION_SET_BOOT_MAIN = 1u << 2,
    HW1_OTA_ACTION_REBOOT = 1u << 3,
    HW1_OTA_ACTION_START_APPLY = 1u << 4,
    HW1_OTA_ACTION_RESUME_APPLY = 1u << 5,
    HW1_OTA_ACTION_RUN_HEALTH_CHECK = 1u << 6,
    HW1_OTA_ACTION_REPORT_RESULT = 1u << 7,
    HW1_OTA_ACTION_HOLD_FOR_OPERATOR = 1u << 8,
} hw1_ota_action_t;

typedef struct {
    uint32_t actions;
    hw1_ota_event_t event_before_actions;
    hw1_ota_result_code_t event_result_code;
} hw1_ota_reconcile_decision_t;

uint32_t hw1_ota_crc32(uint32_t previous_crc, const void *data, size_t size);
bool hw1_ota_sequence_is_newer(uint32_t candidate, uint32_t reference);

const hw1_ota_signature_spec_t *hw1_ota_manifest_signature_spec(void);
hw1_ota_status_t hw1_ota_manifest_encode(
    const hw1_ota_manifest_t *manifest,
    uint8_t payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE]);
hw1_ota_status_t hw1_ota_manifest_parse_untrusted(
    const uint8_t *payload,
    size_t payload_size,
    hw1_ota_manifest_t *manifest);
hw1_ota_status_t hw1_ota_manifest_verify(
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *signature,
    size_t signature_size,
    hw1_ota_manifest_verify_fn verifier,
    void *verifier_context,
    hw1_ota_verified_manifest_t *verified_manifest);
hw1_ota_status_t hw1_ota_manifest_validate(
    const hw1_ota_verified_manifest_t *verified_manifest,
    const hw1_ota_target_policy_t *policy,
    uint32_t observed_image_size,
    const uint8_t observed_image_sha256[HW1_OTA_SHA256_SIZE],
    uint32_t *mismatches);
int hw1_ota_semver_compare(const char *left, const char *right, bool *valid);

void hw1_ota_record_init(hw1_ota_record_t *record);
hw1_ota_status_t hw1_ota_record_validate(const hw1_ota_record_t *record);
hw1_ota_status_t hw1_ota_record_encode(
    const hw1_ota_record_t *record,
    uint8_t wire[HW1_OTA_RECORD_WIRE_SIZE]);
hw1_ota_status_t hw1_ota_record_decode(
    const uint8_t *wire,
    size_t wire_size,
    hw1_ota_record_t *record);

hw1_ota_status_t hw1_ota_begin(
    hw1_ota_record_t *record,
    uint64_t operation_id,
    hw1_ota_source_t source,
    uint16_t request_flags,
    const hw1_ota_verified_manifest_t *candidate);
hw1_ota_status_t hw1_ota_transition(
    hw1_ota_record_t *record,
    hw1_ota_event_t event,
    const hw1_ota_transition_args_t *args);
bool hw1_ota_result_pending(const hw1_ota_record_t *record);
/*
 * Compares the durable candidate with a manifest freshly produced by
 * hw1_ota_manifest_verify(). This is the required bridge across a reboot;
 * record->candidate alone must never authorize an installation.
 */
bool hw1_ota_record_candidate_matches_verified(
    const hw1_ota_record_t *record,
    const hw1_ota_verified_manifest_t *freshly_verified_manifest);
hw1_ota_status_t hw1_ota_acknowledge_result(
    hw1_ota_record_t *record,
    uint32_t result_sequence);
hw1_ota_status_t hw1_ota_reconcile(
    const hw1_ota_record_t *record,
    const hw1_ota_observation_t *observation,
    hw1_ota_reconcile_decision_t *decision);

#ifdef __cplusplus
}
#endif
