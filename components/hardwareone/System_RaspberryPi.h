// System_RaspberryPi.h — finite Raspberry Pi 5 / CM5 host-power protocol.
//
// Requests leave HardwareOne only as fixed UART EVT tokens. The Linux host
// acknowledges them through the normal authenticated command path, which
// keeps authorization in the central command dispatcher rather than adding a
// second control channel.
#ifndef SYSTEM_RASPBERRY_PI_H
#define SYSTEM_RASPBERRY_PI_H

#include <Arduino.h>
#include "System_BuildConfig.h"

struct CommandEntry;

#if ENABLE_RASPBERRY_PI_HOST_POWER

void raspberryPiHostPowerInit();
void raspberryPiHostPowerTick();

extern const CommandEntry raspberryPiHostPowerCommands[];
extern const size_t raspberryPiHostPowerCommandsCount;

#else

inline void raspberryPiHostPowerInit() {}
inline void raspberryPiHostPowerTick() {}

#endif  // ENABLE_RASPBERRY_PI_HOST_POWER

#endif  // SYSTEM_RASPBERRY_PI_H
