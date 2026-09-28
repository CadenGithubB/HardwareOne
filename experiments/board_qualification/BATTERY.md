# P4X-EYE battery investigation - 2026-09-28

Read-only hardware/source investigation after board qualification. No battery
probe was flashed, no live cell voltage was measured, and production sources
remain unchanged. Whether a battery is attached to the Mac mini's EYE is not
confirmed at the time of this note.

## Hardware path

The ESP32-P4X-EYE-MB V2.4 reference schematic (2026-02-02, sheets 2 and 5)
connects the BAT_ADC net to P4 GPIO49. IDF 5.5.5's
`components/soc/esp32p4/include/soc/adc_channel.h` maps that pin to ADC2 channel 0.
This is a P4-local measurement; it needs neither C6 RPC nor an I2C fuel gauge.

R15 = 1 Mohm and R16 = 332 kohm in that schematic. The nominal conversion is:

`cell-terminal volts = calibrated ADC volts * (1000000 + 332000) / 332000`

The multiplier is approximately 4.01205; a 4.20 V cell puts approximately
1.047 V on the ADC. The older charging diagram in the online guide shows
301 kohm / 100 kohm instead (4.01). Both are approximately 4:1, but use the
actual PCB revision's values rather than copying the older diagram blindly.
The connected PCB's resistor population has not been physically confirmed.

The AP5056 charger is autonomous. BAT_CHRG and BAT_STBY connect to the red and
green LEDs on sheet 5; neither is routed to a P4/C6 GPIO in this schematic.
No separate CPU-readable VBUS-presence net or battery-current monitor was found.
Therefore the stock wiring provides battery-terminal voltage, not authoritative
charging state, USB supply state, charge current, remaining capacity or runtime.
The official guide describes red as charging and green as charging complete.
A charger can also drive the BAT node without a connected cell; a plausible
voltage alone is insufficient to prove battery presence.

The high-value divider, resistor tolerance and ADC acquisition behavior warrant
averaging/settling checks and a comparison with a multimeter on a known attached
cell. C172 at the ADC input is marked not populated in the schematic. Do not
turn calibration failure or an unloaded-charger voltage into a precise SOC.

## Existing HardwareOne integration

`System_Battery.cpp` already selects ADC, MAX17048 or disabled backends behind
one `BatteryState` and common accessors. Consumers include `batterystatus json`,
`/api/battery/status`, `/battery`, battery CSV history, G2/OLED widgets,
notifications, power policy and OTA admission. Reuse this boundary.

The restored P4 IO build explicitly has `ENABLE_BATTERY_MONITOR=0`; there is no
measured battery readout in that firmware. The `voltage` command estimates power
draw and does not read this divider. A disabled backend seeds 5 V / 100% / USB
assumed internally, which must not be presented as a cell measurement.

Simply enabling the existing ADC backend is insufficient:

- `System_Battery.h:11-17` hardcodes GPIO35, ADC1 channel7 and divider2 instead
  of using board configuration.
- `System_Battery.cpp:30-32,107-181` uses legacy ADC/calibration APIs. IDF 5.5.5
  does not expose the legacy calibration API for P4. Legacy and new ADC drivers
  must not be linked together; IDF intentionally rejects that combination.
- The ADC sampling path never populates `percentage`, leaving the initialized
  zero even though widgets, events and policies consume it.
- Voltage/rise heuristics infer charging and USB. A charged disconnected cell
  may remain above 4.15 V; that is not proof of USB power. The current public
  booleans do not distinguish unknown from measured false.
- No-cell detection by voltage threshold alone is not valid on an active charger.
  OTA and power policy must not interpret guessed USB presence as authoritative.

## Portable implementation direction

1. Modernize the existing ADC backend with IDF `adc_oneshot` and `adc_cali`.
   Derive ADC unit/channel from the configured GPIO. Select curve-fitting or
   line-fitting calibration through SDK capability macros. P4/S3 use the former;
   classic ESP32 supports the latter. Keep MAX17048 behavior behind the same API.
2. Supply pin, divider and attenuation from board configuration. P4X-EYE uses
   GPIO49 / approximately 4.012; select attenuation with headroom above 1.047 V.
   ESP32/S3 boards retain their own wiring. Base XIAO S3 Sense has no onboard
   battery divider, so it must continue reporting measurement unavailable.
3. Add explicit validity/capability metadata for voltage, cell presence, SOC,
   charging and external power. Unknown should render unknown, not USB/full/0%.
   Keep existing consumers on the shared API and extend them for these states.
4. Offer voltage-derived SOC only as an explicitly approximate estimate, with
   chemistry/load/charging limitations. Preserve real fuel-gauge SOC where present.
5. Keep hardware reads/calibration lifecycle serialized, propagate failures and
   stale samples, then enable the P4 battery command/page in its build profile.

No IDF 6 upgrade is required: these APIs are present in the pinned IDF 5.5.5.

## Qualification before enabling by default

Compare measured voltage against a meter with a known battery, both under USB
charging and on battery. Check averaging, stale/error handling, no-battery USB
operation, low-voltage behavior and calibration failure. Build ESP32, S3 ADC,
S3 fuel-gauge and P4 variants; check existing battery JSON/UI, logs, events,
power and OTA policy. The current XIAO S3 Sense can verify the unavailable path,
but cannot physically qualify another S3 board's divider or MAX17048.

## Sources

- [Official EYE guide and reference design](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html).
- Local reference design: primary checkout `work/p4x-enclosure/reference/ESP32-P4X-EYE/01_Schematic/SCH_ESP32-P4X-EYE-MB_V2.4_20260202.pdf`, visually inspected sheets 2 and 5.
- [IDF 5.5.5 P4 ADC oneshot driver](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/adc_oneshot.html), cross-checked against installed source.
- Tracked `components/hardwareone/System_Battery.{h,cpp}`, `WebPage_Battery.cpp`, `System_OTA.cpp`, and `experiments/p4_io/features.h`.
