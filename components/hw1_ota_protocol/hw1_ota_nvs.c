#include "hw1_ota_nvs.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

typedef struct {
    bool exists;
    bool valid;
    uint8_t wire[HW1_OTA_RECORD_WIRE_SIZE];
    hw1_ota_record_t record;
} slot_value_t;

static StaticSemaphore_t s_store_mutex_storage;
static SemaphoreHandle_t s_store_mutex;
static portMUX_TYPE s_store_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t store_lock(void)
{
    if (s_store_mutex == NULL) {
        taskENTER_CRITICAL(&s_store_mutex_init_lock);
        if (s_store_mutex == NULL) {
            s_store_mutex = xSemaphoreCreateMutexStatic(&s_store_mutex_storage);
        }
        taskEXIT_CRITICAL(&s_store_mutex_init_lock);
    }
    if (s_store_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return xSemaphoreTake(s_store_mutex, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}

static void store_unlock(void)
{
    if (s_store_mutex != NULL) {
        (void)xSemaphoreGive(s_store_mutex);
    }
}

static esp_err_t open_store(const char *partition_name, nvs_open_mode_t mode,
                            nvs_handle_t *handle)
{
    if (partition_name == NULL || partition_name[0] == '\0') {
        return nvs_open(HW1_OTA_NVS_NAMESPACE, mode, handle);
    }
    return nvs_open_from_partition(partition_name, HW1_OTA_NVS_NAMESPACE, mode, handle);
}

static esp_err_t read_slot(nvs_handle_t handle, const char *key, slot_value_t *slot)
{
    size_t size = 0;
    esp_err_t err;

    memset(slot, 0, sizeof(*slot));
    err = nvs_get_blob(handle, key, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        slot->exists = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    slot->exists = true;
    if (size != HW1_OTA_RECORD_WIRE_SIZE) {
        return ESP_OK;
    }
    err = nvs_get_blob(handle, key, slot->wire, &size);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH || err == ESP_ERR_NVS_INVALID_LENGTH) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    slot->valid = hw1_ota_record_decode(slot->wire, size, &slot->record) == HW1_OTA_OK;
    return ESP_OK;
}

static esp_err_t select_newest(const slot_value_t slots[2], hw1_ota_record_t *record,
                               hw1_ota_nvs_info_t *info)
{
    uint8_t selected;

    memset(info, 0, sizeof(*info));
    if (slots[0].valid) {
        info->valid_slots |= 1u;
    } else if (slots[0].exists) {
        info->corrupt_slots |= 1u;
    }
    if (slots[1].valid) {
        info->valid_slots |= 2u;
    } else if (slots[1].exists) {
        info->corrupt_slots |= 2u;
    }
    if (!slots[0].valid && !slots[1].valid) {
        return (slots[0].exists || slots[1].exists) ? ESP_ERR_INVALID_CRC
                                                    : ESP_ERR_NVS_NOT_FOUND;
    }
    if (slots[0].valid && slots[1].valid) {
        if (slots[0].record.sequence == slots[1].record.sequence) {
            if (memcmp(slots[0].wire, slots[1].wire, HW1_OTA_RECORD_WIRE_SIZE) != 0) {
                return ESP_ERR_INVALID_STATE;
            }
            selected = 0;
        } else {
            selected = hw1_ota_sequence_is_newer(slots[1].record.sequence,
                                                 slots[0].record.sequence) ? 1u : 0u;
        }
    } else {
        selected = slots[1].valid ? 1u : 0u;
    }
    info->selected_slot = selected;
    info->sequence = slots[selected].record.sequence;
    if (record != NULL) {
        *record = slots[selected].record;
    }
    return ESP_OK;
}

static esp_err_t load_handle(nvs_handle_t handle, hw1_ota_record_t *record,
                             hw1_ota_nvs_info_t *info, slot_value_t slots[2])
{
    esp_err_t err = read_slot(handle, HW1_OTA_NVS_SLOT_A_KEY, &slots[0]);
    if (err != ESP_OK) {
        return err;
    }
    err = read_slot(handle, HW1_OTA_NVS_SLOT_B_KEY, &slots[1]);
    if (err != ESP_OK) {
        return err;
    }
    return select_newest(slots, record, info);
}

esp_err_t hw1_ota_nvs_load_from_partition(
    const char *partition_name,
    hw1_ota_record_t *record,
    hw1_ota_nvs_info_t *info)
{
    hw1_ota_nvs_info_t local_info;
    slot_value_t slots[2];
    nvs_handle_t handle;
    esp_err_t err;

    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (info == NULL) {
        info = &local_info;
    }
    err = store_lock();
    if (err != ESP_OK) {
        return err;
    }
    err = open_store(partition_name, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = load_handle(handle, record, info, slots);
        nvs_close(handle);
    } else {
        memset(info, 0, sizeof(*info));
    }
    store_unlock();
    return err;
}

esp_err_t hw1_ota_nvs_load(hw1_ota_record_t *record, hw1_ota_nvs_info_t *info)
{
    return hw1_ota_nvs_load_from_partition(NULL, record, info);
}

esp_err_t hw1_ota_nvs_commit_to_partition(
    const char *partition_name,
    const hw1_ota_record_t *desired,
    uint32_t expected_sequence,
    hw1_ota_record_t *stored,
    hw1_ota_nvs_info_t *info)
{
    hw1_ota_nvs_info_t local_info;
    hw1_ota_record_t current;
    hw1_ota_record_t next;
    slot_value_t slots[2];
    uint8_t wire[HW1_OTA_RECORD_WIRE_SIZE];
    uint8_t verify_wire[HW1_OTA_RECORD_WIRE_SIZE];
    hw1_ota_record_t verify_record;
    const char *target_key;
    size_t verify_size = sizeof(verify_wire);
    uint32_t current_sequence = 0;
    uint8_t target_slot = 0;
    nvs_handle_t handle;
    esp_err_t err;

    if (desired == NULL || hw1_ota_record_validate(desired) != HW1_OTA_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (info == NULL) {
        info = &local_info;
    }
    err = store_lock();
    if (err != ESP_OK) {
        return err;
    }
    err = open_store(partition_name, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        store_unlock();
        return err;
    }

    err = load_handle(handle, &current, info, slots);
    if (err == ESP_OK) {
        current_sequence = current.sequence;
        target_slot = (uint8_t)(info->selected_slot ^ 1u);
    } else if (err == ESP_ERR_NVS_NOT_FOUND ||
               (err == ESP_ERR_INVALID_CRC && expected_sequence == HW1_OTA_SEQUENCE_ANY)) {
        current_sequence = 0;
        target_slot = (info->corrupt_slots & 1u) != 0 ? 0u :
                      (info->corrupt_slots & 2u) != 0 ? 1u : 0u;
        err = ESP_OK;
    }
    if (err == ESP_OK && expected_sequence != HW1_OTA_SEQUENCE_ANY &&
        expected_sequence != current_sequence) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        store_unlock();
        return err;
    }

    next = *desired;
    next.sequence = current_sequence + 1u;
    if (next.sequence == 0) {
        next.sequence = 1;
    }
    if (hw1_ota_record_encode(&next, wire) != HW1_OTA_OK) {
        nvs_close(handle);
        store_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    target_key = target_slot == 0 ? HW1_OTA_NVS_SLOT_A_KEY : HW1_OTA_NVS_SLOT_B_KEY;
    err = nvs_set_blob(handle, target_key, wire, sizeof(wire));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, target_key, verify_wire, &verify_size);
    }
    if (err == ESP_OK &&
        (verify_size != sizeof(verify_wire) || memcmp(wire, verify_wire, sizeof(wire)) != 0 ||
         hw1_ota_record_decode(verify_wire, verify_size, &verify_record) != HW1_OTA_OK ||
         verify_record.sequence != next.sequence)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) {
        info->valid_slots |= (uint8_t)(1u << target_slot);
        info->corrupt_slots &= (uint8_t)~(1u << target_slot);
        info->selected_slot = target_slot;
        info->sequence = next.sequence;
        if (stored != NULL) {
            *stored = next;
        }
    }
    nvs_close(handle);
    store_unlock();
    return err;
}

esp_err_t hw1_ota_nvs_commit(
    const hw1_ota_record_t *desired,
    uint32_t expected_sequence,
    hw1_ota_record_t *stored,
    hw1_ota_nvs_info_t *info)
{
    return hw1_ota_nvs_commit_to_partition(NULL, desired, expected_sequence, stored, info);
}
