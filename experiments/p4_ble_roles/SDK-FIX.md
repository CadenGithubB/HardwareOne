# ESP-IDF 5.5.5 Bluetooth ECC backport

This experiment retains **ESP-IDF 5.5.5** and uses the same clean Bluetooth
component override for P4/C6 and native S3. It does not require ESP-IDF 6 or a
separate application protocol implementation for P4.

P4 Secure Connections failures matched
[Espressif issue 19002](https://github.com/espressif/esp-idf/issues/19002).
The backport preserves the five TinyCrypt file diffs from official commit
[`9fd7cb7e606a06111e1b14be7f4e00d77d9cf3dd`](https://github.com/espressif/esp-idf/commit/9fd7cb7e606a06111e1b14be7f4e00d77d9cf3dd).
It corrects the accelerator byte-order handling and uses the regularized
software path for ECDH/signing. The unrelated NimBLE submodule pointer update
is omitted, following the maintainer's guidance. These builds use Bluedroid.

The shared SDK checkout remains untouched. The local application R1 CCCD
encryption request is a separate change; it is not part of this SDK patch.

## Recorded inputs and output

| Item | Recorded value |
| --- | --- |
| SDK release | IDF 5.5.5, commit `b774170ff46c393eeb5e495ea37936038d3f4f4f` |
| Patch | `idf-tinycrypt-9fd7cb7.patch` |
| Patch SHA-256 | `e8185397ceb5e74653c08c7cb22666f54445b27b0a578488c5e35bbca7eac367` |
| Manifest | `idf-bt-manifest.json` |
| Generated component | `private/idf-components/bt` |
| Changed files | `esp_tinycrypt_port.c`, `esp_tinycrypt_port.h`, `ecc.c`, `ecc_dh.c`, `ecc_dsa.c` under `common/tinycrypt` |

The manifest hashes all **2,046 component files**, including initialized
submodule contents, plus `tools/cmake/version.cmake`. It records baseline
submodule revisions and the five patched output hashes and official Git blob
identities. Other SDK components are outside this content-hash coverage.

Preparation materializes the baseline's one internal NimBLE header symlink as
a regular file and omits Git metadata. No generated file is a symlink back to
the SDK or diagnostic copy. The five patched files were independently verified
against their official Git blob identities; all other component files match
the pinned baseline. SMP diagnostic instrumentation is absent.

## Prepare and verify

From the repository root, after activating the pinned IDF environment:

```sh
python3 -B experiments/p4_ble_roles/prepare_idf_bt.py --idf "$IDF_PATH"
python3 -B experiments/p4_ble_roles/prepare_idf_bt.py --check
python3 -B experiments/p4_ble_roles/tests/test_prepare_idf_bt.py -v
```

`--idf` defaults to `IDF_PATH`. The helper first verifies the complete baseline,
copies it into a staging directory, applies the patch with zero fuzz and no
offsets, then checks the complete output before publishing it. A repeat run
checks an existing output without rewriting it. Unknown files, content changes,
executable-mode changes, unrecorded symlinks or altered patch bytes fail the
check; the helper never repairs or overwrites an unknown directory.

Both build wrappers call this helper and select its output through
`EXTRA_COMPONENT_DIRS`. Their normal commands are unchanged:

```sh
bash experiments/p4_ble_roles/build-s3.sh
bash experiments/p4_ble_roles/build-p4.sh
```

These commands build only. An existing build cache that used a diagnostic
component must be reconfigured to the clean component; verify the selected
component path in build metadata. A component's existence or a successful host
test is not evidence that a device is running its firmware.

## Checks and qualification limits

All **14 preparation safety checks** passed, including clean reconstruction,
unchanged idempotent output, baseline and output tampering, unexpected files,
symlink rejection, SDK version, executable modes, patch offsets and cleanup
after failure. A separate fresh reconstruction also matched the official
patched-file identities.

Independent host review ran actual baseline and patched TinyCrypt code against
the SDK's RFC 5903 P-256 vector. Expected public keys and both ECDH directions
passed in the software builds, along with point validation checks. A hardware
failure stub confirmed that the patched ECDH route no longer calls the hardware
adapter. This does not emulate or exhaustively verify the real accelerator.

The review also found an upstream error-path limitation: when hardware
public-key multiplication fails, the software fallback can report success with
an incorrect point for scalar 2. This was reproduced with fault injection, not
observed as a normal board-path failure. It is not evidence of an authentication
bypass, and the patched ECDH path avoids that fallback. The exact official
patch is retained; accelerator errors would require further upstream work.
Neither the host vectors nor the accessory tests establish exhaustive
cryptographic correctness.

The P4 diagnostic build containing this fix connected both G2 temples and R1,
displayed text confirmed by the user, and passed a short encrypted mesh check.
See [the diagnostic accessory evidence](results/2026-09-27/ACCESSORIES.md).
The later **clean images passed the scoped qualification on both targets**,
including three accessory links, separate encrypted Server sessions and role
transitions. [FINAL.md](results/2026-09-27/FINAL.md) records their exact hashes,
coexistence checks and remaining limits. Those final results are distinct from
the diagnostic image's earlier success.

## Next stable 5.x release

As checked on **2026-09-27**, 5.5.5 remained the latest released 5.5 patch
version; 5.5.6 was not available. Espressif's published roadmap listed
**2026-10-08** for 5.5.6, a planned date subject to change.
See the [release list](https://github.com/espressif/esp-idf/releases) and
[official roadmap](https://github.com/espressif/esp-idf/blob/master/ROADMAP.md).
Once released, verify that it contains this fix and qualify both boards before
replacing the backport. No upgrade or automatic branch switch is part of the
current experiment.
