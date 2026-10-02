# Camera and storage qualification investigation

Date: 2026-09-28. The user confirms neither board currently has an SD card; physical SD read/write/recording tests are deferred until cards are available. Branch: `codex/jpeg-portable`. No production camera/storage code has been changed. Device access and flashing belong to the root agent.

## Findings

The P4X-EYE needs a camera backend, not a new application fork. Existing HardwareOne camera consumers already accept owned JPEG buffers through `captureFrame()` and `captureFrameAtResolution()`. The P4 camera is currently disabled in the tested full-app profile; the successful standalone tests below do not establish web/G2/recording integration.

The official P4-EYE factory configuration selects OV2710, and its factory log reports PID `0x2710`. The P4X board keeps the camera interface while moving to chip revision 3.1 or newer. Generic CSI examples that select SC2336 do not identify the attached module. The standalone probe enumerates SCCB addresses and reports the actual detected sensor PID. [P4X-EYE guide](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/index.html), [official factory example](https://github.com/espressif/esp-dev-kits/tree/master/examples/esp32-p4-eye/examples/factory_demo).

| Resource | P4X-EYE connection | Consequence |
| --- | --- | --- |
| Camera | MIPI-CSI; SCCB SDA 14 / SCL 13, XCLK 11 at 24 MHz, reset 26 | Use `esp_video` / `esp_cam_sensor`; DVP pin substitution cannot work. |
| Camera power | GPIO12, shared with LCD rail; CSI LDO3 at 2.5 V | Camera must work without display initialization; LCD sleep must not cut an active camera rail. |
| SD | Native SDMMC slot 0; D0 39, D1 40, D2 41, D3 42, CLK 43, CMD 44; detect 45, enable 46 active low; IO LDO4 | Independent pins and slot from the C6 radio. Explicit power and slot configuration required. |
| C6 Hosted radio | SDMMC slot 1; CMD 27, CLK 28, D0–D3 29–32; reset 9 | SD and networking can coexist, but share the SDMMC controller and need stress/recovery tests. |

Pin and power references: [official BSP header](https://github.com/espressif/esp-bsp/blob/master/bsp/esp32_p4_eye/include/bsp/esp32_p4_eye.h), [BSP implementation](https://github.com/espressif/esp-bsp/blob/master/bsp/esp32_p4_eye/src/esp32_p4_eye.c). Hosted pins are verified against the current local P4 profile.

The XIAO control uses DVP and the existing `esp32-camera` driver; its sensor can be OV2640, OV3660, or a replacement OV5640, so identify it at runtime too. Its microSD uses SPI: CS21, SCK7, MISO8, MOSI9. These are distinct from its camera/microphone pins. [Seeed documentation](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/), [Seeed camera + SD example](https://wiki.seeedstudio.com/xiao-esp32s3-freertos/).

## Shared-code changes needed after hardware qualification

1. Introduce a narrow `HAL_Camera` interface for start/stop, capture, supported sizes, controls and capabilities. Keep lifecycle locking, the asynchronous power worker, and the shared web/G2/mesh/recording consumers above it. Keep the existing DVP driver as one backend; add CSI/V4L2 as the other.
2. Stop exposing `esp_camera.h`, `framesize_t` and sensor-specific control callbacks through the shared header. Preserve persisted resolution IDs with an explicit mapping; report unsupported sensor sizes/controls instead of silently pretending success. Existing G2 and web resolution/control menus are fixed lists and should consume capabilities.
3. Return owned JPEG data with actual width/height and explicit lifetime. S3 normally receives sensor-compressed JPEG; P4 OV2710 provides RAW10 which the ISP converts before JPEG encoding. Use the P4 encoder behind the backend; do not decode/re-encode existing S3 JPEG unnecessarily.
   The standalone probe owns I2C controller 0; a full application backend must coordinate SCCB with the application’s existing I2C owner instead of blindly creating a second bus on the same controller. Camera/LCD power-rail ownership also needs an explicit shared lifetime.
4. Normalize quality semantics. Existing S3 quality is 0–63 with lower values better; P4 IDF JPEG quality is 1–100 with higher values better. Persisted values must not acquire reversed meaning after changing board.
5. Plan resizing/cropping explicitly. The pinned OV2710 driver offers 1280×720 and 1920×1080. Existing mesh tiny frames request 160×120, and G2 uses small preview images. Current JPEG decode defaults cap width/height at 1600, rejecting native 1920-wide capture. Capture size and delivered image size must be separate capabilities; a shared resize API with software fallback can bridge this.
6. Integrate a storage transport seam under the existing VFS. In this frozen qualification branch VFS directly calls Arduino `SD` and global `SPI`; even its format path hardcodes SPI2. P4 LCD also uses SPI2. Native SDMMC slot 0 avoids that collision, but every mount/unmount/stats/format/read/write path must use the selected backend rather than mixing `SD` and `SD_MMC` handles. The primary checkout already contains an in-progress candidate `HAL_SDCard.cpp/.h` and VFS integration, described below; review and reuse that work instead of implementing a competing abstraction.

Relevant code: `components/hardwareone/System_Camera_DVP.h/.cpp`, `System_Camera_Video.cpp`, `System_Camera_DVP_Web.h`, `G2_Page_CameraSettings.cpp`, `System_VFS.cpp`, and `experiments/p4_io/features.h`.

Apply a shared resolution/quality/output-byte budget before connecting the new camera backend to consumers. The lit P4 snapshot is 121,716 bytes: only 9,356 bytes below the existing 128 KiB (131,072-byte) G2 file-loader cap in `components/hardwareone/G2_Glasses.cpp`, `loadJpgAsBmp288x144()` (line 34701 in this branch). A more detailed or noisy scene could exceed the cap at the same resolution and quality. Capture success alone therefore cannot guarantee G2 acceptance. Resizing and bounded quality selection should respect each consumer’s geometry and byte limits; do not simply raise the global limit.

## SD + radio lifecycle audit

Checkout scope matters: the primary checkout at `~/Documents/Codex/Projects/hardwareone` has pre-existing, untracked `HAL_SDCard.cpp/.h`, `docs/P4X_EYE_PERIPHERALS.md` and `experiments/p4_peripherals`, alongside its VFS changes. Read-only inspection found a candidate `SdMmcCardFS` backend that preserves the VFS permission/routing path, chooses slot 0 explicitly, controls card power/LDO4, uses per-slot teardown, avoids formatting on ordinary mount, and handles stats, explicit format and removal state. It is absent from this frozen qualification branch. Reuse/review that candidate during integration; this audit is not a replacement implementation or physical validation of it. The primary files were neither edited nor merged here.

The installed IDF 5.5.5 `sdmmc_host_init()` is idempotent. Its `SDMMC_HOST_DEFAULT()` selects slot-aware `sdmmc_host_deinit_slot`, and FatFs dispatches that callback when unmounting. Installed Hosted 2.12.13 also uses slot-aware deinit on IDF >=5.4. This removes the obvious whole-controller teardown conflict; physical coexistence and fairness are still untested. Retain both default host flags and the slot-specific deinit callback in any storage adapter. Never use unconditional `sdmmc_host_deinit()` while the other slot is active.

Validation should cover mounting/unmounting SD while mesh/BLE run; sustained hashed writes + readback concurrent with mesh file traffic; stopping/restarting Hosted while an SD file remains open; full-card/removed-card handling; and recording finalization after a capture/write failure. Do not format a user card as a test. Qualification files should have unique names and be removed only after verified readback.

## Standalone headless probe

Reviewable source lives in `camera_probe/`, with generated projects and artifacts under ignored `private/`; no board access is performed by the build. It initializes no NVS, filesystem, SD, LCD or radio. Flash only the app at `0x10000` after backups; preserve the installed bootloader/partition table/settings. The standalone project's partition table is not the HardwareOne partition table and must not be flashed.

- IDF 5.5.5; P4 min revision 3.1, 400 MHz, HEX PSRAM at 200 MHz.
- Registry `esp_video` pinned 2.2.0; resolved sensor 2.2.0, IPA 2.1.0, SCCB 0.0.9, H.264 dependency 1.3.0 (H.264 video device disabled), CMake utilities 0.5.3. The dependency lock includes content hashes.
- OV2710 + SC2336 auto-detection enabled, with the detected PID reported. OV2710 defaults to 1280×720 RAW10 at 25 fps. The ISP produces RGB565.
- Captures 90 frames using three driver buffers; two-second dequeue timeout; records byte lengths, last-frame CRCs and elapsed frame rate; JPEG-encodes a snapshot at quality 80 and emits length/CRC/hex chunks for independent host decode.
- Uses the existing qualified JPEG driver override after `prepare_idf_jpeg.py`; no global SDK modifications.
- The new `esp_video` 2.5.0 master source was inspected but not used: its component list refers to `esp_hal_cam`, which is not present in the installed IDF. Pinning a compatible tested release avoids bringing in an SDK upgrade solely for camera work. Version 2.2.0 includes the P4 revision >=3 AWB subwindow fix.

P4 build passed. Frozen image: `private/camera-artifacts/hw1_camera_probe.bin`, 543,056 bytes, SHA-256 `157e2db60dc458cb3f0e2d02518baca30c5da24e37589a60f3cf175e46dc6587`. ELF/config/source hashes are in `private/camera-artifacts/qualification.json`. Final ELF contains no NVS set/commit/erase, flash/partition write/erase, SD initialization or Wi-Fi initialization symbols. `camera_probe/extract_camera_jpeg.py` checks ordered chunk offsets, declared length, CRC32, JPEG decode and image statistics; images remain private.

The S3 control probe built successfully under `private/camera-control-s3/`, using the existing esp32-camera 2.1.4 dependency. It starts/stops capture separately at QVGA and VGA (35 frames each) and emits the final VGA JPEG over USB. Its frozen app is `private/camera-artifacts/hw1_camera_s3_probe.bin`, 284,672 bytes, SHA-256 `4a00f133931acc3de6e16018780c46e7182431761a1777ffe308f3163384bfa8`; ELF and metadata sit beside it. It also has no persistent-write or radio/SD-init symbols. These camera pipeline timings are not a CPU comparison: sensors, output dimensions, JPEG quality scales and compression paths differ.

Physical results follow. A valid JPEG decode and visual inspection are required in addition to transport CRC and frame counts; a changing CRC alone does not prove a good image.


## Physical camera checks — 2026-09-28

S3 standalone control passed with actual sensor PID `0x3660` (OV3660): 35/35 QVGA frames, 35/35 VGA frames after stop/reinitialization, no reported frame errors, and intact heap after teardown. Observed capture rates were 25.38 and 24.71 fps respectively. The final 640×480 JPEG is 16,873 bytes; USB chunk order, length and CRC32 `81899112` all passed. Independent Pillow decode succeeded; visual inspection showed a coherent room scene without gross tearing or mosaic artefacts. This is not controlled colour/focus calibration. Captured images/logs remain private.

The first P4 probe identified PID `0x2710` (OV2710) and its 1280×720 RAW10 mode, then correctly stopped before capture because the test expected nonzero `sizeimage`. Investigation showed that esp_video 2.2.0 leaves optional `bytesperline`/`sizeimage` fields zero while its allocator calculates packed-image capacity itself. This was a probe assumption, not a demonstrated camera failure. The original failure log and binary are preserved.

Probe revision 2 explicitly supplies the checked packed RGB565 layout before setting the format and retains independent buffer-capacity and per-frame byte-count checks. New image `private/camera-artifacts/hw1_camera_probe_v2.bin` is 543,168 bytes, SHA-256 `2e50e754994a58df866bb7a7596829f58de04305fd18b1b44925c62a0e587dc5`. Physical retest passed: sensor PID `0x2710`, 90/90 1280×720 frames, zero frame errors, observed 24.24 fps, intact heap after teardown. Hardware JPEG encoding took 10,099 microseconds for the captured snapshot (quality 80, 4:2:0) and produced 86,879 bytes. Ordered USB chunks, length, CRC32 `713e1dbb`, and independent Pillow RGB decode all passed.

The first v2 frame was almost black and featureless (RGB means approximately 9.6/14.2/8.4; maximum channel value 62), so it did not establish a useful scene image. That capture and its evidence remain preserved.

The user confirmed the lens was uncovered and aimed it at a lit scene. Repeating the **same v2 binary, without software or tuning changes**, again passed 90/90 frames with zero errors at 24.24 fps and intact heap. Hardware JPEG encoding took 10,177 microseconds and produced 121,716 bytes. All 951 USB chunks, length, CRC32 `de87ab4a`, SHA-256 `876d7bd3eda3c13643f3076134e822f337fa53f91b3596c24e6f1ef5a749351b`, and independent Pillow RGB 1280×720 decode passed. Visual inspection now shows a continuous lit scene and an object/fixture near the top, without gross tearing or mosaic artefacts. The black-frame condition is resolved without software changes. The image is noticeably soft/noisy; controlled focus, colour and exposure qualification remains outstanding. This establishes a coherent standalone capture, not calibrated image quality or full-app integration.

Reproducible source/configuration, pinned dependency locks, build-only scripts, app-only flash/restoration instructions and the strict extraction helper now live in [camera_probe](camera_probe/README.md). Both promoted projects build successfully with IDF 5.5.5, with unchanged dependency locks. Seven host integrity checks pass. The promoted S3 probe source is byte-identical to the source of the physically tested S3 binary. The fresh build has a different binary hash due to build metadata/path differences, and does not overwrite the original frozen image.


### Dark-frame source audit

No missing ISP/automatic-control start was found in the probe. Pinned esp_video's `create_csi_video_device()` calls `esp_video_isp_pipeline_init()` when the detected sensor has ISP metadata and a registered IPA configuration. The compiled ELF includes OV2710's IPA configuration, gamma table and AE-target table; Kconfig enables ISP pipeline control, sensor statistics updates and IAN/AWB/AGC/AEN/ATC algorithms. The official capture example also relies on `esp_video_init()` for this initialization. Sensor power, XCLK, reset and SCCB match the BSP sequence.

The [factory OV2710 custom JSON](https://github.com/espressif/esp-dev-kits/blob/master/examples/esp32-p4-eye/examples/factory_demo/main/ov2710_custom.json) differs from the pinned driver's default in colour-temperature adaptation and older environment/AE-target tuning, but uses the same initial AE target (48) and gamma curve table. This does not identify a software cause for the nearly black frame. The subsequent unchanged-v2 retest with the lens uncovered and aimed at a lit scene resolved the black-frame condition, as recorded above. No ISP tuning changes were necessary for that result; focus and colour quality still need a controlled scene.
