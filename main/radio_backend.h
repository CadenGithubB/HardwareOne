#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Radio bring-up for boards whose Wi-Fi/BLE live on an ESP-Hosted companion
// (ESP32-P4X-EYE: onboard ESP32-C6 over SDIO slot 1). Native-radio targets
// compile these to no-ops.
//
// Call from app_main after initArduino(), before HardwareOne starts networking.
// Arduino retains ownership of esp_wifi_init/start and its network interfaces.
// Startup only: this API does not recover a lost or reset companion.
esp_err_t hw1RadioPrepare(void);

// True when ESP-NOW can be started on this radio: always on native targets;
// on a Hosted board only after the boot probe found the HardwareOne bridge in
// the companion firmware (components/esp_now_hosted).
bool hw1RadioEspNowAvailable(void);

// Local diagnostic counters only; this does not send a radio RPC.
void hw1RadioPrintStats(void);

#ifdef __cplusplus
}
#endif
