# P4 / S3 full-feature investigation

Investigation completed on 28 September 2026 in the isolated
`codex/jpeg-portable` worktree. Production sources were not changed this phase.
Both boards were restored from verified original flash backups, then their
accounts, reciprocal pairing and configuration were checked. Private backups,
credentials, raw logs and camera images remain ignored.

## Test matrix

| Area | This round's evidence | Next check/work |
|---|---|---|
| JPEG | S3 passed 44 checks; P4 completed the matrix across restarted runs, with two preserved USB failures | USB output reliability, visible G2 output, endurance |
| Mesh | Existing pair passed 33 checks including exact text/file receipt; fresh pairing saved only the accepting S3 peer | Shared request/accept reliability, session deduplication, receiver-confirmed file completion |
| Camera | Both standalone probes produced valid images; P4 captured 720p at 24.24 fps and hardware-encoded JPEG in about 10.1 ms | Shared camera backend and image size/quality policy; P4 image quality needs tuning |
| Bluetooth / Android | New mode-cycle attempt blocked by pending R1 connect after cancellation | Shared worker cancellation; repeat role tests, then actual Android app |
| Combined workload | No new sustained workload qualification | Endurance and memory/latency monitoring after reliability fixes |
| Recovery | Ordered P4 radio close/open passed; no independent C6 fault injected | Unexpected companion restart and identity preservation |
| Power | Active clock readbacks passed: P4 100/200/400 MHz, S3 80/160/240 MHz | Peripheral load, idle scaling and sleep/wake; electrical measurement needs a meter |
| Storage / recording | Source audit only; neither board has an SD card | Review existing primary-checkout SD HAL, then physical I/O and C6 coexistence |

The P4 has its camera but no local display. The S3 is the XIAO ESP32-S3 Sense
control. Only boards were available this round. Mac Bluetooth and network
interfaces were not changed or used as test peers. Both boards use factory
application layouts; OTA layouts and security provisioning were outside scope.

See [RESULTS.md](RESULTS.md) for physical outcomes and qualifications,
[MESH.md](MESH.md), [CAMERA_STORAGE.md](CAMERA_STORAGE.md) and
[INTEGRATION.md](INTEGRATION.md) for subsystem findings, and
[restoration.json](restoration.json) for final preservation evidence.

## Results and reproducible checks

[RESULTS.md](RESULTS.md) records physical outcomes and their limits. The
[full-app JPEG coordinator](test_jpeg_integration.py) accepts explicit board,
port, radio MAC and a private credentials-file path. It never flashes a device;
it stages synthetic files in a unique directory and invokes the guarded
[diagnostic patch](jpeg-diagnostic.patch). The [camera probes](camera_probe/README.md)
are separate applications, not a production camera backend.

## USB console tip

HardwareOne's current serial input handler ignores carriage returns and submits
on newline. In macOS `screen`, typed characters are not echoed: use **Ctrl+J**
to submit `login <username> <password>`, wait for successful authentication, then
submit `reboot` with Ctrl+J. A normal reboot preserves configuration. No stored
credential values belong in these notes.
