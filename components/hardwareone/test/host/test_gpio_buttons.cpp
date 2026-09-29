#include "../../Input_ButtonCore.h"
#include <assert.h>
#include <stdint.h>

int main() {
  gpioinput::Button button;
  assert(gpioinput::Button::DebounceMs == 20);
  button.reset(false, 0);
  assert(!button.pressed());
  assert(!button.update(false, 100));

  // Chatter postpones the press until a full stable debounce interval.
  assert(!button.update(true, 110));
  assert(!button.update(false, 115));
  assert(!button.update(true, 120));
  assert(!button.update(true, 139));
  assert(!button.pressed());
  assert(button.update(true, 140));
  assert(button.pressed());
  assert(!button.update(true, 141));
  assert(!button.update(true, 10000));
  assert(button.pressed());

  // Release chatter keeps the logical key held, with no repeat press event.
  assert(!button.update(false, 10010));
  assert(!button.update(true, 10015));
  assert(!button.update(false, 10020));
  assert(!button.update(false, 10039));
  assert(button.pressed());
  assert(!button.update(false, 10040));
  assert(!button.pressed());
  assert(!button.update(true, 10050));
  assert(button.update(true, 10070));
  assert(button.pressed());

  // All three keys have independent timing; one bouncing key cannot delay
  // another key, and simultaneous stable presses must all be observable.
  gpioinput::Button keys[3];
  for (auto& key : keys) key.reset(false, 0);
  for (auto& key : keys) assert(!key.update(true, 10));
  assert(!keys[0].update(false, 15));
  assert(!keys[0].update(true, 20));
  assert(!keys[0].update(true, 30));
  assert(keys[1].update(true, 30));
  assert(keys[2].update(true, 30));
  assert(!keys[0].pressed());
  assert(keys[1].pressed() && keys[2].pressed());
  assert(keys[0].update(true, 40));
  for (auto& key : keys) {
    assert(!key.update(false, 50));
    assert(!key.update(false, 70));
    assert(!key.pressed());
    assert(!key.update(true, 80));
    assert(key.update(true, 100));
    assert(key.pressed());
  }

  // Starting (or restarting) while held suppresses both the press event and
  // held state until a debounced release. Release bounce cannot arm it early.
  button.reset(true, 200);
  assert(!button.pressed());
  assert(!button.update(true, 1000));
  assert(!button.pressed());
  assert(!button.update(false, 1010));
  assert(!button.update(true, 1020));
  assert(!button.update(true, 1040));
  assert(!button.pressed());
  assert(!button.update(false, 1050));
  assert(!button.update(false, 1069));
  assert(!button.update(false, 1070));
  assert(!button.pressed());
  assert(!button.update(true, 1080));
  assert(button.update(true, 1100));
  assert(button.pressed());
  button.reset(false, 1110);
  assert(!button.pressed());
  assert(!button.update(false, 1130));

  // Debounce intervals span millis() wrap without premature or lost edges.
  button.reset(false, UINT32_MAX - 100);
  assert(!button.update(true, UINT32_MAX - 10));
  assert(!button.update(true, 8));
  assert(!button.pressed());
  assert(button.update(true, 9));
  assert(button.pressed());
  button.reset(false, UINT32_MAX - 100);
  assert(!button.update(true, UINT32_MAX - 90));
  assert(button.update(true, UINT32_MAX - 70));
  assert(!button.update(false, UINT32_MAX - 5));
  assert(!button.update(false, 13));
  assert(button.pressed());
  assert(!button.update(false, 14));
  assert(!button.pressed());
}
