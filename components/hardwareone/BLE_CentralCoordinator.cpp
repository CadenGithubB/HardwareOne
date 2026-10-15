#include "BLE_CentralCoordinator.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
static StaticSemaphore_t storage;
static SemaphoreHandle_t gate=nullptr;
static portMUX_TYPE initMux=portMUX_INITIALIZER_UNLOCKED;
static std::atomic<unsigned> accessoryWaiters{0},accessoryHolds{0};
// Read/written only by the recursive mutex owner.
static unsigned depth=0;
static bool ownerPhone=false;
bool bleCentralSetupTake(bool phone,uint32_t timeoutMs) {
  portENTER_CRITICAL(&initMux);
  if(!gate)gate=xSemaphoreCreateRecursiveMutexStatic(&storage);
  auto mutex=gate;
  portEXIT_CRITICAL(&initMux);
  if(!mutex || (phone && accessoryWaiters.load()))return false;
  if(!phone)++accessoryWaiters;
  const bool ok=xSemaphoreTakeRecursive(mutex,timeoutMs==UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs))==pdTRUE;
  if(!phone)--accessoryWaiters;
  if(!ok)return false;
  if(depth && ownerPhone!=phone){xSemaphoreGiveRecursive(mutex);return false;}
  if(!depth)ownerPhone=phone;
  ++depth;
  if(!phone)++accessoryHolds;
  return true;
}
void bleCentralSetupGive(){if(!ownerPhone)--accessoryHolds;--depth;xSemaphoreGiveRecursive(gate);}
bool bleCentralAccessoryPending(){return accessoryWaiters.load()!=0;}

bool bleCentralAccessoryBusy(){return accessoryWaiters.load()!=0 || accessoryHolds.load()!=0;}
