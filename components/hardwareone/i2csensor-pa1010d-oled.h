// i2csensor-pa1010d-oled.h - PA1010D GPS OLED display functions
// Include this at the end of i2csensor-pa1010d.cpp
#ifndef I2CSENSOR_PA1010D_OLED_H
#define I2CSENSOR_PA1010D_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// GPS OLED display function - shows GPS data
static void displayGPSData() {
  extern Adafruit_SSD1306* oledDisplay;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("=== GPS ===");
  oledDisplay->println();
  
  if (!gpsConnected || gPA1010D == nullptr || !gpsEnabled) {
    oledDisplay->println("GPS not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }
  
  // Use cached GPS data (gpsTask continuously polls and updates gPA1010D)
  // Do NOT call gPA1010D->read() here - causes I2C bus contention with OLED
  
  // Line 1: Fix status and satellites
  oledDisplay->print(gPA1010D->fix ? "\x10" : "\xDB");
  oledDisplay->print(gPA1010D->fix ? "FIX " : "--- ");
  oledDisplay->print("Sat:");
  oledDisplay->print((int)gPA1010D->satellites);
  oledDisplay->print(" Q:");
  oledDisplay->println((int)gPA1010D->fixquality);
  
  if (gPA1010D->fix) {
    oledDisplay->print("Lat: ");
    oledDisplay->print(gPA1010D->latitudeDegrees, 4);
    oledDisplay->println(gPA1010D->lat);
    
    oledDisplay->print("Long: ");
    oledDisplay->print(gPA1010D->longitudeDegrees, 4);
    oledDisplay->println(gPA1010D->lon);
    
    oledDisplay->print("Alt:");
    oledDisplay->print(gPA1010D->altitude, 0);
    oledDisplay->print("m Spd:");
    oledDisplay->print(gPA1010D->speed, 1);
    oledDisplay->println("kn");
    
    if (gPA1010D->hour < 10) oledDisplay->print('0');
    oledDisplay->print(gPA1010D->hour);
    oledDisplay->print(':');
    if (gPA1010D->minute < 10) oledDisplay->print('0');
    oledDisplay->print(gPA1010D->minute);
    oledDisplay->print(':');
    if (gPA1010D->seconds < 10) oledDisplay->print('0');
    oledDisplay->print(gPA1010D->seconds);
    oledDisplay->println(" UTC");
  } else {
    oledDisplay->println("Waiting for fix");
    oledDisplay->println("Need open sky");
  }
}

// Availability check for GPS OLED mode
static bool gpsOLEDModeAvailable(String* outReason) {
  // Check if GPS is running
  if (gpsConnected && gpsEnabled) {
    return true;
  }
  // Check if hardware was detected during I2C scan (address 0x10)
  extern ConnectedDevice connectedDevices[];
  extern int connectedDeviceCount;
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (connectedDevices[i].address == I2C_ADDR_GPS && connectedDevices[i].isConnected) {
      if (outReason) *outReason = "Disabled\nPress X to start";
      return true;  // Allow navigation so user can press X to start
    }
  }
  if (outReason) *outReason = "Not detected";
  return false;
}

static void gpsToggleConfirmed(void* userData) {
  (void)userData;
  extern bool enqueueSensorStart(SensorType sensor);
  extern bool isInQueue(SensorType sensor);

  if (gpsEnabled && gpsConnected) {
    Serial.println("[GPS] Confirmed: Stopping GPS...");
    gpsEnabled = false;
  } else if (!isInQueue(SENSOR_GPS)) {
    Serial.println("[GPS] Confirmed: Starting GPS...");
    enqueueSensorStart(SENSOR_GPS);
  }
}

// Input handler for GPS OLED mode - X button toggles sensor
static bool gpsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gpsEnabled && gpsConnected) {
      oledConfirmRequest("Stop GPS?", nullptr, gpsToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Start GPS?", nullptr, gpsToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// GPS OLED mode entry
static const OLEDModeEntry gpsOLEDModes[] = {
  {
    OLED_GPS_DATA,           // mode enum
    "GPS",                   // menu name
    "compass",               // icon name
    displayGPSData,          // displayFunc
    gpsOLEDModeAvailable,    // availFunc
    gpsInputHandler,         // inputFunc - X toggles sensor
    true,                    // showInMenu
    50                       // menuOrder
  }
};

// Auto-register GPS OLED mode
REGISTER_OLED_MODE_MODULE(gpsOLEDModes, sizeof(gpsOLEDModes) / sizeof(gpsOLEDModes[0]), "GPS");

#endif // I2CSENSOR_PA1010D_OLED_H
