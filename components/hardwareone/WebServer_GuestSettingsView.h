#pragma once

// The guest's view of GET /api/settings (owner, guest privacy 4, 2026-11-22):
// a guest sees the settings without the saved Wi-Fi networks (their SSIDs),
// the MQTT broker host and user name, and the LLM gateway address. Users and
// administrators get the settings unchanged.
//
// A hidden text value is replaced by kGuestHiddenText, so every settings
// renderer on the page shows that sentence in the field instead of an empty
// box that reads as "not configured"; an empty value stays empty (nothing is
// hidden). The saved-network list is dropped whole. The paths are the
// settings modules' own: network.wifi.networks (System_WiFiNetworks.h),
// network.mqtt + group broker (System_MQTT.cpp), apps.llm + group http
// (System_LLMCommands.cpp); host test web_guest_settings_view pins them.
//
// Dependency-free apart from ArduinoJson, so the host test compiles it as is.

#include <ArduinoJson.h>
#include <stddef.h>
#include <string.h>

namespace guestview {

inline constexpr char kGuestHiddenText[] = "(not shown to guests)";

enum class GuestHide : unsigned char {
  Mask,  // a non-empty text value becomes kGuestHiddenText
  Drop,  // the key is removed
};

struct GuestHiddenSetting {
  const char* path;  // dotted path from the settings object's root
  GuestHide how;
};

inline constexpr GuestHiddenSetting kGuestHiddenSettings[] = {
  {"network.wifi.networks", GuestHide::Drop},
  {"network.mqtt.broker.mqttHost", GuestHide::Mask},
  {"network.mqtt.broker.mqttUser", GuestHide::Mask},
  {"apps.llm.http.url", GuestHide::Mask},
};

// Applies the table to `settings` (the object GET /api/settings sends under
// "settings"). Returns how many values were hidden. A path whose section is
// absent from this build is skipped.
inline size_t reduceSettingsForGuest(JsonObject settings) {
  size_t hidden = 0;
  if (settings.isNull()) return 0;
  for (const GuestHiddenSetting& row : kGuestHiddenSettings) {
    char key[48];
    JsonObject parent = settings;
    const char* cursor = row.path;
    bool found = true;
    for (;;) {
      const char* dot = strchr(cursor, '.');
      const size_t len = dot ? static_cast<size_t>(dot - cursor) : strlen(cursor);
      if (len == 0 || len >= sizeof(key)) { found = false; break; }
      memcpy(key, cursor, len);
      key[len] = '\0';
      if (!dot) break;
      parent = parent[static_cast<const char*>(key)].as<JsonObject>();
      if (parent.isNull()) { found = false; break; }
      cursor = dot + 1;
    }
    if (!found) continue;
    const char* leaf = key;
    JsonVariant value = parent[leaf];
    if (value.isNull()) continue;
    if (row.how == GuestHide::Drop) {
      parent.remove(leaf);
      ++hidden;
      continue;
    }
    const char* text = value.as<const char*>();
    if (!text || text[0] == '\0') continue;
    parent[leaf] = kGuestHiddenText;
    ++hidden;
  }
  return hidden;
}

}  // namespace guestview
