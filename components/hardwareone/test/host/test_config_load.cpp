// System_ConfigLoad.h is the gate every config-file saver asks before it
// rewrites a file from RAM. This compiles the production header against the
// vendored ArduinoJson and pins the rules the load sites rely on.
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>

#include "System_ConfigLoad.h"

using ConfigLoad::Status;

static int failures;
#define EXPECT(condition) do { if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

static Status parse(const char* text) {
  JsonDocument doc;
  return ConfigLoad::classifyParse(deserializeJson(doc, text));
}

struct FailingAllocator : ArduinoJson::Allocator {
  void* allocate(size_t) override { return nullptr; }
  void deallocate(void*) override {}
  void* reallocate(void*, size_t) override { return nullptr; }
};

int main() {
  // A status nobody set is the zero value, and it must not open a save gate.
  Status unset{};
  EXPECT(unset == Status::NotLoaded);
  EXPECT(!ConfigLoad::savable(unset));

  const Status all[] = {Status::NotLoaded, Status::Ok, Status::AbsentOk, Status::NotMounted,
                        Status::OpenFailed, Status::EmptyFile, Status::Truncated, Status::Corrupt,
                        Status::NoMemory, Status::Partial};
  for (Status s : all) {
    const bool savable = s == Status::Ok || s == Status::AbsentOk;
    EXPECT(ConfigLoad::savable(s) == savable);
    // The three verdicts never overlap: a file is savable over, or it is bad
    // and may be moved aside, or it is good and must be left alone.
    EXPECT(!(ConfigLoad::savable(s) && ConfigLoad::damaged(s)));
    EXPECT(!(ConfigLoad::savable(s) && ConfigLoad::fileIsIntact(s)));
    EXPECT(!(ConfigLoad::damaged(s) && ConfigLoad::fileIsIntact(s)));
    EXPECT(std::strcmp(ConfigLoad::statusName(s), "?") != 0);
    for (Status other : all) {
      if (other != s) EXPECT(std::strcmp(ConfigLoad::statusName(s), ConfigLoad::statusName(other)) != 0);
    }
  }
  // Only bad bytes are quarantined. Out of memory and a failed open are not.
  EXPECT(ConfigLoad::damaged(Status::EmptyFile) && ConfigLoad::damaged(Status::Truncated) &&
         ConfigLoad::damaged(Status::Corrupt));
  EXPECT(!ConfigLoad::damaged(Status::NoMemory) && !ConfigLoad::damaged(Status::OpenFailed) &&
         !ConfigLoad::damaged(Status::NotMounted) && !ConfigLoad::damaged(Status::Partial) &&
         !ConfigLoad::damaged(Status::NotLoaded));
  EXPECT(ConfigLoad::fileIsIntact(Status::NoMemory) && ConfigLoad::fileIsIntact(Status::OpenFailed) &&
         ConfigLoad::fileIsIntact(Status::Partial));

  // Real parser outcomes.
  EXPECT(parse("{\"kinds\":{}}") == Status::Ok);
  EXPECT(parse("") == Status::EmptyFile);
  EXPECT(parse("{\"kinds\":") == Status::Truncated);
  EXPECT(parse("\xff\xfe garbage") == Status::Corrupt);
  FailingAllocator noHeap;
  JsonDocument starved(&noHeap);
  EXPECT(ConfigLoad::classifyParse(deserializeJson(starved, "{\"kinds\":{\"a\":\"off\"}}")) == Status::NoMemory);

  if (failures) return 1;
  std::puts("config_load: ok");
  return 0;
}
