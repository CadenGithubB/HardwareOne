# P4X-EYE display, SD card and rotary input

This build integrates the optional peripherals into the actual HardwareOne
application, including the three user buttons, on the verified `p4_connectivity` platform/radio snapshot. Shared
drivers and UI/VFS integration live in the main `components/hardwareone` tree.
The private build combines them with the earlier P4 port without modifying
previous experiments. Camera and microphone remain disabled.

See [hardware, configuration and controls](../../docs/P4X_EYE_PERIPHERALS.md)
and [build/offline validation results](RESULTS.md).
This implementation has not been flashed or exercised on the physical board.
Live screen orientation, card I/O, wheel direction and simultaneous radio use
still require hardware qualification.

## Reconstruct and build

First prepare the exact [connectivity baseline](../p4_connectivity/BUILD.md).
This includes Arduino 3.3.5 and the Hosted Bluetooth/ESP-NOW patches. ESP-IDF
5.5.5 is the tested SDK; the companion baseline is ESP-Hosted 2.12.13.

```sh
python3 experiments/p4_peripherals/prepare.py
# Activate the pinned ESP-IDF 5.5.5 toolchain, then:
bash experiments/p4_peripherals/build-p4.sh
bash experiments/p4_peripherals/build-p4.sh --peripherals-off
```

The second build checks the same P4 application with all three peripherals
disabled. Both retain the existing HTTP, Bluetooth and ESP-NOW profile. These
scripts only build; they do not access serial ports, flash chips or touch
cards. Each profile has its own build directory and SDK configuration.

Preparation verifies the previous manifest and all baseline source hashes,
copies only recorded source files, applies `app-overlay.patch` with zero fuzz,
then verifies the resulting hashes before publishing the new directory. It
refuses existing destinations. Private credentials, generated components and
build caches are not copied.

```sh
python3 experiments/p4_peripherals/prepare.py --check
# Independently reconstruct without touching the built source:
python3 experiments/p4_peripherals/prepare.py --destination /absolute/new/directory
```

`source-manifest.json` records the exact baseline manifest, changed source
hashes, patch and build-input hashes. Maintainers can capture an intentionally
updated private source with `prepare.py --refresh`; this changes only the
durable overlay and manifest. Verify a fresh reconstruction after refreshing.
The overlay is relative to the connectivity copy, not to a clean Git checkout.

## Offline checks

```sh
python3 components/hardwareone/test/host/test_optional_peripheral_config.py
python3 components/hardwareone/test/host/test_rotary_core.py
python3 components/hardwareone/test/host/test_gpio_buttons.py
python3 components/hardwareone/test/host/test_gpio_encoder_integration.py
python3 components/hardwareone/test/host/test_sdmmc.py --sanitize
cmake -S components/hardwareone/test/host -B /tmp/hw1-peripheral-tests -DHW1_SANITIZE=ON
cmake --build /tmp/hw1-peripheral-tests --target p4eye_display_pixels_tests p4eye_display_lifecycle_tests vfs_capacity_cache_tests
ctest --test-dir /tmp/hw1-peripheral-tests --output-on-failure -R 'p4eye_display|rotary_core|gpio_buttons|gpio_encoder_integration|optional_peripheral|sdmmc|vfs_capacity'
```

These checks compile real driver or dependency-free production code and inject
hardware failures where needed. They supplement the real P4 firmware builds;
they do not establish electrical behavior on a board.

## Hardware acceptance still to perform

Use an authorized test image and a backed-up disposable FAT card. Verify no-card
boot, mount/list/read/write/read-back, full-card and unrecognized-filesystem
failures, removal/remount, and repeated unmount while Wi-Fi/BLE/mesh remain
active. Formatting is a separate explicit destructive test. Stop writers and
close file handles before unmounting.

Check the LCD's orientation, brightness, blank/wake and repeated stop/start.
Exercise slow/fast turns, contact bounce, click, hold, double-click and
press-and-turn. Check all three user keys (X, Y, START), held-at-start suppression,
simultaneous key/wheel use and repeated close/open. Complete first-time setup, login, text entry and menu navigation
using the wheel, including keyboard mode changes. Verify card activity and
display transfers do not disrupt the radio companion.
