# ToF polling verification — 2026-10-09

Implementation, six focused host suites, both firmware builds and the QT Py
hardware run pass. Earlier icon-test results remain associated with their
previous firmware.

## Finding and implementation

The stationary [icon test](QTPY_ICON_TEST.md) recorded one historical ToF NACK
counter increment during restoration after a clean display/sensor soak. No
degradation occurred. That log alone does not identify the failed operation.

A deterministic host reproduction of the previous polling implementation
demonstrated the shutdown accounting defect on both manager buses: stop arrived
while the polling task waited for the bus, `tofPoll()` returned false without
reading the device, and the manager classified that result as NACK even when
its address probe succeeded. The later cleanup transaction reset consecutive
errors while retaining the historical error count. This is a source-level
reproduction, not physical qualification of both buses.

`tofPoll()` now owns its transaction, following the existing presence-sensor
pattern. It checks readiness once, reads a ready sample, and checks the result
of rearming the next measurement. No conversion-wait loop holds the shared
bus. A successful not-ready status read leaves the cache unchanged.

The task retains the driver until its own cleanup, so one in-flight operation
may finish after an asynchronous stop. A stop before entry performs no bus
operation; a stop during the operation prevents later publication. The owner
checks the running flag again after acquiring the cache lock. Driver errors
still reach health accounting, including errors during shutdown; the stopped
task does not emit a new auto-disable fault.

Filtering and cache publication happen after releasing I2C. Cache contention
skips publication without becoming a bus error. A true return means successful
bus access, not necessarily a fresh reading. Existing range filtering,
smoothing, near/far events and JSON contents are preserved. No shared manager
API or new polling framework was added.

## Verification status

Six focused host suites pass with ASan/UBSan: `tof_poll_outcomes`,
`small_sensor_snapshots`, `tof_read_command`, `tof_psram_lifecycle`,
`sensor_parent_lifecycle`, and `sensor_task_restart`.

The new regression executes production poll/task, cache guard, manager and
health code. Both buses cover stop before entry and during bus/cache admission,
repeated not-ready status reads, mutex refusal, cache contention, returned
read/readiness/rearm failures and cleanup. Every cache admission verifies that
the I2C locks have been released; stop concurrent with a returned driver error
still records that error. At the fifth consecutive error, a running task emits
a fault while an already stopped task does not. The test uses real error
recording and the manager's recovery interval; degraded cleanup still frees the
owned driver when its best-effort stop transaction is refused. The existing
small-sensor JSON and OLED fixtures
remain unchanged, including filtering, smoothing and near/far event values.
See the [host test guide](../components/hardwareone/test/host/README.md#tof-polling-and-stop-outcomes)
for the runner and limits.

Both saved firmware profiles build successfully:

| Profile | Application bytes | Free application space | Size change |
| --- | ---: | ---: | ---: |
| `qtpy-oled` | 2,893,392 | 3,090,864 | -128 |
| `xiao-default` | 5,888,272 | 95,984 | 0 |

QT Py application SHA-256:
`c886b06f86e2b0f9da7f25dbd9dfb33c00ac26036285c1f8ffbbe8d091bf7c87`.

QT Py ELF SHA-256:
`2168fe840daa104bf9070fd4da64af62247bc1bafa4531c8c100ccc83b31e6b5`.

XIAO application SHA-256:
`8b0986edb816f5d9f2e3bcf7aa7cb48fe97abffcaf0bd26b93bd8ba38209588b`.

XIAO ELF SHA-256:
`4353663ed82fdb4b98cf6d05e6b95f32ee9ff658068c9e612c34a1e91be71790`.

## Hardware result

The application-only flash at `0x10000` was hash verified and boot reported
ELF prefix `2168fe840`. GPS (`0x10`), ToF (`0x29`), OLED (`0x3D`) and presence
(`0x5A`) were attached; APDS/gamepad were absent.

The 143.42-second run passed six completed ToF stop/restart cycles, varying
stop timing after a cached read, then a 25-second shared-bus soak. Each stop
completed task teardown and cache invalidation; the stopped text reader reported
stopped, and the cache did not republish afterward. Each restart produced fresh
readings and exactly one ToF task. GPS and presence kept publishing and the OLED
remained available in File Browser throughout.

Boot count stayed 46, crash count stayed zero, and all tracked I2C errors and
allocation failures stayed zero, including final cleanup. At each completed
ToF stop, free DRAM/PSRAM returned to 219,519/1,813,512 bytes while GPS/presence
ran. With all three sensors active, free PSRAM stayed 1,803,780 bytes; free DRAM
returned to 215,031 bytes after a temporary 284-byte dip. Initial boot and final
memory are not identical because the File Browser retains its initialized data.

Settings snapshots for OLED, ToF, GPS, presence, I2C and power matched before and
after. All three sensors are stopped as at boot. File Browser is left open with
the requested display login; serial is logged out and closed. No gesture, hand
motion, calibrated range or GPS fix was required or claimed.

Local `tof-poll-test-results.json` retains detailed observations and
`tof-poll-test-status.json` summarizes them; the generic `hardware-test-*` files
now index this image. The previous image and icon-test records are preserved in
`historical-before-tof-poll-2026-10-09/` in each build directory.

## Limits

The vendor's existing `VL53L4CX_GetMultiRangingData()` overwrites its internal
read status with `SetMeasurementData()`'s result. This change preserves errors
the public driver operations return; it does not guarantee detection of every
underlying I2C error or alter that vendor behavior. Removing the conversion
wait also does not preempt an individual Wire operation. These are separate
limits, not reasons to classify a skipped cache update as an I2C failure.
