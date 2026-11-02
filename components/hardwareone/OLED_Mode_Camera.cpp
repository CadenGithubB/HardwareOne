#include "OLED_Mode_Camera.h"
#if ENABLE_OLED_DISPLAY && ENABLE_CAMERA_SENSOR
#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "OLED_UI.h"
#include "OLED_MessageText.h"
#include "HAL_Input.h"
#include "HAL_JPEG.h"
#include "CameraMonochrome.h"
#include "System_Camera_DVP.h"
#include "System_User.h"
#include "System_Power.h"
#include "System_TaskUtils.h"
#include "System_MemUtil.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Utils.h" // Command validation context
#include <ArduinoJson.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <new>

namespace {
constexpr unsigned kWidth = DISPLAY_WIDTH;
constexpr unsigned kHeight = OLED_CONTENT_HEIGHT - 9;
constexpr size_t kBytes = ((kWidth + 7) / 8) * kHeight;
constexpr uint32_t kIntervalMs = 400;
static_assert(kHeight > 0 && kWidth <= 256 && kHeight <= 256, "Camera preview viewport is bounded");
struct View {
  bool active = false, worker = false, working = false, frozen = false, ready = false;
  bool dither = true, running = false, busy = false;
  int contrast = 0;
  uint32_t generation = 1, revision = 0, sequence = 0, updatedMs = 0;
  uint32_t captureMs = 0, decodeMs = 0, workerStackFree = 0;
  unsigned whitePixels = 0;
  TransportSessionEpoch epoch = 0;
  char error[128]{};
};
static portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
static View view;
EXT_RAM_BSS_ATTR static uint8_t front[kBytes];
EXT_RAM_BSS_ATTR static uint8_t painting[kBytes]; // OLED owner only
struct Scratch { uint8_t shades[kWidth * kHeight], bits[kBytes]; };
static char lastError[128]{}; // OLED-owner only
static uint32_t retryAt = 0;
struct PhotoTicket { uint32_t generation; bool frozen; };
static PhotoTicket* photoPending = nullptr; // Owned by the OLED task until submitted.
static bool photoInFlight = false;

static View snapshot() {
  portENTER_CRITICAL(&mutex);
  const View result = view;
  portEXIT_CRITICAL(&mutex);
  return result;
}
static bool current(const View& ticket) {
  return view.active && view.generation == ticket.generation && view.epoch == ticket.epoch;
}
static bool publish(const View& ticket, const char* error, bool running, bool busy,
                    const uint8_t* bits = nullptr, const camera_mono::Image* image = nullptr,
                    uint32_t captureMs = 0, uint32_t decodeMs = 0) {
  if (!transportSessionEpochIsLive(SOURCE_LOCAL_DISPLAY, ticket.epoch)) return false;
  const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  bool accepted = false;
  portENTER_CRITICAL(&mutex);
  if (current(ticket) && view.revision == ticket.revision && (!bits || !view.frozen)) {
    view.running = running;
    view.busy = busy;
    snprintf(view.error, sizeof(view.error), "%s", error ? error : "");
    if (!running) {
      view.ready = false; view.frozen = false; view.whitePixels = 0;
      memset(front, 0, sizeof(front));
    }
    if (bits) {
      memcpy(front, bits, sizeof(front));
      view.ready = true;
      ++view.sequence;
      view.updatedMs = millis();
      view.whitePixels = image->whitePixels;
      view.captureMs = captureMs;
      view.decodeMs = decodeMs;
    }
    view.workerStackFree = stackFree;
    accepted = true;
  }
  portEXIT_CRITICAL(&mutex);
  if (accepted) oledMarkDirty();
  return accepted;
}

static void worker(void*) {
  auto* scratch = static_cast<Scratch*>(ps_alloc(sizeof(Scratch), AllocPref::PreferPSRAM, "oled.camera.scratch"));
  camera_mono::Tone tone;
  uint32_t toneGeneration = 0, toneRevision = 0;
  bool streaming = false;
  for (;;) {
    const auto ticket = snapshot();
    if (!ticket.active || !transportSessionEpochIsLive(SOURCE_LOCAL_DISPLAY, ticket.epoch)) break;
    CameraStateSnapshot camera;
    const bool known = cameraStateSnapshot(camera);
    const bool wantStream = known && camera.running && !ticket.frozen;
    if (wantStream && !streaming) { cameraSessionBegin(CameraSessionKind::Stream); streaming = true; }
    if (!wantStream && streaming) { cameraSessionEnd(CameraSessionKind::Stream); streaming = false; }
    if (!scratch) {
      publish(ticket, "Preview needs more memory. Stop unused tasks and reopen Camera.", known && camera.running, false);
      break;
    }
    if (!known) publish(ticket, "Camera busy. Waiting for the current operation.", ticket.running, true);
    else if (!camera.running) publish(ticket, camera.error, false, false);
    else if (!ticket.frozen) {
      if (!ticket.running) publish(ticket, nullptr, true, false);
      portENTER_CRITICAL(&mutex);
      const bool admitted = current(ticket) && view.revision == ticket.revision && !view.frozen;
      if (admitted) view.working = true;
      portEXIT_CRITICAL(&mutex);
      if (!admitted) continue;
      CameraFrame frame;
      const uint32_t captureAt = millis();
      if (!captureCameraFrame(frame)) {
        CameraStateSnapshot failed;
        const bool readable = cameraStateSnapshot(failed);
        publish(ticket, readable && failed.error[0] ? failed.error :
                       "No preview frame received. Retry; check cameraread.",
                !readable || failed.running, !readable);
      } else {
        const uint32_t decodeAt = millis();
        hwjpeg::Image decoded;
        hwjpeg::DecodeOptions options;
        options.maxInputBytes = kCameraMaxFrameBytes;
        options.maxOutputBytes = 1600u * 1600u * 3u;
        const char* error = nullptr;
        const bool decodedOk = hwjpeg::decode(frame.data, frame.length, decoded, options, &error);
        cameraFrameRelease(frame);
        if (!decodedOk) {
          char message[128];
          snprintf(message, sizeof(message), "Preview decode: %s. Try a smaller camera resolution.", error ? error : "no image");
          publish(ticket, message, true, false);
        } else {
          if (toneGeneration != ticket.generation || toneRevision != ticket.revision) {
            tone = {}; toneGeneration = ticket.generation; toneRevision = ticket.revision;
          }
          camera_mono::Image image;
          if (camera_mono::render(decoded.pixels, decoded.width, decoded.height, decoded.stride, decoded.size,
                                  scratch->shades, sizeof(scratch->shades), scratch->bits, sizeof(scratch->bits),
                                  kWidth, kHeight, tone, ticket.contrast, ticket.dither, image)) {
            publish(ticket, nullptr, true, false, scratch->bits, &image, decodeAt - captureAt, millis() - decodeAt);
          } else publish(ticket, "Preview image layout is unsupported. Check cameraread.", true, false);
        }
      }
      portENTER_CRITICAL(&mutex);
      view.working = false;
      portEXIT_CRITICAL(&mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
  }
  if (streaming) cameraSessionEnd(CameraSessionKind::Stream);
  free(scratch);
  portENTER_CRITICAL(&mutex);
  view.worker = false;
  portEXIT_CRITICAL(&mutex);
  oledMarkDirty();
  vTaskDelete(nullptr);
}

static void photoDone(bool ok, const void* context) {
  photoInFlight = false;
  const auto* ticket = static_cast<const PhotoTicket*>(context);
  portENTER_CRITICAL(&mutex);
  if (view.active && view.generation == ticket->generation) {
    view.frozen = ticket->frozen;
    ++view.revision;
  }
  portEXIT_CRITICAL(&mutex);
  delete ticket;
  if (ok) oledToastShow("Color photo saved", 2000);
}
static bool input(int, int, uint32_t buttons) {
  const auto state = snapshot();
  if (photoPending || photoInFlight) {
    if (INPUT_CHECK(buttons, INPUT_BUTTON_B)) return false;
    if (buttons || gNavEvents.up || gNavEvents.down || gNavEvents.left || gNavEvents.right) {
      oledToastShow("Photo finishing", 1500);
      return true;
    }
  }
  if (INPUT_CHECK(buttons, INPUT_BUTTON_X)) {
    oledRunCommandAsync(state.running ? "closecamera" : "opencamera", nullptr, nullptr);
    return true;
  }
  if (INPUT_CHECK(buttons, INPUT_BUTTON_Y)) {
    // A new full-color photo uses the ordinary authorized camera command.
    // Holding the preview freezes only the displayed bitmap.
    if (photoPending) { oledToastShow("Photo pending", 1500); return true; }
    photoPending = new(std::nothrow) PhotoTicket{state.generation, state.frozen};
    if (!photoPending) { oledToastShow("Photo request needs memory", 2000); return true; }
    portENTER_CRITICAL(&mutex);
    view.frozen = true;
    ++view.revision;
    portEXIT_CRITICAL(&mutex);
    return true;
  }
  if (INPUT_CHECK(buttons, INPUT_BUTTON_A)) {
    if (!state.ready) { oledToastShow("Waiting for a camera frame", 1500); return true; }
    portENTER_CRITICAL(&mutex);
    view.frozen = !view.frozen;
    ++view.revision;
    portEXIT_CRITICAL(&mutex);
    oledMarkDirty();
    return true;
  }
  const int dx = gNavEvents.left ? -1 : gNavEvents.right ? 1 : 0;
  const int dy = gNavEvents.up ? -1 : gNavEvents.down ? 1 : 0;
  if (dx || dy) {
    if (state.frozen) { oledToastShow("Resume preview to adjust", 1500); return true; }
    portENTER_CRITICAL(&mutex);
    if (dx) view.dither = dx > 0;
    if (dy) view.contrast = std::max(-2, std::min(2, view.contrast - dy));
    ++view.revision;
    portEXIT_CRITICAL(&mutex);
    oledMarkDirty();
    return true;
  }
  return false; // B keeps the standard back navigation.
}
static void draw() {
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  portENTER_CRITICAL(&mutex);
  const auto state = view;
  if (state.ready) memcpy(painting, front, sizeof(painting));
  portEXIT_CRITICAL(&mutex);
  if (state.ready) oledDisplay->drawBitmap(0, OLED_CONTENT_START_Y + 9, painting, kWidth, kHeight, DISPLAY_COLOR_WHITE);
  if (state.ready) {
    char status[24];
    snprintf(status, sizeof(status), "%s %s C%+d X:Off", state.frozen ? "Held" : "Live", state.dither ? "Dither" : "Mono", state.contrast);
    oledDisplay->print(status);
  } else {
    oledDisplay->println(state.running ? "Waiting for frame" : "Camera stopped");
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y + 12);
    oledDisplay->println(state.error[0] ? state.error : "X: Start camera");
  }
}
static const OLEDModeEntry modes[] = {
  {OLED_CAMERA, "Camera", "notify_sensor", draw, nullptr, input, true, 21, "A:Hold Y:Photo B:Back"}
};
REGISTER_OLED_MODE_MODULE(modes, 1, "Camera");
} // namespace

void resetOLEDCameraPreview() {
  portENTER_CRITICAL(&mutex);
  const bool workerRunning = view.worker;
  const bool workInFlight = view.working;
  const uint32_t next = view.generation + 1;
  view = {};
  view.worker = workerRunning;
  view.working = workInFlight;
  view.generation = next ? next : 1;
  memset(front, 0, sizeof(front));
  memset(painting, 0, sizeof(painting));
  portEXIT_CRITICAL(&mutex);
  lastError[0] = 0;
  delete photoPending;
  photoPending = nullptr;
}

void prepareCameraPreviewData() {
  const bool here = currentOLEDMode == OLED_CAMERA && gOledRunning && oledConnected &&
                    displayIsReady() && powerLocalDisplayAwake() && !oledIsGuestSession();
  auto state = snapshot();
  if (!here) {
    if (state.active || state.ready) resetOLEDCameraPreview();
    return;
  }
  String user;
  bool authed = false;
  const auto epoch = localDisplayTransportSessionSnapshot(user, authed);
  if (!authed || !epoch) { if (state.active) resetOLEDCameraPreview(); return; }
  if (state.active && state.epoch != epoch) { resetOLEDCameraPreview(); state = snapshot(); }
  portENTER_CRITICAL(&mutex);
  view.active = true;
  view.epoch = epoch;
  portEXIT_CRITICAL(&mutex);
  if (photoPending && !state.working) {
    auto* ticket = photoPending;
    photoPending = nullptr;
    photoInFlight = true;
    if (!oledRunCommandAsync("camerasave", photoDone, ticket)) photoDone(false, ticket);
  }
  if (state.error[0] && !state.busy && strcmp(lastError, state.error)) {
    snprintf(lastError, sizeof(lastError), "%s", state.error);
    oledToastShow(state.error, oled_message::kErrorDurationMs, OledUIIcon::ERROR);
  }
  if (!state.error[0]) lastError[0] = 0;
  if (state.worker || int32_t(millis() - retryAt) < 0) return;
  retryAt = millis() + 3000;
  portENTER_CRITICAL(&mutex);
  view.worker = true;
  portEXIT_CRITICAL(&mutex);
  constexpr uint32_t stack = 10240;
  taskStackRecord("oled_camera", stack);
  if (xTaskCreatePinnedToCore(worker, "oled_camera", stack, nullptr, TASK_PRIORITY_LOW, nullptr, APP_CORE) != pdPASS) {
    portENTER_CRITICAL(&mutex);
    view.worker = false;
    snprintf(view.error, sizeof(view.error), "Preview task needs memory. Stop unused tasks and retry.");
    portEXIT_CRITICAL(&mutex);
    oledMarkDirty();
  }
}

const char* cmd_oledcameraread(const String&) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const auto state = snapshot();
  // Fixed metadata only; cached pixels stay private to the display session.
  // Command handlers run on cmd_exec and share its existing reply buffer.
  // A thread-local array would reserve this space in every RTOS task.
  if (!ensureDebugBuffer()) return "Error: Preview status needs memory. Stop unused tasks and retry.";
  char* result = getDebugBuffer();
  JsonDocument doc;
  doc["active"] = state.active;
  doc["workerActive"] = state.worker;
  doc["working"] = state.working;
  doc["ready"] = state.ready;
  doc["frozen"] = state.frozen;
  doc["dither"] = state.dither;
  doc["contrast"] = state.contrast;
  doc["frames"] = state.sequence;
  doc["width"] = kWidth;
  doc["height"] = kHeight;
  doc["whitePixels"] = state.whitePixels;
  doc["captureMs"] = state.captureMs;
  doc["decodeMs"] = state.decodeMs;
  doc["workerStackFreeBytes"] = state.workerStackFree;
  doc["busy"] = state.busy;
  doc["error"] = state.error;
  if (doc.overflowed()) return "Error: Preview status needs memory. Stop unused tasks and retry.";
  if (measureJson(doc) >= GLOBAL_DEBUG_BUFFER_SIZE)
    return "Error: Preview status response exceeded its limit.";
  serializeJson(doc, result, GLOBAL_DEBUG_BUFFER_SIZE);
  return result;
}
#endif
