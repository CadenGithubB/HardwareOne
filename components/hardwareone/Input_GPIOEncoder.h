#pragma once

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_GPIO_ENCODER
// Native quadrature encoder with a push switch and optional auxiliary buttons.
// Pins, button actions and phase direction belong to the board profile.

// How gpioEncoderStart() ended. Everything but Started leaves the encoder stopped.
enum class GpioEncoderStart : uint8_t {
  Started,         // running, also when it already was
  PinsInvalid,     // an encoder or auxiliary button pin is not a usable GPIO
  CacheMissing,    // the shared input cache does not exist
  OutOfMemory,     // the encoder state mutex could not be created
  Busy,            // the encoder state or the input cache stayed locked
  TaskNotCreated,  // the input task could not be created
};
GpioEncoderStart gpioEncoderStart();
// gpioEncoderStart() as started or not, the form every input driver offers.
bool inputStartInternal();
void gpioEncoderStop();
bool gpioEncoderHasPendingDetents();
int gpioEncoderConsumeOneDetent();
int gpioEncoderBuildDataJson(char* buf, size_t size);
#endif
