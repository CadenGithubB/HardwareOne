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
SemaphoreHandle_t gMeshRetryMutex = nullptr;
SemaphoreHandle_t gFileTransferMutex = nullptr;
SemaphoreHandle_t gTopoStreamsMutex = nullptr;
SemaphoreHandle_t gEspNowSessionTxMutex = nullptr;
SemaphoreHandle_t gMapCacheMutex = nullptr;
SemaphoreHandle_t i2sMicMutex = nullptr;

// ============================================================================
// Initialization
// ============================================================================

void initMutexes() {
  // Create standard mutexes
  gFsMutex = xSemaphoreCreateMutex();
  gJsonResponseMutex = xSemaphoreCreateMutex();
  gMeshRetryMutex = xSemaphoreCreateMutex();
  gFileTransferMutex = xSemaphoreCreateMutex();
  gTopoStreamsMutex = xSemaphoreCreateMutex();
  gEspNowSessionTxMutex = xSemaphoreCreateMutex();
  gMapCacheMutex = xSemaphoreCreateMutex();
  i2sMicMutex = xSemaphoreCreateMutex();

  // i2cMutex removed — I2cLockGuard and i2cLock/Unlock go through I2CDeviceManager::getBusMutex() directly

  // Log creation status
  bool allCreated = (gFsMutex != nullptr) && (gJsonResponseMutex != nullptr) &&
                    (gMeshRetryMutex != nullptr) && (gFileTransferMutex != nullptr) &&
                    (gTopoStreamsMutex != nullptr) && (gEspNowSessionTxMutex != nullptr) &&
                    (gMapCacheMutex != nullptr) && (i2sMicMutex != nullptr);
  
  if (!allCreated) {
    if (gOutputFlags & OUTPUT_SERIAL) {
      Serial.println("[MUTEX] CRITICAL: Failed to create one or more mutexes!");
    }
  }
}

// Bridge is now handled in initI2CManager() in System_I2C.cpp

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
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  return mgr ? isHeldByCurrentTask(mgr->getBusMutex()) : false;
}

// ============================================================================
// FsLockGuard Implementation
// ============================================================================

FsLockGuard::FsLockGuard(const char* owner) : held(false) {
  if (gFsMutex) {
    // Reentrant-safe: if already owned by this task, skip
    if (isHeldByCurrentTask(gFsMutex)) {
      // Debug: uncomment to trace reentrant calls
      // Serial.printf("[MUTEX] FsLockGuard reentry (owner=%s)\n", owner ? owner : "");
      return;
    }
    if (xSemaphoreTake(gFsMutex, portMAX_DELAY) == pdTRUE) {
      held = true;
    }
  }
}

FsLockGuard::~FsLockGuard() {
  if (held && gFsMutex) {
    xSemaphoreGive(gFsMutex);
  }
}

// Manual lock/unlock
void fsLock(const char* owner) {
  if (gFsMutex && !isHeldByCurrentTask(gFsMutex)) {
    xSemaphoreTake(gFsMutex, portMAX_DELAY);
  }
}

void fsUnlock() {
  if (gFsMutex && isHeldByCurrentTask(gFsMutex)) {
    xSemaphoreGive(gFsMutex);
  }
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

// ============================================================================
// I2cLockGuard Implementation
// ============================================================================

I2cLockGuard::I2cLockGuard(const char* owner) : held(false) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  SemaphoreHandle_t m = mgr ? mgr->getBusMutex() : nullptr;
  if (m && xSemaphoreTake(m, portMAX_DELAY) == pdTRUE) {
    held = true;
  }
}

I2cLockGuard::~I2cLockGuard() {
  if (held) {
    I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
    SemaphoreHandle_t m = mgr ? mgr->getBusMutex() : nullptr;
    if (m) xSemaphoreGive(m);
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

void i2cLock(const char* owner) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  SemaphoreHandle_t m = mgr ? mgr->getBusMutex() : nullptr;
  if (m) xSemaphoreTake(m, portMAX_DELAY);
}

void i2cUnlock() {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  SemaphoreHandle_t m = mgr ? mgr->getBusMutex() : nullptr;
  if (m) xSemaphoreGive(m);
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
// MeshRetryGuard Implementation
// ============================================================================

MeshRetryGuard::MeshRetryGuard(const char* owner) : held(false) {
  if (gMeshRetryMutex) {
    if (isHeldByCurrentTask(gMeshRetryMutex)) {
      return;
    }
    if (xSemaphoreTake(gMeshRetryMutex, portMAX_DELAY) == pdTRUE) {
      held = true;
    }
  }
}

MeshRetryGuard::~MeshRetryGuard() {
  if (held && gMeshRetryMutex) {
    xSemaphoreGive(gMeshRetryMutex);
  }
}

// ============================================================================
// FileTransferGuard Implementation
// ============================================================================

FileTransferGuard::FileTransferGuard(const char* owner) : held(false) {
  if (gFileTransferMutex) {
    if (isHeldByCurrentTask(gFileTransferMutex)) {
      return;
    }
    if (xSemaphoreTake(gFileTransferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      held = true;
    }
  }
}

FileTransferGuard::~FileTransferGuard() {
  if (held && gFileTransferMutex) {
    xSemaphoreGive(gFileTransferMutex);
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

