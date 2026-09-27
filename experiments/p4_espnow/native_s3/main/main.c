#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "milestone_wire.h"

static const char *TAG = "HW1_PROBE";
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};

typedef enum { EVENT_RX, EVENT_TX, EVENT_COMMAND } event_kind_t;
typedef struct {
    event_kind_t kind;
    uint8_t src[6];
    uint8_t dst[6];
    int8_t rssi;
    int status;
    uint16_t len;
    uint8_t data[HW1_MILESTONE_MAX_FRAME];
} probe_event_t;

static QueueHandle_t events;
static uint8_t own_mac[6];
static uint8_t channel = HW1_MILESTONE_DEFAULT_CHANNEL;
static bool silent;
static uint32_t discovery_id;
static uint32_t callback_drops;
static struct {
    uint32_t rx, invalid, requests, echoes, echo_sent, discoveries;
    uint32_t send_accepted, send_rejected, tx_ok, tx_fail;
    uint32_t peer_errors, restarts;
} counters;

/* Both callbacks only copy their borrowed inputs. Peer/radio operations and
 * logging happen in app_main, never inside a Wi-Fi callback. */
static void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !info->src_addr || !data || len < 1 || len > (int)HW1_MILESTONE_MAX_FRAME) return;
    probe_event_t event = {.kind = EVENT_RX, .len = len, .rssi = -128};
    memcpy(event.src, info->src_addr, 6);
    if (info->des_addr) memcpy(event.dst, info->des_addr, 6);
    if (info->rx_ctrl) event.rssi = info->rx_ctrl->rssi;
    memcpy(event.data, data, len);
    if (xQueueSend(events, &event, 0) != pdTRUE)
        __atomic_fetch_add(&callback_drops, 1, __ATOMIC_RELAXED);
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    probe_event_t event = {.kind = EVENT_TX, .status = status};
    if (info && info->des_addr) memcpy(event.dst, info->des_addr, 6);
    if (xQueueSend(events, &event, 0) != pdTRUE)
        __atomic_fetch_add(&callback_drops, 1, __ATOMIC_RELAXED);
}

static bool ensure_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, mac, 6);
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        counters.peer_errors++;
        ESP_LOGE(TAG, "PEER_ADD mac=" MACSTR " error=%s", MAC2STR(mac), esp_err_to_name(err));
        return false;
    }
    esp_now_peer_info_t readback;
    err = esp_now_get_peer(mac, &readback);
    ESP_LOGI(TAG, "PEER mac=" MACSTR " get=%s channel=%u encrypted=%u",
             MAC2STR(mac), esp_err_to_name(err),
             err == ESP_OK ? readback.channel : 255,
             err == ESP_OK ? (unsigned)readback.encrypt : 255);
    return err == ESP_OK;
}

static void radio_start(void)
{
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, own_mac));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_receive));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
    ESP_ERROR_CHECK(ensure_peer(broadcast) ? ESP_OK : ESP_FAIL);
    uint32_t version = 0;
    ESP_ERROR_CHECK(esp_now_get_version(&version));
    ESP_LOGI(TAG, "READY role=native_s3 mac=" MACSTR " channel=%u espnow_version=%" PRIu32
                 " wire_version=4 mesh_fingerprint=%04x silence=%u",
             MAC2STR(own_mac), channel, version, hw1_milestone_fingerprint(), silent);
}

static void print_stats(void)
{
    esp_now_peer_num_t peers = {0};
    esp_err_t peer_err = esp_now_get_peer_num(&peers);
    uint8_t actual_channel = 0;
    wifi_second_chan_t secondary;
    esp_err_t channel_err = esp_wifi_get_channel(&actual_channel, &secondary);
    ESP_LOGI(TAG, "STATS rx=%" PRIu32 " invalid=%" PRIu32 " requests=%" PRIu32
                 " echo_rx=%" PRIu32 " echo_sent=%" PRIu32 " discoveries=%" PRIu32 " accepted=%" PRIu32
                 " rejected=%" PRIu32 " tx_ok=%" PRIu32 " tx_fail=%" PRIu32
                 " callback_drops=%" PRIu32 " peer_errors=%" PRIu32 " restarts=%" PRIu32
                 " peers=%d peers_status=%s channel=%u channel_status=%s silence=%u",
             counters.rx, counters.invalid, counters.requests, counters.echoes, counters.echo_sent, counters.discoveries,
             counters.send_accepted, counters.send_rejected, counters.tx_ok, counters.tx_fail,
             __atomic_load_n(&callback_drops, __ATOMIC_RELAXED), counters.peer_errors, counters.restarts,
             peers.total_num, esp_err_to_name(peer_err), actual_channel,
             esp_err_to_name(channel_err), silent);
}

static bool send_frame(const uint8_t dst[6], uint8_t *frame, size_t len)
{
    hw1_milestone_header_t h;
    memcpy(&h, frame, sizeof(h));
    esp_err_t err = esp_now_send(dst, frame, len);
    if (err == ESP_OK) counters.send_accepted++;
    else counters.send_rejected++;
    ESP_LOGI(TAG, "SEND type=%u id=%" PRIu32 " len=%u dst=" MACSTR " result=%s",
             h.type, h.msgId, (unsigned)len, MAC2STR(dst), esp_err_to_name(err));
    return err == ESP_OK;
}

static void send_discovery(void)
{
    uint8_t frame[64];
    hw1_milestone_header_t h;
    uint32_t id = ++discovery_id;
    for (size_t i = sizeof(h); i < sizeof(frame); ++i)
        frame[i] = hw1_milestone_payload_byte(id, i - sizeof(h));
    hw1_milestone_init_header(&h, HW1_MILESTONE_DISCOVERY, id, own_mac,
                              frame + sizeof(h), sizeof(frame) - sizeof(h));
    memcpy(frame, &h, sizeof(h));
    send_frame(broadcast, frame, sizeof(frame));
}

static void receive_frame(const probe_event_t *event)
{
    counters.rx++;
    if (!hw1_milestone_valid_pattern(event->data, event->len)) {
        counters.invalid++;
        ESP_LOGW(TAG, "RX_INVALID src=" MACSTR " len=%u rssi=%d",
                 MAC2STR(event->src), event->len, event->rssi);
        return;
    }
    hw1_milestone_header_t h;
    memcpy(&h, event->data, sizeof(h));
    ESP_LOGI(TAG, "RX type=%u id=%" PRIu32 " len=%u src=" MACSTR " dst=" MACSTR
                 " rssi=%d pattern=ok",
             h.type, h.msgId, event->len, MAC2STR(event->src), MAC2STR(event->dst), event->rssi);
    if (h.type == HW1_MILESTONE_DISCOVERY) {
        counters.discoveries++;
        if (!silent) ensure_peer(event->src);
        return;
    }
    if (h.type == HW1_MILESTONE_ECHO) {
        counters.echoes++;
        return;
    }
    if (h.type != HW1_MILESTONE_REQUEST || !hw1_milestone_probe_size(event->len)) {
        counters.invalid++;
        return;
    }
    counters.requests++;
    if (silent || !ensure_peer(event->src)) return;
    uint8_t reply[HW1_MILESTONE_MAX_FRAME];
    memcpy(reply, event->data, event->len);
    hw1_milestone_init_header(&h, HW1_MILESTONE_ECHO, h.msgId, own_mac,
                              reply + sizeof(h), event->len - sizeof(h));
    memcpy(reply, &h, sizeof(h));
    if (send_frame(event->src, reply, event->len)) counters.echo_sent++;
}

static void handle_command(const char *line)
{
    unsigned value;
    char extra;
    if (!strcmp(line, "stats")) {
        print_stats();
    } else if (sscanf(line, "channel %u %c", &value, &extra) == 1 && value >= 1 && value <= 11) {
        esp_err_t err = esp_wifi_set_channel(value, WIFI_SECOND_CHAN_NONE);
        if (err == ESP_OK) channel = value;
        ESP_LOGI(TAG, "CHANNEL requested=%u result=%s", value, esp_err_to_name(err));
        print_stats();
    } else if (sscanf(line, "silence %u %c", &value, &extra) == 1 && value <= 1) {
        silent = value != 0;
        ESP_LOGI(TAG, "SILENCE enabled=%u", silent);
    } else if (!strcmp(line, "restart_radio")) {
        ESP_ERROR_CHECK(esp_now_unregister_recv_cb());
        ESP_ERROR_CHECK(esp_now_unregister_send_cb());
        ESP_ERROR_CHECK(esp_now_deinit());
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_deinit());
        counters.restarts++;
        radio_start();
    } else {
        ESP_LOGW(TAG, "COMMANDS stats | channel <1..11> | silence <0|1> | restart_radio");
    }
}

static void console_task(void *unused)
{
    (void)unused;
    char line[80];
    size_t used = 0;
    bool discard = false;
    setvbuf(stdin, NULL, _IONBF, 0);
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (used && !discard) {
                probe_event_t event = {.kind = EVENT_COMMAND};
                line[used] = 0;
                memcpy(event.data, line, used + 1);
                xQueueSend(events, &event, portMAX_DELAY);
            }
            used = 0;
            discard = false;
        } else if (!discard) {
            if (used + 1 < sizeof(line)) line[used++] = ch;
            else discard = true;
        }
    }
}

void app_main(void)
{
    /* No esp_wifi_connect, SSID, password, provisioning, or NVS writes. */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    events = xQueueCreate(32, sizeof(probe_event_t));
    ESP_ERROR_CHECK(events ? ESP_OK : ESP_ERR_NO_MEM);
    radio_start();
    ESP_ERROR_CHECK(xTaskCreate(console_task, "probe_console", 3072, NULL, 3, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    int64_t next_discovery = 0;
    for (;;) {
        probe_event_t event;
        if (xQueueReceive(events, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (event.kind == EVENT_RX) receive_frame(&event);
            else if (event.kind == EVENT_COMMAND) handle_command((char *)event.data);
            else {
                if (event.status == ESP_NOW_SEND_SUCCESS) counters.tx_ok++;
                else counters.tx_fail++;
                ESP_LOGI(TAG, "TX_CALLBACK dst=" MACSTR " status=%s", MAC2STR(event.dst),
                         event.status == ESP_NOW_SEND_SUCCESS ? "ok" : "fail");
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_discovery) {
            if (!silent) send_discovery();
            next_discovery = now + 2000000;
        }
    }
}
