// The Python runner inserts unchanged production owner/policy/command code.
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>
#include "System_Power.h"
#if ENABLE_I2C_SYSTEM
#include "System_PollPause.h"
#endif

class String {
  std::string value;
 public:
  String(const char* text = "") : value(text) {}
  String(const std::string& text) : value(text) {}
  size_t length() const { return value.size(); }
  char operator[](size_t i) const { return value[i]; }
  void trim() {
    const auto first = value.find_first_not_of(" \t\r\n");
    value = first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
  }
  // ESP32 Arduino uses signed 32-bit atol. Model its saturation, avoiding a
  // host's 64-bit long turning oversized input into a valid narrowed choice.
  int32_t toInt() const {
    uint32_t n = 0;
    for (char c : value) {
      if (c < '0' || c > '9') break;
      if (n > (uint32_t(INT32_MAX) - uint32_t(c - '0')) / 10) return INT32_MAX;
      n = n * 10 + uint32_t(c - '0');
    }
    return static_cast<int32_t>(n);
  }
};

static std::vector<std::string> trace, output, events, battery;
static int currentTask = 1;
struct StaticSemaphore_t {
  int id = 0, holder = 0, takes = 0, gives = 0;
  bool refuse = false;
};
using SemaphoreHandle_t = StaticSemaphore_t*;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
static std::string lockName(SemaphoreHandle_t lock) {
  return lock->id == 0 ? "gate" : "bus" + std::to_string(lock->id - 1);
}
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
static int xTaskGetCurrentTaskHandle() { return currentTask; }
static int xSemaphoreGetMutexHolder(SemaphoreHandle_t lock) { return lock->holder; }
static int xSemaphoreTake(SemaphoreHandle_t lock, int timeout) {
  assert(timeout == (lock->id == 0 ? 0 : 2000));
  trace.push_back("take:" + lockName(lock));
  ++lock->takes;
#if ENABLE_I2C_SYSTEM
  if (lock->id) assert(pollPaused(0) && pollPaused(1));
#endif
  if (lock->refuse || lock->holder) return 0;
  lock->holder = currentTask;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t lock) {
  assert(lock->holder == currentTask);
  trace.push_back("give:" + lockName(lock));
  ++lock->gives;
  lock->holder = 0;
}
#if ENABLE_I2C_SYSTEM
static StaticSemaphore_t buses[2];
class I2CDeviceManager {
 public:
  static constexpr uint8_t NUM_BUSES = 2;
  static I2CDeviceManager* available;
  static int lookups;
  SemaphoreHandle_t mutexes[2] = {&buses[0], &buses[1]};
  static I2CDeviceManager* existingInstance() { ++lookups; return available; }
  SemaphoreHandle_t getBusMutex(uint8_t i) { assert(i < NUM_BUSES); return mutexes[i]; }
  // Deliberately no lazy getInstance(): clock changes must never construct it.
};
I2CDeviceManager* I2CDeviceManager::available = nullptr;
int I2CDeviceManager::lookups = 0;
static I2CDeviceManager manager;
#endif

static uint32_t current = POWER_CPU_PERFORMANCE_MHZ;
static int halCalls = 0, brightnessCalls = 0, settingWrites = 0;
enum class HalResult { Success, Refuse, Mismatch, MutateThenRefuse, AcceptWithoutChange };
static HalResult halResult = HalResult::Success;
static std::function<void()> halCallback;
static uint32_t getCpuFrequencyMhz() { return current; }
static uint32_t getXtalFrequencyMhz() { return 40; }
static uint32_t getApbFrequency() { return 80000000; }
static bool setCpuFrequencyMhz(uint32_t mhz);
struct { int oledBrightness = 1; } gSettings;
template <typename T, typename U> static void setSetting(T& field, U value) {
  trace.push_back("setting"); ++settingWrites; field = value;
}
void applyOLEDBrightness() { trace.push_back("brightness"); ++brightnessCalls; }
static void systemEventPost(int type, const char* text) {
  assert(type == 1); trace.push_back("event"); events.push_back(text);
}
void batteryLogEvent(const char* text) { trace.push_back("battery"); battery.push_back(text); }
static void broadcastOutput(const char* text) { output.push_back(text); }
static void broadcastPrintf(const char* fmt, ...) {
  char text[256]; va_list args; va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args); va_end(args); output.push_back(text);
}
static bool validateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validateOnly) return "VALID"; } while (0)
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
#define DEBUG_SYSTEMF(...) ((void)0)
#define INFO_SYSTEMF(...) ((void)0)
#define ERROR_SYSTEMF(...) ((void)0)
#define SYSEVT_POWER_MODE_CHANGED 1

// INSERT_PRODUCTION

static bool setCpuFrequencyMhz(uint32_t mhz) {
  assert(sCpuFrequencyMutex->holder == currentTask);
#if ENABLE_I2C_SYSTEM
  assert(pollPaused(0) && pollPaused(1));
  if (I2CDeviceManager::available) {
    for (auto* lock : manager.mutexes) if (lock) assert(lock->holder == currentTask);
  }
#endif
  ++halCalls; trace.push_back("hal:" + std::to_string(mhz));
  if (halCallback) halCallback();
  if (halResult == HalResult::Refuse) return false;
  if (halResult == HalResult::AcceptWithoutChange) return true;
  current = halResult == HalResult::Mismatch ? mhz - 1 : mhz;
  return halResult != HalResult::MutateThenRefuse;
}

static void reset() {
  assert(sCpuFrequencyMutex && sCpuFrequencyMutex->holder == 0);
  sCpuFrequencyMutexStorage = {};
  trace.clear(); output.clear(); events.clear(); battery.clear();
  currentTask = 1; current = POWER_CPU_PERFORMANCE_MHZ; halCalls = 0;
  brightnessCalls = settingWrites = 0; gSettings.oledBrightness = 1;
  halCallback = {}; halResult = HalResult::Success; validateOnly = false;
#if ENABLE_I2C_SYSTEM
  assert(!pollPaused(0) && !pollPaused(1) && !gSensorPollingPaused);
  for (int i = 0; i < 2; ++i) {
    assert(!buses[i].holder); buses[i] = {}; buses[i].id = i + 1;
    manager.mutexes[i] = &buses[i];
  }
  I2CDeviceManager::available = &manager; I2CDeviceManager::lookups = 0;
#endif
}
static void released() {
  assert(sCpuFrequencyMutex->holder == 0);
#if ENABLE_I2C_SYSTEM
  assert(!buses[0].holder && !buses[1].holder);
  assert(!pollPaused(0) && !pollPaused(1) && !gSensorPollingPaused);
#endif
}
static void noPresentation() {
  assert(output.empty() && events.empty() && battery.empty());
  assert(!brightnessCalls && !settingWrites);
}
static void basicOutcomes() {
  reset(); bool changed = true; assert(powerSetCpuFrequencyMhz(current, &changed)); assert(!changed);
  assert(halCalls == 0 && trace == std::vector<std::string>({"take:gate", "give:gate"}));
#if ENABLE_I2C_SYSTEM
  assert(I2CDeviceManager::lookups == 0);
#endif
  noPresentation(); released();

  for (auto result : {HalResult::Success, HalResult::Refuse, HalResult::Mismatch,
                      HalResult::MutateThenRefuse, HalResult::AcceptWithoutChange}) {
    reset(); halResult = result;
    changed = result == HalResult::Refuse;
    assert(powerSetCpuFrequencyMhz(POWER_CPU_BALANCED_MHZ, &changed) == (result == HalResult::Success));
    assert(changed == (result != HalResult::Refuse && result != HalResult::AcceptWithoutChange));
    assert(halCalls == 1); released(); noPresentation();
  }
  reset(); assert(powerSetCpuFrequencyMhz(40)); assert(current == 40); released();
  reset(); halResult = HalResult::Refuse;
  assert(!powerSetCpuFrequencyMhz(0)); assert(halCalls == 1); released();

  reset(); auto* saved = sCpuFrequencyMutex; sCpuFrequencyMutex = nullptr;
  changed = true; assert(!powerSetCpuFrequencyMhz(current, &changed)); assert(!changed);
  assert(!halCalls && trace.empty());
  sCpuFrequencyMutex = saved; released();
  for (int holder : {1, 2}) {
    reset(); sCpuFrequencyMutex->holder = holder;
    changed = true;
    assert(!powerSetCpuFrequencyMhz(current, &changed)); // Gate precedes even a no-op.
    assert(!changed);
    assert(!halCalls && sCpuFrequencyMutex->gives == 0);
    sCpuFrequencyMutex->holder = 0; released();
  }
  for (int callbackTask : {1, 2}) {
    reset();
#if ENABLE_I2C_SYSTEM
    I2CDeviceManager::available = nullptr; // Serialization also exists without buses.
#endif
    halCallback = [callbackTask]() {
      currentTask = callbackTask;
      bool innerChanged = true;
      assert(!powerSetCpuFrequencyMhz(current, &innerChanged)); assert(!innerChanged);
      innerChanged = true;
      assert(!powerSetCpuFrequencyMhz(POWER_CPU_BALANCED_MHZ, &innerChanged)); assert(!innerChanged);
      currentTask = 1;
    };
    assert(powerSetCpuFrequencyMhz(POWER_CPU_BALANCED_MHZ));
    assert(halCalls == 1); released();
  }
}
#if ENABLE_I2C_SYSTEM
static void busOutcomes() {
  reset(); I2CDeviceManager::available = nullptr;
  assert(powerSetCpuFrequencyMhz(40)); assert(!buses[0].takes && !buses[1].takes); released();
  for (int absent = 0; absent < 3; ++absent) {
    reset();
    if (absent != 1) manager.mutexes[0] = nullptr;
    if (absent != 0) manager.mutexes[1] = nullptr;
    assert(powerSetCpuFrequencyMhz(40));
    for (int i = 0; i < 2; ++i) assert(buses[i].takes == (manager.mutexes[i] ? 1 : 0));
    released();
  }
  for (int failBus : {0, 1}) {
    reset(); buses[failBus].refuse = true;
    bool changed = true;
    assert(!powerSetCpuFrequencyMhz(40, &changed)); assert(!changed);
    assert(!halCalls); released(); noPresentation();
    assert(buses[0].takes == 1 && buses[0].gives == (failBus == 1 ? 1 : 0));
    assert(buses[1].takes == (failBus == 1 ? 1 : 0) && buses[1].gives == 0);
  }
  reset(); assert(powerSetCpuFrequencyMhz(40));
  assert(trace == std::vector<std::string>({"take:gate", "take:bus0", "take:bus1", "hal:40",
                                          "give:bus1", "give:bus0", "give:gate"})); released();
  for (int ownBus : {0, 1}) {
    reset(); buses[ownBus].holder = currentTask;
    bool changed = true;
    assert(!powerSetCpuFrequencyMhz(40, &changed)); assert(!changed); assert(!halCalls);
    assert(!buses[0].takes && !buses[1].takes && !buses[0].gives && !buses[1].gives);
    buses[ownBus].holder = 0; released();
  }
  for (bool fail : {false, true}) {
    reset(); pollPause(0); pollPause(0); pollPause();
    buses[1].refuse = fail;
    assert(powerSetCpuFrequencyMhz(40) == !fail);
    assert(pollPaused(0) && pollPaused(1));
    pollResume(); assert(pollPaused(0) && !pollPaused(1));
    pollResume(0); assert(pollPaused(0));
    pollResume(0); released();
  }
  for (auto result : {HalResult::Refuse, HalResult::Mismatch, HalResult::MutateThenRefuse}) {
    reset(); halResult = result; pollPause(1);
    assert(!powerSetCpuFrequencyMhz(40));
    assert(!pollPaused(0) && pollPaused(1));
    pollResume(1); released();
  }
}
#endif
static void consumerOutcomes() {
  reset(); assert(getPowerCpuFrequencyCount() == 3);
  const uint32_t choices[] = {POWER_INTERACTIVE_FLOOR_MHZ, POWER_CPU_BALANCED_MHZ, POWER_CPU_PERFORMANCE_MHZ};
  for (size_t i = 0; i < 3; ++i) assert(getPowerCpuFrequencyMhz(i) == choices[i]);
  assert(!getPowerCpuFrequencyMhz(3) && !isPowerCpuFrequencySupported(40));
  assert(!applyPowerMode(255)); assert(!halCalls); noPresentation();
  for (auto result : {HalResult::Refuse, HalResult::Mismatch, HalResult::MutateThenRefuse}) {
    reset(); halResult = result; assert(!applyPowerMode(POWER_MODE_BALANCED));
    assert(events.empty() && battery.empty() && !brightnessCalls && !settingWrites); released();
    reset(); halResult = result;
    assert(std::string(cmd_cpufreq(String(std::to_string(POWER_CPU_BALANCED_MHZ)))) == "Error: CPU frequency change failed");
    noPresentation(); released();
  }
  reset(); sCpuFrequencyMutex->holder = 2;
  assert(!applyPowerMode(POWER_MODE_PERFORMANCE)); // Busy no-op cannot publish success.
  assert(std::string(cmd_cpufreq(String(std::to_string(current)))) == "Error: CPU frequency change failed");
  noPresentation(); sCpuFrequencyMutex->holder = 0; released();
#if ENABLE_I2C_SYSTEM
  reset(); buses[1].refuse = true;
  assert(!applyPowerMode(POWER_MODE_BALANCED)); assert(!halCalls); noPresentation(); released();
  reset(); buses[1].refuse = true;
  assert(std::string(cmd_cpufreq(String(std::to_string(POWER_CPU_BALANCED_MHZ)))) == "Error: CPU frequency change failed");
  assert(!halCalls); noPresentation(); released();
#endif
  for (uint8_t mode = 0; mode < POWER_MODE_COUNT; ++mode) {
    reset(); current = 40;
    assert(applyPowerMode(mode)); assert(current == getPowerModeActiveCpuFreq(mode));
    assert(events == std::vector<std::string>({getPowerModeName(mode)}));
    assert(battery.size() == 1 && brightnessCalls == 1 && settingWrites == 1);
    assert(trace[trace.size() - 5] == "give:gate"); // Success effects follow release.
    assert(gSettings.oledBrightness == (getPowerModeDisplayBrightness(mode) * 255) / 100);
    released();
  }
  reset(); assert(applyPowerMode(POWER_MODE_PERFORMANCE)); assert(!halCalls && events.size() == 1);
  reset(); gSettings.oledBrightness = 204;
  assert(applyPowerMode(POWER_MODE_BALANCED)); assert(!settingWrites && !brightnessCalls); released();

  for (const char* invalid : {"-80", "+80", "80.0", "80junk", "8 0", "0", "40", "1", "9999999999999999999999999", "4294967376"}) {
    reset(); assert(std::string(cmd_cpufreq(String(invalid))).find("Error:") == 0);
    assert(!halCalls && trace.empty()); noPresentation();
  }
  reset(); validateOnly = true;
  assert(std::string(cmd_cpufreq("160")) == "VALID"); assert(trace.empty()); noPresentation();
  reset(); assert(std::string(cmd_cpufreq(" \t")) == "[System] CPU frequency displayed");
  assert(!halCalls && trace.empty() && events.empty() && battery.empty());
  for (uint32_t choice : choices) {
    bool found = false;
    for (const auto& line : output) if (line == "    " + std::to_string(choice) + " MHz") found = true;
    assert(found);
  }
  reset();
  assert(std::string(cmd_cpufreq(String("  " + std::to_string(POWER_CPU_BALANCED_MHZ) + " \t"))) == "[System] CPU frequency updated");
  assert(current == POWER_CPU_BALANCED_MHZ && battery.size() == 1 && output.size() == 1);
  assert(events.empty() && !settingWrites && !brightnessCalls);
  assert(trace[trace.size() - 2] == "give:gate" && trace.back() == "battery"); released();
  reset();
  assert(std::string(cmd_cpufreq(String(std::to_string(current)))) == "[System] CPU frequency updated");
  assert(!halCalls && battery.size() == 1 && output.size() == 1); released();
}
int main() {
  basicOutcomes();
#if ENABLE_I2C_SYSTEM
  busOutcomes();
#endif
  consumerOutcomes();
  std::printf("PASS CPU owner + command/modes: I2C=%d, max=%d MHz\n", ENABLE_I2C_SYSTEM, POWER_CPU_PERFORMANCE_MHZ);
}
