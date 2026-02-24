/**
 * Mutex System Implementation
 * 
 * Centralized mutex management and RAII lock guards
 */

#include "System_Mutex.h"
#include "System_Debug.h"

// ============================================================================
// Global Mutex Definitions
// ============================================================================

SemaphoreHandle_t fsMutex = nullptr;
SemaphoreHandle_t i2cMutex = nullptr;  // Set by initI2CManager() bridge — do not allocate here
SemaphoreHandle_t gJsonResponseMutex = nullptr;
SemaphoreHandle_t gMeshRetryMutex = nullptr;
SemaphoreHandle_t gFileTransferMutex = nullptr;
SemaphoreHandle_t gTopoStreamsMutex = nullptr;
SemaphoreHandle_t gChunkedMsgMutex = nullptr;
SemaphoreHandle_t i2sMicMutex = nullptr;

// ============================================================================
// Initialization
// ============================================================================

void initMutexes() {
  // Create standard mutexes
  fsMutex = xSemaphoreCreateMutex();
  gJsonResponseMutex = xSemaphoreCreateMutex();
  gMeshRetryMutex = xSemaphoreCreateMutex();
  gFileTransferMutex = xSemaphoreCreateMutex();
  gTopoStreamsMutex = xSemaphoreCreateMutex();
  gChunkedMsgMutex = xSemaphoreCreateMutex();
  i2sMicMutex = xSemaphoreCreateMutex();
  
  // i2cMutex is NOT allocated here — initI2CManager() bridges it to the manager's busMutex

  // Log creation status
  bool allCreated = (fsMutex != nullptr) && (gJsonResponseMutex != nullptr) && 
                    (gMeshRetryMutex != nullptr) && (gFileTransferMutex != nullptr) &&
                    (gTopoStreamsMutex != nullptr) && (gChunkedMsgMutex != nullptr) &&
                    (i2sMicMutex != nullptr);
  
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
  return isHeldByCurrentTask(fsMutex);
}

bool isI2cLockedByCurrentTask() {
  // Regular mutex has ownership tracking
  return isHeldByCurrentTask(i2cMutex);
}

// ============================================================================
// FsLockGuard Implementation
// ============================================================================

FsLockGuard::FsLockGuard(const char* owner) : held(false) {
  if (fsMutex) {
    // Reentrant-safe: if already owned by this task, skip
    if (isHeldByCurrentTask(fsMutex)) {
      // Debug: uncomment to trace reentrant calls
      // Serial.printf("[MUTEX] FsLockGuard reentry (owner=%s)\n", owner ? owner : "");
      return;
    }
    if (xSemaphoreTake(fsMutex, portMAX_DELAY) == pdTRUE) {
      held = true;
    }
  }
}

FsLockGuard::~FsLockGuard() {
  if (held && fsMutex) {
    xSemaphoreGive(fsMutex);
  }
}

// Manual lock/unlock
void fsLock(const char* owner) {
  if (fsMutex && !isHeldByCurrentTask(fsMutex)) {
    xSemaphoreTake(fsMutex, portMAX_DELAY);
  }
}

void fsUnlock() {
  if (fsMutex && isHeldByCurrentTask(fsMutex)) {
    xSemaphoreGive(fsMutex);
  }
}

// ============================================================================
// I2cLockGuard Implementation
// ============================================================================
// i2cMutex is the manager's recursive mutex (set by initI2CManager bridge)

I2cLockGuard::I2cLockGuard(const char* owner) : held(false) {
  if (i2cMutex) {
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      held = true;
    }
  }
}

I2cLockGuard::~I2cLockGuard() {
  if (held && i2cMutex) {
    xSemaphoreGive(i2cMutex);
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

void i2cLock(const char* owner) {
  if (i2cMutex) {
    xSemaphoreTake(i2cMutex, portMAX_DELAY);
  }
}

void i2cUnlock() {
  if (i2cMutex) {
    xSemaphoreGive(i2cMutex);
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
// ChunkedMsgGuard Implementation
// ============================================================================

ChunkedMsgGuard::ChunkedMsgGuard(const char* owner) : held(false) {
  if (gChunkedMsgMutex) {
    if (isHeldByCurrentTask(gChunkedMsgMutex)) {
      return;
    }
    if (xSemaphoreTake(gChunkedMsgMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      held = true;
    }
  }
}

ChunkedMsgGuard::~ChunkedMsgGuard() {
  if (held && gChunkedMsgMutex) {
    xSemaphoreGive(gChunkedMsgMutex);
  }
}
