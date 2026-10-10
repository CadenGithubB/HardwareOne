#ifndef SYSTEM_ICONS_H
#define SYSTEM_ICONS_H

#include <Arduino.h>

struct EmbeddedIcon {
  const char* name;
  const uint8_t* pngData;
  size_t pngSize;
  const uint8_t* bitmapData;
  uint8_t width;
  uint8_t height;
};

extern const EmbeddedIcon EMBEDDED_ICONS[];
extern const size_t EMBEDDED_ICONS_COUNT;

const EmbeddedIcon* findEmbeddedIcon(const char* name);

// Embedded asset availability, independent of the display backend.
bool iconExists(const char* name);

// Map a file extension to its icon name. Ignores one leading dot and ASCII case;
// empty/unknown extensions use "file". The returned string has static lifetime.
const char* getIconNameForExtension(const char* ext);

#endif
