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
filesystem. The original 16 MB FeatherS3 layout is:

| Partition | Non-OTA 16 MB | OTA 16 MB |
| --- | ---: | ---: |
| `factory` | 5,716 KiB (the app) | 1,336 KiB (recovery updater) |
| `otadata` | - | 8 KiB |
| `ota_0` | - | 5,760 KiB (the app) |
| `littlefs` | 10,604 KiB | 9,216 KiB |

LittleFS gives up 1,388 KiB. In exchange the app slot gains 44 KiB and you get
a recovery updater that cannot be bricked. The arithmetic balances exactly.

The checked-in Headless Node deployment also supports the 8 MB Feather ESP32
V2. It uses a 1,144 KiB golden updater, a 4.5 MiB main slot, and 2,368 KiB of
LittleFS, with a 4 MiB release gate. Its exact feature and migration contract
is in [`deployments/headless/README.md`](../deployments/headless/README.md).

Enable OTA when the device will be updated in the field, has a checked-in OTA
contract, and you control a signing key you can keep for the life of the
deployment.

Do NOT enable it when:

- The board/deployment combination has no checked-in OTA contract. The recovery
  updater has the partition geometry compiled in as constants; a similarly
  sized flash chip is not interchangeable.
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
- A supported board on USB, with the port known. The end-to-end commands below
  document the original FeatherS3 flow; use the deployment wrapper shown below
  for the Headless Feather ESP32 V2.
- ESP-IDF 5.5.x exported in the shell.

Set these once per shell:

```sh
. ~/esp/esp-idf/export.sh
export HW1_BOARD=feathers3
export HW1_KEY=/absolute/path/to/hw1-ota-signing.pem
export HW1_PORT=/dev/cu.YOUR_PORT
export ESPPORT="$HW1_PORT"   # see the note below - idf.py -p does NOT reach these targets
export HW1_MAIN_BUILD=build-ota
export HW1_UPDATER_BUILD=/private/tmp/hw1-updater-migration
```

For the paired Headless Node images, the deployment wrapper replaces the two
manual build steps and runs the pair audit, manifest signing, and bundle
creation as one operation:

```sh
HW1_OTA_SIGNING_KEY="$HW1_KEY" \
  tools/build_deployment.sh headless feather_esp32_v2

# Or Unexpected Maker FeatherS3 / FeatherS3[D]:
HW1_OTA_SIGNING_KEY="$HW1_KEY" \
  tools/build_deployment.sh headless feathers3
```

Its outputs are isolated under `build/deployments/headless/<board>/`.
Installing either deployment partition layout over a cable is still a
destructive, one-time migration; back up LittleFS first as described next.

## Migration is destructive and one-time

Moving a board onto the OTA layout relocates LittleFS. Everything on the
filesystem is lost. Back up first if the device has anything you want, using
`tools/ota/device_backup.py` against the running web server.

Starting from a blank chip is also fine and is what the steps below assume:

```sh
esptool.py --chip esp32s3 -p "$HW1_PORT" erase_flash
```

For the Headless Feather ESP32 V2, select its classic ESP32 target instead:

```sh
esptool.py --chip esp32 -p "$HW1_PORT" erase_flash
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

For the checked-in Headless Feather deployment, use its isolated paired build
directories and preserve the deployment selector when invoking the target:

```sh
HW_DEPLOYMENT=headless/feather_esp32_v2 \
  HW_BOARD=feather_esp32_v2 HW_OTA_LAYOUT=1 \
  HW1_OTA_SIGNING_KEY="$HW1_KEY" \
  HW1_UPDATER_BIN="$PWD/build/deployments/headless/feather_esp32_v2/updater/hw1-updater.bin" \
  ESPPORT="$HW1_PORT" \
  idf.py -B build/deployments/headless/feather_esp32_v2/main migration-flash
```

For FeatherS3[D], substitute `headless/feathers3`, `HW_BOARD=feathers3`, and
`build/deployments/headless/feathers3/...`. Its exact non-interactive phrase is
`MIGRATE headless/feathers3 hw1-hl-f3-ota-v1`; the release directory's
`MIGRATION.md` also records every raw flash offset.

`idf.py -p` is deliberately not used here, and passing it would be a trap.
`-p` is a global option that the BUILT-IN flash actions forward to esptool;
every HardwareOne cable target is a plain CMake custom target, which idf.py
dispatches through a fallback path that drops the port entirely. esptool then
scans and writes whichever device answers first. With one board attached that
is harmless; with two it silently flashes the wrong one. Every guarded target
now prints the port it is about to write and refuses outright when more than
one USB serial device is attached and `ESPPORT` is unset.

The legacy flow prompts for the exact text `MIGRATE feathers3`; its sanctioned
scripted escape remains
`HW1_OTA_MIGRATION_CONFIRM="MIGRATE $HW1_BOARD"`. The Headless Feather flow
instead requires the deployment- and layout-bound phrase
`MIGRATE headless/feather_esp32_v2 hw1-hl-fv2-ota-v1`. If scripting that
destructive migration, set the complete phrase explicitly; do not derive it
from the physical board name alone.

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

For the Headless Feather deployment, keep the deployment identity and use its
isolated updater build:

```sh
HW_DEPLOYMENT=headless/feather_esp32_v2 \
  HW_BOARD=feather_esp32_v2 HW1_OTA_SIGNING_KEY="$HW1_KEY" \
  idf.py -C updater \
    -B build/deployments/headless/feather_esp32_v2/updater \
    -p "$HW1_PORT" monitor
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

On `feathers3_fe` use `encrypted-littlefs-flash`; the plain target is refused
there. Either writes the same blank image over `littlefs` and touches nothing else.
Do not use `idf.py erase-flash` for this: on an OTA device it also erases the
recovery updater (see "`idf.py erase-flash` wipes the recovery updater" below).

## Verifying the result

```sh
python3 tools/ota/check_ota_builds.py --board "$HW1_BOARD" \
  --main-build "$HW1_MAIN_BUILD" --updater-build "$HW1_UPDATER_BUILD"
```

Expect a single OK line naming both versions and one shared key fingerprint.
Run it with the ESP-IDF Python - the system Python lacks `espsecure` and the
audit fails with a confusing signature error.

On the device, `otastatus` should show the running partition as `ota_0`, a
coherent journal, and `recoveryCredentialConfigured` true. Its "Factory
updater" line (`factoryUpdaterVersion` in `otastatus json`) names the recovery
updater in the factory slot, for example `1.1.0+<suffix>`; a manifest whose
`minUpdaterVersion` is newer than that is refused.

## Known rough edges

Checked 2026-11-08 by reading ESP-IDF 5.5.5's idf.py and the target graph of a
configured OTA build. Not run on a device.

**`idf.py erase-flash` wipes the recovery updater.** On this layout the build
refuses the stock flash and erase CMake targets (`flash`, `app-flash`,
`bootloader-flash`, `partition-table-flash`, `erase-otadata`, the
`erase_flash` CMake target and the rest). `idf.py flash` and
`idf.py erase-otadata` run through those targets, so they stop with
"Refusing stock ESP-IDF flash/erase target". `idf.py erase-flash` (and its
old spelling `erase_flash`) is different: idf.py runs esptool's `erase_flash`
itself and never builds a CMake target, so the refusal does not run. The
command succeeds and erases the whole chip: bootloader, partition table, the
`factory` recovery updater, `ota_0`, `otadata`, NVS (which holds the recovery
credential) and `littlefs`.

Do not run it on a provisioned OTA device. Use the targeted cable writes:

- To wipe only the filesystem, use `littlefs-flash` (`encrypted-littlefs-flash`
  on `feathers3_fe`; see above).
- To replace only the recovery updater, use `factory-flash`
  (`encrypted-factory-flash` on `feathers3_fe`).

If the chip was erased anyway, by `idf.py erase-flash` or by a full-erase
install ([COMPATIBILITY.md](COMPATIBILITY.md)), it is now a blank chip.
Nothing boots until it is provisioned again from
[step 3](#3-run-the-guarded-migration-flash). `migration-flash` writes the
bootloader, the partition table, the `factory` updater, `ota_0`, a blank
`otadata` and a blank `littlefs`. Use `encrypted-migration-flash` on
`feathers3_fe`. The existing paired build directories are fine if nothing has
changed since they were built. Then repeat
[step 4](#4-set-the-recovery-credential-and-finish-setup). The erase removed
NVS, so `setpin` (or `otapin`) is needed again, and so is first-time setup.

**Tags are mostly lightweight here** (2026-11-08: 51 of the 57 tags, including
`v0.99.98.4` and `v0.99.99`), so `git push --follow-tags` does not push them.
Push release tags by name.

## Notes

Provisioning is now: build updater, build app, one guarded flash, set a
credential, finish setup. The previous procedure required a second updater
build with a destructive `formatfs confirm` console command compiled in, a
manual Kconfig flip in each direction, and a second migration flash to remove
it. Flashing a host-built blank filesystem removed all of that, and with it the
window in which a device carried firmware able to erase its own storage.

External clients such as the Android companion must size a candidate image
from `slotSize` in `otastatus json` and must not hard-code a slot size (the old
0x5A0000 is larger than the 8 MB layout's 0x500000 `ota_0`).

This remains a BUILD and CABLE-time decision. It cannot become a switch in the
on-device setup wizard: the partition table is flashed before any application
runs, switching layouts relocates LittleFS and destroys its contents, and the
recovery updater is a separate signed binary paired at build time.
