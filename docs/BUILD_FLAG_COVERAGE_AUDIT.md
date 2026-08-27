# Build-Flag Coverage Audit — 2026-08-07

> **Actionable version:** `docs/BUILD_FLAG_FIX_PLAN.md` re-cuts these findings into work chunks by codebase section, each with a deliverable, priority, and verification build. This doc is the evidence base.

**Question:** for every compile-time toggle in `System_BuildConfig.h` (Bluetooth, WiFi, web server, sensors, apps, …), is all the code serving that feature actually excluded from the binary when the toggle is 0 — or does some of it still compile in?

**Method:** 14-agent audit (7 domain finders + 7 adversarial verifiers, ~2M tokens, 708 tool calls). Every finding was independently re-verified by walking the actual preprocessor nesting and CMake gating; verifiers were instructed to refute. Result: **74 findings — 66 confirmed, 8 confirmed-with-adjustments, 0 refuted.** Read-only audit: no builds were run; link-break claims are from exhaustive symbol-resolution analysis (one was empirically compile-tested by a verifier with the real xtensa toolchain).

**Config audited** (working tree 2026-08-07, XIAO camera build): WiFi=1 HTTP=1 HTTPS=1 ESPNOW=1 MQTT=0 · **BT=0** (header has G2=1, but every use-site requires BT&&G2 so all G2 code is out; R1_HEALTH derives 0) · I2C level 4 with ALL sensors 0, DISPLAY_TYPE=0, INPUT_DEVICE_TYPE=0 · camera=1 mic=1 · ESP_SR=0 EDGE_IMPULSE=0 LLM=0 AUTOMATION=1 BONDED=1 GAMES=0 MAPS=1.

> **Config note:** `ENABLE_BLUETOOTH` was flipped 1→0 mid-session (user's board switch to the XIAO). The finder agents' briefing said BT=1; the verifiers observed BT=0 on disk. Both readings of every finding hold — the flip only changes which tier a BLE/G2/R1 leak lands in, and the tiers below are computed for the real config (BT=0). Related: the active `sdkconfig` correctly has `CONFIG_BT_ENABLED` unset, but **`sdkconfig.defaults.esp32s3:44` still carries `CONFIG_BT_ENABLED=y`** — a fullclean/regenerate would silently re-link the ~150 KB Bluedroid stack into a BT=0 app build.

## How gating works today (context)

Two layers:
1. **CMake layer** — `CMakeLists.txt` regex-greps the config header and excludes whole `.cpp` files (MQTT, LLM, Maps, Automation, EdgeImpulse, G2/R1 protocol, web server files, OLED mode files, per-input-driver files, per-game pages).
2. **Preprocessor layer** — everything else compiles unconditionally and must self-gate with `#if`. This is where all 74 leaks live.

Because ESP-IDF links with `-ffunction-sections/-fdata-sections` + `--gc-sections`, an unguarded function that is *unreferenced* when a flag is off gets dead-stripped for free. The findings that cost real flash are the **live** ones: rows in always-registered command/settings/icon/event tables, wizard dispatch, boot calls, and `gSettings` struct fields. Each finding below is classified `live` / `data` / `stripped`.

---

## Tier 0 — Build breaks in documented configurations (fix first)

These make advertised flag combinations fail to compile or link. None affect the current config; all affect configs the header itself documents.

| # | Config that breaks | Failure | Ref |
|---|---|---|---|
| 1 | WiFi=1, ESPNOW=0 (stock `NET_LEVEL_WIFI_ONLY` / `NET_LEVEL_WIFI_HTTP`) | **Link failure**: `System_WiFi.cpp` calls `isEspNowInitialized()` (5+ sites incl. `wifiRadioState()`) and `espnowNoteWifiChannelMayHaveChanged()` (inside the address-taken WiFi event callback) — both defined only inside `System_ESPNow.cpp`'s `#if ENABLE_ESPNOW` wrap, no stubs | espnow #0 |
| 2 | NETWORK_FEATURE_LEVEL=0 | **Compile failure**: `System_Utils.cpp:1643-1654` (`buildSystemInfoJson`) calls `WiFi.RSSI()/channel()/macAddress()/localIP().toString()` which the stub `WiFiClass` (`System_SensorStubs.h`) does not provide | wifi-http #0 |
| 3 | HTTP=0 + ENABLE_MIGRATION_TOOL=1 (the documented headless-recovery build, BuildConfig ~535-545) | **Link failure**: CMake never greps `ENABLE_MIGRATION_TOOL`; `WebServer_MigrationTool.cpp` is only added when HTTP is on, but `System_FirstTimeSetup.cpp` calls its restore functions under `#if ENABLE_MIGRATION_TOOL && ENABLE_WIFI` | wifi-http #1 |
| 4 | BT=1, G2=1, AUTOMATION=0 | **Link failure**: `G2_Page_Automations.cpp` (gated only on BT&&G2) externs `AUTOMATIONS_JSON_FILE`, whose sole definition (`HardwareOne.cpp:246`) is inside `#if ENABLE_AUTOMATION`; the page is live via the registered G2 page table | apps #1 |

**Plus two functional (not size) bugs found in passing:**
- `OLED_SetupWizard.cpp:384-392` — **wrong guard polarity** `#ifndef ENABLE_ESPNOW` (the macro is *always* defined, 0 or 1, so the block is dead in every config). In an ESPNOW=0 OLED build the System page's item list contains the device-name row but the renderer never draws it → selection desync. Fix: `#if !ENABLE_ESPNOW`. (espnow #3)
- `OLED_Utils.cpp:3427-3429` — the UserManager OLED mode's linker anchor sits under `#if ENABLE_ONDEVICE_LLM` (wrong flag): an OLED build with LLM=0 silently loses the UserManager mode. (ble-g2-r1 notes)

**Latent link breaks (fine today, one refactor away from breaking):**
- `System_Command.cpp:456-460` — `CommandArgs::argMac()` in the always-compiled command core calls `parseMacAddress()`, defined only inside `System_ESPNow.cpp`'s guard. Links today only because argMac's sole caller is itself ESPNOW-gated, so GC drops the unresolved reloc. First shared-code caller breaks ESPNOW=0 builds. Fix: move `parseMacAddress` to a shared TU. (espnow #9)
- `System_Utils.cpp` autolog block reads `gAutomationLog*` globals defined in CMake-excluded `System_Automation.cpp`; survives AUTOMATION=0 only because the header's stub branch defines a TU-local static and `-O2` folds the rest (verifier empirically compile-tested; breaks at `-O0`). (apps #0)

---

## Tier 1 — Waste in TODAY'S binary (~36 KB flash + ~0.7 KB DRAM reclaimable)

Everything here is live in the image currently being flashed (given the config above). Sorted by size. With BT=0 in the current config, the BLE/G2/R1 family (second table) is in today's binary too.

| Size | What | Where | Ref |
|---|---|---|---|
| ~9-11 KB | **Voice-routing columns in every CommandEntry** — `voiceCategory/voiceSubCategory/voiceTarget` (3 pointers × ~800-912 live rows); sole consumer is SR-gated `System_ESPSR.cpp`, off today | `System_Utils.h:62-64` | camera-mic-sr #0 |
| ~6 KB | Per-sensor CSV/text row formatters + header builders + mask UI for sensors that can't produce data (all sensors 0 today) | `System_SensorLogging.cpp:160-190, 395-684, 1420-1500` | i2c #2 |
| ~2.5 KB | Serial setup-wizard MQTT page (`handleSerialMQTTPage`, ~87 lines + ~15 literals) — MQTT=0; hidden only at runtime | `System_SetupWizard.cpp:1069-1073, 1192-1278, 1538-1549` | wifi-http #2 |
| ~2.3 KB | 14 per-sensor settings-editor commands (`thermalenabled`, `tofenabled`, `gpsbus`-family enables, poll-ms setters…) that register and dead-end with "setting not found" | `System_Settings.cpp:3064-3100, 3150-3185` | i2c #1 / registries #0 |
| ~2.2 KB | Embedded espsr icon (872 B PNG + 128 B bitmap) + edgeimpulse icon (1067 B + 128 B) in the linearly-scanned `EMBEDDED_ICONS[]` | `System_Icons.cpp:7069-7237, 7617-7618` | camera-mic-sr #2, #3 |
| ~1.5-2 KB | Six `debugsr*` commands (rows + strings + thunks) with ESP_SR=0 | `System_Debug.cpp:2194-2199` | camera-mic-sr #1 |
| ~1.5-2 KB | NeoPixel commands + `neopixel` module row + dummy `Adafruit_NeoPixel(0,-1)` object on a pin=-1 board (XIAO) — **no ENABLE_ flag exists** (flag gap) | `System_NeoPixel.cpp:505-513`, `System_Utils.cpp:2884-2892` | i2c #11 |
| ~1.6 KB | 10+ per-sensor bus-routing commands (`gpsbus`, `rtcbus`, `thermalbus`…) registered under `ENABLE_I2C_SYSTEM` only, all sensor flags 0 | `System_I2C.cpp:810-821, 2270-2282` | i2c #3 |
| ~1.3 KB | `i2cSensors[]` metadata rows (name/desc/manufacturer/library strings) for all 12 sensors regardless of flags (partly deliberate for `detect` naming — but the MAX17048 row IS guarded, so intent is inconsistent) | `System_I2C.cpp:216-248` | i2c #6 |
| ~1.2-1.5 KB | Battery time-series logging block that survives GC via unguarded power-event calls (`System_Utils.cpp:1389/1455/1466/1502`, `System_Power.cpp:139`) — appends fake `100%/5.0V/USB Power` rows on sleep/cpufreq/powermode events on a monitor-less board | `System_Battery.cpp:628-811` | i2c #0 (adjusted) |
| ~1.2 KB | `oled` command-module row + ~1.1 KB help string carried in a DISPLAY_TYPE=0 build (the ONLY unguarded feature-module row — all ~23 others are gated) | `System_Utils.cpp:2870-2883` | i2c #5 / registries #10 |
| ~0.8 KB | 5 Edge Impulse settings-editor commands (register + dead-end; EI=0) | `System_Settings.cpp:3070-3073, 3109, 3159-3162, 3194` | registries #1 |
| ~0.8 KB | Off-feature SYSEVT kind names in the unconditional X-macro (5 llm_*, 4 mqtt_*, 3 ei_*, 4 voice_*, ~14 sensor kinds) + dead notification-default switch cases | `System_Events.h:129-312` | registries #5 |
| ~0.5 KB | 3 ESP-SR settings-editor commands (`srenabled`, `srautostart`, `srmodelsource`) | `System_Settings.cpp:3074-3075, 3103, 3163-3164, 3188` | registries #2 |
| ~0.5 KB | 5 `DBG_ROW(MQTT*)` settings rows + `debugMqtt*` bools with MQTT=0 | `System_Settings.cpp:2000-2004`, `System_Settings.h:92-96, 537-541` | wifi-http #5 |
| ~0.5 KB | OLED residuals: `oledautostart` (dead-ends) + `oledclihistorysize` (actually WORKS and persists an OLED-only key on a display-less build) | `System_Settings.cpp:137-150, 246, 3108, 3193`, `System_Command.cpp:531-544` | registries #4 |
| ~0.45 KB | Dashboard JS `if(c.llm){...}` card-updater block shipped as string data; the `c.llm` key is never emitted with LLM=0 | `WebPage_Dashboard.h:432-439` | apps #5 |
| ~0.3-0.4 KB | Settings-page JS grouping lists/labels naming gated-off apps (`llm`, `espsr`, `edgeimpulse`…) | `WebPage_Settings.h:995, 1008-1009, 1536-1542, 1891-1892` | apps #9 |
| ~0.25 KB | `llmenabled` settings-editor command + `kStops` `{"llmenabled","llmunload"}` row (registers, dead-ends) | `System_Settings.cpp:2896, 3104, 3189` | apps #2 / registries #3 |
| ~0.15 KB | Guest-API allowlist dead strings: 5 `/api/llm/*` + `/api/mqtt/status` + `/api/speech/status` (harmless security-wise: unregistered URI = 404) | `WebServer_Utils.cpp:383-400` | apps #6 |
| ~45 B | FS permission row for `/system/llm/` — matches a directory never created in LLM-off builds | `System_Filesystem.cpp:1361-1365` | apps #8 |
| ~0.6 KB DRAM + ~0.5 KB ctor flash | Dead `gSettings` fields for off features: 23 MQTT (incl. 7 Strings), 13 SR, 8 EI, ~13 OLED, ~50 sensor tuning fields | `System_Settings.h` (see Pattern P2 for the constraint) | registries #6-#9, wifi-http #4, camera-mic-sr #7/#8, i2c #4 |

**BLE/G2/R1 family — in today's binary because BT=0:**

| Size | What | Ref |
|---|---|---|
| ~3.5-4 KB | `healthlogging`/`healthstatus`/`healthlogmerge` command rows + full `cmd_healthlogmerge` + big help strings (R1_HEALTH derives 0) | ble #0 |
| ~1 KB | 4 `debugbluetooth*` rows + `outble` row/stub (the static_asserts knowingly retain them) | ble #1 |
| ~1 KB | 11 BT/G2 debug-flag X-macro rows + persisted debug settings (sanctioned design — see P3) | ble #5 |
| ~0.5 KB | BLE notification rule cases + `NSINK_G2` plumbing + `notifG2` setting row (persists to settings.json) + `g2*` counters — plus the working `notifydeviceg2` editor command toggling a sink that cannot exist | ble #2, registries #11 |
| ~0.4 KB | 13 BLE/G2/RING SYSEVT kind names | ble #6 |
| ~0.25 KB | `bleenabled` editor command + `kStops` rows (registers, dead-ends) | ble #4 |
| ~0.25 KB | `g2StreamToneMap`/`g2PackRateMs` rows inside the **camera** settings module — camera=1/BT=0 today, so these G2 settings persist and list right now | ble #7 |
| ~0.15 KB | `g2status`/`ringstatus`/`blestatus` quiet-poll strings + `blesecret` redaction row | ble #8 |
| ~110 B RAM | BT/G2/R1 `gSettings` fields incl. 2 Strings | ble #3 |

Not counted above (also in today's binary, deliberate by documented design): the debug-flag X-macro registry keeps all 117 flag rows + ~156 `Settings` debug bools on every build (~2-3 KB; sanctioned by `docs/DEBUG_FLAG_XMACRO_PLAN.md` §3), and `System_FeatureRegistry` deliberately lists every feature so `features json` can report `compiled:false`.

---

## Tier 2 — Leaks that bite when a currently-ON flag goes off

The same classes of leak, for flags that are 1 today. Relevant to the FeatherS3 build (camera=0, mic-via-G2) and to any slimmed variant.

**ENABLE_BLUETOOTH / ENABLE_G2_GLASSES / ENABLE_R1_HEALTH off (~6-9 KB) — NOTE: this IS today's config (BT=0); the family is tabulated in Tier 1 above. Details kept here with the rest of the domain:**
- `healthlogging`/`healthstatus`/`healthlogmerge` command rows + full `cmd_healthlogmerge` (~116 lines) + big help strings — ~3.5-4 KB (ble #0)
- 4 `debugbluetooth*` rows + `outble` row/stub — ~1 KB; note the adjacent G2 rows ARE gated, and the table's static_asserts knowingly retain these (ble #1)
- BLE/G2 notification plumbing: `SYSEVT_BLE_*` rule cases, `NSINK_G2` mask math, `notifG2` setting row (persists to settings.json), `g2*` counters — ~0.5 KB (ble #2, registries #11)
- `bleenabled` editor command + `kStops` rows — ~0.25 KB (ble #4)
- 11 BT/G2 debug-flag X-macro rows (~1 KB, design change — see P3) (ble #5); 13 BLE/G2/RING event kinds (~0.4 KB) (ble #6)
- BT/G2/R1 `gSettings` fields incl. 2 Strings (~110 B RAM) (ble #3)
- `g2StreamToneMap`/`g2PackRateMs` rows inside the **camera** settings module — persist on a camera-on/BT-off build (ble #7)
- Quiet-poll strings + `blesecret` redaction row (~150 B) (ble #8)
- registries note: the entire `even_r1` ring command module registers under bare BT&&G2 — `ENABLE_R1_HEALTH` gates only the health pages, so a BT+G2/R1=0 build still carries every `ring*` command (flag-scope question)

**ENABLE_ESPNOW off (~15-20 KB, on top of the Tier-0 link break):**
- G2 "ESP-NOW App" lens page: file gates only on BT&&G2; ~572 of 1306 lines survive ESPNOW=0, and the Apps-menu row + page registration are unguarded (siblings Maps/LLM/Health ARE gated) — ~8-12 KB (espnow #1)
- Setup-wizard ESP-NOW identity pages, serial + OLED (~105 lines each), runtime-hidden only — ~4-6 KB (espnow #2)
- 3 `espnow*` settings-editor commands (~0.8 KB) (espnow #4); `usersync` row + ~450 B help + stub (~0.6 KB) (espnow #5)
- ~25 espnow/mesh `gSettings` fields incl. `MeshIdentity meshes[4]` with 128 B of key material — ~0.5 KB RAM (espnow #6)
- ~20 mesh/bond SYSEVT kinds + notification format strings — ~1-1.5 KB (espnow #7)

**ENABLE_CAMERA_SENSOR off (FeatherS3): ** 5 `debugcamera*` commands (~1.5 KB), camera icon (~1.1 KB), ~26 camera `gSettings` fields (camera-mic-sr #4, #5, #6). **ENABLE_MICROPHONE off** (PDM-less, no-G2 boards): 4 `debugmic*` commands + duplicate mic icons + mic fields — ~3 KB (camera-mic-sr #9).

**ENABLE_HTTPS off:** G2 network lens renders an HTTPS toggle whose backing command is unregistered → tap errors (~0.3 KB) (wifi-http #7). **ENABLE_HTTP_SERVER off:** `webclihistorysize` command + `webConsole` debug row register with nothing behind them (~0.45 KB) (wifi-http #6). **ENABLE_MQTT off with OLED on:** the OLED MQTT wizard page compiles too (~1.5 KB) (wifi-http #3). **ENABLE_MAPS / ENABLE_AUTOMATION off:** 4 `debugmaps*` + `debugautomations` rows (~0.75 KB) (apps #4); `automationautostart` editor command (~0.2 KB) (apps #3).

---

## Tier 3 — Compiled-then-stripped (no flash cost, but compile time + fragility)

GC saves these today; each is one unguarded caller away from becoming live, and all cost parse/compile on every build.

| What | Where | Ref |
|---|---|---|
| **Entire Automations web page** — 1910 lines / ~50-70 KB of HTML/JS string data compiled per build; `HardwareOne.cpp` guards the same include, `WebServer_Server.cpp` doesn't | `WebPage_Automations.h`, `WebServer_Server.cpp:44, 1669` | apps #7 |
| **Entire ImageManager** (~17 KB object code, 674 lines) — no file-level guard; all roots happen to be camera-gated; ~50 B ctor+BSS residual always survives via `.init_array` | `System_ImageManager.cpp` | camera-mic-sr #10 |
| **Entire I2C machinery** (~4,100 lines: `System_I2C.cpp` + `System_I2C_Manager.cpp`) compiles even at I2C level 0 — no whole-file wrap; expected 0 flash after GC (unverified against a real level-0 link) | both files | i2c #7 |
| `System_ESPNow_Tx.cpp` — the ONLY ESP-NOW TU (of 12) missing the file-wide `#if ENABLE_ESPNOW` (~2-3 KB would-be) | whole TU | espnow #8 |
| `BLE_CentralTx.cpp` — no guard at all; both consumers are G2-wrapped | whole file | ble #9 |
| HAL_Input core (~150 lines) — guard covers only includes + CLI tail | `HAL_Input.cpp:20-238` | i2c #8 |
| MAX17048 driver + backend gate on `BATTERY_BACKEND_FUEL_GAUGE` (board constant) instead of `ENABLE_BATTERY_MONITOR` — force-disabling the monitor still compiles the driver | `i2csensor_max17048.cpp:5`, `System_Battery.cpp:179-288` | i2c #9 |
| `/api/sensors` early-return anti-pattern: `#if !ENABLE_THERMAL_SENSOR return; #endif` then ~80 unguarded thermal lines relying on DCE (same shape for tof/imu/input) | `WebPage_Sensors.cpp:100-180` | i2c #10 |
| `executeUnifiedWebCommand(httpd_req_t*,...)` unguarded (~400 B object) | `System_Utils.cpp:5150-5164` | wifi-http #8 |

---

## Systemic patterns (the actual fix list)

Most of the 74 findings are instances of nine repeated shapes. Fixing pattern-wise beats fixing finding-wise.

**P1 — The `settingEditorCommands[]` table is entirely unguarded.** ~30 per-setting commands register regardless of flags (sensors ×14, EI ×5, SR ×3, espnow ×3, llm, automation, oled ×2, bleenabled…); each is a CommandEntry + help/usage strings + a generated setter that dead-ends with "Error: setting not found". The codebase's own convention (`espnowenabled` row, `#if ENABLE_ESPNOW` at `System_Settings.cpp:231-233`) shows the intended shape. Fix: wrap each cluster in the same `#if` its `SettingsModule` registration already uses. Also gate the matching `kStops[]` teardown rows (`System_Settings.cpp:2880-2899`).

**P2 — `gSettings` struct fields are unguarded for most features** while BONDED_MODE / AUTOMATION / LLM fields prove the pattern works (`System_Settings.h:225-240, 420-437, 782-807, 1098-1116`). ~140 dead fields across MQTT/SR/EI/OLED/sensors/espnow/BT-G2/camera. **Constraint discovered by the verifiers:** `System_FeatureRegistry` rows (deliberately unconditional) and the `System_RamFlush` default map read each feature's `*Enabled`/`*AutoStart` field from always-compiled code — so gate only the *tuning* fields and keep the enable/autostart pairs unconditional (or gate those readers with the same flags). Specific keep-lists are in registries #6-#9.

**P3 — Debug command rows vs. the sanctioned flag registry.** The 256-bit debug-flag X-macro banks are deliberately unconditional (drift = build error; `docs/DEBUG_FLAG_XMACRO_PLAN.md` §3) — leave them. But the *CLI command rows* are separable and the table already does it for LLM and G2 (`System_Debug.cpp:2095-2102, 2183-2192`): extend the same treatment to `debugbluetooth*` ×4 + `outble`, `debugsr*` ×6, `debugcamera*` ×5, `debugmaps*` ×4, `debugautomations`, `debugmic*` ×4 — and adjust the `kDbgGenCmdRowsInTable`/`kDbgHandCmdRowsInTable` static_asserts, which currently *prove* these rows are knowingly retained.

**P4 — The SYSEVT kind X-macro carries every family unconditionally** (151 kinds; ~30 belong to off features today). Split into per-family segments included under their flags (names, not ordinals, are the persisted form — safe under the no-backwards-compat policy), or accept as registry uniformity and document it. (`System_Events.h:129-312`)

**P5 — `EMBEDDED_ICONS[]` has no per-feature rows.** The linear scan keeps every icon referenced. Wrap each icon's PNG + bitmap + row in its feature flag (espsr, edgeimpulse, camera, microphone+mic). (`System_Icons.cpp`)

**P6 — Setup-wizard pages are runtime-hidden, not compile-gated.** MQTT (serial + OLED) and ESP-NOW (serial + OLED) page handlers, dispatch cases, and `kPageOrder` entries all compile; only `wizardShouldShow*()` hides them. The same file already guards its other MQTT bits, so the shape exists. (`System_SetupWizard.cpp`, `OLED_SetupWizard.cpp`)

**P7 — Cross-feature UI needs secondary guards.** A surface gated on its *host* feature but not the feature it *fronts*: G2 ESP-NOW app (BT&&G2 but not ESPNOW), G2 Automations page (missing ENABLE_AUTOMATION — Tier-0 link break), G2 HTTPS toggle (missing ENABLE_HTTPS), camera settings module carrying G2 rows (missing BT&&G2). House rule worth writing down: **a UI row/page must AND-in every feature it dispatches to.** Note `G2_Glasses.cpp` already does this right for Maps/LLM/Health rows — ESP-NOW and Automations rows are the stragglers.

**P8 — Missing whole-file wraps that GC currently rescues.** Add the file-wide `#if` anyway (BLE_CentralTx, System_ESPNow_Tx, System_ImageManager, System_I2C + Manager, HAL_Input core) so exclusion is structural rather than an accident of caller guards. Zero flash win; buys compile time and removes resurrection/link-break risk.

**P9 — Flag gaps (no compile-time toggle exists at all).** Opportunities, not defects: `System_UartLink.cpp` (~350 lines / ~8 KB + a task; gated only by board `#ifdef UART_LINK_PORT`, and every current board defines the pins), NeoPixel (~1.5-2 KB live on pin-less boards — see Tier 1), the `led` module (`System_Hardware.cpp:89`), `System_PollPause.cpp` (trivial), the sensorlog subsystem core (flagless by design — which is exactly why its per-sensor formatters leaking matters), I2C dual-bus init paths (runtime-only decision even on boards with no I2C2 pins, ~1-2 KB), plus capturecrypt (`System_SensorLogging.cpp:2493`) which stays registered when R1_HEALTH=0.

---

## What's verifiably CORRECT today (don't re-audit)

The audit positively verified (guard-walked, not assumed) the following as properly gated:

- **Whole-file wraps:** `Bluetooth.cpp`, `System_BleSecureChannel.cpp`, `BLE_Events.cpp`, `BLE_Peers.cpp`, `BLE_IDF.cpp` (double-gated, experimental flag defaults 0), `G2_Glasses.cpp` (26 KLOC), `G2_Ring.cpp`, `G2_Pet.cpp`, `G2_Hijack*.cpp`, all 12 `G2_Page_*.cpp` (correct BT&&G2 idiom; CameraSettings correctly triple-gates), `G2_Health.cpp` + `R1_HealthHistoryStore.cpp` (gate on ENABLE_R1_HEALTH — compile empty with G2 on/R1 off), `System_WiFi.cpp` (ENABLE_WIFI, with correct internal HTTP/HTTPS nesting), 11 of 12 `System_ESPNow*.cpp` (17.7 KLOC main TU included), `System_MeshPeers.cpp`, `System_BondedPeer.cpp` (BONDED_MODE, equivalent), `System_Camera_DVP.cpp`, `System_Microphone.cpp` + `HAL_Audio.cpp` (ENABLE_MICROPHONE with PDM inner gates), `System_ESPSR.cpp`, all 10 `i2csensor_*.cpp` (guard opens ≤ line ~20, closes at EOF), `HAL_Display.cpp` (empty TU at DISPLAY_TYPE=0 — no font/framebuffer leakage), `OLED_Utils.cpp` (one big guard + deliberate stub tail), `OLED_ESPNow.cpp`, `OLED_Mode_Remote/UnifiedMenu` (triple-gated).
- **Registration ladders:** `gCommandModules[]` rows are per-feature `#if`'d for ~23 modules (only `oled` is not); `registerAllSettingsModules()` is exemplary — every module extern + registration gated, incl. nested BT→G2; web nav links + route registration all under the right `ENABLE_WEB_*`; HardwareOne boot/loop init + autostart calls all wrapped (unguarded-looking calls resolve to verified deliberate header stubs).
- **BONDED_MODE=0 with ESPNOW=1 is clean:** bond opcodes unregistered, dispatcher has an honest drop path, bond commands/settings/cache/condition-vars all gated (~40 regions in System_ESPNow.cpp verified).
- **GAMES is completely clean** — no unguarded references anywhere; per-game CMake selection consistent with self-gates; the both-games `#error` works.
- **Deliberate stub patterns (leave alone):** `System_SensorStubs.{h,cpp}`, `OLED_Utils` stub tail, `Bluetooth.h`/`G2_Ring.h` inline no-op stubs, `System_Automation.h`/`System_Maps.h`/`System_ESPSR.h`/`System_EdgeImpulse.h` stub blocks, WebMirror cap-0 stubs, FeatureRegistry's compiled:false design, `System_Camera_Video.cpp`'s video-browse-without-camera base experience.

## IDF / sdkconfig-level observations (outside the app component)

- `idf_component.yml` pulls **esp32-camera, esp-sr, and esp-tflite-micro into every build**; with the features off their objects are unreferenced and stay out of flash (static-archive semantics + GC), so the cost is build time only.
- However `sdkconfig` still carries `CONFIG_USE_AFE=y`, `CONFIG_USE_WAKENET=y`, `CONFIG_USE_MULTINET=y`, `CONFIG_SR_WN_WN9_HILEXIN=y`, `CONFIG_MODEL_IN_FLASH=y` while `ENABLE_ESP_SR=0`. The active partition table has no model partition so `srmodels.bin` isn't flashed, but the esp-sr component + model-packing machinery builds every time. Recommend flipping these off in the SR-off sdkconfig defaults (build time + desync risk).
- `ENABLE_BLUETOOTH=0 also needs `CONFIG_BT_ENABLED=n` to reclaim the Bluedroid stack (~14 KB IRAM / ~70 KB flash / ~80 KB DRAM) — already documented in the header.

## Caveats

- Read-only audit; **no test builds were run.** Link-break claims rest on exhaustive symbol-definition analysis (sole definitions in gated/excluded TUs, no stubs, live referencers verified); apps #0 was additionally compile-tested by its verifier with the real toolchain. A trial build of `NET_LEVEL_WIFI_HTTP` would confirm Tier-0 #1 in one shot and may surface more.
- Size figures are line-count heuristics (~25 B/line + measured string lengths), not `.map`-file measurements. Icon and module-string sizes were measured exactly.
- "Today's binary" assumes the audited config above. `sdkconfig` currently in the tree is the XIAO camera-build variant.
- Full agent transcripts + raw findings JSON: workflow run `wf_8a5bb61f-0e8` (session `c89114e3`), journal at `subagents/workflows/wf_8a5bb61f-0e8/journal.jsonl`.

---

# Detailed findings by domain

Below: every finding with verdict from the adversarial verify pass. Duplicates across domains are cross-referenced rather than removed (the registries agent often adds precision the domain agent lacked). "Reachability": **live** = survives `--gc-sections` (real flash), **data** = static data kept via live tables, **stripped** = compiled then GC'd (no flash), **unknown**.

## Domain: Bluetooth / G2 Glasses / R1 Health

**ble #0 — ENABLE_R1_HEALTH — `System_SensorLogging.cpp:2453-2492, 2209-2325` — live, ~3.5-4 KB.** `healthlogging`/`healthstatus`/`healthlogmerge` CommandEntry rows (~1.3-1.4 KB of help/usage strings) plus the FULL `cmd_healthlogmerge` (~116 lines) sit outside the file's R1 guards (which correctly cover the implementations at 1575/1597/1795/2069/2514). Registered via the unconditional `sensorlog` module row (`System_Utils.cpp:3201`). Verifier: `cmd_healthlogging`'s dispatch body also compiles (only the `on` core is stripped). Fix: wrap the three rows + `cmd_healthlogmerge` in `#if ENABLE_R1_HEALTH`.

**ble #1 — ENABLE_BLUETOOTH — `System_Debug.cpp:2035-2038, 2193, 1195-1224` — live, ~0.8-1 KB.** 4 `debugbluetooth*` rows + `outble` row/stub; the `outble` row sits immediately AFTER the `#endif` of the BT&&G2 block that correctly strips `outg2`/`debugg2*`. The `kDbg*RowsInTable` static_asserts (2216-2236) subtract only LLM+G2 rows — proving deliberate retention. Fix: extend the guards; adjust the static_assert accounting.

**ble #2 — ENABLE_BLUETOOTH — `System_Notifications.cpp:65-66, 374-389, 424, 661-662, 734-788` — live, ~0.5 KB + persisted settings row.** `SYSEVT_BLE_*` rule cases, "BLE:" format strings, `NSINK_G2` mask plumbing, `notifG2` SettingEntry (persists to settings.json when BT=0 — `notifSettingsModule` registers unconditionally at `System_Settings.cpp:2429`), `g2Pushed/Filtered/Dropped` counters + printfs. Fix: `#if ENABLE_BLUETOOTH` (G2 bits BT&&G2).

**ble #3 — ENABLE_BLUETOOTH — `System_Settings.h:297-379, 913-1047` — data, ~110 B RAM + ~200 B ctor flash.** Entire BT/G2/R1 settings-field family unguarded (`bleEnabled`…`bleSecureChannelSecret` String, `healthLogging*`, `ring*Desired`, `g2Stream*`) while BONDED/AUTOMATION/LLM fields in the same header ARE gated. Fix: mirror the BONDED_MODE pattern (see P2 constraint).

**ble #4 — ENABLE_BLUETOOTH — `System_Settings.cpp:2883, 2905-2907, 3106, 3191` — live, ~250 B.** `bleenabled` editor command + `kStops {"bleenabled","g2deinit"}` + `closeble` special-case, all unguarded; with BT=0 the command dead-ends (module unregistered). Fix: `#if ENABLE_BLUETOOTH` (P1).

**ble #5 — ENABLE_BLUETOOTH — `System_DebugFlags.h:216-231` — data, ~0.8-1 KB.** 11 BT/G2 X-macro debug-flag rows expand into always-compiled flag tables + persisted debug settings. Deliberate X-macro-unification design — gating rows is a design change (see P3), not an oversight fix.

**ble #6 — ENABLE_BLUETOOTH — `System_Events.h:156-170` — data, ~0.35-0.45 KB.** 13 BLE/G2/RING SYSEVT kinds; wire-name strings survive via live kind tables in guard-free `System_Events.cpp`. Fix: per-family X-macro segments (P4).

**ble #7 — ENABLE_G2_GLASSES — `System_Camera_DVP.cpp:2294-2296` — live, ~250 B + 2 persisted JSON keys, IN TODAY'S BINARY.** `g2StreamToneMap`/`g2PackRateMs` rows inside `cameraSettingEntries` — a camera-on/BT-off build (exactly today's XIAO config) persists and lists G2 settings. Fix: `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` around the two rows (P7).

**ble #8 — ENABLE_BLUETOOTH — `System_Utils.cpp:866-880, 1118` — live, ~150 B.** `kQuiet` strings (`g2status`/`ringstatus`/`blestatus`) checked on every command in the always-on audit path + `blesecret` redaction-rule row. Pure hygiene.

**ble #9 — ENABLE_BLUETOOTH — `BLE_CentralTx.cpp` (whole 43-line file) — stripped.** Zero guards; both consumers (G2_Ring, G2_Glasses) are G2-wrapped so GC strips everything when BT=0. Fix: add the BT&&G2 wrap for hygiene (P8).

**Domain notes:** all 30 primary BLE/G2 files verified correctly wrapped (see "verifiably correct" above). Tail items: quiet-route strings `/api/ble/status`,`/api/health/status` in `WebServer_Utils.cpp:381/386` (~40 B); `"128|R1 Health"` mask option + `r1` keyword branches in SensorLogging (~30 lines, runtime-dead); `BLE_Peers.cpp` keeps g2/ring peer-kind rows when BT=1/G2=0 (~100 B); mislabeled closing-`#endif` comments at `BLE_IDF.cpp:1155`, `WebPage_Bluetooth.cpp:170`. Adjacent: capturecrypt has no flag (registered when R1=0); `System_Utils.cpp:57` includes G2_Glasses.h under bare `#if ENABLE_G2_GLASSES` (violates the BT&&G2 house rule; harmless — header self-stubs).

## Domain: WiFi / HTTP / HTTPS / MQTT / Migration tool

**wifi #0 — ENABLE_WIFI — `System_Utils.cpp:1643-1654` — live, COMPILE BREAK at NETWORK_FEATURE_LEVEL=0.** `buildSystemInfoJson()` net block calls `WiFi.RSSI()/channel()/macAddress()/localIP().toString()`; the preceding `#if ENABLE_WIFI` closes at 1642, and the stub `WiFiClass` lacks those members. Fix: extend the guard with an `#else` emitting empty fields, or extend the stub.

**wifi #1 — ENABLE_MIGRATION_TOOL — `CMakeLists.txt:403-416` vs `System_BuildConfig.h:~535-563` — live, LINK BREAK in the advertised headless config.** CMake never greps `ENABLE_MIGRATION_TOOL`; `WebServer_MigrationTool.cpp` is HTTP-gated only, while `System_FirstTimeSetup.cpp:587/606/613` calls its restore functions under `#if ENABLE_MIGRATION_TOOL && ENABLE_WIFI`. The stub-suppression at `System_SensorStubs.h:302-310` shows the headless config was engineered on the preprocessor side only. Fix: grep the flag in CMake; include the file when HTTP **or** MIGRATION is on.

**wifi #2 — ENABLE_MQTT — `System_SetupWizard.cpp:1069-1073, 1192-1278, 1538-1549` — live, ~2.5 KB, IN TODAY'S BINARY.** Serial MQTT wizard page + dispatch; runtime-hidden only (`wizardShouldShowMQTT`). The same file guards its other MQTT bits (506, 555). Fix: P6.

**wifi #3 — ENABLE_MQTT — `OLED_SetupWizard.cpp:810-857` — live in OLED builds, ~1.5 KB.** `handleOLEDMQTTPage()` (host/port/user/password prompts writing `gSettings.mqtt*`); file has no ENABLE_MQTT anywhere. Absent today only because DISPLAY_TYPE=0 excludes the file. Fix: P6.

**wifi #4 — ENABLE_MQTT — `System_Settings.h:395-418, 1070-1093` — data, ~140 B RAM.** 23 MQTT fields (7 Strings, 13 bools, 3 ints) + defaults `"homeassistant"`, `"/system/certs/mqtt_ca.crt"` (the one over-SSO heap alloc). Adjacent LLM fields ARE wrapped. See registries #6 for the keep-list constraint. Fix: P2.

**wifi #5 — ENABLE_MQTT — `System_Settings.cpp:2000-2004` + `System_Settings.h:92-96, 537-541` — live, ~0.5 KB, IN TODAY'S BINARY.** 5 `DBG_ROW(MQTT*)` settings rows + `debugMqtt*` bools. Fixer caveat verified: `DBG_ROW` picks from the X-macro-generated `kDbgFlagRegEntry[]` which is live at runtime and row-count-asserted — the fix must account for `kDbgRegGeneratedRows`.

**wifi #6 — ENABLE_HTTP_SERVER — `System_Settings.cpp:121-134, 245, 2012` — live in HTTP=0 builds, ~450 B.** `webclihistorysize` command + `webConsole` debug row + fields; neighbors are correctly guarded (WIFI 216-224, HTTP 235-237, HTTPS 238-240) — these two sit outside. Fix: move under `#if ENABLE_HTTP_SERVER`.

**wifi #7 — ENABLE_HTTPS — `G2_Page_Network.cpp:1867-1869, 1902-1908, 1938-1955` — live in HTTP-on/HTTPS-off builds, ~300 B.** G2 lens renders an `HTTPS: ON/OFF` toggle dispatching `httpsEnabled <0|1>`, but the backing command exists only under `#if ENABLE_HTTPS` — tap yields unknown-command. Fix: P7.

**wifi #8 — ENABLE_HTTP_SERVER — `System_Utils.cpp:5150-5164` — stripped, ~400 B object.** `executeUnifiedWebCommand(httpd_req_t*,...)` unguarded (compiles against the typedef stub); all callers are CMake-HTTP-gated. Hygiene (P8).

**Domain notes:** `System_WiFi.cpp` fully wrapped with correct internal HTTP/HTTPS nesting; NTP block + `cmd_ntpserver` properly guarded/stubbed; OTA (`System_OTA.cpp`/`System_OTASafety.cpp`) verified genuinely network-free (BLE/file-based). No mDNS/captive-portal/provisioning code exists in the tree at all. Tail: `httpsEnabled` field unguarded (~1 B); `kStops` wifi/http rows (~30 B); `mqttpassword ` redact literal; `gWebMirrorSeq`; FeatureRegistry `mqtt` row (deliberate); `System_User.cpp:614` gSessions walk relies on the stub nullptr (runtime-safe).

## Domain: ESP-NOW / Bonded mode / Mesh

**espnow #0 — ENABLE_ESPNOW — `System_WiFi.cpp:297-298, 429-430, 459-460, 893-894, 1660-1670, 1726-1728` — live, LINK FAILURE in WiFi-on/ESPNOW-off (stock levels 1-2).** Unguarded calls to `isEspNowInitialized()` and `espnowNoteWifiChannelMayHaveChanged()` (the latter inside the address-taken `wifiEventLogger` registered via `WiFi.onEvent`). Both defined only inside `System_ESPNow.cpp`'s file-wide `#if ENABLE_ESPNOW` (depth-tracked, 17.7 KLOC, no `#else` stub; no other definition repo-wide). The power-save block in the SAME file (479-539) correctly guards its espnow calls — these sites were missed. Fix: mirror the power-save guards, or add inline no-op stubs.

**espnow #1 — ENABLE_ESPNOW — `G2_Page_ESPNow.cpp` (~572 of 1306 lines) + `G2_Glasses.cpp:4739, 4610, 6973` — live in BT+G2/ESPNOW=0 builds, ~8-12 KB.** File self-gates on BT&&G2 only; verifier simulated ESPNOW=0 across its 30 internal regions → ~570 lines survive (menu shell, pager, tap dispatch, fallback screens). The Apps-menu row `add("ESP-NOW")` and `g2RegisterPage(kEspNowAppPage)` are unguarded while sibling Maps/LLM/Health rows ARE flag-gated. Fix: P7.

**espnow #2 — ENABLE_ESPNOW — `System_SetupWizard.cpp:177, 1064-1067, 1087-1280, 1525-1534` + `OLED_SetupWizard.cpp:694-798` — live, ~4-6 KB.** Serial + OLED ESP-NOW identity wizard pages, direct-called from the always-compiled wizard loop; only `wizardShouldShowESPNow()` (545-552) hides them at runtime. Fix: P6.

**espnow #3 — ENABLE_ESPNOW — `OLED_SetupWizard.cpp:383-392` — WRONG GUARD POLARITY, functional bug.** `#ifndef ENABLE_ESPNOW` never fires (macro always defined 0/1) → in ESPNOW=0 OLED builds the System page's item list and rendered rows desync. Fix: `#if !ENABLE_ESPNOW`.

**espnow #4 — ENABLE_ESPNOW — `System_Settings.cpp:~3087-3088, 3109, 3171-3172, 3192` — live in ESPNOW=0 builds, ~0.8 KB.** `espnowcapturetosd`/`espnowcaptureskipheartbeats`/`espnowautostart` editor commands; backing SettingEntry rows are inside `System_ESPNow.cpp`'s wrap → register-and-dead-end. Fix: P1.

**espnow #5 — ENABLE_ESPNOW — `System_User.cpp:3507-3510, 3769-3775` — live, ~0.6 KB.** `usersync` row with ~450 B help string + "not enabled" stub; the `espnowenabled` row precedent (`System_Settings.cpp:231-233`) shows the intended shape. Fix: move the row inside the guard, drop the stub.

**espnow #6 — ENABLE_ESPNOW — `System_Settings.h:195-224, 698-770, 927` — data, ~0.5 KB RAM.** ~25 espnow/mesh fields incl. 9 Strings and `MeshIdentity meshes[4]` (each with a 32-byte stretched key). Serialization and module registration ARE gated — fields are pure dead state. Fix: P2 (`SelfDevice::deviceName` fallback needs a small `#else`).

**espnow #7 — ENABLE_ESPNOW — `System_Events.h:103, 130-147, 289-291` + `System_Notifications.cpp:63-73, 340-341, 659-673` — live, ~1-1.5 KB.** Exactly 20 mesh/bond/pairing SYSEVT kinds + notification policy cases + render format strings. Fix: P4.

**espnow #8 — ENABLE_ESPNOW — `System_ESPNow_Tx.cpp` (whole 305-line TU) — stripped, ~2-3 KB would-be.** The ONLY ESP-NOW-family TU missing the file-wide wrap (all 11 siblings open it within their first 11 lines). Verifier found a caller the finder missed (`System_TaskUtils.cpp:527`) — itself guarded, so the conclusion survives. Fix: P8.

**espnow #9 — ENABLE_ESPNOW — `System_Command.cpp:456-460` — stripped, latent LINK BREAK.** `argMac()` calls `parseMacAddress` (sole def inside System_ESPNow.cpp's wrap). Links today only because argMac's only caller is espnow-gated. Fix: move `parseMacAddress` into a shared TU.

**Domain notes:** 11 of 12 ESP-NOW TUs + MeshPeers/BondedPeer/OLED surfaces verified correctly wrapped; BONDED=0/ESPNOW=1 combination verified clean end-to-end (~40 guard regions); `System_SelfDevice.cpp` is deliberately shared identity code. Tail: `DBG_ROW(ESPNOW_*)` rows + `DEBUG_ESPNOW_*` format strings in shared TUs (~0.3-0.5 KB live); OLED mode slug map strings (~50 B). None of these espnow leaks are in TODAY'S binary (ESPNOW=1); finding #0 makes two documented stock network levels unlinkable.

## Domain: Camera / Microphone / ESP-SR / Edge Impulse

**cam #0 — ENABLE_ESP_SR — `System_Utils.h:62-64` — data, ~9-11 KB, IN TODAY'S BINARY. The single largest finding.** `voiceCategory/voiceSubCategory/voiceTarget` — 3 pointer columns in EVERY `CommandEntry` (verifier's awk count: 940 rows repo-wide, ~800+ live). Sole consumer is `System_ESPSR.cpp` (fully SR-wrapped). Enabled-module tables are referenced from the unconditional `gCommandModules[]`, so the .rodata is not GC-able. Fix: wrap the 3 fields in `#if ENABLE_ESP_SR` with ctors that accept-and-drop the voice args when off — no table rows need editing.

**cam #1 — ENABLE_ESP_SR — `System_Debug.cpp:2194-2199` — live, ~1.5-2 KB, IN TODAY'S BINARY.** Six `debugsr*` commands setting flags nothing reads. Fix: P3.

**cam #2 — ENABLE_ESP_SR — `System_Icons.cpp:7160-7237, 7618` — data, ~1.0 KB, IN TODAY'S BINARY.** espsr icon (872 B PNG + 128 B bitmap + row) kept via the `findEmbeddedIcon()` linear scan. Fix: P5.

**cam #3 — ENABLE_EDGE_IMPULSE — `System_Icons.cpp:7069-7140, 7617` — data, ~1.2 KB, IN TODAY'S BINARY.** edgeimpulse icon, same table. Fix: P5.

**cam #4 — ENABLE_CAMERA_SENSOR — `System_Debug.cpp:2039-2043` — live on camera=0 builds (FeatherS3), ~1.5 KB.** Five `debugcamera*` commands; the same table gates LLM and G2 rows, proving the pattern. Fix: P3.

**cam #5 — ENABLE_CAMERA_SENSOR — `System_Icons.cpp:6499-6580, 7610` — data on camera=0 builds, ~1.1 KB.** Camera icon. Fix: P5.

**cam #6 — ENABLE_CAMERA_SENSOR — `System_Settings.h:322-354, 568-572, 908, 953, 963-1014` — data on camera=0 builds, ~110-150 B RAM.** ~26 camera tuning/automation fields incl. 2 Strings + 5 `debugCamera*` bools; schema is correctly compiled out with the camera TU, leaving orphaned fields. Keep `cameraAutoStart`/`cameraEnabled` for the FeatureRegistry row or gate it too. Fix: P2.

**cam #7 — ENABLE_ESP_SR — `System_Settings.h:389-394, 643-648, 910, 1063-1069` — data, ~40 B RAM, IN TODAY'S BINARY.** 13 SR fields; unguarded readers are only the deliberate FeatureRegistry row and RamFlush map. Fix: P2 (gate the 5 tuning fields + 6 debug bools; keep srEnabled/srAutoStart).

**cam #8 — ENABLE_EDGE_IMPULSE — `System_Settings.h:355-361, 929, 1016-1022` — data, ~30 B RAM, IN TODAY'S BINARY.** 8 EI fields; same keep-list constraint (`eiEnabled` read by the registry row). Fix: P2.

**cam #9 — ENABLE_MICROPHONE — `System_Debug.cpp:2047, 2085-2087` + `System_Icons.cpp:6582-6706, 7611-7612` + `System_Settings.h:959-962` — live on mic-less builds, ~3 KB.** 4 `debugmic*` commands, DUPLICATE microphone+mic icons (762 B PNG each + 2 bitmaps), 4 mic fields incl. String `micSource`. Zero cost on both current boards (both derive mic=1). Fix: P3/P5/P2.

**cam #10 — ENABLE_CAMERA_SENSOR — `System_ImageManager.cpp` (whole 674-line file) — stripped, ~17 KB object code per build + ~50 B ctor/BSS residual.** No file-level guard; every reachability root happens to be camera-gated (`System_Filesystem.cpp:90-93`, `System_Utils.cpp:251-254, 3244-3254`); G2 image push verified NOT to use it. The `gImageManager` ctor survives via `.init_array`. Fix: P8 (wrap the file or add to the CMake camera group).

**Domain notes:** `System_Camera_DVP.cpp`, `System_Microphone.cpp`, `HAL_Audio.cpp`, `System_ESPSR.cpp` all verified fully wrapped (working-tree ESPSR diff is 1 inserted line, no guard changes); G2 camera viewer + G2_Page_CameraSettings correctly inner/triple-gated; WebPage_Sensors camera/mic/EI handlers degrade to `not_compiled` JSON; SensorStubs lockstep verified. Deliberate: `System_Camera_Video.cpp` video browse/delete outside the camera gate (any SD board browses recordings — documented). Tail: `kStops` rows for camera/mic/sr (~60 B); espsr string compares (`System_I2C.cpp:1166`, `OLED_Utils.cpp:5237`); mislabeled `#endif` comment `WebPage_Speech.cpp:61`. Cross-domain: `WebServer_Server.cpp:5531-5532` calls `registerEdgeImpulseHandlers`/`registerESPSRHandlers` unguarded — safe only via header inline stubs. IDF-level: see sdkconfig section (CONFIG_USE_WAKENET etc. still `y` with SR off).

## Domain: I2C sensors / OLED display / Input / Battery

**i2c #0 — ENABLE_BATTERY_MONITOR — `System_Battery.cpp:628-811` — live (adjusted), ~1.2-1.5 KB survives GC, IN TODAY'S BINARY.** Battery CSV-logging block. Verifier corrected the finder on three points: the main-loop `batteryLogTick` call IS guarded (no periodic 60-s logging / flash wear), the HardwareOne `batteryLogEvent` calls are OLED-gated, and `cmd_batterylog` is GC'd. Still genuinely live: `batteryLogEvent→batteryLogAppend` via UNGUARDED calls at `System_Utils.cpp:1389` (cpufreq), `1455/1466` (light sleep), `1502` (deep sleep), `System_Power.cpp:139` (powermode) — event rows with fake `100%/5.0V/"USB Power"` values on a monitor-less board (`batteryLogEnabled` defaults true). `batteryLogSettingsModule` registers unconditionally (`System_Settings.cpp:2426`). Fix: wrap the block, stub `batteryLogTick/Event`, guard the registration.

**i2c #1 — per-sensor flags — `System_Settings.cpp:3064-3110, 3141-3230` — live, ~3 KB, IN TODAY'S BINARY.** 15+ per-sensor settings-editor commands (see registries #0 for the full list). Fix: P1.

**i2c #2 — per-sensor flags — `System_SensorLogging.cpp:160-190, 395-684, 829-835, 1100-1130, 1420-1500` — live, ~6 KB, IN TODAY'S BINARY.** Per-sensor CSV header builders, text/CSV row formatters, mask checkbox UI + mask-name parsing for sensors that cannot produce data; row-builder region 347-684 has NO sensor guards while the snapshot-CAPTURE section (685-825) is correctly per-sensor gated. `sensorlog all` even sets mask bits for non-compiled sensors. Fix: mirror the capture-section guards.

**i2c #3 — per-sensor flags — `System_I2C.cpp:810-821, 2270-2282` — live, ~1.6 KB, IN TODAY'S BINARY.** 10+ `*bus` routing commands. Possibly deliberate (set-bus-before-reflash workflow) — but the MAX17048's DB row IS guarded, so intent is inconsistent. Fix: per-sensor guards, or document the workflow.

**i2c #4 — per-sensor/OLED/battery flags — `System_Settings.h:30-300, 440-1100` — data, ~450-700 B RAM, IN TODAY'S BINARY.** ~79-91 sensor/display/battery fields (thermal ~20 incl. String, tof/imu/gps/fm/apds/presence/servo/rtc/oled, per-sensor `*Bus` bytes, `batteryLog*`). Fix: P2 with the registries #8/#9 keep-lists.

**i2c #5 — ENABLE_OLED_DISPLAY — `System_Utils.cpp:2870-2883` — data, ~1.2 KB (measured 1231 B), IN TODAY'S BINARY.** The `oled` gCommandModules row + help string — the ONLY unguarded feature-module row. Binds to the stub empty `oledCommands[]`; registration is a count-0 no-op but the strings stay in .rodata. Fix: wrap the row; the SensorStubs empty array covers every other reference.

**i2c #6 — all ten sensor flags — `System_I2C.cpp:216-248` — data, ~1.3 KB, IN TODAY'S BINARY.** `i2cSensors[]` metadata rows for every sensor. Partly deliberate (runtime filter at 88-134; `detect` names unknown chips) — but the MAX17048 row is guarded, showing the table CAN be flag-gated. Fix: per-row `#if`, or embrace the deliberate design and drop the inconsistent MAX17048 guard.

**i2c #7 — ENABLE_I2C_SYSTEM — `System_I2C.cpp` + `System_I2C_Manager.cpp` (4,133 lines) — stripped at level 0.** No whole-file wrap (the flag appears at just 2 internal spots); all known roots verified guarded so a level-0 build should GC everything (unverified by real link). High resurrection risk. Fix: P8.

**i2c #8 — ENABLE_OLED_INPUT — `HAL_Input.cpp:20-238` — stripped.** Input HAL core compiles with no input device selected; guard covers only includes + CLI tail; all 4 caller sites verified dead today. Fix: P8.

**i2c #9 — ENABLE_BATTERY_MONITOR — `i2csensor_max17048.cpp:5` + `System_Battery.cpp:179-288` — stripped.** Driver+backend gate on `BATTERY_BACKEND_FUEL_GAUGE` (board constant) instead of the user-facing monitor flag — force-disabling the monitor on a FeatherS3 still compiles the whole driver. Fix: `#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE`.

**i2c #10 — ENABLE_THERMAL_SENSOR — `WebPage_Sensors.cpp:101-179` — stripped (fragile).** Early-return `#if !ENABLE_THERMAL_SENSOR … return; #endif` then ~76 unguarded thermal lines relying on stubs + DCE. Same shape for tof (180-183), imu (205-208), input (226-229). Fix: real `#if/#else/#endif` around each branch.

**i2c #11 — (no flag exists) — `System_NeoPixel.cpp:24-28, 505-513` + `System_Utils.cpp:2884-2892` — live, ~1.5-2 KB, IN TODAY'S BINARY.** NeoPixel availability is pin-derived only; on the XIAO (pin -1) a dummy `Adafruit_NeoPixel(0,-1)` object, the 3 `led*` commands, the 64-entry color table, and a ~700 B module description all compile and register as live no-ops. Fix: introduce `ENABLE_NEOPIXEL` (default `NEOPIXEL_PIN_DEFAULT>=0`) — P9.

**Domain notes:** all 12 `i2csensor_*.cpp` verified full-file wrapped (pca9685's servoCommands INSIDE its guard); OLED_Utils/HAL_Display structure verified clean; per-sensor gCommandModules entries and settings-module registrations all correctly gated; System_I2C autostart processing has clean per-sensor `#if`s; G2_Page_Sensors + OLED_Mode_Sensors have ZERO unguarded sensor refs. I2C dual-bus is runtime-only even on boards with no I2C2 pins (~1-2 KB always-live in every I2C build) — flag-gap observation. Flag gaps in scope: NeoPixel (#11), the `led` module (`System_Hardware.cpp:89`, always registered), `System_PollPause.cpp` (54 lines, trivial), sensorlog core (flagless by design).

## Domain: Apps — LLM / Automation / Maps / Games

**apps #0 — ENABLE_AUTOMATION — `System_Utils.cpp:329-330, 964-1010, 4623, 4803, 4888` — stripped (adjusted), ~0 B at -O2.** Autolog machinery references `gAutomationLog*` globals defined in CMake-excluded `System_Automation.cpp`. The finder claimed a hard link failure; the verifier **empirically compile-tested with the real xtensa toolchain** and refuted that: the header's stub branch defines a TU-local `static bool gAutomationLogActive = false` which the extern binds to, `-O2` constant-folds the rest, link succeeds (fails only at `-O0`). Residual: two genuinely undefined externs whose linkability depends on optimizer folding + a static-shadows-global wart. Fix (hygiene): move the three definitions to an always-compiled TU or guard the blocks.

**apps #1 — ENABLE_AUTOMATION — `G2_Page_Automations.cpp` (whole 318-line file) + `G2_Glasses.cpp:4647, 4751, ~4879, 6974` + `HardwareOne.cpp:245-247` — live, ~5-7 KB + LINK FAILURE in BT+G2/AUTOMATION=0 builds.** See Tier 0 #4. The adjacent `APP_ROW_LLM` rows ARE under `#if ENABLE_ONDEVICE_LLM` — proving the missing-guard contrast. Fix: extend the file wrap to `&& ENABLE_AUTOMATION` and guard the page-table row + Apps launcher row.

**apps #2 — ENABLE_ONDEVICE_LLM — `System_Settings.cpp:2896, 3104, 3189` — live, ~250 B, IN TODAY'S BINARY.** `llmenabled` editor command + `kStops {"llmenabled","llmunload"}`; registers and always errors (backing row is in CMake-excluded System_LLM.cpp). Fix: P1.

**apps #3 — ENABLE_AUTOMATION — `System_Settings.cpp:3110, 3195` — live in AUTOMATION=0 builds, ~220 B.** `automationautostart` editor command, same broken-command pattern. Fix: P1.

**apps #4 — ENABLE_MAPS / ENABLE_AUTOMATION — `System_Debug.cpp:2091-2094, 2179` — live in MAPS=0 / AUTOMATION=0 builds, ~600 B / ~150 B.** 4 `debugmaps*` rows + `debugautomations` row; the adjacent `debugllm*` group IS wrapped. Fix: P3.

**apps #5 — ENABLE_ONDEVICE_LLM — `WebPage_Dashboard.h:432-439` — data, ~450-550 B, IN TODAY'S BINARY.** Dashboard `if(c.llm){...}` JS updater shipped as string data; the card HTML above it IS guarded, and the `c.llm` JSON key is only emitted under a guarded block (`System_Utils.cpp:1786`) — functionally dead strings. Fix: same `#if` as the card HTML.

**apps #6 — ENABLE_ONDEVICE_LLM (+MQTT/SR) — `WebServer_Utils.cpp:383-400` — data, ~150 B, IN TODAY'S BINARY.** Guest-API allowlist `kAllow[]` carries exactly 7 dead path strings today (5 `/api/llm/*`, `/api/mqtt/status`, `/api/speech/status`). Harmless security-wise (unregistered URI = 404). Fix: per-feature groups inside the table.

**apps #7 — ENABLE_AUTOMATION — `WebPage_Automations.h` (1910 lines) + `WebServer_Server.cpp:44, 1669` — stripped, ~50-70 KB compiled-then-GC'd per AUTOMATION=0 build.** The complete Automations web page string blob. `HardwareOne.cpp:61-63` guards the same include; `WebServer_Server.cpp` doesn't. All callers/routes ARE guarded, so GC saves the flash — this is compile-time + hygiene. Fix: guard the include + `streamAutomationsContent`.

**apps #8 — ENABLE_ONDEVICE_LLM — `System_Filesystem.cpp:1361-1365` — data, ~30-45 B, IN TODAY'S BINARY.** FS permission row for `/system/llm/` matching a directory never created in LLM-off builds (both mkdir sites ARE guarded). Fix: wrap the row.

**apps #9 — ENABLE_ONDEVICE_LLM — `WebPage_Settings.h:995, 1008-1009, 1536-1542, 1891-1892` — data, ~300-400 B, IN TODAY'S BINARY.** Settings-page JS grouping lists/labels naming gated-off apps. Functionally tolerant (page renders from the live schema). Fix: low priority; split per-app strings under their flags or accept.

**Domain notes:** G2 LLM viewer + submenu fully guarded; OLED Apps rows per-app guarded with an explicit "Not compiled" fallback (model hygiene); all boot autoloads, help topics, module registrations, nav links, and route registrations verified guarded; WEB-off/app-on combos degrade to empty TUs, not link breaks. **GAMES is completely clean.** Deliberate: `System_Automation.h`/`System_Maps.h` stub blocks; debug-flag registry rows sanctioned by `docs/DEBUG_FLAG_XMACRO_PLAN.md`; `llmEnabled/llmAutoStart` kept for the feature registry; SYSEVT_LLM_* kinds as stable vocabulary; `CommandContext.automationName[64]` defensive field.

## Domain: Cross-cutting registries (command table / settings / feature registry / boot)

This domain deliberately overlaps the others; unique findings and added precision only.

**reg #0 — per-sensor flags — `System_Settings.cpp:3064-3068, 3092-3100, 3150-3154, 3177-3185` — live, ~2.3 KB, IN TODAY'S BINARY.** (= i2c #1, with the full list) 14 commands: `tofi2cclockhz`, `presencedevicepollms`, `apdsdevicepollms`, `fmradiodevicepollms`, `gpsdevicepollms`, `thermalenabled`, `tofenabled`, `imuenabled`, `gpsenabled`, `fmradioenabled`, `apdsenabled`, `rtcenabled`, `presenceenabled`, `inputenabled`. Every owning SettingEntry verified in gated code with exactly one owner. Fix: P1.

**reg #1 — ENABLE_EDGE_IMPULSE — `System_Settings.cpp:3070-3073, 3109, 3159-3162, 3194` — live, ~0.8 KB, IN TODAY'S BINARY.** 5 EI editor commands (`eirequirelabels`, `eimaxdetections`, `eiinputsize`, `eiinterval`, `eiautostart`). Fix: P1.

**reg #2 — ENABLE_ESP_SR — `System_Settings.cpp:3074-3075, 3103, 3163-3164, 3188` — live, ~0.5 KB, IN TODAY'S BINARY.** 3 SR editor commands. Fix: P1.

**reg #3 — ENABLE_ONDEVICE_LLM — (= apps #2).**

**reg #4 — ENABLE_OLED_DISPLAY — `System_Settings.cpp:137-150, 246, 3108, 3193` + `System_Command.cpp:531-544` — live, ~0.5 KB + a persisted dead JSON key, IN TODAY'S BINARY.** `oledautostart` dead-ends; **`oledclihistorysize` actually WORKS** — `cliSettingsModule` registers unconditionally, so a display-less build parses/persists an OLED-only key in `system.cli`. Fix: P1 + gate the `oledHistorySize` SettingEntry.

**reg #5 — LLM/MQTT/EI/SR/sensor flags — `System_Events.h:129-312` — live, ~0.8 KB+, IN TODAY'S BINARY.** 151-kind SYSEVT X-macro; ~30 kinds belong to off features (verifier: mqtt kinds number 4, not 2), and with BT=0 in the real config the ~13 BLE/G2/RING kinds are dead too (~40+ total off-feature kinds). Fix: P4.

**reg #6 — ENABLE_MQTT — (= wifi #4, adjusted).** Verifier correction that shapes the P2 fix: `&gSettings.mqttEnabled` is read by the deliberately-unconditional FeatureRegistry row and `mqttAutoStart` by the unguarded `RF_MQTT` ramflush case — those 2 fields must stay (or their readers gate with them); the other 21 are pure dead state.

**reg #7 — ENABLE_ESP_SR / ENABLE_EDGE_IMPULSE — (= cam #7/#8, adjusted).** Same keep-list: `srAutoStart` (registry + RF_SR), `eiEnabled` (registry). Only the tuning subsets are pure dead state.

**reg #8 — ENABLE_OLED_DISPLAY — `System_Settings.h:851-869, 256-265, 302, 451` — data, ~100-120 B, IN TODAY'S BINARY (adjusted).** ~13 OLED fields; keep-list is FOUR fields, not one: `localDisplayRequireAuth` (auth stub), `oledEnabled` (registry row), `oledAutoStart` (RF_OLED), `oledCliHistorySize` (actively written by the live command, reg #4). No String heap allocs (all defaults fit SSO). Fix: P2.

**reg #9 — per-sensor flags — `System_Settings.h:467-512, 241-329` — data, ~250 B, IN TODAY'S BINARY (adjusted).** ~50 sensor tuning fields are the genuinely dead subset; the per-sensor `autostart`/`enabled` bools are read by the unconditional FeatureRegistry rows + `RF_*` ramflush switch and must stay (the finder's fix was insufficient). Fix: P2 with this keep-list.

**reg #10 — ENABLE_OLED_DISPLAY — (= i2c #5).** Verifier enumerated all ~23 other module rows as correctly gated — `oled` is the sole exception.

**reg #11 — BT&&G2 — `System_Notifications.cpp:424` + `System_Settings.cpp:3080, 3170` — live, ~0.1 KB + 1 persisted JSON key, IN TODAY'S BINARY.** (= ble #2 row detail) `notifG2` SettingEntry + working `notifydeviceg2` editor command: with BT=0 (today's config, per the verifier's on-disk check) the G2 delivery sinks are compiled out while the setting still registers, parses, persists, and toggles a sink that cannot exist.

**Domain notes:** `gCommandModules[]` and `registerAllSettingsModules()` verified exemplary apart from the listed exceptions; persist/load iterates registered modules only (schema strings for off features DO drop — the leak is struct fields + the editor-command table); FeatureRegistry unconditional-rows design is explicitly commented and sanctioned; HardwareOne boot/loop verified fully gated with all unguarded-looking calls resolving to deliberate stubs; ramflush per-feature restore fully gated. Flag-gap roll-up: `System_UartLink.cpp` (~350 lines/~8 KB + task, board-pin gate only, both boards define the pins — runtime `uartLinkEnabled` is the only gate), NeoPixel/led, sensorlog, power, OTA, crash-record, ramflush, setup wizard — sizable unconditional subsystems with no toggles (by design for most; listed for completeness). Scope question: `even_r1` ring command module registers under bare BT&&G2 — R1_HEALTH gates only the health pages, so a BT+G2/R1=0 build carries every `ring*` command.

---

*Audit run: workflow `wf_8a5bb61f-0e8`, 14 agents (7 find + 7 adversarial verify), 2026-08-07. Report assembled from verifier-merged findings; no code changes were made. (`ENABLE_BLUETOOTH` changed 1→0 mid-session — the user's XIAO board switch; see the config note at top.)*
