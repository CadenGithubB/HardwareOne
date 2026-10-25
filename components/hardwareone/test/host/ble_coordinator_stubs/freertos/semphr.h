#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* p){return p;}
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* p){return p;}
// Lets a harness observe coordinator state at the moment a gate take begins.
inline void (*gSemaphoreTakeHook)()=nullptr;
inline int xSemaphoreTake(SemaphoreHandle_t p,TickType_t timeout){
 if(gSemaphoreTakeHook)gSemaphoreTakeHook();
 bool held;
 if(timeout==portMAX_DELAY){p->mutex.lock();held=true;}
 else held=p->mutex.try_lock_for(std::chrono::milliseconds(timeout));
 if(held)p->owner=xTaskGetCurrentTaskHandle();return held?pdTRUE:0;
}
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t p,TickType_t timeout){return xSemaphoreTake(p,timeout);}
inline void xSemaphoreGive(SemaphoreHandle_t p){p->owner=nullptr;p->mutex.unlock();}
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t p){xSemaphoreGive(p);}
inline TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t p){return p->owner.load();}
