#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstdint>
using TickType_t=uint32_t;
constexpr int pdTRUE=1;
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(x) (x)
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
using TaskHandle_t=void*;
struct StaticSemaphore_t {std::recursive_timed_mutex mutex;std::atomic<TaskHandle_t> owner{nullptr};};
using SemaphoreHandle_t=StaticSemaphore_t*;
inline std::atomic<TickType_t> fakeTicks{100};
inline TickType_t xTaskGetTickCount(){return fakeTicks.load();}
inline TaskHandle_t xTaskGetCurrentTaskHandle(){static thread_local int task;return &task;}
