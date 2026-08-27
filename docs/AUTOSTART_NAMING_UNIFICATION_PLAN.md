# Autostart naming unification - plan

Status: PLAN ONLY. Nothing implemented. Written 2026-07-25 against commit 889f1b8 (v0.99.1).

## Goal

Every feature gets **two independent, uniformly named controls**:

| Axis | Command | C++ field | jsonKey | Meaning |
| ---- | ------- | --------- | ------- | ------- |
| enable | `<thing>enabled` | `<thing>Enabled` | `<thing>Enabled` | May this subsystem run at all? |
| autostart | `<thing>autostart` | `<thing>AutoStart` | `<thing>AutoStart` | Does it start itself at boot? |

Boot becomes one uniform rule everywhere:

```c
if (gSettings.<thing>Enabled && ramFlushResolve(RF_<THING>, gSettings.<thing>AutoStart)) start<Thing>();
```

This is the shape MQTT already has (HardwareOne.cpp:1800) - the plan generalizes it.

Per the repo's standing rule, **no backwards compatibility**: a clean flag-day rename, no aliases,
no migration shim. The device is erased before flashing.

## Why this is not just a rename

### The two axes already exist - they are just collapsed onto one bit

This is the core justification, and it is stronger than "add a missing setting".

`features <id> on|off` writes `*f->enabledSetting` (System_FeatureRegistry.cpp:608) and
`isFeatureEnabled()` reads that same pointer (:406). For **22 of 23** features that pointer IS the
autostart field. So today:

```
features thermal off     and     thermalautostart off      write the SAME bit
```

The device already offers the user an enable/disable control (`features`, plus the Settings UI rows
that mirror it) and a boot-autostart control - and they are indistinguishable. Disabling a feature
for this session silently also disables it at next boot, and there is no way to express "installed
and available, but do not start it automatically" except for MQTT, which got a second flag by hand.

The `FeatureEntry` comment admits the conflation outright: `bool* enabledSetting; // Pointer to
gSettings.xxxEnabled/AutoStart`.

So this refactor **separates two controls that already exist and are already user-visible**, rather
than inventing a new concept.

### Field-level starting point

| Today | Count | Work |
| ----- | ----- | ---- |
| autostart only | 15 | add `<thing>enabled` |
| enabled only | 7 | add `<thing>autostart` |
| both | 1 (MQTT) | rename only |

Verified by enumerating all 38 `bool` fields in gSettings matching `Enabled|AutoStart` and grouping
by prefix. Five prefix groups contain both an `*Enabled` and an `*AutoStart`, but in four of them the
`*Enabled` is a sub-feature, not a master switch: `thermalInterpolationEnabled` /
`thermalRollingMinMaxEnabled` (image processing), `imuOrientationCorrectionEnabled` (processing),
`httpsEnabled` (protocol choice), `i2cBusEnabled` / `i2c2BusEnabled` (per-bus hardware). Sensors have
autostart ONLY - thermal has 13 setting rows and no enable; GPS has 2.

So this adds ~22 settings and ~22 commands on top of renaming ~24 existing ones across four
surfaces. Roughly 46 settings / 46 commands when done.

### What the user gains

Independent control per feature: keep a subsystem available but off at boot, or disable it outright
without losing its autostart preference. Today those are the same switch.

## The one mechanic that makes this dangerous

`settingsEditorCommandName()` (System_SettingsEditorCore.cpp:152-156) returns `entry->cmdKey` if
non-null, **else `entry->jsonKey`**. `findCommand()` (System_Command.cpp:86-112) lowercases both
sides and prefix-matches.

**For the ~16 rows where `cmdKey == nullptr`, the jsonKey IS the command name.** Renaming such a
jsonKey without adding an explicit `cmdKey` silently unbinds the setting from its command: no
compile error, no runtime error - the row simply stops being editable on the OLED, the G2 lens, and
the web Settings page.

Affected rows (cmdKey absent): `wifiEnabled`, `httpAutoStart`, `mqttClientEnabled`, `mqttAutoStart`,
`oledEnabled`, `ledStartupEnabled`, `automationsEnabled`, `inputAutoStart`, `thermalAutoStart`,
`tofAutoStart`, `imuAutoStart`, `gpsAutoStart`, `fmRadioAutoStart`, `apdsAutoStart`, `rtcAutoStart`,
`presenceAutoStart`, `cameraAutoStart`.

**Rule for this refactor: every touched SettingEntry row gets an EXPLICIT `cmdKey`.** Never rely on
the jsonKey fallback again. This is the single highest-value structural change in the plan.

## Consumer map

### Compiler-checked (safe - a missed rename fails the build)

Three independent pointer tables must stay in sync for every feature. All take raw
`&gSettings.<field>`, so the compiler catches misses:

1. `System_FeatureRegistry.cpp:220-360` - the feature table
2. `System_I2C.cpp:1968-1982` - `sensorHeapCosts`
3. `System_RamFlush.cpp:215-234` - the `ramFlushResolve` switch

Blast radius per field (refs / files):

| Field | Refs | Files | Field | Refs | Files |
| ----- | ---- | ----- | ----- | ---- | ----- |
| espnowenabled | 16 | 12 | oledEnabled | 10 | 5 |
| bluetoothAutoStart | 16 | 8 | thermal/tof/imu/apds/rtc/presence AutoStart | 10 ea | 5 ea |
| inputAutoStart | 16 | 8 | gpsAutoStart | 11 | 5 |
| automationsEnabled | 16 | 7 | fmRadioAutoStart | 10 | 5 |
| httpAutoStart | 14 | 10 | sensorLogAutoStart | 10 | 4 |
| i2cBusEnabled | 13 | 5 | mqttClientEnabled | 8 | 4 |
| mqttAutoStart | 12 | 7 | camera/microphone AutoStart | 8 ea | 4 ea |
| edgeImpulseEnabled | 12 | 3 | llmAutoStart | 6 | 5 |
| systemLogAutoStart | 12 | 4 | led/sr | 4 ea | 4 ea |
| | | | wifiEnabled | 2 | 2 |

### Compiler-INVISIBLE (the actual risk surface)

**Hardcoded command strings on the on-device UIs.** Each is a literal the compiler never sees:

| Site | String |
| ---- | ------ |
| G2_Page_Network.cpp:1199 | `wifiautoreconnect %d` |
| G2_Page_Network.cpp:1467 | `espnowenabled %d` |
| G2_Page_Network.cpp:1508 | `bleautostart %s` |
| G2_Page_Network.cpp:1951 | `httpAutoStart %d` |
| G2_Page_Sensors.cpp:797 | `inputautostart %s` |
| G2_Page_Sensors.cpp:800 | `oledenabled %d` |
| G2_Page_Sensors.cpp:801, :1557 | `sensorautostart ...` |
| OLED_Utils.cpp:6241 | `executeOLEDCommand("espnowenabled 1")` |

**Hand-written JSON emitters using literal keys:** System_ESPNow.cpp:11100 `doc["enabled"]`,
System_Utils.cpp:1664 `espnow["enabled"]`.

**Bare jsonKeys that collide across modules** and work only via section namespacing - `enabled` is
the jsonKey for THREE fields (System_WiFi.cpp:1776, System_ESPNow.cpp:15619,
System_EdgeImpulse.cpp:385) and `autoStart` for two (System_LLM.cpp:2995, System_DebugFlags.h:276).
Prefixing them is a genuine improvement, and changes settings.json layout.

**Duplicate command registration:** `espnowenabled` is registered TWICE - System_ESPNow.cpp:15583
and System_Settings.cpp:223. Both must change together.

**Cross-device schema sync:** OLED_RemoteSettings.cpp:183 builds rows at runtime from a *peer's*
schema. Renames propagate once peers update; mixed-version peers will mismatch until then.

**The web UI is the cleanest surface** - swept all 26 WebPage_*, 6 WebServer_*, and 10
i2csensor_*_web.h files: exactly ONE literal autostart command string exists
(`bleautostart`, WebPage_Bluetooth.h:916). The Settings page is genuinely schema-driven - rendering
and both API handlers (`handleSettingsGet` WebServer_Server.cpp:2505, `handleSettingsSchema` :2564)
read `entry.key` / `entry.label` / `entry.cmdKey` straight from the schema, so labels and keys flow
automatically with no web edits. Three caveats:

1. What IS hardcoded is MODULE names, not setting keys (`sensorModules`/`networkModules`/
   `moduleLabels`, WebPage_Settings.h:663-945). Safe unless module names change - but note the
   precedent that jsonKeys DO get hardcoded here when convenient (:1421 `updates['cameraAutoCapture']`,
   :1170 `espnowKeys = ['cameraSendAfterCapture','cameraTargetDevice']`).
2. `readOnly` is honored ONLY in SchemaPanel (:371). `renderNetworkInput` and the sensors
   `renderInput` ignore it, so a readOnly entry renders as a LIVE checkbox. This matters directly:
   `wifiEnabled` is `readOnly=true` and sits in the network panel, so the dead WiFi flag is currently
   presented to the user as a working control.
3. The big generated bundles (WebPage_Games.h 1.0 MB, WebPage_DarkRoom.h 710 KB) contain zero
   matches - confirmed clean, no need to regenerate them.

**Command names printed to the user as instructions.** These tell the user what to type, so a missed
rename ships a UI instructing people to run a command that no longer exists:

| Site | Text |
| ---- | ---- |
| OLED_Utils.cpp:5290 | `Disabled\nRun: espnowenabled 1\nReboot required` |
| OLED_Mode_Network.cpp:683 | same string, verbatim duplicate |
| OLED_Utils.cpp:5268 | `Disabled\nRun: automation system enable` |

**Duplicate submit sites** - each command is emitted from more than one place:
`micautostart` x3 (G2_Page_Sensors.cpp:799, :1245, G2_Glasses.cpp:4687), `cameraautostart` x2
(:798, :1551), `sensorautostart` x2 (:801, :1557), `automation system enable` x2
(OLED_Mode_Automations.cpp:1001, OLED_Utils.cpp:5853). Note G2_Page_Sensors.cpp has TWO parallel
detail-page paths - `SENSORS_LEVEL_LIVE` (~1376-1385) routes through the `sensorAutostartCmd()`
dispatcher while the legacy `SENSORS_LEVEL_DETAIL` path (~1440-1560) re-implements it inline. Miss
one and the two G2 sensor paths diverge.

**Duplicate command registration:** a global scan finds exactly four duplicated CommandEntry names -
`espnowenabled` (System_ESPNow.cpp:15583 + System_Settings.cpp:223), plus `pendinglist`,
`serialrequireauth`, `voicecancel`. Renaming one row leaves both old and new names live.

**Non-uniform value grammar** - worth fixing in the same flag day: `on|off` for `micautostart`,
`cameraautostart`, `inputautostart`, `sensorautostart`, `bleautostart`; `0|1` for `espnowenabled`,
`oledenabled`, `httpAutoStart`; `enable|disable` for `automation system`.

**A literal cross-module call:** `cmd_gpslog` invokes `cmd_sensorlog("autostart on")` at
i2csensor_pa1010d.cpp:696.

**SAVED AUTOMATIONS - the one consumer that lives outside the repo.** Automations persist a raw
command string and execute it: `String cmdStr = a.value("command");` (System_Automation.cpp:979),
stored under `"command"` in `/system/automations.json`. Any automation whose action is
`micautostart on`, `espnowenabled 1`, `automation system enable`, etc. **breaks silently on rename**
- it fails at fire time, not at save time, and no repo-side grep can find it because the data is on
the user's device.

Normally the no-backwards-compat rule covers this (the device is erased before flashing, taking
automations.json with it). The gap is **backup/restore**: a `.hwbackup` taken from a pre-rename
device carries its automations.json forward through the Migration Tool onto post-rename firmware.
Decide one of: accept it and note it in the changelog; have the restore path rewrite known-renamed
commands; or have automation execution report a clear "unknown command - renamed in vX.Y.Z" instead
of a generic failure. The third is cheapest and helps for typos generally.

The same applies to any automation CONDITION referencing a setting by name.

**Prose comments naming fields** (go stale silently): System_Settings.h:853,
System_FeatureRegistry.cpp:343, System_WiFi.cpp:423, System_Automation.cpp:3630, HardwareOne.cpp:1297
/:1655/:1659/:1681, System_I2C_Manager.h:168/:229, System_BuildConfig.h:908.

**Docs**: 109 hits across tracked files (README.md has zero). `tools/settings_registry.py` is fully
data-driven with ZERO hardcoded autostart names - it parses SettingEntry rows out of the .cpp files -
so renaming a jsonKey or cmdKey propagates into docs/SETTINGS_MATRIX.md automatically. No generator
edits needed for the rename itself. But two blockers:

1. **The matrix is already badly stale.** Committed = 529 lines; regenerated from the current tree =
   418; 339 lines differ. It documents things that no longer exist (`wifienabled` cmdKey,
   `gamepadAutoStart` jsonKey) and misses things that do (`srautostart`, `micautostart`, an entire
   `llmautostart` row). **Regenerate and commit as a separate baseline BEFORE the rename**, or the
   post-rename diff will be unreviewable.
2. **Regenerating today silently deletes the entire 155-row debug drawer** (matrix line 242 onward).
   The uncommitted Debug-flags Phase B/C work moved debug settings into the X-macro table in
   System_DebugFlags.h, and the generator's regex only matches literal SettingEntry rows. Teach the
   generator to expand the X-macro before using it as a baseline.

## Interaction semantics to specify up front

With two switches instead of one, their interaction has to be defined once and applied uniformly, or
each subsystem will decide it ad-hoc. Proposed rules:

| Situation | Behaviour |
| --------- | --------- |
| `<thing>enabled 0` while the feature is RUNNING | Stop it now. "Disabled" should mean not running. |
| `open<thing>` while `<thing>enabled` is 0 | Refuse with a clear reason naming the setting, e.g. `Error: thermal is disabled - run 'thermalenabled 1' first`. Do NOT silently auto-enable. |
| `<thing>autostart 1` while `<thing>enabled` is 0 | Accept and persist, but warn that it will not start until enabled. The preference is not lost. |
| Boot | `enabled && ramFlushResolve(RF_X, autostart)` - one uniform gate. |
| `features <id> off` | Maps to `<thing>enabled 0` (the master switch), NOT autostart. This preserves what the command already appears to promise. |
| `ramflush` restore | Keys off `autostart`, and must still respect `enabled`. |

The refusal path matters most: it is the difference between a user understanding why a sensor will
not start and thinking the hardware is broken. This is the same class of problem as the ESP-NOW
sensor-stream on-ramp gap already recorded for v0.99.1.

## PHASE 0 - DECISIONS RESOLVED (2026-07-25)

All open decisions are now settled. Details in the sections below; this is the summary.

### D1. Canonical noun = the prefix the CLI command family already shares

Tested against all 23 registry entries; yields exactly one answer each, no ties. **Four nouns change**,
everything else only needs casing cleanup:

| Was | Becomes | Why |
| --- | ------- | --- |
| `bluetooth` | `ble` | 17 existing `ble*` commands |
| `microphone` | `mic` | 12 existing `mic*` commands (the C++ symbol is already `micSettingsModule`) |
| `espsr` | `sr` | 40 existing `sr*` commands |
| `edgeimpulse` | `ei` | 20 existing `ei*` commands |

Casing: command + cmdKey all-lowercase; C++ field + jsonKey camelCase. Only `fmradio -> fmRadio` and
`i2c2` have a non-identity camel rendering. `automations` becomes singular `automation`.

Driver nouns stay separate and legitimate: `gamepad` / `anoencoder` remain as DRIVER names
(task/tag names, help modules, `gamepadread`) - a different namespace from the subsystem noun
`input`. The subsystem noun replaces `gamepad` at exactly three sites: System_I2C.cpp:3092,
System_RamFlush.cpp:362, G2_Page_Sensors.cpp:773 (a translation shim that then becomes deletable).

**MQTT `gamepad_*` Home Assistant ids need no action** - they are hand-written literals, structurally
disconnected from the noun.

Also delete the stub command `srenable` (System_ESPSR.cpp:3857) - it always returns "ENABLE_ESP_SR is
a compile-time flag" and sits one character from the new `srenabled`.

### D2. MQTT - keep both flags, rename one field, no semantic change

`mqttClientEnabled -> mqttEnabled` (master gate, live runtime gate - not a boot flag);
`mqttAutoStart` unchanged. **Mandatory companion rename in the same commit:** the file-static
`bool mqttEnabled` (System_MQTT.cpp:79) becomes `mqttClientRunning`, or the file would contain
`mqttEnabled` (running) and `gSettings.mqttEnabled` (permitted) four lines apart with opposite
meanings. 11 sites, one file, every miss is a compile error.

### D3. WiFi - two fields, no reconnect setting, no persisted radiopower

Keep `gSettings.wifiEnabled` but re-mint it as a real gate: jsonKey `enabled -> wifiEnabled`, add
cmdKey `wifienabled`, **drop `readOnly`**, and gate `ensureWiFiInitialized()` (System_WiFi.cpp:1681) -
the single chokepoint for all four STA entry paths. `wifienabled 0` while connected reuses the
existing `cmd_wifidisconnect` body. Rename `wifiAutoReconnect -> wifiAutoStart` throughout. Flip the
registry flag from `FEATURE_FLAG_REQUIRES_REBOOT` to `RUNTIME_TOGGLE` once teardown is real.

### D4. Degenerate axes - five of six get both; i2c is the only deviation

Mechanical test used instead of case-by-case argument: **`autostart` is meaningful iff a runtime
start path exists** - a function reachable from a command that brings the subsystem up after
`setup()` returns. If not, the two flags are one bit wearing two names.

OLED, automations, RTC, LED and edgeimpulse all pass and get both axes. Notable specifics:
- **LED**: re-noun first. `ledenabled` gates `setLEDColor()` (System_NeoPixel.cpp:162) - the single
  chokepoint all commands, effects, boot animation and status colors funnel through.
  `ledautostart` = run the startup effect. This resolves the earlier "ledStartupEnabled is the wrong
  subject" objection.
- **Automations**: implement `openautomations`/`closeautomations` by giving the two ALREADY-DEAD
  functions `resumeAutomationSystem`/`suspendAutomationSystem` (System_Automation.cpp:931-940, zero
  callers tree-wide) a one-line body. Also gates `runAutomationsOnBoot()` (HardwareOne.cpp:1865),
  which is ungated today - a live bug.
- **RTC**: leave `rtcEarlyBootSync()` (HardwareOne.cpp:1402) UNGATED by both flags, with a comment
  saying so. It is the only offline time source; gating it produces 1970-stamped logs with no visible
  cause.

### D5. `<thing>enabled` = admission control, copied from MQTT verbatim

Do not invent semantics - MQTT already implements every row of the interaction table in shipped code.
Copy it: `cmd_mqttclientenabled` (System_MQTT.cpp:1293-1298) for stop-on-disable, `startMQTT()`
(:916-919) for the refusal, `cmd_mqttautostart` (:1302-1313) for persist-only, HardwareOne.cpp:1800
for the boot gate.

For all 9 I2C sensors the stop is one existing call - `handleDeviceStopped(I2C_DEVICE_X)`
(System_I2C.cpp:2291), already the entire body of `close<sensor>`. A disabled subsystem stays VISIBLE
in `features` / `sensors` / `detect` / MQTT / web as present-and-disabled; `detect` already has that
state (`DetectStatus::PRESENT_DISABLED`).

### D6. Guard rail - delete the second resolution path, then check

The highest-value move is not a check at all:

**STEP 0.** Change `System_SettingsEditorCore.cpp:152-156` to `return entry->cmdKey;` - delete the
`?: jsonKey` fallback - and make `controls json` (System_Settings.cpp:2795) emit `cmdKey` explicitly,
fixing its false "also the set command" comment.

After this there is exactly ONE resolution path. A row with no cmdKey renders "No command"
(G2_Page_Settings.cpp:638) instead of silently firing something else: **fail-safe instead of
fail-dangerous.** All 6 documented MISFIREs are jsonKey-lane artifacts whose debug rows already carry
correct cmdKeys - Step 0 fixes them outright, with no checker involved.

**Then** extend `tools/settings_registry.py` with a `check` subcommand (exit 1 on failure) rather than
writing a new tool - it already parses the rows and already needs the X-macro work.

### D7. Sub-verbs, grammar, admin

- `sensorlog autostart` -> **`sensorlogautostart`**; `log autostart` -> **`systemlogautostart`** (NOT
  `logautostart` - ambiguous against sensorlog); `automation system enable|disable` ->
  **`automationenabled`**. Also rename `logcategorytags -> systemlogcategorytags` to close the family.
- **Grammar: `on|off` canonical**, with `1|0`, `true|false`, `enable|disable` accepted as synonyms;
  bare argument reports state and never toggles. No new code - route everything through the existing
  `BOOL_CMD` -> `settingBoolToggle()` -> `parseBoolArg()` path, which already does exactly this.
- **Admin: every `<thing>enabled` / `<thing>autostart` is `requiresAdmin = true`**; runtime
  `open*`/`close*` stay non-admin. Boot policy is administration; turning a sensor on for this session
  is not. 13 rows flip false -> true, none the other way.

## Decide what `<thing>` IS before anything else

`<thing>autostart` is not well-defined until the noun is. The same subsystem is spelled differently
on different surfaces today:

| Subsystem | Command | jsonKey / field | Registry id | Elsewhere |
| --------- | ------- | --------------- | ----------- | --------- |
| microphone | `mic`autostart | `microphone`AutoStart | `microphone` | - |
| bluetooth | `ble`autostart | `bluetooth`AutoStart | `bluetooth` | - |
| speech | `sr`autostart | `sr`AutoStart | `espsr` | - |
| input | `input`autostart | `input`AutoStart | `input` | `gamepad` (System_I2C.cpp:3092, ramFlushIdForModule, MQTT `gamepad_*`) |

Input has **five** spellings in play. Pick one noun per subsystem first; otherwise the refactor
renames the suffix and preserves the drift.

Note the MQTT `gamepad_*` Home Assistant object ids are deliberately frozen for HA back-compat and
must NOT be renamed even if the internal noun changes.

## The highest-severity consumer: `controls json`

`controls json` (System_Settings.cpp:2795) treats the **jsonKey as both the settings key and the
companion app's set-command**, by design and by comment, and nothing validates the coupling.
docs/CONTROLS_WRITE_INTEGRITY.md already measured **243 of 407 entries DEAD and 6 MISFIRES** under
this contract *before* any rename.

A MISFIRE is worse than a DEAD entry: a real command fires against the wrong feature.

**Mandatory mitigation, and arguably worth doing before the rename regardless:** add a boot-time or
build-time assertion that for every non-secret SettingEntry, `findCommand(cmdKey ?: jsonKey)`
resolves. That single guard converts this entire class of silent failure into a loud one, and it is
what makes the rest of the refactor safe to attempt.

## RTC state survives the flag day

`RAMFLUSH_LAYOUT_VERSION` (System_RamFlush.cpp:92) must be bumped if any `RF_*` id is added or
reordered. Otherwise a warm reboot reads stale RTC_NOINIT bit masks against the new layout and
resolves the wrong feature's boot intent. **Erase-before-flash does NOT cover RTC_NOINIT across a
soft reset** - this is the one place the no-backwards-compat rule does not save us.

Related: `ramFlushIdForModule` (System_RamFlush.cpp:352-366) returns `RF_FEATURE_COUNT` on a miss,
which makes `ramFlushMarkAutostartFailed` a silent no-op - literally the v0.99.0 bug. It already
contains a stale name today (`"gamepad"` where everything else says `"input"`), kept in sync only by
its single caller's matching literal at System_I2C.cpp:3092.

## Assumptions checked and REFUTED (no work needed)

- **MQTT discovery topics are not derived from setting or feature names.** Every objectId in
  System_MQTT.cpp:519-604 and :686-733 is a hand-written literal; topics are
  prefix/component/MAC/objectId. No rename can move a published Home Assistant topic or entity id.
- **Automation *conditions* cannot reference settings by name.** (Automation *actions* still store
  raw command strings - see above.)
- **Generated web bundles are clean** - WebPage_Games.h and WebPage_DarkRoom.h have zero matches.

## Other silent-failure sites

- **Archetype seeding** (System_SetupWizard.cpp:35-43 -> :79) consumes feature-id string arrays via
  `getFeatureById` with a nullptr-skip. A stale id means a first-boot user picks "Standard Handheld"
  and gets nothing seeded, with no log line. The same nullptr-skip shape recurs at 13 more
  `getFeatureById` literal sites plus System_I2C.cpp:1086/:1129 and G2_Page_Sensors.cpp:773.
- **`srautostart` has a THREE-site string binding**, none compiler-checked: the CommandEntry name
  (System_Settings.cpp:2745), the `SETTING_EDITOR_CMD` macro literal (:2689), and the SettingEntry
  cmdKey (System_ESPSR.cpp:3918). The macro calls `findSettingByCmdKey(literal)`; if they drift the
  command silently returns "Error: setting not found for this command".
- **`llmAutoStart`'s jsonKey is the bare string `autoStart`**, disambiguated only by its module.

## Normalize while you are in here

- **Admin gating is inconsistent**: `cameraautostart` and `bleautostart` are admin=true; every other
  per-sensor autostart is admin=false.
- **Value grammar is inconsistent** (on/off vs 0/1 vs enable/disable) - see the consumer map.

## Strike from the rename set (verified NOT boot controls)

Four things look like boot flags and are not. Renaming them would assert behaviour that does not
exist:

| Item | What it actually is |
| ---- | ------------------- |
| `debugi2cautostart` | A debug LOG flag, X-macro generated from System_DebugFlags.h:276 |
| `sensorautostart` | A nine-way DISPATCHER over other subsystems' flags (System_I2C.cpp:1969-1981), not a flag |
| `edgeImpulseEnabled` | No boot reader at all - only runtime gates (System_EdgeImpulse.cpp:993, :1812) |
| `ledStartupEnabled` | Gates the boot ANIMATION, not the LED driver (HardwareOne.cpp:1829) |

`sensorautostart` should still be kept as a convenience dispatcher; it just is not itself renamed.

## Live bugs found during the inventory (fix independently of the rename)

These exist today and are worth fixing whether or not the rename proceeds:

1. **The `automationsEnabled` settings row cannot save.** Its SettingEntry has `cmdKey=nullptr`
   (System_Automation.cpp:4111), so the editor derives the command from jsonKey
   `"automationsEnabled"` - and no such command exists. The only CLI writer is the two-level sub-verb
   `automation system enable|disable` (System_Automation.cpp:1955). A top-level `automationautostart`
   / `automationenabled` fixes it.
2. **`espnowenabled` is registered twice** (System_ESPNow.cpp:15583, System_Settings.cpp:223), same
   handler. Three other commands are also double-registered: `pendinglist`, `serialrequireauth`,
   `voicecancel`.
3. **docs/SETTINGS_MATRIX.md is 339 lines out of date** and the generator drops the debug drawer.

## Decisions forced before any renaming

These are not mechanical; each changes what the plan does.

1. **`wifiEnabled` is inert today.** 2 refs, both declarative, `readOnly=true`. No boot code reads it -
   HardwareOne.cpp:1566 gates WiFi on `wifiAutoReconnect`. RESOLVED: **keep the field and re-mint it
   as a real gate** rather than deleting it - see the resolved-decisions section. Do NOT repoint the
   feature registry: under the decided rule (`features <id> off` maps to `<thing>enabled`) the
   registry pointing at the master gate is CORRECT, not a bug. An earlier draft of this plan said to
   repoint it; that was wrong and self-contradictory.
2. **`wifiAutoReconnect` conflates two ideas** - "reconnect after a drop" and "start at boot"
   (System_RamFlush.cpp:233 uses it as RF_WIFI's boot intent). Split into `wifiautostart` (boot) and
   keep a reconnect policy setting, or accept the conflation explicitly.
3. **MQTT: there is no real collision.** RESOLVED: `mqttClientEnabled` is a live runtime gate, not a
   boot flag, so it maps onto `mqttEnabled` and `mqttAutoStart` stays as-is. Do NOT repoint
   System_FeatureRegistry.cpp:245 - it points at the master gate, which is exactly what the decided
   `features` rule requires. An earlier draft said to repoint it; that was wrong.
4. **`ledStartupEnabled` is not a subsystem flag** - it gates the boot LED *effect*, and belongs to a
   family (`ledStartupEffect/Color/Color2/Duration`, System_NeoPixel.cpp:523-526). Renaming just this
   one orphans the prefix. Either leave the family alone, or rename the whole family and give the LED
   subsystem its own separate `ledenabled`/`ledautostart`.
5. **`debugI2CAutoStart` is X-macro generated** (System_DebugFlags.h:276) - field, command and
   jsonKey all emit from one line. Per the repo's DebugSubFlags rule it must be *regenerated*, never
   hand-edited. It is also not a real boot flag; exclude it.
6. **The feature registry needs a second pointer.** `FeatureEntry` has one `bool* enabledSetting`
   whose own comment says "gSettings.xxxEnabled/AutoStart" - it knowingly conflates the axes. The
   two-axis model requires `bool* enabledSetting` + `bool* autoStartSetting`, and `features` output
   should show both.
7. **Sub-verbs get promoted** (your call). There are THREE, not two - the third is
   `automation system enable|disable` (System_Automation.cpp:1955), two levels deep, using
   enable/disable rather than on/off, and conceptually colliding with `automation enable <id>` which
   enables a single automation.

   Promotion is **mechanically collision-free**, verified rather than assumed: `findCommand()`
   (System_Command.cpp:86-119) does whole-word longest-prefix matching, requiring the next character
   to be a space or end-of-string (:109). So `sensorlogautostart` / `systemlogautostart` /
   `automationautostart` are not shadowed by `sensorlog` / `log` / `automation`, and
   `sensorlogautostart` does not collide with the existing `sensorautostart`.

   When promoting, the cmdKey strings and multi-line usage text at System_SensorLogging.cpp:1020/1030
   and System_Debug.cpp:2062 must change in the same commit, and the literal
   `cmd_sensorlog("autostart on")` at i2csensor_pa1010d.cpp:696 must be repointed.

   Note `sensorLogAutoStart` and `systemLogAutoStart` are each simultaneously a gSettings field AND a
   function, called on the same line (HardwareOne.cpp:1973-1974) - the rename must disambiguate both.
8. **Mixed-case command names.** `httpAutoStart` and `httpsEnabled` are the only two; they work
   because lookup is case-insensitive. Normalize to lowercase.

## Defaults that must not be clobbered

The family is overwhelmingly `false`, so a mass rename tends to copy-paste `false`. These are `true`:

`rtcAutoStart` (and its SettingEntry intDefault is `1` while every sibling sensor row is `0`),
`httpAutoStart`, `wifiAutoReconnect`, `ledStartupEnabled`, `i2cBusEnabled`, `automationsEnabled`.
`i2c2BusEnabled` is a board-dependent macro (`I2C2_BUS_ENABLED_DEFAULT`), not a literal.

Silently flipping `rtcAutoStart` to false stops the clock starting, with no compile error.

New `<thing>Enabled` fields should default **true** (feature available unless disabled); new
`<thing>AutoStart` fields inherit the existing autostart default so behaviour does not change.

## Execution order

Do it in this order so the compiler catches as much as possible before the string surfaces.

- **Phase 0 - decisions.** Resolve the noun per subsystem, then the 8 items above. Nothing else
  starts until these are settled.
- **Phase 1 - guard rail FIRST.** Add the `findCommand(cmdKey ?: jsonKey)` resolution assertion, give
  every SettingEntry row an explicit `cmdKey`, and add `autoStartSetting` to `FeatureEntry`. No
  renames yet. Build green. This phase is independently valuable: it turns the entire silent-failure
  class loud, and would surface the 243 already-DEAD `controls json` entries as a side effect.
  Regenerate and commit docs/SETTINGS_MATRIX.md as a clean baseline here too (after teaching the
  generator to expand the debug X-macro).
- **Phase 2 - C++ fields.** Rename `gSettings` fields + initializers, and fix the three pointer
  tables. Fully compiler-checked. Watch the `#if`-guarded duplicates.
- **Phase 3 - add missing flags.** Introduce the ~22 new fields/settings/commands so every feature
  has both axes. Unify the boot gate to `enabled && autostart`.
- **Phase 4 - command + jsonKey renames.** The string surfaces. Do the on-device UI literals in the
  same commit as their command, since nothing will catch a mismatch.
- **Phase 5 - docs.** Regenerate SETTINGS_MATRIX.md; update USERGUIDE/QUICKSTART/WEB_API_INVENTORY.

## Verification

- **A green FeatherS3 build does NOT prove this refactor.** `inputAutoStart` appears twice under
  `#if ENABLE_ANO_ENCODER/#else` (System_I2C.cpp:1976/:1978); `automationsEnabled` is
  `#if ENABLE_AUTOMATION`-guarded in all three of declaration, initializer and registry; and
  fmRadio/thermal/tof/imu/apds are compiled out on FeatherS3. Build the ANO and gamepad variants and
  an `ENABLE_AUTOMATION=0` build before calling it done.
- Grep sweep for stale literals after Phase 4: every old command name must return zero hits outside
  CHANGELOG.md.
- Boot each build once and confirm `features` reports both axes, and that a `ramflush` round-trip
  restores the same set (this refactor touches exactly the tables that caused the shipped
  v0.99.0 ramflush bug, where it checked setting names that did not exist).
- Confirm every renamed setting is still editable from the OLED settings editor, the G2 interactive
  settings editor, and the web Settings page - that is what the explicit-cmdKey rule protects.
