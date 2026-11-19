// System_ESPNow_BondForward.h - a bond master forwards its partner's data to the mesh master
//
// With 'bondforward on' a bond master announces to each master named on it
// and paired in its primary mesh (FORWARD_LINK, 155) that it forwards its bond
// partner's readings, battery state, events and sensor status, and forwards
// them once that master accepted. The master it forwards readings to may ask
// for the partner's readings (SENSOR_REQ with the link byte): this device
// turns that into its own bond-link control of the partner's sensors, with
// the request's lease. The rules are in System_ESPNow_ForwardCore.h; the
// receiving side is System_ESPNow_ForwardRx.cpp. Everything here runs on
// espnow_task except the command and the status readers (cmd_exec, the web
// server), which read a snapshot, and bondForwardStreamByHand (cmd_exec).
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
// counter rule kept them; `src` is the radio source). Battery and event
// frames come as the payload received, at its full length: they are sent on
// verbatim with only the forwarding fields patched (espnowsensor::
// forwardPowerCopy / forwardEventCopy).
void bondForwardReading(const uint8_t src[6], const espnowsensor::ReadingHeader& h, const uint8_t* text, size_t len);
void bondForwardPower(const uint8_t src[6], const uint8_t* payload, size_t len);
void bondForwardEvent(const uint8_t src[6], const uint8_t* payload, size_t len);
// A frame of the partner this device could not keep (no record, no memory).
void bondForwardNotKept(uint8_t streamId);
// The partner's sensor status (BOND_SENSOR_STATUS) this device kept.
void bondForwardStatus(const uint8_t src[6], const espnowfwd::StatusFrame& f);
// A SENSOR_REQ whose link byte is set: a master asks for the partner's
// readings (v4h_sensor_req hands it over; it is never applied to this
// device's own sensors).
void bondForwardSensorRequest(const uint8_t src[6], uint8_t mode, uint32_t sensorMask, uint32_t intervalMs,
                              uint32_t leaseMs, uint8_t fwdLink);
// This device's user switched a sensor of the partner by hand (bondstream).
void bondForwardStreamByHand(uint8_t sensor, bool on);
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
inline void bondForwardPower(const uint8_t*, const uint8_t*, size_t) {}
inline void bondForwardEvent(const uint8_t*, const uint8_t*, size_t) {}
inline void bondForwardNotKept(uint8_t) {}
inline void bondForwardStatus(const uint8_t*, const espnowfwd::StatusFrame&) {}
#endif  // ENABLE_BONDED_MODE

#endif  // ENABLE_ESPNOW
