// Executes the production LittleFS log-overflow latch from System_VFS.cpp
// (resolveLogPath, resolveOverflowPath and the free-space cache they share)
// over a controlled clock, LittleFS size report and SD state. The latch
// must refuse rather than spend the reserve, need a WRITABLE card, and re-arm
// once space returns.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } \
  ++checks; } while (0)

static unsigned long nowMs = 1000;
static unsigned long millis() { return nowMs; }
static size_t lfsTotal = 2 * 1024 * 1024, lfsUsed = 0;
static int traverses = 0, writableProbes = 0, lowSpacePosts = 0, warnings = 0, sdFailureNotes = 0;
static bool sdMounted = false, sdWritableNow = false;
static std::vector<std::string> eventLines;
static constexpr const char* kLittleFsPartitionLabel = "littlefs";
static int esp_littlefs_info(const char* label, size_t* total, size_t* used) {
  CHECK(std::strcmp(label, kLittleFsPartitionLabel) == 0);
  ++traverses; *total = lfsTotal; *used = lfsUsed; return 0;
}
struct FsLockGuard { explicit FsLockGuard(const char*) {} };
enum { SYSEVT_FS_LOW_SPACE = 7 };
static void systemEventPost(uint8_t kind, const char*, const char*) { CHECK(kind == SYSEVT_FS_LOW_SPACE); ++lowSpacePosts; }
static void logSystemEvent(const char*, const char* fmt, ...) {
  char line[256]; va_list args; va_start(args, fmt); std::vsnprintf(line, sizeof(line), fmt, args); va_end(args);
  eventLines.push_back(line);
}
#define WARN_SYSTEMF(...) (++warnings)

namespace VFS {
enum class LogTier : uint8_t { Primary, Overflow, Refused };
static bool isSDAvailable() { return sdMounted; }
static bool isSDWritable() { ++writableProbes; return sdMounted && sdWritableNow; }
static void noteSDWriteFailure(const char*) { ++sdFailureNotes; }
// INSERT_PRODUCTION
}  // namespace VFS

using VFS::LogTier;
static void setFree(size_t bytes) { lfsUsed = lfsTotal - bytes; }
static LogTier resolve(std::string& out, const char* path = "/system/sys_logs/events.log", size_t reserve = 1024) {
  char dest[128];
  const LogTier tier = VFS::resolveLogPath(path, reserve, dest, sizeof(dest));
  out = dest;
  return tier;
}
static void advance(unsigned long ms) { nowMs += ms; }

int main() {
  std::string out;
  const size_t reserve = VFS::logReserveBytes();
  CHECK(reserve == 100 * 1024);

  // Plenty of space: primary path, no latch.
  setFree(1024 * 1024);
  CHECK(resolve(out) == LogTier::Primary && out == "/system/sys_logs/events.log");
  CHECK(!VFS::isLogOverflowActive());

  // Below the reserve with no card: latched, and a writer is told Refused
  // (the primary path is still named for readers/recovery). Before the reserve fix this
  // answer was "primary, not overflow" and the line ate the reserve.
  setFree(60 * 1024); advance(16000);  // >800 KB free was cached for 15 s
  CHECK(resolve(out) == LogTier::Refused && out == "/system/sys_logs/events.log");
  CHECK(VFS::isLogOverflowActive());
  CHECK(lowSpacePosts == 1 && warnings == 1 && eventLines.size() == 1);
  CHECK(eventLines.back().find("then are dropped") != std::string::npos);
  char dest[128];
  CHECK(!VFS::resolveOverflowPath("/system/sys_logs/events.log", 0, dest, sizeof(dest)));
  // An explicit /sd path never touches the reserve and is not refused.
  CHECK(resolve(out, "/sd/system/sys_logs/events.log") == LogTier::Primary && out == "/sd/system/sys_logs/events.log");

  // A writable card takes the overflow — after the failed-probe backoff.
  sdMounted = sdWritableNow = true;
  const int probesBefore = writableProbes;
  CHECK(resolve(out) == LogTier::Refused);  // still inside the 10 s backoff
  CHECK(writableProbes == probesBefore);
  advance(10000);
  CHECK(resolve(out) == LogTier::Overflow && out == "/sd/system/sys_logs/events.log");
  CHECK(VFS::resolveOverflowPath("/system/sys_logs/events.log", 0, dest, sizeof(dest)) &&
        std::strcmp(dest, "/sd/system/sys_logs/events.log") == 0);

  // Card yanked but still "mounted" (SPI / detect-less SDMMC): writability,
  // not mount state, decides. Before the reserve fix the latch kept routing here.
  sdWritableNow = false;
  CHECK(resolve(out) == LogTier::Refused && out == "/system/sys_logs/events.log");
  const int probesAfterYank = writableProbes;
  for (int i = 0; i < 50; ++i) CHECK(resolve(out) == LogTier::Refused);
  CHECK(writableProbes == probesAfterYank);  // no write probe per logged line
  sdWritableNow = true; advance(10000);
  CHECK(resolve(out) == LogTier::Overflow);  // card back: overflow resumes

  // Hysteresis: free back above the trip level but not a reserve's width
  // above it keeps the latch, so logs stay on the card.
  setFree(150 * 1024); advance(30000);
  CHECK(resolve(out) == LogTier::Overflow && VFS::isLogOverflowActive());
  // Latched, card gone, but free space above the 100 KB floor: the line still
  // lands on flash. Refusing here would drop audit and event lines with the
  // reserve intact.
  sdMounted = sdWritableNow = false;
  CHECK(resolve(out) == LogTier::Primary && out == "/system/sys_logs/events.log" && VFS::isLogOverflowActive());

  // Re-arm: free space returned past trip level + reserve. The latch clears;
  // before the reserve fix it held until reboot.
  setFree(220 * 1024); advance(5000);
  CHECK(resolve(out) == LogTier::Primary && VFS::isLogOverflowActive());  // re-arm check runs at most every 30 s
  advance(30000);
  const int traversesBefore = traverses;
  CHECK(resolve(out) == LogTier::Primary && out == "/system/sys_logs/events.log");
  CHECK(!VFS::isLogOverflowActive() && traverses == traversesBefore + 1);
  CHECK(eventLines.back().find("re-armed") != std::string::npos);

  // A second episode latches again; the once-per-boot event is not re-posted
  // but the [EVENT] line records it.
  setFree(50 * 1024); advance(20000);
  CHECK(resolve(out) == LogTier::Refused && VFS::isLogOverflowActive());
  CHECK(lowSpacePosts == 1 && warnings == 1 && eventLines.size() == 3);

  // A caller with a large margin (the sensor log asks for its whole rotation
  // size) trips the latch far above the floor. Refusal is decided against the
  // 100 KB floor, not that margin: with no card the capped logs keep writing
  // to flash, and only the floor itself refuses them.
  setFree(800 * 1024); advance(30000);
  CHECK(resolve(out) == LogTier::Primary && !VFS::isLogOverflowActive());
  setFree(400 * 1024); advance(20000);
  CHECK(resolve(out, "/logging_captures/a.csv", 512 * 1024) == LogTier::Primary && VFS::isLogOverflowActive());
  CHECK(resolve(out) == LogTier::Primary && out == "/system/sys_logs/events.log");
  setFree(580 * 1024); advance(30000);
  CHECK(resolve(out) == LogTier::Primary && VFS::isLogOverflowActive());  // 580 KB < 512 KB + 100 KB: still latched
  setFree(90 * 1024); advance(6000);
  CHECK(resolve(out) == LogTier::Refused);  // the floor itself
  CHECK(resolve(out, "/logging_captures/a.csv", 512 * 1024) == LogTier::Refused);
  setFree(700 * 1024); advance(30000);
  CHECK(resolve(out) == LogTier::Primary && !VFS::isLogOverflowActive());

  // A log append failed on a card whose write probe still passes (damaged log
  // tree, read-only file). The overflow path must keep off the card for a
  // while rather than probe it again on the next line: each re-probe flipped
  // the card writable, the next append failed again and the pair of SD
  // events went round the events log.
  sdMounted = sdWritableNow = true;
  setFree(50 * 1024); advance(20000);
  CHECK(resolve(out) == LogTier::Overflow && VFS::isLogOverflowActive());
  VFS::noteLogOverflowSdFailure("log.append");
  CHECK(sdFailureNotes == 1);
  const int probesAtFailure = writableProbes;
  CHECK(resolve(out) == LogTier::Refused);  // below the floor: a counted drop, not the card
  for (int i = 0; i < 50; ++i) CHECK(resolve(out) != LogTier::Overflow);
  CHECK(writableProbes == probesAtFailure);
  setFree(150 * 1024); advance(3000);
  CHECK(resolve(out) == LogTier::Primary && out == "/system/sys_logs/events.log");  // above the floor: flash
  advance(10000);
  CHECK(resolve(out) == LogTier::Primary && writableProbes == probesAtFailure);  // longer than a failed probe's backoff
  advance(50000);
  CHECK(resolve(out) == LogTier::Overflow && writableProbes == probesAtFailure + 1);

  VFS::noteLogReserveDrop(); VFS::noteLogReserveDrop();
  CHECK(VFS::logReserveDrops() == 2);
  std::fprintf(stderr, "checks=%d\n", checks);
  return 0;
}
