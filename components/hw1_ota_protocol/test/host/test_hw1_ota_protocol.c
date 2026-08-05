#include "hw1_ota_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

static hw1_ota_manifest_t make_manifest(void)
{
    hw1_ota_manifest_t manifest;
    size_t i;

    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = HW1_OTA_MANIFEST_FORMAT_VERSION;
    strcpy(manifest.board_id, HW1_OTA_BOARD_ID_FEATHERS3);
    strcpy(manifest.layout_id, HW1_OTA_LAYOUT_ID_FEATHERS3_OTA_V1);
    strcpy(manifest.project_name, "hardwareone-idf");
    strcpy(manifest.version, "1.2.3+f3o1");
    manifest.image_size = 0x00500000u;
    for (i = 0; i < sizeof(manifest.image_sha256); ++i) {
        manifest.image_sha256[i] = (uint8_t)(i + 1u);
    }
    strcpy(manifest.min_updater_version, "1.0.0");
    manifest.data_schema = 3;
    return manifest;
}

static hw1_ota_status_t accepting_verifier(
    void *context,
    const hw1_ota_signature_spec_t *spec,
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *signature,
    size_t signature_size)
{
    const uint8_t expected_marker = *(const uint8_t *)context;
    CHECK(spec != NULL);
    CHECK(spec->algorithm == HW1_OTA_SIGNATURE_RSA_PSS_SHA256);
    CHECK(spec->rsa_key_bits == 3072);
    CHECK(spec->digest_size == 32);
    CHECK(spec->pss_salt_length == 32);
    CHECK(payload != NULL);
    CHECK(payload_size == HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE);
    CHECK(signature_size == HW1_OTA_RSA3072_SIGNATURE_SIZE);
    return signature != NULL && signature[0] == expected_marker
               ? HW1_OTA_OK
               : HW1_OTA_ERR_SIGNATURE_INVALID;
}

static hw1_ota_verified_manifest_t make_verified_manifest(void)
{
    const uint8_t marker = 0xa5;
    hw1_ota_manifest_t manifest = make_manifest();
    hw1_ota_verified_manifest_t verified;
    uint8_t payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];
    uint8_t signature[HW1_OTA_RSA3072_SIGNATURE_SIZE];

    memset(signature, 0, sizeof(signature));
    signature[0] = marker;
    CHECK(hw1_ota_manifest_encode(&manifest, payload) == HW1_OTA_OK);
    CHECK(hw1_ota_manifest_verify(payload, sizeof(payload), signature, sizeof(signature),
                                  accepting_verifier, (void *)&marker, &verified) == HW1_OTA_OK);
    return verified;
}

static void test_crc_and_sequence(void)
{
    static const char vector[] = "123456789";
    const uint32_t first = hw1_ota_crc32(0, vector, 4);

    CHECK(hw1_ota_crc32(0, vector, 9) == 0xcbf43926u);
    CHECK(hw1_ota_crc32(first, vector + 4, 5) == 0xcbf43926u);
    CHECK(hw1_ota_sequence_is_newer(11, 10));
    CHECK(!hw1_ota_sequence_is_newer(10, 10));
    CHECK(hw1_ota_sequence_is_newer(1, UINT32_MAX));
    CHECK(!hw1_ota_sequence_is_newer(UINT32_MAX, 1));
}

static void test_manifest_auth_and_policy(void)
{
    const uint8_t marker = 0xa5;
    hw1_ota_manifest_t source = make_manifest();
    hw1_ota_manifest_t parsed;
    hw1_ota_verified_manifest_t verified;
    hw1_ota_target_policy_t policy = {
        .board_id = HW1_OTA_BOARD_ID_FEATHERS3,
        .layout_id = HW1_OTA_LAYOUT_ID_FEATHERS3_OTA_V1,
        .project_name = "hardwareone-idf",
        .required_version_suffix = HW1_OTA_VERSION_SUFFIX_FEATHERS3_PLAIN,
        .current_updater_version = "1.1.0+f3o1",
        .maximum_image_size = 0x00595000u,
        .minimum_data_schema = 2,
        .maximum_data_schema = 4,
    };
    uint8_t payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];
    uint8_t corrupt[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];
    uint8_t signature[HW1_OTA_RSA3072_SIGNATURE_SIZE];
    uint32_t mismatches = UINT32_MAX;

    memset(signature, 0, sizeof(signature));
    signature[0] = marker;
    CHECK(hw1_ota_manifest_encode(&source, payload) == HW1_OTA_OK);
    /* Cross-language fixture from tools/ota/make_manifest.py encode_payload(). */
    CHECK(payload[220] == 0x94 && payload[221] == 0x69 &&
          payload[222] == 0x42 && payload[223] == 0x4e);
    CHECK(hw1_ota_manifest_parse_untrusted(payload, sizeof(payload), &parsed) == HW1_OTA_OK);
    CHECK(strcmp(parsed.board_id, source.board_id) == 0);
    CHECK(strcmp(parsed.version, source.version) == 0);
    CHECK(memcmp(parsed.image_sha256, source.image_sha256, HW1_OTA_SHA256_SIZE) == 0);

    CHECK(hw1_ota_manifest_verify(payload, sizeof(payload), NULL, 0, NULL, NULL,
                                  &verified) == HW1_OTA_ERR_AUTH_REQUIRED);
    CHECK(hw1_ota_manifest_verify(payload, sizeof(payload), signature,
                                  sizeof(signature) - 1, accepting_verifier,
                                  (void *)&marker, &verified) == HW1_OTA_ERR_SIGNATURE_INVALID);
    signature[0] ^= 1u;
    CHECK(hw1_ota_manifest_verify(payload, sizeof(payload), signature, sizeof(signature),
                                  accepting_verifier, (void *)&marker,
                                  &verified) == HW1_OTA_ERR_SIGNATURE_INVALID);
    signature[0] = marker;
    CHECK(hw1_ota_manifest_verify(payload, sizeof(payload), signature, sizeof(signature),
                                  accepting_verifier, (void *)&marker, &verified) == HW1_OTA_OK);
    CHECK(hw1_ota_manifest_validate(&verified, &policy, source.image_size,
                                    source.image_sha256, &mismatches) == HW1_OTA_OK);
    CHECK(mismatches == 0);

    policy.board_id = HW1_OTA_BOARD_ID_FEATHERS3_FLASH_ENCRYPTED;
    policy.current_updater_version = "0.9.9";
    CHECK(hw1_ota_manifest_validate(&verified, &policy, source.image_size - 1,
                                    source.image_sha256, &mismatches) ==
          HW1_OTA_ERR_INCOMPATIBLE);
    CHECK((mismatches & HW1_OTA_MANIFEST_MISMATCH_BOARD) != 0);
    CHECK((mismatches & HW1_OTA_MANIFEST_MISMATCH_IMAGE_SIZE) != 0);
    CHECK((mismatches & HW1_OTA_MANIFEST_MISMATCH_UPDATER_VERSION) != 0);

    memcpy(corrupt, payload, sizeof(corrupt));
    corrupt[40] ^= 0x80u;
    CHECK(hw1_ota_manifest_parse_untrusted(corrupt, sizeof(corrupt), &parsed) ==
          HW1_OTA_ERR_CORRUPT);
}

static void test_semver(void)
{
    bool valid = false;

    CHECK(hw1_ota_semver_compare("1.2.3", "1.2.3", &valid) == 0 && valid);
    CHECK(hw1_ota_semver_compare("1.2.4+f3o1", "1.2.3", &valid) > 0 && valid);
    CHECK(hw1_ota_semver_compare("1.2.3-alpha.2", "1.2.3-alpha.10", &valid) < 0 && valid);
    CHECK(hw1_ota_semver_compare("1.2.3", "1.2.3-rc.1", &valid) > 0 && valid);
    (void)hw1_ota_semver_compare("1.02.3", "1.2.3", &valid);
    CHECK(!valid);
}

static void test_record_round_trip_and_transitions(void)
{
    hw1_ota_verified_manifest_t verified = make_verified_manifest();
    hw1_ota_record_t record;
    hw1_ota_record_t decoded;
    hw1_ota_transition_args_t args = {
        .verified_manifest = &verified,
    };
    uint8_t wire[HW1_OTA_RECORD_WIRE_SIZE];
    uint8_t corrupt[HW1_OTA_RECORD_WIRE_SIZE];
    uint32_t result_sequence;

    hw1_ota_record_init(&record);
    CHECK(hw1_ota_begin(&record, 0x1122334455667788ULL,
                        HW1_OTA_SOURCE_STAGED_FILE, 0, &verified) == HW1_OTA_OK);
    CHECK(hw1_ota_record_candidate_matches_verified(&record, &verified));
    verified.manifest.image_sha256[0] ^= 1u;
    CHECK(!hw1_ota_record_candidate_matches_verified(&record, &verified));
    verified.manifest.image_sha256[0] ^= 1u;
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_ARM_RECOVERY_BOOT, NULL) == HW1_OTA_OK);
    record.sequence = 41;
    CHECK(hw1_ota_record_encode(&record, wire) == HW1_OTA_OK);
    CHECK(hw1_ota_record_decode(wire, sizeof(wire), &decoded) == HW1_OTA_OK);
    CHECK(decoded.sequence == 41);
    CHECK(decoded.operation_id == record.operation_id);
    CHECK(decoded.phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED);
    CHECK(strcmp(decoded.candidate.manifest.layout_id,
                 HW1_OTA_LAYOUT_ID_FEATHERS3_OTA_V1) == 0);

    memcpy(corrupt, wire, sizeof(corrupt));
    corrupt[100] ^= 1u;
    CHECK(hw1_ota_record_decode(corrupt, sizeof(corrupt), &decoded) ==
          HW1_OTA_ERR_CORRUPT);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_TRIAL_STARTED, NULL) ==
          HW1_OTA_ERR_INVALID_STATE);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_RECOVERY_STARTED, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_APPLY_STARTED, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_IMAGE_VERIFIED, &args) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_ARM_TRIAL_BOOT, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_TRIAL_STARTED, NULL) == HW1_OTA_OK);
    CHECK(record.trial_boot_count == 1);
    args.detail = "health window completed";
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_MARK_VALID, &args) == HW1_OTA_OK);
    CHECK(record.phase == HW1_OTA_PHASE_SUCCEEDED);
    CHECK(hw1_ota_result_pending(&record));
    result_sequence = record.last_result.sequence;
    CHECK(hw1_ota_acknowledge_result(&record, result_sequence + 1) == HW1_OTA_ERR_CONFLICT);
    CHECK(hw1_ota_acknowledge_result(&record, result_sequence) == HW1_OTA_OK);
    CHECK(!hw1_ota_result_pending(&record));
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_CLEAR_TERMINAL, NULL) == HW1_OTA_OK);
    CHECK(record.phase == HW1_OTA_PHASE_IDLE);
    CHECK(record.last_result.sequence == result_sequence);
    CHECK(hw1_ota_begin(&record, 0x8877665544332211ULL,
                        HW1_OTA_SOURCE_RECOVERY_UPLOAD, 0, NULL) == HW1_OTA_OK);
    CHECK(record.last_result.sequence == result_sequence);
}

static void test_failure_and_cancel_rules(void)
{
    hw1_ota_record_t record;
    hw1_ota_transition_args_t args = {
        .result_code = HW1_OTA_RESULT_STORAGE_ERROR,
        .native_error = -7,
        .detail = "staged candidate unavailable",
    };

    hw1_ota_record_init(&record);
    CHECK(hw1_ota_begin(&record, 1, HW1_OTA_SOURCE_STAGED_FILE, 0, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_CANCEL, NULL) == HW1_OTA_OK);
    CHECK(record.phase == HW1_OTA_PHASE_CANCELED);
    CHECK(record.last_result.code == HW1_OTA_RESULT_CANCELED);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_CLEAR_TERMINAL, NULL) == HW1_OTA_OK);

    CHECK(hw1_ota_begin(&record, 2, HW1_OTA_SOURCE_RECOVERY_UPLOAD, 0, NULL) ==
          HW1_OTA_ERR_INVALID_STATE);
    CHECK(hw1_ota_acknowledge_result(&record, record.last_result.sequence) == HW1_OTA_OK);
    CHECK(hw1_ota_begin(&record, 2, HW1_OTA_SOURCE_RECOVERY_UPLOAD, 0, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_RECOVERY_STARTED, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_APPLY_STARTED, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_CANCEL, NULL) == HW1_OTA_OK);
    CHECK(record.phase == HW1_OTA_PHASE_CANCELED);
    CHECK(record.last_result.code == HW1_OTA_RESULT_CANCELED);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_CLEAR_TERMINAL, NULL) == HW1_OTA_OK);

    CHECK(hw1_ota_result_pending(&record));
    CHECK(hw1_ota_begin(&record, 3, HW1_OTA_SOURCE_RECOVERY_UPLOAD,
                        HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT,
                        NULL) == HW1_OTA_OK);
    record.request_flags = 0;
    CHECK(hw1_ota_record_validate(&record) == HW1_OTA_ERR_INVALID_STATE);
    record.request_flags = HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT;
    CHECK(hw1_ota_record_validate(&record) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_RECOVERY_STARTED, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_APPLY_STARTED, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_FAIL, &args) == HW1_OTA_OK);
    CHECK(record.phase == HW1_OTA_PHASE_FAILED);
    CHECK(record.last_result.native_error == -7);
}

static void test_reconciliation(void)
{
    hw1_ota_verified_manifest_t verified = make_verified_manifest();
    hw1_ota_record_t record;
    hw1_ota_observation_t observation = {
        .running_role = HW1_OTA_ROLE_MAIN,
        .configured_boot_target = HW1_OTA_BOOT_TARGET_MAIN,
        .main_image_state = HW1_OTA_IMAGE_ACCEPTED,
        .staged_candidate_available = true,
    };
    hw1_ota_reconcile_decision_t decision;
    hw1_ota_transition_args_t args = {.verified_manifest = &verified};

    hw1_ota_record_init(&record);
    CHECK(hw1_ota_begin(&record, 3, HW1_OTA_SOURCE_STAGED_FILE, 0, &verified) == HW1_OTA_OK);
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK(decision.actions == HW1_OTA_ACTION_NONE);

    {
        hw1_ota_record_t direct;
        hw1_ota_record_init(&direct);
        CHECK(hw1_ota_begin(&direct, 4, HW1_OTA_SOURCE_RECOVERY_UPLOAD,
                            0, NULL) == HW1_OTA_OK);
        CHECK(hw1_ota_reconcile(&direct, &observation, &decision) == HW1_OTA_OK);
        CHECK(decision.event_before_actions == HW1_OTA_EVENT_ARM_RECOVERY_BOOT);
        CHECK((decision.actions & HW1_OTA_ACTION_COMMIT_EVENT) != 0);
        CHECK((decision.actions & HW1_OTA_ACTION_SET_BOOT_RECOVERY) != 0);
        CHECK((decision.actions & HW1_OTA_ACTION_REBOOT) != 0);
    }

    /* The operator's explicit launch action commits ARM_RECOVERY_BOOT. */
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_ARM_RECOVERY_BOOT, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK(decision.event_before_actions == 0);
    CHECK((decision.actions & HW1_OTA_ACTION_SET_BOOT_RECOVERY) != 0);
    CHECK((decision.actions & HW1_OTA_ACTION_REBOOT) != 0);

    observation.running_role = HW1_OTA_ROLE_RECOVERY;
    observation.configured_boot_target = HW1_OTA_BOOT_TARGET_RECOVERY;
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK(decision.event_before_actions == HW1_OTA_EVENT_RECOVERY_STARTED);
    CHECK(hw1_ota_transition(&record, decision.event_before_actions, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK(decision.event_before_actions == HW1_OTA_EVENT_APPLY_STARTED);
    CHECK((decision.actions & HW1_OTA_ACTION_START_APPLY) != 0);
    CHECK(hw1_ota_transition(&record, decision.event_before_actions, NULL) == HW1_OTA_OK);

    observation.main_image_state = HW1_OTA_IMAGE_VALID;
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK(decision.event_before_actions == HW1_OTA_EVENT_IMAGE_VERIFIED);
    CHECK(hw1_ota_transition(&record, decision.event_before_actions, &args) == HW1_OTA_OK);
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK(decision.event_before_actions == HW1_OTA_EVENT_ARM_TRIAL_BOOT);
    CHECK((decision.actions & HW1_OTA_ACTION_SET_BOOT_MAIN) != 0);

    CHECK(hw1_ota_transition(&record, decision.event_before_actions, NULL) == HW1_OTA_OK);
    observation.running_role = HW1_OTA_ROLE_MAIN;
    observation.configured_boot_target = HW1_OTA_BOOT_TARGET_MAIN;
    observation.main_image_state = HW1_OTA_IMAGE_PENDING_VERIFY;
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK(decision.event_before_actions == HW1_OTA_EVENT_TRIAL_STARTED);
    CHECK(hw1_ota_transition(&record, decision.event_before_actions, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK((decision.actions & HW1_OTA_ACTION_RUN_HEALTH_CHECK) != 0);

    observation.running_role = HW1_OTA_ROLE_RECOVERY;
    observation.main_image_state = HW1_OTA_IMAGE_INVALID;
    CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
    CHECK(decision.event_before_actions == HW1_OTA_EVENT_ROLLBACK_OBSERVED);
    CHECK((decision.actions & HW1_OTA_ACTION_HOLD_FOR_OPERATOR) != 0);
}

/*
 * A trial image that the bootloader rejected BEFORE the main app could journal
 * TRIAL_STARTED leaves the record in TRIAL_BOOT_ARMED, not TRIAL_RUNNING. The
 * core must still call that a rollback; if a consumer instead treats
 * TRIAL_BOOT_ARMED as "still installable" it will re-arm the same crashing
 * image forever, and CANCEL is refused from that phase so the only escape is a
 * cable. This pins the core's half of that contract.
 */
static void test_trial_boot_armed_rejection_is_a_rollback(void)
{
    hw1_ota_verified_manifest_t verified = make_verified_manifest();
    hw1_ota_record_t record;
    hw1_ota_reconcile_decision_t decision;
    hw1_ota_transition_args_t args = {.verified_manifest = &verified};
    hw1_ota_observation_t observation = {
        .running_role = HW1_OTA_ROLE_RECOVERY,
        .configured_boot_target = HW1_OTA_BOOT_TARGET_MAIN,
        .main_image_state = HW1_OTA_IMAGE_INVALID,
        .staged_candidate_available = true,
    };
    /* Every state the bootloader can leave behind that is NOT installable. */
    const int rejected[] = {
        HW1_OTA_IMAGE_INVALID,
        HW1_OTA_IMAGE_ABSENT,
        HW1_OTA_IMAGE_UNKNOWN,
    };
    size_t i;

    hw1_ota_record_init(&record);
    CHECK(hw1_ota_begin(&record, 11, HW1_OTA_SOURCE_STAGED_FILE, 0, &verified) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_ARM_RECOVERY_BOOT, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_RECOVERY_STARTED, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_APPLY_STARTED, NULL) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_IMAGE_VERIFIED, &args) == HW1_OTA_OK);
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_ARM_TRIAL_BOOT, NULL) == HW1_OTA_OK);
    CHECK(record.phase == HW1_OTA_PHASE_TRIAL_BOOT_ARMED);

    /* Back in recovery with a main image the bootloader would not accept. */
    for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        observation.main_image_state = (hw1_ota_image_state_t)rejected[i];
        CHECK(hw1_ota_reconcile(&record, &observation, &decision) == HW1_OTA_OK);
        CHECK(decision.event_before_actions == HW1_OTA_EVENT_ROLLBACK_OBSERVED);
        CHECK(decision.event_result_code == HW1_OTA_RESULT_ROLLBACK_DETECTED);
        CHECK((decision.actions & HW1_OTA_ACTION_HOLD_FOR_OPERATOR) != 0);
        /* Must never be recommended back onto the main slot. */
        CHECK((decision.actions & HW1_OTA_ACTION_SET_BOOT_MAIN) == 0);
    }

    /* And the rollback must actually be committable from this phase. */
    CHECK(hw1_ota_transition(&record, HW1_OTA_EVENT_ROLLBACK_OBSERVED, NULL) == HW1_OTA_OK);
    CHECK(record.phase == HW1_OTA_PHASE_FAILED);
}

static void test_single_bit_corruption_is_rejected(void)
{
    hw1_ota_verified_manifest_t verified = make_verified_manifest();
    hw1_ota_manifest_t parsed;
    hw1_ota_record_t record;
    hw1_ota_record_t decoded;
    uint8_t manifest_wire[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];
    uint8_t record_wire[HW1_OTA_RECORD_WIRE_SIZE];
    size_t byte;
    unsigned bit;

    CHECK(hw1_ota_manifest_encode(&verified.manifest, manifest_wire) == HW1_OTA_OK);
    for (byte = 0; byte < sizeof(manifest_wire); ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            manifest_wire[byte] ^= (uint8_t)(1u << bit);
            CHECK(hw1_ota_manifest_parse_untrusted(manifest_wire, sizeof(manifest_wire),
                                                   &parsed) != HW1_OTA_OK);
            manifest_wire[byte] ^= (uint8_t)(1u << bit);
        }
    }

    hw1_ota_record_init(&record);
    CHECK(hw1_ota_begin(&record, 99, HW1_OTA_SOURCE_STAGED_FILE, 0, &verified) == HW1_OTA_OK);
    CHECK(hw1_ota_record_encode(&record, record_wire) == HW1_OTA_OK);
    for (byte = 0; byte < sizeof(record_wire); ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            record_wire[byte] ^= (uint8_t)(1u << bit);
            CHECK(hw1_ota_record_decode(record_wire, sizeof(record_wire), &decoded) !=
                  HW1_OTA_OK);
            record_wire[byte] ^= (uint8_t)(1u << bit);
        }
    }
}

int main(void)
{
    test_crc_and_sequence();
    test_manifest_auth_and_policy();
    test_semver();
    test_record_round_trip_and_transitions();
    test_failure_and_cancel_rules();
    test_reconciliation();
    test_trial_boot_armed_rejection_is_a_rollback();
    test_single_bit_corruption_is_rejected();
    puts("hw1_ota_protocol host tests: PASS");
    return 0;
}
