/**
 * Microphone Sensor Module - ESP32-S3 PDM Microphone Support
 * 
 * Supports PDM microphone on XIAO ESP32S3 Sense via I2S interface.
 * Records audio samples for visualization, recording, and ML inference.
 */

#ifndef SYSTEM_MICROPHONE_H
#define SYSTEM_MICROPHONE_H

#include <Arduino.h>
#include "System_BuildConfig.h"

#if ENABLE_MICROPHONE_SENSOR

// Microphone sensor state
extern bool micEnabled;
extern bool micConnected;
extern bool micRecording;

// Microphone info
extern int micSampleRate;
extern int micBitDepth;
extern int micChannels;
extern int micGain;

// Microphone initialization
bool initMicrophone();
void stopMicrophone();

// Capture audio samples (returns raw PCM data)
// Caller must free the buffer with free() when done
int16_t* captureAudioSamples(size_t sampleCount, size_t* outLen);

// Get audio level (0-100 for VU meter display)
int getAudioLevel();

// Audio preprocessing (shared with ESP-SR)
// Applies: DC offset removal, high-pass filter, pre-emphasis, software gain
// Pass gainMultiplier <= 0 to use default from micGain setting
// Pass filtersEnabled = false to skip high-pass and pre-emphasis (for ESP-SR AFE testing)
void applyMicAudioProcessing(int16_t* buf, size_t sampleCount, float gainMultiplier = 0.0f, bool filtersEnabled = true);
void resetMicAudioProcessingState();
float getMicSoftwareGainMultiplier();
int32_t getMicDcOffset();

// Get microphone status JSON
const char* buildMicrophoneStatusJson();

// Recording functions
bool startRecording();
void stopRecording();
int getRecordingCount();
String getRecordingsList();
bool deleteRecording(const char* filename);

// Command handlers
const char* cmd_mic(const String& cmd);
const char* cmd_micstart(const String& cmd);
const char* cmd_micstop(const String& cmd);
const char* cmd_miclevel(const String& cmd);
const char* cmd_micrecord(const String& cmd);
const char* cmd_miclist(const String& cmd);
const char* cmd_micdelete(const String& cmd);
const char* cmd_micsamplerate(const String& cmd);
const char* cmd_micgain(const String& cmd);
const char* cmd_micbitdepth(const String& cmd);

// Command registry
struct CommandEntry;
extern const CommandEntry micCommands[];
extern const size_t micCommandsCount;

#endif // ENABLE_MICROPHONE_SENSOR

#endif // SYSTEM_MICROPHONE_H
