#pragma once

#include "System_BuildConfig.h"
#include <stddef.h>

#if ENABLE_GPIO_ENCODER
// Native quadrature encoder with a push switch and optional auxiliary buttons.
// Pins, button actions and phase direction belong to the board profile.
bool inputStartInternal();
void gpioEncoderStop();
bool gpioEncoderHasPendingDetents();
int gpioEncoderConsumeOneDetent();
int gpioEncoderBuildDataJSON(char* buf, size_t size);
#endif
