# Feature cost ledger — what each feature costs in flash and DRAM (2026-08-23)

**Build measured:** `xiao_s3`, full carrier config (NETWORK/WEB/I2C level 4, HTTPS, OLED, gamepad,
BT, G2, R1, TestSuite, automations, UART host link, Pi power/fan, bonded mode, LLM backend + CM5),
fw 0.99.91.1, `-O2`, image **5,929,712 B** of a 5,984,256 B factory partition (54,640 B free).
**Tool:** `tools/map_feature_cost.py build-xiao_s3/hardwareone-idf.map 5929712`.
**Provenance:** every §1/§2 number was adversarially reviewed (4 lenses → 25 findings, all folded
in); §3 is empirical (16 rebuilds). Attribution sums to 5,911,018 B; the remaining **18,694 B is
section-alignment fill** (ld's `*fill*` rows — 17.6 KB in `.flash.text` alone), not error.

> The DRAM report fix (`docs/MEMORY_REPORT_FIX_RESULT.md`) made *live task/TCB* numbers true. It
> does not attribute memory to features and says nothing about flash. This ledger is that missing
> view: link map (flash, static RAM) + measured boot reports (stacks, heap bounds) + rebuild sweep.

---

## 1. Why the image is 5.9 MB — flash by feature

`code` includes `.wifi*iram`/`.coexiram` (placed in flash by IDF linker fragments); non-loaded
`.xt.prop`/`.eh_frame` are excluded; `str~` = the feature's share of the 1.78 MB merged string
pool (pre-merge × 0.932 — the pool folds only ~7%, so this is a solid estimate). Arduino wrapper
objects are charged to the feature that uses them (BLE → Bluetooth app, SD/SPI → SD card, …).

| feature | TOTAL | code | rodata | str~ | note |
|---|---:|---:|---:|---:|---|
| **Web UI** (server + embedded HTML/JS/CSS) | **1,172,227** | 209,011 | 5,781 | **957,430** | 20 % of the image. Page text measured off the objects: 972,287 B raw across 12 page sections (`streamEspNowContent` 243 K, `streamSettingsContent` 199 K, Automations 107 K, Files 72 K, Logging 68 K…). `WebServer_MigrationTool` 28 K of code has no working flag. |
| **G2 glasses** (lens UI + protocol) | **604,090** | 451,632 | 11,060 | 140,817 | `G2_Glasses.cpp` alone is 300 K of code — the largest object in the image. Ring/health files are NOT in this row (they're R1). |
| WiFi / LWIP / PHY (IDF) | 470,227 | 402,936 | 38,950 | 23,204 | incl. 23.4 K of `.wifi*iram` code an earlier draft missed. |
| **ESP-NOW / mesh / bond** (app layer) | **443,580** | 322,713 | 6,435 | 114,367 | `System_ESPNow.cpp` is 234 K of code (14 k-line file). Crypto is separate ↓. |
| Bluetooth stack + controller (Bluedroid) | 443,159 | 359,217 | 24,765 | 58,596 | required by G2 + R1. |
| **Core** (commands, CLI, settings, users, FS, boot) | 442,605 | 293,311 | 23,793 | 125,233 | `System_User` 75 K, `System_Settings` 63 K, `System_Utils` 60 K; ~125 K help text + format strings. |
| ESP-IDF platform (drivers, heap, system, ROM glue) | 237,728 | 174,516 | 15,096 | 44,591 | the true "cost of being an ESP32" after evicting libsodium/SD/i2s/jpeg to their features. |
| OLED display UI | 231,142 | 192,037 | 5,072 | 33,936 | |
| Camera / mic / live audio / capture | 169,228 | 118,624 | 4,299 | 46,209 | incl. `esp_driver_i2s` (PDM mic, pulled only by `HAL_Audio`). |
| **libsodium** (ESP-NOW / bond / capture crypto) | 163,772 | 128,749 | 32,529 | 1,370 | ed25519 84 K, blake2b 30 K, argon2 14 K… **~25.7 K of it (argon2/aegis/salsa) is dragged in by `sodium_init()`'s `*_pick_best_implementation` with no caller** — a real lever. |
| TLS / mbedTLS (HTTPS) | 150,871 | 128,631 | 9,935 | 12,213 | |
| I2C sensors + gamepad + sensor logging | 146,181 | 101,935 | 3,622 | 40,487 | incl. Arduino `Wire`/hal-i2c (shared with OLED). |
| Debug / diagnostics / notifications | 130,452 | 71,413 | 18,807 | 40,145 | ~36 K of this row (`System_Notifications`+`System_Events`) is the user-facing event bus, not diagnostics. |
| Bluetooth (app layer) | 129,570 | 104,534 | 1,928 | 23,044 | now includes the 67 K Arduino BLE wrapper (`BLEClient`/`BLEDevice`/…) it actually consists of. |
| **R1 ring / health** | 122,057 | 101,908 | 2,217 | 17,386 | `G2_Ring.cpp` 66 K (the ring's BLE central) + `G2_Health.cpp` 22 K + `System_R1_Protocol` 20 K + history store. |
| libc / libstdc++ / libgcc | 116,578 | 106,517 | 7,746 | 1,775 | |
| Icons (rodata tables) | 94,608 | 114 | 93,488 | 1,006 | one coarse gate; ~11 K belongs to features that are off. |
| LLM backend / CM5 link / dictation | 93,048 | 70,834 | 2,349 | 19,104 | |
| SD card (FATFS + SDMMC + SDSPI + Arduino SD) | 83,640 | 65,367 | 4,700 | 13,345 | pulled in solely by `System_VFS.cpp` (`SD_CS_PIN` boards). Was invisible in earlier accountings. |
| Automations | 82,701 | 69,241 | 1,032 | 12,424 | one file. |
| WiFi / network / time (app layer) | 80,604 | 59,051 | 1,488 | 20,051 | incl. the 30 K Arduino Network/STA wrapper. |
| liblc3 (G2 mic audio) | 58,798 | 12,702 | 46,096 | 0 | after the 2026-08-22 trim. |
| LittleFS (IDF + Arduino FS glue) | 41,668 | 35,032 | 1,075 | 5,433 | |
| Camera driver (esp32-camera + esp_jpeg) | 39,305 | 28,110 | 3,974 | 4,231 | |
| Arduino core (genuine: String/Print/HWCDC/UART/GPIO) | 37,755 | 30,450 | 826 | 6,366 | the real core — the other ~78 % of libarduino.a is charged to its features above. |
| HTTP server/client (IDF) | 33,629 | 25,531 | 1,168 | 6,930 | |
| FreeRTOS | 29,375 | 20,072 | 1,716 | 4,473 | |
| NVS 15 K · Adafruit GFX/SSD1306 14 K · Power 8 K · FileManager 6 K · OTA 4 K · HAL/other ~11 K | ~58,000 | | | | |

**Roll-up (sums to the image):**

| group | bytes | share |
|---|---:|---:|
| Feature code + strings (web, G2, ESP-NOW, core, OLED, R1, sensors, camera/mic, icons, LLM, automations, debug, SD…) | 4,171,032 | 70 % |
| Protocol stacks those features require (WiFi/LWIP 470 K + Bluedroid 443 K + libsodium 164 K + mbedTLS 151 K + httpd 34 K) | 1,261,658 | 21 % |
| Platform (ESP-IDF base 238 K + libc 117 K + LittleFS 42 K + Arduino core 38 K + FreeRTOS 29 K + NVS 15 K) | 478,328 | 8 % |
| section-alignment fill | 18,694 | 0.3 % |

So "ballooned" is: **1.17 MB web UI (0.97 MB of it page text), 0.60 MB G2 lens, 0.44 MB ESP-NOW,
1.26 MB of stacks the connected features require — and 0.78 MB of `-O2`** (§3 measured). Disabled
features leak only crumbs; every gate was verified against the map (gating doc §1).

### The levers, ranked (empirical where possible)

| lever | bytes | effort | notes |
|---|---:|---|---|
| **`-Os` instead of `-O2`** | **775,584 measured** (§3) | one sdkconfig line | bigger than the earlier 650 K relink estimate. Runtime cost unmeasured — time the mic/LC3/BLE paths before shipping it. |
| **gzip the web page text into build-time assets** | **~535–738 K** | real refactor | Measured on the linked objects: all 12 page sections 972,287 → 234,150 B gz (4.15×, saves 738 K) — that is the *ceiling*, externalizing all HTML+JS+CSS. `<script>`/`<style>` blocks only: **~535 K** (JS 732→185 K, CSS 38→11 K). Feasible: page branches are compile-time `#if`; runtime injection is a few window globals. **Constraints from the earlier websrc/ plan (memory note `project_web_asset_strip_plan` — its Phase 3 costed a narrower 24-asset variant at ~398 K): each asset must be served as ONE gzip member — Chrome and libcurl silently truncate multi-member gzip — so never concatenate per-chunk compressions.** |
| Comment/indent strip (the 158 K plan) | ~50 K *on top of* gzip | designed | measured: stripped-then-gzipped saves a further 54.5 K over gzip alone. Compounds — not superseded. Standalone value 158 K. |
| G2 TestSuite off on the carrier (`ENABLE_G2_TESTSUITE 0`) | 112,288 measured | flag exists | |
| libsodium: stop `sodium_init()` pulling argon2/aegis/salsa nobody calls | ~25,700 | small, vendored-lib patch | `*_pick_best_implementation` reachability, no project caller (grepped). |
| `WebServer_MigrationTool` behind a real flag | 28,282 | small | `ENABLE_MIGRATION_TOOL` ≡ `ENABLE_HTTP_SERVER`, CMake never greps it. |
| Icons for off features 11 K · debug rows 8 K | ~19,000 | fiddly | gating doc §8. |

---

## 2. What each feature costs in DRAM — measured

Sources: the fixed boot report (stacks/TCBs **measured**), the map (static `.bss` **by placement
address** — the WiFi/BT `.bss` name prefix lies: `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`
puts most of it in PSRAM), and residual *bounds* from the three boots.

| boot | up | `ACTUAL DRAM USED` | attributed | residual |
|---|---|---:|---:|---:|
| #6 minimal build | kernel + cmd_exec + debug_out | 51,948 | 41,560 | 10,388 |
| #7 full build | WiFi connected + HTTP + OLED + gamepad | 103,620 | 68,924 | 34,696 |
| #8 full build | BT on, G2 + R1 connecting (WiFi off) | 164,524 | 111,876 | 52,648 |

Internal heap totals 287,523 B on the full build vs 370,971 B minimal. The 83.4 KB difference is,
exactly: **+39.4 K of IRAM code** (`.iram0.text` 120.8 K vs 81.4 K, mirrored out of SRAM as
`.dram0.dummy`), +29.9 K `.bss`, +14.1 K `.data`. For lever-hunting that order matters: IRAM
placement (`CONFIG_ESP_WIFI_IRAM_OPT` and friends) is a bigger knob than `.data`.

| feature | task stacks (measured) | static `.bss` int (by address) | heap beyond that | ≈ total int |
|---|---:|---:|---:|---:|
| Kernel baseline (main, IDLE×2, ipc×2, esp_timer, Tmr Svc) | 20,992 + TCBs | — | alloc metadata ~5 K + kernel objs/libc/VFS/NVS ~6 K | ~35 K |
| Core app (cmd_exec, debug_out) | 12,288 + TCBs | 3,350 | debug queues ~1.7 K | ~18 K |
| Bluetooth stack (BTC_TASK, btController, hciT, BTU_TASK) | 20,224 + 4 TCBs | 692 | **bounded, not split:** boot-#8 residual − minimal base = **≤ 42.3 K shared by** BT controller+host, G2/R1 session state, and the full build's own non-radio base (unmeasured — no full-build boot with radios off exists). `BT_ALLOCATION_FROM_SPIRAM_FIRST` + `BT_BLE_DYNAMIC_ENV_MEMORY` push host allocations to PSRAM, so most of this is controller. | ≤ ~63 K |
| G2 glasses (5 tasks) | 39,936 + 5 TCBs | 7,848 | inside the ≤ 42.3 K above | ~50 K + share |
| R1 ring (r1_owner) | 6,144 + TCB | 2,024 | " | ~9 K + share |
| WiFi (wifi, tiT, sys_evt, arduino_events) | 17,152 + 4 TCBs | **413** (the 14.7 K an earlier draft called internal is in PSRAM) | **≤ 24.3 K** shared by WiFi+LWIP+httpd+full-build base (boot #7) | ≤ ~43 K |
| Web server (httpd) | 7,680 + TCB | 667 | inside the WiFi bound | ~9 K + share |
| OLED + gamepad | — | 3,087 | small | ~3 K |

The two radio bounds cannot be split further from existing boots. To split them: one full-build
boot with neither radio started (the base), then `openble` with G2/R1 disabled (controller+host
alone). Boot-#8 minus boot-#7 residuals = 17,952 B is the only boot-supported BT-vs-WiFi delta.

**Worth acting on (measured, previously invisible):** `BTU_TASK` 84 % used at boot during dual
GATT discovery; `main` 77 % + `[MEMSAMPLE] LOW` with BT+G2+OLED up (do NOT apply the census's
minimal-build stack shrinks); `g2_tap_disp`+`g2_session_w` hold 20 KB at 13 %/7 % boot usage
(boot HWM ≠ image-work peak — measure under load before touching).

**PSRAM `.bss` (static, by address):** G2 141.9 K · R1 83.2 K · Web 68.5 K · OLED 61.4 K ·
Debug 57.6 K · LLM 20.2 K · ESP-NOW 16.5 K · WiFi-IDF 14.7 K · Core 12.5 K — of 489,180 B total.
Not a DRAM lever; listed so nobody mistakes it for one.

---

## 3. Empirical cross-check — turn each feature off, rebuild, measure

One feature family to 0 (plus dependents so the config is legal), full rebuild in an isolated
clone, `.bin` measured. Baseline reproduces the live image byte-exact (5,929,712). Ground truth
for "what does deleting X reclaim" — captures string/IDF/cross-file effects attribution can only
estimate.

| variant | image | **saved** | map §1 row(s) | difference is |
|---|---:|---:|---:|---|
| `-Os` instead of `-O2` (sdkconfig only) | 5,154,128 | **775,584** | — | the single biggest lever, zero features lost |
| NETWORK family off (WiFi+web+HTTPS+ESP-NOW+bond) | 3,439,904 | **2,489,808** | ~2.3 M | + shared strings, sodium usage, Arduino wrappers |
| BT family off (BT+G2+R1+TestSuite) | 4,417,504 | **1,512,208** | ~1.36 M | + coexist, BT-only platform bits, G2 content in web/OLED |
| WEB off (web + HTTPS) | 4,591,552 | **1,338,160** | ~1.36 M | close agreement — the web really is ~1.3 MB |
| G2 off (G2+TestSuite; BT/R1 stay) | 5,032,560 | **897,152** | ~720 K | + G2 web page, OLED modes, ESP-NOW G2 paths |
| I2C + OLED + INPUT off | 5,565,184 | 364,528 | ~390 K | |
| OLED + INPUT off | 5,608,192 | 321,520 | ~245 K + shares | |
| Sense camera + mic off | 5,649,424 | 280,288 | ~208 K | + mic glue outside the bucket |
| UART host link + Pi power/fan (+LLM/CM5) off | 5,714,800 | 214,912 | | |
| LLM backend (+CM5 source) off | 5,794,208 | 135,504 | ~93 K | |
| G2 TestSuite off | 5,817,424 | 112,288 | ~80 K | cheapest "free" 110 K on the carrier |
| HTTPS off (plain HTTP stays) | 5,825,776 | 103,936 | 151 K | part of mbedTLS retained by other users |
| Bonded mode off | 5,842,064 | 87,648 | | |
| R1 health off | 5,849,856 | 79,856 | 122 K row | R1 flag doesn't drop `G2_Ring`/`G2_Health` (they ride the G2/BT gates) — the row is the code, the flag is a subset |
| Automations off | **BUILD FAILS** | — | 83 K | never-built combo: `G2_Page_Automations.cpp` links `AUTOMATIONS_JSON_FILE` which exists only under `ENABLE_AUTOMATION` |

Never-built combos surfaced by the sweep: **BT on + web off** (fixed this session — `BLE_Events.cpp`
now includes `System_SensorStubs.h` for the `broadcastEventToAllSessions` no-op) and **G2 on +
automations off** (open — needs a gate/stub decision on the G2 automations page). Sweep script:
`tools/feature_size_sweep.sh` (run it from an isolated clone); raw results:
`docs/feature_size_sweep_results_2026-08-23.tsv`.

### Reading it together

- Map (§1) = where the bytes sit; sweep (§3) = what deleting actually reclaims. Sweep ≥ map
  because features own content inside shared files (their web page, OLED mode, strings, icons).
- **Stacked path from 5.93 MB losing nothing in use:** `-Os` (776 K) → gzip'd page text
  (535–738 K) → strip-on-top (~50 K) → TestSuite off (112 K) → migration-tool flag (28 K) →
  sodium dead pulls (26 K) ≈ **~4.2–4.4 MB image, ~27–30 % free.**
