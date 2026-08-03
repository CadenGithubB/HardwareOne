#ifndef G2_PAGE_SENSORS_H
#define G2_PAGE_SENSORS_H

// =============================================================================
// G2 glasses — "Sensors" page
// =============================================================================
// One of the per-screen modules rendered on the Even Realities G2 lens.
// Follows the compartmentalization pattern established by OLED_Mode_*.cpp
// and WebPage_*.cpp — each G2 page gets its own file so the core G2
// infrastructure (G2_Glasses.cpp) stays focused on transport /
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

// On-glasses interactive Sensors page: show a list with one row per
// COMPILED-IN sensor (non-compiled rows are filtered out entirely).
// Tapping a row drills into a detail sub-menu (back / auto-start
// toggle / live value). Used as the kSensorsPage showMenu hook.
void g2ShowSensorsMenu();

// Re-render the sensor detail page for whichever sensor was last
// drilled into (uses the module-internal gSensorsDetailIdx cache).
// Used by sub-pages — Camera Settings, future per-sensor flows —
// that want to land the user back on the originating detail page
// when they tap "<- Back" rather than dropping all the way to the
// sensors landing list.
void g2ReshowSensorsDetail();

// Multi-line LIVE readout for the sensor currently drilled into. Consumed by
// renderSensorDetailLive() in G2_Glasses.cpp (the live-compound transport),
// refreshed every tick. Caller owns the buffer; always null-terminated.
void g2BuildSensorReadout(char* out, size_t cap);

// Build the LIVE sensor-detail compound's list rows (row 0 = back, row 1 =
// Auto-Start toggle — index order matches the DETAIL tap handler). Pointers
// reference shared static buffers, valid until the next call. Returns count.
size_t g2BuildSensorLiveList(const char** outRows, size_t maxRows);

#if ENABLE_CAMERA_SENSOR
// Install post-hook for Sensors detail redraw after async camera power ops
// (call once from G2 init). Does not start cam_pwr — that is lazy on first use.
void g2RegisterSensorsCameraPowerHook();
#endif

// Tap dispatcher for the Sensors landing list and its sub-menu. Routes
// based on which level we're at (gSensorsLevel internal to the
// implementation).
void g2SensorsHandleTap(uint32_t idx);

#else  // !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)

inline void g2BuildSensorList(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline bool g2ShowSensorList() { return false; }
inline void g2ShowSensorsMenu() {}
inline void g2ReshowSensorsDetail() {}
inline void g2BuildSensorReadout(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline size_t g2BuildSensorLiveList(const char**, size_t) { return 0; }
#if ENABLE_CAMERA_SENSOR
inline void g2RegisterSensorsCameraPowerHook() {}
#endif
inline void g2SensorsHandleTap(uint32_t /*idx*/) {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif  // G2_PAGE_SENSORS_H
