# README / QUICKSTART / USERGUIDE accuracy audit — 2026-07-26

Scope: `README.md`, `docs/QUICKSTART.md`, `docs/USERGUIDE.md` at v0.99.3, checked
against the actual source tree (command registries, `System_BuildConfig.h`,
`CMakeLists.txt`, `boards/*.defaults`, web route table, OLED/G2 menu tables).

Method: command names extracted from every `CommandEntry …[] = {…}` array
(896 registered commands across 27 modules); build flags read from
`System_BuildConfig.h`; routes from `.uri =` registrations; menus from
`oledMenuCategories[]` / `g2RegisterPage()` / `g2AppsBuildRows()`.

Findings are ranked: **wrong** (a reader following the doc gets a failure) sits
above **missing** (a reader never learns the feature exists).

**Status:** **fixed** — Tier 1 items 1.2-1.3, 1.5-1.8, 1.10; all of Tier 2; all of
Tier 3; Tier 4 (now generated). **Open** — 1.1 (`HW_BOARD`), 1.4 (MQTT CLI
section), 1.9 (README hardware claims), Tier 5 (build-config table), Tier 6.

A registry sweep (2026-07-26) additionally found and fixed two dead command
invocations in firmware and left three duplicate registrations documented — see
*Registry drift* at the end.

---

## Tier 1 — Wrong, and it costs the reader a build or a bricked flash

### 1.1 `HW_BOARD` is never mentioned anywhere — flashing a XIAO by the QUICKSTART produces a FeatherS3 image

`CMakeLists.txt:47-115` selects a board file from `boards/` via the `HW_BOARD`
environment variable, and **defaults to `feathers3` for any esp32s3 target** and
`qtpy_esp32` for any esp32 target. The board file sets the Arduino variant,
PSRAM mode, flash size, and (through flash size) the partition table.

QUICKSTART §3 tells the reader only:

```
idf.py set-target esp32s3    # XIAO ESP32-S3 or QT PY ESP32-S3
```

A XIAO owner who follows that gets `CONFIG_ARDUINO_VARIANT="um_feathers3"`,
`CONFIG_SPIRAM_MODE_QUAD`, and a 16 MB partition table on an 8 MB board — wrong
pin map, wrong PSRAM mode, wrong partitions. `boards/feathers3.defaults:20-23`
documents the failure mode for the PSRAM half: *boots fine but reports 0 KB
PSRAM*, so the reader gets silent runtime OOM rather than a build error.

The correct invocation is in the CMakeLists comment and in no user-facing doc:

```bash
HW_BOARD=xiao_s3 idf.py set-target esp32s3 && idf.py fullclean && idf.py build
```

Valid values: `feathers3`, `xiao_s3`, `qtpy_esp32`, `feather_esp32_v2`.

**Fix:** add `HW_BOARD` to QUICKSTART §3 as a required step with the board
table, and to the USERGUIDE Board Reference.

### 1.2 "ESP32-S3 uses Octal SPI PSRAM" is wrong for the primary board — **FIXED**

- QUICKSTART:110 — "PSRAM mode: ESP32 uses Quad SPI PSRAM at 40 MHz; ESP32-S3 uses Octal SPI PSRAM at 80 MHz. Using the wrong mode will cause a boot failure or crash."
- USERGUIDE:70 — Board Reference table, "PSRAM | Quad SPI, 40 MHz | Octal SPI, 80 MHz"

PSRAM **mode** is per-board, not per-chip-family. From `boards/`:

| board | mode |
| --- | --- |
| `feathers3` | `CONFIG_SPIRAM_MODE_QUAD` |
| `xiao_s3` | `CONFIG_SPIRAM_MODE_OCT` |
| `qtpy_esp32` | `CONFIG_SPIRAM_MODE_QUAD` |
| `feather_esp32_v2` | `CONFIG_SPIRAM_MODE_QUAD` |

Only **speed** is per-family (`sdkconfig.defaults.esp32s3` → 80M,
`sdkconfig.defaults.esp32` → 40M). The FeatherS3 — the primary board — is quad,
so both docs currently tell FeatherS3 owners to configure the mode that
`boards/feathers3.defaults` explicitly warns produces 0 KB PSRAM.

Compounding it: the docs frame this as something the reader must set by hand
via `menuconfig`, when `HW_BOARD` layering already sets it correctly.

### 1.3 `useradd`'s `[0|1]` is not the admin flag — **FIXED**

USERGUIDE:729 — `useradd <user> <pass> [0|1]     - Create user (1 = admin)`

Actual (`System_User.cpp:3396-3398`):

```
useradd <username> <password> [0|1] [guest|user|admin|superadmin]
  0|1:  1 = require a new password on next login (default 0)
```

A reader following the doc to create an admin instead creates an ordinary user
*and* silently arms a forced password change on that account's first login.

### 1.4 The whole MQTT CLI section is commands that do not exist

USERGUIDE:317-327 documents `mqtt broker`, `mqtt port`, `mqtt user`,
`mqtt pass`, `mqtt topic`, `mqtt connect`, `mqtt disconnect`, `mqtt status`.
There is no `mqtt` command in the registry. The real names are `mqttHost`,
`mqttPort`, `mqttUser`, `mqttPassword`, `mqttBaseTopic`, `openmqtt`,
`closemqtt`, `mqttstatus` — and the USERGUIDE's own Command Reference block
(lines 548-573) already lists them correctly. The two sections contradict.

### 1.5 ESP-NOW metadata commands are documented with a space that isn't there — **FIXED**

USERGUIDE:128-131 — `espnow setname`, `espnow setroom`, `espnow setzone`,
`espnow settags`. None exist. Real: `espnowsetname`, `espnowroom`,
`espnowzone`, `espnowtags` (note: `room`/`zone`/`tags`, not `setroom`/…).
Same defect at USERGUIDE:117 — `espnow pair` should be `espnowpair`.

### 1.6 `battery status` / `battery calibrate` do not exist — **FIXED**

USERGUIDE:860-861. Real: `batterystatus`, `batterycalibrate` (plus an
undocumented `batterylog`). The `"battery"` / `"status"` strings in the registry
entry are the *voice* category/target fields, not a CLI subcommand.

### 1.7 The gamepad command family was replaced by the input family — **FIXED**

USERGUIDE:1190-1194 documents `opengamepad`, `closegamepad`,
`gamepadautostart`. All three are gone. The Seesaw gamepad and the ANO encoder
now share a device-agnostic layer (`HAL_Input.cpp:315`): `openinput`,
`closeinput`, `inputautostart`, `inputdevicepollms`. Only `gamepadread`
survives from the old set.

> **Side finding, not a docs issue:** `i2csensor_seesaw_oled.h:149` still calls
> `executeOLEDCommand("opengamepad")` and `:23` still prints `Use 'opengamepad'`.
> Those are dead command names — the OLED gamepad screen's open action fails.
> Flagged for separate triage; not touched by this audit.

### 1.8 Wrong flag name: `ENABLE_ESPSR` — **FIXED**

USERGUIDE:934 — "requires `ENABLE_ESPSR`". The flag is `ENABLE_ESP_SR`
(`System_BuildConfig.h:293`). `ENABLE_ESPSR` appears nowhere in the tree.

### 1.9 README lists hardware that has no driver

- **AMG8833 8×8 thermal camera** — README:73 feature row and README:118 product
  link. The string `AMG8833` does not appear anywhere in the source tree. The
  only thermal driver is `i2csensor_mlx90640.cpp`.
- **TEA5767 FM radio** — README:80 and README:123 (product 1712). No `TEA5767`
  in the tree; the driver is `i2csensor_rda5807.cpp` (RDA5807). The USERGUIDE
  gets this right at line 57 — README and USERGUIDE disagree with each other.
- **QT Py ESP32-S3** — README:131-132 lists it as a camera and PDM-mic host;
  QUICKSTART:79/91 names it too. There is no `ARDUINO_..._QTPY_ESP32S3_...`
  branch in the `System_BuildConfig.h` board chain (lines 770-957: QT Py ESP32,
  Feather ESP32 V2, XIAO S3 / S3 Sense, FeatherS3, generic ESP32) and no
  `boards/qtpy_esp32s3.defaults`. It is not a supported board.

### 1.10 QUICKSTART's first-boot wizard walkthrough does not match the wizard — **FIXED**

QUICKSTART:122-143 describes two setup modes and "seven pages".

Actual modes (`System_FirstTimeSetup.cpp:406-420`): **three** — Basic, Advanced,
and **Import from Backup** (undocumented).

Actual pages (`System_SetupWizard.h:18-28`) — nine, several conditional:
Features, **Web mode (HTTP/HTTPS)**, **Bluetooth mode (Server/G2)**, Sensors,
Network, System, **ESP-NOW identity**, **MQTT**, WiFi.

The doc invents two pages that don't exist (**"Device name"** is a field on the
Network page, not a page; **"Web UI theme"** is not in the wizard at all) and
omits four that do.

---

## Tier 2 — Structurally stale: sections describing a UI that has been reorganized

### 2.1 USERGUIDE "OLED Interface" describes the pre-reorg menu — **FIXED**

Documented (USERGUIDE:103-108): Network, Sensors, System, Settings, Logging, Power.

Actual (`OLED_Utils.cpp:4803-4810`), six categories:

| Category | Contents (`oledMenuCategory0-5`) |
| --- | --- |
| **System** | Status, Memory, Perf, Notifs, CLI Output, CLI Input, Logging |
| **Config** | Settings, Login, Logout, Change PW, Users, Gamepad PW |
| **Connect** | Network, Bluetooth, Bond, Web |
| **Hardware** | Sensors, Microphone, Speech, I2C Scan, LED |
| **Apps** | ESP-NOW, Files, Maps, LLM Chat, Automations, Health |
| **Power** | Power |

The section also still says the OLED wizard configures "WiFi, device name, room,
and zone" — see 1.10.

### 2.2 USERGUIDE "Web UI" is missing more than half the pages — **FIXED**

Documented: Sensors, ESP-NOW, **Pair**, Maps, Bluetooth, MQTT, Settings, LLM, CLI.

Actual routes (from `.uri =` registrations): `/dashboard`, `/sensors`,
`/espnow`, `/bond`, `/automations`, `/files`, `/logging`, `/maps`,
`/bluetooth`, `/mqtt`, `/settings`, `/llm`, `/cli`, `/battery`, `/r1-health`,
`/speech`, `/games` (or `/darkroom`), `/login`, `/register`.

Undocumented: **Dashboard, Automations, Files, Logging, Battery, R1 Health,
Speech, Bond, Games**. And there is no `/pair` route — pairing lives inside the
ESP-NOW page, so the documented "Pair tab" does not exist.

### 2.3 The G2 lens UI is essentially undocumented — **FIXED**

The USERGUIDE has a 12-line `g2` command block and an Apps → Health note. The
lens actually carries a full six-category menu mirroring the OLED
(`G2_Glasses.cpp:6172-6209`): **System / Config / Connect / Hardware / Apps /
Power**, with ~20 registered pages behind it (Status, Settings, Tests, Files,
ESP-NOW, Automations, Users, System Events, Logging, LED, FM tuner, LLM menu,
Camera settings, Mic detail, Maps, Pet, Health).

`g2AppsBuildRows()` alone yields ESP-NOW, Files, Maps, LLM, Automations, Health,
Pet — of which only Health is described.

Related: **38 of 50 `g2*` commands** are undocumented, including the whole
`g2ai*` family, `g2notify` / `g2nativenotify`, `g2map`, `g2files`, `g2sensors`,
`g2settings`, `g2network`, `g2battery`, and the `g2mic*` set.

---

## Tier 3 — Whole shipped subsystems absent from all three docs — **FIXED**

Added: USERGUIDE sections *Users, Roles & Access* / *Notifications* / *Backup &
Restore*; ANO encoder across all three docs; R1 Health + a *Wearable Companion*
configuration in README and QUICKSTART; games, HTTPS, maps and the missing core
commands (`ramflush`, `factoryreset`, `perftop`, `bootcount`, `deepsleep`).

Keyword sweep across README / QUICKSTART / USERGUIDE (0 hits in all three
unless noted):

| Subsystem | Evidence it ships | Doc coverage |
| --- | --- | --- |
| **Super Admin tier** | `System_User.h:90`, `CommandEntry.requiresSuperAdmin`, `userpromote … superadmin` | none |
| **Guest role (view-only)** | `System_User.h:91-93`, `hw.isGuest()` in web pages | none |
| **4-tier role model** | `roleRank()`, `guest\|user\|admin\|superadmin` | USERGUIDE still shows the 2-tier `[0|1]` model |
| **Notification system** | `System_Notifications.cpp/.h`, per-kind device levels + per-user mutes, `notify*` settings | 2 incidental mentions in USERGUIDE, no section |
| **ANO rotary encoder** | `i2csensor_ano_encoder.cpp`, `INPUT_DEVICE_TYPE 2`, 5 `anoencoder*` commands | none — README/QUICKSTART assume a gamepad throughout |
| **R1 smart ring / R1 Health** | `G2_Ring.cpp`, `G2_Health.cpp`, `ENABLE_R1_HEALTH`, `/r1-health`, headline of v0.99.3 | USERGUIDE covers it well; **README and QUICKSTART: zero mentions** |
| **G2 Pet app** | `G2_Pet.cpp`, Apps → Pet | USERGUIDE 7 mentions; README/QUICKSTART none |
| **Games / A Dark Room** | `ENABLE_GAMES`, `WebPage_Games.cpp`, `WebPage_DarkRoom.cpp`, `/games`, `/darkroom` | none in any doc |
| **Backup / restore** | `/api/backup`, `/api/restore`, wizard "Import from Backup" | 1 incidental USERGUIDE hit |
| **`factoryreset`, `ramflush`, `perftop`, `bootcount`, `deepsleep`** | `commands[]` in `System_Utils.cpp:2554` | none |
| **Second I2C bus (FeatherS3[D])** | `i2c2busenabled`, `i2c2sdapin`, `i2c2sclpin`, per-device bus commands | README notes "dual STEMMA QT"; no commands documented |
| **Session / ban management** | `sessionlist`, `sessionrevoke`, `ban`, `banuser`, idle-timeout settings | USERGUIDE lists the commands; no concept section |
| **Capture tree layout** | `CAPTURE_DIR` = `/logging_captures` vs `/system/sys_logs` | none — reader can't find their own logs |

---

## Tier 4 — Command Reference completeness — **FIXED (generated)**

Resolved by generating the list instead of maintaining it: `tools/command_registry.py reference` writes `docs/COMMAND_REFERENCE.md` (893 commands, 44 modules) straight from the `CommandEntry` tables, and `tools/command_registry.py audit` reports registry drift with a non-zero exit for CI. The USERGUIDE section is retitled as a curated tour and points at the generated file.

**437 of 896 registered commands appear nowhere in the USERGUIDE.** Worst
offenders by module:

| Module | Undocumented / total |
| --- | ---: |
| `debugCommands` | 142 / 171 |
| `espNowCommands` | 66 / 118 |
| `settingEditorCommands` | 55 / 55 |
| `g2Commands` | 38 / 50 |
| `llmCommands` | 24 / 30 |
| `cameraCommands` | 18 / 49 |
| `i2cCommands` | 17 / 33 |
| `espsrCommands` | 10 / 45 |
| `bluetoothCommands` | 7 / 19 |
| `commands` (core) | 7 / 25 |
| `wifiCommands` | 6 / 20 |
| `anoEncoderCommands` | 5 / 5 |
| `inputCommands` | 4 / 4 |
| `mapsSettingCommands` | 3 / 3 |
| `batteryCommands` | 3 / 3 |

Notes on the big ones:

- **Debug flags.** The USERGUIDE table lists 28 flags. There are 171 debug
  commands, now organized as a generated X-macro table with parent/sub-flag
  banks (`debugllm` → `debugllmload` / `debugllmtokenizer` / …). Neither the
  parent/sub structure nor the `debugflags` command (which prints the live set)
  is documented. Recommendation: stop enumerating flags by hand — describe the
  parent/sub model and point at `debugflags`.
- **`settingEditorCommands` (55/55).** Every per-setting mutator
  (`wifienabled`, `thermalenabled`, `notifylevel`, `sessionidleweb`, …) is
  undocumented. This is the surface v0.99.3's enabled-vs-autostart split
  introduced, so it is the newest and least-covered area.
- **ESP-NOW.** The undocumented two-thirds includes the entire
  discover→confirm pairing flow (`espnowpairmode`, `espnowdiscovered`,
  `espnowaccept`, `espnowreject`, `espnowpairrequest`, `espnowforget`),
  `espnowchannel`, sessions/subs, saturation counters, and mesh backup/failover.
- **`micsource`** — the unified PDM/G2 mic source selector, undocumented.

---

## Tier 5 — Build-config table drift (USERGUIDE §Build Configuration)

The `Default` column no longer matches the shipped `System_BuildConfig.h`:

| Flag | Doc says | File says |
| --- | :---: | :---: |
| `ENABLE_BLUETOOTH` | 0 | **1** |
| `ENABLE_G2_GLASSES` | 0 | **1** |
| `ENABLE_MQTT` | 1 | **0** |
| `ENABLE_ONDEVICE_LLM` | 1 | **0** |
| `ENABLE_CAMERA_SENSOR` | 0 | auto (XIAO Sense → 1) |
| `ENABLE_MICROPHONE_SENSOR` | 0 | auto (XIAO Sense → 1) |
| `ENABLE_BATTERY_MONITOR` | 0 | auto per board (commented out) |

Root cause: the file's values are the *current working FeatherS3 build*, not a
canonical default set — the file itself says `CURRENT: FeatherS3` in several
places. Recommendation: retitle the column **"Shipped value"**, or drop it and
describe each flag's auto-derivation instead.

Flags entirely absent from the table:

`ENABLE_HTTPS`, `ENABLE_MAPS`, `ENABLE_GAMES`, `ENABLE_WEB_GAME_MAZE`,
`ENABLE_WEB_GAME_DARKROOM`, `ENABLE_ESP_SR`, `INPUT_DEVICE_TYPE`,
`XIAO_ESP32S3_SENSE_ENABLED`, `ENABLE_MICROPHONE` (the derived source-agnostic
gate), the three `CUSTOM_ENABLE_NET_*` flags, all ten `CUSTOM_ENABLE_WEB_*`
page gates, `G2_ICON_ANIMATIONS_VFS_PATH`, and the `CAPTURE_DIR*` tree.

Also missing from the sensor sub-table: `CUSTOM_ENABLE_GAMEPAD` is listed but
`INPUT_DEVICE_TYPE` (which actually picks gamepad vs ANO encoder) is not, and
`CUSTOM_ENABLE_FM_RADIO` correctly says RDA5807 while README says TEA5767.

---

## Tier 6 — Smaller corrections

| Location | Issue |
| --- | --- |
| USERGUIDE:31 | `DISPLAY_TYPE` lists ST7789/ILI9341 as equal options. `HAL_Display.cpp` has branches for both, but ILI9341 is labelled `PLACEHOLDER` (`:115`) and the whole `OLED_Mode_*` UI is SSD1306-shaped. Mark these as untested/partial rather than supported |
| USERGUIDE:388 | `debugespnow` described as "alias of `debugespnowcore`" — confirm still true under the X-macro table |
| USERGUIDE:1198+ | Per-Module Notes omit RTC, presence, servo, camera, mic, LED, input |
| USERGUIDE:1217 | "FM Radio (RDA5807) … `fmradio tune <MHz>`" — real command is `fmradiotune` (no space) |
| USERGUIDE:82 | "web server must be running (`webstart` or `webauto on`)" — neither command exists; real: `openhttp` / `httpAutoStart 1` |
| QUICKSTART:146 | same phantom commands: "Run `webstart` … or `webauto on`" |
| README:19 | Board list says "and several other ESP32 / ESP32-S3 boards" — the chain supports exactly 4 named boards + a generic `ESP32_DEV` fallback |
| README:53-87 | Feature matrix has no row for R1 ring/Health, ANO encoder, HTTPS, SD card, maps, sensor logging, notifications, or the role model |
| QUICKSTART:53 | ESP-IDF v5.5.1 pin — verify against current `sdkconfig` / vendored arduino-esp32 before the next release |

---

## Recommended order of work

1. **Tier 1.1 + 1.2** (`HW_BOARD`, PSRAM mode) — these actively break a first
   build on any board except FeatherS3. Highest value per line changed.
2. **Tier 1.3-1.8** — mechanical find/replace of phantom commands; ~20 lines.
3. **Tier 1.9-1.10** — README hardware claims and the QUICKSTART wizard walkthrough.
4. **Tier 2** — rewrite the OLED, Web UI, and G2 sections against the menu tables.
5. **Tier 3** — add sections for roles/permissions, notifications, input devices,
   R1 Health (README/QUICKSTART), games, backup/restore, capture tree.
6. **Tier 5** — retitle/repair the build-config table, add the missing flags.
7. **Tier 4** — Command Reference. Given 437 gaps and a registry that changes
   every release, consider generating this section from the `CommandEntry`
   arrays rather than maintaining it by hand.


---

## Registry drift (sweep, 2026-07-26)

Run `python3 tools/command_registry.py audit` to reproduce. Registration wiring
is clean: no orphaned `CommandEntry` arrays, no module pointing at a missing
array, and all 276 `SettingEntry` `cmdKey` bindings resolve to real commands.

### Fixed — dead command invocations in firmware

Both were user-visible no-ops, not doc problems:

1. `i2csensor_seesaw_oled.h:147-149` — the OLED Gamepad screen's X-button toggle
   called `closegamepad` / `opengamepad`, removed when the gamepad and ANO
   encoder moved behind the shared `HAL_Input` layer. Both branches silently did
   nothing. Now `closeinput` / `openinput`.
2. `OLED_Mode_Network.cpp:494` — selecting a saved network on the OLED called
   `wificonnect --index <N>`. No such command: `wificonnect` is only the
   handler's C function name (`cmd_wificonnect`); the registered name is
   `openwifi`. The UI popped back to the WiFi menu as if the connect had been
   kicked off, so the failure was invisible. Now `openwifi --index <N>`.

### Open — three duplicate registrations

`registerCommand()` appends without deduplication, and `findCommand()` prefers
strictly-longer matches, so for an equal-length duplicate the **first**
registered entry wins and the second occupies a dead registry slot. All three
pairs share a handler and permission level, so behaviour is identical — the
drift is two help-page entries with differing syntax hints, plus a wasted slot
(registry is at 897 of `MAX_COMMANDS` 1024).

| Command | Wins (module order) | Dead copy | Divergence |
| --- | --- | --- | --- |
| `espnowenabled` | `espNowCommands` | `settingsCommands` | help wording only |
| `pendinglist` | `commands` (system) | `userSystemCommands` | dead copy documents `json`, winner does not |
| `serialrequireauth` | `settingsCommands` | `userSystemCommands` | winner says `<0|1>`, dead copy says `[on\|off]` |

The handlers accept both `on`/`off` and `1`/`0`, so neither help text is wrong.
Recommendation: delete the dead copy in each pair and fold any better wording
into the survivor — `pendinglist` in particular loses its `json` note. Left
alone here because removing a registration also moves the command's help page,
which is an organizational call.

> **Not drift:** `voicecancel` is deliberately registered twice in
> `espsrCommands` to give it two voice phrases (`cancel` and `nevermind`);
> `CommandEntry` has no alias list. The audit tool allow-lists it.
