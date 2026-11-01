# P4X-EYE all-day battery budget

Investigation: 2026-10-26. Intended use: continuously connected over Bluetooth,
intermittent glasses/display interaction, occasional transcription or recorded
quick notes. Provisional day: 16 hours; also calculate 12 hours below.

The follow-up [shared power-policy investigation](CROSS_BOARD_POWER_POLICY.md)
compares P4/SDIO and S3/native-radio sleep requirements and defines the staged
compatibility and hardware qualification plan.

## What is established

There is no measured whole-board current or endurance result in the repository.
The existing battery qualification verified BAT-node voltage sampling, without
confirming an installed cell or measuring current. No firmware, device settings,
or connected hardware were changed for this investigation.

The stock board measures voltage on GPIO49 through a divider. It has no battery
current sensor or authoritative software indication of USB/charging state.
`batterystatus json` is useful for voltage logging, but voltage-derived percentage
cannot establish energy use. The `voltage` command is particularly unsuitable:
`System_Utils.cpp` starts with a generic 80 mA ESP32 assumption and adds fixed
feature allowances; it is not a P4X-EYE power measurement.

Source evidence:

- [Battery hardware results](../experiments/battery_portable/RESULTS.md).
- [Battery circuit investigation](../experiments/board_qualification/BATTERY.md).
- [Board peripherals and qualification limits](P4X_EYE_PERIPHERALS.md).
- [Power policy](../components/hardwareone/System_Power.h).
- [Display backend](../components/hardwareone/HAL_Display_P4Eye.cpp).
- [Camera backend](../components/hardwareone/HAL_Camera_P4.cpp).
- [Companion policy](../components/hardwareone/System_RadioCompanion.cpp).

## Implications for this workload

| Consumer | Finding | Battery implication |
| --- | --- | --- |
| P4 and memory | Family defaults select 400 MHz and 250 MHz PSRAM. Runtime supports 100/200/400 MHz, with 40 MHz only during UltraSaver idle. | Profile and runtime state both matter. Lower clock does not imply proportionally lower energy per transcription. |
| C6 radio companion | BLE/Wi-Fi/ESP-NOW run on a separate ESP32-C6 through Hosted SDIO. C6 automatic hold defaults off; when enabled it waits for no radio feature to be running. | A continuous BLE connection needs the companion available. Its reset/hold saving cannot be counted during connected use. |
| Idle power policy | The display timeout blanks the screen and downclocks according to the selected mode. A sampled existing P4 build (`build-config-p4-openclaw/sdkconfig`) has `CONFIG_PM_ENABLE` unset. | Screen-off idle is not evidence of automatic light-sleep. This sampled build is not proof of what is currently flashed. |
| LCD/backlight | Backend turns off backlight/panel but deliberately leaves GPIO12 asserted, because LCD and camera share the rail. | Screen-off saves backlight power, but does not electrically remove both peripherals. |
| Camera | Stop tears down capture/clock resources while preserving the shared rail. | A compiled but unused camera is not the same as a camera streaming, or as a disconnected camera module. Measure the actual state. |
| Microphone/STT/storage | Microphone and camera support are compiled in the board profile; SD is off by default. Recording and transcription are distinct workloads. | Budget short active bursts separately. Confirm whether a mic/VAD/STT pipeline continues running between notes. |
| Accessories | Glasses and ring have separate batteries unless powered by the P4. | Their own battery capacities are separate; their connection traffic affects P4/C6 consumption. |

For context, Espressif's current P4 datasheet lists typical chip current at
400 MHz of 23/56 mA in dual-core WAITI, and 97/150 mA in a dual-core data-access
test. At 100 MHz those cases are 17/40 and 36/63 mA. The pairs represent all
peripheral clocks disabled/enabled, not statistical lower/upper bounds.
Flash/PSRAM accesses can increase draw. These are chip tests at 3.3 V and 25 C,
not measurements of this application's board or its radio. Consequently the
chip's approximately 0.076–0.495 W examples cannot be used as whole-board limits.
[P4 datasheet, section 5.6](https://documentation.espressif.com/esp32-p4_datasheet_en.html).

## Calculation

Use energy, because battery, USB and internal rails have different voltages.
For assumed total board load after battery power conversion:

```text
average_load_W = sum(state_power_W * state_hours) / total_hours
required_cell_Wh = average_load_W * hours / (efficiency * usable_fraction)
required_cell_mAh = required_cell_Wh / nominal_cell_V * 1000
runtime_hours = capacity_mAh / 1000 * nominal_cell_V
                * efficiency * usable_fraction / average_load_W
```

Example assumptions: 3.7 V nominal single-cell lithium battery, 85% aggregate
conversion efficiency, 80% usable nominal capacity. The last factor provides
an allowance for reserve, aging, temperature and cutoff; it is not a measured
property of this board. Together they deliver 68% of nominal cell energy to the
modeled loads. Adjust them after measurement.

**Sensitivity cases, not measured or predicted board loads:**

| Assumed average board load | Cell capacity for 12 h | Cell capacity for 16 h |
| --- | ---: | ---: |
| 0.25 W | 1,200 mAh | 1,600 mAh |
| 0.50 W | 2,400 mAh | 3,200 mAh |
| 0.75 W | 3,600 mAh | 4,800 mAh |
| 1.00 W | 4,800 mAh | 6,400 mAh |
| 1.50 W | 7,200 mAh | 9,500 mAh |

Rounded to approximately 100 mAh. Conversely, a 1,000 mAh cell permits only
0.157 W average board load over 16 hours under these assumptions; 2,000 mAh
permits 0.315 W, 3,000 mAh permits 0.472 W, and 5,000 mAh permits 0.786 W.

An illustrative connected-idle load of 0.5 W for 90% of the day and active load
of 1.2 W for 10% gives 0.57 W average, requiring approximately 3,625 mAh for
16 hours. Those two loads are placeholders, not measurements or vendor claims.
For this mostly-idle workload, reducing connected-idle draw by 0.1 W for 14.4
hours saves about 572 mAh of required nominal capacity. That makes the connected
baseline the first thing to characterize.

If measuring **at the battery connector**, measured watts already include the
board's conversion losses: use efficiency = 100% to avoid counting losses twice.
If measuring on USB, `5 V * USB current` includes the USB power path and possibly
charging; it does not establish battery-path consumption. Do not mix 5 V mAh
with 3.7 V cell mAh. Integrate actual battery voltage times current for watt-hours.

## Measurements that resolve the estimate

Use an inline current/energy instrument at the battery connector, with USB
power disconnected. Record actual voltage and integrate current over time;
capture transient peaks as well as average draw. A voltage-only ADC log cannot
replace this. Keep the same cell/supply voltage and peripherals for A/B tests.

1. Establish a 10–30 minute connected-idle baseline: intended glasses/phone/ring
   links connected, screen blanked, camera stopped, intended background services
   running. Record live `power json`, `c6status json`, and `batterystatus json`.
2. Repeat at supported 100, 200 and 400 MHz settings. Check BLE responsiveness
   and missed work as well as current. Treat 40 MHz UltraSaver idle as an
   additional unqualified experiment, not an assumed saving.
3. Measure incremental screen use at the intended brightness, a typical glasses
   exchange, a one-minute recording and a representative transcription. Include
   completion time and return to idle; watt-hours per note are more useful than
   peak current alone. Check background mic/STT state between notes.
4. Run a representative hour and then a full discharge on the candidate battery.
   Confirm reconnection behavior, recording integrity, low-voltage cutoff and
   transient supply stability. Capacity alone does not establish peak-current
   capability.

No current instrument was available through this investigation; no serial port
was opened and no physical qualification is claimed. A firmware build would
not validate these electrical quantities.

## Packaging and sources

Espressif specifies a battery envelope of **4 × 25 × 45 mm**, a **1.25 mm pitch
connector**, and polarity matching the PCB for the stock enclosure. A required
capacity must be checked against actual cell dimensions; connector pitch alone
does not establish pin polarity. No particular capacity fitting this enclosure
has been established here.
[Official P4X-EYE guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html).

The older MB v2.3 schematic labels a 500 mA charging setting. That concerns
charging, not operating draw or battery capacity; confirm the actual PCB
revision's charging circuit before using it to choose a cell or predict recharge
time. The repository's v2.4 circuit investigation is the relevant revision
reference, but its archived local schematic path is absent in this checkout.
[Official MB v2.3 schematic, sheet 5](https://dl.espressif.com/AE/esp-dev-kits/SCH_ESP32-P4-EYE-MB_V2.3_20250416.pdf).
