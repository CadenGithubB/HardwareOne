# Memory report fix — full implementation plan (generated, adversarially reviewed)

> Companion to `docs/MEMORY_REPORT_FIX_PLAN.md` (the why). This file is the how: the
> downstream-consumer table, the attacks that landed, and the ordered edits E1-E19.
> Produced 2026-08-23. NOT APPLIED.

# Implementation Plan — make the boot memory report tell the truth

All line numbers verified against the working tree at `/Users/morgan/esp/hardwareone-idf` on 2026-08-23 (`System_Utils.cpp` is already modified vs HEAD; anchors below are the current file).

---

## 1. Downstream consumer list

Every surface that reads the report's values, its helpers, or duplicates its logic.

| Consumer | file:line | Reads what | Affected? |
|---|---|---|---|
| `printMemoryReport()` TOTALS block | `System_Utils.cpp:4334-4352` | `total_known`, `static_total`, `dram_used` | **YES — rewritten (E16).** `Static Over-Estimate` at `:4345` is `static_total - unaccounted`; it prints 0 today only because `static_total` 20,480 < `unaccounted` 31,043. Honest subtotals make it print ~22 KB of artifact. **Deleted, not repaired.** VERIFIED |
| Vanishing-diagnostic guard | `System_Utils.cpp:4343` | `dram_used > total_known` | **YES — deleted (E16).** An over-count currently prints *nothing*. Replaced by a signed residual. VERIFIED |
| `system_tasks_total` | `System_Utils.cpp:4012` | nothing — declared, never read | **YES.** Only occurrence in the file; live `-Wunused-variable`. Replaced by a real accumulator (E6/E10). VERIFIED |
| `reportAllTaskStacks()` | `System_TaskUtils.cpp:560`, TCB size `:630`, assumed margin `:777-780`, table totals `:804`, summary `:838-844` | its own `TCB_SIZE = 104` and `kAssumedUsedBytes = 4096` | **YES — competing report.** Auto-runs every 60 s under `DEBUG_MEMORY` (`HardwareOne.cpp:2554-2560`) into the same sinks. 104 vs the census-verified 376 is 3.6× low. See E19 — **all-or-nothing**. VERIFIED |
| `cmd_memreport` text arm | `System_Utils.cpp:4410-4411` | calls the function, returns a 31-byte string | Output only. `CMD_RESULT_MAX` (4096) does **not** apply. VERIFIED |
| `cmd_memreport` JSON arm | `System_Utils.cpp:4360-4408` | `hw1Internal*`, `heap_caps_*` directly | **No.** Has never carried a `total_known` analogue — no drift. VERIFIED |
| Flutter/Android app | `docs/APP_JSON_CONTRACT.md:57` | — | **No.** `memreport` is in the "Deferred set"; zero hits for `memreport`/`heapCaps` across `docs/FlutterApp-main`. VERIFIED |
| `cmd_taskstats` | `System_Utils.cpp:4416-4508`, uses `taskStackLookup` at `:4478` | its own walk | **No.** Already correct. |
| `sampleMemoryState()` / `[MEMSAMPLE]` | `System_MemoryMonitor.cpp:206-434` | `hw1InternalFreeBytes()` | **No.** Third copy of a task table; prints no accounting total. |
| `heapLogSummary()` `[HEAP]` boot lines | `HardwareOne.cpp:1232-1259` | free/min/largest | **No.** |
| `HEAP_WARNING_THRESHOLD = 25600` | `System_MemoryMonitor.cpp:205`, compared at `:254`, `:509` | `hw1InternalFreeBytes()` | **No.** The owner's specific worry. The fix does not move free heap — nothing to retune. VERIFIED |
| `gMemoryRequirements[]` feature gates | `System_MemoryMonitor.cpp:69-105`, `checkMemoryAvailable()` `:127-146` | `hw1InternalFreeBytes()` | **No.** Cannot wrongly refuse a feature. VERIFIED |
| `hw1InternalTotalBytes()` | `System_MemUtil.h:39`; 10 call sites (`OLED_Mode_System.cpp:90,284`; `System_MemoryMonitor.cpp:214`; `System_SetupWizard.cpp:332,335,348`; `G2_Glasses.cpp:6747`; `System_Utils.cpp:1780,3841,4364`) | — | **No.** Semantics unchanged. The fix is made *at the report*. |
| OLED memory page / perf STACK page | `OLED_Mode_System.cpp:90,284,494-511` | `hw1Internal*` | **No.** |
| G2 lens status meter | `G2_Glasses.cpp:6746-6752` | `hw1Internal*` | **No.** |
| Web bond page / MQTT / status JSON | `WebPage_Bond.cpp:1128`; `System_MQTT.cpp:1181`; `System_Utils.cpp:1780` | `hw1Internal*` | **No.** |
| `System_FeatureRegistry` `heapCostKB` | `System_FeatureRegistry.cpp:639-657`, `:810-855` | own estimate table vs free heap | **No.** Neither feeds nor consumes the report. |
| Automation condition variables | `System_Automation.cpp:61-78` | no memory variable exists | **No.** |
| Host tests | `components/hardwareone/test/host/` (5 files) | — | **No.** None reference the report. |
| `tools/` scripts | — | — | **No.** `tools/map_live_sections.py` parses the linker map, not runtime output. |
| Help text / generated command docs | `System_Utils.cpp:2913` → `docs/COMMAND_REFERENCE.md:93`, `docs/USERGUIDE.md:840` | help string only | **No.** Body-only change. |
| Boot system log file | — | — | **No.** The report runs at `HardwareOne.cpp:2295`, *before* `systemLogAutoStart()` at `:2298-2299`. VERIFIED |
| `docs2/files/System_Utils.cpp.md:200-212` | line-anchored + `@assert` | cites `:3732-4253` (already stale vs the real `:3828-4356`) | **DOCS DEBT.** `:209` ("32 KB WiFi and 8 KB FreeRTOS") wrong on both halves after this. Re-stamp in the same PR. |
| `docs2/systems/boot-and-lifecycle.md:237,242`; `docs2/files/HardwareOne.cpp.md:100` | prose | three-table / four-reporter overlap | **DOCS DEBT.** |
| `docs/ESP32_PITFALL_AUDIT.md:1053, 1066, 1097` | prose | open findings | **RESOLVED by this fix** — mark closed. |
| `docs/MEMORY_LAYOUT.md:171,178,218,244` | prose | tells the reader to trust `memreport` as ground truth | **DOCS DEBT.** Pre-fix figures there understate task cost by ~21 KB. |

**Transport ceilings.** The binding limit is `BROADCAST_PRINTF`'s 256-byte stack buffer (`System_Debug.h:756-763`), and truncation there is *silent*. Every new envelope below is ≤ 207 B expanded. Static broadcast call sites in `3828-4355` are **92 today** (counted programmatically); after this change 93 static / **89 on the normal runtime path** (−4 removed, +1 unconditional, +4 conditional-diagnostic). Printed lines drop by 2. The earlier draft's "92 → 90" claim was wrong.

---

## 2. Attacks that landed, and what changed because of them

| Attack | Verdict | Reflected in |
|---|---|---|
| Span table walked with `MALLOC_CAP_INVALID` = **all heaps**, including PSRAM/RTCRAM, then summed against an `INTERNAL\|8BIT`-only `dram_used` | **SERIOUS, correct.** `heap_caps.c:600` `bool all_heaps = caps & MALLOC_CAP_INVALID;`. Both enabling configs are ON: `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` and `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` on **all five** board profiles. VERIFIED | **E1/E5**: walk `MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT` |
| `kMax = 12` truncates silently → silent under-count | **SERIOUS.** 5 heaps match `INTERNAL\|8BIT` on xiao_s3 (`bootlog.txt:3-6` + reserve pool); `esp_psram.c:506-517` can register the reserve in *multiple* chunks | **E1**: `kMax = 16` + `overflow` flag + loud line (E10) |
| Measurement interleaved with printing → use-after-free read of a TLSF header for a self-deleted task; `heap_caps_get_allocated_size` does **no** used/free check, so a coalesced block reports an unbounded size straight into ATTRIBUTED | **SERIOUS.** `multi_heap.c:133-136` returns `tlsf_block_size(p)` with no liveness test; 27 `vTaskDelete(NULL)` sites in the component; the codebase already guards this race at `System_TaskUtils.cpp:756` | **E6/E9**: one tight measure loop, print afterwards; 64 KB plausibility clamp; the `:756` stale-name guard reused |
| "Cost: zero net" false — `taskStackLookup()` takes a `portMAX_DELAY` mutex and linearly `strncmp`s a **PSRAM-resident** 56-slot registry, called 2× per task | **SERIOUS.** `System_TaskUtils.cpp:110` `EXT_RAM_BSS_ATTR`, `:131-136` `xSemaphoreTake(..., portMAX_DELAY)`, `:163-180` linear scan | **E9**: called **once** per task, cached in `taskMeasure[i].regBytes` |
| Board matrix built on a false premise ("feather_esp32_v2 has no PSRAM") | **SERIOUS.** All five boards: `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`, `CONFIG_FREERTOS_NUMBER_OF_CORES=2`. The `R == 0` arm is **not exercised by any config in the repo**. VERIFIED | **E18** rewritten |
| The promised "fold the existing `heap_caps_get_largest_free_block` call" never appeared in any step | **Correct.** | **E2/E3** actually do it |
| Sample output shipped figures the author knew wouldn't print (`alloc_meta 4436`, and a `[-1%]` that integer-truncates to 0) | **SERIOUS**, in a plan about honesty | §4 states a band, no fabricated percent |
| Two-sample skew: `dram_used` at `:3841`, `heap_caps_get_info` ~490 lines later | **Correct.** | **E2**: sampled together |
| "Closes to zero by construction" overstated — reserve pool registered as `[start, start+size-1]`, possibly multi-chunk | **Correct**, and `System_MemUtil.h:36-38` already documents it | **E2** comment says "to within one byte per reserve chunk" |
| Section header "IDF/FreeRTOS-owned" asserts ownership a registry-membership test can't establish (`llm_gen`, `System_LLM.cpp:277`, skips `taskStackRecord`; registry is 56 slots for a ~40-name union) | **Correct.** | **E10**: header is "Other task stacks (kernel / unregistered)" |
| `Subtotal (tasks)` duplicates `Task stacks + TCBs` ten lines later | **Correct.** | **E10** omits it |
| `unmeasured_count` double-increments per task | **Correct.** | **E9/E10**: per-task flag |
| E15 (TCB-only) makes the competing report *newly* inconsistent | **Correct.** | **E19**: all-or-nothing, and it widens `%4u`→`%6u` at `:804` |
| E12 anchor off by one | **Correct** — the `[3]` line is `:4128`, `:4127` is `broadcastOutput("")` | **E14** |
| `#include "HAL_Input.h"` comment orphaned once `appTasks[]` dies | **Correct.** `INPUT_TASK_NAME` appears only at `:52` (the comment) and `:4028` | **E17** |
| Scope creep (17 edits) | **Fair.** | `calculateSensorSystemMemory()` deletion dropped from this change; `taskStatusCap` kept (one word, strictly correct) |

**One attack claim I reject:** dropping the range guard in favour of `xTaskGetStaticBuffers()`. ESP-IDF `pvPortMalloc`s the IDLE and Tmr Svc stacks *and* TCBs (`port_common.c:41-49`, `:84-92`) but FreeRTOS still flags them statically allocated — skipping "static" tasks would drop 5,120 B of stacks + 1,128 B of TCBs, a quarter of defect 1. The heap-span table earns its place. VERIFIED

---

## 3. Ordered edits

Apply **bottom-up (E19 → E1)**, or re-grep after each edit:
`grep -n 'total_known\|static_total\|appTasks\|system_tasks_total\|TOTAL ACCOUNTED' components/hardwareone/System_Utils.cpp`

No new includes anywhere: `esp_heap_caps.h` at `System_Utils.cpp:15` pulls in `multi_heap.h` (`esp_heap_caps.h:11`) for `multi_heap_info_t`; `System_TaskUtils.h` at `:50` for `taskStackLookup`. VERIFIED

### E1 — INSERT helpers at `System_Utils.cpp:3826`
(blank line between the `ps_alloc` extern at `:3825` and the `// Comprehensive memory report` comment at `:3827`)

```cpp
// ── Heap-block measurement helpers for printMemoryReport() ──────────────
// heap_caps_get_allocated_size() ends in assert(find_containing_heap(ptr))
// (esp-idf/components/heap/heap_caps.c:465-466) and assertions are compiled in
// on EVERY board profile (CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2), so a
// pointer outside every registered heap aborts the device inside the boot
// report. Collect the heap spans once, range-check before asking.
//
// THE CAP SET IS LOAD-BEARING. Walk the same set the report's dram_used
// describes (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), never MALLOC_CAP_INVALID:
// INVALID means "every heap" (heap_caps.c:600) and would let a PSRAM or RTCRAM
// block be measured and folded into a DRAM-only total. Both
// CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY and
// CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM are set on all five board
// profiles, so a PSRAM stack is one xTaskCreateWithCaps away. With the narrow
// set such a stack falls outside the table, reads as unmeasured, and is
// reported rather than mis-added.
//
// Returning false stops tlsf_walk_pool after the FIRST block of a pool
// (tlsf/tlsf.c:205-224) while heap_caps_walk's SLIST_FOREACH still advances to
// the next heap (heap_caps.c:600-609): one callback per heap, not a heap walk.
struct HeapSpanTable {
  enum { kMax = 16 };   // 5 heaps match INTERNAL|8BIT on xiao_s3
  intptr_t start[kMax];
  intptr_t end[kMax];
  size_t   count;
  bool     overflow;
};

extern "C" {
static bool hw1HeapSpanCollect(walker_heap_into_t heap_info,
                               walker_block_info_t block_info,
                               void* user_data) {
  (void)block_info;
  HeapSpanTable* t = static_cast<HeapSpanTable*>(user_data);
  if (t->count < (size_t)HeapSpanTable::kMax) {
    t->start[t->count] = heap_info.start;
    t->end[t->count]   = heap_info.end;
    t->count++;
  } else {
    t->overflow = true;   // never truncate silently
  }
  return false;
}
}  // extern "C"

// Allocated block size for ptr, or 0 when ptr is not inside a registered
// INTERNAL|8BIT heap (a .bss stack, or one placed in PSRAM/RTCRAM). 0 means
// UNKNOWN, never "zero bytes" — callers must report it, never fold it in as
// nothing. An implausible size is also rejected: a task deleted between the
// snapshot and this call leaves a dangling pxStackBase whose TLSF header may
// now describe a coalesced free block, and heap_caps_get_allocated_size
// performs no used/free check (multi_heap.c:133-136).
static constexpr size_t kHw1MaxPlausibleBlock = 64 * 1024;

static size_t hw1HeapBlockBytes(const HeapSpanTable& spans, const void* ptr) {
  if (!ptr) return 0;
  const intptr_t p = (intptr_t)ptr;
  for (size_t i = 0; i < spans.count; i++) {
    if (p >= spans.start[i] && p < spans.end[i]) {
      const size_t sz = heap_caps_get_allocated_size(const_cast<void*>(ptr));
      return (sz > 0 && sz <= kHw1MaxPlausibleBlock) ? sz : 0;
    }
  }
  return 0;
}
```

### E2 — INSERT after `System_Utils.cpp:3845` (`dram_peak_used`)

```cpp
  // The allocator's own view of the SAME heap set, sampled here beside
  // dram_total/dram_free so every figure in TOTALS comes from one moment.
  //
  // RAW heap_caps_get_total_size(), NOT hw1InternalTotalBytes(): the helper
  // subtracts CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL to cancel the reserve
  // pool's double registration (System_MemUtil.h:36-45). Allocator metadata is
  // a property of the physical spans, so the helper would understate it by
  // exactly 32,768 B — on every board, all five set that to 32768.
  //
  // Per heap, span = heap_t + TLSF control_t + payload + free + 4 B/live block
  // (multi_heap.c:146-159, :431), so span - free - allocated IS the
  // bookkeeping, over the identical predicate (heap_caps_match,
  // heap_caps.c:73-76, applied at :270 and :373). dram_used already nets the
  // reserve out, so the residual in TOTALS cancels it without naming it —
  // exact to within one byte per reserve chunk (System_MemUtil.h:36-38).
  multi_heap_info_t dram_info{};
  heap_caps_get_info(&dram_info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t dram_span_raw  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t dram_span_used = dram_info.total_free_bytes + dram_info.total_allocated_bytes;
  const size_t alloc_meta =
      (dram_span_raw > dram_span_used) ? (dram_span_raw - dram_span_used) : 0;
```

### E3 — REPLACE `System_Utils.cpp:3907-3908`

```cpp
  size_t cap_int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t cap_int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
```
with
```cpp
  // Derived from the single sample above, not re-queried: get_free_size sums
  // multi_heap_free_size (heap_caps.c:282) and get_info sums heap->free_bytes
  // (multi_heap.c:432) — the same number; and get_largest_free_block IS
  // get_info(...).largest_free_block verbatim (heap_caps.c:296-306). One fewer
  // full TLSF block walk than today.
  size_t cap_int_free    = dram_info.total_free_bytes;
  size_t cap_int_largest = dram_info.largest_free_block;
```

### E4 — `System_Utils.cpp:3930` relabel, `:3932` delete
```cpp
  broadcastOutput("-- MEMORY BREAKDOWN (measured live heap + cumulative allocation traffic) --");
```
and delete `  size_t total_known = 0;` (`:3932`). Its only three `+=` sites (`:4124`, `:4215`, `:4223`) all go below, so the declaration must go with them.

### E5 — REPLACE `System_Utils.cpp:3990-3994`

```cpp
  // ========== SECTION 2: LIVE HEAP ATTRIBUTION ==========
  // Everything summed here is MEASURED off the live allocator. Lines marked
  // '~' are estimates and are deliberately NOT summed — an estimate folded
  // into a measured total corrupts the measurement, which is exactly how the
  // old hardcoded "FreeRTOS ~8192" credit hid a 24 KB shortfall.
  broadcastOutput("");
  broadcastOutput("[2] LIVE HEAP ATTRIBUTION (measured; '~' lines are estimates, not summed):");

  // Registered INTERNAL|8BIT heap spans — range-check before
  // heap_caps_get_allocated_size(). Same cap set as dram_used above.
  HeapSpanTable heapSpans{};
  heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, hw1HeapSpanCollect, &heapSpans);
  if (heapSpans.overflow) {
    broadcastOutput("  !! heap span table full - some blocks will read as unmeasured");
  }
```
(~140 B of stack. The report runs on `main` at boot and on `cmd_exec_task` from the CLI, which the bootlog shows at 11 % of 8192.)

### E6 — REPLACE `System_Utils.cpp:3996-4012`

```cpp
  // Task stacks — snapshot buffer plus a parallel measurement buffer, so the
  // pointer reads can all happen in one tight loop before any printing.
  UBaseType_t taskCount = uxTaskGetNumberOfTasks();

  struct TaskMeasure {
    uint32_t stackBytes;  // measured; 0 = not in a registered INTERNAL|8BIT heap
    uint32_t tcbBytes;    // measured; 0 = same
    uint32_t regBytes;    // taskStackLookup(): size recorded at creation; 0 = not ours
  };
  static TaskStatus_t* taskStatusArray = nullptr;
  static TaskMeasure*  taskMeasure     = nullptr;
  static UBaseType_t   taskStatusCap   = 0;
  if (taskCount > taskStatusCap) {
    if (taskStatusArray) { free(taskStatusArray); taskStatusArray = nullptr; }
    if (taskMeasure)     { free(taskMeasure);     taskMeasure     = nullptr; }
    taskStatusCap = 0;
    taskStatusArray = (TaskStatus_t*)ps_alloc(taskCount * sizeof(TaskStatus_t),
                                              AllocPref::PreferPSRAM, "memreport.tasks");
    taskMeasure     = (TaskMeasure*)ps_alloc(taskCount * sizeof(TaskMeasure),
                                             AllocPref::PreferPSRAM, "memreport.taskmeas");
    if (taskStatusArray && taskMeasure) {
      taskStatusCap = taskCount;
    } else {                       // partial failure must not leave a half-armed pair
      if (taskStatusArray) { free(taskStatusArray); taskStatusArray = nullptr; }
      if (taskMeasure)     { free(taskMeasure);     taskMeasure     = nullptr; }
    }
  }

  size_t   app_stacks_total   = 0;
  size_t   other_stacks_total = 0;
  size_t   tcb_total          = 0;
  unsigned app_task_count     = 0;
  unsigned other_task_count   = 0;
  unsigned tcb_count          = 0;
  unsigned unmeasured_tasks   = 0;
```

### E7 — `System_Utils.cpp:4014` and `:4015`
```cpp
  if (taskStatusArray && taskMeasure) {
    // taskStatusCap, not taskCount: a task spawned between
    // uxTaskGetNumberOfTasks() above and this call makes the kernel return 0
    // when the array is undersized, silently zeroing the subtotal. cap is only
    // ever set on a successful pair-alloc, so it is always the true capacity.
    UBaseType_t actualCount = uxTaskGetSystemState(taskStatusArray, taskStatusCap, NULL);
```

### E8 — DELETE `System_Utils.cpp:4017-4032`
The `// Application tasks we created` comment, the anonymous `struct {` at `:4018`, and the whole `appTasks[]` table `:4021-4032`. It can name only 10 of ~106 `xTaskCreate*` call sites, and `sensor_queue_task` (17 chars) can never match anyway: FreeRTOS truncates to 15 (`CONFIG_FREERTOS_MAX_TASK_NAME_LEN=16`, all five boards) while the matchers at `:4042`/`:4081` used bare `strcmp`. `taskStackLookup()` compares under the same 16-byte bound (`System_TaskUtils.cpp:175`). VERIFIED

### E9 — REPLACE `System_Utils.cpp:4034-4070` (from `broadcastOutput("  Application Task Stacks:");` through `static_total += app_tasks_total;`)

```cpp
    // MEASURE FIRST, PRINT SECOND. pxStackBase and xHandle are raw pointers
    // into memory the IDLE task frees asynchronously after vTaskDelete(NULL)
    // (27 such sites in this component), and heap_caps_get_allocated_size()
    // performs no used/free check — a freed, coalesced block reports the
    // COALESCED size. Doing every pointer read here instead of interleaved
    // with ~80 queue messages shrinks that window to microseconds. It also
    // calls taskStackLookup() ONCE per task: it is not a table lookup but a
    // portMAX_DELAY mutex over a PSRAM-resident 56-slot registry
    // (System_TaskUtils.cpp:110, :131-136, :163-180).
    for (UBaseType_t i = 0; i < actualCount; i++) {
      const char* name = taskStatusArray[i].pcTaskName;
      // Same stale-entry guard reportAllTaskStacks() already uses
      // (System_TaskUtils.cpp:756).
      taskMeasure[i].regBytes =
          (name && (uintptr_t)name >= 0x3F000000) ? taskStackLookup(name) : 0;
      taskMeasure[i].stackBytes =
          (uint32_t)hw1HeapBlockBytes(heapSpans, taskStatusArray[i].pxStackBase);
      taskMeasure[i].tcbBytes =
          (uint32_t)hw1HeapBlockBytes(heapSpans, (const void*)taskStatusArray[i].xHandle);
    }

    broadcastOutput("  Application task stacks (used / allocated):");

    for (UBaseType_t i = 0; i < actualCount; i++) {
      if (taskMeasure[i].regBytes == 0) continue;   // not in our creation registry
      const char*  name      = taskStatusArray[i].pcTaskName;
      const size_t freeBytes = taskStatusArray[i].usStackHighWaterMark;  // BYTES on this port

      if (taskMeasure[i].tcbBytes) { tcb_total += taskMeasure[i].tcbBytes; tcb_count++; }

      if (taskMeasure[i].stackBytes == 0) {
        // Registry knows the requested size, but the block is not in internal
        // heap, so it is NOT part of dram_used. Shown, never summed.
        unmeasured_tasks++;
        BROADCAST_PRINTF("    %-20s     ? / ~%5lu bytes (not internal heap; free %lu)",
                         name, (unsigned long)taskMeasure[i].regBytes,
                         (unsigned long)freeBytes);
        continue;
      }
      const size_t allocatedBytes = taskMeasure[i].stackBytes;
      const size_t usedBytes = (allocatedBytes > freeBytes) ? (allocatedBytes - freeBytes) : 0;
      app_stacks_total += allocatedBytes;
      app_task_count++;
      BROADCAST_PRINTF("    %-20s %5lu / %5lu bytes (%2u%% used)",
                       name, (unsigned long)usedBytes, (unsigned long)allocatedBytes,
                       (unsigned)((usedBytes * 100) / allocatedBytes));
    }
```

### E10 — REPLACE `System_Utils.cpp:4072-4095` (from `// Second pass: show system tasks` through the loop's closing `}`)

```cpp
    // Second pass: everything not in our creation registry. Header does NOT
    // claim kernel ownership — taskStackLookup() is a registry-membership
    // test, and a task created without taskStackRecord (e.g. "llm_gen",
    // System_LLM.cpp:277) or dropped by registry overflow lands here too.
    // These stacks AND TCBs are heap, not .bss: ESP-IDF pvPortMallocs both
    // even for the "static" IDLE and Tmr Svc tasks (port_common.c:41-49,
    // :84-92). The old report printed their HWM and summed NOTHING — 20,992 B
    // of live heap silently dropped on the minimal build, more on every
    // feature-enabled one.
    broadcastOutput("");
    broadcastOutput("  Other task stacks (kernel / unregistered, used / allocated):");

    for (UBaseType_t i = 0; i < actualCount; i++) {
      if (taskMeasure[i].regBytes != 0) continue;   // shown above
      const char*  name      = taskStatusArray[i].pcTaskName;
      const size_t freeBytes = taskStatusArray[i].usStackHighWaterMark;

      if (taskMeasure[i].tcbBytes) { tcb_total += taskMeasure[i].tcbBytes; tcb_count++; }

      if (taskMeasure[i].stackBytes == 0) {
        unmeasured_tasks++;
        BROADCAST_PRINTF("    %-20s     ? /     ? bytes (not internal heap; free %lu)",
                         name, (unsigned long)freeBytes);
        continue;
      }
      const size_t allocatedBytes = taskMeasure[i].stackBytes;
      const size_t usedBytes = (allocatedBytes > freeBytes) ? (allocatedBytes - freeBytes) : 0;
      other_stacks_total += allocatedBytes;
      other_task_count++;
      BROADCAST_PRINTF("    %-20s %5lu / %5lu bytes (%2u%% used)",
                       name, (unsigned long)usedBytes, (unsigned long)allocatedBytes,
                       (unsigned)((usedBytes * 100) / allocatedBytes));
    }

    // 3 lines, ~170 B expanded against the 256 B BROADCAST_PRINTF stack buffer
    // (System_Debug.h:756-763, where overflow truncates SILENTLY).
    BROADCAST_PRINTF(
      "  Stacks (application): %6lu bytes  (%2u tasks)\n"
      "  Stacks (other):       %6lu bytes  (%2u tasks)\n"
      "  Task control blocks:  %6lu bytes  (%2u of %2u tasks measured)",
      (unsigned long)app_stacks_total,   app_task_count,
      (unsigned long)other_stacks_total, other_task_count,
      (unsigned long)tcb_total,          tcb_count, (unsigned)actualCount);

    if (unmeasured_tasks > 0) {
      BROADCAST_PRINTF("  (%u task stack(s) outside every registered internal heap - NOT attributed)",
                       unmeasured_tasks);
    }
    if (actualCount == 0) {
      // uxTaskGetSystemState also has a weak stub in this component that
      // returns 0 unconditionally (System_TaskUtils.cpp:22-31).
      broadcastOutput("  !! uxTaskGetSystemState returned 0 - task memory NOT attributed this run");
    }
```

### E11 — `System_Utils.cpp:4097` (the `}` closing `if (taskStatusArray)`)
```cpp
  } else {
    broadcastOutput("  !! task snapshot allocation failed - task memory NOT attributed this run");
  }
```

### E12 — `System_Utils.cpp:4106-4108`
Keep the line and its `#if ENABLE_WIFI` guard — the right precedent, gated rather than zeroed — but stop folding an estimate into a measured total. Delete `:4108` and relabel:
```cpp
  BROADCAST_PRINTF("  WiFi Driver:   ~ %6lu bytes (%2lu KB) (estimate; inside UNATTRIBUTED)",
                  (unsigned long)wifi_estimate, (unsigned long)(wifi_estimate / 1024));
```
The block becomes print-only, so no accumulator is orphaned on a WiFi-less build.

### E13 — DELETE `System_Utils.cpp:4111-4124`
- `:4111-4114` — the `UI Framework: ~0 bytes (untracked)` line. Always zero, never summed, costs a full 256 B queue slot and a BLE/ESP-NOW ring record.
- `:4116-4120` — **defect 2.** The `8 * 1024` literal is **deleted, not raised to 26 KB**: the real FreeRTOS heap cost *is* the stacks and TCBs E9/E10 now measure (`docs/DRAM_UNACCOUNTED_CENSUS_2026-08-23.md:31`), so raising it would double-count ~24 KB.
- `:4122-4124` — `Subtotal (static)` and `total_known += static_total;`.

Nothing replaces them; the measured rows are the replacement.

### E14 — `System_Utils.cpp:4128` relabel, `:4215` delete
`:4128` (not `:4127`, which is `broadcastOutput("")`):
```cpp
  // .bss/.data, NOT heap. .dram0.bss ends at 0x3fc9e4c0, which IS
  // .dram0.heap_start and the base of heap region 1 (bootlog "At 3FC9E4C0"),
  // so no static byte can ever be inside ACTUAL DRAM USED. Printed for scale,
  // excluded from the totals.
  broadcastOutput("[3] STATIC VARIABLES BY MODULE (.bss/.data - NOT heap, excluded from totals):");
```
Delete `:4215` `total_known += static_vars_total;`. **The `#if ENABLE_HTTP_SERVER` arithmetic at `:4202-4211` is untouched** — Section 3 keeps printing on every config, it just stops polluting the total.

### E15 — REPLACE `System_Utils.cpp:4220-4224`
`printConnectedDevicesLibraries()` (`:3745-3788`) fills an estimate table (`i2cSensors[].libraryHeapBytes`), not a measurement:
```cpp
  BROADCAST_PRINTF("  Device Libraries: ~%6lu bytes (%3lu KB) (estimate; inside UNATTRIBUTED)",
                  (unsigned long)devices_lib_total, (unsigned long)(devices_lib_total / 1024));
```
(deletes the `if (devices_lib_total > 0) { total_known += ... }` block).

### E16 — REPLACE `System_Utils.cpp:4334-4352` (the whole TOTALS block through the closing `}` of `if (dram_used > total_known)`)

```cpp
  // ========== TOTALS ==========
  // dram_used and alloc_meta were sampled together at the top of this function,
  // so this is one snapshot, not two ~490 lines apart.
  const size_t tasks_total = app_stacks_total + other_stacks_total + tcb_total;
  const size_t attributed  = tasks_total + alloc_meta;

  // SIGNED on purpose. The old block lived inside `if (dram_used >
  // total_known)`, so an over-count printed NOTHING; and its "Static
  // Over-Estimate" line was static_total - unaccounted, two different
  // quantities subtracted, which read 0 only because it clamped.
  const long residual = (long)dram_used - (long)attributed;
  const unsigned attr_pct =
      dram_used ? (unsigned)((attributed * 100 + dram_used / 2) / dram_used) : 0;

  // 2 envelopes: ~180 B and ~150 B expanded, against the 256 B buffer.
  BROADCAST_PRINTF(
    "\n---------- TOTALS ----------\n"
    "  ACTUAL DRAM USED:     %6lu bytes (%4lu KB)\n"
    "  Task stacks + TCBs:   %6lu bytes (%4lu KB)  measured\n"
    "  Allocator metadata:   %6lu bytes (%4lu KB)  measured",
    (unsigned long)dram_used,   (unsigned long)(dram_used / 1024),
    (unsigned long)tasks_total, (unsigned long)(tasks_total / 1024),
    (unsigned long)alloc_meta,  (unsigned long)(alloc_meta / 1024));

  BROADCAST_PRINTF(
    "  ATTRIBUTED:           %6lu bytes (%4lu KB) [%2u%%]\n"
    "  UNATTRIBUTED:       %8ld bytes  (kernel objs, libc/VFS/FS/NVS, C++ ctors, driver heap)",
    (unsigned long)attributed, (unsigned long)(attributed / 1024), attr_pct,
    residual);
```

### E17 — `System_Utils.cpp:52`
`INPUT_TASK_NAME` / `INPUT_STACK_WORDS` appear only at `:52` (the comment) and `:4028` (the deleted table). VERIFIED. Drop the include, or if `HAL_Input.h` is still needed transitively, correct the comment so it does not name a symbol the file no longer uses.

### E18 — build the gate matrix (see §6)

### E19 — `System_TaskUtils.cpp`, **separate commit, all-or-nothing**
`reportAllTaskStacks()` auto-runs every 60 s under `DEBUG_MEMORY` (`HardwareOne.cpp:2554-2560`) into the same sinks. Fixing only `TCB_SIZE` makes it agree on TCBs while newly disagreeing on system-stack bytes — worse than the status quo for the one user with both open. Do all four or none:

1. `:630` — `const uint32_t TCB_SIZE = 104;` → measure per task with the same span-table + `heap_caps_get_allocated_size(xHandle)` approach (or, if the span table is not worth duplicating, `(uint32_t)sizeof(StaticTask_t)`, which FreeRTOS guarantees equals `sizeof(TCB_t)` — `FreeRTOS.h:1275-1281`; `freertos/FreeRTOS.h` is already included at `:10`).
2. `:777-780` — retire `kAssumedUsedBytes = 4096`; measure `pxStackBase` instead. The function already has the snapshot in hand at `:598`.
3. `:625` — the banner "kernel tasks report no stack size, so Stack/Used/Used% assume a 4 KB margin" becomes false; rewrite.
4. `:804` — widen `%4u` to `%6u`: at 376 B/TCB a ~40-task build reaches ~15,040, five digits, and breaks the fixed-width rule the table is drawn against.

**Explicitly out of this change:** `calculateSensorSystemMemory()` (`System_Utils.cpp:3791-3810`, `System_Utils.h:381`) is dead with zero callers, but deleting it is unrelated cleanup.

---

## 4. Corrected sample output — minimal build (xiao_s3, serial-only, all feature flags 0)

Changed sections only; `[1]` and `[4]` are untouched.

```
[2] LIVE HEAP ATTRIBUTION (measured; '~' lines are estimates, not summed):
  Application task stacks (used / allocated):
    debug_out             3236 /  4096 bytes (79% used)
    cmd_exec_task          928 /  8192 bytes (11% used)

  Other task stacks (kernel / unregistered, used / allocated):
    main                  4472 /  8704 bytes (51% used)
    IDLE1                  796 /  1536 bytes (51% used)
    IDLE0                  700 /  1536 bytes (45% used)
    esp_timer              732 /  4096 bytes (17% used)
    Tmr Svc                668 /  2048 bytes (32% used)
    ipc1                   732 /  1536 bytes (47% used)
    ipc0                   732 /  1536 bytes (47% used)
  Stacks (application):  12288 bytes  ( 2 tasks)
  Stacks (other):        20992 bytes  ( 7 tasks)
  Task control blocks:    3384 bytes  ( 9 of  9 tasks measured)

[3] STATIC VARIABLES BY MODULE (.bss/.data - NOT heap, excluded from totals):
  ...unchanged rows...
  Subtotal (static vars):    425 bytes (  0 KB)
  Device Libraries: ~     0 bytes (  0 KB) (estimate; inside UNATTRIBUTED)

---------- TOTALS ----------
  ACTUAL DRAM USED:      51948 bytes (  50 KB)
  Task stacks + TCBs:    36664 bytes (  35 KB)  measured
  Allocator metadata:     ~4800 bytes (   4 KB)  measured      <- see band below
  ATTRIBUTED:            41464 bytes (  40 KB) [80%]
  UNATTRIBUTED:          10484 bytes  (kernel objs, libc/VFS/FS/NVS, C++ ctors, driver heap)
```

**Reconciliation.**

| Row | Bytes | Source |
|---|---:|---|
| App stacks — `debug_out` 4,096 + `cmd_exec_task` 8,192 | 12,288 | matches today's `Subtotal (app)` in `bootlog.txt` |
| Other stacks — main 8,704 + esp_timer 4,096 + Tmr Svc 2,048 + IDLE×2 3,072 + ipc×2 3,072 | 20,992 | census row 1 — **defect 1** |
| TCBs — 9 × 376 | 3,384 | census row 3 — **defect 4a** |
| **Task stacks + TCBs** | **36,664** | exact, no estimate |
| Allocator metadata `T − F − A` | **band: 4,400–5,400** | census rows 2+16 — **defect 4b** |
| **ATTRIBUTED** | **41,064 – 42,064** | |
| **UNATTRIBUTED** = `51,948 − ATTRIBUTED` | **9,884 – 10,884** | census rows 4-17 |

The `~4800` above is an **illustrative midpoint, not a prediction of the exact printed value.** The census's 4,436 counted 4-byte TLSF headers on only the 19 blocks it enumerated; the live `T − F − A` folds in 4 B for *every* live internal block (`multi_heap.c:430-431`), and the live internal block count cannot be determined statically. **The invariant that does hold exactly, whatever `alloc_meta` prints: `ATTRIBUTED + UNATTRIBUTED == ACTUAL DRAM USED`**, because the residual is computed as a difference and never assembled from parts. Do not claim agreement with the census's rows 4-17 until a hardware boot confirms it.

Of the residual, `docs/DRAM_UNACCOUNTED_CENSUS_2026-08-23.md` row 17 says ~3,052 B is genuinely unexplained and static analysis cannot close it (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` routes untracked C++ static-constructor traffic to internal DRAM). That is fine. A report that says "unattributed: 10,484" honestly beats one padded to zero.

**Delta vs today's output:**

```
TOTAL ACCOUNTED      20,905  ->  ATTRIBUTED     ~41,464   (+20,559)
Unaccounted DRAM     31,043  ->  UNATTRIBUTED   ~10,484   (-20,559)
Static Over-Estimate      0  ->  DELETED (would have printed ~22,432 once honest)
FreeRTOS:      ~ 8,192       ->  DELETED (the literal, defect 2)
UI Framework:  ~     0       ->  DELETED (always zero, never summed)
static vars 425 inside total ->  printed, excluded (defect 3)
```

**Failure paths now visible instead of silent:**

```
  !! task snapshot allocation failed - task memory NOT attributed this run
  ...
  ATTRIBUTED:             4800 bytes (   4 KB) [ 9%]
  UNATTRIBUTED:          47148 bytes  (kernel objs, libc/VFS/FS/NVS, C++ ctors, driver heap)
```
and on an over-count or snapshot skew the residual goes negative rather than the whole block disappearing:
```
  UNATTRIBUTED:           -312 bytes  (kernel objs, libc/VFS/FS/NVS, C++ ctors, driver heap)
```
(No percentage is printed on the residual: `(-312*100 + 25974)/51948` truncates toward zero and would print `0%`.)

---

## 5. Blind-safe vs board-required

**Safe to apply and verify by compiler alone** (all APIs are unconditional declarations in `esp_heap_caps.h:283, 449, 471, 481, 495, 504` with no Kconfig gate; the component has no `-Werror` — `CMakeLists.txt:562-565` adds only two `-Wno-error=` flags):

- E1, E4, E5, E6, E7, E8, E12, E13, E14, E15, E17 — pure structure, deletions, relabels.
- E2, E3 — the `dram_info` derivation is provably identical to the calls it replaces (`heap_caps.c:282` vs `multi_heap.c:432`; `heap_caps.c:296-306` returns the field verbatim). VERIFIED
- E16 — arithmetic only; verify the `-Wformat` types (`%ld` for the `long` residual) compile clean.
- Confirm the `system_tasks_total` `-Wunused-variable` warning is **gone** from `build.log` and no new one appeared for `static_total` / `total_known`.

**Requires hardware, first boot:**

- **Print raw `dram_span_raw`, `dram_info.total_free_bytes`, `dram_info.total_allocated_bytes`, `alloc_meta`, `dram_used` side by side on one boot before trusting the arithmetic.** The identity `dram_used == (A − R) + M` is proved from IDF sources, but the reserve pool's double registration (`esp_psram.c:508-517`) is odd enough to deserve one direct observation. `A` should land ~46-48 KB if the 32 KB reserve block is inside it; ~14-16 KB if not (in which case the identity is wrong and `alloc_meta` must be re-derived). Temporary diagnostic line, removed after.
- The measured per-task stack sizes — confirm they equal 8704/4096/2048/1536/1536 for the system tasks and that `tcb_count == actualCount`.
- The live `alloc_meta` value, to replace the band in §4 with a real number.
- E19 on hardware with `DEBUG_MEMORY` set, watching both reports in the same stream.

---

## 6. Must be verified on EVERY board config, not just xiao_s3

Board-gated code hides compile breaks — green on the minimal build proves only the minimal build.

**The earlier draft's matrix was wrong.** All five profiles (`boards/xiao_s3.defaults`, `feathers3.defaults`, `feathers3_fe.defaults`, `feather_esp32_v2.defaults`, `qtpy_esp32.defaults`) carry `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`, `CONFIG_FREERTOS_NUMBER_OF_CORES=2`, `CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2`, `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`, `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`. VERIFIED across all four existing `build-*/sdkconfig` files. So:

- **The `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL == 0` arm of `hw1InternalTotalBytes()` is not exercised by any config in this repo.** The claim "the fix removes the most dangerous `#if`" is untested rather than verified. Flagged, not papered over.
- Every board is dual-core, so `IDLE1`/`ipc1` always exist.

Use `tools/build_board.sh`:

| Config | Why |
|---|---|
| `xiao_s3` | S3, octal PSRAM — the reference build |
| `feathers3` | S3, quad PSRAM — the primary board |
| `feather_esp32_v2` **or** `qtpy_esp32` | **Classic ESP32.** `soc_memory_regions[]` has 43 entries (`components/heap/port/esp32/memory_layout.c`) vs 11 on S3 — this is where a heap-count overflow of `HeapSpanTable::kMax` would show. Confirm the `!! heap span table full` line does **not** fire, and that `tcb_count == actualCount`. |
| one `ENABLE_WIFI=0` | E12's block must compile print-only with no orphaned accumulator |
| one `ENABLE_HTTP_SERVER=0` | E14 leaves `:4202-4211` intact; the `#else` arm must still build |
| one full-feature build (G2 + ESP-NOW + LLM) | `System_LLM.cpp:268-277` creates `llm_gen` via `xTaskCreateStaticPinnedToCore` from `heap_caps_malloc`'d **internal** buffers, and without `taskStackRecord` — the one live case where a task lands in the "other" bucket while being ours. Confirm it measures, does not `?`, and does not abort. |

**On every board**, check the boot report for: no `!! heap span table full`, no `(N task stack(s) outside every registered internal heap)` unless expected, `Task control blocks: N of N tasks measured`, and a non-negative `UNATTRIBUTED`.

---

## 7. What no agent could verify

1. **`alloc_meta`'s exact printed value.** The live internal allocated-block count is not statically determinable. Band 4,400–5,400 B; hardware settles it.
2. **Whether the 32,768 B reserve block is inside `A`.** Inferred from the census's byte-exact reconciliation, never observed directly. If it is *excluded*, `dram_used` double-corrects and `alloc_meta` is wrong by 32 KB. One boot print of raw `A` settles it — this is the single highest-value hardware check.
3. **Whether any linked IDF/third-party component (Bluedroid, esp_wifi, ESP-SR) creates a task with a genuine `.bss` stack on a full-feature build.** None on the minimal build. This is precisely why the range guard is mandatory, not defensive.
4. **Whether `docs2/check_docs.py` is wired into CI.** No `.github/` directory exists in this repo, so nothing currently fails a build — but the checker and `docs2/meta/baselines/check_docs.json` exist and are run via `docs2/docsctl.py`. The docs debt is real, its enforcement path is not confirmed.
5. **Whether any user-authored automation or saved command sequence on the device pipes `memreport` output somewhere.** That lives in device flash (`/automations`), outside the repo.
6. **The interrupts-off cost of the existing `uxTaskGetSystemState`** (`tasks.c:2976` → `taskENTER_CRITICAL`, non-SMP dual-core, byte-scanning every untouched stack). Direction is certain — it scales with total unused stack — magnitude is an estimate. This change adds no new call there.
7. **The exact broadcast-call count on a fully-featured build.** 92 is the static count for `3828-4355`; the allocation-traffic section emits up to 15 extra rows (`kMemReportTopCount`, `:3829`) and several sections are `#if`-gated, so the live worst case is higher.