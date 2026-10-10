// Link fixtures only: catalogue rows and their feature gates come from production.
#include "System_CommandRegistry.h"
#include <cassert>
#include <cstdio>
#include <cstring>

#define COMMAND_FIXTURE(symbol, count) \
  extern const CommandEntry symbol[] = {{#symbol, "fixture", false, nullptr}}; \
  extern const size_t symbol##Count = count;
#include "catalogue_fixture_rows.inc"
#undef COMMAND_FIXTURE

struct Binding {
  const char* name;
  const CommandEntry* commands;
  const size_t* count;
  size_t value;
};
#define COMMAND_FIXTURE(symbol, count) {#symbol, symbol, &symbol##Count, count},
static const Binding bindings[] = {
#include "catalogue_fixture_rows.inc"
};
#undef COMMAND_FIXTURE

// Exercise every callback with all providers off, then one provider on at a
// time. This distinguishes wrong providers as well as frozen true/false values.
enum Provider { OFF, RADIO, SD, INPUT, ANO, CAMERA, AUDIO, SENSOR, PROVIDER_COUNT };
static Provider active = OFF;
static const char* lastSensor = nullptr;
bool gInputConnected = false;
bool gAnoEncoderConnected = false;
bool cameraConnected = false;
bool radioCompanionOnline() { return active == RADIO; }
namespace VFS { bool isSDAvailable() { return active == SD; } }
bool audioAnySourceAvailable() { return active == AUDIO; }
bool isSensorConnected(const char* name) {
  assert(name && name[0]);
  lastSensor = name;
  return active == SENSOR;
}

static void printConnection(bool (*callback)()) {
  if (!callback) {
    std::printf("-\n");
    return;
  }
  unsigned mask = 0;
  lastSensor = nullptr;
  for (int provider = OFF; provider < PROVIDER_COUNT; ++provider) {
    active = static_cast<Provider>(provider);
    gInputConnected = active == INPUT;
    gAnoEncoderConnected = active == ANO;
    cameraConnected = active == CAMERA;
    if (callback()) mask |= 1u << provider;
  }
  assert(mask && !(mask & (mask - 1)) && !(mask & 1));
  static const char* names[] = {"off", "radio", "sd", "input", "ano", "camera", "audio", "sensor"};
  for (int provider = RADIO; provider < PROVIDER_COUNT; ++provider) {
    if (mask == (1u << provider)) {
      std::printf("%s", names[provider]);
      if (provider == SENSOR) {
        assert(lastSensor);
        std::printf(":%s", lastSensor);
      } else {
        assert(!lastSensor);
      }
    }
  }
  std::printf("\n");
}

int main() {
  size_t count = static_cast<size_t>(-1), again = 0;
  const CommandModule* modules = getCommandModules(count);
  assert(modules && count && getCommandModules(again) == modules && again == count);
  for (size_t i = 0; i < count; ++i) {
    const CommandModule& module = modules[i];
    const Binding* matched = nullptr;
    for (const Binding& binding : bindings) {
      if (module.commands == binding.commands) matched = &binding;
    }
    assert(matched);
    assert(module.countPtr == matched->count);
    assert(module.commandCount() == matched->value);
    assert(std::strcmp(module.commands[0].name, matched->name) == 0);
    std::printf("%s|%s|%u|", module.name, matched->name, module.flags);
    printConnection(module.isConnected);
  }
}
