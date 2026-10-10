#include "HAL_Display.h"
#include "display_lifecycle_platform.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
static bool hasEvent(const std::string& event) {
  return std::find(platform.events.begin(), platform.events.end(), event) != platform.events.end();
}

[[maybe_unused]] static void requireEventOrder(std::initializer_list<std::string> expected) {
  auto cursor = platform.events.begin();
  for (const auto& event : expected) {
    cursor = std::find(cursor, platform.events.end(), event);
    assert(cursor != platform.events.end());
    ++cursor;
  }
}

static void exerciseSsd1306(uint8_t bus, uint8_t address) {
  gSettings.oledBus = bus;
  assert(!displayIsReady() && !gDisplay);
  assert(!displaySetPower(true));

  platform.managerAvailable = false;
  assert(!displayInit());
  platform.managerAvailable = true;
  platform.busAvailable[bus] = false;
  assert(!displayInit());
  assert(platform.constructors == 0 && platform.beginCalls == 0);
  platform.busAvailable[bus] = true;

  platform.detectedAddress[bus] = 0;
  assert(!displayInit());
  assert(!displayIsReady() && !gDisplay && platform.beginCalls == 0);
  assert(!platform.probes.empty());
  for (const auto& probe : platform.probes) assert(probe.first == bus);

  platform.detectedAddress[bus] = address;
  platform.lockAvailable = false;
  assert(!displayInit());
  assert(!displayIsReady() && !gDisplay && platform.beginCalls == 0);
  assert(platform.constructors == platform.destructors);
  platform.lockAvailable = true;

  // A driver object allocated by a failed first begin must never be published
  // as an initialized display. A retry must execute the driver again.
  platform.beginResults = {false, false, false};
  assert(!displayInit());
  assert(!displayIsReady() && !gDisplay);
  assert(platform.constructors == platform.destructors);
  const int failedBegins = platform.beginCalls;
  platform.beginResults.clear();
  platform.probes.clear();
  assert(displayInit());
  assert(displayIsReady() && gDisplay && gDisplayI2cAddress == address);
  assert(platform.beginCalls > failedBegins);
  assert(platform.probes.front() == std::make_pair(bus, uint8_t{0x3d}));
  if (address == 0x3c) {
    assert(platform.probes.size() >= 2);
    assert(platform.probes[1] == std::make_pair(bus, uint8_t{0x3c}));
  }
  assert(platform.registrations.back() == std::make_pair(bus, address));
  auto* retained = gDisplay;
  const int initialBegins = platform.beginCalls;
  assert(displayInit());
  assert(platform.beginCalls == initialBegins);

  // The bus is fixed with the retained driver. Changing a reboot-required
  // setting must not acquire the other bus lock around the original Wire.
  gSettings.oledBus = 1 - bus;
  const int beforeUpdate = platform.updates;
  displayUpdate();
  assert(platform.updates == beforeUpdate + 1 && platform.transactionBus == bus);
  assert(platform.transactionAddress == address);
  gSettings.oledBus = bus;
  platform.paused[bus] = true;
  displayUpdate();
  assert(platform.updates == beforeUpdate + 1);
  platform.paused[bus] = false;

  // Lock refusal cannot turn into unprotected writes, including dim and power.
  platform.lockAvailable = false;
  const int commands = platform.commands, dims = platform.dims, updates = platform.updates;
  displaySetBrightness(35);
  displayDim(true);
  displayUpdate();
  assert(!displaySetPower(false));
  assert(platform.commands == commands && platform.dims == dims && platform.updates == updates);
  platform.lockAvailable = true;
  displaySetBrightness(35);
  displayDim(true);
  assert(displaySetPower(false));
  assert(displaySetPower(true));
  assert(platform.commands >= commands + 4 && platform.dims == dims + 1);

  // Stopping keeps the canvas alive for an already-admitted UI render, but
  // pointer existence is no longer enough to allow transport operations.
  displayStop();
  assert(gDisplay == retained && !displayIsReady());
  const int stoppedCommands = platform.commands, stoppedUpdates = platform.updates;
  displaySetBrightness(70);
  displayDim(false);
  displayUpdate();
  assert(!displaySetPower(true));
  assert(platform.commands == stoppedCommands && platform.updates == stoppedUpdates);
  assert(displayInit());
  assert(displayIsReady() && gDisplay == retained && platform.beginCalls > initialBegins);

  platform.events.clear();
  displayPrepareForSleep();
  const auto preparedEvents = platform.events;
  displayPrepareForSleep();
  assert(platform.events == preparedEvents); // Preserve the first pending resume.
#if defined(I2C2_POWER_PIN) && I2C2_POWER_PIN >= 0
  if (bus == 1) {
    assert(!displayIsReady() && gDisplay == retained);
    requireEventOrder({"cmd:174", "delay:10", "rail:39:0", "delay:200"});
    platform.events.clear();
    platform.beginResults = {false, false};
    assert(!displayResumeFromSleep());
    assert(!displayIsReady() && gDisplay == retained);
    requireEventOrder({"rail:39:1", "delay:100", "recover:1", "begin", "delay:100", "begin"});
    // Restoring the rail is not initialization. A repeated resume must not
    // forget the preceding begin failure merely because Vcc is already up.
    platform.beginResults = {false, false};
    assert(!displayResumeFromSleep());
    assert(!displayIsReady());
    platform.beginResults.clear();
    const int failedWakeBegins = platform.beginCalls;
    platform.events.clear();
    assert(displayInit());
    assert(displayIsReady() && platform.beginCalls > failedWakeBegins && gDisplay == retained);
    requireEventOrder({"begin", "cmd:174", "cmd:141", "cmd:20", "cmd:175"});

    platform.events.clear();
    displayPrepareForSleep();
    platform.events.clear();
    assert(displayResumeFromSleep());
    requireEventOrder({"rail:39:1", "delay:100", "recover:1", "begin",
                       "cmd:174", "cmd:141", "cmd:20", "cmd:175"});

    // Successful begin alone cannot hide a denied post-power-cycle command.
    displayPrepareForSleep();
    platform.lockUnavailableAfterBegin = true;
    assert(!displayResumeFromSleep());
    assert(!displayIsReady());
    platform.lockUnavailableAfterBegin = false;
    platform.lockAvailable = true;
    assert(displayResumeFromSleep() && displayIsReady());
  } else
#endif
  {
    assert(!hasEvent("rail:39:0"));
    const int begins = platform.beginCalls;
    assert(displayResumeFromSleep());
    assert(displayIsReady() && platform.beginCalls == begins);
    assert(hasEvent("cmd:174") && hasEvent("cmd:175"));
  }
  const auto resumedEvents = platform.events;
  assert(displayResumeFromSleep() && displayIsReady());
  assert(platform.events == resumedEvents); // Already resumed is a safe success.

  // Closing while asleep must cancel the pending resume request.
  displayPrepareForSleep();
  displayStop();
  const int cancelledBegins = platform.beginCalls;
  assert(!displayResumeFromSleep() && !displayIsReady());
  assert(platform.beginCalls == cancelledBegins);
  platform.events.clear();
  assert(displayInit());
#if defined(I2C2_POWER_PIN) && I2C2_POWER_PIN >= 0
  if (bus == 1) {
    requireEventOrder({"begin", "cmd:174", "cmd:141", "cmd:20", "cmd:175"});

    // A manual start can itself restore the rail after a close while asleep;
    // it needs the same recovery and charge-pump sequence as normal wake.
    displayPrepareForSleep();
    displayStop();
    platform.events.clear();
    assert(displayInit() && displayIsReady());
    requireEventOrder({"rail:39:1", "delay:100", "recover:1", "begin",
                       "cmd:174", "cmd:141", "cmd:20", "cmd:175"});
  }
#endif
  displayStop();
  platform.events.clear();
  displayPrepareForSleep();
  displayResumeFromSleep();
  assert(!displayIsReady() && gDisplay == retained);
  assert(!hasEvent("begin") && !hasEvent("cmd:175"));
  assert(platform.destructors == platform.constructors - 1);
}
#endif

int main(int argc, char** argv) {
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  assert(argc == 3);
  exerciseSsd1306(static_cast<uint8_t>(std::atoi(argv[1])),
                 static_cast<uint8_t>(std::strtol(argv[2], nullptr, 16)));
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789_P4_EYE
  (void)argc;
  (void)argv;
  platform.beginResults = {false, true};
  assert(!displayInit() && !displayIsReady());
  auto* retained = gDisplay;
  assert(retained);
  auto* canvas = retained->getBuffer();
  assert(displayInit() && displayIsReady() && gDisplay == retained);
  displaySetBrightness(60);
  displayDim(true);
  assert(displaySetPower(false));
  assert(displaySetPower(true));
  displayStop();
  assert(!displayIsReady() && gDisplay == retained && retained->getBuffer() == canvas);
  assert(platform.ends == 1 && platform.destructors == 0);
  assert(displayInit() && gDisplay == retained && retained->getBuffer() == canvas);
  displayPrepareForSleep();
  displayPrepareForSleep();
  assert(displayResumeFromSleep());
  assert(displayIsReady() && platform.powerCalls >= 4);
  const int powerCalls = platform.powerCalls;
  assert(displayResumeFromSleep() && platform.powerCalls == powerCalls);
  displayStop();
#else
  (void)argc;
  (void)argv;
  assert(!DISPLAY_ENABLED);
  assert(!displayInit() && !displayIsReady());
  assert(!displaySetPower(true) && !displayResumeFromSleep());
  displayStop();
  displayPrepareForSleep();
  displayClear();
  displayUpdate();
  displayDim(true);
  displaySetBrightness(12);
  assert(platform.constructors == 0 && platform.beginCalls == 0);
#endif
  std::puts("display lifecycle variant passed");
}
