#include "HAL_Camera.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) ((m)->lock())
#define portEXIT_CRITICAL(m) ((m)->unlock())
using StaticSemaphore_t = std::recursive_timed_mutex;
using SemaphoreHandle_t = StaticSemaphore_t*;
static constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* s) { return s; }
int xSemaphoreTakeRecursive(SemaphoreHandle_t s, unsigned ms) {
  return ms ? s->try_lock_for(std::chrono::milliseconds(ms)) : s->try_lock();
}
void xSemaphoreGiveRecursive(SemaphoreHandle_t s) { s->unlock(); }
void vTaskDelay(unsigned) {}
#define INFO_CAMERAF(...) ((void)0)
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)0)
void sensorStatusBumpWith(const char*) {}
void logSystemEvent(const char*, const char*, ...) {}
enum {SYSEVT_SENSOR_START_FAILED, SYSEVT_SENSOR_STARTED, SYSEVT_SENSOR_STOPPED};
int startFailedEvents = 0;
void systemEventPost(int kind, const char*, const char* = nullptr) {
  if (kind == SYSEVT_SENSOR_START_FAILED) ++startFailedEvents;
}
#include "camera_core_types.inc"
bool videoRecording = false;
void stopVideoRecording() { videoRecording = false; }
class String {
  std::string s;
public:
  String(const char* value = "") : s(value) {}
  void trim() {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == s.npos) { s.clear(); return; }
    s = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
  }
  bool operator==(const char* other) const { return s == other; }
  void toLowerCase() { for (auto& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch))); }
  size_t length() const { return s.size(); }
  const char* c_str() const { return s.c_str(); }
  int toInt() const { return std::atoi(s.c_str()); }
  bool equalsIgnoreCase(const char* other) const { return strcasecmp(s.c_str(), other) == 0; }
};
int parseBoolArg(const String& a) {
  if (a.equalsIgnoreCase("on") || a.equalsIgnoreCase("true") || a.equalsIgnoreCase("1")) return 1;
  if (a.equalsIgnoreCase("off") || a.equalsIgnoreCase("false") || a.equalsIgnoreCase("0")) return 0;
  return -2;
}
struct Settings {
  int cameraFramesize = 1, cameraQuality = 12;
  int cameraBrightness = 0, cameraContrast = 0, cameraSaturation = 0;
  int cameraSharpness = 0, cameraDenoise = 0, cameraWBMode = 0, cameraSpecialEffect = 0, cameraAELevel = 0;
  bool cameraHMirror = false, cameraVFlip = false;
  bool cameraAutoStart = true;
} gSettings;
int persistCount = 0;
template<typename T> void setSetting(T& target, T value) { target = value; ++persistCount; }
bool initCamera(bool recovery = false);
void stopCamera(bool recovery = false);
bool captureCameraFrame(CameraFrame&, size_t maxBytes = kCameraMaxFrameBytes);

namespace Fake {
bool running = false, failBegin = false, failEnd = false, failControl = false, failQuality = false;
bool failSize = false, failRestore = false, invalidFrame = false, oversizedFrame = false;
bool partialFailure = false;
int failCaptures = 0, beginCount = 0, endCount = 0, captureCount = 0;
CameraFrameSize size = CameraFrameSize::VGA;
int quality = 12, controls[unsigned(CameraControl::Count)] = {};
uint32_t sizeMask = (uint32_t(1) << unsigned(CameraFrameSize::Count)) - 1;
uint64_t controlMask = (uint64_t(1) << unsigned(CameraControl::Count)) - 1;
std::mutex gateMutex;
std::condition_variable gate;
bool blockCapture = false, enteredCapture = false, releaseCapture = false;
CameraCapabilities caps() { return {"fake", sizeMask, controlMask, false, false, 0, 0}; }
CameraBackendInfo info() {
  auto* dims = cameraFrameSizeInfo(size);
  return {"Fake sensor", true, size, quality, dims->width, dims->height, nullptr};
}
bool begin(const CameraConfig& c) {
  ++beginCount;
  if (failBegin) return false;
  running = true; size = c.resolution; quality = c.quality;
  memcpy(controls, c.controls, sizeof controls);
  return true;
}
bool end() { ++endCount; if (failEnd) return false; running = false; return true; }
bool setResolution(CameraFrameSize value) {
  if (failSize || (failRestore && value == CameraFrameSize::VGA)) return false;
  size = value; return true;
}
bool setQuality(int value) { if (failQuality) return false; quality = value; return true; }
bool setControl(CameraControl c, int value) { if (failControl) return false; controls[unsigned(c)] = value; return true; }
bool getControl(CameraControl c, int& value) { value = controls[unsigned(c)]; return true; }
bool capture(CameraFrame& frame, size_t maxBytes) {
  ++captureCount;
  assert(running);
  if (blockCapture) {
    std::unique_lock<std::mutex> lock(gateMutex);
    enteredCapture = true; gate.notify_all();
    gate.wait(lock, [] { return releaseCapture; });
  }
  if (failCaptures > 0) { --failCaptures; return false; }
  auto* dims = cameraFrameSizeInfo(size);
  frame.length = oversizedFrame ? maxBytes + 1 : 8;
  frame.data = static_cast<uint8_t*>(malloc(frame.length));
  memset(frame.data, 0, frame.length);
  frame.data[0] = 0xff; frame.data[1] = invalidFrame ? 0 : 0xd8;
  frame.data[frame.length - 2] = 0xff; frame.data[frame.length - 1] = 0xd9;
  frame.width = dims->width; frame.height = dims->height;
  return !partialFailure;
}
void reset() {
  running = failBegin = failEnd = failControl = failQuality = failSize = failRestore = false;
  invalidFrame = oversizedFrame = partialFailure = false;
  failCaptures = beginCount = endCount = captureCount = 0;
  size = CameraFrameSize::VGA; quality = 12;
  sizeMask = (uint32_t(1) << unsigned(CameraFrameSize::Count)) - 1;
  controlMask = (uint64_t(1) << unsigned(CameraControl::Count)) - 1;
  blockCapture = enteredCapture = releaseCapture = false;
}
}
const CameraBackend& cameraBackend() {
  static const CameraBackend backend{Fake::caps, Fake::info, Fake::begin, Fake::end, Fake::capture,
    Fake::setResolution, Fake::setQuality, Fake::setControl, Fake::getControl, nullptr, nullptr};
  return backend;
}
#include "camera_core_extracted.inc"

// Boundaries of the power worker's per-command body; the queue, task and
// completion slots are not part of this suite.
using CameraPowerPostHook = void (*)();
static CameraPowerPostHook sCamPwrHook = nullptr;
static bool cameraPwrLifecycleTake() { return true; }
static void cameraPwrLifecycleGive() {}
static int pwrCompletions = 0, ramFlushFailedMarks = 0;
static bool pwrLastResult = false;
template <typename Msg> static void cameraPwrComplete(const Msg&, bool result) {
  ++pwrCompletions; pwrLastResult = result;
}
enum { RF_CAMERA = 1 };
static void ramFlushMarkAutostartFailed(int) { ++ramFlushFailedMarks; }
#define BROADCAST_PRINTF(...) ((void)0)
#include "camera_pwr_extracted.inc"

void reset() {
  Fake::reset(); gSettings = {}; persistCount = 0;
  gCameraRunning = cameraConnected = cameraStreaming = cameraDetected = false;
  sCameraDesiredOn = false;
  startFailedEvents = pwrCompletions = ramFlushFailedMarks = 0;
}
void start() { sCameraDesiredOn = true; assert(initCamera()); }

#define CHECK(condition, what) \
  do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", what); return 1; } } while (0)

// A failed start from any surface keeps the saved intent and reports once.
static int powerStartScenario() {
  reset(); sCameraDesiredOn = true; gSettings.cameraAutoStart = true;
  Fake::failBegin = true;
  const CameraPwrMsg msg{CAM_PWR_CMD_START, 0xff, 0, 0};
  cameraPwrRunOne(msg);
  CHECK(!gCameraRunning && pwrCompletions == 1 && !pwrLastResult, "failed start completes with false");
  CHECK(gSettings.cameraAutoStart && persistCount == 0, "failed start keeps the saved camera auto-start");
  CHECK(startFailedEvents == 1, "failed start posts exactly one start-failed event");
  CHECK(ramFlushFailedMarks == 1, "failed start is marked for the RAM-flush resume");
  Fake::failBegin = false;
  cameraPwrRunOne(msg);
  CHECK(gCameraRunning && pwrLastResult && startFailedEvents == 1, "a later start succeeds without new failure events");
  puts("camera core power-start: failed start keeps auto-start, posts one event PASS");
  return 0;
}

// Capture recovery, a live resolution change and an explicit stop leave the
// consumers' sessions alone.
static int sessionsScenario() {
  CameraFrame frame;
  reset(); start();
#if HAS_CAMERA_SESSIONS
  cameraSessionBegin(CameraSessionKind::Stream);
  CHECK(cameraStreamActive() && cameraCaptureBusy() && cameraStreaming, "stream session is visible");
#else
  cameraStreaming = true;  // the base build's only stream state
#endif
  Fake::failCaptures = 1;
  CHECK(captureCameraFrame(frame) && Fake::beginCount == 2 && Fake::endCount == 1, "capture recovered by re-init");
  cameraFrameRelease(frame);
  CHECK(cameraStreaming, "capture recovery keeps the stream state");
  const char* reply = cmd_camerares("qvga");
  CHECK(strstr(reply, "320x240") && !strstr(reply, "stopped"), "resolution change does not claim the stream stopped");
  CHECK(cameraStreaming, "resolution change keeps the stream state");
#if HAS_CAMERA_SESSIONS
  CHECK(cameraStreamActive() && cameraCaptureBusy(), "stream session survives recovery and resolution change");
  cameraSessionBegin(CameraSessionKind::Recording);
  cameraSessionEnd(CameraSessionKind::Stream);
  CHECK(!cameraStreamActive() && !cameraStreaming && cameraCaptureBusy(), "recording alone keeps the camera busy");
  stopCamera();
  CHECK(!gCameraRunning && cameraCaptureBusy(), "stop leaves the recording session to its owner");
  cameraSessionEnd(CameraSessionKind::Recording);
  CHECK(!cameraCaptureBusy(), "ending the last session clears busy");
  cameraSessionEnd(CameraSessionKind::Recording);
  cameraSessionEnd(CameraSessionKind::Stream);
  CHECK(!cameraCaptureBusy() && !cameraStreaming, "an unmatched end does not underflow");
  cameraSessionBegin(CameraSessionKind::Stream);
  CHECK(cameraStreamActive(), "a session after an unmatched end still counts");
#endif
  puts("camera core sessions: recovery, resolution change and stop keep session state PASS");
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "power-start") == 0) return powerStartScenario();
  if (argc > 1 && strcmp(argv[1], "sessions") == 0) return sessionsScenario();
  if (argc > 1) { fprintf(stderr, "unknown scenario %s\n", argv[1]); return 2; }
  CameraFrame frame;
  reset(); assert(!captureCameraFrame(frame)); assert(Fake::captureCount == 0);
  Fake::failBegin = true; assert(!initCamera()); assert(!gCameraRunning && !cameraConnected);
  stopCamera(); assert(Fake::endCount == 1); // retries cleanup even after failed begin
  assert(cameraResolutionBit(CameraFrameSize(255)) == 0);
  assert(cameraControlBit(CameraControl(255)) == 0);
  reset(); gSettings.cameraFramesize = 5;
  Fake::sizeMask = cameraResolutionBit(CameraFrameSize::VGA);
  start(); assert(Fake::size == CameraFrameSize::VGA); assert(gSettings.cameraFramesize == 5);
  reset(); start(); assert(captureCameraFrame(frame));
  assert(captureCameraFrame(frame)); // replaces/releases a previous owned allocation
  auto* owned = frame.data; stopCamera(); assert(owned[0] == 0xff && owned[7] == 0xd9);
  cameraFrameRelease(frame); assert(!frame.data && !frame.length);
  reset(); start(); Fake::oversizedFrame = true;
  assert(!captureCameraFrame(frame, 8)); assert(!frame.data && !frame.length);
  Fake::oversizedFrame = false; Fake::invalidFrame = true;
  assert(!captureCameraFrame(frame)); assert(!frame.data);
  reset(); start(); Fake::partialFailure = true;
  assert(!captureCameraFrame(frame)); assert(!frame.data);
  reset(); start(); Fake::failCaptures = 1;
  assert(captureCameraFrame(frame)); assert(Fake::beginCount == 2 && Fake::endCount == 1);
  cameraFrameRelease(frame);
  reset(); start(); Fake::failCaptures = 1; sCameraDesiredOn = false;
  assert(!captureCameraFrame(frame)); assert(Fake::beginCount == 1 && !gCameraRunning);
  reset(); start(); size_t len = 99;
  auto* tiny = captureFrameAtResolution(CameraFrameSize::QQVGA, 40, &len);
  assert(tiny && len == 8 && Fake::size == CameraFrameSize::VGA && Fake::quality == 12);
  free(tiny);
  Fake::failQuality = true; len = 99;
  assert(!captureFrameAtResolution(CameraFrameSize::QQVGA, 40, &len));
  assert(len == 0 && Fake::size == CameraFrameSize::VGA);
  Fake::failQuality = false; Fake::failRestore = true;
  assert(!captureFrameAtResolution(CameraFrameSize::QQVGA, 40, &len));
  assert(!gCameraRunning && !Fake::running && len == 0);
  reset(); start();
  Fake::controlMask &= ~cameraControlBit(CameraControl::Brightness);
  assert(strstr(cameraIntegerCommand("2", CameraControl::Brightness, -2, 2, &gSettings.cameraBrightness), "unsupported"));
  assert(persistCount == 0 && gSettings.cameraBrightness == 0);
  Fake::controlMask |= cameraControlBit(CameraControl::Brightness); Fake::failControl = true;
  assert(strstr(cameraIntegerCommand("2", CameraControl::Brightness, -2, 2, &gSettings.cameraBrightness), "Error"));
  assert(persistCount == 0);
  Fake::failControl = false;
  assert(!strstr(cameraIntegerCommand("2", CameraControl::Brightness, -2, 2, &gSettings.cameraBrightness), "Error"));
  assert(persistCount == 1 && gSettings.cameraBrightness == 2);
  assert(strstr(cameraIntegerCommand("nonsense", CameraControl::Brightness, -2, 2, &gSettings.cameraBrightness), "Error"));
  assert(persistCount == 1);
  assert(strstr(cmd_cameraquality("oops"), "Error"));
  assert(strstr(cmd_cameraquality("12xyz"), "Error"));
  assert(strstr(cmd_cameraquality("99999999999999999999999999"), "Error"));
  assert(gSettings.cameraQuality == 12 && persistCount == 1);
  Fake::failQuality = true;
  assert(strstr(cmd_cameraquality("0"), "Error")); assert(gSettings.cameraQuality == 12 && persistCount == 1);
  Fake::failQuality = false;
  assert(!strstr(cmd_cameraquality("0"), "Error")); assert(gSettings.cameraQuality == 0 && Fake::quality == 0);
  assert(!strstr(cmd_cameraquality("63"), "Error")); assert(gSettings.cameraQuality == 63 && Fake::quality == 63);
  reset(); cameraWidth = cameraHeight = 0;
  assert(strstr(cmd_camerares("qvga"), "320x240"));
  assert(gSettings.cameraFramesize == 0 && !Fake::running);
  assert(strstr(cmd_camerares(""), "Saved preference: 320x240"));
  assert(strstr(cmd_cameraframesize("11"), "1280x720"));
  assert(gSettings.cameraFramesize == 11);
  int savedCount = persistCount;
  assert(strstr(cmd_cameraframesize("oops"), "Error"));
  assert(strstr(cmd_cameraframesize("12x"), "Error"));
  assert(strstr(cmd_cameraframesize("9999999999999999999999999"), "Error"));
  assert(persistCount == savedCount);
  start(); Fake::failSize = true;
  assert(strstr(cmd_camerares("640x480"), "Error"));
  assert(strstr(cmd_cameraframesize("1"), "Error"));
  assert(gSettings.cameraFramesize == 11 && Fake::size == CameraFrameSize::HD);
  assert(persistCount == savedCount);
  Fake::failSize = false;
  assert(strstr(cmd_camerares("640x480"), "640x480"));
  assert(gSettings.cameraFramesize == 1 && Fake::size == CameraFrameSize::VGA);
  Fake::sizeMask &= ~cameraResolutionBit(CameraFrameSize::UXGA);
  savedCount = persistCount;
  assert(strstr(cmd_camerares("uxga"), "unsupported"));
  assert(persistCount == savedCount);
  reset(); start(); Fake::failEnd = true; stopCamera(); assert(gCameraRunning && Fake::running);
  Fake::failEnd = false; stopCamera(); assert(!gCameraRunning);
  reset(); start(); Fake::blockCapture = true;
  std::atomic<bool> stopped{false};
  std::thread capture([&] { assert(captureCameraFrame(frame)); });
  { std::unique_lock<std::mutex> lock(Fake::gateMutex); Fake::gate.wait(lock, [] { return Fake::enteredCapture; }); }
  auto beforeCaps = std::chrono::steady_clock::now();
  assert(getCameraCapabilities().resolutions & cameraResolutionBit(CameraFrameSize::VGA));
  assert(std::chrono::steady_clock::now() - beforeCaps < std::chrono::milliseconds(100));
  CameraFrame busy;
  assert(!captureCameraFrame(busy)); // no queuing behind another capture
  std::thread stop([&] { stopCamera(); stopped = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(30)); assert(!stopped);
  { std::lock_guard<std::mutex> lock(Fake::gateMutex); Fake::releaseCapture = true; Fake::gate.notify_all(); }
  capture.join(); stop.join(); assert(stopped && !Fake::running);
  assert(frame.data && frame.data[1] == 0xd8); cameraFrameRelease(frame);
  puts("camera core: recovery, bounds, ownership, temporary restore, control persistence and serialization PASS");
}
