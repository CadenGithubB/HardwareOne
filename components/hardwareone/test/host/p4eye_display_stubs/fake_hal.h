#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

struct TwoWire {};
class Adafruit_SSD1306 {
 public:
  Adafruit_SSD1306(int, int, TwoWire*, int) {}
  ~Adafruit_SSD1306() { free(buffer); }
  void clearDisplay() { memset(buffer, 0, 1024); }
  uint8_t* getBuffer() { return buffer; }
 protected:
  uint8_t* buffer = nullptr;
};
#define SSD1306_SETCONTRAST 0x81
#define SSD1306_DISPLAYOFF 0xae
#define SSD1306_DISPLAYON 0xaf
#define SSD1306_INVERTDISPLAY 0xa7
#define SSD1306_NORMALDISPLAY 0xa6
#define OUTPUT 1
#define HIGH 1
#define LOW 0
void pinMode(int, int);
void digitalWrite(int, int);
void delay(int);
bool ledcAttach(int, int, int);
bool ledcWrite(int, int);
bool ledcDetach(int);

using BaseType_t = int;
using TickType_t = uint32_t;
struct FakeSemaphore { int count; };
using SemaphoreHandle_t = FakeSemaphore*;
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
SemaphoreHandle_t xSemaphoreCreateBinary();
SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t*);
void vSemaphoreDelete(SemaphoreHandle_t);

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
const char* esp_err_to_name(int);
#define ESP_LOGE(tag, ...) ((void)(tag))
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
void* heap_caps_malloc(size_t, int);
void heap_caps_free(void*);
using gpio_num_t = int;
void rtc_gpio_hold_dis(gpio_num_t);
void rtc_gpio_deinit(gpio_num_t);
enum spi_host_device_t { SPI2_HOST = 1 };
constexpr int SPI_DMA_CH_AUTO = 0;
struct spi_bus_config_t {
  int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num, quadhd_io_num;
  size_t max_transfer_sz;
};
int spi_bus_initialize(spi_host_device_t, const spi_bus_config_t*, int);
int spi_bus_free(spi_host_device_t);
using esp_lcd_spi_bus_handle_t = int;
using esp_lcd_panel_io_handle_t = void*;
using esp_lcd_panel_handle_t = void*;
struct esp_lcd_panel_io_event_data_t {};
using DoneCallback = bool (*)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*);
struct esp_lcd_panel_io_spi_config_t {
  int cs_gpio_num, dc_gpio_num, spi_mode, pclk_hz, trans_queue_depth;
  DoneCallback on_color_trans_done;
  void* user_ctx;
  int lcd_cmd_bits, lcd_param_bits;
};
constexpr int LCD_RGB_ELEMENT_ORDER_RGB = 0;
struct esp_lcd_panel_dev_config_t { int reset_gpio_num, rgb_ele_order, bits_per_pixel; };
int esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
                           const esp_lcd_panel_io_spi_config_t*, esp_lcd_panel_io_handle_t*);
int esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t,
                            const esp_lcd_panel_dev_config_t*, esp_lcd_panel_handle_t*);
int esp_lcd_panel_reset(esp_lcd_panel_handle_t);
int esp_lcd_panel_init(esp_lcd_panel_handle_t);
int esp_lcd_panel_invert_color(esp_lcd_panel_handle_t, bool);
int esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t, bool);
int esp_lcd_panel_mirror(esp_lcd_panel_handle_t, bool, bool);
int esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool);
int esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void*);
int esp_lcd_panel_del(esp_lcd_panel_handle_t);
int esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t);
