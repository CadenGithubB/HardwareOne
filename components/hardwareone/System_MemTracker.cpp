#include "System_MemTracker.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <freertos/semphr.h>
#include <string.h>

namespace {

using TrackerRegistry =
    hw1_memtracker_detail::Registry<kMemTrackerCapacity>;

#if defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
EXT_RAM_BSS_ATTR static TrackerRegistry sRegistry;
#else
static TrackerRegistry sRegistry;
#endif

// The mutex control block must remain in internal RAM.  It is statically
// backed, so tracker initialization cannot recurse through an allocator.
static StaticSemaphore_t sMutexStorage;
static SemaphoreHandle_t sMutex = nullptr;
static bool sInitialized = false;  // protected by sMutex once initialized
static uint32_t sGeneration = 0;   // protected by sMutex

// The registry can live in PSRAM, so it is never touched from a port critical
// section.  This tiny control block stays in internal RAM and only protects
// the enabled flag plus counters that must be updated when the registry mutex
// cannot be acquired.  No allocation or logging occurs while it is held.
static portMUX_TYPE sControlMux = portMUX_INITIALIZER_UNLOCKED;
static bool sEnabled = false;
static uint64_t sContentionDrops = 0;
static uint64_t sInvalidTagEvents = 0;

bool trackerEnabled() {
  portENTER_CRITICAL(&sControlMux);
  const bool enabled = sEnabled;
  portEXIT_CRITICAL(&sControlMux);
  return enabled;
}

void setTrackerEnabled(bool enabled) {
  portENTER_CRITICAL(&sControlMux);
  sEnabled = enabled;
  portEXIT_CRITICAL(&sControlMux);
}

void resetSideCounters() {
  portENTER_CRITICAL(&sControlMux);
  sContentionDrops = 0;
  sInvalidTagEvents = 0;
  portEXIT_CRITICAL(&sControlMux);
}

void resetControlState(bool enabled) {
  portENTER_CRITICAL(&sControlMux);
  sEnabled = enabled;
  sContentionDrops = 0;
  sInvalidTagEvents = 0;
  portEXIT_CRITICAL(&sControlMux);
}

void incrementContentionDrops() {
  portENTER_CRITICAL(&sControlMux);
  ++sContentionDrops;
  portEXIT_CRITICAL(&sControlMux);
}

void incrementInvalidTagEvents() {
  portENTER_CRITICAL(&sControlMux);
  ++sInvalidTagEvents;
  portEXIT_CRITICAL(&sControlMux);
}

void snapshotControlState(bool& enabled, uint64_t& contentionDrops,
                          uint64_t& invalidTagEvents) {
  portENTER_CRITICAL(&sControlMux);
  enabled = sEnabled;
  contentionDrops = sContentionDrops;
  invalidTagEvents = sInvalidTagEvents;
  portEXIT_CRITICAL(&sControlMux);
}

}  // namespace

bool memTrackerInit(bool enabled) {
  if (!sMutex) {
    sMutex = xSemaphoreCreateMutexStatic(&sMutexStorage);
  }
  if (!sMutex || xSemaphoreTake(sMutex, portMAX_DELAY) != pdTRUE) return false;

  resetControlState(false);
  sGeneration = 1;
  sRegistry.reset(sGeneration, millis());
  sInitialized = true;
  setTrackerEnabled(enabled);
  xSemaphoreGive(sMutex);
  return true;
}

bool memTrackerSetEnabled(bool enabled) {
  if (!sMutex || xSemaphoreTake(sMutex, portMAX_DELAY) != pdTRUE) return false;
  if (!sInitialized) {
    xSemaphoreGive(sMutex);
    return false;
  }
  setTrackerEnabled(enabled);
  xSemaphoreGive(sMutex);
  return true;
}

bool memTrackerReset() {
  if (!sMutex || xSemaphoreTake(sMutex, portMAX_DELAY) != pdTRUE) return false;
  if (!sInitialized) {
    xSemaphoreGive(sMutex);
    return false;
  }

  // Establish a clean epoch while updates fail fast on the held mutex.  Reset
  // the side counters before the potentially long PSRAM clear so every update
  // rejected during that clear remains visible in the new epoch.
  ++sGeneration;
  resetSideCounters();
  sRegistry.reset(sGeneration, millis());
  xSemaphoreGive(sMutex);
  return true;
}

bool memTrackerSnapshot(MemTrackerSnapshot& out,
                        MemTrackerEntry* topEntries,
                        size_t topCapacity,
                        TickType_t timeoutTicks) {
  out = {};
  if ((topCapacity != 0 && !topEntries) || !sMutex) return false;
  if (xSemaphoreTake(sMutex, timeoutTicks) != pdTRUE) return false;
  if (!sInitialized) {
    xSemaphoreGive(sMutex);
    return false;
  }

  bool enabled = false;
  uint64_t contentionDrops = 0;
  uint64_t invalidTagEvents = 0;
  snapshotControlState(enabled, contentionDrops, invalidTagEvents);
  sRegistry.snapshot(out, sInitialized, enabled, contentionDrops,
                     invalidTagEvents, topEntries, topCapacity);
  xSemaphoreGive(sMutex);
  return true;
}

void memTrackerRecord(const char* op, void* ptr, size_t size,
                      bool requestedPS, bool usedPS, bool fellBack,
                      const char* tag) {
  (void)op;
  if (!trackerEnabled()) return;

  if (!tag) {
    incrementInvalidTagEvents();
    return;
  }
  const size_t tagLength = strnlen(tag, kMemTrackerTagBytes);
  if (tagLength == 0 || tagLength >= kMemTrackerTagBytes) {
    incrementInvalidTagEvents();
    return;
  }

  SemaphoreHandle_t mutex = sMutex;
  if (!mutex || xSemaphoreTake(mutex, 0) != pdTRUE) {
    incrementContentionDrops();
    return;
  }
  if (!sInitialized || !trackerEnabled()) {
    xSemaphoreGive(mutex);
    return;
  }

  const bool actualFallback =
      ptr != nullptr && requestedPS && !usedPS && fellBack;
  sRegistry.record(tag, tagLength, size, ptr != nullptr, usedPS,
                   actualFallback);
  xSemaphoreGive(mutex);
}
