// i2csensor-ds3231-oled.h - DS3231 RTC OLED display functions
// Include this at the end of i2csensor-ds3231.cpp
#ifndef I2CSENSOR_DS3231_OLED_H
#define I2CSENSOR_DS3231_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// RTC OLED display function - shows date, time, and temperature
// Respects OLED_CONTENT_HEIGHT to not overlap footer/navigation bar
static void displayRTCData() {
  extern Adafruit_SSD1306* oledDisplay;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("=== RTC ===");
  
  if (!rtcConnected || !rtcEnabled) {
    oledDisplay->println();
    oledDisplay->println("RTC not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }
  
  // Get cached RTC data (thread-safe)
  RTCDateTime dt;
  float temp = 0.0f;
  bool valid = false;
  
  if (gRTCCache.mutex && xSemaphoreTake(gRTCCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    dt = gRTCCache.dateTime;
    temp = gRTCCache.temperature;
    valid = gRTCCache.dataValid;
    xSemaphoreGive(gRTCCache.mutex);
  }
  
  if (!valid) {
    oledDisplay->println("Reading RTC...");
    return;
  }
  
  // Display date
  oledDisplay->print(dt.year);
  oledDisplay->print("-");
  if (dt.month < 10) oledDisplay->print("0");
  oledDisplay->print(dt.month);
  oledDisplay->print("-");
  if (dt.day < 10) oledDisplay->print("0");
  oledDisplay->print(dt.day);
  
  // Day of week on same line
  const char* days[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  if (dt.dayOfWeek >= 1 && dt.dayOfWeek <= 7) {
    oledDisplay->print("  ");
    oledDisplay->println(days[dt.dayOfWeek]);
  } else {
    oledDisplay->println();
  }
  
  // Display time (larger) - this takes 16 pixels height
  oledDisplay->setTextSize(2);
  if (dt.hour < 10) oledDisplay->print("0");
  oledDisplay->print(dt.hour);
  oledDisplay->print(":");
  if (dt.minute < 10) oledDisplay->print("0");
  oledDisplay->print(dt.minute);
  oledDisplay->print(":");
  if (dt.second < 10) oledDisplay->print("0");
  oledDisplay->println(dt.second);
  
  // Display temperature - check we're still within content area
  oledDisplay->setTextSize(1);
  int cursorY = oledDisplay->getCursorY();
  if (cursorY + 8 <= OLED_CONTENT_HEIGHT) {
    oledDisplay->print("Temp: ");
    oledDisplay->print(temp, 1);
    oledDisplay->println("C");
  }
}

// Availability check for RTC OLED mode
static bool rtcOLEDModeAvailable(String* outReason) {
  // Check if RTC is running
  if (rtcConnected && rtcEnabled) {
    return true;
  }
  // Check if hardware was detected during I2C scan (address 0x68)
  extern ConnectedDevice connectedDevices[];
  extern int connectedDeviceCount;
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (connectedDevices[i].address == I2C_ADDR_DS3231 && connectedDevices[i].isConnected) {
      if (outReason) *outReason = "Disabled\nPress X to start";
      return true;  // Allow navigation so user can press X to start
    }
  }
  if (outReason) *outReason = "Not detected";
  return false;
}

static void rtcToggleConfirmed(void* userData) {
  (void)userData;
  extern void executeOLEDCommand(const String& cmd);

  if (rtcEnabled && rtcConnected) {
    Serial.println("[RTC] Confirmed: Stopping RTC...");
    executeOLEDCommand("rtcstop");
  } else {
    Serial.println("[RTC] Confirmed: Starting RTC...");
    executeOLEDCommand("rtcstart");
  }
}

// Input handler for RTC OLED mode - X button toggles sensor
static bool rtcInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  (void)deltaX;
  (void)deltaY;
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (rtcEnabled && rtcConnected) {
      oledConfirmRequest("Stop RTC?", nullptr, rtcToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Start RTC?", nullptr, rtcToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// RTC OLED mode entry
static const OLEDModeEntry rtcOLEDModes[] = {
  {
    OLED_RTC_DATA,           // mode enum
    "RTC",                   // menu name
    "clock",                 // icon name
    displayRTCData,          // displayFunc
    rtcOLEDModeAvailable,    // availFunc
    rtcInputHandler,         // inputFunc - X toggles sensor
    true,                    // showInMenu
    55                       // menuOrder
  }
};

// Auto-register RTC OLED mode
REGISTER_OLED_MODE_MODULE(rtcOLEDModes, sizeof(rtcOLEDModes) / sizeof(rtcOLEDModes[0]), "RTC");

#endif // I2CSENSOR_DS3231_OLED_H
