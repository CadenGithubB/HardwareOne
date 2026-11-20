// Drives the production SD-in-use registration, unmountSd/remountSd/formatSd
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
static bool gCliValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (gCliValidateOnly) return "VALID"; } while (0)
static String currentExecUser() { return String("admin"); }

enum { SYSEVT_SD_UNMOUNTED = 1, SYSEVT_STORAGE_FORMATTED = 2 };
static unsigned unmountedEvents = 0;
static void systemEventPost(int kind, const char* = nullptr, const char* = nullptr) {
  if (kind == SYSEVT_SD_UNMOUNTED) ++unmountedEvents;
}

// UP-06: handles kept open on the card (the system log) are closed while the
// mount still exists, i.e. before the driver's end() or format().
static unsigned handleReleases = 0;
static struct FakeSd {
  unsigned ends = 0, formats = 0;
  bool endOk = true;  // false: the driver keeps the card
  unsigned releasesSeenAtLastCall = 0;
  bool end() { ++ends; releasesSeenAtLastCall = handleReleases; return endOk; }
  bool format() { ++formats; releasesSeenAtLastCall = handleReleases; return true; }
} SD;

namespace VFS {
enum StorageType { INTERNAL = 0, SDCARD = 1, AUTO = 2 };
static bool gSdMounted = true;
static bool gSdWritable = true;
static uint32_t gSdMountGeneration = 0;
static unsigned mounts = 0;
static void invalidateStatsSnapshot(StorageType) {}
static bool probeSdWriteInternal() { return true; }
static bool mountOk = true;
static bool tryMountSd() { ++mounts; gSdMounted = mountOk; if (mountOk) ++gSdMountGeneration; return mountOk; }
// UP-06 notes the lifecycle code leaves for the card watch (System_VFS.cpp;
// the policy itself runs in test_sdmmc_lifecycle.cpp).
static void sdReleaseHandlesBeforeUnmount() { ++handleReleases; }
static bool removalAlreadyReported = false;   // the card was pulled before sdunmount
static unsigned explicitUnmountNotes = 0, userMountOk = 0, userMountFailed = 0;
static bool sdWatchNoteExplicitUnmount() { ++explicitUnmountNotes; return removalAlreadyReported; }
static void sdWatchNoteUserMount(bool ok) { ++(ok ? userMountOk : userMountFailed); }
bool isSdAvailable() { return gSdMounted; }
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
    CHECK(!VFS::unmountSd(), "unmountSD refuses while registered");
    CHECK(SD.ends == 0 && VFS::gSdMounted && unmountedEvents == 0, "the card stays mounted");
    CHECK(refusals == 1 && refusalsUnlocked == 0, "the refusal is decided under the filesystem lock");
    VFS::sdInUseEnd(kRecorder);
    CHECK(VFS::sdInUseOwner() == nullptr, "registration ended");
    gCliValidateOnly = true;
    CHECK(std::strcmp(cmd_sdunmount(String("")), "VALID") == 0 && SD.ends == 0 && VFS::gSdMounted,
          "a validate pass does not unmount");
    gCliValidateOnly = false;
    CHECK(handleReleases == 0 && VFS::explicitUnmountNotes == 0, "a refused unmount releases nothing and holds nothing");
    CHECK(std::strstr(cmd_sdunmount(String("")), "unmounted successfully"), "sdunmount works after release");
    CHECK(SD.ends == 1 && !VFS::gSdMounted && unmountedEvents == 1, "the card is unmounted");
    CHECK(handleReleases == 1 && SD.releasesSeenAtLastCall == 1, "open handles on the card are closed before the driver lets go");
    CHECK(VFS::explicitUnmountNotes == 1, "sdunmount holds the card watch");
    CHECK(std::strcmp(cmd_sdunmount(String("")), "Error: SD card is not mounted. Mount it with 'sdmount'.") == 0 && SD.ends == 1,
          "sdunmount with nothing mounted is an error with guidance");
    // A driver that does not let go is the one failure sdunmount reports.
    VFS::gSdMounted = true;
    SD.endOk = false;
    CHECK(std::strcmp(cmd_sdunmount(String("")),
                      "Error: SD driver did not release the card. Restart the device before removing the card.") == 0,
          "a driver that keeps the card is an error");
    CHECK(SD.ends == 2 && unmountedEvents == 1 && !VFS::gSdMounted,
          "the failed release posts no unmount event");
    SD.endOk = true;
    // UP-06: a card pulled before the command was reported by the card watch
    // when it left; releasing its stale mount is not reported a second time.
    VFS::gSdMounted = true;
    VFS::removalAlreadyReported = true;
    CHECK(std::strstr(cmd_sdunmount(String("")), "unmounted successfully"), "a stale mount is released");
    CHECK(SD.ends == 3 && unmountedEvents == 1 && VFS::explicitUnmountNotes == 3, "one removal, one event");
    VFS::removalAlreadyReported = false;
  } else if (name == "remount") {
    VFS::gSdMounted = false;
    gCliValidateOnly = true;
    CHECK(std::strcmp(cmd_sdmount(String("")), "VALID") == 0 && VFS::mounts == 0, "a validate pass does not mount");
    gCliValidateOnly = false;
    CHECK(VFS::sdInUseBegin(kRecorder), "recorder registers");
    CHECK(std::strstr(cmd_sdmount(String("")), "in use by video recording"), "sdmount names the holder");
    CHECK(!VFS::remountSd() && SD.ends == 0 && VFS::mounts == 0, "remountSD refuses while registered");
    VFS::sdInUseEnd(kRecorder);
    CHECK(VFS::userMountOk == 0 && VFS::userMountFailed == 0 && handleReleases == 0,
          "a refused or validated sdmount leaves the card watch alone");
    CHECK(std::strstr(cmd_sdmount(String("")), "mounted successfully"), "sdmount works after release");
    CHECK(VFS::mounts == 1 && VFS::gSdMounted, "the card is mounted");
    CHECK(VFS::userMountOk == 1 && VFS::userMountFailed == 0, "sdmount ends an sdunmount hold");
    CHECK(handleReleases == 1 && SD.releasesSeenAtLastCall == 1, "a remount closes open handles before the old mount ends");
    // A mount that fails is noted too: the card watch tries it again later.
    VFS::gSdMounted = false;
    VFS::mountOk = false;
    CHECK(std::strstr(cmd_sdmount(String("")), "Error: Failed to mount SD card"), "a failed sdmount says so");
    CHECK(VFS::userMountOk == 1 && VFS::userMountFailed == 1, "a failed sdmount is noted for the card watch");
    VFS::mountOk = true;
  } else if (name == "format") {
    CHECK(VFS::sdInUseBegin(kRecorder), "recorder registers");
    CHECK(std::strstr(cmd_sdformat(String("")), "sdformat confirm"), "sdformat without confirm names the command to run");
    CHECK(std::strcmp(cmd_sdformat(String("")), cmd_sdformat(String("confrim"))) == 0 &&
          std::strstr(cmd_sdformat(String("confirm now")), "not formatted") && SD.formats == 0,
          "anything but exactly confirm is refused and formats nothing");
    CHECK(std::strstr(cmd_sdformat(String("confirm")), "in use by video recording"), "sdformat names the holder");
    CHECK(!VFS::formatSd() && SD.formats == 0, "formatSD refuses while registered");
    VFS::sdInUseEnd(kRecorder);
    gCliValidateOnly = true;
    CHECK(std::strcmp(cmd_sdformat(String("confirm")), "VALID") == 0 && SD.formats == 0,
          "a validate pass does not format the card");
    gCliValidateOnly = false;
    CHECK(handleReleases == 0 && VFS::userMountOk == 0, "a refused or validated sdformat touches nothing");
    CHECK(std::strstr(cmd_sdformat(String("confirm")), "formatted successfully"), "sdformat works after release");
    CHECK(SD.formats == 1, "the card is formatted");
    CHECK(handleReleases == 1 && SD.releasesSeenAtLastCall == 1, "a format closes open handles before the volume is replaced");
    CHECK(VFS::userMountOk == 1, "a format ends an sdunmount hold: the card is mounted again");
  } else if (name == "slots") {
    static const char* const owners[] = {"a", "b", "c", "d"};
    CHECK(!VFS::sdInUseBegin(nullptr), "an unnamed registration is refused");
    for (const char* owner : owners) CHECK(VFS::sdInUseBegin(owner), "registration fits");
    CHECK(!VFS::sdInUseBegin("e"), "a full table refuses a fifth registration");
    VFS::sdInUseEnd("a");
    CHECK(VFS::sdInUseOwner() && std::strcmp(VFS::sdInUseOwner(), "a") != 0, "other holders remain");
    CHECK(!VFS::unmountSd(), "any remaining holder keeps the card mounted");
    VFS::sdInUseEnd("zz");  // not registered: no effect
    for (const char* owner : owners) VFS::sdInUseEnd(owner);
    CHECK(VFS::sdInUseOwner() == nullptr && VFS::unmountSd(), "all released: unmount proceeds");
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
