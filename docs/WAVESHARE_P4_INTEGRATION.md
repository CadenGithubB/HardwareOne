# Waveshare P4 integration

Prepared on 2026-11-01 in `codex/waveshare-main-integration`. The integration
branch contains current main plus the Waveshare support and the factoring below.
Main has not been advanced, nothing has been pushed, and neither device was
flashed during this integration. A physical smoke test remains before merging.

## Source and checkout

- Upstream: `aa6508fd288a8693d14150c46575c8d68185b7f6` (`origin/main` when fetched).
- Original support: `a18efd5843baeffac20caca273f3dbd5b5f5d39f`.
- Common ancestor: `40261a39b525db8f7f1973d1f323c3fcac5cb8f0`.
- Integration merge: `4b3c0550`.
- HAL/UI ownership factoring: `fa3473c2`.
- Explicit legacy companion update policy: `ab387b02`.

The isolated checkout is
`/Users/cadbecaimacmini/.codex/worktrees/waveshare-integration/hardwareone`.
It protects the original checkout's unrelated STT, G2 and OpenClaw edits.
Firmware was built from the integration working tree subsequently recorded in
these commits; its reports therefore show the merge HEAD plus local changes.
No firmware source changed between those successful builds and the commits.

There were two textual merge conflicts. The command registry retains main's
`displayrequireauth` documentation and adds the guarded speaker module. The
wizard retains main's Bluetooth include and the touch adapter include. Main's
filesystem authorization, radio recovery and power-policy changes are retained.

## Ownership boundaries

| Area | Result |
| --- | --- |
| Board configuration | Wiring, panel timing, flash geometry, ADC divider and required SDK settings remain board-owned. The bring-up profile is an optional application selection. |
| Display | HX8394 initialization, DSI cache handling and the measured backlight limit remain in the Waveshare backend. The canvas uses the shared allocation policy. |
| Touch presentation | `OLED_TouchControls.cpp` draws the physical controls through a small synchronous HAL surface-paint operation. The driver no longer imports UI layout or button labels. Drawing and hit-testing still share one rectangle table. |
| Touch input | GT911 owns I2C polling; HAL_Input owns snapshots and queued events; the UI adapter owns navigation. The no-input path excludes touch-context work, and existing physical buttons retain their fallback. |
| Speaker | `HAL_Speaker_Waveshare.cpp` implements the shared speaker API. Disabled builds keep the unavailable stub. The unchanged Espressif codec source and license are under `third_party/es8311`. |
| Radio | SDIO wiring comes from the board's SDK configuration. EYE's existing UART/boot constants moved to its board header without changing pin numbers. A board without that UART reports the console operation unavailable. |
| SD and battery | Existing shared backends use board-selected pins, optional card detection and ADC scaling. No separate Waveshare battery subsystem was introduced. |

Normal Hosted startup and recovery require the version RPC to succeed, as on
main. Only the authenticated `c6update` worker can open a legacy update session;
it validates the image first and requires a successful remote probe. A legacy
session reports normal radio availability as false, skips normal identity and
monitoring setup, and uses 1 KB OTA chunks. A failed update-only transfer closes
that session before restoring users. Modern companions keep the existing larger
chunk size and normal post-update verification.

Existing board defaults, saved JSON profiles and partition CSVs are unchanged.
The Waveshare factory-only 32 MB layout remains separate and provides no
application OTA slots. Adding this board does not migrate existing devices'
filesystems or partition layouts.

## Verification

All four full firmware builds and saved-choice checks passed with ESP-IDF 5.5.5:

| Saved profile | Image bytes | App partition bytes | Free bytes |
| --- | ---: | ---: | ---: |
| `waveshare-p4-bringup` | 2,500,768 | 8,388,608 | 5,887,840 |
| `p4-glasses-test` | 5,629,488 | 7,426,048 | 1,796,560 |
| `xiao-default` | 4,384,352 | 5,984,256 | 1,599,904 |
| `qtpy-oled` | 2,891,200 | 5,984,256 | 3,093,056 |

The 17 pre-existing saved profiles were preprocessed against both main and this
integration. Existing macros retain their effective values; compiler assertions
checked the changed derived expressions. Disabled Waveshare peripheral choices
and display-disabled touch also pass configuration checks. Native XIAO covers
headless/no-speaker behavior; QT Py covers OLED with no physical input; EYE keeps
its normal display, encoder, camera, speech, SD and Hosted radio selection.

Compiler-resolved checks confirm Waveshare display 5, input 4, speaker enabled,
SDMMC enabled and battery ADC 20. EYE remains display 4, input 3, speaker disabled
and battery ADC 49. Waveshare retains cache-safe DSI refresh and its 32 MB table;
EYE retains `partitions_no_sr_16mb_p4x_eye.csv`.

Central host testing: **324 of 328 enabled tests passed** after resolving the
integration failures. Touch input, control rendering, shared battery and legacy
radio tests are registered centrally. Focused display, battery, touch and radio
runs also passed with ASan/UBSan. Four browser tests were disabled because Node
was not configured. Four remaining failures are inherited from main:

- `oled_mode_availability`: stale Bluetooth availability expectation; reproduced
  with the production source overlay from `aa6508fd`.
- `ble_central_coordinator`: Clang rejects uninitialized fixture identity tokens;
  coordinator, TX implementation and fixture are unchanged from main.
- `tof_poll_outcomes`: the extraction fixture omits main's new I2C health-event
  helpers; relevant production and fixture files are unchanged.
- `tool_pins`: secure-event tools pin 165 kinds/3149 bytes while main's fixture
  contains 167 kinds/3190 bytes; relevant inputs are unchanged.

The suite is not fully green; these failures were not hidden or disabled.

## Build reproducibility and retained evidence

Use the saved-profile wrapper so automatic CMake reruns preserve board identity:

```sh
source /Users/cadbecaimacmini/esp/esp-idf/export.sh
IDF_PY_BUILD_JOBS=4 python3 tools/build_config.py build waveshare-p4-bringup build
python3 tools/build_config.py check waveshare-p4-bringup
```

An initial bare-Ninja retry selected generic P4 settings during an automatic
CMake rerun and was rejected as verification evidence. The successful P4 builds
were checked for the actual board, peripheral and partition selections above.
For host compiler subprocesses in this Mac's exported SDK environment, set
`SDKROOT="$(xcrun --show-sdk-path)"` explicitly.

The private Arduino dependency was verified using main's
[recorded patches](arduino-local-patches/README.md) and the
[phone-pipe SDK hash checker](../tools/ble-phone-pipe/README.md). The P4 overrides
were staged with `tools/p4/prepare_sdk.sh`. No mutable dependency was symlinked
to the original checkout. Provision those dependencies again before rebuilding
this now-cleaned checkout; shared download caches remain available.

Evidence is retained under `work/waveshare-integration/` in the isolated checkout:
full build/test logs, profile-comparison script/results, binary hashes, resolved
configuration and dependency locks. `artifacts/waveshare-p4-bringup/` additionally
retains the application binary, matching ELF/map, bootloader and partition image.
This is build evidence, not a complete factory-flash bundle; no filesystem image
is included. The artifacts occupy about 52 MB.

Completed build trees and private dependency copies were removed (about 3.98 GB
of file contents; exact inventory in `cleanup.json`). The managed source
checkout is retained for review. Archive it after integration is accepted,
preserving needed ignored evidence first. The original checkout and its working
firmware/dependencies remain intact.

## Before advancing main

Run a Waveshare device smoke test on this integrated image: invalid-credentials
popup, brightness settings, short touch taps and context changes, SD persistence,
and idle/wake with main's Adaptive power policy. Include sustained display plus
filesystem/radio activity because earlier bring-up logs recorded underruns and
a companion recovery panic outside the short popup test.

Previous physical evidence and limitations are in
[WAVESHARE_P4_SUPPORT.md](WAVESHARE_P4_SUPPORT.md). The user confirmed stable popup
rendering and improved touch response on earlier firmware, and SD persistence
was tested. Battery checks used USB without a cell; audible speaker confirmation
was deferred. These builds do not extend that physical qualification.
