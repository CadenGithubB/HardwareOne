// i2csensor-pa1010d-oled.h - PA1010D GPS OLED display functions
// Include this at the end of i2csensor-pa1010d.cpp
#ifndef I2CSENSOR_PA1010D_OLED_H
#define I2CSENSOR_PA1010D_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// GPS OLED display function - shows GPS data
static void displayGPSData() {
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);
  
  if (!gpsConnected || !gpsEnabled) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("GPS not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }
  
  // Read from GPS cache (thread-safe, no I2C contention)
  float lat = 0, lon = 0, alt = 0, speed = 0;
  uint8_t sats = 0, quality = 0;
  uint8_t hour = 0, minute = 0, second = 0;
  bool hasFix = false, valid = false;
  
  if (gGPSCache.mutex && xSemaphoreTake(gGPSCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    lat = gGPSCache.latitude;
    lon = gGPSCache.longitude;
    alt = gGPSCache.altitude;
    speed = gGPSCache.speed;
    hasFix = gGPSCache.hasFix;
    quality = gGPSCache.fixQuality;
    sats = gGPSCache.satellites;
    hour = gGPSCache.hour;
    minute = gGPSCache.minute;
    second = gGPSCache.second;
    valid = gGPSCache.dataValid;
    xSemaphoreGive(gGPSCache.mutex);
  }
  
  if (!valid) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Reading GPS...");
    return;
  }
  
  // Line 1: Fix status and satellites
  oledDisplay->setCursor(0, y);
  oledDisplay->print(hasFix ? "\x10" : "\xDB");
  oledDisplay->print(hasFix ? "FIX " : "--- ");
  oledDisplay->print("Sat:");
  oledDisplay->print((int)sats);
  oledDisplay->print(" Q:");
  oledDisplay->println((int)quality);
  y += 10;
  
  if (hasFix) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("Lat: ");
    oledDisplay->print(lat, 4);
    oledDisplay->println(lat >= 0 ? "N" : "S");
    y += 10;
    
    oledDisplay->setCursor(0, y);
    oledDisplay->print("Lon: ");
    oledDisplay->print(lon, 4);
    oledDisplay->println(lon >= 0 ? "E" : "W");
    y += 10;
    
    oledDisplay->setCursor(0, y);
    oledDisplay->print("Alt:");
    oledDisplay->print(alt, 0);
    oledDisplay->print("m Spd:");
    oledDisplay->print(speed, 1);
    oledDisplay->println("kn");
    y += 10;
    
    oledDisplay->setCursor(0, y);
    if (hour < 10) oledDisplay->print('0');
    oledDisplay->print(hour);
    oledDisplay->print(':');
    if (minute < 10) oledDisplay->print('0');
    oledDisplay->print(minute);
    oledDisplay->print(':');
    if (second < 10) oledDisplay->print('0');
    oledDisplay->print(second);
    oledDisplay->println(" UTC");
  } else {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Waiting for fix");
    oledDisplay->setCursor(0, y + 10);
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
  // enqueueDeviceStart, isInQueue provided by System_I2C.h (included in parent .cpp)

  if (gpsEnabled && gpsConnected) {
    Serial.println("[GPS] Confirmed: Stopping GPS...");
    gpsEnabled = false;
  } else if (!isInQueue(I2C_DEVICE_GPS)) {
    Serial.println("[GPS] Confirmed: Starting GPS...");
    enqueueDeviceStart(I2C_DEVICE_GPS);
  }
}

// Input handler for GPS OLED mode - X button toggles sensor
static bool gpsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gpsEnabled && gpsConnected) {
      oledConfirmRequest("Close GPS?", nullptr, gpsToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open GPS?", nullptr, gpsToggleConfirmed, nullptr);
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
