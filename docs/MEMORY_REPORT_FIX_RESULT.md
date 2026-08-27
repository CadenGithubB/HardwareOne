# Memory report fix — what was applied (2026-08-23)

> Companion to `docs/MEMORY_REPORT_FIX_PLAN.md` (the why) and `docs/MEMORY_REPORT_FIX_STEPS.md`
> (the how, E1-E19). This file records what actually landed, where it deviates from the steps,
> the build matrix, and what still needs hardware. **Uncommitted. Boot report HW-validated on the
> bench XIAO 2026-08-23 (see "Hardware result"); the 60 s `reportAllTaskStacks()` and the
> full-feature build are still unchecked.**

## Files changed

| file | change |
|---|---|
| `components/hardwareone/System_TaskUtils.h` | `struct TaskHeapMeasure { stackBytes, tcbBytes, regBytes }` + `taskHeapMeasureSnapshot()` — the one shared measurement helper |
| `components/hardwareone/System_TaskUtils.cpp` | helper implementation (anonymous-namespace walker + the public function, right after `taskStackLookup()`); `reportAllTaskStacks()` rewritten to consume it (E19) |
| `components/hardwareone/System_Utils.cpp` | `printMemoryReport()` Section 2 + TOTALS rewritten (E2-E16); `#include "HAL_Input.h"` dropped (E17) |
| `docs2/files/System_Utils.cpp.md`, `docs2/files/System_TaskUtils.cpp.md`, `docs2/systems/boot-and-lifecycle.md` | re-stamped anchors and prose |
| `docs/ESP32_PITFALL_AUDIT.md` | three findings marked RESOLVED with a status line |
| `docs/MEMORY_LAYOUT.md` §7 | "re-measure before acting on figures quoted here" note |

## The design question — answered

`configRECORD_STACK_HIGH_ADDRESS` is **1** (hard-defined, `esp-idf/components/freertos/config/include/freertos/FreeRTOSConfig.h:160` — the port requires it). It does not help: `TaskStatus_t` only exposes `pxEndOfStack` when `portSTACK_GROWTH > 0` (`FreeRTOS-Kernel/include/freertos/task.h:174`), and Xtensa's `portmacro.h:115` is `-1`. `vTaskGetInfo` copies only `pxStackBase` (`tasks.c:4655`). So the kernel offers **no** allocated-depth for a running task, and the only true runtime measurement is the allocator block behind `pxStackBase` — which is what was implemented. No `CONFIG_*` name-matching fallback exists anywhere in the new code.

## Deviation from `MEMORY_REPORT_FIX_STEPS.md` — read this

The steps (E1/E9) measured each pointer with a span table + `heap_caps_get_allocated_size()` + a 64 KB plausibility clamp. **The applied code walks the heaps instead:**

```
taskHeapMeasureSnapshot(snapshot, out, n):
  out[i].regBytes = taskStackLookup(name)        // once per task
  heap_caps_walk(INTERNAL|8BIT, walker, ctx)     // ONE walk, all internal heaps
  walker: for each LIVE block, if block.ptr == snapshot[i].pxStackBase → out[i].stackBytes = size
                               if block.ptr == snapshot[i].xHandle     → out[i].tcbBytes   = size
```

Why: `heap_caps_get_allocated_size()` ends in `assert(find_containing_heap(ptr))` (`heap_caps.c:465-466`, assertions on for every board) and does no used/free check (`multi_heap.c:57` → `tlsf_block_size`). The walk reports only a block that is **live and starts exactly at the pointer** (`tlsf_walk_pool` hands `block_to_ptr`, `block_size`, `!block_is_free` — `tlsf.c:205-224`), so the abort hazard, the span-table overflow flag, the coalesced-free-block hazard and the plausibility clamp all disappear by construction. Attacks 1 (cap set) and 4 (`taskStackLookup` once) from the steps still apply and are honoured. The walker runs under each heap's `portENTER_CRITICAL` (`multi_heap.c:442`) and is a pointer-compare loop only — the function already did two such walks (`heap_caps_get_info` + `get_largest_free_block`); E3 removed one, this adds one back.

`block.ptr` equals what `pvPortMalloc` returned: `MULTI_HEAP_ADD_BLOCK_OWNER_OFFSET` is 0 without `CONFIG_HEAP_TASK_TRACKING` (unset on every board) and `CONFIG_HEAP_POISONING_DISABLED=y`.

Everything else in E1-E19 was applied as written, with these small differences:
- E2's `alloc_meta` comment cites `multi_heap_get_info_impl` (`multi_heap.c:431`: `total_allocated = pool - tlsf_size - free - 4B*allocated_blocks`), verified in source.
- E19 goes further than the four listed items: known-task rows use the measured stack block when present (compile-time constant only as fallback), the TCB column prints a measured value or `?`, unmeasured kernel rows print `?` and are excluded from TOTALS with a trailing count line, and the summary says "N of M tasks measured".
- `TaskHeapMeasure` carries `regBytes` too, so the boot report's application/other split is driven by the registry through the same helper rather than a second lookup pass.

## Build matrix (all green, no warnings in the changed regions)

| config | where | result |
|---|---|---|
| `xiao_s3`, minimal (every flag 0 → `ENABLE_WIFI=0`, `ENABLE_HTTP_SERVER=0`) | the live tree, `build-xiao_s3/` | ✅ 1,089,168 B |
| `feathers3`, full carrier config (NETWORK/WEB/I2C level 4, HTTPS, DISPLAY 1, INPUT 1, BT, G2, R1, TestSuite, automation, UART host link, Pi power/fan, bonded, LLM backend + CM5) | isolated APFS clone in the session scratchpad (`scratchpad/full/`), so the live `System_BuildConfig.h` was never touched | ✅ 5,711,664 B (10 % free) |
| `feather_esp32_v2` (classic ESP32), minimal flags | isolated clone `scratchpad/min32/` | ✅ 966,800 B |

The old `system_tasks_total` `-Wunused-variable` warning is gone; nothing new appeared for the two files.

## Hardware result — minimal xiao_s3, boot #6, 2026-08-23

```
  Application task stacks (used / allocated):
    debug_out             3300 /  4096 bytes (80% used)
    cmd_exec_task          932 /  8192 bytes (11% used)
  Other task stacks (kernel / unregistered, used / allocated):
    main                  4400 /  8704   IDLE1 804 / 1536   IDLE0 720 / 1536   esp_timer 740 / 4096
    Tmr Svc                676 /  2048   ipc1  740 / 1536   ipc0  740 / 1536
  Stacks (application):  12288 bytes  ( 2 tasks)
  Stacks (other):        20992 bytes  ( 7 tasks)
  Task control blocks:    3392 bytes  ( 9 of  9 tasks measured)
---------- TOTALS ----------
  ACTUAL DRAM USED:      51948 bytes (  50 KB)
  Task stacks + TCBs:    36672 bytes (  35 KB)  measured
  Allocator metadata:     4888 bytes (   4 KB)  measured
  ATTRIBUTED:            41560 bytes (  40 KB) [80%]
  UNATTRIBUTED:          10388 bytes  (kernel objs, libc/VFS/FS/NVS, C++ ctors, driver heap)
```

- Every system stack measures exactly the literal in the binary (census row 1). No `!!` lines, no
  unmeasured task, `9 of 9`.
- **TCBs 3,392, not 9 × 376 = 3,384.** One TCB sits in a 384 B block: TLSF will not split off an
  8 B remainder. The allocator figure is the true held size — keep it.
- **`alloc_meta` 4,888** — inside the 4,400–5,400 band, so the reserve-pool identity in the E2
  comment holds (wrong would have read ~37 KB). The 528 B over the census's 4,360 is 132 live
  blocks × 4 B header, which the census said it could not count statically.
- **`UNATTRIBUTED` 10,388** — inside the predicted 9,884–10,884; census rows 4–17. The ~3 KB
  genuinely-unexplained residual is still in there and still honest.
- `ATTRIBUTED + UNATTRIBUTED == ACTUAL DRAM USED` to the byte.
- `BSS (Internal)` 10,032 → 10,040: the two new function-static `TaskHeapMeasure*` cache
  pointers (4 B each). Recorded because this project tracks that number.

## Hardware result — 60 s `reportAllTaskStacks()`, minimal build, `debugmemory 1 temp`

Every row numeric, no `?`; TCB column 376 for all but `debug_out` (384); `TOTALS … 3392`;
summary `3392 B (9 of 9 tasks measured)`. Per-task figures agree with the boot report.

## Hardware result — full carrier build (WiFi+HTTP+I2C 4+OLED+gamepad; BT compiled in, not started), boot #7

```
  Stacks (application):  12288 bytes  ( 2 tasks)
  Stacks (other):        45824 bytes  (12 tasks)   main, IDLE×2, tiT, Tmr Svc, arduino_events,
                                                   esp_timer, httpd, sys_evt, wifi, ipc×2
  Task control blocks:    5376 bytes  (14 of 14 tasks measured)
  WiFi Driver:   ~  32768 bytes (32 KB) (estimate; inside UNATTRIBUTED)
---------- TOTALS ----------
  ACTUAL DRAM USED:     103620 bytes ( 101 KB)
  Task stacks + TCBs:    63488 bytes (  62 KB)  measured
  Allocator metadata:     5436 bytes (   5 KB)  measured
  ATTRIBUTED:            68924 bytes (  67 KB) [67%]
  UNATTRIBUTED:          34696 bytes
```

- No `?`, no `!!`. Every IDF task measures its config literal (`wifi` 6656 matches the driver's
  own `stack:6656` log line; `httpd` 7680, `tiT` 3584, `sys_evt` 2816, `arduino_events` 4096).
- **Every TCB is 384 here, vs 376 on the minimal build — and `sizeof(TCB_t)` is 376 in BOTH ELFs
  (gdb).** This is the allocator, and it is real: ESP's TLSF rounds every allocation ≥
  `small_block_size` up to its size-class granularity *in the allocation itself*
  (`tlsf_control_functions.h:325-333` `mapping_search(control, size_t* size, …)` →
  `*size = align_up(*size, round)`), and the granularity depends on the **pool size**
  (`tlsf.c:45-47`: ≤16 KB → 32 B steps at this size, ≤256 KB → 16 B, >256 KB → 8 B). Minimal
  region 1 is 307,784 B (>256 KB → 376 stays 376); full-build region 1 is 224,336 B (≤256 KB →
  376 → 384). The minimal build's lone 384 was `debug_out`, whose TCB came from the 22 KB region 2
  (registered post-scheduler, sorts first). A `sizeof(TCB_t) × N` estimate would be 112 B low on
  this build; the measurement is not.
- `UNATTRIBUTED` 34,696 bounds the WiFi/LWIP/httpd/ESP-NOW *internal* heap at ≤ ~24 KB once the
  ~10.4 KB base residual is taken off — the print-only "~32 KB" WiFi line is a high estimate
  (`WiFi/LWIP prefer SPIRAM` is on). Harmless, since it is never summed.
- `sys_evt` 75 % used (IDF default 2816) and `cmd_exec_task` 65 % at boot (it ran `openhttp`) —
  the census's "shrink `CMD_EXEC_STACK_WORDS`" knob is NOT safe on a full build.
- `llm_gen` check is N/A for this config: `ENABLE_LLM_SOURCE_ONBOARD` is 0 in the carrier build,
  so `System_LLM.cpp`'s static-created task is not compiled. BT tasks were not running (BT off by
  default) — `openble` + `memreport` is the remaining natural check.

## Hardware result — full carrier build with Bluetooth ON (G2 + R1 connecting), WiFi not started, boot #8

```
  Application (8): r1_owner 6144, cmd_exec_task 8192, g2_ctrl_owner 6144, g2_ble_connect 5120,
                   debug_out 4096, g2_page_swap_w 8192, g2_tap_disp 10240, g2_session_w 10240
  Other (11):      main 8704, IDLE×2, ipc×2, BTC_TASK 8704, esp_timer 4096, btController 4096,
                   hciT 2560, BTU_TASK 4864, Tmr Svc 2048
  Stacks (application):  58368   Stacks (other): 41216   TCBs: 7296 (19 of 19 measured)
  ACTUAL DRAM USED 164524 | Task stacks + TCBs 106880 | Allocator metadata 4996
  ATTRIBUTED 111876 [68%] | UNATTRIBUTED 52648
```

- No `?`, no `!!`. Every Bluedroid/controller task measured; registry split correct
  (all `g2_*`/`r1_*` under Application). TCBs 19 × 384 (pool-size rounding, as above).
- **`BTU_TASK` 84 % used at boot** (4,092 / 4,864, `CONFIG_BTU_TASK_STACK_SIZE` 4352 + 512) during
  GATT discovery of two peripherals. The old report printed its HWM with no denominator.
- **`main` 77 % used (6,760 / 8,704)**; `[MEMSAMPLE]` flags `MainLoop stack free=1944 B !! LOW`.
  **The census's tunable-stack table (main 8192→6144, esp_timer 3584→2048) was measured on the
  minimal build and is NOT safe on a BT-enabled build** — 6144 would overflow here.
- `g2_tap_disp` / `g2_session_w` hold 10 KB each at 13 % / 7 % boot usage — visible now, not a
  recommendation (boot HWM is not the image-work peak).
- Fixed after this boot: the print-only `WiFi Driver: ~32 KB` line was gated on compile-time
  `ENABLE_WIFI` only and printed here although the driver was never started. It is now also gated
  on `wifiRadioState() != WIFI_RADIO_OFF` (the documented single source of truth in
  `System_WiFi.h`), prints `(compiled in, radio off - holds nothing)` otherwise, and is labelled an
  *upper-bound* estimate — the WiFi-up boot bounded the driver's internal heap nearer 24 KB.
  Rebuilt green on xiao_s3 full and feather_esp32_v2 minimal (`ENABLE_WIFI=0`).

## Hardware checks still outstanding

- ~~1–5~~ — **done, above** (BT tasks measured, boot #8). Remaining only: `llm_gen` on an `ENABLE_LLM_SOURCE_ONBOARD=1` build.

4. `reportAllTaskStacks()` with `DEBUG_MEMORY` set: every row has a numeric Stack/TCB (no `?` on the minimal build), TOTALS TCB column is 3384, and the two reports agree per task.
5. Full-feature build: `llm_gen` (`xTaskCreateStaticPinnedToCore` from `heap_caps_malloc`'d internal buffers, no `taskStackRecord`) must land in "Other task stacks" with a measured size, not `?`, and nothing aborts. Any task created with a PSRAM stack via `xTaskCreateWithCaps` legitimately prints `?`.
6. Classic ESP32: confirm no `(N task stack(s) outside every registered internal heap)` line on a plain boot.

## Review (adversarial, 5 lenses → 10 findings → 2 skeptics each)

Run after the edits landed: lenses were walk/race, arithmetic, output transport, build gates,
and reporter consistency. **Arithmetic and gating returned nothing.** The other three produced
ten findings, all real on the snapshot they reviewed, all fixed in the live source before the
verify phase (every skeptic pair refuted the finding *because the fix was already in the tree*):

| finding | fix applied |
|---|---|
| snapshot array sized to exactly `uxTaskGetNumberOfTasks()` — a task spawning before `uxTaskGetSystemState()` makes the kernel return **0**, and the new TOTALS would have printed `Task stacks + TCBs: 0 … measured` | `+4` headroom (as `reportAllTaskStacks()` does) **and** a `tasks_measured` flag: with no snapshot, TOTALS prints `Task stacks + TCBs: NOT MEASURED this run` and stops rather than printing 0 as a measurement |
| known-task rows fell back to the compile-time `*_STACK_WORDS` and summed it under a "(measured)" label — while the boot report excluded the same task | known-task rows now follow the same rule as every other row: measured block or `?`, never summed; the constant only feeds the WARNINGS threshold |
| `?` row one column narrower than the numeric row; TOTALS TCB value under the wrong header | format strings re-derived and checked programmatically against the header |
| registry lookups ran between the snapshot and the walk, widening the stale-pointer window | walk first, lookups after |
| `CONFIG_HEAP_POISONING_*` / `CONFIG_HEAP_TASK_TRACKING` would shift user pointers off the TLSF block start and silently turn every row into `?` | `#error` in `System_TaskUtils.cpp` — no board profile sets either; refuse to build rather than guess an untested offset |

Nothing the review raised survives in the tree. The matrix above was rebuilt on the final source.
