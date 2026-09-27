#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_now_hosted_host.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "milestone_wire.h"

static const char *TAG = "HW1_HOST";
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
#define PING_TIMEOUT_US 200000
#define PING_GAP_US 20000

typedef enum { EVENT_RX, EVENT_TX, EVENT_COMMAND } event_kind_t;
typedef struct {
    event_kind_t kind;
    uint8_t src[6];
    uint8_t dst[6];
    int8_t rssi;
    int status;
    int64_t received_us;
    uint16_t len;
    uint8_t data[HW1_MILESTONE_MAX_FRAME];
} probe_event_t;

static QueueHandle_t events;
static uint8_t own_mac[6], peer_mac[6];
static uint8_t channel = HW1_MILESTONE_DEFAULT_CHANNEL;
static bool peer_known;
static uint32_t discovery_id, message_id, callback_drops;
static struct {
    uint32_t rx, invalid, discoveries, requests, echoes, late_echoes;
    uint32_t send_accepted, send_rejected, tx_ok, tx_fail;
    uint32_t ping_timeouts, peer_errors, restarts, peer_test_failures;
} counters;
static struct {
    bool active, pending;
    uint8_t dst[6];
    unsigned count, size, attempted, replies, timeouts, rejected;
    uint32_t current_id;
    int64_t started_us, next_send_us, rtt_total_us, rtt_min_us, rtt_max_us;
} run;

/* Borrowed callback pointers never escape the callback. Radio and peer RPCs
 * run only in the main task, outside Hosted's RPC/callback worker threads. */
static void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !info->src_addr || !data || len < 1 || len > (int)HW1_MILESTONE_MAX_FRAME) return;
    probe_event_t event = {.kind = EVENT_RX, .len = len, .rssi = -128,
                           .received_us = esp_timer_get_time()};
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

static esp_now_peer_info_t make_peer(const uint8_t mac[6])
{
    esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, mac, 6);
    return peer;
}

static bool peer_matches(const esp_now_peer_info_t *peer, const uint8_t mac[6])
{
    return !memcmp(peer->peer_addr, mac, 6) && peer->channel == 0 &&
           peer->ifidx == WIFI_IF_STA && !peer->encrypt;
}

static bool ensure_peer(const uint8_t mac[6])
{
    esp_now_peer_info_t readback = {0};
    esp_err_t err = esp_now_get_peer(mac, &readback);
    if (err == ESP_ERR_ESPNOW_NOT_FOUND) {
        esp_now_peer_info_t peer = make_peer(mac);
        err = esp_now_add_peer(&peer);
        ESP_LOGI(TAG, "PEER_ADD mac=" MACSTR " result=%s", MAC2STR(mac), esp_err_to_name(err));
        if (err == ESP_OK) err = esp_now_get_peer(mac, &readback);
    }
    bool okay = err == ESP_OK && peer_matches(&readback, mac);
    if (!okay) {
        counters.peer_errors++;
        ESP_LOGE(TAG, "PEER_VERIFY mac=" MACSTR " result=%s match=%u",
                 MAC2STR(mac), esp_err_to_name(err), okay);
    }
    return okay;
}

static void prepare_board(void)
{
    /* Release C6's ROM download strap before Hosted pulses EN. GPIO35 is P4's
     * own BOOT strap as well as C6 RX; do not drive it low during host reset. */
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_33, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_33, 1));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_35, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_NUM_35, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_36, GPIO_MODE_INPUT));
}

static void radio_start(void)
{
    prepare_board();
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    esp_hosted_coprocessor_fwver_t version = {0};
    ESP_ERROR_CHECK(esp_hosted_get_coprocessor_fwversion(&version));
    ESP_LOGI(TAG, "COPROCESSOR version=%" PRIu32 ".%" PRIu32 ".%" PRIu32,
             version.major1, version.minor1, version.patch1);
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
    uint32_t espnow_version = 0;
    ESP_ERROR_CHECK(esp_now_get_version(&espnow_version));
    esp_now_peer_num_t peers = {0};
    ESP_ERROR_CHECK(esp_now_get_peer_num(&peers));
    ESP_LOGI(TAG, "READY role=hosted_p4 mac=" MACSTR " channel=%u espnow_version=%" PRIu32
                 " wire_version=4 mesh_fingerprint=%04x peers=%d",
             MAC2STR(own_mac), channel, espnow_version, hw1_milestone_fingerprint(), peers.total_num);
}

static void print_stats(void)
{
    esp_now_peer_num_t peers = {0};
    esp_err_t peer_err = esp_now_get_peer_num(&peers);
    uint8_t actual_channel = 0;
    wifi_second_chan_t secondary;
    esp_err_t channel_err = esp_wifi_get_channel(&actual_channel, &secondary);
    ESP_LOGI(TAG, "STATS rx=%" PRIu32 " invalid=%" PRIu32 " discoveries=%" PRIu32
                 " requests=%" PRIu32 " echoes=%" PRIu32 " late_echoes=%" PRIu32
                 " accepted=%" PRIu32 " rejected=%" PRIu32 " tx_ok=%" PRIu32 " tx_fail=%" PRIu32
                 " timeouts=%" PRIu32 " callback_drops=%" PRIu32 " peer_errors=%" PRIu32
                 " peer_test_failures=%" PRIu32 " restarts=%" PRIu32
                 " peers=%d peers_status=%s channel=%u channel_status=%s peer_known=%u ping_active=%u",
             counters.rx, counters.invalid, counters.discoveries, counters.requests, counters.echoes,
             counters.late_echoes, counters.send_accepted, counters.send_rejected, counters.tx_ok,
             counters.tx_fail, counters.ping_timeouts, __atomic_load_n(&callback_drops, __ATOMIC_RELAXED),
             counters.peer_errors, counters.peer_test_failures, counters.restarts, peers.total_num,
             esp_err_to_name(peer_err), actual_channel, esp_err_to_name(channel_err), peer_known, run.active);
    esp_now_hosted_host_stats_t bridge = {0};
    esp_now_hosted_host_get_stats(&bridge);
    ESP_LOGI(TAG, "BRIDGE_STATS malformed=%" PRIu32 " stale_responses=%" PRIu32
                 " event_drops=%" PRIu32 " request_timeouts=%" PRIu32 " transport_errors=%" PRIu32,
             bridge.malformed_messages, bridge.stale_responses, bridge.events_dropped,
             bridge.request_timeouts, bridge.transport_errors);
}

static bool send_frame(const uint8_t dst[6], const uint8_t *frame, size_t len)
{
    esp_err_t err = esp_now_send(dst, frame, len);
    if (err == ESP_OK) counters.send_accepted++;
    else {
        counters.send_rejected++;
        ESP_LOGW(TAG, "SEND_REJECT len=%u dst=" MACSTR " result=%s",
                 (unsigned)len, MAC2STR(dst), esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void make_frame(uint8_t *frame, size_t len, uint8_t type, uint32_t id)
{
    hw1_milestone_header_t h;
    for (size_t i = sizeof(h); i < len; ++i)
        frame[i] = hw1_milestone_payload_byte(id, i - sizeof(h));
    hw1_milestone_init_header(&h, type, id, own_mac, frame + sizeof(h), len - sizeof(h));
    memcpy(frame, &h, sizeof(h));
}

static void send_discovery(void)
{
    uint8_t frame[64];
    make_frame(frame, sizeof(frame), HW1_MILESTONE_DISCOVERY, ++discovery_id);
    send_frame(broadcast, frame, sizeof(frame));
}

static void receive_frame(const probe_event_t *event)
{
    counters.rx++;
    if (!hw1_milestone_valid_pattern(event->data, event->len)) {
        counters.invalid++;
        ESP_LOGW(TAG, "RX_INVALID src=" MACSTR " len=%u", MAC2STR(event->src), event->len);
        return;
    }
    hw1_milestone_header_t h;
    memcpy(&h, event->data, sizeof(h));
    if (memcmp(h.origin, event->src, 6) || !memcmp(event->src, own_mac, 6)) {
        counters.invalid++;
        return;
    }
    if (h.type == HW1_MILESTONE_DISCOVERY) {
        counters.discoveries++;
        if (!peer_known && ensure_peer(event->src)) {
            memcpy(peer_mac, event->src, 6);
            peer_known = true;
            ESP_LOGI(TAG, "DISCOVERED mac=" MACSTR " rssi=%d pattern=ok",
                     MAC2STR(peer_mac), event->rssi);
            print_stats();
        }
        return;
    }
    if (h.type == HW1_MILESTONE_REQUEST && hw1_milestone_probe_size(event->len)) {
        counters.requests++;
        if (ensure_peer(event->src)) {
            uint8_t frame[HW1_MILESTONE_MAX_FRAME];
            make_frame(frame, event->len, HW1_MILESTONE_ECHO, h.msgId);
            send_frame(event->src, frame, event->len);
        }
        return;
    }
    if (h.type != HW1_MILESTONE_ECHO) {
        counters.invalid++;
        return;
    }
    if (!run.active || !run.pending || h.msgId != run.current_id || event->len != run.size ||
        memcmp(event->src, run.dst, 6) || memcmp(event->dst, own_mac, 6)) {
        counters.late_echoes++;
        ESP_LOGW(TAG, "ECHO_UNMATCHED id=%" PRIu32 " len=%u src=" MACSTR,
                 h.msgId, event->len, MAC2STR(event->src));
        return;
    }
    int64_t rtt = event->received_us - run.started_us;
    if (rtt > PING_TIMEOUT_US) {
        counters.late_echoes++;
        ESP_LOGW(TAG, "ECHO_LATE id=%" PRIu32 " rtt_us=%" PRId64, h.msgId, rtt);
        return;
    }
    run.pending = false;
    run.replies++;
    counters.echoes++;
    run.rtt_total_us += rtt;
    if (run.replies == 1 || rtt < run.rtt_min_us) run.rtt_min_us = rtt;
    if (rtt > run.rtt_max_us) run.rtt_max_us = rtt;
    run.next_send_us = esp_timer_get_time() + PING_GAP_US;
    ESP_LOGI(TAG, "PING_REPLY id=%" PRIu32 " len=%u rtt_us=%" PRId64 " rssi=%d pattern=ok",
             h.msgId, event->len, rtt, event->rssi);
}

static void tick_ping(int64_t now)
{
    if (!run.active) return;
    if (run.pending && now - run.started_us >= PING_TIMEOUT_US) {
        run.pending = false;
        run.timeouts++;
        counters.ping_timeouts++;
        run.next_send_us = now + PING_GAP_US;
        ESP_LOGW(TAG, "PING_TIMEOUT id=%" PRIu32 " len=%u deadline_ms=200", run.current_id, run.size);
    }
    if (run.pending) return;
    if (run.attempted == run.count) {
        ESP_LOGI(TAG, "PING_DONE requested=%u size=%u attempted=%u replies=%u timeouts=%u rejected=%u"
                     " rtt_min_us=%" PRId64 " rtt_avg_us=%" PRId64 " rtt_max_us=%" PRId64,
                 run.count, run.size, run.attempted, run.replies, run.timeouts, run.rejected,
                 run.rtt_min_us, run.replies ? run.rtt_total_us / run.replies : 0, run.rtt_max_us);
        run.active = false;
        return;
    }
    if (now < run.next_send_us) return;
    uint8_t frame[HW1_MILESTONE_MAX_FRAME];
    run.current_id = ++message_id;
    make_frame(frame, run.size, HW1_MILESTONE_REQUEST, run.current_id);
    run.attempted++;
    run.started_us = esp_timer_get_time();
    run.pending = true;
    if (!send_frame(run.dst, frame, run.size)) {
        run.pending = false;
        run.rejected++;
        run.next_send_us = esp_timer_get_time() + PING_GAP_US;
    }
}

static void peer_check(const char *step, esp_err_t actual, esp_err_t expected, unsigned *failed)
{
    bool pass = actual == expected;
    if (!pass) (*failed)++;
    ESP_LOGI(TAG, "PEER_TEST step=%s actual=%s expected=%s pass=%u",
             step, esp_err_to_name(actual), esp_err_to_name(expected), pass);
}

static void peer_test(void)
{
    if (!peer_known || run.active) {
        ESP_LOGW(TAG, "PEER_TEST rejected: requires discovered peer and idle ping");
        return;
    }
    unsigned failed = 0;
    esp_now_peer_num_t before = {0}, after = {0};
    esp_now_peer_info_t peer = make_peer(peer_mac), readback = {0};
    peer_check("count_before", esp_now_get_peer_num(&before), ESP_OK, &failed);
    peer_check("get_real", esp_now_get_peer(peer_mac, &readback), ESP_OK, &failed);
    peer_check("real_fields", peer_matches(&readback, peer_mac) ? ESP_OK : ESP_FAIL, ESP_OK, &failed);
    peer_check("exists_real", esp_now_is_peer_exist(peer_mac) ? ESP_OK : ESP_FAIL, ESP_OK, &failed);
    peer_check("duplicate_real", esp_now_add_peer(&peer), ESP_ERR_ESPNOW_EXIST, &failed);
    peer_check("modify_real", esp_now_mod_peer(&peer), ESP_OK, &failed);
    peer_check("delete_real", esp_now_del_peer(peer_mac), ESP_OK, &failed);
    peer_check("get_missing", esp_now_get_peer(peer_mac, &readback), ESP_ERR_ESPNOW_NOT_FOUND, &failed);
    peer_check("delete_missing", esp_now_del_peer(peer_mac), ESP_ERR_ESPNOW_NOT_FOUND, &failed);
    peer_check("restore_real", esp_now_add_peer(&peer), ESP_OK, &failed);

    /* Fill only local table entries; never transmit to these synthetic MACs.
     * Retain the real S3 and broadcast entries and clean up only our additions. */
    uint8_t added[ESP_NOW_MAX_TOTAL_PEER_NUM][6];
    unsigned added_count = 0;
    esp_err_t terminal = ESP_OK;
    for (unsigned i = 0; i <= ESP_NOW_MAX_TOTAL_PEER_NUM; ++i) {
        uint8_t mac[6] = {0x02, 0x48, 0x57, 0x31, 0xff, (uint8_t)i};
        if (!memcmp(mac, own_mac, 6) || !memcmp(mac, peer_mac, 6) || esp_now_is_peer_exist(mac)) continue;
        esp_now_peer_info_t dummy = make_peer(mac);
        terminal = esp_now_add_peer(&dummy);
        if (terminal != ESP_OK) break;
        if (added_count < ESP_NOW_MAX_TOTAL_PEER_NUM) memcpy(added[added_count++], mac, 6);
        else { terminal = ESP_FAIL; esp_now_del_peer(mac); break; }
    }
    peer_check("capacity_error", terminal, ESP_ERR_ESPNOW_FULL, &failed);
    peer_check("capacity_query", esp_now_get_peer_num(&after), ESP_OK, &failed);
    peer_check("capacity_count", after.total_num == ESP_NOW_MAX_TOTAL_PEER_NUM ? ESP_OK : ESP_FAIL,
               ESP_OK, &failed);
    for (unsigned i = 0; i < added_count; ++i)
        peer_check("cleanup", esp_now_del_peer(added[i]), ESP_OK, &failed);
    peer_check("count_after", esp_now_get_peer_num(&after), ESP_OK, &failed);
    peer_check("count_restored", after.total_num == before.total_num ? ESP_OK : ESP_FAIL, ESP_OK, &failed);
    peer_check("real_restored", ensure_peer(peer_mac) ? ESP_OK : ESP_FAIL, ESP_OK, &failed);
    counters.peer_test_failures += failed;
    ESP_LOGI(TAG, "PEER_TEST_DONE real=" MACSTR " capacity=%d added=%u failures=%u",
             MAC2STR(peer_mac), ESP_NOW_MAX_TOTAL_PEER_NUM, added_count, failed);
}

static void handle_command(const char *line)
{
    unsigned value, size;
    char extra;
    if (!strcmp(line, "stats")) {
        print_stats();
    } else if (!strcmp(line, "peer_test")) {
        peer_test();
    } else if (sscanf(line, "ping %u %u %c", &value, &size, &extra) == 2 &&
               value >= 1 && value <= 1000 && hw1_milestone_probe_size(size)) {
        if (!peer_known || run.active || !ensure_peer(peer_mac)) {
            ESP_LOGW(TAG, "PING_REJECT peer_known=%u already_active=%u", peer_known, run.active);
            return;
        }
        memset(&run, 0, sizeof(run));
        run.active = true;
        run.count = value;
        run.size = size;
        memcpy(run.dst, peer_mac, 6);
        ESP_LOGI(TAG, "PING_START count=%u size=%u dst=" MACSTR, value, size, MAC2STR(run.dst));
    } else if (sscanf(line, "channel %u %c", &value, &extra) == 1 && value >= 1 && value <= 11) {
        if (run.active) {
            ESP_LOGW(TAG, "CHANNEL rejected: ping active");
            return;
        }
        esp_err_t err = esp_wifi_set_channel(value, WIFI_SECOND_CHAN_NONE);
        if (err == ESP_OK) channel = value;
        ESP_LOGI(TAG, "CHANNEL requested=%u result=%s", value, esp_err_to_name(err));
        print_stats();
    } else if (!strcmp(line, "restart_radio")) {
        if (run.active) {
            ESP_LOGW(TAG, "RADIO_RESTART rejected: ping active");
            return;
        }
        ESP_LOGI(TAG, "RADIO_RESTART begin physical_c6_reset=1");
        ESP_ERROR_CHECK(esp_now_unregister_recv_cb());
        ESP_ERROR_CHECK(esp_now_unregister_send_cb());
        ESP_ERROR_CHECK(esp_now_deinit());
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_deinit());
        ESP_ERROR_CHECK(esp_hosted_deinit());
        peer_known = false;
        counters.restarts++;
        radio_start();
        ESP_LOGI(TAG, "RADIO_RESTART complete");
    } else {
        ESP_LOGW(TAG, "COMMANDS stats | peer_test | ping <1..1000> <32|64|128|250> | channel <1..11> | restart_radio");
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
    /* This experiment never calls esp_wifi_connect or configures credentials. */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    events = xQueueCreate(48, sizeof(probe_event_t));
    ESP_ERROR_CHECK(events ? ESP_OK : ESP_ERR_NO_MEM);
    radio_start();
    ESP_ERROR_CHECK(xTaskCreate(console_task, "probe_console", 3072, NULL, 3, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    int64_t next_discovery = 0;
    for (;;) {
        probe_event_t event;
        if (xQueueReceive(events, &event, pdMS_TO_TICKS(5)) == pdTRUE) {
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
        tick_ping(now);
        /* Discovery pauses during a measured run so it cannot add bus traffic
         * to a pending request. It otherwise runs only once every two seconds. */
        if (!run.active && now >= next_discovery) {
            send_discovery();
            next_discovery = now + 2000000;
        }
    }
}
