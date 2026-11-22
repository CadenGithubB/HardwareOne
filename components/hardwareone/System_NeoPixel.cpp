/**
 * NeoPixel LED colors, effects, commands and persistent settings
 */

#include "System_NeoPixel.h"
#include "System_Utils.h"
#include "System_Command.h"
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_BuildConfig.h"  // For NEOPIXEL_PIN_DEFAULT
#include "System_CommandRegistry.h"
#include "System_LedTarget.h"
#include "i2csensor_encoder.h"   // the `encoder` target's pixels

#include <stdarg.h>

// Board power rail — asserted independently of ENABLE_NEOPIXEL because the
// NeoPixel power pin doubles as peripheral power on some boards (Feather V2
// GPIO2 also powers the STEMMA QT connector: compiling the LED out must not
// kill I2C). On boards where the pin only feeds the pixel (QT Py GPIO8) this
// harmlessly powers an unused rail. No-op when no power pin is defined.
// (FeatherS3's GPIO39/LDO2 is additionally driven by the I2C2 bus init and
// the OLED power path — multiple asserts of the same rail are fine.)
void boardPowerRailInit() {
#if defined(NEOPIXEL_POWER_PIN) && NEOPIXEL_POWER_PIN >= 0
  pinMode(NEOPIXEL_POWER_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_POWER_PIN, HIGH);
  delay(10);  // Allow power to stabilize
#endif
}

#if ENABLE_NEOPIXEL
#include <Adafruit_NeoPixel.h>

// Private driver instance (ENABLE_NEOPIXEL guarantees a real pin — the
// BuildConfig #error refuses pin -1, so no dummy branch exists anymore).
#define NUMPIXELS 1
static Adafruit_NeoPixel pixels(NUMPIXELS, NEOPIXEL_PIN_DEFAULT, NEO_GRB + NEO_KHZ800);
#endif

// ============================================================================
// 64-Color Palette (stored in PROGMEM to save RAM)
// ============================================================================
const ColorEntry colorTable[] PROGMEM = {
  // Primary colors
  { "red", { 255, 0, 0 } },
  { "green", { 0, 255, 0 } },
  { "blue", { 0, 0, 255 } },
  { "yellow", { 255, 255, 0 } },
  { "cyan", { 0, 255, 255 } },
  { "magenta", { 255, 0, 255 } },
  { "white", { 255, 255, 255 } },
  { "black", { 0, 0, 0 } },

  // Orange family
  { "orange", { 255, 165, 0 } },
  { "darkorange", { 255, 140, 0 } },
  { "orangered", { 255, 69, 0 } },
  { "coral", { 255, 127, 80 } },
  { "tomato", { 255, 99, 71 } },
  { "peach", { 255, 218, 185 } },

  // Red family
  { "darkred", { 139, 0, 0 } },
  { "crimson", { 220, 20, 60 } },
  { "firebrick", { 178, 34, 34 } },
  { "indianred", { 205, 92, 92 } },
  { "lightcoral", { 240, 128, 128 } },
  { "salmon", { 250, 128, 114 } },

  // Pink family
  { "pink", { 255, 192, 203 } },
  { "lightpink", { 255, 182, 193 } },
  { "hotpink", { 255, 105, 180 } },
  { "deeppink", { 255, 20, 147 } },
  { "palevioletred", { 219, 112, 147 } },
  { "mediumvioletred", { 199, 21, 133 } },

  // Purple family
  { "purple", { 128, 0, 128 } },
  { "darkviolet", { 148, 0, 211 } },
  { "blueviolet", { 138, 43, 226 } },
  { "mediumpurple", { 147, 112, 219 } },
  { "plum", { 221, 160, 221 } },
  { "orchid", { 218, 112, 214 } },

  // Blue family
  { "darkblue", { 0, 0, 139 } },
  { "navy", { 0, 0, 128 } },
  { "mediumblue", { 0, 0, 205 } },
  { "royalblue", { 65, 105, 225 } },
  { "steelblue", { 70, 130, 180 } },
  { "lightblue", { 173, 216, 230 } },
  { "skyblue", { 135, 206, 235 } },
  { "lightskyblue", { 135, 206, 250 } },
  { "deepskyblue", { 0, 191, 255 } },
  { "dodgerblue", { 30, 144, 255 } },
  { "cornflowerblue", { 100, 149, 237 } },
  { "cadetblue", { 95, 158, 160 } },

  // Green family
  { "darkgreen", { 0, 100, 0 } },
  { "forestgreen", { 34, 139, 34 } },
  { "seagreen", { 46, 139, 87 } },
  { "mediumseagreen", { 60, 179, 113 } },
  { "springgreen", { 0, 255, 127 } },
  { "limegreen", { 50, 205, 50 } },
  { "lime", { 0, 255, 0 } },
  { "lightgreen", { 144, 238, 144 } },
  { "palegreen", { 152, 251, 152 } },
  { "aquamarine", { 127, 255, 212 } },
  { "mediumaquamarine", { 102, 205, 170 } },

  // Yellow/Gold family
  { "gold", { 255, 215, 0 } },
  { "lightyellow", { 255, 255, 224 } },
  { "lemonchiffon", { 255, 250, 205 } },
  { "lightgoldenrodyellow", { 250, 250, 210 } },
  { "khaki", { 240, 230, 140 } },
  { "darkkhaki", { 189, 183, 107 } },

  // Brown family
  { "brown", { 165, 42, 42 } },
  { "saddlebrown", { 139, 69, 19 } },
  { "sienna", { 160, 82, 45 } },
  { "chocolate", { 210, 105, 30 } },
  { "peru", { 205, 133, 63 } },
  { "tan", { 210, 180, 140 } },
  { "burlywood", { 222, 184, 135 } },
  { "wheat", { 245, 222, 179 } },

  // Gray family
  { "gray", { 128, 128, 128 } },
  { "darkgray", { 169, 169, 169 } },
  { "lightgray", { 211, 211, 211 } },
  { "silver", { 192, 192, 192 } },
  { "dimgray", { 105, 105, 105 } },
  { "gainsboro", { 220, 220, 220 } }
};
const int numColors = sizeof(colorTable) / sizeof(colorTable[0]);

// ============================================================================
// LED Control Functions
// ============================================================================

#if ENABLE_NEOPIXEL
void initNeoPixelLed() {
  // Power rail is asserted by boardPowerRailInit() (called first at boot,
  // independent of this feature — see the rail comment above).
  pixels.begin();
  // Apply saved brightness (0-100% -> 0-255); fall back to 50% if settings not yet loaded
  int bPct = (gSettings.ledBrightness > 0) ? gSettings.ledBrightness : 50;
  if (bPct > 100) bPct = 100;
  pixels.setBrightness((uint8_t)(bPct * 255 / 100));
  pixels.show();  // Initialize all pixels to 'off'
}

void setLedColor(RGB color) {
  pixels.setPixelColor(0, pixels.Color(color.r, color.g, color.b));
  pixels.show();
}
#endif  // ENABLE_NEOPIXEL

bool getRgbFromName(const String& colorName, RGB& color) {
  String name = colorName;
  name.toLowerCase();
  
  // Handle special "off" alias
  if (name == "off") { color = {0, 0, 0}; return true; }
  
  // Search color table (PROGMEM)
  for (int i = 0; i < numColors; i++) {
    ColorEntry entry;
    memcpy_P(&entry, &colorTable[i], sizeof(ColorEntry));
    if (name.equalsIgnoreCase(entry.name)) {
      color = entry.rgb;
      return true;
    }
  }
  
  return false;  // Unknown color
}

RGB blendColors(RGB a, RGB b, float ratio) {
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  return {
    (uint8_t)(a.r + (b.r - a.r) * ratio),
    (uint8_t)(a.g + (b.g - a.g) * ratio),
    (uint8_t)(a.b + (b.b - a.b) * ratio)
  };
}

RGB adjustBrightness(RGB color, float brightness) {
  if (brightness < 0.0f) brightness = 0.0f;
  if (brightness > 1.0f) brightness = 1.0f;
  return {
    (uint8_t)(color.r * brightness),
    (uint8_t)(color.g * brightness),
    (uint8_t)(color.b * brightness)
  };
}

String getClosestColorName(uint16_t r, uint16_t g, uint16_t b, RGB& closestRgb) {
  // Find closest color in table using Euclidean distance
  int minDist = INT_MAX;
  String closestName = "unknown";
  
  for (int i = 0; i < numColors; i++) {
    ColorEntry entry;
    memcpy_P(&entry, &colorTable[i], sizeof(ColorEntry));
    
    int dr = (int)r - entry.rgb.r;
    int dg = (int)g - entry.rgb.g;
    int db = (int)b - entry.rgb.b;
    int dist = dr*dr + dg*dg + db*db;
    
    if (dist < minDist) {
      minDist = dist;
      closestName = entry.name;
      closestRgb = entry.rgb;
    }
  }
  
  return closestName;
}

// ============================================================================
// Shared effect-name table + brightness ladder (see System_NeoPixel.h)
// ============================================================================
// Order matches the EFFECT_* enum starting at EFFECT_FADE=1, so
// code = table index + 1. Keep in sync with the EffectType enum.
const char* const ledEffectNames[] = { "fade", "pulse", "rainbow", "blink", "strobe" };
const int ledEffectNameCount = (int)(sizeof(ledEffectNames) / sizeof(ledEffectNames[0]));

int ledEffectCodeForName(const String& name) {
  for (int i = 0; i < ledEffectNameCount; i++) {
    if (name.equalsIgnoreCase(ledEffectNames[i])) return i + 1;  // EFFECT_FADE=1
  }
  return EFFECT_NONE;
}

int ledBrightnessNextPreset(int cur) {
  static const int presets[] = { 10, 25, 50, 75, 100 };
  const int n = (int)(sizeof(presets) / sizeof(presets[0]));
  for (int i = 0; i < n; i++) {
    if (cur < presets[i]) return presets[i];
  }
  return presets[0];  // at/above top -> wrap
}

// <fade|pulse|blink|rainbow|strobe|off> [color] [color2] [duration 100..60000]
// The checks run in this order, so every command that uses the grammar refuses
// the same input with the same reason.
LedEffectArgsError parseLedEffectArgs(const String& argsInput, LedEffectArgs& out) {
  out = LedEffectArgs();
  CommandArgs a(argsInput);
  if (a.count() == 0) return LedEffectArgsError::Missing;

  out.effectName = a.arg(0);
  out.effectName.toLowerCase();
  if (out.effectName == "off" || out.effectName == "none") {
    if (a.count() > 1) return LedEffectArgsError::TooMany;
    out.off = true;
    return LedEffectArgsError::None;
  }

  // Optional arguments: up to two color names, then a duration. A token that
  // starts with a digit is the duration and has to be the last one.
  RGB* const colors[2] = { &out.color1, &out.color2 };
  int next = 1;
  for (int c = 0; c < 2 && next < a.count() && !isdigit(a.arg(next).charAt(0)); ++c, ++next) {
    if (!getRgbFromName(a.arg(next), *colors[c])) {
      out.badToken = a.arg(next);
      return LedEffectArgsError::UnknownColor;
    }
  }
  if (next < a.count()) {
    if (!parseIntArg(a.arg(next), out.durationMs)) return LedEffectArgsError::NotANumber;
    if (out.durationMs < 100 || out.durationMs > 60000) return LedEffectArgsError::DurationRange;
    ++next;
  }
  if (next < a.count()) return LedEffectArgsError::TooMany;

  // Parse via the shared name table (same list the OLED and G2 pickers render,
  // so the surfaces can't drift from the parser).
  out.effect = ledEffectCodeForName(out.effectName);
  if (out.effect == EFFECT_NONE) {
    out.badToken = out.effectName;
    return LedEffectArgsError::UnknownEffect;
  }
  return LedEffectArgsError::None;
}

// ============================================================================
// Non-blocking effect engine
// ============================================================================
// Every effect is a pure function of elapsed time, so instead of the old
// delay()-loop (which parked cmd_exec — and therefore the OLED UI, which waits
// on it synchronously — for the whole duration), the engine keeps a tiny state
// struct and advances one frame per ledEffectTick() call from the main loop.
// cmd_ledeffect now starts an effect and returns immediately; `ledeffect off`,
// ledcolor, and ledclear genuinely cancel a running effect (previously
// impossible — cmd_exec was busy running it).

#if ENABLE_NEOPIXEL

static struct {
  bool          active;
  int           type;
  RGB           c1, c2;
  unsigned long startMs;
  unsigned long durationMs;
  unsigned long lastFrameMs;
} gLedFx = {};

// Frame color at `elapsed` ms: ledEffectFrame() in System_LedEffectCore.h.

void ledEffectStart(int effectType, RGB startColor, RGB endColor, unsigned long duration) {
  gLedFx.type        = effectType;
  gLedFx.c1          = startColor;
  gLedFx.c2          = endColor;
  gLedFx.durationMs  = duration;
  gLedFx.startMs     = millis();
  gLedFx.lastFrameMs = 0;
  gLedFx.active      = true;
}

void ledEffectStop(bool clearLed) {
  if (!gLedFx.active) return;
  gLedFx.active = false;
  if (clearLed) setLedColor({0, 0, 0});  // matches the old end-of-effect clear
}

bool ledEffectActive() { return gLedFx.active; }

// Main-loop tick. Cheap no-op while idle; ~20 ms frame pacing while active
// (the fastest cadence the old loops used — strobe/rainbow).
void ledEffectTick() {
  if (!gLedFx.active) return;
  const unsigned long now = millis();
  const unsigned long elapsed = now - gLedFx.startMs;
  if (elapsed >= gLedFx.durationMs) {
    ledEffectStop(true);
    return;
  }
  if (now - gLedFx.lastFrameMs < 20) return;
  gLedFx.lastFrameMs = now;
  setLedColor(ledEffectFrame(gLedFx.type, gLedFx.c1, gLedFx.c2, elapsed, gLedFx.durationMs));
}

// Blocking wrapper over the same engine — kept for the boot-time startup
// effect (HardwareOne.cpp), which deliberately runs before the main loop is
// ticking and where blocking a few seconds of setup() is the intended UX.
// Runtime paths (cmd_ledeffect) use ledEffectStart instead.
void runLedEffect(int effectType, RGB startColor, RGB endColor, unsigned long duration) {
  ledEffectStart(effectType, startColor, endColor, duration);
  while (gLedFx.active) {
    ledEffectTick();
    delay(10);
  }
}

#endif  // ENABLE_NEOPIXEL (effect engine)

// ============================================================================
// LED commands: one set for every light, the light named by a target
// ============================================================================
// ledcolor [<target>] <colour>, ledeffect [<target>] <effect> ...,
// ledbrightness [<target>] <0..100>, ledclear [<target>] and ledstatus
// (System_LedTarget.h). `onboard` is this board's NeoPixel and runs the
// engine above exactly as before; `encoder` and `encoder:<n>` reach the
// encoder board's pixels (i2csensor_encoder.h). An omitted target is the
// onboard pixel, or the encoder on a board without one, so every existing
// line (automations, MQTT, voice, the web, the app, the OLED and G2 pages,
// the startup effect) keeps driving the light it drove.

#if ENABLE_LED_LIGHTS

namespace {

constexpr bool kHasOnboard = ENABLE_NEOPIXEL;

#if ENABLE_NEOPIXEL
// What the onboard pixel shows, for ledstatus. The commands below keep it;
// an effect leaves the pixel dark when it ends, as it always has.
char sOnboardColour[24] = "off";
RGB sOnboardRgb = {0, 0, 0};
char sOnboardEffect[12] = "off";

void onboardShows(const char* colour, RGB rgb) {
  strlcpy(sOnboardColour, colour, sizeof(sOnboardColour));
  sOnboardRgb = rgb;
}
#endif

// Split the target off the arguments. Returns nullptr and the remaining
// arguments, or the reply that refuses the line: a malformed encoder:<n>, or a
// light this build or board does not have.
const char* takeLedTarget(const String& argsInput, LedTarget& target, String& rest) {
  size_t restOffset = 0;
  const LedTargetWord word = ledSplitTarget(argsInput.c_str(), kHasOnboard, target, restOffset);
  if (word == LedTargetWord::BadPixel) {
    return "Error: encoder:<n> takes a pixel number, for example encoder:1. Run 'ledstatus' to "
           "see the encoder board's pixels.";
  }
  rest = argsInput.substring(restOffset);
  rest.trim();
  if (target.kind == LedTargetKind::Onboard && !kHasOnboard) {
    return "Error: This board has no onboard LED. Name the encoder board instead, for example "
           "'ledcolor encoder red'.";
  }
#if !ENABLE_ENCODER
  if (target.kind != LedTargetKind::Onboard) return kEncoderReplyNotBuilt;
#endif
  return nullptr;
}

// The encoder pixel a target reaches: 0 for every pixel.
uint8_t encoderPixelOf(const LedTarget& target) {
  return target.kind == LedTargetKind::EncoderPixel ? target.pixel : 0;
}

const char* encoderDoneReply(const LedTarget& target, const char* what, const char* value) {
  if (!ensureDebugBuffer()) return "Encoder LED set";
  if (target.kind == LedTargetKind::EncoderPixel) {
    snprintf(getDebugBuffer(), 1024, "Encoder pixel %u %s %s", (unsigned)target.pixel, what, value);
  } else {
    snprintf(getDebugBuffer(), 1024, "Encoder LED %s %s", what, value);
  }
  return getDebugBuffer();
}

const char* cmd_ledcolor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  LedTarget target;
  String colorName;
  if (const char* refused = takeLedTarget(argsInput, target, colorName)) return refused;
  colorName.toLowerCase();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (colorName.length() == 0) {
    return "Error: invalid arguments — Usage: ledcolor [onboard|encoder|encoder:<n>] <red|green|blue|yellow|magenta|cyan|white|orange|purple|pink>";
  }

  RGB color;
  if (!getRgbFromName(colorName, color)) {
    snprintf(getDebugBuffer(), 1024, "Error: Unknown color: %.40s", colorName.c_str());
    return getDebugBuffer();
  }

  if (target.kind != LedTargetKind::Onboard) {
    if (const char* refused = encoderLightSetColour(encoderPixelOf(target), color, colorName.c_str())) {
      return refused;
    }
    return encoderDoneReply(target, "set to", colorName.c_str());
  }
#if ENABLE_NEOPIXEL
  // Cancel any running effect first, or the next tick would repaint over the
  // user's chosen color within 20 ms.
  ledEffectStop(false);
  setLedColor(color);
  onboardShows(colorName.c_str(), color);
  snprintf(getDebugBuffer(), 1024, "LED set to %s", colorName.c_str());
  return getDebugBuffer();
#else
  return "Error: This board has no onboard LED.";
#endif
}

const char* cmd_ledclear(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  LedTarget target;
  String rest;
  if (const char* refused = takeLedTarget(argsInput, target, rest)) return refused;
  if (rest.length() > 0) return "Error: Too many arguments. Usage: ledclear [onboard|encoder|encoder:<n>]";
  if (target.kind != LedTargetKind::Onboard) {
    if (const char* refused = encoderLightSetColour(encoderPixelOf(target), {0, 0, 0}, "off")) return refused;
    return target.kind == LedTargetKind::EncoderPixel ? encoderDoneReply(target, "cleared", "(turned off)")
                                                      : "Encoder LED cleared (turned off)";
  }
#if ENABLE_NEOPIXEL
  ledEffectStop(false);  // cancel a running effect too, not just the color
  setLedColor({0, 0, 0});
  onboardShows("off", {0, 0, 0});
  return "LED cleared (turned off)";
#else
  return "Error: This board has no onboard LED.";
#endif
}

const char* cmd_ledeffect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  static const char* const kTooManyArgs =
      "Error: Too many arguments. Usage: ledeffect [onboard|encoder|encoder:<n>] <fade|pulse|blink|rainbow|strobe|off> [color] [color2] [duration 100..60000]";

  LedTarget target;
  String rest;
  if (const char* refused = takeLedTarget(argsInput, target, rest)) return refused;

  LedEffectArgs fx;
  switch (parseLedEffectArgs(rest, fx)) {
    case LedEffectArgsError::None: break;
    case LedEffectArgsError::Missing:
      return "Usage: ledeffect [onboard|encoder|encoder:<n>] <fade|pulse|blink|rainbow|strobe|off> [color] [color2] [duration 100..60000]";
    case LedEffectArgsError::TooMany: return kTooManyArgs;
    case LedEffectArgsError::UnknownColor:
      if (!ensureDebugBuffer()) return "Error: Unknown color";
      snprintf(getDebugBuffer(), 1024, "Error: Unknown color '%.40s'", fx.badToken.c_str());
      return getDebugBuffer();
    case LedEffectArgsError::NotANumber: return kCommandReplyNotANumber;
    case LedEffectArgsError::DurationRange: return "Error: LED effect duration must be 100..60000 ms";
    case LedEffectArgsError::UnknownEffect:
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024, "Error: Unknown effect: %.20s. Options: fade, pulse, blink, rainbow, strobe, off", fx.badToken.c_str());
      return getDebugBuffer();
  }

  if (target.kind != LedTargetKind::Onboard) {
    // The encoder's pixels show their colour again when an effect ends;
    // `off` stops it and darkens them, as on the onboard pixel.
    const uint8_t pixel = encoderPixelOf(target);
    if (fx.off) {
      if (const char* refused = encoderLightSetColour(pixel, {0, 0, 0}, "off")) return refused;
      return encoderDoneReply(target, "effect:", "off");
    }
    if (const char* refused = encoderLightStartEffect(pixel, fx.effect, fx.color1, fx.color2,
                                                      fx.durationMs, fx.effectName.c_str())) {
      return refused;
    }
    if (!ensureDebugBuffer()) return "Encoder effect started";
    snprintf(getDebugBuffer(), 1024, "%s effect started on the encoder%s (%lums), then its colour",
             fx.effectName.c_str(), target.kind == LedTargetKind::EncoderPixel ? " pixel" : "",
             fx.durationMs);
    return getDebugBuffer();
  }

#if ENABLE_NEOPIXEL
  if (fx.off) {
    // Genuinely cancels now — with the old blocking engine this branch could
    // never run mid-effect because cmd_exec was busy inside the delay loop.
    ledEffectStop(false);
    setLedColor({0, 0, 0});
    onboardShows("off", {0, 0, 0});
    strlcpy(sOnboardEffect, "off", sizeof(sOnboardEffect));
    return "LED effect: off";
  }

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  // Non-blocking: the main-loop ledEffectTick() drives the frames. The command
  // (and any UI waiting on it) returns immediately instead of parking cmd_exec
  // for the whole duration. Starting a new effect replaces a running one.
  ledEffectStart(fx.effect, fx.color1, fx.color2, fx.durationMs);
  onboardShows("off", {0, 0, 0});  // what the pixel shows when the effect ends
  strlcpy(sOnboardEffect, fx.effectName.c_str(), sizeof(sOnboardEffect));
  snprintf(getDebugBuffer(), 1024, "%s effect started (%lums)", fx.effectName.c_str(), fx.durationMs);
  return getDebugBuffer();
#else
  return "Error: This board has no onboard LED.";
#endif
}

// Saved per light: ledBrightness for the onboard pixel, each encoder
// instance's own brightness for its board (both apply at once). The `encoder`
// target is instance 1.
const char* cmd_ledbrightness(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  LedTarget target;
  String arg;
  if (const char* refused = takeLedTarget(argsInput, target, arg)) return refused;
  if (target.kind == LedTargetKind::EncoderPixel) {
    return "Error: Brightness is one setting for the whole encoder board. Usage: ledbrightness encoder <0..100>";
  }
  if (arg.length() == 0) return "Error: invalid arguments — Usage: ledbrightness [onboard|encoder] <0..100>";
  int v = 0;
  if (!parseIntArg(arg, v)) return kCommandReplyNotANumber;
  if (v < 0 || v > 100) return "Error: LED brightness must be 0..100";
  if (target.kind == LedTargetKind::Encoder) {
    encoderLightSetBrightness(v, 1);
    snprintf(getDebugBuffer(), 1024, "Encoder LED brightness set to %d%%", v);
    return getDebugBuffer();
  }
#if ENABLE_NEOPIXEL
  setSetting(gSettings.ledBrightness, v);
  // Apply immediately to hardware (0-100% -> 0-255)
  pixels.setBrightness((uint8_t)(v * 255 / 100));
  pixels.show();
  snprintf(getDebugBuffer(), 1024, "LED brightness set to %d%%", v);
  return getDebugBuffer();
#else
  return "Error: This board has no onboard LED.";
#endif
}

// Append to the reply; false once it is full.
bool statusAppend(char* buf, size_t size, size_t& pos, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
bool statusAppend(char* buf, size_t size, size_t& pos, const char* fmt, ...) {
  if (pos >= size) return false;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf + pos, size - pos, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= size - pos) {
    pos = size;
    return false;
  }
  pos += (size_t)n;
  return true;
}

// Every light this build has, connected or not, and what it shows.
const char* cmd_ledstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  char* buf = getDebugBuffer();
  const size_t size = 1024;
  size_t pos = 0;
  const bool json = argWantsJson(argsInput);
  const char* defaultTarget = kHasOnboard ? kLedTargetOnboardName : kLedTargetEncoderName;

  if (json) {
    statusAppend(buf, size, pos, "{\"schema\":1,\"default\":\"%s\",\"lights\":[", defaultTarget);
  } else {
    statusAppend(buf, size, pos, "LED lights (default target: %s)", defaultTarget);
  }

#if ENABLE_NEOPIXEL
  {
    const char* effect = ledEffectActive() ? sOnboardEffect : "off";
    if (json) {
      statusAppend(buf, size, pos,
                   "{\"target\":\"onboard\",\"present\":true,\"connected\":true,\"color\":\"%s\","
                   "\"rgb\":[%u,%u,%u],\"brightness\":%d,\"effect\":\"%s\"}",
                   sOnboardColour, sOnboardRgb.r, sOnboardRgb.g, sOnboardRgb.b,
                   gSettings.ledBrightness, effect);
    } else {
      statusAppend(buf, size, pos, "\n  onboard: %s, brightness %d%%, effect %s", sOnboardColour,
                   gSettings.ledBrightness, effect);
    }
  }
#else
  if (json) {
    statusAppend(buf, size, pos, "{\"target\":\"onboard\",\"present\":false}");
  } else {
    statusAppend(buf, size, pos, "\n  onboard: not present on this board");
  }
#endif

#if ENABLE_ENCODER
  {
    EncoderLightStatus enc;
    if (!encoderLightReadStatus(enc)) return "Error: The encoder light state is busy. Retry.";
    if (json) {
      statusAppend(buf, size, pos,
                   ",{\"target\":\"encoder\",\"present\":true,\"connected\":%s,\"product\":%u,"
                   "\"board\":\"%s\",\"bus\":%u,\"address\":%u,\"brightness\":%u,\"pixels\":[",
                   enc.connected ? "true" : "false", (unsigned)enc.product,
                   enc.boardName ? enc.boardName : "", (unsigned)enc.bus, (unsigned)enc.address,
                   (unsigned)enc.brightness);
      for (uint8_t i = 0; i < enc.pixelCount; ++i) {
        const EncoderLightPixelStatus& px = enc.pixels[i];
        statusAppend(buf, size, pos,
                     "%s{\"pixel\":%u,\"color\":\"%s\",\"rgb\":[%u,%u,%u],\"effect\":\"%s\"}",
                     i ? "," : "", (unsigned)(i + 1), px.colour, px.rgb.r, px.rgb.g, px.rgb.b,
                     px.effect);
      }
      statusAppend(buf, size, pos, "]}");
    } else {
      if (enc.boardName) {
        statusAppend(buf, size, pos, "\n  encoder: %s (%u) at I2C%u 0x%02X, %s, brightness %u%%",
                     enc.boardName, (unsigned)enc.product, (unsigned)enc.bus + 1,
                     (unsigned)enc.address, enc.connected ? "connected" : "not connected",
                     (unsigned)enc.brightness);
      } else {
        statusAppend(buf, size, pos,
                     "\n  encoder: not connected yet (I2C%u 0x%02X), brightness %u%%. "
                     "Open it with 'openencoder'.",
                     (unsigned)enc.bus + 1, (unsigned)enc.address, (unsigned)enc.brightness);
      }
      for (uint8_t i = 0; i < enc.pixelCount; ++i) {
        statusAppend(buf, size, pos, "\n    encoder:%u %s, effect %s", (unsigned)(i + 1),
                     enc.pixels[i].colour, enc.pixels[i].effect);
      }
    }
  }
#endif

  if (json) statusAppend(buf, size, pos, "]}");
  if (pos >= size) return "Error: The LED status outgrew the reply buffer.";
  return buf;
}

}  // namespace

// ============================================================================
// LED Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry neopixelCommands[] = {
  { "ledcolor", "Set an LED colour: [onboard|encoder|encoder:<n>] <colour>.", false, cmd_ledcolor, "Usage: ledcolor [onboard|encoder|encoder:<n>] <red|green|blue|yellow|magenta|cyan|white|orange|purple|pink>" },
  { "ledclear", "Turn an LED off: [onboard|encoder|encoder:<n>].", false, cmd_ledclear, "Usage: ledclear [onboard|encoder|encoder:<n>]" },
  { "ledeffect", "Run an LED effect: [onboard|encoder|encoder:<n>] <effect>.", false, cmd_ledeffect, "Usage: ledeffect [onboard|encoder|encoder:<n>] <fade|pulse|blink|rainbow|strobe|off> [color] [color2] [duration 100..60000]" },
  { "ledbrightness", "Save an LED brightness 0-100: [onboard|encoder] <0..100>.", true, cmd_ledbrightness, "Usage: ledbrightness [onboard|encoder] <0..100>" },
  { "ledstatus", "Show every LED this build has and what it shows. (add 'json' for JSON output)", false, cmd_ledstatus, "Usage: ledstatus [json]" },
};

const size_t neopixelCommandsCount = sizeof(neopixelCommands) / sizeof(neopixelCommands[0]);

#endif  // ENABLE_LED_LIGHTS

// Command grouping is owned by System_CommandRegistry.cpp.
// Keep the live controls and the onboard startup settings as separate help groups.

#if ENABLE_NEOPIXEL

static const char* cmd_hardwareled_startupenabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Error: invalid arguments — Usage: ledstartupenabled <0|1>";
  const int parsed = parseBoolArg(arg);
  if (parsed < 0) return kCommandReplyNotOnOff;
  const bool enabled = (parsed == 1);
  setSetting(gSettings.ledStartupEnabled, enabled);
  snprintf(getDebugBuffer(), 1024, "LED startup effect %s", enabled ? "enabled" : "disabled");
  return getDebugBuffer();
}

static const char* cmd_hardwareled_startupeffect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  if (arg.length() == 0) return "Error: invalid arguments — Usage: ledstartupeffect <none|rainbow|pulse|fade|blink|strobe>";
  if (arg == "none"   || arg == "rainbow" || arg == "pulse" ||
      arg == "fade"   || arg == "blink"   || arg == "strobe") {
    setSetting(gSettings.ledStartupEffect, arg.c_str());
  } else {
    return "Error: Unknown effect. Valid: none, rainbow, pulse, fade, blink, strobe";
  }
  snprintf(getDebugBuffer(), 1024, "LED startup effect set to %s", gSettings.ledStartupEffect.c_str());
  return getDebugBuffer();
}

static const char* cmd_hardwareled_startupcolor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Error: invalid arguments — Usage: ledstartupcolor <color name|off>  (any of ~80 named colors)";
  RGB unused;
  if (!getRgbFromName(arg, unused)) {
    snprintf(getDebugBuffer(), 1024,
             "Error: Unknown color '%.40s'. Use a named color such as red, cyan, orange, gold or off.",
             arg.c_str());
    return getDebugBuffer();
  }
  setSetting(gSettings.ledStartupColor, arg.c_str());
  snprintf(getDebugBuffer(), 1024, "LED startup color set to %s", gSettings.ledStartupColor.c_str());
  return getDebugBuffer();
}

static const char* cmd_hardwareled_startupcolor2(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Error: invalid arguments — Usage: ledstartupcolor2 <color name|off>  (any of ~80 named colors)";
  RGB unused;
  if (!getRgbFromName(arg, unused)) {
    snprintf(getDebugBuffer(), 1024,
             "Error: Unknown color '%.40s'. Use a named color such as red, cyan, orange, gold or off.",
             arg.c_str());
    return getDebugBuffer();
  }
  setSetting(gSettings.ledStartupColor2, arg.c_str());
  snprintf(getDebugBuffer(), 1024, "LED startup color 2 set to %s", gSettings.ledStartupColor2.c_str());
  return getDebugBuffer();
}

static const char* cmd_hardwareled_startupduration(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) return "Error: invalid arguments — Usage: ledstartupduration <100..10000>";
  int v = 0;
  if (!parseIntArg(arg, v)) return kCommandReplyNotANumber;
  if (v < 100 || v > 10000) return "Error: LED duration must be 100..10000 ms";
  setSetting(gSettings.ledStartupDuration, v);
  snprintf(getDebugBuffer(), 1024, "LED startup duration set to %dms", v);
  return getDebugBuffer();
}

// ============================================================================
// Persistent LED Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
extern const CommandEntry ledCommands[] = {
  { "ledstartupenabled",    "Enable/disable LED startup effect [0|1].",        true, cmd_hardwareled_startupenabled,"Usage: ledstartupenabled <0|1>" },
  { "ledstartupeffect",     "Set LED startup effect [none|rainbow|pulse|fade|blink|strobe].",   true, cmd_hardwareled_startupeffect, "Usage: ledstartupeffect <none|rainbow|pulse|fade|blink|strobe>" },
  { "ledstartupcolor",      "Set LED startup primary color (any of ~80 named colors or 'off'; an unknown name is refused).",                  true, cmd_hardwareled_startupcolor,  "Usage: ledstartupcolor <color name|off>  (any of ~80 named colors)" },
  { "ledstartupcolor2",     "Set LED startup secondary color (any of ~80 named colors or 'off'; an unknown name is refused).",                true, cmd_hardwareled_startupcolor2, "Usage: ledstartupcolor2 <color name|off>  (any of ~80 named colors)" },
  { "ledstartupduration",   "Set LED startup effect duration in ms.",         true, cmd_hardwareled_startupduration,"Usage: ledstartupduration <100..10000>" },
};

extern const size_t ledCommandsCount = sizeof(ledCommands) / sizeof(ledCommands[0]);


// ============================================================================
// LED Settings Module
// ============================================================================
// (Still inside #if ENABLE_NEOPIXEL — the schema goes with the hardware.)

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry ledSettingEntries[] = {
  { "ledBrightness", SETTING_INT, &gSettings.ledBrightness, 100, 0, nullptr, 0, 100, "Brightness", nullptr, false, nullptr, "ledbrightness" },
  { "ledStartupEnabled", SETTING_BOOL, &gSettings.ledStartupEnabled, true, 0, nullptr, 0, 1, "Startup Enabled", nullptr, false, nullptr, "ledstartupenabled" },
  { "ledStartupEffect", SETTING_STRING, &gSettings.ledStartupEffect, 0, 0, "rainbow", 0, 0, "Startup Effect", "none,rainbow,pulse,fade,blink,strobe", false, nullptr, "ledstartupeffect" },
  { "ledStartupColor", SETTING_STRING, &gSettings.ledStartupColor, 0, 0, "cyan", 0, 0, "Startup Color", nullptr, false, nullptr, "ledstartupcolor" },
  { "ledStartupColor2", SETTING_STRING, &gSettings.ledStartupColor2, 0, 0, "magenta", 0, 0, "Startup Color 2", nullptr, false, nullptr, "ledstartupcolor2" },
  { "ledStartupDuration", SETTING_INT, &gSettings.ledStartupDuration, 1000, 0, nullptr, 100, 10000, "Startup Duration (ms)", nullptr, false, nullptr, "ledstartupduration" }
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule ledSettingsModule = {
  "led", "hardware.led", ledSettingEntries,
  sizeof(ledSettingEntries) / sizeof(ledSettingEntries[0]),
  nullptr,
  "LED brightness and startup effects"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

#endif  // ENABLE_NEOPIXEL (startup commands + settings module)
