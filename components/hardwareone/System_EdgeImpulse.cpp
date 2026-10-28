#include "System_EdgeImpulse.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_Command.h"     // CommandArgs
#include "System_Filesystem.h"  // requireQuotedPath (uniform quoted-path rule)
#include <esp_attr.h>

#if ENABLE_EDGE_IMPULSE

#include "System_Settings.h"
#include "System_Utils.h"  // validate-only command guard
#include "System_Debug.h"
#include "System_Camera_DVP.h"
#include "System_MemUtil.h"
#include "System_TaskUtils.h"
#include "HAL_JPEG.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "System_VFS.h"
#include "System_AuthIdentity.h"
#include "System_Mutex.h"
#include "System_I2C.h"        // sensorStatusBumpWith
#include <atomic>

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

// Largest model input side accepted at load (camera frames are capped at VGA).
constexpr int kMaxModelInputSide = 640;

// Upper bound on label slots, whatever the output tensor claims.
constexpr int kMaxModelClasses = 1024;

// How long a stop waits for the continuous task to finish its current frame.
constexpr uint32_t kContinuousJoinTimeoutMs = 15000;

static const char* const kEIBusyMessage = "Edge Impulse busy";

// Model storage path (relative to LittleFS mount point)
static const char* MODEL_DIR = "/EI Models";
static const char* DEFAULT_MODEL = "/EI Models/default.tflite";

// ============================================================================
// Edge Impulse Module State
// ============================================================================

static bool gEIInitialized = false;
static bool gEIModelLoaded = false;
static std::atomic<bool> gEIContinuousRunning{false};
static TaskHandle_t gEIContinuousTask = nullptr;  // owned under the lifecycle lock

// Model-input resize buffer: RGB888 at the model's input width x height,
// allocated by the model load and freed with the model.
static uint8_t* gResizedBuffer = nullptr;
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
// Input geometry, read from the loaded model's input tensor (never a setting).
static int gModelInputWidth = 0;
static int gModelInputHeight = 0;
static int gModelInputChannels = 0;
// Class count: last dimension of the loaded model's output tensor.
static int gModelClassCount = 0;

// ============================================================================
// Locks
// ============================================================================
// Inference lock: one interpreter, tensor arena, resize buffer and tracking
// table serve every caller. An inference holds this lock from capture to
// results; load and unload hold it while they free or rebuild the model.
// Detect commands only try it and report busy.
static SemaphoreHandle_t eiInferenceMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}

struct EIInferenceLock {
  bool held;
  explicit EIInferenceLock(TickType_t wait)
      : held(xSemaphoreTake(eiInferenceMutex(), wait) == pdTRUE) {}
  ~EIInferenceLock() {
    if (held) xSemaphoreGive(eiInferenceMutex());
  }
  EIInferenceLock(const EIInferenceLock&) = delete;
  EIInferenceLock& operator=(const EIInferenceLock&) = delete;
};

// Lifecycle lock: serializes start, stop, load and unload (and so the
// continuous task handle). Always taken before the inference lock.
static SemaphoreHandle_t eiLifecycleMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}

struct EILifecycleLock {
  EILifecycleLock() { xSemaphoreTake(eiLifecycleMutex(), portMAX_DELAY); }
  ~EILifecycleLock() { xSemaphoreGive(eiLifecycleMutex()); }
  EILifecycleLock(const EILifecycleLock&) = delete;
  EILifecycleLock& operator=(const EILifecycleLock&) = delete;
};

// Continuous-task handshake: a stop gives "wake" to cut the interval sleep
// short; the task gives "done" as its last action, which the stop waits for.
static SemaphoreHandle_t eiContinuousWake() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateBinaryStatic(&storage);
  return handle;
}

static SemaphoreHandle_t eiContinuousDone() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateBinaryStatic(&storage);
  return handle;
}

// ============================================================================
// Labels
// ============================================================================
// One fixed-size slot per model class (the output tensor's last dimension),
// filled from <model>.labels.txt. Detections copy their label out of this
// table, so nothing keeps a pointer into it.
static char* gModelLabels = nullptr;  // gModelLabelSlots * EI_LABEL_MAX bytes
static int gModelLabelSlots = 0;
static int gModelLabelCount = 0;      // slots filled from the labels file

// Free label memory
static void freeLabels() {
  free(gModelLabels);
  gModelLabels = nullptr;
  gModelLabelSlots = 0;
  gModelLabelCount = 0;
}

static bool loadLabelsFromExplicitPath(const String& labelsPath, int classCount) {
  if (classCount <= 0) return false;
  if (classCount > kMaxModelClasses) classCount = kMaxModelClasses;

  FsLockGuard fsGuard("edge.labels_load");
  if (!VFS::existsGuarded(labelsPath, VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.labels_load"))) {
    return false;
  }

  File labelsFile = VFS::openGuarded(labelsPath, "r", VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.labels_load"));
  if (!labelsFile) {
    return false;
  }

  const size_t tableBytes = static_cast<size_t>(classCount) * EI_LABEL_MAX;
  gModelLabels = static_cast<char*>(ps_alloc(tableBytes, AllocPref::PreferPSRAM, "ei.labels"));
  if (!gModelLabels) {
    labelsFile.close();
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate %d label slots", classCount);
    return false;
  }
  memset(gModelLabels, 0, tableBytes);
  gModelLabelSlots = classCount;

  // Lines past the model's class count name no class and are ignored.
  while (labelsFile.available() && gModelLabelCount < gModelLabelSlots) {
    String line = labelsFile.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      char* slot = gModelLabels + static_cast<size_t>(gModelLabelCount) * EI_LABEL_MAX;
      strncpy(slot, line.c_str(), EI_LABEL_MAX - 1);
      DEBUG_SYSTEMF("[EI_DEBUG] Label[%d] = '%s'", gModelLabelCount, slot);
      gModelLabelCount++;
    }
  }
  labelsFile.close();
  return gModelLabelCount > 0;
}

// Load labels file for model, one slot per model class.
// Expected structure: /EI Models/<modelname>/<modelname>.tflite + <modelname>.labels.txt
static bool loadLabelsFromFile(const char* modelPath, int classCount) {
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
  
  if (loadLabelsFromExplicitPath(labelsPath, classCount)) {
    DEBUG_SYSTEMF("[EI] Loaded %d labels for %d classes from %s",
                  gModelLabelCount, classCount, labelsPath.c_str());
    return true;
  }

  ERROR_SYSTEMF("[EI] Labels file not found: %s", labelsPath.c_str());
  return false;
}

// Copy the label for an output class into a detection's own storage. Classes
// the labels file does not name get "class_<index>".
static void copyLabelForIndex(int index, char* out, size_t outSize) {
  if (gModelLabels && index >= 0 && index < gModelLabelCount) {
    snprintf(out, outSize, "%s", gModelLabels + static_cast<size_t>(index) * EI_LABEL_MAX);
  } else {
    snprintf(out, outSize, "class_%d", index);
  }
}

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
  uint8_t seenFrames;       // Consecutive frames this object has existed
  bool announced;           // SYSEVT_EI_DETECTED posted (gates the LOST event too)
};

#define MAX_TRACKED_OBJECTS 5
#define STATE_STABLE_FRAMES 3   // Require N consecutive frames to confirm state
#define OBJECT_TIMEOUT_MS 2000  // Object considered gone after this

// Guarded by the inference lock.
static TrackedObject gTrackedObjects[MAX_TRACKED_OBJECTS];
static int gTrackedObjectCount = 0;
static bool gStateTrackingEnabled = true;

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

// Update tracked objects with new detections. Holds: inference lock.
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
      if (tracked->seenFrames < 255) tracked->seenFrames++;

      // Bus event once the object has survived 3 frames — raw first-frame
      // appearances flicker at the FOMO confidence threshold, so the appear
      // event gets the same stability treatment label changes already have.
      if (!tracked->announced && tracked->seenFrames >= 3) {
        tracked->announced = true;
        char det2[32];
        snprintf(det2, sizeof(det2), "%.0f%% at %d,%d", (double)(tracked->confidence * 100.0f),
                 tracked->x, tracked->y);
        systemEventPost(SYSEVT_EI_DETECTED, tracked->label, det2);
      }
      
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

          // Keep the detected/lost event pairing consistent across a label
          // morph: the old label "leaves" and the new one "appears", so an
          // automation counting matches per label never sees a lost with no
          // prior detected (or vice versa).
          if (tracked->announced) {
            systemEventPost(SYSEVT_EI_LOST, tracked->prevLabel);
            char det2[32];
            snprintf(det2, sizeof(det2), "%.0f%% at %d,%d", (double)(tracked->confidence * 100.0f),
                     tracked->x, tracked->y);
            systemEventPost(SYSEVT_EI_DETECTED, tracked->label, det2);
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
      newObj->seenFrames = 1;
      newObj->announced = false;
      
      DEBUG_SYSTEMF("[EdgeImpulse] New tracked object: %s at (%d,%d)", 
                    newObj->label, newObj->x, newObj->y);
    }
  }
  
  // Remove stale objects
  for (int i = gTrackedObjectCount - 1; i >= 0; i--) {
    if (now - gTrackedObjects[i].lastSeenMs > OBJECT_TIMEOUT_MS) {
      DEBUG_SYSTEMF("[EdgeImpulse] Object lost: %s at (%d,%d)",
                    gTrackedObjects[i].label, gTrackedObjects[i].x, gTrackedObjects[i].y);
      // Only objects that were announced get a LOST event — un-announced
      // flickers would otherwise post orphan losses.
      if (gTrackedObjects[i].announced) {
        systemEventPost(SYSEVT_EI_LOST, gTrackedObjects[i].label);
      }
      // Shift remaining objects down
      for (int j = i; j < gTrackedObjectCount - 1; j++) {
        gTrackedObjects[j] = gTrackedObjects[j + 1];
      }
      gTrackedObjectCount--;
    }
  }
}

// Public: Enable/disable state tracking
void setStateTrackingEnabled(bool enabled) {
  EIInferenceLock lock(portMAX_DELAY);
  gStateTrackingEnabled = enabled;
  if (!enabled) {
    gTrackedObjectCount = 0;
  }
}

// ============================================================================
// Settings Module Definition
// ============================================================================

// The model input size is not a setting: it comes from the loaded model's
// input tensor and is reported by eistatus, eimodelinfo and the detection
// JSON. A settings.json that still carries "inputSize" loads unchanged; the
// loader only reads the keys listed here.
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry edgeImpulseSettingEntries[] = {
  { "eiAutoStart", SETTING_BOOL, &gSettings.eiAutoStart, 1, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, nullptr, "eiautostart" },
  { "enabled", SETTING_BOOL, &gSettings.eiEnabled, 0, 0, nullptr, 0, 1, "Enable Inference", nullptr, false, nullptr, "eienable" },
  { "requireLabels", SETTING_BOOL, &gSettings.edgeImpulseRequireLabels, 1, 0, nullptr, 0, 1, "Require Labels", nullptr, false, nullptr, "eirequirelabels" },
  { "minConfidence", SETTING_FLOAT, &gSettings.edgeImpulseMinConfidence, 0, 0.6f, nullptr, 0, 1, "Min Confidence", nullptr, false, nullptr, "eiconfidence" },
  { "maxDetections", SETTING_INT, &gSettings.edgeImpulseMaxDetections, 5, 0, nullptr, 1, 10, "Max Detections", nullptr, false, nullptr, "eimaxdetections" },
  { "continuous", SETTING_BOOL, &gSettings.edgeImpulseContinuous, 0, 0, nullptr, 0, 1, "Continuous Mode", nullptr, false, nullptr, "eicontinuous" },
  { "intervalMs", SETTING_INT, &gSettings.edgeImpulseIntervalMs, 1000, 0, nullptr, 100, 10000, "Interval (ms)", nullptr, false, nullptr, "eiinterval" },
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule edgeImpulseSettingsModule = {
  "edgeimpulse",
  "apps.edgeimpulse",
  edgeImpulseSettingEntries,
  sizeof(edgeImpulseSettingEntries) / sizeof(edgeImpulseSettingEntries[0]),
  nullptr,
  "Edge Impulse on-device object detection"
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

// Bytes per element for the tensor types the pipeline reads and writes;
// 0 for any other type.
static size_t tensorElementBytes(TfLiteType type) {
  switch (type) {
    case kTfLiteFloat32: return sizeof(float);
    case kTfLiteUInt8: return sizeof(uint8_t);
    case kTfLiteInt8: return sizeof(int8_t);
    default: return 0;
  }
}

// The input tensor is the only source of input geometry. Accepts [1,H,W,C] or
// [H,W,C] with C = 1 (grayscale) or 3 (RGB) and a float32/uint8/int8 element
// type whose element count matches the tensor's bytes, so every fill writes
// exactly input->bytes.
static bool configureInputGeometry(const TfLiteTensor* input) {
  gModelInputWidth = 0;
  gModelInputHeight = 0;
  gModelInputChannels = 0;
  if (!input || !input->dims) return false;
  const TfLiteIntArray* dims = input->dims;
  int first = 0;
  if (dims->size == 4) {
    first = 1;
  } else if (dims->size != 3) {
    return false;
  }
  const int height = dims->data[first];
  const int width = dims->data[first + 1];
  const int channels = dims->data[first + 2];
  if (width <= 0 || height <= 0 || width > kMaxModelInputSide || height > kMaxModelInputSide) {
    return false;
  }
  if (channels != 1 && channels != 3) return false;
  const size_t elementBytes = tensorElementBytes(input->type);
  if (!elementBytes ||
      static_cast<size_t>(width) * height * channels * elementBytes != input->bytes) {
    return false;
  }
  gModelInputWidth = width;
  gModelInputHeight = height;
  gModelInputChannels = channels;
  return true;
}

// Output tensor: [1, gridH, gridW, classes] (FOMO) or [..., classes]
// (classification) of float32/uint8/int8, whose element count matches its
// bytes so the parser never reads past it.
static bool configureOutputShape(const TfLiteTensor* output) {
  gModelClassCount = 0;
  if (!output || !output->dims || output->dims->size < 1) return false;
  const size_t elementBytes = tensorElementBytes(output->type);
  if (!elementBytes) return false;
  size_t elements = 1;
  for (int i = 0; i < output->dims->size; i++) {
    if (output->dims->data[i] <= 0) return false;
    elements *= static_cast<size_t>(output->dims->data[i]);
  }
  if (elements * elementBytes != output->bytes) return false;
  gModelClassCount = output->dims->data[output->dims->size - 1];
  return true;
}

// Holds: inference lock (or runs before any inference can see the model).
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

  free(gResizedBuffer);
  gResizedBuffer = nullptr;
  gResizedBufferSize = 0;

  gLoadedModelPath = "";
  gModelInputWidth = 0;
  gModelInputHeight = 0;
  gModelInputChannels = 0;
  gModelClassCount = 0;
  gEIModelLoaded = false;

  // Free label memory
  freeLabels();

  DEBUG_SYSTEMF("[EI_DEBUG]   Heap after cleanup: %lu free", (unsigned long)ESP.getFreeHeap());
}

// Holds: inference lock, with no continuous task running.
static bool loadModelLocked(const char* path) {
  DEBUG_SYSTEMF("[EI_DEBUG] ========== loadModelFromFile() START ==========");
  DEBUG_SYSTEMF("[EI_DEBUG] Path: %s", path);
  DEBUG_SYSTEMF("[EI_DEBUG] Heap before load: %lu free, PSRAM: %lu free",
                (unsigned long)ESP.getFreeHeap(), 
                (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
  
  // Free any existing model
  freeModelResources();
  
  size_t fileSize = 0;
  size_t bytesRead = 0;
  uint32_t readTime = 0;
  {
    FsLockGuard fsGuard("edge.model_load");
    File modelFile = VFS::openGuarded(path, "r", VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.model_load"));
    if (!modelFile) {
      ERROR_SYSTEMF("[EdgeImpulse] Failed to open model file: %s", path);
      return false;
    }

    fileSize = modelFile.size();
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

    DEBUG_SYSTEMF("[EI_DEBUG] Allocating model buffer: %zu bytes", fileSize);
    gModelBuffer = (uint8_t*)ps_alloc(fileSize, AllocPref::PreferPSRAM, "ei.model");
    if (!gModelBuffer) {
      ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate model buffer (%zu bytes)", fileSize);
      DEBUG_SYSTEMF("[EI_DEBUG] Allocation FAILED! Heap: %lu, PSRAM: %lu",
                    (unsigned long)ESP.getFreeHeap(),
                    (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
      modelFile.close();
      return false;
    }
    DEBUG_SYSTEMF("[EI_DEBUG] Model buffer allocated at %p", gModelBuffer);

    DEBUG_SYSTEMF("[EI_DEBUG] Reading model data from file...");
    uint32_t readStart = millis();
    bytesRead = modelFile.read(gModelBuffer, fileSize);
    readTime = millis() - readStart;
    modelFile.close();
  }
  
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
  DEBUG_SYSTEMF("[EI_DEBUG] Allocating tensor arena: %zu KB", kTensorArenaSize / 1024);
  gTensorArena = (uint8_t*)ps_alloc(kTensorArenaSize, AllocPref::PreferPSRAM, "ei.arena");
  
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
  
  // Input geometry (NHWC [batch, height, width, channels] or HWC) comes from
  // the tensor alone and must account for exactly its bytes.
  if (!configureInputGeometry(gInputTensor)) {
    ERROR_SYSTEMF("[EdgeImpulse] Unsupported input tensor (type=%d, bytes=%zu): need HxWx1 or HxWx3 "
                  "float32/uint8/int8, sides up to %d",
                  gInputTensor->type, gInputTensor->bytes, kMaxModelInputSide);
    freeModelResources();
    return false;
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Input geometry: %dx%dx%d",
                gModelInputWidth, gModelInputHeight, gModelInputChannels);

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
  if (!configureOutputShape(gOutputTensor)) {
    ERROR_SYSTEMF("[EdgeImpulse] Unsupported output tensor: need float32/uint8/int8 class scores");
    freeModelResources();
    return false;
  }

  // The resize buffer holds one RGB888 frame at the model's input geometry.
  gResizedBufferSize = static_cast<size_t>(gModelInputWidth) * gModelInputHeight * 3;
  gResizedBuffer = static_cast<uint8_t*>(ps_alloc(gResizedBufferSize, AllocPref::PreferPSRAM, "ei.resized"));
  if (!gResizedBuffer) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate %zu-byte resize buffer", gResizedBufferSize);
    freeModelResources();
    return false;
  }

  gLoadedModelPath = path;
  gEIModelLoaded = true;

  // Try to load labels from labels.txt, one slot per output class
  bool haveLabels = loadLabelsFromFile(path, gModelClassCount);
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

  return true;
}

static bool joinContinuousTaskLocked();
static bool spawnContinuousTaskLocked();

bool loadModelFromFile(const char* path) {
  EILifecycleLock lifecycle;
  // Continuous mode pauses for the swap and resumes on the new model.
  const bool resume = gEIContinuousTask != nullptr && gEIContinuousRunning;
  if (!joinContinuousTaskLocked()) {
    ERROR_SYSTEMF("[EdgeImpulse] Model not loaded: continuous inference did not stop");
    return false;
  }
  bool loaded = false;
  {
    EIInferenceLock inference(portMAX_DELAY);
    loaded = loadModelLocked(path);
  }
  if (resume) {
    if (!loaded || !spawnContinuousTaskLocked()) {
      ERROR_SYSTEMF("[EdgeImpulse] Continuous inference stopped: model load failed");
      setSetting(gSettings.edgeImpulseContinuous, false);
    }
  }
  return loaded;
}

void unloadModel() {
  DEBUG_SYSTEMF("[EI_DEBUG] unloadModel() called");
  EILifecycleLock lifecycle;
  DEBUG_SYSTEMF("[EI_DEBUG]   Current model: %s",
                gLoadedModelPath.length() > 0 ? gLoadedModelPath.c_str() : "(none)");

  // Stop continuous inference and wait for its task to exit
  if (gEIContinuousTask) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Stopping continuous inference first...");
    joinContinuousTaskLocked();
    setSetting(gSettings.edgeImpulseContinuous, false);
  }

  // Waits for any inference still holding the model.
  EIInferenceLock inference(portMAX_DELAY);
  freeModelResources();
  DEBUG_SYSTEMF("[EI_DEBUG]   Model unloaded successfully");
}

static void listModelsRecursive(const String& absDir, const String& relPrefix, String& output, int& count) {
  FsLockGuard fsGuard("edge.list_models");
  File dir = VFS::openGuarded(absDir, "r", VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.list_models"));
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
        char fullPathBuf[96];
        snprintf(fullPathBuf, sizeof(fullPathBuf), "%s/%s", MODEL_DIR, rel.c_str());
        char lineBuf[128];
        snprintf(lineBuf, sizeof(lineBuf), "  %s (%d bytes)", rel.c_str(), (int)file.size());
        output += lineBuf;
        if (gLoadedModelPath == fullPathBuf) {
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
  FsLockGuard fsGuard("edge.list_models");
  char hdrBuf[64];
  snprintf(hdrBuf, sizeof(hdrBuf), "Available models in %s:\n", MODEL_DIR);
  output = hdrBuf;
  
  File root = VFS::openGuarded(MODEL_DIR, "r", VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.list_models"));
  if (!root || !root.isDirectory()) {
    char errBuf[64];
    snprintf(errBuf, sizeof(errBuf), "  (directory not found - create %s)\n", MODEL_DIR);
    output += errBuf;
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
  if (!VFS::existsGuarded(MODEL_DIR, VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.ensure_model_dir"))) {
    VFS::mkdirGuarded(MODEL_DIR, VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.ensure_model_dir"));
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

// Fill the input tensor from an RGB888 frame already at the model geometry.
// Writes exactly width * height * channels elements, which
// configureInputGeometry matched to input->bytes at load. One-channel models
// get luma (BT.601 weights).
static void fillInputTensor(TfLiteTensor* input, const uint8_t* rgb,
                            int width, int height, int channels) {
  const size_t pixels = static_cast<size_t>(width) * height;
  for (size_t p = 0; p < pixels; p++) {
    const uint8_t* px = rgb + p * 3;
    uint8_t values[3] = {px[0], px[1], px[2]};
    if (channels == 1) {
      values[0] = static_cast<uint8_t>((77u * px[0] + 150u * px[1] + 29u * px[2]) >> 8);
    }
    for (int c = 0; c < channels; c++) {
      const size_t i = p * channels + c;
      switch (input->type) {
        case kTfLiteFloat32: input->data.f[i] = values[c] / 255.0f; break;
        case kTfLiteUInt8: input->data.uint8[i] = values[c]; break;
        case kTfLiteInt8: input->data.int8[i] = static_cast<int8_t>(values[c] - 128); break;
        default: return;  // rejected at load
      }
    }
  }
}

// ============================================================================
// Initialization
// ============================================================================

// Mark the module ready for inference (the model directory exists). Model
// buffers belong to the model and are allocated by the model load.
static void markEdgeImpulseReady() {
  if (gEIInitialized) return;
  DEBUG_SYSTEMF("[EI_DEBUG] Ensuring model directory exists...");
  ensureModelDirectory();
  gEIInitialized = true;
  DEBUG_SYSTEMF("[EI_DEBUG] Edge Impulse initialized successfully");
}

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
  DEBUG_SYSTEMF("[EI_DEBUG] Settings: minConf=%.2f, interval=%dms",
                gSettings.edgeImpulseMinConfidence,
                gSettings.edgeImpulseIntervalMs);

  markEdgeImpulseReady();

  // Try to load default model if it exists
  DEBUG_SYSTEMF("[EI_DEBUG] Checking for default model: %s", DEFAULT_MODEL);
  if (VFS::existsGuarded(DEFAULT_MODEL, VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.default_model_check"))) {
    DEBUG_SYSTEMF("[EI_DEBUG] Default model found, loading...");
    if (loadModelFromFile(DEFAULT_MODEL)) {
      DEBUG_SYSTEMF("[EI_DEBUG] Default model loaded successfully");
    } else {
      ERROR_SYSTEMF("[EdgeImpulse] Failed to load default model");
    }
  } else {
    DEBUG_SYSTEMF("[EI_DEBUG] No default model at %s", DEFAULT_MODEL);

    // List available models
    File dir = VFS::openGuarded(MODEL_DIR, "r", VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.list_models_init"));
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

// Decoded pixels go to PSRAM; hwjpeg::Image releases them with free().
static uint8_t* allocateEdgeImpulseRgb(size_t bytes) {
  return static_cast<uint8_t*>(ps_alloc(bytes, AllocPref::PreferPSRAM, "ei.rgb"));
}

// Decode a JPEG through the HAL_JPEG owner, capped at VGA. On success `image`
// owns the RGB888 pixels and their geometry; on failure it is empty and
// `error` says why.
static bool decodeEdgeImpulseJpeg(const uint8_t* jpeg, size_t length,
                                  hwjpeg::Image& image, const char** error) {
  hwjpeg::DecodeOptions options;
  options.softwareAllocator = allocateEdgeImpulseRgb;
  options.maxWidth = 640;
  options.maxHeight = 480;
  options.maxOutputBytes = 640u * 480u * 3u;
  return hwjpeg::decode(jpeg, length, image, options, error);
}

// Dequantized confidence of element `index` of the output tensor.
static float outputConfidence(const TfLiteTensor* output, int index) {
  switch (output->type) {
    case kTfLiteFloat32:
      return output->data.f[index];
    case kTfLiteUInt8:
      return (output->data.uint8[index] - output->params.zero_point) * output->params.scale;
    case kTfLiteInt8:
      return (output->data.int8[index] - output->params.zero_point) * output->params.scale;
    default:
      return 0.0f;  // rejected at load
  }
}

static void addDetection(EIResults& results, int classIndex, float confidence,
                         int x, int y, int width, int height) {
  EIDetection& det = results.detections[results.detectionCount++];
  copyLabelForIndex(classIndex, det.label, sizeof(det.label));
  det.confidence = confidence;
  det.x = x;
  det.y = y;
  det.width = width;
  det.height = height;
}

// Parse the output tensor into detections; returns the highest confidence
// seen. FOMO grids ([1, gridH, gridW, classes], class 0 = background) give one
// box per grid cell above the threshold; any other shape is per-class scores
// over the whole input. Holds: inference lock.
static float parseOutputTensor(EIResults& results) {
  results.detectionCount = 0;
  int maxDetections = gSettings.edgeImpulseMaxDetections;
  if (maxDetections < 1) maxDetections = 1;
  if (maxDetections > EI_MAX_DETECTIONS) maxDetections = EI_MAX_DETECTIONS;
  const float threshold = gSettings.edgeImpulseMinConfidence;
  const TfLiteIntArray* dims = gOutputTensor->dims;

  float maxConf = 0.0f;
  int maxIdx = -1;
  if (dims->size == 4) {
    const int gridHeight = dims->data[1];
    const int gridWidth = dims->data[2];
    const int numClasses = dims->data[3];
    const int cellWidth = gModelInputWidth / gridWidth;
    const int cellHeight = gModelInputHeight / gridHeight;
    DEBUG_SYSTEMF("[EI_DEBUG]   FOMO grid=%dx%d, classes=%d", gridHeight, gridWidth, numClasses);
    for (int y = 0; y < gridHeight; y++) {
      for (int x = 0; x < gridWidth; x++) {
        const int cellOffset = (y * gridWidth + x) * numClasses;
        float bestConf = 0.0f;
        int bestClass = -1;
        for (int c = 1; c < numClasses; c++) {  // class 0 is background
          const float conf = outputConfidence(gOutputTensor, cellOffset + c);
          if (conf > bestConf) {
            bestConf = conf;
            bestClass = c;
          }
        }
        if (bestConf >= threshold && results.detectionCount < maxDetections) {
          addDetection(results, bestClass, bestConf,
                       x * cellWidth, y * cellHeight, cellWidth, cellHeight);
        }
        if (bestConf > maxConf) {
          maxConf = bestConf;
          maxIdx = bestClass;
        }
      }
    }
  } else {
    for (int i = 0; i < gModelClassCount && results.detectionCount < maxDetections; i++) {
      const float confidence = outputConfidence(gOutputTensor, i);
      if (confidence > maxConf) {
        maxConf = confidence;
        maxIdx = i;
      }
      if (confidence >= threshold) {
        addDetection(results, i, confidence, 0, 0, gModelInputWidth, gModelInputHeight);
      }
    }
  }
  DEBUG_SYSTEMF("[EI_DEBUG]   Output: max=%.4f at class=%d, %d detections",
                maxConf, maxIdx, results.detectionCount);
  return maxConf;
}

// The one inference pipeline, shared by the camera and stored-file entry
// points: resize a decoded RGB888 frame to the model's input geometry, fill
// the input tensor, run the interpreter and parse the output. inferenceTimeMs
// covers these steps. Holds: inference lock.
static void inferRgb(const uint8_t* rgb, int width, int height, EIResults& results) {
  if (!gEIModelLoaded || !gInterpreter || !gInputTensor || !gOutputTensor || !gResizedBuffer) {
    results.errorMessage = "Interpreter not ready";
    return;
  }
  const uint32_t start = millis();
  DEBUG_SYSTEMF("[EI_DEBUG] Resizing %dx%d -> %dx%dx%d", width, height,
                gModelInputWidth, gModelInputHeight, gModelInputChannels);
  if (!resizeRgb888(rgb, width, height, gResizedBuffer, gModelInputWidth, gModelInputHeight)) {
    results.errorMessage = "Failed to resize image";
    return;
  }
  fillInputTensor(gInputTensor, gResizedBuffer, gModelInputWidth, gModelInputHeight,
                  gModelInputChannels);

  const uint32_t invokeStart = millis();
  const TfLiteStatus invokeStatus = gInterpreter->Invoke();
  DEBUG_SYSTEMF("[EI_DEBUG]   Invoke returned %d in %lu ms", invokeStatus,
                (unsigned long)(millis() - invokeStart));
  if (invokeStatus != kTfLiteOk) {
    results.errorMessage = "Inference failed";
    return;
  }

  const float maxConf = parseOutputTensor(results);
  results.inferenceTimeMs = millis() - start;
  results.success = true;
  INFO_SYSTEMF("[EdgeImpulse] Inference: %lums | Max confidence: %.3f | Detections: %d (threshold: %.2f)",
               (unsigned long)results.inferenceTimeMs, maxConf, results.detectionCount,
               gSettings.edgeImpulseMinConfidence);
}

// Checks every entry point applies before it acquires a frame.
static const char* inferencePreflight() {
  if (!gEIInitialized) return "Edge Impulse not initialized";
  if (!gSettings.eiEnabled) return "Edge Impulse disabled";
  if (!gEIModelLoaded) return "No model loaded";
  return nullptr;
}

// Camera entry point. Holds: inference lock, from capture to results.
static EIResults runCameraInferenceLocked() {
  EIResults results = {};
  if (const char* reason = inferencePreflight()) {
    results.errorMessage = reason;
    return results;
  }
  if (!cameraConnected) {
    results.errorMessage = "Camera not connected";
    return results;
  }
  if (!gCameraRunning) {
    results.errorMessage = "Camera not started";
    return results;
  }

  // Capture and decode, retrying corrupted frames.
  hwjpeg::Image image;
  const char* decodeError = nullptr;
  bool decoded = false;
  const int maxRetries = 3;
  for (int attempt = 0; attempt < maxRetries && !decoded; attempt++) {
    if (attempt > 0) {
      DEBUG_SYSTEMF("[EI_DEBUG]   Retry %d/%d after capture/decode failure", attempt + 1, maxRetries);
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    size_t jpegLen = 0;
    uint8_t* jpegBuf = captureFrame(&jpegLen);
    if (!jpegBuf || jpegLen == 0) {
      free(jpegBuf);
      continue;
    }
    decoded = decodeEdgeImpulseJpeg(jpegBuf, jpegLen, image, &decodeError);
    free(jpegBuf);
  }
  if (!decoded) {
    results.errorMessage = decodeError ? decodeError : "Failed to convert frame to RGB888";
    return results;
  }

  inferRgb(image.pixels, static_cast<int>(image.width), static_cast<int>(image.height), results);
  if (results.success && gStateTrackingEnabled) {
    updateTrackedObjects(results);
  }
  return results;
}

EIResults runEdgeImpulseInference() {
  EIInferenceLock lock(0);
  if (!lock.held) {
    EIResults busy = {};
    busy.errorMessage = kEIBusyMessage;
    return busy;
  }
  return runCameraInferenceLocked();
}

// ============================================================================
// Inference from Stored Image File
// ============================================================================

EIResults runInferenceFromFile(const char* imagePath, const AuthContext& ctx) {
  EIResults results = {};
  EIInferenceLock lock(0);
  if (!lock.held) {
    results.errorMessage = kEIBusyMessage;
    return results;
  }
  if (const char* reason = inferencePreflight()) {
    results.errorMessage = reason;
    return results;
  }

  DEBUG_SYSTEMF("[EI_DEBUG] Inference from file: %s", imagePath);
  size_t fileSize = 0;
  size_t bytesRead = 0;
  uint8_t* imgBuffer = nullptr;
  {
    FsLockGuard fsGuard("edge.image_classify");
    // The image is read as the caller who named it, never as the system.
    File imgFile = VFS::openGuarded(imagePath, "r", ctx);
    if (!imgFile) {
      results.errorMessage = "Failed to open image file";
      return results;
    }

    fileSize = imgFile.size();
    if (fileSize == 0) {
      imgFile.close();
      results.errorMessage = "Image file is empty";
      return results;
    }

    imgBuffer = (uint8_t*)ps_alloc(fileSize, AllocPref::PreferPSRAM, "ei.img");
    if (!imgBuffer) {
      imgFile.close();
      results.errorMessage = "Failed to allocate image buffer";
      return results;
    }

    bytesRead = imgFile.read(imgBuffer, fileSize);
    imgFile.close();
  }

  if (bytesRead != fileSize) {
    free(imgBuffer);
    results.errorMessage = "Failed to read image file";
    return results;
  }

  hwjpeg::Image image;
  const char* decodeError = nullptr;
  const bool decoded = decodeEdgeImpulseJpeg(imgBuffer, fileSize, image, &decodeError);
  free(imgBuffer);
  if (!decoded) {
    results.errorMessage = decodeError ? decodeError : "Failed to decode image";
    return results;
  }

  inferRgb(image.pixels, static_cast<int>(image.width), static_cast<int>(image.height), results);
  // A stored image is not the live scene: tracked objects (and their
  // detected/lost events) follow the camera only.
  return results;
}

// ============================================================================
// Continuous Inference Task
// ============================================================================

// Runs until a stop clears gEIContinuousRunning; its last action gives the
// "done" semaphore that the stop waits for.
static void continuousInferenceTask(void* param) {
  (void)param;
  DEBUG_SYSTEMF("[EI_DEBUG] ===== Continuous inference task STARTED =====");
  DEBUG_SYSTEMF("[EI_DEBUG]   Interval: %d ms", gSettings.edgeImpulseIntervalMs);
  DEBUG_SYSTEMF("[EI_DEBUG]   Running on core: %d", xPortGetCoreID());
  
  uint32_t inferenceCount = 0;
  uint32_t successCount = 0;
  uint32_t detectionCount = 0;
  uint32_t taskStartTime = millis();
  
  while (gEIContinuousRunning) {
    // Held for one frame, capture to results. A detect command may hold it
    // for one frame too; load and unload stop this task before taking it.
    xSemaphoreTake(eiInferenceMutex(), portMAX_DELAY);
    if (gEIContinuousRunning && gSettings.eiEnabled && gEIModelLoaded) {
      inferenceCount++;
      const EIResults results = runCameraInferenceLocked();

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
    } else if (gEIContinuousRunning) {
      DEBUG_SYSTEMF("[EI_DEBUG] [Continuous] Skipping - enabled=%d modelLoaded=%d",
                    gSettings.eiEnabled, gEIModelLoaded);
    }
    xSemaphoreGive(eiInferenceMutex());

    // Interval sleep; a stop gives "wake" to end it early.
    xSemaphoreTake(eiContinuousWake(), pdMS_TO_TICKS(gSettings.edgeImpulseIntervalMs));
  }

  uint32_t totalTime = millis() - taskStartTime;
  DEBUG_SYSTEMF("[EI_DEBUG] ===== Continuous inference task STOPPED =====");
  DEBUG_SYSTEMF("[EI_DEBUG]   Total: %lu inferences in %lu ms", inferenceCount, totalTime);
  DEBUG_SYSTEMF("[EI_DEBUG]   Success rate: %.1f%% (%lu/%lu)",
                inferenceCount > 0 ? (100.0f * successCount / inferenceCount) : 0,
                successCount, inferenceCount);
  DEBUG_SYSTEMF("[EI_DEBUG]   Total detections: %lu", detectionCount);

  xSemaphoreGive(eiContinuousDone());
  vTaskDelete(nullptr);
}

// Holds: lifecycle lock. Returns true once no continuous task is left: the
// task finished its current frame and exited, so no second task can start
// beside it and the model can be freed or replaced. On a timeout the handle is
// kept, start refuses, and a later stop joins the task again.
static bool joinContinuousTaskLocked() {
  gEIContinuousRunning = false;
  if (!gEIContinuousTask) return true;
  xSemaphoreGive(eiContinuousWake());
  if (xSemaphoreTake(eiContinuousDone(), pdMS_TO_TICKS(kContinuousJoinTimeoutMs)) != pdTRUE) {
    ERROR_SYSTEMF("[EdgeImpulse] Continuous task did not stop within %lu ms",
                  (unsigned long)kContinuousJoinTimeoutMs);
    return false;
  }
  gEIContinuousTask = nullptr;
  DEBUG_SYSTEMF("[EI_DEBUG]   Continuous task joined");
  return true;
}

// Holds: lifecycle lock, with no continuous task left (joined).
static bool spawnContinuousTaskLocked() {
  if (gEIContinuousTask) return false;
  // Drop a wake left over from a stop that raced the task's own exit.
  while (xSemaphoreTake(eiContinuousWake(), 0) == pdTRUE) {
  }
  gEIContinuousRunning = true;

  // No core affinity: prio 1 on Core 0 was getting starved by WiFi (prio ~23)
  // and BT — continuous inference would stall whenever the radios were busy.
  // Letting the scheduler float it gets CPU on whichever core has slack.
  taskStackRecord("ei_continuous", EI_CONTINUOUS_STACK_WORDS);
  BaseType_t result = xTaskCreatePinnedToCore(
    continuousInferenceTask,
    "ei_continuous",
    EI_CONTINUOUS_STACK_WORDS,
    nullptr,
    1,
    &gEIContinuousTask,
    APP_CORE   // inference compute — keep off the Wi-Fi/BLE core
  );
  if (result != pdPASS) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to create continuous task (result=%d)", result);
    gEIContinuousRunning = false;
    gEIContinuousTask = nullptr;
    return false;
  }
  DEBUG_SYSTEMF("[EI_DEBUG]   Continuous inference started, task handle=%p", gEIContinuousTask);
  return true;
}

bool startContinuousInference() {
  DEBUG_SYSTEMF("[EI_DEBUG] startContinuousInference() called");
  EILifecycleLock lifecycle;
  if (gEIContinuousTask) {
    if (gEIContinuousRunning) {
      DEBUG_SYSTEMF("[EI_DEBUG]   Skipping - already running");
      return true;
    }
    // A previous stop timed out; the old task must be gone first.
    if (!joinContinuousTaskLocked()) return false;
  }

  if (!gEIModelLoaded) {
    ERROR_SYSTEMF("[EdgeImpulse] Cannot start continuous - no model loaded");
    return false;
  }
  if (!spawnContinuousTaskLocked()) {
    return false;
  }

  setSetting(gSettings.edgeImpulseContinuous, true);
  {
    const char* slash = strrchr(gLoadedModelPath.c_str(), '/');
    systemEventPost(SYSEVT_EI_CONTINUOUS_STARTED,
                    slash ? slash + 1 : gLoadedModelPath.c_str());
  }
  return true;
}

void stopContinuousInference() {
  DEBUG_SYSTEMF("[EI_DEBUG] stopContinuousInference() called");
  EILifecycleLock lifecycle;
  joinContinuousTaskLocked();
  setSetting(gSettings.edgeImpulseContinuous, false);
}

bool isContinuousInferenceRunning() {
  return gEIContinuousRunning;
}

// ============================================================================
// JSON Output
// ============================================================================

// Detection fields shared by every detection reply (CLI and web).
static void appendDetectionJson(const EIResults& results, JsonDocument& doc) {
  doc["success"] = results.success;
  doc["inferenceTimeMs"] = results.inferenceTimeMs;
  // Reported model geometry. The web overlay reads modelInputWidth and
  // modelInputHeight; modelInputSize (the width) remains for older readers,
  // and the overlay falls back to it when a reply lacks the other two.
  doc["modelInputSize"] = gModelInputWidth;
  doc["modelInputWidth"] = gModelInputWidth;
  doc["modelInputHeight"] = gModelInputHeight;

  if (results.errorMessage) {
    doc["error"] = results.errorMessage;
  }
  
  JsonArray detections = doc["detections"].to<JsonArray>();
  for (int i = 0; i < results.detectionCount; i++) {
    JsonObject det = detections.add<JsonObject>();
    // As const char* so ArduinoJson copies it: a char array would be stored
    // by pointer and read after `results` is gone.
    det["label"] = static_cast<const char*>(results.detections[i].label);
    det["confidence"] = results.detections[i].confidence;
    det["x"] = results.detections[i].x;
    det["y"] = results.detections[i].y;
    det["width"] = results.detections[i].width;
    det["height"] = results.detections[i].height;
  }
}

void buildDetectionJson(const EIResults& results, String& output) {
  PSRAM_JSON_DOC(doc);
  appendDetectionJson(results, doc);
  serializeJson(doc, output);
}

// ============================================================================
// Web API Handler
// ============================================================================

#if ENABLE_HTTP_SERVER

#include "WebServer_Server.h"
#include "WebServer_Utils.h"  // WEB_AUTH_OR_RETURN
#include "System_User.h"

// Tracked-object fields for the web detect reply. Holds: inference lock.
static void appendTrackedObjectsJson(JsonDocument& doc) {
  JsonArray objects = doc["trackedObjects"].to<JsonArray>();
  for (int i = 0; i < gTrackedObjectCount; i++) {
    JsonObject obj = objects.add<JsonObject>();
    // Copied (const char*): the reply is serialized after the lock is released.
    obj["label"] = static_cast<const char*>(gTrackedObjects[i].label);
    obj["prevLabel"] = static_cast<const char*>(gTrackedObjects[i].prevLabel);
    obj["confidence"] = gTrackedObjects[i].confidence;
    obj["x"] = gTrackedObjects[i].x;
    obj["y"] = gTrackedObjects[i].y;
    obj["width"] = gTrackedObjects[i].width;
    obj["height"] = gTrackedObjects[i].height;
    obj["stateChanged"] = gTrackedObjects[i].stateChanged;
    obj["lastSeenMs"] = gTrackedObjects[i].lastSeenMs;
    obj["stateChangeMs"] = gTrackedObjects[i].stateChangeMs;
  }
}

// Organize EI model files: move loose .tflite and .labels.txt into proper /EI Models/<name>/ folders
static esp_err_t handleEIOrganize(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  extern bool filesystemReady;
  if (!filesystemReady) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"filesystem_not_ready\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  File dir = VFS::openGuarded(MODEL_DIR, "r", ctx);
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
    
    char srcModelBuf[96], dstDirBuf[96], dstModelBuf[128];
    snprintf(srcModelBuf, sizeof(srcModelBuf), "%s/%s", MODEL_DIR, tflite.c_str());
    snprintf(dstDirBuf, sizeof(dstDirBuf), "%s/%s", MODEL_DIR, modelName.c_str());
    snprintf(dstModelBuf, sizeof(dstModelBuf), "%s/%s", dstDirBuf, tflite.c_str());
    String srcModel = srcModelBuf;
    String dstDir = dstDirBuf;
    String dstModel = dstModelBuf;
    
    // Create folder
    if (!VFS::existsGuarded(dstDir, ctx)) {
      if (!VFS::mkdirGuarded(dstDir, ctx)) {
        failed++;
        continue;
      }
    }

    // Move .tflite
    if (!VFS::existsGuarded(dstModel, ctx)) {
      if (VFS::renameGuarded(srcModel, dstModel, ctx)) {
        moved++;
      } else {
        failed++;
        continue;
      }
    }

    // Look for matching labels file
    char labelsNameBuf[64], srcLabelsBuf[128], dstLabelsBuf[128];
    snprintf(labelsNameBuf, sizeof(labelsNameBuf), "%s.labels.txt", modelName.c_str());
    snprintf(srcLabelsBuf, sizeof(srcLabelsBuf), "%s/%s", MODEL_DIR, labelsNameBuf);
    snprintf(dstLabelsBuf, sizeof(dstLabelsBuf), "%s/%s", dstDirBuf, labelsNameBuf);
    String srcLabels = srcLabelsBuf;
    String dstLabels = dstLabelsBuf;

    if (VFS::existsGuarded(srcLabels, ctx) && !VFS::existsGuarded(dstLabels, ctx)) {
      if (VFS::renameGuarded(srcLabels, dstLabels, ctx)) {
        moved++;
      }
    }
  }

  httpd_resp_set_type(req, "application/json");
  char jsonBuf[64];
  snprintf(jsonBuf, sizeof(jsonBuf), "{\"success\":true,\"moved\":%d,\"failed\":%d}", moved, failed);
  httpd_resp_send(req, jsonBuf, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t handleEdgeImpulseDetect(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  // Detections plus tracked objects, built from one locked inference; a busy
  // module answers at once with the shared detection fields and its error.
  PSRAM_JSON_DOC(doc);
  {
    EIInferenceLock lock(0);
    if (!lock.held) {
      EIResults busy = {};
      busy.errorMessage = kEIBusyMessage;
      appendDetectionJson(busy, doc);
    } else {
      const EIResults results = runCameraInferenceLocked();
      appendDetectionJson(results, doc);
      appendTrackedObjectsJson(doc);
    }
  }

  String jsonOutput;
  serializeJson(doc, jsonOutput);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_send(req, jsonOutput.c_str(), jsonOutput.length());

  return ESP_OK;
}

#endif  // ENABLE_HTTP_SERVER

// ============================================================================
// CLI Command Handlers (no web server dependency)
// ============================================================================

EXT_RAM_BSS_ATTR static char gEICmdBuffer[512];

const char* cmd_ei(const String& argsInput) {
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

const char* cmd_ei_enable(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = argsInput;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Edge Impulse: %s", 
      gSettings.eiEnabled ? "enabled" : "disabled");
    return gEICmdBuffer;
  }
  
  int val = trimmed.toInt();
  setSetting(gSettings.eiEnabled, (bool)(val != 0));

  if (gSettings.eiEnabled && !gEIInitialized) {
    initEdgeImpulse();
  } else if (!gSettings.eiEnabled) {
    // Stop live inference + unload model so Enabled off frees the subsystem.
    // Both wait for an inference in progress to finish before freeing.
    stopContinuousInference();
    if (gEIModelLoaded) unloadModel();
  }

  sensorStatusBumpWith(gSettings.eiEnabled ? "ei_enable" : "ei_disable");
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Edge Impulse %s", 
    gSettings.eiEnabled ? "enabled" : "disabled");
  return gEICmdBuffer;
}

const char* cmd_ei_detect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  EIResults results = runEdgeImpulseInference();
  
  if (!results.success) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Error: Detection failed: %s",
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

const char* cmd_ei_file(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedPath(a, 0, path);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: ei file \"<path>\"";

  // Check file exists
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Error: File not found: %s", path.c_str());
    return gEICmdBuffer;
  }

  EIResults results = runInferenceFromFile(path.c_str(), currentAuthContext());
  
  if (!results.success) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Error: Inference failed: %s",
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

const char* cmd_ei_continuous(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = argsInput;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Continuous mode: %s", 
      gEIContinuousRunning ? "running" : "stopped");
    return gEICmdBuffer;
  }
  
  int val = trimmed.toInt();
  if (val) {
    if (startContinuousInference()) {
      return "Continuous inference started";
    }
    return "Error: cannot start continuous inference (load a model first with eimodelload)";
  } else {
    stopContinuousInference();
    return "Continuous inference stopped";
  }
}

const char* cmd_ei_confidence(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = argsInput;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Min confidence: %.2f", 
      gSettings.edgeImpulseMinConfidence);
    return gEICmdBuffer;
  }
  
  float val = trimmed.toFloat();
  if (val < 0.0f) val = 0.0f;
  if (val > 1.0f) val = 1.0f;
  
  setSetting(gSettings.edgeImpulseMinConfidence, val);
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Min confidence set to %.2f", val);
  return gEICmdBuffer;
}

const char* cmd_ei_status(const String& argsInput) {
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
    gSettings.eiEnabled ? "yes" : "no",
    gEIContinuousRunning ? "running" : "stopped",
    gSettings.edgeImpulseMinConfidence,
    gModelInputWidth, gModelInputHeight,
    gSettings.edgeImpulseIntervalMs
  );
  return gEICmdBuffer;
}

// Model management commands
const char* cmd_ei_model(const String& argsInput) {
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

const char* cmd_ei_model_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String output;
  listAvailableModels(output);
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "%s", output.c_str());
  return gEICmdBuffer;
}

const char* cmd_ei_model_load(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CommandArgs a(argsInput);
  String trimmed;
  const char* qerr = requireQuotedToken(a, 0, trimmed);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: ei model load \"<filename>\"";

  // Build full path if just filename given
  String path = trimmed;
  if (!path.startsWith("/")) {
    char pathBuf[96];
    snprintf(pathBuf, sizeof(pathBuf), "%s/%s", MODEL_DIR, trimmed.c_str());
    path = pathBuf;
  }
  
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Error: Model not found: %s", path.c_str());
    return gEICmdBuffer;
  }
  
  // Try to load model first - only mark the module ready if model is valid
  if (loadModelFromFile(path.c_str())) {
    // The load allocated the model's buffers; readiness only needs the model
    // directory (initEdgeImpulse would also load default.tflite over it).
    markEdgeImpulseReady();
    EIInferenceLock inference(portMAX_DELAY);
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer),
      "Model loaded: %s\nInput: %dx%dx%d\nArena: %zu bytes",
      path.c_str(), gModelInputWidth, gModelInputHeight, gModelInputChannels,
      gInterpreter ? gInterpreter->arena_used_bytes() : 0);
  } else {
    snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Error: Failed to load model: %s", path.c_str());
  }
  return gEICmdBuffer;
}

const char* cmd_ei_model_info(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  EIInferenceLock inference(portMAX_DELAY);  // the model cannot be freed while read
  if (!gEIModelLoaded || !gInterpreter) {
    return "Error: No model loaded. Use 'ei model load <filename>' to load one.";
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

const char* cmd_ei_model_unload(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gEIModelLoaded) {
    return "Error: No model is currently loaded.";
  }
  
  String oldPath = gLoadedModelPath;
  unloadModel();
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Model unloaded: %s", oldPath.c_str());
  return gEICmdBuffer;
}

// State tracking commands
const char* cmd_ei_track(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer),
    "State Tracking Commands:\n"
    "  ei track status     - Show tracked objects\n"
    "  ei track enable <0|1> - Enable/disable tracking\n"
    "  ei track clear      - Clear tracked objects"
  );
  return gEICmdBuffer;
}

const char* cmd_ei_track_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  EIInferenceLock inference(portMAX_DELAY);  // tracking table is updated per frame
  if (!gStateTrackingEnabled) {
    return "Error: State tracking is disabled. Use 'ei track enable 1' to enable.";
  }

  if (gTrackedObjectCount == 0) {
    return "No objects currently tracked. Run continuous inference to track objects.";
  }
  
  char hdrBuf[32];
  snprintf(hdrBuf, sizeof(hdrBuf), "Tracked Objects (%d):\n", gTrackedObjectCount);
  String output = hdrBuf;
  uint32_t now = millis();
  
  for (int i = 0; i < gTrackedObjectCount; i++) {
    const TrackedObject& obj = gTrackedObjects[i];
    char lineBuf[128];
    int pos = snprintf(lineBuf, sizeof(lineBuf), "  [%d] %s at (%d,%d) conf=%.2f",
                       i, obj.label, obj.x, obj.y, obj.confidence);
    if (obj.prevLabel[0]) {
      pos += snprintf(lineBuf + pos, sizeof(lineBuf) - pos, " prev=%s", obj.prevLabel);
    }
    pos += snprintf(lineBuf + pos, sizeof(lineBuf) - pos, " age=%.1fs", (now - obj.lastSeenMs) / 1000.0f);
    if (obj.stateChanged) {
      pos += snprintf(lineBuf + pos, sizeof(lineBuf) - pos, " [CHANGED]");
    }
    output += lineBuf;
    output += "\n";
  }
  
  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "%s", output.c_str());
  return gEICmdBuffer;
}

const char* cmd_ei_track_enable(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = argsInput;
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

const char* cmd_ei_track_clear(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  int count = 0;
  {
    EIInferenceLock inference(portMAX_DELAY);
    count = gTrackedObjectCount;
    gTrackedObjectCount = 0;
  }

  snprintf(gEICmdBuffer, sizeof(gEICmdBuffer), "Cleared %d tracked objects", count);
  return gEICmdBuffer;
}

// ---------------------------------------------------------------------------
// The CLI handlers above and the command table below are gated on
// ENABLE_EDGE_IMPULSE alone: gCommandModules[] in System_CommandRegistry.cpp
// references edgeImpulseCommands under `#if ENABLE_EDGE_IMPULSE` only, so an
// ENABLE_EDGE_IMPULSE=1, ENABLE_HTTP_SERVER=0 build needs every handler here.
// ---------------------------------------------------------------------------

// ============================================================================
// Command Registration
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry edgeImpulseCommands[] = {
  { "ei", "Edge Impulse ML inference commands.", false, cmd_ei, "Usage: ei <subcommand>" },
  { "eienable", "Enable/disable Edge Impulse inference.", false, cmd_ei_enable, "Usage: eienable <0|1>" },
  { "eidetect", "Run single object detection inference.", false, cmd_ei_detect, "Usage: eidetect" },
  { "eifile", "Run inference on stored JPEG image.", false, cmd_ei_file, "Usage: eifile \"<path>\"" },
  { "eicontinuous", "Start/stop continuous inference mode.", false, cmd_ei_continuous, "Usage: eicontinuous <0|1>" },
  { "eiconfidence", "Set minimum detection confidence.", false, cmd_ei_confidence, "Usage: eiconfidence <0.0-1.0>" },
  { "eistatus", "Show Edge Impulse status.", false, cmd_ei_status, "Usage: eistatus" },
  { "eimodel", "Model management commands.", false, cmd_ei_model, "Usage: eimodel <subcommand>" },
  { "eimodellist", "List available .tflite models.", false, cmd_ei_model_list, "Usage: eimodellist" },
  { "eimodelload", "Load a TFLite model from LittleFS.", false, cmd_ei_model_load, "Usage: eimodelload \"<filename>\"" },
  { "eimodelinfo", "Show loaded model information.", false, cmd_ei_model_info, "Usage: eimodelinfo" },
  { "eimodelunload", "Unload the current model.", false, cmd_ei_model_unload, "Usage: eimodelunload" },
  { "eitrack", "State tracking commands.", false, cmd_ei_track, "Usage: eitrack <subcommand>" },
  { "eitrackstatus", "Show currently tracked objects.", false, cmd_ei_track_status, "Usage: eitrackstatus" },
  { "eitrackenable", "Enable/disable state tracking.", false, cmd_ei_track_enable, "Usage: eitrackenable <0|1>" },
  { "eitrackclear", "Clear all tracked objects.", false, cmd_ei_track_clear, "Usage: eitrackclear" }
};

const size_t edgeImpulseCommandsCount = sizeof(edgeImpulseCommands) / sizeof(edgeImpulseCommands[0]);

// Auto-register commands at startup
// Registration handled by gCommandModules[] in System_Utils.cpp

#if ENABLE_HTTP_SERVER
// =============================================================================
// Register Edge Impulse Web Handlers
// =============================================================================

void registerEdgeImpulseHandlers(httpd_handle_t server) {
  static const httpd_uri_t eiOrganizePost = { .uri = "/api/ei/organize", .method = HTTP_POST, .handler = handleEIOrganize, .user_ctx = NULL };
  static const httpd_uri_t edgeImpulseDetect = { .uri = "/api/edgeimpulse/detect", .method = HTTP_GET, .handler = handleEdgeImpulseDetect, .user_ctx = NULL };
  httpd_register_uri_handler(server, &eiOrganizePost);
  httpd_register_uri_handler(server, &edgeImpulseDetect);
}

#endif // ENABLE_HTTP_SERVER

#endif // ENABLE_EDGE_IMPULSE
