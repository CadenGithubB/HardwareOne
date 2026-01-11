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

// I2C bus mutex - protects Wire/Wire1 transactions
extern SemaphoreHandle_t i2cMutex;

// I2C health tracking mutex - protects device health array and clock stack
extern SemaphoreHandle_t i2cHealthMutex;

// JSON response buffer mutex - protects shared gJsonResponseBuffer
extern SemaphoreHandle_t gJsonResponseMutex;

// ESP-NOW mesh retry queue mutex
extern SemaphoreHandle_t gMeshRetryMutex;

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
