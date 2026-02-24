// OLED_Mode_Remote.cpp - Remote device UI for paired mode
// Only visible when paired mode is enabled and connected

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"
#include "System_ESPNow.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

extern DisplayDriver* oledDisplay;
extern bool oledConnected;
extern Settings gSettings;
extern EspNowState* gEspNow;

extern String getEspNowDeviceName(const uint8_t* mac);
extern bool parseMacAddress(const String& macStr, uint8_t mac[6]);

void displayRemoteMode() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Check if paired mode is enabled
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Not bonded.");
    oledDisplay->println();
    oledDisplay->println("Use CLI:");
    oledDisplay->println("  bond connect <device>");
    return;
  }
  
  // Get peer device name
  uint8_t peerMac[6];
  String peerName = gSettings.bondPeerMac;
  
  if (parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    String name = getEspNowDeviceName(peerMac);
    if (name.length() > 0) {
      peerName = name;
    }
  }
  
  // Display paired device info
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  oledDisplay->print("Peer: ");
  oledDisplay->println(peerName);
  
  oledDisplay->print("Role: ");
  oledDisplay->println(gSettings.bondRole == 1 ? "master" : "worker");
  
  // Show remote capabilities if available
  if (gEspNow && gEspNow->lastRemoteCapValid) {
    CapabilitySummary& cap = gEspNow->lastRemoteCap;
    oledDisplay->println();
    oledDisplay->print("Remote: ");
    oledDisplay->println(cap.deviceName);
    
    // Show features in human-readable format
    String features = getCapabilityListShort(cap.featureMask, FEATURE_NAMES);
    oledDisplay->print("F:");
    oledDisplay->println(features.substring(0, 20));  // Truncate for OLED width
    
    // Show sensors in human-readable format
    String sensors = getCapabilityListShort(cap.sensorMask, SENSOR_NAMES);
    oledDisplay->print("S:");
    oledDisplay->println(sensors.substring(0, 20));
    
    // Hardware info
    char buf[32];
    snprintf(buf, sizeof(buf), "%luMB/%luMB Ch%d",
             (unsigned long)cap.flashSizeMB, (unsigned long)cap.psramSizeMB, cap.wifiChannel);
    oledDisplay->println(buf);
    
    // Age of capability data
    unsigned long age = (millis() - gEspNow->lastRemoteCapTime) / 1000;
    if (age < 60) {
      snprintf(buf, sizeof(buf), "Updated %lus ago", age);
    } else {
      snprintf(buf, sizeof(buf), "Updated %lum ago", age / 60);
    }
    oledDisplay->println(buf);
  } else {
    oledDisplay->println();
    oledDisplay->println("No remote caps yet.");
    oledDisplay->println("bond requestcap");
  }
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW
