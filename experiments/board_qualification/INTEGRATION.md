# Full-application integration audit — 2026-09-28

This document combines source/tool audit notes with explicitly identified
physical outcomes. [RESULTS.md](RESULTS.md) is the final test summary. The
coordinator alone owned serial ports, flashing and configuration changes. The prior JPEG
probe results are in [JPEG results](../jpeg_portable/RESULTS.md). The user confirmed
boards only for this phase, with no SD card in either board. G2/R1 and actual
Android UI checks below are deferred; USB full-app conversion is the current
integration target.

## Prior JPEG qualification builds

This opening section describes the preceding JPEG milestone. Today's separate
USB diagnostic builds retain the original camera-disabled profiles on both
boards; see [their fingerprints](jpeg-diagnostic-builds.json) and the diagnostic
preparation section below. The preceding milestone's reproducible applications are `../jpeg_portable/private/app-p4` and
`app-s3`, with binaries in `private/build-p4/hardwareone-idf.bin` and
`private/build-s3/hardwareone-idf.bin`. Their exact fingerprints are recorded in
[qualification.json](../jpeg_portable/qualification.json). These include the
previous isolated power, BLE, Hosted and P4 input/display changes; the tracked
base application alone does not contain every previous investigation change.

| Profile | Original installed application | Prior JPEG milestone build |
|---|---|---|
| P4 | P4 IO profile; LCD, wheel/buttons, G2/R1, HTTP and mesh; camera disabled | Same feature profile, new shared JPEG decoding |
| S3 | BLE-role profile; camera and Sense features disabled | Camera and Sense enabled to compile both G2 live-camera decode paths |

The S3 difference is substantive: `ENABLE_CAMERA_SENSOR` and
`XIAO_ESP32S3_SENSE_ENABLED` change from 0 to 1. Sense changes the board model,
disables the base LED on GPIO21 and enables the expansion board's SD SPI pins
(CS21/SCK7/MISO8/MOSI9). Microphone remains disabled. The sdkconfig values were
unchanged in the JPEG qualification. A full-app S3 deployment therefore needs
its feature profile selected deliberately; it is not solely a JPEG change.
P4 camera enablement needs the separate camera work described in CAMERA_STORAGE.md.

The preceding milestone build commands, after activating the pinned IDF 5.5.5
environment, are:

```sh
bash experiments/jpeg_portable/build.sh p4
bash experiments/jpeg_portable/build.sh s3
bash experiments/jpeg_portable/build-codec.sh p4
bash experiments/jpeg_portable/build-codec.sh s3
```

They build only. `prepare.py --target p4 --check` and the S3 equivalent verify
the snapshots. `--refresh-hal` accepts only HAL changes: adding an integration
hook elsewhere requires a separate prepared destination/overlay and provenance,
not editing the old qualified snapshot silently.

Both original application layouts start at `0x10000`; the previous P4 factory
partition is `0x615000` bytes and S3 is `0x5b5000` bytes. Re-identify the connected
board and its actual table before reusing these bounds. The existing private
`device-20260928/run_image.py` performs app-only write, digest verification and
serial capture, but has old hard-coded ports and partition bounds. It is a
recorded one-run helper, not a portable launcher. Do not flash the generic codec
probe's bootloader/partition table over a provisioned application. Preserve the
new full backups and existing credentials; no factory reset is needed.

## Authenticated USB access and fixture files

`experiments/p4_mesh/console.py` provides `MeshConsole`, paced input, command
completion and redacted private logs. `p4_ble_roles/board_control.py` adds
`RolesConsole` lifecycle guards; `p4_io/board_control.py` additionally supports a
private-credential display login action. Importing these modules opens no ports;
running their main functions opens both configured boards.

The coordinator main reads one shared `private/credentials.json`, opens the
historical `DEFAULT_BOARDS` ports, logs in and enables runtime `loglink on`.
Those historical defaults are not current-board discovery, and one shared
credential record is not sufficient if the boards now have different accounts.
The worktree has no copies of these old credential files or the old Python
virtual environment. Their primary-checkout equivalents exist (credential files
mode 0600); this audit checked existence/mode only, never their contents. Adapt
port and per-board private credential plumbing before using the coordinator.
Do not invoke its `setup` action casually: that action provisions test AP/BLE
configuration. Normal command work only needs login.

Existing CLI contract:

- `login <user> <password>` authenticates the submitting interface; supply
  values only through private in-memory handling with redaction, never shell
  arguments or tracked examples. `whoami` verifies the named admin identity.
- An explicit `login … display` requires an already authenticated local
  interface and affects the display session. USB login alone is not display or
  G2 identity binding.
- `logout` removes the submitting interface's session.
- If onboarding appears, the normal Basic setup is `1`, an available deployment
  archetype choice, then username/password and the remaining displayed prompts.
  Basic mode needs no Wi-Fi credentials. Do not replay a fixed prompt sequence:
  compiled features change the archetype list. Existing boards should retain
  their accounts and should not enter onboarding after app-only replacement.

Synthetic JPEGs can be staged in LittleFS; an SD card is not required:

```text
files stats json "/"
files json "/jpeg_qualification"
mkdir "/jpeg_qualification"
filewrite "/jpeg_qualification/rgb420_320x240.jpg" 0 <base64-chunk>
filewrite "/jpeg_qualification/rgb420_320x240.jpg" <next-offset> <base64-chunk> final
fileread "/jpeg_qualification/rgb420_320x240.jpg" 0 768 b64
```

The placeholders are generated host-side from committed synthetic fixtures.
Use a fresh run-specific directory and check existence before writing: offset 0
creates or truncates. Later offsets must equal the file's current size, and each
reply must report `success:true` and the expected next size. A 768-byte raw chunk
becomes 1024 base64 characters and fits the 2047-character USB command limit for
these short paths. Use smaller chunks for BLE's approximately 512-byte command
frames. Read every chunk back, honor the returned actual length, reconstruct it
and verify SHA-256 before decoding. `final` runs post-save hooks; do not use
settings/system paths for fixtures. Cleanup supports
`filedelete "<exact-created-file>" confirm`, then `rmdir "<created-directory>"`.

## JPEG application path: ready and missing pieces

The real file path is:

`VFS guarded file read → loadJpgAsBmp288x144 → decodeJpegForG2 → HAL_JPEG →
288×144 4-bpp BMP builder → G2 image fragments`.

`loadJpgAsBmp288x144()` is static in `G2_Glasses.cpp`. It enforces the 128 KiB
compressed-file cap, uses the caller's auth context, retains the application's
PSRAM allocation policy, and frees decoded pixels before return. It does not
require a camera or radio itself. Its current callers are G2 viewer workers;
they select an eligible connected temple **before** invoking it. Consequently,
there is currently no supported USB command that exercises this complete
conversion without glasses.

`g2bmp` loads BMP files, not JPEG. `g2files` is a text-renderer CLI shim, not the
interactive Files chooser. Neither proves the new JPEG call site. The standalone
codec probe qualifies the HAL but omits application VFS/auth, BMP conversion,
worker lifetime and the G2 image transport.

**USB-only diagnostic now prepared in isolated application copies:**
`jpegdiag "/jpeg_qualification/<fixture>.jpg"` calls that same static loader
under the existing command/auth context. It requires a live named Serial administrator login even when normal auth-bypass
policy is enabled; other transports are refused. It accepts one quoted
fixture path (maximum 191 characters, required prefix, no parent traversal). The
checked [diagnostic patch](jpeg-diagnostic.patch) adds optional measurement fields
to the actual loader and a compile-gated command; production sources remain
unchanged. It returns success/error, source geometry/backend, RGB and BMP FNV-1a
hashes, validated BMP geometry/size, decode/BMP/total microseconds, internal free
heap/largest-block snapshots and heap-integrity flags. The BMP buffer is freed
before the after-snapshot. Total time includes diagnostic hashing; decode time
excludes hashing. JSON-response allocations occur after the reported heap sample.
One conversion per command lets the host pace a bounded loop without blocking
normal application tasks for an unbounded batch.

[jpeg-expectations.json](jpeg-expectations.json) supplies per-board RGB hashes
from the successful codec probes. Its small-image BMP hashes independently apply
the G2 Linear tone/letterbox rules to complete synthetic RGB dumps. The Python
reference was compared byte-for-byte with the extracted production C++ converter
on all 21 available buffers. These are reference expectations, not new full-app
results. [jpeg_expectations.py](jpeg_expectations.py) regenerates them from the
private codec logs and committed fixtures.
Compare normal, odd-width fallback, malformed/truncated and limit-exceeding
fixtures, repeated calls, and an allocation-policy/PSRAM-bypass case. This tests
application conversion but still does not establish G2 transport or visible output.
Preparation/build tools are [prepare_jpeg_diagnostic.py](prepare_jpeg_diagnostic.py)
and [build-jpeg-diagnostic.sh](build-jpeg-diagnostic.sh). They create new
`private/app-jpeg-p4`/`app-jpeg-s3` copies with per-copy source manifests and new
`private/build-jpeg-p4`/`build-jpeg-s3` outputs. Both retain their original feature
profiles: S3 camera/Sense remain off. Both final build commands exited 0;
post-build checks verified all 7,934 P4 and 7,924 S3 source files. ELF symbols
confirm the actual loader/diagnostic/HAL, hardware JPEG only on P4, and no live
camera workers on either target. Actual preprocessor output confirms camera,
Sense and microphone off; LCD is compiled only on P4. sdkconfig values and
partition binaries match the preceding full-app builds. Exact binary/source/log
fingerprints are in [jpeg-diagnostic-builds.json](jpeg-diagnostic-builds.json).
Physical results are recorded separately; compilation is not a passing runtime test.

```sh
python3 -B experiments/board_qualification/prepare_jpeg_diagnostic.py --target p4
python3 -B experiments/board_qualification/prepare_jpeg_diagnostic.py --target s3
# Existing copies: use --check instead of preparation.
bash experiments/board_qualification/build-jpeg-diagnostic.sh p4
bash experiments/board_qualification/build-jpeg-diagnostic.sh s3
```

**Ready with G2/R1:** use `blemode client`, `g2init`, `openg2 auto`,
`ringconnect`, then inspect `g2status` and `ringstatus json`. With the prepared
accessories, `g2hijacktest` starts the same Blocks/menu path; navigate Apps →
Files → the synthetic JPEG → View and View Full. Capture fragment acknowledgments
and errors, and ask for visible orientation/tone/cropping plus ring navigation.
The JPEG viewer has a 60-second hold cap and double-tap dismissal. Repeat a
software-fallback fixture. Check the G2 paired user's permissions rather than
assuming USB admin login also grants the G2 worker access.

The known prerequisite still applies: after a glasses reboot, connect G2/R1 to
the Even app once, then turn off the phone's Bluetooth without rebooting the
accessories. Earlier tests qualified this prepared state; they did not solve
that bootstrap dependency.

**S3 live camera:** `cameraread`, `opencamera`, `camerares qvga`,
`cameracapture` and `closecamera` exercise capture/status. Capture commands alone
pass camera-produced JPEG through and do not necessarily exercise HAL decoding.
The actual G2 paths are Sensors → Camera → Capture and Stream after camera
startup. Both call `decodeJpegForG2`. Neither has a direct CLI wrapper. Restore
any changed resolution/quality/FPS settings after the check. P4 uses a different
camera interface and its current application camera is disabled.

## Power and recovery boundaries

Ready commands in the qualified full-app snapshots are `cpufreq`, `power json`,
`status json`, `memreport json`, `taskstats json`, `perftop json`, `bootcount json`
and `uptime json`. `cpufreq <MHz>` is a temporary override; the advertised list
is authoritative (P4 rev3: 100/200/400, S3: 80/160/240). Read it back after each
change and restore the prior clock. Test representative JPEG/radio work at each
clock, with error/latency and memory observations. A `power mode` change also
persists the preset and changes brightness; capture and restore both values if
using presets. `voltage` is a model estimate, not an electrical measurement.
USB supply/current measurement needs external instrumentation and must account
for both P4 and C6.

Idle saving is compile-gated by `ENABLE_OLED_DISPLAY`. The S3 headless profile
cannot perform it. The LCD-enabled P4 snapshot has no runtime `oledConnected`
gate in `powerSaveTick`, so missing physical LCD alone does not disable CPU
scaling. Local capture inhibits it. Crucially, USB Serial commands stamp activity
and wake it: repeatedly polling `power json` can hide the idle state. Use passive
logs or a separately validated non-waking observer to prove the actual clock;
an idle frequency merely listed in status is insufficient. LCD blank/wake needs
a display. UltraSaver's 40 MHz path and peripheral work need their own measured
qualification. Direct clock changes do not use the idle path's I2C drain guard.

| Recovery check | Existing route | Scope/limit |
|---|---|---|
| ESP-NOW service restart | `closeespnow`, verify `espnowstatus json` uninitialized; `openespnow`, verify session and both-way payloads | Logical service recovery; does not reboot C6 |
| BLE role/lifecycle restart | Retire diagnostic central; `blemode server`, `openble`, inspect `blestatus json`; return through Client sequence | Shared lifecycle retains Wi-Fi/ESP-NOW transport; inspect actual G2/R1 shutdown, not just saved mode |
| Missing G2 arm | `g2recover` when exactly one arm is connected | Queued missing-arm repair; both down needs `openg2 auto` |
| Menu session recovery | `g2reopen` after established right-arm link | G2 app/menu only; cannot repair a dead peripheral link |
| Coordinated processor/radio restart | Normal authenticated `reboot` | Restarts P4; current SDK resets C6 on every host boot; preserve settings/identity, then authenticate again |
| Unexpected C6 reset | No qualified application command exists | Needs dedicated fault injection/recovery design |

`HAL_Bluetooth::bluetoothHalDeinit()` explicitly releases only BLE use and keeps
Hosted Wi-Fi/ESP-NOW alive. SDK `esp_hosted_deinit()` tears down the shared RPC
and transport, and `esp_hosted_connect_to_slave()` only attempts transport
reconfiguration; neither is a safe drop-in live-C6 restart for active owners.
`esp_hosted_cp_gpio_reset_pin()` resets a GPIO configuration, not the C6 CPU.
The current SDIO configuration has `TRANSPORT_RESTART_ON_FAILURE` enabled and
its error paths can restart the P4 to avoid transport races. Thus pulling a C6
reset GPIO during active traffic may reboot the whole unit; it is not a bounded
radio-only test. Establish orderly owner shutdown, failure deadlines, subsequent
reinitialization and saved-state checks before introducing that fault.

`lightsleep [seconds]` calls `esp_light_sleep_start()` without checking its return
and has no Hosted owner shutdown/recovery orchestration. Its “Woke” text alone
cannot prove sleep. `deepsleep` configures no wake source and requires physical
reset. Keep these outside the initial automated workload loop; they need separate
recovery qualification and a deliberate wake plan.

## Board-only BLE roles and active clocks

[test_board_roles.py](test_board_roles.py) is an explicit coordinator entry point
for the current diagnostic application pair. `--help`, import and `--preflight`
never open hardware. It reuses `RolesConsole` and `test_ble.run_probe`; it does
not run the old coordinator's `setup` action or open the Mac Bluetooth adapter.
Both boards must already accept the same named admin account, the target must
already require authentication and Secure Channel, and existing P4 ESP-NOW must
be running so the encrypted reply can prove the target identity. It performs no
onboarding, fresh pairing, SSID, name, secret or authentication-setting changes.

The coordinator supplies the currently observed ports and private credential
file; nothing defaults to the old board's USB paths. Example (fill the local
variables first; do not put credential values in shell arguments):

```sh
/Users/cadbecaimacmini/Documents/Codex/Projects/hardwareone/experiments/p4_connectivity/private/ble-env/bin/python \
  experiments/board_qualification/test_board_roles.py --physical \
  --p4-port "$P4_PORT" --s3-port "$S3_PORT" \
  --credentials "$PRIVATE_CREDENTIAL_FILE" --ble-mac "$OBSERVED_P4_BLE_MAC" \
  --clocks --run-root experiments/board_qualification/private/board-roles
```

`bleinfo json` and `blestatus json` expose remote connected-client addresses,
not the server's local BLE address. The wrapper reads the configured advertised
name and ESP-NOW MAC over authenticated P4 USB. `--ble-mac` is a separate, optional
**observed BLE advertisement** selector when the name is omitted from the
advertisement. Without it, discovery requires exactly one exact-name match. It
never derives a BLE address from the ESP-NOW MAC. Encrypted `espnowstatus json`
then checks the target against the authenticated USB identity.

Before any lifecycle mutation, it snapshots both boards' saved roles, actual
Server/Client initialization, peer targets/owners/auto-reconnect flags, active
clock/preset and log routing. It refuses existing BLE server, G2, ring or
diagnostic-central connections, and refuses ownerless enabled peer flags whose
restoration could silently assign ownership. Automatic reconnect flags are
temporarily disabled and verified. Normal `ringdisconnect` and `closeg2`
commands cancel work first; a bounded 12-second poll requires G2 idle/off with
both arms down and the ring disconnected with no pending connect. Exactly one
`g2deinit` then must acknowledge and produce `state=off`. `closeg2` success text
alone is insufficient: that handler can report disconnected while an internal
worker is still exiting. It exercises empty Client initialization, shuts that owner down, then starts
Server. S3's isolated GATT probe uses this initialized BLE host as the test
central while the production G2/R1 owners stay stopped; it does not connect the
production G2 client to the other board.

For each of two role cycles, the S3 central checks an initial encrypted connection
and a reconnect to P4: Device Information, MTU, fresh unauthenticated state,
pre-login denial, named admin login, command replies and GATT counters. The second
cycle verifies these operations after both boards have torn down and restarted
their production roles. This covers protocol and lifecycle behavior; it does
not claim actual G2/R1 interaction or Android UI coverage.

`--clocks` additionally checks active `cpufreq`/`power json` readbacks at
100/200/400 MHz on P4 and 80/160/240 MHz on S3, confirming the advertised
capability list and unchanged power presets. These commands are transient and
restore the initially observed active clock. They do not request sleep, change
brightness/presets or measure power. This is suitable for the current camera,
microphone, I2C and SD-disabled diagnostic profiles; direct clock commands have
no I2C drain guard and this test does not qualify active peripheral operation or
JPEG load at each clock. A separate paced JPEG workload can provide that evidence.

A `finally` path attempts restoration on both boards, including after one
board's failure, and compares saved roles, initialized owners, peer targets and
owners, auto-reconnect flags, power presets and clock. A first `g2init` lazily
registers G2/R1 peers for this boot. New entries are accepted on restoration only
when disconnected, auto-reconnect off, unowned, and with both target addresses
empty; every previously registered entry must still match exactly. These harmless
new registry entries are reported, not misrepresented as full RAM restoration.
The coordinator's later original-flash restore and reboot reset that boot state.
The wrapper restores log routing last. Restoration failures fail the run and remain in private `results.json`.
No script can guarantee restoration after disconnect, a poisoned serial barrier
or firmware failure: the coordinator must inspect this result and use the saved
full-flash backups if necessary. The wrapper does not reconstruct an in-flight
accessory scan, RAM reconnect-suppression history or an earlier accessory
connection; those are outside this boards-only scope. Re-enabling a saved
reconnect flag can resume its prior intent. Keep other UIs/consoles idle during
the run, and inspect final state before ending the hardware session.

Offline checks are `python3 experiments/board_qualification/test_board_roles_host.py`:
nine tests cover successful round trips, failed probes, independent board cleanup,
refusals for live or ownerless peers, cancellation-before-deinit, the cancellation
deadline, and strict saved-peer versus empty new-registration handling. These checks and runtime dependency
preflight passed; they are not physical BLE or clock results. The coordinator's
private run artifacts are the authority for any later device results.

The first physical wrapper run on 2026-09-28 stopped before role cycles:
`private/run-20260928/roles/20260928T121254.474813Z-cf473182`. P4 began with
G2 scanning and a pending ring connect. Its two `g2deinit` attempts returned the
same generic refusal after roughly 140 ms, including the cleanup attempt. The
wrapper recorded restoration as failed even though the later settings/readbacks
matched. This run is preserved; it did not qualify mode cycling or clocks.
`cmd_g2deinit` maps all internal shutdown refusals to “control owner did not stop”;
the elapsed time does not demonstrate the actual 6-second control-worker timeout.
The added normal cancellation step is a bounded retry strategy, not a production
fix or proof of the specific failure cause. A repeated refusal must remain a
blocked role test; investigate the internal debug reason rather than forcing
stack teardown. Separate clock-only checks remain possible without a role switch.

The bounded retry also stopped before any role cycle or encrypted BLE session:
`private/run-20260928/roles/20260928T121929.793682Z-b2c6d128`. After saved
auto-reconnect flags were disabled, `ringdisconnect` and `closeg2` brought the
G2 status to **idle, left down, right down**. Ring status continued to report
`connected=false, connectPending=true` throughout both the initial 12-second
cancellation wait and the separate cleanup wait. Thus the wrapper correctly
refused to proceed to `g2deinit`, Server startup or its diagnostic central. The
65 ring status responses in the P4 log all retained `connectPending=true`; this
is evidence for the observed bounded interval, not a claim that it can never
clear. Saved configuration restoration readbacks matched, but shutdown and
runtime restoration were not fully verified, so the run remains failed.

The next source investigation should follow **shared central-job cancellation
and completion**, with the admitted job kind, cancel generation, active/queued
job state and completion acknowledgement recorded. `ringstatus` publishes
`gRingConnectTaskActive` as `connectPending`; `g2RingDisconnect()` advances the
cancellation generation and requests scan cancellation, but deliberately does
not clear that in-flight ownership flag. `g2RingConnectMarkComplete()` clears it
when the shared `bleConnectWorkerLoop()` dispatch returns (and on submission
failure). `g2CancelActiveRingScan()` explicitly interrupts only the active
`RING_SCAN`/`RING_SCAN_ONLY` kinds; a saved-MAC job takes the `RING_SAVED` path,
whose connect waits and cancellation checks need separate tracing. A queued
cancelled ring job also cannot acknowledge completion until the shared worker
reaches it. These are candidate investigation points, not a demonstrated root
cause. Do not force-clear the flag or destroy the BLE host while its owner may
still be using it.

No further role retry or production fix was made in this phase. This current
boot/configuration blocks **new role-transition qualification**; it does not
invalidate the prior successful P4/S3 encrypted BLE or G2/R1 tests. Independent
clock and JPEG checks do not require forcing this transition. Both failed run
directories are preserved unchanged; public outcome records and final original
flash restoration are owned by the coordinator.

## Endurance and Android feasibility

The existing board-to-board BLE/HTTP fixtures avoid Mac Bluetooth and Wi-Fi
configuration. `p4_connectivity/test_ble.py::run_probe()` reuses the coordinator's
authenticated console, checks encrypted Secure Channel login/denial/reconnect,
MTU and reply counters. `board_control.py` also exposes mesh and HTTP actions.
The prior clean-role result is [FINAL.md](../p4_ble_roles/results/2026-09-27/FINAL.md),
which explicitly excludes Android UI and long-duration endurance.

Start a bounded soak (for example 30 minutes), recording duration and completed
work rather than only elapsed uptime. Combine authenticated HTTP reads,
bidirectional mesh payloads with exact receipt validation, and sequential JPEG
conversions through the proposed full-app hook. Compare warmed-up internal free
heap/largest block, task stack high-water values, latency, drops and payload/pixel
hash failures; scan logs for watchdog/assert/panic/reset. Sample after queues
drain so in-flight buffers are not mistaken for leaks. Separate no-accessory
Client/Server cycles from accessory-connected cycles, and exclude the diagnostic
BLE central whenever G2/R1 own scanning/connections. Restore all initial modes,
clocks and changed settings, then remove only this run's fixture files.

Actual Android testing still needs the phone/app. No APK/Gradle Android project
was found in this checkout and `adb` was not available on this shell PATH in this
audit. The board-radio fixture validates the protocol, not Android permissions,
discovery UI, credential entry, app lifecycle or reconnect presentation. With a
phone available, run Server mode, connect through the real app, verify protected
access before login and commands after login, disconnect/reconnect, then a full
Client → Server → Client cycle. Record the app/device version and user-visible
outcomes separately. Do not reuse G2/R1 Client mode concurrently with Server.

This phase changed only investigation documentation/tools and isolated
application copies; production sources were not edited. The source-audit agent
did not access devices. The coordinator performed the physical checks recorded
in [RESULTS.md](RESULTS.md), then restored and verified both original flash
images and configurations as recorded in [restoration.json](restoration.json).
