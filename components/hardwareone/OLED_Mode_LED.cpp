// OLED_Mode_LED.cpp - NeoPixel LED control screen (Sensors submenu)
//
// Three internal levels inside one OLED mode (mirrors OLED_Mode_FileBrowser's
// internal-level pattern rather than Power's one-enum-per-submenu, so the
// mode costs a single OledMode value):
//   ROOT     Color >  /  Effect >  /  Brightness: NN%  /  Off
//   COLORS   one row per colorTable[] name (+ "off" first) — A = ledcolor <name>
//   EFFECTS  fade / pulse / blink / rainbow / strobe / off — A = ledeffect <name>
//
// All actions route through executeOledCommand so they carry the normal [CMD]
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
// Compiled only when BOTH the OLED and the NeoPixel feature are in the build
// (ENABLE_NEOPIXEL, System_BuildConfig.h): the body reads gSettings.led*
// fields that are gated with the feature. The registration anchor in
// OLED_Utils.cpp (oledLedModeInit call) is gated in lockstep.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_NEOPIXEL

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
static LedLevel sLedLevel = LED_LEVEL_ROOT;

EXT_RAM_BSS_ATTR static OledScrollState sLedRootScroll;
EXT_RAM_BSS_ATTR static OledScrollState sLedColorScroll;
EXT_RAM_BSS_ATTR static OledScrollState sLedEffectScroll;
// The brightness nudge's own wanted value (OLED background commands, step 2):
// a LEFT/RIGHT step moves the row at once, only the newest value is sent.
EXT_RAM_BSS_ATTR static OledValueScroll sLedBrightnessScroll;
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
  snprintf(brightLabel, sizeof(brightLabel), "Brightness: %ld%%",
           (long)oledValueScrollShown(sLedBrightnessScroll, gSettings.ledBrightness));
  oledScrollAddItem(&sLedRootScroll, brightLabel);
  oledScrollAddItem(&sLedRootScroll, "Off");
  oledScrollClampSelection(&sLedRootScroll);
}

static void populateLedColorMenu() {
  initLedScrollStates();
  oledScrollClearKeepSelection(&sLedColorScroll);
  // "off" first (getRgbFromName alias), then the PROGMEM palette. On ESP32 the
  // ColorEntry.name pointers reference flash-resident string literals, so they
  // are valid program-lifetime char*s once copied out of the PROGMEM struct —
  // same memcpy_P idiom getRgbFromName uses.
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
void displayLed() {
  if (!oledDisplay || !oledConnected) return;
  switch (sLedLevel) {
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
    case 0: sLedLevel = LED_LEVEL_COLORS;  break;
    case 1: sLedLevel = LED_LEVEL_EFFECTS; break;
    case 2: {
      // A on the brightness row cycles the shared preset ladder (works on
      // both input backends; gamepad users can also nudge with LEFT/RIGHT
      // below). Ladder logic shared with the G2 LED page.
      // The same row as the nudge: from the value it shows, coalesced.
      const int32_t shown = oledValueScrollShown(sLedBrightnessScroll, gSettings.ledBrightness);
      oledValueScrollStep(sLedBrightnessScroll, "ledbrightness %ld",
                          ledBrightnessNextPreset((int)shown));
      break;
    }
    case 3: executeOledCommand("ledclear"); break;
  }
}

static void executeLedColorAction() {
  const int sel = sLedColorScroll.selectedIndex;
  if (sel < 0 || sel >= sLedColorScroll.itemCount) return;
  const char* name = sLedColorScroll.items[sel].line1;
  if (!name) return;
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "ledcolor %s", name);
  executeOledCommand(cmd);
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
  executeOledCommand(cmd);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static bool ledInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  initLedScrollStates();

  OledScrollState* scroll =
      (sLedLevel == LED_LEVEL_COLORS)  ? &sLedColorScroll :
      (sLedLevel == LED_LEVEL_EFFECTS) ? &sLedEffectScroll : &sLedRootScroll;
  if (oledScrollHandleNav(scroll)) return true;

  // Gamepad LEFT/RIGHT nudges brightness ±5 while the brightness row is
  // selected. Gated off for the ANO encoder, where LEFT carries B-back
  // semantics (same rationale as the Settings editor's adjust block).
#if !ENABLE_ANO_ENCODER
  if (sLedLevel == LED_LEVEL_ROOT && sLedRootScroll.selectedIndex == 2 &&
      (gNavEvents.left || gNavEvents.right)) {
    // From the value the row shows; steps coalesce (only the newest is sent,
    // nothing is refused "Busy" while one runs).
    const int32_t shown = oledValueScrollShown(sLedBrightnessScroll, gSettings.ledBrightness);
    int32_t next = shown + (gNavEvents.right ? 5 : -5);
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    if (next != shown) oledValueScrollStep(sLedBrightnessScroll, "ledbrightness %ld", next);
    return true;
  }
#endif

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    switch (sLedLevel) {
      case LED_LEVEL_ROOT:    executeLedRootAction();   break;
      case LED_LEVEL_COLORS:  executeLedColorAction();  break;
      case LED_LEVEL_EFFECTS: executeLedEffectAction(); break;
    }
    return true;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (sLedLevel != LED_LEVEL_ROOT) {
      sLedLevel = LED_LEVEL_ROOT;  // sub-level → back to root, stay in mode
      return true;
    }
    return false;  // root level — let the global handler pop the mode stack
  }

  return false;
}

// Fresh visit resets to ROOT; back-navigation (B from a pushed mode) keeps
// whatever level the user was on. Mirrors the onEnter convention.
static void ledOnEnter(bool isForward) {
  if (isForward) sLedLevel = LED_LEVEL_ROOT;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc,
//          showInMenu, menuOrder, hints, onEnterFunc.
// No entry rule (availFunc nullptr): the LED screen is usable whenever it is
// built; getMenuAvailability() reports NOT_BUILT without ENABLE_NEOPIXEL.
static const OledModeEntry kLedModes[] = {
  { OLED_LED, "LED", "notify_sensor", displayLed, nullptr, ledInputHandler,
    false, -1, "A:Sel B:Back", ledOnEnter },
};

REGISTER_OLED_MODE_MODULE(kLedModes, sizeof(kLedModes) / sizeof(kLedModes[0]), "LED");

// Force linker to include this file - called from OLED_Utils.cpp
void oledLedModeInit() {}

#endif // ENABLE_OLED_DISPLAY && ENABLE_NEOPIXEL
