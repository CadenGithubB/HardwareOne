#pragma once
#include "esp_err.h"
#include <stdint.h>
// Bounded synchronous playback; rejects overlap and releases I2S/amp on exit.
esp_err_t speakerPlayTone(uint32_t frequencyHz, uint32_t durationMs,
                          uint8_t volume);
