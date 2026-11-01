#include "Input_GT911.h"
#if ENABLE_GT911_TOUCH
#include "HAL_InputInternal.h"
#include "System_I2C.h"
#include "System_TaskUtils.h"
#include "System_Debug.h"
#include "esp_log.h"

bool gInputRunning = false;
bool gInputConnected = false;
extern TaskHandle_t gInputTaskHandle;
namespace {
StaticSemaphore_t stateStorage;
SemaphoreHandle_t stateLock = xSemaphoreCreateMutexStatic(&stateStorage);
uint8_t address = 0;
TwoWire* wire = nullptr;

bool readRegister(uint16_t reg, uint8_t* data, size_t size) {
  wire->beginTransmission(address);
  wire->write(uint8_t(reg >> 8)); wire->write(uint8_t(reg));
  if (wire->endTransmission(false) != 0 || wire->requestFrom(address, size) != size) return false;
  for (size_t i = 0; i < size; ++i) data[i] = wire->read();
  return true;
}
bool acknowledge() {
  wire->beginTransmission(address);
  wire->write(uint8_t(0x81)); wire->write(uint8_t(0x4e)); wire->write(uint8_t(0));
  return wire->endTransmission() == 0;
}
}
void inputTask(void*) {
  uint32_t lastPoll = millis();
  for (;;) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    const bool paused = pollPaused(WS_TOUCH_I2C_BUS);
    if (gInputRunning && !paused) {
      // Brief shared-bus pauses preserve taps; an interruption over 250 ms
      // cancels before accepting more data from a potentially stale contact.
      if (millis() - lastPoll > 250) inputTouchCancel();
      uint8_t status = 0, point[8]{};
      bool entered = false;
      const bool ok = i2cDeviceTransaction(WS_TOUCH_I2C_BUS, address, 400000, 20, [&]() {
        entered = true;
        if (!readRegister(0x814e, &status, 1)) return false;
        if (!(status & 0x80)) return true;
        if ((status & 15) == 1 && !readRegister(0x814f, point, sizeof(point))) return false;
        return acknowledge();
      });
      // Bus contention is not a device fault. A real I/O error cancels contact.
      if (entered && !ok) {
        inputTouchCancel();
        gInputConnected = false;
      } else if (ok) {
        lastPoll = millis();
        gInputConnected = true;
        if (status & 0x80) {
          const uint16_t x = point[1] | (uint16_t(point[2]) << 8);
          const uint16_t y = point[3] | (uint16_t(point[4]) << 8);
          if ((status & 15) == 1 && (x >= WS_TOUCH_WIDTH || y >= WS_TOUCH_HEIGHT)) inputTouchCancel();
          else inputTouchPublish(status & 15, point[0], x, y, millis());
        }
      }
    }
    xSemaphoreGive(stateLock);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
bool inputStartInternal() {
  if (!stateLock || !inputCacheInit() || xSemaphoreTake(stateLock, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  if (gInputRunning) { xSemaphoreGive(stateLock); return true; }
  inputTouchCancel();
  wire = i2c() ? i2c()->getWire(WS_TOUCH_I2C_BUS) : nullptr;
  address = 0;
  if (wire) {
    for (const uint8_t candidate : {uint8_t(0x5d), uint8_t(0x14)}) {
      if (!i2cPingAddress(candidate, 400000, 100, WS_TOUCH_I2C_BUS)) continue;
      address = candidate;
      uint8_t product[4]{};
      const bool read = i2cDeviceTransaction(WS_TOUCH_I2C_BUS, address, 400000, 100, [&]() {
        return readRegister(0x8140, product, sizeof(product));
      });
      if (read && product[0] == '9' && product[1] == '1' && product[2] == '1') break;
      ESP_LOGE("GT911", "identity probe at 0x%02x: read=%d product=%02x %02x %02x %02x",
               address, read, product[0], product[1], product[2], product[3]);
      address = 0;
    }
  }
  bool ok = address != 0;
  if (ok && !gInputTaskHandle)
    ok = xTaskCreateLogged(inputTask, INPUT_TASK_NAME, INPUT_STACK_WORDS, nullptr,
                          TASK_PRIORITY_LOW, &gInputTaskHandle, INPUT_TASK_TAG, tskNO_AFFINITY) == pdPASS;
  if (ok) {
    inputAbstractionInit();
    ok = inputPublishState(UINT32_MAX, JOYSTICK_CENTER, JOYSTICK_CENTER, 0, false, millis());
  }
  if (!ok && gInputTaskHandle) {
    vTaskDelete(gInputTaskHandle);
    gInputTaskHandle = nullptr;
  }
  gInputRunning = gInputConnected = ok;
  xSemaphoreGive(stateLock);
  if (!ok) ESP_LOGE("GT911", "start failed, address 0x%02x", address);
  else INFO_INPUT_LIFECYCLEF("GT911 single-touch ready at 0x%02x", address);
  return ok;
}
void touchStop() {
  if (!stateLock || xSemaphoreTake(stateLock, pdMS_TO_TICKS(500)) != pdTRUE) return;
  gInputRunning = gInputConnected = false;
  if (gInputTaskHandle) {
    vTaskDelete(gInputTaskHandle);
    gInputTaskHandle = nullptr;
  }
  inputTouchCancel();
  inputCacheInvalidate();
  xSemaphoreGive(stateLock);
}
#endif
