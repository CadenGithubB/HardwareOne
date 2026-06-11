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

---

## Device behaviour model — the mental model for the app

So the app author isn't guessing about what's on the other end:

- **It's a CLI over BLE.** The device exposes its whole command line over the Command
  service: the app writes a command string to REQUEST, the device runs it on a worker
  task, and notifies the text result on RESPONSE. Same commands as the serial console and
  web CLI — there is no bespoke binary protocol.
- **Replies are asynchronous and notification-based.** A command does not "return" inline;
  the result arrives later as a RESPONSE notification (practically ~one command → one
  reply). Be event-driven, never block waiting on a write to "return" data.
- **Two mutually-exclusive BLE roles.** `bleMode` is either **Server** (phone peripheral —
  what the app talks to) or **G2 client** (drives smart glasses). They share one radio and
  never run at once. The app only ever sees Server mode; if the device is switched to G2
  it stops advertising the command service entirely.
- **Authorization = `login`, always.** Connecting grants nothing. The device requires
  `login <user> <pass>` per connection; until then only `login`/`logout`/`whoami` work and
  anything else returns `Authentication required`. True in both encrypted and plaintext
  modes.
- **Confidentiality = optional APP-LAYER encryption** (the "Secure Channel", `blesecure`,
  server mode only, **off by default**). It is **NOT** BLE link-layer bonding — there is no
  OS pairing dialog, no passkey, no bond, and no `removeBond` GrapheneOS trap. All crypto
  rides inside the existing REQUEST/RESPONSE chars (X25519 + a pre-shared passphrase +
  ChaCha20-Poly1305). This is the ESP-NOW security model. `login` still runs *on top* —
  encryption = confidentiality, login = authorization.
- **Shared secret, provisioned out-of-band.** The operator sets a passphrase on the device
  (`blesecret <phrase>`); the user enters the **same** passphrase in the app once (store it
  in Keystore). Both sides derive the same PSK via PBKDF2. No per-connection pairing step.
- **When required (`blesecure on`)** the device refuses plaintext commands and only executes
  DATA frames sent after a successful handshake. When off (default) plaintext works as today
  and a client MAY still negotiate a Secure Channel opportunistically.

## Encryption: Secure Channel v1 (build the app to match byte-for-byte)

App-layer only — no bonding, no pairing dialog. All frames ride the existing REQUEST (write)
/ RESPONSE (notify) chars. All integers big-endian. Each GATT message = `type(1) || body`:

```
0x01 HELLO        body = appEphPub(32) || appNonce(16)       (app → device)
0x02 HELLO_ACK    body = devEphPub(32) || devNonce(16)       (device → app)
0x03 CONFIRM      body = ct(2) || tag(16)   = AEAD("ok", K_c2d, ctr 0)   (app → device)
0x04 CONFIRM_ACK  body = ct(2) || tag(16)   = AEAD("ok", K_d2c, ctr 0)   (device → app)
0x10 DATA         body = ctr(8) || ct(N) || tag(16)          (either direction)
```

Crypto:
```
ss  = X25519(ownEphPriv, peerEphPub)
PSK = PBKDF2-HMAC-SHA256(passphrase, salt="HW1-SC-v1-psk", iters=100000, 32 bytes)
K   = HKDF-SHA256(ikm = ss||PSK, salt = appNonce||devNonce, info="HW1-SC-v1", 64 bytes)
      K_c2d = K[0:32] (app→device)    K_d2c = K[32:64] (device→app)
AEAD = ChaCha20-Poly1305-IETF (12-byte nonce, 16-byte tag, no AAD)
nonce(12) = dirTag(4) || ctr(8);  dirTag = 0x00000000 (c2d) / 0x00000001 (d2c)
per-direction ctr strictly increasing (CONFIRM/CONFIRM_ACK = 0; DATA = 1,2,3,…) → replay-safe
```
A decrypt failure = wrong passphrase / MITM → abort the channel.

Handshake (once per connection, before login):
1. App → HELLO       (app makes an ephemeral X25519 keypair + 16-byte nonce)
2. Dev → HELLO_ACK   (device does the same; both derive `ss` then `K`)
3. App → CONFIRM     (proves the app holds the right passphrase, via K_c2d)
4. Dev → CONFIRM_ACK (proves the device side) → channel **ESTABLISHED**

Then every command/reply is a DATA frame; plaintext is the same UTF-8 CLI line as today.
A reply longer than one frame arrives as **multiple DATA frames** — decrypt each and
concatenate (device chunks at ~200 plaintext bytes/frame, so negotiate a large MTU).
`login <user> <pass>` runs INSIDE the channel after ESTABLISHED.

Android primitives (all available on GrapheneOS): X25519 (`java.security` XDH API 33+, or
lazysodium/BouncyCastle), HKDF-SHA256 (HMAC-SHA256), `PBKDF2WithHmacSHA256`, and
`Cipher.getInstance("ChaCha20-Poly1305")` (API 28+).

## How the phone (app) is expected to behave — runtime contract

1. Scan by **service UUID** (not just name); `connectGatt(autoConnect=false)`; request HIGH
   connection priority.
2. `discoverServices()` → `requestMtu(517)` → wait for `onMtuChanged` (treat the granted
   value as your real per-notification size; never assume 514 — iOS caps at 185).
3. Enable RESPONSE notifications: `setCharacteristicNotification(true)` **and** write
   `0x0001` to its CCCD (`00002902-…`). Skip the CCCD write and you receive nothing.
4. If using encryption: run the Secure Channel handshake (HELLO → … → CONFIRM_ACK), then
   send/receive everything as DATA frames. If the device requires the channel and you send
   plaintext, it replies "Secure channel required". If the handshake won't decrypt, the
   passphrase is wrong → prompt the user to re-enter it.
5. `login <user> <pass>` (inside the channel when encrypted); gate the command UI on the reply.
6. Command/response loop: write a line, render RESPONSE notifications as they arrive
   (multi-line; reassemble multi-frame DATA replies). Stay event-driven.
7. On disconnect: reconnect and **re-run the handshake** — channel state is per-connection
   and keys are wiped on disconnect; there's no persisted bond to restore. The stored
   passphrase keeps this seamless for the user.
8. **No OS pairing, ever:** no `createBond`, no `ACTION_PAIRING_REQUEST`, no passkey dialog.
   If you find yourself reaching for those, you're on the old (wrong) design.
