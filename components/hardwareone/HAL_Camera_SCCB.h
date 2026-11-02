#pragma once
#include <stdint.h>
#include "driver/i2c_master.h"
// Retain the manager-owned bus for esp_video. Release only after video/SCCB
// teardown. Each sensor transaction still runs through the I2C manager.
bool cameraSharedSccbAcquire(uint8_t bus, int hardwarePort, i2c_master_bus_handle_t* out);
void cameraSharedSccbRelease();
