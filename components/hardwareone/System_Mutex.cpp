/**
 * Mutex System Implementation
 * 
 * Centralized mutex management and RAII lock guards
 */

#include "System_Mutex.h"
#include "System_Debug.h"
#include "System_I2C_Manager.h"

// ============================================================================
// Global Mutex Definitions
// ============================================================================

SemaphoreHandle_t gFsMutex = nullptr;
SemaphoreHandle_t gJsonResponseMutex = nullptr;
SemaphoreHandle_t gTopoStreamsMutex = nullptr;
SemaphoreHandle_t gEspNowSessionTxMutex = nullptr;
SemaphoreHandle_t gMapCacheMutex = nullptr;
SemaphoreHandle_t i2sMicMutex = nullptr;

// ============================================================================
// Initialization
// ============================================================================

// S-14: the filesystem lock's current holder and the longest hold this boot.
// Written only by the task that holds the lock (take and release), read
// without a lock by fsLockHolder. A copy of the owner tag, so a caller's
// temporary text is never read after the guard is gone. PSRAM where there is
// some (a few dozen bytes, touched once per take and release).
struct FsLockHolderState {
  char     owner[24];
  uint32_t sinceMs;
  bool     held;
  char     longestOwner[24];
  uint32_t longestMs;
};
EXT_RAM_BSS_ATTR static FsLockHolderState sFsHolder;

static void copyLockOwner(char* out, size_t cap, const char* owner) {
  strlcpy(out, (owner && owner[0]) ? owner : "?", cap);
}

static void fsHolderTaken(const char* owner) {
  copyLockOwner(sFsHolder.owner, sizeof(sFsHolder.owner), owner);
  sFsHolder.sinceMs = millis();
  sFsHolder.held = true;
}

static void fsHolderReleased() {
  if (!sFsHolder.held) return;
  const uint32_t heldMs = millis() - sFsHolder.sinceMs;
  if (heldMs >= sFsHolder.longestMs) {
    sFsHolder.longestMs = heldMs;
    memcpy(sFsHolder.longestOwner, sFsHolder.owner, sizeof(sFsHolder.longestOwner));
  }
  sFsHolder.held = false;
}

void fsLockHolder(FsLockHolderInfo* out) {
  if (!out) return;
  const bool held = sFsHolder.held;
  if (held) {
    memcpy(out->owner, sFsHolder.owner, sizeof(out->owner));
    out->heldMs = millis() - sFsHolder.sinceMs;
  } else {
    out->owner[0] = '\0';
    out->heldMs = 0;
  }
  out->owner[sizeof(out->owner) - 1] = '\0';
  memcpy(out->longestOwner, sFsHolder.longestOwner, sizeof(out->longestOwner));
  out->longestOwner[sizeof(out->longestOwner) - 1] = '\0';
  out->longestMs = sFsHolder.longestMs;
}

void initMutexes() {
  FsLockGuard::noteTaken = fsHolderTaken;
  FsLockGuard::noteReleased = fsHolderReleased;
  // Create standard mutexes
  gFsMutex = xSemaphoreCreateMutex();
  gJsonResponseMutex = xSemaphoreCreateMutex();
  gTopoStreamsMutex = xSemaphoreCreateMutex();
  gEspNowSessionTxMutex = xSemaphoreCreateMutex();
  gMapCacheMutex = xSemaphoreCreateMutex();
  i2sMicMutex = xSemaphoreCreateMutex();

  // Log creation status
  bool allCreated = (gFsMutex != nullptr) && (gJsonResponseMutex != nullptr) &&
                    (gTopoStreamsMutex != nullptr) && (gEspNowSessionTxMutex != nullptr) &&
                    (gMapCacheMutex != nullptr) && (i2sMicMutex != nullptr);
  
  if (!allCreated) {
    if (gOutputFlags & MSG_ROUTE_SERIAL) {
      Serial.println("[MUTEX] CRITICAL: Failed to create one or more mutexes!");
    }
  }
}

// Bridge is now handled in initI2cManager() in System_I2C.cpp

// ============================================================================
// Helper: Check if current task holds a mutex
// ============================================================================

static bool isHeldByCurrentTask(SemaphoreHandle_t mutex) {
  if (!mutex) return false;
#if (configUSE_MUTEXES == 1)
  void* holder = xSemaphoreGetMutexHolder(mutex);
  return holder == (void*)xTaskGetCurrentTaskHandle();
#else
  return false;
#endif
}

bool isFsLockedByCurrentTask() {
  return isHeldByCurrentTask(gFsMutex);
}

bool isI2cLockedByCurrentTask() {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  return mgr ? isHeldByCurrentTask(mgr->getBusMutex()) : false;
}

// ============================================================================
// FsLockGuard Implementation
// ============================================================================

FsLockGuard::FsLockGuard(const char* owner)
    : FsLockGuard(owner, portMAX_DELAY) {}

FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks) : held(false) {
  if (gFsMutex) {
    // Reentrant-safe: if already owned by this task, skip
    if (isHeldByCurrentTask(gFsMutex)) {
      // Debug: uncomment to trace reentrant calls
      // Serial.printf("[MUTEX] FsLockGuard reentry (owner=%s)\n", owner ? owner : "");
      return;
    }
    if (xSemaphoreTake(gFsMutex, timeoutTicks) == pdTRUE) {
      held = true;
      if (noteTaken) noteTaken(owner);
    }
  }
}

FsLockGuard::~FsLockGuard() {
  if (held && gFsMutex) {
    if (noteReleased) noteReleased();
    xSemaphoreGive(gFsMutex);
  }
}

// Manual lock/unlock
void fsLock(const char* owner) {
  if (gFsMutex && !isHeldByCurrentTask(gFsMutex)) {
    xSemaphoreTake(gFsMutex, portMAX_DELAY);
    if (FsLockGuard::noteTaken) FsLockGuard::noteTaken(owner);
  }
}

void fsUnlock() {
  if (gFsMutex && isHeldByCurrentTask(gFsMutex)) {
    if (FsLockGuard::noteReleased) FsLockGuard::noteReleased();
    xSemaphoreGive(gFsMutex);
  }
}

bool fsLockForRestart(uint32_t timeoutMs) {
  if (!gFsMutex) return false;
  if (isHeldByCurrentTask(gFsMutex)) return true;
  return xSemaphoreTake(gFsMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

// ============================================================================
// MapCacheGuard Implementation
// ============================================================================

MapCacheGuard::MapCacheGuard(const char* owner) : held(false) {
  if (gMapCacheMutex) {
    // Reentrant-safe: renderMap holds this and calls loadTileData; loadMapFile
    // holds it and calls unloadMap. If this task already owns it, skip the take
    // and leave release to the outer holder.
    if (isHeldByCurrentTask(gMapCacheMutex)) return;
    // portMAX_DELAY matches FsLockGuard: a partial timeout would let the caller
    // proceed WITHOUT the lock and reintroduce the exact evict/free race this
    // guards. Renders are bounded and priority inheritance prevents inversion.
    if (xSemaphoreTake(gMapCacheMutex, portMAX_DELAY) == pdTRUE) {
      held = true;
    }
  }
}

MapCacheGuard::~MapCacheGuard() {
  if (held && gMapCacheMutex) {
    xSemaphoreGive(gMapCacheMutex);
  }
}

I2sMicLockGuard::I2sMicLockGuard(const char* owner) : held(false) {
  if (i2sMicMutex) {
    if (isHeldByCurrentTask(i2sMicMutex)) {
      return;
    }
    if (xSemaphoreTake(i2sMicMutex, portMAX_DELAY) == pdTRUE) {
      held = true;
    }
  }
}

I2sMicLockGuard::~I2sMicLockGuard() {
  if (held && i2sMicMutex) {
    xSemaphoreGive(i2sMicMutex);
  }
}

// ============================================================================
// SensorCacheGuard Implementation
// ============================================================================

SensorCacheGuard::SensorCacheGuard(SemaphoreHandle_t m,
                                    TickType_t timeoutTicks,
                                    const char* owner)
    : held(false), mutex(m) {
  if (mutex) {
    // Reentrant-safe (matches the convention of the other guards in this
    // file): if the current task already holds the mutex, skip the take.
    // held stays false; dtor won't release. The outer holder releases
    // when its scope ends.
    if (isHeldByCurrentTask(mutex)) return;
    if (xSemaphoreTake(mutex, timeoutTicks) == pdTRUE) {
      held = true;
    }
  }
}

SensorCacheGuard::~SensorCacheGuard() {
  if (held && mutex) {
    xSemaphoreGive(mutex);
  }
}


// ============================================================================
// JsonBufferGuard Implementation
// ============================================================================

JsonBufferGuard::JsonBufferGuard(const char* owner) : held(false) {
  if (gJsonResponseMutex) {
    if (isHeldByCurrentTask(gJsonResponseMutex)) {
      return;
    }
    if (xSemaphoreTake(gJsonResponseMutex, portMAX_DELAY) == pdTRUE) {
      held = true;
    }
  }
}

JsonBufferGuard::~JsonBufferGuard() {
  if (held && gJsonResponseMutex) {
    xSemaphoreGive(gJsonResponseMutex);
  }
}


// ============================================================================
// TopoStreamsGuard Implementation
// ============================================================================

TopoStreamsGuard::TopoStreamsGuard(const char* owner) : held(false) {
  if (gTopoStreamsMutex) {
    if (isHeldByCurrentTask(gTopoStreamsMutex)) {
      return;
    }
    if (xSemaphoreTake(gTopoStreamsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      held = true;
    }
  }
}

TopoStreamsGuard::~TopoStreamsGuard() {
  if (held && gTopoStreamsMutex) {
    xSemaphoreGive(gTopoStreamsMutex);
  }
}

// ============================================================================
// EspNowTxGuard Implementation
// ============================================================================

EspNowTxGuard::EspNowTxGuard(const char* owner) : held(false) {
  if (gEspNowSessionTxMutex) {
    // Reentrant-safe: if this task already holds it, skip — the outer holder
    // releases. (sessionWrapFrame isn't nested today, but match the convention
    // so a future nested send can't self-deadlock.)
    if (isHeldByCurrentTask(gEspNowSessionTxMutex)) return;
    // portMAX_DELAY is safe: the guarded section is the txSeqNext bump + AEAD
    // seal (microseconds, no blocking calls), so a waiter is unblocked almost
    // immediately and priority inheritance prevents inversion.
    if (xSemaphoreTake(gEspNowSessionTxMutex, portMAX_DELAY) == pdTRUE) {
      held = true;
    }
  }
}

EspNowTxGuard::~EspNowTxGuard() {
  if (held && gEspNowSessionTxMutex) {
    xSemaphoreGive(gEspNowSessionTxMutex);
  }
}
