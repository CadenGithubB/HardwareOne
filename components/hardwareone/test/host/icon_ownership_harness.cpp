#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include "System_BuildConfig.h"
#include "System_Icons.h"
#if ENABLE_OLED_DISPLAY
#include "OLED_UI.h"
DisplayDriver* gDisplay = nullptr;
#endif

extern const EmbeddedIcon* findProductionEmbeddedIcon(const char* name);
static const EmbeddedIcon* injectedIcon = nullptr;
const EmbeddedIcon* findEmbeddedIcon(const char* name) {
  return injectedIcon ? injectedIcon : findProductionEmbeddedIcon(name);
}
#define DEBUG_STORAGEF(...) ((void)0)

// INSERT_PRODUCTION

static void bytes(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) std::printf("%02x", unsigned(data[i]));
}

#if ENABLE_OLED_DISPLAY
static bool render(DisplayDriver* display, const char* name, int x, int y, uint16_t color, float scale) {
#if BASELINE_CAPTURE
  return drawIconScaled(display, name, x, y, color, scale);
#else
  return oledDrawIcon(display, name, x, y, color, scale);
#endif
}
static void pixels(const PixelRecorder& display) {
  for (const auto& pixel : display.pixels)
    std::printf("%d,%d,%u;", pixel.first.first, pixel.first.second, unsigned(pixel.second));
}
static void rendering() {
  DisplayDriver display;
  const float scales[] = {0.01f, 0.25f, 0.49f, 0.5f, 0.51f, 0.75f, 1.0f, 1.3f, 2.0f};
  for (size_t i = 0; i < EMBEDDED_ICONS_COUNT; ++i) {
    const auto& icon = EMBEDDED_ICONS[i];
    for (float scale : scales) {
      display.pixels.clear();
      const bool ok = render(&display, icon.name, 3, 5, 0xA55A, scale);
      std::printf("render:%s:%.3f\t%d:", icon.name, double(scale), int(ok));
      pixels(display); std::puts("");
    }
    display.pixels.clear();
    const bool ok = render(&display, icon.name, -5, -7, 0, 1.0f);
    std::printf("clipped:%s\t%d:", icon.name, int(ok)); pixels(display); std::puts("");
#if BASELINE_CAPTURE
    DisplayDriver unscaled;
    assert(drawIcon(&unscaled, icon.name, -5, -7, 0));
    assert(unscaled.pixels == display.pixels);
#else
    gDisplay = &display;
    for (int size : {16, 32, 64}) {
      DisplayDriver explicitDisplay;
      assert(oledDrawIcon(&explicitDisplay, icon.name, 4, 6, DISPLAY_FG, float(size) / 32));
      display.pixels.clear(); oledDrawIcon(4, 6, icon.name, size);
      assert(display.pixels == explicitDisplay.pixels);
    }
    gDisplay = nullptr;
#endif
  }
  display.pixels.clear();
  assert(!render(nullptr, "file", 0, 0, 1, 1));
  assert(!render(&display, "no-such-icon", 0, 0, 1, 1));
  assert(!render(&display, "", 0, 0, 1, 1));
  assert(!render(&display, "file", 0, 0, 1, 0));
  assert(!render(&display, "file", 0, 0, 1, -1));
#if !BASELINE_CAPTURE
  assert(!render(&display, nullptr, 0, 0, 1, 1));
  // Existing scroll consumers expose an SSD1306 pointer even when the selected
  // display backend is different. Both remain valid common-GFX consumers.
  Adafruit_SSD1306 ssd;
  Adafruit_GFX* common = &ssd;
  assert(oledDrawIcon(common, "file", 0, 0, 1));
  assert(!ssd.pixels.empty());
  assert(!render(&display, "file", 0, 0, 1, std::numeric_limits<float>::quiet_NaN()));
  assert(!render(&display, "file", 0, 0, 1, std::numeric_limits<float>::infinity()));
  assert(!render(&display, "file", 0, 0, 1, std::numeric_limits<float>::max()));
  const uint8_t bitmap[128] = {};
  EmbeddedIcon malformed{"bad", nullptr, 0, nullptr, 32, 32};
  injectedIcon = &malformed;
  assert(!render(&display, "bad", 0, 0, 1, 1));
  malformed.bitmapData = bitmap;
  for (uint8_t width : {0, 16, 31, 33}) {
    malformed.width = width;
    assert(!render(&display, "bad", 0, 0, 1, 1));
  }
  malformed.width = 32; malformed.height = 16;
  assert(!render(&display, "bad", 0, 0, 1, 1));
  injectedIcon = nullptr;
  oledDrawIcon(0, 0, "file", 32); // Null global display remains harmless.
  gDisplay = &display;
  oledDrawIcon(0, 0, nullptr, 32);
  oledDrawIcon(0, 0, "", 32);
  oledDrawIcon(0, 0, "file", 0);
  gDisplay = nullptr;
#endif
  assert(display.pixels.empty());
}
#endif

int main() {
  std::printf("count\t%zu\n", EMBEDDED_ICONS_COUNT);
  for (size_t i = 0; i < EMBEDDED_ICONS_COUNT; ++i) {
    const auto& icon = EMBEDDED_ICONS[i];
    assert(findEmbeddedIcon(icon.name) == &icon && iconExists(icon.name));
    assert(icon.width == 32 && icon.height == 32);
    assert(bool(icon.pngData) == bool(ENABLE_HTTP_SERVER));
    assert(bool(icon.bitmapData) == bool(ENABLE_OLED_DISPLAY));
    assert(bool(icon.pngSize) == bool(ENABLE_HTTP_SERVER));
    std::printf("asset:%s\t%u,%u,", icon.name, unsigned(icon.width), unsigned(icon.height));
    if (icon.pngData) bytes(icon.pngData, icon.pngSize);
    std::printf("|"); if (icon.bitmapData) bytes(icon.bitmapData, 128); std::puts("");
  }
  assert(!iconExists("no-such-icon") && !iconExists(""));
#if !BASELINE_CAPTURE
  assert(!iconExists(nullptr));
#endif
  for (const char* extension : {static_cast<const char*>(nullptr), "", ".", "JSON", ".json", "txt", "md", "log",
       "ino", "cpp", "h", "hpp", "c", "py", "js", "html", "css", "xml", "yaml", "yml", "JPG", "jpeg", "png",
       "gif", "bmp", "svg", "zip", "tar", "gz", "7z", "rar", "pdf", "bin", "hex", "elf", ".unknown",
       "verylongunknownextension", "..json"}) {
    std::printf("extension:%s\t%s\n", extension ? extension : "<null>", getIconNameForExtension(extension));
  }
#if ENABLE_OLED_DISPLAY
  rendering();
#endif
}
