#pragma once

// The target argument of the LED commands (owner decision 2026-11-21). Every
// light takes the same commands, and the light is named by an optional first
// word:
//
//   ledcolor [<target>] <colour>
//   ledeffect [<target>] <effect> [colour] [colour2] [duration]
//   ledbrightness [<target>] <0..100>
//   ledclear [<target>]
//
// Targets: `onboard` (the board's own NeoPixel), `encoder` (every pixel of the
// attached seesaw encoder board) and `encoder:<n>` (one pixel, 1-based). A
// first word that is a target name or `encoder:<n>` is the target; anything
// else is the colour or effect, and the command goes to the default light.
// No colour or effect name equals a target name (the host test checks).
//
// Header-only and free of Arduino types: the host tests parse with this code.

#include <stddef.h>
#include <stdint.h>

enum class LedTargetKind : uint8_t {
  Onboard,       // the board's own GPIO NeoPixel
  Encoder,       // every pixel of the encoder board
  EncoderPixel,  // one pixel of the encoder board (LedTarget::pixel, 1-based)
};

struct LedTarget {
  LedTargetKind kind = LedTargetKind::Onboard;
  uint8_t pixel = 0;   // 1-based, EncoderPixel only
  bool named = false;  // the line named the target (false: the default)
};

// What the first word of the arguments is.
enum class LedTargetWord : uint8_t {
  NotATarget,  // a colour, an effect or a number: the default target applies
  Target,      // onboard, encoder or encoder:<n> with n 1..99
  BadPixel,    // encoder:<anything that is not a pixel number 1..99>
};

constexpr const char kLedTargetOnboardName[] = "onboard";
constexpr const char kLedTargetEncoderName[] = "encoder";
constexpr uint8_t kLedTargetMaxPixelNumber = 99;

// The light an omitted target means: the board's own NeoPixel when it has one,
// otherwise the encoder board (the only light such a build can have).
constexpr LedTargetKind ledDefaultTargetKind(bool hasOnboard) {
  return hasOnboard ? LedTargetKind::Onboard : LedTargetKind::Encoder;
}

inline bool ledWordEquals(const char* word, size_t length, const char* name) {
  size_t i = 0;
  for (; i < length && name[i]; ++i) {
    char c = word[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != name[i]) return false;
  }
  return i == length && name[i] == '\0';
}

// Read one word (not NUL-terminated, `length` bytes) as a target.
inline LedTargetWord ledParseTargetWord(const char* word, size_t length, LedTarget& out) {
  out = LedTarget();
  if (!word || length == 0) return LedTargetWord::NotATarget;
  if (ledWordEquals(word, length, kLedTargetOnboardName)) {
    out.kind = LedTargetKind::Onboard;
    out.named = true;
    return LedTargetWord::Target;
  }
  constexpr size_t kEncoderLength = sizeof(kLedTargetEncoderName) - 1;
  if (length < kEncoderLength || !ledWordEquals(word, kEncoderLength, kLedTargetEncoderName)) {
    return LedTargetWord::NotATarget;
  }
  if (length == kEncoderLength) {
    out.kind = LedTargetKind::Encoder;
    out.named = true;
    return LedTargetWord::Target;
  }
  if (word[kEncoderLength] != ':') return LedTargetWord::NotATarget;  // "encoderx": not a target word
  const char* digits = word + kEncoderLength + 1;
  const size_t count = length - kEncoderLength - 1;
  if (count == 0 || count > 2) return LedTargetWord::BadPixel;
  unsigned value = 0;
  for (size_t i = 0; i < count; ++i) {
    if (digits[i] < '0' || digits[i] > '9') return LedTargetWord::BadPixel;
    value = value * 10 + (unsigned)(digits[i] - '0');
  }
  if (value < 1 || value > kLedTargetMaxPixelNumber) return LedTargetWord::BadPixel;
  out.kind = LedTargetKind::EncoderPixel;
  out.pixel = (uint8_t)value;
  out.named = true;
  return LedTargetWord::Target;
}

inline bool ledIsSpace(char c) { return c == ' ' || c == '\t'; }

// Split a target off the front of `args` (NUL-terminated). `rest` is the
// offset of the remaining arguments: past the target and its spaces, or 0
// when the first word is not a target and the default light applies.
inline LedTargetWord ledSplitTarget(const char* args, bool hasOnboard, LedTarget& target, size_t& rest) {
  target = LedTarget();
  target.kind = ledDefaultTargetKind(hasOnboard);
  rest = 0;
  if (!args) return LedTargetWord::NotATarget;
  size_t start = 0;
  while (ledIsSpace(args[start])) ++start;
  size_t end = start;
  while (args[end] && !ledIsSpace(args[end])) ++end;
  LedTarget parsed;
  const LedTargetWord word = ledParseTargetWord(args + start, end - start, parsed);
  if (word == LedTargetWord::NotATarget) return word;
  if (word == LedTargetWord::Target) target = parsed;
  while (ledIsSpace(args[end])) ++end;
  rest = end;
  return word;
}

// Whether a one-pixel target names a pixel the connected board has.
constexpr bool ledTargetPixelFits(const LedTarget& target, uint8_t pixelCount) {
  return target.kind != LedTargetKind::EncoderPixel ||
         (target.pixel >= 1 && target.pixel <= pixelCount);
}
