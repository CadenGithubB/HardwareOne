# Shared CPU power controls

The isolated application now uses one target-aware clock policy for the CLI,
web power page, G2 power panel and OLED menu. ESP-IDF remains **5.5.5**, with
the common Bluetooth override described in [SDK-FIX.md](SDK-FIX.md).

| Build target | Supported interactive CPU clocks | Performance / idle floor |
| --- | --- | --- |
| ESP32 / ESP32-S3 | 80, 160, 240 MHz | 240 / 80 MHz |
| ESP32-P4 revision 3.x | 100, 200, 400 MHz | 400 / 100 MHz |
| Older ESP32-P4 revision selection | 90, 180, 360 MHz | 360 / 90 MHz |

The current P4X-EYE build selects the revision 3.x policy. The older P4 policy
has host-test coverage only. These controls change the main CPU clock, not
the companion C6 radio clock or Bluetooth transmit power.

Performance uses the target maximum; Balanced uses its middle clock;
PowerSaver uses the interactive floor. UltraSaver also uses that floor while
active, with a separate 40 MHz idle-only policy. Locked keeps the maximum
through idle power-save. Actual idle behavior depends on the display-enabled
power-save path; a listed idle frequency is not proof that a downclock occurred.

`cpufreq` lists the supported interactive values. `cpufreq <MHz>` accepts only
that list and checks the setter and actual readback. `power mode perf`,
`balanced`, `saver`, `ultra` and `locked` select shared presets. A failed clock
change returns an error instead of emitting a success event; the mode command
restores its previous saved selection. Direct clock overrides are temporary
and can be replaced by a subsequent mode change or wake.

`power json` and `/api/power/status` share the same snapshot, including live
`cpuMhz`, preset `activeMhz`/`idleMhz`, `supportedCpuMhz` and
`interactiveFloorMhz`. The web buttons use those reported capabilities. The G2
preset picker uses `Perf` and `Ultra` abbreviations to preserve the complete
active/idle frequencies and `MHz` suffix within its fixed row width. OLED labels
use the same policy; no separate frequency table is maintained in either UI.

## Initial device observations

Private serial run `20260927T190344Z-3106e303` recorded:

- P4 reported `[100, 200, 400]` and an interactive floor of 100 MHz. Direct
  commands successfully selected 100 and 200 MHz; selecting Performance then
  reported a live 400 MHz clock.
- S3 reported a live 240 MHz clock, supported values `[80, 160, 240]`, and
  preset idle/floor values of 80 MHz. This trace does not establish an actual
  80 MHz idle transition.
- Both boards reported `powerSaveSupported: false` in this headless profile.

These are initial command/telemetry observations, not measured energy savings,
clock qualification under every peripheral workload, or physical UI validation.
The OLED is disabled in this profile. Idle 40 MHz operation, light/deep sleep,
and automatic battery-driven policy transitions were not qualified here.

## Offline coverage and limits

The [offline report](results/2026-09-27/OFFLINE.md) records three compiled power
policies, checked failure handling, G2/OLED label lengths, bounded JSON output,
and five embedded-JavaScript syntax checks. The syntax suite does not execute
browser controls. Final abbreviated G2 labels retain the complete frequencies;
their visible presentation and physical selection still need their own evidence.

The existing direct clock-change path does not use the I2C drain guard employed
by the idle power-save path. This investigation did not qualify active I2C/OLED
peripherals during clock changes. No additional peripheral-support or sleep
claim follows from the headless checks above.
