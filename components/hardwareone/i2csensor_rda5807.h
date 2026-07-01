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
#include <freertos/semphr.h>

// FM Radio cache structure (always available for type-safe references).
// Groups all sensor data fields under one mutex so readers (web API, OLED,
// ESP-NOW broadcaster) get a consistent snapshot and writers (fmRadio task,
// RDS callbacks) don't tear strings mid-update.
struct FMRadioCache {
  SemaphoreHandle_t mutex = nullptr;
  bool dataValid = false;
  uint16_t frequency = 10390;    // Current frequency in 10kHz units (10390 = 103.9 MHz)
  uint8_t volume = 6;            // 0-15
  bool muted = false;
  bool stereo = true;
  uint8_t rssi = 0;              // Received Signal Strength Indicator
  uint8_t snr = 0;               // Signal-to-Noise Ratio
  bool headphonesConnected = false;
  char stationName[9] = {0};     // 8 chars + null (RDS station name)
  char stationText[65] = {0};    // 64 chars + null (RDS radio text)
};

#if ENABLE_FM_RADIO

// FM Radio lifecycle state (not cached — read/written only on fmRadio task)
extern bool gFmRadioEnabled;
extern bool gFmRadioConnected;
extern unsigned long gFmRadioLastStopTime;
extern bool gRadioInitialized;      // Radio hardware initialization status
extern TaskHandle_t gFmRadioTaskHandle;

// FM Radio data cache (mutex-protected)
extern FMRadioCache gFmRadioCache;

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

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry fmRadioCommands[];
extern const size_t fmRadioCommandsCount;

#endif // ENABLE_FM_RADIO
#endif // I2CSENSOR_RDA5807_H
