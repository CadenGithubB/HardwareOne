# ESP32-P4X-EYE

The ESP32-P4X-EYE is a supported HardwareOne board (`HW_BOARD=p4x_eye`). Its
display, clickable wheel, three user buttons, MIPI camera, PDM microphone,
battery monitor and microSD slot use HardwareOne's existing shared interfaces.
The P4 has no radio: Wi-Fi, Bluetooth LE (G2 glasses, R1 ring, phone app) and
ESP-NOW mesh run on the board's ESP32-C6 companion through ESP-Hosted.
Select the board explicitly: a generic ESP32-P4 target does not imply EYE wiring.

## Building

With ESP-IDF 5.5.5 exported and the vendored Arduino core carrying the
[local patches](arduino-local-patches/README.md) (including the P4 set):

```sh
tools/p4/prepare_sdk.sh          # once: stages patched IDF bt + JPEG components
tools/build_board.sh p4x_eye     # -> build-p4x_eye/
tools/build_board.sh p4x_eye flash monitor
```

`prepare_sdk.sh` copies two ESP-IDF components into the ignored
`.sdk-overrides/esp32p4/` and patches the copies; the SDK itself is only read.
`bt` gets the upstream TinyCrypt ECC fix that BLE Secure Connections needs on P4
([esp-idf#19002](https://github.com/espressif/esp-idf/issues/19002)), and
`esp_driver_jpeg` gets an allocation/interrupt-unwind fix that enables the
hardware JPEG decoder. A P4 build stops with that command if they are missing.
Other boards are unaffected.

The build combines `config/sdkconfig.defaults`, the chip-family
`config/sdkconfig.defaults.esp32p4`, `boards/p4x_eye.defaults` (C6 wiring, flash
size, camera sensor) and the application feature profile
`boards/p4x_eye.features.h`. The image uses its own 16 MB layout,
`partitions/partitions_no_sr_16mb_p4x_eye.csv` (7 MB factory partition,
LittleFS at `0x725000`, ESP-SR and STT models in LittleFS); a board flashed
with the earlier shared table needs one full reflash, which reformats its data
partition. There is no recovery-OTA layout for this board yet; flash it over
USB.

## Radio companion (ESP32-C6)

`components/hardwareone/HAL_Radio.cpp` prepares the C6 at startup: C6 boot
strap high on GPIO33, SDIO slot 1 on GPIO27-32, enable/reset on GPIO9.
Arduino's Wi-Fi and BLE then run unchanged through `esp_wifi_remote` and
Bluedroid over Hosted VHCI. The BLE TX-power setting is not applied on this
board because Hosted has no remote power control; the C6 uses its default.

ESP-NOW needs more than stock Hosted: `components/esp_now_hosted` forwards each
`esp_now_*` call to the C6, which must run the companion firmware (the
ESP-Hosted slave pinned in
`components/esp_now_hosted/include/esp_now_hosted_companion.h` plus the
HardwareOne bridge). Build, back up and flash it with
[tools/p4/companion](../tools/p4/companion/README.md); both USB ports lead to
the P4, so the C6 is programmed through a temporary P4 service image, and the
procedure backs up the original C6 flash first. Once a bridge-carrying image
runs, later companion images can be sent over SDIO with `c6update`.

The companion is a managed subsystem (`System_RadioCompanion`), present only
in P4 images; every other board compiles it to nothing.

- **Boot.** A C6 that does not answer is not fatal. The P4 boots without
  Wi-Fi, BLE and ESP-NOW, says so, and keeps trying in the background with a
  growing back-off (5 s, 15 s, 1 min, 5 min, 15 min). When the companion
  answers, the features configured to auto-start are started then.
- **Status.** `c6status [json]` reports the companion's ESP-Hosted version
  against the qualified one, its image (project, version, build date, running
  slot, confirmed or pending), the ESP-NOW bridge state, heartbeat, memory,
  bridge counters and recovery counters. `/api/system` carries the same
  summary under `companion`.
- **Events.** The "Radio companion" family of System Events
  (`companion_online`, `companion_offline`, `companion_restarted`,
  `companion_mismatch`, `companion_low_memory`, `companion_updated`,
  `companion_reboot`) goes through the notification system like every other
  family, so each kind can be routed or muted per surface and used in
  automations.
- **Watchdog and recovery.** The companion sends a heartbeat (`c6heartbeat
  <seconds>`, default 5, 0 disables). Three missed beats, an unexpected C6
  reset or an SDIO transport failure start a soft recovery that never reboots
  the P4: the radio users are stopped, the transport is rebuilt (which resets
  the C6), and what was running is restored. Only when soft recovery fails
  three times within ten minutes does the P4 reboot, after posting
  `companion_reboot`. `c6autorecover off` leaves every outage to the user;
  `c6restart` is the manual restart. P4 images set
  `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE=n` so ESP-Hosted itself
  never reboots the P4 for a transport failure.
- **Power.** The companion never acts on its own. `lightsleep` pauses the
  heartbeat watchdog across the sleep; `deepsleep` stops the radio users and
  holds the C6 in reset (GPIO9 low, held through sleep) because the next boot
  resets it anyway. `c6hold on` turns the radio fully off the same way at any
  time and `c6hold off` brings back what was running. With `c6autohold on`
  the P4 does that by itself after the radio has been idle for 30 s, and
  releases the companion when a feature asks for the radio.
- **Firmware updates.** `c6update "<file>"` streams an image from the P4's
  storage to the C6 over SDIO (ESP-Hosted OTA into the inactive ota slot).
  The file is checked first: an ESP32-C6 application image of at most 1920 KB
  that carries the bridge marker. ESP-NOW and BLE pause during the transfer.
  The companion is built with bootloader rollback enabled, and the new image
  is confirmed only after the bridge answers from it; otherwise the C6 returns
  to the previous image on its next reset. `c6confirm` accepts a pending
  image by hand.

The recovery, hold and update paths were built against the ESP-Hosted
2.12.13 host API with a host-tested policy core
(`test_radio_companion_core.cpp`); their first hardware runs are still to be
recorded.

The shared mesh code never queries the radio per message on this board: the
STA and AP identities are cached for the life of each ESP-NOW instance and
`esp_now_is_peer_exist()` is answered from the host's mirror of the C6 peer
table. Channel reads remain live driver calls on the few paths that change or
report the channel.

A reproducible release of this board is the factory-only deployment
`deployments/handheld/boards/p4x_eye` (`tools/build_deployment.sh handheld
p4x_eye`, no signing key); see its `MIGRATION.md` for the cable-flash
procedure. There is no recovery-OTA layout for the P4 yet.

## Hardware identified

| Device | Hardware and wiring |
| --- | --- |
| LCD | 1.54-inch 240×240 ST7789 SPI panel; SCLK 17, MOSI 16, CS 18, DC 19, reset 15, active-low backlight 20, enable 12 |
| microSD | Four-bit SDMMC **slot 0**; CLK 43, CMD 44, D0–D3 39–42, detect 45, active-low card power 46, internal I/O LDO channel 4 |
| Clickable wheel | SIQ-02FVS3 mechanical quadrature encoder; A 48, B 47, push switch 2 |
| User buttons | BUTTON_1 / SW3 on GPIO 3, BUTTON_2 / SW4 on GPIO 4, BUTTON_3 / SW5 on GPIO 5; active-low with external 10 kΩ pull-ups |

References: [Espressif P4X-EYE user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html),
[official BSP pin definitions](https://github.com/espressif/esp-bsp/blob/master/bsp/esp32_p4_eye/include/bsp/esp32_p4_eye.h),
[official BSP implementation](https://github.com/espressif/esp-bsp/blob/master/bsp/esp32_p4_eye/src/esp32_p4_eye.c),
[Mitsumi SIQ-02FVS3 datasheet](https://nmbtc.com/wp-content/uploads/2019/04/switch_siq_02fvs_e.pdf),
and [MB v2.3 schematic](https://dl.espressif.com/AE/esp-dev-kits/SCH_ESP32-P4-EYE-MB_V2.3_20250416.pdf).
The three user-button pins are confirmed by the [factory-demo BSP](https://github.com/espressif/esp-dev-kits/blob/master/examples/esp32-p4-eye/examples/common_components/esp32_p4_eye/include/bsp/esp32_p4_eye.h)
and the MB v2.4 schematic in Espressif's [P4X reference design archive](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/_downloads/0857ce9b0e3668c3e56170a8a595db07/ESP32-P4X-EYE-EN.zip).
BOOT and RESET are separate controls and are not registered as UI buttons.

GPIO12 supplies both the LCD and camera rail; display shutdown leaves that rail
asserted. The card bus is separate from the C6's Hosted SDIO **slot 1** on
GPIO27–32. SD mount failure, format and unmount must never deinitialize that
shared host globally.

## Optional feature selection

The board wiring lives in `components/hardwareone/System_Board_P4X_EYE.h`.
`boards/p4x_eye.features.h` selects the devices independently; it applies only
when this board is built. The shipped profile enables the display
(`DISPLAY_TYPE 4`), wheel and buttons (`INPUT_DEVICE_TYPE 3`), camera,
microphone, battery, BLE/G2/R1, Wi-Fi/web, ESP-NOW, ESP-SR and local
speech-to-text. The microSD card is off until it has been qualified with a card:

```c
#undef ENABLE_SDMMC_CARD
#define ENABLE_SDMMC_CARD 1
#undef ENABLE_SD_CARD
#define ENABLE_SD_CARD 1
```

`DISPLAY_TYPE=0`, `INPUT_DEVICE_TYPE=0`, and `ENABLE_SDMMC_CARD=0` remove those
devices independently. The image is close to its app-partition limit, so
turning on more features may not fit. `ENABLE_SD_CARD=0` can force-disable removable storage.
These devices work with `I2C_FEATURE_LEVEL=0`; no I2C sensor task is needed.
Existing SPI-card boards retain their `SD_CS_PIN` backend and existing Seesaw
gamepad/ANO input selections keep their numeric values.

`ENABLE_GPIO_ENCODER_BUTTONS` defaults to 1 for the EYE's GPIO encoder profile.
Set it to 0 to retain the wheel without claiming the three extra button pins.
The buttons share the input lifecycle and task; disabling the input controller
also disables them. Explicitly enabling them requires both the EYE board and
the GPIO encoder input selection.


## User interface and storage

The display adapter preserves the existing 128×64 monochrome framebuffer and
all current menus. It renders that framebuffer at 1.5× as 192×96 pixels, centered on
the 240×240 panel. The UI deliberately remains black and white: the adapter
outputs only black and white pixels, with no grayscale or color UI. The panel's
RGB565 transport does not change the one-bit drawing model used by screens.
Nearest-neighbor enlargement preserves hard edges and the original aspect ratio,
but its fractional scale alternates one- and two-pixel widths. There are 24-pixel
black margins on the left/right and 72-pixel margins above/below. The borders
stay black when the image is inverted. This compatibility layout, the wheel and
the three buttons were confirmed on hardware on 2026-09-27
([results](../experiments/p4_io/RESULTS.md)). A future native-resolution layout should
retain the monochrome design while adapting spacing and typography to the square
panel; it need not introduce color or change the existing OLED layouts.
The existing `oled*` commands, brightness, rotation, authentication and sleep
behavior operate through the backend. SPI transfers use a small DMA stripe
buffer rather than an additional full RGB framebuffer.

Wheel rotation scrolls through the same previous/next channel as ANO rotation.
A short click sends A/select, a 700 ms hold sends B/back, and a
double-click sends SELECT/function (quick settings or keyboard mode). Press-and-turn provides the secondary X/Y actions
(submit/delete where the active screen uses those actions; X also advances the
setup wizard). The joystick pattern keyboard is skipped on this wheel. A single click
waits for the 250 ms double-click window before being delivered. Gray-code
decoding rejects invalid phase jumps, and the switch has 20 ms debounce.
`openinput`, `closeinput`, `inputautostart` and `gpioencoderread` use the shared
input module. Button sampling is fixed at 5 ms; I2C polling settings do not slow
the interrupt-driven rotation capture.

| Physical button | Logical action | Existing UI use |
| --- | --- | --- |
| BUTTON_1 (GPIO 3) | X | Submit text; advance setup |
| BUTTON_2 (GPIO 4) | Y | Delete/backspace |
| BUTTON_3 (GPIO 5) | START | The active screen's standard START action, including submit/advance where supported |

These keys deliver a press after 20 ms of stable contact, without the wheel's
double-click delay. Their held state remains available to shared input consumers;
press edges are latched until consumed. Each key debounces independently, so
simultaneous keys and wheel operation are supported. Keys held when input starts
must first be released. `closeinput` releases their pins and clears pending presses;
`openinput` starts fresh. `gpioencoderread` includes their logical bits in `buttons`
and reports `auxButtonsEnabled`.

Cards mount at `/sd` and run at 40 MHz SD High Speed (`SD_MMC_MAX_FREQ_KHZ`), the clock Espressif's own P4-EYE BSP uses; a card without High Speed support is negotiated down to 20 MHz. Existing `sdmount`, `sdunmount`, `sdinfo`, `sddiag` and
permission-checked file commands use the selected storage backend. LittleFS
remains the source of settings and account data. An absent or unrecognized card
does not prevent boot, and ordinary mounting **never formats a card**.
`sdformat confirm` remains an explicitly destructive operation. Stop writers and close files, then unmount before
removing a card; removal detection blocks new VFS use and requires remounting.

## Power

CPU presets use the P4's PLL steps: Performance 400 MHz, Balanced 200 MHz and
PowerSaver 100 MHz (the interactive floor); UltraSaver drops to 40 MHz only
while idle power-save has blanked the display. `cpufreq` lists and accepts the
same 100/200/400 MHz steps. 100/200/400 MHz switching was verified on hardware;
idle 40 MHz, light/deep sleep and battery life have not been measured.

## Qualification

Host tests exercise real configuration derivation, rotary decoding/gestures,
framebuffer conversion and SD lifecycle code with controlled hardware substitutes.
Hardware results live with the experiments that produced this board support:
[connectivity](../experiments/p4_connectivity/README.md),
[BLE roles](../experiments/p4_ble_roles/README.md),
[mesh](../experiments/p4_mesh/README.md),
[display and input](../experiments/p4_io/RESULTS.md),
[camera](../experiments/camera_portable/RESULTS.md),
[JPEG](../experiments/jpeg_portable/RESULTS.md),
[microphone](../experiments/audio_portable/RESULTS.md),
[battery](../experiments/battery_portable/RESULTS.md) and
[speech-to-text](../experiments/stt_p4/README.md).

Not yet qualified: the microSD card (no card was fitted), battery-only
operation and power draw, idle/sleep clocks, C6 crash recovery, and long
endurance. Known issues: mesh pairing between two new devices saves only the
accepting peer, and cancelling an R1 connection can block the next BLE role
change until reboot ([details](../experiments/board_qualification/RESULTS.md)).
