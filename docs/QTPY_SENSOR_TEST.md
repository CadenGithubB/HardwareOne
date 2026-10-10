# QT Py sensor bench test — 2026-10-08

Status: **corrected firmware passed the completed stop/start and polling test**.
Live testing confirmed sensor communication and exposed two software failures,
which were addressed before the final run. Qualification limits remain below.

## Scope and setup

The saved [`qtpy-sensors`](../configurations/qtpy-sensors.json) profile targets
the Adafruit QT Py ESP32 Pico with VL53L4CX ToF, Seesaw gamepad, STHS34PF80
presence and PA1010D GPS enabled. All four share the existing STEMMA I2C bus.
The profile exercises the modules' copied readings and existing JSON builders
without requiring a display. Shared editable defaults and board wiring were
not changed for this test.

The first board's USB bridge enumerated, but automatic and manual bootloader
attempts produced no usable ESP32 response. No firmware was written to it.
The replacement worked through the same host connection. This narrows the
first board's problem but does not establish that its chip is dead.

The replacement passed flash verification and the PSRAM boot test. Its existing
filesystem would not mount. The complete data partition was backed up locally
before the user authorized erasing that partition. The empty filesystem then
formatted and mounted normally. A local administrator account was created
using the user-requested credentials; credentials and raw logs are not part
of this report. Login survived subsequent firmware flashing.

## Observed results, in order

| Stage | Evidence | Conclusion |
| --- | --- | --- |
| Initial sensor firmware | ToF returned valid, status-0 targets at 1666 mm, then 95 mm and 1705 mm. Presence changed from not detected to detected. GPS returned fresh parsed readings with no fix. | ToF, presence and GPS communicated and produced live cache readings. Distance accuracy and outdoor GPS reception were not qualified. |
| Gamepad connected | Initialization succeeded; shared-cache JSON became valid with advancing timestamps. | The gamepad was detected and polled. Full joystick travel and button presses were not captured in the sampled data. |
| I2C disruption during initial testing | Bus errors were followed by a reported stack overflow in `tof_task` and a panic reboot. The normal ToF stack minimum was only 604 bytes free out of 3072. | A real firmware crash occurred. Its corrupted backtrace cannot identify the deepest call or prove the precise electrical trigger. |
| ToF stack increased to 4096 bytes | The rebuilt image ran all four tasks, with 1636 bytes free in the ToF stack at the first live measurement. Three affected sanitized host tests passed. | The additional stack budget is installed and normal polling works. The original I2C failure path has not yet been reproduced on this image. |
| Stop and restart on the stack-adjusted image | All four tasks stopped and readings became invalid. After restart, ToF, presence and GPS announced online but their tasks were absent and readings stayed invalid/stale. | The lifecycle test failed. Completed deletion left task handles that restart code queried after their tasks could already have been reclaimed. The lifecycle correction below was then built and flashed for a repeat test. |

The failed restart was not an immediate stop/start overlap: diagnostics showed
the sensor tasks gone, reopening began about 18 seconds after stopping, and
the workers were still absent about 10 seconds after reopening. Zero I2C
health errors in this state do not show successful polling.

The correction reuses the existing sensor task exit helper. Each task clears
its owning handle after sensor cleanup, immediately before deleting itself;
creation no longer queries a deleted task's handle. This addresses restarting
after a completed stop. Reopening during unfinished cleanup remains outside
this test's scope.

One concurrent gamepad start reported “not detected,” then succeeded on a
later attempt. Its existing preflight maps a failed 50 ms bus-lock acquisition
to that same message. Contention with another sensor's initialization is a
plausible explanation; the message alone does not establish disconnection.

The power-on reset associated with reflashing reset the RTC-backed crash count
and hid the previous panic from the current `crashlog` response, as designed.
Those clean counters do not erase the earlier observed crash from this test
record. Stability must be measured within a known boot of the final image.

## Qualification limits and remaining checks

The user performed the requested physical interactions, but sampling did not
capture a pressed gamepad button or full joystick travel. One interaction
window also overlapped the failed restart. The JSON command reports current
button levels, so sparse sampling can miss short presses. These checks remain
unverified, rather than evidence of defective controls. ToF and presence
responses were also captured on the final corrected image, as recorded below.

GPS had fresh readings with no fix, quality zero and no satellites. No fix is
expected in this indoor test; a location fix is outside its acceptance scope.
Wi-Fi and the sensor web interface are compiled in but were not exercised.
MQTT testing was explicitly skipped. Other sensors and board types were not
physically qualified by this run.

The completed stop/start test does not qualify rapid reopening while cleanup
is still underway. The original I2C disruption was not deliberately reproduced
on the corrected image; normal polling cannot establish that failure path's
stack margin or recovery behavior.

## Final corrected-firmware result

The final image passed a 263-second automated run in one unchanged boot: a
60-second initial soak, three completed stop/start cycles and a 120-second
final soak. The soaks sampled all four caches in 15 and 29 rounds respectively;
every sampled reading was valid and timestamps advanced. Each stop invalidated
all four readings and reduced the task count from 14 to 10; each restart
restored all four workers and fresh readings.

The final raw log contains valid ToF targets from 86 to 1633 mm, and both
presence and motion detection flags changed between false and true. GPS
readings remained fresh without a fix. Gamepad timestamps advanced, but its
sampled values stayed at x=519, y=510 and the idle button mask throughout both
soaks; button/full-travel validation remains incomplete.

| Final-run measure | Result |
| --- | --- |
| New resets or panics | None |
| Reported per-device I2C health errors after startup | Zero throughout |
| Allocation failures | Zero |
| Free internal RAM, stopped / running | 234,343 / 218,943 bytes, identical across all three cycles |
| Free PSRAM, stopped / running | 1,884,460 / 1,871,268 bytes, identical across all three cycles; running value unchanged at the end |
| Final stack minimum free, ToF / GPS / presence / gamepad | 1628 / 1652 / 1532 / 1348 bytes |

One raw I2C receive error occurred during boot, before the sensor-start test;
its cause was not established. No further raw I2C errors occurred in the test.
CLI activity also produced main-loop stall warnings from 203 to 1344 ms,
including 1013 ms during sampling and 1344 ms during login, without a watchdog reset.
This run does not qualify worst-case command latency.

The firmware application is `build-config-qtpy-sensors/hardwareone-idf.bin`;
its SHA-256 is
`a4b8e49aac576ba58ce7c12cc0a8af37ade884e3ac5737d1a679825731938884`.
Eight affected sanitizer-enabled host suites passed, including a regression
that exercises the production task-creation functions and exit helper. The
following firmware builds also passed:

| Build | Image (bytes) | Free app space (bytes) |
| --- | ---: | ---: |
| `qtpy-sensors` | 2,732,304 | 3,251,952 |
| XIAO all-sensor verification profile | 5,625,600 | 358,656 |
| Sensors-disabled `xiao-default` | 5,888,592 | 95,664 |

## Discovery follow-up — 2026-10-08

This follow-up uses a later image than the lifecycle result above. Investigation
of the old discovery path reproduced a transient address ACK at unattached
address `0x0B`, followed by the raw receive error when the scanner attempted an
unrequested byte read. Reads at all four attached addresses succeeded during
that diagnostic run. This establishes one way the old scanner produced the
error; it does not identify the address involved in the earlier boot error or
attribute that error to the Seesaw gamepad.

Discovery now uses one address-only probe owned by `I2CDeviceManager`. A positive
result requires two address ACKs under the same bus mutex and clock setting;
no device payload is read or written. The manager restores its clock policy
after the probe and does not add scan attempts to sensor health records or
ordinary transaction metrics. CLI, OLED, boot discovery and hardware detection
all reuse this probe. The duplicate smart-scan `Wire.begin`/clock handling and
unused full scanner were removed. Address-table labels now prefer primary
matches, so `0x29` is labeled VL53L4CX rather than BNO055's alternate address;
the sensor driver remains responsible for checking actual device identity.

Three focused host checks pass: `i2c_probe`, `i2c_transaction_options_tests` and
`i2c_transaction_contract`. The compiled harnesses ran with ASan/UBSan. These
cover probe confirmation, failed/contended access, both physical bus mappings,
clock restoration, disabled wrappers and keeping all scanner callers on the
shared operation. Firmware builds also pass:

| Build | Image (bytes) | Free app space (bytes) |
| --- | ---: | ---: |
| `qtpy-sensors` | 2,732,096 | 3,252,160 |
| XIAO all-sensor verification profile | 5,624,784 | 359,472 |
| Sensors-disabled `xiao-default` | 5,888,608 | 95,648 |

The flashed QT Py application SHA-256 is
`7af3e83921c72f03900837ee2e47be9796793502593658946b931e9ed2bc49d9`.
**This image passed the completed 237.87-second hardware run.** Ten scans with
the sensor workers stopped and ten with all four polling each found exactly
`0x10`, `0x29`, `0x50` and `0x5A`, with the correct ToF label and no phantom
addresses. Both `discover` and `detect` found all four in each state. `detect`
reports saved feature/autostart choices, so its "attachable" label does not
mean that a sensor's live worker is stopped; separate task and reading checks
verified runtime state.

A 30-second initial soak sampled all four readings in seven rounds; a
120-second final soak did so in 29 rounds. Every sampled reading was valid and
connected, with advancing timestamps. Between those soaks, three completed
stop/start cycles reduced the task count from 14 to 10 and invalidated all four
readings, then restored the four workers and fresh data. The boot count stayed
at 14 throughout, with no crash record or reset.

| Discovery-run measure | Result |
| --- | --- |
| Raw I2C errors, GPIO warnings or panics in the completed run, including boot | None |
| Per-device I2C health errors / allocation failures | Zero / zero |
| Free internal RAM, stopped / running | 234,347 / 218,939–218,955 bytes |
| Free PSRAM, stopped / running | 1,882,404 / 1,869,212 bytes; unchanged across cycles and final soak |
| Final stack minimum free, ToF / GPS / presence / gamepad | 1632 / 1656 / 1316 / 1424 bytes |
| Main-loop stall warnings during command activity | 201–1298 ms, including the longest during login; no watchdog reset |

The raw log now captures both joystick axes changing and pressed states for
all four face buttons, A/B/X/Y. Across 256 valid gamepad snapshots, X ranged
from 2 to 978 and Y from 238 to 510. This confirms responsiveness that earlier
sampling missed; full travel on both axes and Select/Start remain unqualified.
Valid ToF targets ranged from 135 to 1748 mm, and presence and motion detection
each changed between false and true. GPS remained fresh with no fix, quality
zero and no satellites, as expected indoors.

Two preliminary runs stopped because the host harness incorrectly expected
`detect` to report live worker state; those assertions were corrected without
changing firmware. One abbreviated run emitted two GPIO reservation warnings
during detection. They did not recur in the completed run, but their cause is
not established. Successful normal scans and polling still do not qualify
electrical fault recovery or reproduce the original ToF overflow path. OLED
scan integration was checked in host tests and the enabled XIAO build, not on
physical display hardware.

The next separate lifecycle work should address reopening during unfinished
cleanup and the start-command preflight that can report an unavailable bus as
an absent device. Neither behavior is claimed fixed by this discovery change.
