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
#include "System_PsramJson.h"  // PsramJsonText: the web detect reply
#include "System_TaskUtils.h"
#include "HAL_JPEG.h"
#include <ArduinoJson.h>
#include <FS.h>
#include "System_VFS.h"
#include "System_AuthIdentity.h"
#include "System_Mutex.h"
#include "System_I2C.h"        // sensorStatusBumpWith
#include <atomic>

// TensorFlow Lite Micro includes
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "System_CommandRegistry.h"

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

static const char* const kEiBusyMessage = "Edge Impulse busy";

// Model storage path (relative to LittleFS mount point)
static const char* kModelDir = "/EI Models";
static const char* kDefaultModel = "/EI Models/default.tflite";

// ============================================================================
// Edge Impulse Module State
// ============================================================================

static bool sEiInitialized = false;
static bool sEiModelLoaded = false;
static std::atomic<bool> sEiContinuousRunning{false};
static TaskHandle_t sEiContinuousTask = nullptr;  // owned under the lifecycle lock

// Model-input resize buffer: RGB888 at the model's input width x height,
// allocated by the model load and freed with the model.
static uint8_t* sResizedBuffer = nullptr;
static size_t sResizedBufferSize = 0;

// TFLite Micro state
static uint8_t* sModelBuffer = nullptr;           // Loaded .tflite model data
static size_t sModelSize = 0;                     // Size of loaded model
static uint8_t* sTensorArena = nullptr;           // TFLite tensor arena
static const tflite::Model* sTflModel = nullptr;  // Parsed model
static tflite::MicroInterpreter* sInterpreter = nullptr;
static TfLiteTensor* sInputTensor = nullptr;
static TfLiteTensor* sOutputTensor = nullptr;
static String sLoadedModelPath;                   // Path of currently loaded model
// Input geometry, read from the loaded model's input tensor (never a setting).
static int sModelInputWidth = 0;
static int sModelInputHeight = 0;
static int sModelInputChannels = 0;
// Class count: last dimension of the loaded model's output tensor.
static int sModelClassCount = 0;

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

struct EiInferenceLock {
  bool held;
  explicit EiInferenceLock(TickType_t wait)
      : held(xSemaphoreTake(eiInferenceMutex(), wait) == pdTRUE) {}
  ~EiInferenceLock() {
    if (held) xSemaphoreGive(eiInferenceMutex());
  }
  EiInferenceLock(const EiInferenceLock&) = delete;
  EiInferenceLock& operator=(const EiInferenceLock&) = delete;
};

// Lifecycle lock: serializes start, stop, load and unload (and so the
// continuous task handle). Always taken before the inference lock.
static SemaphoreHandle_t eiLifecycleMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}

struct EiLifecycleLock {
  EiLifecycleLock() { xSemaphoreTake(eiLifecycleMutex(), portMAX_DELAY); }
  ~EiLifecycleLock() { xSemaphoreGive(eiLifecycleMutex()); }
  EiLifecycleLock(const EiLifecycleLock&) = delete;
  EiLifecycleLock& operator=(const EiLifecycleLock&) = delete;
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
static char* sModelLabels = nullptr;  // sModelLabelSlots * EI_LABEL_MAX bytes
static int sModelLabelSlots = 0;
static int sModelLabelCount = 0;      // slots filled from the labels file

// Free label memory
static void freeLabels() {
  free(sModelLabels);
  sModelLabels = nullptr;
  sModelLabelSlots = 0;
  sModelLabelCount = 0;
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
  sModelLabels = static_cast<char*>(ps_alloc(tableBytes, AllocPref::PreferPsram, "ei.labels"));
  if (!sModelLabels) {
    labelsFile.close();
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate %d label slots", classCount);
    return false;
  }
  memset(sModelLabels, 0, tableBytes);
  sModelLabelSlots = classCount;

  // Lines past the model's class count name no class and are ignored.
  while (labelsFile.available() && sModelLabelCount < sModelLabelSlots) {
    String line = labelsFile.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      char* slot = sModelLabels + static_cast<size_t>(sModelLabelCount) * EI_LABEL_MAX;
      strlcpy(slot, line.c_str(), EI_LABEL_MAX);
      DEBUG_SYSTEMF("[EI_DEBUG] Label[%d] = '%s'", sModelLabelCount, slot);
      sModelLabelCount++;
    }
  }
  labelsFile.close();
  return sModelLabelCount > 0;
}

// Load labels file for model, one slot per model class.
// Expected structure: /EI Models/<modelname>/<modelname>.tflite + <modelname>.labels.txt
static bool loadLabelsFromFile(const char* modelPath, int classCount) {
  freeLabels();
  
  String model = String(modelPath);
  int lastSlash = model.lastIndexOf('/');
  String modelDir = (lastSlash >= 0) ? model.substring(0, lastSlash) : String(kModelDir);
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
                  sModelLabelCount, classCount, labelsPath.c_str());
    return true;
  }

  ERROR_SYSTEMF("[EI] Labels file not found: %s", labelsPath.c_str());
  return false;
}

// Copy the label for an output class into a detection's own storage. Classes
// the labels file does not name get "class_<index>".
static void copyLabelForIndex(int index, char* out, size_t outSize) {
  if (sModelLabels && index >= 0 && index < sModelLabelCount) {
    snprintf(out, outSize, "%s", sModelLabels + static_cast<size_t>(index) * EI_LABEL_MAX);
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
static TrackedObject sTrackedObjects[MAX_TRACKED_OBJECTS];
static int sTrackedObjectCount = 0;
static bool sStateTrackingEnabled = true;

// Find tracked object by approximate location (within tolerance)
static TrackedObject* findTrackedObject(int x, int y, int tolerance = 20) {
  for (int i = 0; i < sTrackedObjectCount; i++) {
    int dx = abs(sTrackedObjects[i].x - x);
    int dy = abs(sTrackedObjects[i].y - y);
    if (dx <= tolerance && dy <= tolerance) {
      return &sTrackedObjects[i];
    }
  }
  return nullptr;
}

// Update tracked objects with new detections. Holds: inference lock.
static void updateTrackedObjects(const EiResults& results) {
  uint32_t now = millis();
  
  // Mark all as potentially stale
  for (int i = 0; i < sTrackedObjectCount; i++) {
    sTrackedObjects[i].stateChanged = false;
  }
  
  // Process new detections
  for (int d = 0; d < results.detectionCount; d++) {
    const EiDetection& det = results.detections[d];
    
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
    } else if (sTrackedObjectCount < MAX_TRACKED_OBJECTS) {
      // New object - add to tracking
      TrackedObject* newObj = &sTrackedObjects[sTrackedObjectCount++];
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
  for (int i = sTrackedObjectCount - 1; i >= 0; i--) {
    if (now - sTrackedObjects[i].lastSeenMs > OBJECT_TIMEOUT_MS) {
      DEBUG_SYSTEMF("[EdgeImpulse] Object lost: %s at (%d,%d)",
                    sTrackedObjects[i].label, sTrackedObjects[i].x, sTrackedObjects[i].y);
      // Only objects that were announced get a LOST event — un-announced
      // flickers would otherwise post orphan losses.
      if (sTrackedObjects[i].announced) {
        systemEventPost(SYSEVT_EI_LOST, sTrackedObjects[i].label);
      }
      // Shift remaining objects down
      for (int j = i; j < sTrackedObjectCount - 1; j++) {
        sTrackedObjects[j] = sTrackedObjects[j + 1];
      }
      sTrackedObjectCount--;
    }
  }
}

// Public: Enable/disable state tracking
void setStateTrackingEnabled(bool enabled) {
  EiInferenceLock lock(portMAX_DELAY);
  sStateTrackingEnabled = enabled;
  if (!enabled) {
    sTrackedObjectCount = 0;
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
static const SettingEntry kEdgeImpulseSettingEntries[] = {
  { "eiAutoStart", SETTING_BOOL, &gSettings.eiAutoStart, 0, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, nullptr, "eiautostart" },
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
  kEdgeImpulseSettingEntries,
  sizeof(kEdgeImpulseSettingEntries) / sizeof(kEdgeImpulseSettingEntries[0]),
  nullptr,
  "Edge Impulse on-device object detection"
};

// ============================================================================
// TFLite Micro Model Loading
// ============================================================================

// Op resolver with common ops for image classification/detection models
// Increased to 20 ops to support more Edge Impulse model types
static tflite::MicroMutableOpResolver<20>* sOpResolver = nullptr;

static bool setupOpResolver() {
  if (sOpResolver) return true;
  
  DEBUG_SYSTEMF("[EI_DEBUG] Setting up op resolver with 20 ops capacity...");
  
  sOpResolver = new tflite::MicroMutableOpResolver<20>();
  if (!sOpResolver) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate op resolver");
    return false;
  }
  
  // Add common ops used by FOMO and image classification models
  sOpResolver->AddConv2D();
  sOpResolver->AddDepthwiseConv2D();
  sOpResolver->AddFullyConnected();
  sOpResolver->AddReshape();
  sOpResolver->AddSoftmax();
  sOpResolver->AddMaxPool2D();
  sOpResolver->AddAveragePool2D();
  sOpResolver->AddQuantize();
  sOpResolver->AddDequantize();
  sOpResolver->AddMean();
  
  // Additional ops commonly used by Edge Impulse models
  sOpResolver->AddPad();           // Required for many EI models
  sOpResolver->AddPadV2();         // Variant of Pad
  sOpResolver->AddAdd();           // Element-wise add
  sOpResolver->AddMul();           // Element-wise multiply
  sOpResolver->AddRelu();          // Activation function
  sOpResolver->AddRelu6();         // Activation function
  sOpResolver->AddLogistic();      // Sigmoid activation
  sOpResolver->AddConcatenation(); // Tensor concatenation
  sOpResolver->AddSplit();         // Tensor split
  sOpResolver->AddSplitV();        // Tensor split variant
  
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
  sModelInputWidth = 0;
  sModelInputHeight = 0;
  sModelInputChannels = 0;
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
  sModelInputWidth = width;
  sModelInputHeight = height;
  sModelInputChannels = channels;
  return true;
}

// Output tensor: [1, gridH, gridW, classes] (FOMO) or [..., classes]
// (classification) of float32/uint8/int8, whose element count matches its
// bytes so the parser never reads past it.
static bool configureOutputShape(const TfLiteTensor* output) {
  sModelClassCount = 0;
  if (!output || !output->dims || output->dims->size < 1) return false;
  const size_t elementBytes = tensorElementBytes(output->type);
  if (!elementBytes) return false;
  size_t elements = 1;
  for (int i = 0; i < output->dims->size; i++) {
    if (output->dims->data[i] <= 0) return false;
    elements *= static_cast<size_t>(output->dims->data[i]);
  }
  if (elements * elementBytes != output->bytes) return false;
  sModelClassCount = output->dims->data[output->dims->size - 1];
  return true;
}

// Holds: inference lock (or runs before any inference can see the model).
static void freeModelResources() {
  DEBUG_SYSTEMF("[EI_DEBUG] freeModelResources() called");
  DEBUG_SYSTEMF("[EI_DEBUG]   Current state: interpreter=%p model=%p arena=%p", 
                sInterpreter, sModelBuffer, sTensorArena);
  
  if (sInterpreter) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Deleting interpreter...");
    delete sInterpreter;
    sInterpreter = nullptr;
  }
  sTflModel = nullptr;  // Points into sModelBuffer, don't delete
  sInputTensor = nullptr;
  sOutputTensor = nullptr;
  
  if (sModelBuffer) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Freeing model buffer (%zu bytes)...", sModelSize);
    free(sModelBuffer);
    sModelBuffer = nullptr;
  }
  sModelSize = 0;
  
  if (sTensorArena) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Freeing tensor arena (%zu bytes)...", kTensorArenaSize);
    free(sTensorArena);
    sTensorArena = nullptr;
  }

  free(sResizedBuffer);
  sResizedBuffer = nullptr;
  sResizedBufferSize = 0;

  sLoadedModelPath = "";
  sModelInputWidth = 0;
  sModelInputHeight = 0;
  sModelInputChannels = 0;
  sModelClassCount = 0;
  sEiModelLoaded = false;

  // Free label memory
  freeLabels();

  DEBUG_SYSTEMF("[EI_DEBUG]   Heap after cleanup: %lu free", (unsigned long)ESP.getFreeHeap());
}

// Why the last model load failed, for the eimodelload reply: the reason and
// the next action. Written under the lifecycle lock.
static const char* sEiLoadFailure = nullptr;
static bool loadRefused(const char* why) {
  sEiLoadFailure = why;
  return false;
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
      return loadRefused("the file cannot be opened. Check the name with eimodellist.");
    }

    fileSize = modelFile.size();
    DEBUG_SYSTEMF("[EI_DEBUG] File size: %zu bytes (max allowed: %zu)", fileSize, kMaxModelSize);

    if (fileSize == 0) {
      ERROR_SYSTEMF("[EdgeImpulse] Model file is empty");
      modelFile.close();
      return loadRefused("the file is empty. Upload the model again.");
    }
    if (fileSize > kMaxModelSize) {
      ERROR_SYSTEMF("[EdgeImpulse] Model too large: %zu bytes (max %zu)", fileSize, kMaxModelSize);
      modelFile.close();
      return loadRefused("the file is too large for this device. Use a smaller model.");
    }

    DEBUG_SYSTEMF("[EI_DEBUG] Allocating model buffer: %zu bytes", fileSize);
    sModelBuffer = (uint8_t*)ps_alloc(fileSize, AllocPref::PreferPsram, "ei.model");
    if (!sModelBuffer) {
      ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate model buffer (%zu bytes)", fileSize);
      DEBUG_SYSTEMF("[EI_DEBUG] Allocation FAILED! Heap: %lu, PSRAM: %lu",
                    (unsigned long)ESP.getFreeHeap(),
                    (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));
      modelFile.close();
      return loadRefused("not enough memory. Free memory and retry.");
    }
    DEBUG_SYSTEMF("[EI_DEBUG] Model buffer allocated at %p", sModelBuffer);

    DEBUG_SYSTEMF("[EI_DEBUG] Reading model data from file...");
    uint32_t readStart = millis();
    bytesRead = modelFile.read(sModelBuffer, fileSize);
    readTime = millis() - readStart;
    modelFile.close();
  }
  
  DEBUG_SYSTEMF("[EI_DEBUG] Read %zu bytes in %lu ms (%.1f KB/s)",
                bytesRead, readTime, 
                readTime > 0 ? (bytesRead / 1024.0f) / (readTime / 1000.0f) : 0);
  
  if (bytesRead != fileSize) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to read model: got %zu of %zu bytes", bytesRead, fileSize);
    free(sModelBuffer);
    sModelBuffer = nullptr;
    return loadRefused("the file could not be read completely. Upload the model again.");
  }
  sModelSize = fileSize;
  
  // Parse the model
  DEBUG_SYSTEMF("[EI_DEBUG] Parsing TFLite model...");
  sTflModel = tflite::GetModel(sModelBuffer);
  if (!sTflModel) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to parse TFLite model");
    DEBUG_SYSTEMF("[EI_DEBUG] First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                  sModelBuffer[0], sModelBuffer[1], sModelBuffer[2], sModelBuffer[3],
                  sModelBuffer[4], sModelBuffer[5], sModelBuffer[6], sModelBuffer[7]);
    free(sModelBuffer);
    sModelBuffer = nullptr;
    return loadRefused("the file is not a TFLite model. Upload a .tflite export.");
  }
  
  // Check model version
  DEBUG_SYSTEMF("[EI_DEBUG] Model version: %lu (expected: %d)", 
                sTflModel->version(), TFLITE_SCHEMA_VERSION);
  
  if (sTflModel->version() != TFLITE_SCHEMA_VERSION) {
    ERROR_SYSTEMF("[EdgeImpulse] Model schema version mismatch: got %lu, expected %d",
                  sTflModel->version(), TFLITE_SCHEMA_VERSION);
    free(sModelBuffer);
    sModelBuffer = nullptr;
    sTflModel = nullptr;
    return loadRefused("the TFLite schema version is not supported. Export the model again.");
  }
  
  // Setup op resolver
  DEBUG_SYSTEMF("[EI_DEBUG] Setting up op resolver...");
  if (!setupOpResolver()) {
    free(sModelBuffer);
    sModelBuffer = nullptr;
    sTflModel = nullptr;
    return loadRefused("not enough memory. Free memory and retry.");
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Op resolver ready");
  
  // Allocate tensor arena in PSRAM
  DEBUG_SYSTEMF("[EI_DEBUG] Allocating tensor arena: %zu KB", kTensorArenaSize / 1024);
  sTensorArena = (uint8_t*)ps_alloc(kTensorArenaSize, AllocPref::PreferPsram, "ei.arena");
  
  if (!sTensorArena) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate tensor arena (%zu bytes)", kTensorArenaSize);
    DEBUG_SYSTEMF("[EI_DEBUG] Arena allocation FAILED! PSRAM free: %lu",
                  (unsigned long)(psramFound() ? ESP.getFreePsram() : ESP.getFreeHeap()));
    free(sModelBuffer);
    sModelBuffer = nullptr;
    sTflModel = nullptr;
    return loadRefused("not enough memory. Free memory and retry.");
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Tensor arena allocated at %p", sTensorArena);
  
  // Create interpreter
  DEBUG_SYSTEMF("[EI_DEBUG] Creating MicroInterpreter...");
  sInterpreter = new tflite::MicroInterpreter(
    sTflModel, *sOpResolver, sTensorArena, kTensorArenaSize
  );
  
  if (!sInterpreter) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to create interpreter");
    freeModelResources();
    return loadRefused("not enough memory. Free memory and retry.");
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Interpreter created at %p", sInterpreter);
  
  // Allocate tensors
  DEBUG_SYSTEMF("[EI_DEBUG] Allocating tensors...");
  uint32_t allocStart = millis();
  TfLiteStatus allocStatus = sInterpreter->AllocateTensors();
  uint32_t allocTime = millis() - allocStart;
  
  if (allocStatus != kTfLiteOk) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate tensors (status=%d)", allocStatus);
    freeModelResources();
    return loadRefused("the model does not fit the tensor arena or uses an unsupported operation. Use a smaller model.");
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Tensors allocated in %lu ms", allocTime);
  DEBUG_SYSTEMF("[EI_DEBUG] Arena used: %zu / %zu bytes (%.1f%%)",
                sInterpreter->arena_used_bytes(), kTensorArenaSize,
                100.0f * sInterpreter->arena_used_bytes() / kTensorArenaSize);
  
  // Get input tensor info
  DEBUG_SYSTEMF("[EI_DEBUG] Getting input/output tensors...");
  DEBUG_SYSTEMF("[EI_DEBUG] Model has %zu inputs, %zu outputs",
                sInterpreter->inputs_size(), sInterpreter->outputs_size());
  
  sInputTensor = sInterpreter->input(0);
  if (!sInputTensor) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to get input tensor");
    freeModelResources();
    return loadRefused("unsupported input tensor. The model needs an HxWx1 or HxWx3 image input.");
  }
  
  // Log input tensor details
  DEBUG_SYSTEMF("[EI_DEBUG] Input tensor: dims=%d type=%d bytes=%zu",
                sInputTensor->dims->size, sInputTensor->type, sInputTensor->bytes);
  for (int i = 0; i < sInputTensor->dims->size; i++) {
    DEBUG_SYSTEMF("[EI_DEBUG]   dim[%d] = %d", i, sInputTensor->dims->data[i]);
  }
  
  // Input geometry (NHWC [batch, height, width, channels] or HWC) comes from
  // the tensor alone and must account for exactly its bytes.
  if (!configureInputGeometry(sInputTensor)) {
    ERROR_SYSTEMF("[EdgeImpulse] Unsupported input tensor (type=%d, bytes=%zu): need HxWx1 or HxWx3 "
                  "float32/uint8/int8, sides up to %d",
                  sInputTensor->type, sInputTensor->bytes, kMaxModelInputSide);
    freeModelResources();
    return loadRefused("unsupported input tensor. The model needs an HxWx1 or HxWx3 image input.");
  }
  DEBUG_SYSTEMF("[EI_DEBUG] Input geometry: %dx%dx%d",
                sModelInputWidth, sModelInputHeight, sModelInputChannels);

  // Get output tensor
  sOutputTensor = sInterpreter->output(0);
  if (sOutputTensor) {
    DEBUG_SYSTEMF("[EI_DEBUG] Output tensor: dims=%d type=%d bytes=%zu",
                  sOutputTensor->dims->size, sOutputTensor->type, sOutputTensor->bytes);
    for (int i = 0; i < sOutputTensor->dims->size; i++) {
      DEBUG_SYSTEMF("[EI_DEBUG]   dim[%d] = %d", i, sOutputTensor->dims->data[i]);
    }
    if (sOutputTensor->type == kTfLiteUInt8 || sOutputTensor->type == kTfLiteInt8) {
      DEBUG_SYSTEMF("[EI_DEBUG] Output quantization: scale=%.6f zero_point=%d",
                    sOutputTensor->params.scale, sOutputTensor->params.zero_point);
    }
  }
  if (!configureOutputShape(sOutputTensor)) {
    ERROR_SYSTEMF("[EdgeImpulse] Unsupported output tensor: need float32/uint8/int8 class scores");
    freeModelResources();
    return loadRefused("unsupported output tensor. The model needs float32, uint8 or int8 class scores.");
  }

  // The resize buffer holds one RGB888 frame at the model's input geometry.
  sResizedBufferSize = static_cast<size_t>(sModelInputWidth) * sModelInputHeight * 3;
  sResizedBuffer = static_cast<uint8_t*>(ps_alloc(sResizedBufferSize, AllocPref::PreferPsram, "ei.resized"));
  if (!sResizedBuffer) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to allocate %zu-byte resize buffer", sResizedBufferSize);
    freeModelResources();
    return loadRefused("not enough memory. Free memory and retry.");
  }

  sLoadedModelPath = path;
  sEiModelLoaded = true;

  // Try to load labels from labels.txt, one slot per output class
  bool haveLabels = loadLabelsFromFile(path, sModelClassCount);
  if (gSettings.edgeImpulseRequireLabels && !haveLabels) {
    ERROR_SYSTEMF("[EdgeImpulse] Labels are required but no labels file was found for: %s", path);
    freeModelResources();
    return loadRefused("the labels file <model>.labels.txt is missing. Upload it next to the model or run eirequirelabels 0.");
  }
  
  DEBUG_SYSTEMF("[EI_DEBUG] ========== loadModelFromFile() SUCCESS ==========");
  DEBUG_SYSTEMF("[EI_DEBUG] Model: %s", path);
  DEBUG_SYSTEMF("[EI_DEBUG] Size: %zu bytes", sModelSize);
  DEBUG_SYSTEMF("[EI_DEBUG] Input: %dx%dx%d, type=%d", 
                sModelInputWidth, sModelInputHeight, sModelInputChannels,
                sInputTensor->type);
  DEBUG_SYSTEMF("[EI_DEBUG] Arena: %zu / %zu bytes (%.1f%% used)",
                sInterpreter->arena_used_bytes(), kTensorArenaSize,
                100.0f * sInterpreter->arena_used_bytes() / kTensorArenaSize);
  DEBUG_SYSTEMF("[EI_DEBUG] Heap after load: %lu free, PSRAM: %lu free",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(psramFound() ? ESP.getFreePsram() : 0));

  return true;
}

static bool joinContinuousTaskLocked();
static bool spawnContinuousTaskLocked();

bool loadModelFromFile(const char* path) {
  EiLifecycleLock lifecycle;
  // Continuous mode pauses for the swap and resumes on the new model.
  const bool resume = sEiContinuousTask != nullptr && sEiContinuousRunning;
  if (!joinContinuousTaskLocked()) {
    ERROR_SYSTEMF("[EdgeImpulse] Model not loaded: continuous inference did not stop");
    return loadRefused("continuous inference did not stop. Retry in a few seconds.");
  }
  bool loaded = false;
  {
    EiInferenceLock inference(portMAX_DELAY);
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
  EiLifecycleLock lifecycle;
  DEBUG_SYSTEMF("[EI_DEBUG]   Current model: %s",
                sLoadedModelPath.length() > 0 ? sLoadedModelPath.c_str() : "(none)");

  // Stop continuous inference and wait for its task to exit
  if (sEiContinuousTask) {
    DEBUG_SYSTEMF("[EI_DEBUG]   Stopping continuous inference first...");
    joinContinuousTaskLocked();
    setSetting(gSettings.edgeImpulseContinuous, false);
  }

  // Waits for any inference still holding the model.
  EiInferenceLock inference(portMAX_DELAY);
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
        snprintf(fullPathBuf, sizeof(fullPathBuf), "%s/%s", kModelDir, rel.c_str());
        char lineBuf[128];
        snprintf(lineBuf, sizeof(lineBuf), "  %s (%d bytes)", rel.c_str(), (int)file.size());
        output += lineBuf;
        if (sLoadedModelPath == fullPathBuf) {
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
  snprintf(hdrBuf, sizeof(hdrBuf), "Available models in %s:\n", kModelDir);
  output = hdrBuf;
  
  File root = VFS::openGuarded(kModelDir, "r", VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.list_models"));
  if (!root || !root.isDirectory()) {
    char errBuf[64];
    snprintf(errBuf, sizeof(errBuf), "  (directory not found - create %s)\n", kModelDir);
    output += errBuf;
    return;
  }

  int count = 0;
  listModelsRecursive(String(kModelDir), "", output, count);

  if (count == 0) {
    output += "  (no .tflite files found)\n";
  }
}

// Create models directory if it doesn't exist
static void ensureModelDirectory() {
  if (!VFS::existsGuarded(kModelDir, VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.ensure_model_dir"))) {
    VFS::mkdirGuarded(kModelDir, VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.ensure_model_dir"));
    DEBUG_SYSTEMF("[EdgeImpulse] Created models directory: %s", kModelDir);
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
  if (sEiInitialized) return;
  DEBUG_SYSTEMF("[EI_DEBUG] Ensuring model directory exists...");
  ensureModelDirectory();
  sEiInitialized = true;
  DEBUG_SYSTEMF("[EI_DEBUG] Edge Impulse initialized successfully");
}

void initEdgeImpulse() {
  DEBUG_SYSTEMF("[EI_DEBUG] ========== initEdgeImpulse() START ==========");
  DEBUG_SYSTEMF("[EI_DEBUG] Already initialized: %s", sEiInitialized ? "YES" : "NO");

  if (sEiInitialized) {
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
  DEBUG_SYSTEMF("[EI_DEBUG] Checking for default model: %s", kDefaultModel);
  if (VFS::existsGuarded(kDefaultModel, VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.default_model_check"))) {
    DEBUG_SYSTEMF("[EI_DEBUG] Default model found, loading...");
    if (loadModelFromFile(kDefaultModel)) {
      DEBUG_SYSTEMF("[EI_DEBUG] Default model loaded successfully");
    } else {
      ERROR_SYSTEMF("[EdgeImpulse] Failed to load default model");
    }
  } else {
    DEBUG_SYSTEMF("[EI_DEBUG] No default model at %s", kDefaultModel);

    // List available models
    File dir = VFS::openGuarded(kModelDir, "r", VFS::systemAuth(VFS::Scopes::EI_MODELS, "edge.list_models_init"));
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
  return sEiModelLoaded;
}

// ============================================================================
// Inference Implementation
// ============================================================================

// Decoded pixels go to PSRAM; hwjpeg::Image releases them with free().
static uint8_t* allocateEdgeImpulseRgb(size_t bytes) {
  return static_cast<uint8_t*>(ps_alloc(bytes, AllocPref::PreferPsram, "ei.rgb"));
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

static void addDetection(EiResults& results, int classIndex, float confidence,
                         int x, int y, int width, int height) {
  EiDetection& det = results.detections[results.detectionCount++];
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
static float parseOutputTensor(EiResults& results) {
  results.detectionCount = 0;
  int maxDetections = gSettings.edgeImpulseMaxDetections;
  if (maxDetections < 1) maxDetections = 1;
  if (maxDetections > EI_MAX_DETECTIONS) maxDetections = EI_MAX_DETECTIONS;
  const float threshold = gSettings.edgeImpulseMinConfidence;
  const TfLiteIntArray* dims = sOutputTensor->dims;

  float maxConf = 0.0f;
  int maxIdx = -1;
  if (dims->size == 4) {
    const int gridHeight = dims->data[1];
    const int gridWidth = dims->data[2];
    const int numClasses = dims->data[3];
    const int cellWidth = sModelInputWidth / gridWidth;
    const int cellHeight = sModelInputHeight / gridHeight;
    DEBUG_SYSTEMF("[EI_DEBUG]   FOMO grid=%dx%d, classes=%d", gridHeight, gridWidth, numClasses);
    for (int y = 0; y < gridHeight; y++) {
      for (int x = 0; x < gridWidth; x++) {
        const int cellOffset = (y * gridWidth + x) * numClasses;
        float bestConf = 0.0f;
        int bestClass = -1;
        for (int c = 1; c < numClasses; c++) {  // class 0 is background
          const float conf = outputConfidence(sOutputTensor, cellOffset + c);
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
    for (int i = 0; i < sModelClassCount && results.detectionCount < maxDetections; i++) {
      const float confidence = outputConfidence(sOutputTensor, i);
      if (confidence > maxConf) {
        maxConf = confidence;
        maxIdx = i;
      }
      if (confidence >= threshold) {
        addDetection(results, i, confidence, 0, 0, sModelInputWidth, sModelInputHeight);
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
static void inferRgb(const uint8_t* rgb, int width, int height, EiResults& results) {
  if (!sEiModelLoaded || !sInterpreter || !sInputTensor || !sOutputTensor || !sResizedBuffer) {
    results.errorMessage = "Interpreter not ready";
    return;
  }
  const uint32_t start = millis();
  DEBUG_SYSTEMF("[EI_DEBUG] Resizing %dx%d -> %dx%dx%d", width, height,
                sModelInputWidth, sModelInputHeight, sModelInputChannels);
  if (!resizeRgb888(rgb, width, height, sResizedBuffer, sModelInputWidth, sModelInputHeight)) {
    results.errorMessage = "Failed to resize image";
    return;
  }
  fillInputTensor(sInputTensor, sResizedBuffer, sModelInputWidth, sModelInputHeight,
                  sModelInputChannels);

  const uint32_t invokeStart = millis();
  const TfLiteStatus invokeStatus = sInterpreter->Invoke();
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
// With no model there is nothing to run: a model loads with eimodelload, or
// with eiautostart at boot.
static const char* inferencePreflight() {
  if (!sEiInitialized || !sEiModelLoaded) return "No model loaded. Load one with eimodelload \"<file>\"";
  return nullptr;
}

// Camera entry point. Holds: inference lock, from capture to results.
static EiResults runCameraInferenceLocked() {
  EiResults results = {};
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
  if (results.success && sStateTrackingEnabled) {
    updateTrackedObjects(results);
  }
  return results;
}

EiResults runEdgeImpulseInference() {
  EiInferenceLock lock(0);
  if (!lock.held) {
    EiResults busy = {};
    busy.errorMessage = kEiBusyMessage;
    return busy;
  }
  return runCameraInferenceLocked();
}

// ============================================================================
// Inference from Stored Image File
// ============================================================================

EiResults runInferenceFromFile(const char* imagePath, const AuthContext& ctx) {
  EiResults results = {};
  EiInferenceLock lock(0);
  if (!lock.held) {
    results.errorMessage = kEiBusyMessage;
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

    imgBuffer = (uint8_t*)ps_alloc(fileSize, AllocPref::PreferPsram, "ei.img");
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

// Runs until a stop clears sEiContinuousRunning; its last action gives the
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
#if ENABLE_CAMERA_SENSOR
  // Continuous capture holds a stream session (capture dot, power-save
  // inhibitor) for the task's whole life.
  cameraSessionBegin(CameraSessionKind::Stream);
#endif
  
  while (sEiContinuousRunning) {
    // Held for one frame, capture to results. A detect command may hold it
    // for one frame too; load and unload stop this task before taking it.
    xSemaphoreTake(eiInferenceMutex(), portMAX_DELAY);
    if (sEiContinuousRunning && sEiModelLoaded) {
      inferenceCount++;
      const EiResults results = runCameraInferenceLocked();

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
    } else if (sEiContinuousRunning) {
      DEBUG_SYSTEMF("[EI_DEBUG] [Continuous] Skipping - no model loaded");
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

#if ENABLE_CAMERA_SENSOR
  cameraSessionEnd(CameraSessionKind::Stream);
#endif
  xSemaphoreGive(eiContinuousDone());
  vTaskDelete(nullptr);
}

// Holds: lifecycle lock. Returns true once no continuous task is left: the
// task finished its current frame and exited, so no second task can start
// beside it and the model can be freed or replaced. On a timeout the handle is
// kept, start refuses, and a later stop joins the task again.
static bool joinContinuousTaskLocked() {
  sEiContinuousRunning = false;
  if (!sEiContinuousTask) return true;
  xSemaphoreGive(eiContinuousWake());
  if (xSemaphoreTake(eiContinuousDone(), pdMS_TO_TICKS(kContinuousJoinTimeoutMs)) != pdTRUE) {
    ERROR_SYSTEMF("[EdgeImpulse] Continuous task did not stop within %lu ms",
                  (unsigned long)kContinuousJoinTimeoutMs);
    return false;
  }
  sEiContinuousTask = nullptr;
  DEBUG_SYSTEMF("[EI_DEBUG]   Continuous task joined");
  return true;
}

// Holds: lifecycle lock, with no continuous task left (joined).
static bool spawnContinuousTaskLocked() {
  if (sEiContinuousTask) return false;
  // Drop a wake left over from a stop that raced the task's own exit.
  while (xSemaphoreTake(eiContinuousWake(), 0) == pdTRUE) {
  }
  sEiContinuousRunning = true;

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
    &sEiContinuousTask,
    APP_CORE   // inference compute — keep off the Wi-Fi/BLE core
  );
  if (result != pdPASS) {
    ERROR_SYSTEMF("[EdgeImpulse] Failed to create continuous task (result=%d)", result);
    sEiContinuousRunning = false;
    sEiContinuousTask = nullptr;
    return false;
  }
  DEBUG_SYSTEMF("[EI_DEBUG]   Continuous inference started, task handle=%p", sEiContinuousTask);
  return true;
}

bool startContinuousInference() {
  DEBUG_SYSTEMF("[EI_DEBUG] startContinuousInference() called");
  EiLifecycleLock lifecycle;
  if (sEiContinuousTask) {
    if (sEiContinuousRunning) {
      DEBUG_SYSTEMF("[EI_DEBUG]   Skipping - already running");
      return true;
    }
    // A previous stop timed out; the old task must be gone first.
    if (!joinContinuousTaskLocked()) return false;
  }

  if (!sEiModelLoaded) {
    ERROR_SYSTEMF("[EdgeImpulse] Cannot start continuous - no model loaded");
    return false;
  }
  if (!spawnContinuousTaskLocked()) {
    return false;
  }

  setSetting(gSettings.edgeImpulseContinuous, true);
  {
    const char* slash = strrchr(sLoadedModelPath.c_str(), '/');
    systemEventPost(SYSEVT_EI_CONTINUOUS_STARTED,
                    slash ? slash + 1 : sLoadedModelPath.c_str());
  }
  return true;
}

bool stopContinuousInference() {
  DEBUG_SYSTEMF("[EI_DEBUG] stopContinuousInference() called");
  EiLifecycleLock lifecycle;
  const bool stopped = joinContinuousTaskLocked();
  setSetting(gSettings.edgeImpulseContinuous, false);
  return stopped;
}

bool isContinuousInferenceRunning() {
  return sEiContinuousRunning;
}

// ============================================================================
// JSON Output
// ============================================================================

// Detection fields shared by every detection reply (CLI and web).
static void appendDetectionJson(const EiResults& results, JsonDocument& doc) {
  doc["success"] = results.success;
  doc["inferenceTimeMs"] = results.inferenceTimeMs;
  // Reported model geometry. The web overlay reads modelInputWidth and
  // modelInputHeight; modelInputSize (the width) remains for older readers,
  // and the overlay falls back to it when a reply lacks the other two.
  doc["modelInputSize"] = sModelInputWidth;
  doc["modelInputWidth"] = sModelInputWidth;
  doc["modelInputHeight"] = sModelInputHeight;

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

void buildDetectionJson(const EiResults& results, String& output) {
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
  for (int i = 0; i < sTrackedObjectCount; i++) {
    JsonObject obj = objects.add<JsonObject>();
    // Copied (const char*): the reply is serialized after the lock is released.
    obj["label"] = static_cast<const char*>(sTrackedObjects[i].label);
    obj["prevLabel"] = static_cast<const char*>(sTrackedObjects[i].prevLabel);
    obj["confidence"] = sTrackedObjects[i].confidence;
    obj["x"] = sTrackedObjects[i].x;
    obj["y"] = sTrackedObjects[i].y;
    obj["width"] = sTrackedObjects[i].width;
    obj["height"] = sTrackedObjects[i].height;
    obj["stateChanged"] = sTrackedObjects[i].stateChanged;
    obj["lastSeenMs"] = sTrackedObjects[i].lastSeenMs;
    obj["stateChangeMs"] = sTrackedObjects[i].stateChangeMs;
  }
}

// Organize EI model files: move loose .tflite and .labels.txt into proper /EI Models/<name>/ folders
static esp_err_t handleEiOrganize(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  // Moving model files is a rename: administrators only (accounts below admin
  // never rename or delete files).
  if (!hasAdminPrivilege(ctx)) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"admin required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (!filesystemReady) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"filesystem_not_ready\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  File dir = VFS::openGuarded(kModelDir, "r", ctx);
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
    snprintf(srcModelBuf, sizeof(srcModelBuf), "%s/%s", kModelDir, tflite.c_str());
    snprintf(dstDirBuf, sizeof(dstDirBuf), "%s/%s", kModelDir, modelName.c_str());
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
    snprintf(srcLabelsBuf, sizeof(srcLabelsBuf), "%s/%s", kModelDir, labelsNameBuf);
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
    EiInferenceLock lock(0);
    if (!lock.held) {
      EiResults busy = {};
      busy.errorMessage = kEiBusyMessage;
      appendDetectionJson(busy, doc);
    } else {
      const EiResults results = runCameraInferenceLocked();
      appendDetectionJson(results, doc);
      appendTrackedObjectsJson(doc);
    }
  }

  const PsramJsonText jsonOutput(doc, "ei.detect.json");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_send(req, jsonOutput.c_str(), jsonOutput.length());

  return ESP_OK;
}

#endif  // ENABLE_HTTP_SERVER

// ============================================================================
// CLI Command Handlers (no web server dependency)
// ============================================================================

EXT_RAM_BSS_ATTR static char gEiCmdBuffer[512];

// ei, eimodel and eitrack only print help. Words after them are a subcommand
// in a spelling that runs nothing ("ei enable 1"), so they are refused.
static const char* eiHelpArgumentRefusal(const String& argsInput) {
  String trimmed = argsInput;
  trimmed.trim();
  if (trimmed.length() == 0) return nullptr;
  return "Error: Unknown Edge Impulse subcommand. The commands are single words, for example "
         "eidetect, eimodelload \"<file>\". Run ei for the list.";
}

const char* cmd_ei(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* refusal = eiHelpArgumentRefusal(argsInput)) return refusal;
  
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer),
    "Edge Impulse Commands:\n"
    "  eidetect            - Run single inference (from camera)\n"
    "  eifile \"<path>\"     - Run inference on stored JPEG file\n"
    "  eicontinuous <0|1>  - Start/stop continuous mode\n"
    "  eiconfidence <val>  - Set min confidence (0.0-1.0)\n"
    "  eistatus            - Show current status\n"
    "  eimodel             - Model management (list/load/info/unload)\n"
    "  eitrack             - State change tracking (status/enable/clear)\n"
  );
  return gEiCmdBuffer;
}

// The detection reply of eidetect and eifile: the whole JSON in the shared
// debug buffer, never a cut one.
static const char* detectionReply(const EiResults& results) {
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  PSRAM_JSON_DOC(doc);
  appendDetectionJson(results, doc);
  if (doc.overflowed() || measureJson(doc) >= GLOBAL_DEBUG_BUFFER_SIZE) {
    return "Error: Detection result does not fit the reply buffer. Raise eiconfidence to get fewer detections.";
  }
  char* reply = getDebugBuffer();
  serializeJson(doc, reply, GLOBAL_DEBUG_BUFFER_SIZE);
  return reply;
}

const char* cmd_ei_detect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  EiResults results = runEdgeImpulseInference();
  
  if (!results.success) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Error: Detection failed: %s",
      results.errorMessage ? results.errorMessage : "unknown error");
    return gEiCmdBuffer;
  }
  
  if (results.detectionCount == 0) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), 
      "No objects detected (inference: %lums)", results.inferenceTimeMs);
    return gEiCmdBuffer;
  }
  
  return detectionReply(results);
}

const char* cmd_ei_file(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedPath(a, 0, path);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: eifile \"<path>\"";

  // Check file exists
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Error: File not found: %s", path.c_str());
    return gEiCmdBuffer;
  }

  EiResults results = runInferenceFromFile(path.c_str(), currentAuthContext());
  
  if (!results.success) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Error: Inference failed: %s",
      results.errorMessage ? results.errorMessage : "unknown error");
    return gEiCmdBuffer;
  }
  
  if (results.detectionCount == 0) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), 
      "No objects detected in %s (inference: %lums)", path.c_str(), results.inferenceTimeMs);
    return gEiCmdBuffer;
  }
  
  return detectionReply(results);
}

const char* cmd_ei_continuous(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = argsInput;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Continuous mode: %s", 
      sEiContinuousRunning ? "running" : "stopped");
    return gEiCmdBuffer;
  }
  
  const int val = parseBoolArg(trimmed);
  if (val < 0) return kCommandReplyNotOnOff;
  if (val) {
    if (sEiContinuousRunning) {
      return "Error: Continuous inference is already running. Stop it first with 'eicontinuous 0' to restart it.";
    }
    if (startContinuousInference()) {
      return "Continuous inference started";
    }
    if (!sEiModelLoaded) return "Error: No model loaded. Load one with eimodelload \"<file>\" first.";
    return "Error: Continuous inference not started (previous run still stopping or no memory for the task). "
           "Retry in a few seconds.";
  } else {
    // Nothing to stop: no task, and the saved mode is already off. A saved
    // mode that is on (its start failed or waits for a model) is still turned
    // off below, so the settings editors can save Continuous Mode off.
    if (!sEiContinuousRunning && !sEiContinuousTask && !gSettings.edgeImpulseContinuous) {
      return "Error: Continuous inference is not running. Start it with 'eicontinuous 1'.";
    }
    if (!stopContinuousInference()) {
      return "Error: Continuous inference is still finishing a frame. Retry eicontinuous 0 in a few seconds.";
    }
    return "Continuous inference stopped";
  }
}

const char* cmd_ei_confidence(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = argsInput;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Min confidence: %.2f", 
      gSettings.edgeImpulseMinConfidence);
    return gEiCmdBuffer;
  }
  
  float val = 0.0f;
  if (!parseFloatArg(trimmed, 0.0, 1.0, val)) {
    return "Error: eiconfidence must be a decimal number from 0.0 to 1.0, for example 0.6.";
  }
  
  setSetting(gSettings.edgeImpulseMinConfidence, val);
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Min confidence set to %.2f", val);
  return gEiCmdBuffer;
}

const char* cmd_ei_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer),
    "Edge Impulse Status:\n"
    "  Initialized: %s\n"
    "  Model loaded: %s\n"
    "  Model path: %s\n"
    "  Continuous: %s\n"
    "  Min confidence: %.2f\n"
    "  Input size: %dx%d\n"
    "  Interval: %dms",
    sEiInitialized ? "yes" : "no",
    sEiModelLoaded ? "yes" : "no",
    sLoadedModelPath.length() > 0 ? sLoadedModelPath.c_str() : "(none)",
    sEiContinuousRunning ? "running" : "stopped",
    gSettings.edgeImpulseMinConfidence,
    sModelInputWidth, sModelInputHeight,
    gSettings.edgeImpulseIntervalMs
  );
  return gEiCmdBuffer;
}

// Model management commands
const char* cmd_ei_model(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* refusal = eiHelpArgumentRefusal(argsInput)) return refusal;
  
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer),
    "Model Commands:\n"
    "  eimodellist           - List available models in %s\n"
    "  eimodelload \"<name>\"  - Load a .tflite model\n"
    "  eimodelinfo           - Show loaded model details\n"
    "  eimodelunload         - Unload current model",
    kModelDir
  );
  return gEiCmdBuffer;
}

const char* cmd_ei_model_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String output;
  listAvailableModels(output);
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  // Whole lines only; lines that do not fit are counted, never cut.
  char* buf = getDebugBuffer();
  constexpr size_t kMoreNote = 24;  // "  ... (<n> more)\n" and its terminator
  size_t pos = 0;
  int left = 0;
  const char* line = output.c_str();
  while (*line) {
    const char* nl = strchr(line, '\n');
    const size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
    if (left || pos + len + kMoreNote >= GLOBAL_DEBUG_BUFFER_SIZE) {
      ++left;
    } else {
      memcpy(buf + pos, line, len);
      pos += len;
    }
    line += len;
  }
  buf[pos] = '\0';
  if (left) snprintf(buf + pos, GLOBAL_DEBUG_BUFFER_SIZE - pos, "  ... (%d more)\n", left);
  return buf;
}

const char* cmd_ei_model_load(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CommandArgs a(argsInput);
  String trimmed;
  const char* qerr = requireQuotedToken(a, 0, trimmed);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: eimodelload \"<filename>\"";

  // Build full path if just filename given
  String path = trimmed;
  if (!path.startsWith("/")) {
    char pathBuf[96];
    snprintf(pathBuf, sizeof(pathBuf), "%s/%s", kModelDir, trimmed.c_str());
    path = pathBuf;
  }
  
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Error: Model not found: %s", path.c_str());
    return gEiCmdBuffer;
  }
  
  // Try to load model first - only mark the module ready if model is valid
  if (loadModelFromFile(path.c_str())) {
    // The load allocated the model's buffers; readiness only needs the model
    // directory (initEdgeImpulse would also load default.tflite over it).
    markEdgeImpulseReady();
    EiInferenceLock inference(portMAX_DELAY);
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer),
      "Model loaded: %s\nInput: %dx%dx%d\nArena: %zu bytes",
      path.c_str(), sModelInputWidth, sModelInputHeight, sModelInputChannels,
      sInterpreter ? sInterpreter->arena_used_bytes() : 0);
  } else {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Error: Model not loaded: %s",
             sEiLoadFailure ? sEiLoadFailure : "see the system log. Check the file with eimodellist.");
  }
  return gEiCmdBuffer;
}

const char* cmd_ei_model_info(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  EiInferenceLock inference(portMAX_DELAY);  // the model cannot be freed while read
  if (!sEiModelLoaded || !sInterpreter) {
    return "Error: No model loaded. Load one with eimodelload \"<file>\".";
  }
  
  const char* inputType = "unknown";
  if (sInputTensor) {
    switch (sInputTensor->type) {
      case kTfLiteFloat32: inputType = "float32"; break;
      case kTfLiteUInt8: inputType = "uint8"; break;
      case kTfLiteInt8: inputType = "int8"; break;
      default: inputType = "other"; break;
    }
  }
  
  const char* outputType = "unknown";
  if (sOutputTensor) {
    switch (sOutputTensor->type) {
      case kTfLiteFloat32: outputType = "float32"; break;
      case kTfLiteUInt8: outputType = "uint8"; break;
      case kTfLiteInt8: outputType = "int8"; break;
      default: outputType = "other"; break;
    }
  }
  
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer),
    "Loaded Model Info:\n"
    "  Path: %s\n"
    "  Size: %zu bytes\n"
    "  Input: %dx%dx%d (%s)\n"
    "  Output dims: %d (%s)\n"
    "  Arena used: %zu bytes",
    sLoadedModelPath.c_str(),
    sModelSize,
    sModelInputWidth, sModelInputHeight, sModelInputChannels, inputType,
    sOutputTensor ? sOutputTensor->dims->data[sOutputTensor->dims->size - 1] : 0, outputType,
    sInterpreter->arena_used_bytes()
  );
  return gEiCmdBuffer;
}

const char* cmd_ei_model_unload(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!sEiModelLoaded) {
    return "Error: No model is loaded. Load one with 'eimodelload \"<file>\"'.";
  }
  
  String oldPath = sLoadedModelPath;
  unloadModel();
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Model unloaded: %s", oldPath.c_str());
  return gEiCmdBuffer;
}

// State tracking commands
const char* cmd_ei_track(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* refusal = eiHelpArgumentRefusal(argsInput)) return refusal;
  
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer),
    "State Tracking Commands:\n"
    "  eitrackstatus         - Show tracked objects\n"
    "  eitrackenable <0|1>   - Enable/disable tracking\n"
    "  eitrackclear          - Clear tracked objects"
  );
  return gEiCmdBuffer;
}

const char* cmd_ei_track_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  EiInferenceLock inference(portMAX_DELAY);  // tracking table is updated per frame
  if (!sStateTrackingEnabled) {
    return "Error: State tracking is disabled. Enable it with eitrackenable 1.";
  }

  if (sTrackedObjectCount == 0) {
    return "No objects currently tracked. Run continuous inference to track objects.";
  }
  
  char hdrBuf[32];
  snprintf(hdrBuf, sizeof(hdrBuf), "Tracked Objects (%d):\n", sTrackedObjectCount);
  String output = hdrBuf;
  uint32_t now = millis();
  
  for (int i = 0; i < sTrackedObjectCount; i++) {
    const TrackedObject& obj = sTrackedObjects[i];
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
  
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "%s", output.c_str());
  return gEiCmdBuffer;
}

const char* cmd_ei_track_enable(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String trimmed = argsInput;
  trimmed.trim();
  
  if (trimmed.length() == 0) {
    snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "State tracking: %s",
             sStateTrackingEnabled ? "enabled" : "disabled");
    return gEiCmdBuffer;
  }
  
  const int val = parseBoolArg(trimmed);
  if (val < 0) return kCommandReplyNotOnOff;
  setStateTrackingEnabled(val == 1);
  
  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "State tracking %s",
           sStateTrackingEnabled ? "enabled" : "disabled");
  return gEiCmdBuffer;
}

const char* cmd_ei_track_clear(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  int count = 0;
  {
    EiInferenceLock inference(portMAX_DELAY);
    count = sTrackedObjectCount;
    sTrackedObjectCount = 0;
  }

  snprintf(gEiCmdBuffer, sizeof(gEiCmdBuffer), "Cleared %d tracked objects", count);
  return gEiCmdBuffer;
}

// ---------------------------------------------------------------------------
// The CLI handlers above and the command table below are gated on
// ENABLE_EDGE_IMPULSE alone: kCommandModules[] in System_CommandRegistry.cpp
// references edgeImpulseCommands under `#if ENABLE_EDGE_IMPULSE` only, so an
// ENABLE_EDGE_IMPULSE=1, ENABLE_HTTP_SERVER=0 build needs every handler here.
// ---------------------------------------------------------------------------

// ============================================================================
// Command Registration
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry edgeImpulseCommands[] = {
  { "ei", "List the Edge Impulse commands.", false, cmd_ei, "Usage: ei" },
  { "eidetect", "Run single object detection inference.", false, cmd_ei_detect, "Usage: eidetect" },
  { "eifile", "Run inference on stored JPEG image.", false, cmd_ei_file, "Usage: eifile \"<path>\"" },
  { "eicontinuous", "Start/stop continuous inference mode.", false, cmd_ei_continuous, "Usage: eicontinuous <0|1>" },
  { "eiconfidence", "Set minimum detection confidence.", false, cmd_ei_confidence, "Usage: eiconfidence <0.0-1.0>" },
  { "eistatus", "Show Edge Impulse status.", false, cmd_ei_status, "Usage: eistatus" },
  { "eimodel", "List the model management commands.", false, cmd_ei_model, "Usage: eimodel" },
  { "eimodellist", "List available .tflite models.", false, cmd_ei_model_list, "Usage: eimodellist" },
  { "eimodelload", "Load a TFLite model from LittleFS.", false, cmd_ei_model_load, "Usage: eimodelload \"<filename>\"" },
  { "eimodelinfo", "Show loaded model information.", false, cmd_ei_model_info, "Usage: eimodelinfo" },
  { "eimodelunload", "Unload the current model.", false, cmd_ei_model_unload, "Usage: eimodelunload" },
  { "eitrack", "List the state tracking commands.", false, cmd_ei_track, "Usage: eitrack" },
  { "eitrackstatus", "Show currently tracked objects.", false, cmd_ei_track_status, "Usage: eitrackstatus" },
  { "eitrackenable", "Enable/disable state tracking.", false, cmd_ei_track_enable, "Usage: eitrackenable <0|1>" },
  { "eitrackclear", "Clear all tracked objects.", false, cmd_ei_track_clear, "Usage: eitrackclear" }
};

const size_t edgeImpulseCommandsCount = sizeof(edgeImpulseCommands) / sizeof(edgeImpulseCommands[0]);

// Auto-register commands at startup
// Registration handled by kCommandModules[] in System_Utils.cpp

#if ENABLE_HTTP_SERVER
// =============================================================================
// Register Edge Impulse Web Handlers
// =============================================================================

void registerEdgeImpulseHandlers(httpd_handle_t server) {
  static const httpd_uri_t eiOrganizePost = { .uri = "/api/ei/organize", .method = HTTP_POST, .handler = handleEiOrganize, .user_ctx = NULL };
  static const httpd_uri_t edgeImpulseDetect = { .uri = "/api/edgeimpulse/detect", .method = HTTP_GET, .handler = handleEdgeImpulseDetect, .user_ctx = NULL };
  httpd_register_uri_handler(server, &eiOrganizePost);
  httpd_register_uri_handler(server, &edgeImpulseDetect);
}

#endif // ENABLE_HTTP_SERVER

#endif // ENABLE_EDGE_IMPULSE
