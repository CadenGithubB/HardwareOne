# HardwareOne recovery OTA: operator and test guide

Status: implementation-under-test, not a production qualification record.

Verification snapshot, 2026-08-03: both supported same-key artifact pairs build
and pass the generated paired-artifact gate. The current plain-board pair is a
5,181,440-byte `0.99.7+f3o1` main and 921,600-byte `1.0.0+f3o1` updater. The
last recorded `feathers3_fe` software pair was a 5,115,904-byte main and
921,600-byte updater; it must be rebuilt and requalified after the current
source fixes before use. Audits verify the exact no-SR partition layout,
shared public-key fingerprint `6c361c4cd894e7c5`, required configuration, and
both ESP-IDF image signatures. Host protocol tests and all 14 backup/restore
contract tests pass. A disposable plain FeatherS3 has now passed targeted
migration, direct recovery upload, signature/auth/rate-limit/deadline checks,
same-size digest and native-app-signature failures, interrupted direct upload,
multiple complete probation cycles, unexpected-reset rollback and repair,
result acknowledgement, physical journal reset, and wrong-length
failure/repair. Staged OTA, literal power-loss, watchdog, filesystem-failure,
real backup/restore, and encrypted-board rows remain open; this is not a
production qualification record.

This guide describes the native ESP-IDF implementation in the current source
tree. It intentionally does not repeat older design-plan behavior that was not
implemented. In particular, the current recovery updater is SoftAP-only, keeps
the network off until a credential is provisioned, uses one `ota_0` application
slot, and cannot update itself over the air.

The prospective implementation order, exhaustive change inventory, downstream
impact analysis, lab-only fault-injection design, and production-readiness exit
criteria are maintained separately in
[`OTA_PRODUCTION_READINESS_IMPLEMENTATION_PLAN.md`](OTA_PRODUCTION_READINESS_IMPLEMENTATION_PLAN.md).
That plan is not evidence that a feature or test exists today.

Do the first migration and every destructive test on the spare FeatherS3, with
USB power and serial attached. Do not begin fleet rollout until the matrix in
[Qualification matrix](#qualification-matrix) has been recorded on real
hardware.

## What this system does

The 16 MB flash is divided into a small factory recovery application and one
large HardwareOne application slot:

| Region | Offset | Size | Purpose |
|---|---:|---:|---|
| NVS | `0xA000` | 16 KiB | Recovery credential, OTA journal, and other NVS state |
| Factory | `0x10000` | 1336 KiB | Pure ESP-IDF `hw1-updater` recovery image |
| OTA data | `0x15E000` | 8 KiB | ESP-IDF boot selection and rollback state |
| `ota_0` | `0x160000` | 5760 KiB | Main `hardwareone-idf` application |
| LittleFS, no ESP-SR | `0x700000` | 9216 KiB | Device files |
| Model + LittleFS, ESP-SR | `0x700000` + `0x9F0000` | 3008 KiB + 6208 KiB | Speech model and device files |

The current source has `ENABLE_ESP_SR=0`, and this runbook is qualified only for
that no-ESP-SR layout. Although an OTA/SR partition CSV exists, the guarded
migration target does not back up or populate the relocated `model` partition.
Stop and add an explicit model-partition provisioning and verification step
before migrating any ESP-SR-enabled build.

An update is accepted only when both of these checks pass:

1. The detached manifest has a valid RSA-3072/PSS/SHA-256 signature and matches
   the board, layout, project, version, image size, SHA-256, minimum updater,
   and data-schema contract.
2. ESP-IDF accepts the application's own signed-image signature block.

After installation, the new main image remains rollback-pending. HardwareOne
marks it valid only after setup reaches `RUNNING` and the complete main loop and
core command/storage services remain healthy continuously for 60 seconds. A
five-second heartbeat gap restarts that interval; a 30-second loop hang or a
five-minute setup/probation deadline reboots into rollback handling.

The updater canonicalizes OTA data by selecting factory and then `ota_0` before
every return to main. That makes `ota_0` a fresh trial, so even an unchanged
main image returning from a canceled/manual recovery visit must pass the same
60-second probation again.

This is a recovery-loader design, not A/B firmware. The previous main image is
overwritten. A failed trial returns to the factory updater, not to the prior
HardwareOne version.

## Supported scope

The opt-in OTA layout currently supports only:

| `HW_BOARD` | Layout ID | Main version suffix | Cable targets |
|---|---|---|---|
| `feathers3` | `hw1-f3-ota-v1` | `+f3o1` | `migration-flash`, `ota0-flash` |
| `feathers3_fe` | `hw1-f3fe-ota-v1` | `+f3feo1` | `encrypted-migration-flash`, `encrypted-ota0-flash` |

Both require ESP32-S3 and 16 MB flash. XIAO S3, 8 MB layouts, and classic ESP32
boards are not supported. Do not try to force `HW_OTA_LAYOUT=1` on them.

## Prerequisites

- A spare, known 16 MB FeatherS3 connected by USB.
- The correct serial port, positively identified before any flash command.
- ESP-IDF 5.5 activated.
- `openssl` and Python 3.
- A working current HardwareOne build with a reachable superadmin account for
  the pre-migration file backup.
- A unique RSA-3072 signing key stored outside the repository and backed up.
- Stable USB power for build installation and all first tests.
- An empty directory on encrypted storage for the LittleFS backup, with a
  second encrypted copy. The backup can contain user records, settings,
  certificates, and other device secrets.

### Host qualification tooling

The maintained host tooling is documented in `tools/ota/README.md`. As of
2026-08-04, it can audit artifacts/builds, generate lab-only negative fixtures,
render all STG-001 through STG-018 contracts, probe explicitly selected
serial/ADB interfaces, and create/resume redacted evidence bundles.

Run its host-only suite with:

```sh
python3 -m unittest discover -s tools/ota/tests -v
```

The current `hardware_qualification.py` is intentionally non-destructive. It
cannot upload/stage/apply an image, acknowledge a result, reboot, or flash a
device, and every initialized run records that limitation. Do not treat a dry
run as staged-path hardware evidence. A reviewed executor is the next tooling
phase.

The examples below assume the shell is in the repository root. Set task-specific
paths for the device under test:

```sh
cd /absolute/path/to/hardwareone-idf
source /absolute/path/to/esp-idf/export.sh

export HW1_TEST_BOARD=feathers3
export HW1_TEST_PORT=/dev/cu.usbmodem_REPLACE_ME
export HW1_TEST_KEY=/absolute/off-repository/path/hw1-ota-signing.pem
export HW1_MAIN_BUILD=/private/tmp/hw1-main-feathers3-ota
export HW1_UPDATER_BUILD=/private/tmp/hw1-updater-feathers3-ota
```

For the flash-encrypted board, use `HW1_TEST_BOARD=feathers3_fe` and distinct
build directories. Never reuse plain-board artifacts on that board. Conversely,
do not select `feathers3_fe` for a plain spare unless permanently converting it
is intentional: the first encrypted boot burns flash-encryption eFuses,
encrypts flash in place, and permanently disables JTAG. That cannot be undone
by reflashing a plain build.

## Signing-key handling

The updater, main application, and detached manifest must all use the same
private key. Only the derived public key is embedded in firmware. Losing the
key means future OTA images will be refused; recovery then requires a cable
installation of a new updater and main image built with a replacement key.

For disposable bench testing only, a temporary key can be generated as follows:

```sh
umask 077
openssl genpkey -algorithm RSA \
  -pkeyopt rsa_keygen_bits:3072 \
  -out /private/tmp/hw1-ota-test-key.pem
```

Do not use that temporary path for a real device fleet. A production key should
live in backed-up secret storage, outside this checkout. Never commit a `.pem`,
paste private-key material into a log, or attach the private key to a release.

The build rejects missing, unreadable, or non-RSA-3072 keys. `sdkconfig.ota`,
the per-board updater sdkconfig files, build trees, and the conventional local
key names are ignored, but that is a last guard, not a key-management system.

## Build and validate artifacts

### 1. Build the recovery updater

```sh
HW_BOARD="$HW1_TEST_BOARD" \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
idf.py -C updater -B "$HW1_UPDATER_BUILD" build
```

Expected artifact:

```text
$HW1_UPDATER_BUILD/hw1-updater.bin
```

The updater must fit the 1336 KiB factory slot and the stricter 1.15 MiB
release gate.

### 2. Build the OTA-layout main application

`HW1_UPDATER_BIN` is mandatory for every guarded cable-write target. It enables
the one-time migration target and makes both migration and later `ota_0`
programming depend on the paired-build audit:

```sh
HW_BOARD="$HW1_TEST_BOARD" \
HW_OTA_LAYOUT=1 \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
HW1_UPDATER_BIN="$HW1_UPDATER_BUILD/hw1-updater.bin" \
idf.py -B "$HW1_MAIN_BUILD" build
```

Expected artifacts include:

```text
$HW1_MAIN_BUILD/hardwareone-idf.bin
$HW1_MAIN_BUILD/hw1_ota_public_key.pem
$HW1_MAIN_BUILD/partition_table/partition-table.bin
$HW1_MAIN_BUILD/ota_data_initial.bin
```

The OTA build uses the isolated root `sdkconfig.ota`; it does not intentionally
change the ordinary non-OTA `sdkconfig`. A build directory is permanently
marked `ordinary` or `recovery-ota` at first configure and refuses a later mode
switch. Use a fresh `-B` directory when changing modes. Configure also asserts
all load-bearing rollback, signed-image, partition, and board-encryption
settings rather than relying on defaults alone.

### 3. Run the build-contract audit

Run this after every paired updater/main build, before flashing or publishing
an artifact:

```sh
python3 tools/ota/check_ota_builds.py \
  --board "$HW1_TEST_BOARD" \
  --main-build "$HW1_MAIN_BUILD" \
  --updater-build "$HW1_UPDATER_BUILD"
```

Do not continue unless it prints `OK`. The audit checks target, flash size,
partition-table offset, rollback and signed-app settings, flash-encryption/NVS
agreement (including flash-encryption-protected NVS keys on `feathers3_fe`),
matching ESP-IDF revisions, generated public-key DER fingerprints, both
signed-image signatures,
board/version suffixes, project identities, image size gates, identical blank
OTA-data initializers, and both emitted partition tables against the exact
`partitions_ota_no_sr_16mb.csv` bytes generated by the build's ESP-IDF. An
ESP-SR artifact therefore fails closed until its migration path is explicitly
supported. As a transparent independent cross-check, you can also run:

```sh
rg -n '^#define ENABLE_ESP_SR[[:space:]]+0$' \
  components/hardwareone/System_BuildConfig.h

export HW1_EXPECTED_PART_TABLE=/private/tmp/hw1-expected-no-sr-partitions.bin
python "$IDF_PATH/components/partition_table/gen_esp32part.py" \
  --flash-size 16MB \
  partitions_ota_no_sr_16mb.csv \
  "$HW1_EXPECTED_PART_TABLE"
cmp "$HW1_EXPECTED_PART_TABLE" \
  "$HW1_MAIN_BUILD/partition_table/partition-table.bin"
```

Both commands must exit successfully. Treat a difference as a stop condition.
The generated `hw1-ota-pair-audit` target runs this same check and is a required
dependency of every supported cable-write target.

### 4. Create and independently verify the detached manifest

```sh
export HW1_TEST_IMAGE="$HW1_MAIN_BUILD/hardwareone-idf.bin"
export HW1_TEST_MANIFEST="$HW1_MAIN_BUILD/hardwareone-idf.manifest.json"

python3 tools/ota/make_manifest.py create \
  --image "$HW1_TEST_IMAGE" \
  --key "$HW1_TEST_KEY" \
  --output "$HW1_TEST_MANIFEST" \
  --board "$HW1_TEST_BOARD" \
  --min-updater 1.0.0 \
  --data-schema 1

python3 tools/ota/make_manifest.py verify \
  --manifest "$HW1_TEST_MANIFEST" \
  --image "$HW1_TEST_IMAGE" \
  --public-key "$HW1_MAIN_BUILD/hw1_ota_public_key.pem"
```

The defaults are updater `1.0.0`, data schema `1`, and a `0x5A0000` byte slot.
The current device policy accepts exactly data schema 1; there is not yet a
general data migration engine behind that field.

Keep image and manifest together as a board-specific pair. This repository's
release policy is source-only, so signed board binaries are local operator
artifacts unless that policy is deliberately changed.

## One-time layout migration

The first OTA-layout installation is the only routine that moves LittleFS. It
requires a cable. It writes the bootloader, partition table, factory updater,
blank OTA data, and main image. It deliberately does not write NVS and does not
format the relocated LittleFS partition.

Never add `erase-flash`, `erase_flash`, or a whole-chip erase to this procedure.

### 1. Back up files before changing the layout

While the old main application and its web server still work, run:

```sh
umask 077
python3 tools/ota/device_backup.py \
  --url https://REPLACE_WITH_DEVICE_IP \
  --username REPLACE_WITH_SUPERADMIN \
  backup /absolute/path/to/empty/hw1-file-backup
```

The tool prompts for the password by default, so the secret does not need to be
placed in shell history. `--password-env NAME` is available for controlled
automation. Prefer HTTPS with a certificate the operator can verify. Use
`--insecure` only for a physically controlled, isolated network with a known
self-signed endpoint. If the installed main exposes only HTTP, perform the
backup on an isolated trusted network: the real superadmin credentials and all
downloaded device data otherwise cross the LAN in plaintext.

Keep the destination on encrypted storage with owner-only permissions. The
backup is file-level and can contain credentials, certificates, user records,
and other sensitive device state. Control its copies like a secret and securely
dispose of them after the retention period.

The helper:

- traverses internal files through authenticated device APIs;
- stores size and SHA-256 for each file in `hw1-file-backup.json`;
- skips `/sd` because the migration does not touch the SD card;
- skips `/system/ota` because staged firmware is disposable;
- does not read or back up NVS.

The main file-read endpoint has a legacy behavior in which some errors arrive
as HTTP 200 text. The helper now fails closed by requiring the endpoint's
success content-type/charset marker, rejecting every known error body, and
comparing each response length with the size from the directory listing before
writing it. It then reads the local copy back and records its SHA-256. The host
test suite covers the known error strings even when their length exactly
matches the listing. Before writing the manifest or printing completion, it
also requires one canonical, parseable `users.json` and proves that the backup
login username will remain authorized after that roster is restored.

This is still a sequential file export, not a transactionally frozen snapshot,
and the device listing does not provide a source-side hash. Quiesce settings,
capture, and user mutations during backup. Inspect `hw1-file-backup.json`, its
file count, paths, and representative sensitive/irreplaceable files, then make
a second encrypted copy of the entire backup directory. A legitimate file
whose complete content is exactly one of the legacy error strings is refused
on purpose and must be exported and verified separately. If important SD
content has no other copy, back it up separately even though migration does not
write the card.

Run the helper's host contract tests before using it for migration:

```sh
python3 -m unittest discover -s tools/ota/tests -p 'test_*.py' -v
```

They must pass, but they do not replace a backup/restore drill against the
actual firmware on the spare device.

After the file-level backup is complete, capture a read-only cable baseline on
the same encrypted operator storage. This is a device-specific forensic safety
net for diagnosing a failed migration, not a substitute for the portable file
backup and not blanket authorization to write a raw image back:

```sh
esptool.py --chip esp32s3 -p "$HW1_TEST_PORT" flash_id
esptool.py --chip esp32s3 -p "$HW1_TEST_PORT" \
  read_flash 0x0 0x1000000 \
  /absolute/encrypted/path/pre-migration-flash.bin
shasum -a 256 /absolute/encrypted/path/pre-migration-flash.bin
```

Record the detected chip, flash size, port, and digest. A dump from a board
whose flash encryption is already active is encrypted and device-specific; a
pre-conversion plaintext dump can also become non-restorable after
`feathers3_fe` burns encryption eFuses. Never improvise a whole-flash restore.
If ROM security settings refuse the read, record that fact and do not weaken
them merely to obtain a dump.

### 2. Build a migration-only updater with explicit format support

The relocated LittleFS contains unrelated old-layout bytes and must be
explicitly formatted once. The only firmware command allowed to do that is a
build-gated, physical-serial command in the updater.

Configure the updater build:

```sh
HW_BOARD="$HW1_TEST_BOARD" \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
idf.py -C updater -B "$HW1_UPDATER_BUILD" menuconfig
```

In `HardwareOne recovery updater`, enable:

```text
Enable migration-only serial 'formatfs confirm' command
```

Then rebuild the updater and main and rerun `check_ota_builds.py` using the
commands in [Build and validate artifacts](#build-and-validate-artifacts).

This setting is for the one migration boot only. A qualified device must not be
left with this updater in its factory slot; step 5 replaces it with a build in
which the command is disabled.

### 3. Run the guarded migration flash

For a plain FeatherS3:

```sh
HW_BOARD="$HW1_TEST_BOARD" \
HW_OTA_LAYOUT=1 \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
HW1_UPDATER_BIN="$HW1_UPDATER_BUILD/hw1-updater.bin" \
ESPPORT="$HW1_TEST_PORT" idf.py -B "$HW1_MAIN_BUILD" migration-flash
```

For `feathers3_fe`, use the same environment but replace the final target with:

```text
encrypted-migration-flash
```

The plain target intentionally fails on `feathers3_fe`. Do not work around that
guard. Development-mode flash encryption still needs the encrypted target for
the bootloader, updater, and main image.

The preflight reruns the paired-build audit, then asks for the exact text:

```text
MIGRATE feathers3
```

or:

```text
MIGRATE feathers3_fe
```

Read the resolved port, board, key path, binary paths, and preflight output
before entering the confirmation. Keep USB connected until flashing and the
automatic reset complete.

### 4. Provision recovery and format the new LittleFS over serial

The blank OTA data causes the factory updater to boot. Open its serial console:

```sh
HW_BOARD="$HW1_TEST_BOARD" \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
idf.py -C updater -B "$HW1_UPDATER_BUILD" \
  -p "$HW1_TEST_PORT" monitor
```

At the `hw1up>` prompt, run:

```text
status
setpin REPLACE_WITH_A_UNIQUE_12_TO_63_CHAR_PASSPHRASE
formatfs confirm
status
cancel
```

`setpin` stores the same persistent credential for recovery WPA2 and HTTP Basic
or Bearer authentication. The updater never prints it back. Use printable ASCII
and do not reuse a device login password. If the recovery network was already
running, the new value takes effect only after `reboot`; the console reports
that condition explicitly.

`formatfs confirm` destroys the contents of the new LittleFS partition. It is
correct only here, after the guarded backup has completed and its manifest and
representative files have been independently inspected. It is never automatic
and is never available over HTTP.

`cancel` selects the valid main image and reboots. `reboot` would only restart
the factory updater and is not the command to leave recovery.

### 5. Replace the migration-only updater

Immediately disable `CONFIG_HW1_UPDATER_ENABLE_SERIAL_FORMATFS` in updater
`menuconfig`, use a fresh updater build directory, and rebuild. Updater
`menuconfig` writes the shared source file `updater/sdkconfig.<board>`; a fresh
build directory alone does not reset this setting.

```sh
export HW1_UPDATER_PROD_BUILD=/private/tmp/hw1-updater-feathers3-production

HW_BOARD="$HW1_TEST_BOARD" \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
idf.py -C updater -B "$HW1_UPDATER_PROD_BUILD" menuconfig

HW_BOARD="$HW1_TEST_BOARD" \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
idf.py -C updater -B "$HW1_UPDATER_PROD_BUILD" build
```

Verify both the shared sdkconfig and generated header explicitly:

```sh
rg -n '^# CONFIG_HW1_UPDATER_ENABLE_SERIAL_FORMATFS is not set$' \
  "updater/sdkconfig.$HW1_TEST_BOARD"

if rg -q '^#define CONFIG_HW1_UPDATER_ENABLE_SERIAL_FORMATFS 1$' \
  "$HW1_UPDATER_PROD_BUILD/config/sdkconfig.h"; then
  printf '%s\n' 'STOP: production updater still includes formatfs'
  false
fi
```

The first search must match and the second must not. The paired-build audit
does not check this option. Run that audit separately with
`--updater-build "$HW1_UPDATER_PROD_BUILD"`, then force the main build to
reconfigure with the production updater path:

```sh
HW_BOARD="$HW1_TEST_BOARD" \
HW_OTA_LAYOUT=1 \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
HW1_UPDATER_BIN="$HW1_UPDATER_PROD_BUILD/hw1-updater.bin" \
idf.py -B "$HW1_MAIN_BUILD" reconfigure
```

The explicit `reconfigure` is required because changing an environment variable
alone does not guarantee that an existing CMake target is regenerated. Now
rerun the guarded migration target with the same environment and production
updater path. Use `migration-flash` for `feathers3` or
`encrypted-migration-flash` for `feathers3_fe`:

```sh
HW_BOARD="$HW1_TEST_BOARD" \
HW_OTA_LAYOUT=1 \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
HW1_UPDATER_BIN="$HW1_UPDATER_PROD_BUILD/hw1-updater.bin" \
ESPPORT="$HW1_TEST_PORT" idf.py -B "$HW1_MAIN_BUILD" migration-flash
```

For `feathers3_fe`, replace only the final target with
`encrypted-migration-flash`.

This second guarded flash replaces the factory updater and rewrites the same
boot/table/main/OTA-data regions. It still does not touch NVS or the newly
formatted LittleFS. Because the prior migration visit ended as `canceled`, the
production updater should automatically select the valid compatible main image.
If it holds instead, inspect serial `status` and use `cancel` only when status
shows that returning to main is safe.

There is currently no dedicated cable target that updates only the factory
updater. Reusing the guarded migration target is the fail-closed supported path
in this tree.

### 6. Bootstrap the empty filesystem and restore files

The new filesystem is empty. The restore helper requires an authenticated main
web server, so complete the minimum first-time setup needed to create a
temporary superadmin and reach the device over the network. Use **the same
username as a superadmin present in the backed-up `users.json`** (preferably the
owner); its temporary password may differ. For a legacy roster with no explicit
superadmin, use the first/owner username; its role must be absent or `admin` to
satisfy both current firmware checks. Before creating directories or uploading
anything, the helper validates that this exact username will remain both admin
and superadmin-equivalent under the backed-up roster. It rejects missing,
malformed, duplicate, ordering-incompatible, or ambiguous identity records. It
restores and read-back verifies every other file first, then restores
`users.json` last. A differently named bootstrap user therefore fails before
device mutation instead of losing access midway through restore.

The pending main trial has only five minutes to reach `RUNNING`, followed by 60
continuous healthy seconds. Complete bootstrap promptly. If it reboots before
validation, factory recovery should hold; inspect serial `status`, use `cancel`
to re-arm the same valid compatible main, and try bootstrap/probation again.

Then run:

```sh
python3 tools/ota/device_backup.py \
  --url https://REPLACE_WITH_DEVICE_IP \
  --username REPLACE_WITH_BOOTSTRAP_SUPERADMIN \
  restore /absolute/path/to/empty/hw1-file-backup
```

Apply the same HTTPS/isolated-network rule used for backup; restore transmits
the sensitive files and bootstrap credentials too.

Before uploading, the tool verifies every local file against the backup
manifest. After each upload, it reads the file back and verifies its size and
SHA-256. Directory creation is idempotent, so directories created by first-time
setup do not invalidate the restore.

The restore overwrites temporary users/settings files with the originals.
After it completes, restart the main application, reconnect using the restored
network/account configuration, and verify representative settings, users,
captures, and sealed health data. Keep the encrypted backup until the device
has passed multiple boots and at least one successful OTA cycle, then follow
the chosen secure retention/disposal policy.

NVS is preserved by the migration target but is not backed up by this helper.
If the target reports an NVS error, stop: neither application is allowed to
erase NVS automatically.

## Post-migration cable development loop

Once a board has the OTA partition table and production factory updater, flash
main-application iterations only to `ota_0`:

```sh
HW_BOARD="$HW1_TEST_BOARD" \
HW_OTA_LAYOUT=1 \
HW1_OTA_SIGNING_KEY="$HW1_TEST_KEY" \
HW1_UPDATER_BIN="$HW1_UPDATER_PROD_BUILD/hw1-updater.bin" \
ESPPORT="$HW1_TEST_PORT" idf.py -B "$HW1_MAIN_BUILD" ota0-flash
```

Use `encrypted-ota0-flash` for `feathers3_fe`. The plain targets intentionally
fail there. Use the exact production updater path paired with this main build;
the cable target reruns the paired-artifact audit before it can open the serial
port. Stock `flash`, `app-flash`, bootloader/partition flash, encrypted stock
variants, `erase_flash`, and the `erase-otadata`/`erase_otadata` build targets
are disabled for this layout because their default behavior is unsafe or
bypasses the guarded workflow. The deliberate `otatool.py erase_otadata`
recovery-entry procedure below is separate and remains an operator-only lab
step. A direct cable write also bypasses the recovery manifest/journal
transaction, so it is a developer iteration path, not evidence that OTA or
rollback works.

## Routine staged update

This is the preferred path when LittleFS has room for the application image.
It validates the complete pair in the main application before selecting
recovery, then the updater validates it again before and after writing.

1. Build and verify an image/manifest pair as described above.
2. Package the verified pair as a deterministic `.hw1ota` bundle with
   `tools/ota/make_bundle.py` and place it on the phone through any user-managed
   offline transfer.
3. In the Android companion, connect through the HardwareOne Secure Channel,
   sign in as a superadmin, open **Devices → OTA**, and select the local bundle
   with Android's system document picker.
4. Start the update. The app uses the dedicated encrypted-BLE `otawrite`
   protocol to stream `candidate.part` and then `manifest.part`; generic file
   commands are intentionally denied access to `/system/ota`.
5. The app runs and verifies the equivalent command sequence:

   ```text
   otastatus
   otastage confirm
   otapin REPLACE_WITH_A_UNIQUE_12_TO_63_CHAR_PASSPHRASE   # serial console only
   otaupdate confirm
   ```

   `otapin` is restricted to the physical serial console and is normally issued
   once during cable bring-up; the other three steps work over any transport.

`/system/ota` is inaccessible to ordinary admins. `otastage` validates both
`.part` files, renames them separately to `candidate.bin` and `manifest.json`,
revalidates the promoted pair, and only then starts the durable OTA journal.
The two renames are individually filesystem-atomic but are not one transaction;
if either promotion fails, re-upload both `.part` files before retrying. If
intentionally installing an older signed release, the explicit staging command
is:

```text
otastage confirm allow-downgrade
```

The BLE upload contract is byte-exact and independent of generic file writes.
`otawrite begin <candidate|manifest> <size> <sha256>` fixes the only two legal
member names, expected length, and transport digest. Each encrypted binary
frame contains the marker `00 48 57 31 4f 54 41 01`, a big-endian 32-bit file
offset, and raw payload. Small frame bursts are followed by `otawrite status`;
the app resumes from the device-reported offset if queue pressure dropped a
frame. `otawrite finish` closes the file only after the exact length and
SHA-256 match. Disconnect, explicit abort, or 30 seconds without activity
closes and removes the active partial. Finishing the candidate preserves it
for the following manifest transfer; starting a new candidate removes any
older manifest partial so releases cannot be accidentally mixed.

`otawrite` is accepted only from an established encrypted BLE channel whose
connection still owns a live superadmin login. It is forbidden from
automations and unavailable in the ordinary partition layout. The phone and
device each serialize GATT operations; the app limits a burst to three raw
frames before requesting a checkpoint so the firmware command queue retains
headroom for the checkpoint and normal control traffic.

If `otastatus` reports an unacknowledged terminal result, review it and run
`otaack <resultSequence> confirm` before `otastage`. Naming the sequence makes
the acknowledgement fail if a different terminal result appeared after the
operator's review. Normal staging and direct-recovery commands fail closed
while `resultPending` is true so a later result cannot silently replace the one
durable result slot.

`otaupdate` revalidates the pair and requires either a fresh USB-present sample
or a fresh battery reading of at least 30 percent. It records an explicit
`force-power` override if invoked as:

```text
otaupdate confirm force-power
```

That override is an emergency lab option, not a normal workflow. Keep stable
USB power connected instead. The updater samples power again before it erases
or writes `ota_0` unless the persisted `force-power` override was recorded;
that override deliberately bypasses the updater's second power gate too.

The recovery updater automatically applies a journaled staged pair. Serial
`apply` and the recovery page's `Apply read-only staged pair` button are manual
resume controls, not required in a normal success path.

After the new main application returns, leave it running for at least 60
healthy seconds. Then run:

```text
otastatus json
otaack REPLACE_WITH_RESULT_SEQUENCE confirm
```

Pass criteria are `phase=succeeded`, `resultCode=success`,
`resultPending=true`, the expected board and layout, `runningPartition=ota_0`,
and the expected version in the ordinary firmware status. Review the result
before acknowledging it; a later status must then show `resultPending=false`.
Successful validation removes the promoted staged image and manifest.

Use `otacancel confirm` only while a staged request is still in `requested`
state and before recovery boot has been armed.

## Routine direct recovery upload

This path does not need a staged image on LittleFS. It is the normal rescue when
the main application is still able to request recovery, and the recovery path
used after a failed trial.

From the running main application:

```text
otapin REPLACE_WITH_A_UNIQUE_12_TO_63_CHAR_PASSPHRASE   # serial console only
otarecovery confirm
```

If the credential is already provisioned, only `otarecovery confirm` is needed
and it can be issued from any transport.

As with staging, first review and acknowledge any pending terminal result.
`otarecovery` refuses an ordinary new operation while `resultPending` is true.

For an intentional downgrade:

```text
otarecovery confirm allow-downgrade
```

After reboot:

1. Join `HW1-Recovery-XXXX`; `XXXX` is derived from the SoftAP MAC.
2. Use the recovery credential as the WPA2 password.
3. Open `http://192.168.77.1/`.
4. At the HTTP Basic prompt, use username `admin` and the same recovery
   credential.
5. Select the signed manifest first and its exact matching
   `hardwareone-idf.bin` second.
6. Keep USB power connected and press `Verify manifest and install image`.

The page posts the manifest to `/manifest` and then streams the exact binary to
`/firmware`. The updater checks the manifest before erasing, requires the binary
content length and application descriptor to match it, samples power, hashes
the full stream, asks ESP-IDF to validate the signed image, verifies the written
partition, and only then arms the trial boot.

For a scripted bench upload, `curl --user admin` prompts for the HTTP password
instead of embedding it in shell history:

```sh
curl --fail --user admin \
  -H 'Content-Type: application/json' \
  --data-binary @"$HW1_TEST_MANIFEST" \
  http://192.168.77.1/manifest

curl --fail --user admin \
  -H 'Content-Type: application/octet-stream' \
  --upload-file "$HW1_TEST_IMAGE" \
  http://192.168.77.1/firmware
```

Authenticated `GET /status` reports layout, journal, main-image state,
candidate version, and byte progress. Manifest receive has a 45-second absolute
deadline and the firmware receive/apply deadline is 10 minutes. Five failed
HTTP authentications within one minute cause a 30-second block.

If the network drops during direct upload, reconnect and start again with the
manifest. The old main may already have been erased, so recovery must remain
available until a good pair is installed.

## Recovery serial console

The factory updater uses USB Serial/JTAG at 115200 and prints `hw1up>`.

| Command | Meaning |
|---|---|
| `status` | Print updater, layout, journal, main-image, network, and progress JSON |
| `setpin <12..63 printable>` | Set/replace the persistent WPA2 and HTTP credential; start SoftAP if it was off, or require reboot if it was already running |
| `apply` | Apply/resume the authenticated staged pair already journaled by the main app |
| `cancel` | Cancel a safe recovery visit and select a valid compatible main image |
| `reboot` | Restart the factory updater; it does not mean return to main |
| `allowdowngrade confirm` | Persist downgrade permission for the active recovery transaction |
| `resetjournal confirm` | Erase only the two `hw1up` transaction slots; keep credential and other NVS data |
| `formatfs confirm` | Present only in an explicitly enabled migration build; format all LittleFS |

The main application has a separate serial-only repair command,
`otaresetjournal confirm`. Use either journal reset only when status reports the
journal corrupt and ordinary cancel/retry cannot proceed. It intentionally does
not erase credentials, settings, NVS keys, or device files.

## Entering recovery

Preferred entry paths are:

- `otarecovery confirm` for direct upload;
- `otaupdate confirm` for a staged update;
- automatic rollback after a bad pending image;
- automatic crash-loop escape after three consecutive pre-healthy main crashes;
- automatic recovery when main LittleFS cannot mount.

There is no implemented recovery button. On an already migrated lab device, a
cable operator can deliberately erase only OTA data to make the next boot pass
through factory:

```sh
python "$IDF_PATH/components/app_update/otatool.py" \
  --port "$HW1_TEST_PORT" \
  --partition-table-file "$HW1_MAIN_BUILD/partition_table/partition-table.bin" \
  erase_otadata
```

This is a developer boot-path tool, not a normal update step and not an
unconditional recovery hold. If the durable journal is already `succeeded` or
`canceled` and main is valid, the updater intentionally selects main and returns
immediately. Verify that the device already has the matching OTA layout first.
Running it with a new partition table against an unmigrated device can erase
bytes that belong to the old application. It resets boot-selection state but
does not erase NVS or LittleFS.

If both `ota_0` and the factory updater are damaged, rerun the guarded cable
migration target with known-good paired artifacts. It preserves NVS and the
current relocated LittleFS. Use `encrypted-migration-flash` for
`feathers3_fe`.

## Qualification matrix

Record board, commit, ESP-IDF version, key fingerprint, main/updater versions,
artifact hashes, power source, exact interruption point, observed status, and
pass/fail for every row. Start on `feathers3`; repeat all relevant rows on
`feathers3_fe` after the plain-board matrix is green.

### Recorded plain-board bench slice — 2026-08-03

This is development evidence from a dirty working tree, not release evidence.
The base source revision was `ee87ea7`; the tested fixes were still uncommitted.
The board was a USB-powered plain `feathers3` running ESP-IDF v5.5.1. The exact
main was `0.99.7+f3o1`, 5,181,440 bytes, SHA-256
`32ead128dde696ad9c27740ab6451f0454432078e11f6424360241f1805c8617`.
The final factory updater was `1.0.0+f3o1`, 921,600 bytes, SHA-256
`dd067a4498132e2043224a0983250c34b37a1002c532b9a85c5cbcd860049976`.
The paired-artifact audit passed with public-key fingerprint
`6c361c4cd894e7c5`.

Observed passes: targeted layout installation and explicit one-time format;
production updater with `formatfs` disabled; NVS credential and LittleFS setup
retention; Bearer/Basic failure, safe-origin, five-failure rate limit and
30-second recovery; absolute 45-second incomplete-manifest deadline;
one-bit-corrupted manifest signature rejection without journal/write mutation;
exact signed direct upload through a Pixel client; signed-image and written
partition verification; real OTA command availability; two complete 60-second
probations; durable success/result reporting; wrong acknowledgement rejection
and exact acknowledgement; physical journal-slot reset; and wrong-length
firmware rejection transitioning durably to `FAILED`, followed by a fresh
manifest and successful repair; same-size direct-stream digest rejection after
the full write; a manifest-authenticated but natively invalid app rejection at
`esp_ota_end`; and an interrupted direct upload that failed in about three
seconds after 127,264 bytes, held, and accepted a fresh repair. An unexpected
USB reset during probation (a brownout/reset proxy, not a literal power cut)
left `ota_0` `ABORTED`; factory recovery recorded the rollback and held.

That rollback exposed a repair defect: after fully rewriting and verifying the
candidate, the updater's ordinary return-eligibility check still rejected the
stale `ABORTED` OTA-data state, so boot selection failed with
`ESP_ERR_INVALID_STATE`. The fixed updater permits that state only from the
freshly verified finalization path, erases stale selection metadata by selecting
factory, then creates a new `ota_0` trial. Repeating the exact repair produced
HTTP 200, a complete 60-second probation, durable success, and exact result
acknowledgement. Ordinary cancel/return paths still reject `ABORTED` and
`INVALID` images.

The host remained on its ordinary Wi-Fi throughout; Pixel cleanup restored
mobile data and forgot every temporary recovery profile. Android raw `nc`
occasionally lost the final HTTP body when the updater rebooted. The harness is
now bounded and records that case as `PROBATION_PENDING`, requiring USB evidence
instead of falsely reporting either HTTP success or a multi-minute timeout.

Not run in this slice: staged OTA, wrong-board/layout, interrupted staging,
literal controlled power loss during write/trial, low/unknown power,
purpose-built trial crash/setup/loop hangs, post-commit crash-loop escape,
15-minute idle/failed holds, LittleFS/factory corruption, forced watchdog
stalls, hardware backup/restore, and every `feathers3_fe` row.

| Test | Method | Required result |
|---|---|---|
| Paired build contract | Build updater/main with one key; run `check_ota_builds.py`, the explicit `ENABLE_ESP_SR=0` check, and emitted partition-table comparison | Audit prints `OK`; both artifacts fit their gates; artifact is the exact expected no-SR layout |
| Cable-target guards | Inspect/dry-run stock and guarded target graphs before connecting a board; then use a disposable board | Stock flash/erase commands fail before serial access; safe targets require a matched updater and write only their declared regions/slot |
| First-layout backup | Quiesce file mutations; back up old files; inspect manifest and representative files; retain a second encrypted copy | Known HTTP-200 error bodies, malformed sizes, and list/read size races abort before archive; expected files/count/content are present; no `/sd` or `/system/ota`; no migration yet |
| First-layout migration | Run guarded target, format once by serial, install production updater, restore | Main runs from `ota_0`; NVS state survives; restored files hash/read correctly; production updater was built with `formatfs` disabled |
| Successful staged update | Select a verified `.hw1ota` in Android; encrypted BLE uploads both exact members; stage and apply | Checkpoints advance monotonically; recovery revalidates/writes; main passes 60 s; status is `succeeded/success` |
| Result acknowledgement | After a terminal result, try normal stage/direct recovery before and after `otaack <resultSequence> confirm`; separately exercise marked crash-loop emergency supersession | Normal operation refuses while `resultPending=true`, stale/wrong sequences are rejected, an exact acknowledgement proceeds, and emergency supersession is visible/behaves as the documented one-slot policy |
| Successful direct update | Enter recovery, upload manifest then image from phone/computer | SoftAP auth works; exact pair installs; main passes probation |
| Bad manifest signature | Mutate the manifest's signed payload/signature field or use another key | `/manifest` or `otastage` rejects before `ota_0` is erased; installed main remains bootable |
| Wrong board/layout | Present an artifact pair for the other board variant | Main and updater refuse it before write |
| Binary/manifest mismatch | Pair a valid manifest with another image | Staged mismatch is refused before recovery; a same-size direct-stream SHA mismatch is caught after write and recovery holds |
| Invalid ESP app signature | On the sacrificial board only, sign a manifest for an intentionally corrupted app image | Detached manifest passes, ESP-IDF signed-image check fails, recovery holds; a good direct upload restores service |
| Interrupted staging upload | Disconnect BLE or stop the app during candidate/manifest transfer, then wait beyond the cleanup bound | Active partial closes/removes; `otastage` refuses an incomplete pair; current main remains selected; a fresh upload succeeds |
| Power loss after recovery is armed | Cut power after `otaupdate` reports reboot but before apply begins | Next boot enters recovery and resumes the journaled staged operation |
| Power loss during staged erase/write | Watch serial/status for nonzero partial progress, then cut power | Next boot remains in recovery, revalidates the read-only staged pair, and resumes to success |
| Power loss after write/verify | Interrupt as close as practical after full progress but before trial boot | Boot lands in recovery or the exact authenticated trial; it never boots an unrelated image |
| Direct-upload network loss | Break WiFi during `/firmware` PUT | Updater records failure/holds; reconnect, resend manifest and complete good upload |
| Direct-upload power loss | Cut power during a sacrificial direct write | Partial main never executes; recovery returns and accepts a fresh pair |
| Low/unknown power | With USB absent, exercise the pre-write gate below 30 percent or with unavailable gauge | Update refuses before erase/write; reconnect USB and retry |
| Trial crash | Install a signed test image that aborts before the 60-second mark | Bootloader rejects the trial and factory recovery holds with rollback/failure result |
| Trial setup hang | Install a signed test image that never reaches `RUNNING` | Supervisor reboots by five minutes; recovery is reachable |
| Trial loop hang | Install a signed test image that stops complete-loop heartbeats | Supervisor reboots within about 30 seconds; recovery is reachable |
| Power loss during 60-second trial | Cut power before the image is marked valid | Trial is not silently accepted; bootloader returns to recovery and reports rollback |
| Data rollback compatibility | During a sacrificial trial, write representative NVS/LittleFS data, fail the trial, then install a known-good older `dataSchema=1` image | The known-good image reads the data without erasing or corrupting it; no irreversible schema write occurs before validation |
| Post-commit crash loop | Use a signed test image that first passes probation, then crashes before healthy on three consecutive boots | Early crash counter redirects `ota_0` to authenticated recovery |
| Recovery idle return | Enter direct recovery, take no action for 15 minutes with a valid main | Transaction cancels and updater selects main |
| Failed recovery hold | Cause a verified update failure and wait beyond 15 minutes | Updater remains held for authenticated repair; it does not select invalid main |
| Auth rate limit | Submit five bad HTTP credentials within one minute, then make a sixth request | The fifth bad request returns 401 and activates the block; the sixth returns 429 for about 30 seconds; correct credential works afterward |
| Journal corruption drill | On sacrificial hardware, corrupt only the OTA journal records, then use physical serial reset | Network mutation is insufficient; serial reset restores an empty journal without erasing credential/data |
| LittleFS mount failure | Corrupt/withhold LittleFS on sacrificial hardware | Neither app formats it automatically; main selects recovery; direct path remains available if NVS credential works |
| Factory updater corruption | Corrupt factory on sacrificial hardware | Cable migration with known-good artifacts restores recovery without bulk-erasing NVS/LittleFS |
| FE parity | Repeat migration, staged, direct, rollback, and power interruption with `feathers3_fe` encrypted targets | No plaintext target succeeds; NVS/OTA state and recovery survive development-mode flash encryption |

The current source has no deterministic fault-injection hook for the
millisecond-scale windows around `esp_ota_end`, OTA-data updates, or reboot.
Hand-timed power pulls can cover coarse windows but cannot prove those exact
boundaries. Add test-only halt/injection points or an external programmable
power rig before claiming exhaustive power-loss qualification.

## Result and recovery interpretation

Main `otastatus [json]` is the durable operator record. Useful phases include
`requested`, `recovery_boot_armed`, `recovery_running`, `applying`,
`image_verified`, `trial_boot_armed`, `trial_running`, `succeeded`, `failed`,
and `canceled`. JSON includes `resultPending`. Do not acknowledge a result until
its code/detail and installed version have been reviewed. Normal `otastage` and
`otarecovery` calls reject while it is true.

The v1 journal has one terminal-result slot. Availability-critical crash-loop
recovery, filesystem-failure recovery, and a recovery upload after a manually
forced factory boot may set a recorded emergency-supersession flag. The prior
result remains visible while that emergency operation is active, but the new
terminal result then replaces it. This prevents an old result from blocking
rescue indefinitely, but it is not a multi-entry audit history; export operator
logs before proceeding when durable fleet history matters.

Recovery behavior by failure class:

| Failure | Expected recovery |
|---|---|
| Incomplete or bad staged files | Main remains bootable; upload both `.part` files again |
| Manifest refused before direct write | Correct the artifact/board/key pair and repost manifest |
| Direct write interrupted or app-signature failure after erase | Stay in factory recovery and upload a known-good pair directly |
| Trial crash/hang/power interruption | Factory recovery holds; upload a known-good pair |
| Valid main, deliberate recovery visit, no action | `cancel`, or wait for the 15-minute idle return |
| Failed transaction but prior main still fully valid/compatible | Recovery holds for review; explicit `cancel` may return to main while retaining the failed result |
| Corrupt transaction journal | Physical serial `resetjournal confirm`, then start a new operation |
| LittleFS unreadable | Direct upload can replace main but does not repair files; restore/format requires explicit migration recovery work |
| NVS init failure | Network credentials and journal are unavailable; stop and use cable diagnostics - no auto-erase is permitted |
| Factory updater damaged | Guarded cable migration with known-good updater/main |
| Signing key lost or rotated | Guarded cable migration of updater and main with the new shared key |

## Security properties and explicit limitations

- Signed-app verification is enabled without Secure Boot eFuses. It protects
  the OTA path, not an attacker with physical cable access.
- There is no anti-rollback. Downgrades require an explicit per-transaction
  option, but a physically present operator can cable-flash anything.
- There is one main slot. A failed write or trial does not restore the previous
  main image; it restores reachability through the factory updater.
- The factory updater, bootloader, and partition table are cable-only. There is
  no updater self-update protocol.
- Recovery is SoftAP-only. It does not join a saved infrastructure network and
  offers no BLE, ESP-NOW, GitHub-pull, or TLS transport. BLE is a main-app
  staging transport only; once recovery starts, apply is local from LittleFS.
- Recovery networking stays off until a valid 12-63 printable-character
  credential exists in NVS. Contrary to the older plan, there is no open rescue
  AP after NVS loss. If NVS still works, physical serial `setpin` restores the
  network path.
- The current implementation stores that credential as the same plaintext NVS
  string twice (`ap_pass` and `auth_token`). `feathers3_fe` protects NVS with
  flash encryption; plain `feathers3` does not. This differs from the older
  PBKDF2 proposal and should receive an explicit security decision before
  production use.
- HTTP recovery uses Basic or Bearer authentication over plain HTTP inside the
  WPA2 SoftAP. Use a unique high-entropy passphrase. Anyone who learns it can
  trigger recovery operations, although they still cannot install an image that
  fails the signature policy.
- LittleFS and NVS are never automatically formatted/erased on init failure.
  That preserves data but can require cable/serial intervention.
- Direct upload cannot repair a broken filesystem. It can install a main image;
  if the filesystem remains unreadable, that image will return to recovery.
- The staged path needs enough LittleFS space for the full image plus metadata.
  The direct path is the fallback when it does not.
- The checked-in ESP-SR OTA layout is not operationally complete: the migration
  target does not provision its relocated model partition. Current testing and
  rollout must keep `ENABLE_ESP_SR=0`.
- The image version is currently assigned at build time in CMake and includes a
  board/layout suffix. Release operators must bump the semantic version before
  producing a new release pair.
- `dataSchema=1` is an acceptance gate, not an implemented general file-format
  migration framework. Every image carrying schema 1 must keep its NVS and
  LittleFS writes readable by older schema-1 recovery images. Do not perform an
  irreversible data migration while an image is still in probation; defer it
  until after validation or use a separately designed two-phase schema rollout.
  The current OTA code does not enforce that application-level discipline.
- The updater limits image receive/apply to 10 minutes and ordinary idle
  recovery to 15 minutes. Failed/unsafe active state deliberately holds rather
  than silently returning to an invalid main.
- The updater configures the task watchdog from
  `HW1_UPDATER_WDT_TIMEOUT_MS`, subscribes and feeds its control task, uses
  sequential OTA writes to bound flash-erase stalls, and refuses writes if that
  subscription is unavailable. The migration-only 9 MiB format temporarily
  unsubscribes only the control task and fails closed if it cannot resubscribe;
  idle-task watchdogs remain enabled. The direct HTTP handler is not separately
  subscribed and relies on its absolute receive/apply deadline plus platform
  idle-task coverage. Forced-stall behavior is still a hardware qualification
  item; do not claim universal deadlock recovery from build evidence alone.
- The journal retains one terminal result, not an append-only history. Ordinary
  operations require explicit sequence-bound acknowledgement, while marked
  emergency recovery can supersede the pending result when the emergency itself
  terminates.
- No firmware path broadens authorization to automations: mutating main OTA
  commands reject automation context and require superadmin; recovery uses its
  separate credential.

## Current qualification gaps

Before this can be called functional for production, the following need a
recorded resolution:

1. Complete the remaining plain-board matrix, then the FE parity rows, on the
   two dedicated test boards. Migration and an unexpected-reset rollback proxy
   have hardware evidence; literal power interruption and every
   flash-encryption claim still require hardware tests.
2. Decide whether plaintext recovery-credential storage on the plain board is
   acceptable or replace it with an explicit verifier/token design.
3. Add deterministic fault injection for the short post-write/boot-selection
   windows, or record those windows as untested residual risk.
4. Hardware-test the backup/restore helper against the actual web server,
   including HTTP-200 read errors, a changing file, malformed/duplicate roster,
   wrong bootstrap username, `users.json` last ordering, and an interrupted
   restore. Host unit tests cover these contracts but cannot prove device-side
   behavior or transactional consistency.
5. Add a dedicated, guarded factory-updater cable target if rerunning the full
   migration target solely to remove migration-only `formatfs` is considered
   too broad.
6. Decide and document artifact archival/key-rotation policy. Source-only
   releases mean an operator must retain reproducible board-specific signed
   binaries for recovery and intentional downgrade.
7. Define, implement, and test model-partition migration before claiming the
   checked-in ESP-SR OTA layout is supported.
8. Run forced-stall tests for the subscribed updater control task, staged apply,
   and direct HTTP handler. Decide whether the direct handler also needs a
   task-specific watchdog beyond its deadline and the idle-task watchdogs.
9. Decide whether the explicit one-slot emergency-supersession policy plus
   external operator logs is sufficient for fleet audit, or implement durable
   multi-result history.

Until those items and the matrix are closed, treat the implementation as a
substantial, fail-closed prototype with a credible recovery architecture, not
as a fully qualified fleet updater.
