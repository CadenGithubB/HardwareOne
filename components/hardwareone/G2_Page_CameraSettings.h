#ifndef G2_PAGE_CAMERASETTINGS_H
#define G2_PAGE_CAMERASETTINGS_H

// =============================================================================
// G2 glasses — "Camera Settings" sub-page
// =============================================================================
// Reached by drilling from Sensors → CAM → "Settings >". Hidden from the
// top-level hijack menu (registered with hijackLabel=nullptr) — it's
// navigated to programmatically.
//
// Layout: one tappable row per exposed setting. Each tap cycles the
// targeted value through its valid range (with wrap), persists via
// setSetting()-equivalent path, and applies live to the camera sensor
// where supported (most settings apply without a restart; framesize
// triggers an init/stop cycle internally).
//
//   <- Camera
//   Resolution: QVGA
//   Brightness: 0
//   Contrast: 0
//   Exposure: 0
//   Sharpness: 0
//   Denoise: 0
//   H Mirror: OFF
//   V Flip: OFF
//   Quality: 12
//
// Saturation, White Balance, and Special Effect are intentionally
// omitted — the lens is 4-bpp grayscale so those settings affect the
// underlying capture but produce no visible change in the viewer. Use
// the web UI for those if needed.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_CAMERA_SENSOR

void g2BuildCameraSettingsInfo(char* out, size_t cap);
void g2ShowCameraSettingsMenu();
void g2CameraSettingsHandleTap(uint32_t idx);

#else
inline void g2BuildCameraSettingsInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline void g2ShowCameraSettingsMenu() {}
inline void g2CameraSettingsHandleTap(uint32_t idx) { (void)idx; }
#endif

#endif  // G2_PAGE_CAMERASETTINGS_H
