// =============================================================================
// G2 glasses — "LED" sub-page implementation
// =============================================================================
// See header for the contract. Mirrors the ESP-NOW App page's sub-mode shape
// (file-static level enum + per-level showMenu + one tap dispatcher) and the
// Automations page's cmd_exec redraw plumbing. Color rows use the shared
// G2Paginator chrome ("<< Prev page" / "Next page >>") since 76 names far
// exceed one lens screen.

#include "G2_Page_LED.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_NEOPIXEL

#include <Arduino.h>
#include "G2_Glasses.h"        // g2ShowListPage / g2Set/GetHijackPage
#include "G2_Page_Common.h"    // G2Paginator chrome
#include "G2_Page_Sensors.h"   // g2ShowSensorsMenu — Back target
#include "G2_HijackCmd.h"      // G2CmdCookie / g2SubmitHijackCommand / redraw plumbing
#include "System_NeoPixel.h"   // colorTable / ledEffectNames / ledBrightnessNextPreset
#include "System_Settings.h"   // gSettings.ledBrightness (label)
#include "System_Debug.h"

// -----------------------------------------------------------------------------
// Sub-level state
// -----------------------------------------------------------------------------
enum LedSub : uint8_t { LED_SUB_ROOT = 0, LED_SUB_COLORS = 1, LED_SUB_EFFECTS = 2 };
static LedSub gSub = LED_SUB_ROOT;
static size_t gColorPage = 0;   // paginator page for the color list

// Every transition bumps the menu generation so in-flight cmd_exec redraw
// callbacks with an older gen get dropped (same rationale as ESP-NOW's setSub).
// The renderers call it only once the lens accepted the sub-level's rows.
static inline void setSub(LedSub s) {
  if (gSub == s) return;
  gSub = s;
  g2BumpMenuGen();
}

static G2CmdCookie buildCookie() {
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gSub;
  return cookie;
}

// -----------------------------------------------------------------------------
// Renderers
// -----------------------------------------------------------------------------
// ROOT is also the page's entry level, so its publish claims the LED hijack
// page (a no-op when already there).
static void showRootMenu() {
  // Live brightness label — static buffer because g2ShowListPage expects the
  // row pointers to stay valid until it returns (same idiom everywhere).
  EXT_RAM_BSS_ATTR static char brightRow[28];  // PSRAM: deep-copied by g2ShowListPage
  snprintf(brightRow, sizeof(brightRow), "Brightness: %d%%", gSettings.ledBrightness);
  const char* items[] = {
    "<- Hardware",   // 0
    "Color >",      // 1
    "Effect >",     // 2
    brightRow,      // 3 — tap cycles the shared preset ladder
    "Off",          // 4 — ledclear (also cancels a running effect)
  };
  g2ShowListPageAndPublish(items, sizeof(items) / sizeof(items[0]), [] {
    setSub(LED_SUB_ROOT);
    g2SetHijackPage(G2_HIJACK_PAGE_LED);
  });
}

// Colors: "<- Back" + up to kColorsPerPage names + paginator chrome. 76 names
// (off + colorTable) at 10/page = 8 pages.
static constexpr size_t kColorsPerPage = 10;
static constexpr size_t kColorRowMax   = 1 + kColorsPerPage + 2;  // back + items + prev/next
static G2Paginator gColorPg;   // window of the color rows on the lens

static void showColorsMenu() {
  EXT_RAM_BSS_ATTR static char rows[kColorRowMax][20];
  EXT_RAM_BSS_ATTR static const char* ptrs[kColorRowMax];  // PSRAM: deep-copied by g2ShowListPage
  const size_t total = 1 + (size_t)numColors;   // "off" + palette

  G2Paginator pg = g2PaginatorPrepare(total, kColorsPerPage, gColorPage);
  size_t row = 0;
  snprintf(rows[row], sizeof(rows[row]), "<- Back");
  ptrs[row] = rows[row];
  row++;
  for (size_t i = pg.startIdx; i < pg.endIdx; i++) {
    if (i == 0) {
      snprintf(rows[row], sizeof(rows[row]), "off");
    } else {
      ColorEntry entry;
      memcpy_P(&entry, &colorTable[i - 1], sizeof(ColorEntry));
      snprintf(rows[row], sizeof(rows[row]), "%s", entry.name);
    }
    ptrs[row] = rows[row];
    row++;
  }
  row = g2PaginatorWriteChrome(pg, gColorPage, row, kColorRowMax,
                               &rows[0][0], sizeof(rows[0]), ptrs);
  g2ShowListPageAndPublish(ptrs, row, [&pg] {
    setSub(LED_SUB_COLORS);
    gColorPg = pg;
  });
}

static void showEffectsMenu() {
  const char* items[2 + 8];   // back + shared names + off (headroom)
  size_t n = 0;
  items[n++] = "<- Back";
  for (int i = 0; i < ledEffectNameCount && n < sizeof(items) / sizeof(items[0]) - 1; i++) {
    items[n++] = ledEffectNames[i];
  }
  items[n++] = "off";
  g2ShowListPageAndPublish(items, n, [] { setSub(LED_SUB_EFFECTS); });
}

// -----------------------------------------------------------------------------
// Tap dispatch
// -----------------------------------------------------------------------------
static void ledBrightnessDone(bool /*ok*/, const char* /*result*/,
                              const G2CmdCookie& c, void* /*ud*/) {
  // Re-render ROOT so the brightness label reflects the committed value.
  (void)g2EnqueueRedraw(c, showRootMenu, "LED brightness");
}

static void rootHandleTap(uint32_t idx) {
  switch (idx) {
    case 0: g2ShowSensorsMenu(); return;          // <- Sensors
    case 1: gColorPage = 0; showColorsMenu(); return;
    case 2: showEffectsMenu(); return;
    case 3: {
      // Cycle the shared preset ladder; redraw on completion so the label
      // shows the value the command actually committed.
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "ledbrightness %d",
               ledBrightnessNextPreset(gSettings.ledBrightness));
      if (!g2SubmitHijackCommand(cmd, buildCookie(), ledBrightnessDone, nullptr)) {
        DEBUG_G2F("[G2-LED] brightness submit failed (queue full)");
      }
      return;
    }
    case 4:
      (void)g2SubmitHijackCommand("ledclear", buildCookie(), nullptr, nullptr);
      return;
    default: return;
  }
}

static void colorsHandleTap(uint32_t idx) {
  if (idx == 0) { showRootMenu(); return; }       // <- Back
  // Paginator chrome rows?
  if (gColorPg.prevRow >= 0 && idx == (uint32_t)gColorPg.prevRow) {
    if (gColorPage > 0) gColorPage--;
    showColorsMenu();
    return;
  }
  if (gColorPg.nextRow >= 0 && idx == (uint32_t)gColorPg.nextRow) {
    gColorPage++;
    showColorsMenu();
    return;
  }
  // Item row: map back to the color index. Row 1 on the page = startIdx.
  const size_t item = gColorPg.startIdx + (idx - 1);
  if (item >= gColorPg.endIdx) return;
  char cmd[32];
  if (item == 0) {
    snprintf(cmd, sizeof(cmd), "ledcolor off");
  } else {
    ColorEntry entry;
    memcpy_P(&entry, &colorTable[item - 1], sizeof(ColorEntry));
    snprintf(cmd, sizeof(cmd), "ledcolor %s", entry.name);
  }
  // Fire-and-forget: the result is visible on the physical LED itself, and
  // staying on the list lets the user flip through swatches.
  (void)g2SubmitHijackCommand(cmd, buildCookie(), nullptr, nullptr);
}

static void effectsHandleTap(uint32_t idx) {
  if (idx == 0) { showRootMenu(); return; }       // <- Back
  const int sel = (int)idx - 1;
  if (sel > ledEffectNameCount) return;           // names + trailing off row
  const char* name = (sel < ledEffectNameCount) ? ledEffectNames[sel] : "off";
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "ledeffect %s", name);
  // Non-blocking engine: returns immediately; picking another effect replaces
  // the running one, "off" cancels. Stay on the list.
  (void)g2SubmitHijackCommand(cmd, buildCookie(), nullptr, nullptr);
}

void g2LedHandleTap(uint32_t idx) {
  switch (gSub) {
    case LED_SUB_COLORS:  colorsHandleTap(idx);  return;
    case LED_SUB_EFFECTS: effectsHandleTap(idx); return;
    default:              rootHandleTap(idx);    return;
  }
}

// -----------------------------------------------------------------------------
// Entry + registry stub
// -----------------------------------------------------------------------------
void g2ShowLedMenu() {
  gColorPage = 0;
  showRootMenu();   // claims the LED page only if the lens shows it
}

void g2BuildLedInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "LED control (color / effect / brightness)\nBrightness: %d%%",
           gSettings.ledBrightness);
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_NEOPIXEL
