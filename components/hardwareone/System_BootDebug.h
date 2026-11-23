// System_BootDebug.h - every debug output on during boot, into a file of its
// own (owner decision B13, 2026-11-21; follow-up 2026-11-22).
//
// From app_main, right after initArduino() and before the radio companion's
// bring-up, until setup() ends, unless the `bootdebug` setting is off, every
// debug output is on: every debug flag and sub-flag, every explicit
// isDebugFlagSet() gate, the log level at DEBUG (gDebugLineForce in
// System_Debug.h), and every ESP-IDF tag at the build's default level (its
// compiled maximum): the firmware's esp_log calls are recorded instead of
// lowering tags (bootDebugEspLogSet) and replayed when the capture ends. The
// saved settings are not changed. Each line of that window - debug lines,
// broadcast lines, system events, ESP-IDF lines (an esp_log vprintf hook) - is
// copied into the boot buffer (256 KB in PSRAM; 4 KB of internal heap on a
// build without PSRAM). A line the saved settings would have shown still goes
// to its usual sinks; a line only the capture made goes nowhere else, so the
// console looks as it always did. When the buffer is full it keeps the start
// and the newest lines with a [GAP] mark between them (System_BootDebugCore.h).
//
// When setup() ends the force stops and the buffer is written on cmd_exec as
// boot-debug-1.log: on a writable SD card in /sd/system/sys_logs (the two boots
// before it move to -2 and -3), else in /system/sys_logs on flash (only the
// newest boot kept). Admin-only to read: `fileview "<path>"` (pages any size)
// or the web file browser; `bootdebug` says where this boot's file went.
//
// The setting lives in debug.json like every debug setting, and in a one-byte
// NVS copy the capture's start reads before any file system is mounted; off
// means no force, no capture, no buffer (the fastest boot). A change applies
// from the next boot.
//
// Also the boot window for the C6 console mirror (System_RadioCompanion.cpp):
// on during boot, off when boot ends unless `c6console on` or a fault wants it
// (whatever the setting).
#ifndef SYSTEM_BOOT_DEBUG_H
#define SYSTEM_BOOT_DEBUG_H

#include <esp_log.h>
#include <stddef.h>
#include <stdint.h>

class String;

// True from bootDebugBegin() (when its buffer was allocated) until
// bootDebugFinish(): the capture is taking lines. Read on every debug line,
// so a plain flag.
extern volatile bool gBootDebugCapturing;

// app_main, right after initArduino(): unless the setting is off, allocate
// the buffer and force every debug output on.
void bootDebugBegin();
// End of setup(): stop the force and the capture, put the ESP-IDF tag levels
// where the firmware set them, hand the buffer to cmd_exec for the file write.
// Restores the saved settings' gates (never writes settings).
void bootDebugFinish();
// Main loop: retries the file write's hand-off (with a back-off) when the
// executor queue was full. One flag read otherwise.
void bootDebugLoopTick();
// True while the capture wants every output forced (debugverbose off keeps it).
bool bootDebugForceWanted();
// True until bootDebugFinish() ran (also with the setting off or no memory):
// the C6 console mirror's boot window.
bool bootDebugInProgress();

// System_Debug.cpp's line path while gBootDebugCapturing: one formatted line,
// `shown` when the saved settings would have shown it anyway, `formatStartUs`
// the esp_timer time before the line was formatted for the capture (0: not
// counted). Not from an ISR (it skips).
void bootDebugCaptureLine(const char* text, size_t len, bool shown, int64_t formatStartUs);
// A forced line whose gate does not go through the line path (BLE_DEBUGF, the
// task-create trace, the [PERM] lines while their sub-flag is off): the boot
// file only. Nothing outside the capture.
void bootDebugCaptureText(const char* text);
void bootDebugCaptureF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// The firmware's ESP-IDF tag levels. Outside the capture these are
// esp_log_level_set / esp_log_level_get. During it a set is recorded (the tag
// stays at the default level, so its lines reach the hook) and a get answers
// from the record; bootDebugFinish() replays the record.
void bootDebugEspLogSet(const char* tag, esp_log_level_t level);
esp_log_level_t bootDebugEspLogGet(const char* tag);
// The esp_log vprintf hook (System_Debug.cpp) asks this while it is true.
bool bootDebugEspLogWanted();
// One formatted ESP-IDF line (no trailing newline): captured while the capture
// runs. Returns whether the saved levels would show it (print it then).
bool bootDebugEspLogLine(const char* text, size_t len, int64_t formatStartUs);
// The hook's formatting line, from the capture's own allocation (no line
// buffer on the logging task's stack): nullptr when another task holds it or
// the buffer is gone (the hook then formats on its own stack). Give it back
// with bootDebugScratchGive() as soon as the line is captured.
char* bootDebugScratchTake(size_t* cap);
void bootDebugScratchGive();
// System_Debug.cpp: installs the esp_log vprintf hook while `loglink` or the
// capture wants it, and puts the previous one back when neither does.
void idfLogHookRefresh();

// `bootdebug [on|off]`: the setting, and what this boot's capture did.
const char* cmd_bootdebug(const String& argsInput);

#endif  // SYSTEM_BOOT_DEBUG_H
