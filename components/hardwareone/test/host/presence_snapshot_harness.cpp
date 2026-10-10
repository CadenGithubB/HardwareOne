#include <array>
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#define ENABLE_PRESENCE_SENSOR 1
#define ESP_OK 0
#define LOG_PRESENCE 1u
#define DISPLAY_COLOR_WHITE 1
#define CACHE_MUTEX_TIMEOUT_MS 100
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define INFO_PRESENCE_LIFECYCLEF(...) ((void)0)
#define DEBUG_PRESENCE_LIFECYCLEF(...) ((void)0)
#define ERROR_PRESENCEF(...) ((void)0)
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validationOnly) return "VALID"; } while (false)
#define SENSOR_JSON_NOBUF "no-buffer"
#define SENSOR_JSON_UNAVAILABLE "unavailable"
#define I2C_ADDR_PRESENCE 0x5A
using String = std::string;
using TickType_t = uint32_t;
struct Semaphore { bool held = false; } semaphore;
using SemaphoreHandle_t = Semaphore*;
static bool admit = true, memoryAvailable = true, createMutex = true, busHeld = false;
static std::vector<uint32_t> waits;
static bool isHeldByCurrentTask(SemaphoreHandle_t m) { return m && m->held; }
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t ms) {
  assert(!busHeld);  // A sensor cache wait must not hold the shared I2C bus.
  waits.push_back(ms);
  if (!m || m->held || !admit) return 0;
  m->held = true;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(m && m->held); m->held = false; }
static SemaphoreHandle_t xSemaphoreCreateMutex() { return createMutex ? &semaphore : nullptr; }
static bool checkMemoryAvailable(const char*, void*) { return memoryAvailable; }

// INSERT_TYPES
// INSERT_STATE

bool gPresenceRunning = true, gPresenceConnected = true;
static void* gPresenceTaskHandle = reinterpret_cast<void*>(0x1234);
static struct { bool mqttPublishPresence = true; } gSettings;
static bool validationOnly = false, debugAvailable = true;
static char debugBuffer[1024];
static bool argWantsJson(const String& arg) { return arg == "json"; }
static bool ensureDebugBuffer() { return debugAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
static std::string response;
static void sendJsonResponse(void*, const char* text, int len = -1) {
  assert(!semaphore.held && !busHeld);
  response.assign(text, len < 0 ? std::strlen(text) : static_cast<size_t>(len));
}
static void renderingAllowed() {
#if !TEST_BASELINE
  assert(!semaphore.held && !busHeld);
#endif
}
struct FakeDisplay {
  std::string text;
  void clearDisplay() { renderingAllowed(); text.clear(); }
  void setTextSize(int) { renderingAllowed(); }
  void setTextColor(int) { renderingAllowed(); }
  void setCursor(int, int) { renderingAllowed(); }
  void print(const char* value) { renderingAllowed(); text += value; }
  void print(int value) { renderingAllowed(); text += std::to_string(value); }
  void print(float value, int precision) {
    renderingAllowed(); char out[64];
    std::snprintf(out, sizeof(out), "%.*f", precision, static_cast<double>(value)); text += out;
  }
  void println(const char* value = "") { print(value); text += '\n'; }
} display;
static FakeDisplay* oledDisplay = &display;
static bool oledConnected = true;
static int trackedSnprintf(char* dst, size_t cap, const char* format, ...) {
  renderingAllowed();
  va_list args;
  va_start(args, format);
  const int result = std::vsnprintf(dst, cap, format, args);
  va_end(args);
  return result;
}
#define snprintf trackedSnprintf

// The production poll owns register ordering, conversion, flags and debounce.
// Only the register/transaction boundary supplies deterministic device bytes.
struct TwoWire {} wire;
static bool busAvailable = true, transactionAvailable = true;
static int failedRegister = -1, transactions = 0;
static unsigned long nowMs = 56789;
static std::array<int16_t, 256> registers{};
static bool presenceResolveBus(uint8_t* outBus, TwoWire** outWire) {
  if (!busAvailable) return false;
  *outBus = 1; *outWire = &wire; return true;
}
template <typename F>
static bool i2cTaskWithTimeout(uint8_t bus, uint8_t addr, int hz, int timeout, F callback) {
  assert(bus == 1 && addr == I2C_ADDR_PRESENCE && hz == 100000 && timeout == 100);
  assert(!semaphore.held && !busHeld); ++transactions;
  if (!transactionAvailable) return false;
  busHeld = true;
  const bool result = callback();
  busHeld = false;
  return result;
}
static bool readRegister(TwoWire& w, uint8_t reg, uint8_t* value) {
  assert(&w == &wire && busHeld);
  if (reg == failedRegister) return false;
  *value = static_cast<uint8_t>(registers[reg]); return true;
}
static bool readInt16(TwoWire& w, uint8_t reg, int16_t* value) {
  assert(&w == &wire && busHeld);
  if (reg == failedRegister) return false;
  *value = registers[reg]; return true;
}
static unsigned long millis() { return nowMs; }
static constexpr int SYSEVT_PRESENCE_DETECTED = 1, SYSEVT_PRESENCE_CLEARED = 2, SYSEVT_MOTION_DETECTED = 3;
static std::vector<std::string> events;
static void systemEventPost(int event, const char* detail = "") {
  assert(!semaphore.held && !busHeld);
  events.push_back(std::to_string(event) + ":" + detail);
}
static int exited = 0, delays = 0;
static bool stopAfterDelay = false;
struct StopPending {};
static void vTaskDelay(TickType_t ticks) {
  assert(ticks == 10 && !semaphore.held); ++delays;
  if (stopAfterDelay) throw StopPending{};
  admit = true;
}
#define SENSOR_TASK_EXIT(subsys, ...) do { assert(!semaphore.held); ++exited; return; } while (false)

// INSERT_FUNCTIONS

static auto fields(const PresenceCache& s) {
  return std::make_tuple(s.ambientTemp, s.objectTemp, s.compObjectTemp, s.presenceValue,
                        s.motionValue, s.tempShockValue, s.presenceDetected, s.motionDetected,
                        s.tempShockDetected, s.lastUpdate, s.dataValid);
}
static void reset() {
  DATA = {}; semaphore = {}; MUTEX = &semaphore;
  DATA.ambientTemp = -12.5f; DATA.objectTemp = -150; DATA.compObjectTemp = 27.5f;
  DATA.presenceValue = 321; DATA.motionValue = -44; DATA.tempShockValue = 7;
  DATA.presenceDetected = DATA.motionDetected = DATA.tempShockDetected = true;
  DATA.lastUpdate = 12345; DATA.dataValid = true;
  gPresenceRunning = gPresenceConnected = gSettings.mqttPublishPresence = true;
  gPresenceTaskHandle = reinterpret_cast<void*>(0x1234);
  admit = memoryAvailable = createMutex = debugAvailable = true;
  validationOnly = busHeld = stopAfterDelay = false;
  waits.clear(); response.clear(); exited = delays = 0;
  display.text.clear(); oledDisplay = &display; oledConnected = true;
  sPresenceEvtPresent = gMotionActive = false; sPresenceEvtClearPolls = 0;
  events.clear(); transactions = 0; failedRegister = -1;
  busAvailable = transactionAvailable = true; nowMs = 56789;
  registers.fill(0);
  registers[STHS34PF80_STATUS] = 0x04;
  registers[STHS34PF80_FUNC_STATUS] = 0x07;
  registers[STHS34PF80_TAMBIENT_L] = -1234;
  registers[STHS34PF80_TOBJECT_L] = -200;
  registers[STHS34PF80_TOBJ_COMP_L] = 2150;
  registers[STHS34PF80_TPRESENCE_L] = 123;
  registers[STHS34PF80_TMOTION_L] = -15;
  registers[STHS34PF80_TAMB_SHOCK_L] = 9;
}
static void waited(uint32_t ms) {
  assert(waits.size() == 1 && waits[0] == ms && !semaphore.held);
  waits.clear();
}
static void emit(const std::string& text) { std::cout << text << '\n'; }
static std::string json() {
  char out[512]; const int n = presenceBuildDataJSON(out, sizeof(out));
  assert(n > 0 && n == static_cast<int>(std::strlen(out))); waited(50); return out;
}
static void bufferLimits(const std::string& expected) {
  for (size_t cap = 0; cap <= expected.size() + 2; ++cap) {
    std::vector<char> out(cap + 2, 'Z');
    const int n = presenceBuildDataJSON(out.data() + 1, cap);
    assert(out.front() == 'Z' && out.back() == 'Z');
    if (cap <= expected.size()) assert(n == 0);
    else assert(n == static_cast<int>(expected.size()) && std::string(out.data() + 1) == expected);
  }
  assert(presenceBuildDataJSON(nullptr, 20) == 0); waits.clear();
}
static void outputContracts() {
  reset(); const auto ready = json(); emit(ready); bufferLimits(ready);
  reset(); admit = false;
  const auto unavailable = json(); emit(unavailable); bufferLimits(unavailable);
  assert(unavailable.find("\"ambient\":0,") != std::string::npos);
  reset(); DATA = {}; MUTEX = &semaphore;
  const auto zero = json(); emit(zero); bufferLimits(zero);
  assert(zero.find("\"ambient\":0.00,") != std::string::npos);
  for (int state = 0; state < 5; ++state) {
    reset();
    if (state == 1) DATA.presenceDetected = DATA.tempShockDetected = false;
    if (state == 2) DATA.presenceDetected = DATA.motionDetected = DATA.tempShockDetected = false;
    if (state == 3) DATA.dataValid = false;
    if (state == 4) admit = false;
    char out[256]{};
    presenceG2FormatValue(out, sizeof(out)); waited(5); emit(out);
    g2BuildPresenceReadout(out, sizeof(out)); waited(5); emit(out);
    assert(webPresence() == ESP_OK); waited(50); emit(response);
    emit(cmd_presenceread("")); waited(100);
    displayPresenceData(); waited(10); emit(display.text);
    const auto snap = loggingPresence(LOG_PRESENCE); waited(10);
    assert(snap.gPresenceRunning && snap.gPresenceConnected);
    assert(snap.presenceAmbientTemp == (state == 4 ? 0 : -12.5f));
    assert(snap.presenceValue == (state == 4 ? 0 : 321));
    assert(snap.motionValue == (state == 4 ? 0 : -44));
    assert(snap.presenceDetected == (state == 4 ? false : DATA.presenceDetected));
    assert(snap.motionDetected == (state == 4 ? false : DATA.motionDetected));
    if (state != 4) {
      emit(cmd_presencestatus(""));
#if !TEST_BASELINE
      waited(10);
#endif
      char mqtt[16384]{}; const int n = mqttPresence(mqtt);
      assert(n == static_cast<int>(std::strlen(mqtt)));
#if !TEST_BASELINE
      waited(10);
#endif
      assert((n != 0) == (state != 3)); emit(mqtt);
    }
  }
  reset(); emit(cmd_presenceread("json")); waited(50);
  emit(cmd_presencestatus("json")); waited(50);
  reset(); gPresenceRunning = false;
  assert(webPresence() == ESP_OK); emit(response);
  emit(cmd_presenceread("")); displayPresenceData(); emit(display.text);
  char mqtt[16384]{};
  assert(mqttPresence(mqtt) == 0 && waits.empty());
  reset(); gSettings.mqttPublishPresence = false;
  assert(mqttPresence(mqtt) == 0 && waits.empty());
  (void)loggingPresence(0); assert(waits.empty());
  debugAvailable = false;
  assert(std::string(cmd_presenceread("")) == "Error: [PRESENCE] Debug buffer unavailable");
  assert(std::string(cmd_presencestatus("json")) == SENSOR_JSON_NOBUF);
  validationOnly = true;
  assert(std::string(cmd_presenceread("")) == "VALID");
  assert(std::string(cmd_presencestatus("json")) == "VALID" && waits.empty());
}
static void pollContracts() {
  // Exercise each bit separately: the production implementation interprets
  // presence=bit2, motion=bit1, shock=bit0 despite a conflicting old comment.
  for (int flags = 0; flags < 8; ++flags) {
    reset(); registers[STHS34PF80_FUNC_STATUS] = flags;
    assert(presencePoll()); waited(50);
    assert(DATA.ambientTemp == -12.34f && DATA.compObjectTemp == 21.5f && DATA.objectTemp == -200);
    assert(DATA.presenceValue == 123 && DATA.motionValue == -15 && DATA.tempShockValue == 9);
    assert(DATA.presenceDetected == bool(flags & 4) && DATA.motionDetected == bool(flags & 2));
    assert(DATA.tempShockDetected == bool(flags & 1) && DATA.dataValid && DATA.lastUpdate == nowMs);
  }
  for (int reg : {STHS34PF80_STATUS, STHS34PF80_FUNC_STATUS, STHS34PF80_TAMBIENT_L,
                 STHS34PF80_TOBJECT_L, STHS34PF80_TOBJ_COMP_L, STHS34PF80_TPRESENCE_L,
                 STHS34PF80_TMOTION_L, STHS34PF80_TAMB_SHOCK_L}) {
    reset(); const auto old = fields(DATA); failedRegister = reg;
    assert(!presencePoll() && waits.empty() && events.empty() && fields(DATA) == old);
  }
  reset(); const auto old = fields(DATA); registers[STHS34PF80_STATUS] = 0;
  assert(presencePoll() && waits.empty() && events.empty() && fields(DATA) == old);
  reset(); transactionAvailable = false;
  assert(!presencePoll() && waits.empty() && events.empty());
  reset(); busAvailable = false;
  assert(!presencePoll() && transactions == 0 && waits.empty());
  reset(); gPresenceConnected = false;
  assert(!presencePoll() && transactions == 0 && waits.empty());
  reset(); admit = false; const auto beforeBusy = fields(DATA);
  assert(presencePoll()); waited(50);
  assert(fields(DATA) == beforeBusy && events.size() == 2);

  reset(); assert(presencePoll()); waited(50);
  assert((events == std::vector<std::string>{"1:123", "3:detected"}));
  assert(presencePoll()); waited(50); assert(events.size() == 2);
  registers[STHS34PF80_FUNC_STATUS] = 0;
  for (int i = 0; i < 9; ++i) { assert(presencePoll()); waited(50); }
  assert(events.size() == 3 && events.back() == "3:cleared");
  // Another arrival resets the falling-edge debounce; nine new clear polls
  // still cannot emit cleared, the tenth must emit it exactly once.
  registers[STHS34PF80_FUNC_STATUS] = 4;
  assert(presencePoll()); waited(50); assert(events.size() == 3);
  registers[STHS34PF80_FUNC_STATUS] = 0;
  for (int i = 0; i < 9; ++i) { assert(presencePoll()); waited(50); }
  assert(events.size() == 3);
  assert(presencePoll()); waited(50); assert(events.size() == 4 && events.back() == "2:");
  assert(presencePoll()); waited(50); assert(events.size() == 4);
  for (const auto& event : events) emit(event);
}

#if !TEST_BASELINE
static void ownerContracts() {
  reset(); PresenceCache out{};
  assert(presenceCacheSnapshot(out)); waited(10); assert(fields(out) == fields(DATA));
  out.ambientTemp = 100; assert(DATA.ambientTemp == -12.5f);
  for (uint32_t timeout : {5, 10, 50, 100}) {
    admit = true; assert(presenceCacheSnapshot(out, timeout)); waited(timeout);
    admit = false; assert(!presenceCacheSnapshot(out, timeout)); waited(timeout);
    assert(fields(out) == fields(PresenceCache{}));
  }
  reset(); MUTEX = nullptr; out.dataValid = true;
  assert(!presenceCacheSnapshot(out) && fields(out) == fields(PresenceCache{}) && waits.empty());
  char jsonOut[512]; assert(presenceBuildDataJSON(jsonOut, sizeof(jsonOut)) > 0 && waits.empty());
  assert(std::string(jsonOut).find("\"ambient\":0,") != std::string::npos);
  reset(); admit = false;
  assert(std::string(cmd_presencestatus("")).find("dataValid=0") != std::string::npos); waited(10);
  char mqtt[16384]{}; assert(mqttPresence(mqtt) == 0); waited(10);

  reset(); const auto old = fields(DATA); admit = false;
  assert(!presenceInvalidateCache()); waited(50); assert(fields(DATA) == old);
  admit = true; PresenceCache stopped = DATA; stopped.dataValid = false;
  assert(presenceInvalidateCache()); waited(50); assert(fields(DATA) == fields(stopped));
  reset(); gPresenceRunning = false; admit = false; stopAfterDelay = true;
  try { runStopBranch(); assert(false); } catch (const StopPending&) {}
  waited(50); assert(exited == 0 && delays == 1 && DATA.dataValid);
  admit = true; stopAfterDelay = false;
  runStopBranch(); waited(50);
  assert(exited == 1 && !gPresenceConnected && !DATA.dataValid && DATA.presenceDetected);
  reset(); gPresenceRunning = false; admit = false;
  runStopBranch(); assert((waits == std::vector<uint32_t>{50, 50}));
  assert(delays == 1 && exited == 1 && !DATA.dataValid);

  reset(); gPresenceRunning = false;
  const auto pending = fields(DATA);
  assert(!runStartReset() && fields(DATA) == pending && waits.empty());
  gPresenceTaskHandle = nullptr; admit = false;
  assert(!runStartReset()); waited(100); assert(DATA.dataValid && DATA.lastUpdate == 12345);
  admit = true; assert(runStartReset()); waited(100);
  assert(fields(DATA) == fields(PresenceCache{}));
  reset(); gPresenceRunning = false; gPresenceTaskHandle = nullptr; MUTEX = nullptr; createMutex = false;
  assert(!runStartReset() && waits.empty());
  reset(); gPresenceRunning = false; gPresenceTaskHandle = nullptr; memoryAvailable = false;
  assert(!runStartReset() && waits.empty());
}
#endif

int main() {
  outputContracts(); pollContracts();
#if !TEST_BASELINE
  ownerContracts();
#endif
}
