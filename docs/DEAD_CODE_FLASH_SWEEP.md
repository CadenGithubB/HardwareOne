# Dead-code / flash sweep — investigation report (2026-08-08)

**Scope:** whole `components/hardwareone` firmware, hunting code that costs
**flash / program space** (not RAM) but is never really used. Investigation
only — nothing edited. Produced by a 7-analyzer sweep against tonight's
**carrier-build ELF** (`build/hardwareone-idf.elf`: BT on; LLM, maps, OLED,
I2C-sensors compiled OUT; camera + G2 + LC3 + web-UI in).

## The headline (read this first)

**This firmware is already very flash-clean, and the obvious strategy —
"delete functions that are never called" — recovers almost nothing.** The
build runs `-ffunction-sections -fdata-sections` + linker `--gc-sections`,
which already discarded **1.32 MB** of `.text` (+139 KB literal, +249 KB
rodata). Crucially, gc-sections drops uncalled functions **including
global-linkage ones in the archive build** — verified: every classic
"defined-but-never-called" symbol (e.g. `sendChunkedResponse`, `v4_send_text`,
the 8 dead `g2Build*` protocol encoders) is **absent from the final ELF and
costs 0 bytes.** Deleting those is source hygiene, not a flash win.

Disabled features are also cleanly gated: OLED/maps/LLM/I2C-sensors and the
disabled web pages (Speech/Bond/MQTT/Games/Maps) contribute **exactly 0 kept
bytes**. The biggest kept symbols are all *legitimately used* — libsodium
Ed25519/blake2b (OTA signature verify, ~90 KB), embedded web-UI HTML/JS
(~1.06 MB, all live), newlib/lwip/wifi, and live app code.

So the real, kept-in-flash dead weight is **narrow and specific**: dev/test
scaffolding shipped in production, plus two vendored-library over-inclusions.

## Realistically recoverable flash

| Tier | What | ~Flash | Where the cut is |
|---|---|---|---|
| **1 — clean, in OUR code** | G2 Test-suite page + image-probe cluster | **~47 KB** | delete the hidden "Tests" page (transitively gc's the probes) |
| 1 | G2 diagnostic CLI (`g2nativeconfig`/`g2devcfg`/`g2protostats`/`g2probe`) + `g2ProtocolGoldenSelfTest` | ~5 KB | drop the dev commands from `g2Commands[]` |
| 1 | ESP-NOW dev/test CLI (`teststreams`/`testconcurrent`/`testcleanup`) | ~2 KB | drop from `espNowCommands[]` |
| **2 — vendored library** | LC3 MDCT window tables (all sample-rate/duration configs; only 16 kHz/10 ms used) | **~38 KB** | edit vendored `liblc3/src/tables.c` |
| 2 | libsodium Argon2/pwhash (pulled by `sodium_init`, never called — this fw uses PBKDF2/mbedtls) | **~13.7 KB** | libsodium build config / exclude |
| **3 — config decision** | Camera (`System_Camera_DVP.cpp`) | ~57.7 KB | only if the carrier is headless (you chose to keep it) |
| **4 — source hygiene** | ~7 dead functions already gc'd | **0 KB** | delete for cleanliness, no flash change |

**Net without touching the camera or vendored libs: ~54 KB, cleanly in our
own code.** Add the two vendored-lib edits for ~+52 KB. The camera is
~57.7 KB more if you ever go headless.

## Tier 1 — the clean wins (detail, all present in every `ENABLE_G2_GLASSES` build)

### G2 Test-suite + image probes — ~47 KB (the standout)
- `G2_Page_TestSuite.cpp` whole TU: **~23–37.5 KB** kept flash
  (`g2TestSuiteHandleTap` alone ~8.6 KB; `runSelectionPattern` 3.2 KB;
  `kSourceText` 1.9 KB lorem-ipsum blob; `gAnimIconPickPaths` 768 B).
- `g2ProbeImage*` cluster in `G2_Glasses.cpp:18940+`: **~25.5 KB** (30+
  `const char* g2ProbeImage…` impls). **Reachable ONLY through the TestSuite
  dispatch** (`spawnImgProbeWorker`), so removing the Tests page transitively
  gc's all of them.
- It's a *hidden* diagnostic page — registered via `g2RegisterPage(kTestSuitePage)`
  (`G2_Glasses.cpp:6992`, "hidden — reached via System"), reached at runtime
  through the G2 System menu (`SYS_ROW_TESTS`). No strip flag gates it.
- **Recommendation:** put the whole test bench behind a compile flag
  (`ENABLE_G2_TESTSUITE`, default 0) rather than deleting — it's useful for
  bring-up. Flag-off = ~47 KB back.

### G2 diagnostic CLI — ~5 KB
`cmd_g2nativeconfig` (~1.1 KB), `cmd_g2devcfg` (~954 B), `cmd_g2protostats`
(~844 B), `cmd_g2probe` (~574 B) in `G2_Glasses.cpp:16896+`, registered in
`g2Commands[]` (~:17978); `g2ProtocolGoldenSelfTest` (`System_G2_Protocol.cpp:2324`,
~1.3 KB incl. golden-vector rodata, called only by `g2nativeconfig selftest`).
Dev diagnostics; same flag could gate them.

### ESP-NOW dev/test CLI — ~2.1 KB
`cmd_test_streams` (~793 B, `System_ESPNow.cpp:13306`), `cmd_test_concurrent`
(~781 B, :13352), `cmd_test_cleanup` (~537 B, :13404) — fabricate fake
topology streams; **registered UNGATED** in `espNowCommands[]` (:16908–16910).
Their only non-definition reference is the table row. (Sibling
`cmd_test_filelock` was already removed — these three were missed.)

## Tier 2 — vendored libraries (bigger effort, touch third-party code)

- **LC3 MDCT windows, ~38 KB** (`components/hardwareone_libs/liblc3/src/tables.c`):
  the `lc3_mdct_win[]` pointer table keeps every sample-rate/duration window,
  but G2 only ever runs a 16 kHz/10 ms decoder (`G2_Glasses.cpp:17293`), so
  only `mdct_win_10m_16k` (~1 KB) is ever dereferenced; the other ~38 KB are
  linked-but-never-read. All LC3 *encoder* code is already gc'd. Trimming =
  editing the vendored table (or a build-time config to emit only the used
  windows).
- **libsodium Argon2, ~13.7 KB** (`managed_components/espressif__libsodium/argon2-fill-block-ref.c`):
  `fill_block` (6.5 KB) + `fill_block_with_xor` (6.6 KB) + `argon2_fill_segment_ref`.
  Kept only because `sodium_init()` reaches the pwhash "pick best implementation"
  dispatcher — **zero call sites for `crypto_pwhash`/argon2** in the firmware
  (password hashing here is PBKDF2 via mbedtls). Config-**independent**: present
  in *every* build. Removable via a libsodium minimal-build config that excludes
  pwhash.

## Tier 4 — source-hygiene only (0 flash, already gc'd — do NOT expect savings)

Delete for cleanliness, but they cost nothing today: `sendChunkedResponse`
(`System_ESPNow.cpp:1038`), `v4_send_text` (:2898), `v4_broadcast_topo_request`
/`v4_send_topo_request` (:2649 dead island), `espnowCollapsedPeerMessages`
(:17564), and 8 dead G2 protocol encoders (`g2BuildRebuildImage`,
`g2BuildCreateMultiText`, `g2EncodeVarint`, … `System_G2_Protocol.cpp:924+`).
Also `gMeshTopology` (`System_ESPNow.cpp:368`) — a never-populated
`std::vector` global; costs a little BSS (RAM, not flash) because global
ctors defeat gc-sections. Two retired-opcode branches (`v4h_sensor_broadcast`
:3433, `METADATA_PUSH` :5667) are ~56 B of kept stub — negligible.

## For whoever cuts the code — method guidance

1. **Don't chase "never-called" functions for flash.** `--gc-sections` already
   dropped them (verified against the ELF). That work saves 0 bytes. It's fine
   for source cleanliness, but budget it as hygiene, not flash.
2. **The wins are reachable-but-dev-only clusters** (the G2 test bench is 90%
   of the in-our-code recoverable flash) and **vendored-library over-inclusion**
   (LC3 windows, Argon2). Prefer a compile flag over deletion for the test/diag
   scaffolding — it's genuinely useful for bring-up.
3. **The dominant flash consumer is live web-UI HTML/JS (~1.06 MB).** If you
   ever need a *large* flash reduction, that's the pool — minifying/gzipping
   embedded assets or dropping web pages you don't use dwarfs every dead-code
   item here. (Out of scope for this sweep — it's not "dead," just big.)

## Coverage caveats

- ELF analyzed is the **carrier config**. Map-derived sizes are carrier-specific;
  source-level findings are config-independent unless noted.
- Three source-reader slices (System_Icons / OLED / Maps; WebServer / Utils /
  Automation; LLM / ESPSR / User / Settings) **failed on a tool error and were
  not deep-swept** for no-op-functions-that-still-have-callers. The nm size
  analysis covered these files at the symbol level and found them
  **live-code-dominated** (their dead statics are gc'd → 0 flash), so the
  expected additional flash upside is low — but a targeted re-sweep of
  `System_Icons.cpp` (95 KB kept — worth confirming every icon array is
  actually referenced) would close the gap if desired.
