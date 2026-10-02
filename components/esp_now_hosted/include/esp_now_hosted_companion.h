/* SPDX-License-Identifier: Apache-2.0
 * The ESP32-C6 companion firmware a HardwareOne P4 image expects.
 *
 * Single source of truth for three consumers:
 *   - main/radio_backend.cpp compares the running C6's reported Hosted version
 *     with these values at boot and probes for the ESP-NOW bridge;
 *   - tools/p4/companion/prepare.py reads the TAG/SHA literals below to fetch
 *     and verify the pinned ESP-Hosted slave and serial-flasher sources;
 *   - the documentation quotes them.
 * Keep every value a plain literal on its own line; prepare.py parses this
 * file as text, not through the C preprocessor.
 */
#ifndef HW1_ESP_NOW_HOSTED_COMPANION_H
#define HW1_ESP_NOW_HOSTED_COMPANION_H

/* ESP-Hosted MCU slave firmware the bridge was qualified with. */
#define HW1_C6_HOSTED_TAG "v2.12.13"
#define HW1_C6_HOSTED_SHA "dd0176e5fc959d79b306f6677a2f878123c58e9a"
#define HW1_C6_HOSTED_VERSION_MAJOR 2
#define HW1_C6_HOSTED_VERSION_MINOR 12
#define HW1_C6_HOSTED_VERSION_PATCH 13

/* esp-serial-flasher used by the P4-side C6 programmer service. */
#define HW1_C6_FLASHER_TAG "v1.10.0"
#define HW1_C6_FLASHER_SHA "77a994b91f2b7466b97c2fba8ed2d8538330b2bc"

/* The ESP-NOW bridge wire version is ESP_NOW_HOSTED_WIRE_VERSION in
 * esp_now_hosted_rpc.h; both sides compile the same header. */

#endif
