// i2csensor_mlx90640_oled.h - MLX90640 Thermal OLED display functions
// Include this at the end of i2csensor_mlx90640.cpp
#ifndef I2CSENSOR_MLX90640_OLED_H
#define I2CSENSOR_MLX90640_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// Thermal OLED display function - shows thermal visualization
static void displayThermalVisual() {
  if (!gThermalConnected || !gThermalRunning) {
    oledDisplay->setTextSize(1);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Thermal not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }

  auto frame = thermalSnapshotBuffer(THERMAL_RAW_FRAME_PIXELS);
  ThermalCache reading;
  if (!frame || !thermalFrameSnapshot(reading, frame.get(), THERMAL_RAW_FRAME_PIXELS, false, 10)) {
    oledDisplay->println("Thermal: Busy");
    return;
  }

  if (!reading.thermalDataValid || !reading.thermalFrameAvailable) {
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Waiting for");
    oledDisplay->println("thermal data...");
    return;
  }

  // Get thermal data from cache
  float minTemp = reading.thermalMinTemp;
  float maxTemp = reading.thermalMaxTemp;
  float avgTemp = reading.thermalAvgTemp;

  // Adjust dimensions based on rotation setting
  const int thermalWidth = reading.thermalFrameWidth;
  const int thermalHeight = reading.thermalFrameHeight;
  const float scale = (gSettings.oledThermalScale > 0.0) ? gSettings.oledThermalScale : 2.5;
  const int imageWidth = (int)(thermalWidth * scale);
  const int textStartX = imageWidth + 2;

  // Temperature range for mapping
  float tempRange = maxTemp - minTemp;
  if (tempRange < 1.0) tempRange = 1.0;

  // Draw thermal pixels
  for (int ty = 0; ty < thermalHeight; ty++) {
    for (int tx = 0; tx < thermalWidth; tx++) {
      int pixelIndex = ty * thermalWidth + tx;
      float temp = frame.get()[pixelIndex] / 100.0f;

      // Map temperature to brightness (0-1)
      float normalized = (temp - minTemp) / tempRange;
      if (normalized < 0.0) normalized = 0.0;
      if (normalized > 1.0) normalized = 1.0;

      // Create simple 3-level visualization
      bool drawPixel = false;
      if (normalized > 0.66) {
        drawPixel = true;
      } else if (normalized > 0.33) {
        drawPixel = ((tx + ty) % 2 == 0);
      }

      // Draw scaled pixel block
      if (drawPixel) {
        int startX = (int)(tx * scale);
        int startY = (int)(ty * scale);
        int endX = (int)((tx + 1) * scale);
        int endY = (int)((ty + 1) * scale);

        for (int y = startY; y < endY && y < 64; y++) {
          for (int x = startX; x < endX && x < imageWidth; x++) {
            oledDisplay->drawPixel(x, y, DISPLAY_COLOR_WHITE);
          }
        }
      }
    }
  }

  // Draw temperature data on the right side
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(textStartX, 0);
  oledDisplay->print("THERMAL");
  oledDisplay->setCursor(textStartX, 16);
  oledDisplay->print("Min:");
  oledDisplay->print((int)minTemp);
  oledDisplay->setCursor(textStartX, 32);
  oledDisplay->print("Avg:");
  oledDisplay->print((int)avgTemp);
  oledDisplay->setCursor(textStartX, 48);
  oledDisplay->print("Max:");
  oledDisplay->print((int)maxTemp);
}

// Availability check for Thermal OLED mode
static bool thermalOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void thermalToggleConfirmed(void* userData) {
  (void)userData;
  if (gThermalRunning && gThermalConnected) {
    executeOLEDCommand("closethermal");
  } else {
    executeOLEDCommand("openthermal");
  }
}

// Input handler for Thermal OLED mode - X button toggles sensor
static bool thermalInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gThermalRunning && gThermalConnected) {
      oledConfirmRequest("Close Thermal?", nullptr, thermalToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open Thermal?", nullptr, thermalToggleConfirmed, nullptr);
    }
    return true;  // Input handled
  }
  return false;  // Let default handler process
}

// Thermal OLED mode entry
static const OLEDModeEntry thermalOLEDModes[] = {
  {
    OLED_THERMAL_VISUAL,     // mode enum
    "Thermal",               // menu name
    "thermal",               // icon name
    displayThermalVisual,    // displayFunc
    thermalOLEDModeAvailable,// availFunc
    thermalInputHandler,     // inputFunc - X toggles sensor
    true,                    // showInMenu
    20,                      // menuOrder
    nullptr                  // hints
  }
};

// Auto-register Thermal OLED mode
REGISTER_OLED_MODE_MODULE(thermalOLEDModes, sizeof(thermalOLEDModes) / sizeof(thermalOLEDModes[0]), "Thermal");

#endif // I2CSENSOR_MLX90640_OLED_H
