# HardwareOne OTA host tools

These tools build, audit, migrate, back up, and qualify the native ESP-IDF
recovery OTA system. Run them from the repository root.

## Tool index

| Tool | Purpose | Device mutation |
|---|---|---|
| `check_ota_builds.py` | Audit an exact main/updater pair | None |
| `make_manifest.py` | Create or verify a signed OTA manifest | None |
| `make_bundle.py` | Package a verified pair for the offline Android companion | None |
| `make_test_fixtures.py` | Generate deterministic lab-only negative fixtures | None |
| `hardware_qualification.py` | List cases, preflight artifacts/interfaces, and initialize/resume dry-run evidence | None in the current implementation |
| `device_backup.py` | Back up or restore user files for layout migration | Backup is read-only; restore writes files |
| `confirm_migration.py` | Guard first-layout cable migration | Confirmation helper only |

The current qualification CLI intentionally has no destructive scenario
executor. It cannot upload OTA files, run OTA commands, acknowledge results,
reboot, or flash a device. Its output must be reviewed before that executor is
implemented.

## Create an offline companion-app bundle

The Android companion never browses or downloads releases. Publish a
deterministic `.hw1ota` file alongside the ordinary image and manifest; the
user downloads it separately and selects it with Android's system file picker.

```sh
python3 tools/ota/make_bundle.py create \
  --image build/hardwareone-idf.bin \
  --manifest path/to/hardwareone-idf.manifest.json \
  --public-key build/hw1_ota_public_key.pem \
  --output /path/outside-the-repository/HardwareOne-feathers3.hw1ota
```

The tool verifies the detached RSA-PSS signature, ESP app descriptor, image
size, and SHA-256 identity before writing the bundle. The bundled public key is
only an offline fail-fast aid; the device independently verifies against its
embedded key before flash selection. Inspect a downloaded bundle without
mutating anything:

```sh
python3 tools/ota/make_bundle.py inspect HardwareOne-feathers3.hw1ota
```

The companion transfers both bundle members directly over the established
HardwareOne Secure Channel. `otawrite begin` binds the member name, exact byte
length, and SHA-256; encrypted binary frames carry an explicit big-endian
offset; and periodic `otawrite status` replies report the exact accepted
offset. `finish` promotes no data unless the streamed length and SHA-256 match.
The candidate is completed before the manifest begins, and only the later
`otastage confirm` promotes the pair into a durable OTA transaction. A dropped
or out-of-order frame therefore causes an offset resynchronization or a loud
failure instead of a guessed append. Generic `filewrite` intentionally has no
permission to `/system/ota`.

This transport is available only to a live superadmin session over encrypted
BLE. The Android manifest still declares no Internet permission; release files
must arrive through the system document picker. Factory recovery itself remains
SoftAP/HTTP-only, so the phone does not need recovery Wi-Fi for a normal staged
update but can still use it as the rescue path.

## Host tests

Run the complete OTA tooling suite with:

```sh
python3 -m unittest discover -s tools/ota/tests -v
```

The suite uses a temporary RSA-3072 key and synthetic ESP application descriptor
for fixture tests. It does not connect to hardware.

## Inspect the staged qualification contracts

List STG-001 through STG-018:

```sh
python3 tools/ota/hardware_qualification.py cases
```

Render a complete machine-readable case plan:

```sh
python3 tools/ota/hardware_qualification.py plan --case STG-012 --json
```

Each step states whether it mutates the device and what durable result must be
observed. A final HTTP response alone is never sufficient evidence.

## Artifact and interface preflight

Run under the ESP-IDF Python environment when including the paired-build audit,
because `check_ota_builds.py` invokes ESP-IDF's `espsecure` module:

```sh
python3 tools/ota/hardware_qualification.py preflight \
  --board feathers3 \
  --image build/hardwareone-idf.bin \
  --manifest path/to/hardwareone-idf.manifest.json \
  --public-key build/hw1_ota_public_key.pem \
  --main-build build \
  --updater-build path/to/updater-build \
  --serial-port /dev/cu.usbmodem-example \
  --adb-serial ANDROID_SERIAL \
  --json
```

The command is read-only. Omitting the paired build directories, serial port,
or ADB serial records that check as `SKIP`. A skipped paired audit means
`readyForDestructiveRun` remains false even if the manifest/image pair verifies.

Serial enumeration needs `pyserial`. The ESP-IDF Python environment normally
provides it. The tool does not install dependencies automatically.

The Android recovery transport sends authentication inside raw HTTP on stdin to
the selected `adb` process; it does not put the recovery credential in process
arguments. No current CLI command prompts for or uses that credential because
the destructive executor has not been implemented.

## Initialize and resume a dry run

Create a private evidence bundle under the system temporary directory by
default:

```sh
python3 tools/ota/hardware_qualification.py init-run \
  --case STG-012 \
  --board feathers3 \
  --image build/hardwareone-idf.bin \
  --manifest path/to/hardwareone-idf.manifest.json \
  --public-key build/hw1_ota_public_key.pem \
  --main-build build \
  --updater-build path/to/updater-build \
  --disposable-device \
  --json
```

`init-run` writes:

```text
environment.json
artifacts.json
preflight.json
summary.json
checkpoint.json
events.jsonl
cases/STG-NNN.json
```

It records `dryRunOnly=true` and `destructiveExecutorPresent=false`.

Validate and display a saved checkpoint:

```sh
python3 tools/ota/hardware_qualification.py resume \
  --run /path/to/run-directory \
  --json
```

Future execution must rediscover serial identity, the running component,
authenticated device state, candidate identity, and journal/result sequences
before continuing from a checkpoint.

## Generate negative fixtures

Use a dedicated non-production RSA-3072 lab key. The output directory must not
already exist:

```sh
openssl genpkey -algorithm RSA \
  -pkeyopt rsa_keygen_bits:3072 \
  -out /protected/path/hw1-ota-lab-key.pem
```

```sh
python3 tools/ota/make_test_fixtures.py \
  --board feathers3 \
  --image build/hardwareone-idf.bin \
  --lab-key /protected/path/hw1-ota-lab-key.pem \
  --output /path/outside-the-repository/hw1-ota-fixtures \
  --acknowledge-lab-key
```

The generated index includes 16 fixture pairings covering missing parts,
truncation, same-size digest mismatch, detached-signature failure, a detached
manifest-valid/native-app-invalid image, wrong identity/compatibility fields,
malformed JSON, and the 2049-byte manifest limit. It records the public-key
fingerprint but never the private key or its path.

Generated images, manifests, evidence, credentials, private keys, and device
captures must remain outside Git and `docs2/`.

## Security and evidence rules

- Select serial and ADB devices explicitly; never use “first attached device.”
- Do not place credentials in command-line arguments or evidence fields.
- Evidence recursively redacts password, authorization, cookie, session,
  private-key, signing-key, and caller-supplied secret values.
- `PASS` requires the case's durable assertions. A lost reboot response is not a
  pass.
- Do not acknowledge a pending device result during setup or cleanup unless its
  exact sequence and contents were reviewed.
- Do not run destructive tests until the current tool has gained a reviewed
  executor and the operator guide says that executor is qualified.
