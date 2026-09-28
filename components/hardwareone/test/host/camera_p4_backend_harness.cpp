// Compile the real P4 backend below with only its hardware/SDK boundaries mocked.
#include "HAL_Camera.h"
#include "CameraRgb565.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sys/time.h>
#include <vector>
#include <algorithm>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 1,
    ESP_ERR_INVALID_STATE = 2, ESP_ERR_NOT_SUPPORTED = 3, ESP_ERR_NO_MEM = 4,
    ESP_ERR_TIMEOUT = 5;
const char* esp_err_to_name(int err) { return err == 0 ? "OK" : "mock_error"; }
void mockLog(const char*, const char*, ...) {}
#define ESP_LOGW(...) mockLog(__VA_ARGS__)
#define ESP_LOGI(...) mockLog(__VA_ARGS__)
#define CAMERA_SCCB_I2C_PORT 0
using gpio_num_t = int;
constexpr int GPIO_NUM_12 = 12, GPIO_NUM_11 = 11, GPIO_NUM_26 = 26,
    GPIO_NUM_14 = 14, GPIO_NUM_13 = 13, GPIO_NUM_NC = -1, GPIO_MODE_OUTPUT = 1;
using i2c_port_num_t = int;
using i2c_master_bus_handle_t = void*;
using esp_cam_sensor_xclk_handle_t = void*;
using jpeg_encoder_handle_t = void*;
constexpr int I2C_CLK_SRC_DEFAULT = 0, ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER = 1;
constexpr uint32_t ESP_VIDEO_INIT_FLAGS_MIPI_CSI = 1, ESP_VIDEO_INIT_FLAGS_ISP = 2;
constexpr const char* ESP_VIDEO_MIPI_CSI_DEVICE_NAME = "/dev/camera-mock";
constexpr unsigned ESP_CAM_SENSOR_IOC_G_CHIP_ID = 10, V4L2_CTRL_CLASS_ESP_CAM_IOCTL = 11,
    V4L2_BUF_TYPE_VIDEO_CAPTURE = 12, V4L2_MEMORY_MMAP = 13, V4L2_PIX_FMT_RGB565 = 14,
    V4L2_BUF_FLAG_ERROR = 1;
constexpr int ESP_CACHE_MSYNC_FLAG_DIR_C2M = 1, ESP_CACHE_MSYNC_FLAG_INVALIDATE = 2;
constexpr int JPEG_ENC_ALLOC_INPUT_BUFFER = 1, JPEG_ENC_ALLOC_OUTPUT_BUFFER = 2,
    JPEG_ENCODE_IN_FORMAT_RGB565 = 3, JPEG_DOWN_SAMPLING_YUV420 = 4;
constexpr int O_RDONLY = 0, PROT_READ = 1, PROT_WRITE = 2, MAP_SHARED = 3;
#define MAP_FAILED reinterpret_cast<void*>(-1)
enum { VIDIOC_STREAMOFF = 100, VIDIOC_G_EXT_CTRLS, VIDIOC_G_SENSOR_FMT, VIDIOC_G_FMT,
       VIDIOC_S_FMT, VIDIOC_S_DQBUF_TIMEOUT, VIDIOC_REQBUFS, VIDIOC_QUERYBUF,
       VIDIOC_QBUF, VIDIOC_STREAMON, VIDIOC_DQBUF };
struct i2c_master_bus_config_t {
  int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
  struct { bool enable_internal_pullup; } flags;
};
struct esp_cam_sensor_xclk_config_t { struct { int xclk_pin, xclk_freq_hz; } esp_clock_router_cfg; };
struct esp_video_init_csi_config_t {
  struct { bool init_sccb; void* i2c_handle; unsigned freq; } sccb_config;
  int reset_pin, pwdn_pin; bool dont_init_ldo;
};
struct esp_video_init_config_t { esp_video_init_csi_config_t* csi; };
struct esp_cam_sensor_id_t { uint16_t pid; };
struct esp_cam_sensor_format_t { unsigned width, height; };
struct v4l2_ext_control { unsigned id; uint8_t* p_u8; unsigned size; };
struct v4l2_ext_controls { unsigned ctrl_class, count; v4l2_ext_control* controls; };
struct v4l2_format {
  unsigned type;
  struct { struct { unsigned width, height, pixelformat, bytesperline, sizeimage; } pix; } fmt;
};
struct v4l2_requestbuffers { unsigned count, type, memory; };
struct v4l2_buffer {
  unsigned index, type, memory, length, flags, bytesused;
  struct { unsigned offset; } m;
};
struct jpeg_encode_engine_cfg_t { int timeout_ms; };
struct jpeg_encode_memory_alloc_cfg_t { int buffer_direction; };
struct jpeg_encode_cfg_t { unsigned width, height, src_type, sub_sample, image_quality; bool pixel_reverse; };
namespace fake {
constexpr size_t rawBytes = 1280 * 720 * 2;
bool foreignBus = false, bus = false, clock = false, clockRunning = false,
    video = false, stream = false, encoder = false, fd = false;
bool failVideo = false, failEncoder = false, failStreamOff = false, failJpeg = false,
    badFrame = false, badIndex = false, badNative = false, noClock = false, failFlush = false;
bool dirtyOutput = false;
void* outputAllocation = nullptr;
size_t allocatedOutputBytes = 0;
int failRequest = -1, dequeue = 0, requeue = 0, gpioLow = 0, closes = 0;
int64_t now = 0, frameUs = 40000, maxDqWait = 2000000;
size_t outputCapacity = 0;
unsigned jpegQuality = 0;
std::vector<uint8_t*> raw;
void* token(int n) { return reinterpret_cast<void*>(intptr_t(n)); }
void releaseBuffers() { for (auto* p : raw) free(p); raw.clear(); }
void reset() {
  assert(!bus && !clock && !clockRunning && !video && !stream && !encoder && !fd && raw.empty());
  foreignBus = failVideo = failEncoder = failStreamOff = failJpeg = badFrame = badIndex = badNative = noClock = failFlush = false;
  dirtyOutput = false; outputAllocation = nullptr; allocatedOutputBytes = 0;
  failRequest = -1; dequeue = requeue = gpioLow = closes = 0;
  now = 0; frameUs = 40000; outputCapacity = 0; jpegQuality = 0;
}
}
esp_err_t i2c_master_get_bus_handle(int port, void** out) {
  assert(port == 0); *out = fake::foreignBus || fake::bus ? fake::token(1) : nullptr;
  return *out ? ESP_OK : ESP_FAIL;
}
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* cfg, void** out) {
  assert(cfg->i2c_port == 0 && cfg->sda_io_num == 14 && cfg->scl_io_num == 13);
  assert(!fake::foreignBus && !fake::bus); fake::bus = true; *out = fake::token(1); return ESP_OK;
}
esp_err_t i2c_del_master_bus(void* p) {
  assert(p && fake::bus && !fake::video); fake::bus = false; return ESP_OK;
}
esp_err_t rtc_gpio_hold_dis(int pin) { assert(pin == 12); return ESP_OK; }
esp_err_t rtc_gpio_deinit(int pin) { assert(pin == 12); return ESP_OK; }
esp_err_t gpio_set_direction(int, int) { return ESP_OK; }
esp_err_t gpio_set_level(int pin, int value) { if (pin == 12 && !value) ++fake::gpioLow; return ESP_OK; }
esp_err_t esp_cam_sensor_xclk_allocate(int, void** out) {
  if (fake::noClock) return ESP_ERR_NO_MEM;
  fake::clock = true; *out = fake::token(2); return ESP_OK;
}
esp_err_t esp_cam_sensor_xclk_start(void* h, const esp_cam_sensor_xclk_config_t* cfg) {
  assert(h && fake::clock && cfg->esp_clock_router_cfg.xclk_pin == 11);
  fake::clockRunning = true; return ESP_OK;
}
esp_err_t esp_cam_sensor_xclk_stop(void* h) { assert(h && fake::clockRunning); fake::clockRunning = false; return ESP_OK; }
esp_err_t esp_cam_sensor_xclk_free(void* h) { assert(h && fake::clock && !fake::clockRunning); fake::clock = false; return ESP_OK; }
esp_err_t esp_video_init_with_flags(const esp_video_init_config_t* cfg, unsigned flags) {
  assert(fake::bus && fake::clockRunning && cfg->csi->sccb_config.i2c_handle && flags == 3);
  // The real esp_video2.2 initializer unwinds its own partial failure.
  if (fake::failVideo) return ESP_FAIL;
  fake::video = true; return ESP_OK;
}
esp_err_t esp_video_deinit_with_flags(unsigned flags) {
  assert(flags == 3 && fake::video && !fake::fd && !fake::stream); fake::video = false; return ESP_OK;
}
int64_t esp_timer_get_time() { return fake::now; }
void vTaskDelay(unsigned ms) { fake::now += int64_t(ms) * 1000; }
#define pdMS_TO_TICKS(ms) (ms)
int mockOpen(const char*, int) { assert(fake::video); fake::fd = true; return 7; }
int mockClose(int fd) {
  assert(fd == 7 && fake::fd && !fake::stream); fake::fd = false; ++fake::closes;
  fake::releaseBuffers(); return 0;
}
void* mockMmap(void*, size_t size, int, int, int fd, unsigned offset) {
  assert(fd == 7 && size == fake::rawBytes && offset < fake::raw.size()); return fake::raw[offset];
}
int mockMunmap(void*, size_t) { return 0; }
int mockIoctl(int fd, int request, void* arg) {
  assert(fd == 7 && fake::fd);
  if (request == fake::failRequest) return -1;
  switch (request) {
    case VIDIOC_G_EXT_CTRLS: reinterpret_cast<esp_cam_sensor_id_t*>(static_cast<v4l2_ext_controls*>(arg)->controls->p_u8)->pid = 0x2710; break;
    case VIDIOC_G_SENSOR_FMT: *static_cast<esp_cam_sensor_format_t*>(arg) = {fake::badNative ? 1920u : 1280u, 720}; break;
    case VIDIOC_G_FMT: { auto* f = static_cast<v4l2_format*>(arg); f->fmt.pix.width = 1280; f->fmt.pix.height = 720; break; }
    case VIDIOC_S_FMT: break;
    case VIDIOC_S_DQBUF_TIMEOUT: {
      auto* t = static_cast<timeval*>(arg); fake::maxDqWait = int64_t(t->tv_sec) * 1000000 + t->tv_usec; break;
    }
    case VIDIOC_REQBUFS: {
      auto* req = static_cast<v4l2_requestbuffers*>(arg);
      for (unsigned i = 0; i < req->count; ++i) fake::raw.push_back(static_cast<uint8_t*>(calloc(1, fake::rawBytes)));
      break;
    }
    case VIDIOC_QUERYBUF: { auto* b = static_cast<v4l2_buffer*>(arg); b->length = fake::rawBytes; b->m.offset = b->index; break; }
    case VIDIOC_QBUF: ++fake::requeue; break;
    case VIDIOC_STREAMON: assert(!fake::stream); fake::stream = true; break;
    case VIDIOC_STREAMOFF:
      if (fake::failStreamOff) return -1;
      assert(fake::stream); fake::stream = false; break;
    case VIDIOC_DQBUF: {
      assert(fake::stream);
      const int64_t waited = std::min(fake::frameUs, fake::maxDqWait);
      fake::now += waited;
      if (waited < fake::frameUs) return -1;
      auto* b = static_cast<v4l2_buffer*>(arg); b->index = fake::badIndex ? 99 : fake::dequeue % 3;
      b->bytesused = fake::badFrame ? 0 : fake::rawBytes; ++fake::dequeue; break;
    }
    default: assert(false);
  }
  return 0;
}
esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t* cfg, void** out) {
  assert(cfg->timeout_ms == 2000);
  if (fake::failEncoder) return ESP_ERR_NO_MEM;
  fake::encoder = true; *out = fake::token(3); return ESP_OK;
}
esp_err_t jpeg_del_encoder_engine(void* h) { assert(h && fake::encoder); fake::encoder = false; return ESP_OK; }
void* jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t* cfg, size_t* capacity) {
  *capacity = size;
  auto* allocation = calloc(1, size);
  if (cfg->buffer_direction == JPEG_ENC_ALLOC_OUTPUT_BUFFER) {
    // Cached calloc dirties output cache lines before the DMA engine owns it.
    fake::dirtyOutput = true; fake::outputAllocation = allocation;
    fake::allocatedOutputBytes = size;
  }
  return allocation;
}
esp_err_t esp_cache_msync(void* p, size_t size, int flags) {
  assert(p == fake::outputAllocation && size == fake::allocatedOutputBytes);
  assert(flags == (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
  if (fake::failFlush) return ESP_FAIL;
  fake::dirtyOutput = false;
  return ESP_OK;
}
esp_err_t jpeg_encoder_process(void* h, const jpeg_encode_cfg_t* cfg, const uint8_t*, uint32_t rawBytes,
    uint8_t* out, uint32_t capacity, uint32_t* used) {
  assert(h && fake::encoder && rawBytes == cfg->width * cfg->height * 2);
  // Omitting pre-DMA preparation would let dirty calloc lines overwrite the
  // entropy output. This checks the real backend ownership handoff order.
  assert(out == fake::outputAllocation && !fake::dirtyOutput);
  fake::outputCapacity = capacity; fake::jpegQuality = cfg->image_quality;
  if (fake::failJpeg) return ESP_ERR_INVALID_STATE;
  *used = 8; out[0] = 255; out[1] = 216; out[6] = 255; out[7] = 217; return ESP_OK;
}
#define open mockOpen
#define close mockClose
#define mmap mockMmap
#define munmap mockMunmap
#define ioctl mockIoctl

// INSERT_PRODUCTION_P4_BACKEND

int main() {
  const auto& b = cameraP4Backend();
  CameraConfig config;
  assert(!(b.capabilities().resolutions & cameraResolutionBit(CameraFrameSize::UXGA)));
  assert(!b.begin(CameraConfig{CameraFrameSize::UXGA, 12, {}}));
  assert(!fake::bus);
  // Never delete or reconfigure a controller owned by Wire/another driver.
  fake::reset(); fake::foreignBus = true;
  assert(!b.begin(config)); assert(fake::foreignBus && !fake::bus && !fake::clock);
  fake::foreignBus = false;
  // Every ordinary partial initialization failure must unwind our resources.
  for (int fault = 0; fault < 6; ++fault) {
    fake::reset();
    if (fault == 0) fake::noClock = true;
    if (fault == 1) fake::failVideo = true;
    if (fault == 2) fake::badNative = true;
    if (fault == 3) fake::failRequest = VIDIOC_QUERYBUF;
    if (fault == 4) fake::failEncoder = true;
    if (fault == 5) fake::failRequest = VIDIOC_STREAMON;
    assert(!b.begin(config));
    assert(!fake::bus && !fake::clock && !fake::video && !fake::encoder && !fake::fd && fake::raw.empty());
    assert(b.info().error && fake::gpioLow == 0);
  }
  fake::reset(); assert(b.begin(config));
  assert(b.info().detected && b.info().width == 640 && b.info().height == 480);
  CameraFrame frame;
  assert(b.capture(frame, 128 * 1024));
  assert(frame.width == 640 && frame.height == 480 && frame.length == 8 && frame.data);
  assert(fake::dequeue == 51 && fake::requeue == 54 && fake::jpegQuality == 81);
  // The first returned frame is independent of the stream lifetime.
  uint8_t* owned = frame.data;
  assert(b.end()); assert(owned[0] == 255 && owned[7] == 217); cameraFrameRelease(frame);
  assert(b.end() && fake::gpioLow == 0);
  fake::reset(); assert(b.begin(config));
  assert(b.setResolution(CameraFrameSize::QQVGA)); assert(b.setQuality(63));
  assert(b.setControl(CameraControl::HMirror, 1));
  assert(!b.setControl(CameraControl::Brightness, 1));
  assert(!b.setResolution(CameraFrameSize::UXGA));
  assert(b.info().resolution == CameraFrameSize::QQVGA);
  assert(b.capture(frame, 8191));
  assert(frame.width == 160 && frame.height == 120 && fake::outputCapacity == 8128 && fake::jpegQuality == 1);
  cameraFrameRelease(frame);
  const int before = fake::dequeue;
  assert(b.capture(frame, 128 * 1024)); assert(fake::dequeue - before == 4); cameraFrameRelease(frame);
  assert(!b.capture(frame, 999) && !frame.data);
  fake::failFlush = true; assert(!b.capture(frame, 128 * 1024) && !frame.data);
  fake::failFlush = false;
  fake::failJpeg = true; assert(!b.capture(frame, 128 * 1024) && !frame.data);
  fake::failJpeg = false; assert(b.capture(frame, 128 * 1024)); cameraFrameRelease(frame);
  fake::badFrame = true;
  const int queued = fake::requeue;
  assert(!b.capture(frame, 128 * 1024) && !frame.data && fake::requeue == queued + 1);
  fake::badFrame = false; fake::badIndex = true;
  assert(!b.capture(frame, 128 * 1024)); fake::badIndex = false;
  // A failed stream stop retains DMA storage and bus/clock for a retry.
  fake::failStreamOff = true;
  assert(!b.end() && fake::stream && fake::fd && fake::bus && fake::clockRunning && !fake::raw.empty());
  assert(!b.begin(config) && fake::stream);
  fake::failStreamOff = false;
  assert(b.end());
  fake::reset(); assert(b.begin(config));
  fake::frameUs = 1000000;
  const int64_t began = fake::now;
  assert(!b.capture(frame, 128 * 1024));
  assert(fake::now - began <= 5000000 && fake::dequeue <= 5 && !frame.data);
  assert(b.end()); fake::reset();
  puts("Actual P4 backend ownership, cleanup, stream-stop retry, deadlines, budget and capture failures passed");
}
