# Shared battery integration

This change connects the ESP32-P4X-EYE's BAT_ADC divider to HardwareOne's
existing battery subsystem. ESP32/S3 ADC boards and the MAX17048 backend use
the same state, commands, web page, history and display consumers. The stock
XIAO ESP32-S3 Sense has no onboard divider and continues to report measurement
unavailable when queried through the shared API; its disabled build profile
omits the battery CLI and page.

Implementation is in `components/hardwareone/System_Battery.{h,cpp}`,
`BatteryPolicy.h` and the board settings in `System_BuildConfig.h`. ADC unit and
channel come from IDF's GPIO mapping. Calibration uses SDK capability macros:
curve fitting on P4/S3 and line fitting on classic ESP32. This is verified with
ESP-IDF 5.5.5; no IDF 6 migration is needed. A generic P4 build does not inherit EYE wiring;
`HW_BOARD_P4X_EYE=1` selects GPIO49, the nominal 1332/332 divider and 6 dB
attenuation. Existing Feather wiring remains unchanged.

## What the reading means

For the EYE V2.4 reference design, R15 is 1 Mohm and R16 is 332 kohm:

```text
BAT terminal volts = calibrated ADC volts × (1000 + 332) / 332
```

The implementation averages 16 calibrated conversions after discarding a
settling conversion. Hardware initialization, reads and recalibration share a
mutex; consumers obtain a coherent snapshot without holding that hardware
lock. A failed read invalidates the current reading and never advances its
last-success timestamp. Cached measurements expire after 30 seconds; normal
sampling runs on the existing 10-second main-loop tick.

The stock EYE cannot tell software whether a cell is installed, whether it is
charging, or whether USB power is present. Its charger status signals drive
LEDs and no CPU-readable VBUS detector was found. A charger can drive BAT even
without a cell. Consequently:

- `present`, `charging` and `usbPresent` are `null`, with their `*Known` flags
  false. A high voltage never creates a USB or charging claim.
- `voltage` is a BAT-node measurement. `voltageValid` means a successful recent
  read, not proof of an attached cell or meter-confirmed absolute accuracy.
- The optional percentage is a rough single-cell LiPo voltage estimate,
  identified by `percentageEstimated` and `percentageSource: "voltage"`.
  Charge, load, temperature and cell chemistry affect it. A plausible charger
  voltage without a cell can also produce this estimate.
- Status labels such as `Full (estimated)` describe the estimated voltage
  band. They do not confirm charge termination or a full physical battery.
- Calibration failure, ADC clipping, read errors and stale samples are
  unavailable, rather than fabricated zero/full/USB values.

MAX17048 boards retain their gauge readings. Their SOC is identified as a
fuel-gauge estimate, positive/negative CRATE informs an explicitly estimated
charging state, and a routed VBUS GPIO remains an independent input. The old
voltage-based USB inference and forced 100% SOC clamp are removed. Unsupported
or disabled backends no longer invent a 5 V / 100% reading.

The nominal EYE divider population, settling behavior and absolute accuracy
still need checking with a known attached cell and a multimeter. Implementation
and compile checks alone do not settle those questions. Physical evidence and
its limits belong in [RESULTS.md](RESULTS.md).

## Use on the device

After signing in with an existing device account:

```text
batterystatus
batterystatus json
batterylog status
batterylog tail
```

`batterycalibrate` is an admin command. It safely reloads the ADC calibration
scheme and refreshes a reading, or re-probes a MAX17048. It does not measure
resistor tolerance or calibrate against an external meter. On a USB serial
session that ignores Return, submit with LF, such as Ctrl+J in `screen`.

With the battery web feature enabled, visit `/battery`; the authenticated
`/api/battery/status` endpoint has the same JSON schema as the CLI. Clients must
honor validity/known flags and nullable values. `lastReadMsAgo` reports age of
the last successful sample; `lastError` carries the latest driver error.

The web page, OLED/G2 widgets and notifications show estimated percentages with
`~` and show unavailable information as unknown. CSV history preserves the
existing 11-column layout; unknown numeric/charging/USB fields are blank, and
status text identifies estimates. Historical rows are not rewritten. The OTA
power gate uses fresh measured voltage for ADC boards or confirmed VBUS; it
never obtains permission from a guessed USB state or stale value.

## Build workflow and inherited baseline

This work remains isolated from the user's primary checkout. The production
battery changes are captured in `integration.patch` and
`integration-inputs.json`, relative to commit `a95a9aa`, and applied over the
previously qualified private JPEG application copies. Those copies already
contain the P4 I/O and native-S3/Hosted-P4 radio integration. The wrapper
`radio_backend.{h,cpp}` reuses that radio implementation; this change does not
introduce another radio backend or alter C6 firmware.

The P4 profile retains the previous LCD/wheel/buttons, BLE client/server, web
and mesh configuration and enables battery monitoring and its web page. The
S3 profile retains its radio test configuration with battery monitoring off.
Both battery profiles keep camera, microphone and SD features off. Display
support being compiled does not imply that a physical display is attached.

Preparation requires the prior verified private snapshots; follow the
[JPEG preparation instructions](../jpeg_portable/README.md) first if they are
missing. A source-only clone does not contain those local build artifacts.
With the verified JPEG copies and pinned SDK components already prepared:

```sh
. /private/tmp/hw1-p4-investigation-20260927/activate-idf.sh
python3 -B experiments/battery_portable/prepare.py --target p4
python3 -B experiments/battery_portable/prepare.py --target s3
bash experiments/battery_portable/build.sh p4
bash experiments/battery_portable/build.sh s3
```

For an existing battery copy, use `prepare.py --target p4 --check` (or `s3`).
Preparation refuses to overwrite an existing destination. After an intentional
source edit, `--capture` regenerates the patch and source hashes;
`--target p4 --refresh` updates only a previously verified battery copy. Inspect
that captured diff before refreshing, and never refresh an active build.
These development operations are unnecessary when reproducing the committed
inputs.

Build preparation verifies the baseline manifest, each source hash, the patch,
feature profile and radio wrappers. Patch application requires exact context
with zero fuzz. Generated apps, dependencies, binaries and private logs stay
under ignored `private/`; the qualified JPEG originals and installed SDK are
not changed. `prepare.py` and `build.sh` do not open serial ports or flash.

## Verification

Run platform-independent policy, backend fault/concurrency, JSON and OTA gate
checks against the actual source with:

```sh
python3 components/hardwareone/test/host/test_battery.py --sanitize
```

These host tests mock hardware/SDK boundaries; they do not establish analog
accuracy. They exercise unavailable/invalid/stale readings, board selection,
calibration and read failures, serialized recalibration, notification
hysteresis, MAX17048 behavior, JSON nulls and OTA admission.

[SDK_ADC.md](SDK_ADC.md) and [sdk-adc-results.json](sdk-adc-results.json) record
successful real IDF 5.5.5 compilation/linking of the actual ADC backend for
classic ESP32 and S3. The classic build covers line fitting; S3 covers curve
fitting. These small fixtures were not flashed. Full application builds,
device testing, restoration/deployment state and outstanding physical checks
are recorded separately in [RESULTS.md](RESULTS.md).

`test_hardware.py` is an explicit board-only test harness with required port,
MAC and private credential-file arguments. It opens both serial ports, checks
P4 telemetry/recalibration/clock changes and preserved mesh traffic, and can
optionally use the S3 to query the P4's temporary HTTP fixture. It never flashes
or provisions accounts. The optional HTTP run changes runtime radio state and
requires a reboot afterward. Only the test coordinator should own the ports;
use `--help` and the recorded physical procedure rather than guessing ports.

Hardware evidence and driver references are in the earlier
[battery investigation](../board_qualification/BATTERY.md), including the
local V2.4 schematic and pinned IDF driver documentation.
