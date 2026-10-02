HardwareOne P4X-EYE investigation: experiment results (2026-09-27)

Repository branch: codex/investigate-esp32-p4x-eye
Repository HEAD: 6ba1c6c (no commits made)
Original source: unchanged from initial working tree, including its 16 pre-existing modified/deleted tracked paths.

Toolchain:
ESP-IDF v5.5.5, commit b774170ff46c393eeb5e495ea37936038d3f4f4f
Compilers: GCC 14.2.0, esp-14.2.0_20260121, Apple Silicon host
Python 3.12.14; CMake 4.4.3; Ninja 1.13.2
Identification tool: esptool 5.4.0 (separate tools-env)
SDK/toolchains and experiments: /private/tmp/hw1-p4-investigation-20260927
Activate in bash: source /private/tmp/hw1-p4-investigation-20260927/activate-idf.sh
These /private/tmp paths are disposable and are not guaranteed to survive OS cleanup.

Device queries:
P4 rev3.2, 16MB flash, USB Serial/JTAG; existing factory demo detects 32MB PSRAM and OV2710 camera.
XIAO S3 rev0.2, 8MB flash, 8MB embedded PSRAM; existing demo greeting observed.
No flash writes, erase commands, eFuse changes or newly built firmware execution occurred.
Chip/flash queries hard-reset the devices; no ROM RAM stub was uploaded (--no-stub).
No C6 firmware version or radio/mesh operation was verified.

Build controls (same upstream hello_world source):
PASS: ESP32-P4 compile and link; hello_world.bin 0x29810 bytes (170,000 bytes).
PASS: ESP32-S3 compile and link; hello_world.bin 0x27780 bytes.
P4 sdkconfig: CONFIG_IDF_TARGET_ARCH_RISCV=y; CONFIG_ESP_REV_MIN_FULL=301;
CONFIG_ESP32P4_SELECTS_REV_LESS_V3 is not set (new revision silicon).
Firmware images were not flashed; compiled controls prove host-toolchain availability only.

HardwareOne throwaway experiments:
Copied 5702 existing tracked files from current working tree, preserving user edits/deletions.
Restored Arduino 3.3.5 commit 11bc7ac1a458f1f4e7afc8fd4de1dfa710f31562 only in this copy.
PASS: git apply --check and application of tracked arduino-local-patches.patch.
PASS: XIAO/S3 reconfigure on IDF5.5.5 using existing dependency lock (ESP-SR1.9.5).
FAIL: XIAO/S3 full application build. Vendored Adafruit_GPS NMEA_build.cpp:76/78,
NMEA_parse.cpp:778 trigger -Werror=stringop-truncation. No library edits were made.
This is an SDK/toolchain-upgrade control result, not a test of the historically validated IDF5.5.1 pairing.
The current xiao_s3 board selects upstream XIAO_ESP32S3 variant, which exists;
the older missing custom Sense variant in documentation was not this build's blocker.
FAIL: P4 reconfigure before compilation. Fresh dependency solving rejects simultaneous
ESP-SR ^1.4.0 (HardwareOne) and ^2.1.5 (Arduino). Existing S3 lock reuse had masked this mismatch.
P4 probe deliberately has no invented production board wiring profile; the point was to
measure the first dependency blocker without implementing the port.

Experiment logistics:
An initial relative -B path placed generated S3 build output in the original checkout.
That generated directory was moved to the disposable directory; subsequent probes used
absolute paths. Original source/config files were not modified by that attempt.
The first S3 compilation attempt could not download the required LittleFS Python package
inside the network sandbox; the authorized install/build retry fetched it and reached the
actual source warning above. Neither issue is treated as a P4 compatibility finding.

Evidence files:
hello-p4-build.log, hello-s3-build.log: successful control builds
hw1-s3-configure.log: successful existing-lock configure
hw1-s3-build.log: first source/compiler failure
hw1-p4-configure.log: conflicting dependency constraints
device-observations.txt: selected USB and serial observations
repository-verification.txt: preservation check
