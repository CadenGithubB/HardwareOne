/**
 * Mutex System - Centralized synchronization primitives
 * 
 * All FreeRTOS mutexes and RAII lock guards for thread-safe access
 * to shared resources across tasks (web server, sensors, automation, CLI)
 */

#ifndef MUTEX_SYSTEM_H
#define MUTEX_SYSTEM_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// Global Mutexes (created by initMutexes() in setup())
// ============================================================================

// Filesystem mutex - protects LittleFS access (not thread-safe)
extern SemaphoreHandle_t fsMutex;

// I2C bus mutex - set by initI2CManager() to the manager's recursive mutex
extern SemaphoreHandle_t i2cMutex;

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

// ESP-NOW chunked message mutex - protects gActiveMessage
extern SemaphoreHandle_t gChunkedMsgMutex;

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

/**
 * ChunkedMsgGuard - RAII guard for chunked message state mutex
 */
struct ChunkedMsgGuard {
  bool held;
  explicit ChunkedMsgGuard(const char* owner = nullptr);
  ~ChunkedMsgGuard();
  
  ChunkedMsgGuard(const ChunkedMsgGuard&) = delete;
  ChunkedMsgGuard& operator=(const ChunkedMsgGuard&) = delete;
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

#endif // MUTEX_SYSTEM_H
