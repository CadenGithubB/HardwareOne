# Setting up a device for OTA

How to take a board from any state to a working signed-OTA device, and how to
decide whether a given board should have OTA at all.

Procedure verified end to end on a plain FeatherS3 on 2026-08-05, from a full
chip erase through first-time setup, on firmware v0.99.8. Every command below
was actually run; the outputs quoted are real.

The design, threat model and qualification matrix used to live in
`docs/OTA_RECOVERY_UPDATER.md`, which was removed from this repo along with the
other internal planning and audit documents; read it from git history with
`git show v0.99.92:docs/OTA_RECOVERY_UPDATER.md`. This document is only the
provisioning path.

## Should this board have OTA?

OTA is opt-in per build, and that is deliberate. It is not a feature flag - it
is a different flash layout, a second firmware, and a signing key. Turning it on
for every build would be wrong.

What it costs, measured rather than estimated:

| | Ordinary build | OTA build |
| --- | ---: | ---: |
| OTA code in the app | ~800 B flash, 0 B RAM | ~64 KiB flash, ~3.9 KiB RAM |

The flash layout cost is the part that matters, and it comes out of the
filesystem:

| Partition | Non-OTA 16 MB | OTA 16 MB |
| --- | ---: | ---: |
| `factory` | 5,716 KiB (the app) | 1,336 KiB (recovery updater) |
| `otadata` | - | 8 KiB |
| `ota_0` | - | 5,760 KiB (the app) |
| `littlefs` | 10,604 KiB | 9,216 KiB |

LittleFS gives up 1,388 KiB. In exchange the app slot gains 44 KiB and you get
a recovery updater that cannot be bricked. The arithmetic balances exactly.

Enable OTA when the device will be updated in the field, is a 16 MB FeatherS3,
and you control a signing key you can keep for the life of the deployment.

Do NOT enable it when:

- The board is not a 16 MB FeatherS3. The build refuses anything else today,
  and the recovery updater has the partition geometry compiled in as constants.
- The build uses ESP-SR. The speech model partition is ~3,008 KiB and there is
  no arrangement that fits a model, two app slots and a filesystem in 8 MB.
- The device is a fixed sensor appliance that is reflashed by cable anyway. It
  would pay 1.36 MB of filesystem for a capability it never uses.
- You do not have durable custody of a signing key. Losing it means no deployed
  unit can ever be updated again.

## Prerequisites

- An RSA-3072 signing key on durable, backed-up storage. NOT `/tmp` - macOS
  purges it, and losing the key ends OTA for every device that shipped with the
  matching public key baked in.
- A 16 MB FeatherS3 on USB, with the port known.
- ESP-IDF 5.5.x exported in the shell.

Set these once per shell:

```sh
. ~/esp/esp-idf/export.sh
export HW1_BOARD=feathers3
export HW1_KEY=/absolute/path/to/hw1-ota-signing.pem
export HW1_PORT=/dev/cu.usbmodem1301
export ESPPORT="$HW1_PORT"   # see the note below - idf.py -p does NOT reach these targets
export HW1_MAIN_BUILD=build-ota
export HW1_UPDATER_BUILD=/private/tmp/hw1-updater-migration
```

## Migration is destructive and one-time

Moving a board onto the OTA layout relocates LittleFS. Everything on the
filesystem is lost. Back up first if the device has anything you want, using
`tools/ota/device_backup.py` against the running web server.

Starting from a blank chip is also fine and is what the steps below assume:

```sh
esptool.py --chip esp32s3 -p "$HW1_PORT" erase_flash
```

## 1. Build the recovery updater

```sh
HW_BOARD="$HW1_BOARD" HW1_OTA_SIGNING_KEY="$HW1_KEY" \
  idf.py -C updater -B "$HW1_UPDATER_BUILD" build
```

There is no longer a migration-only variant. No build of this firmware can
format the device filesystem; that capability lives in the host toolchain.

## 2. Point the main build at that updater

The pair-audit gate is wired at CMake configure time, so exporting the variable
is not enough - the `reconfigure` is required:

```sh
HW_BOARD="$HW1_BOARD" HW_OTA_LAYOUT=1 HW1_OTA_SIGNING_KEY="$HW1_KEY" \
  HW1_UPDATER_BIN="$HW1_UPDATER_BUILD/hw1-updater.bin" \
  idf.py -B "$HW1_MAIN_BUILD" reconfigure
```

Expect: `Recovery OTA layout ENABLED: board=feathers3, layout=hw1-f3-ota-v1`.

A build directory is permanently either ordinary or recovery-OTA. Use a
separate one for each.

The first OTA-layout build also creates a Python venv in the build directory
and installs `littlefs-python` into it, so that build needs network access. It
produces `<build>/littlefs.bin`, a blank filesystem sized from the partition
table.

## 3. Run the guarded migration flash

One target writes bootloader, partition table, `factory`, `ota_0`, a blank
`otadata`, and the blank `littlefs`. It reruns the paired-build audit first, so
a mismatched key or layout cannot reach the device.

```sh
HW_BOARD="$HW1_BOARD" HW_OTA_LAYOUT=1 HW1_OTA_SIGNING_KEY="$HW1_KEY" \
  HW1_UPDATER_BIN="$HW1_UPDATER_BUILD/hw1-updater.bin" \
  ESPPORT="$HW1_PORT" idf.py -B "$HW1_MAIN_BUILD" migration-flash
```

`idf.py -p` is deliberately not used here, and passing it would be a trap.
`-p` is a global option that the BUILT-IN flash actions forward to esptool;
every HardwareOne cable target is a plain CMake custom target, which idf.py
dispatches through a fallback path that drops the port entirely. esptool then
scans and writes whichever device answers first. With one board attached that
is harmless; with two it silently flashes the wrong one. Every guarded target
now prints the port it is about to write and refuses outright when more than
one USB serial device is attached and `ESPPORT` is unset.

It prompts for the exact text `MIGRATE feathers3`; the sanctioned scripted
escape is `HW1_OTA_MIGRATION_CONFIRM="MIGRATE $HW1_BOARD"`.

On `feathers3_fe` use `encrypted-migration-flash`. The plain target fails on
that board on purpose.

This target DESTROYS the filesystem. To replace only the recovery updater on a
device that is already provisioned, use `factory-flash` (or
`encrypted-factory-flash`), which writes one partition and leaves `ota_0`,
`otadata`, NVS and `littlefs` untouched.

## 4. Set the recovery credential and finish setup

Blank `otadata` means the bootloader takes `factory`, so the device comes up in
the recovery updater. Open its console ONCE and keep it open - unlike the main
app it does not hold USB CDC, so reopening the port resets the chip:

```sh
HW_BOARD="$HW1_BOARD" HW1_OTA_SIGNING_KEY="$HW1_KEY" \
  idf.py -C updater -B "$HW1_UPDATER_BUILD" -p "$HW1_PORT" monitor
```

At the `hw1up>` prompt:

```text
status
setpin REPLACE_WITH_A_UNIQUE_12_TO_63_CHAR_PASSPHRASE
cancel
```

`status` should report `littlefsReadOnly: true` - the updater mounted the blank
image that was just flashed. No format step is needed or available.

`setpin` stores the recovery credential. It is currently BOTH the WPA2 PSK for
`HW1-Recovery-XXXX` and the HTTP Basic password (username `admin`);
`recovery_network_start` refuses to run unless the two stored values are
byte-identical. The device never prints it back and has no verify command, so
record it before you type it.

`cancel` selects the valid main image and reboots. `reboot` only restarts the
updater.

The main app then mounts the empty filesystem, finds no `users.json`, and
enters first-time setup:

```text
FS: LittleFS mounted successfully
[EVENT][SETUP] first-time setup required (users.json absent)
FIRST-TIME SETUP
```

Complete it to create your superadmin. If you skipped `setpin`, set the
recovery credential now with `otapin <12 to 63 printable characters>`. Without
it the updater refuses to raise the recovery SoftAP at all, which is the single
most common way this system looks broken when it is not.

Do this while you still have the cable attached: `otapin` is restricted to the
physical serial console, so it is not reachable from the web UI, Bluetooth,
MQTT, ESP-NOW, the OLED or the glasses. Everything else in the OTA lifecycle
(`otawrite`, `otastage`, `otaupdate`) stays available remotely - only changing
or clearing the credential requires someone at the device. If you do get locked
out, the recovery console's `cancel` boots the main app on the same USB
connection, and you can set it again from there.

## Wiping a device's filesystem later

The capability that used to live in firmware is now a cable target:

```sh
ESPPORT="$HW1_PORT" idf.py -B "$HW1_MAIN_BUILD" littlefs-flash
```

That writes the same blank image over `littlefs` and touches nothing else.

## Verifying the result

```sh
python3 tools/ota/check_ota_builds.py --board "$HW1_BOARD" \
  --main-build "$HW1_MAIN_BUILD" --updater-build "$HW1_UPDATER_BUILD"
```

Expect a single OK line naming both versions and one shared key fingerprint.
Run it with the ESP-IDF Python - the system Python lacks `espsecure` and the
audit fails with a confusing signature error.

On the device, `otastatus` should show the running partition as `ota_0`, a
coherent journal, and `recoveryCredentialConfigured` true.

## Known rough edges

**The pair audit has missing build dependencies.** `hw1-ota-pair-audit` reads
`partition_table/partition-table.bin` and `ota_data_initial.bin` but does not
depend on the targets that produce them, so on a fresh build directory the
first `ota0-flash` fails twice with bare `FileNotFoundError` text. Workaround:
run a full `idf.py -B <dir> build` once before the first flash.

**`idf.py erase-flash` is not gated.** The build refuses stock flash targets on
this layout, but ESP-IDF routes the `erase-flash` action straight to esptool
without building the guarded CMake target, so it succeeds and destroys the
recovery updater, NVS and the filesystem. Treat it as a full re-migration.

**Tags are lightweight here**, so `git push --follow-tags` does not push them.
Push release tags by name.

## Notes

Provisioning is now: build updater, build app, one guarded flash, set a
credential, finish setup. The previous procedure required a second updater
build with a destructive `formatfs confirm` console command compiled in, a
manual Kconfig flip in each direction, and a second migration flash to remove
it. Flashing a host-built blank filesystem removed all of that, and with it the
window in which a device carried firmware able to erase its own storage.

This remains a BUILD and CABLE-time decision. It cannot become a switch in the
on-device setup wizard: the partition table is flashed before any application
runs, switching layouts relocates LittleFS and destroys its contents, and the
recovery updater is a separate signed binary paired at build time.
