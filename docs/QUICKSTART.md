# HardwareOne v0.99.92 - Quick Start Guide

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
3. Connect your input device via Stemma QT - either the Seesaw Gamepad or the ANO rotary encoder - and set `INPUT_DEVICE_TYPE` in `System_BuildConfig.h` to match your wiring: `1` for the Seesaw Gamepad, `2` for the ANO rotary encoder, `0` for no input device at all. Wire and select exactly one: they share the bus, and the build compiles exactly one driver.
4. **Optional battery:** Connect a LiPo battery to the board's JST connector or BFF module if you want the device to run untethered. If you skip this, the board will be powered over USB.
5. **Optional battery:** Make sure the power switch is in the **Off** position before continuing.

> **Before powering on or plugging in:** double-check that all power and ground connections are correct. Reversing polarity can damage components or the battery.

6. Connect the board to your computer via USB-C.
7. Continue to Software Setup.




### Wearable Companion (G2 glasses / R1 ring)

Needs no extra wiring - the glasses and ring are BLE peripherals. In
`System_BuildConfig.h`, set `ENABLE_BLUETOOTH` to `1` to build the BLE stack,
`ENABLE_G2_GLASSES` to `1` for the glasses client, and `ENABLE_R1_HEALTH` to `1`
for ring vitals - R1 rides the G2 BLE client, so it needs the other two on as
well. Pick a board that keeps the BT stack: `qtpy_esp32` and `xiao_s3` set
`CONFIG_BT_ENABLED=n`, and the header's derived rules then force
`ENABLE_BLUETOOTH` - and G2/R1 with it - back to `0` whatever you wrote. Then,
after Software Setup:

1. `openg2` to connect the glasses. Tap a temple to open the on-lens menu.
2. Pair the R1 ring from the OLED (**Connect → Bluetooth → R1 Ring**) or the web **Bluetooth** page, then `ringconnect`.
3. `bleautoreconnect r1-ring on` makes the ring reconnect at boot and reseek after unexpected drops.
4. `healthlogging on` starts background vitals logging; `healthlogging interval <sec>` sets how often it polls the ring (default 900).

Both return OK when the connection is *started*, not finished - give them a
moment before expecting status to read connected.

### Bonded Microcontrollers
With `ENABLE_BONDED_MODE=1`, two devices can bond into a paired set. One acts as the local controller (typically with OLED + gamepad), the other as the remote endpoint. The controller gains a **Remote** tab in its web UI showing the paired device's features, even if those features aren't compiled into the controller. Command registries are shared between the two, so either device can execute commands on the other transparently.

1. Set up the hardware for your specific needs, using the steps above. Do it twice, one for each of the bonded devices.
2. Complete two Software Setups, one for each Hardware Setup.
3. Bring ESP-NOW up on both devices (`openespnow`) and give them the same mesh: `espnowsetpassphrase <mesh> <passphrase>`, using an identical mesh name and passphrase on each. Bonding runs over ESP-NOW, so this comes first.
4. Pair them. Run `espnowpairmode` on both to open the discovery window, `espnowdiscovered` to see the other device, then `espnowpairrequest <mac_or_name>` on one and `espnowaccept` on the other. `espnowlist` should now show the peer on both sides.
5. Run `bondconnect <mac_or_name>` on **both** devices. You do not choose the roles: they are assigned deterministically by comparing the two MAC addresses, and the higher MAC becomes the master. `bondstatus` reports which side you ended up on.
6. `bondconnect` returns immediately - it only enables bond mode. The bond establishes once each device sees the other's heartbeat and the capability, manifest and settings exchange completes. Watch `bondstatus` until the peer reads `ONLINE` and sync reads `SYNCED`; the **Remote** tab is live at that point. If it stalls, `bondresync` re-runs the exchange.

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

Open `components/hardwareone/System_BuildConfig.h` and enable or disable any features you want - sensors, web modules, ESP-NOW, MQTT, etc. This header is *your* configuration surface: every `ENABLE_*` flag is a plain `0` / `1` that you set for your own device. The values checked into the repo are whatever the last person building from this tree happened to need - treat them as a starting point, not as a contract, and read through the file once to set the flags your hardware and use case actually call for.

If you are using Bonded mode, ensure that you reconfigure, fullclean, and then compile the build again.

**LLM assistant (`ENABLE_LLM_BACKEND`):** `ENABLE_LLM_BACKEND` is the master switch for the feature. Set it to `1` and turn on at least one answer source alongside it - `ENABLE_LLM_SOURCE_ONBOARD` (tiny transformer inference on this chip; ESP32-S3 with PSRAM only, and by far the largest part of the feature's flash) or `ENABLE_LLM_SOURCE_CM5` (a CM5 / Pi 5 co-processor reached over the UART host link, which costs almost no flash because the model runs on the other end). The header refuses to compile the two mismatched combinations - the backend with no source, and a source with no backend - so you get a clear `#error` rather than a strange build. Keep all three as plain literals - CMake greps those exact lines to decide which files reach the compiler. For the on-board source, put a model file on the device's LittleFS under `/system/llm/` or on an SD card under `/sd/llm/`; `llmload <filename.bin>` looks a bare filename up internally first and then on the SD card, and `llmmodels` lists what it can see. You can copy files to LittleFS via the web UI's file manager or the `filecreate`/`fileview` CLI commands.

### 3. Set your target board and flash

Use the per-board wrapper. Every board gets its own build directory and its own `sdkconfig`, so you never have to clean between boards.

```bash
# Build (board name = a boards/<name>.defaults file, minus the .defaults suffix)
tools/build_board.sh feathers3                 # -> build-feathers3/

# Flash and open serial monitor (replace PORT with your device's port)
tools/build_board.sh feathers3 -p PORT flash monitor
```

The wrapper reads the chip target from the board file's `# HW_TARGET:` marker and always passes `HW_BOARD`, so there is no separate `set-target` step. Any other idf.py action (`monitor`, `menuconfig`, `fullclean`, …) passes straight through. Run `tools/build_board.sh` with no arguments to list the boards it knows about.

> **`boards/*.ota.defaults` files are not boards.** They are OTA-only sdkconfig
> *overlays* - a handful of chip deltas that the root `CMakeLists.txt` layers on
> top of the real board file, and only when `HW_OTA_LAYOUT=1`.
> They match the same `*.defaults` glob, so the no-argument listing prints them
> alongside the real boards (`feather_esp32_v2.ota`, `qtpy_esp32.ota`). Do not
> pass one as a board name - the selectable boards are the files with no `.ota`
> in the middle.

> **Do not run two boards' builds at the same time.** The root `partitions.csv` is generated at configure time and read during the build - it is the one file the per-board directories still share. Serialize them.

Each successful build also writes `build-<board>/BUILD_INFO.md`: a manifest of what that image actually contains - every feature flag as the compiler resolved it, chip / PSRAM / flash settings, image size and partition headroom, and the git commit it came from.

The plain single-directory flow still works if you only ever build one board:

```bash
# Set the chip target (do this once, or any time you switch chip families)
idf.py set-target esp32s3    # Unexpected Maker FeatherS3 or Seeed XIAO ESP32-S3
idf.py set-target esp32      # Adafruit QT Py ESP32 or Feather ESP32 V2

# Build
idf.py build

# Flash and open serial monitor (replace PORT with your device's port)
idf.py -p PORT flash monitor
```

> You can also run `idf.py -p PORT flash monitor` directly - it will build automatically if anything has changed. The separate `build` step is useful if you want to confirm the build succeeds before connecting the device.

- **FeatherS3 / XIAO ESP32-S3:** port is usually `/dev/cu.usbmodem*` (native USB). You may need to hold the BOOT button when initiating the flash.
- **QT Py ESP32 / Feather ESP32 V2:** port is usually `/dev/cu.usbserial*` or `/dev/cu.SLAB*` (UART bridge).

That's it. The build can take a few minutes the first time.

---

## Switching Between Board Families

With `tools/build_board.sh` there is nothing to switch. Every board keeps its own `build-<board>/` directory, its own `sdkconfig` and its own bootloader, so moving between an **ESP32** board (QT Py, Feather V2) and an **ESP32-S3** board (FeatherS3, XIAO) is just a different board name on the command line, and rebuilding a board you built before stays incremental. Once compiled the boards can be used together without issue. The only difference is setup.

If you are working in the single shared `build/` directory instead, a full clean is still required - the two chip families have different architectures and the build cache is not compatible.

```bash
idf.py fullclean
idf.py set-target esp32s3    # or esp32
idf.py -p PORT flash monitor
```

Two layers of defaults are applied for you, and it helps to know which is which.

**Per chip family** - `config/sdkconfig.defaults.esp32` / `config/sdkconfig.defaults.esp32s3`. The root `CMakeLists.txt` points `SDKCONFIG_DEFAULTS` at `config/sdkconfig.defaults` plus your board file, and ESP-IDF derives the chip-family file by appending `.<target>` to each entry it is handed - not by looking in the project root - so the file matching your chip is layered in automatically (the root `CMakeLists.txt` spells this out where it sets `SDKCONFIG_DEFAULTS`):

- **PSRAM speed:** ESP32 runs PSRAM at 40 MHz; ESP32-S3 at 80 MHz.
- **Flash mode:** ESP32 uses `DIO`; ESP32-S3 uses `QIO`.
- **Bluetooth:** ESP32 supports Classic BT + BLE 4.2; ESP32-S3 is **BLE 5.0 only** - no Classic Bluetooth.

**Per board** - `boards/<name>.defaults`, layered on top of the family file. This is where the Arduino pin variant, flash size, partition table, and **PSRAM mode** come from:

| Board | Board file | PSRAM mode | Flash | Bluetooth |
| ----- | ---------- | :--------: | :---: | :-------: |
| Unexpected Maker FeatherS3 | `feathers3` | **Quad** | 16 MB | on |
| Seeed XIAO ESP32-S3 | `xiao_s3` | **Octal** | 8 MB | **off** |
| Adafruit QT Py ESP32 | `qtpy_esp32` | **Quad** | 8 MB | **off** |
| Adafruit Feather ESP32 V2 | `feather_esp32_v2` | **Quad** | 8 MB | on |

> **Two boards ship with the Bluedroid stack compiled out** (`CONFIG_BT_ENABLED=n`) to buy back
> flash and RAM: `qtpy_esp32` (~14 KB IRAM / ~80 KB DRAM / ~70 KB flash on the memory-tight
> classic ESP32) and `xiao_s3` (the image overflowed the factory partition with Bluetooth plus
> the on-board LLM source). You do **not** hand-edit `System_BuildConfig.h` for this any more:
> the header's derived rules see `CONFIG_BT_ENABLED` missing and force `ENABLE_BLUETOOTH` - and
> its G2 / R1 dependents - to `0`, and `components/hardwareone/CMakeLists.txt` applies the same
> downgrade to its source-list filter, so the `#if` gates and the file list cannot diverge.

> **`feathers3_fe` is a fifth board file, and it is not a normal build.** Identical hardware to
> `feathers3`, but with flash encryption (Development mode) and NVS encryption enabled. Its
> **first boot burns eFuses and encrypts the flash in place, permanently**, and burns JTAG off.
> Never flash it onto a board you do not intend to encrypt. Reflash such a unit with
> `idf.py encrypted-flash`; a plain `idf.py flash` produces a harmless no-boot.

> **PSRAM mode is a property of the board, not of the chip family.** Both ESP32-S3
> boards here use different modes - the XIAO is octal, the FeatherS3 is quad. Do
> not assume "S3 means octal". Setting the wrong mode does **not** fail loudly:
> the device boots normally and reports 0 KB PSRAM, so the first symptom is the
> LLM, large web responses, or ESP-NOW buffers running out of memory at runtime.

You do not normally set any of this by hand - selecting the right board file does
it. If you do need to deviate, run `tools/build_board.sh <board> menuconfig`, which
applies to that board's directory only (or `idf.py menuconfig` after `set-target` in
the shared-`build/` flow). See
[BOARD_SWITCHING.md](BOARD_SWITCHING.md) for the full per-board menuconfig reference.

---

## First-Time Use

On first boot, the device detects that no users file exists and launches the setup wizard automatically. The wizard runs on **serial and OLED simultaneously** - use whichever is more convenient. Open the serial monitor at **115200 baud** to follow along or drive setup from your computer.

### Step 1 - Choose setup mode

You'll be prompted to choose one of three modes:

- **Basic** - asks what you will use the device for, pre-enables a matching set of features, then creates your admin account. Quickest way to get running.
- **Advanced** - runs the full configuration wizard after account creation.
- **Import from Backup** - restores settings from a `.hwbackup` file instead of configuring by hand. Only offered on builds with WiFi and the migration tool compiled in.

Basic mode's question is *"What will you use this device for?"*. Pick one of four deployment archetypes; each turns on a small set of features before the account prompts. Archetypes whose required features are not compiled into your build are hidden rather than offered and failing.

| Archetype | Needs compiled in | Pre-enables |
| --------- | ----------------- | ----------- |
| **Standard Handheld** | OLED + an input device | WiFi, web, I2C, OLED, input, automations |
| **Headless / relay** | - | WiFi, web, I2C, ESP-NOW, automations |
| **G2 Companion** | Bluetooth | WiFi, web, I2C, Bluetooth, automations |
| **Meshed Node** | ESP-NOW | WiFi, I2C, ESP-NOW, automations |

Detected sensors are not touched by the archetype - hardware detection handles those separately. `b` / `back` at the archetype prompt returns to the mode menu.

### Step 2 - Create your admin account

Enter a username and password when prompted. These are your credentials for the web UI. Both fields are required and cannot be blank.

### Step 3 - Advanced wizard (Advanced mode only)

The wizard has up to nine pages. Three always appear - Features, Sensors &
Display, and System Settings; the other six are shown only when the relevant
feature is compiled in or enabled, so the page count you see (`SETUP 3/6` etc.)
depends on your build and on what you turned on earlier in the wizard.

| # | Page | Shown when | Contents |
| - | ---- | ---------- | -------- |
| 1 | **Features** | always | Enable/disable network features - WiFi, Web Interface, Bluetooth, ESP-NOW - each with a rough heap cost |
| 2 | **Web Mode** | Web Interface enabled on page 1 | `HTTP` or `HTTPS` (HTTPS costs ~20 KB more RAM; picking it generates a self-signed cert) |
| 3 | **Bluetooth Mode** | Bluetooth enabled on page 1 | `Server (phone)` or `G2 Glasses` (~10 KB more RAM) |
| 4 | **Sensors & Display** | always | Enable/disable the display and each I2C sensor |
| 5 | **Startup & Auto-start** | WiFi, ESP-NOW, MQTT or any sensor compiled in | WiFi auto-connect, ESP-NOW mesh, MQTT auto-start, plus per-sensor auto-start |
| 6 | **System Settings** | always | Timezone and log level always; NTP server when WiFi is compiled in, LED startup effect when the LED is, and the device name on builds without ESP-NOW compiled in |
| 7 | **ESP-NOW Identity** | ESP-NOW enabled | Device name (used for **both** Bluetooth and ESP-NOW, default `HardwareOne`), room, zone, mobile/stationary. All optional |
| 8 | **MQTT Broker** | MQTT enabled on page 1 **and** MQTT auto-start on page 5 | Broker host, port, username, password |
| 9 | **WiFi Setup** | WiFi enabled | Scan and join. Select by number, type an SSID directly, `rescan`, or `skip` |

On any page, `b` / `back` returns to the previous one.

> **Skipping the ESP-NOW Identity page turns ESP-NOW off.** Answering `n` (or
> pressing Enter) at that page's prompt sets `espnowEnabled = false`, because the
> mesh cannot start without a device name. If you want ESP-NOW, answer `c` and
> at minimum accept the default name. You can set it later with `espnowsetname`
> and re-enable with `espnowenabled 1`.

The theme is not one of the nine pages. On builds with the web server compiled
in, first-time setup asks for a **Light** or **Dark** web UI theme once, right
after the wizard - and in Basic mode too, right after the account is created. It
is skipped entirely when `ENABLE_HTTP_SERVER=0`, and you can change it later from
the web **Settings** page.

### Step 4 - Access the UI
> If you chose Basic mode, or turned the Web Interface off, or skipped WiFi during the wizard, the web server will not come up on its own - the boot path starts it only when the Web Interface is enabled **and** WiFi is already connected. The Serial interface and the OLED interface (if connected) are then the only ones available. Run `openhttp` in the serial console to start the server for this session, or `httpAutoStart 1` to have it start on every boot.
> If you enabled the Web Interface and joined a network during the wizard, the device connects to WiFi and prints its IP address in the serial monitor. Navigate to that address in a browser to reach the web UI, and log in with the username and password you created in Step 2.

Type `help` at any time in the serial console to see all available commands.

---

> ## Back to the overview: [README](../README.md)

> ## Full reference, commands, and configuration: [User Guide](USERGUIDE.md)
