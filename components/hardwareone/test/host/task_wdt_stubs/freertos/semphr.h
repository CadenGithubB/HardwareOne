#pragma once
#include "FreeRTOS.h"

struct FakeSemaphore;
typedef FakeSemaphore* SemaphoreHandle_t;
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t waitTicks);
