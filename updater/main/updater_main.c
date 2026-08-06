#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hw1_ota_nvs.h"
#include "hw1_ota_protocol.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "recovery_network.h"
#include "recovery_auth_throttle.h"
#include "sdkconfig.h"
#include "updater_power.h"
#include "updater_preflight.h"

#define CONTROL_QUEUE_DEPTH 8u
#define CONTROL_TASK_STACK 10240u
#define CONSOLE_TASK_STACK 4096u
#define OTA_WRITE_CHUNK 4096u
#define STATUS_DETAIL_SIZE 192u

static const char *TAG = "hw1-updater";

typedef enum {
    UPDATER_STATE_BOOT = 0,
    UPDATER_STATE_IDLE,
    UPDATER_STATE_PREFLIGHT,
    UPDATER_STATE_WRITING,
    UPDATER_STATE_VERIFYING,
    UPDATER_STATE_ERROR,
    UPDATER_STATE_HELD,
    UPDATER_STATE_REBOOTING,
} updater_state_t;

typedef enum {
    CONTROL_APPLY_STAGED = 1,
    CONTROL_CANCEL,
    CONTROL_REBOOT,
    CONTROL_START_NETWORK,
    CONTROL_ALLOW_DOWNGRADE,
    CONTROL_RESET_JOURNAL,
} control_command_type_t;

typedef struct {
    control_command_type_t type;
    bool from_serial;
} control_command_t;

typedef struct {
    updater_state_t state;
    bool layout_valid;
    bool littlefs_mounted;
    bool main_valid;
    bool network_started;
    bool nvs_ready;
    bool journal_valid;
    esp_ota_img_states_t main_state;
    hw1_ota_phase_t phase;
    hw1_ota_source_t source;
    uint32_t journal_sequence;
    size_t bytes_written;
    size_t image_size;
    char detail[STATUS_DETAIL_SIZE];
    char candidate_version[HW1_OTA_VERSION_SIZE];
} updater_snapshot_t;

static QueueHandle_t s_control_queue;
static SemaphoreHandle_t s_snapshot_mutex;
static SemaphoreHandle_t s_operation_mutex;
static updater_snapshot_t s_snapshot;
static portMUX_TYPE s_activity_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_activity_us;
static bool s_littlefs_mounted;
static bool s_nvs_ready;
static const esp_partition_t *s_ota0;
static hw1_ota_verified_manifest_t s_upload_manifest;
static uint64_t s_upload_operation_id;
static bool s_upload_manifest_valid;
static volatile bool s_control_watchdog_active;

static void set_last_activity_now(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_activity_mux);
    s_last_activity_us = now;
    portEXIT_CRITICAL(&s_activity_mux);
}

static int64_t last_activity_us(void)
{
    int64_t value;
    portENTER_CRITICAL(&s_activity_mux);
    value = s_last_activity_us;
    portEXIT_CRITICAL(&s_activity_mux);
    return value;
}

static const char *state_name(updater_state_t state)
{
    switch (state) {
    case UPDATER_STATE_IDLE: return "idle";
    case UPDATER_STATE_PREFLIGHT: return "preflight";
    case UPDATER_STATE_WRITING: return "writing";
    case UPDATER_STATE_VERIFYING: return "verifying";
    case UPDATER_STATE_ERROR: return "error";
    case UPDATER_STATE_HELD: return "held";
    case UPDATER_STATE_REBOOTING: return "rebooting";
    default: return "boot";
    }
}

static const char *phase_name(hw1_ota_phase_t phase)
{
    switch (phase) {
    case HW1_OTA_PHASE_IDLE: return "idle";
    case HW1_OTA_PHASE_REQUESTED: return "requested";
    case HW1_OTA_PHASE_RECOVERY_BOOT_ARMED: return "recovery_boot_armed";
    case HW1_OTA_PHASE_RECOVERY_RUNNING: return "recovery_running";
    case HW1_OTA_PHASE_APPLYING: return "applying";
    case HW1_OTA_PHASE_IMAGE_VERIFIED: return "image_verified";
    case HW1_OTA_PHASE_TRIAL_BOOT_ARMED: return "trial_boot_armed";
    case HW1_OTA_PHASE_TRIAL_RUNNING: return "trial_running";
    case HW1_OTA_PHASE_SUCCEEDED: return "succeeded";
    case HW1_OTA_PHASE_FAILED: return "failed";
    case HW1_OTA_PHASE_CANCELED: return "canceled";
    default: return "unknown";
    }
}

static const char *source_name(hw1_ota_source_t source)
{
    switch (source) {
    case HW1_OTA_SOURCE_STAGED_FILE: return "staged_file";
    case HW1_OTA_SOURCE_RECOVERY_UPLOAD: return "recovery_upload";
    case HW1_OTA_SOURCE_SERIAL: return "serial";
    default: return "none";
    }
}

static const char *ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

static void snapshot_set(updater_state_t state, const char *detail)
{
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot.state = state;
        if (detail != NULL) {
            snprintf(s_snapshot.detail, sizeof(s_snapshot.detail), "%s", detail);
        }
        xSemaphoreGive(s_snapshot_mutex);
    }
}

static void snapshot_progress(size_t written, size_t total)
{
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot.bytes_written = written;
        s_snapshot.image_size = total;
        xSemaphoreGive(s_snapshot_mutex);
    }
}

/* Console narration for the two multi-minute operations.
 *
 * Writing a ~5 MB image takes 60-120 seconds over the recovery SoftAP, and the
 * updater used to print nothing at all for the whole of it. From the operator's
 * seat that is a dead `hw1up>` prompt and an HTTP request that appears hung,
 * and the natural responses - reset the board, unplug it, type `cancel` - are
 * precisely the ones that abandon a half-written ota_0. Progress was only ever
 * visible to someone who already knew to type `status`.
 *
 * So: say how long it will take BEFORE starting, then prove liveness while it
 * runs. Ten decile lines is enough to look alive without flooding a 115200-baud
 * console or stealing time from the transfer.
 *
 * Two different tasks reach these: the direct-upload path runs on the
 * esp_http_server task, the staged-apply path on hw1up_control - which is the
 * only TWDT-subscribed task, so its tick sits inside the fed window between
 * esp_ota_write() and feed_control_watchdog(). The two are mutually exclusive
 * under s_operation_mutex, so the unlocked statics below are not a race.
 *
 * Console writes cannot stall a transfer. stdout is the USB-Serial/JTAG driver,
 * whose VFS tries a 0-tick write first and, on failure, blocks ONCE for at most
 * TX_FLUSH_TIMEOUT_US (50 ms) before latching into drop-the-byte mode - so an
 * unattended board with nobody draining the port costs ~50 ms, not a hang. The
 * console reads with plain fgets() and holds only the driver's read lock, so
 * interleaved output cannot corrupt any line-editor state or deadlock. */
static uint8_t s_progress_decile;
static bool s_progress_active;

static void progress_console_begin(const char *what, size_t total)
{
    s_progress_decile = 0;
    s_progress_active = true;
    printf("\n[recovery] %s: %u bytes. This normally takes 1-3 minutes.\n"
           "[recovery] Do NOT reset, unplug, or type 'cancel' until it reports done.\n",
           what, (unsigned)total);
    fflush(stdout);
}

static void progress_console_tick(size_t written, size_t total)
{
    uint8_t decile;
    if (total == 0) {
        return;
    }
    /* 64-bit intermediate: 5 MB * 10 overflows nothing here, but written is
     * size_t and this keeps the arithmetic obviously safe if the slot grows. */
    decile = (uint8_t)(((uint64_t)written * 10U) / total);
    if (decile > 10U) {
        decile = 10U;
    }
    if (decile <= s_progress_decile) {
        return;
    }
    s_progress_decile = decile;
    printf("[recovery] %3u%%  (%u / %u bytes)\n", (unsigned)decile * 10U,
           (unsigned)written, (unsigned)total);
    fflush(stdout);
}

static void progress_console_end(bool ok, const char *detail)
{
    /* Symmetric with begin(): a refusal that happens before the banner was
     * printed must not emit a lone "done"/"FAILED" line out of nowhere. */
    if (!s_progress_active) {
        return;
    }
    s_progress_active = false;
    printf("[recovery] %s%s%s\n", ok ? "done" : "FAILED",
           (detail != NULL && detail[0] != '\0') ? ": " : "",
           (detail != NULL) ? detail : "");
    fflush(stdout);
}

static void snapshot_record(const hw1_ota_record_t *record, bool valid)
{
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot.journal_valid = valid;
        if (valid && record != NULL) {
            s_snapshot.phase = record->phase;
            s_snapshot.source = record->source;
            s_snapshot.journal_sequence = record->sequence;
            if (record->candidate_present) {
                snprintf(s_snapshot.candidate_version,
                         sizeof(s_snapshot.candidate_version), "%s",
                         record->candidate.manifest.version);
            }
        }
        xSemaphoreGive(s_snapshot_mutex);
    }
}

static updater_snapshot_t snapshot_copy(void)
{
    updater_snapshot_t copy = {0};
    /* Bounded, never portMAX_DELAY: this runs on the single esp_http_server
     * task that also serves /cancel, so an unbounded wait here would take the
     * whole recovery UI down with it.  On timeout report an honest degraded
     * payload (state stays UPDATER_STATE_BOOT, the neutral "unknown") rather
     * than blocking or inventing a status. */
    if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        copy = s_snapshot;
        xSemaphoreGive(s_snapshot_mutex);
    } else {
        snprintf(copy.detail, sizeof(copy.detail),
                 "status momentarily unavailable; retry");
    }
    return copy;
}

static bool validated_layout_ready(void)
{
    bool layout_valid = false;
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        layout_valid = s_snapshot.layout_valid;
        xSemaphoreGive(s_snapshot_mutex);
    }
    return layout_valid && s_nvs_ready && s_ota0 != NULL;
}

static size_t status_json(char *buffer, size_t buffer_size, void *context)
{
    updater_snapshot_t status = snapshot_copy();
    cJSON *root;
    bool printed;
    (void)context;
    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    root = cJSON_CreateObject();
    if (root == NULL) {
        return 0;
    }
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "project", "hw1-updater");
    cJSON_AddStringToObject(root, "version", HW1_OTA_UPDATER_VERSION);
    cJSON_AddStringToObject(root, "board", HW1_OTA_BOARD_ID);
    cJSON_AddStringToObject(root, "layout", HW1_OTA_LAYOUT_ID);
    cJSON_AddStringToObject(root, "state", state_name(status.state));
    cJSON_AddStringToObject(root, "detail", status.detail);
    cJSON_AddBoolToObject(root, "layoutValid", status.layout_valid);
    cJSON_AddBoolToObject(root, "littlefsReadOnly", status.littlefs_mounted);
    cJSON_AddBoolToObject(root, "nvsReady", status.nvs_ready);
    cJSON_AddBoolToObject(root, "journalValid", status.journal_valid);
    cJSON_AddStringToObject(root, "phase", phase_name(status.phase));
    cJSON_AddStringToObject(root, "source", source_name(status.source));
    cJSON_AddNumberToObject(root, "journalSequence", status.journal_sequence);
    cJSON_AddBoolToObject(root, "mainValid", status.main_valid);
    cJSON_AddStringToObject(root, "mainState", ota_state_name(status.main_state));
    cJSON_AddBoolToObject(root, "networkStarted", status.network_started);
    cJSON_AddNumberToObject(root, "bytesWritten", (double)status.bytes_written);
    cJSON_AddNumberToObject(root, "imageSize", (double)status.image_size);
    cJSON_AddStringToObject(root, "candidateVersion", status.candidate_version);
    {
        /* Visible so an operator can tell "the AP is being guessed at" apart
         * from "the AP is misbehaving". */
        uint32_t blocked_peers = 0;
        uint32_t auth_failures = 0;
        recovery_network_auth_stats(&blocked_peers, &auth_failures);
        cJSON_AddNumberToObject(root, "authBlockedPeers", (double)blocked_peers);
        cJSON_AddNumberToObject(root, "authFailuresTotal", (double)auth_failures);
    }
    printed = cJSON_PrintPreallocated(root, buffer, (int)buffer_size, false);
    cJSON_Delete(root);
    if (!printed) {
        buffer[0] = '\0';
        return 0;
    }
    return strlen(buffer);
}

static void note_activity(void *context)
{
    (void)context;
    set_last_activity_now();
}

static bool staged_pair_available(void)
{
    struct stat image;
    struct stat manifest;
    return s_littlefs_mounted &&
           stat(HW1_UPDATER_STAGED_IMAGE_PATH, &image) == 0 &&
           image.st_size > 0 &&
           stat(HW1_UPDATER_STAGED_MANIFEST_PATH, &manifest) == 0 &&
           manifest.st_size > 0;
}

static esp_err_t load_record(hw1_ota_record_t *record, bool allow_empty)
{
    hw1_ota_nvs_info_t info;
    esp_err_t err;
    if (!s_nvs_ready || record == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(record, 0, sizeof(*record));
    memset(&info, 0, sizeof(info));
    err = hw1_ota_nvs_load(record, &info);
    if (err == ESP_ERR_NVS_NOT_FOUND && allow_empty) {
        hw1_ota_record_init(record);
        snapshot_record(record, true);
        return ESP_OK;
    }
    snapshot_record(record, err == ESP_OK);
    return err;
}

static esp_err_t commit_record(hw1_ota_record_t *record)
{
    hw1_ota_record_t stored;
    hw1_ota_nvs_info_t info;
    esp_err_t err;
    if (!s_nvs_ready || record == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    err = hw1_ota_nvs_commit(record, record->sequence, &stored, &info);
    if (err == ESP_OK) {
        *record = stored;
        snapshot_record(record, true);
    }
    return err;
}

static esp_err_t transition_record(hw1_ota_record_t *record,
                                   hw1_ota_event_t event,
                                   const hw1_ota_transition_args_t *args)
{
    hw1_ota_status_t status = hw1_ota_transition(record, event, args);
    if (status != HW1_OTA_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return commit_record(record);
}

static uint64_t new_operation_id(void)
{
    uint64_t value = ((uint64_t)esp_random() << 32) | esp_random();
    return value == 0 ? 1 : value;
}

static void record_failure(hw1_ota_record_t *record,
                           hw1_ota_result_code_t code,
                           esp_err_t native_error, const char *detail)
{
    hw1_ota_transition_args_t args = {
        .verified_manifest = NULL,
        .result_code = code,
        .native_error = (int32_t)native_error,
        .detail = detail,
    };
    if (record != NULL && record->phase != HW1_OTA_PHASE_IDLE &&
        record->phase != HW1_OTA_PHASE_SUCCEEDED &&
        record->phase != HW1_OTA_PHASE_FAILED &&
        record->phase != HW1_OTA_PHASE_CANCELED) {
        esp_err_t journal_err = transition_record(record, HW1_OTA_EVENT_FAIL,
                                                  &args);
        if (journal_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not journal failure: %s",
                     esp_err_to_name(journal_err));
        }
    }
    snapshot_set(UPDATER_STATE_ERROR, detail);
    ESP_LOGE(TAG, "%s", detail);
}

static esp_err_t enqueue_control(control_command_type_t type, bool from_serial)
{
    control_command_t command = {.type = type, .from_serial = from_serial};
    if (xQueueSend(s_control_queue, &command, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    note_activity(NULL);
    return ESP_OK;
}

static esp_err_t mount_littlefs_read_only(void)
{
    const esp_vfs_littlefs_conf_t config = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .partition = NULL,
        .format_if_mount_failed = false,
        .read_only = true,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    esp_err_t err;
    if (s_littlefs_mounted) {
        return ESP_OK;
    }
    err = esp_vfs_littlefs_register(&config);
    if (err == ESP_OK) {
        s_littlefs_mounted = true;
    }
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot.littlefs_mounted = s_littlefs_mounted;
        xSemaphoreGive(s_snapshot_mutex);
    }
    return err;
}

static esp_err_t load_credentials(recovery_credentials_t *credentials)
{
    nvs_handle_t handle;
    size_t ap_size = HW1_RECOVERY_CREDENTIAL_CAPACITY;
    size_t token_size = HW1_RECOVERY_CREDENTIAL_CAPACITY;
    esp_err_t err;
    size_t i;
    if (!s_nvs_ready || credentials == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(credentials, 0, sizeof(*credentials));
    err = nvs_open(HW1_OTA_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, "ap_pass", credentials->ap_password, &ap_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "auth_token", credentials->auth_token,
                          &token_size);
    }
    nvs_close(handle);
    if (err != ESP_OK || ap_size != token_size || ap_size < 13 || ap_size > 64 ||
        strcmp(credentials->ap_password, credentials->auth_token) != 0) {
        memset(credentials, 0, sizeof(*credentials));
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    for (i = 0; i + 1 < ap_size; ++i) {
        unsigned char c = (unsigned char)credentials->ap_password[i];
        if (c < 0x20 || c > 0x7e) {
            memset(credentials, 0, sizeof(*credentials));
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;
    while (size-- != 0) {
        *bytes++ = 0;
    }
}

static void clear_upload_authorization(void)
{
    s_upload_manifest_valid = false;
    s_upload_operation_id = 0;
    secure_zero(&s_upload_manifest, sizeof(s_upload_manifest));
}

static esp_err_t subscribe_control_watchdog(void)
{
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_OK) {
        s_control_watchdog_active = true;
        err = esp_task_wdt_reset();
        if (err != ESP_OK) {
            (void)esp_task_wdt_delete(NULL);
            s_control_watchdog_active = false;
        }
    }
    return err;
}

static esp_err_t start_control_watchdog(void)
{
    uint32_t idle_core_mask = 0;
    esp_task_wdt_config_t config;
    esp_err_t err;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    idle_core_mask |= 1u << 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    idle_core_mask |= 1u << 1;
#endif
    memset(&config, 0, sizeof(config));
    config.timeout_ms = CONFIG_HW1_UPDATER_WDT_TIMEOUT_MS;
    config.idle_core_mask = idle_core_mask;
#if CONFIG_ESP_TASK_WDT_PANIC
    config.trigger_panic = true;
#endif
    err = esp_task_wdt_reconfigure(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&config);
    }
    if (err == ESP_OK) {
        err = subscribe_control_watchdog();
    }
    return err;
}

static void feed_control_watchdog(void)
{
    if (s_control_watchdog_active && esp_task_wdt_reset() != ESP_OK) {
        s_control_watchdog_active = false;
        ESP_LOGE(TAG, "control-task watchdog feed failed");
    }
}

static esp_err_t store_recovery_pin(const char *pin)
{
    nvs_handle_t handle;
    size_t length;
    size_t i;
    esp_err_t err;
    if (!s_nvs_ready || pin == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    length = strlen(pin);
    if (length < 12 || length > 63) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)pin[i];
        if (c < 0x20 || c > 0x7e) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    err = nvs_open(HW1_OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "ap_pass", pin);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "auth_token", pin);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t start_network(void);

static bool main_partition_is_return_eligible(bool allow_rejected_ota_state)
{
    bool valid = false;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_app_desc_t desc;
    char project[sizeof(desc.project_name) + 1] = {0};
    char version[sizeof(desc.version) + 1] = {0};
    const char *project_end;
    const char *version_end;
    size_t version_length;
    size_t suffix_length = strlen(HW1_OTA_VERSION_SUFFIX);
    if (updater_validate_existing_ota0(s_ota0, &valid, &state) != ESP_OK ||
        !valid ||
        (!allow_rejected_ota_state &&
         (state == ESP_OTA_IMG_ABORTED || state == ESP_OTA_IMG_INVALID)) ||
        esp_ota_get_partition_description(s_ota0, &desc) != ESP_OK) {
        return false;
    }
    project_end = memchr(desc.project_name, '\0', sizeof(desc.project_name));
    version_end = memchr(desc.version, '\0', sizeof(desc.version));
    if (project_end == NULL || version_end == NULL) {
        return false;
    }
    memcpy(project, desc.project_name,
           (size_t)(project_end - desc.project_name));
    memcpy(version, desc.version, (size_t)(version_end - desc.version));
    version_length = strlen(version);
    return strcmp(project, "hardwareone-idf") == 0 &&
           suffix_length <= version_length &&
           memcmp(version + version_length - suffix_length,
                  HW1_OTA_VERSION_SUFFIX, suffix_length) == 0;
}

static esp_err_t canonicalize_and_select_main(bool allow_rejected_ota_state)
{
    const esp_partition_t *factory = esp_ota_get_running_partition();
    esp_err_t err;
    if (factory == NULL ||
        factory->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY ||
        s_ota0 == NULL ||
        !main_partition_is_return_eligible(allow_rejected_ota_state)) {
        return ESP_ERR_INVALID_STATE;
    }
    /* On this single-OTA-slot layout, selecting factory first canonicalizes
     * both otadata copies. A power cut between these calls safely stays in
     * recovery; only the second call creates the ota_0 NEW trial intent. */
    err = esp_ota_set_boot_partition(factory);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s_ota0);
    }
    return err;
}

static esp_err_t queue_reboot_or_restart_directly(const char *detail)
{
    esp_err_t err;
    snapshot_set(UPDATER_STATE_REBOOTING, detail);
    err = enqueue_control(CONTROL_REBOOT, false);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    /* Boot selection has already been committed. Do not leave the updater
     * running indefinitely merely because the bounded control queue filled. */
    ESP_LOGE(TAG, "Could not queue required reboot (%s); restarting directly",
             esp_err_to_name(err));
    snapshot_set(UPDATER_STATE_REBOOTING,
                 "reboot queue full; restarting directly");
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
    abort();
}

static esp_err_t set_main_boot_and_reboot(void)
{
    esp_err_t err = canonicalize_and_select_main(false);
    if (err == ESP_OK) {
        err = queue_reboot_or_restart_directly(
            "main image selected; rebooting");
    }
    return err;
}

static esp_err_t finalize_verified_image(hw1_ota_record_t *record,
                                         const hw1_ota_verified_manifest_t *fresh,
                                         char *reason, size_t reason_size)
{
    hw1_ota_transition_args_t args = {
        .verified_manifest = fresh,
        .result_code = HW1_OTA_RESULT_NONE,
        .native_error = 0,
        .detail = NULL,
    };
    esp_err_t err;
    if (record->candidate_present &&
        !hw1_ota_record_candidate_matches_verified(record, fresh)) {
        snprintf(reason, reason_size,
                 "freshly verified manifest does not match OTA journal");
        return ESP_ERR_INVALID_CRC;
    }
    if (record->phase == HW1_OTA_PHASE_APPLYING) {
        err = transition_record(record, HW1_OTA_EVENT_IMAGE_VERIFIED, &args);
        if (err != ESP_OK) {
            snprintf(reason, reason_size,
                     "could not journal verified image: %s",
                     esp_err_to_name(err));
            return err;
        }
    }
    if (record->phase == HW1_OTA_PHASE_IMAGE_VERIFIED) {
        if (!hw1_ota_record_candidate_matches_verified(record, fresh)) {
            snprintf(reason, reason_size,
                     "authenticated candidate no longer matches journal");
            return ESP_ERR_INVALID_CRC;
        }
        err = transition_record(record, HW1_OTA_EVENT_ARM_TRIAL_BOOT, NULL);
        if (err != ESP_OK) {
            snprintf(reason, reason_size,
                     "could not arm trial boot: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (record->phase != HW1_OTA_PHASE_TRIAL_BOOT_ARMED ||
        !hw1_ota_record_candidate_matches_verified(record, fresh)) {
        snprintf(reason, reason_size,
                 "journal is not ready for authenticated trial boot");
        return ESP_ERR_INVALID_STATE;
    }
    /* A fully rewritten candidate can inherit ABORTED/INVALID only from the
     * old otadata entry. The fresh manifest, digest, native signature, and
     * partition verification above authorize clearing that stale rejection.
     * Selecting factory first erases otadata; selecting ota_0 then creates a
     * new trial intent. No ordinary return/cancel path gets this exception. */
    err = canonicalize_and_select_main(true);
    if (err != ESP_OK) {
        snprintf(reason, reason_size, "boot selection failed: %s",
                 esp_err_to_name(err));
        record_failure(record, HW1_OTA_RESULT_BOOT_SWITCH_ERROR, err, reason);
        return err;
    }
    return queue_reboot_or_restart_directly(
        "signed image verified; trial boot armed");
}

static esp_err_t begin_or_resume_applying(hw1_ota_record_t *record,
                                          char *reason, size_t reason_size)
{
    esp_err_t err;
    if (record->phase == HW1_OTA_PHASE_REQUESTED ||
        record->phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED) {
        err = transition_record(record, HW1_OTA_EVENT_RECOVERY_STARTED, NULL);
        if (err != ESP_OK) {
            snprintf(reason, reason_size, "could not enter recovery state");
            return err;
        }
    }
    if (record->phase == HW1_OTA_PHASE_RECOVERY_RUNNING) {
        err = transition_record(record, HW1_OTA_EVENT_APPLY_STARTED, NULL);
        if (err != ESP_OK) {
            snprintf(reason, reason_size, "could not enter applying state");
            return err;
        }
    }
    if (record->phase != HW1_OTA_PHASE_APPLYING) {
        snprintf(reason, reason_size, "journal is not ready to apply an image");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t write_staged_candidate(updater_candidate_t *candidate,
                                        hw1_ota_record_t *record,
                                        char *reason, size_t reason_size)
{
    esp_ota_handle_t handle = 0;
    uint8_t *buffer;
    uint32_t written = 0;
    int64_t started_us = esp_timer_get_time();
    esp_err_t err;
    err = (record->request_flags & HW1_OTA_REQUEST_FORCED_POWER_OVERRIDE) != 0
              ? ESP_OK
              : updater_power_require_safe(reason, reason_size);
    if (err != ESP_OK) {
        return err;
    }
    err = begin_or_resume_applying(record, reason, reason_size);
    if (err != ESP_OK) {
        return err;
    }
    snapshot_set(UPDATER_STATE_WRITING, "writing staged signed image to ota_0");
    progress_console_begin("Writing staged firmware to ota_0",
                           candidate->image_size);
    snapshot_progress(0, candidate->image_size);
    /* Incremental erases bound each blocking flash operation so the control
     * task can keep servicing its watchdog between chunks. */
    err = esp_ota_begin(s_ota0, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        snprintf(reason, reason_size, "esp_ota_begin failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    feed_control_watchdog();
    buffer = malloc(OTA_WRITE_CHUNK);
    if (buffer == NULL) {
        esp_ota_abort(handle);
        snprintf(reason, reason_size, "OTA write buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    rewind(candidate->image);
    while (written < candidate->image_size) {
        size_t wanted = candidate->image_size - written;
        if (wanted > OTA_WRITE_CHUNK) {
            wanted = OTA_WRITE_CHUNK;
        }
        if (esp_timer_get_time() - started_us >
            (int64_t)CONFIG_HW1_UPDATER_APPLY_TIMEOUT_SEC * 1000000LL) {
            err = ESP_ERR_TIMEOUT;
            snprintf(reason, reason_size, "staged apply timed out");
            break;
        }
        if (fread(buffer, 1, wanted, candidate->image) != wanted) {
            err = ESP_FAIL;
            snprintf(reason, reason_size,
                     "staged image short read at %" PRIu32, written);
            break;
        }
        err = esp_ota_write(handle, buffer, wanted);
        if (err != ESP_OK) {
            snprintf(reason, reason_size, "esp_ota_write failed at %" PRIu32
                     ": %s", written, esp_err_to_name(err));
            break;
        }
        written += (uint32_t)wanted;
        snapshot_progress(written, candidate->image_size);
        progress_console_tick(written, candidate->image_size);
        feed_control_watchdog();
    }
    memset(buffer, 0, OTA_WRITE_CHUNK);
    free(buffer);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return err;
    }
    snapshot_set(UPDATER_STATE_VERIFYING,
                 "verifying ESP image signature and partition digest");
    feed_control_watchdog();
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        snprintf(reason, reason_size, "esp_ota_end rejected image: %s",
                 esp_err_to_name(err));
        return err;
    }
    feed_control_watchdog();
    err = updater_verify_written_candidate(s_ota0, &candidate->verified,
                                           reason, reason_size);
    feed_control_watchdog();
    return err;
}

static void apply_staged(void)
{
    updater_candidate_t candidate = {0};
    hw1_ota_record_t record = {0};
    char reason[STATUS_DETAIL_SIZE] = {0};
    bool allow_downgrade;
    bool main_valid = false;
    esp_ota_img_states_t main_state;
    esp_err_t err;
    if (!s_littlefs_mounted || s_ota0 == NULL) {
        snapshot_set(UPDATER_STATE_ERROR,
                     "read-only staged files are unavailable");
        return;
    }
    if (xSemaphoreTake(s_operation_mutex, 0) != pdTRUE) {
        /* Do NOT clobber the snapshot here.  The operation that owns the mutex
         * is still running fine; overwriting state with ERROR made /status
         * report a failure for the rest of an active flash write, and an
         * operator watching the recovery page would pull power mid-write. */
        ESP_LOGW(TAG, "apply refused: another OTA operation is active");
        return;
    }
    snapshot_set(UPDATER_STATE_PREFLIGHT,
                 "freshly verifying staged envelope and complete image");
    feed_control_watchdog();
    err = load_record(&record, false);
    if (err != ESP_OK || record.source != HW1_OTA_SOURCE_STAGED_FILE ||
        !record.candidate_present) {
        snprintf(reason, sizeof(reason),
                 "journal does not authorize the staged candidate");
        goto failed;
    }
    allow_downgrade = (record.request_flags &
                       HW1_OTA_REQUEST_ALLOW_DOWNGRADE) != 0;
    err = updater_open_staged_candidate(s_ota0, allow_downgrade,
                                        &candidate, reason, sizeof(reason));
    feed_control_watchdog();
    if (err != ESP_OK) {
        goto failed;
    }
    if (!hw1_ota_record_candidate_matches_verified(&record,
                                                    &candidate.verified)) {
        err = ESP_ERR_INVALID_CRC;
        snprintf(reason, sizeof(reason),
                 "fresh signed staged envelope does not match journal");
        updater_close_candidate(&candidate);
        goto failed;
    }

    /* Replay a power cut after esp_ota_end without erasing a second time. */
    (void)updater_validate_existing_ota0(s_ota0, &main_valid, &main_state);
    feed_control_watchdog();
    if (main_valid && record.phase == HW1_OTA_PHASE_APPLYING &&
        updater_verify_written_candidate(s_ota0, &candidate.verified,
                                         reason, sizeof(reason)) == ESP_OK) {
        err = ESP_OK;
    } else if (record.phase == HW1_OTA_PHASE_IMAGE_VERIFIED ||
               record.phase == HW1_OTA_PHASE_TRIAL_BOOT_ARMED) {
        err = updater_verify_written_candidate(s_ota0, &candidate.verified,
                                               reason, sizeof(reason));
    } else {
        err = write_staged_candidate(&candidate, &record,
                                     reason, sizeof(reason));
    }
    feed_control_watchdog();
    updater_close_candidate(&candidate);
    if (err == ESP_OK) {
        err = finalize_verified_image(&record, &candidate.verified,
                                      reason, sizeof(reason));
    }
    /* No-ops unless write_staged_candidate() actually opened the banner. */
    progress_console_end(err == ESP_OK,
                         err == ESP_OK ? "image verified and trial boot armed"
                                       : reason);
    if (err != ESP_OK) {
        goto failed;
    }
    xSemaphoreGive(s_operation_mutex);
    return;

failed:
    if (reason[0] == '\0') {
        snprintf(reason, sizeof(reason), "staged update failed: %s",
                 esp_err_to_name(err));
    }
    if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC) {
        record_failure(&record, HW1_OTA_RESULT_SIGNATURE_INVALID, err, reason);
    } else if (err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_IMAGE_INVALID) {
        record_failure(&record, HW1_OTA_RESULT_INCOMPATIBLE_IMAGE, err, reason);
    } else if (err == ESP_ERR_INVALID_STATE) {
        record_failure(&record, HW1_OTA_RESULT_POWER_UNSAFE, err, reason);
    } else {
        record_failure(&record, HW1_OTA_RESULT_FLASH_ERROR, err, reason);
    }
    xSemaphoreGive(s_operation_mutex);
}

static int receive_exact(recovery_upload_t *upload, uint8_t *buffer,
                         size_t size, int64_t deadline_us)
{
    size_t received = 0;
    while (received < size) {
        int got;
        if (esp_timer_get_time() >= deadline_us) {
            return -1;
        }
        got = upload->receive(upload->context, buffer + received,
                              size - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            return got;
        }
        received += (size_t)got;
        note_activity(NULL);
    }
    return (int)received;
}

static esp_err_t accept_manifest(const uint8_t *body, size_t body_size,
                                 char *reason, size_t reason_size,
                                 void *context)
{
    hw1_ota_verified_manifest_t fresh;
    hw1_ota_record_t record = {0};
    uint16_t retry_flags = 0;
    esp_err_t err;
    (void)context;
    if (!s_control_watchdog_active) {
        snprintf(reason, reason_size,
                 "control watchdog unavailable; update writes disabled");
        return ESP_ERR_INVALID_STATE;
    }
    /* 0-timeout, not 1000 ms: this runs on the single esp_http_server
    * task, so a blocking wait stalls every other socket - including
    * /status and /cancel - before failing anyway. */
    if (xSemaphoreTake(s_operation_mutex, 0) != pdTRUE) {
        snprintf(reason, reason_size, "another OTA operation is active");
        return ESP_ERR_TIMEOUT;
    }
    err = updater_verify_manifest_envelope(body, body_size, &fresh,
                                           reason, reason_size);
    if (err == ESP_OK) {
        /* Authenticates exact board/layout/project/suffix/updater/schema/size/hash. */
        err = updater_validate_manifest_contract(
            &fresh, fresh.manifest.image_size, fresh.manifest.image_sha256,
            reason, reason_size);
    }
    if (err == ESP_OK) {
        err = load_record(&record, true);
        if (err != ESP_OK) {
            snprintf(reason, reason_size,
                     "OTA journal corrupt/unavailable; use serial resetjournal confirm");
        }
    }
    if (err == ESP_OK &&
        (record.phase == HW1_OTA_PHASE_FAILED ||
         record.phase == HW1_OTA_PHASE_CANCELED ||
         record.phase == HW1_OTA_PHASE_SUCCEEDED)) {
        retry_flags = record.phase == HW1_OTA_PHASE_FAILED
                          ? record.request_flags : 0;
        if (hw1_ota_result_pending(&record)) {
            retry_flags |= HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT;
        }
        err = transition_record(&record, HW1_OTA_EVENT_CLEAR_TERMINAL, NULL);
        if (err == ESP_OK &&
            hw1_ota_begin(&record, new_operation_id(),
                          HW1_OTA_SOURCE_RECOVERY_UPLOAD, retry_flags,
                          NULL) != HW1_OTA_OK) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) {
            err = commit_record(&record);
        }
    } else if (err == ESP_OK && record.phase == HW1_OTA_PHASE_IDLE) {
        if (hw1_ota_result_pending(&record)) {
            retry_flags |= HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT;
        }
        if (hw1_ota_begin(&record, new_operation_id(),
                          HW1_OTA_SOURCE_RECOVERY_UPLOAD, retry_flags,
                          NULL) != HW1_OTA_OK) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = commit_record(&record);
        }
    }
    if (err == ESP_OK && record.candidate_present &&
        !hw1_ota_record_candidate_matches_verified(&record, &fresh)) {
        err = ESP_ERR_INVALID_CRC;
        snprintf(reason, reason_size,
                 "signed manifest does not match journaled candidate");
    }
    if (err == ESP_OK) {
        err = updater_validate_not_downgrade(
            s_ota0, &fresh,
            (record.request_flags & HW1_OTA_REQUEST_ALLOW_DOWNGRADE) != 0,
            reason, reason_size);
    }
    if (err == ESP_OK &&
        (record.phase == HW1_OTA_PHASE_IMAGE_VERIFIED ||
         record.phase == HW1_OTA_PHASE_TRIAL_BOOT_ARMED)) {
        err = updater_verify_written_candidate(s_ota0, &fresh,
                                               reason, reason_size);
        if (err == ESP_OK) {
            err = finalize_verified_image(&record, &fresh,
                                          reason, reason_size);
        }
    } else if (err == ESP_OK) {
        err = begin_or_resume_applying(&record, reason, reason_size);
        if (err == ESP_OK) {
            s_upload_manifest = fresh;
            s_upload_operation_id = record.operation_id;
            s_upload_manifest_valid = true;
            snapshot_set(UPDATER_STATE_IDLE,
                         "manifest authenticated; awaiting exact firmware body");
            /* The operator's next action is the multi-minute one. Warn before
             * it starts, not once they are already staring at a dead prompt. */
            printf("\n[recovery] Manifest authenticated for %s (%u bytes).\n"
                   "[recovery] Send the matching image next; that transfer runs "
                   "1-3 minutes with no HTTP reply until it finishes.\n",
                   fresh.manifest.version,
                   (unsigned)fresh.manifest.image_size);
            fflush(stdout);
        }
    }
    if (err != ESP_OK && reason[0] == '\0') {
        snprintf(reason, reason_size, "manifest refused: %s",
                 esp_err_to_name(err));
    }
    xSemaphoreGive(s_operation_mutex);
    return err;
}

static esp_err_t accept_firmware(recovery_upload_t *upload,
                                 char *reason, size_t reason_size,
                                 void *context)
{
    hw1_ota_record_t record = {0};
    esp_app_desc_t app_desc;
    esp_ota_handle_t handle = 0;
    mbedtls_sha256_context sha;
    uint8_t prefix[HW1_UPDATER_IMAGE_PREFIX_SIZE];
    uint8_t *buffer = NULL;
    uint8_t digest[HW1_OTA_SHA256_SIZE];
    size_t written;
    int crypto_err;
    int64_t deadline;
    esp_err_t err = ESP_OK;
    bool ota_started = false;
    (void)context;
    if (upload == NULL || upload->receive == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!validated_layout_ready()) {
        snprintf(reason, reason_size,
                 "validated OTA layout/NVS state is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_control_watchdog_active) {
        snprintf(reason, reason_size,
                 "control watchdog unavailable; update writes disabled");
        return ESP_ERR_INVALID_STATE;
    }
    /* 0-timeout, not 1000 ms: this runs on the single esp_http_server
    * task, so a blocking wait stalls every other socket - including
    * /status and /cancel - before failing anyway. */
    if (xSemaphoreTake(s_operation_mutex, 0) != pdTRUE) {
        snprintf(reason, reason_size, "another OTA operation is active");
        return ESP_ERR_TIMEOUT;
    }
    if (!s_upload_manifest_valid) {
        err = ESP_ERR_INVALID_STATE;
        snprintf(reason, reason_size,
                 "firmware upload has no freshly signed manifest");
        goto done;
    }
    err = load_record(&record, false);
    if (err != ESP_OK || record.operation_id != s_upload_operation_id ||
        record.phase != HW1_OTA_PHASE_APPLYING ||
        (record.candidate_present &&
         !hw1_ota_record_candidate_matches_verified(&record,
                                                    &s_upload_manifest))) {
        err = ESP_ERR_INVALID_STATE;
        snprintf(reason, reason_size,
                 "fresh manifest no longer matches active OTA journal");
        goto done;
    }
    /* Load and authenticate the active record before validating the body
     * length. A wrong-length upload is a terminal failure of the APPLYING
     * transaction; checking it before load_record() left the durable journal
     * stuck in APPLYING because record_failure() received an empty record. */
    if (upload->content_length != s_upload_manifest.manifest.image_size ||
        upload->content_length > s_ota0->size ||
        upload->content_length < sizeof(prefix)) {
        err = ESP_ERR_INVALID_SIZE;
        snprintf(reason, reason_size,
                 "firmware size does not match freshly signed manifest");
        goto done;
    }
    deadline = esp_timer_get_time() +
               (int64_t)CONFIG_HW1_UPDATER_APPLY_TIMEOUT_SEC * 1000000LL;
    if (receive_exact(upload, prefix, sizeof(prefix), deadline) !=
        (int)sizeof(prefix)) {
        err = ESP_FAIL;
        snprintf(reason, reason_size, "firmware header receive failed");
        goto done;
    }
    err = updater_validate_image_prefix(prefix, sizeof(prefix),
                                        &s_upload_manifest, &app_desc,
                                        reason, reason_size);
    if (err == ESP_OK) {
        err = updater_validate_not_downgrade(
            s_ota0, &s_upload_manifest,
            (record.request_flags & HW1_OTA_REQUEST_ALLOW_DOWNGRADE) != 0,
            reason, reason_size);
    }
    if (err == ESP_OK &&
        (record.request_flags & HW1_OTA_REQUEST_FORCED_POWER_OVERRIDE) == 0) {
        err = updater_power_require_safe(reason, reason_size);
    }
    if (err != ESP_OK) {
        goto done;
    }

    snapshot_set(UPDATER_STATE_WRITING,
                 "streaming signed image directly to ota_0");
    snapshot_progress(0, upload->content_length);
    progress_console_begin("Receiving and writing firmware to ota_0",
                           upload->content_length);
    /* Avoid one partition-sized blocking erase. This keeps the platform idle
     * watchdogs serviceable while each received chunk erases and writes. */
    err = esp_ota_begin(s_ota0, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        snprintf(reason, reason_size, "esp_ota_begin failed: %s",
                 esp_err_to_name(err));
        goto done;
    }
    ota_started = true;
    mbedtls_sha256_init(&sha);
    crypto_err = mbedtls_sha256_starts(&sha, 0);
    if (crypto_err == 0) {
        crypto_err = mbedtls_sha256_update(&sha, prefix, sizeof(prefix));
    }
    if (crypto_err != 0 ||
        esp_ota_write(handle, prefix, sizeof(prefix)) != ESP_OK) {
        err = ESP_FAIL;
        snprintf(reason, reason_size, "initial OTA write/hash failed");
        mbedtls_sha256_free(&sha);
        goto done;
    }
    written = sizeof(prefix);
    snapshot_progress(written, upload->content_length);
    buffer = malloc(OTA_WRITE_CHUNK);
    if (buffer == NULL) {
        err = ESP_ERR_NO_MEM;
        snprintf(reason, reason_size, "OTA receive buffer allocation failed");
        mbedtls_sha256_free(&sha);
        goto done;
    }
    while (written < upload->content_length) {
        size_t wanted = upload->content_length - written;
        if (wanted > OTA_WRITE_CHUNK) {
            wanted = OTA_WRITE_CHUNK;
        }
        if (receive_exact(upload, buffer, wanted, deadline) != (int)wanted) {
            err = ESP_FAIL;
            snprintf(reason, reason_size,
                     "firmware receive failed at %u bytes", (unsigned)written);
            break;
        }
        if (mbedtls_sha256_update(&sha, buffer, wanted) != 0) {
            err = ESP_FAIL;
            snprintf(reason, reason_size, "firmware SHA-256 update failed");
            break;
        }
        err = esp_ota_write(handle, buffer, wanted);
        if (err != ESP_OK) {
            snprintf(reason, reason_size, "esp_ota_write failed at %u: %s",
                     (unsigned)written, esp_err_to_name(err));
            break;
        }
        written += wanted;
        snapshot_progress(written, upload->content_length);
        progress_console_tick(written, upload->content_length);
    }
    if (err == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) {
        err = ESP_FAIL;
        snprintf(reason, reason_size, "firmware SHA-256 finalization failed");
    }
    mbedtls_sha256_free(&sha);
    if (err == ESP_OK &&
        memcmp(digest, s_upload_manifest.manifest.image_sha256,
               sizeof(digest)) != 0) {
        err = ESP_ERR_INVALID_CRC;
        snprintf(reason, reason_size,
                 "uploaded firmware SHA-256 does not match signed manifest");
    }
    if (err != ESP_OK) {
        goto done;
    }
    snapshot_set(UPDATER_STATE_VERIFYING,
                 "verifying ESP-IDF signature and written partition");
    err = esp_ota_end(handle);
    ota_started = false;
    if (err != ESP_OK) {
        snprintf(reason, reason_size, "esp_ota_end rejected image: %s",
                 esp_err_to_name(err));
        goto done;
    }
    err = updater_verify_written_candidate(s_ota0, &s_upload_manifest,
                                           reason, reason_size);
    if (err == ESP_OK) {
        err = finalize_verified_image(&record, &s_upload_manifest,
                                      reason, reason_size);
    }

done:
    progress_console_end(err == ESP_OK,
                         err == ESP_OK ? "image verified and trial boot armed"
                                       : reason);
    if (ota_started) {
        (void)esp_ota_abort(handle);
    }
    if (buffer != NULL) {
        memset(buffer, 0, OTA_WRITE_CHUNK);
        free(buffer);
    }
    memset(prefix, 0, sizeof(prefix));
    memset(digest, 0, sizeof(digest));
    if (err != ESP_OK) {
        hw1_ota_result_code_t code = HW1_OTA_RESULT_FLASH_ERROR;
        if (err == ESP_ERR_INVALID_CRC) {
            code = HW1_OTA_RESULT_DIGEST_MISMATCH;
        } else if (err == ESP_ERR_INVALID_VERSION ||
                   err == ESP_ERR_IMAGE_INVALID) {
            code = HW1_OTA_RESULT_INCOMPATIBLE_IMAGE;
        } else if (err == ESP_ERR_INVALID_STATE) {
            code = HW1_OTA_RESULT_POWER_UNSAFE;
        }
        record_failure(&record, code, err, reason);
    } else {
        s_upload_manifest_valid = false;
    }
    xSemaphoreGive(s_operation_mutex);
    return err;
}

static esp_err_t allow_downgrade_for_transaction(void);

static esp_err_t network_action(recovery_action_t action, void *context)
{
    (void)context;
    switch (action) {
    case RECOVERY_ACTION_APPLY_STAGED:
        return enqueue_control(CONTROL_APPLY_STAGED, false);
    case RECOVERY_ACTION_CANCEL:
        return enqueue_control(CONTROL_CANCEL, false);
    case RECOVERY_ACTION_REBOOT:
        return enqueue_control(CONTROL_REBOOT, false);
    case RECOVERY_ACTION_ALLOW_DOWNGRADE:
        /* 0-timeout, not 1000 ms: this runs on the single esp_http_server
        * task, so a blocking wait stalls every other socket - including
        * /status and /cancel - before failing anyway. */
        if (xSemaphoreTake(s_operation_mutex, 0) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        {
            esp_err_t err = allow_downgrade_for_transaction();
            xSemaphoreGive(s_operation_mutex);
            return err;
        }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t start_network(void)
{
    recovery_credentials_t credentials;
    recovery_network_callbacks_t callbacks = {
        .action = network_action,
        .status = status_json,
        .activity = note_activity,
        .manifest = accept_manifest,
        .firmware = accept_firmware,
        .context = NULL,
    };
    esp_err_t err;
    if (!validated_layout_ready()) {
        ESP_LOGE(TAG,
                 "Validated OTA layout/NVS state unavailable; SoftAP remains off");
        return ESP_ERR_INVALID_STATE;
    }
    err = load_credentials(&credentials);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Persistent 12..63-character recovery credential unavailable; "
                 "SoftAP remains off (%s)", esp_err_to_name(err));
        return err;
    }
    err = recovery_network_start(&credentials, &callbacks);
    memset(&credentials, 0, sizeof(credentials));
    if (err == ESP_OK &&
        xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot.network_started = true;
        xSemaphoreGive(s_snapshot_mutex);
    }
    return err;
}

static hw1_ota_image_state_t observed_main_state(bool valid,
                                                 esp_ota_img_states_t state)
{
    if (!valid) {
        return HW1_OTA_IMAGE_INVALID;
    }
    if (state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY) {
        return HW1_OTA_IMAGE_PENDING_VERIFY;
    }
    if (state == ESP_OTA_IMG_VALID) {
        return HW1_OTA_IMAGE_ACCEPTED;
    }
    if (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED) {
        return HW1_OTA_IMAGE_INVALID;
    }
    return HW1_OTA_IMAGE_VALID;
}

static hw1_ota_boot_target_t configured_boot_target(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot == NULL) {
        return HW1_OTA_BOOT_TARGET_UNKNOWN;
    }
    return boot->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY
               ? HW1_OTA_BOOT_TARGET_RECOVERY
               : boot->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0
                     ? HW1_OTA_BOOT_TARGET_MAIN
                     : HW1_OTA_BOOT_TARGET_UNKNOWN;
}

static esp_err_t begin_emergency_recovery(hw1_ota_record_t *record)
{
    uint16_t flags = hw1_ota_result_pending(record)
                         ? HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT
                         : 0;
    if (hw1_ota_begin(record, new_operation_id(),
                      HW1_OTA_SOURCE_RECOVERY_UPLOAD, flags, NULL) != HW1_OTA_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return commit_record(record);
}

static void boot_reconcile(void)
{
    hw1_ota_record_t record = {0};
    hw1_ota_observation_t observation;
    hw1_ota_reconcile_decision_t decision;
    hw1_ota_transition_args_t reconciled_rollback = {
        .verified_manifest = NULL,
        .result_code = HW1_OTA_RESULT_ROLLBACK_DETECTED,
        .native_error = 0,
        .detail = "trial image rejected before it could report running",
    };
    bool main_valid = false;
    esp_ota_img_states_t main_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err;
    unsigned pass;
    feed_control_watchdog();
    err = updater_validate_existing_ota0(s_ota0, &main_valid, &main_state);
    feed_control_watchdog();
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot.main_valid = main_valid;
        s_snapshot.main_state = main_state;
        xSemaphoreGive(s_snapshot_mutex);
    }
    if (err != ESP_OK) {
        /* The main image could not be examined (read/mmap failure), as opposed
         * to being examined and found invalid. Reconciling on that would let a
         * transient fault masquerade as a rejected trial image and commit a
         * terminal rollback. Hold instead, with the recovery network up, and
         * let the next boot — or an operator — decide. */
        snapshot_set(UPDATER_STATE_HELD,
                     "main image could not be examined; not reconciling");
        (void)start_network();
        return;
    }
    err = load_record(&record, true);
    if (err != ESP_OK) {
        snapshot_set(UPDATER_STATE_HELD,
                     "OTA journal corrupt; serial resetjournal confirm is required");
        (void)start_network();
        return;
    }

    /* Factory selected with no coherent request is an emergency hold, not a
     * silent return to main. This covers filesystem-failure escape and a
     * deliberately selected factory image. */
    if (record.phase == HW1_OTA_PHASE_IDLE) {
        err = begin_emergency_recovery(&record);
        if (err != ESP_OK) {
            snapshot_set(UPDATER_STATE_HELD,
                         "could not journal emergency recovery hold");
            /* Every hold must still be reachable over the air. start_network()
             * has no other reachable trigger here (CONTROL_START_NETWORK is
             * enqueued only by the serial `setpin`), so returning without it
             * left the device recoverable by cable alone. */
            (void)start_network();
            return;
        }
    }

    for (pass = 0; pass < 3; ++pass) {
        observation.running_role = HW1_OTA_ROLE_RECOVERY;
        observation.configured_boot_target = configured_boot_target();
        observation.main_image_state = observed_main_state(main_valid,
                                                            main_state);
        observation.staged_candidate_available = staged_pair_available();
        if (hw1_ota_reconcile(&record, &observation, &decision) != HW1_OTA_OK) {
            snapshot_set(UPDATER_STATE_HELD,
                         "shared OTA state reconciliation rejected journal");
            (void)start_network();  /* keep the hold reachable without a cable */
            return;
        }
        if ((decision.actions & HW1_OTA_ACTION_COMMIT_EVENT) == 0) {
            break;
        }
        if (decision.event_before_actions != HW1_OTA_EVENT_RECOVERY_STARTED &&
            decision.event_before_actions != HW1_OTA_EVENT_APPLY_STARTED &&
            decision.event_before_actions != HW1_OTA_EVENT_ROLLBACK_OBSERVED) {
            /* Image/trial events require a freshly verified signed envelope;
             * persisted candidate bytes never authorize them.
             *
             * ROLLBACK_OBSERVED is exempt because it records a FAILURE and
             * installs nothing, so it needs no envelope. Refusing it was a
             * brick-class bug: a trial image that panics before journaling
             * TRIAL_STARTED leaves the record in TRIAL_BOOT_ARMED (not
             * TRIAL_RUNNING, which the fixup below covers), the core correctly
             * recommends ROLLBACK_OBSERVED, this filter dropped it, and the
             * staged-apply gate at the end of this function accepts
             * TRIAL_BOOT_ARMED — so the same crashing image was re-armed every
             * boot. CANCEL is refused from that phase and the crash-loop escape
             * counts only fault resets, not our esp_restart(), so the only way
             * out was serial `resetjournal confirm`. */
            break;
        }
        if (decision.event_before_actions == HW1_OTA_EVENT_ROLLBACK_OBSERVED) {
            /* last_result.detail is the operator's only account of why the
             * update stopped, so name the phase we actually rolled back from. */
            reconciled_rollback.detail =
                record.phase == HW1_OTA_PHASE_TRIAL_RUNNING
                    ? "trial main returned to factory recovery"
                    : "trial image rejected before it could report running";
        }
        err = transition_record(&record, decision.event_before_actions,
                                decision.event_before_actions ==
                                        HW1_OTA_EVENT_ROLLBACK_OBSERVED
                                    ? &reconciled_rollback
                                    : NULL);
        if (err != ESP_OK) {
            snapshot_set(UPDATER_STATE_HELD,
                         "could not commit reconciled recovery state");
            (void)start_network();  /* keep the hold reachable without a cable */
            return;
        }
    }

    if ((record.phase == HW1_OTA_PHASE_SUCCEEDED ||
         record.phase == HW1_OTA_PHASE_CANCELED) && main_valid &&
        main_state != ESP_OTA_IMG_ABORTED &&
        main_state != ESP_OTA_IMG_INVALID) {
        if (set_main_boot_and_reboot() != ESP_OK) {
            snapshot_set(UPDATER_STATE_HELD,
                         "terminal transaction could not return to main");
        }
        return;
    }
    /* Backstop only. The reconcile loop above now commits ROLLBACK_OBSERVED for
     * both TRIAL_RUNNING and TRIAL_BOOT_ARMED, so this should already be FAILED;
     * kept because an early loop exit must never leave a trial phase live. The
     * transition is a no-op from a terminal phase. */
    if (record.phase == HW1_OTA_PHASE_TRIAL_RUNNING) {
        hw1_ota_transition_args_t rollback = {
            .verified_manifest = NULL,
            .result_code = HW1_OTA_RESULT_ROLLBACK_DETECTED,
            .native_error = 0,
            .detail = "trial main returned to factory recovery",
        };
        (void)transition_record(&record, HW1_OTA_EVENT_ROLLBACK_OBSERVED,
                                &rollback);
    }

    snapshot_set(record.phase == HW1_OTA_PHASE_FAILED
                     ? UPDATER_STATE_HELD : UPDATER_STATE_IDLE,
                 record.phase == HW1_OTA_PHASE_FAILED
                     ? "update failed; authenticated recovery is holding"
                     : "authenticated factory recovery ready");
    (void)start_network();

    if (record.source == HW1_OTA_SOURCE_STAGED_FILE &&
        staged_pair_available() &&
        (record.phase == HW1_OTA_PHASE_APPLYING ||
         record.phase == HW1_OTA_PHASE_RECOVERY_RUNNING ||
         record.phase == HW1_OTA_PHASE_IMAGE_VERIFIED ||
         record.phase == HW1_OTA_PHASE_TRIAL_BOOT_ARMED)) {
        apply_staged();
    }
}

static void cancel_recovery(void)
{
    hw1_ota_record_t record = {0};
    bool main_eligible = false;
    esp_err_t err;
    if (xSemaphoreTake(s_operation_mutex, 0) != pdTRUE) {
        /* Same reasoning as apply_staged: the refusal is about THIS request,
         * not about the health of the running write, so it must not latch
         * ERROR into the status every viewer is polling. */
        ESP_LOGW(TAG, "cancel refused: a flash write is currently active");
        return;
    }
    err = load_record(&record, false);
    if (err == ESP_OK) {
        feed_control_watchdog();
        main_eligible = main_partition_is_return_eligible(false);
        feed_control_watchdog();
    }

    /* FAILED is an audit result, not a state to rewrite as CANCELED. An
     * explicit operator cancel may still boot a fully verified compatible
     * main image; the main app will retain and report the failed result. */
    if (err == ESP_OK && record.phase == HW1_OTA_PHASE_FAILED) {
        if (main_eligible) {
            err = set_main_boot_and_reboot();
        } else {
            err = ESP_ERR_IMAGE_INVALID;
        }
        if (err != ESP_OK) {
            snapshot_set(UPDATER_STATE_HELD,
                         "failed update retained; no eligible signed main image");
        }
        xSemaphoreGive(s_operation_mutex);
        return;
    }

    if (err == ESP_OK) {
        err = transition_record(&record, HW1_OTA_EVENT_CANCEL, NULL);
    }
    if (err != ESP_OK) {
        snapshot_set(UPDATER_STATE_HELD,
                     "cancel request could not be committed; recovery remains held");
    } else {
        clear_upload_authorization();
        if (main_eligible) {
            err = set_main_boot_and_reboot();
            if (err != ESP_OK) {
                snapshot_set(
                    UPDATER_STATE_HELD,
                    "transaction canceled; main selection failed; recovery remains held");
            }
        } else {
            snapshot_set(
                UPDATER_STATE_HELD,
                "transaction canceled; invalid main held for a new authenticated upload");
        }
    }
    xSemaphoreGive(s_operation_mutex);
}

static esp_err_t reset_journal_slots(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    esp_err_t one;
    if (!s_nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    err = nvs_open(HW1_OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    one = nvs_erase_key(handle, HW1_OTA_NVS_SLOT_A_KEY);
    if (one != ESP_OK && one != ESP_ERR_NVS_NOT_FOUND) {
        err = one;
    }
    one = nvs_erase_key(handle, HW1_OTA_NVS_SLOT_B_KEY);
    if (err == ESP_OK && one != ESP_OK && one != ESP_ERR_NVS_NOT_FOUND) {
        err = one;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        s_upload_manifest_valid = false;
        snapshot_set(UPDATER_STATE_HELD,
                     "OTA journal slots reset; credentials/data preserved; reboot recovery");
    }
    return err;
}

static esp_err_t allow_downgrade_for_transaction(void)
{
    hw1_ota_record_t record = {0};
    esp_err_t err = load_record(&record, false);
    if (err != ESP_OK) {
        return err;
    }
    if (record.phase == HW1_OTA_PHASE_IDLE ||
        record.phase == HW1_OTA_PHASE_SUCCEEDED ||
        record.phase == HW1_OTA_PHASE_CANCELED ||
        record.phase == HW1_OTA_PHASE_IMAGE_VERIFIED ||
        record.phase == HW1_OTA_PHASE_TRIAL_BOOT_ARMED ||
        record.phase == HW1_OTA_PHASE_TRIAL_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    record.request_flags |= HW1_OTA_REQUEST_ALLOW_DOWNGRADE;
    err = commit_record(&record);
    if (err == ESP_OK) {
        snapshot_set(UPDATER_STATE_IDLE,
                     "explicit downgrade permission persisted for this transaction");
    }
    return err;
}

/* format_littlefs_migration_only() is deliberately gone.  The relocated
 * filesystem is now made mountable by flashing a host-built blank littlefs
 * image as part of `migration-flash`, so no firmware on the device carries
 * the ability to erase its own storage.  To wipe a device's filesystem, use
 * the host `littlefs-flash` cable target. */

static void process_idle_timeout(void)
{
    updater_snapshot_t status = snapshot_copy();
    hw1_ota_record_t record = {0};
    bool main_valid = false;
    esp_ota_img_states_t main_state;
    if (status.state != UPDATER_STATE_IDLE &&
        status.state != UPDATER_STATE_ERROR) {
        return;
    }
    if (esp_timer_get_time() - last_activity_us() <
        (int64_t)CONFIG_HW1_UPDATER_IDLE_TIMEOUT_SEC * 1000000LL) {
        return;
    }
    if (load_record(&record, false) != ESP_OK ||
        record.phase == HW1_OTA_PHASE_FAILED ||
        record.phase == HW1_OTA_PHASE_APPLYING ||
        record.phase == HW1_OTA_PHASE_IMAGE_VERIFIED ||
        record.phase == HW1_OTA_PHASE_TRIAL_BOOT_ARMED) {
        snapshot_set(UPDATER_STATE_HELD,
                     record.phase == HW1_OTA_PHASE_FAILED
                         ? "idle timeout; failed update remains held for operator review"
                         : "idle timeout; active/unknown transaction held safely");
        return;
    }
    feed_control_watchdog();
    (void)updater_validate_existing_ota0(s_ota0, &main_valid, &main_state);
    feed_control_watchdog();
    if (main_valid && main_state != ESP_OTA_IMG_INVALID &&
        main_state != ESP_OTA_IMG_ABORTED) {
        esp_err_t err = ESP_OK;
        if (record.phase == HW1_OTA_PHASE_RECOVERY_RUNNING) {
            err = transition_record(&record, HW1_OTA_EVENT_CANCEL, NULL);
        }
        if (err == ESP_OK) {
            err = set_main_boot_and_reboot();
        }
        if (err != ESP_OK) {
            snapshot_set(UPDATER_STATE_HELD,
                         "idle timeout; safe return to main could not be committed");
        }
    } else {
        snapshot_set(UPDATER_STATE_HELD,
                     "idle timeout; invalid main requires authenticated recovery");
    }
}

static void control_task(void *argument)
{
    control_command_t command;
    char layout_reason[STATUS_DETAIL_SIZE] = {0};
    esp_err_t layout_err;
    esp_err_t watchdog_err;
    (void)argument;
    watchdog_err = start_control_watchdog();
    if (watchdog_err != ESP_OK) {
        ESP_LOGE(TAG, "control-task watchdog unavailable: %s",
                 esp_err_to_name(watchdog_err));
    }
    layout_err = updater_validate_layout(&s_ota0, layout_reason,
                                         sizeof(layout_reason));
    feed_control_watchdog();
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot.layout_valid = layout_err == ESP_OK;
        xSemaphoreGive(s_snapshot_mutex);
    }
    if (watchdog_err != ESP_OK) {
        snapshot_set(UPDATER_STATE_HELD,
                     "control watchdog unavailable; update writes disabled");
        if (layout_err == ESP_OK && s_nvs_ready) {
            (void)start_network();
        }
    } else if (layout_err != ESP_OK || !s_nvs_ready) {
        snapshot_set(UPDATER_STATE_HELD,
                     layout_err != ESP_OK ? layout_reason
                                          : "NVS init failed; no erase/retry was attempted");
    } else {
        boot_reconcile();
    }
    set_last_activity_now();
    for (;;) {
        feed_control_watchdog();
        if (xQueueReceive(s_control_queue, &command,
                          pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (command.type == CONTROL_APPLY_STAGED) {
                if (s_control_watchdog_active) {
                    apply_staged();
                } else {
                    snapshot_set(
                        UPDATER_STATE_HELD,
                        "control watchdog unavailable; staged apply refused");
                }
            } else if (command.type == CONTROL_CANCEL) {
                cancel_recovery();
            } else if (command.type == CONTROL_REBOOT) {
                snapshot_set(UPDATER_STATE_REBOOTING, "restarting");
                vTaskDelay(pdMS_TO_TICKS(750));
                esp_restart();
            } else if (command.type == CONTROL_START_NETWORK) {
                if (!command.from_serial) {
                    ESP_LOGE(TAG, "network start request is serial-only");
                } else if (!snapshot_copy().network_started) {
                    esp_err_t err;
                    feed_control_watchdog();
                    err = start_network();
                    feed_control_watchdog();
                    ESP_LOGI(TAG, "serial recovery network start: %s",
                             esp_err_to_name(err));
                }
            } else if (command.type == CONTROL_ALLOW_DOWNGRADE) {
                if (xSemaphoreTake(s_operation_mutex, 0) == pdTRUE) {
                    esp_err_t err = allow_downgrade_for_transaction();
                    ESP_LOGW(TAG, "explicit allow-downgrade: %s",
                             esp_err_to_name(err));
                    xSemaphoreGive(s_operation_mutex);
                }
            } else if (command.type == CONTROL_RESET_JOURNAL) {
                if (!command.from_serial) {
                    ESP_LOGE(TAG, "resetjournal is serial-only");
                } else if (xSemaphoreTake(s_operation_mutex, 0) == pdTRUE) {
                    esp_err_t err = reset_journal_slots();
                    ESP_LOGI(TAG, "resetjournal: %s", esp_err_to_name(err));
                    xSemaphoreGive(s_operation_mutex);
                }
            }
        }
        feed_control_watchdog();
        process_idle_timeout();
    }
}

static void print_status_console(void)
{
    /* Keep the console task's stack shallow. status_json() is also used by
     * HTTP, so this console-only buffer does not create cross-task sharing. */
    /* Shares STATUS_JSON_MAX with the HTTP /status buffer: both render the same
     * document, and a private constant here silently truncated the console copy
     * once the JSON outgrew it. */
    static char json[STATUS_JSON_MAX];
    size_t length = status_json(json, sizeof(json), NULL);
    fwrite(json, 1, length, stdout);
    fputc('\n', stdout);
}

/* There is nothing sensitive in this list, and an operator who reaches this
 * prompt is usually here because something already went wrong.  Printing it on
 * request - and naming it when a command is not recognised - costs nothing and
 * saves guessing at a prompt that only accepts seven exact strings. */
static void print_console_help(void)
{
    puts("commands:");
    puts("  help                     show this list");
    puts("  unlock <credential>      authorize the mutating commands below");
    puts("  status                   print recovery state as JSON");
    puts("  apply                    install the staged firmware pair [locked]");
    puts("  cancel                   leave recovery and boot the main app");
    puts("  reboot                   restart the recovery updater");
    puts("  setpin <12..63 chars>    set the recovery WPA2 + HTTP credential [locked]");
    puts("  allowdowngrade confirm   permit an older signed version this once [locked]");
    puts("  resetjournal confirm     repair the two OTA journal slots [locked]");
    puts("[locked] commands need 'unlock' once a credential is set.");
}

/* Serial console authorization.
 *
 * The hole this closes: `setpin` writes its argument to BOTH the WPA2 PSK and
 * the HTTP Basic password, so five seconds of physical access converted into
 * PERSISTENT REMOTE ownership of the recovery AP. The attacker walked away and
 * still had the device over the air. No eFuse fixes that; this does.
 *
 * Deliberately NOT gated: status, help, reboot, and above all `cancel`. Cancel
 * is escape hatch #1 - it boots the main app, where a superadmin can reset the
 * credential with `otapin` (same NVS namespace and keys, verified). Gating it
 * would strand a locked-out operator in recovery.
 *
 * Open while no credential is stored, because otherwise a fresh device could
 * never be provisioned - you would need the pin to set the pin.
 *
 * Honest limits: the gate lives in the binary an attacker can replace, and the
 * throttle is RAM-resident so a reset clears it. This raises the cost of casual
 * physical access; it does not stop someone who keeps the board. Escape hatch
 * #2 remains a cable erase and re-provision, which works as long as ROM
 * download mode is enabled. */
static auth_throttle_t s_console_throttle;
static bool s_console_unlocked;
/* The throttle is keyed by peer address for HTTP; the console is a single
 * pseudo-peer with its own table instance, so console attempts can never evict
 * or be evicted by a network peer. */
#define CONSOLE_PSEUDO_PEER 1u

static bool console_credential_required(void)
{
    recovery_credentials_t credentials;
    const bool present = load_credentials(&credentials) == ESP_OK;
    secure_zero(&credentials, sizeof(credentials));
    return present;
}

static bool console_authorized(void)
{
    return s_console_unlocked || !console_credential_required();
}

/* Returns true when the caller should treat this line as consumed. */
static bool console_handle_unlock(const char *argument)
{
    recovery_credentials_t credentials;
    int64_t retry_us = 0;
    bool ok;
    if (!console_credential_required()) {
        puts("OK: no recovery credential is set; console is already open");
        return true;
    }
    if (auth_throttle_begin(&s_console_throttle, CONSOLE_PSEUDO_PEER,
                            esp_timer_get_time(), &retry_us) ==
        AUTH_DECISION_BLOCKED) {
        printf("Error: too many failed attempts; retry in %lld seconds\n",
               (long long)((retry_us + 999999) / 1000000));
        return true;
    }
    if (load_credentials(&credentials) != ESP_OK) {
        puts("Error: recovery credential unreadable");
        return true;
    }
    ok = auth_constant_time_equals(argument, credentials.auth_token);
    secure_zero(&credentials, sizeof(credentials));
    if (!ok) {
        auth_throttle_record_failure(&s_console_throttle, CONSOLE_PSEUDO_PEER,
                                     esp_timer_get_time());
        puts("Error: incorrect credential");
        return true;
    }
    auth_throttle_record_success(&s_console_throttle, CONSOLE_PSEUDO_PEER,
                                 esp_timer_get_time());
    s_console_unlocked = true;
    note_activity(NULL);
    puts("OK: console unlocked for this session");
    return true;
}

static void console_task(void *argument)
{
    char line[96];
    (void)argument;
    puts("HardwareOne native ESP-IDF signed recovery console");
    print_console_help();
    for (;;) {
        esp_err_t err = ESP_OK;
        fputs("hw1up> ", stdout);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (strpbrk(line, "\r\n") == NULL) {
            int c;
            do {
                c = fgetc(stdin);
            } while (c != '\r' && c != '\n' && c != EOF);
            secure_zero(line, sizeof(line));
            puts("Error: command line too long");
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        /* A failed unlock must NOT refresh the idle timer, or an attacker holds
         * the device in recovery indefinitely by spamming guesses. Every other
         * line is real operator presence and should keep the session alive. */
        if (strncmp(line, "unlock ", 7) != 0) {
            note_activity(NULL);
        }
        if (strncmp(line, "unlock ", 7) == 0) {
            (void)console_handle_unlock(line + 7);
            secure_zero(line, sizeof(line));
            continue;
        }
        /* Mutating commands only. status/help/reboot/cancel stay open. */
        if ((strncmp(line, "setpin ", 7) == 0 ||
             strcmp(line, "apply") == 0 ||
             strcmp(line, "allowdowngrade confirm") == 0 ||
             strcmp(line, "resetjournal confirm") == 0) &&
            !console_authorized()) {
            secure_zero(line, sizeof(line));
            puts("Error: locked. Use 'unlock <credential>' first, or 'cancel' to "
                 "boot the main app and reset it there with 'otapin'.");
            continue;
        }
        if (strcmp(line, "status") == 0) {
            print_status_console();
            continue;
        } else if (strncmp(line, "setpin ", 7) == 0) {
            if (xSemaphoreTake(s_operation_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                bool network_was_started = snapshot_copy().network_started;
                err = store_recovery_pin(line + 7);
                secure_zero(line, sizeof(line));
                xSemaphoreGive(s_operation_mutex);
                if (err == ESP_OK) {
                    /* Provisioning a fresh device would otherwise lock the
                     * operator out with the credential they just created: the
                     * console was open only because none existed. */
                    s_console_unlocked = true;
                    if (network_was_started) {
                        puts("OK: persistent recovery credential stored; reboot required before WPA2/HTTP use the new value");
                    } else {
                        esp_err_t queue_err =
                            enqueue_control(CONTROL_START_NETWORK, true);
                        if (queue_err == ESP_OK) {
                            puts("OK: persistent recovery credential stored; authenticated recovery network start requested");
                        } else {
                            printf("OK: credential stored, but recovery network start was not queued (%s); reboot and retry\n",
                                   esp_err_to_name(queue_err));
                        }
                    }
                } else {
                    printf("Error: credential not stored (%s); use 12..63 printable characters\n",
                           esp_err_to_name(err));
                }
            } else {
                secure_zero(line, sizeof(line));
                puts("Error: another OTA operation is active");
            }
            continue;
        } else if (strcmp(line, "apply") == 0) {
            err = enqueue_control(CONTROL_APPLY_STAGED, true);
        } else if (strcmp(line, "cancel") == 0) {
            err = enqueue_control(CONTROL_CANCEL, true);
        } else if (strcmp(line, "reboot") == 0) {
            err = enqueue_control(CONTROL_REBOOT, true);
        } else if (strcmp(line, "allowdowngrade confirm") == 0) {
            err = enqueue_control(CONTROL_ALLOW_DOWNGRADE, true);
        } else if (strcmp(line, "resetjournal confirm") == 0) {
            err = enqueue_control(CONTROL_RESET_JOURNAL, true);
        } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            print_console_help();
            continue;
        } else if (line[0] == '\0') {
            continue;
        } else {
            /* Name the escape hatch rather than leaving the operator to guess
             * at a prompt that accepts only exact strings.  A bare "reboot" or
             * "cancel" typed as "restart"/"exit" is the common case. */
            printf("Error: unknown command '%s'. Type 'help' for the list.\n",
                   line);
            continue;
        }
        printf("%s: %s\n", err == ESP_OK ? "OK" : "Error",
               esp_err_to_name(err));
    }
}

static esp_err_t initialize_console_io(void)
{
    usb_serial_jtag_driver_config_t config =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err;

    /* The startup USB-Serial/JTAG VFS polls the hardware FIFO and every read
     * is nonblocking. In that mode fgets() may return a one-character
     * "line", and an idle console repeatedly returns EWOULDBLOCK. Install the
     * interrupt-driven driver so a blocking stdio read waits for a complete
     * line exactly as the console parser expects. */
    if (fcntl(fileno(stdin), F_SETFL, 0) != 0 ||
        fcntl(fileno(stdout), F_SETFL, 0) != 0) {
        return ESP_FAIL;
    }
    err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
        return err;
    }
    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t nvs_err;
    esp_err_t fs_err;
    esp_err_t console_err;
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    s_snapshot_mutex = xSemaphoreCreateMutex();
    s_operation_mutex = xSemaphoreCreateMutex();
    s_control_queue = xQueueCreate(CONTROL_QUEUE_DEPTH,
                                   sizeof(control_command_t));
    if (s_snapshot_mutex == NULL || s_operation_mutex == NULL ||
        s_control_queue == NULL) {
        ESP_LOGE(TAG, "control primitive allocation failed");
        abort();
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = UPDATER_STATE_BOOT;
    s_snapshot.main_state = ESP_OTA_IMG_UNDEFINED;
    snprintf(s_snapshot.detail, sizeof(s_snapshot.detail), "initializing");

    /* Never erase/retry NVS: it contains credentials, settings, and OTA state. */
    nvs_err = nvs_flash_init();
    s_nvs_ready = nvs_err == ESP_OK;
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot.nvs_ready = s_nvs_ready;
        xSemaphoreGive(s_snapshot_mutex);
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); no erase was attempted",
                 esp_err_to_name(nvs_err));
    }
    fs_err = mount_littlefs_read_only();
    if (fs_err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS unavailable (%s); no format was attempted",
                 esp_err_to_name(fs_err));
    }
    console_err = initialize_console_io();
    if (console_err != ESP_OK) {
        ESP_LOGE(TAG, "blocking USB recovery console unavailable: %s",
                 esp_err_to_name(console_err));
    }
    if (xTaskCreate(control_task, "hw1up_control", CONTROL_TASK_STACK,
                    NULL, 8, NULL) != pdPASS ||
        (console_err == ESP_OK &&
         xTaskCreate(console_task, "hw1up_console", CONSOLE_TASK_STACK,
                     NULL, 5, NULL) != pdPASS)) {
        ESP_LOGE(TAG, "task creation failed");
        abort();
    }
}
