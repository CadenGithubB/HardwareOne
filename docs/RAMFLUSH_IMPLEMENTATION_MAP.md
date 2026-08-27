# RAM Flush Reboot — Implementation Map

**Status:** map only. No code written. Every claim below is cited to `file:line` verified against the tree at
commit `bd872c7` (v0.98.7).

**Scope contract (settled — not up for redesign):**
- Layer 1 = intent = `gSettings.*AutoStart`. **This feature never writes them.**
- Layer 2 = session overlay = observed live state in `RTC_NOINIT`, content = the diff `{f : isLive(f) != intent(f)}`.
- Baseline the diff against **intent**, never against achieved-state.
- Consume on read: magic → copy to plain RAM → **invalidate magic** → then apply.
- Capture trigger = **only** the new RAM-flush command. A standard `reboot` behaves exactly as today.

---

## 0. Executive verdicts (read this first)

| Question | Verdict |
|---|---|
| **WiFi participates?** | **YES — but keyed to `gSettings.wifiAutoReconnect`, NOT `gSettings.wifiEnabled`.** `wifiEnabled` is verified inert. |
| **Automations participate?** | **NO — structurally impossible.** Live and intent are the same `bool`. Recommend excluding; it already survives reboot for free. |
| **Const `enabledSetting`?** | **BLOCKED. Drop it.** 3 direct writers + 5 aliased writers. |
| **Features lacking a live getter?** | **http** and **mqtt** need NEW exports. **led** has none and cannot participate. |
| **Missed `*AutoStart` readers?** | **YES — 4 families.** The `OLED_Utils.cpp` input path is the dangerous one: it re-reads intent **at runtime**, after the overlay is consumed. |
| **Prerequisite bugs?** | **3**: `cmd_rtcstart` never sets `gRtcEnabled`; http getter is `static`; mqtt started-flag is `static`. |
| Feature count | **20 participate**, 5 excluded. |

**The single most dangerous finding:** `OLED_Utils.cpp:6008` re-reads `gSettings.inputAutoStart` on every OLED menu
entry and login — see §8.1. Without a session-lifetime overlay copy, the input feature is silently resurrected
minutes after resume.

---

## 1. Participation table

Live-getter column is the **pinned** choice. The source map contained duplicate per-feature entries naming
*conflicting* getters; for `apds`, `http`, `mqtt`, `bluetooth` the wrong copy is silently harmful. What follows
is the adjudicated single answer, verified by grep.

### 1.1 Participating — I2C sensors (replay via `processAutoStartSensors`, `System_I2C.cpp:2966`)

| id | intent flag | live getter (pinned) | replay gate | notes |
|---|---|---|---|---|
| `thermal` | `gSettings.thermalAutoStart` `System_Settings.h:809` | `gThermalEnabled` `i2csensor_mlx90640.h:35` | `System_I2C.cpp:3001` | clean reference case |
| `tof` | `.tofAutoStart` `System_Settings.h:810` | `gTofEnabled` `i2csensor_vl53l4cx.h:58` | `System_I2C.cpp:3012` | task-create fail leaves flag true (`i2csensor_vl53l4cx.cpp:230`) |
| `imu` | `.imuAutoStart` `System_Settings.h:811` | `gImuEnabled` `i2csensor_bno055.h:107` | `System_I2C.cpp:3023` | clean; unwinds on every failure |
| `gps` | `.gpsAutoStart` `System_Settings.h:812` | `gGpsEnabled` `i2csensor_pa1010d.h:38` | `System_I2C.cpp:3034` | bus asymmetry, §8.4 |
| `fmradio` | `.fmRadioAutoStart` `System_Settings.h:813` | `gFmRadioEnabled` `i2csensor_rda5807.h:41` | `System_I2C.cpp:3045` | deferred init — flag true before HW confirms |
| `apds` | `.apdsAutoStart` `System_Settings.h:814` | **`(gApdsColorEnabled \|\| gApdsProximityEnabled \|\| gApdsGestureEnabled)`** `i2csensor_apds9960.h:25-27` | `System_I2C.cpp:3056` | **NEVER `gApdsEnabled`** — see §1.5 |
| `input` | `.inputAutoStart` `System_Settings.h:804` | `gInputEnabled` `i2csensor_seesaw.h:25` | `System_I2C.cpp:3067` | **OLED hazard §8.1** |
| `rtc` | `.rtcAutoStart` `System_Settings.h:815` | `gRtcEnabled` `i2csensor_ds3231.h:38` | `System_I2C.cpp:3078` | **prereq fix required §5.1** |
| `presence` | `.presenceAutoStart` `System_Settings.h:817` | `gPresenceEnabled` `i2csensor_sths34pf80.h:31` | `System_I2C.cpp:3089` | clean |

All nine share the availability gate `isSensorAvailableForAutoStart()` `System_I2C.cpp:2950`, whose false-return
site is `System_I2C.cpp:2962`. All nine are hard-gated by `gSettings.i2cBusEnabled` — early return at
`System_I2C.cpp:2971-2974`.

### 1.2 Participating — boot replay block (`HardwareOne.cpp:1770-1835`)

| id | intent flag | live getter (pinned) | replay site | notes |
|---|---|---|---|---|
| `camera` | `.cameraAutoStart` `System_Settings.h:830` | `gCameraEnabled` `System_Camera_DVP.h:17` | `HardwareOne.cpp:1777` → `"opencamera"` `:1778` | **intent self-clears at `System_Camera_DVP.cpp:941` §8.2** |
| `sr` | `.srAutoStart` `System_Settings.h:926` | `isESPSRRunning()` `System_ESPSR.h:19` | `HardwareOne.cpp:1786` → `"srstart"` `:1788` | function, not global; `#if ENABLE_ESP_SR` |
| `microphone` | `.microphoneAutoStart` `System_Settings.h:831` | `gMicEnabled` `System_Microphone.h:17` | `HardwareOne.cpp:1789` (else-if) / `:1794` (non-SR) → `"openmic"` | **coupled to `sr` §7.3** |
| `http` | `.httpAutoStart` `System_Settings.h:893` | **NEW `isHttpServerRunning()`** | `HardwareOne.cpp:1805` → `"openhttp"` `:1806` | **new export §5.2** |
| `llm` | `.llmAutoStart` `System_Settings.h:834` | **`llmGetStatus()`, test `READY \|\| GENERATING`** `System_LLM.h:143` | `HardwareOne.cpp:1821` → `"llmload " + gSettings.llmDefaultModel` `:1823-1824` | lossy on model name §8.5 |
| `mqtt` | **`.mqttAutoStart`** `System_Settings.h:934` | **NEW `isMqttStarted()`** | `HardwareOne.cpp:1830` → `"openmqtt"` `:1833` | **new export §5.3**; `mqttClientEnabled` stays a hard master gate |

### 1.3 Participating — outside the block

| id | intent flag | live getter (pinned) | replay site | notes |
|---|---|---|---|---|
| `bluetooth` | `.bluetoothAutoStart` `System_Settings.h:904` | **`gBLEState && gBLEState->initialized`** `Bluetooth.h:109` / field `Bluetooth.h:75` | `HardwareOne.cpp:1720` (outer, OR'd) + `:1747` (server branch) | **NEVER `isBLERunning()`** §1.5; inline init, not a CLI string |
| `sensorLog` | `.sensorLogAutoStart` `System_Settings.h:820` | `gSensorLoggingEnabled` `System_SensorLogging.h:100` | `System_SensorLogging.cpp:1063`, called `HardwareOne.cpp:2001` | gate at **call site**, §8.6 |
| `systemLog` | `.systemLogAutoStart` `System_Settings.h:826` | `gSystemLogEnabled` `System_Debug.h:419` | `System_Debug.cpp:3375`, called `HardwareOne.cpp:2002` | gate at call site |
| `espnow` | `.espnowenabled` `System_Settings.h:658` | `isEspNowInitialized()` `System_ESPNow_Sensors.h:125` (impl `System_ESPNow.cpp:14937`) | `HardwareOne.cpp:1961` → `cmd_espnow_init("")` `:1970` | master-enable, not `*AutoStart`; direct call |
| `wifi` | **`.wifiAutoReconnect`** `System_WiFi.cpp:1490` | `WiFi.isConnected()` | `HardwareOne.cpp:1606` → `setupWiFi()` `:1608` | see §1.4 |

### 1.4 WiFi — final recommendation: **PARTICIPATE, keyed to `wifiAutoReconnect`**

The user wants WiFi. The code supports it — but not through the flag the registry advertises.

`gSettings.wifiEnabled` is **verified inert**. Exhaustive grep for `wifiEnabled` across the tree:

```
System_Settings.h:19        wifiEnabled(true),            <- ctor default
System_Settings.h:408       bool wifiEnabled;             <- decl
System_FeatureRegistry.cpp:222  &gSettings.wifiEnabled,   <- registry display entry only
System_WiFi.cpp:1487        { "enabled", SETTING_BOOL, &gSettings.wifiEnabled, ... nullptr }  <- cmdKey is nullptr
System_SetupWizard.cpp:834/839/1443   result.wifiEnabled  <- DIFFERENT struct (System_SetupWizard.h:34), not gSettings
```

Zero functional readers. It has no command (`cmdKey == nullptr`, `System_WiFi.cpp:1487`), so it can only ever be
intent, never live — nothing to diff. The real boot gate is `gSettings.wifiAutoReconnect` at
`HardwareOne.cpp:1606` → `setupWiFi()` at `:1608`.

**Therefore:** `intent(wifi) = gSettings.wifiAutoReconnect`; `live(wifi) = WiFi.isConnected()`;
replay ON = skip nothing (let `HardwareOne.cpp:1608` run); replay OFF = **skip the `setupWiFi()` call**.

> **Do NOT run `closewifi` at boot to realize an off-overlay.** `cmd_wifidisconnect` (`System_WiFi.cpp:348`)
> stops httpd at `:358-361` **and calls `setSetting(gSettings.outWeb, false)` at `System_WiFi.cpp:365`** — a
> whole-document flash write that tramples intent. Skipping the call is the only safe off-path.

**Do NOT "fix" `wifiEnabled` in this change.** Three reasons: (a) the registry marks wifi
`FEATURE_FLAG_REQUIRES_REBOOT` (`System_FeatureRegistry.cpp:221`), so making it live changes a shipped control's
meaning inside the same change that introduces RTC boot ordering; (b) it has no command, so it can never be live;
(c) wiring it creates two intent flags for one feature — the exact ambiguity the two-layer design avoids. File
separately.

**HTTP/MQTT cascade is correct with zero extra code**, purely from boot ordering. `httpAutoStart` gates on
`WiFi.isConnected()` (`HardwareOne.cpp:1805`), `mqttAutoStart` likewise (`HardwareOne.cpp:1832`) — both read
**live** state, not the overlay. The wifi decision lands at `HardwareOne.cpp:1606`, ~200 lines earlier. So
wifi-resumes-OFF forces both to their offline branches regardless of their own overlay entries. A stranded
`http:on` entry simply evaporates. Accept one asymmetry: wifi resumes ON but the AP is out of range →
`setupWiFi()` times out silently (`System_WiFi.cpp:1465`) and http/mqtt don't resume. Identical to today. **Do not
add retry logic.**

### 1.5 Automations — final recommendation: **DO NOT PARTICIPATE**

The user wants this. The code cannot support it without a prerequisite refactor. Recommending exclusion, with the
refactor scoped below so the user can choose.

The overlay is defined as `{f : isLive(f) != intent(f)}`. For automations, **live and intent are the same memory**:

```
HardwareOne.cpp:1398   if (gSettings.automationsEnabled) { initAutomationSystem(); }   <- intent (boot init)
HardwareOne.cpp:2202   if (gSettings.automationsEnabled) { ... }                        <- live  (per-loop tick)
System_Automation.cpp:1961/1963  setSetting(gSettings.automationsEnabled, true/false);  <- runtime toggle
```

The diff is **identically empty, forever**. An overlay entry can never be produced. Any code forcing one would be
dead code dressed as a feature. `startAutomationScheduler()` / `stopAutomationScheduler()`
(`System_Automation.cpp:4032` / `:4038`) are **no-op stubs** — no task handle, no running flag. The design comment
at `System_Automation.cpp:3610-3611` says it outright: *"the only caller gates on `gSettings.automationsEnabled`,
so a disabled device never runs a tick"*.

This is the exact inverse of the sensor pattern the design is built on: sensors deliberately **decouple** runtime
toggle from the autostart flag (`System_I2C.cpp:1708-1712`), and that decoupling is what creates the divergence
the overlay captures. Automations never got that split.

**The good news — automations already behave the way the user wants, with zero new code.**
`automation system disable` calls `setSetting` (`System_Automation.cpp:1963`); `setSetting` writes flash
immediately (`System_Settings.h:1039-1045`); boot reads that same flag (`HardwareOne.cpp:1398`). **A runtime
automation toggle already survives any reboot — standard or RAM-flush.** An overlay entry would be a no-op at best.

**The hazard cuts the same way:** because the toggle *is* a `gSettings` field, routing it through the overlay means
writing session state into `gSettings`, which `System_Settings.cpp:2498` serializes whole-document to
`settings.json` — precisely the leak the design forbids.

*If the user still wants it*, this is a **prerequisite refactor, not part of this feature**: split
`gSettings.automationsEnabled` into intent + a new non-persisted `gAutomationsRunning`; repoint the live readers
(`HardwareOne.cpp:2202`; `OLED_Mode_Automations.cpp:261/:297/:548/:558`; `OLED_Utils.cpp:4874`) at the live flag;
leave `HardwareOne.cpp:1398` and `System_Filesystem.cpp:200` on intent; give `cmd_automation` a runtime-only path
that stops calling `setSetting`; and fix the latent gap that a runtime enable never calls
`initAutomationSystem()`. That touches the main loop and OLED — scope, review and HW-validate it on its own.

### 1.6 Excluded — with reasons

| id | why excluded |
|---|---|
| `automation` | §1.5 — live and intent are one `bool`. Diff always empty. |
| `oled` | `FEATURE_FLAG_REQUIRES_REBOOT` (`System_FeatureRegistry.cpp:247`). Live getter exists (`gOledEnabled`, `OLED_Display.h:358`) but **no replay path** — the entry would be unactionable. |
| `led` | **No live getter exists.** `gSettings.ledStartupEnabled` (`System_Settings.h:787`) gates a *one-shot boot animation* at `HardwareOne.cpp:1859`, not a subsystem. No persistent "LED on" state → the diff is undefined. Registry's `FEATURE_FLAG_RUNTIME_TOGGLE` is misleading here. |
| `i2c` | Master gate, `FEATURE_FLAG_REQUIRES_REBOOT`. It is the **precondition** for every sensor replay (`System_I2C.cpp:2971-2974`), not a peer. |
| `edgeimpulse` | **No replay path exists.** `grep edgeImpulseEnabled HardwareOne.cpp` → zero hits. Also three candidate getters with three meanings (`System_EdgeImpulse.h:73/:82/:95`). Nothing to replay through. |

### 1.7 Getter traps — pinned rulings (each verified)

**`apds` → NEVER `gApdsEnabled`.** Exhaustive grep, complete output:
```
i2csensor_apds9960.cpp:32   bool gApdsEnabled = false;      <- definition, the ONLY assignment
i2csensor_apds9960.h:61     extern bool gApdsEnabled;
System_ESPNow.cpp:5556      extern bool gRtcEnabled, gApdsEnabled, gFmRadioEnabled;
System_ESPNow.cpp:5580      if (gApdsEnabled) enabled |= CAP_SENSOR_APDS;
G2_Page_Sensors.cpp:327     add("APDS", "APDS9960", "apds", true, gApdsEnabled, ...);
```
Never assigned `true` anywhere. Using it: intent `true` + live permanently `false` → **every capture records a
spurious "user turned APDS off"** → resume suppresses a sensor the user never touched. Wrong direction, silent.
The sub-flags are genuinely assigned (`i2csensor_apds9960.cpp:235`, `:306`, `:312`).
*(`gApdsEnabled` being dead is a real pre-existing bug affecting `System_ESPNow.cpp:5580` and
`G2_Page_Sensors.cpp:327`. **Do not fix it here** — and never let the overlay write it. File separately.)*

**`bluetooth` → NEVER `isBLERunning()`.** Its own comment (`Bluetooth.cpp:1362-1369`) documents the deliberate
widening: *"The previous implementation only checked `gBLEState->initialized`, which is server-mode state. In
g2-client mode the BLE radio is active … so the settings UI showed 'Disabled' while BT was clearly running."* It
returns `true` on bare controller status (`Bluetooth.cpp:1370-1372`). Boot gate `HardwareOne.cpp:1720` is
`bluetoothAutoStart || wantClientForAutoReconnect`, so a G2/ring peer brings the controller up in **client** mode
with `bluetoothAutoStart == false` → `isBLERunning()` true → false divergence → next boot replays **server** init
and **silently changes the user's BLE role**. `gBLEState && gBLEState->initialized` is server-mode-only, so client
auto-reconnect yields `live == intent == false` → no entry → correct fall-through. Confirmed by
`HardwareOne.cpp:1748-1749`: *"Server-mode path only runs when the user *explicitly* asked for BT at boot."*

**`llm` → `llmIsReady()` alone is insufficient.** `System_LLM.cpp:1072-1074` returns
`gLLM.runState == LLMState::READY`, and `GENERATING` is a **distinct** enum member (`System_LLM.h:93-99`:
`UNLOADED/LOADING/READY/GENERATING/ERROR`). A capture mid-generation reads `live=false` for a loaded model →
spurious divergence. Use `llmGetStatus()` (`System_LLM.h:143`, impl `System_LLM.cpp:1076`) and test
`READY || GENERATING`.

**`mqtt` → NEVER `isMqttConnected()`.** `System_MQTT.cpp:167-169` returns `mqttTofConnected` — **broker-connected**,
not started. The codebase proves they're distinct at `System_MQTT.cpp:1234-1235`:
`doc["enabled"] = mqttEnabled; doc["connected"] = mqttTofConnected;`. Connect is async, so a started client
mid-handshake reads `live=false` vs `intent=true` → spurious "user turned MQTT off". Aggravating:
`System_MQTT.h:48` defines a stub `inline bool isMqttConnected() { return false; }` — on stub builds MQTT is
hardcoded off. *(`mqttTofConnected` is a copy-paste misnomer from ToF code — genuinely the MQTT connected flag,
not a bug, but it will mislead.)*

---

## 2. New module — `System_RamFlush.{h,cpp}`

New files: `components/hardwareone/System_RamFlush.h`, `components/hardwareone/System_RamFlush.cpp`.
Register the `.cpp` in `components/hardwareone/CMakeLists.txt` (`SRCS` list).

### 2.1 Stable feature IDs — **do not use the registry index**

The task asks what happens on a reflash that reorders the registry. The answer is that the registry is unusable as
an ID source, for a stronger reason than reordering:

**The registry array length is build-config-dependent.** `System_FeatureRegistry.cpp:353-358` wraps the
`automation` entry in `#if ENABLE_AUTOMATION` — the only conditionally-compiled entry. So index *N* denotes
**different features in different builds** of the same firmware. A registry-index-keyed overlay is wrong across a
config change, not merely across a reorder.

**Therefore: a hand-assigned, append-only enum owned by this module.** Never renumber; never reuse a retired
slot; append only. Guard reflash/enum drift with an explicit `layoutVersion` (§2.3), bumped by hand whenever the
enum changes.

```c
// System_RamFlush.h
typedef enum : uint8_t {
  RF_THERMAL = 0, RF_TOF = 1, RF_IMU = 2, RF_GPS = 3, RF_FMRADIO = 4,
  RF_APDS = 5, RF_INPUT = 6, RF_RTC = 7, RF_PRESENCE = 8,
  RF_CAMERA = 9, RF_SR = 10, RF_MICROPHONE = 11, RF_HTTP = 12,
  RF_LLM = 13, RF_MQTT = 14, RF_BLUETOOTH = 15, RF_SENSORLOG = 16,
  RF_SYSTEMLOG = 17, RF_ESPNOW = 18, RF_WIFI = 19,
  RF_FEATURE_COUNT      // 20 — must stay <= 32 (mask width)
} RamFlushFeatureId;
```

IDs are **independent of** `FeatureEntry.id` strings and of registry order. Map them to live/intent with an
explicit `switch` in the module (compile-gated per `#if ENABLE_*`), not a table of pointers — a pointer table
would reintroduce the `enabledSetting` aliasing hazard of §3.

### 2.2 The magic-guard constant + the reset-reason corroboration

Follow `HardwareOne.cpp:1144-1166`. Constant, in the style of `REBOOT_REASON_MAGIC = 0x5245424F` (`'REBO'`,
`HardwareOne.cpp:1155`):

```c
static const uint32_t RAMFLUSH_OVERLAY_MAGIC  = 0x52414D46;  // 'RAMF'
static const uint16_t RAMFLUSH_LAYOUT_VERSION = 1;           // bump on ANY enum change
```

> **The magic alone is NOT sufficient — the design under-specifies this and the cited precedent disagrees with it.**
> `HardwareOne.cpp:1301-1303` requires **two** conditions:
> ```c
> bool swReset   = ((esp_reset_reason_t)rtcLastResetReason == ESP_RST_SW);
> bool haveStash = (rtcRebootReasonMagic == REBOOT_REASON_MAGIC);
> if (swReset && haveStash) {
> ```
> with the reason at `HardwareOne.cpp:1298-1300`: *"Only ESP_RST_SW is a deliberate esp_restart(); a
> crash/watchdog/brownout/power-loss has a different reset reason and no stash, so it can't masquerade as a
> reboot."* And `HardwareOne.cpp:1151-1152`: *"Guarded by its own magic because RTC_NOINIT is garbage on a cold
> power-on."*
>
> **Mirror both conditions.** This matters concretely for G2 Power Off: `esp_deep_sleep_start()`
> (`G2_Page_Power.cpp:144`) with no wake source, woken by the reset button, re-enters `setup()` with
> `ESP_RST_DEEPSLEEP` — **not** `ESP_RST_SW`. `RTC_NOINIT_ATTR` lives in `.rtc_noinit` (RTC **slow** memory) and
> **is retained across deep sleep**. A magic-only guard fires there; the two-condition guard does not.
> Cost: one comparison. It also means the overlay is deliberately dropped after a panic mid-flush — the desired
> fail-to-intent behavior.

*While in the file:* the comment at `HardwareOne.cpp:1145` (*"RTC fast memory: survives soft reset / WDT / panic
but NOT power-off"*) is **wrong on both counts** — `RTC_NOINIT_ATTR` is RTC *slow* memory and it *is* retained
across deep sleep. Anyone reasoning about the G2 Power Off path from that comment reaches the wrong conclusion.
Fix the comment.

### 2.3 RTC_NOINIT struct layout

```c
// System_RamFlush.cpp — file-scope
RTC_NOINIT_ATTR static uint32_t rtcRfMagic;
RTC_NOINIT_ATTR static uint16_t rtcRfLayoutVersion;
RTC_NOINIT_ATTR static uint16_t rtcRfCrc;         // over the two masks + layoutVersion
RTC_NOINIT_ATTR static uint32_t rtcRfDivergeMask; // bit i => feature i was touched (live != intent)
RTC_NOINIT_ATTR static uint32_t rtcRfLiveMask;    // bit i => observed live value (only read where diverge bit set)
```

Declare them as **separate `RTC_NOINIT_ATTR` scalars**, matching the existing style at
`HardwareOne.cpp:1146-1154` — not a packed struct. Consistent with the precedent and avoids padding surprises.

**Why two masks and not one.** The settled design says the overlay is the diff, and strictly
`live == !intent` wherever a diverge bit is set — so `liveMask` is derivable. Storing it anyway costs 4 bytes and
buys drift-immunity: `gSettings.cameraAutoStart` **self-clears during the very boot that applies the overlay**
(`System_Camera_DVP.cpp:941`, §8.2), so intent at apply-time is not guaranteed equal to intent at capture-time.
With both masks, apply is `divergeMask.bit(f) ? liveMask.bit(f) : intent(f)` — self-describing and independent of
intent drift. This is an **encoding** choice, not a semantic change: untouched features have no bit and fall
through to intent exactly as specified. *(Engineer's call — but recommended, and cheap.)*

`RF_FEATURE_COUNT` is 20; both masks are `uint32_t`. **Add a `static_assert(RF_FEATURE_COUNT <= 32)`** so a future
21st feature that overflows the mask fails the build rather than silently truncating.

### 2.4 Header surface

```c
// System_RamFlush.h
#pragma once
#include <stdint.h>
#include "System_Utils.h"   // CommandEntry

typedef enum : uint8_t { /* … §2.1 … */ } RamFlushFeatureId;

// ---- Capture (runs in the ramflush command's scope, pre-reboot) -------------
// Reads every participating feature's live getter, diffs against intent, writes RTC.
// MUST NOT be called from recordRebootIntent()/rebootDevice() — see §6.1.
void ramFlushCaptureOverlay(void);

// ---- Consume (runs ONCE, early in setup(), before any apply site) -----------
// Validates ESP_RST_SW + magic + layoutVersion + crc, copies to a plain-RAM
// struct, then INVALIDATES the magic immediately. Safe to call unconditionally.
void ramFlushConsumeOverlay(void);

// ---- Apply-side query (plain RAM; valid for the WHOLE session — see §8.1) ---
// Returns the resolved decision for a feature: the overlay's value if that
// feature diverged, else the caller-supplied intent. `intent` is the snapshot
// value — see §7.2.
bool ramFlushResolve(RamFlushFeatureId f, bool intent);

// True if a consumed overlay is in effect this session (for status/debug output).
bool ramFlushOverlayActive(void);

// ---- Not-user-intent mask (plain RAM, this session only) — see §6 ----------
void ramFlushMarkAutostartFailed(RamFlushFeatureId f);
void ramFlushClearAutostartFailed(RamFlushFeatureId f);

// ---- Commands --------------------------------------------------------------
extern const CommandEntry ramFlushCommands[];
extern const size_t ramFlushCommandsCount;
```

`ramFlushResolve()` is the **only** thing the ~20 edit sites in §4 call. Keep the masks private to the `.cpp`.

**Lifetime:** the plain-RAM copy is a file-static in `System_RamFlush.cpp` with **process lifetime**, not a
`setup()` local. Required — `sensorLogAutoStart()` runs at `HardwareOne.cpp:2001`, and `tryAutoStartInputForMenu()`
runs **indefinitely after boot** (§8.1).

---

## 3. Registry change

### 3.1 The `isLive` column — **recommendation: do not add it**

The natural design is a `bool (*isLive)()` column on `FeatureEntry` (`System_FeatureRegistry.h:33-42`). Reasons to
decline:

1. **Only 20 of 25 registry entries participate** (§1.6), so the column is `nullptr` for 5 and the registry stops
   being a truth source — the caller needs the switch anyway.
2. **Four getters are not plain function pointers**: `apds` is a 3-term OR expression; `bluetooth` is
   `gBLEState && gBLEState->initialized`; `llm` is a status-struct test; `wifi` is `WiFi.isConnected()`. Each needs
   a wrapper thunk regardless — put the thunks in `System_RamFlush.cpp`, where they're colocated with the ID enum.
3. **Registry IDs are unusable as overlay keys** (§2.1), so the overlay can't iterate the registry anyway.
4. **Touching `FeatureEntry` pulls in the `enabledSetting` hazard** (§3.2) — the overlay is better off never
   holding a `FeatureEntry*` at all.

**Instead:** keep the live-getter switch inside `System_RamFlush.cpp`, keyed by `RamFlushFeatureId`. The registry
is left byte-identical. This also satisfies the design constraint that the overlay never obtains a writable handle
into intent.

### 3.2 Consting `enabledSetting` — **BLOCKED. Drop it.**

Verified: `FeatureEntry::enabledSetting` is `bool*` at `System_FeatureRegistry.h:39` and is **written through** in
three places, plus escapes as a non-const alias into two subsystems that write through the copy.

**Direct writes:**
| site | code | follow-up |
|---|---|---|
| `System_FeatureRegistry.cpp:601-602` | `bool wasEnabled = *f->enabledSetting;` / `*f->enabledSetting = enable;` | `writeSettingsJson()` `:604` |
| `System_SetupWizard.cpp:80` | `if (f && isFeatureCompiled(f) && f->enabledSetting) { *f->enabledSetting = true; n++; }` | `writeSettingsJson()` `:82` |
| `System_I2C.cpp:1130` | `*f->enabledSetting = true;` | `writeSettingsJson()` `:1134` |

`System_FeatureRegistry.cpp:601` **is** the "setFeatureEnabled-style function" — it's just not named that. It's
inside `cmd_features()` (`System_FeatureRegistry.cpp:463`), the user-facing `features <id> on|off` subcommand.
Fully wired, not dead code.

**Non-const alias escapes** (pointer copied into another struct's `bool*`, then written):
- `System_SetupWizard.cpp:392`, `:413` → written at `System_SetupWizard.cpp:720`, `:728` (`*item->setting = !*item->setting;`)
- `System_SetupWizard.cpp:529` → written at `System_SetupWizard.cpp:613` (`if (m->boolSetting) *m->boolSetting = (v != 0);`)

**Additional `bool*`-typed reads** that block a const change without signature edits: `G2_Page_Sensors.cpp:611`,
`:1151`, and `:1429` (`setSetting(*feat->enabledSetting, next);` — the macro assigns to its first argument, so
this is also a **write**).

**Ruling: const-ing is blocked at ≥8 sites and is the wrong lever anyway.** It would break three legitimate,
shipped, user-facing mutators (CLI `features <id> on|off`, first-time-setup archetype seeding, hardware
auto-detect enablement). Note the relevance: **every direct write site is followed by `writeSettingsJson()`** —
the registry pointer is a live writable handle straight into intent, and writing through it is exactly the
whole-document serialization hazard (`System_Settings.cpp:2498`). The correct protection is §3.1: **the overlay
never obtains a `FeatureEntry::enabledSetting` pointer at all.**

---

## 4. Edit sites, in dependency order

### Phase 0 — prerequisites (must land and be HW-validated first; see §5)
| # | file:line | change |
|---|---|---|
| 0.1 | `i2csensor_ds3231.cpp:793-810` | fix `cmd_rtcstart` to set `gRtcEnabled` |
| 0.2 | `WebServer_Handle.h:11` (after) | add `inline bool isHttpServerRunning()` |
| 0.3 | `System_MQTT.h:18` (near) + `System_MQTT.cpp` | add `bool isMqttStarted();` + impl + stub |

### Phase 1 — new module
| # | file:line | change |
|---|---|---|
| 1.1 | `System_RamFlush.h` (new) | header per §2.4 |
| 1.2 | `System_RamFlush.cpp` (new) | RTC scalars §2.3, capture/consume/resolve, live-getter switch, `ramFlushCommands[]` |
| 1.3 | `CMakeLists.txt` | add `System_RamFlush.cpp` to `SRCS` |
| 1.4 | `System_CLI.cpp` (module registration) | register `ramFlushCommands[]` — follow the sibling registrations |

### Phase 2 — consume (must precede every apply site)
| # | file:line | change |
|---|---|---|
| 2.1 | `HardwareOne.cpp:1342` (immediately after the sibling `rtcRebootReasonMagic = 0;`) | call `ramFlushConsumeOverlay();` |

Placing it adjacent to the existing consume keeps both RTC stashes invalidated at one point, and `:1342`
**precedes every apply site** (earliest apply is `HardwareOne.cpp:1398`; §7.1).

### Phase 3 — apply sites (each swaps an intent read for `ramFlushResolve`)
| # | file:line | current | change |
|---|---|---|---|
| 3.1 | `HardwareOne.cpp:1606` | `if (gSettings.wifiAutoReconnect) {` | `if (ramFlushResolve(RF_WIFI, gSettings.wifiAutoReconnect)) {` |
| 3.2 | `HardwareOne.cpp:1720` | `if (gSettings.bluetoothAutoStart \|\| wantClientForAutoReconnect) {` | substitute **only** the gSettings read → `if (ramFlushResolve(RF_BLUETOOTH, gSettings.bluetoothAutoStart) \|\| wantClientForAutoReconnect) {`. **Do not replace the whole condition** — §7.4 |
| 3.3 | `HardwareOne.cpp:1747` | `if (gSettings.bluetoothAutoStart) {` | same substitution; keep the `:1716-1718` `bootBleMode` computation intact |
| 3.4 | `HardwareOne.cpp:1777` | `if (gSettings.cameraAutoStart) {` | `if (ramFlushResolve(RF_CAMERA, snapCameraAutoStart)) {` — **snapshot**, §7.2 |
| 3.5 | `HardwareOne.cpp:1786` | `if (gSettings.srAutoStart) {` | `if (ramFlushResolve(RF_SR, gSettings.srAutoStart)) {` — **keep the else-if**, §7.3 |
| 3.6 | `HardwareOne.cpp:1789` | `} else if (gSettings.microphoneAutoStart) {` | `} else if (ramFlushResolve(RF_MICROPHONE, gSettings.microphoneAutoStart)) {` |
| 3.7 | `HardwareOne.cpp:1794` | `if (gSettings.microphoneAutoStart) {` (non-SR build) | `if (ramFlushResolve(RF_MICROPHONE, gSettings.microphoneAutoStart)) {` |
| 3.8 | `HardwareOne.cpp:1805` | `if (gSettings.httpAutoStart && WiFi.isConnected()) {` | `if (ramFlushResolve(RF_HTTP, gSettings.httpAutoStart) && WiFi.isConnected()) {` |
| 3.9 | `HardwareOne.cpp:1808` | `} else if (!gSettings.httpAutoStart) {` | mirror with the same resolve so the message matches the decision |
| 3.10 | `HardwareOne.cpp:1821` | `if (gSettings.llmAutoStart) {` | `if (ramFlushResolve(RF_LLM, gSettings.llmAutoStart)) {` |
| 3.11 | `HardwareOne.cpp:1830` | `if (gSettings.mqttClientEnabled && gSettings.mqttAutoStart) {` | `if (gSettings.mqttClientEnabled && ramFlushResolve(RF_MQTT, gSettings.mqttAutoStart)) {` — master gate stays **un-overridable**, §8.3 |
| 3.12 | `HardwareOne.cpp:1961` | `if (gSettings.espnowenabled && identityOk) {` | `if (ramFlushResolve(RF_ESPNOW, gSettings.espnowenabled) && identityOk) {` |
| 3.13 | `HardwareOne.cpp:2001` | `sensorLogAutoStart();` | `if (ramFlushResolve(RF_SENSORLOG, gSettings.sensorLogAutoStart)) sensorLogAutoStart();` — gate at **call site**, §8.6 |
| 3.14 | `HardwareOne.cpp:2002` | `systemLogAutoStart();` | `if (ramFlushResolve(RF_SYSTEMLOG, gSettings.systemLogAutoStart)) systemLogAutoStart();` |
| 3.15 | `System_I2C.cpp:3001` | `if (gSettings.thermalAutoStart) {` | `if (ramFlushResolve(RF_THERMAL, gSettings.thermalAutoStart)) {` |
| 3.16 | `System_I2C.cpp:3012` | `if (gSettings.tofAutoStart) {` | `RF_TOF` |
| 3.17 | `System_I2C.cpp:3023` | `if (gSettings.imuAutoStart) {` | `RF_IMU` |
| 3.18 | `System_I2C.cpp:3034` | `if (gSettings.gpsAutoStart) {` | `RF_GPS` |
| 3.19 | `System_I2C.cpp:3045` | `if (gSettings.fmRadioAutoStart) {` | `RF_FMRADIO` |
| 3.20 | `System_I2C.cpp:3056` | `if (gSettings.apdsAutoStart) {` | `RF_APDS` |
| 3.21 | `System_I2C.cpp:3067` | `if (gSettings.inputAutoStart) {` | `RF_INPUT` |
| 3.22 | `System_I2C.cpp:3078` | `if (gSettings.rtcAutoStart) {` | `RF_RTC` |
| 3.23 | `System_I2C.cpp:3089` | `if (gSettings.presenceAutoStart) {` | `RF_PRESENCE` |
| 3.24 | `OLED_Utils.cpp:4392` | `if (gSettings.inputAutoStart && gSettings.i2cBusEnabled) {` | `if (ramFlushResolve(RF_INPUT, gSettings.inputAutoStart) && gSettings.i2cBusEnabled) {` — **§8.1** |
| 3.25 | `OLED_Utils.cpp:6004` **and** `:6006` | `bool autoStart = gSettings.inputAutoStart;` (both `#if`/`#else` arms) | `bool autoStart = ramFlushResolve(RF_INPUT, gSettings.inputAutoStart);` — **the runtime resurrection path, §8.1** |

> `OLED_Utils.cpp:6003-6007` is an `#if ENABLE_ANO_ENCODER / #else` whose two arms are **byte-identical** — both
> read `gSettings.inputAutoStart`. Dead branch. Collapse it to one line while editing (cosmetic, safe), or edit
> both arms. Do not edit only one.

### Phase 4 — failure-mask instrumentation (§6)

### Phase 5 — the new commands (§5.4)

---

## 5. Prerequisites and new commands

### 5.1 PREREQ — `cmd_rtcstart` never sets `gRtcEnabled` (**confirmed bug, blocks accurate capture**)

`i2csensor_ds3231.cpp:793-810` is the **only** sensor open-command that bypasses the start queue. Verbatim:

```c
const char* cmd_rtcstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (gRtcEnabled && gRtcConnected) { return "[RTC] Already running"; }
  if (!rtcInit())       { return "Error: [RTC] Failed to initialize - check wiring"; }
  if (!createRTCTask()) { return "Error: [RTC] Failed to create task"; }
  return "[RTC] Opened successfully";     // <- :809, and gRtcEnabled was NEVER set
}
```

`rtcTask`'s main loop is `while (gRtcEnabled)` (`i2csensor_ds3231.cpp:475`), so the freshly-created task evaluates
false and exits immediately — while the command reports success.

The sibling `rtcStartInternal()` **documents this exact failure mode** and fixes it (`i2csensor_ds3231.cpp:605-609`):

```c
  gRtcEnabled = true;  // Set this BEFORE task creation — the task's main loop
                       // is `while (gRtcEnabled)`, so flipping this after
                       // createRTCTask() races and the task exits immediately
                       // (web UI then shows "RTC Closed" despite a successful
                       // openrtc command). Matches IMU/Thermal pattern.
```

The fix never propagated to `cmd_rtcstart`. **Consequence for the overlay:** user runs `openrtc`, believes RTC is
on, RAM-flush-reboots, capture reads `gRtcEnabled == false`, no divergence recorded, RTC does not come back.
RTC's overlay entry is **wrong by construction** until fixed.

**Minimal fix:** route `cmd_rtcstart` through `cmd_sensorstart_queued(I2C_DEVICE_RTC, "RTC", gRtcEnabled,
"openrtc@enqueue")` (dispatcher `System_I2C.cpp:317`), like every other sensor. Failing that, set
`gRtcEnabled = true` before `createRTCTask()`, mirroring `:605`.

### 5.2 PREREQ — export the HTTP live getter

`System_WiFi.cpp:1545` is `static bool isHttpServerRunning() { return server != nullptr; }` — file-local, in no
header, only other reference is `:1562` (a `SettingsModule` fn pointer). Not callable from the capture site.

The handle **is** reachable: `extern httpd_handle_t server;` at `WebServer_Handle.h:11`, defined
`HardwareOne.cpp:377`. **Add to `WebServer_Handle.h`:**
```c
inline bool isHttpServerRunning() { return server != nullptr; }
```
then delete the `static` duplicate at `System_WiFi.cpp:1545` so `:1562` binds to the header version (one
definition, no divergence). Canonical semantics confirmed by `cmd_httpstatus` (`System_WiFi.cpp:1017-1035`), which
open-codes the same null-check.
*Trap: do **not** use `gServerIsHttps` (`WebServer_Server.cpp:115`) as liveness — it is a mode flag, false both
when stopped and when running plain HTTP.*

### 5.3 PREREQ — export the MQTT started flag

`System_MQTT.cpp:78` is `static bool mqttEnabled = false;`, absent from `System_MQTT.h`. Add:
```c
// System_MQTT.h — real build
bool isMqttStarted();
// System_MQTT.h — stub arm, alongside the existing isMqttConnected() stub at :48
inline bool isMqttStarted() { return false; }
// System_MQTT.cpp
bool isMqttStarted() { return mqttEnabled; }
```
Set true `System_MQTT.cpp:1002`, false `:1031`. **Not** `isMqttConnected()` — §1.7.

### 5.4 The new CLI commands

**Collision check — verified clear.** Grepped `"ramflush"`, `"ramflushreboot"`, `"flushreboot"`, `"ramreboot"`,
`"resumereboot"` across all `.cpp`: **zero hits each**. Only `"reboot"` exists
(`System_Utils.cpp:2505`; also invoked as a string at `OLED_Mode_Power.cpp:132`).

**Recommendation: one command, `ramflush`.** A second command is unnecessary — capture is meaningless without the
reboot. Add a `status` subcommand for debugging rather than a second top-level name.

**`CommandEntry` shape** (`System_Utils.h:57-70`): `{ name, help, requiresAdmin, handler, usage, voiceCategory,
voiceTarget }` — the `constexpr` ctor takes 3-level voice fields with `voiceSubCategory` defaulted to `nullptr`.

**Follow `cmd_reboot`'s exact pattern** (`System_Utils.cpp:2035-2044`):

```c
const char* cmd_reboot(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  broadcastOutput("Rebooting system...");
  char detail[96];
  snprintf(detail, sizeof(detail), "commanded restart (reboot) by '%s'", currentExecUser().c_str());
  rebootDevice("command", detail, 1000);
  return "[System] Rebooting";  // Won't actually return due to restart
}
```

**`cmd_ramflush` must mirror this, plus one line before `rebootDevice()`:**

```c
const char* cmd_ramflush(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // `ramflush status` — report the last consumed overlay, no reboot.
  //   (parse argsInput; trim; compare "status")
  broadcastOutput("Capturing session state and rebooting...");
  ramFlushCaptureOverlay();                 // <- the ONLY difference from cmd_reboot
  char detail[96];
  snprintf(detail, sizeof(detail), "commanded RAM-flush restart by '%s'", currentExecUser().c_str());
  rebootDevice("ramflush", detail, 1000);   // reason string feeds the next boot's SYSEVT_REBOOT
  return "[System] RAM flush reboot";
}
```

`"ramflush"` as the reason is ≤23 chars, so it fits `rtcRebootReason[24]` (`HardwareOne.cpp:1153`) without
truncation.

**Registration**, in the "Misc" group of `systemCommands[]` next to `reboot`
(`System_Utils.cpp:2505`) — or in the module's own `ramFlushCommands[]` (preferred; keeps the new module
self-contained):
```c
{ "ramflush", "Reboot, restoring the features that are running right now.", true, cmd_ramflush,
  "Usage: ramflush [status]\n"
  "  (bare):  capture live feature state, then reboot and restore it\n"
  "  status:  show the overlay consumed at the last boot (no reboot)",
  "system", "ramflush" },
```
`requiresAdmin = true`, matching `reboot` (`System_Utils.cpp:2505`).

> **`voiceTarget`**: the 7-arg `constexpr` ctor maps arg 7 to `voiceTarget` with `voiceSubCategory = nullptr`
> (`System_Utils.h:65-80`) — i.e. `"system"`/`"ramflush"` is the 2-level form, same as `reboot`. Confirm against
> the ctor before relying on positional args.

---

## 6. The `sAutostartFailed` mask

**Purpose.** Divergence is *supposed* to mean "the user touched it". But `live == false, intent == true` also
occurs when the feature simply **failed to start** or **self-disabled** — the user never touched it. Without a
guard, capture records a spurious "user turned it off" and the resume **suppresses a feature the user still
wants**. This mask is the correction: a set bit means *"this feature's live-off is not user intent"* → capture
skips it → falls through to intent.

**Storage:** plain `static uint32_t` in `System_RamFlush.cpp`. Session-scoped; **must not** be in `RTC_NOINIT` —
it is consulted only at capture time, in the same session as the boot that set it.

**Capture rule:**
```c
bool live   = liveGetterFor(f);
bool intent = intentSnapshotFor(f);
if (live == intent) continue;                                  // no entry — falls through to intent
if (!live && (sAutostartFailed & (1u << f))) continue;         // off because it FAILED, not because user asked
setDivergeBit(f); setLiveBit(f, live);
```

### 6.1 SET sites (verified)

| feature(s) | file:line | condition |
|---|---|---|
| all 9 sensors | `System_I2C.cpp:2962` | `isSensorAvailableForAutoStart()` false-return — the site that already does `logSystemEvent("SENSOR", …)` + `systemEventPost(SYSEVT_SENSOR_START_FAILED, …)`. **One site covers all nine**; it has `deviceType` as a param. |
| all 9 sensors | `System_I2C.cpp:2758-2770+` | the queue's start verdict switch. Each arm is `xStartInternal(); INFO_I2C_AUTOSTARTF("X: %s", gXEnabled ? "SUCCESS" : "FAILED"); announceSensorStart("X", gXEnabled);` — set the bit where the flag is false after the call. **`announceSensorStart(name, ok)` is the shared chokepoint**; give it the `I2CDeviceType` (or set the bit in the switch using `req.device`). |
| thermal | `i2csensor_mlx90640.cpp:1487`, `:1527` | stack-safety bailout; I2C auto-disable |
| tof | `i2csensor_vl53l4cx.cpp:747`, `:772` | stack-safety; auto-disable |
| imu | `i2csensor_bno055.cpp:1152`, `:1167`, `:1197` | stack-safety; auto-disable |
| gps | `i2csensor_pa1010d.cpp:441`, `:483` | stack-safety; auto-disable |
| fmradio | `i2csensor_rda5807.cpp:322`, and init-fail `:223`/`:255` | stack-safety; deferred-init failure |
| apds | `i2csensor_apds9960.cpp:503`, `:582-585` | stack-safety (**watches only the COLOR flag**); auto-disable |
| input | `i2csensor_seesaw.cpp:473` / `i2csensor_ano_encoder.cpp:490` | stack-safety (**ANO arm watches `gAnoEncoderEnabled`, not `gInputEnabled`** — §8.7) |
| rtc | `i2csensor_ds3231.cpp:505` | stack-safety (`break`s the task loop) |
| presence | `i2csensor_sths34pf80.cpp:575`, `:603-604` | stack-safety; auto-disable |
| camera | `System_Camera_DVP.cpp:941` | `initCamera()` failed — §8.2 |
| http | `HardwareOne.cpp:1812` | WiFi offline → server never started |
| mqtt | `HardwareOne.cpp:1835` | WiFi offline → client never started |
| wifi | `System_WiFi.cpp:1465` | `connectToBestWiFiNetwork()` timed out (swallowed; broadcasts only) |
| sr / mic / llm | start-command failure returns | optional — a failed `srstart`/`openmic`/`llmload` at boot |

### 6.2 CLEAR sites (equally important)

A set bit must be cleared the moment the user genuinely acts, or a failed-then-user-started feature is wrongly
skipped at capture.

| file:line | why |
|---|---|
| `System_I2C.cpp:2277` (`handleDeviceStopped`, entry) | **single chokepoint for all 9 sensor stops.** A user `closeX` is a genuine touch. Clear the bit for `sensor`. |
| each `cmd_*start` success path | user explicitly started it — clear before/after the enqueue |
| `System_Camera_DVP.cpp:1091-1097` / `:1099-1103` | `opencamera` / `closecamera` |

> `handleDeviceStopped` (`System_I2C.cpp:2277-2335`) is one `switch` clearing `gThermalEnabled`, `gTofEnabled`,
> `gImuEnabled`, `gInputEnabled`, `gGpsEnabled`, `gFmRadioEnabled`, the three APDS sub-flags, `gRtcEnabled`,
> `gPresenceEnabled`. One clear call at its top covers every sensor stop.

**Decision for the engineer:** the runtime self-clear sites (auto-disable / stack-safety) are the debatable half.
A thermal sensor that auto-disabled from I2C errors 3 hours in has `live=false, intent=true`. Treating it as
"failed" (bit set) means the resume **retries** it; treating it as touch means the resume **honors the shutdown**.
**Recommend: bit set (retry).** The user never asked for it to stop, and a RAM flush is a "put me back how I was"
gesture. Retry matches today's plain-reboot behavior.

---

## 7. Ordering constraints

### 7.1 Consume strictly before every apply

Apply sites span **`HardwareOne.cpp:1398` → `HardwareOne.cpp:2002`**, and `RF_INPUT` extends to the whole session
(§8.1). Consume at `HardwareOne.cpp:1342`.

```
HardwareOne.cpp:1342   rtcRebootReasonMagic = 0;   <- existing consume
        + ramFlushConsumeOverlay();                <- INSERT HERE: validate, copy, invalidate
HardwareOne.cpp:1398   automations init            <- (excluded, §1.5)
HardwareOne.cpp:1606   wifi        (RF_WIFI)       <- earliest real apply
HardwareOne.cpp:1720   bluetooth   (RF_BLUETOOTH)
HardwareOne.cpp:1772   processAutoStartSensors()   -> System_I2C.cpp:3001-3097  (9 sensors)
HardwareOne.cpp:1777   camera      (RF_CAMERA)
HardwareOne.cpp:1786   sr / mic    (RF_SR / RF_MICROPHONE)
HardwareOne.cpp:1805   http        (RF_HTTP)
HardwareOne.cpp:1821   llm         (RF_LLM)
HardwareOne.cpp:1830   mqtt        (RF_MQTT)
HardwareOne.cpp:1961   espnow      (RF_ESPNOW)
HardwareOne.cpp:2001   sensorLog   (RF_SENSORLOG)
HardwareOne.cpp:2002   systemLog   (RF_SYSTEMLOG)
OLED_Utils.cpp:6008    input       (RF_INPUT)      <- RUNTIME, indefinitely after boot (§8.1)
```

**The boot-loop hatch holds.** `HardwareOne.cpp:1342` precedes every apply, so an apply-induced panic cannot
re-apply — the next boot is pure intent. The three FATAL sites before it (`HardwareOne.cpp:1227`, `:1242`) are
`while (1) delay(1000)` **hangs**, not resets, so they cannot loop either. Verified sound.

**The RAM copy must outlive `setup()`.** Invalidating RTC early is correct and required; the decoded plain-RAM copy
must be a module-scope static (§2.4).

### 7.2 Snapshot intent at boot entry (camera forces this)

`gSettings.cameraAutoStart` is **not stable across the boot that applies the overlay** — `System_Camera_DVP.cpp:941`
writes it during replay (§8.2). Snapshot the intent values the overlay diffs against **before any replay runs**
(alongside the consume at `HardwareOne.cpp:1342`), and feed `ramFlushResolve()` from the snapshot for `RF_CAMERA`
at minimum. The two-mask encoding (§2.3) makes this robust for every feature.

### 7.3 SR/microphone must resolve as one decision

`HardwareOne.cpp:1786-1792` is an `if/else-if` — SR **takes the mic**. Resolve both keys **through the same
if/else**, never independently, or you double-claim the I2S channel. `startESPSR()` explicitly inspects
`gMicEnabled` (`System_ESPSR.cpp:2568-2569`), so "SR running while mic-sensor also on" is a real capturable state:
resolve SR first and let it suppress the mic branch exactly as the boot code does today. Note the non-SR build has
its own copy at `HardwareOne.cpp:1794` — edit both arms.

### 7.4 Bluetooth: substitute the read, not the condition

`HardwareOne.cpp:1720` is `gSettings.bluetoothAutoStart || wantClientForAutoReconnect`. Replace **only** the
gSettings term. The `bootBleMode` computation at `:1716-1718` coerces to `BLE_MODE_G2_CLIENT` when a peer wants
auto-reconnect and must stay intact. Replay is **inline** (`initBluetooth()` / `startBLEAdvertising()` at
`:1750-1751`), not a CLI string — you cannot swap in a `runUnifiedSystemCommand("openble")`.

### 7.5 I2C bus gates all sensor replay

`processAutoStartSensors()` early-returns at `System_I2C.cpp:2971-2974` when `!gSettings.i2cBusEnabled`. If i2c
intent is off, **no sensor overlay entry can be applied**, regardless of content. Correct and desirable — do not
try to override it.

---

## 8. Open risks

### 8.1 ⚠️ MISSED READER — the OLED input path defeats the overlay **at runtime** (highest severity)

The claim that `HardwareOne.cpp:1770-1835` + `processAutoStartSensors()` are the only start-deciding intent readers
is **false**. Verified:

```
OLED_Utils.cpp:4392    if (gSettings.inputAutoStart && gSettings.i2cBusEnabled) {
OLED_Utils.cpp:4393        tryAutoStartInputForMenu();
OLED_Utils.cpp:5993    void tryAutoStartInputForMenu() {
OLED_Utils.cpp:6004/6006   bool autoStart = gSettings.inputAutoStart;      // #if/#else, identical arms
OLED_Utils.cpp:6009        if (!autoStart || !gSettings.i2cBusEnabled) return;
OLED_Utils.cpp:6031        bool enqueued = enqueueDeviceStart(I2C_DEVICE_INPUT);   // a genuine start
```

`tryAutoStartInputForMenu()` re-reads intent **itself**, and has **three callers that fire long after boot**:
- `OLED_Utils.cpp:3780` — on `oledmode menu` from the CLI
- `OLED_Utils.cpp:6238` — on becoming authenticated at the login screen
- `OLED_Mode_Auth.cpp:198` — on successful OLED login

**Concrete failure:** user runs `closeinput` → RAM-flush-reboots → overlay says `input:off` →
`processAutoStartSensors()` honors it → **then the user opens the OLED menu or logs in** →
`tryAutoStartInputForMenu()` consults `gSettings.inputAutoStart` (which the design mandates is never written), sees
`true`, and **resurrects the input device**. The overlay is silently defeated by a path that runs after the RTC
entry is already gone.

**This is why `ramFlushResolve()` must be session-lifetime, not boot-scoped.** Edit sites 3.24 + 3.25. If the
engineer prefers to accept this as a known exception, it must be documented — but it is a real user-visible
regression against the feature's own promise.

*(Stub exists for `!ENABLE_OLED_INPUT`: `OLED_Utils.cpp:6042` `void tryAutoStartInputForMenu() {}`.)*

### 8.2 ⚠️ Camera intent self-clears **on the boot path**

`System_Camera_DVP.cpp:937-943`:
```c
case CAM_PWR_CMD_START:
  if (!gCameraEnabled) {
    if (!initCamera()) {
      BROADCAST_PRINTF("[CAM_PWR] initCamera failed — reverting camera auto-start");
      setSetting(gSettings.cameraAutoStart, false);          // <- :941
      systemEventPost(SYSEVT_SENSOR_START_FAILED, "Camera", "init failed");
    }
  }
```
Boot replay `HardwareOne.cpp:1777` → `"opencamera"` → `cmd_camerastart` (`:1093`) →
`cameraPowerRequestStartSync` → this same path. So a camera that fails to init **at boot** does a whole-document
`gSettings` serialize to `settings.json` **on the boot path** (`setSetting` → `System_Settings.cpp:2498`).

Three consequences: (1) `cameraAutoStart` is **not ground truth** — it is self-mutating state, not user intent;
(2) intent can change **under** the overlay during the very boot applying it; (3) a resume that re-runs
`opencamera` and fails **silently erases the user's persisted camera intent from flash**.

**Mitigation:** snapshot intent at boot entry (§7.2) and never let the overlay itself trigger a write-back. Note
(3) is pre-existing and **not fixed** by this feature — the overlay merely must not make it worse.

### 8.3 MQTT has two flags — keep the master gate un-overridable

Replay requires **both**: `HardwareOne.cpp:1830` is `gSettings.mqttClientEnabled && gSettings.mqttAutoStart`.
Baseline the diff against `mqttAutoStart`; keep `mqttClientEnabled` as a hard gate the overlay can **never**
override — otherwise a RAM-flush could bring MQTT up with the subsystem master disabled, which `:1830` currently
forbids. Note this **conflicts with `System_FeatureRegistry.cpp:240-243`**, which points the `"mqtt"` registry
entry at `mqttClientEnabled` — another reason not to drive the overlay from the registry (§3.1).

### 8.4 GPS/FM/presence autostart probe the wrong bus (pre-existing; inherited verbatim)

`isSensorAvailableForAutoStart` pings via `i2cAddressForDeviceType(...)` on the **default** bus
(`System_I2C.cpp:2954`, `i2cPingAddress` with no bus arg), while the CLI open-commands ping with the configured
bus — `gSettings.gpsBus` (`i2csensor_pa1010d.cpp:226`), `gSettings.fmRadioBus` (`i2csensor_rda5807.cpp:516`),
`gSettings.presenceBus` (`i2csensor_sths34pf80.cpp:186`). If the device is on bus 1, the availability check probes
bus 0 and can skip a present sensor. **The overlay inherits this**: overlay says "GPS was live" → probe wrong bus →
skipped + `SYSEVT_SENSOR_START_FAILED`. Not a regression; not fixed here.
*(`tryAutoStartInputForMenu` already got this right — `OLED_Utils.cpp:6026` pings on `gSettings.inputBus`.)*

### 8.5 LLM resume is lossy on the model name

Replay is parameterized: `"llmload " + gSettings.llmDefaultModel` (`HardwareOne.cpp:1823`). A user who ran
`llmload other.bin` has `live=loaded` but the loaded model is **not** `llmDefaultModel` → a bool-only overlay
resumes the **wrong model**. The live path exists if you want to widen the slot: `llmGetStatus().modelPath`
(`System_LLM.h:103`, populated `System_LLM.cpp:1079`). Round-trips cleanly — `cmd_llm_load` takes the
`startsWith("/")` branch (`System_LLM.cpp:2595`) for an absolute path. **Recommend: accept lossy for v1, document.**

### 8.6 sensorLog/systemLog — gate at the call site, not inside the function

Both inline their restore rather than re-running a CLI command, and read intent on their first line
(`System_SensorLogging.cpp:1063`, `System_Debug.cpp:3375`). **Gate at `HardwareOne.cpp:2001`/`:2002`** — it keeps
the overlay out of both modules and keeps them symmetric. Critically, `sensorLogAutoStart()` has a **second,
runtime caller**: `i2csensor_pa1010d.cpp:718` (GPS track-logging). Gating inside the function would change
`cmd_gpslog` behavior; gating at the call site does not.

> **Related pre-existing intent-writer, flagged not fixed:** `cmd_gpslog` writes intent at **runtime** —
> `i2csensor_pa1010d.cpp:695` `cmd_gpsautostart("on")` and `:696` `cmd_sensorlog("autostart on")`. So `gpslog`
> silently turns on two autostart flags. Independent of this feature, but it means GPS/sensorLog intent can change
> without the user knowingly editing intent.
>
> *(Task-prompt correction: the prompt attributed `System_Debug.cpp:3374-3409` to `sensorLogAutoStart`; that range
> is in fact `systemLogAutoStart()`. `sensorLogAutoStart()` lives at `System_SensorLogging.cpp:1062+`.)*

### 8.7 Input: two flags, one feature

Under `ENABLE_ANO_ENCODER` the ANO driver sets **both** `gInputEnabled` (`i2csensor_ano_encoder.cpp:249`) and its
own `gAnoEncoderEnabled` (`:248`), but the ANO stack-safety bailout watches only `gAnoEncoderEnabled`
(`i2csensor_ano_encoder.cpp:490`) — leaving `gInputEnabled` **stale-true**. Pre-existing divergence.
**Read `gInputEnabled` only; do not OR them.** It is the driver-agnostic flag, it is what the queue reads
(`System_I2C.cpp:2720`), and it is what `cmd_openinput` passes as `enabledFlag` (`HAL_Input.cpp:263`).

Second ordering note: `HardwareOne.cpp:1568` calls `inputStartInternal()` **directly** during boot, gated on
first-time-setup + OLED (`HardwareOne.cpp:1558-1568`), ignoring `inputAutoStart` entirely. It only fires when
first-time setup is needed — by definition not a RAM-flush-resume scenario — but **apply must not assume
`gInputEnabled == false` at replay time**. The queue's `alreadyRunning` guard (`System_I2C.cpp:2720`) already
handles the collision by skipping.

### 8.8 APDS resume is coarse (decision)

Intent is one bool; live is three sub-flags. `apdsStartInternal` always comes up **color-only**
(`i2csensor_apds9960.cpp:306`) — proximity/gesture are never restored. So a one-bit overlay silently downgrades a
user who had proximity/gesture on via `apdsmode`. Options: **(a)** accept the downgrade — matches today's autostart
behavior exactly, zero new code; **(b)** make APDS a 3-bit entry and replay `apdsmode <mode> on` after the queued
start. **Recommend (a) for v1**, documented.

### 8.9 Capture-accuracy caveats (accept; replay just retries)

- **fmradio**: `fmRadioStartInternal` returns **true** (`i2csensor_rda5807.cpp:385`) after merely setting the flag
  (`:367`) and requesting **deferred** init (`:368`) — it does not wait for the result. Real init failure clears
  the flag later (`:223`/`:255`). `gFmRadioEnabled == true` is **not** proof of hardware presence.
- **tof**: `createToFTask()` failure (`i2csensor_vl53l4cx.cpp:230-233`) returns false **without clearing**
  `gTofEnabled` (set true at `:207`) — a capture right after a failed `opentof` records "on" for a sensor that
  isn't running.

### 8.10 Do NOT put capture in the shared reboot helpers

`recordRebootIntent()` (`System_Utils.cpp:2013`) is a **shared chokepoint** — its own comment
(`System_Utils.cpp:2007-2012`) says *"Every intentional restart routes through recordRebootIntent()"*. Verified
callers:

| caller | file:line |
|---|---|
| `cmd_reboot` | `System_Utils.cpp:2042` (via `rebootDevice`) |
| G2 power menu | `G2_Page_Power.cpp:111` (via `rebootDevice`) |
| factory reset | `System_Utils.cpp:2130` (direct; esp_timer restart at `:2089`) |
| first-time setup ×4 | `System_FirstTimeSetup.cpp:165` (`rebootWithMessage`), called at `:367`, `:655`, `:947`, `:973` |

Capture there would make **all seven** capture an overlay — violating the non-negotiable "a standard reboot behaves
exactly as today". The FTS cases are worst (they'd replay live state over settings the wizard just wrote);
factoryreset is nearly as bad (it reboots to re-trigger the wizard). **`cmd_ramflush` carries its own
`ramFlushCaptureOverlay()` call and `recordRebootIntent()`/`rebootDevice()` stay byte-identical.**

### 8.11 Complete restart-sink enumeration (for the guard's completeness)

Verified by globbing every `.cpp`/`.h` for `esp_restart|ESP.restart|esp_deep_sleep_start` (excluding the embedded
DarkRoom/LLM JS blobs, which produce false `abort()` hits). **Exactly three sinks:**
1. `System_Utils.cpp:2031` — `rebootDevice()` inline `ESP.restart()`
2. `System_Utils.cpp:2089` — `factoryreset_doRestart()` esp_timer callback
3. `G2_Page_Power.cpp:144` — `esp_deep_sleep_start()`, no wake source (§2.2)

**OTA does not exist.** `grep -rlE "esp_ota|https_ota|Update\.begin"` over the component returns **zero files** —
the OTA concern is vacuous. Panic/WDT/brownout have no code path; hardware resets, nothing writes RTC on the way
out. None of the three sinks touches an overlay magic, so "only the RAM-flush command sets it" holds **by
construction** — provided §8.10 is honored.

### 8.12 Non-refuters — intent readers that do NOT decide a start (no overlay work)

Checked and cleared, listed so the next reader doesn't re-investigate:
- `System_ESPNow.cpp:5717` / `:5720` — capability broadcast (`serviceMask`); `:5625` / `:5628` — status.
  *(Pre-existing bug: uses intent as a proxy for live service state. Out of scope.)*
- `Bluetooth.cpp:1603` — `bleSecurityBootNotice()`, a boot **notice**. Same intent-as-proxy smell; not a start.
- `System_SetupWizard.cpp:556`, `:640`, `:648` — wizard page visibility. `:880` — triggers certgen, not a start.
- `System_FirstTimeSetup.cpp:967` — `if (gInputEnabled && !gSettings.inputAutoStart)` sets
  `needsRebootForHardware`; a reboot decision, not a start.
- `Bluetooth.cpp:1922`, `System_Utils.cpp:1610`, `WebPage_MQTT.cpp:166`, `OLED_Mode_Logging.cpp:101`,
  `G2_Page_Network.cpp:671/:1802` — status text.
- All `SettingEntry` table rows and `*autostart` command handlers — intent-only by design.

### 8.13 Direct intent writers worth knowing (not fixed here)

These bypass `setSetting` or write intent from non-user context. None blocks the feature; all are worth a glance if
intent ever looks wrong: `System_FirstTimeSetup.cpp:794` (`gSettings.httpAutoStart = true;`),
`System_FirstTimeSetup.cpp:523/:793/:833` (`wifiAutoReconnect`), `OLED_SetupWizard.cpp:816` and
`System_SetupWizard.cpp:1205` (`gSettings.mqttAutoStart = false;`), plus the three registry writers of §3.2.

---

## 9. Summary of prerequisite work

| # | prereq | file:line | blocks |
|---|---|---|---|
| 1 | `cmd_rtcstart` must set `gRtcEnabled` | `i2csensor_ds3231.cpp:793-810` | RTC's overlay entry is wrong-by-construction without it |
| 2 | export `isHttpServerRunning()` | `System_WiFi.cpp:1545` → `WebServer_Handle.h:11` | http cannot be captured |
| 3 | export `isMqttStarted()` | `System_MQTT.cpp:78` → `System_MQTT.h` | mqtt cannot be captured |

Land, build, and HW-validate these three **before** the overlay module, per the project's
"no incremental commits during refactor" convention — finish the multi-phase work, HW-test, then commit once.

**Explicitly NOT fixed here** (file separately): dead `gApdsEnabled` (`i2csensor_apds9960.cpp:32`, breaking
`System_ESPNow.cpp:5580` + `G2_Page_Sensors.cpp:327`); inert `gSettings.wifiEnabled`; the GPS/FM/presence autostart
bus asymmetry (§8.4); the camera boot-path intent erase (§8.2); `cmd_gpslog`'s runtime intent writes (§8.6); the
wrong `HardwareOne.cpp:1145` RTC-memory comment (§2.2 — cheap, fix in passing).
