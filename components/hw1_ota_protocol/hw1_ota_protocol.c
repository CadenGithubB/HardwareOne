#include "hw1_ota_protocol.h"

#include <limits.h>
#include <string.h>

#define HW1_OTA_MANIFEST_MAGIC 0x314d3148u /* "H1M1", little-endian */
#define HW1_OTA_RECORD_MAGIC 0x544f3148u   /* "H1OT", little-endian */

#define MANIFEST_CRC_OFFSET (HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE - 4u)
#define RECORD_CRC_OFFSET (HW1_OTA_RECORD_WIRE_SIZE - 4u)

#define HW1_OTA_REQUEST_KNOWN_FLAGS \
    (HW1_OTA_REQUEST_ALLOW_DOWNGRADE | HW1_OTA_REQUEST_FORCED_POWER_OVERRIDE | \
     HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT)

static const hw1_ota_signature_spec_t k_signature_spec = {
    .algorithm = HW1_OTA_SIGNATURE_RSA_PSS_SHA256,
    .rsa_key_bits = 3072,
    .digest_size = HW1_OTA_SHA256_SIZE,
    .pss_salt_length = HW1_OTA_PSS_SALT_LENGTH,
};

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_u64(uint8_t *p, uint64_t value)
{
    write_u32(p, (uint32_t)value);
    write_u32(p + 4, (uint32_t)(value >> 32));
}

static bool bytes_are_zero(const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        if (data[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool digest_is_zero(const uint8_t digest[HW1_OTA_SHA256_SIZE])
{
    return bytes_are_zero(digest, HW1_OTA_SHA256_SIZE);
}

static bool text_is_canonical(const char *text, size_t capacity, bool allow_empty)
{
    size_t end = 0;
    size_t i;

    if (text == NULL || capacity == 0) {
        return false;
    }
    while (end < capacity && text[end] != '\0') {
        const unsigned char c = (unsigned char)text[end];
        if (c < 0x21u || c > 0x7eu) {
            return false;
        }
        ++end;
    }
    if (end == capacity || (!allow_empty && end == 0)) {
        return false;
    }
    for (i = end + 1; i < capacity; ++i) {
        if (text[i] != '\0') {
            return false;
        }
    }
    return true;
}

static bool wire_text_is_canonical(const uint8_t *text, size_t capacity, bool allow_empty)
{
    return text_is_canonical((const char *)text, capacity, allow_empty);
}

static bool detail_is_canonical(const char *text, size_t capacity)
{
    size_t end = 0;
    size_t i;

    if (text == NULL || capacity == 0) {
        return false;
    }
    while (end < capacity && text[end] != '\0') {
        const unsigned char c = (unsigned char)text[end];
        if (c < 0x20u || c > 0x7eu) {
            return false;
        }
        ++end;
    }
    if (end == capacity) {
        return false;
    }
    for (i = end + 1; i < capacity; ++i) {
        if (text[i] != '\0') {
            return false;
        }
    }
    return true;
}

static bool copy_text(char *destination, size_t capacity, const char *source, bool allow_empty)
{
    size_t length;

    if (destination == NULL || source == NULL || capacity == 0) {
        return false;
    }
    length = strnlen(source, capacity);
    if (length == capacity || (!allow_empty && length == 0)) {
        return false;
    }
    memset(destination, 0, capacity);
    memcpy(destination, source, length);
    return allow_empty ? detail_is_canonical(destination, capacity)
                       : text_is_canonical(destination, capacity, false);
}

static bool phase_is_valid(hw1_ota_phase_t phase)
{
    return phase >= HW1_OTA_PHASE_IDLE && phase <= HW1_OTA_PHASE_CANCELED;
}

static bool phase_is_terminal(hw1_ota_phase_t phase)
{
    return phase == HW1_OTA_PHASE_SUCCEEDED ||
           phase == HW1_OTA_PHASE_FAILED ||
           phase == HW1_OTA_PHASE_CANCELED;
}

static bool source_is_valid(hw1_ota_source_t source)
{
    return source >= HW1_OTA_SOURCE_NONE && source <= HW1_OTA_SOURCE_SERIAL;
}

static bool result_code_is_valid(hw1_ota_result_code_t code)
{
    switch (code) {
    case HW1_OTA_RESULT_NONE:
    case HW1_OTA_RESULT_SUCCESS:
    case HW1_OTA_RESULT_CANCELED:
    case HW1_OTA_RESULT_MANIFEST_INVALID:
    case HW1_OTA_RESULT_SIGNATURE_INVALID:
    case HW1_OTA_RESULT_INCOMPATIBLE_IMAGE:
    case HW1_OTA_RESULT_DIGEST_MISMATCH:
    case HW1_OTA_RESULT_POWER_UNSAFE:
    case HW1_OTA_RESULT_STORAGE_ERROR:
    case HW1_OTA_RESULT_FLASH_ERROR:
    case HW1_OTA_RESULT_BOOT_SWITCH_ERROR:
    case HW1_OTA_RESULT_HEALTH_TIMEOUT:
    case HW1_OTA_RESULT_ROLLBACK_DETECTED:
    case HW1_OTA_RESULT_INTERNAL_ERROR:
        return true;
    default:
        return false;
    }
}

static uint32_t next_nonzero_counter(uint32_t current)
{
    const uint32_t next = current + 1u;
    return next == 0 ? 1u : next;
}

uint32_t hw1_ota_crc32(uint32_t previous_crc, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = ~previous_crc;
    size_t i;
    unsigned bit;

    if (data == NULL && size != 0) {
        return previous_crc;
    }
    for (i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

bool hw1_ota_sequence_is_newer(uint32_t candidate, uint32_t reference)
{
    return candidate != reference && (int32_t)(candidate - reference) > 0;
}

const hw1_ota_signature_spec_t *hw1_ota_manifest_signature_spec(void)
{
    return &k_signature_spec;
}

static bool semver_char_is_identifier(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '-';
}

static bool parse_core_number(const char **cursor, uint32_t *value, char delimiter)
{
    const char *p = *cursor;
    uint64_t number = 0;
    size_t digits = 0;

    if (*p < '0' || *p > '9') {
        return false;
    }
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        number = (number * 10u) + (uint32_t)(*p - '0');
        if (number > UINT32_MAX) {
            return false;
        }
        ++p;
        ++digits;
    }
    if (digits == 0 || (delimiter != '\0' && *p != delimiter)) {
        return false;
    }
    *value = (uint32_t)number;
    *cursor = delimiter == '\0' ? p : p + 1;
    return true;
}

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    const char *prerelease;
    size_t prerelease_length;
} semver_t;

static bool validate_identifier_list(const char *text, size_t length, bool reject_numeric_leading_zero)
{
    size_t start = 0;
    size_t i;

    if (length == 0) {
        return false;
    }
    for (i = 0; i <= length; ++i) {
        if (i == length || text[i] == '.') {
            const size_t item_length = i - start;
            size_t j;
            bool numeric = true;
            if (item_length == 0) {
                return false;
            }
            for (j = start; j < i; ++j) {
                if (!semver_char_is_identifier(text[j])) {
                    return false;
                }
                if (text[j] < '0' || text[j] > '9') {
                    numeric = false;
                }
            }
            if (reject_numeric_leading_zero && numeric && item_length > 1 && text[start] == '0') {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}

static bool parse_semver(const char *text, semver_t *parsed)
{
    const char *cursor = text;
    const char *suffix;
    const char *build;
    size_t prerelease_length = 0;

    if (text == NULL || parsed == NULL || *text == '\0') {
        return false;
    }
    memset(parsed, 0, sizeof(*parsed));
    if (!parse_core_number(&cursor, &parsed->major, '.') ||
        !parse_core_number(&cursor, &parsed->minor, '.') ||
        !parse_core_number(&cursor, &parsed->patch, '\0')) {
        return false;
    }
    suffix = cursor;
    if (*suffix == '-') {
        const char *pre_start = ++suffix;
        while (*suffix != '\0' && *suffix != '+') {
            ++suffix;
        }
        prerelease_length = (size_t)(suffix - pre_start);
        if (!validate_identifier_list(pre_start, prerelease_length, true)) {
            return false;
        }
        parsed->prerelease = pre_start;
        parsed->prerelease_length = prerelease_length;
    }
    if (*suffix == '+') {
        build = suffix + 1;
        if (!validate_identifier_list(build, strlen(build), false)) {
            return false;
        }
        suffix += strlen(suffix);
    }
    return *suffix == '\0';
}

static int compare_identifiers(const char *left, size_t left_len, const char *right, size_t right_len)
{
    bool left_numeric = true;
    bool right_numeric = true;
    size_t i;
    int compared;

    for (i = 0; i < left_len; ++i) {
        left_numeric = left_numeric && left[i] >= '0' && left[i] <= '9';
    }
    for (i = 0; i < right_len; ++i) {
        right_numeric = right_numeric && right[i] >= '0' && right[i] <= '9';
    }
    if (left_numeric != right_numeric) {
        return left_numeric ? -1 : 1;
    }
    if (left_numeric && left_len != right_len) {
        return left_len < right_len ? -1 : 1;
    }
    compared = memcmp(left, right, left_len < right_len ? left_len : right_len);
    if (compared != 0) {
        return compared < 0 ? -1 : 1;
    }
    if (left_len == right_len) {
        return 0;
    }
    return left_len < right_len ? -1 : 1;
}

static int compare_prerelease(const semver_t *left, const semver_t *right)
{
    size_t left_at = 0;
    size_t right_at = 0;

    if (left->prerelease_length == 0 || right->prerelease_length == 0) {
        if (left->prerelease_length == right->prerelease_length) {
            return 0;
        }
        return left->prerelease_length == 0 ? 1 : -1;
    }
    while (left_at < left->prerelease_length && right_at < right->prerelease_length) {
        size_t left_end = left_at;
        size_t right_end = right_at;
        int compared;
        while (left_end < left->prerelease_length && left->prerelease[left_end] != '.') {
            ++left_end;
        }
        while (right_end < right->prerelease_length && right->prerelease[right_end] != '.') {
            ++right_end;
        }
        compared = compare_identifiers(left->prerelease + left_at, left_end - left_at,
                                       right->prerelease + right_at, right_end - right_at);
        if (compared != 0) {
            return compared;
        }
        left_at = left_end + 1;
        right_at = right_end + 1;
    }
    if (left_at > left->prerelease_length && right_at > right->prerelease_length) {
        return 0;
    }
    return left_at > left->prerelease_length ? -1 : 1;
}

int hw1_ota_semver_compare(const char *left, const char *right, bool *valid)
{
    semver_t left_version;
    semver_t right_version;

    if (valid != NULL) {
        *valid = false;
    }
    if (!parse_semver(left, &left_version) || !parse_semver(right, &right_version)) {
        return 0;
    }
    if (valid != NULL) {
        *valid = true;
    }
    if (left_version.major != right_version.major) {
        return left_version.major < right_version.major ? -1 : 1;
    }
    if (left_version.minor != right_version.minor) {
        return left_version.minor < right_version.minor ? -1 : 1;
    }
    if (left_version.patch != right_version.patch) {
        return left_version.patch < right_version.patch ? -1 : 1;
    }
    return compare_prerelease(&left_version, &right_version);
}

static bool manifest_fields_are_valid(const hw1_ota_manifest_t *manifest)
{
    bool semver_valid;

    if (manifest == NULL || manifest->format_version != HW1_OTA_MANIFEST_FORMAT_VERSION ||
        !text_is_canonical(manifest->board_id, sizeof(manifest->board_id), false) ||
        !text_is_canonical(manifest->layout_id, sizeof(manifest->layout_id), false) ||
        !text_is_canonical(manifest->project_name, sizeof(manifest->project_name), false) ||
        !text_is_canonical(manifest->version, sizeof(manifest->version), false) ||
        !text_is_canonical(manifest->min_updater_version,
                           sizeof(manifest->min_updater_version), false) ||
        manifest->image_size == 0 || digest_is_zero(manifest->image_sha256)) {
        return false;
    }
    (void)hw1_ota_semver_compare(manifest->version, manifest->version, &semver_valid);
    if (!semver_valid) {
        return false;
    }
    (void)hw1_ota_semver_compare(manifest->min_updater_version,
                                 manifest->min_updater_version, &semver_valid);
    return semver_valid;
}

hw1_ota_status_t hw1_ota_manifest_encode(
    const hw1_ota_manifest_t *manifest,
    uint8_t payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE])
{
    uint32_t crc;

    if (payload == NULL || !manifest_fields_are_valid(manifest)) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    memset(payload, 0, HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE);
    write_u32(payload, HW1_OTA_MANIFEST_MAGIC);
    write_u16(payload + 4, manifest->format_version);
    write_u16(payload + 6, HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE);
    memcpy(payload + 8, manifest->board_id, sizeof(manifest->board_id));
    memcpy(payload + 32, manifest->layout_id, sizeof(manifest->layout_id));
    memcpy(payload + 56, manifest->project_name, sizeof(manifest->project_name));
    memcpy(payload + 88, manifest->version, sizeof(manifest->version));
    write_u32(payload + 136, manifest->image_size);
    memcpy(payload + 140, manifest->image_sha256, HW1_OTA_SHA256_SIZE);
    memcpy(payload + 172, manifest->min_updater_version,
           sizeof(manifest->min_updater_version));
    write_u32(payload + 204, manifest->data_schema);
    crc = hw1_ota_crc32(0, payload, MANIFEST_CRC_OFFSET);
    write_u32(payload + MANIFEST_CRC_OFFSET, crc);
    return HW1_OTA_OK;
}

hw1_ota_status_t hw1_ota_manifest_parse_untrusted(
    const uint8_t *payload,
    size_t payload_size,
    hw1_ota_manifest_t *manifest)
{
    hw1_ota_manifest_t decoded;
    uint32_t expected_crc;

    if (payload == NULL || manifest == NULL) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    if (payload_size != HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE) {
        return HW1_OTA_ERR_CORRUPT;
    }
    if (read_u32(payload) != HW1_OTA_MANIFEST_MAGIC) {
        return HW1_OTA_ERR_CORRUPT;
    }
    if (read_u16(payload + 4) != HW1_OTA_MANIFEST_FORMAT_VERSION) {
        return HW1_OTA_ERR_UNSUPPORTED_VERSION;
    }
    if (read_u16(payload + 6) != HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE ||
        !bytes_are_zero(payload + 208, 12)) {
        return HW1_OTA_ERR_CORRUPT;
    }
    expected_crc = hw1_ota_crc32(0, payload, MANIFEST_CRC_OFFSET);
    if (expected_crc != read_u32(payload + MANIFEST_CRC_OFFSET)) {
        return HW1_OTA_ERR_CORRUPT;
    }
    if (!wire_text_is_canonical(payload + 8, HW1_OTA_BOARD_ID_SIZE, false) ||
        !wire_text_is_canonical(payload + 32, HW1_OTA_LAYOUT_ID_SIZE, false) ||
        !wire_text_is_canonical(payload + 56, HW1_OTA_PROJECT_NAME_SIZE, false) ||
        !wire_text_is_canonical(payload + 88, HW1_OTA_VERSION_SIZE, false) ||
        !wire_text_is_canonical(payload + 172, HW1_OTA_MIN_UPDATER_VERSION_SIZE, false)) {
        return HW1_OTA_ERR_CORRUPT;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.format_version = read_u16(payload + 4);
    memcpy(decoded.board_id, payload + 8, sizeof(decoded.board_id));
    memcpy(decoded.layout_id, payload + 32, sizeof(decoded.layout_id));
    memcpy(decoded.project_name, payload + 56, sizeof(decoded.project_name));
    memcpy(decoded.version, payload + 88, sizeof(decoded.version));
    decoded.image_size = read_u32(payload + 136);
    memcpy(decoded.image_sha256, payload + 140, HW1_OTA_SHA256_SIZE);
    memcpy(decoded.min_updater_version, payload + 172,
           sizeof(decoded.min_updater_version));
    decoded.data_schema = read_u32(payload + 204);
    if (!manifest_fields_are_valid(&decoded)) {
        return HW1_OTA_ERR_CORRUPT;
    }
    *manifest = decoded;
    return HW1_OTA_OK;
}

hw1_ota_status_t hw1_ota_manifest_verify(
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *signature,
    size_t signature_size,
    hw1_ota_manifest_verify_fn verifier,
    void *verifier_context,
    hw1_ota_verified_manifest_t *verified_manifest)
{
    hw1_ota_manifest_t parsed;
    hw1_ota_status_t status;

    if (verified_manifest == NULL || payload == NULL) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    memset(verified_manifest, 0, sizeof(*verified_manifest));
    status = hw1_ota_manifest_parse_untrusted(payload, payload_size, &parsed);
    if (status != HW1_OTA_OK) {
        return status;
    }
    if (signature == NULL || signature_size == 0 || verifier == NULL) {
        return HW1_OTA_ERR_AUTH_REQUIRED;
    }
    if (signature_size != HW1_OTA_RSA3072_SIGNATURE_SIZE) {
        return HW1_OTA_ERR_SIGNATURE_INVALID;
    }
    status = verifier(verifier_context, &k_signature_spec, payload, payload_size,
                      signature, signature_size);
    if (status != HW1_OTA_OK) {
        return HW1_OTA_ERR_SIGNATURE_INVALID;
    }
    verified_manifest->manifest = parsed;
    verified_manifest->signature_algorithm = HW1_OTA_SIGNATURE_RSA_PSS_SHA256;
    return HW1_OTA_OK;
}

static bool string_ends_with(const char *text, const char *suffix)
{
    const size_t text_len = text == NULL ? 0 : strlen(text);
    const size_t suffix_len = suffix == NULL ? 0 : strlen(suffix);
    return suffix_len != 0 && suffix_len <= text_len &&
           memcmp(text + text_len - suffix_len, suffix, suffix_len) == 0;
}

hw1_ota_status_t hw1_ota_manifest_validate(
    const hw1_ota_verified_manifest_t *verified_manifest,
    const hw1_ota_target_policy_t *policy,
    uint32_t observed_image_size,
    const uint8_t observed_image_sha256[HW1_OTA_SHA256_SIZE],
    uint32_t *mismatches)
{
    const hw1_ota_manifest_t *manifest;
    uint32_t mismatch = 0;
    bool semver_valid = false;

    if (mismatches != NULL) {
        *mismatches = 0;
    }
    if (verified_manifest == NULL || policy == NULL || mismatches == NULL ||
        observed_image_sha256 == NULL || policy->board_id == NULL ||
        policy->layout_id == NULL || policy->project_name == NULL ||
        policy->required_version_suffix == NULL || policy->current_updater_version == NULL ||
        policy->maximum_image_size == 0 ||
        policy->minimum_data_schema > policy->maximum_data_schema ||
        verified_manifest->signature_algorithm != HW1_OTA_SIGNATURE_RSA_PSS_SHA256) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    manifest = &verified_manifest->manifest;
    if (!manifest_fields_are_valid(manifest)) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_FORMAT;
        *mismatches = mismatch;
        return HW1_OTA_ERR_INCOMPATIBLE;
    }
    if (strcmp(manifest->board_id, policy->board_id) != 0) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_BOARD;
    }
    if (strcmp(manifest->layout_id, policy->layout_id) != 0) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_LAYOUT;
    }
    if (strcmp(manifest->project_name, policy->project_name) != 0) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_PROJECT;
    }
    if (!string_ends_with(manifest->version, policy->required_version_suffix)) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_VERSION_SUFFIX;
    }
    if (manifest->image_size > policy->maximum_image_size) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_IMAGE_TOO_LARGE;
    }
    if (manifest->image_size != observed_image_size) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_IMAGE_SIZE;
    }
    if (memcmp(manifest->image_sha256, observed_image_sha256, HW1_OTA_SHA256_SIZE) != 0) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_IMAGE_SHA256;
    }
    if (hw1_ota_semver_compare(policy->current_updater_version,
                               manifest->min_updater_version, &semver_valid) < 0 || !semver_valid) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_UPDATER_VERSION;
    }
    if (manifest->data_schema < policy->minimum_data_schema ||
        manifest->data_schema > policy->maximum_data_schema) {
        mismatch |= HW1_OTA_MANIFEST_MISMATCH_DATA_SCHEMA;
    }
    *mismatches = mismatch;
    return mismatch == 0 ? HW1_OTA_OK : HW1_OTA_ERR_INCOMPATIBLE;
}

void hw1_ota_record_init(hw1_ota_record_t *record)
{
    if (record == NULL) {
        return;
    }
    memset(record, 0, sizeof(*record));
    record->phase = HW1_OTA_PHASE_IDLE;
    record->source = HW1_OTA_SOURCE_NONE;
}

hw1_ota_status_t hw1_ota_record_validate(const hw1_ota_record_t *record)
{
    uint8_t manifest_payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];

    if (record == NULL || !phase_is_valid(record->phase) || !source_is_valid(record->source) ||
        (record->request_flags & ~HW1_OTA_REQUEST_KNOWN_FLAGS) != 0 ||
        !result_code_is_valid(record->last_result.code) ||
        !detail_is_canonical(record->last_result.detail,
                             sizeof(record->last_result.detail))) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    if (record->phase == HW1_OTA_PHASE_IDLE) {
        if (record->operation_id != 0 || record->source != HW1_OTA_SOURCE_NONE ||
            record->request_flags != 0 || record->trial_boot_count != 0 ||
            record->candidate_present) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
    } else if (record->operation_id == 0 || record->source == HW1_OTA_SOURCE_NONE) {
        return HW1_OTA_ERR_INVALID_STATE;
    }
    if (record->candidate_present) {
        if (record->candidate.signature_algorithm != HW1_OTA_SIGNATURE_RSA_PSS_SHA256 ||
            hw1_ota_manifest_encode(&record->candidate.manifest, manifest_payload) != HW1_OTA_OK) {
            return HW1_OTA_ERR_INVALID_ARG;
        }
    }
    if (record->phase >= HW1_OTA_PHASE_IMAGE_VERIFIED &&
        record->phase <= HW1_OTA_PHASE_SUCCEEDED && !record->candidate_present) {
        return HW1_OTA_ERR_INVALID_STATE;
    }
    if (record->last_result.sequence == 0) {
        if (record->last_result.operation_id != 0 ||
            record->last_result.code != HW1_OTA_RESULT_NONE ||
            record->last_result.native_error != 0 || record->last_result.detail[0] != '\0' ||
            record->acknowledged_result_sequence != 0) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
    } else if (record->last_result.operation_id == 0 ||
               record->last_result.code == HW1_OTA_RESULT_NONE ||
               !phase_is_terminal(record->last_result.terminal_phase)) {
        return HW1_OTA_ERR_INVALID_STATE;
    }
    if (phase_is_terminal(record->phase)) {
        if (record->last_result.operation_id != record->operation_id ||
            record->last_result.terminal_phase != record->phase) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        if ((record->phase == HW1_OTA_PHASE_SUCCEEDED &&
             record->last_result.code != HW1_OTA_RESULT_SUCCESS) ||
            (record->phase == HW1_OTA_PHASE_CANCELED &&
             record->last_result.code != HW1_OTA_RESULT_CANCELED) ||
            (record->phase == HW1_OTA_PHASE_FAILED &&
             (record->last_result.code == HW1_OTA_RESULT_SUCCESS ||
              record->last_result.code == HW1_OTA_RESULT_CANCELED))) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
    } else if (record->phase != HW1_OTA_PHASE_IDLE &&
               hw1_ota_result_pending(record) &&
               (record->request_flags &
                HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT) == 0) {
        /* A nonterminal operation may carry an older pending result only when
         * its emergency supersession policy is explicit in the journal. */
        return HW1_OTA_ERR_INVALID_STATE;
    }
    return HW1_OTA_OK;
}

hw1_ota_status_t hw1_ota_record_encode(
    const hw1_ota_record_t *record,
    uint8_t wire[HW1_OTA_RECORD_WIRE_SIZE])
{
    uint32_t crc;

    if (wire == NULL || hw1_ota_record_validate(record) != HW1_OTA_OK) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    memset(wire, 0, HW1_OTA_RECORD_WIRE_SIZE);
    write_u32(wire, HW1_OTA_RECORD_MAGIC);
    write_u16(wire + 4, HW1_OTA_RECORD_FORMAT_VERSION);
    write_u16(wire + 6, HW1_OTA_RECORD_WIRE_SIZE);
    write_u32(wire + 8, record->sequence);
    write_u64(wire + 16, record->operation_id);
    wire[24] = (uint8_t)record->phase;
    wire[25] = (uint8_t)record->source;
    write_u16(wire + 26, record->request_flags);
    write_u32(wire + 28, record->trial_boot_count);
    wire[32] = record->candidate_present ? 1u : 0u;
    if (record->candidate_present) {
        wire[33] = (uint8_t)record->candidate.signature_algorithm;
        if (hw1_ota_manifest_encode(&record->candidate.manifest, wire + 36) != HW1_OTA_OK) {
            return HW1_OTA_ERR_INVALID_ARG;
        }
    }
    write_u32(wire + 260, record->last_result.sequence);
    write_u64(wire + 264, record->last_result.operation_id);
    write_u16(wire + 272, (uint16_t)record->last_result.code);
    wire[274] = (uint8_t)record->last_result.terminal_phase;
    write_u32(wire + 276, (uint32_t)record->last_result.native_error);
    memcpy(wire + 280, record->last_result.detail, sizeof(record->last_result.detail));
    write_u32(wire + 376, record->acknowledged_result_sequence);
    crc = hw1_ota_crc32(0, wire, RECORD_CRC_OFFSET);
    write_u32(wire + RECORD_CRC_OFFSET, crc);
    return HW1_OTA_OK;
}

hw1_ota_status_t hw1_ota_record_decode(
    const uint8_t *wire,
    size_t wire_size,
    hw1_ota_record_t *record)
{
    hw1_ota_record_t decoded;
    hw1_ota_status_t status;

    if (wire == NULL || record == NULL) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    if (wire_size != HW1_OTA_RECORD_WIRE_SIZE ||
        read_u32(wire) != HW1_OTA_RECORD_MAGIC ||
        read_u16(wire + 6) != HW1_OTA_RECORD_WIRE_SIZE ||
        !bytes_are_zero(wire + 12, 4) || !bytes_are_zero(wire + 34, 2) ||
        wire[32] > 1 || !detail_is_canonical((const char *)(wire + 280),
                                             HW1_OTA_RESULT_DETAIL_SIZE)) {
        return HW1_OTA_ERR_CORRUPT;
    }
    if (read_u16(wire + 4) != HW1_OTA_RECORD_FORMAT_VERSION) {
        return HW1_OTA_ERR_UNSUPPORTED_VERSION;
    }
    if (hw1_ota_crc32(0, wire, RECORD_CRC_OFFSET) != read_u32(wire + RECORD_CRC_OFFSET)) {
        return HW1_OTA_ERR_CORRUPT;
    }
    hw1_ota_record_init(&decoded);
    decoded.sequence = read_u32(wire + 8);
    decoded.operation_id = read_u64(wire + 16);
    decoded.phase = (hw1_ota_phase_t)wire[24];
    decoded.source = (hw1_ota_source_t)wire[25];
    decoded.request_flags = read_u16(wire + 26);
    decoded.trial_boot_count = read_u32(wire + 28);
    decoded.candidate_present = wire[32] != 0;
    if (decoded.candidate_present) {
        if (wire[33] != HW1_OTA_SIGNATURE_RSA_PSS_SHA256) {
            return HW1_OTA_ERR_CORRUPT;
        }
        status = hw1_ota_manifest_parse_untrusted(wire + 36,
                                                  HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE,
                                                  &decoded.candidate.manifest);
        if (status != HW1_OTA_OK) {
            return HW1_OTA_ERR_CORRUPT;
        }
        decoded.candidate.signature_algorithm = HW1_OTA_SIGNATURE_RSA_PSS_SHA256;
    } else if (wire[33] != 0 ||
               !bytes_are_zero(wire + 36, HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE)) {
        return HW1_OTA_ERR_CORRUPT;
    }
    decoded.last_result.sequence = read_u32(wire + 260);
    decoded.last_result.operation_id = read_u64(wire + 264);
    decoded.last_result.code = (hw1_ota_result_code_t)read_u16(wire + 272);
    decoded.last_result.terminal_phase = (hw1_ota_phase_t)wire[274];
    if (wire[275] != 0) {
        return HW1_OTA_ERR_CORRUPT;
    }
    decoded.last_result.native_error = (int32_t)read_u32(wire + 276);
    memcpy(decoded.last_result.detail, wire + 280, sizeof(decoded.last_result.detail));
    decoded.acknowledged_result_sequence = read_u32(wire + 376);
    status = hw1_ota_record_validate(&decoded);
    if (status != HW1_OTA_OK) {
        return HW1_OTA_ERR_CORRUPT;
    }
    *record = decoded;
    return HW1_OTA_OK;
}

hw1_ota_status_t hw1_ota_begin(
    hw1_ota_record_t *record,
    uint64_t operation_id,
    hw1_ota_source_t source,
    uint16_t request_flags,
    const hw1_ota_verified_manifest_t *candidate)
{
    uint8_t payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];

    if (record == NULL || hw1_ota_record_validate(record) != HW1_OTA_OK ||
        operation_id == 0 || source == HW1_OTA_SOURCE_NONE ||
        !source_is_valid(source) || (request_flags & ~HW1_OTA_REQUEST_KNOWN_FLAGS) != 0 ||
        (!phase_is_terminal(record->phase) && record->phase != HW1_OTA_PHASE_IDLE) ||
        (hw1_ota_result_pending(record) &&
         (request_flags & HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT) == 0) ||
        (record->last_result.sequence != 0 &&
         operation_id == record->last_result.operation_id)) {
        return HW1_OTA_ERR_INVALID_STATE;
    }
    if (candidate != NULL &&
        (candidate->signature_algorithm != HW1_OTA_SIGNATURE_RSA_PSS_SHA256 ||
         hw1_ota_manifest_encode(&candidate->manifest, payload) != HW1_OTA_OK)) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    record->operation_id = operation_id;
    record->phase = HW1_OTA_PHASE_REQUESTED;
    record->source = source;
    record->request_flags = request_flags;
    record->trial_boot_count = 0;
    record->candidate_present = candidate != NULL;
    memset(&record->candidate, 0, sizeof(record->candidate));
    if (candidate != NULL) {
        record->candidate = *candidate;
    }
    return HW1_OTA_OK;
}

static hw1_ota_status_t set_terminal_result(
    hw1_ota_record_t *record,
    hw1_ota_phase_t phase,
    hw1_ota_result_code_t code,
    int32_t native_error,
    const char *detail)
{
    hw1_ota_result_t result;

    memset(&result, 0, sizeof(result));
    result.sequence = next_nonzero_counter(record->last_result.sequence);
    result.operation_id = record->operation_id;
    result.code = code;
    result.terminal_phase = phase;
    result.native_error = native_error;
    if (!copy_text(result.detail, sizeof(result.detail), detail == NULL ? "" : detail, true)) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    record->last_result = result;
    record->phase = phase;
    return HW1_OTA_OK;
}

hw1_ota_status_t hw1_ota_transition(
    hw1_ota_record_t *record,
    hw1_ota_event_t event,
    const hw1_ota_transition_args_t *args)
{
    const hw1_ota_transition_args_t empty_args = {0};
    uint8_t payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];

    if (record == NULL || hw1_ota_record_validate(record) != HW1_OTA_OK) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    if (args == NULL) {
        args = &empty_args;
    }
    switch (event) {
    case HW1_OTA_EVENT_ARM_RECOVERY_BOOT:
        if (record->phase != HW1_OTA_PHASE_REQUESTED) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        record->phase = HW1_OTA_PHASE_RECOVERY_BOOT_ARMED;
        break;
    case HW1_OTA_EVENT_RECOVERY_STARTED:
        if (record->phase != HW1_OTA_PHASE_REQUESTED &&
            record->phase != HW1_OTA_PHASE_RECOVERY_BOOT_ARMED) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        record->phase = HW1_OTA_PHASE_RECOVERY_RUNNING;
        break;
    case HW1_OTA_EVENT_APPLY_STARTED:
        if (record->phase != HW1_OTA_PHASE_RECOVERY_RUNNING) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        record->phase = HW1_OTA_PHASE_APPLYING;
        break;
    case HW1_OTA_EVENT_IMAGE_VERIFIED:
        if (record->phase != HW1_OTA_PHASE_APPLYING || args->verified_manifest == NULL ||
            args->verified_manifest->signature_algorithm != HW1_OTA_SIGNATURE_RSA_PSS_SHA256 ||
            hw1_ota_manifest_encode(&args->verified_manifest->manifest, payload) != HW1_OTA_OK) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        record->candidate = *args->verified_manifest;
        record->candidate_present = true;
        record->phase = HW1_OTA_PHASE_IMAGE_VERIFIED;
        break;
    case HW1_OTA_EVENT_ARM_TRIAL_BOOT:
        if (record->phase != HW1_OTA_PHASE_IMAGE_VERIFIED) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        record->phase = HW1_OTA_PHASE_TRIAL_BOOT_ARMED;
        break;
    case HW1_OTA_EVENT_TRIAL_STARTED:
        if (record->phase != HW1_OTA_PHASE_TRIAL_BOOT_ARMED &&
            record->phase != HW1_OTA_PHASE_IMAGE_VERIFIED) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        record->phase = HW1_OTA_PHASE_TRIAL_RUNNING;
        record->trial_boot_count = next_nonzero_counter(record->trial_boot_count);
        break;
    case HW1_OTA_EVENT_MARK_VALID:
        if (record->phase != HW1_OTA_PHASE_TRIAL_RUNNING) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        return set_terminal_result(record, HW1_OTA_PHASE_SUCCEEDED,
                                   HW1_OTA_RESULT_SUCCESS, args->native_error,
                                   args->detail);
    case HW1_OTA_EVENT_FAIL:
        if (record->phase == HW1_OTA_PHASE_IDLE || phase_is_terminal(record->phase) ||
            args->result_code == HW1_OTA_RESULT_NONE ||
            args->result_code == HW1_OTA_RESULT_SUCCESS ||
            args->result_code == HW1_OTA_RESULT_CANCELED) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        return set_terminal_result(record, HW1_OTA_PHASE_FAILED,
                                   args->result_code, args->native_error,
                                   args->detail);
    case HW1_OTA_EVENT_CANCEL:
        if (record->phase != HW1_OTA_PHASE_REQUESTED &&
            record->phase != HW1_OTA_PHASE_RECOVERY_BOOT_ARMED &&
            record->phase != HW1_OTA_PHASE_RECOVERY_RUNNING &&
            record->phase != HW1_OTA_PHASE_APPLYING) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        return set_terminal_result(record, HW1_OTA_PHASE_CANCELED,
                                   HW1_OTA_RESULT_CANCELED, args->native_error,
                                   args->detail);
    case HW1_OTA_EVENT_ROLLBACK_OBSERVED:
        if (record->phase != HW1_OTA_PHASE_TRIAL_BOOT_ARMED &&
            record->phase != HW1_OTA_PHASE_TRIAL_RUNNING) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        return set_terminal_result(record, HW1_OTA_PHASE_FAILED,
                                   HW1_OTA_RESULT_ROLLBACK_DETECTED,
                                   args->native_error, args->detail);
    case HW1_OTA_EVENT_CLEAR_TERMINAL:
        if (!phase_is_terminal(record->phase)) {
            return HW1_OTA_ERR_INVALID_STATE;
        }
        record->operation_id = 0;
        record->phase = HW1_OTA_PHASE_IDLE;
        record->source = HW1_OTA_SOURCE_NONE;
        record->request_flags = 0;
        record->trial_boot_count = 0;
        record->candidate_present = false;
        memset(&record->candidate, 0, sizeof(record->candidate));
        break;
    default:
        return HW1_OTA_ERR_INVALID_ARG;
    }
    return hw1_ota_record_validate(record);
}

bool hw1_ota_result_pending(const hw1_ota_record_t *record)
{
    return record != NULL && record->last_result.sequence != 0 &&
           record->last_result.sequence != record->acknowledged_result_sequence;
}

bool hw1_ota_record_candidate_matches_verified(
    const hw1_ota_record_t *record,
    const hw1_ota_verified_manifest_t *freshly_verified_manifest)
{
    uint8_t stored_payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];
    uint8_t fresh_payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];

    if (record == NULL || freshly_verified_manifest == NULL ||
        !record->candidate_present ||
        hw1_ota_record_validate(record) != HW1_OTA_OK ||
        freshly_verified_manifest->signature_algorithm !=
            HW1_OTA_SIGNATURE_RSA_PSS_SHA256 ||
        record->candidate.signature_algorithm !=
            freshly_verified_manifest->signature_algorithm ||
        hw1_ota_manifest_encode(&record->candidate.manifest, stored_payload) !=
            HW1_OTA_OK ||
        hw1_ota_manifest_encode(&freshly_verified_manifest->manifest,
                                fresh_payload) != HW1_OTA_OK) {
        return false;
    }
    return memcmp(stored_payload, fresh_payload, sizeof(stored_payload)) == 0;
}

hw1_ota_status_t hw1_ota_acknowledge_result(
    hw1_ota_record_t *record,
    uint32_t result_sequence)
{
    if (record == NULL || result_sequence == 0 ||
        result_sequence != record->last_result.sequence) {
        return HW1_OTA_ERR_CONFLICT;
    }
    record->acknowledged_result_sequence = result_sequence;
    return HW1_OTA_OK;
}

static void recommend_event(hw1_ota_reconcile_decision_t *decision,
                            hw1_ota_event_t event)
{
    decision->event_before_actions = event;
    decision->actions |= HW1_OTA_ACTION_COMMIT_EVENT;
}

static void recommend_boot_recovery(const hw1_ota_observation_t *observation,
                                    hw1_ota_reconcile_decision_t *decision)
{
    if (observation->configured_boot_target != HW1_OTA_BOOT_TARGET_RECOVERY) {
        decision->actions |= HW1_OTA_ACTION_SET_BOOT_RECOVERY;
    }
    decision->actions |= HW1_OTA_ACTION_REBOOT;
}

static void recommend_boot_main(const hw1_ota_observation_t *observation,
                                hw1_ota_reconcile_decision_t *decision)
{
    if (observation->configured_boot_target != HW1_OTA_BOOT_TARGET_MAIN) {
        decision->actions |= HW1_OTA_ACTION_SET_BOOT_MAIN;
    }
    decision->actions |= HW1_OTA_ACTION_REBOOT;
}

hw1_ota_status_t hw1_ota_reconcile(
    const hw1_ota_record_t *record,
    const hw1_ota_observation_t *observation,
    hw1_ota_reconcile_decision_t *decision)
{
    if (record == NULL || observation == NULL || decision == NULL ||
        hw1_ota_record_validate(record) != HW1_OTA_OK ||
        observation->running_role < HW1_OTA_ROLE_MAIN ||
        observation->running_role > HW1_OTA_ROLE_RECOVERY ||
        observation->configured_boot_target > HW1_OTA_BOOT_TARGET_RECOVERY ||
        observation->main_image_state > HW1_OTA_IMAGE_ACCEPTED) {
        return HW1_OTA_ERR_INVALID_ARG;
    }
    memset(decision, 0, sizeof(*decision));
    if (hw1_ota_result_pending(record)) {
        decision->actions |= HW1_OTA_ACTION_REPORT_RESULT;
    }
    switch (record->phase) {
    case HW1_OTA_PHASE_IDLE:
        if (observation->running_role == HW1_OTA_ROLE_RECOVERY) {
            if (observation->main_image_state == HW1_OTA_IMAGE_VALID ||
                observation->main_image_state == HW1_OTA_IMAGE_ACCEPTED) {
                recommend_boot_main(observation, decision);
            } else {
                decision->actions |= HW1_OTA_ACTION_HOLD_FOR_OPERATOR;
            }
        }
        break;
    case HW1_OTA_PHASE_REQUESTED:
        if (observation->running_role == HW1_OTA_ROLE_RECOVERY) {
            recommend_event(decision, HW1_OTA_EVENT_RECOVERY_STARTED);
        } else if (record->source == HW1_OTA_SOURCE_RECOVERY_UPLOAD) {
            /* A staged-file REQUESTED record means "validated and waiting for
             * operator launch", not "boot recovery now". Direct recovery has
             * no separate stage/launch gesture, so only that source replays
             * an interrupted main-side boot-arm operation automatically. */
            recommend_event(decision, HW1_OTA_EVENT_ARM_RECOVERY_BOOT);
            recommend_boot_recovery(observation, decision);
        }
        break;
    case HW1_OTA_PHASE_RECOVERY_BOOT_ARMED:
        if (observation->running_role == HW1_OTA_ROLE_RECOVERY) {
            recommend_event(decision, HW1_OTA_EVENT_RECOVERY_STARTED);
        } else {
            recommend_boot_recovery(observation, decision);
        }
        break;
    case HW1_OTA_PHASE_RECOVERY_RUNNING:
        if (observation->running_role != HW1_OTA_ROLE_RECOVERY) {
            recommend_boot_recovery(observation, decision);
        } else if (record->source == HW1_OTA_SOURCE_STAGED_FILE &&
                   !observation->staged_candidate_available) {
            decision->actions |= HW1_OTA_ACTION_HOLD_FOR_OPERATOR;
        } else {
            recommend_event(decision, HW1_OTA_EVENT_APPLY_STARTED);
            decision->actions |= HW1_OTA_ACTION_START_APPLY;
        }
        break;
    case HW1_OTA_PHASE_APPLYING:
        if (observation->running_role != HW1_OTA_ROLE_RECOVERY) {
            recommend_boot_recovery(observation, decision);
        } else if ((observation->main_image_state == HW1_OTA_IMAGE_VALID ||
                    observation->main_image_state == HW1_OTA_IMAGE_ACCEPTED) &&
                   record->candidate_present) {
            recommend_event(decision, HW1_OTA_EVENT_IMAGE_VERIFIED);
        } else if (record->source != HW1_OTA_SOURCE_STAGED_FILE ||
                   observation->staged_candidate_available) {
            decision->actions |= HW1_OTA_ACTION_RESUME_APPLY;
        } else {
            decision->actions |= HW1_OTA_ACTION_HOLD_FOR_OPERATOR;
        }
        break;
    case HW1_OTA_PHASE_IMAGE_VERIFIED:
        if (observation->running_role == HW1_OTA_ROLE_MAIN) {
            recommend_event(decision, HW1_OTA_EVENT_TRIAL_STARTED);
        } else {
            recommend_event(decision, HW1_OTA_EVENT_ARM_TRIAL_BOOT);
            recommend_boot_main(observation, decision);
        }
        break;
    case HW1_OTA_PHASE_TRIAL_BOOT_ARMED:
        if (observation->running_role == HW1_OTA_ROLE_MAIN) {
            recommend_event(decision, HW1_OTA_EVENT_TRIAL_STARTED);
        } else if (observation->main_image_state == HW1_OTA_IMAGE_VALID ||
                   observation->main_image_state == HW1_OTA_IMAGE_PENDING_VERIFY ||
                   observation->main_image_state == HW1_OTA_IMAGE_ACCEPTED) {
            recommend_boot_main(observation, decision);
        } else {
            recommend_event(decision, HW1_OTA_EVENT_ROLLBACK_OBSERVED);
            decision->event_result_code = HW1_OTA_RESULT_ROLLBACK_DETECTED;
            decision->actions |= HW1_OTA_ACTION_HOLD_FOR_OPERATOR;
        }
        break;
    case HW1_OTA_PHASE_TRIAL_RUNNING:
        if (observation->running_role == HW1_OTA_ROLE_RECOVERY) {
            recommend_event(decision, HW1_OTA_EVENT_ROLLBACK_OBSERVED);
            decision->event_result_code = HW1_OTA_RESULT_ROLLBACK_DETECTED;
            decision->actions |= HW1_OTA_ACTION_HOLD_FOR_OPERATOR;
        } else if (observation->main_image_state == HW1_OTA_IMAGE_ACCEPTED) {
            recommend_event(decision, HW1_OTA_EVENT_MARK_VALID);
            decision->event_result_code = HW1_OTA_RESULT_SUCCESS;
        } else if (observation->main_image_state == HW1_OTA_IMAGE_PENDING_VERIFY ||
                   observation->main_image_state == HW1_OTA_IMAGE_VALID) {
            decision->actions |= HW1_OTA_ACTION_RUN_HEALTH_CHECK;
        } else {
            recommend_boot_recovery(observation, decision);
        }
        break;
    case HW1_OTA_PHASE_SUCCEEDED:
    case HW1_OTA_PHASE_CANCELED:
        if (observation->running_role == HW1_OTA_ROLE_RECOVERY &&
            (observation->main_image_state == HW1_OTA_IMAGE_VALID ||
             observation->main_image_state == HW1_OTA_IMAGE_ACCEPTED)) {
            recommend_boot_main(observation, decision);
        }
        break;
    case HW1_OTA_PHASE_FAILED:
        if (observation->running_role == HW1_OTA_ROLE_RECOVERY) {
            decision->actions |= HW1_OTA_ACTION_HOLD_FOR_OPERATOR;
        }
        break;
    default:
        return HW1_OTA_ERR_INVALID_STATE;
    }
    return HW1_OTA_OK;
}
