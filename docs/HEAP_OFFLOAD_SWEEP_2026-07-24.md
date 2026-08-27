# Heap / DRAM → PSRAM & DRAM → Flash offload sweep — 2026-07-24

> **STATUS — Tiers 1 + 2 IMPLEMENTED (built green feathers3/esp32s3, UNCOMMITTED, awaiting HW).**
> Measured linker-map delta vs the pre-change build: `.dram0.data` 28,424 → 26,248 B
> (−2,176, all → flash) and `.dram0.bss` 84,800 → 54,592 B (−30,208, → PSRAM + 1 KB flash)
> = **−32,384 B = 31.6 KB internal DRAM freed**, in four build-verified batches:
>
> | Batch | What | Freed |
> |---|---|--:|
> | Tier 1 | const httpd_uri_t (122) + tables + `r1Crc32`→constexpr + `commandRegistry`→PSRAM | 7.1 KB |
> | 2a | 24 OLED render/animation/list caches → `EXT_RAM_BSS_ATTR` | 9.3 KB |
> | 2b | 54 G2 response buffers + rows/list caches → `EXT_RAM_BSS_ATTR` | 11.6 KB |
> | 2c | 31 System command-scratch buffers → `EXT_RAM_BSS_ATTR` | 4.1 KB |
>
> Every batch was triple-checked (uninitialized-POD, no secret, no DMA/ISR/spinlock,
> single-owner unchanged) and nm-verified in the ELF. **Excluded/held by verification:**
> `gScanCache` (non-POD — `String` member), `gFrameRing`+`gSidStats` (BLE-notify/protocol path,
> held for explicit call), `sConfirm` (stores originating command text), `systemLogSettingEntries`
> (String `c_str()` blocks const), and 3 initialized statics (`sPeerPath`/`sPickerReq`/
> `fileBrowserRenderData`). Tier 3 (driver objects, ~20 KB conditional) remains list-only.
> Board caveat: only proves feathers3/esp32s3.


> **List-only audit. No source changed.** A fresh full-tree pass (including all
> uncommitted WIP) for internal-DRAM buffers that can move to PSRAM or to flash.
> Method: authoritative symbol inventory from **today's** linker map
> (`build/hardwareone-idf.map`, 07:50) + a 9-slice fan-out (one finder per
> subsystem) each adversarially verified by a skeptic pass (18 agents, 0 errors).

## 0. TL;DR

Internal DRAM (~240–280 KB) is the bottleneck; PSRAM (8 MB) is ~2 % used; flash is
plentiful. The map says the app owns **5.6 KB `.dram0.data`** (initialized → the
const→flash lever) and **70.9 KB `.dram0.bss`** (zero-init → the PSRAM lever). Most
of the big `.bss` items are correctly excluded (secrets, critical-section, hot-loop,
already-PSRAM); the recoverable set is a **long tail of small render/scratch buffers**
plus a few clean large ones.

| Lever | Mechanism | Verified-movable | Risk | Steady vs conditional |
|---|---|--:|---|---|
| **const → flash** | mark logically-const tables `const`/`constexpr` → `.rodata` | **~3.1 KB** (+0.84 KB harder) | none | steady |
| **.bss → PSRAM** | `EXT_RAM_BSS_ATTR` / `PSRAM_STATIC_BUF` on POD statics | **~30–33 KB** | low, per-item | steady |
| **heap → PSRAM** | placement-new into `ps_alloc` (driver objects) | **~20 KB** | medium (teardown rewrite) | conditional (feature-resident) |

**The single most valuable line-changes:** `commandRegistry` (4 KB, one attribute),
the `const httpd_uri_t` sweep (~1.9 KB flash, mechanical), and `r1Crc32` → constexpr
(1 KB flash). Everything else is bulk mechanical using tooling that **already exists**
in the tree (`EXT_RAM_BSS_ATTR`, `PSRAM_STATIC_BUF` at `System_MemUtil.h:368`).

**How this differs from the prior "coverage-complete ≥512 B" audits**
(`docs/LAZY_ALLOCATION_AUDIT.md`, `docs/STACK_TO_PSRAM_CANDIDATES.md`): this run targets
(a) the **const→flash lever** those passes flagged but never implemented, and (b) the
**sub-512 B scratch/response-buffer family** they didn't enumerate individually. Rules
carried forward and re-confirmed: **no task stacks in PSRAM**, **no secrets in PSRAM**
(flash encryption is OFF), **no PSRAM for critical-section / ISR / DMA / hot-loop data**.

---

## 1. Lever 1 — const → flash (zero cost, zero risk)

`.dram0.data` holds initialized globals that occupy internal DRAM. A **logically-const**
table (never written after init, all-link-time-constant initializers) relocates to
`.rodata` (flash) when marked `const` — `CONFIG_SPIRAM_RODATA` is OFF so rodata = flash,
not PSRAM. Pure DRAM saving, no runtime cost.

| Bytes | Symbol | File:line | Fix | Note |
|--:|---|---|---|---|
| 1024 | `r1Crc32EnsureTable()::table` | `System_R1_Protocol.cpp:66` | precompute → `static constexpr uint32_t[256]` | in `.bss` today; also deletes the runtime build. Flagged in LAZY audit §181, never done |
| 800 | 50 × `startHttpServer()::…` httpd_uri_t | `WebServer_Server.cpp:5286‑5341` | prefix each `static httpd_uri_t` with `const` | `httpd_register_uri_handler` takes `const httpd_uri_t*` and deep-copies (verified `httpd_uri.c`) |
| 140 | `kAllow` guest-API allowlist | `WebServer_Utils.cpp:356` | `static const char* const kAllow[]` | inner const missing → pointer array is writable → `.data` |
| 128 | 8 × migration httpd_uri_t | `WebServer_MigrationTool.cpp:769‑972` | `const httpd_uri_t` | same pattern |
| ~280 | 11 menu/preset tables (`gMapMainMenuItems`, `g*Submenu`, `ledEffects`, `ntpPresets`, `logLevelNames`, `kVerbs`, `kBlockedSuffixes`) | `OLED_Mode_Map.cpp`, `System_SetupWizard.cpp`, `Bluetooth.cpp:658`, `System_Filesystem.cpp:1422` | `static const char* const NAME[]` | element-writes never happen (assignment is to a `menuItems=` cursor, not the array) |

**Subtotal ~2.37 KB clean.** Plus the sibling `httpd_uri_t` tables in `WebPage_*.cpp`
(Sensors 176 B, Bond 224 B, LLM 192 B, ESPNow 96 B, Maps 80 B, Bluetooth/Battery 32 B)
— a **project-wide `const httpd_uri_t` sweep ≈ 1.9 KB flash total**.

**Harder / not-clean (excluded from the clean subtotal):**
- `gCommandModules` (840 B, `System_Utils.cpp:2647`) — already `static const` but stuck in
  `.dram0.data` because element initializers use non-constant-expression counts
  (extern/`.c_str()` derived) → dynamic init. Fixable only by making every module's count a
  `constexpr`. Sibling `featureRegistry` / SettingsModule tables const-init cleanly and are
  *already* in flash, which proves the pattern — `gCommandModules` is the outlier.
- ⚠️ `systemLogSettingEntries` (280 B, `System_Debug.cpp:2396`) — **DOWNGRADED by the skeptic.**
  `entry[3].options = gSystemLogFlagsBitmaskOptions.c_str()` is a runtime `String` buffer →
  non-constant initializer → dynamic init → `.data`. const→flash will **not** relocate it
  without making the bitmask-options a compile-time constant, which conflicts with its
  live-regenerated design. **Not a clean win — do not attempt as first described.**

---

## 2. Lever 2 — .bss → PSRAM (`EXT_RAM_BSS_ATTR` / `PSRAM_STATIC_BUF`)

POD file/function-static buffers that are single-task, non-secret, and never touched in a
critical section / ISR / DMA / flash-cache-off window. **Tooling already exists:**
`EXT_RAM_BSS_ATTR` (static PSRAM `.bss`, contiguity-safe, simplest) or `PSRAM_STATIC_BUF(name, size)`
(`System_MemUtil.h:368`, lazy `ps_alloc` — saves PSRAM until first use). Since PSRAM is ~empty,
`EXT_RAM_BSS_ATTR` is the recommended default (no heap, no fragmentation — consistent with the
`HOT_PATH_HEAP_AUDIT` "prefer static over lazy for never-freed" conclusion).

### 2a. Clean large items
| Bytes | Symbol | File:line | Note |
|--:|---|---|---|
| 4096 | `commandRegistry` | `System_Command.cpp:27` | **top win.** POD `const CommandEntry*[MAX_COMMANDS]`, written once at boot then read-only on every (non-hot) dispatch. Sibling `registeredModules` is *already* `EXT_RAM_BSS_ATTR` — one-line change |
| 1680 | `sBannerQueue` | `OLED_UI.cpp:521` | OLED main-loop task; cross-task push is lock-free (benign pre-existing race, PSRAM-safe: no portMUX/ISR) |
| 1280 | `sFbViewLineOff` | `OLED_Mode_FileBrowser.cpp:164` | file-browser line offsets |
| 1152 | `gFrameRing` | `G2_Glasses.cpp:326` | POD `G2FrameEvent[32]`. `head[12]` is **display-protocol** framing, not secure-channel secret. ⚠️ has a pre-existing lock-free two-writer race (RX task + TX worker) — section change doesn't affect it, but worth fixing separately |
| 1088 | `gAutos` | `G2_Page_Automations.cpp:49` | G2 list cache |
| 704 | `sNotifView` | `OLED_Utils.cpp:203` | |
| 640×2 | `gUsers` | `OLED_Mode_UserManager.cpp:34`, `G2_Page_Users.cpp:48` | user-list caches — **verified they hold no password/hash fields** (display metadata only) |
| 576 | `gSidStats` | `System_G2_Protocol.cpp:1261` | `G2SidStat[16]` |
| 540 | `sKindLastShownMs` | `System_Notifications.cpp:769` | |
| 528 | `sWifiScanSSIDs` | `OLED_Mode_Network.cpp:353` | SSIDs are not secrets |
| 512 | `waveform`, `gTrackFiles`, `gSysEvLastReadout` | anim / map / G2 | |
| 480 | `stars` (starfield) | `OLED_Mode_Animations.cpp:128` | animation renderers are plain fn-ptrs, no ISR |
| 452 | `fileBrowserRenderData` | `OLED_Mode_FileBrowser.cpp:205` | |
| 448 | `connectedDevices[16]` | `System_I2C.cpp:208` | I2C device registry (not flash-path) |
| 384 | `sTurns` | `System_LLMChat.cpp:43` | |

### 2b. The scratch/response-buffer families (bulk mechanical)
Two large families of near-identical `static char …[96‑256]` buffers, all cold and
human-triggered, `snprintf`'d **after** any LittleFS/flash op returns:

- **G2 handlers (~9.0 KB):** ~30 `cmd_g2*::ret/out/buf/err`, `g2ProbeImage*::ret`,
  `cmd_ring*::buf` in `G2_Glasses.cpp` / `G2_Ring.cpp`, plus `G2_Page_*` row/list caches
  (`showListMenu::rows`, `gUsers`, `gRows`, `gBuckets`, `gFilesActionPath`…). All run
  serialized on the cmd-exec task / gHijack-gated probe FSM; `g2ShowListPage` deep-copies
  rows via `dupPageSwapItems` (`G2_Glasses.cpp:10692`) — the same safety proof as the
  already-shipped Group-A. G2_Glasses already has 34 `EXT_RAM_BSS_ATTR`; this **finishes the
  migration.**
- **System command handlers (~4.2 KB):** ~25 `respBuf/buf/ret/errBuf/result` stragglers in
  `System_User`/`Filesystem`/`I2C`/`Settings`/`Debug`/`Automation`/`FeatureRegistry`/`Maps`.
  The large siblings were already converted (`PSRAM_STATIC_BUF`/`EXT_RAM_BSS_ATTR`); only the
  small ones were left. Recommend `PSRAM_STATIC_BUF` for the returned-`const char*` response
  buffers, `EXT_RAM_BSS_ATTR` for POD file-scope statics (`sConfirm`, `connectedDevices`,
  `gSettingsModules`).
- **OLED animation/menu scratch (~2 KB):** `fire`, `columns`, `sCatLabels`,
  `sNetworkMainActions`, `sBtActions`, map submenu bufs, etc.

**Verified-movable `.bss` total ≈ 37.5 KB** (of which ~3 KB is vendored `MLX90640_API
scratchData` — a driver-internal buffer, lower priority). **App-owned steady relief ≈ 30–33 KB.**

---

## 3. Lever 3 — heap → PSRAM (driver objects, conditional)

Runtime `new` ≤ 16 KB lands in internal DRAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`).
Only resident while the feature is active. **Template already in-tree:** `gPA1010D`
(`i2csensor_pa1010d.cpp:136`) is placement-new'd into `ps_alloc(PreferPSRAM)` with explicit
`~T()+free()` teardown (`freeGpsObject()`, **not** `delete` — ps_alloc memory must be `free`'d).

| Bytes | Object | File:line | Note |
|--:|---|---|---|
| 9400 | `gVL53L4CX` | `i2csensor_vl53l4cx.cpp:338` | **NEW.** ranging math walks the struct each poll (bounded ≤10 Hz) → PSRAM latency acceptable |
| 5024 | `gOledFileManager` | `OLED_Mode_FileBrowser.cpp:625` | ⚠️ **not POD** (has ctor) and **does** have a delete site (`:1442`) — placement-new must add explicit `~FileManager()+free()`. Lifecycle-scoped (resident only while browser open) |
| 4748 | `gMLX90640` | `i2csensor_mlx90640.cpp:527` | already flagged LAZY §100 |
| 1344 | `bondPickerScroll`/`pickerLabels` | `OLED_Mode_Remote.cpp:285` | |

**Ceiling ~20 KB, all conditional.** The 5 `PreferInternal` / `MALLOC_CAP_INTERNAL` sites
examined (`System_Debug:505`, `System_ESPNow:9314` streamQueue, `OLED_Mode_FileBrowser:577`,
`System_Automation:3662`, `OLED_Utils:2421`) are **all deliberate** (DMA / no-PSRAM fallback /
cache-safety) — no missed opportunities there.

---

## 4. Confirmed exclusions (so future sweeps don't re-litigate)

| Symbol | Bytes | Reason |
|---|--:|---|
| `gEventRing` | 7872 | filled inside `taskENTER_CRITICAL(&gEventMux)` — PSRAM write with IRQs off |
| `gAllocTracker` | 2560 | on the allocation hot path w/ documented multi-task races (judgment call — could revisit under "only when a dev enables it") |
| `gOledEspNowState` | 2096 | holds a typed remote password (secret) + String members |
| `gSettings` | 1756 | wifi/mqtt passwords (secret) + hot |
| `gAutoCache[64]` | 1536 | evaluated on the automation hot path |
| `gDisplay` (SSD1306+fb) | 1024 | framebuffer touched every render (hot) |
| `gSc` / `gMeshDerivedKeys` / `gIdentity` | 512/272/272 | crypto keys — **never PSRAM** (flash enc OFF) |
| `bleMessageHistory` | 128 | credential-transit: raw inbound `login <user> <pass>` copied here before parse (`Bluetooth.cpp:708`) |
| `gPeerBuffer`/`gTopoStreams`/`gMeshRetryQueue`/`gBlePeerData` | 360/352/288/416 | **non-POD** (Arduino `String` members) — `EXT_RAM_BSS_ATTR` unusable; even placement-new leaves String backing in DRAM |
| `sWizard` | 336 | setup-wizard result holds wifi/mqtt password (secret) + non-POD |
| `prompt_tokens` / `gPromptDiag` | 2048/272 | LLM hot path |

---

## 5. Recommended sequencing

1. **Zero-risk flash sweep** (~3.1 KB + the WebPage siblings → ~4 KB total): `const httpd_uri_t`
   everywhere, `kAllow` inner-const, `r1Crc32` → constexpr, menu/preset tables `const char* const`.
   Mechanical, no behavior change.
2. **`commandRegistry` → `EXT_RAM_BSS_ATTR`** (4 KB, one line, sibling precedent).
3. **Bulk `.bss` families** (~15 KB): the G2 (~9 KB) + command-scratch (~4.2 KB) + OLED (~2 KB)
   buffers via `EXT_RAM_BSS_ATTR` / `PSRAM_STATIC_BUF`. Highest byte-count, all low-risk, but
   many files — do in reviewable batches.
4. **Remaining clean large `.bss`** (`sBannerQueue`, `gFrameRing`, `sFbViewLineOff`, `gAutos`,
   `gUsers`, animation buffers…) individually.
5. **Driver objects → placement-new** (~20 KB conditional): copy the `gPA1010D` template;
   `gVL53L4CX` and `gMLX90640` first, then `gOledFileManager` (needs the `~FileManager()+free()`
   teardown-site rewrite).
6. `gCommandModules` constexpr-count refactor (840 B) — only if worth the touch.

## 6. Corrections to prior findings
- `systemLogSettingEntries` const→flash (implied by the .data listing) is **not clean** — the
  runtime `String::c_str()` options pointer forces dynamic init. Documented here so it isn't
  re-proposed.
- `gOledFileManager` is **not** "never freed" (LAZY §152 implied a single-construction advantage):
  there is a `delete` at `:1442`; the placement-new refactor must convert it.
- `r1Crc32` constexpr and `commandRegistry` PSRAM were both previously flagged but remain
  **unimplemented** as of this build.
