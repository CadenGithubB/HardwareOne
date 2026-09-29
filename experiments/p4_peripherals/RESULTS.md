# Build and offline validation — 2026-09-27

Both final ESP32-P4 firmware builds passed using ESP-IDF 5.5.5, Arduino 3.3.5
and ESP-Hosted 2.12.13. The enabled image includes LCD, GPIO wheel with three user buttons, and SDMMC
with HTTP, BLE and ESP-NOW; camera and microphone are disabled. The enabled
application has 42% free space in its 0x615000-byte app partition.

| Profile | Image bytes | SHA-256 |
| --- | ---: | --- |
| peripherals_enabled | 3,671,472 | `822f07efa02e2414620f17d4049a62289c7b612dc2f179264e0bf201ecb9b07b` |
| peripherals_disabled | 3,371,008 | `7379798775192bb9133f26da03b332f54e417d0713ceb8a998e6aee5c1360a9e` |

All **13** focused checks passed, with address/undefined-behavior sanitizers
for the C++ driver/core tests:

- Display pixel geometry and real-driver lifecycle, including partial-init
  cleanup, active-low backlight, sleep, DMA timeout retention and restart.
- Rotary Gray-code decoding, contact bounce, invalid jumps, rapid turns,
  click/hold/double-click/press-turn, startup-held switch and timer rollover.
- Auxiliary button debounce, simultaneous X/Y/START, held-at-start suppression
  and timer rollover; actual GPIO driver enabled/disabled, wheel/key coexistence,
  short-press latching, pin release and restart.
- Real HAL button snapshot consumption: short latched clicks, first sample,
  exactly-once delivery, lock failure and invalid/stopped state.
- Actual preprocessor configuration for all eight independent peripheral
  enable/disable combinations, optional user buttons, no-I2C operation and explicit
  board identity (including an undefined board-selection macro).
- Real SDMMC lifecycle with injected no-card, power, mount, unmount and format
  failures, removal latch, resource ordering, no implicit format and an
  SDK-free disabled-backend compilation.
- Existing VFS capacity/cache and filesystem permission contracts, I2C
  transaction contract, and unchanged raw-allocation inventory.

After changing the display to centered 1.5× scaling, both display tests were
rerun and passed, including exact image bounds, four black borders, inversion
and monochrome pixel output. The enabled P4 image was rebuilt; the disabled
profile does not compile the rasterizer and retains its previous image.

A new reconstruction applied the overlay with zero fuzz and verified all
**7,929 source files** against the recorded manifest. All 26 peripheral/UI
source files without platform-overlay differences were also compared directly
with the shared production implementation. The source diff passes whitespace
checks. Build logs and test logs remain under ignored `private/`.

**No hardware was accessed or flashed.** These results do not establish LCD
orientation, physical wheel direction/feel, successful card I/O or simultaneous
radio/peripheral operation. Follow the hardware acceptance procedure in
[README.md](README.md). The first display backend preserves the existing
128×64 monochrome UI, scaled 1.5× to a centered 192×96 region. Native full-screen
layouts are not implemented; further layout work should retain black-and-white
rendering. Noninteger scaling and the letterbox margins still need visual review.

The production repository still needs the prior platform/radio port integrated
before ordinary `tools/build_board.sh` can build this board. This experiment is
a reproducible integration build on the preceding verified connectivity copy;
it is not a production OTA or companion-update qualification.
