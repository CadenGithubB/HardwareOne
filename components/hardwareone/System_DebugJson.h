#ifndef SYSTEM_DEBUG_JSON_H
#define SYSTEM_DEBUG_JSON_H

// JSON command replies that share the debug buffer (System_Debug.h).

#include <ArduinoJson.h>
#include "System_Debug.h"

// Serializes `doc` into the shared debug buffer, sized with measureJson so a
// reply is never cut mid-object. When the whole document does not fit,
// trailing entries of `list` (an array inside `doc`) are dropped and
// "truncated":true is added, so the reply still parses and says it is
// partial. Returns nullptr when the buffer is unavailable or the document
// does not fit even with `list` emptied; the caller then replies with an
// explicit error.
inline const char* serializeJsonListToDebugBuffer(JsonDocument& doc, JsonArray list) {
  if (!ensureDebugBuffer()) return nullptr;
  if (measureJson(doc) >= GLOBAL_DEBUG_BUFFER_SIZE) {
    doc["truncated"] = true;
    size_t total = measureJson(doc);
    size_t count = list.size();
    // An array costs its brackets, each entry, and one comma between entries.
    while (total >= GLOBAL_DEBUG_BUFFER_SIZE && count > 0) {
      --count;
      total -= measureJson(list[count]) + (count > 0 ? 1 : 0);
      list.remove(count);
    }
    if (measureJson(doc) >= GLOBAL_DEBUG_BUFFER_SIZE) return nullptr;
  }
  serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);
  return getDebugBuffer();
}

#endif  // SYSTEM_DEBUG_JSON_H
