#pragma once
// Host stand-in for the FreeRTOS types System_TaskWdt.h uses. 1 tick = 1 ms.
#include <cstdint>

typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
