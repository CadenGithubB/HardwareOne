#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
using SemaphoreHandle_t = void*;
#define pdMS_TO_TICKS(ms) (ms)
#define ENABLE_ESPNOW 0
#define ANO_AXIS_VERTICAL 0
#define ANO_AXIS_HORIZONTAL 1
#define DEBUG_CLIF(...) ((void)0)
#define ERROR_ANO_ENCODERF(...) ((void)0)
#define DEBUG_ANO_ENCODER_LIFECYCLEF(...) ((void)0)
static bool lockAvailable = true;
static int lockDepth = 0, initCalls = 0;
struct String {};
static bool gInputRunning = false, gAnoEncoderEnabled = false;
static bool gInputConnected = false, gAnoEncoderConnected = false;
static void* gInputTaskHandle = nullptr;
static void anoLogHeap(const char*) {}
static bool checkMemoryAvailable(const char*, String*) { return true; }
static bool inputCacheInit() { return true; }
static bool inputCacheInvalidate() { return true; }
static bool anoEncoderInit() { ++initCalls; assert(lockDepth == 0); return true; }
static bool createInputTask() { return true; }
static void sensorStatusBumpWith(const char*) {}
static SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
class SensorCacheGuard {
 public:
  bool held;
  SensorCacheGuard(SemaphoreHandle_t mutex, unsigned, const char*) : held(mutex && lockAvailable) {
    if (held) { assert(lockDepth == 0); ++lockDepth; }
  }
  ~SensorCacheGuard() { if (held) --lockDepth; }
};
#include "ano_owner_under_test.h"
int main() {
  sAnoCacheMutex = reinterpret_cast<void*>(1);
  sAnoEncoderCache.buttons = 9;
  sAnoEncoderCache.buttonPressedAccum = 7;
  sAnoEncoderCache.encoderPosition = 40;
  sAnoEncoderCache.encoderDelta = 3;
  sAnoEncoderCache.dataValid = true;
  sAnoEncoderCache.seq = 81;
  AnoEncoderCache copy;
  assert(anoEncoderCacheSnapshot(copy, 10));
  assert(copy.buttons == 9 && copy.encoderPosition == 40 && copy.encoderDelta == 3);
  assert(anoEncoderConsumeOneDetent() == 1 && sAnoEncoderCache.encoderDelta == 2);
  lockAvailable = false;
  assert(anoEncoderConsumeOneDetent() == 0 && sAnoEncoderCache.encoderDelta == 2);
  assert(!anoEncoderCacheSnapshot(copy, 10) && !copy.dataValid && !copy.buttons);
  assert(!invalidateAnoCache() && sAnoEncoderCache.dataValid);
  assert(!inputStartInternal() && initCalls == 0 && !gInputRunning);
  assert(sAnoEncoderCache.encoderDelta == 2 && sAnoEncoderCache.buttonPressedAccum == 7);
  lockAvailable = true;
  sAnoEncoderCache.encoderDelta = -2;
  assert(anoEncoderConsumeOneDetent() == -1 && anoEncoderConsumeOneDetent() == -1);
  assert(anoEncoderConsumeOneDetent() == 0);
  anoEncoderResetAxisForMode(ANO_AXIS_HORIZONTAL);
  assert(sAnoEncoderCache.currentAxis == ANO_AXIS_HORIZONTAL);
  lockAvailable = false;
  anoEncoderResetAxisForMode(ANO_AXIS_VERTICAL);
  assert(sAnoEncoderCache.currentAxis == ANO_AXIS_HORIZONTAL);
  lockAvailable = true;
  gInputTaskHandle = reinterpret_cast<void*>(1);
  assert(!inputStartInternal() && initCalls == 0 && sAnoEncoderCache.encoderPosition == 40);
  gInputTaskHandle = nullptr;
  assert(inputStartInternal() && initCalls == 1 && gInputRunning && gAnoEncoderEnabled);
  assert(!sAnoEncoderCache.dataValid && !sAnoEncoderCache.buttonPressedAccum);
  assert(sAnoEncoderCache.encoderDelta == 0 && sAnoEncoderCache.encoderPosition == 0);
  assert(sAnoEncoderCache.seq == 81);  // Restart is not new activity.
  sAnoEncoderCache.dataValid = true;
  assert(invalidateAnoCache() && !sAnoEncoderCache.dataValid && lockDepth == 0);
}
