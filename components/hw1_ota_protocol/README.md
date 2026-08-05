# HardwareOne OTA protocol component

This component is the shared native C contract between the HardwareOne main
application and the recovery updater. It contains no Arduino dependency, no
signing key, no partition-selection side effects, and no NVS initialization or
erase policy.

## Manifest v1

Release tooling serializes a canonical 224-byte payload with these fields:
`formatVersion`, `boardId`, `layoutId`, `projectName`, `version`, `imageSize`,
`imageSha256`, `minUpdaterVersion`, and `dataSchema`. The payload has its own
version, size and CRC framing. The detached signature is exactly 384 bytes and
uses RSA-3072-PSS with SHA-256, MGF1-SHA-256, and a 32-byte salt.

`hw1_ota_manifest_parse_untrusted()` is for diagnostics only. Installation must
go through `hw1_ota_manifest_verify()` and then
`hw1_ota_manifest_validate()`. The first function fails if either the detached
signature or a verifier callback is absent. `hw1_ota_idf.h` supplies
`hw1_ota_idf_rsa3072_pss_sha256_verify()` for caller-owned PEM public-key bytes.

The candidate copied into the transaction record is durable comparison
metadata, not a durable authentication token. Its CRC detects torn or corrupt
writes but cannot authenticate an NVS writer. After every reboot, consumers
must freshly verify the signed envelope and call
`hw1_ota_record_candidate_matches_verified()` before trusting the persisted
candidate or installing its image.

The v1 fleet IDs are:

| Build | boardId | layoutId | version suffix |
|---|---|---|---|
| Plain FeatherS3 | `feathers3` | `hw1-f3-ota-v1` | `+f3o1` |
| Flash-encrypted FeatherS3 | `feathers3_fe` | `hw1-f3fe-ota-v1` | `+f3feo1` |

Consumers should compile `HW1_OTA_BOARD_ID` and `HW1_OTA_LAYOUT_ID` and may use
`HW1_OTA_TARGET_POLICY_BUILD_INIT(...)` to bind them into validation policy.

## Persistent transaction sequence

`hw1_ota_nvs_commit()` alternates two CRC-protected blobs (`hw1up/tx_a` and
`hw1up/tx_b`), commits the inactive slot, reads it back, and preserves the
previous valid slot. It takes the sequence returned by the last load as an
optimistic concurrency check. Both-corrupt recovery requires an explicit
`HW1_OTA_SEQUENCE_ANY` commit; corruption is never silently reset.

The intended durable ordering is:

1. `hw1_ota_begin()` and commit the request.
2. Transition to `RECOVERY_BOOT_ARMED` and commit **before** changing the boot
   target or rebooting.
3. In recovery, commit `RECOVERY_RUNNING`, then commit `APPLYING` **before**
   erasing or writing the main slot.
4. Authenticate and validate the manifest, verify the ESP image, then commit
   `IMAGE_VERIFIED`.
5. Commit `TRIAL_BOOT_ARMED` **before** selecting the main slot and rebooting.
6. Main commits `TRIAL_RUNNING`; after the health policy and IDF mark-valid
   operation succeed, it commits `SUCCEEDED`.

`hw1_ota_reconcile()` compares a durable phase with observed app/boot/image
state. When its decision contains both `COMMIT_EVENT` and external actions, the
suggested event must be transitioned and durably committed first. Terminal
results are not erased at boot. Ordinary new operations are rejected while a
result is unacknowledged. Automatic crash-loop/storage recovery and a manually
forced factory-recovery upload may set the explicit emergency-supersession flag;
because v1 retains one result slot, the old result then remains visible until
the emergency operation itself reaches a terminal result.
