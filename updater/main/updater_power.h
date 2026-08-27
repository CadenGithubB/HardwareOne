#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Meaningful ONLY when usb_sense_available is true. The Feather ESP32 V2
     * exposes no VBUS sense line, so on that board this field stays false and
     * says nothing about whether USB is actually connected. */
    bool usb_present;
    bool usb_sense_available;

    bool battery_valid;
    /* True when a cell really appears to be attached. On the ADC backend a
     * false here means the unit is running on external power, which is the
     * safe state -- not a depleted battery. */
    bool battery_present;

    /* Fuel-gauge backend only; 0 on boards with no gauge. */
    float battery_percent;
    /* Voltage-divider backend only; 0 on boards with no ADC battery sense. */
    uint32_t battery_millivolts;
} updater_power_status_t;

/* Performs a new GPIO/I2C sample on every call; no persisted telemetry is used. */
esp_err_t updater_power_sample(updater_power_status_t *status,
                               char *reason, size_t reason_size);
esp_err_t updater_power_require_safe(char *reason, size_t reason_size);

#ifdef __cplusplus
}
#endif
