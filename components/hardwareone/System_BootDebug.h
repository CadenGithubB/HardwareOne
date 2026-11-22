// System_BootDebug.h - every debug flag on during boot, into a file of its own
// (owner decision B13, 2026-11-21).
//
// From the top of setup() until setup() ends, every debug line passes its
// flag and level gate (gDebugLineForce in System_Debug.h); the saved flags
// are not changed. Each line of that window is copied into a boot buffer
// (56 KB in PSRAM; 4 KB of internal heap on a board without PSRAM). A line
// the saved flags would have shown still goes to its usual sinks as well; a
// line only the boot capture made goes nowhere else. When setup() ends the
// force stops, the saved flags (the defaults on a first boot) rule again, and
// the buffer is written on cmd_exec to /system/sys_logs/boot-debug-1.log,
// the previous two boots moving to -2 and -3 (admin-only to read, like every
// file there: `fileview "/system/sys_logs/boot-debug-1.log"` or the web file
// browser). The file's header records the capture's start and end uptime, the
// bytes kept and dropped, and the CPU time spent in the capture path.
//
// Also the boot window for the C6 console mirror (System_RadioCompanion.cpp):
// on during boot, off when boot ends unless `c6console on` or a fault wants it.
#ifndef SYSTEM_BOOT_DEBUG_H
#define SYSTEM_BOOT_DEBUG_H

#include <stddef.h>
#include <stdint.h>

// True from bootDebugBegin() (when its buffer was allocated) until
// bootDebugFinish(): the capture is taking lines. Read on every debug line,
// so a plain flag.
extern volatile bool gBootDebugCapturing;

// Top of setup(): allocate the buffer and force every debug line on.
void bootDebugBegin();
// End of setup(): stop the force and the capture, hand the buffer to cmd_exec
// for the file write. Restores the saved flags' gate (never writes settings).
void bootDebugFinish();
// Main loop: retries the file write's hand-off when the executor queue was
// full at the end of setup. One flag read otherwise.
void bootDebugLoopTick();
// True until bootDebugFinish() ran (also on a board whose buffer could not be
// allocated): the C6 console mirror's boot window.
bool bootDebugInProgress();

// System_Debug.cpp's line path while gBootDebugCapturing: one formatted line,
// `shown` when the saved flags would have shown it anyway, `formatStartUs`
// the esp_timer time before a forced line was formatted (0 for a shown line,
// whose formatting is not the capture's cost). Not from an ISR (it skips).
void bootDebugCaptureLine(const char* text, size_t len, bool shown, int64_t formatStartUs);

#endif  // SYSTEM_BOOT_DEBUG_H
