#ifndef SYSTEM_USERSETTINGS_H
#define SYSTEM_USERSETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>

bool loadUserSettings(uint32_t userId, JsonDocument& doc);
bool saveUserSettings(uint32_t userId, const JsonDocument& doc);
bool mergeAndSaveUserSettings(uint32_t userId, const JsonDocument& patch);
String getUserSettingsPath(uint32_t userId);

#endif
