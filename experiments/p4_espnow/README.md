# ESP32-P4X-EYE ↔ XIAO ESP32-S3 ESP-NOW milestone

This isolated experiment tests whether the P4 can use its onboard ESP32-C6 radio
to exchange HardwareOne-sized ESP-NOW packets with a native ESP32-S3. Both radios
start on channel 6 as unassociated stations. No access point, SSID, Wi-Fi
password, or Internet connection is needed for the radio test; fetching build
dependencies initially requires Internet access.

The probes live under `experiments/p4_espnow/`. The bridge they exercise is
production code now: the host side is `components/esp_now_hosted`, and the C6
side, the companion firmware build, the P4 programmer service and the backup
tooling moved to [`tools/p4/companion`](../../tools/p4/companion/README.md).
This directory keeps the native S3 and Hosted P4 probes, the hardware runner
and the recorded results. These are separate firmware applications, so
flashing them replaces the firmware currently running on the selected chip.
Full flash backups make that replacement reversible.

This milestone is a transport/backend probe. It does **not** establish a working
HardwareOne P4 application, application pairing, mesh-passphrase authentication,
encrypted sessions, fragmentation, or three-node relay routing. The P4 plus its
C6 companion is **one logical radio node**; the S3 is the second.

## Architecture

```text
P4X-EYE                                             XIAO ESP32-S3 Sense
P4 probe ─ ESP-NOW API adapter ─ ESP-Hosted/SDIO ─ C6  ~~~ ESP-NOW ~~~  native S3 probe
         application/test logic                    radio             control endpoint
```

The P4 adapter forwards ESP-NOW API requests through Hosted CustomRpc. The C6
executes the native API and returns its actual result; asynchronous receive and
send events travel back separately. Peer queries read the C6's real peer table.
Radio callbacks and transport callbacks enqueue bounded events instead of doing
blocking RPC work. Source/destination MAC, RSSI, channel, and payload are carried
through the bridge. See the
[bridge README](../../tools/p4/companion/bridge/README.md) for lifetime,
timeout, supported-API and metadata details.

The probe uses HardwareOne's packed 32-byte V4 header and the reserved experiment
opcodes 200/201/202 for discovery/request/echo. REQUEST and ECHO frame sizes are
32, 64, 128 and 250 bytes total. Both endpoints validate CRC and deterministic
payload bytes. These checks distinguish end-to-end application receipt from a
successful send submission or radio MAC acknowledgement. The full wire contract
is in [native_s3/README.md](native_s3/README.md).

HardwareOne itself registers unencrypted ESP-NOW radio peers on `WIFI_IF_STA`
with channel 0, meaning they follow the current radio channel. Its confidentiality
is provided by application-layer sessions. This probe follows the same radio
peer policy but does not implement those sessions.

## Reproducible builds

Use the exported **ESP-IDF 5.5.5** environment for all three chips. The P4 board
in this experiment is revision 3.2; its configuration selects the revision-3
support path. IDF 6 is not needed for this milestone.

| Dependency | Pin |
| --- | --- |
| ESP-IDF | 5.5.5 |
| ESP-Hosted MCU, `esp-serial-flasher` | the tags and SHAs in `components/esp_now_hosted/include/esp_now_hosted_companion.h` (v2.12.13 and v1.10.0 when this was run) |
| P4 `esp_wifi_remote` component | 1.3.1 |
| Bridge derivation | ESPHome overlay `14ee146ec71923aa250a7f743e5cff266a5ffb1a`, with the local changes and license documented in `tools/p4/companion/bridge/` |

From the repository root, with the ESP-IDF 5.5.5 environment exported:

```sh
experiments/p4_espnow/build.sh all
```

`all` builds the two probes here and delegates `programmer` and `c6` to
`tools/p4/companion/build.sh`, whose `prepare.py` fetches the pinned
dependencies into `tools/p4/companion/private/`, applies the serial-flasher
patch and generates the C6 slave project. Individual builds are
`./build.sh s3`, `./build.sh p4`, `./build.sh programmer` and `./build.sh c6`.
The script requires IDF 5.5.5, refuses a build cache or configuration for the
wrong target instead of deleting it, and opens no serial port.

| Source | Target | Build directory | Purpose |
| --- | --- | --- | --- |
| `native_s3/` | ESP32-S3 | `private/build-s3/` | Native-radio discovery and echo control. |
| `hosted_p4/` | ESP32-P4 | `private/build-p4/` | Hosted-radio initiator, peer lifecycle and delivery tests; links `components/esp_now_hosted`. |
| `tools/p4/companion/c6_programmer/` | ESP32-P4 | `tools/p4/companion/private/build-programmer/` | Temporary P4 service for backing up and programming the onboard C6. |
| Generated `tools/p4/companion/private/c6-slave/` | ESP32-C6 | `tools/p4/companion/private/build-c6/` | Hosted radio firmware plus the ESP-NOW RPC bridge. |
| `tools/p4/companion/c6_tool.py` | Desktop Python | None | Explicit C6 info/read/hash/write/run/monitor operations via the P4 service. |

## Board wiring and access

Both EYE USB connectors lead to the **P4**, not directly to the onboard C6's ROM
loader. Access to the C6 in this experiment uses the P4 service firmware and the
board's existing internal UART, reset and boot connections. No additional jumper
wiring is needed on this EYE board.

The wiring below is specific to the EYE board used here, not a generic P4 pinout:

| Connection | P4 GPIO |
| --- | --- |
| C6 enable/reset | 9 |
| C6 ROM boot strap | 33 |
| P4 UART TX toward C6 | 35 |
| P4 UART RX from C6 | 36 |
| Hosted SDIO CMD | 27 |
| Hosted SDIO CLK | 28 |
| Hosted SDIO D0 / D1 / D2 / D3 | 29 / 30 / 31 / 32 |

The Hosted host uses SDIO slot 1, four data bits and a 20 MHz clock. Both Hosted
endpoints use **SDIO streaming mode** and a **1000 Hz FreeRTOS tick**; the host's
initial packet-mode default was incompatible with the companion's streaming
default. The pinned C6 Kconfig selects SDIO, streaming and high-speed capability
by default. Its defaults select 4 MiB flash and the C6 custom partition table
(`partitions.esp32c6.csv`); IDF 5.5.5 defaults resolve DIO and 80 MHz flash.
The Hosted
P4 probe releases the C6 boot strap high before Hosted resets the companion.
Hosted's `RESET_ACTIVE_HIGH` option describes the enable polarity here; the
actual reset is a low pulse on the C6 enable line.

The observed USB serial ports were `/dev/cu.usbmodem2101` for P4 and
`/dev/cu.usbmodem1101` for S3. **Identify them again before flashing**: macOS may
assign different names after reconnecting or changing USB connectors. A P4
programmer `PROGRAMMER_READY` banner and the C6 tool's `INFO chip=esp32c6`
response are distinct from the S3/P4 probe `READY` banners. Close other serial
monitors before using a port.

## Backup, install and restore workflow

The C6 part of this workflow (programmer service, C6 backup, companion
install, C6 restore) is documented once, in
[`tools/p4/companion/README.md`](../../tools/p4/companion/README.md); the
commands below are the probe-specific remainder and still work as written
when `c6_tool.py` and the `private/` paths are read as
`tools/p4/companion/c6_tool.py` and `tools/p4/companion/private/`.

The commands below assume the current directory is `experiments/p4_espnow/`,
the IDF environment is exported, and `P4_PORT`/`S3_PORT` name the independently
identified boards. Do not rely on `c6_tool.py`'s default port. Its Python runtime
needs `pyserial`, included by the isolated tooling installation below.

The validated P4 path uses **esptool 5.4 with `--no-stub`**. The exported IDF
environment contains esptool 4.12, whose commands use different spellings and
which is not the P4 flashing path used here. Keep the SDK environment intact and
install the separate tool in ignored `private/`:

```sh
python3 -m venv private/tools-env
ESPTOOL_PY="$PWD/private/tools-env/bin/python"
"$ESPTOOL_PY" -m pip install 'esptool==5.4'
"$ESPTOOL_PY" -m esptool version
```

Use that absolute `ESPTOOL_PY` path for the hyphenated commands below and for
`c6_tool.py`. All direct P4 operations use the ROM loader through `--no-stub`.
The C6 service separately uses its tested embedded serial-flasher stub.

Keep full images, checksums and raw device logs in ignored `private/backups/`.
**Flash backups may contain credentials and other secrets. Never commit or
publish them.** A Git checkout alone does not preserve ignored backups; retain
them locally for restoration.

1. Before installing either probe, read and verify the P4 and S3's complete
   original flash images. The observed capacities are 16 MiB and 8 MiB:

   ```sh
   mkdir -p private/backups
   "$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub read-flash \
     0 0x1000000 private/backups/p4-factory-16mb.bin
   "$ESPTOOL_PY" -m esptool --chip esp32s3 --port "$S3_PORT" read-flash \
     0 0x800000 private/backups/xiao-original-8mb.bin
   "$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub verify-flash \
     0 private/backups/p4-factory-16mb.bin
   "$ESPTOOL_PY" -m esptool --chip esp32s3 --port "$S3_PORT" verify-flash \
     0 private/backups/xiao-original-8mb.bin
   shasum -a 256 private/backups/p4-factory-16mb.bin private/backups/xiao-original-8mb.bin
   ```

   Preserve existing verified originals; do not rerun backup commands over those
   filenames after installing experimental firmware. Use a new filename for a
   later snapshot.

2. Flash the P4 service using the already-built programmer project:

   ```sh
   "$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub write-flash \
     --flash-mode dio --flash-size 16MB --flash-freq 80m \
     0x2000 private/build-programmer/bootloader/bootloader.bin \
     0x8000 private/build-programmer/partition_table/partition-table.bin \
     0x10000 private/build-programmer/hw1_c6_programmer.bin
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" info
   ```

   The service talks to the C6's ROM/stub loader and identifies its actual flash
   size. It does not write the C6 automatically. Reads and info queries put the
   C6 into the loader, so they interrupt its normal radio firmware until reset.

3. Before writing the C6, make its full original backup. The observed C6 flash
   size is 4 MiB. Omitting `--size` with offset 0 uses the detected whole flash:

   ```sh
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" read --offset 0 \
     --file private/backups/c6-original-4mb.bin
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" hash --offset 0 --size 0x400000
   shasum -a 256 private/backups/c6-original-4mb.bin
   ```

   A successful read ends with `BACKUP_VERIFIED`: it checks contiguous offsets,
   exact byte count and matching device/host SHA-256. It writes a `.partial`
   file first and refuses an existing output. A separate `hash` rereads flash
   on the C6; compare that SHA-256 with the saved image. An incomplete or
   `.partial` file is not a restoration backup.

4. Install the C6 companion firmware while the P4 service remains running.
   `private/build-c6/flasher_args.json` is the authority for image filenames
   and offsets. Write the application first, followed by bootloader, partition
   table and OTA data, matching the validated installation order. Leave the C6
   in its loader until all writes verify; the tool does not run it automatically.
   For the pinned build, a complete image-layout installation is:

   ```sh
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" write --offset 0x10000 \
     --file private/build-c6/network_adapter.bin
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" write --offset 0x0 \
     --file private/build-c6/bootloader/bootloader.bin
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" write --offset 0x8000 \
     --file private/build-c6/partition_table/partition-table.bin
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" write --offset 0xd000 \
     --file private/build-c6/ota_data_initial.bin
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" run
   "$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" monitor
   ```

   Each write validates range/alignment, waits for the loader's verification,
   rereads the written region and compares its SHA-256 with the input file.
   Success ends with `FLASH_READBACK_VERIFIED`. `run` resets only the C6 into
   normal execution. `monitor` returns about ten seconds of its UART output.
   Updating only the application at `0x10000` is appropriate only after verifying
   that the installed partition table and selected boot slot match this build.

5. Replace the P4 service with the Hosted probe, and install the S3 control:

   ```sh
   "$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub write-flash \
     --flash-mode dio --flash-size 16MB --flash-freq 80m \
     0x2000 private/build-p4/bootloader/bootloader.bin \
     0x8000 private/build-p4/partition_table/partition-table.bin \
     0x10000 private/build-p4/hw1_hosted_p4_probe.bin
   idf.py -C "$PWD/native_s3" -B "$PWD/private/build-s3" -p "$S3_PORT" flash
   ```

   `c6_tool.py` requires the service firmware, so it will no longer work while
   the Hosted probe runs on the P4. The normal P4 probe accesses the C6 through
   SDIO instead.

To restore the original boards, first reinstall the P4 service if needed, then
restore and verify the **C6 before restoring P4**:

```sh
"$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000 private/build-programmer/bootloader/bootloader.bin \
  0x8000 private/build-programmer/partition_table/partition-table.bin \
  0x10000 private/build-programmer/hw1_c6_programmer.bin
"$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" write --offset 0 \
  --file private/backups/c6-original-4mb.bin
"$ESPTOOL_PY" c6_tool.py --port "$P4_PORT" run
"$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub write-flash \
  0 private/backups/p4-factory-16mb.bin
"$ESPTOOL_PY" -m esptool --chip esp32p4 --port "$P4_PORT" --no-stub verify-flash \
  0 private/backups/p4-factory-16mb.bin
"$ESPTOOL_PY" -m esptool --chip esp32s3 --port "$S3_PORT" write-flash \
  0 private/backups/xiao-original-8mb.bin
"$ESPTOOL_PY" -m esptool --chip esp32s3 --port "$S3_PORT" verify-flash \
  0 private/backups/xiao-original-8mb.bin
```

These are full-image restores, including saved settings. They must use the
verified originals from the corresponding chip, not another board's image.

## Probe commands and acceptance evidence

Both endpoints print their MAC and channel at `READY`. Discovery registers the
other endpoint as an unencrypted channel-0 peer. The P4 initiates the measured
requests; the S3 echoes their payloads and message IDs.

| Endpoint | Command | Purpose |
| --- | --- | --- |
| Both | `stats` | Capture send, receive, callback, error and queue counters. |
| Both | `channel N` | Select channel 1–11 without recreating channel-0 peers. |
| Both | `restart_radio` | Restart the local radio stack and verify recovery. |
| P4 | `peer_test` | Exercise and report native companion peer-table/API behavior. |
| P4 | `ping COUNT SIZE` | Request 1–1000 exchanges at size 32, 64, 128 or 250 bytes. |
| S3 | `silence 1` / `silence 0` | Suppress/resume discovery and application replies while leaving the radio running. |

Retain separate evidence for API submission, TX callback delivery, validated
application echo, timeouts, payload errors, and round-trip latency. HardwareOne's
fragment ACK budget is 200 ms, so latency matters as well as successful delivery.
Exercise all four frame sizes, a sustained 250-byte run, peer lifecycle errors,
silence/timeouts, channel mismatch and recovery, and radio restart. See the S3
README for malformed-frame cases and the bridge README for its host protocol
test. Passing parser tests or compiling firmware alone does not prove the link.

## Results

**Passed on 27 September 2026:** all 26 automated steps, 1,540/1,540 normal
round trips, and 20/20 expected timeouts during deliberately silent/mismatched
channel tests. The 1,000-packet 250-byte run averaged 16.19 ms round trip;
the maximum was 57.42 ms, below the 200 ms deadline. Peer-table behavior,
channel recovery, one S3 radio restart and two P4/C6 radio restarts passed.
Final bridge and callback-drop counters were zero.

See [the results report](results/2026-09-27/RESULTS.md),
[machine-readable results](results/2026-09-27/summary.json), and
[firmware identities](results/2026-09-27/firmware.json) for evidence and limits.
Both boards remain on the probe firmware, channel 6; original complete images
remain in ignored `private/backups/`. No production HardwareOne source was changed.

To repeat the matrix with these firmwares running and other serial monitors closed:

```sh
"$ESPTOOL_PY" test_hardware.py --p4-port "$P4_PORT" --s3-port "$S3_PORT"
```

The runner writes timestamped per-board logs, individual events and a summary
under `private/test-runs/`. It stops on an unexpected result and retains partial
evidence. It sends radio/test commands but does not flash firmware.
