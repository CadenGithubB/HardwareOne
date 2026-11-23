// i2csensor_vl53l4cx_oled.h - VL53L4CX ToF OLED display functions
// Include this at the end of i2csensor_vl53l4cx.cpp
#ifndef I2CSENSOR_VL53L4CX_OLED_H
#define I2CSENSOR_VL53L4CX_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "i2csensor_vl53l4cx.h"
#include <Adafruit_SSD1306.h>

// ToF OLED display function - shows distance data
static void displayTofData() {
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);

  if (!gTofConnected || !gTofRunning) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("ToF not active");
    oledDisplay->println();
    oledDisplay->println(oledPortrait() ? "X: start" : "Press X to start");
    return;
  }

  TofCache reading{};
  if (oledPortrait()) {
    // Portrait: the hero card on the left (DIST, the millimetres, mm) and the
    // vertical gauge on the right, 0..2000 mm filled from the bottom.
    namespace P = oled_layout::portrait;
    const bool have = tofCacheSnapshot(reading);
    const bool valid = have && reading.tofDataValid && reading.tofTotalObjects > 0;
    const int distMm = valid ? reading.tofObjects[0].distance_mm : 0;
    char value[8];
    snprintf(value, sizeof(value), valid ? "%d" : "--", distMm);
    oledHeroCard(oledDisplay, "DIST", value, "mm", true);
    oledDisplay->drawRect(P::kGaugeX, P::kGaugeY, P::kGaugeW, P::kGaugeH, DISPLAY_COLOR_WHITE);
    const int fill = oled_layout::gaugeFill(distMm, 2000, P::kGaugeFillSpan);
    if (fill > 0) {
      oledDisplay->fillRect(P::kGaugeFillX, P::kGaugeFillBottom + 1 - fill, P::kGaugeFillW, fill,
                            DISPLAY_COLOR_WHITE);
    }
    oledDisplay->setCursor(P::kGaugeLabelX, P::kGaugeFillTop);
    oledDisplay->print("2m");
    oledDisplay->setCursor(P::kGaugeLabelX, 57);
    oledDisplay->print("1m");
    oledDisplay->setCursor(P::kGaugeLabelX + 6, P::kGaugeFillBottom - 6);
    oledDisplay->print("0");
    if (!have || !valid) {
      oledDisplay->setCursor(P::kTextX, 80);
      oledDisplay->print(have ? "Waiting" : "Busy");
    }
    return;
  }
  if (tofCacheSnapshot(reading)) {
    if (reading.tofDataValid && reading.tofTotalObjects > 0) {
      // Show first detected object distance
      int distMm = reading.tofObjects[0].distance_mm;
      oledDisplay->setCursor(0, y);
      oledDisplay->setTextSize(2);
      oledDisplay->print(distMm);
      oledDisplay->println(" mm");
      oledDisplay->setTextSize(1);
      y += 20;
      
      // Visual bar representation (0-2000mm range)
      int barWidth = map(constrain(distMm, 0, 2000), 0, 2000, 0, SCREEN_WIDTH - 8);
      oledDisplay->drawRect(0, y, SCREEN_WIDTH - 4, 10, DISPLAY_COLOR_WHITE);
      oledDisplay->fillRect(2, y + 2, barWidth, 6, DISPLAY_COLOR_WHITE);
      
      oledDisplay->setCursor(0, y + 12);
      oledDisplay->print("0");
      oledDisplay->setCursor(SCREEN_WIDTH - 28, y + 12);
      oledDisplay->print("2000mm");
    } else {
      oledDisplay->setCursor(0, y);
      oledDisplay->println("Waiting for data...");
    }
  } else {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("ToF: Busy");
  }
}

// Availability check for ToF OLED mode
static bool tofOledModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void tofToggleConfirmed(void* userData) {
  (void)userData;
  if (gTofRunning && gTofConnected) {
    executeOledCommand("closetof");
  } else {
    executeOledCommand("opentof");
  }
}

// Input handler for ToF OLED mode - X button toggles sensor
static bool tofInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gTofRunning && gTofConnected) {
      oledConfirmRequest("Close ToF?", nullptr, tofToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open ToF?", nullptr, tofToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// ToF OLED mode entry
static const OledModeEntry tofOledModes[] = {
  {
    OLED_TOF_DATA,           // mode enum
    "ToF",                   // menu name
    "tof_radar",             // icon name
    displayTofData,          // displayFunc
    tofOledModeAvailable,    // availFunc
    tofInputHandler,         // inputFunc - X toggles sensor
    true,                    // showInMenu
    30,                      // menuOrder
    nullptr                  // hints
  }
};

// Auto-register ToF OLED mode
REGISTER_OLED_MODE_MODULE(tofOledModes, sizeof(tofOledModes) / sizeof(tofOledModes[0]), "ToF");

#endif // I2CSENSOR_VL53L4CX_OLED_H
