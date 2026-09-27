# P4/C6 and S3 Bluetooth role investigation

This experiment enables the real G2 glasses and R1 ring clients in an independent
copy of the preceding [connectivity experiment](../p4_connectivity/README.md).
It investigates switching between **Client mode** for G2/R1 and **Server mode**
for the Android app. The two application modes are mutually exclusive. Original
application sources are not edited by this BLE-role experiment; its changes
live in the independent copy and overlays. Concurrent peripheral work in the
main working tree is separate.

The shared application uses native S3 Bluetooth or the P4's Bluedroid host with
the EYE's C6 radio over Hosted SDIO. `HAL_Bluetooth` provides lifecycle, status
and power capability boundaries; G2/R1 protocol and application logic stay
shared. Client mode owns accessory connections and their control tasks. Server
mode owns the HardwareOne GATT services, application authentication and Secure
Channel. Enabling both features in a build does not mean running both roles
together.

The [board-only qualification](results/2026-09-27/RESULTS.md) passed: four measured
role cycles on each board with stable server-mode DRAM, six encrypted BLE
sessions, a complete 19-response HTTP check while P4 Client mode was idle, and
16 encrypted mesh messages. The subsequent [clean-image qualification](results/2026-09-27/FINAL.md)
also passed on both boards: both G2 temples plus R1, Client-to-Server transitions,
four encrypted BLE sessions, 19 HTTP responses with P4 accessories active, and
12 encrypted mesh messages. The user confirmed the P4 menu and ring swipe
navigation. The upstream TinyCrypt fix and shared lifecycle-counter correction
are included in those final images. P4 was restored to Client with all three
accessory links up; S3 was left advertising in Server mode.

The known Even-app step after rebooting accessories remains a prerequisite for
the tested setup. Android UI and long-duration endurance are unqualified.
[Accessory history](results/2026-09-27/ACCESSORIES.md) preserves the earlier
diagnostic failures and their fixes.
[Offline checks](results/2026-09-27/OFFLINE.md) document the supporting code-level
coverage and rerun commands.

## Prepare the isolated source

Use the already reconstructed `../p4_connectivity/private/app` baseline. If it
is missing, follow its [build instructions](../p4_connectivity/BUILD.md), which
also explain the preceding mesh baseline. A clean upstream checkout alone does
not include the working-tree changes preserved by those experiments.

From the repository root, with Python 3.9+ and the system `patch` tool:

```sh
python3 -B experiments/p4_ble_roles/prepare.py
```

Preparation verifies the recorded baseline, creates a separate source tree,
applies `app-overlay.patch` and `arduino-overlay.patch` with zero fuzz, and
verifies the output before publishing `private/app`. It refuses an existing
destination. To verify an existing prepared copy without changing it:

```sh
python3 -B experiments/p4_ble_roles/prepare.py --check
```

`source-manifest.json` records baseline, output, overlay and build-input hashes.
Generated build directories, managed components, credentials and logs are
outside the source snapshot. `--refresh` is a maintainer operation for recording
intentional source changes, not a preparation step.

## Build

Retain the sibling `p4_espnow`, `p4_mesh`, `p4_connectivity` and `p4_ble_roles`
directories. Activate the existing ESP-IDF **5.5.5** environment, with patched
Arduino-ESP32 **3.3.5** and Hosted **2.12.13** dependencies inherited from the
baseline, then build sequentially:

```sh
bash experiments/p4_ble_roles/build-s3.sh
bash experiments/p4_ble_roles/build-p4.sh
```

Both wrappers prepare and verify the same clean IDF 5.5.5 Bluetooth component
override at `private/idf-components/bt` using `prepare_idf_bt.py`. It applies the
five-file official TinyCrypt fix, leaves the shared SDK unchanged and contains
no temporary SMP diagnostics. The [SDK fix procedure](SDK-FIX.md) records exact
provenance, verification commands and qualification limits. There is no IDF 6
migration; the planned 5.5.6 release is not yet available at this snapshot.

These commands build only. Separate `private/build-s3` and `private/build-p4`
directories keep target SDK configurations apart. Both use `features.h` and
`sdkconfig.connectivity.defaults`; P4 also layers
`sdkconfig.p4.bluetooth.defaults`. Use fresh generated SDK configurations when
changing defaults. Component downloads may require registry access. This
experiment does not require an ESP-IDF 6 migration or create/install new C6
companion firmware.

The profile enables G2, R1 health, BLE, HTTP and encrypted ESP-NOW while keeping
camera, local microphone, display and unrelated features disabled. It is an
investigation profile, not complete P4X-EYE peripheral support.

## Server object ownership

Repeated Server/Client transitions create and retire the server's GATT tree.
The shared application now reuses four static callback objects instead of
allocating callbacks at every server start. Its four notification CCCDs
(`BLE2902` descriptors) explicitly transfer ownership to their characteristics.
Callback pointers remain borrowed and are never deleted by tree cleanup.

The isolated Arduino overlay adds Bluedroid-only terminal cleanup to the
checked teardown path. After host and controller retirement are confirmed, the
server deletes its registered services, each service deletes its factory-owned
characteristics, and each characteristic deletes explicitly owned descriptors.
Owned objects must have a single parent; borrowed objects retain their external
owner. Runtime removal APIs keep their existing behavior. Failed or incomplete
teardown retains the tree rather than freeing objects that may still be used.

All 10 extracted-code lifetime scenarios passed, including failure retention,
borrowed-object survival and 20 simulated restart cycles. In the subsequent
four-cycle hardware check, server-mode free DRAM varied by only 80 bytes on P4
and 16 bytes on S3; the earlier roughly 11 KB per-cycle loss did not recur.
This supports the cleanup change over the measured run, not unlimited restart
endurance or behavior with connected accessories. See the [hardware results](results/2026-09-27/RESULTS.md)
and [offline evidence](results/2026-09-27/OFFLINE.md) for their separate scopes.

## Role control and fixture ownership

The CLI separates mode preference from initialization. The intended Client
sequence is `blemode client`, `g2init`, then deliberate accessory connection
commands such as `openg2 auto` and `ringconnect`. The intended Server sequence
is `blemode server` followed by `openble`. Status must establish the actual
runtime state: `g2status` reports `state=off` only when G2 is uninitialized;
`blestatus json` reports server initialization; `ringstatus json` reports the
ring connection and pending connect. A saved mode alone is insufficient proof.

The inherited `bleprobe` diagnostic central is separate from production G2/R1
ownership. It must be retired **before** stack teardown or a role change:
terminal stack teardown deletes its client object, while the fixture retains
its pointer. It also shares the global BLE scanner with the accessory owners.

Use this experiment's `board_control.py` for coordinated serial work. Its
`RolesConsole` requires acknowledged `bleprobe disconnect`, then positively
disconnected/unsubscribed status before lifecycle and accessory connection
commands. Diagnostic scan/connect requires G2 off and R1 disconnected with no
connect pending. The guard applies only to this coordinator; avoid role changes
through another transport and keep accessory automatic reconnect disabled
while using the diagnostic central. The separate Server-mode check should
exercise Android or the inherited Secure Channel probe after Client mode has
fully stopped.

## Coordinator and private data

Only the primary coordinator accesses hardware. With both application images
provisioned and other serial clients closed, its explicit launch command is:

```sh
experiments/p4_connectivity/private/ble-env/bin/python -B \
  experiments/p4_ble_roles/board_control.py
```

Importing the module opens no ports. Running it opens both existing board ports,
waits for startup, logs in using `private/credentials.json`, and automatically
enables runtime `loglink on` after each board login. This routes IDF logs through
the application output queue to address serial line interleaving; the
[serial note](results/2026-09-27/SERIAL-NOTE.md) records the failed attempt and a
distinct successful retry. It does not perform first-time onboarding or repair mesh pairing.
The local credentials copy is mode 0600. No secret belongs in a shell command or
stdin job.

JSON jobs include:

```json
{"action":"setclock"}
{"action":"command","board":"p4","command":"g2status"}
{"action":"command","board":"p4","command":"blemode client","timeout":65}
{"action":"exit"}
```

`setclock` uses `timeset <host Unix epoch>` and `tzoffsetminutes <offset>` on
both boards, with no Wi-Fi requirement. Optional `board` selects one board;
optional `tzoffsetminutes` overrides the host's current local UTC offset.
Other inherited actions are `setup`, `mesh`, `ble` and `http`; `setup` explicitly
provisions test BLE credentials/name and the test AP, so it is not needed merely
to switch roles.

Serial, BLE and HTTP results use new timestamped directories under this
experiment's ignored `private/`. `ConnectivityConsole` redacts credentials and
omits encoded HTTP response bodies from logs while preserving raw responses in
memory for assertions. Private flash backups and logs may still contain
sensitive device data; never publish them. No accessory firmware update or
factory reset is part of this experiment.
