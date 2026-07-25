// OLED_Mode_LED.cpp - NeoPixel LED control screen (Sensors submenu)
//
// Three internal levels inside one OLED mode (mirrors OLED_Mode_FileBrowser's
// internal-level pattern rather than Power's one-enum-per-submenu, so the
// mode costs a single OLEDMode value):
//   ROOT     Color >  /  Effect >  /  Brightness: NN%  /  Off
//   COLORS   one row per colorTable[] name (+ "off" first) — A = ledcolor <name>
//   EFFECTS  fade / pulse / blink / rainbow / strobe / off — A = ledeffect <name>
//
// All actions route through executeOLEDCommand so they carry the normal [CMD]
// audit line and the OLED auth identity — same pattern as the Power menu.
// ledcolor/ledeffect are LIVE-only (persist nothing); brightness persists via
// ledbrightness, matching the web Live Control panel's behavior. The persisted
// startup effect/color settings stay in the Settings editor — this screen
// deliberately does not touch them.
//
// Effects are NON-BLOCKING as of the ledEffectStart/ledEffectTick engine
// (System_NeoPixel.cpp): cmd_ledeffect starts the effect and returns
// immediately, frames advance from the main loop, and the UI stays live.
// The historical "never wire a duration arg" footgun (the old engine parked
// cmd_exec + this UI for the whole run) no longer applies; effects still run
// the 3 s default here, and Off / a new pick cancels a running effect.
//
// The file compiles wherever the OLED does — including boards without a
// NeoPixel — per the board-gated-code-hides-compile-breaks rule. Reachability
// is gated instead: the Sensors submenu row and getMenuAvailability() both
// key on NEOPIXEL_PIN_DEFAULT >= 0 (there is no ENABLE_NEOPIXEL macro).

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"       // executeOLEDCommand, OLEDScrollState, guest gate
#include "HAL_Input.h"
#include "System_Settings.h"  // gSettings.ledBrightness (display + nudge base)
#include "System_NeoPixel.h"  // colorTable / numColors (PROGMEM palette)
#include "System_Debug.h"

// ---------------------------------------------------------------------------
// Internal level state
// ---------------------------------------------------------------------------
enum LedLevel : uint8_t { LED_LEVEL_ROOT = 0, LED_LEVEL_COLORS, LED_LEVEL_EFFECTS };
static LedLevel gLedLevel = LED_LEVEL_ROOT;

EXT_RAM_BSS_ATTR static OLEDScrollState sLedRootScroll;
EXT_RAM_BSS_ATTR static OLEDScrollState sLedColorScroll;
EXT_RAM_BSS_ATTR static OLEDScrollState sLedEffectScroll;
static bool sLedScrollInitialized = false;

// Effect rows = the shared ledEffectNames table (single source of truth with
// the cmd_ledeffect parser and the G2 LED page) + a trailing "off" cancel row.

static void initLedScrollStates() {
  if (sLedScrollInitialized) return;
  const int fullVis = OLED_CONTENT_HEIGHT / 8;
  oledScrollInit(&sLedRootScroll,   nullptr, fullVis > 0 ? fullVis : 1);
  oledScrollInit(&sLedColorScroll,  nullptr, fullVis > 0 ? fullVis : 1);
  oledScrollInit(&sLedEffectScroll, nullptr, fullVis > 0 ? fullVis : 1);
  sLedScrollInitialized = true;
}

static void populateLedRootMenu() {
  initLedScrollStates();
  oledScrollClearKeepSelection(&sLedRootScroll);
  oledScrollAddItem(&sLedRootScroll, "Color >");
  oledScrollAddItem(&sLedRootScroll, "Effect >");
  // Live label, rewritten every frame (scroll items store the char*, not a
  // copy — same static-buffer idiom as Power's powerSaveLabel).
  static char brightLabel[24];
  snprintf(brightLabel, sizeof(brightLabel), "Brightness: %d%%", gSettings.ledBrightness);
  oledScrollAddItem(&sLedRootScroll, brightLabel);
  oledScrollAddItem(&sLedRootScroll, "Off");
  oledScrollClampSelection(&sLedRootScroll);
}

static void populateLedColorMenu() {
  initLedScrollStates();
  oledScrollClearKeepSelection(&sLedColorScroll);
  // "off" first (getRGBFromName alias), then the PROGMEM palette. On ESP32 the
  // ColorEntry.name pointers reference flash-resident string literals, so they
  // are valid program-lifetime char*s once copied out of the PROGMEM struct —
  // same memcpy_P idiom getRGBFromName uses.
  oledScrollAddItem(&sLedColorScroll, "off");
  for (int i = 0; i < numColors; i++) {
    ColorEntry entry;
    memcpy_P(&entry, &colorTable[i], sizeof(ColorEntry));
    oledScrollAddItem(&sLedColorScroll, entry.name);
  }
  oledScrollClampSelection(&sLedColorScroll);
}

static void populateLedEffectMenu() {
  initLedScrollStates();
  oledScrollClearKeepSelection(&sLedEffectScroll);
  for (int i = 0; i < ledEffectNameCount; i++) {
    oledScrollAddItem(&sLedEffectScroll, ledEffectNames[i]);
  }
  oledScrollAddItem(&sLedEffectScroll, "off");
  oledScrollClampSelection(&sLedEffectScroll);
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void displayLED() {
  if (!oledDisplay || !oledConnected) return;
  switch (gLedLevel) {
    case LED_LEVEL_ROOT:
      populateLedRootMenu();
      oledScrollRenderSimple(oledDisplay, &sLedRootScroll);
      break;
    case LED_LEVEL_COLORS:
      populateLedColorMenu();
      oledScrollRenderSimple(oledDisplay, &sLedColorScroll);
      break;
    case LED_LEVEL_EFFECTS:
      populateLedEffectMenu();
      oledScrollRenderSimple(oledDisplay, &sLedEffectScroll);
      break;
  }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void executeLedRootAction() {
  switch (sLedRootScroll.selectedIndex) {
    case 0: gLedLevel = LED_LEVEL_COLORS;  break;
    case 1: gLedLevel = LED_LEVEL_EFFECTS; break;
    case 2: {
      // A on the brightness row cycles the shared preset ladder (works on
      // both input backends; gamepad users can also nudge with LEFT/RIGHT
      // below). Ladder logic shared with the G2 LED page.
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "ledbrightness %d",
               ledBrightnessNextPreset(gSettings.ledBrightness));
      executeOLEDCommand(cmd);
      break;
    }
    case 3: executeOLEDCommand("ledclear"); break;
  }
}

static void executeLedColorAction() {
  const int sel = sLedColorScroll.selectedIndex;
  if (sel < 0 || sel >= sLedColorScroll.itemCount) return;
  const char* name = sLedColorScroll.items[sel].line1;
  if (!name) return;
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "ledcolor %s", name);
  executeOLEDCommand(cmd);
  // Stay on the color list so the user can flip through swatches; B backs out.
}

static void executeLedEffectAction() {
  const int sel = sLedEffectScroll.selectedIndex;
  if (sel < 0 || sel > ledEffectNameCount) return;  // names + trailing "off" row
  // Non-blocking: ledEffectStart returns immediately and the main loop drives
  // the frames, so the command round-trip is milliseconds and the menu stays
  // responsive. Picking another effect replaces the running one; "off" cancels.
  const char* name = (sel < ledEffectNameCount) ? ledEffectNames[sel] : "off";
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "ledeffect %s", name);
  executeOLEDCommand(cmd);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static bool ledInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  initLedScrollStates();

  OLEDScrollState* scroll =
      (gLedLevel == LED_LEVEL_COLORS)  ? &sLedColorScroll :
      (gLedLevel == LED_LEVEL_EFFECTS) ? &sLedEffectScroll : &sLedRootScroll;
  if (oledScrollHandleNav(scroll)) return true;

  // Gamepad LEFT/RIGHT nudges brightness ±5 while the brightness row is
  // selected. Gated off for the ANO encoder, where LEFT carries B-back
  // semantics (same rationale as the Settings editor's adjust block).
#if !ENABLE_ANO_ENCODER
  if (gLedLevel == LED_LEVEL_ROOT && sLedRootScroll.selectedIndex == 2 &&
      (gNavEvents.left || gNavEvents.right)) {
    int next = gSettings.ledBrightness + (gNavEvents.right ? 5 : -5);
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    if (next != gSettings.ledBrightness) {
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "ledbrightness %d", next);
      executeOLEDCommand(cmd);
    }
    return true;
  }
#endif

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    switch (gLedLevel) {
      case LED_LEVEL_ROOT:    executeLedRootAction();   break;
      case LED_LEVEL_COLORS:  executeLedColorAction();  break;
      case LED_LEVEL_EFFECTS: executeLedEffectAction(); break;
    }
    return true;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (gLedLevel != LED_LEVEL_ROOT) {
      gLedLevel = LED_LEVEL_ROOT;  // sub-level → back to root, stay in mode
      return true;
    }
    return false;  // root level — let the global handler pop the mode stack
  }

  return false;
}

// Fresh visit resets to ROOT; back-navigation (B from a pushed mode) keeps
// whatever level the user was on. Mirrors the onEnter convention.
static void ledOnEnter(bool isForward) {
  if (isForward) gLedLevel = LED_LEVEL_ROOT;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc,
//          showInMenu, menuOrder, hints, onEnterFunc.
// availFunc is a dead field (never invoked) — availability comes from the
// central getMenuAvailability() switch in OLED_Utils.cpp.
static const OLEDModeEntry sLedModes[] = {
  { OLED_LED, "LED", "notify_sensor", displayLED, nullptr, ledInputHandler,
    false, -1, "A:Sel B:Back", ledOnEnter },
};

REGISTER_OLED_MODE_MODULE(sLedModes, sizeof(sLedModes) / sizeof(sLedModes[0]), "LED");

// Force linker to include this file - called from OLED_Utils.cpp
void oledLEDModeInit() {}

#endif // ENABLE_OLED_DISPLAY
