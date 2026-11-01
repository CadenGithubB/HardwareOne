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

## Wake flicker investigation — 2026-11-01

After the integrated-image touch/popup smoke test passed, the user reported
flicker when waking from sleep. The Waveshare backend previously illuminated
immediately after DISPLAYON, before the UI refreshed. This ordering predates
the integration; it is a hypothesis for the reported flicker, not a proven
merge regression.

The backend now blanks before DISPLAYOFF and gates backlight PWM through the
first successful refreshed-frame cache flush after DISPLAYON. It then waits
80 ms, matching the vendor initialization table's display-on settling interval.
Brightness updates cannot bypass the gate; unchanged canvas comparisons cannot
skip the initial wake flush. The 720/1024 PWM cap is preserved. Shared power
policy and other display backends are unchanged. CPU clock-transition effects
remain an alternative cause if hardware still flickers.

Production-method regression checks pass with ASan/UBSan for sequencing,
brightness gating, failed cache flush, repeated DISPLAYON, wake cancellation
and failed DISPLAYON. Existing shared display lifecycle variants also pass.
The saved Waveshare configuration check passes. Physical confirmation remains
pending; do not label the flicker fixed until the user retests.

The isolated integration build directory and its provisioned SDK dependencies
are retained temporarily for this active hardware investigation. Remove compiler
objects and private dependency copies after wake behavior is accepted, retaining
selected firmware/debug/config artifacts and the small verification logs.

## Immediate idle command and new evidence

User reports the 80 ms wake gate still flashes. Added admin-only `powersleep`
to request the normal display-idle path on its owner loop without changing the
saved timeout. Local input wakes the display; remote activity can restore the
active CPU clock without lighting the panel. Busy workloads, transition cooldown
and OTA probation reject admission. An accepted request is asynchronous; the
owner loop rechecks workload/cooldown before entry. A disabled automatic timeout
does not immediately undo manual idle. Unsupported headless builds reject it.

Power policy tests passed 672 fresh-process scenarios across ESP32/P4 revisions,
display and UART/STT combinations under ASan/UBSan; OLED power settings and the
command catalogue's eight feature selections passed. Waveshare build passed
(2,501,648-byte application) and esptool verified its flash. Existing timeout
was confirmed as 10 minutes. Serial `powersleep` returned accepted, followed
immediately by an LCD DSI external-memory-fetch underrun warning. This is new
evidence for the continuously running DSI stream being disturbed by idle
clock/downclock behavior; it does not yet prove the cause of the visible flash.
Next diagnostic: compare sleep/wake at a fixed CPU clock, then restore the saved
Performance mode. Do not label the earlier settling delay a confirmed fix.

Current logs: `work/waveshare-integration/manual-idle-{build,flash,runtime,tests,baseline}.log`.
P4-EYE baseline build passed: 5,630,224-byte application with 1,795,824 bytes
free. No baseline hardware was flashed. Its compiler build directory was removed
after retaining manifest/config evidence (`manual-idle-cleanup.json`).
User confirmed immediate sleep and touch wake work. A second immediate sleep
was requested in temporary Locked power mode to isolate the clock transition;
no underrun warning appeared at that entry. Visual comparison is pending and
Performance mode must be restored afterward.
