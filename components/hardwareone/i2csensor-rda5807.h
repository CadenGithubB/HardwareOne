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

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// FM Radio state
extern bool fmRadioEnabled;
extern bool fmRadioConnected;
extern bool radioInitialized;      // Radio hardware initialization status
extern uint16_t fmRadioFrequency;  // Current frequency in 10kHz units (e.g., 10390 = 103.9 MHz)
extern uint8_t fmRadioVolume;      // 0-15
extern bool fmRadioMuted;
extern bool fmRadioStereo;

// RDS data (Radio Data System)
extern char fmRadioStationName[9];   // 8 chars + null
extern char fmRadioStationText[65];  // 64 chars + null (Radio Text)

// Signal quality
extern uint8_t fmRadioRSSI;          // Received Signal Strength Indicator
extern uint8_t fmRadioSNR;           // Signal-to-Noise Ratio

// Command handlers
const char* cmd_fmradio(const String& cmd);
const char* cmd_fmradio_start(const String& cmd);
const char* cmd_fmradio_stop(const String& cmd);
const char* cmd_fmradio_tune(const String& cmd);
const char* cmd_fmradio_seek(const String& cmd);
const char* cmd_fmradio_volume(const String& cmd);
const char* cmd_fmradio_mute(const String& cmd);
const char* cmd_fmradio_status(const String& cmd);

// FM Radio functions
bool initFMRadio();
void deinitFMRadio();
void pollFMRadio();  // Called periodically to update RDS data

// JSON data builder (for web API)
int buildFMRadioDataJSON(char* buf, size_t bufSize);

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry fmRadioCommands[];
extern const size_t fmRadioCommandsCount;

#endif // FM_RADIO_H
