#ifndef SYSTEM_EDGE_IMPULSE_H
#define SYSTEM_EDGE_IMPULSE_H

#include "System_BuildConfig.h"

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
//   - Runtime model loading from /littlefs/models/
//   - Single-shot inference via CLI command
//   - Continuous inference mode with configurable interval
//   - Configurable confidence threshold
//   - JSON output of detections for web/SSE consumption
//
// Usage:
//   1. Train a FOMO model on Edge Impulse Studio
//   2. Export as TensorFlow Lite (.tflite) and upload to /littlefs/models/
//   3. Load model: ei model load mymodel.tflite
//   4. Enable: ei enable 1
//   5. Run inference: ei detect
// ============================================================================

// Detection result structure
struct EIDetection {
  const char* label;
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

// Run single inference on current camera frame
// Returns detection results
EIResults runEdgeImpulseInference();

// Start/stop continuous inference mode
void startContinuousInference();
void stopContinuousInference();
bool isContinuousInferenceRunning();

// Get last detection results (for SSE/web)
const EIResults& getLastDetectionResults();

// Build JSON from detection results
void buildDetectionJson(const EIResults& results, String& output);

// Check if Edge Impulse model is loaded
bool isEdgeImpulseModelLoaded();

// ============================================================================
// Model Management API
// ============================================================================

// Load a .tflite model from LittleFS
bool loadModelFromFile(const char* path);

// Unload current model and free resources
void unloadModel();

// Check if a model is loaded
bool isModelLoaded();

// Get path of currently loaded model
const char* getLoadedModelPath();

// List available models in /littlefs/models/
void listAvailableModels(String& output);

// ============================================================================
// State Change Tracking API
// ============================================================================

// Tracked object structure (forward declaration for external use)
struct TrackedObject;

// State change callback type
typedef void (*StateChangeCallback)(const char* objectLabel, const char* prevState, const char* newState, int x, int y);

// Set callback for state change events
void setStateChangeCallback(StateChangeCallback callback);

// Enable/disable state tracking
void setStateTrackingEnabled(bool enabled);

// Get tracked object count and access
int getTrackedObjectCount();
const TrackedObject* getTrackedObject(int index);

// Build JSON output for tracked objects
void buildStateChangeJson(String& output);

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
const char* cmd_ei(const String& cmd);
const char* cmd_ei_enable(const String& cmd);
const char* cmd_ei_detect(const String& cmd);
const char* cmd_ei_continuous(const String& cmd);
const char* cmd_ei_confidence(const String& cmd);
const char* cmd_ei_status(const String& cmd);

// Web handler registration
#include <esp_http_server.h>
void registerEdgeImpulseHandlers(httpd_handle_t server);

#else

// Stub when Edge Impulse is disabled
#include <esp_http_server.h>
inline void registerEdgeImpulseHandlers(httpd_handle_t server) { (void)server; }

#endif // ENABLE_EDGE_IMPULSE
#endif // SYSTEM_EDGE_IMPULSE_H
