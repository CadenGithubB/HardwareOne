# Heap / internal-DRAM optimization — findings

**Date:** 2026-06-07
**Build under analysis:** XIAO ESP32-S3 (8 MB flash, 8 MB octal PSRAM), `HW_BOARD=xiao_s3`, BT enabled for G2.
**Why this exists:** After re-enabling Bluetooth for the G2 glasses, the dashboard reported only ~241 KB internal heap with little free headroom, and disabling the on-device LLM produced *no change*. This documents what actually consumes internal DRAM, what regressed, and the levers (ranked) to recover it. It is the precursor to a recommended-settings doc.

> Supersedes the stale [`MEMORY_FOOTPRINT_REPORT.md`](MEMORY_FOOTPRINT_REPORT.md) (2026-05-03 build, pre-multi-board partitions).

---

## 0. TL;DR

- **The LLM is a red herring for *internal* DRAM.** Every large LLM buffer is PSRAM-backed (`MALLOC_CAP_SPIRAM` / `EXT_RAM_BSS_ATTR` / `ps_alloc`); its only internal allocation (`stateHotData`) is runtime-only and exists *only while a model is loaded*. Disabling it freed ~125 KB of **flash**, ~0 internal DRAM. Confirmed empirically (no dashboard change).
- **The internal-DRAM constraint is real but it's death-by-a-thousand-cuts**, dominated by **task stacks**, not any single giant buffer.
- **Already-found-and-fixed regression:** `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST` was silently flipped `y → not set` by commit `9a23158` (the "BSS-to-PSRAM diversion" commit, which regenerated `sdkconfig` against the wrong PSRAM target). Restored in `sdkconfig` + pinned in `sdkconfig.defaults`. This only helps once BT is **running**.
- **Biggest untapped lever:** **all 34 task-create sites allocate their stacks from internal DRAM.** `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` is already set but **nothing uses it** (0 `…WithCaps` call sites).
- **IRAM is saturated** (16,383 / 16,384 bytes, 99.99%). No room to add IRAM-resident code; this constrains some otherwise-useful WiFi options.

---

## 1. How this was measured (reproducible)

```bash
. $IDF_PATH/export.sh
HW_BOARD=xiao_s3 idf.py size                 # link-time DRAM/IRAM/flash
HW_BOARD=xiao_s3 idf.py size-components      # per-component
# per-symbol region classification from the linked ELF:
xtensa-esp32s3-elf-nm --print-size --size-sort --radix=x build/hardwareone-idf.elf
#   classify by address: 0x3Cxxxxxx/0x3Dxxxxxx = PSRAM (ext_ram), 0x3FC8xxxx = internal DRAM
```

On-device (authoritative runtime truth): the firmware's `printMemoryReport()` / `memreport` and
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`. **Do not trust `ESP.getFreeHeap()`** — it can mix PSRAM and hide internal starvation.

---

## 2. Current memory picture

### 2.1 Link-time (`idf.py size`, this build)

| Region | Used | Used % | Remain | Total |
|--------|-----:|------:|------:|------:|
| DIRAM (static .data/.bss) | 240,911 | 70.5% | **100,849** | 341,760 |
| IRAM | 16,383 | **99.99%** | **1** | 16,384 |
| Flash code | 2,985,932 | | | |
| Flash data | 2,016,024 | | | |

### 2.2 Runtime (device dashboard, BT *not* running, no LLM model loaded)

| Pool | Used | Total |
|------|-----:|------:|
| Internal heap | 139 KB | 241 KB |
| PSRAM | 203 KB | 8,105 KB |

**Operative number: ~100 KB free internal DRAM at idle** — agrees two ways (link-time remainder 100,849 B; runtime 241−139 ≈ 102 KB). That headroom is what gets eaten as G2 spawns on-demand worker tasks (see §4) and as BT/WiFi connect — which is the "barely usable" feeling.

### 2.3 Static data by region (per-symbol, from ELF)

| Region | Static bytes | Notes |
|--------|-------------:|-------|
| **PSRAM** (ext_ram .bss/.data) | **449,996 (439 KB)** | Already diverted — 90 `EXT_RAM_BSS_ATTR` sites. The diversion machinery works. |
| **Internal DRAM** (named symbols) | 129,854 (127 KB) | App + driver static; candidate pool in §5. |

The gap between §2.1 static DIRAM (240 KB) and the 127 KB of *named* internal symbols is **unnamed IDF/library static** — WiFi, the now-compiled-in BT controller, lwIP, FreeRTOS — most of which is DMA-capable and **cannot** move to PSRAM.

---

## 3. The regression (found + fixed)

Commit `9a23158` "BSS-to-PSRAM diversion + memreport accounting fixes + NTP boot speedup" (2026-05-12), made *while G2/BT was active*, carried a 1049-line `sdkconfig` regeneration that was clearly done against the wrong PSRAM target. Collateral damage in that hunk:

```diff
-CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y
+# CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST is not set
```
(plus `SPIRAM_MODE_OCT→QUAD`, `SPEED_80M→40M`, ESP32 cache-workaround/bankswitch settings — those were corrected later when the board target was fixed, but the BT knob never was.)

`BT_ALLOCATION_FROM_SPIRAM_FIRST=y` routes Bluedroid **host** allocations to PSRAM first. It was the mechanism that kept BT-on viable during the original G2 work. **Fix applied:** restored in `sdkconfig` and pinned in `sdkconfig.defaults` (with `CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y`, which moves the BLE host env off BSS onto the PSRAM-backed heap). Effect is only visible **when BT is running** — the dashboard above shows BT *not* running, so this win isn't yet reflected.

> Note: the BT **controller** always needs DMA-capable *internal* memory; no flag moves that. `BT_ALLOCATION_FROM_SPIRAM_FIRST` only affects the Bluedroid host.

---

## 4. Where internal DRAM actually goes: task stacks

**34 task-create sites; every one allocates its stack from internal DRAM (0 use `xTaskCreate…WithCaps`).** FreeRTOS task stacks are internal unless explicitly created with `MALLOC_CAP_SPIRAM`. `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` is already enabled but unused.

Many are **transient / on-demand** (created when viewing, deleted after) so they don't all coexist — but each one that spawns carves into the ~100 KB free internal pool at exactly the moment the user is doing something:

| Task | Stack | Lifetime | PSRAM-stack candidate? |
|------|------:|----------|------------------------|
| `g2_bmp_full` | 8192 B | transient (BMP viewer) | ✅ pure compute/render |
| `g2_jpg_full` | 8192 B | transient | ✅ |
| `g2_cam_view` / `g2_cam_stream` / `g2_bmp_view` / `g2_jpg_view` | 6144 B | transient | ✅ |
| `llm_gen` | `LLM_TASK_STACK_SIZE` | transient | ✅ already PSRAM-heavy |
| `mapRender` | `MAP_RENDER_STACK_WORDS` (~32 KB) | transient | ✅ |
| `mic_record` / `mic_viz` / `cam_record` | words | transient | ✅ (verify no cache-disabled I/O) |
| `g2_tap_disp` | 6400 B | resident | ⚠️ runs BLE-notify-adjacent work — keep internal |
| `espnow_task` / `sensor_bcast` | words | resident | ❌ RF/timing critical — keep internal |
| `debug_out` | words | resident | ❌ may run during flash ops |
| `cam_pwr` | ~10 KB | transient init | ⚠️ touches camera init/I2C |

**Caveats for PSRAM stacks** (why not all):
- A task with a PSRAM stack **cannot run while the cache is disabled** (e.g. during SPI-flash writes / LittleFS/NVS commits). Tasks that write flash, or that any flash-writing path can preempt into, must stay internal.
- **ISRs and BLE/Wi-Fi callbacks cannot use PSRAM stacks.** Keep RF/timing tasks internal.
- Slightly slower (cache misses). Fine for render/compute, not for hot RF loops.

**Helper to add:** a `taskCreatePsram(...)` wrapper around `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` in `System_TaskUtils`, used only by the ✅ tasks above. Moving the heavy transient render/view tasks alone reclaims tens of KB *exactly during the load spikes that hurt*.

**Right-sizing:** recent commits bumped stacks (`45ba16b` sensor_queue_task 12→16 KB; `00d3341` sensor_bcast stabilization). The diagnostics from `74254d5` expose `uxTaskGetStackHighWaterMark`; use it to trim over-allocated resident stacks back toward real high-water + margin.

---

## 5. Internal static buffers — diversion candidates

Top internal-DRAM **named** symbols (from ELF). "Move" = add `EXT_RAM_BSS_ATTR`. **Verify each is never DMA, never touched before PSRAM init, and not in an ISR path** before moving.

| Bytes | Symbol (demangled) | Verdict |
|------:|--------------------|---------|
| 4096 | `commandRegistry` | ✅ move |
| 3792 ×2 | `showInboxMenu()::msgs`, `showPeerInboxMenu()::msgs` | ✅ move |
| 3408 | `gOledConsole` | ✅ move (render-time only) |
| 3072 | `port_IntStack` (data) | ❌ FreeRTOS interrupt stack — keep |
| 2584 | `sWebFsBridge` | ✅ move |
| 2560 | `gAllocTracker` | ✅ move (debug only) |
| 2112 | `gEspNowRxRing` | ⚠️ verify RX-callback context (likely OK, not DMA) |
| 2092 | `gOledEspNowState` | ✅ move |
| 2072 / 1400 / 796 | `sensor_default_regs` (data) | ↪️ make `const` → flash/rodata, not PSRAM |
| 2048 | `renderGameOfLifeAnimation()::grid` | ✅ move |
| 1636 | `gSettings` | ✅ move (hot-read but not DMA) |
| 1536 | `gAutoCache` | ✅ move |
| 1280 | `gV4Dedup` | ✅ move |
| 1216 | `gBroadcastTrackers` | ✅ move |
| 1152 | `gFrameRing` | ⚠️ verify not DMA frame path |
| 1024 | `meshesCmd_listjson()::meshesJsonBuf` | ✅ move |
| 1024 | `r1Crc32EnsureTable()::table` | ↪️ make `const` LUT → flash |
| 1024 | `_aes_lut` (data) | ↪️ `const` → flash (if app-owned) |
| 960 | `TxRxCxt` (data) | ❌ likely WiFi/ESP-NOW DMA ctx — keep |
| 948 ×~12 | OLED scroll structs (`sMainScroll`, `sSensorScroll`, `sWifiMenuScroll`, …) | ✅ **cluster ~11 KB — move together** |
| 936 ×2 | `showInboxMenu()::rows`, `showPeerInboxMenu()::rows` | ✅ move |
| 896 | `gPeerIdentities` | ✅ move |
| 712 | `phy_param` (data) | ❌ WiFi PHY — keep |

**Realistic safe app diversion: ~35–50 KB internal DRAM** without touching DMA/RF/FreeRTOS structures. The OLED scroll-struct cluster and the inbox/menu buffers are the easiest large wins.

---

## 6. Settings already correct — do NOT change

| Setting | Value | Why it's right |
|---------|-------|----------------|
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` | `y` | TLS session buffers (~20 KB each) in PSRAM, not internal |
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | `y` | WiFi/lwIP dynamic buffers prefer PSRAM |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | `y` | enables the `EXT_RAM_BSS_ATTR` diversions (439 KB already out) |
| `CONFIG_ESP_WIFI_IRAM_OPT` / `RX_IRAM_OPT` | `n` | keeps WiFi code in flash — **mandatory** while IRAM is 99.99% full |
| `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST` | `y` (restored) | Bluedroid host → PSRAM |
| `CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY` | `y` | BLE host env off BSS onto PSRAM heap |

---

## 7. Levers, ranked

| # | Lever | Internal DRAM recovered | Risk | Status |
|---|-------|------------------------|------|--------|
| 1 | Move transient render/compute **task stacks** to PSRAM (`…WithCaps`) | **tens of KB, at load spikes** | Med (cache-disable/ISR rules) | TODO |
| 2 | Move safe app **static buffers** to `EXT_RAM_BSS_ATTR` (§5) | ~35–50 KB | Low (per-symbol verify) | TODO |
| 3 | Convert const tables (`sensor_default_regs`, CRC/AES LUTs) to `const` → flash | ~5–8 KB | Low | TODO |
| 4 | **Right-size** resident stacks via high-water marks | ~5–15 KB | Low | TODO |
| 5 | `BT_ALLOCATION_FROM_SPIRAM_FIRST` (host → PSRAM) | visible when BT runs | None | ✅ done |
| 6 | Reduce `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` 16→10 (DMA, internal) | ~10 KB | Med (throughput) | Candidate |

Current other relevant config: `STATIC_RX_BUFFER_NUM=16`, `DYNAMIC_RX_BUFFER_NUM=32`, `CACHE_TX_BUFFER_NUM=32`, `RX_BA_WIN=16`, AMPDU TX+RX on; `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, `SPIRAM_MALLOC_RESERVE_INTERNAL=32768`; `ESP_MAIN_TASK_STACK_SIZE=8192`.

---

## 8. Open questions / verify next

1. Capture an on-device `memreport` with **BT running** and a G2 image viewer open — that's the real worst case; confirm the §7 levers move the needle there, not just at idle.
2. Confirm `gEspNowRxRing` / `gFrameRing` are not accessed from DMA or pre-PSRAM-init paths before moving.
3. Decide on `STATIC_RX_BUFFER_NUM` 16→10 with a throughput test (web UI + camera stream).
4. **LLM flag:** currently `ENABLE_ONDEVICE_LLM 0` in the working tree (test artifact). It costs ~0 internal DRAM — decide whether to re-enable (it does cost ~125 KB flash, and the app partition is at 93% with it on).

---

## 9. Revision history

| Date | Notes |
|------|-------|
| 2026-06-07 | Initial findings: BT PSRAM-first regression fixed; LLM ruled out; task stacks identified as primary internal consumer; candidate buffer list; lever ranking. Measured on XIAO S3 build, `idf.py size` + ELF nm region analysis. |
