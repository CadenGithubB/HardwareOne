// System_ESPNow_ForwardRx.h - the mesh master's side of forwarding for a bond partner
//
// A bond master (B) may forward its bond partner's (X) readings, battery
// state, events and sensor status to the masters named on it
// (System_ESPNow_ForwardCore.h has the rules, System_ESPNow_BondForward.cpp
// the bond master's side). This is the receiving side, on every build with
// ESP-NOW (with or without bond mode): the link table that FORWARD_LINK (155)
// announcements fill, the answer to them, the question every received
// reading, battery, event and sensor status frame asks: whose is it? and the
// address and link number a sensor request for X goes to (B, with B's link
// number). Any paired device may announce, as any paired device's own
// readings are kept: what B sends for X is always shown "via" B.
//
//   espnow_task  v4hForwardLink, espnowForwardResolve (from the four
//                ingests), espnowForwardRxTick
//   cmd_exec     espnowforwardlinks, espnowForwardRequestTarget, the readers
// The table is written on espnow_task only; readers take copies under its lock.
#pragma once

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "System_ESPNow_ForwardCore.h"

#include "System_ESPNow_Verdict.h"  // V4Verdict, what a handler returns (1.0 step 5b)
struct V4RxCtx;

// ForwardOrigin, ForwardResolve and ForwardTarget are in System_ESPNow_ForwardCore.h.

// espnow_task. `bondLink`: the frame came as 179/180/182 over the bond link.
// `hasBit`: the frame type has a forwarded flag bit (readings, events);
// `streamId`: 0..10 a sensor, 11 battery, 12 events, 13 the sensor status.
ForwardResolve espnowForwardResolve(const uint8_t src[6], const char* srcName, bool bondLink, bool hasBit, bool bitSet,
                                    uint8_t fwdLink, uint8_t streamId, ForwardOrigin* out);
// The counter rule judged the first frame of `streamId` since the link began.
void espnowForwardResumeTaken(const uint8_t via[6], uint8_t streamId);
// The link of `via` is active and names `origin` (a forwarded record is kept).
bool espnowForwardLinkActive(const uint8_t via[6], const uint8_t origin[6]);
// Active links (for espnowsensorstatus).
size_t espnowForwardLinkCount();
// A forwarded origin by the name its link shows (or its address), optionally
// of one via. False: no link names it.
bool espnowForwardFindOrigin(const char* who, const uint8_t* via, uint8_t outOrigin[6], uint8_t outVia[6]);
// The name a link shows for its partner, or "" (copy).
void espnowForwardOriginName(const uint8_t via[6], const uint8_t origin[6], char* out, size_t cap);
// Where a sensor request for the forwarded origin `who` (its shown name or
// address) goes: the device that forwards for it (`viaWho`: that device's name
// or address; nullptr: any) and that link's number, which the request carries
// in its link byte. espnowsensorreq on a mesh master.
ForwardTarget espnowForwardRequestTarget(const char* who, const char* viaWho, uint8_t outVia[6], uint8_t outOrigin[6],
                                         uint8_t* outLinkGen);

void espnowForwardRxTick(uint32_t nowMs);
V4Verdict v4hForwardLink(const V4RxCtx& ctx);

const char* cmd_espnow_forwardlinks(const String& args);

#endif  // ENABLE_ESPNOW
