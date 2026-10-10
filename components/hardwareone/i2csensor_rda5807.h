/**
 * FM Radio Sensor - ScoutMakes FM Radio Board (RDA5807M)
 * fm_radio.h
 * 
 * STEMMA QT / Qwiic I2C FM Radio breakout board
 * I2C Address: 0x11
 * Library: PU2CLR RDA5807 (install via Arduino Library Manager)
 */

#ifndef I2CSENSOR_RDA5807_H
#define I2CSENSOR_RDA5807_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// FM Radio cache structure (always available for type-safe references).
// Groups all sensor data fields in one copied reading so readers (web API, OLED,
// ESP-NOW broadcaster) get a consistent snapshot and writers (fmRadio task,
// RDS callbacks) don't tear strings mid-update.
struct FMRadioCache {
  bool dataValid = false;
  unsigned long lastUpdate = 0;  // millis() of the last successful poll — feeds the sensor envelope's ts
  uint16_t frequency = 10390;    // Current frequency in 10kHz units (10390 = 103.9 MHz)
  uint8_t volume = 6;            // 0-15
  bool muted = false;
  bool stereo = true;
  uint8_t rssi = 0;              // Received Signal Strength Indicator
  uint8_t snr = 0;               // Signal-to-Noise Ratio
  bool headphonesConnected = false;
  char stationName[9] = {0};     // 8 chars + null (RDS station name)
  char stationText[65] = {0};    // 64 chars + null (RDS radio text)
  // Async seek state — cmd_fmradio_seek starts the hardware seek in a short
  // I2C transaction and returns immediately (it used to busy-wait ~5 s while
  // HOLDING the bus mutex, freezing the gamepad + OLED on the shared bus).
  // fmRadioTask's 250 ms updateFMRadio() poll finalizes via ri.tuned (6 s
  // failsafe). OLED/web render a "Seeking" indicator from seekInProgress.
  bool seekInProgress = false;
  bool seekDirUp = true;           // direction of the pending seek
  unsigned long seekStartMs = 0;   // millis() at seek start (arm delay + failsafe)
};

#if ENABLE_FM_RADIO

// FM Radio lifecycle state (not cached — read/written only on fmRadio task)
extern bool gFmRadioRunning;
extern bool gFmRadioConnected;
extern unsigned long gFmRadioLastStopTime;
extern bool gRadioInitialized;      // Radio hardware initialization status
extern TaskHandle_t gFmRadioTaskHandle;

// Fixed-size copy under a private critical section. Enabled builds always
// acquire it; timeoutMs is retained for snapshot-interface compatibility.
// No I2C, allocation or formatting occurs in the critical section.
bool fmRadioCacheSnapshot(FMRadioCache& out, uint32_t timeoutMs = 10);

// Command handlers
const char* cmd_fmradio(const String& argsInput);
const char* cmd_fmradio_start(const String& argsInput);
const char* cmd_fmradio_stop(const String& argsInput);
const char* cmd_fmradio_tune(const String& argsInput);
const char* cmd_fmradio_seek(const String& argsInput);
const char* cmd_fmradio_volume(const String& argsInput);
const char* cmd_fmradio_mute(const String& argsInput);
const char* cmd_fmradio_unmute(const String& argsInput);
const char* cmd_fmradio_status(const String& argsInput);

// FM Radio functions
bool fmRadioInit();
void fmRadioDeinit();
void fmRadioPoll();  // Called periodically to update RDS data

// JSON data builder (for web API)
int fmRadioBuildDataJSON(char* buf, size_t bufSize);

// Command registry (for System_CommandRegistry.cpp module list)
struct CommandEntry;
extern const CommandEntry fmRadioCommands[];
extern const size_t fmRadioCommandsCount;

#else
inline bool fmRadioCacheSnapshot(FMRadioCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}
inline int fmRadioBuildDataJSON(char*, size_t) { return 0; }
#endif // ENABLE_FM_RADIO
#endif // I2CSENSOR_RDA5807_H
