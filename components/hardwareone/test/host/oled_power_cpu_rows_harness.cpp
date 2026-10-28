// Builds the OLED Power > CPU rows with the production mode table and getters
// (System_Power.cpp) and row formatter (OLED_Mode_Power.cpp
// populatePowerCpuMenu) for one chip's clock set, and checks that every row
// plus its "> " cursor fits one line of the 128-pixel-wide UI (SSD1306 and the
// P4X-EYE's logical canvas): 21 glyphs of 6 px at text size 1. The runner
// compiles it once per clock set.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

class String;
#include "System_Power.h"

#define EXT_RAM_BSS_ATTR

struct OLEDScrollState {
  std::vector<std::string> rows;
};
static OLEDScrollState sPowerCpuScroll;
static void initPowerScrollStates() {}
static void oledScrollClearKeepSelection(OLEDScrollState* s) { s->rows.clear(); }
static void oledScrollAddItem(OLEDScrollState* s, const char* line) { s->rows.push_back(line); }
static void oledScrollClampSelection(OLEDScrollState*) {}

// INSERT_PRODUCTION_DEFINITIONS

int main() {
  constexpr size_t kLineGlyphs = 128 / 6;
  constexpr size_t kCursorGlyphs = 2;  // "> " or "  "
  populatePowerCpuMenu();
  populatePowerCpuMenu();  // a rebuild replaces the rows, it does not append
  assert(sPowerCpuScroll.rows.size() == POWER_MODE_COUNT);
  for (uint8_t mode = 0; mode < POWER_MODE_COUNT; ++mode) {
    const std::string& row = sPowerCpuScroll.rows[mode];
    std::printf("  [%zu] %s\n", kCursorGlyphs + row.size(), row.c_str());
    assert(row.rfind(getPowerModeName(mode), 0) == 0);
    assert(row.find("MHz") == std::string::npos);
    assert(kCursorGlyphs + row.size() <= kLineGlyphs);
  }
  return 0;
}
