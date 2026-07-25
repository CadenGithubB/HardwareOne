// =============================================================================
// Settings-editor core — display-independent helpers (see header)
// =============================================================================
// Compiled on every board. Bodies were lifted verbatim from
// OLED_SettingsEditor.cpp (which is OLED-only) so the G2 lens can share them.

#include "System_SettingsEditorCore.h"

#include <Arduino.h>
#include <cstring>
#include <strings.h>   // strcasecmp

#include "System_BuildConfig.h"
#include "System_Command.h"   // findCommand

// gThermalConnected / gTofConnected come from the sensor headers.
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif

// -----------------------------------------------------------------------------
// Visibility
// -----------------------------------------------------------------------------
bool settingsEditorIsVisible(const SettingEntry* entry) {
  if (!entry || !entry->jsonKey) return true;

  // Hide Thermal I2C clock if thermal sensor not compiled or not connected.
  if (strcmp(entry->jsonKey, "i2cClockThermalHz") == 0) {
#if ENABLE_THERMAL_SENSOR
    return gThermalConnected;
#else
    return false;
#endif
  }

  // Hide ToF I2C clock if ToF sensor not compiled or not connected.
  if (strcmp(entry->jsonKey, "i2cClockToFHz") == 0) {
#if ENABLE_TOF_SENSOR
    return gTofConnected;
#else
    return false;
#endif
  }

  return true;
}

// -----------------------------------------------------------------------------
// Editability
// -----------------------------------------------------------------------------
bool settingsEditorIsEditable(const SettingEntry* entry, uint32_t typeMask) {
  if (!entry) return false;
  if (entry->readOnly || entry->isSecret) return false;
  if ((typeMask & (1u << (uint32_t)entry->type)) == 0) return false;
  return settingsEditorIsVisible(entry);
}

// -----------------------------------------------------------------------------
// Width-correct current-value read
// -----------------------------------------------------------------------------
int settingsEditorCurrentValue(const SettingEntry* entry) {
  if (!entry || !entry->valuePtr) return 0;

  switch (entry->type) {
    case SETTING_INT:  return *((int*)entry->valuePtr);
    case SETTING_U8:   return (int)*((uint8_t*)entry->valuePtr);
    case SETTING_U16:  return (int)*((uint16_t*)entry->valuePtr);
    case SETTING_U32:  return (int)*((uint32_t*)entry->valuePtr);
    case SETTING_BOOL: return *((bool*)entry->valuePtr) ? 1 : 0;
    default:           return 0;
  }
}

// -----------------------------------------------------------------------------
// Enum "options" parser
// -----------------------------------------------------------------------------
bool settingsEditorHasEnumOptions(const SettingEntry* entry) {
  if (!entry || !entry->options || !entry->options[0]) return false;
  if (strncmp(entry->options, "bitmask:", 8) == 0) return false;
  return true;
}

int settingsEditorEnumCount(const char* options) {
  if (!options || !options[0]) return 0;
  int count = 1;
  for (const char* p = options; *p; p++) {
    if (*p == ',') count++;
  }
  return count;
}

bool settingsEditorEnumAt(const char* options, int idx,
                          char* valueOut, size_t valueCap,
                          char* labelOut, size_t labelCap) {
  if (!options || idx < 0) return false;
  const char* tokStart = options;
  for (int i = 0; i < idx; i++) {
    const char* comma = strchr(tokStart, ',');
    if (!comma) return false;
    tokStart = comma + 1;
  }
  const char* tokEnd = strchr(tokStart, ',');
  size_t tokLen = tokEnd ? (size_t)(tokEnd - tokStart) : strlen(tokStart);

  // Split on '|' (legacy ':' fallback) within the token.
  const char* sep = (const char*)memchr(tokStart, '|', tokLen);
  if (!sep) sep = (const char*)memchr(tokStart, ':', tokLen);

  const char* valStart = tokStart;
  size_t valLen = sep ? (size_t)(sep - tokStart) : tokLen;
  const char* labStart = sep ? (sep + 1) : tokStart;
  size_t labLen = sep ? (tokLen - valLen - 1) : tokLen;

  if (valueOut && valueCap > 0) {
    if (valLen >= valueCap) valLen = valueCap - 1;
    memcpy(valueOut, valStart, valLen);
    valueOut[valLen] = '\0';
  }
  if (labelOut && labelCap > 0) {
    if (labLen >= labelCap) labLen = labelCap - 1;
    memcpy(labelOut, labStart, labLen);
    labelOut[labLen] = '\0';
  }
  return true;
}

int settingsEditorEnumIndexForCurrent(const SettingEntry* entry) {
  if (!entry || !entry->options) return 0;
  char cur[48];
  if (entry->type == SETTING_STRING) {
    snprintf(cur, sizeof(cur), "%s", ((String*)entry->valuePtr)->c_str());
  } else {
    snprintf(cur, sizeof(cur), "%d", settingsEditorCurrentValue(entry));
  }
  const int n = settingsEditorEnumCount(entry->options);
  char val[48];
  for (int i = 0; i < n; i++) {
    if (settingsEditorEnumAt(entry->options, i, val, sizeof(val), nullptr, 0) &&
        strcasecmp(val, cur) == 0) {
      return i;
    }
  }
  return 0;
}

// -----------------------------------------------------------------------------
// Command resolution
// -----------------------------------------------------------------------------
const char* settingsEditorCommandName(const SettingEntry* entry) {
  if (!entry) return nullptr;
  if (entry->cmdKey) return entry->cmdKey;
  return entry->jsonKey;
}

bool settingsEditorHasCommand(const SettingEntry* entry) {
  const char* cmdName = settingsEditorCommandName(entry);
  if (!cmdName || !cmdName[0]) return false;
  return findCommand(String(cmdName)) != nullptr;
}
