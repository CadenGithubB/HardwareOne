#ifndef G2_PAGE_SENSORS_H
#define G2_PAGE_SENSORS_H

// =============================================================================
// G2 glasses — "Sensors" page
// =============================================================================
// One of the per-screen modules rendered on the Even Realities G2 lens.
// Follows the compartmentalization pattern established by OLED_Mode_*.cpp
// and WebPage_*.cpp — each G2 page gets its own file so the core G2
// infrastructure (Optional_EvenG2.cpp) stays focused on transport /
// lifecycle concerns, not content.
//
// This page enumerates the device's sensor registry and reports each one's
// compile-time + runtime state:
//   [ENABLED] sensor — compiled IN and probe succeeded (hardware present)
//   [off]     sensor — compiled IN but probe failed (expected hardware missing)
//   [stub]    sensor — compiled OUT (ENABLE_X=0 at build time)
//
// Output is caller-sized text ready for g2ShowText() or g2ShowNotification().
//
// Example rendered content (tight format for 576x288 lens):
//   Sensors (3/10)
//   IMU   BNO055    on
//   TOF   VL53L4CX  off
//   THERM MLX90640  stub
//   RTC   DS3231    stub
//   ...
//
// Build-guarded behind ENABLE_BLUETOOTH + ENABLE_G2_GLASSES so lean builds
// don't drag in the formatter. When the gates are off, the declarations
// become no-op stubs so any caller using them compiles cleanly.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// Fill `out` with a newline-separated, display-ready sensor list. Caller
// owns the buffer; 512 bytes is comfortable for the current ~10-sensor
// roster. Output is always null-terminated; truncation is silent but
// safe.
void g2BuildSensorList(char* out, size_t cap);

// Convenience wrapper: build the list and push via g2ShowText() in one
// call. Returns what g2ShowText returns (true = sent to the lens).
// Use this for the `g2sensors` CLI path and for the hijack menu tap
// handler — both need identical behaviour so centralising here keeps
// the page self-contained.
bool g2ShowSensorList();

#else  // !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)

inline void g2BuildSensorList(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline bool g2ShowSensorList() { return false; }

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif  // G2_PAGE_SENSORS_H
