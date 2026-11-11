// Exercises the production order of the handover wipe (System_FactoryWipePolicy.h)
// on a fake device: no Arduino/IDF replacements here.
#include "System_FactoryWipePolicy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

static unsigned checks = 0;
#define CHECK(condition) \
  do { ++checks; if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

// A device as the wipe sees it: one stored request, a filesystem, the other
// NVS entries, and switches for each step failing or the power going.
struct PowerLoss {};
struct FakeDevice {
  bool requestStored = false;
  uint8_t attempts = 0;
  bool filesystemData = true;
  bool nvsData = true;

  bool recordOk = true, filesystemOk = true, clearOk = true, eraseNvsOk = true;
  const char* powerLossIn = "";  // step during which power is lost
  std::string steps;

  void step(const char* name) {
    steps += name;
    steps += ' ';
    if (std::strcmp(powerLossIn, name) == 0) throw PowerLoss{};
  }
  bool armed(uint8_t& count) { count = attempts; return requestStored; }
  bool recordAttempt(uint8_t count) {
    step("record");
    if (!recordOk) return false;
    attempts = count;
    return true;
  }
  void disarm() { step("disarm"); requestStored = false; attempts = 0; }
  bool eraseFilesystem() {
    step("filesystem");
    if (!filesystemOk) return false;
    filesystemData = false;
    return true;
  }
  bool clearNvsEntries() {
    step("clear");
    if (!clearOk) return false;
    nvsData = false;
    return true;
  }
  bool eraseNvs() {
    step("nvs");
    if (!eraseNvsOk) return false;
    nvsData = false;
    requestStored = false;  // the request lives in the partition
    attempts = 0;
    return true;
  }
  void arm() { requestStored = true; attempts = 0; }
  FactoryWipeResult boot() {
    steps.clear();
    try {
      return factoryWipeRun(*this);
    } catch (const PowerLoss&) {
      return FactoryWipeResult::Retry;
    }
  }
};

static void nothingHappensWithoutARequest() {
  FakeDevice device;
  CHECK(device.boot() == FactoryWipeResult::NotArmed);
  CHECK(device.steps.empty() && device.filesystemData && device.nvsData);
  CHECK(factoryWipeResultText(FactoryWipeResult::NotArmed) == nullptr);
}

static void aRequestErasesFilesystemThenNvs() {
  FakeDevice device;
  device.arm();
  CHECK(device.boot() == FactoryWipeResult::Done);
  // The attempt is counted first; NVS, which holds the request, goes last.
  CHECK(device.steps == "record filesystem clear nvs ");
  CHECK(!device.filesystemData && !device.nvsData && !device.requestStored);
  CHECK(device.boot() == FactoryWipeResult::NotArmed && device.steps.empty());
}

static void powerLossAtAnyStepResumes() {
  for (const char* lostIn : {"record", "filesystem", "clear", "nvs"}) {
    FakeDevice device;
    device.arm();
    device.powerLossIn = lostIn;
    CHECK(device.boot() == FactoryWipeResult::Retry);
    CHECK(device.requestStored);  // still asked for at the next boot
    device.powerLossIn = "";
    CHECK(device.boot() == FactoryWipeResult::Done);
    CHECK(!device.filesystemData && !device.nvsData && !device.requestStored);
  }
}

static void failedStepsKeepTheRequest() {
  // The attempt cannot be counted: nothing is erased.
  FakeDevice device;
  device.arm();
  device.recordOk = false;
  CHECK(device.boot() == FactoryWipeResult::Retry && device.steps == "record ");
  CHECK(device.filesystemData && device.nvsData && device.requestStored);

  // The filesystem erase fails: NVS, and with it the request, is left alone.
  FakeDevice second;
  second.arm();
  second.filesystemOk = false;
  CHECK(second.boot() == FactoryWipeResult::Retry && second.steps == "record filesystem ");
  CHECK(second.nvsData && second.requestStored && second.attempts == 1);

  // Neither NVS step works: credentials may remain, so the request stays.
  FakeDevice third;
  third.arm();
  third.clearOk = third.eraseNvsOk = false;
  CHECK(third.boot() == FactoryWipeResult::Retry && third.requestStored && third.nvsData);
}

static void aFailedPartitionEraseStillFinishes() {
  // Entries deleted, partition erase refused: done, and the next boot does
  // not erase the filesystem a second time.
  FakeDevice device;
  device.arm();
  device.eraseNvsOk = false;
  CHECK(device.boot() == FactoryWipeResult::DoneNvsCleared);
  CHECK(device.steps == "record filesystem clear nvs disarm ");
  CHECK(!device.filesystemData && !device.nvsData && !device.requestStored);
  CHECK(device.boot() == FactoryWipeResult::NotArmed);

  // Entries could not be deleted but the partition erase worked: done.
  FakeDevice second;
  second.arm();
  second.clearOk = false;
  CHECK(second.boot() == FactoryWipeResult::Done && !second.nvsData && !second.requestStored);
}

static void attemptsAreBounded() {
  FakeDevice device;
  device.arm();
  device.filesystemOk = false;
  for (uint8_t boot = 1; boot <= kFactoryWipeMaxAttempts; ++boot) {
    CHECK(device.boot() == FactoryWipeResult::Retry && device.attempts == boot);
  }
  CHECK(device.boot() == FactoryWipeResult::GaveUp && device.steps == "disarm ");
  CHECK(!device.requestStored && device.nvsData);  // dropped; nothing more erased
  CHECK(device.boot() == FactoryWipeResult::NotArmed);

  // A stored count that is already out of range gives up at once.
  FakeDevice second;
  second.arm();
  second.attempts = 200;
  CHECK(second.boot() == FactoryWipeResult::GaveUp && second.filesystemData);

  // A new request after giving up starts counting again.
  device.arm();
  device.filesystemOk = true;
  CHECK(device.boot() == FactoryWipeResult::Done);
}

static void everyOutcomeHasAReport() {
  for (FactoryWipeResult result : {FactoryWipeResult::Done, FactoryWipeResult::DoneNvsCleared,
                                   FactoryWipeResult::Retry, FactoryWipeResult::GaveUp}) {
    const char* text = factoryWipeResultText(result);
    CHECK(text != nullptr && std::strncmp(text, "full wipe ", 10) == 0);
  }
}

int main() {
  nothingHappensWithoutARequest();
  aRequestErasesFilesystemThenNvs();
  powerLossAtAnyStepResumes();
  failedStepsKeepTheRequest();
  aFailedPartitionEraseStillFinishes();
  attemptsAreBounded();
  everyOutcomeHasAReport();
  std::printf("factory wipe policy: %u checks passed\n", checks);
  return 0;
}
