# HardwareOne v0.99.81 - Quick Start Guide

This guide will help you get up and running with Hardware One.

## Hardware Setup

Choose the setup that matches your deployment type. All types use the same Software Setup steps that follow.

### Barebones / Headless Node
1. Just your board and a USB-C cable.
2. Connect it to your computer and continue to Software Setup.

### Sensor Appliance

> **NOTE:** This assumes you have already soldered headers to any modules that require them.

1. Connect your I2C sensors and peripherals via Stemma QT cables or GPIO headers.
2. **Optional battery:** Connect a LiPo battery to the board's JST connector or BFF module if you want the device to run untethered. If you skip this, the board will be powered over USB.
3. **Optional battery:** Make sure the power switch is in the **Off** position before continuing.

> **Before powering on or plugging in:** double-check that all power and ground connections are correct. Reversing polarity can damage components or the battery.

4. Connect the board to your computer via USB-C.
5. Continue to Software Setup.

### Standard Handheld

1. Connect your I2C sensors and peripherals via Stemma QT cables and GPIO headers. 
2. Connect the SSD1306 OLED display via I2C.
3. Connect your input device via Stemma QT - either the Seesaw Gamepad (`INPUT_DEVICE_TYPE=1`, the default) or the ANO rotary encoder (`INPUT_DEVICE_TYPE=2`). Only one: they share the bus and the build compiles exactly one driver.
4. **Optional battery:** Connect a LiPo battery to the board's JST connector or BFF module if you want the device to run untethered. If you skip this, the board will be powered over USB.
5. **Optional battery:** Make sure the power switch is in the **Off** position before continuing.

> **Before powering on or plugging in:** double-check that all power and ground connections are correct. Reversing polarity can damage components or the battery.

6. Connect the board to your computer via USB-C.
7. Continue to Software Setup.




### Wearable Companion (G2 glasses / R1 ring)

Needs no extra wiring - the glasses and ring are BLE peripherals. Build with
`ENABLE_BLUETOOTH=1` and `ENABLE_G2_GLASSES=1` (and `ENABLE_R1_HEALTH=1`, the
default, for ring vitals), then after Software Setup:

1. `openg2` to connect the glasses. Tap a temple to open the on-lens menu.
2. Pair the R1 ring from the OLED (**Connect → Bluetooth → R1 Ring**) or the web **Bluetooth** page, then `ringconnect`.
3. `bleautoreconnect r1-ring on` makes the ring reconnect at boot and reseek after unexpected drops.
4. `healthtrack on` starts background vitals logging.

Both return OK when the connection is *started*, not finished - give them a
moment before expecting status to read connected.

### Bonded Microcontrollers
With `ENABLE_BONDED_MODE=1`, two devices can bond into a paired set. One acts as the local controller (typically with OLED + gamepad), the other as the remote endpoint. The controller gains a **Remote** tab in its web UI showing the paired device's features, even if those features aren't compiled into the controller. Command registries are shared between the two, so either device can execute commands on the other transparently.

1. Set up the hardware for your specific needs, using the steps above. Do it twice, one for each of the bonded devices.
2. Complete two Software Setups, one for each Hardware Setup.
3. Im too lazy to write these steps now, but you pair them with espnow and then run the bond connect command and wait for the sync/handshake to finish and then its ready to go.

---

## Software Setup

Hardware One requires **ESP-IDF v5.5.1** (not the Arduino IDE). The firmware vendors
arduino-esp32 3.3.5 (supports IDF ≥5.3,<5.6); **v5.5.1 is the validated pairing** - older
5.3.x predates the `i2c_master` `Wire` HAL this build relies on. If you don't have it installed:

- [ESP-IDF v5.5.1 install guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32/get-started/index.html)
- In your esp-idf checkout: `git checkout v5.5.1 && git submodule update --init --recursive && ./install.sh && . ./export.sh`

### 1. Clone the repo

```bash
git clone https://github.com/CadenGithubB/HardwareOne.git
cd HardwareOne
```

### 2. Configure your build (optional)

Open `components/hardwareone/System_BuildConfig.h` and enable or disable any features you want - sensors, web modules, ESP-NOW, MQTT, etc. The defaults are set for the standard full build. If you're happy with defaults, skip this step.

If you are using Bonded mode, ensure that you reconfigure, fullclean, and then compile the build again.

**On-device LLM (`ENABLE_ONDEVICE_LLM`):** Enabled by default on ESP32-S3 builds. To use it, place an LLM1-format model file at `/system/llm/model.bin` on the device's LittleFS filesystem, or on an SD card at `/sd/llm/<filename>.bin`. You can copy files to LittleFS via the web UI's file manager or the `filecreate`/`fileview` CLI commands. Once a model file is in place, load it with `llmload` from the serial console or the **LLM** tab in the web UI. If you are not using the LLM feature, set `ENABLE_ONDEVICE_LLM=0` to save PSRAM.

### 3. Set your target board and flash

```bash
# Set the chip target (do this once, or any time you switch chip families)
idf.py set-target esp32s3    # XIAO ESP32-S3 or QT PY ESP32-S3
idf.py set-target esp32      # Adafruit QT PY ESP32 or Feather ESP32

# Build
idf.py build

# Flash and open serial monitor (replace PORT with your device's port)
idf.py -p PORT flash monitor
```

> You can also run `idf.py -p PORT flash monitor` directly - it will build automatically if anything has changed. The separate `build` step is useful if you want to confirm the build succeeds before connecting the device.

- **XIAO / QT PY ESP32-S3:** port is usually `/dev/cu.usbmodem*` (native USB). You may need to hold the BOOT button when initiating the flash.
- **QT PY ESP32 / Feather ESP32:** port is usually `/dev/cu.usbserial*` or `/dev/cu.SLAB*` (UART bridge).

That's it. The build can take a few minutes the first time.

---

## Switching Between Board Families

If you are switching between an **ESP32** board (QT PY, Feather) and an **ESP32-S3** board (XIAO, QT PY S3), a full clean is required - the two chip families have different architectures and the build cache is not compatible. This being said, once compiled the boards can be used together without issue. The only difference is setup.

```bash
idf.py fullclean
idf.py set-target esp32s3    # or esp32
idf.py -p PORT flash monitor
```

Two layers of defaults are applied for you, and it helps to know which is which.

**Per chip family** - `sdkconfig.defaults.esp32` / `sdkconfig.defaults.esp32s3`, picked automatically by `set-target`:

- **PSRAM speed:** ESP32 runs PSRAM at 40 MHz; ESP32-S3 at 80 MHz.
- **Flash mode:** ESP32 uses `DIO`; ESP32-S3 uses `QIO`.
- **Bluetooth:** ESP32 supports Classic BT + BLE 4.2; ESP32-S3 is **BLE 5.0 only** - no Classic Bluetooth.

**Per board** - `boards/<name>.defaults`, layered on top of the family file. This is where the Arduino pin variant, flash size, partition table, and **PSRAM mode** come from:

| Board | Board file | PSRAM mode | Flash |
| ----- | ---------- | :--------: | :---: |
| Unexpected Maker FeatherS3 | `feathers3` | **Quad** | 16 MB |
| Seeed XIAO ESP32-S3 | `xiao_s3` | **Octal** | 8 MB |
| Adafruit QT Py ESP32 | `qtpy_esp32` | **Quad** | 8 MB |
| Adafruit Feather ESP32 V2 | `feather_esp32_v2` | **Quad** | 8 MB |

> **PSRAM mode is a property of the board, not of the chip family.** Both ESP32-S3
> boards here use different modes - the XIAO is octal, the FeatherS3 is quad. Do
> not assume "S3 means octal". Setting the wrong mode does **not** fail loudly:
> the device boots normally and reports 0 KB PSRAM, so the first symptom is the
> LLM, large web responses, or ESP-NOW buffers running out of memory at runtime.

You do not normally set any of this by hand - selecting the right board file does
it. If you do need to deviate, run `idf.py menuconfig` after `set-target`. See
[BOARD_SWITCHING.md](BOARD_SWITCHING.md) for the full per-board menuconfig reference.

---

## First-Time Use

On first boot, the device detects that no users file exists and launches the setup wizard automatically. The wizard runs on **serial and OLED simultaneously** - use whichever is more convenient. Open the serial monitor at **115200 baud** to follow along or drive setup from your computer.

### Step 1 - Choose setup mode

You'll be prompted to choose one of three modes:

- **Basic** - creates your admin account and uses default settings for everything else. Quickest way to get running.
- **Advanced** - runs the full configuration wizard after account creation.
- **Import from Backup** - restores settings from a `.hwbackup` file instead of configuring by hand. Only offered on builds with WiFi and the migration tool compiled in.

### Step 2 - Create your admin account

Enter a username and password when prompted. These are your credentials for the web UI. Both fields are required and cannot be blank.

### Step 3 - Advanced wizard (Advanced mode only)

The wizard has up to nine pages. Five always appear; four are shown only when
the relevant feature is enabled, so the page count you see (`SETUP 3/6` etc.)
depends on what you turned on earlier in the wizard.

| # | Page | Shown when | Contents |
| - | ---- | ---------- | -------- |
| 1 | **Features** | always | Enable/disable network features - WiFi, Web Interface, Bluetooth, ESP-NOW - each with a rough heap cost |
| 2 | **Web Mode** | Web Interface enabled on page 1 | `HTTP` or `HTTPS` (HTTPS costs ~20 KB more RAM; picking it generates a self-signed cert) |
| 3 | **Bluetooth Mode** | Bluetooth enabled on page 1 | `Server (phone)` or `G2 Glasses` (~10 KB more RAM) |
| 4 | **Sensors & Display** | always | Enable/disable the display and each I2C sensor |
| 5 | **Startup & Auto-start** | always | WiFi auto-connect, ESP-NOW mesh, MQTT auto-start, plus per-sensor auto-start |
| 6 | **System Settings** | always | Timezone, log level, NTP server, LED startup effect |
| 7 | **ESP-NOW Identity** | ESP-NOW enabled | Device name (used for **both** Bluetooth and ESP-NOW, default `HardwareOne`), room, zone, mobile/stationary. All optional |
| 8 | **MQTT Broker** | MQTT enabled on page 1 **and** MQTT auto-start on page 5 | Broker host, port, username, password |
| 9 | **WiFi Setup** | WiFi enabled | Scan and join. Select by number, type an SSID directly, `rescan`, or `skip` |

On any page, `b` / `back` returns to the previous one.

> **Skipping the ESP-NOW Identity page turns ESP-NOW off.** Answering `n` (or
> pressing Enter) at that page's prompt sets `espnowEnabled = false`, because the
> mesh cannot start without a device name. If you want ESP-NOW, answer `c` and
> at minimum accept the default name. You can set it later with `espnowsetname`
> and re-enable with `espnowenabled 1`.

There is no theme page in the wizard - the web UI theme is chosen later from the
web **Settings** page.

### Step 4 - Access the UI
> If you chose Basic mode, or turned the Web Interface off, or skipped WiFi during the wizard, the web server will not come up on its own. The Serial interface and the OLED interface (if connected) are then the only ones available. Run `openhttp` in the serial console to start the server for this session, or `httpAutoStart 1` to have it start on every boot.
> If you enabled the Web Interface and joined a network during the wizard, the device connects to WiFi and prints its IP address in the serial monitor. Navigate to that address in a browser to reach the web UI, and log in with the username and password you created in Step 2.

Type `help` at any time in the serial console to see all available commands.

---

> ## Back to the overview: [README](../README.md)

> ## Full reference, commands, and configuration: [User Guide](USERGUIDE.md)
