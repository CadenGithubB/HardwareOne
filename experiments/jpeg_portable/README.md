# Portable JPEG qualification

This experiment qualifies one shared JPEG decode interface on ESP32-S3 and
ESP32-P4. The S3 keeps Espressif's existing TJpgDec software decoder. A qualified
P4 SDK component enables hardware decoding for the supported subset, with
software fallback for the other previously supported images. JPEG encoding and
camera JPEG passthrough are unchanged. The G2 stored-image viewer and its live
camera preview use the interface; Edge Impulse is outside this change.

All generated app copies, downloaded dependencies, firmware and logs remain in
`private/`. No script flashes a board or opens a serial port. The previous
experiment snapshots and installed SDK are never edited.

## Reproduce full application builds

Activate ESP-IDF 5.5.5 and provide the root of the checkout containing the prior
private snapshots. On the dedicated Mac mini the existing activation script is
`/private/tmp/hw1-p4-investigation-20260927/activate-idf.sh`.

```sh
. /private/tmp/hw1-p4-investigation-20260927/activate-idf.sh
python3 -B experiments/jpeg_portable/prepare.py --target p4 --baseline-root /path/to/baseline/checkout
python3 -B experiments/jpeg_portable/prepare.py --target s3 --baseline-root /path/to/baseline/checkout
bash experiments/jpeg_portable/build.sh p4
bash experiments/jpeg_portable/build.sh s3
```

`prepare.py` validates every source against the prior checked-in manifests. It
creates separate copies of the qualified P4 I/O and S3 BLE-role snapshots, adds
the five shared codec files, and applies the checked-in G2 integration patch.
Three exact CMake anchors register those sources and dependencies. The snapshots
already pin `esp_jpeg` 1.3.1 and retain that pin. Local dependency caches are
copied, not shared as writable directories. Before every build, the private
qualification manifest verifies the baseline, overlaid files and current codec
source hashes. Preparation refuses to overwrite an existing destination.

The P4 build retains its LCD, wheel, buttons, G2/R1, web and mesh profile. The S3
qualification profile additionally enables its camera and Sense carrier so both
live-camera decode paths compile. Neither profile claims that every optional
HardwareOne feature has been enabled or tested.

P4 builds prepare a private `esp_driver_jpeg` component from the pinned SDK with
allocation-lifetime fixes. `prepare_idf_jpeg.py` verifies the original component
and patch before applying it; its public qualification define permits the
hardware backend. An ordinary, unqualified SDK keeps the software fallback.
The TinyCrypt Bluetooth component is also reproduced from the previous verified
experiment. Run the first P4/S3 builds sequentially because they share that
component's initial preparation destination.

After an intentional edit to only the HAL files, `--refresh-hal` updates those
five files in a verified private copy. Do not refresh while its build is active.
Changes to G2/CMake/dependency integration require a new destination and a new
captured integration patch; `--capture-integration` is a development operation,
not part of normal reproduction after committing the change.

## Self-contained firmware probe

After preparing the app copies (which provide the pinned software decoder):

```sh
bash experiments/jpeg_portable/build-codec.sh p4
bash experiments/jpeg_portable/build-codec.sh s3
```

The probe uses the actual shared HAL and the committed original JPEG fixtures;
it does not boot HardwareOne or use its settings, accounts, network or radios.
It reports:

- SoftwareOnly, Auto and HardwareOnly selection, decode times and output hashes.
- 4:4:4 / 4:2:2 / 4:2:0, odd dimensions, MCU-padded dimensions, grayscale,
  progressive input and solid red/blue channel-order checks.
- Tight visible dimensions/stride and exact pixels when Auto falls back to
  software; hardware/software maximum and mean pixel differences are reported
  because different IDCT/chroma rounding can produce different pixels.
- Malformed/truncated inputs, valid repeated-marker padding and output-budget rejection.
- Sixty decodes from two concurrent workers, hardware/software call counts,
  heap retention and allocator integrity.

Expected final output is `JPEG_RESULT failures=0`. Timings are probe measurements,
not a benchmark of the whole HardwareOne application. Merely compiling this
firmware does not qualify hardware behaviour or establish an acceleration gain.

The P4 probe targets the tested revision-3.x board at 400 MHz with 32 MB PSRAM;
the XIAO S3 probe uses 240 MHz, octal PSRAM and 8 MB flash. The scripts deliberately
have no flash command. Before a physical test, identify the exact board and
preserve its bootloader, partition table, application and configuration. Flash
only a reviewed layout-compatible image, capture the probe output, then restore
the saved complete application/layout and verify its usual startup and login.
Do not use a generic probe partition table to overwrite configured-device data.
