/** Shared camera lifecycle and features; hardware drivers live behind HAL_Camera. */
#include "System_Camera_DVP.h"
#include "System_TaskUtils.h"
#include "System_Events.h"
#include "System_Filesystem.h"
#include <esp_attr.h>
#include "System_Camera_Video.h"
#include "System_BuildConfig.h"
#if ENABLE_CAMERA_SENSOR
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <esp_heap_caps.h>
#include "System_Debug.h"
#include "System_RamFlush.h"
#include "System_MemUtil.h"
#include "System_Command.h"
#include "System_Settings.h"
#include "System_I2C.h"
#include "System_Utils.h"
#include <ArduinoJson.h>
#include <atomic>

// Static storage prevents first-call races and never treats allocation failure
// as permission to access a driver without the lifecycle lock.
static StaticSemaphore_t gCameraMutexStorage;
static SemaphoreHandle_t gCameraMutex = xSemaphoreCreateRecursiveMutexStatic(&gCameraMutexStorage);
static bool lockCameraMutex(uint32_t timeoutMs) {
  return gCameraMutex && xSemaphoreTakeRecursive(gCameraMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
static void unlockCameraMutex() { xSemaphoreGiveRecursive(gCameraMutex); }
bool gCameraRunning = false;
bool cameraConnected = false;
bool cameraStreaming = false;
bool cameraDetected = false;
const char* cameraModel = "Unknown";
int cameraWidth = 0;
int cameraHeight = 0;
static char* cameraStatusBuffer = nullptr;
static const size_t kStatusBufSize = 4096;
static std::atomic<bool> sCameraDesiredOn{false};
static bool stopCameraInternal(bool isRecovery);

// Capability snapshots are small, immutable-value copies. Rendering never
// waits behind an in-flight capture or sensor initialization. The driver is
// still inspected only under its lifecycle mutex; a critical section protects
// the last complete snapshot while another task is using that driver.
static portMUX_TYPE sCameraCapsMutex = portMUX_INITIALIZER_UNLOCKED;
static CameraCapabilities sCameraCaps{"initializing", 0, 0, false, false, 0, 0};
static void cacheCameraCapabilities(const CameraCapabilities& caps) {
  portENTER_CRITICAL(&sCameraCapsMutex);
  sCameraCaps = caps;
  portEXIT_CRITICAL(&sCameraCapsMutex);
}
CameraCapabilities getCameraCapabilities() {
  if (lockCameraMutex(0)) {
    cacheCameraCapabilities(cameraBackend().capabilities());
    unlockCameraMutex();
  }
  portENTER_CRITICAL(&sCameraCapsMutex);
  const auto caps = sCameraCaps;
  portEXIT_CRITICAL(&sCameraCapsMutex);
  return caps;
}
bool cameraSupportsResolution(CameraFrameSize size) {
  return cameraFrameSizeInfo(size) && (getCameraCapabilities().resolutions & cameraResolutionBit(size));
}
bool cameraSupportsControl(CameraControl control) {
  return unsigned(control) < unsigned(CameraControl::Count) &&
         (getCameraCapabilities().controls & cameraControlBit(control));
}
bool getCameraControl(CameraControl control, int& value) {
  if (!lockCameraMutex(15000)) return false;
  const bool ok = gCameraRunning && cameraBackend().getControl(control, value);
  unlockCameraMutex();
  return ok;
}
bool setCameraControl(CameraControl control, int value) {
  if (!lockCameraMutex(15000)) return false;
  const bool ok = gCameraRunning && cameraSupportsControl(control) && cameraBackend().setControl(control, value);
  unlockCameraMutex();
  return ok;
}
static void updateCameraInfo() {
  cacheCameraCapabilities(cameraBackend().capabilities());
  const auto state = cameraBackend().info();
  cameraDetected = cameraDetected || state.detected;
  cameraModel = state.model;
  cameraWidth = state.width;
  cameraHeight = state.height;
}
static CameraConfig cameraConfigFromSettings() {
  CameraConfig config;
  config.resolution = cameraFrameSizeFromSetting(gSettings.cameraFramesize);
  config.quality = gSettings.cameraQuality;
  auto put = [&](CameraControl c, int value) { config.controls[unsigned(c)] = value; };
  put(CameraControl::Brightness, gSettings.cameraBrightness);
  put(CameraControl::Contrast, gSettings.cameraContrast);
  put(CameraControl::Saturation, gSettings.cameraSaturation);
  put(CameraControl::Sharpness, gSettings.cameraSharpness);
  put(CameraControl::Denoise, gSettings.cameraDenoise);
  put(CameraControl::WhiteBalanceMode, gSettings.cameraWBMode);
  put(CameraControl::Effect, gSettings.cameraSpecialEffect);
  put(CameraControl::HMirror, gSettings.cameraHMirror);
  put(CameraControl::VFlip, gSettings.cameraVFlip);
  put(CameraControl::ExposureLevel, gSettings.cameraAELevel);
  put(CameraControl::AutoExposure, 1);
  put(CameraControl::AutoGain, 1);
  put(CameraControl::GainCeiling, 6);
  put(CameraControl::WhiteBalance, 1);
  put(CameraControl::WhiteBalanceGain, 1);
  put(CameraControl::WhitePixelCorrection, 1);
  put(CameraControl::Gamma, 1);
  put(CameraControl::LensCorrection, 1);
  put(CameraControl::Downsize, 1);
  return config;
}
bool initCamera(bool isRecovery) {
  if (!lockCameraMutex(15000)) return false;
  if (gCameraRunning) { unlockCameraMutex(); return true; }
  if (isRecovery && !sCameraDesiredOn) { unlockCameraMutex(); return false; }
  auto config = cameraConfigFromSettings();
  // Saved IDs remain portable. A legacy size outside this backend's safe
  // envelope uses VGA/QVGA for this run and leaves the saved preference intact.
  const auto caps = cameraBackend().capabilities();
  cacheCameraCapabilities(caps);
  if (!(caps.resolutions & cameraResolutionBit(config.resolution))) {
    config.resolution = config.resolution != CameraFrameSize::Square96 &&
      (caps.resolutions & cameraResolutionBit(CameraFrameSize::VGA))
      ? CameraFrameSize::VGA : CameraFrameSize::QVGA;
    INFO_CAMERAF("Saved resolution %d unsupported by %s; using %u this run", gSettings.cameraFramesize, caps.backend, unsigned(config.resolution));
  }
  const bool ok = cameraBackend().begin(config);
  updateCameraInfo();
  if (!ok) {
    const auto state = cameraBackend().info();
    cameraConnected = gCameraRunning = false;
    const char* why = state.error ? state.error : "camera initialization failed";
    logSystemEvent("CAM", "camera init FAILED: %s", why);
    if (!isRecovery) systemEventPost(SYSEVT_SENSOR_START_FAILED, "Camera", why);
    unlockCameraMutex();
    return false;
  }
  cameraConnected = gCameraRunning = true;
  if (isRecovery && !sCameraDesiredOn) {
    (void)stopCameraInternal(true);
    unlockCameraMutex();
    return false;
  }
  sensorStatusBumpWith("opencamera");
  if (!isRecovery) {
    logSystemEvent("CAM", "camera online: %s (%dx%d), backend=%s", cameraModel, cameraWidth, cameraHeight, caps.backend);
    systemEventPost(SYSEVT_SENSOR_STARTED, "Camera", cameraModel);
  }
  unlockCameraMutex();
  return true;
}
static bool stopCameraInternal(bool isRecovery) {
  // The recorder can own the camera lock: join it before taking that lock.
  if (!isRecovery && videoRecording) stopVideoRecording();
  if (!lockCameraMutex(15000)) return false;
  const bool wasRunning = gCameraRunning;
  // A failed begin may retain resources when safe teardown itself failed.
  // Idempotent end must still be retried by an explicit stop in that state.
  if (!cameraBackend().end()) { unlockCameraMutex(); return false; }
  gCameraRunning = cameraConnected = cameraStreaming = false;
  cacheCameraCapabilities(cameraBackend().capabilities());
  if (wasRunning) sensorStatusBumpWith("closecamera");
  if (wasRunning && !isRecovery) systemEventPost(SYSEVT_SENSOR_STOPPED, "Camera");
  unlockCameraMutex();
  return true;
}
void stopCamera(bool isRecovery) { (void)stopCameraInternal(isRecovery); }

bool captureCameraFrame(CameraFrame& out, size_t maxBytes) {
  cameraFrameRelease(out);
  if (maxBytes < 4 || !lockCameraMutex(0)) return false;
  if (!gCameraRunning) { unlockCameraMutex(); return false; }
  bool ok = cameraBackend().capture(out, maxBytes);
  if (!ok) {
    cameraFrameRelease(out);
    const bool stopped = stopCameraInternal(true);
    vTaskDelay(pdMS_TO_TICKS(150));
    ok = stopped && sCameraDesiredOn && initCamera(true) && cameraBackend().capture(out, maxBytes);
    if (!sCameraDesiredOn) ok = false;
  }
  if (!ok || !cameraFrameIsValid(out, maxBytes)) {
    cameraFrameRelease(out);
    unlockCameraMutex();
    return false;
  }
  cameraWidth = out.width;
  cameraHeight = out.height;
  unlockCameraMutex();
  return true;
}
uint8_t* captureFrame(size_t* outLen) {
  if (outLen) *outLen = 0;
  CameraFrame frame;
  if (!captureCameraFrame(frame)) return nullptr;
  if (outLen) *outLen = frame.length;
  return frame.data;
}
bool setCameraResolution(CameraFrameSize size) {
  if (!cameraFrameSizeInfo(size) || !lockCameraMutex(15000)) return false;
  const bool ok = gCameraRunning && cameraBackend().setResolution(size);
  if (ok) updateCameraInfo();
  unlockCameraMutex();
  return ok;
}
bool setCameraQuality(int quality) {
  if (quality < 0 || quality > 63 || !lockCameraMutex(15000)) return false;
  const bool ok = gCameraRunning && cameraBackend().setQuality(quality);
  unlockCameraMutex();
  return ok;
}
uint8_t* captureFrameAtResolution(CameraFrameSize size, int quality, size_t* outLen) {
  if (outLen) *outLen = 0;
  if (!cameraSupportsResolution(size) || quality < 0 || quality > 63 || !lockCameraMutex(15000)) return nullptr;
  if (!gCameraRunning) { unlockCameraMutex(); return nullptr; }
  const auto old = cameraBackend().info();
  CameraFrame frame;
  bool changedSize = cameraBackend().setResolution(size);
  bool changedQuality = changedSize && cameraBackend().setQuality(quality);
  bool ok = changedQuality && cameraBackend().capture(frame, kCameraMaxFrameBytes) && cameraFrameIsValid(frame, kCameraMaxFrameBytes);
  bool restored = true;
  if (changedQuality) restored = cameraBackend().setQuality(old.quality);
  if (changedSize) restored = cameraBackend().setResolution(old.resolution) && restored;
  updateCameraInfo();
  if (!restored) {
    // Continuing with temporary geometry would corrupt subsequent recording
    // headers. Close the driver and require an explicit start to recover.
    (void)stopCameraInternal(true);
    ok = false;
  }
  if (!ok) cameraFrameRelease(frame);
  unlockCameraMutex();
  if (outLen) *outLen = frame.length;
  return frame.data;
}
uint8_t* captureTinyFrame(size_t* outLen) { return captureFrameAtResolution(CameraFrameSize::QQVGA, 40, outLen); }

const char* buildCameraStatusJson() {
  if (!lockCameraMutex(15000)) return "{\"error\":\"Camera busy\"}";
  if (!cameraStatusBuffer) cameraStatusBuffer = static_cast<char*>(ps_alloc(kStatusBufSize, AllocPref::PreferPSRAM, "camera.status.json"));
  if (!cameraStatusBuffer) { unlockCameraMutex(); return "{}"; }
  const auto caps = cameraBackend().capabilities();
  const auto state = cameraBackend().info();
  PSRAM_JSON_DOC(doc);
  doc["supported"] = true;
  doc["detected"] = cameraDetected;
  doc["enabled"] = gCameraRunning;
  doc["connected"] = cameraConnected;
  doc["streaming"] = cameraStreaming;
  doc["model"] = cameraModel;
  doc["width"] = cameraWidth;
  doc["height"] = cameraHeight;
  doc["psram"] = psramFound();
  doc["backend"] = caps.backend;
  doc["hardwareJpeg"] = caps.hardwareJpeg;
  doc["sourceWidth"] = caps.sourceWidth;
  doc["sourceHeight"] = caps.sourceHeight;
  doc["upscaled"] = caps.sourceWidth && (cameraWidth > caps.sourceWidth || cameraHeight > caps.sourceHeight);
  doc["framing"] = caps.sourceWidth ? "center-crop" : "sensor";
  doc["requestedFramesize"] = gSettings.cameraFramesize;
  doc["framesize"] = unsigned(state.resolution);
  doc["quality"] = state.quality;
  doc["maxFrameBytes"] = kCameraMaxFrameBytes;
  doc["error"] = state.error;
  JsonArray sizes = doc["resolutions"].to<JsonArray>();
  for (unsigned i = 0; i < unsigned(CameraFrameSize::Count); ++i) {
    if (!(caps.resolutions & cameraResolutionBit(CameraFrameSize(i)))) continue;
    const auto* size = cameraFrameSizeInfo(CameraFrameSize(i));
    JsonObject entry = sizes.add<JsonObject>();
    entry["id"] = i; entry["name"] = size->name;
    entry["width"] = size->width; entry["height"] = size->height;
  }
  JsonArray controls = doc["controls"].to<JsonArray>();
  JsonObject values = doc["controlValues"].to<JsonObject>();
  for (unsigned i = 0; i < unsigned(CameraControl::Count); ++i) {
    const auto c = CameraControl(i);
    if (!(caps.controls & cameraControlBit(c))) continue;
    controls.add(cameraControlName(c));
    int value;
    if (gCameraRunning && cameraBackend().getControl(c, value)) values[cameraControlName(c)] = value;
  }
  if (doc.overflowed() || measureJson(doc) >= kStatusBufSize) {
    unlockCameraMutex(); return "{\"error\":\"Camera status overflow\"}";
  }
  serializeJson(doc, cameraStatusBuffer, kStatusBufSize);
  unlockCameraMutex();
  return cameraStatusBuffer;
}

// =============================================================================
// Camera power worker — init/stop/restart run here (large stack), not on the
// G2 tap dispatcher (4 KB) or other shallow stacks. captureFrame() recovery
// still calls init/stop inline while holding the camera mutex from the same
// task; do not route that path through this queue (deadlock risk).
// =============================================================================

enum : uint8_t {
  CAM_PWR_CMD_START   = 0,
  CAM_PWR_CMD_STOP    = 1,
  CAM_PWR_CMD_RESTART = 2,
};

struct CameraPwrMsg {
  uint8_t  cmd;
  uint8_t  completionSlot;
  uint16_t reserved;
  uint32_t completionGeneration;
};

struct CameraPwrCompletionSlot {
  StaticSemaphore_t storage;
  SemaphoreHandle_t done;
  uint32_t          generation;
  bool              inUse;
  bool              waiterAttached;
  bool              completed;
  bool              result;
};

static QueueHandle_t       sCamPwrQueue = nullptr;
static TaskHandle_t        sCamPwrTask  = nullptr;
static CameraPowerPostHook sCamPwrHook  = nullptr;
static StaticSemaphore_t   sCamPwrLifecycleMutexStorage;
static SemaphoreHandle_t   sCamPwrLifecycleMutex =
    xSemaphoreCreateMutexStatic(&sCamPwrLifecycleMutexStorage);
// Synchronous completions use bounded static storage rather than a caller task
// handle. A timed-out waiter detaches, but its generation remains reserved
// until the queued/in-flight worker completes it; only then can the slot be
// reused. This avoids stale task handles and never consumes or overwrites
// another subsystem's task notification.
constexpr uint8_t          kCamPwrNoCompletion = 0xff;
constexpr uint8_t          kCamPwrCompletionSlotCount = 4;
static CameraPwrCompletionSlot
    sCamPwrCompletions[kCamPwrCompletionSlotCount] = {};
static UBaseType_t         sCamPwrAdmissions = 0;
static TickType_t          sCamPwrLastDetachTick = 0;
static bool                sCamPwrHasDetached = false;

// A full quiet interval after the last completed command is the retirement
// boundary. The drained queue is deleted and the published pair is detached
// under sCamPwrLifecycleMutex, so admissions either land on this worker or
// create a wholly new pair; no sender can retain a queue being deleted.
constexpr uint32_t kCamPwrIdleRetireMs = 10000;

static bool cameraPwrLifecycleTake() {
  return sCamPwrLifecycleMutex &&
         xSemaphoreTake(sCamPwrLifecycleMutex, portMAX_DELAY) == pdTRUE;
}

static void cameraPwrLifecycleGive() {
  xSemaphoreGive(sCamPwrLifecycleMutex);
}

static bool cameraPwrReserveCompletionLocked(CameraPwrMsg& m) {
  for (uint8_t i = 0; i < kCamPwrCompletionSlotCount; ++i) {
    CameraPwrCompletionSlot& slot = sCamPwrCompletions[i];
    if (slot.inUse) continue;

    if (!slot.done) {
      slot.done = xSemaphoreCreateBinaryStatic(&slot.storage);
      if (!slot.done) {
        ERROR_CAMERAF("[CAM_PWR] static completion semaphore init failed slot=%u",
                      (unsigned)i);
        return false;
      }
    }
    // A give that raced just after a previous caller timed out can leave the
    // binary semaphore full. Generation matching makes it harmless; draining
    // here prevents it from completing this new request immediately.
    while (xSemaphoreTake(slot.done, 0) == pdTRUE) {}

    ++slot.generation;
    if (slot.generation == 0) ++slot.generation;
    slot.waiterAttached = true;
    slot.completed = false;
    slot.result = false;
    slot.inUse = true;
    m.completionSlot = i;
    m.completionGeneration = slot.generation;
    return true;
  }
  ERROR_CAMERAF("[CAM_PWR] all %u static completion slots are busy",
                (unsigned)kCamPwrCompletionSlotCount);
  return false;
}

static void cameraPwrReleaseCompletionLocked(const CameraPwrMsg& m) {
  if (m.completionSlot >= kCamPwrCompletionSlotCount) return;
  CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
  if (slot.inUse && slot.generation == m.completionGeneration) {
    slot.inUse = false;
    slot.waiterAttached = false;
    slot.completed = false;
  }
}

static void cameraPwrComplete(const CameraPwrMsg& m, bool result) {
  if (m.completionSlot >= kCamPwrCompletionSlotCount) return;
  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable for completion");
    return;
  }
  CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
  if (slot.inUse && slot.generation == m.completionGeneration) {
    slot.result = result;
    slot.completed = true;
    if (!slot.waiterAttached) {
      // The caller timed out while this command was queued or running. The
      // worker is the final owner of the generation and releases it now; no
      // stale signal is published and reuse was impossible before this point.
      cameraPwrReleaseCompletionLocked(m);
    } else if (xSemaphoreGive(slot.done) != pdTRUE) {
      ERROR_CAMERAF("[CAM_PWR] completion semaphore already full slot=%u gen=%lu",
                    (unsigned)m.completionSlot,
                    (unsigned long)m.completionGeneration);
    }
  }
  cameraPwrLifecycleGive();
}

void cameraPowerSetPostHook(CameraPowerPostHook hook) {
  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable; post hook unchanged");
    return;
  }
  sCamPwrHook = hook;
  cameraPwrLifecycleGive();
}

static void cameraPwrRunOne(const CameraPwrMsg& m) {
  bool result = false;
  switch (m.cmd) {
    case CAM_PWR_CMD_STOP:
      result = stopCameraInternal(/*isRecovery=*/false);
      break;
    case CAM_PWR_CMD_START:
      if (!gCameraRunning) {
        if (!initCamera()) {
          BROADCAST_PRINTF("[CAM_PWR] initCamera failed — reverting camera auto-start");
          setSetting(gSettings.cameraAutoStart, false);
          systemEventPost(SYSEVT_SENSOR_START_FAILED, "Camera", "init failed");
          // The camera is off because init failed, not because the user closed it —
          // don't let a ramflush capture read this as an intentional close.
          ramFlushMarkAutostartFailed(RF_CAMERA);
        }
      }
      result = gCameraRunning;
      break;
    case CAM_PWR_CMD_RESTART: {
      const bool was = gCameraRunning;
      if (was) {
        if (!stopCameraInternal(/*isRecovery=*/false)) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!initCamera()) {
          BROADCAST_PRINTF("[CAM_PWR] restart: initCamera failed after stop");
        }
      }
      result = gCameraRunning;
      break;
    }
    default:
      break;
  }
  CameraPowerPostHook hook = nullptr;
  if (cameraPwrLifecycleTake()) {
    hook = sCamPwrHook;
    cameraPwrLifecycleGive();
  }
  if (hook) {
    hook();
  }
  cameraPwrComplete(m, result);
}

static void cameraPwrWorker(void* arg) {
  QueueHandle_t ownQueue = static_cast<QueueHandle_t>(arg);
  CameraPwrMsg m;
  for (;;) {
    if (xQueueReceive(ownQueue, &m,
                      pdMS_TO_TICKS(kCamPwrIdleRetireMs)) == pdTRUE) {
      cameraPwrRunOne(m);
      continue;
    }

    // Serialise the empty re-check with every ensure+send operation. If a
    // sender won the mutex first, its item is visible and this worker stays.
    // If retirement wins, both globals are detached before the mutex is
    // released, so the sender creates a new queue/task and never touches this
    // queue. No task is force-deleted; this worker owns its final vTaskDelete.
    if (!cameraPwrLifecycleTake()) continue;
    const bool ownsPublishedPair =
        sCamPwrTask == xTaskGetCurrentTaskHandle() &&
        sCamPwrQueue == ownQueue;
    const bool queueDrained = uxQueueMessagesWaiting(ownQueue) == 0;
    if (!ownsPublishedPair || !queueDrained || sCamPwrAdmissions != 0) {
      cameraPwrLifecycleGive();
      continue;
    }

    // No sender can be blocked on or retain ownQueue: every send holds an
    // admission reference from pointer capture through xQueueSend completion.
    // Delete the drained queue while new admissions are still excluded, then
    // detach the pair. The only unavoidable overlap left is the old dynamic
    // task's stack/TCB, which FreeRTOS reclaims later from an IDLE task after
    // this worker self-deletes; a replacement allocation may therefore need a
    // second 10 KB stack briefly and can fail explicitly under heap pressure.
    vQueueDelete(ownQueue);
    sCamPwrTask = nullptr;
    sCamPwrQueue = nullptr;
    sCamPwrLastDetachTick = xTaskGetTickCount();
    sCamPwrHasDetached = true;
    cameraPwrLifecycleGive();

    vTaskDelete(nullptr);
  }
}

// Caller must hold sCamPwrLifecycleMutex. Publishing and teardown of the pair
// happen under that same mutex, so a non-null pair always accepts the send that
// follows ensure. Dynamic xTaskCreate keeps the stack in internal DRAM.
static bool cameraPwrEnsureStartedLocked() {
  if (sCamPwrQueue && sCamPwrTask) {
    return true;
  }
  if (sCamPwrQueue || sCamPwrTask) {
    ERROR_CAMERAF("[CAM_PWR] inconsistent lifecycle state (queue=%p task=%p)",
                  (void*)sCamPwrQueue, (void*)sCamPwrTask);
    return false;
  }
  constexpr UBaseType_t kDepth = 6;
  // ESP-IDF xTaskCreate stack depth is in BYTES — 10240 here is 10 KB.
  // Observed HWM under light load was ~2.5–7 KB; leave headroom until a
  // worst-case capture/stream measurement justifies cutting.
  constexpr uint32_t  kStack = 10240;
  sCamPwrQueue = xQueueCreate(kDepth, sizeof(CameraPwrMsg));
  if (!sCamPwrQueue) {
    ERROR_CAMERAF("[CAM_PWR] queue create failed (DRAM free=%u largest=%u)",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (sCamPwrHasDetached) {
      ERROR_CAMERAF("[CAM_PWR] previous self-deleted worker detached %lu ms ago; "
                    "its internal stack/TCB are reclaimed asynchronously by IDLE",
                    (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() -
                                                 sCamPwrLastDetachTick));
    }
    return false;
  }
  taskStackRecord("cam_pwr", kStack);
  const BaseType_t ok =
      // Keep camera SCCB initialization off the radio-heavy core. The backend
      // owns its camera bus; no application Wire scan runs here.
      xTaskCreatePinnedToCore(cameraPwrWorker, "cam_pwr", kStack, sCamPwrQueue,
                  tskIDLE_PRIORITY + 2, &sCamPwrTask, I2C_SENSOR_CORE);
  if (ok != pdPASS) {
    ERROR_CAMERAF("[CAM_PWR] worker task create failed — need ~%u B internal stack "
                  "(DRAM free=%u largest=%u)",
                  (unsigned)kStack,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (sCamPwrHasDetached) {
      ERROR_CAMERAF("[CAM_PWR] previous self-deleted worker detached %lu ms ago; "
                    "its internal stack/TCB are reclaimed asynchronously by IDLE",
                    (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() -
                                                 sCamPwrLastDetachTick));
    }
    vQueueDelete(sCamPwrQueue);
    sCamPwrQueue = nullptr;
    sCamPwrTask  = nullptr;
    return false;
  }
  return true;
}

bool cameraPowerWorkerEnsureStarted() {
  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable; cannot start worker");
    return false;
  }
  const bool ok = cameraPwrEnsureStartedLocked();
  cameraPwrLifecycleGive();
  return ok;
}

static bool cameraPwrSend(CameraPwrMsg& m, TickType_t queueTicks,
                          bool needsCompletion = false) {
  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable; command=%u rejected",
                  (unsigned)m.cmd);
    return false;
  }

  // Desired power is authoritative and published at admission, before any
  // allocation or queue operation can fail. In particular, a STOP that cannot
  // recreate cam_pwr still fences captureFrame's inline recovery from re-init.
  // Plain initCamera()/stopCamera() and recovery do not mutate this latch.
  sCameraDesiredOn = m.cmd != CAM_PWR_CMD_STOP;

  if (!cameraPwrEnsureStartedLocked()) {
    cameraPwrLifecycleGive();
    return false;
  }
  if (needsCompletion && !cameraPwrReserveCompletionLocked(m)) {
    cameraPwrLifecycleGive();
    return false;
  }
  QueueHandle_t targetQueue = sCamPwrQueue;
  ++sCamPwrAdmissions;
  cameraPwrLifecycleGive();

  // Do not block on a full queue while holding the lifecycle mutex: the worker
  // copies the post-hook under that mutex before it can receive the next item.
  // The admission reference prevents retirement/deletion while this send is in
  // flight, so targetQueue remains valid across the blocking call.
  const bool sent = xQueueSend(targetQueue, &m, queueTicks) == pdTRUE;

  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable after command=%u send",
                  (unsigned)m.cmd);
    return false;
  }
  configASSERT(sCamPwrAdmissions > 0);
  --sCamPwrAdmissions;
  const UBaseType_t queued = sent ? 0 : uxQueueMessagesWaiting(targetQueue);
  if (!sent) cameraPwrReleaseCompletionLocked(m);
  cameraPwrLifecycleGive();
  if (!sent) {
    ERROR_CAMERAF("[CAM_PWR] queue send failed command=%u queued=%u",
                  (unsigned)m.cmd, (unsigned)queued);
  }
  return sent;
}

bool cameraPowerRequestStartAsync() {
  CameraPwrMsg m{CAM_PWR_CMD_START, kCamPwrNoCompletion, 0, 0};
  return cameraPwrSend(m, 0);
}

bool cameraPowerRequestStopAsync() {
  CameraPwrMsg m{CAM_PWR_CMD_STOP, kCamPwrNoCompletion, 0, 0};
  return cameraPwrSend(m, 0);
}

static bool cameraPwrWaitDone(const CameraPwrMsg& m, uint32_t waitMs,
                              bool& result) {
  if (m.completionSlot >= kCamPwrCompletionSlotCount) return false;

  SemaphoreHandle_t done = nullptr;
  if (cameraPwrLifecycleTake()) {
    CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
    if (slot.inUse && slot.generation == m.completionGeneration) {
      done = slot.done;
    }
    cameraPwrLifecycleGive();
  }
  if (!done || xSemaphoreTake(done, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
    if (cameraPwrLifecycleTake()) {
      CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
      if (slot.inUse && slot.generation == m.completionGeneration) {
        slot.waiterAttached = false;
        if (slot.completed) {
          // Completion raced the timeout and already published its give. The
          // worker no longer owns this generation, so drain and release it;
          // the API still reports the elapsed timeout honestly.
          while (xSemaphoreTake(slot.done, 0) == pdTRUE) {}
          cameraPwrReleaseCompletionLocked(m);
        }
        // Otherwise leave inUse set. The queued/in-flight worker owns the last
        // reference and will release this generation from cameraPwrComplete().
      }
      cameraPwrLifecycleGive();
    }
    return false;
  }

  bool matched = false;
  if (cameraPwrLifecycleTake()) {
    CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
    matched = slot.inUse && slot.generation == m.completionGeneration &&
              slot.completed;
    if (matched) result = slot.result;
    cameraPwrReleaseCompletionLocked(m);
    cameraPwrLifecycleGive();
  }
  return matched;
}

bool cameraPowerRequestStartSync(uint32_t waitMs) {
  CameraPwrMsg m{CAM_PWR_CMD_START, kCamPwrNoCompletion, 0, 0};
  if (!cameraPwrSend(m, pdMS_TO_TICKS(5000), true)) {
    return false;
  }
  bool result = false;
  if (!cameraPwrWaitDone(m, waitMs, result)) {
    ERROR_CAMERAF("[CAM_PWR] start timed out slot=%u gen=%lu",
                  (unsigned)m.completionSlot,
                  (unsigned long)m.completionGeneration);
    return false;
  }
  return result;
}

bool cameraPowerRequestStopSync(uint32_t waitMs) {
  CameraPwrMsg m{CAM_PWR_CMD_STOP, kCamPwrNoCompletion, 0, 0};
  if (!cameraPwrSend(m, pdMS_TO_TICKS(5000), true)) {
    return false;
  }
  bool result = false;
  if (!cameraPwrWaitDone(m, waitMs, result)) {
    ERROR_CAMERAF("[CAM_PWR] stop timed out slot=%u gen=%lu",
                  (unsigned)m.completionSlot,
                  (unsigned long)m.completionGeneration);
    return false;
  }
  return result;
}

bool cameraPowerRequestRestartSync(uint32_t waitMs) {
  CameraPwrMsg m{CAM_PWR_CMD_RESTART, kCamPwrNoCompletion, 0, 0};
  if (!cameraPwrSend(m, pdMS_TO_TICKS(5000), true)) {
    return false;
  }
  bool result = false;
  if (!cameraPwrWaitDone(m, waitMs, result)) {
    ERROR_CAMERAF("[CAM_PWR] restart timed out slot=%u gen=%lu",
                  (unsigned)m.completionSlot,
                  (unsigned long)m.completionGeneration);
    return false;
  }
  return result;
}


// Command handlers
const char* cmd_camera(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return buildCameraStatusJson();
}

const char* cmd_camerastart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gSettings.cameraEnabled) {
    return "ERROR: Camera is disabled - run 'cameraenabled 1' first";
  }
  if (cameraPowerRequestStartSync(60000)) {
    return "Camera started successfully";
  }
  return "Error: Camera initialization failed";
}

const char* cmd_camerastop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cameraPowerRequestStopSync(30000)
             ? "Camera stopped"
             : "Error: Camera stop failed or timed out";
}

const char* cmd_cameracapture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) {
    return "Error: Camera not enabled - run opencamera first";
  }
  
  size_t len = 0;
  uint8_t* frame = captureFrame(&len);
  if (frame) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Captured frame: %u bytes", (unsigned)len);
    free(frame);
    // The captured frame is measured then discarded — it is not written to
    // storage. To save a photo to disk, use 'camerasave'.
    cliHint("to save a photo to storage, run 'camerasave'");
    return result;
  }
  return "Error: Frame capture failed";
}

static const char* applyCameraResolutionSetting(CameraFrameSize size) {
  if (!cameraSupportsResolution(size)) return "Error: Resolution unsupported by this camera backend; see cameraread";
  // A recording has one fixed geometry in its AVI header. Finish it before a
  // live resolution change, without waiting for its task under the camera lock.
  if (videoRecording) stopVideoRecording();
  if (!lockCameraMutex(15000)) return "Error: Camera busy; resolution not saved";
  const bool wasRunning = gCameraRunning;
  if (wasRunning && !setCameraResolution(size)) {
    unlockCameraMutex();
    return "Error: Failed to apply resolution; setting not saved";
  }
  setSetting(gSettings.cameraFramesize, int(size));
  const bool wasStreaming = cameraStreaming;
  if (wasRunning) cameraStreaming = false;
  const auto* dims = cameraFrameSizeInfo(size);
  EXT_RAM_BSS_ATTR static char result[160];
  snprintf(result, sizeof(result), "Resolution set to %ux%u (saved). %s",
           unsigned(wasRunning ? cameraWidth : dims->width),
           unsigned(wasRunning ? cameraHeight : dims->height),
           wasStreaming ? "Streaming stopped; restart the stream." :
           wasRunning ? "Applied live." : "Will apply on next camera start.");
  unlockCameraMutex();
  return result;
}
const char* cmd_camerares(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  if (!arg.length()) {
    EXT_RAM_BSS_ATTR static char result[192];
    const auto* requested = cameraFrameSizeInfo(cameraFrameSizeFromSetting(gSettings.cameraFramesize));
    snprintf(result, sizeof(result), "%s: %dx%d\nUsage: camerares <name|WIDTHxHEIGHT>\nUse cameraread for this backend's available resolutions.",
             gCameraRunning ? "Current" : "Saved preference",
             gCameraRunning ? cameraWidth : int(requested->width),
             gCameraRunning ? cameraHeight : int(requested->height));
    return result;
  }
  for (unsigned i = 0; i < unsigned(CameraFrameSize::Count); ++i) {
    const auto* size = cameraFrameSizeInfo(CameraFrameSize(i));
    char dimensions[24];
    snprintf(dimensions, sizeof(dimensions), "%ux%u", unsigned(size->width), unsigned(size->height));
    if (arg == size->name || arg == dimensions) return applyCameraResolutionSetting(size->id);
  }
  return "Error: Unknown resolution; see cameraread for available names and dimensions";
}

// Numeric values are the persistent IDs declared by CameraFrameSize, never a
// vendor driver's frame-size enumeration. Existing IDs 0..10 stay unchanged.
const char* cmd_cameraframesize(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (!arg.length()) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "cameraFramesize=%d", gSettings.cameraFramesize);
    return result;
  }
  char* tail = nullptr;
  const long size = strtol(arg.c_str(), &tail, 10);
  if (!tail || *tail || size < 0 || size >= int(CameraFrameSize::Count))
    return "Error: Framesize must be an integer 0-12; see cameraread for supported IDs";
  return applyCameraResolutionSetting(CameraFrameSize(size));
}

const char* cmd_cameraquality(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[80];
    snprintf(result, sizeof(result), "Current: %d\nUsage: cameraquality <0-63> (lower = better quality, larger file)", 
             gSettings.cameraQuality);
    return result;
  }
  
  char* tail = nullptr;
  const long parsed = strtol(valStr.c_str(), &tail, 10);
  if (!tail || *tail || parsed < 0 || parsed > 63) {
    return "Error: Quality must be an integer 0-63";
  }
  const int quality = int(parsed);
  
  if (gCameraRunning && !setCameraQuality(quality)) return "Error: Failed to apply JPEG quality; setting not saved";
  setSetting(gSettings.cameraQuality, quality);
  
  // Apply live if camera is running (quality can be changed without restart)
  if (gCameraRunning) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "JPEG quality set to %d (saved, applied live)", quality);
    return result;
  }
  
  EXT_RAM_BSS_ATTR static char result[64];
  snprintf(result, sizeof(result), "JPEG quality set to %d (saved, will apply on camera start)", quality);
  return result;
}

const char* cmd_cameratiny(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) {
    return "Error: Camera not enabled - run opencamera first";
  }
  
  size_t len = 0;
  uint8_t* frame = captureTinyFrame(&len);
  if (frame) {
    EXT_RAM_BSS_ATTR static char result[96];
    snprintf(result, sizeof(result), "Tiny frame (160x120): %u bytes %s", 
             (unsigned)len, len <= 250 ? "(ESP-NOW compatible)" : "(too large for single ESP-NOW packet)");
    free(frame);
    return result;
  }
  return "Error: Tiny frame capture failed";
}


// All control operations use the lifecycle mutex through the shared API.
static const char* cameraIntegerCommand(const String& args, CameraControl control,
                                       int minimum, int maximum, int* setting = nullptr) {
  if (!gCameraRunning) return "Error: Camera not enabled";
  if (!cameraSupportsControl(control)) return "Error: Control unsupported by this camera backend";
  EXT_RAM_BSS_ATTR static char result[128];
  String arg = args; arg.trim();
  if (!arg.length()) {
    int value;
    if (!getCameraControl(control, value)) return "Error: Camera control unavailable";
    snprintf(result, sizeof(result), "%s: %d (range %d to %d)", cameraControlName(control), value, minimum, maximum);
    return result;
  }
  char* end = nullptr;
  long value = strtol(arg.c_str(), &end, 10);
  if (!end || *end || value < minimum || value > maximum) return "Error: Control value outside supported range";
  if (!setCameraControl(control, int(value))) return "Error: Failed to apply camera control";
  if (setting) setSetting(*setting, int(value));
  snprintf(result, sizeof(result), "%s set to %ld%s", cameraControlName(control), value, setting ? " (saved)" : "");
  return result;
}
static const char* cameraBoolToggle(const String& args, CameraControl control, bool* setting = nullptr) {
  if (!gCameraRunning) return "Error: Camera not enabled";
  if (!cameraSupportsControl(control)) return "Error: Control unsupported by this camera backend";
  EXT_RAM_BSS_ATTR static char result[96];
  String arg = args; arg.trim();
  int value = 0;
  if (!arg.length()) {
    if (!getCameraControl(control, value)) return "Error: Camera control unavailable";
  } else {
    if (arg.equalsIgnoreCase("auto") && (control == CameraControl::AutoExposure || control == CameraControl::AutoGain)) value = 1;
    else value = parseBoolArg(arg);
    if (value < 0) return "Error: Use on or off";
    if (!setCameraControl(control, value)) return "Error: Failed to apply camera control";
    if (setting) setSetting(*setting, value != 0);
  }
  snprintf(result, sizeof(result), "%s: %s%s", cameraControlName(control), value ? "ON" : "OFF", arg.length() && setting ? " (saved)" : "");
  return result;
}
#define CAMERA_INT_COMMAND(fn, control, low, high, field) \
const char* fn(const String& argsInput) { \
  RETURN_VALID_IF_VALIDATE_CSTR(); \
  return cameraIntegerCommand(argsInput, CameraControl::control, low, high, &gSettings.field); \
}
CAMERA_INT_COMMAND(cmd_camerabrightness, Brightness, -2, 2, cameraBrightness)
CAMERA_INT_COMMAND(cmd_cameracontrast, Contrast, -2, 2, cameraContrast)
CAMERA_INT_COMMAND(cmd_camerasaturation, Saturation, -2, 2, cameraSaturation)
CAMERA_INT_COMMAND(cmd_camerawb, WhiteBalanceMode, 0, 4, cameraWBMode)
CAMERA_INT_COMMAND(cmd_camerasharpness, Sharpness, -2, 2, cameraSharpness)
CAMERA_INT_COMMAND(cmd_cameradenoise, Denoise, 0, 8, cameraDenoise)
CAMERA_INT_COMMAND(cmd_cameraeffect, Effect, 0, 6, cameraSpecialEffect)
CAMERA_INT_COMMAND(cmd_cameraexposure, ExposureLevel, -2, 2, cameraAELevel)
#undef CAMERA_INT_COMMAND
#define CAMERA_BOOL_COMMAND(fn, control) \
const char* fn(const String& argsInput) { \
  RETURN_VALID_IF_VALIDATE_CSTR(); \
  return cameraBoolToggle(argsInput, CameraControl::control); \
}
CAMERA_BOOL_COMMAND(cmd_cameraaec, AutoExposure)
CAMERA_BOOL_COMMAND(cmd_cameraagc, AutoGain)
CAMERA_BOOL_COMMAND(cmd_camerawhitebal, WhiteBalance)
CAMERA_BOOL_COMMAND(cmd_cameraawbgain, WhiteBalanceGain)
CAMERA_BOOL_COMMAND(cmd_cameraaec2, NightMode)
CAMERA_BOOL_COMMAND(cmd_cameradcw, Downsize)
CAMERA_BOOL_COMMAND(cmd_camerabpc, BlackPixelCorrection)
CAMERA_BOOL_COMMAND(cmd_camerawpc, WhitePixelCorrection)
CAMERA_BOOL_COMMAND(cmd_cameragamma, Gamma)
CAMERA_BOOL_COMMAND(cmd_cameralenc, LensCorrection)
CAMERA_BOOL_COMMAND(cmd_cameracolorbar, ColorBar)
#undef CAMERA_BOOL_COMMAND
const char* cmd_camerahmirror(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cameraBoolToggle(argsInput, CameraControl::HMirror, &gSettings.cameraHMirror);
}
const char* cmd_cameravflip(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cameraBoolToggle(argsInput, CameraControl::VFlip, &gSettings.cameraVFlip);
}
const char* cmd_cameragainceiling(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cameraIntegerCommand(argsInput, CameraControl::GainCeiling, 0, 6);
}
static const char* cameraManualCommand(const String& args, CameraControl automatic,
                                       CameraControl manual, int maximum) {
  String arg = args; arg.trim();
  if (!arg.length()) return "Error: Exposure/gain value required";
  char* tail;
  long value = strtol(arg.c_str(), &tail, 10);
  if (*tail || value < 0 || value > maximum) return "Error: Value outside supported range";
  if (!cameraSupportsControl(automatic) || !cameraSupportsControl(manual)) return "Error: Control unsupported by this camera backend";
  if (!lockCameraMutex(15000)) return "Error: Camera busy";
  int oldAuto = 0;
  bool haveOld = getCameraControl(automatic, oldAuto);
  bool ok = haveOld && setCameraControl(automatic, 0) && setCameraControl(manual, int(value));
  if (!ok && haveOld) (void)setCameraControl(automatic, oldAuto);
  unlockCameraMutex();
  return ok ? "Manual camera control applied" : "Error: Failed to apply manual camera control";
}
const char* cmd_cameraaecvalue(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cameraManualCommand(argsInput, CameraControl::AutoExposure, CameraControl::ExposureValue, 1200);
}
const char* cmd_cameraagcgain(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cameraManualCommand(argsInput, CameraControl::AutoGain, CameraControl::Gain, 30);
}
const char* cmd_camerareg(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  unsigned address, mask, value;
  if (sscanf(argsInput.c_str(), "%x %x %x", &address, &mask, &value) != 3) return "Error: Usage: camerareg <address> <mask> <value>";
  if (!lockCameraMutex(15000)) return "Error: Camera busy";
  bool ok = gCameraRunning && cameraBackend().capabilities().registerAccess && cameraBackend().writeRegister && cameraBackend().writeRegister(address, mask, value);
  unlockCameraMutex();
  return ok ? "Camera register written" : "Error: Register write unsupported or failed";
}
const char* cmd_cameradump(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!lockCameraMutex(15000)) return "Error: Camera busy";
  bool ok = gCameraRunning && cameraBackend().dump && cameraBackend().dump();
  unlockCameraMutex();
  return ok ? "Sensor status dumped (see [CAM_DUMP] lines)" : "Error: Sensor dump unsupported or unavailable";
}
const char* cmd_camerafx(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  if (!cameraSupportsControl(CameraControl::Brightness) || !cameraSupportsControl(CameraControl::Contrast) || !cameraSupportsControl(CameraControl::Saturation)) return "Error: Control unsupported by this camera backend";
  String arg = argsInput; arg.trim();
  if (!arg.length()) {
    EXT_RAM_BSS_ATTR static char result[128];
    int bri, con, sat;
    if (!getCameraControl(CameraControl::Brightness, bri) || !getCameraControl(CameraControl::Contrast, con) || !getCameraControl(CameraControl::Saturation, sat)) return "Error: Camera control unavailable";
    snprintf(result, sizeof(result), "camerafx: bri=%d con=%d sat=%d. Usage: camerafx <bri> <con> <sat>", bri, con, sat);
    return result;
  }
  int bri, con, sat;
  if (sscanf(arg.c_str(), "%d %d %d", &bri, &con, &sat) != 3 || bri < -2 || bri > 2 || con < -2 || con > 2 || sat < -2 || sat > 2) return "Error: Each value must be -2..2";
  if (!lockCameraMutex(15000)) return "Error: Camera busy";
  bool ok = setCameraControl(CameraControl::Contrast, con) && setCameraControl(CameraControl::Brightness, bri) && setCameraControl(CameraControl::Saturation, sat);
  if (ok) {
    setSetting(gSettings.cameraBrightness, bri);
    setSetting(gSettings.cameraContrast, con);
    setSetting(gSettings.cameraSaturation, sat);
  }
  unlockCameraMutex();
  return ok ? "Camera brightness, contrast and saturation applied (saved)" : "Error: Camera effect sequence failed; settings not saved";
}
const char* cmd_camerarotate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  if (!cameraSupportsControl(CameraControl::HMirror) || !cameraSupportsControl(CameraControl::VFlip)) return "Error: Control unsupported by this camera backend";
  String arg = argsInput; arg.trim();
  if (!arg.length()) return gSettings.cameraHMirror && gSettings.cameraVFlip ? "Rotate 180: ON" : "Rotate 180: OFF";
  int value = arg == "180" ? 1 : parseBoolArg(arg);
  if (value < 0) return "Error: Use on or off";
  if (!lockCameraMutex(15000)) return "Error: Camera busy";
  int oldMirror = 0;
  bool haveOld = getCameraControl(CameraControl::HMirror, oldMirror);
  bool ok = haveOld && setCameraControl(CameraControl::HMirror, value) && setCameraControl(CameraControl::VFlip, value);
  if (ok) {
    setSetting(gSettings.cameraHMirror, value != 0);
    setSetting(gSettings.cameraVFlip, value != 0);
  } else if (haveOld) (void)setCameraControl(CameraControl::HMirror, oldMirror);
  unlockCameraMutex();
  return ok ? "Camera rotation applied (saved)" : "Error: Failed to apply camera rotation";
}

const char* cmd_camerafps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String valStr = argsInput;
  valStr.trim();

  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[96];
    snprintf(buf, sizeof(buf), "Camera FPS: %d fps\nUsage: camerafps <1-20>",
             gSettings.cameraStreamFps);
    return buf;
  }

  int val = valStr.toInt();
  if (val < 1 || val > 20) return "Error: cameraStreamFps must be 1-20";
  setSetting(gSettings.cameraStreamFps, val);
  EXT_RAM_BSS_ATTR static char buf[64];
  snprintf(buf, sizeof(buf), "cameraStreamFps set to %d fps", val);
  return buf;
}


// ============================================================================
// Camera Settings Commands
// ============================================================================

const char* cmd_cameraautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.cameraAutoStart ? "[Camera] Auto-start: enabled" : "[Camera] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.cameraAutoStart, true);
    return "[Camera] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.cameraAutoStart, false);
    return "[Camera] Auto-start disabled";
  }
  return "Error: invalid arguments — Usage: cameraautostart [on|off]";
}

const char* cmd_camerastoragelocation(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[64];
    snprintf(buf, sizeof(buf), "cameraStorageLocation = %d (0=LittleFS, 1=SD, 2=Both)", gSettings.cameraStorageLocation);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 0 || val > 2) return "Error: cameraStorageLocation must be 0-2";
  setSetting(gSettings.cameraStorageLocation, val);
  EXT_RAM_BSS_ATTR static char buf[48];
  snprintf(buf, sizeof(buf), "cameraStorageLocation set to %d", val);
  return buf;
}

const char* cmd_cameracapturefolder(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = argsInput;
  val.trim();
  
  if (val.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[256];
    snprintf(buf, sizeof(buf), "cameraCaptureFolder = %s", gSettings.cameraCaptureFolder.c_str());
    return buf;
  }
  setSetting(gSettings.cameraCaptureFolder, val);
  EXT_RAM_BSS_ATTR static char buf[256];
  snprintf(buf, sizeof(buf), "cameraCaptureFolder set to %s", val.c_str());
  return buf;
}

const char* cmd_cameramaxstoredimages(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[64];
    snprintf(buf, sizeof(buf), "cameraMaxStoredImages = %d", gSettings.cameraMaxStoredImages);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 0 || val > 1000) return "Error: cameraMaxStoredImages must be 0-1200";
  setSetting(gSettings.cameraMaxStoredImages, val);
  EXT_RAM_BSS_ATTR static char buf[48];
  snprintf(buf, sizeof(buf), "cameraMaxStoredImages set to %d", val);
  return buf;
}

const char* cmd_cameraautocapture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    return gSettings.cameraAutoCapture ? "cameraAutoCapture = true" : "cameraAutoCapture = false";
  }
  bool enable = (arg == "1" || arg.equalsIgnoreCase("true") || arg.equalsIgnoreCase("on"));
  setSetting(gSettings.cameraAutoCapture, enable);
  // Set default capture folder if enabling and folder is empty
  if (enable && gSettings.cameraCaptureFolder.length() == 0) {
    setSetting(gSettings.cameraCaptureFolder, String("/photos"));
  }
  return enable ? "cameraAutoCapture set to true" : "cameraAutoCapture set to false";
}

const char* cmd_cameraautocaptureinterval(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[64];
    snprintf(buf, sizeof(buf), "cameraAutoCaptureInterval = %d sec", gSettings.cameraAutoCaptureIntervalSec);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 10 || val > 3600) return "Error: cameraAutoCaptureInterval must be 10-3600";
  setSetting(gSettings.cameraAutoCaptureIntervalSec, val);
  EXT_RAM_BSS_ATTR static char buf[48];
  snprintf(buf, sizeof(buf), "cameraAutoCaptureInterval set to %d sec", val);
  return buf;
}

const char* cmd_camerasendaftercapture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    return gSettings.cameraSendAfterCapture ? "cameraSendAfterCapture = true" : "cameraSendAfterCapture = false";
  }
  bool enable = (arg == "1" || arg.equalsIgnoreCase("true") || arg.equalsIgnoreCase("on"));
  setSetting(gSettings.cameraSendAfterCapture, enable);
  return enable ? "cameraSendAfterCapture set to true" : "cameraSendAfterCapture set to false";
}

const char* cmd_cameratargetdevice(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = argsInput;
  val.trim();
  
  if (val.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[256];
    snprintf(buf, sizeof(buf), "cameraTargetDevice = %s", gSettings.cameraTargetDevice.c_str());
    return buf;
  }
  setSetting(gSettings.cameraTargetDevice, val);
  EXT_RAM_BSS_ATTR static char buf[256];
  snprintf(buf, sizeof(buf), "cameraTargetDevice set to %s", val.c_str());
  return buf;
}

// Forward declare ImageManager for camerasave
#include "System_ImageManager.h"
extern ImageManager gImageManager;

const char* cmd_camerasave(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) {
    return "Error: Camera not enabled - run opencamera first";
  }
  
  // Determine storage location from settings
  ImageStorageLocation loc = IMAGE_STORAGE_LITTLEFS;
  if (gSettings.cameraStorageLocation == 1) loc = IMAGE_STORAGE_SD;
  else if (gSettings.cameraStorageLocation == 2) loc = IMAGE_STORAGE_BOTH;
  
  // Ensure capture folder exists and set default if needed
  if (gSettings.cameraCaptureFolder.length() == 0) {
    setSetting(gSettings.cameraCaptureFolder, String("/photos"));
  }
  
  // Capture and save
  String savedPath = gImageManager.captureAndSave(loc);
  if (savedPath.length() > 0) {
    EXT_RAM_BSS_ATTR static char result[128];
    snprintf(result, sizeof(result), "Saved: %s", savedPath.c_str());
    return result;
  }
  return "Error: Failed to save image";
}

// ── Video recording commands ────────────────────────────────────────────────
// These forward to System_Camera_Video. Kept here so they register alongside
// the rest of the camera command table.
EXT_RAM_BSS_ATTR static char gCameraCmdBuffer[192];

const char* cmd_camerarecord(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled - run opencamera first";

  String arg = argsInput;
  arg.trim();

  if (arg.length() == 0) {
    return videoRecording ? "Recording: active" : "Recording: stopped";
  }
  if (arg == "1" || arg.equalsIgnoreCase("start")) {
    return startVideoRecording() ? "Recording started"
                                 : "Error: Failed to start recording (SD card available?)";
  }
  if (arg == "0" || arg.equalsIgnoreCase("stop")) {
    bool wasRecording = videoRecording;
    stopVideoRecording();
    if (!wasRecording) return "Recording stopped";
    EXT_RAM_BSS_ATTR static char out[160];
    snprintf(out, sizeof(out), "Recording stopped — %s (%lu frames)",
             videoLastRecordingPath(), (unsigned long)videoLastRecordingFrames());
    return out;
  }
  return "Error: invalid arguments — Usage: camerarecord <start|stop|1|0>";
}

const char* cmd_cameravideolist(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["count"] = 0;  // patched in place below, once the single walk is parsed
    JsonArray arr = doc["recordings"].to<JsonArray>();
    // One walk only — see the note in handleVideoRecordingsList.
    String list = getVideoRecordingsList();  // "name:size\nname:size"
    int count = 0;
    int start = 0;
    while (start < (int)list.length()) {
      int nl = list.indexOf('\n', start);
      String entry = (nl < 0) ? list.substring(start) : list.substring(start, nl);
      entry.trim();
      if (entry.length()) {
        int colon = entry.lastIndexOf(':');
        JsonObject o = arr.add<JsonObject>();
        if (colon > 0) { o["filename"] = entry.substring(0, colon); o["size"] = entry.substring(colon + 1).toInt(); }
        else           { o["filename"] = entry; }
        count++;
      }
      if (nl < 0) break;
      start = nl + 1;
    }
    doc["count"] = count;  // in-place update: keeps "count" first in the object
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  // Single walk; entries are newline-joined, so the count follows from the string.
  String list = getVideoRecordingsList();
  if (list.length() == 0) return "No video recordings found";
  int count = 1;
  for (int i = 0; i < (int)list.length(); i++) if (list[i] == '\n') count++;
  snprintf(gCameraCmdBuffer, sizeof(gCameraCmdBuffer),
           "Recordings (%d):\n%s", count, list.c_str());
  return gCameraCmdBuffer;
}

const char* cmd_cameravideodelete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  String name;
  const char* qerr = requireQuotedToken(a, 0, name);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: cameravideodelete \"<filename>\"";
  return deleteVideoRecording(name) ? "Deleted" : "Error: Delete failed (file not found or SD unavailable)";
}

// Command registry
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry cameraCommands[] = {
  {"cameraread",       "Read camera status",              false, cmd_camera},
  {"opencamera",       "Start camera sensor.",            false, cmd_camerastart},
  {"closecamera",      "Stop camera sensor.",             false, cmd_camerastop},
  {"cameracapture",    "Capture a single frame",          false, cmd_cameracapture},
  {"camerasave",       "Save current frame to storage",   false, cmd_camerasave},
  {"camerares",        "Set camera resolution: <res>",    false, cmd_camerares, "Usage: camerares <96x96|qqvga|qcif|hqvga|240x240|qvga|cif|vga|svga|xga|sxga|uxga|hd>"},
  {"cameraframesize",  "Set resolution by index: <0-12>", true,  cmd_cameraframesize, "Usage: cameraframesize <0..12> (0-5: QVGA..UXGA, 6-10: small, 11: HD, 12: CIF; availability depends on backend)"},
  {"cameraquality",    "Set JPEG quality: <0-63>",        false, cmd_cameraquality, "Usage: cameraquality <0..63> (lower = better quality, larger file)"},
  {"camerafps",            "Camera FPS: <1-20>",          true, cmd_camerafps, "Usage: camerafps <1..20>"},
  {"cameratiny",       "Capture tiny frame for ESP-NOW",  false, cmd_cameratiny},
  {"camerabrightness", "Set brightness: <-2..2>",         false, cmd_camerabrightness, "Usage: camerabrightness <-2..2>"},
  {"cameracontrast",   "Set contrast: <-2..2>",           false, cmd_cameracontrast, "Usage: cameracontrast <-2..2>"},
  {"camerasaturation", "Set saturation: <-2..2>",         false, cmd_camerasaturation, "Usage: camerasaturation <-2..2>"},
  {"camerawb",         "White balance mode: <0-4>",       true,  cmd_camerawb, "Usage: camerawb <0..4> (0=Auto,1=Sunny,2=Cloudy,3=Office,4=Home)"},
  {"camerasharpness",  "Set sharpness: <-2..2>",          true,  cmd_camerasharpness, "Usage: camerasharpness <-2..2> (OV3660 only)"},
  {"cameradenoise",    "Set denoise level: <0-8>",        true,  cmd_cameradenoise, "Usage: cameradenoise <0..8>"},
  {"cameraeffect",     "Special effect: <0-6>",           true,  cmd_cameraeffect, "Usage: cameraeffect <0..6> (0=None,1=Negative,2=Grayscale,3=Red,4=Green,5=Blue,6=Sepia)"},
  {"cameraexposure",   "Set AE level: <-2..2>",           true,  cmd_cameraexposure, "Usage: cameraexposure <-2..2> (negative=darker)"},
  {"cameraaec",        "Auto exposure: <on|off>",         true,  cmd_cameraaec, "Usage: cameraaec <on|off|1|0|true|auto>"},
  {"cameraaecvalue",   "Exposure value: <0-1200>",        true,  cmd_cameraaecvalue, "Usage: cameraaecvalue <0..1200>"},
  {"cameraagc",        "Auto gain: <on|off>",             true,  cmd_cameraagc, "Usage: cameraagc <on|off|1|0|true|auto>"},
  {"cameraagcgain",    "Gain value: <0-30>",              true,  cmd_cameraagcgain, "Usage: cameraagcgain <0..30>"},
  // ── Runtime sensor tuning (no persistence — for testing OV3660 image quality) ──
  {"cameragainceiling","Gainceiling: <0-6> (2X..128X)",   true,  cmd_cameragainceiling, "Usage: cameragainceiling <0..6> (2X..128X)"},
  {"camerawhitebal",   "AWB master: <on|off>",            true,  cmd_camerawhitebal, "Usage: camerawhitebal <on|off>"},
  {"cameraawbgain",    "AWB gain: <on|off>",              true,  cmd_cameraawbgain, "Usage: cameraawbgain <on|off>"},
  {"cameraaec2",       "Night mode (slower fps, brighter in low light): <on|off>", true,  cmd_cameraaec2, "Usage: cameraaec2 <on|off>"},
  {"cameradcw",        "Downsize crop window: <on|off>",  true,  cmd_cameradcw, "Usage: cameradcw <on|off>"},
  {"camerabpc",        "Black pixel correction: <on|off>",true,  cmd_camerabpc, "Usage: camerabpc <on|off>"},
  {"camerawpc",        "White pixel correction: <on|off>",true,  cmd_camerawpc, "Usage: camerawpc <on|off>"},
  {"cameragamma",      "Raw gamma: <on|off>",             true,  cmd_cameragamma, "Usage: cameragamma <on|off>"},
  {"cameralenc",       "Lens shading correction: <on|off>",true, cmd_cameralenc, "Usage: cameralenc <on|off>"},
  {"cameracolorbar",   "Color bar test pattern: <on|off>",true,  cmd_cameracolorbar, "Usage: cameracolorbar <on|off>"},
  {"camerareg",        "Direct register write: <addr_hex> <mask_hex> <value_hex>", true, cmd_camerareg, "Usage: camerareg <addr_hex> <mask_hex> <value_hex> (example: camerareg 0x3824 0x1f 0x04)"},
  {"cameradump",       "Print all current sensor values", false, cmd_cameradump},
  {"camerafx",         "Set bri/con/sat together: <bri> <con> <sat> (-2..+2 each)", false, cmd_camerafx, "Usage: camerafx <bri> <con> <sat> (-2..+2 each)"},
  {"camerahmirror",    "Horizontal mirror: <on|off>",     false, cmd_camerahmirror, "Usage: camerahmirror <on|off|1|0|true>"},
  {"cameravflip",      "Vertical flip: <on|off>",         false, cmd_cameravflip, "Usage: cameravflip <on|off|1|0|true>"},
  {"camerarotate",     "Rotate 180°: <on|off>",           false, cmd_camerarotate, "Usage: camerarotate <on|off|1|0|true|180>"},
  {"cameraautostart",  "Auto-start: <on|off>",            true,  cmd_cameraautostart, "Usage: cameraautostart <on|off|1|0|true|false>"},
  {"camerastoragelocation", "Storage location: <0-2>",    true,  cmd_camerastoragelocation, "Usage: camerastoragelocation <0..2> (0=LittleFS,1=SD,2=Both)"},
  {"cameracapturefolder",   "Photo folder: <path>",       true,  cmd_cameracapturefolder, "Usage: cameracapturefolder <path>"},
  {"cameramaxstoredimages", "Max stored: <0-1200>",       true,  cmd_cameramaxstoredimages, "Usage: cameramaxstoredimages <0..1200> (0=unlimited)"},
  {"cameraautocapture",     "Auto-capture: <on|off>",     true,  cmd_cameraautocapture, "Usage: cameraautocapture <on|off|1|0|true>"},
  {"cameraautocaptureinterval", "Auto-capture: <sec>",    true, cmd_cameraautocaptureinterval, "Usage: cameraautocaptureinterval <10..3600>"},
  {"camerasendaftercapture", "Send after capture: <on|off>", true, cmd_camerasendaftercapture, "Usage: camerasendaftercapture <on|off|1|0|true>"},
  {"cameratargetdevice",    "Target device: <name>",      true,  cmd_cameratargetdevice, "Usage: cameratargetdevice <name>"},
  {"camerarecord",          "Start/stop MJPEG-AVI recording (SD only): <start|stop>", false, cmd_camerarecord, "Usage: camerarecord <start|stop|1|0>"},
  {"cameravideolist",       "List AVI recordings on SD (add 'json' for JSON output)",  false, cmd_cameravideolist},
  {"cameravideodelete",     "Delete recording: \"<filename>\"", true, cmd_cameravideodelete, "Usage: cameravideodelete \"<filename>\""},
};

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry cameraSettingEntries[] = {
  { "cameraEnabled", SETTING_BOOL, &gSettings.cameraEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "cameraenabled" },
  { "cameraAutoStart", SETTING_BOOL, &gSettings.cameraAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "cameraautostart" },
  { "cameraFramesize", SETTING_INT, &gSettings.cameraFramesize, 10, 0, nullptr, 0, 12, "Resolution", "0:320x240 (QVGA),1:640x480 (VGA),2:800x600 (SVGA),3:1024x768 (XGA),4:1280x1024 (SXGA),5:1600x1200 (UXGA),"
    "6:96x96,7:160x120 (QQVGA),8:176x144 (QCIF),9:240x176 (HQVGA),10:240x240,11:1280x720 (HD),12:400x296 (CIF)", false, "image", nullptr },
  { "cameraBrightness", SETTING_INT, &gSettings.cameraBrightness, 2, 0, nullptr, -2, 2, "Brightness (-2 to 2)", nullptr, false, "tuning", "camerabrightness" },
  { "cameraContrast", SETTING_INT, &gSettings.cameraContrast, 2, 0, nullptr, -2, 2, "Contrast (-2 to 2)", nullptr, false, "tuning", "cameracontrast" },
  { "cameraSaturation", SETTING_INT, &gSettings.cameraSaturation, 2, 0, nullptr, -2, 2, "Saturation (-2 to 2)", nullptr, false, "tuning", "camerasaturation" },
  { "cameraAELevel", SETTING_INT, &gSettings.cameraAELevel, 0, 0, nullptr, -2, 2, "Exposure Compensation (-2 to 2)", nullptr, false, "tuning", "cameraexposure" },
  { "cameraWBMode", SETTING_INT, &gSettings.cameraWBMode, 0, 0, nullptr, 0, 4, "White Balance", "0:Auto,1:Sunny,2:Cloudy,3:Office,4:Home", false, "tuning", "camerawb" },
  { "cameraSharpness", SETTING_INT, &gSettings.cameraSharpness, 0, 0, nullptr, -2, 2, "Sharpness (-2 to 2, OV3660)", nullptr, false, "tuning", "camerasharpness" },
  { "cameraDenoise", SETTING_INT, &gSettings.cameraDenoise, 0, 0, nullptr, 0, 8, "Denoise (0-8)", nullptr, false, "tuning", "cameradenoise" },
  { "cameraSpecialEffect", SETTING_INT, &gSettings.cameraSpecialEffect, 0, 0, nullptr, 0, 6, "Special Effect", "0:None,1:Negative,2:Grayscale,3:Red Tint,4:Green Tint,5:Blue Tint,6:Sepia", false, "tuning", "cameraeffect" },
  { "cameraHMirror", SETTING_BOOL, &gSettings.cameraHMirror, 0, 0, nullptr, 0, 1, "Horizontal mirror", nullptr, false, "image", "camerahmirror" },
  { "cameraVFlip", SETTING_BOOL, &gSettings.cameraVFlip, 0, 0, nullptr, 0, 1, "Vertical flip", nullptr, false, "image", "cameravflip" },
  { "cameraQuality", SETTING_INT, &gSettings.cameraQuality, 12, 0, nullptr, 0, 63, "JPEG quality (0-63, lower=better)", nullptr, false, "image", "cameraquality" },
  { "cameraStreamFps", SETTING_INT, &gSettings.cameraStreamFps, 5, 0, nullptr, 1, 20, "Camera FPS (higher=smoother)", nullptr, false, "image", "camerafps" },
  { "g2StreamToneMap", SETTING_INT, &gSettings.g2StreamToneMap, 1, 0, nullptr, 0, 3,
    "G2 lens 4-bpp tone", "0:Linear,1:Balanced,2:Shadows,3:Legacy", false, "image", "g2streamtonemap" },
  { "g2PackRateMs", SETTING_INT, &gSettings.g2PackRateMs, 80, 0, nullptr, 20, 2000, "G2 SD-pack animation cadence (ms per frame)", nullptr, false, "image", "g2packrate" },
  { "cameraStorageLocation", SETTING_INT, &gSettings.cameraStorageLocation, 1, 0, nullptr, 0, 2, "Storage Location", "0:LittleFS (Internal),1:SD Card,2:Both", false, "storage", "camerastoragelocation" },
  { "cameraCaptureFolder", SETTING_STRING, &gSettings.cameraCaptureFolder, 0, 0, "/photos", 0, 0, "Photo folder path", nullptr, false, "storage", "cameracapturefolder" },
  { "cameraMaxStoredImages", SETTING_INT, &gSettings.cameraMaxStoredImages, 100, 0, nullptr, 0, 1000, "Max images (0=unlimited)", nullptr, false, "storage", "cameramaxstoredimages" },
  { "cameraAutoCapture", SETTING_BOOL, &gSettings.cameraAutoCapture, 0, 0, nullptr, 0, 1, "Enable auto-capture", nullptr, false, "autoCapture", "cameraautocapture" },
  { "cameraAutoCaptureInterval", SETTING_INT, &gSettings.cameraAutoCaptureIntervalSec, 60, 0, nullptr, 10, 3600, "Auto-capture interval (sec)", nullptr, false, "autoCapture", "cameraautocaptureinterval" },
  { "cameraSendAfterCapture", SETTING_BOOL, &gSettings.cameraSendAfterCapture, 0, 0, nullptr, 0, 1, "Send to target after capture", nullptr, false, "autoCapture", "camerasendaftercapture" },
  { "cameraTargetDevice", SETTING_STRING, &gSettings.cameraTargetDevice, 0, 0, nullptr, 0, 0, "ESP-NOW target device name", nullptr, false, "autoCapture", "cameratargetdevice" },
};

static bool isCameraConnected() {
  if (!gCameraRunning) return true;
  return cameraConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule cameraSettingsModule = {
  "camera",
  "hardware.sensors.camera",
  cameraSettingEntries,
  sizeof(cameraSettingEntries) / sizeof(cameraSettingEntries[0]),
  isCameraConnected,
  "Camera capture and image settings"
};
const size_t cameraCommandsCount = sizeof(cameraCommands) / sizeof(cameraCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

#endif // ENABLE_CAMERA_SENSOR
