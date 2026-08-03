// =============================================================================
// Time anchors — retro-dating capture files written before clock sync
// =============================================================================
// Problem: boards without an RTC (and before NTP) don't know wall-clock time,
// so sensor/health capture sessions started "in the dark" are named by boot
// counter + uptime (health-boot<N>-<ms>.csv) inside a boot-<N>/ subfolder.
// That guarantees ORDER (bootCounter is NVS-monotonic, millis is monotonic
// within a boot) but not DATES.
//
// Fix: the first time a boot learns real time — early RTC sync, NTP after
// WiFi, or a manual `time set`; anything that flips Clock::isSynced() —
// record ONE anchor line in /logging_captures/sensors/.anchors.csv:
//
//     bootCounter,millisAtSync,epochAtSync
//
// Every file stamped (boot, ms) is then exactly datable, INCLUDING files
// written before the sync happened:
//
//     fileEpoch = epochAtSync - (millisAtSync - fileMs) / 1000
//
// A low-duty sweep (timeAnchorsTick, called from the main loop) promotes
// boot-<N>/ files into dated YYYY-MM-DD/ folders as
//   <base>-<YYYY-MM-DDTHH-MM-SS>-boot<N>-<ms9><ext>
// so same-second collisions cannot LittleFS-rename-overwrite a prior file.
// Destinations that somehow still exist are counter-suffixed or skipped
// (left in boot-<N>/) — never overwritten.
//   - current boot: as soon as the anchor lands. The file an active
//     sensorlog session holds open is skipped and promotes after the
//     session stops (the tick watches the running→stopped edge) or on the
//     next boot.
//   - previous boots: on any later boot, via their registry line. Boots
//     that never learned time keep their boot-<N>/ folder — still ordered,
//     just undated (a later boot's anchor can't recover powered-off gaps).
// A boot's registry line is pruned once its folder has fully promoted.
//
// Limits: millis() wraps at ~49.7 days. A promoted date must fall within
// ±40 days of its anchor or the file is left in place — mis-dating is worse
// than staying boot-named.
#pragma once

// Main-loop tick. Self-throttled (a few seconds); cheap no-op until the
// filesystem is up and Clock::isSynced(). Writes this boot's anchor once,
// then drives the promote sweep to completion. Also re-arms the sweep when
// it observes sensor logging stop (so the just-closed boot-named file
// promotes promptly).
void timeAnchorsTick();
