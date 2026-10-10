#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "System_VFSCapacityCache.h"

static void check(bool value, const std::string& why) {
  if (!value) throw std::runtime_error(why);
}
class String {
 public:
  String() = default;
  String(const char* value) : text_(value ? value : "") {}
  size_t length() const { return text_.length(); }
  const char* c_str() const { return text_.c_str(); }
  void trim() {
    while (!text_.empty() && std::isspace(static_cast<unsigned char>(text_.front()))) text_.erase(0, 1);
    while (!text_.empty() && std::isspace(static_cast<unsigned char>(text_.back()))) text_.pop_back();
  }
  void reserve(size_t bytes) { text_.reserve(bytes); }
  bool startsWith(const char* prefix) const { return text_.rfind(prefix, 0) == 0; }
  bool endsWith(const char* suffix) const {
    const size_t size = std::strlen(suffix);
    return text_.size() >= size && text_.compare(text_.size() - size, size, suffix) == 0;
  }
  int indexOf(const char* needle) const {
    const auto pos = text_.find(needle);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  void replace(const char* before, const char* after) {
    size_t pos = 0;
    while ((pos = text_.find(before, pos)) != std::string::npos) {
      text_.replace(pos, std::strlen(before), after); pos += std::strlen(after);
    }
  }
  void remove(size_t pos) { text_.erase(pos); }
  String& operator=(char ch) { text_ = ch; return *this; }
  String& operator+=(const String& other) { text_ += other.text_; return *this; }
  bool operator==(const char* other) const { return text_ == other; }
  bool operator!=(const char* other) const { return text_ != other; }
 private:
  std::string text_;
};

using TickType_t = uint32_t;
using SemaphoreHandle_t = int*;
static int semaphore;
static SemaphoreHandle_t gFsMutex = &semaphore;
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
static constexpr int pdTRUE = 1;
static bool ownsFs = false;
static unsigned takes = 0, gives = 0, criticalDepth = 0;
static bool isHeldByCurrentTask(SemaphoreHandle_t) { return ownsFs; }
static int xSemaphoreTake(SemaphoreHandle_t, TickType_t) {
  check(!ownsFs, "only outer guard takes the non-recursive FS mutex");
  ownsFs = true; ++takes; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t) {
  check(ownsFs, "only owner releases the FS mutex"); ownsFs = false; ++gives;
}
// INSERT_FS_GUARD
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
static void portENTER_CRITICAL(portMUX_TYPE*) { check(criticalDepth == 0, "cache critical sections do not nest"); ++criticalDepth; }
static void portEXIT_CRITICAL(portMUX_TYPE*) { check(criticalDepth == 1, "cache critical section is balanced"); --criticalDepth; }
static uint32_t nowMs = 100;
static uint32_t millis() { return nowMs; }
static bool filesystemReady = true, sdMounted = true;
static uint64_t lfsTotal = 1000, lfsUsed = 250, sdTotal = 2000, sdUsed = 750;
static unsigned lfsReads = 0, sdTotalReads = 0, sdUsedReads = 0;
using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0;
static esp_err_t infoError = ESP_OK;
static bool infoWritesOutputsOnError = false;
static constexpr const char* kLittleFsPartitionLabel = "littlefs";
static std::function<void()> duringSample;
static void driverAccess() {
  check(ownsFs, "driver sampling requires the FS mutex");
  check(criticalDepth == 0, "slow driver sampling must stay outside cache critical sections");
}
static esp_err_t esp_littlefs_info(const char* label, size_t* total, size_t* used) {
  driverAccess(); ++lfsReads;
  check(std::strcmp(label, kLittleFsPartitionLabel) == 0, "query uses selected LittleFS partition");
  if (duringSample) { auto callback = std::move(duringSample); duringSample = {}; callback(); }
  if (infoError == ESP_OK || infoWritesOutputsOnError) { *total = lfsTotal; *used = lfsUsed; }
  return infoError;
}
struct SDBackend {
  uint64_t totalBytes() { driverAccess(); ++sdTotalReads; return sdTotal; }
  uint64_t usedBytes() { driverAccess(); ++sdUsedReads; return sdUsed; }
} SD;
namespace VFS {
// INSERT_STORAGE_TYPE
static bool isSDAvailable() { return sdMounted; }
// INSERT_VFS
}
// INSERT_JSON_BUILDER

static void balanced() {
  check(!ownsFs && takes == gives && criticalDepth == 0, "FS and cache locks released");
}
static void reset() {
  // Reset boundary state even after an assertion so every regression is reported.
  ownsFs = false; takes = gives = criticalDepth = 0;
  filesystemReady = sdMounted = true;
  lfsTotal = 1000; lfsUsed = 250; sdTotal = 2000; sdUsed = 750;
  infoError = ESP_OK; infoWritesOutputsOnError = false; nowMs = 100; duringSample = {};
  lfsReads = sdTotalReads = sdUsedReads = 0;
  VFS::gCapacitySnapshots = VFS::detail::CapacitySnapshotCache{};
}
static std::string json(const char* path) {
  char buffer[256] = {};
  buildFilesStatsJson(path, buffer, sizeof(buffer));
  return buffer;
}
static constexpr const char* unavailableJson = "{\"success\":false,\"error\":\"Storage statistics unavailable\"}";
static void expectFailure(VFS::StorageType tier, bool snapshot) {
  uint64_t total = 111, used = 222, free = 333;
  VFS::CapacitySnapshot out{111, 222, 333, 444};
  const bool ok = snapshot ? VFS::getStatsSnapshot(tier, out, 0) : VFS::getStats(tier, total, used, free);
  check(!ok, "unavailable/failed sample must return false");
  check(total == 111 && used == 222 && free == 333, "failed fresh query leaves outputs unchanged");
  check(out.totalBytes == 111 && out.usedBytes == 222 && out.freeBytes == 333 && out.sampledAtMs == 444,
        "failed snapshot leaves output unchanged");
  VFS::CapacitySnapshot cached{};
  check(!VFS::readCapacitySnapshot(tier, 5000, cached), "failed sample must not seed cache");
}

int main(int argc, char** argv) {
  unsigned passed = 0, failed = 0;
  const bool regressionOnly = argc > 1 && std::string(argv[1]) == "regression";
  auto run = [&](const std::string& name, const std::function<void()>& test) {
    reset();
    try { test(); balanced(); ++passed; std::printf("PASS %s\n", name.c_str()); }
    catch (const std::exception& error) { ++failed; std::printf("FAIL %s: %s\n", name.c_str(), error.what()); }
  };
  for (const bool snapshot : {false, true}) {
    const std::string api = snapshot ? "snapshot" : "fresh";
    run(api + " LittleFS driver failure", [&] { infoError = -1; expectFailure(VFS::INTERNAL, snapshot); });
    run(api + " LittleFS driver failure with nonzero outputs", [&] {
      infoError = -1; infoWritesOutputsOnError = true; expectFailure(VFS::INTERNAL, snapshot);
    });
    run(api + " SD unusable zero capacity", [&] { sdTotal = 0; expectFailure(VFS::SDCARD, snapshot); });
    run(api + " LittleFS unusable zero capacity", [&] { lfsTotal = 0; expectFailure(VFS::INTERNAL, snapshot); });
  }
  run("JSON unavailable internal storage", [] { filesystemReady = false; const auto out = json("/"); check(out == unavailableJson, out); });
  run("JSON failed LittleFS sample", [] { infoError = -1; const auto out = json("/notes"); check(out == unavailableJson, out); });
  run("JSON failed SD sample", [] { sdTotal = 0; const auto out = json("/sd"); check(out == unavailableJson, out); });
  run("JSON zero-capacity LittleFS sample", [] { lfsTotal = 0; const auto out = json("/"); check(out == unavailableJson, out); });
  if (!regressionOnly) {
    for (const bool snapshot : {false, true}) {
      const std::string api = snapshot ? "snapshot" : "fresh";
      run(api + " unmounted internal storage", [&] {
        filesystemReady = false; expectFailure(VFS::INTERNAL, snapshot); check(lfsReads == 0, "unmounted LittleFS is not queried");
      });
      run(api + " unmounted SD", [&] {
        sdMounted = false; expectFailure(VFS::SDCARD, snapshot); check(sdTotalReads == 0 && sdUsedReads == 0, "unmounted SD is not queried");
      });
    }
    run("fresh query takes one LittleFS traversal and seeds cache", [] {
      uint64_t total = 0, used = 0, free = 0;
      check(VFS::getStats(VFS::INTERNAL, total, used, free), "fresh success");
      check(total == 1000 && used == 250 && free == 750 && lfsReads == 1, "one consistent total/used pair");
      VFS::CapacitySnapshot out{}; nowMs += 10;
      check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 5000), "seeded cache hit");
      check(out.totalBytes == total && out.usedBytes == used && out.freeBytes == free && out.sampledAtMs == 100,
            "cache retains complete sample and original sample time");
      check(lfsReads == 1 && takes == 1, "cache hit avoids traversal and filesystem lock");
    });
    run("fresh query bypasses existing cache", [] {
      VFS::CapacitySnapshot out{}; check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 5000), "initial sample");
      uint64_t total = 0, used = 0, free = 0; lfsUsed = 400;
      check(VFS::getStats(VFS::INTERNAL, total, used, free), "fresh refresh");
      check(used == 400 && free == 600 && lfsReads == 2, "getStats always queries driver");
    });
    run("zero max age forces new snapshot", [] {
      VFS::CapacitySnapshot out{}; check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 5000), "initial sample");
      lfsUsed = 600; ++nowMs;
      check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 0), "forced refresh");
      check(out.usedBytes == 600 && out.sampledAtMs == nowMs && lfsReads == 2, "zero age bypasses cache");
    });
    run("cache expiry refreshes exactly at max age", [] {
      VFS::CapacitySnapshot out{}; check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 10), "initial sample");
      nowMs += 10; lfsUsed = 500;
      check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 10) && out.usedBytes == 500 && lfsReads == 2, "expired cache refreshed");
    });
    run("failed refresh does not replace previous successful snapshot", [] {
      VFS::CapacitySnapshot out{}; check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 5000), "initial sample");
      infoError = -1; uint64_t total = 8, used = 9, free = 10;
      check(!VFS::getStats(VFS::INTERNAL, total, used, free), "fresh failure is reported");
      check(total == 8 && used == 9 && free == 10, "failed fresh output unchanged");
      check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 5000) && out.totalBytes == 1000 && out.usedBytes == 250,
            "explicit age-bounded reader can still reuse prior successful sample");
      check(lfsReads == 2, "cached request does not retry failed driver");
    });
    run("invalidation during sample prevents cache publication", [] {
      duringSample = [] { VFS::invalidateStatsSnapshot(VFS::INTERNAL); };
      VFS::CapacitySnapshot out{};
      check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 5000), "fresh caller still receives valid sample");
      lfsUsed = 900;
      check(VFS::getStatsSnapshot(VFS::INTERNAL, out, 5000) && out.usedBytes == 900 && lfsReads == 2,
            "invalidated in-flight sample was not cached");
    });
    run("SD and internal cache tiers stay independent", [] {
      VFS::CapacitySnapshot internal{}, sd{};
      check(VFS::getStatsSnapshot(VFS::INTERNAL, internal, 5000), "internal sample");
      check(VFS::getStatsSnapshot(VFS::SDCARD, sd, 5000), "SD sample");
      VFS::invalidateStatsSnapshot(VFS::SDCARD);
      check(VFS::getStatsSnapshot(VFS::INTERNAL, internal, 5000) && lfsReads == 1, "internal cache survives SD invalidation");
      check(VFS::getStatsSnapshot(VFS::SDCARD, sd, 5000) && sdTotalReads == 2, "SD invalidation refreshes SD only");
    });
    run("zero used bytes remains a valid empty SD volume", [] {
      sdUsed = 0; uint64_t total = 0, used = 999, free = 0;
      check(VFS::getStats(VFS::SDCARD, total, used, free) && total == sdTotal && used == 0 && free == sdTotal,
            "used zero alone does not imply failed capacity sampling");
    });
    run("SD preserves 64-bit capacity", [] {
      sdTotal = UINT64_C(64) * 1024 * 1024 * 1024; sdUsed = UINT64_C(5) * 1024 * 1024 * 1024;
      uint64_t total = 0, used = 0, free = 0;
      check(VFS::getStats(VFS::SDCARD, total, used, free), "SD success");
      check(total == sdTotal && used == sdUsed && free == sdTotal - sdUsed, "large totals are not truncated");
      check(sdTotalReads == 1 && sdUsedReads == 1 && lfsReads == 0, "SD uses SD backend only");
    });
    for (const auto tier : {VFS::INTERNAL, VFS::SDCARD}) {
      run(tier == VFS::INTERNAL ? "LittleFS free capacity clamps at zero" : "SD free capacity clamps at zero", [tier] {
        lfsUsed = lfsTotal + 1; sdUsed = sdTotal + 1;
        uint64_t total = 0, used = 0, free = 999;
        check(VFS::getStats(tier, total, used, free) && used > total && free == 0, "free bytes do not underflow");
      });
    }
    run("AUTO retains internal routing", [] {
      uint64_t total = 0, used = 0, free = 0;
      check(VFS::getStats(VFS::AUTO, total, used, free) && total == 1000 && lfsReads == 1 && sdTotalReads == 0, "AUTO uses internal tier");
    });
    run("capacity query reuses existing FS lock", [] {
      { FsLockGuard outer("test"); uint64_t total = 0, used = 0, free = 0;
        check(VFS::getStats(VFS::INTERNAL, total, used, free), "nested fresh query");
        check(ownsFs && gives == 0 && takes == 1, "nested query does not release outer lock"); }
    });
    run("normalization routes exact SD mount boundary", [] {
      for (const char* path : {"/sd", "/sd/", "sd/folder", "  //sd//folder/ "})
        check(VFS::getStorageType(path) == VFS::SDCARD, std::string("SD path: ") + path);
      for (const char* path : {"", "/", "/sdata", "/sdata/file", "/sdcard", "/SD", "sd.txt"})
        check(VFS::getStorageType(path) == VFS::INTERNAL, std::string("internal path: ") + path);
    });
    run("JSON preserves successful internal shape", [] {
      check(json("/") == "{\"success\":true,\"total\":1000,\"used\":250,\"free\":750,\"usagePercent\":25}", "internal wire output unchanged");
    });
    run("JSON sdata uses internal tier even with no SD", [] {
      sdMounted = false;
      check(json("/sdata/file") == "{\"success\":true,\"total\":1000,\"used\":250,\"free\":750,\"usagePercent\":25}", "prefix neighbor remains internal");
    });
    run("JSON missing SD preserves specific availability error", [] {
      sdMounted = false;
      check(json("sd/file") == "{\"success\":false,\"error\":\"SD card not available\"}", "missing SD wire output unchanged");
      check(sdTotalReads == 0, "unavailable SD is not queried");
    });
    run("JSON retains 64-bit SD fields", [] {
      sdTotal = UINT64_C(64) * 1024 * 1024 * 1024; sdUsed = UINT64_C(16) * 1024 * 1024 * 1024;
      check(json("/sd/file") == "{\"success\":true,\"total\":68719476736,\"used\":17179869184,\"free\":51539607552,\"usagePercent\":25}", "64-bit wire capacity intact");
    });
  }
  std::printf("%u storage-stats scenarios passed; %u failed\n", passed, failed);
  return failed ? 1 : 0;
}
