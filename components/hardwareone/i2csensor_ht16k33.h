#pragma once

#include "System_BuildConfig.h"
#include <stdint.h>

#if ENABLE_LED_MATRIX
#include <stddef.h>
struct CommandEntry;
extern const CommandEntry matrixCommands[];
extern const size_t matrixCommandsCount;
// Routing is latched on first use, like the other library-backed devices.
uint8_t matrixDeviceAddress();
uint8_t matrixDeviceBus();
#endif
