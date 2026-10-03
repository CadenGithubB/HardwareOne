/* SPDX-License-Identifier: Apache-2.0
 * Derived from ESPHome esp-hosted-firmware/slave-overlay/esp_now_hosted_slave.c
 * at 14ee146ec71923aa250a7f743e5cff266a5ffb1a (see LICENSE and README.md).
 * HardwareOne changes: versioned IDs, 64-bit sequences/epochs, strict bounds,
 * real peer queries, explicit callback operations and queued native events.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_now.h" /* native C6 API; never link the host shim here */
#include "esp_ota_ops.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_hosted_peer_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_now_hosted_rpc.h"
#include "esp_now_hosted_slave.h"

#if !CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
#error "ESP-NOW overlay requires CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y"
#endif

static const char *TAG = "hw1_now_slave";

/* Kept in the image's read-only data so a host can verify a firmware file
 * carries this bridge before flashing it (see ESP_NOW_HOSTED_BRIDGE_MARKER). */
__attribute__((used)) const char esp_now_hosted_bridge_marker[] = ESP_NOW_HOSTED_BRIDGE_MARKER;
/* True between a successful OP_INIT and OP_DEINIT. esp_now_get_version() on an
 * uninitialised ESP-NOW dereferences a null driver handle (seen on hardware:
 * load access fault at 0x4c), so GET_INFO must not touch it before then. */
static bool s_espnow_initialized;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_events;
static QueueHandle_t s_requests;
static uint64_t s_epoch;
static uint32_t s_events_dropped;
static uint32_t s_transport_errors;
typedef struct {
    uint32_t msg_id;
    size_t length;
    uint8_t bytes[];
} slave_event_t;

typedef struct {
    size_t length;
    uint8_t bytes[];
} slave_request_t;

static uint64_t current_epoch(void)
{
    portENTER_CRITICAL(&s_state_lock);
    uint64_t value = s_epoch;
    portEXIT_CRITICAL(&s_state_lock);
    return value;
}

static esp_now_hosted_event_header_t event_header(size_t size, uint64_t epoch)
{
    esp_now_hosted_event_header_t header = {
        .magic = ESP_NOW_HOSTED_MAGIC,
        .version = ESP_NOW_HOSTED_WIRE_VERSION,
        .reserved = 0,
        .payload_len = (uint16_t)(size - sizeof(esp_now_hosted_event_header_t)),
        .epoch = epoch,
    };
    return header;
}

static void queue_event(slave_event_t *event)
{
    if (event && xQueueSend(s_events, &event, 0) == pdTRUE) return;
    free(event);
    portENTER_CRITICAL(&s_state_lock);
    ++s_events_dropped;
    portEXIT_CRITICAL(&s_state_lock);
}

/* Native Wi-Fi task callbacks only copy bounded metadata/payload and enqueue.
 * RPC transport allocation/transmission happens on a separate worker. */
static void slave_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    uint64_t epoch = current_epoch();
    if (!epoch || !info || !info->src_addr || !info->des_addr || !data ||
        len < 0 || len > (int)ESP_NOW_HOSTED_MAX_FRAME) return;
    size_t length = sizeof(esp_now_hosted_recv_evt_t) + (size_t)len;
    slave_event_t *event = malloc(sizeof(*event) + length);
    if (!event) { queue_event(NULL); return; }
    esp_now_hosted_recv_evt_t wire = {0};
    wire.header = event_header(length, epoch);
    memcpy(wire.src_addr, info->src_addr, 6);
    memcpy(wire.des_addr, info->des_addr, 6);
    wire.has_rx_ctrl = info->rx_ctrl != NULL;
    if (info->rx_ctrl) {
        wire.rssi = info->rx_ctrl->rssi;
        wire.channel = info->rx_ctrl->channel;
        wire.timestamp = info->rx_ctrl->timestamp;
    }
    wire.data_len = (uint16_t)len;
    event->msg_id = ESP_NOW_HOSTED_MSG_RECV;
    event->length = length;
    memcpy(event->bytes, &wire, sizeof(wire));
    memcpy(event->bytes + sizeof(wire), data, (size_t)len);
    queue_event(event);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void slave_send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
#else
static void slave_send_cb(const uint8_t *mac, esp_now_send_status_t status)
#endif
{
    uint64_t epoch = current_epoch();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    if (!epoch || !info || !info->des_addr) return;
#else
    if (!epoch || !mac) return;
#endif
    slave_event_t *event = malloc(sizeof(*event) + sizeof(esp_now_hosted_send_evt_t));
    if (!event) { queue_event(NULL); return; }
    esp_now_hosted_send_evt_t wire = {0};
    wire.header = event_header(sizeof(wire), epoch);
    wire.status = (int32_t)status;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    memcpy(wire.des_addr, info->des_addr, 6);
    wire.has_tx_info = info->src_addr != NULL;
    if (info->src_addr) memcpy(wire.src_addr, info->src_addr, 6);
    wire.ifidx = (int32_t)info->ifidx;
    wire.rate = (int32_t)info->rate;
    wire.tx_status = (int32_t)info->tx_status;
#else
    memcpy(wire.des_addr, mac, 6);
    wire.tx_status = (int32_t)status;
#endif
    event->msg_id = ESP_NOW_HOSTED_MSG_SEND;
    event->length = sizeof(wire);
    memcpy(event->bytes, &wire, sizeof(wire));
    queue_event(event);
}

static void event_task(void *argument)
{
    (void)argument;
    uint32_t reported_drops = 0;
    uint32_t reported_errors = 0;
    for (;;) {
        slave_event_t *event = NULL;
        if (xQueueReceive(s_events, &event, portMAX_DELAY) != pdTRUE) continue;
        esp_now_hosted_event_header_t header;
        memcpy(&header, event->bytes, sizeof(header));
        if (header.epoch == current_epoch()) {
            esp_err_t err = esp_hosted_send_custom_data(event->msg_id, event->bytes, event->length);
            if (err != ESP_OK) {
                portENTER_CRITICAL(&s_state_lock);
                ++s_transport_errors;
                portEXIT_CRITICAL(&s_state_lock);
            }
        }
        free(event);
        portENTER_CRITICAL(&s_state_lock);
        uint32_t drops = s_events_dropped;
        uint32_t errors = s_transport_errors;
        portEXIT_CRITICAL(&s_state_lock);
        if (drops != reported_drops || errors != reported_errors) {
            ESP_LOGW(TAG, "event losses: queue=%lu transport=%lu",
                     (unsigned long)drops, (unsigned long)errors);
            reported_drops = drops;
            reported_errors = errors;
        }
    }
}

/* Called by the single bridge request worker, before registering native
 * callbacks. Resources stay alive across deinit/init so callbacks cannot race
 * queue destruction. */
static esp_err_t ensure_event_worker(void)
{
    if (s_events) return ESP_OK;
    s_events = xQueueCreate(ESP_NOW_HOSTED_EVENT_QUEUE_DEPTH, sizeof(slave_event_t *));
    if (!s_events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(event_task, "now_events", 4096, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void peer_to_wire(const esp_now_peer_info_t *peer, esp_now_hosted_peer_t *wire)
{
    memset(wire, 0, sizeof(*wire));
    memcpy(wire->peer_addr, peer->peer_addr, 6);
    memcpy(wire->lmk, peer->lmk, 16);
    wire->channel = peer->channel;
    wire->ifidx = (int32_t)peer->ifidx;
    wire->encrypt = peer->encrypt;
}

static void process_request(const uint8_t *data, size_t len)
{
    esp_now_hosted_req_t req;
    if (!esp_now_hosted_decode_request(data, len, &req)) return;

    esp_now_hosted_resp_t response = {.request = req, .status = ESP_OK};
    response.request.payload_len = 0;
    uint8_t result[ESP_NOW_HOSTED_MAX_RETURN] = {0};
    const uint8_t *payload = data + sizeof(req);

#define EXPECT_SIZE(size_) \
    if (req.payload_len != (size_)) { response.status = ESP_ERR_INVALID_SIZE; break; }

    switch (req.opcode) {
    case ESP_NOW_HOSTED_OP_INIT:
        EXPECT_SIZE(0);
        response.status = ensure_event_worker();
        if (response.status == ESP_OK) response.status = esp_now_init();
        if (response.status == ESP_OK) s_espnow_initialized = true;
        if (response.status == ESP_OK) {
            portENTER_CRITICAL(&s_state_lock);
            s_epoch = req.seq;
            portEXIT_CRITICAL(&s_state_lock);
        }
        break;
    case ESP_NOW_HOSTED_OP_DEINIT:
        EXPECT_SIZE(0);
        response.status = esp_now_deinit();
        s_espnow_initialized = false;
        if (response.status == ESP_OK) {
            portENTER_CRITICAL(&s_state_lock);
            s_epoch = 0;
            portEXIT_CRITICAL(&s_state_lock);
        }
        break;
    case ESP_NOW_HOSTED_OP_REGISTER_RECV:
        EXPECT_SIZE(0);
        response.status = esp_now_register_recv_cb(slave_recv_cb);
        break;
    case ESP_NOW_HOSTED_OP_UNREGISTER_RECV:
        EXPECT_SIZE(0);
        response.status = esp_now_unregister_recv_cb();
        break;
    case ESP_NOW_HOSTED_OP_REGISTER_SEND:
        EXPECT_SIZE(0);
        response.status = esp_now_register_send_cb(slave_send_cb);
        break;
    case ESP_NOW_HOSTED_OP_UNREGISTER_SEND:
        EXPECT_SIZE(0);
        response.status = esp_now_unregister_send_cb();
        break;
    case ESP_NOW_HOSTED_OP_ADD_PEER:
    case ESP_NOW_HOSTED_OP_MOD_PEER: {
        EXPECT_SIZE(sizeof(esp_now_hosted_peer_t));
        esp_now_hosted_peer_t wire;
        memcpy(&wire, payload, sizeof(wire));
        if (wire.encrypt > 1) { response.status = ESP_ERR_ESPNOW_ARG; break; }
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, wire.peer_addr, 6);
        memcpy(peer.lmk, wire.lmk, 16);
        peer.channel = wire.channel;
        peer.ifidx = (wifi_interface_t)wire.ifidx;
        peer.encrypt = wire.encrypt != 0;
        response.status = req.opcode == ESP_NOW_HOSTED_OP_ADD_PEER
                              ? esp_now_add_peer(&peer) : esp_now_mod_peer(&peer);
        memset(&peer, 0, sizeof(peer));
        memset(&wire, 0, sizeof(wire));
        break;
    }
    case ESP_NOW_HOSTED_OP_DEL_PEER:
        EXPECT_SIZE(6);
        response.status = esp_now_del_peer(payload);
        break;
    case ESP_NOW_HOSTED_OP_IS_PEER_EXIST:
        EXPECT_SIZE(6);
        result[0] = esp_now_is_peer_exist(payload) ? 1 : 0;
        response.request.payload_len = 1;
        break;
    case ESP_NOW_HOSTED_OP_GET_PEER: {
        EXPECT_SIZE(6);
        esp_now_peer_info_t peer = {0};
        response.status = esp_now_get_peer(payload, &peer);
        if (response.status == ESP_OK) {
            esp_now_hosted_peer_t wire;
            peer_to_wire(&peer, &wire);
            memcpy(result, &wire, sizeof(wire));
            response.request.payload_len = sizeof(wire);
            memset(&wire, 0, sizeof(wire));
        }
        memset(&peer, 0, sizeof(peer));
        break;
    }
    case ESP_NOW_HOSTED_OP_GET_PEER_NUM: {
        EXPECT_SIZE(0);
        esp_now_peer_num_t count = {0};
        response.status = esp_now_get_peer_num(&count);
        if (response.status == ESP_OK) {
            esp_now_hosted_peer_num_t wire = {
                .total_num = count.total_num, .encrypt_num = count.encrypt_num,
            };
            memcpy(result, &wire, sizeof(wire));
            response.request.payload_len = sizeof(wire);
        }
        break;
    }
    case ESP_NOW_HOSTED_OP_GET_VERSION: {
        EXPECT_SIZE(0);
        uint32_t version = 0;
        response.status = esp_now_get_version(&version);
        if (response.status == ESP_OK) {
            memcpy(result, &version, sizeof(version));
            response.request.payload_len = sizeof(version);
        }
        break;
    }
    case ESP_NOW_HOSTED_OP_SET_PMK:
        EXPECT_SIZE(ESP_NOW_KEY_LEN);
        response.status = esp_now_set_pmk(payload);
        break;
    case ESP_NOW_HOSTED_OP_SEND: {
        if (req.payload_len < sizeof(esp_now_hosted_send_req_t)) {
            response.status = ESP_ERR_INVALID_SIZE;
            break;
        }
        esp_now_hosted_send_req_t send;
        memcpy(&send, payload, sizeof(send));
        if (send.has_addr > 1 || !send.data_len ||
            send.data_len > ESP_NOW_HOSTED_MAX_FRAME ||
            req.payload_len != sizeof(send) + send.data_len) {
            response.status = ESP_ERR_ESPNOW_ARG;
            break;
        }
        response.status = esp_now_send(send.has_addr ? send.peer_addr : NULL,
                                       payload + sizeof(send), send.data_len);
        break;
    }
    case ESP_NOW_HOSTED_OP_GET_INFO: {
        EXPECT_SIZE(0);
        esp_now_hosted_info_t info;
        memset(&info, 0, sizeof(info));
        info.wire_version = ESP_NOW_HOSTED_WIRE_VERSION;
        info.image_state = ESP_NOW_HOSTED_IMAGE_UNKNOWN;
        info.running_slot = 0xff;
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running) {
            if (running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
                running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
                info.running_slot = (uint8_t)(running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
            }
            esp_ota_img_states_t state;
            if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
                info.image_state = state == ESP_OTA_IMG_PENDING_VERIFY ? ESP_NOW_HOSTED_IMAGE_PENDING_VERIFY
                                 : (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)
                                       ? ESP_NOW_HOSTED_IMAGE_INVALID
                                       : ESP_NOW_HOSTED_IMAGE_VALID;
            } else {
                info.image_state = ESP_NOW_HOSTED_IMAGE_VALID; /* no OTA record: cable-flashed */
            }
        }
        info.reset_reason = (uint8_t)esp_reset_reason();
        info.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
        info.free_heap = esp_get_free_heap_size();
        info.min_free_heap = esp_get_minimum_free_heap_size();
        info.cpu_mhz = (uint32_t)(esp_clk_cpu_freq() / 1000000);
        uint32_t version = 0;
        if (s_espnow_initialized && esp_now_get_version(&version) == ESP_OK) info.native_espnow_version = version;
        const esp_app_desc_t *desc = esp_app_get_description();
        if (desc) memcpy(info.build, desc->version, sizeof(info.build));
        memcpy(result, &info, sizeof(info));
        response.request.payload_len = sizeof(info);
        response.status = ESP_OK;
        break;
    }
    case ESP_NOW_HOSTED_OP_CONFIRM_IMAGE:
        /* With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the bootloader rolls a
         * never-confirmed image back on the next reboot. Only the host confirms,
         * and only after this bridge answered it; a stock or broken image is
         * therefore undone by the next host reset. */
        EXPECT_SIZE(0);
        response.status = esp_ota_mark_app_valid_cancel_rollback();
        break;
    default:
        response.status = ESP_ERR_NOT_SUPPORTED;
        break;
    }
#undef EXPECT_SIZE

    uint8_t bytes[sizeof(response) + sizeof(result)];
    memcpy(bytes, &response, sizeof(response));
    memcpy(bytes + sizeof(response), result, response.request.payload_len);
    esp_err_t err = esp_hosted_send_custom_data(ESP_NOW_HOSTED_MSG_RESP, bytes,
                                                sizeof(response) + response.request.payload_len);
    memset(bytes, 0, sizeof(bytes));
    memset(result, 0, sizeof(result));
    if (err != ESP_OK) ESP_LOGW(TAG, "native response could not be queued: %s", esp_err_to_name(err));
}

static void request_task(void *argument)
{
    (void)argument;
    for (;;) {
        slave_request_t *request = NULL;
        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE) continue;
        process_request(request->bytes, request->length);
        memset(request->bytes, 0, request->length);
        free(request);
    }
}

/* Runs only on the upstream pserial/RPC RX task. Initialize lazily here, not
 * in the startup constructor. In particular, NEVER send CustomRpc data here:
 * upstream enqueues it with portMAX_DELAY into pserial_task's OWN req_queue.
 * Sending a response from that consumer deadlocks if events filled its queue.
 * Native operations and responses therefore run on request_task instead.
 */
static void slave_req_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *context)
{
    (void)context;
    esp_now_hosted_req_t req;
    if (msg_id != ESP_NOW_HOSTED_MSG_REQ ||
        !esp_now_hosted_decode_request(data, len, &req)) return;
    if (!s_requests) {
        s_requests = xQueueCreate(4, sizeof(slave_request_t *));
        if (!s_requests) {
            ESP_LOGE(TAG, "request queue allocation failed; host will time out");
            return;
        }
        if (xTaskCreate(request_task, "now_requests", 6144, NULL, 5, NULL) != pdPASS) {
            vQueueDelete(s_requests);
            s_requests = NULL;
            ESP_LOGE(TAG, "request worker allocation failed; host will time out");
            return;
        }
    }
    slave_request_t *request = malloc(sizeof(*request) + len);
    if (request) {
        request->length = len;
        memcpy(request->bytes, data, len);
        if (xQueueSend(s_requests, &request, 0) == pdTRUE) return;
        memset(request->bytes, 0, len);
        free(request);
    }
    /* A saturated bridge must not block the pserial consumer even to send an
     * error. A missing native response becomes ESP_ERR_TIMEOUT on the host;
     * it can never be mistaken for the Hosted transport acknowledgement. */
    ESP_LOGW(TAG, "request dropped (queue/memory full); host will time out");
}

esp_err_t esp_now_hosted_slave_init(void)
{
    /* The host's c6update scans a candidate image for this marker before it
     * sends anything. `used` keeps the compiler from dropping the array; this
     * reference from a linker-rooted function keeps section garbage
     * collection from dropping it too. */
    __asm__ __volatile__("" : : "r"(esp_now_hosted_bridge_marker) : "memory");
    esp_err_t err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_REQ, slave_req_cb, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "CustomRpc handler registration failed: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "HardwareOne ESP-NOW bridge registered; awaiting host init");
    return err;
}

#ifndef ESP_NOW_HOSTED_SLAVE_AUTOSTART
#define ESP_NOW_HOSTED_SLAVE_AUTOSTART 1
#endif
#if ESP_NOW_HOSTED_SLAVE_AUTOSTART
/* Same registration strategy as the ESPHome overlay. Force-link the exported
 * init symbol; this constructor only registers a handler, not a task/radio. */
static void __attribute__((constructor)) esp_now_hosted_autoreg(void)
{
    esp_now_hosted_slave_init();
}
#endif
