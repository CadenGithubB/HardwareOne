// The ANO rotary encoder's poll without the hardware (i2csensor_ano_encoder_core.h):
// pins to buttons through the mount and the screen's rotation, the address rule, taps from the interrupt
// flags, the RIGHT tap = SELECT at release and RIGHT+IN = START rules, the
// wheel's detents, and failed or implausible reads.

#include <cassert>
#include <cstdio>

#include "i2csensor_ano_encoder_core.h"

namespace {

// The GPIO bulk for the buttons held (as ANO_BTN_* with no swaps): every
// other button pin reads high (open).
uint32_t bulkFor(uint32_t held) {
  uint32_t pressedPins = 0;
  if (held & ANO_BTN_IN) pressedPins |= 1u << kAnoPinIn;
  if (held & ANO_BTN_UP) pressedPins |= 1u << kAnoPinUp;
  if (held & ANO_BTN_DOWN) pressedPins |= 1u << kAnoPinDown;
  if (held & ANO_BTN_LEFT) pressedPins |= 1u << kAnoPinLeft;
  if (held & ANO_BTN_RIGHT) pressedPins |= 1u << kAnoPinRight;
  return kAnoPinMask & ~pressedPins;
}

// The INTFLAG bits for the buttons that changed (no swaps).
uint32_t flagsFor(uint32_t changed) { return kAnoPinMask & ~bulkFor(changed); }

AnoPollReads reads(uint32_t held, uint32_t changed = 0, int32_t delta = 0) {
  AnoPollReads r;
  r.buttonsOk = r.flagsOk = r.deltaOk = true;
  r.bulk = bulkFor(held);
  r.intFlags = flagsFor(changed);
  r.delta = delta;
  return r;
}

AnoPollResult poll(AnoPollState& s, const AnoPollReads& r, bool mirror = false) {
  return anoPoll(s, r, input_rotation::Mount{0, mirror});
}

void testPinsAndSwaps() {
  using input_rotation::Mount;
  const uint32_t up = 1u << kAnoPinUp, down = 1u << kAnoPinDown;
  const uint32_t left = 1u << kAnoPinLeft, right = 1u << kAnoPinRight, in = 1u << kAnoPinIn;
  assert(anoPinsToButtons(in) == ANO_BTN_IN);
  assert(anoPinsToButtons(up | left) == (ANO_BTN_UP | ANO_BTN_LEFT));
  // The default mount (180): both pairs exchanged, as the two old swaps did.
  assert(anoPinsToButtons(up, Mount{2, false}) == ANO_BTN_DOWN);
  assert(anoPinsToButtons(down, Mount{2, false}) == ANO_BTN_UP);
  assert(anoPinsToButtons(up | right, Mount{2, false}) == (ANO_BTN_DOWN | ANO_BTN_LEFT));
  assert(anoPinsToButtons(in, Mount{2, false}) == ANO_BTN_IN);
  // Mirror: LEFT and RIGHT exchanged (the old LEFT/RIGHT swap); 180 mirror
  // is the old UP/DOWN swap.
  assert(anoPinsToButtons(left, Mount{0, true}) == ANO_BTN_RIGHT);
  assert(anoPinsToButtons(up, Mount{0, true}) == ANO_BTN_UP);
  assert(anoPinsToButtons(up, Mount{2, true}) == ANO_BTN_DOWN);
  assert(anoPinsToButtons(left, Mount{2, true}) == ANO_BTN_LEFT);
  // A quarter turn: the ring's RIGHT is at the top (mount 90, or the screen
  // turned 90 with a straight mount): it is UP, with UP's roles.
  assert(anoPinsToButtons(right, Mount{1, false}) == ANO_BTN_UP);
  assert(anoPinsToButtons(right, Mount{0, false}, 1) == ANO_BTN_UP);
  assert(anoPinsToButtons(up, Mount{0, false}, 1) == ANO_BTN_LEFT);
  // The mount first, then the screen: 90 + 270 is straight.
  assert(anoPinsToButtons(up | right, Mount{1, false}, 3) == (ANO_BTN_UP | ANO_BTN_RIGHT));
  // Pins that are not buttons are ignored.
  assert(anoPinsToButtons(1u << 0 | 1u << 9) == 0);
  // The mount applies to the interrupt flags too.
  AnoPollState s;
  AnoPollReads r = reads(0, ANO_BTN_UP);
  const AnoPollResult turned = anoPoll(s, r, Mount{2, false});
  assert(turned.edges == ANO_BTN_DOWN && turned.presses == ANO_BTN_DOWN);
  // The screen at 270 puts the physical RIGHT button at the bottom: it is
  // DOWN, a navigation step, and the button now on the right (physical UP)
  // carries SELECT.
  AnoPollState t;
  assert(anoPoll(t, reads(0, ANO_BTN_RIGHT), Mount{0, false}, 3).edges == ANO_BTN_DOWN);
  AnoPollState u;
  assert(anoPoll(u, reads(0, ANO_BTN_UP), Mount{0, false}, 3).edges == ANO_VIRT_SELECT);
}

void testAddressRule() {
  assert(anoAddressValid(0x49) && anoAddressValid(0x50) && anoAddressValid(0x4C));
  assert(!anoAddressValid(0x48) && !anoAddressValid(0x51) && !anoAddressValid(0) && !anoAddressValid(-1));
  assert(anoAddressFromSetting(0x4A) == 0x4A);
  assert(anoAddressFromSetting(0) == kAnoAddressDefault);
  assert(anoAddressFromSetting(0x36) == kAnoAddressDefault);
  assert(anoAddressFromSetting(127) == kAnoAddressDefault);
}

void testPressesAndTaps() {
  AnoPollState s;
  // A press held across polls: one edge, then nothing, and its release is no
  // press even with its change flag set.
  AnoPollResult r = poll(s, reads(ANO_BTN_IN, ANO_BTN_IN));
  assert(r.buttonsValid && r.buttons == ANO_BTN_IN && r.edges == ANO_BTN_IN && r.presses == ANO_BTN_IN);
  assert(r.level == ANO_BTN_IN);
  r = poll(s, reads(ANO_BTN_IN));
  assert(r.edges == 0 && r.presses == 0 && r.buttons == ANO_BTN_IN);
  r = poll(s, reads(0, ANO_BTN_IN));
  assert(r.edges == 0 && r.presses == 0 && r.buttons == 0 && r.level == 0);
  // A tap inside one window (released at both reads, flag set): one press.
  r = poll(s, reads(0, ANO_BTN_UP));
  assert(r.edges == ANO_BTN_UP && r.presses == ANO_BTN_UP && r.buttons == 0 && r.level == 0);
  r = poll(s, reads(0));
  assert(r.edges == 0 && r.presses == 0);
  // Released, then pressed again before the next read: a normal press, once.
  r = poll(s, reads(ANO_BTN_DOWN, ANO_BTN_DOWN));
  assert(r.edges == ANO_BTN_DOWN && r.presses == ANO_BTN_DOWN);
  r = poll(s, reads(0, ANO_BTN_DOWN));
  assert(r.edges == 0);
  // LEFT (B) press.
  r = poll(s, reads(ANO_BTN_LEFT, ANO_BTN_LEFT));
  assert(r.edges == ANO_BTN_LEFT);
}

void testRightIsSelectAtRelease() {
  AnoPollState s;
  // RIGHT pressed: nothing yet, and not in the navigation's level.
  AnoPollResult r = poll(s, reads(ANO_BTN_RIGHT, ANO_BTN_RIGHT));
  assert(r.edges == 0 && r.presses == ANO_BTN_RIGHT && r.buttons == ANO_BTN_RIGHT && r.level == 0);
  r = poll(s, reads(ANO_BTN_RIGHT));
  assert(r.edges == 0 && r.level == 0);
  // Released with no chord: SELECT only (RIGHT is no direction), once.
  r = poll(s, reads(0, ANO_BTN_RIGHT));
  assert(r.edges == ANO_VIRT_SELECT && r.presses == 0 && r.buttons == 0);
  r = poll(s, reads(0));
  assert(r.edges == 0);
  // A RIGHT tap inside one window: SELECT at once.
  r = poll(s, reads(0, ANO_BTN_RIGHT));
  assert(r.edges == ANO_VIRT_SELECT && r.presses == ANO_BTN_RIGHT);
  // A long hold, no IN: still SELECT at its release (no timeout).
  poll(s, reads(ANO_BTN_RIGHT, ANO_BTN_RIGHT));
  for (int i = 0; i < 50; ++i) assert(poll(s, reads(ANO_BTN_RIGHT)).edges == 0);
  assert(poll(s, reads(0, ANO_BTN_RIGHT)).edges == ANO_VIRT_SELECT);
  // IN held first, then RIGHT: no chord (RIGHT must lead).
  poll(s, reads(ANO_BTN_IN, ANO_BTN_IN));
  r = poll(s, reads(ANO_BTN_IN | ANO_BTN_RIGHT, ANO_BTN_RIGHT));
  assert(r.edges == 0 && r.level == ANO_BTN_IN);
  r = poll(s, reads(ANO_BTN_IN, ANO_BTN_RIGHT));
  assert(r.edges == ANO_VIRT_SELECT);
  poll(s, reads(0, ANO_BTN_IN));
}

void testRightInChordIsStartOnly() {
  AnoPollState s;
  poll(s, reads(ANO_BTN_RIGHT, ANO_BTN_RIGHT));
  // IN while RIGHT is held: START only, never A (IN), never SELECT.
  AnoPollResult r = poll(s, reads(ANO_BTN_RIGHT | ANO_BTN_IN, ANO_BTN_IN));
  assert(r.edges == ANO_VIRT_START && r.presses == ANO_BTN_IN);
  assert(r.buttons == (ANO_BTN_RIGHT | ANO_BTN_IN) && r.level == 0);
  // Releasing RIGHT after the chord sends nothing; the chord's IN stays out
  // of the level until it is released too.
  r = poll(s, reads(ANO_BTN_IN, ANO_BTN_RIGHT));
  assert(r.edges == 0 && r.level == 0 && r.buttons == ANO_BTN_IN);
  r = poll(s, reads(0, ANO_BTN_IN));
  assert(r.edges == 0 && r.level == 0);
  // The next IN press is A again.
  r = poll(s, reads(ANO_BTN_IN, ANO_BTN_IN));
  assert(r.edges == ANO_BTN_IN && r.level == ANO_BTN_IN);
  poll(s, reads(0, ANO_BTN_IN));
  // RIGHT and IN arriving in the same poll: the chord.
  r = poll(s, reads(ANO_BTN_RIGHT | ANO_BTN_IN, ANO_BTN_RIGHT | ANO_BTN_IN));
  assert(r.edges == ANO_VIRT_START);
  r = poll(s, reads(0, ANO_BTN_RIGHT | ANO_BTN_IN));
  assert(r.edges == 0);
  // An IN tap inside one window while RIGHT is held: START, then no SELECT.
  poll(s, reads(ANO_BTN_RIGHT, ANO_BTN_RIGHT));
  r = poll(s, reads(ANO_BTN_RIGHT, ANO_BTN_IN));
  assert(r.edges == ANO_VIRT_START && r.level == 0);
  r = poll(s, reads(0, ANO_BTN_RIGHT));
  assert(r.edges == 0);
  // After a chord, a plain RIGHT tap is SELECT again.
  assert(poll(s, reads(0, ANO_BTN_RIGHT)).edges == ANO_VIRT_SELECT);
}

// The press lands between the buttons read and the flags read: released
// with its flag set at poll k (a tap), held with no flag at k+1.
void testTapRaceCountsOnce() {
  AnoPollState s;
  AnoPollResult r = poll(s, reads(0, ANO_BTN_UP));
  assert(r.presses == ANO_BTN_UP && r.edges == ANO_BTN_UP);
  // Held on: no press, and out of the navigation's level (it had the press
  // as the tap's edge) until released.
  r = poll(s, reads(ANO_BTN_UP));
  assert(r.presses == 0 && r.edges == 0 && r.buttons == ANO_BTN_UP && r.level == 0);
  assert(r.navHidden == ANO_BTN_UP);
  r = poll(s, reads(ANO_BTN_UP));
  assert(r.presses == 0 && r.edges == 0 && r.level == 0);
  r = poll(s, reads(0, ANO_BTN_UP));
  assert(r.presses == 0 && r.edges == 0 && r.navHidden == 0);
  // The next press of it is a normal press, in the level again.
  r = poll(s, reads(ANO_BTN_UP, ANO_BTN_UP));
  assert(r.presses == ANO_BTN_UP && r.level == ANO_BTN_UP);
  poll(s, reads(0, ANO_BTN_UP));
  poll(s, reads(0));
  // The same race, released again before k+1: the release's flag is no
  // second tap.
  r = poll(s, reads(0, ANO_BTN_DOWN));
  assert(r.presses == ANO_BTN_DOWN);
  r = poll(s, reads(0, ANO_BTN_DOWN));
  assert(r.presses == 0 && r.edges == 0);
  // Then a real tap a window later counts.
  poll(s, reads(0));
  r = poll(s, reads(0, ANO_BTN_DOWN));
  assert(r.presses == ANO_BTN_DOWN && r.edges == ANO_BTN_DOWN);
  poll(s, reads(0));
  // A tap at k, then a flagged press at k+1: two presses.
  r = poll(s, reads(0, ANO_BTN_LEFT));
  assert(r.presses == ANO_BTN_LEFT);
  r = poll(s, reads(ANO_BTN_LEFT, ANO_BTN_LEFT));
  assert(r.presses == ANO_BTN_LEFT && r.edges == ANO_BTN_LEFT);
  poll(s, reads(0, ANO_BTN_LEFT));
  // A press held across reads with no tap before it is a normal press.
  poll(s, reads(0));
  r = poll(s, reads(ANO_BTN_IN));
  assert(r.presses == ANO_BTN_IN && r.edges == ANO_BTN_IN);
  poll(s, reads(0, ANO_BTN_IN));
}

// The race on RIGHT: exactly one SELECT, and the held RIGHT still chords.
void testRightTapRaceSelectsOnce() {
  AnoPollState s;
  unsigned selects = 0;
  auto count = [&](const AnoPollResult& r) {
    if (r.edges & ANO_VIRT_SELECT) ++selects;
    assert(!(r.edges & ANO_BTN_RIGHT));
    return r;
  };
  count(poll(s, reads(0, ANO_BTN_RIGHT)));            // k: counted as a tap
  AnoPollResult r = count(poll(s, reads(ANO_BTN_RIGHT)));  // k+1: held, no flag
  assert(r.presses == 0 && r.level == 0 && r.navHidden == ANO_BTN_RIGHT);
  count(poll(s, reads(ANO_BTN_RIGHT)));
  count(poll(s, reads(0, ANO_BTN_RIGHT)));            // the real release
  count(poll(s, reads(0)));
  assert(selects == 1);
  // Held on after the race, IN is still the chord: START, no A, no SELECT.
  selects = 0;
  count(poll(s, reads(0, ANO_BTN_RIGHT)));
  count(poll(s, reads(ANO_BTN_RIGHT)));
  r = count(poll(s, reads(ANO_BTN_RIGHT | ANO_BTN_IN, ANO_BTN_IN)));
  assert(r.edges == ANO_VIRT_START && r.navHidden == (ANO_BTN_RIGHT | ANO_BTN_IN));
  count(poll(s, reads(0, ANO_BTN_RIGHT | ANO_BTN_IN)));
  assert(selects == 1);
}

void testWheel() {
  AnoPollState s;
  AnoPollResult r = poll(s, reads(0, 0, 3));
  assert(r.rawDelta == 3 && r.detents == 3);
  // A mirrored ring (seen from behind) turns the other way; a turned one does not.
  r = poll(s, reads(0, 0, -2), true);
  assert(r.rawDelta == -2 && r.detents == 2);
  r = anoPoll(s, reads(0, 0, -2), input_rotation::Mount{2, false}, 1);
  assert(r.detents == -2);
  // A delta read that failed counts nothing.
  AnoPollReads failed = reads(0, 0, 7);
  failed.deltaOk = false;
  r = poll(s, failed);
  assert(r.rawDelta == 0 && r.detents == 0 && r.buttonsValid);
  // A good delta counts even when the button part failed.
  AnoPollReads buttonsFailed = reads(ANO_BTN_UP, ANO_BTN_UP, 4);
  buttonsFailed.buttonsOk = false;
  buttonsFailed.bulk = 0;
  r = poll(s, buttonsFailed);
  assert(!r.buttonsValid && r.detents == 4 && r.edges == 0 && r.presses == 0);
}

void testFailedAndImplausibleReads() {
  AnoPollState s;
  poll(s, reads(ANO_BTN_RIGHT, ANO_BTN_RIGHT));
  // A failed button or flag read changes nothing (no phantom release).
  AnoPollReads r = reads(0, ANO_BTN_RIGHT);
  r.buttonsOk = false;
  AnoPollResult out = poll(s, r);
  assert(!out.buttonsValid && out.edges == 0 && out.presses == 0 && out.buttons == ANO_BTN_RIGHT);
  assert(out.level == 0);
  r = reads(0, ANO_BTN_RIGHT);
  r.flagsOk = false;
  out = poll(s, r);
  assert(!out.buttonsValid && out.edges == 0);
  // All five pressed at once reads as a bus held low: not plausible.
  out = poll(s, reads(ANO_BTN_ALL, ANO_BTN_ALL));
  assert(!out.buttonsValid && out.edges == 0 && out.buttons == ANO_BTN_RIGHT);
  // The state survived: the real release still sends SELECT.
  assert(poll(s, reads(0, ANO_BTN_RIGHT)).edges == ANO_VIRT_SELECT);
}

}  // namespace

int main() {
  testPinsAndSwaps();
  testAddressRule();
  testPressesAndTaps();
  testRightIsSelectAtRelease();
  testRightInChordIsStartOnly();
  testTapRaceCountsOnce();
  testRightTapRaceSelectsOnce();
  testWheel();
  testFailedAndImplausibleReads();
  std::puts("ANO encoder core tests passed (pins and swaps, address rule, taps, RIGHT = SELECT at "
            "release, RIGHT+IN = START only, the tap race, wheel, failed reads)");
  return 0;
}
