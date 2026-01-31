/**
 * HAL_Display.cpp - Display Hardware Abstraction Layer Implementation
 * 
 * Provides unified display initialization and control functions that work
 * with both OLED (I2C) and TFT (SPI) displays based on DISPLAY_TYPE.
 */

#include "HAL_Display.h"
#include "System_BuildConfig.h"

#if DISPLAY_ENABLED

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  #include "System_I2C.h"
  #include <Wire.h>
#endif

// Global display instance
DisplayDriver* gDisplay = nullptr;

/**
 * Initialize display hardware based on DISPLAY_TYPE
 * Returns true on success, false on failure
 */
bool displayInit() {
  if (gDisplay) {
    return true;  // Already initialized
  }
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // ============================================================================
  // I2C OLED (SSD1306) Initialization
  // ============================================================================
  extern TwoWire Wire1;  // System uses Wire1 for all I2C sensors
  gDisplay = new Adafruit_SSD1306(DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire1, DISPLAY_RESET_PIN);
  if (!gDisplay) {
    return false;
  }
  
  // Use I2C transaction wrapper for thread-safe initialization
  bool success = i2cDeviceTransaction(DISPLAY_I2C_ADDR, 100000, 100, [&]() -> bool {
    return gDisplay->begin(SSD1306_SWITCHCAPVCC, DISPLAY_I2C_ADDR);
  });
  
  if (!success) {
    delete gDisplay;
    gDisplay = nullptr;
    return false;
  }
  
  // Clear the display buffer
  gDisplay->clearDisplay();
  gDisplay->display();
  return true;
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // ============================================================================
  // SPI TFT (ST7789) Initialization
  // ============================================================================
  gDisplay = new Adafruit_ST7789(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);
  if (!gDisplay) {
    return false;
  }
  
  // Initialize TFT with correct dimensions
  gDisplay->init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  
  // Set rotation (0=portrait, 1=landscape, 2=inverted portrait, 3=inverted landscape)
  gDisplay->setRotation(0);
  
  // Clear screen to black
  gDisplay->fillScreen(DISPLAY_COLOR_BLACK);
  
  // Optional: Initialize backlight if pin is defined
  #if DISPLAY_BL_PIN >= 0
    pinMode(DISPLAY_BL_PIN, OUTPUT);
    digitalWrite(DISPLAY_BL_PIN, HIGH);  // Turn on backlight
  #endif
  
  return true;
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  // ============================================================================
  // SPI TFT (ILI9341) Initialization - PLACEHOLDER
  // ============================================================================
  gDisplay = new Adafruit_ILI9341(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);
  if (!gDisplay) {
    return false;
  }
  
  gDisplay->begin();
  gDisplay->setRotation(0);
  gDisplay->fillScreen(DISPLAY_COLOR_BLACK);
  
  #if DISPLAY_BL_PIN >= 0
    pinMode(DISPLAY_BL_PIN, OUTPUT);
    digitalWrite(DISPLAY_BL_PIN, HIGH);
  #endif
  
  return true;
  
#else
  return false;
#endif
}

/**
 * Clear entire display
 * OLED: Clears framebuffer (requires display() to show)
 * TFT: Fills screen with black (immediate)
 */
void displayClear() {
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  gDisplay->clearDisplay();
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789 || DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  gDisplay->fillScreen(DISPLAY_COLOR_BLACK);
#endif
}

/**
 * Update display (push framebuffer to screen)
 * OLED: Pushes framebuffer to display (required)
 * TFT: No-op (direct rendering, already on screen)
 */
void displayUpdate() {
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // LOW PRIORITY: Try-lock with short timeout - yield to gamepad/high-pri I2C devices
  if (i2cMutex && xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
    // Bus is busy - skip this refresh, try again next cycle
    return;
  }
  gDisplay->display();  // Push framebuffer to OLED
  if (i2cMutex) xSemaphoreGiveRecursive(i2cMutex);
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789 || DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  // No-op for TFT (direct rendering)
#endif
}

/**
 * Dim display (brightness control)
 * OLED: Uses built-in dim() function
 * TFT: Controls backlight PWM if available
 */
void displayDim(bool dim) {
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  gDisplay->dim(dim);
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789 || DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  // TFT dimming via backlight PWM (if connected)
  #if DISPLAY_BL_PIN >= 0
    analogWrite(DISPLAY_BL_PIN, dim ? 64 : 255);
  #endif
#endif
}

/**
 * Set display brightness (0-255)
 * OLED: Uses ssd1306_command for contrast control
 * TFT: Controls backlight PWM if available
 */
void displaySetBrightness(uint8_t level) {
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // SSD1306 contrast control via command interface
  // Contrast range is 0x00-0xFF, maps directly to level
  gDisplay->ssd1306_command(0x81);  // Set Contrast Control
  gDisplay->ssd1306_command(level);
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789 || DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  // TFT brightness via backlight PWM
  #if DISPLAY_BL_PIN >= 0
    analogWrite(DISPLAY_BL_PIN, level);
  #endif
#endif
}

#endif // DISPLAY_ENABLED
