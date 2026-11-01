#pragma once
#include "System_BuildConfig.h"
#if DISPLAY_TYPE == DISPLAY_TYPE_HX8394_WAVESHARE
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "freertos/semphr.h"
#include <Adafruit_SSD1306.h>

// Software-only SSD1306 canvas preserves the established Hardware One UI API.
class WaveshareDisplay : public Adafruit_SSD1306 {
public:
  WaveshareDisplay();
  bool begin();
  void end();
  void display();
  bool paintSurface(void (*paint)(Adafruit_GFX&));
  void dim(bool dimmed);
  void setBrightness(uint8_t level);
  void setPower(bool on);
  void invertDisplay(bool inverted);
  void ssd1306_command(uint8_t command);

private:
  void release();
  void backlight();
  SemaphoreHandle_t lock_ = nullptr;
  esp_lcd_dsi_bus_handle_t bus_ = nullptr;
  esp_lcd_panel_io_handle_t io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  esp_ldo_channel_handle_t ldo_ = nullptr;
  uint16_t *pixels_ = nullptr;
  uint8_t presentedCanvas_[1024] = {};
  bool canvasPresented_ = false;
  bool wakePending_ = false;
  bool pwm_ = false, powered_ = false, inverted_ = false, contrast_ = false;
  uint8_t brightness_ = 180;
};
#endif
