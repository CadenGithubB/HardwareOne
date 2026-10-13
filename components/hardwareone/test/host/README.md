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
`web_batch_handlers` suite covers the same reader inside `handleCLICommand`
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
`System_User.cpp` or `HardwareOne.cpp` (a Begin/End pair, or the scope of a
`NamedSourceUseGuard`) contains console output, `displayUpdate`, the auth-log
append, a roster read, the output-drain wait or the shared mirror broadcast.
The harness then exercises snapshots, lazy bypass creation, a full registry,
policy and identity changes while waiting for a held writer, the Begin/End
delivery windows (held only for the live epoch, a contended Begin waits for
the holder) and `serialTransportSessionClearIfEpoch`, which returns with the
writer released. The mutex stand-in fails a take on a writer the harness
already holds and a give without a take. It does not cover `OLED_Utils.cpp`'s
windows, task priorities or the device. `--baseline` compares the transcript
against a saved `System_User.cpp` that already uses the mutex writers.

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
`rmdir[Guarded]`, `isSDAvailable`, `getStats`, `normalize`, `systemAuth` and the
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
`handleGPSTracksAPI` against an httpd double that records whether the
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

`wifi_connect_budget` runs the shipping `connectToBestWiFiNetwork`,
`connectWiFiId`, `connectWiFiTarget` and the bounded attempt function from
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
console-origin checks fail behaviourally. Run with
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
and the cases fail behaviourally. Run with
`ctest --test-dir build -R '^time_anchor_persistence$' --output-on-failure`.

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
`System_User.cpp` (the `credential_attempts` namespace and the
`credentialAttempt*` functions) and its `System_User.h` contract unchanged,
with a fake clock, critical section, diagnostics and event register. It checks
that tiers 5/10/20 are all reachable by a peer that waits out each lock, that
keys are transport plus peer, that a success clears only failures that named
the same account, that the window ending never cancels a running lock, that a
locked entry is never evicted, that a pool full of locks refuses new peers
while serial, UART, the display and MQTT keep reserved slots, the lock event
and since-boot total, a lock across the 32-bit `millis()` wrap, and that ended
locks stay ended through weeks of idle. Source contracts: the table keeps time
on the 64-bit `esp_timer_get_time()` clock (no `millis()`, no signed-difference
checks) and is not under `ENABLE_HTTP_SERVER`; `/login`, Basic Auth, serial,
UART, MQTT and the
targeted `login` consult it before validating; Basic Auth failures are audited
and evented like `/login`; MQTT failures write the `mqtt/login` audit line.
`--baseline-web <WebServer_Server.cpp>` runs the same scenarios against the old
per-IP web lockout through an adapter (expected to fail). Not covered: BLE,
display and ESP-NOW logins (they adopt the table later), real httpd/MQTT
traffic and the device reply texts. Run with
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
`System_LLM_Internal.h` and `loadTokenizerFromFile`, the merge map,
`freeTokenizer`, `encode` and `decode` from `System_LLM_Tokenizer.cpp`
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
large for the 4 KB reply buffer is an explicit `{"error":...}` rather than a
truncated document; and that the lookup takes the ban-table lock while no file
I/O runs under it. It also checks that the ban's session kick reads the session
table under the session fence (a counting double), and runs the real
`ban`/`unban` adapters (`cmd_ban`, `cmd_unban` with `CommandArgs`): a ban or
unban whose save failed is reported as not saved, distinct from a full list or
an address that is not banned. Not covered: real task interleaving and the
device AES helpers. Run with
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
A source check requires `/api/backup` to refuse an overflowed or oversized
bundle with the limit `/api/restore` uses. Not covered: the HTTP handlers
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
build. The ban writer, session fan-out, web session tables, hashing and the
per-user settings loader are doubles; the OLED and lens pages themselves are
not compiled. Run with
`ctest --test-dir build -R '^user_roster_authority$' --output-on-failure`.

## LLM load policy

`llm_load_policy` compiles `handleLLMLoad`, `handleLLMUnload` and their reply
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

## Edge Impulse inference lock and continuous task

`test_edge_impulse_lifecycle.py` compiles the whole of `System_EdgeImpulse.cpp`
on the same fakes as `test_edge_impulse_jpeg.py`, with the continuous task on a
`std::thread`, FreeRTOS semaphores on `std::mutex`, and a fake interpreter that
can hold one Invoke open. Scenarios: `eicontinuous 0` then `1` inside one
interval leaves exactly one task, and a stop returns with the task gone;
`eienable 0`, a model load and an unload during a running Invoke wait for that
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
`publishPeerDiscoveryConfig`, their shared 1 KB discovery buffer and bounded
`MqttJsonWriter`, `updateExternalSensor` with its sensor-table state
(`System_MQTT.cpp`) and `macToPathToken` (`System_Utils.h`), and compiles them
against a recording `esp_mqtt_client_publish`. Cases: a short base topic
publishes the same retained discovery document byte for byte (local and mesh
peer); a 400-character base topic skips the publish with one warning instead
of writing past the buffer; a sweep of base-topic lengths across the capacity
publishes every document of at most 1023 bytes exactly and skips every longer
one; esp-mqtt's topic and payload, given in exactly-sized buffers with no NUL
(payload with an embedded NUL), are stored with exactly `topic_len` /
`data_len` bytes, and a topic length covering a prefix of a longer buffer
stores only that prefix. Source guards: no unchecked `sizeof(configJson) - pos`
remains, and the periodic state document is checked before it is published.
Not covered: the state document's sensor sections, JSON escaping of setting
text, and the real esp-mqtt client. `--case discovery|peer|sensor` runs one
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
registration, `unmountSD`/`remountSD`/`formatSD` and the
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
header staging file). It takes `--cxx` and `--sanitize`; CTest runs it
without arguments (`c++`, no sanitizers).

`web_json_replies` compiles the production `handleAutomationsExport`,
`handleIconGet`, `handleFilesCreate` and `handleMicRecordingsList`
(plus `webJsonString`, `sendJsonError` and the recordings-list
builder) against a fake httpd that keeps header pointers and reads them at
`httpd_resp_send`, with real ArduinoJson. Under `--sanitize` a header value
that dies before the send is an ASan error (the `?id=` export disposition, the
icon `X-Icon-Size`); the export filename keeps only `[A-Za-z0-9._-]`, at most
48 characters. The runner parses every printed payload with Python's strict
`json` module: recordings named with a backslash, comma, colon, quote or
control character; `files/create` errors with quotes, a backslash, a newline
and a control character (folder and file branches); a 150-character name that
must reach `filecreate` whole. Not
covered: the map, GPS track and LLM status replies (same helper, no harness)
and the recordings producer in `System_Microphone.cpp`. Run with
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
`gCLIValidateOnly`), while a second thread broadcasts, once with no command
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
web status endpoint submits through `executeInternalStatusCommand`, and who
writes `gCLIValidateOnly`). Run with
`ctest --test-dir build -R '^command_task_isolation$' --output-on-failure` (or
the runner with `--sanitize`).

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
undefined three-argument `streamBeginHtml` prototype is gone. The HTTP-off
no-op stubs in `System_SensorStubs.h` (`authSuccessUnified`,
`sseEnqueueNotice`, `sseDequeueNotice`), a file set 1 does not edit, are
exempt by name. It does not compile anything. Run with
`ctest --test-dir build -R '^dead_auth_helpers_removed$' --output-on-failure`.

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
peer metadata panel) and the `WebPage_ESPNow.h` script chunk that draws the graph
view, unmodified, against a fake `hw`/DOM on whatever JavaScript engine
`tools/webui/js_engine.py` finds (node, deno, jsc, ...). Peer names and metadata
must reach `innerHTML` as literal text, clean text must render unchanged, and the
Pair button must pass a discovered name with quotes intact. Static source
contracts on the same renderers run first; `--contract-only` runs only those.
When no engine exists and the contracts pass, the suite exits 77, which CTest
reports as skipped. Both suites take `--source-dir <component-directory>` to
replay other production sources, such as the pre-fix tree.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^(espnow_metadata_text|espnow_page_render)$' --output-on-failure
```

These checks do not open a browser or exchange real ESP-NOW frames.

## ESP-NOW dispatch policy

`espnow_dispatch_policy` extracts the production V4 opcode table, its flag constants, the lookup, the dispatcher (`v4_dispatch_table_try`), `espnowOriginIsKnown`, `v4h_text` and the espnow_task text-queue drain from `System_ESPNow.cpp`. It also extracts `LogMessageType` and `TextQueueEntry` from `System_ESPNow.h`, the opcode/flag enums and header from `System_ESPNow_Wire.h`, and `V4RxCtx`. It generates one recording double per other handler named in the table and compiles the harness with `ENABLE_BONDED_MODE` 1 and 0. It checks the following. SUBSCRIBE_UPDATE needs a paired, session-encrypted frame. Once the device has a mesh passphrase, SENSOR_STATUS, TEXT and BOOT need an authenticated frame, and a plaintext one is dropped with one warning; a keyless device still accepts plaintext. Plaintext TEXT accepted on a keyless device is filed under its origin MAC, never a paired peer's name, and posts no `SYSEVT_TEXT_RX`. Authenticated text (BROADCAST_AUTH or session-encrypted, direct or relayed) keeps its registry name, encryption flag, per-fragment records and single bus event, and BOOT stays a system notice. The retired opcode 150 has no row. Every zero-flag row is on an explicit allow-list (handshakes, HEARTBEAT, TOPO_*, USER_SYNC); this is a subset check, so later items may only shrink the list. `--component <component-directory>` or `--source-dir <overlay>` replays the pre-fix sources. It uses ASan/UBSan when `HW1_SANITIZE` is enabled and mocks history, the event bus and the radio.

## ESP-NOW fragment reassembly

`espnow_reassembly` runs the production V4 fragment reassembler from `System_ESPNow.cpp` (entry allocation, GC, `v4_reasm_accept`, `v4_reasm_release` and `v4_dispatch_post_cleanup`) on crafted plaintext fragments, with the table allocated exactly as init does. It checks in-order and out-of-order completion with learned strides (direct 200 B and routed 130 B), duplicates and GC; that a fragment whose type, count or index disagrees with its entry is dropped; that a fragCount above `V4_FRAG_MAX` gets no entry; that a completion larger than the entry's buffer is refused; and, in separate invocations, the late-stride over-size case and cleanup when the table was never allocated. Source contracts check that `v4_try_handle_incoming` reaches the table only through these helpers. `--component <component-directory>` or `--source-dir <overlay>` replays the former over-read and null dereference. It uses ASan/UBSan when `HW1_SANITIZE=ON` and does not exercise sessions, ACKs or the radio.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^espnow_reassembly$' --output-on-failure
```

(Alternative placement: as a paragraph after `espnow_disabled_interface` in "Network build selection and disabled ESP-NOW", adding `espnow_reassembly` to that section's `ctest -R` line.)

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

## ESP-NOW peer message store

`espnow_message_store` compiles the whole message-buffer section of
`System_ESPNow.cpp` (slot table, received and sent rings, chunked store, merged
reads, collapsed views, `espnowCopyCollapsedHead`, delivery stamps) with the
store's types and declarations from `System_ESPNow.h` and FreeRTOS mutex,
allocation, clock and logging doubles. Each case runs in its own process. The
cases check that a collapsed ref taken before new peers arrive still reads its
own record, that reads for unknown MACs create no slot and allocate nothing,
that history blocks never move, that the 17th peer is refused, that a ref whose
ring index was reused is refused, and that a refused or contended store lock
changes nothing and is never left held. A storing thread also races the readers
under ASan/UBSan; `--thread-sanitize` reruns that race under ThreadSanitizer.
Every case checks that no log line is written while the store lock is held.
Source contracts confirm that the OLED and G2 screens copy records through
`espnowCopyCollapsedHead` and hold no pointers into the history.
`--source-dir <component-directory>` replays the former use-after-free. No
radio, display or filesystem is touched.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^espnow_message_store$' --output-on-failure
python3 test_espnow_message_store.py --thread-sanitize
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

`espnow_tx_multiframe` compiles the production `espnowtx` namespace from `System_ESPNow_Tx.cpp` (queue, completion pool, `submit`/`submitSync`, `sendAead`/`sendAeadSync`, the dispatcher task body and `stop()`) against single-threaded FreeRTOS doubles and a stubbed radio whose multi-frame path models the per-fragment ACK wait; while a sender waits, producers submit fire-and-forget jobs and the dispatcher drains unless it is the task that is waiting. A 4 KB `sendAeadSync` from cmd_exec must run on cmd_exec with no queue drops among the 42 jobs submitted during its waits; the destination's single-frame cap decides (150 B from cmd_exec to a routed peer runs on cmd_exec, to a direct peer and exactly at 202 B it stays dispatcher work); another task's synchronous multi-frame job (httpd's 144 B FS request to a routed peer) stays dispatcher work; failure accounting, the self-deadlock guard, refusal before `init()` and after `stop()`, and the fire-and-forget path are unchanged. A second unit extracts `sendEspNowStreamMessage`, its session helper and lookup and `v4_single_frame_cap` from `System_ESPNow.cpp` and checks that STREAM lines are clamped to one frame (202 B direct, 130 B routed) on the legacy and the command-session path, with the rate limit, the cmd_exec-only session gate and suspension kept. The script checks that `System_ESPNow_Wire.h` still defines the budgets the doubles mirror. `--component <dir>` / `--source-dir <dir>` read the production sources from another tree; the f4cc542 sources fail the dispatcher-placement, queue-drop, routed-cap and four STREAM-length checks. Radio timing, real ACK loss and task stack depth are not modelled.

## ESP-NOW metadata push

`espnow_metadata_push` extracts the production metadata setters (`metaGetSet`
with the metadata text policy, `cmd_espnow_stationary`, `cmd_espnow_setname`),
`buildMetadataPayload`, `sendMetadata`, the metadata tracking block and
`metadataPushTick` from `System_ESPNow.cpp`, plus the real `SessionState`,
`EspNowEventCategory` and `V4PayloadMetadata` definitions, and runs them against
doubles for the session table, the per-peer event subscription, the encrypted
transport, settings and the clock. A local room/zone/tags/friendly name/
stationary/device name change must reach every ACTIVE session whose peer
subscribes to METADATA_PUSH as one type-55 frame after the settle window
(ESTABLISHING, REKEYING, CLOSED and opted-out peers get nothing); a burst of
edits coalesces into one frame; the deadline is millis()-wrap safe; a change
made while ESP-NOW is down is kept; a failed send is not retried; rejected
values do not arm a push; and the request/response path (forced RESP,
debounced non-forced RESP) is unchanged. `--component <hardwareone dir>` and
`--source-dir <overlay>` replay other production sources; a tree without the
push tick compiles against a no-op tick and fails on behavior. ASan/UBSan under
`HW1_SANITIZE=ON`.

```sh
ctest --test-dir /tmp/hw1-hardwareone-host -R '^espnow_metadata_push$' --output-on-failure
```

This check does not exchange real ESP-NOW frames.

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

`oled_remote_settings` compiles the production OLED remote-settings module (`OLED_RemoteSettings.cpp` whole: schema-driven module builder, non-blocking schema fetch, remote apply and peer-reply outcome), the settings editor's module source and commit routing (`OLED_SettingsEditor.cpp`), the remote-settings mode adapter (`OLED_Mode_RemoteSettings.cpp`), the ESP-NOW command-result record (`System_ESPNow`) and BondedPeer's schema sync and remote-edit policy, against doubles for the OLED command executor, the peer caches, the clock, PSRAM allocation and the display. It checks that the remote screen lists only the peer's schema rows that carry a setter (no read-only, secret or float rows, no `users`/`bond`/`wifi`, only `ntpServer`/`tzOffsetMinutes` from `clock`) with the cached values, sends `remote:<cmdKey> <value>`, reports the peer's CMD_RESP failure (`Peer: ...`) or silence (`No reply from peer` after 5 s) instead of the local "sent" result, ignores another device's reply, never runs a local setter from the remote screen, fetches a missing schema once without blocking (6 s budget), does not let the local screen keep editing a peer module, and releases every PSRAM block and String it allocated, also when an allocation fails. A static check confirms the CMD_RESP drain publishes the redacted result. `--component <dir>` runs the same cases against saved sources; the f4cc542 sources fail them (modules built from settings.json without setters, `remote:set <jsonKey>` reported as applied, the local setter run from the remote screen). The radio link and the real display are not exercised.

## BLE output privacy and console mirror

`stt_ble_privacy` compiles the production BLE output seams from `Bluetooth.cpp` (the private-STT classifier, the trace/history redaction, the directed plaintext and secure per-session senders, the async command-result callback, and the console-mirror chokepoint `sendBLEResponse` with its recipient collector `bleCollectMirrorSessions`) against a four-slot fake GATT peer table. A directed send lands only in the addressed slot; the characteristic-wide notify lands in every connected slot, as the vendored Arduino wrapper sends it. The suite checks that private STT results never reach the trace or history mirrors, that command results reach exactly the originating session, and that the console mirror (`outble`, `blesend`) reaches only logged-in non-guest sessions. A connected peer that never logged in, a peer holding a Secure Channel without a login, a guest session, a logged-out slot and a disconnected slot receive nothing. No characteristic-wide notify is used, each recipient gets its own plaintext or encrypted copy, a congested recipient misses one flush without any recipient receiving a chunk twice, and secure-required mode still refuses plaintext. `--component <dir>` points the suite at another copy of the sources (the pre-fix tree fails the unauthenticated-peer, channel-only-peer and guest cases); `--cxx` selects the compiler; ASan/UBSan follow `--sanitize` / `HW1_SANITIZE`.

```sh
python3 components/hardwareone/test/host/test_stt_ble_privacy.py --sanitize
```

## BLE Secure Channel directed delivery

`ble_sc_directed` compiles the production `System_BleSecureChannel.cpp` whole against harness-local libsodium, mbedTLS, FreeRTOS, settings and Arduino stand-ins (`ble_sc_stubs/`). It adds four pieces extracted from `Bluetooth.cpp`: the directed raw sender `bleRawNotifyToSession`, the session sender `sendBLEResponseToSession`, the console-mirror entry `sendBLEResponse` with its recipient collector, and the deferred inbound job `bleScDeferredInbound`. A two-peer fake GATT keeps one inbox per connection. A directed send lands only in the addressed connection; the characteristic-wide notify that pre-fix sources use lands in every connected one.

The suite checks that HELLO_ACK, CONFIRM_ACK, both REJECT reasons and every DATA fragment (command results and `fileread ... bin` chunks) reach only the connection and session epoch they belong to, admitted under the BLE lifecycle guard. It also checks these cases:
- A stale session epoch sends nothing.
- A session rotated mid-message stops the transfer at the next fragment without resetting the channel.
- When the server stops during a handshake (every epoch invalidated), no reply reaches the radio.
- The console mirror sends each recipient one copy, directed or over its own channel.

Crypto is a routing-neutral stand-in, so keys, radio timing and scheduling are not tested here. `--component <dir>` points the suite at another copy of the sources; the pre-fix tree fails every cross-connection case. `--cxx` selects the compiler; ASan/UBSan follow `--sanitize` / `HW1_SANITIZE`.

```sh
python3 components/hardwareone/test/host/test_ble_sc_directed.py --sanitize
```

## BLE Secure Channel policy

`ble_sc_policy` compiles the production `System_BleSecureChannel.cpp` whole against the `ble_sc_stubs/` stand-ins it shares with `ble_sc_directed`. It splices the BLE command ingress from `Bluetooth.cpp`: the GATT write entry `processIncomingBLECommand`, the deferred secure-frame and plaintext jobs (`bleScDeferredInbound`, `blePlaintextDeferredInbound`), the command tail `processBleCommandLine` with its file-browser gate, and the directed and session senders. The fake GATT keeps one inbox per connection. A write runs the GATT entry as BTC_TASK; deferred jobs queue in submission order and run as cmd_exec_task after it returns, and the executor records which command would run for which session. The FreeRTOS doubles count every tx-mutex take that may wait and every delay made on BTC_TASK.

The suite checks:
- With the secure channel required, a plaintext write from a connection without a channel gets the fixed refusal on that connection only, in plaintext. A connection with a channel gets the refusal as one encrypted DATA frame, and its encrypted commands still run.
- With no passphrase, plaintext commands run.
- With encryption optional, file-browser verbs (`fileread`, `filewrite`, ...) written in plaintext are refused with `secure_channel_required`, even on a connection that holds a channel. The same verbs inside DATA frames run. Non-file plaintext commands still run, and a verb that only shares a prefix (`filesystem`) is not treated as a file verb.
- No reply to a plaintext write waits on the secure-channel tx mutex on BTC_TASK (which serves the GATT/GAP callbacks of every link while a paced reply holds that mutex): the replies the command tail sends itself (`whoami`, login usage, `logout`, auth-required) and the required-mode refusal reach a channel holder as encrypted DATA frames from cmd_exec, and with the command queue full the writer gets one best-effort `Error: Failed to queue command` and the pool slot is returned.
- A plaintext write's pool slot (the line can be `login <user> <password>`, and the production pool prefers PSRAM) holds no byte of the line once it is released: after the command ran, after a full command queue, after the session changed before cmd_exec ran the job, and after a required-mode refusal; a shorter later write leaves no password tail.
- A source check of the command registry: `blerequireauth`, `blesecret` and `blesecure` are all superadmin-only.

Crypto is a routing-neutral stand-in, so keys and radio timing are not tested here. `--component <dir>` points the suite at another copy of the sources; the pre-fix tree fails the refusal, file-verb, tier and BTC_TASK cases. `--cxx` selects the compiler; ASan/UBSan follow `--sanitize` / `HW1_SANITIZE`. `command_ingress_guards` holds the matching source contract: every notice the GATT write entry sends itself passes `blocking=false`, and the entry never calls the command tail.

```sh
python3 components/hardwareone/test/host/test_ble_sc_policy.py --sanitize
```

## Bluetooth server stop

`ble_server_stop` extracts the production `deinitBluetooth()`, its session
and advertising helpers, the role-claim and lifecycle-guard classes, and the
`closeble` and `blemode` commands unchanged from `Bluetooth.cpp`. Host
boundaries stand in for the role transition, the Ring owner
(`g2RingPrepareForStackTeardown`), the host/controller teardown, transport
sessions and settings writes. A stop deferred by a busy role transition or
by a Ring owner that has not quiesced, and a host teardown that fails, must
reach the caller. In each case `closeble` and `blemode client` answer
`Error: ...; mode unchanged` and leave `bleMode` alone. A deferred stop must
also leave the app session epochs live and advertising on: the Ring prepare
runs before session admission closes. The exception is a companion reset: under
its outer RECOVERING claim the caller forces the host down next, so a
Ring-deferred stop closes the session epochs, clears the GATT interface id and
stops advertising before it reports failure; a nested stop under any other
claim keeps them. A retry after the Ring owner quiesces
stops the server. A clean stop keeps its reply, its role reset, its freed
state and the order Ring prepare, session close, advertising stop, host
teardown. `closeble` with no server and `blemode server` keep their old
outcomes. The suite builds with and without the G2 client. A second
translation unit compiles the real `Bluetooth.h` with `ENABLE_BLUETOOTH=0`
and checks that the stub returns `bool` true. `--component <dir>` runs the
suite against another copy of `Bluetooth.cpp/.h`. On the pre-fix source the
harness still compiles (a `void` stop is read as "reported stopped") and
fails on the reported outcome, the persisted role and the closed sessions.
`HW1_SANITIZE` enables ASan/UBSan. Real Ring/host timing still needs a device.

## Bluetooth commands and status

`ble_info_status` (see "Bluetooth status coverage") also runs the production `blesend`, `blestream` and `bleevent` handlers with the real `CommandArgs` parser and stream-flag helpers, and runs the data-service push functions against a fake directed GATT sender. Every keyword and the interval pair come from the dispatcher's arguments. `blestream sensors off` clears only that stream. Bad tokens and non-positive intervals are usage errors. `blesend` forwards the whole message. Sensor, system and event frames reach only the console-mirror recipients, by directed notify and never characteristic-wide. The checks also cover stale sessions, congestion, a missing GATT interface, a refused lifecycle lock and secure-required mode. `--component <dir>` points the suite at another copy of the BLE sources; the pre-fix tree fails at the `blestream on` case.

It also runs the production `bledisconnect` command and its owner functions (`bleDisconnectConnection`, `bleDisconnectAppClients`) against a recording GATT-server double. `bledisconnect <connId>` closes exactly that app client. The bare form, which the page-level button and the OLED and G2 items use, closes every app client and never a paired R1 ring or G2 temple link. Unknown ids, peer links and malformed ids (`abc`, `-1`, `1x`, `0x1`, `70000`) are refused without a close. The close is issued with the lifecycle guard released, and a missing BLE state, server or lifecycle lock closes nothing. A Node-free check reads `WebPage_Bluetooth.h`: each client's Kick button must pass that client's `connId` to `bledisconnect`. `--component <dir>` also points these reads at another tree; the pre-fix tree fails at the first per-id case.

`ble_info_status` also runs the production `blestatus`/`bleread` command against the same connection table: a logged-in user named "Unknown", a logged-in user and an anonymous peer. Both forms render the guarded snapshot that `bleinfo json` uses. The checks: the JSON views list the same client set (connId, authed, MAC, age, commands, slot index, with the display user "Unknown" only for a peer that has not logged in); each call takes and releases the lifecycle guard once with the 100 ms budget and reads connection age inside it; a busy guard gives an explicit unavailable reply; a change published as the guard is released does not appear; the text form keeps its exact lines; uninitialized replies are unchanged; four 64-character usernames give complete JSON; and every JSON allocation failure gives the oom reply.

## BLE area cleanup coverage

`ble_area_cleanup` runs the production connection typing from Bluetooth.cpp (`bleIdentifyDeviceByMAC`, its MAC helpers and `bleDeviceTypeToString`, extracted unchanged with BLE_Types.h and the saved-target types from BLE_Peers.h) against a host table standing in for the peer owner's saved targets. Saved glasses, ring and phone addresses classify in any letter case, an unsaved or all-zero address is Unknown, and after a re-pair the new addresses classify as the peer and the old ones as Unknown on the next lookup (`bledisconnect` keeps a link open only when it is typed as a G2 temple or the ring). Source contracts on the same files: every characteristic Bluetooth.cpp creates with PROPERTY_WRITE has write callbacks; the uncalled Bluetooth helpers and the MAC cache are absent from Bluetooth.cpp/.h; G2_Ring.cpp carries none of the unregistered ring-to-glasses spoof push or temple bridge (commands, tasks, state) while `g2RingNoteBridgePoll` stays defined for the G2 receive path, and once that code is gone G2_Ring.cpp names the commit that still holds it (f4cc542); and Bluetooth.h compiles with ENABLE_BLUETOOTH=0. `--component <dir>` runs it against another copy of Bluetooth.cpp/.h and G2_Ring.cpp/.h. It does not exercise the GATT server or a real connection.

## Ring forwarded telemetry parser

`test_ring_forwarded_telemetry.py` compiles the production
`g2RingNoteForwardedTelemetry` and its local protobuf helpers (`G2_Ring.cpp`)
with the shared `g2Pb*` readers (`System_G2_Protocol.cpp`), the production
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

`ring_daily_ack` compiles the production ring RX path (`ringProcessRxFrame`, `ringExtractTelemetryCache`, `ringReasmFinalize`, the RX duplicate filter and the packetAck lane from G2_Ring.cpp) with the production Health pending-session model (`g2HealthApply*Daily`, `g2HealthHistoryFetchStarted/Finished`, the day-key and peer checks from G2_Health.cpp) and the production profile capabilities and packetAck descriptor factories (System_R1_Protocol.cpp). The frame decoder and typed daily parsers are doubles that return the test's page. On 2.2.9 a daily page gets a packetAck only when the model accepted it into the active sweep: a page outside a sweep, in a sweep dropped behind an uncommitted result or a failing commit, with another day key, with a zero-base day or during a pending peer switch gets none, while it still refreshes the live cache; an accepted page gets exactly one ACK, and its retransmit is ACKed again without a second ingestion. 2.2.7 keeps its established flow control (every eligible daily NOTIFY, sleep and reassembled activity included, is ACKed) until a device check. `--component` / `--source-dir` read the production sources from another tree; the 0.99.97.4 sources fail the seven refused-page checks. No BLE link, ring firmware, owner-loop scheduling or store commit is simulated.

## R1 ring health controls and history

`health_logging_ui` (see the G2 Health paragraphs) also drives the R1 Health action body reader through a scripted `httpd_req_recv`: one idle wait between chunks is retried, two consecutive idle waits (a half-open socket) end the request with the 400 reply and release the httpd worker, and a socket error never retries. `--component <component-directory>` reads production sources from another checkout and `--source-dir <directory>` overlays single files; the 0.99.97.4 sources fail the half-open case because that reader retried timeouts without bound.

The ring controls run the production `g2RingCommands` and `healthCommands` rows, `cmd_ringcontrol` and the `healthstatus force-history` branch through a registry executor double (longest-prefix lookup, the registry admin gate, an audit record per executed command). A non-admin is refused with the same text from the web handler (403), the OLED page ([DENIED]) and the serial CLI, nothing reaches the ring setters, and the validate pass is refused at the same admin row; an admin's change from each transport reaches the owner and is audited as `user@web`, `user@display` or `user@serial`. The 0.99.97.4 sources fail because the web and OLED adapters checked admin themselves and called the setters, and no command existed for collection or low power.

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

## G2 Files page tasks and redraws

```sh
python3 components/hardwareone/test/host/test_g2_files_psram.py --sanitize
```

Besides the reader/viewer harness, `g2_files_psram` compiles the rest of the production Files page
(`G2_Page_Files.cpp`, includes stripped, the reader/viewer stubbed) in `g2_files_redraw_harness.cpp` and drives it from
simulated tasks: taps on `g2_tap_disp`, queued redraws on the lens applier, the rename/delete completion on
`cmd_exec_task`, the info-overlay expiry on the main loop and the image viewers' `onDone` on the session worker. The
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

The camera power post-hook (cam_pwr, after every camera power operation from any source) and the openmic/closemic
completion (cmd_exec) refresh the Sensors CAM/MIC detail. They never render or write the Sensors level or hijack
page on their own task: they queue a Redraw on the lens applier (the hook with the menu generation taken when it
fires, the mic completion with its submit-time cookie), and the render re-checks level DETAIL and page SENSORS.
`g2_sensors_redraw` compiles the whole production Sensors page (camera and microphone on, other sensors off) beside
a lens double with swap admission, a generation-checked Redraw queue and task-context tags, and runs a user tap
("Settings >", "<- Hardware") between a completion's check and its render: nothing renders on cam_pwr or
cmd_exec, Camera Settings keeps its rows and routing, the list keeps routing list taps, and an undisturbed detail is
still refreshed. It also checks that an out-of-range list tap does nothing and that the live view's level is
committed only after the live page started. `--component <dir>` replays it against a pre-fix tree. Lens, camera,
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

## G2 Network row bindings

`wifi_saved_ui` (see "Saved WiFi network coverage") also has a unit that compiles the
G2 Network page's row bindings, its WiFi, ESP-NOW, HTTP(S), Bluetooth, G2 and R1
menus and their tap handlers: each case draws a menu, changes the radio, link,
server, BLE, ring or setting state behind its labels, taps a row and checks that
the command matches the label that was shown (and that a refused render after a
submenu change runs nothing), plus the unchanged-state row maps. Each case runs in
its own process; the ENABLE_ESPNOW, ENABLE_HTTP_SERVER and ENABLE_WIFI off builds
are compile-checked. `--component` runs it against another tree.

## Automation summaries

`automation_scheduler` (see "Automation scheduler") also checks the owner's automation summaries (`automationCopySummaries` / `automationFindSummary`), one case per trigger type, built from `parseOneTrigger`: daily, weekly days, week interval, monthly, yearly, interval, manual delay, boot, event, unknown event, legacy or malformed triggers, extra-trigger counts, name fallback, cap and unreadable/malformed files. A second program compiles the whole G2 Automations page beside lens, command and file doubles: the lens shows the owner's trigger text, a tap opens the automation its row showed even after a web add/delete, Enable/Disable submits what the visible row says, and taps read nothing. `automation_scheduler_contract` also guards that the G2 and OLED pages never open or parse automations.json. Both accept `--component <dir>` to replay against a pre-fix tree.

## QuickNote interrupted saves and note tiers

`quicknote_store` runs the production `QuickNoteStore::sweep`: on both tiers it
removes only names that parse as the store's own temporary spelling inside
canonical `u<id>` notes folders, never opens a file for reading, and leaves
user-placed names, directories, transcripts and other folders alone; nothing is
promoted, because a note header carries no length. `quicknote_service` covers the
worker that runs the sweep once a boot, and `g2_notes_page` checks the lens tier
row: with a card in, the last row lists the other tier (`note list sd|internal`),
stale replies for the abandoned tier are dropped, and a new show returns to the
save tier. The f4cc542 sources fail the sweep case (no sweep) and the tier row.

## G2 page and hijack-bridge API contract

```sh
python3 components/hardwareone/test/host/test_g2_page_api_contract.py --sanitize
```

`g2_page_api_contract` keeps the G2 Files, Settings, Network and Sensors pages, the hijack command bridge and the hijack FSM free of entry points nothing calls. The pages are reached through their interactive list (`g2Show*Menu` / `g2*HandleTap`) and the `g2Build*Info` text the page registry renders; the text-dump wrappers `g2ShowFilesPage`, `g2ShowSettingsPage`, `g2ShowNetworkPage` and `g2ShowSensorList`, the no-op `g2FilesTick`, `g2CurrentCmdSeq` and `hijackEventName` must not exist, and `g2ShowSensorsMenu` keeps no copy of its rows (`gSensorsListCache` / `gSensorsListCount`). A header probe compiles `G2_Page_Files.h`, `G2_Page_Settings.h`, `G2_Page_Network.h`, `G2_Page_Sensors.h` and `G2_HijackFsm.h` with G2 enabled, with `ENABLE_G2_GLASSES=0` and with `ENABLE_BLUETOOTH=0` (the xiao_s3 board header plus a feature profile, as in `g2_disabled_header`): a probe namespace offers variadic fallbacks, and each removed name must resolve to one, so neither an enabled declaration nor a disabled inline stub may remain; the kept API keeps its signatures, and the disabled builds link and run its stubs (empty text, `Idle` state). A source contract scans every production `.cpp`/`.h` under the component (test/ excluded, comments and string literals stripped) for the nine names, and the owning files for any mention at all. `--component <dir>` replays the suite against another tree; the board header always comes from this repository.

## File browser classification and rename/delete authority

```sh
python3 components/hardwareone/test/host/test_file_kind.py --sanitize
```

`file_kind` compiles the shared classifier `FileBrowser_FileKind.h` and, over one table of names, the production G2 Files page file-tap branch (the `else` block of `g2FilesHandleTap` with the classification it calls, `showFileChooser` recording the chooser kind) beside the OLED browser's "View" decision (its `sFbActionViewable` assignment). The lens must offer a text or JSON view exactly when the OLED offers View: `.log`, `.md`, `.ini`, `.yaml`, `.html`, `.c` and the other OLED text types open in both, BMP/JPEG open only on the lens, and png, bin, `.tar.gz` and names without an extension get Info/Rename/Delete only. A second harness runs the OLED browser's rename/delete helpers (`fbRenameSelected`, `fbDeleteSelected`, `fbSubmitMutate`) against FileManager and executor doubles: both submit the admin-only `filerename` and one-shot `filedelete ... confirm` commands with the selected entry's full path (a 70-byte SD name), re-scan only after the executor's `OK`, report the executor's admin refusal, and submit nothing for an unresolved selection or a quote. Source contracts check that the OLED browser no longer calls `FileManager::renameItem`/`deleteItem`, that its prepare pass routes Rename/Delete through those helpers under the identity guard, that guests are still stopped on the input path, and that neither browser keeps a private extension list. `--component <dir>` reads the production sources from another tree; the f4cc542 sources fail the agreement checks (the lens gives `.log` and `.md` no viewer while the OLED offers View) and the contracts (direct FileManager rename/delete). No device, executor or FileManager is run.

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

Suggested one-sentence addition to the existing `settings_mutation` paragraph: In the
OLED-enabled variant it also runs the dedicated `oledenabled` command with display
boundaries: the generic on/off/true/false/1/0/enable/disable tokens start or tear down
the panel as before, and any other token (or none) is refused before it persists or
touches the display. `--component` points the suite at another tree.

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

`oled_owner_requests` compiles the production `requestOLEDMode`, the owner-request queue, `cmd_oledmode`, `cmd_oledtext`, `cmd_oledclear`, `cmd_oledanim` and `oledUpdate` from OLED_Utils.cpp with `cmd_setpattern` (`setgamepadpassword`) from OLED_Mode_SetPattern.cpp. A task double makes `xTaskGetCurrentTaskHandle()` answer the main loop or cmd_exec, and fakes record which task touched the framebuffer, ran an onEnter hook, read the display role or auth mirror, pushed the back stack or auto-started input. Called as cmd_exec, every command and `requestOLEDMode` leave `customOLEDText`, the mode, the stack, the animation and the pattern flow unchanged until the owner runs `oledUpdate()`. The owner then applies them in post order with the same side effects as before: `oledmode menu` auto-starts input; `oledmode off` and `oledclear` blank the panel on the owner, and `oledclear` leaves the page clean; `setgamepadpassword` pushes the previous mode and restarts the flow; the auth gate still redirects to the login page. A command posted before a session boundary hands over its text or reset but does not navigate after that boundary. A command posted after a boundary the frame has not applied yet waits for it. A ninth request drops the oldest without leaking its text copy, and nothing allocates inside the queue lock. A source check requires `updateOLEDDisplay()` to drain the requests right after its first session-boundary apply. `--component` / `--source-dir` read production sources from another tree; the f4cc542 sources fail 26 checks (state written on cmd_exec) and the source check.

## OLED mode availability

```sh
python3 components/hardwareone/test/host/test_oled_mode_availability.py --sanitize
```

`oled_mode_availability` compiles these from the production tree: `getMenuAvailability`, with its `oledModeFeatureState` classification and `oledModeRuleAllows`; the mode registry (`registerOLEDMode`, `registerOLEDModes`, `findOLEDMode`) from OLED_Utils.cpp; and `OLEDModeEntry`, the callback typedefs and `MenuAvailability` from OLED_Display.h. It registers the real entry rules of CLI Input, Gamepad PW, Logout, GPS (i2csensor_pa1010d_oled.h) and thermal through the registry, with GPS and thermal compiled in. A module's refusal makes its launcher row FEATURE_DISABLED with the module's reason. Without a named display session, CLI Input and Gamepad PW show "Login required" and Logout shows "Not logged in". The central classification keeps its class and reason: GPS or thermal not detected is NOT_DETECTED, detected but stopped is FEATURE_DISABLED "Disabled / Press X to start", and a mode that is not built is NOT_BUILT. Modes without a rule are unchanged, and a note an allowing rule leaves never reaches the launcher. A source check requires `requestOLEDModePrepared()` to be declared in OLED_Display.h beside `requestOLEDMode()` and in no other file. `--component` / `--source-dir` read production sources from another tree. On f4cc542 the suite fails five checks (the rules are ignored) and the prototype check.

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

The live replacements must stay defined: `oledUIRender`, the toast and ribbon renders, the live drawing helpers `oledDrawIcon` and `oledDrawLevelBars`, the `*Rendered` displays, the mode registry, back-navigation and the slug functions. The suite then compiles the production `enum OLEDMode`, `modeFromSlug` and `slugFromMode` and checks three things: `oledmode unified` names no mode, no mode's slug resolves to a different mode, and "actions"/"imuactions" still open IMU actions. `--component`/`--source-dir` read production sources from another tree. On f4cc542 the contract names 65 identifiers, and the slug harness fails 2 checks: "unified" opens a mode, and `slugFromMode(OLED_UNIFIED_MENU)` is "actions", which opens IMU actions.

`oled_without_input` also accepts `--component`/`--source-dir`/`--sanitize`. It requires that the display-state API carry no bonded data-source selection.

## OLED action feedback

```sh
python3 components/hardwareone/test/host/test_oled_command_feedback.py --sanitize
```

`oled_command_feedback` compiles the shipping OLED action helper (`oledRunCommand` and its
reply-to-toast mapping `oledCommandReplyText`, both from `OLED_Utils.cpp`) and real callers:

- the Automations toggle and delete actions with their footer toast (`OLED_Mode_Automations.cpp`);
- the Quick Settings radio, Wi-Fi, Bluetooth and HTTP rows (`OLED_SettingsEditor.cpp`);
- the Network menu's HTTP confirm callbacks (`OLED_Mode_Network.cpp`).

The OLED command executor (scripted verdict and reply), the global toast, the Automations
rows and the radio/BLE/HTTP state are doubles.

The checks:

- A refused or failed command toasts its reply: an admin or super-admin denial as
  "Admin required", any other reply as its first line without the `Error`/`ERROR`/`[ERROR]`
  token, an empty or bare `ERROR` reply as "Failed". An `Error` reply fails even when the
  executor says ok, and an over-long reply stays inside the toast buffer.
- An accepted command returns true and leaves the toast to the caller.
- `oledCommandReplyText` (also used by the Add-WiFi flow) gives the caller's success text on
  success and the caller's fallback text for an empty refusal, and stays inside the caller's buffer.
- Each caller shows its success text ("Enabled", "Deleted", "Radio OFF", "HTTP OFF", ...)
  only on success; local refusals ("Radio off!", "Need WiFi first!") still run no command.
- Static: the Automations, Logging and Network screens, the Quick Settings section and the
  unavailable-page / Web Stats actions (`handleOLEDActionButton`) no longer fire commands
  whose reply is dropped, and their `broadcastOutput` success lines are conditional.

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
- A guest is still blocked ("View-only"); Sleep 20s and Screen Off run their commands as before.
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
  `automationadd ... enabled=1` line whole, and so does a command of the keyboard's full
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
and the G2 join-a-scanned-AP flow (`G2_Page_Network.cpp`). Both build a quoted `wifiadd` line that the
command parser splits at every double-quote, so a double-quote or control character in the typed password
or in a scanned SSID must be refused before any line is built, with a toast (OLED) or lens text (G2). The
OLED toasts the owner's reply after an add through OLED_Utils' shared `oledCommandReplyText` ("Network saved",
"Connected", "Admin required" or the refusal's first line without its `[ERROR]`/`Error` token) and stays silent for a replacement display session; G2 keeps showing the owner's reply
on the lens. The OLED keyboard, session epoch and executor, and the G2 command bridge, text entry and lens are
host doubles. `--component <dir>` replays the cases against a pre-fix tree.

## OLED keyboard masking and dictation UI

`dictation_ui` compiles the shared OLED/G2 dictation delivery: the G2 text-entry
pad, `kbdPadConsumeDictationText`, `kbdPadDictationServiceOnTap` and
`oledKeyboardDictationTick`.

A second unit compiles the shipping OLED virtual keyboard from `OLED_Utils.cpp`:

- the character tables and `getCharAt`;
- `oledKeyboardInit` and `oledKeyboardReset`;
- the preview helper and `oledKeyboardDisplay`.

It uses the real `OLEDKeyboardState` and the Set Pattern openers, and draws on a
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
  - first-time setup password prompts.

  The SSID does not pass it, and first-time setup no longer writes `*` into the
  shared buffer.

Related checks in other suites:

- `wifi_saved_ui`'s Add-WiFi unit checks that the OLED password keyboards open
  masked and the SSID keyboard opens plain.
- `setup_display_cancel` checks that a first-time-setup password prompt opens
  masked and that its render leaves the field buffer intact.

`--component` points `dictation_ui` and `setup_display_cancel` at another tree.
ASan/UBSan follow `--sanitize`. The panel driver, fonts, and the user-manager
password fields (OLED_Mode_UserManager.cpp, outside this set) are not covered.

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
- A re-sent request (new arrival time) opens once more; an expired one answers
  quietly.
- A session boundary drops the dialog without an answer and the next session
  sees the request once.
- Idle frames make no role query, and the prompt never reads `users.json`.

`--component`/`--source-dir` point it at another tree. ASan/UBSan follow
`--sanitize`. The real commands, the role cache and the radio are not run.

## OLED list navigation

```sh
python3 components/hardwareone/test/host/test_oled_list_nav.py --sanitize
python3 components/hardwareone/test/host/test_sensorlog_ui.py --sanitize
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
"Configure" list with the shipping scroll code (`OLEDScrollState`, the scroll primitives and both
renderers from `OLED_Utils`) through the real `handleLoggingModeInput` and render block, walks the
11 rows down and up, and checks every frame: rows inside the content area, every glyph on the 128x64
panel, a visible scroll indicator while rows are hidden, the selected row drawn, and as many rows as
the content area holds.

`--component`/`--source-dir` point both suites at another tree; the f4cc542 sources fail them (the
Logging list's arrows are drawn at x=128, off the panel, with 4 rows shown; Automations and Files
drop a nav event or press that follows the previous one within 200 ms; the editor's function-static
item offset goes negative and shows a spurious down arrow in the next module). ASan/UBSan follow
`--sanitize`. Physical input timing, the ANO encoder and the real display are not run.

## OLED Map actions, input devices and waypoint saves

```sh
python3 components/hardwareone/test/host/test_oled_map_actions.py --sanitize
```

`oled_map_actions` compiles the production OLED Map mode (OLED_Mode_Map.cpp: the Map state block, the track scan, the menu dispatch, `gpsMapInputHandler` and `displayGPSMap` through map centering) against owner, keyboard, file-picker and VFS doubles. It also compiles HAL_Input's real button mapping (the controller tables, `inputGetButtonMask`, `inputIsButtonPressed` and `INPUT_CHECK`, with the seesaw and ANO bit definitions) and `NavEvents`. The Map unit is built four times: for the seesaw gamepad with `ENABLE_GPS_SENSOR` 0 and 1, for the ANO rotary encoder, and for the GPIO wheel with the P4X-EYE keys. Each build feeds the handler what the OLED input router hands a mode on that device: the device's own press bits; the wheel's detents with up/down nav events; ANO dpad presses as nav events; and stick deflection, which produces a nav event only on the router's first-deflection and auto-repeat cadence. The identity double mirrors the per-task slot: `CommandIdentityScope` installs a context, and otherwise `currentAuthContext()` returns the empty anonymous one, which the owner and VFS doubles refuse as `checkPerm` does. Map actions: Mark Waypoint adds the typed name at the map center once the keyboard completes. A cancel adds nothing, and a refused write or a full table is shown as a toast. Goto Waypoint, Clear Nav, Delete Waypoint, Load Track, Delete Track, Next Map, Previous Map, the first-map auto-load (at most one attempt per 5 s while no map loads) and the load from the Select Map picker all run as the OLED user, and the track picker scans with that identity. Select Map opens the file browser as a `/maps` picker that shows only `.hwmap` files. Input, on every device: START opens the menu. The wheel or stick moves through the menu, the track and waypoint pickers, the features list and the routes list. A confirms and B goes back or closes. Search results step with left/right or the wheel. The info and track-status overlays close on A or B. In the routes viewer X highlights a route. On the map, X zooms in, Y zooms out and B leaves. ANO UP moves up instead of backing out, a GPIO double tap (SELECT) does not confirm, and a held stick moves the menu only on the router's nav events. A second unit compiles the production WaypointManager mutators, `cmd_waypoint`, `cmd_waypointfile`, CommandArgs and `requireQuotedToken` (System_Maps.cpp, System_Command.cpp, System_Filesystem.cpp) with a `saveWaypoints()` double. `waypoint add`, `waypoint del` and `waypointfile` report a write that fails, and the waypoint table and navigation target stay unchanged. Adds and deletes that save keep their replies. `--component <components/hardwareone>` reads all production sources from another tree. The f4cc542 sources fail 30 Map checks in each seesaw build, 59 on the ANO build and 55 on the GPIO build, plus 11 save checks.

## OLED Map context bar

```sh
python3 components/hardwareone/test/host/test_oled_map_context_bar.py --sanitize
```

`oled_map_context_bar` compiles System_Maps.h's `LocationContext`, the context-bar append helper from OLED_Mode_Map.cpp and the `displayGPSMap` block that builds "On <road> • Near <area> (<n>m)" in its 128-byte `contextBuf`, all unchanged. Inside them `snprintf`/`vsnprintf` follow the C contract and store every byte from instrumented code, so ASan sees any write outside `contextBuf`. Each case runs in its own process: short names, area only, road only, no names, a 127-byte exact fit, and road and area names of 58+58, 58+57 and 63+63 characters with the area near and far. The bar text must be the whole context cut at 127 bytes, stay terminated inside the array, and `pos` must equal its length and stay below 128. `--component <dir>` reads the production sources from another tree; the f4cc542 sources fail four cases: `pos` ends at 134 or 139, and with two 58- or 63-character names and a near area the third append forms `contextBuf + 129` (UBSan bounds error; with ASan alone, a stack-buffer-overflow write).

## Bond manifest and schema buffers

`espnow_bond_json` guards the bond manifest (every command's name and help, 70,773
bytes on xiao-default today) against the 65,535-byte cap of an Arduino `String` on boards
without PSRAM support in the core. Source contracts: `sendBondManifest`,
`sendBondSchema` and `cmd_bond_showmanifest` serialize through `serializeBondJson`;
`v4h_file_end` hands the transfer buffer to the manifest and schema handlers without a
`String` copy; `cmd_bond_showremotemanifest` and the fwHash branch of
`handleEspNowRemoteManifest` read through `espnowReadCachedManifest` and the web branch
takes no filesystem lock itself. It then compiles the production `bondJsonEnvelopeOk`,
`serializeBondJson`, `writeBondJsonFileLocked`, `cacheManifestToLittleFS`,
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
parity (same input tensor, same results, same eiEnabled and no-model errors;
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
