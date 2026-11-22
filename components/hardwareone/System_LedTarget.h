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
// attached seesaw encoder board) and `encoder:<p>` (one pixel, 1-based). With
// several encoder boards (multi-instance design, G5) board n >= 2 is
// `encoder<n>` and `encoder<n>:<p>`; board 1 is `encoder`, never `encoder1`.
// A first word that is a target is the target; anything else is the colour or
// effect, and the command goes to the default light. No colour or effect name
// equals a target name (the host test checks).
//
// Header-only and free of Arduino types: the host tests parse with this code.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "i2csensor_encoder_light.h"  // kEncoderMaxInstances

enum class LedTargetKind : uint8_t {
  Onboard,       // the board's own GPIO NeoPixel
  Encoder,       // every pixel of the encoder board
  EncoderPixel,  // one pixel of the encoder board (LedTarget::pixel, 1-based)
};

struct LedTarget {
  LedTargetKind kind = LedTargetKind::Onboard;
  uint8_t pixel = 0;     // 1-based, EncoderPixel only
  uint8_t instance = 1;  // the encoder board, 1-based (Encoder / EncoderPixel)
  bool named = false;    // the line named the target (false: the default)
};

// What the first word of the arguments is.
enum class LedTargetWord : uint8_t {
  NotATarget,   // a colour, an effect or a number: the default target applies
  Target,       // onboard, encoder[<n>] or encoder[<n>]:<p> with p 1..99
  BadPixel,     // encoder[<n>]:<anything that is not a pixel number 1..99>
  InstanceOne,  // encoder1 or encoder1:<p>: board 1 is `encoder`
  BadInstance,  // encoder<n> with n 0, past the maximum, or a leading zero
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
// `maxEncoders` is how many encoder boards a target may name.
inline LedTargetWord ledParseTargetWord(const char* word, size_t length, LedTarget& out,
                                        uint8_t maxEncoders = kEncoderMaxInstances) {
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
  // encoder[<n>][:<p>]: the board number runs to the end or to the colon.
  size_t at = kEncoderLength;
  const size_t instanceStart = at;
  while (at < length && word[at] >= '0' && word[at] <= '9') ++at;
  const size_t instanceDigits = at - instanceStart;
  if (at < length && word[at] != ':') return LedTargetWord::NotATarget;  // encoderx, encoder2x
  uint8_t instance = 1;
  if (instanceDigits > 0) {
    const char* digits = word + instanceStart;
    if (digits[0] == '0' || instanceDigits > 3) return LedTargetWord::BadInstance;
    unsigned value = 0;
    for (size_t i = 0; i < instanceDigits; ++i) value = value * 10 + (unsigned)(digits[i] - '0');
    if (value == 1) return LedTargetWord::InstanceOne;
    if (value > maxEncoders) return LedTargetWord::BadInstance;
    instance = (uint8_t)value;
  }
  if (at == length) {
    out.kind = LedTargetKind::Encoder;
    out.instance = instance;
    out.named = true;
    return LedTargetWord::Target;
  }
  const char* digits = word + at + 1;  // past the colon
  const size_t count = length - at - 1;
  if (count == 0 || count > 2) return LedTargetWord::BadPixel;
  unsigned value = 0;
  for (size_t i = 0; i < count; ++i) {
    if (digits[i] < '0' || digits[i] > '9') return LedTargetWord::BadPixel;
    value = value * 10 + (unsigned)(digits[i] - '0');
  }
  if (value < 1 || value > kLedTargetMaxPixelNumber) return LedTargetWord::BadPixel;
  out.kind = LedTargetKind::EncoderPixel;
  out.pixel = (uint8_t)value;
  out.instance = instance;
  out.named = true;
  return LedTargetWord::Target;
}

inline bool ledIsSpace(char c) { return c == ' ' || c == '\t'; }

// Split a target off the front of `args` (NUL-terminated). `rest` is the
// offset of the remaining arguments: past the target and its spaces, or 0
// when the first word is not a target and the default light applies. A
// refused word (BadPixel, InstanceOne, BadInstance) leaves the default target.
inline LedTargetWord ledSplitTarget(const char* args, bool hasOnboard, LedTarget& target, size_t& rest,
                                    uint8_t maxEncoders = kEncoderMaxInstances) {
  target = LedTarget();
  target.kind = ledDefaultTargetKind(hasOnboard);
  rest = 0;
  if (!args) return LedTargetWord::NotATarget;
  size_t start = 0;
  while (ledIsSpace(args[start])) ++start;
  size_t end = start;
  while (args[end] && !ledIsSpace(args[end])) ++end;
  LedTarget parsed;
  const LedTargetWord word = ledParseTargetWord(args + start, end - start, parsed, maxEncoders);
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

// The target word of encoder board n: "encoder" for board 1, "encoder2" for
// board 2 (what ledstatus names it and ledParseTargetWord reads back).
inline const char* ledEncoderTargetId(uint8_t n, char* buf, size_t size) {
  if (n <= 1) snprintf(buf, size, "%s", kLedTargetEncoderName);
  else snprintf(buf, size, "%s%u", kLedTargetEncoderName, (unsigned)n);
  return buf;
}
