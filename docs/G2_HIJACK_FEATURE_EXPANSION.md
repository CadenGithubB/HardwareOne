# G2 Hijack Menu — Feature Expansion Opportunities

## 1. Executive summary

The G2 hijack menu is a tap-navigable UI the firmware paints onto the Even Realities G2 lens over BLE. It already has the right bones: a page registry (`G2PageModule`), a shared command seam (`g2SubmitHijackCommand` → `cmd_exec` under the pairer's identity), a live-text/live-list worker, a mixed list+text compound for "controls + live readout," a character-picker `g2BeginTextEntry` overlay, and a menu-gen-guarded async redraw path. Every CLI command is therefore one tap away.

**The core problem is that most G2 pages are read-only mirrors, and most big shipped features never reach the lens.** Concretely:

- **Read-only pages that should act.** Status shows telemetry but offers zero actions. Settings is the *only* settings surface on the device where tapping a row is an explicit no-op. Power shows no battery/CPU state despite `doPowerOff` already logging battery events. ESP-NOW App's peers/stats/chat surfaces are info-only.
- **Flagship features with no lens surface at all.** Automations, the on-device LLM chat, the ~130-kind system event ring, the v0.98.7 notification pipeline, sensor logging, and CPU frequency scaling all exist and are exercised by web/OLED/CLI — but there is no hijack page for any of them.
- **Half-wired pages.** FM radio has the richest command set on the device yet its detail page exposes only Run/Auto-Start. The MIC detail page *displays* rec/idle but can't start recording. THERM is the only sensor with no real readout.

**The biggest wins, and why:**

1. **Status live-tick migration to per-child `UPDATE_TEXT`** (Mic-detail pattern) — the enabler that stops re-sending the whole compound every 5s, fixes the cursor-reset that blocks any action list, and unlocks "make Status actionable."
2. **Tap-editable Settings** — closes the single largest "share logic, don't duplicate" gap; the registry already hands each `SettingEntry` to the page.
3. **Sensors: thermal grayscale image viewer + FM tuner** — turn the one sensor with no readout into a live image, and turn the richest command set into a real tuner.
4. **Power: live telemetry + one-tap CPU-mode cycle** — a page literally named "Power" that finally shows power state and exposes the battery lever.
5. **New app pages: Automations, LLM Chat, Events viewer** — pure command-registry/live-worker reuse; these are the marquee wearable interactions.
6. **Tests: command-registry path probe** — the transport bench never invokes the command seam it nominally validates.

Two cross-cutting truths shape everything below: taps run as the glasses' `pairedByUser` (so admin-gated commands are denied for a non-admin pairer — surface the `Error:`, never silently swallow it), and passive no-touch pages are force-exited by the 60s hijack watchdog (`HIJACK_SAFETY_MS`) because an ack is not user presence.

---

## 2. Screen inventory

| Screen | Current role on the lens |
|---|---|
| **Status** | Read-only live compound (name/uptime/temp/IP/ESPNow body + heap/PSRAM corner meter + battery corners). Full REBUILD every 5s. Zero actions. |
| **Sensors** | Sensor roster with connected/missing/off state; per-sensor live detail (auto-start toggle + `UPDATE_TEXT` readout); hidden Camera-Settings and Mic-detail compounds. THERM has no real readout. |
| **Network** | Deepest page (~1940 lines): WiFi scan/saved/status/add-via-keyboard, HTTP(S) server + auto-start, ESP-NOW settings, Bluetooth G2/R1 submenus. |
| **ESP-NOW App** | Mesh chat/send/broadcast/ping/peers/stats + Bonded-Device remote-sensor readout. Peer detail *does* ship RTT ping; most other surfaces are info-only. |
| **Settings** | The only read-only settings surface on the device — module list + PRETTY/JSON inspector. Tapping a setting row is an explicit no-op. |
| **Power** | Restart / Power Off, each behind a two-step confirm sub-list. No telemetry. |
| **Tests** | Transport test bench (BLE/Transport/AI Panel/Display/Image); diagnostics logged to serial only; never invokes `executeCommand`. |
| **Apps** | Launcher forwarding to ESP-NOW App / Files / Maps. Fixed `items[4]` buffer with no compile guard (latent stack overflow). |
| **TextEntry** | The only free-text path (13-char picker). Today only hands a captured string back to three ESP-NOW/Network callers; no post-submit result screen. |
| *(Maps / Files)* | **Out of scope** — owned by other agents. Referenced only as Apps launch hosts. |

---

## 3. Enhance existing screens

Only VERIFIED-feasible opportunities are listed; infeasible/already-shipped/mis-specified items are called out as **BLOCKED** or **DEPRIORITIZED** with the real path. Values/efforts are the *adjusted* (post-verification) figures.

### 3.1 Status

- **Migrate the live tick to per-child `UPDATE_TEXT`** — *high / M* — **the enabler.** Keep the one-time CREATE of the 1-list+5-text compound but stop re-sending the whole multi-child REBUILD every 5s; push only changed TEXT children via `sendUpdateTextNamed(...)` (G2_Glasses.cpp:7540), gated by the existing `gStatusLast{Body,Batt,Meter,R1,Esp}Str` diff caches (9855-9859). Reuses the exact `renderMicDetailLive`/`g2ShowSensorLive` pattern. Fixes two documented fragilities: multi-child REBUILD blanks omitted siblings (empirically verified 2026-04-30), and re-CREATE snaps the list cursor to row 0 — which is why no action list works today. **HW-validation caveat:** both existing `UPDATE_TEXT` users are single-TEXT-child compounds; Status has five siblings and multi-text-sibling preservation under magic-220 `UPDATE_TEXT` is untested. Never `UPDATE_TEXT` the `'app'` list child (historically froze acks).

- **Turn Status actionable: tappable action rows on the `'app'` list child** — *medium / L* — Give `kStatusPage` its own `G2_HIJACK_PAGE_*` enum + `handleTap` (currently `nullptr`/`TEXT_VIEW`, G2_Glasses.cpp:3667). The compound's list child already emits real single-tap `ListEvent CLICK`. Route rows: Restart → the existing `kPowerPage` two-step confirm sub-list; ESPNow/WiFi via `g2SubmitHijackCommand`. **Real path corrections:** there is no `wifi connect` command — use `openwifi`; `espnowenabled` only takes effect after reboot (not a live toggle); Restart already exists one level up on Power. Depends on the `UPDATE_TEXT` migration for a stable cursor, and must refresh `gHijackStartedMs` on tap. Net value dropped from high because two of the four proposed rows are weak.

- **Recent-events glance line + `Events →` drill-in** — *medium / M* (part 1 is *high / S*) — **Part 1:** fold the newest event into `buildG2StatusSnapshot` (G2_Glasses.cpp:3296) via `systemEventGetBySeq(systemEventLatestSeq())` + `systemEventKindName` → e.g. `Evt: peer_online host-2`. Trivial, high-value, no footguns. **Part 2:** the drill-in must *extend* the single back-row list child to two rows + add idx routing (Status is TEXT_VIEW, not a pre-existing action list), spawning a read-only `g2StartLiveTextPage` walking the ring newest-first. Live-text identity and SYSTEM_EXIT teardown are already handled by infra.

- **Wall-clock time + sync state, folded into the uptime line** — *medium / S* — **quick win.** Change body line 2 from `Up XhYm - NNC` to lead with `HH:MM` when synced (`~`/`no-sync` marker otherwise), via `Clock::isSynced()` + `Clock::epochSeconds()` re-inlined as a `strftime('%H:%M')`. No 5th line (hard 4-line body cap), minute-granularity keeps the diff cache short-circuiting. Do **not** touch the G2 TIME_SYNC quarter-hour encoding — display read only.

- **Extend the memory meter with storage free-space (+ min-heap / largest-block)** — *medium / M* — Storage half genuinely reuses the shared JSON: add a third `renderCircleBar` gauge from `buildSystemInfoJson`'s `storage.*_kb` (System_Utils.cpp:1566-1582), SD sub-line gated on `VFS::isSDAvailable()`. This is the high-value half (BLE uploads are storage-gated, lens shows nothing). Min-heap watermark / largest-free-block are **not** in the shared JSON — call `ESP.getMinFreeHeap()` / `heap_caps_get_largest_free_block()` directly (same primitives OLED uses). **Footgun the proposal under-weighted:** the meter corner is documented as sized for two lines (~250px); a third line risks vertical overflow — confirm geometry on HW.

- **On-device LLM status line (gated on `ENABLE_ONDEVICE_LLM`)** — *medium / S* — Add a 6th corner text child from `llmGetStatus()` + `llmModelDescription()` (System_LLM.h:132/139) — `LLM: ready <model>` / `gen N.Nt/s` / `off`. `buildConnectivityJson` already reads these the same read-only way. Free right-column canvas exists at y≈110-220. Fold-into-body is blocked by the 4-line/width cap, so the corner child is the only viable sub-option (new geom const + diff-cache global + extend CREATE array 5→6 under `#if`).

- **Notifications unread count + `Notifications →` drill-in** — *medium / M* — Badge from the persistent `NSINK_QUEUE` events resolved against `pairedByUser` via `notifViewerResolve` + `notifFormatEvent`; drill-in via `g2StartLiveTextPage`. **`duplicatesWebLogic` — actually OLED logic:** the queue-enumeration walk lives only as static `notifViewRebuild` in OLED_Utils.cpp. Correct path is to **extract** a shared `notifQueueForViewer(...)`/`notifQueueCountForViewer(...)` into System_Notifications so both G2 and OLED call it. Also: "unread" needs a per-viewer read cursor only OLED has — a v1 G2 badge can honestly only be a *queued* count.

### 3.2 Sensors

- **Render the thermal array as a live grayscale image** — *high / L* — **flagship "dead value → feature."** THERM is the only compiled sensor with no dedicated readout (falls through to a one-line `min/maxC`). Drill into a list+image compound mirroring `g2ShowCameraStream` (`g2BuildCreateMixedListImage`), colorize the raw 32×24 `gThermalCache.thermalFrame` (not `thermalInterpolated` — it's only allocated when `thermalUpscaleFactor==2`) into a 288×144 4bpp grayscale via `buildBmp4bppFromRgb888` (G2_Glasses.cpp:14457) — whose `BmpToneMap::AutoLevels` mode *already does* the min/max normalization. Put Back/Run on the tappable list child (image-only children emit only DOUBLE_CLICK). **Hard cap:** passive viewing can't exceed 60s (never feed `gHijackStartedMs` from frame pushes), so this is short-glance, not continuous monitoring. No per-frame task; re-push on the thermal poll cadence.

- **Make the FM detail page an actual tuner (Seek / Vol± / Mute)** — *high / M* — **turn read-only into actionable.** Branch `g2BuildSensorLiveList` + the `SENSORS_LEVEL_LIVE` tap handler on `featureId=='fmradio'` to add Seek/Vol+/Vol-/Mute rows dispatching `fmradioseek up` / `fmradiovolume <cur±1>` / `fmradiomute`/`unmute` (all **non-admin**, i2csensor_rda5807.cpp:825-828) via `g2SubmitHijackCommand`; bump `listItems[4]` in `renderSensorDetailLive`; re-CREATE via `showSensorDetail`. The 1 Hz `g2BuildFmReadout` closes the feedback loop for free. **Footgun:** `fmradioseek` is a blocking ~5-6s I2C op on `cmd_exec` — it will feel unresponsive and serializes other queued commands (Vol/Mute are fast).

- **Add a Record toggle to the MIC detail page** — *medium / S* — Finishes a look-only page: it already shows rec/idle but can't record. Add a `Record` row to `renderMicDetailLive` dispatching `micrecord start`/`stop` (non-admin, self-guards the no-mic case with an `Error:`, System_Microphone.cpp:812) via the same async path the MIC toggle uses. Keep REC state in the existing `UPDATE_TEXT` readout so selection persists; the one lockstep edit is the positional idx map in `g2MicDetailHandleTap` (Record at idx 2, HW:PDM shifts to 3).

- **One-tap RTC sync from the RTC detail page** — *medium / S* — Add a `Sync ← System` row dispatching `rtcsync from` (i2csensor_ds3231.cpp:760); `g2BuildRtcReadout` confirms at 1 Hz. The tap→submit→re-CREATE pattern is already proven on this page (Run/Auto-Start rows). **`rtcsync` is admin-gated** — grey/toast the row for non-admin pairers. Gate the row on `featureId=='rtc'` (list shape differs from other sensors, harmless since `showSensorDetail` re-CREATEs).

- **Global sensor-logging toggle + status on the Sensors landing** — *medium / S* — Add a screen-level `Logging: ON|OFF` row reading `gSensorLoggingEnabled`, dispatching `sensorlog start <path>`/`stop` (non-admin). **Correction:** bare `sensorlog start` is rejected — send a filepath (use `gSettings.sensorLogPath`, default `/logs/sensors/sensors.txt`). **Real implementation risk:** inserting a row at idx 1 shifts every sensor-row index in `g2SensorsHandleTap` (`pos=idx-1`) *and* the hard-coded `idx>=2 && idx<=5` CAM-desync window — re-base both or camera taps mis-route.

- **Surface sensor faults (+ last APDS gesture) on the landing list** — *medium / M* — Walk the ring on page-build; badge a row `FAULT` when a recent `SYSEVT_SENSOR_FAULT` matches, and stamp the APDS row with the last `SYSEVT_GESTURE`. Distinguishes a fault-trip from a clean off/missing. **Inverted footgun — correct it:** match the fault subject against the row's **label** (case-insensitive), not the canonical hardware name — subjects are literals like `"Thermal"`/`"ToF"` that substring-match `THERM`/`TOF` but never `MLX90640`/`VL53L4CX`. Best done with an explicit fault-subject field on `G2SensorRow`. Thread the badge through both `g2BuildSensorList` and `formatListRow`.

- **BLOCKED → re-scoped: one-tap canned "Alert me" automation** — *medium / L* — Buildable but the proposal's reuse path is wrong three ways. There is **no PRESENCE condition variable** (the PRES row is STHS34PF80/`gPresenceCache`; `evaluateCondition`'s MOTION reads APDS), `automationadd` is **key=value not JSON**, and there is **no `notify` command** (only `g2notify`). **Real path:** build these as *event-trigger* automations — `automationadd name=... type=event on=presence_detected|tof_object_detected|gps_fix command=<action>` (all three `SYSEVT_*` kinds exist and `type=event` is fully wired). A proper device-notification action command would have to be added first.

### 3.3 Network

- **WiFi Scan → join a NEW network (on-glasses keyboard + `wifiadd`/`openwifi`)** — *high / M* — **converts the entire scan page from look-but-don't-touch to functional.** `handleWiFiScanTap` today only logs the SSID behind a stale "G2 has no keyboard" comment, yet `g2BeginTextEntry` is used two functions away for the ESP-NOW name. Secured AP → masked-ish password entry → `wifiadd "<ssid>" "<pass>"` then `openwifi --index N` (or `--best`), re-arming the existing `gWifiPendingDeadlineMs` + `wifiPendingWatchdogTask` optimistic-UI overlay (the saved-network Connect path is the exact template). Open APs skip the keyboard. **Two real limits to surface, not swallow:** `maxLen` caps at 32 < WPA2's 63-char PSK (many passwords untypeable), and `wifiadd` is admin-gated (non-admin pairer denied).

- **ESP-NOW "Pair Mode" row (WPS-style secure pairing)** — *high / S* — **quick win.** Add a `Pair Mode` row to `showEspNowMenu` dispatching `espnowpairmode 120`/`off`/`status` (System_ESPNow.cpp:14737). Pure discrete taps, zero text input; both devices open windows and auto secure-pair. The device list literally reads `(no peers — pair via web)` today — this removes the only reason to leave the glasses to grow the mesh. Adjacent `openespnow`/`espnowenabled` rows already ride the same admin-gated `g2SubmitHijackCommand` path. **Footguns:** admin-gated (surface denial); the command self-refuses if ESP-NOW isn't initialized or no passphrase is set (put the row only in the `running==true` branch); index-lockstep with `kNameIdx*`/`kAutoIdx*`. A *live* countdown is not free (completion redraw is one-shot) — keep S as a status snapshot.

- **WiFi TX power — read-only line → cycle-on-tap row** — *medium / S* — Add a `TX Power: NdBm` row on the WiFi **action** menu (not Status) cycling a preset ladder and dispatching `wifitxpower <dBm>` (admin-gated), redrawn via `onWifiMenuRefreshDone`. The `wifiautoreconnect` row is a working template. **Footgun:** `WiFi.getTxPower()` returns the raw quarter-scale enum (~78 for 19.5 dBm) — divide by 4 for display; don't rely on it to recover the ladder rung.

- **Network-scoped live event viewer** — *medium / M* — Add a `Recent Events` drill opening a `prefersTextWidget` live-text page (~2500ms) whose `buildFn` walks `systemEventGetBySeq` newest-first and filters to network kinds (wifi/ip/peer_online/text_rx/http). `cmd_events` broadcasts to the output stream so it *can't* fill the buffer — the `buildFn` walks the accessors directly (shared low-level reads, sink-specific formatting → no duplication). Model on `kStatusPage`. All four footguns (60s watchdog, resident worker, PSRAM buffers, `gLiveTextOwnerCtx`+`CommandIdentityScope`) are pre-identified and real.

- **Live-refresh the WiFi Status page** — *medium / M* — Make RSSI/IP/GW/DNS track in real time via `g2StartLiveTextPage(buildText, 3000, ...)`. **Real path:** WiFi Status is sub-mode `NET_SUB_WIFI_STATUS`, not a registered module, so wire it with a **direct** `g2StartLiveTextPage()` call (the proven `g2ShowSensorLive`/mic-detail pattern), not a `G2PageModule` flag. The row-array→text-buffer refactor plus a tap→double-click exit regression (all sibling Network pages are tap-to-back) push this from S to M and keep value contained.

- **DEPRIORITIZED: ESP-NOW device list → per-peer RTT ping** — *low / M* — Feasible but **the capability already ships** on the ESP-NOW App page peer detail (`showPeerDetail` row 4 "Ping", driving `espnowAppPingStart`/`Poll`/`Clear`). This would be a second entry point, and the proposal mis-specifies the primitive (it prescribes the Cmd=5 mixed compound; the shipped impl uses a plain list rebuild that sidesteps the ack-freeze risk). Recommend linking the Network device row to the existing App-page detail instead.

### 3.4 ESP-NOW App

- **Show per-message delivery status in the chat views** — *medium / S* — In `chatLogicalLine`, append a 2-4 char ASCII token to `me:` lines by switching on the durable `ReceivedTextMessage.sendState` (`SEND_STATUS_PENDING/DELIVERED/TIMEOUT/FAILED`). OLED already renders this field as Delivered/No ACK/Failed; the G2 chat flattens sends to `me: %s` with no feedback — the biggest read-side parity gap on an async surface. Extract a shared `state→string` helper rather than inlining the OLED switch. **Footgun:** the RX push-kick fires only on text RX, not on an ACK state-flip, so `Pending→Delivered` won't auto-redraw (or extend the kick to fire on ACK).

- **Peer liveness markers + role/encryption on Peers/Stats** — *medium / M* — Prefix each peer row with an alive/dead marker via `MeshPeers::isHealthy(mac)`; add `Role`/`Encrypt`/`Online N/M` info rows to Stats via `getMeshRoleString` / `gEspNow->encryptionEnabled` / `MeshPeers::countHealthy`/`countActive` (all shown on OLED today). **The proposal MISSED a real memory-corruption bug:** `showStatsMenu` uses fixed `static char rows[10][40]`/`ptrs[10]` and already fills 9 rows — adding 3 overflows both arrays; **resize to ≥13 first.** Also **drop the `isSelfMac` filter** — self isn't in the paired registry, so filtering only introduces the row→device index-remap bug it separately flags.

- **WPS-style pairing-mode entry with a live countdown** — *medium / M* — Add `Pair New Device` to `showMainMenu` dispatching `espnowpairmode 45`, opening a read-only live-text countdown reading `espnowPairModeActive()`/`RemainingMs()`. The G2 App page is the natural place to add peers but has none. **Footguns (all real):** admin-gated (surface a denial state, not a stuck "open" screen); **teardown MUST call both `g2StopLiveTextPage()` and `espnowpairmode off`** or the radio window lingers; clamp to ~45s (< the 60s watchdog; shipped default is 120s, so the explicit arg is required).

- **Make the Bonded-Device sensor detail live** — *medium / M* — Today it renders once (no tick) and the push-kick filters to INBOX only, so OLED (1s timer) shows live remote-sensor data the G2 freezes. Rebuild `showBondDetailMenu` on `renderSensorDetailLive` (static back-row list child + one `UPDATE_TEXT` readout via `formatRemoteSensorReadable(e->jsonData)`). `sendCreateMixedListMultiTextAndWait` is file-static, so the new render fn must live in G2_Glasses.cpp with a thin `g2ShowBondSensorLive()` entry. Niche (one bonded remote sensor) but a genuine parity gap.

- **Live Ping RTT via a bounded redraw push-kick** — *medium / S* — The page's own deferred "Phase 2" TODO: after `espnowAppPingStart`, auto-re-check instead of manual re-tap. **Real path:** the `g2KickLivePageRefresh` reuse claim is a no-op here (no live worker running) — use a persistent `esp_timer` firing `LensJobKind::Redraw` of `showPeerDetail` (clone the Notify auto-clear timer / `enqueueWifiRedrawFromCallback`), re-arming every ~150ms and stopping on Ok/Timeout or past 2000ms. Do **not** `vTaskDelay`-block the shared lens applier.

- **Toggle remote sensor streaming from the Bonded-Device view** — *low / M* — Add a `Streaming: On/Off` row dispatching `bondstream`. **Corrections that raise effort S→M and drop value:** the command is `bondstream <sensorType> <on|off>` (the sensor arg is required); it targets the single bonded worker on a master (or the local sensor otherwise), while this view is a generic mesh-remote mirror — gate the row on `isBondMaster()` **and** a `bondPeerMac` match or it mis-targets.

- **BLOCKED: Remote device control (restart/rename/room/zone/role) from Peer Detail** — *infeasible* — There is **no `espnow cmd <mac> …` command**; those strings are only *constructed* by OLED code and resolve to nothing. The real primitive `espnowremote <target> <user> <pass> <command>` requires an account **on the remote peer** (two secrets per action) — breaking the "single-word tap-friendly" framing entirely. The UI primitives exist, but the backend does not. Do not build as specified.

### 3.5 Settings

- **Make discrete settings tap-editable via a per-setting action page** — *high / L* — **the single biggest gap on the device.** This is the only settings surface that can't mutate, while web/OLED/CLI all write through `handleSettingCommand`. Drill a tapped row into a Level-3 action page (sidesteps the cursor-reset footgun) whose rows derive from `SettingEntry.type`: BOOL toggle, enum one-row-per-option, numeric ±step, Reset-to-default; each builds `"<cmdKey> <value>"` and dispatches via `g2SubmitHijackCommand`, then a menu-gen-guarded `LensJobKind::Redraw` re-renders. `readOnly`/`isSecret` render value-only; STRING hands off to TextEntry. Reuses the registered `settingEditorCommands[]`→`handleSettingCommand` seam and the correctly-casting `formatSettingValue` (the 2026-05-18 width-read fix). **Reach blocker:** `SET_TOTAL_MODULES_ROWS=13` drops the module-list "Next" on middle pages — bump to 14 or high-index modules stay unreachable. Only cmdKey/jsonKey-resolvable settings are editable (same set OLED already edits).

- **Surface the registry metadata the page silently drops** — *medium / S* — **cheap fidelity fix and a soft prerequisite.** Render `SettingEntry.label` (not raw `jsonKey`), prefix lock/key glyphs for `readOnly`/`isSecret`, and badge modules where `SettingsModule.isConnected()` is false (the same call the web serializer already makes). **Footgun:** inserting non-tappable group-header rows shifts every list index across *both* dispatch levels (`moduleIdxFromTap` + the entry-row/paginator math) — skip headers in the index math or taps hit the wrong entry. `SET_ROW_LEN=40` truncates at 39 chars.

- **Free value entry via the TextEntry keyboard (STRING + arbitrary numeric)** — *medium / M* — An `Edit value` row for STRING/free-numeric settings arms `g2BeginTextEntry` (maxLen ≤32) seeded with the current value; `onCommit` dispatches `"<cmdKey> <typed>"` through the same `handleSettingCommand` path. `espnowNameCommit` is the working precedent. **Depends on the action page (opp 1)** — standalone it trends to L. Blank input = value unchanged for all types (a side effect: you can't clear a non-secret STRING to empty via this path). Sequence after/with the discrete editor.

- **DEPRIORITIZED: Event-driven value refresh** — *low / M* — The event-ring/live-list primitives are real, but the proposal is **partly fabricated**: the "per-setting detail value `UPDATE_TEXT`" half describes an edit page that doesn't exist (tapping a setting is a no-op), and `sendUpdateTextNamed` is file-static. The only buildable piece is converting the PRETTY list to `g2StartLiveListPage` + a `SYSEVT_SETTING_CHANGED` cursor that REBUILDs — but REBUILD resets the cursor and the page already rebuilds fresh on each drill-in, so value is low. Revisit only after the action page ships.

- **DEPRIORITIZED: Find/jump-to-setting search** — *low / M* — Input/walk/paginator primitives all fit, but the headline "tap a hit → drill to the setting" payoff is contingent on the (nonexistent) per-setting detail page; until then it can only re-navigate to the containing module list. Ship the action page first.

- **DEPRIORITIZED: Inspect a bonded peer's settings** — *low / L* — Feasible but the "reuse this page's JSON renderer" claim is false — the named helpers don't exist and the page's own `showModuleJsonViaTextWidget` is module-index-bound (can't take remote JSON). **Real path:** call the shared `BondedPeer::readCachedSettingsJson()` and render with a *new* `G2TextPager` instance (like G2_Page_Files), driven off async arrival via `LensJobKind::Redraw`. Duplicates OLED_Mode_RemoteSettings + web viewers; niche.

### 3.6 Power

- **Make the Power page live: battery + CPU-mode + uptime readout** — *high / M* — A page named "Power" shows zero power state today. Rebuild the action level as the `renderMicDetailLive` compound: action rows (Back/Restart/Power Off) CREATEd once as the list child + a ~5s `UPDATE_TEXT` readout (`Batt 87% 4.01V chg` / `CPU balanced 160MHz` / `Up 3h12m`) from `getBatteryPercentage`/`Voltage`/`isBatteryCharging`, `getPowerModeName(gSettings.powerMode)`/`getCpuFrequencyMhz()`, `SelfDevice::uptimeSeconds()`. These are unguarded getters so the ANON-identity footgun is moot. The ACTIONS↔CONFIRM transition is a shape change → `g2StopLiveTextPage` + SHUTDOWN+CREATE.

- **CPU power-mode cycle row (perf/balanced/saver/ultra)** — *high / S* — **quick win.** CPU scaling exists on OLED/web but not G2, and `power mode` is **non-admin** — the most useful battery lever a wearer can reach mid-session. Add a `CPU: <mode> <freq>MHz` row cycling `power mode 0..3` (numeric, avoids the display-name→token mismatch) and re-rendering inline via `getPowerModeCpuFreq(mode)` (synchronous). One-liner row; bump `POWER_ROW_LEN` 24→~28. The camera-settings `applyByCmd` is the exact precedent.

- **Route Restart through the shared registry (`reboot`)** — *medium / S* — Replace the direct `rebootDevice()` in `doRestart()` with `g2SubmitHijackCommand("reboot")`, matching OLED's deliberate "route through the command system for a `[CMD]` audit line." Adds the audit entry *and* the admin gate every other surface enforces. **Behavior change to surface, not silently ship:** `reboot` is admin-gated, so a non-admin pairer (or a blank `pairedByUser`) is now denied — push the "Restarting…" banner before dispatch and render the denial/queue-full via the callback (success never returns).

- **Power-save idle-timeout cycle row** — *low / S* — Add a `Power Saving: <Off|Nm>` row cycling the OLED preset ladder `{0,1,2,5,10,15,30,60}` and dispatching `powersave <minutes>` (non-admin). Value low because the OLED-off half is irrelevant on a lens; only the idle downclock matters. Nest in a CPU/Sleep submenu to avoid bumping `POWER_MAX_ROWS` on the destructive action list.

- **DEPRIORITIZED: Light Sleep action** — *low / S* — `lightsleep 20` is real but on the G2 lens a successful light sleep drops the BLE hijack link and boots the user out of the menu (it belongs on the device's own OLED), and it's admin-gated. Mirror `doPowerOff`'s inline cooldown check + `g2ShowText` if built.

- **DEPRIORITIZED: Last-reboot reason line** — *low / M* — The 48-deep RAM ring ages out exactly on the long-uptime devices where you'd ask; the durable RTC stash has no reader. Needs new boot-time capture code. The uptime half is solid via `uptimeSeconds()`; the reason half drags value down.

### 3.7 Tests

- **Command-registry path probe** — *high / M* — **closes the single most-cited gap: the transport bench never invokes the command seam it nominally validates** (zero `g2SubmitHijackCommand`/`executeCommand` calls in the whole page). Add a `Command Path →` category whose rows dispatch benign non-admin commands (`status`, `uptime`, `time`) via `g2SubmitHijackCommand`; the callback captures the `OK:`/`Error:` result and enqueues a gen-guarded `LensJobKind::Redraw` onto a `g2ShowTextAsList` result screen. This is a copy of the proven `G2_Page_Network` `enqueueWifiRedrawFromCallback` pattern — an end-to-end test of queue → cmd_exec → per-task identity → `stampOkStatus` envelope. Copy the result string into a static/PSRAM buffer (RedrawSpec.render is stateless). *(Note: the `G2_LENS_GEN_GUARD` hard-drop defaults 0 today, so the guard logs-but-dispatches — fine for a transient result line.)*

- **On-lens live diagnostics readout for the BLE Tests sub-page** — *medium / L* — Convert the BLE-Tests level to a `renderSensorDetailLive`-style compound: keep the four action rows (Hide AI Card / Toggle Mic Feed / Heap Snapshot / Lens State Dump) as the list child + a TEXT child showing heap free/min, AFE ring depth + overruns (`g2MicAfeRingDepth`/`OverrunCount`/`FeedIsActive`), lens state (`g2LensGetState`), battery — all serial-only today. A wearer with no serial console currently gets zero readout. Realistic scope is **tap-refreshed** (`UPDATE_TEXT` is fire-and-forget; the page carries one `liveIntervalMs` for all levels), so if true auto-refresh is wanted, scope a hidden sub-level enum. Dev-only audience → medium.

- **Auth allow/deny negative-path probe** — *medium / M* — Two rows: `User cmd (expect OK)` (`notifyusermute`, non-admin) and `Admin cmd (expect DENY)` (`notifydevicekind`, admin), each showing `OK:`/`Error:` on-lens — making the blank-`pairedByUser` "anonymous never permitted" failure mode testable. **Correction:** don't display `currentExecIsAdmin()` from the callback — it runs after the command's `CommandIdentityScope` has unwound (returns ANON). Diagnose from the result string + the stored `gBlePeerData[G2].pairedByUser` instead.

- **Live system-event ring viewer probe** — *medium / S* — **quick win, effort downgraded M→S.** A hidden `prefersTextWidget` live-text page (~1000ms) whose `buildText` walks `systemEventGetBySeq` newest-first from `systemEventLatestSeq` for the last ~8 events (`kindName`/`sourceName`). Simpler than the `kMicDetailPage` template (single TEXT widget, no compound). The dangerous footguns (SYSTEM_EXIT teardown, ANON identity) are already handled by the live-text framework. Doubles as a live SYSEVT-pipeline confirmation while other probes run.

- **Battery snapshot canary row** — *low / S* — Mirror `actionHeapSnapshot` with a one-tap `getBattery*` serial log; the catalog flags the absence of a battery canary among BLE Tests. Add `#include "System_Battery.h"` (currently missing); the info-dump enumerates `kActions` dynamically so no dump edit is needed. Serial-only (needs a host attach) → low.

- **BLOCKED half: Event + notification pipeline probe** — *low / M* — Keep only the event-probe row (`systemEventPost` → read-back seq → `g2ShowNotification`, ~3s self-clearing). **Drop the "Notif stats on-lens" row:** `cmd_notifstats` writes counters via `BROADCAST_PRINTF` to the output sink and returns only `"OK"`; `g2SubmitHijackCommand` routes that to FILE, so the counters never reach the lens (plus notifstats is admin-gated). Surfacing them would need new `BROADCAST_PRINTF`→lens capture plumbing, not reuse.

### 3.8 Apps

- **Make the launcher dynamic (fix the `items[4]` stack-overflow) + capability-aware rows** — *medium / S* — **latent-bug fix + enabler for every new app page.** `items[4]` is sized to exactly today's rows with no compile guard, so adding one row overflows a stack array. Grow it to a static `rows[8][64]`+`ptrs[8]` (mirror Network's `rows[12][48]`) and add capability/state labels from the signals that are *actually* runtime — ESP-NOW radio-down → `(off)`, Maps-empty → the existing `(none)` — gating new hardware rows (FM, Camera) with the same compile-time `#if ENABLE_*` the OLED local menu uses. Paginator chrome is YAGNI at ~4-6 rows; add it only when an app pushes past one screen. Keep `g2BuildAppsInfo` in sync or delete it (it already drifts).

- *(New app pages — Automations, LLM Chat, Events, Notifications, NeoPixel LED — are detailed in §4. The launcher fix above is their shared prerequisite.)*

- **DEPRIORITIZED: FM Radio app page** — *low / S* — Feasible but **redundant**: the FM controls+live-RDS compound essentially already ships on the G2 Sensors page (`g2BuildFmReadout` pushes freq/vol/mute/RSSI/RDS every tick). The only real delta is ~5 control rows — build them by extending the existing Sensors FM live-detail (see §3.2 FM tuner), not a parallel Apps app that forfeits the existing 60s-refresh/`UPDATE_TEXT`/auth mitigations.

### 3.9 TextEntry (the input enabler)

TextEntry is a primitive, not a destination — its value is unlocking the pages that consume it. The two highest-value consumers (LLM Chat, tap-editable Settings) are covered in §4 and §3.5.

- **Lens command console** — *medium / M* — A hidden `Run Command` page: `g2BeginTextEntry` → `g2SubmitHijackCommand` with a `G2CmdCookie` → the callback copies the `OK:`/`Error:` result into a static buffer and enqueues a gen-guarded `LensJobKind::Redraw` onto `g2ShowTextAsList` with a Back row. The G2 analogue of `OLED_Mode_CLIInput`. Infra reuse is excellent, but value is medium not high: char-by-char entry of a ≤32-char, non-admin command line is niche UX.

- **Ask the on-device LLM** — *high / M* — Same feature as the LLM Chat app page (§4); TextEntry is simply the prompt-entry seam. Chains `g2BeginTextEntry` → `chatBeginTurn` → `g2StartLiveTextPage` polling `chatReadStream`.

- **Masked-entry mode + symbol group → WiFi credential entry** — *medium / L* — **security-driven.** Today `finishCommit`/`finishCancel`/`handleTap` all `DEBUG_G2F` the cleartext buffer, and the buffer renders verbatim — a credential leak. Add a `mask` field to `TextEntryConfig` (net-new; it doesn't exist), a punctuation group to `kGroupChars`, and suppress the three cleartext DEBUG sites when masked. This unblocks `isSecret` SETTING_STRING editing device-wide. **Correction:** there is no `wifi connect <ssid> <pass>` command — chain `wifiadd` (admin) → `openwifi --index/--best`. The durable win is the mask/secret fix; the "self-contained WiFi onboarding" framing is niche (setup already works over serial/web) and the Network page deliberately omits lens credential entry today.

- **DEPRIORITIZED: Autocomplete/suggestion rows** — *low / M* — Only the `OLEDKeyboardAutocompleteFunc` *typedef* and the enumerables are reusable; there's no callable autocomplete core (one map-only OLED provider) and the beneficial hosts (command console, peer DM, setting-key edit) don't exist yet. Revisit once those pages ship.

- **DEPRIORITIZED: Generalize STRING setting editing** — *low / M* — Two central claims are false: no existing bool/enum edit path to piggyback on (settings are read-only no-ops), and `isSecret` masking doesn't exist. Only ~half of STRING settings even have a `cmdKey`; the editable set is a handful (espnow identity, `blename`, `blesecret`, `llmdefaultmodel`). Folds into §3.5 once the action page + masking land.

---

## 4. New app pages

The OLED→G2 gap map identifies whole OLED modes with no lens equivalent. These are the clean fits — all list/tap/live-worker plus the shared command registry, no new mechanism.

- **Automations** — *high / M* — **the cleanest missing-page fit** (OLED_Mode_Automations, `portFit: easy`). List rules with an on/off glyph, drill into a per-rule detail sub-mode with Enable/Disable/Run/Trigger rows + a live `evaluateCondition` readout. **Corrections:** commands are **id-addressed key=value** — `automation enable id=<id>` etc., not `<name>`; and the rule list must be sourced by **reading `automations.json` directly** on the `g2_tap_disp` worker (the `automation list` command dumps to output sinks, it doesn't return JSON). Enable/disable/delete are creator/admin-gated; run/trigger are not. Never author (`automationadd` is JSON-free but key=value — read/toggle/run only). Reuse: `cmd_automation`, `evaluateCondition`, `g2SubmitHijackCommand` + `LensJobKind::Redraw`.

- **LLM Chat** — *high / L* — **the marquee wearable use case** (OLED_Mode_LLM), and nothing surfaces the on-device LLM on the lens today. `g2BeginTextEntry` prompt → `chatBeginTurn` → `g2StartLiveTextPage` polling `chatReadStream` (pull-on-read) each ~700ms, with a Stop row (`chatStop`) and completion keyed on `chatReadFinished` (instant domain-gate refusals finish before the first poll). `chatBeginTurn` returns 0 if a generation is in flight (the single-flight guard) and the live worker auto-installs `gLivePageOwnerCtx` (the ANON footgun is pre-solved). **Under-weighted footgun:** a slow PSRAM-bound generation with no taps hits the 60s watchdog — keep turns short (`llmsentencelimit`) or accept clean mid-gen exit. Reuse: `System_LLMChat` (shared source of truth for web+BLE), `g2BeginTextEntry`, `g2StartLiveTextPage`.

- **Events live viewer** — *medium / S* — ~130 semantic events post device-wide with no on-lens viewer (only the OLED banner path consumes them). A `prefersTextWidget` live-text page (~2000ms) walking `systemEventLatestSeq` downward via `systemEventGetBySeq` + `systemEventKindName`/`SourceName`. **Effort S** — `kStatusPage` is a copy-paste precedent and the auth/teardown footguns are handled by the worker. This is the canonical home for the "recent events" idea that recurs on Status/Network/Tests.

- **Notifications** — *medium / M* — The v0.98.7 pipeline (per-kind levels, per-user mutes, persistent queue) has no lens surface. A mixed list+text compound: `NSINK_QUEUE` items via `notifFormatEvent`/`notifViewerResolve` (resolved against `pairedByUser`, which `g2SubmitHijackCommand` sets coherently), a live `notifstats` readout child, and a `Mutes →` sub-mode cycling `notifyusermute <kind>`. Do **not** reuse `g2ShowNotification` (it wipes the lens for `durationMs`). Refresh `gHijackStartedMs` on TAP only, never from the live push.

- **NeoPixel LED** — *medium / S* — **the cleanest end-to-end proof of "add an Apps row + host a real command page."** A paginated color picker over the 75-entry `colorTable[]` + a small effects sub-list, each row `ledcolor <name>`/`ledeffect <name>` (all non-admin). Fixed enum → zero keyboard. **Footgun:** `cmd_ledeffect` *blocks* `cmd_exec` for the effect duration (3s default, up to 60s) — never wire a duration arg into an effect row. Needs the launcher-fix's row buffer (75 colors force the paginator).

- **CLI Input / Unified exec** — *medium / M* — Covered by the Lens command console (§3.9). A Unified-menu port (local+remote action list) would ride the same exec plumbing plus the ESP-NOW App remote path; the differentiator is the remote-target selector.

**Notably NOT worth porting** (from the gap map): `OLED_Mode_SetPattern` (needs the on-device joystick — `na` by input mismatch), `OLED_Mode_Animations` (per-frame OLED loop with no G2 equivalent), `OLED_Mode_Auth`/`ChangePassword` (identity is already inherited from BLE pairing — low value), `OLED_Mode_RemoteSettings` (heaviest settings-family fit; needs remote value-entry — `hard`).

---

## 5. Cross-cutting infrastructure

These enablers recur across the opportunities and are worth landing once, deliberately:

1. **Per-child `UPDATE_TEXT` as the default live pattern** (`sendUpdateTextNamed` + a byte-diff cache, gating on the readout child only). The Status migration is the first; it removes the blank-sibling fragility *and* the cursor reset that blocks every action list. Every "controls + live readout" page (Power, Tests diagnostics, Bonded-Device, FM) reuses it.

2. **Action rows on read-only pages via the shared registry.** The pattern is uniform: a list row → `g2SubmitHijackCommand("<cmd> <arg>")` → a menu-gen-guarded `LensJobKind::Redraw`. Surface the `OK:`/`Error:` result (esp. for admin-gated commands run as `pairedByUser`); never inline heavy work on BTC_TASK; refresh `gHijackStartedMs` on tap. This single seam covers Settings edits, Power/CPU, FM/RTC/MIC/logging toggles, Network, Automations, and the command console.

3. **Event-driven redraw kicks.** For async results, prefer a persistent `esp_timer` firing `LensJobKind::Redraw` (the Notify-timer / `enqueueWifiRedrawFromCallback` pattern) over polling or, worse, blocking the shared lens applier — the correct fix for live Ping RTT. Consider extracting `enqueueWifiRedrawFromCallback` into a shared render-agnostic helper (its name is already historical).

4. **More live-tick pages, modeled on `kStatusPage`.** `g2StartLiveTextPage` with a direct `buildFn` is the substrate for Events, network-scoped events, live WiFi Status, and the Tests ring viewer. The two dangerous footguns (SYSTEM_EXIT resurrection, ANON identity) are already solved by the worker — reuse it, never spawn a task per action.

5. **A shared "enumerate queued notifications for a viewer" helper.** Today it's static in OLED_Utils. Extract `notifQueueForViewer`/`notifQueueCountForViewer` into System_Notifications so the Status badge, the Notifications app page, and OLED all call one path (the "share logic, don't duplicate" rule).

6. **Richer TextEntry: masking + a symbol group.** Add a `mask` field to `TextEntryConfig` and suppress the cleartext `DEBUG_G2F` sites — a security fix that unblocks WiFi creds *and* `isSecret` setting editing. Autocomplete is a later layer once consuming hosts exist.

7. **A generic two-step action-confirm pattern.** `kPowerPage`'s confirm sub-list is the template for any destructive tap (remote restart, delete). Route through it; bump `menuGen` on the drill-in.

---

## 6. Prioritized roadmap

Quick wins first (high value-to-effort, S–M), then core builds, then bigger bets. Effort/value are post-verification. Deprioritized/blocked items are listed last for completeness.

### Tier 1 — Quick wins (ship first)

| # | Item | Screen / Page | Value | Effort | Reuse seam |
|---|---|---|---|---|---|
| 1 | ESP-NOW **Pair Mode** row | Network | high | S | `espnowpairmode` + `onEspNowMenuRefreshDone` |
| 2 | **CPU power-mode** cycle row | Power | high | S | `power mode 0..3` + `getPowerModeCpuFreq` |
| 3 | Wall-clock **time + sync** line | Status | med | S | `Clock::isSynced`/`epochSeconds` in `buildG2StatusSnapshot` |
| 4 | Recent-events **glance line** (part 1) | Status | high | S | `systemEventGetBySeq`/`LatestSeq` |
| 5 | **Events ring viewer** probe | Tests | med | S | `g2StartLiveTextPage` + `systemEventGetBySeq` |
| 6 | **NeoPixel LED** app page | Apps (new) | med | S | `ledcolor`/`ledeffect` + `g2PaginatorWriteChrome` |
| 7 | Route **Restart via `reboot`** | Power | med | S | `g2SubmitHijackCommand` + `cmd_reboot` |
| 8 | **LLM status** line | Status | med | S | `llmGetStatus`/`llmModelDescription` |
| 9 | Dynamic launcher (**fix `items[4]`**) | Apps | med | S | static `rows[8][64]` + `#if ENABLE_*` |
| 10 | Surface **registry metadata** | Settings | med | S | `SettingEntry.label/readOnly/isSecret` + `isConnected` |
| 11 | **Per-message delivery status** in chat | ESP-NOW App | med | S | `ReceivedTextMessage.sendState` |

### Tier 2 — Core builds

| # | Item | Screen / Page | Value | Effort | Reuse seam |
|---|---|---|---|---|---|
| 12 | Status **`UPDATE_TEXT` migration** *(enabler)* | Status | high | M | `sendUpdateTextNamed` + `gStatusLast*` caches |
| 13 | **FM tuner** (seek/vol/mute) | Sensors | high | M | `fmradioseek/volume/mute` + `g2SubmitHijackCommand` |
| 14 | **WiFi join new network** | Network | high | M | `wifiadd`/`openwifi` + `g2BeginTextEntry` + `gWifiPendingDeadlineMs` |
| 15 | **Command-path probe** | Tests | high | M | `g2SubmitHijackCommand` + `LensJobKind::Redraw` |
| 16 | **Power page live** telemetry | Power | high | M | `renderMicDetailLive` + `getBattery*`/`getPowerModeName` |
| 17 | **Automations** app page | Apps (new) | high | M | `cmd_automation` (id=) + `automations.json` + Redraw |
| 18 | **Events viewer** app page | Apps (new) | med | S | `g2StartLiveTextPage` + `systemEventGetBySeq` |
| 19 | **Peer liveness + role/encrypt** *(fix `rows[10]` overflow)* | ESP-NOW App | med | M | `MeshPeers::isHealthy`/`countHealthy` + `getMeshRoleString` |
| 20 | **MIC Record** toggle | Sensors | med | S | `micrecord` + `g2MicDetailHandleTap` |
| 21 | **RTC sync** row *(admin)* | Sensors | med | S | `rtcsync from` + `g2BuildRtcReadout` |
| 22 | **Sensor-logging** toggle | Sensors | med | S | `sensorlog start <path>` + `gSensorLoggingEnabled` |
| 23 | **Storage** gauge on the meter | Status | med | M | `buildSystemInfoJson` storage + `renderCircleBar` |
| 24 | **WiFi TX-power** cycle row *(÷4 display)* | Network | med | S | `wifitxpower` + `onWifiMenuRefreshDone` |
| 25 | **Notifications** app page | Apps (new) | med | M | `notifFormatEvent`/`notifViewerResolve` + `notifyusermute` |

### Tier 3 — Bigger bets

| # | Item | Screen / Page | Value | Effort | Reuse seam |
|---|---|---|---|---|---|
| 26 | **Tap-editable Settings** action page | Settings | high | L | `handleSettingCommand` + `g2SubmitHijackCommand` + Redraw *(bump `SET_TOTAL_MODULES_ROWS`→14)* |
| 27 | **Thermal grayscale image** viewer | Sensors | high | L | `buildBmp4bppFromRgb888` (AutoLevels) + mixed list+image |
| 28 | **LLM Chat** app page | Apps (new) | high | L | `chatBeginTurn`/`chatReadStream` + `g2BeginTextEntry` + `g2StartLiveTextPage` |
| 29 | **Free-value / STRING** setting entry | Settings | med | M | `g2BeginTextEntry` + `handleSettingCommand` *(after #26)* |
| 30 | **Masked TextEntry** + WiFi creds | TextEntry / Network | med | L | new `mask` field + `wifiadd`/`openwifi` |
| 31 | **Lens command console** | TextEntry (new) | med | M | `g2SubmitHijackCommand` + `g2ShowTextAsList` + Redraw |
| 32 | **Bonded-Device live** sensor detail | ESP-NOW App | med | M | `formatRemoteSensorReadable` + `renderSensorDetailLive` |
| 33 | Live-refresh **WiFi Status** | Network | med | M | direct `g2StartLiveTextPage()` from sub-mode |
| 34 | Network-scoped **event viewer** | Network | med | M | `systemEventGetBySeq` + `g2StartLiveTextPage` |
| 35 | On-lens **Tests diagnostics** readout | Tests | med | L | mixed compound + `g2MicAfe*`/`g2LensGetState` |

### Deprioritized / blocked (do not build as originally specified)

| Item | Screen | Why |
|---|---|---|
| Remote device control from Peer Detail | ESP-NOW App | **Infeasible** — no `espnow cmd <mac>` command; `espnowremote` needs remote-account credentials (two secrets/action) |
| Per-peer RTT ping | Network | Already ships on the ESP-NOW App peer detail — link to it instead |
| FM Radio app page | Apps | Redundant — extend the existing Sensors FM live-detail (#13) |
| Settings event-driven refresh / search / bonded-peer viewer | Settings | Contingent on the (nonexistent) action page (#26); partly mis-specified |
| Light Sleep / last-reboot reason / powersave | Power | Low lens value (drops BLE link / ring ages out / OLED-off irrelevant) |
| Notif-stats-on-lens row | Tests | `cmd_notifstats` broadcasts to FILE, never returns counters to the callback |
| Autocomplete / generalized STRING editing | TextEntry | No callable autocomplete core; consuming hosts don't exist yet |
| Canned "Alert me" automation | Sensors | Re-scope to *event-trigger* automations; no PRESENCE var / no `notify` command |