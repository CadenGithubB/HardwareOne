# JPEG build qualification — 2026-09-28

All four final build commands exited **0** after the final marker-padding,
allocator and G2 cleanup changes. No serial port was opened and no firmware was
flashed. Hardware timing, visual comparison and end-to-end G2 display checks
remain pending.

| Build | Application bytes | SHA-256 |
|---|---:|---|
| p4 | 4,412,832 | `9bd7f4446cee55e9a4236dbba24f4ef3e8645480503cb18a7f1d410a911995ce` |
| s3 | 4,986,240 | `b2d3c9e56bc188a074bfdca431aaedcc8c52e28e80a79a731799142c352ec7a2` |
| codec-p4 | 518,880 | `629d2552dc4e23271aa5632e4c453a49bb55fc09c65c67719cffc3ca2c7d5227` |
| codec-s3 | 455,584 | `18e04f3f6c5a8090f39f3991164fb04eff7058e49739128dc684a452e9f1fe3e` |

The full P4 application retains the qualified LCD/wheel/buttons profile and its
G2/R1, web and mesh components. The S3 application enables the camera in addition
to the BLE-role profile, exercising both live-camera decode call sites at compile
and link time.

Post-build validation confirmed:

- 7,934 P4 and 7,924 S3 source files still match their frozen baseline and final codec overlay.
- The S3 ELF contains `g2CameraViewerWorker`, `g2CameraStreamWorker` and the shared `hwjpeg::decode`; it contains no `jpeg_decoder_process` symbol.
- The P4 ELF contains `jpeg_decoder_process`. Both P4 builds use the verified private JPEG component and `HW1_JPEG_DRIVER_QUALIFIED=1`; neither S3 codec compilation has that define or hardware driver include path.
- The isolated JPEG component preparation check passes. The installed ESP-IDF component is unchanged.
- Both probes embed the original 13-image corpus plus runtime malformed, valid marker-padding, output-limit and concurrent-decode checks. Successful compilation is not a device test result.

Host validation also passed under AddressSanitizer and UndefinedBehaviorSanitizer:
metadata/resource limits, backend selection/fallback, marker normalization,
custom allocator policy, G2 PSRAM bypass, failure cleanup, RGB ordering and padded
output stride. Actual TJpgDec output matches the legacy converter byte-for-byte
for all **12 decodable fixtures**, including **192 concurrent decodes**. The
post-test manifest contains **44 matching source hashes**.

The actual SDK allocation/IRQ lifetime functions pass **10 host test methods**;
the five original-SDK negative-control scenarios fail as intended, establishing
that the checks exercise the repaired failure paths. Raw host logs and their
source manifest remain private alongside the build logs.

The SDK is ESP-IDF 5.5.5 with the experiment’s private Bluetooth and JPEG component
overrides; the portable decoder is esp_jpeg 1.3.1. Full source and firmware
fingerprints are recorded in `qualification.json`. The scripts in `README.md`
reproduce the preparation and builds; build logs and images are in `private/`.

The source change adds JPEG **decoding** acceleration. Existing JPEG encoding,
camera passthrough and Edge Impulse conversion remain unchanged. The ordinary
unqualified P4 SDK build retains software decoding.

Before enabling a broader release, run both probes on their corresponding boards,
inspect the hardware/software pixel differences and timings, then restore the
full application and test stored JPEG display and S3 live camera viewing. The
probe output must end with `JPEG_RESULT failures=0`; any positive timing claim
requires those measured device results.
