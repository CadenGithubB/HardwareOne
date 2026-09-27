# Board-only Bluetooth role qualification — 2026-09-27

The P4/C6 and XIAO S3 completed the board-only role-switching qualification on
ESP-IDF 5.5.5. Both switched between the real HardwareOne Server role and an
initialized, idle G2 Client role. Repeated cycles no longer showed the earlier
large downward heap trend. Encrypted board-to-board BLE, HTTP and ESP-NOW checks
also passed. **G2/R1 accessory links and the Android application are not
qualified by this result.**

Evidence comes from serial run `20260927T181111Z-7c325f0e` and its associated BLE
and HTTP runs. The publishable measurements and firmware hashes are in
[AFTER-CLEANUP.json](AFTER-CLEANUP.json); the earlier comparison is retained in
[BEFORE-CLEANUP.json](BEFORE-CLEANUP.json). Raw serial logs, credentials, device
addresses and response contents remain private.

## What passed

| Check | Result |
| --- | --- |
| Exclusive application roles | Four measured Client→Server cycles on each board, with the alternate role stopped. Client was initialized and idle, with both G2 temples disconnected. |
| BLE Server behavior | Three complete probe runs: P4 as server twice and S3 as server once. Initial connection and reconnect passed in every run: six sessions, all negotiated MTU 517. |
| Authentication and encryption | Each BLE session began unauthenticated, denied a protected command, accepted a named admin login, and returned eight verified encrypted command replies over Secure Channel v1. Device Information, status and fresh reconnect behavior were checked. |
| HTTP while Client mode was idle | S3 fetched and checked 19 P4 HTTP responses totaling 849,727 body bytes while P4's real G2 Client role remained idle. Login, five HTML pages, two JSON APIs, authenticated CLI, logout and post-logout denial passed. |
| Encrypted mesh coexistence | Sixteen 401-byte messages passed in both directions, each reconstructed from three encrypted pieces and checked against the intended contents: 12 during BLE sessions, two during the HTTP callback and two afterward. |

The diagnostic central was retired before production role changes. For BLE
server tests, the other board supplied the GATT radio client, while the host
used the existing Secure Channel protocol client over USB. No Mac Bluetooth
access or Android UI was involved. The HTTP check used the S3's Wi-Fi client
against the P4 test AP; it was not a browser-rendering test.

The HTTP run was one complete functional cycle, not a sustained load test.
The six BLE sessions establish successful exchanges under the tested bench
conditions, not an RF reliability rate across distances or environments. No
panic/abort signature appeared in the saved board logs for this run.

## Heap behavior and cleanup

Server-mode free DRAM after each measured cycle, in bytes:

| Board | Cycle 1 | Cycle 2 | Cycle 3 | Cycle 4 | Observed spread |
| --- | ---: | ---: | ---: | ---: | ---: |
| P4/C6 | 228,335 | 228,355 | 228,307 | 228,387 | 80 |
| S3 | 88,259 | 88,259 | 88,259 | 88,243 | 16 |

The earlier source lost roughly 11 KB over successive role cycles. The updated
source reuses four static callbacks, explicitly owns four notification CCCDs,
and releases the owned server/service/characteristic/descriptor tree only after
checked Bluedroid terminal teardown. Borrowed objects and callback pointers
retain their external ownership; runtime removal APIs keep their contracts.

The measured four-cycle result supports the cleanup change: the prior large
per-cycle loss did not recur on either board. It does not establish unlimited
restart endurance, zero allocations elsewhere, or heap behavior with connected
G2/R1 accessories. The [offline report](OFFLINE.md) separately records ten
extracted-code lifetime scenarios, including partial-shutdown retention and
twenty simulated ownership cycles.

## Serial retry and end state

The earlier HTTP attempt failed after 16 responses because an IDF warning
interleaved with the serial completion line. The coordinator now enables the
existing `loglink on` runtime queue after each login. The distinct run reported
here completed the HTTP callback, remaining requests and logout successfully.
The earlier attempt remains failed; see [SERIAL-NOTE.md](SERIAL-NOTE.md).

At the end of this recorded run, both boards reported Server mode advertising
with zero connected clients, G2 off, ring disconnected with no connect pending,
and accessory automatic reconnect disabled. Later accessory work uses a
separate run and is outside this qualification's end-state snapshot.

## Firmware and source scope

The final application binary metadata matches the independent private firmware
hash record:

| Target | Application bytes | SHA-256 |
| --- | ---: | --- |
| P4 | 4,100,496 | `12b95c9af746aaef25eed3b8ca957bb896df48afea5b5bd07537bcdc73767c7e` |
| S3 | 4,736,384 | `0a0a31c4459b0877b8363c4f9b33f06f9acdfd99731f2e610f659fddd882305e` |

Bootloader and partition-table hashes are included in
[AFTER-CLEANUP.json](AFTER-CLEANUP.json). The experiment retains patched
Arduino-ESP32 3.3.5 and Hosted 2.12.13 with ESP-IDF 5.5.5. These results do not
require an ESP-IDF 6 migration.

This BLE-role experiment changed its independent `private/app` copy and durable
overlays, not the original application source. Concurrent peripheral work in
the main working tree is separate. Source reconstruction and build instructions
are in the [experiment README](../../README.md).

## Pending accessory and Android qualification

Actual G2 and R1 connection, protocol traffic, accessory recovery and role
switching while accessories are connected belong to a separate ongoing phase.
No health readings or accessory identifiers are included in this report.
Android discovery, connection, authentication and UI behavior still need a real
Android application test. Longer repeated-role and mixed-radio stress tests
remain useful beyond this short board-only qualification.
