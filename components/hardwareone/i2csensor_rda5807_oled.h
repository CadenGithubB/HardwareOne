// i2csensor_rda5807_oled.h - RDA5807 FM Radio OLED display functions
// Include this at the end of i2csensor_rda5807.cpp
#ifndef I2CSENSOR_RDA5807_OLED_H
#define I2CSENSOR_RDA5807_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// FM Radio OLED display function - shows radio data
static void displayFmRadio() {
  extern void oledDrawIcon(int x, int y, const char* iconName, int targetSize);
  extern void oledDrawLevelBars(int x, int y, int level, int maxBars, int barHeight);
  
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);
  
  if (!gFmRadioConnected || !gRadioInitialized) {
    oledDrawIcon(48, y + 2, "vol_mute", 16);
    oledDisplay->setCursor(16, y + 22);
    oledDisplay->println("FM Radio not active");
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 8);
    oledDisplay->print("X: Start");
    return;
  }
  
  // Take a snapshot under the cache mutex so we don't tear strings mid-draw.
  FMRadioCache snap;
  if (gFmRadioCache.mutex && xSemaphoreTake(gFmRadioCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = gFmRadioCache;
    xSemaphoreGive(gFmRadioCache.mutex);
  } else {
    snap = gFmRadioCache;  // best-effort
  }

  // Frequency (large) — replaced by a seek indicator while a seek is pending
  // (the async seek core: cmd_fmradio_seek starts it, fmRadioTask finalizes).
  oledDisplay->setCursor(0, y);
  oledDisplay->setTextSize(2);
  if (snap.seekInProgress) {
    oledDisplay->printf("Seek %s...", snap.seekDirUp ? "up" : "dn");
  } else {
    oledDisplay->printf("%.1f MHz", snap.frequency / 100.0);
  }
  oledDisplay->setTextSize(1);
  y += 18;

  // Station name (if available)
  oledDisplay->setCursor(0, y);
  if (strlen(snap.stationName) > 0) {
    oledDisplay->printf("Station: %s", snap.stationName);
  } else {
    oledDisplay->print("No RDS Station");
  }
  y += 10;

  // RDS Radio Text
  oledDisplay->setCursor(0, y);
  if (strlen(snap.stationText) > 0) {
    oledDisplay->print(snap.stationText);
  }
  y += 10;

  // Status bar: volume as a level bar (MUTED overrides), RSSI, stereo.
  // Volume 0-15 scales to 8 bars (4px each) so the bar ends before the
  // RSSI text at x=64.
  oledDisplay->setCursor(0, y);
  if (snap.muted) {
    oledDisplay->print("MUTED");
  } else {
    oledDisplay->print("Vol");
    oledDrawLevelBars(22, y, (snap.volume + 1) / 2, 8, 8);
  }
  oledDisplay->setCursor(64, y);
  oledDisplay->print(" RSSI:");
  oledDisplay->print(snap.rssi);
  oledDisplay->print(snap.stereo ? " ST" : " MO");

  // While seeking, keep re-rendering so the indicator clears the moment the
  // fmRadio task finalizes (same self-perpetuating dirty idiom as Speech).
  if (snap.seekInProgress) oledMarkDirty();
}

// Availability check for FM Radio OLED mode
static bool fmRadioOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void fmRadioToggleConfirmed(void* userData) {
  (void)userData;
  if (gRadioInitialized && gFmRadioConnected) {
    executeOLEDCommand("closefmradio");
  } else {
    executeOLEDCommand("openfmradio");
  }
}

// Input handler for FM Radio OLED mode. Full tuner controls (all dispatched
// through executeOLEDCommand so they carry the [CMD] audit line and the OLED
// auth identity — same stance as the LED screen):
//   L/R      tune -/+ 0.1 MHz        Up/Down  seek down/up (async core)
//   A        mute/unmute             Y        volume ladder 0>3>6>9>12>15>0
//   X        open/close (confirm)    B        back (falls through)
// Seek returns in milliseconds now (start-and-return core); the display's
// "Seek..." indicator tracks cache.seekInProgress until fmRadioTask finalizes.
static bool fmRadioInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (oledGuestBlocksMutate()) return true;
    if (gRadioInitialized && gFmRadioConnected) {
      oledConfirmRequest("Close FM?", nullptr, fmRadioToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open FM?", nullptr, fmRadioToggleConfirmed, nullptr);
    }
    return true;
  }

  // Everything below drives a running tuner.
  if (!gRadioInitialized || !gFmRadioConnected) return false;

  // L/R: step tune ±0.1 MHz (10 kHz-unit command form), clamped to the band.
  if (gNavEvents.left || gNavEvents.right) {
    if (oledGuestBlocksMutate()) return true;
    int f = (int)gFmRadioCache.frequency + (gNavEvents.right ? 10 : -10);
    if (f < 7600)  f = 7600;
    if (f > 10800) f = 10800;
    char cmd[28];
    snprintf(cmd, sizeof(cmd), "fmradiotune %d", f);
    executeOLEDCommand(cmd);
    return true;
  }

  // Up/Down: station seek. Ignored while one is already pending.
  if (gNavEvents.up || gNavEvents.down) {
    if (oledGuestBlocksMutate()) return true;
    if (!gFmRadioCache.seekInProgress) {
      executeOLEDCommand(gNavEvents.up ? "fmradioseek up" : "fmradioseek down");
      oledMarkDirty();  // show the Seek... indicator immediately
    }
    return true;
  }

  // A: mute toggle.
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (oledGuestBlocksMutate()) return true;
    executeOLEDCommand(gFmRadioCache.muted ? "fmradiounmute" : "fmradiomute");
    return true;
  }

  // Y: volume preset ladder (wraps 15 -> 0). The command banners "Vol: N/15".
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    if (oledGuestBlocksMutate()) return true;
    static const uint8_t kVolLadder[] = {0, 3, 6, 9, 12, 15};
    uint8_t next = kVolLadder[0];
    for (size_t i = 0; i < sizeof(kVolLadder); i++) {
      if (kVolLadder[i] > gFmRadioCache.volume) { next = kVolLadder[i]; break; }
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "fmradiovolume %u", (unsigned)next);
    executeOLEDCommand(cmd);
    return true;
  }

  return false;
}

// FM Radio OLED mode entry
static const OLEDModeEntry fmRadioOLEDModes[] = {
  {
    OLED_FM_RADIO,           // mode enum
    "FM Radio",              // menu name
    "radio",                 // icon name
    displayFmRadio,          // displayFunc
    fmRadioOLEDModeAvailable,// availFunc
    fmRadioInputHandler,     // inputFunc - full tuner controls (see handler)
    true,                    // showInMenu
    60,                      // menuOrder
    "A:Mute Y:Vol X:Pwr"     // hints (L/R tune + Up/Dn seek shown by use)
  }
};

// Auto-register FM Radio OLED mode
REGISTER_OLED_MODE_MODULE(fmRadioOLEDModes, sizeof(fmRadioOLEDModes) / sizeof(fmRadioOLEDModes[0]), "FMRadio");

#endif // I2CSENSOR_RDA5807_OLED_H
