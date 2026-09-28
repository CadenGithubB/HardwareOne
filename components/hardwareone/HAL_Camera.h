#pragma once

#include <stddef.h>
#include <stdint.h>

// Values are persisted cameraFramesize IDs. Never renumber existing entries.
enum class CameraFrameSize : uint8_t {
  QVGA = 0, VGA, SVGA, XGA, SXGA, UXGA, Square96, QQVGA, QCIF,
  HQVGA, Square240, HD, CIF, Count
};
struct CameraFrameSizeInfo {
  CameraFrameSize id;
  const char* name;
  uint16_t width;
  uint16_t height;
};
const CameraFrameSizeInfo* cameraFrameSizeInfo(CameraFrameSize size);
CameraFrameSize cameraFrameSizeFromSetting(int value);

// Controls retain the existing CLI units; unsupported controls fail explicitly.
enum class CameraControl : uint8_t {
  Brightness, Contrast, Saturation, Sharpness, Denoise, WhiteBalanceMode,
  Effect, HMirror, VFlip, ExposureLevel, AutoExposure, ExposureValue,
  AutoGain, Gain, GainCeiling, WhiteBalance, WhiteBalanceGain, NightMode,
  Downsize, BlackPixelCorrection, WhitePixelCorrection, Gamma,
  LensCorrection, ColorBar, Count
};
const char* cameraControlName(CameraControl control);
constexpr uint64_t cameraControlBit(CameraControl c) {
  return unsigned(c) < unsigned(CameraControl::Count) ? uint64_t(1) << unsigned(c) : 0;
}
constexpr uint32_t cameraResolutionBit(CameraFrameSize s) {
  return unsigned(s) < unsigned(CameraFrameSize::Count) ? uint32_t(1) << unsigned(s) : 0;
}
constexpr size_t kCameraMaxFrameBytes = 128 * 1024;

struct CameraConfig {
  CameraFrameSize resolution = CameraFrameSize::VGA;
  int quality = 12;  // 0..63, lower is higher quality on every backend.
  int controls[unsigned(CameraControl::Count)] = {};
};
struct CameraFrame {
  // Owned allocation compatible with free(). No driver/DMA buffer escapes.
  uint8_t* data = nullptr;
  size_t length = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};
struct CameraCapabilities {
  const char* backend;
  uint32_t resolutions;
  uint64_t controls;
  bool hardwareJpeg;
  bool registerAccess;
  uint16_t sourceWidth;
  uint16_t sourceHeight;
};
struct CameraBackendInfo {
  const char* model;
  bool detected;
  CameraFrameSize resolution;
  int quality;
  uint16_t width;
  uint16_t height;
  const char* error;
};

// Driver contract. System_Camera_DVP serializes every call, including reads.
// begin failure must unwind acquired resources; end is idempotent. capture
// fails with an empty frame and never returns more than maxBytes. Resolution
// and quality setters must leave their previous configuration on failure.
struct CameraBackend {
  CameraCapabilities (*capabilities)();
  CameraBackendInfo (*info)();
  bool (*begin)(const CameraConfig&);
  bool (*end)();
  bool (*capture)(CameraFrame&, size_t maxBytes);
  bool (*setResolution)(CameraFrameSize);
  bool (*setQuality)(int);
  bool (*setControl)(CameraControl, int);
  bool (*getControl)(CameraControl, int&);
  bool (*writeRegister)(unsigned address, unsigned mask, unsigned value);
  bool (*dump)();
};
const CameraBackend& cameraBackend();
const CameraBackend& cameraDvpBackend();
const CameraBackend& cameraP4Backend();

// Pure validation helpers used at the shared capture boundary.
bool cameraFrameIsValid(const CameraFrame& frame, size_t maxBytes);
void cameraFrameRelease(CameraFrame& frame);
