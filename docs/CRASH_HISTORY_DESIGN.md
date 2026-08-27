# Crash History — Design Exploration

**Date:** 2026-07-28
**Status:** Exploration only. No code changed. Recommendations, not a spec.
**Method:** 5 parallel explorations, each adversarially verified against the real ESP-IDF v5.5.1 source at `/Users/morgan/esp/esp-idf` and the real repo. Verifier corrections are folded in and flagged.
**Companion:** [PRE_1_0_HARDENING_AUDIT.md](PRE_1_0_HARDENING_AUDIT.md) §5.2, which declined an auto-safe-mode boot-loop breaker. This is the instrumentation-first alternative.

---

## 0. The test case: would this have diagnosed the 2026-07-27 BLE crash?

**Yes — decisively, and with ~15 lines plus a linker flag.**

The crash: `g2_ble_connect` created with `/*stack bytes*/ 5120` while its block comment believes "5120 words = 20 KB" and its own success log says "stack=6 KB" ([G2_Glasses.cpp:8916-8930](../components/hardwareone/G2_Glasses.cpp:8916)). Measured peak ~9.4 KB. Three contradictory beliefs in one function; actual stack 5 KB.

`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y`, so FreeRTOS **detected it and knew the task name**. The default `vApplicationStackOverflowHook` ([port.c:551](../../esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/port.c:551), `__attribute__((weak))`) builds `"***ERROR*** A stack overflow in task g2_ble_connect has been detected."` and passes it to `esp_system_abort()`. That string was printed to UART and destroyed by the 0-second reboot.

**It is recoverable.** `esp_system_abort()` routes the text into `g_panic_abort_details`, declared `extern` in the **public** header [`esp_private/panic_internal.h:21-22`](../../esp-idf/components/esp_system/include/esp_private/panic_internal.h:21). A `--wrap` on the panic handler can read it before the reboot.

> **Critical correction the verifier caught.** My earlier instinct — read `panic_info_t::reason` — would have **missed this entirely**. `esp_panic_handler()` sets `info->reason = NULL` for the whole abort class ([panic.c:293-298](../../esp-idf/components/esp_system/panic.c:293)). The abort class is where every `configASSERT`, `ESP_ERROR_CHECK` failure, heap-corruption abort, `__stack_chk_fail`, **and the stack-overflow canary message** live. You must read `g_panic_abort` / `g_panic_abort_details` directly.

Budget the reason field at **96-128 bytes**, not 24 — assert text looks like `assert failed: xQueueGenericSend queue.c:832 (pxQueue)`.

### Scorecard against this specific crash

| Field | Verdict |
|---|---|
| `g_panic_abort_details` text | ✅ **Names `g2_ble_connect` verbatim** |
| `panic_info_t::addr` (faulting PC) + `core` + `exception` | ✅ Confirms and locates |
| App ELF SHA256 (`esp_app_get_elf_sha256_str()`) | ✅ Makes the PC decodable across rebuilds |
| Free heap trend (1 Hz ring) | ✅ Would show runner-up #2, the per-reconnect `BLEClient` leak |
| Per-task stack HWM | ⚠️ Would show it, but `g2_ble_connect` must be in the sampled set |
| Reset reason alone | ⚠️ `ESP_RST_PANIC` — "software fault", not which |
| Boot-phase breadcrumb | ❌ Useless — runtime crash, not boot |
| Backtrace | ❌ Needs a coredump partition |

**Runner-up #3 (the unlocked null-out race in `onDisconnect`) remains out of reach.** A use-after-free needs the backtrace. That is the first concrete justification for the coredump partition cost — see §5.

---

## 1. Substrate: it is free, and it is not the memory the boot log shows

`RTC_NOINIT_ATTR` emits `.rtc_noinit`, placed in `rtc_data_location`, which aliases to **RTC SLOW** at `0x50000000`, length `0x2000` = **8192 bytes** — because `CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM` is *not* set (sdkconfig:1743) and ULP is off. Confirmed from the real map: `.rtc_noinit 0x50000000 0x54` — **84 bytes used, ~8076 free.**

> **Trap:** the boot log's `At 600FE038 len 00001FB0 (7 KiB): RTCRAM` is **RTC FAST**, a *different pool* that `RTC_NOINIT` never touches. Do not size against it. Also: the in-repo comment at [HardwareOne.cpp:1230](../components/hardwareone/HardwareOne.cpp:1230) says "RTC fast memory" — the map contradicts it. Doc fix.

A ring of **8 × 64 B records + 16 B header = 528 B** costs zero heap and zero DRAM. Hard ceiling ~125 records. Do **not** enable `CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM` — it converts free RTC SLOW into stolen heap.

RTC SLOW cannot be powered down on the S3, so deep-sleep survival is unconditional.

---

## 2. What can be captured, and when

The hard rule: **anything needing a lock, an allocator, or a filesystem must be pre-staged or deferred.**

**MUST happen before (1 Hz snapshot):** all heap/PSRAM figures, stack HWMs, battery, CPU frequency, live-feature mask, last-command breadcrumb. Every one needs a lock or a scheduler suspend.

**CAN happen during (panic/ISR):** plain stores to RTC SLOW; register reads; `esp_timer_get_time()`; `esp_backtrace_get_start`/`get_next_frame` (both unconditionally IRAM); `pcTaskGetName`; `esp_app_get_elf_sha256_str()` (its doc comment explicitly sanctions panic use).

**Forbidden in panic:** `malloc`, any locking FreeRTOS call, `printf`, LittleFS, and `esp_flash_*`/`esp_partition_*` — the last deadlocks because the flash guard IPCs the other core, which is stalled.

**MUST wait for next boot:** every durable write, `esp_core_dump_get_summary()`, the raw per-CPU reset-reason read.

### The panic context is safer than expected
By the time `esp_panic_handler` runs: the other core is stalled, TIMG WDTs are off, the RTC WDT is armed at **10 s** (your hard budget), and flash cache has been re-enabled by `panic_enable_cache()` — from **both** entry points, so the INT_WDT/cache-error path is covered too. A normal flash-resident function reading rodata is fine.

`-Wl,--wrap=esp_panic_handler` works because [panic_handler.c:266](../../esp-idf/components/esp_system/port/panic_handler.c:266) calls it as a plain cross-TU call, and the `target_link_libraries(... INTERFACE "-Wl,--wrap=...")` form is officially documented.

### Capture surfaces I did not know about

- **`esp_brownout_register_callback()`** — public API ([esp_brownout.h:33](../../esp-idf/components/esp_hw_support/power_supply/include/esp_brownout.h:33)), invoked *before* the reset. The **one** genuine crash-time hook, and it covers the class the panic path cannot. **Hazard:** `flash_power_down=true`, so the callback and every string/table it touches must be IRAM/DRAM — a flash rodata dereference turns a recoverable brownout into a silent hang. Store one compile-time constant and return.
- **`heap_caps_register_failed_alloc_callback()`** — signature is `(size_t size, uint32_t caps, const char* function_name)`. Targets the LLM PSRAM-starvation class *and* the BLEClient leak: records which allocation lost, in task context, before the collapse. Must be `IRAM_ATTR` and must not dereference the flash-resident `function_name` — store a hash.
- **`-Wl,--wrap=__stack_chk_fail`** — `CONFIG_COMPILER_STACK_CHECK=y` means GCC stack-protector is live across the build and its failure path is currently anonymous. Catches intra-frame smashes the canary hook cannot. Not weak, so `--wrap` is required.
- **`g_exc_frames[]`** — both cores' exception frames. Given every task is pinned and the known bugs are cross-core races, what the *other* core was doing is often the whole diagnosis.

### Corrections to earlier claims

- **`ESP_RST_TASK_WDT` is reachable.** An explorer claimed the branch at [HardwareOne.cpp:1291](../components/hardwareone/HardwareOne.cpp:1291) is dead; the verifier refuted it. TWDT hardware arms **STAGE1 = RESET_SYSTEM at 2× timeout (10 s)** independent of `CONFIG_ESP_TASK_WDT_PANIC`. Stage 1 is avoided only because the ISR feeds the timer. If interrupts are starved for 10 s, a real reset fires. **Keep the branch — it is the highest-signal case there is.** This also softens hardening-audit §5.1: a hang with interrupts *enabled* wedges forever, but an interrupts-disabled deadlock does get a hardware backstop.
- **`esp_reset_reason()` is lossy on S3.** `CPU_LOCKUP`, `JTAG`, `EFUSE`, `SDIO`, `EXT`, `PWR_GLITCH` are all unreachable; USB-JTAG and USB-UART both collapse to `ESP_RST_USB`; clock-glitch / efuse-CRC / **power-glitch** fall through to `ESP_RST_UNKNOWN`. For a battery wearable that last one is the diagnosis you most want.
- **`esp_register_shutdown_handler` is useless here** — handlers run only from `esp_restart()`; the panic path calls `panic_restart()` → `esp_restart_noos()` and bypasses them.
- **`esp_cache_err_get_panic_info()` is infeasible** — it lives behind a `PRIVATE` include dir and will not compile in app code (unlike `panic_internal.h`, which really is public).
- **The ROM "Saved PC" is not app-readable on S3.** `CONFIG_ESP_SYSTEM_HW_PC_RECORD` depends on `SOC_ASSIST_DEBUG_SUPPORTED`, absent on this chip. Use `panic_info_t::addr`.
- **Overriding `vApplicationStackOverflowHook` is largely redundant** with the panic wrap done correctly, since its message already flows through `g_panic_abort_details`. Do it only for a machine-parseable name field, and keep the abort message non-empty or you lose the record.

---

## 3. Structural traps

1. **Write the magic LAST.** `RTC_NOINIT` statics are **not volatile**, so magic-last is a compiler-reordering hazard, not a given — needs a barrier or `volatile`. `PANIC_ENTRY_COUNT_MAX` is only 2, so a wrap that faults gets one retry then a hard restart; magic-last is what makes that survivable.
2. **Version the magic on layout.** RTC SLOW is **not cleared by erasing flash** — only by true cold power-on. A reflash that reorders the struct while the device stays powered will consume old bytes through a still-valid magic and emit a plausible, wrong record. Derive the guard word from `sizeof(record)` + a hand-bumped version. (Note: RamFlush's `static_assert` is on mask width, not layout — the sizeof assert is new work, not a copy.)
3. **The 1 Hz snapshot structurally cannot capture the boot-loop case.** `periodicMemorySample()` runs from `hardwareone_loop()`; nothing in that path executes until `setup()` returns. A crash during the autostart block leaves the record holding the **previous boot's** values while the boot-phase byte says "died in initCamera" — the two halves actively contradict each other. Stamp eagerly at each phase advance with a `source:` bit.
4. **Deep sleep and commanded reboots replay a stale snapshot as a crash.** `RTC_NOINIT` survives both. Gate consume on the reset reason being in the fault set, and invalidate in the same breath as the existing `rtcRebootReasonMagic = 0`.
5. **`RTC_NOINIT` is not unconditionally durable.** `esp_restart_noos` arms the RTC WDT with `STAGE1 = RESET_RTC`; a stalled restart wipes the record *including `rtcMagic`*, so the next boot reports `crashCount=0` on a real fault. Cross-check against the NVS boot counter: if `gBootCounter` advanced but RTC came up cold, that is "RTC lost", **not** "power cycle".
6. **A crash loop rotates the first crash out of `events.log`** — and ~6× faster than first estimated: `appendLineWithCap` rotates at `cap` down to 85%, so ~39 KB dropped and ~223 KB copied **per rotation**, ≈44 rotations/day at 20 B/s. De-duplicate on crash signature and write a **write-once first-crash file**.
7. **The last 0-2 s before any crash never reaches `events.log`** — `systemEventLogTick` drains on a 2 s throttle. That is exactly the crash window. Record "N posted, M reached the file"; the delta is itself diagnostic.
8. **Cap `crash.log` at 8-16 KB, not 256 KB** — `partitions_sr_8mb.csv` has a **128 KB LittleFS in total**.

---

## 4. Privacy — the closed-vocabulary rule

**Never store the username.** `rtcRebootWho[24]` already carries one into `events.log` today. On unencrypted flash at a fixed offset, an accumulating `{username, transport, timestamp}` record is a usage-pattern dataset — it survives rotation in orphan blocks, it is pullable over the air via `espnowfetch`, and no redaction rule can ever catch it because it is the field working as intended. Store a **user index or 4-byte truncated hash** (copy the `deviceKeyFingerprint` shape), resolve to a name only at render time behind the admin gate.

**Never store command arguments.** Verb only, from `found->name` after `findCommand()`.

**Consequence:** once the content is a closed vocabulary (registry command name, reason code, watermarks, counters — no free text, no username), the privacy argument for defaulting the feature OFF evaporates. **Reject default-off** — instrumentation you must enable before the incident is not instrumentation.

---

## 5. Ranked plan

### Tier 0 — do this first (small, and it solves the actual crash)
1. `-Wl,--wrap=esp_panic_handler` capturing `g_panic_abort_details` (96-128 B), `info->addr`, `core`, `exception`, app ELF SHA256 → RTC. Magic last.
2. Render it at boot **in the pre-Serial phase** via `esp_rom_printf`. This is the only path that survives a `setup()`-phase boot loop, where the device never reaches `loop()`.
3. A `crashlog` command in the registry — this gets CLI/web/OLED/G2 for free through `executeCommand`, with `authorizeCommand` gating already applied. **Without a read surface the ring is write-only and worth nothing.**

### Tier 1 — cheap and high-signal
4. Separate **consecutive** crash counter (the existing one is cumulative-since-poweron).
5. Boot-phase byte, latched to plain RAM at the top of the pre-Serial block before the first store advances it.
6. Crash signature (fold in the ELF SHA or it is not comparable across rebuilds) + de-dup so a loop does not shred the log.

### Tier 2 — the 1 Hz vitals ring
7. Ride `periodicMemorySample()`'s existing always-on tier-1 gate — do **not** add a second timer. Add PSRAM figures, battery (record the backend id; the USB stub hardcodes 5.0 V/100%), CPU freq. Label it "at last heartbeat", never "at crash".

### Tier 3 — targeted at known bug classes
8. `heap_caps_register_failed_alloc_callback` → the LLM PSRAM class and the BLEClient leak.
9. Per-core last-checkpoint breadcrumbs (2 RTC words). Because all tasks are pinned, the `setCpuFrequencyMhz`/`gps_task` race shows up as core1 advancing while core0 is frozen mid-I2C — which a single "crashing task" string would have destroyed.
10. `--wrap=__stack_chk_fail`; brownout callback (IRAM discipline, one constant).

### Declined / deferred
- **Coredump partition.** ~1792 B permanent internal DRAM, and it must be added to **all four** partition layouts or it silently finds no partition at runtime — `partitions_sr_8mb.csv` has only 128 KB of LittleFS. It is also `#if`-gated, so a board profile missing the flags **fails to compile** (the board-gated-breakage footgun). Revisit only for the `onDisconnect` race class, which nothing else can reach.
- **SoC temperature** (no recorded thermal failure; shares a peripheral with the automation sampler), **SD-present**, **radio RSSI on the 1 Hz tick** (`WiFi.RSSI()` takes the WiFi API lock — make it event-driven or drop it).

---

## 6. Live defects found incidentally

These are **not** part of this design — they are existing bugs surfaced while verifying it.

1. **`remote:`/`@` broadcasts full plaintext credentials with zero redaction.** [System_Utils.cpp:4596-4599](../components/hardwareone/System_Utils.cpp:4596) does `broadcastOutput("[REMOTE] Sent to bonded device: " + actualCommand)` and **returns before any `logCommandExecution` call**. So `@login bob hunter2` reaches every sink including the file tee. A live credential leak, independent of this feature. Belongs in the hardening audit.
2. **`redactCmdForAudit` is fail-open with three verified bypasses** — it `return c;` unchanged on no match, and all three `logCommandExecution` call sites in `executeCommand` pass the **raw** `cmd`, not the trimmed/redacted one. It also has no test, which is why the bypasses survived; note there is no host-test harness for this component, so a test means new infrastructure.
3. **`kResetReasonLabels` has 11 entries; the enum runs to 15.** [System_Utils.cpp:1546-1573](../components/hardwareone/System_Utils.cpp:1546) renders reasons 11-15 as "Unknown" over web/BLE/MQTT, while `resetReasonName()` handles all 16 correctly. Two consumers (`buildSystemInfoJson`, `cmd_bootcount`) should collapse onto `resetReasonName`. Cheap, independently reportable.
