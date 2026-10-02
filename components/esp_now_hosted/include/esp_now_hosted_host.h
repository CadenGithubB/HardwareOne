/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HW1_ESP_NOW_HOSTED_HOST_H
#define HW1_ESP_NOW_HOSTED_HOST_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional explicit resource initialization. esp_now_init() calls this too.
 * ESP-Hosted must already be initialized. These resources intentionally live
 * until reboot; esp_now_deinit() deinitializes the REMOTE ESP-NOW instance.
 */
esp_err_t esp_now_hosted_host_start(void);

typedef struct {
    uint32_t malformed_messages;
    uint32_t stale_responses;
    uint32_t events_dropped;
    uint32_t request_timeouts;
    uint32_t transport_errors;
} esp_now_hosted_host_stats_t;

void esp_now_hosted_host_get_stats(esp_now_hosted_host_stats_t *out);

/* Asks the C6's ACTUAL peer table and preserves transport errors. The standard
 * esp_now_is_peer_exist() answers from the host mirror of successfully applied
 * add/mod/del operations instead, so the mesh router's per-frame check never
 * waits on an SDIO round trip; see README.md "Peer existence".
 */
esp_err_t esp_now_hosted_is_peer_exist(const uint8_t *mac, bool *exists);

/* Boot-time check that the C6 firmware carries the HardwareOne bridge. Sends
 * one GET_VERSION request and classifies the outcome. ESP-Hosted must already
 * be connected. A reply with ANY native status proves the bridge is present;
 * no reply within ESP_NOW_HOSTED_TIMEOUT_MS means stock Hosted firmware (or a
 * companion that is not answering). native_version/native_status are optional
 * and only meaningful when the bridge answered.
 */
typedef enum {
    ESP_NOW_HOSTED_BRIDGE_UNKNOWN = 0,   /* not probed yet */
    ESP_NOW_HOSTED_BRIDGE_PRESENT = 1,   /* a bridge reply was received */
    ESP_NOW_HOSTED_BRIDGE_ABSENT = 2,    /* probe timed out: no bridge in the C6 */
    ESP_NOW_HOSTED_BRIDGE_TRANSPORT_ERROR = 3, /* Hosted refused the request */
} esp_now_hosted_bridge_state_t;

esp_err_t esp_now_hosted_host_probe(uint32_t *native_version, int32_t *native_status);
esp_now_hosted_bridge_state_t esp_now_hosted_bridge_state(void);

#ifdef __cplusplus
}
#endif
#endif
