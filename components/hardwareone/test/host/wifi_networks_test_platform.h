#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <cstdlib>
#include <mutex>

String operator+(const char* left, const String& right);
void broadcastOutput(const String& message);
void logSystemEvent(const char*, const char* format, ...);
#define ERROR_WIFIF(...) ((void)0)
#define DEBUG_WIFIF(...) ((void)0)
#define WARN_WIFIF(...) ((void)0)
#define DEBUG_STORAGEF(...) ((void)0)
#define ERROR_STORAGEF(...) ((void)0)
#define WARN_STORAGEF(...) ((void)0)

extern bool filesystemReady;
struct FakeWifiSettings { bool wifiAutoStart = false; };
extern FakeWifiSettings gSettings;
bool requestSettingsPersist();
void notifySettingChanged(const void* field);
void putSecret(JsonObject object, const char* key, const String& plaintext);
void putSecretPreserving(JsonObject object, const char* key, const String& plaintext,
                         const String& previousBlob);
String getSecret(JsonObjectConst object, const char* key);
void systemEventPost(int kind, const char* source, const char* detail = nullptr);
static constexpr int SYSEVT_SECRET_DECRYPT_FAILED = 1;
static constexpr int SYSEVT_WIFI_NET_REMOVED = 2;

enum class AllocPref { RequireInternal };
inline bool failNetworkAllocation = false;
inline size_t networkAllocationCount = 0;
inline void* ps_alloc(size_t size, AllocPref preference, const char*) {
  assert(preference == AllocPref::RequireInternal);
  ++networkAllocationCount;
  return failNetworkAllocation ? nullptr : std::malloc(size);
}
inline void ps_free(void* pointer) { std::free(pointer); }

// Real host synchronization beneath the production lock scopes. The depth
// check also verifies persistence cannot inherit the saved-list lock.
using StaticSemaphore_t = std::recursive_mutex;
using SemaphoreHandle_t = StaticSemaphore_t*;
using TickType_t = unsigned;
inline thread_local int wifiTestSemaphoreDepth = 0;
static constexpr int pdTRUE = 1;
static constexpr TickType_t portMAX_DELAY = ~TickType_t(0);
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* storage) { return storage; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) {
  mutex->lock(); ++wifiTestSemaphoreDepth; return pdTRUE;
}
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t wait) { return xSemaphoreTake(mutex, wait); }
inline void xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(wifiTestSemaphoreDepth > 0); --wifiTestSemaphoreDepth; mutex->unlock();
}
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) { xSemaphoreGive(mutex); }
using portMUX_TYPE = std::recursive_mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE* mutex) { mutex->lock(); }
inline void portEXIT_CRITICAL(portMUX_TYPE* mutex) { mutex->unlock(); }
