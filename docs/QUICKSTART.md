# Quick Start Guide

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
3. Connect the Seesaw Gamepad via Stemma QT.
4. **Optional battery:** Connect a LiPo battery to the board's JST connector or BFF module if you want the device to run untethered. If you skip this, the board will be powered over USB.
5. **Optional battery:** Make sure the power switch is in the **Off** position before continuing.

> **Before powering on or plugging in:** double-check that all power and ground connections are correct. Reversing polarity can damage components or the battery.

6. Connect the board to your computer via USB-C.
7. Continue to Software Setup.




### Bonded Microcontrollers
With `ENABLE_BONDED_MODE=1`, two devices can bond into a paired set. One acts as the local controller (typically with OLED + gamepad), the other as the remote endpoint. The controller gains a **Remote** tab in its web UI showing the paired device's features, even if those features aren't compiled into the controller. Command registries are shared between the two, so either device can execute commands on the other transparently.

1. Set up the hardware for your specific needs, using the steps above. Do it twice, one for each of the bonded devices.
2. Complete two Software Setups, one for each Hardware Setup.
3. Im too lazy to write these steps now, but you pair them with espnow and then run the bond connect command and wait for the sync/handshake to finish and then its ready to go.

---

## Software Setup

Hardware One uses **ESP-IDF**, not Arduino IDE. If you don't have it installed:

- [ESP-IDF installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
- Install the version matching your target chip (ESP32 or ESP32-S3).

### 1. Clone the repo

```bash
git clone https://github.com/CadenGithubB/HardwareOne.git
cd HardwareOne
```

### 2. Configure your build (optional)

Open `components/hardwareone/System_BuildConfig.h` and enable or disable any features you want — sensors, web modules, ESP-NOW, MQTT, etc. The defaults are set for the standard full build. If you're happy with defaults, skip this step.

If you are using Bonded mode, ensure that you reconfigure, fullclean, and then compile the build again.

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

> You can also run `idf.py -p PORT flash monitor` directly — it will build automatically if anything has changed. The separate `build` step is useful if you want to confirm the build succeeds before connecting the device.

- **XIAO / QT PY ESP32-S3:** port is usually `/dev/cu.usbmodem*` (native USB). You may need to hold the BOOT button when initiating the flash.
- **QT PY ESP32 / Feather ESP32:** port is usually `/dev/cu.usbserial*` or `/dev/cu.SLAB*` (UART bridge).

That's it. The build can take a few minutes the first time.

---

## Switching Between Board Families

If you are switching between an **ESP32** board (QT PY, Feather) and an **ESP32-S3** board (XIAO, QT PY S3), a full clean is required — the two chip families have different architectures and the build cache is not compatible. This being said, once compiled the boards can be used together without issue. The only difference is setup.

```bash
idf.py fullclean
idf.py set-target esp32s3    # or esp32
idf.py -p PORT flash monitor
```

The project includes target-specific defaults (`sdkconfig.defaults.esp32` and `sdkconfig.defaults.esp32s3`) that handle the most important differences automatically, including:

- **PSRAM mode:** ESP32 uses Quad SPI PSRAM at 40 MHz; ESP32-S3 uses Octal SPI PSRAM at 80 MHz. Using the wrong mode will cause a boot failure or crash.
- **Flash mode:** ESP32 uses `DIO`; ESP32-S3 can use `QIO`.
- **Bluetooth:** ESP32 supports Classic BT + BLE 4.2; ESP32-S3 is **BLE 5.0 only** — no Classic Bluetooth.

If you need to deviate from the defaults, run `idf.py menuconfig` after `set-target`. See [BOARD_SWITCHING.md](BOARD_SWITCHING.md) for the full per-board menuconfig reference.

---

## First-Time Use

On first boot, the device detects that no users file exists and launches the setup wizard automatically. The wizard runs on **serial and OLED simultaneously** — use whichever is more convenient. Open the serial monitor at **115200 baud** to follow along or drive setup from your computer.

### Step 1 — Choose setup mode

You'll be prompted to choose:

- **Basic** — creates your admin account and uses default settings for everything else. Quickest way to get running.
- **Advanced** — runs the full configuration wizard after account creation, letting you configure features, sensors, WiFi, device name, and web UI theme.

### Step 2 — Create your admin account

Enter a username and password when prompted. These are your credentials for the web UI. Both fields are required and cannot be blank.

### Step 3 — Advanced wizard (Advanced mode only)

The wizard walks through seven pages:

1. **Features** — enable or disable network features (WiFi, HTTP server, Bluetooth, ESP-NOW)
2. **Sensors** — enable or disable I2C sensors and display
3. **Network** — auto-start options and device name
4. **System** — timezone and log level
5. **WiFi** — scan for networks and enter credentials. You can select by number, type an SSID directly, rescan, or skip.
6. **Device name** — sets the name used for Bluetooth and ESP-NOW identity (default: `HardwareOne`)
7. **Web UI theme** — choose Light or Dark

### Step 4 — Access the UI

> If you chose Basic mode or skipped WiFi during the wizard, the web server will not auto-start. This means that the Serial interface and the OLED interface (if connected) are the only ones available. Run `webstart` in the serial console to start it manually, or `webauto on` to enable auto-start on every boot.

> If you chose Advanced mode and enabled Wifi during the wizard, the device will connect to WiFi and prints its IP address in the serial monitor. Navigate to that address in a browser to access the web UI. Use the username and password entered in the first time setup to login.

Type `help` at any time in the serial console to see all available commands.

---

> ## Back to the overview: [README](../README.md)

> ## Full reference, commands, and configuration: [User Guide](USERGUIDE.md)

