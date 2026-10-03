# ESP32-C6 companion firmware for the P4X-EYE

The ESP32-P4 has no radio. On the P4X-EYE every Wi-Fi, Bluetooth LE and
ESP-NOW operation runs on the board's ESP32-C6 through ESP-Hosted over SDIO.
The C6 is therefore a second firmware that every `HW_BOARD=p4x_eye` image
depends on, and this directory is its home in the main tree:

| Path | What |
| --- | --- |
| [`../../../components/esp_now_hosted/include/esp_now_hosted_companion.h`](../../../components/esp_now_hosted/include/esp_now_hosted_companion.h) | **The pins.** ESP-Hosted tag/SHA and version, esp-serial-flasher tag/SHA. The P4 firmware checks the running C6 against these at boot; `prepare.py` fetches exactly these. |
| `bridge/esp_now_hosted_slave.c`, `.h` | The C6 side of the HardwareOne ESP-NOW bridge (stock Hosted does not forward ESP-NOW). The host side and the shared wire header are the production component `components/esp_now_hosted`. |
| [`bridge/README.md`](bridge/README.md) | Protocol, threading, timeouts, provenance and license of the bridge. |
| `prepare.py` | Fetches the pinned ESP-Hosted MCU slave and esp-serial-flasher into the ignored `private/`, applies the recorded flasher patch, generates `private/c6-slave/` (pinned slave + bridge). `--check` verifies without writing. |
| `build.sh` | Builds the C6 firmware and the P4 programmer service into `private/build-c6/` and `private/build-programmer/`. |
| `c6_programmer/` | Temporary P4 firmware that talks to the C6's ROM loader over the board's internal UART, EN and BOOT lines (both USB ports lead to the P4). Explicit commands only; it never writes flash on its own. |
| `c6_tool.py` | Host-side driver for the programmer: `info`, `read`, `hash`, `write`, `run`, `monitor`. Every write is read back and SHA-256 verified. |
| `dependency_locks/c6.lock` | The exact registry component versions resolved for the C6 build on ESP-IDF 5.5.5. |
| `dependency_patches/` | The serial-flasher fix for reads ending at the flash boundary and an escaped first SLIP byte (both matter for faithful backups); `tests/slip_read_regression.c` is its regression test. |

The hardware-qualified results that produced this firmware are in
[`experiments/p4_espnow`](../../../experiments/p4_espnow/README.md), which now
builds its probes against this directory instead of carrying copies.

## What the P4 does at boot, and after

`components/hardwareone/HAL_Radio.cpp` brings the SDIO transport up (two
attempts) and `System_RadioCompanion` classifies the companion in one line
each:

- the C6's reported ESP-Hosted version is compared with the header; a
  mismatch is a warning (Hosted's own RPCs may still work) with a pointer
  here and a `companion_mismatch` event;
- one ESP-NOW bridge request (`GET_INFO`) is sent. A reply means the bridge
  is present and also tells the P4 which ota slot runs, whether that image is
  still pending confirmation, and the C6's uptime, heap and reset reason. No
  reply within the bridge timeout means the C6 runs stock Hosted firmware:
  the log says so, Wi-Fi and BLE keep working through Hosted's own RPCs, and
  ESP-NOW cannot start. `radioCompanionLogBootSummary()` repeats the verdict
  after setup.

Neither check is fatal, and a C6 that does not answer at all is not fatal
either: the P4 boots without its radio features and retries in the
background. From then on the companion is monitored (heartbeat, Hosted
transport events, C6 reset notifications) and recovered in place when it
stops answering; `c6status` shows all of it, and the C6's own console is
mirrored into the P4 log as `C6>` lines (`c6console`), which is how a crash
in the companion shows up without the programmer. The P4 side is described in
[docs/P4X_EYE_PERIPHERALS.md](../../../docs/P4X_EYE_PERIPHERALS.md#radio-companion-esp32-c6).

The bridge answers two requests for this beyond ESP-NOW itself: `GET_INFO`
(above) and `CONFIRM_IMAGE`, which marks the running image valid so the
bootloader's rollback protection keeps it. `prepare.py` adds
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` to the slave build; the protection
is active once a C6 carries that bootloader (a cable flash), while `c6update`
itself also works with the older bootloader, just without the fallback.

## Build

```sh
. "$IDF_PATH/export.sh"        # ESP-IDF 5.5.5, the same SDK as the P4 image
tools/p4/companion/build.sh    # prepare.py, then programmer + c6
```

`prepare.py` clones the pinned sources on first use (network needed once),
refuses a cache at the wrong commit or with unrelated edits, and keeps a
manifest of the generated C6 files so a refreshed bridge is applied while
unknown edits are never overwritten. `python3 tools/p4/companion/prepare.py
--check` is the offline verification.

## Back up and flash the C6

Everything below runs from the repository root with the IDF environment
exported, `P4_PORT` naming the board's USB port (identify it again after
reconnecting) and an esptool 5.4 with `--no-stub` for the P4 ROM loader. The
exported IDF carries esptool 4.12, whose spellings differ; keep that
environment intact and install the newer tool in the ignored workspace:

```sh
python3 -m venv tools/p4/companion/private/tools-env
ESPTOOL_PY="$PWD/tools/p4/companion/private/tools-env/bin/python"
"$ESPTOOL_PY" -m pip install 'esptool==5.4' pyserial
```

Keep images, checksums and logs under the ignored
`tools/p4/companion/private/backups/`. **Flash backups can contain
credentials. Never commit or publish them.**

1. **Back up the P4** before replacing its application:

   ```sh
   mkdir -p tools/p4/companion/private/backups
   "$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub read-flash \
     0 0x1000000 tools/p4/companion/private/backups/p4-before-$(date +%Y%m%d).bin
   ```

2. **Flash the programmer service** onto the P4 (it replaces the factory app
   at `0x10000`; bootloader and partition table are untouched):

   ```sh
   "$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub write-flash \
     --flash-mode dio --flash-size 16MB --flash-freq 80m \
     0x2000 tools/p4/companion/private/build-programmer/bootloader/bootloader.bin \
     0x8000 tools/p4/companion/private/build-programmer/partition_table/partition-table.bin \
     0x10000 tools/p4/companion/private/build-programmer/hw1_c6_programmer.bin
   "$ESPTOOL_PY" tools/p4/companion/c6_tool.py --port "$P4_PORT" info
   ```

   `info` puts the C6 into its loader and reports its real flash size (4 MiB
   on the EYE); the C6's radio firmware is interrupted until `run`.

3. **Back up the C6** once, before its first write:

   ```sh
   "$ESPTOOL_PY" tools/p4/companion/c6_tool.py --port "$P4_PORT" read --offset 0 \
     --file tools/p4/companion/private/backups/c6-original-4mb.bin
   "$ESPTOOL_PY" tools/p4/companion/c6_tool.py --port "$P4_PORT" hash --offset 0 --size 0x400000
   ```

   A good read ends with `BACKUP_VERIFIED` (contiguous offsets, exact size,
   device and host SHA-256 equal). The tool refuses to overwrite an existing
   output; a `.partial` file is not a backup.

4. **Install the companion firmware.** `private/build-c6/flasher_args.json`
   is the authority for names and offsets. Write the application first, then
   bootloader, partition table and OTA data, and only then `run`:

   ```sh
   C6=tools/p4/companion/private/build-c6
   T="$ESPTOOL_PY tools/p4/companion/c6_tool.py --port $P4_PORT"
   $T write --offset 0x10000 --file $C6/network_adapter.bin
   $T write --offset 0x0     --file $C6/bootloader/bootloader.bin
   $T write --offset 0x8000  --file $C6/partition_table/partition-table.bin
   $T write --offset 0xd000  --file $C6/ota_data_initial.bin
   $T run
   $T monitor
   ```

   Each write validates range and alignment, waits for the loader's own
   verification, rereads the region and compares SHA-256; success ends with
   `FLASH_READBACK_VERIFIED`. `run` resets only the C6; `monitor` shows about
   ten seconds of its UART log.

5. **Put the HardwareOne image back on the P4** (`tools/build_board.sh p4x_eye
   flash`, or the deployment release in `deployments/handheld`). Its boot log
   should now report the companion version as matching and the bridge as
   present.

To restore the original C6, repeat step 2, then
`c6_tool.py write --offset 0 --file .../c6-original-4mb.bin` and `run`.

## Updating the C6 from the P4 (`c6update`)

Once a bridge-carrying image runs on the C6, later companion images need no
cable. Copy `private/build-c6/network_adapter.bin` to the P4's storage (SD
card or LittleFS, for example through the web file manager or
`espnowsendfile`) and run, as an admin:

```
c6update "/sd/network_adapter.bin"
```

The P4 checks the file before sending anything (an ESP32-C6 application image
of at most 1920 KB that contains the bridge marker `HW1-ESPNOW-BRIDGE/1`),
pauses ESP-NOW and Bluetooth, streams the image into the inactive ota slot
through ESP-Hosted's OTA requests, activates it, waits for the C6 to reboot,
rebuilds the transport and asks the bridge again. Only when the bridge
answers from the new image is it confirmed; if it does not, the C6 falls back
to the previous image on its next reset (with the rollback-enabled
bootloader) and `c6status` reports what happened. Wi-Fi stays up during the
transfer when it was up; the paused features return afterwards. `c6confirm`
accepts a pending image by hand and `c6restart` restarts the companion in
place.

The programmer service and `c6_tool.py` remain the way to flash a C6 that has
no bridge yet, to replace the bootloader or partition table, and to take or
restore full backups.

Restart, hold and release ran on the board on 2026-10-03. The update path
has not: the bench device's LittleFS had less free space than the image
needs (about 1.3 MB), so the transfer and the rollback confirmation remain
to be exercised.

## Changing the pin

Edit the header, run `prepare.py` (it refuses a cache at the old commit and
tells you to move it aside), rebuild and re-flash the C6, then rebuild the P4
image so its boot check expects the new version. Changing the bridge wire
format means bumping `ESP_NOW_HOSTED_WIRE_VERSION` in the shared header and
flashing both sides together; the two ends reject each other's frames
otherwise.
