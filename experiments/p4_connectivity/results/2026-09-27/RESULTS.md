# Connectivity results — 2026-09-27

The P4/C6 serves the real HardwareOne web interface to the S3, with working
login, protected APIs, command execution and logout. Encrypted BLE login,
commands and reconnect pass with P4/C6 as both central and server. Mesh messages
also pass during the central-role BLE connection. The server-role test required
Wi-Fi to be stopped on the S3 client; this phase does not establish reliable
operation under every combination of active radios.

All firmware changes belong to the isolated connectivity copy. Native S3 and
Hosted P4/C6 share the application and GATT/authentication code through the new
Bluetooth HAL. Builds use ESP-IDF 5.5.5, patched Arduino-ESP32 3.3.5 and the
ESP-Hosted 2.12.13 companion. See [BUILD.md](../../BUILD.md) for reproduction and
[HAL_DESIGN.md](../../HAL_DESIGN.md) for the boundary and status semantics.
No ESP-IDF 6 migration was needed. Native hardware coverage here is S3 only;
the original ESP32 target was not exercised on a physical board in this phase.

## Verified web and mesh behavior

| Test | Recorded result | Evidence |
| --- | --- | --- |
| HTTP baseline | 16 requests passed; 753,275 response bytes | [HTTP baseline](http-baseline.json) |
| HTTP load with mesh interleaving | 31 requests passed; 1,130,136 response bytes; 20-second load phase and five callbacks | [HTTP/mesh load](http-mesh-load.json) |
| HTTP repeat and final-image check | Two further 16-request passes, each 753,275 bytes | [Repeat](http-repeat.json), [final images](http-final-images.json) |
| Mesh delivery during that load | Ten 401-byte encrypted messages, five in each direction; each reconstructed from three pieces and checked against its expected SHA-256 | [Message records](http-mesh-load.json) |
| Ordered radio/BLE lifecycle sequence | Fourteen additional 401-byte encrypted messages, seven in each direction; mesh checks passed after BLE stop/reopen on both boards | [Lifecycle/mesh records](lifecycle-mesh.json) |
| P4/C6 central → S3 server | Initial connection and reconnect passed encrypted authentication and commands at MTU 517; four 401-byte mesh messages passed while BLE remained connected | [P4 central](ble-p4-central.json) |
| S3 central → P4/C6 server | Initial connection and reconnect passed encrypted authentication and commands at MTU 517, with S3 Wi-Fi off | [P4 server](ble-p4-server.json) |
| Final restoration check | Two further 401-byte encrypted mesh messages passed after reopening S3 ESP-NOW and restoring its original BLE power setting | [Final state](final-state.json) |

The four HTTP runs total **79 requests and 3,389,961 complete response bytes**.

The S3 was the HTTP client, controlled through its already authenticated USB
console. It joined the P4 AP on channel 6 as `192.168.4.2`; its own soft-AP IP was
changed to `192.168.5.1` to avoid a conflict with the P4 gateway `192.168.4.1`.
The P4 runs the actual HTTP server and application; the S3 fixture issues normal
HTTP requests and summarizes complete responses.

All four HTTP runs checked the login form, rejection of an unauthenticated protected
API request, named-account login with a session cookie and `303 /dashboard`,
all five pages below, two JSON APIs, authenticated admin `whoami`, three JSON CLI
commands, logout, and another protected API rejection. The expected `401`
responses are passing authentication checks, not transport failures.

| Page | Complete response bytes |
| --- | ---: |
| `/dashboard` | 84,879 |
| `/settings` | 215,961 |
| `/bluetooth` | 73,709 |
| `/espnow` | 258,063 |
| `/cli` | 95,636 |

The APIs were `/api/system` and `/api/buildconfig`; CLI checks were `whoami`,
`status json`, `blestatus json` and `espnowstatus json`. Every response was
bounded, read completely and hashed. Small bodies were independently checked
against their returned digest before JSON/CLI validation. Large pages required
both opening and closing HTML markers. This verifies serving and application
endpoints, not browser JavaScript execution or interactive page controls.

The HTTP load test deliberately places mesh callbacks between HTTP reads.
Bluetooth was advertising during that run. Separate BLE tests exercised mesh
traffic while a BLE link remained connected, then verified another encrypted
BLE command afterward. No run drove HTTP requests, mesh traffic and connected
BLE commands together. These bounded checks do not establish sustained throughput
or latency guarantees. The mesh
encryption claim refers to HardwareOne's application session and checked receive
history; it is not a new claim about native ESP-NOW link encryption.

## Bluetooth results and connection limits

P4 advertising was observed by S3. With Wi-Fi fully stopped on both boards, the
P4 still reported advertising and the S3 still received its advertisement. BLE
stop and reopen on each board left subsequent encrypted mesh exchanges working.
The shutdown logs also reported first-cycle heap deltas of approximately 11 KB
on P4 and 33 KB on S3. These are observed retention warnings, not proof of a
cumulative leak; long-run heap behavior remains unqualified.

Both successful directions verified a fresh X25519/PSK + ChaCha20-Poly1305 Secure
Channel v1, denial of a protected command before login, named-admin login,
`whoami`, `status json`, `blestatus json`, `espnowstatus json`, increasing GATT
counters, and an additional encrypted command after the optional mesh work.
Device Information matched the authenticated firmware and board identity.
Disconnect and reconnect started a new unauthenticated session. Both negotiated
MTU 517, and the large status reply was reassembled from five encrypted records.

P4-central → S3-server passed with Wi-Fi/ESP-NOW active on both boards. Four
encrypted 401-byte mesh messages, two per direction, were verified during its
two connected cycles. S3-central → P4-server repeatedly failed to establish a
link with S3 Wi-Fi active, including after S3 joined the P4 AP. It then passed
initial connection and reconnect after S3 Wi-Fi/ESP-NOW were stopped. P4 Wi-Fi,
the HTTP server and ESP-NOW remained enabled; no HTTP requests or mesh messages
were sent during that server-role run because the only peer's Wi-Fi was off.

Successful runs used S3 BLE transmit-power level 7. Its original setting, level
3, was restored afterward. Earlier scans/link attempts were inconsistent, with
`0x3e`/`0x85` connection failures and RSSI around −74 to −89 dBm despite adjacent
boards. Some early attempts failed even with Wi-Fi off. Radio scheduling,
power settings and the antenna/RF path need controlled qualification; the
observations do not isolate one cause or prove that proximity resolves it.

Two test-harness corrections were necessary. The existing Arduino wrapper can
discard CONNECT before OPEN assigns its connection ID, skipping automatic MTU
negotiation. The fixture now completes service discovery, explicitly requests
MTU when needed, and waits for the negotiated value. An initial `GATT_BUSY`
could also reflect another outstanding ATT operation, not necessarily duplicate
MTU exchange. The coordinator also stopped adding literal quotes to the BLE
passphrase: that handler consumes the whole argument without token unquoting.
No crypto protocol changes were made. Earlier failures are retained in
[BLE attempt summaries](ble-attempts.json).

G2/glasses/ring support remains disabled and untested. A working central-role
transport is useful groundwork, but it does not qualify the glasses protocol,
its optional code paths or multiple simultaneous connections.

## Remaining platform qualifications

Espressif's C6 coexistence table restricts ESP-NOW receive coexistence with BLE
to STA mode; other modes are outside that support classification. It also marks
SoftAP client activity with BLE as potentially unstable. The APSTA setup used
here therefore cannot establish a supported production combination merely
because these bounded tests pass. Prefer qualifying a STA-based normal operating
mode and a separately defined provisioning/AP mode. This is an architectural
inference from the [ESP-IDF 5.5.5 C6 coexistence guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c6/api-guides/coexist.html).

The HAL/controller audit leaves these specific limits:

- Hosted controller status records successful lifecycle acknowledgements; it
  does not query live C6 health or detect an unexpected companion reset.
- BLE ownership protects the shared Hosted transport during Wi-Fi shutdown.
  A fresh BLE-only start after the last owner fully deinitializes Wi-Fi/Hosted
  still needs explicit transport preparation and qualification.
- Shared Wi-Fi/BLE transport lifecycle calls do not yet have one common mutex;
  the successful ordered tests do not qualify concurrent start/stop calls.
- The application's four connection slots exceed the companion's configured
  three-controller-link limit. Multiple clients have not been qualified.
- Hosted BLE transmit-power control has no supported remote setter in this
  pinned adapter; it reports unavailable rather than claiming success.

The Mac joined the test AP and received DHCP address `192.168.4.2`, but local
processes and the browser could not reach the P4 (`No route` /
`ERR_ADDRESS_UNREACHABLE`). Local-network policy is a hypothesis, not a diagnosed
cause. Switching the HTTP client to S3 avoided relying on that path. Mac Wi-Fi
`en1` was restored to off, Ethernet was left unchanged, and no Mac Bluetooth
access was used.

## Evidence provenance

The sanitized JSON files here contain request metadata, hashes, message results,
redacted status objects and test statuses, without credentials, cookies, complete
HTTP bodies or encrypted command payloads. The connectivity console now omits
base64 HTTP body fields from logs; earlier closed private serial logs were also
filtered. The raw bodies remain available only in memory for test assertions.
Original private evidence remains under these ignored run IDs:

- `serial-runs/20260927T164703Z-5b27d6ad`: HTTP baseline and load with complete
  `coexistenceMessages` records.
- `http-s3-runs/20260927T164732.720408Z-4072994a`: baseline requests.
- `http-s3-runs/20260927T164842.217972Z-a66357ca`: load requests.
- `serial-runs/20260927T163655Z-de71653f`: ordered lifecycle and fourteen mesh
  messages.
- `ble-runs/20260927T164756.109424Z-9bf01e51`: reverse-role attempt before the MTU
  fixture correction; earlier connection failures are enumerated in the summary.
- `serial-runs/20260927T170922Z-c93dc9f8`: final firmware/setup, successful BLE
  tests in both directions, final HTTP pass and restoration checks.
- `ble-runs/20260927T171013.592247Z-95f9f39a`: P4 central with connected mesh tests.
- `ble-runs/20260927T171210.400317Z-f0f464f8`: P4 server, S3 Wi-Fi off.
- `http-s3-runs/20260927T171109.009621Z-bb9b67b6`: final-image HTTP pass.

Both final firmware images built and flashed with hash verification; their
SHA-256 values are in [images.json](images.json). C6 firmware was unchanged in
this phase. Source reconstruction verified all 7,919 prepared files from the
7,913-file baseline with zero-fuzz overlays. **40 offline checks** passed, plus
**24 existing Secure Channel protocol tests**. These supplement the hardware
evidence rather than substituting for it.

The original 16 modified/deleted working-tree paths still match their starting
snapshot. Original production sources and both previous phase copies were
preserved. Both probe clients were disconnected, the HTTP cookie was cleared,
mesh operation was restored and S3 BLE power returned to level 3. USB coordinator
ports were closed. The experimental images remain on the boards; factory/original
backups remain available under `p4_espnow/private/backups/`. The changes are local
to the investigation branch and have not been pushed.
