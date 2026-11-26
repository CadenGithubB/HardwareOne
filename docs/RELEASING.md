# Releasing

How to cut a versioned release of this firmware. Every release gets a commit, a tag
and a `CHANGELOG.md` entry. A GitHub Release is OPTIONAL and occasional - cut one when
a version is worth announcing, using the same notes as its CHANGELOG section. The
CHANGELOG is the complete record; the Releases page is a highlights reel.

This repo is trunk-based: the release commit + tag land directly on `main`.
Commit style is plain-English and version-prefixed, like a changelog line:
`vX.Y.Z: what the change gives you` (e.g. `v0.97.0: unified sensor reading format -
one shared shape for every sensor`), optionally with a `- ` bullet body. Pure-docs
commits use `docs: <plain summary>`. Describe the user-facing outcome, not the
mechanism; do NOT use Conventional Commit type()/scope prefixes.

## Version is declared in (keep all four in sync)
- `CMakeLists.txt` -> `set(PROJECT_VER "X.Y.Z")` - the runtime source of truth; flows via
  `esp_app_get_description()->version` to the boot banner, `status` CLI, OLED, settings JSON,
  `/api/ping`, and backup metadata.
- `README.md` -> `# Hardware One vX.Y.Z`
- `docs/USERGUIDE.md` -> title line
- `docs/QUICKSTART.md` -> title line

## Recovery updater version
The factory recovery updater has its own version, `HW1_UPDATER_SEMVER` in `tools/updater/CMakeLists.txt`
(plain X.Y.Z; each board appends its `+suffix`). Devices report it as `factoryUpdaterVersion` in
`otastatus json`. Bump it in the same commit as any change under `tools/updater/`, or to the
`hw1_ota_protocol` code the updater links, that changes recovery behaviour. If a release needs an
updater change to be safe, build it with `HW1_MIN_UPDATER_VERSION=<that version>
tools/build_deployment.sh ...`: the signed manifest then carries that `minUpdaterVersion`, and
devices with an older factory updater refuse the update until the updater is reflashed. Never set it
above the `HW1_UPDATER_SEMVER` shipped in the same release; the paired-artifact audit
(`check_ota_builds.py --min-updater`) fails the build if it is. Unset keeps the `1.0.0` floor that
every updater meets.

## SemVer (pre-1.0)
PATCH = fixes/docs. MINOR = backward-compatible features. Breaking changes bump MINOR (pre-1.0).
An optional fourth number marks a point release on top of X.Y.Z (e.g. `0.99.94.1`); the OTA
protocol, recovery updater and `tools/ota/semver.py` accept it and order it between X.Y.Z and the next patch.

## Gate: before any tag

Every step below must pass on the exact commit you intend to tag, from a clean tree
(`git status --porcelain` empty). They are the checks for a 1.0 tag.

1. Arduino component vendored and patched: `tools/vendor_arduino.sh --check` prints OK
   (a fresh clone: `tools/vendor_arduino.sh`, then ESP32-P4 rows also `tools/p4/prepare_sdk.sh`).
2. Host suite green, serially and in parallel, with Node.js installed so the browser suites run.
   It runs on the development Mac, where it is maintained; the repository has no GitHub Actions
   workflow on purpose, so a push to GitHub runs nothing:
   `cmake -S components/hardwareone/test/host -B /tmp/hw1-host && cmake --build /tmp/hw1-host -j6 &&
   ctest --test-dir /tmp/hw1-host && ctest --test-dir /tmp/hw1-host -j6` — 0 failures; "Not Run" only for
   the documented optional suites (`stt_conv_scratch`, the camera managed-SCCB/video suites). The
   OTA protocol suite is part of the same tree. It includes `command_registry_gate`, which runs
   `python3 tools/command_registry.py audit` and `python3 tools/command_registry.py reference --check`;
   both must exit 0 (if the reference drifted, regenerate it with `... reference` and commit it).
   On a machine with the ESP32-P4 SDK prepared, add `-DHW1_ESP_HOSTED_TEST_IDF=$IDF_PATH` to the
   first `cmake` so the ESP-Hosted memory-pool regression (`esp_hosted_mempool`) runs too.
   The OTA tools, the web UI page harnesses and the model tools run in the same ctest
   (`tools_ota_unittests`, `tools_webui_unittests`, `tools_model_unittests`). The Mac
   test phone (`tools/phone_link_peer`, needs Python 3.10 or newer) and `tools/ble_secure` (needs
   PyNaCl) are bench tools, not part of the gate.
3. Every stable row of the supported matrix builds from that commit with `tools/build_board.sh <board>`
   (or `tools/build_deployment.sh` for a deployment row) and its BUILD_INFO.md says "clean tree".
4. Tracked tree hygiene: the host suite's `tree_hygiene_gate` passes (alone:
   `python3 components/hardwareone/test/host/test_tree_hygiene_gate.py`). It fails on an account home path
   (`/Users/<name>`) or a `192.168.x` address outside the ranges the tree uses on purpose (generic examples,
   the SoftAP defaults, test fixtures; the list and reasons are in the test). It cannot check for a Wi-Fi
   network name or a password: read the diff since the last tag for those.
5. CHANGELOG `[Unreleased]` lists every user-visible change since the last tag; COMPATIBILITY.md and the
   support matrix in `docs/README_DETAILS.md` match what you built.

## The 1.0 cut: HardwareOne's own versions go to 1

Decided 2026-11-08 and done only in the 1.0.0 release commit, never before. 1.0.0 is a full-erase
install and every board and the phone app are rebuilt together, so no older peer has to be served.
In that one commit:

1. Firmware `1.0.0` in the four files above.
2. Recovery updater: `HW1_UPDATER_SEMVER` in `tools/updater/CMakeLists.txt` goes to `1.0.0`. The default
   `minUpdaterVersion` floor stays `1.0.0`. Every board is re-flashed through the full erase, so no
   device keeps a `1.1.0` updater.
3. Phone link is already inner version 1 (`hw1pipe::kVersion`). The release commit does not
   change it. The Android app is updated to the same byte on its own.
4. ESP-NOW wire byte is already 1 (`ESPNOW_V4_VERSION`). The release commit does not change it.
   `versions` shows custom ESP-NOW v1, and `versions json` shows wire byte 1. A later field is
   appended or is a new message type; the byte stays 1.
5. `versions` on a built 1.0.0 image shows firmware 1.0.0, updater 1.0.0, phone link v1 and
   custom ESP-NOW v1 with wire byte 1 in `versions json`.
6. `docs/COMPATIBILITY.md` lists the new numbers.

Already at 1, leave as they are: HW1-PIPE-v1, the CM5 control protocol, the OTA manifest format and
the OTA layout ids. Not HardwareOne's to renumber: ESP-Hosted, ESP-IDF, arduino-esp32 and the speech
model file names, which are checked at runtime.

## Cut a release
1. Draft notes from `git log <last-tag>..HEAD --oneline`, grouped into Added / Changed / Fixed / Security / Docs.
2. Bump the version in ALL four files above so they agree.
3. Add `## [X.Y.Z] - <date>` to the top of `CHANGELOG.md`. Get the date from `date +%F` - do not guess it.
4. Commit the change as ONE `vX.Y.Z: <plain summary>` commit that carries the code + the version bump (all four files) + the CHANGELOG entry. The version-prefixed commit IS the release - there is no separate content vs `chore(release)` split.
5. Check the build records before tagging. The release commit from step 4 is the commit the gate
   must pass on. For every build directory you archive or hand out, read its `BUILD_INFO.md`:
   the **Source:** line must show `git rev-parse --short HEAD` and "(clean tree)". If a build ran
   before this commit (for example a gate build made before the version bump), rebuild it here on
   the clean tree with `tools/build_board.sh <board>`, `tools/build_board.sh <board> --config <name>`
   (or `python3 tools/build_config.py build <name>`) for a `build-config-<name>/` directory, or
   `tools/build_deployment.sh` for a deployment row; the build rewrites `BUILD_INFO.md`
   (`tools/gen_build_info.py`). Do not run `gen_build_info.py` alone on an old build: it reads the
   current HEAD, not the commit the image was built from.
6. Tag and push - push the tag EXPLICITLY, in two commands:
   ```
   git tag vX.Y.Z
   git push origin main
   git push origin vX.Y.Z
   ```
   Do NOT rely on `--follow-tags`: it pushes annotated tags only, and every tag in this repo is
   lightweight (`git tag vX.Y.Z` with no `-a`/`-m`). Using it silently pushes the commit and
   leaves the release untagged on the remote - that is how v0.99.6 and v0.99.91.1 ended up with
   commits but no tags. Verify with `git ls-remote --tags origin | grep vX.Y.Z` before step 7,
   because `gh release create` needs the tag to exist on the remote.
7. OPTIONAL - create a GitHub Release (notes mirror the CHANGELOG section). Write notes to a file and use
   `--notes-file` (NOT `--notes`) so backticks/quotes/apostrophes are not mangled by the shell:
   ```
   gh release create vX.Y.Z --title "vX.Y.Z - <theme>" --notes-file <file>
   ```
8. Verify: `gh release view vX.Y.Z --json tagName,url`; confirm `git status` is clean and local == origin; report the URL.

## This repo's specifics
- Multi-board firmware: do NOT attach board binaries to releases. A single `.bin` is board-specific
  and a footgun; keep releases source-only (GitHub attaches the source archive automatically). Build
  per board with `tools/build_board.sh <board>` (its own `build-<board>/` directory and target, no
  fullclean; `docs/BOARD_SWITCHING.md`), a saved configuration with `--config <name>`, and a
  deployment row with `tools/build_deployment.sh`.
- Never `git add` build artifacts (`build/`, `*.bin`, `sdkconfig.<board>`) - they are gitignored; do not attach them.
- Release notes are world-readable if this repo ever goes public: keep them user-facing - no secrets,
  credentials, internal paths, or private hostnames.
- Most tags have no GitHub Release and that is fine. A missing Release is NOT a backlog item and
  needs no backfill - check the CHANGELOG entry instead, which always exists. Step 7 is optional
  by design.
- ASCII only in docs (this repo strips em-dashes): use `-` and `->`, not unicode dashes.
