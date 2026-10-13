// Drives the production SD-in-use registration, unmountSD/remountSD/formatSD
// and the sdmount/sdunmount/sdformat commands from System_VFS.cpp against a
// fake SD driver. The lifecycle refusal must happen under the filesystem lock
// and before the driver is touched.
#include <Arduino.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

void webBatchTestStringAppend() {}

#define ENABLE_SD_CARD 1
#define ENABLE_SDMMC_CARD 1
#define EXT_RAM_BSS_ATTR

static int lockDepth = 0;
struct FsLockGuard {
  explicit FsLockGuard(const char*) { ++lockDepth; }
  ~FsLockGuard() { --lockDepth; }
  FsLockGuard(const FsLockGuard&) = delete;
  FsLockGuard& operator=(const FsLockGuard&) = delete;
};

static unsigned refusals = 0, refusalsUnlocked = 0;
static void hostWarn(const char* format, ...) {
  if (std::strstr(format, "refused")) {
    ++refusals;
    if (lockDepth == 0) ++refusalsUnlocked;
  }
}
static void hostLog(const char*, ...) {}
#define WARN_STORAGEF(...) hostWarn(__VA_ARGS__)
#define INFO_STORAGEF(...) hostLog(__VA_ARGS__)
static void logSystemEvent(const char*, const char*, ...) {}
static String currentExecUser() { return String("admin"); }

enum { SYSEVT_SD_UNMOUNTED = 1, SYSEVT_STORAGE_FORMATTED = 2 };
static unsigned unmountedEvents = 0;
static void systemEventPost(int kind, const char* = nullptr, const char* = nullptr) {
  if (kind == SYSEVT_SD_UNMOUNTED) ++unmountedEvents;
}

static struct FakeSd {
  unsigned ends = 0, formats = 0;
  bool end() { ++ends; return true; }
  bool format() { ++formats; return true; }
} SD;

namespace VFS {
enum StorageType { INTERNAL = 0, SDCARD = 1, AUTO = 2 };
static bool gSdMounted = true;
static bool gSdWritable = true;
static uint32_t gSdMountGeneration = 0;
static unsigned mounts = 0;
static void invalidateStatsSnapshot(StorageType) {}
static bool probeSDWriteInternal() { return true; }
static bool tryMountSD() { ++mounts; gSdMounted = true; ++gSdMountGeneration; return true; }
bool isSDAvailable() { return gSdMounted; }
bool getStats(StorageType, uint64_t& total, uint64_t& used, uint64_t& free) {
  total = used = free = 0;
  return false;
}

// INSERT_VFS
}  // namespace VFS

// INSERT_COMMANDS

#define CHECK(condition, what) \
  do { if (!(condition)) { std::fprintf(stderr, "FAIL: %s\n", what); return 1; } } while (0)

static const char* const kRecorder = "video recording";

static int run(const std::string& name) {
  if (name == "unmount") {
    CHECK(VFS::sdInUseBegin(kRecorder), "recorder registers");
    CHECK(std::strstr(cmd_sdunmount(String("")), "in use by video recording"), "sdunmount names the holder");
    CHECK(!VFS::unmountSD(), "unmountSD refuses while registered");
    CHECK(SD.ends == 0 && VFS::gSdMounted && unmountedEvents == 0, "the card stays mounted");
    CHECK(refusals == 1 && refusalsUnlocked == 0, "the refusal is decided under the filesystem lock");
    VFS::sdInUseEnd(kRecorder);
    CHECK(VFS::sdInUseOwner() == nullptr, "registration ended");
    CHECK(std::strstr(cmd_sdunmount(String("")), "unmounted successfully"), "sdunmount works after release");
    CHECK(SD.ends == 1 && !VFS::gSdMounted && unmountedEvents == 1, "the card is unmounted");
  } else if (name == "remount") {
    VFS::gSdMounted = false;
    CHECK(VFS::sdInUseBegin(kRecorder), "recorder registers");
    CHECK(std::strstr(cmd_sdmount(String("")), "in use by video recording"), "sdmount names the holder");
    CHECK(!VFS::remountSD() && SD.ends == 0 && VFS::mounts == 0, "remountSD refuses while registered");
    VFS::sdInUseEnd(kRecorder);
    CHECK(std::strstr(cmd_sdmount(String("")), "mounted successfully"), "sdmount works after release");
    CHECK(VFS::mounts == 1 && VFS::gSdMounted, "the card is mounted");
  } else if (name == "format") {
    CHECK(VFS::sdInUseBegin(kRecorder), "recorder registers");
    CHECK(std::strstr(cmd_sdformat(String("")), "sdformat confirm"), "the confirmation prompt is unchanged");
    CHECK(std::strstr(cmd_sdformat(String("confirm")), "in use by video recording"), "sdformat names the holder");
    CHECK(!VFS::formatSD() && SD.formats == 0, "formatSD refuses while registered");
    VFS::sdInUseEnd(kRecorder);
    CHECK(std::strstr(cmd_sdformat(String("confirm")), "formatted successfully"), "sdformat works after release");
    CHECK(SD.formats == 1, "the card is formatted");
  } else if (name == "slots") {
    static const char* const owners[] = {"a", "b", "c", "d"};
    CHECK(!VFS::sdInUseBegin(nullptr), "an unnamed registration is refused");
    for (const char* owner : owners) CHECK(VFS::sdInUseBegin(owner), "registration fits");
    CHECK(!VFS::sdInUseBegin("e"), "a full table refuses a fifth registration");
    VFS::sdInUseEnd("a");
    CHECK(VFS::sdInUseOwner() && std::strcmp(VFS::sdInUseOwner(), "a") != 0, "other holders remain");
    CHECK(!VFS::unmountSD(), "any remaining holder keeps the card mounted");
    VFS::sdInUseEnd("zz");  // not registered: no effect
    for (const char* owner : owners) VFS::sdInUseEnd(owner);
    CHECK(VFS::sdInUseOwner() == nullptr && VFS::unmountSD(), "all released: unmount proceeds");
  } else {
    CHECK(false, "known scenario");
  }
  std::puts("PASS");
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
    return 2;
  }
  return run(argv[1]);
}
