# Shared camera integration

This experiment connects the P4X-EYE CSI camera to HardwareOne's existing photo
features while retaining the XIAO ESP32-S3 Sense DVP camera. Production code uses
one camera interface; driver headers and hardware decisions stay in backends.

| Layer | Responsibility |
| --- | --- |
| `HAL_Camera.h` | Stable resolution IDs, supported controls, owned JPEG frames and bounds |
| `System_Camera_DVP.cpp` | Shared lifecycle, settings, commands, recovery and status |
| `HAL_Camera_DVP.cpp` | S3 sensor JPEG capture through esp32-camera |
| `HAL_Camera_P4.cpp` | P4 CSI/ISP capture and hardware JPEG encoding |
| Existing image manager, web routes and G2 consumers | Save, display and transport the same JPEG API |

The existing filename of the shared camera module is retained to avoid needless
consumer churn. Vendor camera types no longer escape through its public header.
The existing `HAL_JPEG` still supplies shared JPEG decoding.

## Behavior

- Existing saved resolution IDs 0–10 retain their meaning; HD is 11 and CIF is 12.
- Camera web controls and G2 settings use backend capabilities. Unsupported
  settings fail explicitly; a failed live change is not saved as a success.
- S3 retains its VGA memory limit and 96×96 guard. Its buffers are allocated for
  the largest supported geometry before selecting a smaller output. JPEG SOF
  dimensions are checked because driver framebuffer metadata can describe the
  current sensor setting rather than an older queued frame. Before accepting
  a frame, the shared JPEG HAL validates its entropy with discarded output;
  malformed frames are released and retried at most three times. The check
  needs decoder scratch rather than a full RGB image and avoids a vendor fork.
- P4 captures a native 1280×720 OV2710 image. Smaller outputs use a centered crop
  and software RGB565 resize; no upscaling is advertised. Encoding uses the IDF
  JPEG engine. Mirror/flip are supported. Sensor exposure/white balance remain
  automatic; unavailable manual controls are not presented as working knobs.
  Before encoder DMA, the output allocation is cleaned and invalidated so dirty
  zero-filled cache lines cannot overwrite the compressed stream. The pinned
  driver already synchronizes raw input and completed output; this fills its
  missing output-buffer handoff step.
- The shared output limit is 128 KiB. An image exceeding it fails explicitly;
  quality is never silently reduced to fit. Quality retains HardwareOne's
  existing 0–63 convention, with lower numbers meaning higher quality.
- Camera SCCB owns the actual SDK-selected I²C controller. General sensor bus
  initialization cannot claim that controller. The S3 test profile selects I²C0
  so the primary Wire1 sensor bus stays available; an already occupied controller
  is rejected before driver initialization. P4 uses I²C0 independently.
- P4 camera teardown leaves the shared LCD/camera power rail asserted.

## Reproduce the applications

These app copies extend the qualified battery/IO/BLE/JPEG applications. The
primary checkout is not used as a firmware staging directory. `prepare.py`
checks every inherited source, applies the exact camera overlay, and records
hashes. It leaves previous qualified experiment copies unchanged.

Use the existing pinned ESP-IDF **5.5.5**, including the already qualified local
Bluetooth/JPEG component overlays. ESP-IDF 6 is not needed. P4 adds pinned
`esp_video` **2.2.0** and its lockfile dependencies. S3 retains esp32-camera
**2.1.4**. P4 uses `dependencies.lock.esp32p4`; S3 uses `dependencies.lock`.

```sh
source /private/tmp/hw1-p4-investigation-20260927/activate-idf.sh
python3 -B experiments/camera_portable/prepare.py --capture
python3 -B experiments/camera_portable/prepare.py --target p4
python3 -B experiments/camera_portable/prepare.py --target s3
bash experiments/camera_portable/build.sh p4
bash experiments/camera_portable/build.sh s3
```

For existing copies use `--refresh` after `--capture`. `--accept-lock` accepts
Component Manager manifest-hash normalization only after exact dependency
versions, component hashes and direct dependencies match the qualified inputs.
Do not refresh a copy while its build is running. Regenerate an existing
`sdkconfig` when changing SDK defaults, then inspect the generated values.

Both profiles retain existing BLE, mesh, power and battery choices. Camera and
its web Sensors page are enabled. Neither microphone nor SD is enabled in this
board-only qualification. P4 local display/input support remains compiled for
users who reconnect those accessories.

## Verification

Host checks compile production camera code with mocked hardware boundaries:

```sh
python3 components/hardwareone/test/host/test_camera_core.py --sanitize
python3 components/hardwareone/test/host/test_camera_dvp.py --sanitize
python3 components/hardwareone/test/host/test_camera_p4.py --sanitize
python3 components/hardwareone/test/host/test_camera_consumers.py --sanitize
python3 components/hardwareone/test/host/test_camera_i2c_reservation.py --sanitize
python3 components/hardwareone/test/host/test_jpeg_validation.py --sanitize \
  --jpeg-component experiments/camera_portable/private/app-s3/managed_components/espressif__esp_jpeg
python3 -m unittest discover -s tools/webui/tests -p 'test_camera_page.py'
python3 -m unittest discover -s tools/webui/tests -p 'test_embedded_js_syntax.py'
```

`test_hardware.py --help` describes the explicit USB test. It never flashes,
creates accounts or repairs pairing. Supply existing private credentials,
verified ports/MACs and an image Python with Pillow. Photographs and logs stay
under ignored `private/`. It restores camera settings and removes only its own
test photos. Optional HTTP temporarily uses P4's isolated AP and S3's Wi-Fi
client; reboot both boards afterward to retire that temporary network state.

Use `--board p4` or `--board s3` to open only that board's USB console. Both
port/MAC arguments remain required, but the unselected board is never opened.
`--smoke` saves one photograph per selected board; `--smoke --http` also runs
both-board encrypted mesh and HTTP checks without the full resolution sweep.
HTTP requires `--board both`. For a repeated single-size check, use
`--board s3 --resolution 8 --captures 5` (QCIF, quality 12); supported IDs are
listed by `cameraread`. This replaces the resolution sweep, permits 1–20
captures, and restores the original camera settings afterward. `--resolution`
and `--smoke` cannot be combined.

Each downloaded image is decoded fully with Pillow. Qualification also records
SOS entropy spans and zero-run offsets/lengths in `results.json`, rejecting runs
of 64 or more consecutive zero bytes inside scan data. This diagnostic catches
the observed P4 DMA/cache corruption signature, which a JPEG decoder can accept.
JPEG headers are excluded, including headers between scans. This is a test
heuristic, not a JPEG validity rule or a replacement for visually inspecting
images; production image acceptance does not use it.
Build the strict decoder against a matching libjpeg header/library installation:

```sh
python3 experiments/camera_portable/build-jpeg-validator.py \
  --jpeg-prefix /path/to/libjpeg
```

The prefix contains `include/jpeglib.h` and `lib/libjpeg`; `--include-dir` and
`--lib-dir` override either location. The default output is ignored
`experiments/camera_portable/private/jpeg-validate`. Pass its absolute path as
`--jpeg-validator PATH` to `test_hardware.py` to run the warning-fatal libjpeg
validator after Pillow decoding and record strict validation on each image.
Unlike ordinary Pillow decoding, it rejects libjpeg's recoverable entropy
warnings. Use this validator together with visual inspection for qualification.
A completed USB file-read command with a missing, malformed-base64 or incorrectly
sized chunk is reread at the same offset up to three times. Timeouts and fatal
console errors propagate immediately; no bad chunk is accepted. Retry counts
and offsets are retained in `results.json` separately from image diagnostics.

If a saved photograph fails image qualification, the harness downloads that
same board file once more using smaller chunks, retains both private artifacts,
and records exact byte/hash agreement and the second decode outcome. The run remains failed even
if the second download passes. The current file-read API has no board-side
checksum; a local hash identifies received bytes but does not alone establish
transport integrity.

Before flashing, make and verify full backups, check the physical chip identity,
compare the partition table against the backup, and write/verify only the
application at `0x10000`. Existing storage, accounts and paired identities must
remain intact. See `RESULTS.md` for the actual qualification evidence and limits.

### Stored-file decoder probe

Prepared application copies include the experiment-only command
`camerajpegprobe "/path/to/test.jpg"`. It requires an authenticated serial admin
and preserves that caller's VFS permissions. The probe reads at most 128 KiB,
releases the filesystem lock, then calls `hwjpeg::validateSoftware` with a
640×480 geometry limit. It reports `valid`, dimensions, byte length, decoder
microseconds, error and whether the ROM decoder was compiled in; it does not
return image bytes or allocate a full RGB image. This permits testing the same
stored good/bad fixture without relying on a USB download to validate it.

`prepare.py` hashes the probe header and insertion logic and records both
injected files in the application overlay manifest. The command is absent from
the production source tree. Use only known test fixtures and remove uploaded
fixtures after testing; the command itself does not write or delete files.
