// test_g2_page_publish.py compiles the complete production LED page
// (G2_Page_LED.cpp, includes stripped) beside these boundaries. The
// g2ShowListPage double can refuse a render; a refused render leaves the
// previous rows on the lens, so taps and the hijack page must keep their
// old meaning.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_NEOPIXEL 1
#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)

// INSERT_BOUNDARY_TYPES
// INSERT_PAGINATOR

// --- Lens, page and command boundaries -------------------------------------
static bool listAccepts = true;
static unsigned listsShown = 0, listRefusals = 0, menuGen = 0, sensorsMenus = 0;
static std::vector<std::string> shownRows;
static G2HijackPage page = G2_HIJACK_PAGE_SENSORS;
static std::vector<std::string> submitted;

bool g2ShowListPage(const char* const* rows, size_t count) {
  if (!listAccepts) { ++listRefusals; return false; }
  ++listsShown;
  shownRows.assign(rows, rows + count);
  return true;
}
void g2BumpMenuGen() { ++menuGen; }
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) { if (page != value) { page = value; ++menuGen; } }
void g2ShowSensorsMenu() { ++sensorsMenus; page = G2_HIJACK_PAGE_SENSORS; }
using Completion = void (*)(bool, const char*, const G2CmdCookie&, void*);
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie&, Completion, void*) {
  submitted.emplace_back(line);
  return true;
}
bool g2EnqueueRedraw(const G2CmdCookie&, void (*)(), const char*) { return true; }

// --- NeoPixel catalog and settings -----------------------------------------
struct RGB { uint8_t r, g, b; };
struct ColorEntry { const char* name; RGB rgb; };
const ColorEntry colorTable[] = {
  {"red", {255, 0, 0}}, {"green", {0, 255, 0}}, {"blue", {0, 0, 255}},
  {"white", {255, 255, 255}}, {"amber", {255, 191, 0}},
};
const int numColors = (int)(sizeof(colorTable) / sizeof(colorTable[0]));
const char* const ledEffectNames[] = {"rainbow", "pulse"};
const int ledEffectNameCount = 2;
#define memcpy_P std::memcpy
int ledBrightnessNextPreset(int current) { return current >= 100 ? 10 : current + 10; }
struct { int ledBrightness = 50; } gSettings;

// INSERT_LIST_PUBLISH_HELPER

// INSERT_LED_PAGE

// --- Scenarios --------------------------------------------------------------
static bool shown(size_t row, const char* text) {
  return row < shownRows.size() && shownRows[row].find(text) != std::string::npos;
}

static void rootUp() {
  listAccepts = true;
  submitted.clear();
  page = G2_HIJACK_PAGE_SENSORS;
  g2ShowLedMenu();
  CHECK(page == G2_HIJACK_PAGE_LED);
  CHECK(shownRows.size() == 5 && shown(1, "Color >") && shown(4, "Off"));
}

// Entry refused from Sensors: the Sensors rows keep the Sensors dispatcher.
static void testRefusedEntry() {
  listAccepts = true;
  page = G2_HIJACK_PAGE_SENSORS;
  listAccepts = false;
  g2ShowLedMenu();
  listAccepts = true;
  const bool refusedEntryClaimedLedPage = page == G2_HIJACK_PAGE_LED;
  CHECK(!refusedEntryClaimedLedPage);
}

// Color list refused: the visible ROOT row 4 is "Off".
static void testRefusedColors() {
  rootUp();
  listAccepts = false;
  g2LedHandleTap(1);
  listAccepts = true;
  g2LedHandleTap(4);
  const bool refusedColorsSentColor =
      !submitted.empty() && submitted.back().compare(0, 9, "ledcolor ") == 0;
  CHECK(!refusedColorsSentColor);
  CHECK(submitted.size() == 1 && submitted.back() == "ledclear");
}

// Effects list refused: the visible ROOT row 1 still opens the colors.
static void testRefusedEffects() {
  rootUp();
  listAccepts = false;
  g2LedHandleTap(2);
  listAccepts = true;
  g2LedHandleTap(1);
  CHECK(submitted.empty());
  CHECK(shown(0, "<- Back") && shown(1, "off") && shown(2, "red"));
}

// Back to ROOT refused: the color rows still pick colors.
static void testRefusedBack() {
  rootUp();
  g2LedHandleTap(1);
  CHECK(shown(3, "green"));
  listAccepts = false;
  g2LedHandleTap(0);
  listAccepts = true;
  g2LedHandleTap(3);
  CHECK(submitted.size() == 1 && submitted.back() == "ledcolor green");
}

int main() {
  testRefusedEntry();
  testRefusedColors();
  testRefusedEffects();
  testRefusedBack();
  char info[96];
  g2BuildLedInfo(info, sizeof(info));
  CHECK(std::strstr(info, "Brightness: 50%") != nullptr);
  if (failures) {
    std::fprintf(stderr, "G2 LED publish: %u of %u checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("G2 LED publish: %u checks passed (%u lists, %u Sensors menus)\n",
              checks, listsShown, sensorsMenus);
  return 0;
}
