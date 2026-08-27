# HardwareOne — Memory Layout & Offload Reference

**What lives in Flash vs PSRAM vs internal SRAM, why, and the knobs that move it.**

Last measured: 2026-06-07, app v0.95, ESP-IDF v5.3.1, BT+G2 enabled.
(Codebase is now app **v0.95.5** / **ESP-IDF v5.5.1** — figures below are pre-upgrade;
the 5.5.1 boot deltas are small: FeatherS3 ~77 KB free DRAM, XIAO Sense ~42 KB.)
Numbers are from `idf.py size` / `xtensa-esp32s3-elf-nm` on the actual build plus
the on-device `BOOT MEMORY REPORT`. Re-measure after big changes.

---

## 0. TL;DR

The ESP32-S3 has **three** memory pools. Two are huge, one is the bottleneck:

| Pool | Size | Pressure | Notes |
|------|------|----------|-------|
| **Flash** (code/rodata/littlefs) | 8 MB (XIAO) / 16 MB (Feather) | Tight on XIAO with BT+G2 | App binary ~4.7–5.1 MB |
| **PSRAM** (external RAM heap) | 8 MB | ~2 % used — *empty* | Spill everything here that can tolerate it |
| **Internal SRAM** (DRAM heap + IRAM) | ~512 KB total, ~240–280 KB heap | **THE bottleneck** | DMA, ISRs, FreeRTOS objects, task stacks must live here |

**The whole game is: keep internal SRAM free by pushing everything that *can* go to
PSRAM/Flash there, and keeping only what *must* be internal (DMA buffers, FreeRTOS
objects, task stacks, ISR-touched state) in internal SRAM.**

### Per-board internal heap total (the number on the dashboard)

| Build | Internal heap total | Why |
|-------|--------------------|-----|
| XIAO ESP32-S3 (BT on, camera+mic on) | ~241 KB | Camera/mic static buffers + BT static cost |
| **Feather S3** (BT on, camera+mic off) | **~277 KB** | +36 KB from camera/mic being off |
| (historical, BT **off**) | ~300+ KB | BT not compiled in → no ~60 KB BT static cost |

**Enabling Bluetooth costs ~60 KB of internal SRAM permanently** (static BSS/data +
the controller's DMA pools). That is inherent — no config moves it while BT is
compiled in. See §4.

---

## 1. Flash (8/16 MB)

Holds: bootloader, partition table, **app binary** (code `.text` + read-only
`.rodata`, including all embedded web HTML/JS/CSS as C string literals), and the
**LittleFS** filesystem (user files, settings JSON, logs, saved recordings).

Partition tables are picked by `CMakeLists.txt` from `HW_BOARD` + `ENABLE_ESP_SR`:

| Board | CSV | factory (app) | littlefs |
|-------|-----|---------------|----------|
| XIAO (8 MB) | `partitions_no_sr_8mb.csv` | **5332 KB** (grown from 4948) | 2796 KB |
| Feather (16 MB) | `partitions_no_sr_16mb.csv` | 4948 KB | 11628 KB |

> The XIAO factory partition was **grown +384 KB** (littlefs shrunk to match)
> because the BT+G2 binary (~5.07 MB) overflows the stock 4948 KB partition.
> Changing the littlefs offset reformats it — on-device files are wiped on the
> first flash after that change. The Feather's 16 MB layout has room to spare, so
> no grow was needed there.

The XIAO binary is ~350 KB larger than the Feather binary because camera + mic +
`esp32-camera` driver compile in on the XIAO Sense and are stubbed out on the Feather.

---

## 2. PSRAM (8 MB external RAM) — where the big stuff already lives

PSRAM is integrated into the heap allocator (`CONFIG_SPIRAM_USE_MALLOC=y`), so
`malloc`/`new` can land here, and code can force it. **~570 KB is already offloaded:**

### 2a. Static globals moved to PSRAM — `EXT_RAM_BSS_ATTR` (~419 KB)

90 declarations across the codebase tag large globals with `EXT_RAM_BSS_ATTR`, which
places them in `.ext_ram.bss` (PSRAM) instead of internal `.bss`. Total ~**419 KB**.
This is the bulk of the offload and was done in prior optimization passes.

```cpp
EXT_RAM_BSS_ATTR static uint8_t gBigBuffer[32768];   // lives in PSRAM, zero-init only
```

Rules: zero-initialized only (no initializer), **not** for DMA targets, **not** for
anything touched before PSRAM init, avoid for hot per-loop/ISR state (PSRAM is slower).

### 2b. Runtime heap allocations forced to PSRAM — `ps_alloc` / `heap_caps_malloc` (~150 KB)

Big runtime buffers use the project's `ps_alloc()` helper or
`heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`. From the boot report:

| Allocation | Size | |
|------------|------|--|
| `json.resp.buf` | 64 KB | web JSON response scratch |
| `debug.pool` | 54 KB | async debug message pool (192 slots) |
| `gWebMirror.buf` | 8 KB | web console mirror |
| `users.json.buf` | 8 KB | user DB |
| `cmd.exec.req` | 6 KB | command exec request ring |
| `espnow.sessions` | 3.3 KB | ESP-NOW SIGMA-I sessions |
| …and ~15 smaller | | all PSRAM |

### 2c. Bluetooth host + WiFi/LWIP dynamic buffers → PSRAM

- `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y` + `CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y`
  → Bluedroid host env, HCI queues, GATT cache allocate from PSRAM first. **This is
  what keeps BT scan/connect from exhausting internal DRAM** (see §4).
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` → WiFi/LWIP *dynamic* buffers go to PSRAM.
  (WiFi *static* buffers still must be internal — see §3 / §5.)

---

## 3. Internal SRAM — the bottleneck (~240–280 KB heap)

What is here and **why it cannot move**:

| Consumer | Approx | Movable? |
|----------|--------|----------|
| Static `.bss`/`.data` (internal) | ~88 KB (was ~109 KB — 21 KB moved to PSRAM 2026-06-07) | Mostly no — small fragments; biggest single is 4 KB (`commandRegistry`). Cold UI clusters (menu scroll states, inbox buffers, GoL grid) **already moved** to PSRAM via `EXT_RAM_BSS_ATTR`. Remaining is hot/RX-context (`gSettings`, `commandRegistry`, ESP-NOW rings) — leave internal. |
| WiFi static RX/TX buffers | **~10 KB** (runtime: 4 static RX ×1600 + 2 TX FG) | Already minimal — NOT a useful lever. (kconfig says `=16` but the runtime allocates 4; the boot log `wifi:Init static rx buffer num: 4` is authoritative.) |
| Task stacks (see below) | ~90–130 KB | Only by trimming sizes; FreeRTOS stacks must be internal. |
| **BT controller** (when BT on): activities (`BLE_MAX_ACT=6`), ACL connections (`ACL_CONNECTIONS=4`), GATT-C cache (40) | part of the ~60 KB + per-connection runtime | DMA/ISR — can't move to PSRAM, but **counts are tunable** (§5). G2 uses 1 scan + 2 temple connections. |
| Arduino BLE C++ object graph (per connection) | tens of KB during GATT discovery | Small `new`/`malloc` objects + FreeRTOS semaphores → forced internal (semaphores **must** be internal; small allocs land internal under `MALLOC_ALWAYSINTERNAL`). Hard to move without patching the vendored lib. |
| WiFi driver | ~32 KB | No. |
| FreeRTOS kernel + ISR stack (`port_IntStack` 3 KB) | ~8 KB | No. |

### Task stacks (`components/hardwareone/System_TaskUtils.h`)

Always-resident heavy ones:

| Task | Stack | Observed HWM | Slack |
|------|-------|--------------|-------|
| `cmd_exec_task` | 32 KB (`CMD_EXEC_STACK_WORDS=8192`) | ~19 KB (57 %) | ~14 KB — **but comment warns**: automation `findCommand` validation needs headroom |
| `debug_out` | 14 KB (`DEBUG_OUT_STACK_WORDS=3584`) | ~9 KB (62 %) | ~5 KB — already trimmed from 16 KB |
| `sensor_queue` | 16 KB | — | bumped from 12 KB; don't shrink |
| `espnow_hb` | 26 KB | — | bumped after clerk consolidation; don't shrink |
| `httpd` (IDF) | large | ~26 KB HWM | near limit |
| WiFi driver | ~32 KB | | fixed |

Per-sensor task stacks (thermal/imu/tof/fmradio/input/apds) are created **on demand**
only when that sensor runs, so they're not all resident.

---

## 4. The Bluetooth cost (read this before "why is RAM lower")

- BT **OFF**: internal heap total ~300+ KB.
- BT **ON**: ~60 KB less, *before BT even initializes*. This is static `.bss`/`.data`
  for `libbt.a` (~20 KB) + `libbtdm_app.a` (~14 KB DRAM + ~15 KB DIRAM) + controller
  pools. It is carved out at link time. **No config recovers it while BT is compiled in.**
  Also: **IRAM goes to ~100 % full** with BT on.
- The part that *is* controllable is BT's **runtime** allocation during scan/connect/GATT
  discovery. Without `BT_ALLOCATION_FROM_SPIRAM_FIRST=y` those land in internal DRAM and,
  on top of the already-reduced pool, exhaust it → `xSemaphoreCreateBinary` returns NULL
  during GATT discovery → `assert(pxQueue)` crash. **With it on, they go to PSRAM.**

> ⚠️ **Stale-config footgun (this bit us):** `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y`
> is **pinned in `sdkconfig.defaults`**, but it was silently dropped from the *generated*
> `sdkconfig` by commit `9a23158` (regenerated against the wrong PSRAM target). The XIAO
> binary that crashed on BT scan was built from that stale config (`=n` → BT in internal
> DRAM → exhaustion → crash). The Feather build came from a clean `fullclean && set-target`
> regen, which restored `=y` from defaults — which is why it survives BT scan. **Lesson:
> after enabling BT, do a clean regen and verify `grep BT_ALLOCATION_FROM_SPIRAM_FIRST
> sdkconfig` shows `=y` (not `# ... is not set`).** A clean XIAO rebuild now also gets it.

To get the full ~300 KB internal heap back, the only option is **BT off** (no G2).

---

## 5. Levers to reclaim internal SRAM (ranked)

| # | Lever | Frees | Risk / tradeoff |
|---|-------|-------|-----------------|
| 1 | **BT → PSRAM** (`BT_ALLOCATION_FROM_SPIRAM_FIRST=y`) | prevents BT-scan crash; keeps Bluedroid host off internal | none — **already applied** (pinned in defaults). HW-confirmed stable. |
| 2 | Camera + mic off (non-Sense boards) | ~36 KB | none on Feather (no such HW) — already off |
| 3 | **BT controller counts**: `BT_CTRL_BLE_MAX_ACT` 6→4, `BT_ACL_CONNECTIONS` 4→3 | a few KB of internal/DMA during BT (est. — verify with live `memreport`) | BT-specific (no WiFi impact). G2 needs 1 scan + 2 connections, so 4/3 keeps headroom. Low risk if not set below G2's needs. |
| 4 | `cmd_exec` stack 32→26 KB | ~6 KB | overflow risk in deep automation command validation — comment explicitly warns. Needs worst-case HWM testing first. |
| 5 | `debug_out` stack 14→12 KB | ~2 KB | low value |
| 6 | Move more static globals to `EXT_RAM_BSS_ATTR` | ~10–15 KB | tedious (many 1–4 KB symbols), must verify none are DMA/ISR/pre-PSRAM |
| — | ~~WiFi static buffers~~ | ~~—~~ | **Not a lever**: runtime already allocates only 4 static RX (~10 KB). |

**Recommended order:** 1 + 2 are done. To target the BT-active floor, get a live
`memreport` *while glasses are connected* first (it shows internal vs PSRAM per
allocation + task-stack HWM), then pick from 3–6 against real numbers rather than
estimates. WiFi is ruled out.

---

## 6. Config knobs reference (current values)

```
# PSRAM heap
CONFIG_SPIRAM_USE_MALLOC=y                       # PSRAM in the heap allocator
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768      # keep 32 KB internal for DMA
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384        # allocs <16 KB stay internal by default
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y           # WiFi/LWIP dynamic buffers → PSRAM

# Bluetooth memory
CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y         # BT host allocations → PSRAM (explicit)
CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y               # BLE host env on heap (so it can be PSRAM)

# WiFi static buffers — kconfig says 16 but RUNTIME allocates 4 (boot log:
# "wifi:Init static rx buffer num: 4"). Already ~10 KB. NOT a useful lever.
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16          # runtime=4
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32         # dynamic → can use PSRAM

# BT controller counts (INTERNAL/DMA, BT-specific) — lever #3
CONFIG_BT_CTRL_BLE_MAX_ACT=6                      # G2 needs scan+2 conns; 4 is enough
CONFIG_BT_ACL_CONNECTIONS=4                       # G2 uses 2 (L+R temple); 3 keeps headroom
CONFIG_BT_GATTC_MAX_CACHE_CHAR=40                # GATT-C characteristic cache
```

Mechanisms in code:
- **`EXT_RAM_BSS_ATTR`** on a global → PSRAM `.bss` (zero-init, non-DMA, non-hot).
- **`ps_alloc(n)` / `heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`** → runtime PSRAM alloc.
- Plain `malloc`/`new` → internal first (per `ALWAYSINTERNAL`), spills to PSRAM.
- **`heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`** → forced internal (DMA).

---

## 7. How to re-measure

- On device: run `memreport` (the `BOOT MEMORY REPORT`). Since 2026-08-23 its
  `[2] LIVE HEAP ATTRIBUTION` and `TOTALS` sections are measured, not estimated:
  every task's stack and TCB block is read off the allocator (kernel tasks
  included), allocator metadata is derived from `heap_caps_get_info`, and the
  `UNATTRIBUTED` residual is a signed difference that is deliberately not padded
  to zero. Figures quoted in this file from earlier reports understate task cost
  by ~21 KB (the seven IDF system-task stacks were printed but never summed) —
  re-measure before acting on any number here. See
  `docs/MEMORY_REPORT_FIX_PLAN.md` and `docs/DRAM_UNACCOUNTED_CENSUS_2026-08-23.md`.
- Static breakdown: `idf.py size`, `idf.py size-components`, `idf.py size-files`.
- Biggest internal-DRAM static symbols:
  ```
  xtensa-esp32s3-elf-nm -S --size-sort --radix=d build/hardwareone-idf.elf | \
    awk 'tolower($3)~/^[bd]$/ && $1>=1069547520 && $1<1071644672 {print $2"\t"$4}' | sort -rn | head
  ```
- PSRAM `.ext_ram.bss` total: same, with range `1006632960..1040187392`.

See also `docs/HEAP_OPTIMIZATION_FINDINGS.md` for the optimization history.

---

## 8. Changelog

### 2026-06-07 — BT+G2 enable + internal-DRAM reclaim
- **BT→PSRAM confirmed working on HW** (Feather): glasses connect/discover/operate
  with no crash. `BT_ALLOCATION_FROM_SPIRAM_FIRST=y` (pinned in `sdkconfig.defaults`)
  is the fix; the XIAO crashed only because its generated config had lost it.
- **Stack trims (reduce *used*):** `g2_page_swap_w` 16→14 KB (HWM ~6 KB),
  `DEBUG_OUT_STACK_WORDS` 14→12 KB (HWM ~9.3 KB; 12 KB is the documented floor). ≈4 KB.
- **Ceiling raise (reduce internal static):** moved cold-UI `.bss` to PSRAM via
  `EXT_RAM_BSS_ATTR` — 13 `OLEDScrollState` menu scroll states (~12 KB), the two
  G2 ESP-NOW inbox buffers (`msgs`+`rows`, ~9 KB), and the Game-of-Life grid (2 KB).
  **Measured: internal static 111,812 → 89,880 B (−21,932 B). Ceiling ~277 → ~298 KB.**
- Net: BT-active free floor expected ~24 KB → ~45–49 KB (verify on HW with
  `memreport` while glasses are connected).
- Deliberately left internal (hot / RX-context / pre-PSRAM): `commandRegistry`,
  `gSettings`, ESP-NOW RX rings + dedup, `gAllocTracker`.

### 2026-06-07 (Tier 1) — further internal reclaim
- **More cold `.bss` → PSRAM** (`EXT_RAM_BSS_ATTR`): `sWifiScanLabels`, `gSensorsRows`,
  `gAnimIconPickPaths`, `meshesJsonBuf`, `sWebFsBridge`. Internal static 89,880 → 84,224 B
  (−5.5 KB). **Cumulative internal static: 111,812 → 84,224 B (−27.6 KB); ceiling ~277 → ~305 KB.**
- **BT controller counts** (`sdkconfig.defaults`, frees internal/DMA at runtime):
  `BT_CTRL_BLE_MAX_ACT` 6→4, `BT_ACL_CONNECTIONS` 4→3, `BT_CTRL_SCAN_DUPL_CACHE_SIZE` 100→30.
  Sized for G2 (2 temples) + R1 ring (1) + 1 scan/adv = 4 acts / 3 conns. ⚠️ Raise these if
  you run MORE than 3 simultaneous BLE links.
- After Tier 1, remaining levers are tradeoffs (Tier 2/3 in §5), not free wins.

### 2026-06-07 — disable unused I2C sensors
- `CUSTOM_ENABLE_{GPS,FM_RADIO,RTC,PRESENCE}` → 0 (kept Gamepad + OLED). **Flash −136 KB**
  (drivers + pulled-in libs + web/OLED/G2 page code). Internal static only −1 KB (their
  static state was tiny); main RAM benefit is latent (those polling tasks can no longer spawn).
- Fixed a latent transitive-include bug this exposed: `G2_Page_Sensors.cpp` now `#include`s
  `System_Mutex.h` directly (it used `SensorCacheGuard`, previously pulled in via a sensor header).

---

## 9. Build Size Log

Running log of **measured** build points. Append a row whenever a config materially changes
(measurement commands in §7). All rows: ESP-IDF v5.3.1, app v0.95, BT+G2 ON, `no_sr`, unless noted.
(Pre-upgrade rows; codebase is now ESP-IDF v5.5.1 / app v0.95.5 — re-measure before relying on exact bytes.)

`(m)` = measured on device/binary · `(c)` = calculated · `(e)` = estimated, verify on HW.

| # | Board | Config (delta from prev) | Flash bin (B) | Int. static `.bss` (B) | Ceiling (KB) | BT-active free floor |
|---|-------|--------------------------|--------------:|-----------------------:|-------------:|----------------------|
| A | XIAO S3 (8MB) | cam+mic ON, all sensors, `SPIRAM_FIRST` **stale=n** | 5,194,048 (m) | — | 241 (m) | **crashed** (BT in internal DRAM); bin overflowed 4948K partition |
| B | Feather S3 (16MB) | cam+mic OFF; `SPIRAM_FIRST=y` | 4,717,344 (m) | 111,812 (m) | 277 (m) | ~24 KB (m) |
| C | Feather S3 | + cold-UI `.bss`→PSRAM (scroll/inbox/grid) | 4,717,344 (m) | 89,880 (m) | ~298 (c) | ~45 (e) |
| D | Feather S3 | + Tier 1 (more `.bss` + BT counts 4/3/30 + stack trims) | 4,717,328 (m) | 84,224 (m) | ~304 (c) | ~55–60 (e) |
| E | Feather S3 | + disable GPS/FM/RTC/presence | 4,581,184 (m) | 83,192 (m) | **306 (m)** | **G2: ~56 KB · G2+R1: ~46 KB (m)** |
| F | Feather S3 | + BT counts headroom (ACL 3→4, MAX_ACT 4→5) | 4,581,200 (m) | 83,192 (static unchanged) | 306 (c) | re-measure w/ G2+R1 (slightly < 46 KB; counts are runtime) |

> **Ceiling formula:** internal `.bss` carve-out sets where the heap starts, so
> `ceiling ≈ 277 KB + (111,812 − current_static)/1024`. The computed ~305 for row E was
> confirmed on HW at **306 KB** (boot report 2026-06-07: total 313,667 B; internal BSS down to
> 63 KB from 90 KB; largest free block up to 76 KB, fragmentation much improved).
>
> **BT-active floors (measured, row E):** boot 172 KB free → G2 connected ~56 KB → G2 **+ R1
> ring** ~46 KB. So each extra BLE connection costs ~**10 KB** internal (Arduino BLE object
> graph + controller per-connection). The original pre-optimization G2-only floor was ~24 KB,
> so the net gain is ~+32 KB at the floor.
>
> **G2 (2 temples) + R1 ring = 3 simultaneous BLE connections.** HW-confirmed working at
> ACL=3, but at zero headroom — so **raised to `ACL_CONNECTIONS=4` / `BLE_MAX_ACT=5`
> (2026-06-07, row F)** = G2(2)+R1(1)+1 spare connection + 1 scan/adv, for the planned
> ESP-brokered G2↔R1 pairing handshake (and transient reconnects / a future phone link).
>
> **Note on R1 link drops:** a ring disconnect with HCI `rsn=0x8` is a BLE **supervision
> timeout** (link went silent), not an ESP fault or a connection-limit issue — the R1 (battery
> wearable, incompletely paired by a 3rd-party host) sleeps/lapses the link. Mitigate with
> `bleautoreconnect r1-ring on`; real fix is implementing the R1 keep-alive/pairing flow.

### Per-feature deltas (measured where noted)

| Feature toggled | Flash Δ | Internal Δ | Source |
|---|---|---|---|
| Disable GPS+FM+RTC+presence (I2C sensors) | **−136 KB** (m) | −1 KB static (m) | E vs D — drivers + pulled-in libs (Adafruit GPS, Radio) + web/OLED/G2 pages |
| Camera + mic subsystem (XIAO Sense) | ~+465 KB (m) | ~+36 KB ceiling (m) | A vs B — also crosses board, so flash Δ is approximate |
| BT + G2 (vs BT off) | ~+400 KB (m) | ~+60 KB static/DMA (e) | `libbt` 316K + `libbtdm_app` 83K (size-components); internal from the BT-disable commit + `nm` |
| Cold-UI `.bss` → PSRAM (`EXT_RAM_BSS_ATTR`) | 0 | **−27.6 KB static** (m) | B→D; raises ceiling 1:1, costs nothing in flash |
| BT controller counts (MAX_ACT 6→4, ACL 4→3) | ~0 | ~5–8 KB runtime (e) | frees controller internal/DMA only while BT is live |

These deltas are roughly **additive**, so you can estimate an untested combo by starting from a
logged row and applying the relevant deltas — but a new combo isn't *confirmed* until it has its
own measured row here.
