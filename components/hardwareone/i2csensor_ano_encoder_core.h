#pragma once

// The Adafruit ANO rotary encoder (product 5740) without the hardware: the
// seesaw pins and their button bits, the address rule, and the poll state
// machine (buttons, short taps from the interrupt flags, the RIGHT / RIGHT+IN
// roles and the wheel). Pure and free of Arduino types: the driver
// (i2csensor_ano_encoder.cpp) feeds it each poll's reads, and the host test
// (test/host/test_ano_encoder_core.cpp) runs the same code.

#include <stdint.h>

// Button bits (the device-native layout of the `buttons` field every reading
// carries: ESP-NOW, MQTT, the web card, `anoencoderread json`). Active high.
#define ANO_BTN_IN     (1u << 0)
#define ANO_BTN_UP     (1u << 1)
#define ANO_BTN_DOWN   (1u << 2)
#define ANO_BTN_LEFT   (1u << 3)
#define ANO_BTN_RIGHT  (1u << 4)
#define ANO_BTN_ALL    (ANO_BTN_IN | ANO_BTN_UP | ANO_BTN_DOWN | ANO_BTN_LEFT | ANO_BTN_RIGHT)
// Edge-only bits the poll makes (never in a level): START is the RIGHT+IN
// chord (bit 16, as the gamepad's START), SELECT a RIGHT tap at its release.
// RIGHT itself is never an edge: its only role is SELECT.
#define ANO_VIRT_START  (1u << 16)
#define ANO_VIRT_SELECT (1u << 17)

// Seesaw GPIO pins of the breakout's five switches (Adafruit's example
// sketch: the ATtiny817 has them on pins 1..5; the wheel is encoder 0).
constexpr uint8_t kAnoPinIn = 1;
constexpr uint8_t kAnoPinUp = 2;
constexpr uint8_t kAnoPinLeft = 3;
constexpr uint8_t kAnoPinDown = 4;
constexpr uint8_t kAnoPinRight = 5;
constexpr uint32_t kAnoPinMask = (1UL << kAnoPinIn) | (1UL << kAnoPinUp) | (1UL << kAnoPinLeft) |
                                 (1UL << kAnoPinDown) | (1UL << kAnoPinRight);

// Addresses: 0x49 plus the A0-A2 jumpers, 0x49..0x50. One rule for the
// setter, the driver and the device table.
constexpr uint8_t kAnoAddressDefault = 0x49;
constexpr uint8_t kAnoAddressMin = 0x49;
constexpr uint8_t kAnoAddressMax = 0x50;
#define ANO_ADDRESS_OPTIONS \
  "73|0x49 (default),74|0x4A,75|0x4B,76|0x4C,77|0x4D,78|0x4E,79|0x4F,80|0x50"

constexpr bool anoAddressValid(long address) {
  return address >= kAnoAddressMin && address <= kAnoAddressMax;
}
// The address the driver uses for a saved value (the default for one outside
// the range).
constexpr uint8_t anoAddressFromSetting(long saved) {
  return anoAddressValid(saved) ? static_cast<uint8_t>(saved) : kAnoAddressDefault;
}

// Pins set in `pins` (a pressed mask or an interrupt-flag mask, one bit per
// seesaw pin) as ANO_BTN_* bits. The swaps (mounting orientation) pick which
// pin is which button, here at the one place pins become buttons, so
// everything downstream sees the labels the user sees.
inline uint32_t anoPinsToButtons(uint32_t pins, bool swapUpDown, bool swapLeftRight) {
  const uint8_t upPin = swapUpDown ? kAnoPinDown : kAnoPinUp;
  const uint8_t downPin = swapUpDown ? kAnoPinUp : kAnoPinDown;
  const uint8_t leftPin = swapLeftRight ? kAnoPinRight : kAnoPinLeft;
  const uint8_t rightPin = swapLeftRight ? kAnoPinLeft : kAnoPinRight;
  uint32_t out = 0;
  if (pins & (1UL << kAnoPinIn)) out |= ANO_BTN_IN;
  if (pins & (1UL << upPin)) out |= ANO_BTN_UP;
  if (pins & (1UL << downPin)) out |= ANO_BTN_DOWN;
  if (pins & (1UL << leftPin)) out |= ANO_BTN_LEFT;
  if (pins & (1UL << rightPin)) out |= ANO_BTN_RIGHT;
  return out;
}

// One poll's reads. Each part counts only when its read succeeded. The
// driver reads the buttons, then the interrupt flags, then the wheel delta
// (reading the delta clears it on the chip, so it goes last: a failed earlier
// read leaves the turns on the chip for the next poll).
struct AnoPollReads {
  bool buttonsOk = false;
  uint32_t bulk = 0;      // GPIO bulk: a pin's bit is 1 while its switch is open (active low)
  bool flagsOk = false;
  uint32_t intFlags = 0;  // INTFLAG: a pin's bit is 1 when it changed since the last read
  bool deltaOk = false;
  int32_t delta = 0;      // ENCODER_DELTA: detents since the last read
};

// The state the poll keeps between reads.
struct AnoPollState {
  uint32_t buttons = 0;   // the buttons at the last good read
  bool rightDown = false; // RIGHT is held and its release decides SELECT
  bool chorded = false;   // IN was pressed while RIGHT was held (START sent)
  bool inMasked = false;  // the chord's IN is still held: hidden from the level
  uint32_t tappedLast = 0; // the buttons counted as taps at the last good read
};

struct AnoPollResult {
  bool buttonsValid = false;   // the button part was read and is plausible
  uint32_t buttons = 0;        // the buttons held now (ANO_BTN_*): the native reading
  uint32_t presses = 0;        // physical presses this poll, taps included (button events)
  uint32_t edges = 0;          // the presses the navigation sees (ANO_BTN_*, START, SELECT)
  uint32_t level = 0;          // the held buttons the navigation sees (no RIGHT, no chord IN)
  uint32_t navHidden = 0;      // buttons & ~level: held, but not for the navigation
  int32_t rawDelta = 0;        // the chip's turns (the reading's position follows them)
  int32_t detents = 0;         // the turns as the navigation sees them (inverted on request)
};

// One poll. Rules:
// - A press is a button held now that was not at the last read. A switch
//   that was open at the last read, is open now and has its interrupt flag
//   set was pressed and released between the reads: one press (a tap). A
//   flag on a button held at either read is not a press (the flags mark
//   every change, releases too).
// - The reads race the switch: the buttons are read before the flags, and
//   reading the flags clears them. A press that lands between the two reads
//   shows released with its flag set (counted as a tap), and held at the next
//   read with no flag. That held button is the same press, not a second one:
//   a button counted as a tap at the last read and held now without a new
//   flag is not pressed again. A flag at this read is a new change, so a real
//   second press still counts. The same press released again before the
//   next read shows released with its flag set (the release) at that read:
//   a button counted as a tap at the last read is not a tap again at this
//   one. Two real taps of one button in back-to-back windows (faster than
//   about 11 a second at the 90 ms poll) count as one.
// - RIGHT sends nothing when pressed. Released with no chord, it sends
//   SELECT only (a tap inside one window, too); RIGHT is no navigation
//   direction. A RIGHT tap whose press runs on into the next read (the race
//   above) already sent its SELECT: its release sends nothing, and IN pressed
//   while it is held is still the chord. IN pressed while RIGHT is
//   held is the chord: START only, never SELECT, never A (IN); that IN stays
//   out of the level until it is released.
// - All five switches pressed at once reads as a bus held low: not plausible,
//   nothing changes.
// - The wheel's turns count whenever the delta read succeeded, whatever the
//   buttons did.
inline AnoPollResult anoPoll(AnoPollState& state, const AnoPollReads& reads, bool swapUpDown,
                             bool swapLeftRight, bool invert) {
  AnoPollResult out;
  if (reads.deltaOk) {
    out.rawDelta = reads.delta;
    out.detents = invert ? -reads.delta : reads.delta;
  }
  const bool plausible = reads.buttonsOk && reads.flagsOk && (reads.bulk & kAnoPinMask) != 0;
  if (!plausible) {
    out.buttons = state.buttons;
    out.level = state.buttons & ~ANO_BTN_RIGHT & ~(state.inMasked ? ANO_BTN_IN : 0u);
    out.navHidden = state.buttons & ~out.level;
    return out;
  }
  out.buttonsValid = true;
  const uint32_t now = anoPinsToButtons(~reads.bulk & kAnoPinMask, swapUpDown, swapLeftRight);
  const uint32_t flagged = anoPinsToButtons(reads.intFlags & kAnoPinMask, swapUpDown, swapLeftRight);
  const uint32_t prev = state.buttons;
  // Held now after a tap at the last read, with no change since: that tap's
  // own press, already counted.
  const uint32_t continued = now & ~prev & state.tappedLast & ~flagged;
  const uint32_t newly = now & ~prev & ~continued;
  const uint32_t taps = flagged & ~prev & ~now & ~state.tappedLast;
  out.presses = newly | taps;
  state.tappedLast = taps;

  uint32_t edges = 0;
  if (newly & ANO_BTN_RIGHT) {
    state.rightDown = true;
    state.chorded = false;
  }
  if (continued & ANO_BTN_RIGHT) {
    // Its SELECT went out with the tap: held on, it can still chord, and its
    // release sends nothing more (chorded stands for "SELECT is not owed").
    state.rightDown = true;
    state.chorded = true;
  }
  const uint32_t inPress = (newly | taps) & ANO_BTN_IN;
  if (inPress && state.rightDown) {
    edges |= ANO_VIRT_START;
    state.chorded = true;
    if (now & ANO_BTN_IN) state.inMasked = true;
  } else {
    edges |= inPress;
  }
  edges |= (newly | taps) & (ANO_BTN_UP | ANO_BTN_DOWN | ANO_BTN_LEFT);
  if ((taps & ANO_BTN_RIGHT) && !state.rightDown) edges |= ANO_VIRT_SELECT;
  if (state.rightDown && !(now & ANO_BTN_RIGHT)) {
    if (!state.chorded) edges |= ANO_VIRT_SELECT;
    state.rightDown = false;
    state.chorded = false;
  }
  if (state.inMasked && !(now & ANO_BTN_IN)) state.inMasked = false;

  state.buttons = now;
  out.buttons = now;
  out.edges = edges;
  out.level = now & ~ANO_BTN_RIGHT & ~(state.inMasked ? ANO_BTN_IN : 0u);
  out.navHidden = now & ~out.level;
  return out;
}
