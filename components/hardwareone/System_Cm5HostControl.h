// System_Cm5HostControl.h — finite CM5 host power/fan control protocols.
//
// Requests leave HardwareOne only as fixed UART EVT tokens. The Linux host's
// four machine callbacks (power/fan ACK and report) are authenticated and
// handled directly by the UART drain, outside cmd_exec and the human command
// audit. Human-issued power/fan operations remain ordinary audited commands.
//
// Two independent protocols share that plumbing and live in this module
// because they are the same CM5 request/ACK/report machine over the same
// link: host power/profile (`cm5 power`) and host fan policy (`cm5 fan`).
// They keep separate request records so a fan change is never blocked by, and
// never resolves, a power transition.
//
// The command surface is deliberately NOT registered here. Every verb lives
// in the one `cm5` namespace so a single `help cm5` page describes the whole
// device: System_Cm5Presence.cpp owns cm5PresenceCommands[] and pulls in the
// handlers declared below. This file is the protocol, not the registry.
#ifndef SYSTEM_CM5_HOST_CONTROL_H
#define SYSTEM_CM5_HOST_CONTROL_H

#include <Arduino.h>
#include "System_BuildConfig.h"

enum class Cm5HostCallbackIntrinsicResult : uint8_t {
  NotCallback,
  Handled,
};

// Recognize and apply only the canonical machine callbacks emitted by the CM5
// daemon: `cm5 <power|fan> <ack|report> ...`. Once that three-token prefix is
// recognized the line is consumed, including usage/auth/state errors, so a
// host retry can never fall through to cmd_exec or the durable command audit.
// `sessionMayControl` is the role decision captured with `namedSessionEpoch`
// by the UART session snapshot; Guest sessions are rejected here just as they
// are by the ordinary command authorizer.
#if ENABLE_RASPBERRY_PI_HOST_POWER || ENABLE_RASPBERRY_PI_HOST_FAN
Cm5HostCallbackIntrinsicResult cm5HostControlHandleCallbackIntrinsic(
    const char* line, uint32_t namedSessionEpoch, bool sessionMayControl,
    char* reply, size_t replySize);
#else
inline Cm5HostCallbackIntrinsicResult cm5HostControlHandleCallbackIntrinsic(
    const char*, uint32_t, bool, char*, size_t) {
  return Cm5HostCallbackIntrinsicResult::NotCallback;
}
#endif

#if ENABLE_RASPBERRY_PI_HOST_POWER

void cm5HostPowerInit();
void cm5HostPowerTick();

// Registered by System_Cm5Presence.cpp as `cm5 power ...`.
const char* cmdCm5Power(const String& argsInput);
const char* cmdCm5PowerAck(const String& argsInput);
const char* cmdCm5PowerReport(const String& argsInput);
const char* cmdCm5PowerReboot(const String& argsInput);
const char* cmdCm5PowerHalt(const String& argsInput);
const char* cmdCm5PowerSuspend(const String& argsInput);
const char* cmdCm5PowerSleepFor(const String& argsInput);
const char* cmdCm5PowerRecover(const String& argsInput);

#else

inline void cm5HostPowerInit() {}
inline void cm5HostPowerTick() {}

#endif  // ENABLE_RASPBERRY_PI_HOST_POWER

#if ENABLE_RASPBERRY_PI_HOST_FAN

void cm5HostFanInit();
void cm5HostFanTick();

// Registered by System_Cm5Presence.cpp as `cm5 fan ...`.
const char* cmdCm5Fan(const String& argsInput);
const char* cmdCm5FanAck(const String& argsInput);
const char* cmdCm5FanReport(const String& argsInput);

#else

inline void cm5HostFanInit() {}
inline void cm5HostFanTick() {}

#endif  // ENABLE_RASPBERRY_PI_HOST_FAN

#endif  // SYSTEM_CM5_HOST_CONTROL_H
