#include "hw1_ota_idf.h"

#include <string.h>

#include "esp_image_format.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#define HW1_OTA_HASH_CHUNK_SIZE 1024u

/* PSA's generic PSS verifier may accept any salt length; v1 requires exactly 32. */
#if defined(MBEDTLS_USE_PSA_CRYPTO)
#error "hw1_ota_protocol requires fixed-salt legacy mbedTLS RSA-PSS verification"
#endif

hw1_ota_status_t hw1_ota_idf_rsa3072_pss_sha256_verify(
    void *context,
    const hw1_ota_signature_spec_t *spec,
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *signature,
    size_t signature_size)
{
    const hw1_ota_idf_rsa_public_key_t *key =
        (const hw1_ota_idf_rsa_public_key_t *)context;
    const mbedtls_pk_rsassa_pss_options options = {
        .mgf1_hash_id = MBEDTLS_MD_SHA256,
        .expected_salt_len = HW1_OTA_PSS_SALT_LENGTH,
    };
    mbedtls_pk_context public_key;
    uint8_t digest[HW1_OTA_SHA256_SIZE];
    int crypto_err;

    if (key == NULL || spec == NULL || payload == NULL || signature == NULL ||
        key->public_key_pem == NULL || key->public_key_pem_size < 2 ||
        key->public_key_pem[key->public_key_pem_size - 1] != '\0' ||
        spec->algorithm != HW1_OTA_SIGNATURE_RSA_PSS_SHA256 ||
        spec->rsa_key_bits != 3072 || spec->digest_size != HW1_OTA_SHA256_SIZE ||
        spec->pss_salt_length != HW1_OTA_PSS_SALT_LENGTH ||
        payload_size != HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE ||
        signature_size != HW1_OTA_RSA3072_SIGNATURE_SIZE) {
        return HW1_OTA_ERR_INVALID_ARG;
    }

    mbedtls_pk_init(&public_key);
    crypto_err = mbedtls_pk_parse_public_key(&public_key, key->public_key_pem,
                                             key->public_key_pem_size);
    if (crypto_err == 0 &&
        (!mbedtls_pk_can_do(&public_key, MBEDTLS_PK_RSA) ||
         mbedtls_pk_get_bitlen(&public_key) != 3072u)) {
        crypto_err = MBEDTLS_ERR_PK_TYPE_MISMATCH;
    }
    if (crypto_err == 0) {
        crypto_err = mbedtls_sha256(payload, payload_size, digest, 0);
    }
    if (crypto_err == 0) {
        crypto_err = mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, &options,
                                           &public_key, MBEDTLS_MD_SHA256,
                                           digest, sizeof(digest), signature,
                                           signature_size);
    }
    mbedtls_pk_free(&public_key);
    memset(digest, 0, sizeof(digest));
    return crypto_err == 0 ? HW1_OTA_OK : HW1_OTA_ERR_SIGNATURE_INVALID;
}

esp_err_t hw1_ota_idf_sha256_bytes(
    const void *data,
    size_t size,
    uint8_t digest[HW1_OTA_SHA256_SIZE])
{
    if ((data == NULL && size != 0) || digest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return mbedtls_sha256((const unsigned char *)data, size, digest, 0) == 0
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t hw1_ota_idf_sha256_partition(
    const esp_partition_t *partition,
    size_t image_size,
    uint8_t digest[HW1_OTA_SHA256_SIZE])
{
    mbedtls_sha256_context context;
    uint8_t buffer[HW1_OTA_HASH_CHUNK_SIZE];
    size_t offset = 0;
    esp_err_t err = ESP_OK;
    int crypto_err;

    if (partition == NULL || digest == NULL || image_size == 0 ||
        image_size > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_sha256_init(&context);
    crypto_err = mbedtls_sha256_starts(&context, 0);
    while (crypto_err == 0 && offset < image_size) {
        const size_t remaining = image_size - offset;
        const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        err = esp_partition_read(partition, offset, buffer, chunk);
        if (err != ESP_OK) {
            break;
        }
        crypto_err = mbedtls_sha256_update(&context, buffer, chunk);
        offset += chunk;
    }
    if (err == ESP_OK && crypto_err == 0) {
        crypto_err = mbedtls_sha256_finish(&context, digest);
    }
    mbedtls_sha256_free(&context);
    memset(buffer, 0, sizeof(buffer));
    if (err != ESP_OK) {
        return err;
    }
    return crypto_err == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t hw1_ota_idf_verify_partition(
    const esp_partition_t *partition,
    const hw1_ota_verified_manifest_t *verified_manifest,
    const hw1_ota_target_policy_t *policy,
    uint32_t *manifest_mismatches)
{
    esp_partition_pos_t position;
    esp_image_metadata_t image_metadata;
    uint8_t image_sha256[HW1_OTA_SHA256_SIZE];
    hw1_ota_status_t protocol_status;
    esp_err_t err;

    if (partition == NULL || verified_manifest == NULL || policy == NULL ||
        manifest_mismatches == NULL || partition->type != ESP_PARTITION_TYPE_APP) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest_mismatches = 0;
    memset(&image_metadata, 0, sizeof(image_metadata));
    position.offset = partition->address;
    position.size = partition->size;
    image_metadata.start_addr = partition->address;
    err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &image_metadata);
    if (err != ESP_OK) {
        return err;
    }
    if (image_metadata.image_len == 0 || image_metadata.image_len > partition->size) {
        return ESP_ERR_IMAGE_INVALID;
    }
    err = hw1_ota_idf_sha256_partition(partition, image_metadata.image_len, image_sha256);
    if (err != ESP_OK) {
        return err;
    }
    protocol_status = hw1_ota_manifest_validate(verified_manifest, policy,
                                                image_metadata.image_len,
                                                image_sha256,
                                                manifest_mismatches);
    memset(image_sha256, 0, sizeof(image_sha256));
    if (protocol_status == HW1_OTA_OK) {
        return ESP_OK;
    }
    return protocol_status == HW1_OTA_ERR_INCOMPATIBLE ? ESP_ERR_INVALID_VERSION
                                                       : ESP_ERR_INVALID_ARG;
}
