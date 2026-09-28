#include "System_BuildConfig.h"

#if ENABLE_CAMERA_SENSOR && defined(CONFIG_IDF_TARGET_ESP32P4)
#if !defined(HW_BOARD_P4X_EYE) || !HW_BOARD_P4X_EYE
#error "The CSI camera wiring must be selected by an explicit supported board profile"
#endif

#include "HAL_Camera.h"
#include "CameraRgb565.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Calls are serialized by the camera service, not by individual driver locks.
// No shared application consumer sees CSI buffers or esp_video types.
namespace {
constexpr char kTag[] = "CameraP4";
constexpr uint32_t kWidth = 1280, kHeight = 720;
constexpr size_t kRawBytes = size_t(kWidth) * kHeight * 2;
constexpr unsigned kBufferCount = 3;
constexpr uint32_t kVideoFlags = ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP;
constexpr uint32_t kResolutionMask = ((1u << unsigned(CameraFrameSize::Count)) - 1) &
    ~(cameraResolutionBit(CameraFrameSize::XGA) | cameraResolutionBit(CameraFrameSize::SXGA) |
      cameraResolutionBit(CameraFrameSize::UXGA));
constexpr i2c_port_num_t kSccbPort = static_cast<i2c_port_num_t>(CAMERA_SCCB_I2C_PORT);
static_assert(CAMERA_SCCB_I2C_PORT == 0, "P4X-EYE camera owns I2C controller0");
constexpr uint64_t kControls = cameraControlBit(CameraControl::HMirror) |
                               cameraControlBit(CameraControl::VFlip);
// P4X-EYE MB v2.4: camera/LCD enable12, XCLK11, reset26, SDA14/SCL13.
// The common sensor manager's primary bus uses controller1. Controller0 is
// reserved for the on-board camera while this backend is enabled. We do not
// borrow an arbitrary Arduino Wire handle: its pins/lifetime are not leased.
constexpr gpio_num_t kPower = GPIO_NUM_12, kXclk = GPIO_NUM_11;
constexpr gpio_num_t kReset = GPIO_NUM_26, kSda = GPIO_NUM_14, kScl = GPIO_NUM_13;

struct State {
  i2c_master_bus_handle_t bus = nullptr;
  esp_cam_sensor_xclk_handle_t clock = nullptr;
  jpeg_encoder_handle_t encoder = nullptr;
  bool clockRunning = false;
  bool video = false;
  bool streaming = false;
  bool ready = false;
  bool detected = false;
  bool warm = false;
  int fd = -1;
  uint8_t* buffers[kBufferCount] = {};
  size_t lengths[kBufferCount] = {};
  uint8_t* resized = nullptr;
  size_t resizedCapacity = 0;
  CameraFrameSize resolution = CameraFrameSize::VGA;
  int quality = 12;
  bool mirror = false, flip = false;
  char model[40] = "ESP32-P4 CSI";
  char error[128] = {};
} state;

bool fail(const char* stage, esp_err_t err = ESP_FAIL) {
  snprintf(state.error, sizeof(state.error), "%s: %s", stage, esp_err_to_name(err));
  ESP_LOGW(kTag, "%s", state.error);
  return false;
}
bool io(int request, void* value, const char* stage) {
  if (ioctl(state.fd, request, value) == 0) return true;
  snprintf(state.error, sizeof(state.error), "%s: errno=%d", stage, errno);
  ESP_LOGW(kTag, "%s", state.error);
  return false;
}
CameraCapabilities capabilities() {
  return {"p4-csi", kResolutionMask, kControls, true, false, kWidth, kHeight};
}
CameraBackendInfo info() {
  const auto* size = cameraFrameSizeInfo(state.resolution);
  return {state.model, state.detected, state.resolution, state.quality,
          uint16_t(size ? size->width : 0), uint16_t(size ? size->height : 0),
          state.error[0] ? state.error : nullptr};
}

bool end() {
  state.ready = false;
  // If stopping DMA fails, retain the buffers, device and clock for a later
  // retry. Freeing them under a live CSI/ISP transfer would be unsafe.
  if (state.streaming) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (!io(VIDIOC_STREAMOFF, &type, "stop CSI stream")) return false;
    state.streaming = false;
  }
  if (state.fd >= 0) {
    // esp_video's mmap returns driver memory; close releases its stream
    // buffers. munmap is a documented no-op in the pinned driver.
    for (unsigned i = 0; i < kBufferCount; ++i) {
      if (state.buffers[i]) munmap(state.buffers[i], state.lengths[i]);
      state.buffers[i] = nullptr; state.lengths[i] = 0;
    }
    const int fd = state.fd;
    state.fd = -1; // POSIX close consumes the descriptor even on an error.
    if (close(fd) != 0) return fail("close CSI device");
  }
  if (state.video) {
    const esp_err_t err = esp_video_deinit_with_flags(kVideoFlags);
    if (err != ESP_OK) return fail("deinitialize CSI/ISP", err);
    state.video = false;
  }
  if (state.encoder) {
    const esp_err_t err = jpeg_del_encoder_engine(state.encoder);
    if (err != ESP_OK) return fail("delete JPEG encoder", err);
    state.encoder = nullptr;
  }
  free(state.resized); state.resized = nullptr; state.resizedCapacity = 0;
  if (state.clockRunning) {
    const esp_err_t err = esp_cam_sensor_xclk_stop(state.clock);
    if (err != ESP_OK) return fail("stop camera clock", err);
    state.clockRunning = false;
  }
  if (state.clock) {
    const esp_err_t err = esp_cam_sensor_xclk_free(state.clock);
    if (err != ESP_OK) return fail("free camera clock", err);
    state.clock = nullptr;
  }
  if (state.bus) {
    const esp_err_t err = i2c_del_master_bus(state.bus);
    if (err != ESP_OK) return fail("release camera SCCB bus", err);
    state.bus = nullptr;
  }
  // Never pull GPIO12 low or reset it: the optional LCD owns the same rail.
  // The display driver follows the same rule on panel stop/sleep/release.
  state.warm = false;
  state.error[0] = 0;
  return true;
}

bool initialize(const CameraConfig& config) {
  i2c_master_bus_handle_t occupied = nullptr;
  if (i2c_master_get_bus_handle(kSccbPort, &occupied) == ESP_OK && occupied) {
    return fail("camera requires free I2C0; another owner is active", ESP_ERR_INVALID_STATE);
  }
  i2c_master_bus_config_t bus = {};
  bus.i2c_port = kSccbPort; bus.sda_io_num = kSda; bus.scl_io_num = kScl;
  bus.clk_source = I2C_CLK_SRC_DEFAULT; bus.glitch_ignore_cnt = 7;
  bus.flags.enable_internal_pullup = true;
  esp_err_t err = i2c_new_master_bus(&bus, &state.bus);
  if (err != ESP_OK) return fail("acquire camera SCCB bus", err);

  // These operations only assert the shared rail. Failures do not power down
  // the display, and a headless board does not need LCD initialization.
  err = rtc_gpio_hold_dis(kPower);
  if (err == ESP_OK) err = rtc_gpio_deinit(kPower);
  if (err == ESP_OK) err = gpio_set_direction(kPower, GPIO_MODE_OUTPUT);
  if (err == ESP_OK) err = gpio_set_level(kPower, 1);
  if (err != ESP_OK) return fail("enable camera/LCD rail", err);
  vTaskDelay(pdMS_TO_TICKS(20));
  err = esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &state.clock);
  if (err != ESP_OK) return fail("allocate camera clock", err);
  esp_cam_sensor_xclk_config_t clock = {};
  clock.esp_clock_router_cfg.xclk_pin = kXclk;
  clock.esp_clock_router_cfg.xclk_freq_hz = 24000000;
  err = esp_cam_sensor_xclk_start(state.clock, &clock);
  if (err != ESP_OK) return fail("start camera clock", err);
  state.clockRunning = true;
  err = gpio_set_direction(kReset, GPIO_MODE_OUTPUT);
  if (err == ESP_OK) err = gpio_set_level(kReset, 1);
  if (err != ESP_OK) return fail("release camera reset", err);
  vTaskDelay(pdMS_TO_TICKS(20));

  esp_video_init_csi_config_t csi = {};
  csi.sccb_config.init_sccb = false;
  csi.sccb_config.i2c_handle = state.bus;
  csi.sccb_config.freq = 400000;
  csi.reset_pin = kReset; csi.pwdn_pin = GPIO_NUM_NC;
  // esp_video owns/relinquishes MIPI LDO3; card LDO4 is independent.
  csi.dont_init_ldo = false;
  esp_video_init_config_t video = {};
  video.csi = &csi;
  err = esp_video_init_with_flags(&video, kVideoFlags);
  if (err != ESP_OK) return fail("initialize CSI/ISP", err);
  state.video = true;
  state.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (state.fd < 0) return fail("open CSI device");
  esp_cam_sensor_id_t chip = {};
  v4l2_ext_control ctrl = {};
  ctrl.id = ESP_CAM_SENSOR_IOC_G_CHIP_ID;
  ctrl.p_u8 = reinterpret_cast<uint8_t*>(&chip); ctrl.size = sizeof(chip);
  v4l2_ext_controls ctrls = {};
  ctrls.ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL;
  ctrls.count = 1; ctrls.controls = &ctrl;
  if (!io(VIDIOC_G_EXT_CTRLS, &ctrls, "read camera identity")) return false;
  state.detected = true;
  snprintf(state.model, sizeof(state.model), "%s (CSI 0x%04x)",
           chip.pid == 0x2710 ? "OV2710" : "Sensor", chip.pid);
  esp_cam_sensor_format_t sensor = {};
  if (!io(VIDIOC_G_SENSOR_FMT, &sensor, "read native camera format")) return false;
  // Pinned OV2710 configuration is 720p RAW10/25fps. Refuse a differently
  // configured module instead of allocating unchecked sensor-sized buffers.
  if (sensor.width != kWidth || sensor.height != kHeight) {
    return fail("camera native mode must be 1280x720", ESP_ERR_NOT_SUPPORTED);
  }
  v4l2_format format = {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (!io(VIDIOC_G_FMT, &format, "read CSI output format")) return false;
  if (format.fmt.pix.width != kWidth || format.fmt.pix.height != kHeight) {
    return fail("unexpected CSI dimensions", ESP_ERR_INVALID_STATE);
  }
  // esp_video2.2 leaves optional stride/size at zero. Supply the checked
  // packed format; verify both returned layout and every dequeued buffer.
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  format.fmt.pix.bytesperline = kWidth * 2;
  format.fmt.pix.sizeimage = kRawBytes;
  if (!io(VIDIOC_S_FMT, &format, "select RGB565 output")) return false;
  if (format.fmt.pix.width != kWidth || format.fmt.pix.height != kHeight ||
      format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 ||
      format.fmt.pix.bytesperline != kWidth * 2 || format.fmt.pix.sizeimage != kRawBytes) {
    return fail("unsupported CSI output layout", ESP_ERR_NOT_SUPPORTED);
  }
  timeval timeout = {}; timeout.tv_sec = 2;
  if (!io(VIDIOC_S_DQBUF_TIMEOUT, &timeout, "set capture timeout")) return false;
  v4l2_requestbuffers req = {};
  req.count = kBufferCount; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
  if (!io(VIDIOC_REQBUFS, &req, "allocate CSI frames")) return false;
  if (req.count != kBufferCount) return fail("unexpected CSI buffer count");
  for (unsigned i = 0; i < kBufferCount; ++i) {
    v4l2_buffer buffer = {};
    buffer.index = i; buffer.type = req.type; buffer.memory = req.memory;
    if (!io(VIDIOC_QUERYBUF, &buffer, "query CSI buffer")) return false;
    if (buffer.length < kRawBytes || buffer.length > kRawBytes + 4096) {
      return fail("unexpected CSI buffer capacity");
    }
    auto* mapped = static_cast<uint8_t*>(mmap(nullptr, buffer.length,
        PROT_READ | PROT_WRITE, MAP_SHARED, state.fd, buffer.m.offset));
    if (!mapped || mapped == MAP_FAILED) return fail("map CSI buffer", ESP_ERR_NO_MEM);
    state.buffers[i] = mapped; state.lengths[i] = buffer.length;
    if (!io(VIDIOC_QBUF, &buffer, "queue CSI buffer")) return false;
  }
  jpeg_encode_engine_cfg_t jpeg = {}; jpeg.timeout_ms = 2000;
  err = jpeg_new_encoder_engine(&jpeg, &state.encoder);
  if (err != ESP_OK) return fail("allocate JPEG encoder", err);
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (!io(VIDIOC_STREAMON, &type, "start CSI stream")) return false;
  state.streaming = true; state.ready = true; state.warm = false;
  state.resolution = config.resolution; state.quality = config.quality;
  state.mirror = config.controls[unsigned(CameraControl::HMirror)] != 0;
  state.flip = config.controls[unsigned(CameraControl::VFlip)] != 0;
  state.error[0] = 0;
  ESP_LOGI(kTag, "%s, native %lux%lu, JPEG budget %u bytes", state.model,
           static_cast<unsigned long>(kWidth), static_cast<unsigned long>(kHeight),
           unsigned(kCameraMaxFrameBytes));
  return true;
}

bool begin(const CameraConfig& config) {
  if (!cameraFrameSizeInfo(config.resolution) ||
      !(kResolutionMask & cameraResolutionBit(config.resolution)) ||
      camera_rgb565::qualityToJpeg(config.quality) == 0) {
    return fail("invalid camera resolution or quality", ESP_ERR_INVALID_ARG);
  }
  if (!end()) return false;
  if (initialize(config)) return true;
  char original[sizeof(state.error)]; memcpy(original, state.error, sizeof(original));
  // end preserves live resources on teardown failure; the next start retries
  // teardown before allocating anything. Preserve the initial failure detail.
  const bool cleaned = end();
  if (cleaned) memcpy(state.error, original, sizeof(state.error));
  return false;
}

bool capture(CameraFrame& frame, size_t maxBytes) {
  cameraFrameRelease(frame);
  if (!state.ready || !state.streaming) return fail("camera is stopped", ESP_ERR_INVALID_STATE);
  const auto* size = cameraFrameSizeInfo(state.resolution);
  if (!size) return fail("invalid delivered resolution", ESP_ERR_INVALID_ARG);
  // Round down the capacity handed to DMA. Its allocation may round up, but
  // the driver can never emit more than the caller's accepted byte budget.
  size_t budget = maxBytes < kCameraMaxFrameBytes ? maxBytes : kCameraMaxFrameBytes;
  budget &= ~size_t(63);
  if (budget < 1024) return fail("JPEG budget too small", ESP_ERR_INVALID_ARG);
  const size_t rawBytes = size_t(size->width) * size->height * 2;
  if (state.resizedCapacity < rawBytes) {
    jpeg_encode_memory_alloc_cfg_t allocation = {};
    allocation.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    size_t capacity = 0;
    auto* replacement = static_cast<uint8_t*>(jpeg_alloc_encoder_mem(rawBytes, &allocation, &capacity));
    if (!replacement || capacity < rawBytes) {
      free(replacement); return fail("allocate delivered RGB frame", ESP_ERR_NO_MEM);
    }
    free(state.resized); state.resized = replacement; state.resizedCapacity = capacity;
  }
  // The ISP's AE/AWB needs settling on a cold start. On later captures flush
  // all three queued frames to avoid returning a scene cached while idle.
  const unsigned discard = state.warm ? kBufferCount : 50;
  const int64_t deadline = esp_timer_get_time() + 5000000;
  for (unsigned i = 0; i <= discard; ++i) {
    const int64_t left = deadline - esp_timer_get_time();
    if (left <= 0) return fail("capture deadline exceeded", ESP_ERR_TIMEOUT);
    const int64_t waitUs = left < 2000000 ? left : 2000000;
    timeval timeout = {};
    timeout.tv_sec = waitUs / 1000000; timeout.tv_usec = waitUs % 1000000;
    if (!io(VIDIOC_S_DQBUF_TIMEOUT, &timeout, "set remaining capture timeout")) return false;
    v4l2_buffer buffer = {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buffer.memory = V4L2_MEMORY_MMAP;
    if (!io(VIDIOC_DQBUF, &buffer, "dequeue CSI frame")) return false;
    bool valid = buffer.index < kBufferCount &&
        !(buffer.flags & V4L2_BUF_FLAG_ERROR) && buffer.bytesused == kRawBytes &&
        buffer.bytesused <= state.lengths[buffer.index] && state.buffers[buffer.index];
    if (valid && i == discard) {
      valid = camera_rgb565::resize(state.buffers[buffer.index], buffer.bytesused,
          kWidth, kHeight, state.resized, state.resizedCapacity, size->width,
          size->height, state.mirror, state.flip);
    }
    // Even an invalid frame must be returned before reporting failure.
    if (!io(VIDIOC_QBUF, &buffer, "return CSI frame")) return false;
    if (!valid) return fail("invalid CSI frame layout or capture error");
  }
  state.warm = true;
  jpeg_encode_memory_alloc_cfg_t allocation = {};
  allocation.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  size_t capacity = 0;
  auto* jpeg = static_cast<uint8_t*>(jpeg_alloc_encoder_mem(budget, &allocation, &capacity));
  if (!jpeg || capacity < budget) {
    free(jpeg); return fail("allocate bounded JPEG frame", ESP_ERR_NO_MEM);
  }
  // IDF 5.5's encoder allocator uses cached calloc. Its encoder synchronizes
  // raw input and invalidates completed output, but does not clean output
  // before DMA writes it. Dirty zero-filled cache lines can otherwise evict
  // over the new JPEG entropy stream under concurrent application activity.
  // Hand a clean, invalidated allocation to DMA. The driver subsequently
  // writes its aligned CPU-only JPEG header in separate cache lines.
  const esp_err_t prepared = esp_cache_msync(jpeg, capacity,
      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (prepared != ESP_OK) {
    free(jpeg); return fail("prepare JPEG output for DMA", prepared);
  }
  jpeg_encode_cfg_t encode = {};
  encode.width = size->width; encode.height = size->height;
  encode.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  encode.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  encode.image_quality = camera_rgb565::qualityToJpeg(state.quality);
  encode.pixel_reverse = false;
  uint32_t used = 0;
  const esp_err_t err = jpeg_encoder_process(state.encoder, &encode,
      state.resized, rawBytes, jpeg, budget, &used);
  if (err != ESP_OK) {
    free(jpeg);
    return fail(err == ESP_ERR_INVALID_STATE ?
        "JPEG rejected (output budget exceeded or encoder error); lower resolution/quality" :
        "JPEG encoding failed", err);
  }
  if (used < 4 || used > budget || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
      jpeg[used - 2] != 0xff || jpeg[used - 1] != 0xd9) {
    free(jpeg); return fail("invalid bounded JPEG output");
  }
  // jpeg_alloc_encoder_mem storage is free()-compatible. It is detached from
  // all camera/driver state and remains valid through stop/reconfiguration.
  frame = {jpeg, used, size->width, size->height};
  state.error[0] = 0;
  return true;
}

bool setResolution(CameraFrameSize resolution) {
  if (!cameraFrameSizeInfo(resolution) || !(kResolutionMask & cameraResolutionBit(resolution)))
    return fail("resolution would upscale the native camera image", ESP_ERR_NOT_SUPPORTED);
  state.resolution = resolution; state.error[0] = 0; return true;
}
bool setQuality(int quality) {
  if (!camera_rgb565::qualityToJpeg(quality)) return fail("quality must be 0..63", ESP_ERR_INVALID_ARG);
  state.quality = quality; state.error[0] = 0; return true;
}
bool setControl(CameraControl control, int value) {
  if ((control != CameraControl::HMirror && control != CameraControl::VFlip) || (value != 0 && value != 1)) {
    return fail("unsupported control or value", ESP_ERR_NOT_SUPPORTED);
  }
  (control == CameraControl::HMirror ? state.mirror : state.flip) = value != 0;
  state.error[0] = 0; return true;
}
bool getControl(CameraControl control, int& value) {
  if (control == CameraControl::HMirror) { value = state.mirror; return true; }
  if (control == CameraControl::VFlip) { value = state.flip; return true; }
  return false;
}
}  // namespace

const CameraBackend& cameraP4Backend() {
  static const CameraBackend backend = {capabilities, info, begin, end, capture,
      setResolution, setQuality, setControl, getControl, nullptr, nullptr};
  return backend;
}
#endif
