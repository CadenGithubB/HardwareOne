// System_ESPNow_BondForward.h - a bond master forwards its partner's data to the mesh master
//
// With 'bondforward on' a bond master announces to each master named on it
// (FORWARD_LINK, 155) that it forwards its bond partner's readings, battery
// state and events, and forwards them once that master accepted. The rules are
// in System_ESPNow_ForwardCore.h; the receiving side is
// System_ESPNow_ForwardRx.cpp. Everything here runs on espnow_task except the
// command and the status readers (cmd_exec, the web server), which read a
// snapshot.
#pragma once

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stddef.h>
#include <stdint.h>

#include "System_ESPNow_ForwardCore.h"
#include "System_ESPNow_PowerEventCore.h"
#include "System_ESPNow_SensorCore.h"

#if ENABLE_BONDED_MODE
#include <Arduino.h>

void bondForwardTick(uint32_t nowMs);
// A query or an answer from a master (System_ESPNow_ForwardRx.cpp hands it
// over). True: taken (also when it is ignored and counted here).
bool bondForwardOnLinkFrame(const uint8_t src[6], const espnowfwd::LinkFrame& f);
// The partner's frames this device kept (the ingests call these after the
// counter rule kept them; `src` is the radio source).
void bondForwardReading(const uint8_t src[6], const espnowsensor::ReadingHeader& h, const uint8_t* text, size_t len);
void bondForwardPower(const uint8_t src[6], const espnowsensor::PowerFrame& f);
void bondForwardEvent(const uint8_t src[6], const espnowsensor::EventFrame& f);
// A frame of the partner this device could not keep (no record, no memory).
void bondForwardNotKept(uint8_t streamId);
// The "forward" object of 'bondstatus json' and GET /api/bond/status. 0 when
// it does not fit; espnowfwd::kStatusJsonCap holds the longest.
size_t bondForwardStatusJson(char* out, size_t cap);
// The lines of 'bondstatus', separated by '\n', each at most
// espnowfwd::kStatusLineMax bytes: the caller prints them one by one.
// espnowfwd::kStatusLinesCap holds all of them.
void bondForwardStatusLines(char* out, size_t cap);
const char* cmd_bond_forward(const String& args);
#else
// Builds without bond mode never forward.
inline void bondForwardReading(const uint8_t*, const espnowsensor::ReadingHeader&, const uint8_t*, size_t) {}
inline void bondForwardPower(const uint8_t*, const espnowsensor::PowerFrame&) {}
inline void bondForwardEvent(const uint8_t*, const espnowsensor::EventFrame&) {}
inline void bondForwardNotKept(uint8_t) {}
#endif  // ENABLE_BONDED_MODE

#endif  // ENABLE_ESPNOW
