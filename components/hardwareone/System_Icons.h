#ifndef ICONS_EMBEDDED_H
#define ICONS_EMBEDDED_H

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

#endif
