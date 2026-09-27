# Reproduce the connectivity source experiment

This is an incremental experiment on the verified `../p4_mesh/private/app`
snapshot, including its patched Arduino-ESP32 3.3.5 tree. It retains the same
ESP-IDF 5.5.5 toolchain and dependency locks. A clean checkout alone is not that
baseline: the previous experiment preserved the user's existing working-tree
edits. If the private baseline is unavailable, first reconstruct it using
[the mesh build instructions](../p4_mesh/BUILD.md).

## Prepare and verify

From the repository root, with Python 3.9 or newer and the system `patch` tool:

```sh
python3 experiments/p4_connectivity/prepare.py
```

The command verifies all 7,913 baseline files before copying them, applies the
two overlays with zero fuzz, verifies every resulting source file, and then
publishes the new `private/app` directory. It refuses an existing destination.
Neither the baseline nor existing build directories are modified. Generated
managed components, build caches, credentials and serial logs are not copied.

The already prepared working copy can be checked without changing it:

```sh
python3 experiments/p4_connectivity/prepare.py --check
```

`source-manifest.json` records the baseline hashes, changed/new source hashes,
patch hashes, and hashes of the durable build inputs (profiles, build scripts,
radio backend and shared ESP-NOW bridge). The check verifies recorded source
files; unrelated generated files in the existing copy remain outside its scope.
An independent reconstruction was verified against the same manifest.
A custom `--destination` is useful for
source verification; firmware builds expect the normal directory layout below.

## Build

Keep `p4_connectivity`, `p4_mesh`, and `p4_espnow` as sibling directories. The
copied main component uses `p4_connectivity/radio_backend.cpp` and the existing
`p4_espnow/bridge` implementation. Activate ESP-IDF **5.5.5**, commit
`b774170ff46c393eeb5e495ea37936038d3f4f4f`, with the same tools described in the
mesh experiment, then build sequentially:

```sh
bash experiments/p4_connectivity/build-s3.sh
bash experiments/p4_connectivity/build-p4.sh
```

The scripts use separate target/build/sdkconfig paths, the shared `features.h`,
and `sdkconfig.connectivity.defaults`. P4 additionally layers
`sdkconfig.p4.bluetooth.defaults` last to select a Bluedroid host with the C6
controller over Hosted SDIO. Start new build directories with fresh sdkconfig
files when changing defaults; IDF does not reliably replace persisted settings
from new defaults. The scripts build only; they do not flash or provision devices.
Component-manager downloads require registry access.

The C6 needs the already prepared ESP-Hosted **2.12.13** companion image from
the prior radio experiment, with its ESP-NOW overlay and Bluetooth controller
support. This source preparation does not create or install companion firmware.
Secure boot, flash encryption and NVS encryption remain explicitly disabled in
the experimental image configuration. Application authentication and Secure
Channel behavior remain part of the shared HardwareOne implementation.

## Overlay scope

`app-overlay.patch` adds the Bluetooth HAL, routes shared Bluetooth lifecycle,
status and power requests through it, uses the actual board name in Device
Information, and fixes the bounded MAC-cache copy and args-only `blename` and
`bletxpower` handlers. It also registers an admin test AP command with password
redaction and serial GATT/HTTP test clients. These clients are test fixtures,
not production central-role abstractions. The GATT fixture can run on either
backend for reversed-role testing; the HTTP fixture is registered only on the S3.

`arduino-overlay.patch` enables existing BLE wrapper sources for Hosted
Bluedroid, adds checked remote controller lifecycle/acknowledgement state,
attaches HCI before host startup, and tracks Bluetooth ownership of the shared
Hosted transport. Wi-Fi shutdown must not remove that transport while BLE owns
it; successful BLE shutdown does not shut down Wi-Fi. See
[HAL_DESIGN.md](HAL_DESIGN.md) for the interface and limits.

The manifest enumerates every changed/new path, including the existing Arduino
BLE feature guards. The patches are cumulative only relative to the mesh
baseline. Do not apply them to the original repository
or an unpatched upstream Arduino checkout. No hardware success is implied by
source reconstruction or compilation; device evidence is recorded separately.

## Offline checks and maintenance

```sh
python3 -m unittest discover -s experiments/p4_connectivity -p 'test_prepare.py' -v
python3 -m unittest discover -s experiments/p4_connectivity -p 'test_hal_bluetooth.py' -v
python3 -m unittest discover -s experiments/p4_connectivity -p 'test_hosted_bluetooth.py' -v
python3 -m unittest discover -s experiments/p4_connectivity -p 'test_ble_offline.py' -v
python3 -m unittest discover -s experiments/p4_connectivity -p 'test_http_body.py' -v
python3 -m unittest discover -s experiments/p4_connectivity -p 'test_http_s3_offline.py' -v
python3 -m unittest discover -s experiments/p4_connectivity -p 'test_connectivity_safety.py' -v
```

Reproduction tests cover exact add/change/delete output, missing final newlines,
baseline drift, existing destination preservation, patch tampering and path
escapes. HAL and controller tests compile actual isolated source with controlled
driver substitutes; they supplement firmware builds and hardware runs. Missing
private source or a host compiler may skip those source-level tests. BLE client
dependencies and test coverage are documented in [BLE_TESTING.md](BLE_TESTING.md).

The HTTP fixture's `httpprobe join` uses base64 SSID/password arguments and joins
only the channel-6 test AP. S3 keeps APSTA mode and changes its own soft-AP IP to
`192.168.5.1`, avoiding the P4 gateway's `192.168.4.1` address. HTTP requests target
that P4 address only. The fixture retains a private session cookie, follows no
redirects, hashes the complete dechunked body (maximum 2 MB), and reports HTML
markers across stream boundaries. Bodies up to 8 KiB are returned as base64 for
exact API/CLI checks; larger pages return their digest, size and marker flags.
Incomplete, oversized or timed-out reads fail. Credentials and POST arguments
must stay redacted in the coordinator's serial logs. Use `ConnectivityConsole`
so complete base64 HTTP body fields are omitted as well: replacing literal
credentials or base64 of each credential cannot redact a credential embedded
inside an encoded response body. Source-level stream tests
exercise bounds and marker handling with a hash-driver substitute; they do not
claim real HTTP delivery or validate the cryptographic implementation itself.
Harness safety checks cover body omission, literal BLE-secret provisioning,
asynchronous serial cancellation and refusal to reuse an unresolved timed-out
console. The BLE fixture explicitly requests MTU after discovery and waits for
the negotiated result; a successful API call alone does not pass negotiation.
`test_http_s3.run_probe_http` drives the fixture through a caller-owned serial
console and applies the login, protected API, page, named-admin CLI and logout
checks. An optional synchronous callback inserts mesh work between HTTP reads.
This fetches complete pages but does not execute browser JavaScript or exercise
interactive page controls. Offline runner tests cover the request sequence,
small-body digest validation, failure cleanup and output redaction.

After an authorized source fix has been frozen, maintainers can regenerate the
overlays without touching either app tree:

```sh
python3 experiments/p4_connectivity/prepare.py --refresh
python3 experiments/p4_connectivity/prepare.py --check
```

`--refresh` requires the verified baseline and writes only the durable patches
and manifest. Add newly introduced source paths to `NEW_SOURCES` in `prepare.py`
before capturing them; generated files are intentionally not discovered
automatically. Validate a refreshed overlay with a new disposable destination
before recording final firmware hashes. Do not hand-edit a captured patch
without refreshing its manifest and checking its output.
