#pragma once
// Platform doubles for the file catch-up ledger store (System_CatchupLedger.cpp,
// its CATCHUP_LEDGER_HOST region) over the shared fake VFS: the cl* adapters
// (clock, random, boot counter, capture key, PSRAM block, mutex, spinlock, UTC
// clock, scopes), FsLockGuard on the fake's lock double and StagedWrite with
// the production contract. Shared by catchup_ledger_harness.cpp and
// espnow_catchup_serve_harness.cpp. Include after fake_vfs.h (with
// FAKE_VFS_PROVIDE_VFS_NAMESPACE and FAKE_VFS_LOCK_DOUBLE) and ArduinoJson.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- doubles for the production adapters -----------------------------------
#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
static unsigned gWarns = 0;
#define CL_WARN(...) (++gWarns)
#define CL_DEBUG(...) ((void)0)

struct FsLockGuard {
  bool held;
  explicit FsLockGuard(const char*) : held(xSemaphoreTake(gFsMutex, portMAX_DELAY) == pdTRUE) {}
  ~FsLockGuard() { if (held) xSemaphoreGive(gFsMutex); }
};

namespace VFS {
// The production contract: the temporary is written, the destination keeps
// its previous bytes until commit, discard removes the temporary.
class StagedWrite {
 public:
  explicit StagedWrite(const char* writer) : writer_(writer) {}
  bool begin(const String& destination, const AuthContext& ctx) {
    destination_ = destination.c_str();
    temporary_ = destination_ + ".upload.tmp";
    file_ = openGuarded(String(temporary_.c_str()), "w", ctx, true);
    open_ = static_cast<bool>(file_);
    return open_;
  }
  File& file() { return file_; }
  bool commit(const AuthContext& ctx) {
    if (!open_) return false;
    open_ = false;
    file_.flush();
    const bool clean = !file_.getWriteError();
    file_.close();
    if (!clean) { removeGuarded(String(temporary_.c_str()), ctx); return false; }
    return replaceFileGuarded(String(temporary_.c_str()), String(destination_.c_str()), ctx);
  }
  void discard(const AuthContext& ctx) {
    if (!open_) return;
    open_ = false;
    file_.close();
    removeGuarded(String(temporary_.c_str()), ctx);
  }
 private:
  [[maybe_unused]] const char* writer_;
  std::string destination_, temporary_;
  File file_;
  bool open_ = false;
};
}  // namespace VFS

static uint32_t gNow = 1000, gBoot = 41, gSdGen = 1, gRandom = 0x1a2b3c4d;
static bool gKeyReady = true, gNoteHeld = false, gLedgerHeld = false, gAllocFail = false;
static unsigned gYields = 0, gYieldsLocked = 0, gAllocs = 0, gNoteEnters = 0;
static std::vector<void*> gBlocks;
static uint32_t clMillis() { return gNow; }
static uint32_t clRandom() { return gRandom++; }
static uint32_t clBootCounter() { return gBoot; }
static bool clEnsureKey() { return gKeyReady; }
static void* clAlloc(size_t bytes, const char*) {
  ++gAllocs;
  if (gAllocFail) return nullptr;
  void* p = std::calloc(1, bytes);
  gBlocks.push_back(p);
  return p;
}
static bool clSdMounted() { return fake_vfs::fs().sdAvailable(); }
static uint32_t clSdGeneration() { return gSdGen; }
static void clYield() { ++gYields; if (isFsLockedByCurrentTask()) ++gYieldsLocked; }
static void clNoteEnter() { if (gNoteHeld) std::abort(); gNoteHeld = true; ++gNoteEnters; }
static void clNoteExit() { gNoteHeld = false; }
static bool clLedgerMutexCreate() { return true; }
static bool clLedgerTake(uint32_t) { if (gLedgerHeld) return false; gLedgerHeld = true; return true; }
static void clLedgerGive() { gLedgerHeld = false; }
static bool clUtcNow(char* out, size_t cap) { std::snprintf(out, cap, "2026-11-19T18:02:11Z"); return true; }
static const char* clStateScope() { return "/system/catchup"; }
static const char* clTranscriptScope(const String& path) {
  return std::strncmp(path.c_str(), "/sd/", 4) == 0 ? "/sd/stt" : "/stt";
}

