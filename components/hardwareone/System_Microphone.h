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

// Microphone initialization
bool initMicrophone();
void stopMicrophone();

// Capture audio samples (returns raw PCM data)
// Caller must free the buffer with free() when done
int16_t* captureAudioSamples(size_t sampleCount, size_t* outLen);

// Get audio level (0-100 for VU meter display)
int getAudioLevel();

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
