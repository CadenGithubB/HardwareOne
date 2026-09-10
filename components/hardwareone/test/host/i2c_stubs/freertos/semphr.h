#pragma once

#include "FreeRTOS.h"

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t waitTicks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
