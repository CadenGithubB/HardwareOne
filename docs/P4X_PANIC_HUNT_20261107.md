# P4X-EYE reboots during STT bench work: instrumented hunt, 2026-11-07 (evening)

Follow-up to "Other findings" in `STT_MIC_PATH_INVESTIGATION_20261107.md` and to the read-only report
`HardwareOne_Speech_To_Text/private/sttraw-bench/PANIC_INVESTIGATION.md`. Branch `claude/p4-sttraw-capture`
(fix 149ec9b8, diagnostics and this doc in the next commit; the tuning-knob edits stay uncommitted). Board: the test P4X-EYE (MAC checked with esptool before every write),
driven over HTTP; serial used only for esptool and a passive listener. Production was restored and
hash-verified after every board session.

## What crash A actually was

The read-only report concluded that IDF's VFS table (`s_vfs`, internal .bss) held a stray PSRAM pointer. The
instrumented build shows the opposite: the table was intact, and an **entry's contents** were corrupted.

- On this board every VFS entry lives in PSRAM (boot registry dump: slot 0 `/dev/usbserjtag` 0x4809faec,
  slot 1 `/dev/null`, slot 2 `/dev/console`, slot 3 `/littlefs`, slot 4 `/sd`, slot 5 lwIP sockets; addresses
  from a poisoned build, +8 bytes against production). Slot 0's entry is the **first block of the PSRAM heap**:
  heap start 0x4809f0dc (`_ext_ram_bss_end`) + 20-byte multi_heap header + 2,544-byte TLSF control for 32 MB
  = pool at 0x4809fae0, first block header at 0x4809fadc, user data at **0x4809fae4**, which is the entry
  pointer in crash A's dump (S0).
- The register values match the original entry **shifted up by exactly one 32-bit word**:

  | field (offset) | original | in the crash | = original word at |
  |---|---|---|---|
  | flags (+0) | 0x8 (STATIC) | READONLY bit set (fopen -> EROFS, logged) | -4: TLSF size word 0x24 |
  | vfs (+4) | 0x4052b7f4 | 0x8 | +0: old flags |
  | path_prefix_len (+16) | 15 | 0 (matched every path as the default VFS) | +12: old offset (0) |

  That single 4-byte shift explains the logged `fopen(/littlefs/battery.csv) failed`, the default-prefix
  match for a `/littlefs` path, and the load fault at 8 + 0x2c = MTVAL 0x34, all at once.
- So the writer moved memory around 0x4809fae0 up by 4 bytes. The L2 cache line holding the entry
  (0x4809fa80-0x4809faff, 128 B) also holds the TLSF free-list heads of the largest size class, which change
  on almost every PSRAM malloc/free.

## Instrumentation (bench only)

`components/hardwareone/System_MemDiag.{h,cpp}`, compiled only when the build environment sets
`HW1_BENCH_MEMDIAG=1` (inline no-ops otherwise; host suites unaffected). Bench sdkconfig (build folder only,
not tracked): coredump to the console (ELF, `CAPTURE_DRAM`), heap poisoning light in builds 1-3.
The p4x_eye partition table has no free space for a flash coredump partition (nvs, nvs_key, phy_init,
factory 0x715000, littlefs to the end of 16 MB), so the coredump goes to the console and the passive listener.

- Locates `s_vfs` / `s_vfs_upper_bound` at boot (scan of .bss plus a dummy register/unregister), logs the
  registry and the first PSRAM blocks.
- CPU store watchpoints, armed from boot on both cores: slot 0's entry header, slot 1's entry header and the
  `/littlefs` entry header. Validated end to end with `memdiag wptest` (store into the watched entry ->
  Breakpoint panic at the store -> coredump on the console).
- 1 ms poller over the table and the upper bound; entry contents compared every second; aborts (panic +
  coredump) on any invalid change.
- `heap_caps_check_integrity_all(true)` at boot, `radiopower on`, the STT model open, before
  `sttFeatureStop` / `sttSpeakerEnd` / `sttWarmPark`, after `sttWarmPark`, after the PCM frees, and every 5 s.
- `memdiag psramtest <s>`: 512 KB of address-tagged words, cache write-back + invalidate, verify from PSRAM;
  a bad word prints its address, value and the address it was tagged for (a burst error shows as a shift).
- Hooks: `HardwareOne.cpp` (boot), `System_WiFi.cpp` (radiopower on), `System_STTLocal.cpp` (model open),
  `System_STT.cpp` (teardown), `HAL_SDCard.cpp` (SD mount/unmount bracketed as legitimate registry writes),
  `System_Utils.cpp` (`memdiag` command). `System_TaskUtils.cpp`: the task heap walker now adds the 8-byte
  poison head when poisoning is on (its `#error` blocked poisoned builds).
- Mistake on the way: build 2 watched the table with one 256-byte region (the only power-of-two region that
  covers it), which also covered a spinlock; every `memdiag arm` panicked on that spinlock's normal stores.
  Those six cycles are void. Build 3 onward watches entry headers only.

## Board runs

| run | build | what | result |
|---|---|---|---|
| boot check | 1 (poisoning) | flash, 150 s boot log | registry not located (entries in PSRAM, scan looked in internal RAM only); heap clean |
| run 1 | 2 (poisoning) | wptest + 6 crash-A cycles | void: table watchpoint caught a neighbouring spinlock |
| run 2 | 3 (poisoning) | wptest + 6 crash-A cycles (radio on, 8 s, stt start, 20 s, stop) | wptest fired correctly; 6/6 clean |
| B1 | 3 (poisoning) | 4 x 10-min web-owned `sttraw` sessions, ab1 playlist | 4/4 stopped cleanly; heap and entries clean at every checkpoint |
| B3 | 5 (no poisoning, production layout, PSRAM test) | 3 x 10-min sessions, `memdiag psramtest 690` running through each | 3/3 no reboot; ~55,600 passes x 512 KB (~27 GB) verified, 0 bad words; session 3 failed at start with "warm-up failed: Cancelled" (separate issue, see below) |
| B4 | 5 (production layout, PSRAM test) | 12-session loop, stopped after the event below | sessions 1-4 clean; **session 5: PSRAM errors then crash B** |

## Crash B reproduced (20:29, loop session 5, build 5, PSRAM at 250 MHz)

Serial log `private/sttraw-bench/memdiag-b4-loop/serial.log` (STT repo), ELF
`/Volumes/Audiodrive/stt/hw1-work/memdiag-20261107/memdiag-build5-psramtest/` (SHA 6c328e4c9, matches the dump).

1. `stt stop` at uptime 3534.9 s; teardown checkpoints all clean (`stt-pre-feature-stop`,
   `stt-pre-speaker-end`, `stt-pre-warm-park`, `stt-post-warm-park`, `stt-post-pcm-free`: heap ok, VFS ok).
   `sttWarmPark` gave ~24 MB of PSRAM back between the third and fourth checkpoint.
2. The PSRAM test's next verify pass (pass 16,323, 512 KB at 0x49b17f00) found bad words from 0x49b60780 on:
   every 32-byte chunk held the data this same pass had written **32 bytes lower**, and the first chunk held
   data written for 0x49b70760 (64 KB + 32 B away). The values decode to this pass's own tags, so this is not
   stale data and not a software overwrite with other content: PSRAM transfers landed at the wrong address.
3. ~0.2 s later, on core 1, the web server task freeing a command buffer
   (`handleCLICommand -> submitAndExecuteSync -> releaseSyncExecReqOwner -> heap_caps_free`) hit
   `CORRUPT HEAP: multi_heap.c:123` (`assert_valid_block`) on PSRAM block ~0x49f8de8c -> `abort()` ->
   reset=panic. A different PSRAM region than the test buffer, corrupted at the same moment.
4. The console coredump was lost: the USB serial device re-enumerated after the reset (listener got
   `Device not configured`); the register and stack dump were captured in full.

Crash A (4-byte shift of the first PSRAM block) and crash B (32-byte shift plus a 64 KB alias, heap block
headers broken) are the same class: PSRAM data written to or read from the wrong address under load.

## Root cause: CPU clock changes without the MSPI speed-mode sync

- The power policy holds 400 MHz during an STT session and changes the clock on the main-loop pass after it
  ends (and raises it again at `stt start`). `powerSetCpuFrequencyMhz` calls Arduino's `setCpuFrequencyMhz`
  -> `rtc_clk_cpu_freq_set_config_fast`. On the P4 this re-divides the CPU, MEM and AXI clocks
  (400 MHz: MEM 200; 200/100 MHz: MEM 200/100).
- IDF's own `esp_pm` wraps the same switch in `esp_clk_utils_mspi_speed_mode_sync_before/after_cpu_freq_switching`
  (`esp_hw_support/clk_utils.c`): on AXI chips "AXI frequency ... needs to be greater than or equal to MSPI
  PSRAM frequency to avoid writing MSPI FIFO overflow", so below `CONFIG_SPIRAM_SPEED` PSRAM is first moved to
  its low-speed mode. Arduino's `setCpuFrequencyMhz` does not do this; the firmware called it directly.
- Evidence, all on the bench P4X-EYE:
  - **250 MHz PSRAM, 40 fast STT start/stop cycles** (`ab-250`): 3 crashes in 20 cycles, each right after
    `stt-post-pcm-free` (the end of teardown, i.e. the clock drop): PSRAM test errors (32-byte shifted bursts),
    then `CORRUPT HEAP` aborts, one `cmp_lfn`/`dir_find` fault in FatFs state.
  - **200 MHz PSRAM, same cycles** (`ab-200`, build 6): no PSRAM test errors but 4 hardware watchdog resets
    (`rst:0x7 HP_SYS_HP_WDT_RESET`, no panic output) in 29 cycles, each right after `stt-post-pcm-free`.
    Lowering the PSRAM clock changes the symptom (bus hang instead of corruption), it is not a fix.
  - **No STT at all, `cpufreq 100` / `cpufreq 400` over HTTP every second** (`toggle-250`): PSRAM test errors
    with the same 32-byte shift and a `tlsf_malloc` fault on a corrupt free list within three switches.
  - Constant 400 MHz for hours of STT sessions with the PSRAM test running: ~70 GB verified, 0 bad words
    except at the session-end transitions.
- Crash A fits the same mechanism: it happened at `stt start` (the clock raise) right after `radiopower on`,
  and its victim (a 4-byte shift of the first PSRAM heap block) is the same class of misplaced PSRAM write.

## Fix (branch, `System_Power.cpp`)

`powerSetCpuFrequencyMhz` is the one owner of runtime clock changes (the only other `setCpuFrequencyMhz` call is
Arduino's startup call, which sets the boot clock the chip is already at). On P4 builds with PSRAM it no
longer calls Arduino's `setCpuFrequencyMhz`; `p4SwitchCpuClock` (IRAM) does what `esp_pm` does, plus a quiesce:

1. interrupts off on this core (`portENTER_CRITICAL_SAFE`);
2. `esp_clk_utils_mspi_speed_mode_sync_before_cpu_freq_switching` (PSRAM to low-speed mode before a drop
   below `CONFIG_SPIRAM_SPEED`);
3. other core stalled (`esp_ipc_isr_stall_other_cpu`) and the external-memory cache frozen
   (`esp_cache_freeze_ext_mem_cache`) while `rtc_clk_cpu_freq_set_config_fast` changes the CPU/MEM/SYS/APB
   dividers, then unfreeze and release (the same sequence as IDF's `mspi_timing_change_speed_mode_cache_safe`);
4. `..._sync_after_...` (PSRAM back to high speed after a raise).

Arduino's APB-change callbacks are skipped: the APB clock is fixed on the P4 (`calculateApb` returns
`APB_CLK_FREQ`). PSRAM stays at 250 MHz; STT sessions hold 400 MHz, so speech speed is unchanged. Below 250 MHz
CPU (idle 100, Balanced 200) PSRAM now runs in its 20 MHz low-speed mode, which only affects idle work.

Build 7 had only steps 2 and 4 (the plain `esp_pm` sync): the 32-byte corruption and crashes stopped, but the
toggle test still showed 2-byte shifted words at a switch (7-9 bad words per event, no crash). Step 3 removed
those.

## Verification on the board (PSRAM 250 MHz, test P4X-EYE)

| build | test | before the fix | with the fix |
|---|---|---|---|
| 5 (unfixed, diagnostics) / 8 (fixed, diagnostics) | `cpufreq 100`/`400` toggled every second, PSRAM test running, no STT | PSRAM errors and a `tlsf_malloc` fault within 3 toggles | 300 toggles: 0 bad words, 0 panics, 0 resets |
| 5 / 8 | fast STT start/stop cycles (~40 s each, `sttraw` on), PSRAM test running | 3 crashes in 20 cycles (heap asserts, FatFs fault), PSRAM errors at teardown | 40 cycles: 0 bad words (83,006 x 512 KB verified), 0 panics, 0 resets |
| 7 (sync only, no quiesce) | toggles | - | 7-9 bad words (2-byte shifts) per event, no crash: not enough |
| fix at production config (no diagnostics, coredump off, SHA d0846f08) | 150 toggles, then 40 fast STT cycles | - | 0 panics, 0 heap asserts, 0 watchdog resets; 40/40 sessions done |

Each fixed run ended with production restored and hash-verified. Host suites: 331/339 pass; the 8 failures
(`oled_owner_requests`, `optional_peripheral_config`, `ble_central_coordinator`, `ble_console_phone_link`,
`phone_sdk_manifest`, `tof_poll_outcomes`, `tool_pins`, `wifi_connect_budget`) are in code this work does not
touch; their errors are about that code (camera preview symbols, BLE harness warnings under the newer clang,
ToF events, event-catalog fixture counts). They were not re-run on a tree without these changes. The four suites the diagnostics hooks first broke (`stt_local_lm`,
`stt_continuous`, `stt_continuous_full_phrase`, `task_heap_measure`) pass. Power-policy suites
(`power_cpu_frequency`, `power_save_tick`, 696 scenarios) pass with the fix.

## Not verified / open

- Long 10-minute playlist sessions on the fixed build (the fast cycles exercise the same teardown/clock-drop
  path ~15 times faster; the 2-in-11 historical rate of crash B was not re-measured on the fixed build).
- Crash A's exact precondition (`radiopower on` without a Wi-Fi join) cannot be produced over HTTP; it is
  covered by the mechanism (clock raise at `stt start`) and by the toggle test, not reproduced as such.
- Idle-time cost: below 250 MHz CPU, PSRAM now runs at its 20 MHz low-speed mode. Not measured; STT holds
  400 MHz so captions are unaffected, but idle PSRAM-heavy work (web UI, logs) is slower at 100/200 MHz.
- Other boards: the fix is compiled for every ESP32-P4 build with PSRAM (Waveshare P4 too); only the
  P4X-EYE was tested.
- ESP32-S3 (2026-11-08): the S3 needs the sync too, for a different reason. Its MSPI core
  clock follows the CPU clock source (IDF 5.5.5 `hal/esp32s3/include/hal/mspi_ll.h:52`
  `MSPI_TIMING_LL_FLASH_CPU_CLK_SRC_BINDED`), and UltraSaver's 40 MHz idle clock runs from XTAL, so after
  Arduino's switch the MSPI would run from XTAL with the RX delays boot tuning chose for the PLL clock.
  Tuning is on for Octal PSRAM at 80 MHz (DDR, `MSPI_TIMING_PSRAM_NEEDS_TUNING` = clock > 40,
  `port/esp32s3/mspi_timing_tuning_configs.h:81`), which is the XIAO S3 default; the Quad-PSRAM FeatherS3
  boards with 80 MHz STR flash/PSRAM have no tuning, so for them the sync only lowers the MSPI clock.
  `clk_utils.c:35/57` moves the MSPI to its untuned low-speed mode before a switch onto XTAL and back after
  a switch onto PLL; IDF calls it from `esp_pm` (`pm_impl.c:708/726`), sleep (`sleep_modes.c:1030`) and the
  boot o-code calibration (`port/esp32s3/rtc_init.c:275/324`). On S3 builds `powerSetCpuFrequencyMhz` now
  brackets Arduino's `setCpuFrequencyMhz` with those two calls (`s3SetCpuFrequencyMhz`, each under
  `portENTER_CRITICAL_SAFE` because the sync freezes the external-memory cache; the after-sync uses the
  clock actually running). No other-core stall: `esp_pm` does none on the S3, and the untuned low-speed mode
  is valid on both clock sources. Host-tested (`power_cpu_frequency`, s3 profile); not run on an S3 board
  (the bench check it needs, >= 30 min UltraSaver idle with a PSRAM write/verify loop, is still open).
- Separate issues seen on the way, not fixed: STT starts that fail with `warm-up failed: Cancelled`
  (4 of 40 fast cycles, also once in a long session); the IDLE0 task-watchdog warnings at boot.
- The console coredump path works (validated with `memdiag wptest`), but the real crash's coredump was cut off
  by the USB re-enumeration after reset; the listener should reopen the port, as `memdiag_repro.py` does.
