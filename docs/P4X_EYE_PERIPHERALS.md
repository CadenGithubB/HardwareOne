# ESP32-P4X-EYE optional peripherals

The built-in display, microSD slot, clickable wheel and three user buttons use
HardwareOne's existing display/input interfaces and VFS. Camera support remains disabled.
Select the board explicitly: a generic ESP32-P4 target does not imply EYE wiring.

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
A feature overlay selects the devices independently:

```c
#define HW_BOARD_P4X_EYE 1
#undef DISPLAY_TYPE
#define DISPLAY_TYPE 4
#undef INPUT_DEVICE_TYPE
#define INPUT_DEVICE_TYPE 3
#undef ENABLE_SDMMC_CARD
#define ENABLE_SDMMC_CARD 1
```

`DISPLAY_TYPE=0`, `INPUT_DEVICE_TYPE=0`, and `ENABLE_SDMMC_CARD=0` remove those
devices independently. `ENABLE_SD_CARD=0` can force-disable removable storage.
These devices work with `I2C_FEATURE_LEVEL=0`; no I2C sensor task is needed.
Existing SPI-card boards retain their `SD_CS_PIN` backend and existing Seesaw
gamepad/ANO input selections keep their numeric values.

`ENABLE_GPIO_ENCODER_BUTTONS` defaults to 1 for the EYE's GPIO encoder profile.
Set it to 0 to retain the wheel without claiming the three extra button pins.
The buttons share the input lifecycle and task; disabling the input controller
also disables them. Explicitly enabling them requires both the EYE board and
the GPIO encoder input selection.

The production checkout does not yet include the preceding P4 radio/platform
port. Use the reproducible [peripheral build](../experiments/p4_peripherals/README.md)
on top of the verified connectivity experiment. Its profile keeps HTTP,
Bluetooth and ESP-NOW available and camera/microphone off. A normal board build
must incorporate that separate platform port before these peripherals can run.

## User interface and storage

The display adapter preserves the existing 128×64 monochrome framebuffer and
all current menus. It renders that framebuffer at 1.5× as 192×96 pixels, centered on
the 240×240 panel. The UI deliberately remains black and white: the adapter
outputs only black and white pixels, with no grayscale or color UI. The panel's
RGB565 transport does not change the one-bit drawing model used by screens.
Nearest-neighbor enlargement preserves hard edges and the original aspect ratio,
but its fractional scale alternates one- and two-pixel widths. There are 24-pixel
black margins on the left/right and 72-pixel margins above/below. The borders
stay black when the image is inverted. This is a compatibility layout whose appearance
has not yet been verified on hardware. A future native-resolution layout should
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

Cards mount at `/sd`. Existing `sdmount`, `sdunmount`, `sdinfo`, `sddiag` and
permission-checked file commands use the selected storage backend. LittleFS
remains the source of settings and account data. An absent or unrecognized card
does not prevent boot, and ordinary mounting **never formats a card**.
`sdformat confirm` remains an explicitly destructive operation. Stop writers and close files, then unmount before
removing a card; removal detection blocks new VFS use and requires remounting.

## Qualification

Host tests exercise real configuration derivation, rotary decoding/gestures,
framebuffer conversion and SD lifecycle code with controlled hardware substitutes.
Firmware build and hardware qualification results are recorded with the
peripheral experiment. Compilation does not establish display orientation,
physical detents, card signal integrity or radio coexistence on a live board.
