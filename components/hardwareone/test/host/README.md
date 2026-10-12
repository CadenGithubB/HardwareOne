# HardwareOne host tests

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

Node.js is required for the three browser suites (`map_live_browser`,
`g2_status_browser`, `sensor_mic_card`). Set `HW1_NODE_EXECUTABLE` at configure
time if it is outside PATH. Without it they stay registered but are reported as
not run (disabled), so a green run then covers the remaining suites only.

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
```

`log_level_application` runs the shipping `applyLogLevel`, `cmd_loglevel`, the
boot clamp block from `applySettings()` and `setDebugSetting`, with DebugManager,
`esp_log_level_set` and the debug-settings persistence request recorded. It
checks the ERROR/WARN/INFO mapping (anything else is DEBUG), that exactly the
`wifi`, `wifi_init` and `phy_init` tags follow the level, every accepted
spelling, the persistence request before the apply, no request for an unchanged
value, refusals that change nothing, and the boot clamp into 0-3.

`system_log_status` runs `formatSystemLogStatus`, both `formatSystemLogFlags`
overloads, `parseSystemLogFlags` and `systemLogApplyPersistedFlags`. It reads the
CLI, web-page and flag-message buffer sizes from their callers, renders the three
logging states into both status buffers, fills the web buffer to 511 bytes plus
NUL for a long path, and checks null and zero-size outputs, the 70-byte minimum
for the 69-character mask, a format/parse round trip of all 256 single-bit masks
in both accepted forms, and the persisted-mask restore (empty or prefix-only text
keeps the live flags; non-hex text currently parses as zero).

`command_feed` runs `originPrefix` and `appendCommandToFeed` from
`HardwareOne.cpp`, with the CM5/live-audio classifiers, audit redaction and the
web mirror recorded. Replies carry `[source user@ip] ` and the feed
`[source user@ip] $ `, including the source-only, user-only and IP-only forms.
For long identities both truncate inside the same 128-byte buffer, so the feed
marker is cut with the prefix instead of being appended after it. Machine polls
never reach the feed. None of these three suites touches a device, a file or the
network.

## Boot temporary-file cleanup

`boot_temp_cleanup` executes the production startup sweep, `VFS::DirectoryReader`
and `FsLockGuard` with real Arduino `String`. Controlled file, semaphore and log
boundaries cover rooted/leaf backend names, child close before removal, exact
directory scope, LLM enabled/disabled, nested caller lock ownership, failed opens
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
stores, clock due times, event throttling, safety polling and millisecond wrap.
Controlled overlap verifies that refreshes during a rebuild survive and that
stopping during a read cannot restore a freed cache. Failed file writes leave
the cached schedule intact. Threaded polling, notifications, event posts and
start/stop exercise cache lifetime under ThreadSanitizer. Permission revoked by
another writer stops the scheduler on its next tick; a later grant stays idle
until the owner starts it. The actual disabled
public header is compiled independently. `automation_scheduler_contract`
retains the main-loop integration and full-tick early-exit/source guards, plus
the quiet in-pass reschedule writes, the unconditional main-loop tick and the
owner transition shared with `features automation`.

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
hardware or claim that every selectable feature combination fits.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^(saved_build_config|build_config_workflow|build_config_wizard)$' --output-on-failure
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
responses. `--baseline <component-directory>` replays regressions against
saved sources. `vfs_capacity_cache_tests` and `vfs_capacity_snapshot_contract`
retain expiry, clock-wrap, invalidation and consumer-boundary checks.

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
existing destination untouched and removes only the staged copy, and a name the
backend cannot take as a staged copy falls back to the in-place write. Two
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
G2 raw/pretty/parse-error output, and malformed encrypted rows that expand on
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

Application-owned heap driver objects use `AllocPref::PreferPSRAM`: external
RAM is preferred, but an absent/exhausted external heap or the PSRAM bypass
routes them to byte-addressable internal RAM. No sensor parent allocation uses
`RequirePSRAM`. Genuine exhaustion of available internal/external memory still
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
zeroed before release; command registration guards retain
their existing authentication requirement.
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
the GPS portions of G2, web, MQTT and sensor logging. It checks independent
copies, cleared failed reads, 5/10/50 ms consumer budgets, all JSON buffer
capacities, disabled-header behavior, failed startup reset, and deferred stop
cleanup. Publication holds the cache lock only for the copy; JSON formatting,
track/event callbacks and destruction run after its release. ASan/UBSan follow
`HW1_SANITIZE`.

The output fixture was captured from the pre-refactor production functions.
An optional `--baseline /path/to/old/components/hardwareone` also executes that
source and compares its output directly. MQTT timeout now omits GPS data; its
successful wire format, raw speed units, and existing no-fix behavior remain
covered. OLED has a narrow source-boundary check; its full renderer is not
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
portions of G2, web, MQTT, OLED and logging. It verifies independent copies,
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
owner synchronization is covered by the suites above. These four suites accept
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
  -R '^(i2c_device_queue|sensor_lifecycle_requests|sensor_start_outcomes|sensor_task_restart|tof_psram_lifecycle)$'
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
back/cancel/timeout, staging refusal, skip behavior, strict MQTT ports and OLED
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
compares successful CLI output against saved pre-change source.

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

`g2_connection_status` executes the production state-name function, topology
snapshot, public connection snapshot and shared legacy text formatter. All seven FSM
states, both lifecycle presence gates and four temple-link combinations are
covered, including off versus initialized idle, partially connected glasses,
physical links before session promotion, unknown batteries, maximum counters,
independent caller copies, and every output capacity through the complete text
length. Formatting a captured copy does not resample the owner. Null/zero-length
output skips capture. `--baseline <component-dir>`
compares successful text bytes with saved production source. The real disabled
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

`g2_client_lifecycle` executes the production initialization and teardown bodies,
role/init claims, private runtime publication/read helpers, combined status and
page/tap worker admission. BLE, resource allocation, clocks and worker outcomes
are host boundaries. It exercises startup refusals, host/scan rollback (including
failed rollback), repeated initialization, retryable worker allocation failures,
and every deferred teardown barrier without retiring the runtime prematurely.
It also executes the production memory checker and G2 registry entry against
15 admission scenarios: cold startup and retained-worker restart below, at and
above the threshold; server-to-client switching; and simultaneous low memory
and failed/deferred cleanup. These check that old host cleanup and temple
quiescence precede memory admission, while new temple/host/worker initialization
follows it. Retained workers remain subject to the unchanged registry floor.
The low-memory restart and role-switch cases retry the same stopped instance
after memory becomes available, checking claim release and worker reuse. The
worker boundary retains page-swap, tap and ring tasks across runtime shutdown;
it retires the session worker and checks that an admitted restart recreates it
once, without duplicating persistent workers or acknowledgement semaphores.
Heap samples and retained allocations are boundary outcomes, not measured
device usage. The existing shared TX mutex may initialize before cleanup.
`--baseline <component-dir>` compares unchanged lifecycle outcomes with saved
production `G2_Glasses.cpp/.h` and `System_MemoryMonitor.cpp/.h`. The additional
admission cases reproduce an older source's early memory refusal and explicitly
account for the intended change in cleanup-versus-memory refusal precedence.
The production state writers and four concurrent readers use
real host mutexes across 12,000 publication/reset cycles and 100 real init/deinit
control-flow cycles; raw temple reconstruction in the host boundary checks that
the existing init claim excludes status readers. This covers state lifetime and
lock ordering, including worker admission without recursive lifecycle locking;
it does not qualify BLE resources, temple teardown, scheduling on-device or
independently sampled telemetry. ASan/UBSan follow `HW1_SANITIZE`. Run
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
`setSetting` overloads, shared change finalization/notification, feature command,
and persistence adapter with the real owner-scoped batch core and ArduinoJson.
OLED-enabled and disabled variants cover full integer widths and adjacent-field
canaries, complete-token parsing, malformed/overflow/nonfinite values, metadata
ranges, read-only and validation-only requests, bool aliases, repeated-off service
reconciliation, publication/persistence/stop/event order, explicit and first-row
alias labels, long string event values and secret redaction.
Changed values notify and request persistence once; no-ops leave batching clean.
Feature tests preserve compiled/toggle gates, owner-applied toggles (Automations
goes through its owner transition, never a raw write), reboot notes, feature-specific
events and settings notifications. Controlled argument-copy/reserve failures
leave the old String intact; failed persistence retains live RAM changes and
retry intent. Foreign-owner/background writes remain independent of an open
batch. Registry contents, task identity, filesystem writers, lifecycle commands,
OLED and String allocation failures are host boundaries, so this does not qualify
device concurrency or physical flash durability. ASan/UBSan follow
`HW1_SANITIZE`, and sanitizer diagnostics fail the test.

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
source-selection or recording command. It runs when Node.js is found at
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

The runner compiles the production `BatteryPolicy.h`, the unmodified backend,
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
stream-stop retry, deadlines, budget and capture failures). No hardware is
touched.

The five camera suites (`camera_core`, `camera_dvp`, `camera_p4`,
`camera_i2c_reservation` and `camera_consumers`) are registered CTest suites;
ASan/UBSan follow `HW1_SANITIZE`.

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


### Edge Impulse JPEG capacity

```sh
python3 components/hardwareone/test/host/test_edge_impulse_jpeg.py --sanitize
```

This compiles the actual Edge Impulse conversion guard and shared JPEG parser.
Only the legacy converter is mocked, writing the complete inspected RGB size
so a missed capacity check is observable under ASan. Real synthetic small/VGA
JPEGs retain their actual geometry. HD input, undersized or null output,
malformed headers, truncated input and a one-byte file cannot reach conversion;
decoder failure cannot publish stale dimensions. Both camera and stored-file
call sites are checked for use of this capacity-aware boundary. No additional
full-image RGB allocation or inference/model behavior is introduced.

### Camera / sensor I2C ownership

```sh
python3 components/hardwareone/test/host/test_camera_i2c_reservation.py --sanitize
```

This runs the actual `System_BuildConfig.h` reservation policy and unchanged
manager constructor, `initBus`, `cmd_i2cbusenabled` and `cmd_i2c2busenabled`
definitions across 18 configurations: P4, S3 and classic ESP32, camera enabled and
disabled, with the DVP SDK I2C1 option undefined, explicitly false, and true.
P4 always reserves hardware I2C0 when its camera is compiled. DVP follows the
SDK-selected port, and camera-disabled builds reserve neither controller.
Sixteen configurations compile and run. The two DVP configurations selecting
camera SCCB I2C1 with external I2C enabled must fail compilation with the specific
controller-ownership conflict diagnostic.

The runtime checks verify logical bus 0 maps to Wire1 / hardware I2C1 and bus 1
to Wire / hardware I2C0. The unreserved controller initializes normally while
the reserved one performs no Wire begin, clock/timeout setup, power-pin writes,
online event or bus-state update. Each CLI enable command rejects its reserved
controller without persisting or changing either bus's existing configuration;
the unreserved bus remains independently configurable. Disabling remains allowed,
and empty and validation-only commands make no writes.

Wire, GPIO, logging, core-affinity dispatch, Arduino String parsing and settings
persistence are host adapters. These tests do not prove physical controller
routing, ISR placement or camera/SCCB operation on a board.

`test_jpeg_validation.py --jpeg-component /path/to/espressif__esp_jpeg --sanitize`
compiles the shared entropy validator against the actual pinned TJpgDec codec.
It exercises FASTDECODE 0/1/2 with scaling enabled and disabled, complete and
truncated entropy, normalization, unsupported formats, and allocation failure.
An optional `--private-bad /path/to/observed.jpg` checks a local failure artifact
without adding private photographs to the repository.

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
