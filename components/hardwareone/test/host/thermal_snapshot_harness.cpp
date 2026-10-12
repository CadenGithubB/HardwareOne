#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define CACHE_MUTEX_TIMEOUT_MS 100
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
template <typename... Args> static void debugLog(const char*, Args...) {}
#define DEBUG_THERMAL_POLLINGF(...) debugLog(__VA_ARGS__)
#define DEBUG_THERMAL_LIFECYCLEF(...) debugLog(__VA_ARGS__)
#define DEBUG_PERFORMANCEF(...) debugLog(__VA_ARGS__)
#define DISPLAY_COLOR_WHITE 1
#define OLED_CONTENT_START_Y 8
using TickType_t = uint32_t;
struct Semaphore { bool held = false; } semaphore;
using SemaphoreHandle_t = Semaphore*;
static bool admit = true, allocationAvailable = true;
// >= 0: admit/allocate that many more times, then refuse (one-shot faults).
static int admitsBeforeRefusal = -1, allocationsBeforeFailure = -1;
static std::vector<uint32_t> waits;
static bool isHeldByCurrentTask(SemaphoreHandle_t m) { return m && m->held; }
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t ms) {
  waits.push_back(ms);
  if (!m || m->held || !admit) return 0;
  if (admitsBeforeRefusal == 0) return 0;
  if (admitsBeforeRefusal > 0) --admitsBeforeRefusal;
  m->held = true; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(m && m->held); m->held = false; }
static struct { int thermalRotation = 0; float oledThermalScale = 2.5f; int thermalUpscaleFactor = 1; } gSettings;
static bool gThermalRunning = true, gThermalConnected = true;
static unsigned long millis() { return 12345; }
static std::vector<void*> allocations;
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char*) {
  assert(!semaphore.held);
  if (!allocationAvailable || allocationsBeforeFailure == 0) return nullptr;
  if (allocationsBeforeFailure > 0) --allocationsBeforeFailure;
  void* result = std::malloc(size); assert(result); allocations.push_back(result); return result;
}
static void trackedFree(void* ptr) {
  assert(!semaphore.held);
  if (!ptr) return;
  const auto found = std::find(allocations.begin(), allocations.end(), ptr);
  assert(found != allocations.end()); allocations.erase(found); std::free(ptr);
}
static void ps_free(void* ptr) { trackedFree(ptr); }
#define free trackedFree

// INSERT_TYPES
// INSERT_STATE

static void renderingAllowed() {
#if !TEST_BASELINE
  assert(!semaphore.held);
#endif
}
static int trackedSnprintf(char* dst, size_t size, const char* format, ...) {
  renderingAllowed();
  va_list args; va_start(args, format);
  int result = std::vsnprintf(dst, size, format, args); va_end(args); return result;
}
#define snprintf trackedSnprintf
struct Display {
  std::string text;
  uint64_t pixels = 1469598103934665603ULL;
  unsigned pixelCount = 0;
  void setTextSize(int) { renderingAllowed(); }
  void setCursor(int x, int y) { renderingAllowed(); text += "@" + std::to_string(x) + "," + std::to_string(y); }
  void print(const char* value) { renderingAllowed(); text += value; }
  void print(int value) { renderingAllowed(); text += std::to_string(value); }
  void println(const char* value = "") { print(value); text += '\n'; }
  void drawPixel(int x, int y, int color) {
    renderingAllowed(); assert(color == DISPLAY_COLOR_WHITE);
    pixels ^= static_cast<unsigned>(x + 512 * y); pixels *= 1099511628211ULL; ++pixelCount;
  }
} display;
static Display* oledDisplay = &display;
static int driverDestroyed = 0, scratchReset = 0, exited = 0, delays = 0;
static void* gMLX90640 = reinterpret_cast<void*>(0x1234);
static void ps_delete(void*) { assert(!semaphore.held); ++driverDestroyed; }
void resetThermalFrameBuffers() { assert(!semaphore.held); ++scratchReset; }
static bool stopAfterDelay = false;
struct StopPending {};
static void vTaskDelay(uint32_t ms) {
  assert(ms == 10 && !semaphore.held); ++delays;
  if (stopAfterDelay) throw StopPending{};
  admit = true;
}
#define SENSOR_TASK_EXIT(sensor, ...) do { assert(!semaphore.held); ++exited; return; } while(false)

[[maybe_unused]] static bool gThermalInitRequested=false, gThermalInitDone=false, gThermalInitResult=false;
[[maybe_unused]] static bool gThermalPendingFirstFrame=false;
[[maybe_unused]] static unsigned long gThermalArmAtMs=0;

// INSERT_FUNCTIONS

static void releaseFrames() {
  trackedFree(DATA.thermalFrame); trackedFree(DATA.thermalInterpolated);
  DATA.thermalFrame = nullptr; DATA.thermalInterpolated = nullptr;
}
static void reset(bool interpolation = false) {
  releaseFrames(); DATA = {}; semaphore = {}; MUTEX = &semaphore;
  admit = allocationAvailable = true; gThermalRunning = gThermalConnected = true;
  admitsBeforeRefusal = allocationsBeforeFailure = -1;
  waits.clear(); gSettings = {}; gSettings.oledThermalScale = 2.5f; display = {};
  gMLX90640 = reinterpret_cast<void*>(0x1234);
  driverDestroyed = scratchReset = exited = delays = 0; stopAfterDelay = false;
  DATA.thermalFrame = static_cast<int16_t*>(ps_alloc(768 * sizeof(int16_t), AllocPref::PreferPSRAM, "fixture"));
  for (int i = 0; i < 768; ++i) DATA.thermalFrame[i] = static_cast<int16_t>((i % 257 - 128) * 13);
  DATA.thermalMinTemp = 2.5f; DATA.thermalMaxTemp = 9.5f; DATA.thermalAvgTemp = 5.5f;
  DATA.thermalLastUpdate = 98765; DATA.thermalSeq = 27; DATA.thermalDataValid = true;
  if (interpolation) {
    DATA.thermalInterpolated = static_cast<float*>(ps_alloc(3072 * sizeof(float), AllocPref::PreferPSRAM, "fixture"));
    DATA.thermalInterpolatedWidth = 64; DATA.thermalInterpolatedHeight = 48;
    for (int i = 0; i < 3072; ++i) DATA.thermalInterpolated[i] = (i % 157 - 78) / 4.0f;
  }
}
static void waited(uint32_t ms) {
  assert(waits.size() == 1 && waits[0] == ms && !semaphore.held); waits.clear();
}
using Builder = int (*)(char*, size_t);
static std::string output(Builder builder) {
  std::vector<char> buf(20000);
  const int n = builder(buf.data(), buf.size());
  assert(n > 0 && n == static_cast<int>(std::strlen(buf.data())));
  return buf.data();
}
static void emit(const std::string& text) { std::cout << text << '\n'; }
static void commonOutputs() {
  for (bool interpolation : {false, true}) {
    for (int rotation = 0; rotation < 4; ++rotation) {
      reset(interpolation); gSettings.thermalRotation = rotation;
      emit(output(thermalBuildDataJSON)); waited(100);
      emit(output(buildThermalDataJSONInteger)); waited(100);
      const auto summary = output(thermalBuildSummaryJSON); waited(50); emit(summary);
      assert(summary.find("\"min\":-16.6") != std::string::npos);
      assert(summary.find("\"max\":16.6") != std::string::npos);
      displayThermalVisual(); waited(10);
      emit(display.text + ":pixels=" + std::to_string(display.pixelCount) + ":hash=" + std::to_string(display.pixels));
      assert(allocations.size() == (interpolation ? 2u : 1u));
    }
  }
  for (int state = 0; state < 3; ++state) {
    reset();
    if (state == 0) { releaseFrames(); }
    if (state == 1) DATA.thermalDataValid = false;
    if (state == 2) admit = false;
    emit(output(thermalBuildDataJSON)); waited(100);
    emit(output(buildThermalDataJSONInteger)); waited(100);
    emit(output(thermalBuildSummaryJSON)); waited(50);
    displayThermalVisual(); waited(10); emit(display.text);
  }
  reset(); gThermalRunning = false;
  displayThermalVisual(); assert(waits.empty()); emit(display.text);
}

#if !TEST_BASELINE
static void bufferBounds(Builder builder) {
  const auto expected = output(builder); waits.clear();
  // Every capacity through the full payload catches truncation at the header,
  // individual values and final bracket without reproducing serialization.
  for (size_t capacity = 0; capacity <= expected.size() + 1; ++capacity) {
    std::vector<char> guarded(capacity + 2, 'Z');
    const int n = builder(guarded.data() + 1, capacity);
    assert(guarded.front() == 'Z' && guarded.back() == 'Z');
    if (capacity <= expected.size()) assert(n == 0);
    else assert(n == static_cast<int>(expected.size()) && std::string(guarded.data() + 1) == expected);
  }
  assert(builder(nullptr, 4) == 0); waits.clear();
}
static void ownerContracts() {
  reset(); ThermalCache out;
  assert(thermalCacheSnapshot(out)); waited(10);
  assert(out.thermalDataValid && out.thermalFrameAvailable && out.thermalSeq == 27);
  assert(out.thermalFrameWidth == 32 && out.thermalFrameHeight == 24);
  out.thermalMinTemp = 99; assert(DATA.thermalMinTemp == 2.5f);
  assert(thermalCacheStorageBytes() == sizeof(DATA) + sizeof(MUTEX));
  for (uint32_t ms : {5, 10, 50, 100}) {
    admit = true; assert(thermalCacheSnapshot(out, ms)); waited(ms);
    admit = false; assert(!thermalCacheSnapshot(out, ms)); waited(ms);
    assert(!out.thermalDataValid && !out.thermalFrameAvailable && out.thermalFrameWidth == 0 && !out.thermalSeq);
  }
  reset(); MUTEX = nullptr; out.thermalDataValid = true;
  assert(!thermalCacheSnapshot(out) && !out.thermalDataValid && waits.empty());
  reset(true);
  std::vector<int16_t> raw(768, -2222), frame(3072, -3333);
  assert(!thermalFrameSnapshot(out, nullptr, 768) && waits.empty());
  assert(!thermalFrameSnapshot(out, raw.data(), 767) && waits.empty());
  assert(!thermalFrameSnapshot(out, raw.data(), raw.size(), true)); waited(10);
  assert(std::all_of(raw.begin(), raw.end(), [](int16_t v) { return v == -2222; }));
  assert(!out.thermalDataValid && !out.thermalFrameAvailable);
  assert(thermalFrameSnapshot(out, raw.data(), raw.size())); waited(10);
  assert(!out.thermalFrameInterpolated && raw[0] == -1664 && raw[256] == 1664);
  assert(thermalFrameSnapshot(out, frame.data(), frame.size(), true, 50)); waited(50);
  assert(out.thermalFrameInterpolated && out.thermalFrameWidth == 64 && out.thermalFrameHeight == 48);
  assert(frame[0] == -19 && frame[1] == -19 && frame[156] == 19);
  const auto copied = frame;
  admit = false; assert(!thermalResetCacheForStart()); waited(100);
  assert(DATA.thermalFrame && DATA.thermalInterpolated && DATA.thermalDataValid);
  // Start retires the previous run's buffers and allocates this run's (1x by
  // default), so the polling task never allocates inside its transaction.
  admit = true; assert(thermalResetCacheForStart());
  assert((waits == std::vector<uint32_t>{100, 100}) && !semaphore.held); waits.clear();
  assert(allocations.size() == 1 && allocations[0] == DATA.thermalFrame);
  assert(!DATA.thermalInterpolated && !DATA.thermalInterpolatedWidth && !DATA.thermalInterpolatedHeight);
  assert(frame == copied);  // independent data survives retirement/free of both live buffers
  assert(!DATA.thermalDataValid && !DATA.thermalSeq);
  // Until the first frame the new buffer reads as an invalid, zeroed frame.
  assert(thermalFrameSnapshot(out, raw.data(), raw.size())); waited(10);
  assert(out.thermalFrameAvailable && !out.thermalDataValid);
  assert(std::all_of(raw.begin(), raw.end(), [](int16_t v) { return v == 0; }));

  // 2x upscale allocates the interpolation buffer at start too.
  reset(true); gSettings.thermalUpscaleFactor = 2;
  assert(thermalResetCacheForStart()); waits.clear();
  assert(allocations.size() == 2 && DATA.thermalFrame && DATA.thermalInterpolated);
  assert(DATA.thermalInterpolatedWidth == 64 && DATA.thermalInterpolatedHeight == 48);
  // A missing interpolation buffer falls back to 1x; the start still succeeds.
  reset(true); gSettings.thermalUpscaleFactor = 2; allocationsBeforeFailure = 1;
  assert(thermalResetCacheForStart()); waits.clear();
  assert(allocations.size() == 1 && DATA.thermalFrame && !DATA.thermalInterpolated);
  assert(!DATA.thermalInterpolatedWidth && !DATA.thermalInterpolatedHeight);
  // No frame buffer fails the start, with the old buffers already released.
  reset(true); allocationAvailable = false;
  assert(!thermalResetCacheForStart()); waited(100);
  assert(allocations.empty() && !DATA.thermalFrame && !DATA.thermalInterpolated && !DATA.thermalDataValid);
  // A refused install lock frees the new buffers and fails the start.
  reset(true); gSettings.thermalUpscaleFactor = 2; admitsBeforeRefusal = 1;
  assert(!thermalResetCacheForStart());
  assert((waits == std::vector<uint32_t>{100, 100}) && !semaphore.held); waits.clear();
  assert(allocations.empty() && !DATA.thermalFrame && !DATA.thermalInterpolated);

  reset(); admit = false;
  assert(!thermalInvalidateCache()); waited(50); assert(DATA.thermalDataValid && DATA.thermalSeq == 27);
  admit = true; assert(thermalInvalidateCache()); waited(50);
  assert(!DATA.thermalDataValid && !DATA.thermalSeq && DATA.thermalFrame);
  reset(); gThermalRunning = false; admit = false; stopAfterDelay = true;
  try { runStopBranch(); assert(false); } catch (const StopPending&) {}
  waited(50); assert(!exited && !driverDestroyed && !scratchReset && DATA.thermalDataValid);
  admit = true; stopAfterDelay = false; runStopBranch(); waited(50);
  assert(exited == 1 && driverDestroyed == 1 && scratchReset == 1 && !gMLX90640);
  assert(!DATA.thermalDataValid && !DATA.thermalSeq && DATA.thermalFrame);
  reset(); gThermalRunning = false; admit = false; runStopBranch();
  assert((waits == std::vector<uint32_t>{50, 50}) && delays == 1 && exited == 1);

  for (Builder builder : {thermalBuildDataJSON, buildThermalDataJSONInteger, thermalBuildSummaryJSON}) {
    reset(); bufferBounds(builder);
    reset(true); bufferBounds(builder);
    reset(); admit = false; bufferBounds(builder);
    reset(); releaseFrames(); bufferBounds(builder);
    reset(); allocationAvailable = false;
    assert(output(builder).find("unavailable") != std::string::npos || builder == thermalBuildSummaryJSON);
    assert(waits.empty() && allocations.size() == 1);
  }
}
#endif

int main() {
  commonOutputs();
#if !TEST_BASELINE
  ownerContracts();
#endif
  releaseFrames(); assert(allocations.empty());
}
