# Waveshare P4 support investigation

Initial inspection: 2026-11-01. Branch: `codex/waveshare-p4-support`,
created from `40261a39`. Existing uncommitted STT/G2/OpenClaw work was
preserved in the checkout; it is not part of this investigation's changes.

## Device identity and observed factory boot

- USB hub device: `/dev/cu.usbmodem5ABA0541421`, USB serial `5ABA054142`,
  VID:PID `1a86:55d3`, USB name `USB Single Serial`.
- Existing STT board remains separate: USB serial `E8:F6:0A:E3:A7:68`,
  `/dev/cu.usbmodem101`. Do not use that port for this board's work.
- Read the new device at 115200 baud with DTR/RTS initially false. A complete
  boot sequence appeared during the read; opening the bridge may have reset
  the device. No commands, flash writes or erase operations were issued.
- Factory firmware identifies `Waveshare ESP32-P4-WIFI6-Touch-LCD-5`.
- ESP32-P4 revision v1.3; runtime CPU frequency 360 MHz.
- 32 MB PSRAM, 200 MHz; memory test passed. Flash vendor reports `gd`, QIO;
  flash capacity has not yet been measured.
- Application `phone_p4_function_ev_board`, version
  `v0.4.2-113-ge0e613f-dirty`, compiled Jan 27 2026.
- Factory SDK `v5.5.2-249-gf56bea3d1f-dirty`.
- HX8394 MIPI DSI display initialized; demo stylesheet 720x1280.
- GT911 touch initialized at I2C address 0x5d.
- ES8311 output codec and ES7210 microphone codec initialized; MIC1/MIC2,
  16 kHz, 16-bit, two channels reported.
- ESP-Hosted SDIO initialized successfully: CLK18, CMD19, D0=14, D1=15,
  D2=16, D3=17, companion reset GPIO54, four-bit bus at 40 MHz.
- Console UART uses GPIO38/37.

These observations establish factory peripheral initialization, not Hardware
One qualification. Exact PCB revision, physically measured flash size, remaining wiring and radio
firmware compatibility still need verification.

## Integration approach

Use the separate board identity `waveshare_p4_touch_lcd_5`, following
CONTRIBUTING.md. The existing `boards/p4x_eye.defaults` has different Hosted
SDIO pins (27–32, reset9) and must not be used unchanged on this board.

1. Verify schematic/BSP pin ownership, flash geometry, SD slot, audio clocks,
   display reset/backlight and touch wiring against the actual board.
2. Add board defaults and pin definitions through the existing board-selection
   mechanism, plus a minimal saved configuration for serial bring-up.
3. Reuse P4 services and HAL interfaces; add the appropriate display/touch and
   codec backends without making shared services depend on the board name.
4. Verify SDK support for silicon revision v1.3 and compatibility with the
   factory radio firmware. Preserve recovery inputs before firmware changes.
5. Build the new profile and a P4X-EYE baseline, then qualify serial, memory,
   radio, display/touch and audio incrementally on this device.

The user physically confirmed the **5-inch model** on 2026-11-01. The pasted
4.3-inch product page was a different variant; its ST7701 display parameters
must not be used here.

## Initial implementation

- `boards/waveshare_p4_touch_lcd_5.defaults`: rev1.x silicon, 360 MHz,
  32 MB flash, shared P4 PSRAM policy (200 MHz), UART0 GPIO37/38 console,
  C6 SDIO on GPIO14–19 and reset54. Radio clock starts at 20 MHz.
- Board feature header and System_BuildConfig identity: no EYE display,
  encoder, PDM microphone, battery ADC or SD power/detect assumptions.
- `configurations/waveshare-p4-bringup.json`: serial, Wi-Fi, basic web and BLE
  bring-up. No ESP-NOW bridge is assumed in the factory companion firmware.
- A factory-only 32 MB partition layout with an 8 MB app slot and remaining
  LittleFS space. This is not the vendor layout and has no recovery-OTA contract.
- `HAL_Radio` now reads SDIO/reset pins from the selected SDK board defaults;
  optional companion boot/UART wiring belongs to the P4X-EYE board header.
  Console mirroring reports unsupported on the Waveshare instead of touching
  EYE pins. EYE pins and behavior are preserved for its own profile.

Build with the existing setup tool after exporting ESP-IDF 5.5.5 and staging
`tools/p4/prepare_sdk.sh`:

```sh
python3 tools/build_config.py build waveshare-p4-bringup
```

This is **initial headless support**, not complete peripheral support. The
HX8394 DSI display, GT911 touch, ES8311/ES7210 audio and optional OV5647 need
separate backend integration. SD qualification remains deferred under the
project guidance. Do not enable the EYE drivers to substitute for these.
The factory radio's version compatibility remains unqualified.

No device was flashed. Before deploying the new layout, preserve the complete
factory flash and establish recovery, verify actual capacity and companion
compatibility, then explicitly authorize the layout-changing flash.

Build directories are isolated by saved profile; shared SDK override and
managed dependency directories belong to this checkout. No source snapshot
or extra worktree was created.

## Vendor references

- https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-5
- https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5
- https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-5/ESP32-P4-WIFI6-Touch-LCD-5-Schematic.pdf

The official repository provides examples, schematic and factory firmware;
inspect those sources before implementing pin assignments beyond the observed
serial log.

## Validation completed 2026-11-01

- `waveshare-p4-bringup`: ESP-IDF 5.5.5 build and link passed. Image
  2,095,664 bytes; 8,388,608-byte app partition; 6,292,944 bytes free.
  App SHA-256: `03706eb4e9a2ef123e759395c30773d5ba0c54635e4603a54ed9a82f0ffd5192`.
- `p4-glasses-test` regression build and link passed: 5,470,848-byte image,
  1,955,200 bytes free. Resolved EYE clock=400 MHz, SDIO CLK28/reset9;
  Waveshare clock=360 MHz, SDIO CLK18/reset54. EYE console mirror wiring
  remains GPIO33/35/36. No physical EYE regression test was performed.
- 13 optional-peripheral configuration tests passed, including Waveshare
  identity/defaults and rejection of EYE-only display/encoder/SD choices.
- 21 saved-configuration tests and 10 SDK-policy tests passed.
- Explicit checks of the generated Waveshare SDK configuration passed for
  silicon revision, flash/PSRAM, UART and all SDIO/reset pins.
- Existing dependency/compiler warnings remain; no build errors. No flash or
  runtime Hardware One qualification was performed on either device.

Local evidence is in `work/waveshare-p4/` (logs, build manifests, resolved SDK
settings and image hashes). The selected Waveshare build, including ELF and
partition/bootloader binaries, is retained in
`build-config-waveshare-p4-bringup/` (about 858 MiB) for incremental bring-up.
The temporary P4X-EYE regression build was removed after preserving its
manifest/configuration/hash, recovering 1,081,741,312 allocated bytes (about
1.01 GiB). Shared patched SDK sources (134 MiB) and managed dependencies
(426 MiB) remain for builds in this checkout. No additional cleanup is pending.
