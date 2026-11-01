# Shared power policy: P4, S3 and native-radio boards

Investigation dated 2026-10-26, checkout HEAD `f694a01b` with existing local
changes. Scope: examine power ownership and radio/sleep compatibility, define
a bounded implementation sequence, and broaden existing host regressions.
No production firmware, saved profile, SDK defaults or live device state was
changed. Host tests cannot qualify electrical savings or connection retention.

The unimplemented implementation plan is in
[POWER_MANAGEMENT_IMPLEMENTATION_PLAN.md](POWER_MANAGEMENT_IMPLEMENTATION_PLAN.md):
clock demand read from existing busy flags, the phase 1 scope, and the
measurements required before any sleep milestone.

The target use is an all-day Bluetooth connection with intermittent local or
glasses interaction and occasional notes/transcription. See
[battery calculations](P4_BATTERY_BUDGET.md) for the separate energy budget.

## Recommendation

Use one workload-driven policy in `System_Power`, with chip-specific clock
choices and radio/backend-specific sleep admission. Services publish whether
they are busy through their existing flags; `System_Power` reads those flags
and chooses the clock. A service never chooses a board or writes a CPU
frequency, and holds no request that it must later release. Keep display
visibility, CPU performance and permission to enter sleep independent.

Do not enable automatic processor sleep globally as a consequence of reducing
the default clock. Clock scaling and connected processor sleep have different
prerequisites and should be qualified separately. Until connected sleep is
qualified for the active backend and service combination, remain awake at the
supported idle clock and report why deeper savings are unavailable.

## Existing implementation and gaps

| Area | Evidence | Consequence |
| --- | --- | --- |
| Clock ownership | `System_Power.cpp::powerSetCpuFrequencyMhz` serializes Arduino clock changes and drains existing I2C buses. Mode, `cpufreq`, and idle/wake callers use it. | Preserve one clock owner. Do not layer SDK DFS and manual Arduino clock changes as independent controllers. |
| Chip clocks | `System_Power.h` selects P4 rev3 100/200/400 MHz, older P4 90/180/360, and ESP32/S3 80/160/240. | Shared workload levels can map to target clocks; 400 MHz must never be hardcoded into STT/camera service policy. These branches do not establish support for every possible future chip. |
| Idle state | `HardwareOne.cpp::powerSaveTick` is compiled/called under the local-display feature. It combines screen blanking, idle clock and restoration of the captured pre-idle frequency. | A future shared governor must run without a local display; headless tests are required. Display gating is currently a policy coupling. |
| Work inhibitors | Camera sessions (`cameraCaptureBusy()`) and microphone recording (`micRecordingBusy()`) defer idle entry. STT sessions (`sttAnySessionActive()`) wake/prevent idle and retain the current interactive clock. Each flag stays set until its workload has fully finished, including the STT workers' `vTaskDelete` exit. | Being busy must not automatically mean a lit display. The power owner should raise the clock while these flags are set, rather than relying on the selected mode being fast enough; in PowerSaver, STT currently runs at the floor clock and falls behind speech. |
| Command activity | `System_Utils.cpp::executeCommand` stamps activity for most external transports, excluding internal/UART and passive transcription commands. | Separate real user interaction from background status traffic before using activity to light a panel or raise the clock. |
| P4 light-sleep hooks | Companion hooks only pause/resume the heartbeat watchdog. | Watchdog silence is not a transport quiescence, buffering or wake handshake. |
| Explicit sleep | `cmd_lightsleep` uses timer wake and ignores the sleep API return. `cmd_deepsleep` stops/holds the companion only through P4 hooks; native hooks are empty. | Audit orderly native radio shutdown, busy operations, error handling, restoration and wake-source ownership before reusing these commands for connected idle. The deep-sleep current comment is not a board measurement. |
| Display/peripheral rails | `HAL_Display` handles per-board display power; P4 LCD/camera share GPIO12 and remain supplied on panel stop. | Keep rail ownership in HAL/board paths. Do not copy FeatherS3 display power gating to P4. |
| Periodic work | P4 encoder task samples buttons every 5 ms; main app loop has a 10 ms delay plus work inside its loop. C6 console and health monitoring have their own task/timer activity. | Enabling tickless idle alone does not create long sleep windows. Inventory deadlines, polling and wake frequency before changing them; retain input debounce and responsiveness. |

Relevant owners:

- [Power policy and clock owner](../components/hardwareone/System_Power.cpp)
- [Target clock definitions](../components/hardwareone/System_Power.h)
- [Main-loop idle handling](../components/hardwareone/HardwareOne.cpp)
- [Sleep commands and command activity](../components/hardwareone/System_Utils.cpp)
- [Radio HAL](../components/hardwareone/HAL_Radio.h)
- [Companion hooks](../components/hardwareone/System_RadioCompanion.cpp)
- [Display lifecycle](../components/hardwareone/HAL_Display.cpp)

## Radio/backend compatibility

### S3 and native radios

Direct `esp_light_sleep_start()` does not maintain native Wi-Fi/Bluetooth
connections. The supported connected-saving path combines radio modem sleep
with driver-coordinated automatic light-sleep. Deep sleep loses the connection.
This distinction is explicit in the
[IDF 5.5.5 S3 sleep guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/sleep_modes.html).

Automatic light-sleep requires PM configuration and FreeRTOS tickless idle;
driver/peripheral locks can prevent entry. See
[IDF 5.5.5 S3 power management](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/power_management.html).

Local IDF 5.5.5 source gives a more specific BLE constraint:
`components/bt/controller/esp32s3/Kconfig.in` includes the ESP32-C3 controller
Kconfig. It offers main-XTAL or external-32k clock paths for connected modem
sleep and warns against the internal RC low-power clock for connected BLE.
The main-XTAL light-sleep option also needs evaluating. The bundled S3 NimBLE
power-save example chooses external 32k and main-XTAL retention, but this
application uses Bluedroid: an example is not qualification for our host stack,
board crystal population, controller binary or multi-link usage. Do not assume
that every S3 has an external 32k crystal, or that no such crystal makes every
form of connected power saving impossible.

The checked-in `sdkconfig.esp32s3` has both `CONFIG_PM_ENABLE` and
`CONFIG_BT_CTRL_MODEM_SLEEP` disabled. Existing `build-config-xiao-default`
also has PM disabled. These are saved/build inputs, not evidence of current
device flash contents.

### P4 with C6 over SDIO

The C6 contains the BLE controller; the P4 runs the Bluetooth host over Hosted
VHCI. A running controller does not prove that the P4 can stop processing HCI
traffic indefinitely. Qualify transport queue capacity/flow control, sleep
entry races, usable wake signaling, timing and watchdog behavior together.
Do not reset or power-hold the C6 while retaining an active connection.

The pinned companion is Hosted **v2.12.13**. The existing local C6 build has
`CONFIG_BT_LE_SLEEP_ENABLE=y`, but PM and Hosted host-power-save are disabled;
the sampled P4 build also disables PM and Hosted host-power-save. BLE controller
sleep being enabled is not proof that either CPU enters automatic light-sleep.

The pinned Hosted host-power-save documentation describes a coordinated host
and companion protocol using a dedicated wake GPIO, with light-sleep described
as forthcoming. Its network-split examples concern network offload; they do
not prove retention of this application's P4-side BLE host across sleep.
Verify actual EYE wiring and the pinned sources rather than copying Function
EV Board GPIO defaults or newer Hosted examples.
[Pinned Hosted documentation](https://github.com/espressif/esp-hosted-mcu/blob/v2.12.13/docs/feature_host_power_save.md).

### Wi-Fi, ESP-NOW and other consumers

The application intentionally requests `WIFI_PS_NONE` after Wi-Fi connection;
`System_WiFi.cpp` records an I2C-recovery/peripheral-spinlock interrupt-watchdog
interaction as the reason. Its earlier handshake theory is explicitly marked
unproven in the same comments. Revisit the documented interaction before
enabling modem sleep; do not present the handshake theory as established.

`System_ESPNow.cpp` also requests `WIFI_PS_NONE` to retain asynchronous RX when
unassociated. Duty-cycled ESP-NOW receive requires an explicit protocol/latency
decision and testing with peers, rather than a global CPU power switch.

Native CPU sleep must account for Bluetooth, STA/AP Wi-Fi and ESP-NOW sharing
the radio. P4 must account for the same users plus transport and companion
state. An offline radio, running feature, connected link and actual traffic
are different states. Preserve explicit user feature choices; do not silently
stop mesh or Wi-Fi to make a power-saving state available.

## Proposed shared contract

Keep clock decisions and transition arbitration in `System_Power`; extend
existing HAL/radio interfaces only when a sleep milestone needs a concrete
backend answer. Native companion no-ops do not mean that native connected
sleep is supported.

The power loop derives three independent demands on every pass. None of them
is a request that a service holds:

1. Performance: read from the workloads' existing busy flags
   (`sttAnySessionActive()`, `cameraCaptureBusy()`, `micRecordingBusy()`) and
   resolved to the target's clock table. Phase 1 sends any busy workload to
   full speed; lower levels per workload come later and only with
   measurements. Camera capture is not automatically CPU-intensive: ISP/DMA
   activity may instead need peripheral clocks and uninterrupted service.
2. Continuity: the same busy flags also mark work that cannot tolerate sleep,
   such as capture, storage commit or transfer. Today they defer idle entry;
   a future sleep path must treat them as blockers too. Preserve deadlines and
   flash/PSRAM constraints.
3. Display demand: local interaction, explicit local output and the display
   timeout. Glasses-only use need not light the local LCD.

Demands compose: any busy workload raises the clock, any busy workload blocks
CPU sleep, and display visibility follows its own demand. Because the loop
reads flags instead of counting requests, a workload that is cancelled, fails
to start or deletes its own task clears its flag and the demand ends with it;
there is no separate hold to release or leak. A workload with no busy flag
gets one in its own module. Add a request API only if a real caller ever
needs one that a flag cannot serve. Apply transitions through the existing
serialized owner, outside I2C transactions/critical sections and without
blocking callbacks. That owner never waits for its lock, so a refused change
is retried later rather than failing the workload.

Do not change persisted user mode or brightness for a temporary workload boost.
Retain explicit manual/Locked behavior. The planned loop changes the clock only
when its target changes, so `cpufreq` and `power mode` choices last until the
next idle entry, wake or workload change. Recompute the desired clock when
work ends; blindly restoring a stale pre-idle value could overwrite a newer
user choice.

Extend the existing power JSON/UI owner rather than adding separate status
truth. Phase 1 adds only the target clock and the name of the busy workload.
Sleep capability and blocker reporting (unqualified Hosted wake path, native
BLE sleep configuration unavailable, radio recovery/update, OTA probation, a
driver-held SDK lock) is designed in the sleep milestone that needs it.
Configured permission is not proof of actual sleep residency: measure actual
transitions, wake cause and time at each CPU level.

CPU micro-idle should occur as soon as deadlines allow, independently of a
display timeout measured in minutes. If adjacent operations cause clock
thrashing, add a measured short delay before dropping the clock after a
workload ends. Do not reuse the existing 5-second explicit-sleep cooldown for
each SDK micro-sleep.

## Bounded implementation and verification sequence

1. **Baseline observability.** Measure connected-idle current on P4 and S3 with
   equivalent links/services. Record wake/poll rates, CPU residency and notes
   energy. Inventory timer deadlines without globally suppressing diagnostics.
   This runs alongside step 2. Steps 3–5 start only when these measurements
   show they target a real share of idle power.
2. **Busy-flag clock demand, no new CPU sleep.** Move the clock part of the
   idle routine out of the display gate into `System_Power`, so it runs on
   every board. Raise the clock while the existing STT, camera or recording
   busy flags are set, and remove the STT branch that wakes the panel to get
   the clock back. Stop remote commands from lighting the local panel. Make
   Adaptive the default mode (it took PowerSaver's place: with the boost in
   every mode the two were identical). Test normal, background, headless and
   cancellation cases; retain explicit settings. *Implemented 2026-10-26;
   hardware checks pending.*
3. **Native connected sleep qualification.** In an opt-in SDK preset/profile,
   validate native controller sleep clock choices, SDK PM/DFS ownership, driver
   locks, input wake and the existing Wi-Fi/I2C/ESP-NOW constraints. Build a
   selected S3 profile plus its baseline. Run real multi-link BLE endurance,
   recording, sensors and coexistence tests before enabling by default.
4. **Hosted connected sleep qualification.** Separately establish EYE wake
   wiring, transport sleep/wake support in the pinned version and BLE HCI
   continuity. Upgrade Hosted only as a deliberate compatible change if needed;
   qualify both host and companion. Native fallback remains available while
   Hosted remains unqualified, and vice versa.
5. **Explicit suspend/deep sleep.** Audit orderly service stop, storage drain,
   native radio shutdown, companion hold, return-code handling and wake
   configuration. This state may disconnect and must not masquerade as connected
   idle. Verify reboot/restoration and voltage behavior on each assembly.

Hardware matrix for each relevant phase:

| Combination | Required evidence |
| --- | --- |
| P4X-EYE + C6 | Actual glasses links/phone/ring as intended; HCI/SDIO flow, first incoming action, no false recovery, recovery after real companion failure |
| XIAO S3 / selected native BLE assembly | Same BLE traffic where supported; clock/driver constraints and input wake; no missed actions after long idle |
| Classic ESP32 baseline | Preserve 80/160/240 policy and radio/controller-specific behavior; do not import S3 BLE clock choices blindly |
| Headless or display disabled | Clock policy and idle work independently; remote actions do not require a display backend |
| Radio disabled/offline | No Hosted dependencies in native builds; unsupported connected sleep falls back with an honest reason |
| Wi-Fi STA/AP and ESP-NOW combinations | Join, receive, transfer, latency, coexistence and I2C error recovery stay correct; no silent service disablement |
| Recording/STT/camera/storage | No dropped samples, queue overrun, incomplete file or stale boost after cancel/error; compare energy per completed task |

Measure battery-side current, wake latency and observed sleep residency rather
than inferring success from configured flags or a completed build. Pending
hardware qualification is not an assertion that the hardware cannot support it.

## Verification performed in this investigation

The CPU-frequency host suite builds classic ESP32 and both P4 PLL/revision
branches, with I2C on/off. The idle/wake suite runs its real production
policy/tick across the same target branches, UART constraints and STT gates.
These are controlled host boundaries, not SDK/controller emulation or physical
sleep tests.

Both suites also list an `esp32s3` profile, added during this investigation.
It adds no coverage. `System_Power.h` only distinguishes ESP32-P4 from every
other target, and none of the code the suites extract checks for the S3. The
only other target-dependent branch, `SOC_UART_SUPPORT_REF_TICK`, is set
explicitly by the idle/wake suite for every build. So the `esp32s3` builds
compile exactly the same code as the `esp32` builds. The
[implementation plan](POWER_MANAGEMENT_IMPLEMENTATION_PLAN.md) proposes
removing that profile.

Commands:

```sh
python3 -B components/hardwareone/test/host/test_power_cpu_frequency.py --sanitize
python3 -B components/hardwareone/test/host/test_power_save_tick.py --sanitize
```

Both commands passed with AddressSanitizer and UndefinedBehaviorSanitizer,
first during this investigation and again on 2026-10-26 after this section
was corrected. The CPU suite runs 8 builds: 4 profiles × I2C on/off. Only 6
of them are distinct, because the `esp32s3` builds duplicate `esp32`. The
idle/wake suite runs 240 fresh-process scenarios: 15 scenarios × 4 UART/STT
builds × 4 profiles. Only 180 are distinct. Its summary line, which still
mentions S3, comes from the test script. This does not test real connected
sleep, target linking or current consumption.
`git diff --check` also passed. No firmware build directories,
dependency copies or serial sessions are needed for this investigation; host
executables live in automatically removed temporary directories.
