#ifndef G2_STOCK_FEED_H
#define G2_STOCK_FEED_H

// Feeds the stock G2 stock widget (dashboard tile + expanded stock page) with
// HardwareOne's own system metrics (System_Metrics.h) instead of share
// prices: one stock per metric, label as the ticker, long name as the
// company, the value as the price, the change since the previous sample as
// the change, the history as the chart (185 points, one per `g2 stats step`
// of 10, 20 or 60 s, default 20 s = about an hour; newest at the right edge;
// padded on the left per `g2 stats pad gap|flat` until the window is full;
// a step change starts the chart over). Wire, clamps, whitelist and
// timing live in G2_StockFeedCore.h; the send policy is the shared dashboard
// feed core (G2_FeedCore.h), the second instance next to the Health panel.
//
// Off by default in every build; `g2 stats feed on` starts it and saves that
// (gSettings.g2StockFeed). While it runs and the right temple is connected:
// a full batch after every connect, then a batch when a shown value changed,
// at most once per `g2 stats interval` (default 60 s), every 10 s while the
// glasses report the expanded stock page open. While it is on, the sampler
// keeps sampling whether or not the glasses are connected, so a reconnect
// sends the whole chart window; feed off (or a reboot) drops the history. Paused
// while Even AI,
// Conversate, the native terminal, a native note or QuickNote own the
// glasses. Only glasses firmware with a captured fixture (2.3.2.14) is fed.
// `g2 stats feed off` clears the widget (stock_total 0). The glasses keep the
// stocks in RAM only; nothing here writes their flash, settings or layout.

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// G2 control owner only (g2_owner, after g2HealthPushTick): link state,
// sampler interest, send policy and the writes.
void g2StockFeedTick();
// G2 control owner only: the longest the owner may sleep before the next
// lap so a waiting request or batch is served on time (UINT32_MAX: no limit).
uint32_t g2StockFeedOwnerWaitMs();
// G2 control owner only, once as it exits (closeg2): drops the page-open
// cadence and any preview warm-up that no later lap would expire; keeps the
// sampler consumer (and its history) at the feed interval while the feed is
// on, releases it otherwise.
void g2StockFeedOwnerStopped();
// sid 0x01 envelope from temple `side` ('L'/'R') at `generation` (same task):
// acks from the right temple, page-state pushes from either.
void g2StockFeedOnRx(char side, uint32_t generation, const uint8_t* pb, size_t len);

// G2 transport boundary, implemented in G2_Glasses.cpp. `busy` is true while
// Even AI, Conversate, the native terminal, a native note or QuickNote own
// the glasses; `firmware` receives the right temple's firmware version ("" if
// not known yet).
bool g2StockFeedRightLink(uint32_t* generation, bool* busy, char* firmware, size_t firmwareCap);
// Re-validates `pb` against the stock whitelist and writes it to the right
// temple at exactly `generation` (sid 0x01). `gapPrefix`: the chart was
// padded in gap mode, so a NaN prefix is allowed (and nowhere else). False if
// invalid, not ready or the write failed.
bool g2StockFeedWireSend(uint32_t generation, const uint8_t* pb, size_t len, bool gapPrefix);
// Asks the right temple at exactly `generation` for its settings (a BASIC
// request); the reply fills the firmware string. False if not sent.
bool g2StockFeedRequestFirmware(uint32_t generation);
// Wakes the G2 control owner (any task) so a command's request is taken now
// instead of at the next idle lap (up to 6 s).
void g2StockFeedWake();

class String;
const char* cmd_g2statsstatus(const String& args);
const char* cmd_g2statspreview(const String& args);
const char* cmd_g2statssend(const String& args);
const char* cmd_g2statsfeed(const String& args);
const char* cmd_g2statsmetrics(const String& args);
const char* cmd_g2statsinterval(const String& args);
const char* cmd_g2statspad(const String& args);
const char* cmd_g2statsstep(const String& args);

#else

inline void g2StockFeedTick() {}
inline uint32_t g2StockFeedOwnerWaitMs() { return UINT32_MAX; }
inline void g2StockFeedOwnerStopped() {}
inline void g2StockFeedOnRx(char, uint32_t, const uint8_t*, size_t) {}

#endif

#endif  // G2_STOCK_FEED_H
