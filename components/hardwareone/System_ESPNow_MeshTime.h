// System_ESPNow_MeshTime.h - mesh time (TIME_SYNC, opcode 35)
//
// The mesh master sends its UTC clock to each paired device over that
// device's session; a device with no better source of its own sets its clock
// from it. The rules are in System_ESPNow_TimeCore.h. This part reads the
// settings and the clock, sends the frames and sets the clock.
#ifndef SYSTEM_ESPNOW_MESHTIME_H
#define SYSTEM_ESPNOW_MESHTIME_H

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <Arduino.h>
#include <stdint.h>

struct V4RxCtx;

// TIME_SYNC receive handler (espnow_task). Keeps the frame for the main loop;
// sets nothing itself. The receive table has already required a paired sender
// and a session-encrypted frame.
void v4hTimeSync(const V4RxCtx& ctx);

// espnow_task loop: on the acting master, sends the clock to paired devices
// (one per half second) and starts a session with a paired device that has
// none.
void espnowMeshTimeSendTick(uint32_t nowMs);

// Main loop, after ntpSyncDrainTick() and before Clock::clockDutiesTick():
// judges the frame the handler kept and sets the clock when the rules say so.
void espnowMeshTimeApplyTick();

const char* cmd_espnow_timesync(const String& argsInput);
const char* cmd_espnow_timestatus(const String& argsInput);

#else

inline void espnowMeshTimeApplyTick() {}

#endif  // ENABLE_ESPNOW
#endif  // SYSTEM_ESPNOW_MESHTIME_H
