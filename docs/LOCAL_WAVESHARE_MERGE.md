# Local fixes and Waveshare integration — 2026-11-01

## Source history and recovery

- Starting checkout: `aa6508fd` (`v0.99.99`), on `codex/android-ble-relay`.
- Local checkpoint: `64713c0d`, preserving all 137 modified tracked files and
  46 non-ignored untracked files before integration. Ignored local files were
  left in place.
- Incoming main: `138c59fb`, including the Waveshare P4 work and follow-up fixes.
- Integration branch: `codex/local-fixes-waveshare`.
- No reset, rebase, force-push or hardware flashing was used.

The local checkpoint is the first parent of the merge; incoming main is its
second parent. The v0.99.99 release tag is unchanged.

## Conflict resolution and preservation

Thirteen files changed on both sides. Git needed manual resolution in two:

- `HAL_Input.h`: retain local `input_*` task names and add the GT911 branch as
  `input_touch`, using the same macro for task creation and reporting.
- `HAL_Radio.cpp`: call Waveshare's generalized `prepareBoard()` while retaining
  the local `c6_console` task name and 4096-byte console stack.

Reviewed the automatic merges in initialization, OLED utilities, feature
registration, I2C, power, companion update, task reporting, VFS and host tests.
Before regenerating documentation, all 170 local-only changed files matched
the checkpoint byte-for-byte and all 64 remote-only changed files matched
incoming main byte-for-byte. The generated command reference was then refreshed
to include the new speaker and powersleep commands and the local setting changes;
its retired-feature consistency check passes.

## Host verification

The host build passed with the default ASan/UBSan instrumentation. The full
CTest run registered 333 tests: 322 passed, seven failed and four browser tests
were disabled because Node.js is unavailable. Regenerating the command reference
fixed `retired_feature_text`; its targeted rerun and the touch/input/wake/update
checks passed. Final effective result: 323 passed, six existing failures and four
disabled tests. No full second run was needed for the generated-document fix.

All five Waveshare suites passed: `touch_input`, `touch_controls`, `battery`,
`radio_legacy_update` and `waveshare_wake`. Shared CPU-frequency and idle/wake
power-policy suites passed, as did optional peripheral, display lifecycle and
SDMMC coverage.

Remaining failures, kept separate from this merge:

- `oled_mode_availability`: Bluetooth AVAILABLE versus FEATURE_DISABLED;
  reproduced by compiling against the pre-merge checkpoint's OLED source.
- `phone_sdk_manifest`: the ignored vendored Arduino `BLEClient.cpp` differs
  from its reviewed checksum. The checker/manifest are unchanged by this merge;
  integration did not edit the vendored BLE source.
- `tof_poll_outcomes`: its harness omits sensor-health symbols. The generated
  translation unit is byte-identical using the pre-merge checkpoint inputs.
- `tool_pins`: BLE catalog consumers still expect 165 kinds / 3149 bytes while
  the local fixture contains 174 / 3307. All affected inputs match the checkpoint.
- `wifi_connect_budget`: its harness has an unused `SYSEVT_WIFI_CONNECTED`
  constant rejected by `-Werror`. The generated translation unit is byte-identical using the checkpoint inputs.
- `oled_layout`: its extracted profiler lacks `TASK_CPU_SAMPLE_MS`; the profiler
  source is unchanged from the local checkpoint.

## Firmware verification and workspace ownership

Both saved profiles passed choice validation, full compilation, linking, image
creation and partition-size checks:

| Profile | Application bytes | App partition bytes | Free bytes |
| --- | ---: | ---: | ---: |
| `waveshare-p4-bringup` | 2,502,592 | 8,388,608 | 5,886,016 |
| `p4-phone-pipe` (P4X-EYE baseline) | 6,125,808 | 7,426,048 | 1,300,240 |

No hardware was flashed; these results do not extend physical qualification.
The original v0.99.99 release identity was not changed.

Host tests used `/tmp/hw1-waveshare-merge-host` to avoid reusing another task's
host build. Waveshare used its normal new saved-profile build directory,
`build-config-waveshare-p4-bringup`. Both task-created build directories were
removed after verification, recovering 959,451,136 allocated bytes (about
915 MiB). Unrelated ignored files and other worktrees were left in place.

Existing `build-config-p4-phone-pipe` was reused for the baseline; its approximately
1.09 GiB build cache and new matching firmware/debug outputs remain in the
normal saved-profile location for future builds. No extra baseline build copy
was created.

Local evidence is retained under `.scratch/local-waveshare-merge` (approximately
46 MiB total):

- `premerge-p4-phone-pipe-firmware.tar.gz`: approximately 30 MiB of the previous
  firmware, matching ELF/map, build manifest and saved inputs, preserved before
  reusing the baseline. Retain until that earlier firmware is no longer needed.
- `waveshare-merged-firmware.tar.gz`: approximately 16 MiB of the successfully
  built Waveshare firmware, bootloader/partition binaries, matching ELF/map and
  saved inputs. Archive contents were checked byte-for-byte before build cleanup.
  Retain for debugging or remove once this integration's firmware is superseded.
- Build/test logs, build manifests, dependency lock, preservation/cleanup records
  and archive checksums. No dependency trees or compiler-object copies were kept.

Local `main` is advanced to the completed merge without pushing. The integration
branch and `codex/local-fixes-v0.99.99` recovery branch are retained. The latter
points directly at `64713c0d`, before any Waveshare changes.

## Subsequent physical verification — 2026-11-01

After the merge, the Waveshare P4 was fully erased and flashed with the verified
image; bootloader, application and partition-table hashes passed. The user
confirmed it works. Its blank LittleFS partition formatted successfully; a
one-time task-watchdog warning during that first initialization is retained in
the local boot log.

The connected P4X-EYE (MAC e8:f6:0a:e3:a7:73, revision 3.2) had a partition table
matching the verified baseline byte-for-byte. Only the 6,125,808-byte application
at 0x10000 was flashed, with hash verification; setup/data partitions were
preserved. Boot mounted LittleFS/SDMMC and brought up the display and C6, with
crashCount=0. The user confirmed it works as expected. Push was then held while
implementing the newly requested Waveshare landscape setting; that feature's
physical qualification remains separate from these successful merge tests.
