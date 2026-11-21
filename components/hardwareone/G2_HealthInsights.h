#pragma once

// Stock G2 Health panel, page 2 ("insights"), fed from the R1 ring's daily
// history: up to three lines (heart rate + HRV, steps, sleep or SpO2) over the
// most recent days that have data. Sent like the Even app sends its insights:
// sid 0x0E cmd 4 to the right temple. Wire, limits and text layout live in
// G2_HealthInsightsCore.h; docs/G2_HEALTH_PANEL.md describes the feature.
//
// Runs while the Health panel feed runs (gSettings.g2HealthPanelFeed) and its
// own switch is on (gSettings.g2HealthInsightsFeed, default on); the window is
// gSettings.g2HealthInsightsDays days (1-7, default 3). The glasses keep the
// lines in RAM only; nothing here writes their flash or the ring.
//
// Threads:
//   main loop      g2HealthInsightsMainTick (from g2HealthPushMainTick) reads
//                  the history (RAM day under the history lock, older days
//                  from the store), builds the message body and publishes it
//                  in a PSRAM buffer;
//   G2 control     g2HealthInsightsOwnerTick / g2HealthInsightsOnRx (from
//                  G2_HealthPush.cpp) only send the published body and match
//                  the echo. They never take the history lock.

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_R1_HEALTH

#include "G2_FeedCore.h"

// Main loop only, every pass. `linkReady` = the right temple is connected (as
// the Health panel feed last published it).
void g2HealthInsightsMainTick(bool linkReady);

// G2 control owner only, once per lap from g2HealthPushTick after the
// MULT_DATA snapshot's own decision. `snapshotInFlight` = a MULT_DATA snapshot
// on this link still waits for its echo: the insights feed then holds its
// automatic send (the snapshot goes first after a connect).
void g2HealthInsightsOwnerTick(hw1_g2_feed::Gate gate, uint32_t generation, uint32_t nowMs,
                               bool snapshotInFlight);
// G2 control owner only: an insights message on this link waits for its echo
// (the MULT_DATA snapshot holds its automatic send meanwhile).
bool g2HealthInsightsInFlight(uint32_t generation);
// G2 control owner only: a sid 0x0E cmd 4 envelope from the right temple.
void g2HealthInsightsOnRx(uint32_t rightGeneration, const uint8_t* pb, size_t len);

// G2 control owner only (G2_HealthPush.cpp): the one rolling magic for every
// sid 0x0E message HardwareOne sends, so two in a row never share one.
uint8_t g2HealthSid0eNextMagic();

// G2 transport boundary, implemented in G2_Glasses.cpp. Re-validates `pb`
// against the insights whitelist and writes it to the right temple at exactly
// `generation`.
bool g2HealthInsightsWireSend(uint32_t generation, const uint8_t* pb, size_t len);

class String;
const char* cmd_ringinsightsstatus(const String& args);
const char* cmd_ringinsightspreview(const String& args);
const char* cmd_ringinsightssend(const String& args);
const char* cmd_ringinsightsfeed(const String& args);
const char* cmd_ringinsightsdays(const String& args);

#endif
