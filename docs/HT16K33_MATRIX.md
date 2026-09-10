# HT16K33 monochrome matrix

The Adafruit 16x8 backpack is supported as the `matrix` command module. It is
independent of the main OLED/TFT display. The current custom build enables it
with `CUSTOM_ENABLE_LED_MATRIX=1`; full I2C builds include it, while disabled,
OLED-only and standalone I2C builds omit it.

## Wiring and address

For the assembled Adafruit monochrome backpack, connect VCC to 3.3 V, GND to
GND, DAT/SDA to SDA and CLK/SCL to SCL. The QT Py ESP32 profile uses its STEMMA
QT pins GPIO22 (SDA) and GPIO19 (SCL), exposed as I2C1 / bus 0.

The default address is `0x70`. Solder jumpers select `0x70..0x77`. A PCA9685
on the same bus also responds to `0x70` by default; choose an unused address
such as `0x71` for the matrix in that case. Discovery identifies the configured
address by expected device type; an address response is not a chip-ID check.

Sources: [Adafruit matrix guide](https://learn.adafruit.com/adafruit-led-backpack?view=all),
[address jumpers](https://learn.adafruit.com/adafruit-led-backpack/changing-i2c-address),
[PCA9685 coexistence](https://learn.adafruit.com/16-channel-pwm-servo-driver/faq).

## Commands

```text
matrix status
matrix test
matrix clear
matrix pixel 3 2 on
matrix text HI
matrix brightness 4
matrix rotation 1
matrix blink 0
matrix off
matrix on
```

`clear` blanks pixels; `fill` lights all pixels in the active canvas. `off`
disables output while retaining the in-memory picture; `on` restores it.
Brightness is global, 0..15 (0 means dimmest, not off). Blink values are
0=steady, 1=2 Hz, 2=1 Hz, 3=0.5 Hz. Text is static and clips at the edge;
quote text containing spaces. Content and blink state do not persist over reboot.

## Use just one 8x8 square

```text
matrix size 8x8
matrix panel 0
matrix test
matrix text A
```

Use `matrix panel 1` to select the other physical square. The unused square is
kept blank. In this mode both coordinates range from 0 to 7; rotation stays
within the chosen square. Return to both squares with `matrix size 16x8`.
In full mode rotations 1/3 are 16x8 and rotations 0/2 are 8x16. Changing size,
panel, or rotation clears the previous picture.

Size, panel, brightness, and rotation commands apply immediately and persist;
they require an administrator. The same settings are available in the `matrix`
settings module. Generic settings edits apply on the next drawing/control
command. Bus/address settings take effect after reboot:

```text
matrixbus 0
matrixaddress 0x71
```

The in-memory framebuffer starts blank and needs no polling task. The first
drawing/control command programs the backpack; rebooting only the MCU can leave
the previous physical image visible until then. The driver writes the complete
16-byte frame through the shared bus transaction manager and checks every
I2C write result. A failed update reports an error; the next successful update
reasserts the full frame and controller settings. A failure partway through an
update may leave a partial picture on the physical display.

## Build and initial provisioning

```sh
source /Users/morgan/esp/esp-idf/export.sh
tools/build_board.sh qtpy_esp32 build
```

The ordinary QT Py build uses its own build directory and sdkconfig. After an
authorized full erase, program the bootloader, partition table, application,
and the generated blank LittleFS image (`flash littlefs-flash`). First boot
opens the serial setup wizard at 115200 baud. An existing installation should
normally receive only the firmware update; flashing blank LittleFS destroys
its files and accounts.
