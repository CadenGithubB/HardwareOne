# RAM-Flush Resume — Feasibility Assessment

**Question:** *"If I turn some features on and then off, and then I want to restart but keep the current features on when it reboots."*

**Constraint:** *"I don't want it to use the autostart settings, because those are toggleable by the user, and using that would trample on any previous settings set."*

Date: 2026-07-16 · Scope: feasibility only, no implementation.

---

## 1. Verdict

**Feasible, and much smaller than the original framing implied. Roughly 1.5–2 days to a green build, ~350 lines, most of it one-line edits to code that already exists.**

The reason is that **the restore half is already built and shipping.** There are 18 persisted `*AutoStart` flags, and a boot block that already replays them by re-running the ordinary CLI commands — `runUnifiedSystemCommand("opencamera")` (HardwareOne.cpp:1777), `"srstart"` / `"openmic"` (:1786/:1789), `"openhttp"` (:1805), `"llmload " + llmDefaultModel` (:1821), `"openmqtt"` (:1830), plus `processAutoStartSensors()` (System_I2C.cpp:2966) for the nine I2C sensors. Every feature already knows how to come back on at boot.

**The only missing half is capture** — and that omission is deliberate, documented design intent, not an oversight (System_I2C.cpp:1708-1712). Runtime start/stop never writes the autostart flag, on purpose.

Given your constraint, the feature is **a session overlay**: a small record of observed state that the boot replay consults *instead of* the autostart flag, for exactly one boot, and which never writes the flag. Intent stays intent; observation overrides it once.

The correct storage tier is **RTC_NOINIT** — 20 bytes, zero flash writes. That choice makes your constraint *structural rather than disciplinary*: a module that has no flash writer cannot trample settings.json even if a future contributor tries.

**Cost:** ~20 bytes RTC, no heap, no task, no partition, no new file format, and zero internal DRAM — which matters, because reclaiming internal DRAM is the entire point of the reboot.

**One honest caveat up front:** this resumes across a *commanded reboot* (`esp_restart()`, `ESP_RST_SW`), not across a power cut. That is a deliberate semantic choice, argued in §6 — not a limitation I'm working around.

---

## 2. The Two-Layer Model

This is the spine. Everything else follows from it.

| | **Layer 1 — Intent** | **Layer 2 — Session Overlay** |
|---|---|---|
| **What it means** | "This is what my device is *for*." | "This is what I was *doing* just now." |
| **Where** | `gSettings.*AutoStart` → settings.json | RTC_NOINIT (20 bytes) |
| **Set by** | `thermalautostart on` — a deliberate, separate act | Observed automatically at reboot |
| **Lifetime** | Forever, until the user changes it | One boot, then gone |
| **Written by this feature** | **NEVER. Not once. Not anywhere.** | Yes — it *is* the feature |
| **Survives power loss** | Yes | No (by design) |

**The precedence rule, stated plainly:**

> At boot, for each feature: **if the overlay has an entry for it, use that. Otherwise use `gSettings.<x>AutoStart`.** The overlay is then discarded.

In code this is one function, and its signature is the whole compliance story:

```c
bool resumeWant(const char* featureId, bool intent);   // overlay hit ? overlay value : intent
```

It takes intent **by value** and returns a bool. It holds no pointer into `gSettings` and cannot alias one. Every replay site changes from `if (gSettings.thermalAutoStart)` to `if (resumeWant("thermal", gSettings.thermalAutoStart))`. That is the entire integration.

Your instinct here matches the codebase's own stated principle, at BLE_Peers.cpp:222: *"Don't auto-flip autoReconnect — that's an explicit user opt-in."* Same idea, same reason.

---

## 3. What Already Works Today

**18 autostart flags** (System_Settings.h). The brief I was given said 19 — that count included `debugI2CAutoStart` (System_Settings.h:539), which is a debug-logging flag with a colliding name, not a feature.

| Flag | Decl | Default | Restores | Replayed by | Replay site |
|---|---|---|---|---|---|
| `thermalAutoStart` | :813 | `false` :270 | MLX90640 thermal | `enqueueDeviceStart(I2C_DEVICE_THERMAL)` | System_I2C.cpp:3001 |
| `tofAutoStart` | :814 | `false` :271 | VL53L4CX ToF | `enqueueDeviceStart(I2C_DEVICE_TOF)` | System_I2C.cpp:~3011 |
| `imuAutoStart` | :815 | `false` :272 | BNO055 IMU | `enqueueDeviceStart(...)` | `processAutoStartSensors()` |
| `gpsAutoStart` | :816 | `false` :273 | PA1010D GPS | `enqueueDeviceStart(...)` | `processAutoStartSensors()` |
| `fmRadioAutoStart` | :817 | `false` :274 | RDA5807 FM | `enqueueDeviceStart(...)` | `processAutoStartSensors()` |
| `apdsAutoStart` | :818 | `false` :275 | APDS9960 | `enqueueDeviceStart(...)` | `processAutoStartSensors()` |
| **`rtcAutoStart`** | :819 | **`true`** :276 | DS3231 RTC | `enqueueDeviceStart(...)` | `processAutoStartSensors()` |
| `presenceAutoStart` | :821 | `false` :278 | STHS34PF80 | `enqueueDeviceStart(...)` | `processAutoStartSensors()` |
| `inputAutoStart` | :808 | `false` :265 | Gamepad / ANO | `enqueueDeviceStart(...)` | `processAutoStartSensors()` |
| `cameraAutoStart` | :834 | `false` :289 | DVP camera | `runUnifiedSystemCommand("opencamera")` | HardwareOne.cpp:1777 |
| `srAutoStart` | :930 | `false` :349 | ESP-SR speech | `runUnifiedSystemCommand("srstart")` | HardwareOne.cpp:1786 |
| `microphoneAutoStart` | :835 | `false` :290 | PDM mic | `runUnifiedSystemCommand("openmic")` | HardwareOne.cpp:1789 / :1794 |
| **`httpAutoStart`** | :897 | **`true`** :326 | Web server | `runUnifiedSystemCommand("openhttp")` | HardwareOne.cpp:1805 |
| **`bluetoothAutoStart`** | :908 | **`true`** :333 | BLE server | `initBluetooth()` + advertise | HardwareOne.cpp:1720 |
| `mqttAutoStart` | :938 | `false` :356 | MQTT client | `runUnifiedSystemCommand("openmqtt")` | HardwareOne.cpp:1830 |
| `llmAutoStart` | :838 | `false` :291 | On-device LLM | `runUnifiedSystemCommand("llmload " + …)` | HardwareOne.cpp:1821 |
| `sensorLogAutoStart` | :824 | `false` :280 | Sensor logging | `sensorLogAutoStart()` | HardwareOne.cpp:2001 |
| `systemLogAutoStart` | :830 | `false` :285 | System logging | `systemLogAutoStart()` | HardwareOne.cpp:2002 |

Three default ON: `rtcAutoStart`, `httpAutoStart`, `bluetoothAutoStart`. Everything else is opt-in.

**The other half that's already built:** `System_FeatureRegistry.cpp:218` enumerates **25 rows** (the brief said 22), each carrying `bool* enabledSetting` pointing at its persisted flag (System_FeatureRegistry.h:39). It has no live-state getter. Adding one column turns it into a complete snapshot table.

And every live flag it would need already exists, uniformly: `gThermalEnabled`, `gTofEnabled`, `gImuEnabled`, `gInputEnabled`, `gGpsEnabled`, `gFmRadioEnabled`, `gPresenceEnabled`, `gRtcEnabled`, `gCameraEnabled`, `gMicEnabled` — all declared with stub fallbacks at System_SensorStubs.h:22-213, plus `gCameraEnabled` (System_Camera_DVP.cpp:75), `gMicEnabled` (System_Microphone.cpp:57), `llmIsReady()` (System_LLM.h:139), `isMqttConnected()` (System_MQTT.h:18).

**`sensorLogAutoStart` is the existing exemplar worth studying** — it persists last-used *parameters* live as the user changes them (System_SensorLogging.cpp:809-811, :933-939), so autostart replays config rather than just a bool. It also deliberately does *not* reopen the same file; it timestamps a new one. That principle — **resume the activity, not the handle** — is the right one and this design inherits it.

---

## 4. The One Missing Piece

Runtime start/stop never touches the autostart flag. Concretely:

```c
// i2csensor_mlx90640.cpp:195 — the START path
gThermalEnabled = true;                              // live flag only
```
```c
// i2csensor_mlx90640.cpp:1376-1379 — the SEPARATE autostart command
setSetting(gSettings.thermalAutoStart, true);        // intent only
```

Two different flags, two different commands, zero coupling. I verified this holds for **every** sensor: all the `setSetting(gSettings.*AutoStart, …)` sites live inside a `cmd_*autostart` handler. Not one start/stop path writes them.

**This is not a bug.** It is stated design intent, in a comment at System_I2C.cpp:1708-1712:

> `enabled` is the **LIVE** running flag (`g<X>Enabled`) — so the app's power toggle, which sends `open<id>`/`close<id>`, reflects reality. **(Auto-start-on-boot is a SEPARATE persisted knob**, surfaced in `controls json` as `<id>AutoStart`, **NOT this toggle.)**

So `openthermal` turns thermal on now and it is silently off after reboot — *correctly*, by the current contract.

**Frame it right:** this decoupling is not damage to repair. It is **the seam the overlay plugs into.** Because live and intent are already kept separate and already mean different things, a third thing that observes the gap between them can slot in without disturbing either. If the two had been coupled, your constraint would be unimplementable.

It also yields the precedence semantics for free — see §6.2.

---

## 5. The `gSettings` Serialization Trap

**This is the most important technical warning in this document.**

The tempting shortcut: at boot, after loading settings.json, just overlay in RAM —

```c
gSettings.thermalAutoStart = overlayValue;   // ← WRONG. Do not do this.
```

— and let the existing replay block read it. Resume for free, zero changes to the replay path.

**It silently destroys the intent you are trying to protect.** I traced the mechanism end to end:

1. **`setSetting()`** — System_Settings.h:1043-1049:
   ```c
   template<typename T>
   inline void setSetting(T& field, const T& value) {
     if (field != value) {
       field = value;
       if (!gDeferWrites) writeSettingsJson();      // ← any change, anywhere
       notifySettingChanged(&field);
     }
   }
   ```
2. **`writeSettingsJson()`** — System_Settings.cpp:1156 — merge-reads the existing file, then calls `buildSettingsJsonDoc(doc, false, true)` at **:1191**.
3. **`buildSettingsJsonDoc()`** — System_Settings.cpp:1032 — calls `writeRegisteredSettings(doc, nullptr, …)` at **:1066**.
4. **`writeRegisteredSettings()`** — System_Settings.cpp:2433 — walks every `SettingEntry` and dereferences its `valuePtr`, which points **into `gSettings`**:
   ```c
   case SETTING_BOOL:
     target[leaf] = *((bool*)e->valuePtr);      // System_Settings.cpp:2496-2497
   ```
5. The whole ~5120-byte document is then serialized atomically to flash (:1216-1229).

**So the failure looks like this.** You overlay `gSettings.thermalAutoStart = true` at boot. Nothing bad happens. Hours later the user runs an entirely unrelated command — `ledbrightness 40`. That fires `setSetting()`, which re-serializes **every registered field**, reading the *overlaid* value out of `gSettings`, and writes `thermalAutoStart: true` to settings.json permanently.

The user's configured intent is gone. The reboot didn't do it. `ledbrightness` did it, hours later, with no visible causal link. **That is a far worse outcome than the trampling you were trying to avoid** — it's the same damage, delayed and disguised.

> **THE RULE:** The overlay lives in **its own structure**, and the replay path **consults** it. It is never merged into `gSettings`. Not at boot, not ever.
>
> `resumeOverlayHas(f) ? resumeOverlayGet(f) : gSettings.<x>AutoStart`

One nuance worth knowing: `setSetting` only writes when the value actually *changes* (`if (field != value)`). So the trap doesn't fire on every call — it fires on the next call that changes something. That makes it *less frequent and therefore harder to debug*, not safer.

**Two structural defenses, both cheap:**

1. **Don't `#include "System_Settings.h"` in the overlay module.** If `gSettings` isn't a visible symbol in that translation unit, a write-back is a compile error rather than a code-review catch.
2. **Make `FeatureEntry::enabledSetting` const, or expose intent through a by-value getter.** Today it is a plain non-const `bool*` (System_FeatureRegistry.h:39). Any code holding it can write it — `*getFeatureById("thermal")->enabledSetting = x` compiles fine *without ever naming `gSettings`*, so defense (1) alone is porous. Const the pointer and the firewall is real.

That second point matters because the design in §9 reads intent during capture. It must read it through a path that cannot write.

---

## 6. The Design Decisions That Remain

### 6.1 Storage tier → **RTC_NOINIT**

The workflow is *a commanded reboot to flush RAM*. That is `esp_restart()` → `ESP_RST_SW` → **precisely the reset class RTC_NOINIT survives.** The tier's physical retention boundary and the feature's semantic boundary are the same line.

The precedent is already in your codebase and proven: HardwareOne.cpp:1145 —

> `// RTC fast memory: survives soft reset / WDT / panic but NOT power-off`

with `rebootStashReason()` (:1158-1167) magic-guarded by `REBOOT_REASON_MAGIC` (:1157) exactly because RTC_NOINIT is garbage on a cold power-on. Stash a small thing before restart, guard it, replay and consume it next boot. This feature is the same shape.

**Why not NVS:**
- It buys durability the workflow never asked for, and turns the overlay into a *second, invisible, permanent* config layer — the exact thing you objected to, relocated to a different file.
- It survives an app-partition reflash that wipes settings.json — so the overlay could outlive the intent layer it overrides, imposing a config derived from a firmware generation that no longer exists.
- The durability is partly notional anyway: `bootStateInit()` calls `nvs_flash_erase()` on `NO_FREE_PAGES` (System_BootState.cpp:19-23), which erases the whole **partition** — every namespace, including any new one. A separate namespace buys logical isolation and *zero* protection from that.
- To be honest about power-loss durability you'd need continuous capture (a brownout gives no warning), which means a background tick writing flash during normal operation — the largest possible surface for this feature to grow into the thing you vetoed.

**Why not a LittleFS file:** flash churn, plus filesystem-ready ordering problems at the consume point, plus orphan cleanup.

**Why RTC wins on your constraint specifically:** every flash-backed design satisfies "don't write the autostart flags" *by discipline*. RTC makes it **structural**. The module has no flash writer, no `setSetting()` call, no `SettingEntry` registration. `writeSettingsJson()` walks the SettingEntry tables; the overlay lives in RTC hardware and can never be in those tables. The trap in §5 becomes unreachable rather than merely avoided.

**And the power-loss "limitation" is the correct semantics, not a compromise:**
- A **commanded reboot** is a *continuation*. You were mid-session, you want your DRAM defragmented, you expect to land where you left off.
- A **power cycle** is a *discontinuity*. You ended the session. There is no "current features" to keep — the device was off. Coming up in the configured state is the only defensible answer, and it's what the device does today.

Battery pull → RTC is garbage → magic fails → pure intent. The failure mode is clean, not dangerous.

### 6.2 Precedence → **touched-only, derived as `live != intent`**

Take option (c) — but the insight is that **it costs nothing**, contrary to the assumption that touched-only needs a "touched" bit wired into ~20 toggle sites.

**Divergence *is* touch.** A feature's live state can only differ from its configured intent if someone toggled it at runtime — because §4 proved the codebase deliberately keeps them decoupled. So:

```
overlay = { f : isLive(f) != intent(f) }
```

Zero toggle-site instrumentation. Nothing to drift. And when the user changed nothing this session, **the overlay is empty and the feature is a no-op** — which is what makes automatic capture safe (§6.3).

Rejecting the alternatives:
- **(a) overlay-wins-for-all** pins features you never touched to whatever happened to be observed — including a sensor that merely failed its I2C ping this boot. That's trampling at a different layer.
- **(b) union** (resume anything live *or* autostart-set, never turn anything off) *cannot express your sentence.* You said "turn some features on and then off … keep the current features on." Union structurally cannot represent "I closed thermal, keep it closed." It would also silently re-enable a radio you deliberately switched off.

**The critical correctness property — do not lose this.** The diff baseline must be **intent**, never "state achieved at end of boot." If you baseline against achieved state, then after the overlay is applied at boot the diff reads empty, the overlay erases itself, and **resume works exactly once and then silently stops.** Baselining against `intent` means `live(thermal)=true, thermalAutoStart=false` still diffs after the resume boot, so it re-derives identical bits. **Idempotent.** Write this in a comment at the capture site so nobody "optimizes" it into a snapshot.

**A free bonus:** running `thermalautostart on` while thermal is live makes `live == intent`, so the overlay entry *evaporates on its own*. That is also the clean answer to "how do I promote a session state to permanent?" — you state the intent, and the override gets out of the way.

**The tax this owes** — and it is mandatory, not optional. `processAutoStartSensors()` is best-effort: `isSensorAvailableForAutoStart()` I2C-pings and skips on failure (System_I2C.cpp:2950-2964). So an unplugged sensor with `thermalAutoStart=true` reads `live=false ≠ intent=true` and the diff would record *"user turned it off"* — suppressing your configured autostart on every future boot, with re-plugging the sensor doing nothing to fix it. **A `sAutostartFailed` mask is required**, fed from the two verified failure sites:
- System_I2C.cpp:2961-2962 (which already posts `SYSEVT_SENSOR_START_FAILED`)
- System_Camera_DVP.cpp:938-943 (which today *clears* `cameraAutoStart` on init failure — so that flag isn't ground truth)

Be aware this is an exception bolted onto an elegant rule, and elegant rules acquire exceptions. Any *other* silent start-failure path will pin a feature off for one boot. Bounded by one-shot lifetime, but worth an audit pass.

### 6.3 Lifetime → **one-shot**; Trigger → **automatic**

**One-shot.** Consume and clear at boot; the boot after next reverts to pure intent. Sticky would make the overlay an indefinite effective config — i.e. the intent layer again, which is what you rejected. One-shot also bounds any confusion to a single boot.

**Clear *before* apply**, not after. If applying the overlay panics, the next boot returns to pure intent rather than replaying a state that may have caused the crash. This is the boot-loop hatch and it's free.

**Automatic**, on every `rebootDevice()`. A RAM flush should be transparent — you shouldn't have to remember `reboot --keep`, and forgetting it is the failure mode that makes the feature useless. This is *only* defensible because touched-only makes the overlay empty by default. Opt-*out* via `reboot --forget`.

**Hook `rebootDevice()` (System_Utils.cpp:2028), NOT `recordRebootIntent()` (:2013).** The code says why, at :2009-2012:

> `rebootDevice()` adds the inline flush + restart for simple sites; **deferred sites (factoryreset's esp_timer) call `recordRebootIntent()` directly** and restart on their own schedule.

Capture in the wrong one and **factory reset carries an overlay across the config wipe** — resuming the old feature set onto a freshly wiped device. RTC survives a factory reset. Factory reset should additionally call `resumeOverlayClear()`.

### 6.4 Inspection & clearing — non-negotiable

An invisible override layer is a debugging nightmare. The design must surface itself:

- **`resume`** — a per-feature table: `id | intent | live | overlay | effective-next-boot`. This is the literal answer to *"why is thermal on when thermalAutoStart is false?"* Because it's a diff, it's naturally short.
- **`resume clear [id]`** / **`reboot --forget`** — revert to pure intent.
- **A boot log line** per applied override, next to the existing `logSystemEvent("BOOT", …)`, plus a durable typed event so it lands in events.log.
- **`features` gains an `overlay` column** — `isFeatureEnabled()` currently reports intent as though it were truth.

One-shot is itself the strongest debuggability argument: after boot the override is *gone as a mechanism*. Thermal is simply running, and `thermalAutoStart` still honestly reads `off`.

---

## 7. Scope

**In:** on/off state for registry features with a real runtime toggle — the 9 I2C sensors, camera, microphone, ESP-SR, HTTP, Bluetooth, MQTT, LLM. Roughly 16 of the 25 rows.

**Out** — and most of these are impossible *in principle*, not merely unimplemented:

| Not resumed | Why |
|---|---|
| Web login sessions | **Deliberately** killed: `gBootId` regenerates each boot (HardwareOne.cpp:1359) and every request re-checks it. Resuming = persisting live bearer tokens to flash. |
| BLE GATT connections / per-connection auth | `connId` is controller-assigned for a live ACL link. A peripheral cannot re-initiate to a central. Restoring `authed=true` would be a privilege-escalation primitive. |
| BLE secure-channel keys | Derived from the *phone's* ephemeral X25519 half. Cannot be recreated unilaterally; persisting counters would defeat replay protection. |
| G2 hijack / lens container | A hijack can only *begin* when the wearer launches Blocks on the glasses. The firmware cannot re-hijack. |
| ESP-NOW sessions, in-flight transfers | Peer-tied; no end-to-end resume exists. |
| GPS fix | **Harmful** to restore — no consumer checks staleness, so a stale lat/lon would be consumed as live by the map, track logger, and MQTT. |
| Camera stream / LLM generation / chat context | Session-scoped; re-derived or re-requested. |
| `REQUIRES_REBOOT` features (wifi, oled, i2c) | No runtime toggle exists to preserve. |

**Also out — deliberately deferred, because they are Layer-1 bugs, not resume bugs.** Each is real and you *will* hit it:
- **FM radio** resumes ON but at 103.9 MHz / volume 6 — tuning lives only in `gFmRadioCache` (i2csensor_rda5807.h:27-29), persisted nowhere.
- **APDS** resumes ON but color-only — sub-modes (i2csensor_apds9960.cpp:32-35) aren't persisted and the start path hardcodes color.
- **LLM** resumes ON but loads `llmDefaultModel`, not what you had loaded.

These are *parameter* problems. Widening the overlay to carry parameters turns a 20-byte blob into a schema. Fix them in Layer 1 with real `SettingEntry` rows — `sensorLogAutoStart` is the exemplar.

---

## 8. Risks & Guardrails

**⛔ NON-NEGOTIABLE — boot loop.** This feature reboots *already-degraded* devices, and "restore what was on" can faithfully restore whatever wedged it. Required, in order:

1. **Reset-reason gate: `esp_reset_reason() == ESP_RST_SW`.** Same idiom already at HardwareOne.cpp:1301. A panic/WDT reboot returns to pure intent — the conservative posture for a crash you don't understand.
2. **One-shot, cleared *before* apply.** A resume-induced panic can recur at most once.
3. **Magic + integrity check word.** RTC_NOINIT is garbage on cold power-on (HardwareOne.cpp:1150-1151).
4. **Power-cycle = escape hatch, for free.** The universal "unwedge the device" gesture — pull the battery — reverts to pure intent because RTC doesn't survive it. **This is the strongest argument against NVS**, which would carry the wedge *through* the power cycle.

**⚠️ On the crash-count gate — I recommend against it, contrary to the review panel's top suggestion.** The proposal was `rtcCrashCount > 0 → discard overlay`. I traced the actual semantics at HardwareOne.cpp:1198-1211 and it doesn't hold up:

```c
if (rtcMagic != RTC_CRASH_MAGIC)        { rtcCrashCount = 0; rtcMagic = RTC_CRASH_MAGIC; }
else if (reason == ESP_RST_POWERON)     { rtcCrashCount = 0; }
else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_PANIC   || reason == ESP_RST_BROWNOUT) { rtcCrashCount++; }
```

`rtcCrashCount` **only clears on `ESP_RST_POWERON`.** `ESP_RST_SW` is in neither branch. So one panic would disable resume on *every subsequent commanded reboot, indefinitely*, until a physical power cycle. Meanwhile it adds nothing: the ESP_RST_SW gate already rejects panic-reason boots, and clear-before-apply already covers a crash during the resumed boot. It's high cost, near-zero marginal value. **Use `reboot --forget` as the escape instead.** (Latent bug found in passing: `ESP_RST_DEEPSLEEP` is in *neither* branch, so a pre-sleep crash count survives Power Off indefinitely.)

**⚠️ Deep-sleep "Power Off".** G2_Page_Power.cpp:144 is `esp_deep_sleep_start()` with no wake source — it reports `ESP_RST_DEEPSLEEP`, and RTC_NOINIT *survives* it. A user who chose Power Off and later hit reset meant *power cycle*, so intent must win. The `ESP_RST_SW` gate handles this cleanly. Belt and braces: Power Off never calls `rebootDevice()`, so it writes no overlay, and any prior overlay was consumed at the preceding boot.

**⛔ NON-NEGOTIABLE — stale layout after a reflash.** If a firmware change reorders `featureRegistry[]`, bit 7 means something different and a stale RTC blob misapplies. A hand-declared magic proves only "some HardwareOne build wrote this."
**Use a derived FNV-1a hash of the registry's id list** (plus each row's resumability) as a fourth guard. Any reorder/rename/insert/delete auto-discards the blob. **Crucially, it cannot be forgotten** the way a hand-bumped constant can — which is exactly the failure class worth engineering against. ~200 bytes of code.
Residual hole, documented rather than papered over: repointing `enabledSetting` to a different flag while keeping id `"thermal"` leaves the hash unchanged. Narrow (needs reflash + SW reset + same-session RTC survival), and you full-erase before flashing anyway.

**Flash wear:** a non-issue — RTC writes no flash. (For the record, had NVS been chosen: `bootStateIncrementBootCount()` already writes a u32 to that partition on *every* boot (System_BootState.cpp:57-64), so the wear question was never the real objection.)

**⚠️ Best-effort resume fails quietly.** `isSensorAvailableForAutoStart()` skips undetected sensors with only a log line (System_I2C.cpp:2961-2962). You asked for your features back and may get 8 of 9. This is *inherited*, not introduced — but it will be blamed on this feature. `resume` should surface the skip, and a partial-resume report ~10s after boot is worth it.

**⚠️ `processAutoStartSensors()` QUEUES, it does not start.** System_I2C.cpp:3001 is `enqueueDeviceStart(I2C_DEVICE_THERMAL)` onto `sensorQueueProcessorTask`. So `gThermalEnabled` still reads false at the end of the replay block; the sensor comes up seconds later. **This is why any design that samples a live "baseline" during boot is wrong** — it would read every autostarted sensor as off and then report them all as user-toggled-on. Diff-vs-intent is immune: intent is a static value, correct at every instant, needing no baseline and no timing heuristic.

**⚠️ Verify the weak `isLive` cells before trusting them.** `isMqttConnected()` (System_MQTT.h:18) is **link** state, not **start** state — if the broker is down at reboot, live reads false against `mqttAutoStart=true` and the overlay records a spurious "off." Same scrutiny for ESP-SR (does `srstart` leave `gMicEnabled` true? — if both rows claim live, the `else if` at HardwareOne.cpp:1786-1794 prevents a real deadlock but the `resume` listing would lie) and Bluetooth. **Rule: if a clean live predicate isn't obvious, set `isLive = nullptr` and drop the feature from the overlay rather than guess.**

**⚠️ Audio mutual exclusion.** ESP-SR and the mic are exclusive owners of I2S_NUM_0 (HAL_Audio.h:13-16) — that's why HardwareOne.cpp:1786-1794 is an `if/else if`, not two `if`s. **Substitute only the conditions; never restructure that block into a registry-driven loop.**

**⚠️ Resuming the LLM partially undoes the flush.** `llm_gen`'s ~12 KB stack is claimed at load precisely because the heap is cleanest then (System_LLM.cpp:251-259 documents a later 12 KB internal alloc *failing* once fragmentation left a ~9 KB largest block). A faithful resume re-consumes that immediately on the way up. Correct behavior — you asked for it — but `resume` should note that `resume clear llm` before a flush buys back the thing you rebooted for.

**⚠️ Minor:** `gApdsEnabled` exists (i2csensor_apds9960.cpp:32) but is *not* declared in System_SensorStubs.h (only the three sub-mode flags are, at :83-85). Its `isLive` shim needs a stub for builds without APDS.

---

## 9. Recommended Design, in Phases

**Phase 1 — Complete the registry (no behavior change).**
Add one column to `FeatureEntry` (System_FeatureRegistry.h:33-42):
```c
bool (*isLive)();   // live runtime state; nullptr = not resumable, excluded from overlay
```
Fill the ~16 resumable rows from the flags that already exist; `nullptr` for the `REQUIRES_REBOOT` rows and anything without a clean predicate. **Make `enabledSetting` const, or add a by-value `getIntent()`**, so the overlay can read intent without holding a writable pointer into `gSettings` (§5).
Ships alone, testable alone, and immediately fixes the wire bugs in §10.

**Phase 2 — The overlay module.**
New `System_ResumeOverlay.{h,cpp}`, which **does not include `System_Settings.h`**:
```c
RTC_NOINIT_ATTR static uint32_t rtcResumeMagic;       // 'RSMO'
RTC_NOINIT_ATTR static uint32_t rtcResumeLayoutHash;  // FNV-1a over registry ids + resumability
RTC_NOINIT_ATTR static uint32_t rtcResumePresent;     // bit i = feature i has an entry
RTC_NOINIT_ATTR static uint32_t rtcResumeValue;       // bit i = its live value at capture
RTC_NOINIT_ATTR static uint32_t rtcResumeCheck;       // ~(present ^ value ^ layoutHash)
static_assert(featureRegistryCount <= 32, "resume overlay is a uint32 bitfield");
```
~20 bytes. Capture is the diff (§6.2), minus `sAutostartFailed`. Four guards, in order: `ESP_RST_SW` → magic → check word → layout hash.

**Phase 3 — Wire it in.** Four one-liners plus condition swaps:
- `resumeOverlayCapture()` as the **first line of `rebootDevice()`** (System_Utils.cpp:2028) — *not* `recordRebootIntent()`.
- `resumeOverlayInit()` at HardwareOne.cpp:~1211 — validate, latch to RAM, **zero the magic immediately**.
- ~15 condition swaps: HardwareOne.cpp:1777/:1786/:1789/:1805/:1821/:1830 and the nine gates in `processAutoStartSensors()`. **Preserve the `else if` structure.**
- `sAutostartFailed` fed from System_I2C.cpp:2961-2962 and System_Camera_DVP.cpp:938-943.
- `resumeOverlayClear()` in factoryreset.

**Phase 4 — Surface it.** `resume`, `resume clear`, `reboot --forget`, boot log line, `overlay` column in `features`, durable event.

**Acceptance test (the one that matters):** toggle `openthermal` / `closecamera`, `reboot`, confirm the features return **and** `cat /system/settings.json` is **byte-identical before and after.** That last check *is* your constraint, mechanized. Then pull the battery and confirm it returns to pure intent.

Build `HW_BOARD=feathers3`.

---

## 10. Bugs This Surfaced (worth fixing regardless)

**1. ESP-NOW reports persisted INTENT as live ACTIVE status — over the wire.** *(My brief's line numbers were ~30 off; these are verified.)*
- System_ESPNow.cpp:5625 — `status.bluetoothActive = gSettings.bluetoothAutoStart ? 1 : 0;`
- System_ESPNow.cpp:5628 — `status.httpActive = gSettings.httpAutoStart ? 1 : 0;`
- System_ESPNow.cpp:5717 — `if (gSettings.httpAutoStart) cap.serviceMask |= CAP_SERVICE_HTTP;`
- System_ESPNow.cpp:5720 — `if (gSettings.bluetoothAutoStart) cap.serviceMask |= CAP_SERVICE_BLUETOOTH;`

After `closehttp`, peers still see `httpActive=1` and advertise `CAP_SERVICE_HTTP` for a server that isn't listening. The comment at :5710 literally says *"Build service mask (**runtime**…)"* while reading a setting, and the adjacent line :5714 uses live `WiFi.status() == WL_CONNECTED` — proving the intended semantics. This is intent/live conflation already shipping wrong output, and Phase 1's `isLive` column is the fix. **Land it as a separate commit** — it's wire-visible, and entangling it with a boot-path change means a bad HW validation can't tell you which half broke.

**2. `gSettings.wifiEnabled` is completely inert.** Only two references exist repo-wide: the registry pointer (System_FeatureRegistry.cpp:222) and its `SettingEntry` (System_WiFi.cpp:1487). Nothing reads it to gate anything — boot gates on the separate `wifiAutoReconnect` (HardwareOne.cpp:1606). Setting it to 0 and rebooting does **not** disable WiFi, yet `features` will report WiFi disabled while it's associated and serving HTTP.

**3. `gSettings.wifiSSID` / `wifiPassword` are persisted but never used to connect.** Read back and *displayed* (System_WiFi.cpp:98 `savedSsid`, :117 debug print) but the connect path is solely `connectToBestWiFiNetwork()` over `gWifiNetworks[]`. System_SetupWizardMode.cpp:798 calls it "the legacy single-SSID field" outright. A resume feature reading either of these as "was WiFi on / what was I on" would read values that have never meant anything.

**4. Automation ordering vs. resume.** `initAutomationSystem()` runs at HardwareOne.cpp:1399 — *before* the autostart replay at :1770-1835. So automations are live when features come back on, and edge-triggered automations can fire on the resumed transitions. This is an **ordering property that already exists today** for the autostart path; resume makes it fire more often. Flagging as a thing to watch, not a verified defect — I did not trace a specific misfire.

> **Not verified:** I was asked to report an "event-cursor gap." I found no evidence for it in this pass and am not asserting it. It may exist; treat it as unexamined rather than cleared.

---

## 11. What I'd Actually Do

**Build it.** The cost/benefit is unusually good precisely because you already built the hard half years ago and didn't notice — the replay block re-runs your own CLI commands, which means resume inherits every start path's correctness for free.

**Do this:**
1. **Phase 1 alone, first.** The `isLive` column is independently valuable, fixes real wire bugs, and de-risks the rest. If you stop here you've still improved the firmware.
2. **RTC_NOINIT, diff-vs-intent, touched-only, one-shot, automatic.** Every one of those choices makes the others safe. Touched-only → empty overlay by default → automatic capture is safe → RAM flush is transparent. One-shot → can't become a shadow config → your constraint holds over time, not just at commit.
3. **Const that `bool*`.** It's the difference between a constraint the compiler enforces and one the next contributor has to remember.
4. **Ship `resume` in the same commit as the overlay.** An override layer without an inspector is a bug generator with a long fuse.

**Skip this:**
- **The crash-count gate.** The panel ranked it #1; the code says otherwise (§8). One panic would kill resume until a physical power cycle, and it duplicates what the `ESP_RST_SW` gate already does.
- **NVS / power-loss durability.** It buys a semantic you don't want, removes the battery-pull escape hatch, needs a background flash-writing tick to be honest, and survives reflashes that wipe settings.json. If you ever *do* want power-loss survival, that's a signal you actually wanted to change intent — which is what `thermalautostart on` is for.
- **Parameters** (FM tuning, APDS sub-modes, LLM model path). Real gaps, wrong layer. Fix them as Layer-1 settings with the `sensorLogAutoStart` pattern.
- **Everything in §7's Out table.** Most is impossible; some is actively harmful.

**The one real decision you have to make** is §6.2's OFF direction. The diff is symmetric: `closehttp` (intent=on) writes `overlay[http]=OFF`, so **the web server does not come back.** I believe that's exactly what you asked for — "turn some features on and then off … keep the current features" reads as fidelity in both directions, and it's strictly better than today, where `closehttp` + reboot silently resurrects the server. But you only articulated the ON direction, and the alternative (union — never turn anything off) is a one-word change to the capture predicate. **Decide that before implementation, because it's the one thing that's expensive to change afterwards** — it changes what users' muscle memory comes to expect.

**Expectation-setting on the flush itself:** thermal's big buffers are already PSRAM (i2csensor_mlx90640.cpp:584-915, all `ps_alloc`/`PreferPSRAM`), and the session table is PSRAM too. The internal DRAM your sensors actually hold is dominated by **task stacks**, which must stay internal. A RAM-flush reboot reclaims fragmentation and stacks — genuinely valuable given DRAM is the bottleneck — but far less sensor-side DRAM than the buffer sizes suggest. And resuming the LLM hands ~12 KB of it straight back.
