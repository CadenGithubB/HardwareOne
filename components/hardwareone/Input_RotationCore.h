#pragma once

// Input directions follow the screen (owner, 2026-11-23): with the OLED turned
// by `oledrotation`, "up" on the joystick, the ANO ring and the IMU tilt is
// always up on the screen as it is seen. The input devices are fixed to the
// board, as the panel is, so a physical direction becomes a screen direction
// by the same quarter turns the display applies. Pure (no Arduino types), so
// the host test runs the shipping code.
//
// GFX rotation r maps a logical pixel (x, y) onto the panel turned r quarter
// turns clockwise: at r = 1 the screen's up is the panel's right edge. So a
// physical direction d (clockwise order up, right, down, left) shows as the
// screen direction (d - r) mod 4.

#include <stdint.h>

namespace input_rotation {

enum Dir : uint8_t { kUp = 0, kRight = 1, kDown = 2, kLeft = 3 };

// The screen direction a physical direction shows as at `quarterTurns`.
constexpr Dir screenDir(Dir physical, int quarterTurns) {
  return static_cast<Dir>((static_cast<int>(physical) - (quarterTurns & 3) + 4) & 3);
}

// A joystick reading (raw values around `center`, x grows to the right, y
// grows UP as the Seesaw and the IMU input report it) as the screen sees it.
inline void rotateStick(int quarterTurns, int center, int& x, int& y) {
  const int rx = x - center;  // physical right
  const int ry = y - center;  // physical up
  switch (quarterTurns & 3) {
    case 1:  // screen up = physical right, screen right = physical down
      x = center - ry;
      y = center + rx;
      break;
    case 2:
      x = center - rx;
      y = center - ry;
      break;
    case 3:  // screen up = physical left, screen right = physical up
      x = center + ry;
      y = center - rx;
      break;
    default:
      break;
  }
}

// Direction button bits (one bit each for up, right, down, left) as the
// screen sees them: each set physical bit moves to its screen direction's bit.
// Other bits pass through unchanged.
inline uint32_t rotateDirectionBits(uint32_t bits, int quarterTurns, uint32_t upBit, uint32_t rightBit,
                                    uint32_t downBit, uint32_t leftBit) {
  if ((quarterTurns & 3) == 0) return bits;
  const uint32_t dirBits[4] = {upBit, rightBit, downBit, leftBit};
  uint32_t out = bits & ~(upBit | rightBit | downBit | leftBit);
  for (int d = 0; d < 4; ++d) {
    if (bits & dirBits[d]) out |= dirBits[screenDir(static_cast<Dir>(d), quarterTurns)];
  }
  return out;
}

// ---- the device's mount (`imuinputmount`, `anomount`) -----------------------
// How an input device sits relative to the panel at rotation 0, in the
// screen's terms: turned 0, 90, 180 or 270 degrees the way `oledrotation`
// turns the screen, and optionally mirrored (left and right exchanged in the
// device's own frame, applied before the turn). The 4 turns x mirror are the
// 8 ways a device can sit. The mount correction comes first ("as if mounted
// straight"), the screen rotation after it, so turning the screen never undoes
// a mount fix (owner, 2026-11-23).
//
// A saved mount is one int: the degrees, plus 1 when mirrored (0, 1, 90, 91,
// 180, 181, 270, 271), so a settings pick-list can offer all eight.
struct Mount {
  int quarterTurns;
  bool mirror;
};

constexpr bool mountCodeValid(int code) {
  return code == 0 || code == 1 || code == 90 || code == 91 || code == 180 || code == 181 || code == 270 ||
         code == 271;
}
constexpr int mountCode(int degrees, bool mirror) { return degrees + (mirror ? 1 : 0); }
constexpr Mount mountFromCode(int code) {
  return mountCodeValid(code) ? Mount{(code / 90) & 3, (code & 1) != 0} : Mount{0, false};
}
constexpr int mountDegrees(int code) { return mountFromCode(code).quarterTurns * 90; }

// A mount argument: "<0|90|180|270> [mirror]", or a saved code (1, 91, 181,
// 271: what the settings pick-list sends). Spaces around it are fine.
// False for anything else; code is then untouched.
inline bool parseMountArg(const char* text, int& code) {
  if (!text) return false;
  while (*text == ' ' || *text == '\t') ++text;
  int value = 0, digits = 0;
  while (*text >= '0' && *text <= '9' && digits < 4) {
    value = value * 10 + (*text - '0');
    ++text;
    ++digits;
  }
  if (digits == 0 || (*text >= '0' && *text <= '9')) return false;
  bool mirror = false;
  while (*text == ' ' || *text == '\t') ++text;
  if (*text) {
    const char* word = "mirror";
    int i = 0;
    for (; word[i]; ++i) {
      char c = text[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != word[i]) return false;
    }
    text += i;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text) return false;
    mirror = true;
  }
  if (mirror) {
    if (!mountCodeValid(value) || (value & 1)) return false;
    code = mountCode(value, true);
    return true;
  }
  if (!mountCodeValid(value)) return false;
  code = value;
  return true;
}

// The mount as a reply shows it: "90" or "90 mirror".
inline const char* mountName(int code) {
  switch (code) {
    case 1: return "0 mirror";
    case 90: return "90";
    case 91: return "90 mirror";
    case 180: return "180";
    case 181: return "180 mirror";
    case 270: return "270";
    case 271: return "270 mirror";
    default: return "0";
  }
}

// The settings pick-list: every mount, value|label.
#define INPUT_MOUNT_OPTIONS \
  "0|0,90|90,180|180,270|270,1|0 mirror,91|90 mirror,181|180 mirror,271|270 mirror"

// A direction vector (x right, y up) in the device's frame, straightened by
// its mount and then turned by the screen's quarter turns.
template <class T>
inline void orientVector(Mount mount, int screenQuarterTurns, T& x, T& y) {
  if (mount.mirror) x = -x;
  const int turns = (mount.quarterTurns + screenQuarterTurns) & 3;
  const T rx = x, ry = y;
  switch (turns) {
    case 1: x = -ry; y = rx; break;
    case 2: x = -rx; y = -ry; break;
    case 3: x = ry; y = -rx; break;
    default: break;
  }
}

// The same for direction-button bits (one each for up, right, down, left).
inline uint32_t orientDirectionBits(uint32_t bits, Mount mount, int screenQuarterTurns, uint32_t upBit,
                                    uint32_t rightBit, uint32_t downBit, uint32_t leftBit) {
  if (mount.mirror) {
    const bool l = bits & leftBit, r = bits & rightBit;
    bits &= ~(leftBit | rightBit);
    if (l) bits |= rightBit;
    if (r) bits |= leftBit;
  }
  return rotateDirectionBits(bits, mount.quarterTurns + screenQuarterTurns, upBit, rightBit, downBit, leftBit);
}

// The quarter turns a mount needs so that a lean the device reads as
// `deviceDir` (in its own frame) shows as up on the screen, keeping the
// mount's mirror: the teach step ("tilt toward the top of the screen").
constexpr int learnQuarterTurns(Dir deviceDir, bool mirror, int screenQuarterTurns) {
  // Mirroring exchanges right and left; then (d - mount - screen) mod 4 = up.
  return ((static_cast<int>(mirror && (deviceDir == kRight || deviceDir == kLeft)
                                ? static_cast<Dir>((static_cast<int>(deviceDir) + 2) & 3)
                                : deviceDir) -
           (screenQuarterTurns & 3)) % 4 + 4) % 4;
}

}  // namespace input_rotation
