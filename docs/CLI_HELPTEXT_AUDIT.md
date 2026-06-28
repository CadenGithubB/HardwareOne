<!-- Project-wide CLI help-text vs behavior audit. Verified findings, code-cited. espnow audited separately (docs/ESPNOW_HELPTEXT_AUDIT.md). -->

# CLI Help/Behavior Mismatch Audit — Final Deliverable

## Summary

**Total findings: 122** across **22 modules** (ESP-NOW excluded — audited separately).

| Module | Findings | Module | Findings |
|---|---|---|---|
| debug | 79 | thermal | 3 |
| espsr | 7 | mic | 4 |
| wifi | 6 | bluetooth | 4 |
| apds | 5 | oled | 4 |
| imu | 6 | camera | 5 |
| feature | 4 | system | 7 |
| led | 3 | settings | 2 |
| presence | 2 | rtc | 2 |
| neopixel | 2 | battery | 2 |
| image | 2 | gps | 2 |
| fmRadio | 2 | sd | 2 |
| edgeImpulse | 2 | mqtt | 2 |
| filesystem | 2 | thermal/tof/gamepad/anoEncoder/automation/userSystem/mapsSetting/sensorLogging/setPattern/servo | 1 each |

*(Counts above reflect the verified-findings set; per-module sections below are authoritative.)*

### Dominant cross-cutting patterns

1. **Undocumented `[temp|runtime]` arg on debug toggles (~70 commands).** A large family of `debug*` sub-flag commands route through `cmd_debugsubflag_impl` (or bespoke twins) which accept an optional second token `temp`/`runtime` for a non-persistent toggle. The usage strings list only `<0|1>`, while a documented sibling set (`debughttp`, `debugllm`, `debugmicrophone`, …) does show `[temp|runtime]`. This is the single largest pattern, all Low severity.

2. **Undocumented `json` output flag (~25 commands).** Status/read commands across nearly every sensor and subsystem module accept a word-boundary `json` token (via `argWantsJson`) that switches to a machine-readable JSON return, but carry no usage string or omit the flag.

3. **Stale/inert (stored-but-never-read) settings (~13 commands, several High).** Settings that are written and persisted but never consumed: camera auto-capture/send-after-capture suite, OLED boot mode/duration, `thermalWebMaxFps`, `webCliHistorySize`/`oledCliHistorySize`, plus dead debug streams (`debugi2cbus`, `debugi2cdiscovery`).

4. **Role/auth overclaims & gating bugs (4 commands, incl. High).** `wifigettxpower` is a non-admin alias of an admin setter (privilege bypass); `cpufreq` gates its read path behind admin; `setgamepadpassword` has an undocumented OLED-login precondition.

5. **Wrong reply-sink / "Says OK but…" (several Med).** Commands whose help promises a return value or a side effect that doesn't happen as described: `thermalread`/`sddiag` broadcast instead of returning, `imagesend` returns a different string than documented, `eicontinuous`/`fmradiounmute`/`ledeffect` execute the wrong thing.

6. **References to non-existent commands (5 commands).** User-facing strings point at `wificonnect`, `wifiinfo`, `apdscolorstart`, `apdsproximitystart`, `apdsgesturestart`, `rtc` — none of which are registered.

7. **Dev-history tags in help (2 commands).** `setmicsource` ("Phase 2B:") and the `debugespnow` "(parent flag)" mislabel.

8. **Arg-signature drift (many).** Usage strings narrower (or wider) than what the parser accepts; range bounds advertised in usage not enforced by the handler.

---

## debug

By far the largest module. The dominant theme is the undocumented `[temp|runtime]` second arg; two stale-dead debug streams and three behavior bugs sit above it in severity.

### Medium

**`log autostart` — `[on|off]` syntax is fictional; always toggles**
- **Says:** `autostart [on|off]: Toggle logging auto-start on boot (bare = toggle)` — implying `log autostart on`/`off` set the state explicitly.
- **Actually:** The branch never reads `ca.arg(1)` and unconditionally inverts the current value, so `log autostart on` while already ON *disables* it. The no-arg help at :2165 is correct (`autostart: Toggle …`).
- **Evidence:** System_Debug.cpp:2998 (usage), System_Debug.cpp:2356-2361 (toggle-only handler).
- **Fix:** Drop `[on|off]` so it reads `autostart: Toggle logging auto-start on boot`, OR parse on/off to set explicitly and only toggle when bare.

**`loglink` — usage understates accepted tokens and hides status mode**
- **Says:** `Usage: loglink <0|1>`.
- **Actually:** Also accepts `on`/`off`/`true`/`false` and a bare no-arg status-query form; its own error string advertises `<0|1|on|off>`.
- **Evidence:** System_Debug.cpp:2926 (usage), System_Debug.cpp:1705-1712 (status branch + wider token set).
- **Fix:** `Usage: loglink [<0|1|on|off>]  (bare = show status)`.

**`debugespnow` — labelled "(parent flag)" but is a duplicate of `debugespnowcore`**
- **Says:** Table description `Debug ESP-NOW (parent flag).`, implying it lights up the ESP-NOW sub-categories (core/router/mesh/topo/encryption/metadata/stream) like the real camera parent.
- **Actually:** It toggles only the single `DEBUG_ESPNOW_CORE` bit — the exact bit `debugespnowcore` sets. There is no bare `DEBUG_ESPNOW` parent bit, and router/mesh/topo/etc. gate on their own single bits with no `DEBUG_ESPNOW_CORE |` OR (unlike the camera macros that DO OR the parent). So `debugespnow 1` enables only core messages; a user expecting mesh/router diagnostics sees nothing.
- **Evidence:** System_Debug.cpp:1108 (same bit as core at :1657); table :2830 vs sibling :2915; router gating single-bit at System_ESPNow.cpp:1813; contrast camera parent macro System_Debug.h:568.
- **Fix:** Drop "(parent flag)"; reword to `Debug ESP-NOW core messages (alias of debugespnowcore).` — or make it a real parent by OR-ing `DEBUG_ESPNOW_CORE` into the router/mesh/topo/stream macros.

**`debugi2cbus` — advertises a debug stream that emits nothing**
- **Says:** `Debug I2C bus lifecycle, polling pause/resume, status bumps.`
- **Actually:** `DEBUG_I2C_BUSF` macro exists but has **zero call sites** repo-wide and the flag is never read directly; enabling produces no output.
- **Evidence:** System_Debug.cpp:2898 (row), :1073 (handler); macro defined only at System_Debug.h:541 (0 call sites); flag appears outside System_Debug only at System_Settings.cpp:665 (DBG_MAP, not a logging path).
- **Fix:** Wire `DEBUG_I2C_BUSF` into the I2C bus lifecycle code paths, or remove the command + flag. Until wired the help over-promises.

**`debugi2cdiscovery` — advertises probe/registry/scan diagnostics that emit nothing**
- **Says:** `Debug I2C device probing, registry, scan results.`
- **Actually:** `DEBUG_I2C_DISCOVERYF` exists but has **zero call sites** repo-wide; the flag is never read. Enabling emits nothing.
- **Evidence:** System_Debug.cpp:2899 (row), :1076 (handler); macro at System_Debug.h:542 (0 call sites); flag outside System_Debug only at System_Settings.cpp:666.
- **Fix:** Wire `DEBUG_I2C_DISCOVERYF` into the probe/registry/scan code, or drop the command + flag.

### Low

**`loglevel` — accepts a documented-name superset**
- **Says:** `Usage: loglevel <error|warn|info|debug>`.
- **Actually:** Also accepts `0|1|2|3`, single-letter `e|w|i|d`, `warning`, and a bare no-arg query that prints the current level — none documented. Not harmful (superset).
- **Evidence:** System_Debug.cpp:2997 (usage), :385-392 (extra tokens), :369-381 (query path).
- **Fix:** `loglevel [error|warn|info|debug|0-3]` and note the bare form prints the current level. Low priority since canonical names work.

**Undocumented `[temp|runtime]` second arg — ~70 commands (merged)**
- **Says:** Each usage lists only `<0|1>`.
- **Actually:** Every command below routes through `cmd_debugsubflag_impl` (or `cmd_debugsrsub_impl` / a bespoke twin) which reads `ca.arg(1)` and treats `temp`/`runtime` as a non-persistent ("runtime only") toggle. Documented siblings (`debughttp`, `debugllm`, `debugmicrophone`, `debugmqtt`, `debugwifi`, `debugstorage`, `debugperformance`, `debughttphandlers`) DO show `[temp|runtime]`.
- **Shared evidence:** Impl arg-parse at System_Debug.cpp:1012-1019 (`String mode = ca.arg(1); bool modeTemp = …; "(runtime only)"`); SR twin at :1494-1506; bespoke maps handlers at :1311-1373. Documented sibling example at :2825 (`debughttp`).
- **Affected commands (usage line → handler):** `debugauth` (2829/1104), `debugespnow` (2830/1108), `debugcameralifecycle` (2836/1032), `debugcameracapture` (2837/1035), `debugcamerasettings` (2838/1038), `debugcameravideo` (2839/1041), `debugdisplay` (2840/1061), `debuggps` (2842/1240), `debugrtc` (2843/1244), `debugimu` (2844/1248), `debugthermal` (2845/1252), `debugtof` (2846/1256), `debuginput` (2847/1260), `debuganoencoder` (2848/1264), `debugapds` (2849/1268), `debugpresence` (2850/1272), `debugthermallifecycle` (2852/1277), `debugthermalpolling` (2853/1278), `debugthermalvalues` (2854/1279), `debugtoflifecycle` (2855/1280), `debugtofpolling` (2856/1281), `debugtofvalues` (2857/1282), `debuginputlifecycle` (2858/1283), `debuginputpolling` (2859/1284), `debuginputvalues` (2860/1285), `debuganoencoderlifecycle` (2861/1286), `debuganoencoderpolling` (2862/1287), `debuganoencodervalues` (2863/1288), `debugimulifecycle` (2864/1289), `debugimupolling` (2865/1290), `debugimuvalues` (2866/1291), `debugapdslifecycle` (2867/1292), `debugapdspolling` (2868/1293), `debugapdsvalues` (2869/1294), `debuggpslifecycle` (2870/1295), `debuggpspolling` (2871/1296), `debuggpsvalues` (2872/1297), `debugrtclifecycle` (2873/1298), `debugrtcpolling` (2874), `debugrtcvalues` (2875), `debugfmradiolifecycle` (2876), `debugfmradiopolling` (2877), `debugfmradiovalues` (2878), `debugmiclifecycle` (2879/1304), `debugmicpolling` (2880/1305), `debugmicvalues` (2881/1306), `debugpresencelifecycle` (2882/1307), `debugpresencepolling` (2883/1308), `debugpresencevalues` (2884/1309), `debugmaps` (2885/1311), `debugmapsloading` (2886/1327), `debugmapsrendering` (2887/1343), `debugmapsperf` (2888/1359), `debugi2c` (2897/1069), `debugi2cbus` (2898/1073), `debugi2cdiscovery` (2899/1076), `debugi2cautostart` (2900/1079), `debugespnowstream` (2914/1637), `debugespnowcore` (2915/1653), `debugespnowrouter` (2916/1669), `debugespnowmesh` (2917/1732), `debugespnowtopo` (2918/1748), `debugespnowencryption` (2919/1764), `debugespnowmetadata` (2920/1780), `debugautoscheduler` (2921/1796), `debugautoexec` (2922/1812), `debugautocondition` (2923/1828), `debugautotiming` (2924/1844), `debugmemory` (2925/1685), `debugmemoryheap` (2927/1719), `debugmemorystack` (2928/1722), `debugmemorybuffers` (2929/1725), `debugfmradio` (2995/1427), `debugg2` (2980/1432), `debugg2lifecycle` (2981/1436), `debugg2protocol` (2982/1439), `debugg2events` (2983/1442), `debugg2pages` (2984/1445), `debugg2heartbeat` (2985/1448), `debugg2dump` (2986/1451), `debugsrwake`/`debugsrcommand`/`debugsrafe`/`debugsrlifecycle`/`debugsrtuning` (2990+/1519, via `cmd_debugsrsub_impl`).
- **Fix:** Append ` [temp|runtime]` to each usage string to match the handler and the already-documented siblings. (For `debugi2cbus`/`debugi2cdiscovery` this is secondary to the stale-dead issue above.)

---

## system

### Medium

**`broadcast` — claims per-user targeting it doesn't have**
- **Says:** `Send message to all or specific user.`
- **Actually:** The handler trims `argsInput` into one message and passes it to `broadcastOutput(msg)`, which fans out to ALL interfaces (route 0). No username/target parsing; `broadcast bob hello` broadcasts the literal `bob hello` to everyone.
- **Evidence:** System_Utils.cpp:2379 (row), :2124-2131 (handler), broadcastOutput→route 0 at System_Debug.cpp:740.
- **Fix:** `Send a message to all connected output interfaces.` (drop "or specific user").

**`voltage` — does not measure voltage**
- **Says:** `Read supply voltage.`
- **Actually:** Prints a hardcoded current estimate (base 80 mA + active subsystems) and derives power as `estimate × 3.3`; no ADC/VCC read. The JSON path emits `"measured":false` with a note that it's an estimate.
- **Evidence:** System_Utils.cpp:1263 (comment), :1271 (`estimatedCurrent = 80`), :1302-1303 (`"measured":false`), :1312, table :2366.
- **Fix:** `Estimate power draw from active subsystems (not a real voltage measurement; use batterystatus for measured volts).`

### Low

**`cpufreq` — admin gate blocks the documented "Get" path**
- **Says:** `Get/set CPU frequency.`
- **Actually:** `requiresAdmin=true` on the entry gates the WHOLE command at the dispatcher, including the no-arg GET branch that just prints current/XTAL/APB freq. A non-admin cannot read the frequency.
- **Evidence:** System_Utils.cpp:2367 (`requiresAdmin=true`), :1325-1331 (GET branch), :1332 (comment scopes admin to set), per-entry gating at :3000.
- **Fix:** Gate only the set branch inside the handler (drop `requiresAdmin` on the entry), OR change help to `Get/set CPU frequency (admin).`

**`time` — omits RTC, the primary clock source**
- **Says:** `Show device time (uptime + NTP if synced).`
- **Actually:** Resolves time with RTC as the PRIMARY source, NTP only as fallback; on RTC-equipped builds it reports `(RTC)` (+ RTC temp) and returns before the NTP branch.
- **Evidence:** System_Utils.cpp:2352 (row), :1816 (priority comment), :1825 (`(RTC)` print + return), :1833 (NTP fallback).
- **Fix:** `Show device time (uptime + RTC if present, else NTP).`

**`lightsleep` — undocumented cooldown can refuse the command**
- **Says:** Help/usage describe only entering light sleep for `[seconds]`.
- **Actually:** An anti-flap cooldown gate (`powerSleepTransitionAllowed`) can refuse with `Sleep refused: cooldown active…` and not sleep — an unexpected no-op.
- **Evidence:** System_Utils.cpp:2386 (row), :1370-1375 (cooldown gate).
- **Fix:** Append to help: `; refused if within power-transition cooldown (tune with powercooldown)`.

**`pendinglist` — undocumented `json` flag**
- **Says:** `List pending user requests.` (no usage string).
- **Actually:** Accepts a `json` token that returns a single JSON document instead of the streamed list.
- **Evidence:** System_Utils.cpp:2381 (row), System_User.cpp:2128 (`argWantsJson`).
- **Fix:** `Usage: pendinglist [json]`.

---

## espsr

### High

**`srenable` — advertised `<0|1>` arg is dead**
- **Says:** `Usage: srenable <0|1>` — enable/disable ESP-SR at runtime.
- **Actually:** `setEnabledFromArgs` ignores its argument (`(void)argsInput;`) and unconditionally returns `Error: ENABLE_ESP_SR is a compile-time flag`. Both `srenable 0` and `srenable 1` print the same error.
- **Evidence:** System_ESPSR.cpp:2724-2728 (handler), table :3801.
- **Fix:** Drop `<0|1>` from usage (`Usage: srenable`) and reword help as read-only/informational.

### Medium

**`opensr` / `srstart` — undocumented voice auto-arm (security-relevant)**
- **Says:** `Start ESP-SR pipeline.`
- **Actually:** `cmd_sr_start` also auto-arms voice command execution as the current authenticated user (`voiceArmFromContextInternal(currentAuthContext())`), broadcasts `[VOICE] Armed as …`, and appends `(voice armed as …)` to the reply.
- **Evidence:** System_ESPSR.cpp:2740-2767 (handler); tables :3802 (`opensr`), :3806 (`srstart`, same handler).
- **Fix:** `Start ESP-SR pipeline and arm voice as the current user.`

**`srtuning` — implies it sets params; it only prints status**
- **Says:** `Show/set audio tuning parameters.` / `Usage: srtuning [gain|agc|vad]`.
- **Actually:** Ignores all args (`(void)argsInput;`) and only prints a status dump; setting is done by `srtuninggain`/`srtuningagc`/`srtuningvad`. The printed status also lists `swgain` and `filters`, which the table usage `[gain|agc|vad]` omits.
- **Evidence:** System_ESPSR.cpp:3514-3516 (handler), table :3831; status string includes swgain/filters from :3522.
- **Fix:** Description `Show audio tuning parameters.`, usage `Usage: srtuning`; point users to `srtuninggain`/`srtuningagc`/`srtuningvad`.

**`srconfidence` — usage omits the `category`/`target` sub-forms**
- **Says:** `Usage: srconfidence [0.0-1.0]`.
- **Actually:** Also accepts `category <0.0-1.0>` and `target <0.0-1.0>` (set one threshold); a bare float sets both. The handler's own status output documents all three forms.
- **Evidence:** System_ESPSR.cpp:3225-3227 (sub-form parse), table :3824.
- **Fix:** `Usage: srconfidence [<0.0-1.0> | category <0.0-1.0> | target <0.0-1.0>]`.

### Low

**`srcmds` — printed help inconsistent with registry and table**
- **Says:** Table usage `srcmds <list|add|del|clear|save|reload|sync>` (includes `sync`).
- **Actually:** The handler ignores args and prints `Usage: sr cmds <list|add|del|clear|save|reload>` — omits `sync` and uses a space-separated `sr cmds` form, while the registered commands are flat (`srcmdslist`, …, `srcmdssync`).
- **Evidence:** System_ESPSR.cpp:2897-2901 (printed string), table :3811, flat `srcmdssync` :3818.
- **Fix:** Make the printed string match the registry — list `sync` and use the flat command names (or the `srcmds <…>` wording from the table).

**`setmicsource` — dev-history tag in help**
- **Says:** `Phase 2B: switch SR feed source (local PDM / G2 left temple).`
- **Actually:** "Phase 2B:" is a dev-milestone label in the visible description, not user-facing info.
- **Evidence:** System_ESPSR.cpp:3830.
- **Fix:** `Switch SR feed source (local PDM / G2 left temple).`

---

## wifi

### High

**`wifigettxpower` — non-admin alias bypasses admin gate on a privileged setter**
- **Says:** Registered `requiresAdmin=false`, "(alias of wifitxpower)"; sibling `wifitxpower` is `requiresAdmin=true`.
- **Actually:** Both names dispatch to the SAME `cmd_wifitxpower`, which **sets** radio TX power via `esp_wifi_set_max_tx_power()`. Admin gating is per-entry keyed by the typed name, so a non-admin can run `wifigettxpower <dBm>` to set TX power, bypassing the admin requirement. No internal admin re-check.
- **Evidence:** System_WiFi.cpp:1311 (`requiresAdmin=false`) vs System_Settings.cpp:224 (`requiresAdmin=true`); setter at System_WiFi.cpp:443; per-entry gating at System_Utils.cpp:3000.
- **Fix:** Set `requiresAdmin=true` on the `wifigettxpower` entry (both names invoke the same privileged setter).

### Medium

**`wifigettxpower` — name says "get" but it sets; the real getter is unwired**
- **Says:** Name `wifigettxpower` implies a read of current TX power.
- **Actually:** The registered handler is `cmd_wifitxpower` (sets). The genuine getter `cmd_wifigettxpower` (`esp_wifi_get_max_tx_power`) is declared and defined but never registered, so it is dead. `wifigettxpower 15` silently changes radio power.
- **Evidence:** Wiring System_WiFi.cpp:1311; real getter defined at :452, referenced only at System_WiFi.h:37 (decl) and :452 (def).
- **Fix:** Point `wifigettxpower` at the real getter `cmd_wifigettxpower`, OR rename the entry to `wifisettxpower`; remove the unused getter if left unwired.

**`wifipromote` — usage omits the `[newPriority]` arg**
- **Says:** `Usage: wifipromote <ssid>`.
- **Actually:** Accepts an optional second arg `[newPriority]` (`argInt(1,1)`) and sets that exact priority rather than always top. The handler's own error string includes it.
- **Evidence:** Table System_WiFi.cpp:1304; handler :226 (error string), :228 (`argInt(1,1)`), :236.
- **Fix:** `Usage: wifipromote <ssid> [newPriority]`.

**`openwifi` — error/usage strings name non-existent commands**
- **Says:** Usage-error output instructs `Usage: wificonnect …` and `Check 'wifiinfo' for status`.
- **Actually:** Neither `wificonnect` nor `wifiinfo` is registered (only `openwifi`, `wifiread`, `wifistatus` exist). A user who mistypes is pointed at non-existent commands.
- **Evidence:** System_WiFi.cpp:309, :318, :341; registered names at :1307/:1299/:1300.
- **Fix:** Replace `wificonnect` with `openwifi` and `wifiinfo` with `wifistatus` (or `wifiread`) in these strings.

### Low

**`openwifi` — undocumented legacy bare-index form**
- **Says:** `openwifi [--best | --index <1..N>]`.
- **Actually:** Also accepts a bare positional numeric index (e.g. `openwifi 2`) that connects to that 1-based entry.
- **Evidence:** System_WiFi.cpp:313-315 (legacy positional), usage :1307.
- **Fix:** `openwifi [<index> | --best | --index <1..N>]`, or drop the legacy form.

**`wifilist` — hint names a non-existent command**
- **Says:** Prints `Use 'wificonnect <index>' to connect to a specific entry.`
- **Actually:** No `wificonnect` command; the connect command is `openwifi`.
- **Evidence:** System_WiFi.cpp:152 (hint), only `openwifi` registered (:1307).
- **Fix:** `Use 'openwifi --index <N>' to connect to a specific entry.`

---

## imu

### Medium

**`imupitchoffset` / `imurolloffset` / `imuyawoffset` — usage advertises a range the handler never enforces (merged)**
- **Says:** `Usage: imu{pitch,roll,yaw}offset <-180..180>`.
- **Actually:** The dedicated CLI handlers do NO range check/clamp — they parse the float and write it directly, silently accepting out-of-range values. The `-180..180` bound exists only in the SettingEntry table (enforced by the generic/web settings path), not by these commands. The offsets are applied to orientation output, so a bad value silently corrupts it.
- **Evidence:** pitch i2csensor_bno055.cpp:934-936 (usage :1008, SettingEntry :1151, applied :437); roll :948-950 (usage :1009, entry :1152, applied :438); yaw :962-965 (usage :1010, entry :1153, applied :439). Contrast sibling `cmd_imupollingms` :843 which DOES bounds-check.
- **Fix:** Add `if (v < -180.0f || v > 180.0f) return "Error: imu{Pitch,Roll,Yaw}Offset must be -180..180";` before each `setSetting` (mirroring `imupollingms`), OR drop the range from the usage strings.

### Low

**`openimu` — help implies synchronous start; it's an async enqueue**
- **Says:** `Start BNO055 IMU sensor.`
- **Actually:** `cmd_imustart` only enqueues the start on the device queue and returns immediately (`[IMU] Sensor queued for open`); hardware start happens later.
- **Evidence:** i2csensor_bno055.cpp:229-233, help :991.
- **Fix:** `Queue BNO055 IMU sensor start (completes asynchronously).`

**`closeimu` — help implies synchronous stop; cleanup is async**
- **Says:** `Stop BNO055 IMU sensor.`
- **Actually:** `cmd_imustop` only requests the stop; cleanup completes asynchronously (per the handler's own return string).
- **Evidence:** i2csensor_bno055.cpp:242-243, help :992.
- **Fix:** `Request BNO055 IMU stop (cleanup completes asynchronously).`

**`imuread` — undocumented `json` flag**
- **Says:** `Read IMU sensor data.` (no usage string).
- **Actually:** Accepts a `json` arg that switches to a JSON blob.
- **Evidence:** table :995, handler i2csensor_bno055.cpp:79-83.
- **Fix:** `Usage: imuread [json]`.

---

## oled

### High

**`oledbootmode` — stored but never read; boot screen is hard-wired**
- **Says:** Selects which screen the display shows during boot.
- **Actually:** `gSettings.oledBootMode` is written but never read to drive the boot display. The boot sequence is hard-wired (`ANIM_BOOT_PROGRESS` → `OLED_LOGO` → `OLED_LOGIN`/`gSettings.oledDefaultMode`, a *different* setting). Only the setter, the settings registry rows, and the confirmation print reference it.
- **Evidence:** table OLED_Utils.cpp:6175; setter :3504-3516; hard-coded boot at :4233, :4280; boot.complete uses `oledDefaultMode` at :4310-4313.
- **Fix:** Wire `oledBootMode` into the boot sequence, or remove the command/setting; until then mark the help non-functional.

**`oledbootduration` — stored but never read; boot timing is hard-coded**
- **Says:** `Boot animation duration (ms): <500-10000>` controls how long boot screens show.
- **Actually:** `gSettings.oledBootDuration` is never read. Boot phase durations are hard-coded locals (`LOGO_DURATION=5000`, `SENSORS_DURATION=3000`) and the animation phase ends when `bootProgressPercent >= 100`.
- **Evidence:** table :6177; setter :3560; hard-coded :4270-4271; phase exit :4276.
- **Fix:** Wire `oledBootDuration` into boot phase timing, or remove the command/setting.

### Medium

**`oleddefaultmode` — `sensors` token silently resolves to System Status**
- **Says:** `oleddefaultmode <logo|status|sensors|thermal|network|mesh|off>` — `sensors` is a valid default.
- **Actually:** The handler stores the literal `sensors`, but every consumer resolves it via `modeFromSlug()`, which has no `sensors` case (only `sensordata`/`sensorlist`) and returns -1; both enable and boot-complete paths then fall back to `OLED_SYSTEM_STATUS`. So `oleddefaultmode sensors` reports success but the default screen becomes System Status.
- **Evidence:** store OLED_Utils.cpp:3535; `modeFromSlug` cases :4007-4008 (no `sensors`); fallback :3463-3464 and :4312-4313.
- **Fix:** Accept `sensordata`/`sensorlist` in the handler/usage, or add a `sensors` alias to `modeFromSlug`.

### Low

**`oledthermalscale` — lower bound inconsistent across surfaces**
- **Says:** Usage/description state `0.1..10.0`; the dedicated handler validates `f < 0.1 || f > 10.0`.
- **Actually:** The setting is registered with int `minVal=1`, so the generic `SETTING_FLOAT` validator rejects `set oledThermalScale 0.5` (`Error: oledThermalScale must be 1..10`) and the web UI advertises min=1. The 0.1 floor is honored only by the dedicated CLI command.
- **Evidence:** OLED_Utils.cpp:3618 (CLI check) + usage :3616; SettingEntry `minVal=1` OLED_Settings.cpp:23; generic validator System_Settings.cpp:2213-2215.
- **Fix:** Reconcile: advertise `1.0..10.0` in the CLI handler/usage/description to match the registered setting and web UI, OR make the float validator float-bound-aware to honor 0.1.

---

## camera

### High

**Camera auto-capture / send-after-capture suite — stored but never read (merged)**

All four settings are written by their handlers and surfaced in the web UI, but **no capture/send path reads them**. `captureAndSave()` is called only by `cmd_capture` and `cmd_camerasave`; no timed-capture task exists; `sendFileToMac` is invoked only by `cmd_imagesend`.

- **`cameraautocapture`** — Says `Auto-capture: <on|off>` / label "Enable auto-capture". Actually: written at System_Camera_DVP.cpp:2004, declared struct System_Settings.h:871; never read. Enabling does nothing.
- **`cameraautocaptureinterval`** — Says `cameraautocaptureinterval <10..3600>` governs periodic capture. Actually: written + range-validated at System_Camera_DVP.cpp:2023-2024; no timer consumes it.
- **`camerasendaftercapture`** — Says `Send after capture: <on|off>` auto-sends frames over ESP-NOW. Actually: written at System_Camera_DVP.cpp:2039; no capture path reads it; `sendFileToMac` only from `cmd_imagesend` (System_ImageManager.cpp:645).
- **`cameratargetdevice`** — Says `Target device: <name>` names the auto-send peer. Actually: written at System_Camera_DVP.cpp:2053; never consumed (no send-after-capture path exists).
- **Fix:** Implement a periodic-capture task reading `cameraAutoCapture`/`cameraAutoCaptureIntervalSec`, and a send-after-capture path that checks `cameraSendAfterCapture` and dispatches to `cameraTargetDevice` — OR document all four as not-yet-implemented / remove them. Until wired, the help over-promises.

### Low

**`cameravideolist` — undocumented `json` flag**
- **Says:** `List AVI recordings on SD` (no usage string).
- **Actually:** Accepts a `json` flag (`argWantsJson`) that switches to structured JSON output.
- **Evidence:** table System_Camera_DVP.cpp:2216, handler :2122.
- **Fix:** `Usage: cameravideolist [json]`.

---

## bluetooth

### Medium

**`blestream` — description lists only 4 of 6 sub-commands**
- **Says:** `Control streaming: <on|off|sensors|system>.`
- **Actually:** Also accepts `events` and `interval` (the latter takes two ms args). Only the 5th-field usage string lists them.
- **Evidence:** Bluetooth.cpp:1993 (description vs full usage); handler :1684 (`events`), :1694 (`interval`).
- **Fix:** `Control streaming: <on|off|sensors|system|events|interval>.`

### Low

**`blestatus` / `bleinfo` / `blepeers` — undocumented `json` flag (merged)**
- **Says:** Status/info/peers descriptions with no usage string imply no args.
- **Actually:** Each accepts an undocumented `json` arg returning a machine-readable JSON payload to the caller.
- **Evidence:** `blestatus` Bluetooth.cpp:1986 / handler :1398-1407 (also `bleread` alias); `bleinfo` :1987 / :1819-1856; `blepeers` :2013 / BLE_Peers.cpp:419.
- **Fix:** Add usage strings: `Usage: blestatus [json]`, `Usage: bleinfo [json]`, `Usage: blepeers [json]`.

---

## thermal

### Medium

**`thermalwebmaxfps` — stored but never read (no-op control)**
- **Says:** `Thermal web max FPS: <1..30>` caps the thermal web stream frame rate.
- **Actually:** `thermalWebMaxFps` is only written/declared; never read by any frame-pacing path (whole-repo grep finds zero consumers). The IMU sibling `imuWebMaxFps` is dead the same way.
- **Evidence:** setter i2csensor_mlx90640.cpp:379, SettingEntry :41, struct System_Settings.h:433.
- **Fix:** Wire `thermalWebMaxFps` into the thermal web stream's frame-pacing, or remove the command + setting (and align the IMU twin).

### Low

**`thermalread` — min/max/avg are broadcast, not returned**
- **Says:** `Read thermal sensor data (min/max/avg).` (implies the figures are returned).
- **Actually:** In the default path the numbers go out via `BROADCAST_PRINTF` and the return value is only `[Thermal] Reading complete`. A direct CLI/BLE caller gets the status line, not the data; only the `json` token returns the values.
- **Evidence:** i2csensor_mlx90640.cpp:316-318.
- **Fix:** `Read thermal frame; min/max/avg broadcast to output` (or return the formatted figures directly).

**`thermalread` — undocumented `json` flag**
- **Says:** No usage string; no args documented.
- **Actually:** Accepts a leading `json` token that returns a JSON object (valid/min/max/avg/seq).
- **Evidence:** handler i2csensor_mlx90640.cpp:288, table :1333.
- **Fix:** `Usage: thermalread [json]`.

---

## mic

### Medium

**`micrecord` — usage narrower than accepted tokens; omits status form**
- **Says:** `Usage: micrecord <start|stop>`.
- **Actually:** Also accepts numeric `1`(=start)/`0`(=stop), and a no-arg invocation is a recording-status query. The handler's own fallback error advertises `<start|stop|1|0>`.
- **Evidence:** table System_Microphone.cpp:1153; handler status branch :808-816, token matches :819/:825, fallback :838.
- **Fix:** `Usage: micrecord [start|stop|1|0]` and note that bare `micrecord` reports current recording status.

### Low

**`micread` / `miclevel` / `miclist` — undocumented `json` flag (merged)**
- **Says:** `Usage: micread` / `Usage: miclevel` / `Usage: miclist` — implying no args.
- **Actually:** Each accepts a `json` token returning a JSON status/data blob.
- **Evidence:** `micread` System_Microphone.cpp:1148 / handler :743; `miclevel` :1151 / :785-788; `miclist` :1154 / :844.
- **Fix:** Append `[json]` to each usage string.

---

## feature

### Medium

**`features` — undocumented `json` capability-list mode**
- **Says:** Usage documents only `features`, `features <id>`, `features <id> <on|off>`.
- **Actually:** The handler's first branch is `if (argWantsJson(...))`, returning a full structured JSON capability list whenever a `json` token appears.
- **Evidence:** System_FeatureRegistry.cpp:469-486; usage :661-664.
- **Fix:** Add `features json - JSON capability list` to the usage.

**`featuresetup` — stale `(serial + OLED)` annotation**
- **Says:** `featuresetup - Launch the feature toggle wizard (serial + OLED)`.
- **Actually:** `cmd_featuresetup` calls `setupWizardMode_start()`, entering a CLIMode text state machine driven by the dispatcher — works from ANY transport (serial, web CLI, BLE, MQTT, ESP-NOW, automations), navigated by typing `n`/`b`/numbers. The legacy serial+OLED+joystick path is now used only by FTS-at-boot.
- **Evidence:** System_FeatureRegistry.cpp:648-652; `setupWizardMode_start` enters `kWizardMode` via `cliEnterMode` System_SetupWizardMode.cpp:935-941; usage still says `(serial + OLED)` :666.
- **Fix:** `featuresetup - Launch the feature config wizard (any CLI transport; navigate with n/b/numbers, 'cancel' to abort).`

### Low

**`features` — toggle value accepts more than `<on|off>`**
- **Says:** Toggle value must be `<on|off>`.
- **Actually:** Also accepts `true`/`false` and `1`/`0`. Mild — the rejection error self-describes the full set.
- **Evidence:** System_FeatureRegistry.cpp:589-590, error :593, usage :664.
- **Fix:** Document `<on|off|true|false|1|0>`, or rely on the error message.

---

## settings

### Medium

**`webclihistorysize` — stored but never read**
- **Says:** `Set web CLI history size: <1..100>` resizes the web CLI history buffer.
- **Actually:** `gSettings.webCliHistorySize` is only written/persisted; never read anywhere in firmware or web JS. No observable effect.
- **Evidence:** setter System_Settings.cpp:116-129; whole-repo grep finds only setter, SettingEntry (System_WiFi.cpp:1480), struct decl/default (System_Settings.h:21,405); no web-JS consumer.
- **Fix:** Wire it into web CLI history buffer sizing, or remove the dead setting + command.

**`oledclihistorysize` — stored but never read; "(requires reboot)" doubly misleading**
- **Says:** `Set OLED CLI history size: <10..100>` + success `oledCliHistorySize set to %d (requires reboot)`.
- **Actually:** `gSettings.oledCliHistorySize` is only written/persisted; never read by any OLED history code. No reboot makes a never-read setting take effect.
- **Evidence:** setter System_Settings.cpp:131-144 (reboot note :142); grep finds only setter, SettingEntry (System_Command.cpp:484), struct (System_Settings.h:22,406).
- **Fix:** Wire it into OLED CLI history allocation (and only then keep the reboot note), or delete the dead setting/command; remove the "(requires reboot)" claim until consumed.

---

## apds

### Medium

**Color/proximity/gesture "not enabled" hints name non-existent commands (merged)**

When the relevant mode is off, each read command broadcasts a hint pointing at a command that does not exist anywhere in the codebase (grep finds the name only in the hint string).

- **`apdscolor`** — broadcasts `Color sensing not enabled. Use 'apdscolorstart' first.`; no such command. Real paths: `openapds` or `apdsmode color on`. Evidence: i2csensor_apds9960.cpp:332; real paths :413/:416.
- **`apdsproximity`** — broadcasts `… Use 'apdsproximitystart' first.`; real path `apdsmode proximity on`. Evidence: :362; real path :416.
- **`apdsgesture`** — broadcasts `… Use 'apdsgesturestart' first.`; real path `apdsmode gesture on`. Evidence: :377; real path :416.
- **Fix:** Reference real commands, e.g. `Color sensing not enabled. Use 'openapds' or 'apdsmode color on' first.` (and the proximity/gesture equivalents via `apdsmode`).

### Low

**`apdsread` — undocumented `json` flag**
- **Says:** `Read APDS9960 sensor status and data.` (no usage).
- **Actually:** Accepts a `json` token returning a JSON document.
- **Evidence:** table i2csensor_apds9960.cpp:415, handler :108-112.
- **Fix:** `Usage: apdsread [json]`.

**`apdsmode` — undocumented `prox` alias, no-arg query, broader bool tokens**
- **Says:** `apdsmode <color|proximity|gesture> [on|off]`.
- **Actually:** Also accepts `prox` as an alias for `proximity`; a no-arg call prints current mode states; the `[on|off]` arg is parsed by `argBool` (accepts on/off/true/false/1/0/enable/disable).
- **Evidence:** i2csensor_apds9960.cpp:220 (`prox`), :202-209 (no-arg query), argBool via parseBoolArg System_Command.cpp:414-420.
- **Fix:** Document the `prox` alias and no-arg query, or drop the alias. Low priority since the documented token works.

---

## neopixel

### High

**`ledeffect` — name→effect mapping is misaligned; 3 of 4 named effects run the wrong thing**
- **Says:** `ledeffect <fade|blink|pulse|strobe|off>` — each name runs that effect.
- **Actually:** `cmd_ledeffect` maps `fade=1, blink=2, pulse=3, strobe=4`, but `runLEDEffect`'s enum is `EFFECT_FADE=1, EFFECT_PULSE=2, EFFECT_RAINBOW=3, EFFECT_BLINK=4, EFFECT_STROBE=5`. Net: `blink`→PULSE, `pulse`→RAINBOW, `strobe`→BLINK. Real STROBE (5) is unreachable from any documented token; `rainbow` is reachable but undocumented. Only `fade` and `off` behave as documented.
- **Evidence:** parser System_NeoPixel.cpp:426-429; switch :260-315; enum System_NeoPixel.h:29-33.
- **Fix:** Map names to named enum constants: `blink→EFFECT_BLINK(4)`, `pulse→EFFECT_PULSE(2)`, `strobe→EFFECT_STROBE(5)`, add `rainbow→EFFECT_RAINBOW(3)`; update usage to `<fade|pulse|blink|rainbow|strobe|off>`.

### Low

**`ledcolor` — usage presents a closed 10-color enum; handler accepts ~80**
- **Says:** `ledcolor <red|green|blue|yellow|magenta|cyan|white|orange|purple|pink>`.
- **Actually:** Accepts any of ~80 named colors from `colorTable` plus an undocumented `off` alias.
- **Evidence:** usage System_NeoPixel.cpp:335/446; `getRGBFromName` :175 (`off`), colorTable :35-127.
- **Fix:** State the list is a representative subset ("many CSS color names supported"), mention `off`, or generate the accepted set from `colorTable`.

---

## led

### Medium

**`ledstartupcolor` / `ledstartupcolor2` — usage presents a closed 9-color enum; handler accepts anything (merged)**
- **Says:** `ledstartupcolor[2] <red|green|blue|cyan|magenta|yellow|white|orange|purple>`.
- **Actually:** The handler does NO validation — it stores ANY string and reports success (`LED startup color set to <arg>`), so garbage like `lavender` "succeeds". The real valid set is the ~80-name `colorTable` (+ `off`/`black`) consumed by `getRGBFromName` at boot; an unrecognized name silently falls back to default cyan (color1) / magenta (color2) at render time.
- **Evidence:** color1 System_Hardware.cpp:57, fallback HardwareOne.cpp:1742-1743; color2 System_Hardware.cpp:67, fallback HardwareOne.cpp:1745-1746; colorTable System_NeoPixel.cpp:35-127.
- **Fix:** Validate the arg against `getRGBFromName`/`colorTable` (error on unknown), OR broaden the usage to state any of the ~80 named colors (and `off`) is accepted and that unknown names silently default to cyan/magenta.

### Low

**`ledstartupenabled` — only `1`/`true` enable; everything else silently disables**
- **Says:** `ledstartupenabled <0|1>` / `Enable/disable LED startup effect [0|1]`.
- **Actually:** Accepts `1`/`true` as enable; every other token (including `enable`, `on`, `yes`) is treated as disable with no error, still returning a success-style message.
- **Evidence:** System_Hardware.cpp:31 (`arg == "1" || arg.equalsIgnoreCase("true")`), :33.
- **Fix:** Reject non-`0`/`1`/`true` tokens with an error, or document the truthy tokens and that any unrecognized value disables.

---

## fmRadio

### High

**`fmradiounmute` — actually mutes (shares a handler that decides from args, not the command name)**
- **Says:** `Unmute audio`.
- **Actually:** `fmradiounmute` and `fmradiomute` share `cmd_fmradio_mute`, which decides mute vs unmute from its argument string, not the command name. The dispatcher passes only post-name args, so bare `fmradiounmute` arrives with empty args; `shouldMute = (arg != "off" && arg != "unmute")` is true for empty → the radio is **muted**. The only way to unmute is `fmradiomute off`/`fmradiomute unmute`.
- **Evidence:** table i2csensor_rda5807.cpp:776-777 (both → `cmd_fmradio_mute`); handler :659-666; dispatcher strips command name System_Utils.cpp:4199-4204.
- **Fix:** Give `fmradiounmute` its own handler that forces `shouldMute=false`, or make the shared handler inspect the invoked command name.

### Low

**`fmradioread` — undocumented `json` flag**
- **Says:** `Read FM Radio status.` (no usage).
- **Actually:** Accepts a `json` arg returning a JSON blob.
- **Evidence:** table i2csensor_rda5807.cpp:772, handler :670-677.
- **Fix:** `fmradioread [json]`.

---

## sensorLogging

### High

**`sensorlog sensors` — advertises `gamepad`, but the parser only accepts `input`**
- **Says:** `sensors <thermal|tof|imu|gamepad|apds|gps|presence|all|none>: Select sensors to log`.
- **Actually:** The parser never matches `gamepad`; the token that enables the gamepad/input sensor is `input`. `sensorlog sensors gamepad` falls through and returns `Error: Unknown sensor 'gamepad'`. The real token `input` is itself undocumented.
- **Evidence:** only token match `else if (sensor == "input") gSensorLogMask |= LOG_GAMEPAD;` System_SensorLogging.cpp:950; grep for `== "gamepad"` returns zero parser matches (all `gamepad` occurrences are display/help strings); else branch :954-956; user-facing docs :608, :921, :1023.
- **Fix:** Either change the parser at :950 to accept `gamepad` (keeping `input` as an alias), OR change every user-facing string (:608, :921, :1023) to list `input` instead of `gamepad`.

---

## setPattern

### Medium

**`setgamepadpassword` — undocumented OLED-login precondition**
- **Says:** `Set gamepad joystick password (OLED).` with `requiresAdmin:true` — implying any admin can run it.
- **Actually:** Beyond the admin gate, the handler hard-requires the OLED display session itself be logged in (`isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)`); otherwise it returns `Error: Log in on OLED first (login <user> <pass> display)` and does nothing. An admin invoking from CLI/web/BLE without an active OLED-display login just gets an error.
- **Evidence:** OLED_Mode_SetPattern.cpp:336-338; table :366.
- **Fix:** `Open gamepad password setup on OLED (requires an active OLED-display login).`

### Low

**`setgamepadpassword` — does not set a password; launches an interactive flow**
- **Says:** `Set gamepad joystick password (OLED).`
- **Actually:** Only switches the OLED into interactive setup mode (`requestOLEDMode(OLED_SET_PATTERN,…)`) and returns `Opening gamepad password setup on OLED...`. The password is saved only after the on-device joystick enter/confirm flow; `argsInput` is never parsed.
- **Evidence:** OLED_Mode_SetPattern.cpp:340-343; handler never reads `argsInput`.
- **Fix:** `Open gamepad joystick password setup on OLED.`

---

## edgeImpulse

### Medium

**`eicontinuous` — reports "started" even when no model is loaded**
- **Says:** `eicontinuous 1` reports `Continuous inference started`; description `Start/stop continuous inference mode`.
- **Actually:** The success string is returned unconditionally, but `startContinuousInference()` (void) bails early when no model is loaded — no task is created. The user is told it started when it did not.
- **Evidence:** handler System_EdgeImpulse.cpp:2192-2194; guard :1834-1836.
- **Fix:** Make `startContinuousInference()` return bool (or check `gEIContinuousRunning`) and emit a "no model loaded" message instead of unconditional success.

### Low

**`eimodelload` — usage omits the accepted absolute-path form**
- **Says:** `eimodelload "<filename>"` (and `Load a TFLite model from LittleFS`) imply a bare filename relative to the model directory.
- **Actually:** If the argument starts with `/`, it is used verbatim; otherwise `MODEL_DIR` is prepended. The full-path form is undocumented.
- **Evidence:** System_EdgeImpulse.cpp:2282-2287.
- **Fix:** `Usage: eimodelload "<filename|/full/path>"`.

---

## mqtt

### Low

**`mqttSubscribeTopics` — undocumented `clear` token**
- **Says:** `Usage: mqttSubscribeTopics [topic1,topic2,...]`.
- **Actually:** Also accepts a literal `clear` to empty the list — undocumented here, though sibling `mqttUser`/`mqttPassword` document their `clear` token.
- **Evidence:** System_MQTT.cpp:1482 (clear branch), usage :1602.
- **Fix:** `Usage: mqttSubscribeTopics [topic1,topic2,...|clear]`.

**`mqttclientenabled` — asymmetric enable/disable**
- **Says:** `Enable/disable MQTT [0|1]` — symmetric.
- **Actually:** Disabling calls `stopMQTT()`; enabling only flips the gate setting and returns `MQTT client enabled` with no start path — the user must run `openmqtt` separately to connect.
- **Evidence:** System_MQTT.cpp:1207-1213.
- **Fix:** `Enable/disable MQTT client gate [0|1] (use openmqtt to start).`

---

## thermal/tof/gps/presence/rtc/gamepad/anoEncoder — sensor `json`-flag & related gaps

### rtc

**Medium — `rtcread` error string names a non-existent `rtc` command**
- **Says:** On an unrecognized arg, prints `[RTC] Unknown command. Use: rtc [status|temp]`.
- **Actually:** There is no `rtc` command; the registered name is `rtcread`. A user who follows the suggestion (`rtc status`) gets an unknown-command error.
- **Evidence:** i2csensor_ds3231.cpp:677; `rtcCommands[]` :906-915 has no `rtc` entry; table :909.
- **Fix:** `[RTC] Unknown command. Use: rtcread [status|temp]`.

**Low — `rtcread` undocumented `json` token**
- **Says:** `Usage: rtcread [status|temp]`.
- **Actually:** Also accepts `json` (matched anywhere) short-circuiting to a JSON status dump.
- **Evidence:** i2csensor_ds3231.cpp:619-623, usage :909.
- **Fix:** `Usage: rtcread [status|temp|json]` (or leave if `json` is treated as a universal flag).

### tof

**Low — `tofread` undocumented `json` flag**
- **Says:** `Read ToF distance sensor.` (no usage).
- **Actually:** Accepts an optional `json` token returning a JSON blob via `tofBuildDataJSON()` instead of the human-readable `Distance: X cm` broadcast.
- **Evidence:** table i2csensor_vl53l4cx.cpp:640, handler :140-143.
- **Fix:** `Usage: tofread [json]`.

### gps

**Low — `gpsread` undocumented `json` flag**
- **Says:** `Read GPS location and time data.` (no usage).
- **Actually:** Accepts a `json` arg returning a JSON payload via `gpsBuildDataJSON`.
- **Evidence:** table i2csensor_pa1010d.cpp:625, handler :233-235.
- **Fix:** `Usage: gpsread [json]`.

**Low — `gpslog` undocumented upper bound**
- **Says:** `interval_ms: log interval in ms (default 1000, min 100)` — no max stated.
- **Actually:** Also enforces an undocumented max of 3600000 ms; values above it are rejected with the same usage error.
- **Evidence:** usage i2csensor_pa1010d.cpp:635, handler :571/:574.
- **Fix:** `interval_ms: 100..3600000 (default 1000)`.

### presence

**Low — `presenceread` undocumented `json` flag (bypasses not-running guard)**
- **Says:** `Read STHS34PF80 presence/motion/temperature data.` (no usage).
- **Actually:** Accepts a leading `json` flag returning a JSON object via `presenceBuildDataJSON()`; the json branch precedes the "sensor not running" guard.
- **Evidence:** table i2csensor_sths34pf80.cpp:463, handler :215-217, guard :221.
- **Fix:** `Usage: presenceread [json]`.

**Low — `presencestatus json` returns data, not status**
- **Says:** `Show STHS34PF80 sensor status.` (no usage).
- **Actually:** Accepts a `json` flag, but in json mode calls `presenceBuildDataJSON()` — returning the same sensor DATA as `presenceread json`, not the status fields (connected/enabled/taskHandle/dataValid).
- **Evidence:** table i2csensor_sths34pf80.cpp:464, json branch :249-251, non-json status string :257-260.
- **Fix:** `Usage: presencestatus [json]`; ideally have the json branch emit the status fields, or note that `json` returns sensor data.

### gamepad

**Low — `gamepadread` undocumented `json` flag**
- **Says:** `Read Seesaw gamepad state (x/y/buttons).` (no usage).
- **Actually:** Accepts a `json` token returning a JSON blob via `gamepadBuildDataJSON`.
- **Evidence:** table i2csensor_seesaw.cpp:385, handler :85-88.
- **Fix:** `gamepadread [json]`.

### anoEncoder

**Low — `anoencoderread` undocumented `json` flag**
- **Says:** `Read ANO encoder state.` (no usage).
- **Actually:** Accepts a `json` token returning a JSON object.
- **Evidence:** handler i2csensor_ano_encoder.cpp:302-311, table :416.
- **Fix:** `Read ANO encoder state (add 'json' for JSON).` / `Usage: anoencoderread [json]`.

---

## sd

### Low

**`sdinfo` — undocumented `json` flag**
- **Says:** `sdinfo - Display SD card type, size, and usage` (no args).
- **Actually:** Accepts a `json` token returning a JSON object instead of the text report.
- **Evidence:** handler System_VFS.cpp:971/974/979/996, table :1170-1171.
- **Fix:** `sdinfo [json] - Display SD card type, size, and usage (add 'json' for machine-readable output)`.

**`sddiag` — return string names the wrong reply-sink**
- **Says:** Return value points the user at "serial log output".
- **Actually:** `sddiag` emits the full report via `broadcastMultilineReport()`→`broadcastOutput()`, routing to the invoking session's output mask (serial/web/file/BLE, plus OLED/G2). A web or BLE caller receives the whole report on THEIR channel.
- **Evidence:** System_VFS.cpp:1155-1156, `broadcastMultilineReport` :52-84, mask at HardwareOne.cpp:857-858.
- **Fix:** `sddiag complete (full report sent to this session's output)`.

---

## battery

### Medium

**`batterycalibrate` — "ADC" wording wrong on fuel-gauge and USB-only boards**
- **Says:** `Recalibrate battery ADC readings`.
- **Actually:** ADC recalibration happens only on `BATTERY_BACKEND_ADC` boards. On the FeatherS3[D] (a primary board, `BATTERY_BACKEND_FUEL_GAUGE=1`, `BATTERY_ADC_PIN=-1`) it re-probes the self-calibrating MAX17048 (nothing is recharacterized); on USB-only builds it does nothing.
- **Evidence:** table System_Utils.cpp:2403; backend branches System_Battery.cpp:566-576; board config System_BuildConfig.h:817-820.
- **Fix:** `Recalibrate/re-probe the battery sensor (ADC characterize or fuel-gauge re-probe).`

### Low

**`batterystatus` — undocumented `json` flag**
- **Says:** `Show battery voltage, charge level, and status` (usage = nullptr).
- **Actually:** Accepts a `json` arg returning a raw JSON telemetry blob instead of the box display.
- **Evidence:** table System_Utils.cpp:2402, handler System_Battery.cpp:488-496.
- **Fix:** `Usage: batterystatus [json]`.

---

## image

### Low

**`images` — description/usage disagree and neither matches the handler**
- **Says:** Description `images [littlefs|sd]`; usage `images [sd] [json]`.
- **Actually:** `cmd_images` only inspects for ` sd` and the `json` flag; it never parses `littlefs`/`lfs` (location defaults to LittleFS and anything not `sd` falls through). The littlefs/lfs branch exists only in `cmd_capture`.
- **Evidence:** table System_ImageManager.cpp:658; `cmd_images` :533-536 (`sd` only), :546 (`json`); littlefs branch in `cmd_capture` :514-515.
- **Fix:** `List saved images: images [sd] [json]` (drop the unparsed littlefs, add the real json flag), or add a littlefs/lfs parse branch to `cmd_images`.

**`imagesend` — usage claims it "Returns OK"; it returns a different string**
- **Says:** Usage states it "Returns OK on dispatch".
- **Actually:** On success the handler returns `Sending <path> to <device>`, not the literal `OK`. A script matching for `OK` never matches.
- **Evidence:** usage System_ImageManager.cpp:660; handler :645-648.
- **Fix:** `Returns 'Sending <path> to <device>' on dispatch`.

---

## filesystem

### Low

**`logtier` — undocumented `json` output mode**
- **Says:** Description/usage present `logtier` as taking no arguments.
- **Actually:** Accepts a `json` arg emitting a JSON envelope (`{schema,tier,overflow,littlefs{…},sd{…}}`).
- **Evidence:** handler System_Filesystem.cpp:1040/1046-1054, table :1119-1120.
- **Fix:** Add a `logtier json` line to the usage/description.

**`filedelete` — undocumented confirm-token aliases**
- **Says:** `Usage: filedelete "<path>" [confirm]` — only `confirm`.
- **Actually:** Also accepts `--yes`, `-y`, `yes` as equivalent one-shot tokens.
- **Evidence:** System_Filesystem.cpp:992-994, usage :1117.
- **Fix:** Document `[confirm|yes|-y|--yes]`, or drop the undocumented aliases.

---

## automation

### Low

**`automationadd` — alias has no usage; field syntax undocumented**
- **Says:** `Add automation (same as 'automation add').` (no usage string).
- **Actually:** `cmd_automation_add` parses many `key=value` args (`name=`, `type=`, `time=`, `days=`, `command=`/`commands=`, `delayms=`, `intervalms=`, `condition=`, `enabled=`, `runatboot=`, `bootdelayms=`, …), but neither the alias nor the top-level `automation` usage documents any field names (only the bare token `add`). Mitigated by field-specific error messages.
- **Evidence:** table System_Automation.cpp:3777; handler :906-924; top-level usage :3773.
- **Fix:** Add a usage string for `automationadd` documenting the `name=`/`type=`/`time=`/`command=`/`commands=`/`delayms=`/`intervalms=` fields.

---

## userSystem

### Low

**`sessionrevoke` — short description reads as positional value; handler needs a keyword**
- **Says:** `Revoke session: <sid|user> [reason]` — reads as if you pass a SID/username directly (e.g. `sessionrevoke alice`).
- **Actually:** The handler requires a literal sub-command keyword (`sid` or `user`) as arg 0, then the value as arg 1; `sessionrevoke alice` matches neither branch and falls to the usage fallback. The 5th-field usage string is correct — only the short description is ambiguous.
- **Evidence:** System_User.cpp:3170 (description), handler :2281-2289 / :2312.
- **Fix:** `Revoke sessions: sid <sid> | user <username> [reason]`.

---

## llm

### Medium

**`llmmirostateta` — advertised `0.5` upper bound is wrong (real ceiling is 1.0)**
- **Says:** `Usage: llmmirostateta <0.01-0.5>`.
- **Actually:** The SettingEntry has `minVal=0, maxVal=0`, so `handleSettingCommand`'s FLOAT validation (guarded by `minVal != 0 || maxVal != 0`) is skipped — any value is stored. The only real clamp is at generation time, bounding the effective value to `[0.01, 1.0]` — ceiling 1.0, not 0.5. Values up to 1.0 are honored.
- **Evidence:** SettingEntry System_LLM.cpp:2312, usage :2377; validation skip System_Settings.cpp:2211-2212; gen-time clamp System_LLMChat.cpp:267-268.
- **Fix:** `Usage: llmmirostateta <0.01-1.0>` to match the real gen-time clamp. (Note: the SettingEntry int min/max cannot express a fractional floor, so set-time validation can't enforce 0.01.)

### Low

**`llmgenerate` — undocumented leading `json` async mode**
- **Says:** `Usage: llmgenerate <prompt text>` (synchronous only).
- **Actually:** Accepts a leading `json` token (`argLeadingTokenIsJson`) switching to async non-blocking mode: `chatBeginTurn()` runs and `{"schema":1,"ok":true,"session":N}` returns immediately instead of inline text.
- **Evidence:** System_LLM.cpp:2118-2128, usage :2360.
- **Fix:** `Usage: llmgenerate [json] <prompt text> (json = async; returns {session}, poll with llmresult json <offset>)`.

---

## servo

### Low

**`pwm` — returns an "Error" on bad freq but still applies the channel value**
- **Says:** On out-of-range optional freq, returns `[Servo] Error: Frequency must be 24-1526Hz`, implying nothing was applied.
- **Actually:** The PWM channel value IS still written: after building the error string, control falls through to the unconditional `setPWM` call.
- **Evidence:** i2csensor_pca9685.cpp:259-261 (error build), :266-268 (unconditional setPWM).
- **Fix:** Return early before `setPWM` when freq is out of range, OR change the message to reflect that the value was applied but the requested frequency was ignored.

---

## mapsSetting

### Medium

**`mapcachekb` — help/label say "reboot to apply"; no reboot is needed**
- **Says:** `Set tile cache size in KB (reboot to apply)` and label `Tile cache size (KB, reboot to apply)`.
- **Actually:** The cache pool is (re)allocated inside `MapCore::loadMapFile()` on every map load, reading `gSettings.mapCacheSizeKB` directly — a new value takes effect on the NEXT map load. The handler's own success string says `effective on next map load`.
- **Evidence:** table System_Maps.cpp:3679; handler output :3646; pool re-init inside `MapCore::loadMapFile` :564; SettingEntry label :3654.
- **Fix:** Change both the description and the SettingEntry label from `reboot to apply` to `effective on next map load`.

---

## Cross-module patterns worth a systemic fix

1. **~70 `debug*` commands omit `[temp|runtime]` from their usage.** They share `cmd_debugsubflag_impl`/`cmd_debugsrsub_impl` and a couple of bespoke twins, all of which accept the second token; a documented sibling set already shows it. Fix once by appending ` [temp|runtime]` programmatically to every sub-flag usage string (or generate usage from the shared impl), eliminating the entire class.

2. **~25 commands expose an undocumented `json` flag.** `argWantsJson` is a codebase-wide convention; the usage strings simply haven't kept up. A systemic fix is to auto-append `[json]` (and/or a footer note) to the usage of any command whose handler calls `argWantsJson`. Affected: thermalread, imuread, tofread, gpsread, presenceread/presencestatus, gamepadread, anoencoderread, rtcread, apdsread, fmradioread, micread/miclevel/miclist, blestatus/bleinfo/blepeers, sdinfo, logtier, batterystatus, pendinglist, cameravideolist, features, llmgenerate (leading-json), images.

3. **~13 stored-but-never-read settings advertise working controls.** Camera auto-capture suite (4, High), OLED boot mode/duration (2, High), `thermalWebMaxFps` (+`imuWebMaxFps` twin), `webCliHistorySize`, `oledCliHistorySize`, plus dead debug streams `debugi2cbus`/`debugi2cdiscovery`. A systemic audit pass: for every SettingEntry, confirm at least one non-setter read site; otherwise wire it or remove it. This is the highest-impact category because the help actively lies about functionality.

4. **4 role/auth overclaims & gating bugs.** `wifigettxpower` (non-admin alias of an admin setter — privilege bypass, High), `cpufreq` (admin gate blocks the documented read path), `setgamepadpassword` (undocumented OLED-login precondition). Per-entry `requiresAdmin` keyed by typed name means aliases can silently diverge in privilege — audit every alias pair for matching `requiresAdmin`.

5. **6 user-facing strings reference non-existent commands.** `wificonnect`, `wifiinfo` (wifi), `apdscolorstart`/`apdsproximitystart`/`apdsgesturestart` (apds), `rtc` (rtc). These are pure copy bugs in error/hint strings — a grep of broadcast/usage strings against the registered command table would catch them all.

6. **Several "says OK but does the wrong thing" execution bugs (High/Med).** `ledeffect` (name→enum misalignment runs the wrong effect), `fmradiounmute` (mutes), `eicontinuous` (reports started with no model), `sensorlog sensors gamepad` (token never matches). These are behavior bugs surfaced via the help audit, not mere wording — worth prioritizing over the cosmetic arg-signature class.

7. **Range bounds advertised in usage but unenforced by the dedicated handler.** `imupitchoffset`/`imurolloffset`/`imuyawoffset` (Med ×3), `oledthermalscale`, `llmmirostateta` — the bound lives only in the SettingEntry/generic validator, while the dedicated CLI command writes unchecked. Systemic fix: have dedicated set-handlers reuse the SettingEntry bound, or drop the range from usage where the handler can't enforce it.

8. **2 dev-history tags in visible help:** `setmicsource` ("Phase 2B:") and `debugespnow` ("(parent flag)" mislabel). Strip dev-milestone prefixes from all visible description strings.

---

## debug — batch 7 gap-fill (positions 97-112) - closes 100% coverage

The one batch that errored in the all-module run, re-audited (audit -> adversarial verify): **18 findings (6 medium, 12 low)**, dominated by the same undocumented `[temp|runtime]` pattern, plus two items of note.

**[BUG] `debugmqtt` - ODR violation (duplicate external-linkage symbol).** `cmd_debugmqtt` is defined non-`static` in BOTH `System_Debug.cpp:1045` and `System_MQTT.cpp:1543`, with *different* behavior (the MQTT copy rejects `[temp|runtime]`). The linker resolves to one arbitrarily - undefined behavior, and which one wins decides whether the documented `[temp|runtime]` arg works. **Fix:** make one `static` (or delete the `System_MQTT.cpp` copy).

**Fake-granular debug toggles (5, medium):** `debugauthsessions`, `debugauthcookies`, `debugauthlogin`, `debugauthbootid`, `debughttphandlers` each advertise a distinct per-scope debug stream, but none has its own gating macro - every one just ORs into the parent `DEBUG_AUTH` / `DEBUG_HTTP` bit, so e.g. `debugauthsessions 1` is identical to `debugauth 1`. (Same class as `debugespnow` "(parent flag)".) **Fix:** add real per-scope gating macros and route the relevant log lines through them, or reword each as a parent alias.

**`loglink` (medium):** usage `<0|1>` understates the accepted `on/off/true/false` + bare-status forms (also flagged in the main run).

**Undocumented `[temp|runtime]` (low):** `debugauthbootid`, `debugauthsessions`, `debugautotiming`, `debugmemory`, `debugmemorybuffers`, `debugmemoryheap`, `debugmemorystack`, `debugmqttcommands`, `debugmqttconnection`, `debugmqttdiscovery`, `debugmqttpubsub` - append `[temp|runtime]` to each usage string.

---

## Medium-pass applied (wording fixes + inert-setting wiring)

Wording-only pass over the Medium findings (dead-end refs, "says one thing does another", arg drift), plus wiring the three Medium "stored-but-never-read" settings so they actually function (no behavior bugs touched — those remain deferred to the "real bugs" bucket).

### Bucket A — dead-end command references (fixed)
- `System_WiFi.cpp` — `openwifi` usage-error strings `wificonnect`→`openwifi`; `Check 'wifiinfo'`→`wifistatus`; `wifilist` hint `wificonnect <index>`→`openwifi --index <N>`.
- `i2csensor_apds9960.cpp` — color/proximity/gesture "not enabled" hints `apds*start`→`openapds`/`apdsmode <mode> on`.
- `i2csensor_ds3231.cpp` — `rtcread` error `Use: rtc [...]`→`Use: rtcread [...]`.

### Bucket B1 — says-one-does-another (description/return wording)
- `System_Utils.cpp` — `voltage` (estimate, not real measurement), `broadcast` (drop "or specific user"), `batterycalibrate` (ADC/fuel-gauge wording). (`wifigettxpower` desc + `cpufreq` "(admin)" already corrected in prior session.)
- `System_ESPSR.cpp` — `opensr`/`srstart` note voice auto-arm side effect.
- `System_Maps.cpp` — `mapcachekb` description + SettingEntry label "(reboot to apply)"→"(effective on next map load)".
- `System_Debug.cpp` — `debugespnow` drop "(parent flag)"→"alias of debugespnowcore".
- `System_FeatureRegistry.cpp` — `featuresetup` drop stale "(serial + OLED)".
- `System_VFS.cpp` — `sddiag` return "serial log"→"this session's output".
- `System_ImageManager.cpp` — `imagesend` "Returns OK"→"Returns 'Sending <path> to <device>'".
- `i2csensor_mlx90640.cpp` — `thermalread` clarify min/max/avg are broadcast.

### Bucket C1 — arg drift (handler accepts a superset; usage broadened)
- `System_Debug.cpp` `loglink`; `System_ESPSR.cpp` `srconfidence`; `System_WiFi.cpp` `wifipromote`; `Bluetooth.cpp` `blestream`; `System_Microphone.cpp` `micrecord`; `System_FeatureRegistry.cpp` `features` (json line); `System_LLM.cpp` `llmmirostateta` (`0.5`→`1.0`).

### Bucket C2 — range/enum advertised but not enforced (wording)
- `i2csensor_bno055.cpp` — IMU pitch/roll/yaw offsets: `<-180..180>`→`<degrees> (recommended -180..180)`.
- `System_Hardware.cpp` — `ledstartupcolor`/`2`: closed 9-color enum → "any of ~80 named colors or 'off'; unknown defaults to cyan/magenta".

### Inert settings — now wired (functional, no "not implemented" notes)
- **`thermalWebMaxFps`** — `i2csensor_mlx90640_web.h`: firmware value injected as `window.__thermalWebMaxFps`; `startThermalPolling()` now clamps the browser poll cadence to `max(thermalPollingMs, ceil(1000/maxFps))`, making it a real web FPS ceiling.
- **`webCliHistorySize`** — `WebPage_CLI.h`: injected as `window.__cliHistoryMax`; the command-history cap (was hard-coded `50`) now uses it (local + bonded paths). Live per page load.
- **`oledCliHistorySize`** — `OLED_ConsoleBuffer.h`/`OLED_Utils.cpp`: ring buffer gained a runtime `capacity` member latched from the setting at `init()` (physical cap raised 50→100 to cover the 10..100 range; ~+3.4KB RAM). "(requires reboot)" in the success string is now accurate.

### Behavior bugs — now FIXED (real code fixes)
- **`debugmqtt` ODR** — removed the duplicate non-static `cmd_debugmqtt` definition + its table entry in `System_MQTT.cpp`; the canonical definition (debug sub-flag family with `[temp|runtime]` + runtime `DEBUG_MQTT` bit) in `System_Debug.cpp` is now the sole definition/registration. *Note: this drops `debugmqtt` from the MQTT module's command table; if the MQTT web settings page surfaced a toggle from that table, it's now sourced only via the debug module — confirm on hardware.*
- **`log autostart [on|off]`** (`System_Debug.cpp`) — now parses `on/off/1/0/true/false/enable/disable` to set explicitly; bare `log autostart` still toggles. Previously ignored the arg and always inverted.
- **`eicontinuous`** (`System_EdgeImpulse.cpp` + `.h`) — `startContinuousInference()` now returns `bool`; the handler returns `"Error: cannot start continuous inference (load a model first with eimodelload)"` when no model is loaded instead of falsely reporting "started".
- **`srtuning <gain|agc|vad|swgain|filters> <value>`** (`System_ESPSR.cpp`) — now delegates to the dedicated setters (forward-declared); bare `srtuning` still prints status. Previously ignored all args.
- **`oleddefaultmode sensors`** (`OLED_Utils.cpp`) — added `sensors` alias to `modeFromSlug()` → `OLED_SENSOR_DATA`, so the documented `sensors` default now resolves instead of silently falling back to System Status.
