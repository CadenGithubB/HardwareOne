#include "updater_preflight.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_task_wdt.h"
#include "hw1_ota_idf.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

/*
 * Expected partition geometry. Supplied by updater/CMakeLists.txt, parsed out
 * of the same partitions_ota_*.csv this firmware is built against -- see the
 * comment there. These were literals holding one board's numbers, and three of
 * them had already gone stale for that board.
 */
#if !defined(HW1_FACTORY_OFFSET) || !defined(HW1_FACTORY_SIZE) || \
    !defined(HW1_OTADATA_OFFSET) || !defined(HW1_OTADATA_SIZE) || \
    !defined(HW1_OTA0_OFFSET) || !defined(HW1_LITTLEFS_OFFSET) || \
    !defined(HW1_LITTLEFS_SIZE)
#error "The recovery layout gate needs HW1_* partition geometry from the build. \
        Build via updater/CMakeLists.txt; do not define these by hand."
#endif
#define HW1_MANIFEST_ENVELOPE_MAX 2048u
#define HW1_HASH_CHUNK 4096u

extern const uint8_t _binary_hw1_ota_public_key_pem_start[];
extern const uint8_t _binary_hw1_ota_public_key_pem_end[];

static void feed_current_task_watchdog(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        (void)esp_task_wdt_reset();
    }
}

static esp_err_t fail(char *reason, size_t reason_size, esp_err_t err,
                      const char *format, ...)
{
    if (reason != NULL && reason_size != 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(reason, reason_size, format, args);
        va_end(args);
    }
    return err;
}

static bool partition_matches(const esp_partition_t *partition,
                              uint32_t address, uint32_t size)
{
    return partition != NULL && partition->address == address &&
           partition->size == size;
}

static bool fixed_field_to_string(const char *field, size_t field_size,
                                  char *output, size_t output_size)
{
    const char *end;
    size_t length;
    if (field == NULL || output == NULL || output_size == 0) {
        return false;
    }
    end = memchr(field, '\0', field_size);
    if (end == NULL) {
        return false;
    }
    length = (size_t)(end - field);
    if (length == 0 || length >= output_size) {
        return false;
    }
    memcpy(output, field, length);
    output[length] = '\0';
    return true;
}

static const hw1_ota_target_policy_t *target_policy(void)
{
    static const hw1_ota_target_policy_t policy = {
        .board_id = HW1_OTA_BOARD_ID,
        .layout_id = HW1_OTA_LAYOUT_ID,
        .project_name = "hardwareone-idf",
        .required_version_suffix = HW1_OTA_VERSION_SUFFIX,
        .current_updater_version = HW1_OTA_UPDATER_VERSION,
        .maximum_image_size = HW1_UPDATER_OTA0_SIZE,
        .minimum_data_schema = HW1_UPDATER_DATA_SCHEMA,
        .maximum_data_schema = HW1_UPDATER_DATA_SCHEMA,
    };
    return &policy;
}

esp_err_t updater_validate_layout(const esp_partition_t **ota0,
                                  char *reason, size_t reason_size)
{
    const esp_partition_t *running;
    const esp_partition_t *factory;
    const esp_partition_t *candidate;
    const esp_partition_t *otadata;
    const esp_partition_t *littlefs;
    esp_app_desc_t desc;
    char project[sizeof(desc.project_name) + 1];
    char version[sizeof(desc.version) + 1];

    if (ota0 == NULL) {
        return fail(reason, reason_size, ESP_ERR_INVALID_ARG,
                    "null layout output");
    }
    *ota0 = NULL;
    running = esp_ota_get_running_partition();
    factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
    candidate = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    littlefs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY, "littlefs");

    if (running == NULL || factory == NULL || running != factory ||
        !partition_matches(factory, HW1_FACTORY_OFFSET, HW1_FACTORY_SIZE)) {
        return fail(reason, reason_size, ESP_ERR_INVALID_STATE,
                    "updater is not running from the expected factory slot");
    }
    if (!partition_matches(candidate, HW1_OTA0_OFFSET,
                           HW1_UPDATER_OTA0_SIZE) ||
        !partition_matches(otadata, HW1_OTADATA_OFFSET,
                           HW1_OTADATA_SIZE) ||
        !partition_matches(littlefs, HW1_LITTLEFS_OFFSET,
                           HW1_LITTLEFS_SIZE)) {
        return fail(reason, reason_size, ESP_ERR_INVALID_SIZE,
                    "partition layout does not match %s", HW1_OTA_LAYOUT_ID);
    }
    memset(&desc, 0, sizeof(desc));
    if (esp_ota_get_partition_description(factory, &desc) != ESP_OK ||
        !fixed_field_to_string(desc.project_name, sizeof(desc.project_name),
                               project, sizeof(project)) ||
        !fixed_field_to_string(desc.version, sizeof(desc.version),
                               version, sizeof(version)) ||
        strcmp(project, "hw1-updater") != 0 ||
        strcmp(version, HW1_OTA_UPDATER_VERSION) != 0) {
        return fail(reason, reason_size, ESP_ERR_INVALID_VERSION,
                    "factory updater identity/version mismatch");
    }
    *ota0 = candidate;
    return ESP_OK;
}

esp_err_t updater_validate_existing_ota0(const esp_partition_t *ota0,
                                         bool *valid,
                                         esp_ota_img_states_t *state)
{
    esp_partition_pos_t position;
    esp_image_metadata_t metadata;
    esp_err_t state_err;
    esp_err_t verify_err;
    if (ota0 == NULL || valid == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *valid = false;
    *state = ESP_OTA_IMG_UNDEFINED;
    position.offset = ota0->address;
    position.size = ota0->size;
    memset(&metadata, 0, sizeof(metadata));
    metadata.start_addr = ota0->address;
    verify_err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata);
    *valid = verify_err == ESP_OK;
    /* Separate "this image is not valid" from "the check could not be
     * completed". Only ESP_ERR_IMAGE_INVALID is evidence the bootloader would
     * reject the image; a flash read or mmap failure must never be mistaken for
     * one, because callers turn a rejected main image into a TERMINAL rollback
     * verdict. Collapsing both into valid=false let a transient read error burn
     * a healthy transaction to FAILED. */
    if (verify_err != ESP_OK && verify_err != ESP_ERR_IMAGE_INVALID) {
        return verify_err;
    }
    state_err = esp_ota_get_state_partition(ota0, state);
    if (state_err == ESP_ERR_NOT_FOUND || state_err == ESP_ERR_INVALID_ARG) {
        *state = ESP_OTA_IMG_UNDEFINED;
        return ESP_OK;
    }
    return state_err;
}

static bool decode_base64_exact(const char *encoded, size_t encoded_size,
                                uint8_t *output, size_t output_size)
{
    size_t decoded = 0;
    return encoded != NULL && output != NULL &&
           mbedtls_base64_decode(output, output_size, &decoded,
               (const unsigned char *)encoded, encoded_size) == 0 &&
           decoded == output_size;
}

esp_err_t updater_verify_manifest_envelope(
    const uint8_t *json, size_t json_size,
    hw1_ota_verified_manifest_t *verified,
    char *reason, size_t reason_size)
{
    cJSON *root = NULL;
    const cJSON *format;
    const cJSON *format_version;
    const cJSON *payload_value;
    const cJSON *signature_object;
    const cJSON *algorithm;
    const cJSON *signature_value;
    uint8_t payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE];
    uint8_t signature[HW1_OTA_RSA3072_SIGNATURE_SIZE];
    hw1_ota_idf_rsa_public_key_t key;
    hw1_ota_status_t status;
    char *json_text = NULL;
    const char *parse_end = NULL;
    esp_err_t err = ESP_OK;

    if (json == NULL || verified == NULL || json_size == 0 ||
        json_size > HW1_MANIFEST_ENVELOPE_MAX ||
        memchr(json, '\0', json_size) != NULL) {
        return fail(reason, reason_size, ESP_ERR_INVALID_SIZE,
                    "manifest envelope size/content is invalid");
    }
    memset(verified, 0, sizeof(*verified));
    memset(payload, 0, sizeof(payload));
    memset(signature, 0, sizeof(signature));
    json_text = malloc(json_size + 1u);
    if (json_text == NULL) {
        return fail(reason, reason_size, ESP_ERR_NO_MEM,
                    "manifest JSON allocation failed");
    }
    memcpy(json_text, json, json_size);
    json_text[json_size] = '\0';
    root = cJSON_ParseWithOpts(json_text, &parse_end, true);
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 4 ||
        parse_end != json_text + json_size) {
        err = fail(reason, reason_size, ESP_ERR_INVALID_ARG,
                   "manifest envelope is malformed");
        goto done;
    }
    format = cJSON_GetObjectItemCaseSensitive(root, "format");
    format_version = cJSON_GetObjectItemCaseSensitive(root, "formatVersion");
    payload_value = cJSON_GetObjectItemCaseSensitive(root, "payload");
    signature_object = cJSON_GetObjectItemCaseSensitive(root, "signature");
    algorithm = cJSON_IsObject(signature_object)
                    ? cJSON_GetObjectItemCaseSensitive(signature_object,
                                                       "algorithm")
                    : NULL;
    signature_value = cJSON_IsObject(signature_object)
                    ? cJSON_GetObjectItemCaseSensitive(signature_object,
                                                       "value")
                    : NULL;
    if (!cJSON_IsString(format) || !cJSON_IsNumber(format_version) ||
        !cJSON_IsString(payload_value) ||
        !cJSON_IsObject(signature_object) ||
        cJSON_GetArraySize(signature_object) != 2 ||
        !cJSON_IsString(algorithm) || !cJSON_IsString(signature_value) ||
        strcmp(format->valuestring, HW1_OTA_MANIFEST_ENVELOPE_FORMAT) != 0 ||
        format_version->valuedouble != HW1_OTA_MANIFEST_FORMAT_VERSION ||
        strcmp(algorithm->valuestring,
               HW1_OTA_MANIFEST_SIGNATURE_ALGORITHM) != 0 ||
        strlen(payload_value->valuestring) != 300u ||
        strlen(signature_value->valuestring) != 512u ||
        !decode_base64_exact(payload_value->valuestring, 300u,
                             payload, sizeof(payload)) ||
        !decode_base64_exact(signature_value->valuestring, 512u,
                             signature, sizeof(signature))) {
        err = fail(reason, reason_size, ESP_ERR_INVALID_ARG,
                   "manifest envelope fields/signature encoding are invalid");
        goto done;
    }

    key.public_key_pem = _binary_hw1_ota_public_key_pem_start;
    key.public_key_pem_size = (size_t)(_binary_hw1_ota_public_key_pem_end -
                                       _binary_hw1_ota_public_key_pem_start);
    status = hw1_ota_manifest_verify(
        payload, sizeof(payload), signature, sizeof(signature),
        hw1_ota_idf_rsa3072_pss_sha256_verify, &key, verified);
    if (status != HW1_OTA_OK) {
        err = fail(reason, reason_size, ESP_ERR_INVALID_RESPONSE,
                   "manifest RSA-3072-PSS signature rejected (%d)",
                   (int)status);
    }

done:
    cJSON_Delete(root);
    if (json_text != NULL) {
        memset(json_text, 0, json_size + 1u);
        free(json_text);
    }
    memset(payload, 0, sizeof(payload));
    memset(signature, 0, sizeof(signature));
    return err;
}

esp_err_t updater_validate_manifest_contract(
    const hw1_ota_verified_manifest_t *verified,
    uint32_t observed_size,
    const uint8_t observed_sha256[HW1_OTA_SHA256_SIZE],
    char *reason, size_t reason_size)
{
    uint32_t mismatches = 0;
    hw1_ota_status_t status = hw1_ota_manifest_validate(
        verified, target_policy(), observed_size, observed_sha256, &mismatches);
    if (status != HW1_OTA_OK) {
        return fail(reason, reason_size, ESP_ERR_INVALID_VERSION,
                    "signed manifest/image policy mismatch 0x%08" PRIx32,
                    mismatches);
    }
    return ESP_OK;
}

esp_err_t updater_validate_image_prefix(
    const uint8_t *prefix, size_t prefix_size,
    const hw1_ota_verified_manifest_t *verified,
    esp_app_desc_t *app_desc,
    char *reason, size_t reason_size)
{
    esp_image_header_t header;
    esp_image_segment_header_t first_segment;
    char project[sizeof(app_desc->project_name) + 1];
    char version[sizeof(app_desc->version) + 1];
    size_t offset = 0;
    if (prefix == NULL || verified == NULL || app_desc == NULL ||
        prefix_size < HW1_UPDATER_IMAGE_PREFIX_SIZE) {
        return fail(reason, reason_size, ESP_ERR_INVALID_SIZE,
                    "candidate image header is truncated");
    }
    memcpy(&header, prefix + offset, sizeof(header));
    offset += sizeof(header);
    memcpy(&first_segment, prefix + offset, sizeof(first_segment));
    offset += sizeof(first_segment);
    memcpy(app_desc, prefix + offset, sizeof(*app_desc));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0 ||
        header.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
        header.chip_id != HW1_OTA_EXPECTED_CHIP_ID || header.hash_appended != 1 ||
        first_segment.data_len < sizeof(*app_desc) ||
        app_desc->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !fixed_field_to_string(app_desc->project_name,
                               sizeof(app_desc->project_name), project,
                               sizeof(project)) ||
        !fixed_field_to_string(app_desc->version,
                               sizeof(app_desc->version), version,
                               sizeof(version)) ||
        strcmp(project, verified->manifest.project_name) != 0 ||
        strcmp(version, verified->manifest.version) != 0) {
        return fail(reason, reason_size, ESP_ERR_IMAGE_INVALID,
                    "candidate header/chip/app identity does not match manifest");
    }
    return ESP_OK;
}

esp_err_t updater_validate_not_downgrade(
    const esp_partition_t *ota0,
    const hw1_ota_verified_manifest_t *verified,
    bool allow_downgrade,
    char *reason, size_t reason_size)
{
    bool valid = false;
    bool semver_valid = false;
    int comparison;
    esp_ota_img_states_t state;
    esp_app_desc_t existing;
    char existing_version[sizeof(existing.version) + 1];
    if (allow_downgrade) {
        return ESP_OK;
    }
    if (updater_validate_existing_ota0(ota0, &valid, &state) != ESP_OK ||
        !valid || esp_ota_get_partition_description(ota0, &existing) != ESP_OK ||
        !fixed_field_to_string(existing.version, sizeof(existing.version),
                               existing_version, sizeof(existing_version))) {
        /* Recovery of an absent/corrupt application cannot be a downgrade. */
        return ESP_OK;
    }
    comparison = hw1_ota_semver_compare(verified->manifest.version,
                                        existing_version, &semver_valid);
    if (!semver_valid || comparison < 0) {
        return fail(reason, reason_size, ESP_ERR_INVALID_VERSION,
                    "candidate is older than installed main image");
    }
    return ESP_OK;
}

static esp_err_t read_and_hash_file(FILE *image, uint32_t image_size,
                                    uint8_t digest[HW1_OTA_SHA256_SIZE],
                                    char *reason, size_t reason_size)
{
    mbedtls_sha256_context context;
    uint8_t *buffer = NULL;
    uint32_t offset = 0;
    int crypto_err;
    esp_err_t err = ESP_OK;
    buffer = malloc(HW1_HASH_CHUNK);
    if (buffer == NULL) {
        return fail(reason, reason_size, ESP_ERR_NO_MEM,
                    "image hash buffer allocation failed");
    }
    mbedtls_sha256_init(&context);
    crypto_err = mbedtls_sha256_starts(&context, 0);
    rewind(image);
    while (crypto_err == 0 && offset < image_size) {
        size_t wanted = image_size - offset;
        if (wanted > HW1_HASH_CHUNK) {
            wanted = HW1_HASH_CHUNK;
        }
        if (fread(buffer, 1, wanted, image) != wanted) {
            err = fail(reason, reason_size, ESP_FAIL,
                       "staged image short read at %" PRIu32, offset);
            break;
        }
        crypto_err = mbedtls_sha256_update(&context, buffer, wanted);
        offset += (uint32_t)wanted;
        feed_current_task_watchdog();
    }
    if (err == ESP_OK && crypto_err == 0) {
        crypto_err = mbedtls_sha256_finish(&context, digest);
    }
    mbedtls_sha256_free(&context);
    memset(buffer, 0, HW1_HASH_CHUNK);
    free(buffer);
    rewind(image);
    if (err != ESP_OK) {
        return err;
    }
    if (crypto_err != 0) {
        return fail(reason, reason_size, ESP_FAIL,
                    "staged image SHA-256 failed");
    }
    return ESP_OK;
}

esp_err_t updater_open_staged_candidate(
    const esp_partition_t *ota0,
    bool allow_downgrade,
    updater_candidate_t *candidate,
    char *reason, size_t reason_size)
{
    struct stat info;
    FILE *manifest_file = NULL;
    uint8_t *json = NULL;
    uint8_t prefix[HW1_UPDATER_IMAGE_PREFIX_SIZE];
    size_t manifest_size;
    esp_err_t err;
    if (ota0 == NULL || candidate == NULL) {
        return fail(reason, reason_size, ESP_ERR_INVALID_ARG,
                    "staged preflight arguments invalid");
    }
    memset(candidate, 0, sizeof(*candidate));
    if (stat(HW1_UPDATER_STAGED_MANIFEST_PATH, &info) != 0 ||
        info.st_size <= 0 || info.st_size > HW1_MANIFEST_ENVELOPE_MAX) {
        return fail(reason, reason_size, ESP_ERR_NOT_FOUND,
                    "staged signed manifest is unavailable");
    }
    manifest_size = (size_t)info.st_size;
    json = malloc(manifest_size);
    manifest_file = fopen(HW1_UPDATER_STAGED_MANIFEST_PATH, "rb");
    if (json == NULL || manifest_file == NULL ||
        fread(json, 1, manifest_size, manifest_file) != manifest_size) {
        err = fail(reason, reason_size, ESP_FAIL,
                   "could not read staged signed manifest: errno=%d", errno);
        goto done;
    }
    err = updater_verify_manifest_envelope(json, manifest_size,
                                           &candidate->verified,
                                           reason, reason_size);
    if (err != ESP_OK) {
        goto done;
    }
    if (stat(HW1_UPDATER_STAGED_IMAGE_PATH, &info) != 0 ||
        info.st_size <= 0 || info.st_size > UINT32_MAX ||
        (uint32_t)info.st_size != candidate->verified.manifest.image_size ||
        (uint32_t)info.st_size > ota0->size) {
        err = fail(reason, reason_size, ESP_ERR_INVALID_SIZE,
                   "staged image size does not match signed manifest");
        goto done;
    }
    candidate->image_size = (uint32_t)info.st_size;
    candidate->image = fopen(HW1_UPDATER_STAGED_IMAGE_PATH, "rb");
    if (candidate->image == NULL ||
        fread(prefix, 1, sizeof(prefix), candidate->image) != sizeof(prefix)) {
        err = fail(reason, reason_size, ESP_FAIL,
                   "could not read staged candidate header: errno=%d", errno);
        goto done;
    }
    err = updater_validate_image_prefix(prefix, sizeof(prefix),
                                        &candidate->verified,
                                        &candidate->app_desc,
                                        reason, reason_size);
    if (err == ESP_OK) {
        err = read_and_hash_file(candidate->image, candidate->image_size,
                                 candidate->image_sha256,
                                 reason, reason_size);
    }
    if (err == ESP_OK) {
        err = updater_validate_manifest_contract(
            &candidate->verified, candidate->image_size,
            candidate->image_sha256, reason, reason_size);
    }
    if (err == ESP_OK) {
        err = updater_validate_not_downgrade(
            ota0, &candidate->verified, allow_downgrade,
            reason, reason_size);
    }

done:
    if (manifest_file != NULL) {
        fclose(manifest_file);
    }
    if (json != NULL) {
        memset(json, 0, manifest_size);
        free(json);
    }
    if (err != ESP_OK) {
        updater_close_candidate(candidate);
    }
    return err;
}

void updater_close_candidate(updater_candidate_t *candidate)
{
    if (candidate != NULL && candidate->image != NULL) {
        fclose(candidate->image);
        candidate->image = NULL;
    }
}

esp_err_t updater_verify_written_candidate(
    const esp_partition_t *ota0,
    const hw1_ota_verified_manifest_t *verified,
    char *reason, size_t reason_size)
{
    uint32_t mismatches = 0;
    esp_app_desc_t app_desc;
    char project[sizeof(app_desc.project_name) + 1];
    char version[sizeof(app_desc.version) + 1];
    esp_err_t err = hw1_ota_idf_verify_partition(
        ota0, verified, target_policy(), &mismatches);
    if (err != ESP_OK) {
        return fail(reason, reason_size, err,
                    "written image verification failed (%s, mask=0x%08" PRIx32 ")",
                    esp_err_to_name(err), mismatches);
    }
    if (esp_ota_get_partition_description(ota0, &app_desc) != ESP_OK ||
        !fixed_field_to_string(app_desc.project_name,
                               sizeof(app_desc.project_name), project,
                               sizeof(project)) ||
        !fixed_field_to_string(app_desc.version,
                               sizeof(app_desc.version), version,
                               sizeof(version)) ||
        strcmp(project, verified->manifest.project_name) != 0 ||
        strcmp(version, verified->manifest.version) != 0) {
        return fail(reason, reason_size, ESP_ERR_INVALID_VERSION,
                    "written app descriptor does not match signed manifest");
    }
    return ESP_OK;
}
