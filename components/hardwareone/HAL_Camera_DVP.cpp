#include "System_BuildConfig.h"
#if ENABLE_CAMERA_SENSOR && !defined(CONFIG_IDF_TARGET_ESP32P4)
#include "HAL_Camera.h"
#include "HAL_JPEG.h"
#include "esp_camera.h"
#include "driver/i2c_master.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include <Arduino.h>
#include <string.h>

namespace {
bool sRunning = false;
bool sDetected = false;
CameraFrameSize sResolution = CameraFrameSize::VGA;
int sQuality = 12;
const char* sModel = "Unknown";
const char* sError = nullptr;

framesize_t nativeSize(CameraFrameSize size) {
  static const framesize_t map[] = {
    FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XGA,
    FRAMESIZE_SXGA, FRAMESIZE_UXGA, FRAMESIZE_96X96, FRAMESIZE_QQVGA,
    FRAMESIZE_QCIF, FRAMESIZE_HQVGA, FRAMESIZE_240X240, FRAMESIZE_HD, FRAMESIZE_CIF
  };
  return map[unsigned(size)];
}
using Setter = int (*)(sensor_t*, int);
Setter setter(sensor_t* s, CameraControl c) {
  if (!s) return nullptr;
  switch (c) {
    case CameraControl::Brightness: return s->set_brightness;
    case CameraControl::Contrast: return s->set_contrast;
    case CameraControl::Saturation: return s->set_saturation;
    case CameraControl::Sharpness: return s->set_sharpness;
    case CameraControl::Denoise: return s->set_denoise;
    case CameraControl::WhiteBalanceMode: return s->set_wb_mode;
    case CameraControl::Effect: return s->set_special_effect;
    case CameraControl::HMirror: return s->set_hmirror;
    case CameraControl::VFlip: return s->set_vflip;
    case CameraControl::ExposureLevel: return s->set_ae_level;
    case CameraControl::AutoExposure: return s->set_exposure_ctrl;
    case CameraControl::ExposureValue: return s->set_aec_value;
    case CameraControl::AutoGain: return s->set_gain_ctrl;
    case CameraControl::Gain: return s->set_agc_gain;
    case CameraControl::WhiteBalance: return s->set_whitebal;
    case CameraControl::WhiteBalanceGain: return s->set_awb_gain;
    case CameraControl::NightMode: return s->set_aec2;
    case CameraControl::Downsize: return s->set_dcw;
    case CameraControl::BlackPixelCorrection: return s->set_bpc;
    case CameraControl::WhitePixelCorrection: return s->set_wpc;
    case CameraControl::Gamma: return s->set_raw_gma;
    case CameraControl::LensCorrection: return s->set_lenc;
    case CameraControl::ColorBar: return s->set_colorbar;
    default: return nullptr;
  }
}
CameraCapabilities capabilities() {
  // Retain the established VGA/PSRAM memory-pressure guard, and reject 96x96:
  // its esp32-camera JPEG framebuffer estimate can cause an FB-OVF boot loop.
  uint32_t sizes = cameraResolutionBit(CameraFrameSize::QVGA) |
    cameraResolutionBit(CameraFrameSize::QQVGA) | cameraResolutionBit(CameraFrameSize::QCIF) |
    cameraResolutionBit(CameraFrameSize::HQVGA) | cameraResolutionBit(CameraFrameSize::Square240);
  if (psramFound()) sizes |= cameraResolutionBit(CameraFrameSize::VGA) | cameraResolutionBit(CameraFrameSize::CIF);
  uint64_t controls = 0;
  sensor_t* s = sRunning ? esp_camera_sensor_get() : nullptr;
  for (unsigned i = 0; i < unsigned(CameraControl::Count); ++i) {
    const auto c = CameraControl(i);
    if ((!s && c != CameraControl::Sharpness && c != CameraControl::Denoise) ||
        (s && (setter(s, c) || (c == CameraControl::GainCeiling && s->set_gainceiling))))
      controls |= cameraControlBit(c);
  }
  return {"dvp", sizes, controls, false, s ? s->set_reg != nullptr : true, 0, 0};
}
CameraBackendInfo info() {
  const auto* size = cameraFrameSizeInfo(sResolution);
  return {sModel, sDetected, sResolution, sQuality,
          uint16_t(size ? size->width : 0), uint16_t(size ? size->height : 0), sError};
}
bool setControl(CameraControl c, int value) {
  sensor_t* s = sRunning ? esp_camera_sensor_get() : nullptr;
  if (!s) return false;
  int rc = -1;
  if (c == CameraControl::GainCeiling && s->set_gainceiling)
    rc = s->set_gainceiling(s, gainceiling_t(value));
  else if (auto fn = setter(s, c)) rc = fn(s, value);
  if (rc != 0) { sError = "Sensor control unsupported or failed"; return false; }
  sError = nullptr;
  return true;
}
bool getControl(CameraControl c, int& value) {
  sensor_t* s = sRunning ? esp_camera_sensor_get() : nullptr;
  if (!s || !(capabilities().controls & cameraControlBit(c))) return false;
  const auto& st = s->status;
  switch (c) {
    case CameraControl::Brightness: value = st.brightness; break;
    case CameraControl::Contrast: value = st.contrast; break;
    case CameraControl::Saturation: value = st.saturation; break;
    case CameraControl::Sharpness: value = st.sharpness; break;
    case CameraControl::Denoise: value = st.denoise; break;
    case CameraControl::WhiteBalanceMode: value = st.wb_mode; break;
    case CameraControl::Effect: value = st.special_effect; break;
    case CameraControl::HMirror: value = st.hmirror; break;
    case CameraControl::VFlip: value = st.vflip; break;
    case CameraControl::ExposureLevel: value = st.ae_level; break;
    case CameraControl::AutoExposure: value = st.aec; break;
    case CameraControl::ExposureValue: value = st.aec_value; break;
    case CameraControl::AutoGain: value = st.agc; break;
    case CameraControl::Gain: value = st.agc_gain; break;
    case CameraControl::GainCeiling: value = st.gainceiling; break;
    case CameraControl::WhiteBalance: value = st.awb; break;
    case CameraControl::WhiteBalanceGain: value = st.awb_gain; break;
    case CameraControl::NightMode: value = st.aec2; break;
    case CameraControl::Downsize: value = st.dcw; break;
    case CameraControl::BlackPixelCorrection: value = st.bpc; break;
    case CameraControl::WhitePixelCorrection: value = st.wpc; break;
    case CameraControl::Gamma: value = st.raw_gma; break;
    case CameraControl::LensCorrection: value = st.lenc; break;
    case CameraControl::ColorBar: value = st.colorbar; break;
    default: return false;
  }
  return true;
}
void flushFrames(unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    camera_fb_t* fb = esp_camera_fb_get();
    // One timeout already consumed the driver's four-second deadline. Do not
    // multiply that wait by the warmup frame count; setup can still apply its
    // controls and prove readiness with a real JPEG afterward.
    if (!fb) break;
    esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
bool end() {
  if (!sRunning) return true;
  if (esp_camera_deinit() != ESP_OK) { sError = "Camera deinitialization failed"; return false; }
  sRunning = false;
  return true;
}
bool setResolution(CameraFrameSize size) {
  if (!(capabilities().resolutions & cameraResolutionBit(size))) return false;
  sensor_t* s = sRunning ? esp_camera_sensor_get() : nullptr;
  if (!s) return false;
  if (s->set_framesize(s, nativeSize(size)) != 0) {
    (void)s->set_framesize(s, nativeSize(sResolution));
    sError = "Camera resolution change failed";
    return false;
  }
  sResolution = size;
  // Two queued frames can retain the previous geometry. Drain those before
  // returning; capture checks the JPEG SOF, not esp_camera's relabeled metadata.
  flushFrames(2);
  return true;
}
bool setQuality(int quality) {
  if (quality < 0 || quality > 63) return false;
  sensor_t* s = sRunning ? esp_camera_sensor_get() : nullptr;
  if (!s || s->set_quality(s, quality) != 0) return false;
  sQuality = quality;
  return true;
}
bool capture(CameraFrame& out, size_t maxBytes);

bool begin(const CameraConfig& requested) {
  if (sRunning) return true;
  if (!(capabilities().resolutions & cameraResolutionBit(requested.resolution)) ||
      requested.quality < 0 || requested.quality > 63) {
    sError = "Unsupported camera configuration";
    return false;
  }
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 15; config.pin_d1 = 17; config.pin_d2 = 18; config.pin_d3 = 16;
  config.pin_d4 = 14; config.pin_d5 = 12; config.pin_d6 = 11; config.pin_d7 = 48;
  config.pin_xclk = 10; config.pin_pclk = 13; config.pin_vsync = 38; config.pin_href = 47;
  config.pin_sccb_sda = 40; config.pin_sccb_scl = 39;
  config.pin_pwdn = -1; config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  // cam_hal allocates JPEG buffers only at init (normally width*height/5).
  // Allocate for the largest delivery size advertised by this backend, even
  // when booting at 240x240. A later live VGA request must not inherit a tiny
  // 11 KiB buffer. This works with the SDK's default automatic sizing; boards
  // may additionally select a custom 128 KiB JPEG buffer in sdkconfig.
  const CameraFrameSize allocationSize = psramFound() ? CameraFrameSize::VGA : CameraFrameSize::QVGA;
  config.frame_size = nativeSize(allocationSize);
  config.pixel_format = PIXFORMAT_JPEG;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  // Stabilize the initial maximum-size sensor frames at the established safe
  // quality before selecting the caller's delivery size and exact quality.
  config.jpeg_quality = requested.quality < 10 ? 10 : requested.quality;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  // esp32-camera owns SCCB probing. A preliminary Wire scan would steal the
  // shared I2C controller and is neither necessary nor evidence of capture.
  // SCCB-ng marks its bus as owned before attempting creation. On a conflict,
  // esp_camera_init's error path can otherwise delete another owner's bus.
  // Check the actual SDK-selected controller, which the shared I2C manager
  // also reserves while this camera backend is compiled in.
#if defined(CONFIG_SCCB_HARDWARE_I2C_PORT1) && CONFIG_SCCB_HARDWARE_I2C_PORT1
  constexpr i2c_port_num_t sccbPort = I2C_NUM_1;
#else
  constexpr i2c_port_num_t sccbPort = I2C_NUM_0;
#endif
  static_assert(int(sccbPort) == CAMERA_SCCB_I2C_PORT, "SCCB SDK port and shared I2C reservation must agree");
  i2c_master_bus_handle_t occupied = nullptr;
  if (i2c_master_get_bus_handle(sccbPort, &occupied) == ESP_OK && occupied) {
    sError = "Camera SCCB controller already owned by another device";
    return false;
  }
  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { sError = esp_err_to_name(err); return false; }
  sRunning = true;
  sensor_t* s = esp_camera_sensor_get();
  if (!s) { (void)end(); sError = "Camera sensor handle unavailable"; return false; }
  sDetected = true;
  switch (s->id.PID) {
    case OV2640_PID: sModel = "OV2640"; break;
    case OV3660_PID: sModel = "OV3660"; break;
    case OV5640_PID: sModel = "OV5640"; break;
    default: sModel = "Unknown"; break;
  }
  sResolution = allocationSize;
  sQuality = config.jpeg_quality;
  if (s->id.PID == OV3660_PID) vTaskDelay(pdMS_TO_TICKS(500));
  if (s->set_framesize(s, nativeSize(requested.resolution)) != 0 ||
      s->set_quality(s, requested.quality) != 0) {
    (void)end();
    sError = "Camera delivery configuration failed";
    return false;
  }
  sResolution = requested.resolution;
  sQuality = requested.quality;
  flushFrames(s->id.PID == OV3660_PID ? 5 : 3);
  // Preserve OV3660's required chained contrast → brightness → saturation
  // application order and the established automatic exposure/gain defaults.
  const CameraControl order[] = {
    CameraControl::Contrast, CameraControl::Brightness, CameraControl::Saturation,
    CameraControl::HMirror, CameraControl::VFlip, CameraControl::Effect,
    CameraControl::WhiteBalance, CameraControl::WhiteBalanceGain, CameraControl::WhiteBalanceMode,
    CameraControl::Sharpness, CameraControl::Denoise, CameraControl::AutoExposure,
    CameraControl::NightMode, CameraControl::ExposureLevel, CameraControl::AutoGain,
    CameraControl::Gain, CameraControl::GainCeiling, CameraControl::BlackPixelCorrection,
    CameraControl::WhitePixelCorrection, CameraControl::Gamma, CameraControl::LensCorrection,
    CameraControl::Downsize, CameraControl::ColorBar
  };
  bool controlsOk = true;
  for (auto c : order) {
    if (!(capabilities().controls & cameraControlBit(c))) continue;
    if (!setControl(c, requested.controls[unsigned(c)])) {
      controlsOk = false;
      INFO_CAMERAF("DVP control not applied: %s", cameraControlName(c));
    }
  }
  if (s->id.PID == OV3660_PID) {
    vTaskDelay(pdMS_TO_TICKS(100));
    flushFrames(3);
  }
  // A successful sensor probe is not a working image pipeline. Require a
  // bounded JPEG with the requested SOF geometry before publishing readiness.
  // Early flush timeouts are allowed to recover after the controls above.
  CameraFrame readyFrame;
  if (!capture(readyFrame, kCameraMaxFrameBytes)) {
    const char* captureError = sError;
    cameraFrameRelease(readyFrame);
    const bool stopped = end();
    if (stopped) sError = captureError;
    return false;
  }
  cameraFrameRelease(readyFrame);
  sError = controlsOk ? nullptr : "Some sensor controls were not applied";
  return true;
}
bool capture(CameraFrame& out, size_t maxBytes) {
  out = {};
  if (!sRunning || maxBytes < 4) return false;
  const auto* size = cameraFrameSizeInfo(sResolution);
  sError = nullptr;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { sError = "Camera frame timeout"; return false; }
    hwjpeg::Info jpeg;
    hwjpeg::DecodeOptions limits;
    limits.maxWidth = 640; limits.maxHeight = 480;
    limits.maxOutputBytes = 640 * 480 * 3;
    limits.maxInputBytes = maxBytes;
    const bool valid = fb->format == PIXFORMAT_JPEG && fb->buf && fb->len >= 4 &&
      fb->len <= maxBytes && hwjpeg::inspect(fb->buf, fb->len, jpeg, limits);
    // esp_camera_fb_get overwrites fb width/height from current sensor status,
    // even for an older queued JPEG. Only its bounded SOF parse proves geometry.
    if (valid && (jpeg.width != size->width || jpeg.height != size->height)) {
      esp_camera_fb_return(fb);
      continue;
    }
    const char* entropyError = nullptr;
    hwjpeg::Info validated;
    if (!valid || !hwjpeg::validateSoftware(fb->buf, fb->len, validated, limits, &entropyError)) {
      esp_camera_fb_return(fb);
      sError = entropyError ? entropyError : "Invalid or oversized JPEG frame";
      INFO_CAMERAF("Discarding invalid JPEG (%u/3): %s", attempt + 1, sError);
      continue;
    }
    if (valid) {
      out.data = static_cast<uint8_t*>(ps_alloc(fb->len, AllocPref::PreferPSRAM, "camera.frame"));
      if (out.data) {
        memcpy(out.data, fb->buf, fb->len);
        out.length = fb->len; out.width = jpeg.width; out.height = jpeg.height;
      }
    }
    esp_camera_fb_return(fb);
    sError = out.data ? nullptr : "Invalid, oversized, or unallocatable JPEG frame";
    return out.data != nullptr;
  }
  if (!sError) sError = "Camera returned stale frame dimensions";
  return false;
}
bool writeRegister(unsigned addr, unsigned mask, unsigned value) {
  sensor_t* s = sRunning ? esp_camera_sensor_get() : nullptr;
  return s && s->set_reg && s->set_reg(s, int(addr), int(mask), int(value)) == 0;
}
bool dump() {
  sensor_t* s = sRunning ? esp_camera_sensor_get() : nullptr;
  if (!s) return false;
  BROADCAST_PRINTF("[CAM_DUMP] PID=0x%04X framesize=%u quality=%u", unsigned(s->id.PID), unsigned(s->status.framesize), unsigned(s->status.quality));
  for (unsigned i = 0; i < unsigned(CameraControl::Count); ++i) {
    int value;
    if (getControl(CameraControl(i), value)) BROADCAST_PRINTF("[CAM_DUMP] %s=%d", cameraControlName(CameraControl(i)), value);
  }
  return true;
}
}  // namespace

const CameraBackend& cameraDvpBackend() {
  static const CameraBackend backend{capabilities, info, begin, end, capture,
    setResolution, setQuality, setControl, getControl, writeRegister, dump};
  return backend;
}
#endif
