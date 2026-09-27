/* SPDX-License-Identifier: Apache-2.0
 * Derived from ESPHome esp-hosted-firmware/slave-overlay; see README.md.
 */
#ifndef HW1_ESP_NOW_HOSTED_SLAVE_H
#define HW1_ESP_NOW_HOSTED_SLAVE_H
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Registers the single CustomRpc request handler. No Wi-Fi/ESP-NOW startup.
 * A constructor calls this by default; force-link this symbol, or compile with
 * ESP_NOW_HOSTED_SLAVE_AUTOSTART=0 and call it explicitly during slave startup.
 */
esp_err_t esp_now_hosted_slave_init(void);

#ifdef __cplusplus
}
#endif
#endif
