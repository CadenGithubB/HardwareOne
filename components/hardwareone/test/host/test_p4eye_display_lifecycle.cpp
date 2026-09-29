#include "p4eye_display_stubs/fake_hal.h"
#include "../../HAL_Display_P4Eye.h"
#include <cassert>
#include <cstdio>

namespace {
bool failBus = false, failPanel = false, completeDma = true;
int buses = 0, ios = 0, panels = 0, buffers = 0, pwm = 0, duty = 255;
int transfers = 0;
bool dmaPending = false;
DoneCallback done = nullptr;
void* doneContext = nullptr;
void finishDma() {
  assert(dmaPending && done);
  dmaPending = false;
  done(nullptr, nullptr, doneContext);
}
void released() {
  assert(buses == 0 && ios == 0 && panels == 0 && buffers == 0 && pwm == 0);
}
}

void pinMode(int, int) {}
void digitalWrite(int, int) {}
void delay(int) {}
bool ledcAttach(int, int, int) { ++pwm; return true; }
bool ledcWrite(int, int value) { duty = value; return true; }
bool ledcDetach(int) { --pwm; return true; }
SemaphoreHandle_t xSemaphoreCreateBinary() { return new FakeSemaphore{0}; }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new FakeSemaphore{1}; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) {
  if (!s->count) return pdFALSE;
  --s->count;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { assert(s->count == 0); ++s->count; return pdTRUE; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t*) { return xSemaphoreGive(s); }
void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
const char* esp_err_to_name(int) { return "test error"; }
void* heap_caps_malloc(size_t size, int) { ++buffers; return malloc(size); }
void heap_caps_free(void* p) { assert(!dmaPending); --buffers; free(p); }
void rtc_gpio_hold_dis(gpio_num_t) {}
void rtc_gpio_deinit(gpio_num_t) {}
int spi_bus_initialize(spi_host_device_t, const spi_bus_config_t*, int) {
  if (failBus) return ESP_FAIL;
  assert(buses == 0); ++buses; return ESP_OK;
}
int spi_bus_free(spi_host_device_t) { assert(!dmaPending && buses == 1); --buses; return ESP_OK; }
int esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
    const esp_lcd_panel_io_spi_config_t* config, esp_lcd_panel_io_handle_t* io) {
  done = config->on_color_trans_done; doneContext = config->user_ctx;
  *io = reinterpret_cast<void*>(1); ++ios; return ESP_OK;
}
int esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t,
    const esp_lcd_panel_dev_config_t*, esp_lcd_panel_handle_t* panel) {
  if (failPanel) return ESP_FAIL;
  *panel = reinterpret_cast<void*>(2); ++panels; return ESP_OK;
}
int esp_lcd_panel_reset(esp_lcd_panel_handle_t) { return ESP_OK; }
int esp_lcd_panel_init(esp_lcd_panel_handle_t) { return ESP_OK; }
int esp_lcd_panel_invert_color(esp_lcd_panel_handle_t, bool) { return ESP_OK; }
int esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t, bool) { return ESP_OK; }
int esp_lcd_panel_mirror(esp_lcd_panel_handle_t, bool, bool) { return ESP_OK; }
int esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool) { assert(!dmaPending); return ESP_OK; }
int esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void*) {
  assert(!dmaPending); dmaPending = true; ++transfers;
  if (completeDma) finishDma();
  return ESP_OK;
}
int esp_lcd_panel_del(esp_lcd_panel_handle_t) { assert(!dmaPending); --panels; return ESP_OK; }
int esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t) { assert(!dmaPending); --ios; return ESP_OK; }

int main() {
  // The firmware retains this object for its lifetime, including failed init.
  auto* display = new P4EyeDisplay();
  failBus = true;
  assert(!display->begin());
  released(); // Never free a bus that failed to initialize (may belong to another user).
  failBus = false;
  failPanel = true;
  assert(!display->begin());
  released(); // Partial IO allocation unwinds too.
  failPanel = false;
  assert(display->begin());
  auto* canvas = display->getBuffer();
  assert(canvas && transfers == 30 && duty == 0);
  display->setBrightness(64);
  assert(duty == 191); // The board backlight is active low.
  display->setPower(false);
  assert(duty == 255);
  display->setBrightness(128);
  assert(duty == 255); // Brightness changes must not wake a sleeping panel.
  display->setPower(true);
  assert(duty == 127);

  completeDma = false;
  display->display(); // Timeout: keep the in-flight strip and callback alive.
  const int beforeRetry = transfers;
  display->display();
  assert(transfers == beforeRetry && dmaPending);
  display->end();
  assert(buses == 1 && buffers == 1 && duty == 255);
  finishDma();
  display->end();
  released();
  assert(display->getBuffer() == canvas); // No use-after-free for a render already in progress.
  display->display(); // Stopped transport is a safe no-op.
  assert(transfers == beforeRetry);

  completeDma = true;
  assert(display->begin());
  assert(display->getBuffer() == canvas && duty == 127);
  display->display();
  display->end();
  display->end(); // Idempotent.
  released();
  std::puts("P4-EYE display lifecycle tests passed");
}
