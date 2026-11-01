#include "HAL_Display_Waveshare.h"
#if DISPLAY_TYPE == DISPLAY_TYPE_HX8394_WAVESHARE
#include "HAL_Display_WaveshareInit.h"
#include "OLED_TouchLayout.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "hal/mipi_dsi_ll.h"
#include <cstring>
namespace {
constexpr int kWidth = 720, kHeight = 1280;
#if ENABLE_GT911_TOUCH
class PanelCanvas : public Adafruit_GFX {
 public:
  explicit PanelCanvas(uint16_t* pixels) : Adafruit_GFX(kWidth, kHeight), pixels_(pixels) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x >= 0 && y >= 0 && x < kWidth && y < kHeight) pixels_[y*kWidth+x] = color;
  }
 private:
  uint16_t* pixels_;
};
void drawTouchControls(uint16_t* pixels) {
  PanelCanvas canvas(pixels);
  canvas.setTextSize(3);
  canvas.setTextColor(0xffff);
  for (const auto& c : touchControls) {
    canvas.drawRoundRect(c.x, c.y, c.w, c.h, 8, 0xffff);
    canvas.setCursor(c.x + (c.w - strlen(c.label)*18)/2, c.y+27);
    canvas.print(c.label);
  }
}
#endif
constexpr size_t kBytes = kWidth * kHeight * sizeof(uint16_t);
// IDF 5.5's DBI transmit spins indefinitely when its FIFO fills. Drain each
// small command with a deadline so a missing/reset panel cannot block boot.
// This backend is the sole owner of DSI bus 0 and serializes all submissions.
bool waitForCommandDrain() {
  auto *host = MIPI_DSI_LL_GET_HOST(0);
  const uint32_t start = millis();
  while (!mipi_dsi_host_ll_gen_is_cmd_fifo_empty(host) ||
         !mipi_dsi_host_ll_gen_is_write_fifo_empty(host)) {
    if (millis() - start >= 250)
      return false;
    delay(1);
  }
  return true;
}
esp_err_t sendCommand(esp_lcd_panel_io_handle_t io, int command,
                      const void *data, size_t size) {
  if (size > 64)
    return ESP_ERR_INVALID_SIZE;
  if (!waitForCommandDrain())
    return ESP_ERR_TIMEOUT;
  const esp_err_t result = esp_lcd_panel_io_tx_param(io, command, data, size);
  if (result != ESP_OK)
    return result;
  return waitForCommandDrain() ? ESP_OK : ESP_ERR_TIMEOUT;
}
struct Guard {
  SemaphoreHandle_t m;
  explicit Guard(SemaphoreHandle_t s) : m(s) {
    xSemaphoreTake(m, portMAX_DELAY);
  }
  ~Guard() { xSemaphoreGive(m); }
};
} // namespace
WaveshareDisplay::WaveshareDisplay()
    : Adafruit_SSD1306(128, 64, static_cast<TwoWire *>(nullptr), -1) {
  lock_ = xSemaphoreCreateMutex();
}
bool WaveshareDisplay::begin() {
  if (!lock_)
    lock_ = xSemaphoreCreateMutex();
  if (!lock_)
    return false;
  Guard guard(lock_);
  if (panel_)
    return true;
  if (!buffer)
    buffer = static_cast<uint8_t *>(calloc(1024, 1));
  if (!buffer)
    return false;
  pinMode(WS_LCD_RESET_PIN, OUTPUT);
  pinMode(WS_LCD_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(WS_LCD_BACKLIGHT_PIN, LOW);
  pwm_ = ledcAttach(WS_LCD_BACKLIGHT_PIN, 5000, 10);
  if (!pwm_)
    return false;
  // Waveshare's board reset circuit is active-high (BSP reset_active_high=1).
  digitalWrite(WS_LCD_RESET_PIN, LOW);
  delay(10);
  digitalWrite(WS_LCD_RESET_PIN, HIGH);
  delay(10);
  digitalWrite(WS_LCD_RESET_PIN, LOW);
  delay(120);
  esp_ldo_channel_config_t power = {};
  power.chan_id = 3;
  power.voltage_mv = 2500;
  esp_err_t err = esp_ldo_acquire_channel(&power, &ldo_);
  esp_lcd_dsi_bus_config_t bus = {};
  bus.bus_id = 0;
  bus.num_data_lanes = 2;
  bus.lane_bit_rate_mbps = 700;
  if (err == ESP_OK)
    err = esp_lcd_new_dsi_bus(&bus, &bus_);
  esp_lcd_dbi_io_config_t io = {};
  io.virtual_channel = 0;
  io.lcd_cmd_bits = 8;
  io.lcd_param_bits = 8;
  if (err == ESP_OK)
    err = esp_lcd_new_panel_io_dbi(bus_, &io, &io_);
  esp_lcd_dpi_panel_config_t dpi = {};
  dpi.virtual_channel = 0;
  dpi.dpi_clock_freq_mhz = 58;
  dpi.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  dpi.num_fbs = 1;
  dpi.video_timing.h_size = kWidth;
  dpi.video_timing.v_size = kHeight;
  dpi.video_timing.hsync_pulse_width = 20;
  dpi.video_timing.hsync_back_porch = 20;
  dpi.video_timing.hsync_front_porch = 40;
  dpi.video_timing.vsync_pulse_width = 4;
  dpi.video_timing.vsync_back_porch = 10;
  dpi.video_timing.vsync_front_porch = 24;
  if (err == ESP_OK)
    err = esp_lcd_new_panel_dpi(bus_, &dpi, &panel_);
  // Configure the DPI path before the vendor command table, as in the BSP.
  if (err == ESP_OK) {
    for (const auto &cmd : kLcdInit) {
      err = sendCommand(io_, cmd.command, cmd.data, cmd.size);
      if (err != ESP_OK) {
        ESP_LOGE("WaveshareLCD", "command 0x%02x: %s", cmd.command,
                 esp_err_to_name(err));
        break;
      }
      delay(cmd.delayMs ? cmd.delayMs : 2);
    }
  }
  void *frameBuffer = nullptr;
  if (err == ESP_OK)
    err = esp_lcd_dpi_panel_get_frame_buffer(panel_, 1, &frameBuffer);
  pixels_ = static_cast<uint16_t *>(frameBuffer);
  if (err == ESP_OK) {
    memset(pixels_, 0, kBytes);
#if ENABLE_GT911_TOUCH
    drawTouchControls(pixels_);
#endif
    err = esp_cache_msync(pixels_, kBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
  if (err == ESP_OK)
    err = esp_lcd_panel_init(panel_);
  if (err != ESP_OK) {
    ESP_LOGE("WaveshareLCD", "init: %s", esp_err_to_name(err));
    release();
    return false;
  }
  powered_ = true;
  backlight();
  ESP_LOGI("WaveshareLCD",
           "HX8394 ready: 720x1280, 128x64 UI canvas");
  return true;
}
void WaveshareDisplay::display() {
  if (!lock_)
    return;
  Guard guard(lock_);
  if (!pixels_ || !powered_ || !buffer)
    return;
  if (canvasPresented_ &&
      memcmp(presentedCanvas_, buffer, sizeof(presentedCanvas_)) == 0)
    return;
  // Integer scaling preserves crisp text. The unused panel area stays black.
  constexpr int scale = 5, left = (kWidth - 128 * scale) / 2,
                top = (kHeight - 64 * scale) / 2;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 128; x++) {
      const bool on = ((buffer[x + (y / 8) * 128] >> (y % 8)) & 1) != 0;
      const uint16_t color = (on != inverted_) ? 0xffff : 0;
      for (int dy = 0; dy < scale; dy++)
        for (int dx = 0; dx < scale; dx++)
          pixels_[(top + y * scale + dy) * kWidth + left + x * scale + dx] =
              color;
    }
  // Only the centered UI band changed. Avoid flushing the unused panel area,
  // which competes with continuous DSI reads and other PSRAM users.
  const esp_err_t result = esp_cache_msync(
      pixels_ + top * kWidth, 64 * scale * kWidth * sizeof(uint16_t),
      ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  if (result == ESP_OK) {
    memcpy(presentedCanvas_, buffer, sizeof(presentedCanvas_));
    canvasPresented_ = true;
  } else {
    ESP_LOGE("WaveshareLCD", "frame cache sync: %s", esp_err_to_name(result));
  }
}
void WaveshareDisplay::backlight() {
  // This board's AC-coupled backlight strobes near 100% duty, including
  // 1022/1024. A ~70% duty was physically verified stable. Keep sufficient
  // low time even when boot power policy requests logical brightness 255.
  // Arduino also promotes duty 1023 to constant HIGH, so never request it.
  if (pwm_)
    ledcWrite(WS_LCD_BACKLIGHT_PIN, powered_ ? brightness_ * 720u / 255u : 0);
}
void WaveshareDisplay::setBrightness(uint8_t level) {
  if (!lock_)
    return;
  Guard g(lock_);
  brightness_ = level;
  backlight();
}
void WaveshareDisplay::dim(bool dimmed) { setBrightness(dimmed ? 48 : 180); }
void WaveshareDisplay::setPower(bool on) {
  if (!lock_)
    return;
  Guard g(lock_);
  if (!io_)
    return;
  if (sendCommand(io_, on ? 0x29 : 0x28, nullptr, 0) == ESP_OK)
    powered_ = on;
  backlight();
}
void WaveshareDisplay::invertDisplay(bool inverted) {
  if (!lock_)
    return;
  Guard g(lock_);
  if (inverted_ != inverted)
    canvasPresented_ = false;
  inverted_ = inverted;
}
void WaveshareDisplay::ssd1306_command(uint8_t cmd) {
  if (contrast_) {
    contrast_ = false;
    setBrightness(cmd);
  } else if (cmd == SSD1306_SETCONTRAST)
    contrast_ = true;
  else if (cmd == SSD1306_DISPLAYON)
    setPower(true);
  else if (cmd == SSD1306_DISPLAYOFF)
    setPower(false);
  else if (cmd == SSD1306_INVERTDISPLAY)
    invertDisplay(true);
  else if (cmd == SSD1306_NORMALDISPLAY)
    invertDisplay(false);
}
void WaveshareDisplay::release() {
  canvasPresented_ = false;
  powered_ = false;
  backlight();
  if (panel_) {
    esp_lcd_panel_del(panel_);
    panel_ = nullptr;
    pixels_ = nullptr;
  }
  if (io_) {
    esp_lcd_panel_io_del(io_);
    io_ = nullptr;
  }
  if (bus_) {
    esp_lcd_del_dsi_bus(bus_);
    bus_ = nullptr;
  }
  if (ldo_) {
    esp_ldo_release_channel(ldo_);
    ldo_ = nullptr;
  }
  if (pwm_) {
    ledcDetach(WS_LCD_BACKLIGHT_PIN);
    pwm_ = false;
    digitalWrite(WS_LCD_BACKLIGHT_PIN, LOW);
  }
}
void WaveshareDisplay::end() {
  if (!lock_)
    return;
  Guard g(lock_);
  release();
}
#endif
