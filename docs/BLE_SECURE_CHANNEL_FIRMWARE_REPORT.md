# HardwareOne BLE — Firmware Interop Report (Secure Channel v1)

Authoritative firmware-side description of the BLE command interface and the app-layer
**Secure Channel v1**. Reflects the actual implementation in `System_BleSecureChannel.cpp`
+ `Bluetooth.cpp`. The crypto has been verified **byte-for-byte against the test vectors
in §11** using libsodium (the same primitives the firmware calls). Hand this to the
Android app side to cross-check interop.

Status: implemented, builds green, **uncommitted on branch `ble-encryption`**, not yet
HW-interop-tested (no app to test against yet).

---

## 1. Model
The device is a **BLE GATT server** exposing a line-oriented CLI: the app writes a command,
the device runs it and notifies the text result. Security is **app-layer** (ESP-NOW–style:
X25519 + pre-shared passphrase + ChaCha20-Poly1305), riding the existing GATT
characteristics as opaque binary. **No BLE pairing, bonding, or link-layer encryption** —
the device never requests link security on any characteristic. (This deliberately avoids the
GrapheneOS `removeBond` orphan-bond trap.)

## 2. GATT interface
- **Advertises** as `gSettings.bleDeviceName` (default `"HardwareOne"`); advertises the
  Command + Data service UUIDs.
- **Command service** `12345678-1234-5678-1234-56789abcdef0`
  - `…de01` **REQUEST** — WRITE / WRITE_NR — app → device (commands or secure frames)
  - `…de02` **RESPONSE** — NOTIFY (+ CCCD `2902`) — device → app (replies or secure frames)
  - `…de03` **STATUS** — READ — `{"state":"connected","uptime":..,"rx":..,"tx":..}` —
    **always plaintext**, not gated
- **Device Information** `180A`: Manufacturer `"HardwareOne"`, Model `"ESP32-S3 Hub"`,
  Firmware = real build version. **Always plaintext.**
- **MTU:** device offers **517** (`BLEDevice::setMTU(517)`); it does NOT initiate
  negotiation — the app must `requestMtu`. Up to **4** concurrent connections.
- **Connect sequence the firmware expects:** connect (`autoConnect=false`, `TRANSPORT_LE`)
  → discover → requestMtu → enable RESPONSE notifications (CCCD write) → then HELLO (secure)
  or a plaintext command.

## 3. Operator configuration (device side)
- `blesecret <passphrase>` — set the PSK passphrase (≥10 chars; needs upper+lower+digit+symbol).
  Refused over Bluetooth — set via serial/OLED/web. `blesecret clear` removes it; bare `blesecret`
  reports set/unset. Setting it derives + caches the PSK off the BT task.
- `blesecure on|off` — require the secure channel (`on` needs a secret first).
- Persisted settings: `bleSecureChannelSecret` (secret, hidden), `bleRequireSecureChannel`
  (bool, **default ON** — but only enforced once a passphrase is set, so a fresh device is
  plaintext for provisioning, then secure).

## 4. Modes (device behavior)
- **Plaintext** (no secret / `blesecure off`): REQUEST bytes are a UTF-8 command line; reply
  notified as plaintext. Exactly today's behavior.
- **Secure** (secret set): if the first REQUEST byte is a channel type, the device runs the
  handshake; once established, all traffic is encrypted frames.
- **Required** (`blesecure on`): a plaintext (non-frame) REQUEST is refused with the plaintext
  line `Secure channel required — connect with the encrypted app.` and not executed.

The device distinguishes a **frame** from a plaintext command by the **first byte**:
`0x01` (HELLO), `0x03` (CONFIRM), `0x10` (DATA) → frame; anything else (printable ASCII
commands start ≥ 0x20) → plaintext.

## 5. Crypto parameters (exact — verified vs vectors)
- **X25519** (RFC 7748; libsodium `crypto_scalarmult`, scalar clamped internally).
- **PSK** = `PBKDF2-HMAC-SHA256(passphrase_utf8, salt = ASCII "HW1-SC-v1", iters = 100000,
  dkLen = 32)`. **Salt is `"HW1-SC-v1"`** (no suffix).
- **KDF** = `HKDF-SHA256(ikm = ss‖PSK, salt = appNonce‖devNonce, info = ASCII "HW1-SC-v1",
  L = 64)` → `K_c2d = K[0:32]` (app→device), `K_d2c = K[32:64]` (device→app).
- **AEAD** = ChaCha20-Poly1305-**IETF** (RFC 8439), 12-byte nonce, 16-byte tag, **empty AAD**.
- **Nonce(dir, ctr)** = `dirTag(4B big-endian) ‖ ctr(8B big-endian)`; `dirTag = 0x00000000`
  (c2d) / `0x00000001` (d2c).

## 6. Wire format
`message = type(1) ‖ body`, binary, sized to fit `MTU − 3`:
```
0x01 HELLO        appEphPub(32) ‖ appNonce(16)                       app → device   (49 B)
0x02 HELLO_ACK    devEphPub(32) ‖ devNonce(16)                       device → app   (49 B)
0x03 CONFIRM      ct(2) ‖ tag(16)  = AEAD(K_c2d, nonce(c2d,0), "ok")  app → device   (19 B)
0x04 CONFIRM_ACK  ct(2) ‖ tag(16)  = AEAD(K_d2c, nonce(d2c,0), "ok")  device → app   (19 B)
0x10 DATA         ctr(8 BE) ‖ ct(N) ‖ tag(16)                        either dir
```
Detached AEAD order in the frame is **ciphertext then tag** (matches the combined `ct‖tag`
the app's lib produces). `"ok"` = `6f 6b`.

## 7. Handshake — what the firmware does (it is the responder)
1. Receives **HELLO** (`0x01`, 49 B). Generates its own ephemeral X25519 keypair + 16-byte
   `devNonce`. Computes `ss = X25519(devEphPriv, appEphPub)`. Derives `K` (both nonces known).
   Stores `K_c2d`/`K_d2c`; sets `rxCtr = 0`, `txCtr = 1`. Replies **HELLO_ACK** (`0x02`).
   State → *awaiting CONFIRM*.
2. Receives **CONFIRM** (`0x03`). Decrypts with `K_c2d`, nonce `(c2d, 0)`; requires plaintext
   `"ok"`. On success replies **CONFIRM_ACK** (`0x04`) = AEAD(`K_d2c`, nonce `(d2c, 0)`, `"ok"`).
   State → **established**.
3. Any AEAD-open failure during CONFIRM (**wrong passphrase / MITM**): the device **wipes the
   connection's channel state** and sends no CONFIRM_ACK → the app should treat the absence /
   failure as "wrong passphrase."

## 8. Data phase
- **Inbound DATA** (`0x10`): read the 8-byte counter, **reject `ctr ≤ rxCtr`** (replay/reorder),
  decrypt with `K_c2d` + nonce `(c2d, ctr)`, set `rxCtr = ctr`, run the plaintext as a command
  line. **One command per DATA frame** — the device executes each frame's plaintext as a
  complete line; do not split a command across frames.
- **Outbound DATA** (replies): the device chunks the plaintext into **consecutive DATA frames**,
  each with the next `txCtr` (`K_d2c`, nonce `(d2c, ctr)`), counters strictly monotonic. **The
  app concatenates decrypted payloads in arrival order** (BLE notifications are in-order on a
  connection). **No end-of-reply marker in v1** (append as frames arrive). Current device chunk
  size = **200 plaintext bytes/frame** (→ 225-byte frame); safe for negotiated MTU ≥ 228
  (Android `requestMtu(517)` far exceeds this).
- A tag failure on inbound DATA → frame ignored (counter not advanced).

## 9. Login & command execution
`login <user> <pass>` runs **inside** the data phase (as DATA plaintext). Encryption =
confidentiality; login = authorization. Until logged in (when `bluetoothRequireAuth`, default
true) only `login`/`logout`/`whoami` work; others return `Authentication required`. Commands
run on a worker task; replies arrive asynchronously (~one command → one reply, possibly
multi-frame).

## 10. Per-connection state & lifecycle
- Channel state (keys, counters) is **per `connId`**; wiped on connect AND disconnect.
  Reconnect ⇒ **re-run the handshake** (no persisted state, no bond).
- Multi-client note: the RESPONSE characteristic notifies all subscribers, so with >1 client
  the others receive your encrypted frames and drop them (wrong key). Confidentiality holds;
  it's benign extra traffic. Single phone = exact. (True per-client radio targeting would use
  `esp_ble_gatts_send_indicate(conn_id, …)` — a refinement, not done in v1.)

## 11. Verified test vectors (shared regression fixture)
The firmware reproduces all of these (verified via libsodium):
```
passphrase  = "test-passphrase"
appEphPriv  = 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
devEphPriv  = 2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40
appNonce    = a0a1a2a3a4a5a6a7a8a9aaabacadaeaf
devNonce    = b0b1b2b3b4b5b6b7b8b9babbbcbdbebf

PSK         = bb2618c01518a49da4508693a69e803efd6f4bd24f7a67adb53eb5c97a60b8ff
appEphPub   = 07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c
devEphPub   = 5869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67b
ss          = a84dc7c3c8f058b1b2dc4cd1e9b5dc0a7987f88b6a9564cde3391fc421159e77
K_c2d       = 613cb9d4d1f08af8d37bf2e6eac0cc51b2f6151a0e97a171d3c5761b8f05368b
K_d2c       = 01bf12a0d6e3b1ef2d024894cd213f6ffb72788c033bdb90fb68666451bc6be1
CONFIRM     = 032a690c7bf82c3bb321b0c10cf971a71568a1
CONFIRM_ACK = 04b20a930da943f0801f14f7e7696496210c71
DATA "help" (c2d, ctr=1) = 1000000000000000017d06562d2d6324e923d16e3d38c373b5ed3f5437
```
A standalone re-derivation (Python + pynacl/libsodium) reproducing these lives in the commit
history / can be regenerated; treat these as the canonical regression fixture for BOTH sides.

## 12. Known caveats / validate on hardware
- **BTC_TASK stack:** RESOLVED — the handshake is deferred off BTC_TASK (8 KB) to `cmd_exec`
  via `submitDeferredToCmdExec`; the BLE callback only copies the frame + queues. PBKDF2 uses
  HW-SHA and is pre-derived off the callback. HW-validated end-to-end.
- **Chunk size** is a fixed 200 B (Android-safe); can be made `negotiatedMTU − 28` for iOS /
  low-MTU clients.
- **No reply-boundary marker** in v1 (terminal-append). Add a "final frame" flag if the app
  needs explicit command/response correlation.

## 13. Interop checklist for the app side
1. PBKDF2 salt = exactly `"HW1-SC-v1"`, 100000 iters, SHA-256, 32 B.
2. HKDF ikm = `ss‖PSK` (that order), salt = `appNonce‖devNonce`, info = `"HW1-SC-v1"`, 64 B;
   split `0:32` / `32:64`.
3. Nonce = `dirTag(4 BE) ‖ ctr(8 BE)`; c2d = 0, d2c = 1.
4. Empty AAD; tag 16 B; frame layout `type ‖ [ctr] ‖ ct ‖ tag`.
5. Counters: CONFIRM / CONFIRM_ACK = 0; first DATA each direction = 1; strictly monotonic;
   reject `ctr ≤ last_accepted`.
6. One command per app→device DATA frame; reassemble device→app replies by concatenation in
   arrival order.
7. First secure REQUEST = HELLO (`0x01`); reconnect ⇒ new handshake.

## 14. Hardening applied (2026-06-11) + deferred follow-ups

Applied since the initial report (wire protocol / vectors / §13 checklist UNCHANGED):
- `blesecret` passphrase is now **redacted** in command/audit logs (was logged in cleartext).
- Passphrase **policy**: ≥10 chars with uppercase + lowercase + digit + symbol (was ≥8). The
  app should enforce the same so both ends accept the chosen passphrase.
- **Encryption required by default** (`bleRequireSecureChannel` defaults on). `bleScRequired()` =
  require && secret-set, so a device with no passphrase stays plaintext for provisioning, then
  enforces once a secret is set.
- `blesecret` is **refused over Bluetooth** — provision via serial/OLED/web so the passphrase
  never traverses BLE.
- **Data-service streams** (sensor/system/event, `…de11–de14`) are **suppressed in required mode**
  (they are NOT on the Secure Channel — prevents a plaintext leak).
- **Boot console** prints a "BLE is UNENCRYPTED — set a passphrase" notice when required-but-unset.

Deferred (TODO — not yet done):
- **Surface the "unencrypted BLE" warning beyond the serial console.** Today only the boot
  console shows it. Add one or more: a **web-dashboard banner** (recommended — most-seen; mirror
  the existing "Detected but not compiled" banner, gated on `require && !secret`), an **OLED boot
  splash** for headless users, and/or a **passphrase prompt in the first-time-setup** Bluetooth-Mode
  page so it's provisioned during setup. *(May fall out naturally of later UI work.)*
- **Encrypted Data-service streaming** (vs. suppression) if live sensor/event push is ever needed.
- Lower-severity audit items still open: per-device PBKDF2 salt, AAD-bind the frame type/counter,
  failed-handshake rate-limiting, at-rest PSK encryption (see the security audit).
