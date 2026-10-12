// Definitions are extracted from production by test_apds_read_commands.py.
// Hardware/event interfaces exist only in the task-sampling variant; cache
// commands must compile and execute without either interface in the reader.
#include <Arduino.h>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

void webBatchTestStringAppend() {}
using TickType_t = uint32_t;
struct Semaphore { bool held = false; } semaphore;
using SemaphoreHandle_t = Semaphore*;
static bool cacheAdmit = true, validateOnly = false, bufferAvailable = true;
static std::vector<TickType_t> waits;
static int bufferRequests;
static char debugBuffer[1024];
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define CACHE_MUTEX_TIMEOUT_MS 100
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validateOnly) return "VALID"; } while (0)
static bool isHeldByCurrentTask(SemaphoreHandle_t m) { return m && m->held; }
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t ms) {
  waits.push_back(ms);
  if (!m || m->held || !cacheAdmit) return 0;
  m->held = true;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) {
  assert(m && m->held);
  m->held = false;
}
static bool ensureDebugBuffer() {
  assert(!semaphore.held);
  ++bufferRequests;
  return bufferAvailable;
}
static char* getDebugBuffer() { assert(!semaphore.held); return debugBuffer; }
static int trackedSnprintf(char* out, size_t size, const char* format, ...) {
  assert(!semaphore.held);
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(out, size, format, args);
  va_end(args);
  return n;
}
#define snprintf trackedSnprintf
static unsigned long harnessClockMs = 0;
[[maybe_unused]] static unsigned long millis() { return harnessClockMs; }
static bool gApdsRunning, gApdsConnected, gApdsColorRunning;
static bool gApdsProximityRunning, gApdsGestureRunning;
static constexpr uint8_t APDS9960_UP = 1, APDS9960_DOWN = 2;
static constexpr uint8_t APDS9960_LEFT = 3, APDS9960_RIGHT = 4;

// INSERT_TYPES
// INSERT_STATE
// INSERT_READER_FUNCTIONS

static auto values(const APDSCache& reading) {
  return std::make_tuple(reading.apdsRed, reading.apdsGreen, reading.apdsBlue,
                        reading.apdsClear, reading.apdsProximity, reading.apdsGesture,
                        reading.apdsLastUpdate, reading.apdsDataValid, reading.apdsColorValid,
                        reading.apdsProximityValid, reading.apdsGestureValid,
                        reading.apdsLastGesture, reading.apdsLastGestureMs);
}
static bool error(const char* text) { return std::strncmp(text, "Error:", 6) == 0; }
static bool contains(const char* text, const char* part) { return std::strstr(text, part) != nullptr; }
static void reset() {
  sApdsCache = {};
  semaphore = {};
  sApdsCacheMutex = &semaphore;
  cacheAdmit = bufferAvailable = true;
  validateOnly = false;
  waits.clear();
  bufferRequests = 0;
  debugBuffer[0] = '\0';
  gApdsRunning = gApdsConnected = gApdsColorRunning = true;
  gApdsProximityRunning = gApdsGestureRunning = true;
}
static void ready() {
  reset();
  sApdsCache.apdsRed = 65535;
  sApdsCache.apdsGreen = 1234;
  sApdsCache.apdsBlue = 17;
  sApdsCache.apdsClear = 42;
  sApdsCache.apdsProximity = 255;
  sApdsCache.apdsGesture = APDS9960_UP;
  sApdsCache.apdsLastUpdate = 12345;
  sApdsCache.apdsDataValid = sApdsCache.apdsColorValid = true;
  sApdsCache.apdsProximityValid = sApdsCache.apdsGestureValid = true;
  sApdsCache.apdsLastGesture = APDS9960_LEFT;  // latched 1.2 s before the clock
  sApdsCache.apdsLastGestureMs = 11000;
  harnessClockMs = 12250;
}
using Handler = const char* (*)(const String&);
static void readerTests() {
  const Handler handlers[] = {cmd_apdscolor, cmd_apdsproximity, cmd_apdsgesture};
  bool* const modeFlags[] = {&gApdsColorRunning, &gApdsProximityRunning, &gApdsGestureRunning};
  bool* const validFlags[] = {&sApdsCache.apdsColorValid, &sApdsCache.apdsProximityValid,
                            &sApdsCache.apdsGestureValid};
  APDSCache empty;
  assert(!empty.apdsDataValid && !empty.apdsColorValid && !empty.apdsProximityValid && !empty.apdsGestureValid);
  for (size_t i = 0; i < 3; ++i) {
    const auto handler = handlers[i];
    reset();
    assert(error(handler(""))); // No first sample yet.
    ready(); validateOnly = true;
    assert(std::strcmp(handler(""), "VALID") == 0 && waits.empty() && bufferRequests == 0);
    ready(); gApdsRunning = false;
    assert(error(handler("")) && waits.empty());
    ready(); gApdsConnected = false;
    assert(error(handler("")) && waits.empty());
    ready(); *modeFlags[i] = false;
    assert(error(handler("")));
    ready(); cacheAdmit = false;
    const auto before = values(sApdsCache);
    assert(error(handler("")) && values(sApdsCache) == before && !semaphore.held);
    assert(waits.size() == 1 && waits[0] <= 100);
    ready(); sApdsCacheMutex = nullptr;
    assert(error(handler("")) && waits.empty());
    ready(); *validFlags[i] = false;
    assert(error(handler(""))); // Another mode's data cannot validate this one.
    ready(); sApdsCache.apdsDataValid = false;
    assert(error(handler("")));
    ready(); bufferAvailable = false;
    assert(error(handler("")));  // Every formatted reply needs the buffer.
    ready();
    const auto unchanged = values(sApdsCache);
    const std::string first = handler("");
    assert(!error(first.c_str()));
    assert(first == handler("") && values(sApdsCache) == unchanged && !semaphore.held);
  }
  ready();
  const std::string color = cmd_apdscolor("");
  for (const char* value : {"65535", "1234", "17", "42"}) assert(contains(color.c_str(), value));
  assert(contains(cmd_apdsproximity(""), "255"));
  // The command reports the latched last swipe and its age, whatever the
  // latest sample holds (one poll sees only one FIFO batch).
  for (const auto& gesture : {std::pair<uint8_t, const char*>{APDS9960_UP, "Gesture: UP (1.2 s ago)"},
                             {APDS9960_DOWN, "Gesture: DOWN (1.2 s ago)"},
                             {APDS9960_LEFT, "Gesture: LEFT (1.2 s ago)"},
                             {APDS9960_RIGHT, "Gesture: RIGHT (1.2 s ago)"}}) {
    sApdsCache.apdsLastGesture = gesture.first;
    sApdsCache.apdsGesture = 0;
    const auto before = values(sApdsCache);
    assert(std::strcmp(cmd_apdsgesture(""), gesture.second) == 0);
    assert(std::strcmp(cmd_apdsgesture(""), gesture.second) == 0 && values(sApdsCache) == before);
  }
  harnessClockMs = 11000 + 65432;
  assert(std::strcmp(cmd_apdsgesture(""), "Gesture: RIGHT (65.4 s ago)") == 0);
  sApdsCache.apdsLastGesture = 0; sApdsCache.apdsGesture = APDS9960_UP; bufferAvailable = false;
  assert(std::strcmp(cmd_apdsgesture(""), "No gesture since start") == 0);  // literal: no buffer
  bufferAvailable = true;
  sApdsCache.apdsLastGesture = 99;
  assert(error(cmd_apdsgesture("")));
  ready(); cacheAdmit = false;
  assert(!apdsInvalidateCache() && sApdsCache.apdsColorValid && sApdsCache.apdsProximityValid && sApdsCache.apdsGestureValid);
  cacheAdmit = true;
  assert(apdsInvalidateCache() && !sApdsCache.apdsDataValid && !sApdsCache.apdsColorValid &&
         !sApdsCache.apdsProximityValid && !sApdsCache.apdsGestureValid);
  assert(!sApdsCache.apdsLastGesture && !sApdsCache.apdsLastGestureMs);  // no swipe from a past session
  ready(); assert(resetApdsReading());
  assert(!sApdsCache.apdsDataValid && !sApdsCache.apdsColorValid &&
         !sApdsCache.apdsProximityValid && !sApdsCache.apdsGestureValid);
  assert(!sApdsCache.apdsLastGesture && !sApdsCache.apdsLastGestureMs);
}

static void combinedReadTests() {
  ready();
  const auto before = values(sApdsCache);
  const std::string expectedText = "APDS9960 Status:\n"
      "  Color: ON  Proximity: ON  Gesture: ON\n"
      "  RGBC: R=65535 G=1234 B=17 C=42\n  Proximity: 255\n";
  const std::string expectedJson = "{\"valid\":true,\"connected\":true,\"ts\":12345,"
      "\"r\":65535,\"g\":1234,\"b\":17,\"c\":42,\"proximity\":255}";
  assert(cmd_apdsread("") == expectedText);
  assert(cmd_apdsread("json") == expectedJson);
  assert(values(sApdsCache) == before && !semaphore.held);
  sApdsCache.apdsColorValid = false;
  assert(contains(cmd_apdsread(""), "RGBC: waiting for sample"));
  sApdsCache.apdsProximityValid = false;
  assert(contains(cmd_apdsread(""), "Proximity: waiting for sample"));
  gApdsColorRunning = gApdsProximityRunning = false;
  assert(!contains(cmd_apdsread(""), "\n  RGBC:") && !contains(cmd_apdsread(""), "\n  Proximity:"));
  ready(); sApdsCache.apdsDataValid = false;
  assert(error(cmd_apdsread("")) && contains(cmd_apdsread("json"), "\"valid\":false"));
  ready(); gApdsRunning = false;
  assert(error(cmd_apdsread("")) && cmd_apdsread("json") == expectedJson);
  ready(); gApdsConnected = false;
  assert(error(cmd_apdsread("")) && contains(cmd_apdsread("json"), "\"connected\":false"));
  ready(); cacheAdmit = false;
  assert(error(cmd_apdsread("")));
  assert(std::strcmp(cmd_apdsread("json"),
      "{\"valid\":false,\"connected\":true,\"ts\":0,\"r\":0,\"g\":0,\"b\":0,\"c\":0,\"proximity\":0}") == 0);
  ready(); bufferAvailable = false;
  assert(error(cmd_apdsread("")) && std::strcmp(cmd_apdsread("json"), SENSOR_JSON_NOBUF) == 0);
  ready(); validateOnly = true;
  assert(std::strcmp(cmd_apdsread(""), "VALID") == 0);
  assert(std::strcmp(cmd_apdsread("json"), "VALID") == 0 && waits.empty() && bufferRequests == 0);
}

#if TEST_SAMPLING
// Only this variant can name a driver or bus/event API. The complete production
// sampling conditional is wrapped in a one-iteration loop for its fault continue.
static struct { int apdsBus = 0; } gSettings;
static constexpr uint8_t I2C_ADDR_APDS = 0x39;
static bool busAdmit = true, busHeld = false, autoDisable = false, gApdsPresent = false;
static int busCalls, healthyTransactions, failedTransactions, healthChecks, statusBumps;
static bool deviceColorReady = true;
static uint8_t deviceProximity = 220, deviceGesture = APDS9960_UP;
struct APDSDriver {
  int readinessCalls = 0, colorCalls = 0, proximityCalls = 0, gestureCalls = 0;
  void held() const { assert(busHeld && !semaphore.held); }
  bool colorDataReady() { held(); ++readinessCalls; return deviceColorReady; }
  void getColorData(uint16_t* red, uint16_t* green, uint16_t* blue, uint16_t* clear) {
    held(); ++colorCalls; *red = 1; *green = 2; *blue = 3; *clear = 4;
  }
  uint8_t readProximity() { held(); ++proximityCalls; return deviceProximity; }
  uint8_t readGesture() { held(); ++gestureCalls; return deviceGesture; }
} driver;
static APDSDriver* gAPDS9960 = &driver;
template<class Operation>
static bool i2cTaskWithTimeout(uint8_t bus, uint8_t address, unsigned hz,
                               unsigned wait, Operation operation) {
  assert(!busHeld && !semaphore.held);
  assert(bus == gSettings.apdsBus && address == I2C_ADDR_APDS && hz == 100000 && wait == 100);
  ++busCalls;
  if (!busAdmit) return false;
  busHeld = true;
  const bool result = operation();
  busHeld = false;
  if (result) ++healthyTransactions; else ++failedTransactions;
  return result;
}
enum { SYSEVT_GESTURE, SYSEVT_PRESENCE_DETECTED, SYSEVT_PRESENCE_CLEARED, SYSEVT_SENSOR_FAULT };
static std::vector<std::tuple<int, std::string, std::string>> events;
static void systemEventPost(int type, const char* subject, const char* detail = "") {
  assert(!busHeld && !semaphore.held);
  events.emplace_back(type, subject, detail);
}
static bool i2cShouldAutoDisable(uint8_t address, uint8_t bus) {
  assert(address == I2C_ADDR_APDS && bus == gSettings.apdsBus);
  ++healthChecks;
  return autoDisable;
}
static uint8_t i2cGetConsecutiveErrors(uint8_t address, uint8_t bus) {
  assert(address == I2C_ADDR_APDS && bus == gSettings.apdsBus);
  return 7;
}
static void sensorStatusBumpWith(const char*) { assert(!busHeld && !semaphore.held); ++statusBumps; }
static void logSystemEvent(const char*, const char*, ...) { assert(!busHeld && !semaphore.held); }
#define DEBUG_APDS_LIFECYCLEF(...) do {} while (0)

// INSERT_SAMPLING_FUNCTION

static unsigned long lastGestureRead;
static bool sampleOnce(unsigned long nowMs, unsigned long& lastRead, unsigned long pollMs = 200) {
  return apdsPollDevice(nowMs, pollMs, lastRead, lastGestureRead);
}
static void samplingReset() {
  ready();
  driver = {};
  gSettings.apdsBus = 0;
  busAdmit = deviceColorReady = true;
  busHeld = autoDisable = gApdsPresent = false;
  busCalls = healthyTransactions = failedTransactions = healthChecks = statusBumps = 0;
  deviceProximity = 220;
  deviceGesture = APDS9960_UP;
  lastGestureRead = 0;
  events.clear();
}
static void samplingTests() {
  // Exercise all mode combinations and both color-readiness states. Modes that
  // did not produce a sample cannot inherit the previous sample's validity.
  for (int bus : {0, 1}) for (int modes = 0; modes < 8; ++modes) for (bool colorReady : {false, true}) {
    samplingReset();
    gSettings.apdsBus = bus;
    gApdsColorRunning = (modes & 1) != 0;
    gApdsProximityRunning = (modes & 2) != 0;
    gApdsGestureRunning = (modes & 4) != 0;
    deviceColorReady = colorReady;
    unsigned long lastRead = 0;
    sampleOnce(1000, lastRead);
    // A conversion that is not ready keeps the previous colour sample (the
    // fixture's) and its validity; a mode that is off reads zero and invalid.
    const bool color = gApdsColorRunning && colorReady;
    const bool keptColor = gApdsColorRunning && !colorReady;
    assert(sApdsCache.apdsColorValid == gApdsColorRunning);
    assert(sApdsCache.apdsProximityValid == gApdsProximityRunning);
    assert(sApdsCache.apdsGestureValid == gApdsGestureRunning);
    assert(sApdsCache.apdsDataValid == (gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning));
    assert(sApdsCache.apdsRed == (color ? 1 : keptColor ? 65535 : 0));
    assert(sApdsCache.apdsGreen == (color ? 2 : keptColor ? 1234 : 0));
    assert(sApdsCache.apdsBlue == (color ? 3 : keptColor ? 17 : 0));
    assert(sApdsCache.apdsClear == (color ? 4 : keptColor ? 42 : 0));
    assert(sApdsCache.apdsProximity == (gApdsProximityRunning ? 220 : 0));
    assert(sApdsCache.apdsGesture == (gApdsGestureRunning ? APDS9960_UP : 0));
    assert(sApdsCache.apdsLastGesture == (gApdsGestureRunning ? APDS9960_UP : APDS9960_LEFT));
    assert(sApdsCache.apdsLastGestureMs == (gApdsGestureRunning ? 1000u : 11000u));
    assert(sApdsCache.apdsLastUpdate == 1000 && lastRead == 1000);
    assert(driver.readinessCalls == int(gApdsColorRunning) && driver.colorCalls == int(color));
    assert(driver.proximityCalls == int(gApdsProximityRunning) && driver.gestureCalls == int(gApdsGestureRunning));
    assert(busCalls == 1 && healthyTransactions == 1 && failedTransactions == 0 && healthChecks == 0);
    assert(waits.size() == 1 && waits[0] == 50 && !semaphore.held && !busHeld);
    const auto reads = std::make_tuple(driver.readinessCalls, driver.colorCalls, driver.proximityCalls, driver.gestureCalls);
    const auto oldEvents = events;
    const auto oldCache = values(sApdsCache);
    for (const auto command : {cmd_apdscolor, cmd_apdsproximity, cmd_apdsgesture, cmd_apdsread}) command("");
    cmd_apdsread("json");
    assert(reads == std::make_tuple(driver.readinessCalls, driver.colorCalls, driver.proximityCalls, driver.gestureCalls));
    assert(busCalls == 1 && events == oldEvents && values(sApdsCache) == oldCache);
  }
  samplingReset();
  unsigned long lastRead = 900;
  lastGestureRead = 950;
  const auto before = values(sApdsCache);
  sampleOnce(1000, lastRead);
  assert(busCalls == 0 && values(sApdsCache) == before && lastRead == 900);
  sampleOnce(1100, lastRead);
  assert(busCalls == 1 && lastRead == 1100 && lastGestureRead == 1100);

  // Gesture cadence: the FIFO is drained at least every 100 ms however long
  // the colour/proximity interval is; a gesture-only pass leaves the other
  // fields and their validity alone and cannot move presence.
  samplingReset(); lastRead = 0;
  sampleOnce(1000, lastRead, 1000);
  assert(busCalls == 1 && driver.gestureCalls == 1 && driver.proximityCalls == 1);
  assert(lastRead == 1000 && lastGestureRead == 1000 && events.size() == 2);
  sampleOnce(1050, lastRead, 1000);
  assert(busCalls == 1);
  const auto colourAndProximity = std::make_tuple(sApdsCache.apdsRed, sApdsCache.apdsProximity,
                                                  sApdsCache.apdsColorValid, sApdsCache.apdsProximityValid);
  deviceProximity = 0; deviceGesture = 0;
  sampleOnce(1100, lastRead, 1000);
  assert(busCalls == 2 && driver.gestureCalls == 2 && driver.proximityCalls == 1 && driver.readinessCalls == 1);
  assert(lastRead == 1000 && lastGestureRead == 1100 && events.size() == 2 && gApdsPresent);
  assert(colourAndProximity == std::make_tuple(sApdsCache.apdsRed, sApdsCache.apdsProximity,
                                               sApdsCache.apdsColorValid, sApdsCache.apdsProximityValid));
  assert(sApdsCache.apdsGesture == 0 && sApdsCache.apdsGestureValid && sApdsCache.apdsLastUpdate == 1100);
  for (unsigned long t = 1200; t < 2000; t += 100) sampleOnce(t, lastRead, 1000);
  assert(driver.gestureCalls == 10 && driver.proximityCalls == 1);
  sampleOnce(2000, lastRead, 1000);
  assert(driver.gestureCalls == 11 && driver.proximityCalls == 2 && lastRead == 2000);
  samplingReset(); lastRead = 0;
  sampleOnce(1000, lastRead, 50); sampleOnce(1050, lastRead, 50);
  assert(driver.gestureCalls == 2 && driver.proximityCalls == 2 && busCalls == 2);
  samplingReset(); lastRead = 0; gApdsGestureRunning = false;
  sampleOnce(1000, lastRead, 1000); sampleOnce(1100, lastRead, 1000);
  assert(busCalls == 1 && driver.gestureCalls == 0);

  // Colour "not ready" is no new sample: without a previous one it stays
  // invalid; with one it is kept; turning colour off clears it.
  samplingReset(); sApdsCache = {}; deviceColorReady = false; lastRead = 0;
  sampleOnce(1000, lastRead);
  assert(!sApdsCache.apdsColorValid && sApdsCache.apdsRed == 0 && sApdsCache.apdsProximityValid);
  deviceColorReady = true; sampleOnce(1200, lastRead);
  assert(sApdsCache.apdsColorValid && sApdsCache.apdsRed == 1 && sApdsCache.apdsClear == 4);
  deviceColorReady = false; sampleOnce(1400, lastRead);
  assert(sApdsCache.apdsColorValid && sApdsCache.apdsRed == 1 && sApdsCache.apdsClear == 4);
  gApdsColorRunning = false; sampleOnce(1600, lastRead);
  assert(!sApdsCache.apdsColorValid && sApdsCache.apdsRed == 0 && sApdsCache.apdsDataValid);

  // The latch keeps a swipe after later empty samples; 'apdsgesture' reports it.
  samplingReset(); lastRead = 0; sApdsCache.apdsLastGesture = 0;
  deviceGesture = APDS9960_RIGHT; sampleOnce(1000, lastRead);
  deviceGesture = 0; sampleOnce(1200, lastRead); sampleOnce(1400, lastRead);
  harnessClockMs = 2250;
  assert(sApdsCache.apdsGesture == 0 && sApdsCache.apdsLastGesture == APDS9960_RIGHT);
  assert(std::strcmp(cmd_apdsgesture(""), "Gesture: RIGHT (1.2 s ago)") == 0);

  samplingReset(); lastRead = 0; busAdmit = false;
  const auto retained = values(sApdsCache);
  sampleOnce(1000, lastRead);
  assert(values(sApdsCache) == retained && driver.readinessCalls == 0 && events.empty());
  assert(healthChecks == 1 && healthyTransactions == 0 && lastRead == 1000 && waits.empty());

  samplingReset(); lastRead = 0; cacheAdmit = false;
  const auto locked = values(sApdsCache);
  sampleOnce(1000, lastRead);
  assert(values(sApdsCache) == locked && driver.gestureCalls == 1 && !semaphore.held);
  assert(events.size() == 2); // Existing task events do not depend on cache admission.
  cacheAdmit = true;
  sampleOnce(1200, lastRead);
  assert(sApdsCache.apdsLastUpdate == 1200 && sApdsCache.apdsColorValid);

  samplingReset(); lastRead = 0;
  sampleOnce(1000, lastRead);
  assert(events.size() == 2 && gApdsPresent);
  assert(std::get<0>(events[0]) == SYSEVT_GESTURE && std::get<1>(events[0]) == "UP");
  assert(std::get<0>(events[1]) == SYSEVT_PRESENCE_DETECTED);
  for (int i = 0; i < 3; ++i) assert(contains(cmd_apdsgesture(""), "UP"));
  assert(events.size() == 2 && driver.gestureCalls == 1);
  deviceGesture = 0; deviceProximity = 150;
  sampleOnce(1200, lastRead);
  assert(events.size() == 2 && gApdsPresent && sApdsCache.apdsGestureValid);
  harnessClockMs = 1200;
  assert(std::strcmp(cmd_apdsgesture(""), "Gesture: UP (0.2 s ago)") == 0);
  deviceProximity = 50;
  sampleOnce(1400, lastRead);
  assert(events.size() == 3 && !gApdsPresent && std::get<0>(events.back()) == SYSEVT_PRESENCE_CLEARED);

  samplingReset(); lastRead = 0; busAdmit = false; autoDisable = true;
  sampleOnce(1000, lastRead);
  assert(!gApdsColorRunning && !gApdsProximityRunning && !gApdsGestureRunning && !gApdsConnected);
  assert(statusBumps == 1 && events.size() == 1 && std::get<0>(events[0]) == SYSEVT_SENSOR_FAULT);
  assert(lastRead == 0); // Production continue leaves timestamp for the shutdown path.
}
#endif

int main() {
  readerTests();
  combinedReadTests();
#if TEST_SAMPLING
  samplingTests();
  std::puts("APDS task sampling passed (32 mode/readiness/bus cases; cache admission and task-only events)");
#else
  std::puts("APDS cached command outcomes passed (production commands/snapshots; no hardware or events)");
#endif
}
