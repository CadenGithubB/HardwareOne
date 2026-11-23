# HardwareOne host tests

## Expanding OLED Health and graphs

`python3 components/hardwareone/test/host/test_oled_health.py` executes the
shipping health page, list renderer and graph snapshot with ASan/UBSan at
64/88/128 pixels. It checks P4 list capacity, OLED scrolling, graph bounds,
empty/offline series, owned chronological snapshots, rollover, guest navigation,
poll gating and nested Back behavior. `--preview-dir <path>` writes synthetic
PGM render previews. See [OLED Health](../../../../docs/OLED_HEALTH.md).

## OLED list text bounds and scrolling

`python3 components/hardwareone/test/host/test_oled_text_field.py` executes the
shipping bounded text field (`OLED_TextFieldCore.h` directly, the drawing side
spliced from `OLED_TextField.cpp` over `oled_text_field_gfx_double.h`, a double
of the vendored GFX surface) at 64/88/128 pixels with ASan/UBSan: no pixel
outside a field or the content area, exact-fit and one-glyph-too-long text,
ellipsis degradation down to one dot, opaque inverse cells, the 1000 ms /
20 px/s / 1500 ms marquee on elapsed time, continuity across list rebuilds,
resets on selection/label/width/owner/mode changes, view and modal pauses,
clock rollover and hostile text. `test_oled_layout.py` adds overflow cases
through the shipping compact, split-pane and two-line renderers; the health
and logging-UI suites run those renderers through the same primitive
(`oled_text_field_op_double.h` for op-recording canvases). See
[OLED list text](../../../../docs/OLED_LIST_TEXT.md).

## OLED header status bar

`python3 components/hardwareone/test/host/test_oled_header.py` compiles the
shipping `OLED_HeaderCore.h` unchanged over a GFX double with the shipping
`glcdfont.c` (ASan/UBSan). It checks the layout rules for every combination of
Power, Notify, Rec, Busy and Context widths and title lengths 0-24 (slots
packed 3 px apart inside 0-127, only Context drops, a dropped item never
outranks a shown one, nothing moves and the title never grows when Busy comes),
the title cut (one-cell `…` mark, `KeepPrefix` for paths, the shared
sanitizer), the Power text from the shared battery backend (hidden on a board
with no battery measurement), the staleness signature, that every lit pixel in
rows 1-8 belongs to exactly one region, and seven golden frames byte for byte.
The on-screen keyboard's top line (the core's second profile: prompt, then
Mode, Rec, Busy from the right) gets the same checks and four golden frames.
The band guard scans the in-frame OLED painters for a draw at a literal y of
0-9 (literal coordinates only; computed ones are caught on the device by the
`DEBUG_DISPLAY` band check), and the wiring of the capture dot, the keyboard
poll and `oleddump` is checked in the sources.
The `oledIsDirty` side of the staleness poll is in `test_millis_deadlines.py`.

## OLED rotation, portrait layout and input mounts

`oled_rotation_core_tests` (CTest; `test_oled_rotation_core.cpp`) compiles
`OLED_Layout.h` and `Input_RotationCore.h` unchanged (`-Wall -Wextra -Werror
-pedantic`). It checks the `oledrotation` degrees and quarter turns, the live
geometry for 128 x 64, 64 x 128 and the P4 canvases (portrait content y21-105,
footer rule y107, landscape unchanged), the glyph metrics and the hero card's
ink-centred columns, the ToF gauge fill, the portrait scrollbar against the
design's worked examples, the key/value stacking rule, the footer chips (hint
tokens to the four fixed A/B/X/Y slots, 3-letter verbs, arrow and START tokens
dropped), the portrait title cut (whole, leaf, path tail, 9 glyphs and the
mark), the keyboard's 30 keys re-wrapped 5 x 6, input directions that follow
the screen (joystick and direction bits at every rotation), the mount codes and
their argument forms, mount-then-screen for vectors and bits at all 8 mounts x
4 screens, that the teach step learns each mount back, and the portrait
header (`OLED_HeaderCore.h`'s tall profile: fixed status slots, title cut,
the rule on y19; the keyboard profile's page in the Power slot).
`input_imu_core_tests` checks the 8 IMU mounts on the tilt and the teach step
(each mount and screen rotation learned from one lean; no lean is a quiet
timeout; after a calibration only an offer: the stick works at once, a side tap
stays a button, the offer lapses quietly, a face tap starts the step, and a
shake cancels it with an event). `ano_encoder_core_tests` also checks that a
button held across a turn makes no press of its new direction, and
`test_oled_layout.py` that a list set up in portrait gets the landscape rows
after a turn back (selection kept in view). `ano_encoder_core_tests`
checks the ANO mount (180 = both old swaps, mirror = the LEFT/RIGHT swap and a
reversed wheel) and that the ring's roles follow the turned screen.
The harnesses that splice shipping screens draw the landscape canvas: their
portrait branches compile against a landscape double of the live layout
(`oled_landscape_stubs.py`, or the same lines inline in the harness).
`test_oled_layout.py` also packs a 64 x 128 map raster.

## OLED CLI Output and its console history

`python3 components/hardwareone/test/host/test_oled_console_buffer.py` executes
the shipping console owner (`OLED_ConsoleCore.h` directly, `OLED_ConsoleBuffer.cpp`
over mutex/allocator/clock doubles) with ASan/UBSan: checked sanitizer bounds
for escapes and malformed UTF-8, a model check of the byte-budget ring,
contiguous record IDs across duplicate timestamps, clock and ID wrap, loss and
truncation accounting, the identity-boundary delivery fence, and a producer
thread against a snapshotting reader thread.

`python3 components/hardwareone/test/host/test_oled_cli_output.py` drives the
shipping CLI Output page over that owner at 64/88/128 pixels (the P4X-EYE's
10/10 chrome and the 12/16 color chrome): Live/Paused/Reader, pause by ID,
arrival counts, expiry, the pinned Reader copy after eviction, multiline and
hostile text, busy-ring retry frames, dropped-output reporting and session
reset. Its framebuffer double rejects any draw outside the content area, any
row that would wrap, and any draw while the ring's lock is held.
`--preview-dir <path>` writes PGM renders. Both are registered as
`oled_console_buffer` and `oled_cli_output`. See
[OLED CLI Output](../../../../docs/OLED_CLI_OUTPUT.md).

## Shared OLED feedback

`python3 components/hardwareone/test/host/test_oled_toast.py` executes the
production queue and renderer with sanitizers at 64/88/128-pixel heights. It
checks error styling and eight-second lifetime, FIFO promotion, duplicate
suppression, error-preserving overflow, confirmation hiding and lifetime pause,
arrival while paused, clock rollover and clearing. Source guards connect the
pause to the dirty poll and clearing to display-session replacement.

`test_oled_command_feedback.py --sanitize`, `test_oled_remote_settings.py
--sanitize`, `test_oled_automation_wizard.py --sanitize`,
`test_oled_espnow_ownership.py --sanitize` and `test_oled_pair_prompt.py
--sanitize` cover shared results at their real callers. See [OLED feedback](../../../../docs/OLED_FEEDBACK.md).

## Cross-board power policy

`power_cpu_frequency` explicitly exercises classic ESP32, ESP32-S3 and both P4
PLL/revision branches with I2C enabled/disabled (eight variants).
`power_save_tick` runs the production idle/wake policy across those same target
branches, UART constraints and STT gates (240 fresh-process scenarios).
Both standalone Python runners accept `--sanitize`. Hardware/RTOS boundaries
are controlled: these suites do not establish real radio connection retention,
sleep residency or current draw. See the
[shared power-policy investigation](../../../../docs/CROSS_BOARD_POWER_POLICY.md)
for native-radio versus Hosted qualification requirements.

## Transferred Terminal and P4 regressions

`openclaw_terminal` runs the provider and device-API tests when Node.js is
available. Set `HW1_NODE_EXECUTABLE` during CMake configuration if Node is
outside PATH. The question projection regression covers both `ask_user` and
`openclaw:core:ask_user`, live events and replay, without hiding unrelated JSON.

The existing continuous-STT suite covers failed native questions after a
hard-cut prefix has already been decoded. It waits for the prefix before
advancing to the timeout, then checks that no partial text remains readable.
Continuous caption sessions still retain their completed text after failure.

The optional `esp_hosted_mempool` suite needs ESP-IDF **5.5.5** source plus the
pinned Hosted 2.12.13 package in the component-manager cache or
`managed_components`. Select the SDK explicitly; an older SDK is not a valid
substitute for this test of allocator internals:

```sh
cmake -S components/hardwareone/test/host -B /tmp/hw1-hardwareone-host \
  -DHW1_ESP_HOSTED_TEST_IDF="$IDF_PATH"
ctest --test-dir /tmp/hw1-hardwareone-host --output-on-failure \
  -R '^(openclaw_terminal|esp_hosted_mempool)$'
```

The DMA suite reproduces the original misalignment, checks the patched pool
across cache sizes and allocation fallback paths, and checks preparation hashes
and refusal to overwrite a modified dependency. `HW1_SANITIZE` controls its
ASan/UBSan instrumentation, as with other native host suites. A passing host
test does not qualify firmware or physical Wi-Fi/BLE behavior.

Host-side tests for firmware logic that is pure enough to compile off-target.
They compile the **real** headers from `components/hardwareone/`, not copies —
the same rule `updater/test/host/CMakeLists.txt` states for the recovery
throttle. `System_LLMUtf8.h` is kept dependency-free (no Arduino, no ESP-IDF, no
project headers) precisely so this is possible; the same boundary applies to
`System_MapViewportCore.h`. If either gains a firmware-only include, its host
target stops building and the coverage silently disappears.

Run from the repository root:

```sh
cmake -S components/hardwareone/test/host -B /tmp/hw1-hardwareone-host
cmake --build /tmp/hw1-hardwareone-host
ctest --test-dir /tmp/hw1-hardwareone-host --output-on-failure
```

Node.js is required for the four browser suites (`map_live_browser`,
`g2_status_browser`, `sensor_mic_card`, `logging_system_config_browser`). Set `HW1_NODE_EXECUTABLE` at configure
time if it is outside PATH. Without it they stay registered but are reported as
not run (disabled), so a green run then covers the remaining suites only.

The Python tool suites run in ctest as well: `tools_ota_unittests`,
`tools_webui_unittests` and `tools_model_unittests` each run
`python3 -m unittest discover -s tools/<suite>/tests -t .` from the repository
root. The web UI page harnesses use the first JavaScript engine they find
(`tools/webui/js_engine.py`); with `HW1_NODE_EXECUTABLE` set, that is Node.js.

## Map live recording

`map_live_tracking` executes the production Map owner, argument parser and
logger session-identity helpers with GPS enabled and disabled. GPS queueing,
logger file startup, allocation and VFS are controlled boundaries. It covers
startup refusals, retained imported tracks, checked child results, unchanged
boot preferences, same-path logger replacement, live clear/load refusal and
publication order when restarting after an external stop. Real threaded readers
exercise append/load/clear with owned snapshots; allocation and parse failures
preserve the previous track. Save/render callbacks clear the owner while the
original copy remains usable, and boundary checks reject holding the track
state lock across filesystem, renderer or command work. Metadata-only loads
avoid a point-copy allocation. Live recording starts through the logger's
transient entry point with the literal path and interval, and ending it
restores the saved logger selection exactly once; the CLI load reply returns a
validation refusal unchanged (no second status prefix); renders reuse one
allocated point copy, draw nothing when that allocation is refused and allocate
nothing on later frames.

Checked GPS saves run the shared row writer and real filesystem guard over
controlled File/VFS and semaphore boundaries. Save cases cover unchanged CSV
bytes and timestamp names, a 10,000-point track, owner clear before/during I/O,
short header/row/newline writes, reported Print errors, post-flush size mismatch,
open/directory/promotion/cleanup failures, caller-owned locks and output-buffer
limits. A failed save keeps the prior destination, leaves the output path
unchanged and publishes zero saved points; successful replacement publishes
only after the temporary is checked and promoted. The optional
`--baseline <saved-component-directory>` compares deterministic normal-save
output with saved source and reproduces its former partial-write false success.
`--evidence <directory>` retains generated source and run output. File and
write-error behavior is simulated; these checks do not establish media or
power-loss durability.

`map_live_ui` executes the HTTP authentication/identity prefix, unified web
command helper, live/file/save responses, OLED Tracks action branch and status
drawing. Command submission, snapshots and transport sessions are boundary
doubles; tests check source identity, guest/session refusal, JSON errors and
display feedback. Replacing the owner during HTTP/OLED output cannot change
the captured counts, points or metadata. A Guest session is refused by the
mutation gate (a double of `webGuestMutationAllowed`) on recording control,
saving and loading before any owner or executor call, while the live snapshot
still streams. The file reply publishes through the status-only load and then
copies the published track once, so it describes one coherent snapshot; a
failed copy is a 503, never an empty success. Both compiled suites use
ASan/UBSan when `HW1_SANITIZE=ON`.

`map_live_browser` runs the actual Maps JavaScript functions in Node with
controlled requests, timers and DOM elements. It checks confirmed state changes,
failed-stop polling, duplicate clicks and stale responses. Node must be available
when configuring CMake for this test to run; without it the test is registered
but disabled.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^map_live_(tracking|ui|browser)$' --output-on-failure
# Separate run: ThreadSanitizer cannot be combined with ASan.
python3 components/hardwareone/test/host/test_map_live_tracking.py --thread-sanitize
```

These checks do not qualify physical GPS fixes, display layout, real filesystem
writes or device performance with near-capacity tracks. The GPS-start and
logger-settings suites also exercise the logger lifecycle helpers.

## Capture stitching and plaintext export

`capture_stitching` executes the production GPS and health merge commands,
`capturecrypt export`, shared copy/row helpers, argument/quoted-path parser,
sealed-header reader and actual `VFS::replaceFileGuarded` implementation with
real Arduino `String`. File operations, identity, locking and decryption are
controlled boundaries. Checks cover exact bytes and counts, empty/multi-chunk
inputs, command defaults, normalized source/output/staging aliases and mixed
sealed/plaintext refusal. Faults include incomplete reads, short body/newline
writes, reported Print errors, post-flush size mismatch, directories, failed
promotion and failed cleanup. A temporary left by an interrupted run is the
command's own leftover: the next run overwrites it and completes, with the
previous output still preserved until promotion.

Replacement cases exercise LittleFS rename-over-existing and SD's guarded
backup/promotion/rollback sequence. Failed rollback must retain the previous
bytes in `.bak`; unresolved backups are never consumed, and a backup that blocks a
replacement, survives a failed restoration or outlives a successful promotion is
reported to the system log (asserted here; clean outcomes log nothing). Plaintext export keeps
its schema lines and `[UNDECRYPTABLE]` policy, while detecting incomplete output
before promotion. Failed attempts preserve the previous destination or its
recovery backup and clean up only their own temporary.

```sh
python3 components/hardwareone/test/host/test_capture_stitching.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^capture_stitching$' --output-on-failure
```

The optional `--baseline <saved-component-directory>` compares normal saved
bytes and replies against saved source; intentional failure-handling changes
are checked separately. `--evidence <directory>` retains generated sources and
results. These tests do not qualify physical storage, encryption validity,
power-loss durability or the VFS permission engine.

## Capped system log writes

`capped_log_writes` executes the production capped append/rotation helper and
orphan recovery with real Arduino `String` and `Print`. Controlled File/VFS,
allocation and locking boundaries cover CRLF accounting, exact caps, the 85%
trim target, bounded newline scans, fragmented reads, overflow destinations,
reused PSRAM storage and the existing recovery policies. Faults cover short
body/newline appends, reported write/flush errors, premature reads, mismatched
sizes, allocation/open/seek failures and failed promotion. LittleFS replacement
must retain the original on failure; SD keeps its existing remove/rename orphan
protocol after the temporary copy has been checked. Boot recovery also checks
each log's `/sd` mirror whenever a card is mounted, so an orphan left by a
rotation that ran while overflow was latched on an earlier boot is promoted or
discarded like a primary one, and left alone without a card.

Batching (S-8): with the batch buffers set up, 20 lines within 2 s take no
filesystem lock and reach the file in one append when the log task's tick finds
the oldest 1.5 s old; 2 KB pending, a line that does not fit, a 2 s old batch
and an announced restart flush at once; login and crash-history lines are never
held; a refused batch counts one drop per line. Two generations: a rotation is
one rename (one remove more on the SD mirror) and copies nothing; a file older
firmware left above its cap is cut to its newest half once, and not before the
main loop runs; the three logs file catch-up follows keep the copy rotation in a
build that serves it. UP-03: the writer scales the caller's cap to the
partition. `log_caps` (`test_log_caps.py`) runs `System_LogCapsCore.h` for every
partition table and checks that the call sites' caps add up to the header's
total.

Lines of 0, 1, 255, 256, 257 and 302 bytes (a 47-character prefix and the
longest log text) are appended with and without a rotation on both tiers. The
log task's line, `appendTimestampedLine` (`System_Debug.cpp`), runs against the
statements it replaced (`String line = buildTimestampPrefix(); line +=
text;` and the String form of `appendLineWithCap`) for the wall-clock prefix,
the `[ms=...]` prefix before the clock is set and the longest prefix, text
lengths up to past its kept block, both tiers and with a rotation, first with
the PSRAM block refused: the files and every file call must be equal, the kept
block is asked for once, and a line that fits makes no String block. The
String is the core one with its heap calls counted. `--previous
<component-directory>` runs the same harness on an earlier tree and requires
an equal transcript. A source check pins that log_out's three capped-log lines
go through `appendTimestampedLine` and the command audit hands its buffer on
without a String.

```sh
python3 components/hardwareone/test/host/test_capped_log_writes.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^capped_log_writes$' --output-on-failure
```

The optional `--baseline <saved-component-directory>` compares normal file
contents and reproduces the former append false success, partial-copy promotion
and LittleFS original deletion. This is fault-injection coverage of observable
API results, not physical storage, SD qualification or power-loss durability.
Arduino File's void flush/close calls cannot expose every backend failure.

## Log level, system-log status and command origin

```sh
python3 components/hardwareone/test/host/test_log_level_application.py --sanitize
python3 components/hardwareone/test/host/test_system_log_status.py --sanitize
python3 components/hardwareone/test/host/test_command_feed.py --sanitize
python3 components/hardwareone/test/host/test_esp_log_overrides.py --sanitize
python3 components/hardwareone/test/host/test_radio_stack_logs.py --sanitize
```

`log_level_application` runs the shipping `applyLogLevel`, `cmd_loglevel`, the
boot clamp block from `applySettings()` and `setDebugSetting`, with DebugManager,
`esp_log_level_set` and the debug-settings persistence request recorded. It
checks the ERROR/WARN/INFO mapping (anything else is DEBUG), that exactly the
`wifi`, `wifi_init` and `phy_init` tags follow the level, every accepted
spelling, the persistence request before the apply, no request for an unchanged
value, refusals that change nothing, and the boot clamp into 0-3. The owner
then re-applies the saved `esplog` tag levels (recorded here).

`esp_log_overrides` runs the shipping `System_EspLogCore.h` and the
`applyEspLogOverrides`/`cmd_esplog` block of `System_Debug.cpp` with
`esp_log_level_set` and the persistence request recorded: level names, tag
validation (the C6 mirror's `C6>` included), set/replace in place/remove, the
wildcard first, the 16-entry and 240-character limits, the editors' whole
`tag:L,tag:L` string, malformed stored entries skipped, `HTTP_CLIENT` capped at
warn, the wildcard applied only at boot and by the command, and refusals that
change nothing. Its source checks pin the wiring and the rest of the
2026-11-18 field-log fixes: the boot apply after the firmware's own levels,
heartbeat RSSI asked only while joined, the `BLE_COEX` scan lines following
their tag's level, the G2 scans' skipped stop after a scan that ended itself,
`rpc_utils` among the quiet Hosted tags and `c6console` persisting (off by
default; the fault service follows the saved setting).

`companion_fault_mirror` compiles the shipping fault service, mirror procedure,
dispatch, boot init and `c6console` of `System_RadioCompanion.cpp` against the
real `Radio_CompanionCore.h`: the mirror starts on a fault ahead of the
recovery, stops after ten healthy minutes, is started and stopped only on the
command executor, and the lifetime fault counts are plain assignments saved from
the executor, never in boot or before a reboot.

`portable_web_on_demand` resolves `configurations/p4-portable.json` and checks
that Wi-Fi and the HTTP server are compiled in and that `openwifi` and
`openhttp` are registered and refuse only for the three stated reasons.

`stt_phrase_level_default` checks that the within-phrase leveler is off in both
places that hold its default (the struct initialiser and the `sttphraselevel`
settings row, which is the one a boot applies), that the row, the editor line
and the admin command sit in the speech `#if` block, and that the two readers in
`System_STT.cpp` still gate it on the setting and on that microphone's automatic
gain.

`radio_stack_logs` runs the shipping `System_RadioLogCore.h`, the esplog block
and `applyRadioStackLogLevels` of `System_Debug.cpp` and HAL_Radio's
`quietHostedLogs` against a model of ESP-IDF's tag levels (a default, per-tag
levels, `*` dropping them): every radio stack tag of the build at ERROR by
default; `debugwifidriver` bringing ESP-Hosted's tags and `C6>` back to their
stock level, `debugbluetoothcore` Bluedroid's, `BLE_COEX` and `C6>`; a saved
esplog level winning over both and a saved wildcard taking the quiet default's
place, each removal taking effect at once; the boot order and a new Hosted
session (esp_hosted_init's WARN) ending on the same levels; and the C6
console's line levels (the C6's own prefix, crash reports as errors, among
them a failed `ESP_ERROR_CHECK`, heap corruption and a task watchdog's dump, a
split line keeping its level); and the esplog lock taken once per save and per
apply, never nested, with debug.json written outside it. Its source checks pin
the flag readers and every caller (the paths that replace the whole debug mask
included), HAL's quiet start, the `BLE_COEX` lines following their tag, the help
text, and that every tag in the table is one this build's sources log with.

`system_log_status` runs `formatSystemLogStatus`, `formatSystemLogStatusJson`,
`systemLogState`, both `formatSystemLogFlags`
overloads, `parseSystemLogFlags`, `systemLogApplyPersistedFlags` and
`systemLogFollowFlagChange`. It reads the CLI, web-endpoint and flag-message
buffer sizes from their callers, renders the three logging states as text and
as json (`formatSystemLogStatusJson`, the same object in the CLI buffer and the
endpoint's 1024-byte buffer, with the state key and the OLED label), checks
that a long path cuts the text at a small caller's size while the json is
either whole or the `response too large` object, to the byte at the edge, and
checks null and zero-size outputs, the 70-byte minimum
for the 69-character mask, a format/parse round trip of all 256 single-bit masks
in both accepted forms, the persisted-mask restore (empty or prefix-only text
keeps the live flags; non-hex text currently parses as zero), and the saved mask
following a persistent per-flag change (only the touched bits are copied, a
recomputed parent travels with them, nothing is written without a saved mask).

`command_feed` runs `originPrefix` and `appendCommandToFeed` from
`HardwareOne.cpp`, with the CM5/live-audio classifiers, audit redaction and the
web mirror recorded. Replies carry `[source user@ip] ` and the feed
`[source user@ip] $ `, including the source-only, user-only and IP-only forms.
For long identities both truncate inside the same 128-byte buffer, so the feed
marker is cut with the prefix instead of being appended after it. Machine polls
never reach the feed. None of these three suites touches a device, a file or the
network.

## File create over an existing name

`filecreate` compiles the production `cmd_filecreate` handler against a
name-to-content VFS double whose `"w"` open creates or truncates exactly as
`VFS::open` does. A create that reuses an existing name must reply with an
error, leave the content intact and never reach the truncating open; a new
name is still created once and closed under the FS lock; denied opens and the
early argument errors keep their established replies and touch no file.
Permission rules, the real tokenizer and the storage drivers are not covered.

```sh
python3 components/hardwareone/test/host/test_filecreate.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host -R '^filecreate$' --output-on-failure
```

## Boot-time crash counters and the OTA crash-loop escape

```sh
python3 components/hardwareone/test/host/test_crash_record_boot.py --sanitize
```

`crash_record_boot` runs the shipping RTC layout, `crashRecordBootConsume`, the
counter getters, `crashRecordMarkBootHealthy`, `crashRecordConsumeCrashLoopEscape`
and `crashRecordDetail` from `System_CrashRecord.cpp`, plus the escape decision
(`crashLoopEscapeWanted`, `restartIntoCrashLoopRecovery`, `kCrashLoopEscapeCount`)
from `System_OTA.cpp`. RTC_NOINIT storage is process memory, so a sequence of
`crashRecordBootConsume` calls models resets that keep RTC memory; the build id is
settable and `esp_restart` unwinds. It checks that three brownouts leave the
software-fault count at 0 and never escape while the full count and the
`crashlog` text still report them, that three panic or watchdog resets reach 3
and escape, that a mixed sequence counts only the software faults, that the
software reset after a committed escape (an operator cancel in the updater)
finds both counters consumed and does not escape again while a fresh three still
does, that an unknown previous phase or a different build blocks the escape, and
that a healthy run, a power-on reset and a cold RTC block clear both counters.
It does not run the partition, journal or updater side of the escape, and
touches no device.

## Shared HTTP request-body reader

`web_body_reader` compiles `webRecvRequestBody`, `webReadRequestBody` and
`webSendBodyReadError` from `WebServer_Utils.cpp` (their types from
`WebServer_Utils.h`) plus the `readPostBody` wrapper from `WebPage_LLM.cpp`,
against a scripted httpd socket and allocator. It checks that a Content-Length
of 0 is reported as empty, that a length over the caller's cap is refused before
anything is allocated or received, that a failed allocation is reported instead
of received into, that partial receives are joined and NUL-terminated, that one
idle timeout is retried while two in a row end the read with the buffer released,
that a socket error or short body is a failure and never a truncated success,
and that the reply helper maps empty to 400, over-cap and no-memory to 413 and
a failed receive to 400 (text or the JSON `success`/`error` contract), returning
`ESP_FAIL` where unread bytes remain so httpd closes the socket. The LLM wrapper
must go through the shared reader and give up after two timeouts. The
`web_batch_handlers` suite covers the same reader inside `handleCliCommand`
(forged Content-Length, failed allocation, short and stalled bodies, the
`CMD_INPUT_MAX`-derived cap). Neither runs ESP-IDF's httpd or touches a device.

    ctest --test-dir <build> -R '^web_body_reader$' --output-on-failure

## Transport session writers

```sh
python3 components/hardwareone/test/host/test_session_snapshot.py --sanitize
```

`session_snapshot` extracts the boot-local transport-session namespace and the
serial/display lifecycle functions from `System_User.cpp` unchanged and runs
them against stand-ins for Arduino `String`, the critical sections and the two
lifecycle writers. Before compiling, it checks the source contract that the
writers are FreeRTOS mutex handles created with `xSemaphoreCreateMutexStatic`
(priority inheritance for a higher-priority waiter such as the httpd task) and
not `std::atomic_flag` spin loops, and that no writer window in
`System_User.cpp`, `HardwareOne.cpp` or `OLED_Utils.cpp` (a Begin/End pair,
or the scope of a `NamedSourceUseGuard`) contains console output,
`displayUpdate`, the auth-log append, a roster read, the output-drain wait or
the shared mirror broadcast. In `OLED_Utils.cpp` a window may not copy a
command result either. A window runs from the Begin call to the end of its
enclosing block, so a writer that is released early must sit in its own
block or helper.
The harness then exercises snapshots, lazy bypass creation, a full registry,
policy and identity changes while waiting for a held writer, the Begin/End
delivery windows (held only for the live epoch, a contended Begin waits for
the holder) and `serialTransportSessionClearIfEpoch`, which returns with the
writer released. The mutex stand-in fails a take on a writer the harness
already holds and a give without a take. The behaviour of `OLED_Utils.cpp`'s
windows is covered by `oled_commit_window`. Task priorities and the device
are not covered. `--baseline` compares the transcript against a saved
`System_User.cpp` that already uses the mutex writers. `--component <dir>`
reads the production files from another tree.

    ctest --test-dir <build> -R '^session_snapshot$' --output-on-failure

## Web file editor save through the staged writer

`upload_lifecycle` (above) also compiles the production `handleFileWrite`
(`/api/files/write`, the Files editor save), the shared staged writer
`VFS::StagedWrite` and the form decoders `urlDecode` / `extractFormField`
over the same fakes as the upload. Its `filewrite` scenarios post the
encodeURIComponent form the editor sends and require the name and the content
`a=b&c+d é<TAB>#%` to round-trip byte-exact; a literal `+` still decodes as a
space. The save goes to a sibling `.upload.tmp` opened under the caller's
identity and is promoted with `replaceFileGuarded`: an injected short write or
a failed promotion must leave the previous bytes in place and remove only the
staged copy (caller first, then the `web.filewrite` system identity), the
destination gate (`canWriteGuarded`, the admin-only 403) runs before any
temporary is opened, a name too long for the `.upload.tmp` suffix stages as
`~stage.upload.tmp` (64- and 200-byte names included), a refused staged copy
answers "Could not save: storage full or unavailable" with the destination
never opened, and post-save hooks run once on success only. The fake
`replaceFileGuarded` demands what `renameGuarded` does of a caller (RENAME,
CREATE, and DELETE for the backup of an existing SD file), so the scenarios
also fix the promotion identity: a caller with those bits promotes as itself;
a caller holding only the destination's write right (WRITE on an existing
file, CREATE on a new one, the `/espnow/`, `/system/llm/` and
`/sd/g2_icon_animations/` grants) is promoted by the writer's system identity,
and a caller with neither keeps the previous file; the `replace` group checks
the same for the upload. Not covered: the real rule table itself (the bits are
controls) and LittleFS/FAT behaviour behind the fakes.
Run with `ctest --test-dir build -R '^upload_lifecycle$' --output-on-failure`
or `python3 test_upload_lifecycle.py --case filewrite --sanitize`.

## Shared fake VFS

`fake_vfs.h` is the one in-memory filesystem double that new persistence suites
build on instead of carrying their own File/VFS/stat/lock stand-ins. It models
the production routing rule (`/sd` is the SD card, everything else LittleFS)
with both roots present, directories, and a `File` handle with the Arduino
FS/Stream/Print method names production code calls (`read`, `write`, `print`,
`printf`, `readBytes`, `readString[Until]`, `seek`, `position`, `size`,
`available`, `flush`, `close`, `getWriteError`, `name`, `path`, `isDirectory`,
`openNextFile`, `rewindDirectory`, `getLastWrite`). Copies of a `File` share one
handle and a handle dropped without `close()` closes implicitly and is counted.
It is not a model of power loss, wear or the real allocators.

Knobs live on `fake_vfs::fs()`: `policy` (directory unlink refused like LittleFS
or allowed; rename over an existing file on LittleFS yes and on `/sd` no, each
switchable; an optional 63-byte LittleFS name limit; whether `create=true` makes
parents; leaf or backend-rooted `name()`), `faults` (open, mkdir, rmdir, remove
and rename refusals by path or `"*"`, a short write after N bytes per file, the
write-error flag, a failing flush, a `stat` errno), `capacity[]` (SD or LittleFS
unavailable, SD read-only, totals and a `getStats` failure), `lockHeld` (the
predicate behind the guarded-versus-unguarded seam) and `now` for modified
times. Every operation lands in `log` with its path, mode or destination,
result, whether a `*Guarded` forwarder was used, whether the lock was held and
the `systemAuth` user, reason and scope; `count`, `last`, `ops`,
`lockedCalls`/`unlockedCalls` and `guardedCalls`/`unguardedCalls` query it.
Setup helpers (`mkdirAll`, `put`, `text`, `list`, `exists`) are not logged.

A suite opts in by including the header after the `web_batch_stubs` String
double, defining `FAKE_VFS_PROVIDE_VFS_NAMESPACE` for the production `VFS::`
signatures as thin forwarders (`openGuarded`, `open`, `exists[Guarded]`,
`remove[Guarded]`, `rename[Guarded]`, `replaceFileGuarded`, `mkdir[Guarded]`,
`rmdir[Guarded]`, `isSdAvailable`, `getStats`, `normalize`, `systemAuth` and the
rest) and `FAKE_VFS_LOCK_DOUBLE` for the semaphore double the real `FsLockGuard`
compiles against. `FAKE_VFS_EXTERNAL_AUTH_CONTEXT` keeps the suite's extracted
real `AuthContext`; `#define stat FakeVfsStat` around an extracted block supplies
the POSIX stat seam for `/littlefs/...` and `/sd/...` physical paths. The header
comment carries a usage example. The existing persistence suites keep their
private doubles until they are migrated; no suite was changed for this.

`fake_vfs` is the self-test: it exercises every knob and fault above, the lock
seam with the real `FsLockGuard`, the SD `.bak` sequence of `replaceFileGuarded`,
and compiles the production `writeText`/`writeTextAtomic` against the fake,
checking that a short write, write error, flush fault, failed rename or refused
open preserves the previous file and removes the temporary. Two further units
compile the header bare and with the real `AuthContext`/`CommandSource`.

```sh
python3 components/hardwareone/test/host/test_fake_vfs.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host -R '^fake_vfs$' --output-on-failure
```

## Web file streaming lock discipline

```sh
python3 components/hardwareone/test/host/test_web_file_streaming.py --sanitize
```

`web_file_streaming` compiles the shipping `streamFileChunks` helper,
`handleFileRead`, `streamHtmlEscapedChecked` and the GPS track listing tail of
`handleGpsTracksApi` against an httpd double that records whether the
filesystem mutex or a sensor poll pause was held inside every chunk send, and
that fails one chosen send. It checks that a 1300-byte download arrives in
512-byte chunks with one terminator and the file closed, that the lock is taken
per read and released before each send, that a failed send ends the loop with
no further chunk and no terminator while the file is still closed, that the
missing-file and refused paths leave polling balanced, that the HTML-escaped
viewer path escapes correctly and also stops at the first failed send (including
a run longer than its staging buffer), and that the GPS listing emits its entries
with the lock released around each send and stops scanning after a failure.
`handleFileView` is checked at source level: no function-scope lock on the
non-JSON path, the image, chunked-audio, `.hwmap` and text branches all stream
through the helper, sealed-capture lines use the checked escaper, and polling
resumes exactly once before the first response byte. The File/VFS stand-in is
harness-local and tiny (this is a lock-discipline suite, not a persistence one);
the upload path, authentication and the JSON viewer branch are not covered here.
On a tree without the helper and the checked escaper the runner compiles the
handlers alone, so the lock and stop-on-error checks fail behaviourally rather
than the unit failing to build; the source-level checks run after the binary.
Run with `ctest --test-dir build -R '^web_file_streaming$' --output-on-failure`.

## Wi-Fi connect hunt budget

```sh
python3 components/hardwareone/test/host/test_wifi_connect_budget.py --sanitize
```

`wifi_connect_budget` runs the shipping `connectToBestWifiNetwork`,
`connectWifiId`, `connectWifiTarget` and the bounded attempt function from
`System_WiFi.cpp`, plus the hunt's bounds helpers and `wifiBootConnectBudgetMs`,
against a scripted driver and a virtual clock (`millis()` reads a counter that
`delay()` advances). Each saved network is scripted as missing (the driver
reports `WL_NO_SSID_AVAIL` after its scan), associating (stays
`WL_DISCONNECTED`) or joining after a delay; the serial console and the command
context are doubles. It checks that eight missing networks finish inside the
hunt budget with every attempt ending after the NO_SSID grace rather than its
35 s window, that an associating link keeps the whole 35 s window per attempt,
that the deadline stops the hunt (the running attempt ends, later networks are
reported as not tried), that a reachable network after a missing one is still
joined, that a console keystroke cancels only a boot or serial-console request
(web, companion-restore and other origins leave the byte to the main loop), and
that the boot budget is the default on a verified image, the remaining
probation setup window minus the reserve on a trial boot, and zero once that is
gone. The driver's IDLE-bug recovery, scan coordination, NTP and the OTA
supervisor itself are outside this suite. On a tree without the bounded hunt the
old entry points compile behind the same adapters, so the budget, grace and
console-origin checks fail behaviourally. The radio-ownership cases run a
targeted connect and the hunt twice: with ESP-NOW running, and with a timed-out
ESP-NOW close still pending (`espnowOwnsRadio`). Neither may stop the driver,
switch it to STA or drop AP_STA. With nothing holding the radio, the connect
still restarts the driver in STA. `--component <dir>` reads another tree; the
pre-fix sources fail the pending-close case.

The background boot connect (`WifiConnectOrigin::BACKGROUND`, run by the boot
network task while setup carries on) is covered too, with `setupWifi` itself
compiled in. It never reads the console and leaves NTP to the boot sequence
(a command-origin hunt still syncs). A superseding radio command stops it
within one status poll, during a retry backoff or in the pause between saved
networks, with no later network tried and no connect failure reported; a
command's own hunt ignores the request. The reservation made before the boot
task starts stays active for the whole hunt and is released on every way out,
including the driver-init early return; a request made between the
reservation and the task reaching `setupWifi` stops the connect before
anything is initialized, and a superseded connect is reported as stopped, not
timed out. `wifiSupersedeBackgroundConnect` returns at once when nothing runs,
waits for the release, and gives up after its bound. Source checks confirm
that `openwifi`, `closewifi`, `wifidisconnect` and `radiopower` supersede it
and report a timeout, that `radiopower off` does so before deciding the radio
is already off, that `openwifi` checks its arguments first, that the radio
companion stops it before stopping or reusing the transport, and that
`openespnow` refuses to start beneath it. The boot hand-off in
`HardwareOne.cpp` (which side starts HTTP, MQTT and ESP-NOW) is not compiled
here. Run with
`ctest --test-dir build -R '^wifi_connect_budget$' --output-on-failure`.

## users.json and pending-list writers

```sh
python3 components/hardwareone/test/host/test_user_store_mutations.py --sanitize
```

`user_store_mutations` runs the shipping roster writers from `System_User.cpp`
on the shared fake VFS (`fake_vfs.h`) with vendored ArduinoJson: ban and unban,
promote and demote, delete, `updateUserLastSeen`, `resolvePendingUserCreationTimes`,
`adminCreateUser` (useradd), `approvePendingUserInternal`, `denyPendingUserInternal`
and `cmd_user_request`, together with the real `FsLockGuard`, `readText`,
`writeTextAtomic` and `getUserSettingsPath`. Hashing (a double that never contains
its input), sessions, events, broadcasts and the per-user settings writer are
doubles. It checks that ban reasons holding `}` and `]` leave a file that still
parses after that account and a later one are deleted, promoted and demoted, with
the other records intact and the founder protected; that a role-less legacy entry
gains its role field; that a roster above 10 KB whose early accounts wait for a
creation time is resolved whole and written complete, and that an account past
8 KB is resolved; that useradd writes the credential before the roster entry,
publishes nothing on a refused credential or roster write (the credential is
withdrawn), stores only the hash and refuses reserved names; that a roster without
`nextId`, or with a lagging one, gives the new account an unused id and leaves
every other settings file untouched; that approve keeps the request and publishes
nothing when the credential or roster write fails, retires the request only after
the account is live, reports a failed pending-list rewrite instead of hiding it
(the account exists, a retry is refused as a duplicate), and holds the filesystem
lock across its whole read-modify-write, so a ban that another task commits while
approve runs (modelled with `fs.onCall`) survives; that deny and request write the
pending list through `writeTextAtomic` under the lock; and that an allocation
failure never publishes a truncated roster. Source-level checks then require every
users.json writer to go through `mutateUsersJson`, both creators to go through
`createAccount`, and the pending-list writers to hold the lock. Not covered: the
web and CLI adapters' rank checks and messages, the ESP-NOW USER_SYNC receiver,
and the real per-user settings writer (`settings_file_writes`). On a tree whose
mutators still edit users.json as text the same calls build (with
`-Wno-sign-compare` for those mutators only) and the delete case fails on the
corrupt file. Run with
`ctest --test-dir build -R '^user_store_mutations$' --output-on-failure`.

## Time-anchor registry writes

```sh
python3 components/hardwareone/test/host/test_time_anchor_persistence.py --sanitize
```

`time_anchor_persistence` runs the module body of `System_TimeAnchors.cpp`
(registry load, append, prune rewrite, promote sweep and `timeAnchorsTick`) on
the shared fake VFS (`fake_vfs.h`) with the real `FsLockGuard`, `writeText` and
`writeTextAtomic`; the clock, boot counter and sensor-logging state are doubles,
and each case runs in a fresh process. It checks that a prune rewrite whose
temporary file cannot be opened, comes out short, reports a write error or
cannot be renamed leaves `.anchors.csv` byte-for-byte unchanged with no temporary
left behind, and that the row stays in RAM so a later successful prune still
writes it; that an append that cannot open the registry, comes out short or
reports a write error leaves the boot unanchored and a later tick records the
row (warning once); that a fragment left by an interrupted append, this boot's or
an earlier one's, is replaced by a rewrite instead of having the next row merged
into it, or closed off as its own line when the registry already holds
unparseable rows; that the sweep re-arms when a late anchor lands so this boot's
folder still promotes; and that a working prune drops the row. Source checks
require no unchecked rename of the registry and an anchor latch that follows the
append result. Not covered: power loss, LittleFS durability, the clock sources
and the 32-row cap. On a tree with the unchecked writers the same harness builds
and the cases fail behaviourally. The same harness runs log retention through
`timeAnchorsTick`: the card's free space is read at the hourly decision, a
decision without a usable figure is retried after one minute, and the directory
is listed by name (`getNextFileName`), opening only the file it removes. Run with
`ctest --test-dir build -R '^time_anchor_persistence$' --output-on-failure`.

`log_retention` compiles `System_LogRetentionCore.h` as it is and runs it on an
in-memory volume: the water marks and the low/high latch; secondary files of the
oldest day first, one day a pass; the three newest stored days, every
transcript and note (B12), the open session, today's folder, dot, `.tmp` and
`.bak` files and everything outside the capture tree never removed; health
files of the fourteen newest stored days kept; earlier unanchored boot folders
before the health round; the "recent" day horizon at least the catch-up
ledger's (read from `System_CatchupLedger.cpp`);
and the per-step bounds on listing and removal. Run with
`ctest --test-dir build -R '^log_retention$' --output-on-failure`.

## C6 companion reboot escalation

```sh
python3 components/hardwareone/test/host/test_companion_reboot_contract.py
```

`companion_reboot_contract` is a source check on `System_RadioCompanion.cpp`: no
direct `esp_restart()` or `ESP.restart()`, and the `HardReboot` dispatch case
posts its companion event and C6 log line, sets the RTC marker, then restarts
through `rebootDevice("c6-companion", detail, 750)`, the shared owner that
finalizes a microphone recording, records the reason and flushes the event log.
The restart itself, the WAV header after an escalation during a recording and
the next boot's REBOOT reason are device checks (P4, fault injection). Run with
`ctest --test-dir build -R '^companion_reboot_contract$' --output-on-failure`.

## OTA probation supervisor

```sh
python3 components/hardwareone/test/host/test_ota_probation_supervisor.py --sanitize
```

`ota_probation_supervisor` runs the shipping `probationSupervisor`,
`otaSafetyAcceptProvisioningBoot`, `otaSafetySetupReachedRunning` and
`otaSafetyLoopHeartbeat` from `System_OTASafety.cpp` against a settable tick,
one supervisor pass at a time. The journal check and the otadata write are
fakes that advance the clock and run supervisor passes inside their window.
It covers a provisioning accept 3 s, 45 s and 290 s after boot with no reboot,
a 31 s hang inside the accept still diagnosed as a loop stall, a failed
otadata write returning to Setup under the setup deadline, a Running stall,
and a healthy minute marking the image valid. Not covered: the real FreeRTOS
scheduler, the second core, the bootloader rollback and the RTC breadcrumb.
Run with `ctest --test-dir build -R '^ota_probation_supervisor$' --output-on-failure`.

## OTA journal policy

```sh
python3 components/hardwareone/test/host/test_ota_journal_policy.py --sanitize
```

`ota_journal_policy` first checks the sources: neither `System_OTA.cpp` nor
`updater/main/updater_main.c` spells out the IDLE-or-terminal phase test
(`hw1_ota_phase_is_idle_or_terminal` in hw1_ota_protocol owns it); the
crash-loop escape and the storage-failure path both go through
`armEmergencyRecovery`; the updater's recovery upload takes its flags from
`hw1_ota_replacement_request_flags` and its allow-downgrade action asks
`hw1_ota_phase_accepts_downgrade_permission`; the BLE-resume-after-OTA latch
that nothing set is gone. It then runs `armEmergencyRecovery` and the boot
helpers it uses, unchanged from `System_OTA.cpp`, on the real
`hw1_ota_protocol.c` with a fake slot store, factory check and boot switch:
armed and requested intents replay, a failed switch is journaled as
BOOT_SWITCH_ERROR, a failed or staged record is replaced without its policy
flags, an unusable factory image leaves the journal untouched and a live
operation is refused. Not covered: NVS slots, `esp_image_verify` and the
restart. The protocol rules themselves are in
`components/hw1_ota_protocol/test/host`. Run with
`ctest --test-dir build -R '^ota_journal_policy$' --output-on-failure`.

## Credential attempt table

`credential_attempts` compiles the brute-force lockout table from
`System_User.cpp` (the `credential_attempts` namespace, the
`credentialAttempt*` functions and `checkCredentialAttempt`) and its
`System_User.h` contract unchanged, with a fake clock, critical section,
diagnostics, event register and a counting `isValidUser`.

Scenarios:

- tiers 5/10/20 are all reachable by a peer that waits out each lock;
- keys are transport plus peer;
- a success clears only failures that named the same account;
- the window ending never cancels a running lock;
- a locked entry is never evicted;
- a pool full of locks refuses new peers while serial, UART, the display and
  MQTT keep reserved slots;
- Bluetooth (`ble`) and ESP-NOW (`espnow`) each count under one reserved key.
  A full web pool cannot refuse them, and a changed peer label cannot split
  them;
- `checkCredentialAttempt` refuses a locked key without validating, then
  records the outcome and wipes its copy of every password it validated;
- the lock event and since-boot total;
- a lock across the 32-bit `millis()` wrap;
- ended locks stay ended through weeks of idle.

Source contracts:

- the table keeps time on the 64-bit `esp_timer_get_time()` clock (no
  `millis()`, no signed-difference checks) and is not under
  `ENABLE_HTTP_SERVER`;
- these consult it before validating: `/login`, Basic Auth, serial, UART,
  MQTT, the targeted `login`, a bare `login` (through `loginTransport`), the
  BLE login job, the OLED login screen and set-pattern re-authentication, and
  ESP-NOW's password remote commands and USER_SYNC admin check;
- the BLE job checks after its session check and records success only after
  the bind; the BLE command tail and the ESP-NOW bond-token path stay outside
  the table;
- Basic Auth failures are audited and evented like `/login`; MQTT failures
  write the `mqtt/login` audit line.

`--baseline-web <WebServer_Server.cpp>` runs the same scenarios against the old
per-IP web lockout through an adapter (expected to fail). `--component DIR`
reads the sources from another tree. Behavioural BLE, display and ESP-NOW
cases are in `ble_display_login_lockout` and `espnow_user_credentials`. Not
covered: real httpd/MQTT traffic and the device reply texts. Run with
`ctest --test-dir build -R '^credential_attempts$' --output-on-failure`.

## Capture key loader

`capture_crypto_key` compiles `captureCryptoEnsureKey` from
`System_CaptureCrypto.cpp` unchanged against scripted NVS and libsodium fakes.
It checks that only `ESP_ERR_NVS_NOT_FOUND` (missing namespace or missing blob)
mints and stores a key, that a read-only `nvs_open` failing with
`ESP_ERR_NO_MEM` or `ESP_ERR_NVS_NOT_INITIALIZED` fails closed without calling
`nvs_set_blob` even when the read-write open would succeed, that the next call
loads the stored key, that an unreadable or short stored blob is never
replaced, and that a mint which cannot be committed is not used. Not covered:
sealing and opening rows, the PSRAM scratch, real NVS and flash encryption. Run
with `ctest --test-dir build -R '^capture_crypto_key$' --output-on-failure`.

## LLM tokenizer loader

`llm_tokenizer_loader` compiles the tokenizer structs from
`System_LLMInternal.h` and `loadTokenizerFromFile`, the merge map,
`freeTokenizer`, `encode` and `decode` from `System_LLMTokenizer.cpp`
unchanged, with an in-memory `File` and a PSRAM allocator that truncates sizes
to the ESP32's 32-bit `size_t`. A well-formed blob loads and encodes; a blob
that runs past the file or ends inside the vocab, a `merge_count` that does not
fill the blob exactly (including one whose byte size wraps 32 bits), merge ids
at or above the vocabulary, a tokenizer vocab larger than the model's
`vocab_size` or above the 65536 ids the 16-bit merge keys hold, and trailing
bytes inside the blob all fail the load, and `freeTokenizer` then releases
everything. Each case runs in its own process. Not covered: the header and
weight loaders, `forward()` and sampling. Run with
`ctest --test-dir build -R '^llm_tokenizer_loader$' --output-on-failure`.

## Setting readers

`setting_readers` is a source check over the component sources: every row of
`powerSettingEntries` and `cameraSettingEntries` must name a `gSettings` field
that something reads outside the settings table, `setSetting()` writes and the
setting's own one-word set command. It also requires that `power auto` does not
promise a battery-driven switch and reports that auto mode is unavailable
unless a caller of the auto-mode check exists. It does not check how a reader
uses the value. Run with
`ctest --test-dir build -R '^setting_readers$' --output-on-failure`.

## IP ban store

`ip_ban_store` compiles the IP ban section of `WebServer_Server.cpp` (load,
save, `banIp`, `unbanIp`, the `isIpBanned` lookup the httpd task runs on every
request, `broadcastBanList`, `banListJson`) with the real `FsLockGuard`,
`writeText`/`writeTextAtomic` and vendored ArduinoJson on the shared fake VFS
(`fake_vfs.h`). Encryption, sessions, events and output are doubles. It checks
that a failed save keeps the previous `ip_bans.json`, reports the failure and
keeps the new ban in force in memory, and that banning again retries the save;
that a failed unban save keeps the ban; that an encryption failure never saves
a list missing an entry; that 16 bans with 39-character IPs and 79-character
reasons list as complete JSON and round-trip through the file; that a list too
large for the 4 KB reply buffer is refused with a reply the executor reads as a
failed command (`Error: Ban list too large for one reply. Remove entries with
unban.`) rather than a truncated document; and that the lookup takes the
ban-table lock while no file I/O runs under it. It also checks that the ban's
session kick reads the session table under the session fence (a counting
double), and runs the real `ban`/`unban` adapters (`cmd_ban`, `cmd_unban` with
`CommandArgs`): a ban or unban whose save failed is reported as not saved,
distinct from a full list or an address that is not banned; a host name or a
short IPv4 form (`192.168.1`) is refused with `Error: Not an IP address. Give
it as 192.168.1.100.`, bans nothing and leaves the file unwritten. Not covered:
real task interleaving and the device AES helpers. Run with
`ctest --test-dir build -R '^ip_ban_store$' --output-on-failure`.

## Migration restore

`migration_restore` compiles `applyStagedMigrationRestore`,
`writeRestoreFilesFromDoc` and the backup directory walk from
`WebServer_MigrationTool.cpp` with the real `FsLockGuard`, the real text
writers and vendored ArduinoJson on the shared fake VFS. It checks that one
failing file write fails the whole restore, leaves every device file as it was,
removes the staging files and does not set `gRestoreComplete`; that a
successful restore moves `users.json` into place last; that a failed
cross-device credential-setup marker fails the restore; and that the backup
walk keeps nested paths longer than 128 bytes whole and leaves out, with a
warning, a map that would push the bundle past the 512 KB the restore accepts.
It also checks the confirm prompt's provenance summary (claims sanitised; a
bundle without a fingerprint shown as unverified, never as this device), the
size/SHA-256 advice for unforced and forced uploads, and that discard frees the
staged buffer before clearing the pending flag. `checkBackupFormat` treats a
bundle without `formatVersion` as format 1 and refuses a newer or
non-whole-number one.
A source check requires `/api/backup` to refuse an overflowed or oversized
bundle with the limit `/api/restore` uses, to write `kBackupFormatVersion`, and
`/api/restore` to refuse a bad format after the magic check and before the
fingerprint check and staging. With comments removed, the refusal follows the
check directly, frees the body first and returns, and nothing else branches on
the check. Not covered: the HTTP handlers
themselves, the device AES helpers, a rename failing part-way through
promotion, and binary map content. Run with
`ctest --test-dir build -R '^migration_restore$' --output-on-failure`.

## Camera web frame-only

`camera_web_frame_only` is a source check over the web sources (`WebPage_*`,
`WebServer_*`, `System_Camera*Web*`): no camera stream endpoint is named, no
`multipart/x-mixed-replace` response exists, `handleCameraStream` is neither
defined nor declared, and every httpd handler that captures a camera frame captures one per
request without a loop, because a looping handler holds the single httpd task
until its viewer disconnects. `/api/sensors/camera/frame` must stay registered.
It does not compile or run the handlers. Run with
`ctest --test-dir build -R '^camera_web_frame_only$' --output-on-failure`.

## User roster authority

`user_roster_authority` compiles the admin/super predicates (with their
bond-admin branch), `isValidUser`, `getUserIdByUsername`, `userAccountRank`,
the mutation rank rule, the ban/unban/`sessionrevoke` handlers, the
targeted-logout owner, the `userlist`/`pendinglist`/`sessionlist` handlers and
the roster snapshot from `System_User.cpp`, plus `hasAdminPrivilege` and
`hasSuperAdminPrivilege` from `HardwareOne.cpp`, with the real `FsLockGuard`
and vendored ArduinoJson on the shared fake VFS. It builds twice, with
`ENABLE_HTTP_SERVER` 0 and 1. It checks that an account named `bond-admin`
neither logs in nor resolves to an id and is privileged only as the live
ESP-NOW bond (serial, web, UART and BLE sessions carrying the name are not);
that `unbanuser`, `sessionrevoke user` and targeted logout refuse a
higher-ranked target; that account bans and `sessionrevoke user` work without
the HTTP server; that list replies are NUL-terminated and parseable or an
explicit `Error:`, with `userlist` emitting only id, username, role, banned and
createdAt; and that the roster snapshot keeps full usernames and normalized
roles. A source check requires the OLED and lens user pages to read the
snapshot rather than users.json and the lens page to offer Kick/Ban in every
build. A second source check requires `hasAdminPrivilege` and
`hasSuperAdminPrivilege` to be declared in `System_User.h` and nowhere else
(no local `extern` or forward declaration in any component source), and the
gate's comment to stop calling `authorizeCommand` its only caller (A1).
Since G-55 a role comes only from the account's own entry: no parser treats the
first account as admin or super admin, and `userlist` names a roster with no
super admin.
The ban writer, session fan-out, web session tables, hashing and the
per-user settings loader are doubles; the OLED and lens pages themselves are
not compiled. Run with
`ctest --test-dir build -R '^user_roster_authority$' --output-on-failure`.

## LLM load policy

`llm_load_policy` compiles `handleLlmLoad`, `handleLlmUnload` and their reply
helper from `WebPage_LLM.cpp`, `cmd_llm_load`/`cmd_llm_unload` from
`System_LLMCommands.cpp`, `CommandArgs` from `System_Command.cpp`, and
`llmBackendSelect`, `llmBackendUnload`, the context policy and
`llmBuildLoadCommand` from `System_LLMBackend.cpp`, with vendored ArduinoJson.
The web command executor is a double that refuses the admin rows to a
non-admin (the runner pins `llmload` and `llmunload` as requiresAdmin rows)
and otherwise runs the real command handlers; the engine and model resolution
record their calls. It checks that a user-role session is refused and never
reaches the engine, that the submitted command carries the model id, that an
admin load gets `llmMaxContext` with the body's `max_ctx` only able to lower
it, that `llmBackendSelect` applies `llmMaxContext` when called with 0, that
`llmload <id> <maxctx>` passes the ceiling through, and that an id with a
quote or control character is refused before submission. A source guard
requires `WebPage_LLM.cpp` and `OLED_Mode_LLM.cpp` to submit the commands and
never call `llmBackendSelect`/`llmBackendUnload`. The real `authorizeCommand`,
the OLED screen flow and the engine's KV sizing are not covered. Run with
`ctest --test-dir build -R '^llm_load_policy$' --output-on-failure`.

## Web toast fan-out

`web_toast_fanout` compiles, from the shipping sources, the toast block of
`systemEventsNotifyTick` with `notifFormatEvent`, `levelName` and
`notifRenderToastJson` (`System_Notifications.cpp`), the session fence,
`webInvalidateSessionSlot`, the broadcast helpers and the fenced SSE queue
accessors (`WebServer_Server.cpp`) and `sseEnqueueEvent`/`sseDequeueEvent`
(`WebServer_Events.cpp`), with vendored ArduinoJson and a
`std::recursive_mutex` double for the FreeRTOS mutex. `toast_payload` renders a
LOGIN_FAIL whose attempted name holds a quote, a backslash and a newline and
parses the queued payload back (one SSE data line, the exact text, no injected
fields); an all-control-character name must still give a complete document
inside one queue slot. `fanout_race` runs a producer thread (notification tick
and BLE roles) against an httpd-role thread that signs a slot out and in
between an admin and a guest while draining both sessions' queues: the viewer
predicate must run without the fence, no admin-only toast may reach the guest,
and the stable session must account for every event exactly once. The source
guards for the same contract are in `test_notification_integration_guards.py`.
Socket writes, the browser and the BLE/G2 producers' own state are not
covered. `--tsan` builds with ThreadSanitizer. Run with
`ctest --test-dir build -R '^web_toast_fanout$' --output-on-failure`.

## Forced password change

`password_change_gate` compiles, unchanged from the shipping sources, the
per-user settings merge with its editable-key list `userSettingsPatchRefusedKey`
(`System_Settings.cpp`), `userMustChangePassword` with its cache and guest rule
and `createAccount` (`System_User.cpp`), and `webPasswordChangeAllows`
(`WebServer_Utils.cpp`), with vendored ArduinoJson. The merge must refuse
`password`, `gamepad_password` and `mustChangePassword` whatever the value (a
JSON null included) and any key that is not a preference, leaving the stored
file untouched, while theme, notification level and dashboard layout keys still
merge. The decision must ignore the flag for a guest (the legacy recovery owner
excepted), keep answering from its cache until a password write or identity
change, and not cache unreadable files. The web gate must answer APIs, non-GET
requests and JSON fetches with 403 `password_change_required`, redirect pages to
`/account/password-change`, and admit only that form and `/logout`. A guest
created with a forced change must be refused before anything is published. A
source contract checks that `tgRequireAuth` runs the gate for every web
request, that `setUserPassword` invalidates the cache, that
`/api/user/settings` takes the owner's key decision and that
`userresetpassword` refuses a forced change for a guest. The settings store,
roster, httpd and users.json writer are doubles; command transports
(serial, BLE, MQTT) are not covered because they do not gate on the flag yet.
`test_user_settings_transaction_guards.py` carries the matching source guards.
Run with `ctest --test-dir build -R '^password_change_gate$' --output-on-failure`.

## Factory reset wipe

`factory_reset_wipe` compiles the shipping `factoryreset_confirmed`, its
removal helpers and the `cmd_factoryreset` prompt (`System_Utils.cpp`), the
real `FsLockGuard` and the real VFS `DirectoryReader` against the shared fake
VFS (`fake_vfs.h`). A confirmed reset must remove `users.json`,
`pending_users.json`, every file in `user_settings/` and every `u<id>` folder
under `/stt` and, when the card is mounted, `/sd/stt` (subfolders included),
while settings, IP bans, other `/stt` and `/sd` content and the folders
themselves stay. It must record the issued account ids first, remove account
data before `users.json` and the settings files after it, make every call
under the filesystem lock as system, and schedule exactly one reboot. A
failure before `users.json` (pending list, a transcript, an SD folder, the
roster itself) must stop the reset with the roster in place and no reboot; a
settings file left after the commit is reported but the reset completes; an
unmounted card keeps `/sd/stt` and says so. The prompt must name what is
deleted. The reboot timer, events, logs, the microphone drain and the
System_User id record are doubles (the record itself is covered by
`user_store_mutations`); real LittleFS/FAT behaviour and the setup wizard are
not covered. Run with
`ctest --test-dir build -R '^factory_reset_wipe$' --output-on-failure`.

The same runner covers `factoryreset all`: it is offered only on the serial
console and outside OTA health probation, its prompt names what is erased and
fits the confirm buffer (the size is read from `System_CLIConfirm.cpp`), the
boot-erase request is stored before anything is removed and is dropped again
when the reset stops early. `factory_wipe_policy_tests` runs the pure boot-time
sequence (`System_FactoryWipePolicy.h`) against a fake device: nothing happens without
a request; the order is attempt record, filesystem, NVS entries, NVS partition;
a power loss at any step resumes at the next start; a failed step keeps the
request; a failed partition erase still completes when the entries were
cleared; after three attempts the request is dropped and reported. The flash
and NVS calls in `System_FactoryWipe.cpp` and the first-time setup that follows
are not covered and have not been run on hardware.

## Clock rules (System_ClockCore.h)

`clock_core_tests` runs `System_ClockCore.h`, the device-free rules behind
`System_Clock`, as they are (time rules stage 1, 2026-11-22). Covered: the
validity bounds (2020 for the own clock, 2020-2099 for a time from outside);
the source rank table of `Clock::step` for every pair of sources, set and not
set (ntp > manual > mesh-ntp > cm5 > rtc > ring > mesh > carried; an operator
command always sets; mesh time corrects mesh time down to the source the
device had before mesh time took over, the floor; an implausible time is
refused first); the ledger across an SNTP step that lwIP has made and the main
loop has not reported yet (the other sources are ranked against NTP from the
callback on; an operator command in between keeps its name at the report); civil time against the C library for every day of 2020-2099
and with a fixed offset across a date line; the formatters (ISO UTC and its
parser round trip, compact UTC, local date and time with and without seconds
and the `UTC+H:MM` label, local date, file stamp, the log prefix with its
numeric offset, the offset labels) with a clock that is not set and a buffer
one byte short; durations and ages in both styles; the time `timeset` and
`rtcset` read (local by default, `utc`, `local`, a Unix time, every refusal
and the edges of the range); the timezone table (unique abbreviations,
quarter-hour offsets, the limits of the setting, an offset re-opening on its
standard-time entry, the settings list built from it). Not covered: the live
clock, `settimeofday`, the chores after a step and the settings buffer
(`System_Clock.cpp`); the mesh follower's use of the rank table is in
`espnow_time_core_tests` and `espnow_time_status`.

## Ring clock confirmation

`clock_confirm_policy_tests` runs `System_ClockConfirmPolicy.h`, the rule that decides
which clock the R1 ring is given, against a fake device (wall clock plus
monotonic time). Covered: no reference while the clock is unset; a first
network sync and hourly corrections are accepted; a network step that moves a
set clock by more than a day in either direction starts a hold, the hold
continues over further forged replies and covers the window before the main
loop has seen the step; a reply that agrees with the earlier clock, or any
local source, ends it; a ring-adopted, carried-over or invalid manual clock is
never a reference. The wiring in `System_Clock.cpp` (`Clock::confirmedNow()`)
and its callers in `G2_Ring.cpp` and `G2_Health.cpp` are compiled by the ring
runners with a stub that returns the wall clock; the hold itself is not
exercised against the ring code, and nothing here has run on hardware.

## Command name uniqueness

`command_name_uniqueness` scans every `CommandEntry` table that
`gCommandModules` registers and fails when one name (ASCII case ignored, as
lookup ignores it) has two rows that one build can link: lookup keeps the
first registered row, so the other is dead while `help` still lists it. Rows
in different branches of one `#if` group, or in two definitions of the same
table symbol (the OTA layout variants, a disabled module's stub), are
exclusive; macros are not otherwise evaluated, so rows of different tables
count as compiled together. `DEFERRED_DUPLICATES` in the runner names any
duplicate left to another session. It also compiles the shipping
`registerCommand(s)` and `initializeCommandSystem` (`System_Command.cpp`)
against fixture modules and checks one CMDREG log line per shadowed row with
both module names, and none for unique names. Per-profile compilation of the
real tables is not covered (`command_catalogue` covers module order with
fixture rows). Run with
`ctest --test-dir build -R '^command_name_uniqueness$' --output-on-failure`.

`command_handler_names` reads the same tables for the handler each row names.
When a handler's name, without its `cmd` prefix and its underscores, is the
name of a command that exists, that handler must be on a row of that command:
`cmd_wifidisconnect` was the handler of `closewifi` while the command
`wifidisconnect` ran another function. `HANDLERS` in the runner pins the rows
of the Wi-Fi disconnect commands. `RETIRED` lists handler words that were
never commands (`wifidrop`, which comments and the user guide had named as if
it could be typed): no firmware source, and none of `docs/USERGUIDE.md`,
`docs/QUICKSTART.md`, `KNOWN_ISSUES.md`, the top `README.md` and
`docs/README_DETAILS.md`, may name one. It reads source text only;
`--component` points it at another component directory.

`readme_overview` keeps the top `README.md` a short overview that matches the
tree: the title form `RELEASING.md` names, at most 120 lines, the
getting-started commands and the tools and saved configuration they name, the
boards table against `boards/*.defaults`, the mesh protocol name, every
relative link and anchor in `README.md` and `docs/README_DETAILS.md`, and the
two headings of the details page that other documents link to. `--repo` points
it at another checkout.

## Edge Impulse inference lock and continuous task

`test_edge_impulse_lifecycle.py` compiles the whole of `System_EdgeImpulse.cpp`
on the same fakes as `test_edge_impulse_jpeg.py`, with the continuous task on a
`std::thread`, FreeRTOS semaphores on `std::mutex`, and a fake interpreter that
can hold one Invoke open. Scenarios: `eicontinuous 0` then `1` inside one
interval leaves exactly one task, and a stop returns with the task gone;
`eimodelunload`, a model load and an unload during a running Invoke wait for that
frame before freeing (a load resumes continuous mode on the new model); camera,
`eidetect`, file and web detect during a continuous frame report busy and run
no Invoke, and a detect between frames runs. Not modelled: FreeRTOS priorities,
preemption, core affinity, real TFLite kernels. Run with
`ctest --test-dir build -R '^edge_impulse_lifecycle$' --output-on-failure`
(or the runner directly with `--sanitize`).

## Self-registration

`test_register_submit.py` extracts the shipping `handleRegisterSubmit` and its
failure page (`WebServer_Server.cpp`), `urlDecode`/`extractFormField`,
`CommandArgs`, `authorizeCommand` and its session-control classifier, and the
public account rules plus the `userrequest` adapter (`cmd_user_request`) from
`System_User.cpp`. The account-request owner (`submitAccountRequest`) is a
recording double. Cases: a passphrase with spaces, repeated words, quotes or
percent escapes reaches the owner byte-exact and no command line is built; an
already-pending name, a full queue or a storage fault is shown with fixed text
and does not count toward the login lockout, while rejected form fields still
do; `userrequest` from every external transport without an identity (web CLI
and register path included) is still refused with "Authentication required";
validate-only `userrequest` returns VALID with the arguments checked and never
reaches the owner; a quoted passphrase given to `userrequest` reaches the owner
byte-exact; while `userMustChangePassword` (a double) holds, every external
transport refuses anything but `userchangepassword`, bare `login`/`logout` and
`whoami` with the change-password reply, one audit entry and one denial event,
and other accounts, internal callers and `AuthBypass` are not gated. A source
contract keeps the handler off the command path and the
owner on the locked pending-list writer. Not covered: the owner's pending-list
I/O itself (lock, 32-entry cap, atomic write), the real lockout table and
httpd. Run with
`ctest --test-dir build -R '^register_submit$' --output-on-failure`
(or the runner directly with `--sanitize`).

## MQTT discovery and external-sensor bounds

`test_mqtt_json_bounds.py` extracts the shipping `publishDiscoveryConfig`,
`publishPeerDiscoveryConfig` with `mqttLocalDiscovery` and `meshPeerDeviceId`,
their shared 1 KB discovery buffer, `updateExternalSensor` with its sensor-table
state (`System_MQTT.cpp`) and `macToPathToken` (`System_MacText.h`), and compiles
them with `System_MQTTPayloadCore.h` and the vendored ArduinoJson against a
recording `esp_mqtt_client_publish`. Cases: a short base topic publishes the
expected retained discovery document byte for byte (local with the board as
`model` and `sw_version`; mesh peer without a model, an unnamed one under the
unknown-peer label); a quote and a backslash in the friendly name and room come
out escaped; an unnamed device is named by its fallback; a 400-character base topic skips the publish with one warning instead
of writing past the buffer; a sweep of base-topic lengths across the capacity
publishes every document of at most 1023 bytes exactly and skips every longer
one; esp-mqtt's topic and payload, given in exactly-sized buffers with no NUL
(payload with an embedded NUL), are stored with exactly `topic_len` /
`data_len` bytes, and a topic length covering a prefix of a longer buffer
stores only that prefix; the client's own `<base>/command` and
`<base>/response` (`System_MQTTControlTopic.h`) are never stored, for the base
topic of the live subscription and for the one in the settings, while their
neighbours (`.../command/extra`, another device's command topic) still are.
Source guards: no unchecked `sizeof(configJson) - pos`
remains, the periodic state document is checked by `serializeBounded` before
it is published, the
two topic words are spelled only through the header, and the filter runs
inside `updateExternalSensor` before the table is touched.
Not covered here: the state document's sensor sections (`mqtt_payload_core_tests`)
and the real esp-mqtt client. `--case discovery|peer|sensor` runs one
group. Run with
`ctest --test-dir build -R '^mqtt_json_bounds$' --output-on-failure`
(or the runner directly with `--sanitize`).

MQTT commands no longer wait for cmd_exec on the esp-mqtt task (which holds
the client's API lock during the event). `mqtt_lifecycle_contract` guards that
`System_MQTT.cpp` has no `submitAndExecuteSync`, that a command is queued with
`submitCommandAsync` and answered from its completion with
`esp_mqtt_client_enqueue` (or a direct `esp_mqtt_client_publish` while a stop
is pending), and that the admission moves into the completion only after
allocation and is released only after the answer.
`mqtt_lifecycle_gate_tests` adds the movable `MqttAdmission`
(`System_MQTTAdmission.h`): the callback's return does not release a handed-off
admission, teardown waits for the completion, a refused or released admission
releases exactly once, and a three-thread race (callback, completion, stop)
never lets teardown overlap the completion's publish. Not covered: the real
cmd_exec queue and esp-mqtt outbox.

`test_mqtt_command_reply.py` extracts `MqttCommandReply`,
`enqueueMqttCommandReply` and `mqttCommandCompleted` from `System_MQTT.cpp`
and compiles them with the production gate and `MqttAdmission`, vendored
ArduinoJson and a small esp-mqtt model (a direct publish leaves at once, an
enqueued item waits for the client task's next loop pass, stopping the client
deletes the outbox). Cases: a command that stops MQTT (`closemqtt` over MQTT)
requests the stop before its completion runs, and its reply is still
delivered, published while the admission blocks teardown; with no stop pending
the reply is enqueued (no socket write on cmd_exec) and sent on the next loop
pass; the local and routed documents are byte-exact. Not covered: the real
esp-mqtt client, a broker, or a stop requested from another source after a
reply was queued. `--baseline <component-directory>` replays the cases against
other sources. Run with
`ctest --test-dir build -R '^mqtt_command_reply$' --output-on-failure`
(or the runner directly with `--sanitize`).

## Video recording completion

`video_completion` compiles the shipping AVI recorder file path from
`System_Camera_Video.cpp` (header skeleton, frame writer, first-frame geometry
patch, `finalizeAndClose` and the failed-start cleanup) against the shared fake
VFS with the real `FsLockGuard`. A complete three-frame clip posts exactly one
`VIDEO_SAVED` and its header carries the measured timing (600 ms over three
frames: 200000 us per frame, rate/scale 5 fps), the largest frame as
`dwSuggestedBufferSize` and the recorded geometry in avih, strh `rcFrame` and
strf. A short idx1 write, a failed seek, a failed header patch, a flush error,
an earlier frame-write failure and a failed geometry seek or write post no
saved event, post one `VIDEO_RECORD_FAILED` carrying the clip name and report
an SD write failure; a failed start removes its file and
releases the SD registration. A second harness compiles the real SD-in-use
registration, `unmountSd`/`remountSd`/`formatSd` and the
`sdunmount`/`sdmount`/`sdformat` commands from `System_VFS.cpp` against a fake
SD driver: each refuses (the command names the holder) while a registration is
held, decided under the filesystem lock, and works after it ends; the table
holds four owners. Not covered: the recorder task loop and its pacing, camera
capture, RTOS task creation and the real SD driver. Run with
`ctest --test-dir build -R '^video_completion$' --output-on-failure`
(or the runner directly with `--sanitize`).

## File permission owner and web JSON replies

`filesystem_listing_permissions_contract` also compiles the real path rule
table, role resolver and `canRead`/`canEdit`/`canCreate`/`canDelete`/
`canRename`/`isAdminOnlyPath` from `System_Filesystem.cpp`
(`filesystem_role_matrix_harness.cpp`, fake roster) together with the
`requiresAdmin` column parsed from every file command row, and asserts one
per-role read/write/create/delete matrix through both ways in: the command
table (serial, BLE, web CLI and the Files page's delete/rename) and the direct
web endpoints (their table-derived admin-path pre-check, checked in the source,
then the table; `/api/files/create` adds the command). Rows cover the default
user tree, `/espnow/`, `/system/` files, `/logging_captures/` and the
system-owned `/battery.csv` (users read, admins read and delete, only the
system writes, creates or renames, including the `.1`/`.2` rotations and the
header staging file) and the ESP-NOW inbox `/espnow/received` (A1): pushed
files and files pulled from the bonded peer (its `users.json`, say) are admin
tier there, so users and guests get nothing while admins keep read, write and
delete, and the rest of `/espnow/` (`/espnow/peer.txt`, `/espnow/receivedX.txt`)
is unchanged. It takes `--cxx` and `--sanitize`; CTest runs it without
arguments (`c++`, no sanitizers).

`web_json_replies` compiles the production `handleAutomationsExport`,
`handleIconGet`, `handleFilesCreate`, `handleMicRecordingsList`,
`handleVideoRecordingsList` (`System_Camera_Video.cpp`) and `llmListModels`
(`System_LLMCommands.cpp`, with `LlmModelDesc` taken from
`System_LLMBackend.h`) (plus `jsonQuote`, `jsonAppendEscaped`, `sendJsonError`
and the recordings-list builder) against a fake httpd that keeps header pointers and reads them at
`httpd_resp_send`, with real ArduinoJson. Under `--sanitize` a header value
that dies before the send is an ASan error (the `?id=` export disposition, the
icon `X-Icon-Size`); the export filename keeps only `[A-Za-z0-9._-]`, at most
48 characters. An automations.json from newer firmware is not exported (single
or bulk): a `409` naming both formats, with no attachment header. The runner
parses every printed payload with Python's strict
`json` module: recordings named with a backslash, comma, colon, quote or
control character; `files/create` errors with quotes, a backslash, a newline
and a control character (folder and file branches); a 150-character name that
must reach `filecreate` whole; `/api/videos` names with a quote, a backslash, a
control character or a colon (the size is what follows the last colon);
`/api/llm/models` ids, names and paths with a quote, a backslash or a tab. Not
covered: the map, GPS track and LLM status replies (same helper, no harness)
and the producers of the recordings, video and model lists. Run with
`ctest --test-dir build -R '^web_json_replies$' --output-on-failure` (or the
runner with `--sanitize` / `--case <name>`).

## Web session eviction

`web_session_eviction` compiles, from `WebServer_Server.cpp` on every run,
`setSession`, `findFreeSessionIndex`, `pruneExpiredSessions`, the session
cookie writer and the session fence with its slot helpers, plus `SessionEntry`
from `WebServer_Server.h`, with `unsigned long`/`long` mapped to the ESP32's
32-bit widths so millis() arithmetic wraps as on the device. It checks that a
login into the full two-slot table replaces the session that expires first, at
early uptime and across the millis() wrap (never a blind slot 0), and that the
replaced session is signed out like any other logout: `SYSEVT_LOGOUT` with
`evicted`, a logout reason for its browser unless it is the address signing
in, its socket closed unless it carries this login, its transport epoch closed
and its slot generation bumped (a revoked session still held in its slot gets
its socket closed but no second event or reason). httpd, tokens, epochs,
events and logout reasons are recording doubles. Not covered: the cookie
reaching a browser,
`handleLogin`, or the login page showing the reason. Run with
`ctest --test-dir build -R '^web_session_eviction$' --output-on-failure` (or
the runner with `--sanitize` / `--case <name>`).

## Command task isolation

`command_task_isolation` executes code that runs beside the command executor
on other tasks. The broadcast case compiles `broadcastOutputCore`
(System_Debug.cpp) and `currentCommandValidateOnly` (System_Utils.cpp): one
thread runs a validate pass the way `commandExecTask` does (its own
CommandContext with `validateOnly`, plus the handler mirror
`gCliValidateOnly`), while a second thread broadcasts, once with no command
context and once inside its own command. The validating thread's lines are
dropped; the other thread's lines are delivered, and the executor's own
output resumes after the pass. The password case compiles
`userChangePasswordCore` and `cmd_user_changepassword` (System_User.cpp)
with `RETURN_VALID_IF_VALIDATE_CSTR` and `USER_CHANGE_PASSWORD_MESSAGE_MAX`:
every outcome (mismatch, short, unchanged, wrong current password, filesystem
not ready, not authenticated, storage failure, success, a 64-character name)
returns its status with the message in the caller's buffer while the shared
debug buffer keeps another command's text; short and absent buffers are
handled; the CLI handler still answers through the debug buffer and honours
validate. Threads, task identity, the per-task context, the output queue and
the account store are host doubles. Not covered: the ESP-IDF queue, the
web page, or executeCommand placement (test_command_resolution_contract
checks that every executeCommand call is on the executor or early boot, the
web logging and ESP-NOW status endpoints submit through
`executeInternalStatusCommand`, and command_executor_admission runs
executeCommand's own refusal of other tasks, and who writes
`gCliValidateOnly`). Run with
`ctest --test-dir build -R '^command_task_isolation$' --output-on-failure` (or
the runner with `--sanitize`).

## Flags and counters that several tasks change

`shared_flag_atomics` covers integers that more than one task changes with `|=`,
`&=` or `++`. On a plain or `volatile` integer each of those is a load, a
change and a store, and a task scheduled between another task's load and store
loses one of the two changes. The test takes the definition of each variable
and the statements that change it from the firmware sources and runs them
against each other on host threads; every check is an invariant that holds
under any scheduling once each change is one atomic operation.

- Output lanes (`gOutputFlags`): one thread per lane opens and closes its lane
  with the production statements (`log start`/`log stop`, `outble`, `outg2`,
  the HTTP server start and stop) while another runs the output-lane step of
  `applySettings` with `outSerial` on and off. A lane must read as its owner
  left it until the owner changes it again. `applySettings` keeps WEB, FILE,
  BLE and G2, sets SERIAL from the setting and drops every other bit.
- Glasses tap masks (`gCamStreamPendingTap`, `gHealthPagePendingTap`): one
  thread per row records taps with the event dispatch's statement and waits
  until the worker took each one; a worker thread runs the page worker's
  drain. The number of taps the drains return must equal the number recorded.
  The drain is one `exchange(0)`; a read followed by a store of 0 erases the
  tap that arrives between the two.
- G2 hijack bridge (`gMenuGen`, `gCmdSeq` in `G2_HijackCmd.cpp`): four threads
  call the production `g2BumpMenuGen()` and the generation must move by the
  number of calls; four threads run the statement of `g2SubmitHijackCommand`
  that stamps a command's sequence number, and no number may repeat or be
  skipped.
- Generation counters (`gNotifPrefsGen`, `gHealthPageGen`): four threads run
  the notification prefs bump and `notifPrefsGeneration()` must move by the
  number of bumps; four threads run the statement of `g2ShowHealthPage` that
  stamps an open's generation, and no two opens may get the same one.

Not covered on purpose: `sRxQueueDropped` (`G2_Ring.cpp`) and `gDebugDropped`
(`System_Debug.h`) are diagnostic counters that are only printed; the comment
on `observeHwm` in `System_Utils.h` records that a lost count is accepted there.

Source contract: each variable has one definition and one declaration, both
`std::atomic`, and no statement stores a value computed from an earlier read
(`x = x | y`, or a read followed by `x = 0`). The harness also checks the
type, so a variable that goes back to a plain integer fails without relying on
timing. `--component <component-directory>` replays other sources; the sources
from before the change lose updates in every run. Threads are host threads;
FreeRTOS scheduling, the two cores and the commands' other work are not run.

## Speech recognition: voice text read by other tasks

`espsr_voice_text` covers the category, the subcategory and the last command
of the speech recognizer (`System_ESPSR.cpp`). The recognizer task `sr_main`
re-made three Strings at every recognised phrase and timeout while the OLED
speech page, the status JSON and the telemetry print read them from other
tasks with no lock; a String that is re-made during a copy can be read after
its block was freed. The recognizer now publishes every change into fixed text
under one lock (`voiceSetContext`, `voiceSetLastCommand`) and a reader gets a
copy made under that lock (`getEspSrCurrentCategory`,
`getEspSrCurrentSubCategory`, `getEspSrLastCommand` into a caller buffer;
`voiceTextSnapshot` for the status JSON).

The test compiles that block and the three accessors with the size constants
of `System_ESPSR.h`: values and partial updates (a subcategory alone, the
category alone as `srcmdssync` empties it), a phrase of the esp-sr limit and
the longest last command published whole, over-long text cut in the shared
copy while the recognizer's own Strings keep it, reader buffers of 0, 1 and 16
bytes, every reader and writer waiting while the lock is held, every copy made
with the lock held by the copying thread, and two reader threads that never
see a mix of two values while a third rewrites all three texts. Source
contract for what the host does not compile: the two recognizer Strings are
assigned only in `voiceSetContext` and named only by `onVoiceCommandDetected`,
`srTask` and that writer; `gLastCommand` is gone; `sVoiceText` is named only
by its own functions and the accessors; each of those enters and leaves the
lock once; the status JSON takes its three texts from one non-const snapshot
(ArduinoJson stores a `const char` array as a literal with its padding); the
OLED speech page passes its own buffers. Host doubles: `String`, the critical
section (a mutex that records its owner), `strlcpy` and the esp-sr phrase
limit. Not covered: the ESP-IDF critical section itself, the recognizer and
ArduinoJson; `System_ESPSR.cpp` and `OLED_Mode_Speech.cpp` only compile in a
build with `ENABLE_ESP_SR` set. `--component <component-directory>` replays
other sources; `--sanitize` adds ASan and UBSan.

## Battery log clear

`battery_log_clear` extracts the `clear` branch of `cmd_batterylog` and the log
path constants from `System_Battery.cpp` and runs them against the shared fake
VFS with a `canDelete` double that answers as the `/battery.csv` permission row
does (admins may delete, users only read). An admin clear removes
`/battery.csv` and both rotations under the FS lock and replies
"Battery log cleared."; a role user gets "Error: battery log not cleared:
permission denied" and every file stays; a remove that fails for an admin is
reported with the number of files left; an absent log clears cleanly. Not
covered: the permission table itself, the other `batterylog` branches and the
logger. Run with
`ctest --test-dir build -R '^battery_log_clear$' --output-on-failure` (or the
runner with `--sanitize`).

## Retired feature text

`retired_feature_text` reads the real sources (no compile) and checks that the
camera, power and edgeimpulse module descriptions in
`System_CommandRegistry.cpp` name only commands their own tables register and
none of the retired ones (`cameraautocapture`, `cameraautocaptureinterval`,
`camerasendaftercapture`, `cameratargetdevice`, `eiinputsize`), that the power
description no longer promises an automatic low-battery switch, that no
command table registers a retired command, that `System_Settings.h` no longer
declares `edgeImpulseInputSize` and no production source reads it, and that the
generated `docs/COMMAND_REFERENCE.md` lists no retired command and carries the
current camera, power and edgeimpulse descriptions. Not covered: the commands'
runtime replies and the hand-written `docs/USERGUIDE.md`. Run with
`ctest --test-dir build -R '^retired_feature_text$' --output-on-failure` (or
`python3 test_retired_feature_text.py`).

## Public doc commands

`public_docs_commands` reads `docs/USERGUIDE.md`, `docs/QUICKSTART.md` and the
real sources (no compile). Every command line in a USERGUIDE code block must
resolve against the `CommandEntry` registry (`tools/command_registry.py`
`parse_commands()`, the tables `docs/COMMAND_REFERENCE.md` is generated from)
with the firmware's case-insensitive, whole-word, longest-prefix rule from
`System_CommandLookupCore.h`; `(admin)` / `(super admin)` annotations must match
the registry; every flag in the USERGUIDE debug-flag table and every command in
QUICKSTART's Step 4 must be registered; and the QUICKSTART first-boot wizard
table must list `getWizardPageTitle` titles in `kPageOrder` order
(`System_SetupWizard.cpp`). Indented continuation lines, `--- section ---`
separators, `(note)` lines, automation `IF`/`NAME:`/`SCHEDULE:` syntax,
`debug<flagname>` placeholders and the UART-only `dictate` line are skipped. Not
covered: command names in prose elsewhere and argument syntax. Run with
`ctest --test-dir build -R '^public_docs_commands$' --output-on-failure` (or
`python3 test_public_docs_commands.py`).

## Bare idf.py guard

`bare_idf_guard` runs `cmake/HW1BoardGuard.cmake` with `cmake -P` (no ESP-IDF).
With no `HW_BOARD` and no deployment, configure must stop with a message naming
`tools/build_board.sh` when the sdkconfig is the tracked `config/sdkconfig`
(left byte-for-byte unchanged) or was saved for another Arduino variant or flash
size than the defaulted board; a fresh or matching sdkconfig and any explicit
board pass. It also checks that the root `CMakeLists.txt` calls the guard before
its first write to the sdkconfig. Run with
`ctest --test-dir build -R '^bare_idf_guard$' --output-on-failure` (or
`python3 test_bare_idf_guard.py`).

## Dead auth helpers removed

`dead_auth_helpers_removed` is a source check over the firmware sources
(`components/hardwareone` outside `test/`, plus `main/`, `updater/` and
`components/hw1_ota_protocol` where present; comments ignored). None of the
retired helpers may be defined, declared or called again: the per-transport
branches and HTTP-off stub of `tgRequireAuth`, `authSuccessUnified`,
`isTransportAdmin`, the registry and help helpers (`commandRequiresAdmin`,
`dispatchCommand`, `isHelpModeCommand`, the `renderHelp*` wrappers,
`exitHelpAndExecute`), the I2C and file-transfer mutex guards, and the web
auth, session and SSE helpers (`getHeaderValue`/`getCookieValue`, which
broadcast the cookie, `sendAuthRequired`, `getLogoutReasonForAuthPage`,
`streamNav`, `renderTwoFieldForm`, `buildUserSessionsJson` and the rest), the
per-session SSE notice queue (`sseEnqueueNotice`/`sseDequeueNotice`, the
`SessionEntry` notice ring, `sseSendNotice`, `webSessionDequeueSseNotice`,
`handleNotice`), the unread `CMD_MODULE_ADMIN`/`CMD_MODULE_NETWORK` module
bits and the uncalled `streamDebugRecord`.
`WebPage_Register.h`, `WebPage_LoginRequired.h` and `WebPage_LoginSuccess.h`
must not exist, neither `/login/setsession` nor `/api/notice` may be named and
no source writes an SSE `notice` frame, `tgRequireAuth` is
defined once, under `ENABLE_HTTP_SERVER`, and refuses any context that is not
a web request, a revoke marks the slot without queueing a `[revoke]` notice,
`/api/sessions` and `/api/admin/sessions` share `handleSessionsList`, and the
undefined three-argument `streamBeginHtml` prototype is gone. No name is
exempt. The HTTP-off no-op stubs once kept for
`authSuccessUnified`, `sseEnqueueNotice` and `sseDequeueNotice` were removed
after the merge. It does not compile anything. `--component <dir>` scans
another tree. Run with
`ctest --test-dir build -R '^dead_auth_helpers_removed$' --output-on-failure`.

## BLE and display login lockout

```sh
python3 components/hardwareone/test/host/test_ble_display_login_lockout.py --sanitize
```

`ble_display_login_lockout` compiles, unchanged, the credential attempt table
and `loginTransport` from `System_User.cpp`, the table's `System_User.h`
declarations, and `BleLoginAsyncJob`, `bleLoginJobFree` and `bleLoginDeferred`
from `BLE_Server.cpp`. The clock, critical section, filesystem lock, password
check, BLE session fence, binder and directed sender, console session
publishers, audit line and event register are host doubles. Every job release
is checked: the password is wiped and the job freed exactly once.

Bluetooth checks:

- Five wrong passwords answer `[ble] Authentication failed.` on the requesting
  session and post `SYSEVT_LOGIN_FAIL` with transport `ble`. The fifth sets the
  lock.
- The sixth attempt, even with the right password, is refused before the
  password is checked and before any bind. `[ble] Login locked out. Retry in
  30 seconds.` goes to that session only, and the audit line says `Locked out`.
- Every connection counts under Bluetooth's one key, so a new connection or a
  rotated address is still locked.
- An unavailable credential store and a connection lost before the bind (right
  password) are not guesses.
- A bound login clears its account's failures and posts `SYSEVT_LOGIN_OK`.
- A stale session epoch touches nothing.

Display checks (`loginTransport` serves the OLED login screen and a bare
`login` typed on the display):

- The sixth attempt is refused without checking the password. It reports the
  time left through `lockoutRetryMsOut`, publishes no session, and is audited
  as `Locked out`. The lock event names the `display` key, and Bluetooth stays
  open.
- A login whose session does not publish, and an unavailable store, are not
  guesses.
- A published login clears the failures.

Not covered: the OLED screens themselves (their adoption is a source contract
in `credential_attempts`), the real GATT stack and timing. `--component DIR`
reads the sources from another tree; the pre-fix tree fails the lockout cases
behaviourally. Run with
`ctest --test-dir build -R '^ble_display_login_lockout$' --output-on-failure`.

## ESP-NOW passwords and USER_SYNC accounts

```sh
python3 components/hardwareone/test/host/test_espnow_user_credentials.py --sanitize
```

`espnow_user_credentials` compiles these pieces unchanged:

- the credential attempt table and `checkCredentialAttempt`;
- the `System_User.cpp` account owner region (name and password rules,
  `mutateUsersJson`, `createAccount` and the account-id record), the roster
  lookup and the reserved-name rule;
- the real `FsLockGuard`, text writers and settings path;
- `DeferredUserSyncWork` and `doUserSyncWork`;
- `v4_handle_cmd` up to its authentication decision. The command it would
  queue is recorded instead.

Storage is the shared fake VFS (`fake_vfs.h`) with vendored ArduinoJson. The
password check, admin rule, hashing, per-user settings writer, NVS id mark,
bond-token validators, radio senders, audit and events are doubles.

Remote command and admin-check cases (C8/D3):

- Password remote commands and USER_SYNC's admin check share ESP-NOW's one
  key, whichever peer sends them.
- The sixth attempt is refused before the password is checked. Its CMD_RESP
  (`Error: Login locked out. Retry in 30s.` or `Admin authentication locked
  out. Retry in 30s`) echoes the msgId. A refused login answers `Error: Login
  failed. Check the user name and password of the target device.`; USER_SYNC's
  `Admin authentication failed` stays.
- A plaintext command is refused before any check.
- Bad bond tokens still reach `logBondAuthFailure` and do not count. A live
  bond token still runs its command while passwords are locked.

Account cases (A11 with C10):

- Reserved names (`bond-admin`, `system`, `AuthBypass`), malformed names and
  short passwords are refused with the owner's text, and nothing is written.
- With the id mark at 40 and `nextId` 5, the account gets id 41. The reply
  carries that id.
- The role is forced to `user` and `createdBy` is `espnow:<device>`.
  `createdAt` is stamped from a valid clock.
- The credential is stored under the new id. `SYSEVT_USER_ADDED` names the
  account and sender, the identity generation is bumped, and the `USERS` log
  line names the id.
- A failed credential write publishes nothing and answers ok=false.
- A repeated name is `User already exists (skipped)` with ok=true.
- The session-encryption and opt-in refusals still answer with a bare ACK
  before any check.

Not covered: real radio traffic, the queued command after authentication, and
`doUserSyncWork`'s placement on cmd_exec (the dispatch side is
`espnow_dispatch_policy`). `--component DIR` reads the sources from another
tree; the pre-fix tree fails the lockout and account cases behaviourally. Run
with `ctest --test-dir build -R '^espnow_user_credentials$' --output-on-failure`.

## Command executor admission

```sh
python3 components/hardwareone/test/host/test_command_executor_admission.py --sanitize
```

`command_executor_admission` compiles, unchanged, `executeCommandCallerAdmitted`
and the opening of `executeCommand` from `System_Utils.cpp` up to and including
its admission refusal (closed with a "reached the handlers" return). The
executor queue, the executor task handle, the current task and the error log
are doubles. Handlers share the executor's task state (the validate mirror, the
debug and capture buffers, static result buffers), so:

- before the executor queue exists (the early-boot path of
  `submitAndExecuteSync`) any task is admitted;
- `cmd_exec` is admitted;
- any other task once the queue exists (httpd, an automation, a BLE callback)
  is refused: false, an `Error` result (terminated in a 6-byte buffer too) and
  one log line that names the task and never the command text;
- between creating the queue and the executor task, other tasks are refused,
  and a null task handle never matches a null executor handle.

A source check requires that nothing but clearing the output runs before the
refusal. A second source check covers the last direct caller off the executor:
`handleEspNowStatus` (`WebPage_ESPNow.cpp`) still opens with the guest-loadable
`WEB_AUTH_OR_RETURN`, submits each of its nine fixed status commands (eight
json replies, then the status text for display; same order, the page applies
results by slot) through `executeInternalStatusCommand`,
and the file has no `executeCommand` call. `--component <dir>` /
`--source-dir <dir>` read the production sources from another tree; the
pre-fix tree (41eecfe6) fails both parts. Not covered: the real FreeRTOS
scheduler and the status commands' own output.

## Settings schema setters and the bonded remote-edit policy

```sh
python3 components/hardwareone/test/host/test_settings_schema_setters.py --sanitize
```

`settings_schema_setters` compiles `buildSettingsSchemaJson`
(`System_Settings.cpp`) with the editor core's `settingsEditorCommandName` and
`settingsEditorHasCommand` (`System_SettingsEditorCore.cpp`) and the real
registry types (`System_Settings.h`) against a command-registry double, with
vendored ArduinoJson. A row carries `cmdKey` only when its setter is registered
in the build; a row whose setter is compiled out, and a row without one, is
listed without it (clients show it read-only instead of sending "Unknown
command"), and every other field is unchanged.

It also compiles `BondedPeer::remoteEditAllowed` and `filterSchemaForRemoteEdit`
(`System_BondedPeer.cpp`) and narrows a worker schema: `users`, `bond`, `wifi`
and a nameless module are removed, `clock` keeps only `ntpServer` and
`tzOffsetMinutes`, keyless rows go, what is kept is unchanged, module order and
other top-level fields stay, and `count` follows the module list. Source
checks: `GET /api/bond/settings/schema` (`WebPage_Bond.cpp`) serves the schema
from `BondedPeer::readRemoteEditableSchemaJson`, and `WebPage_Settings.h` keeps
no copy of the policy (`BONDED_MODULE_DENYLIST`, `BONDED_KEY_WHITELIST`) and
never derives a command from a key (bitmask fields, the dynamic and network
save loops, the debug panel). `--component <dir>` / `--source-dir <dir>` read
the production sources from another tree; the pre-fix tree fails the cmdKey
case, the policy case and every source check. Not covered: the page's
JavaScript at runtime (tools/webui's syntax test parses it) and the PSRAM
serialization in `readRemoteEditableSchemaJson`.

## STT result privacy and the targeted BLE reply

```sh
python3 components/hardwareone/test/host/test_stt_privacy.py --sanitize
```

`stt_privacy` splices `broadcastOutput(const String&, const CommandContext&)`
with its body `broadcastOutputRouted` (the route mask as an argument)
and `deliverCommandResult` from `HardwareOne.cpp` and `redactOutputForLog` from
`System_Utils.cpp`. It checks that a private STT result (`sttText`,
`transcriptPath`, QuickNote replies) reaches only the direct serial or BLE reply
and that every shared copy is redacted. A stale session gets nothing.

The BLE cases (D1) model the session-fenced sender: a reply is delivered only
while the connection still holds the expected session epoch. The checks:

- A result for the session on connection 0 reaches exactly that session.
  Bluedroid numbers connections from 0.
- A result for session epoch 7 never reaches the session that replaced it on
  the same connection id (epoch 9).
- A BLE-routed context without a session epoch reaches nobody directly.
- A BLE-routed context without a connection id, or with a non-numeric one,
  never falls back to the console-mirror sessions. The mirror would carry the
  unredacted original. Those sessions get only the queued, redacted copy.

`--source-dir <dir>` points the suite at another copy of the component. The
pre-fix tree fails the replacement-session, no-epoch, no-connection-id and
non-numeric-id cases. `--cxx` selects the compiler. ASan/UBSan follow
`--sanitize` / `HW1_SANITIZE`.

## BLE console mirror recipients

```sh
python3 components/hardwareone/test/host/test_ble_mirror_recipients.py --sanitize
```

`ble_mirror_recipients` splices `bleMirrorRecipientCount`,
`bleCollectMirrorSessions` and `BleSessionFence` from `BLE_Server.cpp` into a
four-slot connection table. The count uses the same collector as
`sendBleResponse`. The checks:

- Not counted: a peer that never logged in, a guest session, a slot whose
  session epoch was cleared, and a disconnected slot.
- Counted: logged-in non-guest sessions, including connection 0.
- Nothing is counted without the lifecycle lock.

Two source checks follow:

- The BLE branch of the debug drain in `System_Debug.cpp` buffers only while the
  count is non-zero. With only guest sessions logged in, it used to fill and
  drop its buffer for nothing.
- `outble` reports the count.

`--source-dir <dir>` points the suite at another copy of the component. The
pre-fix tree has no count. `--cxx` selects the compiler. ASan/UBSan follow
`--sanitize` / `HW1_SANITIZE`.

## C6 companion BLE shutdown

```sh
python3 components/hardwareone/test/host/test_companion_ble_shutdown.py --sanitize
```

`companion_ble_shutdown` compiles `shutDownBleForCompanionReset` from
`System_RadioCompanion.cpp` against a fake BLE owner layer. `deinitBluetooth()`
reports false while the Ring owner has not quiesced, and the server and its host
stay up. Once the Ring owner has quiesced, the call takes the host down itself.
A `bluetoothHalDeinit()` while the Ring owner is busy is recorded as a fault:
it is the use-after-free that the Ring prepare step exists to prevent. The
cases:

- K1: the Ring owner never quiesces. The stop is retried a bounded number of
  times (3), and the host is never forced down. The lifecycle fault keeps
  Bluetooth off until a reboot, and the reason is logged.
- K2: the Ring owner quiesces on the second attempt. The server's own stop takes
  the host down, and no fault is raised.
- K3: the server stops at once (unchanged).
- K4: a G2-client or bare-host teardown still runs the checked HAL teardown
  (unchanged).
- K5: a busy role claim touches nothing (unchanged).
- K6: an idle stack is left alone (unchanged).

`ble_server_stop` C1/C2 still pin the owner side: a Ring-deferred stop under
the RECOVERING claim closes session admission and advertising, and any other
claim keeps the server running. `--source-dir <dir>` points the suite at
another copy of the component. The pre-fix tree forces the host down in K1 and
does not retry in K2. `--cxx` selects the compiler. ASan/UBSan follow
`--sanitize` / `HW1_SANITIZE`.

## Retired BLE code

```sh
python3 components/hardwareone/test/host/test_ble_retired_code.py --sanitize
```

`ble_retired_code` checks that the dead BLE code stays retired.

BLE_IDF:

- `BLE_IDF.cpp` and `BLE_IDF.h` are absent, and the component source list does
  not name them.
- No component source includes the header or reads
  `ENABLE_BLE_IDF_EXPERIMENTAL`.
- `BLE_Types.h` no longer names `BLE_IDF.h`.
- No board or deployment feature header sets the flag.

debugringbridge:

- The flag's X-macro row, command row, settings row, `DEBUG_RING_BRIDGEF` macro
  and `gSettings.debugRingBridge` field are gone.
- Every other R1 ring flag keeps its bit, and bit 230 is not reused. A saved
  `log` flags mask can therefore still name bit 230 without turning on another
  flag.
- The non-G2 row subtraction in `System_Debug.cpp` equals the number of
  generated rows the G2 span gates.
- `readRegisteredSettings` still looks keys up by registry row. A `debug.json`
  that still carries `ring.bridge` therefore loads, and the key is dropped on
  the next save.

The flag table (`System_DebugFlags.h` with the `DebugFlagMask` struct from
`System_Debug.h`) is compiled on the host, so its `static_assert`s hold: row
count 127, unique bits, banks and parent links. The compiled table carries no
`RING_BRIDGE` row and no other row on bit 230.

`--source-dir <dir>` points the suite at another copy of the component. The
pre-fix tree fails the BLE_IDF and debugringbridge cases. `--cxx` selects the
compiler. ASan/UBSan follow `--sanitize` / `HW1_SANITIZE`.

## Module overview text

```sh
python3 components/hardwareone/test/host/test_module_overview_text.py
```

`docs/COMMAND_REFERENCE.md` is regenerated from the module descriptions in
`System_CommandRegistry.cpp`, so a stale description reverts a corrected
reference. `module_overview_text` parses the registry with
`tools/command_registry.py`, the same parser the reference uses, and checks
three overviews against their command rows:

- `bluetooth` gives `blesecret`, `blesecure` and `blerequireauth` their super
  admin tier, not "admin-only".
- `oled` names only registered commands, so the removed `oledrequireauth` is
  gone. It names the display login switch as `displayrequireauth` (super
  admin, Settings module).
- `ring` names `ring control <collection|lowpower> <preserve|on|off>`
  (admin), using the row's own syntax and tier. It names only registered
  commands and no longer claims that unregistered bridge commands "exist in
  the code".
- `ring`, `phone` and `relay` each name every verb row of their module, and no
  registered row or overview uses a retired name (`ringstatus`,
  `healthstatus`, `phonepipe`, `phonestatus`, `phonepipesecret`, ...).

`--source-dir <dir>` points the suite at another copy of the component, and
that repository's tool is used. The pre-fix tree fails all three overviews.

## Command-registry drift gate

```sh
python3 components/hardwareone/test/host/test_command_registry_gate.py
```

`command_registry_gate` runs the two release gates of
`tools/command_registry.py` and fails if either exits non-zero:

- `audit`: no duplicate command names, no unregistered or dangling
  `CommandEntry` arrays, no command string in code that resolves to nothing,
  every `SettingEntry` cmdKey binds to a command, and every reply's
  `Usage: <command>` line and `run/use/type '<command>'` hint names a
  registered command (or a word listed in `USAGE_NON_COMMAND_WORDS` with its
  reason, such as the UART link's in-band `dictate`). A table defined in
  mutually exclusive `#if`/`#else` branches of one file (the two
  `otaCommands[]` in `System_OTA.cpp`) counts once; the first branch is read.
- `reference --check`: `docs/COMMAND_REFERENCE.md` is byte-identical to a
  fresh render, so a registry or module-description change must be followed by
  `python3 tools/command_registry.py reference`.

On failure the gate's own report (the audit sections, or the drift summary
with the missing and stale command rows) is printed. `--repo <dir>` runs
another checkout's tool against its own sources; the pre-fix tree fails both
gates (13 audit problems; no `--check`).

## Tracked-tree hygiene gate

```sh
python3 components/hardwareone/test/host/test_tree_hygiene_gate.py
```

`tree_hygiene_gate` is the release gate for private details in the tracked
tree (RELEASING.md, gate step 4). It runs `git grep` over every tracked text
file except `components/hardwareone_libs` and fails on an account home path
(`/Users/<name>`) or a `192.168.x` address whose third octet is not in the
test's `ALLOWED_SUBNETS` table (generic examples, the SoftAP defaults, the
recovery updater's access point, test fixtures; each entry carries its
reason). Each hit is printed as `file:line: match`.

It does not look for a Wi-Fi network name or a password, because a tracked
test cannot name them. `--repo <dir>` checks another checkout; the tree before
this gate fails with one hit (a home subnet quoted in the readiness appendix).

## Markdown link check

```sh
python3 components/hardwareone/test/host/test_doc_links.py
```

`doc_links` reads every tracked Markdown file (not `components/hardwareone_libs`,
managed components or `experiments/`), skips fenced code, and fails on a
`[text](target)` link that is an absolute path (`/private/tmp/...`) or a
relative path with no tracked file or folder behind it. URLs and in-page
anchors are not checked. A file a tool makes (not tracked) is named in code
text, not linked. Readiness G-22; the tree before it had ten such links.

## Licence notice check

```sh
python3 components/hardwareone/test/host/test_notice_components.py
```

`notice_components` reads every `dependencies.lock*` and fails when a locked
registry component (`  <owner>/<name>:`) is not named in `NOTICE.md`, so a new
managed component cannot ship without its licence row. Readiness G-05/DELTA-6.

## Repository tool pins

```sh
python3 components/hardwareone/test/host/test_tool_pins.py
```

`tool_pins` holds two repository tools to the firmware they describe:

- `tools/command_registry.py`: the `audit` step's `CALL_RE` scans
  `oledRunCommand("...")`, and every such literal in the component resolves to
  a registered command.
- `tools/ble_secure_event_catalog_client.py`: `EXPECTED_FAMILY_COUNT`,
  `EXPECTED_KIND_COUNT` and `EXPECTED_CATALOG_BYTES` equal the checked-in
  fixture's real family count, kind count and compact byte length. The fixture
  itself is held to `System_EventCatalogRows.h` by `event_catalog_fixture`.
  The summary that the client's protocol test
  (`tools/ble_secure/test_secure_channel_v1.py`, run by
  `tools/test_ble_secure_event_catalog_client.sh`) asserts must match too.
  The constants are read with `ast` and the test's pins as text, so the
  client's BLE and crypto dependencies are not needed.

`--source-dir <dir>` points the suite at another copy of the component, and
that repository's tools are used. The pre-fix tree fails both tools: 27 OLED
literals escape the audit. The client pins 164 kinds and 2877 bytes, and its
protocol test pins 12 families, 152 kinds and 2877 bytes, against a fixture of
13 families, 165 kinds and 3149 bytes.

## OLED display-session writer windows

```sh
python3 components/hardwareone/test/host/test_oled_commit_window.py --sanitize
```

`oled_commit_window` compiles two pieces of `OLED_Utils.cpp`:

- the command-result path: `executeOledCommandWithResultImpl` with
  `oledDisplayEpochLiveUnderWriter`;
- the frame-commit fence: `oledFrameCommitAllowed` and `oledCommitFrame`.

The fakes model the display lifecycle writer as a mutex that another task's
publisher (a remote login, logout or replacement) has to wait for. They also
model the live-session registry, the boundary generation and the panel push. A
publication can be injected while the command runs, during the result copy and
during the push. It lands only when the writer is free, as on the device.

The checks:

- The result copy and the panel push run with the writer released, so neither
  holds a publication off.
- A replacement or logout that lands during the copy wipes the result and
  returns false. One that lands while the command runs gets no copy at all.
- A replacement or logout that lands during the push marks the frame dirty for
  an immediate redraw.
- A stale session pushes nothing and marks the frame dirty. That covers a
  pending boundary and a different epoch, user or auth state.
- The logged-out login screen (epoch zero) commits every frame without marking
  itself dirty again.
- With display auth off, the re-checks open no bypass session.
- An expected epoch that is no longer the display's is refused before anything
  is submitted.
- A source contract: `updateOledDisplay` commits through `oledCommitFrame()` and
  never pushes the panel itself.

The writer-window scan over `OLED_Utils.cpp` lives in `session_snapshot`. The
merged tree before this change (41eecfe6) fails both: the copy and the push
ran under the writer, and the fence helpers did not exist.
`--component <dir>` / `--source-dir <dir>` read the production file from
another tree.

    ctest --test-dir <build> -R '^oled_commit_window$' --output-on-failure

## OLED merge leftovers

```sh
python3 components/hardwareone/test/host/test_oled_merge_leftovers.py
```

`oled_merge_leftovers` is a source contract for the OLED leftovers that
neither review set could clear before the merge:

- `OLED_Mode_UnifiedMenu.cpp` (a placeholder comment) is gone, and the
  component `CMakeLists.txt` no longer lists it. The empty
  `displayMemoryStats() moved to ...` section of `System_MemoryMonitor.cpp` is
  gone too.
- `OLED_Utils.cpp` no longer has its `#if !ENABLE_OLED_DISPLAY` stubs of
  `oledBootModeActive` and `shouldBlockForDisplayAuth`. Each is defined once,
  inside the display build. Every firmware reference outside a declaration sits
  under `#if ENABLE_OLED_DISPLAY`, so a `DISPLAY_TYPE=0` build needs no stub.
  The `CMakeLists.txt` comments no longer give those stubs as the reason
  `OLED_Utils.cpp` is always compiled.
- `OLED_Mode_Network.cpp` credits `getTotalFailedLoginCount()` to
  `System_User`'s credential attempt table, which counts failures on every
  transport. Its `WebServer_Server.h` include comment names what the file
  still takes from that header.
- `System_Filesystem.cpp` registers `filerename` and `filedelete` without the
  admin flag. No comment in either file browser (`OLED_Mode_FileBrowser.cpp`,
  `G2_Page_Files.cpp`) still calls them admin-only.

It reads source text only. The merged tree before this change (41eecfe6)
fails every section. `--component <dir>` reads another tree.

    ctest --test-dir <build> -R '^oled_merge_leftovers$' --output-on-failure

## ESP-NOW radio ownership

```sh
python3 components/hardwareone/test/host/test_espnow_radio_ownership.py --sanitize
```

`deinitEspNow` clears `initialized` before its bounded waits. A close that
times out keeps `espnow_task`, `espnow_tx` and the esp_now driver alive with
only the close flag set. `espnowOwnsRadio()` (System_ESPNow) is the single
answer to "does ESP-NOW hold the radio": initialized, or a close still pending.
When ESP-NOW is compiled out, an inline stub in `System_ESPNow.h` returns false.
`isEspNowInitialized()` keeps its meaning.

`espnow_radio_ownership` compiles the production `espnowOwnsRadio`,
`isEspNowInitialized` and `espnowShutdownPending` against a scripted open,
close and timed-out close, and runs the disabled-build stub from the header. A
source contract checks that the five System_WiFi radio decisions ask the
predicate and keep no local `extern` for it: the connect hunt's mode,
`closewifi`'s power-down, `radiopower off`, the targeted connect's driver
restart and the lazy Wi-Fi init. `radiopower`'s restore snapshot deliberately
records `isEspNowInitialized()`, because a pending close is not something to
restart. `wifi_connect_budget` runs the connect behaviour.

`--component <dir>` reads another tree. The pre-fix tree fails the contract
(no header include, decisions made from `isEspNowInitialized()`); a pre-fix
`System_ESPNow.cpp` fails because the predicate is missing.

## Wi-Fi radio state without driver calls

```sh
python3 components/hardwareone/test/host/test_wifi_radio_state.py --sanitize
```

On the P4X-EYE the Wi-Fi driver runs on the ESP32-C6, so every `esp_wifi_*`
query is an RPC, and one against a deinitialized driver logs on both chips.
After `radiopower off` the OLED Radio row and the Network and System screens
read the radio state every frame, about eight failed `Req_GetWifiMode` RPCs a
second. `wifiRadioState()` now asks Arduino's `WiFi.getMode()`, which answers
NULL from its own init/start flags once the radio is stopped; every radio start
and stop in this firmware goes through `WiFi.mode()`. Per-frame screens use
`wifiRadioDisplayState()`/`wifiRadioDisplayOn()`, which reuse the driver's
answer for up to a second while the Arduino interfaces are unchanged.

`wifi_radio_state` compiles those functions unchanged together with Arduino's
own `WiFiGenericClass::getMode` (from `components/arduino`) against a fake
driver that counts calls: none while the radio was never started, after
`radiopower off` or while associated; ESP-NOW's AP_STA hold, its close and
`radiopower on` report correctly; the display read asks the driver once a
second, and at once after `WiFi.mode()` enables or disables an interface; a
driver that lost its state behind Arduino shows within a second. A source
contract keeps the quick-menu Radio row, `prepareNetworkData` and
`prepareSystemStatusData` on the display read, and the two screens' SSID/RSSI
reads behind an `everyMs` gate. `--component <dir>` and `--arduino <dir>` read
other trees; the previous `esp_wifi_get_mode` body fails the first check.

## Camera asynchronous power

```sh
python3 components/hardwareone/test/host/test_camera_async_power.py --sanitize
```

UI adapters submit `opencamera async` / `closecamera async`
(System_Camera_DVP.cpp) through the command executor. The registry authorizes
them, the live-session fence applies and the `[CMD]` audit records them. No
saved setting refuses a start of either form (`cameraautostart` only decides
boot). Both report
only whether cam_power admitted the request and never wait for the camera.

`camera_async_power` compiles the production `cmd_camerastart` /
`cmd_camerastop` and their helpers against power-request doubles. It checks:

- the asynchronous forms (case and spaces ignored);
- the disabled-camera refusal, with no request made;
- an admission failure reported as an error;
- the validate pass, which requests nothing;
- the synchronous forms, unchanged (60 s / 30 s waits, the same replies).

A source contract checks that `G2_Page_Sensors.cpp` requests no camera power
itself. `g2_sensors_redraw` runs the page flow. The pre-fix sources fail
because `opencamera async` started the camera synchronously and the page called
`cameraPowerRequestStartAsync` on the tap task.

## Structured Add-WiFi

```sh
python3 components/hardwareone/test/host/test_wifi_structured_add.py --sanitize
```

The OLED and G2 Add-WiFi forms hand ssid, password, priority and hidden to the
saved-network owner's `wifiAddCommandLine` (network/wifi/System_WiFiNetworks.cpp).
It refuses a field the quoted line cannot carry (a `"` or a control character),
reporting the first such byte, and leaves the line empty. Otherwise it writes
the one `wifiadd` line the form submits through its session-fenced executor
path: the two-argument form when no priority or hidden flag is given,
`<priority> <0|1>` otherwise. The registry's admin row, cmd_exec and the audit's
password mask then apply as for any caller.

`wifi_structured_add` compiles three production pieces unchanged: the
`CommandArgs` tokenizer, `cmd_wifiadd` and the owner's field rule and line
writer. The store behind `cmd_wifiadd` is a recording double; the owner's own
refusals run in `wifi_networks`. The checks:

- Every written line parses back to exactly the fields it was given. The cases
  cover names with spaces, edge spaces, backslashes and `'`, an empty password,
  and a 32-character name with a 64-character password.
- A refused field writes nothing.
- When the store refuses a control character, `cmd_wifiadd` says so instead of
  reporting a full list. The quote and full-list replies are unchanged.

`--component <dir>` reads another tree. The pre-fix sources fail the
control-character reply.

## remote:/@ commands and the bonded peer's authority

```sh
python3 components/hardwareone/test/host/test_remote_command_authority.py --sanitize
```

The bonded peer runs every `remote:`/`@` command as bond-admin, which is super
for the live bond session. The sender can check the unwrapped line only
against its own registry flags. Handlers that decide access at run time from
the caller (file path rules, the owner of notes and transcripts, sessions,
voice arming) would decide it for super on the peer. A command the sender's
build does not compile has no row to check at all. So the routing in
`executeCommand` needs a local administrator. The one exception is the peer
sensor start/stop lines (`open<sensor>` / `close<sensor>`, no arguments), which
the bond page and the OLED bond screen send for any signed-in user.

`remote_command_authority` compiles, unchanged, the remote routing of
`executeCommand` (from the "REMOTE COMMAND ROUTING" marker through its
`if (isRemoteCommand)` block), `authorizeCommand` with its guest,
password-change and session classifiers, `authorizeRemoteCommand` and
`remoteCommandAllowedWithoutAdmin`, and the remote reply owner
(`formatRemoteCommandSent` / `parseRemoteCommandSentReqId`) from
`System_Utils.cpp`. It also compiles `CommandArgs` (`System_Command.cpp`) and
the registry name matcher (`System_CommandLookupCore.h`). Accounts, registry
rows (with the shipping flags, file verbs user-level), redaction, the audit
and event sinks and the bond transport are doubles. It checks that:

- a signed-in user cannot send the file verbs (`files`, `fileread`,
  `fileview`, `filewrite`, `filecreate`, `mkdir`, `rmdir`, `filedelete`,
  `filerename`), `sensorlog`, `opensr`, `whoami`, a setter, or a command this
  build lacks, over any transport. The same holds for an automation stored by
  a user, an ownerless automation row, the require-auth-off identity, an
  anonymous pre-auth line and a guest's `whoami`. Each refusal sends nothing.
  The reply is "Error: Admin access required ..." and there is one failed audit
  entry with the redacted line ("remote cmd=...") and one denial event for a
  named caller;
- the sensor toggles stay open to those callers. The ids are read from
  `WebPage_Bond.cpp`'s remote sensor table and `OLED_Mode_Remote.cpp`'s
  `bondSensorDefs`. Lines with an argument, a quoted verb or another verb are
  refused;
- administrators, the superadmin and the trusted internal identity keep the
  remote surface: one `ESPNOW_V4_TYPE_CMD` frame with `ACK_REQ` carrying the
  unwrapped line, the reply "Remote command sent: <redacted> (reqId <msgId>)",
  which the reply owner parses back (A13), and one redacted broadcast;
- the guest gate, the registry's own admin and super refusals, the
  nested-wrapper refusal (first, for everyone), the bond-readiness errors and
  local lines are unchanged;
- `guest_read_only_surface` (owner decision B6): a guest may run, besides
  `login`/`logout`/`whoami`, exactly the read-only status lines the web pages
  load and poll (`espnowstatus [json]`, `espnowmeshstatus`, `espnowlist`,
  `espnowdeviceinfo [json]`, `espnowtoporesults [page] [id]`,
  `bondstatus [json]`, bare `g2nav`/`g2verbose`, `eistatus`, `eimodellist`,
  `srtuning`). Any other argument, any other command, an admin row and a
  wrapped line get `kCommandReplyGuestViewOnly` with one audit entry and one
  denial event; a user's lines are judged by the rows as before.

A source check confirms that `executeCommand`'s routing is the only caller of
`buildBondedCommandPayload`, so no second sender bypasses the gate.
`--component <dir>` / `--source-dir <dir>` read the production sources from
another tree. The pre-fix tree (af1ed97c) fails the user, unknown-command,
guest-and-unknown-account and redaction cases. Not covered: the peer's
`v4_handle_cmd`, the radio, and the bonded-only FS RPC (`espnow_fs_transcript_privacy`).

## Guest view-only web (owner decision B6)

```sh
python3 components/hardwareone/test/host/test_web_guest_gate.py
python3 components/hardwareone/test/host/test_web_guest_view_browser.py
```

`web_guest_gate` compiles, unchanged, the guest gate of `WebServer_Utils.cpp`
(`webGuestSendForbidden`, `webGuestApiAllowed`, `webResolveGuestRole`,
`webGuestMutationAllowed`, `webGuestAccessAllowed`) over a fake request and
roster. For a guest every HTML page opens (`/cli` answers with its own
view-only notice), the read-only status GETs pass, and `POST /api/cli` passes
so the command guest surface decides. Every other POST (`/api/cli/batch`
included) and the file, log, capture and admin GETs get one 403 whose JSON
carries `code:"guest_forbidden"` and the sentence a page shows
(`error:"Guest accounts are view-only"`). An unresolvable account is refused;
an administrator is never stopped by the gate, a user only by the admin-only
reads (below).

`web_guest_view_browser` runs the two shipping helper scripts every signed-in
page gets (the role/theme script with `hw.applyGuestViewOnly`, the fetch
helpers with `hw._cliRefusal`) against a fake DOM and fetch, under Node.js or,
without it, macOS `jsc` (exit 77, skipped, with neither). A guest sees every
`[data-guest-hide]` control; a refused press (the command line's guest reply,
or the gate's refusal of a POST) is one refusal popup with the reason, not raw
JSON; a refused page load (GET) is no popup and rejects with the sentence and
403 for the page's own state; a non-guest 403 on a non-command POST and a
`hwQuietRefusal` read are not popups. `--component <dir>` reads another tree;
the pre-B6 sources fail both suites.

### Guest privacy (owner, 2026-11-22)

```sh
python3 components/hardwareone/test/host/test_web_guest_settings_view.py
python3 components/hardwareone/test/host/test_web_guest_private_pages.py
```

`web_guest_gate` also covers the private reads: for a guest the ESP-NOW
message history, the ring's health status and the LLM chat (turns and the
streamed answer) get a 403 with `code:"guest_private"` and
`error:"Not available to guests"`; the message history is admin-only, so a
user gets a 403 with `code:"admin_required"` there (and only there), and an
administrator reads it.

`web_guest_settings_view` compiles `guestview::reduceSettingsForGuest`
(`WebServer_GuestSettingsView.h`) with the vendored ArduinoJson: the guest's
`/api/settings` view drops `network.wifi.networks` and shows
`(not shown to guests)` for the MQTT host and user name and the LLM gateway
URL, leaving every other value; an empty value stays empty. Source checks pin
the table's paths to the settings modules' sections and groups, the guest-only
call in `handleSettingsGet`, and the MQTT page's guest-aware, escaped host and
user cells.

`web_guest_private_pages` runs the R1 Health page script and the ESP-NOW
message poll on the shipping fetch helpers against a fake DOM and fetch (Node.js
or `jsc`, 77 without): a guest's health page says "Health data is not available
to guests." and starts no poll; the conversation logs say the history is not a
guest's (or is for administrators only, for a user), polling stops, and the
panels drawn later read the same sentence; an administrator's pages render and
keep polling. The pre-change page scripts fail it.

## Continuous STT trim default

`stt_continuous` (System_STT.cpp's continuous broker) now runs the production
default: a phrase that closes on a pause is transcribed up to its voiced end
plus 500 ms (`kTrimTailSamples`), with speculative finals; the harness takes
every tail expectation from that constant. `stt_continuous_full_phrase` builds
the `-DHW1_STT_TRIM_SILENCE=0` opt-out and checks that full phrases are kept.
`--speakers` adds the speaker-label path; `--trim-silence` is an alias of the
default.

## STT raw-input recorder and replay

These cover the `sttraw` bench tap (System_STTRawCapture; see
`tools/stt_raw/README.md`).
- `stt_raw_ring_tests` runs the production `stt/stt_raw_ring.h`. It checks SPSC order across
  wrap-around, whole-block drops recorded as coalesced gaps in session samples, the gap-list
  cap, and invalid capacities. It also runs a two-thread producer/consumer check; run it under
  ThreadSanitizer by hand with `-fsanitize=thread`.
- `stt_raw_replay` builds the replay CLI and checks it on synthetic noise with one
  ordinary-level and one quiet phrase:
  - the production floors and continuous settings come from `stt/stt_continuous_config.h`;
  - segments are the same at any push chunk size;
  - lowering both floors admits the quiet phrase;
  - `--cut-dir` output is byte-exact;
  - the sidecar rate and drop count are checked.
The firmware tap itself (file writes, drain task) is not host-tested; the
broker calls it only under `ESP_PLATFORM`.

## Boot temporary-file cleanup

`boot_temp_cleanup` executes the production startup sweep, `VFS::DirectoryReader`
and `FsLockGuard` with real Arduino `String`. Controlled file, semaphore and log
boundaries cover rooted/leaf backend names, child close before removal, exact
directory scope (`/`, `/system`, `/system/users`, `user_settings`, `/system/espnow`,
`/system/certs`, `/maps`, and `/system/llm` when built), LLM enabled/disabled, nested caller lock ownership, failed opens
and removals, and accurate success/failure diagnostics. Directories, virtual
entries, `.header.tmp`, backups and out-of-scope children must survive the sweep.
It also checks the shared `VFS::isStagingName` predicate the track pickers use
to skip `.tmp`, `.header.tmp` and `.bak` names.

```sh
python3 components/hardwareone/test/host/test_boot_temp_cleanup.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^boot_temp_cleanup$' --output-on-failure
```

The optional `--baseline <saved-component-directory>` compares ordinary
leaf-name deletion results and characterizes the old path, header-preservation,
success-accounting and traversal-lock defects. `--evidence <directory>` retains
generated sources and reports. These checks do not qualify physical storage,
power-loss durability or the recovery policies owned by individual writers.

## Sensor and battery row writes

`log_row_writes` executes the production shared LF-row writer, sensor append
block and header writers, battery append/tick/event functions, battery status
formatting and filesystem lock guard. Arduino `String`/`Print` and the battery
state types are real; file storage, semaphore ownership, clock and sealing are
controlled boundaries. Checks cover exact row/header bytes, plain and sealed
rows, trusted file identities, accepted-byte accounting, successful-write
counters, failure-only size resynchronization, rotation after partial writes,
close/unlock ordering, nested lock ownership and battery retry cadence.
Fresh or empty header-bearing files are staged before receiving rows; the sensor
opener never creates the destination itself (an empty existing one is removed), so a
new file appears only when its staged header is promoted. The real
`VFS::replaceFileGuarded` is compiled over the suite's fakes, whose `/sd/` rename refuses
an existing destination as FAT does; the SD cases show that a new log is promoted by one
rename, that no backup is created and that a foreign `.bak` beside an absent destination
neither blocks nor is consumed. Header
faults cover short writes, reported Print errors, post-flush size mismatch,
failed staging/promotion, append-reopen failure and retries. A leftover `.header.tmp`
beside an empty destination, including one a failed cleanup left behind, is replaced by a
freshly staged header, also after a simulated runtime reset. Sealed magic/schema lines remain
complete before sealed data is appended. Existing empty battery files receive
their header; nonempty legacy files retain the previous append policy.
With the filesystem lock held by another task the sensor and battery writers
skip the lap and write on a later one; a waiting row or sample counts once in
`sensorlog status` and `batterylog status`, not once per lap.

```sh
python3 components/hardwareone/test/host/test_log_row_writes.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^log_row_writes$' --output-on-failure
```

The optional `--baseline <saved-component-directory>` compares unchanged file
contents and rotation, and characterizes former success accounting and nested
lock-release faults separately. Sensor snapshot acquisition, row formatting and
sampling cadence precede the extracted append block and are outside this suite.
`gpslog_start` separately checks that header-preparation refusal prevents
session publication and settings persistence. Replacement itself is a boundary
in the row suite; `capture_stitching` exercises its production implementation.
These checks do not qualify physical storage, SD, cryptographic validity,
scheduling races or durability.

## R1 history persistence

`r1_history_store` compiles the complete production store, public types and
filesystem guard with real Arduino `String`/`Print`. VFS, semaphores, crypto and
time are controlled boundaries. It checks plain/sealed sparse and dense records,
real serialization/parser round trips, unchanged-content skips, retry timing,
generation acknowledgement, interrupted-swap recovery and nested lock ownership.
Short writes, reported Print errors, final-size mismatch, key/sealing failures,
open failures and failed promotion/rollback must preserve the previous primary
or recovery backup and leave the new generation pending. The store's own
temporary writes run under its private I/O mutex without the shared filesystem
guard, while every publish or recovery rename happens inside one guard that
spans its sequence, so other filesystem users wait only for those spans.
A sweep commit started from the main loop while another task holds the
filesystem lock is put off to a later lap and counted (`historyStoreDeferrals`).

```sh
python3 components/hardwareone/test/host/test_r1_history_store.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^r1_history_store$' --output-on-failure
```

The optional `--baseline <saved-component-directory>` compares normal paths and
stored bytes and characterizes prior unchecked completion and lock boundaries.
`--evidence <directory>` retains generated sources and results. Crypto is a test
double; these checks do not validate encryption, physical flash, concurrent task
scheduling or power-loss durability.

## WAV recording completion

`mic_wav_completion` executes the production WAV header writer and recording
task's complete finalization/publication tail. File I/O, locking, disposition
sealing, live-audio delivery and external callbacks are observed boundaries.
Checks cover retained PCM size/rate, header patching, manual/owned recordings,
discard requests, cleanup failure and G2 source teardown. A short or oversized
WAV, or a reported Print error present after flush, must follow the existing
failure path: no saved event or successful live-audio outcome, and correct
cleanup/result publication after close.

```sh
python3 components/hardwareone/test/host/test_mic_wav_completion.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^mic_wav_completion$' --output-on-failure
```

The optional `--baseline <saved-component-directory>` compares unchanged file
bytes and lifecycle events and demonstrates the old false-success cases.
`--evidence <directory>` retains generated sources and results. Arduino File's
void flush/close APIs do not expose every backend failure; this suite does not
establish physical storage durability, microphone capture quality or task-race
behavior.

## G2 mic WAV power-cut checkpoint

`g2_mic_wav_checkpoint` compiles the production `g2micwav` packet writer,
checkpoint, header patch and close from `G2_Glasses.cpp` over a file double
whose `flush()`/`close()` decide what survives a power cut. Captures are cut
at random points (300 per seed, five seeds): the surviving file must parse as a
16 kHz mono WAV whose RIFF and data sizes match the bytes on disk and hold every
packet up to the last checkpoint (at most one interval lost). It also cuts
between the two syncs of a checkpoint (the header never claims more than the
disk holds), checks a normal stop, a LittleFS path (sync only, no header
rewrite) and a failed seek back (the capture stops instead of overwriting
data). Once per run it also writes a capture to the 16 MB cap: the cap closes
the file inside the packet write, and the FS-lock double fails the run if the
BLE notify path ever waits for the FS lock. Source checks cover the header sync
at start, the SD choice made on the normalized path (as the open routes it) and
no close in the notify path after the write. `--source <G2_Glasses.cpp>` runs
the harness against another copy; the pre-fix file fails. Real SD durability
and the BLE-task stall need a device.

```sh
python3 components/hardwareone/test/host/test_g2_mic_wav_checkpoint.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^g2_mic_wav_checkpoint$' --output-on-failure
```

## Automation logging

`automation_log` compiles the production logging owner, `autolog` command,
argument parser, filesystem guard, output redactor and command-executor logging
hooks. Boundary doubles exercise captured user/scope permissions, revocation,
overflow paths, allocation failures, incomplete writes, failed replacement and
stop reporting. Controlled overlapping writes/replacements/stops and threaded
stress check that destinations and identities stay paired; status remains
available during blocked I/O. Replacing an active session writes the new
LOG_START first, then a LOG_STOP naming the new file to the displaced one, and
the reply names it. The real disabled header is compiled separately
without VFS, authentication or RTOS headers.

```sh
python3 components/hardwareone/test/host/test_automation_log.py --sanitize
# Separate run: ThreadSanitizer cannot be combined with ASan.
python3 components/hardwareone/test/host/test_automation_log.py --thread-sanitize
```

These checks exercise firmware logic, not physical storage reliability. The
permission and storage boundaries are controlled substitutes; existing VFS,
redaction and command-authorization suites qualify their own implementations.

## Automation scheduler

`automation_scheduler` compiles the production scheduler lifecycle, main-task
poll, cache rebuild and condition backoff with the vendored ArduinoJson parser.
It checks startup migration and failures, stopped polling, empty/malformed
stores (G-38: a file that does not parse is reported once per episode, one
system log line and one `config_file_corrupt` event however many rebuilds see
it, the file stays in place, and a good parse ends the episode), clock due times, event throttling, safety polling and millisecond wrap.
Controlled overlap verifies that refreshes during a rebuild survive and that
stopping during a read cannot restore a freed cache. Failed file writes leave
the cached schedule intact. Threaded polling, notifications, event posts and
start/stop exercise cache lifetime under ThreadSanitizer. No saved setting
stops a running scheduler: only `automation system disable` does, and
`automation system enable` resumes it. A newer automations.json format (`"version"` above
`kAutomationsFileVersion`) is refused: nothing is cached or listed, the store is
never written and keeps its text, and the add command's load
(`loadAutomationsStoreForEdit`) refuses without seeding an empty store over it;
no version, version 1 and a non-numeric version load as before, and a missing
file is seeded (written only when not validating). Without a set clock interval
and armed-delay triggers count uptime (`AutomationUptimeSchedule`, case
`uptimeSchedule`): an interval saved with a 2026 next run is first due one
interval after automations start and then every interval, a fire writes nothing
(long intervals included) and a millis() wrap changes nothing; an armed delay
lives in RAM and its fire disarms it; the table has 32 entries, beyond which an
interval waits for a set clock and the cache never calls it due; the client view
shows no next run (null) for these triggers. When the clock is set, one pass
carries over what replaced the file, with the time it had left: an armed delay,
and an interval that already ran on uptime (a short one to the RAM schedule, the
rest in one write), so nothing is repeated, made up or reported missed. An
interval still in its first countdown is dropped without a write (when nothing carries, the file is not even read):
a clock that arrives 5 s after boot leaves a future file next run as it is, and
a past one goes through the missed-run rule as without the uptime schedule.
Adding, editing or deleting an automation drops its uptime entries
(`automationUptimeForget`): an armed delay does not survive an edit, an interval
starts again. The cases written before the uptime schedule count small numbers
as a set clock (`clockForcedSet`); the clock and uptime cases ask
`Clock::isValidEpoch` itself. The actual disabled
public header is compiled independently. `automation_scheduler_contract`
retains the main-loop integration and full-tick early-exit/source guards, plus
the quiet in-pass reschedule writes, the unconditional main-loop tick, that the
feature registry no longer calls the owner transition (`features` only lists),
and that every owner read
goes through the format check and no writer spells the version as a literal.
With comments removed, the add command loads only through
`loadAutomationsStoreForEdit` and returns on its refusal, and the web
Automations page and both exports read through `readAutomationsJson` and name
a refused format. It also guards the pass's uptime `now`, the carry-over before
the file read, the silent advance of a next run from before the clock was set,
the manual run's `automationScheduleNow()`, `automation trigger` arming on
uptime, the boot pass saving only with a set clock, the cache recording and
comparing its base, and `automation add` / `automation delete` calling
`automationUptimeForget` after their write.

```sh
python3 components/hardwareone/test/host/test_automation_scheduler.py --sanitize
# Separate run: ThreadSanitizer cannot be combined with ASan.
python3 components/hardwareone/test/host/test_automation_scheduler.py --thread-sanitize
```

The full rule/command execution pass is substituted with a counter and the real
cache rebuild. These tests qualify scheduler ownership and polling, not every
trigger language, command side effect or physical filesystem operation.

## Host configuration tool

`saved_build_config`, `build_config_workflow`, and `build_config_wizard`
exercise the production configuration helpers and terminal flow with temporary
configuration files. They cover inheritance, compiler verification, isolated
build inputs, atomic saves, competing edits, cancellation, and build delegation.
The wizard tests substitute user input and build execution; they do not flash
hardware or claim that every selectable feature combination fits. Its
`BoardFirstChecklist` cases run the board-first flow against copies of the real
board and SDK defaults files: plain names, prerequisites, a change the header
refuses, a feature-level group, the Bluetooth SDK stack and a multi-value choice.

`build_config_catalog` reads this checkout as it is. Every editable switch has
exactly one entry in `tools/build_config_catalog.json` (an option, part of a
multi-value choice, or a managed level) and every board is described; each saved
configuration has a title and a summary and is accepted by
`System_BuildConfig.h`; every catalogued item is on, or can be switched on, for
at least one board; and a board default the header rejects (the bare Waveshare
selection) can be repaired from the checklist. The header is evaluated by the
host C++ preprocessor (`tools/build_config_resolve.py`); those cases are skipped
without one. It does not build firmware, so image size and SDK settings that
only Kconfig resolves stay with the real build.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^(saved_build_config|build_config_workflow|build_config_wizard|build_config_catalog)$' --output-on-failure
```

## Network build selection and disabled ESP-NOW

`network_build_selection` compiles the real feature header and evaluates the
component CMake through registration in both early requirement discovery and
configured phases. It covers manual edits, board defaults, named/deployment
overrides, custom network choices and restore without the web UI. Enabled
implementations and SDK dependencies must reach the compiler; guarded-out
sources may stay in the build graph.

`espnow_disabled_interface` compiles the production ESP-NOW header and disabled
source with Wi-Fi on and off. Controlled driver calls verify error propagation,
invalid arguments and unavailable outputs. The unchanged enabled MAC cache
functions are also exercised so Hosted-radio cache reads retain their behavior.
This suite uses ASan/UBSan when `HW1_SANITIZE=ON`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^(network_build_selection|espnow_disabled_interface)$' --output-on-failure
```

These checks do not establish device connectivity or restore/reboot behavior.

## Display lifecycle

`display_lifecycle` compiles the complete production `HAL_Display.cpp` and its
header against controlled I2C, SSD1306/P4 driver and GPIO boundaries. It checks
initialization failure/retry, both OLED addresses on both buses, initialization
without restarting Wire, retained canvas lifetime, latched bus routing, bus-lock
refusal, brightness/dim/power transactions, and the power-rail sleep/wake order.
Failed wake must remain unavailable until a successful retry; manual retry uses
the same charge-pump restoration as wake, and that restoration pushes a blank
frame before the panel is switched on, as stop blanks it before switching off.
Repeated sleep/resume calls are safe, a sleep cycle after a failed wake still
cuts the rail, stopping while asleep (or after giving up a failed wake) cancels
automatic resume so a later wake lights nothing, and disabled builds expose
compatible stubs.
ASan/UBSan follow `HW1_SANITIZE`.

The separate `p4eye_display_lifecycle` target runs the actual P4 backend with
SDK boundaries substituted, including DMA cleanup and canvas retention. Neither
test establishes OLED presence, visual output, electrical bus timing or physical
power-rail behavior; those require attached display hardware.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^(display_lifecycle|p4eye_display_lifecycle|oled_without_input)$' --output-on-failure
```

## Checked JSON settings saves

The persistence suites in this and the following sections each carry their own
File/VFS and lock doubles, tuned to their writer. Before adding another
persistence suite, move them into one shared fake VFS with backend policy knobs
(directory unlink refused like LittleFS, rename over an existing `/sd` file,
guarded versus unguarded seams) instead of writing a tenth double.

`settings_file_writes` executes the production main/debug/restamp writers,
shared JSON snapshot writer, per-user save/load/transaction/merge functions and
filesystem/settings writer guards. It uses vendored ArduinoJson with controlled
File/VFS/stat, Arduino String, semaphore, logging and cache-invalidation
boundaries. Bonded mode is compiled both enabled and disabled.

Fault cases cover incomplete serialization, observed size mismatch, reported
write error, open/directory failures, failed rename and failed temporary cleanup.
Per-user failures must preserve the previous file, invalidate the preference
cache exactly once, keep an outer transaction lock held and never acquire the
main settings writer lock or emit its failure events. Retries, absent files,
unavailable storage, JSON overflow, rejected mutations and preservation of
unrelated fields are included. Existing main/debug diagnostics, retry behavior
and overlapping restamp/main-save checks remain covered. A failed version
re-stamp logs its own line and raises none of the full writers' failure
reporting; a complete main save clears a pending re-stamp; and a main save
refused because the existing file cannot be opened retains the main file's
retry intent.

```sh
python3 components/hardwareone/test/host/test_settings_file_writes.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^(settings_file_writes|user_settings_transaction_guards|initial_admin_setup|notification_integration_guards)$' \
  --output-on-failure
```

`--baseline <saved-System_Settings.cpp> --case user` characterizes the former
destructive fallback and ignored write errors. `--case user-parity` emits normal
save byte counts/hashes for comparison between baseline and current sources.
These tests verify observable API behavior, not physical flash durability,
the real notification cache or multi-file account-creation transactions.

## Untrusted web file responses

`web_file_response_policy` compiles the production CSP/header helper and the
policy guards of both file read/view handlers with mock HTTP header storage.
It checks both sandbox variants, MIME/cache policy, and aborting on failure of
each header insertion before reaching file streaming, including polling cleanup.
Source guards cover placement before all format/raw branches and prevent branch
overrides. It does not execute firmware streaming or emulate browser security.

All file responses disable scripts/forms. SVG documents additionally get an
opaque origin. Escaped text and inert media retain their origin for authenticated
viewer links and native playback; this does not enable scripts. The SVG decision
uses the same decoded path suffix as the image MIME branch, including mode=raw.

For real browser checks against the compiled production headers, run:

```sh
python3 -B components/hardwareone/test/host/test_web_file_response_policy.py --serve-fixture
```

Open the printed loopback URL. The unprotected baseline must execute its SVG
script and record authenticated synthetic GET/POST/event probes. Protected SVG
and raw SVG must retain their static picture and produce no probes, including
after clicking the event-handler button. Check escaped text and its authenticated
navigation link (must report True), native audio, and binary downloads. The
home page's script and fetch of protected plain
text must still work. The fixture uses only a synthetic cookie and local data;
it does not contact a device. Browser checks are manual and are not part of
CTest. SVGs requiring scripts or external resources intentionally lose those
features; static inline styling and embedded data images remain allowed.

## Web batch response buffering

`web_batch_handlers` extracts and compiles the production local/bonded HTTP
batch handlers, settings marker helpers, and settings cleanup class on every
run. It uses the vendored ArduinoJson implementation and its Arduino String
adapter, with host mocks for String storage, HTTP, command execution, session
identity, and allocation. The `.cpp` harness is a template for the Python runner,
not a separately compiled copy of the handlers. ASan/UBSan follow `HW1_SANITIZE`.

Coverage includes reply ownership/escaping, empty slots and counts, ordinary
command errors, confirmations, settings finalization, session invalidation,
server shutdown, and response-document allocation failures. Failed buffering
must not skip command execution/cleanup or expose partial results as success.
The final `String` serialization path is deliberately unchanged by this
refactor. These tests do not measure physical PSRAM placement, internal-heap
savings, real executor races, or final-String allocation failure on hardware.

## Filesystem and G2 PSRAM output

`image_writes` exercises the production image-save routine with filesystem,
capacity, event and retention boundaries. It checks complete and failed writes
on each tier, independent BOTH attempts, admission failures, matching the
configured path to its requested storage, guarded cleanup and success-only
notifications. Saves stage a sibling temporary, preserving an existing
same-timestamp image until verified promotion. Cases cover failed promotion,
failed cleanup, unresolved temporary refusal, retained SD recovery backups and
independent BOTH-tier outcomes. Replacement is a service boundary here;
`capture_stitching` exercises the actual shared replacement implementation.
The capture-folder validator refuses a tier-prefixed (`/sd...`) or traversing
folder with a reply that names the setting and `cameraStorageLocation`, and
accepts plain folders. Folder preparation sweeps `.jpg.tmp`/`.jpeg.tmp`
leftovers once per tier per mount (LittleFS at manager init, SD at first use)
through the shared directory reader, leaves other temporaries alone, and
sweeps again only after a remount; the harness prepares both tiers before
observing a save, as a device has by the time it captures.
The `--baseline <component-directory>` option accepts saved sources;
`--case collision` reproduces the old destructive overwrite as an expected test
failure. ASan/UBSan follow `HW1_SANITIZE`. These checks do not qualify a physical
SD card, camera capture or power-loss durability.

```sh
python3 components/hardwareone/test/host/test_image_writes.py --sanitize
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^image_writes$' --output-on-failure
```

`image_storage` runs the actual ImageManager inventory methods, VFS routing,
directory reader and filesystem guard. It covers SD/internal folder creation,
JPEG listing and retention metadata, path routing, availability and operation
failures, nested locking and handle cleanup. The shared reader's timestamp
option is exercised by SD images; existing listing suites reject timestamp
calls with the default option, preserving their previous I/O cost. The
`--baseline <component-directory>` option accepts saved sources for regressions.
ASan/UBSan follow `HW1_SANITIZE`. These are filesystem-boundary tests, not
camera capture, physical-card or power-loss qualification.

`storage_stats` runs the actual VFS sampler, fresh/snapshot APIs, routing and
shared web/CLI JSON builder with the production cache core and filesystem
guard. It covers backend errors (including nonzero outputs on error), zero
capacity, unchanged caller outputs on failure, freshness/cache integration,
generation invalidation, separate storage tiers, 64-bit SD capacity and JSON
responses. It also runs the log writer's free-space check
(`refreshLittleFsFreeCached`), whose walk seeds the internal snapshot that status
polls read at any age (`VFS::kSnapshotAnyAgeMs`): seeding, no walk on a status
read once a reading exists, no seeding from a failed, zero-capacity or unmounted
walk, and no publication across an invalidation during the walk.
`--baseline <component-directory>` replays regressions against
saved sources. `vfs_capacity_cache_tests` and `vfs_capacity_snapshot_contract`
retain expiry, clock-wrap, invalidation and consumer-boundary checks. Source
rules pin that `buildSystemInfoJson` leaves the three storage figures out
without a reading and that the dashboard row then reads "unavailable".

`filedelete_storage_figure` runs the production `doFiledelete` (the routine
behind `filedelete` from every client) against a filesystem double: a LittleFS
delete marks the internal status figure stale once, after the remove, while an SD
delete, a refused delete, a missing file and an unready filesystem leave it alone.
`upload_lifecycle` checks the same for a completed upload (once, after promotion;
never for a failed or SD upload). `--source <cpp>` takes a saved
`System_Filesystem.cpp`.

`redact_audit_nesting` covers the nested forwarding commands of the audit redactor (`espnowremote`,
`espnowroomcmd`, `espnowtagcmd`). It runs the production `redactCmdForAudit` (both overloads),
`redactAuditLevel`, `maskMacTokens`, the rule table and its helpers (`System_Utils.cpp`) and `CommandArgs`
on the counted Arduino String with the device's inline capacity, with the live String bytes tracked.
`resolveCommand` is the production name matcher over a fixed name list. Each forwarded command used to be
redacted by a recursive call from the forwarding handler, so a 2047-byte line of `espnowremote a b c `
nested about a hundred levels and took about 100 KB of stack on the host. Now `redactCmdForAudit` iterates:
each forwarded command is resolved on its own and redacted as a fresh line. The corpus has 7125 lines (seed
20261115): every depth from 0 to 130 with the three verbs mixed and quoted, joined by eight separator forms
(space, double space, tab, space-tab-space, CR, VT, FF, LF); 33 bottom commands (login, wifiadd, usersync,
automation add, dictate, phone pair new, espnowbrowse / espnowfetch, unknown verbs, MAC addresses, the empty
tail); ambiguous quotes at one level; 1084 lines of exactly 2047 bytes (and 300 deeper ones past it, up to
4661 bytes); 6000 random lines. Each line goes through the one-argument form, the two-argument form with and
without an entry, and the one-argument form again on its own result; the report's digest must equal the
recursive redactor's. `--baseline <component dir>` compiles that tree's redactor into the same binary and
compares every output in place. On a painted thread stack a 2047-byte nested line may use at most 2 KB more
than a line nested once, and the peak of live String bytes may be at most 8 times the line length plus
512 bytes. The deep lines also redact on a 64 KB thread stack, where the recursive code dies (SIGBUS). A
source check keeps `redactCmdForAudit` out of the rule handlers and out of one level of the redaction.
Options: `--cxx`, `--sanitize`, `--component`, `--baseline`, `--print-stack`.

`file_manager_refresh_contract` pins one storage refresh per folder load in the
web file manager (`createFileManager`, `WebServer_Utils.h`): the explorer's
`onNavigate` calls `config.onRefresh`, and the actions (new folder, new file,
upload, Refresh) only reload the folder. On the Files page each refresh is a
`/api/files/stats` request, a LittleFS walk of about 1.3 s on the P4X-EYE when
the shared reading is older than 10 s or marked stale. It
takes an optional saved `WebServer_Utils.h` path for a before/after run.

`espnow_fs_transcript_privacy` runs the actual LIST/STAT/GET handlers, path
policies, VFS normalization/directory reader and packed wire definitions. Its
STAT cases distinguish `/sd` and `/sd/...` from `/sdata` and `/sdcard`, with
mounted/unmounted SD and query failures, while checking response fields and
authorization before storage access. `--source-dir <component-directory>`
can replay the former routing bug. Both Python suites use ASan/UBSan when
`HW1_SANITIZE` is enabled. They mock backend/transport boundaries and do not
qualify physical SD or radio behavior. SD's used-byte accessor cannot report
failure separately from valid zero usage; the owner does not guess that state.

`upload_lifecycle` runs the actual streamed web upload handler and filesystem
and polling guards against HTTP, authentication, allocation and file doubles.
It checks resource ownership on rejected requests, failed allocations/opens,
disconnections and failed writes, including partial-file cleanup and nested
guard ownership. Successful chunked text/binary uploads retain their bytes and
run post-save hooks after file, buffer and lock cleanup. `--baseline <cpp>`
accepts a saved pre-change `WebServer_Server.cpp` for the original regressions.
It runs with map hooks enabled and disabled, using C++23 to match the firmware.
ASan/UBSan follow `HW1_SANITIZE`. Uploads stream into a sibling `.upload.tmp`
that is promoted over the destination only after a complete, checked body
(promotion is a VFS double here; `capture_stitching` exercises the real
replacement): a dropped connection, failed write or failed promotion leaves an
existing destination untouched and removes only the staged copy. A name too
long for the `.upload.tmp` suffix stages as `~stage.upload.tmp` in the same
directory, and a refused staged copy fails the upload without ever opening the
destination for writing. Two
consecutive receive timeouts abandon the upload and release the file lock and
polling pause; single idle waits between chunks are retried. These tests do not
establish power-loss durability or real HTTP/SD behavior.

`checked_text_writes` compiles the actual `writeText`/`writeTextAtomic` helpers,
filesystem lock guard and polling guard against file/RTOS boundary doubles.
It checks incomplete writes, reported errors, post-flush size, empty content,
failed opens/renames, destination preservation, temp cleanup, path allocation
failure and nested ownership. A deterministic two-thread test exposes the old
gap between writing and renaming the shared temporary file. `--baseline <cpp>`
can run the same regressions against a saved pre-change `System_Utils.cpp`.
ASan/UBSan follow `HW1_SANITIZE`. These are checked-completion tests; the Arduino
backend does not expose flush/close failures or establish power-loss durability.

`fileread_psram`, `filesystem_psram_listing`, and `g2_files_psram` compile the
actual extracted handlers/walkers/viewer with the production `PsramBuffer`.
They mock filesystem, authentication, HTTP/BLE/rendering and crypto-key/open
boundaries; the listing permission source contract still checks the real
lock-bound authorization implementation separately. ASan/UBSan follow
`HW1_SANITIZE`.

`filemanager_directory` executes the production browser cache/fallback methods
and shared VFS directory reader. It covers 255/256/257-entry boundaries, virtual
SD mounts, picker matches beyond the cache, hidden and malformed entries,
SD-relative names, full permission paths, identity refresh, child-count limits
and borrowed directory/owned entry lifetimes. Operations on the selection
(enter, rename, delete, read) resolve the entry's full native path by replaying
the directory walk, so names longer than the bounded display copy are addressed
correctly within and beyond the cache. Its `--baseline <directory>` mode can
run the same regressions against a saved pre-change FileManager source.

`espnow_directory` executes the production remote listing handler and
reader against protocol/filesystem doubles. It checks pagination, visibility,
permission metadata, errors and reply ownership. These tests do not establish
physical SD behavior or a radio connection.

Coverage includes complete listing JSON/text parity, independent HTTP/command
output ownership, SD naming/counts, permissions-query order, allocation and
capacity failure, fileread encoding/offset/EOF fields and raw-BLE send ordering,
G2 raw/pretty output (pretty through the shared re-indenter, with no JSON
document, including truncated JSON), serial `fileview` re-indenting JSON by
default with `raw` in either argument order (repeats refused; a re-indented
body sized by a counting pass and capped at the pager's 64 KB range, marked
truncated), and malformed encrypted rows that expand on
failure. The existing String reader/decrypt/wrap APIs remain available to other
viewers. Streaming wrap state is additive and does not change chat/event calls.

`memutil_tests` and `memutil_no_psram` additionally exercise the real owned-buffer
growth, self-append, NUL/bounds checks, sticky failures, reuse, and PSRAM fallback
or bypass with mocked heap backends. These are not physical heap measurements.

No directory pagination or new client protocol is introduced. Commands retain
the 4096-byte result budget including NUL; HTTP listings have independently
owned growing storage and are not capped to the command budget. Failed output
allocation never publishes a partial successful listing. Physical PSRAM usage,
BLE congestion/disconnects, SD I/O failures and actual G2/OLED presentation still
require device testing.

`http_feature_gates` preprocesses the actual build header using its deployment
override hook for 100 network/web/custom-WiFi/custom-HTTP combinations. It checks
that the web page flags, including Power, are zero whenever HTTP is disabled.
It does not claim that all unrelated hardware combinations link or boot.

## ToF PSRAM object lifecycle

`tof_psram_lifecycle` extracts the production `tofInit`, `tofTask`, and
`ps_delete` template into a host harness. Mock sensor/I2C/allocation boundaries
verify PSRAM-preferred allocation, placement construction, initialization
failures, reinitialization, and the task's shutdown branch. Every teardown must
destroy the object before freeing its storage and clear the global pointer.
ASan/UBSan follow `HW1_SANITIZE`; the existing `memutil_tests` cover the real
allocation helper's PSRAM fallback/bypass policy with a mocked heap backend.

This moves only the VL53L4CX object's inline state. The vendor driver code,
I2C/Wire buffers, task stack, cache, and sensor timing settings are unchanged.
Host tests cannot establish physical RAM placement, ranging accuracy, or the
polling-time impact of PSRAM access; those require an on-device check.

## ToF polling and stop outcomes

`tof_poll_outcomes` executes the production poll and task through the real I2C
manager transaction template, clock handling, and health accounting on both
buses. Mock RTOS waits inject a stop before polling, during bus acquisition,
and during cache acquisition. A stop before entry does no bus work; a stop
after admission permits one hardware operation to finish without publishing.
Every cache admission, event, and driver destruction requires the bus released.
Repeated not-ready samples do one readiness read each and leave timestamp and
sequence unchanged. Bus/cache refusal and real readiness/read/rearm failures
exercise the distinct health and publication outcomes. Task cleanup executes
the real invalidation and matching object destruction/free path.

The optional `--baseline /path/to/pre-change-checkout` mode reproduces the old
stop-during-bus-wait defect: no vendor read, an ACK diagnostic probe, and a
synthetic NACK recorded by the actual manager. It accepts a bounded snapshot
of the old owner and I2C headers, using unchanged support files from this
checkout. The normal registered test requires the corrected behavior;
`HW1_SANITIZE` enables ASan/UBSan. Hardware timing and physical ranging still
require device testing.

## Optional PSRAM for sensor objects

Application-owned heap driver objects use `AllocPref::PreferPsram`: external
RAM is preferred, but an absent/exhausted external heap or the PSRAM bypass
routes them to byte-addressable internal RAM. No sensor parent allocation uses
`RequirePsram`. Genuine exhaustion of available internal/external memory still
returns allocation failure; this policy is not a guarantee that every feature
configuration fits a board without PSRAM.

`sensor_parent_lifecycle` extracts the production IMU, APDS, servo, gamepad, and
ANO initialization functions and task shutdown blocks into host boundary mocks.
It checks placement construction and matching destruction, failed initialization
and retries, and Seesaw's existing retain-across-retry/stop behavior. Vendor-owned
I2C helpers, Wire buffers, locks, static caches, and driver algorithms are outside
this migration. The tests do not exercise real hardware or task races.

`sensor_task_restart` compiles all nine production sensor task-creation helpers,
the shared self-exit macro, and RTC's existing exit tail with minimal RTOS
boundary mocks. It checks failed creation/retry, duplicate starts, publication
of the correct null handle before deletion, and repeated restarts before and
after idle frees the old task control block. Source checks bind each driver's
exit to its own handle and require cleanup before restart is published. This
guards against querying a deleted task's retired handle; it does not reproduce
the real scheduler, sensor cleanup, or I2C transactions. ASan/UBSan follow
`HW1_SANITIZE`.

`memutil_tests` and `memutil_no_psram` compile the real allocation implementation
with and without `BOARD_HAS_PSRAM`, respectively. They cover runtime-absent
PSRAM, exhausted-PSRAM fallback, bypass, true allocation failure, and matching
placement construction/destruction. Heap-capability backends are mocked, and
ASan/UBSan follow `HW1_SANITIZE` for both targets and the lifecycle harness.

## OTA PSRAM replies

The OTA-enabled main image lazily retains its command reply buffers using
`PSRAM_STATIC_BUF`, preserving the old static returned-pointer lifetimes and
capacities. The eight common replies total 2,972 bytes, with another 512 bytes
when Bluetooth is enabled. Only their pointers remain in fixed internal BSS;
actual buffers prefer PSRAM and fall back to internal RAM when it is absent,
exhausted, or bypassed. Unused commands do not allocate their buffers. The
separate recovery updater and non-OTA command stubs are unchanged.

`ota_psram_replies` executes extracted production status/journal-reset handlers,
the BLE JSON formatter, and the real buffer/JSON macros against host boundary
mocks and vendored ArduinoJson. It checks allocation failure and retry,
persistent reuse, capacities, JSON allocation failures, and journal allocation
failure before mutation. Source guards cover all ten buffers, all three JSON
documents, explicit pointer capacities, BLE reply allocation before upload
mutations, and credential-buffer allocation before copying a secret.

The existing PSRAM/no-PSRAM MemUtil tests cover the actual fallback policy.
ASan/UBSan follow `HW1_SANITIZE`. These tests do not run a real OTA upload,
validate hardware PSRAM placement, or simulate the full BLE/flash state machine.

## System Event catalog coverage

`event_catalog_tests` links the real `System_EventCatalog.cpp` and exercises
the public provider plus its dependency-free C++17 validation/index core. It
pins the reviewed 12-family/152-kind order, checks every global and grouped
boundary, verifies the legacy `boot` alias and fallback behavior, walks an
interleaved synthetic family with more than 32 kinds, counts `new`/`new[]`
calls during provider reads, and stresses concurrent immutable lookups.

`event_catalog_json_tests` links the real provider and
`System_EventCatalogJson.cpp`, while hostile fixtures call the production
provider-shaped JSON core directly. It verifies quote, backslash, C0 and UTF-8
handling; exact size and buffer precedence; NUL/malformed-UTF-8 preflight;
sink-failure byte accounting; and the real `CMD_RESULT_MAX` budget. Its
`--dump-json` mode emits only the production serializer bytes, while
`--dump-typed` emits an independent ordinal/length/hex traversal protocol.
`event_catalog_json_parse` invokes both modes, parses the JSON with Python's
standard library, and compares every family and kind with typed traversal and
the frozen fixture. The current established result is 13 families, 159 kinds,
and exactly 3,051 payload bytes (3,052 including a command-buffer NUL).

`event_catalog_text_tests` exercises the real dependency-light human listing
core used by `events kinds`. It covers basic and exact-fit packing, a canonical
token wider than the former 120-byte staging array, whole-token flush/retry,
sink failure, invalid providers, and zero-callback rejection when one complete
header or token cannot fit the named 255-byte debug payload.

`event_catalog_fixture` compares the private row source with both the reviewed
grouped v1 JSON fixture and an independent 152-row declaration-order fixture,
without rewriting either one. `event_catalog_structure` fails closed under
optimized Python and enforces the repo-wide private-row include allowlist,
legacy-macro and copied-vocabulary guards, single table/index owner, provider
dependency boundary, immutable storage, JSON/text-core include allowlists,
single serializer ownership, typed-provider and CLI/HTTP delegation, removal
of obsolete catalog ArduinoJson builders, and absence of allocation and
locking APIs.

For Phase 3, the same structure test now checks both native OLED event pickers.
It requires direct calls to `systemEventCatalogFamilyCount()`,
`systemEventCatalogFamilyAt()`, and `systemEventCatalogFamilyKindAt()`; rejects
the former 24-entry arrays/build functions and scalar compatibility scans; and
requires the automation wizard to retain provider family/kind ordinals rather
than a copied canonical-name buffer. `notification_integration_guards` also
requires the notification editor to pass the resolved typed record's full name
to the bounded one-kind mutation command. These are source contracts: the host
suite cannot execute the Arduino/OLED renderer or synthesize real button input.

The complete host suite currently passes 45/45, with sanitizers enabled on
native targets. The restoring five-profile firmware matrix remains the Phase 2
provider/adapter evidence, including both ordinary recovery builds and exact
source-config restoration. The ordinary FeatherS3 build performed after the
Phase 3 edits also succeeded with `DISPLAY_TYPE=0`. A separate temporary
FeatherS3 compile-coverage profile enabled the level-4 custom OLED/gamepad
gates and `DISPLAY_TYPE=1`, compiled both active picker bodies without errors,
and confirmed that both archive members refer to the three native indexed
operations and no catalog-JSON symbol. This establishes compilation and link
ownership, not physical OLED behavior.

A separate authenticated live-browser acceptance reached the ordered 12-family/
152-kind web picker and preserved selection without saving. That is web UI
evidence only: it did not expose the raw HTTP response/auth exchange and does
not exercise OLED. Physical OLED/BLE/G2/UART procedures and live HTTP failure
injection remain outside these host guarantees.

## I2C transaction-policy coverage

`i2c_transaction_options_tests` executes the production manager transaction
template against deterministic host stubs. It starts with an already-registered
gamepad identity, then proves consecutive calls independently apply 100 kHz / 80
ms and 400 kHz / 15 ms policies to the bus clock and mutex wait. The companion
`i2c_transaction_contract` guard keeps timing out of duplicate registration,
requires every standard and NACK-tolerant helper to forward explicit per-call
options, and requires boot identities to use their configured physical bus.

`i2c_probe` extracts the production address probe, clock helpers, constructors
and inline probe/ping wrappers, compiling them against the actual manager
header and Wire/RTOS substitutes. It verifies both physical bus mappings,
write-only/unprimed device ACKs without payload reads or writes, confirmation
of positive ACKs under the same bus lock, transient ACK rejection, unchanged
NACK/error statuses, mutex and clock-stack refusal, clock restoration, and
runtime-disabled wrappers. Discovery must not change registry identities,
device health or transaction metrics. Source guards keep CLI, OLED, boot and
hardware detection scans on this shared probe and polling guard, without direct
Wire initialization or clock changes. ASan/UBSan follow `HW1_SANITIZE`;
electrical faults, real device identity and timing still require hardware testing.

## Command catalogue coverage

`command_catalogue` compiles the complete production `System_CommandRegistry.cpp`
with the real metadata header and small command-table/connection-provider link
fixtures. Seven selections cover no optional modules, networking, gamepad and
peripherals, ANO input, applications, glasses without Bluetooth, and combined
features. Readable snapshots captured from the pre-extraction table preserve
module order, command/count addresses, flags and callback provider/sensor names.
Callbacks run with every provider off and each provider enabled individually;
ASan/UBSan follow `HW1_SANITIZE`. These are resolved feature inputs, not a second
board/dependency model or hardware qualification. When deliberately changing the
catalogue, review the affected snapshot rows and selections alongside the change.

## Saved WiFi network coverage

`wifi_networks` compiles the production saved-network owner, real ArduinoJson
and the existing Settings secret wrappers with host String, mutex, allocation,
crypto and persistence boundaries. Cases preserve the original helper behavior:
input limits, blank-password updates, eight-entry capacity, priority ties,
normalization, final removal and RAM changes after persistence failure. New
coverage checks stable entry IDs, credential-free presentation snapshots,
coherent connection copies under concurrent updates, setup-result/autostart
handling, legacy JSON loading, ciphertext preservation and password omission.
The real removal commands and ID parser cover quoted/plain SSIDs, invalid IDs,
stale selections, a removal that a failed name copy can no longer abort, and
persistence failures; `credential_scrub` checks that every plaintext copy the
owner makes (targets, writer/loader arrays, removed and replaced records) is
zeroed before release; `interactive_fields` checks that a double-quote or a
control character (TAB, CR, LF, 0x01, 0x7F) in the name or password is
refused, stores nothing and broadcasts the reason, while spaces, punctuation
and non-ASCII bytes stay ordinary name bytes. It also checks the owner's field
rule (`wifiNetworkFieldProblem`, first offending byte) and the structured add
line (`wifiAddCommandLine`). `--component <dir>` reads another tree; the
pre-fix owner fails `interactive_fields`, because it stored names with control
characters. Command registration guards retain their existing authentication
requirement.
Injected record-copy, initialization and JSON allocation failures must report
failure; crypto, output and persistence must run outside the list lock.
The disabled source/API also compile and run without platform headers.
ASan/UBSan follow `HW1_SANITIZE`; these fixtures do not qualify ESP allocation
placement, radio connections or RTOS scheduling. `wifi_network_consumers` guards
Settings load readiness/key selection, snapshot-failure propagation and setup
callers' result/cancellation handling.
`wifi_saved_ui` executes the production OLED and G2 list, selection and command
submission functions against changing snapshots. It checks retained identities
across reorder/removal, confirmation callbacks, session reset, access checks and
failed page admission without testing radio hardware.

## GPS snapshot boundary

```sh
python3 -B components/hardwareone/test/host/test_gps_snapshot.py --sanitize
```

`gps_snapshot` compiles the production snapshot/JSON/invalidation functions,
startup reset and task publication/shutdown blocks, the real cache guard, and
the GPS portions of G2, web and sensor logging (MQTT sends `gpsBuildDataJson`'s
text, pinned by `mqtt_payload_sources`). It checks independent
copies, cleared failed reads, 5/10/50 ms consumer budgets, all JSON buffer
capacities, disabled-header behavior, failed startup reset, and deferred stop
cleanup. Publication holds the cache lock only for the copy; JSON formatting,
track/event callbacks and destruction run after its release. ASan/UBSan follow
`HW1_SANITIZE`.

The output fixture was captured from the pre-refactor production functions.
An optional `--baseline /path/to/old/components/hardwareone` also executes that
source and compares its output directly. OLED has a narrow source-boundary check; its full renderer is not
compiled because an existing glyph escape is rejected by desktop Clang.

The host semaphore, driver and transport substitutes do not exercise real RTOS
scheduling, NMEA parsing, I2C hardware or rapid stop/restart lifecycle races.
Shutdown waits are bounded individually; persistent lock contention can defer
cleanup. This test does not claim a maximum total shutdown duration.

## Presence snapshot boundary

```sh
python3 -B components/hardwareone/test/host/test_presence_snapshot.py --sanitize
```

`presence_snapshot` extracts the production snapshot, JSON, CLI, startup reset,
shutdown and complete polling functions, the real cache guard, and the presence
portions of G2, web, OLED and logging (MQTT sends `presenceBuildDataJson`'s text,
pinned by `mqtt_payload_sources`). It verifies independent copies,
cleared unavailable reads, 5/10/50/100 ms budgets, JSON buffer boundaries,
disabled-header behavior, reset failure and deferred shutdown retry. Formatting
and callbacks must run outside the cache lock; cache waits must follow release
of the shared I2C bus. ASan/UBSan follow `HW1_SANITIZE`.

Polling checks cover all detection-bit combinations, signed/raw temperature
units, every register-read failure, no new sample, failed I2C admission,
cache contention, presence clear after ten consecutive polls, reset of that
debounce by renewed detection, and motion edge notifications. A saved output
fixture retains successful, invalid, inactive and busy consumer behavior,
including JSON's distinct `ambient:0` timeout and `ambient:0.00` successful-zero
formats. `--baseline /path/to/old/components/hardwareone` additionally executes
the pre-change code and compares its output directly.

Register I/O, RTOS admission, HTTP and OLED printing are host substitutes.
These checks do not qualify hardware detection accuracy, actual display
formatting, bus timing or task scheduling. Each shutdown wait is bounded;
persistent contention can defer completion. General sensor restart lifecycle
and I2C teardown are outside this cache-boundary test.

## Remaining sensor reading boundaries

```sh
ctest --test-dir /path/to/host-build --output-on-failure \
  -R '^(small_sensor_snapshots|thermal_snapshot|fm_radio_snapshot|sensor_reading_consumers|input_button_snapshot|gpio_encoder_integration)$'
```

Configure the host build with `HW1_SANITIZE=ON` for ASan/UBSan coverage.
`small_sensor_snapshots` executes production IMU, ToF, APDS and RTC snapshot,
JSON, OLED and lifecycle blocks, including ToF polling/filtering and RTC
publication. It covers cleared failed copies, disabled interfaces, reset
failure and bounded shutdown attempts. Existing `sensor_parent_lifecycle` and
`tof_psram_lifecycle` suites continue to cover parent teardown and ToF storage.

`thermal_snapshot` executes production metadata/frame copies, serialization
and OLED rendering. It checks raw/interpolated units, all four rotations,
caller capacity, unavailable/allocation-failure paths, copied-frame lifetime,
startup and shutdown. Start allocates the run's zeroed buffers (interpolation
optional) and fails without a frame buffer; a source guard keeps the cache
allocation and its lock out of the poll that runs inside the bus transaction. The output fixture includes the integer ESP-NOW payload.
Consumers own copied frame buffers; a failed copy leaves the supplied buffer
untouched and clears its metadata.

`fm_radio_snapshot` exercises the production fixed-size critical copy, RDS
scrubbing, JSON, OLED, commands and polling. It checks seek admission, the
300 ms arm delay and 6 s failsafe, failed bus admission and command/poll order.
Hardware calls and output must occur outside the critical section. The enabled
snapshot always takes that short critical section; its timeout argument is
retained for interface compatibility.

`sensor_reading_consumers` executes the affected G2 formatters, Bluetooth JSON,
logging blocks, automation conditions and FM readout using substituted owner
APIs. It preserves successful output and consumer-specific validity policies;
owner synchronization is covered by the suites above. The automation `MOTION`
condition reads the presence sensor's motion flag (SEN-8): `DETECTED`, `NONE`,
and false without a reading or the lock. These four suites accept
`--baseline /path/to/old/components/hardwareone` for direct comparison with
pre-change production source. Checked-in output fixtures support ordinary runs.

`input_button_snapshot` executes the real HAL peek, atomic consume, publication
and activity sequence operations, disabled input and ANO detent/reset/start
paths. `gpio_encoder_integration` compiles the GPIO backend and HAL owner with
both three- and five-button wiring, including failed initial publication,
failed task creation and a contended stop. Snapshot readers do not consume
latched presses, and a failed activity read preserves the previous sequence.

Host driver, bus, display and RTOS substitutes do not qualify physical sensors,
interrupt timing or real task scheduling. Lifecycle flags remain separate from
reading ownership; these tests do not claim a general restart/teardown redesign.

## PCA9685 and FM command outcomes

```sh
ctest --test-dir /path/to/host-build --output-on-failure \
  -R '^(servo_command_outcomes|fm_radio_snapshot|sensor_parent_lifecycle)$'
```

`servo_command_outcomes` executes production command parsing, initialization,
servo/profile/calibration and PWM handlers. Invalid input must not initialize
or write hardware; refused transactions must not report success. It covers
initialization failure and retry, profile preservation, reported PWM write
failure and the single lock around optional frequency plus PWM updates.

`fm_radio_snapshot` also covers strict tune/volume/seek/mute argument validation,
refused transactions followed by recovery, unchanged cache/events/banners on
refusal, and offline volume storage. Its existing JSON/OLED fixtures remain
unchanged. Both suites use controlled driver and bus outcomes and inherit
`HW1_SANITIZE`. Void driver calls establish only that a request was issued;
these tests do not qualify physical writes, servo motion or radio reception.

## Cached APDS and ToF read commands

```sh
ctest --test-dir /tmp/hw1-hardwareone-host --output-on-failure \
  -R '^(apds_read_commands|tof_read_command|small_sensor_snapshots|sensor_reading_consumers)$'
```

`apds_read_commands` executes the production commands, snapshots, JSON,
invalidation and startup cache reset in a variant with no hardware or event
APIs declared. A second variant executes the task's polling step
(`apdsPollDevice`) across 32 bus/mode/readiness combinations, bus/cache refusal
and fault handling. It checks per-field availability, color-not-ready keeping
the previous sample without an I2C health error, a gesture cadence of at most
100 ms independent of the color/proximity interval, presence moving only on
proximity samples, gesture/presence event ownership, and the latched last swipe
that `apdsgesture` reports with its age. Reading a gesture repeatedly does not
consume the cached value or read the hardware again.

`tof_read_command` executes the production command, snapshot guard and JSON
builder. It checks stopped/disconnected/busy/invalid/no-object outcomes,
nearest-object selection within the four cached slots, text/JSON distance
agreement, and exactly one distance message outside the cache lock. Validation
does not perform a read. Both suites enable ASan/UBSan with `HW1_SANITIZE`.

These checks qualify cached command behavior; APDS mode writes, driver
teardown synchronization and the vendor gesture reader's execution time remain
separate from this read-command change.

## Incremental APDS gesture reader

`apds_gesture_driver` extracts the vendor's actual `readGesture`, `resetCounts`
and FIFO read methods, with I2C transfers, register reads, gesture-valid status
and Arduino time supplied by the harness. Every call performs at most one FIFO
transfer and three I/O operations, with no sleeping or polling loop. Continuous
held input over many calls must release the bus after each available batch.
It checks all four directions within a batch or across calls 200 ms apart,
recent partial gestures surviving no-data gaps, 300 ms inactivity expiration,
FIFO levels 0/1/2/3/32/33, failed partial transfers, direction state surviving
256+ records, unsigned clock rollover, and a FIFO overflow (GFOV) batch that
still completes an earlier half but drops its own partial direction. ASan/UBSan follow `HW1_SANITIZE`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host --output-on-failure \
  -R '^apds_gesture_driver$'
```

These controlled FIFO fixtures do not qualify physical gesture accuracy or
Wire timeout behavior; each underlying I/O call must still return.

## Sensor start and stop requests

```sh
ctest --test-dir /path/to/host-build --output-on-failure \
  -R '^(i2c_device_queue|sensor_lifecycle_requests|sensor_start_outcomes|sensor_task_restart|tof_psram_lifecycle|sensor_restart_core_tests|sensor_auto_disable_sites|openinput_backends)$'
```

`i2c_device_queue` compiles the production manager queue methods. It covers
duplicate starts, wrapped/full rings, cancellation, stop priority, unavailable
queues and stop admission with the bus disabled. Queue position/depth remain
start-only; stop requests have independent capacity.

`sensor_lifecycle_requests` executes the production command admission and full
worker with the actual manager queue. It injects close requests after dequeue,
during startup spacing and inside initialization. It checks returned start
outcomes, suppressed cancelled-start announcements, cleanup before stop events,
taskless FM/RTC stops, probe errors and balanced polling pauses, and that the
admission core shared with the display's input auto-start never writes the
command reply buffer. Stopping an idle sensor publishes nothing, a cancelled
admitted start still closes with its stop event, a stop with the bus disabled
is refused without allocating the manager, and start hints follow only an
accepted start (helper and the real GPS command). ESP-NOW enabled
and disabled variants run separately.

`sensor_lifecycle_requests` also covers that no saved setting refuses an open
(every sensor opens with its start-at-boot switch off, in and out of first-time
setup) and the automatic restart of a sensor that switched itself off, through
the production worker. `sensor_restart_core_tests` is the restart's wait ladder
on its own (System_SensorRestartCore.h). `sensor_auto_disable_sites` reads the
drivers: each of the ten auto-disable sites reports before it stops, and nothing
else reports. `openinput_backends` compiles the production `openinput` for
the GPIO encoder and the three I2C input backends; no saved setting refuses it.

`tuning_settings_saved` checks that every setting the ESP-SR and G2 commands
write has a settings row, that the five tuning rows' defaults equal the
constructor's, and that the values survive a save and a load with the
production code; auto-tune's trial gain is never what a save writes.

`sensor_start_outcomes` executes all nine production sensor starters, including
both I2C input backends. Cleanup guards must run before initialization or cache
reset; initialization, allocation and task-creation failures must roll back
state; an input task-creation rollback re-publishes status. Thermal/IMU/FM
initialization handshakes also exercise failure and timeout.
The existing task-restart test covers retired task handles. ToF lifecycle tests
check protected stop-measurement writes and release when bus access is refused.
All compiled suites inherit `HW1_SANITIZE`. These deterministic interleavings
do not simulate every RTOS schedule or concurrent hardware command.

## Setup draft, adapters and completion

`setup_draft` compiles the complete noninteractive wizard owner, its actual public
header, registry lookup functions and settings mutators. It checks draft-only
feature toggles, mode selection, optional-page visibility, identity/broker staging,
strict MQTT ports, restart and discard, no-op completion, and applying only fields
changed from the initial snapshot. Unrelated external settings edits survive.
Completion covers cancellation despite a completed result, unavailable save scopes,
String allocation failure before publication, accepted RAM changes with pending
save retry, Wi-Fi save outcomes, debug/main updates and runtime effects after flush.
Full and minimal compiled-feature variants run separately. Registry rows, platform
memory, storage/network and the save-scope boundary are controlled fixtures.

`setup_menu_layout` runs the wizard owner with the actual feature registry and
compiler-resolved build configuration for QT Py Maps, XIAO, P4, minimal, and
XIAO with Edge Impulse/MQTT enabled. Every compiled mutable registry feature must
appear exactly once in the expected group, sorted by its displayed label, with
adjacent startup choices and draft-only bindings. It also checks shared row
formatting, broadcast serial output, empty-group navigation, Wi-Fi in the middle,
and actual CLI group input including malformed/extreme numbers and rejected
completion. The matrix uses board headers and saved configurations, not a copied
feature catalog; it does not compile each device's firmware or drivers.

`setup_wifi_navigation` executes the serial, OLED and CLI Wi-Fi adapters for
accept, skip, rescan, back, expired selections and cancel. Wi-Fi stays an
intermediate page; skip clears captured credentials, while backing out preserves
the previous accepted values. Input, scan delivery and shared navigation are
controlled boundaries; the menu suite above exercises the actual navigation.

`setup_mode_choices` compiles the production Basic/Advanced/Import table and
parser across all eight migration/Wi-Fi/HTTP combinations. It checks visible
choices, numeric/alias parsing, invalid inputs and output preservation. Import
requires migration and Wi-Fi independently of the regular HTTP interface.

`setup_espnow_application` and `setup_mqtt_application` execute all three production
serial, CLI-mode and OLED page handlers against a recording draft-owner boundary.
They cover complete/partial input, blank/early-next fields, stale page scratch,
back/cancel/timeout, staging refusal (the CLI page then asks for its fields again
from the label and stages the new answers, SET-5), skip behavior, strict MQTT ports and OLED
stationary buttons. No page submission writes live settings or starts a service.
The draft owner itself is qualified by `setup_draft`, rather than copied into the
adapter fixtures.

`setup_completion` executes both production exit adapters, universal CLI cancel,
and actual serial timeout/input activity functions. It checks completion messages
only after accepted apply, presentation of the applied result, cancellation before
apply, refusal of a completed draft when the CLI exit is ineligible, scratch/credential
cleanup, idle timeout reset and serial ownership release.
The interactive dialog and apply operation are recording boundaries; full owner
behavior is covered by `setup_draft`. Wi-Fi-enabled and disabled variants run.

`setup_display_cancel` checks production OLED input cancellation and first-page
back/cancel routing, plus CLI tick handling of cancel and ignored input. These
host suites inherit `HW1_SANITIZE`; they do not qualify physical controls, rendering,
radio connections, actual flash persistence or the device scheduler.

`climode_completion_owner` executes the real CLI mode owner. It checks completion
eligibility for serial and deferred display exits, refusal after owner loss or
timeout, and queries from another task or outside the exit callback. The setup
adapter suite independently verifies that refused eligibility discards the draft.

`initial_admin_setup` compiles the shared first-admin creator with vendored
ArduinoJson, the complete post-restore credential flow, and the normal setup
caller's unchanged save-through-success tail in a bounded restart loop. It checks
empty credentials, failed hashing, JSON allocation and String serialization faults,
credential/index save failures, retry, account metadata and theme, and credential
storage before publication of the user index. A refused settings save is logged
and reported to the operator while setup completes. Failed creation must skip completion,
automation creation, runtime settings application and reboot. Migration and OLED
enabled/disabled variants run under `HW1_SANITIZE`. Earlier Basic/Advanced choices,
hashing, String storage, file owners, input delivery and reboot are controlled
boundaries; this does not simulate power loss or qualify durable flash writes.
The real `saveUserSettings`/`writeTextAtomic` writers are not driven here; their
own suites (`settings_file_writes`, `checked_text_writes`) cover them.

```sh
ctest --test-dir /path/to/host-build --output-on-failure \
  -R '^(setup_draft|setup_menu_layout|setup_wifi_navigation|setup_mode_choices|setup_completion|setup_display_cancel|climode_completion_owner|initial_admin_setup|setup_espnow_application|setup_mqtt_application|settings_write_scope|wifi_network_consumers|settings_downstream_persistence_contract)$'
```

## Shared CPU clock changes

`power_cpu_frequency` executes the production clock owner, power-mode application
and `cpufreq` command with the real power-policy header. I2C-enabled variants link
the actual polling-pause implementation; disabled variants provide no manager or
pause symbols. It checks both bus locks, partial acquisition failure, recursive
and concurrent request refusal, absent buses/manager, nested pause preservation,
HAL refusal and readback mismatch, and whether the owner's own HAL call changed
the clock. Commands retain validation and admin registration; mode/command success
messages, settings and events follow confirmed changes. ESP32/S3 and both P4
revision policies run separately, including HAL-accepted idle-only 40 MHz.

`power_save_tick` executes the unchanged idle/wake function in a fresh process
for each scenario. It checks refused idle entry and its backoff (a refusal is
not retried on the next lap, and a persistently wedged bus gets one attempt per
backoff window), retained wake retries after activity is consumed with the
display restored before the pending clock, recovery after an owner's partially
failed clock change, and refusal overlapping another owner's clock change. Display/cooldown/events
must follow confirmed CPU transitions. Disabled power-save, STT, Locked mode and
UART baud-floor behavior are included. These suites inherit `HW1_SANITIZE`; RTOS
semaphores, clock hardware and display effects remain deterministic boundaries,
so they do not qualify electrical I2C timing or physical sleep/wake.

```sh
ctest --test-dir /path/to/host-build --output-on-failure \
  -R '^(power_cpu_frequency|power_save_tick)$'
HW1_POWER_TEST_APP="$PWD" python3 experiments/p4_ble_roles/tests/test_power_profile.py
```

The existing experiment profile test additionally checks power JSON and CPU menu
labels against the selected application's production source.

## Icon lookup and rendering ownership

`icon_ownership` compiles the generated icon assets, complete handwritten lookup
owner and extracted production renderer/adapter with their actual public headers.
OLED+web, OLED-only, web-only and headless selections run separately. The real
`HAL_Display.h` selects SSD1306 and ST7789 aliases over controlled pixel sinks;
an `Adafruit_GFX`-typed SSD1306 caller checks the shared rendering interface.
Web-only and headless compilation have no display-library headers available.

The fixture was captured by executing the previous `System_Utils.cpp` icon
functions before their move. It pins the generated source hash, asset bytes,
every extension mapping/fallback and every compiled icon's pixels at nine scales
(0.01, 0.25, 0.49, 0.5, 0.51, 0.75, 1, 1.3 and 2), plus a clipped placement.
Normal-size legacy and scaled drawings must match. New validation cases cover
null/unknown names, null display/bitmap, invalid dimensions and invalid/overflow
scales. Named global rendering must match the shared pointer overload. ASan/UBSan
follow `HW1_SANITIZE`. Display libraries are pixel-recording boundaries; this
does not test panel transport or prove physical display output.

```sh
ctest --test-dir /path/to/host-build --output-on-failure -R '^icon_ownership$'
```

For an intentional artwork or rendering change, review the changed assets/code,
explicitly regenerate the fixture from current production, then review the
fixture diff with the change:

```sh
python3 components/hardwareone/test/host/test_icon_ownership.py --update-fixture --sanitize
```

Ordinary test runs never update fixtures. `--capture-baseline <source-directory>`
remains available only for the original historical capture from a pre-move
source snapshot; it is mutually exclusive with `--update-fixture`.

## NTP transport coverage

`ntp_transport` compiles the complete production `System_NTP.cpp` and its real
public header with deterministic SNTP, WiFi, time and Clock service substitutes.
It checks actual replies versus retained time, warm/dark timeout budgets, RTC
fallback, disconnected/stopped clients, late-reply status and notices, and
deferred Clock handoff. Callback injection at the deadline and during drain
covers those ordering boundaries; multiple queued replies retain the existing
coalescing behavior. Setup checks callback ordering, timezone reapplication and
stable server storage. RTC-disabled paths run separately; WiFi-disabled source
and APIs compile and run without any platform headers. ASan/UBSan follow
`HW1_SANITIZE`. This does not test real SNTP traffic, RTOS scheduling or Clock's
separately owned post-step duties.

## Bluetooth status coverage

`ble_info_status` compiles the production BLE info formatter, CLI command,
lifecycle guard, state strings, BLE types and Bluetooth HTTP status handler with
vendored ArduinoJson. It checks the existing JSON/text schema, active-slot
ordering, authenticated/anonymous identity, escaping, wrapping connection age,
copied string lifetimes, independent caller documents, bounded lock refusal,
absent initialization, every observed JSON allocation/growth failure, CLI size limits,
HTTP authorization and short final serialization. G2-enabled and disabled
variants preserve G2/ring fields through direct owner access, check typed G2
connection fields independently of display text and all four combinations of
their independent auto-reconnect policies, report unavailable G2/ring
status and allocation errors, and accept ring JSON larger than the CLI
buffer; the BLE-disabled public header is compiled and executed separately. `--baseline <component-dir>`
compares successful CLI output against saved pre-change source. The phone link
block (phone link v2): `bleinfo` ends its text with it and adds `phoneLink`
beside buildBleInfoJson's 14 keys in a pipe build only (the identical-output
rule across the glasses x pipe builds compares the CLI without the block, and
`/api/ble/status` `ble` keeps exactly 14 keys); `blestatus` puts it after the
link lines, with `phoneLink` (full form, end reasons by name) beside `link` in
JSON; `/api/ble/status` adds it from its one link reading. The console view
counts the console stream's own row in any client role (phone-client mode
included) and design A's slot only once joined.

`sensor_mic_card` executes the actual Sensors empty-grid JavaScript chunk with a
small DOM boundary. It checks that a microphone-only page preserves the shared
microphone target, other available sensors remain, and an empty grid
still shows its placeholder. `--baseline <header-or-component-dir>` replays the
same expectations against a saved header; it does not test layout or audio.
It needs Node.js (see the quick start); without it the test is disabled.

The extracted command-write callback and advertising functions also execute
against boundary mocks, checking guarded counter/state updates, invalid writes
and dispatch after lock release. Initialization/teardown as a whole is not
executed by this suite.

Two host reader threads and a writer exercise coherent lock-protected fields
with the extracted guard and formatter. This substitutes host locking for
FreeRTOS and does not qualify radio callbacks, hardware or firmware scheduling.
HTTP, String append failure and allocation policies are host boundaries.
ASan/UBSan follow `HW1_SANITIZE`.

`phone_status` compiles the production phone status owner (BLE_PhoneStatus)
with the real CompactJson and JSON-argument parser; the pipe, the peer registry
and the SSE sink are fakes. It checks the lens/OLED/web status phrase, the
compact `phone-status` SSE event (seeded first tick, a push only on a change,
the 250 ms cadence, the 128-character cap), the `/api/ble/status` phone object,
`phone status [json]` (in G2 mode the console signed in on the pipe's link, and
waiting for the phone link with a slot the old link left uncounted in `status
json`), the console stream first in phone-only mode (signed in, then waiting
while the app does not want it; a signed-out row after the idle logout keeps the
stream open and the relay as it was), `status json`'s compact `phoneLink` block
(exact dicts, no texts or end counts), the secret's kind with and without
`Settings::phonePipeSecretGenerated` (two builds that differ only there), that
no output carries an address or the secret, and that only the
tick stub remains without the pipe (`phone` is then a one-row "not in this
build" table in the `#elif` branch of BLE_PhonePipe.cpp). The G2 Network page's Phone menu is covered by
`wifi_saved_ui` and `g2_network_async_redraw`; the web Phone card by
`g2_status_browser` (Node, or a Deno-backed `node` shim).

`oled_bluetooth_link_watch` compiles all of OLED_Mode_Bluetooth.cpp, with the
real BLE_LinkStatus.cpp behind it, against fakes
(oled_bluetooth_link_watch_harness.cpp) in all four glasses x phone-pipe
builds, on the 64 px OLED and the 128 px P4 canvas. The BT status screen shows
`Role:` (+ `Saved:`), the glasses and ring, `Pipe:` and `Console:` in the
link-status words with no `State:` row; the console's detail stops at the
footer. The Phone screen shows `PIPE [OK]/[..]/[!!]/[--]/[off]`, the pipe's
words and `Console:`, and its taps run what the drawn rows said. Beside the
glasses `Console:` is the console on the pipe's link (`waiting`, `signed out`,
`signed in`, `no secret`, `off`). The menu's Advertising/Disconnect rows come
from bleConsoleView() with gBleState freed; on the phone link the row reads
`End console` (still `bledisconnect`) and Advertising is absent. The App console
row's confirm says `Switch to server?`.
No row wraps past 21 columns. The link watcher marks the display once per
change of role, saved mode, pipe phase or text, pairing, auto-reconnect,
console state, clients, advertising, glasses or ring, and never when idle.
While the G2 Status screen is open the watcher also marks once when a row it
draws changes (a temple, a shown battery, the repair state, Nav Mode, the
status becoming unavailable), not for MTU or counters, not on opening or
leaving the screen, and it does not read the status on any other screen.

`g2_status_corner` compiles the G2 Status compound's geometry, child count,
child builder and phone row from G2_Glasses.cpp in all four phone-pipe x
battery-monitor builds. It checks that CREATE/REBUILD ship exactly the counted
children with distinct container ids (the back list holds 1), that no rectangle
overlaps another or leaves the 576x288 lens, that the phone row exists only
with the pipe and sits right under the last device row, and its width budget
(no wider than "ESP:~100%"). The phone row is `PH:` plus hw1link::pipeBrief's
word for each phase a built pipe can show, driven from the pipe's own inputs
through the real bleLinkStatus() (BLE_LinkStatus.cpp, console server down):
`PH: off`, `PH: nokey`, `PH:unpair` (a six-letter word drops the space),
`PH: pair`, `PH: wait`, `PH: scan`, `PH: link`, `PH: ready`, `PH: close`,
`PH: idle`, `PH: fail` and `PH: fault`; the phone snapshot it reads must agree
with bleLinkStatus()'s pipe. The lens firmware accepting the extra text child is
not covered.

`ring_info_status` compiles the production ring status owner, CLI command,
state type, text formatter, pending-state reader, shared JSON-argument parser
and validation macro. It checks the twelve-field schema, empty/invalid Arduino
Strings, escaping, copied string ownership, independent output documents,
wrapping uptime, absent-task stack defaults and stack-use clamping. The CLI
checks cover validation without sampling, every observed JSON allocation
failure, and the exact 319/320-byte output boundary. The actual public header
is compiled and executed with either or both feature gates disabled. Status readers take fixed-size identity copies under their own lock, so a scan or connect reassigning the owner's Strings can no longer free a buffer under them; the longest possible identity still fits the CLI buffer.

`--baseline <component-dir>` compares ordinary CLI JSON/text bytes and parsed
escaped values against the saved pre-change command using its real CompactJson
implementation. Python's strict JSON decoder checks emitted JSON. A serializer
regression case uses the real vendored ArduinoJson to cover every control byte
(including explicit-length embedded NUL) in object keys and values, quotes,
slashes and UTF-8, compact/pretty output, and measured versus written length.
Allocation policy, String storage and RTOS stack sampling are host boundaries;
this suite does not claim coherent whole-ring snapshots, radio lifecycle or
physical ring qualification. ASan/UBSan follow `HW1_SANITIZE`.

`ble_link_status` runs `BLE_LinkStatusCore.h`, the words every Bluetooth
status surface shows (docs/PHONE_CONSOLE_VS_PIPE.md): the running role and the
saved mode it differs from, the relay pipe (the P4 central to the phone app's
GATT server) and the app console (the phone app's client on this device's
command service: alone in server mode, or on the pipe's own link in G2 client
mode). It checks every role, pipe and console input combination, including the
client host's (registered, enabled, a slot on the pipe's link, the pipe ready,
the secret): "n/a here" where it is not registered, "off", "waiting" for the
phone link, "no secret", then connected or signed in only while the pipe is
ready; each console id distinct. Phone link v2: wherever the pipe runs the
console stream comes first (open on a ready pipe is connected or signed in, a
joined design-A slot still shows, offered and not open is waiting, the phone
pipe's host has nothing else), and the phone link block's words: the stream
console (`off`, `needs-code`, `waiting`, `not-wanted`, `opening`, `open`,
`signed-in`, `closing`, `backoff`, `stopped`), relay `on`/`off`/`unknown`, a
distinct name for every pipe error code 0..18 (`service-off`, `unsupported`,
`paused`; codes past 18 are `other`), the slot counts pinned to
BLE_PhonePipeCore.h, and the text lines exactly and bounded (worst case 539
bytes). It also checks the column limits (pipe
reasons at most 9 characters so `Pipe: fail: <reason>` fits 21 OLED columns;
the lens brief at most 6), and that every status label `BLE_PhonePipe.cpp` can
set is classified, so a new failure label never reads as success. Source checks
pin the atomic step and enrolling reads, the Step/PipeStep order, the
never-blocking console view counting the stream's row, the snapshot's
compile-time mirrors of PipeConsoleState and the end-reason count, the secret
read by length only, and the advertising re-arm below the maximum on connect
and disconnect.

`g2_repair_schedule` runs the G2 one-temple repair as shipped: the schedule
(`G2_RepairCore.h`) and, from `G2_Glasses.cpp`, the heartbeat tick, the queue
path, the setup worker's finish hook, the wear and gesture nudges, the status
text, the reconnect ladder's admission and `g2recover`, on a fake clock with
fake links, peer intent and setup worker. Over a simulated day with every try
failing: tries 1-7 at 5, 10, 30, 60, 120, 240 and 300 s, then every 10 minutes,
no restart at the cap (the v0.99.9 bug), one "retrying every 10 min" event, and
the clock wrapping during the fast tries, and one `repair` SSE push per change
(1/7 .. 7/7, slow). The wear edge sends a slow repair back to try 1; a lens
gesture (click, scrolls, double click, long press; not foreground, exit or IMU
events) does too, at most once per 30 minutes in an episode; a running slow
try counts as try 1. Held (user disconnect, no owner authority, the surviving
temple's address not saved) shows at the next heartbeat and makes no queue
attempt; busy does not hold; neither leaks a claim; a late held result never
marks another caller's running try. One try at a time. The episode ends with
both temples up or down or the client off, but not while a try runs: a tick
during the repair's own connect (both temples up while promoting) changes
nothing, and a refused promotion is one failed try in the fast and slow
phases. The ladder returns COALESCED while one temple is up. Source checks pin
the wear-edge call, the three lens event parsers, the worker's attempt
number, the SSE `rp` key before `w`, and the web JSON field. ASan/UBSan follow
`HW1_SANITIZE`.

`g2_connection_status` executes the production state-name function, topology
snapshot, public connection snapshot and shared legacy text formatter. All seven FSM
states, both lifecycle presence gates and four temple-link combinations are
covered, including off versus initialized idle, partially connected glasses,
physical links before session promotion, unknown batteries, maximum counters,
independent caller copies, and every output capacity through the complete text
length. Formatting a captured copy does not resample the owner. Null/zero-length
output skips capture. The `repair=` field shows the one-temple repair only
while running with exactly one temple up, and a captured copy keeps it after the
schedule changes. `--baseline <component-dir>`
compares successful text bytes with saved production source (the repair field
is new, so the text differs from pre-repair sources). The real disabled
header is separately compiled and executed by `g2_disabled_header` for all three
feature/SDK-off configurations; unavailable snapshots leave caller data alone.
That suite also compiles the same eight typed rendering signatures and default
calls with G2 enabled and disabled. Disabled calls return false without invoking
callbacks or modifying pager/output state. Six test cases cover those four gate
combinations, standalone `G2_RenderTypes.h`, both public/protocol include orders,
and the public header's independence from wire-protocol declarations.
Both suites follow `HW1_SANITIZE`. The runtime-state and topology values are
captured under their existing lifecycle/topology guards. During initialization,
status skips temple objects being reconstructed and returns off/default values
until the runtime is published. Driver state carriers and critical-section
primitives are host boundaries; MTUs, batteries and traffic counters still have
independent writers and are not a coherent whole-device snapshot.

`g2_client_lifecycle` executes the production `initG2Client()`/`deinitG2Client()`
bodies (the glasses runtime alone since the 2026-10-21 lifecycle rework), the
role/init claims, the private runtime publication/read helpers, the combined
status, the page/tap worker admission and the UI-worker creation gate
(`g2UiWorkerCreationOpenLocked`, with the production `gClientUiCreationOpen`
flag spliced in). The shared stack (`bleStackUp()`), resource allocation,
clocks and worker outcomes are host boundaries; the stack's own refusals are
checked with five of its real reason strings and allocate nothing. It
exercises the startup refusals (role, claim, init, renderer, stack, memory,
temples, scan), two scan failures that reset the runtime without touching the
host and retry on the running stack, eight eager-worker combinations, the
repeat-init early return, the exact stop sequence (notes release after the
disconnect, the G2-only setup-slot wait, the UI-worker release before the last
start-block release, worker mask 0 at the end; a release timeout is non-fatal
and keeps page/tap), twelve deferral barriers and twelve memory cases (cold
start, restart after a full stop recreating all three workers, restart with
kept workers, refusal precedence). The creation gate is pinned by producer
injection: a producer racing a start that the stack, the memory floor or the
scan then refuses creates no worker; one racing the eager creation after
admission creates all three, which the start then reuses; a producer racing
the stop, a stopped client and a fresh boot create nothing; a published
runtime lazily recreates each missing worker. Text checks hold the init order
(`bleStackUp()` before the temple quiesce, the `g2client` memory check after
it and before `templeInit`, `g2RegisterBlePeer()` after the publication,
nothing after `g2SessionEnsureWorker()` but the log and `return true`), the
ten deinit calls in order, the banned calls (`phonePipeStop`, `bleStackDown`,
`g2RingDisconnect`, peer arming and the host calls now owned elsewhere), that
`bleConsoleRegisterOnClientHost()` is called from `BLE_CentralStack.cpp`
alone, and that each of the three creators reads the gate once, negated,
inside the lifecycle critical section and claims its slot there, with every
write of the admission flag under the same lock. The production state writers
and four concurrent readers use real host mutexes across 12,000
publication/reset cycles and 100 real init/deinit control-flow cycles. It does
not qualify BLE resources, temple teardown, scheduling on-device or
independently sampled telemetry. Both pipe lanes build the same code (the pipe
no longer gates either function). `--component <dir>` reads another copy of
the component for mutation runs; `--baseline` is gone (a saved pre-rework
source implements the old contract and would not compile against these
fakes). ASan/UBSan follow `HW1_SANITIZE`; run
`python3 test_g2_client_lifecycle.py --thread-sanitize` separately for
ThreadSanitizer (do not combine sanitizer modes).

`g2_redraw` executes the production command-completion redraw helper and the
lens worker's Redraw dispatch branch, using the shipping cookie/job declarations.
It covers original command metadata after navigation advances, stale generations
before and after queue admission, deferred rendering, both allocation failures,
queue rejection, null callbacks, and accepted/rejected ownership cleanup. The
RTOS queue and allocation failures are host boundaries; this does not qualify
physical lens rendering or device scheduling. ASan/UBSan follow `HW1_SANITIZE`.

`g2_power_completion` compiles the full Power page with the production redraw
helper and worker dispatch. It checks copied result lifetime, deferred display,
leaving/reopening pages and submenus, overlapping requests, allocation/queue
failures, submission refusal, and duplicate destructive confirmations. Error
rows replace timed completion banners; the pre-reboot tap delay remains.

`g2_mic_completion` executes the microphone callbacks, redraw handoff and live
renderer with microphone support enabled and disabled. It covers navigation
before and after handoff, failed teardown, a delayed container-state mirror,
exact exit-callback publication after recreation, and unchanged command policy.
Logging completion cases cover success/error deferral, stale cookies and enqueue
failure. Both suites use ASan/UBSan with `HW1_SANITIZE`; driver, display, command
submission and RTOS operations are controlled host boundaries, not physical
glasses or scheduler qualification. A SHUTDOWN that fails while the page, navigation and session are still current is handed back to later ticks a bounded number of times instead of ending the live worker.

`sensorlog_selection` compiles the logger's production sensor catalog, selection
helpers, shared argument parser and changed-only settings mutator. It covers
every initial selection/toggle bit, queued toggle intentions, intervening set
replacement, aliases/case/whitespace, all/none, legacy trailing commas, query
labels, status names/formats, rejected-list rollback, CSV refusal and buffer
allocation failure.
Persistence and notification calls are boundary observations; this does not
qualify flash writes, CSV output, task races or physical sensor logging.
`--baseline <component-directory>` replays the same assertions against the saved
old sensors command branch and reproduces partial invalid-list mutation and
the missing toggle operation. ASan/UBSan follow `HW1_SANITIZE`.

`sensorlog_status_rows` runs the logger's status forms and row builders,
extracted unchanged from `System_SensorLogging.cpp` / `.h`: `sensorlog status`
and `sensorlog status json` print from one `SensorLogStatus` reading (the text
keeps its wording, the json is one whole object or the `response too large`
object, and both agree for every selection, format and running state); a CSV
row always carries the columns its header lists (a selected sensor without a
reading writes empty fields); and the log snapshot keeps the GPS satellite
count and fix quality without a fix. Source contracts pin the upload admission
arithmetic in `WebServer_Server.cpp` to 64 bits, which a 64-bit host cannot
show by running the handler. Globals, the debug buffer, the clock and the GPS
cache read are host doubles; this does not qualify file writes, the logging
task or a real sensor.

`sensorlog_ui` executes the production G2/OLED logging rows and input handlers
with the shared catalog. It checks labels/checkmarks, one selection read per
render, independent toggle requests despite stale displayed masks, repeated
taps, negative/out-of-range indices, guest/navigation gates, authorized executor
handoff, G2 cookie/completion forwarding, start/stop paths and the remaining
interval/format/back controls. UI, input, logger reads and command submission
are host boundaries; this does not qualify physical displays or asynchronous
device execution. ASan/UBSan follow `HW1_SANITIZE`.

`settings_write_scope` executes the production synchronous save scope, batching
adapter and batch core. It covers nested scopes, an existing command owner,
foreign task/command requests, unavailable TLS/scheduler/capacity, repeated finish,
main/debug flush failures and retries, and simultaneous tasks. A scope destroyed
out of nesting order is diagnosed, closes its batch with the pending file
flushed and leaves the task's scope pointer cleared rather than dangling; one
destroyed on a foreign task is diagnosed and its batch flushes when it expires.
No settings scope
is held while the wizard waits for input. The wizard suite records this boundary;
this independent suite verifies its real persistence behavior with controlled
RTOS and file-writer boundaries. ASan/UBSan follow `HW1_SANITIZE`.

`settings_mutation` executes the production generic setting handler, all three
`setSetting` overloads, shared change finalization/notification
and persistence adapter with the real owner-scoped batch core and ArduinoJson.
OLED-enabled and disabled variants cover full integer widths and adjacent-field
canaries, complete-token parsing, malformed/overflow/nonfinite values, metadata
ranges, read-only and validation-only requests, bool aliases, repeated-off service
reconciliation, publication/persistence/stop/event order, explicit and first-row
alias labels, long string event values and secret redaction.
Changed values notify and request persistence once; no-ops leave batching clean.
Controlled argument-copy/reserve failures
leave the old String intact; failed persistence retains live RAM changes and
retry intent. Foreign-owner/background writes remain independent of an open
batch. Registry contents, task identity, filesystem writers, lifecycle commands,
OLED and String allocation failures are host boundaries, so this does not qualify
device concurrency or physical flash durability. ASan/UBSan follow
`HW1_SANITIZE`, and sanitizer diagnostics fail the test.

`features_list_only` pins the 2026-11-20 rule that `features` only lists. It
compiles the production `cmd_features`, its listing, detail and refusal views
and the registry helpers they read (System_FeatureRegistry.cpp), with
CommandArgs and `argWantsJson`, against fixture rows, a fixture switch table
and a fixture live-state reader. The unit defines no setting writer, event
poster or owner transition, so a call to one does not compile. Every query
(`features`, `features <id>`, `features json`) and every refused
`features <id> <on|off>` leaves each row's flags unchanged; the refusal names
the row's own commands (now, at boot, allowed, the reboot note, one shared
command named once) or says the row is not built in, has no switch, or is
unknown; the validate pass refuses the same form; the listing and detail show
built in, running now, start at boot and allowed; `features json` stays schema 1
byte for byte; a full buffer is reported, not cut off. A source half checks
that the registry posts no event and calls no owner, that nothing in the
firmware posts SYSEVT_FEATURE_TOGGLED, that every command the shipping
kFeatureSwitches table names is registered (tools/command_registry.py), that
each flag it pairs with a command is the flag that command's settings row
writes, that the "after a reboot" note sits exactly on the rows whose allowed
command itself says a reboot is required (today only i2cbusenabled, the one
allowed switch left), that only the I2C bus row has an allowed switch, and that
the help row no longer offers the on/off form.

`sensorlog_settings` compiles the production numeric logger handlers, complete
generic settings editor, shared argument parser and changed-only settings
mutator, with R1 Health enabled and disabled. It covers queries and errors when
live/saved state differ, validation-only requests, running-CSV selection refusal,
all numeric masks, strict generic integer parsing and changed-only finalization,
shared text/numeric
format policy and TRACK's GPS-only persistence. Persistence observations check
publication order without claiming atomic snapshots. Enabled cases also run
the production health setter/restart helper: added-R1 header restarts, existing
CSV shortcuts, TEXT/TRACK coercion, stop/restart decisions, failure behavior,
custom paths and fallback intervals. Persistence, path shaping and session start
are host boundaries; this does not qualify actual file writes or scheduling.
`--baseline <component-directory>` reproduces the old query/error live-state
resets, numeric CSV bypass and numeric/text TRACK divergence. ASan/UBSan follow
`HW1_SANITIZE`.

`gpslog_start` executes the production GPS-log orchestration, GPS queued-start
adapter and auto-start command, with the real shared argument parser and settings
mutator. It checks strict interval input, disabled/running/health preflight,
queued versus ready GPS outcomes, borrowed-result lifetime, failures at each
owner boundary and auto-start preferences enabled only after confirmed startup.
A separate unit executes the actual logger typed start, saved-parameter restore,
configured-session entry point, boot helper and explicit-start adapter. It covers
literal paths containing spaces/quotes, default paths, legacy restore/clamp
limits, boot-only health policy, admission failures and session publication.
The transient live-track entry point starts a GPS-only TRACK capture at a
literal path without persisting it as the last-used session; the saved
selection is restored when that session ends by either stop path, a refused
start changes nothing, and typed starts still persist.
VFS, sealing, clock/path shaping, persistence and GPS queue outcomes are controlled
host boundaries; this does not qualify storage devices, GPS reception or task
scheduling. `--baseline <component-directory>` reproduces the old orchestration's
partial-input acceptance, early auto-start changes, ignored failures and false
active/ready claims. `--start-baseline <saved-System_SensorLogging.cpp>` verifies
that the extracted filesystem/admission/publication body is unchanged apart from
indentation. ASan/UBSan follow `HW1_SANITIZE`.

`health_logging_ui` executes production G2 Health model requests, request reset,
worker dispatch and log-only completion handling, plus OLED Health input and the
Health HTTP handler with the real unified web executor. It covers queued toggle
intent, guest/auth gates, web source/user/session forwarding, stale-session reply
rejection, owner/helper failures and unchanged unrelated Health actions. Logger
mutation, transport, session lookup and command submission are boundaries; this
does not qualify physical glasses, radio traffic or the device scheduler.
ASan/UBSan follow `HW1_SANITIZE`. The model mirrors the toggle command's fate (queued, refused by the executor, error result) so the lens status line can show it for a few seconds.

`g2_status_browser` runs extracted production browser JavaScript with Node.js,
using small DOM/network boundaries. It checks structured connection status
rendering and button state independently of the human-readable status text,
preserves control metadata handling, and compares ordinary rendering against
the historical text-parsing production with `--baseline <component-dir>`.
The same runner also executes the complete shipping page script with event,
timer and request boundaries. It covers unknown initial mode, command/refusal
and transport failures, authoritative mode/reconnect readback, repeat clicks,
stale responses, slow polling, persistent connected-ring feedback, and the
G2/Sensors/microphone link guards. It verifies navigation issues no microphone,
source-selection or recording command. The Bluetooth page shows the app
console's clients/advertising line only for the console server, never for the
console on the relay pipe's link (Glasses+pipe, which waits for the phone link
by default), and the "Use app console" confirm asks to switch to server mode
for a direct app connection. It runs when Node.js is found at
configure time (otherwise it is registered but disabled); set
`HW1_NODE_EXECUTABLE` for a runtime outside PATH. These tests do not qualify physical G2 traffic, radio lifecycle,
actual HTTP transport, browser layout or device scheduling.

## MQTT lifecycle coverage

`mqtt_boolean_commands` executes all thirteen production MQTT toggle handlers
with the real shared boolean parser, validation macro and settings mutator.
It checks empty queries, every supported alias with case/whitespace variations,
invalid inputs with no mutation or stop request, validation-only calls, exact
responses, changed-only persistence/notifications and disable-stop ordering.
Gamepad and ANO Encoder labels run separately. The subscription enable response
retains its restart instruction; enabling settings never starts a client.
`--baseline <component-directory>` replays the same expectations against saved
sources to reproduce the former invalid-input and alias regressions.
String storage, persistence/notifications and stop scheduling are deterministic
host boundaries. This does not qualify a broker connection or actual teardown.
ASan/UBSan follow `HW1_SANITIZE`.

`mqtt_lifecycle_gate_tests` compiles the production packed atomic gate and
checks command-first, stop-first, duplicate-stop, stop-during-start, retry, and
two-thread admission-versus-stop orderings. The companion
`mqtt_lifecycle_contract` guard pins the firmware integration: MQTT command
admission precedes queue submission and lasts through response publication,
all driver teardown is main-loop-affine, both shutdown commands use the shared
request path, and pending teardown runs before periodic publication.

To build and run only the dependency-free map geometry target (without the
Python-backed allocation inventory):

```sh
cmake --build /tmp/hw1-hardwareone-host --target map_viewport_core_tests
/tmp/hw1-hardwareone-host/map_viewport_core_tests
```

Runs in well under a second. ASan and UBSan are on by default — turn them off
with `-DHW1_SANITIZE=OFF` if your toolchain lacks them, but understand what you
lose: the walk-back in `utf8TrimPartialTail` reads *backwards* through the
buffer, and an earlier draft ran off the front of it for a one-byte window
holding a lone continuation byte. Without a sanitizer that is invisible, because
the byte it reads is almost always mapped.

## What `test_llm_utf8.c` covers, and why

`utf8TrimPartialTail` shortens a served chunk so it never ends inside a
multi-byte UTF-8 sequence. It sits at the two offset-addressed poll endpoints
(`/api/llm/result` and `llmresult json <offset>`). Getting it wrong corrupts
answers silently, so the properties are asserted rather than assumed:

- **Range and no out-of-bounds**, exhaustive over every byte string of length
  0..2 against exact-sized `malloc` — the OOB case is reachable at length 1.
- **No split leak**: every prefix of a valid stream trims to a whole number of
  complete sequences, across ASCII, 2-, 3- and 4-byte characters and CJK.
- **No false trim**: a chunk already ending on a boundary is returned intact.
- **The stall invariant**, `n >= 4` implies the result is `>= 1`. This is what
  guarantees a 511-byte serving window can never serve nothing, and therefore
  that the endpoint cannot livelock. It is the single most important assertion
  here: a fix that stalls the stream would be worse than the bug it replaces.
- **Named regressions** so two specific rejected designs cannot come back: a
  "never return 0" guard (it fires on the ordinary slow-streaming case and
  re-introduces split characters) and an unbounded walk-back.

Deliberately **not** asserted: idempotence. `trim(trim(p,n)) == trim(p,n)` holds
on well-formed input but fails by design on malformed bytes, which pass through.
Asserting it would forbid that pass-through — and passing malformed bytes
through is what keeps the caller's cursor advancing.

Both rejected designs above were verified to fail this suite before it landed.

## Map viewport geometry coverage

`map_viewport_core_tests` compiles the production
`System_MapViewportCore.h`. It verifies cardinal displacement against the
actual 288×144 visible axes, 90-degree screen-to-geographic rotation, OLED/G2
maximum-zoom scale clamps, rotated edge overscroll, and equivalence between one
accumulated multi-step request and repeated isolated requests within float
center precision. The test has no Arduino, ESP-IDF, renderer, filesystem, BLE,
or map-file dependency.

## Memory allocator and tracker coverage

`memutil_tests` compiles the real `System_MemUtil.cpp` against a deterministic
ESP-IDF heap-capability stub. It verifies all five routing policies, strict
placement, PSRAM fallback accounting, bypass behavior, zero-sized requests,
calloc overflow, failed-realloc pointer preservation, exact-once
`realloc(ptr, 0)`, and ArduinoJson's matching realloc contract.

`memtracker_core_tests` compiles the real dependency-light registry used by the
firmware tracker. It verifies repeated-tag aggregation, actual DRAM/PSRAM byte
accounting, fallback/failure counters, deterministic top-K snapshots,
full-table handling without out-of-bounds writes, updates to existing tags at
capacity, reset epochs, and counters beyond 4 GiB.

`espnow_pair_core_tests` compiles `System_ESPNow_PairCore.h` (the
discover-then-confirm pairing state) as is and runs two modelled devices over a
lossy broadcast channel, with glue that mirrors `System_ESPNow.cpp`. Cases: the
happy path, a lost first PAIR_REQUEST, a lost PAIR_ACCEPT (recovered by the
re-sent REQUEST or by the unsolicited ACCEPT copies), the initiator timing out
with the target paired and the later re-request repairing that half pair, a
duplicate REQUEST while pending (one prompt, same identity) and after an answer
(same answer), the discovery window closing while the prompt is open (no
REJECT, a late accept pairs both sides), an unrelated KEY_EX_HELLO while
waiting (not an accept), a lost REJECT, a plain timeout on both sides, a closed
discovery window, an initiator that sends once (older firmware), and the
millis() wrap. A source contract pins the handler, sweep, command and OLED call
sites and that no KEY_EX_HELLO hook answers a pair request.

`raw_allocation_inventory` runs the comment/string-aware scanner over the main
`components/hardwareone` application. The pre-migration boundary is 12 direct
`malloc` calls plus five existing `calloc` calls. `System_MemUtil` itself,
tests, and third-party code are excluded deliberately; changing that boundary
requires an explicit test update. The standalone recovery updater is outside
this component and currently has six first-party `malloc` calls of its own.


## Portable JPEG decoder

`jpeg_portable` compiles the production `HAL_JPEG.cpp`, `HAL_JPEG_Software.cpp`
and `HAL_JPEG_P4.cpp`. The default tests use controlled codec/SDK/heap boundaries
and execute metadata validation, all header truncations, duplicate frames,
segment/scan errors, capacity limits, explicit backend selection, software
PSRAM-to-internal-memory fallback, partial-result cleanup, and automatic fallback.
The software harness also extracts and executes the production G2 adapter,
checking its shared allocator tag/policy and software-only selection when the
runtime PSRAM bypass is enabled. Custom allocation failure must not escape that
policy by silently using the raw SDK allocator.
The P4 backend is compiled with no SoC capability, with an unqualified driver,
and with both capability and driver qualification enabled. Its SDK mock
covers RGB channel order, 4:4:4/4:2:2/4:2:0 padded-row compaction, input copying,
allocation/engine/process/output-size/deletion failures, retry after failure,
and nonblocking admission while another call owns the decoder. ASan/UBSan follow
`HW1_SANITIZE`. These mocks cannot establish real DMA or timeout behavior.

To also compile the installed real TJpgDec and the legacy `fmt2rgb888` converter:

```sh
python3 -B components/hardwareone/test/host/test_jpeg.py --sanitize \
  --jpeg-component /path/to/managed_components/espressif__esp_jpeg \
  --camera-component /path/to/managed_components/espressif__esp32-camera
```

Or configure CMake with `-DHW1_JPEG_COMPONENT=... -DHW1_CAMERA_COMPONENT=...`.
This optional corpus suite checks software decoding for fifteen original
synthetic fixtures at all three external TJpgDec optimization levels, including odd dimensions, grayscale and red/blue channel
checks. Whenever the legacy converter succeeds, output must match it byte-for-byte.
The real FASTDECODE=1 regression reproduces three legacy 4:2:0 workspace failures;
FASTDECODE=2 exhausts legacy scratch for all fifteen baseline fixtures. The new
configuration-sized private workspace must decode all fifteen in every mode.
Progressive JPEG fails cleanly in both the legacy and new software path;
progressive support is not introduced. Another 192 concurrent decodes per optimization level compare
all pixels to the sequential output, exercising the new per-call workspace.

The third-party wrapper's input callback signature is adapted in a temporary file
from 32-bit `unsigned int` to host `size_t` to match TJpgDec on 64-bit desktops.
Its body, the decoder algorithm and the legacy conversion implementation are
unchanged. ESP-IDF heap calls are mocked with the host allocator; this does not
measure physical PSRAM placement, S3 ROM-decoder behavior or P4 JPEG hardware
speed/quality. The normal test run explicitly reports when real corpus parity
was not requested. Fixture regeneration needs Pillow; normal runs use committed
JPEG files and need no image-generation dependency.


## Shared battery backend and power admission

Run the focused portable battery checks:

```sh
python3 components/hardwareone/test/host/test_battery.py --sanitize
```

The runner compiles the production `System_BatteryPolicy.h`, the unmodified backend,
lifecycle, snapshot/accessor, calibration and JSON functions extracted from
`System_Battery.cpp`, and the exact OTA `powerIsSafe()` function. SDK ADC,
FreeRTOS synchronization, GPIO, clock, event delivery and MAX17048 transport are
mock boundaries. The JSON checks use the repository's real ArduinoJson library.
Tests cover:

- Estimated SOC bounds/monotonicity; unknown presence, USB and charging;
  stale/error handling, successful sample at boot millisecond zero, and wraparound.
- Actual board selection for EYE, generic P4, classic Feather ESP32, XIAO Sense,
  and FeatherS3 fuel gauge, including an explicitly false EYE board marker.
- P4/S3 curve calibration and classic ESP32 line calibration (eFuse, nominal Vref,
  and strict eFuse-only mode); GPIO-derived unit/channel use, partial reads,
  saturation, conversion/configuration/calibration failures, and resource cleanup.
- Concurrent recalibration blocked behind a paused hardware read, with readers
  retaining the previous coherent snapshot until the operation completes.
- Deferred fuel-gauge probe, retry after I2C failure, independent VBUS freshness,
  CRATE deadband, disabled monitoring, and battery-full notification hysteresis.
- Telemetry nulls for unknown/failed/stale readings and complete JSON serialization
  within the CLI/web response buffer.
- OTA admission under ADC, fuel-gauge and unmonitored build policies, including
  invalid/stale values, independent fresh VBUS and the explicit force override.

These checks do not validate resistor values, analog accuracy, actual SDK ADC
routing, a connected cell, battery chemistry, or the charger circuit. Target
builds and physical/meter comparison remain separate qualification steps.


## ESP-NOW remote file RPC requester gate

`espnow_fs_transcript_privacy` (see "Filesystem and G2 PSRAM output") also
checks who the FS_LIST/STAT/GET handlers serve. A request from any MAC other
than the bonded peer, or from the bonded peer without a live bond session, is
answered `PERM_DENIED` before an identity is installed or the filesystem is
touched; a served request runs under the reserved bond identity rather than
SYSTEM. A second compile with `ENABLE_BONDED_MODE=0` expects every request to
be refused. Source-contract checks confirm that `/api/bond/fs/list`,
`/api/bond/fs/get` and the OLED peer browser pass `hasAdminPrivilege` before
sending a request. `--source-dir <component-directory>` replays the former
SYSTEM-identity serving.

## ESP-NOW peer metadata text and page rendering

`espnow_metadata_text` extracts the production metadata types and runs the
metadata text policy, the METADATA RESP/PUSH receive handler, `processMetadata`
and the `espnowroom`/`zone`/`tags`/`friendlyname` setter (`metaGetSet`) against
host doubles for the registry, mesh cache, settings writes, events and logging.
Hostile peer text is replaced byte for byte with `_` before it reaches the mesh
cache or the persisted registry, with one WARN that carries the count and the
sender MAC only; the setters reject control bytes and `< > " ' & \` before any
write or event; clean ASCII and UTF-8 values are stored unchanged. This suite
uses ASan/UBSan when `HW1_SANITIZE=ON`.

`espnow_page_render` runs the shipping `web_assets/espnow-core.js` (device list,
peer metadata panel) and the `WebPage_ESPNow.h` script chunks that define the
graph view, the status apply, the mesh role card, the local metadata / channel
card and the button handlers, unmodified, against a fake `hw`/DOM on whatever JavaScript engine
`tools/webui/js_engine.py` finds (node, deno, jsc, ...). Peer names and metadata
must reach `innerHTML` as literal text, clean text must render unchanged, and the
Pair button must pass a discovered name with quotes intact. Pairing is encrypted only
(ESP-NOW 1.0): Pair on a discovered device sends `espnowpairrequest <mac>` (P1), and the
Add Device form has one Pair button, which runs `espnowpairsecure` and says to pair from
the other device too (B7). The page must decide
from the json replies of `/api/espnow/status` only (nine slots, the last one
display text): a text reply decides nothing, a slot that is not json is
tolerated, settings strings are shape-checked and escaped, a reply that cannot
be applied shows an error, the Initialize button opens the name setup from the
json name fields and the mode toggle reads `espnowmode json`. The source
contracts also pin, in `System_ESPNow.cpp`, the `espnowstatus json` size guard,
`nameValid`, the internal-RAM heap figure in heartbeats and bond status, and the
metadata setter limits taken from `V4PayloadMetadata`; no host test compiles
those lines. Static source
contracts on the same renderers run first; `--contract-only` runs only those.
When no engine exists and the contracts pass, the suite exits 77, which CTest
reports as skipped. Both suites take `--source-dir <component-directory>` to
replay other production sources, such as the pre-fix tree.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^(espnow_metadata_text|espnow_page_render)$' --output-on-failure
```

These checks do not open a browser or exchange real ESP-NOW frames.

## Untrusted names on web pages

`web_untrusted_names` (readiness G-50/G-51) covers the HTML sinks for file names
and beacon SSIDs. Static source contracts run first: the shared file explorers in
`WebServer_Utils.h` (`window.FileBrowser`, `createFileExplorer`,
`window.BondFs.renderExplorer`) must escape names, path segments, peer labels and
status text, and must pass paths to inline `onclick` handlers through
`FileBrowser.jsArg` rather than between quotes; the `WebPage_Settings.h` Wi-Fi scan
must escape the SSID; the Maps details panel must escape the map file name;
`escHtml` must escape `&` as well (an entity such as `&quot;` in a name would
otherwise decode into a quote inside `onclick`); the ESP-NOW remote browser
(`web_assets/espnow-core.js` `browseRemoteFiles`, which navigates by the peer's
folder names) must not concatenate the raw path or CLI reply into `innerHTML`. Then
both file-browser `<script>` blocks and the Settings scan script run unmodified on
whatever engine `tools/webui/js_engine.py` finds, against a fake `hw`/DOM: names
and SSIDs holding `<img src=x onerror=...>`, quotes, backticks and entity
spellings of a quote (`&quot;`, `&#34;`, `&#x22;`) must render as
literal text, each `onclick` (entity-decoded, as a browser does) must hand the
original path to its function, and clean names render unchanged. Without an
engine the suite exits 77 after the contracts (skipped). `--contract-only` and
`--source-dir <component-directory>` work as in `espnow_page_render`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^web_untrusted_names$' --output-on-failure
```

`web_mqtt_sensor_rows` covers a sink the server writes itself: the External
Sensors table of the MQTT page shows the last segment of a received topic and
the first 100 bytes of its payload, both chosen by whoever publishes to a
subscribed topic. It compiles the production `streamMqttExternalSensorRow`
(`WebPage_MQTT.cpp`) with the production `streamHtmlEscaped` /
`streamHtmlEscapedChecked` (`WebServer_Utils.cpp`) against an httpd double.
Markup in either cell must arrive as text and read back unchanged; a clean row
keeps its bytes; the value is cut at 100 received bytes and at its first NUL;
an empty cell sends no zero-length chunk (the terminator of a chunked
response). Source contract: the table loop sends every row through that
function and formats no received text with `%s`, and the "Subscribed topics"
line sends the `mqttSubscribeTopics` setting through `streamHtmlEscaped`. Not
covered: the rest of the page, whose text comes from settings. It takes `--cxx`, `--sanitize` and
`--component <directory>`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^web_mqtt_sensor_rows$' --output-on-failure
```

`web_auth_cookie_reads` covers the prologue every authenticated handler runs:
`WEB_AUTH_OR_RETURN`, `makeWebAuthCtx` and `tgRequireAuth` with the production
`AuthContext`, on the real Arduino String with its heap calls counted
(`arduino_string_counted` in `arduino_string_host.py`). A cookie session reads
the cookie once and makes two String blocks (the URI and the session id), the
same session id reaches `isAuthed` and the handler, the auth log arguments are
those of before, and a refusal sends the denial and logs nothing. Source
contract: the session-id form of `isAuthed` and the session list, sensor
status, CLI and CLI batch handlers read no cookie, and every direct
`tgRequireAuth(ctx)` caller builds `ctx` with `makeWebAuthCtx(req)`. It takes
`--cxx`, `--sanitize` and `--component <directory>`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^web_auth_cookie_reads$' --output-on-failure
```

`auth_log_paths` covers which requests and commands reach the auth log. It
runs the production `logAuthAttempt`, its path test `isSecurityAuditPath`,
`authLogPath` and `appendAuthLogLine` (`System_Debug.cpp`) on the counted
Arduino String against a recording capped-append double. A table of paths
fixes the file and the exact line of each security event (`/login` and
`<transport>/login`, `g2/pair`, `espnow/bond`, also behind `%2F` and `%20`)
and that every other path writes nothing (`/login-help`, `/LOGIN`, `/login/`,
`g2/pair/x`, `espnow%20bond`, an empty and a null path); a web request and a
command that are not security events make no String block. It takes `--cxx`,
`--sanitize` and `--component <directory>`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^auth_log_paths$' --output-on-failure
```

`command_trace_copies` covers the redacted copies that only a debug trace
reads. It runs the production `submitAndExecuteSync`, `submitCommandAsync`,
`releaseSyncExecReqOwner` and `redactOutputForLog` (`System_Utils.cpp`) and
`broadcastOutput` with its body `broadcastOutputRouted` (`HardwareOne.cpp`)
with the production command types, on
the counted Arduino String; the trace macro records a line and evaluates its
arguments only while the trace is on, the command redactor is a counting
double. With the trace off a submitted command is not redacted and
`broadcastOutput` makes no String block; with it on, on a full queue and on
a timeout the lines are those of before (one redaction per sync submit,
shared by its traces and the timeout error). The queued copy of a result is
the result itself, or the placeholder for a private STT result, and a
Bluetooth session still gets the original. Source contract: cmd_exec
redacts its line only inside its trace, and the production `DEBUG_CMD_FLOWF`
(`DEBUGF_QUEUE_DEBUG`) evaluates its arguments only inside
`if (isDebugOutputEnabled(flag))`, as the harness's copy does. It takes `--cxx`, `--sanitize` and
`--component <directory>`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^command_trace_copies$' --output-on-failure
```

`redact_audit` covers the command audit redaction. It runs the production
`redactCmdForAudit` (both overloads), `maskMacTokens`, the rule table and its
helpers (`System_Utils.cpp`) and `CommandArgs` (`System_Command.cpp`) on the
counted Arduino String built with the device's inline capacity (13
characters); `resolveCommand` is the production name matcher
(`System_CommandLookupCore.h`) over a name list. The line is parsed once for
the rule loop, each row's words are read in place from its prefix, and the
`login` verb is found without a lower-cased copy. A fixed corpus of about
21600 lines (every rule prefix in sixteen spellings with 33 tails, the login
forms, blank lines, bytes 0x80-0xFF, embedded NULs, lines of 13 to 1024
bytes, 4000 random lines) is run through the one-argument form, the
two-argument form with and without an entry, and the one-argument form again
on its own result; the report's digest must equal that of the tree before.
Every registered command name (with none to thirteen words, upper case, tabs,
a quoted word, a password-like word) is run the same way, and in the same
binary every rule row's match must equal a copy of the old match.
`--baseline <directory>` builds an older tree beside it and compares both
reports line by line. The String blocks of one call are pinned (`status`
0, before 27; a 20-character line of short words 2, before 58), and a source
check keeps every rule prefix to lowercase words separated by single spaces.
It takes `--cxx`, `--sanitize`, `--component <directory>`, `--baseline
<directory>` and `--print-counts`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^redact_audit$' --output-on-failure
```

`command_identity_copies` covers the copies of a command's identity on the
way to its result. It runs the production `broadcastOutput`,
`broadcastOutputRouted` and `deliverCommandResult` (`HardwareOne.cpp`, with
`redactOutputForLog`) and `TaskIdentity`, `getOrCreateSlot` and
`ExecIdentityGuard` (`System_AuthIdentity.cpp`) on the production command
types and the counted Arduino String with the device's inline capacity; the
line queue, the BLE sender, the serial port, the session registry and the
task-local storage are doubles. For five origins x all 64 masks of the six
route bits x three session ids x three results x validate-only x the serial
kill switch x five session states x a CLI mode owned or not x the trace on or
off (230400 deliveries, plus `broadcastOutput` itself for the live ones) the
transcript of the console bytes, the drain wait, the queued copy with its
route and prefix, the BLE reply, the broadcast lines and the traces must have
the digest of the tree before; `--previous <directory>` builds an older tree
and compares line by line. The serial mirror is no copy of the context: a
serial delivery whose user and address are past the inline capacity makes 3
String blocks (before 5). The identity slot is measured as it is: a web
identity installed and restored on one task makes 2 blocks the first time and
none after, and the restore frees none. Source checks: the serial console,
the UART link and `runUnifiedSystemCommand` move their local `AuthContext`
into the command and do not read it again, and `deliverCommandResult` makes
no `CommandContext` copy. It takes `--cxx`, `--sanitize`, `--component
<directory>`, `--previous <directory>` and `--print-counts`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^command_identity_copies$' --output-on-failure
```

`fs_permission_strings` covers the file permission path. It runs the
production path-rule table, `lookupRule`, the filename tests
(`hasSensitiveExtension`, `isImageFile` and their in-place helpers),
`pathWithinScope`, `resolveRole`, `checkPerm` with the six `canX` queries,
`logFsAccessDeny`, `getPermissions` and `isAdminOnlyPath`
(`System_Filesystem.cpp`) and `VFS::systemAuth` (`System_VFS.cpp`) on the
counted Arduino String, with roster, FS lock and debug queue doubles. For a
path set built from every rule of the table (the prefix, a name below it, a
trailing slash added or taken away, an upper-case spelling, `..`, sensitive
and image names) plus the empty path, the root, paths at 127 and 255
characters and paths with an embedded NUL, it prints the matched rule, both
filename tests, `isAdminOnlyPath` and, for eleven identities (anonymous,
guest, user, admin, super, AuthBypass, an unknown name, system, two scoped
system contexts, a scoped user), the six answers, `getPermissions` and the
reason `logFsAccessDeny` gives for each refusal; then a `pathWithinScope`
matrix and the contexts `systemAuth` builds. The report must equal
`fs_permission_answers.txt`, printed by the tree before these functions
stopped copying. No permission query makes a String block, and `systemAuth`
makes one for its `system:<reason>` text and one for a scope longer than the
inline capacity. It takes `--cxx`, `--sanitize`, `--component <directory>`
and `--print` (write the report instead of comparing it).

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^fs_permission_strings$' --output-on-failure
```

`guarded_path_normalize` covers the guarded file calls. It runs the
production `normalizeFsPath` and permission path (`System_Filesystem.cpp`)
and `VFS::normalize`, `getStorageType`, the backend dispatchers (`exists`,
`open`, `mkdir`, `remove`, `rename`, `rmdir`) and the guarded calls in front
of them (`System_VFS.cpp`) on the counted Arduino String, with LittleFS and SD
doubles that record every path they are handed. For about 5800 generated
paths (every rule prefix with tails, slashes, `.` and `..`, spaces, tabs and
case changes; token sequences of up to three pieces; every byte 0x80-0xFF;
paths at 127, 128, 255 and 256 bytes and one past; embedded NULs) it prints
`normalizeFsPath`'s answer and text, `normalize`, `getStorageType` and, for six
identities, each guarded call's answer with the backend calls and their paths.
The report's digest must equal that of the tree before the guarded calls
stopped normalizing a second and a third time; `--baseline <directory>` builds
an older tree beside it and compares the two reports line by line. In the
same binary the backend path of every normalized path must be
`normalize(normalizeFsPath(path))`, and a guarded call on a normalized path of
14+ characters makes one String block (its normalized text). It takes `--cxx`,
`--sanitize`, `--component <directory>`, `--baseline <directory>` and `--print`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^guarded_path_normalize$' --output-on-failure
```

This does not open a real browser; a user-role upload of a hostile name and a
hotspot named `<b>x</b>` remain the on-device checks.

`json_docs_psram` reads the component sources (test/ excluded): no JSON
document is declared without an allocator (a plain `JsonDocument doc;` takes
ArduinoJson's malloc allocator, internal RAM below 16 KB), except the `doc_`
member of `hw1http::Stream`, which the firmware builds with its budgeted PSRAM
allocator. The eleven documents the 2026-11-14 heap sweep found plain
(`commandSession`, `commandFiles`, `commandNote`, the OLED transcription
`acceptReply`, the two of `cmd_stt`, `cmd_oledcameraread` and the four ESP-NOW
identity readers and writers) are `PSRAM_JSON_DOC(doc)` and their files
include `System_MemUtil.h`. `--component <directory>` reads another copy.
The harnesses that run those functions (`quicknote_service`,
`oled_transcription`, `camera_oled_preview`, `espnow_identity_writes`,
`transcription_ui_adapter`) take `PSRAM_JSON_DOC` from
`psram_json_doc_host.h`, a counting allocator, and check that each reply
document is made on it and released; a plain document leaves the counters at
zero.

`psram_json_text` runs `PsramJsonText` (`System_PsramJson.h`), the reply text
of the polled JSON endpoints, against `serializeJson(doc, String&)` on the
real Arduino core String with its heap calls counted. For documents from
`null` to 20 KB (escapes, an embedded NUL, UTF-8, every number form, the
31-character flush boundary of ArduinoJson's String writer) the bytes are
equal; the new text is one `PreferPsram` block of `measureJson + 1` bytes
with the caller's tag and makes no String block (the old path regrows its
String once per 31 characters: 619 times for 19 KB); a refused block gives the
empty text and no partial document; `wipe()` clears the text. A source
contract checks the eight sites of census-json-docs-03 (`handleLlmPoll`,
`transcriptionWebReply`, `handleEdgeImpulseDetect`, `handleBleStatus`,
`buildEspSrStatusJson`, `publishMeshPeerSensorData`, the GET branch of
`handleWaypointsApi`, `handleSessionsList`) write their reply through it.
`ble_info_status` and `edge_impulse_jpeg` run two of the handlers with it.
It takes `--cxx`, `--sanitize` and `--component <directory>`.

`reused_reply_buffers` runs the production `handleLogs` (`/api/cli/logs`, with
the production `WebMirrorBuf::snapshotTo`) and `publishMqttSensorData` (sensor
sections and ESP-NOW compiled off, with `releaseMqttStateJson`) against doubles
that record every `ps_alloc`. Both keep one `RequirePsram` block (8 KB for the
logs reply, 16 KB for the state document) made by the first call and reused by
the next ones; when PSRAM refuses, a call borrows a `PreferPsram` block and
frees it, as every call did before; with no memory at all the old error reply
and warning stand. The logs replies are those of before; the state document
is built with `System_MQTTPayloadCore.h` and carries `deviceName`,
`deviceNameSet` and `system.psram_free`/`psram_total`. The kept
logs block keeps its own size: after a larger `gWebMirrorCap`, a poll fills it
only to that size. A poll that sends `?since=` with the mirror's current change
counter gets 204 with no body and no copy; any other poll gets the whole mirror
and its counter in `X-HW1-Log-Seq` (S-13). Source
contract: `stopMqttNow` releases the state buffer on both of its finished
paths. It takes `--cxx`, `--sanitize` and `--component <directory>`.

`log_line_writes` runs the log writer's `writeLogLine` (`System_Debug.cpp`)
against the real Arduino core `Print.cpp`, compiled on the host with its
`malloc` counted. For 195 lines (timestamps 0 to 4294967295, no tag, short,
21- and 47-character tags, messages from empty to 255 characters across the
64-byte boundary, `%` text, UTF-8) the bytes equal `Print::printf`'s and both
make one write; printf makes its heap block for each of the 93 lines of 64
bytes or more, `writeLogLine` none. A line that does not fit its buffer (a tag
over 47 characters with a full message) takes printf's path. Source contract:
`debugOutputTask` writes the serial line and both system-log-file lines
through it. It takes `--cxx`, `--sanitize` and `--component <directory>`.

## ESP-NOW frame rules

`espnow_wire_layout` (`test_espnow_wire_layout.py`) pins the 1.0 frame rules of `System_ESPNow_Wire.h`. It finds
every struct in the header and checks that each is `__attribute__((packed))` and has a `static_assert` on its size
and an `offsetof` assert on every field it declares; that the header states little-endian once and asserts it;
and that `CapabilitySummary` and `BondPeerStatus` live there and no longer in `System_ESPNow.h`. It compiles the
header itself (with its four rules headers over a stub `Arduino.h` and build config) together with a golden
table of every 1.0 layout, field by field, so a moved or resized field fails the build of the test
(BOND_HEARTBEAT bytes 12-15 are `reserved2` since ESP-NOW 1.0 step 2). check_retired pins the step 2 numbers: 130
(SUBSCRIBE_UPDATE) and 150 are retired (no enum value, the map says so), the earmarks 2, 16/17, 37/38, 131 and
133-135 are released, and SUBSCRIBE_UPDATE with its subscription state, `espnowsubs` and `espnowrequestevents`
is gone from the sources. Source scans
check one receive style (no `System_ESPNow*.cpp` code casts a payload to a wire struct pointer, the in-place FS
list entries excepted; no handler compares `ctx.payloadLen` with a `sizeof` or subtracts from it, copies of a text
tail into a local buffer excepted) and 32-bit sensor masks (no 16-bit `*Mask` in the sensor and forwarding code,
`receivedPeerMask` excepted; `bondForwardSensors` is 32 bits). Fixed-header frames (RELAY_DATA, PEER_LIST,
SENSOR_ENVELOPE / BOND_SENSOR_DATA, CMD_RESP, FILE_DATA, FS_LIST_RESP and the text-only frames), whose payload ends
in data or fixed-stride entries, never grow: the rules block and `docs/COMPATIBILITY.md` name them, each of their
structs is marked "Fixed header: cannot grow", and every `ctx.tail()` read starts after one of them. The limits that
follow from the 16-byte group tag (the signed-frame fit on send, the flood size check, the relay wrap, the
PEER_LIST receive limit) compare against the named constants, never a written-out number.
Reserved bytes are sent as 0: no sender fills a wire struct field by field without declaring it `= {}`, and the
shared builders (`v4FillHeartbeat`, `buildMetadataPayload`, `buildCapabilitySummary`, `buildLocalBondStatus`)
clear the struct first. It takes `--cxx` and `--component <directory>`; the base tree at 4c8f7627 fails it.

`espnow_mesh_id_kat` (`test_espnow_mesh_id_kat.py`) checks the 1.0 mesh id against known answers computed in
Python (libsodium's BLAKE2b KDF, subkey 3, context `esp-mid1`, the first nonzero 16-bit pair) and runs
`meshid::pick` on host-built subkeys. Source checks pin `meshKeysIdFor`, `meshKeysDerive` and
`meshKeysRefreshIds` (every keyed mesh, enabled or not, takes its key's id; `espnow_mesh_slots` runs a double of
it), and that every send path that stamps a header's mesh id refuses id 0.

`device_name_core_tests` runs the device-name rule (`System_DeviceName.h`): 1-31 letters, digits and `-`, no
leading or trailing `-`, not a command word or a MAC address, the 32-byte wire field with its NUL and zero fill,
`getField` refusing an invalid name, the case-insensitive clash check and the 29-byte advertised name.
`device_name_sources` (`test_device_name_sources.py`) pins what cannot run on the host: no Bluetooth-only name or
`espnowsetname` is left; the boot hands the hostname to Wi-Fi after the settings load, and `applyHostname` also
renames a running station; a new name is handed on in one place (`deviceNameApply`: ESP-NOW, the hostname, the
"same name" warning), by `devicename` on set and clear and by the setup wizards' save after it applies the
settings, and no other file does it itself; Bluetooth's server and client roles use the device name as their GAP
name and a cut name is advertised as shortened; every frame field that carries the name is written with
`putField` and read with `getField`; `espnow pair` and `espnowpairsecure` return the peer-name refusal. The
unnamed fallback and the unknown-peer label use the last two address bytes (`HardwareOne-B2C3`, `peer-B2C3`);
"(default)" is written in one place and never reaches the hostname, the GAP or advertised name or a frame
field; the screens and text replies that show the name use the one wording, and the OLED setup wizard row,
whose value column holds twelve glyphs, shows `(default)` alone; `deviceNameSet` sits beside the name in
`bleinfo json`, `status json` (top level and `connectivity.espnow`) and `espnowdeviceinfo json`.

## MQTT state document and Home Assistant discovery

`mqtt_payload_core_tests` (`test_mqtt_payload_core.cpp`) runs `System_MQTTPayloadCore.h` with the vendored
ArduinoJson: an SSID, friendly name or room with quotes, backslashes and control bytes parses back unchanged and
invalid UTF-8 becomes `?`; every Home Assistant template, and every key it guards on, resolves in a state
document built from the sensor builders' text (ToF nearest target, IMU `accel`/`gyro`, GPS `sats` and the `fix`
guard, presence ON/OFF, RTC `temp`); GPS speed is `kn` with the speed class, gyro `rad/s`; the local device
carries the board as model and `sw_version`, a peer neither, and an unnamed peer the unknown-peer label; `time`
and `rtc.datetime` end in `Z` and are absent for an unset clock or an invalid or pre-2020 RTC; a section that is
not one JSON object (`nan`, cut) is dropped alone; documents that do not fit are refused, never cut; the object
ids and retirements are pinned. `mqtt_payload_sources` (`test_mqtt_payload_sources.py`) checks that each
fixture has only keys its production builder writes and that System_MQTT.cpp builds both documents through the
core (no "ESP32-S3", the thermal summary builder, no hand-written JSON, `MqttClientUse` first).

## ESP-NOW answers (rule F3)

`espnow_answer_core_tests` (`test_espnow_answer_core.cpp`) runs `System_ESPNow_AnswerCore.h` as it is: the 5-byte
answer header `{u32 reqId little endian, u8 status}` and its round trip, a payload under five bytes that does not
parse, 0 as the only ok, the pinned shared numbers (1 busy ... 11 cancelled), the one name table (a shared code has
the same name in every answer, 12-15 and an unknown 16+ code have none, 16 is `failed` in CMD_RESP, `not_a_dir` in
the FS answers and `incomplete` in FILE_CANCEL) and the eight 1.0 answer opcodes.

`espnow_answers` (`test_espnow_answers.py`) compiles, unchanged from `System_ESPNow.cpp`, the CMD_RESP senders
(`v4_send_command_response`, `v4_send_cmd_resp_async`), the STREAM line sender (`v4_send_stream_line`) and the
receivers `v4h_cmd_resp` and `v4h_stream` against the real `System_ESPNow_Wire.h` and `System_ESPNow_RxCtx.h`. A
recorded frame is handed to its receiver under its own header msgId. It checks that a refusal and a result carry
the CMD's number as reqId and a status (busy; failed, named; ok) under a msgId of the answerer's own; that the
receiver files the answer by the payload's reqId whatever the header msgId; that each STREAM line has its own msgId
and is filed under its command (0 for `startstream`); that a line is cut to one frame for its path, header
included; and that a header-only STREAM files nothing. Source rules: no CMD_RESP or STREAM send in
`System_ESPNow.cpp` uses anything but `generateMessageId()` as its msgId, and the duplicate check no longer skips
STREAM, BOND_STREAM_CTRL or CMD_RESP. The same suite compiles the command path unchanged: `v4h_cmd` (a second
sealed CMD while one waits is answered `busy`; the waiting one again and an unsealed one get no answer),
`v4_handle_cmd` from its authentication decision on (`no_slot` with no stream session, `busy` when the queue does
not take the command, `too_large` for one over the input limit) and `v4CmdResultCallback`, the answer to every
command that ran (ok and `failed`, with and without text, and the out-of-memory fallback), each as `{reqId = the
CMD's msgId, status}` under a fresh msgId and read back by name; the console line names the status from the one
table ("Command FAILED (busy)"). It also runs `v4h_topo_start` and `v4h_topo_peer` with a one-slot stream table:
a TOPO_START or TOPO_PEER whose status is not ok (busy, or a code a later firmware adds) records nothing, so a
busy answer with 0 peers is not taken for an edge device; and it runs the web bond page's `fsStatusTag`
(`WebPage_Bond.cpp`), whose JSON error words must be the shared table's for every status (`busy`, `refused`, not
a second set of names). With F3 also: `espnow_rx_frontend` delivers a re-sealed STREAM and CMD_RESP
retry once and two lines of one command both; `espnow_file_transfer` runs the production FILE_CANCEL sender (reqId
= the transfer, fresh msgId, the sender stops the named transfer once) and FILE_END's status 0 = complete;
`espnow_user_credentials` checks that each refusal of a remote command is `{reqId, refused}` under a fresh msgId;
`espnow_tx_multiframe` checks each STREAM line's header and msgId; `espnow_wire_layout` pins the answer layouts
and the status numbers of the FS answers and FILE_CANCEL.

## ESP-NOW 1.0 forward compatibility (back-capture W1, W2, W4, W5)

`espnow_rx_frontend` runs the relay hop rule (W1): a hop passes on any inner opcode except the exact list of 1.0
link-local opcodes (unknown and later opcodes cross, and no range is refused), and still checks the inner frame's
version, magic and header length, `meshRelay`, ttl, the route and split horizon; the final destination refuses
what it does not take routed and counts `routedDropPolicy`. `espnow_power_event_core_tests` (testForwardCopy) runs
W2's copy rule: a bond master forwards the partner's battery and event frames at their full received length with
only the link byte (and the event's forwarded bit) patched; a path too short for a grown frame gets its first
1.0-length bytes; and a frame longer than one sealed frame (202 bytes) goes on at its 1.0 length, never cut in the
middle of a later field. `espnow_bond_forward` (testVerbatimForward) and `espnow_power_events` check the same end
to end, with readings that keep flag bits this firmware does not know. `espnow_forward_core_tests` (testTarget,
testAnnounceGen) runs W4: byte 7 `announceGen` packs and parses, steps 1..255 (never 0) on every announcement
(the back-off re-send, the 10-minute refresh and the retry after a refusal); an answer counts only when it echoes
the latest number, so an older acceptance, refusal or stale answer changes nothing and an answer before any
announcement is ignored; an acceptance may carry a subset of the announced streams and sensors, never more, and
what it carries is what is forwarded. `espnow_forward_rx` and `espnow_bond_forward` check that the master echoes
the number and the bond master sends it. W5: `espnow_wire_layout` (check_earmarks) pins the opcode map's earmarks
(82-85, 156-158, 86 and 159 free), the stream-id rows and the COMPATIBILITY text, and
`espnow_session_caps_core_tests` pins caps bits 8 `backlog_serve` and 9 `files_serve` as named and never sent by a
1.0 build.

## ESP-NOW dispatch policy

`espnow_dispatch_policy` extracts the production V4 opcode table, its flag constants, the lookup, the dispatcher (`v4_dispatch_table_try`), `espnowOriginIsKnown`, `v4h_text` and the espnow_task text-queue drain from `System_ESPNow.cpp`. It also extracts `LogMessageType` and `TextQueueEntry` from `System_ESPNow.h`, the opcode/flag enums and header from `System_ESPNow_Wire.h`, and `V4RxCtx`. It generates one recording double per other handler named in the table and compiles the harness with `ENABLE_BONDED_MODE` 1 and 0. It checks the following. Opcodes 130 (SUBSCRIBE_UPDATE) and 31 (BOOT, step 8), retired in ESP-NOW 1.0, have no row. TIME_SYNC, SENSOR_STATUS, SENSOR_REQ and SENSOR_ENVELOPE need one too, on a keyed and a keyless device, with no bond partner required; a signed broadcast is not enough for them. Once the device has a mesh passphrase, TEXT needs an authenticated frame, and a plaintext one is dropped with one warning; a keyless device still accepts plaintext. Plaintext TEXT accepted on a keyless device is filed under its origin MAC, never a paired peer's name, and posts no `SYSEVT_TEXT_RX`. Authenticated text (BROADCAST_AUTH or session-encrypted, direct or relayed) keeps its registry name, encryption flag, per-fragment records and single bus event. The retired opcode 150 has no row. USER_SYNC requires a session-sealed frame (F9) from a paired sender (1.0 step 9): a plaintext or group-signed one from a paired peer and a sealed one from an unpaired sender reach no handler, a sealed one from a paired peer does. TOPO_REQ/START/PEER (step 9) need a paired, sealed frame: a group-signed one reaches no handler, a sealed one does. Every zero-flag row is on an explicit allow-list (the six handshakes; TOPO_* left it in step 9); this is a subset check, so later items may only shrink the list (`V4_OPC_LEN_EXACT` is not a policy flag). The length column: every row has a minimum length equal to the 1.0 value in the driver's `EXPECTED_MINLEN` (these never rise), `V4_OPC_LEN_EXACT` is on exactly the six handshake rows, and for every row, with all its policy gates passed, one byte short is dropped before the handler and counted once in `gEspNowRxLenDrops`, the minimum passes, and a longer frame reaches the handler with its length intact (a handshake frame one byte long is dropped). An empty STREAM is dropped and counted (1.0 removed the empty STREAM_BEGIN / STREAM_END markers); a 24-byte SUBSCRIBE_UPDATE with reserved bytes set reaches the subscription store through the real handler, and a 20-byte SESSION_UNKNOWN reaches its rule. `--component <component-directory>` or `--source-dir <overlay>` replays the pre-fix sources. It uses ASan/UBSan when `HW1_SANITIZE` is enabled and mocks history, the event bus and the radio.

## ESP-NOW fragment reassembly

`espnow_reassembly` runs the production V4 fragment reassembler from `System_ESPNow.cpp` (entry allocation, GC, `v4_reasm_accept`, `v4_reasm_release` and `v4_dispatch_post_cleanup`) on crafted plaintext fragments, with the table allocated exactly as init does. It checks in-order and out-of-order completion with learned strides (direct 200 B and routed 130 B), duplicates and GC; that a fragment whose type, count or index disagrees with its entry is dropped; that a fragCount above `V4_FRAG_MAX` gets no entry; that a completion larger than the entry's buffer is refused; and, in separate invocations, the late-stride over-size case and cleanup when the table was never allocated. Source contracts check that `v4_try_handle_incoming` reaches the table only through these helpers. `--component <component-directory>` or `--source-dir <overlay>` replays the former over-read and null dereference. It uses ASan/UBSan when `HW1_SANITIZE=ON` and does not exercise sessions, ACKs or the radio.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^espnow_reassembly$' --output-on-failure
```

(Alternative placement: as a paragraph after `espnow_disabled_interface` in "Network build selection and disabled ESP-NOW", adding `espnow_reassembly` to that section's `ctest -R` line.)

## ESP-NOW message numbers

```sh
python3 components/hardwareone/test/host/test_espnow_msgid_dedup.py --sanitize
```

`espnow_msgid_dedup` compiles `System_ESPNow_MsgIdCore.h`, the production `generateMessageId()` and the
production receive duplicate ring (since step 5b the accept memory: `v4_accept_find`, `v4_accept_lookup`,
`v4_accept_record` with the entry type and TTL; cases A1-A5 are in the step 5b section) over a fake clock
(ESP-NOW 1.0, E21). The seed is never 0; the counter goes 0xFFFFFFFF -> 1 and never hands out 0; 300 frames from a
random start across the wrap are all new to the receiver and each re-delivery is caught; fragments of one msgId
stay distinct; a sender restarting within the 5 s TTL from a new random start loses no frame (a control run with
the old fixed start of 1 loses its first 20); the TTL expiry is unchanged. Source checks: the state's counter is
seeded with `espnowmsgid::seed(esp_random())` and `generateMessageId()` goes through `espnowmsgid::take`.

## ESP-NOW session replay window

```sh
python3 components/hardwareone/test/host/test_espnow_session_replay.py --sanitize
```

`espnow_session_replay` drives the production session wrap, unwrap, rekey-apply, REKEYING/abort and prev-key
sweep through two rekeys with a receiver record and a scripted peer on the host. Frames recorded under the
previous key must be rejected by that key's own replay window after the rekey, a genuinely late old-key frame is
accepted once without touching the new key's window, and new-key traffic from sequence 1 is accepted. The AEAD,
clock, TX guard, drain and logging are harness doubles; `--source-dir <component-directory>` replays the
regression against a pre-fix tree. No radio is used.

## ESP-NOW session rekey retry

`espnow_rekey_retry` runs the production SESSION_REKEY initiator, responder and retry sweep from
`System_ESPNow_Handlers_Crypto.cpp` (`sendRekey`, `runDeferredRekey`, `espnowRekeyInitiate`, the in-flight table and
`rekeyRetrySweep`) together with the session lifecycle functions from `System_ESPNow_Sessions.cpp`, against modelled
peers with deterministic stand-in crypto; the radio, clock, logging, lock and allocation boundaries are host doubles.
It checks that an unanswered initiator REKEY is re-sent at 2 s and 4 s as the identical frame with its original msgId,
that a session still REKEYING at 6 s is dropped so the next send re-handshakes, that answered, aborted, replaced and
responder-side rekeys are never re-sent, that sixteen rekeys outstanding at once are all covered, that a refused send
leaves the session ACTIVE while the `kRekeyMinIntervalMs` re-init guard still spaces the next attempt, and that no
frame is sent under `EspNowTxGuard`. It also runs the espnow tick's REKEY auto-trigger (step 1e of
`processMeshHeartbeats` in `System_ESPNow.cpp`, plus a source contract on it) every 10 ms: a session that
`espnowRekeyInitiate` would refuse gets no log line per tick. `--source-dir <component-directory>` replays the former stranded-REKEYING
behaviour. It does not qualify libsodium primitives or radio timing.

## ESP-NOW revocation

ESP-NOW 1.0 step 3 (design section 5.1 A + B). `espnow_revoked_core_tests` compiles
`System_ESPNow_RevokedCore.h` as it is, with the vendored ArduinoJson: the revoked list by MAC and by key under
any MAC ("no key" never matches an all-zero key), a full list of 32, unrevoke by name, MAC or keyId prefix;
`revoked.json` written and read back the same (schema 1, lowercase hex), and nine kinds of unreadable file
(truncated, not JSON, another schema, no list, a bad MAC or key, more than 32 entries) that lock the list (fail
closed) instead of reading as empty; rule A (`espnowrevoke::admit`: an identity only for a registry entry, a
first key only inside its first-key window, no answer to a revoked MAC or key, nothing new while the list is
locked); the boot identity check allowed after an Ok load or with no registry file, and skipped for a
quarantined, partial or unread registry; the 10-minute first-key window across the millis() wrap and across a
restart; a key revoked under a second MAC is a second entry (the first MAC stays listed); only a unicast MAC is
revoked or unpaired by address; `espnowunrevoke --all` reads a never-read list again before it moves anything aside.

The handlers that use it are run by their own suites (review fixes of 2026-11-23 included: keyex_hello K20, a
forged CONFIRM refusal from a peer with no stored key leaves the HELLO retries armed; bond_partner_change, an unpair
whose registry save fails keeps the identity and folder; peer_files source checks for the start-up revoked tidy, the
revoke teardown retry, the rollback window, the unicast rule and the locked-list pair-request refusal): `espnow_keyex_hello` (K14-K19: a HELLO from a non-registry
MAC stores nothing, is answered KEY_EX_CONFIRM status 3 and leaves no radio peer; a revoked key is refused under any
MAC with no answer and the waiting pairsecure entry is rolled back; the first-key window; a REPLY under the same
rules; a locked list), `espnow_rx_frontend` (testRevokedDrops: gate 1 drops a frame from a revoked radio source
before ACK, unwrap and dispatch; gate 2 drops a frame a member relays from a revoked origin before the flood relay;
no routed delivery from, and no envelope to, a revoked device; no health or metadata slot for a revoked MAC; source
checks that SESSION_UNKNOWN goes to registry peers only, the router takes no advert from and no route to a revoked
device, and the heartbeat skips one), `espnow_session_rekey` (C29: the three session handlers drop a revoked
sender, or one with no stored identity, on espnow_task before anything is queued to cmd_exec), `espnow_peer_files`
(the unpair teardown's write order and stores, `espnowrevoke` writing the list first) and
`espnow_command_privileges` (`espnowrevoke` and `espnowunrevoke` super admin, `espnowrevoked` admin, `espnowforget`
gone).

## ESP-NOW 1.0 step 4: handshake crypto and sessions bound to the mesh key

```sh
python3 components/hardwareone/test/host/test_espnow_handshake_kat.py --sanitize
python3 components/hardwareone/test/host/espnow_handshake_kat_vectors.py   # prints the vectors
python3 components/hardwareone/test/host/test_espnow_keyex_hello.py --sanitize
python3 components/hardwareone/test/host/test_espnow_session_rekey.py --sanitize
python3 components/hardwareone/test/host/test_espnow_rekey_retry.py --sanitize
python3 components/hardwareone/test/host/test_espnow_mesh_slots.py
```

`espnow_handshake_kat` (`test_espnow_handshake_kat.py`) compiles `System_ESPNow_HandshakeCore.h` (every byte
the handshake tags or signs) and compares its output with an independent assembly of the written layout in
`espnow_handshake_kat_vectors.py` (Python hashlib/hmac, nothing taken from the firmware), then pins that
assembly's digests: the mesh salt prefix `hw1-espnow-1-mesh-salt:` (passphrase -> stretched key -> mesh id ->
bootstrap and group key), the KEY_EX_HELLO / REPLY / CONFIRM tags (labels, nonces, the REPLY bound to its HELLO,
a refusal with zero reply nonce and responder key), the SESSION_OPEN / CONFIRM / REKEY transcripts (92, 100, 84
bytes, `openEpochMs`), the C1 mesh tags, the C2 session keys and bond token, and the group tag label. Source
checks tie the handlers, `sessionDeriveKeyMaterial`, the rekey derivation, `v4_group_tag`, the stretch and the
settings `kdf` record to those builders. Run the vectors script to print the vectors after a deliberate change.

`espnow_keyex_hello` (K7, K12, K26 after the step 4 review: a same-key exchange on another mesh does not move the
stored slot unless a local pairing action is open and the lower address started it; a new sender takes an unused
HELLO-table slot, else the oldest; K2, K20-K25): a replayed HELLO (a nonce already answered) gets no REPLY; a forged CONFIRM
refusal leaves the HELLO retries armed; an authenticated status 3 ends the key exchange in flight and keeps the
back-off, and is ignored with none in flight or over another HELLO's nonce; the status 3 this device sends is 34
bytes and tagged over the HELLO it refuses; a REPLY with no exchange in flight, or a recorded one over an older
nonce, stores nothing; the initiator's CONFIRM is checked against the REPLY sent (a copy does nothing); after an
accepted pair request only the key it carried is stored (HELLO and REPLY), until the key binds or the first-key
window ends (the binding from `System_ESPNow.cpp` is compiled; its wiring into `espnowKeyExAdmit`,
`espnowaccept`, the pair request and the unpair is pinned). `espnow_session_rekey` (C22-C24, C28, C30-C33): OPEN
154 and CONFIRM 162 bytes; a changed frame fails the mesh tag first and, re-tagged, the signature; golden 92- and
100-byte transcripts; **C1 by host only**: a device with the same mesh id and another bootstrap key gets "mesh key
mismatch" both ways, and with the same key the session opens without re-pairing; another bootstrap key gives other
session keys and bond token; no OPEN on a mesh without a key; `openEpochMs` is the clock (0 unset) and is signed.
`espnow_rekey_retry` (c9, c10): a REKEY tagged under another bootstrap key gets no reply and changes no key; a
session keyed under an older mesh key is not rekeyed and a peer REKEY of it closes it. `espnow_mesh_slots` (L1, L5,
P1, I6, I7, S1b): a passphrase change closes the mesh's sessions and says who reconnects; clearing a passphrase
disables the mesh and drops its id on every slot (the default mesh only after another is the default); a cached
key without `kdf` record 1 is not used. `espnow_wire_layout` and `espnow_dispatch_policy` pin the new sizes and
minimum lengths (KEY_EX_CONFIRM 34, SESSION_OPEN 154, SESSION_CONFIRM 162 = the relay budget, SESSION_REKEY 146).

## ESP-NOW 1.0 step 9: remaining wire and names

DESIGN 5.3 / 5.4 and the step 9 row. Rules for the CMD / USER_SYNC credential block (E29):
`System_ESPNow_CredentialCore.h`.

`espnow_credential_core_tests` compiles that header as is: C1 a password with `:` (and a `:` in the user name and
the command) comes back whole, where the pre-1.0 colon split took the wrong account and command; C2 an empty
password (and an empty user name) parses, so the login check refuses it like any wrong password, and the shortest
CMD is the row minimum (5); C3 a bond token is 16 raw bytes (a NUL and a `:` inside), a 15-byte token or a token
block with a user name is refused; M1-M10 short headers, lengths past the payload, no command, a lone or inner
NUL, an unknown kind, the reserved byte ignored, the old colon text refused; L1-L7 a 255-byte field packs, 256
does not, no room is refused (never cut), an empty command and a NUL in a field are refused, a 32-byte name does
not fit 32 bytes; U1-U6 USER_SYNC's two blocks and flags byte (with `:` in both passwords), no flags byte, bytes
after the flags ignored, a bond token refused, the shortest USER_SYNC is the row minimum (9), the old JSON refused.

Updated on purpose (listed before editing in the step notes): `espnow_user_credentials` (the remote commands go
through `v4_handle_cmd` as credential blocks; a new scenario: a `:` password logs in and runs the whole command,
the old split of the same text is a wrong password, an empty password is checked and refused, a `:` in the command
stays in it, a bond token with a `:` byte runs under the bond identity; USER_SYNC is two blocks + flags);
`espnow_answers` (v4h_cmd stores the block with its length and a NUL after it; the old colon text and a block
with no command are refused bad_request on the ACK and never reach the slot); `espnow_rx_frontend` (TOPO_REQ:
plaintext dropped at the gate, group-signed refused by the row, sealed delivered; TOPO_* are sent with
`v4_send_payload_smart`); `espnow_dispatch_policy` (above); `espnow_wire_layout` (BondRequest {reqId u32 0, role 4,
flags 5, reserved 6} and BondNotice {reqId 0, status 4, role 5, flags 6, reserved 7} goldens; no payload pointer
cast left anywhere, the FS list entries are copied); `espnow_bond_agree_core_tests` (request numbers are 32 bit:
wrap cases at 0xFFFFFFFF, no false wrap past 0xFFFF; the notice status mapping 0/16-19 both ways, an unknown
status ignored); `espnow_bond_agreement` / `espnow_bond_partner_change` (frames read at the new offsets);
`espnow_forward_core_tests` (result 0 = ok, 5 full, 6 bad_partner, 16-19, the old numbers read as a refusal this
firmware does not know); `espnow_forward_rx` / `espnow_bond_forward` (a withdrawal answers 0);
`espnow_fslist_locking` (the list reply's entries are copied to a PSRAM array); `espnow_identity_walk` /
`espnow_identity_writes` / `espnow_keyex_hello` (the peer file is `"schema":1` with `pairedEpochMs` /
`lastSeenEpochMs`, the own identity `createdEpochMs`, the hourly refresh in milliseconds);
`espnow_group_membership` / `espnow_metadata_push` / `espnow_mesh_slots` (`addEspNowDevice` without the
dead `encrypted` / `key`; a room command's frame is a credential block); `remote_command_authority` (the bond
payload is built into a byte buffer); `credential_attempts`, `espnow_tx_multiframe` (the `v4_handle_cmd` slice).

Review fixes: `espnow_credential_core_tests` W1-W2 (`WipeOnExit` clears its buffers at scope exit);
`espnow_user_credentials` pins the wipes (doUserSyncWork's four field copies at every return, the deferred work's
payload before it is freed, the deferred CMD slot after `v4_handle_cmd`) and runs a bond token sent by a device
that is not the bond partner: refused before the token is compared.

## ESP-NOW two-way liveness (1.0 step 8, RC3)

DESIGN section 4: a peer is online only when it was heard within 30 s, proved it hears this device (two-way)
and a session is up; heard only is connecting. Rules: `System_ESPNow_LinkCore.h` (with `MeshLink` and the state
mapping in `System_ESPNow_PeerStateCore.h`).

`espnow_link_core_tests` compiles both production headers: L1-L4 the link definition (none after 30 s silence,
one-way when heard, two-way after two distinct evidences in 30 s and kept with one, the same id counted once,
id 0 never, three slots with the oldest given up, every window across the millis() wrap); L5 the heartbeat echo
memory (last two heartbeats sent within 30 s, the echo and RSSI for the next heartbeat, 0 after 30 s); L6 the
record of direct ACK_REQ sends (fragments one entry, a ring of 8, msgId 0 never); L7 the back-off (0 below 30 s,
10 s after, also after an hour); L8 the event rate limit (no offline before an online, one per 30 s, a change
undone inside the window posts nothing, the state at the window's end is posted); L9 the state from the link;
L10-L12 a two-board simulation at 1 s steps with a 5 s heartbeat tick: both ways online within 16 s with one
online event each; **the one-way case** (B cannot hear A) for 10 minutes: A shows B connecting (one-way) and B
shows A offline the whole time, no link change and no event, A hears B at least every 10 s, B backs off to 10 s,
one lost heartbeat of B's stream keeps A's view; a link flapping every 5 s for 10 minutes posts events at least
30 s apart; L13 a silent peer's link memory is cleared (no stale stamp after the millis() wrap) until it is
heard again. Built against the pre-1.0 back-off (30 s after 5 minutes) L11 fails 7 checks.

`espnow_channel_follow_tests` F30 is channel-follow bench check B8: a master that never hears its worker sends
its heartbeats with the production back-off; with the 10 s ceiling the worker starts no sweep in 10 minutes,
with the pre-1.0 back-off it hunted (both checked, also across the wrap).

`espnow_rx_frontend` (extended) runs the real `v4h_heartbeat`, `meshLinkOnHeartbeat` and `v4_rx_ack` with the
production link and restart headers: a sealed heartbeat echoing one of our heartbeats is two-way evidence and a
second distinct one makes the link two-way; a tagged echo, an echo of an id never sent, echo 0 and an echo of a
heartbeat sent 30 s ago are not; a sealed direct ACK for one of our direct sends is evidence (busy too), one for a
frame not sent direct, a tagged ACK and a relayed sealed ACK are not (the relayed one does not refresh the heard
clock either); the boot counter: the first sealed counter is recorded without a restart record, a replayed
old-boot or same-boot tagged heartbeat closes nothing and files nothing, one naming the peer from another radio
asks nothing, a newer tagged one closes the stale session once and files nothing, forged ever-newer tagged
counters file nothing, a sealed counter past the last sealed one files once and closes nothing, the first
counters after this device's start file nothing; while a session is up a tagged heartbeat between two sealed
ones changes neither the echo nor the RSSI (review fix 1). Source checks: the periodic heartbeat asks no ACK and
is sealed when the session is up, the fan-out is spread over passes (4 per 50 ms) and leaves TX queue room, a
heartbeat msgId is remembered only once sent, the broadcast
ACK tracking and `espnowbroadcaststats` are gone, the router takes adverts only from a two-way neighbour and
`neighbourAlive` and `v4_radio_emit` go direct only on a two-way link.

`espnow_peer_state_surfaces` (extended): a one-way peer with a session is connecting and not counted online
(MESH metric, Home Assistant availability); a session being rekeyed counts as up (review fix 2).
`espnow_meshstatus_fit` (new) runs the production `cmd_espnow_meshstatus` with its row builders and ArduinoJson:
the worst case (32 peer rows, 16 discovered devices, 31-character names, largest numbers) is a reply under
`CMD_RESULT_MAX` that parses, with `peersNotShown` / `unpairedNotShown` adding up to the totals (paired peers
first); a small mesh is shown whole; rows carry `state`, `link`, `lastTwoWayAgeMs` and none of the removed keys. `oled_espnow_remote_actions`, `g2_espnow_peer_rows` and
`espnow_group_membership` read the link through `meshPeerLinkOf` doubles; `espnow_wire_layout` pins HEARTBEAT at
56 bytes and opcode 31 retired; `espnow_dispatch_policy` the HEARTBEAT minimum 56 and no BOOT row;
`espnow_handshake_backoff` that hearing a peer again (not the online event) ends its back-off; `espnow_page_render`
a one-way mesh row showing Connecting with its link; `command_name_form` the retired name `espnowbroadcaststats`.

```sh
ctest --test-dir <build> -R 'espnow_(link_core_tests|channel_follow_tests|rx_frontend|peer_restart_tests|peer_state_surfaces|meshstatus_fit|system_notice|wire_layout|dispatch_policy)' --output-on-failure
```

Not covered (device only): the sealed heartbeat through the TX dispatcher, the per-peer fan-out and back-off
glue (`meshSendHeartbeats`), the event tick in `processMeshHeartbeats` (its rules are tested), the meshstatus JSON
fields and three or more boards (relay paths).

## ESP-NOW files: the Q26 transfer layout (1.0 step 7)

```sh
python3 components/hardwareone/test/host/test_espnow_file_transfer.py --sanitize
python3 components/hardwareone/test/host/test_espnow_wire_layout.py
python3 components/hardwareone/test/host/test_espnow_dispatch_policy.py
python3 components/hardwareone/test/host/test_espnow_chunked_send.py --sanitize
./build-host/espnow_answer_core_tests
```

`espnow_file_transfer` was rewritten for the transferId layout (25 cases). Every frame a test plays has its own
msgId and names its transfer in the payload; FILE_START and FILE_END go through a double of the acknowledged send
(the receive handler's verdict is the ACK), and the receiver's FILE_RESULT and FILE_CANCEL loop back into the
sender's handlers. Sender results: Stored (RAM path, streaming path, a routed peer with 138-byte chunks; every frame
its own msgId, one nonzero transferId, ACK_REQ on START and END only); Failed (the receiver's rename fails:
io_error; a chunk changed on the way: crc_mismatch; a streaming gap: the receiver's FILE_CANCEL stops the pump within
one chunk, incomplete, no FILE_END; no answer to FILE_START: timeout at once, and the sender's FILE_CANCEL frees the
receiver's slot; a 97-byte name: too_large from the receiver's ACK, the name sent whole and no chunk; a name that
cannot fit FILE_START: refused before any frame; four slots taken: no_slot with one frame, no retry and no wait);
Unknown (no FILE_RESULT: one FILE_END copy at 4 s under a new msgId, Unknown at 10 s; a result after that is shown
once as late); a lost first FILE_RESULT is brought again by the copy (Stored); a lost FILE_END ACK still ends
Stored. Receive side: every FILE_START refusal is its ACK's status with no slot, buffer, FILE_CANCEL or FILE_RESULT
(name over 96, over the 63-byte inbox limit, '/', '..', a NUL, kind 2, a bond answer without bond mode, transferId 0,
a chunk over 194 bytes, over 65,535 chunks, over 4 MB, a name cut short, below the 13-byte minimum); incomplete,
crc_mismatch, a wrong chunk count and io_error come in FILE_RESULT after a taken FILE_END; a FILE_END copy after the
slot is released gets the remembered FILE_RESULT (stored or failed), another device and a stranger transfer get
kRefused, a stop for an unknown transfer is taken, and after 60 s the memory has forgotten it; a FILE_END that comes
after the 30 s sweep gets FILE_RESULT timeout. The storage-admission, writer, teardown, busy-table, contention and
small-file-after-ACK cases from before are kept on the new layout (an admission refusal is the START's io_error).
The callers' words: `espnowsendfile` "Stored on peerB: report.bin (1000 bytes)", "Unknown: ...", "Error: '<name>'
was not stored on peerB: <reason>."; `imagesend` "Stored <path> on peerB (<n> bytes)"; the OLED "File stored",
"Not stored: <reason>", "Sent, no answer. Check peer.". `espnow_wire_layout` pins the new FILE_START (12 + name),
FILE_DATA (8 + 194), FILE_END (16) and FILE_RESULT (12) layouts, FILE_START as a fixed-header frame;
`espnow_dispatch_policy` the minimum lengths 13 / 9 / 16 / 12 / 5 and the FILE_RESULT row; `espnow_chunked_send` K13:
a FILE_START refused no_slot is not sent again; `espnow_answer_core` FILE_RESULT as the eleventh answer opcode.
`espnow_fs_transcript_privacy` takes the FS_GET server's `sendFileToMacResult`; `log_overflow_latch` pins the
storage refusal as the START's io_error. Review fixes (case `chunk-length-late`): a chunk that is not exactly
chunkSize (the last one: the rest of totalSize) ends the transfer with FILE_CANCEL(bad_request) and writes nothing
(a 3-byte chunk size with 194-byte chunks on a streamed file); a late chunk of a transfer that has its verdict draws
no cancel; cancels for unknown transfers are one per peer per 100 ms; a "rejected" cancel during the result wait is
not the verdict; names with CR/LF, an escape or DEL are bad_request; a bond answer over 128 KB is refused (source
pin); `espnow_chunked_send` K13 also resends a FILE_START cut by this device's rekey; `espnow_fs_transcript_privacy`
refuses an FS_GET of a name over 63 bytes with too_large before the OK.

## ESP-NOW remote command results and capacity (1.0 step 6)

```sh
./build-host/espnow_remote_cmd_core_tests
python3 components/hardwareone/test/host/test_espnow_answers.py --sanitize
python3 components/hardwareone/test/host/test_espnow_send_words.py --sanitize
python3 components/hardwareone/test/host/test_espnow_chunked_send.py --sanitize
python3 components/hardwareone/test/host/test_espnow_rx_frontend.py --sanitize
python3 components/hardwareone/test/host/test_espnow_reassembly.py --sanitize
python3 components/hardwareone/test/host/test_espnow_message_store.py
```

`espnow_remote_cmd_core_tests` (new) compiles `System_ESPNow_RemoteCmdCore.h` as is: the requester table (16
entries of 16 bytes) and the CMD_RESP result queue (16 entries, texts in one 24 KB ring). Fan-out to 16 responders:
16 requests open at once, each accepted by its own member's ACK (another member's ACK changes nothing), each
answered once (a second answer is a duplicate, an answer to no request unknown); a 17th request pushes out the
open one nearest its deadline (said aloud), and an ended one is reused before any open one. ACK busy and no slot
are Busy, another status Refused with it, a later ACK changes neither. The deadline: "No result" exactly 30 s after
acceptance, reported once, the entry kept 5 min; a result in that time is late; after it the answer is unknown.
STREAM lines move the deadline to 30 s after the last line (a line while Sending proves acceptance; another
device's line does nothing). No ACK in 10 s is No answer (longer for a frame that waits for its session), and a
result after it is late. A whole-message retry renames the request to the new msgId. Deadlines across the 32-bit
millis() wrap; tick reports at most its limit, the rest later, each once. The queue against a model (200 000
random pushes up to 4200 bytes and shows): texts come back whole, a push is refused only with no entry or no
contiguous room, the ring wraps; a held result (lab hook) holds the ones behind it.
`espnow_answers` runs the production `v4h_cmd_resp` over the real queue: a burst of 16 results is taken, the 17th
refused busy with nothing replaced; six 4 KB results fill the 24 KB ring and the seventh is refused busy; after
two are shown the next wraps to the ring's start whole; no store (no PSRAM) answers busy; the lab hold queues a
result with a later ready time. `v4h_cmd` with the slot taken answers by ACK only (no CMD_RESP on the air);
every command result is sent through `v4_send_cmd_result` with an ACK wait (no session: the encrypt-or-wait
sender; a requester still busy after the retry is logged once); each STREAM line reaches the deadline hook.
`espnow_send_words` (new) compiles the tracked-send table of `System_ESPNow_Sessions.cpp`: a TEXT whose receiver's
ring is full ends Busy (state 4, history stamp 4), never Delivered; no slot is Busy; too large is Refused (5) with
its status; another device's ACK changes nothing; no ACK in 10 s is timeout. Source rules: the web snapshot names
busy and refused and gives a refusal its reason, the page and the OLED use Busy / Refused / No answer and mark
"Result (late)", every CMD and USER_SYNC send registers its request first and cancels it on failure, a sealed ACK,
a STREAM line and the loop reach the requester table, `v4h_cmd` sends no CMD_RESP. `espnow_chunked_send` K3 (the
request follows the retry), K11 (a long CMD answered busy is final), K12 (`ackSingle`: one frame, fragment 1 of
1, waits for its ACK; busy sends it once more under a new msgId; no ACK is NoAnswer; without `ackSingle` it is not
waited for). `espnow_rx_frontend`: a sealed ACK reaches the requester table by radio sender with its status and
whether it is the last fragment; the kNoSlot case fills four entries. `espnow_reassembly` case 9 fills a pool of
four. `espnow_message_store` outcome_rows: a received result row keeps its outcome (busy 1, late 5 on every piece)
in the record's state bits; a sent row's busy state (4) is stamped. `espnow_history_core_tests` and
`espnow_history_stress_tests`: the state field has three bits (0-7; reserved bits 0xC0).

Review fixes (step 6): `espnow_remote_cmd_core_tests` retryRename (a held request ignores a busy ACK, follows
the retry to its new msgId, ends once by the sender's final outcome; busy twice is one Busy under the final msgId;
a caller's cancel of the old msgId leaves it; Refused at once, No answer at the next tick, Not sent forgotten),
sixteenEndingsOnePass (16 refusals in one pass all reported); ackOutcomes now reports Busy (with busy / no slot)
and Refused once from tick; cancel drops only a request still Sending. `espnow_chunked_send` K3/K11: held before
the first attempt (not for CMD) and settled once under the final msgId. `espnow_answers`: no store answers
not_ready. `espnow_tx_multiframe`: sendAeadSync reports NotQueued (before init, on the dispatcher), Failed (ran
and failed) and Sent. `espnow_history_core_tests`: byte 7 is a refused sent record's status (1-15), cleared on
any other record, a forged one refused. `espnow_message_store`: a refused stamp keeps its status; espnowmessages
json carries `reason`. `espnow_send_words`: the refusal's status reaches the history; source rules for the
timed-out send, results before the tick, the store emptied at every start, not_ready without a store.

## ESP-NOW ACK after accept (1.0 step 5b)

```sh
python3 components/hardwareone/test/host/test_espnow_rx_frontend.py --sanitize
python3 components/hardwareone/test/host/test_espnow_dispatch_policy.py --sanitize
python3 components/hardwareone/test/host/test_espnow_chunked_send.py --sanitize
python3 components/hardwareone/test/host/test_espnow_session_rekey.py --sanitize
python3 components/hardwareone/test/host/test_espnow_reassembly.py --sanitize
python3 components/hardwareone/test/host/test_espnow_msgid_dedup.py
python3 components/hardwareone/test/host/test_espnow_file_transfer.py
```

Every `V4RxCtx` handler returns a `V4Verdict` (`System_ESPNow_Verdict.h`: Accepted, Refused with a shared status,
Silent); the receive path ACKs only after it. Harnesses that paste `V4RxCtx` in paste `System_ESPNow_Verdict.h`
with it (self-contained; `verdict_src` in the runners). `espnow_dispatch_policy`: the table walk (source) checks
that every handler a `kV4HandlerTable` row names is defined as `V4Verdict name(const V4RxCtx&)` in the handler
files (main file, Handlers_Crypto, Collect, Catchup, ForwardRx, MeshTime, PowerEvents), has no bare `return;` and
ends with a return, and that no `void` handler of that name is left; runVerdictWalk: every non-bond row with its
gates passed reaches its handler and returns its verdict, one byte short is kBadRequest, a gated row refused to an
unpaired or unsealed sender is silent, every opcode without a row is kUnsupported; runBondCases adds that a bond
frame from a paired peer that is not the partner is kRefused over its session and silent otherwise; TEXT into a
full ring is refused busy and leaves the ring unchanged. `espnow_answers`: a CMD into the free slot is accepted,
one while the slot is taken is refused busy (its CMD_RESP(busy) went in step 6: the ACK is the one answer).
`espnow_rx_frontend` now runs the real reassembler (constants, entry, helpers, `v4_reasm_accept`,
`v4_reasm_step`) and the accept memory, with stub handlers whose verdict a case can set: testBusyThenRetry (a CMD
refused busy is ACKed with status 1; its re-sealed retry is processed again and ACKed 0; a copy of the accepted
retry is ACKed with flags.duplicate and not processed; a silent verdict gets no ACK and its copy neither),
testAckFollowsHandler (the ACK is queued after the handler returned, in the same receive call, at the end of a
7 ms handler on the fake clock; an opcode without a row is ACKed kUnsupported and a frame under its row minimum
kBadRequest without a handler), testPolicyRefusals (a sealed bond frame from a non-partner is ACKed kRefused; a
group-tagged CMD gets no answer, nor its copy), testFragmentVerdicts (each stored fragment is ACKed for itself,
the handler runs once at completion with the whole message, a fragment sent again after completion is ACKed with
flags.duplicate, not processed and opens no entry; with every entry in use a fragment is ACKed kNoSlot and opens
none, and once an entry frees its retry is stored; a fragCount over the budget is kTooLarge; a fragment the row
refuses takes no entry). `espnow_reassembly` checks the outcomes Duplicate, Pending, Mismatch, TooLarge and NoSlot
(also with no table) and that the dispatcher feeds `v4_reasm_step` between the row gates and the length rule.
`espnow_msgid_dedup` A1-A5: the accept memory remembers Accepted and Silent, a refused frame leaves no entry (its
retry is processed, then remembered), class and fragment are in the key, an expired entry is forgotten and
refreshed in place, the 21 fragments of a completed message are all remembered, the entry stays 20 bytes.
`espnow_chunked_send` (new) runs the production `v4_send_encrypted_chunked` and `v4_send_chunked_attempt`
against a scripted receiver: K1 Delivered; K2 busy on the last fragment sends the whole message once more under a
new msgId at least 300 ms later and is Delivered (the tracked send marked by the caller's msgId); K3 busy twice is
Refused(kBusy) after exactly two attempts, the session hears Refused (alive); K4 kRefused is final; K5 no ACK is
NoAnswer after three tries (the only strike); K6 a rekey this device starts mid-send is waited for and the message
sent again on the new keys; K7 one that does not apply within 1 s is NotSent and counts toward nothing; K8
kNoSlot is retried like busy. `espnow_session_rekey` C35 (real `sessionNoteSendResult`/`sessionNoteSendOutcome`):
a busy NACK between two unanswered sends keeps the session, NotSent counts toward nothing, refusals in a row
never clear it, two unanswered sends in a row do. `espnow_file_transfer` records each handler's verdict (an
accepted FILE_END is its ACK; the handlers send no ACK of their own) and adds ram-finish-after-ack: a small file's
FILE_END is accepted before the file is written, the slot leaves RECEIVING, a copy of the FILE_END is answered as
taken and a late chunk changes nothing, the loop's next pass (`fileRamFinalizeTick`) writes it once; a CRC
mismatch is refused kRefused with its FILE_CANCEL; a FILE_START over the limit is refused kTooLarge.
`espnow_user_credentials`: the deferred USER_SYNC work sends no second ACK, and `v4h_user_sync` refuses an
unsealed or turned-off sync (kRefused) before it allocates or queues (source). `espnow_sensor_wire`,
`espnow_answers`, `espnow_catchup_serve`, `espnow_bond_agreement`, `espnow_bond_partner_change`,
`espnow_metadata_text`, `espnow_keyex_hello`, `espnow_wire_layout` and the handler markers of the other ESP-NOW
suites follow the verdict signature.

Review fixes (step 5b): `espnow_rx_frontend` testFragmentVerdicts adds that a reassembled message its handler
refused (or that falls under its row minimum) answers its resent last fragment with the same status, opens no
entry and does not run the handler (forgotten after the reassembly timeout), and testPolicyRefusals that a sealed
CMD from a session peer not in the registry is kRefused; `espnow_dispatch_policy` runVerdictWalk checks kRefused
for every REQ_PAIRED row from a sealed unpaired peer; `espnow_reassembly` reports a broken entry as Discarded;
`espnow_chunked_send` K9 (no copy leaves the radio: NotSent, no strike) and K10 (a long TEXT is never sent again
as a whole, after busy or a rekey); `espnow_answers`: a STREAM line into a full ring is refused busy;
`espnow_file_transfer`: closing ESP-NOW after a small file's ACK reports it failed (FILE_CANCEL then; FILE_RESULT io_error since step 7) and frees the
slot (`fileRamFinalizeDropAll`).

## ESP-NOW 1.0 step 5a: the ACK frame

```sh
python3 components/hardwareone/test/host/test_espnow_rx_frontend.py --sanitize
python3 components/hardwareone/test/host/test_espnow_session_rekey.py --sanitize
python3 components/hardwareone/test/host/test_espnow_tx_multiframe.py
```

`espnow_rx_frontend` compiles the production ACK path (`v4_queue_ack` into an `espnowtx::sendAck` double,
`V4FragAckWait`, `v4_rx_ack`, `v4_plaintext_type_allowed`). testAckCounts: a sealed frame with ACK_REQ gets one
sealed ACK in its session naming msgId, fragment 0/1 and status 0; two sealed TEXT fragments get two naming 0/2
and 1/2; a group-tagged frame gets a tagged ACK; an ACK is never answered. testNoPlaintextFrameIsAcked: all 256
opcodes sent plaintext with ACK_REQ get no ACK and only the six handshake frames (10-15) pass the plaintext gate
(also inside a relay envelope); a plaintext TOPO_REQ is dropped and a tagged one delivered.
testAckWaiterMatching: a waiter for B's sealed fragment refuses a tagged ACK from B, a sealed ACK from C (also one
that writes B as origin), a tagged ACK naming B from another radio and a plaintext one, and takes B's sealed ACK
for that fragment; an ACK with a status reaches the waiter as a refusal and marks a tracked send failed; sealed
ACKs reach the tracked sends and the app ping, tagged ones the broadcast tracker and the channel follow; ACKs of
the wrong length or with fragments are dropped. The earlier cases that sent plaintext HEARTBEAT and ACK frames
now expect the plaintext gate (and use tagged frames where a frame must arrive). Source checks: no
`v4_send_ack`/`v4_send_frag_ack`, `v4_emit_ack` seals with `sessionWrapAck` or tags under its own msgId,
`v4_send_frame_ex` sends no plaintext but the handshake, `System_ESPNow_Handlers_Crypto.cpp` sets no ACK_REQ,
TOPO_* is tagged. `espnow_session_rekey` C34: a six-fragment send across a rekey the receiver starts completes
(the receiver seals only ACKs while REKEYING; an ACK sealed under the old key opens with the kept previous key);
C2 and `espnow_rekey_retry` c1: the REKEY carries no ACK_REQ. `espnow_session_replay` compiles the seal through
`sessionWrapImpl`. `espnow_tx_multiframe` J: ACKs have their own queue of 16 PSRAM slots; 16 fit, the 17th
is counted as dropped, the dispatcher sends each with its class and session, oldest first, and the slots come
back (also through `stop()`); J2: with the job queue full an ACK is still queued and goes out before every
queued job; J3: an ACK queued while the dispatcher runs a fragmented send goes out inside that send's ACK wait
(`drainAcksOnDispatcher`, a no-op on other tasks; `espnow_rx_frontend` pins the call in the wait loop).
After review: a sealed refusal ACK marks the tracked send failed through `sendStatusMarkRefused`, which also
counts the session as alive (pinned); a relayed tagged ACK feeds no broadcast tracking.
`espnow_channel_core` S5 pins the follow's ACK hook on `ackFor` and tagged ACKs only; `espnow_wire_layout` pins
`V4PayloadAck` (8 bytes); `espnow_file_transfer` and `espnow_user_credentials` counted `v4_queue_sealed_ack`
(removed in step 5b).

## ESP-NOW session handshake freshness

`espnow_session_rekey` drives the production SESSION_OPEN / SESSION_CONFIRM / SESSION_REKEY handlers and
initiators from `System_ESPNow_Handlers_Crypto.cpp`, with the `System_ESPNow_Sessions.cpp` lifecycle, wrap and
unwrap functions, between two modelled devices that exchange the real handshake frames. Clock, logging, lock,
allocation, the radio and the deferred (cmd_exec) queue are host doubles, and the crypto primitives are
deterministic stand-ins. It checks that a re-delivered REKEY is ignored with one WARN and no reply (nonce of the
last REKEY acted on, 30 s applied-at interval, sequence window), that an older REKEY copy ends in one converged
extra round instead of an endless exchange, that a re-delivered SESSION_OPEN (the slot's live or a recent session
id) keeps the live session, and that normal, concurrent, retransmitted, glare and post-reboot handshakes still
converge. `--source-dir <component-directory>` replays the former REKEY storm and session reset against a pre-fix
tree. It does not qualify libsodium primitives or radio timing.

## ESP-NOW session capability word and header flag classes (1.0)

```sh
python3 components/hardwareone/test/host/test_espnow_session_rekey.py --sanitize
python3 components/hardwareone/test/host/test_espnow_rx_frontend.py --sanitize
python3 components/hardwareone/test/host/test_espnow_bond_agreement.py --sanitize
python3 components/hardwareone/test/host/test_espnow_header_flags.py
```

SESSION_OPEN (154 bytes since step 4) and SESSION_CONFIRM (162) carry each side's 32-bit capability word
(`System_ESPNow_SessionCapsCore.h`) inside the signed transcripts (92 and 100 bytes).
`espnow_session_rekey` (C22-C28) checks that each side keeps the other's word (the initiator's view confirmed at
once, the responder's after the first sealed frame of the session), that a changed caps byte in OPEN or CONFIRM
fails the signature, that a REKEY keeps the words, that a new handshake replaces them and holds none while in
flight, that the glare re-send signs the same word, and golden transcript bytes. `espnow_session_caps_core_tests`
compiles the header: bit values, the image's word, the text with reserved bits as `+0x...`, and the 1.x names of
bits 8 and 9 (named, never set by a 1.0 build).
`espnow_bond_agreement` (caseNoBondBuild) and `espnow_bond_agree_core_tests` (caseNoBondBuild) check the owner's
rule that a partner whose word lacks the bond bit gets no BOND_REQUEST: not from a tick between the session going
ACTIVE and the session hook, not after a reset, not over later sessions; one line, `no_bond_build` in bondstatus,
bondconnect refused on a confirmed word, the decline ended by a session whose word has the bit.
`espnow_rx_frontend` (testMustUnderstandFlags) checks that a frame with a must-understand header bit (0xF000) is
dropped before it is opened, acknowledged, remembered or flooded, and counted, that every other unknown bit is
ignored, and that a relay forwards a routed envelope without looking inside while the final receiver drops the
inner frame. `espnow_header_flags` is the source lint: no removed flag comes back, every flag is both set and read,
the class and the position of the drop in the front door.

## ESP-NOW deadlines across the millis() wrap

```sh
python3 components/hardwareone/test/host/test_espnow_deadline_wrap.py --sanitize
```

`espnow_deadline_wrap` compiles the production prev-key unwrap, rekey apply and prev-key sweep from
`System_ESPNow_Sessions.cpp` (unit 1), and the sensor-subscription apply with its lease-expiry owner from
`System_ESPNow_Sensors.cpp` plus the session pre-warm backoff owners from `System_ESPNow.cpp` (unit 2), against an
injectable clock placed on both sides of the 32-bit `millis()` rollover. It checks that the 5 s previous-key
window, a leased sensor subscription and the 60 s pre-warm cooldown neither outlive the wrap by ~49.7 days nor
lapse at once, that a deadline landing on zero is never stored as "none", and that leases longer than INT32_MAX ms
are capped. Source contracts confirm the sensor broadcaster task and the heartbeat handler call those owners
instead of comparing absolute times. `System_ESPNow_Deadline.h` is pasted when the overlay or the component has it
(`espnow_session_replay` and `espnow_rekey_retry` paste it the same way); `--source-dir <component-directory>`
replays the pre-fix sources. No radio is used.

## ESP-NOW mesh time

Mesh time was broken: `espnowtimesync` sent (epoch, the sender's `millis()`) in a signed broadcast, the receiver
stored the sender's `millis()` as an "offset", set no clock, and `espnowtimestatus` said "Synced: Yes". It is now
a session unicast from the mesh master to each paired device, and a device that names that master and has no
better source of its own sets its clock from it (a device that names no master takes none) (`System_ESPNow_TimeCore.h` has the rules, `System_ESPNow_MeshTime.cpp` the device side).

`espnow_time_core_tests` compiles `System_ESPNow_TimeCore.h` (the production dependency-free header): the 16-byte
frame at its stated offsets; who sends, for every combination of setting, mesh mode, role, clock state and source
(only the acting master with a set clock from rtc, ntp, manual or cm5 and no unconfirmed NTP jump); the rounds
(at once, every 10 minutes, after a clock step and 15 s after that, by hand; a peer whose session came up, once
it is 2 s old; the uptime wrap); and every row of the receiver's rule for every own source, with the frame's
wait added, a change of more than an hour held and applied when the same sender confirms it (not by another
sender, not within 10 s, not when it disagrees, not after 30 minutes), the named primary and backup that cannot
move the clock back and forth, the rank of the primary over the backup, no time at all with no master named, and
the 10 s gap after a step.

`espnow_time_status` compiles the production code of `System_ESPNow_MeshTime.cpp` between `MESH_TIME_HOST_BEGIN`
and `MESH_TIME_HOST_END` (what the handler keeps, the main-loop apply tick, the sender step, `espnowtimesync`
and `espnowtimestatus` with its `json` form) with the real `Clock::isSynced` and `Clock::isValidEpoch`
(`System_Clock.cpp`) against a settable clock. That block reaches the device only through the small `mt*`
functions above it; the harness supplies its own versions (clock, step counter and chokepoint as a recording
double, settings, paired devices with sessions, the radio queue, the console). An applied frame sets the clock
to the frame's time plus its wait and goes through the chokepoint once with the mesh source; an ignored, refused
or held frame sets nothing; a clock set by another source between the decision and the step is not overwritten;
a master sends only from a set clock with a real source (not 55 hours after a boot without one); one device per
half second; a full queue is tried again; the master starts a session with a paired device that has none; the
status text and JSON say the true source and age in each state. Source rules: the old state and sender are gone
from the component; row 35 asks for a paired sender and a session-encrypted frame and nothing else; TIME_SYNC is
not flooded and may travel through a relay; the clock module names the mesh source, writes no mesh time into the
RTC chip and takes it as no reference for the confirmed clock; the main loop applies mesh time after the SNTP
report and before the clock duties. `--component <component-directory>` replays other sources. No radio is used.

Since the review of 2026-11-18 both suites also check: a session stamped after the pass read the uptime is not
settled; a hostile frame time (INT64_MAX, INT64_MIN) is refused with no difference computed from it; mesh mode off
takes no time (`ignored_mesh_off`); a paired device that floods TIME_SYNC cannot keep the named master's frame
from being judged, four senders' frames wait and the rest are counted; the "held" console line comes once a minute at most; the frame names the master's own source and the
minutes since it was set; a master with mesh mode off starts no handshake; the json role of a backup;
`Clock::sourceWritesRtc` (mesh time and a carried clock are not written into the RTC chip), and the source rule
that the RTC task asks it before it calibrates an uncalibrated chip from the system clock.

Since the owner's decision of 2026-11-18 mesh time comes only from the master named on the device: both suites
check that with no master named no frame is taken from anyone (`ignored_no_master`, also while the clock is not
set, with or without the acting-backup bit), that the status says so and names `espnowmasterfingerprint`
(`masterNamed` in the json), that a named backup is taken by its fingerprint and ranks below the primary, that the
acting-backup bit neither names a device nor ranks it, and that a named master's frames step the clock after 10 s
every time (the old one-step-in-10-minutes limit for an unnamed sender is gone with the unnamed sender).

Since the bench of 2026-11-18 both suites also check that a fingerprint set, changed or cleared drops the sender
followed and the claim held at once (row 0 of the receiver's rule, `FollowerInputs::namedTag` from
`meshtime::namedMastersTag`): a master named in place of the one followed, or the two swapped, sets the clock
with its next frame instead of after 25 minutes; a claim held from a master that stays named as the backup is
not confirmed by its repeat; clearing both drops both with the frame that finds no master; the 10 s gap after a
step stays; the tag is the same in either letter case and differs for a swap and a cleared field. The source
rule pins that the device reads the tag from both fingerprint settings with every frame (any path that writes
them: console, web page, settings file).

Not covered: `mtSetClock` itself (`settimeofday` and `Clock::clockStepped`; the source rule pins its two lines
and their order), the real session table and send queue, and two real tasks at the kept frame at the same moment.

## ESP-NOW sensor readings

A reading is the sender's JSON text of one sensor. It used to be refused over 200 bytes with a debug line (ToF
and FM radio readings never arrived), nothing counted a lost reading, the receiver kept only its uptime, two
bit orders named the sensors, and the readers of the receiver's table freed its slots.

`espnow_sensor_core_tests` compiles `System_ESPNow_SensorCore.h` (the production dependency-free header): the one
sensor table (a name per type in order, both ways, the mask bit); the 12-byte reading header at its stated
offsets; the part counts at the direct (202) and routed (146) frame budgets with every part inside its frame,
536 bytes on both paths and 537 refused; the round trip of a reading through its parts with a part missing, out
of order, repeated, of another reading, left open for more than 2 s, and hostile counts and lengths under ASan;
the counter rule (first, new with and without a gap, repeat, late, the wrap, a restart by boot tag, eight late in
a row, the stream-start flag, silence longer than the late window, two receivers of one sender); the JSON text
check with the text that would put keys of the sender's choosing beside the receiver's own; where a reading
goes (the bond partner, the controller of a running lease, the named master for a stream switched on by hand,
and the reason when it goes nowhere); who sends and who keeps by mesh role (a worker keeps nothing, a backup
master sends and keeps, the master only keeps); and that every reason not to send has a name and a sentence of
its own. It also checks the argument of `espnowmasterfingerprint` / `espnowbackupfingerprint`: none shows,
`clear` (any case) empties, exactly 64 hex characters set, anything else (63 or 65 characters, a letter that is
not hex, a space, other words) is refused, and the reply to `clear` in each of its four cases (which of the two,
the other one named or not) says what is then true. `espnow_sensor_wire` pins that both commands go through that
rule and are admin with `clear` in their usage, and that the web settings page sends an emptied field as `clear`.
The same suite checks `fingerprintEditorArg` (an emptied fingerprint field becomes `clear`, any other text or
setting goes as it is); `oled_remote_settings` checks that the OLED settings editor's local commit uses it.

`espnow_sensor_wire` compiles `V4PayloadSensorReading`, `V4PayloadSensorStatus`, `V4PayloadSensorReq`,
`V4PayloadTimeSync`, `V4PayloadPowerStatus` and `V4PayloadEventPush` as `System_ESPNow_Wire.h` declares them,
with its frame budget constants, against the pack functions of the three rules headers: every size and offset,
the bytes the code writes are the bytes the struct states, reserved bytes are 0, every frame fits one frame
through a relay. Source rules: every `CAP_SENSOR_*` is
`espnowsensor::sensorBit` of its `RemoteSensorType` and types no number; the OLED bond page and the bond web page
type no sensor mask; `espnowsensorstatus` has no name list of its own; the old frames, limits and names are gone;
rows 151, 153 and 154 ask for a paired sender and a session-encrypted frame and nothing else, 179 keeps its bond
gate, the three mesh frames may travel through a relay and none is flooded; the status frame is a session
unicast; the two task-loop steps for readings are outside the bonded-mode block; no file but
`System_ESPNow_Sensors.cpp` touches the receiver's table; the sender plans its parts by the frame budget of its
target, starts no handshake and prints nothing, and the broadcaster task prints nothing; ESP-NOW start allocates
nothing for received readings, the ingest makes the table only after the role rule and a sender's slots only for
a reading that is whole and well formed, the status frame makes the table only after the role rule and never
writes a reading's text, freshness, counter state or counts, and `statusNotKept` is gone; a sensor listed by its
status alone that streams reads "streaming, no reading yet" on the master's line; `espnowsensorstatus` asks
the role rules; `espnowsensorstream` answers with the target of that sensor; a lease that lapses does not stop a
stream switched on by hand.

`espnow_sensor_ingest` compiles the production sender and receiver of `System_ESPNow_Sensors.cpp` (the blocks
between its `SENSOR_*_HOST_BEGIN` and `_END` marks, `remoteSensorTableEnsure` and `getRemoteSensorDataJson`)
with the rules header; the harness's radio queue hands every part the sender produces to the receiver. A reading
of up to 536 bytes arrives whole, in 1 to 3 frames direct and up to 4 through a relay; 537 bytes are not sent,
counted, and seen as a gap by the receiver. The receiver stamps a reading with its own UTC epoch (0, `null` in
JSON, while its clock is not set) and its uptime, and says how its clock was set. Lost, late, repeated,
restarted and resumed readings are counted per sender and sensor; a repeat or a late reading changes neither the
stored text nor its stamp; an outage longer than the presence time is still counted. A part lost on the air or
refused by the queue gives no stored reading. The bond frame and the mesh frame give the same slot. Types 11 to
255, short payloads, hostile part counts and a text that is not one JSON object are refused and counted. A
worker that is sent readings refuses them, counts them, says so once a minute and allocates nothing; a backup
master and the master keep them; the bond partner's reading is kept whatever the mesh role. The first reading or
status frame a receiver keeps makes the table and that sender's slots (owner's decision 2026-11-18: the master
lists a device's sensors before any reading); a status frame lists, never makes data, never counts a reading and
never makes an old one fresh; no memory and a seventeenth sender are refused for a status frame as for a reading,
also when sixteen senders were listed by their status alone.
Sixteen senders with eleven sensors each are all kept; the seventeenth sender is refused, counted and said once
a minute; a sender that is no longer paired gives its record and its slots, cleared, to the next one; no memory
for the table or for a sender's slots is refused, counted and said. Without a session nothing is sent or parked
and the task step starts the handshake; the stream-start flag goes on every change of target; every reason not
to send is counted and said once a minute by the task step, which also looks up the named masters. The stream
command's answers are checked word for word. The lock doubles record that nothing that may block, print or
allocate runs under a lock.

`espnow_dispatch_policy` runs frames 35, 132, 151, 152, 153 and 154 through the real receive gates (plaintext, a
signed broadcast and an unpaired sender dropped; a paired session frame accepted, also from a peer that is not
the bond partner), and `espnow_rx_frontend` delivers each of them through the real relay envelope path to its
handler with the origin as the sender (a part of a reading at the full routed size and the 128-byte event frame
included) and checks that none is flooded.

Since the review of 2026-11-18: the counter rule keeps an older frame (256 or less behind) out however long the
silence was; the text check is strict JSON (keys, values, escapes, numbers, UTF-8, no trailing comma) with a
list of texts the structural check let through; `elapsedSinceMs` for a stamp ahead of the pass. In
`espnow_sensor_ingest`: the clock that is not set reads 1970 in the harness and no arrival epoch is stored; a copy
into a buffer with no room for the terminator copies nothing; a status frame that no longer names a sensor clears
its streaming flag and never makes an old reading fresh; restarts and resumes in the JSON; 49.7 days after its
last frame a sensor is not listed, and its old reading not fresh, again (the age step of `sensorLinkTick`); the
4 KB reply of `espnowsensorstatus json` (`sensorStatusJsonFit`, compiled with the vendored ArduinoJson from
`espnow_sensor_status_json_harness.cpp`) is whole JSON inside the buffer at every document size around it and
`devicesMore` is true exactly when devices were left out; source rules: no hold of the table's spinlock clears
memory in bulk or walks every sender's slots, the status frame's streaming bit, its settle wait and its boot tag,
the part length guard, the target rule's lease inputs. `espnow_deadline_wrap` now also runs
`sensorTargetInputsLocked` and `stopSensorDataStreaming`: a subscription switches sending on, a renewal sets no
stream-start flag while a new stream does, a leased sensor goes to the controller while one switched on by hand
goes to the named master, and a stop by hand ends the lease and forgets the controller.

Not covered by any host suite: the broadcaster task loop itself (`sensorBroadcasterTask`, with its lease-lapse
branch), `announceSensorStatus` and `sensorReceiverTargetsRefresh` (they read driver, bond and pairing state;
source rules pin the status frame's parts), `startSensorDataStreaming`, the commands `espnowsensorstream` (its
answers are covered) and `espnowsensorstatus` (its reply buffer is covered), the OLED, G2 and MQTT readers of the
table, the transport below `v4_send_sensor_part`, and the real locks under two tasks.

## ESP-NOW battery state and events

POWER_STATUS (152) and EVENT_PUSH (132) were reserved numbers with no payload: a master could not see a worker's
battery or its alerts.

Since the bench of 2026-11-18 `espnow_power_events` also checks that the event counts of other devices
(`espnowPowerEventsPeerText`, `espnowEventCountsJsonFor`, read by `espnowsensorstatus`) are for an admin caller
only (`peCallerIsAdmin`, `currentExecIsAdmin` on the device): the status line keeps the battery and leaves the
events out, the json of one device gets no counts; source rules pin that `espnowsensorstatus` says "Event
counts: admin only." once and its json for one device has `"eventCounts":"admin only"` instead of `events`.

Since the review of 2026-11-18 `espnow_power_events` also checks: each of the 13 bits of the battery frame's
`known` reaches the JSON key it names, alone and in pairs, against the frame's specification written out again;
no arrival epoch is stored while the clock is not set (it reads 1970 in the harness); a mesh id outside the
lists goes to list 0; the status line's event count; the text form's "[text cut]"; an event goes only to a
named master whose session is ready; an event posted after the pass read the uptime is 0 ms old; a source
outside the table goes out as unknown; a kind name longer than its field is cut and flagged; a full queue takes
no token of the rate limit; events under the floor cost nothing of the four a look (an alert behind a ring of
lesser ones goes out at once); both JSON replies are whole JSON inside reply buffers of 900 to 4096 bytes;
source rules: no hold of the spinlock searches a full list or walks every list, the oldest event held and the
battery sample's age come from the tested rules. `espnow_power_event_core_tests` adds `eventOldestHeld`.

`espnow_power_event_core_tests` compiles `System_ESPNow_PowerEventCore.h` (the production dependency-free
header): the 24-byte battery frame and the 128-byte event frame with every field at its stated offset, reserved
bytes 0, the round trip, a payload that is too short; the battery values (every flag, nothing valid, a rate that
is not a number or beyond 16 bits, a voltage and a percentage out of range); when a battery frame is due (at
once, every interval, on a change of status, charging, USB or presence, never within 5 s, never with the
interval 0, across the uptime wrap) and what the setting may hold; the event floor for every setting and tier; the
token bucket (burst, refill, time spent full earns nothing, the wrap); whether a device sends and the order of the
reasons why not; the text of a frame (a field with no terminator, control bytes, invalid and cut UTF-8, a frame
of 0xFF under ASan) and the JSON escape; the sender's cut (flags, zero padding, never inside a UTF-8
character); the event list (a free entry first, then the oldest entry of the sender that holds the most, walking
by receive number, a `since` from before a restart).

`espnow_power_events` compiles the production code of `System_ESPNow_PowerEvents.cpp` between
`POWER_EVENTS_HOST_BEGIN` and `POWER_EVENTS_HOST_END` (the sender step, both receive paths, the per-sender
records, the event lists, `espnowbattery` and `espnowevents` with their json forms) with the rules header against
a host device: a battery, an event ring of 48, sessions, and a radio queue whose acknowledgement watch follows
the production rule (two watched frames in a row that nobody acknowledges clear the session). Checked: the
battery frame goes to each named master with one counter value, at once, every interval, on a change but not
within 5 s, never with the interval 0, also with no valid sample, not over a session under 2 s old, with every
frame's acknowledgement watched; a worker whose master lost the session has a new one within three battery
intervals; the event floor per setting, the kind as its name, long texts cut with their flags, eight at once
then one every 2 s with what was left out in `skipped` (which stops at 65535), the cursor waiting without a
session and following the head when nothing is sent, a full queue; a worker refuses both frames (counted, said
once a minute, nothing allocated) and a master and a backup master keep them, stamped with their own UTC clock
(0 while it is not set) and uptime; lost, late, repeated and restarted frames by the counter rule of the
readings; what the sender did not know is `null` in JSON; 24 and 128 bytes of 0xFF give valid JSON with
`unknown` names and cleaned text, and quotes and control bytes are escaped (every JSON reply of the suite goes
through a syntax check); sixteen senders with a seventeenth refused; 24 events per mesh with the 25th replacing
the first, `since`, `overwritten`, a `since` from before a restart, mesh 1 not pushing out mesh 0, the
receiver's rate limit per sender, a busy sender not pushing out a quiet one; both JSON replies page (`next`,
`more`) and stay whole JSON inside the 4 KB reply; nothing that may block, print or allocate runs under the
lock; and nothing is posted into the receiver's own event ring. Source rules: rows 132 and 152 ask for a paired
sender and a session-encrypted frame and nothing else, both may travel through a relay and neither is flooded;
the battery frame asks for an acknowledgement and has it watched; the role rule comes before any allocation; the
task step runs once in the `espnow_task` loop, outside every bonded-mode block and after `sensorLinkTick`; the
file names no bond symbol and never `systemEventPost`, and its tested block reaches the device only through its
`pe*` functions; the two settings and the four commands exist; `espnowevents` is admin only and `espnowbattery`
is not (owner's decision 2026-11-18).

Not covered by any host suite: the `pe*` device functions themselves (`peBatteryInputs` copies `BatteryState`;
`peSendPower` / `peSendEvent`, which the source rules pin; `peEventFetch` over the real event ring), the two
thin handlers, `cmd_espnow_powerinterval` and `cmd_espnow_eventpush` (source rules only), the real
`sendStatusSweep` that clears a session after two unacknowledged frames (the harness follows its rule), and the
battery, event and target lines of `espnowsensorstatus`.

## ESP-NOW forwarding for a bond partner

A bond worker never sends its readings, battery state, events or sensor status to a mesh master itself; its
bond master may forward them to the master named on the bond master when that master is paired in the bond
master's primary mesh (`bondforward on`); the mesh master accepts any paired device that announces. The bond
master announces the link in FORWARD_LINK (155); a forwarded 154/152/132/151 frame carries the link number in a
reserved byte (and, for readings and events, a reserved flag bit). The mesh master asks for the partner's
readings with a SENSOR_REQ (153) to the bond master whose reserved byte 14 carries the link number; the bond
master turns it into its own bond-link stream control of the partner's sensors, with the lease. The bond request
and notice carry whether the role was chosen (`bondrole`). Design and decisions:
`/Volumes/hw1build/bond-agreement-build/forward/DESIGN.md` (sections M to O) and `DESIGN_DECISIONS.md`.

`espnow_forward_core_tests` compiles `System_ESPNow_ForwardCore.h` (the production dependency-free header): the
96-byte frame byte by byte, unknown ops, short and hostile frames; `forwardDecide` row by row; the partner's
agreement (unknown after agreed keeps the link, never agreed does not, a decline or a partner change forgets
it); link numbers and their serial order; the bond master's state per master (announce, back-off, only a matching
answer counts, stale, refused and its 10-minute retry, a new session id announces again while a rekey keeps
forwarding, query, refresh, three withdraw tries, the withdrawal of everything once per session, which frames go
to a target and why not); `forwardFrameAllowed` (the link's partner, the hold epoch, the setting, a decline);
every announce check on the master; the generation rule with a late announcement in both orders, after a
withdrawal, across the u8 wrap, and a late withdrawal of everything; every withdraw row; every `attribute`
verdict and the resume once per stream; the query limit; `recordReusable`; names; the primary mesh
(`forwardDecide`'s `not_primary_mesh`, `primaryMeshSlot`, `inPrimaryMesh`); the 16-byte sensor status frame with
its link byte and whose it is; every row of `sensorReqDecide` and the leases (subscribe, renew, one-shot,
unsubscribe, expiry, a stream already running, by hand, the end of every lease, the "on" again on a new bond
session, the clock wrap); X's numbers through the bond master to the master with a loss on each hop and a pause
that is not counted as loss. `espnow_bond_agree_core_tests` adds the role: `roleForBond`, `roleYields`, an
automatic device that yields to a chosen one by request or by notice (a lost request), both chose the same (a
conflict), neither chose, a peer's earlier request used by `bondconnect`, ENDED, a partner change.
`espnow_sensor_core_tests` and `espnow_power_event_core_tests` add the link byte of the reading header, the
battery frame and the event frame, bit 4 of the event flags, `pushDecide`'s bond branch for every mesh input,
and the fairness of a full event list by (sender, via).

`espnow_forward_rx` compiles the production code of `System_ESPNow_ForwardRx.cpp` between
`FORWARD_RX_HOST_BEGIN` and `FORWARD_RX_HOST_END` against a host device (role, paired devices with keys and
names, a recording radio): every answer of an announcement (any paired device, no list), the console lines, a
replaced partner and a late announcement of the old one, whose a frame is (own, bond link, forwarded with the
resume, the sensor status, malformed marks, unannounced and old link numbers refused with a query at most every
5 s), withdraw and the withdrawal of everything, a worker's links ended by the tick, a refused announcement said
once a minute, sixteen entries and the reuse of an unpaired one, where a sensor request for a partner goes
(`<partner> via <device>`, the name alone, addresses, an unknown via, no link, an ended link, the current link
number), `espnowforwardlinks` (text, JSON, paging, "not confirmed"), and nothing that may block, print, send or
allocate under the lock. `espnow_bond_forward` compiles the production code of
`System_ESPNow_BondForward.cpp` between `BOND_FORWARD_HOST_BEGIN` and `BOND_FORWARD_HOST_END` against a host bond,
named masters and sessions with session ids: every reason with its `bondforward` reply, `bondforward sensors`,
the announcement's bytes, readings cut for the master's path with the partner's header plus the bit and link
number (four routed parts of 536 bytes), battery state acknowledged to both masters, events with bit and byte, a
frame of another source or after a partner change began, off at once, a full queue, `not_kept_here`, the JSON
object and the `bondstatus` lines (the longest of each proven to fit), a rekey, a new session, a query, the
refresh, an absent partner, a decline, a partner change (the tick waits while it runs), another hold epoch, a
sensor change while an announcement is out, stale, refused, a new named master, a handshake, promotion and
demotion, the partner named as master (and a master that became the partner); a master paired in another mesh
than the primary one (never announced to, gets nothing, a link it had is withdrawn, `inPrimaryMesh` in the
JSON); the partner's sensor status sent on acceptance and on change with the forwarded sensors and the link
byte; the mesh master's sensor requests (applied, renewed, ended by the lease, refused for each reason and said
once a minute, a stream already running, by hand, unsubscribe, one-shot, the "on" again on a new bond session or
after a failed send, forwarding off, a partner change) and their JSON and `bondstatus` line; an "off" that could
not be sent, sent again once the partner is back (also after the link ended) and forgotten with a new partner or
the worker role; a stream switched on by hand, never touched by a lease; leases ended when their master leaves the
primary mesh, another master is named, or their sensor is no longer forwarded. Its source rules also hold that
"the user streams it" is the saved `bondstream` setting, never the device's own streaming flags, and that a bond
worker applies every stream switch of a loop (`espnowsensor::StreamCtrlQueue`, tested in
`espnow_sensor_core_tests`).

The existing suites cover the rest: `espnow_sensor_ingest` (a forwarded reading kept under (partner, via), its
JSON with `link` and `via`, the partner's own counters, a direct and a forwarded record side by side, refused
link numbers and streams, bit and byte that disagree, the resume taken by the last part of a reading in four
parts, a forwarded record whose link ended given to another sender, the bond master handing its partner's
reading to its forwarder, a repeat not handed on, bit 2 on 179 malformed, `not_kept_here`),
a forwarded sensor status under (partner, via) before any reading, an old link number refused, the bond
master keeping the partner's status (185) whatever its role and handing it on, a link byte on 185 malformed),
`espnow_power_events` (a bond worker sends 180/182 to its partner only and starts no handshake, the bond master
keeps them whatever its role and hands them on, two workers refuse, a mark on 180/182 is malformed, forwarded
battery state and events under (partner, via) with the resume, the via's mesh list, `link`/`via` in JSON, a
0xFF frame now read as forwarded and refused), `espnow_dispatch_policy` (155 through the real gates; 180/182/185
from the partner only), `espnow_handshake_backoff` (180/182/185 without a session are dropped, not parked),
`espnow_sensor_wire` (the layouts with the link bytes of 151 and 153, rows, the routed list, the opcode map,
`meshDataAllowed` at the three sites, a request with the link byte handed to the bond master's side before the
own-sensor path, the worker's status to its bond master only, the single-bond-field rule, every reader passes
the via, MQTT publishes own readings only), `espnow_bond_agreement` (on the production glue: `bondconnect`
keeps a chosen role, the request and notice carry the flag, an automatic role yields to a partner's request or
notice and is answered with the new role, both chose the same is a conflict, a peer's earlier request decides
`bondconnect`).

Not compiled by any host test: the radio and the relay timing, `bondSendEncryptedAsync`, `espnowSessionIdOf`
and the session table, `cmd_espnow_sensorreq`, `v4h_sensor_req`, `bondChangeRole` and `cmd_bond_role` (source
rules), `espnowPeerInPrimaryMesh`, the HTTP handlers, the web page script, and the G2, OLED and MQTT pages (source
rules only).

## Time: the millis() wrap and stored clock values

`millis()` is a 32-bit count that returns to 0 after 49.7 days. These suites put a settable 32-bit clock on both
sides of that point (`unsigned long` is 64 bits on the host, so each clock double returns `uint32_t`) or feed
stored values above 2^31. Each takes `--cxx`, `--sanitize` and `--component <component-directory>` to replay
other sources.

`logout_reason_table` compiles the production `storeLogoutReason` (`WebServer_Server.cpp`) with `LogoutReason`
and `MAX_LOGOUT_REASONS` from `WebServer_Server.h`; the table sits between two guard rows. With the table full,
the ninth address must replace the oldest row by age: without a wrap, with every stored stamp before the wrap
and `now` after it (the search used to select nothing and write one element before the table), and with stamps
on both sides of the wrap (the row with the smallest number is then one of the newest).

`millis_deadlines` compiles the production `System_Deadline.h` (`deadlineAfter`, `deadlinePending`: a deadline is
a 32-bit `millis()` value, 0 means none) together with `System_ESPNow_Deadline.h`, whose two names forward to it.
A deadline that was never set must be pending at no uptime (a bare signed difference read it as pending from day
24.9 to day 49.7), one set just before the wrap is pending until it is reached, one left over from before the
wrap stays passed, an oversized delay is capped, and no deadline is stored as 0. Source rules: each deadline
variable in the test's table (`SITES`) is asked through `deadlinePending()` in its file, and neither a comparison
with `millis()` through `<` or `>=` nor a signed difference without the zero test is left for it. The four
ESP-NOW suites that paste `System_ESPNow_Deadline.h` paste `System_Deadline.h` in front of it.
The table holds the four deadlines that use 0 for "none" or "now" (glasses LLM notice, OLED camera retry, OLED
transcription poll, R1 Health refusal flash) and the five file-scope UI deadlines that were compared with
`millis()` directly (login and change-password error boxes, settings editor error and quick-settings status,
the event stream's notice burst).
Four of them stay in a file-scope variable after they have passed (the OLED camera retry, the R1 Health refusal
flash and the two error boxes); a fifth rule (`CLEARED`) requires the code that reads each to set it back to 0
once passed. Left in place, the camera retry read as pending again 24.9 to 49.7 days later and kept the preview
worker from starting for that window, and an old error box came back over the login form.
The same suite runs the OLED timed dirty deadline as production code: the definition of `oledDirtyUntilMs`,
`oledMarkDirtyUntil` and `oledIsDirty` (`OLED_Utils.cpp`) with every other reason for "dirty" quiet. A mark keeps
the frame dirty until it is reached and no longer, a later mark extends and an earlier one does not shorten; a
mark made on day 40 does not keep the frame dirty after the wrap (the old running maximum of raw `millis()`
values did, for 40 days, and dropped every later mark as smaller); a mark across the wrap works on both sides; a
passed deadline is set back to 0.

`elapsed_time` covers elapsed time since a `millis()` stamp, which is an unsigned 32-bit difference:
`(long)(now - stamp) < interval` is negative, so "not yet", for a stamp between 24.9 and 49.7 days old. It runs
`g2HealthTick`, `healthPollStatusLine` and `g2HealthLoggingStatusLine` (`G2_Health.cpp`) and
`g2RingPollVitalForLogging` (`G2_Ring.cpp`) with stamps that are 25, 30 and 49 days old and across the wrap; the
driver writes `long` in the inserted text as the 32-bit type it is on the device, so a base run shows the fault
(17 of 36 checks). Source rules cover all thirteen comparisons of the change, at twelve stamps (the sensor
log tick, the health logging mine, the Health page, the EvenCore failure dump throttle, the ring vitals poll):
each is `(uint32_t)(now - stamp)`, no signed difference is left for those stamps, and the three stamps another
task renews are copied before the clock is read. `g2_health_graph_core_tests` holds the one test that stays
signed on purpose: the unknown-timestamp dedupe of `pushSeries` orders two stamps, and a replayed sample may be
older than the series tail.

`mqtt_next_publish` compiles `mqttMsUntilNextPublish` (`System_MQTT.cpp`), the value behind the "Next Publish"
line of `mqttstatus`: counting down, 0 when due and when overdue (the old `(last + interval) - millis()` wrapped
to about 4294967000 ms when the command ran after the due time and before the main loop had published), and
right across the wrap. Source rules: `cmd_mqttstatus` copies the main loop's stamp before it reads the clock
and prints that function's result; the reply line is unchanged.

`uptime_owner` compiles the production `SelfDevice::uptimeSeconds` (`System_SelfDevice.cpp`) against a settable
64-bit boot timer: it equals `millis() / 1000` up to the wrap and counts on after it (4294966 s, then 4294968 s,
while `millis() / 1000` is back at 1), for 100 days and for ten years. Source rules: the eight files in the
test's table (`ASKERS`) ask the owner, and the only divisions of the 32-bit millisecond clock by 1000 or 60000
left in the firmware are the three fallbacks for a wall clock that reads 0 (two glasses microphone file names,
one ESP-NOW capture file). Source rules also pin what the four chip-temperature askers do without a reading.

`stored_time_values` compiles the production `parseBootAnchors` (`System_User.cpp`) and `parseOneTrigger` with
`nextFire` (`System_Automation.cpp`) with the real ArduinoJson. `value | default` reads with the type of the
default, and these values are stored as `uint32_t`: a `millisAtSync` of 3000000000 (a sync after 34.7 days of
uptime), an `epochAtSync` of 2200000000 (the year 2039) and an `intervalMs` of 2592000000 (30 days) must read
back as written, where an `int` default gave 0; every value up to 2^31 - 1 reads back as before, and a zero,
negative, text or missing value is still "none". Source rule: no read of `intervalMs`, `epochAtSync`,
`millisAtSync` or `ntpAnchorId` in those two files goes through a default other than `uint32_t(0)`.
The suite also runs the production `resolveUserTimestamp` with `formatEpochAsIso8601`: a user created at
2^31 - 1 ms and synced at 2^31 + 5 ms is 6 ms before the sync, one created just before the `millis()` wrap and
synced just after it is 8.192 s before it, both in the same boot; one created later in the count than the sync
is from a previous boot. The difference of the two 32-bit stamps is taken unsigned and read as `int32_t`.

`automation_ids` compiles `System_AutomationId.h` as it is and runs the production `sanitizeAutomationsJson`
(`System_Automation.cpp`) with the real ArduinoJson. An automation id is stored as a number and read as a signed
32-bit `long` everywhere; new ids were `millis()`, a number above 2^31 - 1 after 24.9 days of uptime, which
`as<long>()` reads as 0. `automationIdSeed` and `automationIdAdvance` stay in 1..2147483647 at every uptime; a
duplicate found on day 35 gets an id in range; a stored id above the range is given a new one with the rest of
the automation unchanged; a file whose ids are in range (2147483647 included) is not touched; what
`automationIdTextAboveRange` flags, the sanitizer changes. The sanitizer counts what it fixed
(`AutomationSanitizeCounts`, optional: a clean file reports 0 and 0 whatever the struct held, a duplicate counts
as a duplicate even when it is also above the range) and `automation sanitize` replies with it
(`automationSanitizeReply`: "fixed 1 duplicate ID", "fixed 2 duplicate IDs", "fixed 1 out-of-range ID",
"fixed 3 out-of-range IDs", "fixed 1 duplicate ID and 3 out-of-range IDs"; "Sanitize: no changes needed" as
before). `automationIdTextInRange` accepts a whole number 1..2147483647 only (white space and leading zeros
allowed; no sign, fraction, 0, overflow or value above the range). Source rules: both places that make an id use
the owner, the scheduler pass leaves an automation with an id above the range out and queues the sanitizer,
`automation add` refuses an `id=` outside the range with "Error: id must be a whole number from 1 to 2147483647"
before it loads the store (validate-only included), and the `automation sanitize` branch replies through
`automationSanitizeReply` and no longer spells "fixed duplicate IDs". `automationIdPosInJson`, which the
commands that take `id=<n>` use to find their automation, finds an id only as a whole number: 12 is not found
inside 123456, and the exact id is found behind a longer one that starts with the same digits.

`automation_store_mutations` compiles the store's mutation owner (`mutateAutomationsJson`) and the commands and
writebacks on it and runs them on the scheduler harness's fake store with the real ArduinoJson: enable, disable
and delete act on exactly the automation with that id, found in the parsed document; a quote or brace inside a
name, command or condition survives a toggle; `"id"` stays the first member of every stored object; an add is one
write and a dry run writes nothing; two mutations started at the same moment both land; a document that does not
parse, has no `automations` array or is in a newer format is never written; `userdelete` disables every
automation the deleted user created and reports the count; and the scheduler claims a pass only after a
filesystem try-lock. Source rule: the only writer of automations.json is `automationsStoreWrite`.

`command_args_keys` compiles the production `CommandArgs` (`System_Command.h`) with its constructor, `parse`,
`nextKeyToken`, `value` and `hasKey` and their helpers (`System_Command.cpp`) on a small String. A key is read
only at the start of a token (the start of the line or after white space) and outside a quoted value:
`dayofmonth=5` is not `month=5`, `bootdelayms=` is not `delayms=`, `condition=` is not `on=`, and an `id=` inside
`commands="..."` is not the id of the line. Quoted values keep their spaces and unescape `\"`. The token walk
(`nextKeyToken`) tells a key=value token from a bare word, so the rest of an unquoted value with a space shows
as a bare word. It takes `--cxx`, `--sanitize` and `--component <directory>`.

`clock_set_question`: "is the clock set" has one owner, `Clock::isValidEpoch` (the year 2020 or later). The
suite runs the real `Clock::isValidEpoch` and `Clock::isSynced` (`System_Clock.cpp`) against a settable clock:
not set while the clock counts from 1970, also 55 hours after boot (the mesh time command's old test
`epoch < 100000` let a 1970 date out to the mesh from hour 28 on), and set from 2020 on. The mesh time command
itself is run as production code by `espnow_time_status` (it refuses and sends nothing while the clock is not
set) and asks the owner through `mtClockSet`.
Source rules: no comparison of a time with 100000 is left; `cmd_certinfo`, `cmd_certgen` and the expiry check
at HTTPS start ask the owner; the scheduler pass asks it and takes its next run from `automationNextRunTime`;
the due cache leaves a time-of-day trigger out while the clock is not set. Four own tests stay and are listed
in the test (`KEPT`) with their reasons; a new hand test elsewhere fails the rule. The scheduler's behaviour is in `automation_scheduler` (`clockNotSet`): with the clock not set, a trigger that
names a time of day is not part of the next run and is not "due" in the cache; an interval trigger counts uptime
instead, one interval from when the scheduler first sees it, and the file's next run is not used meanwhile (see
`uptimeSchedule` under "Automation scheduler"); the public `computeNextRunTime` (the clock-set case) still takes
the file's next run; when the clock becomes set the cache asks for one pass and then holds the file's next runs
again. `Clock::isValidEpoch` is compiled from `System_Clock.cpp` there too.

`automation_condition_bounds` is about a size, not a time, and sits here with the group that fixed it. It
compiles the production `validateConditionSyntax` and `executeConditionalCommand` (`System_Automation.cpp`).
Their scan for ` THEN ` was bounded by `i < len - 5` on a `size_t`: for a command of 3 or 4 characters that
starts with `IF ` the bound wraps and the scan reads past the terminator. Each short command is followed, after
its terminator, by the text ` THEN go ELSE no`, so a scan that leaves the command finds it (and under
`--sanitize` the command also sits in an exactly sized heap block). Well-formed commands, the shortest complete
ones included, behave as before. Source rule: no `for` in the file has a `size_t` bound of the form
`i < length - n`.

## ESP-NOW peer message store

`espnow_message_store` compiles the whole message-buffer section of
`System_ESPNow.cpp` (slot table, allocation on a peer's first store, the
received and sent byte rings of `System_ESPNow_HistoryCore.h`, chunked stores,
merged reads, collapsed views, `espnowCopyCollapsedHead`, delivery stamps, the
`espnowhistorykb` status line) with the store's macros, types, declarations and
constructor items from `System_ESPNow.h` and FreeRTOS mutex, allocation, heap,
clock, settings and logging doubles (`ps_alloc` fills new memory with `0xA5`
and counts bytes and the policy per tag). Each case runs in its own process
(23 cases). They check: the real constructor items on `0xA5` memory leave the
store empty, nothing works or allocates before `peerHistoryStoreInit`, which
allocates nothing, says one start line and latches once, and reads before the
first block return nothing; merged reads, paging,
chunking, collapse, reassembly and delivery stamps; a 16 KB ring keeps a
contiguous suffix ending at the newest; a ref taken before new peers arrive
still reads its record; reads for unknown MACs create no slot and allocate
nothing; blocks come with a peer's first message (`RequirePsram`, or
`RequireInternal` without PSRAM at runtime and only above the internal-heap
floor, counted in 8-bit-capable internal RAM: an IRAM-only heap of the classic
ESP32 does not count), never move and stay within the memory bound; each
allocation is said in its own line; the 17th peer, a failed allocation (held
off 5 s from the failure, no slot, no seq used) and used-up message numbers are
refused with their line at most once a minute and the count folded in; the total
cap and the board maximum lower the ring, a ring lowered below a whole KB is
said in bytes (start and status lines), and more mesh slots than
`MESH_PEER_MAX` give `MESH_PEER_MAX` slots; a view asked for more than
`ESPNOW_HISTORY_VIEW_MAX` entries gets the newest `ESPNOW_HISTORY_VIEW_MAX`;
the conversation and all-peers views are ordered by the head's time when
pieces arrive out of order; a block installed by another store in
between is used and ours freed after unlock; one peer's stores leave every other
block byte-identical; a damaged header, `RingState` field or ring size is
cleared by the next reader, which returns what was before the damage, and a
store right after is visible (a sane ring of the wrong size is put back before a
store writes; `ringBytes` moved together with both ring sizes clears both; a
delivery stamp that meets damage clears the sent ring); the attribution of each record; an unterminated
300-byte buffer and a 52,000-byte chunked text; `getAllMessages` paging through
three peers in seq order with an idle poll that decodes nothing; refs resolve
by seq (corrupted offsets by the walk; dropped records, the other ring and
out-of-range slots are refused); and that a refused or contended store lock
changes nothing and is never left held. A storing thread also races the
readers, which also copy heads of stale refs, under ASan/UBSan;
`--thread-sanitize` reruns that race under ThreadSanitizer. Every case checks
that no log line, allocation or free happens while the store lock is held.
Source contracts confirm that the OLED, G2 and web readers and
`cmd_espnow_messages` copy records through the store API, hold no pointers
into the history and name rows through `espnowHistoryDisplayName` (the CLI
through `espnowHistoryPageNames`; the resolver takes `GroupLockGuard` and uses
no `String`), and that `peerHistoryStoreInit` allocates nothing; that
`espnowmessages json` keeps its keys, in order, with `name`, `sendState`,
`sent`, `seq`, `reqId`, `piece` and `of` taken from the page; and that ESP-NOW
start latches the size from `gSettings.espnowHistoryKb`, the `historyKB` row is
U8 0..`ESPNOW_HISTORY_SETTING_KB_MAX` with default 0, the `espnowhistorykb`
help and usage name the same range, the command shows the status line with no
argument and sets `historyKB` with one, and `Settings()` defaults it to 0. A
UBSan report fails the case under `--sanitize`. `--source-dir <component-directory>` overlays production
sources. No radio, display or filesystem is touched.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^espnow_message_store$' --output-on-failure
python3 test_espnow_message_store.py --thread-sanitize
```

`espnow_history_core_tests` compiles `System_ESPNow_HistoryCore.h` (the production dependency-free header: a
peer's received and sent byte rings of packed records, a 20-byte header plus the text) as is. It checks the header
round trip, that every single changed header byte is refused, and that a reserved flag bit, a nonzero byte 7, type 8
and seq 0 are refused; the ring budget, the attribution guess and the bounded name copies (cap 0/1/2, unterminated
32-byte names); placement against an independent deque model on several ring sizes (exact fit at the end, the wrap
marker, eviction across the wrap, cut texts, utilisation after eviction); refusals that leave the ring byte-identical;
that the validated walk stops at the first changed byte and at a damaged wrap marker; text full of valid fake headers;
`findBySeq` with every hint, including hints in the last 16 bytes of a segment that ends the allocation; stale
references, which resolve exactly while their record is live even when a fake header carrying their seq sits at their
old offset; delivery stamps that change only the flags and check bytes; reassembly and the collapsed views against
ports of the old ring-of-structs code, the newest-`maxN` window and the 64-entry same-source look-back; a remote
command's streamed lines (single records) and its chunked result under one reqId, where each line keeps its row and
the result is its own row that reassembles to the result alone; every corrupted `RingState` field refused by every
operation, and coordinated damage past the end (head, tail or `wrapAt` beyond the size with the other fields kept
consistent) refused before `append` writes; damage met while evicting, which clears the ring and keeps
the new record; and the decode counts at the largest configuration (32 rings of 32 KB). Buffers are exactly sized or
guarded by canaries and the suite runs under ASan/UBSan with `HW1_SANITIZE=ON`.

`espnow_history_stress_tests` (`test_espnow_history_stress.cpp`) drives the same header with random operations and
compares it after every step with a reference model: a deque of records per ring, with its own placement rules and
its own header encoder written from the design. Each episode picks 1 to 16 peers (up to 32 rings), ring sizes from
1024 bytes to 64 KB (also odd multiples of 4, the received and sent ring in one block or apart), and text lengths
from 0 to 300 aimed at exact fits and wraps. The operations are appends (with refusals that must leave the ring
byte-identical), `findBySeq` with right, wrong, stale and out-of-segment hints, references kept from long ago (new text
plants a valid header with a dead reference's seq at its old offset; it must be refused), valid headers with live seqs
planted in free space, delivery stamps by reqId, reassembly, `mergeBySeq` with `sinceSeq` paging, `collapseNewest`,
`sortByTime` and bad arguments. After a step the touched ring must match the model: every `RingState` field, every
record byte (header layout, check byte, text, zero padding, wrap marker), the validated walk and `checkRing`. The
corruption part works on an exactly sized copy of a ring: with one changed byte or one consistent header forgery
(right check byte, wrong direction, seq, type or flag bits) every reader must return exactly the records before the
damage and report it, and an append that evicts into it must clear the ring; with many changed bytes, broken
`RingState` fields or made-up rings it checks bounds only, under ASan with UBSan aborting on the first error. The
default run is 1,000,000 steps (about 2.5 million appends, 20 s); a failure prints an episode seed:

```sh
/tmp/hw1-hardwareone-host/espnow_history_stress_tests --episode-seed 0x...   # replay one episode
/tmp/hw1-hardwareone-host/espnow_history_stress_tests --long                 # 20 million steps
/tmp/hw1-hardwareone-host/espnow_history_stress_tests --ops 5000000 --seed 7
```

## ESP-NOW FS RPC locking

```sh
python3 components/hardwareone/test/host/test_espnow_fslist_locking.py --sanitize
```

`espnow_fslist_locking` compiles the FS RPC pending table, deferred slot, lock guard, send/cancel API, request capture, reply matching, cmd_exec runner and timeout sweep from `System_ESPNow_FsList.cpp` (with the packed wire structs and the header's tunables and callback typedefs) into a harness where host threads stand in for httpd, espnow_task and cmd_exec. The FreeRTOS mutex double honors bounded waits and records its owner; the blocking send can be parked on a gate (a stalled tx dispatcher); the per-op workers are counting doubles. It checks that no LIST/STAT/GET send holds the module mutex, that a captured request is served while a client send is blocked or another task holds the lock for 120 ms, that a request which cannot take the lock within espnow_task's bounded wait is answered TOO_BUSY, that the deferred slot is always released, that `fsListCancel` frees its slot so no callback fires afterwards, and that send failure, cmd_exec queue-full and the single-slot TOO_BUSY are unchanged. `--component <dir>` and `--source-dir <dir>` read production sources from another tree; the f4cc542 sources fail 14 checks (the cmd_exec runner gives up after 50 ms and leaves the slot pending, and the client sends hold the mutex). The tx dispatcher and radio delivery are not modeled.

## ESP-NOW multi-frame sends and STREAM line size

```sh
python3 components/hardwareone/test/host/test_espnow_tx_multiframe.py --sanitize
```

`espnow_tx_multiframe` compiles the production `espnowtx` namespace from `System_ESPNow_Tx.cpp` (queue, completion pool, `submit`/`submitSync`, `sendAead`/`sendAeadSync`, the dispatcher task body and `stop()`) against single-threaded FreeRTOS doubles and a stubbed radio whose multi-frame path models the per-fragment ACK wait; while a sender waits, producers submit fire-and-forget jobs and the dispatcher drains unless it is the task that is waiting. A 4 KB `sendAeadSync` from cmd_exec must run on cmd_exec with no queue drops among the 42 jobs submitted during its waits; the destination's single-frame cap decides (150 B from cmd_exec to a routed peer runs on cmd_exec, to a direct peer and exactly at 202 B it stays dispatcher work); another task's synchronous multi-frame job (httpd's 160 B payload to a routed peer; a 144 B FS request now fits one routed frame) stays dispatcher work; failure accounting, the self-deadlock guard, refusal before `init()` and after `stop()`, and the fire-and-forget path are unchanged. A second unit extracts `sendEspNowStreamMessage`, its session helper and lookup and `v4_single_frame_cap` from `System_ESPNow.cpp` and checks that STREAM lines are clamped to one frame (202 B direct, 146 B routed) on the legacy and the command-session path, with the rate limit, the cmd_exec-only session gate and suspension kept. The script checks that `System_ESPNow_Wire.h` still defines the budgets the doubles mirror. `--component <dir>` / `--source-dir <dir>` read the production sources from another tree; the f4cc542 sources fail the dispatcher-placement, queue-drop, routed-cap and four STREAM-length checks. Radio timing, real ACK loss and task stack depth are not modelled. The second unit also extracts `streamCmdIdForCurrentTask` and `espnowStreamOutputWanted` (A9). The session a line streams to is the one of the command `cmd_exec` is running, taken from its command context (origin `ORIGIN_ESPNOW`, `ctx.id` == the request msgId): with two requests queued each requester gets only its own output, and a web command or a deferred job on `cmd_exec` streams nothing to a queued requester. The `broadcastOutputCore` gate stays closed for other tasks and origins while a request is only queued, and the legacy `startstream` sink still hears every task while a remote command is in flight. A source check pins the order around it: the per-task validate drop before the gate, CMD_RESP before `destroyStreamSession` with no empty STREAM_BEGIN / STREAM_END frame (removed at 1.0), and `commandExecTask` clearing the context before the async callback. The pre-fix tree (one global id set at queue time) fails S7-S10 and the source check.

## ESP-NOW session list output

`espnow_sessions_output` (`test_espnow_sessions_output.py`) runs the production `cmd_espnow_sessions` over a session
table double with 0 to 16 ACTIVE sessions, every capability bit set and the largest numbers. The JSON form
(`docs/APP_JSON_CONTRACT.md`) is always whole JSON inside the 4096-byte debug buffer, with the sessions it cannot
hold left out at the end and counted in `more`; the text form always ends in a NUL and says how many sessions it
did not show. A tree that writes into 1024 bytes fails it (four such sessions overflowed that, and `serializeJson`
then left the reply without a terminator). `--component <dir>`; ASan/UBSan under `HW1_SANITIZE=ON`.

## ESP-NOW metadata push

`espnow_metadata_push` extracts the production metadata setters (`metaGetSet`
with the metadata text policy, `cmd_espnow_stationary`),
`buildMetadataPayload`, `sendMetadata`, the metadata tracking block and
`metadataPushTick` from `System_ESPNow.cpp`, plus the real `SessionState`
and `V4PayloadMetadata` definitions, and runs them against
doubles for the session table, the encrypted
transport, settings and the clock. A local room/zone/tags/friendly name/
stationary/device name change must reach every ACTIVE session (there is no
per-peer opt-out since SUBSCRIBE_UPDATE was removed in ESP-NOW 1.0)
as one type-55 frame after the settle window
(ESTABLISHING, REKEYING and CLOSED sessions get nothing); a burst of
edits coalesces into one frame; the deadline is millis()-wrap safe; a change
made while ESP-NOW is down is kept; a failed send is not retried; rejected
values do not arm a push; and the request/response path (forced RESP,
debounced non-forced RESP) is unchanged. To a peer reached through a relay
(146 bytes a frame) the 202-byte METADATA goes through `v4_send_payload_smart`,
which splits it, never as one sealed frame the relay would refuse (P10b).
`--component <hardwareone dir>` and
`--source-dir <overlay>` replay other production sources; a tree without the
push tick compiles against a no-op tick and fails on behavior. ASan/UBSan under
`HW1_SANITIZE=ON`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^espnow_metadata_push$' --output-on-failure
```

This check does not exchange real ESP-NOW frames.

## ESP-NOW history readers

History records carry no name and references are (slot, direction, seq, offset
hint), so the readers name rows with `espnowHistoryDisplayName` and resolve
refs only through `espnowCopyCollapsedHead`. `espnow_history_names_tests`
compiles `System_ESPNow_HistoryNames.h` (the per-call name cache the readers
share) as is: one resolve per peer and kind, `clear()` between calls, round-robin
replacement past eight keys with the right name every time, a resolver that
writes nothing or every byte, and the null-MAC key. `espnow_history_readers`
(`test_espnow_history_readers.py`, harness `espnow_history_readers_harness.cpp`)
extracts the production `handleEspNowMessages` with its JSON helpers
(`WebPage_ESPNow.cpp`), `oledEspNowRefreshMessages` (`OLED_ESPNow.cpp`) and
`chatWho` / `chatLogicalLine` / `chatEstBytes` (`G2_Page_ESPNow.cpp`), plus the
history types and the reader API declarations of `System_ESPNow.h`, and runs
them against a fake store and resolver, built with and without `CONFIG_SPIRAM`.
The web body is parsed in Python: every message has exactly the keys `seq reqId
piece of mac name msg enc ts type sent sendState` in that order; `name` is the
peer's current name, the MAC text for an unattributed record and `""` for a
sent one; the `?mac=` path, a rename between polls, 50/15 rows per poll and
the `deliveries` array. The OLED asks for `ESPNOW_HISTORY_VIEW_MAX` refs, shows
the newest 32, skips refs the store refuses (evicted), labels parts as
`name (k/n)` and keeps the sent rows' msgId and durable `sendState`. The G2 rows
are labelled `me:` / `<name>:`, reassemble by the record's own peer MAC and
estimate their size from the shown name. Source contracts: both G2 views pass
`ESPNOW_HISTORY_VIEW_MAX`, a G2 page build clears its cache, and no reader
names `MESSAGES_PER_DEVICE`, `senderName`, `senderMac`, the history blocks or a
ref offset. `--source-dir <dir>` overlays production sources. ASan/UBSan under
`HW1_SANITIZE=ON`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^espnow_history_(names_tests|readers)$' --output-on-failure
```

## ESP-NOW history display name

`espnow_history_display_name` (`test_espnow_history_display_name.py`, harness
`espnow_history_display_name_harness.cpp`) extracts the production
`espnowHistoryDisplayName` (`System_ESPNow.cpp`) with `ReceivedTextMessage`
from `System_ESPNow.h` and runs it against doubles of `gMeshPeerMeta`,
`gEspNow->devices` and `GroupLockGuard`: the active metadata name first, then
the registry name, else the MAC text (an empty or inactive metadata slot falls
through; a rename shows at once); `""` for a sent record and the MAC text for an
unattributed one, neither taking the lock; the lock taken once and released
otherwise; 32-byte names with no NUL read bounded; cap 0 and a null `out` write
nothing, caps 1-3 truncate and terminate; null tables and ESP-NOW closed; and
`gMeshPeerSlots` / `deviceCount` past their arrays clamped (ASan reports a read
past either allocation). It also runs `espnowHistoryPageNames`, which names an
`espnowmessages` page: one lookup per distinct attributed peer, a name reused
only between attributed received rows of the same peer, sent and unattributed
rows never lending or borrowing one. Source contracts: the doubles' name and
array sizes match the header, the resolver takes `GroupLockGuard` and uses no
`String`, and `cmd_espnow_messages` names its page through
`espnowHistoryPageNames`.
ASan/UBSan under `HW1_SANITIZE=ON`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^espnow_history_display_name$' --output-on-failure
```

## OLED ESP-NOW unpair and discovery ownership

`oled_espnow_ownership` executes the production OLED "Unpair Device" adapter
(`oledEspNowUnpairDevice`, with the file-static `oledEspNowRunOwnerCommand`
helper) and the `ESPNOW_VIEW_PAIRING` arm of `oledEspNowHandleInput` from
`OLED_ESPNow.cpp`, with the real view/state types from `OLED_ESPNow.h` and
`OLED_Display.h` and the command executor, toast and ESP-NOW owner as recorded
doubles. It checks that the OLED submits `espnowunpair <MAC>` and
`espnowpairmode 120|off` under the display identity, never calls the owner's
peer-delete, registry-remove or pair-window functions itself, shows a short
refusal toast ("Admin required" for the admin gate, "Unpair failed" /
"Discovery failed" otherwise) without leaving the unpair view, treats the
owner's save-warning reply as success, still leaves the discovery screen on B
when the close is refused, and keeps the no-passphrase no-op, candidate
navigation and the pair request unchanged. Static guards then require the
helper and the absence of direct owner calls in `OLED_ESPNow.cpp`.
`--component <dir>` points it at another copy of the sources; ASan/UBSan
follow `HW1_SANITIZE`. Owner behavior (registry save, crypto teardown, events,
authorization) is not exercised here.

## OLED bonded-peer settings editor

```sh
python3 components/hardwareone/test/host/test_oled_remote_settings.py --sanitize
```

`oled_remote_settings` compiles the production OLED remote-settings module (`OLED_RemoteSettings.cpp` whole: schema-driven module builder, non-blocking schema fetch, remote apply and peer-reply outcome), the settings editor's module source and commit routing (`OLED_SettingsEditor.cpp`), the remote-settings mode adapter (`OLED_Mode_RemoteSettings.cpp`), the ESP-NOW command-result record (`System_ESPNow`) and BondedPeer's schema sync and remote-edit policy, against doubles for the OLED command executor, the peer caches, the clock, PSRAM allocation and the display. It checks that the remote screen lists only the peer's schema rows that carry a setter (no read-only, secret or float rows, no `users`/`bond`/`wifi`, only `ntpServer`/`tzOffsetMinutes` from `clock`) with the cached values, sends `remote:<cmdKey> <value>`, reports the peer's CMD_RESP failure (`Peer: ...`) or silence (`No reply from peer` after 5 s) instead of the local "sent" result, ignores another device's reply, never runs a local setter from the remote screen, fetches a missing schema once without blocking (6 s budget), does not let the local screen keep editing a peer module, and releases every PSRAM block and String it allocated, also when an allocation fails. A static check confirms the CMD_RESP drain publishes the redacted result. `--component <dir>` runs the same cases against saved sources; the f4cc542 sources fail them (modules built from settings.json without setters, `remote:set <jsonKey>` reported as applied, the local setter run from the remote screen). The radio link and the real display are not exercised. System_Utils' `remote:` reply owner (`formatRemoteCommandSent` / `parseRemoteCommandSentReqId`) is spliced in and the executor double answers with it: "Remote command sent: <redacted command> (reqId <msgId>)", the echo shortened before the id (since 2026-11-18 a
command still waiting for the bond session reads "Remote command queued until the bond session opens: ... (reqId
<msgId>)" and parses the same way). The commit waits for the CMD_RESP whose `reqId` is that msgId from the bonded peer, so the peer's reply to another `remote:` command (the web console's) does not decide it; a "sent" reply without an id is refused rather than left waiting; the parser accepts only the owner's shape and a non-zero 32-bit id (A13). The pre-fix tree fails these three cases. Since 2026-11-18 it also checks that the local commit sends an emptied master or backup fingerprint field as `<cmd> clear` (the bare command only shows the value, which the editor took for a save) and every other value or setting as typed.

## BLE output privacy and console mirror

`stt_ble_privacy` compiles the production BLE output seams from `BLE_Server.cpp` (the private-STT classifier, the trace/history redaction, the directed plaintext and secure per-session senders, the async command-result callback, and the console-mirror chokepoint `sendBleResponse` with its recipient collector `bleCollectMirrorSessions`) against a four-slot fake GATT peer table. A directed send lands only in the addressed slot; the characteristic-wide notify lands in every connected slot, as the vendored Arduino wrapper sends it. The suite checks that private STT results never reach the trace or history mirrors, that command results reach exactly the originating session, and that the console mirror (`outble`, `blesend`) reaches only logged-in non-guest sessions. A connected peer that never logged in, a peer holding a Secure Channel without a login, a guest session, a logged-out slot and a disconnected slot receive nothing. No characteristic-wide notify is used, each recipient gets its own plaintext or encrypted copy, a congested recipient misses one flush without any recipient receiving a chunk twice, and secure-required mode still refuses plaintext. `--component <dir>` points the suite at another copy of the sources (the pre-fix tree fails the unauthenticated-peer, channel-only-peer and guest cases); `--cxx` selects the compiler; ASan/UBSan follow `--sanitize` / `HW1_SANITIZE`.

```sh
python3 components/hardwareone/test/host/test_stt_ble_privacy.py --sanitize
```

## BLE Secure Channel directed delivery

`ble_sc_directed` compiles the production `BLE_SecureChannel.cpp` whole against harness-local libsodium, mbedTLS, FreeRTOS, settings and Arduino stand-ins (`ble_sc_stubs/`). It adds four pieces extracted from `BLE_Server.cpp`: the directed raw sender `bleRawNotifyToSession`, the session sender `sendBleResponseToSession`, the console-mirror entry `sendBleResponse` with its recipient collector, and the deferred inbound job `bleScDeferredInbound`. A two-peer fake GATT keeps one inbox per connection. A directed send lands only in the addressed connection; the characteristic-wide notify that pre-fix sources use lands in every connected one. On the G2 client host (the console on the phone pipe's link) it also splices `bleSecureChannelOnly` and the `BleConsoleTxGate` around the raw sender: every frame is one try of the shared central TX gate taken before the lifecycle guard, a reliable send skips the accessories' soft priority while a best-effort one yields to it, a busy gate gives the bounded six tries with nothing sent, each reliable frame counts as pipe activity (best-effort mirror and card frames do not), plaintext is refused although `blesecure` is off, and server mode never touches the gate.

The suite checks that HELLO_ACK, CONFIRM_ACK, both REJECT reasons and every DATA fragment (command results and `fileread ... bin` chunks) reach only the connection and session epoch they belong to, admitted under the BLE lifecycle guard (on the G2 client host the session is checked under it and the post made after it is released, counted in flight). It also checks these cases:
- A stale session epoch sends nothing.
- A session rotated mid-message stops the transfer at the next fragment without resetting the channel.
- When the server stops during a handshake (every epoch invalidated), no reply reaches the radio.
- The console mirror sends each recipient one copy, directed or over its own channel.

Crypto is a routing-neutral stand-in, so keys, radio timing and scheduling are not tested here. `--component <dir>` points the suite at another copy of the sources; the pre-fix tree fails every cross-connection case. `--cxx` selects the compiler; ASan/UBSan follow `--sanitize` / `HW1_SANITIZE`.

```sh
python3 components/hardwareone/test/host/test_ble_sc_directed.py --sanitize
```

## BLE Secure Channel policy

`ble_sc_policy` compiles the production `BLE_SecureChannel.cpp` whole against the `ble_sc_stubs/` stand-ins it shares with `ble_sc_directed`. It splices the BLE command ingress from `BLE_Server.cpp`: the GATT write entry `processIncomingBleCommand`, the deferred secure-frame and plaintext jobs (`bleScDeferredInbound`, `blePlaintextDeferredInbound`), the command tail `processBleCommandLine` with its file-browser gate, and the directed and session senders. The fake GATT keeps one inbox per connection. A write runs the GATT entry as BTC_TASK; deferred jobs queue in submission order and run as cmd_exec after it returns, and the executor records which command would run for which session. On the G2 client host (R4) a plaintext write, login included, gets the fixed refusal on its own link although `blesecure` is off, and the same link's encrypted commands run. The FreeRTOS doubles count every tx-mutex take that may wait and every delay made on BTC_TASK.

The suite checks:
- With the secure channel required, a plaintext write from a connection without a channel gets the fixed refusal on that connection only, in plaintext. A connection with a channel gets the refusal as one encrypted DATA frame, and its encrypted commands still run.
- With no passphrase, plaintext commands run.
- On the G2 client host, a DATA frame on a console session renewed under its live link (no channel while the app still holds one) gets one `SC_REJECT` reason `0x03` on that session, a second frame gets nothing, and after a new handshake the app's commands run; a replayed frame on a live channel, server mode and a host without `blesecret` (only the no-passphrase reject) never get it.
- With encryption optional, file-browser verbs (`fileread`, `filewrite`, ...) written in plaintext are refused with `secure_channel_required`, even on a connection that holds a channel. `imagedelete` is a file verb too (F6): refused in plaintext with or without a channel, run when it arrived encrypted; `imagedeleted` is not a file verb. The same verbs inside DATA frames run. Non-file plaintext commands still run, and a verb that only shares a prefix (`filesystem`) is not treated as a file verb.
- No reply to a plaintext write waits on the secure-channel tx mutex on BTC_TASK (which serves the GATT/GAP callbacks of every link while a paced reply holds that mutex): the replies the command tail sends itself (`whoami`, login usage, `logout`, auth-required) and the required-mode refusal reach a channel holder as encrypted DATA frames from cmd_exec, and with the command queue full the writer gets one best-effort `Error: Failed to queue command` and the pool slot is returned.
- A plaintext write's pool slot (the line can be `login <user> <password>`, and the production pool prefers PSRAM) holds no byte of the line once it is released: after the command ran, after a full command queue, after the session changed before cmd_exec ran the job, and after a required-mode refusal; a shorter later write leaves no password tail.
- A source check of the command registry: `blerequireauth`, `blesecret` and `blesecure` are all superadmin-only.

Crypto is a routing-neutral stand-in, so keys and radio timing are not tested here. `--component <dir>` points the suite at another copy of the sources; the pre-fix tree fails the refusal, file-verb, tier and BTC_TASK cases (on the merged tree before H1, the F6 imagedelete refusals). `--cxx` selects the compiler; ASan/UBSan follow `--sanitize` / `HW1_SANITIZE`. `command_ingress_guards` holds the matching source contract: every notice the GATT write entry sends itself passes `blocking=false`, and the entry never calls the command tail.

```sh
python3 components/hardwareone/test/host/test_ble_sc_policy.py --sanitize
```

## Bluetooth server stop

`ble_server_stop` extracts the production `deinitBluetooth()`, its session
and advertising helpers, the role-claim and lifecycle-guard classes, and the
`closeconsole` and `blemode` commands unchanged from `BLE_Server.cpp`. Host
boundaries stand in for the role transition, the Ring owner
(`g2RingPrepareForStackTeardown`), the host/controller teardown, the shared
client stack (`bleStackReady()`), transport sessions and settings writes. A
stop deferred by a busy role transition or by a Ring owner that has not
quiesced, and a host teardown that fails, must reach the caller: `closeconsole`
answers `Error: ...; mode unchanged` and leaves `bleMode` alone, and a deferred
stop also leaves the app session epochs live and advertising on (the Ring
prepare runs before session admission closes). The exception is a companion
reset: under its outer RECOVERING claim the caller retries the stop a bounded
number of times and, if it never completes, lets the companion reboot under a
host it does not force down (see `companion_ble_shutdown`), so no link
survives; a Ring-deferred stop therefore closes the session epochs, clears the
GATT interface id and stops advertising before it reports failure; a nested
stop under any other claim keeps them. A retry after the Ring owner quiesces
stops the server. A clean stop keeps its reply (`App console server stopped
(Bluetooth host off; 'openble' brings the client stack back)`), its role reset
to the build's default, its freed state and the order Ring prepare, session
close, advertising stop, host teardown. `closeconsole` with no server running,
alone or beside the glasses and the pipe, answers `App console server not
running` and changes nothing (no settings write, no pipe stop, no host call).
`blemode` is the saved boot role only (`client|server`): it claims no role and
stops nothing, so it succeeds beside a running server and while another role
transition is active. The query speaks the link-status words
(`BLE_LinkStatusCore.h`, through the shipped `bleLinkStatus()` over host role,
pipe and console inputs: `Saved mode: client (Glasses+pipe); running:
Glasses+pipe`, also for a pipe alone, a console, and nothing running); each
set reply names the stored role, what runs now and the verb that starts the
stored role (`[BLE] Boot role set to client (Glasses+pipe); running now: app
console server; 'closeconsole', then 'openble' starts it`, `... running now:
nothing; 'openble' starts it now`, `[BLE] Boot role set to server (App
console); running now: client stack; 'openconsole' switches to it now`);
`glasses` (any case), `g2` and `1` are `client`, `0` is `server`; the removed
`phone-client` mode, its `phonelink` alias and `2`, the old `phone` alias (it
meant server), `phone-only`, near misses (`phone-link`, `glass`, `phonelinks`)
and any unknown mode answer `Usage: blemode [client|server]` before any role
is touched; the registry row carries the two-role help (`client = the shared
stack for glasses, ring and phone (default); server = the app console server
(legacy)`) and a usage naming `openg2` / `openring` / `openphone` /
`openconsole`; a build with no client device compiled in refuses `blemode
client` (`Error: [BLE] no client device is compiled in`) without a settings
write. The suite builds with and without the G2 client and the phone pipe. A
second translation unit compiles the real `BLE_Server.h` with
`ENABLE_BLUETOOTH=0` and checks that the stub returns `bool` true.
`--component <dir>` runs the suite against another copy of `BLE_Server.cpp/.h`.
`HW1_SANITIZE` enables ASan/UBSan. Real Ring/host timing still needs a device.

The same suite runs the app console's registration on the client stack's host
(`bleConsoleRegisterOnClientHost()`, which `bleStackUp()` calls in pipe builds), the shared state and command-service
builders, the GATT registration tap, `ServerCallbacks` and the two host-teardown
hooks unchanged, over recording Arduino BLE objects. Registration builds the
command service alone, with today's three characteristics, and never touches
advertising, the MTU or TX power; the console state stays uninitialized, so
`isBleServerInitialized()` still means server mode. It checks the GATT
registration status, also when the tap only sees the event after
`createServer()` returns, and a refused or missing registration leaves nothing
behind. It refuses before creating anything outside a role claim, under the
lifecycle lock, on a host that is not ready, with any central link held, with
the G2 runtime already up, with no free GATT app id, a second time on the same
host and while server mode owns the host. The server app registers an id from
the reserved 15-bit client allocator, also once the raw counter has passed
`ESP_APP_ID_MAX` (where Arduino would wait forever), skipping ids clients
hold. The request characteristic is write-only. On the client host no link
Bluedroid reports (temples, ring, phone) gets a slot, count, event or
advertising call; once the phone pipe is READY the shipped reconcile makes
exactly its link's slot, which counts only after the app's first write joins
it, a temple or ring drop leaves and the phone link's drop ends (its stale
record gets no slot back); server mode still admits and ends its apps.
The harness HAL stop calls the hooks the way `HAL_Bluetooth.cpp` does
(`hal_bluetooth` checks the real one): when the host stops, admission, the
session epochs, the characteristic pointers, the interface id and the tap are
already closed and the phone-link slot ended, the attach state starts the next
host empty; the state is freed only after a
terminal stop and kept, closed, after a failed one; the server role's state is
left to `deinitBluetooth()`, which in turn leaves the client host alone. Source
checks keep `createService(BLE_COMMAND_SERVICE_UUID)` in its one builder,
`initBluetooth()` on the shared helpers and the client-host path free of
advertising, MTU and TX-power calls, with every precondition and the
registration check ahead of what they guard. Android joining the link and
Bluedroid's Service Changed behaviour still need the hardware.

## Shared Bluetooth client stack

`ble_central_stack` splices the whole `#if ENABLE_BLUETOOTH` region of
`BLE_CentralStack.cpp` (nested blocks kept, includes blanked, `#line` directives
so compiler errors name the production file), the real `HAL_Bluetooth.h` and
`BLE_CentralStack.h`, the role-transition enum, the G2 MTU preference and
Arduino's `constrain()`. Every boundary call is a fake that appends to one
ordered event vector, the role claim included; after every up/down the harness
checks that the claim is released, that no boundary call happens outside it,
that no teardown step sees `bleStackReady()` or a scan pointer, that peers are
registered only after ready is published, that the refusal is never null and
that there is no scan without ready. Four lanes: full; no pipe; glasses without
the ring; phone only without coex. Scenarios: the HAL fixtures under the
production `ready()` / `stopped()` / `hostAbsent()`; up from an absent host (no
handshake, no deinit; a stopped host acknowledges a pending retirement first; a
Hosted absent-but-not-stopped host, the bench P4's first boot, gets no teardown
and no early acknowledgement and succeeds); up twice; the refusals in order
(disabled, fault, companion with its reason, server) with no HAL call, and a
fault under an outer RECOVERING claim allowed; the stale-generation normalize
(a ring connect in flight refuses; otherwise ready and the scan are withdrawn
before the first teardown call, the teardown runs with one phone handshake,
then the bring-up; each teardown failure's fault or no-fault outcome); the
`blestack` memory refusal; the rollbacks (hal-init fails, the rollback fails,
the setup worker fails, no scan object); TX power clamps and a failed set;
down (the refusals peeled server, glasses, ring, phone with ready kept; from
absent, native and Hosted; from ready with ready cleared before the teardown,
the phone worker released after the host stopped and before the setup worker,
a busy phone or setup worker non-fatal; a HAL failure faults and releases
nothing; a ring refusal keeps the host up and the next up normalizes it; a
verified down clears an earlier fault); a second down makes no HAL call;
`bleStackNoteHostLost()`; a busy role claim. Text checks pin
`registerDevicePeers()` before `bleArmAutoReconnectPeers()`, the four
open-device refusals ahead of any effect in `bleStackDown`, `sReady.store(false`
before `hostTeardown()`, the phone worker release after the teardown and before
the setup worker release, `phonePipeHostStopped(` only in `hostTeardown`, the
rollback and the absent branch of `bleStackDown` (never in `bleStackUp`), and
no `xTaskCreate` / `EXT_RAM` in the file. `--component <dir>` and `--sanitize`
as the neighbouring suites. The device owners it drives are covered by
`g2_client_lifecycle` (glasses), the ring suites and the phone suites; the
verbs over them by `ble_server_stop`, `ble_info_status`, `ble_recycle_phone`,
`phone_recovery`, `ble_role_switch_reconnect` and `companion_ble_shutdown`.

## Bluetooth commands and status

`ble_info_status` (see "Bluetooth status coverage") also runs the production `blesend`, `blestream` and `bleevent` handlers with the real `CommandArgs` parser and stream-flag helpers, and runs the data-service push functions against a fake directed GATT sender. Every keyword and the interval pair come from the dispatcher's arguments. `blestream sensors off` clears only that stream. An unknown first word and intervals that are missing, not a number or not positive are usage errors; a toggle that is not an on/off word is refused with the shared on/off reply (`kCommandReplyNotOnOff`) and changes nothing. `blesend` forwards the whole message. Sensor, system and event frames reach only the console-mirror recipients, by directed notify and never characteristic-wide. The checks also cover stale sessions, congestion, a missing GATT interface, a refused lifecycle lock and secure-required mode. On the G2 client host, which serves no data service, `blestream` and `bleevent` refuse with the phone-link reason before touching a flag or a recipient. `--component <dir>` points the suite at another copy of the BLE sources; the pre-fix tree fails at the `blestream on` case.

It also runs the production `bledisconnect` command and its owner functions (`bleDisconnectConnection`, `bleDisconnectAppClients`) against a recording GATT-server double. `bledisconnect <connId>` closes exactly that app client. The bare form, which the page-level button and the OLED and G2 items use, closes every app client and never a paired R1 ring or G2 temple link. Unknown ids, peer links and malformed ids (`abc`, `-1`, `1x`, `0x1`, `70000`) are refused without a close. The close is issued with the lifecycle guard released, and a missing BLE state, server or lifecycle lock closes nothing. A Node-free check reads `WebPage_Bluetooth.h`: each client's Kick button must pass that client's `connId` to `bledisconnect`. `--component <dir>` also points these reads at another tree; the pre-fix tree fails at the first per-id case.

`ble_info_status` also runs the production `blestatus` command against the same connection table: a logged-in user named "Unknown", a logged-in user and an anonymous peer. It renders the guarded snapshot that `bleinfo json` uses. The checks: the JSON views list the same client set (connId, authed, MAC, age, commands, slot index, with the display user "Unknown" only for a peer that has not logged in); each call takes the lifecycle guard once with the 100 ms budget (the console view only tries it, without waiting) and reads connection age inside it; a busy guard gives an explicit unavailable reply; a change published as the guard is released does not appear; the text form starts with the link lines (`Role: App console`, `App console: app signed in`, `  Clients: 3/4, advertising off`, and the `Phone link:` block in a pipe build) and keeps the client lines; outside the app console both forms answer with the role (`Role: Glasses`, `Role: Glasses+pipe`, `Role: Phone pipe (saved: App console)`, `Role: Off (saved: App console)`), `App console: not available in this mode` or `off`, the phone link block (`Phone link: ready`, `fail: not found`, ...) and how to reach the console (a `Hint:` line, or `hint` in JSON: with only the glasses that `openble` switches to server mode and what it stops; with the pipe running that the console stream needs a device code from `phone pair new`, or `bleconsole on`, or that it opens when the app asks for it), with a `link` JSON object, a `phoneLink` block and no address; in phone-client mode the console stream's row is the console (opening, signed in with five sessions ended and their reasons named in text and JSON, then open and signed out after the idle logout), and `bleinfo` carries the same block; on the G2 client host the console on the phone pipe's link reads `set blesecret first`, `waiting for the phone link` (also once the pipe left READY, when a slot it left is not counted), `app signed in` with its one slot in `consoleClients` and `off`, never with a `Clients:` line or advertising, and its hint says what attaches it (`blesecret`, `phone status`, `bleconsole on`; none once attached), never `openble`; the console view reports that host registered, attachable and attached without claiming the server role; a saved mode that differs, a waiting console and a ring link that is no app are named; four 64-character usernames give complete JSON; and every JSON allocation failure gives the oom reply.

## BLE area cleanup coverage

`ble_area_cleanup` runs the production connection typing from BLE_Server.cpp (`bleIdentifyDeviceByMac`, its MAC helpers and `bleDeviceTypeToString`, extracted unchanged with BLE_Types.h and the saved-target types from BLE_Peers.h) against a host table standing in for the peer owner's saved targets. Saved glasses, ring and phone addresses classify in any letter case, an unsaved or all-zero address is Unknown, and after a re-pair the new addresses classify as the peer and the old ones as Unknown on the next lookup (`bledisconnect` keeps a link open only when it is typed as a G2 temple or the ring). Source contracts on the same files: every characteristic BLE_Server.cpp creates with PROPERTY_WRITE has write callbacks; the uncalled Bluetooth helpers and the MAC cache are absent from BLE_Server.cpp/.h; G2_Ring.cpp carries none of the unregistered ring-to-glasses spoof push or temple bridge (commands, tasks, state) including the no-op `g2RingNoteBridgePoll` hook, and once that code is gone G2_Ring.cpp names the commit that still holds it (f4cc542); and BLE_Server.h compiles with ENABLE_BLUETOOTH=0. `--component <dir>` runs it against another copy of BLE_Server.cpp/.h and G2_Ring.cpp/.h. It does not exercise the GATT server or a real connection.

## Ring forwarded telemetry parser

`test_ring_forwarded_telemetry.py` compiles the production
`g2RingNoteForwardedTelemetry` and its local protobuf helpers (`G2_Ring.cpp`)
with the shared `g2Pb*` readers (`G2_Protocol.cpp`), the production
`R1TelemetryCache` layout and the `G2HealthMetric` enum. The telemetry mutex,
the direct-link ownership check, the Health live series and `millis()` are
doubles. Each case runs in its own process with the frame in an exact-size heap
buffer and a 5 s alarm, so a parser that never returns, or one that reads past
the frame (under `--sanitize`), fails by name. A well-formed frame with unknown
fields of every wire type caches all six metrics and their Health samples; a
field ending exactly at the frame end is accepted; truncated outer and nested
fields are rejected. Ten-byte field lengths near 2^64 (an unknown field, field 4
itself, field 4 of length 2^64-1 followed by eight data bytes, and a nested
field inside a well-bounded field 4) must be rejected without moving the parse
position backwards, reading past the frame or touching the cache.
`--component <dir>` and `--source-dir <dir>` read production sources from
another tree.

## Ring daily packetAck and the Health history model

```sh
python3 components/hardwareone/test/host/test_ring_daily_ack.py --sanitize
```

`ring_daily_ack` compiles the production ring RX path (`ringProcessRxFrame`, `ringExtractTelemetryCache`, `ringReasmFinalize`, the RX duplicate filter and the packetAck lane from G2_Ring.cpp) with the production Health pending-session model (`g2HealthApply*Daily`, `g2HealthHistoryFetchStarted/Finished`, the day-key and peer checks from G2_Health.cpp) and the production profile capabilities and packetAck descriptor factories (R1_Protocol.cpp). The frame decoder and typed daily parsers are doubles that return the test's page. On 2.2.9 a daily page gets a packetAck only when the model accepted it into the active sweep: a page outside a sweep, in a sweep dropped behind an uncommitted result or a failing commit, with another day key, with a zero-base day or during a pending peer switch gets none, while it still refreshes the live cache; an accepted page gets exactly one ACK, and its retransmit is ACKed again without a second ingestion. 2.2.7 keeps its established flow control (every eligible daily NOTIFY, sleep and reassembled activity included, is ACKed) until a device check. `--component` / `--source-dir` read the production sources from another tree; the 0.99.97.4 sources fail the seven refused-page checks. No BLE link, ring firmware, owner-loop scheduling or store commit is simulated.

## Ring connect bound

```sh
python3 components/hardwareone/test/host/test_ring_connect_bound.py --sanitize
```

`ring_connect_bound` compiles the production `RING_DIRECT_CONNECT_TIMEOUT_MS` (25 s), its `static_assert` against `CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT` and `ringSettleClientAfterFailedConnect` from G2_Ring.cpp, and runs the settle over fake BLEClient outcomes: an open-timeout whose GATT app unregistered cleanly gives the link slot back and keeps the client, every other failure goes through the shared quarantine. It checks that the `static_assert` refuses a Bluedroid timer of 25 s and of 20 s, and that every checked-in sdkconfig keeps the timer above 25 s. Source checks pin what the unit cannot run: both ring connect calls (saved MAC and scanned advert) use the constant; the failed-link branch touches no glasses state; the ring asks for no connection priority (only GlassesPriorityGuard lowers the glasses to BALANCED); and the faceclaw backoff (2 s doubling to 60 s from each failure, reset by a finished connect) sits in the admission after the yield check and before `g2RingInit` and the gate, with the manual paths never held. `ring_phone_yield_queue_harness` runs that backoff through the real admission on a virtual clock (`test_ring_phone_yield.py`). No BLE link or ring firmware is simulated.

## R1 ring health controls and history

`health_logging_ui` reads the R1 Health action body with the shared bounded reader (`webRecvRequestBody` from WebServer_Utils.cpp, compiled unchanged); the page keeps no reader of its own. Checks: one idle wait between chunks is retried; two consecutive idle waits (a half-open socket) end the request with the 400 "invalid action body" reply; a socket error is never retried; an empty body or one of 160 bytes or more is refused before reading. `--component <component-directory>` reads production sources from another checkout and `--source-dir <directory>` overlays single files; the 0.99.97.4 sources fail the half-open case because that reader retried timeouts without bound.

The ring controls run the production `g2RingCommands` and `healthCommands` rows, `cmd_ringcontrol`, the `ring history` / `ring history force` / `ring control refresh` handlers and the `ring` root (`ring history force` is its own admin registry row, chosen by longest-prefix lookup over the non-admin `ring history`) through a registry executor double (longest-prefix lookup, the registry admin gate, an audit record per executed command). A non-admin is refused with the same text from the web handler (403), the OLED page ([DENIED]) and the serial CLI, nothing reaches the ring setters, and the validate pass is refused at the same admin row; an admin's change from each transport reaches the owner and is audited as `user@web`, `user@display` or `user@serial`. The 0.99.97.4 sources fail because the web and OLED adapters checked admin themselves and called the setters, and no command existed for collection or low power.

The forced refresh is refused with the registry's text from web (403), OLED ([DENIED]) and serial. Nothing executes; each refusal is recorded as a denied command, and the validate pass is refused at the same row. A line the registry matches only as plain `ring history` (double space) is refused by the handler, and one matched only as the `ring` root gets usage; neither runs anything. `ring control refresh` and `ring history` run for a non-admin. The registry double names the matched row in its refusal (`ring control`), as authorizeCommand does. Only the registry's "Admin access required" text maps to 403; any other refusal is the command's own 409. The pre-fix sources fail: the forced refresh had no admin row and was refused inside its handler, and the page still carried its own reader and a second refusal prefix.

`r1_history_store` (see "R1 history persistence") also compiles the production Health history model that drives the store (the pending-sweep load, merge and commit path of G2_Health.cpp; trend views, the graph push and the profile capability table are doubles). A stored day that exists but cannot be opened loads as `READ_FAILED`, never `NOT_FOUND`, and the retried load merges the sweep into it instead of replacing it; a backup an interrupted publish left alone is promoted and loaded; a corrupt stored day is retried a bounded number of times, is never replaced, and does not stop later sweeps from being accepted and stored. `--component <components/hardwareone>` runs these current expectations against another tree's production sources (the f4cc542 sources fail the twelve load-failure checks); `--baseline` keeps its store-only scope.

## G2 Network background redraws

`g2_network_async_redraw` compiles the Network page's state, renderers, WiFi "Pending..." and R1
"connecting..." watchdogs, WiFi scan worker and WiFi/R1 tap handlers with the production redraw
helper, lens job types and the lens worker's Redraw arm. A fake clock drives connects, expiry,
in-flight ring jobs and scans; each case runs in its own process. It checks that a held note
(Notes), another Network view or leaving during the scan settle window is never covered by a
Network list and that these tasks never draw themselves, while the menu still repaints when the
user stays on it or comes back to it while pending, and scan results still reach the lens through
the Even-AI card, placeholder, backed-up queue and inline-fallback paths. `--component` runs it
against another tree. ASan/UBSan follow `HW1_SANITIZE`; radio, ring, command, display and RTOS
calls are host boundaries, not device scheduling. The list-page double models page-swap
admission: a second list render inside one lens-applier job is refused, as on the device.
The Phone menu and its tap handler are compiled too, over the real bleLinkStatus() with the
console server down: a `phone connect` reply repaints the Phone menu while the user stays on it
and never over a held note.

## G2 Files page tasks and redraws

```sh
python3 components/hardwareone/test/host/test_g2_files_psram.py --sanitize
```

Besides the reader/viewer harness, `g2_files_psram` compiles the rest of the production Files page
(`G2_Page_Files.cpp`, includes stripped, the reader/viewer stubbed) in `g2_files_redraw_harness.cpp` and drives it from
simulated tasks: taps on `g2_taps`, queued redraws on the lens applier, the rename/delete completion on
`cmd_exec`, the info-overlay expiry on the main loop and the image viewers' `onDone` on the session worker. The
completion, the expiry and the viewer return must not draw, delay or touch FileManager; each queues one Redraw (the
completion with its submit cookie) that the applier drops after a menuGen bump. The result shows once in the list's
back row, and a successful rename/delete is re-scanned even when its redraw was dropped. Every FileManager call and
lens draw runs with the Files page lock held by the calling task; callbacks delivered in the middle of a tap's
directory read never re-enter FileManager; info rows stay read-only after the overlay expired until the list is shown;
a list shown by a tap cancels a queued list redraw; and a redraw that waited for the lock while a tap opened the
rename keyboard does not replace it. `--component <dir>` replays the suite against a pre-fix tree. FreeRTOS, lens,
command, VFS and FileManager are host doubles; ASan/UBSan follow `HW1_SANITIZE`.

The same harness checks the chooser's action paths. `FileEntry.name` is FileManager's display copy (63 bytes), so
View, Info's sealed-capture peek, Rename and Delete take the selected entry's full path from
`FileManager::getSelectedPath()`: a 70-byte SD name is deleted by its full path, never by its 63-byte prefix; a path
that cannot be resolved or does not fit the 160-byte action buffer (a 123-byte folder plus that name) is refused with
nothing submitted and no viewer started; and folder rows keep the closing "]" of their "[n]" / "[99+]" badge.

## G2 Sensors completion redraws

```sh
python3 components/hardwareone/test/host/test_g2_sensors_redraw.py --sanitize
```

The camera power post-hook (cam_power, after every camera power operation from any source) and the openmic/closemic
completion (cmd_exec) refresh the Sensors CAM/MIC detail. They never render or write the Sensors level or hijack
page on their own task: they queue a Redraw on the lens applier (the hook with the menu generation taken when it
fires, the mic completion with its submit-time cookie), and the render re-checks level DETAIL and page SENSORS.
`g2_sensors_redraw` compiles the whole production Sensors page (camera and microphone on, other sensors off) beside
a lens double with swap admission, a generation-checked Redraw queue and task-context tags, and runs a user tap
("Settings >", "<- Hardware") between a completion's check and its render: nothing renders on cam_power or
cmd_exec, Camera Settings keeps its rows and routing, the list keeps routing list taps, and an undisturbed detail is
still refreshed. It also checks that an out-of-range list tap does nothing and that the live view's level is
committed only after the live page started. The CAM toggle submits the camera owner's gated `opencamera async` /
`closecamera async` through `g2SubmitHijackCommand` and never powers the camera on the tap task. `cameraautostart`
is submitted only from the completion, after the owner admitted the change. A refusal (camera disabled, not
authorized, cam_power unavailable) persists nothing, clears the pending start and redraws through the lens applier. A
refused submission re-renders the unchanged detail on the tap task. The E3 cases now run the admitted command before
the post-hook. `--component <dir>` replays it against a pre-fix tree. Lens, camera,
audio, registry and command calls are host doubles; ASan/UBSan follow `HW1_SANITIZE`.

## G2 list pages under refused renders

```sh
python3 components/hardwareone/test/host/test_g2_page_publish.py --sanitize
```

`g2ShowListPage` can refuse a render (swap in flight, allocation failure, lost admission, full lens queue) and the
previous rows then stay on the lens. List pages build their rows plus a pending mode / row map and publish it through
`g2ShowListPageAndPublish` (G2_Page_Common.h) only after the lens accepted the rows. `g2_page_publish` compiles the
production Files chooser, delete confirm, info overlay and tap dispatcher; the Settings level/row-map state, row
builders, edit/commit machinery, menu entry and dispatcher; and the whole LED page, each beside a `g2ShowListPage`
double that can refuse. After a refusal the visible rows keep their meaning: chooser row 2 never submits
`filedelete`, a Settings entry row never commits a pick option or an entry from an unseen page, the LED root "Off"
row never sends a color, and a refused entry does not claim the hijack page. `g2_power_completion` (tap 4 refused,
then tap 1 never submits `deepsleep`) and `camera_consumers` (a refused Camera list leaves TOP's FPS row cycling
FPS) carry the same cases for Power and Camera Settings. All three accept `--component <dir>` to replay against a
pre-fix tree. Commands, lens, RTOS and settings-registry calls are host doubles; ASan/UBSan follow `HW1_SANITIZE`.

## Lens Peers rows

`g2_espnow_peer_rows` compiles the production `g2EspNowPeerCounts`, `g2FormatPeerCounts`,
`g2BuildNetworkInfo` and `showEspNowDevices` (G2_Page_Network.cpp), the ESP-NOW app's row table,
`g2BuildEspNowAppInfo`, `showMainMenu`, `showPeersMenu`, `showStatsMenu` and `handlePeersTap`
(G2_Page_ESPNow.cpp) and `espnowPeerState` / `espnowPeerCounts` (System_ESPNow.h) against doubles of the
registry, the heartbeat table, the session test and the lens list page: a peer is every registry entry except
this device; it is online with a recent heartbeat and an ACTIVE session, connecting with the heartbeat alone
(never counted online), offline otherwise; the rows print online/paired as "up" and the connecting peers after
it ("2/3 up +1 conn"), and the lens status line prints the same text; neither peer list has a row for this
device; a tap selects the peer its row named, also after the registry changed or a redraw was refused. Built
again with ENABLE_ESPNOW=0. Its source part checks that the lens RTC texts go through rtcLocalTime, that the OLED G2
status screen reads one g2ConnectionStatusSnapshot, that the OLED ESP-NOW editors take their limits from
devicename::kMaxLen / V4PayloadMetadata, and that the OLED ESP-NOW screens do not call esp_wifi_get_mac.
`--component` runs it against another tree.

    python3 components/hardwareone/test/host/test_g2_espnow_peer_rows.py --sanitize

## Peer state on the metric and MQTT

`espnow_peer_state_surfaces` compiles the production `espnowPeerState` / `espnowPeerCounts`, `readMesh`
(System_Metrics.cpp) and `meshPeerAvailability` (System_MQTT.cpp) against doubles of the registry, the
heartbeat table and the session table: the MESH metric is the online peers out of the paired ones (connecting,
a session without a heartbeat, this device and unpaired peers never count online); a peer's Home Assistant
availability is "online" for an online peer only. Its source part checks that MQTT asks neither table nor
MeshPeers, publishes availability before it skips a peer without a fresh reading, and writes `system.online`
and `system.state` from the state, and that MeshPeers has no `isHealthy` / `countHealthy` / `countActive`.

    python3 components/hardwareone/test/host/test_espnow_peer_state_surfaces.py --sanitize

## G2 Network row bindings

`wifi_saved_ui` (see "Saved WiFi network coverage") also has a unit that compiles the
G2 Network page's row bindings, its WiFi, ESP-NOW, HTTP(S), Bluetooth, G2, R1 and
Phone menus and their tap handlers, with the real bleLinkStatus() over fake role,
pipe, peer and console inputs: each case draws a menu, changes the radio, link,
server, BLE, ring, pipe or setting state behind its labels, taps a row and checks
that the command matches the label that was shown (and that a refused render after
a submenu change runs nothing), plus the unchanged-state row maps. The Bluetooth
menu's `Role:` (+ `Saved:`) rows use the link-status words; its server layout shows
`Console:` in hw1link::consoleText's words (`waiting`, `hidden`, `signed out`,
`signed in`) from the snapshot, never gBleState, so a stray "connected" with no app
client still reads `Console: hidden`. The Phone menu shows `Pipe:` (a failure names
its reason, `Pipe: fail: not found`; a fault reads `Pipe: fault`), `Paired:` and
`Console:` (the console on the pipe's link: `waiting`, `signed out`, `signed in`,
`no secret`). Each case runs in its own process; the ENABLE_ESPNOW,
ENABLE_HTTP_SERVER, ENABLE_WIFI and ENABLE_BLE_PHONE_PIPE off builds are
compile-checked. `--component` runs it against another tree.

## Automation summaries

`automation_scheduler` (see "Automation scheduler") also checks the owner's automation summaries (`automationCopySummaries` / `automationFindSummary`), one case per trigger type, built from `parseOneTrigger`: daily, weekly days, week interval, monthly, yearly, interval, manual delay, boot, event, unknown event, legacy or malformed triggers, extra-trigger counts, name fallback, cap and unreadable/malformed files. A second program compiles the whole G2 Automations page beside lens, command and file doubles: the lens shows the owner's trigger text, a tap opens the automation its row showed even after a web add/delete, Enable/Disable submits what the visible row says, and taps read nothing. `automation_scheduler_contract` also guards that the G2 and OLED pages never open or parse automations.json. Both accept `--component <dir>` to replay against a pre-fix tree.

## QuickNote interrupted saves and note tiers

`quicknote_store` runs the production `QuickNoteStore::sweep`: on both tiers it
removes only names that parse as the store's own temporary spelling inside
canonical `u<id>` notes folders, never opens a file for reading, and leaves
user-placed names, directories, transcripts and other folders alone; nothing is
promoted, because a note header carries no length. `quicknote_service` covers the
worker that runs the sweep once a boot and its idle exit: after 60 s with no
note running or starting, nothing due and no boot note waiting for the clock,
the worker leaves through the shared `ServiceWorker` (below), a start claims
its note before it brings a worker back, and the suite pins that order and
that the module keeps no lifecycle primitive of its own. `g2_notes_page` checks the lens tier
row: with a card in, the last row lists the other tier (`note list sd|internal`),
stale replies for the abandoned tier are dropped, and a new show returns to the
save tier. The f4cc542 sources fail the sweep case (no sweep) and the tier row.

## Idle service workers: the shared lifecycle

```sh
python3 components/hardwareone/test/host/test_service_worker.py --sanitize
```

`service_worker` splices `struct ServiceWorker` from `System_TaskUtils.h` over a
faked kernel. It is the one lifecycle the lazily created, idle-retiring service
tasks share (`qnote`, `dictation_svc`, `cm5_presence`): `ensure()` holds one
creator's claim at a time, makes the wake semaphore once and publishes it under
the module's spinlock before the task exists, then publishes the handle;
`give()` reaches the semaphore and never a task handle (a give before the first
task or between tasks waits for the next one); `wait()` passes the time through
and reports the kernel's answer; `retireIf()` runs the module's idle predicate
and nulls the handle in the same critical section, so a producer that publishes
work and then calls `ensure()` finds the task or makes one. The suite checks a
refused create leaves nothing published and is retried, that priority and core
are the caller's to override, and that every kernel call lands outside the
critical section. A source check pins the three users' declarations and that
none of them creates a task or a semaphore, or notifies a task, on its own.

## G2 page and hijack-bridge API contract

```sh
python3 components/hardwareone/test/host/test_g2_page_api_contract.py --sanitize
```

`g2_page_api_contract` keeps the G2 Files, Settings, Network and Sensors pages, the hijack command bridge and the hijack FSM free of entry points nothing calls. The pages are reached through their interactive list (`g2Show*Menu` / `g2*HandleTap`) and the `g2Build*Info` text the page registry renders; the text-dump wrappers `g2ShowFilesPage`, `g2ShowSettingsPage`, `g2ShowNetworkPage` and `g2ShowSensorList`, the no-op `g2FilesTick`, `g2CurrentCmdSeq` and `hijackEventName` must not exist, and `g2ShowSensorsMenu` keeps no copy of its rows (`gSensorsListCache` / `gSensorsListCount`). A header probe compiles `G2_Page_Files.h`, `G2_Page_Settings.h`, `G2_Page_Network.h`, `G2_Page_Sensors.h` and `G2_HijackFsm.h` with G2 enabled, with `ENABLE_G2_GLASSES=0` and with `ENABLE_BLUETOOTH=0` (the xiao_s3 board header plus a feature profile, as in `g2_disabled_header`): a probe namespace offers variadic fallbacks, and each removed name must resolve to one, so neither an enabled declaration nor a disabled inline stub may remain; the kept API keeps its signatures, and the disabled builds link and run its stubs (empty text, `Idle` state). A source contract scans every production `.cpp`/`.h` under the component (test/ excluded, comments and string literals stripped) for the nine names, and the owning files for any mention at all. `--component <dir>` replays the suite against another tree; the board header always comes from this repository.

## File browser classification and rename/delete authority

```sh
python3 components/hardwareone/test/host/test_file_kind.py --sanitize
```

`file_kind` compiles the shared classifier `System_FileKind.h` and, over one table of names, the production G2 Files page file-tap branch (the `else` block of `g2FilesHandleTap` with the classification it calls, `showFileChooser` recording the chooser kind) beside the OLED browser's "View" decision (its `sFbActionViewable` assignment). The lens must offer a text or JSON view exactly when the OLED offers View: `.log`, `.md`, `.ini`, `.yaml`, `.html`, `.c` and the other OLED text types open in both, BMP/JPEG open only on the lens, and png, bin, `.tar.gz` and names without an extension get Info/Rename/Delete only. A second harness runs the OLED browser's rename/delete helpers (`fbRenameSelected`, `fbDeleteSelected`, `fbSubmitMutate`) against FileManager and executor doubles: both submit the user-level `filerename` and one-shot `filedelete ... confirm` commands with the selected entry's full path (a 70-byte SD name), re-scan only after the executor's `OK`, report the executor's admin refusal, give the handlers' path-policy refusal ("File does not exist or access denied") its own result and the toast "Not found or denied" (through `fbToastMutate`, which the harness also compiles), toast the handler's reason for any other failed rename or delete (the caller's "Rename failed"/"Delete failed" only when the reply carries none), and submit nothing for an unresolved selection or a quote. Source contracts check that the OLED browser no longer calls `FileManager::renameItem`/`deleteItem`, that its prepare pass routes Rename/Delete through those helpers under the identity guard, that guests are still stopped on the input path, and that neither browser keeps a private extension list. `--component <dir>` reads the production sources from another tree; the f4cc542 sources fail the agreement checks (the lens gives `.log` and `.md` no viewer while the OLED offers View) and the contracts (direct FileManager rename/delete). No device, executor or FileManager is run.

## Shared JSON Pretty view

```sh
python3 components/hardwareone/test/host/test_json_pretty.py --sanitize
```

`json_pretty` compiles the shared re-indenter `System_JsonPretty.h` with production code from the OLED browser: its viewer state, `fbFillView` body builder, `fbBuildActionRows` and its `sFbActionJson` decision. Valid JSON (empty `{}`/`[]`, nesting, escaped quotes and backslashes, structural characters inside strings, UTF-8) must come out in ArduinoJson's `serializeJsonPretty` layout with `\n` line ends; re-indenting pretty, CRLF or tab-spaced input gives the same text, and splitting the input at every byte across two `write` calls changes nothing. Truncated and malformed input is laid out as far as it goes (stray closers never drive the indent negative, an unterminated string passes through), and 40 levels of nesting indent no deeper than `kMaxIndentLevels`. The OLED's Pretty View must equal the re-indented text wrapped at the OLED width and JSON View the stored text wrapped; a full line table and a full body both mark the view truncated. JSON files get Pretty View / JSON View / Rename / Delete / Back, text gets View, other files no view. Source contracts check that the web viewer's Pretty mode, the G2 lens Files page, the OLED browser and serial `fileview` (which picks JSON with the shared `fileKindForName`; its own cases run in `fileread_psram`) all include the header and format through `JsonPrettyFormatter`, that the web handler keeps no private indenter state, that neither on-device browser re-serializes through ArduinoJson, and that the OLED's Pretty View row reaches `fbFillView` with `pretty` set through `LOAD_PRETTY_VIEW`. `--component <dir>` reads the production sources from another tree; the c0384687 sources fail every contract. No display, BLE or HTTP transport is run.

## Display login policy ownership

`display_auth_policy` scans every production source for command handlers that write
`localDisplayRequireAuth`, `serialRequireAuth` or `uartRequireAuth`, then compiles the
production OLED and output settings rows, the OLED, Settings and settings-editor command
tables and every other table tied to a policy writer (today `userSystemCommands`) with
generated link fixtures (a `gSettings` built from the rows, one stub per handler, the
writer list). It checks that every row persisting a policy names a command that only
super-admins run, that every command running a writer is super-admin, that each policy
is persisted by exactly one row (one JSON key, so load order cannot decide it), that no
policy write sits outside a registered command handler or behind a generic editor
command, and that every OLED row names an existing command. `--component`/`--source-dir`
point it at another tree; ASan/UBSan follow `--sanitize`. Table metadata only:
`authorizeCommand`, the web UI and device behavior are not run.

`settings_mutation` also checks that no setting write runs a service command: a
boolean off is a plain save (value, persistence request, event), also for a row
that carries a removed `<x>enabled` name. `--component` points the suite at
another tree.

## OLED session role cache

`oled_session_role` compiles the shipping local-display role gate from
`OLED_Utils.cpp`:

- `oledIsGuestSession` with the session-role helpers beside it (everything between
  the `shouldBlockForDisplayAuth()` definition and `oledModeAllowedForGuest`);
- `oledModeAllowedForGuest`, `oledGuestBlocksMutate`;
- `getMenuAvailability`, with every `ENABLE_*` off;
- `oledApplyPendingSessionBoundary`.

It also compiles the launcher populate and the selected-row right-pane decorator
from `OLED_Mode_Menu.cpp`, with the shipping mode enum and menu/scroll structs.
Fakes count every users.json read (`getUserAuthorizationRole`, `isAdminUser`) and
supply the session authority, the owner task and the scroll primitives.

The checks:

- Launcher steps (input populate, render populate, Users-row pane, mutate guard,
  mode gate) read the roster at most once per session and identity generation.
- A failed lookup (banned, unknown or unreadable) is guest and non-admin, and is
  retried rather than kept.
- A `gIdentityGeneration` bump and a session boundary (login of another account,
  logout) re-resolve the role. The boundary's own menu request already sees the
  new identity, and no read happens while the display lifecycle writer is held.
- Other tasks (`cmd_oledmode`) read live and leave the cache alone.
- An unauthenticated launcher is unfiltered and reads nothing.

`--component`/`--source-dir` point it at another tree. ASan/UBSan follow
`--sanitize`. Device rendering, `authorizeCommand` and the real roster parser are
not run.

## OLED display commands from other tasks

```sh
python3 components/hardwareone/test/host/test_oled_owner_requests.py --sanitize
```

`oled_owner_requests` compiles the production `requestOledMode`, the owner-request queue, `cmd_oledmode`, `cmd_oledtext`, `cmd_oledclear`, `cmd_oledanim` and `oledUpdate` from OLED_Utils.cpp with `cmd_setpattern` (`setgamepadpassword`) from OLED_Mode_SetPattern.cpp. A task double makes `xTaskGetCurrentTaskHandle()` answer the main loop or cmd_exec, and fakes record which task touched the framebuffer, ran an onEnter hook, read the display role or auth mirror, pushed the back stack or auto-started input. Called as cmd_exec, every command and `requestOledMode` leave `customOledText`, the mode, the stack, the animation and the pattern flow unchanged until the owner runs `oledUpdate()`. The owner then applies them in post order with the same side effects as before: `oledmode menu` auto-starts input; `oledmode off` and `oledclear` blank the panel on the owner, and `oledclear` leaves the page clean; `setgamepadpassword` pushes the previous mode and restarts the flow; the auth gate still redirects to the login page. A command posted before a session boundary hands over its text or reset but does not navigate after that boundary. A command posted after a boundary the frame has not applied yet waits for it. A ninth request drops the oldest without leaking its text copy, and nothing allocates inside the queue lock. A source check requires `updateOledDisplay()` to drain the requests right after its first session-boundary apply. `--component` / `--source-dir` read production sources from another tree; the f4cc542 sources fail 26 checks (state written on cmd_exec) and the source check.

## OLED mode availability

```sh
python3 components/hardwareone/test/host/test_oled_mode_availability.py --sanitize
```

`oled_mode_availability` compiles these from the production tree: `getMenuAvailability`, with its `oledModeFeatureState` classification and `oledModeRuleAllows`; the mode registry (`registerOledMode`, `registerOledModes`, `findOledMode`) from OLED_Utils.cpp; and `OledModeEntry`, the callback typedefs and `MenuAvailability` from OLED_Display.h. It registers the real entry rules of CLI Input, Gamepad PW, Logout, GPS (i2csensor_pa1010d_oled.h) and thermal through the registry, with GPS and thermal compiled in. A module's refusal makes its launcher row FEATURE_DISABLED with the module's reason. Without a named display session, CLI Input and Gamepad PW show "Login required" and Logout shows "Not logged in". The central classification keeps its class and reason: GPS or thermal not detected is NOT_DETECTED, detected but stopped is FEATURE_DISABLED "Disabled / Press X to start", and a mode that is not built is NOT_BUILT. Modes without a rule are unchanged, and a note an allowing rule leaves never reaches the launcher. A source check requires `requestOledModePrepared()` to be declared in OLED_Display.h beside `requestOledMode()` and in no other file. `--component` / `--source-dir` read production sources from another tree. On f4cc542 the suite fails five checks (the rules are ignored) and the prototype check.

## Dead OLED code paths

```sh
python3 components/hardwareone/test/host/test_oled_dead_paths.py --sanitize
```

`oled_dead_paths` reads every top-level `*.cpp`/`*.h` of the component with comments and string literals stripped. It fails if any code still names a deleted OLED path:

- the dynamic remote menu and its remote command input;
- the bonded `DataSource` selection and its R/B indicator;
- the OLED_UI Dialog and Progress components, with `oledUIHandleInput` and `oledUIModalActive`;
- the display functions superseded by the `*Rendered` versions, and the undefined web-stats externs;
- `forceSettingsModeLink`, `resetOLEDFileBrowser`, `getRegisteredOLEDModes`, `getPreviousOLEDMode` and `LOG_MENU_VIEWER`;
- the helpers those deletions left without a caller: `hasRemoteSettings`, `oledDrawBox`, `oledDrawButton` and `oledDrawTextCentered`;
- the unified actions mode.

The live replacements must stay defined: `oledUiRender`, the shared toast renderer, the live drawing helpers `oledDrawIcon` and `oledDrawLevelBars`, the `*Rendered` displays, the mode registry, back-navigation and the slug functions. The suite then compiles the production `enum OledMode`, `modeFromSlug` and `slugFromMode` and checks three things: `oledmode unified` names no mode, no mode's slug resolves to a different mode, and "actions"/"imuactions" still open IMU actions. `--component`/`--source-dir` read production sources from another tree. On f4cc542 the contract names 65 identifiers, and the slug harness fails 2 checks: "unified" opens a mode, and `slugFromMode(OLED_UNIFIED_MENU)` is "actions", which opens IMU actions.

`oled_without_input` also accepts `--component`/`--source-dir`/`--sanitize`. It requires that the display-state API carry no bonded data-source selection.

## OLED action feedback

```sh
python3 components/hardwareone/test/host/test_oled_command_feedback.py --sanitize
```

`oled_command_feedback` compiles the shipping OLED action helpers (`oledRunCommand`, its
reply-to-toast mapping `oledCommandReplyText`, and the one OLED command in flight behind
`oledRunCommandAsync` and the background `executeOledCommand`, all from `OLED_Utils.cpp`,
with `System_PendingCommand.h/.cpp` spliced in unchanged) and real callers:

- the Automations toggle and delete actions with shared feedback (`OLED_Mode_Automations.cpp`);
- the Quick Settings radio, Wi-Fi, Bluetooth and HTTP rows (`OLED_SettingsEditor.cpp`);
- the Network menu's HTTP confirm callbacks (`OLED_Mode_Network.cpp`).

The OLED command executor and queue (scripted verdict and reply), the global toast, the
Automations rows and the radio/BLE/HTTP state are doubles.

The checks:

- A refused or failed command toasts its reply: an admin or super-admin denial as
  "Admin required", any other reply as its first line without the `Error`/`ERROR`/`[ERROR]`
  token, an empty or bare `ERROR` reply as "Failed". An `Error` reply fails even when the
  executor says ok, and an over-long reply stays inside the toast buffer.
- An accepted command returns true and leaves the toast to the caller.
- `oledCommandReplyText` (also used by the Add-WiFi flow) gives the caller's success text on
  success and the caller's fallback text for an empty refusal, and stays inside the caller's buffer.
- Each caller shows its success text ("Enabled", "Deleted", "Radio OFF", "HTTP OFF", ...)
  only on success; local refusals ("Radio is off. Turn it on first.", "Connect Wi-Fi first.") still run no command.
- The Quick Settings rows never wait for cmd_exec (the P4 radio toggle held the display
  task for 637 ms). The executor double finishes a queued command only when a case says so.
  The row shows "Working..." and returns at once; the executor only stores the reply; the
  next frame shows the row's result, once. A second row while one runs toasts "Busy" and
  queues nothing; a full queue is retried for 2 s and then ends as a refusal ("Command queue
  full...", the row free again); a reply from a replaced display session shows nothing; an
  over-long reply stays bounded.
- The background `executeOledCommand` (OLED background commands, step 1): the press queues
  at once and sets the busy flag; a second press, or an `oledRunCommandAsync`, is refused
  with "Busy" and queues nothing; a success shows no toast (owner 2026-11-22: failures
  only), while `executeOledCommandShowResult` (screens that do not show the result) toasts
  the reply without its `OK:`/`SUCCESS:` lead ("Done" for a bare OK, nothing or JSON; the
  first line or sentence of a long one); admin,
  super-admin and other refusals keep their short reason; 60 s without a reply toasts the
  timeout and the late reply shows nothing (its mailbox freed); a replaced session sees
  nothing; an empty line is refused at the press; the line is wiped when it ends; off the
  display task the action waits on the checked executor as before.
- The value-scroll rows (OLED background commands, step 2: FM tune, LED brightness;
  `oledValueScrollStep` on the production `OLED_ValueScroll.h`): a step is shown at once and
  sent at once when the slot is free; steps while it runs are neither sent nor refused
  "Busy", and only the newest goes out on the frame the running one ends (four steps, two
  commands); a step while another screen's command runs waits silently and goes out after
  it; a refusal toasts its reason and gives the row back to the device, unless a newer value
  waits (that one is still sent); after a success the row keeps its value until the device
  shows it or 1.5 s pass, then follows the device; a replaced session's step shows nothing,
  and a value still waiting at a display session boundary (behind the row's own command or
  another screen's) is dropped, never sent as the new user (statically:
  `oledResetLocalDisplaySessionTransients` calls `oledValueScrollSessionBoundary`).
- Static: the Automations, Logging and Network screens, the Quick Settings section and the
  unavailable-page / Web Stats actions (`handleOledActionButton`) no longer fire commands
  whose reply is dropped, and their `broadcastOutput` success lines are conditional. The
  Quick Settings section never calls the waiting `oledRunCommand`; `executeOledCommand`
  runs in the background; `oledUpdate` delivers the command in flight whether the panel
  runs or not, then sends a value-scroll row's waiting value, and `updateOledDisplay` draws
  its busy mark; every `oledRunCommand` call on an
  OLED screen (`OLED_*.cpp`, `i2csensor_*_oled.h`) uses its verdict or is a
  `(void)oledRunCommand(...);  // waits: <why>` line (the step-2 list).

`--component`/`--source-dir` point it at another tree; the f4cc542 sources fail it (no
helper; "Disabled"/"Deleted"/"Radio OFF" shown for refused commands). ASan/UBSan follow
`--sanitize`. The real executor, admin gate and display are not run.

`oled_remote_settings` (see "OLED bonded-peer settings editor") also checks the local screen: the slider/pick-list (INT) save runs through the same checked commit as the STRING save, so a refused save shows the executor's reply on the editor line.

## OLED Power Saving row

```sh
python3 components/hardwareone/test/host/test_oled_power_settings.py --sanitize
```

`oled_power_settings` compiles the Power > Sleep Settings row action and input handler
(`executePowerSleepAction`, `powerSleepInputHandler` from `OLED_Mode_Power.cpp`), the OLED
action helper (`oledRunCommand` from `OLED_Utils.cpp`) and the real `powersave` handler
(`cmd_powersave` from `System_Power.cpp`). The command executor is a double that applies the
admin gate with the `requiresAdmin` flag of the production `powersave` row, then runs
`cmd_powersave` and records a `[CMD]` audit line; `setSetting` records whether a write ran
inside a command or directly from the OLED task.

The checks:

- "Power Saving" submits `powersave <next step>`: a non-admin is refused ("Admin required"
  toast, value kept); an admin moves along the ladder (10 -> 15, 60 -> Off, Off -> 1, an
  off-ladder value -> 1) with one audit record and no direct write.
- A guest is still blocked ("View-only"); Sleep 20s asks first, defaulting to No (G-49), and runs
  `lightsleep 20` only on Yes; Screen Off runs its command as before.
- Static: no `OLED_Mode_*.cpp` calls `setSetting`, and the `powersave` row is admin-only.

`--component`/`--source-dir` point it at another tree; the f4cc542 sources fail it (the row
writes the setting directly, so a non-admin changes it with no audit line). ASan/UBSan follow
`--sanitize`. The real executor, admin gate, persistence and display are not run.

## OLED automation wizard

```sh
python3 components/hardwareone/test/host/test_oled_automation_wizard.py --sanitize
```

`oled_automation_wizard` compiles the shipping OLED "New automation" wizard from
`OLED_Mode_Automations.cpp` (its state, `wizKeyboardDone`, `wizCreate`, the confirm screen and
`wizHandleInput`, from `enum AutoWizStep` through the end of `wizHandleInput`) and its footer
toast, with `OLED_KEYBOARD_MAX_LENGTH` and the keyboard enums from `OLED_Utils.h`. Arduino
`String`, the display (records what is printed where), the keyboard, the event catalog, the OLED
command executor and the navigation events are doubles. It drives the wizard as the list handler
does: name keyboard, Interval, "5 min", command keyboard, confirm.

The checks:

- A 40-character command (with a quoted path) and a 40-character name reach the
  `automation add ... enabled=1` line whole, and so does a command of the keyboard's full
  length; every wizard keyboard admits no more than the wizard keeps.
- Text that does not fit is refused: the step's keyboard re-opens, nothing is stored and
  nothing is sent.
- The confirm screen shows every command character: the rows are rebuilt by pressing Down
  until the frame stops changing; text stays left of the arrow column, the action line stays
  on screen, Up returns to the top, A creates from any scroll position, and a new wizard
  starts at the top.
- A short command still draws the four-line confirm screen at y 11/20/29/38 without arrows.

`--component`/`--source-dir` point it at another tree; the f4cc542 sources fail it (name and
command cut to 32 characters, 18 shown on confirm, over-long text stored cut). ASan/UBSan follow
`--sanitize`. The real keyboard, executor and display are not run.

## Add-WiFi forms

```sh
python3 components/hardwareone/test/host/test_wifi_saved_ui.py --sanitize
```

`wifi_saved_ui` (see "Saved WiFi network coverage") also compiles the Add-WiFi forms with
`wifi_add_form_harness.cpp`: the OLED keyboard flow (manual Add and scan pick, `OLED_Mode_Network.cpp`)
and the G2 join-a-scanned-AP flow (`G2_Page_Network.cpp`). Both submit a quoted `wifiadd` line written by the
saved-network owner's structured add (`wifiAddCommandLine`), which the command parser splits at every double-quote,
so a double-quote or control character in the typed password
or in a scanned SSID must be refused before any line is built, with a toast (OLED) or lens text (G2). The
OLED toasts the owner's reply after an add through OLED_Utils' shared `oledCommandReplyText` ("Network saved",
"Connected", "Admin required" or the refusal's first line without its `[ERROR]`/`Error` token) and stays silent for a replacement display session; G2 keeps showing the owner's reply
on the lens. The OLED keyboard, session epoch and executor, and the G2 command bridge, text entry and lens are
host doubles. `--component <dir>` replays the cases against a pre-fix tree. A contract checks that neither form
writes the line or keeps a byte rule of its own; the owner's field rule decides and the forms only word the refusal.
The pre-fix forms fail the contract.

## OLED keyboard masking and dictation UI

`dictation_ui` compiles the shared OLED/G2 dictation delivery: the G2 text-entry
pad, `kbdPadConsumeDictationText`, `kbdPadDictationServiceOnTap` and
`oledKeyboardDictationTick`.

A second unit compiles the shipping OLED virtual keyboard from `OLED_Utils.cpp`:

- the character tables and `getCharAt`;
- `oledKeyboardInit` and `oledKeyboardReset`;
- the preview helper and `oledKeyboardDisplay`.

It uses the real `OledKeyboardState` and the Set Pattern openers, and draws on a
recording display.

The checks:

- A masked (secret) field never prints its text, or any 3-character piece of it,
  on any keyboard page: the four text grids, pattern, mic or suggestions.
- A masked field's preview is one `*` per character: the last 20, or the first 8
  on the suggestions page. The field buffer keeps the typed text.
- A plain field keeps its preview window, including a plain field opened after a
  masked one.
- Set Pattern's password and pattern keyboards open masked.
- A source contract: these fields pass the mask:
  - login;
  - change password;
  - Wi-Fi password, from manual add and from a scan pick;
  - the ESP-NOW remote password and passphrase;
  - first-time setup password prompts;
  - the user manager's new-account password (`umStartKeyboard` stage 1, the
    same condition that selects the DENY dictation policy).

  The SSID does not pass it, and first-time setup no longer writes `*` into the
  shared buffer.

Related checks in other suites:

- `wifi_saved_ui`'s Add-WiFi unit checks that the OLED password keyboards open
  masked and the SSID keyboard opens plain.
- `setup_display_cancel` checks that a first-time-setup password prompt opens
  masked and that its render leaves the field buffer intact.

`--component` points `dictation_ui` and `setup_display_cancel` at another tree.
ASan/UBSan follow `--sanitize`. The panel driver and fonts are not covered.

## OLED pair-request prompt

`oled_pair_prompt` compiles the shipping incoming-pair-request prompt from
`OLED_ESPNow.cpp`: everything after the `sPairingSel` static through
`oledEspNowPollPairRequest()` (the prompt statics, the Accept/Reject callbacks
and the poll), plus the file's `oledEspNowRunOwnerCommand` helper when it has
one. It runs them with the shipping confirm overlay from `OLED_Utils.cpp` (state,
request, close, input, session-boundary reset) and the mode enum and `NavEvents`
from `OLED_Display.h`. Doubles: the pending request (sender MAC, name, arrival
time), the `espnowaccept`/`espnowreject` owner behind a switchable admin
refusal, the cached session role (`oledSessionIsAdmin`), counting `users.json`
traps and the toast. A frame is one poll, as `oledUpdate()` makes it.

The checks:

- A request answered on the panel opens the dialog once, even when the owner
  refuses the answer; the refusal is toasted ("Admin required").
- The prompt never opens on the login screen, for AuthBypass (display auth off),
  for a non-admin session, or over the Login screen of a logged-in admin. A
  request that waited through login opens once for the admin.
- Accept and Reject name the shown sender's MAC. If another sender replaced the
  request while the dialog was up, Accept does not pair it; the newer request
  opens next under its own name.
- A re-sent request (new arrival time) opens once more; for an expired one the
  owner refuses the answer and its reason ("No pending pair request. Open
  discovery with 'espnowpairmode' on both devices and send the request again.")
  is toasted once.
- A session boundary drops the dialog without an answer and the next session
  sees the request once.
- Idle frames make no role query, and the prompt never reads `users.json`.

`--component`/`--source-dir` point it at another tree. ASan/UBSan follow
`--sanitize`. The real commands, the role cache and the radio are not run.

## OLED list navigation

```sh
python3 components/hardwareone/test/host/test_oled_list_nav.py --sanitize
python3 components/hardwareone/test/host/test_sensorlog_ui.py --sanitize
python3 components/hardwareone/test/host/test_systemlog_config_ui.py --sanitize
```

`oled_list_nav` compiles the Automations list navigation (`autoListUp`/`autoListDown` and
`struct AutomationRenderData` from `OLED_Mode_Automations.cpp`), the file list navigation
(`oledFileBrowserUp/Down/Select/Back` and the pending-action slot from `OLED_Mode_FileBrowser.cpp`)
and the settings editor (`OLED_SettingsEditor.cpp`: the OLED predicate wrappers, the module source,
`initSettingsEditor`, `displaySettingsEditor`, `settingsEditorDown/Select/Back`,
`openSettingsEditorForModule`, with the settings and editor types). Any file-scope state those
functions use on the tree under test comes along. The clock, FileManager, settings registry,
settings-editor core, keyboard, commit path and display (which records each frame) are doubles.

The checks:

- Nav events 40 ms apart (a briskly turned ANO wheel, quick dpad presses) and two events in one
  tick each move the Automations list one row (clamped at the top and at the "+ New" row) and the
  file list one row; A and B right after a move still request open / parent folder. The input layer
  already paces nav events, so no list adds its own debounce.
- The settings editor draws every row of a 4-row module with no scroll arrow after a module with no
  editable row (entered from the list or with `openSettingsEditorForModule`) and after a scrolled
  module; a 6-row module's window follows the cursor with the right arrows.

`sensorlog_ui` also builds a second unit (`sensorlog_config_list_harness.cpp`) that draws the Logging
"Configure" list with the shipping scroll code (`OledScrollState`, the scroll primitives and both
renderers from `OLED_Utils`) through the real `handleLoggingModeInput` and render block, walks the
11 rows down and up, and checks every frame: rows inside the content area, every glyph on the 128x64
panel, a visible scroll indicator while rows are hidden, the selected row drawn, and as many rows as
the content area holds.

`--component`/`--source-dir` point both suites at another tree; the f4cc542 sources fail them (the
Logging list's arrows are drawn at x=128, off the panel, with 4 rows shown; Automations and Files
drop a nav event or press that follows the previous one within 200 ms; the editor's function-static
item offset goes negative and shows a spurious down arrow in the next module). ASan/UBSan follow
`--sanitize`. Physical input timing, the ANO encoder and the real display are not run.

`systemlog_config_ui` runs the whole System Logging > Configure block of `OLED_Mode_Logging.cpp`
(Tags, Categories as family > flag lists, the read-only Path) over the shipping debug-flag table
(`System_DebugFlags.h`), the mask parse/format and `systemLogConfiguredFlags` (`System_Debug.cpp`)
and the scroll primitives and single-line renderer (`OLED_Utils`). The executor stores
`logcategorytags` / `systemlogflags` the way their set-commands do. It checks every frame against
the 128x64 panel (beside the scrollbar when one is drawn), every family and flag row against the
table with labels clipped to the 19 characters beside the cursor, that the saved mask wins over the
live one, that a flag toggle keeps the other categories and is applied live, and that Back and B
land on the row they came from. A row sized past the panel, or a toggle that sends only the one
flag, fails it. The real commands, settings persistence and display are not run.

## Web Logging page: System Logging configuration

```sh
node components/hardwareone/test/host/logging_system_config_browser_test.js
```

`logging_system_config_browser` runs the Logging page's own `window.onload`, category grid
(`populateFlagsPane`), saved-options preload and `applySystemConfig` from `WebPage_Logging.h` in
Node; only the DOM and HTTP are fake. With `/api/debug/flags` and `/api/logging/status` landing in
either order, the boxes and the tags checkbox end on the saved `systemLog` options (no options in the
reply leaves the HTML defaults). Apply sends one `beginwrite` / `systemlogflags 0x...` /
`logcategorytags` / `savesettings` batch and reports success only when the batch is `ok` with no
refused command; a refusal, an unsaved batch and a transport error are shown, and an empty selection
sends nothing. Dropping either preload call fails it. The endpoint itself and the real commands are
not run.

## OLED Map actions, input devices and waypoint saves

```sh
python3 components/hardwareone/test/host/test_oled_map_actions.py --sanitize
```

`oled_map_actions` compiles the production OLED Map mode (OLED_Mode_Map.cpp: the Map state block, the track scan, the menu dispatch, `gpsMapInputHandler` and `displayGpsMap` through map centering) against owner, keyboard, file-picker and VFS doubles. It also compiles HAL_Input's real button mapping (the controller tables, `inputGetButtonMask`, `inputIsButtonPressed` and `INPUT_CHECK`, with the seesaw and ANO bit definitions) and `NavEvents`. The Map unit is built four times: for the seesaw gamepad with `ENABLE_GPS_SENSOR` 0 and 1, for the ANO rotary encoder, and for the GPIO wheel with the P4X-EYE keys. Each build feeds the handler what the OLED input router hands a mode on that device: the device's own press bits; the wheel's detents with up/down nav events; ANO dpad presses as nav events; and stick deflection, which produces a nav event only on the router's first-deflection and auto-repeat cadence. The identity double mirrors the per-task slot: `CommandIdentityScope` installs a context, and otherwise `currentAuthContext()` returns the empty anonymous one, which the owner and VFS doubles refuse as `checkPerm` does. Map actions: Mark Waypoint adds the typed name at the map center once the keyboard completes. A cancel adds nothing, and a refused write or a full table is shown as a toast. Goto Waypoint, Clear Nav, Delete Waypoint, Load Track, Delete Track, Next Map, Previous Map, the first-map auto-load (at most one attempt per 5 s while no map loads) and the load from the Select Map picker all run as the OLED user, and the track picker scans with that identity. Select Map opens the file browser as a `/maps` picker that shows only `.hwmap` files. Input, on every device: START opens the menu. The wheel or stick moves through the menu, the track and waypoint pickers, the features list and the routes list. A confirms and B goes back or closes. Search results step with left/right or the wheel. The info and track-status overlays close on A or B. In the routes viewer X highlights a route. On the map, X zooms in, Y zooms out and B leaves. ANO UP moves up instead of backing out, a GPIO double tap (SELECT) does not confirm, and a held stick moves the menu only on the router's nav events. A second unit compiles the production WaypointManager mutators, `cmd_waypoint`, `cmd_waypointfile`, CommandArgs and `requireQuotedToken` (System_Maps.cpp, System_Command.cpp, System_Filesystem.cpp) with a `saveWaypoints()` double. `waypoint add`, `waypoint del` and `waypointfile` report a write that fails, and the waypoint table and navigation target stay unchanged. Adds and deletes that save keep their replies. `--component <components/hardwareone>` reads all production sources from another tree. The f4cc542 sources fail 30 Map checks in each seesaw build, 59 on the ANO build and 55 on the GPIO build, plus 11 save checks.

## OLED Map context bar

```sh
python3 components/hardwareone/test/host/test_oled_map_context_bar.py --sanitize
```

`oled_map_context_bar` compiles System_Maps.h's `LocationContext`, the context-bar append helper from OLED_Mode_Map.cpp and the `displayGpsMap` block that builds "On <road> • Near <area> (<n>m)" in its 128-byte `contextBuf`, all unchanged. Inside them `snprintf`/`vsnprintf` follow the C contract and store every byte from instrumented code, so ASan sees any write outside `contextBuf`. Each case runs in its own process: short names, area only, road only, no names, a 127-byte exact fit, and road and area names of 58+58, 58+57 and 63+63 characters with the area near and far. The bar text must be the whole context cut at 127 bytes, stay terminated inside the array, and `pos` must equal its length and stay below 128. `--component <dir>` reads the production sources from another tree; the f4cc542 sources fail four cases: `pos` ends at 134 or 139, and with two 58- or 63-character names and a near area the third append forms `contextBuf + 129` (UBSan bounds error; with ASan alone, a stack-buffer-overflow write).

## Bond manifest and schema buffers

`espnow_bond_json` guards the bond manifest (every command's name and help, 70,773
bytes on xiao-default today) against the 65,535-byte cap of an Arduino `String` on boards
without PSRAM support in the core. Source contracts: `sendBondManifest`,
`sendBondSchema` and `cmd_bond_showmanifest` serialize through `serializeBondJson`;
`v4h_file_end` hands the transfer buffer to the manifest and schema handlers without a
`String` copy; `cmd_bond_showremotemanifest` and the fwHash branch of
`handleEspNowRemoteManifest` read through `espnowReadCachedManifest` and the web branch
takes no filesystem lock itself. It then compiles the production `bondJsonEnvelopeOk`,
`serializeBondJson`, `writeBondJsonFileLocked`, `cacheManifestToLittleFs`,
`processBondModeManifestResp`, `processBondSchema`, `manifestCachedFor` and
`espnowReadCachedManifest` with the vendored ArduinoJson and a harness-local in-memory
filesystem whose reads and writes assert the filesystem lock. A ~110 KB manifest is
serialized into a buffer of exactly `measureJson()` bytes, received, cached and read
back byte for byte; a manifest cut at 65,503 bytes is rejected; a document over
`kFileSlotMaxFileSize`, an allocation failure, a short write (the partial file is
removed), a failed read and malformed hashes (`../`, uppercase, wrong length; no
filesystem call) are refused, and every buffer is freed. `broadcastBondJsonChunks`, the
console printer of a manifest dump, runs against a modeled 64-line output queue that
drops when full and must deliver every line. `--component`/`--source-dir`
point it at another tree; ASan/UBSan follow `--sanitize`. The radio transfer, the
senders' `sendFileToMac` and the web response are not run.

## ESP-NOW identity and HTTPS certificate writes

`espnow_identity_writes` compiles the production `writeIdentityFile` (this device's
encrypted Ed25519 identity) and `writePeerIdentityFile` with their directory and path
helpers and the shared `writeJsonFileComplete`, the vendored ArduinoJson and an
in-memory filesystem whose free space can run out mid-write. A write one byte short,
half a file, a full partition, a backend that stores less than `write()` reports, and a
failed rename must each return false, remove the temp, never reach the rename (short
writes) and leave the previous `identity.json` byte for byte; a write that exactly fits
succeeds. `--source-dir <dir>` reads `System_ESPNow_Identity.cpp`/`.h` from another tree:
the pre-fix writers (only `written == 0` rejected) fail 22 checks.

`https_cert_writes` is a source contract on `cmd_certgen`: no File is opened with "w";
both PEMs are staged as `.tmp` siblings through `writeText` in one checked condition
before either is renamed into place (the helper's own short-write cases are in
`checked_text_writes`), and a staging failure removes both temps. HTTPS start does not
check that key and certificate match, so this keeps a short write from leaving a
mismatched pair. No message claims an HTTP fallback, and the `String` copy and PSRAM
buffer of the key PEM are wiped. `--source-dir` runs it on another tree; the pre-fix
code fails.

```sh
python3 components/hardwareone/test/host/test_espnow_identity_writes.py --sanitize
python3 components/hardwareone/test/host/test_https_cert_writes.py
```

`definition_owners` is a source contract on three definitions that used to be spelled at every
use: the OTA handoff NVS keys `ap_pass`/`auth_token` (owner `hw1_ota_nvs.h`, used by System_OTA.cpp
and the updater), the OTA upload frame marker `00 'HW1OTA' 01` and its 12-byte head (owner
`BLE_OtaWire.h`, used by the secure channel, the phone console records and handleBleUploadFrame)
and the HTTPS key pair paths (owner `System_HttpsCertPaths.h`, used by HTTPS start, certinfo and
certgen). Each literal appears only in its owner (the web settings page script is the one listed
exception) and every former site uses the owner's name. It also compiles the owners and the
extracted certinfo/certgen reply expressions on the host and compares them byte for byte with the
old values. It also pins the OTA result words (N-44): the recovery updater's status carries the
journal's last result (`resultCode`, `resultSequence`, `resultDetail`), and its `result_name` gives
every `hw1_ota_result_code_t` value the word System_OTA.cpp's `resultName` gives it (the two
programs cannot share the function; the updater is not compiled by the firmware builds).
`--root <repository>` reads another tree; the pre-change tree fails.

```sh
python3 components/hardwareone/test/host/test_definition_owners.py
```

`sensor_json_keys` is a source contract on the sensor JSON that travels between devices. Each
key the receiving device reads by name has one owner, `System_SensorJsonKeys.h` (`HW1_SK_*`
string-literal macros, `HW1_SK_Q(key)` for a quoted key inside a format string). The owner is
used by the seven drivers whose readings a receiver parses (GPS, IMU, ToF, FM radio, gamepad,
ANO and GPIO encoder `BuildDataJson`). Its receivers are `getRemoteGpsData` and the remote-sensor
OLED screen. No sender or receiver spells an owned key, and the most distinctive keys appear
nowhere else. The test also compiles every sender format and receiver key on the host. It
compares them byte for byte with the old literals and runs old and new formats through snprintf
with the same sample values. `--root <repository>` reads another tree; the pre-change tree fails.

```sh
python3 components/hardwareone/test/host/test_sensor_json_keys.py
```

`state_file_owners` is a source contract on the state the device stores. The eight /system state
files (settings, debug, users, pending_users, the user_settings folder, boot_anchors, automations,
espnow/devices) take their paths from `System_StatePaths.h`: the modules keep their names
(SETTINGS_JSON_FILE, USERS_JSON_FILE, DEBUG_JSON_FILE, ...) as aliases, and the path permission
table, the backup/restore tool, factory reset and the setup replies use the owner. The phone-link
credential keys come from `BLE_PhonePipeKeys.h`, whose lists the peers writer drops and factory
reset removes, so a key the writer adds cannot survive a reset. The network.wifi section and
switch keys come from `hw1wifijson` (System_WiFiNetworks.h), shared by the wifi settings rows, the
saved-network array and the restore. Each literal appears only in its owner and every former site
uses the owner's name. It also compiles the owners, the permission table (the state-file rows must
equal the old rows), the texts that splice a path, and runs the changed JSON blocks (factory
reset's key removal, the peers.phone writer and reader, the restore's network.wifi merge) beside
the old code on the same JSON. `--root <repository>` reads another tree; the pre-change tree fails.
`--print-table` prints the compiled table for an old/new comparison.

```sh
python3 components/hardwareone/test/host/test_state_file_owners.py
```

## Camera lifecycle and backends

```sh
python3 components/hardwareone/test/host/test_camera_core.py --sanitize
python3 components/hardwareone/test/host/test_camera_dvp.py --sanitize
python3 components/hardwareone/test/host/test_camera_p4.py --sanitize
```

`camera_core` compiles the production lifecycle, integer-control and resolution
functions from `System_Camera_DVP.cpp` with `HAL_Camera.cpp` against a
failure-injecting HAL: shared ownership, serialization, recovery fencing,
temporary settings and persisted control failures. `camera_dvp` runs the DVP
backend from `HAL_Camera_DVP.cpp` with the SCCB port set to 0 and to 1 against
simulated camera and SCCB edges (SDK port ownership, maximum-geometry
allocation, live resize, SOF verification, cleanup). `camera_p4` runs the P4
backend and RGB565 resize code under host faults (ownership, cleanup,
stream-stop retry, deadlines, budget and capture failures) for the EYE and
Waveshare board definitions. The resize unit checks each board mount rotation
(0, 90, 180 and 270 degrees) combined with mirror and flip against an
independent turn-then-reflect model, a scaled quarter turn, tile edges on a
frame that is not a multiple of the tile width, and rejected angles. The
Waveshare lane's 90-degree mount must report, encode and return each selected
size as portrait while the upright EYE lane keeps landscape sizes. No hardware
is touched.

The eight camera suites (`camera_core`, `camera_dvp`, `camera_p4`,
`camera_i2c_reservation`, `camera_managed_sccb`, `camera_video_buffers` and
`camera_consumers`, plus `camera_oled_preview`) are registered CTest suites;
ASan/UBSan follow `HW1_SANITIZE`.

## Shared monochrome camera preview

```sh
python3 components/hardwareone/test/host/test_oled_camera.py --sanitize
```

`camera_oled_preview` executes the production luminance/downsampling/dither
renderer and the complete OLED camera mode against RTOS, display and camera
boundary doubles, using the real ArduinoJson serializer. It checks 64, 88 and
128 pixel canvas heights, bitmap bounds, flat/gradient scenes, freeze/resume,
stopped-source restart, capture/decode drain before photo commands, late-frame
rejection after view/session changes, allocation failures and worker/stream
cleanup. OLED/camera-disabled combinations compile separately. Hardware JPEG,
panel output, physical input and sustained camera/web/STT coexistence require
device qualification.

## Shared camera controls and consumers

```sh
python3 components/hardwareone/test/host/test_camera_consumers.py --sanitize
python3 -m unittest tools.webui.tests.test_camera_page tools.webui.tests.test_embedded_js_syntax
```

The C++ runner compiles the whole production G2 camera settings page with only
runtime boundaries mocked, plus the real HAL descriptor and frame-validation
implementation. It checks fixed resolution IDs/dimensions, invalid IDs, JPEG
marker and byte-budget admission, release ownership, backend-specific settings
and resolution rows, no command on an unsupported control, stale taps after a
capability change, failed command submission, and text truncation at every
capacity from 1 to 1023 bytes. The hijack-page enum, the command cookie and the
redraw declaration come from `G2_Glasses.h` and `G2_HijackCmd.h`; a static
assertion stops the build when the redraw stub's signature no longer matches
the header, so a boundary change cannot leave the stubs compiling against a
stale copy. The JavaScript test executes the actual served
camera script against a small DOM adapter to verify supported options, disabled
controls, capability changes, command gating and visible CLI errors. These are
not sensor, browser layout, G2 transport, SD recording or physical camera tests.


### Edge Impulse input geometry, labels and pipeline

```sh
python3 components/hardwareone/test/host/test_edge_impulse_jpeg.py --sanitize
ctest --test-dir <build> -R '^edge_impulse_jpeg$' --output-on-failure
```

This compiles the whole of `System_EdgeImpulse.cpp` after
`edge_impulse_host_fakes.h` (fake TFLite Micro interpreter with exact-size
tensors and guard bands, in-memory files, camera capture, httpd) and links the
real `HAL_JPEG.cpp` front end with a faked codec backend. Scenarios: a
96x96x1 grayscale model (luma fill of exactly the tensor), a changed former
`eiinputsize` value after load, a non-square 64x48 model, float32 and int8
fills, a 12-class output (label table sized from the model, each detection
owning its label, distinct `class_N` names for unnamed classes), camera/file
parity (same input tensor, same results, same no-model errors;
only the camera updates tracked objects) and the web detect JSON
(`modelInputSize`, `lastSeenMs`, `stateChangeMs`). The decode unit checks the
module's VGA cap and rejections before the codec; the link build checks that an
ENABLE_EDGE_IMPULSE=1, ENABLE_HTTP_SERVER=0 build has every command handler;
static checks cover the single pipeline, `hwjpeg::decode` as the only decoder,
the removed exports and the removed `eiinputsize` command. Not covered: real
TFLite kernels, arena placement, camera hardware, the hardware JPEG codec.

### Camera / sensor I2C ownership

```sh
python3 components/hardwareone/test/host/test_camera_i2c_reservation.py --sanitize
python3 components/hardwareone/test/host/test_camera_sccb.py --sanitize
python3 components/hardwareone/test/host/test_camera_video_buffers.py --sanitize
```

The reservation suite runs the actual `System_BuildConfig.h`, manager constructor,
`initBus`, lifetime leases, `cmd_i2cbusenabled` and `cmd_i2c2busenabled` across
P4X-EYE, Waveshare, S3 and classic ESP32 with camera enabled/disabled and three
DVP SDK port selections. EYE reserves I2C0; Waveshare shares managed I2C1; DVP
follows its SDK port. Camera-disabled builds reserve neither controller.
Twenty-two configurations run. Two dedicated-camera/external-I2C conflicts and
a shared camera without the I2C subsystem must fail compilation explicitly.

The runtime checks verify logical bus 0 maps to Wire1 / hardware I2C1 and bus 1
to Wire / hardware I2C0. The unreserved controller initializes normally while
the reserved one performs no Wire begin, clock/timeout setup, power-pin writes,
online event or bus-state update. Each CLI enable command rejects its reserved
controller without persisting or changing either bus's existing configuration;
the unreserved bus remains independently configurable. Disabling remains allowed,
and empty and validation-only commands make no writes.
Active leases reject bus reconfiguration until the last client releases it.

The SCCB suite compiles `HAL_Camera_SCCB.cpp` with transport/manager adapters and
the pinned SDK's actual `esp_sccb_io_t` interface. Resolve managed dependencies
with a P4 camera profile build first, or pass `--sccb-component` to the component
directory. An unavailable SDK header skips this test (exit 77). It covers dedicated-bus pass-through,
managed transfers, repeated-start reads, short reads, transport/lock failures,
bounded arguments and borrowed-bus lifetime. It does not emulate a sensor.

The video-buffer suite executes the actual pinned SDK constructor/destructor
and `HAL_Camera_VideoBuffers.cpp`. It checks that interrupt descriptors move to
internal RAM, pixel allocations and ownership stay intact, self-references
target the relocated descriptor, SDK-internal allocations pass through, and
failed allocations unwind completely. It requires a resolved P4 `esp_video`
component (`--video-component`), otherwise skips with exit 77. Link placement
and cache-disabled capture still require the real firmware and hardware.

Wire, GPIO, logging, core-affinity dispatch, Arduino String parsing and settings
persistence are host adapters. These tests do not prove physical controller
routing, ISR placement or camera/SCCB operation on a board.

`test_jpeg_validation.py --jpeg-component /path/to/espressif__esp_jpeg --sanitize`
compiles the shared entropy validator against the actual pinned TJpgDec codec.
It exercises FASTDECODE 0/1/2 with scaling enabled and disabled, complete and
truncated entropy, normalization, unsupported formats, and allocation failure.
An optional `--private-bad /path/to/observed.jpg` checks a local failure artifact
without adding private photographs to the repository.

## Speech vocabulary and display formatting

`stt_transcript_tests` compiles the production `stt/stt_transcript.cpp` (the
portable vocabulary table and display formatter ported from the standalone STT
handoff): display aliases are exact whole words matched case-insensitively,
standalone "I" and its contractions, paragraph-start casing, whitespace, the
256-entry capacity, spoken/display validation, immutable raw input and output
bounds with sentinels (an alias that does not fit yields an empty string, never
a cut term).

`stt_vocabulary` (`test_stt_vocabulary.py`) runs the actual
`System_STTVocabulary.cpp` over guarded-VFS, staged-writer, lock and allocator
fakes: card-first loading, the internal copy, whole-file validation (an invalid
file or edit keeps the active list and its generation), staged replacement with
the previous file kept on every failure (stage refused, commit failed, short
write, lock busy, not writable, card read-only, allocation failed), the
generation and hotword snapshot the decoder takes, and bounded display
formatting while the list is busy. `stt_local_lm` covers the decoder side:
hotwords come from the vocabulary snapshot, a new generation is re-taken once
between phrases of the same session, and the retired `custom_words.txt` is
noted once and never read.

## Local STT language-model decoder

`stt_lm_tests` compiles the production `stt/stt_lm.cpp` (host SHA-256 and
allocation path) against HW1LM1 files the test writes itself
(`stt_lm_testlib.h`), per `experiments/stt_train/lm/FORMAT.md`. It covers SHA-256
vectors; header/section/padding/digest/string-order/n-gram-order/word-id
rejection behind a valid digest; ARPA backoff scoring; custom-word
normalisation, limits and de-duplication; and the prefix beam search: blanks,
repeats, leading/double/trailing spaces, capacity truncation, OOV scoring and
`<unk>` context, hotword bonus, byte-order tie-breaks, >255-letter words,
cancellation polling and argument limits. 3000 random small cases are compared
with the independent map-based float64 reference decoder in `stt_lm_testlib.h`;
a case is skipped only when some beam cut or final pick is within 1e-6 nats and
is not a structural tie (equal LM parts): exact real-number coincidences reached
through different arithmetic round differently in any two implementations. Both
decoders follow the readings `experiments/stt_train/lm/hw1lm.py` documents:
`char_prune` never prunes the repeat of a prefix's last symbol (space after space
is blank-like), and custom-word lines split like Python `str.splitlines()`.

`stt_lm_parity` runs `test_stt_lm_parity.py` over `fixtures/stt_lm/` (tiny.lm,
caseN.i8, custom_words.txt and a JSON manifest written by the host builder's
reference decoder). `stt_lm_tool` decodes each case with the same decoder and
the production greedy CTC; both texts must match. With no manifest the test is
reported as skipped (exit 77), not passed.

`stt_local_lm` (`test_stt_local_lm.py`) compiles the actual
`System_STTLocal.cpp` with `ENABLE_STT_LM` on and off against VFS/heap/log/
runtime fakes and the real decoder: load once per session, one-shot lifetime,
custom words, missing/invalid/truncated files, PSRAM refusal, release under
memory pressure, cancellation mid-read and log-once behaviour. The runtime side
of the hook (Decoded/Fallback/Cancelled, unterminated output, release before a
memory refusal) is covered in `test_quartznet_cache.py`.

Host benchmark (not a CTest; optimised, unsanitised build):

```sh
build/stt_lm_tool bench --frames 1000 --beams 16,32
```

It builds a synthetic 20k-word / 400k-bigram / 400k-trigram LM and 20 s of
synthetic logits. Timings are host-only; the P4 has no double-precision FPU, so
score arithmetic there is software double and must be measured on the device.

## G2 transcription input routing

`test_g2_text_navigation.py --sanitize` runs the production decoded TextEvent
and SysEvent navigation branches with actual callback-slot publication and
ownership checks. It verifies directional callbacks for ordinary and multi-text
pages, stale/pending/overlay rejection, and exclusion of ordinary list pages.
The RTOS queue is a controlled boundary; this does not simulate BLE delivery or
prove that the glasses firmware emits gestures for a particular layout.
`test_g2_transcription.py --sanitize` exercises the full page and requires one
capturing header among four children. `test_g2_transcription_wire.py --sanitize`
decodes the production CREATE payload to require capture flags `1,0,0,0`, while
checking CRCs, single-fragment sizes and the observed ATT write limit.

```sh
python3 components/hardwareone/test/host/test_g2_text_navigation.py --sanitize
python3 components/hardwareone/test/host/test_g2_transcription.py --sanitize
python3 components/hardwareone/test/host/test_g2_transcription_wire.py --sanitize
python3 components/hardwareone/test/host/test_g2_text_page_ready.py --sanitize
```

## Local STT latency features

These standalone scripts compile the production broker, runtime, segmenter and
ESP-DL metadata hook against host fakes. Run each once (add `--sanitize` for
ASan/UBSan):

```sh
python3 components/hardwareone/test/host/test_stt_continuous.py
python3 components/hardwareone/test/host/test_quartznet_cache.py
python3 components/hardwareone/test/host/test_stt_segmenter.py
python3 components/hardwareone/test/host/test_stt_runtime.py
python3 components/hardwareone/test/host/test_stt_dl_metadata.py
python3 components/hardwareone/test/host/test_g2_tl_send.py
```

`test_stt_continuous.py` runs the continuous broker with every latency switch at
its default (on): capture-time features (`HW1_STT_CAPTURE_FEATURES`; each cached
row is compared bit for bit with the real frontend's batch row of the exact,
possibly trimmed or speculative, input), trailing-silence trim, speculative
finals (short closes included; promoted on the same input, dropped on resumed
speech, aborted when the final the worker takes next does not match it; the
snapshot is copied over a running draft as soon as that draft is past its
frontend, and staged fake drafts hash their input to prove it is never written
while their frontend reads it), the 1 s short close, the publish wake for drafts
and finals, the 1.25 s native pause feed, feature-pool and task failures, native
full queues and restarts, alongside the existing keep-warm, per-session source
and transcript tests.
`test_stt_runtime.py` builds the one-shot path with all of those switches off,
so every off path still compiles.

`test_stt_continuous.py --speakers --sanitize` additionally enables the production
speaker metadata path with controlled background results. It checks labels arriving
after text ACK, labels without transcript saving, bounded history eviction,
owner/epoch/token refusal, cancellation, terminal drain and expiry, and live labels
when the saved-line ring cannot allocate. The existing saved transcript writer is
exercised for ordered `[Person N]` prefixes. `test_dictation_local.py --sanitize`
checks the App lease's delayed/terminal label snapshots for local, CM5 and disabled
builds; `test_transcription_ui_adapter.py --sanitize` checks their private JSON
response. Speaker model accuracy and on-device label latency require hardware.


`test_quartznet_cache.py` runs the runtime twice: per-call models
(`quartznet_cache_harness.cpp`, `HW1_STT_RESIDENT_MODEL=0`) and the resident
model (`quartznet_cache_harness_resident.cpp`, the firmware default). The latter
checks the build after the warm-up, re-shaping, refusals and the internal-RAM
floor, the hand-over (`dropResident`: the arena is freed and rebuilt by the next
owner), metadata placement through the PSRAM hook with fallbacks and failures,
`callBudget` against admission, and that every TL line stays numeric.
The resident floor is fixed at 128 KiB independently of unrelated task
allocations/frees during the build; the cold floor remains 150 KiB. Tests include
the hardware refusal samples (149,747 / 150,371 B), both floor boundaries and
preserving the PSRAM language model on internal-only refusal. Vendor doubles
exercise admission policy, not real inference scratch peaks.

`test_stt_conv_scratch.py` (CTest `stt_conv_scratch`) compiles the staged
production ConvOpArgs and prepared-storage methods with fake tensors/heaps.
ASan/UBSan covers repeated reuse, 16-byte alignment, per-channel values for
8/16-bit activations, split-core pointer offsets, borrowed versus owned cleanup,
transactional growth/failure, independent layers and the unprepared fallback.
The SDK patch is also verified by applying it to the pinned pristine package.
These tests do not emulate the ISA kernels or measure device latency.

The speaker-enabled continuous broker suite checks per-session opt-out: no
optional model start, submission, polling or drain, text still delivered, and
default speaker behavior restored for the next native session. Native Terminal's
harness requires its speech-start call to opt out; other consumers keep the
API default. Firmware ELF checks verify external placement of text/performance/
level/stage arrays; the performance ring explicitly constructs nonzero sentinels.

`test_stt_dl_metadata.py` compiles the staged ESP-DL override's
`dl_tool_metadata.cpp`, `dl_model_context.cpp` and TensorBase code (extracted
unchanged) with `stt/stt_dl_metadata.cpp` against a fake heap: every block the
hook hands out returns through `ps_free` on every destructor path, prebuilt-style
blocks are accepted, PSRAM refusal falls back to internal RAM and is counted.
The two build-time kinds (the memory planner's records; prebuilt libfbs_model's
own `operator new`, by the names its linked copy calls) are placed the same way
and counted apart from the long-lived totals, and per-channel exponents kept as
`int8_t` (`DlCompactExponents`) read back equal to the `int` arrays through
every copy and move.
Without a staged override (`tools/p4/prepare_sdk.sh`) it exits 77 (skipped).
`test_g2_tl_send.py` checks the Conversate TL send bookkeeping.


`test_g2_settings_protocol.py --sanitize` checks the complete production settings
codec, golden vectors, malformed/truncated payloads, exact version strings,
correlated oneof bodies, explicit zero and nanopb-elided wear OFF, the
captured 2.3.2.14 silent-mode frames (write, ACK with the zero left out, BASIC
f14, cmd 3 change push), auto-brightness frames (write, ACK with the explicit
zero, BASIC f18) and the brightness-level and head-up frames (writes and ACKs,
BASIC f2 and f7). ctest runs it as `g2_settings_protocol`.
`test_g2_settings_command.py --sanitize` exercises the production command,
RX owner and exact-generation sender with simulated BLE boundaries: request IDs,
fresh reads, qualified default OFF, idle renderer/native readiness, ACK matching,
connection rollover, cold discovery, and preservation of other firmware guards.
It also runs the G2_QuickList boundary (the typed read of all five rows, the
wear, silent, auto-brightness and head-up writes, the two-write Brightness step
with its wire bytes, the FULL_UPDATE wire gate) against the
same fixtures, including the resent read: unanswered and late replies (a reply
scheduled on the simulated clock), a late reply to an earlier read, and the
attempt, latency and other-frame numbers each read records.
Real G2 settings acceptance is tracked separately in
`docs/G2_WEAR_DETECTION_23214.md`.

## Native Hey Even with local speech and chat

```sh
python3 -B components/hardwareone/test/host/test_g2_evenai_local.py --sanitize
python3 -B components/hardwareone/test/host/test_stt_continuous.py --sanitize
python3 -B components/hardwareone/test/host/test_llm_http_integration.py --sanitize
python3 -B components/hardwareone/test/host/test_g2_conversate.py --sanitize
```

The native adapter test compiles the actual G2 adapter and packet guard with
controlled speech, chat and BLE boundaries, including disabled build paths.
It checks multi-segment questions, UTF-8 reply deltas, blank/error outcomes,
stale epochs, model changes, dismissal and successor-turn protection. The
production session tick is also exercised through a 20-second speech wait and
120-second reply wait: periodic heartbeats continue, active work outlives the
ordinary idle limit, and idle expiry resumes after completion. The
continuous STT suite exercises the production finite native question mode:
pause endpointing, hard-cut continuation, timeouts, revoked owners and refusal
to silently drop queued question audio. The chat suite checks exact run tokens
even when two backend generations reuse a numeric session ID. These tests do
not qualify physical wake-word recognition, microphone onset or lens rendering;
those checks are tracked in `docs/G2_EVENAI_LOCAL_WORK.md`.

## Retained LLM replies

`System_LLMReplyLimits.h` sets the shared 16 KiB reply/turn storage cap, including
NUL and the HTTP source's reserved terminal notices. The HTTP parser and worker
suites retain a 12,000-byte reply; the chat integration suite retains 16,383
bytes including its truncation notice. The OLED uses PSRAM scratch and the web
history handler streams small JSON-escaped windows, keeping full replies off
the task stacks.

```sh
python3 -B components/hardwareone/test/host/test_llm_turn_serving.py --sanitize
```

This executes the production CLI and web history handlers with long, empty,
UTF-8 and escape-heavy turns, plus a failed web write. `llmturns json <index>
[offset]` preserves full short replies and adds `len`, `next` and `more` for
long replies; fetch `next` until `more` is false. The command response remains
below 4 KiB. These host tests do not qualify device memory headroom or lenses.

### G2 reply reader and microphone shortcut

`test_g2_llm_reader.py --sanitize` compiles the production layout helper,
REBUILD wire encoder/parser and fragment sender. It covers six full body lines,
complete 16 KiB retained replies, UTF-8 stream boundaries, thousands of short
lines, stable/clamped paging, reassembly/CRC and the qualified 241-byte ATT write
limit. Connection, lifecycle or presentation changes stop remaining fragments.

`test_g2_llm_reader_runtime.py --sanitize` extracts the production worker and
intent handlers and named REBUILD helper. It checks full-width creation,
scroll/tap paging, busy holds, exact-run replacement, retry limits and stale
lifecycle/presentation cleanup, including replacement during encoding or ACK.
The settings-command harness additionally exercises the actual physical sender's
presentation check between ATT writes.
`test_g2_text_entry_start.py --sanitize` compiles the real entry implementation
and microphone adapter for manual defaults, opt-in startup, secrets, unavailable
mic fallback, terminal/replacement races and review-before-Done behavior.
These suites do not establish physical hold delivery on a glasses text page.

### Native G2 Terminal

```sh
python3 -B components/hardwareone/test/host/test_g2_terminal_protocol.py --sanitize
python3 -B components/hardwareone/test/host/test_g2_native_terminal.py --sanitize
```

The codec suite compiles the production protocol and fragmenter against fixed
wire fixtures, including nanopb's omitted-zero values, malformed messages,
UTF-8 limits and 512-byte text fragmentation. The adapter suite exercises the
production control owner against controlled speech, Terminal HTTP and transport boundaries.
It checks direct foreground question/approval cards without a separate waiting
cue, exact pending-question restoration after dismissal, display/overlay focus,
and expiry/revocation and stale-reply rejection. Focus reports received before
an Open/SessionChange ACK remain authoritative; navigating back never answers
a request. Host admission remains awaiting while native QUERY owns the screen.
Completed empty recordings reset native speech UI without closing Terminal or
sending an empty message; spoken-answer drafts restore the same question with
a fresh presentation ID. Delayed state ACKs, stale confirmations, retry speech
and genuine STT failures are covered separately.
Background-poll transport failures during recording preserve the microphone,
draft, view and confirmation gate on Wi-Fi and phone paths. Tests cover
completion-based 3/6/12-second backoff, `more` and queued admission respecting
that delay, successful recovery, and exhaustion pausing polls without sending
or discarding the long draft. Mutating-request failures retain their separate
no-replay behavior.
New and queued polls are deferred while recording or draining speech; an
already-running poll may finish. Tests check continued local heartbeats,
no new HTTP admission in either phase and resumption at confirmation/idle.
These checks are separate from physical Terminal display, microphone and gesture
qualification, recorded in `docs/G2_TERMINAL_WORK.md`. Terminal conversations
use host-issued session/view identities independently of the custom LLM reader.

### Phone HTTP transport and TLS

Run from the repository root, with `IDF_PATH` pointing at the pinned SDK and
the phone-pipe profile already configured:

```sh
python3 -B components/hardwareone/test/host/test_llm_http_transport.py --sanitize
python3 -B components/hardwareone/test/host/test_llm_http_idf_open.py --idf "$IDF_PATH" --sanitize
python3 -B components/hardwareone/test/host/test_llm_direct_connect.py --idf "$IDF_PATH" --sanitize
python3 -B components/hardwareone/test/host/test_llm_phone_tls.py --idf "$IDF_PATH" --sdkconfig build-config-p4-phone-pipe/config/sdkconfig.h --sanitize
```

The direct-connect test compiles production transport code with the SDK's actual
ESP-TLS connection state machine. Socket/DNS/handshake boundaries are controlled;
delayed secure/plain TCP, cleared select sets, socket errors, cancellation,
deadlines and pre-HTTP fallback are exercised. The separate HTTP open test uses
the pinned SDK's actual header-write path to verify the no-replay boundary.

The TLS test builds the pinned mbedTLS source with generated device configuration
and `esp_config.h`, substituting native hardware, allocator, entropy, time and
mutex hooks. It preserves protocol, certificate/date and ticket choices. Nine
certificate cases run at two fragment sizes over bounded queues. Expired/future
certificate expectations follow `CONFIG_MBEDTLS_HAVE_TIME_DATE`, including its
disabled state; the test does not impose a different host security policy.
Only explicit synthetic CAs are exercised, not certificate-bundle fallback.
Hardware crypto, complete direct/pipe certificate parity and live BLE remain
hardware qualification items. CMake, a native C++ compiler and OpenSSL are
required; temporary keys and native library builds are removed automatically.
These checks are registered in CTest under the `phone_pipe` label. SDK and
crypto suites additionally have the `phone_sdk` label. Select the pinned SDK
and generated device header explicitly:

```sh
cmake -S components/hardwareone/test/host -B /tmp/hw1-hardwareone-host \
  -DHW1_SANITIZE=ON \
  -DHW1_PHONE_TEST_IDF="$IDF_PATH" \
  -DHW1_PHONE_TEST_SDKCONFIG="$PWD/build-config-p4-phone-pipe/config/sdkconfig.h"
ctest --test-dir /tmp/hw1-hardwareone-host --output-on-failure -j2 -L phone_pipe
```

### Phone pipe interoperability fixtures

The Android port imports the JSON files in `tools/ble_secure/fixtures/`. Each
is generated by production code with fixed test entropy and synthetic content,
and the test compares it byte for byte. Ordinary runs never rewrite them; pass
`--write-vectors` to regenerate deliberately, then update the SHA-256 pins in
`test_phone_fixture_pins.py` (and in the app).

Phone link v2 (inner protocol v2; the outer HW1-PIPE-v1 channel is unchanged):

- `phone_secure_channel` builds `phone_handshake_vectors.cpp`, whose output is
  `phone_secure_channel_v1_pipe_v2_vectors.json`: its 14 handshake and key
  fields equal the frozen v1 golden, its first DATA each way is the v2 pipe
  HELLO (flags 1, console offered) and HELLO_ACK (flags 3).
- It builds `phone_session_vectors.cpp`, which drives the real
  `BLE_PhoneGatt.cpp` and pipe core through `phone_gatt_fake_sdk.h` and writes
  records through `BLE_PhonePipeConsoleCore.h`; the generator stands in only
  for the console driver's decisions. X1 v2 (`phone_pipe_v2_sessions.json`):
  sessions at MTU 64, 244, 247 and 517 with a relay stream interleaved 3:1
  with the console, every record type and OPEN address type, relay CLOSE 0
  and 1, LINK_STATE both ways, OPEN_ERR 16 with the console untouched, CLOSE 3
  after the last TEXT, CLOSE 1/2/4/5 cycles, ABORT, the console-only 60 s PING
  and the phone's PAUSED reset. A scripted phone answers and credits by
  Android's rules (listed in the file). X2 v2
  (`phone_pipe_v2_negative.json`): the adapter's outcome for negative and
  boundary vectors, including the version signal and mismatch strikes
  (`pipe_hello` stage), and `contract_synthetic` P4-to-phone vectors sealed
  with `phone_secure_peer.h`'s P4-direction sealer for the phone's own rules.
  Two `contract` vectors pin a production console CLOSE that crosses the
  phone's own end of the stream: CLOSE 4 after the phone's LINK_STATE 0
  crossed the OPEN it refused with 16 (`console_open_while_unwanted`), and
  CLOSE 2 after its stream RESET 1 (`console_close_2_after_reset_1`). The
  phone answers CLOSE_ACK: it takes a CLOSE 0..5 on an id it refused or reset
  and bounds the reason by class only on live ids (`refused_or_reset_ids`).
  Their frames are in P4 order (`frame_order`) and include the phone's crossed
  frames, so a runner picks each frame's key by its `dir`.
  The MTU-247 session reproduces the v2 golden, and the synthetic v1 P4 HELLO
  equals the v1 golden's first DATA.
- It builds `phone_console_vectors.cpp`: X4 (`phone_console_v2_records.json`),
  console record streams with the P4 writer's output and the phone's and P4's
  verdicts, and `code_vectors` for the pairing code (canonical and display
  forms, entry normalisation, every single-symbol error, PSKs). The test
  re-checks the record headers, the GF(32) check symbol and every PSK
  independently in Python.
- `phone_fixture_pins` (default suite, no compiler) pins the SHA-256 of every
  fixture: the frozen v1 files (`phone_pipe_v1_sessions.json`,
  `phone_pipe_v1_negative.json`, `phone_secure_channel_v1_vectors.json`,
  `secure_channel_v1_vectors.json`), X3 and the v2 files.
- `phone_passphrase` (X3, `phone_pipe_v1_passphrase.json`, unchanged by v2)
  takes the same text through the agreed Android entry rule and through the
  P4 serial path. That path runs the production String, command split,
  `phone secret` handler and PBKDF2, built with `-funsigned-char` as on the
  RISC-V target. Every text Android accepts must store identical bytes on the
  P4. The generator also asserts that the host C locale classifies exactly the
  target's ASCII-only sets, and the test asserts with `hw1code::isCode` that no
  X3 text (as typed, cut at NUL, without CR, or as stored) is a pairing code.

`ble_recycle_phone` runs the real `bleStackRecycleIfWedged` guard: a READY
phone with the glasses off blocks an accessory-wedge recycle, and a forced
client retirement requests the phone's reconnect.

Path switching and the OPEN phases are covered as follows:

- `llm_http_transport` runs the real selector. It checks Wi-Fi⇄phone fallback
  for each OPEN_ERR class and stale-READY outcome, that each path is used at
  most once, and that there is no fallback after an HTTP write. It also checks
  the phone hint's scope, probe and clearing, the A-1 budget handed to
  `phonePipeOpen`, and the F3 records and report text.
- `phone_runtime` drives the real `phonePipeOpen` loop with a scripted worker:
  2 s submission, 4 s from the last fragment, caller deadline, link loss,
  cancellation, no budget and local rejection. It covers the pipe task's two
  exits through the one `leaveIfReleased` step: the release on a stopped host
  (`bleStackDown`) and the idle exit on an up host, 2 s after a pass found the
  pipe stopped and idle (step Idle, GATT adapter idle, app id released, no job,
  scan, forget, pair window or reset pending) with no phone setup turn held;
  an app id, a job, an active pipe or a claimed turn keeps the task, and
  `phoneLinkOpen` publishes `active` before it looks for the task under the
  same lock. The same suite covers A-4
  per-connection parameters and the F3 session counters, and the phone's turn
  on the setup worker: claim-first admission and enrollment, one consumed
  grant, the Waiting backstop, every exit giving the turn back exactly once
  (at READY only once no scan flag is left), enrollment timed from the grant,
  the overrun revoke and escalation races (a timed-out cleanup's own recovery
  request is never lifted by the worker), the wait while a yielding ring still
  holds its link, and `wait_ms` from admission to grant. It also covers the
  app console's link record (`phonePipeLinkRecord`, S4). The record is set at
  READY with the session's generation and link id. It is cleared by every end:
  a command-task stop, link loss, `phone stop`, a replaced intent, a stream or
  callback-queue failure, host reset and quiesce. A new session never sees the
  last one's record. Every pass checks that a valid record means READY on this
  session's link, and the copy takes a leaf lock but never the owner mutex. A
  source check routes every `ready` write through `setReady`/`clearReady`.
  Console traffic (`phonePipeNoteConsoleActivity`, S8) is stream activity for
  A-4: it raises the active parameters and returns to idle 30 s after the last
  note. Traffic noted before READY never counts for the new link. One request is
  in flight at a time: an unanswered one blocks the next until its event or the
  45 s backstop, Bluedroid's own status-16 timeout is not an answer, and success
  is credited to the state that was asked for, only when the interval lands in
  its range.
- `ble_reconnect_schedule` runs the real `commitAdmission` through repeated
  attempts: the phone retries at 2/4/8/16 s, then 30 s up to attempt 24, then
  60 s for good, each timed from the attempt's end (`blePeerNoteAttemptFailed`,
  no intent bump); `blePeerExpediteReconnect` pulls the next try to now
  without an intent bump and is refused after a user disconnect; the phone's
  reseek windows (manual `openphone` 60 s, a restore 120 s) keep a one-shot
  retrying with auto-reconnect off until they end. The glasses keep
  5/15/45/90/180 s, and the ring follows faceclaw (2 s after a drop, then
  2/4/8/16/32/60 s, each step equal to the ring's own hold after that many
  failures).
- `ble_coex_records` runs the shared coexistence records
  (docs/WIFI_BLE_COEXISTENCE_STUDY.md, lever 1). It covers the Wi-Fi state
  classifier's priorities (scanning over associated, associated over a connect
  scope, the hunting heuristic, ESP-NOW parking) and the Wi-Fi span around a
  record; the callback tally under two racing threads; a scan's end tied to
  its own start, its duration source and the aborted/start-failed rules; the
  first-wins disconnect reason; per-peer counters; and the 16 newest-first
  records with their exact lines, worst-case widths and truncation. It also
  compiles the records' no-op stubs (`ENABLE_BLE_COEX_RECORDS=0`) against every
  declared function and checks the flag's wiring.
- `ble_coex_hooks` pins the hooks in the production source: each G2 and ring
  scan record opens after the epoch is claimed and before the radio starts,
  closes after the callbacks quiesce, and has no exit in between; each temple
  and ring connect is bracketed and carries its purpose; the passive GATTC
  observer is installed with the G2 GAP handler; the records hold no addresses;
  a user disconnect holds the pipe while it bumps the intent; and the Wi-Fi
  mirror registers on Arduino's event list.
- `wifi_coex_mirror` runs System_WiFi.cpp's mirror (the Arduino event handler,
  its install and `wifiCoexTag`) over fake events: registering once and
  retrying a failure, STA/AP/IP transitions, the hunting window, scan and
  connect counts, a down companion and `radiopower off`.
- `phone_scan` checks the reconnect scan's profile parser (including the scan
  length).
  `phone_runtime` runs the pipe's own use of it (firmware-007):
  - the real GAP callback with 50 pipe reports and 200 others in one scan;
  - the stop at the first match, and a stop that crosses the scan's own end;
  - scan events that are not the pipe's own (ignored);
  - a refused or failed stop, which no longer leaves a flag pending;
  - the deadline of the scan length plus 1 s;
  - found, missed, aborted and start-failed records with the Wi-Fi span,
    handed to the log after the pass that made them;
  - one attempt record per scan that found the phone: the adapter's stage,
    status and reason; each cancel source; precheck and register attempts;
    READY; Service Changed; the wait for its turn; a stop from a command task; the
    10 ms blind spot when a cancel and a link failure share a tick;
  - `phone scan` (six scans and six attempts within the reply limit), the
    `phone` and `relay` handlers (validate pass, local-admin gates, the split
    between `phone stats` and `relay status`), and
    the setup-time breakdown.
- `ble_peer_registry` registers the builtin phone, then the pipe's real spec, as
  boot does. The pipe's spec must replace the builtin in place, and boot
  reconnect and the tick must reach its admission.
- `phone_gatt` compiles the `sizeof(Gatt) <= 9728` assert (the stub
  `StaticQueue_t` is padded to the P4's 92 B), and drives the v2 HELLO, the
  version signal under a v1 or v2 header, the silent-HELLO latch (link loss
  within 1.5 s, or the 3 s deadline) and both pressure inputs through the real
  adapter. It covers A-5 expiry attribution and the MTU phase as IDF 5.5.5
  runs it: one Exchange MTU, queued behind automatic discovery, then a 2 s
  answer deadline (10 s without discovery), both across the clock wrap. An
  authenticated frame the pipe refuses keeps the core's cause (a phone RESET's
  code), not Unauthorized. It also checks the per-phase setup stamps and the
  attempt's end latch: the first stop's stage (register, open, discovery
  including a failed or unreported one, mtu, search, subscribe, cccd), the
  first non-OK status, the phone's own disconnect reason (another link's copy
  ignored) and one-shot latching.
- `ble_setup_worker` runs the one BLE setup worker's scheduler
  (`BLE_SetupWorkerCore.h`, docs/BLE_SETUP_WORKER_PLAN.md) as is, under
  ASan/UBSan and ThreadSanitizer: the turn order (glasses, ring retirement,
  phone, ring; a ring passed over by the phone runs next), the pass-over bound (measured over a random schedule),
  token-matched picks when a job is withdrawn and resubmitted during `ready()`,
  exactly-once finish under a two-thread withdraw race, no allocations, wait
  accounting (G2-audio time paused, never reset, by other waits and by another
  client's run), the recorded wait, and the phone's turn (grant after the
  priority raise, revoke at 75 s, the 10 s local stop, escalation and its
  settling). Its text checks pin the FreeRTOS wrapper (task name, stack bytes,
  priority, the ops table order and each client's record flag) and that the old
  connect worker, its BARRIER and the setup gate are gone.
- `ble_console_attach` runs the app console's attach decisions
  (`BLE_ConsoleAttachCore.h`) as is: in G2 client mode the command service
  rides the phone pipe's link, and only the authenticated, READY pipe's link
  becomes a console slot, found by the pipe's live connection id and keyed by
  its generation. It runs every input combination (mode, setting, secret, pipe
  record, slot, closed generation) for reconcile and for a link coming up or
  down: server mode keeps its path, client mode never admits on a connect, only
  a slot on that very id ends, a fixpoint after one pass, and a connection id
  reused by another link never inherits a slot. Named sequences cover the phone
  link up before and after the console is enabled, temple and ring traffic, a
  generation or id change, detach and re-attach, a removed secret, a failed
  pipe, a missed disconnect and the host torn down. `join` (the app's first
  admitted write) marks only the slot on its id, once; reconcile keeps a joined
  slot and makes every new one unjoined; `bledisconnect` ends only a joined
  slot on its id and the next reconcile makes a fresh one. A random walk
  over the four C6 links (the pipe publishing while its link is up, or just
  after a drop that races READY before the id is reused, and clearing late)
  checks after every step that the slot sits on the very link its generation
  ran on (or its dead, unused id), and after every reconcile that a live record
  has its slot. The text check keeps the core free of BLE, OS and Arduino
  includes.
- `ble_console_phone_link` runs the shipped wiring of that core in
  `BLE_Server.cpp` (server callbacks, the request characteristic's write,
  the console plan helpers, the directed senders, the console view,
  `bledisconnect`, `bleadv`, `bleconsole`, the host-teardown hooks and
  `bleUpdateStreams`) with `BLE_LinkStatus.cpp`'s `bleLinkStatus()`, over
  recording doubles of the pipe's link record, the Secure Channel, the central
  TX gate, transport sessions and the Arduino GATT objects. A table over every
  link id shows the temples and the ring never become a slot, whatever events
  and writes they see; reused ids never inherit one (a later link on the id, a
  missed disconnect, READY racing the drop, a generation change). It covers the
  main loop reconciling before its no-client return, the first write's BTC
  fast path (setting, blesecret and admission respected; a session that could
  not open is retried), every write reconciling first (a slot whose pipe left
  READY, or whose setting or secret went, takes no command before the next
  pass), a pipe READY with no app being no connected app (no count, state or
  event, none when it ends) until the app's first write joins it, Secure
  Channel only (plaintext refused with the fixed notice, never output), each
  console frame as one central TX gate try before the lifecycle lock, posted
  with that lock released and counted in flight (BTC_TASK takes the lock for
  the phone's writes, and the post waits forever on a full BTC queue), noted as
  pipe activity, `bledisconnect` ending a joined session without a GATT close
  (a session no app joined is no client; its hint reply falls back when the
  debug buffer was never allocated), `bleadv` refusing,
  `bleconsole on|off|status` with its persisted setting row, the status read
  answering the slot's link only, the teardown ending the slot, resetting the
  attach state and waiting for a post in flight, server mode unchanged, and the
  console words on the status surfaces. A random walk over the four C6 links checks after every step that
  only the pipe's link holds a slot or gets a write dispatched, with no
  address, advertising call or GATT close. Source checks keep the BTC-side
  helpers free of logging, waits, advertising, MTU and GATT open/close calls
  and pin the call order in the callbacks, the raw sender, the main loop, the
  registration and the teardown.
- `ble_pipe_console_row` runs the phone link's console row (phone link v2: the
  app console as stream slot 2 of the phone pipe) from `BLE_Server.cpp`: the
  session table and epoch helpers, the row's lifecycle (gBleState holders,
  bind, end, the leaf identity snapshot, the main loop's reconcile), the auth
  rule, revocation, the disconnect owner functions, the command tail, the
  record ingress (`blePipeConsoleInbound`), the output router (TEXT, MIRROR,
  CARD, BLOB) and design A's slot helpers, with `System_OTA.cpp`'s
  `currentSecureBleConnection`, over recording doubles of the console driver
  (`ble_pipe_console_host.py`, shared with the suites below), 12 transport
  sessions, the Secure Channel and the executor. The row binds signed out on its
  own id and the snapshot follows every login, logout, revoke and end; it must
  log in even with `blerequireauth` off (never AuthBypass); design A's slot and
  the row on the same link, signed in as different users, never authorise each
  other (stamp, whoami, the OTA superadmin check, confidentiality,
  revocation, `bledisconnect`); revocation ends the row and asks for CLOSE 5,
  `bledisconnect` for CLOSE 2; COMMAND records run as encrypted lines and
  BINARY records reach OTA after the activity stamp, fenced on their epoch;
  TEXT is reliable only from cmd_exec, MIRROR goes in records of at most 512
  bytes, CARD is the bare JSON object and BLOB needs a confidential session;
  start/stop cycles return every transport session; and no call into the pipe
  runs under the lifecycle lock. `ble_server_stop` runs the holders against the
  real registration and teardown hooks (the client host joins a pipe-held state;
  a host teardown ends the row before the stop and the pipe's hold keeps the
  state), `ble_sc_policy` the row's whoami and gate with `blerequireauth` off,
  and `fileread_psram` the BLOB path's confidential check and epoch fence.
- `ble_session_idle` runs the BLE idle logout (`bleSessionTick`) from
  `BLE_Server.cpp` with login (`bleBindSession`), the activity stamp
  (`bleMarkActivity`), the phone-link slot maker and `System_User.cpp`'s idle
  policy. A contender double stamps a command whenever the tick takes the
  lifecycle lock, as cmd_exec can while the main loop waits for it. A
  phone-link session with commands at +60 s, +120 s and one landing during
  the tick at +180 s stays signed in. After 15 minutes idle it expires once,
  with the notice sent on the new session. In server mode, one session's paced
  expiry notice does not expire another whose command lands during the send.
  An OTA upload in flight outranks the window, and 0 disables it. The phone
  link's console row sends its notice as one TryNow record from the main loop;
  a full stream keeps it pending, offered each tick while that session lasts,
  and a new session drops it. Source checks
  pin the command and OTA-frame stamps and keep every `lastActivityMs` writer
  under the lifecycle lock.
- `ble_command_output` runs a Bluetooth command's own `broadcastOutput` lines
  to the session that sent it: `broadcastOutputCore` from `System_Debug.cpp`,
  the help menu (`renderHelpMain`) from `System_CLI.cpp`, and the command tail,
  reply callback, output batch, senders and mirror collector from
  `BLE_Server.cpp`. In server mode and on the phone link, `help` and `help all`
  answer the session with the menu in whole-line batches of at most 512 bytes,
  then "OK"; the two modes send the same messages, and the phone link sends
  them as reliable gated frames. Another session gets nothing, a JSON result
  stays one byte-exact message, an over-long line keeps its start with a
  `[CUT]` mark, a mirror recipient with `outble` on is not sent the lines
  twice, a session that ends during the command gets nothing more, and lines
  from outside a Bluetooth command and the send traces never reach a session.
  A line printed while the command holds the lifecycle lock (the production
  `BLE_DEBUGF` in `bleIsAuthed`) joins the batch when it fits and never sends
  it under the lock: the Secure Channel sender and the phone link's post run
  with the lock released. On the phone link's console row `help` and `help
  all` arrive as reliable TEXT chains with the same messages, and a line
  printed while the task holds the pipe owner or console chain mutex never
  flushes the batch. On the console row a batch whose send fails drops the
  command's later batches, and the result is still a reliable write (the
  driver's stall latch makes it one try). A server-mode session, plaintext or
  Secure Channel, and design A's slot whose GATT post is refused for one
  batch's six tries lose that batch only: the later batches and "OK" still
  arrive. A source without the batch still compiles and fails
  the delivery cases.
- `ble_role_switch_reconnect` switches to server mode with `openble` and back
  to the glasses with `openg2 saved`. It runs the reconnect scheduler and tick
  from `BLE_Peers.cpp`, `openble` from `BLE_Server.cpp`, the ring's disconnect,
  logical down, admission and `ring disconnect` from `G2_Ring.cpp`, the
  glasses' disconnect intent, admission, `openg2` and `g2ConnectSaved` from
  `G2_Glasses.cpp`, and the phone pipe's peer ops. The role owners and the
  pipe's worker are fakes in the production order, which text checks pin. A
  role switch is not a user disconnect: the ring and the phone pipe come back
  after `openg2 saved`, and ten minutes of server mode start nothing (no client
  host either). A ring the user disconnected, before `openble` or during server
  mode, stays down. With auto-reconnect off, a ring or phone pipe that was up
  comes back once and a ring that was down does not. A boot in server mode
  followed by `openg2 saved` arms the ring and the phone, and a refused
  `openble` lets the glasses and ring come back on the running client host.
  `--component <dir>` reads production from another copy.
- `ring_phone_yield` runs the real `BLE_SetupWorker.cpp` and the glasses and
  ring clients from `G2_Glasses.cpp` on a virtual clock, with the ring's
  submitters, admission, disconnect and yield from `G2_Ring.cpp` and the real
  coordinator. It covers the refusals while the ring gives way to the enabled
  phone pipe below four links; the turn order, with a phone turn never
  cancelled by a glasses or ring submit; cancels, faults, the recovery block
  and the yield ending a queued job at the next 100 ms recheck with one finish
  and the family flag released; role-transition waits; the one-shot reseek put
  back for a yield drop; repair outcomes; the ring's 90 s G2-audio wait; the
  recorded wait; withdraws from `g2Disconnect` and `g2RingDisconnect`;
  preemption of a ring scan; ring retirement (queued only with a ring client
  and the G2 client); the in-flight watchdog restamped at the job's turn; and
  the polled `g2RingScan` with a fake BLEScan (other-queued cut, stop requests
  kept from the job's start and acted on only by the scanning job, lost end
  event, outcomes). Text checks cover the teardown order in `deinitG2Client`
  and that every host teardown after a glasses session drops the ring's client
  first. `--component <dir>` reads production from another copy.
- `phone_pipe` checks the routing core (`Attempt`, `openRefusal`, `Health`),
  the acceptance time of the last OPEN fragment and peer-ended streams, and the
  inner protocol v2 core: slot classes (relays 0..1, console 2 with its exact
  OPEN), HELLO/HELLO_ACK flags, LINK_STATE (on change, one per second,
  overwritten while unsent, strict on receive), errors 16..18, CLOSE reasons
  by class, `closeAfterDrain`, console credit consumed+4096 with `rxPeek` and
  `consume`, whole-record pushes with the ABORT reserve, the 12-message control
  queue (worst legal case exactly 12, the 13th is Resources, F-3 CREDIT purge),
  R R R C scheduling counted at accept, a chat upload under 5 s beside a
  saturated console (with and without relay pressure), a minute of display
  pressure, the PING split, wiping of rings, `packet_` and `assembly_`, and the
  version signal and silent-HELLO latch. It prints the host `sizeof` values.
- `phone_console_codec` runs the phone link v2 console records
  (`BLE_PhonePipeConsoleCore.h`) as is. It pins the header bytes (type, flags
  with bit 0 MORE, len little-endian) and runs the P4 ingress validator over
  every type, flag pattern and boundary length: COMMAND 1..511 and BINARY
  13..489 starting with the OtaBleWire magic are delivered; the reserved
  RESUME and unknown types are skipped (0..1024, no MORE); a P4-to-phone type,
  MORE outside TEXT/BLOB, flag bits 1-7, a bad length or magic is malformed.
  Long streams split anywhere (one byte at a time included) read back exactly
  the records sent, and a malformed record shows exactly when its offending
  byte arrives. Scans get only the bytes they may read, so ASan proves the
  peek bound. The P4 egress chain machine is run at the cap (a 4608 B chain
  passes; 4609 B, or MORE on the record reaching 4608 B, does not), with no
  interleaving inside a chain and ABORT only while one is open. The phone-side
  receiver adds INFO first and once and ignores a stray ABORT, and a random walk
  shows the writer and the receiver agree record by record. The INFO payload is
  pinned byte for byte with its escaping and 64-byte field cut; the driver
  parses every printed INFO as JSON and checks the key order. Text checks keep
  the header pure and tie BINARY to `System_OTA.cpp`'s magic and frame size.
- `phone_pair_code` runs the `phone pair new` code (`BLE_PhonePairCode.h`) as
  is: the GF(2^5) field (x^5+x^2+1 primitive), every single-symbol
  substitution and every swap of two different symbols of 300 generated codes
  rejected (exhaustive over positions and values), entry normalisation (outer
  whitespace, case, `-` and space anywhere, I/L read as 1 and O as 0, U and any
  other byte refused, exactly 16 symbols), a failed parse leaving no bytes
  behind, generation from three random words and the display form. The driver
  recomputes the printed vectors with its own field arithmetic.
- `phone_console` runs the console driver (`BLE_PhonePipeConsole.cpp`) as
  shipped, with the real pipe core, record layer and code headers, against
  fakes of the headers it includes; the fakes carry the real declarations
  extracted from `BLE_Server.h`, `BLE_PhonePipe.h` and the rest, so a changed
  signature fails to compile. The harness co-simulates the pipe task, cmd_exec,
  the main loop, the debug task, httpd and a phone on one clock. It covers
  CONSOLE_OFFERED (bleconsole, the generated flag and a valid code), OPEN and
  INFO first (before any writer), the reopen table (CLOSE 1 and stream RESET
  back off 2, 5, 10 s and then wait for a flag change; 2 and 5 at once; 3 when
  offered; 4, OPEN_ERR 16 and the phone's CLOSE on a CONSOLE_WANTED rise,
  and while wanted still reads 1 also quiet retries after 2, 5, 10 s, a step
  spent only when its timer reopens, so a coalesced 1->0->1 dip never wedges
  the console; OPEN_ERR 17 never again; OPEN_ERR 9 three backoffs then never; link loss
  until the next READY; a command resets the ladder), ingress records to
  cmd_exec with credit withheld and nothing dropped or counted while the row
  is unbound, the pool is full or cmd_exec has under two places, malformed and
  late partial records (CLOSE 1; a record the phone lacks credit for is never
  late), unknown types skipped, the close mailbox (CLOSE after queued output),
  a lost row or refused bind, the 4 B ABORT reserve, Reliable on cmd_exec only,
  TryNow drops counted, the 10 s eligible stall and 15 s ceiling with display
  pressure, ABORT only with an open chain (epoch change, owed ABORT, none once
  draining), no CARD or MIRROR inside a chain with two writers, and pool slots
  wiped on release, close and link loss. Lock doubles enforce the lock order on
  every acquisition. A stored code without
  `Settings::phonePipeSecretGenerated` (typed with `phone secret`) is never
  offered, and text checks pin the PSRAM pool, the source entry and no logging
  on chain paths.
- `phone_pair_sink` proves `phone pair new`'s code reaches exactly one sink. The
  command and its pairing window run as shipped on the `phone_runtime`
  co-simulation (built with `HW1_PAIR_SINK=1`), with the real one-shot serial
  sink from `HardwareOne.cpp` and the code-screen sink from `OLED_Utils.cpp`;
  every output path (Serial, `broadcastOutput`, the debug queue, the OLED
  console, events, every log level) is a recording stand-in. On serial the code
  is one `Serial.write` of the whole line after the output drain and appears
  nowhere else, the window's attempts included; the sink refuses a non-serial
  origin, serial output off, validate-only, a session that is not live or ends
  during the drain; a short write ends the window. On the OLED there is no
  Serial call, one code-screen request, and the code only in the window the
  screen reads. `phone_runtime` runs the command's refusals (all before any
  change), the window's 2 s re-arm with the setup turn free in between, the auth
  cooldown, and its ends (READY, 3 min, B, phone stop/forget/pair new, the
  session); `phone_secret` and `phone_passphrase` the code entry and the
  device-made mark; `oled_bluetooth_link_watch` the menu entry and code screen.

The label includes the existing native Terminal codec/adapter and HTTP worker
regressions, including offline confirmation, the 15-second Busy limit, slot
retirement and completion-based polling. It also includes pipe protocol,
configuration, coordinator, runtime, forget, enrollment, secret, SDK lifetime,
recovery, ring-yield and R1 protocol suites, plus the Arduino manifest/checker.
No firmware build is run by CTest. The suites compile their own small host
harnesses and remove temporary fixtures when they finish.

With no SDK options, CMake reports which SDK/crypto tests are omitted and still
registers the dependency-free group. `HW1_PHONE_TEST_IDF` adds native GATT,
secure-channel, HTTP-open and direct-connect tests. `HW1_PHONE_TEST_SDKCONFIG`
also requires that SDK and adds actual-config TLS parity. Invalid explicit paths
fail configuration. Crypto suites require the vendored libsodium source under
`managed_components`, Make and a native compiler; each owns a disposable build.
The removed SMP `phone_identity` suite is not registered. Native QuickNote was
already registered as `g2_native_notes`; it remains separate from this label.

### Central link capacity and stuck-phone recovery

```sh
python3 -B components/hardwareone/test/host/test_ble_central_coordinator.py --sanitize
python3 -B components/hardwareone/test/host/test_ble_central_stack.py --sanitize
python3 -B components/hardwareone/test/host/test_phone_runtime.py --sanitize
python3 -B components/hardwareone/test/host/test_phone_gatt.py --sanitize
python3 -B components/hardwareone/test/host/test_phone_recovery.py --sanitize
python3 -B components/hardwareone/test/host/test_ring_phone_yield.py --sanitize
```

The coordinator suite runs the production reservation owner with concurrent
callers. It checks two/three/four-slot limits, pending reservations, duplicate
owners, stale token release, allocation rollback, arm reservation, ring yielding
and recovery admission blocking. Who sets up next is the setup worker's call
(`ble_setup_worker`, `ring_phone_yield`, `phone_runtime`). Source-order checks additionally
keep accessory capacity refusal before client creation and connect dispatch;
the phone runtime exercises refusal before scanning or GATT registration.

The runtime test keeps a stuck phone quarantined past the four-second cleanup
deadline, verifies that the setup turn is given back and new admissions close, and
retains callback storage/capacity until terminal host-stop acknowledgement. The
native GATT suite separately exercises lost CLOSE/REG completions and late events.
The recovery suite executes the production role-owner transaction with controlled
HAL/worker boundaries: standalone and glasses roles, quiescence refusal, failed
teardown/acknowledgement/restart, changed user intent, logging before restart,
one attempt per incident and ten-minute restart spacing. Passing host checks
does not establish controller/radio recovery on hardware. Registration remains
part of the staged test-registration work.

The ring policy regression runs the production yielding function with the shared
coordinator for pipe-compiled and pipe-absent cases. Three-slot capacity admits
both arms plus ring while the pipe is off. Enabling the pipe closes ring admission
and requests a non-user disconnect; an active writer or unretired native route
keeps its slot reserved until safe retirement. Disabling the pipe restores ring
admission. Four-slot capacity keeps the ring when the pipe starts. Runtime-owner
checks cover publication on start and clearing on normal stop/terminal teardown.

### Runtime memory diagnostics

`test_task_heap_measure.py` extracts the production walker and tests exact and
P4 coprocessor-shifted stack bases, nested DMA-pool attribution in either
traversal order (including a freed child), exclusive block ends, freed blocks and
address arithmetic under ASan/UBSan. `tools/p4/test_ram_monitor.py` tests the
live graph's numeric whitelist, malformed samples and private-log redaction.
CTest names: `task_heap_measure`, `p4_ram_monitor`.
`heap_reader_sources` holds source contracts for the internal-heap readers that have no harness of their own: the
BLE system stream, the task-create debug line, the DEBUG_MEMORY task report, `memsample` and the Tier 1 pressure
watch read the byte-addressable internal heap (`System_MemUtil.h`) and take the low-heap line, the fragmentation
figure and its warning line from `System_HeapLowCore.h`; `heap_low_core_tests` tests the arithmetic.
`known_task_table` covers the table of tasks created under a fixed name and
stack size. The DEBUG_MEMORY 60 s task report (`reportAllTaskStacks`) and the memory monitor's
periodic stack sample each kept a copy with a parallel list of "alive" flags;
the monitor's copy had lost the `espnow_tx` row. The one table is
`collectKnownTaskRows()` in `System_TaskUtils.cpp`. The test compiles that
function with `KnownTaskRow`, `kKnownTaskMax` and the stack constants of
`System_TaskUtils.h` against stand-in handles and running flags, with and
without ESP-NOW: every row is returned, a row is alive exactly while its owner
says so (a stale handle is not), each row carries its own stack size, the
ESP-NOW rows carry the watermark their owners sampled, a short buffer is not
overrun. Source contracts: both reports walk the owner's rows, neither keeps a
second table, a flag list or a test on a row's position, and every row name is
a name a task is created with (`xTaskCreate*` or `taskStackRecord`). Not
covered: the two reports' output and the live FreeRTOS queries.
The phone runtime suite additionally exercises the production session storage
construction with external BSS enabled and disabled. For phone link v2 it runs
the real v2 Session behind the fake adapter, with a fake phone that answers
HELLO, PING and LINK_STATE. It covers one parked stream id per slot (cleared at
GATT begin and host reset), end reasons 0..18 with unknown codes in 0, the
CONSOLE_OFFERED flag set before the poll (the HELLO carries it, a change sends
LINK_STATE), the console driver's pass after the step machine under the owner
mutex and its row work after it (a stopped pipe included), the display-only
console yield, RELAY_ON from HELLO_ACK and LINK_STATE (cleared with READY; no
OPEN without it), an idle console stream keeping idle parameters, the console
writer (`phonePipeConsolePush`), the status snapshot, version-mismatch strikes
(the version RESET, a link loss within 1.5 s, the 3 s HELLO deadline), the hold
after three (ROLE_BLOCKED, the ring's yield lifted) and its lift by `phone
start|connect|pair` or READY, the gBleState hold given back on stop and host
reset, and 10 min of READY with RELAY_ON 0 (one link PING a minute, idle
parameters). Native Terminal tests
cover speech failure during capture and after release, retained view, discarded
partial draft and late confirmation without submission.

### Speech internal-memory admission

`python3 components/hardwareone/test/host/test_stt_internal_memory.py --sanitize`
compiles the production bounded allocation ledger and task-filtered heap hooks.
It checks overlapping allocation lifetimes, in-place realloc, unrelated tasks,
PSRAM/ISR exclusion, incomplete ledgers, cancellation, nested scopes and disabled
hooks. `test_quartznet_cache.py --sanitize` covers admission with ample total
SRAM but an insufficient contiguous internal block, plus recovery.

The P4 speech profiles opt into `CONFIG_HEAP_USE_HOOKS`. This is not heap task
tracking: it adds no owner header to every allocation and does not change the
Task Manager's heap walker layout. Only the active transcribe task is measured;
other tasks/ISRs and PSRAM allocations do not enter its ledger. Bounds retain the
conservative cold/resident floors until hardware qualification. The ledger is
bounded and never allocates in a hook; overflow invalidates learning. ESP-IDF's
moved-realloc hook does not identify the old block, so retained old entries can
only overestimate lifetime, not justify reducing a reserve.

`phone_gatt` includes accepted `ESP_GATT_CONGESTED` completions during handshake
and encrypted fragmented HELLO, no replay, congestion pacing, and transport
failure without authentication cooldown. `llm_http_transport` checks readable
failure categories and credential-free memory diagnostics, routes relay users
on READY with the phone's RELAY_ON (path choice, fallback, admission), maps
refusal 16 to Network and 17 to Protocol and counts refusals per code 0..18;
`llm_http_runtime` checks the Terminal path with the relay on and off; `llm_direct_connect`
checks that socket resource exhaustion is not misclassified as network failure
and cannot trigger fallback.

`llm_http_runtime` also runs the kept Terminal worker (`g2_terminal_http`) as a
real thread that takes turns with the test: it runs only when woken and hands
the turn back when it parks in `ulTaskNotifyTake` or leaves through `vTaskDelete`.
It pins one task and one run for many requests, the run all zero after every
request, no stale body or response, a start staged while the worker is between
its last request and its wait, the rest of the idle time after a wake with
nothing staged, the exits (idle, `httpTerminalReleaseWorker` while parked or
mid-request, configuration withdrawn or changed while parked or mid-request),
refused starts that leave a staged run alone, and the old Busy answers for no
run memory and a failed task start. `--component <dir>` builds another tree
(an older commit) with the same harness. `g2_native_terminal` pins that the view
asks the worker to leave at every close and right before speech starts.

## Android handshake fixture and timing

The registered `phone_secure_channel` suite compares the production responder,
key schedule and AEAD with
`tools/ble_secure/fixtures/phone_secure_channel_v1_pipe_v2_vectors.json` (whose
handshake fields equal the frozen v1 golden). The generator
replaces only entropy in its host translation unit; no test hooks enter firmware.
It checks the first encrypted pipe HELLO/HELLO_ACK and accepts secure-counter
gaps while rejecting regression. Use `--write-vectors` explicitly to regenerate.

`phone_pipe`, `phone_gatt` and `ble_central_coordinator` cover the shared 2 s
per-stream relay pressure budget, controls bypassing deferred CREDIT, relay
round-robin DATA, clock wraparound and the non-bypassable controller mutex.
`llm_http_transport` covers the 4.5 s deadline from OPEN_OK, including worker
wakeup, TLS setup and a handshake call completing at/after expiry. The real
`llm_phone_tls` certificate suite uses the production timing constants.

## OLED and P4 layout capacity

`oled_layout` executes production list rendering/navigation and map page packing
at 64- and 128-pixel logical heights. `oled_transcription` executes its shipping
mode at both heights, including additional text rows and scroll boundaries.
The P4 pixel/lifecycle suites cover the full 240×240 output, sixteen canvas
pages, inversion, DMA stripes and retained-canvas power/retry behavior.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host --output-on-failure \
  -R '^(oled_layout|oled_transcription|p4eye_display_pixels|p4eye_display_lifecycle|display_lifecycle)$'
```

`test_oled_layout.py --sanitize --preview-dir /tmp/hw1-display-preview` emits
layout illustrations using a graphics test double and the shipping bitmap font.
Its optional `--baseline <component-directory>` compares OLED launcher and
simple-list framebuffer bytes against saved sources. These are host checks,
not physical display qualification.

## Waveshare integration

`touch_input` exercises the production contact queue, hit testing and UI handoff.
`battery` covers the shared battery policy, backend failures and OTA admission.
`radio_legacy_update` runs production Hosted admission and OTA chunking with
controlled RPC results: normal boot remains strict, legacy OTA requires a remote
reply, update-only sessions are unavailable to radio users, and failed chunks
stop the transfer. These tests do not qualify physical I2C, DSI, audio or radio.

## P4 SDK bootstrap

`tools/p4/test_prepare_sdk.py` (CTest `p4_prepare_sdk`, readiness G-26) runs a
copy of `tools/p4` in a temporary fake checkout. On an empty cache both pinned
registry packages are reported missing; `fetch_p4_packages.py` writes a
throwaway project pinned to exactly `espressif/esp-dl ==3.3.12` and
`espressif/esp_hosted ==2.12.13` and runs `idf.py ... -DIDF_TARGET=esp32p4
update-dependencies` (a stand-in registry, nothing is downloaded), after which
the prepare scripts find the fetched trees without `--source`; staged or local
packages fetch nothing; a registry failure names the offline `--source`
remedy; a fetched tree off the pinned digest is refused. `check_idf_pie.py` is
fed the before and after `portasm.S` text of the tracked b25cb29 patch:
present, missing (with the `git apply` command), partial (q3 save without
restore) and unknown (no RISC-V port). `prepare_sdk.sh --check` runs all four
override checks instead of stopping at the first and always prints the PIE
state. A configure failure after `update-dependencies` has downloaded both
packages is not a registry failure when the trees match their pinned digests.

`test_gen_build_info_idf.py` (CTest `gen_build_info_idf`) runs
`tools/gen_build_info.py` on fake build directories: BUILD_INFO.md names the
ESP-IDF tag and commit, lists the tracked IDF paths that differ from it (a
locally patched 5.5.5 shows `portasm.S`), adds the PIE-fix state line for
esp32p4 only, and reports an IDF that is not itself a git checkout as unknown
rather than the HEAD of a repository it happens to sit in. An IDF under the
home directory is shown as `~/...`.

## ESP-NOW review fixes (2026-11-17)

Tests added with the small fixes that followed the ESP-NOW architecture review. Each compiles production code
against host doubles unless it says "source"; each fails on the tree before its fix.

`espnow_dispatch_policy` (extended) models a second paired peer: every bond opcode from it is dropped, from the
configured partner accepted, and with bond mode off or no partner configured dropped (`isBondPeerMac`, the
`REQ_BOND_PEER` table flag). It also pins the HEARTBEAT row's `REQ_AUTH_WHEN_KEYED`.

`espnow_rx_frontend` runs the production `v4_try_handle_incoming` with the duplicate rings, the opcode table and
dispatcher, the real `v4h_heartbeat` and `v4h_relay_data`, the flood forwarder and the peer slot owners. A forged
plaintext frame carrying a victim's origin and msgId no longer makes the genuine sealed frame drop; flooded and
relayed duplicates are still suppressed; a frame whose fragIndex does not fit its fragCount is refused; on a keyed
mesh a plaintext heartbeat is dropped and a tagged one accepted; an unknown MAC, or a captured tagged heartbeat
replayed from an invented MAC, gets no slot; unpair frees the metadata slot; a ninth unpaired MAC does not block a
paired peer's metadata.

`espnow_keyex_hello` runs the production KEY_EX_HELLO handler on the production peer-identity cache: fifty replayed
HELLOs cause one identity file write and rate-limited replies; the initiator's own retries are answered; a changed
key or mesh is still written; lastSeenSec is refreshed at most hourly and never with an unset clock; a failed write
leaves the cache as it was; two HELLOs for two meshes 30 ms apart are both answered. A source rule keeps
KEY_EX_REPLY from kicking SESSION_OPEN over an ACTIVE session. `espnow_session_rekey` (extended) covers
`espnowSessionOpenInitiate`: a re-kick during ESTABLISHING or REKEYING leaves keys and state untouched, a kick from
FREE or CLOSED starts a handshake, a timed-out ESTABLISHING can be retried. The KEY_EX_REPLY handler runs too: a
REPLY to a key exchange an unpair called off stores nothing, so the unpaired peer's identity does not come back,
and a new key exchange afterwards is stored as usual.

`espnow_file_transfer` compiles the receive slot table, the FILE_* handlers, the sender pump and the commands
that report a send (since 1.0 step 7 the transferId layout and 25 cases; see that section): a send without an ACTIVE session is refused and every caller says so; `espnowsend`
re-reads the send status after its history record is written and stamps a state that resolved during the send
(peer, msgId, delivered), never a pending one; a send whose
handshake cannot be started fails instead of saying "try again"; a busy slot table never cancels a healthy
receive; release and abort do no filesystem work under the slot lock; an abort under lock contention does not pin
the slot. Since 2026-11-18 its pending store holds up to 4 frames per peer like the firmware: `espnowsend` before
the session opens answers "Encrypted message queued until the session with <device> opens (ID: ...)" (json
`"queued":true`), four in a row all go out in order when it opens, and a fifth is refused with "Error: 4 messages to
<device> already wait for the session to open. Retry in a few seconds." (marked failed, not stored as sent).

`espnow_command_privileges` (source) pins the admin flag on any ESP-NOW test command that changes live state, and
that the commands the 1.0 clean-out removed (`teststreams`, `testconcurrent`, `testcleanup`, `testfilelock`,
`bondtestsensor`, `espnowpair`, `espnowhbmode`, `espnowheartbeatbroadcast`) have no row or handler and the setting
`meshHeartbeatBroadcast` is gone. `espnow_history_depth` compiles the two `#if` blocks that set the message
history budget (default, maximum and total KB per ring, refs per on-device view) and the web poll: they follow
`CONFIG_SPIRAM`, so the ESP32-P4 gets the PSRAM budget; it also checks that ESP-NOW start allocates no history,
decides the budget from the PSRAM found at runtime and says it in one line.
`settings_onoff_commands` runs four `<0|1>` setting commands: every word the shared parser accepts, any other word
refused, side effects kept. `espnow_user_credentials` (extended) refuses a remote command whose user name or
password does not fit.

`espnow_loop_not_paused` (source) keeps `gMeshActivitySuspended` out of the tree and the ESP-NOW loop's early
return free of outside switches. `espnow_remote_ack` compiles the acknowledgement of `espnowremote`: a long
command shortens the echo, never the request id; a command still waiting for the session (2026-11-18) answers
"Queued '<command>' until the session with <target> opens. ..." with the id still last. `oled_espnow_remote_actions` runs the OLED Device Config remote
rows: they fill in and open the remote form, and no OLED ESP-NOW action sends a command line whose first word is
not a registered command. The remote form's Command field holds the longest command a Device Config row can fill in
("espnowfriendlyname" plus 47 characters): opening the field and confirming it unchanged keeps the whole command;
User and Pass keep 64 characters. The form shows a refusal that leads with "Error" as its reason and any other refusal as
"Remote not sent". Outside ctest, `tools/webui/tests/test_espnow_remote_page.py` feeds the firmware's real
replies to the web page's remote file browser.

`espnow_mesh_slots` runs the production `espnowmeshes` subcommands, the meshes settings writer and reader (with the
vendored ArduinoJson), the slot 0 bootstrap, the passphrase messages and the device-name setter `devicename` with
its hand-on `deviceNameApply` (System_DeviceName.cpp) on the production mesh record. SET-3: a save while a backup
master is promoted writes the backup role, and `espnowmeshrole` (run here) says the master role is a promotion,
ends it for `worker`, `master` and `backup` (the chosen role is what a save then writes) and keeps it for a
refused role. The mesh id cases (I1-I8e): a
new mesh has id 0 (`espnowmeshes add fjqo` no longer collides), a passphrase gives the key's id, `setpassphrase`,
`rename` and `enable` (both the cached-key and the stored-id branch) refuse an id another mesh here has and change
nothing, a cleared passphrase keeps the id across a restart, and two meshes with one id at boot leave the higher
slot off the air with one WARN, which a later ESP-NOW start or settings reload does not undo or repeat (I8b), until
its key differs (I8c). The device-name cases (G1-G3) check the rule, the hostname hand-on and the warning naming a
paired device or a peer that has the name; P1-P3 run the pair commands' peer-name rule (`espnowPeerAliasRefusal`). A device's only mesh can be disabled and deleted, which leaves no mesh, as on a new device; `delete`
frees a slot for `add`, clears its keys and bond settings, and is refused while a stored identity or a registry
device (in RAM, or in the saved file before the first start) still names the slot, and when either cannot be read.
After deleting a middle slot and a save and load, every mesh is still in its slot; files without slot numbers load
in order. The bootstrap creates `primary` only on a device with no mesh at all, so no second default and no second
`primary` appear. A running instance always holds the passphrase a start would load (an enabled default mesh in
slots 1..3 that has one, else slot 0's): a new passphrase on that mesh, `setdefault`, `enable`, `disable` and
`delete` apply at once, a stopped instance holds none and the next start loads it, and every reply says whose
passphrase is in use (or that none is) when it changes; a passphrase set on a disabled mesh says the mesh stays off
until it is enabled, and a clear says what stops (slot 0: the mesh is disabled and its devices cannot exchange
frames; slots 1..3: heartbeats and time sync with devices that still have a passphrase); `listjson` reports the
slot, the web page's mesh table marks it and its clear dialog says the same per slot. A disabled mesh keys nothing:
slot 0's passphrase is in use only while its mesh is enabled, and the start-up warning, the replies, `listjson` and
the web marker agree. An instance whose stop is still pending is left holding no passphrase. Clearing slot 0's
passphrase follows `espnowmeshes remove`: refused with the same reply while that mesh is the default and another
mesh is enabled, otherwise the mesh is disabled and no longer the default; the web page greys out Clear passphrase
in the refused case, and its action panel writes mesh labels into attributes escaped. Exactly one enabled mesh is
the default after `enable`; the start-up warning names the mesh a start loads the passphrase from; `devicename
clear` works only while ESP-NOW is stopped. A pairing with no mesh argument goes to the default mesh and never to a
slot without a mesh (`meshSlotForPairing`); an empty label matches no mesh; deleting a mesh whose passphrase is not
the live one leaves the live one alone (the instance records the slot its passphrase came from; the text does not
say whose it is); a registry that failed to load, or whose last save did not reach the file, is counted from the
table and the file together. The suite fails 102 checks on the tree before the fix, and 51, 17 and 3 on its first
three versions (1319eff0, df7fec4f, 31456b6c), each of which an independent review found wanting. The
passphrase-in-use cases (2026-11-17, rounds 3 and 4) fail 33 harness checks and all 5 web page source checks on
cca36fa9. The round-4 cases (X3, X6, X7, X8) fail 27 harness checks and 2 of the 7 web page source checks on
3c5dda31.

`espnow_identity_walk` runs the production `peerIdentityLoadAll`, `readPeerIdentityFile`, `peerIdentityForget` and
the cache accessors over the shared in-memory filesystem: a readable identity loads with its mesh slot; no peers
directory counts as read; an unmounted filesystem or a peers path that will not open does not; folders without an
`identity.json` are skipped; an `identity.json` that stays out of the cache (unreadable, another address inside, a
17th when the 16 slots are full) leaves `peerIdentitiesLoaded()` false with a warning that names the address and
`espnowunpair` (which removes it; `espnowforget` is gone since ESP-NOW 1.0 step 3), while the readable ones still load and nothing is written past the cache; a forget whose file will
not go away marks the cache incomplete; the next walk recomputes the mark. An identity whose `meshId` is not a slot
of this firmware (4 and up, too large for a byte, negative, text, missing) stays out of the cache like an
unreadable one. Only a folder spelled as `macToPathToken` writes it is a peer folder: a lower- or mixed-case twin
neither loads the identity a second time nor keeps a copy in the cache after a forget, and one that holds its own
`identity.json` is named in a warning without marking the cache incomplete (no command reads or removes it, so it
holds no mesh slot). Two source checks pin the duplicate-address guard and the range check in
`peerIdentityPersist`. 18 of its first 70 checks failed on the tree before the first fix; 44 of its 127 fail on
cca36fa9.

`espnow_peer_files` runs the production `peerFilesRemove`, the manifest reference (`manifestCacheRecordUse`,
`manifestDropIfUnused`, and `manifestCacheHit`, the bond sync's cache lookup) and the unpair reply line over the
shared in-memory filesystem. After an unpair nothing is left under `/system/espnow/peers/<MAC>/` (identity, bond
caches, manifest reference, a subfolder) and the folder is gone, with nothing removed while a listing is open;
files received from the peer stay. The cached manifest goes once no peer folder names its hash (two peers on one
firmware keep it until both are unpaired; folders that cannot be listed keep it), and new firmware on a peer drops
the hash it named before, so the cache holds one file per peer. A manifest the bond sync finds already cached is
recorded for that peer as well, so a second device on the same firmware keeps it, and one cached before the
references is named at the next sync. A file that will not go is reported. The reply line names what went and where
received files are, within its buffer. Source checks pin the order inside the unpair teardown
(`espnowUnpairTeardown`, shared by `espnowunpair` and `espnowrevoke`), that `espnowforget` is gone, the write order
(the registry is saved before the identity is deleted), that the teardown clears every other store (the named master
and backup pins, `meshMasterMac` and the channel-follow master while the identity still holds the key, the sensor
leases, the candidate list, a pending pair request either way, the re-key window, the slots) and that
`espnowrevoke` writes the revoked list before the teardown; they record a reference only for a manifest that reached the cache, and route the sync's cache
lookup through `manifestCacheHit`. 40 of its 72 checks fail on cca36fa9. Two more source checks pin that
the unpair teardown ends the bond with the partner (compared with the partner setting itself) before it deletes the
peer, and calls off its key exchange and the queued frames before the identity goes.

`espnow_start_order` runs the production slot 0 bootstrap and its `PrimaryMeshBootstrap` guard on the production
mesh record, compiled with bonded mode on and off: a start that fails leaves a new device with no mesh (no
`primary`, flags, bond fields and an unlabelled slot's passphrase and key as before) and writes the settings again,
so a file another task wrote during the start cannot bring `primary` back, while a command that changed the meshes
during the start (rename, passphrase, key, `add`, `remove`, bond) is kept and nothing is written, a start that
succeeds creates the enabled default `primary` once, and a configured or deliberately emptied slot 0 is never
touched. Source checks pin what needs the radio: the "already running" answer and the device-name check come before
any allocation, the guard sits after `esp_now_init` and before the callbacks, the passphrase restore and the boot
notice, and only a successful start (or one whose cleanup is pending) keeps it; a later start keeps the peer-array
size the arrays were allocated with; `openespnow` does not say "Error:" twice. 18 of its 48 checks fail on the tree
before the fix. The round-4 cases fail 8 of its 30 compiled checks (7 of 26 without bonded mode) on 3c5dda31.

`espnow_system_notice` (rewritten in ESP-NOW 1.0 step 8, when the BOOT frame went) runs the production
`espnowFilePeerRestart` and the text-queue drain: a peer's restart prints `[ESP-NOW] <name> is online
(boot #N)` and is stored as one attributed `MSG_SYSTEM_EVENT` record `Online (boot #N)` with no group id
(sealed marks it encrypted; no registry name gives the MAC text; a long name is cut); chat is unchanged and
the drain parses nothing. Source checks: no copy of the name in `EspNowState`, `v4h_text` files every frame
as chat, `v4h_heartbeat` files a restart only from a sealed counter past the last sealed one, and nothing names
the BOOT frame, its JSON notice or its formatter.

`espnow_bond_partner_change` runs the production `bondconnect`, `bonddisconnect`, `espnowbondmodeenabled`,
`espnowbondpeermac`, the bond frame hold, `isBondPeerMac`, `resetBondSync`, the worker's capability request and the
CAP_REQ, CAP_RESP and STATUS_REQ handlers against a settings double that lets the receive task run between any two
statements of a command, and a model of the master's sync tick. The bench's order of events (2026-11-17, step c1)
no longer leaves the worker SYNCING; no frame accepted during a partner change is wiped; the old partner's frames
stop and its sync state goes; saving the same value resets nothing; a worker that lost its sync record while the
master stayed SYNCED recovers; an unasked CAP_RESP does not restart a manifest request. espnow_task also runs at
every use of the ESP-NOW state and every INFO line, so a reset moved after the end of the hold is caught at run
time; source rules keep the resets in `bondconnect`, `bonddisconnect`, the two setting commands, `bondresync` and
`bondrole` inside the hold. It also runs the bond parts of the ESP-NOW loop (the heartbeat send and timeout, and
the sync tick up to the streaming setup) at every point espnow_task can take the CPU: while a partner change holds
bond frames off, nothing is sent, no session is cleared, no offline event is posted and no manifest reference is
recorded, and the tick works again afterwards. `cmd_espnow_unpair` runs with doubles for its registry and file
steps: unpairing the bond partner ends the bond first (settings, state, offline event, a reply line) and afterwards
nothing is sent or started toward it; another device leaves the bond alone; a partner set while bond mode is off is
cleared. A partner change that runs while the tick waits (the gap between stream commands, the FS lock in the
manifest lookup) ends that tick's work; the status answer, push and poll wait out a change and the old partner's
pending requests are not answered after it; a bond file answer queued on cmd_exec before an unpair or partner
change is dropped when it runs. 22 of its 66 checks fail on 3c5dda31.

`espnow_handshake_backoff` (since 2026-11-18 also: sixteen silent paired devices, offered a start every 500 ms
for an hour, each reach their long wait; the table has one entry per paired device) runs
`System_ESPNow_HandshakeBackoff.h`, the KEY_EX in-flight table and retry sweep, the
pending-frame queue, the ESTABLISHING sweep, `v4_send_encrypted_or_queue` and `bondSendEncryptedAsync` through
simulated hours: a partner that is not there (step d2) costs 4 warning lines and 17 key exchanges in the first hour
instead of 1,439 and 720, and no heartbeat is queued; a partner that comes back is bonded within 15 s of being
heard; one heard all along but never answering keeps its back-off; an answering partner behaves as before; a manual
key exchange starts at once, is always reported and ends the back-off; `espnowsessionopen` starts at once and its
failure is printed even after the partner has been away; `bondconnect`/`bondresync` end the back-off and start the
handshake at once on the side that starts bond handshakes; bond sensor data is dropped like the heartbeat; the
pending-frame warnings carry the count they held back. Source rules pin the calls that end a back-off (key exchange
reply and confirm, session up, authenticated frame, mesh online edge, unpair, manual commands). An unpair
(`espnowKeyExCancel`, `pendingFrameDropForPeer`) stops the HELLO retries to the peer, frees the entry one retry
interval later without a warning, lets a new key exchange start at once and drops only that peer's queued frames,
each reported failed. 5 of its 60 checks fail on 3c5dda31. Since 2026-11-18 (bench: a second `espnowsend` before
the session opened evicted the first) it runs the per-peer FIFO (`System_ESPNow_PendingQueue.h`, included whole):
sends before a session wait and go out in order when it opens; a send over a session that is ACTIVE before its
drain ran does not overtake the waiting frames; frames that waited across a session that opened and closed go out
with the next; the fifth frame for a peer and the seventeenth in all are refused with their reason, marked failed,
in one warning line a minute, nothing already waiting dropped; an unanswered peer's frames each time out failed
in one line; `pendingFrameDropAll` (closeespnow) and an unpair drop and fail exactly theirs. Source rules pin the
drop in `deinitEspNow` after the RX task stops and the drain before a fast-path send. Review of 2026-11-18: frames
waiting for a session this device answered (the peer's SESSION_OPEN, a lost glare) go out in order from the tick
(`pendingFrameDrainSettled`) once the session is 1 s old, not at once and not failed at 5 s, and a frame queued
just after a drain goes out with the next tick; the bond request commands' send (`bondSendRequestForReply`)
answers queued, sent or refused from what became of the frame (a fifth request refused, not "queued"; a
responder without a session sends nothing). Source rules pin the tick's drain before its timeout sweep, no drain
in the responder's SESSION_OPEN handler, and the four bond request commands and `bondresync` using that send.
`espnow_pending_queue_tests` runs the header's rules alone (admission, order across slot reuse and counter wrap,
expiry across the millis() wrap, the settle time).

## A peer restarted: the session with it ends

Bench 2026-11-18: the P4 restarted while the XIAO still held its session with it; three messages went "via
SESSION_FRAME" into a session the P4 no longer had and were lost, the XIAO noticed only after two send timeouts
(~10 s), and then lost 6 s more to a SESSION_OPEN glare. Rules: `System_ESPNow_PeerRestartCore.h`.

`espnow_peer_restart_tests` compiles that production dependency-free header: a group-tagged heartbeat whose boot
counter is newer (or the first one this device sees; the BOOT notice until ESP-NOW 1.0 step 8) closes a live session; the same counter, an older one (never recorded: two captured
notices of different starts played in turn close nothing), 0 or no counter, no session or a session younger than 2 s
close nothing; a counter from a sealed frame (bond heartbeat) becomes the reference outright, so a peer whose count
was erased is recognised again; copies (same or older counter) are not filed; counters compare as a signed 32-bit
difference across the wrap; only a group tag of the peer's own mesh counts; only a sealed counter past the last sealed
one is reported as a restart (the "Online (boot #N)" record; tagged counters never report); forged heartbeats with ever newer counters close at most once per peer per
10 s, a limit the SESSION_UNKNOWN hint shares; the device without the session answers at most one hint per peer per
second and none for its own handshake or a replaced session; a hint closes only the live session it names, for a
sequence this device sealed in it; the 16-entry table gives up the entry used longest ago; the times survive the
millis() wrap.

`espnow_rx_frontend` (extended) checks that a sealed frame for a session this device does not hold ACTIVE is not
opened, acknowledged or handed on, and asks the hint rule once per frame, and that a frame under the live session
asks nothing; since step 8 it also runs the heartbeat's boot counter through the real rules (see the step 8
section). `espnow_dispatch_policy` (extended) checks that opcode 31 (BOOT, retired in step 8) has no row and runs the
real SESSION_UNKNOWN handler: only a
group-signed hint from the paired peer itself under its mesh's tag reaches the hint rule. `espnow_session_rekey` (extended, C12, C20, C21) runs the
glare change in the production handlers: the A side keeps its SESSION_OPEN and sends it once more, byte for byte,
when the peer's OPEN arrives; a peer that missed the first copy yields to the second and one session stands at once
(before: a 6 s ESTABLISHING timeout); a peer that already answered ignores the copy; one copy per attempt; the B side
sends none. `--source-dir` with the tree before the change fails the cases marked FAILS ON BASE.

`espnow_rx_frontend` (1.0 frame rules) runs the production `v4_group_tag` with the receive path: a group-signed
frame is header + payload + a 16-byte tag and is accepted, a one-bit change of the tag is dropped, a frame carrying
the pre-1.0 32-byte tag is dropped, and a flooded frame goes on with a 16-byte tag. The routed budgets are 194 bytes
of inner frame and 146 of sealed plaintext; the time, sensor, battery, event and forward-link frames reach their
handlers through a relay at those sizes. Frames that reach a handler are sized to the row's minimum (a
METADATA_PUSH is 202 bytes, a KEY_EX_HELLO 72).

```sh
ctest --test-dir <build> -R 'espnow_(peer_restart_tests|rx_frontend|dispatch_policy|session_rekey)' --output-on-failure
```

Not covered: the radio and the session close and hint send glue in `System_ESPNow_Sessions.cpp`
(`sessionPeerBootCounter`, `sessionPeerBootSeen`, `sessionUnknownFrameSeen`, `sessionUnknownHintReceived`), which
run on the device only (the rules they call are tested).

## Bond agreement (BOND_REQUEST / BOND_NOTICE)

A bond is a setting on each device. These tests cover the two frames with which the devices tell each other how
it stands: a device bonded to a peer that is not bonded back, or whose partner ended the bond, is told so, keeps
its own setting, stops the heartbeat, the capability requests and the 15 s timeout that cleared the session, and
asks once a minute. No received frame changes a bond setting.

`espnow_bond_agree_core_tests` compiles `System_ESPNow_BondAgreeCore.h` (the production dependency-free header) and
runs modelled devices over a channel that can lose frames: two devices bonded to each other agree and then send
nothing; a one-sided bond is declined, asked about once a minute for an hour and reported to the other user at most
every 15 minutes, with no setting changed there; BONDED_ELSEWHERE; ENDED and bonding again; a request that its
sender's ENDED overtook is not used or answered (the ENDED names the sender's last request; older and equal numbers
are void, a newer one counts and ends the mark, the 16-bit numbers go round, a partner change and a new session
forget the mark, an ENDED without a number voids nothing, and a waiting ENDED is not replaced in the inbox by the
notice after it); the device that ends a bond makes no request between its ENDED and the settings change, and asks
again after 5 s if that change never comes; the same role on both sides (answered, learnt and reported once on
each); a notice from a peer that is not the partner, an answer to an older request and an unknown reason are not
used, while ENDED counts with any number; lost requests and answers are made good by the repeat; the inbox keeps
arrival order and drops what does not fit; one notice a minute per peer whose bond frames are ignored; more peers
than the limit table has entries share its last entry and cannot start each other's limits over (six peers for ten
minutes, six and ten askers for an hour); a peer that is not the partner and asks 1000 times in 10 s gets two
answers, and a second request inside 5 s is answered when the 5 s are over, to its own number; the request timing
(waits for the session, one second more on the side that answered the handshake, retry after a full send queue, no
repeat once agreed, which requests have their acknowledgement watched, a handshake start a minute for a declined
partner without a session also while a request is waiting, a decline after 30 days of agreement or of a waiting
request, the clock wrap); the bond heartbeat of a partner without these frames stands for its agreement.

`espnow_bond_agreement` (`test_espnow_bond_agreement.py`, cases in `espnow_bond_agreement_cases.cpp`) runs the
production glue on the doubles of `espnow_bond_partner_change_harness.cpp`: the wire payloads and reason codes from
`System_ESPNow_Wire.h`, `v4h_bond_request`, `v4h_bond_notice`, `bondAgreementTick`, `v4h_bond_heartbeat`,
`bondNotifySessionEstablished`, the bond parts of the ESP-NOW loop and the bond commands. The partner is asked when
its session comes up (a second later on the side that answered the handshake) and its request is answered ACCEPTED;
a request from another paired peer is answered NOT_BONDED or BONDED_ELSEWHERE (the frame and the line never name
the partner, and the frame does not carry this device's role), at most once per 5 s, printed for the user within
the limits, and leaves bond mode, partner, role and link as they were; with ENDED or NOT_BONDED from the partner
the setting stays, the partner goes offline once with its sync state cleared, and five minutes bring five frames,
all of them the request with its acknowledgement watched (no heartbeat, no CAP_REQ, no cleared session); a session
coming up or a late heartbeat does not bring a declined partner online, but the session gets its auth-log record; a
declined partner whose session is lost is handshaken once a minute, also while a request is waiting; the bond
returns when the partner bonds again. With no answer at all the bond behaves as before (sessions cleared about
every 20 s for a partner that is not bonded, a normal sync for one that is and only sends heartbeats). Also:
ROLE_CONFLICT on both paths and from the heartbeat after three in a row, the notice limits, the retry after a full
send queue, no request without a session, and a partner change (the agreement is reset, nothing goes out while bond
frames are held, a request received during the hold is answered from the new settings). A decline handled on
espnow_task while cmd_exec is inside `bondNotifySessionEstablished` is tried at every point where espnow_task can
get in (the doubles model the agreement's lock, the auth-log write and the moment before the online event): the
partner ends up offline with no capability request due and no 15 s timeout to come, and an offline event never
precedes its online event. The partner's request from before its ENDED does not agree the bond again, on the same
turn or a later one, and its request 1 after a restart counts on the new session; this device's ENDED names its
last request, and a request that the partner's late heartbeat sets off right after the ENDED is not made.
`bondconnect` to the partner already set says how the bond stands when it is not agreed, asks again on the next
turn and at most every 5 s, and for an agreed partner answers the command replies' Error "already running" while
the session is up. `bondAgreementKey` /
`bondAgreementReasonKey` give the text every JSON carries.

`espnow_bond_partner_change` (extended) checks the unasked ENDED: `bonddisconnect`, `bondconnect` to another
device, `espnowbondmodeenabled 0`, `espnowbondpeermac` to another device and `espnowunpair` of the partner each
tell the dropped partner once, from the command itself (a notice left in the send queue would go out after the
unpair removed the session and start a key exchange with the removed device), before the frame hold and before the
peer is deleted, with the number of the last request made and no request after it; the same partner in another
spelling, a partner set while bond mode is off and a partner without a session are told nothing, and no handshake
is started for it. Source rules pin those call sites, the agreement reset inside the hold, the tick skipped while a
hold is up, handlers that never touch a setting, the online state of a session coming up written only in the lock
hold that reads the agreement with the auth-log write as the last step, and the agreement as the same text in
`bondstatus json`, `/api/bond/status` and the system status.

`espnow_dispatch_policy` (extended) runs the two new table rows: BOND_REQUEST and BOND_NOTICE reach their handlers
from any paired peer over its session, bond mode on or off, and are dropped from an unpaired source or in
plaintext; the drop path notes a paired peer whose bond frame was ignored (not the partner, not an unpaired
source) so that it can be sent a notice; with bonded mode compiled out the rows do not exist.

```sh
ctest --test-dir <build> -R 'espnow_bond_(agree_core_tests|agreement|partner_change)|espnow_dispatch_policy' \
  --output-on-failure
```

## ESP-NOW room and tag membership

`espnowroomcmd` and `espnowtagcmd` send a command with an account name and password to every device of a room or
with a tag. Room and tags are what each device says about itself, so any paired device that named a room got the
password sent to it. A device now keeps, per paired peer, the room and tags accepted there, and the two commands go
only by those.

`espnow_group_core_tests` compiles `System_ESPNow_GroupCore.h` (the production dependency-free header): a newly
announced room or tag is pending and no membership; accepting records what is announced at that moment; a room or
tag the peer stops announcing leaves the accepted set with that announcement and waits again when it comes back; a
change of room is out of the old room at once and pending for the new one; tags are judged one by one; a repeated
announcement reports nothing new and a changed one only what newly waits; a stored record keeps nothing accepted
that it does not announce; letter case, spaces, empty entries and repeats in a tag list; a tag command names one
tag, and a list of tags matches nobody; a peer that keeps changing is reported once a minute with the number of
changes in between; the notice and list texts, which fit a log line with the longest names; text longer than a
field is cut.

`espnow_group_membership` (`test_espnow_group_membership.py`, `espnow_group_membership_harness.cpp`) runs the
production code with the vendored ArduinoJson: the record glue with its lock and the writer and reader of the stored
fields, the per-sender metadata inbox, `processMetadata`, the announcement functions with the report limit and the
save gap, the acceptance functions, the metadata refresh tick, `addEspNowDevice`, `removeEspNowDevice`,
`resolveDeviceNameOrMac`, `countMeshPeerMetaByRoom`, the listing marks with `espnowSerializeMeshPeerMeta`,
`cmd_espnow_devices` and `cmd_espnow_rooms`, and `espnowroomcmd`, `espnowtagcmd`, `espnowgroups`, `espnowgroupaccept` and `espnowgroupdrop`.
The registry state and its save, the sessions, the encrypted send, the metadata request, the pending-frame queue,
the mutex, events and logging are doubles. A
peer that newly says it is in a room gets no frame and is counted in the reply with the accept command; an accepted
peer gets the frame with the credentials while a pending one beside it does not; a move from Garage to Kitchen
stops Garage commands with that frame and Kitchen commands wait for the accept; tags the same, one by one; the
accept reply states what was accepted when the announcement changed after the list was read; `all`, drop, the JSON
forms and the refusals; a registry that cannot be saved is said in the reply; unpairing removes the accepted state
and a re-paired address starts empty; an identity change clears it, drops the frames waiting for a session and is
said once; the stored record round trip, a record without the fields (nothing accepted), one that names as accepted
what it does not announce, and values that do not fit; the notice comes once per change, not per frame and not
again after a restart, and names the peer by its local name and never by a name the peer announces; with fifteen
peers every member is sent to, the replies stay inside their buffers and say how many peers they leave out; the
listings show the announced value with a mark. Further: with a metadata cache smaller than the registry (or none)
a peer without a cache slot still has its room and tags recorded, above all when it leaves; fifty changing frames
in five seconds print one report and save the registry once, each of them still takes effect at once, the state at
the end of the gap is saved once and the report after the minute gives the count and what waits; the mutex double
fails the run when it is taken twice or held while something is logged, saved or sent, and lets "the other task" in
just before a critical section: an unpair before the announcement's store leaves the next record alone, a drop
before an announcement stays dropped, an announcement before an accept is what gets accepted; a tag target with a
comma is refused before anything is sent; `espnowdevices json` and `espnowrooms json` with fifteen peers stay under
the result size and parse, with pending peers, accepted peers and the longest names (the rest counted in
`notShown`); two peers with one name, a peer named like this device's own entry and a name that reads as an address
are called by their address, and the name is refused by the three commands; the inbox keeps a frame per sender,
serves senders in turn and answers every peer that asked; a session that came up gets its peer asked once, after it
settled, one peer per look, again when the session is renewed, and never back and forth for two sessions with one
peer; a group command follows the command reply contract: without encryption nothing is sent, a member whose frame
is refused makes the reply an `Error:` that names it and the reason (the members it went to still listed), none sent
is an `Error:` too, and an empty room answers `Error: No devices found ...` with `espnowgroups` as the next step;
since 2026-11-18 a member whose frame waits for its session is listed "queued until its session opens", the reply
counts them ("(1 of them queued until their session opens)") and, when none went out yet, reads "Queued '<cmd>'
for N device(s) in <room> until their sessions open".
Source rules: `processMetadata` updates the registry record before it looks for a cache slot, the registry writer and reader call the two field
functions, every place that fills a new record starts it empty, the unpair teardown removes the record, forgetting an identity
(`espnowForgetPeerCrypto`), `espnowregenidentity` and both KEY_EX handlers clear the acceptance, `groupCommandRun` sends in one place after the
membership check and does not read the mesh cache, and the five commands are admin rows.

`espnow_metadata_text` and `espnow_rx_frontend` compile `processMetadata` with the announcement functions and the
rules header. `espnow_metadata_text` also runs the receive handlers with the inbox: three peers that move to another
room in one pass are all out of the old room afterwards (the sender that sent twice by its last frame), the registry
is saved once at once and once after the gap, the answer to a refresh the device asked for by itself gets no line in
the message history while a push does, and two requests of one pass are both answered. `espnow_rx_frontend` checks
that a paired peer refused by a full metadata table still has its room recorded; `espnow_keyex_hello` checks that a new or replaced identity calls the clearing and a refused or repeated
one does not; `espnow_page_render` checks the pending mark and the Accept button of the web metadata panel.

```sh
ctest --test-dir <build> \
  -R 'espnow_group_(core_tests|membership)|espnow_(metadata_text|rx_frontend|keyex_hello|page_render)' \
  --output-on-failure
```

Not covered: the radio, the real registry file on flash, and two real tasks at a device record at the same moment
(the lock is a FreeRTOS mutex on the device; here its use is checked in one thread, with the other task's step run
just before a critical section).

## Web server and reports (2026-11-20)

```sh
ctest --test-dir <build> --output-on-failure \
  -R '^(web_server_task_placement|bond_fs_poll|notification_low_resource_rules|memreport_table|internal_poll_audit)$'
```

`web_server_task_placement` reads `WebServer_Server.cpp`: the HTTPS and the plain-HTTP server configs each set
`core_id = PRO_CORE` and `task_priority = TASK_PRIORITY_NORMAL` (core 0, priority 3) once before their start
call, and the stack sizes are unchanged. The restore-only server of first-time setup keeps the defaults.

`bond_fs_poll` compiles the shipping bond file handlers, reply callbacks and bridges of `WebPage_Bond.cpp` with
the real `WebBondFsPollCore.h`: `/api/bond/fs/list`, `/stat` and `/get` start a request and answer
`{success:true,pending:true,id}`; the answer comes with a poll, once; the bridge mutex is never held across the
ESP-NOW send or a socket write and no take waits longer than 100 ms; a reply that arrives before the send returns
is kept; busy, send failure, not bonded, the 7 s timeout, an uncollected answer (dropped after 2 s), a foreign id
and the admin gate on every poll are covered. The page script is `tools/webui/tests/test_bond_fs_poll_page.py`.

`notification_low_resource_rules` compiles the rule switch, tiers, message formatter and cooldown check of
`System_Notifications.cpp`: `heap_low` and `fs_low_space` have the queue and toast sinks at level 2, a
10-minute cooldown that the (now 32-bit) rule field holds, and texts that fit the 64-byte buffer. The EVT-2
faults (`sd_write_failed`, `settings_save_failed`, `sensor_fault`, `wifi_connect_failed`, `g2_disconnected`)
have the queue and toast sinks, no banner, their cooldowns and their own sentences.

`memreport_table` compiles `System_MemReportCore.h` with the real ArduinoJson: every row of the field table
appears in the text and the JSON form, a zero total prints 0 % without a division, the heading says MEMORY REPORT
for the command and BOOT MEMORY REPORT at boot, the documented JSON keys keep their values and
`tools/p4/ram_monitor.py` accepts the reply. Source checks keep `cmd_memreport` and the OLED Memory page on the
shared walks.

`internal_poll_audit` compiles `executeInternalStatusCommand`, the suppression check and both audit entry points
with the real `CommandContextBehavior` enum: a web page's own status poll (bit
`COMMAND_CONTEXT_INTERNAL_STATUS_POLL`) writes no `command-audit.log` line and no `[CMD]` echo; the same command
from a user, an automation or a trusted system command is audited. Its `READ_ONLY_POLLS` list is the review
point: a command added there runs unaudited, so it must be a read-only status command.

## Logs on the SD card, sealed (B11, 2026-11-22)

Log lines go to the card first and every log file there is sealed with the device key
(`System_SealedLog.h`). The pure rules have their own executables; the module and each writer around it are
executed by a harness.

`sd_path_core` compiles `System_SdPathCore.h` as it is: for every protected card folder, each spelling FatFs
would resolve to it (other case, a trailing dot or space, a backslash, the 8.3 alias) is refused, alone and with
a tail; the shape of a short-name alias; paths that are not on the card are never refused.
`filesystem_listing_permissions_contract` runs the same matrix through the production permission code for a
user, a guest and an admin, on the command table and the web endpoints (read, write, create and delete), and
fails when a `/sd/...` row of the rule table names a folder the core does not list. `guarded_path_normalize` and
`fs_permission_strings` hold the reports of the paths that changed (card names ending in a dot or space;
`/sd/STT/u2`).

`sealed_log_core` compiles `System_SealedLogCore.h` and the card part of `System_LogCapsCore.h`: the mark line
with its key id and whose file a head marks (ours, another device's, no id, torn, plain), lines of a batch, a
line over 1023 bytes split with nothing dropped and no UTF-8 sequence cut, stored sizes, the moved-aside name,
the card caps (twice nominal, 5,646,884 bytes) and the card reserve with its hysteresis.

`sealed_log` compiles the production `System_SealedLog.cpp` over `fake_vfs.h` with the toy key and cipher of
`sealed_log_host.h`: the mark first in a new or empty file, the head of our own file read once per mount, a file
that is not this device's moved aside unchanged to the first free `.foreign-<n>` (never appended to, never
removed), nothing created without the key, one row per line and one write per row, a short write that leaves
whole rows and no plain byte, `openFresh`, `writeMark`, `LineStream` (lines put together across pieces of any
size) and `readWindow` (whole rows by stored offset through windows of several sizes, the ends of the file,
rows that do not open, another device's file, a failed read). ASan/UBSan.

`log_overflow_latch` executes `VFS::resolveLogPath` from `System_VFS.cpp`: the card twin while a card takes
writes, has room and the key is there; card absent, back (after the probe back-off), full at 8 MiB and usable
again at 16 MiB with the low-storage event once per episode, a new mount starting a new episode, the minute off
the card after a failed append, the flash floor with and without a card, and no key (flash, counted, one event,
the key asked for once a minute). One `[EVENT]` line per switch. The G-39 and FILE-3 cases are unchanged.

`capped_log_writes` compiles the capped-log writer with the production `System_SealedLog.cpp` under it. The
cases written before B11 run with the harness switch `cardSealing` off (plain bytes on either tier: the append,
both rotations and every failure branch); `sealedCardCases` and `sealedBatchCases` run with it on, as the device
does: the mark and one sealed row per line for a single line and for a flushed batch, a long line split, the
card cap of twice the caller's, the copy rotation (the mark first, whole rows from a row boundary, the trim note
less the mark line), the generation rename (each generation with its own mark), the restart when the copy cannot
be written, a file that is not ours moved aside, and a card that fails (open, short write, seal, no key) sending
the line or the whole batch to flash with no plain byte left on the card.

`log_row_writes` (the sensor log), `automation_log`, `transcript`, `transcription_ui_adapter`,
`gpslog_start`, `map_live_tracking`, `web_file_streaming` and `r1_history_store` use the SealedLog double
`sealed_log_host.h` (the production rules over each harness's own file double) or their own crypto doubles:
a capture on the card is sealed whatever `capturecrypt` says and falls back to flash when the card fails; the
automation log on the card; a transcript whose sealed header and text read back byte for byte as the flash file
and whose kept size is the stored size; `transcripts read` paging a sealed transcript in whole rows; the start
preparing the card twin; a sealed track loaded and listed from both tiers; the mark with its key id on a sealed
health day. `g2_files_psram` runs the production reveal with a mark that names another device's key (one notice
line, no row tried). `espnow_catchup_core_tests` checks that the classification of sealed card files is as
before and that a moved-aside file is never offered. `log_retention` already holds that no pass removes a
transcript on either volume (B12). `time_anchor_persistence` adds the promote sweep on the card: a boot folder
there is promoted on its own tier, the open session's card twin is left alone, and a boot's anchor row is kept
until neither tier holds its folder.

### Review fixes (2026-11-23)

Nine findings of the review of the first B11 commit, each with host cases:

- **Code-page spellings.** The card is mounted with code page 437 and ANSI/OEM paths; FatFs folds a byte of
  0x80 or above to ASCII for the short name (0x82 to E, 0x98 to Y, 0x93/0x95/0xA2 to O). `sd_path_core` refuses
  every protected folder with each letter replaced by each byte from 0x80 up, and any card path with such a byte;
  `filesystem_listing_permissions_contract` runs the folded spellings and the review's paths through the
  permission code for a user, a guest, an admin and the system identity. `guarded_path_normalize` records the 18
  corpus paths that changed (all `/sd/<byte>`, every column now a refusal).
- **Rows bound to the kind of file.** `sealed_log_core` holds `classOf` (system logs, boot debug files, the
  capture tree, one account's transcript folder, else the exact path; a file and its card twin one class).
  `sealed_log`, `transcript`, `transcription_ui_adapter`, `map_live_tracking`, `web_file_streaming` and
  `g2_files_psram` (the production reveal) read rows at a path of another class: a transcript copied into
  another account's folder, the card's root or the capture tree, and the command audit copied elsewhere, show
  `[undecryptable row]`. The toy seal of `sealed_log_host.h` carries the class and has the size of a real row;
  `sealed_reveal_host.h` is the reader-side double (`ENC1:@<class>@text` opens only for that class).
  `web_file_streaming`'s source contract holds that the seal and the open take their additional data from the
  path and that the web viewer hands over the normalized path.
- **Plain lines in a sealed file.** `sealed_log_core` holds `classifyLine` (mark, schema line directly after it
  in a class that has them, row, blank, unsealed); `sealed_log` (`readWindow`), `g2_files_psram` (the production
  `captureCryptoRevealText`, including the growth of short lines and a later page that has no header),
  `transcription_ui_adapter`, `capture_stitching` (the export) and `map_live_tracking` (the loader takes no point
  from a plain row) show `[unsealed line]` or ignore the line.
- **A failed card write is detected.** `fake_vfs.h` has the fault `lostAfter` ("write accepted, flush lost":
  every write and the flush succeed, the file never grows), and the capped-log, sensor and automation harness
  file doubles have the same knob. `sealed_log` holds `settle` (flush, then the size) and
  `sealedlog::linesLanded`; `capped_log_writes`, `log_row_writes` and `automation_log` lose a line or part of
  a batch at the flush and find it on flash, once. The system log's kept lines and failover are held by source
  contract in `log_line_writes` (its writer is not executed on the host).
- **Torn tail.** `sealed_log` and `capped_log_writes`: a file of ours that does not end in a line end gets one
  before the next row (found at a mount, or left by a pull); only the torn row is unreadable; a tail that cannot
  be mended sends the line to flash.
- **Back-off per file.** `log_overflow_latch` executes `VFS::noteLogCardFileFailure`: with the card's write
  probe passing only that log leaves the card (1, 4, 16, 60 minutes, starting over after a quiet spell, cleared
  by a new mount, eight entries), no card failure is reported and the other logs stay; with the probe failing
  it is the card, as before. `sealed_log` reuses the oldest `.foreign-<n>` when all twenty are taken.
- **Move-aside limits.** `sealed_log`, `capped_log_writes` and `log_row_writes`: without the key a file is
  "unreadable", never "not ours", and nothing is moved. `sealed_log` and `automation_log`: outside the log
  trees a file that is not ours is left alone and the session's entries go to flash; `autolog start` names the
  file the entries go to.
- **Log task stack.** `log_line_writes` holds by source contract that the sealed line is formatted in external
  RAM. The stack itself is not measured on the host.
