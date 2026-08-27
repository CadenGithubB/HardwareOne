# Boot memory report — correctness fix (APPLIED 2026-08-23, uncommitted, HW-test pending)

**Status:** all edits applied (E1-E19); see `docs/MEMORY_REPORT_FIX_RESULT.md` for what was
actually done, where it deviates from `MEMORY_REPORT_FIX_STEPS.md`, the build matrix, and the
hardware checks still outstanding. Sections below are the original analysis, kept as the record.
**Prereq reading:** `docs/DRAM_UNACCOUNTED_CENSUS_2026-08-23.md` — the full validated census.

**The design question below is answered:** `configRECORD_STACK_HIGH_ADDRESS` is 1 (hard-defined in
IDF's `FreeRTOSConfig.h:160`) but it does not help — `TaskStatus_t` only exposes `pxEndOfStack`
when `portSTACK_GROWTH > 0` (`task.h:174`), and Xtensa is `-1`. The only runtime measurement
available is the allocator block behind `pxStackBase`, which is what was implemented.

---

## The four defects

All verified against the running binary and the source. Report lives in
`components/hardwareone/System_Utils.cpp`, roughly lines **3930-4350**.

### 1. System task stacks are printed but never summed — 20,992 B (the big one)

The report enumerates the system tasks via `uxTaskGetSystemState` and prints each one's **HWM**
(high-water mark = bytes still **FREE**), then adds **nothing** to any subtotal. It only ever asks
each task how much it has *not* used.

Allocated sizes, read out of the **binary** (the literal passed to
`xTaskCreatePinnedToCore` / `pvPortMalloc`), cross-checked against `build-xiao_s3/sdkconfig`:

| task | allocated | source |
|---|---:|---|
| main | 8,704 | `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 8192 + `TASK_EXTRA_STACK_SIZE` 512 |
| esp_timer | 4,096 | `CONFIG_ESP_TIMER_TASK_STACK_SIZE` 3584 + 512 |
| Tmr Svc | 2,048 | `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH` |
| IDLE0 / IDLE1 | 1,536 x2 | `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE`, dual-core |
| ipc0 / ipc1 | 1,536 x2 | `CONFIG_ESP_IPC_TASK_STACK_SIZE`, dual-core |
| **total** | **20,992** | |

**Units are BYTES, not words** — `portmacro.h:88` `portSTACK_TYPE uint8_t`, and
`xTaskCreatePinnedToCore` passes the size straight to `pvPortMalloc` with no shift. All are heap:
`heap_idf.c:43,55` makes `pvPortMalloc` = `heap_caps_malloc(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)`.

### 2. `freertos_estimate = 8 * 1024` is a hardcoded literal — `System_Utils.cpp:4117`

Added to `static_total`. The real FreeRTOS heap cost is ~26 KB (stacks + TCBs + kernel objects), so
it is **3x low, in the direction that hides the problem**.

A sibling `wifi_estimate = 32 * 1024` literal was removed on 2026-08-23 and is now behind
`#if ENABLE_WIFI` — see `System_Utils.cpp` around the same block for the precedent. **Note what
happened when it was removed:** the report flipped from a 1,713 B "over-estimate" to a 31,043 B
"unaccounted", because the phantom had been *masking* a real hole. Expect the same shape here.

### 3. `static_vars_total` (425 B of `.bss`) is added to a heap-only total — ~`:4215`

Category error. `TOTAL ACCOUNTED` is compared against `ACTUAL DRAM USED`, which is heap-only.
`.bss` must not be in that sum. Print it as its own clearly-separated section, or drop it from the
total — but do not compare bytes of different kinds.

### 4. TCBs and allocator metadata are never mentioned at all

- **TCBs: 3,384 B** — 9 live tasks x `sizeof(TCB_t)` 376 (DWARF, plus `movi a12, 0x178` in
  `xTaskCreatePinnedToCore`). The 376 already contains the per-task newlib `struct _reent` (240 B)
  and the 8 TLS pointers — do **not** add those separately.
- **Per-heap allocator metadata: 4,360 B** — `multi_heap` `heap_t` (20 B) + TLSF `control_t` per
  each of the 5 registered `INTERNAL|8BIT` heaps. This is memory **nobody allocated**:
  `heap_caps_get_total_size` sums each heap's span (`heap_caps.c:271`) while
  `get_free_size` sums `free_bytes` (`:283`), which is initialised to
  `span - sizeof(heap_t) - tlsf_size()`. An empty heap therefore reports its own metadata as "used".

**Also:** `UI Framework: ~0 bytes (untracked)` always prints zero and contributes nothing.
**Also:** `system_tasks_total` is declared at ~`:4012` and **never used** — pre-existing, confirmed
against `git show HEAD`. Decide its fate as part of this (it was almost certainly meant to hold
exactly the number defect #1 is about).

---

## THE design question — decide this first

**Can allocated stack size be measured at runtime, or must it be hardcoded?**

`TaskStatus_t` provides `pxStackBase`. If this build sets **`configRECORD_STACK_HIGH_ADDRESS`** it
also provides `pxEndOfStack`, and then:

```c
allocated = (uint32_t)status.pxEndOfStack - (uint32_t)status.pxStackBase;
```

That is a real measurement: no per-task `CONFIG_*` lookup, correct automatically when someone
changes a stack size, and correct on every board.

**CHECK `build-xiao_s3/sdkconfig` FOR THE ACTUAL VALUE — do not assume.** If it is off, the fallback
is matching task names to `CONFIG_*` values, which is brittle and board-varying — i.e. exactly the
class of hardcoded guess this whole task exists to remove. If the config is off but enable-able,
weigh its cost; that is likely the better fix.

---

## Downstream consumers — DONE, see `docs/MEMORY_REPORT_FIX_STEPS.md`

**The analysis completed and is captured in `docs/MEMORY_REPORT_FIX_STEPS.md`** — full consumer
table, the attacks that landed, and ordered edits E1-E19. Read that file; it supersedes the
checklist below, which is kept only as a description of what was asked for.

**Headlines from it:**

- **There is a COMPETING REPORT.** `reportAllTaskStacks()` (`System_TaskUtils.cpp:560`) has its own
  `TCB_SIZE = 104` — **3.6x low** against the census-verified 376 — and its own
  `kAssumedUsedBytes = 4096`. It auto-runs **every 60 s** under `DEBUG_MEMORY`
  (`HardwareOne.cpp:2554-2560`) into the same sinks. Fixing one report and not the other leaves two
  reporters disagreeing on live output. Treated as **all-or-nothing** (step E19).
- **`HEAP_WARNING_THRESHOLD = 25600`** (`System_MemoryMonitor.cpp:205`) — the specific worry — is
  **NOT affected.** It keys on `hw1InternalFreeBytes()`, and the fix does not move free heap.
  Nothing to retune. Same for the `gMemoryRequirements[]` feature gates.
- **`Static Over-Estimate`** (`:4345`) is an artifact: it prints 0 today only because
  `static_total` 20,480 < `unaccounted` 31,043. With honest subtotals it would start printing
  ~22 KB of nonsense. **Deleted, not repaired.**
- The `memreport` **JSON arm** never carried a `total_known` analogue, so there is no schema drift,
  and the Flutter/Android app does not consume it (`docs/APP_JSON_CONTRACT.md:57` — deferred set).
- **Real transport limit is not `CMD_RESULT_MAX`** — it is `BROADCAST_PRINTF`'s **256-byte stack
  buffer** (`System_Debug.h:756-763`), where truncation is **silent**. Every new line must fit.

**Four SERIOUS attacks landed and are already folded into E1-E19 — do not re-derive them:**

1. Walking heaps with `MALLOC_CAP_INVALID` matches **all** heaps including PSRAM/RTCRAM
   (`heap_caps.c:600`), which would be summed against an `INTERNAL|8BIT`-only figure. Use
   `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
2. A fixed `kMax = 12` truncates **silently**; `esp_psram.c:506-517` can register the reserve pool
   in multiple chunks. Use 16 + an overflow flag that prints loudly.
3. **Use-after-free race:** interleaving measurement with printing can read a TLSF header for a
   task that deleted itself. `heap_caps_get_allocated_size` does **no** liveness check
   (`multi_heap.c:133-136`) and there are 27 `vTaskDelete(NULL)` sites. Measure in one tight loop,
   print afterwards, clamp implausible sizes.
4. `taskStackLookup()` takes a `portMAX_DELAY` mutex over a **PSRAM-resident** 56-slot registry
   (`System_TaskUtils.cpp:110,131-136,163-180`). Call it **once** per task and cache.

**Also corrected:** all five board profiles have `CONFIG_SPIRAM=y` and
`CONFIG_FREERTOS_NUMBER_OF_CORES=2` — the "no PSRAM / single core" fallback arm is **not exercised
by any config in this repo**, so do not design around it.

**Docs debt to re-stamp in the same change:** `docs2/files/System_Utils.cpp.md:200-212` (line
anchors already stale, and `:209` cites both wrong literals), `docs2/systems/boot-and-lifecycle.md`,
`docs/MEMORY_LAYOUT.md:171,178,218,244` (tells the reader to trust `memreport` as ground truth), and
`docs/ESP32_PITFALL_AUDIT.md:1053,1066,1097` can be marked **resolved** by this fix.

---

### Original checklist (superseded, kept for context)

1. **Every CLI command that prints memory info** — `memreport`, `heap`, `perftop`, `sysinfo`,
   `taskstats`. Which share code with the boot report?
2. **A JSON or machine-readable twin.** Does the Android app, web UI, G2 lens, OLED, or an
   automation read any of these fields? Changing the shape breaks them silently.
3. **`System_MemoryMonitor.cpp` / `System_MemTracker.cpp`** — do they compute the same figures, and
   **is there a low-memory THRESHOLD or alert keyed on any of them?** This is the highest-risk
   downstream break: any threshold was calibrated while the report under-counted by ~39 KB. Making
   the number honest could start tripping warnings that were never designed for the true value.
4. **The periodic `[HEAP]` log lines** (`boot.after_debugbuf`, `boot.after_task.cmd_exec`) — same
   helpers?
5. **`hw1InternalTotalBytes()`** and every other helper the report uses — list ALL callers.
   (It subtracts `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 32,768; the totals reconcile byte-exact
   because of that, so changing it moves numbers everywhere.)
6. **`System_FeatureRegistry` `heapCostKB`**, `getEnabledFeaturesHeapEstimate`,
   `getTotalPossibleHeapCost` — the setup wizard reads `heapCostKB` from the C struct.
7. **Anything that PARSES the report text** — a test, a tool under `tools/`, a docs2 generator.
8. **Transport ceiling.** If the report is reachable as a command reply, `CMD_RESULT_MAX` is 4096
   (`System_CommandTypes.h:146`) and is documented as unraisable. Measure the current output size
   before making it longer. (`features json` already sits ~500 B from that cliff.)

---

## Constraints on the fix

- **Do not pad the residual.** The census leaves **3,052 B (7.7%) genuinely unexplained**. A report
  that honestly says "unaccounted: 3 KB" is worth more than one engineered to say zero. Padding it
  would be the exact failure mode being fixed.
- **Measure at runtime where an API exists; label estimates as estimates in the output.**
- **Every build config must compile.** This file is full of feature `#if`s. Verify on at least the
  three configs listed below. `feather_esp32_v2` is a different chip; check whether it is single
  core, because `IDLE1` / `ipc1` do not exist there and any fixed-size loop over "the seven system
  tasks" breaks.
- **Smallest change that makes the numbers true.** The owner pushed back on over-engineering more
  than once this session.

---

## Target output

Must reconcile against the observed **ACTUAL DRAM USED 51,948 B** on the minimal build:

```
app task stacks (debug_out 4096 + cmd_exec 8192)   12,288   measured
system task stacks (7)                             20,992   measured
task control blocks (9 x 376)                       3,384   measured
per-heap allocator metadata (5 heaps)               4,360   measured
kernel objects, drivers, stdio, mounts             ~10,848  partly estimated
------------------------------------------------------------
accounted                                          ~48,896
ACTUAL DRAM USED                                    51,948
unaccounted                                         ~3,052   <- keep this honest
```

`.bss` (10,032 B internal) is reported **separately** and is not part of this sum.

---

## Verification

Build all three; the minimal build alone proves almost nothing here.

```bash
tools/build_board.sh xiao_s3 build      # minimal (current tree state)
# then raise NETWORK/WEB/I2C levels + DISPLAY + INPUT in System_BuildConfig.h and rebuild
# then feather_esp32_v2 — DIFFERENT CHIP, check core count
```

Snapshots of the current config are in the session scratchpad (see the handoff doc).
