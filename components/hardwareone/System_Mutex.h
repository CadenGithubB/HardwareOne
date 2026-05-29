/**
 * Mutex System - Centralized synchronization primitives
 * 
 * All FreeRTOS mutexes and RAII lock guards for thread-safe access
 * to shared resources across tasks (web server, sensors, automation, CLI)
 */

#ifndef SYSTEM_MUTEX_H
#define SYSTEM_MUTEX_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// Shared Timing Constants
// ============================================================================

// Timeout for acquiring a sensor-cache mutex. Used by sensor task loops and
// web/HTTP/OLED handlers that lock a per-cache semaphore. A short, finite
// timeout (vs. portMAX_DELAY) keeps the caller unblocked if the other side
// stalls.
//
// Canonical home — was previously duplicated in System_I2C.h and
// WebServer_Utils.h with a coordinating HW_CACHE_MUTEX_TIMEOUT_MS guard to
// dodge redefinition errors. Both duplicates have been removed.
static constexpr uint32_t CACHE_MUTEX_TIMEOUT_MS = 100;

// ============================================================================
// Global Mutexes (created by initMutexes() in setup())
// ============================================================================

// Filesystem mutex - protects LittleFS access (not thread-safe)
extern SemaphoreHandle_t gFsMutex;

// JSON response buffer mutex - protects shared gJsonResponseBuffer
extern SemaphoreHandle_t gJsonResponseMutex;

// ESP-NOW mesh retry queue mutex
extern SemaphoreHandle_t gMeshRetryMutex;

// ESP-NOW file transfer mutex - protects gActiveFileTransfer state
extern SemaphoreHandle_t gFileTransferMutex;

extern SemaphoreHandle_t i2sMicMutex;

// ============================================================================
// Initialization - call once in setup()
// ============================================================================

// Creates all mutexes. Call early in setup() before any tasks are created.
void initMutexes();

// ============================================================================
// RAII Lock Guards - automatic lock/unlock via scope
// ============================================================================

/**
 * FsLockGuard - RAII guard for filesystem mutex
 * 
 * Reentrant-safe: if the current task already holds the mutex,
 * it won't try to take it again (avoids deadlock on nested calls)
 * 
 * Usage:
 *   {
 *     FsLockGuard guard("myFunction");
 *     File f = LittleFS.open(...);
 *     // ... file operations ...
 *   } // automatically unlocks when guard goes out of scope
 */
struct FsLockGuard {
  bool held;
  explicit FsLockGuard(const char* owner = nullptr);
  ~FsLockGuard();
  
  // Non-copyable
  FsLockGuard(const FsLockGuard&) = delete;
  FsLockGuard& operator=(const FsLockGuard&) = delete;
};

/**
 * I2cLockGuard - RAII guard for I2C bus mutex
 * 
 * Acquires I2CDeviceManager::getBusMutex() directly — no legacy i2cMutex global.
 * 
 * Usage:
 *   {
 *     I2cLockGuard guard("sensorRead");
 *     Wire1.beginTransmission(...);
 *     // ... I2C operations ...
 *   }
 */
struct I2cLockGuard {
  bool held;
  explicit I2cLockGuard(const char* owner = nullptr);
  ~I2cLockGuard();
  
  I2cLockGuard(const I2cLockGuard&) = delete;
  I2cLockGuard& operator=(const I2cLockGuard&) = delete;
};

struct I2sMicLockGuard {
  bool held;
  explicit I2sMicLockGuard(const char* owner = nullptr);
  ~I2sMicLockGuard();

  I2sMicLockGuard(const I2sMicLockGuard&) = delete;
  I2sMicLockGuard& operator=(const I2sMicLockGuard&) = delete;
};

/**
 * SensorCacheGuard - RAII guard for per-sensor cache mutexes.
 *
 * Sensor cache structs (gImuCache, gTofCache, gGpsCache, gThermalCache,
 * gApdsCache, gInputCache, gRtcCache, gPresenceCache, gFmRadioCache) all
 * follow the convention `SemaphoreHandle_t mutex = nullptr;` as their first
 * field. This guard takes that mutex by handle. Unlike the other guards in
 * this file, it has no "the" mutex — each sensor cache has its own, so the
 * handle is stored in the guard for the destructor.
 *
 * Timeouts are caller-specified because reads (UI, snapshot) and writes
 * (driver task) have different patience: UI uses 5ms ("show '...' if busy"),
 * writes use 100ms ("must succeed"). The default of CACHE_MUTEX_TIMEOUT_MS
 * (100ms, defined above) matches the most common driver-side usage.
 *
 * Reentrant-safe: if the current task already holds the mutex, the guard
 * skips the take (held stays false). Matches the convention of the other
 * guards above.
 *
 * Usage (block-scoped read/write):
 *   {
 *     SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(5), "g2.imuRead");
 *     if (g.held) {
 *       // ... read/write gImuCache.* fields ...
 *       if (errorCondition) return "ERROR";  // dtor releases automatically
 *     }
 *   }
 *
 * Usage (function-scoped with intentional early release):
 *   void someTask() {
 *     {
 *       SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(10), "imu.snapshot");
 *       if (!g.held) return;
 *       // copy cache fields into locals
 *     }  // ← guard released here, BEFORE the work below
 *     // do work on locals without holding the lock
 *   }
 */
struct SensorCacheGuard {
  bool held;
  SemaphoreHandle_t mutex;  // remembered for destructor (each cache has its own)

  explicit SensorCacheGuard(SemaphoreHandle_t m,
                             TickType_t timeoutTicks = pdMS_TO_TICKS(CACHE_MUTEX_TIMEOUT_MS),
                             const char* owner = nullptr);
  ~SensorCacheGuard();

  // Non-copyable, non-movable — RAII guards shouldn't move.
  SensorCacheGuard(const SensorCacheGuard&) = delete;
  SensorCacheGuard& operator=(const SensorCacheGuard&) = delete;
};

/**
 * JsonBufferGuard - RAII guard for JSON response buffer mutex
 * 
 * Usage:
 *   {
 *     JsonBufferGuard guard("httpHandler");
 *     snprintf(gJsonResponseBuffer, JSON_RESPONSE_SIZE, ...);
 *     // ... use buffer ...
 *   }
 */
struct JsonBufferGuard {
  bool held;
  explicit JsonBufferGuard(const char* owner = nullptr);
  ~JsonBufferGuard();
  
  JsonBufferGuard(const JsonBufferGuard&) = delete;
  JsonBufferGuard& operator=(const JsonBufferGuard&) = delete;
};

/**
 * MeshRetryGuard - RAII guard for ESP-NOW retry queue mutex
 */
struct MeshRetryGuard {
  bool held;
  explicit MeshRetryGuard(const char* owner = nullptr);
  ~MeshRetryGuard();
  
  MeshRetryGuard(const MeshRetryGuard&) = delete;
  MeshRetryGuard& operator=(const MeshRetryGuard&) = delete;
};

/**
 * FileTransferGuard - RAII guard for ESP-NOW file transfer state mutex
 */
struct FileTransferGuard {
  bool held;
  explicit FileTransferGuard(const char* owner = nullptr);
  ~FileTransferGuard();
  
  FileTransferGuard(const FileTransferGuard&) = delete;
  FileTransferGuard& operator=(const FileTransferGuard&) = delete;
};

// ESP-NOW topology streams mutex - protects gTopoStreams, gTopoDeviceCache, gPeerBuffer
extern SemaphoreHandle_t gTopoStreamsMutex;

/**
 * TopoStreamsGuard - RAII guard for topology streams state mutex
 */
struct TopoStreamsGuard {
  bool held;
  explicit TopoStreamsGuard(const char* owner = nullptr);
  ~TopoStreamsGuard();

  TopoStreamsGuard(const TopoStreamsGuard&) = delete;
  TopoStreamsGuard& operator=(const TopoStreamsGuard&) = delete;
};

// ESP-NOW session TX mutex — serializes the per-session AEAD send critical
// section in sessionWrapFrame() (the ++txSeqNext nonce-counter bump + seal).
//
// The session layer was historically lock-free because espnow_task was the
// ONLY task that sent on a session. That invariant broke when bonded sensor
// streaming began calling the encrypted-send path from SENSOR_BCAST_TASK
// (core 1) at 10 Hz while espnow_task (core 0) and cmd_exec_task also send
// heartbeats/ACKs/bond-sync on the same session. Two cores racing
// `++s->txSeqNext` produce duplicate frameSeq → nonce reuse (a crypto break,
// and the receiver replay-rejects the dup). This mutex makes the bump+seal
// atomic across every sender. Critical section is microseconds (no blocking
// calls inside), so portMAX_DELAY in the guard never meaningfully blocks.
extern SemaphoreHandle_t gEspNowSessionTxMutex;

/**
 * EspNowTxGuard - RAII guard for the ESP-NOW session TX critical section.
 * Reentrant-safe (matches the other guards): if the calling task already
 * holds the mutex, skip the take and leave release to the outer holder.
 */
struct EspNowTxGuard {
  bool held;
  explicit EspNowTxGuard(const char* owner = nullptr);
  ~EspNowTxGuard();

  EspNowTxGuard(const EspNowTxGuard&) = delete;
  EspNowTxGuard& operator=(const EspNowTxGuard&) = delete;
};

// ============================================================================
// Helper Functions
// ============================================================================

// Manual lock/unlock for cases where RAII isn't suitable
void fsLock(const char* owner = nullptr);
void fsUnlock();

void i2cLock(const char* owner = nullptr);
void i2cUnlock();

// Check if current task holds a mutex (for debugging/assertions)
bool isFsLockedByCurrentTask();
bool isI2cLockedByCurrentTask();

#endif // SYSTEM_MUTEX_H
