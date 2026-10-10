# Logger verification — 2026-10-09

This record covers the shared sensor selection, logger settings ownership,
health command routing, and shared session startup changes. Both saved profiles
were flashed and reboot restoration passed. A host test pass, a successful
firmware build, and a visible hardware result are recorded separately below.

The private local evidence archive contains serial captures and
before-test settings snapshots; those raw files are not committed because they
can contain device identifiers, authentication details, or personal readings.

## Equipment and firmware

- QT Py ESP32 Pico with ToF, presence, GPS, and SSD1306 OLED connected. GPS
  testing took place indoors without a satellite fix.
- XIAO ESP32-S3 for logger and Bluetooth accessory checks. G2 glasses and an R1
  ring were available for connection and display testing.
- Both initial test images were flashed and the flash tool verified their
  hashes (`qtpy-flash.log`, `xiao-flash.log`). Settings were captured before
  testing; restoration is recorded separately below.

Owner, GPS, health-policy and G2 interaction tests ran on the initial flashed
images. After the OLED-only route correction, final images repeated reboot
restoration and OLED route checks; the full live sequence was not repeated.
Those first-pass image stages are identified separately in each build
directory's `logging-hardware-test-status.json`. A second pass exercised Web
Health on the 5,875,552-byte XIAO image and prepared separate OLED-input and G2
reopen images; their qualification is recorded below.

## Host tests

All eight selected suites passed on this retest, with no failed tests:

| Suite | Scope |
|---|---|
| `command_catalogue` | Registered commands and their catalogue contracts |
| `command_ingress_guards` | Command entry-point authorization guards |
| `settings_downstream_persistence_contract` | Settings consumers and persistence contracts |
| `sensorlog_selection` | Shared selection metadata and selection mutations |
| `sensorlog_ui` | Logger UI command construction and selection behavior |
| `sensorlog_settings` | Logger-owned numeric settings and health restart behavior |
| `gpslog_start` | GPS orchestration, shared startup, and failure paths |
| `health_logging_ui` | Health controls using their authorized command paths |

Evidence: `host-eight-tests.log`, run through CTest in a local host-test build
directory. This is the focused regression set for
these changes, not a claim that every repository test was run.

## On-device logger behavior

The following checks completed on both boards:

- Select ToF and GPS, toggle GPS off and back on, and reject an invalid sensor
  list without changing the saved or live selection.
- Select numeric Track format and confirm it selects GPS only.
- Start a CSV session at an explicit path containing a space. While it is
  running, reject both named and numeric sensor-selection edits without
  changing saved settings.
- Query the numeric mask and format without changing saved settings or the
  live selection.
- Stop the session and read back a real CSV file with the selected ToF and GPS
  columns. This verifies the logger and file output; it does not imply the
  XIAO had those sensors physically attached or produced valid readings.
- Check that no crash record was reported during these test sequences.

On the QT Py, `gpslog` also refused to replace an existing logging session and
preserved its settings. Evidence: `qtpy-owner-results.json`,
`xiao-owner-results.json`, and the corresponding `*-owner-tests.log` and
`*-owner-finish.log` files.

The QT Py also recorded a combined ToF, GPS, and presence CSV session to a real
file. Readback metadata reports 2,987 bytes; the included preview contains 30
complete rows matching the 25-column header, with advancing timestamps, valid
ToF object readings, and changing presence/motion values including detected
events. GPS fields report
no fix, consistent with the separate valid indoor GPS communication checks;
these are not valid position samples. The preview ends partway through another
row, so the preview alone does not establish the total row count. Evidence:
`qtpy-live-csv-results.json` and `qtpy-live-sensor-readings.json`.

Two early interruptions were test-harness errors, not firmware failures. The
status assertion initially expected comma-separated sensor names, but the
device prints spaces. Separately, the client read a job-result JSON file before
its write completed. The assertions/result-read handling were corrected and
the affected owner checks completed; the original logs retain those errors for
traceability.

## GPS startup and indoor soak

On the QT Py, invalid intervals (`99`, `3600001`, `500junk`, and `500 1000`)
were rejected without changing logger or GPS settings. Disabling the logger
master switch refused GPS logging setup without changing GPS preferences.

A configured filename containing literal double quotes was refused by the
existing VFS filename policy. The command propagated the actual file-creation
failure and did not newly enable either auto-start preference. This is a
verified failure path, not a successful quoted-filename test. The initial
script expected that filename to succeed; its assertion is retained in
`qtpy-gps-tests.log`. Removing literal quotes while retaining a space allowed
startup to succeed.

The successful `gpslog 500` run saved Track format, GPS-only selection, and a
500 ms interval, then enabled logger and GPS auto-start. Its response reported
the logger as active and the GPS as already running separately. Four subsequent
samples reported connected, valid GPS communication with advancing cache
timestamps; all remained without a satellite fix. No crash record was reported
during the soak.

Reading the resulting file returned the GPS track header. There were no
coordinate records because no fix was available. This qualifies startup,
device communication, no-fix handling, and file creation; it does not qualify
outdoor positioning, coordinate samples, or signal-loss/reacquisition records.
Evidence: `qtpy-gps-results.json`, `qtpy-gps-finish.log`, and the private job
result for the track-file readback. The later reboot resumed GPS communication
and Track logging with the saved 500 ms interval, GPS-only mask and literal
spaced path. See the reboot section below for evidence.

## Health logging and R1 limitation

On the XIAO, health logging started an actual CSV/R1 session using a filename
with a space, enabled its saved auto-start policy, and reported sealed capture
encryption. Repeating `healthlogging on` preserved the same session. Turning it
off stopped an R1-only session. With GPS already selected, enabling health
logging added R1; disabling it restarted a GPS-only session. No crash record
was reported during these transitions. Evidence: `xiao-health-results.json`
and `xiao-health-tests.log`.

These checks qualify the logger owner and health policy transitions. They do
not qualify live health measurements or encrypted-data readback. The available
R1 reports firmware `2.3.2.0007`; setup identifies its profile as unknown and
returns `profile-unknown` (`xiao-live.log`). No protocol profile override or
bypass was applied. Live R1 vitals remain unqualified.

A read-only audit of the available local protocol evidence found no capture
for `2.3.2.0007` among 93 raw captures and 17 decoded capture files. The exact
older-firmware profile gate is intentional; the available evidence does not
justify treating this version as a supported profile. No version gate was
broadened for testing.

The user later reported a plain ring tap-and-hold, distinct from a tap followed
by another press-and-hold. The existing G2 input path distinguishes these:
plain holds arrive through the glasses as `LONG_PRESS`/`LONG_RELEASE`, while
the glasses own the tap-then-hold menu gesture. That input path uses the
glasses firmware profile independently of the direct R1 health connection.
The observation does not establish direct health compatibility, and this
specific gesture's decoded event pair was not captured in the test record.

Two later physical Health-page actions submitted successful
`healthlogging toggle` commands through the authenticated G2 command path. The
final status and settings confirm health logging off, no selected sensors, logger inactive,
and logger auto-start off. The user reported that the R1 was not connected;
the Health-page visual indicator was not confirmed. Both Health rendering
paths return early with an offline message before drawing the logging
indicator when R1 is disconnected, explaining why it could not be observed. Evidence:
`g2-health-results.json` and the corresponding G2 command records in
`xiao-live.log`. Command execution and final owner state are verified; live
ring data and the visual health indicator remain unqualified.

## Display checks and discovered route omission

The user could see the OLED main menu. A missing OLED route for the logging
screen was found and corrected in `OLED_Utils.cpp`. The route check and focused
diff are saved as `oled-logging-route-check.cpp` and
`OLED_Utils.logging-route.diff` in the evidence directory.

Both saved profiles built successfully after that correction in the first pass:

| Profile | Image bytes | App partition bytes free |
|---|---:|---:|
| `qtpy-espnow` | 3,443,824 | 2,540,432 |
| `xiao-default` | 5,875,552 | 108,704 |

Evidence: `qtpy-oled-build.log` and `xiao-oled-baseline-build.log`. Both
first-pass final images were flashed application-only, with hash verification,
preserving the filesystem. `qtpy-final-flash.log` and `xiao-final-flash.log` identify these
flashes; the post-flash boot logs match the final ELF hashes recorded in
`final-build-artifacts.json`. The older 3,443,792-byte QT Py flash precedes the
OLED route correction. On the corrected image, `oledmode logging` succeeds
and `oledread json` reports the connected SSD1306 in Logging mode. Visual
confirmation was still pending at that stage; the second pass is recorded below.

The first-pass `qtpy-espnow` profile has no enabled physical input driver.
For the second pass, a gamepad was connected and the separate
`qtpy-logging-test` profile enabled its driver and OLED input. Its
3,492,096-byte image built with 2,492,160 bytes of app-partition headroom and
was flashed with hash verification. Evidence: `qtpy-logging-input-build.log`
and `qtpy-logging-input-flash.log`.

On that image, the user confirmed the OLED ToF selection changed from OFF to
ON. Command records identify the authenticated display session as the source
of the ToF toggle, the change to a 10,000 ms interval and CSV format, and Start
and Stop at 414,199 and 435,522 ms respectively (about 21 seconds apart).
Readback of the actual `oled inputs.csv` contains three complete sample rows
matching its 13-column header. The ToF validity fields are false in those rows;
this verifies physical logger controls and file output, not new valid ToF
measurements. The user's final visual response was "I think it worked", so the
Start/Stop redraws do not have definite visual confirmation. Evidence:
`oled-physical-command-results.json`, `remaining-input-qtpy-live.log`, and
`capture-readback-checks.log`.

The initial G2 display attempt was not visible. On the repeated sequence, the
user confirmed seeing the exact text `HW1 DISPLAY TEST`; clearing and reopening
the display was needed before the menu appeared. The user then confirmed the
GPS selection checkmark and the logger start/stop redraws. Serial command
records independently show `sensorlog sensors toggle gps`, `sensorlog start`,
and `sensorlog stop` executing successfully through the authenticated G2
command path. The final status was inactive with GPS selected. Evidence:
`g2-physical-toggle.json`, `g2-start-stop-results.json`, and `xiao-live.log`.
This qualifies those visible logging controls after the clear/reopen recovery;
it does not establish that the initial display-opening problem is resolved.

A subsequent G2 manual-reopen correction passed 532 assertions against the
production admission, bootstrap and command paths, plus the owner/epoch and
command-catalogue checks. The updated XIAO image built at 5,875,968 bytes with
108,288 bytes of app-partition headroom. Evidence: `g2-reopen-assertions.log`,
`g2-reopen-host-tests.log`, `g2-reopen-owner-epoch.log`,
`g2-reopen-command-catalogue.log`, and `xiao-reopen-build.log`. That image was
flashed with hash verification (`xiao-reopen-flash.log`). Manual `g2reopen`
now waits for the interactive lease to release and uses the existing teardown,
guarded by connection generation and lifecycle epoch, before the shared startup
sequence. A real firmware `MenuStartUp` (command 17) retains its existing
startup ordering.

On the corrected image, the user first confirmed the text `REOPEN TEST` was
visible. A single `g2reopen`, without a separate clear command, then produced
a visible, responding menu. The user also confirmed the GPS selection
checkmark changed. This qualifies replacing the live test display with the
menu and using its logging selection control. Evidence:
`g2-reopen-visible-result.json`.

A repeat with the Health page's interactive lease active recorded its abort
and cleanup, `ShutdownSuccess`, `CreateSuccess`, and return to the main menu
without a failure. There was no separate visual confirmation of this repeat,
so it qualifies the recorded command/lifecycle outcome only. Evidence:
`g2-reopen-health-repeat.json`. Neither sequence establishes recovery when the
glasses plugin is asleep or unresponsive.

## Authenticated Web Health second pass

The XIAO's actual HTTP server passed the following requests on its
5,875,552-byte image, before flashing the G2 reopen correction:

| Request | Verified result |
|---|---|
| Unauthenticated `GET /api/health/status` and `POST /api/health/action` | 401; logger state unchanged |
| `POST /login` | 303 and a usable authenticated session |
| Authenticated `GET /r1-health` and `GET /api/health/status` | 200; page contains the logging action and status reports the initial inactive logger |
| Invalid action at `POST /api/health/action` | 400; logger state unchanged |
| `action=poll` while the ring is disconnected | 409 |
| `action=logging-toggle`, then status | 200; logging enabled, active, R1 selected, worker running and session sealed all become true |
| A second logging toggle, then status | 200; all five state fields become false |
| Authenticated `GET /api/files/view?name=...&mode=raw&dec=1` | 200; saved session marker and CSV structural header returned |
| `GET /logout`, then Health status with the old session | 302, then 401 |

The saved file contains only the `#HW1ENC` marker and plaintext structural CSV
header, with no encrypted data rows. This verifies authenticated access to the
saved capture and its header; it does not verify row decryption or live health
measurements. Page retrieval also does not establish browser-rendered layout
or button interaction. Evidence: `web-health-tests.log` and
`web-health-results.json`.

## Encrypted data-row readback

The QT Py subsequently recorded a gamepad CSV session with capture encryption
temporarily set to `all`. Raw `fileread` output contains the `#HW1ENC` marker,
the plaintext structural header, and 11 data rows, all encoded as `ENC1:`
ciphertext. An authorized `fileview` on the same device reveals all 11 rows,
with the matching four-column CSV schema and no undecryptable-row marker.
This qualifies actual on-device encryption and authorized serial-viewer
readback. It does not qualify live R1 readings or encrypted data-row readback
through HTTP. The capture encryption preference was restored to `health`.
Evidence: `sealed-gamepad-results.json` and `capture-readback-checks.log`.

Two harness corrections were needed for these readbacks. OLED command output
uses its routed reply rather than the serial broadcast the initial assertion
expected; reading the literal capture path with `fileread` verified the
already-written file. Serial `fileview` broadcasts can insert timestamp
prefixes at chunk boundaries, so those prefixes were normalized before
counting decrypted rows. The capture itself did not need to be repeated.

## First-pass reboot and cleanup

Both boards restored exactly the logger preferences captured immediately before
reboot and resumed active logging in a fresh boot capture directory. The path
was shaped once, with no duplicated boot-directory component. QT Py restored
GPS-only Track at 500 ms and reported valid GPS communication without a fix.
XIAO restored R1-only CSV with health logging enabled and the saved health-only
encryption policy. Neither reported a crash record. This verifies startup and
policy restoration; it does not supply missing R1 measurements or qualify
ciphertext readback. Evidence: `post-reboot-results.json`,
`post-reboot-tests.log`, and `post-*-live.log`.

Original logger preferences were restored exactly on both boards, and both
loggers were verified inactive. QT Py GPS preferences were restored; GPS, ToF
and presence stop requests completed, confirmed by disconnected snapshots.
XIAO Bluetooth settings match the initial snapshot, BLE is deinitialized,
ESP-NOW is running again with auto-start restored, and temporary debug flags
are off. G2 pairing learned during the test and generated capture files are
retained. No flash erase was performed. Final crash queries reported no record,
and the serial captures contain no panic, assertion or watchdog-crash signature.
Evidence: `*-restored-logger.json`, `*-restore.log`,
`final-runtime-results.json` and `xiao-radio-restored.json`.

## Second-pass QT Py cleanup

The QT Py's original logger and GPS preferences were restored, its logger is
inactive, and test-started ToF and GPS tasks are stopped. Capture encryption
is back to `health`. The gamepad and OLED were left usable on Menu with the
`qtpy-logging-test` image. User-added Wi-Fi entries were retained. The final
crash query reports no record. Evidence: `remaining-qtpy-restore.log` and
`remaining-qtpy-final.json`.

## Second-pass XIAO cleanup

The XIAO's original logger preferences and Bluetooth controls were restored
exactly. The logger is inactive, BLE is deinitialized, and ESP-NOW is running
with auto-start restored. Temporary debug flags are all zero. User-added saved
Wi-Fi entries were retained, and the final crash query reports no record.
Evidence: `remaining-xiao-restore.log`, `remaining-xiao-final.log`, and
`remaining-xiao-final.json`.

## Remaining qualification

- Obtain definite visual confirmation of OLED Start/Stop redraws; physical
  command execution, the ToF selection indicator and CSV output passed.
- Confirm the G2 Health-page indicator with a supported, connected R1. Track
  asleep or unresponsive plugin recovery separately from the manual replacement
  of a live display that passed here.
- Qualify live R1 health data only when a supported device profile is available.

SD-card testing and memory reduction remain deferred as requested. No claim is
made here for those features or for a complete physical qualification of every
logger entry point.

## Map live recording — 2026-10-10

The QT Py ESP32 Pico passed 33 bounded command/lifecycle checks using the
preserved `0.99.97.1` `qtpy-maps-test` checkpoint image. Its application image
is 3,702,576 bytes, SHA-256
`607d8ed2ea6534c3a4604616a755561f0d19cba0b529e8e3fc0015c4ae1b1aef`.
This is the image before the saved-Wi-Fi directory relocation, not the later
build at the same version. The XIAO was not accessed during this pass.

Preflight found the logger inactive, GPS stopped and the tracks directory
empty. Original logger/GPS preferences and relevant runtime settings were saved
privately before testing. An optional partition-table read encountered serial
noise; application-only flashing subsequently completed with hash verification.
Bootloader, partition table, NVS and filesystem partitions were not written.

Passed checks covered:

- Invalid arguments and a disabled logger refuse live recording.
- An independent active logger prevents Map startup and remains active.
- Live start succeeds, a repeated start is idempotent, and stop/toggle produce
  the expected recording state.
- Clear and load refuse while live recording is active.
- GPS and logger boot auto-start preferences remain unchanged.
- An external logger stop is reflected by Map status.
- A stale Map stop leaves a replacement logger running, even when it uses the
  same capture path as the former Map session.

GPS returned connected, valid samples with no fix and zero satellites, matching
indoor conditions. The resulting capture establishes session/file startup, not
coordinate recording or positional accuracy. Visible display output, physical
buttons and actual HTTP routes were not qualified in this pass. Existing host
UI tests remain separate evidence, and broader track-buffer concurrency is
outside this lifecycle check.

After testing, logger/GPS preferences matched their original values exactly;
ESP-NOW and display preferences were unchanged. The logger is inactive, GPS is
stopped, both test captures were removed and the tracks directory is empty.
The OLED is authenticated in Menu. No crash record was reported. The QT Py
remains on the tested checkpoint Maps firmware, and its serial port is closed.

Private evidence includes `checks.json`, `summary.json`, `final-state.json`,
`gps-live-sample.json`, the verified firmware snapshot and raw command/flash
logs. Settings backups, credentials and device identifiers stay outside Git.
