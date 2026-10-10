# APDS9960 hardware verification — 2026-10-09

Latest status: **the gesture polling fix passes four-sensor stability and two
gesture-enabled restarts. A manual swipe follow-up captured UP and RIGHT.**
Reliable four-direction recognition remains unconfirmed; see the
[manual follow-up](#manual-swipe-follow-up--2026-10-09).

Initial result: color/proximity cached reads, two APDS restarts and four-sensor
stability passed, but gesture sampling failed. The initial evidence below is
retained separately from the corrected firmware's results.

The `qtpy-oled` saved configuration now includes APDS9960. The user replaced
the OLED attachment with APDS for this run; the gamepad is also disconnected.
The firmware still includes optional OLED support. Missing OLED hardware
reports unavailable while the four sensor owners start successfully.

## Build and hardware

ESP-IDF 5.5.5 built the Adafruit QT Py ESP32 Pico image successfully:
2,893,360 application bytes, with 3,090,896 bytes of application space free.
Only the application at `0x10000` was flashed, with hash verification. No
whole-device erase was performed; saved settings and data were preserved.
Serial boot identity matched the new ELF. Runtime sources are unchanged from
the cached-read implementation; this follow-up changes the saved feature
selection.

| Device | Bus 0 address |
| --- | --- |
| PA1010D GPS | `0x10` |
| VL53L4CX ToF | `0x29` |
| APDS9960 | `0x39` |
| STHS34PF80 presence | `0x5A` |

The board uses `Wire1`, SDA GPIO22 and SCL GPIO19. All four addresses appeared
in the initial scan. OLED status correctly reported `connected: false`.

Application SHA-256:
`288843b13c69d6ff9dd919a2012c96950b45609868e4471fb2a9c8b7175da87c`.

ELF SHA-256:
`0107c950284a06b15b25fcea87428c98ad77e886beed8f86fabdc3f715acc93e`.

## Startup, cached reads and restart

Default color mode supplied fresh RGBC values through `apdsread json`,
`apdsread` and `apdscolor`. Disabled proximity/gesture commands reported mode
off. Enabling proximity supplied cached proximity readings alongside color.

Two APDS stop/restart cycles completed. While stopped, all four text reading
commands refused reads, the polling task was absent, and JSON was invalid and
disconnected. Restart restored default color mode and fresh samples, with one
APDS task. GPS, ToF and presence remained available during those checks.

## Gesture failure

The initial run failed during gesture sampling. Gesture mode was acknowledged
at device time 68,106 ms, but the APDS cache remained at 67,877 ms and
`apdsgesture` reported no cached sample through 76,748 ms. Read commands still
returned promptly. Disabling gesture at 77,064 ms allowed sampling to recover;
subsequent APDS timestamps advanced again without restarting the APDS task.

The vendor `Adafruit_APDS9960::readGesture()` loop resets its inactivity timer
on each nonzero directional difference. Sustained input can therefore keep
that call running indefinitely. The polling task calls it inside the managed
I2C transaction and publishes its cache only after it returns. The transaction
timeout limits waiting to acquire the bus; it does not interrupt the driver's
read loop. This source behavior is consistent with the observed stall and is
the leading explanation; this test did not instrument the loop's individual
FIFO reads.

The test accepts a completed zero/no-gesture sample; it did not fail merely
because a swipe was absent. Gesture support is **not physically qualified**.
The focused follow-up is to bound the existing driver's execution and test
sustained directional input. No driver changes were made for this test.

The existing gesture-off command also disables hardware proximity, so recovery
explicitly re-enabled proximity. Sequential mode changes were exercised;
mode-command synchronization with teardown remains unqualified.

## Color/proximity and shared-bus follow-up

An independent 231.21-second run on the same image kept gesture off. It
completed 32 APDS sample rounds over 124.05 seconds, followed by 14 rounds
over 67.53 seconds. All 46 rounds required fresh APDS publications; 26 shared
sensor checkpoints required advancing timestamps from GPS, ToF and presence
as well. Final scan again found the four expected addresses, with one polling
task per sensor and OLED unavailable.

During the interaction window, RGBC ranges were R 2–4, G 2–3, B 2–3, clear
4–9; proximity ranged 0–3. These small variations establish changing readings,
not a confirmed response to hand movement or calibrated color/proximity
accuracy. No same-timestamp text/JSON comparison was captured; production-code
host tests from the preceding change cover exact snapshot parity.

| Diagnostic | Baseline | Final |
| --- | ---: | ---: |
| Boot count | 28 | 28 |
| Crash count | 0 | 0 |
| Tasks | 14 | 14 |
| Free DRAM | 216,451 | 216,451 |
| Free PSRAM | 1,830,876 | 1,830,876 |
| Tracked I2C errors | 0 | 0 |
| Allocation failures | 0 | 0 |

All nine diagnostic checkpoints reported no tracked I2C errors or allocation
failures. Free PSRAM was unchanged; free DRAM varied by 12 bytes at one
intermediate checkpoint. No unexpected reboot, panic or watchdog reset was
recorded. There were 42 loop-health notices, ranging 200–1,142 ms; this does
not qualify command latency. GPS supplied fresh samples without an indoor fix.

The board was left with APDS color/proximity, GPS, ToF and presence running;
gesture is off and serial is closed. Runtime modes were not persisted as new
saved settings. The original failed run is retained alongside the successful
scoped follow-up, and overall APDS qualification remains partial.

Current build-local records are `apds-test-status.json`,
`apds-test-results.json`, `apds-source-sha256.json`, `artifact-sha256.json`
and the common `hardware-test-*` files in `build-config-qtpy-oled/`. Detailed
serial logs remain in the private local APDS evidence archive. Earlier
cached-ToF/OLED results and fingerprints belong to the previous firmware and
are preserved in `historical-cached-read-2026-10-09/`.

Only the APDS saved feature choice changed for this build; the five runtime
and build-rule source fingerprints from the preceding cached-read verification
still match. The prior enabled/disabled firmware builds and host suites remain
applicable to those unchanged sources. Both dependency lock files match their
pre-build contents.


## Gesture fix follow-up — 2026-10-09

The existing vendored gesture reader now processes at most one available FIFO
batch per call, with no delay or loop waiting for additional input. Direction
signs persist between polls and expire after 300 ms without directional
activity. Completed gestures, invalid FIFO levels, failed FIFO transfers and
mode changes clear that state. The public signature, direction codes, sensor
cache and event producer are unchanged.

FIFO levels count four-byte U/D/L/R datasets. The reader now fetches complete
datasets, checks transfer success, processes each dataset and remembers signs
without counter overflow. The sensor owner's timing comment and the vendor
README document this polling behavior. No new manager or sensor abstraction
was introduced.

A first attempt bounded the old blocking loop at 300 ms. That stopped APDS
from freezing but delayed GPS sentence processing: GPS remained at timestamp
26,291 through device time 35,508 while APDS/ToF/presence advanced. Turning
gesture off let GPS resume without a GPS restart. GPS needs frequent shared-bus
access for NMEA characters; APDS's 300 ms transaction exceeded its 200 ms
polling interval. The final single-batch implementation removes that wait.
The failed intermediate run is retained in
`historical-apds-bounded-2026-10-09/` under the QT Py build directory.

Both ESP-IDF 5.5.5 builds pass:

| Build | Application bytes | Free app space |
| --- | ---: | ---: |
| `qtpy-oled`, APDS enabled | 2,893,408 | 3,090,848 |
| `xiao-default`, APDS disabled | 5,888,640 | 95,616 |

The QT Py image grew by 48 bytes from the initial APDS-enabled image. Saved
feature choices are unchanged.

Application SHA-256:
`2e08ac743252f10ac2777d2c5512adffb092e29a8377fc46d564e17f1af1d748`.

ELF SHA-256:
`56a3f518b11178fc70b942a31019eee15b24aa136a5055c12d970f7bc0292e95`.

Four focused CTest suites pass with ASan/UBSan: `apds_gesture_driver`,
`apds_read_commands`, `small_sensor_snapshots` and `sensor_parent_lifecycle`.
The driver suite executes production methods and requires at most one FIFO
transfer per call, no delay, and elapsed simulated time consisting only of
transport latency. It exercises 3,600 held-input calls, all four directions
within/across batches, 200 ms inter-call gaps, 301 ms idle expiry, no-data and
neutral gaps, complete datasets, invalid counts, failed partial transfers,
256+ same-sign records and clock rollover. Physical swipe accuracy and custom
polling intervals remain separate from these deterministic decoder checks.

The final application-only flash was hash verified. The 207.98-second hardware
run matched its ELF at boot, completed 40 fresh gesture-sampling rounds over
the two-minute window, and checked all four sensors' advancing timestamps at
15 checkpoints. APDS completed 49 gesture samples across the whole run,
including checks after both restarts and final mode recovery. Every result
was a completed no-gesture sample, not the former indefinitely pending cache.
No swipe event was captured; physical recognition accuracy remains unconfirmed.

Both close/reopen cycles began with gesture enabled. Stop/task cleanup was
observed within 0.942 and 0.952 seconds respectively, and restart restored fresh
readings with exactly one polling task per sensor. GPS, ToF and presence
continued updating through the stops. Color reads worked before gesture,
after each restart and after gesture-off. While gesture ran, color reported
not-ready on all 49 sampled checks; this is recorded rather than counted as a
successful color measurement.

| Diagnostic | Baseline | Final |
| --- | ---: | ---: |
| Boot count | 32 | 32 |
| Crash count | 0 | 0 |
| Tasks | 14 | 14 |
| Free DRAM | 216,451 | 216,459 |
| Free PSRAM | 1,830,872 | 1,830,872 |
| Tracked I2C errors | 0 | 0 |
| Allocation failures | 0 | 0 |

All nine diagnostic checkpoints had zero tracked I2C errors and allocation
failures. No bus-error, panic or watchdog markers were recorded. Free DRAM
ranged 216,435–216,459 bytes; free PSRAM stayed fixed. Thirty loop-health
notices ranged 200–1,058 ms, so general command latency remains unqualified.
The driver polling loop no longer adds a wait; this does not preempt an
underlying Wire transaction.

APDS color/proximity/gesture, GPS, ToF and presence were left running; serial
is closed. Runtime modes were not persisted as new saved settings. Whole-device
flash and data partitions were not erased. Dependency locks were restored to
their pre-build contents. Gesture recognition at every custom poll interval
is not claimed: intervals above 300 ms expire partial direction state between
calls. Concurrent mode-command/teardown synchronization remains outside this
change.

Current records are `apds-gesture-test-status.json`,
`apds-gesture-test-results.json`, `apds-gesture-source-sha256.json`, the
corresponding `apds-test-*`/`hardware-test-*` files and updated
`artifact-sha256.json` in `build-config-qtpy-oled/`. Earlier failed runs remain
in the two historical APDS directories. Host tests also reject the original
vendor reader because it sleeps while holding the caller's bus lock.

## Manual swipe follow-up — 2026-10-09

The user requested another capture and confirmed completing directional swipes
and a five-second near-sensor hold. No firmware, build choices or saved settings
changed. Serial boot matched the same fixed ELF above.

The 173.61-second run captured 13 unique gesture events from the event ring:
12 UP and one RIGHT. Its two-minute interaction window completed 37 sampling
rounds and 14 checks of all four sensors' advancing timestamps. Across the
whole run, 40 gesture-cache samples completed; one caught UP directly and the
others returned no current gesture. Event history retained the brief detections
between command samples, so the result uses unique event sequence numbers.

The first events appeared late in the window, after closer 3–5 cm passes were
requested. Individual hand movements were not timestamp-labeled, so the test
cannot assign each event to a specific swipe or prove sensitivity at a given
height. DOWN and LEFT were not captured. This establishes physical gesture
response, not reliable recognition of all four directions; board orientation
also affects the direction labels.

All seven diagnostics had boot count 33, crash count zero, 14 tasks, zero
tracked I2C errors and zero allocation failures. No bus-error or fatal runtime
markers were recorded. The sensors were left running with gesture enabled,
and serial is closed. Restart cycles were omitted because the preceding run
already qualified them.

Build-local `apds-swipe-test-status.json` and `apds-swipe-test-results.json`
retain this follow-up, and the existing current status records link to it.
