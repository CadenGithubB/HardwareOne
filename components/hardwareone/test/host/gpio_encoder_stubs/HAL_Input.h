#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>

using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
using TickType_t = uint32_t;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define ARDUINO_ISR_ATTR
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define CHANGE 4
#define JOYSTICK_CENTER 512

namespace fixture {
struct SampleComplete {};
extern uint32_t now;
extern bool locked;
extern int levels[55], reads[55], modes[55], resets[55];
extern void (*interrupts[55])();
extern int taskCreates, abstractionInits;
extern TickType_t lastDelay;
}

inline void portENTER_CRITICAL(portMUX_TYPE* mux) { (void)mux; }
inline void portEXIT_CRITICAL(portMUX_TYPE* mux) { (void)mux; }
inline void portENTER_CRITICAL_ISR(portMUX_TYPE* mux) { (void)mux; }
inline void portEXIT_CRITICAL_ISR(portMUX_TYPE* mux) { (void)mux; }
inline uint32_t millis() { return fixture::now; }
inline int digitalRead(int pin) { ++fixture::reads[pin]; return fixture::levels[pin]; }
inline void pinMode(int pin, int mode) { fixture::modes[pin] = mode; }
inline void attachInterrupt(int pin, void (*callback)(), int mode) {
  assert(mode == CHANGE);
  fixture::interrupts[pin] = callback;
}
inline void detachInterrupt(int pin) { fixture::interrupts[pin] = nullptr; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return &fixture::locked; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
  (void)wait;
  assert(mutex && !fixture::locked);
  fixture::locked = true;
  return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex && fixture::locked);
  fixture::locked = false;
}
inline void vTaskDelay(TickType_t interval) {
  assert(!fixture::locked);
  fixture::lastDelay = interval;
  throw fixture::SampleComplete{};
}

struct InputCache {
  SemaphoreHandle_t mutex = nullptr;
  uint32_t buttons = UINT32_MAX;
  uint32_t buttonPressedAccum = 0;
  int joyX = JOYSTICK_CENTER, joyY = JOYSTICK_CENTER;
  bool dataValid = false;
  uint32_t seq = 0, lastUpdate = 0;
};
inline void inputAbstractionInit() { ++fixture::abstractionInits; }
