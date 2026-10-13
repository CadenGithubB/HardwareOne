#ifndef SYSTEM_EDGEIMPULSE_H
#define SYSTEM_EDGEIMPULSE_H

#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

#if ENABLE_EDGE_IMPULSE

#include <Arduino.h>
#include "System_Command.h"
#include "System_Settings.h"

// ============================================================================
// Edge Impulse Object Detection Module
// ============================================================================
// Provides ML inference using TensorFlow Lite Micro for object detection.
// Models are loaded from LittleFS at runtime (.tflite files).
//
// Features:
//   - Runtime model loading from /littlefs/EI Models/
//   - Single-shot inference via CLI command
//   - Continuous inference mode with configurable interval
//   - Configurable confidence threshold
//   - JSON output of detections for web/SSE consumption
//
// Usage:
//   1. Train a FOMO model on Edge Impulse Studio
//   2. Export as TensorFlow Lite (.tflite) and upload to /littlefs/EI Models/
//   3. Load model: ei model load mymodel.tflite
//   4. Enable: ei enable 1
//   5. Run inference: ei detect
// ============================================================================

// Longest class label kept, including the terminator (longer lines in a
// labels file are truncated).
#define EI_LABEL_MAX 32

// Detection result structure. Each detection owns a copy of its label, so it
// stays valid after the next inference or a model change.
struct EIDetection {
  char label[EI_LABEL_MAX];
  float confidence;
  int x;
  int y;
  int width;
  int height;
};

// Maximum detections per frame
#define EI_MAX_DETECTIONS 10

// Detection results structure
struct EIResults {
  bool success;
  int detectionCount;
  EIDetection detections[EI_MAX_DETECTIONS];
  uint32_t inferenceTimeMs;
  const char* errorMessage;
};

// ============================================================================
// Public API
// ============================================================================

// Initialize Edge Impulse module
void initEdgeImpulse();

// Run single inference on current camera frame and update tracked objects.
// Returns a "busy" error instead of waiting when another inference, a model
// load or an unload is in progress.
EIResults runEdgeImpulseInference();

// Run single inference on a stored JPEG file (same pipeline as the camera;
// does not update tracked objects). Busy rule as above.
EIResults runInferenceFromFile(const char* imagePath);

// Start/stop continuous inference mode. Stop returns once the continuous task
// has finished its current frame and exited.
bool startContinuousInference();  // returns true if running on exit, false if it could not start
void stopContinuousInference();
bool isContinuousInferenceRunning();

// Build JSON from detection results
void buildDetectionJson(const EIResults& results, String& output);

// Check if Edge Impulse model is loaded
bool isEdgeImpulseModelLoaded();

// ============================================================================
// Model Management API
// ============================================================================

// Load a .tflite model from LittleFS. The model's input tensor sets the input
// width, height and channels. Waits for a running inference; continuous mode
// is paused for the swap and resumed when the new model loads.
bool loadModelFromFile(const char* path);

// Stop continuous mode, wait for a running inference, then free the model.
void unloadModel();

// List available models in /littlefs/EI Models/
void listAvailableModels(String& output);

// ============================================================================
// State Change Tracking API
// ============================================================================

// Enable/disable state tracking
void setStateTrackingEnabled(bool enabled);

// ============================================================================
// Settings Module (extern declaration)
// ============================================================================
extern const SettingsModule edgeImpulseSettingsModule;

// ============================================================================
// CLI Commands
// ============================================================================
extern const CommandEntry edgeImpulseCommands[];
extern const size_t edgeImpulseCommandsCount;

// Command handlers
const char* cmd_ei(const String& argsInput);
const char* cmd_ei_enable(const String& argsInput);
const char* cmd_ei_detect(const String& argsInput);
const char* cmd_ei_continuous(const String& argsInput);
const char* cmd_ei_confidence(const String& argsInput);
const char* cmd_ei_status(const String& argsInput);

// Web handler registration
#if ENABLE_HTTP_SERVER
void registerEdgeImpulseHandlers(httpd_handle_t server);
#else
inline void registerEdgeImpulseHandlers(httpd_handle_t server) { (void)server; }
#endif

#else

// Stub when Edge Impulse is disabled
inline void registerEdgeImpulseHandlers(httpd_handle_t server) { (void)server; }

#endif // ENABLE_EDGE_IMPULSE
#endif // SYSTEM_EDGEIMPULSE_H
