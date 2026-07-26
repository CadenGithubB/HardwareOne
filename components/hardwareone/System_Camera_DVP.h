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
extern bool gCameraRunning;
extern bool cameraConnected;
extern bool cameraStreaming;

// Camera info
extern const char* cameraModel;
extern int cameraWidth;
extern int cameraHeight;

// Camera initialization.
// isRecovery=true suppresses the durable [EVENT][CAM] "online" line — used by the
// per-frame capture-recovery path so a glitchy camera doesn't emit a lifecycle
// "online" event on every re-init (see captureFrame).
bool initCamera(bool isRecovery = false);
// isRecovery mirrors initCamera's flag: the per-frame glitch-recovery cycle
// (stop + re-init mid-stream) suppresses the sensor_stopped bus event so a
// flaky stream doesn't spam stop events with no paired start.
void stopCamera(bool isRecovery = false);

// Stack-heavy power transitions run on a dedicated worker (see System_Camera_DVP.cpp).
// G2 tap path uses *_Async; CLI / web use *_Sync so callers block until done.
typedef void (*CameraPowerPostHook)(void);
void cameraPowerSetPostHook(CameraPowerPostHook hook);
// Spawn cam_pwr task + queue early (e.g. G2 init) so the first tap does not
// pay xTaskCreate on the tap worker stack.
void cameraPowerWorkerEnsureStarted();
bool cameraPowerRequestStartAsync();
bool cameraPowerRequestStopAsync();
bool cameraPowerRequestStartSync(uint32_t waitMs);
void cameraPowerRequestStopSync(uint32_t waitMs);
bool cameraPowerRequestRestartSync(uint32_t waitMs);

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
const char* cmd_camera(const String& argsInput);
const char* cmd_camerastart(const String& argsInput);
const char* cmd_camerastop(const String& argsInput);
const char* cmd_cameracapture(const String& argsInput);
const char* cmd_camerares(const String& argsInput);
const char* cmd_cameraquality(const String& argsInput);
const char* cmd_cameratiny(const String& argsInput);
const char* cmd_camerabrightness(const String& argsInput);
const char* cmd_cameracontrast(const String& argsInput);
const char* cmd_camerasaturation(const String& argsInput);
const char* cmd_camerawb(const String& argsInput);
const char* cmd_camerasharpness(const String& argsInput);
const char* cmd_cameradenoise(const String& argsInput);
const char* cmd_cameraeffect(const String& argsInput);
const char* cmd_camerahmirror(const String& argsInput);
const char* cmd_cameravflip(const String& argsInput);
const char* cmd_cameraexposure(const String& argsInput);
const char* cmd_cameraframesize(const String& argsInput);

// Command registry
struct CommandEntry;
extern const CommandEntry cameraCommands[];
extern const size_t cameraCommandsCount;

// Settings module
struct SettingsModule;
extern const SettingsModule cameraSettingsModule;

#endif // ENABLE_CAMERA_SENSOR

#endif // SYSTEM_CAMERA_DVP_H
