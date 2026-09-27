#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call from app_main after initArduino(), before HardwareOne starts networking.
// Prepares the companion transport on the EYE board; native Wi-Fi is a no-op.
// Arduino retains ownership of esp_wifi_init/start and its network interfaces.
// Startup only: this API does not recover a lost or reset companion.
esp_err_t hw1MeshRadioPrepare(void);

// Local diagnostic counters only; this does not send a radio RPC.
void hw1MeshRadioPrintStats(void);

#ifdef __cplusplus
}
#endif
