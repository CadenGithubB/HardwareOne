#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool usb_present;
    bool battery_valid;
    float battery_percent;
} updater_power_status_t;

/* Performs a new GPIO/I2C sample on every call; no persisted telemetry is used. */
esp_err_t updater_power_sample(updater_power_status_t *status,
                               char *reason, size_t reason_size);
esp_err_t updater_power_require_safe(char *reason, size_t reason_size);

#ifdef __cplusplus
}
#endif
