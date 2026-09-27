# BLE testing through a board USB radio bridge

The Mac does not access Bluetooth. `test_ble.py` runs the existing HardwareOne
Secure Channel v1 Python codec, and `serial_gatt.py` forwards GATT operations to
a board over an authenticated USB serial console. The initial direction uses
native S3 as central and P4/C6 as server; the later backend-generic fixture also
allows P4/C6 as central and S3 as server. No OS pairing or macOS permissions are
used. Mac BLE launcher work was abandoned before any scan or permission request;
generated artifacts, if present, remain ignored under `private/`.

The coordinator owns all ports and physical runs. Both boards must already be
provisioned with the private test admin and have `openble` initialized. The P4
must advertise the HardwareOne command service, require authentication, and have the
private `ble_secret` provisioned with `blesecret` / `blesecure on` through serial.
Both boards retain the real ESP-NOW implementation. The bridge never stops or
deinitializes the shared BLE controller.

## What the test proves

The S3 scans for the exact advertised server name, or an explicitly observed BLE
MAC supplied with `expected_ble_mac` / `--ble-mac`, and connects using the
advertised address type. An explicit BLE address takes precedence over the name
and never falls back to another named device. The ESP-NOW MAC is checked separately inside the
server's authenticated response; the host does not derive a BLE address from it.
Each initial/reconnect cycle reads Device Information and GATT connection status,
subscribes to replies, establishes a fresh X25519/PSK + ChaCha20-Poly1305 Secure
Channel v1, proves the new connection is unknown and cannot execute `status json`
before login, then verifies encrypted named admin login, `whoami`, `status json`,
`blestatus json`, and `espnowstatus json` replies. GATT command/response counters
must advance. Disconnect must complete before a fresh connection and login.

There is no registered `version` CLI verb. GATT 0x2A26 firmware version is
compared with `status json` field `fw`. The experimental HAL now uses `BOARD_NAME`
for the model characteristic (P4 reports `Espressif ESP32-P4X-EYE`); the runner records it and
independently verifies the authenticated ESP-NOW MAC as the board identity.
Secure reply counters/fragments detect replay, missing or incomplete records.
The S3 queue's drop count and connection generation are checked on every RPC.
A successful write without the expected decrypted response cannot pass.

## Firmware bridge contract

The isolated app copy contains `Connectivity_BleProbe.cpp/.h` and exports
`connectivityBleProbeCommands` / `connectivityBleProbeCommandsCount` for the
normal module registry. The current fixture is compiled when Bluetooth is
enabled on either native S3 or hosted P4/C6. The earlier S3 firmware remains
compatible. Every operation requires a serial admin identity.

- `bleprobe status`: initialized, connected, subscribed, negotiated MTU.
- `bleprobe scan <1-10>`: up to eight HardwareOne service advertisements with
  exact names, observed MACs, address types and RSSI; requires disconnected state.
- `bleprobe connect <mac> [addressType]`: bounded native connection, MTU >=230,
  verified command/read/response services.
- `bleprobe subscribe`: checked CCCD registration and callback queue.
- `bleprobe read manufacturer|model|firmware|status`: base64 characteristic bytes.
- `bleprobe write <base64>`: one acknowledged Secure Channel record; plaintext
  login/commands are rejected by the bridge.
- `bleprobe poll [1-4]`: queued base64 notifications, byte lengths and generations.
- `bleprobe disconnect`: close link and checked client/callback retirement;
  retain the client and report failure if it cannot be safely deleted.

Responses include schema 1, `bridge: board-gatt-v1` (legacy `s3-gatt-v1` is also
accepted), `ok`, generation, connected,
drops, received and queued. Callback work is bounded to copying at most 517 bytes
into a 40-entry queue, without waits, logging or crypto. Command execution owns
JSON serialization and radio calls. Queue records contain only public handshake
or encrypted Secure Channel frames, not credentials. Source is retained in the
experiment's generated private app; reviewable source patches are collected by
the coordinator.

## Caller-owned console API

`run_probe` is async and does not open or close the caller's serial port:

```python
from test_ble import run_probe
result = await run_probe(
    existing_s3_console,
    private_credentials,
    expected_name="HW1_P4_BLE",
    expected_ble_mac="fc:01:2c:e0:b9:aa",  # optional: observed by S3 scan
    expected_mac="fc:01:2c:e0:b9:a8",  # authenticated ESP-NOW identity
    timeout=65,
)
```

The `ConnectivityConsole` subclass from `connectivity_redaction.py` must use
`completion="hardwareone"` and already be logged in
as the named serial admin. Its command serialization lets the background BLE
notification polling coexist with the coordinator's other S3 commands. Add
`on_connected=callback` to run mesh/HTTP checks while BLE remains connected.
The callback receives `(connection, cycle)` and may be sync or async; its return
value is captured as sanitized evidence. Another encrypted `whoami` then proves
BLE still works. The runner alone does not claim simultaneous mesh traffic.
The connectivity subclass also omits base64 HTTP body fields from logs; matching
literal credentials or base64(credential) cannot redact a credential embedded
inside base64(whole response body). Raw command replies remain in memory for
assertions. For a stopped coordinator's ignored private log,
`clean_private_log(path)` from the same module rewrites those fields without
printing data and returns the number removed. Do not clean an actively written log.
If a serial command times out, this subclass holds its transaction lock and waits
up to ten additional seconds for the old completion barrier. The test retains
the original timeout failure. If completion cannot be established, it latches a
fatal state and refuses further commands; the coordinator reports restartRequired
and closes its ports. Asynchronous cancellation also joins the actual serial
worker before cleanup, so no worker continues consuming responses after return.

The live P4 advertisement was observed with BLE MAC `fc:01:2c:e0:b9:aa` and an
empty name: the two advertised 128-bit services exhausted the legacy advertising
payload, and firmware logged a partial ADV write. The configured name getter
alone therefore cannot select it. Use that observed address for this test setup;
do not calculate it from the separate ESP-NOW MAC. The authenticated
`espnowstatus json` identity check remains mandatory.

Default output is a new `private/ble-runs/<timestamp>-<id>/` directory with
mode-0600 `ble.log` and `results.json`. Returned results include `resultPath`.
Credentials are read from a private dict/file with `username`, `password`, and
`ble_secret`. Every string credential value is redacted before serialization.
Decrypted fragments stay in memory until fully reassembled, then the complete
reply is redacted, so a secret split across notification records cannot leak.

## Standalone and offline use

Runtime dependencies are PyNaCl 1.6.2 (for the repository's existing Secure Channel
codec) and pyserial 3.5 (for standalone USB opening). The prepared private
`ble-env` has these installed; Bleak/PyObjC left there from the abandoned Mac
approach are never loaded by this runner. Any existing coordinator environment
with PyNaCl and its working MeshConsole is suitable.

Standalone invocation opens only S3 USB and therefore resets that board on this
hardware. Do not use it while another coordinator owns the port:

```sh
experiments/p4_connectivity/private/ble-env/bin/python \
  experiments/p4_connectivity/test_ble.py --physical \
  --s3-port /dev/cu.usbmodem1101 --ble-mac fc:01:2c:e0:b9:aa
```

Read dependencies without opening a port or accessing any radio:

```sh
experiments/p4_connectivity/private/ble-env/bin/python \
  experiments/p4_connectivity/test_ble.py --preflight
```

Offline checks (imports never open hardware):

```sh
python3 -m unittest discover -s experiments/p4_connectivity -p 'test_ble_offline.py'
experiments/p4_connectivity/private/ble-env/bin/python tools/ble_secure/test_secure_channel_v1.py -q
```

Actual results are recorded by the coordinator. Native S3 central against P4/C6
exercises the hosted server path; reverse P4/C6 central against S3 exercises the
hosted central path. For the reverse direction pass the existing P4 console,
the observed S3 BLE address, and expected_mac="68:ee:8f:50:e9:d0" to run_probe.
The standalone CLI still names its port option --s3-port; use the caller-owned
console API for the reverse test.
