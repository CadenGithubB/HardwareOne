# Shared CPU clock operation — 2026-10-09

Power modes, `cpufreq` and idle/wake now use `powerSetCpuFrequencyMhz` in
`System_Power`. The operation serializes clock requests, pauses background
polling and acquires existing I2C bus mutexes before calling the HAL. A lock
refusal leaves the clock alone; HAL acceptance and live frequency must agree
before a caller reports success. Startup does not allocate an I2C manager.

Idle entry confirms the clock before marking cooldown or sleeping the display.
Failed wake preserves its saved frequency and retries after the original
activity stamp has been consumed. Recovery of a partially failed idle change
uses the owner's indication that this call changed the clock; a busy refusal
cannot undo another command's accepted change.

## Firmware

| Profile | Image bytes | Free app space | Change from preceding build |
|---|---:|---:|---:|
| `qtpy-oled` | 2,893,456 | 3,090,800 | +128 |
| `xiao-default` | 5,888,384 | 95,872 | -128 |

Both saved profiles build. QT Py has I2C/display support; XIAO has those features
disabled. Saved selections and dependency locks are unchanged. Only the QT Py
application at `0x10000` was flashed; esptool verified the written hash. Its
settings/data partitions were preserved.

QT Py app SHA-256:
`de9b5539803559eadf7263ccdffeee36d2882727833d3a458618a979bc752dc5`.
ELF SHA-256:
`0b8b5a9e02574755751c1b973cc6e37318e796675235873879a579408e2caaae`.

## Verification

- The production owner, mode application and command pass six sanitizer-enabled
  combinations: I2C on/off and ESP32/S3 plus both P4 clock policies. Cases include
  missing buses, either lock failing, self-owned locks, concurrent/reentrant
  requests, nested polling pauses, HAL refusal and readback mismatch.
- The actual idle/wake function passes 56 sanitizer-enabled process-isolated
  scenarios across UART/STT feature gates. Removing the own-change check makes
  the concurrent-command regression test fail, confirming it catches that bug.
- Command catalogue and existing power profile/JSON/menu tests pass.
- A 110.63-second QT Py run exercised three sequences of 240/160/80 MHz requests
  and Balanced, PowerSaver, Performance and Locked presets. All four sensor
  caches (APDS, GPS, ToF and presence) advanced at 15 checkpoints. Sixteen
  diagnostics retained boot count 35, zero crashes, zero tracked I2C errors,
  zero allocation failures and 14 tasks.
- Power settings and CPU were restored and checked: Performance, 240 MHz,
  brightness 255, with the original ten-minute idle timeout. Preset writes used
  the existing write batch and the restored values were saved. That batch
  coalesces writes; it does not roll back live settings automatically.

After flashing, APDS color/proximity/gesture modes were restored to their earlier
runtime state. A 49.01-second follow-up passed 80/160/240 MHz requests with
gesture polling enabled. Five diagnostics retained boot count 36, zero crashes,
I2C errors and allocation failures, and 14 tasks. The original power settings
were again verified; all four sensors remain running and serial is closed.

The OLED is disconnected, so display output and physical idle/wake are not
qualified. Host fault injection covers refusal/retry behavior; the device run
covers ordinary clock changes with active sensors. No P4 was flashed. This
operation protects managed transactions, not concurrent bus initialization;
`initBus` still has its pre-existing separate synchronization behavior. HAL/SPI
callback latency is not bounded by the I2C mutex timeout.
