# Shared microphone qualification — 2026-09-28

**P4 and S3 local microphone qualification passed:** 8/16/48 kHz acoustic
recordings, repeated start/stop, camera coexistence and unavailable-G2/auto
fallback. Both final images build, flash and reboot successfully; final
identity/settings preservation checks also pass. Firmware has not changed
since the verified v2 images below.

The implementation reuses `HAL_Audio` for onboard PDM and G2 sources. Board
configuration supplies pins, shared-rail control and microphone clock limits;
recording consumers receive the existing mono signed 16-bit PCM format. G2
remains fixed at 16 kHz. PDM source ID 1 and G2 source ID 2 are unchanged.

## Build and flash evidence

Built with ESP-IDF 5.5.5 from the verified camera baseline `05614eb`, using
separate sealed audio application copies. Both v2 builds completed. The initial
S3 build correctly rejected an enabled microphone without board pin definitions;
its final profile enables the real XIAO Sense expansion-board identity. Host
checks now compile the actual deployment profiles to catch this combination.

| Final application | Bytes | SHA-256 |
| --- | ---: | --- |
| ESP32-P4X-EYE | 4,893,600 | `a60482b3abcc1e126e9b29443c872687d3401409c30c3d1d1ad27e18832a7405` |
| XIAO ESP32-S3 Sense | 5,072,288 | `39dc3ebaf6ccb73f5853a54def1af4f4d6392c53c8aecce6af0d4df64b1f8fd6` |

Physical chip identities were checked and full flash backups verified (P4:
16 MiB; S3: 8 MiB). Both partition tables match their backups. Only the
application at `0x10000` was written, and esptool verified both flash hashes.
The post-flash boot captures report no panic. No provisioning or account reset
was performed. Final identity/settings preservation checks passed after reboot.
Evidence is retained privately in `private/run-20260928/` (`*-build-v2.log`,
`*-audio-v2-build.json`, flash logs and boot results).

## Physical audio evidence

Recordings use internal flash and the shipping microphone/recorder commands.
The workstation speaker emits a quiet 997 Hz reference; its microphone is not
used. Each tone temporarily changes output volume and restores it afterward.

| Board | PCM rate | Captured samples | Duration | Detected tone | Clipped samples | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| P4 | 8,000 Hz | 10,240 | 1.280 s | 996 Hz | 0% | Pass |
| P4 | 16,000 Hz | 18,432 | 1.152 s | 996 Hz | 0% | Pass |
| P4 | 48,000 Hz | 49,152 | 1.024 s | 998 Hz | 0% | Pass |
| S3 | 8,000 Hz | 11,264 | 1.408 s | 996 Hz | 0% | Pass |
| S3 | 16,000 Hz | 22,528 | 1.408 s | 996 Hz | 0% | Pass |
| S3 | 48,000 Hz | 34,816 | 0.725 s | 1,004 Hz | 0% | Pass |

All six recordings have valid mono 16-bit WAV headers, dynamic PCM, plausible
duration and a reference-frequency match within the test's ±8 Hz tolerance.
The detector searches in 2 Hz steps. The S3 48 kHz result is near the tolerance
edge; these short recordings confirm acoustic capture at each configured rate,
not precision clock accuracy, long-duration continuity or calibrated
sensitivity, noise floor or frequency response.

P4 coverage is complete across these private runs under `private/hardware/`:

- `20260928T163900Z-p4-14651d`: 8/16 kHz recordings passed.
- `20260928T164425Z-p4-9e46b0`: 48 kHz recording and three close/reopen cycles
  passed.
- `20260928T165443Z-p4-e94242`: camera capture during 16 kHz recording passed
  (3.968 s, 998 Hz reference, no clipped samples); camera still captured after
  microphone stop; microphone recorded after camera stop; unavailable-G2 and
  auto preferences selected local PDM. All four checks and all cleanup/settings
  readbacks passed, with zero file-transfer rereads.

The coexistence harness also confirmed mesh remained initialized with a paired
peer during recording. It did not send a mesh test packet. The P4 shared camera/
microphone power rail remained usable when either capture path stopped.

S3 completed all eight checks in
`private/hardware/20260928T165701Z-s3-0d54bc/`: the three rates above, three
close/reopen cycles, camera capture during recording (2.048 s, 996 Hz reference,
no clipping), camera operation after microphone stop, microphone operation
after camera stop, and unavailable-G2/auto fallback. Every cleanup/readback
passed and file transfers needed zero rereads. Its session is closed.

## Test-fixture issues and recovery

The initial P4 run `20260928T163627Z-p4-849e99` captured valid PCM but failed
its acoustic-reference check because the Mac output was muted at volume zero.
All that run's cleanup/readback checks passed. This was a fixture failure.

The next two P4 runs retained the passing checks listed above, then lost the
old file-download `whoami` completion barrier. Their automatic cleanup could
not proceed safely. The coordinator subsequently recovered/reset the console,
restored settings and deleted each exact interrupted test recording; evidence
is in `private/run-20260928/control-164317.json` and `control-165103.json`.
The later coexistence run independently verified clean final microphone/camera
states and restored source/rate/gain values.

File downloads now use a read-only helper that validates complete JSON replies
against the owned recording path, byte offset, size, length and base64 payload.
It transfers 512-byte chunks and allows bounded same-offset retries. Other
commands retain their FIFO completion barriers. The corrected helper completed
the final P4 coexistence run and full S3 run without retries; no firmware
change was needed.

## Final preservation checks

`private/run-20260928/postcheck-170213.json` reports every check passed for both
boards after reboot, with no panic. Existing device/mesh identity, channel,
paired peers and stored peer keys were preserved, as were power, BLE and HTTP
preferences. Wi-Fi remained unjoined. Camera settings were unchanged, and both
camera and microphone were left stopped with the onboard microphone available.

A brief microphone open/close verified the persisted settings on each board:
source `auto`, 16 kHz, gain 70%. P4 battery voltage remained valid. The Mac's
original output volume 0 and muted state were also verified restored. No
account provisioning, mesh re-pairing or new credentials were required.

## Host verification and limits

- `test_audio_hal_pdm.py --sanitize`: passed powered/unpowered PDM cases,
  driver-error cleanup, partial-timeout PCM, clock selection, source preference
  handling, 10 board/default-disable configurations and both real deployment
  profiles. Missing microphone wiring is rejected at compile time.
- `test_live_audio.py --sanitize`: passed after the final HAL changes.
- `test_wav_validation.py`: all 10 offline WAV/transport tests passed, including
  stale/truncated replies, invalid payloads, bounded retry recovery and fatal
  console guards.
- Neither board has an SD card. P4 SD stays disabled. Enabling the correct S3
  Sense identity declares its SD pins; existing no-card mount retries delay
  startup to roughly 12 seconds, unlike the preceding headless profile.
  Recordings fall back to internal flash. SD recording is unqualified.
- The P4 has no display connected. Display/input operation, live G2 microphone
  capture, ESP-SR, Android streaming, extended recording and power measurements
  are outside this run. G2 routing is retained and host-tested; no connected
  glasses are being used for this local microphone qualification.

Credentials, raw console logs, backups and captured audio remain in ignored
private storage. The local microphone milestone is complete within the tested
scope above; connected G2 capture remains a separate hardware check.
