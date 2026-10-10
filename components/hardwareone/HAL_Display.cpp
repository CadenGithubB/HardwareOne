/**
 * HAL_Display.cpp - Display Hardware Abstraction Layer Implementation
 * 
 * Provides unified display initialization and control functions that work
 * with both OLED (I2C) and TFT (SPI) displays based on DISPLAY_TYPE.
 */

#include "HAL_Display.h"
#include "System_BuildConfig.h"
#include <atomic>

#if DISPLAY_ENABLED

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  #include "System_I2C.h"
  #include "System_Settings.h"  // gSettings.oledBus for dual-bus routing
  #include <Wire.h>
#endif

// Global display instance
DisplayDriver* gDisplay = nullptr;
namespace {
std::atomic<bool> displayReady{false};
bool resumeAfterSleep = false;
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
constexpr uint32_t kOledClockHz = 400000;
uint8_t oledBus = 0;  // Latched when the retained canvas is first published.
#if defined(I2C2_POWER_PIN) && (I2C2_POWER_PIN >= 0)
bool displayRailOff = false;
#endif
bool oledNeedsInit = false;

// The library restores the same rate it uses, so it cannot invalidate the
// manager's clock cache. Wire initialization remains owned by the manager.
bool beginOled(DisplayDriver* display, uint8_t bus, uint8_t address, uint32_t waitMs) {
  const bool initialized = i2cDeviceTransaction(bus, address, kOledClockHz, waitMs, [&]() -> bool {
    return display->begin(SSD1306_SWITCHCAPVCC, address, true, false);
  });
  if (!initialized || !oledNeedsInit) return initialized;
  // Keep the post-power-cycle workaround on both wake and manual retry.
  return i2cDeviceTransaction(bus, address, kOledClockHz, 200, [&]() -> bool {
    display->ssd1306_command(SSD1306_DISPLAYOFF);
    display->ssd1306_command(SSD1306_CHARGEPUMP);
    display->ssd1306_command(0x14);
    display->ssd1306_command(SSD1306_DISPLAYON);
    return true;
  });
}

void restoreDisplayRail() {
#if defined(I2C2_POWER_PIN) && (I2C2_POWER_PIN >= 0)
  if (displayRailOff) {
    pinMode(I2C2_POWER_PIN, OUTPUT);
    digitalWrite(I2C2_POWER_PIN, HIGH);
    delay(100);  // Retain the panel's power-on settling interval.
    if (auto* mgr = i2c()) mgr->performBusRecovery(1);
    displayRailOff = false;
  }
#endif
}
#endif
} // namespace

bool displayIsReady() { return displayReady.load(); }
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
uint8_t gDisplayI2cAddress = DISPLAY_I2C_ADDR_ALT;
uint8_t displayI2cBus() {
  return gDisplay ? oledBus : static_cast<uint8_t>(gSettings.oledBus);
}
#endif

/**
 * Initialize display hardware based on DISPLAY_TYPE
 * Returns true on success, false on failure
 */
bool displayInit() {
  if (displayIsReady()) return true;

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  restoreDisplayRail();
  const uint8_t bus = displayI2cBus();
  TwoWire* wire = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!wire) return false;

  // Boot and manual start share the same cold-panel retry policy. Address
  // replies establish presence, not device identity or successful rendering.
  uint8_t detectedAddr = 0;
  const uint8_t addresses[] = {DISPLAY_I2C_ADDR_ALT, DISPLAY_I2C_ADDR};
  for (int attempt = 0; attempt < 3 && !detectedAddr; ++attempt) {
    if (attempt) delay(50);
    for (uint8_t address : addresses) {
      if (i2cPingAddress(address, 100000, 200, bus)) {
        detectedAddr = address;
        break;
      }
    }
  }
  if (!detectedAddr) return false;

  DisplayDriver* candidate = gDisplay;
  if (!candidate) {
    candidate = new Adafruit_SSD1306(DISPLAY_WIDTH, DISPLAY_HEIGHT, wire,
                                     DISPLAY_RESET_PIN, kOledClockHz, kOledClockHz);
  }
  if (!candidate) return false;
  if (!beginOled(candidate, bus, detectedAddr, 500)) {
    // An unpublished failure can be freed safely. A published canvas may
    // still be borrowed by UI code and is retained for the next attempt.
    if (!gDisplay) delete candidate;
    return false;
  }
  oledNeedsInit = false;
  oledBus = bus;
  gDisplayI2cAddress = detectedAddr;
  gDisplay = candidate;
  if (auto* mgr = i2c()) mgr->registerDevice(detectedAddr, "SSD1306", bus);
  displayReady = true;
  resumeAfterSleep = false;
  displayClear();
  displayUpdate();
  return true;

#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
  if (!gDisplay) gDisplay = new P4EyeDisplay();
  if (!gDisplay) return false;
  if (!gDisplay->begin()) {
    // Keep the canvas stable for legacy UI pointers. A retry calls begin()
    // again; the driver releases failed hardware allocations itself.
    return false;
  }
  displayReady = true;
  resumeAfterSleep = false;
  return true;

#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // ============================================================================
  // SPI TFT (ST7789) Initialization
  // ============================================================================
  if (!gDisplay) gDisplay = new Adafruit_ST7789(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);
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
  
  displayReady = true;
  resumeAfterSleep = false;
  return true;
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  // ============================================================================
  // SPI TFT (ILI9341) Initialization - PLACEHOLDER
  // ============================================================================
  if (!gDisplay) gDisplay = new Adafruit_ILI9341(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);
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
  
  displayReady = true;
  resumeAfterSleep = false;
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
  if (!displayIsReady() || !gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306 || DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
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
  if (!displayIsReady() || !gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // Skip I2C push while sensor polling is paused (bus recovery, i2cpause, etc.).
  // CPU-side framebuffer is still updated so the first push after resume is current.
  // gSensorPollingPaused comes from System_PollPause.h (via System_I2C.h).
  if (pollPaused(oledBus)) return;
  
  // Use bus-aware device transaction. Routes mutex/clock to the OLED's
  // latched bus. Run at 400kHz to reduce bus hold
  // time (~20ms vs ~80ms at 100kHz); SSD1306 supports up to 400kHz I2C.
  // Short bus-lock wait (15ms) so OLED yields to higher-priority devices on the
  // SAME bus. If the bus is busy, this frame is skipped and retried next
  // cycle — no visible flicker.
  i2cDeviceTransactionVoid(oledBus, gDisplayI2cAddress, kOledClockHz, 15, [&]() {
    gDisplay->display();  // Push framebuffer to OLED (~20ms at 400kHz)
  });
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
  gDisplay->display();
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
  if (!displayIsReady() || !gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  i2cDeviceTransactionVoid(oledBus, gDisplayI2cAddress, kOledClockHz, 500, [&]() {
    gDisplay->dim(dim);
  });
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
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
  if (!displayIsReady() || !gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // SSD1306 contrast control via command interface
  // Contrast range is 0x00-0xFF, maps directly to level
  i2cDeviceTransactionVoid(oledBus, gDisplayI2cAddress, kOledClockHz, 500, [&]() {
    gDisplay->ssd1306_command(SSD1306_SETCONTRAST);
    gDisplay->ssd1306_command(level);
  });
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
  gDisplay->setBrightness(level);
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789 || DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  // TFT brightness via backlight PWM
  #if DISPLAY_BL_PIN >= 0
    analogWrite(DISPLAY_BL_PIN, level);
  #endif
#endif
}

// Power commands report whether the operation ran. The SSD1306 library's
// void writes cannot confirm that the physical panel accepted each command.
bool displaySetPower(bool on) {
  if (!displayIsReady() || !gDisplay) return false;
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  return i2cDeviceTransaction(oledBus, gDisplayI2cAddress, kOledClockHz, 500, [&]() -> bool {
    gDisplay->ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
    return true;
  });
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
  gDisplay->setPower(on);
  return true;
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  gDisplay->enableDisplay(on);
  return true;
#elif DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  gDisplay->sendCommand(on ? ILI9341_DISPON : ILI9341_DISPOFF);
  return true;
#endif
}

void displayStop() {
  resumeAfterSleep = false;
  const bool wasReady = displayReady.exchange(false);
  if (!gDisplay) return;
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  if (wasReady) {
    i2cDeviceTransactionVoid(oledBus, gDisplayI2cAddress, kOledClockHz, 500, [&]() {
      gDisplay->ssd1306_command(SSD1306_DISPLAYOFF);
    });
  }
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
  gDisplay->end();
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  if (wasReady) gDisplay->enableDisplay(false);
#elif DISPLAY_TYPE == DISPLAY_TYPE_ILI9341
  if (wasReady) gDisplay->sendCommand(ILI9341_DISPOFF);
#endif
  // UI drawing borrows this canvas outside the bus lock. Retain it across
  // stop/restart, as the P4 backend does, so those pointers never dangle.
  (void)wasReady;
}

void displayPrepareForSleep() {
  if (resumeAfterSleep) return;  // Preserve an already pending resume.
  resumeAfterSleep = displayIsReady();
  displaySetPower(false);
  displayReady = false;
  delay(10);  // Let the panel's off sequence complete before removing power.
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306 && defined(I2C2_POWER_PIN) && (I2C2_POWER_PIN >= 0)
  if (displayI2cBus() == 1) {
    digitalWrite(I2C2_POWER_PIN, LOW);
    delay(200);  // Preserve the existing rail decay / power-on-reset interval.
    displayRailOff = true;
    oledNeedsInit = gDisplay != nullptr;
  }
#endif
}

bool displayResumeFromSleep() {
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  restoreDisplayRail();
#endif
  if (!resumeAfterSleep || !gDisplay) return displayIsReady();

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  if (oledNeedsInit) {
    bool initialized = false;
    for (int attempt = 0; attempt < 2 && !initialized; ++attempt) {
      if (attempt) delay(100);
      initialized = i2cPingAddress(gDisplayI2cAddress, 100000, 200, oledBus) &&
                    beginOled(gDisplay, oledBus, gDisplayI2cAddress, 300);
    }
    if (!initialized) return false;
    oledNeedsInit = false;
  }
#endif
  displayReady = true;
  if (!displaySetPower(true)) {
    displayReady = false;
    return false;
  }
  resumeAfterSleep = false;
  return true;
}

#endif // DISPLAY_ENABLED
