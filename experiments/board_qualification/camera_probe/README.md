# Headless camera qualification probes

These standalone applications test the attached camera hardware independently of
HardwareOne. They do **not** establish that HardwareOne's camera HAL, G2 viewer,
web preview, mesh transport or video recording works on P4. They initialize no
NVS/settings, filesystem, SD, display or radio. Camera images and logs stay in
`../private/` and must not be committed.

The P4 probe powers the shared LCD/camera rail on GPIO12, generates 24 MHz XCLK
on GPIO11, and uses GPIO26 reset plus SCCB SDA14/SCL13. It reads the actual sensor
PID after OV2710/SC2336 auto-detection, captures 90 RGB565 frames through CSI/ISP,
and hardware-encodes the last frame as JPEG. OV2710 defaults to 1280×720/25 fps.
The MIPI driver owns LDO3 at 2.5 V. No LCD initialization is required. At completion
the camera pipeline is released and the shared rail is disabled.

The XIAO S3 Sense probe identifies its DVP sensor, captures 35 QVGA frames,
deinitializes/reinitializes, and captures 35 VGA frames. It emits the final
sensor-compressed JPEG. Reported frame rates are pipeline observations, not a
fair CPU benchmark: these are different sensors, resolutions and compression
paths. A transport CRC and changing frame hashes do not establish image quality;
independently decode and visually inspect the captured JPEG.

## Build

Use ESP-IDF 5.5.5 commit `b774170ff46c393eeb5e495ea37936038d3f4f4f`.
Activate that environment, then from the repository root:

```sh
bash experiments/board_qualification/camera_probe/build.sh p4
bash experiments/board_qualification/camera_probe/build.sh s3
```

The scripts only build. Source is copied into ignored private projects, both
component graphs are locked, and dependency-lock changes make the build fail.
P4 uses the existing `jpeg_portable/prepare_idf_jpeg.py` override with its validated
allocation/interrupt fixes. The global SDK is not edited. S3 uses the same
esp32-camera 2.1.4 / esp_jpeg 1.3.1 component hashes as the original control probe;
it no longer needs a pre-existing full HardwareOne build to locate them.

Source, package versions and settings reproduce the test configuration. Build
paths and toolchain metadata can change binary hashes; the originally qualified
USB-test binaries are preserved separately under `../private/camera-artifacts/`
and are never overwritten by this script.

## Flash and restore

1. Identify each chip and port; verify flash capacity and the current partition
   table. Back up the current application and settings, and verify those backups
   against flash before changing the application. Keep all backups private.
2. **Write only the application image at `0x10000`.** The tested boards' installed
   factory partitions start there. The probe's generated partition layout is
   different; never run `idf.py flash`, use generated `flash_args`, or flash its
   bootloader/partition table. Do not erase flash or format storage.
3. Capture the USB Serial/JTAG output immediately after reset. Each probe waits
   two seconds before starting. Allow up to 90 seconds for `CAM_RESULT`; retain
   the complete private log including `CAM_JPEG_BEGIN`, every `CAM_JPEG` chunk,
   and `CAM_JPEG_END`. Avoid printing captured image hex into chat.
4. Run the extraction helper below. Require successful capture counts, zero
   errors, `CAM_RESULT result=ESP_OK heap_ok=1`, validated JPEG geometry, and
   visual inspection. Decode success alone cannot prove correct sensor colour,
   focus, orientation or exposure. Record limitations separately.
5. **Restore the exact original application backup at `0x10000` afterward**, even
   if the probe fails, and verify restored bytes. Reset and check HardwareOne
   starts normally with its account/settings preserved. Do not leave the test
   firmware installed as the user's working application.

Root alone owns device access in this investigation; these source/build/extraction
helpers never open a serial port or flash a device.

## Extract and validate

With Python and Pillow installed:

```sh
python3 experiments/board_qualification/camera_probe/test_extract.py
python3 experiments/board_qualification/camera_probe/extract_camera_jpeg.py \
  experiments/board_qualification/private/camera-p4.log \
  experiments/board_qualification/private/camera-p4.jpg
```

The helper rejects duplicate captures, missing or reordered chunks, length/CRC
mismatches, invalid JPEG data, and existing output paths. It writes the unmodified
JPEG plus a JSON summary of geometry, hash and channel statistics. The bundled
Codex Python runtime on this Mac includes Pillow; the IDF environment does not.

Neither board currently has a microSD card, so SD read/write and recording tests
remain unavailable. See [camera/storage findings](../CAMERA_STORAGE.md) for the
shared camera/storage interface changes identified by the source audit.


## Recorded physical result

On 2026-09-28, the S3 control captured 35/35 frames at each size and produced a
validated 640×480 JPEG with a coherent room scene. P4 revision 2 captured 90/90
1280×720 frames and, after the lens was uncovered and aimed at a lit scene,
hardware-encoded a validated 121,716-byte JPEG in 10.177 ms. Both reported
intact heap after teardown. Exact tested firmware fingerprints and observations
are in [RESULTS.json](RESULTS.json).

The initial P4 v2 capture was nearly black. The lit retest used the same binary
without tuning changes and shows a continuous scene without gross tearing or
mosaic artefacts. The image remains soft/noisy; controlled focus, colour and
exposure qualification is still outstanding. The lit JPEG leaves just 9,356 bytes
below the current G2 file loader’s 128 KiB cap, so integration needs a shared
resolution/quality/output-budget policy. Full-app integration remains untested.
The original P4 probe's zero-`sizeimage` assumption failed before capture; revision
2 explicitly supplies the checked packed RGB565 layout used by esp_video 2.2.0.
Original firmware and failure evidence are preserved privately.
