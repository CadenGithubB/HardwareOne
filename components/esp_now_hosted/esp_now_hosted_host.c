/* SPDX-License-Identifier: Apache-2.0
 * HardwareOne plain-IDF host implementation of the ESPHome CustomRpc bridge
 * pattern. Native application callbacks NEVER run in the RPC receive thread.
 * See README.md for provenance, threading, timeout and metadata limitations.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_hosted_misc.h"
#include "esp_idf_version.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_now_hosted_host.h"
#include "esp_now_hosted_rpc.h"

#if !CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
#error "ESP-NOW bridge requires CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y"
#endif

typedef struct {
    uint32_t msg_id;
    uint32_t callback_generation;
    size_t length;
    uint8_t bytes[];
} host_event_t;

typedef struct {
    bool used;
    uint8_t mac[ESP_NOW_ETH_ALEN];
    void *priv; /* host-only; never serialized */
} peer_private_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_start_state; /* 0 not started, 1 starting, 2 ready, -1 failed */
static esp_err_t s_start_result = ESP_FAIL;
static SemaphoreHandle_t s_request_lock;
static SemaphoreHandle_t s_response_ready;
static QueueHandle_t s_events;
static esp_now_recv_cb_t s_recv_cb;
static esp_now_send_cb_t s_send_cb;
static uint32_t s_recv_generation;
static uint32_t s_send_generation;
static uint64_t s_epoch;
static uint64_t s_next_seq;
static bool s_initialized;
static esp_now_hosted_host_stats_t s_stats;
static esp_now_hosted_bridge_state_t s_bridge_state = ESP_NOW_HOSTED_BRIDGE_UNKNOWN;
static bool s_last_exchange_answered; /* under s_state_lock; set by exchange_locked */
static bool s_offline;                /* under s_state_lock; see esp_now_hosted_host_set_offline */
/* Host mirror of the C6 peer table: an entry exists exactly when the last
 * successful add/mod/del left that MAC in the companion's table. Writers hold
 * s_request_lock; every access additionally runs under s_state_lock so the
 * lock-free hot-path reader esp_now_is_peer_exist() never blocks on an RPC. */
static peer_private_t s_peer_private[ESP_NOW_MAX_TOTAL_PEER_NUM];
static struct {
    bool active;
    bool complete;
    uint8_t opcode;
    uint64_t seq;
    int32_t status;
    uint16_t length;
    uint8_t bytes[ESP_NOW_HOSTED_MAX_RETURN];
} s_pending;

static void count_malformed(void)
{
    portENTER_CRITICAL(&s_state_lock);
    ++s_stats.malformed_messages;
    portEXIT_CRITICAL(&s_state_lock);
}

static void response_callback(uint32_t msg_id, const uint8_t *data, size_t len,
                              void *context)
{
    (void)context;
    esp_now_hosted_resp_t response;
    if (msg_id != ESP_NOW_HOSTED_MSG_RESP ||
        !esp_now_hosted_decode_response(data, len, &response)) {
        count_malformed();
        return;
    }
    bool accepted = false;
    portENTER_CRITICAL(&s_state_lock);
    if (s_pending.active && !s_pending.complete &&
        s_pending.seq == response.request.seq &&
        s_pending.opcode == response.request.opcode) {
        s_pending.status = response.status;
        s_pending.length = response.request.payload_len;
        memcpy(s_pending.bytes, data + sizeof(response), s_pending.length);
        s_pending.complete = true;
        accepted = true;
    } else {
        ++s_stats.stale_responses;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (accepted) xSemaphoreGive(s_response_ready);
}

static void event_callback(uint32_t msg_id, const uint8_t *data, size_t len,
                           void *context)
{
    (void)context;
    esp_now_hosted_event_header_t header;
    if (!esp_now_hosted_decode_event(data, len, &header)) {
        count_malformed();
        return;
    }
    if (msg_id == ESP_NOW_HOSTED_MSG_RECV) {
        esp_now_hosted_recv_evt_t event;
        if (len < sizeof(event)) { count_malformed(); return; }
        memcpy(&event, data, sizeof(event));
        if (event.data_len > ESP_NOW_HOSTED_MAX_FRAME ||
            len != sizeof(event) + event.data_len || event.has_rx_ctrl > 1) {
            count_malformed();
            return;
        }
    } else if (msg_id == ESP_NOW_HOSTED_MSG_SEND) {
        esp_now_hosted_send_evt_t event;
        if (len != sizeof(event)) { count_malformed(); return; }
        memcpy(&event, data, sizeof(event));
        if (event.has_tx_info > 1 ||
            (event.status != ESP_NOW_SEND_SUCCESS && event.status != ESP_NOW_SEND_FAIL)) {
            count_malformed();
            return;
        }
    } else {
        count_malformed();
        return;
    }

    uint32_t generation;
    bool wanted;
    portENTER_CRITICAL(&s_state_lock);
    wanted = s_initialized && s_epoch == header.epoch &&
             (msg_id == ESP_NOW_HOSTED_MSG_RECV ? s_recv_cb != NULL : s_send_cb != NULL);
    generation = msg_id == ESP_NOW_HOSTED_MSG_RECV ? s_recv_generation : s_send_generation;
    portEXIT_CRITICAL(&s_state_lock);
    if (!wanted) return;

    host_event_t *event = malloc(sizeof(*event) + len);
    if (event) {
        event->msg_id = msg_id;
        event->callback_generation = generation;
        event->length = len;
        memcpy(event->bytes, data, len);
        if (xQueueSend(s_events, &event, 0) == pdTRUE) return;
        free(event);
    }
    portENTER_CRITICAL(&s_state_lock);
    ++s_stats.events_dropped;
    portEXIT_CRITICAL(&s_state_lock);
}

static void callback_task(void *argument)
{
    (void)argument;
    for (;;) {
        host_event_t *event = NULL;
        if (xQueueReceive(s_events, &event, portMAX_DELAY) != pdTRUE) continue;
        esp_now_hosted_event_header_t header;
        memcpy(&header, event->bytes, sizeof(header));
        esp_now_recv_cb_t recv_cb = NULL;
        esp_now_send_cb_t send_cb = NULL;
        portENTER_CRITICAL(&s_state_lock);
        if (s_initialized && s_epoch == header.epoch) {
            if (event->msg_id == ESP_NOW_HOSTED_MSG_RECV &&
                event->callback_generation == s_recv_generation) recv_cb = s_recv_cb;
            if (event->msg_id == ESP_NOW_HOSTED_MSG_SEND &&
                event->callback_generation == s_send_generation) send_cb = s_send_cb;
        }
        portEXIT_CRITICAL(&s_state_lock);

        /* No bridge lock is held while executing user code. Calling a
         * synchronous ESP-NOW API from either callback is supported. */
        if (recv_cb) {
            esp_now_hosted_recv_evt_t wire;
            memcpy(&wire, event->bytes, sizeof(wire));
            wifi_pkt_rx_ctrl_t rx = {0};
            rx.rssi = wire.rssi;
            rx.channel = wire.channel;
            rx.timestamp = wire.timestamp;
            esp_now_recv_info_t info = {
                .src_addr = wire.src_addr,
                .des_addr = wire.des_addr,
                .rx_ctrl = wire.has_rx_ctrl ? &rx : NULL,
            };
            recv_cb(&info, event->bytes + sizeof(wire), wire.data_len);
        } else if (send_cb) {
            esp_now_hosted_send_evt_t wire;
            memcpy(&wire, event->bytes, sizeof(wire));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
            esp_now_send_info_t info = {0};
            info.des_addr = wire.des_addr;
            info.src_addr = wire.has_tx_info ? wire.src_addr : NULL;
            info.ifidx = (wifi_interface_t)wire.ifidx;
            info.rate = (wifi_phy_rate_t)wire.rate;
            info.tx_status = (wifi_tx_status_t)wire.tx_status;
            /* Native raw 802.11 data is deliberately not transported. */
            send_cb(&info, (esp_now_send_status_t)wire.status);
#else
            send_cb(wire.des_addr, (esp_now_send_status_t)wire.status);
#endif
        }
        free(event);
    }
}

esp_err_t esp_now_hosted_host_start(void)
{
    bool owner = false;
    portENTER_CRITICAL(&s_state_lock);
    if (s_start_state == 0) {
        s_start_state = 1;
        owner = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!owner) {
        TickType_t start = xTaskGetTickCount();
        for (;;) {
            portENTER_CRITICAL(&s_state_lock);
            int state = s_start_state;
            esp_err_t result = s_start_result;
            portEXIT_CRITICAL(&s_state_lock);
            if (state == 2 || state == -1) return result;
            if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(ESP_NOW_HOSTED_TIMEOUT_MS))
                return ESP_ERR_TIMEOUT;
            vTaskDelay(1);
        }
    }

    s_request_lock = xSemaphoreCreateMutex();
    s_response_ready = xSemaphoreCreateBinary();
    s_events = xQueueCreate(ESP_NOW_HOSTED_EVENT_QUEUE_DEPTH, sizeof(host_event_t *));
    esp_err_t err = ESP_ERR_NO_MEM;
    if (s_request_lock && s_response_ready && s_events) {
        /* Random high bits reduce cross-host-boot collisions; subsequent
         * requests monotonically increment and never reset on deinit/init. */
        s_next_seq = ((uint64_t)esp_random() << 32) | UINT64_C(1);
        err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_RESP, response_callback, NULL);
        if (err == ESP_OK)
            err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_RECV, event_callback, NULL);
        if (err == ESP_OK)
            err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_SEND, event_callback, NULL);
        if (err == ESP_OK && xTaskCreate(callback_task, "now_callbacks", 4096, NULL,
                                         5, NULL) != pdPASS) err = ESP_ERR_NO_MEM;
    }
    /* Retain resources, even on terminal setup failure: the upstream
     * deregistration API cannot fence a callback already in flight. */
    portENTER_CRITICAL(&s_state_lock);
    s_start_result = err;
    s_start_state = err == ESP_OK ? 2 : -1;
    portEXIT_CRITICAL(&s_state_lock);
    return err;
}

static void reset_local_state_locked(void)
{
    s_initialized = false;
    s_epoch = 0;
    s_recv_cb = NULL;
    s_send_cb = NULL;
    ++s_recv_generation;
    ++s_send_generation;
    memset(s_peer_private, 0, sizeof(s_peer_private));
}

void esp_now_hosted_host_reset_local(void)
{
    portENTER_CRITICAL(&s_state_lock);
    reset_local_state_locked();
    portEXIT_CRITICAL(&s_state_lock);
}

void esp_now_hosted_host_set_offline(bool offline)
{
    portENTER_CRITICAL(&s_state_lock);
    s_offline = offline;
    portEXIT_CRITICAL(&s_state_lock);
}

bool esp_now_hosted_host_is_offline(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool offline = s_offline;
    portEXIT_CRITICAL(&s_state_lock);
    return offline;
}

static esp_err_t lock_bridge(void)
{
    if (esp_now_hosted_host_is_offline()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_now_hosted_host_start();
    if (err != ESP_OK) return err;
    return xSemaphoreTake(s_request_lock, pdMS_TO_TICKS(ESP_NOW_HOSTED_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* s_request_lock covers the COMPLETE remote exchange and local bookkeeping. */
static esp_err_t exchange_locked(uint8_t opcode, const void *payload, size_t length,
                                 void *result, size_t result_size, uint64_t *sent_seq)
{
    if (length > ESP_NOW_HOSTED_MAX_PAYLOAD || (length && !payload) ||
        result_size > ESP_NOW_HOSTED_MAX_RETURN || (result_size && !result))
        return ESP_ERR_INVALID_ARG;
    if (s_next_seq == UINT64_MAX) return ESP_ERR_INVALID_STATE; /* never reuse */
    uint64_t seq = ++s_next_seq;
    if (sent_seq) *sent_seq = seq;
    uint8_t *bytes = malloc(sizeof(esp_now_hosted_req_t) + length);
    if (!bytes) return ESP_ERR_NO_MEM;
    esp_now_hosted_req_t header = {
        .magic = ESP_NOW_HOSTED_MAGIC, .version = ESP_NOW_HOSTED_WIRE_VERSION,
        .opcode = opcode, .payload_len = (uint16_t)length, .seq = seq,
    };
    memcpy(bytes, &header, sizeof(header));
    if (length) memcpy(bytes + sizeof(header), payload, length);
    while (xSemaphoreTake(s_response_ready, 0) == pdTRUE) {}
    portENTER_CRITICAL(&s_state_lock);
    s_pending.active = true;
    s_pending.complete = false;
    s_pending.opcode = opcode;
    s_pending.seq = seq;
    s_last_exchange_answered = false;
    portEXIT_CRITICAL(&s_state_lock);

    /* This API itself waits for ESP-Hosted's transport acknowledgement. That
     * acknowledgement is NOT the native ESP-NOW result; await our reply too. */
    TickType_t started = xTaskGetTickCount();
    esp_err_t err = esp_hosted_send_custom_data(ESP_NOW_HOSTED_MSG_REQ, bytes,
                                                sizeof(header) + length);
    memset(bytes, 0, sizeof(header) + length); /* PMK/LMK temporary copies */
    free(bytes);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_pending.active = false;
        ++s_stats.transport_errors;
        portEXIT_CRITICAL(&s_state_lock);
        return err;
    }

    for (;;) {
        portENTER_CRITICAL(&s_state_lock);
        if (s_pending.complete) {
            s_bridge_state = ESP_NOW_HOSTED_BRIDGE_PRESENT; /* any reply proves the bridge */
            s_last_exchange_answered = true;
            err = (esp_err_t)s_pending.status;
            if (err == ESP_OK) {
                if (s_pending.length != result_size) err = ESP_ERR_INVALID_SIZE;
                else if (result_size) memcpy(result, s_pending.bytes, result_size);
            }
            s_pending.active = false;
            memset(s_pending.bytes, 0, sizeof(s_pending.bytes));
            portEXIT_CRITICAL(&s_state_lock);
            return err;
        }
        TickType_t elapsed = xTaskGetTickCount() - started;
        TickType_t budget = pdMS_TO_TICKS(ESP_NOW_HOSTED_TIMEOUT_MS);
        if (elapsed >= budget) {
            s_pending.active = false;
            ++s_stats.request_timeouts;
            portEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_TIMEOUT;
        }
        portEXIT_CRITICAL(&s_state_lock);
        /* A delayed give from an old response can wake this request. Always
         * re-check the matched pending state instead of treating a wake as OK. */
        xSemaphoreTake(s_response_ready, budget - elapsed);
    }
}

static esp_err_t request(uint8_t opcode, const void *payload, size_t length,
                         void *result, size_t result_size)
{
    esp_err_t err = lock_bridge();
    if (err != ESP_OK) return err;
    err = exchange_locked(opcode, payload, length, result, result_size, NULL);
    xSemaphoreGive(s_request_lock);
    return err;
}

void esp_now_hosted_host_get_stats(esp_now_hosted_host_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_state_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t esp_now_init(void)
{
    esp_err_t err = lock_bridge();
    if (err != ESP_OK) return err;
    uint64_t epoch = 0;
    err = exchange_locked(ESP_NOW_HOSTED_OP_INIT, NULL, 0, NULL, 0, &epoch);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_epoch = epoch;
        s_initialized = true;
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_request_lock);
    return err;
}

esp_err_t esp_now_deinit(void)
{
    if (esp_now_hosted_host_is_offline()) {
        /* Nothing to tell a companion that is not answering; its instance is
         * gone with it. Release the host side so the application can close. */
        esp_now_hosted_host_reset_local();
        return ESP_OK;
    }
    esp_err_t err = lock_bridge();
    if (err != ESP_OK) return err;
    err = exchange_locked(ESP_NOW_HOSTED_OP_DEINIT, NULL, 0, NULL, 0, NULL);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        reset_local_state_locked();
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_request_lock);
    return err;
}

static esp_err_t set_recv_callback(esp_now_recv_cb_t cb, bool registering)
{
    if (!registering && esp_now_hosted_host_is_offline()) {
        portENTER_CRITICAL(&s_state_lock);
        s_recv_cb = NULL;
        ++s_recv_generation;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    esp_err_t err = lock_bridge();
    if (err != ESP_OK) return err;
    uint8_t opcode = registering ? ESP_NOW_HOSTED_OP_REGISTER_RECV : ESP_NOW_HOSTED_OP_UNREGISTER_RECV;
    err = exchange_locked(opcode, NULL, 0, NULL, 0, NULL);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_recv_cb = cb;
        ++s_recv_generation;
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_request_lock);
    return err;
}

static esp_err_t set_send_callback(esp_now_send_cb_t cb, bool registering)
{
    if (!registering && esp_now_hosted_host_is_offline()) {
        portENTER_CRITICAL(&s_state_lock);
        s_send_cb = NULL;
        ++s_send_generation;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    esp_err_t err = lock_bridge();
    if (err != ESP_OK) return err;
    uint8_t opcode = registering ? ESP_NOW_HOSTED_OP_REGISTER_SEND : ESP_NOW_HOSTED_OP_UNREGISTER_SEND;
    err = exchange_locked(opcode, NULL, 0, NULL, 0, NULL);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_send_cb = cb;
        ++s_send_generation;
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_request_lock);
    return err;
}

esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb)
{ return cb ? set_recv_callback(cb, true) : ESP_ERR_ESPNOW_ARG; }
esp_err_t esp_now_unregister_recv_cb(void)
{ return set_recv_callback(NULL, false); }
esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb)
{ return cb ? set_send_callback(cb, true) : ESP_ERR_ESPNOW_ARG; }
esp_err_t esp_now_unregister_send_cb(void)
{ return set_send_callback(NULL, false); }

/* Caller holds s_state_lock. */
static peer_private_t *private_entry(const uint8_t *mac, bool create)
{
    peer_private_t *empty = NULL;
    for (size_t i = 0; i < ESP_NOW_MAX_TOTAL_PEER_NUM; ++i) {
        if (s_peer_private[i].used && memcmp(s_peer_private[i].mac, mac, 6) == 0)
            return &s_peer_private[i];
        if (!s_peer_private[i].used && !empty) empty = &s_peer_private[i];
    }
    if (create && empty) {
        empty->used = true;
        memcpy(empty->mac, mac, 6);
        return empty;
    }
    return NULL;
}

static void private_set(const uint8_t *mac, void *priv)
{
    portENTER_CRITICAL(&s_state_lock);
    peer_private_t *entry = private_entry(mac, true);
    if (entry) entry->priv = priv;
    portEXIT_CRITICAL(&s_state_lock);
}

static void private_clear(const uint8_t *mac)
{
    portENTER_CRITICAL(&s_state_lock);
    peer_private_t *entry = private_entry(mac, false);
    if (entry) memset(entry, 0, sizeof(*entry));
    portEXIT_CRITICAL(&s_state_lock);
}

static bool private_lookup(const uint8_t *mac, void **priv)
{
    portENTER_CRITICAL(&s_state_lock);
    peer_private_t *entry = private_entry(mac, false);
    if (entry && priv) *priv = entry->priv;
    portEXIT_CRITICAL(&s_state_lock);
    return entry != NULL;
}

static esp_err_t write_peer(uint8_t opcode, const esp_now_peer_info_t *peer)
{
    if (!peer) return ESP_ERR_ESPNOW_ARG;
    esp_now_hosted_peer_t wire = {0};
    memcpy(wire.peer_addr, peer->peer_addr, 6);
    memcpy(wire.lmk, peer->lmk, 16);
    wire.channel = peer->channel;
    wire.ifidx = (int32_t)peer->ifidx;
    wire.encrypt = peer->encrypt;
    esp_err_t err = lock_bridge();
    if (err == ESP_OK) {
        err = exchange_locked(opcode, &wire, sizeof(wire), NULL, 0, NULL);
        if (err == ESP_OK) private_set(peer->peer_addr, peer->priv);
        xSemaphoreGive(s_request_lock);
    }
    memset(&wire, 0, sizeof(wire));
    return err;
}

esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer)
{ return write_peer(ESP_NOW_HOSTED_OP_ADD_PEER, peer); }
esp_err_t esp_now_mod_peer(const esp_now_peer_info_t *peer)
{ return write_peer(ESP_NOW_HOSTED_OP_MOD_PEER, peer); }

esp_err_t esp_now_del_peer(const uint8_t *mac)
{
    if (!mac) return ESP_ERR_ESPNOW_ARG;
    if (esp_now_hosted_host_is_offline()) {
        private_clear(mac);
        return ESP_OK;
    }
    esp_err_t err = lock_bridge();
    if (err != ESP_OK) return err;
    err = exchange_locked(ESP_NOW_HOSTED_OP_DEL_PEER, mac, 6, NULL, 0, NULL);
    if (err == ESP_OK) private_clear(mac);
    xSemaphoreGive(s_request_lock);
    return err;
}

esp_err_t esp_now_get_peer(const uint8_t *mac, esp_now_peer_info_t *peer)
{
    if (!mac || !peer) return ESP_ERR_ESPNOW_ARG;
    esp_err_t err = lock_bridge();
    if (err != ESP_OK) return err;
    esp_now_hosted_peer_t wire = {0};
    err = exchange_locked(ESP_NOW_HOSTED_OP_GET_PEER, mac, 6, &wire, sizeof(wire), NULL);
    if (err == ESP_OK) {
        if (memcmp(wire.peer_addr, mac, 6) || wire.encrypt > 1) err = ESP_ERR_INVALID_RESPONSE;
        else {
            memset(peer, 0, sizeof(*peer));
            memcpy(peer->peer_addr, wire.peer_addr, 6);
            memcpy(peer->lmk, wire.lmk, 16);
            peer->channel = wire.channel;
            peer->ifidx = (wifi_interface_t)wire.ifidx;
            peer->encrypt = wire.encrypt != 0;
            void *priv = NULL;
            private_lookup(mac, &priv);
            peer->priv = priv;
        }
    }
    memset(&wire, 0, sizeof(wire));
    xSemaphoreGive(s_request_lock);
    return err;
}

esp_err_t esp_now_get_peer_num(esp_now_peer_num_t *num)
{
    if (!num) return ESP_ERR_ESPNOW_ARG;
    esp_now_hosted_peer_num_t wire = {0};
    esp_err_t err = request(ESP_NOW_HOSTED_OP_GET_PEER_NUM, NULL, 0, &wire, sizeof(wire));
    if (err == ESP_OK) {
        if (wire.total_num < 0 || wire.total_num > ESP_NOW_MAX_TOTAL_PEER_NUM ||
            wire.encrypt_num < 0 || wire.encrypt_num > wire.total_num)
            return ESP_ERR_INVALID_RESPONSE;
        num->total_num = wire.total_num;
        num->encrypt_num = wire.encrypt_num;
    }
    return err;
}

esp_err_t esp_now_hosted_is_peer_exist(const uint8_t *mac, bool *exists)
{
    if (!mac || !exists) return ESP_ERR_ESPNOW_ARG;
    uint8_t wire = 0;
    esp_err_t err = request(ESP_NOW_HOSTED_OP_IS_PEER_EXIST, mac, 6, &wire, sizeof(wire));
    if (err == ESP_OK) {
        if (wire > 1) return ESP_ERR_INVALID_RESPONSE;
        *exists = wire != 0;
    }
    return err;
}

/* The native driver answers this from a local table in microseconds and the
 * shared mesh code calls it for every received frame and routing decision. A
 * C6 round trip here (~15 ms over SDIO) would dominate RX, so answer from the
 * host mirror. The mirror only ever changes with a SUCCESSFUL add/mod/del or
 * deinit on the companion, which keeps it equal to the C6 table unless the
 * companion resets underneath us, a case nothing recovers from yet. Callers
 * that need the companion's own answer use esp_now_hosted_is_peer_exist().
 */
bool esp_now_is_peer_exist(const uint8_t *mac)
{
    if (!mac) return false;
    portENTER_CRITICAL(&s_state_lock);
    const bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_state_lock);
    if (!initialized) return false;
    return private_lookup(mac, NULL);
}

esp_err_t esp_now_hosted_host_probe(esp_now_hosted_info_t *info)
{
    esp_now_hosted_info_t wire;
    memset(&wire, 0, sizeof(wire));
    esp_err_t err = lock_bridge();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        if (s_bridge_state != ESP_NOW_HOSTED_BRIDGE_PRESENT &&
            s_bridge_state != ESP_NOW_HOSTED_BRIDGE_PRESENT_NO_INFO)
            s_bridge_state = ESP_NOW_HOSTED_BRIDGE_TRANSPORT_ERROR;
        portEXIT_CRITICAL(&s_state_lock);
        return err;
    }
    err = exchange_locked(ESP_NOW_HOSTED_OP_GET_INFO, NULL, 0, &wire, sizeof(wire), NULL);
    portENTER_CRITICAL(&s_state_lock);
    const bool answered = s_last_exchange_answered;
    if (answered) {
        s_bridge_state = err == ESP_ERR_NOT_SUPPORTED ? ESP_NOW_HOSTED_BRIDGE_PRESENT_NO_INFO
                                                       : ESP_NOW_HOSTED_BRIDGE_PRESENT;
    } else {
        s_bridge_state = err == ESP_ERR_TIMEOUT ? ESP_NOW_HOSTED_BRIDGE_ABSENT
                                                : ESP_NOW_HOSTED_BRIDGE_TRANSPORT_ERROR;
    }
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_request_lock);
    if (answered) {
        if (err == ESP_OK && info) *info = wire;
        return ESP_OK;
    }
    return err;
}

esp_err_t esp_now_hosted_bridge_info(esp_now_hosted_info_t *info)
{
    if (!info) return ESP_ERR_ESPNOW_ARG;
    esp_now_hosted_info_t wire;
    memset(&wire, 0, sizeof(wire));
    esp_err_t err = request(ESP_NOW_HOSTED_OP_GET_INFO, NULL, 0, &wire, sizeof(wire));
    if (err == ESP_OK) *info = wire;
    return err;
}

esp_err_t esp_now_hosted_confirm_image(void)
{
    return request(ESP_NOW_HOSTED_OP_CONFIRM_IMAGE, NULL, 0, NULL, 0);
}

esp_now_hosted_bridge_state_t esp_now_hosted_bridge_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const esp_now_hosted_bridge_state_t state = s_bridge_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

esp_err_t esp_now_get_version(uint32_t *version)
{
    if (!version) return ESP_ERR_ESPNOW_ARG;
    return request(ESP_NOW_HOSTED_OP_GET_VERSION, NULL, 0, version, sizeof(*version));
}

esp_err_t esp_now_set_pmk(const uint8_t *pmk)
{
    if (!pmk) return ESP_ERR_ESPNOW_ARG;
    return request(ESP_NOW_HOSTED_OP_SET_PMK, pmk, ESP_NOW_KEY_LEN, NULL, 0);
}

esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *data, size_t len)
{
    if (!data || !len || len > ESP_NOW_HOSTED_MAX_FRAME) return ESP_ERR_ESPNOW_ARG;
    size_t length = sizeof(esp_now_hosted_send_req_t) + len;
    uint8_t *payload = malloc(length);
    if (!payload) return ESP_ERR_ESPNOW_NO_MEM;
    esp_now_hosted_send_req_t header = {.has_addr = mac != NULL, .data_len = (uint16_t)len};
    if (mac) memcpy(header.peer_addr, mac, 6);
    memcpy(payload, &header, sizeof(header));
    memcpy(payload + sizeof(header), data, len);
    esp_err_t err = request(ESP_NOW_HOSTED_OP_SEND, payload, length, NULL, 0);
    free(payload);
    return err;
}
