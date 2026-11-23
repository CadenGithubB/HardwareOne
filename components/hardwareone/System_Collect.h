// System_Collect.h - file catch-up, the collector: pull files from the devices
// that name this hub
//
// A mesh master or backup master built with ENABLE_ESPNOW_COLLECT (the home
// node) lists the files a worker offers (CATCHUP_LIST 82/83) and reads them
// in windows (CATCHUP_READ 84/85) onto its SD card under /sd/collect/<mac12>/
// files/ (System_CollectStore.h). It runs on its own: when a device's session
// comes back, every 15 minutes, after this hub boots or a card is mounted,
// and on `collectnow`; `collect off` stops it. It pulls only with a writable
// card and a set clock (owner decision Q4); a backup master pulls exactly as
// the master does (Q9). A byte range counts as delivered once one named
// master stored it (Q8): what the device says the other master has is
// skipped and kept as `elsewhere` in index.json, the hook for the later
// master-to-master sync.
//
// Threads, no new task:
//   espnow_task  the receive handlers (copy into PSRAM, never the card) and
//                collectTick: the scheduler and the client state machine,
//                which sends one request at a time;
//   main loop    collectWriteTick: the card (load and plan, append a window
//                and flush it, digest, save index.json), one job at a time;
//   cmd_exec     collectstatus, collectnow, collect, collectrate, collectquota.
// The rules are System_CollectCore.h (host-tested); the card is
// System_CollectStore.cpp (host-tested over the fake VFS).
#ifndef SYSTEM_COLLECT_H
#define SYSTEM_COLLECT_H

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <Arduino.h>
#include <stdint.h>

#include "System_ESPNow_Verdict.h"  // V4Verdict, what a handler returns (1.0 step 5b)
struct V4RxCtx;

// 83 CATCHUP_LIST_RESP and 85 CATCHUP_READ_RESP (espnow_task). The receive
// table has already required a paired sender, a session-encrypted frame and
// the row's minLen. Without ENABLE_ESPNOW_COLLECT: counted and dropped.
V4Verdict v4hCatchupListResp(const V4RxCtx& ctx);
V4Verdict v4hCatchupReadResp(const V4RxCtx& ctx);

// espnow_task loop (step 9f2f): the scheduler and the client.
void collectTick(uint32_t nowMs);

// Main loop (next to sensorLogTick): runs the card job collectTick posted.
void collectWriteTick();

#if ENABLE_ESPNOW_COLLECT
// Commands (module "collect", admin).
const char* cmd_collectstatus(const String& argsInput);
const char* cmd_collectnow(const String& argsInput);
const char* cmd_collect(const String& argsInput);
const char* cmd_collectrate(const String& argsInput);
const char* cmd_collectquota(const String& argsInput);
#endif

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_COLLECT_H
