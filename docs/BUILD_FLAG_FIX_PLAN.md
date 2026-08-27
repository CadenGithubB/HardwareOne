# Build-Flag Fix Plan — work chunks by codebase section

Companion to `docs/BUILD_FLAG_COVERAGE_AUDIT.md` (the evidence: 74 verified findings, file:line refs, verifier notes). This doc re-cuts those findings into **independently implementable chunks**, grouped by the section of the codebase they touch. Each chunk has a definition-of-done ("Deliverable"), the audit findings it closes, payoff, and effort (S <1 h, M = one file/many sites, L = cross-file or needs a design call).

Priorities: **P0** = fixes a broken documented config · **P1** = reclaims flash in today's XIAO binary · **P2** = reclaims flash in variant builds (FeatherS3, WiFi-only, slim) · **P3** = hygiene/compile-time/design decision.

## Chunk index

| Chunk | Codebase section | Prio | Effort | Payoff |
|---|---|---|---|---|
| A1 | CMakeLists.txt (migration tool) | P0 | S | headless-recovery config links |
| F1 | System_WiFi.cpp + System_Utils.cpp (net core) | P0 | S | WiFi-only / NET=0 configs build |
| H1 | G2_Page_Automations.cpp + G2_Glasses.cpp | P0 | S | BT+G2/AUTOMATION=0 links; ~5-7 KB variant |
| G2 | OLED_SetupWizard.cpp (polarity bug) | P0 | S | ESPNOW=0 OLED wizard renders correctly |
| E1 | System_Utils.h (CommandEntry voice columns) | P1 | M | **~9-11 KB today** |
| I2 | System_SensorLogging.cpp (formatters) | P1 | M | ~6 KB today |
| B1 | System_Settings.cpp (editor-command table) | P1 | M | ~4.5 KB today, ~6-7 KB all configs |
| I1 | System_SensorLogging.cpp (health commands) | P1 | S | ~3.5-4 KB today |
| D1 | System_Debug.cpp (+DebugFlags rows) | P1 | M | ~2.5-3 KB today, +~3 KB variants |
| G1 | System_SetupWizard.cpp + OLED_SetupWizard.cpp | P1 | M | ~2.5 KB today, +~5-7 KB variants |
| K1 | System_Icons.cpp | P1 | S | ~2.2 KB today, +~3 KB variants |
| M2 | System_NeoPixel.cpp (new flag) | P1 | S | ~1.5-2 KB today |
| J1 | System_I2C.cpp (bus cmds + DB rows) | P1 | M | ~2.9 KB today |
| M1 | System_Battery.cpp (battery log) | P1 | S | ~1.2-1.5 KB today |
| L1 | System_Events.h + System_Notifications.cpp | P1 | M | ~1-1.5 KB today, +~1.5 KB variants |
| C1 | System_Settings.h (gSettings fields) | P1 | L | ~0.7 KB DRAM + ~1 KB flash today |
| B2 | System_Settings.cpp (DBG_ROW MQTT) | P1 | S | ~0.5 KB today |
| N1 | Web layer (WebServer_*/WebPage_*) | P1/P3 | M | ~1 KB today + big compile-time wins |
| H2 | G2_Page_ESPNow.cpp + G2_Glasses.cpp | P2 | M | ~8-12 KB in ESPNOW=0 G2 builds |
| H3 | G2_Page_Network.cpp + System_Camera_DVP.cpp | P1/P2 | S | small; kills 2 phantom UI/settings |
| E2 | System_Utils.cpp / System_Command.cpp helpers | P2/P3 | S | latent link break removed |
| O1 | Whole-file wraps (4 files) | P3 | S | compile time; structural safety |
| J2 | System_I2C*.cpp whole-file wraps | P3 | S | compile time at I2C level 0 |
| M3 | i2csensor_max17048.cpp gate | P3 | S | correctness of force-disable |
| P1 | sdkconfig defaults | P1 | S | prevents ~150 KB Bluedroid re-link |
| Q | Decisions & deferred | P3 | — | see section |

Suggested batches (respecting the finish-then-HW-test workflow): **Batch 1** = A1+F1+H1+G2 (all P0, small, verifiable by builds alone). **Batch 2** = E1+I1+I2+B1+B2 (the big today-binary wins, all in 3 files). **Batch 3** = D1+G1+K1+M1+M2+J1+L1+H3. **Batch 4** = C1 (needs the keep-list care). **Batch 5** = N1+H2+E2+O1+J2+M3 + P1.

---

## Section A — Build system (`CMakeLists.txt`)

### A1 — Migration-tool CMake gate — P0, S
Closes: wifi #1.
- Add an `ENABLE_MIGRATION_TOOL` regex-grep (same pattern as the other flags; remember the plain-literal rule in the header) and include `WebServer_MigrationTool.cpp` when **HTTP or MIGRATION** is on. The file already self-gates internally.
- **Deliverable:** an `ENABLE_HTTP_SERVER=0 + ENABLE_MIGRATION_TOOL=1` build links and serves the FTS restore server, as `System_BuildConfig.h:~535-563` promises. Verify with build V4.

## Section F — Network core (`System_WiFi.cpp`, `System_Utils.cpp`)

### F1 — Un-break WiFi-only and NET=0 configs — P0, S
Closes: espnow #0, wifi #0.
- `System_WiFi.cpp` 297-298, 429-430, 459-460, 893-894, 1660-1670, 1726-1728: wrap the `isEspNowInitialized()` / `espnowNoteWifiChannelMayHaveChanged()` calls in `#if ENABLE_ESPNOW` (the power-save block at 479-539 in the same file is the model), or add inline no-op stubs in a header.
- `System_Utils.cpp:1643-1654`: wrap the `WiFi.RSSI()/channel()/macAddress()/localIP().toString()` block in `#if ENABLE_WIFI` with an `#else` emitting empty/zero JSON fields (or extend the stub `WiFiClass`).
- **Deliverable:** builds V2 (WIFI_HTTP) and V3 (NET=0) compile and link clean.

## Section B — Settings command surface (`System_Settings.cpp`)

### B1 — Gate the settings-editor command table — P1, M
Closes: reg #0/#1/#2 (=i2c #1), apps #2/#3 (=reg #3), reg #4, espnow #4, ble #4, wifi #6; plus the `kStops[]` rows.
One file, one pattern, ~30 commands. Wrap each `SETTING_EDITOR_CMD` + its `settingEditorCommands[]` row + any matching `kStops[]` entry in the same `#if` its SettingsModule registration already uses:
- 14 sensor/input/display commands (`thermalenabled`, `tofenabled`, `imuenabled`, `gpsenabled`, `fmradioenabled`, `apdsenabled`, `rtcenabled`, `presenceenabled`, `inputenabled`, `tofi2cclockhz`, `presencedevicepollms`, `apdsdevicepollms`, `fmradiodevicepollms`, `gpsdevicepollms`) — per-sensor flags
- 5 EI (`eirequirelabels`, `eimaxdetections`, `eiinputsize`, `eiinterval`, `eiautostart`) — `ENABLE_EDGE_IMPULSE`
- 3 SR (`srenabled`, `srautostart`, `srmodelsource`) — `ENABLE_ESP_SR`
- 3 ESP-NOW (`espnowcapturetosd`, `espnowcaptureskipheartbeats`, `espnowautostart`) — `ENABLE_ESPNOW`
- `llmenabled` (+ kStops `{"llmenabled","llmunload"}`), `automationautostart`, `oledautostart`, `oledclihistorysize` (+ the `oledHistorySize` SettingEntry in `System_Command.cpp:531-544`), `bleenabled` (+ kStops `{"bleenabled","g2deinit"}` + `closeble` case), `webclihistorysize` + `webConsole` debug row — their respective flags
- Also gate the remaining `kStops` rows for gated-off features (`System_Settings.cpp:2880-2899`: camera/mic/sr/wifi/http rows)
- **Deliverable:** in any build, `settingsedit` lists only commands whose backing setting can resolve; zero "registered but always errors" commands remain. ~4.5 KB today, ~6-7 KB summed across variants.

### B2 — MQTT debug settings rows — P1, S
Closes: wifi #5.
- Gate the 5 `DBG_ROW(MQTT*)` rows (2000-2004) + `debugMqtt*` bools (`System_Settings.h:92-96, 537-541`) with `#if ENABLE_MQTT`. **Caveat from the verifier:** `DBG_ROW` picks from the X-macro-generated `kDbgFlagRegEntry[]` and the row count is pinned by `kDbgRegGeneratedRows` (=159) — the fix must adjust that accounting or it trips the static_assert.
- **Deliverable:** MQTT=0 build has no `debugmqtt*` settings; static_asserts still pass.

## Section C — Settings data (`System_Settings.h`)

### C1 — Gate dead gSettings fields (with keep-lists) — P1, L
Closes: reg #6/#7/#8/#9 (= wifi #4, cam #6/#7/#8, i2c #4, ble #3, espnow #6).
Mirror the existing `ENABLE_BONDED_MODE` / `ENABLE_AUTOMATION` / `ENABLE_ONDEVICE_LLM` blocks for: MQTT (~21 of 23 fields), SR tuning (5 fields + 6 debug bools), EI tuning, OLED (~9 of 13), per-sensor tuning (~40), camera tuning (~24), espnow/mesh block (incl. `MeshIdentity meshes[4]`), BT/G2/R1 block.
**Hard constraint (verifier-established):** these fields are read from always-compiled code and MUST stay unconditional unless their readers are gated too: every feature's `*Enabled`/`*AutoStart` pair (FeatureRegistry rows + `System_RamFlush` map), `localDisplayRequireAuth`, `oledCliHistorySize` (written by the live command until B1 lands), `SelfDevice::deviceName` fallback needs a small `#else` for `espnowDeviceName`.
- **Deliverable:** each gated build boots clean, `settings json` round-trips, and `features json` still reports `compiled:false` rows. ~0.6-0.7 KB DRAM + ~1 KB ctor flash today; more in variants.

## Section D — Debug command surface (`System_Debug.cpp`, `System_DebugFlags.h`)

### D1 — Gate feature debug-command rows — P1, M
Closes: ble #1, cam #1/#4/#9(commands), apps #4.
The table already gates LLM and G2 rows — extend the same treatment: `debugbluetooth*` ×4 + `outble` (BT), `debugsr*` ×6 (SR), `debugcamera*` ×5 (camera), `debugmaps*` ×4 (MAPS), `debugautomations` (AUTOMATION), `debugmic*` ×4 (MICROPHONE). Adjust `kDbgGenCmdRowsInTable` / `kDbgHandCmdRowsInTable` (2216-2236) — they currently encode the retention.
- **Deliverable:** `debug` module lists only commands for compiled-in features; static_asserts pass in all flag combinations. ~2.5-3 KB today, ~3 KB more in variants. (The underlying X-macro flag *banks* stay — that's chunk Q1's decision.)

## Section E — Command core (`System_Utils.h/.cpp`, `System_Command.cpp`)

### E1 — Voice-routing columns out of CommandEntry — P1, M — **biggest single win**
Closes: cam #0.
- Wrap `voiceCategory/voiceSubCategory/voiceTarget` (`System_Utils.h:62-64`) in `#if ENABLE_ESP_SR`; give the constructors accept-and-drop overloads for the voice args when SR is off so **no command-table rows need editing** (ctors already default them).
- **Deliverable:** SR-off builds shed 3 pointers × ~800+ live rows ≈ **9-11 KB**; SR-on builds unchanged; `srcmdssync` still works on an SR build.

### E2 — Shared-helper cleanups — P2/P3, S
Closes: espnow #9, wifi #8, ble #8.
- Move `parseMacAddress` from `System_ESPNow.cpp:8450` into a shared TU (it's core arg-parsing used by `CommandArgs::argMac`) — removes the latent ESPNOW=0 link break.
- Wrap `executeUnifiedWebCommand` (`System_Utils.cpp:5150-5164`) in `#if ENABLE_HTTP_SERVER`; gate the 3 BT quiet-poll strings + `blesecret` redaction row.
- **Deliverable:** no core-path symbol depends on a feature-gated TU; grep proves it.

## Section G — Setup wizard (`System_SetupWizard.cpp`, `OLED_SetupWizard.cpp`)

### G2 — Polarity bug — P0, S
Closes: espnow #3. Change `#ifndef ENABLE_ESPNOW` → `#if !ENABLE_ESPNOW` at `OLED_SetupWizard.cpp:384`.
- **Deliverable:** ESPNOW=0 OLED build renders the System page 'Name:' row; item list and rows in sync.

### G1 — Compile-gate wizard feature pages — P1, M
Closes: wifi #2/#3, espnow #2.
- Wrap `handleSerialMQTTPage` + its `printSerialPageStatus` case + dispatch (1069-1073, 1192-1278, 1538-1549) in `#if ENABLE_MQTT`; same for `handleOLEDMQTTPage` (OLED file, 810-857) + prototype.
- Wrap `handleSerialESPNowPage` / `handleOLEDESPNowPage` + `WIZARD_PAGE_ESPNOW` in `kPageOrder` + header case + dispatch (177, 1064-1067, 1087-1280, 1525-1534; OLED 694-798) in `#if ENABLE_ESPNOW`. Keep the `wizardShouldShow*()` runtime checks as belt-and-suspenders.
- **Deliverable:** wizard page list is compile-time correct; MQTT=0 build sheds ~2.5 KB today (serial page), OLED/ESPNOW variants shed the rest.

## Section H — G2 lens surfaces

### H1 — G2 Automations page needs ENABLE_AUTOMATION — P0, S
Closes: apps #1 (Tier-0 #4).
- `G2_Page_Automations.cpp:17`: extend the wrap to `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_AUTOMATION`; guard the Apps launcher row (`G2_Glasses.cpp:4751`), dispatch case (~4879), module struct (4647), and `g2RegisterPage` call (6974) the same way — mirror the adjacent `APP_ROW_LLM` pattern.
- **Deliverable:** build V5 (BT+G2, AUTOMATION=0) links; Apps menu shows no Automations row.

### H2 — G2 ESP-NOW app needs ENABLE_ESPNOW — P2, M
Closes: espnow #1.
- Add the `ENABLE_ESPNOW` term to the file wrap (or demote the fallback shell to a few lines), and guard `add("ESP-NOW", APP_ROW_ESPNOW)` (4739) + `g2RegisterPage(kEspNowAppPage)` (6973) — siblings Maps/LLM/Health are the model.
- **Deliverable:** BT+G2/ESPNOW=0 build sheds ~8-12 KB and shows no ESP-NOW app row.

### H3 — Cross-feature stragglers — P1/P2, S
Closes: wifi #7, ble #7.
- `G2_Page_Network.cpp` 1867-1869/1902-1908/1938-1955: wrap the HTTPS toggle row + tap branch in `#if ENABLE_HTTPS`.
- `System_Camera_DVP.cpp:2294-2296`: wrap `g2StreamToneMap`/`g2PackRateMs` rows in `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` (in today's camera-on/BT-off build these G2 settings currently persist — this one is P1).
- **Deliverable:** no lens row or setting fronts a feature that isn't compiled in. House rule to add to AGENTS.md/plan docs: *a UI row must AND-in every feature it dispatches to.*

## Section I — Sensor logging (`System_SensorLogging.cpp`)

### I1 — Health commands under ENABLE_R1_HEALTH — P1, S
Closes: ble #0.
- Wrap the `healthlogging`/`healthstatus`/`healthlogmerge` rows (2468-2490) and the full `cmd_healthlogmerge` (2209-2325) in `#if ENABLE_R1_HEALTH` (drop the compiled-in "requires ENABLE_R1_HEALTH" stub pattern, or keep one tiny row).
- **Deliverable:** today's BT=0 binary sheds ~3.5-4 KB; `sensorlog` module lists no health commands.

### I2 — Per-sensor formatters mirror the capture guards — P1, M
Closes: i2c #2.
- The snapshot-capture section (685-825) is already per-sensor gated — apply the same `#if`s to the CSV header builders (164-190), text/CSV row formatters (395-684), mask checkbox UI and mask-name parsing (~1420-1500); stop `sensorlog all` setting mask bits for non-compiled sensors.
- **Deliverable:** ~6 KB today; CSV headers only contain columns a sensor in this build can produce.

## Section J — I2C core (`System_I2C.cpp`, `System_I2C_Manager.cpp`)

### J1 — Bus commands + sensor DB rows — P1, M
Closes: i2c #3, i2c #6.
- Gate each `*bus` command handler (810-821) + table entry (2270-2282) with its sensor's flag — **or** explicitly document the set-bus-before-reflash workflow and keep them (decision; the audit flags the inconsistency either way: MAX17048's DB row is guarded while `fuelgaugebus` isn't).
- `i2cSensors[]` (216-248): either per-row `#if`s, or keep the whole table deliberately (detect names unknown chips) and drop the lone `BATTERY_BACKEND_FUEL_GAUGE` row guard for consistency.
- **Deliverable:** one consistent, documented policy; ~2.9 KB today if gated.

### J2 — Whole-file wraps for level 0 — P3, S
Closes: i2c #7. Add `#if ENABLE_I2C_SYSTEM` after the includes of both files (4,133 lines currently compile at level 0 and rely on GC).
- **Deliverable:** build V6 (I2C level 0) compiles both files to empty TUs.

## Section K — Icons (`System_Icons.cpp`)

### K1 — Per-feature icon gates — P1, S
Closes: cam #2/#3/#5, cam #9 (icons).
- Wrap each icon's PNG + bitmap + `EMBEDDED_ICONS[]` row: espsr (7160-7237, 7618), edgeimpulse (7069-7140, 7617), camera (6499-6580, 7610), microphone+mic pair (6582-6706, 7611-7612) in their flags.
- **Deliverable:** ~2.2 KB today (espsr+EI), ~1.1-1.8 KB more on camera/mic-less variants; `/api/icon` and OLED fall back gracefully for absent names.

## Section L — Events & notifications (`System_Events.h`, `System_Notifications.cpp`)

### L1 — Per-family SYSEVT segments + notification cases — P1, M
Closes: reg #5, espnow #7, ble #2/#6, reg #11.
- Split `SYSEVT_KIND_LIST` into per-family segments included under their flags (names are the persisted form — safe under no-backwards-compat). Gate the matching notification defaults/verbosity/format-string cases, the `NSINK_G2` mask plumbing, `notifG2` row + `notifydeviceg2` command, and the `g2Pushed/Filtered/Dropped` counters.
- **Deliverable:** `events kinds` lists only kinds this build can emit; no persisted notification setting for a sink that can't exist. ~40+ dead kinds drop from today's binary (~1-1.5 KB).

## Section M — Hardware misc (`System_Battery.cpp`, `System_NeoPixel.cpp`, `i2csensor_max17048.cpp`)

### M1 — Battery-log block — P1, S
Closes: i2c #0 (as adjusted by the verifier).
- Wrap 628-811 in `#if ENABLE_BATTERY_MONITOR` with no-op `batteryLogTick/batteryLogEvent` stubs in `#else`; guard the `batteryLogSettingsModule` registration (`System_Settings.cpp:2426`). The live paths to kill are the unguarded event calls in `System_Utils.cpp:1389/1455/1466/1502` and `System_Power.cpp:139`.
- **Deliverable:** monitor-less builds write no fake `100%/USB Power` rows and shed ~1.2-1.5 KB.

### M2 — Introduce ENABLE_NEOPIXEL — P1, S
Closes: i2c #11 (flag gap).
- New flag defaulting to `(NEOPIXEL_PIN_DEFAULT >= 0)`; gate `neopixelCommands[]` (505-513), the `neopixel` gCommandModules row (`System_Utils.cpp:2884-2892`), the color table, and the dummy `Adafruit_NeoPixel(0,-1)` object.
- **Deliverable:** pin=-1 boards (today's XIAO) shed ~1.5-2 KB and `ledcolor/ledeffect/ledclear` no longer register as no-ops.

### M3 — Fuel-gauge gate composition — P3, S
Closes: i2c #9. `#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE` in both files.
- **Deliverable:** force-disabling the monitor on a FeatherS3 stops compiling the driver.

## Section N — Web layer (`WebServer_*`, `WebPage_*`)

### N1 — Web-content residue — P1/P3, M
Closes: apps #5/#6/#7/#8/#9, i2c #10.
- Guard the `WebPage_Automations.h` include + `streamAutomationsContent` in `WebServer_Server.cpp` with `#if ENABLE_AUTOMATION` (matches `HardwareOne.cpp:61-63`) — ~50-70 KB of compile-then-GC per AUTOMATION=0 build (compile time).
- Wrap the dashboard `if(c.llm){...}` JS chunk (`WebPage_Dashboard.h:432-439`) in the card's existing `#if`.
- Per-feature groups in the guest allowlist `kAllow[]` (`WebServer_Utils.cpp:383-400`).
- Wrap the `/system/llm/` FS permission row (`System_Filesystem.cpp:1361-1365`).
- Convert `/api/sensors` early-return anti-pattern to real `#if/#else/#endif` for thermal/tof/imu/input (`WebPage_Sensors.cpp:101-229`).
- Optional/low: per-app label strings in `WebPage_Settings.h`.
- **Deliverable:** ~1 KB today + the fragility of the early-return pattern removed; AUTOMATION=0 builds stop compiling a 108 KB source blob.

## Section O — Whole-file hygiene wraps — P3, S

Closes: ble #9, espnow #8, cam #10, i2c #8.
- `BLE_CentralTx.cpp` → `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`; `System_ESPNow_Tx.cpp` → `#if ENABLE_ESPNOW` (matches its 11 siblings); `System_ImageManager.cpp` → `#if ENABLE_CAMERA_SENSOR` (or CMake camera group; kills the ~50 B `.init_array` residual too); `HAL_Input.cpp` core → move the `#endif` at line 18 down to cover 20-238.
- **Deliverable:** exclusion is structural, not an accident of caller guards; ~20 KB less object code compiled per gated-off build.

## Section P — sdkconfig / IDF configs

### P1 — Defaults desync — P1, S (config edit, not code)
- `sdkconfig.defaults.esp32s3:44` still has `CONFIG_BT_ENABLED=y` while the app flag is 0 — a fullclean re-links ~150 KB of Bluedroid. Align it with the header's promise that the two are kept in sync (respecting the board-switching workflow in `docs/BOARD_SWITCHING.md`).
- With SR off, `CONFIG_USE_AFE/WAKENET/MULTINET`, `CONFIG_SR_WN_WN9_HILEXIN`, `CONFIG_MODEL_IN_FLASH` still build the esp-sr component + model packing every time (build-time only; `srmodels.bin` isn't flashed).
- **Deliverable:** a fullclean XIAO build contains no Bluedroid and skips esp-sr model packing.

## Section Q — Decisions & deferred (no default action)

- **Q1 — Debug-flag X-macro segmentation** (ble #5): the 117-row flag registry + ~156 debug bools are sanctioned unconditional design (`docs/DEBUG_FLAG_XMACRO_PLAN.md` §3). Segmenting per family would save ~2-3 KB in slim builds but is a design change to the drift=build-error invariant. Decide once; D1 (command rows) is safe either way.
- **Q2 — FeatureRegistry contract**: rows deliberately unconditional so `features json` reports `compiled:false`. Keep (C1's keep-lists exist because of it) or redesign; touching it changes C1's scope.
- **Q3 — `even_r1` scope**: the whole ring command module registers under BT&&G2; `ENABLE_R1_HEALTH` gates only health pages. Decide whether a BT+G2/R1=0 build should carry `ring*` commands.
- **Q4 — Flag gaps** (opportunities, not defects): `System_UartLink.cpp` (~8 KB + task, board-pin-gated only), `led` module, `System_PollPause`, I2C dual-bus init (~1-2 KB always-live), capturecrypt registered when R1=0.
- **Q5 — Autolog globals** (apps #0): hygiene move of `gAutomationLog*` definitions to an always-compiled TU; no shipped bytes today.

---

## Verification matrix

Each chunk names one of these; collectively they exercise every gate. (Per the house workflow: finish a batch → user HW-tests → then commit.)

| Build | Config | Proves |
|---|---|---|
| V1 | Today's XIAO (BT=0, sensors 0, SR/EI/LLM/MQTT/GAMES 0) | baseline; size deltas for every P1 chunk (compare `idf.py size` before/after) |
| V2 | `NETWORK_FEATURE_LEVEL=2` (WiFi+HTTP, ESPNOW off) | F1 link fix; espnow-family gating |
| V3 | `NETWORK_FEATURE_LEVEL=0` | F1 compile fix; WiFi-family gating |
| V4 | HTTP=0 + `ENABLE_MIGRATION_TOOL=1` | A1 |
| V5 | FeatherS3: BT=1, G2=1, AUTOMATION=0, camera=0 | H1 link fix; D1/K1 camera rows; Tier-2 BLE items stay |
| V6 | `I2C_FEATURE_LEVEL=0`, `DISPLAY_TYPE=0` | J2; sensor-family gating |

Size accounting: `idf.py size-components` + `.map` diff per batch beats the audit's ~25 B/line heuristics — expect the real numbers to differ by up to 2× per item, ~36 KB ±30% in aggregate for V1.
