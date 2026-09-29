#include "HAL_Display_P4Eye.h"

#if DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
#include "System_Board_P4X_EYE.h"
#include "HAL_Display_P4EyePixels.h"
#include "driver/spi_master.h"
#include "driver/rtc_io.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include <System_MemUtil.h>

namespace {
constexpr char kTag[] = "P4EyeDisplay";
constexpr spi_host_device_t kHost = SPI2_HOST;
// Conservative clock also works with early P4 silicon/IDF clock-source errata.
constexpr int kClockHz = 20 * 1000 * 1000;
constexpr size_t kStripBytes = p4eye_display::kWidth *
                               p4eye_display::kStripRows * sizeof(uint16_t);
}

P4EyeDisplay::P4EyeDisplay()
    : Adafruit_SSD1306(128, 64, static_cast<TwoWire*>(nullptr), -1) {
  lock_ = xSemaphoreCreateMutex();
}

P4EyeDisplay::~P4EyeDisplay() {
  // This private destructor is never used by runtime stop/restart. If lifetime
  // management is changed later, freeing the callback's context before an
  // outstanding DMA finishes would be unsafe, even after a transfer timeout.
  if (transferPending_) {
    xSemaphoreTake(transferred_, portMAX_DELAY);
    transferPending_ = false;
  }
  release();
  if (lock_) vSemaphoreDelete(lock_);
}

bool P4EyeDisplay::transferDone(esp_lcd_panel_io_handle_t,
                               esp_lcd_panel_io_event_data_t*, void* context) {
  auto* self = static_cast<P4EyeDisplay*>(context);
  BaseType_t woke = pdFALSE;
  xSemaphoreGiveFromISR(self->transferred_, &woke);
  return woke == pdTRUE;
}

bool P4EyeDisplay::begin() {
  // An initial low-memory failure is retryable without replacing the object.
  if (!lock_) lock_ = xSemaphoreCreateMutex();
  if (!lock_) return false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  struct Unlock {
    SemaphoreHandle_t lock;
    ~Unlock() { xSemaphoreGive(lock); }
  } unlock{lock_};
  if (panel_ && ready_) {
    if (!waitTransfer()) return false;
    if (esp_lcd_panel_disp_on_off(panel_, true) != ESP_OK) return false;
    powered_ = true;
    applyBacklight();
    return true;
  }
  // A failed init may retain a DMA buffer until completion is observed.
  if (panel_) {
    if (!waitTransfer()) return false;
    release();
  }
  // buffer is owned/freed by the SSD1306 base. Do not call its begin(), which
  // would issue real SSD1306 commands to a nonexistent OLED bus.
  if (!buffer) buffer = static_cast<uint8_t*>(ps_calloc(
      128 * 64 / 8, 1, AllocPolicy::PreferInternal, "display.p4eye.canvas"));
  // The transfer stripe must be DMA-capable; the generic project allocator
  // has no DMA policy. Keep this explicitly constrained allocation internal.
  strip_ = static_cast<uint16_t*>(heap_caps_malloc(
      kStripBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  transferred_ = xSemaphoreCreateBinary();
  if (!buffer || !strip_ || !transferred_) {
    ESP_LOGE(kTag, "Display buffers/semaphores unavailable");
    release();
    return false;
  }

  // GPIO12 powers both LCD and camera. Release any retained RTC hold from a
  // previous sleep, assert the rail, and leave it asserted during panel stop.
  rtc_gpio_hold_dis(static_cast<gpio_num_t>(EYE_LCD_EN));
  rtc_gpio_deinit(static_cast<gpio_num_t>(EYE_LCD_EN));
  pinMode(EYE_LCD_EN, OUTPUT);
  digitalWrite(EYE_LCD_EN, HIGH);
  pinMode(EYE_LCD_BL, OUTPUT);
  digitalWrite(EYE_LCD_BL, EYE_LCD_BL_ACTIVE_LOW ? HIGH : LOW);
  delay(20);

  if (!ledcAttach(EYE_LCD_BL, 5000, 8)) {
    ESP_LOGE(kTag, "Backlight PWM allocation failed");
    release();
    return false;
  }
  pwmAttached_ = true;
  applyBacklight();

  spi_bus_config_t bus = {};
  bus.sclk_io_num = EYE_LCD_SCLK;
  bus.mosi_io_num = EYE_LCD_MOSI;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kStripBytes;
  esp_err_t err = spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO);
  if (err == ESP_OK) ownsBus_ = true;

  esp_lcd_panel_io_spi_config_t ioConfig = {};
  ioConfig.cs_gpio_num = EYE_LCD_CS;
  ioConfig.dc_gpio_num = EYE_LCD_DC;
  ioConfig.spi_mode = 0;
  ioConfig.pclk_hz = kClockHz;
  ioConfig.trans_queue_depth = 1;
  ioConfig.on_color_trans_done = transferDone;
  ioConfig.user_ctx = this;
  ioConfig.lcd_cmd_bits = 8;
  ioConfig.lcd_param_bits = 8;
  if (err == ESP_OK) {
    err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kHost),
                                   &ioConfig, &io_);
  }
  esp_lcd_panel_dev_config_t panelConfig = {};
  panelConfig.reset_gpio_num = EYE_LCD_RST;
  panelConfig.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panelConfig.bits_per_pixel = 16;
  if (err == ESP_OK) err = esp_lcd_new_panel_st7789(io_, &panelConfig, &panel_);
  if (err == ESP_OK) err = esp_lcd_panel_reset(panel_);
  if (err == ESP_OK) err = esp_lcd_panel_init(panel_);
  if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel_, true);
  if (err == ESP_OK) err = esp_lcd_panel_swap_xy(panel_, false);
  if (err == ESP_OK) err = esp_lcd_panel_mirror(panel_, false, false);
  if (err == ESP_OK) {
    clearDisplay();
    // Initialize the entire controller RAM including the black letterbox.
    for (int y = 0; y < p4eye_display::kHeight; y += p4eye_display::kStripRows) {
      p4eye_display::rasterize(buffer, strip_, y, p4eye_display::kStripRows, false);
      if (!sendStrip(y, p4eye_display::kStripRows)) { err = ESP_FAIL; break; }
    }
  }
  if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel_, true);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "ST7789 initialization failed: %s", esp_err_to_name(err));
    release();
    return false;
  }
  powered_ = true;
  ready_ = true;
  applyBacklight();
  return true;
}

bool P4EyeDisplay::sendStrip(int y, int rows) {
  transferPending_ = true;
  const esp_err_t err = esp_lcd_panel_draw_bitmap(panel_, 0, y,
      p4eye_display::kWidth, y + rows, strip_);
  if (err != ESP_OK) {
    transferPending_ = false;
    ESP_LOGE(kTag, "LCD transfer failed: %s", esp_err_to_name(err));
    return false;
  }
  // esp_lcd submits DMA asynchronously. Do not change or free strip_ until the
  // color completion callback fires, including on the final strip of a frame.
  return waitTransfer();
}

bool P4EyeDisplay::waitTransfer() {
  if (!transferPending_) return true;
  if (xSemaphoreTake(transferred_, pdMS_TO_TICKS(1000)) != pdTRUE) {
    // Keep the DMA allocation and transport alive after timeout. A later
    // frame/restart/stop retries completion before reusing or freeing them.
    ESP_LOGE(kTag, "LCD DMA completion timed out");
    return false;
  }
  transferPending_ = false;
  return true;
}

void P4EyeDisplay::display() {
  if (!lock_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  if (!panel_ || !powered_ || !waitTransfer()) {
    xSemaphoreGive(lock_);
    return;
  }
  for (int y = p4eye_display::kImageTop;
       y < p4eye_display::kImageTop + p4eye_display::kImageHeight;
       y += p4eye_display::kStripRows) {
    p4eye_display::rasterize(buffer, strip_, y, p4eye_display::kStripRows, inverted_);
    if (!sendStrip(y, p4eye_display::kStripRows)) break;
  }
  xSemaphoreGive(lock_);
}

void P4EyeDisplay::applyBacklight() {
  if (!pwmAttached_) return;
  const uint8_t level = powered_ ? brightness_ : 0;
  ledcWrite(EYE_LCD_BL, EYE_LCD_BL_ACTIVE_LOW ? 255 - level : level);
}

void P4EyeDisplay::setBrightness(uint8_t level) {
  if (!lock_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  brightness_ = level;
  applyBacklight();
  xSemaphoreGive(lock_);
}

void P4EyeDisplay::dim(bool dimmed) { setBrightness(dimmed ? 64 : 255); }

void P4EyeDisplay::setPower(bool on) {
  if (!lock_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  if (!panel_) { xSemaphoreGive(lock_); return; }
  if (!on) { powered_ = false; applyBacklight(); }
  if (waitTransfer() && esp_lcd_panel_disp_on_off(panel_, on) == ESP_OK) powered_ = on;
  applyBacklight();
  xSemaphoreGive(lock_);
}

void P4EyeDisplay::invertDisplay(bool inverted) {
  if (!lock_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  inverted_ = inverted;
  xSemaphoreGive(lock_);
}

void P4EyeDisplay::ssd1306_command(uint8_t command) {
  // Compatibility for legacy local-display callers. These commands are
  // interpreted locally and never sent to either an I2C bus or the ST7789.
  if (contrastPending_) { contrastPending_ = false; setBrightness(command); }
  else if (command == SSD1306_SETCONTRAST) contrastPending_ = true;
  else if (command == SSD1306_DISPLAYOFF) setPower(false);
  else if (command == SSD1306_DISPLAYON) setPower(true);
  else if (command == SSD1306_INVERTDISPLAY) invertDisplay(true);
  else if (command == SSD1306_NORMALDISPLAY) invertDisplay(false);
}

void P4EyeDisplay::release() {
  ready_ = false;
  powered_ = false;
  applyBacklight();
  if (!waitTransfer()) return;
  if (panel_) { esp_lcd_panel_del(panel_); panel_ = nullptr; }
  if (io_) { esp_lcd_panel_io_del(io_); io_ = nullptr; }
  if (ownsBus_) { spi_bus_free(kHost); ownsBus_ = false; }
  if (pwmAttached_) {
    ledcDetach(EYE_LCD_BL);
    pinMode(EYE_LCD_BL, OUTPUT);
    digitalWrite(EYE_LCD_BL, EYE_LCD_BL_ACTIVE_LOW ? HIGH : LOW);
    pwmAttached_ = false;
  }
  if (transferred_) { vSemaphoreDelete(transferred_); transferred_ = nullptr; }
  if (strip_) { heap_caps_free(strip_); strip_ = nullptr; }
}

void P4EyeDisplay::end() {
  if (!lock_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  release();
  xSemaphoreGive(lock_);
}
#endif
