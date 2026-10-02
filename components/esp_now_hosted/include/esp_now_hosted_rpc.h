/* SPDX-License-Identifier: Apache-2.0
 * Derived from ESPHome esp-hosted-firmware/slave-overlay, commit
 * 14ee146ec71923aa250a7f743e5cff266a5ffb1a. See README.md and LICENSE.
 * HardwareOne experiment: incompatible, versioned wire protocol. Both ends
 * MUST use this header. No native structs or pointers cross the transport.
 */
#ifndef HW1_ESP_NOW_HOSTED_RPC_H
#define HW1_ESP_NOW_HOSTED_RPC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Separate IDs prevent accidental interoperability with the ESPHome overlay. */
#define ESP_NOW_HOSTED_MSG_REQ  UINT32_C(0x48574e01)
#define ESP_NOW_HOSTED_MSG_RESP UINT32_C(0x48574e02)
#define ESP_NOW_HOSTED_MSG_RECV UINT32_C(0x48574e03)
#define ESP_NOW_HOSTED_MSG_SEND UINT32_C(0x48574e04)
#define ESP_NOW_HOSTED_MAGIC UINT32_C(0x314e5748)
#define ESP_NOW_HOSTED_WIRE_VERSION 1u
#define ESP_NOW_HOSTED_MAX_FRAME 1470u
#define ESP_NOW_HOSTED_TIMEOUT_MS 2000u
#define ESP_NOW_HOSTED_EVENT_QUEUE_DEPTH 24u

enum {
    ESP_NOW_HOSTED_OP_INIT = 1,
    ESP_NOW_HOSTED_OP_DEINIT = 2,
    ESP_NOW_HOSTED_OP_ADD_PEER = 3,
    ESP_NOW_HOSTED_OP_DEL_PEER = 4,
    ESP_NOW_HOSTED_OP_IS_PEER_EXIST = 5,
    ESP_NOW_HOSTED_OP_SEND = 6,
    ESP_NOW_HOSTED_OP_GET_VERSION = 7,
    ESP_NOW_HOSTED_OP_SET_PMK = 8,
    ESP_NOW_HOSTED_OP_MOD_PEER = 9,
    ESP_NOW_HOSTED_OP_GET_PEER = 10,
    ESP_NOW_HOSTED_OP_GET_PEER_NUM = 11,
    ESP_NOW_HOSTED_OP_REGISTER_RECV = 12,
    ESP_NOW_HOSTED_OP_UNREGISTER_RECV = 13,
    ESP_NOW_HOSTED_OP_REGISTER_SEND = 14,
    ESP_NOW_HOSTED_OP_UNREGISTER_SEND = 15,
    /* Additive within wire version 1: a slave without them answers
     * ESP_ERR_NOT_SUPPORTED, which the host reads as "bridge present, older". */
    ESP_NOW_HOSTED_OP_GET_INFO = 16,      /* companion runtime + image state, no side effects */
    ESP_NOW_HOSTED_OP_CONFIRM_IMAGE = 17, /* mark the running C6 image valid (cancel rollback) */
};

/* Embedded verbatim in the C6 image's read-only data by the slave, so a host
 * can check that a candidate firmware FILE carries this bridge before sending
 * a byte of it over OTA. The trailing number is the wire version; bump both
 * together. */
#define ESP_NOW_HOSTED_BRIDGE_MARKER "HW1-ESPNOW-BRIDGE/1"

/* P4 and C6 are little-endian. Packed layout is verified below. Each sequence
 * is used once per host boot, including timed-out requests and reinitialization.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t opcode;
    uint16_t payload_len;
    uint64_t seq;
} esp_now_hosted_req_t;

typedef struct __attribute__((packed)) {
    esp_now_hosted_req_t request; /* payload_len describes RETURN bytes here */
    int32_t status;
} esp_now_hosted_resp_t;

typedef struct __attribute__((packed)) {
    uint8_t peer_addr[6];
    uint8_t lmk[16];
    uint8_t channel;
    uint8_t encrypt;
    int32_t ifidx;
} esp_now_hosted_peer_t;

typedef struct __attribute__((packed)) {
    int32_t total_num;
    int32_t encrypt_num;
} esp_now_hosted_peer_num_t;

typedef struct __attribute__((packed)) {
    uint8_t has_addr;
    uint8_t peer_addr[6];
    uint16_t data_len;
} esp_now_hosted_send_req_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved;
    uint16_t payload_len;
    uint64_t epoch; /* sequence of the latest successful INIT */
} esp_now_hosted_event_header_t;

typedef struct __attribute__((packed)) {
    esp_now_hosted_event_header_t header;
    uint8_t src_addr[6];
    uint8_t des_addr[6];
    uint8_t has_rx_ctrl;
    int8_t rssi;
    uint8_t channel;
    uint32_t timestamp;
    uint16_t data_len;
} esp_now_hosted_recv_evt_t;

typedef struct __attribute__((packed)) {
    esp_now_hosted_event_header_t header;
    uint8_t des_addr[6];
    uint8_t src_addr[6];
    uint8_t has_tx_info;
    int32_t ifidx;
    int32_t rate;
    int32_t status;
    int32_t tx_status;
} esp_now_hosted_send_evt_t;

/* GET_INFO return payload. Every field is a plain value the host may display;
 * none of it is authoritative for Hosted's own version check. */
typedef struct __attribute__((packed)) {
    uint8_t wire_version;          /* ESP_NOW_HOSTED_WIRE_VERSION compiled into the slave */
    uint8_t image_state;           /* ESP_NOW_HOSTED_IMAGE_* below */
    uint8_t running_slot;          /* OTA slot of the running app (0 = ota_0), 0xff unknown */
    uint8_t reset_reason;          /* esp_reset_reason() of the companion */
    uint32_t uptime_ms;            /* companion uptime, wraps after 49.7 days */
    uint32_t free_heap;            /* esp_get_free_heap_size() */
    uint32_t min_free_heap;        /* esp_get_minimum_free_heap_size() since boot */
    uint32_t cpu_mhz;
    uint32_t native_espnow_version;
    char build[16];                /* slave app version string, truncated, not always NUL-terminated */
} esp_now_hosted_info_t;

enum {
    ESP_NOW_HOSTED_IMAGE_UNKNOWN = 0,
    ESP_NOW_HOSTED_IMAGE_VALID = 1,          /* valid, or no OTA record (cable-flashed) */
    ESP_NOW_HOSTED_IMAGE_PENDING_VERIFY = 2, /* new image awaiting CONFIRM_IMAGE; rolls back on the next reboot otherwise */
    ESP_NOW_HOSTED_IMAGE_INVALID = 3,
};

#define ESP_NOW_HOSTED_MAX_PAYLOAD \
    (sizeof(esp_now_hosted_send_req_t) + ESP_NOW_HOSTED_MAX_FRAME)
/* The largest value any request returns: the info block outgrew the peer record. */
#define ESP_NOW_HOSTED_MAX_RETURN \
    (sizeof(esp_now_hosted_info_t) > sizeof(esp_now_hosted_peer_t) \
         ? sizeof(esp_now_hosted_info_t) : sizeof(esp_now_hosted_peer_t))
#define ESP_NOW_HOSTED_MAX_EVENT \
    (sizeof(esp_now_hosted_recv_evt_t) + ESP_NOW_HOSTED_MAX_FRAME)

/* Shared decoders reject truncation AND trailing bytes before payload access. */
static inline int esp_now_hosted_decode_request(const uint8_t *data, size_t len,
                                               esp_now_hosted_req_t *out)
{
    if (!data || !out || len < sizeof(*out)) return 0;
    memcpy(out, data, sizeof(*out));
    return out->magic == ESP_NOW_HOSTED_MAGIC &&
           out->version == ESP_NOW_HOSTED_WIRE_VERSION && out->seq != 0 &&
           out->payload_len <= ESP_NOW_HOSTED_MAX_PAYLOAD &&
           len == sizeof(*out) + out->payload_len;
}

static inline int esp_now_hosted_decode_response(const uint8_t *data, size_t len,
                                                esp_now_hosted_resp_t *out)
{
    if (!data || !out || len < sizeof(*out)) return 0;
    memcpy(out, data, sizeof(*out));
    return out->request.magic == ESP_NOW_HOSTED_MAGIC &&
           out->request.version == ESP_NOW_HOSTED_WIRE_VERSION &&
           out->request.seq != 0 &&
           out->request.payload_len <= ESP_NOW_HOSTED_MAX_RETURN &&
           len == sizeof(*out) + out->request.payload_len;
}

static inline int esp_now_hosted_decode_event(const uint8_t *data, size_t len,
                                             esp_now_hosted_event_header_t *out)
{
    if (!data || !out || len < sizeof(*out) || len > ESP_NOW_HOSTED_MAX_EVENT)
        return 0;
    memcpy(out, data, sizeof(*out));
    return out->magic == ESP_NOW_HOSTED_MAGIC &&
           out->version == ESP_NOW_HOSTED_WIRE_VERSION &&
           out->reserved == 0 && out->epoch != 0 &&
           len == sizeof(*out) + out->payload_len;
}

#if defined(__cplusplus)
#define HW1_WIRE_ASSERT static_assert
#else
#define HW1_WIRE_ASSERT _Static_assert
#endif
HW1_WIRE_ASSERT(sizeof(esp_now_hosted_req_t) == 16, "request wire size");
HW1_WIRE_ASSERT(sizeof(esp_now_hosted_resp_t) == 20, "response wire size");
HW1_WIRE_ASSERT(sizeof(esp_now_hosted_peer_t) == 28, "peer wire size");
HW1_WIRE_ASSERT(sizeof(esp_now_hosted_peer_num_t) == 8, "peer count wire size");
HW1_WIRE_ASSERT(sizeof(esp_now_hosted_send_req_t) == 9, "send wire size");
HW1_WIRE_ASSERT(sizeof(esp_now_hosted_recv_evt_t) == 37, "recv wire size");
HW1_WIRE_ASSERT(sizeof(esp_now_hosted_send_evt_t) == 45, "send event wire size");
HW1_WIRE_ASSERT(sizeof(esp_now_hosted_info_t) == 40, "info wire size");
HW1_WIRE_ASSERT(ESP_NOW_HOSTED_MAX_RETURN == 40, "max return size");
HW1_WIRE_ASSERT(ESP_NOW_HOSTED_MAX_EVENT < 8166, "CustomRpc payload limit");
#undef HW1_WIRE_ASSERT

#endif
