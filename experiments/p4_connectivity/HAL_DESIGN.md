# Bluetooth hardware boundary

The connectivity copy adds `HAL_Bluetooth.h/.cpp` beside the existing Input,
Display and Audio HALs. One `Bluetooth.cpp` still owns the GATT services,
application authentication, command handling, connection slots and role-transition
guards. The HAL owns controller selection, stack start/stop checks and power
capability. No separate P4 Bluetooth application is introduced.

The public interface is deliberately small:

- `bluetoothHalInit(name)` starts the Arduino BLE stack and requires an
  initialized wrapper, enabled Bluedroid host and enabled controller state.
- `bluetoothHalDeinit()` uses Arduino's checked teardown, retains controller
  memory for restart, and copies the existing phase/state/quarantine diagnostics.
  It additionally refuses success if the wrapper or host remains enabled or the
  controller is not idle.
- `bluetoothHalStatus()` reports backend, controller state, host state, wrapper
  state and whether controller status is a live local-driver read.
- `bluetoothHalSupportsTxPower()` and `bluetoothHalSetTxPower(level)` preserve
  the native 0–7 setting scale and propagate native driver failures.

Native builds read `esp_bt_controller_get_status()`. Hosted builds read the
Arduino adapter's `btHostedControllerStatus()`: idle/initialized/enabled only
after the corresponding C6 RPC succeeds, and unknown when the adapter cannot
establish the state. Hosted 2.12.13 has no remote controller-status query, so this
is explicitly **last acknowledged lifecycle state**, not a live C6 health
probe. A connected SDIO transport alone never counts as an enabled Bluetooth
controller. Unexpected companion resets and stale acknowledged state remain a
separate recovery qualification task.

Arduino owns the ordered Bluedroid/HCI/controller transitions. The Hosted
adapter disables/deinitializes only Bluetooth, keeps restart memory available,
and retains the shared Hosted transport while Wi-Fi uses it. The HAL never calls
Wi-Fi shutdown, resets C6 or tears down SDIO. Existing application role-transition
serialization remains in place; BLE callbacks must not start or stop the stack.

The pinned Hosted API has no remote BLE power setter. Hosted startup keeps the
controller default, and `bletxpower` reports that control is unavailable instead
of claiming a setting was applied. Native startup reports driver failures; a
runtime native change is saved only after all requested power operations succeed.

The Device Information Model characteristic now uses the existing `BOARD_NAME`
capability, with `HardwareOne` as the unknown-board fallback. P4 therefore does
not advertise itself as an ESP32-S3.

Add `HAL_Bluetooth.cpp` to the existing Bluetooth CMake source group. The HAL
requires the Arduino adapter's Hosted Bluedroid changes; it does not implement
a second controller owner. G2/glasses/ring code remains disabled for this
phase. Its additional direct stack/power calls need review before enabling
those optional roles.

`python3 experiments/p4_connectivity/test_hal_bluetooth.py` compiles the actual
HAL source for native and Hosted configurations with controlled driver
substitutes. Both checks pass: incomplete startup, uncertain remote state,
failed/incomplete teardown, restart, native power error propagation and explicit
Hosted power unavailability. These are interface checks; they do not prove
controller behavior, Wi-Fi coexistence or hardware recovery. Those require the
subsequent firmware builds and on-device tests.

## Integration work still required

The interface abstracts the location of the Bluetooth controller, while retaining
the existing Arduino/Bluedroid host and shared GATT implementation. It does not
make application code portable to an arbitrary Bluetooth stack. The reusable
parts are this HAL and the Hosted controller adapter; the serial GATT/HTTP clients
and `probeap` command are investigation fixtures.

Before integrating this into a normal deployment, add a shared transport owner
and lifecycle mutex for Wi-Fi and Bluetooth, define companion-reset recovery,
and reconcile the application's four connection slots with the C6's three-link
configuration. Define capabilities for unsupported operations such as Hosted
power control. Qualify normal STA operation separately from provisioning/AP mode,
and audit the optional glasses/ring code's direct controller calls. The P4 still
owns one HardwareOne identity, web server and authentication state; the C6 runs
the radio companion firmware, not a second HardwareOne application.
