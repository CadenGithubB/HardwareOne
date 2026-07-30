# Local patches to the vendored arduino component

`components/arduino/` is excluded by the main repo's `.gitignore` and is its own
nested git checkout (arduino-esp32 core 3.3.5, nested HEAD `11bc7ac1a`). Every
local fix to it exists **only as uncommitted working-tree changes** in that
nested repo. A `git clean -dfx`, a fresh clone, or a re-vendor of the component
silently reverts all of them — while the firmware code that depends on them
survives and keeps compiling. The failure mode is invisible until hardware
misbehaves.

## What the patches are

All marked with `[hardwareone local patch YYYY-MM-DD]` comments:

| File | Markers | What |
|---|---|---|
| `libraries/BLE/src/BLEClient.cpp` | 11 | Raise-only local-MTU guard in `setMTU` (the per-link MTU fix — without it, ring-first glasses discovery fails with `pkt size: 102, PDU size: 64`); `m_conn_id`/`m_mtu` reset on disconnect and failed open; REG_EVT app_id filter (stops a parked client capturing another client's registration); bounded REG/OPEN/SEARCH waits; semaphore-leak fixes; stale-rc-on-timeout fix |
| `libraries/BLE/src/BLEDevice.cpp` | 1 | Sync `m_localMTU` cache to Bluedroid's real boot value (517) at init, so the raise-only guards compare against the truth |
| `libraries/BLE/src/BLERemoteCharacteristic.cpp` | 5 | Semaphore-leak + bounded-wait + failure-delivery fixes |
| `libraries/BLE/src/BLERemoteDescriptor.cpp` | 4 | Same family |
| `libraries/Network/src/NetworkEvents.cpp` | 0 (unmarked) | arduino_events task stack fix (see wifirm/arduino_events memory) |
| `CMakeLists.txt`, `Kconfig.projbuild`, `idf_component.yml` | — | Build-integration tweaks |

`arduino-local-patches.patch` is the full `git -C components/arduino diff`
snapshot of all of the above.

**Not covered:** the untracked `components/arduino/variants/XIAO_ESP32S3_SENSE/`
directory (board variant) — `git diff` cannot capture untracked files.

## Verify (run any time, especially after anything touched components/arduino)

```bash
bash docs/arduino-local-patches/verify_patches.sh
```

## Re-apply after a revert

```bash
git -C components/arduino apply --check ../../docs/arduino-local-patches/arduino-local-patches.patch
git -C components/arduino apply ../../docs/arduino-local-patches/arduino-local-patches.patch
```

(The `../../` matters: `git -C` resolves relative paths against
`components/arduino`, not your shell's cwd.)

(`--check` first; if it fails, the component was re-vendored to a different
base — re-apply hunks by hand using the marker comments as the guide.)

## Refresh after adding a new patch

```bash
git -C components/arduino diff > docs/arduino-local-patches/arduino-local-patches.patch
```

and bump the marker counts in `verify_patches.sh`.

## Honest limitation

This `docs/` directory is itself untracked (project convention), so it also
does not survive `git clean -dfx`. For real durability, either commit the
patches inside the nested `components/arduino` repo, commit this directory in
the main repo, or keep a copy outside the tree. That decision is deliberately
left to the repo owner.
