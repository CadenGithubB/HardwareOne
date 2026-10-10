// i2csensor_pa1010d_oled.h - PA1010D GPS OLED display functions
// Include this at the end of i2csensor_pa1010d.cpp
#ifndef I2CSENSOR_PA1010D_OLED_H
#define I2CSENSOR_PA1010D_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "i2csensor_pa1010d.h"
#include <Adafruit_SSD1306.h>

// Persistent scroll state for the GPS view — survives between render frames
static OLEDContentArea sGPSContent;
static bool sGPSContentInited = false;

// GPS OLED display function - shows GPS data using the shared content area system
static void displayGPSData() {
  if (!sGPSContentInited) {
    oledContentInit(&sGPSContent, oledDisplay);
    sGPSContentInited = true;
  }

  oledDisplay->setTextSize(1);
  oledContentBegin(&sGPSContent);

  if (!gGpsConnected || !gGpsRunning) {
    oledContentPrint(&sGPSContent, "GPS not active");
    oledContentPrint(&sGPSContent, "");
    oledContentPrint(&sGPSContent, "Press X to start");
    oledContentEnd(&sGPSContent);
    return;
  }

  // Read one GPS snapshot (thread-safe, no I2C contention).
  GPSCache gps{};
  if (!gpsCacheSnapshot(gps) || !gps.dataValid) {
    oledContentPrint(&sGPSContent, "Reading GPS...");
    oledContentEnd(&sGPSContent);
    return;
  }

  char buf[32];

  // Line 0: Fix status + satellites + quality
  snprintf(buf, sizeof(buf), "%s Sat:%d Q:%d",
           gps.hasFix ? "\x10FIX" : "\xDB---", (int)gps.satellites, (int)gps.fixQuality);
  oledContentPrint(&sGPSContent, buf);

  if (gps.hasFix) {
    // Line 1: Latitude
    snprintf(buf, sizeof(buf), "Lat: %.4f%c", gps.latitude, gps.latitude >= 0 ? 'N' : 'S');
    oledContentPrint(&sGPSContent, buf);

    // Line 2: Longitude
    snprintf(buf, sizeof(buf), "Lon: %.4f%c", gps.longitude, gps.longitude >= 0 ? 'E' : 'W');
    oledContentPrint(&sGPSContent, buf);

    // Line 3: Altitude + Speed
    snprintf(buf, sizeof(buf), "Alt:%.0fm Spd:%.1fkn", gps.altitude, gps.speed);
    oledContentPrint(&sGPSContent, buf);

    // Line 4: UTC time
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d UTC", (int)gps.hour, (int)gps.minute, (int)gps.second);
    oledContentPrint(&sGPSContent, buf);
  } else {
    oledContentPrint(&sGPSContent, "Waiting for fix");
    oledContentPrint(&sGPSContent, "Need open sky");
  }

  oledContentEnd(&sGPSContent);
}

// Availability check for GPS OLED mode
static bool gpsOLEDModeAvailable(String* outReason) {
  if (gGpsConnected && gGpsRunning) return true;

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
  if (gGpsRunning && gGpsConnected) {
    executeOLEDCommand("closegps");
  } else {
    executeOLEDCommand("opengps");
  }
}

// Input handler: up/down scrolls content, X toggles sensor
static bool gpsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Scroll: only consume the event if there is actually something to scroll to
  if (gNavEvents.up && sGPSContentInited && !sGPSContent.scrollAtTop) {
    oledContentScrollUp(&sGPSContent);
    return true;
  }
  if (gNavEvents.down && sGPSContentInited && !sGPSContent.scrollAtBottom) {
    oledContentScrollDown(&sGPSContent);
    return true;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gGpsRunning && gGpsConnected) {
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
    gpsInputHandler,         // inputFunc
    true,                    // showInMenu
    50,                      // menuOrder
    "\x18\x19:Scroll  X:Toggle"  // hints (↑↓ when there's overflow, X always)
  }
};

// Auto-register GPS OLED mode
REGISTER_OLED_MODE_MODULE(gpsOLEDModes, sizeof(gpsOLEDModes) / sizeof(gpsOLEDModes[0]), "GPS");

#endif // I2CSENSOR_PA1010D_OLED_H
