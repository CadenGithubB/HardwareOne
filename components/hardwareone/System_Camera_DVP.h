/**
 * Camera Sensor Module - ESP32-S3 DVP Camera Support
 * 
 * Supports OV2640, OV3660, and OV5640 cameras on XIAO ESP32S3 Sense
 * Uses ESP32-S3 DVP (Digital Video Port) parallel interface, not I2C.
 */

#ifndef SYSTEM_CAMERA_DVP_H
#define SYSTEM_CAMERA_DVP_H

#include <Arduino.h>
#include "System_BuildConfig.h"

#if ENABLE_CAMERA_SENSOR

// Camera sensor state
extern bool cameraEnabled;
extern bool cameraConnected;
extern bool cameraStreaming;

// Camera info
extern const char* cameraModel;
extern int cameraWidth;
extern int cameraHeight;

// Camera initialization
bool initCamera();
void stopCamera();

// Capture a single frame (returns JPEG data)
// Caller must free the buffer with free() when done
uint8_t* captureFrame(size_t* outLen);

// Resolution and quality control
#include "esp_camera.h"
bool setCameraResolution(framesize_t size);
bool setCameraQuality(int quality);

// Capture at specific resolution (for ESP-NOW: use FRAMESIZE_QQVGA)
uint8_t* captureFrameAtResolution(framesize_t size, int quality, size_t* outLen);

// Capture tiny frame for ESP-NOW (160x120, high compression)
uint8_t* captureTinyFrame(size_t* outLen);

// Get camera status JSON
const char* buildCameraStatusJson();

// Command handlers
const char* cmd_camera(const String& cmd);
const char* cmd_camerastart(const String& cmd);
const char* cmd_camerastop(const String& cmd);
const char* cmd_cameracapture(const String& cmd);
const char* cmd_camerares(const String& cmd);
const char* cmd_cameraquality(const String& cmd);
const char* cmd_cameratiny(const String& cmd);
const char* cmd_camerabrightness(const String& cmd);
const char* cmd_cameracontrast(const String& cmd);
const char* cmd_camerasaturation(const String& cmd);
const char* cmd_camerawb(const String& cmd);
const char* cmd_camerasharpness(const String& cmd);
const char* cmd_cameradenoise(const String& cmd);
const char* cmd_cameraeffect(const String& cmd);
const char* cmd_camerahmirror(const String& cmd);
const char* cmd_cameravflip(const String& cmd);

// Command registry
struct CommandEntry;
extern const CommandEntry cameraCommands[];
extern const size_t cameraCommandsCount;

// Settings module
struct SettingsModule;
extern const SettingsModule cameraSettingsModule;

#endif // ENABLE_CAMERA_SENSOR

#endif // SYSTEM_CAMERA_DVP_H
