#include "HAL_Camera.h"
#include "HAL_JPEG_Backend.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
const char* esp_err_to_name(int) { return "mock SDK failure"; }
using i2c_master_bus_handle_t = void*;
enum i2c_port_num_t {I2C_NUM_0, I2C_NUM_1};
using gainceiling_t = int;
enum framesize_t { FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XGA,
  FRAMESIZE_SXGA, FRAMESIZE_UXGA, FRAMESIZE_96X96, FRAMESIZE_QQVGA, FRAMESIZE_QCIF,
  FRAMESIZE_HQVGA, FRAMESIZE_240X240, FRAMESIZE_HD, FRAMESIZE_CIF };
enum {LEDC_CHANNEL_0, LEDC_TIMER_0, PIXFORMAT_JPEG, CAMERA_FB_IN_PSRAM,
      CAMERA_FB_IN_DRAM, CAMERA_GRAB_LATEST, OV2640_PID, OV3660_PID, OV5640_PID};
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned) {}
#define INFO_CAMERAF(...) ((void)0)
#define BROADCAST_PRINTF(...) ((void)0)
struct sensor_t;
using Setter = int (*)(sensor_t*, int);
#define CONTROL_FIELDS(X) \
  X(brightness) X(contrast) X(saturation) X(sharpness) X(denoise) X(wb_mode) \
  X(special_effect) X(hmirror) X(vflip) X(ae_level) X(exposure_ctrl) X(aec_value) \
  X(gain_ctrl) X(agc_gain) X(gainceiling) X(whitebal) X(awb_gain) X(aec2) X(dcw) \
  X(bpc) X(wpc) X(raw_gma) X(lenc) X(colorbar)
struct sensor_t {
  struct {int PID = OV3660_PID;} id;
  struct {
#define STATUS(name) int name = 0;
    CONTROL_FIELDS(STATUS)
#undef STATUS
    int framesize = FRAMESIZE_VGA, quality = 12, aec = 0, agc = 0, awb = 0;
  } status;
#define SETTER(name) Setter set_##name = nullptr;
  CONTROL_FIELDS(SETTER)
#undef SETTER
  Setter set_framesize = nullptr, set_quality = nullptr;
  int (*set_reg)(sensor_t*, int, int, int) = nullptr;
};
struct camera_config_t {
  int ledc_channel, ledc_timer;
  int pin_d0, pin_d1, pin_d2, pin_d3, pin_d4, pin_d5, pin_d6, pin_d7;
  int pin_xclk, pin_pclk, pin_vsync, pin_href, pin_sccb_sda, pin_sccb_scl, pin_pwdn, pin_reset;
  int xclk_freq_hz, pixel_format, fb_location, jpeg_quality, fb_count, grab_mode;
  framesize_t frame_size;
};
struct camera_fb_t {
  uint8_t* buf;
  size_t len;
  uint16_t width, height;
  int format;
};
enum class AllocPref { PreferPSRAM };
namespace Fake {
bool psram = true, busyBus = false, nullSensor = false, failInit = false;
bool failFrameSetter = false, failQualitySetter = false, failAllocation = false;
bool invalidJpeg = false, oversizedJpeg = false, falseMetadata = false;
int initCalls = 0, returned = 0, liveBuffers = 0, queriedPort = -1;
int nullFrames = 0, fetchCalls = 0, deinitCalls = 0;
bool alwaysTimeout = false;
int entropyFailures = 0, entropyCalls = 0;
sensor_t sensor;
camera_config_t config;
size_t capacity = 0;
std::vector<uint8_t> original, jpeg;
std::deque<std::pair<unsigned, unsigned>> geometries;
size_t sof = 0;
}
bool psramFound() { return Fake::psram; }
void* ps_alloc(size_t n, AllocPref, const char*) { return Fake::failAllocation ? nullptr : malloc(n); }
esp_err_t i2c_master_get_bus_handle(i2c_port_num_t port, i2c_master_bus_handle_t* out) {
  Fake::queriedPort = int(port);
  *out = Fake::busyBus ? &Fake::sensor : nullptr;
  return Fake::busyBus ? ESP_OK : ESP_FAIL;
}
int genericSetter(sensor_t*, int) { return 0; }
int frameSetter(sensor_t* s, int value) {
  if (Fake::failFrameSetter) return -1;
  s->status.framesize = value; return 0;
}
int qualitySetter(sensor_t* s, int value) {
  if (Fake::failQualitySetter) return -1;
  s->status.quality = value; return 0;
}
esp_err_t esp_camera_init(const camera_config_t* config) {
  ++Fake::initCalls; Fake::config = *config;
  if (Fake::failInit) return ESP_FAIL;
  auto* size = cameraFrameSizeInfo(CameraFrameSize(config->frame_size));
  Fake::capacity = size->width * size->height / 5;
  Fake::sensor.status.framesize = config->frame_size;
  Fake::sensor.status.quality = config->jpeg_quality;
#define ASSIGN(name) Fake::sensor.set_##name = genericSetter;
  CONTROL_FIELDS(ASSIGN)
#undef ASSIGN
  Fake::sensor.set_framesize = frameSetter;
  Fake::sensor.set_quality = qualitySetter;
  return ESP_OK;
}
sensor_t* esp_camera_sensor_get() { return Fake::nullSensor ? nullptr : &Fake::sensor; }
esp_err_t esp_camera_deinit() { ++Fake::deinitCalls; assert(Fake::liveBuffers == 0); return ESP_OK; }
camera_fb_t* esp_camera_fb_get() {
  ++Fake::fetchCalls;
  if (Fake::alwaysTimeout) return nullptr;
  if (Fake::nullFrames > 0) { --Fake::nullFrames; return nullptr; }
  auto* current = cameraFrameSizeInfo(CameraFrameSize(Fake::sensor.status.framesize));
  unsigned width = current->width, height = current->height;
  if (!Fake::geometries.empty()) {
    width = Fake::geometries.front().first; height = Fake::geometries.front().second;
    Fake::geometries.pop_front();
  }
  Fake::jpeg = Fake::original;
  Fake::jpeg[Fake::sof + 5] = uint8_t(height >> 8);
  Fake::jpeg[Fake::sof + 6] = uint8_t(height);
  Fake::jpeg[Fake::sof + 7] = uint8_t(width >> 8);
  Fake::jpeg[Fake::sof + 8] = uint8_t(width);
  if (Fake::invalidJpeg) Fake::jpeg[0] = 0;
  if (Fake::oversizedJpeg) Fake::jpeg.resize(kCameraMaxFrameBytes + 1);
  static camera_fb_t frame;
  // Match the real driver's misleading relabeling from current sensor status.
  frame = {Fake::jpeg.data(), Fake::jpeg.size(),
           uint16_t(Fake::falseMetadata ? 17 : current->width),
           uint16_t(Fake::falseMetadata ? 19 : current->height), PIXFORMAT_JPEG};
  ++Fake::liveBuffers;
  assert(Fake::liveBuffers == 1);
  return &frame;
}
void esp_camera_fb_return(camera_fb_t*) { --Fake::liveBuffers; ++Fake::returned; }
namespace hwjpeg { namespace detail {
bool validateSoftwareEntropy(const uint8_t*, size_t, const Info&, const char** error) {
  ++Fake::entropyCalls;
  if (Fake::entropyFailures > 0) {
    --Fake::entropyFailures;
    if (error) *error = "simulated entropy failure";
    return false;
  }
  return true;
}
bool decodeHardware(const uint8_t*, size_t, const Info&, Image&, const char**) { assert(false); return false; }
bool decodeSoftware(const uint8_t*, size_t, const Info&, Image&, const char**, OutputAllocator) { assert(false); return false; }
}}
#include "camera_dvp_extracted.inc"
int main(int argc, char** argv) {
  assert(argc == 2);
  std::ifstream input(argv[1], std::ios::binary);
  Fake::original = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  for (size_t i = 0; i + 1 < Fake::original.size(); ++i)
    if (Fake::original[i] == 0xff && Fake::original[i + 1] == 0xc0) { Fake::sof = i; break; }
  assert(Fake::sof);
  const auto& backend = cameraDvpBackend();
  CameraConfig config;
  config.resolution = CameraFrameSize::Square240; config.quality = 0;
  Fake::busyBus = true;
  assert(!backend.begin(config)); assert(Fake::initCalls == 0);
  assert(Fake::queriedPort == CAMERA_SCCB_I2C_PORT);
  Fake::busyBus = false;
  assert(backend.begin(config));
  assert(Fake::config.frame_size == FRAMESIZE_VGA && Fake::capacity == 61440);
  assert(Fake::config.jpeg_quality == 10); // stable initial mode only
  assert(Fake::sensor.status.framesize == FRAMESIZE_240X240 && Fake::sensor.status.quality == 0);
  assert(backend.info().resolution == CameraFrameSize::Square240 && backend.info().quality == 0);
  assert(backend.setResolution(CameraFrameSize::VGA));
  assert(Fake::capacity == 61440 && Fake::config.frame_size == FRAMESIZE_VGA);
  CameraFrame frame;
  Fake::geometries = {{240,240}, {640,480}};
  int before = Fake::returned;
  assert(backend.capture(frame, kCameraMaxFrameBytes));
  assert(Fake::returned == before + 2 && frame.width == 640 && frame.height == 480);
  cameraFrameRelease(frame);
  Fake::falseMetadata = true;
  assert(backend.capture(frame, kCameraMaxFrameBytes));
  assert(frame.width == 640 && frame.height == 480); cameraFrameRelease(frame);
  Fake::entropyFailures = 2; before = Fake::returned;
  assert(backend.capture(frame, kCameraMaxFrameBytes));
  assert(Fake::returned == before + 3 && Fake::entropyFailures == 0);
  cameraFrameRelease(frame);
  Fake::entropyFailures = 3; before = Fake::returned;
  assert(!backend.capture(frame, kCameraMaxFrameBytes));
  assert(Fake::returned == before + 3 && !frame.data && Fake::liveBuffers == 0);
  Fake::falseMetadata = false; Fake::geometries = {{240,240}, {240,240}, {240,240}};
  assert(!backend.capture(frame, kCameraMaxFrameBytes)); assert(!frame.data && Fake::liveBuffers == 0);
  Fake::invalidJpeg = true;
  assert(!backend.capture(frame, kCameraMaxFrameBytes)); assert(!frame.data && Fake::liveBuffers == 0);
  Fake::invalidJpeg = false; Fake::oversizedJpeg = true;
  assert(!backend.capture(frame, kCameraMaxFrameBytes)); assert(!frame.data && Fake::liveBuffers == 0);
  Fake::oversizedJpeg = false; Fake::failAllocation = true;
  assert(!backend.capture(frame, kCameraMaxFrameBytes)); assert(!frame.data && Fake::liveBuffers == 0);
  Fake::failAllocation = false;
  assert(backend.end()); Fake::psram = false;
  config.resolution = CameraFrameSize::QQVGA;
  assert(backend.begin(config));
  assert(Fake::config.frame_size == FRAMESIZE_QVGA && Fake::capacity == 15360);
  assert(!(backend.capabilities().resolutions & cameraResolutionBit(CameraFrameSize::VGA)));
  assert(!backend.setResolution(CameraFrameSize::VGA));
  assert(backend.end());
  Fake::failFrameSetter = true;
  assert(!backend.begin(config)); assert(!sRunning && Fake::liveBuffers == 0);
  Fake::failFrameSetter = false;
  Fake::nullFrames = 1; // an early timeout may recover after controls settle
  assert(backend.begin(config)); assert(sRunning && Fake::liveBuffers == 0);
  assert(backend.end());
  Fake::alwaysTimeout = true;
  int fetchBefore = Fake::fetchCalls, cleanupBefore = Fake::deinitCalls;
  assert(!backend.begin(config));
  assert(!sRunning && Fake::liveBuffers == 0 && Fake::deinitCalls == cleanupBefore + 1);
  assert(Fake::fetchCalls - fetchBefore == 3); // first warmup, post-controls, readiness
  assert(backend.info().error && strstr(backend.info().error, "timeout"));
  Fake::alwaysTimeout = false; Fake::invalidJpeg = true;
  assert(!backend.begin(config)); assert(!sRunning && Fake::liveBuffers == 0);
  assert(backend.info().error && strstr(backend.info().error, "Invalid"));
  Fake::invalidJpeg = false;
  assert(backend.begin(config)); assert(backend.end());
  printf("DVP PASS: SDK port%d ownership, max geometry allocation, live resize, SOF verification, cleanup\n", CAMERA_SCCB_I2C_PORT);
}
