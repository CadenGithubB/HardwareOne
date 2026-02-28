#ifndef OLED_REMOTE_SETTINGS_H
#define OLED_REMOTE_SETTINGS_H

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#include <Arduino.h>
#include "System_Settings.h"

// Remote settings mode - displays and edits settings from bonded peer device
// Reuses the existing SettingsEditor infrastructure but loads data from cache

// Load remote settings from cache and populate virtual settings modules
// Returns true if settings were loaded successfully
bool loadRemoteSettingsModules();

// Free remote settings modules (call when exiting remote settings mode)
void freeRemoteSettingsModules();

// Get remote settings modules (similar to getSettingsModules but for remote)
const SettingsModule** getRemoteSettingsModules(size_t& count);

// Apply a remote setting change by sending command to bonded peer
// Returns true if command was sent successfully
bool applyRemoteSettingChange(const char* moduleName, const char* settingKey, const String& value);

// Check if remote settings are available (bonded and cached)
bool hasRemoteSettings();

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#endif // OLED_REMOTE_SETTINGS_H
