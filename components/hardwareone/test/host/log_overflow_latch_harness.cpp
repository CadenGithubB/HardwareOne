// Executes the production LittleFS log-overflow latch from System_VFS.cpp
// (resolveLogPath, resolveOverflowPath and the free-space cache they share)
// over a controlled clock, LittleFS size report and SD state. G-39: the latch
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
using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0;
static bool filesystemReady = true;
static esp_err_t esp_littlefs_info(const char* label, size_t* total, size_t* used) {
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
// FILE-3: the bulk-write admission lives in the same region.
static int storageWarnings = 0;
#define WARN_STORAGEF(...) (++storageWarnings)
#define EXT_RAM_BSS_ATTR
// cmd_exec's queue: a refusal on espnow_task asks it for a fresh free figure.
static std::vector<std::pair<void (*)(void*), void*>> deferredJobs;
static bool cmdExecQueueFull = false;
static bool submitDeferredToCmdExec(void (*fn)(void*), void* arg) {
  if (cmdExecQueueFull) return false;
  deferredJobs.push_back({fn, arg});
  return true;
}
static void runCmdExec() {
  const auto jobs = deferredJobs;
  deferredJobs.clear();
  for (const auto& job : jobs) job.first(job.second);
}

namespace VFS {
enum class LogTier : uint8_t { Primary, Overflow, Refused };
static bool isSdAvailable() { return sdMounted; }
static bool isSdWritable() { ++writableProbes; return sdMounted && sdWritableNow; }
static void noteSdWriteFailure(const char*) { ++sdFailureNotes; }
// Each free-space walk also hands its reading to the presentation snapshot
// (the real cache and its fencing run in the storage_stats test).
enum StorageType { INTERNAL, SDCARD };
enum class BulkFigure : uint8_t { Peek, Refresh, RefreshIfShort };  // System_VFS.h
bool admitBulkWrite(StorageType tier, size_t bytes, const char* why,
                    BulkFigure figure = BulkFigure::Peek, size_t* roomBytes = nullptr,
                    size_t wantBytes = 0);  // System_VFS.h, with its defaults
struct CapacitySnapshot { uint64_t totalBytes, usedBytes, freeBytes; uint32_t sampledAtMs; };
static int snapshotPublishes = 0;
static uint32_t capacitySnapshotGeneration(StorageType) { return 7; }
static void publishCapacitySnapshot(StorageType type, uint32_t generation, const CapacitySnapshot& sample) {
  CHECK(type == INTERNAL && generation == 7);
  CHECK(sample.totalBytes == lfsTotal && sample.usedBytes == lfsUsed && sample.freeBytes == lfsTotal - lfsUsed);
  ++snapshotPublishes;
}
// INSERT_PRODUCTION
}  // namespace VFS

using VFS::LogTier;

// FILE-3: the microphone recorder's size helpers (System_Microphone.cpp).
// INSERT_MIC_ADMISSION
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
  // FILE-3: nothing sampled yet (the first moments of a boot): the peek knows
  // nothing, walks nothing, and an admission on it passes.
  {
    size_t room = 0;
    CHECK(VFS::peekLittleFsFree() == SIZE_MAX && traverses == 0);
    CHECK(VFS::admitBulkWrite(VFS::INTERNAL, 50u * 1024u * 1024u, "t", VFS::BulkFigure::Peek, &room) && room == SIZE_MAX);
    CHECK(traverses == 0 && VFS::bulkWriteRefusals() == 0);
  }

  // Plenty of space: primary path, no latch.
  setFree(1024 * 1024);
  CHECK(resolve(out) == LogTier::Primary && out == "/system/sys_logs/events.log");
  CHECK(!VFS::isLogOverflowActive());

  // Below the reserve with no card: latched, and a writer is told Refused
  // (the primary path is still named for readers/recovery). Before G-39 this
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
  // not mount state, decides. Before G-39 the latch kept routing here.
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
  // before G-39 it held until reboot.
  setFree(220 * 1024); advance(5000);
  CHECK(resolve(out) == LogTier::Primary && VFS::isLogOverflowActive());  // re-arm check runs at most every 30 s
  advance(30000);
  const int traversesBefore = traverses;
  CHECK(resolve(out) == LogTier::Primary && out == "/system/sys_logs/events.log");
  CHECK(!VFS::isLogOverflowActive() && traverses == traversesBefore + 1);
  CHECK(eventLines.back().find("re-armed") != std::string::npos);

  // A second episode latches again and warns again (B10: once per low
  // episode, not once per boot), with its own [EVENT] line.
  setFree(50 * 1024); advance(20000);
  CHECK(resolve(out) == LogTier::Refused && VFS::isLogOverflowActive());
  CHECK(lowSpacePosts == 2 && warnings == 2 && eventLines.size() == 3);
  // Still inside that episode: no further post.
  advance(6000);
  CHECK(resolve(out) == LogTier::Refused && lowSpacePosts == 2 && warnings == 2);

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

  // ---- FILE-3: bulk-write admission over the same free figure ----
  using VFS::BulkFigure;
  const size_t need = micRecordingBytes(16000, kRecMinAdmitSec);  // five seconds of 16 kHz audio
  CHECK(need == 160044);
  setFree(reserve + need); advance(20000);
  int walks = traverses;
  CHECK(VFS::getCachedLittleFsFree() == reserve + need && traverses == walks + 1);
  walks = traverses;
  size_t room = 0;
  // Free is reserve + N: N bytes are admitted, N + 1 are refused. Both
  // writers ask this one function (the recorder for 5 s, ESP-NOW receive for
  // the file and what other transfers still have to write).
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, need, "microphone recording", BulkFigure::Peek, &room) && room == need);
  CHECK(VFS::bulkWriteRefusals() == 0 && storageWarnings == 0);
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, need + 1, "ESP-NOW file receive", BulkFigure::Peek, &room) && room == need);
  CHECK(VFS::bulkWriteRefusals() == 1 && storageWarnings == 1);
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, need + 1, "ESP-NOW file receive") && storageWarnings == 1);  // one line per 10 s
  advance(10000);
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, need + 1, "ESP-NOW file receive") && storageWarnings == 2);
  CHECK(VFS::bulkWriteRefusals() == 3);
  // The recorder: 5 s fit, so it starts, limited to what is above the reserve
  // less the header and one capture chunk.
  CHECK(micStorageSampleLimit(room) == (need - 44 - RECORDING_CHUNK_SIZE) / 2);
  CHECK(micStorageSampleLimit(SIZE_MAX) == 0);                     // a card, or no figure: no limit
  CHECK(micStorageSampleLimit(0) == 1 && micStorageSampleLimit(44 + RECORDING_CHUNK_SIZE) == 1);
  for (size_t r : {size_t(160044), size_t(500000), size_t(1920044), size_t(4000000)}) {
    CHECK(44 + 2 * static_cast<size_t>(micStorageSampleLimit(r)) + RECORDING_CHUNK_SIZE <= r);
  }
  CHECK(micStorageSampleLimit(micRecordingBytes(16000, 60) + RECORDING_CHUNK_SIZE) == 16000u * 60u);  // a full recording fits
  // The stored figure is never refreshed by a peek, however old it is: no walk.
  setFree(reserve + 10); advance(600000);
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, need, "t", BulkFigure::Peek) && traverses == walks);
  CHECK(VFS::peekLittleFsFree() == reserve + need && traverses == walks);
  // A refresh walks when the figure is stale and decides on the new one.
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 11, "t", BulkFigure::Refresh, &room) && room == 10 && traverses == walks + 1);
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, 10, "t", BulkFigure::Refresh) && traverses == walks + 1);  // fresh: no second walk
  CHECK(VFS::peekLittleFsFree() == reserve + 10);
  // A card is not LittleFS: it passes whatever the size.
  CHECK(VFS::admitBulkWrite(VFS::SDCARD, SIZE_MAX, "t", BulkFigure::Peek, &room) && room == SIZE_MAX && traverses == walks + 1);
  // Below the reserve the room is zero.
  setFree(reserve - 1); advance(20000);
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 1, "t", BulkFigure::Refresh, &room) && room == 0);
  // An invalidation (every remove and rename) keeps the last reading for the peek.
  walks = traverses;
  VFS::invalidateLittleFsFreeCache();
  CHECK(VFS::peekLittleFsFree() == reserve - 1 && traverses == walks);
  CHECK(deferredJobs.empty());
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 1, "t") && traverses == walks);
  CHECK(deferredJobs.size() == 1);  // refused on a reading from before a remove: a fresh one is asked of cmd_exec
  setFree(reserve + 5000);
  runCmdExec();
  CHECK(traverses == walks + 1 && deferredJobs.empty());
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, 5000, "t", BulkFigure::Refresh) && traverses == walks + 1);
  CHECK(VFS::peekLittleFsFree() == reserve + 5000);

  // ---- Admissions in a row on one stored figure (the figure is read again
  // only by a log write: every 2 to 15 s with lines flowing, never on a
  // silent device). What a writer put on flash is taken off the figure the
  // next admission sees. Before, three 100 KB files were each accepted
  // against the same 300 KB reading and the reserve was gone.
  const size_t kFile = 100 * 1024;
  lfsTotal = 8 * 1024 * 1024;  // room for the larger figures below
  setFree(reserve + 3 * kFile); advance(20000);
  CHECK(VFS::getCachedLittleFsFree() == reserve + 3 * kFile);
  walks = traverses;
  const uint32_t refusedBefore = VFS::bulkWriteRefusals();
  CHECK(deferredJobs.empty());
  for (int i = 0; i < 3; ++i) {
    CHECK(VFS::admitBulkWrite(VFS::INTERNAL, kFile, "ESP-NOW file receive", BulkFigure::Peek, &room));
    CHECK(room == (3 - i) * kFile);
    lfsUsed += kFile;                       // the file is written...
    VFS::noteBulkWriteTaken(kFile);         // ...and its writer says so
  }
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 1, "ESP-NOW file receive", BulkFigure::Peek, &room) && room == 0);
  CHECK(traverses == walks && VFS::peekLittleFsFree() == reserve + 3 * kFile);  // no walk; the reading itself is as it was
  CHECK(VFS::bulkWriteRefusals() == refusedBefore + 1);
  // That refusal was decided on an estimate, and the caller could not wait:
  // cmd_exec is asked for a fresh reading, once.
  CHECK(deferredJobs.size() == 1);
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 1, "ESP-NOW file receive") && deferredJobs.size() == 1);
  runCmdExec();
  CHECK(traverses == walks + 1 && VFS::peekLittleFsFree() == reserve);
  // Decided on the volume as it is now: still no room, and no further walk is asked for.
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 1, "ESP-NOW file receive", BulkFigure::Peek, &room) && room == 0);
  CHECK(deferredJobs.empty() && traverses == walks + 1);

  // A writer that took more than it used gives the rest back (a recording
  // that was stopped early).
  setFree(reserve + 500000); advance(20000);
  CHECK(VFS::getCachedLittleFsFree() == reserve + 500000);
  walks = traverses;
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, need, "microphone recording", BulkFigure::RefreshIfShort, &room, 400000) &&
        room == 500000 && traverses == walks);          // a true reading with room for all that is wanted: no walk
  VFS::noteBulkWriteTaken(400000);
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 100001, "ESP-NOW file receive", BulkFigure::Peek, &room) && room == 100000);
  runCmdExec();                                          // (the fresh reading that refusal asked for)
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, need, "t", BulkFigure::Peek, &room) && room == 500000);
  VFS::noteBulkWriteTaken(400000);
  VFS::noteBulkWriteReturned(250000);                    // 150 000 bytes were recorded
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, 350000, "t", BulkFigure::Peek, &room) && room == 350000);
  VFS::noteBulkWriteReturned(SIZE_MAX);                  // never below zero
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, 500000, "t", BulkFigure::Peek, &room) && room == 500000);
  // A new reading of the volume (a log write's refresh) already shows what was
  // written: the count starts over.
  VFS::noteBulkWriteTaken(150000);
  lfsUsed += 150000; advance(20000);
  CHECK(VFS::getCachedLittleFsFree() == reserve + 350000);
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, 350000, "t", BulkFigure::Peek, &room) && room == 350000);

  // ---- A figure from before a delete. The user removed files to make room
  // (every remove invalidates the reading; the peek keeps the old, low one).
  setFree(reserve + 1000); advance(20000);
  CHECK(VFS::getCachedLittleFsFree() == reserve + 1000);
  setFree(reserve + 2000000);                            // the delete...
  VFS::invalidateLittleFsFreeCache();                    // ...as VFS::remove notes it
  walks = traverses;
  // espnow_task may not walk: refused on the old figure, and a fresh reading is asked for.
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, need, "ESP-NOW file receive", BulkFigure::Peek, &room) && room == 1000);
  CHECK(traverses == walks && deferredJobs.size() == 1);
  runCmdExec();                                          // the queued reading runs: one walk
  CHECK(traverses == walks + 1 && VFS::peekLittleFsFree() == reserve + 2000000);
  setFree(reserve + 1000); advance(20000);               // (again, from the top, for the recorder)
  CHECK(VFS::getCachedLittleFsFree() == reserve + 1000);
  setFree(reserve + 2000000);
  VFS::invalidateLittleFsFreeCache();
  walks = traverses;
  // The recorder may wait when the answer would be no: it reads the volume and starts.
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, need, "microphone recording", BulkFigure::RefreshIfShort, &room,
                            micRecordingBytes(16000, 60)) && room == 2000000 && traverses == walks + 1);
  // The same when the old figure would admit five seconds but cut the recording short.
  setFree(reserve + 200000); advance(20000);
  CHECK(VFS::getCachedLittleFsFree() == reserve + 200000);
  setFree(reserve + 3000000);
  VFS::invalidateLittleFsFreeCache();
  walks = traverses;
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, need, "microphone recording", BulkFigure::RefreshIfShort, &room,
                            micRecordingBytes(16000, 60)) && room == 3000000 && traverses == walks + 1);
  CHECK(micStorageSampleLimit(room) >= 16000u * 60u);    // a whole recording, not 200 000 bytes of one
  // A true reading that is short is not walked again: the recording starts at once, limited.
  setFree(reserve + 200000); advance(20000);
  CHECK(VFS::getCachedLittleFsFree() == reserve + 200000);
  walks = traverses;
  advance(60000);                                        // however old: only an estimate is read again
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, need, "microphone recording", BulkFigure::RefreshIfShort, &room,
                            micRecordingBytes(16000, 60)) && room == 200000 && traverses == walks);
  // A Refresh that would refuse on a figure writers took from reads the volume first.
  setFree(reserve + 300000); advance(20000);
  CHECK(VFS::getCachedLittleFsFree() == reserve + 300000);
  VFS::noteBulkWriteTaken(250000);                       // claimed, and then not written after all
  walks = traverses;
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, 200000, "ESP-NOW file receive", BulkFigure::Refresh, &room) &&
        room == 300000 && traverses == walks + 1);
  // The command queue is full: the refusal stands, nothing is left marked as queued.
  setFree(reserve + 10); advance(20000);
  CHECK(VFS::getCachedLittleFsFree() == reserve + 10);
  VFS::invalidateLittleFsFreeCache();
  cmdExecQueueFull = true;
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 11, "ESP-NOW file receive") && deferredJobs.empty());
  cmdExecQueueFull = false;
  CHECK(!VFS::admitBulkWrite(VFS::INTERNAL, 11, "ESP-NOW file receive") && deferredJobs.size() == 1);
  runCmdExec();
  CHECK(VFS::admitBulkWrite(VFS::INTERNAL, 10, "ESP-NOW file receive", BulkFigure::Peek, &room) && room == 10);
  CHECK(deferredJobs.empty());
  CHECK(traverses > 0 && VFS::snapshotPublishes == traverses);
  std::fprintf(stderr, "checks=%d\n", checks);
  return 0;
}
