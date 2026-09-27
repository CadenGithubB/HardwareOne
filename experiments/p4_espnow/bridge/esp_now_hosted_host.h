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

/* Unlike esp_now_is_peer_exist()'s bool API, this preserves transport errors. */
esp_err_t esp_now_hosted_is_peer_exist(const uint8_t *mac, bool *exists);

#ifdef __cplusplus
}
#endif
#endif
