#include "OLED_Mode_Camera.h"
#if ENABLE_OLED_DISPLAY && ENABLE_CAMERA_SENSOR
#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "OLED_UI.h"
#include "OLED_MessageText.h"
#include "HAL_Input.h"
#include "HAL_JPEG.h"
#include "OLED_CameraMonochrome.h"
#include "System_Camera_DVP.h"
#include "System_User.h"
#include "System_Power.h"
#include "System_TaskUtils.h"
#include "System_MemUtil.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Deadline.h" // deadlinePending: the worker retry timer
#include "System_Utils.h" // Command validation context
#include <ArduinoJson.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <new>

namespace {
// The preview raster keeps the landscape content size (the panel's width,
// the landscape content rows less the status row); portrait shows it at half
// scale across the 64 px width.
constexpr unsigned kWidth = DISPLAY_WIDTH;
constexpr unsigned kHeight =
    DISPLAY_HEIGHT - oled_layout::kHeaderHeight - oled_layout::kFooterHeight - oled_layout::kContentGap - 9;
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
static portMUX_TYPE sMutex = portMUX_INITIALIZER_UNLOCKED;
static View sView;
EXT_RAM_BSS_ATTR static uint8_t front[kBytes];
EXT_RAM_BSS_ATTR static uint8_t painting[kBytes]; // OLED owner only
struct Scratch { uint8_t shades[kWidth * kHeight], bits[kBytes]; };
static char sLastError[128]{}; // OLED-owner only
static uint32_t sRetryAt = 0;
struct PhotoTicket { uint32_t generation; bool frozen; };
static PhotoTicket* sPhotoPending = nullptr; // Owned by the OLED task until submitted.
static bool sPhotoInFlight = false;

static View snapshot() {
  portENTER_CRITICAL(&sMutex);
  const View result = sView;
  portEXIT_CRITICAL(&sMutex);
  return result;
}
static bool current(const View& ticket) {
  return sView.active && sView.generation == ticket.generation && sView.epoch == ticket.epoch;
}
static bool publish(const View& ticket, const char* error, bool running, bool busy,
                    const uint8_t* bits = nullptr, const camera_mono::Image* image = nullptr,
                    uint32_t captureMs = 0, uint32_t decodeMs = 0) {
  if (!transportSessionEpochIsLive(SOURCE_LOCAL_DISPLAY, ticket.epoch)) return false;
  const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  bool accepted = false;
  portENTER_CRITICAL(&sMutex);
  if (current(ticket) && sView.revision == ticket.revision && (!bits || !sView.frozen)) {
    sView.running = running;
    sView.busy = busy;
    snprintf(sView.error, sizeof(sView.error), "%s", error ? error : "");
    if (!running) {
      sView.ready = false; sView.frozen = false; sView.whitePixels = 0;
      memset(front, 0, sizeof(front));
    }
    if (bits) {
      memcpy(front, bits, sizeof(front));
      sView.ready = true;
      ++sView.sequence;
      sView.updatedMs = millis();
      sView.whitePixels = image->whitePixels;
      sView.captureMs = captureMs;
      sView.decodeMs = decodeMs;
    }
    sView.workerStackFree = stackFree;
    accepted = true;
  }
  portEXIT_CRITICAL(&sMutex);
  if (accepted) oledMarkDirty();
  return accepted;
}

static void worker(void*) {
  auto* scratch = static_cast<Scratch*>(ps_alloc(sizeof(Scratch), AllocPref::PreferPsram, "oled.camera.scratch"));
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
      portENTER_CRITICAL(&sMutex);
      const bool admitted = current(ticket) && sView.revision == ticket.revision && !sView.frozen;
      if (admitted) sView.working = true;
      portEXIT_CRITICAL(&sMutex);
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
        // The software decoder's workspace, a marker-normalized copy and the
        // pixels in PSRAM first, as the camera HAL and the glasses viewer ask;
        // without an allocator the workspace and the copy are internal RAM.
        options.softwareAllocator = [](size_t size) -> uint8_t* {
          return static_cast<uint8_t*>(ps_alloc(size, AllocPref::PreferPsram, "oled.camera.jpeg"));
        };
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
      portENTER_CRITICAL(&sMutex);
      sView.working = false;
      portEXIT_CRITICAL(&sMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
  }
  if (streaming) cameraSessionEnd(CameraSessionKind::Stream);
  free(scratch);
  portENTER_CRITICAL(&sMutex);
  sView.worker = false;
  portEXIT_CRITICAL(&sMutex);
  oledMarkDirty();
  vTaskDelete(nullptr);
}

static void photoDone(bool ok, const void* context) {
  sPhotoInFlight = false;
  const auto* ticket = static_cast<const PhotoTicket*>(context);
  portENTER_CRITICAL(&sMutex);
  if (sView.active && sView.generation == ticket->generation) {
    sView.frozen = ticket->frozen;
    ++sView.revision;
  }
  portEXIT_CRITICAL(&sMutex);
  delete ticket;
  if (ok) oledToastShow("Color photo saved", 2000);
}
static bool input(int, int, uint32_t buttons) {
  const auto state = snapshot();
  if (sPhotoPending || sPhotoInFlight) {
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
    if (sPhotoPending) { oledToastShow("Photo pending", 1500); return true; }
    sPhotoPending = new(std::nothrow) PhotoTicket{state.generation, state.frozen};
    if (!sPhotoPending) { oledToastShow("Photo request needs memory", oled_message::kErrorDurationMs, OledUiIcon::ERROR); return true; }
    portENTER_CRITICAL(&sMutex);
    sView.frozen = true;
    ++sView.revision;
    portEXIT_CRITICAL(&sMutex);
    return true;
  }
  if (INPUT_CHECK(buttons, INPUT_BUTTON_A)) {
    if (!state.ready) { oledToastShow("Waiting for a camera frame", 1500); return true; }
    portENTER_CRITICAL(&sMutex);
    sView.frozen = !sView.frozen;
    ++sView.revision;
    portEXIT_CRITICAL(&sMutex);
    oledMarkDirty();
    return true;
  }
  const int dx = gNavEvents.left ? -1 : gNavEvents.right ? 1 : 0;
  const int dy = gNavEvents.up ? -1 : gNavEvents.down ? 1 : 0;
  if (dx || dy) {
    if (state.frozen) { oledToastShow("Resume preview to adjust", 1500); return true; }
    portENTER_CRITICAL(&sMutex);
    if (dx) sView.dither = dx > 0;
    if (dy) sView.contrast = std::max(-2, std::min(2, sView.contrast - dy));
    ++sView.revision;
    portEXIT_CRITICAL(&sMutex);
    oledMarkDirty();
    return true;
  }
  return false; // B keeps the standard back navigation.
}
static void draw() {
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  portENTER_CRITICAL(&sMutex);
  const auto state = sView;
  if (state.ready) memcpy(painting, front, sizeof(painting));
  portEXIT_CRITICAL(&sMutex);
  if (state.ready && oledPortrait()) {
    // Half scale: one dot per 2 x 2 block (lit when any of the four is).
    const int top = OLED_CONTENT_START_Y + 18;
    const unsigned rowBytes = (kWidth + 7) / 8;
    auto lit = [&](unsigned x, unsigned y) {
      return x < kWidth && y < kHeight && (painting[y * rowBytes + x / 8] & (0x80u >> (x & 7)));
    };
    for (unsigned y = 0; y < kHeight; y += 2)
      for (unsigned x = 0; x < kWidth; x += 2)
        if (lit(x, y) || lit(x + 1, y) || lit(x, y + 1) || lit(x + 1, y + 1))
          oledDisplay->drawPixel((int)(x / 2), top + (int)(y / 2), DISPLAY_COLOR_WHITE);
  } else if (state.ready) {
    oledDisplay->drawBitmap(0, OLED_CONTENT_START_Y + 9, painting, kWidth, kHeight, DISPLAY_COLOR_WHITE);
  }
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
static const OledModeEntry kCameraModes[] = {
  {OLED_CAMERA, "Camera", "notify_sensor", draw, nullptr, input, true, 21, "A:Hold Y:Photo B:Back"}
};
REGISTER_OLED_MODE_MODULE(kCameraModes, 1, "Camera");
} // namespace

void resetOledCameraPreview() {
  portENTER_CRITICAL(&sMutex);
  const bool workerRunning = sView.worker;
  const bool workInFlight = sView.working;
  const uint32_t next = sView.generation + 1;
  sView = {};
  sView.worker = workerRunning;
  sView.working = workInFlight;
  sView.generation = next ? next : 1;
  memset(front, 0, sizeof(front));
  memset(painting, 0, sizeof(painting));
  portEXIT_CRITICAL(&sMutex);
  sLastError[0] = 0;
  delete sPhotoPending;
  sPhotoPending = nullptr;
}

void prepareCameraPreviewData() {
  // Runs on every display update, also when the page is not shown. A passed
  // retry deadline is set back to 0 here: left in place, it would read as
  // pending again 24.9 to 49.7 days later and keep the preview worker from
  // starting for that whole window.
  if (sRetryAt != 0 && !deadlinePending(sRetryAt, millis())) sRetryAt = 0;
  const bool here = currentOledMode == OLED_CAMERA && gOledRunning && oledConnected &&
                    displayIsReady() && powerLocalDisplayAwake() && !oledIsGuestSession();
  auto state = snapshot();
  if (!here) {
    if (state.active || state.ready) resetOledCameraPreview();
    return;
  }
  String user;
  bool authed = false;
  const auto epoch = localDisplayTransportSessionSnapshot(user, authed);
  if (!authed || !epoch) { if (state.active) resetOledCameraPreview(); return; }
  if (state.active && state.epoch != epoch) { resetOledCameraPreview(); state = snapshot(); }
  portENTER_CRITICAL(&sMutex);
  sView.active = true;
  sView.epoch = epoch;
  portEXIT_CRITICAL(&sMutex);
  if (sPhotoPending && !state.working) {
    auto* ticket = sPhotoPending;
    sPhotoPending = nullptr;
    sPhotoInFlight = true;
    if (!oledRunCommandAsync("camerasave", photoDone, ticket)) photoDone(false, ticket);
  }
  if (state.error[0] && !state.busy && strcmp(sLastError, state.error)) {
    snprintf(sLastError, sizeof(sLastError), "%s", state.error);
    oledToastShow(state.error, oled_message::kErrorDurationMs, OledUiIcon::ERROR);
  }
  if (!state.error[0]) sLastError[0] = 0;
  if (state.worker || deadlinePending(sRetryAt, millis())) return;  // sRetryAt 0: never tried
  sRetryAt = deadlineAfter(millis(), 3000);
  portENTER_CRITICAL(&sMutex);
  sView.worker = true;
  portEXIT_CRITICAL(&sMutex);
  constexpr uint32_t stack = 10240;
  taskStackRecord("oled_camera", stack);
  if (xTaskCreatePinnedToCore(worker, "oled_camera", stack, nullptr, TASK_PRIORITY_LOW, nullptr, APP_CORE) != pdPASS) {
    portENTER_CRITICAL(&sMutex);
    sView.worker = false;
    snprintf(sView.error, sizeof(sView.error), "Preview task needs memory. Stop unused tasks and retry.");
    portEXIT_CRITICAL(&sMutex);
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
  PSRAM_JSON_DOC(doc);
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
