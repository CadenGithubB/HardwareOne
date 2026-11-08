#ifndef G2_HEALTH_PUSH_H
#define G2_HEALTH_PUSH_H

// Feeds the stock G2 Health panel (dashboard Health tile + expanded page) with
// R1 ring data, the way the Even app does: a sid 0x0E MULT_DATA snapshot to
// the right temple. Encoding, limits and policy live in G2_HealthPushCore.h.
//
// Off by default; `ring panel feed on` turns it on and saves that
// (gSettings.g2HealthPanelFeed). While it runs and the right temple is
// connected it also keeps the R1 data fresh: the health logger's timed vitals
// read runs (g2HealthPushWantsRingData) and ring history is refreshed every
// g2HealthPanelHistoryMin minutes. The glasses keep a snapshot in RAM only;
// nothing here writes their flash, settings, dashboard layout or the ring.

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_R1_HEALTH

// G2 control owner only (the task that drains glasses RX): link state, send
// policy, the write and the echo. It never takes the history lock or starts
// ring work.
void g2HealthPushTick();
// Main loop only, every pass (from healthLoggingTick, next to the health
// logger's timed vitals read): rebuilds the snapshot from the R1 data every
// 10 s and schedules ring history refreshes. `vitalsReadActive` holds a due
// history request back while the logger's vitals read is in flight.
void g2HealthPushMainTick(bool vitalsReadActive);
// sid 0x0E envelope from the right temple at `rightGeneration` (same task).
void g2HealthPushOnRx(uint32_t rightGeneration, const uint8_t* pb, size_t len);
// Any task: true while the feed is on and the right temple is connected, i.e.
// while the panel wants fresh ring vitals.
bool g2HealthPushWantsRingData();

// G2 transport boundary, implemented in G2_Glasses.cpp. `busy` is true while
// Even AI, Conversate, the native terminal or a native note owns the glasses.
bool g2HealthRightLink(uint32_t* generation, bool* busy);
// Re-validates `pb` against the whitelist and writes it to the right temple
// at exactly `generation`. False if invalid, not ready or the write failed.
bool g2HealthWireSend(uint32_t generation, const uint8_t* pb, size_t len);

class String;
const char* cmd_ringpanelstatus(const String& args);
const char* cmd_ringpanelon(const String& args);
const char* cmd_ringpaneloff(const String& args);
const char* cmd_ringpanelfeed(const String& args);
const char* cmd_ringpanelhistory(const String& args);
const char* cmd_ringpanelsend(const String& args);
const char* cmd_ringpanelpreview(const String& args);

#else

inline void g2HealthPushTick() {}
inline void g2HealthPushMainTick(bool) {}
inline void g2HealthPushOnRx(uint32_t, const uint8_t*, size_t) {}
inline bool g2HealthPushWantsRingData() { return false; }

#endif

#endif  // G2_HEALTH_PUSH_H
