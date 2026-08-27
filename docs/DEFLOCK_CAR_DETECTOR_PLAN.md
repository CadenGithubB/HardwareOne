# DeFlock Car Detector — Implementation Plan

**Audience:** Another AI / implementer. This is the product + architecture plan for a car-mounted HardwareOne node. Follow these constraints literally; do not “optimize away” continuous sniff or reintroduce micro duty-cycled radio teardown.

**Date:** 2026-07-20 (Rev 2 — reconciled against the actual codebase)
**Related research:** [docs/DEFLOCK_RESEARCH_FEASIBILITY.md](DEFLOCK_RESEARCH_FEASIBILITY.md)
**Status:** Plan only — implement when explicitly requested.

---

## 0. Rev 2 changes (why this differs from the first draft)

Rev 1 was written before checking the tree. A codebase review corrected several
assumptions. Read this section first — some Rev 1 “non-negotiables” were wrong.

- **This is a compile-time stripped build, not a runtime toggle of the shipping
  firmware.** The current full image (`build/hardwareone-idf.bin` = 5,526,528 B)
  already **overflows** the QT Py’s 8 MB factory partition
  (`partitions_no_sr_8mb.csv` factory `0x535000` = 5,459,968 B) by ~65 KB, before
  any sniffer code. Classic-ESP32 internal SRAM has already overflowed once
  (`iram0_0_seg` by 468 B with just BT + G2; see `sdkconfig.defaults.esp32`). So
  “toggleable feature” means: a **stripped detector build profile** (its own
  product image), inside which `silent / geofenced / always_rf` is a runtime mode.
  See [§1a](#1a-build-model--what-toggleable-actually-means).
- **The promiscuous callback ring must be INTERNAL DRAM, not PSRAM** (Rev 1 §6.1.5
  said “PSRAM” — that is unsafe on classic ESP32: no `SPI_FLASH_AUTO_SUSPEND`, so a
  PSRAM access from Wi-Fi-callback context can stall/fault on a flash cache-miss).
- **BLE + continuous Wi-Fi promiscuous is a physics tradeoff, not a scheduling
  one** (single radio time-shares). BLE is now a **compile-time OFF-by-default**
  option, and if used, **passive** scan only. Resolves the Rev 1 §2.1-vs-§6.2
  contradiction.
- **`System_RfMode` is a *retrofit* of a radio arbiter, not a new module beside
  the existing radio code.** There is no single Wi-Fi owner today. See [§9](#9-architecture--the-radio-arbiter-retrofit).
- **Detection is probabilistic on a single pass** — Rev 1 §6.3 (“main miss is
  wrong channel”) was an overclaim. See [§6.3](#63-detection-probability-be-honest).
- **Bystander data-minimization is now a hard privacy rule** — persist only
  matched hits; never write third-party MACs to disk. See [§4.5](#45-bystander-data-minimization-hard-rule).
- **IMU is net-new hardware + validation**, not a config flip (BNO055 driver
  exists but is compiled out and has only ever run on the S3). See [§8](#8-track-logging-non-rf).

---

## 1. Product intent

Build a **QT Py ESP32** car unit (Stemma QT: GPS, RTC, IMU) that:

1. **Remembers where I’ve been** for a few days (GPS track log; IMU assists dead-reckoning / rough motion when GPS is weak).
2. **Detects nearby Flock / ALPR RF signatures** when hunting, using a hybrid of:
   - **Packed on-device ALPR location DB** (DeFlock/OSM-style lat/lon packs) for geofencing and “known quiet camera” awareness.
   - **Continuous Wi‑Fi promiscuous sniff** (± BLE scan, off by default) for live Flock/Raven signatures when in a detect mode.

A second ESP at home will eventually sync tracks/hits over **ESP‑NOW**. House sync is **out of scope for v1 detection work** — design so it doesn’t fight RF privacy/modes, but do not block on ironing out sync.

### Primary board

- Adafruit **QT Py ESP32** (ESP32-PICO-V3-02: classic ESP32, dual LX6, **2 MB Quad PSRAM**, 8 MB flash, **no native USB**, BT-Classic + BLE), Stemma QT GPS + RTC + IMU.
- The board is **already a first-class target**: `boards/qtpy_esp32.defaults` is the default for `idf.py set-target esp32`, with a complete pin block at `System_BuildConfig.h:727` (`ARDUINO_ADAFRUIT_QTPY_ESP32_DEV`) and `partitions_no_sr_8mb.csv` auto-selected. Board bring-up is essentially zero. See `docs/BOARD_SWITCHING.md`.

### 1a. Build model — what “toggleable” actually means

`HW_BOARD` switches **sdkconfig / pins / partitions only**. It does **not** switch
the application feature set — that lives in `System_BuildConfig.h` as one global
block currently carrying the **FeatherS3 full profile** (LLM + BT + G2 + full web
+ sensors + OLED). That full profile does not fit the QT Py (see §0). Therefore:

- The detector is a **stripped feature profile** on the `esp32` target. Minimum
  edits to `System_BuildConfig.h` for a coherent, fitting image:
  - `ENABLE_ONDEVICE_LLM 0` — **footgun:** it defaults to `1`, its CMake gate
    (`CMakeLists.txt:399`, `if(HW_CFG_ENABLE_LLM GREATER 0)`) has **no target
    check**, and the comment at `System_BuildConfig.h:685` **falsely** claims CMake
    excludes it on non-S3. Left at 1 it links on esp32 and then OOMs at runtime on
    2 MB PSRAM. Set it 0 by hand **and fix that stale comment**.
  - Web feature level → minimal/off, `ENABLE_G2_GLASSES 0`, `ENABLE_GAMES 0`,
    maps-web off.
  - Reconcile Bluetooth: `boards/qtpy_esp32.defaults` sets `CONFIG_BT_ENABLED=n`
    (and its comment *assumes* `ENABLE_BLUETOOTH=0` in the header — that edit was
    never made). Set `ENABLE_BLUETOOTH 0` / `ENABLE_G2_GLASSES 0` unless the BLE
    detect option is explicitly enabled (see §6.2), in which case the board file’s
    `CONFIG_BT_ENABLED` must flip too.
- **A per-product feature-profile mechanism does not exist yet.** Keeping the
  FeatherS3 full build and the QT Py detector build as selectable profiles is
  **net-new infrastructure** that this project owns. Simplest first cut: a
  documented `HW_PROFILE=deflock` preset (or a `#if defined(ARDUINO_ADAFRUIT_QTPY_ESP32_DEV)`
  override block) that forces the stripped flags. Do this **before** Phase C.
- **The stripped-build fit is unverified.** Flash margin after dropping web
  (`.rodata` ~2.17 MB) + LLM + G2 is plausibly comfortable, but the binding
  constraint is **internal IRAM/DRAM**, which cannot be estimated from the S3
  build. See the Phase-C proof-of-fit gate in [§11](#11-phased-delivery).

---

## 2. Hard constraints (non-negotiable)

### 2.1 Continuous promiscuous while detecting

When the device is in a **detect / RF-active** mode (geofenced region OR user “Always RF”):

- Wi‑Fi stays in **continuous promiscuous** monitor (channel hop) for the **entire session segment**.
- Do **not** stop scanning between poles or on a timer “to save resources.”
- At driving speed, RF dwell near a pole is only seconds; pausing the sniffer to tear down/rebuild the stack will **miss cameras**. That is unacceptable.
- **“Continuous” applies to Wi‑Fi RX.** Any BLE activity (§6.2) time-shares the
  same radio and *reduces* Wi‑Fi RX duty — so BLE is off by default and must be
  a deliberate, time-multiplexed choice, never a free parallel add-on.

### 2.2 No micro start/stop of Wi‑Fi/BT for MAC privacy

Re-initing Wi‑Fi/BT repeatedly to scramble MACs causes **heap fragmentation** on ESP32 (limited internal heap; corroborated in project history). Therefore:

- Randomize the Wi‑Fi address **once per RF session** (on enter detect mode).
- Leave the stack up; do **not** tear down every N minutes to re-scramble.
- Tear down **once** when leaving detect mode (back to silent).
- Note: for a strict passive-RX posture the MAC never reaches the air anyway
  (see §4.1) — the once-per-session randomization is belt-and-suspenders, and the
  heap-frag avoidance is the real reason for the rule.

### 2.3 Reboot is failsafe only — not the sniff scheduler

Do **not** design “reboot every 5 minutes” as the normal way to reset radios or rotate identity.

- Prefer: one init → long continuous sniff → one teardown.
- Reboot only if the radio/heap path is wedged (exception path).
- If a long-trip optional reboot is added: checkpoint track + mode + “resume detect
  continuous” using the **existing** `System_RamFlush` RTC-overlay
  (`ramFlushCaptureOverlay` / `consume` / `resolve`, `RTC_NOINIT`) so boot returns
  to sniffing, not a silent miss. Do not roll a new NVS checkpoint.

### 2.4 Silent mode is the default privacy posture

Most of the time: **GPS + IMU (+ RTC) only**. Radios fully off. No SoftAP, no BLE advertising, no web server, no STA to random networks.

- **Silent must hold at boot, not just at runtime.** The boot sequence brings
  radios up via **five independent** `ramFlushResolve(RF_X, gSettings.xAutoStart)`
  apply-sites (`HardwareOne.cpp:1561/1677/1704/1764/1792/1923` for
  WiFi/BT/HTTP/MQTT/ESP-NOW). Silent mode must force those intents false, not sit
  beside them (see §9).

### 2.5 Geofence gates *sessions*, not sniff bursts

Geofencing decides **when to enter/leave continuous RF mode**, not when to blink the radio on for 30 seconds.

---

## 3. Operating modes

| Mode ID | Radios | Behavior |
|---------|--------|----------|
| `silent` | Off | Default. Log GPS track; IMU assists. No SoftAP/BLE ADV/web/STA. Packed DB may still be queried for UI “nearby known ALPR” without RF. |
| `geofenced` | Off until enter zone → then continuous | GPS + packed ALPR DB. When inside outer/inner policy (see §5), enter RF session once; continuous promiscuous until leave region / end trip / user exit. |
| `always_rf` | Continuous for whole trip | Same sniffer as geofenced RF session, but without needing to enter a map zone first. Still: one init at start, continuous until stop — **not** duty-cycled bursts. |

**Always RF ≠ forever allocate/free.** It means continuous promiscuous for the drive, same stack lifetime rules as geofenced detect.

Modes are **mutually exclusive top-level radio states**, not overlays (see §9): a
tri-state `OFF (silent) / NORMAL / DETECT` radio arbiter, where `geofenced` and
`always_rf` both resolve to `DETECT`.

---

## 4. Privacy / anti-contact (inbound + outbound)

### 4.1 Outbound identity (session MAC)

On **enter RF session** (before first bring-up completes):

- Set Wi‑Fi STA MAC to a **random locally administered unicast** address (`mac[0] &= 0xFE; mac[0] |= 0x02`). *(Bit-math is correct.)*
- **Reality check:** in a strict passive-RX posture (no association, no SoftAP, no
  ADV, no probe **requests**), the device **transmits nothing**, so its MAC never
  reaches the air and the randomization buys ~zero on-air privacy in that state.
  Keep it as belt-and-suspenders. The **real** privacy protection is the no-TX
  posture in §4.2–§4.3.
- Do not expose the factory eFuse MAC on the air during the session.

### 4.2 Inbound / contactability

Even during RF detect:

- **No SoftAP**
- **No BLE connectable advertising**; if BLE detect is enabled, **passive scan only**
  (a passive scan sends no scan-requests → truly zero TX; an *active* scan
  transmits scan-requests carrying the device address, which would violate the
  zero-TX claim).
- **No mDNS / open HTTP** on the car unit while mobile
- ESP‑NOW (when used later): allowlist house peer only; no reply to strangers
- Hostname / BLE name: generic; no owner string, no “HardwareOne” banner in ADV

Silent mode: strongest — radios off.

### 4.3 What this does not claim

- Does **not** hide the vehicle from ALPR cameras (plates / vehicle fingerprint).
- Does **not** make any TX invisible to a sniffer — minimize TX; passive promiscuous RX is preferred.
- Session MAC is stable for one drive segment by design (privacy vs fragmentation tradeoff; residual cross-session linkage only matters if the device ever transmits, which the zero-TX posture avoids).

### 4.4 ESP‑NOW vs MAC randomization (future sync)

House peer bonding is MAC-based, and ESP‑NOW is **mutually exclusive with DETECT**
(fixed channel, see §9). Options when sync is built:

- A dedicated **`DOCKED`** radio state (reserve this in the arbiter enum now) with a stable sync identity only at home, or
- Re-pair / discovery after each car session MAC.

Do not block v1 sniff on this; the arbiter enum should already anticipate `DOCKED` so Phase E doesn’t force a reshape.

### 4.5 Bystander data-minimization (hard rule)

Promiscuous mode receives **every** nearby probe/management frame, including
third parties’ device MACs. Therefore:

- The transient RX ring may hold raw frames (ephemeral, in RAM) — fine.
- **Only matched Flock/Raven hits are ever persisted.** Non-matching frames are
  matched-then-**discarded**; bystander MACs are **never** written to the CSV /
  track store, ESP‑NOW sync, or any log.
- Without this rule the detector becomes a geolocated bystander-tracking dataset —
  the exact surveillance posture the project opposes. This is a correctness
  requirement, not a nicety, and has an acceptance test (§12).

---

## 5. Geofence policy

Use packed ALPR points + GPS fix (IMU only as support, not sole geofence truth when a fix exists). GPS error (tens of metres worst case) is comfortably smaller than the inner ring.

Suggested rings (tunable):

| Ring | Approx radius | Action |
|------|---------------|--------|
| Outer | ~1–2 km | Optional “approaching” state; may pre-enter RF session in dense corridors |
| Inner | ~150–500 m | Ensure RF session active (if `geofenced`) |
| Exit / hysteresis | Leave outer + cooldown | End RF session → silent; don’t thrash on GPS jitter |

**Cooldown (in-session):** While an RF session is active, sniff is continuous regardless of oscillating between inner/outer.

**Cooldown (re-entry):** Prevents re-entering RF for the same cluster every few seconds after exit.

**Dense urban:** Once any ALPR point is within the outer ring, stay in one RF session until the whole cluster is cleared — do not flap per pole.

**Reuse:** `LocationContextManager` (`System_Maps.h:541`) is a distance/time-gated
proximity engine with point-to-segment distance — use it as the ring/hysteresis
core rather than rolling a new one. `haversineDistance` already exists
(`System_Maps.cpp:2675` / `:3916`).

---

## 6. RF detection (path 1)

> Path 1 is the only genuinely greenfield piece: there is **no** promiscuous /
> channel-hop / OUI-IE / probe-parse code anywhere in the tree today (Wi‑Fi RX is
> `WiFi.scanNetworks` AP-scan only). Build it fresh — but copy the proven no-alloc
> RX-ring shape from ESP‑NOW (see §9).

### 6.1 Primary: Wi‑Fi promiscuous

1. Promiscuous 802.11 on **2.4 GHz** (classic ESP32 has no 5 GHz radio — state
   this assumption; any 5 GHz-only emission is invisible to this hardware). Hop
   **1 / 6 / 11**. **Do not** offer “optionally 1–13” as a casual knob — scanning
   all 13 channels triples the revisit interval on the target’s actual channel and
   measurably lowers drive-by capture. If a target channel is ever known, **pin**
   to it instead of hopping.
2. Signature engine (tune sensitivity vs false-positive rate consciously; **log
   which tier fired**):
   - **IE fingerprint is the primary discriminator** when the source MAC is
     locally-administered/randomized (common now) — because in that case the OUI
     is meaningless.
   - **OUI is corroboration, not a hard gate.** A hard OUI gate silently produces
     false negatives against MAC-randomizing targets.
   - **SSID content matters**: accept directed probe requests / beacons whose SSID
     matches Flock patterns (`Flock-*`, `flock`/`flck`, backhaul SSIDs) — SSID
     content is often the *stronger* tell, and a directed probe would fail a naive
     “empty-SSID only” gate.
   - Highest confidence = probe/mgmt frame that matches IE **and** OUI **and**
     SSID; lower tiers still logged with a confidence label.
3. Keep OUI/IE/SSID tables small and updateable (separate file from the location pack).
4. **No heap alloc in the promiscuous callback.** Use a **once-allocated INTERNAL
   DRAM** ring (`heap_caps_calloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`),
   allocated **before** the callback is registered, `volatile` head/tail, bounded
   `memcpy`, drop-on-full counter. **Not PSRAM** (no `SPI_FLASH_AUTO_SUSPEND` on
   classic ESP32 → cache-miss stall/fault in callback context). Copy the exact
   shape of the ESP‑NOW RX ring (`onEspNowDataReceived` + `InboundRxItem` +
   `ESPNOW_RX_RING_SLOTS`, `System_ESPNow.cpp:374-392`).
5. **Management-frame filter is a hard requirement, not “if available”:**
   `esp_wifi_set_promiscuous_filter(WIFI_PROMIS_FILTER_MASK_MGMT)` is always
   available and is the single biggest CPU / ring-pressure reducer. Bound the
   per-frame copy length to the captured subtype (full 802.11 frames are up to
   ~2.3 KB, far larger than the 250 B ESP‑NOW cap — do not copy whole frames into
   the tight internal DRAM pool).

### 6.2 Secondary: BLE (compile-time OFF by default)

Optional scan for older units / Penguin battery / Raven UUIDs / mfg IDs. On a
single-radio classic ESP32 this is a **physics tradeoff**, not a scheduling one:

- Wi‑Fi (2.4 GHz) and BLE **time-share** the radio via the coex scheduler. A
  concurrent BLE scan **inherently duty-cycles Wi‑Fi RX** — you cannot have §2.1’s
  “continuous, never duty-cycle” Wi‑Fi *and* a concurrent BLE scan.
- Enabling BLE also reintroduces the ~14 KB IRAM / ~80 KB DRAM / ~70 KB flash the
  QT Py board default reclaims by disabling BT — against a pool that already
  overflowed with BT + G2. It may force stripping other subsystems just to link.
- Therefore BLE detect is a **compile-time switch defaulting OFF**. If enabled:
  **passive** scan only (§4.2), deliberately **time-multiplexed** against the
  Wi‑Fi hop, and its IRAM/DRAM cost must be budgeted in the Phase-C proof-of-fit.
- Coex on this HW is documented-fragile (`ESP_ERR_ESPNOW_INTERNAL` “BLE coex
  glitch” handling at `System_ESPNow.cpp:9105`; `WIFI_PS_NONE` deadlock workaround
  at `System_WiFi.cpp:873`). Validate on real LX6 hardware.

### 6.3 Detection probability (be honest)

The Rev 1 claim “the main miss mode is wrong channel, not radio-down” was wrong.
On a fast pass the dominant factor is **whether the target transmits at all during
the ~5–7 s in-range window**, which the research doc implies is sporadic (units
sleep and wake to upload). Realistically:

```
P(detect on one pass) ≈ P(target TX during window) × channel-duty(~1/3 for 1/6/11) × in-range-fraction
```

- **State plainly that single-pass capture of a sleeping/rarely-probing unit is
  probabilistic and can be low.** Lean on **repeated passes + crowd/fleet
  aggregation** (DeFlock is inherently crowdsourced), not on an implied guarantee.
- An empty hit log after one drive does **not** mean “no cameras” — the UI must not
  imply certainty.
- If any Flock unit operates as an **AP** (beacons ~every 100 ms, SSID directly
  readable), beacon capture is *far* more catchable on a drive-by and should be
  **co-primary** with the probe path. This is unknown until measured — see the
  Phase-C bench-characterization gate (§11).

### 6.4 Logging a hit

On a **matched** detection, log: timestamp (RTC, **dated** — see §8), GPS
lat/lon/speed if fix, RSSI, matched MAC, method/tier (`wifi_probe_ie_oui_ssid`,
`wifi_ie_only`, `ble_raven_uuid`, …), channel, confidence. Persist to LittleFS/SD
CSV via the existing `System_SensorLogging` writer (rotation + LittleFS→SD overflow
via `VFS::resolveOverflowPath`) and/or model a dedicated logger task on
`captureEspNowFrame` (`System_ESPNow.cpp:5485`) fronted by a queue. **Only matched
hits are written (§4.5).** Optional later: sync to house via ESP‑NOW.

---

## 7. Map / packed DB (path 2)

### 7.1 Format

Packed binary, **not** full GeoJSON:

- ~8–16 bytes/record: `lat`, `lon` (int32 microdegrees), optional `vendor`/`flags` uint8.
- US pack ~100k points ≈ **~1.2 MB**.
- **Reuse the existing encoding:** the map-tile loader already uses on-demand
  `int32`-microdegree packed-binary reads from an open `File`
  (`System_Maps.h:203/396/478`) — the exact precedent for this pack format.

### 7.2 Runtime & storage (reconciled for the 8 MB QT Py)

- **Do not full-load a 1–2 MB pack into the 2 MB PSRAM** — it is shared with
  Wi‑Fi/lwip buffers (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`) plus the ALPR pack
  plus downstream buffers. Prefer **memory-mapped / on-demand file scan** (map-tile
  loader pattern) with **sort-by-lat + binary search** (or geohash buckets), then
  haversine on the shortlist.
- **Storage sizing is board-specific and was previously computed for the 16 MB
  FeatherS3.** On the 8 MB QT Py the no-SR LittleFS is **2796 KB**
  (`partitions_no_sr_8mb.csv` littlefs `0x2BB000`); a 1–2 MB US pack consumes
  40–70 % of it and crowds logs/system files. **Commit to one:**
  - no-SR partition + **regional/state packs** that live within 2796 KB, **or**
  - **mandate an SD card** — but the QT Py Stemma SD reader/pin wiring is **not
    confirmed in-tree** and needs bring-up first.
- Versioned file; update over Wi‑Fi only when explicitly in a NORMAL (non-detect)
  state — never mid-detect.

### 7.3 Hybrid confidence

| Observation | Meaning |
|-------------|---------|
| Map only | Known mapped ALPR, quiet / no RF |
| RF only | Live signature, possibly unmapped |
| Both near same point | High confidence |

---

## 8. Track logging (non-RF)

Independent of sniff. **Good news:** GPS (PA1010D, `gGpsCache`), RTC (DS3231,
`rtcReadDateTime`), and two existing track loggers (`GPSTrackManager` in-RAM +
`System_SensorLogging` file) already run on radio-independent I2C poll tasks — so
they work in `silent` with radios off today. **Net-new work** is the fusion +
retention story, not the sensors:

- **IMU is not a config flip.** The BNO055 driver is complete but compiled out
  (`ENABLE_IMU_SENSOR 0`, “not installed”) and has **only ever run on the S3** —
  and board-gated code has hidden compile breaks here before. Treat “install
  hardware + `CUSTOM_ENABLE_IMU 1` + compile **and HW-test on the QT Py LX6**” as a
  **hard Phase-A gate**, not an assumption. Note the BNO055’s ~100 Hz fused output
  may limit high-rate dead-reckoning during GPS dropout.
- **Dated record format is net-new.** Existing file formats carry time-of-day
  (`HH:MM:SS`) or `millis()` with **no date** → multi-day / across-midnight tracks
  are ambiguous. Define a new **RTC-dated** GPS+IMU+RTC record; use **RTC as the
  authoritative timestamp** (it already survives radios-off and reboots).
- **Multi-day retention is net-new.** The in-RAM `GPSTrackManager` caps at 10 000
  points and dies on reboot; define file rotation/retention for a multi-day window.
- **Parked breadcrumb:** `GPSTrackManager::appendPoint` skips points <2 m from the
  previous, so a stationary device logs nothing. If “breadcrumb while parked” is
  wanted, change the dedup rule (e.g. time-based minimum) for the track logger.
- Must survive the optional failsafe reboot via the `System_RamFlush` checkpoint (§2.3).

---

## 9. Architecture — the radio-arbiter retrofit

> **Correction to Rev 1:** there is **no single owner** of Wi‑Fi start/stop/mode
> today. Radio control is scattered across `System_WiFi.cpp` (STA connect/scan,
> including a raw `esp_wifi_deinit` “nuclear” path at `:824`), `System_ESPNow.cpp`
> (flips to `WIFI_AP_STA` at `:9062`, pins a fixed channel with no resync at
> `:9077-9087`), and both first-time-setup paths. `System_RfMode` must therefore be
> a **retrofit that becomes the single owner**, not a new module that races the
> existing ones.

```
┌─────────────────────────────────────────────────────────┐
│  Radio arbiter (System_RfMode): OFF | NORMAL | DETECT    │
│  (+ reserve DOCKED for Phase E)  — SINGLE Wi-Fi owner     │
└───────────────┬─────────────────────────────┬───────────┘
                │                             │
    ┌───────────▼───────────┐     ┌───────────▼───────────┐
    │ Position: GPS+IMU+RTC │     │ ALPR pack proximity   │
    │ Track logger (radio-  │     │ Geofence state        │
    │ independent, always)  │     │ (LocationContextMgr)  │
    └───────────┬───────────┘     └───────────┬───────────┘
                │                             │
                │         enter/leave         │
                │      DETECT session (once)  │
                └──────────────┬──────────────┘
                               ▼
                ┌──────────────────────────────┐
                │ DETECT session lifecycle     │
                │ 0. Evict ESP-NOW + STA;      │
                │    stop HTTP/MQTT (closewifi)│
                │ 1. Randomize MAC once        │
                │ 2. WiFi promiscuous (INTERNAL│
                │    ring) continuous, hop     │
                │ 3. (opt) passive BLE window  │
                │ 4. Hit logger (queue+task)   │
                │ 5. Single teardown; restore  │
                │    STA mode explicitly       │
                └──────────────────────────────┘
```

**The arbiter must own / neutralize all of these — none are optional:**

1. **Boot autostart (silent-at-boot).** Force `RF_WIFI/RF_BLUETOOTH/RF_HTTP/
   RF_MQTT/RF_ESPNOW` intents false at the 5 apply-sites
   (`HardwareOne.cpp:1561-1923`) when the mode is `silent`. Slot the master switch
   **into** the `ramFlushResolve` intent layer — do not bypass it — so silent is
   coherent at boot and composes with reboot-resume.
2. **The nuclear deinit hazard.** `connectWiFiIndex` / `connectToBestWiFiNetwork` /
   `ensureWiFiInitialized` and the `wifiAutoReconnect` watchdog must be **no-op (or
   routed through the arbiter)** while `mode == DETECT`, or a stray STA connect’s
   `esp_wifi_deinit` (`System_WiFi.cpp:824`) will silently destroy the registered
   promiscuous callback and channel — an intermittent “sniffer went deaf” bug.
3. **Bypass surfaces.** Gate/hide the runtime radio toggles that otherwise defeat
   “single owner”: OLED **Quick Settings** (Wi‑Fi/BT/HTTP toggles,
   `OLED_Display.h:129`), the OLED **Wi‑Fi menu** (`OLED_NETWORK_WIFI_*`), and the
   CLI `wifi` / `openhttp` / `closewifi` / `espnowenable` commands. There is an
   acceptance test for this (§12).
4. **ESP‑NOW exclusivity.** DETECT must fully `deinitEspNow` (`System_ESPNow.cpp:9453`)
   — accept that ~855 KB won’t fully free until reboot (`:9324-9326`), and note
   `deinitEspNow` does **not** restore `WIFI_STA` mode, so the arbiter must set mode
   explicitly on DETECT exit.
5. **HTTP/MQTT consumers.** They gate on `WiFi.isConnected` (`System_WiFi.cpp:1026`,
   `System_MQTT.cpp:228`). On NORMAL→DETECT, invoke the existing `closewifi`
   teardown (`System_WiFi.cpp:385-395`, `httpd_stop` + drop `MSG_ROUTE_WEB`) rather
   than leaving them polling a dead interface.
6. **ISR safety.** Wrap **every** `esp_wifi_set_mode` / channel change in the
   existing `I2CDeviceManager` `pausePolling()/resumePolling()` + `vTaskDelay`
   bracket (`System_ESPNow.cpp:9057`) or it trips the interrupt WDT.
7. **Signalling.** Emit existing `SYSEVT_*` events on every transition
   (`SYSEVT_WIFI_CONNECTED/DISCONNECTED`, `SYSEVT_ESPNOW_ON/OFF`,
   `SYSEVT_HTTP_SERVER_STOPPED`) so OLED status, notifications, and
   event-triggered automations observe DETECT enter/exit for free.

### Module split

| Module | Responsibility | Reuse / notes |
|--------|----------------|---------------|
| `System_RfMode` | Radio arbiter: `OFF/NORMAL/DETECT(/DOCKED)`, single owner of Wi‑Fi start/stop/mode, enter/leave DETECT | Generalize the read-only tri-state in `G2_Page_ESPNow.cpp:92-107` into the owning enum |
| `System_AlprPack` | Load/query packed lat/lon DB | Map-tile loader int32-microdegree file-scan precedent |
| `System_AlprGeofence` | Rings, hysteresis, cluster session state | `LocationContextManager` + `haversineDistance` |
| `System_FlockSniff` | Promiscuous hop, INTERNAL RX ring, IE/OUI/SSID match, hit events — **long-lived while DETECT** | Copy ESP‑NOW RX-ring shape; `xTaskCreateLogged`; pin decode to core 1 |
| `System_RfPrivacy` | Session MAC randomize, ADV/SoftAP/passive-scan policy, bystander-discard | — |
| Track logger | Dated GPS+IMU+RTC record, rotation, RamFlush checkpoint | `GPSTrackManager` + `System_SensorLogging` |

**Task discipline** (`System_TaskUtils.h` conventions): create tasks via
`xTaskCreateLogged`; **stack sizes are BYTE counts** despite the `*_STACK_WORDS`
names — add `SNIFFER_STACK_WORDS` / `TRACKLOG_STACK_WORDS` as byte counts, no ×4.
Pin decode-heavy sniffer work to **core 1** (compute) so it doesn’t starve Wi‑Fi
RX on core 0; give the file-writing track/hit logger ≥4096 B and decouple it from
the sniffer via a queue (a single LittleFS append has overflowed a 3 KB stack
before). Register both in `reportAllTaskStacks`; guard loops with
`checkTaskStackSafety`.

---

## 10. Explicit non-goals / anti-patterns

Implementers must **not**:

1. Duty-cycle promiscuous Wi‑Fi sniff in 30–90 s windows as the normal detect design.
2. Tear down Wi‑Fi/BT every few minutes to rotate MAC.
3. Use reboot-every-N-minutes as the identity or heap “solution” in the happy path.
4. Run SoftAP / phone BLE peripheral / open web UI during car detect.
5. Ship full OSM GeoJSON as the on-device DB.
6. Jam, deauth, or otherwise TX-attack cameras — **passive detect only**.
7. Block v1 on house ESP‑NOW sync polish.
8. **Persist bystander MACs / non-matching frames** to any log (§4.5).
9. **Put the promiscuous callback ring in PSRAM** — INTERNAL DRAM only (§6.1.4).
10. **Run an active BLE scan** during detect — passive only, and off by default (§6.2).
11. Leave `ENABLE_ONDEVICE_LLM 1` on the esp32 build (§1a).
12. Add `System_RfMode` “beside” the existing radio code as a 5th uncoordinated owner (§9).

---

## 11. Phased delivery

> **Two proof gates were added.** They are cheap and de-risk everything downstream.

### Gate 0 — Build proof-of-fit (before Phase A commits)

- Produce a real `idf.py set-target esp32 && HW_BOARD=qtpy_esp32 idf.py build`
  with the stripped profile (`ENABLE_ONDEVICE_LLM 0`, web minimal/off,
  `ENABLE_G2_GLASSES 0`, games off, maps-web off, BT reconciled).
- Run `idf_size` and confirm **both** factory-partition fit **and** `iram0_0_seg`
  headroom. Flash is estimable; **internal SRAM is the binding constraint** and
  cannot be inferred from the S3 build.
- Fix the stale LLM comment (`System_BuildConfig.h:685`) so the next reconfigurer
  isn’t trapped.

### Phase A — Silent car logger

- Stripped QT Py build (Gate 0). Mode stub: `silent` only.
- **Install IMU + flip `CUSTOM_ENABLE_IMU 1` + compile AND HW-test on QT Py** (hard gate).
- Dated GPS+IMU+RTC track record (RTC authoritative), rotation/retention, parked-breadcrumb dedup, RamFlush reboot-survival. Reuse `GPSTrackManager` + `System_SensorLogging`.

### Phase B — Packed DB + geofence state machine

- Ship/load a **regional** pack (or SD-mounted US pack) within the 2796 KB LittleFS reality (§7.2), using the map-tile file-scan pattern.
- Geofence enter/leave events via `LocationContextManager` (log + CLI/OLED). Still no sniff (or sniff stub).

### Phase C — RF session + continuous Flock sniff

- **Gate C0 — bench characterization:** capture a real Flock/Raven emission
  (frame type: probe vs beacon; cadence; band; whether it MAC-randomizes) with a
  bench sniffer. If any unit beacons as an AP, make beacon capture co-primary.
- Build `System_RfMode` as the **radio-arbiter retrofit** (§9): single owner,
  evict ESP‑NOW + STA on DETECT enter, block the nuclear deinit path, neuter bypass
  surfaces, ISR-safe mode changes, SYSEVT signalling.
- `System_FlockSniff`: INTERNAL DRAM ring, MGMT filter (hard), IE-primary /
  OUI-corroboration / SSID-content signature engine, scramble-once, continuous
  promiscuous, single teardown. Hit log with dated GPS; **only matched hits persisted**.
- Modes: `geofenced`, `always_rf`.
- Validate 3-radio coex + continuous promiscuous RX **on real LX6 hardware** (S3 results do not transfer).

### Phase D — Privacy hardening + hybrid UI

- SoftAP/ADV-off guarantees; bystander-discard verified; passive-BLE (if compiled in).
- Map+RF confidence merge on OLED/web/CLI.
- Heap/DRAM discipline audit (no alloc in RX path; INTERNAL ring; PSRAM only for downstream).
- Expose the mode as a runtime toggle: **enum setting + a real per-setting command
  `cmd_rfmode`** (settings save via real per-setting commands only — no
  auto-register) + an enum pick-list entry in `OLED_SettingsEditor`, and a DeFlock
  OLED screen via the `REGISTER_OLED_MODE_MODULE` X-macro. Admin-gate mutation.

### Phase E — House sync (separate track)

- ESP‑NOW **`DOCKED`** state sync of tracks/hits (arbiter already reserves it);
  resolve MAC session vs peer identity. Never runs concurrently with DETECT.

---

## 12. Acceptance criteria

1. Gate 0: a real esp32/QT Py stripped build **links and fits** — factory partition and `iram0_0_seg` both have headroom per `idf_size`.
2. In `silent` (including **at boot**): Wi‑Fi/SoftAP off, BLE not advertising, HTTP/MQTT/ESP‑NOW not started; track still logs with **dated** RTC timestamps.
3. Entering `always_rf` or a geofence session: MAC randomized once; promiscuous runs **without** planned teardown for ≥ one multi-minute drive segment.
4. Leaving detect: single teardown; STA mode explicitly restored; device returns to silent.
5. Highway pass: sniffer was not “off between windows” by design — only channel-hop gaps. (Detection remains probabilistic — an empty log is not asserted as “no cameras”.)
6. **Bypass test:** toggling Wi‑Fi in OLED Quick Settings (or `wifi`/`openhttp` CLI) during silent/detect is **refused** and does not disturb a live session.
7. **Bystander test:** a capture run near many non-Flock devices writes **zero** non-matching MACs to any persisted log.
8. Hit log includes dated time + GPS (when fix) + detection method/tier + channel; **only matched hits** present.
9. Packed DB query works offline (no live Overpass) within the chosen storage strategy.
10. The promiscuous callback ring is **INTERNAL** (`MALLOC_CAP_INTERNAL`); no alloc occurs in the callback; MGMT filter is set.
11. No jamming/deauth/TX-attack code paths; if BLE detect is compiled in, it is **passive** scan only.

---

## 13. Key external references

- Research: `docs/DEFLOCK_RESEARCH_FEASIBILITY.md` (note: its DB-fit math is the 16 MB FeatherS3 — re-derive for the 8 MB QT Py, §7.2).
- Detection methods: flock-you / WatchFlock / flockdar community (probe/beacon + IE + OUI). Characterize empirically before Phase C (Gate C0).
- Map data: DeFlock / OSM `surveillance:type=ALPR` → **stripped pack only**.
- Board: `docs/BOARD_SWITCHING.md`; board file `boards/qtpy_esp32.defaults`; pin block `System_BuildConfig.h:727`; partitions `partitions_no_sr_8mb.csv`.
- Existing GPS/RTC/IMU: `gGpsCache`/PA1010D, DS3231 (`rtcReadDateTime`), BNO055 (`i2csensor_bno055.h`, compiled out).
- Existing Wi‑Fi: `WiFi.scanNetworks` AP scan only — **new** promiscuous path required. Radio control scattered across `System_WiFi.cpp` / `System_ESPNow.cpp` — arbiter retrofit required (§9).
- Reuse anchors: ESP‑NOW RX ring `System_ESPNow.cpp:374-392`; `captureEspNowFrame:5485`; `GPSTrackManager` / `LocationContextManager` / map-tile loader in `System_Maps.*`; `haversineDistance`; `System_RamFlush`; `System_SensorLogging`; `xTaskCreateLogged`; `SYSEVT_*`; `REGISTER_OLED_MODE_MODULE`.

---

## 14. One-paragraph brief for the implementing AI

Ship a **stripped, compile-time QT Py ESP32 detector build profile** (LLM/web/G2
off, BT reconciled — the full firmware does not fit the 8 MB board) whose default
runtime mode is radio-silent GPS+IMU+RTC **dated** track logging; when `geofenced`
or `always_rf`, a **radio-arbiter retrofit** (`System_RfMode`, the single Wi‑Fi
owner — it evicts ESP‑NOW/STA, stops HTTP/MQTT, blocks the nuclear `esp_wifi_deinit`
path, and neutralizes the OLED/CLI bypass toggles) brings Wi‑Fi up **once** with a
randomized locally-administered MAC and runs **continuous** passive promiscuous
Flock/ALPR detection (2.4 GHz hop 1/6/11, MGMT filter, **INTERNAL-DRAM** no-alloc
ring, IE-primary/OUI-corroboration/SSID-content matching) until the mode exits —
never duty-cycling for privacy or heap, never rebooting on a timer as the scheduler,
BLE off by default (and passive if used, because it time-shares the one radio);
detection is **probabilistic per pass** so lean on repeated/crowd aggregation; use a
regional (or SD-hosted) packed int32-microdegree ALPR DB scanned from file (not
full-loaded into the shared 2 MB PSRAM) for geofence entry/exit and hybrid
confidence; **persist only matched hits, never bystander MACs**; gate Phase C on a
measured `set-target esp32` build fit + a bench characterization of the real Flock
emission; leave house ESP‑NOW sync to a reserved `DOCKED` state in Phase E.
