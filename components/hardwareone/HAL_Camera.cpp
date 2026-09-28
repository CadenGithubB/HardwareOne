#include "HAL_Camera.h"
#include <stdlib.h>

static constexpr CameraFrameSizeInfo kSizes[] = {
  {CameraFrameSize::QVGA, "qvga", 320, 240},
  {CameraFrameSize::VGA, "vga", 640, 480},
  {CameraFrameSize::SVGA, "svga", 800, 600},
  {CameraFrameSize::XGA, "xga", 1024, 768},
  {CameraFrameSize::SXGA, "sxga", 1280, 1024},
  {CameraFrameSize::UXGA, "uxga", 1600, 1200},
  {CameraFrameSize::Square96, "96x96", 96, 96},
  {CameraFrameSize::QQVGA, "qqvga", 160, 120},
  {CameraFrameSize::QCIF, "qcif", 176, 144},
  {CameraFrameSize::HQVGA, "hqvga", 240, 176},
  {CameraFrameSize::Square240, "240x240", 240, 240},
  {CameraFrameSize::HD, "hd", 1280, 720},
  {CameraFrameSize::CIF, "cif", 400, 296},
};
static_assert(sizeof(kSizes) / sizeof(kSizes[0]) == unsigned(CameraFrameSize::Count), "size table");
const CameraFrameSizeInfo* cameraFrameSizeInfo(CameraFrameSize size) {
  return unsigned(size) < unsigned(CameraFrameSize::Count) ? &kSizes[unsigned(size)] : nullptr;
}
CameraFrameSize cameraFrameSizeFromSetting(int value) {
  return value >= 0 && value < int(CameraFrameSize::Count) ? CameraFrameSize(value) : CameraFrameSize::VGA;
}
const char* cameraControlName(CameraControl c) {
  static const char* names[] = {
    "brightness", "contrast", "saturation", "sharpness", "denoise", "wbMode",
    "effect", "hmirror", "vflip", "exposureLevel", "autoExposure", "exposureValue",
    "autoGain", "gain", "gainCeiling", "whiteBalance", "whiteBalanceGain", "nightMode",
    "downsize", "blackPixelCorrection", "whitePixelCorrection", "gamma",
    "lensCorrection", "colorBar"
  };
  return unsigned(c) < unsigned(CameraControl::Count) ? names[unsigned(c)] : "unknown";
}
void cameraFrameRelease(CameraFrame& frame) {
  free(frame.data);
  frame = {};
}
bool cameraFrameIsValid(const CameraFrame& frame, size_t maxBytes) {
  return frame.data && frame.length >= 4 && frame.length <= maxBytes &&
         frame.width && frame.height && frame.data[0] == 0xff && frame.data[1] == 0xd8 &&
         frame.data[frame.length - 2] == 0xff && frame.data[frame.length - 1] == 0xd9;
}

#ifndef HW_CAMERA_HAL_HOST_TEST
#include "System_BuildConfig.h"
#if ENABLE_CAMERA_SENSOR
const CameraBackend& cameraBackend() {
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4
  return cameraP4Backend();
#else
  return cameraDvpBackend();
#endif
}
#endif
#endif
