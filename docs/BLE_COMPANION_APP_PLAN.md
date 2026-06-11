# BLE Companion App — Interface Contract & Firmware Plan

**Goal:** a minimal, Google-free Android/AOSP/GrapheneOS app that pairs with and
messages a HardwareOne device over BLE. The device side is the easy half — it already
advertises a GATT command interface with **no Google dependencies and no BLE
bonding/pairing/encryption** (auth is app-layer via a `login` command). This doc is the
verified source-of-truth for that interface plus the firmware-side follow-ups.

All facts below are verified against `components/hardwareone/Bluetooth.cpp` /
`Bluetooth.h` (the **active** backend). `BLE_IDF.cpp` is an experimental IDF-native GATTS
reimplementation gated behind `ENABLE_BLE_IDF_EXPERIMENTAL` (OFF) — ignore it.

## Host-stack decision (firmware)
- The build uses **Bluedroid** (`CONFIG_BT_BLUEDROID_ENABLED=y`; NimBLE off) via the
  vendored Arduino `BLE` library (`BLEDevice`, `esp_ble_gatts_*`).
- Targets used interchangeably: **ESP32-S3 (current), original ESP32, and eventually
  ESP32-C6.** Decision: **stay Bluedroid on all of them.** C6 supports Bluedroid in
  IDF 5.5, so `Bluetooth.cpp` ports unchanged. Maintaining a parallel NimBLE port is
  explicitly **out of scope** (not feasible to maintain both).
- TODO when adding C6: create `sdkconfig.defaults.esp32c6` (today only `.esp32` and
  `.esp32s3` exist).

---

## GATT contract

**Advertising:** device name = `gSettings.bleDeviceName` (default "HardwareOne",
user-configurable — match by service UUID, not just name). Advertises the Command +
Data service UUIDs, scan response on. Up to **4** concurrent connections
(`BLE_MAX_CONNECTIONS`). No pairing/bonding/encryption.

### Command Service — `12345678-1234-5678-1234-56789abcdef0`
| Char | UUID | Props | Use |
|------|------|-------|-----|
| REQUEST  | `…de01` | WRITE + WRITE_NR | client writes a command line (UTF-8/ASCII text). Max input ~512 B. |
| RESPONSE | `…de02` | NOTIFY (has CCCD/0x2902) | client subscribes; command output arrives as text. **Single notification per reply** (see chunking below). |
| STATUS   | `…de03` | READ | JSON `{"state":"connected","uptime":<s>,"rx":<n>,"tx":<n>}` |

### Data Service — `12345678-1234-5678-1234-56789abcdef1` (optional for v1)
- `…de11` SENSOR_DATA (NOTIFY), `…de12` SYSTEM_STATUS (NOTIFY),
  `…de13` EVENT_NOTIFY (NOTIFY), `…de14` STREAM_CONTROL (WRITE).

### Device Information Service — `180A` (standard)
- `2A29` Manufacturer = "HardwareOne"
- `2A24` Model = "ESP32-S3 Hub"
- `2A26` Firmware = `SelfDevice::firmwareVersion()` (real build version from the app
  descriptor — was a hardcoded "2.1.0", fixed this session).

## Connection sequence (the firmware enforces this order)
1. `connectGatt(autoConnect=false)`; then `discoverServices()`.
2. `requestMtu(517)` — the firmware does **not** initiate MTU negotiation. (See MTU below.)
3. Enable notifications on RESPONSE: `setCharacteristicNotification(true)` **and** write
   `0x0001` to its CCCD (`00002902-…`). Skip this and you receive nothing.
4. **Auth** (`bluetoothRequireAuth` defaults **true**, per-connection session):
   - Write `login <user> <pass>` to REQUEST. Success → notify
     `"[ble] Login successful. User: <name> (admin)"`.
   - Any non-session command before login → notify
     `"Authentication required. Use: login <username> <password>"`.
   - Also available: `logout`, `whoami`.
5. After login: write any CLI command; output returns on RESPONSE notifications.
   Responses are **asynchronous** (commands run on a worker task), so be event-driven on
   the notification. Practically ~one command → one reply notification.

Notes: replies are plain text, may contain `\n`. Multi-connection edge case: with >1
client connected, replies are broadcast to all, prefixed `"[ble conn:<id>] "`; single
connection (normal) = no prefix.

## Android / GrapheneOS specifics
- Permissions (API 31+): `BLUETOOTH_SCAN` (with `usesPermissionFlags="neverForLocation"`),
  `BLUETOOTH_CONNECT`; request at runtime, handle denial. No location needed for scan.
- No Play Services / Firebase / Nearby / Fast Pair. Only `android.bluetooth.*`.
- No INTERNET permission required. Distribute via APK / Obtainium / F-Droid.

---

## MTU & the chunking story (firmware)

**Default BLE MTU is 23 → 20 usable bytes** per notification. The phone-server path
historically never raised it, so replies were capped at ~20 chars (every long reply
truncated). Negotiated MTU = `min(client request, device offer)`; clients vary
(Android ~247–517, **iOS hard-caps at 185**).

**DONE this session:** `initBluetooth()` now calls `BLEDevice::setMTU(517)` (the BLE
spec max; payload = MTU−3 = **514 B**). This is a global Bluedroid setting; the G2 client
path sets its own (244). RAM cost is small (~`4 × MTU` for buffers) and PSRAM-backed on
S3/ESP32 via `BT_ALLOCATION_FROM_SPIRAM_FIRST` (C6 has no PSRAM but the cost is
negligible vs 512 KB SRAM).

**Two output paths to BLE** (both end in `sendBLEResponse()` → `setValue()` + `notify()`):
- **A — broadcast/log drain** (`System_Debug.cpp`): each line ≤ `DEBUG_MSG_SIZE−1` = 255 B;
  aggregated into `gBLEOutputBuffer` (`BLE_OUTPUT_BUFFER_MAX = 512`) and flushed as one
  notification when near-full or every `BLE_OUTPUT_FLUSH_INTERVAL_MS = 500`.
- **B — command result** (`bleCommandResultCallback`): sends the whole result string in
  one notification.

**Why an aggregation buffer at all (vs the per-message Serial/file/OLED sinks):** BLE
notifications are rate-limited (one connection-interval, a few TX buffers); notifying
faster than the radio drains makes Bluedroid drop notifications silently. The other
sinks can't be "congested," so only BLE needs pacing. The *concept* is justified; the
current *implementation* is weak (500 ms latency; silently drops a line that won't fit —
`System_Debug.cpp:293`; buffer size was mis-coupled to an MTU that was never raised).

### Current behaviour after the MTU bump (what "ships now")
- Short/medium replies (`login`, `status`, toggles, most commands) → arrive intact.
- Long replies (full `help`, big reports) → **still truncated**: a flush > negotiated
  payload, or a single result string > payload, is cut by Bluedroid. No reassembly.

### Deferred firmware work — pick one (NOT yet done)
- **(A) ship MTU-only** ← chosen for now; unblocks the app for short/medium output.
- **(B) repair the buffer:** cap each notification at the *per-connection negotiated*
  MTU−3 (read it from `ESP_GATTS_MTU_EVT`), flush on idle / command-complete instead of
  a blind 500 ms, never silently drop, and converge paths A and B onto one. Makes the
  value choice and client/chip MTU variance irrelevant. **Recommended next.**
- **(C) per-message + congestion handling:** drop the buffer, emit one notification per
  line, watch the GATT congest event and pace. Most consistent with the other sinks,
  most BLE-specific code to get right.

App-side implication: until B/C lands, the app should expect possible truncation of
very long replies and not assume a fixed envelope — it gets `negotiated_MTU − 3` bytes
per notification.
