#pragma once

#include <cstdint>

using BaseType_t = int;
using TickType_t = uint32_t;

struct FakeSemaphore;
using SemaphoreHandle_t = FakeSemaphore*;

static constexpr BaseType_t pdFALSE = 0;
static constexpr BaseType_t pdTRUE = 1;

#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
