# QT Py OLED lifecycle verification — 2026-10-08

Status: **four host checks, three firmware builds and the automated QT Py
lifecycle run pass. The user confirmed that the OLED works.** Detailed contrast
response and electrical qualification remain outside that confirmation.

## Scope and configuration

The saved [`qtpy-oled`](../configurations/qtpy-oled.json) profile targets the
Adafruit QT Py ESP32 Pico with an SSD1306 OLED, ToF, presence and GPS enabled.
Physical input is disabled, so the display must build and operate without a
gamepad. This is a separate profile from the existing four-sensor bench build;
it does not change the shared editable feature defaults or board wiring.

The focused goal is one hardware lifecycle for boot, manual startup, stop and
wake. The existing `HAL_Display` now owns address probing, driver allocation,
readiness, brightness, power commands and the board's sleep/wake sequence.
`OLED_Utils` retains startup policy, boot animation, navigation, settings
application and display-session authentication boundaries. No new device
framework or registry was introduced.

## Implementation and ownership

- Boot and manual startup call the same HAL initialization. A newly allocated
  SSD1306 object is published only after the managed initialization succeeds;
  unpublished failures are freed. A retained object alone does not establish
  readiness, and failed initialization can be retried.
- The SSD1306 bus is latched with the driver. Subsequent operations and legacy
  setup-screen transactions use that same bus even after a reboot-required
  bus setting changes. Both common addresses remain supported, with the
  existing cold-panel retry policy shared by both startup paths.
- Wire initialization, locking and clock restoration remain with the I2C
  manager. The SSD1306 library is told not to initialize Wire, and its internal
  clock settings agree with the surrounding 400 kHz managed transaction.
- Brightness, dimming and power commands use managed bus transactions. Manual
  startup reapplies saved brightness as well as rotation, matching boot setup.
- The HAL preserves the existing power-gated board's rail decay, settling,
  bus recovery and charge-pump restoration sequence. Manual retry after failed
  wake uses that same restoration. Repeated sleep/resume calls preserve state;
  closing while asleep cancels automatic reopening. Restoring a shared rail
  does not start an inactive display.
- Stop clears the UI's authenticated display session and marks hardware
  unavailable. Successfully published SSD1306 storage is retained, matching
  the P4 display's existing canvas lifetime. This keeps borrowed drawing
  pointers valid across stop/restart, at the cost of retaining the 1 KiB
  framebuffer plus its driver object after the display has been stopped.

## Host verification

Four focused host checks pass with ASan/UBSan: `display_lifecycle`,
`p4eye_display_lifecycle`, `p4eye_display_pixels` and `oled_without_input`.
The display lifecycle harness compiles the complete production HAL
implementation and header with controlled I2C,
driver and GPIO boundaries. It covers initialization failure/retry, both
addresses on both buses, unavailable buses, lock refusal, latched routing,
retained storage, brightness/dim/power protection, disabled APIs and sleep/wake
ordering. It also covers failed wake followed by manual retry, repeated
prepare/resume, and closing while asleep.

The P4 backend's separate lifecycle harness exercises its production driver
with SDK boundaries substituted, including DMA cleanup and retained canvas
lifetime. The P4 pixel checks cover its canvas-to-panel conversion, and the
display-without-input check covers the independent feature selection.

## Firmware and hardware results

All three firmware builds pass:

| Build | Image (bytes) | Free app space (bytes) |
| --- | ---: | ---: |
| `qtpy-oled` | 2,877,744 | 3,106,512 |
| `xiao-display` | 5,734,992 | 249,264 |
| `xiao-default` | 5,888,608 | 95,648 |

The tested QT Py application's SHA-256 is
`29381fbf9e53237160f144728b42ab91250e0f47ff3b660f1d48be6826534b1a`.
The source and application fingerprints were checked against the tested build.
Dependency locks were restored to their contents at the start of this change.

The user replaced the gamepad with an OLED. The final automated run completed
in 194.19 seconds. Its deliberate restart exercised early boot initialization
and advanced the boot count from 18 to 19; the crash count stayed zero. The
remaining run stayed in boot 19. Thirteen bus scans each found exactly `0x10`,
`0x29`, `0x3D` and `0x5A`, matching the GPS, ToF, OLED and presence attachment
addresses.

The flash tool verified all three written segments. Review of both captured
boots and the test commands found no raw I2C errors, GPIO warnings, panics or
watchdog resets.

| Device test | Observed result |
| --- | --- |
| Early boot and three OLED stop/start cycles | OLED reported connected/running after startup and unavailable after stop; the other three sensor readings remained valid. |
| Brightness changes | Commands for 32 and 255 were accepted, with the final setting restored to 255. Visible contrast changes still require confirmation. |
| Two-second light sleep with OLED active | The device woke and OLED reported running again. |
| Two-second light sleep with OLED stopped | OLED remained stopped after wake, then manual startup succeeded. |
| Final 90-second soak | Nine rounds found all three sensor readings fresh; final task count was 13. |
| Free internal RAM / PSRAM | 215,535 / 1,830,932 bytes, identical in all six diagnostics samples. |
| Per-device I2C health errors / allocation failures | Zero / zero. |

The board was left with OLED enabled and autostart enabled, brightness 255,
the Login mode selected, and the three sensors running. After the run, the user
confirmed that the display works. Individual contrast levels and every visible
off/on transition were not separately confirmed.

Twenty main-loop stall warnings ranged from 201 to 3,122 ms, with the maximum
around the intentional light-sleep command; other notices reached 1,141 ms.
This run does not establish worst-case command or render latency.

## Qualification limits

Adafruit SSD1306 initialization and command methods do not report every I2C
write result. A successful managed operation shows that it ran; it does not
confirm that the panel acknowledged every command or displayed the intended
pixels. Address replies establish presence, not device identity.

Retaining the framebuffer prevents deletion from invalidating existing
borrowed pointers. It does not make all legacy framebuffer access synchronized
across tasks. Rendering remains with the existing UI owner, and wake schedules
redraw through that owner rather than adding a new rendering mechanism.

The QT Py OLED uses the always-powered bus 0. Its light-sleep test therefore
exercises software display off/on; it does not physically exercise the
power-gated board's rail decay, recovery or charge-pump restoration. Those
paths were checked with host substitutes. Physical power-rail behavior,
electrical fault recovery and bus timing remain unqualified. The P4 mock
tests likewise do not constitute physical P4 display qualification.
