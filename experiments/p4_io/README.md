# P4X-EYE LCD, wheel and three buttons

This independent experiment combines the qualified
[BLE-role snapshot](../p4_ble_roles/README.md) with the optional peripheral
drivers captured in this experiment's overlay. Their original input hashes
are bundled in [the provenance manifest](provenance/peripheral-source-manifest.json).
It retains G2/R1,
separate BLE Server mode, HTTP, encrypted ESP-NOW and the common SDK ECC fix.
Only the P4X-EYE SPI LCD, rotary wheel and three user buttons are enabled here;
both SD/MMC and the generic card feature are explicitly disabled. Camera and
microphone remain disabled. Earlier experiments and the main source tree are
not modified by this workflow.

The wheel turns map to previous/next, a short press to select, a 700 ms hold
to back, and a double press to the function action. The three user buttons
on GPIO 3/4/5 map to the existing X/Y/START actions. BOOT and RESET keep their
hardware roles. Existing 128×64 monochrome screens are drawn at 192×96 in
the center of the 240×240 panel.

## Reconstruct and build

Keep the committed BLE-role experiment and its baseline dependencies in place,
then reconstruct the verified `p4_ble_roles/private/app` baseline. The original
`p4_peripherals` directory is not needed for checking, rebuilding or
reconstructing this experiment. From the repository root:

```sh
python3 -B experiments/p4_io/prepare.py
python3 -B experiments/p4_io/prepare.py --check
# Activate the pinned ESP-IDF 5.5.5 environment, then:
bash experiments/p4_io/build-p4.sh
```

Preparation verifies the baseline and build-input hashes, copies only recorded
sources, applies `app-overlay.patch` with zero fuzz and verifies the output
before publishing `private/app`. It refuses an existing destination. Use
`--destination /absolute/new/directory` for an independent reconstruction.
Generated components, credentials and build caches are not part of the source
snapshot. `--refresh` is a maintainer-only capture of an intentionally frozen
merge; it records the overlay and manifest without modifying the source copy.
The bundled peripheral provenance is hash-pinned alongside the overlay; the
overlay itself contains every incoming driver and requires no external merge.

The build wrapper uses the BLE-role SDK defaults and prepares the same verified
IDF 5.5.5 Bluetooth backport into this experiment's
`private/idf-components/bt`. It leaves the shared SDK and earlier experiment
outputs untouched. See the [SDK procedure](../p4_ble_roles/SDK-FIX.md).
Arduino 3.3.5 and Hosted 2.12.13 remain inherited from the baseline. Building
does not open serial ports or flash firmware.

`features.h` is a flattened copy of the pinned BLE-role feature profile with
the explicit LCD/input/card overrides. The existing CMake source-selection
logic reads literal defines and does not follow header includes, so an
include-only profile would not reliably select matching sources. The manifest
pins the inherited feature header as provenance. Two forwarding radio-backend
files reuse the qualified implementation; their own and inherited hashes are
also recorded.

## Verification scope

The first hardware run found that the shared login form discarded wheel-only
navigation because it checked only buttons and joystick deflection. This
overlay keeps the existing rotary up/down events through that guard for both
GPIO and ANO wheels. It also reports the actual display mode after an auth
redirect and supplies missing status labels from the existing mode-name table.
Authentication remains required. Run the focused regression with:

```sh
python3 -B experiments/p4_io/tests/test_login_wheel.py -v
```

The test compiles the actual login handler with keyboard/auth stubs, covers
field navigation and existing authentication behavior, and proves the old
guard fails both wheel directions. Runtime results are recorded separately
in [RESULTS.md](RESULTS.md).

The merge preserves the qualified Bluetooth changes and adds peripheral
display/input paths. A successful reconstruction or build does not qualify the
physical LCD orientation, brightness, wheel direction, button actions, sleep
behavior or simultaneous radio use. Those require separate device evidence.
This profile does not exercise or change a card's contents.

Private source copies, firmware, logs and credentials stay under ignored
`private/`; never publish them. No hardware test result is claimed by this
workflow document.
