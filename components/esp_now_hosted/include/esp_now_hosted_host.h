/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HW1_ESP_NOW_HOSTED_HOST_H
#define HW1_ESP_NOW_HOSTED_HOST_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_now.h"
#include "esp_now_hosted_rpc.h"  /* esp_now_hosted_info_t, image states */

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

/* Check that the C6 firmware carries the HardwareOne bridge. Sends one
 * GET_INFO request and classifies the outcome. ESP-Hosted must already be
 * connected. A reply with ANY status proves the bridge is present (a slave
 * older than GET_INFO answers ESP_ERR_NOT_SUPPORTED: PRESENT_NO_INFO); no
 * reply within ESP_NOW_HOSTED_TIMEOUT_MS means stock Hosted firmware or a
 * companion that is not answering. `info` is optional and filled only when the
 * slave answered GET_INFO with ESP_OK.
 */
typedef enum {
    ESP_NOW_HOSTED_BRIDGE_UNKNOWN = 0,         /* not probed yet */
    ESP_NOW_HOSTED_BRIDGE_PRESENT = 1,         /* bridge answered GET_INFO */
    ESP_NOW_HOSTED_BRIDGE_ABSENT = 2,          /* probe timed out: no bridge in the C6 */
    ESP_NOW_HOSTED_BRIDGE_TRANSPORT_ERROR = 3, /* Hosted refused the request */
    ESP_NOW_HOSTED_BRIDGE_PRESENT_NO_INFO = 4, /* bridge answered, but predates GET_INFO */
} esp_now_hosted_bridge_state_t;

esp_err_t esp_now_hosted_host_probe(esp_now_hosted_info_t *info);
esp_now_hosted_bridge_state_t esp_now_hosted_bridge_state(void);

/* Live companion info (GET_INFO) and image confirmation (CONFIRM_IMAGE). Both
 * return the slave's own status; ESP_ERR_NOT_SUPPORTED from an older bridge. */
esp_err_t esp_now_hosted_bridge_info(esp_now_hosted_info_t *info);
esp_err_t esp_now_hosted_confirm_image(void);

/* Offline mode for a companion that stopped answering (transport failure,
 * C6 reset or an update in progress). While offline every request fails
 * immediately with ESP_ERR_INVALID_STATE instead of waiting out the bridge
 * timeout, and esp_now_deinit() / callback unregistration only clear the
 * host's local state and report ESP_OK, so the application's normal teardown
 * completes in milliseconds. Clear it after the transport is back up.
 */
void esp_now_hosted_host_set_offline(bool offline);
bool esp_now_hosted_host_is_offline(void);

/* Forget the remote ESP-NOW instance without talking to the companion: after a
 * C6 reset its table, callbacks and epoch are gone regardless. */
void esp_now_hosted_host_reset_local(void);

#ifdef __cplusplus
}
#endif
#endif
