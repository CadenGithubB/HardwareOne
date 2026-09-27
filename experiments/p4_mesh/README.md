# HardwareOne P4/C6 ↔ S3 application mesh experiment

Application milestone following the successful transport probe in `../p4_espnow/`.
This milestone builds the actual HardwareOne application with one minimal
feature profile, then tests its discovery, secure pairing, sessions, encrypted
messages and fragmented transfers on the two connected boards.

The application is built in an ignored copy under `private/app/`, made from the
current tracked working-tree content. The production checkout and its existing
user edits remain unchanged. Copy-only integration changes are preserved
as reviewable patches; the shared protocol is not replaced by a simulation.

The S3 uses its native radio. The P4 uses the same C6 firmware and ESP-NOW bridge
that passed milestone one. All builds use ESP-IDF 5.5.5. Optional web, Bluetooth,
camera, display, audio, sensor and automation features are disabled for this test.

Test-only local administrator and mesh credentials are generated into ignored
`private/credentials.json`, with file mode 0600. They are never Wi-Fi credentials.
Serial tooling redacts these values before writing logs. Full original flash
backups for all three chips remain in `../p4_espnow/private/backups/`; preserve
them when cleaning build directories.

See [BUILD.md](BUILD.md) for the pinned build and cumulative copy-only patches,
and [PORTING.md](PORTING.md) for the CPU-independent design and remaining work.
See [the hardware results](results/2026-09-27/RESULTS.md) for passed checks and
the remaining pairing and file-delivery failures. A successful build alone does
not qualify every board feature.

## Device setup and tests

Use the original backup/restore instructions in `../p4_espnow/README.md`. Keep
the C6's tested bridge firmware. Consult each application build's `flash_args`:
P4 and S3 have different bootloader offsets. Flashing the application does not
erase the filesystem. HardwareOne refuses to format an existing, unrecognized
filesystem automatically; inspect its partition and preserve its original data
before initializing a new test filesystem.

On a fresh application filesystem, choose **Basic Setup**, then **Meshed Node**,
and supply a test administrator username/password. Wait for `[Boot] Setup
complete`; Basic Setup does not necessarily reboot or print an initial shell
prompt. It skips Wi-Fi credentials. Store the chosen credentials and a new shared
mesh passphrase in ignored `private/credentials.json` with permissions `0600`:

```json
{
  "username": "YOUR_LOCAL_TEST_USERNAME",
  "password": "YOUR_LOCAL_TEST_PASSWORD",
  "mesh_passphrase": "YOUR_SHARED_TEST_PASSPHRASE",
  "mesh_label": "primary"
}
```

Use Python with `pyserial` installed. Close other serial clients before running:

```bash
# Provisioned boards with no peer relationship:
python experiments/p4_mesh/test_mesh.py --configure --pair discovery --initiator s3 --rekey --reopen

# Already configured and paired boards:
python experiments/p4_mesh/test_mesh.py --rekey --reopen

# Recovery checks independent of a file-transfer failure:
python experiments/p4_mesh/probe_recovery.py

# Saved identities/configuration/peers across proven USB reopen reboots:
python experiments/p4_mesh/test_persistence.py

# Offline checks (no hardware access):
python -m unittest discover -s experiments/p4_mesh -p 'test_*.py'
```

The default ports are P4 `/dev/cu.usbmodem2101` and S3 `/dev/cu.usbmodem1101`;
override them with `--p4-port` / `--s3-port`. Port names may change. The runner
checks each board's radio MAC and channel. On these boards, opening the serial
connection was observed to reset the chip; the runner allows startup time.

The runner never silently unpairs a device or repairs a failed acceptance. It
requires reciprocal peer registries, matching authenticated sessions and actual
encrypted delivery. It reconstructs long TEXT from the application's individual
history fragments, and reads back both binary files to compare every byte.
Rekey and radio close/open are explicit optional phases. It leaves unique test
files on the devices and writes redacted logs plus sanitized results into a new
ignored `private/test-runs/` directory. Passwords are read from the file, never
passed on the command line.

Discovery pairing is not yet reliable. The S3-initiated direction succeeded in
one focused trace, but a later run lost its request before the P4 pairing UI.
The runner reports failure and does not silently repair it. `--pair secure`
explicitly establishes both peer records to investigate subsequent protocol
phases; passing that mode does not qualify the request/accept workflow.

HardwareOne command completion uses a queued, read-only `whoami` barrier. USB
prompts can arrive late or interleave with asynchronous logs, so a prompt alone
is insufficient evidence that the preceding command finished.
