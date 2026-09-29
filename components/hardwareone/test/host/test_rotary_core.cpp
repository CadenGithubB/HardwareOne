#include "../../Input_RotaryCore.h"
#include <assert.h>
#include <stdint.h>
#include <initializer_list>

static int sequence(rotary::Quadrature& q, std::initializer_list<uint8_t> states, int steps = 4) {
  int result = 0;
  for (const auto state : states) result += q.update(state, steps);
  return result;
}

static rotary::Events sample(rotary::Gestures& g, bool pressed, uint32_t at, int turns = 0) {
  return g.update(pressed, at, turns);
}

int main() {
  rotary::Quadrature q;
  q.reset(0);
  assert(sequence(q, {1, 3, 2, 0}) == 1);
  assert(sequence(q, {2, 3, 1, 0}) == -1);
  // Bouncing phases cancel; no extra detent and no half-detent is emitted.
  assert(sequence(q, {1, 0, 1, 3, 1, 3, 2, 3, 2, 0}) == 1);
  assert(sequence(q, {1, 3, 1, 0}) == 0);
  // Missing a Gray-code phase must not manufacture motion.
  q.reset(0);
  assert(sequence(q, {1, 2, 0}) == 0);
  q.reset(0);
  assert(sequence(q, {3, 0, 3, 0}) == 0);
  for (int edges : {1, 2, 4}) {
    q.reset(0);
    assert(sequence(q, {1, 3, 2, 0}, edges) == 4 / edges);
    assert(sequence(q, {2, 3, 1, 0}, edges) == -4 / edges);
  }
  // Many rapid turns remain proportional, independently of UI sampling.
  q.reset(0);
  int total = 0;
  for (int i = 0; i < 1000; ++i) total += sequence(q, {1, 3, 2, 0});
  assert(total == 1000);

  rotary::Gestures g;
  g.reset(false, 0);
  assert(sample(g, false, 1, 3).detents == 3);
  sample(g, true, 10);
  sample(g, false, 14);  // Switch chatter is not a click.
  sample(g, true, 18);
  assert(sample(g, true, 38).actions == 0);
  sample(g, false, 70);
  assert(sample(g, false, 90).actions == 0);
  assert(sample(g, false, 340).actions == 0);
  assert(sample(g, false, 341).actions == rotary::Select);
  assert(sample(g, false, 400).actions == 0);

  // A hold has exactly one Back event and no preliminary Select on press.
  g.reset(false, 0);
  sample(g, true, 10);
  sample(g, true, 30);
  assert(sample(g, true, 729).actions == 0);
  assert(sample(g, true, 730).actions == rotary::Back);
  assert(sample(g, true, 1500).actions == 0);
  sample(g, false, 1510);
  assert(sample(g, false, 1530).actions == 0);
  assert(sample(g, false, 2000).actions == 0);

  // Double click emits Function only. Pressing just inside the window still
  // qualifies when debounce completes just outside it.
  g.reset(false, 0);
  sample(g, true, 10); sample(g, true, 30);
  sample(g, false, 50); sample(g, false, 70);
  assert(sample(g, true, 319).actions == 0);
  assert(sample(g, true, 321).actions == 0);
  assert(sample(g, true, 339).actions == 0);
  sample(g, false, 360);
  assert(sample(g, false, 380).actions == rotary::Function);
  assert(sample(g, false, 1000).actions == 0);

  // Press-turn routes to secondary actions, suppressing scroll, click and hold.
  g.reset(false, 0);
  sample(g, true, 10); sample(g, true, 30);
  auto event = sample(g, true, 40, 2);
  assert(event.detents == 0 && event.actions == rotary::Extra);
  event = sample(g, true, 45, -1);
  assert(event.detents == 0 && event.actions == rotary::Delete);
  assert(sample(g, true, 1000).actions == 0);
  sample(g, false, 1010); sample(g, false, 1030);
  assert(sample(g, false, 1300).actions == 0);

  // A startup-held switch cannot trigger an action until released/re-pressed.
  g.reset(true, 0);
  assert(sample(g, true, 1000).actions == 0);
  assert(sample(g, true, 1001, 1).detents == 0);
  sample(g, false, 1010);
  assert(sample(g, false, 1030).actions == 0);
  assert(sample(g, false, 2000).actions == 0);

  // millis() rollover does not turn a short click into a hold.
  g.reset(false, UINT32_MAX - 100);
  sample(g, true, UINT32_MAX - 90);
  sample(g, true, UINT32_MAX - 70);
  sample(g, false, UINT32_MAX - 10);
  assert(sample(g, false, 10).actions == 0);
  assert(sample(g, false, 261).actions == rotary::Select);
}
