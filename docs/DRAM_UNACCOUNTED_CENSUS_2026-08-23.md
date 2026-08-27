# Where the 31,043 "unaccounted" bytes went

**Build:** `xiao_s3`, ESP32‑S3, 8 MB octal PSRAM, fw 0.99.91.1, image 1,086,336 B, every feature flag 0.
**Artifacts:** `/Users/morgan/esp/hardwareone-idf/build-xiao_s3/{hardwareone-idf.elf,hardwareone-idf.map,sdkconfig}`, ESP‑IDF v5.5.1 at `/Users/morgan/esp/esp-idf`.
**Scope:** investigation only. No source was modified.

---

## Lead finding: 42% of the "gap" is not consumption at all

Before the table, two things change what the number means.

**1. 8,617 B of the 31,043 is the report crediting itself for memory it never measured.** `TOTAL ACCOUNTED` = 20,905 is built from three terms, and only one of them is real:

| term | value | status |
|---|---|---|
| `app_tasks_total` (debug_out 4096 + cmd_exec 8192) | 12,288 | legitimate, measured |
| `freertos_estimate` | 8,192 | **a hard‑coded literal.** `System_Utils.cpp:4117` — `size_t freertos_estimate = 8 * 1024;  // FreeRTOS ~ 8KB` |
| `static_vars_total` | 425 | **category error.** `.bss`/`.data` statics added at `System_Utils.cpp:4215` and compared against a heap‑only figure |

Arithmetic reproduces the printout exactly: 12,288 + 8,192 = 20,480 = printed `Subtotal (static)`; + 425 = 20,905 = printed `TOTAL ACCOUNTED`. **The real heap shortfall is 51,948 − 12,288 = 39,660 B.** The printed 31,043 is that number minus 8,617 B of unearned credit.

**2. A further 4,436 B is memory nobody allocated** — per‑heap TLSF/multi_heap control structures that fall inside "used" purely because of how total and free are computed (details in row 2 below).

So of the 39,660 B the report fails to explain, **24,376 B is FreeRTOS task infrastructure the report deliberately prints but never sums**, 4,436 B is allocator overhead, and 10,848 B is everything else.

---

## Attribution table

All rows attribute the **printed 31,043 B**. Rows 1–16 are live heap the report never counts; rows 18–19 remove the two fictitious credits so the table lands on the printed number.

| # | Consumer | Bytes | V/I | Running total |
|---:|---|---:|:--:|---:|
| 1 | **FreeRTOS system task stacks (7)** — main 8,704 · esp_timer 4,096 · Tmr Svc 2,048 · IDLE0 1,536 · IDLE1 1,536 · ipc0 1,536 · ipc1 1,536 | 20,992 | **VERIFIED** | 20,992 |
| 2 | **Per‑heap allocator metadata** — `multi_heap` `heap_t` (20 B) + TLSF `control_t` across the 5 registered INTERNAL\|8BIT heaps: 1,772 + 736 + 736 + 380 + 736 | 4,360 | **VERIFIED** | 25,352 |
| 3 | **Task control blocks** — 9 live tasks × `sizeof(TCB_t)` 376 | 3,384 | **VERIFIED** | 28,736 |
| 4 | Debug subsystem queues — `gDebugFreeQueue` + `gDebugOutputQueue`, 2 × (92 + 192×4 + 4) | 1,728 | INFERRED | 30,464 |
| 5 | LittleFS mount buffers — rcache 512 + pcache 512 + lookahead 128 + `lfs_t` 128 + `esp_littlefs_t` 128 + fd cache 16 + 2 mutexes | 1,640 | INFERRED | 32,104 |
| 6 | NVS runtime objects — `Page[4]` + `Storage` + `PageManager` + hash‑list blocks + namespace entries | ~1,000 | INFERRED | 33,104 |
| 7 | Arduino HWCDC serial — tx mutex 96 + rx queue 352 + tx ringbuf 376 + intr records 28 | 852 | INFERRED | 33,956 |
| 8 | newlib stdio — FILE glue block 432 + 2 × 132 stream buffers + `environ` 16 | 712 | INFERRED | 34,668 |
| 9 | `initMutexes()` — 7 unconditional FreeRTOS mutexes × 96 | 672 | INFERRED | 35,340 |
| 10 | VFS registrations + duplicated ops tables (4 mount points) | ~400 | INFERRED | 35,740 |
| 11 | `esp_partition` list — 5 rows × (52 + 4) | 280 | INFERRED | 36,020 |
| 12 | `heap_caps` registry — `heaps_array` (4 × 36 + 4) + 2 late `heap_t` blocks (add‑region) | 228 | INFERRED | 36,248 |
| 13 | `gCmdExecQ` — `xQueueCreate(8, sizeof(ExecReq*))` | 128 | INFERRED | 36,376 |
| 14 | Capture‑crypto function‑local static mutex | 96 | INFERRED | 36,472 |
| 15 | `esp_task_wdt` — 1 `twdt_obj_t` + 2 idle entries × (16+4) | 60 | INFERRED | 36,532 |
| 16 | TLSF 4‑byte block headers on the 19 stack/TCB/reserve blocks | 76 | **VERIFIED** | 36,608 |
| 17 | **RESIDUAL — live heap I could not name** | **3,052** | — | **39,660** |
| 18 | *less* the report's hard‑coded `FreeRTOS: ~8192` credit | −8,192 | **VERIFIED** | 31,468 |
| 19 | *less* the 425 B of `.bss` statics credited to a heap‑only total | −425 | **VERIFIED** | **31,043** ✔ |

The top of the ledger closes to the byte against the boot log's own totals; the honest uncertainty is row 17, ~3.1 KB (7.7% of the real 39,660 B shortfall).

### Evidence for the VERIFIED rows

**Row 1 — stacks, read out of the binary, not out of sdkconfig.** Every literal is the third argument to `xTaskCreatePinnedToCore` or the size passed to `pvPortMalloc`:

```
420a70a0: l32r a12, (2200)      main       = 0x2200 = 8704   (8192 + TASK_EXTRA_STACK_SIZE 512)
420845a0: l32r a12, (1000)      esp_timer  = 0x1000 = 4096   (3584 + 512)
4037f32b: l32r a6,  (800)       Tmr Svc    = 0x800  = 2048
4037f2eb: movi a10, 0x600       IDLE       = 0x600  = 1536   (×2 cores)
42005093: movi a12, 0x600       ipc        = 0x600  = 1536   (×2, loop at 420050c1 beqz a7)
```

Units are **bytes, not words**: `portmacro.h:88` `#define portSTACK_TYPE uint8_t`, and `xTaskCreatePinnedToCore` at `0x403818c8` does `mov.n a10, a4` straight into `pvPortMalloc` — no shift. All are heap: `heap_idf.c:43,55` makes `pvPortMalloc` = `heap_caps_malloc(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)`, and `portable.h:191` aliases `pvPortMallocStack` to it. Every reported HWM is strictly less than the allocation (main 4,232 < 8,704; esp_timer 3,364 < 4,096; …), so the sizes are self‑consistent with the log.

**Row 2 — the counter‑intuitive one.** `heap_caps_get_total_size` sums `(heap->end - heap->start)` (`heap_caps.c:271`) while `heap_caps_get_free_size` sums `heap->free_bytes` (`:283`), which `multi_heap_register_impl` initialises to `span − sizeof(heap_t) − tlsf_size()`. So every registered heap reports 20 + `control->size` bytes as "used" with zero allocations in it. `control->size = 36 + 4·fl_index_count + 4·(fl_index_count · sl_index_count)` (`tlsf.c:42-57`; `sizeof(control_t)`=36 confirmed by DWARF). Re‑derived per region: 1,752 / 716 / 716 / 360 / 716 → +5×20 = **4,360**.

This is confirmed *on the running device*: predicted RTCRAM free = 8,112 − 20 − 360 = **7,732**, and the boot log's own `INTERNAL free 319031 − DMA-able free 311299 = 7,732` exactly. RTCRAM is the only INTERNAL|8BIT heap without `MALLOC_CAP_DMA`, so that subtraction isolates it. Same identity also proves RTCRAM holds zero allocations.

**Row 3 — TCBs.** `sizeof(TCB_t)` = 376 (gdb DWARF), confirmed twice in code: `4037f2f3: movi a10, 0x178` → `pvPortMalloc` (idle), and `403818e4: movi a12, 0x178` + `memset` in `xTaskCreatePinnedToCore`. Nine tasks, and the list is exhaustive — the report enumerates via `uxTaskGetSystemState` (`System_Utils.cpp:4015`). The 376 B already contains the per‑task newlib `struct _reent` (240 B) and the 8 TLS pointers, so there is no separate allocation for either.

**Total reconciliation, byte‑exact.** Registered INTERNAL|8BIT spans: 0x4B250 (307,792) + 0x5724 (22,308) + 0x8000 (32,768) + 0x1FB0 RTCRAM (8,112) + 32,767 (the DMA reserve pool, registered as `[start, start+size−1]`) = 403,747. `hw1InternalTotalBytes()` subtracts `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` = 32,768 → **370,979** = printed Total. 370,979 − 319,031 = **51,948** = printed Used. That the total lands exactly also proves the 32 KB reserve was taken as a *single* chunk.

---

## Corrections applied to earlier claims

The table above uses corrected values. Where an earlier analysis was wrong, here is what changed and why:

| Claim | Was | Now | Why |
|---|---:|---:|---|
| FreeRTOS timer‑daemon queue | 256 B of heap | **0** — REFUTED | It is static `.bss`. `nm -S`: `3fc9e074 5c b xStaticTimerQueue$14` and `3fc9e0d0 a0 b ucStaticTimerQueueStorage$15`. `configSUPPORT_STATIC_ALLOCATION=1`, so `timers.c` takes the `xQueueCreateStatic` arm. Counting it invents heap that does not exist. |
| "Reserving pool of 32K" line | 32,768 B / 779 B | **~4 B incremental** | The 32 KB block is re‑registered as a second heap whose free space is counted again, and `hw1InternalTotalBytes()` already cancels the double count (`System_MemUtil.h:38-45`). 736 B of the 779 is that heap's own `heap_t`+`control_t`, already inside row 2. Only the block header survives. **This line in the boot log is a dead end — do not chase it.** |
| newlib stdio | 552 / 1,476 / 2,048 B | **712 B** | `BUFSIZ` is **128**, not 1024 — `sys/config.h:197` defines `__BUFSIZ__ 128`, and the binary confirms: `__swhatbuf_r` at `420a05a9: movi a8, 128; s32i a8, a4, 0` is the `*bufsize = BUFSIZ` fallback, taken because `usb_serial_jtag_fstat` memsets the stat (`st_blksize`=0). And the three std FILEs are **not** the static `__sf` array — `esp_libc_init` memsets `__sglue` before any `fopen`, so `__sfp` mallocs a 428‑B glue block instead: `4208da05: movi a11, 0x1ac` → `_malloc_r` (0x1ac = 12 + 4×`sizeof(FILE)` 104). |
| `esp_task_wdt` | 84 / 96 B | **60 B** | No `esp_timer` object. `esp_task_wdt_impl_timer_allocate` at `0x42083980` calls `wdt_hal_init`/`TIMERG0` — the timergroup implementation, not the esp_timer one. |
| Arduino HWCDC | 824 / 1,100 B | **852 B** | 824 omitted the `esp_intr_alloc` records (8+4 and 12+4 = 28 B). 1,100 wrongly included the peripheral‑manager pin tables, which are `EXT_RAM_BSS_ATTR` (PSRAM `.bss`). |
| `esp_partition` list | 240 B | **280 B** | `sizeof(partition_list_item_t)` = 52 (DWARF), not 48. Five rows: nvs, nvs_key, phy_init, factory, littlefs. |
| `sizeof(vfs_entry_t)` | 56 B | **20 B** | DWARF. The 600 B VFS row built on 56 is not defensible; ~400 B is a midpoint (150–600) because I did not determine which of the four registrations duplicate their ops tables. |
| Allocator metadata | 4,364 B | **4,360 B** | The extra 4 B is a block header, moved to row 16 so it is counted once. |
| Residual "unattributed" | 8,351 / 3,472 / 2,524 B | **3,052 B** | The 8,351 figure silently absorbed 3,624 B of allocator metadata it had not named. The others mis‑sized stdio and HWCDC. |

Also worth recording: `sizeof(nvs::PageManager)` = 40 and `sizeof(nvs::NVSHandleSimple)` = 20 (one analysis had these swapped), and `sizeof(vfs_littlefs_file_t)` = **648**, not 84 — which matters below.

---

## What `ACTUAL DRAM USED` actually measures

It is **heap only**. Nothing else can be in it.

```cpp
// System_Utils.cpp:3841-3843
size_t dram_total = hw1InternalTotalBytes();
size_t dram_free  = hw1InternalFreeBytes();
size_t dram_used  = (dram_total > dram_free) ? (dram_total - dram_free) : 0;
```
```cpp
// System_MemUtil.h:24, 38-46
inline size_t hw1InternalFreeBytes()  { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
inline size_t hw1InternalTotalBytes() { size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                                        ... if (total > reserved) total -= reserved; }
```

`.bss`, `.data` and `.noinit` are carved out *below* the heap floor and are invisible to it. Proof: `objdump -h` puts `.dram0.bss` at 0x3fc9bd90 size 0x2730, ending at **0x3fc9e4c0** — which is exactly section 21 `.dram0.heap_start` and exactly the base of heap region 1 in the boot log (`At 3FC9E4C0 len 0004B250`). No static byte can ever be inside a heap. That is why row 19 (the 425 B statics credit) is a category error, and it is why none of the 31 KB is under‑reported BSS.

The report's BSS numbers are, for the record, correct: `.dram0.bss` 0x2730 = **10,032** = printed `BSS (Internal)`; `.ext_ram.bss` 0x11364 = **70,500** = printed `BSS (PSRAM)`; both NOINIT sections are genuinely 0.

**What the report never surfaces at all** (correctly excluded from the gap, but useful context): `.dram0.data` = 0x3b90 = **15,248 B** of initialised statics, and `.dram0.dummy` = 0x10200 = **66,048 B** of internal DRAM address space consumed by the flash‑rodata mapping window. Together with `.bss` that is **91,328 B of internal DRAM gone before `heap_init` runs.**

---

## (a) Unavoidable ESP‑IDF overhead — 32,772 B

Memory you cannot get back without changing what the chip does.

| item | bytes | note |
|---|---:|---|
| IDLE0 + IDLE1 stacks | 3,072 | 46% / 52% consumed at report time — no headroom |
| ipc0 + ipc1 stacks | 3,072 | required whenever `CONFIG_ESP_IPC_ENABLE=y` on a dual‑core build |
| esp_timer + Tmr Svc stacks | 6,144 | |
| main task stack | 8,704 | permanently live — `app_main` never returns on this firmware |
| 9 TCBs | 3,384 | 376 B each, non‑negotiable |
| Per‑heap allocator metadata | 4,360 | scales with *number of heaps*, not usage |
| TLSF block headers | 76 | 4 B per allocation |
| newlib stdio, VFS, partitions, heap registry, task WDT | ~1,680 | |
| — of which pure allocator bookkeeping | *4,436* | *unreclaimable by definition* |

One structural observation: **RTCRAM inflates every number in this report.** `MALLOC_RTCRAM_BASE_CAPS` carries `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT` in its low‑priority column and `heap_caps_match` ORs all three columns, so the 8,112 B RTC region is counted in Total (+8,112), Free (+7,732) and Used (+380) — even though, as the DMA cross‑check proves, nothing ever lands there.

## (b) Firmware‑chosen and tunable — ~14,900 B in play

Knobs and realistic savings. **These are measurements, not recommendations.**

> **2026-08-23 correction from the measured report on the full carrier build with Bluetooth on:**
> `main` ran at **6,760 / 8,704 (77 %)** and `[MEMSAMPLE]` flagged the main-loop stack LOW; the
> `CONFIG_ESP_MAIN_TASK_STACK_SIZE` and `CONFIG_ESP_TIMER_TASK_STACK_SIZE` rows below are minimal-build
> numbers and are **not safe** on a feature build. `BTU_TASK` sat at 84 % on the same boot.

| knob | current | observed use | realistic saving |
|---|---:|---:|---:|
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` (sdkconfig:1429) | 8192 (+512) | 4,472 B used, 4,232 free | 2,048 B at 6144, still ~2.2 KB headroom |
| `CONFIG_ESP_TIMER_TASK_STACK_SIZE` (:1476) | 3584 (+512) | 732 B used, 3,364 free | 1,536 B at 2048 |
| `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH` (:1645) | 2048 | 668 B used | 512 B at 1536 |
| `CONFIG_ESP_IPC_TASK_STACK_SIZE` (:1464) | 1536 ×2 | 732 B used each | 1,024 B at 1024 — but ipc runs arbitrary caller callbacks, so headroom is not really yours |
| `CMD_EXEC_STACK_WORDS` (`System_TaskUtils.h:31`) | 8192 | **928 B (11%)** | up to 4,096 B — *already inside `TOTAL ACCOUNTED`, not part of the gap* |
| `DEBUG_OUT_STACK_WORDS` (`System_TaskUtils.h:67`) | 4096 | 3,236 B (79%) | none — this one is tight |
| `DEBUG_QUEUE_SIZE_MAX` (`System_Debug.h:197`) | 192 slots | — | 768 B at 96 slots (two queues, 4 B/slot + 92 B header) |
| `CONFIG_LITTLEFS_CACHE_SIZE` (:2518) | 512 | — | 512 B at 256 (rcache + pcache), plus 256 B per open file |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (:1339) | 16384 | — | not a heap saving; it is the *policy* that forces every app allocation under 16 KB into internal DRAM and out of the `ps_alloc` tracker's view |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` (:1341) | 32768 | — | ~780 B of metadata if removed; the 32 KB itself is not lost, it is walled into a DMA‑only sub‑heap |

## (c) Things that look like waste

1. **8,192 B of phantom credit.** `System_Utils.cpp:4117` invents a FreeRTOS figure. The real FreeRTOS heap cost on this build is 20,992 + 3,384 + kernel objects ≈ 26 KB — the estimate is off by 3×, in the direction that hides the problem.
2. **425 B of statics added to a heap‑only total** (`System_Utils.cpp:4215`). Wrong units for the comparison it feeds.
3. **The seven `initMutexes()` mutexes** (`System_Mutex.cpp:29-35`) are all unconditional. Five of them — `gFileTransferMutex`, `gTopoStreamsMutex`, `gEspNowSessionTxMutex`, `gMapCacheMutex`, `i2sMicMutex` — guard subsystems that are **compiled out of this build entirely**. 480 B of internal DRAM protecting code that does not exist.
4. **312 B of orphaned `.bss`.** `nm`: `3fc9e1c8 00000138 B __sf` — three `FILE` structs that `esp_libc_init` renders unreachable by zeroing `__sglue`, forcing a 432 B heap glue block instead. IDF's waste, not yours, but it is real on your board.
5. **Two dormant tasks are linked in.** `_ZN12_GLOBAL__N_119probationSupervisorEPv` (3,072 B stack, `System_OtaSafety.cpp:257`) and `i2c_slave_task` at `0x4205f64c` — the latter linked despite the report printing `I2C FEATURE LEVEL: DISABLED`. Neither was running at report time (the 9‑task list is exhaustive), but each would add stack + 376 B TCB if it ever starts.
6. **`port_IntStack` = 3,072 B** (`nm -S`: `3fc99680 00000c00 D`). Not heap, not part of the gap — but `CONFIG_FREERTOS_ISR_STACKSIZE=1536` × 2 cores is a sizeable slice of the 15,248 B `.dram0.data` that the report never mentions.

---

## Two report semantics worth knowing before anyone acts

**`Peak Used` 57,320 is an upper bound, not an observed peak.** `heap_caps_get_minimum_free_size` sums `multi_heap_minimum_free_size` per heap (`heap_caps.c:289-299`), across five heaps whose minima need not be simultaneous. The 5,372 B delta over `Used` is an envelope. That said, the most plausible real driver is open LittleFS files: `sizeof(vfs_littlefs_file_t)` = **648 B** plus a 512 B per‑file cache (`lfs.c:3193`) ≈ **1.2 KB per open file**.

**`frag=23%` is not fragmentation.** Free space decomposes as r1 245,760 + r2 1,476 + r3 32,032 + reserve pool 32,031 + RTCRAM 7,732 = 319,031 (and, dropping RTC, 311,299 = the printed DMA‑able free). Region 2 at 0x3FCE9710 is the ROM startup‑stack region, registered late by `heap_caps_enable_nonos_stack_heaps`; `heap_caps_init.c` sorts registered heaps smallest‑first, so it absorbs every post‑scheduler allocation and is now ~93% full. Nothing is stranded in the middle of the 300 KB region — a 240 KB contiguous DMA buffer would still succeed.

---

## What remains genuinely unknown

**3,052 B (row 17), plus the soft edges of rows 5–15.** Static analysis reached its limit here for one specific reason: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` means every `new`, `std::string`, `std::vector`, Arduino `String` and plain `malloc` under 16 KB lands in internal DRAM by policy — and none of it goes through `ps_alloc`, which recorded only **96 B of DRAM across 804 attempts**. Boot‑time C++ static‑constructor traffic inside `libhardwareone.a` cannot be enumerated from the ELF.

Specific open questions:

- **Row 6, NVS (~1,000 B, range 700–1,600).** Fixed structures are measured (`Page` 84 ×4, `Storage` 104, `PageManager` 40, `NVSPartition` 16). Hash‑list block count and namespace/handle counts scale with what is actually stored on the device.
- **Row 10, VFS (~400 B, range 150–600).** `sizeof(vfs_entry_t)`=20 is solid; what is not determined is which of the four registrations pass `ESP_VFS_FLAG_STATIC` and which heap‑duplicate their ops tables (`vfs.c:485-490`).
- **Row 8, stdio.** 712 B is the floor. IDF's newlib lock retargeting (`locks.c:294-303` → `xQueueCreateMutex`) creates a 96 B recursive mutex per FILE lazily; three to six of those would add 288–576 B.
- **Row 12, heap registry (228 B).** Assumes `num_heaps` = 4, from the four `heap_init` lines in the log. If the log excerpt omits a region, add 40 B per extra heap.
- **Whether any LittleFS file was open** when the report ran. Each open file is ~1.2 KB and would move that much out of row 17.

### How to close it on hardware

Three options, in ascending cost:

1. **Cheapest, one boot, no rebuild.** Print `heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` at the `heapLogSummary` checkpoints that already exist — `HardwareOne.cpp:1407` (`boot.after_fs`), `:1622` (`boot.after_debugbuf`), `:1673` (`boot.after_task.cmd_exec`). Three deltas bracket LittleFS, the debug system and cmd_exec independently and would confirm or kill rows 4, 5 and the app‑stack accounting in one run.
2. **`heap_caps_dump(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` at the end of `setup()`.** Prints every live block with its size and region. Resolves the entire 10,848 B "other" bucket, including row 17, in a single boot.
3. **One build with `CONFIG_HEAP_TASK_TRACKING=y`** (currently unset, sdkconfig:1716). Attributes every block to the task that created it. Costs 4 B/block while enabled, so it is a diagnostic build, not a shipping one — but it is the only method that names the app's static‑constructor traffic.

For the report itself, the same instrumentation is available in‑firmware: `heap_caps_get_info()` per region would pin rows 2 and 16 exactly, and `uxTaskGetSystemState`'s `TaskStatus_t` already carries everything needed to sum allocated system‑task stack sizes instead of printing HWM alone.