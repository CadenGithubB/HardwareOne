// Bench-only memory-corruption diagnostics. See System_MemDiag.h.
#include "System_MemDiag.h"

#if HW1_BENCH_MEMDIAG
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

#include "sdkconfig.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

extern "C" {
extern uint32_t _bss_start;
extern uint32_t _bss_end;
}

namespace {
constexpr const char* kTag = "MEMDIAG";
constexpr int kSlots = CONFIG_VFS_MAX_COUNT;
constexpr int kWpVfs = 0;
constexpr int kWpEntry = 1;  // FreeRTOS's end-of-stack watchpoint would use this one; it is off in this config
constexpr int kWpUpper = 2;
constexpr uint32_t kHeapCheckEveryMs = 5000;
constexpr uint32_t kEntryCheckEveryMs = 1000;
constexpr int kMaxChangeLogs = 32;
#ifndef ESP_VFS_PATH_MAX
#define ESP_VFS_PATH_MAX 15
#endif

// Mirror of vfs_entry_t, IDF 5.5 components/vfs/private_include/esp_vfs_private.h.
struct VfsEntryMirror {
  int flags;
  const void* vfs;
  void* ctx;
  int offset;
  size_t path_prefix_len;
  char path_prefix[];
};
constexpr size_t kPrefixIgnored = SIZE_MAX;  // LEN_PATH_PREFIX_IGNORED in vfs.c

struct EntrySnap {
  uint32_t words[5];
  char prefix[ESP_VFS_PATH_MAX + 1];
};

volatile uintptr_t* sVfs = nullptr;  // IDF's static s_vfs[]
volatile size_t* sUpper = nullptr;   // IDF's static s_vfs_upper_bound
uintptr_t sBase[kSlots];
size_t sBaseUpper = 0;
EntrySnap sSnap[kSlots];
portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
int sWriteDepth = 0;
bool sWpWanted = false;
bool sWpArmed = false;
uintptr_t sWpBase = 0;
size_t sWpSize = 0;
uintptr_t sWpEntryBase = 0;  // the /littlefs entry's header
size_t sWpEntrySize = 0;
uintptr_t sWp2Base = 0;      // slot 1's entry header (slot 0's PSRAM neighbour)
size_t sWp2Size = 0;
const char* volatile sLastCheckpoint = "none";
int sChangeLogs = 0;
uint32_t sHeapChecks = 0;
uint32_t sHeapFails = 0;
int64_t sHeapMaxUs = 0;
bool sAbortOnCorruption = true;
TaskHandle_t sTask = nullptr;
SemaphoreHandle_t sLock = nullptr;  // serialises baseline updates between the poller and checkpoint callers
char sReply[1536];

uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

bool readable(uintptr_t p, size_t n) {
  if (!p || (p & 3)) return false;
  const void* a = (const void*)p;
  const void* b = (const void*)(p + n - 1);
  return (esp_ptr_internal(a) && esp_ptr_internal(b)) ||
         (esp_ptr_external_ram(a) && esp_ptr_external_ram(b));
}

// VFS entries come from heap_caps_malloc(MALLOC_CAP_DEFAULT): internal RAM normally, PSRAM possible.
bool entryAddrOk(uintptr_t p) {
  return p && !(p & 3) && ((esp_ptr_internal((const void*)p) && esp_ptr_internal((const void*)(p + 40))) ||
                           (esp_ptr_external_ram((const void*)p) && esp_ptr_external_ram((const void*)(p + 40))));
}

const char* region(uintptr_t p) {
  return esp_ptr_internal((const void*)p) ? "int" : esp_ptr_external_ram((const void*)p) ? "psram" : "other";
}

bool entryValid(int slot, uintptr_t p) {
  if (!p) return true;
  if (!entryAddrOk(p)) return false;
  const auto* e = (const VfsEntryMirror*)p;
  if (e->offset != slot || !e->vfs) return false;
  if (e->path_prefix_len != kPrefixIgnored) {
    if (e->path_prefix_len > ESP_VFS_PATH_MAX) return false;
    if (e->path_prefix[e->path_prefix_len] != 0) return false;
  }
  return true;
}

void snapEntry(int slot, uintptr_t p) {
  memset(&sSnap[slot], 0, sizeof(sSnap[slot]));
  if (!p || !entryValid(slot, p)) return;
  memcpy(sSnap[slot].words, (const void*)p, sizeof(sSnap[slot].words));
  const auto* e = (const VfsEntryMirror*)p;
  if (e->path_prefix_len != kPrefixIgnored) memcpy(sSnap[slot].prefix, e->path_prefix, e->path_prefix_len);
}

void hexdump(const char* label, uintptr_t p, int before, int after) {
  uintptr_t a = (p - before) & ~(uintptr_t)3;
  const int words = (before + after) / 4;
  if (!readable(a, (size_t)words * 4)) {
    esp_rom_printf("MEMDIAG %s %08x: not readable\n", label, (unsigned)p);
    return;
  }
  for (int i = 0; i < words; i += 4) {
    const uint32_t* w = (const uint32_t*)(a + i * 4);
    esp_rom_printf("MEMDIAG %s %08x: %08x %08x %08x %08x\n", label, (unsigned)(a + i * 4),
                   (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3]);
  }
}

void logRegistry(const char* why) {
  if (!sVfs) {
    ESP_LOGW(kTag, "registry (%s): not located", why);
    return;
  }
  ESP_LOGW(kTag, "registry (%s): s_vfs=%p upper=%p (%u) wp=%08x/%u armed=%d", why, (void*)sVfs, (void*)sUpper,
           sUpper ? (unsigned)*sUpper : 0u, (unsigned)sWpBase, (unsigned)sWpSize, (int)sWpArmed);
  for (int i = 0; i < kSlots; ++i) {
    const uintptr_t p = sVfs[i];
    if (!p) continue;
    if (!entryValid(i, p)) {
      ESP_LOGE(kTag, "  [%d] %08x (%s) INVALID", i, (unsigned)p, region(p));
      continue;
    }
    const auto* e = (const VfsEntryMirror*)p;
    ESP_LOGW(kTag, "  [%d] %08x (%s) flags=%x vfs=%p ctx=%p prefix=%s", i, (unsigned)p, region(p), e->flags, e->vfs, e->ctx,
             e->path_prefix_len == kPrefixIgnored ? "(fd range)" : e->path_prefix);
  }
}

// Called with the registry located. Returns true when every slot holds NULL or a well-formed entry.
bool registryValid() {
  if (!sVfs) return true;
  for (int i = 0; i < kSlots; ++i)
    if (!entryValid(i, sVfs[i])) return false;
  return !sUpper || *sUpper <= (size_t)kSlots;
}

void corruptionFound(const char* what) {
  esp_rom_printf("MEMDIAG CORRUPTION %s t=%u ms cp=%s\n", what, (unsigned)nowMs(), sLastCheckpoint);
  if (sAbortOnCorruption) {
    // Stop here so the panic dump and coredump show every task right after the write.
    abort();
  }
}

// Compare the slots and the upper bound with the baseline. `legit` re-baselines silently-ish.
void compareRegistryLocked(const char* who, bool legit);
void compareRegistry(const char* who, bool legit) {
  if (!sVfs || !sLock) return;
  xSemaphoreTakeRecursive(sLock, portMAX_DELAY);
  compareRegistryLocked(who, legit);
  xSemaphoreGiveRecursive(sLock);
}

void compareRegistryLocked(const char* who, bool legit) {
  uintptr_t now[kSlots];
  size_t upper = 0;
  portENTER_CRITICAL(&sMux);
  for (int i = 0; i < kSlots; ++i) now[i] = sVfs[i];
  if (sUpper) upper = *sUpper;
  portEXIT_CRITICAL(&sMux);
  bool bad = false;
  for (int i = 0; i < kSlots; ++i) {
    if (now[i] == sBase[i]) continue;
    const bool valid = entryValid(i, now[i]);
    if (sChangeLogs++ < kMaxChangeLogs || !valid) {
      esp_rom_printf("MEMDIAG VFS-CHANGE slot=%d %08x -> %08x valid=%d legit=%d by=%s cp=%s t=%u ms\n", i,
                     (unsigned)sBase[i], (unsigned)now[i], (int)valid, (int)legit, who, sLastCheckpoint,
                     (unsigned)nowMs());
    }
    if (!valid) {
      hexdump("new-target", now[i], 16, 48);
      bad = true;
    }
    sBase[i] = now[i];
    snapEntry(i, now[i]);
  }
  if (sUpper && upper != sBaseUpper) {
    const bool valid = upper <= (size_t)kSlots;
    esp_rom_printf("MEMDIAG VFS-UPPER %u -> %u valid=%d legit=%d by=%s cp=%s t=%u ms\n", (unsigned)sBaseUpper,
                   (unsigned)upper, (int)valid, (int)legit, who, sLastCheckpoint, (unsigned)nowMs());
    sBaseUpper = upper;
    if (!valid) bad = true;
  }
  if (legit) {
    // A remount can reuse an entry's address with new contents (ctx); re-snapshot every slot.
    for (int i = 0; i < kSlots; ++i) snapEntry(i, sBase[i]);
  }
  if (bad) {
    logRegistry("after invalid change");
    corruptionFound("vfs-registry");
  }
}

// Entry contents never change after registration.
void compareEntries(const char* who) {
  if (!sVfs || !sLock) return;
  xSemaphoreTakeRecursive(sLock, portMAX_DELAY);
  for (int i = 0; i < kSlots; ++i) {
    const uintptr_t p = sVfs[i];
    if (!p || p != sBase[i] || !entryValid(i, p)) continue;
    if (memcmp(sSnap[i].words, (const void*)p, sizeof(sSnap[i].words)) == 0) continue;
    esp_rom_printf("MEMDIAG VFS-ENTRY-CHANGE slot=%d entry=%08x by=%s cp=%s t=%u ms\n", i, (unsigned)p, who,
                   sLastCheckpoint, (unsigned)nowMs());
    hexdump("entry-was", (uintptr_t)sSnap[i].words, 0, 20);
    hexdump("entry-now", p, 0, 32);
    corruptionFound("vfs-entry");
  }
  xSemaphoreGiveRecursive(sLock);
}

bool heapCheck(const char* where, bool logOk) {
  const int64_t t0 = esp_timer_get_time();
  const bool ok = heap_caps_check_integrity_all(true);
  const int64_t us = esp_timer_get_time() - t0;
  ++sHeapChecks;
  if (us > sHeapMaxUs) sHeapMaxUs = us;
  const bool vok = registryValid();
  if (!ok || !vok) {
    ++sHeapFails;
    esp_rom_printf("MEMDIAG CHECK at=%s heap=%s vfs=%s us=%u t=%u ms\n", where, ok ? "ok" : "CORRUPT",
                   vok ? "ok" : "INVALID", (unsigned)us, (unsigned)nowMs());
    logRegistry(where);
    corruptionFound(ok ? "vfs-check" : "heap");
  } else if (logOk) {
    ESP_LOGW(kTag, "check at=%s heap=ok vfs=ok us=%u free_int=%u free_ps=%u", where, (unsigned)us,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  return ok && vok;
}

bool locateVfs() {
  for (uint32_t* w = &_bss_start; w < &_bss_end; ++w) {
    const uintptr_t p = *w;
    if (!entryAddrOk(p)) continue;
    const auto* e = (const VfsEntryMirror*)p;
    if (e->path_prefix_len != 9 || e->offset < 0 || e->offset >= kSlots) continue;
    if (memcmp(e->path_prefix, "/littlefs", 10) != 0) continue;
    uint32_t* base = w - e->offset;
    if (base < &_bss_start || base + kSlots > &_bss_end) continue;
    bool ok = true;
    int used = 0;
    for (int i = 0; i < kSlots && ok; ++i) {
      ok = entryValid(i, base[i]);
      used += base[i] != 0;
    }
    if (ok && used >= 2) {
      sVfs = (volatile uintptr_t*)base;
      return true;
    }
  }
  return false;
}

// s_vfs_upper_bound: register a dummy prefix (which must land at index == upper), keep the .bss words that
// went upper -> upper+1 -> upper.
bool locateUpper() {
  size_t upper = 0;
  for (int i = 0; i < kSlots; ++i)
    if (sVfs[i]) upper = (size_t)i + 1;
  for (size_t i = 0; i < upper; ++i)
    if (!sVfs[i]) {
      ESP_LOGW(kTag, "upper bound: registry has holes below %u; not located", (unsigned)upper);
      return false;
    }
  if (upper >= (size_t)kSlots) {
    ESP_LOGW(kTag, "upper bound: registry full; not located");
    return false;
  }
  constexpr int kMax = 1024;
  uint32_t** cand = (uint32_t**)heap_caps_malloc(kMax * sizeof(uint32_t*), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!cand) return false;
  int n = 0;
  for (uint32_t* w = &_bss_start; w < &_bss_end && n < kMax; ++w)
    if (*w == upper) cand[n++] = w;
  static const esp_vfs_fs_ops_t kNoOps = {};
  bool found = false;
  if (n < kMax && esp_vfs_register_fs("/memdiag", &kNoOps, ESP_VFS_FLAG_STATIC, nullptr) == ESP_OK) {
    int m = 0;
    for (int i = 0; i < n; ++i)
      if (*cand[i] == upper + 1) cand[m++] = cand[i];
    esp_vfs_unregister_fs("/memdiag");
    int k = 0;
    for (int i = 0; i < m; ++i)
      if (*cand[i] == upper) cand[k++] = cand[i];
    if (k == 1) {
      sUpper = (volatile size_t*)cand[0];
      found = true;
    } else {
      ESP_LOGW(kTag, "upper bound: %d candidates after the probe (from %d); not located", k, n);
    }
  }
  heap_caps_free(cand);
  return found;
}

void napotCover(uintptr_t a, size_t len, uintptr_t* base, size_t* size) {
  size_t s = 4;
  while (s < len) s <<= 1;
  for (; s <= SOC_CPU_WATCHPOINT_MAX_REGION_SIZE; s <<= 1) {
    const uintptr_t b = a & ~(uintptr_t)(s - 1);
    if (b + s >= a + len) {
      *base = b;
      *size = s;
      return;
    }
  }
  *base = 0;
  *size = 0;
}

void armLocal(void*) {
  if (sWpSize) esp_cpu_set_watchpoint(kWpVfs, (const void*)sWpBase, sWpSize, ESP_CPU_WATCHPOINT_STORE);
  if (sWpEntrySize) esp_cpu_set_watchpoint(kWpEntry, (const void*)sWpEntryBase, sWpEntrySize, ESP_CPU_WATCHPOINT_STORE);
  if (sWp2Size) esp_cpu_set_watchpoint(kWpUpper, (const void*)sWp2Base, sWp2Size, ESP_CPU_WATCHPOINT_STORE);
}

void disarmLocal(void*) {
  esp_cpu_clear_watchpoint(kWpVfs);
  esp_cpu_clear_watchpoint(kWpEntry);
  esp_cpu_clear_watchpoint(kWpUpper);
}

void onBothCores(esp_ipc_func_t fn) {
  for (uint32_t core = 0; core < portNUM_PROCESSORS; ++core) esp_ipc_call_blocking(core, fn, nullptr);
}

void setArmed(bool armed) {
  if (!sVfs || !(sWpSize || sWpEntrySize || sWp2Size)) return;
  if (armed == sWpArmed) return;
  onBothCores(armed ? armLocal : disarmLocal);
  sWpArmed = armed;
  ESP_LOGW(kTag, "store watchpoints %s: slot0 entry %08x/%u, slot1 entry %08x/%u, /littlefs entry %08x/%u",
           armed ? "armed" : "disarmed", (unsigned)sWpBase, (unsigned)sWpSize, (unsigned)sWp2Base, (unsigned)sWp2Size,
           (unsigned)sWpEntryBase, (unsigned)sWpEntrySize);
}

struct FirstBlocks {
  int n;
  void* ptr[4];
  size_t size[4];
  bool used[4];
  intptr_t start;
};

bool firstBlocksCb(walker_heap_into_t heap, walker_block_info_t block, void* user) {
  auto* fb = (FirstBlocks*)user;
  if (fb->n == 0) fb->start = heap.start;
  if (fb->n < 4) {
    fb->ptr[fb->n] = block.ptr;
    fb->size[fb->n] = block.size;
    fb->used[fb->n] = block.used;
    ++fb->n;
  }
  return fb->n < 4;
}

void logFirstPsramBlocks(const char* why) {
  FirstBlocks fb = {};
  heap_caps_walk(MALLOC_CAP_SPIRAM, firstBlocksCb, &fb);
  ESP_LOGW(kTag, "PSRAM heap (%s) start=%08x first blocks:", why, (unsigned)fb.start);
  for (int i = 0; i < fb.n; ++i)
    ESP_LOGW(kTag, "  %p size=%u %s", fb.ptr[i], (unsigned)fb.size[i], fb.used[i] ? "used" : "free");
}

// PSRAM integrity under load: fill a buffer with address-tagged words, write back and invalidate the caches,
// read back from PSRAM. A mismatch prints the word's address, what it holds, and which address that value was
// written for (a burst/latch error shows as a fixed shift, e.g. +4 bytes).
volatile bool sPsTestRunning = false;
void psramTestTask(void* arg) {
  const uint32_t seconds = (uint32_t)(uintptr_t)arg;
  constexpr size_t kBytes = 512 * 1024;
  constexpr size_t kWords = kBytes / 4;
  uint32_t* buf = (uint32_t*)heap_caps_aligned_alloc(128, kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    ESP_LOGE(kTag, "psramtest: no 512 KB PSRAM block");
    sPsTestRunning = false;
    vTaskDelete(nullptr);
  }
  const int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
  uint32_t passes = 0, errors = 0, logged = 0, syncFails = 0;
  int64_t lastReport = esp_timer_get_time();
  ESP_LOGW(kTag, "psramtest: %u s on %u KB at %p", (unsigned)seconds, (unsigned)(kBytes / 1024), buf);
  while (esp_timer_get_time() < end) {
    const uint32_t salt = (passes + 1) * 0x9E3779B9u;
    for (size_t i = 0; i < kWords; ++i) buf[i] = (uint32_t)(uintptr_t)&buf[i] ^ salt;
    if (esp_cache_msync(buf, kBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) ++syncFails;
    if (esp_cache_msync(buf, kBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK) ++syncFails;
    for (size_t i = 0; i < kWords; ++i) {
      const uint32_t v = buf[i];
      const uint32_t want = (uint32_t)(uintptr_t)&buf[i] ^ salt;
      if (v == want) continue;
      ++errors;
      if (logged++ < 24) {
        const uint32_t src = v ^ salt;
        esp_rom_printf("MEMDIAG PSRAM-ERROR pass=%u addr=%08x got=%08x want=%08x tagged-for=%08x shift=%d t=%u ms cp=%s\n",
                       (unsigned)passes, (unsigned)(uintptr_t)&buf[i], (unsigned)v, (unsigned)want, (unsigned)src,
                       (int)((int32_t)((uint32_t)(uintptr_t)&buf[i] - src)), (unsigned)nowMs(), sLastCheckpoint);
      }
    }
    ++passes;
    if (esp_timer_get_time() - lastReport > 10000000) {
      lastReport = esp_timer_get_time();
      ESP_LOGW(kTag, "psramtest: %u passes, %u bad words, %u sync failures", (unsigned)passes, (unsigned)errors,
               (unsigned)syncFails);
    }
    vTaskDelay(1);
  }
  esp_rom_printf("MEMDIAG PSRAM-TEST done: %u passes x %u KB, %u bad words, %u sync failures\n", (unsigned)passes,
                 (unsigned)(kBytes / 1024), (unsigned)errors, (unsigned)syncFails);
  heap_caps_free(buf);
  sPsTestRunning = false;
  vTaskDelete(nullptr);
}

void pollTask(void*) {
  uint32_t lastHeap = nowMs();
  uint32_t lastEntries = lastHeap;
  for (;;) {
    vTaskDelay(1);
    if (__atomic_load_n(&sWriteDepth, __ATOMIC_ACQUIRE) == 0) compareRegistry("poll", false);
    const uint32_t t = nowMs();
    if (t - lastEntries >= kEntryCheckEveryMs) {
      lastEntries = t;
      if (__atomic_load_n(&sWriteDepth, __ATOMIC_ACQUIRE) == 0) compareEntries("poll");
    }
    if (t - lastHeap >= kHeapCheckEveryMs) {
      lastHeap = t;
      heapCheck("periodic", false);
    }
  }
}
}  // namespace

void memDiagBootInit() {
  if (sTask) return;
  sLock = xSemaphoreCreateRecursiveMutex();
  ESP_LOGW(kTag, "bench diagnostics on: heap poisoning %s, coredump %s",
#if CONFIG_HEAP_POISONING_LIGHT
           "light",
#elif CONFIG_HEAP_POISONING_COMPREHENSIVE
           "comprehensive",
#else
           "off",
#endif
#if CONFIG_ESP_COREDUMP_ENABLE_TO_UART
           "to console"
#elif CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
           "to flash"
#else
           "off"
#endif
  );
  if (locateVfs()) {
    locateUpper();
    for (int i = 0; i < kSlots; ++i) {
      sBase[i] = sVfs[i];
      snapEntry(i, sBase[i]);
    }
    sBaseUpper = sUpper ? *sUpper : 0;
    // Crash A's victim was slot 0's entry (the first PSRAM heap block): watch the entry headers, never the
    // table (see armLocal).
    auto header = [](uintptr_t p, uintptr_t* base, size_t* size) {
      *base = 0;
      *size = 0;
      if (!p) return;
      for (size_t len : {sizeof(VfsEntryMirror), (size_t)16}) {
        napotCover(p, len, base, size);
        if (*size && *size <= 64) return;
      }
      *base = 0;
      *size = 0;
    };
    header(sBase[0], &sWpBase, &sWpSize);
    header(sBase[1], &sWp2Base, &sWp2Size);
    for (int i = 0; i < kSlots; ++i) {
      const auto* e = (const VfsEntryMirror*)sBase[i];
      if (e && entryValid(i, sBase[i]) && e->path_prefix_len == 9 && !memcmp(e->path_prefix, "/littlefs", 10)) {
        // Whole entry if one naturally aligned region can hold it, else its 20-byte header (every field crash A
        // read: flags, vfs, ctx, offset, path_prefix_len), else the first 16 bytes.
        for (size_t len : {sizeof(VfsEntryMirror) + 10, sizeof(VfsEntryMirror), (size_t)16}) {
          napotCover(sBase[i], len, &sWpEntryBase, &sWpEntrySize);
          if (sWpEntrySize && sWpEntrySize <= 64) break;
        }
        ESP_LOGW(kTag, "/littlefs entry %08x (%s), block size %u", (unsigned)sBase[i], region(sBase[i]),
                 (unsigned)heap_caps_get_allocated_size((void*)sBase[i]));
      }
    }
  }
  logRegistry("boot");
  logFirstPsramBlocks("boot");
  // Nothing writes a registered entry; only its own unregister frees it, and these three stay registered.
  sWpWanted = true;
  setArmed(true);
  memDiagCheckpoint("boot");
  xTaskCreatePinnedToCore(pollTask, "memdiag", 3072, nullptr, 10, &sTask, tskNO_AFFINITY);
}

void memDiagCheckpoint(const char* where) {
  sLastCheckpoint = where;
  if (__atomic_load_n(&sWriteDepth, __ATOMIC_ACQUIRE) == 0) compareRegistry(where, false);
  heapCheck(where, true);
}

void memDiagVfsWriteBegin() {
  if (!sVfs) return;
  if (__atomic_fetch_add(&sWriteDepth, 1, __ATOMIC_ACQ_REL) == 0 && sWpArmed) {
    onBothCores(disarmLocal);
    sWpArmed = false;
  }
}

void memDiagVfsWriteEnd() {
  if (!sVfs) return;
  if (__atomic_sub_fetch(&sWriteDepth, 1, __ATOMIC_ACQ_REL) != 0) return;
  compareRegistry("mount-or-unmount", true);
  if (sWpWanted) {
    onBothCores(armLocal);
    sWpArmed = true;
  }
}

const char* memDiagCommand(const char* args) {
  while (args && *args == ' ') ++args;
  const char* op = (args && *args) ? args : "status";
  if (!strncmp(op, "arm", 3)) {
    sWpWanted = true;
    setArmed(true);
  } else if (!strncmp(op, "disarm", 6)) {
    sWpWanted = false;
    setArmed(false);
  } else if (!strncmp(op, "check", 5)) {
    memDiagCheckpoint("cmd");
  } else if (!strncmp(op, "dump", 4)) {
    logRegistry("cmd");
    logFirstPsramBlocks("cmd");
  } else if (!strncmp(op, "wptest", 6)) {
    // End-to-end check of the watchpoint -> panic -> coredump path: store the same value back into the
    // watched registry. Armed, this panics at the store below; disarmed, it is a no-op.
    if (sVfs && sWpArmed && sWpSize) {
      volatile VfsEntryMirror* e = (volatile VfsEntryMirror*)sBase[0];
      esp_rom_printf("MEMDIAG wptest: storing into slot 0's entry %08x with the watchpoint armed\n", (unsigned)sBase[0]);
      e->offset = e->offset;
      return "wptest: watchpoint did not fire";
    }
    return "wptest: not armed";
  } else if (!strncmp(op, "psramtest", 9)) {
    unsigned seconds = 60;
    sscanf(op + 9, "%u", &seconds);
    if (sPsTestRunning) return "psramtest: already running";
    sPsTestRunning = true;
    if (xTaskCreatePinnedToCore(psramTestTask, "psramtest", 3072, (void*)(uintptr_t)seconds, 1, nullptr,
                                tskNO_AFFINITY) != pdPASS) {
      sPsTestRunning = false;
      return "psramtest: task create failed";
    }
    return "psramtest: started";
  } else if (!strncmp(op, "noabort", 7)) {
    sAbortOnCorruption = false;
  } else if (strncmp(op, "status", 6)) {
    return "Usage: memdiag [status|check|arm|disarm|dump|wptest|psramtest <s>|noabort]";
  }
  int n = snprintf(sReply, sizeof(sReply),
                   "{\"located\":%s,\"s_vfs\":\"%08x\",\"upper_addr\":\"%08x\",\"upper\":%u,\"wp\":\"%08x/%u\","
                   "\"armed\":%s,\"registry_valid\":%s,\"heap_checks\":%u,\"heap_fails\":%u,\"heap_max_us\":%u,"
                   "\"changes\":%d,\"last_cp\":\"%s\",\"abort\":%s,\"slots\":[",
                   sVfs ? "true" : "false", (unsigned)(uintptr_t)sVfs, (unsigned)(uintptr_t)sUpper,
                   sUpper ? (unsigned)*sUpper : 0u, (unsigned)sWpBase, (unsigned)sWpSize, sWpArmed ? "true" : "false",
                   registryValid() ? "true" : "false", (unsigned)sHeapChecks, (unsigned)sHeapFails,
                   (unsigned)sHeapMaxUs, sChangeLogs, sLastCheckpoint, sAbortOnCorruption ? "true" : "false");
  for (int i = 0; sVfs && i < kSlots && n > 0 && n < (int)sizeof(sReply) - 64; ++i) {
    const uintptr_t p = sVfs[i];
    const char* prefix = "";
    if (p && entryValid(i, p)) {
      const auto* e = (const VfsEntryMirror*)p;
      prefix = e->path_prefix_len == kPrefixIgnored ? "(fd)" : e->path_prefix;
    } else if (p) {
      prefix = "INVALID";
    }
    n += snprintf(sReply + n, sizeof(sReply) - n, "%s\"%08x %s\"", i ? "," : "", (unsigned)p, prefix);
  }
  if (n > 0 && n < (int)sizeof(sReply) - 3) snprintf(sReply + n, sizeof(sReply) - n, "]}");
  return sReply;
}
#endif
