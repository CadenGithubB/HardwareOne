# BLE roles offline evidence — 2026-09-27

All checks below passed against the prepared
`experiments/p4_ble_roles/private/app` source copy. No serial port, Bluetooth
radio, network service or attached device was accessed during these checks.
This report establishes host-level behavior only; hardware results belong in a
separate report.

| Check | Observed result | What it exercises |
| --- | --- | --- |
| `test_hal_bluetooth.py` | 3/3 passed; no skips | Actual HAL with controlled native ESP32, native S3 and Hosted drivers: state, initialization, checked teardown, restart and power capability/mapping. |
| `test_client_mtu.py` | 10/10 passed; no skips | Actual G2/R1 MTU helper: discovery before request, callback completion, retained negotiated MTU, missing/disconnected client, empty/disconnected discovery, request failure, wait timeout, clock wrap and disconnect during wait. |
| `test_gatt_lifetime.py` | 10/10 passed; no skips | Actual checked teardown, terminal tree cleanup, factory ownership, descriptor adoption and map removal code with controlled drivers and lifetime-counting stubs. |
| `test_board_control_offline.py` | 10/10 passed | Fixture retirement before every guarded lifecycle verb; failures prevent execution; actual `OK: state=off` replies are accepted, while initialized or multiple status replies are rejected; pending R1 connect blocks probe use; compound commands cannot bypass interception; exact clock command grammar. |
| `tests/test_native_protocols.py` | 4/4 passed with ASan/UBSan | Complete current G2/R1 codecs and embedded vectors, actual graph core, extracted role-owner functions. R1 emitted 59 named passing checks. |
| `tests/test_ring_notify_security.py` | 9/9 passed with ASan/UBSan | Actual R1 subscription block and descriptor `setAuth`/`writeValueChecked`: encrypted CCCD request, acknowledged completion and setup rejection on failures. |
| `tests/test_prepare_idf_bt.py` | 14/14 passed | Synthetic SDK preparation fixtures: exact copying/patching, idempotence, version/content/mode checks, symlink containment, refusal of unknown output, and cleanup on failure. |
| `tests/test_g2_submit_lifetime.py` | 2/2 passed with ASan/UBSan | Actual G2 enqueue/teardown admission code across six scenarios; a negative control restores the old guard placement and reproduces the stuck gate. |
| `tests/test_power_profile.py` | 1 test passed across 3 compiled target policies | Actual power policy, checked clock changes, G2/OLED labels and real ArduinoJson serialization for S3, P4 revision 3.x and older P4 builds. |
| Existing `EmbeddedJsSyntaxTests` | 5/5 passed; no skips | Embedded raw-string JavaScript syntax across the prepared firmware, including the changed power page, plus extraction and checker coverage assertions. |
| Existing `test_g2_conversate.py --sanitize` | Passed | Production G2 protocol plus extracted owner and receive functions: captured frames, strict malformed RX, owner generations, CLI, session deadlines, close/re-entry, pause and recovery. |
| Existing `test_g2_files_psram.py --sanitize` | 6,976 checks passed in each of two variants | Actual reader/viewer, capture reveal, paging and memory behavior with PSRAM enabled and disabled; allocation and filesystem boundaries mocked. |
| Existing `test_secure_channel_v1.py -v` | 24/24 passed | Synthetic handshake/key/nonce vectors, directional authenticated records, counter/replay rejection, reply fragment boundaries, malformed/incomplete reassembly and companion cleanup. |

The four durable native tests regenerate wrappers from the prepared source on
each run. They do not embed a second copy of the production codec or role
algorithm. Only platform includes, time/critical-section primitives, debug
output and allocation boundaries are substituted for the codec checks. The
role test uses host mutexes and task identities to exercise same-owner nesting,
cross-thread exclusion, nonowner release, balanced release and fault publication.
The health graph test compiles the existing production-header harness directly.

## GATT lifetime coverage and ownership contract

`test_gatt_lifetime.py` extracts the current `BLEDevice::deinitChecked`, the
three terminal tree-cleanup methods, characteristic factory, descriptor
adoption, and real map iteration/removal methods from the prepared Arduino
copy. Its 10 scenarios cover:

1. Successful terminal retirement and idempotent repeated teardown.
2. Host-disable failure retaining the tree.
3. Host-deinitialization failure retaining the tree.
4. Controller-disable failure retaining the tree.
5. Controller-deinitialization failure retaining the tree.
6. Unknown controller acknowledgement retaining the tree.
7. An already-active lifecycle transition retaining the tree.
8. A removed service remaining outside the server's terminal cleanup.
9. Borrowed characteristics, descriptors and callback pointers surviving cleanup.
10. Twenty simulated create/retire cycles returning every owned object.

The synthetic tree includes duplicate descriptor map registration and a derived
descriptor, checking that cleanup destroys each adopted object once. Driver
states, callback-drain events and object destructors are host substitutes;
destructors count lifetimes and reject destruction before terminal host and
controller state. The test does not execute real GATT callback races, vendor
destructors, NimBLE or an allocator/heap measurement. It uses ordinary C++17
compilation; the ASan/UBSan claim above applies to the separately identified
native protocol, R1 notification security, Conversate and PSRAM checks.

In the shared application, four static callbacks now outlive individual server
generations, and four `BLE2902` CCCDs explicitly transfer ownership to their
characteristics. The server owns services still registered in its tree;
services own factory-created characteristics; characteristics own descriptors
only after explicit adoption. Each owned object must have a single parent.
Public attachment APIs otherwise borrow, callback pointers remain borrowed,
and runtime removal APIs retain their existing contracts. Cleanup is
Bluedroid-only and entered from checked terminal teardown; partial shutdown
must retain the object tree. These changes address allocation ownership but do
not establish that a physical repeated-role heap leak has been eliminated.

## R1 encrypted notification subscription

`tests/test_ring_notify_security.py` extracts the current R1 subscription block
and the real `BLERemoteDescriptor::setAuth` and `writeValueChecked` methods from
the prepared source. All nine checks passed with AddressSanitizer and UBSan:

1. An acknowledged encrypted CCCD write permits setup.
2. An authentication error stops setup.
3. A missing CCCD stops setup.
4. An immediate write API rejection stops setup.
5. A completion timeout stops setup and leaves the pending generation gate closed.
6. A disconnected completion stops setup.
7. A notification registration failure stops setup.
8. An already-disconnected client never submits a descriptor write.
9. The real application `pairAuth` setup remains after the subscription gate.

The harness verifies CCCD value `01 00`, a write with response, the existing
10-second wait arguments, and `ESP_GATT_AUTH_REQ_NO_MITM` reaching the descriptor
write API. This security request means link encryption without MITM protection;
it does not replace the later R1 application authentication. Both lookups use
the same characteristic-owned cached descriptor, with no ownership transfer.

BLE/RTOS calls, characteristic registration and event delivery are controlled
host substitutes. The ninth check verifies source ordering; the other eight
execute extracted code. These checks do not execute SMP pairing, establish the
ring's security policy, or prove peripheral interoperability and real callback
timing. The subsequent device results are recorded separately in
[FINAL.md](FINAL.md).

## SDK preparation, G2 submission and power checks

The SDK preparer's 14 tests use synthetic files and a small patch. They check
that unrelated baseline changes, extra files, changed version or executable
modes, output tampering, redirected paths, escaping links and patch offsets
are rejected. Valid internal source links become regular output files;
existing valid output is checked without rewriting. Failure leaves no published
component or staging directory. Separately, a fresh reconstruction of the real
2,046-file component matched the manifest and all five upstream Git blob
identities. See [SDK-FIX.md](../../SDK-FIX.md) for the exact coverage and patch.

The two G2 submission tests extract `pageSwapEnqueue`, its runtime snapshot,
submission-release guard, and the actual `g2ClientInitIdle` admission gate.
Six scenarios cover repeated lifecycle-stamp rejection, unavailable runtime,
allocation failure, full queue, another outstanding claim, and successful
enqueue. The rejection case runs 300 times to expose counter accumulation.
Queue entry must keep teardown blocked until its claim is released. The
negative control restores the old guard placement and must fail at the stuck
counter assertion. RTOS, allocation and lifecycle-stamping boundaries are
mocked; this is not a real scheduling or accessory transition test.

The power test compiles the extracted policy/apply/JSON and G2/OLED row builders
for **80/160/240**, **100/200/400**, and **90/180/360 MHz** profiles. It checks
rejected clocks, failed setters/readback, no success events on failure, full
frequency labels and JSON round trips. Payload sizes were 823, 831 and 823 bytes
respectively, within the 1,024-byte response buffer. These checks use ordinary
C++17 compilation and real ArduinoJson headers; clock drivers and displays are
substitutes. The G2 picker abbreviates `Perf` and `Ultra` so active/idle MHz stay
complete within its 23-character rows.

The five JavaScript checks are the existing `EmbeddedJsSyntaxTests` class,
covering extraction, a minimum region count, checker completion, syntax and
matching checked-region counts. They are **not** five power-button interaction
tests and do not establish DOM, HTTP or physical clock behavior. Initial device
clock observations and remaining limits are recorded separately in
[POWER.md](../../POWER.md).

## Reproduce

Run from the repository root after preparing the source copy. A host C++17
compiler with AddressSanitizer/UndefinedBehaviorSanitizer and Python 3.9+ are
required for the native checks. Secure Channel uses the previously isolated
Python environment with the dependencies documented in the
[BLE procedure](../../../p4_connectivity/BLE_TESTING.md).

First verify that the prepared source matches its recorded experiment inputs:

```sh
python3 -B experiments/p4_ble_roles/prepare.py --check
python3 -B experiments/p4_ble_roles/prepare_idf_bt.py --check
```

Run the independent suites in fresh Python processes:

```sh
python3 -B experiments/p4_ble_roles/test_hal_bluetooth.py -v
python3 -B experiments/p4_ble_roles/test_client_mtu.py -v
python3 -B experiments/p4_ble_roles/test_gatt_lifetime.py -v
python3 -B experiments/p4_ble_roles/test_board_control_offline.py -v
python3 -B -m unittest discover -s experiments/p4_ble_roles/tests -v
python3 -B experiments/p4_ble_roles/tests/test_ring_notify_security.py -v
python3 -B experiments/p4_ble_roles/tests/test_prepare_idf_bt.py -v
python3 -B experiments/p4_ble_roles/tests/test_g2_submit_lifetime.py -v
python3 -B experiments/p4_ble_roles/tests/test_power_profile.py -v
python3 -B experiments/p4_ble_roles/private/app/components/hardwareone/test/host/test_g2_conversate.py --sanitize
python3 -B experiments/p4_ble_roles/private/app/components/hardwareone/test/host/test_g2_files_psram.py --sanitize
experiments/p4_connectivity/private/ble-env/bin/python -B \
  experiments/p4_ble_roles/private/app/tools/ble_secure/test_secure_channel_v1.py -v
```

For the five embedded-JavaScript checks, run from the prepared application
directory with a supported JavaScript engine available:

```sh
cd experiments/p4_ble_roles/private/app
python3 -B -m unittest tools.webui.tests.test_embedded_js_syntax.EmbeddedJsSyntaxTests -v
```

The explicit new test commands are focused reruns; discovery under `tests`
also includes them. `test_g2_submit_lifetime.py` supports `HW1_BLE_ROLES_APP`,
and `test_power_profile.py` supports `HW1_POWER_TEST_APP` for a different source
copy. Missing prerequisites or syntax-suite skips do not count as passing.

Use separate processes as shown: broad discovery across the experiment root
can resolve the identically named connectivity HAL test after the coordinator
adds its shared-tool directory to `sys.path`; unittest rejects that directory
collision. This is a host test-discovery limitation, not a firmware failure.

`tests/test_native_protocols.py` accepts another reconstructed source root
through `HW1_BLE_ROLES_APP`; its default is this experiment's `private/app`.
The R1 notification security suite supports the same override. Its explicit
command above is a focused rerun; test discovery also includes all nine checks.
It creates temporary translation units/executables and fails if prerequisites
are missing. The HAL, MTU and GATT lifetime suites instead report skips when
their prepared copy or compiler is missing: **skips are not passing qualification
evidence**.

The existing host CMake tree also exposes `g2_conversate`, `g2_files_psram` and
`g2_health_graph_core_tests`. Direct commands above avoid requiring CMake and
do not execute the unrelated full application host suite.

## Limits and remaining hardware checks

These tests do not emulate Bluedroid, the Hosted HCI transport, C6 controller,
FreeRTOS timing, RF congestion, actual GATT discovery, physical PSRAM placement,
accessory firmware or the Android app. Passing the role-token unit does not
prove complete server-to-client or client-to-server teardown.

No separate existing host suite was found for the complete R1 control-owner
lifecycle or complete application role transition. Production R1 initialization
also runs storage, transaction-ordering, telemetry/custody and protocol
self-tests; G2 initialization checks renderer liveness. The protocol and
Conversate portions executed above must not be described as execution of every
boot self-test or as a successful physical accessory connection.

Actual G2/R1 traffic, role transitions and Wi-Fi/ESP-NOW coexistence require
their own device evidence; the scoped final-image results are in
[FINAL.md](FINAL.md). These host tests alone establish none of those physical
outcomes. Android application testing and longer endurance remain outside the
completed checks.

## Diagnostic fixture hazard

The inherited `Connectivity_BleProbe` keeps a static `BLEClient*` and GATT
pointers. `BLEDevice::deinitChecked()` deletes all registered clients after
terminal teardown, but the fixture is absent from
`bleCentralClientsTerminalTeardownAcknowledged()`. Leaving it allocated across
a role change can make a later fixture command dereference freed storage.

The roles coordinator contains this known fixture limitation by requiring
successful `bleprobe disconnect` and then disconnected/unsubscribed status
before lifecycle commands. It also blocks diagnostic scan/connect unless
`g2status` reports `state=off L=down R=down` and `ringstatus json` reports no
connection or pending connect. G2 has no equivalent JSON CLI field; this is the
actual production text format, where `off` reflects initialization state.

These serial checks cannot atomically exclude autonomous accessory reconnect or
role changes initiated through HTTP/OLED/another console. Keep automatic
reconnect disabled during diagnostic-central runs and retire the fixture before
handing ownership to the real G2/R1 client. The coordinator tests validate this
containment, not a firmware-level repair of the fixture's lifetime handling.
