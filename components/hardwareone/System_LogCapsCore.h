// System_LogCapsCore.h - capped-log sizes by LittleFS size, and which capped
// log is written and rotated how (UP-03, S-8). Pure: no Arduino, no ESP-IDF,
// no allocation, so the host tests run it as it is.
//
// The capped logs under /system/sys_logs each pass a fixed cap to
// appendLineWithCap. Those caps add up to 2,823,442 bytes, more than the whole
// LittleFS partition of the 8 MB layouts (1856 and 2284 KiB). The writer
// therefore scales the cap it is given: all capped logs together use at most a
// quarter of the mounted LittleFS partition, today's caps are the upper bound,
// and each log keeps a floor of 4 KiB (eight logs at the floor are a quarter
// of the smallest table, 128 KiB; on that table the 100 KB reserve refuses
// log lines without a card anyway).
//
// The total comes from the partition table entry (read once at mount, no
// volume walk). An unknown total (0) leaves every cap as the caller gave it.
#ifndef SYSTEM_LOG_CAPS_CORE_H
#define SYSTEM_LOG_CAPS_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace logcaps {

// Today's caps: the upper bound of each log. The login and 256 KB values are
// the constants in System_Logging.h; the audit and crash-history values are
// the literals at their call sites (System_Utils.cpp, System_CrashRecord.cpp).
// test_log_caps.py adds up every appendLineWithCap call site and fails when
// the sum differs from kNominalTotal, so a new capped log cannot pass the
// quarter unnoticed.
constexpr size_t kLoginCap        = 696969;       // successful_login.log, failed_login.log (each)
constexpr size_t kI2cCap          = 64 * 1024;    // i2c_errors.log
constexpr size_t kErrorsCap       = 256 * 1024;   // errors.log
constexpr size_t kSystemEventsCap = 256 * 1024;   // system-events.log
constexpr size_t kEventsCap       = 256 * 1024;   // events.log
constexpr size_t kAuditCap        = 500 * 1024;   // command-audit.log
constexpr size_t kCrashHistoryCap = 64 * 1024;    // crash-history.log
constexpr size_t kLogCount        = 8;
static_assert(kI2cCap >= 4096 && kCrashHistoryCap >= 4096, "every nominal cap is at least the floor");
constexpr size_t kNominalTotal = 2 * kLoginCap + kI2cCap + kErrorsCap + kSystemEventsCap +
                                 kEventsCap + kAuditCap + kCrashHistoryCap;  // 2,823,442

// Share of the LittleFS partition all capped logs may use together.
constexpr size_t kShareDivisor = 4;
// No log is scaled below this (or below its own cap, when that is smaller).
constexpr size_t kFloorBytes = 4096;

// The cap a log with nominal cap `capBytes` gets on a LittleFS partition of
// `totalBytes`. Never above `capBytes`.
//
// Every log keeps its floor and the rest of the quarter is shared in
// proportion to what each log has above the floor:
//   cap = floor + (nominal - floor) * (quarter - 8 floors) / (nominal total - 8 floors)
// so the eight caps add up to the quarter (less rounding) for every partition
// of 128 KiB or more, and to the eight floors (32 KiB) below that.
constexpr size_t scaledCap(size_t capBytes, size_t totalBytes) {
  if (totalBytes == 0) return capBytes;
  const uint64_t budget = static_cast<uint64_t>(totalBytes) / kShareDivisor;
  if (budget >= kNominalTotal) return capBytes;
  const size_t floor = capBytes < kFloorBytes ? capBytes : kFloorBytes;
  const uint64_t floors = static_cast<uint64_t>(kLogCount) * kFloorBytes;
  if (budget <= floors) return floor;
  return floor + static_cast<size_t>(static_cast<uint64_t>(capBytes - floor) * (budget - floors) /
                                     (kNominalTotal - floors));
}

struct LogCaps {
  size_t login;         // each of the two login logs
  size_t i2c;
  size_t errors;
  size_t systemEvents;
  size_t events;
  size_t audit;
  size_t crashHistory;
  constexpr size_t total() const {
    return 2 * login + i2c + errors + systemEvents + events + audit + crashHistory;
  }
};

// Every capped log's cap on a LittleFS partition of `totalBytes`.
constexpr LogCaps logCapsFor(size_t totalBytes) {
  return LogCaps{scaledCap(kLoginCap, totalBytes),        scaledCap(kI2cCap, totalBytes),
                 scaledCap(kErrorsCap, totalBytes),       scaledCap(kSystemEventsCap, totalBytes),
                 scaledCap(kEventsCap, totalBytes),       scaledCap(kAuditCap, totalBytes),
                 scaledCap(kCrashHistoryCap, totalBytes)};
}

// ---------------------------------------------------------------------------
// The card (B11, 2026-11-22). Logs go to the card first when one takes writes
// (VFS::resolveLogPath) and are sealed there (System_SealedLog.h), which
// stores about 2.0x the plain bytes of a 60-byte line, 1.7x at 120 and 1.5x
// at 300. A capped log on the card therefore gets twice its nominal cap,
// counted in stored (sealed) bytes: about the nominal number of lines, and
// 5.6 MB for all eight. The flash share above does not apply to the card.
// ---------------------------------------------------------------------------
constexpr size_t kCardCapFactor = 2;
constexpr size_t cardCap(size_t capBytes) { return capBytes * kCardCapFactor; }
static_assert(cardCap(kNominalTotal) == 2 * 2823442u, "all eight logs on the card: 5,646,884 stored bytes");

// The card reserve: with less than this free the card counts as absent for
// log lines (they go to flash, as with no card), and it counts as usable
// again once twice this is free, so a card at the edge does not flip a log
// between the tiers line by line. Capture retention (System_LogRetentionCore.h)
// starts freeing card space long before: at 20 % free, capped at 64 MiB.
constexpr uint64_t kCardReserveBytes = 8ull * 1024 * 1024;
constexpr uint64_t kCardRearmBytes = 2 * kCardReserveBytes;
// Whether the card is full for log lines, given whether it was and its free
// bytes now.
constexpr bool cardFull(bool wasFull, uint64_t freeBytes) {
  return wasFull ? freeBytes < kCardRearmBytes : freeBytes < kCardReserveBytes;
}

// ---------------------------------------------------------------------------
// Per-log policy, by file name (the last path component), so the /sd mirror of
// a log follows the same rule as the log itself.
// ---------------------------------------------------------------------------

inline const char* baseName(const char* path) {
  if (!path) return "";
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// Written to the file at once, never held in the batch buffer: the login logs
// (security evidence) and the crash history (written once per crashed boot,
// and the next crash may be seconds away).
inline bool writesThrough(const char* path) {
  const char* name = baseName(path);
  return strcmp(name, "successful_login.log") == 0 || strcmp(name, "failed_login.log") == 0 ||
         strcmp(name, "crash-history.log") == 0;
}

// The logs a collector fetches through file catch-up (System_CatchupLedger:
// kFixedPaths). The ledger knows them by exact path and follows a head trim
// (catchupNoteTrim), so in a build that serves catch-up they keep the copy
// rotation: a rename to "<log>.1" would take every row the collector has not
// fetched out of its reach (the whitelist in System_ESPNow_CatchupCore.h does
// not name ".1" files).
inline bool followedByCatchup(const char* path) {
  const char* name = baseName(path);
  return strcmp(name, "events.log") == 0 || strcmp(name, "errors.log") == 0 ||
         strcmp(name, "crash-history.log") == 0;
}

// Two-generation logs: the current file is renamed to "<log>.1" once it holds
// half the cap, so the two files together stay within the cap.
constexpr size_t generationBytes(size_t capBytes) { return capBytes / 2; }

// A current file this far past its generation size was not left by this
// writer (which rotates within one batch of the size): it comes from older
// firmware with a larger cap. Its newest generation is copied to "<log>.1"
// once instead of renaming the whole file.
constexpr size_t kGenerationSlack = 4096;

// A rotation that copies data (the copy rotation of a followed log, or the
// one-time cut of a file older firmware left above its generation size) may
// run once boot is over. During boot it runs only for a file above the cap
// its caller names (`nominalCap`, the cap before scaling): that is where
// firmware before UP-03 rotated too, so boot never copies more than it did,
// and a device that restarts over and over still keeps every log within the
// old caps.
constexpr bool copyAllowedNow(bool bootOver, size_t fileBytes, size_t nominalCap) {
  return bootOver || fileBytes > nominalCap;
}

// ---------------------------------------------------------------------------
// Batching (S-8): lines of a log collect in one buffer and reach the file in
// one append.
// ---------------------------------------------------------------------------
constexpr size_t   kBatchCapacity   = 2560;  // bytes per log buffer
constexpr size_t   kBatchFlushBytes = 2048;  // pending bytes that flush at once
constexpr uint32_t kBatchDueMs      = 1500;  // oldest pending line: the log task flushes
constexpr uint32_t kBatchMaxAgeMs   = 2000;  // oldest pending line: the next appender flushes
constexpr uint32_t kBatchTickMs     = 500;   // the log task's wake while idle

// True when the oldest pending line (queued at `firstMs`) is `ageMs` old or
// older at `nowMs` (wrap-safe).
constexpr bool batchAged(uint32_t nowMs, uint32_t firstMs, uint32_t ageMs) {
  return static_cast<uint32_t>(nowMs - firstMs) >= ageMs;
}

}  // namespace logcaps

#endif  // SYSTEM_LOG_CAPS_CORE_H
