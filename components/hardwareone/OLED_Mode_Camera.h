#pragma once
#include "System_BuildConfig.h"
#include <Arduino.h>
#if ENABLE_OLED_DISPLAY && ENABLE_CAMERA_SENSOR
// Owner-loop lifecycle tick; worker results are fenced to this view/session.
void prepareCameraPreviewData();
void resetOLEDCameraPreview();
const char* cmd_oledcameraread(const String& args);
#else
inline void prepareCameraPreviewData() {}
inline void resetOLEDCameraPreview() {}
inline const char* cmd_oledcameraread(const String&) { return "Error: Camera preview is not built."; }
#endif
