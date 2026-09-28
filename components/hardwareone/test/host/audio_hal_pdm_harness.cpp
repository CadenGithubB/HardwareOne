// Shipping HAL with only GPIO, I2S, BLE and RTOS boundaries replaced.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#define IRAM_ATTR
#define DRAM_ATTR
#define ENABLE_MICROPHONE 1
#define ENABLE_MICROPHONE_SENSOR 1
#ifndef TEST_MIC_CLOCK_LIMITS
#define TEST_MIC_CLOCK_LIMITS 1
#endif
#if TEST_MIC_CLOCK_LIMITS
#define MIC_PDM_LOW_POWER_MAX_HZ 900000
#define MIC_PDM_STANDARD_MIN_HZ 1100000
#endif
#ifndef TEST_WITH_PINS
#define TEST_WITH_PINS 1
#endif
#if TEST_WITH_PINS
#if TEST_POWERED_MIC
#define MIC_CLK_PIN 22
#define MIC_DATA_PIN 21
#define MIC_POWER_PIN 12
#else
#define MIC_CLK_PIN 42
#define MIC_DATA_PIN 41
#endif
#endif
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
void portENTER_CRITICAL(int*) {}
void portEXIT_CRITICAL(int*) {}
void logStub(const char*, ...) {}
#define WARN_SYSTEMF(...) logStub(__VA_ARGS__)
#define INFO_SYSTEMF(...) logStub(__VA_ARGS__)
using StaticSemaphore_t = int;
using SemaphoreHandle_t = int*;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(int* p) { return p; }
void xSemaphoreTake(int* p, uint32_t) { assert(!*p); *p = 1; }
void xSemaphoreGive(int* p) { assert(*p); *p = 0; }
// Intentionally use a 100 Hz scheduler: the I2S API still takes milliseconds.
uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms / 10; }
int hardwareLockDepth = 0;
struct I2sMicLockGuard {
  explicit I2sMicLockGuard(const char*) { ++hardwareLockDepth; }
  ~I2sMicLockGuard() { assert(hardwareLockDepth > 0); --hardwareLockDepth; }
};
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = 0x107;
const char* esp_err_to_name(int err) { return err == ESP_OK ? "ESP_OK" : "mock error"; }
using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 2;
int powerLevel = 0, powerLevelCalls = 0, powerDirectionCalls = 0;
int failPowerLevel = 0, failPowerDirection = 0;
std::vector<char> powerEvents;
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t value) {
  assert(hardwareLockDepth > 0 && pin == 12 && value == 1);
  ++powerLevelCalls; powerEvents.push_back('L');
  if (failPowerLevel) return ESP_FAIL;
  powerLevel = int(value); return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, int mode) {
  assert(hardwareLockDepth > 0 && pin == 12 && mode == GPIO_MODE_OUTPUT);
  assert(powerLevel == 1 && !powerEvents.empty() && powerEvents.back() == 'L');
  ++powerDirectionCalls; powerEvents.push_back('D');
  return failPowerDirection ? ESP_FAIL : ESP_OK;
}
constexpr int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
bool failWarmupAllocation = false;
void* warmupAllocation = nullptr;
int warmupAllocations = 0, warmupFrees = 0;
void* heap_caps_malloc(size_t bytes, int caps) {
  assert(hardwareLockDepth > 0 && bytes == 2048 && caps == 3 && !warmupAllocation);
  if (failWarmupAllocation) return nullptr;
  ++warmupAllocations;
  warmupAllocation = std::malloc(bytes);
  assert(warmupAllocation);
  std::memset(warmupAllocation, 0x55, bytes);
  return warmupAllocation;
}
void heap_caps_free(void* memory) {
  assert(memory && memory == warmupAllocation && hardwareLockDepth > 0);
  auto* bytes = static_cast<const uint8_t*>(memory);
  for (size_t i = 0; i < 2048; ++i) assert(bytes[i] == 0); // scrubbed before free
  ++warmupFrees; std::free(memory); warmupAllocation = nullptr;
}
using i2s_chan_handle_t = int*;
constexpr int I2S_NUM_0 = 0, I2S_ROLE_MASTER = 1;
constexpr int I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_SLOT_MODE_MONO = 1;
struct i2s_chan_config_t { int id, role, dma_desc_num, dma_frame_num; };
#define I2S_CHANNEL_DEFAULT_CONFIG(id, role) i2s_chan_config_t{id, role, 0, 0}
struct i2s_pdm_rx_slot_config_t { int width, mode; };
#define I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(width, mode) i2s_pdm_rx_slot_config_t{width, mode}
enum { I2S_PDM_DSR_8S = 8, I2S_PDM_DSR_16S = 16 };
struct ClockConfig { uint32_t rate; int dn_sample_mode; };
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate) ClockConfig{rate, I2S_PDM_DSR_8S}
struct I2sGpioConfig { int clk, din; struct { bool clk_inv; } invert_flags; };
struct i2s_pdm_rx_config_t {
  ClockConfig clk_cfg;
  i2s_pdm_rx_slot_config_t slot_cfg;
  I2sGpioConfig gpio_cfg;
};
int channel = 0, newCalls = 0, initCalls = 0, enableCalls = 0;
int disableCalls = 0, deleteCalls = 0, readCalls = 0, zeroReadTimeouts = 0;
int failNew = 0, failInit = 0, failEnable = 0, failCallbacks = 0, readError = ESP_OK;
int callbackRegistrations = 0;
struct i2s_event_data_t { size_t size; };
using I2sCallback = bool (*)(i2s_chan_handle_t, i2s_event_data_t*, void*);
struct i2s_event_callbacks_t { I2sCallback on_recv_q_ovf = nullptr; };
i2s_event_callbacks_t registeredCallbacks;
void* callbackContext = nullptr;
void overflow(unsigned count=1) {
  assert(channel == 2 && registeredCallbacks.on_recv_q_ovf && callbackContext);
  i2s_event_data_t event{2048};
  while(count--) assert(!registeredCallbacks.on_recv_q_ovf(&channel, &event, callbackContext));
}
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t h, const i2s_event_callbacks_t* cb, void* context) {
  assert(hardwareLockDepth > 0 && h == &channel && channel == 1);
  ++callbackRegistrations;
  if (failCallbacks) return ESP_FAIL;
  registeredCallbacks = *cb; callbackContext = context;
  return ESP_OK;
}
size_t readLimit = std::numeric_limits<size_t>::max();
uint32_t lastRate = 0;
int lastDownsample = 0;
std::vector<uint32_t> readTimeouts;
std::function<void()> enableHook, disableHook;
esp_err_t i2s_new_channel(const i2s_chan_config_t* cfg, void* tx, i2s_chan_handle_t* rx) {
  assert(hardwareLockDepth > 0 && !tx && channel == 0);
  assert(cfg->id == 0 && cfg->role == I2S_ROLE_MASTER);
  assert(cfg->dma_desc_num == 4 && cfg->dma_frame_num == 1024);
#if TEST_POWERED_MIC
  assert(powerLevel == 1 && powerEvents.back() == 'D');
#endif
  ++newCalls;
  if (failNew) return ESP_FAIL;
  channel = 1; *rx = &channel; return ESP_OK;
}
esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t h, const i2s_pdm_rx_config_t* cfg) {
  assert(hardwareLockDepth > 0 && h == &channel && channel == 1);
#if TEST_WITH_PINS
  assert(cfg->gpio_cfg.clk == MIC_CLK_PIN && cfg->gpio_cfg.din == MIC_DATA_PIN);
#endif
  assert(!cfg->gpio_cfg.invert_flags.clk_inv);
  assert(cfg->slot_cfg.width == 16 && cfg->slot_cfg.mode == 1);
  ++initCalls; lastRate = cfg->clk_cfg.rate; lastDownsample = cfg->clk_cfg.dn_sample_mode;
  return failInit ? ESP_FAIL : ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t h) {
  assert(hardwareLockDepth > 0 && h == &channel && channel == 1);
  ++enableCalls;
  if (failEnable) return ESP_FAIL;
  channel = 2;
  if (enableHook) enableHook();
  return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t h) {
  assert(hardwareLockDepth > 0 && h == &channel && channel == 2);
  ++disableCalls;
  if (disableHook) disableHook();
  channel = 1; return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t h) {
  assert(hardwareLockDepth > 0 && h == &channel && channel == 1);
  ++deleteCalls; channel = 0; registeredCallbacks = {}; callbackContext = nullptr; return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t h, void* out, size_t cap,
                           size_t* bytes, uint32_t timeoutMs) {
  assert(hardwareLockDepth > 0 && h == &channel && channel == 2);
  ++readCalls; readTimeouts.push_back(timeoutMs);
  if (zeroReadTimeouts > 0) { --zeroReadTimeouts; *bytes = 0; return ESP_ERR_TIMEOUT; }
  *bytes = std::min(cap, readLimit);
  assert(*bytes % sizeof(int16_t) == 0);
  auto samples = static_cast<int16_t*>(out);
  for (size_t n = 0; n < *bytes / sizeof(int16_t); ++n) samples[n] = int16_t(n + 17);
  return readError;
}
bool g2Connected = false, g2Feed = false;
uint32_t g2Errors = 0;
std::function<void()> g2MetricHook;
uint32_t g2MicAfeIntegrityErrors() {
  const uint32_t count = g2Errors;
  if (g2MetricHook) { auto hook = std::move(g2MetricHook); g2MetricHook = {}; hook(); }
  return count;
}
int g2ControlCalls = 0;
bool g2LeftConnected() { return g2Connected; }
bool g2MicStreamEnable(bool) { ++g2ControlCalls; return g2Connected; }
bool g2MicSetAfeFeedActive(bool on, uint32_t = 0) { if (on && !g2Feed) g2Errors = 0; g2Feed = on; return true; }
void g2MicPauseAfeFeed(bool) {}
void g2MicLinkFastRelease() {}
size_t g2MicReadPcmSamples(int16_t* out, size_t cap, uint32_t) {
  if (!g2Feed || !cap) return 0;
  out[0] = 999; return 1;
}
size_t g2MicTrimAfeRingToNewest(size_t) { return 0; }

// INSERT_HAL_HEADER
// INSERT_HAL_SOURCE

struct { std::string micSource = "auto"; } gSettings;
// INSERT_SOURCE_PREFERENCES

int expectedDownsample(uint32_t rate) {
#if TEST_MIC_CLOCK_LIMITS
  return uint64_t(rate) * 64 > 900000 && uint64_t(rate) * 64 < 1100000
      ? I2S_PDM_DSR_16S : I2S_PDM_DSR_8S;
#else
  (void)rate;
  return I2S_PDM_DSR_8S;
#endif
}
void expectIdle() {
  assert(!audioCaptureBusy() && !audioCaptureActive() && !audioCaptureOwnedBy("mic"));
  assert(!audioCaptureOwner()[0] && !gPdmRx && channel == 0 && hardwareLockDepth == 0);
  assert(!warmupAllocation && warmupAllocations == warmupFrees);
}
int main() {
  static_assert(AUDIO_SRC_LOCAL_PDM == 1 && AUDIO_SRC_G2_LEFT == 2);
  static_assert(AUDIO_HAL_BITS == 16 && AUDIO_HAL_CHANNELS == 1);
  assert(audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) && !audioSourceAvailable(AUDIO_SRC_G2_LEFT));
  AudioSource sources[3] = {};
  assert(audioListAvailableSources(sources, 3) == 1 && sources[0] == AUDIO_SRC_LOCAL_PDM);
  assert(!audioSetSource(AUDIO_SRC_G2_LEFT));
  int16_t samples[8] = {};
  assert(!audioReadPcm(samples, 8, 55));
  uint32_t overruns = 0xDEADBEEF;
  assert(!audioCaptureOverruns("mic", &overruns) && overruns == 0xDEADBEEF);
  enableHook = [] { overflow(7); }; // intentional warmup is excluded
  assert(audioCaptureStart("mic") && lastRate == 16000);
  enableHook = {};
  assert(audioCaptureOverruns("mic", &overruns) && overruns == 0);
  assert(!audioCaptureOverruns(nullptr, &overruns) && !audioCaptureOverruns("", &overruns));
  assert(!audioCaptureOverruns("mic", nullptr));
  overflow(3);
  assert(audioCaptureOverruns("mic", &overruns) && overruns == 3);
  assert(!audioCaptureOverruns("sr", &overruns) && overruns == 3);
  assert(lastDownsample == expectedDownsample(16000));
  assert(audioGetSource() == AUDIO_SRC_LOCAL_PDM && audioCaptureOwnedBy("mic"));
  assert(readTimeouts.size() == 3);
  for (uint32_t timeout : readTimeouts) assert(timeout == 100);
  const int allocations = newCalls;
  assert(audioCaptureStart("mic", 48000) && newCalls == allocations && lastRate == 16000);
  assert(audioCaptureOverruns("mic", &overruns) && overruns == 3); // idempotent start kept baseline
  assert(!audioCaptureStart("sr") && !audioSetSource(AUDIO_SRC_NONE));
  audioCaptureStop("sr"); assert(audioCaptureOwnedBy("mic"));
  assert(!audioReadPcm(nullptr, 8, 1) && !audioReadPcm(samples, 0, 1));
  readLimit = 6;
  std::fill(std::begin(samples), std::end(samples), int16_t(-123));
  assert(audioReadPcm(samples, 8, 257) == 3 && readTimeouts.back() == 257);
  assert(samples[0] == 17 && samples[2] == 19 && samples[3] == -123);
  readError = ESP_ERR_TIMEOUT; readLimit = 0;
  assert(audioReadPcm(samples, 8, 9) == 0 && readTimeouts.back() == 9);
  // IDF can copy complete PCM samples before a later DMA-buffer wait expires.
  readLimit = 6;
  std::fill(std::begin(samples), std::end(samples), int16_t(-123));
  assert(audioReadPcm(samples, 8, 100) == 3 && readTimeouts.back() == 100);
  assert(samples[0] == 17 && samples[2] == 19 && samples[3] == -123);
  readError = ESP_FAIL;
  assert(audioReadPcm(samples, 8, 100) == 0); // only timeout permits partial delivery
  readError = ESP_OK; readLimit = std::numeric_limits<size_t>::max();
  disableHook = [] { uint32_t count = 77; assert(!audioCaptureOverruns("mic", &count) && count == 77); assert(audioCaptureBusy() && !audioCaptureActive()); assert(!audioCaptureStart("sr")); };
  audioCaptureStop("mic"); disableHook = {}; expectIdle();
  const int deletes = deleteCalls;
  audioCaptureStop("mic"); assert(deleteCalls == deletes);
#if TEST_POWERED_MIC
  assert(powerLevel == 1 && powerLevelCalls == 1 && powerDirectionCalls == 1);
  failPowerLevel = 1;
  assert(!audioCaptureStart("mic") && newCalls == allocations); expectIdle();
  failPowerLevel = 0; failPowerDirection = 1;
  assert(!audioCaptureStart("mic") && newCalls == allocations); expectIdle();
  failPowerDirection = 0;
#else
  assert(powerLevelCalls == 0 && powerDirectionCalls == 0);
#endif
  // Each partial startup releases the lease and channel; the next attempt works.
  for (int stage = 0; stage < 4; ++stage) {
    failNew = stage == 0; failInit = stage == 1; failEnable = stage == 2; failCallbacks = stage == 3;
    assert(!audioCaptureStart("mic", 22050)); expectIdle();
    failNew = failInit = failEnable = failCallbacks = 0;
    assert(audioCaptureStart("mic", 22050) && lastRate == 22050);
    assert(audioCaptureOverruns("mic", &overruns) && overruns == 0);
    audioCaptureStop("mic"); expectIdle();
  }
  // Bounded startup refuses OOM, a partial DMA block, a permanent timeout,
  // or a driver error. Every allocated warmup buffer is scrubbed and freed.
  failWarmupAllocation = true;
  const int readsBeforeOOM = readCalls;
  assert(!audioCaptureStart("mic") && readCalls == readsBeforeOOM); expectIdle();
  failWarmupAllocation = false;
  readLimit = 0; readError = ESP_ERR_TIMEOUT;
  assert(!audioCaptureStart("mic")); expectIdle();
  readError = ESP_FAIL; readLimit = 2048;
  assert(!audioCaptureStart("mic")); expectIdle();
  readError = ESP_ERR_TIMEOUT; readLimit = 256;
  assert(!audioCaptureStart("mic")); expectIdle();
  readError = ESP_OK; readLimit = 0;
  assert(!audioCaptureStart("mic")); expectIdle();
  readError = ESP_ERR_TIMEOUT; readLimit = std::numeric_limits<size_t>::max();
  assert(audioCaptureStart("mic")); // complete valid buffer even with timeout
  assert(audioCaptureOverruns("mic", &overruns) && overruns == 0);
  audioCaptureStop("mic"); expectIdle();
  readError = ESP_OK; zeroReadTimeouts = 2;
  assert(audioCaptureStart("mic")); // low sample-rate DMA waits can time out
  audioCaptureStop("mic"); expectIdle();
  enableHook = [] { audioCaptureStop("mic"); assert(!audioCaptureActive() && audioCaptureBusy()); };
  assert(!audioCaptureStart("mic")); enableHook = {}; expectIdle();
  // The nearest integer PCM rates on both sides of each mic clock boundary
  // verify strict comparisons; ordinary 8/16/48 kHz stay at their actual rates.
  for (uint32_t rate : {8000u, 14062u, 14063u, 16000u, 17187u, 17188u, 48000u}) {
    assert(audioCaptureStart("mic", rate) && lastRate == rate);
    assert(lastDownsample == expectedDownsample(rate));
    audioCaptureStop("mic"); expectIdle();
  }
  // Source IDs/preferences and native G2 semantics remain unchanged.
  g2Connected = true;
  assert(audioListAvailableSources(sources, 3) == 2 && sources[1] == AUDIO_SRC_G2_LEFT);
  assert(audioSetSource(AUDIO_SRC_G2_LEFT) && audioCaptureStart("mic", 48000));
  assert(audioCaptureOverruns("mic", &overruns) && overruns == 0);
  g2Errors = 11;
  assert(audioCaptureOverruns("mic", &overruns) && overruns == 11);
  g2MetricHook = [] { audioCaptureStop("mic"); assert(audioCaptureStart("mic")); };
  overruns = 101;
  assert(!audioCaptureOverruns("mic", &overruns) && overruns == 101); // same owner, new generation
  assert(audioCaptureOverruns("mic", &overruns) && overruns == 0);
  g2Errors = 5;
  g2MetricHook = [] { audioCaptureStop("mic"); assert(audioSetSource(AUDIO_SRC_LOCAL_PDM)); assert(audioCaptureStart("sr")); };
  assert(!audioCaptureOverruns("mic", &overruns)); // source/owner changed mid-snapshot
  assert(audioCaptureOverruns("sr", &overruns) && overruns == 0);
  audioCaptureStop("sr"); assert(audioSetSource(AUDIO_SRC_G2_LEFT) && audioCaptureStart("mic"));
  const int readsBeforeG2 = readCalls;
  assert(audioReadPcm(samples, 8, 10) == 1 && samples[0] == 999 && readCalls == readsBeforeG2);
  audioCaptureStop("mic"); expectIdle();
  assert(audioSetSource(AUDIO_SRC_LOCAL_PDM));
  const int g2Writes = g2ControlCalls;
  assert(audioCaptureStartG2Native("native", 7));
  assert(audioGetSource() == AUDIO_SRC_G2_LEFT && !audioCaptureStart("mic"));
  g2Errors = 9;
  assert(audioCaptureOverruns("native", &overruns) && overruns == 9);
  audioCaptureStop("native"); expectIdle();
  assert(audioGetSource() == AUDIO_SRC_LOCAL_PDM && g2ControlCalls == g2Writes);
  for (auto applyPreference : {applyMicPreference, applySrPreference}) {
    // Auto must forget a previously chosen G2 source even while G2 is present.
    assert(audioSetSource(AUDIO_SRC_G2_LEFT));
    gSettings.micSource = "auto"; applyPreference();
    assert(audioGetSource() == AUDIO_SRC_NONE);
    assert(audioCaptureStart("mic") && audioGetSource() == AUDIO_SRC_LOCAL_PDM);
    audioCaptureStop("mic"); expectIdle();
    gSettings.micSource = "g2"; applyPreference();
    assert(audioGetSource() == AUDIO_SRC_G2_LEFT);
    g2Connected = false; applyPreference();
    assert(audioGetSource() == AUDIO_SRC_NONE);
    assert(audioCaptureStart("mic") && audioGetSource() == AUDIO_SRC_LOCAL_PDM);
    audioCaptureStop("mic"); expectIdle();
    g2Connected = true;
    gSettings.micSource = "pdm"; applyPreference();
    assert(audioGetSource() == AUDIO_SRC_LOCAL_PDM);
  }
  g2Connected = false;
  assert(!audioCaptureStartG2Native("native", 7)); expectIdle();
#if TEST_POWERED_MIC
  assert(powerLevel == 1); // shared rail retained across all starts/stops/failures
#endif
  std::puts(TEST_POWERED_MIC ? "P4 PDM HAL regression passed" : "XIAO PDM HAL regression passed");
}
