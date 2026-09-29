#include "Input_GPIOEncoder.cpp"
#include <algorithm>
#include <string.h>

namespace fixture {
uint32_t now = 0;
bool locked = false;
int levels[55], reads[55], modes[55], resets[55];
void (*interrupts[55])() = {};
int taskCreates = 0, abstractionInits = 0;
TickType_t lastDelay = 0;
}
TaskHandle_t gInputTaskHandle = nullptr;

static void sample(uint32_t now) {
  fixture::now = now;
  try { inputTask(nullptr); assert(false); }
  catch (const fixture::SampleComplete&) {}
  assert(!fixture::locked);
}
static uint32_t consumePresses() {
  const uint32_t actions = gInputCache.buttonPressedAccum;
  gInputCache.buttonPressedAccum = 0;
  return actions;
}
static uint32_t held() { return ~gInputCache.buttons; }

static void turnPositive() {
  // Startup phase is 11; 11 -> 10 -> 00 -> 01 -> 11 is one positive detent.
  const unsigned phases[] = {2, 0, 1, 3};
  for (unsigned ab : phases) {
    const int a = (ab >> 1) & 1u, b = ab & 1u;
    const int pin = fixture::levels[GPIO_ENCODER_PIN_A] != a ?
                    GPIO_ENCODER_PIN_A : GPIO_ENCODER_PIN_B;
    fixture::levels[GPIO_ENCODER_PIN_A] = a;
    fixture::levels[GPIO_ENCODER_PIN_B] = b;
    assert(fixture::interrupts[pin]);
    fixture::interrupts[pin]();
  }
}

int main() {
  constexpr uint32_t x = 1u << 2, y = 1u << 3, start = 1u << 4;
  (void)x; (void)y; (void)start;
  std::fill(fixture::levels, fixture::levels + 55, HIGH);
  assert(inputStartInternal());
  assert(gInputRunning && gInputConnected && gInputCache.dataValid);
  assert(fixture::taskCreates == 1 && fixture::abstractionInits == 1);
  assert(inputStartInternal());
  assert(fixture::taskCreates == 1 && fixture::abstractionInits == 1);
  sample(5);
  assert(held() == 0 && consumePresses() == 0 && fixture::lastDelay == 5);

#if ENABLE_GPIO_ENCODER_BUTTONS
  // Exercise the actual board profile mapping, not copied numeric pin tables.
  assert(kAuxButtonCount == 3);
  assert(kAuxButtons[0].pin == 3 && kAuxButtons[0].action == x);
  assert(kAuxButtons[1].pin == 4 && kAuxButtons[1].action == y);
  assert(kAuxButtons[2].pin == 5 && kAuxButtons[2].action == start);
  for (int pin = 3; pin <= 5; ++pin) assert(fixture::modes[pin] == INPUT_PULLUP);
  fixture::levels[3] = LOW;
  sample(10);
  sample(29);
  assert(held() == 0 && consumePresses() == 0);
  fixture::levels[3] = HIGH; sample(30); // Bounce cancels this candidate press.
  fixture::levels[3] = LOW; sample(35);
  sample(55);
  assert(held() == x && consumePresses() == x);
  const uint32_t pressedSeq = gInputCache.seq;
  sample(60);
  sample(200);
  assert(held() == x && consumePresses() == 0 && gInputCache.seq == pressedSeq);

  // Debounced release changes held state but never latches a new press.
  fixture::levels[3] = HIGH; sample(205); sample(225);
  assert(held() == 0 && consumePresses() == 0);
  fixture::levels[4] = fixture::levels[5] = LOW;
  sample(230); sample(250);
  assert(held() == (y | start));
  // Release before a UI update: both independent press latches survive.
  fixture::levels[4] = fixture::levels[5] = HIGH;
  sample(255); sample(275);
  assert(held() == 0 && consumePresses() == (y | start));

  // A wheel hold and auxiliary press completing in the same sample both arrive.
  fixture::levels[GPIO_ENCODER_PIN_BUTTON] = LOW;
  sample(300); sample(320);
  fixture::levels[3] = LOW; sample(1000); sample(1020);
  assert(held() == (x | rotary::Back));
  assert(consumePresses() == (x | rotary::Back));
  sample(1025);
  assert(held() == x && consumePresses() == 0);
  fixture::levels[GPIO_ENCODER_PIN_BUTTON] = fixture::levels[3] = HIGH;
  sample(1030); sample(1050); sample(1400);
  assert(held() == 0 && consumePresses() == 0);
#else
  // A disabled profile neither configures nor samples any user key.
  fixture::levels[3] = fixture::levels[4] = fixture::levels[5] = LOW;
  sample(10); sample(30);
  assert(held() == 0 && consumePresses() == 0);
#endif

  // Pin ISR -> task -> shared detent queue still works alongside the buttons.
  turnPositive(); sample(1500);
  assert(gpioEncoderHasPendingDetents());
  assert(gpioEncoderConsumeOneDetent() == 1);
  assert(!gpioEncoderHasPendingDetents() && gpioEncoderConsumeOneDetent() == 0);
  turnPositive(); sample(1510); // Leave a pending turn to be cleared on stop.
  gpioEncoderStop();
  assert(!gInputRunning && !gInputConnected && !gInputCache.dataValid);
  assert(held() == 0 && consumePresses() == 0 && !gpioEncoderHasPendingDetents());
  assert(fixture::resets[48] == 1 && fixture::resets[47] == 1 && fixture::resets[2] == 1);
  assert(!fixture::interrupts[48] && !fixture::interrupts[47]);
  sample(1600);
  assert(fixture::lastDelay == 100 && !gInputCache.dataValid);

  // Opening while a key is held must wait for a full release and new press.
  fixture::levels[3] = LOW;
  fixture::now = 1700;
  assert(inputStartInternal());
  assert(fixture::taskCreates == 1 && fixture::abstractionInits == 2);
  sample(1800);
  assert(held() == 0 && consumePresses() == 0);
  fixture::levels[3] = HIGH; sample(1810); sample(1830);
  fixture::levels[3] = LOW; sample(1840); sample(1860);
#if ENABLE_GPIO_ENCODER_BUTTONS
  assert(held() == x && consumePresses() == x);
  char json[512];
  assert(gpioEncoderBuildDataJSON(json, sizeof(json)) > 0);
  assert(strstr(json, "\"buttons\":4") != nullptr);
#else
  assert(held() == 0 && consumePresses() == 0);
#endif
  gpioEncoderStop();
  assert(held() == 0 && consumePresses() == 0);
  for (int pin = 3; pin <= 5; ++pin) {
#if ENABLE_GPIO_ENCODER_BUTTONS
    assert(fixture::resets[pin] == 2);
#else
    assert(fixture::modes[pin] == 0 && fixture::reads[pin] == 0 && fixture::resets[pin] == 0);
#endif
  }
}
