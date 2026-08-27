# Where the XIAO build's flash goes, and what is *not* gated
### Investigation 2026-08-22 — measured against `build-xiao_s3` as built at 15:24 (BuildConfig saved 15:18)

## 0. The headroom number

| | bytes |
|---|---|
| `build-xiao_s3/hardwareone-idf.bin` | **5,978,496** |
| `factory` partition (`partitions.csv:31`, `0x5B5000`) | **5,984,256** |
| free | **5,760  (0.10 %)** |

It fits — by less than one flash page. Any commit that adds a command row tips it over.
Reference: v0.99.89 (2026-08-16) measured 5,345,776 B, so the image has grown ~633 KB in six days
(BT/Bluedroid was turned on for the carrier build in that window; libbt + libbtdm_app alone are 381 KB).

Composition of the current image (from the link map, `text+rodata+data`, merged strings excluded):

```
4,155,631  everything with a real section        + ~1.65 MB merged string pool + fill = 5.98 MB
2,293,463    libhardwareone.a  (project code)
  296,644    libbt.a
  162,354    libsodium
  150,309    libnet80211
  143,347    libarduino
  106,672    libhardwareone_libs.a   <-- 78,388 of this is ONE file: liblc3 tables.c
```

## 1. Headline: file-level gating is not the problem

Every whole-file gate was checked against the linked image, not just the source. **They all work.**

* CMake excluded 7 of 176 `.cpp` (`OLED_Mode_Map`, `System_EdgeImpulse`, `System_Maps`,
  `WebPage_DarkRoom`, `WebPage_Games`, `WebPage_Maps`, `i2csensor_ano_encoder`).
* The other 169 compile, but the ones whose feature is off contribute **zero placed bytes** —
  verified individually in the map: `System_MQTT` (75 KB object → 0 B linked), `System_ESPSR`,
  `WebPage_MQTT`, `WebPage_Speech`, `WebPage_Bond`, `System_LLM_Model/Kernels/Sampler/Tokenizer`,
  and every disabled `i2csensor_*` (apds9960, bno055, ds3231, mlx90640, pa1010d, pca9685,
  rda5807, sths34pf80, vl53l4cx). Whole-file `#if` + `--gc-sections` is doing its job.
* The 1.03 MB of embedded web assets for disabled pages (`WebPage_Games.h` 1,028,632 B,
  `WebPage_DarkRoom.h` 710,632 B, `WebPage_Maps.h` 127,734 B) are correctly behind
  `#if ENABLE_WEB_GAME_MAZE` / `_DARKROOM` / `ENABLE_WEB_MAPS` at
  `WebServer_Server.cpp:101-109`. None of it is in the image.

Symbol-level sweep for every disabled feature found only crumbs leaking past the gates —
icons, a few no-op command stubs, `handleOLEDMQTTPage` (606 B). Full per-feature leak
totals: MQTT 1,681 B · ESP-SR 1,011 B · EdgeImpulse 1,202 B · thermal 1,873 B ·
APDS 247 B · ToF / IMU / RDA5807 / DS3231 / STHS34 / PCA9685 **0 B**.

**So the reason the build is full is not leaked disabled code. It is code that never got a
flag in the first place, plus tables that `--gc-sections` structurally cannot reach.**

## 2. What has no gate at all — ranked, all measured in *this* image

| # | Item | Bytes | Flag today | Note |
|---|---|---|---|---|
| 1 | ~~**liblc3 dead MDCT windows**~~ | ~~38,496~~ | **PARTLY FIXED — `LC3_PLUS=0 LC3_PLUS_HR=0`, see §9** | 27 `mdct_win_<dt>_<sr>` tables; only `mdct_win_10m_16k` (1,040 B) is reachable — G2 calls `lc3_setup_decoder(10000, 16000)` and nothing else (`G2_Glasses.cpp:3841`, `:4007`, `:25481`). Kept alive by the runtime-indexed pointer table `lc3_mdct_win[LC3_NUM_DT][LC3_NUM_SRATE]` (`tables.c:4040`, read at `mdct.c:211,377`), which defeats `--gc-sections`. The LC3 **encoder** is already gone. |
| 2 | ~~**`g2Probe*` diagnostics**~~ | ~~35,398~~ | **FIXED — subsumed by `ENABLE_G2_TESTSUITE`** | See §7. Their only caller was the TestSuite dispatch table, so gating that TU dropped all of them. |
| 3 | **`WebServer_MigrationTool.cpp`** | **28,282** | `ENABLE_MIGRATION_TOOL` exists but is `#define`d to `ENABLE_HTTP_SERVER` (`System_BuildConfig.h:583-584`) and **CMakeLists never greps it**, so the file always compiles when web is on. |
| 4 | ~~**`G2_Page_TestSuite.cpp`**~~ | ~~25,566~~ | **FIXED — `ENABLE_G2_TESTSUITE`** | On-glasses test harness. See §6. |
| 5 | ~~**`G2_Pet.cpp`**~~ | ~~18,452~~ | **FIXED — `ENABLE_G2_PET`** | Novelty feature. See §6. |
| 6 | **Icons for disabled features** | **11,215** | one coarse gate (*current* state — the fix is ~22 guards, see §8) | `System_Icons.cpp` is 7,657 lines / 93,488 B of rodata behind a single `#if ENABLE_HTTP_SERVER \|\| ENABLE_OLED_DISPLAY` (line 11); only NeoPixel is sub-gated. 22 icons belong to features that are off: rtc 1,118 · edgeimpulse 1,067 · radio 982 · tof_radar 921 · espsr 872 · servo 841 · presence 830 · thermal 820 · mqtt 800 · gamepad 784 · imu_axes 772, plus eleven 128 B OLED bitmaps. |
| 7 | **`kDebugMappings`** | **5,120** | none | One flat table; rows for every disabled family ride along. |
| 8 | **`cmd_debug*` / `cmd_set_*` stubs for off features** | **3,076** | none | 73 symbols (`debuggps`, `debugthermal*`, `debugtof*`, `debugimu*`, `debugapds*`, `debugrtc*`, `debugpresence*`, `debugmaps*`, `debugmqtt*`, `debugsr*` — `System_Debug.cpp:2083-2239`). Row strings and `CommandEntry` slots are extra on top. |
| | **subtotal** | **≈ 165,600** | | ≈ 29× the current 5,760 B of headroom |

Not counted above, flag-controlled and therefore the user's call:
`i2csensor_seesaw` 15,737 B is linked because `INPUT_DEVICE_TYPE 1` / `CUSTOM_ENABLE_GAMEPAD 1`,
and `System_I2C.cpp` (41,884 B, 3,217 lines) carries 526 mentions of sensors that are all off
behind only 90 `ENABLE_*_SENSOR` guards.

## 3. Two levers that dwarf all of the above (not gating, stated for honesty)

1. **`-O2` → `-Os`.** `build-xiao_s3/sdkconfig:720` has `CONFIG_COMPILER_OPTIMIZATION_PERF=y`.
   The root `sdkconfig.defaults:195` already says `..._SIZE=y`; it is overridden by
   `sdkconfig.defaults.esp32s3:55`. A relink harness measured this at **650,752 B** with a
   byte-identical `-O2` control. One line.
2. **Web asset comment/indent strip: 158,133 B**, fully designed in
   `docs/WEB_ASSET_STRIP_PLAN` work / `[[project_web_asset_strip_plan]]` (websrc/ move +
   CMake pre-pass; all-or-nothing, four documented gotchas).

## 4. Suggested order of work

**Batch A — pure gating, no behaviour change, ≈ 100 KB**
1. LC3 window trim (38,496 B) — `#if` the unreachable `(dt,sr)` entries in `tables.c` behind a
   `LC3_ONLY_10MS_16KHZ`-style macro and NULL the pointer-table slots. Assert in
   `lc3_setup_decoder` so a future 48 kHz caller fails loudly instead of dereferencing NULL.
2. `ENABLE_G2_DIAGNOSTICS` covering `g2Probe*` + `G2_Page_TestSuite.cpp` (60,964 B), CMake-gated.
3. Make `ENABLE_MIGRATION_TOOL` a real flag: default it to 0, add the CMake grep so the `.cpp`
   drops out (28,282 B). *(This is Batch-1 item A1 of `docs/BUILD_FLAG_FIX_PLAN.md`, still open.)*

**Batch B — rows and tables, ≈ 20 KB**
4. `ENABLE_G2_PET` (18,452 B).
5. Gate the per-feature icon blocks in `System_Icons.cpp` (11,215 B now; more on screenless builds,
   where the PNG *and* the OLED bitmap of every icon ship together).
6. Gate the disabled-family rows in `kDebugMappings` and the `cmd_debug*` table (8,196 B + strings).
   Note the X-macro **flag banks** are sanctioned and stay — only the *rows* are fixable.

**Batch C — the real money**
7. `-Os` (650 KB) and the web strip (158 KB).

## 5. Method / reproduction

* `python3 tools/map_attribute.py build-xiao_s3/hardwareone-idf.map [filter] [n]` —
  per-object `text/rodata/data/bss` from the link map.
* `xtensa-esp-elf-nm --print-size --defined-only …elf` for the symbol sweeps.
* **Do not** use `esp_idf_size --archives` for rodata: GNU ld folds all `SHF_MERGE|SHF_STRINGS`
  sections program-wide and then re-lists each contributor at its *pre-merge* size, so the
  per-object `str~` column sums to 3,691,206 B against a true pool of ~1.65 MB
  (see `[[project_firmware_size_accounting]]`).

Related: `docs/BUILD_FLAG_COVERAGE_AUDIT.md`, `docs/BUILD_FLAG_FIX_PLAN.md` (2026-08-07, 74 findings,
list-only — its Tier-0 items are about *other* configs breaking, not about today's XIAO image).


---

## 6. Done: `ENABLE_G2_PET` + `ENABLE_G2_TESTSUITE` (2026-08-22)

Both features now have real build flags, defaulting to **0**.

**Measured on `build-xiao_s3`:**

| | image | free in `factory` |
|---|---|---|
| before | 5,978,496 | 5,760 (0.10 %) |
| both flags = 1 (reverse-arm check) | 5,979,824 | 5,456 (0.09 %) |
| **both flags = 0 (shipping)** | **5,847,024** | **137,232 (2.3 %)** |

**Saved 131,472 B — 3× the 44,018 B the two objects alone accounted for.** The extra came
from `G2_Glasses.cpp`, which shrank 303,151 → 249,693 B (**53,458 B**): the pet page worker,
its BLE image-staging loop, `cmd_g2pet`, the `lstPet` tap dispatch and the System→Tests
dispatch all live there, not in the feature TUs. Merged-string dedup gave the rest
(G2_Glasses' pre-merge string contribution fell 112,023 → 93,044 B).

**What changed**
* `System_BuildConfig.h` — two plain-literal flags in §3 next to `ENABLE_R1_HEALTH`, plus a
  DERIVED rule forcing both to 0 when `!(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)`, and both
  added to the header banner's list of CMake-grepped names.
* `CMakeLists.txt` — defaults, the two regexes, the BT+G2 force-off, and both `.cpp` moved out
  of the unconditional source list into `list(APPEND ...)` guards that log ENABLED/EXCLUDED.
* `G2_Pet.{h,cpp}` / `G2_Page_TestSuite.{h,cpp}` — existing whole-file guard extended with the
  new flag; the headers' inline no-op stub arms already existed and now cover the flag-off case.
* `G2_Glasses.cpp` — 13 gated sites: forward decls, the `gPetPage*` volatiles, the `lstPet`
  container tap dispatch, the Apps/System launcher rows and their `case` arms, the info-builder
  and page-description strings, `kTestSuitePage` + its `g2RegisterPage`, and the whole
  pet worker/entry/command block.

**Deliberately left ungated:** the `APP_ROW_PET` / `SYS_ROW_TESTS` enumerators and
`G2_HIJACK_PAGE_TESTS` (`G2_Glasses.h:932`). Row ids must stay stable across flag combinations —
that is the launcher builders' documented design — and a hijack page id must never be minted
as a local cast.

**Verified:** `xiao_s3` green with both flags 0 *and* both flags 1 (the on-arm reproduces the
original image to within 1,328 B); `feather_esp32_v2` (BT off) green, both features correctly
EXCLUDED by the derived rule. No new compiler warnings — the `-Wunused-function` hits in
`G2_Glasses.cpp` (`fillStripePattern`, `sendRebuildListNamedAndWait`,
`sendRebuildMultiTextAndWait`, `sendToBoth`, `enumerateDiagService`) each have exactly one
reference in the file, their own definition, so they were already unused before this change.

**Not hardware-tested.** Flipping either flag to 1 restores the feature.


---

## 7. The `g2Probe*` item was already paid for (2026-08-22)

Measured, before vs after the §6 change, over the linked ELF:

```
BEFORE  78 probe-experiment symbols   43,252 B
AFTER    5 probe-experiment symbols    1,398 B
```

**41,854 B of the 131,472 B in §6 was the probes.** Nothing separate had to be gated for them.

Why: every `g2ProbeImageQ*` is `extern`, declared in `G2_Glasses.h` — but its *only* reference
anywhere in the firmware was the dispatch table in `G2_Page_TestSuite.cpp:2415-2494`
(`case N: fn = g2ProbeImageQ…;`). Dropping that TU made the whole Q-series unreferenced, and
`--gc-sections` took all 40 of them plus their private helpers (`probeBanner`, `probeFooter`,
`runMixedListImageProbe`, `runQ30ListTextImageProbe`, `probeLiveScrollingBarSolo`,
`buildMoveSpriteBmp`, `buildLz4ArtBmp`, `g2Lz4WrapName`, `lz4TimeLeg`, `bmpDrawRect4bpp`).

**Lesson for the rest of this list: check the caller graph before pricing a gate.** A large
block of `extern` functions with a single call site in a TU you are already removing costs
nothing extra to gate — and conversely, a "diagnostics" name is not evidence a symbol is
diagnostics-only.

The five survivors were the two wire-probe CLI commands, `g2probe` and `g2imgprobe`
(1,398 B) — arbitrary-pb-command and Cmd=3 multi-fragment RE tools, reachable from the CLI
with no TestSuite involvement. Those are now gated under `ENABLE_G2_TESTSUITE` too (the flag's
documented meaning — bring-up / RE tooling — already covers them, so no third flag).
`parseHexNibble` / `parseHexBytes` stay: `g2probe` is not their only caller.

**Deliberately kept:** the `probe*`-named *helpers* that production page workers share —
`probeTearDownActiveContainer`, `probePrepImageCreateAck`, `probePrepImagePushAcks`,
`probeWaitImagePushAcks`, `probeWaitImageCreateAck`, `probePostProbeShutdown`,
`probeHoldUntilTapOrTimeout`, `imageProbeBegin/End`. Despite the names, these are called by the
BMP/JPG viewers, the camera viewer and stream, Maps, Health and the keyboard pad. The name
prefix is historical — they were extracted from the probes and became the shared image-ack
plumbing.

### Running total

| | image | free in `factory` |
|---|---|---|
| start of day | 5,978,496 | 5,760 (0.10 %) |
| **now** | **5,845,280** | **138,976 (2.3 %)** |
| | **−133,216 B** | **24× the headroom** |

Verified: `xiao_s3` green with `ENABLE_G2_TESTSUITE` 0 and 1; `feather_esp32_v2` green.

## 8. Icons: how to do it with 22 guards, not 66

Each icon is three things — `icon_<name>_png[]`, `icon_<name>_bitmap[]`, and a row in
`EMBEDDED_ICONS[]`. But the two arrays are **`static`**, so they are emitted only if something
references them. Gate the *row* and the arrays fall out on their own (unreferenced static data
is either never emitted or dead-stripped by `--gc-sections`; either way the bytes go). So:

1. **Fix the count first — this is load-bearing.** `System_Icons.cpp:7630` is
   `const size_t EMBEDDED_ICONS_COUNT = 105;`, a hand-maintained literal. Change it to
   `sizeof(EMBEDDED_ICONS)/sizeof(EMBEDDED_ICONS[0])`. Without this, removing any row makes
   `findEmbeddedIcon()` and the gallery loop at `WebServer_Server.cpp:5061` run off the end of
   the array — silent, and only on trimmed builds.
2. **Guard only the table rows** — 22 `#if`/`#endif` pairs, all contiguous in one table at the
   end of the file, nowhere near the 7,000 lines of byte arrays.
3. Confirm every lookup tolerates a miss. `findEmbeddedIcon` already returns `nullptr`, and the
   `ENABLE_HTTP_SERVER || ENABLE_OLED_DISPLAY`-off arm at `:7651` already ships
   `EMBEDDED_ICONS_COUNT = 0` with a null-returning `findEmbeddedIcon`, so the miss path exists
   and is exercised.

**Fewer still, if it is worth restructuring:** make the table an X-macro list —
`ICON(thermal, ENABLE_THERMAL_SENSOR)` — with `ICON_ROW_0(n)` expanding to nothing and
`ICON_ROW_1(n)` to the row, selected by pasting the flag value onto the macro name. That is the
same trick the debug-flag banks already use in this codebase, it makes the count automatic, and
it makes each icon's gate a single token on a single line. It is a bigger diff than option 2 for
the same bytes, so it is only worth it if icons are going to keep being added.


---

## 9. LC3 trimmed to baseline (2026-08-22)

**Not a liblc3 flaw — an upstream extension that defaults on.** Most of the dead window
tables are **LC3plus** (2.5 ms / 5 ms frame durations) and **LC3plus HR** (48/96 kHz
high-res). Bluetooth LE Audio's baseline LC3 is 7.5 ms and 10 ms only, and the G2 mic is
hardwired to 10 ms / 16 kHz mono (`kMicLc3FrameUs` / `kMicLc3SampleHz`,
`G2_Glasses.cpp:3841-3842`; both `lc3_setup_decoder` call sites pass those constants).

liblc3 gates both behind `LC3_PLUS` / `LC3_PLUS_HR` (`liblc3/src/common.h:38-45`), which
default to 1, and writes the table for it: `LC3_IF_PLUS(mdct_win_2m5_8k, NULL)`.

**Change:** one property on the `set_source_files_properties` block that already existed —
`COMPILE_DEFINITIONS "LC3_PLUS=0;LC3_PLUS_HR=0"` at
`components/hardwareone_libs/CMakeLists.txt`. **No edits to the vendored tree.**

| | bytes |
|---|---|
| `tables.c.obj` before | 78,388 |
| `tables.c.obj` after | 44,840 |
| **image** | **5,845,280 → 5,811,488 = −33,792** |

More than the 24,320 B of windows alone, because `LC3_PLUS=0` also drops the per-`dt` band-limit
and `ns`/`ne` tables and the 2.5/5 ms code paths.

**Safe by construction, and statically verifiable end to end:** `resolve_dt()` /
`resolve_srate()` (`lc3.c:59-77`) are guarded by the same flags, so a 2.5 ms or HR request now
resolves to `LC3_NUM_DT` / `LC3_NUM_SRATE` and `lc3_setup_decoder()` returns `NULL` at
`lc3.c:706` — a clean rejection, never a NULL window dereference. The live path still resolves:
`resolve_dt(10000,false) → LC3_DT_10M`, `resolve_srate(16000,false) → LC3_SRATE_16K`, and
`mdct_win_10m_16k` (1,040 B) is present in the linked map. `lc3_mdct_win` is still 112 B =
28 pointers = `LC3_NUM_DT(4) × LC3_NUM_SRATE(7)`, so the enum indices did not shift — only the
pointed-to data went away, replaced by NULL in the unused slots.

**This is the floor without touching vendored code.** `LC3_PLUS` and `LC3_PLUS_HR` are the
*only* two configuration macros liblc3 exposes — every other `#ifndef` in `src/` and `include/`
is an include guard. The 15,216 B that remains is baseline LC3: the 7.5 ms and 10 ms windows for
8/16/24/32/48 kHz, of which only `mdct_win_10m_16k` is reachable. Recovering that last
~14,000 B means patching `tables.c` / `lc3_mdct_win`, which is explicitly out of scope.

**Not hardware-tested** — the G2 mic decode path needs a real device.

### Running total for the day

| | image | free in `factory` |
|---|---|---|
| start | 5,978,496 | 5,760 (0.10 %) |
| after Pet + TestSuite (+ the probes it took with it) | 5,845,280 | 138,976 (2.3 %) |
| **after baseline LC3** | **5,811,488** | **172,768 (3.0 %)** |
| | **−167,008 B** | **30× the starting headroom** |

Verified: `xiao_s3` and `feather_esp32_v2` both green.


---

## 10. Second sweep — why the first one kept missing things (2026-08-22)

Three shapes defeat the method I opened with. Each one is why a later item looked like a
surprise rather than something §2 should have listed:

| shape | example | what is blind to it |
|---|---|---|
| Feature code living in a **shared, always-compiled TU** | the Pet page worker + `cmd_g2pet` + tap dispatch, 53,458 B **inside `G2_Glasses.cpp`** | per-object attribution — the object is "G2 glasses", which is enabled |
| Code kept alive by **one dispatch table in another TU** | 40 `g2ProbeImageQ*`, 41,854 B, referenced only from `G2_Page_TestSuite.cpp` | per-object *and* per-feature symbol naming — nothing about the probes says "TestSuite" |
| Data pinned by a **runtime-indexed pointer table** | `lc3_mdct_win[dt][sr]`, `EMBEDDED_ICONS[]` | `--gc-sections`, `esp_idf_size`, and any dead-code sweep — the table is live, so its contents are live |

**Tooling trap, for the next person:** the link map lists *both* the sections that were kept and
the ones `--gc-sections` discarded, and discarded ones have load address `0x0`. Attribute without
filtering on `addr != 0` and gc'd code reappears in your report as if it shipped — which is
exactly what my first per-symbol listing did, showing probe functions that were already gone.
`tools/map_live_sections.py` filters them.

### What the live-section sweep finds that is still ungated

**1. The `DBG_FLAG_LIST` layer — 38,386 B total, ~9,900 B of it for disabled families.**
`System_DebugFlags.h:152` is a single X-macro list of 128 rows, and it generates *everything*:
`debugSettingEntries` (9,520 B), `kSystemLogFlagsOptions` (5,482 B), `kDebugMappings` (5,120 B),
`debugCommands` (4,224 B), `kDbgMask` (4,096 B), `kDbgTag`, `kDbgSettingPtr`, `kDbgCmdRetName`,
`kDbgCmdIdent`, `kDbgOptLabel`, plus the `cmd_debug*` CLI thunks. 43 of 167 registry rows and
47 of 182 command rows belong to features that are off (MQTT, ESP-SR, Maps, thermal, ToF, IMU,
APDS, GPS, RTC, presence, FM).

*This is the best structural target left* — one list, one place to add a gate, ~8 tables shrink
together, the same "one guard per row" shape that makes the icons job tractable. **But it is not
free:** `System_DebugFlags.h:633` carries `static_assert(DBG_FLAG_COUNT == 128, …)`, a deliberate
drift tripwire that will fire, and the `kDbg*` arrays are positional (`[DBG_FLAG_COUNT]`
initialised from the list in order), so removing rows shifts ordinals. Bit numbers are explicit
per row and the persisted identity is (group, jsonKey), so nothing on disk should move — but that
needs verifying, not assuming. Prior analysis also concluded the flag *banks* are sanctioned and
only *rows* are fixable, which caps the realistic recovery nearer 6-8 KB than 9,900 B.

**2. `i2cCommands` (`System_I2C.cpp`, 33 rows, zero internal gates)** in a 41,884 B
always-compiled TU. Twelve rows are per-sensor bus selectors — `gpsbus`, `rtcbus`, `fmradiobus`,
`presencebus`, `imubus`, `thermalbus`, `tofbus`, `apdsbus`, `servobus` — for sensors that are all
compiled out. Low KB, but a clean and independent fix.

**3. Icons — 11,215 B**, see §8.

### Residual: what is actually left of the disabled features

Name-matched across the whole current image: **6,002 B** — MQTT 1,681 · thermal 1,865 ·
EdgeImpulse 1,202 · ESP-SR 1,007 · APDS 247. Most of that *is* the icon data already counted in
§8; the rest is a handful of no-op command stubs.

**So the big fish are caught.** Everything still ungated lives in the row/table layer and totals
roughly 25-30 KB, at a far worse effort-per-byte than anything done today. The next large numbers
are not gating at all: `-Os` (650,752 B) and the web-asset strip (158,133 B).


---

## 11. Sensor bus commands moved to their sensor modules (2026-08-22)

**The right fix was relocation, not guards.** Nine `<sensor>bus` commands lived in
`System_I2C.cpp` — an always-compiled TU — while every other command belonging to those sensors
already lived in the sensor's own build-gated `.cpp`. `System_I2C.cpp` even carries a comment
recording that the sensor-specific *I2C clock* commands (`thermalI2cClockHz`, `tofI2cClockHz`)
made exactly this move earlier; the bus commands were simply left behind.

Moved `cmd_gpsbus`, `cmd_rtcbus`, `cmd_fmradiobus`, `cmd_presencebus`, `cmd_imubus`,
`cmd_thermalbus`, `cmd_tofbus`, `cmd_apdsbus`, `cmd_servobus` — handler *and* table row — into
`gpsCommands` / `rtcCommands` / `fmRadioCommands` / `presenceCommands` / `imuCommands` /
`thermalCommands` / `tofCommands` / `apdsCommands` / `servoCommands`. Each of those tables is
already inside its file's `#if ENABLE_<SENSOR>` guard (verified per file), so the commands now
compile out with their driver and **no new `#if` was needed for them at all**.

`setDeviceBusAndReport` was `static`; it is now `i2cSetDeviceBusAndReport`, declared in
`System_I2C.h`, so all twelve `<device>bus` commands still share one parse/clamp/persist/report
implementation rather than duplicating it nine times.

`oledbus`, `inputbus` and `fuelgaugebus` stay in `System_I2C.cpp`: their drivers are not
independently build-gated modules, so there is nowhere better for them to live.

**The trap this exposed.** `i2cSettingEntries` carries a `cmdKey` column naming the CLI command
that applies each setting, and `OLED_SettingsEditor.cpp:167` resolves it with **no fallback** —
it builds `"<cmdKey> <value>"` and executes it. Moving the command without touching the setting
row would have left the OLED settings editor firing `gpsbus 1` at a build where no such command
exists. The nine matching rows are therefore guarded on the same `ENABLE_*` flags. They stay in
`i2cSettingEntries` rather than moving to the sensor files, so their JSON identity — and the
persisted settings keys — are unchanged.

**Result:** image 5,811,488 → 5,811,168 (**−320 B**); `System_I2C.cpp.obj` 41,884 → 40,511
(−1,373 B of sections, −1,123 B of pre-merge strings). The image figure is well under my
~1,253 B estimate — part of the object saving is returned by `i2cSetDeviceBusAndReport` becoming
a real extern symbol instead of a static the compiler could fold, and I cannot attribute the
remainder precisely without the pre-change map.

Verified by absence in the linked `.bin`: `gpsbus`, `thermalbus`, `Route PA1010D GPS to bus`,
`GPS bus (reboot required)` — all **0 occurrences**; `oledbus` and `Route MAX17048 fuel gauge to
bus` still present, as intended. `xiao_s3` and `feather_esp32_v2` both green.

**Judge this one on correctness, not bytes.** 320 B is noise against the day's 167 KB. What it
fixes is that `gpsbus` no longer appears in `help` — and no longer accepts a value — on a board
whose GPS driver was never compiled.


---

## 12. Full sweep for the same pattern (2026-08-22)

Method: classify all 176 `.cpp` as CMake-**unconditional** (96) or **conditional** (80), then find
every `CommandEntry` row in an unconditional file that is *not* inside any `#if` and whose subject
is a feature owned by a conditional file. **31 hits.** They split three ways:

### (a) Keep-list — must NOT be moved or gated (11 rows)

`thermalenabled` · `tofenabled` · `imuenabled` · `gpsenabled` · `fmradioenabled` ·
`apdsenabled` · `rtcenabled` · `presenceenabled` · `llmenabled` · `eiautostart` ·
`automationautostart`, plus `maps` at `System_FeatureRegistry.cpp:557`.

These are the master **enable/autostart** pairs. `System_FeatureRegistry` and `System_RamFlush`
read them from always-compiled code *by design*, and a `featureRegistry[]` row must never be
`#if`-wrapped — the Android app treats a missing id as "assume present", so wrapping hides
nothing and breaks capability reporting. This is the documented exception, not an oversight.

### (b) Genuinely misplaced — 9 rows, 935 B

All in `settingEditorCommands` (`System_Settings.cpp`): `tofi2cclockhz` · `gpsdevicepollms` ·
`presencedevicepollms` · `apdsdevicepollms` · `fmradiodevicepollms` · `eirequirelabels` ·
`eimaxdetections` · `eiinputsize` · `eiinterval`.

Their backing `SettingEntry` rows **already live in the gated sensor files**
(`gpsDevicePollMs` in `i2csensor_pa1010d.cpp`, `tofI2cClockHz` in `i2csensor_vl53l4cx.cpp`, …) —
only the editor command was left behind. `tofi2cclockhz` is the sharpest example: the comment in
`System_I2C.cpp` says the ToF *clock* command already moved to its sensor module, and it did — but
its settings-editor twin did not follow.

**These are NOT the `gpsbus` shape, and should not be fixed the same way.** `cmd_gpsbus` had a real
body touching `gSettings.gpsBus`. These nine are generic thunks generated by `SETTING_EDITOR_CMD`
(`System_Settings.cpp:3082`), whose entire body is
`findSettingByCmdKey(key)` → `handleSettingCommand(e, a)`. They never mention the sensor.
Relocating them would mean exporting the macro plus `findSettingByCmdKey` and
`handleSettingCommand` into six sensor drivers — spreading the settings machinery across the
codebase to recover 935 B. Bad trade.

**And there is no correctness bug here**, unlike §11: with the `SettingEntry` gated out,
`findSettingByCmdKey` returns null and the thunk already answers
`"Error: setting not found for this command"`. The rows are inert, just not absent.

If they are ever worth doing: `#if`-guard the `SETTING_EDITOR_CMD` line and its table row in
place — 18 guards, 935 B, no relocation. Worst effort-per-byte on this list.

### (c) Nowhere to move it — 1 row

`fuelgaugebus`. `i2csensor_max17048.cpp` has no command table of its own, so leaving it in
`System_I2C.cpp` (as §11 did) is correct.

### Conclusion

The `gpsbus` class — a command with a real body, in an always-compiled file, whose feature owns a
gated module that already has a command table — was **nine rows, and §11 moved all nine**. There
is no second cluster of them. What is left is either deliberate (a) or inert (b).


---

## 13. REGRESSION introduced by §11 — `<sensor>bus` verbs lost their discoverability

**Found by the follow-up investigation. Verified directly; this is a defect in today's work.**

`System_CLI.cpp:126-133`: the `help` module listing **skips a `CMD_MODULE_SENSOR` module
entirely** when its `isConnected()` predicate is false.

- **Before §11:** `gpsbus` lived in `i2cCommands`, whose module row is
  `flags = 0, isConnected = nullptr` (`System_Utils.cpp:3361`) — a non-sensor module, always listed.
- **After §11:** `gpsbus` lives in `gpsCommands`, whose module row is `CMD_MODULE_SENSOR` with
  `[](){ return isSensorConnected("gps"); }` (`System_Utils.cpp:3244`).

So `gpsbus` — the command whose entire purpose is to reroute the GPS to the *other* I²C bus — is
now absent from `help` and `help sensors` **exactly when the GPS is undetected because it is on
the wrong bus.** Same for `thermalbus`, `tofbus`, `imubus`, `rtcbus`, `apdsbus`, `fmradiobus`,
`presencebus`, `servobus`.

Bounded: the verb still *executes* if typed, and `help gps` still lists it — that path calls
`renderModuleHelp(&modules[i], true)` at `System_CLI.cpp:470`, forcing `showAll`. Only discovery
through the general listing is lost. But it is lost in the one situation the command exists for.

**The design tension this exposes.** Cohesion says `gpsbus` belongs with the GPS driver. Semantics
say bus *routing* is how a sensor becomes findable, so it must stay visible while the sensor is
missing — which is what the always-visible `i2c` module gave it. Three ways out:

1. **Revert these nine to `i2cCommands`** and `#if`-guard them there. Correct discovery; back to
   guards; loses the cohesion win.
2. **Keep the move, fix the renderer** so an undetected sensor module is listed as
   `(not detected)` rather than skipped. Fixes the whole class, but changes deliberate behaviour —
   `System_Utils.cpp` descriptions say outright "the module shows in help only when detected".
3. **Keep the move, accept reduced discovery**, and document it.

Owner's call — it is a product judgement, not a technical one. **Not yet resolved.**

## 14. Q1 verdict — gate in place; the real fix already exists

The relocation instinct is right in general but should not be extended here.

**The correctness fix is one function that is already written and already used.**
`settingsEditorHasCommand()` (`System_SettingsEditorCore.cpp:166-170`) resolves a `cmdKey` against
the **live** command registry via `findCommand()`. `G2_Page_Settings.cpp:827` is its **only**
caller. `OLED_SettingsEditor.cpp` does not call it: `setSettingValue` (`:161-176`) checks only
that `cmdKey` is non-empty, calls `executeOLEDCommand` (void, result discarded), and then reports
`"Saved <key> = <value>"` unconditionally at `:829-838`.

Calling the existing helper in `setSettingValue` and `setSettingValueStr` makes gate *placement* a
hygiene question instead of a correctness one — and it is correct no matter where any row lives.
That supersedes the §11 `#if` guards as the actual fix.

**The dead-row population is 21, not 9.** `eiinterval` was missed (thunk
`System_Settings.cpp:3108`, row `:3198`, backing entry `System_EdgeImpulse.cpp:394`). Static
reconstruction for all 21: **~3.0-3.1 KB**. Calibrate that against §11, where the same method
predicted 1,253 B and the image moved 320 B — a 3.9x over-prediction. Treat ~3 KB as an upper
bound, and do not sell this as a size fix; it is 0.05% of the image.

**Prerequisite bug (pre-existing, unrelated to today):** `edgeImpulseCommands[]`
(`System_EdgeImpulse.cpp:2506`) sits inside an `#if ENABLE_HTTP_SERVER` region opened at `:1964`
that never closes before the table, while `System_Utils.cpp:3326` references it under
`#if ENABLE_EDGE_IMPULSE` alone with no stub. **`ENABLE_EDGE_IMPULSE=1, ENABLE_HTTP_SERVER=0`
does not link today.** Latent only because `System_BuildConfig.h:320` pins EI to 0.

## 15. Q2 verdict — do NOT change the assume-present contract

**The premise is wrong: the rule is not preventing better architecture.** It forbids exactly two
things — `#if`-wrapping a `featureRegistry[]` row, and moving three hoisted bools
(`System_Settings.h:836` `automationEnabled`, `:875` `ledStartupEnabled`, `:990` `llmAutoStart`)
back under their `#if`. It blocks no file split, no subsystem boundary, no registry refactor.
There is no architecture on the other side of it — just 16 fewer table rows on xiao_s3.

Measured recovery: **1,304-1,544 B** (448 B of rows at 28 B each, 128 B of predicates — `nm -S`
shows every `is*Compiled()` is 7 B on an 8 B stride — and 728-968 B of strings depending on
dedup method). That is 0.022-0.027% of the image, against **10 files across two repos in
lockstep** and permanently losing the ability to distinguish "compiled false" from "old firmware".
The keep-list bools cost ~zero flash: `ledStartupEnabled`, `ledstartupenabled` and `llmAutoStart`
each appear **0 times** in the shipped `.bin`; their real cost is 29 bytes of DRAM.

**And the contract binds nothing shipped.** `DeviceCapabilities.kt` is untracked (`??`) in the
Android repo and `git grep` finds no `loadCapabilities` in `HEAD` — **no released APK
(versionCode 34 / 2.6.3) parses `features json` at all.**

### What IS constraining you — and it is not the rule

**`features json` is ~500 bytes from a hard cliff.** Worst case measures **3,573 B** against a
usable ceiling of **4,094 B** (`System_FeatureRegistry.cpp:735` fails at `len >= jbuf_SIZE - 1`;
`jbuf` is matched to `CMD_RESULT_MAX`, correctly documented as unraisable). **That is about four
more rows.** The last batch added eight in one commit.

The cliff is caused by three fields with **zero readers**:
`:701 category` · `:702 heapKB` · `:705 toggleable`. The setup wizard reads `heapCostKB` from the
C struct, not the JSON; the text path reads `getCategoryName` from the struct. Deleting the three
lines takes the payload to ~2,058 B — and because it is fragmented at
`SC_MAX_PAY_FRAME` = 195 B with a 30 ms inter-fragment delay
(`System_BleSecureChannel.cpp:256, :265, :317`), that is **19 frames / 540 ms → 11 frames /
300 ms**: ~240 ms off every post-login capability fetch.

**`toggleable` is already wrong on the wire.** `:705` emits the raw
`FEATURE_FLAG_RUNTIME_TOGGLE` bit, while the text path at `:817` emits `canToggleFeature(f)`,
which additionally requires `isFeatureCompiled && !COMPILE_TIME && enabledSetting` (`:618-624`).
They disagree on **every compiled-out row** — 12 of 16 emit `"toggleable":true` while
`features thermal on` answers "Feature not compiled in this build." `:705` is also the only read
of `FEATURE_FLAG_RUNTIME_TOGGLE` anywhere, so deleting it makes that flag write-only. If the field
is ever wanted back, re-add it as `canToggleFeature(f)`.

**Failure mode if the buffer does overrun:** `:735` returns a bare non-JSON string; the Android
capture layer refuses a fragment not starting with `{` (`BleManager.kt:733-735`), so the capture
burns its full 5,000 ms timeout and — captures being serialised — stalls every other page's
fetch, on every login.

## 16. Two separate defects found, each needing its own ticket

1. **`ramFlushReadIntent` (`System_RamFlush.cpp:231-256`) has 22 arms with zero `#if` guards**
   while its sibling `ramFlushReadLive` (`:126-225`) is fully guarded. The comment at `:123-125`
   claims uncompiled features are "paired with an intent that is likewise false, so they never
   diverge" — untrue whenever a stale `*AutoStart` flag is true on a build lacking that sensor.
   Live/intent diverge, a diverge bit is set for a nonexistent feature, and the resume overlay
   reapplies it. **A behaviour bug on trimmed builds, not dead code.**
2. **`getFeaturesByCategory()`** is declared at `System_FeatureRegistry.h:49` and defined nowhere.
   Latent link error the moment anyone calls it.

## 17. Evidence quality

**No agent compiled or relinked anything** — every byte figure in §13-§16 is a static
reconstruction from `build-xiao_s3/hardwareone-idf.{elf,map,bin}`. The one calibration point
available (§11: 1,253 B predicted, 320 B measured) says this method over-predicts substantially.
Three independent reconstructions of the `features json` worst case disagree by ~40 B; the
measured payload is 3,573 B. Do not quote any of these to the byte.

---

## 18. Fixed: Edge Impulse link break, and the phantom `getFeaturesByCategory`

**A. `edgeImpulseCommands[]` was inside the wrong guard.** `System_EdgeImpulse.cpp` opens
`#if ENABLE_HTTP_SERVER` at `:1964` and did not close it until `:2541` — enclosing the CLI command
table at `:2506`. `System_Utils.cpp:3326` references `edgeImpulseCommands` under
`#if ENABLE_EDGE_IMPULSE` alone, with no stub in `System_SensorStubs.h`, so
**`ENABLE_EDGE_IMPULSE=1` + `ENABLE_HTTP_SERVER=0` failed to link.** Latent only because
`System_BuildConfig.h` pins EI to 0.

Fix: close the HTTP region before the Command Registration section and reopen it for
`registerEdgeImpulseHandlers()` only. Guard state now verified by walking the preprocessor stack:
command table under `[ENABLE_EDGE_IMPULSE]`, web handlers under
`[ENABLE_EDGE_IMPULSE, ENABLE_HTTP_SERVER]`, balanced at EOF. CLI commands have nothing to do with
the web server, which is the whole point.

**Caveat, stated plainly: the EI=1 arm is still not built anywhere.** Proving the link is fixed
needs an `ENABLE_EDGE_IMPULSE=1, ENABLE_HTTP_SERVER=0` build, and turning HTTP off means dropping
`NETWORK_FEATURE_LEVEL`/`WEB_FEATURE_LEVEL`, configurations that carry their own known breaks (see
the 2026-08-07 audit's Tier-0 list). The fix is structurally verified, not build-verified.

**B. `getFeaturesByCategory()` cannot be implemented as declared — declaration removed.**
Declared at `System_FeatureRegistry.h:49`, defined nowhere, called nowhere: a latent link error
for its first caller. The signature returns `const FeatureEntry*` plus an out-count, which
describes a **contiguous slice** — but `featureRegistry[]` is ordered for display, not grouped by
category. Verified by walking the row order: NETWORK, DISPLAY, SENSOR and SYSTEM each appear in
**three separate runs**. No pointer+count can describe them.

Replaced with a comment recording why it is absent and pointing at the shape that does work —
`getFeatureCount()` + `getFeatureByIndex()` filtered on `->category`, which is exactly what
`getCategoryHeapEstimate()` already does.

Both verified green on `xiao_s3`; image unchanged at 5,811,168 B, as expected — neither edit
changes what the current config compiles.

---

## 19. CORRECTION to §13 — the bus sweep already probes both buses

**§13's chicken-and-egg framing was wrong, and it was my error.** I asserted `gpsbus` is hidden
"exactly when the GPS is undetected because it is on the wrong bus." The code does not have that
failure mode. Verified directly:

- `discoverI2CDevices()` (`System_I2C.cpp:1359`) builds ONE address list from the compiled rows of
  `i2cSensors[]` and hands the identical list to `scanBusForDevicesSmart(0, ...)` at `:1426` **and**
  `scanBusForDevicesSmart(1, ...)` at `:1435`. No `gSettings.<x>Bus` value is read anywhere in it.
- `isSensorConnected()` (`System_I2C.cpp:255-277`) matches on device **name** with no bus term.

So a sensor plugged into the "wrong" port **is** discovered, **is** registered, and its module
**does** pass the help gate. Widening the sweep is not the fix, because it is already what ships.
(Bus 1 is skipped when `gSettings.i2c2Enabled` is false — but `i2c2busenabled` lives in
`i2cCommands`, an always-visible non-sensor module, so that path has no chicken-and-egg either.)

**The regression is still real, just narrower than I said:** the nine `<sensor>bus` verbs are
absent from the bare `help` listing whenever the sensor is not detected *at all* — not plugged in,
not powered, or the driver never opened. Before §11 they lived in the always-listed `i2c` module.

## 20. Fixed: help visibility, without abandoning detection-gating

`System_CLI.cpp` — detection-gated help is KEPT: an inactive sensor module still gets no row and
no command block. What changed is that its **name** is no longer swallowed, so a user can learn to
type `help gps`:

```
  Not active: thermal, tof, imu, gps, fmradio, apds, rtc, presence
    (not detected, or detected but not started)
    'help <name>' still lists their setup verbs - e.g. 'help gps' for gpsbus
```

Two attacks shaped this and both are reflected:

- **The label is "Not active", not "Not detected".** Only the 8 `isSensorConnected()`-backed
  predicates are detection results. `input`/`gamepad`/`anoencoder`/`camera` read a flag that only a
  successful driver *open* sets; `microphone` reads source reachability. "Not detected" would be a
  false claim for 7 of the 15 sensor modules.
- **`help all` previously printed every module with no status suffix**, so it listed the inactive
  ones without saying which were missing. It now carries `(Not active)`.

Also: a `CMD_MODULE_SENSOR` row with a NULL predicate is now printed plainly rather than treated as
disconnected — matching `renderModuleHelp`, which already treats NULL as connected. And the `i2c`
module blurb (`System_Utils.cpp`) no longer advertises `gpsBus`/`rtcBus`/`imuBus`/`thermalBus`/
`tofBus` as living there, since they don't any more.

## 21. Fixed: `features json` — all three fields kept, and made true

Per the owner's decision the fields stay. They are now correct.

- **`toggleable` was the raw `FEATURE_FLAG_RUNTIME_TOGGLE` bit**, which ignores whether the feature
  is compiled. It now emits `canToggleFeature(f)` — byte-identical to what the text detail path
  already printed, so the two cannot drift.
- **Three rows were lying in the table itself.** `wifi`, `oled` and `i2c` carried
  `FEATURE_FLAG_REQUIRES_REBOOT` *instead of* `FEATURE_FLAG_RUNTIME_TOGGLE`, despite each owning a
  real `enabledSetting`. They now carry both. Verified across the full 30-row matrix:
  `COMPILE_TIME <=> enabledSetting == nullptr` holds on **all 30**, and exactly those three had a
  setting without `RUNTIME_TOGGLE` — so `canToggleFeature` now reading the positive bit
  (`RUNTIME_TOGGLE`) instead of the negative one (`!COMPILE_TIME`) returns an **identical value for
  every row**. Behaviour-neutral by construction; the flags simply stopped lying.
- **`heapKB` reported a cost that cannot be incurred.** A compiled-out feature now reports 0 via a
  single `reportedHeapKB()` accessor used by both the JSON and text paths. The build-planning
  number survives in the text detail as prose: `Heap cost: ~0KB (~24KB if built in)`.
- **`category`:** the `i2c` row was declared `FEATURE_CAT_NETWORK` while sitting under the
  `=== HARDWARE FEATURES ===` header. Now `FEATURE_CAT_CORE`.
- **The flag enum is documented as two orthogonal axes** (mutability: exactly one of
  RUNTIME_TOGGLE/COMPILE_TIME; timing: REQUIRES_REBOOT, optional and only meaningful alongside
  RUNTIME_TOGGLE), so the next row author cannot repeat the mistake.

**And a compile-time tripwire, since keeping the fields means the payload does not shrink.**
`kFeaturesJsonWorstCase()` is a `constexpr` walk of the registry computing the exact worst-case
serialized length; a `static_assert` requires it to leave 256 B of runway inside the 4096 B
buffer. The build now breaks while roughly two rows of *deliverable* headroom still exist, instead
of the payload silently becoming undeliverable in the field. The assert message says explicitly:
do not raise the buffer, do not page the list (a dropped page reads as "those features exist"),
move it behind a dedicated endpoint. `featureRegistry` and `getCategoryName()` became `constexpr`
to make the bound computable. Same shape as the `DBG_FLAG_COUNT` drift assert.

**Cost of §20 + §21, measured:** `System_FeatureRegistry.cpp.obj` 6,842 -> 6,924 (+82 B),
`System_CLI.cpp.obj` 4,732 -> 4,940 (+208 B), `System_Utils.cpp.obj` 59,608 -> 59,418 (-190 B).
Net about +100 B.

**Unrelated: the image is now 5,911,376 B (1% free) because `ENABLE_MAPS` is set to 1 in the
working tree** ("TEMPORARY G2 MAP HARDWARE VERIFICATION"). `System_Maps.cpp` 57,094 B +
`OLED_Mode_Map.cpp` 16,089 B + the other map surfaces account for it. That flag was not touched.

## 22. OPEN DECISION — four bus settings are write-only

`thermalBus`, `tofBus`, `imuBus` and `apdsBus` are **read by nothing**. Verified by reference
count across the component: each appears exactly twice — its own setter command and its settings
row — against `gpsBus` (7 sites) and `rtcBus` (8 sites), which genuinely resolve the bus in their
transactions. Those four drivers use the legacy implicit-bus-0 overloads and hardcode
`pollPaused(0)`.

So `thermalbus 1` persists a value, reports success, and changes nothing. **§11 relocated four
commands that do not work**, which is worse than leaving them where they were — the move implied
they belong to a driver that ignores them.

Two ways out, and it is a product decision:

- **A — make them real:** migrate the four drivers onto the explicit-bus overloads
  (`i2cDeviceTransaction(bus, ...)`, `i2cTaskWithTimeout(bus, ...)`, `pollPaused(bus)`) the way
  `pa1010d` / `ds3231` / `rda5807` / `sths34pf80` already are.
- **B — retire them:** delete the four settings rows and their four setter commands, and say in the
  `i2c` blurb that those four sensors are bus-0 only.

**Not actioned.** Doing either silently would be guessing at intent.

Also dead and worth removing either way: `cmd_thermalstart_queued` / `cmd_tofstart_queued` /
`cmd_imustart_queued` / `cmd_apdsstart_queued` (`System_I2C.cpp:355-373`) are registered in no
command table — only forward-declared. The real `openthermal`/`opentof`/`openimu`/`openapds` bind
handlers in the driver files. These orphans are exactly what would make "edit System_I2C.cpp to fix
openthermal" look correct in review.

---

## 23. G2 Pet deleted; Tests kept as a flag, defaulting ON

**Pet removed outright** at the owner's instruction ("the pet is useless and I don't use it").
`G2_Pet.cpp` and `G2_Pet.h` deleted via `git rm` (recoverable from history), plus 223 lines from
`G2_Glasses.cpp`: the page worker, `cmd_g2pet`, the `lstPet` tap dispatch, the `gPetPage*`
volatiles, the Apps launcher row and its `case`, the forward declarations, the command-table row,
the `APP_ROW_PET` enumerator, `G2_APPS_MAX_ROWS` 8 -> 7, and six stale prose references.
`ENABLE_G2_PET` removed from `System_BuildConfig.h` and from all four of its sites in
`CMakeLists.txt`. Verified: zero `g2Pet` / `PetPage` / `RenderPetBmp` symbols in the linked image.

Leftover to be aware of: a device that ran the Pet keeps an orphaned `/system/pet.json`. Harmless,
and moot given the erase-before-flash policy.

**`ENABLE_G2_TESTSUITE` kept, default changed 0 -> 1.** Defaulting it off was a scope error on my
part: the owner asked for the code to be *gateable*, and shipping it gated-off silently removed the
Tests page from the lens on working hardware. The flag comment now records that turning it off also
drops ~42 KB of Q-series image probes, because this page's dispatch table is their only caller.

**Note on the earlier over-engineering question.** The owner asked whether putting these under the
existing G2 flag would have sufficed. It would not have: before the change all four files already
opened with `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`, which is why they appear in §2 as "flag
today: none" — no guard capable of shedding them on a build where G2 is ON, which is every build
in use. A flag independent of `ENABLE_G2_GLASSES` was the only thing that could recover the 131 KB.
The genuine error was the default, not the flag.

### Build result

| | bytes |
|---|---|
| image | **5,924,416** |
| XIAO factory | 5,984,256 |
| free | **59,840 (1.0%)** |

`G2_Page_TestSuite.cpp.obj` = 26,274 B; 66 TestSuite/probe symbols present; 0 Pet symbols.

**Caveat on that number:** this binary also contains another agent's uncommitted work in the same
tree (map viewport refactor, `System_Events.h`, `HardwareOne.cpp`, `OLED_ESPNow`, and others — 86
source files were modified today between two agents). It is not a clean measurement of this
session's changes alone, and 1.0% headroom is tighter than the 2.3% recorded in §19 for that reason
as much as for the Tests flag.

### Concurrency note — the toggle that was NOT applied

The owner asked to toggle Tests off and rebuild, because the previous build overflowed by
182,496 B. Between that request and acting on it, the other agent set `ENABLE_MAPS` back to 0
(`System_BuildConfig.h` mtime 20:15:52), so the overflow premise no longer held: the build is green
with Tests ON. The toggle was not applied — applying it would have removed a feature the owner had
just asked to have restored, for no benefit.

For the record, the overflow was a shared-`BuildConfig.h` artefact, not a defect in either agent's
work: the identical binary measured 6,166,752 B, which **fits the FeatherS3's 0x615000 factory with
210,720 B free and overflows the XIAO's 0x5B5000 by 182,496 B.** Maps dominates it —
`System_Maps.cpp` 56,834 + `OLED_Mode_Map.cpp` 15,877 + `WebPage_Maps.cpp` 14,351 plus ~127 KB of
embedded web assets in the string pool — against ~68 KB for Tests. Turning Tests off alone would
NOT have brought that image under the XIAO ceiling.

---

## 24. Minimal serial-only build, and the UART-host-link gate (2026-08-23)

### The stripped build

Every feature flag to 0, `CONFIG_BT_ENABLED=n` in `boards/xiao_s3.defaults`, saved sdkconfig
regenerated from defaults (the app flag alone does not reclaim Bluedroid).

| | bytes |
|---|---|
| carrier build | 5,924,416 |
| **minimal** | **1,083,232** |
| saved | **4,841,184 — 82%** |
| free in factory | 4,901,024 (82%) |

96 of 171 translation units compile.

### Three pre-existing breaks had to be fixed — this config had never been compiled

1. **`WebServer_Handle.h`** included `<esp_http_server.h>` and declared `httpd_handle_t server`
   unconditionally, but `esp_http_server` is not a CMake dependency when HTTP is off. The stub
   *definitions* already existed in `System_SensorStubs.cpp`; only the header was ungated.
2. **`System_RamFlush.cpp`** pulled in the real `<WiFi.h>` ungated, redefining the stub
   `WiFiClass`. Its only use is `WiFi.isConnected()`, which the stub provides.
3. **The stub `WiFiClass` was missing half its API** — `buildSystemInfoJson()` calls `RSSI()`,
   `channel()`, `macAddress()` and `localIP().toString()`, and the stub returned `String` from
   `localIP()`. This is Tier-0 item #2 of the 2026-08-07 audit, now actually fixed.

### FOOTGUN: `#define XIAO_ESP32S3_SENSE_ENABLED 0` did not disable anything

The camera/mic gates tested `defined(XIAO_ESP32S3_SENSE_ENABLED)`, **not its value**, so writing
`0` left ~114 KB compiled in (`System_Camera_DVP` 37,924 + `System_Microphone` 34,932 +
`libespressif__esp32-camera` 30,916 + `System_LiveAudio` 10,566). All three gate sites now read the
value; an undefined macro evaluates to 0 in `#if`, so it is safe either way. **That fix alone took
the image 1,427,376 -> 1,130,336.**

### `ENABLE_UART_HOST_LINK` — one new flag, default 1

`System_LiveAudio.cpp` had **no `#if` guard at all** (1,409 lines), held in by its ungated
`gCommandModules` row and by four calls from `System_UartLink.cpp`. `System_UartLink.cpp` was gated
only by `#ifdef UART_LINK_PORT`, which is **board-hardware presence** — and all six board blocks
define it, so it gated nothing on any shipping board.

**`ENABLE_MICROPHONE` is the wrong gate**, which was my first guess and it was wrong: `liveaudio
synth` fabricates its PCM arithmetically and needs no mic at all, so gating on the mic would delete
a working UART bring-up diagnostic on exactly the mic-less carrier boards it serves. The link is a
first-class transport peer of serial/web/BLE, not a mic feature.

**Nothing existing expressed "this board has no Linux host."** `ENABLE_RASPBERRY_PI_HOST_POWER` and
`_FAN` gate only the `cm5 power` / `cm5 fan` command families and say so in their own comments;
`ENABLE_LLM_SOURCE_CM5` is a *consumer* of the link; `UART_LINK_PORT` is hardware presence and
expands to `Serial0`/`Serial1`/`Serial2`, not an integer. Hence one new flag, `ENABLE_UART_HOST_LINK`,
**defaulting to 1** — `gSettings.uartLinkEnabled` already defaults false, so "compiled in, off until
asked" is exactly today's behaviour. Deliberately **not** added to the CMake-parsed list: the `#if`
does the work, and a second copy of the condition in a second language is the silent-regex-failure
mode that list warns about.

**The gate is `#undef UART_LINK_PORT`.** Every consumer already has a correct `#else`/`#ifndef`
arm; undefining the port macro activates all of them at once — the 18 inert stubs in
`System_UartLink.h`, the `cmd_voicefetch` fallback, and all seven guarded regions in
`System_Settings.cpp`. Zero new stubs, zero auth edits.

Only ~72% of `System_UartLink.cpp` is gateable and that is **deliberate**: its session/auth
prologue is unconditional by design so revocation sweeps and `tgRequireAuth` link on every board.
An earlier draft of the plan called that a broken gate and proposed 11 stubs to delete it; that was
wrong and was not done.

**Measured, all four arms built green:**

| config | image |
|---|---|
| link ON (minimal) | 1,130,336 — **byte-identical to before the change**, so the on-path is a true no-op |
| link OFF (minimal) | **1,083,232** (−47,104) |
| mic ON + link ON | green |
| mic ON + link OFF | green — the case that splits the LiveAudio types |

The saving exceeds the two objects' 18,594 B because `#undef UART_LINK_PORT` also drops the
guarded 72% of UartLink, four settings rows and three commands. Residue: `System_UartLink.cpp.obj`
1,526 B (the deliberate auth prologue), `System_LiveAudio.cpp.obj` **gone**.

`System_LiveAudio.h` splits types from functions: `LiveAudioReadyIntrinsicResult`,
`LiveAudioRecorderSource`, `LiveAudioRecorderOutcome` and `LiveAudioRecorderAuthorization` stay
**unconditional** because `System_Microphone.cpp` holds the struct by value and takes the enums by
reference in eight places. `cmd_liveaudio` and its table also stay declared, with fail-clean
out-of-line definitions in `System_SensorStubs.cpp`, honouring the contract in
`System_LiveAudio.h` that the command stays registered and fails cleanly.

### OPEN — needs sign-off before this ships to a carrier board

With the flag at 0, `#undef UART_LINK_PORT` compiles out four settings rows (`uartLinkEnabled`,
`uartLinkBaud`, `uartRequireAuth`, `sessionIdleUart`). Because `writeSettingsJson()` rebuilds the
file from live RAM, **a flag-0 image permanently erases those four keys from `settings.json` on the
first save.** `uartRequireAuth` and `sessionIdleUart` revert to fail-closed defaults, so it is data
loss rather than a security hole — but a hand-tuned `uartLinkBaud` (the carrier runs 2,000,000) is
gone. This path was never reachable before, because no shipping board is portless.

Mitigation, if wanted: add a `UART_LINK_BAUD_MAX` fallback and move the four rows out of their
`#ifdef UART_LINK_PORT` guards — they are pure table entries with `nullptr` handlers. Only the three
`uartlink*` commands then disappear, which is the intended effect. **Not applied.**

### Deferred — `System_Cm5Presence.cpp` (2,124 B)

A third always-linked module in the same family, unconditional in CMakeLists with no preprocessor
directive until line 23. Safe to leave: `ensurePresenceTask()` is lazy and only reachable from
events that originate in the UART port arm, so no task is created and no stack consumed — it is
dead flash, not a live cost. Gating it is a separate pass because `cm5TimeSyncTick()` is called
**unconditionally** from `HardwareOne.cpp:2571` and `cm5PresenceIsProtocolCommand` from `:465`, its
header has no conditional at all, and its snapshot functions return by value. Derive that stub set
from a **link error list, not a grep**.

---

## 25. Settings durability across a non-erase flash — I was WRONG, and the codebase already does this

**Correction to §24's "OPEN — needs sign-off".** I warned that a flag-0 image would permanently
erase four UART keys from `settings.json` on first save. **That is false.** `writeSettingsJson()`
performs a merge-read: it deserializes the existing on-disk file into the very `JsonDocument` it is
about to stamp.

```
System_Settings.cpp:1074  // CRITICAL: Read existing settings first to preserve orphaned sensor
                          // sections. This allows settings from disabled sensors to persist and
                          // show as grayed-out in UI
:1080  deserializeJson(doc, existingFile)
:1120  buildSettingsJsonDoc(doc, ...)   // "orphaned sections remain untouched"
:1157  serializeJson(doc, file)
```

Registered scalars land through **get-or-create** walks that never destroy an existing object —
`jsonPathCreate()` at `:60` calls `.to<JsonObject>()` only when the segment `isNull() ||
!is<JsonObject>()` (`:71-73`). There is **no prune pass**; `readRegisteredSettings()` walks the
*registry*, never the document, so an unknown key is never even observed. Exactly two things are
removed by name: `wifiPrimarySSID` (runtime-only) and a stale `system.debug` block (a completed
one-way migration).

**The sticky-note semantics the owner described are already implemented, deliberately, and the
codebase says so** at `System_Settings.cpp:1410-1411`: *"re-serialises the PARSED FILE with one key
changed, so every other byte — including keys this build does not know — survives untouched."*

**What misled me:** the comment at `:1034-1039` — *"the merge-read cannot recover any of it"* —
is true but scoped to the **failed-load** case. Read cold it denies that a merge-read exists.

### Per-file reality

| file | writer | merge or rebuild |
|---|---|---|
| `settings.json` scalars | `writeSettingsJson()` `:1027` | **MERGE** — keys survive |
| `settings.json` peers/meshes/wifi arrays | destroy-and-rebuild | each rebuild sits behind the same `#if` as its data, so a feature-less build never enters it |
| `users.json`, per-user settings, `automations.json` | deserialize→mutate→atomic write | **MERGE** |
| **`debug.json`** | `writeDebugJson()` `:1222` | **REBUILD** — no merge-read |
| `notifications.json`, `espnow/devices.json`, `mesh_peers.json`, identity files, `ip_bans.json`, waypoints | rebuilt from fixed RAM arrays | **REBUILD** |
| NVS | per-key `nvs_set_*` | merge by construction |

`debug.json` loses **zero keys today** — `debugSettingEntries` is 169 rows with no preprocessor
directives inside it — so it is safe **by accident**, not by design. `:1213-1214` currently says
"it needs no merge-read", which is the standing licence for that gap.

**Blast radius, measured:** 294 `SettingEntry` rows across 35 arrays; **245 sit behind a row-level
`#if`**; 291 persist to `settings.json`. The merge-read is what makes 87% of the registry safe to
gate at all.

### Verdict: document and pin it — do not build a preservation project

Recommended, in order: fix the three misleading comments (`:1074-1075`, `:1034-1039`,
`:1213-1214`); mark the get-or-create walks at `:60-64` and `:2804-2808` **load-bearing** (turning
either into an unconditional `.to<JsonObject>()` silently deletes every unknown key); add one
regression test that seeds an unowned key, saves, and asserts it survives; and either give
`debug.json` the same merge-read or put a hard comment on `debugSettingEntries` saying a `#if`
inside that array requires one first.

**Explicitly rejected:** a `settingsprune` command (an "orphan" classifier would flag
`firmwareVersion`, BLE peers, WiFi networks and ESP-NOW meshes — all written by non-registry code —
and deleting them destroys every pairing and encrypted PSK on the device); and an orphan census on
the save path (registry lookup is a linear `strcmp` walk, so ~90,000 comparisons per save, and
saves fire on every WiFi association).

### The four UART keys: LEAVE THEM GUARDED

They already survive, so no fix is needed — and unguarding them would **create** the only mechanism
by which a flag-0 build could destroy them. Registered rows get read *and written*: a
`settingsTypeCheck` reject `continue`s (`:2677`) leaving RAM at the compiled default, which the next
save stamps to disk — and those defaults are `uartLinkEnabled=false`, `uartRequireAuth=true`,
precisely the values that erase the note. `uartRequireAuth` also has four unconditional readers
(`System_Utils.cpp:5080`, `System_User.cpp:114/693/766`); today the row's absence pins it to its
fail-closed default `true`, whereas unguarded it would load whatever a flag-1 build left on disk.

## 26. Separate defects found on the way — worse than the question asked

These are **load-failure wipes**: they need no reflash at all.

1. **`loadIpBans()`** sets `sIpBansLoaded = true` at `WebServer_Server.cpp:1156` **before** the
   parse at `:1164`. On a parse error it returns at `:1166` with a zeroed `sIpBans[]` and the flag
   set, so the next `banIp()` commits the empty array via `saveIpBans()`. A corrupt `ip_bans.json`
   silently wipes every ban. **Verified.**
2. **`loadEspNowDevices()`** returns on parse error (`System_ESPNow.cpp:8886-8890`) leaving
   `deviceCount == 0`, which `saveEspNowDevices()` then commits — losing pairings and per-device
   AES keys.
3. **`loadMeshPeers()`** (`:776`) has **no `deserializeJson` at all** — it hand-scans
   `content.indexOf("\"mac\":")` at `:810` — and deactivates the live peer table at `:801-806`
   *before* scanning, so a truncated file leaves a partially populated table no boolean can describe.

**The fix rule must be `loaded-ok := (parse succeeded) OR (file legitimately absent)`.** A
success-only flag deadlocks every erase-flashed device: flag never set → save refused → file never
created. `HardwareOne.cpp:1458` already does this correctly for settings.

**Also unaddressed: `writeSettingsJson()` is a read-modify-write with no lock held across it.**
`fsLock("settings.read_for_merge")` at `:1077` is released at `:1115`; `fsLock("settings.write")` is
not taken until `:1142`. **Verified.** There are 51 call sites outside `System_Settings.cpp`, firing
from the WiFi/arduino_events task, BLE peer callbacks, camera, I2C, G2 and the wizard. Two
concurrent writers both merge-read and both open the same `/settings.tmp` (`:1139`) with `"w"` — and
`settingsRestampFirmwareVersion()` uses that same temp path (`:1430`) with its `readText` outside
any lock. Wants a settings-write mutex and distinct temp paths, **before** any work is added to that
window.

---

## 27. Load-failure wipe class — 12 sites, fix in progress (2026-08-23)

**The sweep found 12 sites, not the 3 spotted by accident.** Full inventory and the ordered
15-step plan: `scratchpad/wipe.md`. Fix rule:

```
savable := (parse succeeded OR file legitimately ABSENT) AND (no entry lost at load)
```

Absence is a **first-class savable state decided by an explicit `existsGuarded()` probe**, never
inferred from a failed open — that is the bootstrap escape hatch. Without it an erase-flashed
device deadlocks forever: flag never set → save refused → file never created → flag never set.
The anti-pattern is literally `WebServer_Server.cpp:1157`'s
`if (!filesystemReady || !exists(p)) { ok = true; return; }`.

`NoMemory` and `OpenFailed` are their own arms because both mean **the file is FINE** — telling an
operator to quarantine an intact file on a transient heap shortage is worse than the bug.

### Landed

- **STEP 1 — `user_settings/<id>.json`.** `loadUserSettings()` returns true with an empty object
  when ABSENT but false with the document **cleared** on a parse error. Two callers dropped the
  return value under the comment `// OK if doesn't exist yet` — the author considered only the
  absent case. From a cleared doc, `settings["password"] = hashed` then saved a file containing ONE
  key, destroying every other per-user setting; on the gamepad path the destroyed key is the
  account's own login credential. Both sites in `System_User.cpp` now refuse and return false.
  Every other caller already checked.
- **STEP 6 — `System_ConfigLoad.h`.** The shared classifier: 9-state `Status`, `savable()`,
  `fileIsIntact()`, `classifyParse()`. **Deliberately has no file-reading helper** — each site must
  stream `deserializeJson()` off its own `File` so `NoMemory` stays distinguishable from
  `IncompleteInput`. Reading into a `String` first collapses them and would tell someone to throw
  away an intact file.
- **STEP 9 — `notifications.json`.** `notifPolicyLoad()` filled zero-initialised locals inside two
  nested successes then committed them **unconditionally**, so a corrupt file yielded all-zero
  masks — byte-identical to "no policy file" — which `notifPolicySave()` then wrote back, erasing
  every per-kind override. Now returns early on a parse error and gates the save on
  `gNotifLoadedOk`.
- **STEP 10 — `debug.json`.** `readDebugJson()` had no flag on any of its three failure arms while
  `writeDebugJson()` rebuilds the file from `gSettings` RAM — compiled defaults after a failed
  read — so one corrupt file plus one flag change flattened the operator's entire flag set. Added
  `gDebugLoadedOk` with the absent arm marked savable, mirroring `gSettingsLoadedOk`.

  *Noted tension:* the earlier settings-durability investigation advised AGAINST a `gDebugLoadedOk`
  guard — but that was in the context of also giving `debug.json` a merge-read, where a second
  guard would be redundant. With no merge-read the flag IS the mechanism, and the resulting
  "read-only until reboot" trade is exactly what `writeSettingsJson()` already documents as
  intended.

Build green after each; image 1,083,232 → 1,085,120 (+1,888 B for all four).

### Adversarial review reversed four proposals — do not re-derive these

1. **Do NOT add `gEspNow->deviceCount = 0` to `loadEspNowDevices`.** `deviceCount` is never assigned
   anywhere and `deinitEspNow` retains the PSRAM struct, so the loader is genuinely append-only.
   Resetting it rewrites `dev.name` (an Arduino `String`, freeing the old buffer) while
   `espnow_task` concurrently copy-constructs from the same slot (`System_ESPNow.cpp:9891/:9901`) —
   a **use-after-free**, not a stale read.
2. **Do NOT count the table-full `break` or the duplicate skip as loss unconditionally.** With no
   `deviceCount` reset, a `closeespnow; openespnow` cycle on a full table fires the break on entry
   #1 and would poison `savable()` on a **byte-perfect file**, then send the operator to `discard`
   and destroy 15 real pairings. Gate both on `baseCount == 0`.
3. **Do NOT rebuild `saveEspNowDevices` on ArduinoJson.** A partial PSRAM allocation makes
   `arr.add<JsonObject>()` silently no-op and `serializeJson` emit **valid-but-short** JSON, which
   tmp+rename commits atomically and the next boot classifies `Ok`. The streaming printer cannot do
   this.
4. **`ERROR_AUTHF` does not exist** — `System_Debug.h` defines only `DEBUG_AUTHF`. Use
   `ERROR_STORAGEF`.

Also rejected: **auto-quarantine at load time.** Renaming a corrupt file to `.corrupt` and
continuing as `AbsentOk` converts S6/S7 into the bootstrap state, so the next save writes the empty
table — the original bug with an extra file and no operator signal.

### Still open

- **Blind-safe, not yet landed:** STEP 11 (boot anchors), 12 (capture key — a failed load mints a
  new key over the existing blob, **stranding every AEAD-sealed file**), 13 (waypoints — wipe plus
  cross-map contamination), 14 (boot-time repair), 15 (low tail).
- **Needs hardware:** STEP 2 (`writeTextAtomic`), 3 + 7 (`devices.json`), 4 + 8 (`ip_bans.json`),
  5 (mesh peers).
- **`System_Filesystem.cpp:211-228` DELETES a corrupt critical config file**, converting
  `corrupt` → `absent`, which the fix rule treats as legitimate bootstrap. Must change WITH the
  rule (STEP 14) or it defeats it.
- **Site 4 (`mesh_peers.json`) DEFERRED.** The recommendation was to delete the persistence
  outright, and every field in `MeshPeerHealth` is runtime telemetry (millis() timestamps invalid
  across a reboot, per-session counters, live RSSI). **But** the file's own comment says
  "mesh_peers.json is topology-only", and `System_ESPNow.cpp:14039` says peers are "behind after a
  reboot until `loadMeshPeers()` re-registers all peers" — so it may also restore peer
  REGISTRATION. Whether that is redundant with `devices.json` is unestablished. Do not delete it
  until that is answered.

### Landed (continued)

- **STEP 11 — `boot_anchors.json`, two fixes.** (a) A parse error did `doc.clear()` — "start empty"
  — collapsing up to 16 anchors to one and permanently breaking timestamp resolution for every
  pending user. Now refuses and preserves the file. (b) `cleanupOldBootAnchors()` **failed open**:
  when users.json could not be read, `usersDoc` stayed null, the pending scan never ran, and it
  pruned 16 anchors to 1 — *a read failure of file A triggering a destructive write of file B*. Now
  fails closed and skips the prune.
- **STEP 12 — capture key (NVS).** The only success path required
  `nvs_get_blob(...) == ESP_OK && len == sizeof(sKey)`. A blob that exists but is the WRONG LENGTH
  returns `ESP_ERR_NVS_INVALID_LENGTH`, and a short blob succeeds with `len != sizeof` — both fell
  through to `randombytes_buf` + `nvs_set_blob`, **overwriting the real key and permanently
  stranding every AEAD-sealed capture and health file.** Unrecoverable: the plaintext exists
  nowhere else. Now refuses unless the error is genuinely `ESP_ERR_NVS_NOT_FOUND` (real first use).
- **STEP 13 — waypoints.** The parse-error `return false` sat ABOVE the `memset`, so a truncated
  waypoint file left the **previous map's** waypoints live in `_waypoints` — and `saveWaypoints()`
  (reachable from nine mutators) wrote them into THIS map's file. Cross-map contamination on top of
  a wipe. Now clears before returning, with a `_waypointsLoadedOk` gate on the saver.
- **STEP 14 — boot-time repair.** `System_Filesystem.cpp` **deleted** any critical config file whose
  first non-space byte was not `{` or `[`. That converts `corrupt` → `absent`, the state every load
  gate treats as legitimate bootstrap — silently defeating STEPS 9/10/11 — and for users.json it is
  unrecoverable account loss with no seeder. Now renames to `<path>.corrupt`, which survives the
  `.tmp` sweep and stays readable via the file manager. Also dropped `"/settings.json"` from the
  list: **verified dead** — a repo-wide grep for that literal finds only that line (the real path is
  `/system/settings.json`), so it has never validated anything. Deliberately NOT repointed: settings
  already has two strictly better guards, and a first-character check cannot see truncation, which
  is the dominant corruption mode.

**Verified in two configurations**, since green on one board proves only that board:
minimal serial-only (1,086,192 B) and a regression build with WiFi + HTTP + ESP-NOW + Maps + I2C +
OLED + input all on (3,661,920 B). The second was needed because `System_Maps.cpp` is not compiled
at `ENABLE_MAPS=0`, so STEP 13 had no coverage otherwise.

**Another pre-existing config break found while setting that up:** `I2C_FEATURE_LEVEL` > 0 with
`INPUT_DEVICE_TYPE 0` does not link — `OLED_Utils.cpp`, `OLED_UI.cpp`, `OLED_SettingsEditor.cpp` and
`OLED_FirstTimeSetup.cpp` reference `gNavEvents`, `gDataSource`, `gDataSourceIndicatorVisible`,
`updateInputState()`, `getNewlyPressedButtons()` and `getJoystickDelta()` with no stub. Same class as
everything else in this document; not fixed here.

### Remaining

- **STEP 15** (low tail, optional): time-anchor partial-line tracking; ESP-SR clearing the live
  MultiNet table *before* a reload that may fail; migration-restore silently dropping just-entered
  WiFi credentials on an unparseable local settings.json.
- **Needs hardware:** STEPS 2, 3, 4, 5, 7, 8.
- **Site 4 (`mesh_peers.json`) still deferred** pending the peer-registration question.

### STEP 15 landed — all nine blind-safe steps complete

- **15a — time anchors.** `registryLoad()` sets `gAnchorCount = 0; gRegistryLoaded = true;` *before*
  the open, same shape as `loadIpBans`. The total-open-failure case is safe by accident, but a
  **truncated final line** — the dominant corruption mode for an append-only file — leaves a partial
  table that `registryPrune()` → `registryPersist()` commits. Added `gRegistryPartial`, set when a
  non-empty line fails `sscanf`, and the persist is skipped when it is set.
- **15b — ESP-SR.** `esp_mn_commands_clear()` runs *before* `loadCommandsFileLocked()`, so a failed
  reload leaves the **live** MultiNet table empty — and `sr cmds save` would serialise that over the
  commands file. The clear cannot simply move (the load appends), so added `gMnTableTrusted`,
  cleared at the start of a reload and set only on `ok && parseErrors == 0`, with
  `cmd_sr_cmds_save` refusing while it is false.
- **15c — migration restore.** An unparseable local `settings.json` left `haveDeviceWifi == false`
  and the restore proceeded, keeping the **source** device's `wifiNetworks` — encrypted with the
  source device's key, so useless here — silently discarding the credentials the user had just
  entered. Now aborts. *(First attempt used `sendJsonError(req, ...)`; the enclosing function is
  `static bool writeRestoreFilesFromDoc(...)` with no `req` in scope. Corrected to return false, and
  the caller already reports "Restore failed: no files were written.")*

### Verification: three configurations, because one proves nothing

| config | image | covers |
|---|---|---|
| minimal serial-only | 1,086,624 | time anchors, capture key, user settings, notifications, debug.json, filesystem repair |
| + WiFi/HTTP/ESP-NOW/Maps/I2C/OLED/input | 3,661,920 | `System_Maps.cpp` (STEP 13), migration tool (15c), the HTTP branches |
| + ESP-SR + mic | 5,034,960 | `System_ESPSR.cpp` (15b) |

`System_Maps.cpp`, `WebServer_MigrationTool.cpp` and `System_ESPSR.cpp` are each compiled out of the
minimal build, so STEPS 13, 15c and 15b would have had **zero coverage** without the second and
third builds.

### Two more pre-existing config breaks found while building those

Neither introduced here; both are the same class as everything else in this document.

1. **`I2C_FEATURE_LEVEL` > 0 with `INPUT_DEVICE_TYPE 0` does not link.** `OLED_Utils.cpp`,
   `OLED_UI.cpp`, `OLED_SettingsEditor.cpp` and `OLED_FirstTimeSetup.cpp` reference `gNavEvents`,
   `gDataSource`, `gDataSourceIndicatorVisible`, `updateInputState()`, `getNewlyPressedButtons()`
   and `getJoystickDelta()` with no stub.
2. **`ENABLE_ESP_SR 1` with `ENABLE_MICROPHONE 0` does not compile.** `System_ESPSR.cpp:2173-2204`
   uses `audioGetSource()` and `AUDIO_SRC_G2_LEFT` ungated. ESP-SR needs an audio source, and
   nothing enforces it.

---

## 28. `mesh_peers.json` — RESOLVED: persistence deleted (2026-08-23)

**The deferral in §27 was caused by a comment that is factually wrong.**
`System_ESPNow.cpp:14039` said the hardware peer table *"may lag behind after a reboot until
`loadMeshPeers()` re-registers all peers"*. It does not. Verified:

- **Init order is `loadEspNowDevices()` → `restoreEspNowPeers()` → `loadMeshPeers()`.** Radio
  registration is already complete before `mesh_peers.json` is even opened.
- `loadMeshPeers()` made **no radio call**. Per MAC it called only
  `getMeshPeerHealth(mac, /*createIfMissing=*/true)` and then zeroed every metric — its own comment
  said *"will rebuild from heartbeats"*.
- `getMeshPeerHealth()` is memset + memcpy MAC + `isActive = true` + meta slot. No radio call.
- The hardware peer table is written in exactly three places, and the one that matters —
  `addEspNowPeerWithEncryption()`, reached from `restoreEspNowPeers()` — is fed from
  **`devices.json`**, never from `mesh_peers.json`.

The comment now names `restoreEspNowPeers()`.

### The refutation found one real cost, and it is repaired

Three of four independent refutation lenses landed on the same mechanism: **the boot notification
fans out over `gMeshPeers` only**, and at that instant `loadMeshPeers()` was the only thing that had
populated it.

```
initEspNow():
  restoreEspNowPeers();          // devices.json -> radio peer table
  loadMeshPeers();               // the ONLY writer of gMeshPeers in this window
  ...
  meshSendBootToPeers(bootMsg);  // fans out over gMeshPeers ONLY, no devices[] fallback
  startEspNowTask();             // the devices[] bootstrap lives here — runs LATER
```

`buildBootNotification` has exactly one call site and is never retried, and receivers file it as a
durable `MSG_SYSTEM_EVENT` inbox entry carrying the boot counter and epoch timestamp. Deleting the
persistence with no replacement would have sent every boot notice to zero peers, on every boot,
permanently.

**Repaired by seeding from the registry instead.** The bootstrap loop already inside
`processMeshHeartbeats()` — whose own comment names the case, *"bootstrap case: after reboot,
gMeshPeers is empty"* — was extracted into `meshBootstrapSlotsFromRegistry()` and is now called from
**both** the heartbeat tick and the boot path, replacing `loadMeshPeers()` at the same position.
Sourced from `devices.json`, this reaches a **superset** of the old recipients: `mesh_peers.json` was
written from that same registry, so it could only ever be a stale subset of it.

### Why delete rather than gate-and-keep

Gate-and-keep (~10 lines) would have fixed one defect of three:

1. `saveMeshPeers()` wrote with a bare `"w"` truncate — no tmp+rename — so it destroyed the
   destination at `fopen`, and returned a value that said nothing about whether bytes landed.
2. The `peerName` it wrote was unescaped and peer-supplied, so a name containing the literal
   `"mac":` could inject a phantom entry into the hand-scanning parser.
3. **The wipe here was live, not latent.** `saveEspNowDevices` skips self, so `alreadyRegistered` at
   the own-device re-registration was *always* false, so `saveMeshPeers()` ran on **every boot** with
   a device name set — 52 lines after the load. A corrupt file was destroyed the same boot.

Deleting the sink and the writer together removes all three. And 5 of the 9 `saveMeshPeers()` call
sites were already no-ops for their stated intent — the device name and the mode are both persisted
by `setSetting()` into `settings.json` a few lines earlier.

### What was removed

`saveMeshPeers()`, `loadMeshPeers()`, `MESH_PEERS_FILE`, `cmd_espnow_meshsave` and its registry row,
the `System_ESPNow.h` declaration, all 9 call sites, and the `ESPNOW_MESH_PEERS` backup entry in
`WebServer_MigrationTool.cpp` (which had to go — `addFileToBackup` pushes a "not found, skipped"
warning for a missing file, so leaving it would make every future backup emit a spurious warning).
The `parseMacAddress` forward declaration directly above `loadMeshPeers` was **kept**: it has no
header declaration and five callers precede its definition.

A tree-wide grep for `mesh_peers` / `MESH_PEERS` / `saveMeshPeers` / `loadMeshPeers` / `meshsave`
now returns only the explanatory comments.

**Build:** green with ESP-NOW on (the minimal build compiles `System_ESPNow.cpp` to empty, so it
proves nothing here). **Flash: ~2.3 KB estimated, NOT measured** — `saveMeshPeers` was 1,106 B of
`.text` by `nm -S`; the loader is `static` and inlined so it carries no symbol. A clean figure would
need a same-config before/after, which is not worth a build cycle for 2 KB.

### Residual costs, all transient

- First-message latency to each peer after a reboot grows by one handshake — the boot fan-out is
  also the only eager KEY_EX kick, and the same seed repairs it.
- A MAC known to `mesh_peers.json` but absent from `devices.json` (a peer the *far end* paired with)
  loses its slot until the first inbound heartbeat, ≤5 s.
- `PEERSKNOWN` reads 0 instead of N for ≤10 ms at boot — and by its own definition
  ("recently-seen") 0 is the correct answer, since restored slots had zeroed timestamps anyway.
- Repopulation is **≤5 s unconditionally with zero traffic**, and the block is not
  `meshEnabled()`-gated, so it works in DIRECT mode too.

### Hardware checks (not yet run)

1. **Boot notice survives** — reboot node A, confirm node B receives a `MSG_SYSTEM_EVENT` naming A
   with the right boot counter and a non-zero timestamp. This is the one thing the deletion breaks
   and the seed repairs; check it first, then reboot three more times to confirm the counter
   increments.
2. **Mesh comes up with no traffic** — reboot A with B idle; within ~5 s each should list the other.
3. **Same in DIRECT mode** — the bootstrap is not `meshEnabled()`-gated; verify that on a radio.
