#include "Input_GPIOEncoder.h"

#if ENABLE_GPIO_ENCODER
#include "HAL_Input.h"
#include "Input_RotaryCore.h"
#if ENABLE_GPIO_ENCODER_BUTTONS
#include "Input_ButtonCore.h"
#endif
#include "System_Debug.h"
#include "System_TaskUtils.h"
#include <driver/gpio.h>
#include <limits.h>

#ifndef GPIO_ENCODER_STEPS_PER_DETENT
#define GPIO_ENCODER_STEPS_PER_DETENT 4
#endif
#ifndef GPIO_ENCODER_INVERT
#define GPIO_ENCODER_INVERT 0
#endif
#ifndef GPIO_ENCODER_BUTTON_ACTIVE_LOW
#define GPIO_ENCODER_BUTTON_ACTIVE_LOW 1
#endif
static_assert(GPIO_ENCODER_STEPS_PER_DETENT >= 1 && GPIO_ENCODER_STEPS_PER_DETENT <= 4,
              "GPIO encoder requires 1..4 phase edges per detent");
static_assert(GPIO_ENCODER_PIN_A != GPIO_ENCODER_PIN_B &&
              GPIO_ENCODER_PIN_A != GPIO_ENCODER_PIN_BUTTON &&
              GPIO_ENCODER_PIN_B != GPIO_ENCODER_PIN_BUTTON, "GPIO encoder pins must be distinct");

bool gInputRunning = false;
bool gInputConnected = false;
InputCache gInputCache;

static portMUX_TYPE sEncoderMux = portMUX_INITIALIZER_UNLOCKED;
static rotary::Quadrature sDecoder;
static rotary::Gestures sGestures;
static int sCapturedDetents = 0;  // ISR writes, task drains under sEncoderMux.
static int sPendingDetents = 0;   // UI consumes under cache mutex.
static int32_t sPosition = 0;
extern TaskHandle_t gInputTaskHandle;  // Shared slot used by task/memory monitors.
static constexpr int kPendingLimit = 1024;
static constexpr uint32_t kSampleMs = 5;

#if ENABLE_GPIO_ENCODER_BUTTONS
struct AuxiliaryButtonConfig {
  int pin;
  bool activeLow;
  uint32_t action;
};
static constexpr AuxiliaryButtonConfig kAuxButtons[] = GPIO_ENCODER_AUX_BUTTONS;
static constexpr size_t kAuxButtonCount = sizeof(kAuxButtons) / sizeof(kAuxButtons[0]);
static gpioinput::Button sAuxButtons[kAuxButtonCount];
static uint32_t sAuxPressedActions = 0;

static bool auxiliaryPressed(size_t index) {
  return digitalRead(kAuxButtons[index].pin) == (kAuxButtons[index].activeLow ? LOW : HIGH);
}

static bool auxiliaryPinsValid() {
  for (size_t i = 0; i < kAuxButtonCount; ++i) {
    const int pin = kAuxButtons[i].pin;
    if (!GPIO_IS_VALID_GPIO(pin) || pin == GPIO_ENCODER_PIN_A ||
        pin == GPIO_ENCODER_PIN_B || pin == GPIO_ENCODER_PIN_BUTTON) return false;
    for (size_t j = 0; j < i; ++j) if (pin == kAuxButtons[j].pin) return false;
  }
  return true;
}
#endif

static void ARDUINO_ISR_ATTR encoderEdge() {
  const uint8_t ab = (digitalRead(GPIO_ENCODER_PIN_A) << 1) | digitalRead(GPIO_ENCODER_PIN_B);
  portENTER_CRITICAL_ISR(&sEncoderMux);
  int detent = sDecoder.update(ab, GPIO_ENCODER_STEPS_PER_DETENT);
  if (GPIO_ENCODER_INVERT) detent = -detent;
  if ((detent > 0 && sCapturedDetents < kPendingLimit) ||
      (detent < 0 && sCapturedDetents > -kPendingLimit)) sCapturedDetents += detent;
  portEXIT_CRITICAL_ISR(&sEncoderMux);
}

static bool buttonPressed() {
  return digitalRead(GPIO_ENCODER_PIN_BUTTON) == (GPIO_ENCODER_BUTTON_ACTIVE_LOW ? LOW : HIGH);
}

static void clearCacheLocked() {
  gInputCache.buttons = UINT32_MAX;  // Active-low cache: all buttons released.
  gInputCache.buttonPressedAccum = 0;
  gInputCache.joyX = gInputCache.joyY = JOYSTICK_CENTER;
  gInputCache.dataValid = false;
  sPendingDetents = 0;
#if ENABLE_GPIO_ENCODER_BUTTONS
  sAuxPressedActions = 0;
#endif
}

static void stopLocked() {
  gInputRunning = gInputConnected = false;
  detachInterrupt(GPIO_ENCODER_PIN_A);
  detachInterrupt(GPIO_ENCODER_PIN_B);
  gpio_reset_pin(static_cast<gpio_num_t>(GPIO_ENCODER_PIN_A));
  gpio_reset_pin(static_cast<gpio_num_t>(GPIO_ENCODER_PIN_B));
  gpio_reset_pin(static_cast<gpio_num_t>(GPIO_ENCODER_PIN_BUTTON));
#if ENABLE_GPIO_ENCODER_BUTTONS
  for (const auto& button : kAuxButtons) gpio_reset_pin(static_cast<gpio_num_t>(button.pin));
#endif
  portENTER_CRITICAL(&sEncoderMux);
  sCapturedDetents = 0;
  portEXIT_CRITICAL(&sEncoderMux);
  clearCacheLocked();
}

void inputTask(void*) {
  for (;;) {
    if (xSemaphoreTake(gInputCache.mutex, portMAX_DELAY) == pdTRUE) {
      if (gInputRunning) {
        portENTER_CRITICAL(&sEncoderMux);
        const int detents = sCapturedDetents;
        sCapturedDetents = 0;
        portEXIT_CRITICAL(&sEncoderMux);
        const uint32_t now = millis();
        const bool previousPress = sGestures.pressed();
        const rotary::Events events = sGestures.update(buttonPressed(), now, detents);
        uint32_t heldActions = 0;
        uint32_t pressedActions = events.actions;
        bool auxiliaryChanged = false;
#if ENABLE_GPIO_ENCODER_BUTTONS
        for (size_t i = 0; i < kAuxButtonCount; ++i) {
          if (sAuxButtons[i].update(auxiliaryPressed(i), now)) pressedActions |= kAuxButtons[i].action;
          if (sAuxButtons[i].pressed()) heldActions |= kAuxButtons[i].action;
        }
        auxiliaryChanged = heldActions != sAuxPressedActions;
        sAuxPressedActions = heldActions;
#endif
        const int64_t position = int64_t(sPosition) + detents;
        sPosition = static_cast<int32_t>(position > INT32_MAX ? INT32_MAX :
                                       position < INT32_MIN ? INT32_MIN : position);
        sPendingDetents += events.detents;
        if (sPendingDetents > kPendingLimit) sPendingDetents = kPendingLimit;
        if (sPendingDetents < -kPendingLimit) sPendingDetents = -kPendingLimit;
        gInputCache.buttons = ~(heldActions | events.actions);
        gInputCache.buttonPressedAccum |= pressedActions;
        gInputCache.lastUpdate = now;
        gInputCache.dataValid = true;
        if (detents || events.actions || auxiliaryChanged || previousPress != sGestures.pressed()) ++gInputCache.seq;
      }
      xSemaphoreGive(gInputCache.mutex);
    }
    // Phase transitions use interrupts. Switches and gesture timers are sampled
    // here; slow I2C polling settings cannot lose turns or delay button sampling.
    const TickType_t interval = pdMS_TO_TICKS(gInputRunning ? kSampleMs : 100);
    vTaskDelay(interval ? interval : 1);
  }
}

bool inputStartInternal() {
  if (!GPIO_IS_VALID_GPIO(GPIO_ENCODER_PIN_A) || !GPIO_IS_VALID_GPIO(GPIO_ENCODER_PIN_B) ||
      !GPIO_IS_VALID_GPIO(GPIO_ENCODER_PIN_BUTTON)) return false;
#if ENABLE_GPIO_ENCODER_BUTTONS
  if (!auxiliaryPinsValid()) return false;
#endif
  if (!gInputCache.mutex) gInputCache.mutex = xSemaphoreCreateMutex();
  if (!gInputCache.mutex || xSemaphoreTake(gInputCache.mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  if (gInputRunning) { xSemaphoreGive(gInputCache.mutex); return true; }
  pinMode(GPIO_ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(GPIO_ENCODER_PIN_B, INPUT_PULLUP);
  pinMode(GPIO_ENCODER_PIN_BUTTON, GPIO_ENCODER_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
#if ENABLE_GPIO_ENCODER_BUTTONS
  for (size_t i = 0; i < kAuxButtonCount; ++i) {
    pinMode(kAuxButtons[i].pin, kAuxButtons[i].activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
    sAuxButtons[i].reset(auxiliaryPressed(i), millis());
  }
#endif
  portENTER_CRITICAL(&sEncoderMux);
  sCapturedDetents = 0;
  sDecoder.reset((digitalRead(GPIO_ENCODER_PIN_A) << 1) | digitalRead(GPIO_ENCODER_PIN_B));
  portEXIT_CRITICAL(&sEncoderMux);
  sGestures.reset(buttonPressed(), millis());
  clearCacheLocked();
  sPosition = 0;
  inputAbstractionInit();
  attachInterrupt(GPIO_ENCODER_PIN_A, encoderEdge, CHANGE);
  attachInterrupt(GPIO_ENCODER_PIN_B, encoderEdge, CHANGE);
  // Retain the task across close/open, like the other optional input drivers.
  // Stopping detaches interrupts, releases pins and invalidates all cached input.
  if (!gInputTaskHandle && xTaskCreateLogged(inputTask, INPUT_TASK_NAME, INPUT_STACK_WORDS, nullptr,
                                       TASK_PRIORITY_LOW, &gInputTaskHandle, INPUT_TASK_TAG,
                                       tskNO_AFFINITY) != pdPASS) {
    stopLocked();
    xSemaphoreGive(gInputCache.mutex);
    return false;
  }
  gInputCache.dataValid = true;
  gInputCache.lastUpdate = millis();
  gInputRunning = gInputConnected = true;
  xSemaphoreGive(gInputCache.mutex);
  INFO_INPUT_LIFECYCLEF("GPIO encoder started (A=%d B=%d click=%d)",
                        GPIO_ENCODER_PIN_A, GPIO_ENCODER_PIN_B, GPIO_ENCODER_PIN_BUTTON);
#if ENABLE_GPIO_ENCODER_BUTTONS
  INFO_INPUT_LIFECYCLEF("GPIO encoder auxiliary buttons enabled (%u keys)", static_cast<unsigned>(kAuxButtonCount));
#endif
  return true;
}

void gpioEncoderStop() {
  if (!gInputCache.mutex || xSemaphoreTake(gInputCache.mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  if (gInputRunning) stopLocked();
  xSemaphoreGive(gInputCache.mutex);
}

bool gpioEncoderHasPendingDetents() {
  if (!gInputCache.mutex || xSemaphoreTake(gInputCache.mutex, 0) != pdTRUE) return false;
  const bool pending = gInputRunning && sPendingDetents != 0;
  xSemaphoreGive(gInputCache.mutex);
  return pending;
}

int gpioEncoderConsumeOneDetent() {
  if (!gInputCache.mutex || xSemaphoreTake(gInputCache.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
  const int step = (sPendingDetents > 0) - (sPendingDetents < 0);
  sPendingDetents -= step;
  xSemaphoreGive(gInputCache.mutex);
  return step;
}

int gpioEncoderBuildDataJSON(char* buf, size_t size) {
  if (!buf || !size) return 0;
  if (!gInputCache.mutex || xSemaphoreTake(gInputCache.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    const int n = snprintf(buf, size, "{\"connected\":false,\"valid\":false}");
    return n > 0 && static_cast<size_t>(n) < size ? n : 0;
  }
  const int n = snprintf(buf, size,
      "{\"connected\":%s,\"running\":%s,\"valid\":%s,\"ts\":%lu,\"pos\":%ld,\"axis\":0,"
      "\"pendingDetents\":%d,\"buttons\":%lu,\"pressed\":%s,\"sampleMs\":%lu,\"auxButtonsEnabled\":%s}",
      gInputConnected ? "true" : "false", gInputRunning ? "true" : "false",
      gInputCache.dataValid ? "true" : "false", static_cast<unsigned long>(gInputCache.lastUpdate),
      static_cast<long>(sPosition), sPendingDetents,
      static_cast<unsigned long>(gInputCache.dataValid ? ~gInputCache.buttons : 0),
      gInputRunning && sGestures.pressed() ? "true" : "false", static_cast<unsigned long>(kSampleMs),
      ENABLE_GPIO_ENCODER_BUTTONS ? "true" : "false");
  xSemaphoreGive(gInputCache.mutex);
  return n > 0 && static_cast<size_t>(n) < size ? n : 0;
}
#endif  // ENABLE_GPIO_ENCODER
