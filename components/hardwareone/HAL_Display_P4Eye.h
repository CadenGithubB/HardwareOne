#pragma once

#include "System_BuildConfig.h"

#if DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
#include <Adafruit_SSD1306.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Keep the established 128x64, page-packed UI surface, including getBuffer()
// consumers and Adafruit_SSD1306* drawing helpers. The base is used ONLY as a
// software canvas: its begin/display/command methods must never reach I2C.
// Panel transport and all resources belong to this concrete driver.
class P4EyeDisplay : public Adafruit_SSD1306 {
 public:
  P4EyeDisplay();
  bool begin();
  void end();
  void display();
  void dim(bool dimmed);
  void setBrightness(uint8_t level);
  void setPower(bool on);
  void invertDisplay(bool inverted);
  void ssd1306_command(uint8_t command);

 private:
  // The UI exposes the canvas to legacy drawing helpers on several tasks.
  // Keep its lifetime stable; end() releases hardware but never the canvas.
  ~P4EyeDisplay();
  static bool transferDone(esp_lcd_panel_io_handle_t,
                           esp_lcd_panel_io_event_data_t*, void* context);
  bool sendStrip(int y, int rows);
  bool waitTransfer();
  void applyBacklight();
  void release();
  esp_lcd_panel_io_handle_t io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  SemaphoreHandle_t transferred_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;
  uint16_t* strip_ = nullptr;
  bool ownsBus_ = false;
  bool pwmAttached_ = false;
  bool powered_ = false;
  bool inverted_ = false;
  bool contrastPending_ = false;
  bool transferPending_ = false;
  bool ready_ = false;
  uint8_t brightness_ = 255;
};
#endif
