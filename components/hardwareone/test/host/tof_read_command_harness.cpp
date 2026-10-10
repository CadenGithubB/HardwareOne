// Production types, snapshot, serializer and command are inserted by the runner.
#include <cassert>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using String = std::string;
using TickType_t = uint32_t;
struct Semaphore { bool held = false; } semaphore;
using SemaphoreHandle_t = Semaphore*;
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
// INSERT_TIMEOUT
// INSERT_TYPES
// INSERT_STATE

static bool gTofRunning = true, gTofConnected = true;
static bool admit = true, validateOnly = false, debugBufferAvailable = true;
static unsigned releases = 0, debugBufferRequests = 0;
static std::vector<unsigned> waits;
static std::vector<std::string> output;
static std::function<void()> afterRelease;
static char debugBuffer[1024];

static bool isHeldByCurrentTask(SemaphoreHandle_t mutex) { return mutex && mutex->held; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
  waits.push_back(timeout);
  if (!mutex || mutex->held || !admit) return 0;
  mutex->held = true;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex && mutex->held);
  mutex->held = false;
  ++releases;
  if (afterRelease) afterRelease();
}
static void presentationAllowed() { assert(!semaphore.held); }
static int trackedSnprintf(char* buffer, size_t capacity, const char* format, ...) {
  presentationAllowed();
  va_list args;
  va_start(args, format);
  int result = std::vsnprintf(buffer, capacity, format, args);
  va_end(args);
  return result;
}
#define snprintf trackedSnprintf
static void broadcastPrintf(const char* format, ...) {
  presentationAllowed();
  char buffer[256];
  va_list args;
  va_start(args, format);
  int count = std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  assert(count >= 0 && static_cast<size_t>(count) < sizeof(buffer));
  output.emplace_back(buffer);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validateOnly) return "VALID"; } while (0)
static void diagnostic(const char*, ...) { presentationAllowed(); }
#define DEBUG_TOF_POLLINGF(...) diagnostic(__VA_ARGS__)
static bool argWantsJson(const String& input) { return input == "json"; }
static bool ensureDebugBuffer() {
  ++debugBufferRequests;
  return debugBufferAvailable;
}
static char* getDebugBuffer() { return debugBuffer; }

// INSERT_FUNCTIONS

static void reset() {
  semaphore = {};
  sTofCache = {};
  sTofCacheMutex = &semaphore;
  gTofRunning = gTofConnected = true;
  admit = debugBufferAvailable = true;
  validateOnly = false;
  releases = debugBufferRequests = 0;
  waits.clear();
  output.clear();
  afterRelease = {};
  debugBuffer[0] = '\0';
}
static void object(int slot, bool detected, bool valid, int mm) {
  auto& value = sTofCache.tofObjects[slot];
  value.detected = detected;
  value.valid = valid;
  value.distance_mm = mm;
  // JSON's published integer millimeters are the text command's source too;
  // intermediate smoothing fields and the aggregate count do not select it.
  value.distance_cm = 999.0f;
  value.smoothed_distance_mm = 9999.0f;
  value.smoothed_distance_cm = 999.0f;
}
static void expectUnavailable(const char* reason) {
  std::string result = cmd_tof("");
  assert(result.rfind("Error:", 0) == 0 && result.find(reason) != std::string::npos);
  assert(output.empty() && !semaphore.held && debugBufferRequests == 0);
}
static void expectDistance(const char* expected) {
  assert(std::string(cmd_tof("")) == "[ToF] Reading complete");
  assert(output.size() == 1 && output[0] == expected);
  assert(waits == std::vector<unsigned>{CACHE_MUTEX_TIMEOUT_MS});
  assert(releases == 1 && !semaphore.held && debugBufferRequests == 0);
}

int main() {
  reset();
  validateOnly = true;
  assert(std::string(cmd_tof("")) == "VALID");
  assert(std::string(cmd_tof("json")) == "VALID");
  assert(waits.empty() && output.empty() && debugBufferRequests == 0);

  reset();
  sTofCache.tofDataValid = true;
  object(0, true, true, 250);
  gTofRunning = false;
  expectUnavailable("stopped");
  assert(waits.empty());
  gTofRunning = true;
  gTofConnected = false;
  expectUnavailable("not connected");
  assert(waits.empty());

  reset();
  sTofCache.tofDataValid = true;
  object(0, true, true, 250);
  admit = false;
  expectUnavailable("cache busy or unavailable");
  assert(waits == std::vector<unsigned>{CACHE_MUTEX_TIMEOUT_MS} && releases == 0);
  reset();
  sTofCacheMutex = nullptr;
  expectUnavailable("cache busy or unavailable");
  assert(waits.empty() && releases == 0);

  reset();
  object(0, true, true, 250);
  expectUnavailable("not yet available");
  assert(releases == 1);
  reset();
  sTofCache.tofDataValid = true;
  expectUnavailable("No valid detected object");
  reset();
  sTofCache.tofDataValid = true;
  object(0, true, false, 1);
  object(1, false, true, 2);
  object(2, true, true, 0);
  object(3, true, true, -10);
  expectUnavailable("No valid detected object");

  // A stale or excessive count cannot hide the fourth slot or overrun storage.
  for (int count : {0, -1, 1, 4, INT_MAX}) {
    reset();
    sTofCache.tofDataValid = true;
    sTofCache.tofTotalObjects = count;
    object(0, true, false, 1);
    object(1, false, true, 2);
    object(2, true, true, 6000);
    object(3, true, true, 237);
    expectDistance("Distance: 23.7 cm");
  }
  reset();
  sTofCache.tofDataValid = true;
  object(0, true, true, 456);
  object(1, true, true, 700);
  afterRelease = [] {
    sTofCache.tofDataValid = false;
    sTofCache.tofObjects[0].distance_mm = 1;
  };
  expectDistance("Distance: 45.6 cm");
  assert(!sTofCache.tofDataValid && sTofCache.tofObjects[0].distance_mm == 1);

  reset();
  sTofCache.tofDataValid = true;
  sTofCache.tofLastUpdate = 1234;
  object(0, true, true, 237);
  assert(std::string(cmd_tof("json")) ==
      "{\"valid\":true,\"connected\":true,\"ts\":1234,\"objects\":[{\"id\":1,\"distance_mm\":237,\"status\":0,\"valid\":true}]}");
  assert(output.empty() && releases == 1 && debugBufferRequests == 1);

  reset();
  gTofRunning = gTofConnected = false;
  assert(std::string(cmd_tof("json")) ==
      "{\"valid\":false,\"connected\":false,\"ts\":0,\"objects\":[]}");
  assert(output.empty() && debugBufferRequests == 1);
  reset();
  admit = false;
  assert(std::string(cmd_tof("json")) ==
      "{\"valid\":false,\"connected\":true,\"ts\":0,\"objects\":[],\"total_objects\":0}");
  assert(output.empty() && releases == 0);
  reset();
  debugBufferAvailable = false;
  assert(std::string(cmd_tof("json")) == SENSOR_JSON_NOBUF);
  assert(waits.empty() && output.empty());

  std::cout << "ToF read command: unavailable states, bounded nearest selection, snapshot ownership and unchanged JSON passed\n";
}
