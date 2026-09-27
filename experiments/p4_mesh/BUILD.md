# Shared HardwareOne mesh build experiment

Both ESP32-S3 and ESP32-P4 builds pass with one copied application and one
`features.h`. The original application files are untouched. The source baseline
is commit `56f3a7e07e04e6e7d6349836c39b18dae2e45075` **plus the user's existing
working-tree edits and deletions**. `overlay-baseline.json` records the original
hashes of files touched by the overlay. A clean checkout of that commit alone is
not the complete baseline.

## Reproduce

1. Create `private/app` from the baseline's tracked working-tree files, excluding
   `experiments/`. Preserve modified contents and skip deleted files; do not use
   `git archive`, which would discard the user's edits. Keep the durable
   `experiments/p4_mesh/` and sibling `experiments/p4_espnow/bridge/` directories
   beside the copy as arranged here. Build directories must start with fresh
   per-target sdkconfig files.
2. Populate the copy's `components/arduino` with Arduino-ESP32 **3.3.5**, commit
   `11bc7ac1a458f1f4e7afc8fd4de1dfa710f31562`. Apply the repository's
   `docs/arduino-local-patches/arduino-local-patches.patch` inside that Arduino
   directory. An existing copy of that exact already-patched tree is equivalent.
3. Apply `app-overlay.patch` and then `arduino-overlay.patch` with the copied
   application as the patch root. For example, with absolute `app` and
   `experiment` shell variables:

   ```bash
   patch -F0 -p1 -d "$app" < "$experiment/app-overlay.patch"
   patch -F0 -p1 -d "$app" < "$experiment/arduino-overlay.patch"
   ```

   Both cumulative patches were applied to separate baseline copies with zero
   fuzz, and their outputs were compared byte-for-byte with the built source.
4. Activate ESP-IDF **5.5.5**, commit
   `b774170ff46c393eeb5e495ea37936038d3f4f4f`, with its matching tools. This run used
   GCC 14.2.0 (`esp-14.2.0_20260121`), Python 3.12.14, CMake 4.4.3 and Ninja 1.13.2.
5. From the repository root, run these build-only scripts sequentially:

   ```bash
   bash experiments/p4_mesh/build-s3.sh
   bash experiments/p4_mesh/build-p4.sh
   ```

   Each script sets its board, target, private build/sdkconfig path, shared
   feature header and shared SDK overlay. Component locks are included in the
   cumulative patch and also saved as `dependencies.s3.lock` and
   `dependencies.p4.lock`. Build scripts do not flash devices or provision
   credentials. ESP-IDF's component manager requires process inspection and
   registry access, which the restricted macOS sandbox may block.

## Scope and fixes

The profile retains HardwareOne's serial CLI, setup, settings, LittleFS, Wi-Fi,
ESP-NOW mesh, identity and session cryptography. HTTP, Bluetooth, I2C sensors,
display, battery, camera, microphone, speech/ML, automation, CM5 and bonded mode
are disabled. Production OTA layout, secure boot, flash encryption and NVS
encryption are disabled explicitly.

The private build transports its profile through an IDF build property so the
early dependency pass and compiler use the same values. Peripheral library
compilation is pruned to header-only ArduinoJson; inactive camera, speech and
TFLite manifests are removed. Required libraries remain pinned. This avoids the
fresh-resolution ESP-SR 1.x/2.x conflict and the unrelated Adafruit GPS compile
failure without globally suppressing warnings.

Copy-only source fixes guard unused NeoPixel/OLED code, replace two intentionally
truncated strings with bounded `strlcpy`, and type raw ESP-NOW state storage as
`void*` until its existing placement construction. P4 adds its correct IDF image
chip ID, a minimal board description, and the radio backend immediately after
Arduino initialization. The backend configures EYE SDIO pins and uses Arduino's
Hosted lifecycle helper before normal application Wi-Fi startup. The shared
mesh wire protocol is unchanged. Arduino receives one P4 UART clock-enum
compatibility fix for IDF 5.5.5.

## Build evidence and remaining qualification

The final S3 image is `private/build-s3/hardwareone-idf.bin`, size `0x2131a0`
bytes; the final log is `private/build-s3-mac-cache.log`. P4 is
`private/build-p4/hardwareone-idf.bin`, size `0x1c78e0` bytes; its log is
`private/build-p4-mac-cache.log`. Their SHA-256 hashes are:

- S3: `e91e099196599b46ae4ae6f3386fc2dfed93fe1c3db71498a5328ceb30675087`
- P4: `8eece05480b510514d5675d51855a9ed26f6ac0b5fc773169b2319248df085b1`

The first S3 build used for initial provisioning is
preserved in `private/s3-first-pass/`. P4's ELF map resolves native `esp_now_*`
entry points to the hosted adapter. Flash layouts differ: consult each build's
`flash_args` rather than borrowing offsets across chips. Blank LittleFS images
are separate build targets and are not included in ordinary `flash_args`.

These results prove compilation, not complete platform support. Device/mesh
results are recorded separately by the hardware test owner. Peripheral support,
power-mode frequency policy, companion recovery/update handling, IDF 6 migration
and production OTA qualification remain outside this minimal build milestone.

The follow-up RX cleanup retains each callback destination MAC and removes an
idle-loop remote MAC lookup; it also restores noisy Hosted RPC logs to WARN.
The shared `isSelfMac` helper now uses a checked STA-MAC cache during an active
radio instance, avoiding repeated RPCs during routing and receive processing.
The cumulative application patch includes all 17 changed paths and was applied
with zero fuzz to a separate baseline copy, then compared byte-for-byte.
Temporary pairing diagnostics are excluded from both final images and overlays;
`private/hardwareone-s3-pair-diagnostics.bin` retains that separate test artifact.
See [PORTING.md](PORTING.md) for pairing/session findings and remaining seams.

Run `python3 experiments/p4_mesh/test_mac_cache.py` from the repository root for
offline cache verification. It compiles the actual cache extracted from the
copied application with host driver/mutex substitutes, checks failed queries,
zero-RPC runtime reads, invalidation/reinitialization and concurrent publication,
and checks lifecycle ordering in the source. It also tests that a delayed
fallback cannot overwrite a newer cached identity. Missing private sources or
a host C++17 compiler produce a unittest skip. These checks supplement the
firmware builds and hardware tests; they do not exercise FreeRTOS on a device.
