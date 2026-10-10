# Sensor lifecycle and ESP-NOW setup verification — 2026-10-09

Status: **19 focused host suites, four firmware builds and the QT Py sensor
lifecycle run pass.** The physical run covers GPS, ToF and presence with an
SSD1306 OLED sharing the bus. Other sensor backends and setup page behavior
were exercised by host tests and firmware builds.

## Scope

The existing I2C worker now orders sensor start and stop requests. Closing
cancels pending starts, waits for an active initialization to return, and
announces completion after the old task releases its resources. Owner startup
rejects unfinished cleanup and reports initialization/task-creation failure.
The shared command path distinguishes an address NACK from a failed bus probe.
Sensor owners keep their drivers, readings and cleanup; lifecycle transport
announcements belong to the worker.

Serial, CLI mode and OLED ESP-NOW setup pages now apply accepted values through
one shared operation. Early Next at Name retains the current name, and leaving
an unfinished OLED page does not apply its partial name edits. This does not
change whole-wizard persistence or roll back previously accepted pages.

## Host and build verification

The four new production-code harnesses cover queue ordering/cancellation,
worker outcomes, all nine sensor slots with both I2C input backends, and all
three ESP-NOW setup handlers. They include cancellation during initialization,
failed allocation/task creation, unfinished cleanup, taskless FM/RTC shutdown,
deferred stop events, retry, and setup skip/back/early-next behavior. Existing
snapshot, GPIO input, MQTT setup, transaction, probe and sensor cleanup checks
also pass. The focused host build enabled ASan/UBSan for executable harnesses;
the source-contract checks remain static checks.

The additional camera-controller matrix passes 16 valid runtime selections
and two expected compiler rejections for the existing shared-controller
conflict. Its test was updated for the manager request type and to recognize
that pre-existing configuration rejection; camera rules were not changed.

All four firmware builds pass with ESP-IDF 5.5.5. Compiler-resolved feature
reports confirm the intended input variants and ESP-NOW selection.

| Build | App image (bytes) | Free app space (bytes) |
| --- | ---: | ---: |
| `qtpy-oled` | 2,877,168 | 3,107,088 |
| XIAO sensor verification, Seesaw input and ESP-NOW | 4,623,520 | 1,360,736 |
| XIAO sensor verification, ANO input and ESP-NOW | 4,618,432 | 1,365,824 |
| `xiao-default`, sensor queue disabled | 5,888,640 | 95,616 |

The two XIAO sensor selections are private verification profiles. Shared
editable flags, saved user selections and board wiring were not changed.
Relative to the immediate pre-change images, QT Py decreased by 576 bytes and
`xiao-default` increased by 32 bytes. Dependency lock files were restored to
their contents at the start of this change; the QT Py build's resolved lock
is retained with its local artifacts.

## Physical run

The Adafruit QT Py ESP32 Pico used the existing `qtpy-oled` profile and bus 0:
GPS `0x10`, ToF `0x29`, SSD1306 `0x3D`, presence `0x5A`. The gamepad remained
disconnected. Only the application at `0x10000` was flashed, with esptool hash
verification; flash was not erased and the data partition was preserved.

Application SHA-256:
`68d1b40ef26a7d6f0694b223bb206d431c06d161059d57972862425b806a3f5e`.

ELF SHA-256:
`cb7568d951ac0dd35b00e2971ada93448301142859ec551216606c68b4e3f122`.
The serial boot's reported ELF prefix matched this build.

The automated run completed in 153.39 seconds, including initial boot after
opening serial. Boot count stayed at 23 and crash count stayed zero thereafter.

| Exercise | Result |
| --- | --- |
| Three close/open/close burst rounds | All three sensors ended stopped, with no polling tasks and invalid readings. |
| Open all, then close all | All three ended stopped; queued work did not reopen them afterward. |
| Three completed restart cycles | Every cycle restored fresh connected readings and exactly one task per sensor. |
| Final 60-second soak | Seven samples over 61.72 seconds had fresh readings with advancing timestamps. |
| Three I2C scans | Each found all four expected addresses. |
| OLED alongside sensor changes | Reported connected/enabled throughout sampled checks; settings were unchanged. |
| I2C health / allocation failures | Zero errors and zero failures at all diagnostic checkpoints. |

Three rapid ToF starts reported failure within the existing two-second
restart cooldown; later starts succeeded. The test therefore establishes safe
refusal and successful retry, not that every immediate open request succeeds.
No panic, watchdog reset or new crash was recorded. The log contains 20
loop-health stall notices ranging from 200 to 1,062 ms; this test does not
qualify command or render latency.

| Diagnostic checkpoint | Free DRAM | Free PSRAM | Tasks |
| --- | ---: | ---: | ---: |
| Initial running baseline | 215,455 | 1,826,836 | 13 |
| Stopped after cancellation | 226,867 | 1,840,028 | 10 |
| Restart 1 | 215,443 | 1,826,836 | 13 |
| Restart 2 | 215,435 | 1,826,836 | 13 |
| Restart 3 | 215,451 | 1,826,836 | 13 |
| Final running sample | 215,451 | 1,826,836 | 13 |

The board was left with all three sensors running and the OLED enabled on
the Login screen. GPS supplied fresh readings without an indoor fix. Serial
was closed after testing. Machine-readable lifecycle results, status, image
and source fingerprints are retained in the local `build-config-qtpy-oled`
directory; the previous OLED-specific run is retained as historical evidence.

## Limits

This run does not physically qualify the absent sensor/input backends, FM
reception, servo output, ESP-NOW peer communication, unplug faults or electrical
recovery. The host tests model specific scheduling interleavings; they do not
prove every RTOS schedule or general concurrent peripheral-command safety.
The existing APDS mode-command versus driver-teardown synchronization remains
a separate concern.

The OLED stayed available, but its full brightness/sleep/wake test was not
repeated on this image. That earlier result remains in the dated
[OLED verification record](QTPY_OLED_TEST.md), tied to its own firmware hash.

## Cached-read follow-up — 2026-10-09

APDS's three dedicated read commands and ToF's text read now consume owner
snapshots. Four redundant hardware readers were removed. APDS records which
fields were sampled, so color-not-ready reports unavailable while independent
proximity/gesture samples can remain available. Gesture read commands do not
consume hardware FIFO data or publish events. ToF text uses the same published
filtered/smoothed distances as JSON and emits one distance line.

Eight focused ASan/UBSan host suites pass: `apds_read_commands`,
`tof_read_command`, `small_sensor_snapshots`, `sensor_reading_consumers`,
`sensor_parent_lifecycle`, `sensor_start_outcomes`, `tof_psram_lifecycle` and
`command_catalogue`. The APDS task sampling check includes 32 combinations of
bus, selected modes and color readiness. These are production-code tests with
controlled platform boundaries; APDS hardware was not connected.

| Build | App image (bytes) | Free app space (bytes) | Size change |
| --- | ---: | ---: | ---: |
| `qtpy-oled` | 2,876,544 | 3,107,712 | −624 |
| XIAO sensor verification, ANO input and ESP-NOW | 4,617,808 | 1,366,448 | −624 |
| `xiao-default`, APDS/ToF disabled | 5,888,640 | 95,616 | 0 |

The QT Py application was flashed without erasing data. Its SHA-256 is
`fa2b2349d196f37e5dea56ea9d273ee459f5ad62abf5e120d1a7b829b39e0362`;
the ELF SHA-256 is
`c3dca2e468c2d071f707110cb2c3d5fce47ec5cfa68ddfd40744e65382abeeb7`.
Esptool verified the application write, and the captured boot ELF prefix
matched. Dependency locks were restored to the contents at this follow-up's
start; resolved build artifacts were retained separately.

The 67.81-second hardware run passed. Ten JSON/text/JSON rounds produced six
honest no-target replies before restart and four successful single-distance
outputs afterward. Those four values agreed approximately with their adjacent
stable JSON samples; no same-timestamp triple was observed on hardware. Exact
text/JSON parity is covered by the host test. A stopped ToF returned an error
without a distance; reopening restored fresh samples after its cooldown.

GPS, presence and OLED stayed available during the check. Boot count remained
25, crash count remained zero, and both running diagnostic checkpoints had 13
tasks, 215,435 bytes free DRAM and 1,830,936 bytes free PSRAM. I2C health errors
and allocation failures were zero. Ten loop-health notices ranged from 208 to
1,484 ms; the run does not qualify latency. All three sensors and OLED were
left running, with serial closed and no saved-setting writes.

The local build's `cached-read-test-*` artifacts describe this image. The
earlier, broader sensor lifecycle run is retained under
`historical-sensor-lifecycle-2026-10-09/`; its results above remain tied to the
earlier image. APDS mode writes/teardown, proximity/gesture mode interactions,
and vendor gesture-read duration remain separate control-path concerns.
