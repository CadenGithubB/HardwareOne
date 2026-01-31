#include "System_EdgeImpulse.h"

#if ENABLE_EDGE_IMPULSE

#include "System_Settings.h"
#include "System_Debug.h"
#include "System_Camera_DVP.h"
#include "esp_camera.h"
#include "img_converters.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

// SSE status broadcast
extern void sensorStatusBumpWith(const char* reason);

// TensorFlow Lite Micro includes
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ============================================================================
// TFLite Micro Configuration
// ============================================================================

// Arena size for TFLite interpreter (adjust based on model requirements)
// Using 1.25MB for 240x240 FOMO models (requires ~1.1MB)
constexpr size_t kTensorArenaSize = 1280 * 1024;  // 1.25MB

// Maximum model file size to load
constexpr size_t kMaxModelSize = 1024 * 1024;  // 1MB

// Model storage path (relative to LittleFS mount point)
static const char* MODEL_DIR = "/EI Models";
static const char* DEFAULT_MODEL = "/EI Models/default.tflite";

// ============================================================================
// Edge Impulse Module State
// ============================================================================

static bool gEIInitialized = false;
static bool gEIModelLoaded = false;
static bool gEIContinuousRunning = false;
static EIResults gLastResults = {false, 0, {}, 0, nullptr};
static TaskHandle_t gEIContinuousTask = nullptr;

// Image buffers (allocated in PSRAM if available)
static uint8_t* gRgbBuffer = nullptr;      // Full-size RGB888 buffer
static uint8_t* gResizedBuffer = nullptr;  // Resized buffer for model input
static size_t gRgbBufferSize = 0;
static size_t gResizedBufferSize = 0;

// TFLite Micro state
static uint8_t* gModelBuffer = nullptr;           // Loaded .tflite model data
static size_t gModelSize = 0;                     // Size of loaded model
static uint8_t* gTensorArena = nullptr;           // TFLite tensor arena
static const tflite::Model* gTflModel = nullptr;  // Parsed model
static tflite::MicroInterpreter* gInterpreter = nullptr;
static TfLiteTensor* gInputTensor = nullptr;
static TfLiteTensor* gOutputTensor = nullptr;
static String gLoadedModelPath;                   // Path of currently loaded model
static int gModelInputWidth = 0;
static int gModelInputHeight = 0;
static int gModelInputChannels = 0;

// Label names (extracted from labels.txt or configured)
static char* gModelLabels[EI_MAX_DETECTIONS] = {nullptr};
static int gModelLabelCount = 0;

// Free label memory
static void freeLabels() {
  for (int i = 0; i < EI_MAX_DETECTIONS; i++) {
    if (gModelLabels[i]) {
      free(gModelLabels[i]);
      gModelLabels[i] = nullptr;
    }
  }
  gModelLabelCount = 0;
}

static bool loadLabelsFromExplicitPath(const String& labelsPath) {
  if (!LittleFS.exists(labelsPath.c_str())) {
    return false;
  }

  File labelsFile = LittleFS.open(labelsPath.c_str(), "r");
  if (!labelsFile) {
    return false;
  }

  while (labelsFile.available() && gModelLabelCount < EI_MAX_DETECTIONS) {
    String line = labelsFile.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      gModelLabels[gModelLabelCount] = strdup(line.c_str());
      DEBUG_SYSTEMF("[EI_DEBUG] Label[%d] = '%s'", gModelLabelCount, gModelLabels[gModelLabelCount]);
      gModelLabelCount++;
    }
  }
  labelsFile.close();
  return gModelLabelCount > 0;
}

// Load labels file for model
// Expected structure: /EI Models/<modelname>/<modelname>.tflite + <modelname>.labels.txt
static bool loadLabelsFromFile(const char* modelPath) {
  freeLabels();
  
  String model = String(modelPath);
  int lastSlash = model.lastIndexOf('/');
  String modelDir = (lastSlash >= 0) ? model.substring(0, lastSlash) : String(MODEL_DIR);
  String modelFile = (lastSlash >= 0) ? model.substring(lastSlash + 1) : model;
  
  // Extract model stem (filename without .tflite)
  String modelStem = modelFile;
  if (modelStem.endsWith(".tflite")) {
    modelStem.remove(modelStem.length() - 7);
  }

  // Labels file must be: <modeldir>/<modelname>.labels.txt
  String labelsPath = modelDir + "/" + modelStem + ".labels.txt";
  DEBUG_SYSTEMF("[EI] Looking for labels: %s", labelsPath.c_str());
  
  if (loadLabelsFromExplicitPath(labelsPath)) {
    DEBUG_SYSTEMF("[EI] Loaded %d labels from %s", gModelLabelCount, labelsPath.c_str());
    return true;
  }

  ERROR_SYSTEMF("[EI] Labels file not found: %s", labelsPath.c_str());
  return false;
}

// Get label for output index (returns generic if not found)
static const char* getLabelForIndex(int index) {
  if (index >= 0 && index < gModelLabelCount && gModelLabels[index]) {
    return gModelLabels[index];
  }
  // Return generic label with index
  static char genericLabel[32];
  snprintf(genericLabel, sizeof(genericLabel), "class_%d", index);
  return genericLabel;
}

// ============================================================================
// Forward Declarations
// ============================================================================

static bool allocateImageBuffers(int inputSize);

// ============================================================================
// State Change Tracking
// ============================================================================

// Tracked object state
struct TrackedObject {
  char label[32];           // Current state/label
  char prevLabel[32];       // Previous state/label
  float confidence;         // Current confidence
  int x, y, width, height;  // Location
  uint32_t lastSeenMs;      // Last detection timestamp
  uint32_t stateChangeMs;   // When state last changed
  bool stateChanged;        // Flag for recent state change
  int stableCount;          // Frames at current state
};

#define MAX_TRACKED_OBJECTS 5
#define STATE_STABLE_FRAMES 3   // Require N consecutive frames to confirm state
#define OBJECT_TIMEOUT_MS 2000  // Object considered gone after this

static TrackedObject gTrackedObjects[MAX_TRACKED_OBJECTS];
static int gTrackedObjectCount = 0;
static bool gStateTrackingEnabled = true;

// State change callback type
typedef void (*StateChangeCallback)(const char* objectLabel, const char* prevState, const char* newState, int x, int y);
static StateChangeCallback gStateChangeCallback = nullptr;

// Find tracked object by approximate location (within tolerance)
static TrackedObject* findTrackedObject(int x, int y, int tolerance = 20) {
  for (int i = 0; i < gTrackedObjectCount; i++) {
    int dx = abs(gTrackedObjects[i].x - x);
    int dy = abs(gTrackedObjects[i].y - y);
    if (dx <= tolerance && dy <= tolerance) {
      return &gTrackedObjects[i];
    }
  }
  return nullptr;
}

// Extract base object name from label (e.g., "device_led_red" -> "device")
static void extractBaseName(const char* label, char* baseName, size_t maxLen) {
  strncpy(baseName, label, maxLen - 1);
  baseName[maxLen - 1] = '\0';
  
  // Find last underscore and truncate there (simple heuristic)
  char* lastUnderscore = strrchr(baseName, '_');
  if (lastUnderscore && lastUnderscore != baseName) {
    // Check if what follows looks like a state (red, green, on, off, etc.)
    const char* states[] = {"red", "green", "blue", "on", "off", "active", "inactive", nullptr};
    for (int i = 0; states[i]; i++) {
      if (strcasecmp(lastUnderscore + 1, states[i]) == 0) {
        *lastUnderscore = '\0';
        return;
      }
    }
  }
}

// Update tracked objects with new detections
static void updateTrackedObjects(const EIResults& results) {
  uint32_t now = millis();
  
  // Mark all as potentially stale
  for (int i = 0; i < gTrackedObjectCount; i++) {
    gTrackedObjects[i].stateChanged = false;
  }
  
  // Process new detections
  for (int d = 0; d < results.detectionCount; d++) {
    const EIDetection& det = results.detections[d];
    
    // Find existing tracked object at this location
    TrackedObject* tracked = findTrackedObject(det.x, det.y);
    
    if (tracked) {
      // Update existing object
      tracked->lastSeenMs = now;
      tracked->x = det.x;
      tracked->y = det.y;
      tracked->width = det.width;
      tracked->height = det.height;
      tracked->confidence = det.confidence;
      
      // Check for state change
      if (strcmp(tracked->label, det.label) != 0) {
        // Label changed - check if stable
        tracked->stableCount++;
        if (tracked->stableCount >= STATE_STABLE_FRAMES) {
          // Confirmed state change
          strncpy(tracked->prevLabel, tracked->label, sizeof(tracked->prevLabel) - 1);
          strncpy(tracked->label, det.label, sizeof(tracked->label) - 1);
          tracked->stateChangeMs = now;
          tracked->stateChanged = true;
          tracked->stableCount = 0;
          
          DEBUG_SYSTEMF("[EdgeImpulse] State change: %s -> %s at (%d,%d)",
                        tracked->prevLabel, tracked->label, tracked->x, tracked->y);
          
          // Call callback if registered
          if (gStateChangeCallback) {
            char baseName[32];
            extractBaseName(tracked->label, baseName, sizeof(baseName));
            gStateChangeCallback(baseName, tracked->prevLabel, tracked->label, tracked->x, tracked->y);
          }
        }
      } else {
        // Same label - reset stability counter
        tracked->stableCount = 0;
      }
    } else if (gTrackedObjectCount < MAX_TRACKED_OBJECTS) {
      // New object - add to tracking
      TrackedObject* newObj = &gTrackedObjects[gTrackedObjectCount++];
      strncpy(newObj->label, det.label, sizeof(newObj->label) - 1);
      newObj->label[sizeof(newObj->label) - 1] = '\0';
      newObj->prevLabel[0] = '\0';
      newObj->confidence = det.confidence;
      newObj->x = det.x;
      newObj->y = det.y;
      newObj->width = det.width;
      newObj->height = det.height;
      newObj->lastSeenMs = now;
      newObj->stateChangeMs = now;
      newObj->stateChanged = false;
      newObj->stableCount = 0;
      
      DEBUG_SYSTEMF("[EdgeImpulse] New tracked object: %s at (%d,%d)", 
                    newObj->label, newObj->x, newObj->y);
    }
  }
  
  // Remove stale objects
  for (int i = gTrackedObjectCount - 1; i >= 0; i--) {
    if (now - gTrackedObjects[i].lastSeenMs > OBJECT_TIMEOUT_MS) {
      DEBUG_SYSTEMF("[EdgeImpulse] Object lost: %s at (%d,%d)",
                    gTrackedObjects[i].label, gTrackedObjects[i].x, gTrackedObjects[i].y);
      // Shift remaining objects down
      for (int j = i; j < gTrackedObjectCount - 1; j++) {
        gTrackedObjects[j] = gTrackedObjects[j + 1];
      }
      gTrackedObjectCount--;
    }
  }
}

// Public: Set state change callback
void setStateChangeCallback(StateChangeCallback callback) {
  gStateChangeCallback = callback;
}

// Public: Enable/disable state tracking
void setStateTrackingEnabled(bool enabled) {
  gStateTrackingEnabled = enabled;
  if (!enabled) {
    gTrackedObjectCount = 0;
  }
}

// Public: Get tracked objects
int getTrackedObjectCount() {
  return gTrackedObjectCount;
}

const TrackedObject* getTrackedObject(int index) {
  if (index >= 0 && index < gTrackedObjectCount) {
    return &gTrackedObjects[index];
  }
  return nullptr;
}

// Build JSON for state changes
void buildStateChangeJson(String& output) {
  JsonDocument doc;
  
  JsonArray objects = doc["trackedObjects"].to<JsonArray>();
  for (int i = 0; i < gTrackedObjectCount; i++) {
    JsonObject obj = objects.add<JsonObject>();
    obj["label"] = gTrackedObjects[i].label;
    obj["prevLabel"] = gTrackedObjects[i].prevLabel;
    obj["confidence"] = gTrackedObjects[i].confidence;
    obj["x"] = gTrackedObjects[i].x;
    obj["y"] = gTrackedObjects[i].y;
    obj["width"] = gTrackedObjects[i].width;
    obj["height"] = gTrackedObjects[i].height;
    obj["stateChanged"] = gTrackedObjects[i].stateChanged;
    obj["lastSeenMs"] = gTrackedObjects[i].lastSeenMs;
    obj["stateChangeMs"] = gTrackedObjects[i].stateChangeMs;
  }
  
  serializeJson(doc, output);
}

// ============================================================================
// Settings Module Definition
// ============================================================================

static const SettingEntry edgeImpulseSettingEntries[] = {
  { "enabled", SETTING_BOOL, &gSettings.edgeImpulseEnabled, 0, 0, nullptr, 0, 1, "Enable Inference", nullptr },
  { "requireLabels", SETTING_BOOL, &gSettings.edgeImpulseRequireLabels, 1, 0, nullptr, 0, 1, "Require Labels", nullptr },
  { "minConfidence", SETTING_FLOAT, &gSettings.edgeImpulseMinConfidence, 0, 0.6f, nullptr, 0, 1, "Min Confidence", nullptr },
  { "maxDetections", SETTING_INT, &gSettings.edgeImpulseMaxDetections, 5, 0, nullptr, 1, 10, "Max Detections", nullptr },
  { "inputSize", SETTING_INT, &gSettings.edgeImpulseInputSize, 96, 0, nullptr, 48, 320, "Input Size", nullptr },
  { "continuous", SETTING_BOOL, &gSettings.edgeImpulseContinuous, 0, 0, nullptr, 0, 1, "Continuous Mode", nullptr },
  { "intervalMs", SETTING_INT, &gSettings.edgeImpulseIntervalMs, 1000, 0, nullptr, 100, 10000, "Interval (ms)", nullptr }
};

extern const SettingsModule edgeImpulseSettingsModule = {
  "edgeimpulse",
  "edgeimpulse",
  edgeImpulseSettingEntries,
  sizeof(edgeImpulseSettingEntries) / sizeof(edgeImpulseSettingEntries[0]),
  nullptr,
  "Edge Impulse ML object detection settings"
};

// ============================================================================
// TFLite Micro Model Loading
// ============================================================================

// Op resolver with common ops for image classification/detection models
// Increased to 20 ops to support more Edge Impulse model types
static tflite::MicroMutableOpResolver<20>* gOpResolver = nullptr;

static bool setupOpResolver() {
  if (gOpResolver) return true;
  
  DEBUG_SYSTEMF("[EI_DEBUG] Setting up op resolver with 20 ops capacity...");
  
  gOpResolver = new tflite::MicroMutableOpResolver<20>();
  if (!gOpResolver) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate op resolver");
    return false;
  }
  
  // Add common ops used by FOMO and image classification models
  gOpResolver->AddConv2D();
  gOpResolver->AddDepthwiseConv2D();
  gOpResolver->AddFullyConnected();
  gOpResolver->AddReshape();
  gOpResolver->AddSoftmax();
  gOpResolver->AddMaxPool2D();
  gOpResolver->AddAveragePool2D();
  gOpResolver->AddQuantize();
  gOpResolver->AddDequantize();
  gOpResolver->AddMean();
  
  // Additional ops commonly used by Edge Impulse models
  gOpResolver->AddPad();           // Required for many EI models
  gOpResolver->AddPadV2();         // Variant of Pad
  gOpResolver->AddAdd();           // Element-wise add
  gOpResolver->AddMul();           // Element-wise multiply
  gOpResolver->AddRelu();          // Activation function
  gOpResolver->AddRelu6();         // Activation function
  gOpResolver->AddLogistic();      // Sigmoid activation
  gOpResolver->AddConcatenation(); // Tensor concatenation
  gOpResolver->AddSplit();         // Tensor split
  gOpResolver->AddSplitV();        // Tensor split variant
  
  DEBUG_SYSTEMF("[EI_DEBUG] Op resolver configured with 20 operations");
  
  return true;
}

static void freeModelResources() {
  DEBUG_SYSTEMF("[EI_DEBUG] freeModelResources() called");
  DEBUG_SYSTEMF("[EI_DEBUG]   Current state: interpreter=%p model=%p arena=%p", 
                gInterpreter, gModelBuffer, gTensorArena);
  
  if (gInterpreter) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Deleting interpreter...");
    delete gInterpreter;
    gInterpreter = nullptr;
  }
  gTflModel = nullptr;  // Points into gModelBuffer, don't delete
  gInputTensor = nullptr;
  gOutputTensor = nullptr;
  
  if (gModelBuffer) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Freeing model buffer (%zu bytes)...", gModelSize);
    free(gModelBuffer);
    gModelBuffer = nullptr;
  }
  gModelSize = 0;
  
  if (gTensorArena) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Freeing tensor arena (%zu bytes)...", kTensorArenaSize);
    free(gTensorArena);
    gTensorArena = nullptr;
  }
  
  gLoadedModelPath = "";
  gModelInputWidth = 0;
  gModelInputHeight = 0;
  gModelInputChannels = 0;
  gEIModelLoaded = false;
  
  // Free label memory
  freeLabels();
  
  DEBUG_SYSTEMF("[EI_DEBUG]   Heap after cleanup: %lu free", (unsigned long)ESP.getFreeHeap());
}

bool loadModelFromFile(const char* path) {
  DEBUG_SYSTEMF("[EI_DEBUG] ========== loadModelFromFile() START ==========");
  DEBUG_SYSTEMF("[EI_DEBUG] Path: %s", path);
  DEBUG_SYSTEMF("[EI_DEBUG] Heap before load: %lu free, PSRAM: %lu free",
                (unsigned long)ESP.getFreeHeap(), 
                (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
  
  // Free any existing model
  freeModelResources();
  
  // Open file
  File modelFile = LittleFS.open(path, "r");
  if (!modelFile) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to open model file: %s", path);
    return false;
  }
  
  // Check file size
  size_t fileSize = modelFile.size();
  DEBUG_SYSTEMF("[EI_DEBUG] File size: %zu bytes (max allowed: %zu)", fileSize, kMaxModelSize);
  
  if (fileSize == 0) {
    ERROR_SYSTEMF("[EdgeImpulse] Model file is empty");
    modelFile.close();
    return false;
  }
  if (fileSize > kMaxModelSize) {
    ERROR_SYSTEMF("[EdgeImpulse] Model too large: %zu bytes (max %zu)", fileSize, kMaxModelSize);
    modelFile.close();
    return false;
  }
  
  // Allocate model buffer in PSRAM if available
  DEBUG_SYSTEMF("[EI_DEBUG] Allocating model buffer: %zu bytes in %s",
                fileSize, psramFound() ? "PSRAM" : "DRAM");
  
  if (psramFound()) {
    gModelBuffer = (uint8_t*)ps_malloc(fileSize);
  } else {
    gModelBuffer = (uint8_t*)malloc(fileSize);
  }
  
  if (!gModelBuffer) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate model buffer (%zu bytes)", fileSize);
    DEBUG_SYSTEMF("[EI_DEBUG] Allocation FAILED! Heap: %lu, PSRAM: %lu",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
    modelFile.close();
    return false;
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Model buffer allocated at %p", gModelBuffer);
  
  // Read model data
  DEBUG_SYSTEMF("[EI_DEBUG] Reading model data from file...");
  uint32_t readStart = millis();
  size_t bytesRead = modelFile.read(gModelBuffer, fileSize);
  uint32_t readTime = millis() - readStart;
  modelFile.close();
  
  DEBUG_SYSTEMF("[EI_DEBUG] Read %zu bytes in %lu ms (%.1f KB/s)",
                bytesRead, readTime, 
                readTime > 0 ? (bytesRead / 1024.0f) / (readTime / 1000.0f) : 0);
  
  if (bytesRead != fileSize) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to read model: got %zu of %zu bytes", bytesRead, fileSize);
    free(gModelBuffer);
    gModelBuffer = nullptr;
    return false;
  }
  gModelSize = fileSize;
  
  // Parse the model
  DEBUG_SYSTEMF("[EI_DEBUG] Parsing TFLite model...");
  gTflModel = tflite::GetModel(gModelBuffer);
  if (!gTflModel) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to parse TFLite model");
    DEBUG_SYSTEMF("[EI_DEBUG] First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                  gModelBuffer[0], gModelBuffer[1], gModelBuffer[2], gModelBuffer[3],
                  gModelBuffer[4], gModelBuffer[5], gModelBuffer[6], gModelBuffer[7]);
    free(gModelBuffer);
    gModelBuffer = nullptr;
    return false;
  }
  
  // Check model version
  DEBUG_SYSTEMF("[EI_DEBUG] Model version: %lu (expected: %d)", 
                gTflModel->version(), TFLITE_SCHEMA_VERSION);
  
  if (gTflModel->version() != TFLITE_SCHEMA_VERSION) {
    ERROR_SYSTEMF("[EdgeImpulse] Model schema version mismatch: got %lu, expected %d",
                  gTflModel->version(), TFLITE_SCHEMA_VERSION);
    free(gModelBuffer);
    gModelBuffer = nullptr;
    gTflModel = nullptr;
    return false;
  }
  
  // Setup op resolver
  DEBUG_SYSTEMF("[EI_DEBUG] Setting up op resolver...");
  if (!setupOpResolver()) {
    free(gModelBuffer);
    gModelBuffer = nullptr;
    gTflModel = nullptr;
    return false;
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Op resolver ready");
  
  // Allocate tensor arena in PSRAM
  DEBUG_SYSTEMF("[EI_DEBUG] Allocating tensor arena: %zu KB in %s",
                kTensorArenaSize / 1024, psramFound() ? "PSRAM" : "DRAM");
  
  if (psramFound()) {
    gTensorArena = (uint8_t*)ps_malloc(kTensorArenaSize);
  } else {
    gTensorArena = (uint8_t*)malloc(kTensorArenaSize);
  }
  
  if (!gTensorArena) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate tensor arena (%zu bytes)", kTensorArenaSize);
    DEBUG_SYSTEMF("[EI_DEBUG] Arena allocation FAILED! PSRAM free: %lu",
                  (unsigned long)(psramFound() ? ESP.getFreePsram() : ESP.getFreeHeap()));
    free(gModelBuffer);
    gModelBuffer = nullptr;
    gTflModel = nullptr;
    return false;
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Tensor arena allocated at %p", gTensorArena);
  
  // Create interpreter
  DEBUG_SYSTEMF("[EI_DEBUG] Creating MicroInterpreter...");
  gInterpreter = new tflite::MicroInterpreter(
    gTflModel, *gOpResolver, gTensorArena, kTensorArenaSize
  );
  
  if (!gInterpreter) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to create interpreter");
    freeModelResources();
    return false;
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Interpreter created at %p", gInterpreter);
  
  // Allocate tensors
  DEBUG_SYSTEMF("[EI_DEBUG] Allocating tensors...");
  uint32_t allocStart = millis();
  TfLiteStatus allocStatus = gInterpreter->AllocateTensors();
  uint32_t allocTime = millis() - allocStart;
  
  if (allocStatus != kTfLiteOk) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate tensors (status=%d)", allocStatus);
    freeModelResources();
    return false;
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Tensors allocated in %lu ms", allocTime);
  DEBUG_SYSTEMF("[EI_DEBUG] Arena used: %zu / %zu bytes (%.1f%%)",
                gInterpreter->arena_used_bytes(), kTensorArenaSize,
                100.0f * gInterpreter->arena_used_bytes() / kTensorArenaSize);
  
  // Get input tensor info
  DEBUG_SYSTEMF("[EI_DEBUG] Getting input/output tensors...");
  DEBUG_SYSTEMF("[EI_DEBUG] Model has %zu inputs, %zu outputs",
                gInterpreter->inputs_size(), gInterpreter->outputs_size());
  
  gInputTensor = gInterpreter->input(0);
  if (!gInputTensor) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to get input tensor");
    freeModelResources();
    return false;
  }
  
  // Log input tensor details
  DEBUG_SYSTEMF("[EI_DEBUG] Input tensor: dims=%d type=%d bytes=%zu",
                gInputTensor->dims->size, gInputTensor->type, gInputTensor->bytes);
  for (int i = 0; i < gInputTensor->dims->size; i++) {
    DEBUG_SYSTEMF("[EI_DEBUG]   dim[%d] = %d", i, gInputTensor->dims->data[i]);
  }
  
  // Extract input dimensions (assuming NHWC format: [batch, height, width, channels])
  if (gInputTensor->dims->size >= 4) {
    gModelInputHeight = gInputTensor->dims->data[1];
    gModelInputWidth = gInputTensor->dims->data[2];
    gModelInputChannels = gInputTensor->dims->data[3];
    DEBUG_SYSTEMF("[EI_DEBUG] Detected NHWC format: %dx%dx%d",
                  gModelInputWidth, gModelInputHeight, gModelInputChannels);
  } else if (gInputTensor->dims->size == 3) {
    // Some models use [height, width, channels]
    gModelInputHeight = gInputTensor->dims->data[0];
    gModelInputWidth = gInputTensor->dims->data[1];
    gModelInputChannels = gInputTensor->dims->data[2];
    DEBUG_SYSTEMF("[EI_DEBUG] Detected HWC format: %dx%dx%d",
                  gModelInputWidth, gModelInputHeight, gModelInputChannels);
  } else {
    DEBUG_SYSTEMF("[EI_DEBUG] WARNING: Unexpected dims->size=%d", gInputTensor->dims->size);
  }
  
  // Get output tensor
  gOutputTensor = gInterpreter->output(0);
  if (gOutputTensor) {
    DEBUG_SYSTEMF("[EI_DEBUG] Output tensor: dims=%d type=%d bytes=%zu",
                  gOutputTensor->dims->size, gOutputTensor->type, gOutputTensor->bytes);
    for (int i = 0; i < gOutputTensor->dims->size; i++) {
      DEBUG_SYSTEMF("[EI_DEBUG]   dim[%d] = %d", i, gOutputTensor->dims->data[i]);
    }
    if (gOutputTensor->type == kTfLiteUInt8 || gOutputTensor->type == kTfLiteInt8) {
      DEBUG_SYSTEMF("[EI_DEBUG] Output quantization: scale=%.6f zero_point=%d",
                    gOutputTensor->params.scale, gOutputTensor->params.zero_point);
    }
  }
  
  gLoadedModelPath = path;
  gEIModelLoaded = true;
  
  // Try to load labels from labels.txt
  bool haveLabels = loadLabelsFromFile(path);
  if (gSettings.edgeImpulseRequireLabels && !haveLabels) {
    ERROR_SYSTEMF("[EdgeImpulse] Labels are required but no labels file was found for: %s", path);
    freeModelResources();
    return false;
  }
  
  DEBUG_SYSTEMF("[EI_DEBUG] ========== loadModelFromFile() SUCCESS ==========");
  DEBUG_SYSTEMF("[EI_DEBUG] Model: %s", path);
  DEBUG_SYSTEMF("[EI_DEBUG] Size: %zu bytes", gModelSize);
  DEBUG_SYSTEMF("[EI_DEBUG] Input: %dx%dx%d, type=%d", 
                gModelInputWidth, gModelInputHeight, gModelInputChannels,
                gInputTensor->type);
  DEBUG_SYSTEMF("[EI_DEBUG] Arena: %zu / %zu bytes (%.1f%% used)",
                gInterpreter->arena_used_bytes(), kTensorArenaSize,
                100.0f * gInterpreter->arena_used_bytes() / kTensorArenaSize);
  DEBUG_SYSTEMF("[EI_DEBUG] Heap after load: %lu free, PSRAM: %lu free",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
  
  // Update settings with detected input size and reallocate buffers if needed
  if (gModelInputWidth > 0 && gModelInputWidth != gSettings.edgeImpulseInputSize) {
    DEBUG_SYSTEMF("[EI_DEBUG] Model input size changed: %d -> %d, reallocating buffers...",
                  gSettings.edgeImpulseInputSize, gModelInputWidth);
    gSettings.edgeImpulseInputSize = gModelInputWidth;
    writeSettingsJson();  // Persist the new input size
    
    // Reallocate image buffers for new model size
    if (!allocateImageBuffers(gSettings.edgeImpulseInputSize)) {
      ERROR_SYSTEMF("[EdgeImpulse] Failed to reallocate buffers for new model size");
      return false;
    }
    DEBUG_SYSTEMF("[EI_DEBUG] Buffers reallocated successfully for %dx%d input",
                  gModelInputWidth, gModelInputHeight);
  }
  
  return true;
}

void unloadModel() {
  DEBUG_SYSTEMF("[EI_DEBUG] unloadModel() called");
  DEBUG_SYSTEMF("[EI_DEBUG]   Current model: %s", 
                gLoadedModelPath.length() > 0 ? gLoadedModelPath.c_str() : "(none)");
  
  // Stop continuous inference if running
  if (gEIContinuousRunning) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Stopping continuous inference first...");
    stopContinuousInference();
    vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to stop
  }
  
  freeModelResources();
  DEBUG_SYSTEMF("[EI_DEBUG]   Model unloaded successfully");
}

bool isModelLoaded() {
  return gEIModelLoaded && gInterpreter != nullptr;
}

const char* getLoadedModelPath() {
  return gLoadedModelPath.c_str();
}

static void listModelsRecursive(const String& absDir, const String& relPrefix, String& output, int& count) {
  File dir = LittleFS.open(absDir.c_str());
  if (!dir || !dir.isDirectory()) return;

  File file = dir.openNextFile();
  while (file) {
    String fullName = file.name();
    String entryName = fullName;
    int ls = entryName.lastIndexOf('/');
    if (ls >= 0) entryName = entryName.substring(ls + 1);

    if (file.isDirectory()) {
      String subAbs = absDir;
      if (!subAbs.endsWith("/")) subAbs += "/";
      subAbs += entryName;
      String subRel = relPrefix.length() ? (relPrefix + "/" + entryName) : entryName;
      listModelsRecursive(subAbs, subRel, output, count);
    } else {
      if (entryName.endsWith(".tflite")) {
        String rel = relPrefix.length() ? (relPrefix + "/" + entryName) : entryName;
        String fullPath = String(MODEL_DIR) + "/" + rel;
        output += "  " + rel + " (" + String(file.size()) + " bytes)";
        if (gLoadedModelPath == fullPath) {
          output += " [LOADED]";
        }
        output += "\n";
        count++;
      }
    }
    file = dir.openNextFile();
  }
}

// List available models in /littlefs/EI Models/
void listAvailableModels(String& output) {
  output = "Available models in " + String(MODEL_DIR) + ":\n";
  
  File root = LittleFS.open(MODEL_DIR);
  if (!root || !root.isDirectory()) {
    output += "  (directory not found - create " + String(MODEL_DIR) + ")\n";
    return;
  }

  int count = 0;
  listModelsRecursive(String(MODEL_DIR), "", output, count);

  if (count == 0) {
    output += "  (no .tflite files found)\n";
  }
}

// Create models directory if it doesn't exist
static void ensureModelDirectory() {
  if (!LittleFS.exists(MODEL_DIR)) {
    LittleFS.mkdir(MODEL_DIR);
    DEBUG_SYSTEMF("[EdgeImpulse] Created models directory: %s", MODEL_DIR);
  }
}

// ============================================================================
// Image Processing Utilities
// ============================================================================

// Bilinear resize RGB888 image
static bool resizeRgb888(const uint8_t* src, int srcW, int srcH,
                         uint8_t* dst, int dstW, int dstH) {
  if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
    return false;
  }
  
  float xRatio = (float)(srcW - 1) / dstW;
  float yRatio = (float)(srcH - 1) / dstH;
  
  for (int y = 0; y < dstH; y++) {
    float srcY = y * yRatio;
    int y0 = (int)srcY;
    int y1 = (y0 + 1 < srcH) ? y0 + 1 : y0;
    float yDiff = srcY - y0;
    
    for (int x = 0; x < dstW; x++) {
      float srcX = x * xRatio;
      int x0 = (int)srcX;
      int x1 = (x0 + 1 < srcW) ? x0 + 1 : x0;
      float xDiff = srcX - x0;
      
      // Get 4 neighboring pixels
      const uint8_t* p00 = src + (y0 * srcW + x0) * 3;
      const uint8_t* p10 = src + (y0 * srcW + x1) * 3;
      const uint8_t* p01 = src + (y1 * srcW + x0) * 3;
      const uint8_t* p11 = src + (y1 * srcW + x1) * 3;
      
      // Bilinear interpolation for each channel
      uint8_t* dstPixel = dst + (y * dstW + x) * 3;
      for (int c = 0; c < 3; c++) {
        float top = p00[c] * (1 - xDiff) + p10[c] * xDiff;
        float bot = p01[c] * (1 - xDiff) + p11[c] * xDiff;
        dstPixel[c] = (uint8_t)(top * (1 - yDiff) + bot * yDiff);
      }
    }
  }
  return true;
}

// Allocate image buffers (prefers PSRAM)
static bool allocateImageBuffers(int inputSize) {
  DEBUG_SYSTEMF("[EI_DEBUG] allocateImageBuffers() called with inputSize=%d", inputSize);
  DEBUG_SYSTEMF("[EI_DEBUG]   Current buffers: RGB=%p (%zu), Resized=%p (%zu)",
                gRgbBuffer, gRgbBufferSize, gResizedBuffer, gResizedBufferSize);
  
  // Free existing buffers
  if (gRgbBuffer) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Freeing existing RGB buffer...");
    free(gRgbBuffer);
    gRgbBuffer = nullptr;
  }
  if (gResizedBuffer) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Freeing existing resized buffer...");
    free(gResizedBuffer);
    gResizedBuffer = nullptr;
  }
  
  // Calculate buffer sizes (VGA max = 640x480, RGB888 = 3 bytes/pixel)
  gRgbBufferSize = 640 * 480 * 3;
  gResizedBufferSize = inputSize * inputSize * 3;
  
  DEBUG_SYSTEMF("[EI_DEBUG]   Allocating: RGB=%zu bytes, Resized=%zu bytes in %s",
                gRgbBufferSize, gResizedBufferSize, psramFound() ? "PSRAM" : "DRAM");
  DEBUG_SYSTEMF("[EI_DEBUG]   PSRAM free before: %lu",
                (unsigned long)(psramFound() ? ESP.getFreePsram() : ESP.getFreeHeap()));
  
  // Allocate in PSRAM if available
  if (psramFound()) {
    gRgbBuffer = (uint8_t*)ps_malloc(gRgbBufferSize);
    gResizedBuffer = (uint8_t*)ps_malloc(gResizedBufferSize);
  } else {
    gRgbBuffer = (uint8_t*)malloc(gRgbBufferSize);
    gResizedBuffer = (uint8_t*)malloc(gResizedBufferSize);
  }
  
  if (!gRgbBuffer || !gResizedBuffer) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate image buffers");
    DEBUG_SYSTEMF("[EI_DEBUG]   ALLOCATION FAILED! RGB=%p, Resized=%p", gRgbBuffer, gResizedBuffer);
    if (gRgbBuffer) { free(gRgbBuffer); gRgbBuffer = nullptr; }
    if (gResizedBuffer) { free(gResizedBuffer); gResizedBuffer = nullptr; }
    return false;
  }
  
  DEBUG_SYSTEMF("[EI_DEBUG]   RGB buffer at %p, Resized buffer at %p", gRgbBuffer, gResizedBuffer);
  DEBUG_SYSTEMF("[EI_DEBUG]   PSRAM free after: %lu",
                (unsigned long)(psramFound() ? ESP.getFreePsram() : ESP.getFreeHeap()));
  return true;
}

// ============================================================================
// Initialization
// ============================================================================

void initEdgeImpulse() {
  DEBUG_SYSTEMF("[EI_DEBUG] ========== initEdgeImpulse() START ==========");
  DEBUG_SYSTEMF("[EI_DEBUG] Already initialized: %s", gEIInitialized ? "YES" : "NO");
  
  if (gEIInitialized) {
    DEBUG_SYSTEMF("[EI_DEBUG] Skipping - already initialized");
    return;
  }
  
  DEBUG_SYSTEMF("[EI_DEBUG] Heap: %lu free, PSRAM: %lu free",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
  DEBUG_SYSTEMF("[EI_DEBUG] Settings: inputSize=%d, minConf=%.2f, interval=%dms",
                gSettings.edgeImpulseInputSize,
                gSettings.edgeImpulseMinConfidence,
                gSettings.edgeImpulseIntervalMs);
  
  // Ensure models directory exists
  DEBUG_SYSTEMF("[EI_DEBUG] Ensuring model directory exists...");
  ensureModelDirectory();
  
  // Allocate image buffers
  DEBUG_SYSTEMF("[EI_DEBUG] Allocating image buffers...");
  if (!allocateImageBuffers(gSettings.edgeImpulseInputSize)) {
    ERROR_SYSTEMF("[EdgeImpulse] Buffer allocation failed");
    return;
  }
  
  gEIInitialized = true;
  DEBUG_SYSTEMF("[EI_DEBUG] Edge Impulse initialized successfully");
  
  // Try to load default model if it exists
  DEBUG_SYSTEMF("[EI_DEBUG] Checking for default model: %s", DEFAULT_MODEL);
  if (LittleFS.exists(DEFAULT_MODEL)) {
    DEBUG_SYSTEMF("[EI_DEBUG] Default model found, loading...");
    if (loadModelFromFile(DEFAULT_MODEL)) {
      DEBUG_SYSTEMF("[EI_DEBUG] Default model loaded successfully");
    } else {
      ERROR_SYSTEMF("[EdgeImpulse] Failed to load default model");
    }
  } else {
    DEBUG_SYSTEMF("[EI_DEBUG] No default model at %s", DEFAULT_MODEL);
    
    // List available models
    File dir = LittleFS.open(MODEL_DIR);
    if (dir && dir.isDirectory()) {
      int modelCount = 0;
      File file = dir.openNextFile();
      while (file) {
        String name = file.name();
        if (name.endsWith(".tflite")) {
          DEBUG_SYSTEMF("[EI_DEBUG]   Found model: %s (%zu bytes)", name.c_str(), file.size());
          modelCount++;
        }
        file = dir.openNextFile();
      }
      DEBUG_SYSTEMF("[EI_DEBUG]   Total models available: %d", modelCount);
    }
  }
  
  DEBUG_SYSTEMF("[EI_DEBUG] ========== initEdgeImpulse() COMPLETE ==========");
}

bool isEdgeImpulseModelLoaded() {
  return gEIModelLoaded;
}

// ============================================================================
// Inference Implementation
// ============================================================================

EIResults runEdgeImpulseInference() {
  EIResults results = {false, 0, {}, 0, nullptr};
  
  DEBUG_SYSTEMF("[EI_DEBUG] ========== runEdgeImpulseInference() START ==========");
  uint32_t totalStart = millis();
  
  // Pre-flight checks with detailed logging
  if (!gEIInitialized) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: Not initialized");
    results.errorMessage = "Edge Impulse not initialized";
    return results;
  }
  
  if (!gSettings.edgeImpulseEnabled) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: Disabled in settings");
    results.errorMessage = "Edge Impulse disabled";
    return results;
  }
  
  if (!gEIModelLoaded) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: No model loaded");
    results.errorMessage = "No model loaded - add Edge Impulse SDK";
    return results;
  }
  
  // Check camera is available
  if (!cameraConnected) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: Camera not connected");
    results.errorMessage = "Camera not connected";
    return results;
  }

  if (!cameraEnabled) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: Camera not started");
    results.errorMessage = "Camera not started";
    return results;
  }
  
  // Check buffers are allocated
  if (!gRgbBuffer || !gResizedBuffer) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: Buffers not allocated (RGB=%p, Resized=%p)",
                  gRgbBuffer, gResizedBuffer);
    results.errorMessage = "Image buffers not allocated";
    return results;
  }
  
  DEBUG_SYSTEMF("[EI_DEBUG] Pre-flight checks passed");
  DEBUG_SYSTEMF("[EI_DEBUG]   Model: %s", gLoadedModelPath.c_str());
  DEBUG_SYSTEMF("[EI_DEBUG]   Input size: %dx%d, MinConf: %.2f",
                gModelInputWidth, gModelInputHeight, gSettings.edgeImpulseMinConfidence);
  DEBUG_SYSTEMF("[EI_DEBUG]   Heap: %lu, PSRAM: %lu",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
  
  uint32_t startTime = millis();
  
  // Step 1: Capture frame from camera (with retry for corrupted frames)
  DEBUG_SYSTEMF("[EI_DEBUG] Step 1: Capturing frame...");
  
  bool converted = false;
  int frameWidth = 0;
  int frameHeight = 0;
  const int maxRetries = 3;
  uint32_t captureTime = 0;
  uint32_t convertTime = 0;
  
  for (int attempt = 0; attempt < maxRetries && !converted; attempt++) {
    if (attempt > 0) {
      DEBUG_SYSTEMF("[EI_DEBUG]   Retry %d/%d after decode failure...", attempt + 1, maxRetries);
      vTaskDelay(pdMS_TO_TICKS(20));  // Brief delay before retry
    }
    
    uint32_t captureStart = millis();
    size_t jpegLen = 0;
    uint8_t* jpegBuf = captureFrame(&jpegLen);
    captureTime = millis() - captureStart;
    
    if (!jpegBuf || jpegLen == 0) {
      DEBUG_SYSTEMF("[EI_DEBUG]   Attempt %d: captureFrame() returned NULL after %lu ms",
                    attempt + 1, captureTime);
      continue;
    }

    frameWidth = cameraWidth;
    frameHeight = cameraHeight;
    DEBUG_SYSTEMF("[EI_DEBUG]   Captured in %lu ms: %dx%d, JPEG len=%zu",
                  captureTime, frameWidth, frameHeight, jpegLen);

    // Step 2: Convert JPEG to RGB888
    uint32_t convertStart = millis();
    converted = fmt2rgb888(jpegBuf, jpegLen, PIXFORMAT_JPEG, gRgbBuffer);
    convertTime = millis() - convertStart;
    free(jpegBuf);

    DEBUG_SYSTEMF("[EI_DEBUG]   Conversion JPEG->RGB888: %s in %lu ms",
                  converted ? "OK" : "FAILED", convertTime);
  }
  
  if (!converted) {
    DEBUG_SYSTEMF("[EI_DEBUG] FAIL: Format conversion failed after %d attempts", maxRetries);
    results.errorMessage = "Failed to convert frame to RGB888";
    return results;
  }
  
  // Step 3: Resize to model input size
  DEBUG_SYSTEMF("[EI_DEBUG] Step 3: Resizing %dx%d -> %dx%d...",
                frameWidth, frameHeight, gModelInputWidth, gModelInputHeight);
  uint32_t resizeStart = millis();
  
  int inputSize = gSettings.edgeImpulseInputSize;
  if (!resizeRgb888(gRgbBuffer, frameWidth, frameHeight,
                    gResizedBuffer, inputSize, inputSize)) {
    DEBUG_SYSTEMF("[EI_DEBUG] FAIL: Resize failed");
    results.errorMessage = "Failed to resize image";
    return results;
  }
  
  uint32_t resizeTime = millis() - resizeStart;
  DEBUG_SYSTEMF("[EI_DEBUG]   Resize complete in %lu ms", resizeTime);
  
  // Step 4: Run TFLite inference
  DEBUG_SYSTEMF("[EI_DEBUG] Step 4: Running TFLite inference...");
  
  if (!gInterpreter || !gInputTensor || !gOutputTensor) {
    DEBUG_SYSTEMF("[EI_DEBUG] FAIL: Interpreter not ready (int=%p, in=%p, out=%p)",
                  gInterpreter, gInputTensor, gOutputTensor);
    results.errorMessage = "Interpreter not ready";
    return results;
  }
  
  // Copy resized image to input tensor
  DEBUG_SYSTEMF("[EI_DEBUG]   Copying to input tensor (type=%d, bytes=%zu)...",
                gInputTensor->type, gInputTensor->bytes);
  uint32_t copyStart = millis();
  
  // Handle different input tensor types
  if (gInputTensor->type == kTfLiteFloat32) {
    // Float model - normalize to 0-1 or -1 to 1
    float* inputData = gInputTensor->data.f;
    int pixelCount = inputSize * inputSize * 3;
    for (int i = 0; i < pixelCount; i++) {
      inputData[i] = gResizedBuffer[i] / 255.0f;  // Normalize to 0-1
    }
    DEBUG_SYSTEMF("[EI_DEBUG]   Float32 normalization: %d pixels", pixelCount);
  } else if (gInputTensor->type == kTfLiteUInt8) {
    // Quantized model - copy directly
    memcpy(gInputTensor->data.uint8, gResizedBuffer, inputSize * inputSize * 3);
    DEBUG_SYSTEMF("[EI_DEBUG]   UInt8 direct copy: %d bytes", inputSize * inputSize * 3);
  } else if (gInputTensor->type == kTfLiteInt8) {
    // Signed quantized - convert
    int8_t* inputData = gInputTensor->data.int8;
    int pixelCount = inputSize * inputSize * 3;
    for (int i = 0; i < pixelCount; i++) {
      inputData[i] = (int8_t)(gResizedBuffer[i] - 128);
    }
    DEBUG_SYSTEMF("[EI_DEBUG]   Int8 conversion: %d pixels", pixelCount);
  } else {
    DEBUG_SYSTEMF("[EI_DEBUG]   WARNING: Unknown tensor type %d", gInputTensor->type);
  }
  
  uint32_t copyTime = millis() - copyStart;
  DEBUG_SYSTEMF("[EI_DEBUG]   Input copy complete in %lu ms", copyTime);
  
  // Run inference
  DEBUG_SYSTEMF("[EI_DEBUG]   Invoking interpreter...");
  uint32_t invokeStart = millis();
  TfLiteStatus invokeStatus = gInterpreter->Invoke();
  uint32_t invokeTime = millis() - invokeStart;
  
  DEBUG_SYSTEMF("[EI_DEBUG]   Invoke returned %d in %lu ms", invokeStatus, invokeTime);
  
  if (invokeStatus != kTfLiteOk) {
    DEBUG_SYSTEMF("[EI_DEBUG] FAIL: Inference failed with status %d", invokeStatus);
    results.errorMessage = "Inference failed";
    return results;
  }
  
  // Process output tensor
  DEBUG_SYSTEMF("[EI_DEBUG] Step 5: Processing output tensor...");
  DEBUG_SYSTEMF("[EI_DEBUG]   Output type=%d, dims=%d, bytes=%zu",
                gOutputTensor->type, gOutputTensor->dims->size, gOutputTensor->bytes);
  
  // Print all output dimensions for debugging
  DEBUG_SYSTEMF("[EI_DEBUG]   Output tensor dimensions:");
  for (int d = 0; d < gOutputTensor->dims->size; d++) {
    DEBUG_SYSTEMF("[EI_DEBUG]     dim[%d] = %d", d, gOutputTensor->dims->data[d]);
  }
  
  // Output format depends on model type (classification vs detection)
  results.detectionCount = 0;
  
  // Apply configurable max detections limit (clamped to array bounds)
  int maxDetections = gSettings.edgeImpulseMaxDetections;
  if (maxDetections < 1) maxDetections = 1;
  if (maxDetections > EI_MAX_DETECTIONS) maxDetections = EI_MAX_DETECTIONS;
  
  int outputSize = gOutputTensor->dims->data[gOutputTensor->dims->size - 1];
  DEBUG_SYSTEMF("[EI_DEBUG]   Output size: %d classes/detections", outputSize);
  
  // Detect if this is a FOMO model (grid-based output)
  // FOMO output shape: [1, grid_height, grid_width, num_classes]
  bool isFOMO = (gOutputTensor->dims->size == 4);
  int gridHeight = 1, gridWidth = 1, numClasses = outputSize;
  
  if (isFOMO) {
    gridHeight = gOutputTensor->dims->data[1];
    gridWidth = gOutputTensor->dims->data[2];
    numClasses = gOutputTensor->dims->data[3];
    DEBUG_SYSTEMF("[EI_DEBUG]   FOMO model detected: grid=%dx%d, classes=%d", 
                  gridHeight, gridWidth, numClasses);
  } else {
    DEBUG_SYSTEMF("[EI_DEBUG]   Classification model detected: %d classes", outputSize);
  }
  
  int aboveThreshold = 0;
  float maxConf = 0.0f;
  int maxIdx = -1;
  
  if (gOutputTensor->type == kTfLiteFloat32) {
    float* outputData = gOutputTensor->data.f;
    
    if (isFOMO) {
      // FOMO object detection: parse grid-based output
      DEBUG_SYSTEMF("[EI_DEBUG]   Parsing FOMO grid output...");
      int cellSize = inputSize / gridWidth;  // Size of each grid cell in pixels
      
      for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
          // Get class probabilities for this grid cell
          int cellOffset = (y * gridWidth + x) * numClasses;
          
          // Find highest confidence class (skip background class 0)
          float bestConf = 0.0f;
          int bestClass = -1;
          for (int c = 1; c < numClasses; c++) {  // Start at 1 to skip background
            float conf = outputData[cellOffset + c];
            if (conf > bestConf) {
              bestConf = conf;
              bestClass = c;
            }
          }
          
          // If confidence above threshold, add detection
          if (bestConf >= gSettings.edgeImpulseMinConfidence && 
              results.detectionCount < maxDetections) {
            aboveThreshold++;
            
            // Calculate bounding box (centered on grid cell)
            int centerX = x * cellSize + cellSize / 2;
            int centerY = y * cellSize + cellSize / 2;
            int boxSize = cellSize;  // Use cell size as box size
            
            results.detections[results.detectionCount].label = getLabelForIndex(bestClass);
            results.detections[results.detectionCount].confidence = bestConf;
            results.detections[results.detectionCount].x = max(0, centerX - boxSize / 2);
            results.detections[results.detectionCount].y = max(0, centerY - boxSize / 2);
            results.detections[results.detectionCount].width = boxSize;
            results.detections[results.detectionCount].height = boxSize;
            results.detectionCount++;
            
            DEBUG_SYSTEMF("[EI_DEBUG]     Detection at grid[%d,%d]: class=%d conf=%.3f box=(%d,%d,%d,%d)",
                          x, y, bestClass, bestConf,
                          results.detections[results.detectionCount-1].x,
                          results.detections[results.detectionCount-1].y,
                          results.detections[results.detectionCount-1].width,
                          results.detections[results.detectionCount-1].height);
          }
          
          if (bestConf > maxConf) {
            maxConf = bestConf;
            maxIdx = bestClass;
          }
        }
      }
      DEBUG_SYSTEMF("[EI_DEBUG]   FOMO Float32: max=%.4f at class=%d, %d detections",
                    maxConf, maxIdx, aboveThreshold);
    } else {
      // Classification: output is array of class probabilities
      for (int i = 0; i < outputSize && results.detectionCount < maxDetections; i++) {
        float confidence = outputData[i];
        if (confidence > maxConf) {
          maxConf = confidence;
          maxIdx = i;
        }
        if (confidence >= gSettings.edgeImpulseMinConfidence) {
          aboveThreshold++;
          results.detections[results.detectionCount].label = getLabelForIndex(i);
          results.detections[results.detectionCount].confidence = confidence;
          results.detections[results.detectionCount].x = 0;
          results.detections[results.detectionCount].y = 0;
          results.detections[results.detectionCount].width = inputSize;
          results.detections[results.detectionCount].height = inputSize;
          results.detectionCount++;
        }
      }
      DEBUG_SYSTEMF("[EI_DEBUG]   Classification Float32: max=%.4f at idx=%d, %d above threshold",
                    maxConf, maxIdx, aboveThreshold);
    }
  } else if (gOutputTensor->type == kTfLiteUInt8) {
    uint8_t* outputData = gOutputTensor->data.uint8;
    float scale = gOutputTensor->params.scale;
    int32_t zeroPoint = gOutputTensor->params.zero_point;
    DEBUG_SYSTEMF("[EI_DEBUG]   UInt8 quantization: scale=%.6f, zeroPoint=%d", scale, zeroPoint);
    
    if (isFOMO) {
      // FOMO object detection: parse grid-based output
      DEBUG_SYSTEMF("[EI_DEBUG]   Parsing FOMO grid output (UInt8)...");
      int cellSize = inputSize / gridWidth;
      
      for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
          int cellOffset = (y * gridWidth + x) * numClasses;
          
          float bestConf = 0.0f;
          int bestClass = -1;
          for (int c = 1; c < numClasses; c++) {
            float conf = (outputData[cellOffset + c] - zeroPoint) * scale;
            if (conf > bestConf) {
              bestConf = conf;
              bestClass = c;
            }
          }
          
          if (bestConf >= gSettings.edgeImpulseMinConfidence && 
              results.detectionCount < maxDetections) {
            aboveThreshold++;
            
            int centerX = x * cellSize + cellSize / 2;
            int centerY = y * cellSize + cellSize / 2;
            int boxSize = cellSize;
            
            results.detections[results.detectionCount].label = getLabelForIndex(bestClass);
            results.detections[results.detectionCount].confidence = bestConf;
            results.detections[results.detectionCount].x = max(0, centerX - boxSize / 2);
            results.detections[results.detectionCount].y = max(0, centerY - boxSize / 2);
            results.detections[results.detectionCount].width = boxSize;
            results.detections[results.detectionCount].height = boxSize;
            results.detectionCount++;
            
            DEBUG_SYSTEMF("[EI_DEBUG]     Detection at grid[%d,%d]: class=%d conf=%.3f",
                          x, y, bestClass, bestConf);
          }
          
          if (bestConf > maxConf) {
            maxConf = bestConf;
            maxIdx = bestClass;
          }
        }
      }
      DEBUG_SYSTEMF("[EI_DEBUG]   FOMO UInt8: max=%.4f at class=%d, %d detections",
                    maxConf, maxIdx, aboveThreshold);
    } else {
      // Classification
      for (int i = 0; i < outputSize && results.detectionCount < maxDetections; i++) {
        float confidence = (outputData[i] - zeroPoint) * scale;
        if (confidence > maxConf) {
          maxConf = confidence;
          maxIdx = i;
        }
        if (confidence >= gSettings.edgeImpulseMinConfidence) {
          aboveThreshold++;
          results.detections[results.detectionCount].label = getLabelForIndex(i);
          results.detections[results.detectionCount].confidence = confidence;
          results.detections[results.detectionCount].x = 0;
          results.detections[results.detectionCount].y = 0;
          results.detections[results.detectionCount].width = inputSize;
          results.detections[results.detectionCount].height = inputSize;
          results.detectionCount++;
        }
      }
      DEBUG_SYSTEMF("[EI_DEBUG]   Classification UInt8: max=%.4f at idx=%d, %d above threshold",
                    maxConf, maxIdx, aboveThreshold);
    }
  } else if (gOutputTensor->type == kTfLiteInt8) {
    int8_t* outputData = gOutputTensor->data.int8;
    float scale = gOutputTensor->params.scale;
    int32_t zeroPoint = gOutputTensor->params.zero_point;
    DEBUG_SYSTEMF("[EI_DEBUG]   Int8 quantization: scale=%.6f, zeroPoint=%d", scale, zeroPoint);
    
    if (isFOMO) {
      // FOMO object detection: parse grid-based output
      DEBUG_SYSTEMF("[EI_DEBUG]   Parsing FOMO grid output (Int8)...");
      int cellSize = inputSize / gridWidth;
      
      for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
          int cellOffset = (y * gridWidth + x) * numClasses;
          
          float bestConf = 0.0f;
          int bestClass = -1;
          for (int c = 1; c < numClasses; c++) {
            float conf = (outputData[cellOffset + c] - zeroPoint) * scale;
            if (conf > bestConf) {
              bestConf = conf;
              bestClass = c;
            }
          }
          
          if (bestConf >= gSettings.edgeImpulseMinConfidence && 
              results.detectionCount < maxDetections) {
            aboveThreshold++;
            
            int centerX = x * cellSize + cellSize / 2;
            int centerY = y * cellSize + cellSize / 2;
            int boxSize = cellSize;
            
            results.detections[results.detectionCount].label = getLabelForIndex(bestClass);
            results.detections[results.detectionCount].confidence = bestConf;
            results.detections[results.detectionCount].x = max(0, centerX - boxSize / 2);
            results.detections[results.detectionCount].y = max(0, centerY - boxSize / 2);
            results.detections[results.detectionCount].width = boxSize;
            results.detections[results.detectionCount].height = boxSize;
            results.detectionCount++;
            
            DEBUG_SYSTEMF("[EI_DEBUG]     Detection at grid[%d,%d]: class=%d conf=%.3f",
                          x, y, bestClass, bestConf);
          }
          
          if (bestConf > maxConf) {
            maxConf = bestConf;
            maxIdx = bestClass;
          }
        }
      }
      DEBUG_SYSTEMF("[EI_DEBUG]   FOMO Int8: max=%.4f at class=%d, %d detections",
                    maxConf, maxIdx, aboveThreshold);
    } else {
      // Classification
      for (int i = 0; i < outputSize && results.detectionCount < maxDetections; i++) {
        float confidence = (outputData[i] - zeroPoint) * scale;
        if (confidence > maxConf) {
          maxConf = confidence;
          maxIdx = i;
        }
        if (confidence >= gSettings.edgeImpulseMinConfidence) {
          aboveThreshold++;
          results.detections[results.detectionCount].label = getLabelForIndex(i);
          results.detections[results.detectionCount].confidence = confidence;
          results.detections[results.detectionCount].x = 0;
          results.detections[results.detectionCount].y = 0;
          results.detections[results.detectionCount].width = inputSize;
          results.detections[results.detectionCount].height = inputSize;
          results.detectionCount++;
        }
      }
      DEBUG_SYSTEMF("[EI_DEBUG]   Classification Int8: max=%.4f at idx=%d, %d above threshold",
                    maxConf, maxIdx, aboveThreshold);
    }
  }
  
  results.inferenceTimeMs = millis() - startTime;
  results.success = true;
  
  uint32_t totalTime = millis() - totalStart;
  DEBUG_SYSTEMF("[EI_DEBUG] ========== runEdgeImpulseInference() COMPLETE ==========");
  DEBUG_SYSTEMF("[EI_DEBUG]   Detections: %d (threshold: %.2f)",
                results.detectionCount, gSettings.edgeImpulseMinConfidence);
  INFO_SYSTEMF("[EdgeImpulse] Inference: %dms | Max confidence: %.3f | Detections: %d (threshold: %.2f)",
               results.inferenceTimeMs, maxConf, results.detectionCount, gSettings.edgeImpulseMinConfidence);
  DEBUG_SYSTEMF("[EI_DEBUG]   Timing breakdown:");
  DEBUG_SYSTEMF("[EI_DEBUG]     Capture:  %lu ms", captureTime);
  DEBUG_SYSTEMF("[EI_DEBUG]     Convert:  %lu ms", convertTime);
  DEBUG_SYSTEMF("[EI_DEBUG]     Resize:   %lu ms", resizeTime);
  DEBUG_SYSTEMF("[EI_DEBUG]     Copy:     %lu ms", copyTime);
  DEBUG_SYSTEMF("[EI_DEBUG]     Invoke:   %lu ms", invokeTime);
  DEBUG_SYSTEMF("[EI_DEBUG]     Total:    %lu ms", totalTime);
  
  // Update state tracking
  if (gStateTrackingEnabled) {
    updateTrackedObjects(results);
  }
  
  gLastResults = results;
  return results;
}

const EIResults& getLastDetectionResults() {
  return gLastResults;
}

// ============================================================================
// Inference from Stored Image File
// ============================================================================

EIResults runInferenceFromFile(const char* imagePath) {
  EIResults results = {false, 0, {}, 0, nullptr};
  
  DEBUG_SYSTEMF("[EI_DEBUG] ========== runInferenceFromFile() START ==========");
  DEBUG_SYSTEMF("[EI_DEBUG] Image path: %s", imagePath);
  uint32_t totalStart = millis();
  
  // Pre-flight checks
  if (!gEIInitialized) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: Not initialized");
    results.errorMessage = "Edge Impulse not initialized";
    return results;
  }
  
  if (!gEIModelLoaded) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: No model loaded");
    results.errorMessage = "No model loaded";
    return results;
  }
  
  if (!gRgbBuffer || !gResizedBuffer) {
    DEBUG_SYSTEMF("[EI_DEBUG] ABORT: Buffers not allocated");
    results.errorMessage = "Image buffers not allocated";
    return results;
  }
  
  // Open and read image file
  DEBUG_SYSTEMF("[EI_DEBUG] Step 1: Loading image from file...");
  uint32_t loadStart = millis();
  
  File imgFile = LittleFS.open(imagePath, "r");
  if (!imgFile) {
    DEBUG_SYSTEMF("[EI_DEBUG] FAIL: Cannot open file: %s", imagePath);
    results.errorMessage = "Failed to open image file";
    return results;
  }
  
  size_t fileSize = imgFile.size();
  DEBUG_SYSTEMF("[EI_DEBUG]   File size: %zu bytes", fileSize);
  
  if (fileSize == 0) {
    imgFile.close();
    results.errorMessage = "Image file is empty";
    return results;
  }
  
  // Allocate buffer for image file (JPEG/etc)
  uint8_t* imgBuffer = nullptr;
  if (psramFound()) {
    imgBuffer = (uint8_t*)ps_malloc(fileSize);
  } else {
    imgBuffer = (uint8_t*)malloc(fileSize);
  }
  
  if (!imgBuffer) {
    imgFile.close();
    DEBUG_SYSTEMF("[EI_DEBUG] FAIL: Cannot allocate %zu bytes for image", fileSize);
    results.errorMessage = "Failed to allocate image buffer";
    return results;
  }
  
  size_t bytesRead = imgFile.read(imgBuffer, fileSize);
  imgFile.close();
  
  uint32_t loadTime = millis() - loadStart;
  DEBUG_SYSTEMF("[EI_DEBUG]   Loaded %zu bytes in %lu ms", bytesRead, loadTime);
  
  if (bytesRead != fileSize) {
    free(imgBuffer);
    results.errorMessage = "Failed to read image file";
    return results;
  }
  
  // Step 2: Decode image to RGB888
  DEBUG_SYSTEMF("[EI_DEBUG] Step 2: Decoding image...");
  uint32_t decodeStart = millis();
  
  bool decoded = false;
  int imgWidth = 0, imgHeight = 0;
  
  // Check for JPEG signature (FFD8)
  if (fileSize > 2 && imgBuffer[0] == 0xFF && imgBuffer[1] == 0xD8) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Detected JPEG format");
    // Use ESP32 JPEG decoder - assume VGA max
    decoded = fmt2rgb888(imgBuffer, fileSize, PIXFORMAT_JPEG, gRgbBuffer);
    if (decoded) {
      // JPEG decoder doesn't give us dimensions easily, assume common sizes
      // For now, assume 640x480 or use model input size
      imgWidth = 640;
      imgHeight = 480;
    }
  } else {
    DEBUG_SYSTEMF("[EI_DEBUG]   Unknown format (first bytes: %02X %02X)", imgBuffer[0], imgBuffer[1]);
    free(imgBuffer);
    results.errorMessage = "Unsupported image format (JPEG only)";
    return results;
  }
  
  free(imgBuffer);
  imgBuffer = nullptr;
  
  uint32_t decodeTime = millis() - decodeStart;
  DEBUG_SYSTEMF("[EI_DEBUG]   Decode %s in %lu ms, size=%dx%d", 
                decoded ? "OK" : "FAILED", decodeTime, imgWidth, imgHeight);
  
  if (!decoded) {
    results.errorMessage = "Failed to decode image";
    return results;
  }
  
  // Step 3: Resize to model input size
  DEBUG_SYSTEMF("[EI_DEBUG] Step 3: Resizing %dx%d -> %dx%d...",
                imgWidth, imgHeight, gModelInputWidth, gModelInputHeight);
  uint32_t resizeStart = millis();
  
  int inputSize = gSettings.edgeImpulseInputSize;
  if (!resizeRgb888(gRgbBuffer, imgWidth, imgHeight,
                    gResizedBuffer, inputSize, inputSize)) {
    DEBUG_SYSTEMF("[EI_DEBUG] FAIL: Resize failed");
    results.errorMessage = "Failed to resize image";
    return results;
  }
  
  uint32_t resizeTime = millis() - resizeStart;
  DEBUG_SYSTEMF("[EI_DEBUG]   Resize complete in %lu ms", resizeTime);
  
  // Step 4: Run TFLite inference (same as camera inference)
  DEBUG_SYSTEMF("[EI_DEBUG] Step 4: Running TFLite inference...");
  
  if (!gInterpreter || !gInputTensor || !gOutputTensor) {
    results.errorMessage = "Interpreter not ready";
    return results;
  }
  
  // Copy to input tensor
  DEBUG_SYSTEMF("[EI_DEBUG]   Copying to input tensor (type=%d)...", gInputTensor->type);
  uint32_t copyStart = millis();
  
  if (gInputTensor->type == kTfLiteFloat32) {
    float* inputData = gInputTensor->data.f;
    int pixelCount = inputSize * inputSize * 3;
    for (int i = 0; i < pixelCount; i++) {
      inputData[i] = gResizedBuffer[i] / 255.0f;
    }
  } else if (gInputTensor->type == kTfLiteUInt8) {
    memcpy(gInputTensor->data.uint8, gResizedBuffer, inputSize * inputSize * 3);
  } else if (gInputTensor->type == kTfLiteInt8) {
    int8_t* inputData = gInputTensor->data.int8;
    int pixelCount = inputSize * inputSize * 3;
    for (int i = 0; i < pixelCount; i++) {
      inputData[i] = (int8_t)(gResizedBuffer[i] - 128);
    }
  }
  
  uint32_t copyTime = millis() - copyStart;
  
  // Run inference
  DEBUG_SYSTEMF("[EI_DEBUG]   Invoking interpreter...");
  uint32_t invokeStart = millis();
  TfLiteStatus invokeStatus = gInterpreter->Invoke();
  uint32_t invokeTime = millis() - invokeStart;
  
  DEBUG_SYSTEMF("[EI_DEBUG]   Invoke returned %d in %lu ms", invokeStatus, invokeTime);
  
  if (invokeStatus != kTfLiteOk) {
    results.errorMessage = "Inference failed";
    return results;
  }
  
  // Process output (same as camera inference)
  DEBUG_SYSTEMF("[EI_DEBUG] Step 5: Processing output...");
  results.detectionCount = 0;
  
  // Apply configurable max detections limit (clamped to array bounds)
  int maxDetections = gSettings.edgeImpulseMaxDetections;
  if (maxDetections < 1) maxDetections = 1;
  if (maxDetections > EI_MAX_DETECTIONS) maxDetections = EI_MAX_DETECTIONS;
  
  int outputSize = gOutputTensor->dims->data[gOutputTensor->dims->size - 1];
  float maxConf = 0.0f;
  int maxIdx = -1;
  
  // Detect FOMO model
  bool isFOMO = (gOutputTensor->dims->size == 4);
  int gridHeight = 1, gridWidth = 1, numClasses = outputSize;
  if (isFOMO) {
    gridHeight = gOutputTensor->dims->data[1];
    gridWidth = gOutputTensor->dims->data[2];
    numClasses = gOutputTensor->dims->data[3];
    DEBUG_SYSTEMF("[EI_DEBUG]   FOMO model: grid=%dx%d, classes=%d", gridHeight, gridWidth, numClasses);
  }
  
  if (gOutputTensor->type == kTfLiteFloat32) {
    float* outputData = gOutputTensor->data.f;
    if (isFOMO) {
      int cellSize = inputSize / gridWidth;
      for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
          int cellOffset = (y * gridWidth + x) * numClasses;
          float bestConf = 0.0f;
          int bestClass = -1;
          for (int c = 1; c < numClasses; c++) {
            float conf = outputData[cellOffset + c];
            if (conf > bestConf) { bestConf = conf; bestClass = c; }
          }
          if (bestConf >= gSettings.edgeImpulseMinConfidence && results.detectionCount < maxDetections) {
            int centerX = x * cellSize + cellSize / 2;
            int centerY = y * cellSize + cellSize / 2;
            results.detections[results.detectionCount].label = getLabelForIndex(bestClass);
            results.detections[results.detectionCount].confidence = bestConf;
            results.detections[results.detectionCount].x = max(0, centerX - cellSize / 2);
            results.detections[results.detectionCount].y = max(0, centerY - cellSize / 2);
            results.detections[results.detectionCount].width = cellSize;
            results.detections[results.detectionCount].height = cellSize;
            results.detectionCount++;
          }
          if (bestConf > maxConf) { maxConf = bestConf; maxIdx = bestClass; }
        }
      }
    } else {
      for (int i = 0; i < outputSize && results.detectionCount < maxDetections; i++) {
        float confidence = outputData[i];
        if (confidence > maxConf) { maxConf = confidence; maxIdx = i; }
        if (confidence >= gSettings.edgeImpulseMinConfidence) {
          results.detections[results.detectionCount].label = getLabelForIndex(i);
          results.detections[results.detectionCount].confidence = confidence;
          results.detections[results.detectionCount].x = 0;
          results.detections[results.detectionCount].y = 0;
          results.detections[results.detectionCount].width = inputSize;
          results.detections[results.detectionCount].height = inputSize;
          results.detectionCount++;
        }
      }
    }
  } else if (gOutputTensor->type == kTfLiteUInt8) {
    uint8_t* outputData = gOutputTensor->data.uint8;
    float scale = gOutputTensor->params.scale;
    int32_t zeroPoint = gOutputTensor->params.zero_point;
    if (isFOMO) {
      int cellSize = inputSize / gridWidth;
      for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
          int cellOffset = (y * gridWidth + x) * numClasses;
          float bestConf = 0.0f;
          int bestClass = -1;
          for (int c = 1; c < numClasses; c++) {
            float conf = (outputData[cellOffset + c] - zeroPoint) * scale;
            if (conf > bestConf) { bestConf = conf; bestClass = c; }
          }
          if (bestConf >= gSettings.edgeImpulseMinConfidence && results.detectionCount < maxDetections) {
            int centerX = x * cellSize + cellSize / 2;
            int centerY = y * cellSize + cellSize / 2;
            results.detections[results.detectionCount].label = getLabelForIndex(bestClass);
            results.detections[results.detectionCount].confidence = bestConf;
            results.detections[results.detectionCount].x = max(0, centerX - cellSize / 2);
            results.detections[results.detectionCount].y = max(0, centerY - cellSize / 2);
            results.detections[results.detectionCount].width = cellSize;
            results.detections[results.detectionCount].height = cellSize;
            results.detectionCount++;
          }
          if (bestConf > maxConf) { maxConf = bestConf; maxIdx = bestClass; }
        }
      }
    } else {
      for (int i = 0; i < outputSize && results.detectionCount < maxDetections; i++) {
        float confidence = (outputData[i] - zeroPoint) * scale;
        if (confidence > maxConf) { maxConf = confidence; maxIdx = i; }
        if (confidence >= gSettings.edgeImpulseMinConfidence) {
          results.detections[results.detectionCount].label = getLabelForIndex(i);
          results.detections[results.detectionCount].confidence = confidence;
          results.detections[results.detectionCount].x = 0;
          results.detections[results.detectionCount].y = 0;
          results.detections[results.detectionCount].width = inputSize;
          results.detections[results.detectionCount].height = inputSize;
          results.detectionCount++;
        }
      }
    }
  } else if (gOutputTensor->type == kTfLiteInt8) {
    int8_t* outputData = gOutputTensor->data.int8;
    float scale = gOutputTensor->params.scale;
    int32_t zeroPoint = gOutputTensor->params.zero_point;
    if (isFOMO) {
      int cellSize = inputSize / gridWidth;
      for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
          int cellOffset = (y * gridWidth + x) * numClasses;
          float bestConf = 0.0f;
          int bestClass = -1;
          for (int c = 1; c < numClasses; c++) {
            float conf = (outputData[cellOffset + c] - zeroPoint) * scale;
            if (conf > bestConf) { bestConf = conf; bestClass = c; }
          }
          if (bestConf >= gSettings.edgeImpulseMinConfidence && results.detectionCount < maxDetections) {
            int centerX = x * cellSize + cellSize / 2;
            int centerY = y * cellSize + cellSize / 2;
            results.detections[results.detectionCount].label = getLabelForIndex(bestClass);
            results.detections[results.detectionCount].confidence = bestConf;
            results.detections[results.detectionCount].x = max(0, centerX - cellSize / 2);
            results.detections[results.detectionCount].y = max(0, centerY - cellSize / 2);
            results.detections[results.detectionCount].width = cellSize;
            results.detections[results.detectionCount].height = cellSize;
            results.detectionCount++;
          }
          if (bestConf > maxConf) { maxConf = bestConf; maxIdx = bestClass; }
        }
      }
    } else {
      for (int i = 0; i < outputSize && results.detectionCount < maxDetections; i++) {
        float confidence = (outputData[i] - zeroPoint) * scale;
        if (confidence > maxConf) { maxConf = confidence; maxIdx = i; }
        if (confidence >= gSettings.edgeImpulseMinConfidence) {
          results.detections[results.detectionCount].label = getLabelForIndex(i);
          results.detections[results.detectionCount].confidence = confidence;
          results.detections[results.detectionCount].x = 0;
          results.detections[results.detectionCount].y = 0;
          results.detections[results.detectionCount].width = inputSize;
          results.detections[results.detectionCount].height = inputSize;
          results.detectionCount++;
        }
      }
    }
  }
  
  uint32_t totalTime = millis() - totalStart;
  results.inferenceTimeMs = totalTime;
  results.success = true;
  
  DEBUG_SYSTEMF("[EI_DEBUG] ========== runInferenceFromFile() COMPLETE ==========");
  DEBUG_SYSTEMF("[EI_DEBUG]   Detections: %d, Max confidence: %.4f at idx %d",
                results.detectionCount, maxConf, maxIdx);
  DEBUG_SYSTEMF("[EI_DEBUG]   Timing: load=%lu decode=%lu resize=%lu copy=%lu invoke=%lu total=%lu ms",
                loadTime, decodeTime, resizeTime, copyTime, invokeTime, totalTime);
  
  gLastResults = results;
  return results;
}

// ============================================================================
// Continuous Inference Task
// ============================================================================

static void continuousInferenceTask(void* param) {
  DEBUG_SYSTEMF("[EI_DEBUG] ===== Continuous inference task STARTED =====");
  DEBUG_SYSTEMF("[EI_DEBUG]   Interval: %d ms", gSettings.edgeImpulseIntervalMs);
  DEBUG_SYSTEMF("[EI_DEBUG]   Running on core: %d", xPortGetCoreID());
  
  uint32_t inferenceCount = 0;
  uint32_t successCount = 0;
  uint32_t detectionCount = 0;
  uint32_t taskStartTime = millis();
  
  while (gEIContinuousRunning) {
    if (gSettings.edgeImpulseEnabled && gEIModelLoaded) {
      inferenceCount++;
      EIResults results = runEdgeImpulseInference();
      
      if (results.success) {
        successCount++;
        if (results.detectionCount > 0) {
          detectionCount += results.detectionCount;
          DEBUG_SYSTEMF("[EI_DEBUG] [Continuous #%lu] Detected %d objects",
                        inferenceCount, results.detectionCount);
        }
      } else {
        DEBUG_SYSTEMF("[EI_DEBUG] [Continuous #%lu] FAILED: %s",
                      inferenceCount, results.errorMessage ? results.errorMessage : "unknown");
      }
      
      // Periodic stats every 10 inferences
      if (inferenceCount % 10 == 0) {
        uint32_t elapsed = millis() - taskStartTime;
        float fps = (elapsed > 0) ? (inferenceCount * 1000.0f / elapsed) : 0;
        DEBUG_SYSTEMF("[EI_DEBUG] [Continuous stats] %lu inferences, %lu success, %lu detections, %.2f FPS",
                      inferenceCount, successCount, detectionCount, fps);
        DEBUG_SYSTEMF("[EI_DEBUG]   Heap: %lu, PSRAM: %lu",
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
      }
    } else {
      DEBUG_SYSTEMF("[EI_DEBUG] [Continuous] Skipping - enabled=%d modelLoaded=%d",
                    gSettings.edgeImpulseEnabled, gEIModelLoaded);
    }
    
    vTaskDelay(pdMS_TO_TICKS(gSettings.edgeImpulseIntervalMs));
  }
  
  uint32_t totalTime = millis() - taskStartTime;
  DEBUG_SYSTEMF("[EI_DEBUG] ===== Continuous inference task STOPPED =====");
  DEBUG_SYSTEMF("[EI_DEBUG]   Total: %lu inferences in %lu ms", inferenceCount, totalTime);
  DEBUG_SYSTEMF("[EI_DEBUG]   Success rate: %.1f%% (%lu/%lu)",
                inferenceCount > 0 ? (100.0f * successCount / inferenceCount) : 0,
                successCount, inferenceCount);
  DEBUG_SYSTEMF("[EI_DEBUG]   Total detections: %lu", detectionCount);
  
  gEIContinuousTask = nullptr;
  vTaskDelete(nullptr);
}

void startContinuousInference() {
  DEBUG_SYSTEMF("[EI_DEBUG] startContinuousInference() called");
  DEBUG_SYSTEMF("[EI_DEBUG]   Already running: %s", gEIContinuousRunning ? "YES" : "NO");
  DEBUG_SYSTEMF("[EI_DEBUG]   Model loaded: %s", gEIModelLoaded ? "YES" : "NO");
  
  if (gEIContinuousRunning) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Skipping - already running");
    return;
  }
  
  if (!gEIModelLoaded) {
    ERROR_SYSTEMF("[EdgeImpulse] Cannot start continuous - no model loaded");
    return;
  }
  
  DEBUG_SYSTEMF("[EI_DEBUG]   Creating continuous task on core 0...");
  gEIContinuousRunning = true;
  
  BaseType_t result = xTaskCreatePinnedToCore(
    continuousInferenceTask,
    "ei_continuous",
    8192,  // Increased stack for debug logging
    nullptr,
    1,
    &gEIContinuousTask,
    0  // Run on core 0
  );
  
  if (result != pdPASS) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to create continuous task (result=%d)", result);
    gEIContinuousRunning = false;
    return;
  }
  
  gSettings.edgeImpulseContinuous = true;
  DEBUG_SYSTEMF("[EI_DEBUG]   Continuous inference started, task handle=%p", gEIContinuousTask);
}

void stopContinuousInference() {
  DEBUG_SYSTEMF("[EI_DEBUG] stopContinuousInference() called");
  DEBUG_SYSTEMF("[EI_DEBUG]   Was running: %s", gEIContinuousRunning ? "YES" : "NO");
  DEBUG_SYSTEMF("[EI_DEBUG]   Task handle: %p", gEIContinuousTask);
  
  gEIContinuousRunning = false;
  gSettings.edgeImpulseContinuous = false;
  // Task will self-delete on next loop iteration
  
  DEBUG_SYSTEMF("[EI_DEBUG]   Stop signal sent, task will exit on next iteration");
}

bool isContinuousInferenceRunning() {
  return gEIContinuousRunning;
}

// ============================================================================
// JSON Output
// ============================================================================

void buildDetectionJson(const EIResults& results, String& output) {
  JsonDocument doc;
  
  doc["success"] = results.success;
  doc["inferenceTimeMs"] = results.inferenceTimeMs;
  doc["modelInputSize"] = gSettings.edgeImpulseInputSize;
  
  if (results.errorMessage) {
    doc["error"] = results.errorMessage;
  }
  
  JsonArray detections = doc["detections"].to<JsonArray>();
  for (int i = 0; i < results.detectionCount; i++) {
    JsonObject det = detections.add<JsonObject>();
    det["label"] = results.detections[i].label;
    det["confidence"] = results.detections[i].confidence;
    det["x"] = results.detections[i].x;
    det["y"] = results.detections[i].y;
    det["width"] = results.detections[i].width;
    det["height"] = results.detections[i].height;
  }
  
  serializeJson(doc, output);
}

// ============================================================================
// Web API Handler
// ============================================================================

#include "WebServer_Server.h"
#include "System_Auth.h"

// Organize EI model files: move loose .tflite and .labels.txt into proper /EI Models/<name>/ folders
static esp_err_t handleEIOrganize(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = req ? req->uri : "/api/ei/organize";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  logAuthAttempt(true, req->uri, ctx.user, ctx.ip, "");

  extern bool filesystemReady;
  if (!filesystemReady) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"filesystem_not_ready\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  File dir = LittleFS.open(MODEL_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"models_dir_missing\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  int moved = 0;
  int failed = 0;

  // First pass: collect files at root level
  std::vector<String> tfliteFiles;
  std::vector<String> labelsFiles;
  
  File entry = dir.openNextFile();
  while (entry) {
    String full = String(entry.name());
    bool isDir = entry.isDirectory();
    entry.close();

    if (!isDir) {
      String fn = full;
      if (fn.startsWith("/EI Models/")) fn = fn.substring(11);
      if (fn.startsWith("/")) fn = fn.substring(1);
      // Only root-level files (no subdirs)
      if (fn.indexOf('/') == -1) {
        if (fn.endsWith(".tflite")) {
          tfliteFiles.push_back(fn);
        } else if (fn.endsWith(".labels.txt")) {
          labelsFiles.push_back(fn);
        }
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();

  // Process each .tflite file
  for (size_t i = 0; i < tfliteFiles.size(); i++) {
    String tflite = tfliteFiles[i];
    String modelName = tflite.substring(0, tflite.length() - 7); // Remove .tflite
    
    String srcModel = String(MODEL_DIR) + "/" + tflite;
    String dstDir = String(MODEL_DIR) + "/" + modelName;
    String dstModel = dstDir + "/" + tflite;
    
    // Create folder
    if (!LittleFS.exists(dstDir)) {
      if (!LittleFS.mkdir(dstDir)) {
        failed++;
        continue;
      }
    }
    
    // Move .tflite
    if (!LittleFS.exists(dstModel)) {
      if (LittleFS.rename(srcModel.c_str(), dstModel.c_str())) {
        moved++;
      } else {
        failed++;
        continue;
      }
    }
    
    // Look for matching labels file
    String labelsName = modelName + ".labels.txt";
    String srcLabels = String(MODEL_DIR) + "/" + labelsName;
    String dstLabels = dstDir + "/" + labelsName;
    
    if (LittleFS.exists(srcLabels) && !LittleFS.exists(dstLabels)) {
      if (LittleFS.rename(srcLabels.c_str(), dstLabels.c_str())) {
        moved++;
      }
    }
  }

  httpd_resp_set_type(req, "application/json");
  String json = "{\"success\":true,\"moved\":" + String(moved) + ",\"failed\":" + String(failed) + "}";
  httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t handleEdgeImpulseDetect(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = "/api/edgeimpulse/detect";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  
  // Run inference
  EIResults results = runEdgeImpulseInference();
  
  // Build combined JSON response with detections and tracked objects
  JsonDocument doc;
  
  doc["success"] = results.success;
  doc["inferenceTimeMs"] = results.inferenceTimeMs;
  
  if (results.errorMessage) {
    doc["error"] = results.errorMessage;
  }
  
  // Detections array
  JsonArray detections = doc["detections"].to<JsonArray>();
  for (int i = 0; i < results.detectionCount; i++) {
    JsonObject det = detections.add<JsonObject>();
    det["label"] = results.detections[i].label;
    det["confidence"] = results.detections[i].confidence;
    det["x"] = results.detections[i].x;
    det["y"] = results.detections[i].y;
    det["width"] = results.detections[i].width;
    det["height"] = results.detections[i].height;
  }
  
  // Tracked objects array
  JsonArray tracked = doc["trackedObjects"].to<JsonArray>();
  for (int i = 0; i < gTrackedObjectCount; i++) {
    JsonObject obj = tracked.add<JsonObject>();
    obj["label"] = gTrackedObjects[i].label;
    obj["prevLabel"] = gTrackedObjects[i].prevLabel;
    obj["confidence"] = gTrackedObjects[i].confidence;
    obj["x"] = gTrackedObjects[i].x;
    obj["y"] = gTrackedObjects[i].y;
    obj["width"] = gTrackedObjects[i].width;
    obj["height"] = gTrackedObjects[i].height;
    obj["stateChanged"] = gTrackedObjects[i].stateChanged;
  }
  
  String jsonOutput;
  serializeJson(doc, jsonOutput);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_send(req, jsonOutput.c_str(), jsonOutput.length());
  
  return ESP_OK;
}

// ============================================================================
// CLI Command Handlers
// ============================================================================

static char gEICmdBuffer[512];

const char* cmd_ei(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer),
    "Edge Impulse Commands:\n"
    "  ei enable <0|1>     - Enable/disable inference\n"
    "  ei detect           - Run single inference (from camera)\n"
    "  ei file <path>      - Run inference on stored JPEG file\n"
    "  ei continuous <0|1> - Start/stop continuous mode\n"
    "  ei confidence <val> - Set min confidence (0.0-1.0)\n"
    "  ei status           - Show current status\n"
    "  ei model ...        - Model management (list/load/info/unload)\n"
    "  ei track ...        - State change tracking (status/enable/clear)\n"
  );
  return gEICmdBuffer;
}

const char* cmd_ei_enable(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = cmd;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Edge Impulse: %s", 
      gSettings.edgeImpulseEnabled ? "enabled" : "disabled");
    return gEICmdBuffer;
  }
  
  int val = trimmed.toInt();
  gSettings.edgeImpulseEnabled = (val != 0);
  
  if (gSettings.edgeImpulseEnabled && !gEIInitialized) {
    initEdgeImpulse();
  }
  
  sensorStatusBumpWith(gSettings.edgeImpulseEnabled ? "ei_enable" : "ei_disable");
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Edge Impulse %s", 
    gSettings.edgeImpulseEnabled ? "enabled" : "disabled");
  return gEICmdBuffer;
}

const char* cmd_ei_detect(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  EIResults results = runEdgeImpulseInference();
  
  if (!results.success) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Detection failed: %s", 
      results.errorMessage ? results.errorMessage : "unknown error");
    return gEICmdBuffer;
  }
  
  if (results.detectionCount == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), 
      "No objects detected (inference: %lums)", results.inferenceTimeMs);
    return gEICmdBuffer;
  }
  
  String output;
  buildDetectionJson(results, output);
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "%s", output.c_str());
  return gEICmdBuffer;
}

const char* cmd_ei_file(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String path = args;
  path.trim();
  
  if (path.length() == 0) {
    return "Usage: ei file <path>\nExample: ei file /images/test.jpg";
  }
  
  // Ensure path starts with /
  if (!path.startsWith("/")) {
    path = "/" + path;
  }
  
  // Check file exists
  if (!LittleFS.exists(path)) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "File not found: %s", path.c_str());
    return gEICmdBuffer;
  }
  
  EIResults results = runInferenceFromFile(path.c_str());
  
  if (!results.success) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Inference failed: %s", 
      results.errorMessage ? results.errorMessage : "unknown error");
    return gEICmdBuffer;
  }
  
  if (results.detectionCount == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), 
      "No objects detected in %s (inference: %lums)", path.c_str(), results.inferenceTimeMs);
    return gEICmdBuffer;
  }
  
  String output;
  buildDetectionJson(results, output);
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "%s", output.c_str());
  return gEICmdBuffer;
}

const char* cmd_ei_continuous(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = cmd;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Continuous mode: %s", 
      gEIContinuousRunning ? "running" : "stopped");
    return gEICmdBuffer;
  }
  
  int val = trimmed.toInt();
  if (val) {
    startContinuousInference();
    return "Continuous inference started";
  } else {
    stopContinuousInference();
    return "Continuous inference stopped";
  }
}

const char* cmd_ei_confidence(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = cmd;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Min confidence: %.2f", 
      gSettings.edgeImpulseMinConfidence);
    return gEICmdBuffer;
  }
  
  float val = trimmed.toFloat();
  if (val < 0.0f) val = 0.0f;
  if (val > 1.0f) val = 1.0f;
  
  gSettings.edgeImpulseMinConfidence = val;
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Min confidence set to %.2f", val);
  return gEICmdBuffer;
}

const char* cmd_ei_status(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer),
    "Edge Impulse Status:\n"
    "  Initialized: %s\n"
    "  Model loaded: %s\n"
    "  Model path: %s\n"
    "  Enabled: %s\n"
    "  Continuous: %s\n"
    "  Min confidence: %.2f\n"
    "  Input size: %dx%d\n"
    "  Interval: %dms",
    gEIInitialized ? "yes" : "no",
    gEIModelLoaded ? "yes" : "no",
    gLoadedModelPath.length() > 0 ? gLoadedModelPath.c_str() : "(none)",
    gSettings.edgeImpulseEnabled ? "yes" : "no",
    gEIContinuousRunning ? "running" : "stopped",
    gSettings.edgeImpulseMinConfidence,
    gModelInputWidth, gModelInputHeight,
    gSettings.edgeImpulseIntervalMs
  );
  return gEICmdBuffer;
}

// Model management commands
const char* cmd_ei_model(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer),
    "Model Commands:\n"
    "  ei model list       - List available models in %s\n"
    "  ei model load <name>- Load a .tflite model\n"
    "  ei model info       - Show loaded model details\n"
    "  ei model unload     - Unload current model",
    MODEL_DIR
  );
  return gEICmdBuffer;
}

const char* cmd_ei_model_list(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String output;
  listAvailableModels(output);
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "%s", output.c_str());
  return gEICmdBuffer;
}

const char* cmd_ei_model_load(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = cmd;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    return "Usage: ei model load <filename or path>\nExample: ei model load default.tflite";
  }
  
  // Build full path if just filename given
  String path = trimmed;
  if (!path.startsWith("/")) {
    path = String(MODEL_DIR) + "/" + trimmed;
  }
  
  if (!LittleFS.exists(path)) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Model not found: %s", path.c_str());
    return gEICmdBuffer;
  }
  
  // Try to load model first - only initialize if model is valid
  if (loadModelFromFile(path.c_str())) {
    // Model loaded successfully - now initialize buffers if needed
    if (!gEIInitialized) {
      initEdgeImpulse();
    }
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), 
      "Model loaded: %s\nInput: %dx%dx%d\nArena: %zu bytes",
      path.c_str(), gModelInputWidth, gModelInputHeight, gModelInputChannels,
      gInterpreter ? gInterpreter->arena_used_bytes() : 0);
  } else {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Failed to load model: %s", path.c_str());
  }
  return gEICmdBuffer;
}

const char* cmd_ei_model_info(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gEIModelLoaded || !gInterpreter) {
    return "No model loaded. Use 'ei model load <filename>' to load one.";
  }
  
  const char* inputType = "unknown";
  if (gInputTensor) {
    switch (gInputTensor->type) {
      case kTfLiteFloat32: inputType = "float32"; break;
      case kTfLiteUInt8: inputType = "uint8"; break;
      case kTfLiteInt8: inputType = "int8"; break;
      default: inputType = "other"; break;
    }
  }
  
  const char* outputType = "unknown";
  if (gOutputTensor) {
    switch (gOutputTensor->type) {
      case kTfLiteFloat32: outputType = "float32"; break;
      case kTfLiteUInt8: outputType = "uint8"; break;
      case kTfLiteInt8: outputType = "int8"; break;
      default: outputType = "other"; break;
    }
  }
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer),
    "Loaded Model Info:\n"
    "  Path: %s\n"
    "  Size: %zu bytes\n"
    "  Input: %dx%dx%d (%s)\n"
    "  Output dims: %d (%s)\n"
    "  Arena used: %zu bytes",
    gLoadedModelPath.c_str(),
    gModelSize,
    gModelInputWidth, gModelInputHeight, gModelInputChannels, inputType,
    gOutputTensor ? gOutputTensor->dims->data[gOutputTensor->dims->size - 1] : 0, outputType,
    gInterpreter->arena_used_bytes()
  );
  return gEICmdBuffer;
}

const char* cmd_ei_model_unload(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gEIModelLoaded) {
    return "No model is currently loaded.";
  }
  
  String oldPath = gLoadedModelPath;
  unloadModel();
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Model unloaded: %s", oldPath.c_str());
  return gEICmdBuffer;
}

// State tracking commands
const char* cmd_ei_track(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer),
    "State Tracking Commands:\n"
    "  ei track status     - Show tracked objects\n"
    "  ei track enable <0|1> - Enable/disable tracking\n"
    "  ei track clear      - Clear tracked objects"
  );
  return gEICmdBuffer;
}

const char* cmd_ei_track_status(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gStateTrackingEnabled) {
    return "State tracking is disabled. Use 'ei track enable 1' to enable.";
  }
  
  if (gTrackedObjectCount == 0) {
    return "No objects currently tracked. Run continuous inference to track objects.";
  }
  
  String output = "Tracked Objects (" + String(gTrackedObjectCount) + "):\n";
  uint32_t now = millis();
  
  for (int i = 0; i < gTrackedObjectCount; i++) {
    const TrackedObject& obj = gTrackedObjects[i];
    output += "  [" + String(i) + "] " + obj.label;
    output += " at (" + String(obj.x) + "," + String(obj.y) + ")";
    output += " conf=" + String(obj.confidence, 2);
    if (obj.prevLabel[0]) {
      output += " prev=" + String(obj.prevLabel);
    }
    output += " age=" + String((now - obj.lastSeenMs) / 1000.0f, 1) + "s";
    if (obj.stateChanged) {
      output += " [CHANGED]";
    }
    output += "\n";
  }
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "%s", output.c_str());
  return gEICmdBuffer;
}

const char* cmd_ei_track_enable(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = cmd;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "State tracking: %s",
             gStateTrackingEnabled ? "enabled" : "disabled");
    return gEICmdBuffer;
  }
  
  int val = trimmed.toInt();
  setStateTrackingEnabled(val != 0);
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "State tracking %s",
           gStateTrackingEnabled ? "enabled" : "disabled");
  return gEICmdBuffer;
}

const char* cmd_ei_track_clear(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  int count = gTrackedObjectCount;
  gTrackedObjectCount = 0;
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Cleared %d tracked objects", count);
  return gEICmdBuffer;
}

// ============================================================================
// Command Registration
// ============================================================================

const CommandEntry edgeImpulseCommands[] = {
  { "ei", "Edge Impulse ML inference commands.", false, cmd_ei, "Usage: ei <subcommand>" },
  { "ei enable", "Enable/disable Edge Impulse inference.", false, cmd_ei_enable, "Usage: ei enable <0|1>" },
  { "ei detect", "Run single object detection inference.", false, cmd_ei_detect, "Usage: ei detect" },
  { "ei file", "Run inference on stored JPEG image.", false, cmd_ei_file, "Usage: ei file <path>" },
  { "ei continuous", "Start/stop continuous inference mode.", false, cmd_ei_continuous, "Usage: ei continuous <0|1>" },
  { "ei confidence", "Set minimum detection confidence.", false, cmd_ei_confidence, "Usage: ei confidence <0.0-1.0>" },
  { "ei status", "Show Edge Impulse status.", false, cmd_ei_status, "Usage: ei status" },
  { "ei model", "Model management commands.", false, cmd_ei_model, "Usage: ei model <subcommand>" },
  { "ei model list", "List available .tflite models.", false, cmd_ei_model_list, "Usage: ei model list" },
  { "ei model load", "Load a TFLite model from LittleFS.", false, cmd_ei_model_load, "Usage: ei model load <filename>" },
  { "ei model info", "Show loaded model information.", false, cmd_ei_model_info, "Usage: ei model info" },
  { "ei model unload", "Unload the current model.", false, cmd_ei_model_unload, "Usage: ei model unload" },
  { "ei track", "State tracking commands.", false, cmd_ei_track, "Usage: ei track <subcommand>" },
  { "ei track status", "Show currently tracked objects.", false, cmd_ei_track_status, "Usage: ei track status" },
  { "ei track enable", "Enable/disable state tracking.", false, cmd_ei_track_enable, "Usage: ei track enable <0|1>" },
  { "ei track clear", "Clear all tracked objects.", false, cmd_ei_track_clear, "Usage: ei track clear" }
};

const size_t edgeImpulseCommandsCount = sizeof(edgeImpulseCommands) / sizeof(edgeImpulseCommands[0]);

// Auto-register commands at startup
REGISTER_COMMAND_MODULE(edgeImpulseCommands, edgeImpulseCommandsCount, "EdgeImpulse");

// =============================================================================
// Register Edge Impulse Web Handlers
// =============================================================================

void registerEdgeImpulseHandlers(httpd_handle_t server) {
  static httpd_uri_t eiOrganizePost = { .uri = "/api/ei/organize", .method = HTTP_POST, .handler = handleEIOrganize, .user_ctx = NULL };
  static httpd_uri_t edgeImpulseDetect = { .uri = "/api/edgeimpulse/detect", .method = HTTP_GET, .handler = handleEdgeImpulseDetect, .user_ctx = NULL };
  httpd_register_uri_handler(server, &eiOrganizePost);
  httpd_register_uri_handler(server, &edgeImpulseDetect);
}

#endif // ENABLE_EDGE_IMPULSE
