# Board qualification — 2026-09-28

Investigation completed on `codex/jpeg-portable`. This phase left the primary
checkout and production sources untouched.
The available hardware is a headless ESP32-P4X-EYE with its camera and a XIAO
ESP32-S3 Sense. No SD cards, glasses, ring or Android phone are available for
this round. The Mac's network interfaces and Bluetooth are not test peers.

## Preservation

Both full-flash backups were read and verified against their physical devices
before configuration or firmware changes. Backups, accounts, raw serial logs and
camera images remain in ignored `private/run-20260928/` storage.

| Board | Chip | Flash | Verified backup SHA-256 |
|---|---|---|---|
| P4 | ESP32-P4 revision 3.2, 32 MiB PSRAM | 16 MiB | `64b2f6eea59c6eeae2596a3f1f06cebe3b66cd9bc911b231a3d710861448563f` |
| S3 | ESP32-S3 revision 0.2, 8 MiB PSRAM | 8 MiB | `500802cadbf71273347d066fb86586293a20d882e8a11c37fdea2a4e2b63c722` |

Existing known test accounts authenticated; no account reset was needed. Both
boards were already reciprocally paired on mesh channel 6. Probe applications
replace only the factory application at `0x10000`; bootloaders and partition
tables remained untouched. Original mutable flash contents have been restored.
Both complete flash images matched their original backups before the restored
boot, and both reached setup completion with no observed panic. Post-boot
authentication and readback verified the original reciprocal peer registries,
mesh slot/fingerprint, channel 6 and Performance clocks (P4 400 MHz, S3 240 MHz).
Runtime log routing was returned to its original state and both USB consoles
were closed. Normal boot can change counters after the exact-flash check.
See [restoration evidence](restoration.json).

## Existing application mesh baseline

All 33 checks in run `63419af2` passed on the original installed applications:

- Encrypted text of 1, 201, 202, 203, 400, 401 and 1,024 bytes in both directions.
  Reassembled bytes were verified, not merely the sender's success response.
- 1,025-byte text was rejected at the intended boundary on both boards.
- 8,192-byte files completed in both directions, with complete receiver readback
  and SHA-256 verification. Each direction had one transfer in this run.
- Session key rotation followed by successful traffic after the old-key grace
  period expired.
- Ordered P4 ESP-NOW close/reopen, followed by successful encrypted traffic.
- No RX-ring drops. S3's general failed-message counter increased from 4 to 5;
  P4 remained at zero. This is not a claim of loss-free radio operation.

The pairing check deliberately preserved existing identities: **fresh discovery
pairing was not tested by this run**. A successful file run does not resolve the
source-level missing retry/receiver-commit-acknowledgment gaps or the previously
observed missing-chunk failure. See [MESH.md](MESH.md). Ordered radio reopen also
does not simulate an unexpected C6 restart.

A separate fresh-pair attempt initially timed out waiting passively for the
receiver notification. The notice appeared when a later statistics query
flushed console output; that run alone did not prove request loss.

A second attempt actively polled the receiver. Both boards discovered each
other, S3 observed and explicitly accepted P4's request, but **S3 saved P4 while
P4's peer registry stayed empty** throughout the 30-second readiness check.
Fresh request/accept pairing therefore failed this test. No implicit secure-pair
repair was used. [Passive observation](mesh-fresh-passive.json) and
[active-poll evidence](mesh-fresh-active.json) retain both runs. Original peer
registries were restored from backup and verified on both devices.

## Camera probes

S3's standalone camera control detected **OV3660** (`0x3660`). It captured 35
320×240 frames, deinitialized/reinitialized the driver, then captured 35 640×480
frames with zero reported capture errors. Rates were 25.38 and 24.71 fps for
these short runs. The final VGA JPEG was 16,873 bytes. Chunk order, length and
CRC passed; Pillow decoded it and visual inspection showed a coherent scene.
Heap integrity passed; no panic.

P4 detected **OV2710** (`0x2710`). The first probe stopped before capture because
it incorrectly required the driver to fill an optional `sizeimage` field. The
corrected probe supplies bounded RGB565 geometry, still validating the driver's
allocated buffers and each returned frame size. Both subsequent runs captured
90 1280×720 frames at 24.24 fps with no reported errors, then encoded a JPEG
using the hardware engine in about 10.1 ms. Heap integrity passed; no panic.

The first valid P4 image was almost black. After the user pointed the uncovered
camera at a lit area, the unchanged probe produced a recognizable, continuous
scene. It remains soft/noisy, so this establishes basic image output rather than
calibrated focus/colour quality. The second JPEG was 121,716 bytes; chunk order,
length, CRC and independent image decode all passed. That leaves only 9,356
bytes below the application's 128 KiB stored-JPEG limit: scene detail can change
compressed size, so a shared resolution/quality/output-budget policy is needed.

These standalone probes do not initialize HardwareOne, radios, SD or the LCD.
They qualify sensor/driver paths independently; P4 camera integration in the
application remains separate work. See [CAMERA_STORAGE.md](CAMERA_STORAGE.md)
and [camera fingerprints/results](camera_probe/RESULTS.json). The probes used
different resolutions and sensor pipelines; their FPS figures are not a
controlled CPU-performance comparison.

## Full-application JPEG, roles and clock tests

Separate diagnostic applications built successfully for both boards, preserving
the original feature profiles, with camera/Sense disabled. The guarded USB
`jpegdiag` command invokes the actual G2 stored-JPEG loader and BMP converter.
See [INTEGRATION.md](INTEGRATION.md) and [build fingerprints](jpeg-diagnostic-builds.json).
S3 passed all 44 checks: all 16 committed fixtures with their expected supported
or rejected outcomes, four derived malformed/limit cases, a missing file, exact
uploaded-file readback and ten repeated QVGA conversions. Repeated RGB/BMP hashes
and heap integrity remained stable; free internal heap was 185,135 bytes at both
ends of that short batch. This is not long-duration leak qualification.

P4's initial run lost the USB result for a SHA-verified QVGA 4:2:2 fixture, with a
partial command audit and intact completion barrier. Its `-> OK` audit alone
cannot distinguish successful conversion from a JSON-reported decode failure.
No panic was observed. Two later post-restart conversions returned matching
hardware RGB/BMP hashes and intact heap. A second run encountered a truncated
USB file-read response before decoding the next large fixture. Its completion
barrier was lost inside that response, so the coordinator stopped. A third run
used paced 256-byte readback and completed the remaining cases,
including exact readback of the 115,107-byte fixture. Both original failed runs
remain preserved. Across these restarted runs, all 20 fixture outcomes, the
missing-file case and ten repeated QVGA conversions have successful evidence.

P4 repeated hashes and heap integrity stayed valid. Internal heap changed from
207,519 to 206,991 bytes while background application tasks were running; its
largest free block stayed 163,840 bytes. This short batch neither establishes
nor rules out a leak. [Machine-readable integration results](jpeg-integration-results.json)
retain expected failures, per-image checks, timing and original run failures.

The symptom is consistent with Arduino HWCDC's queued-output handling during
connection-state changes, but that cause is unproven. Do not treat the initial
missing response as a passing conversion or the retries as uninterrupted-run
recovery. Passing conversion also does not imply visible G2 output, which needs
the glasses.

## Bluetooth and clock checks

The first new role-transition run stopped before switching roles: P4 returned
`G2 deinit deferred; control owner did not stop`. The generic CLI error covers
several guards and does not identify the refusing owner.

A second run used explicit ring disconnect and G2 close, then a bounded 12-second
idle-state wait. G2 became idle with both arms down, but R1 remained disconnected
with `connectPending=true` throughout that wait and the cleanup wait. The test
therefore stopped without forcing a mode switch or bypassing shutdown guards.
**No new Bluetooth role-cycle or encrypted-server pass was obtained this round.**
Prior successful prepared-accessory/server tests remain valid in their recorded
conditions. Investigate shared connect-worker cancellation and pending-state
cleanup; this result alone does not isolate a chip-specific defect.
[Bluetooth evidence](bluetooth-results.json) preserves both failed runs.

The final full-flash restore recovered original persistent state on both boards,
independently of these failed helper-cleanup attempts. Those failures remain
recorded rather than being reclassified as successful role tests.

Active CPU readbacks passed at **100/200/400 MHz on P4** and **80/160/240 MHz
on S3**, then returned to the original 400/240 MHz. Preset values did not change.
[Clock evidence](clock-results.json) tests selection and readback, not load
stability, electrical consumption, idle scaling or sleep/wake recovery.

## Explicitly unqualified

- Actual Android app interaction and visible G2/R1 behavior this round.
- Physical SD I/O, recording-to-card and SD/C6 coexistence under load.
- Full-application P4 camera capture, camera/mesh/BLE concurrency and 1080p policy.
- Independent C6 crash/reset recovery and coordinated light-sleep/wake handling.
- Electrical power consumption and long-duration endurance.

The audits identify shared camera, storage and radio lifecycle boundaries that
can keep application behavior portable. They are implementation proposals, not
new production features.

## Recommended next work

1. Repair shared mesh request/accept persistence and bounded retries; retain
   explicit consent and test dropped/duplicate messages on both boards.
2. Trace shared R1 connect cancellation and completion before retrying Bluetooth
   role cycles; do not force teardown while a connect worker still owns the host.
3. Isolate the USB output-loss cause and make diagnostic responses reliable.
4. Add a shared camera boundary with S3 sensor-JPEG and P4 CSI/ISP/JPEG backends,
   plus common geometry and compressed-size limits. Reuse/review the SD HAL
   already present in the primary checkout when cards are available.

These are proposed follow-ups; no production repair was applied in this phase.
