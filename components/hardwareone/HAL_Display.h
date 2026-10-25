/**
 * HAL_Display.h - Display Hardware Abstraction Layer
 * 
 * Provides compile-time display type selection and hardware-agnostic macros.
 * This allows the same OLED/display code to work with different hardware by
 * changing DISPLAY_TYPE in System_BuildConfig.h.
 * 
 * Currently supported:
 *   - DISPLAY_TYPE_SSD1306: 128x64 monochrome OLED (I2C)
 *   - DISPLAY_TYPE_ST7789: 240x320 color IPS TFT (SPI) - Adafruit 2.0" EYESPI
 *   - DISPLAY_TYPE_ST7789_P4_EYE: onboard 240x240 ST7789, 128x128 logical UI
 * 
 * Future support planned:
 *   - DISPLAY_TYPE_ILI9341: 320x240 color TFT (SPI)
 */

#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include <stdint.h>

#include "System_BuildConfig.h"

// =============================================================================
// Display Type Selection
// =============================================================================

#if DISPLAY_TYPE == DISPLAY_TYPE_NONE
  // No display - stub everything out
  #define DISPLAY_ENABLED       0
  #define DISPLAY_WIDTH         0
  #define DISPLAY_HEIGHT        0
  #define DISPLAY_COLOR_DEPTH   0
  #define DISPLAY_IS_COLOR      0
  #define DISPLAY_INTERFACE     "none"
  #define DISPLAY_NAME          "None"

#elif DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // SSD1306 128x64 Monochrome OLED (I2C)
  #include <Adafruit_SSD1306.h>
  
  #define DISPLAY_ENABLED       1
  #define DISPLAY_WIDTH         128
  #define DISPLAY_HEIGHT        64
  #define DISPLAY_COLOR_DEPTH   1    // 1-bit monochrome
  #define DISPLAY_IS_COLOR      0
  #define DISPLAY_INTERFACE     "i2c"
  #define DISPLAY_NAME          "SSD1306 OLED"
  
  // I2C-specific settings
  #define DISPLAY_I2C_ADDR      0x3C  // Default, can also be 0x3D
  #define DISPLAY_I2C_ADDR_ALT  0x3D
  #define DISPLAY_RESET_PIN     -1    // -1 = no reset pin
  
  // Type aliases for hardware abstraction
  typedef Adafruit_SSD1306 DisplayDriver;
  
  // Color definitions (monochrome)
  #define DISPLAY_COLOR_BLACK   SSD1306_BLACK
  #define DISPLAY_COLOR_WHITE   SSD1306_WHITE
  #define DISPLAY_COLOR_INVERSE SSD1306_INVERSE
  
  // Default foreground/background for themes
  #define DISPLAY_FG            SSD1306_WHITE
  #define DISPLAY_BG            SSD1306_BLACK

#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
  #include "HAL_Display_P4Eye.h"
  #define DISPLAY_ENABLED       1
  // Keep OLED text wrapping; add rows on the taller software canvas.
  #include "HAL_Display_P4EyePixels.h"
  #define DISPLAY_WIDTH         P4EYE_CANVAS_WIDTH
  #define DISPLAY_HEIGHT        P4EYE_CANVAS_HEIGHT
  #define DISPLAY_PANEL_WIDTH   240
  #define DISPLAY_PANEL_HEIGHT  240
  #define DISPLAY_COLOR_DEPTH   1
  #define DISPLAY_IS_COLOR      0
  #define DISPLAY_INTERFACE     "spi"
  #define DISPLAY_NAME          "P4-EYE ST7789"
  typedef P4EyeDisplay DisplayDriver;
  #define DISPLAY_COLOR_BLACK   SSD1306_BLACK
  #define DISPLAY_COLOR_WHITE   SSD1306_WHITE
  #define DISPLAY_COLOR_INVERSE SSD1306_INVERSE
  #define DISPLAY_FG            SSD1306_WHITE
  #define DISPLAY_BG            SSD1306_BLACK

#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // ST7789 2.0" 320x240 Color IPS TFT (SPI) - Adafruit EYESPI
  // Product: https://www.adafruit.com/product/4311
  // Guide: https://learn.adafruit.com/adafruit-2-0-320x240-color-ips-tft-display
  #include <Adafruit_ST7789.h>
  
  #define DISPLAY_ENABLED       1
  #define DISPLAY_WIDTH         240
  #define DISPLAY_HEIGHT        320   // 2.0" IPS display is 240x320
  #define DISPLAY_COLOR_DEPTH   16    // RGB565
  #define DISPLAY_IS_COLOR      1
  #define DISPLAY_INTERFACE     "spi"
  #define DISPLAY_NAME          "ST7789 2.0\" IPS"
  
  // SPI-specific settings (customize for your ESP32 wiring)
  // These are example pins - adjust based on your actual hardware connections
  #define DISPLAY_SPI_CS        5     // TFT Chip Select
  #define DISPLAY_SPI_DC        16    // Data/Command select
  #define DISPLAY_SPI_RST       17    // Reset (can be -1 if using auto-reset)
  #define DISPLAY_SPI_MOSI      23    // SPI MOSI (hardware SPI)
  #define DISPLAY_SPI_SCLK      18    // SPI Clock (hardware SPI)
  #define DISPLAY_SPI_MISO      19    // SPI MISO (for SD card if present)
  
  // Backlight control (optional)
  #define DISPLAY_BL_PIN        -1    // PWM backlight pin (-1 = always on)
  
  // SD card (if using the microSD slot on the breakout)
  #define DISPLAY_SD_CS         15    // SD card chip select
  
  typedef Adafruit_ST7789 DisplayDriver;
  
  // Color definitions (RGB565 - 16-bit color)
  #define DISPLAY_COLOR_BLACK   ST77XX_BLACK
  #define DISPLAY_COLOR_WHITE   ST77XX_WHITE
  #define DISPLAY_COLOR_RED     ST77XX_RED
  #define DISPLAY_COLOR_GREEN   ST77XX_GREEN
  #define DISPLAY_COLOR_BLUE    ST77XX_BLUE
  #define DISPLAY_COLOR_CYAN    ST77XX_CYAN
  #define DISPLAY_COLOR_MAGENTA ST77XX_MAGENTA
  #define DISPLAY_COLOR_YELLOW  ST77XX_YELLOW
  #define DISPLAY_COLOR_ORANGE  ST77XX_ORANGE
  
  // Default foreground/background for themes
  #define DISPLAY_FG            ST77XX_WHITE
  #define DISPLAY_BG            ST77XX_BLACK

#elif DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  // ILI9341 Color TFT (SPI) - PLACEHOLDER for future implementation
  #include <Adafruit_ILI9341.h>
  
  #define DISPLAY_ENABLED       1
  #define DISPLAY_WIDTH         320
  #define DISPLAY_HEIGHT        240
  #define DISPLAY_COLOR_DEPTH   16   // RGB565
  #define DISPLAY_IS_COLOR      1
  #define DISPLAY_INTERFACE     "spi"
  #define DISPLAY_NAME          "ILI9341 TFT"
  
  // SPI-specific settings (customize for your wiring)
  #define DISPLAY_SPI_CS        5
  #define DISPLAY_SPI_DC        16
  #define DISPLAY_SPI_RST       17
  
  typedef Adafruit_ILI9341 DisplayDriver;
  
  // Color definitions (RGB565)
  #define DISPLAY_COLOR_BLACK   ILI9341_BLACK
  #define DISPLAY_COLOR_WHITE   ILI9341_WHITE
  #define DISPLAY_FG            ILI9341_WHITE
  #define DISPLAY_BG            ILI9341_BLACK

#else
  #error "Unknown DISPLAY_TYPE. Check System_BuildConfig.h"
#endif

// =============================================================================
// Common Display Macros (derived from display dimensions)
// =============================================================================

#if DISPLAY_ENABLED

// Monochrome canvases keep compact chrome as more content rows are added.
#include "OLED_Layout.h"

// Header configuration
#if !DISPLAY_IS_COLOR
  #define DISPLAY_HEADER_HEIGHT   oled_layout::kHeaderHeight
#elif DISPLAY_HEIGHT <= 64
  #define DISPLAY_HEADER_HEIGHT   10
#elif DISPLAY_HEIGHT <= 128
  #define DISPLAY_HEADER_HEIGHT   12
#else
  #define DISPLAY_HEADER_HEIGHT   16
#endif

// Footer configuration
#if !DISPLAY_IS_COLOR
  #define DISPLAY_FOOTER_HEIGHT   oled_layout::kFooterHeight
#elif DISPLAY_HEIGHT <= 64
  #define DISPLAY_FOOTER_HEIGHT   10
#elif DISPLAY_HEIGHT <= 128
  #define DISPLAY_FOOTER_HEIGHT   16
#else
  #define DISPLAY_FOOTER_HEIGHT   20
#endif

// Content area is between header and footer (with 1px gap after header)
#define DISPLAY_CONTENT_HEIGHT  (DISPLAY_HEIGHT - DISPLAY_HEADER_HEIGHT - DISPLAY_FOOTER_HEIGHT - 1)
#define DISPLAY_CONTENT_START_Y (DISPLAY_HEADER_HEIGHT + 1)

// Convenience macros for common coordinates
#define DISPLAY_CENTER_X        (DISPLAY_WIDTH / 2)
#define DISPLAY_CENTER_Y        (DISPLAY_HEIGHT / 2)
#define DISPLAY_LAST_X          (DISPLAY_WIDTH - 1)
#define DISPLAY_LAST_Y          (DISPLAY_HEIGHT - 1)

#endif // DISPLAY_ENABLED

// =============================================================================
// Legacy Compatibility Macros
// =============================================================================
// These map old SCREEN_* names to new DISPLAY_* names for backward compatibility.
// New code should use DISPLAY_* macros directly.

#if DISPLAY_ENABLED
  #ifndef SCREEN_WIDTH
    #define SCREEN_WIDTH          DISPLAY_WIDTH
  #endif
  #ifndef SCREEN_HEIGHT
    #define SCREEN_HEIGHT         DISPLAY_HEIGHT
  #endif
  #ifndef OLED_HEADER_HEIGHT
    #define OLED_HEADER_HEIGHT    DISPLAY_HEADER_HEIGHT
  #endif
  #ifndef OLED_FOOTER_HEIGHT
    #define OLED_FOOTER_HEIGHT    DISPLAY_FOOTER_HEIGHT
  #endif
  #ifndef OLED_CONTENT_HEIGHT
    #define OLED_CONTENT_HEIGHT   DISPLAY_CONTENT_HEIGHT
  #endif
  #ifndef OLED_CONTENT_START_Y
    #define OLED_CONTENT_START_Y  DISPLAY_CONTENT_START_Y
  #endif
#endif

// =============================================================================
// Display Runtime Functions
// =============================================================================
#if DISPLAY_ENABLED

// Global display instance (replaces oledDisplay)
extern DisplayDriver* gDisplay;

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
// Address selected by the successful 0x3D/0x3C probe. All manager wrappers
// must key health and failure probes to the same address the library uses.
extern uint8_t gDisplayI2cAddress;
uint8_t displayI2cBus();  // Latched for the canvas lifetime; bus settings require reboot.
#endif

// Legacy compatibility alias
#define oledDisplay gDisplay

// Display control functions
// Hardware lifetime belongs to the HAL. A retained canvas is not proof of
// readiness; callers must use displayIsReady(). Stop keeps borrowed UI pointers
// valid. Successful initialization means the probe and library setup ran, not
// that every write was acknowledged (some drivers provide no write result).
// Task contract: the lifecycle state here is not synchronized. The main loop
// owns it (power-save tick, UI pump); cmd_exec's lightsleep/deepsleep call
// displayPrepareForSleep/displayResumeFromSleep as well, and that overlap is
// benign only because displayPrepareForSleep is idempotent (off always wins,
// the rail is cut once). Routing those command paths through the main loop is
// the follow-up; until then no third task may call these.
bool displayInit();
bool displayIsReady();
void displayStop();
bool displaySetPower(bool on);            // Whether the power operation ran
void displayPrepareForSleep();
bool displayResumeFromSleep();            // Restores only a previously active panel
void displayClear();                     // Clear entire display
void displayUpdate();                    // Update display (no-op for TFT, display() for OLED)
void displayDim(bool dim);               // Dim display (on/off brightness)
void displaySetBrightness(uint8_t level); // Set brightness 0-255 (PWM for TFT, contrast for OLED)

#else
inline bool displayInit() { return false; }
inline bool displayIsReady() { return false; }
inline void displayStop() {}
inline bool displaySetPower(bool) { return false; }
inline void displayPrepareForSleep() {}
inline bool displayResumeFromSleep() { return false; }
inline void displayClear() {}
inline void displayUpdate() {}
inline void displayDim(bool) {}
inline void displaySetBrightness(uint8_t) {}
#endif // DISPLAY_ENABLED

#endif // HAL_DISPLAY_H
