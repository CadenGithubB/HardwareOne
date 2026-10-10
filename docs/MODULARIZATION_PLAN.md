# Code simplification plan

Status: incremental implementation started, 2026-10-08; latest focused work
2026-10-10. The sequence below describes planned work; completion records at the
end describe each implemented slice. The shared
[development guidelines](../CONTRIBUTING.md) remain the source of coding rules.

**Release direction, 2026-10-10:** `0.99.97.x` is the cleanup and unification
series, focused on long-term readability and editability. `0.99.97.1` was the
first checkpoint; `0.99.97.2` checkpoints the follow-on work through shared
setup menu organization. Subsequent milestones follow
the [cleanup-series guidance](../CONTRIBUTING.md#the-09997x-cleanup-series), with
a bounded scope and an explicit completion condition for each change.

**Latest code milestone: shared setup menu organization.**

Advanced setup groups compiled options under Connections, Display & Controls,
Sensors and System. Registry rows are sorted by name, companion settings stay
beside their parent, and serial/local-display adapters share the rows, values,
actions and page order. Startup settings explicitly show boot behavior. The
opening Basic/Advanced/Import menu also shares its labels and availability.

**Previous milestone: shared initial-admin creation.**

Basic/Advanced and post-restore setup now use one account-creation operation.
It checks credential and user-record preparation, saves credentials before the
user index, and reports success only after both writes succeed. Basic/Advanced
setup returns to its menu on failure instead of declaring setup complete.

**Previous milestone: shared wizard draft and completion.**

On-device serial and local-display setup now edit one private draft. Page
visibility, feature/mode selections and estimates read those choices; Finish
is the only point that publishes edited settings and applies live effects.
Cancel, timeout and session loss discard the draft. P4 already initializes
its LCD and GPIO controls before first-time setup and uses this same interface.

**Previous milestone: shared runtime setting mutation.**

Generic setting commands and feature toggles now use the existing `setSetting`
path for changed values, persistence requests and notifications. Generic numeric
and boolean inputs are validated completely before mutation. Existing service
actions, authorization and owner-scoped save batching retain their contracts.
The wizard milestone builds on that path; neither slice adds a new build-flag
owner or changes the host configuration tool's role.

**Previous milestone: automation scheduler ownership.**

`System_Automation` now owns the main-loop scheduling decision and cache
lifetime. Consumers use operations instead of exported running/dirty state.
Cache reads, publication and release share a short lock; pending changes and
event hints are atomic. External rule-file saves always request a refresh.
The completion record below covers this slice and the combined regression run
for the recent Wi-Fi, Map snapshot and automation logging changes.

**Previous milestone: automation logging ownership.**

Automation log state, captured writer identity, start/stop and checked appends
now live together in `System_Automation`. Command execution keeps its redaction
hooks and calls the owner's small interface. Session changes and file writes
are serialized; status reads use a short state lock. The completion record
below distinguishes executable host checks and firmware builds from hardware
qualification.

**Previous milestone: consistent GPS track reads.**

`GPSTrackManager` owns track synchronization and returns copied status or owned
point snapshots. OLED, web, CLI, saving and rendering no longer borrow its
replaceable buffer. Loads publish a complete result; failed or rejected loads
retain the previous track. The completion record below separates host/build
verification from hardware qualification.

**Previous milestone: saved Wi-Fi module layout.**

The saved-network owner and its public/internal interfaces now live together in
[`network/wifi`](../components/hardwareone/network/wifi/README.md). That module
map identifies the radio, settings, setup, boot and UI integration points. The
three relocated files are byte-for-byte unchanged; production includes, the
explicit CMake entry and affected host harness paths use the new location.
Nine focused suites and QT Py/XIAO builds passed. No forwarding files, extra
include directories or new IDF components were introduced.

This completes the original first round's implementation criteria: shared setup
operations, focused utility owners, saved Wi-Fi state ownership, one reduced
build-rule family and a proven module layout. It does not mean every subsystem
has migrated or every hardware combination is qualified. Work continues in
bounded packages. Remaining build-rule duplication and other service owners
are separate candidates beyond the completed automation and track owners.

**Previous milestone: shared Map live recording.**

OLED and web now submit one authorized Map recording command. The existing
Maps owner checks GPS/logger outcomes, preserves an imported track on startup
failure, and records the logger session it started. Stopping Maps cannot stop
a replacement capture at the same filename. The logger's shared startup and
settings ownership remain the foundation. Dated records below distinguish
host/build verification from device qualification. The checkpoint Map image
subsequently passed 33 command/lifecycle checks on the QT Py; settings were
restored. [The hardware record](LOGGER_TEST.md#map-live-recording--2026-10-10)
describes the no-fix GPS condition and remaining UI/coordinate limits.
Memory slimming and
qualification of simultaneous BLE/ESP-NOW operation are separate future work;
these radios can be tested separately for now. SD work is deferred.

The goal is to make a feature understandable and changeable through a small,
predictable set of files. Related behavior should have one owner, callers should
use small interfaces, and each cleanup should remove an existing dependency or
duplicate. File count and line count alone do not measure progress.

**Keep the user configuration experience.**
[System_BuildConfig.h](../components/hardwareone/System_BuildConfig.h) remains
the human-editable home for shared build flags. Preserve the existing flag names,
grouping, supported editing format, and board/profile precedence as consumers
migrate. The [setup script and saved configurations](../configurations/README.md)
remain an additional way to choose features. Generated inputs stay in build
directories. The runtime utility files `System_Utils.h/.cpp` are a separate
cleanup target.

Boards remain general-purpose, with lean defaults, real backend/hardware
constraints, and measured resource limits. Features do not all have to fit at
once. Existing authentication, saved-data formats, partition layouts, and
release contracts remain part of the compatibility baseline.

**Existing foundation:** saved configurations and guided host setup use shared
functions; `System_CommandRegistry.h` provides a narrower command metadata
interface; the project already has HALs, settings and command registries, shared
dictation operations, and host tests. Extend these pieces. User testing of the
new host wizard is still pending and can inform follow-up fixes independently
of the runtime cleanup.

1. **Record the baseline for the first change.**

   **Work:** Choose the MQTT page in the on-device setup wizard as the first
   bounded area. Map its three callers, settings owner, persistence calls, and
   completion/cancellation behavior. Use the existing ownership guide and test
   suites. Record the affected profile's resolved features and image headroom
   alongside a relevant baseline before changing firmware. Capture only the
   behavior needed for this slice; repeat this short exercise for later slices.

   **Purpose:** Make it possible to distinguish a structural cleanup from a
   behavior change and to verify that working configurations still build.

   **How it advances the goal:** Every following change has a specific boundary
   and an observable result, keeping the work reviewable.

   **Done when:** The first slice lists its callers, owner, behavior cases, and
   validation commands. The editable-header and saved-configuration entry
   points are explicit compatibility requirements.

2. **Share the on-device MQTT setup operation.**

   **Work:** Consolidate the repeated settings application in
   [System_SetupWizard.cpp](../components/hardwareone/System_SetupWizard.cpp),
   [System_SetupWizardMode.cpp](../components/hardwareone/System_SetupWizardMode.cpp),
   and [OLED_SetupWizard.cpp](../components/hardwareone/OLED_SetupWizard.cpp).
   Give the shared operation responsibility for interpreting and applying the
   supplied fields. Reuse the existing MQTT settings metadata and persistence
   path. Each interface keeps input collection, prompts, rendering, and
   navigation.

   Record blank, skip, back, cancel, and partial-entry semantics first. The
   current paths differ in skip/autostart and blank-port behavior, and blank
   credentials need a clear keep-versus-clear meaning. Start by preserving
   behavior during extraction; describe any intended behavior correction
   separately before implementing it. Keep the shared operation independent
   of display availability and physical input.

   **Purpose:** Give the same setting one implementation across user interfaces.

   **How it advances the goal:** This becomes a small working example of the
   same design used by the host setup tool: shared behavior with thin interfaces.
   It also makes future setup changes easier to apply consistently.

   **Done when:** All three callers use one application operation; duplicated
   field assignments are removed; agreed input/cancellation cases exercise the
   production operation; MQTT-enabled and relevant disabled/UI combinations
   compile. Broker connectivity is outside this settings-only slice; the user
   has explicitly skipped live broker testing.

3. **Separate runtime utilities by responsibility.**

   **Work:** Finish the command-registry separation first: place registry
   implementation in a dedicated owner and migrate its callers to the narrow
   interface already introduced. Follow with a separate time-synchronization
   slice that works with the existing `System_Clock` interface. Later, evaluate
   diagnostics and icon helpers individually. Keep authorization, execution
   ordering, and session handling together in their existing command path
   during the registry-only extraction.

   Move implementations and their state together where they belong. Use direct
   includes at callers. Remove redundant declarations and compatibility imports
   once their consumers have migrated. Correct stale ownership comments in
   touched code. `System_Utils` can retain genuinely shared, focused helpers;
   deleting the filename is not a completion requirement.

   **Purpose:** Let callers depend on the operation they need without importing
   unrelated runtime subsystems.

   **How it advances the goal:** A reader can find command registration or time
   synchronization in an obvious place, and changing one responsibility affects
   fewer unrelated consumers.

   **Done when, per extraction:** One implementation owns the moved behavior;
   old declarations/imports are removed; migrated public headers compile with
   their declared dependencies; relevant enabled/disabled builds and existing
   behavior tests pass. Registry extraction and time extraction are separate
   reviewable changes.

4. **Give saved Wi-Fi networks one state owner.**

   **Work:** Bring saved-network storage and lifetime behind Wi-Fi operations.
   Today, startup allocates the array, Wi-Fi and settings code modify it, and
   OLED, glasses and CLI code read its storage or count directly. Reuse existing
   add/update/remove/priority operations and introduce only the missing read or
   persistence interfaces.
   Screens receive views that omit credentials. Settings loading and saving
   use a deliberate integration point with the existing persistence system.
   Startup invokes initialization instead of constructing Wi-Fi internals.

   Preserve initialization-before-settings-load ordering, allocation policy,
   network order/capacity, encrypted credential handling, and existing saved
   files. Specify which task owns mutation and how readers obtain a consistent
   view. Keep permission checks on every existing command/UI entry path.

   **Purpose:** Make one module responsible for maintaining valid Wi-Fi state.

   **How it advances the goal:** Changing storage or connection selection no
   longer requires unrelated UI and startup code to understand the underlying
   array. Failures and resource lifetime become easier to reason about.

   **Done when:** The raw mutable array is private; startup and UI use deliberate
   operations; load/save round trips, ordering, capacity, allocation failure,
   and Wi-Fi-disabled behavior are covered as relevant. Check connection and
   reboot persistence on available hardware for lifecycle-affecting changes.

5. **Reduce duplicated build rules while preserving editable flags.**

   **Work:** Take one small rule family, such as network/web feature gates, and
   trace it from editable defaults and overrides through CMake source selection
   to compiler results. Establish an authoritative resolution path for that
   family and remove its redundant policy copy after equivalence is verified.
   Keep `System_BuildConfig.h` as the editable entry point and preserve its
   existing public flag interface. The setup tool continues to use shared
   discovery and validation.

   ESP-IDF discovers component requirements before final SDK configuration is
   available. Prototype and verify those phases before choosing a resolver
   mechanism. Conservative source inclusion can be valid; omitting an enabled
   implementation cannot. Keep final compiler/Kconfig checks and linked image
   size authoritative. Expand to another family only after the first works.

   **Purpose:** Make a new feature or dependency require fewer coordinated edits
   across build scripts, headers, and setup tooling.

   **How it advances the goal:** Manual configuration and the guided tool retain
   one consistent meaning as the runtime modules become more independent.

   **Done when, per family:** One redundant rule implementation is removed;
   equivalent inputs through supported entry points produce matching resolved
   features; enabled implementations reach the compiler; clean and incremental
   builds work; affected profiles fit their unchanged partitions. Include a
   relevant second board/backend when the changed rules span targets.

6. **Group files around the established modules.**

   **Work:** Start with a module whose ownership is now clear, such as saved
   Wi-Fi networks. Place related implementation, public interface, commands,
   settings integration, and tests in a predictable arrangement. Use a nearby
   test location compatible with the existing host harness. Document a short
   map from a feature to its entry points. Keep shared UI rendering, storage,
   and hardware infrastructure with their own owners.

   Move one module at a time and update CMake paths, includes, test harnesses,
   tools, and documentation. Keep file relocation separate from behavior
   changes. Preserve readable boundaries without requiring every feature to
   have the same number of files or introducing a new IDF component for each
   directory. The central user configuration file stays easy to find.

   **Purpose:** Make the source tree reflect how developers think about features.

   **How it advances the goal:** A reader can locate a feature, understand its
   interface, and follow its implementation without searching the whole project.

   **Done when:** A representative change can be traced through the feature's
   documented files and explicit shared interfaces; paths and builds work;
   obsolete forwarding files and duplicate implementations have been removed
   when their callers are migrated.

7. **Apply the proven pattern to further feature work.**

   **Work:** Choose the next module using observed duplication, exposed state,
   and actual development needs. Potential later slices include other setup
   pages, remaining utility responsibilities, and sensor consumers. Extend
   existing HAL/service interfaces when multiple callers share an operation.
   Prefer descriptive names, explicit units/results, and short functions that
   express meaningful steps. Write interface comments about ownership and
   asynchronous behavior where those details affect callers.

   **Purpose:** Make maintainability part of normal development rather than an
   ever-growing parallel rewrite.

   **How it advances the goal:** New features follow examples already tested in
   this codebase, and each migration reduces the amount of legacy coupling.

   **Done when, per change:** The owning module and its callers are clear, the
   new shared path replaces an old path, tests exercise the production behavior,
   and a short change note records what became easier to understand or modify.

**Execution order and review size.** Steps 1 and 2 are the first milestone.
Steps 3 and 4 follow as separate changes. Step 5 is a distinct build-system
workstream and can proceed after the baseline when it does not overlap active
firmware edits. Step 6 follows the ownership work for each chosen module.
Step 7 is the continuing development practice. Each step can contain several
small changes; this plan does not require a single large branch or simultaneous
refactoring of every subsystem.

**Verification belongs to every change.** Use focused host tests for shared
behavior, relevant enabled/disabled combinations for optional modules, and an
affected firmware profile plus a baseline for runtime/build changes. Reuse the
existing [host test instructions](../components/hardwareone/test/host/README.md)
and saved configurations. Record size/headroom changes and exercise hardware
when lifecycle, timing, or drivers change. A build does not establish hardware
qualification. The absent SD card and skipped MQTT broker test are known test
conditions, not refactoring failures.

**Measure the outcome for each migrated area.** Record a small before/after:

| Question | Evidence of improvement |
| --- | --- |
| How many independent places implement this behavior? | Duplicate implementations removed |
| Who can mutate this state? | Fewer external writers; one clear owner |
| What must a caller include or understand? | Narrow public interfaces; fewer unrelated dependencies |
| Where would a developer make a representative change? | A predictable set of files and named shared operations |
| Does a supported optional configuration still work? | Focused tests, successful builds, measured headroom, relevant hardware checks |

The first round succeeds when setup shares its settings operation, extracted
utilities have clear homes, saved Wi-Fi networks have one state owner, a build
rule family has fewer policy copies, and one module has a proven source layout.
Manual flag editing and guided configuration remain usable throughout.

**Implementation record — 2026-10-08, first slice of steps 1–2.**

The serial, OLED, and CLI-mode MQTT setup handlers now call
`wizardApplyMQTTSettings` in `System_SetupWizard.cpp`. The function applies
the four collected broker fields to the existing in-memory settings. The
three previous implementations have been replaced by one, without introducing
a new module, settings schema, dependency table, or persistence mechanism.
Its declaration documents the boundary; the result's port comment now correctly
describes zero as keeping the current setting.

The behavior baseline for this extraction is:

| Case | Preserved behavior |
| --- | --- |
| Apply collected fields | Nonempty strings and positive ports replace current values; other fields keep their current values |
| Empty credentials | Existing credentials remain; clearing them is outside this extraction |
| Blank port | Serial/OLED produce zero and keep the saved value; CLI mode selects 1883 |
| Skip at the introduction | Serial/OLED disable auto-start; CLI mode advances without changing it |
| Back/cancel before the apply point | Existing return paths keep collected broker fields from being applied |
| CLI next during field collection | Applies the fields collected so far at the existing apply point |
| Cancel after an earlier application | Existing in-memory changes remain; final wizard saving is skipped |
| Validation and persistence | Existing input parsing, accepted values, and final save path are preserved |

The shared helper remains available in MQTT-disabled builds because wizard
handlers and the settings fields exist in those builds; page visibility decides
whether the user reaches MQTT setup. Input normalization and settings-metadata
validation remain separate planned work. The known differences above are
documented behavior to revisit deliberately, rather than newly chosen defaults.

Firmware verification used the saved profiles below. Resolved feature flags,
SDK settings, and partition-table hashes match their pre-extraction manifests.
The app partition remains 5,984,256 bytes for all three profiles.

| Profile | Before (bytes) | After (bytes) | Remaining app space (bytes) |
| --- | ---: | ---: | ---: |
| `xiao-mqtt` | 5,784,272 | 5,784,032 | 200,224 |
| `xiao-default` | 5,883,136 | 5,882,960 | 101,296 |
| `xiao-display` | 5,730,816 | 5,730,432 | 253,824 |

These are compile/link results; this extraction did not require a device flash
or claim a physical broker/display test. MQTT feature-selection tests and the
display-without-input host check also pass.

The `setup_mqtt_application` host test executes the production helper and all
three MQTT input handlers with platform/input substitutes. The same handler
cases passed against source snapshots taken before this extraction and the
current code, under AddressSanitizer and UndefinedBehaviorSanitizer. The
registered CTest also passed with `HW1_SANITIZE=ON`. Coverage includes partial
updates, back/cancel, blank values, existing skip differences, and absence of
save/start side effects at this application step. The test does not execute
the whole wizard's persistence or physical input/rendering.

**Implementation record — 2026-10-08, command catalog extraction in step 3.**

`System_CommandRegistry.cpp` now owns the compiled module catalog and its
`getCommandModules` accessor. The 695-line catalog moved without changing its
53 source rows, help text, order, feature guards, command/count bindings, or
availability callbacks. `System_Utils.cpp` shrank from 6,411 to 5,577 lines after
removing the catalog, registry-only declarations, and ten unnecessary module
includes. Three command consumers also dropped their broad utility imports.
Ownership comments now point to the catalog's actual home.

Core and battery command tables remain with their existing implementations and
now have explicit external linkage so the catalog can reference them. The
catalog remains in `.flash.rodata` in all four linked firmware images. Command
lookup still lives in `System_Command.cpp`; permission checks, dispatch,
redaction, executor, and session code in `System_Utils.cpp` are unchanged.
The public registry header stays independent of Arduino and hardware headers.

The new `command_catalogue` host test compiles the production translation unit
with module/provider fixtures. Seven resolved feature selections cover all 53
rows and the separate Bluetooth/G2 guard combinations. The original catalog
and extracted implementation matched under AddressSanitizer and
UndefinedBehaviorSanitizer. All nine targeted catalog, header, lookup, command,
help, and QuickNote tests passed. Existing command-documentation tooling still
finds the same 53 source module rows.

| Profile | Image (bytes) | Change from immediate baseline | Remaining app space (bytes) |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,882,944 | -16 | 101,312 |
| `xiao-mqtt` | 5,784,048 | +16 | 200,208 |
| `xiao-display` | 5,730,400 | -32 | 253,856 |
| `xiao-no-bluetooth` | 4,349,536 | Additional build check | 1,634,720 |

All four builds passed. The first three profiles retain their baseline feature
flags, SDK settings, and partition hashes. A temporary battery-enabled override
also compiled the battery, utilities, and catalog translation units and verified
the core/battery table exports; this is a compile check, not battery hardware
qualification. The final ownership-comment edits have no executable effect.

The NTP extraction below follows this catalog change. Other utility
responsibilities remain candidates for later slices of step 3. Manual build
flags and saved configuration inputs are intact.

**Implementation record — 2026-10-08, NTP extraction in step 3.**

Inspection confirmed that clock consolidation was already implemented:
`System_Clock` owns validity, timezone, formatting, source tracking and shared
post-step duties for NTP, RTC, manual and peer time sources. `System_TimeAnchors`
owns capture timestamp anchoring. Their implementations remain unchanged.

`System_NTP.cpp/.h` now owns only the remaining network synchronization work:
client setup, callback state, reply/status counters, foreground waiting and the
main-loop handoff to `Clock::clockStepped`. The 325-line implementation body
moved unchanged from `System_Utils.cpp`; the utility file shrank from 5,577 to
5,240 lines including removed section framing and SDK imports. All six callers
include the new public interface directly. Old utility declarations, two local
extern declarations and the SensorStubs NTP stub were removed. Disabled-WiFi
APIs live in the new header; a foreground attempt returns false/`Failed`.
The separate server-validation command in Settings remains where it was.

The new `ntp_transport` host test runs the production implementation with
deterministic platform substitutes. The original and extracted implementations
both pass 14 scenarios plus RTC-disabled paths under AddressSanitizer and
UndefinedBehaviorSanitizer. Cases preserve reply-versus-prior-time outcomes,
7/15-second wait budgets, RTC fallback, deadline and drain interleavings, late
notices, callback restrictions, stable server storage and timezone reapplication.
The registered CTest and nine existing command, setup and QuickNote regressions
pass. The WiFi-disabled header and source compile and execute without platform
headers in the host test.

| Profile | Image (bytes) | Change from immediate baseline | Remaining app space (bytes) |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,883,088 | +144 | 101,168 |
| `xiao-display` | 5,730,224 | -176 | 254,032 |

Both firmware builds pass and retain their saved/resolved choices, SDK settings
and partition hashes. Additional target-compiler checks cover the NTP source
with WiFi disabled and with RTC enabled (camera disabled to avoid its existing
I2C controller conflict). These are compile checks, not full firmware builds
for those temporary selections. No device was flashed or live NTP traffic tested.

An additional standalone `System_Utils.h` probe with networking disabled fails
because ESP-NOW's disabled MAC helpers lack WiFi SDK declarations. The same
failure was reproduced using the pre-extraction utility header; the narrow
NTP header passes that target-compiler check. Record this existing aggregate
header dependency for the networking cleanup rather than expanding this slice.

**Pre-refactor scan — 2026-10-08, next saved-WiFi slice.**

The next step was checked against the current implementation and its callers.
This pass changes the plan only; it introduces no firmware changes.

| Concern | Existing implementation | Remaining coupling |
| --- | --- | --- |
| Saved-network operations | `System_WiFi.cpp` already implements find, upsert, remove, sorting, priority normalization and persistence requests | The public header exports the raw array/count but omits the shared mutation helpers; three setup consumers redeclare them locally |
| Lifetime | `HardwareOne.cpp` allocates internal RAM and constructs eight entries before loading settings | Startup understands Wi-Fi's storage layout and credential allocation requirements |
| Persistence | `System_Settings.cpp` selects the encryption key epoch, loads entries, preserves recoverable encrypted blobs and guards failed-load writes | Settings directly reads/writes the array; extracting serialization requires preserving these contracts |
| Presentation | OLED and glasses submit mutations through their authorized command paths; CLI renders the saved count | They read mutable storage directly; list positions can become stale between rendering and command execution |
| Setup application | Serial/legacy wizard, CLI-mode wizard and first-time backup setup call the same Wi-Fi helpers | Each repeats upsert/sort/save and ignores the returned failures |

Existing shared work also includes `System_SettingsEditorCore`, the Dictation
application interface, HALs, Clock, command metadata and the host configuration
model. Extend those owners when relevant. Remaining utility file length and
directory arrangement alone are not reasons to add another abstraction.

The next implementation should begin with the saved-list behavior baseline and
the existing Wi-Fi operations. Add tests for capacity, validation, keeping an
existing password on blank input, update/removal, ordering and persistence
failure. Existing settings tests cover batching and writer contracts, but the
scan found no production saved-network behavior suite.

The first bounded implementation can expose the existing helpers through the
Wi-Fi header and share the setup save sequence across its three callers.
Handle rejected input and failed persistence before reporting success or
enabling auto-start. Preserve the distinction between a persistence request
accepted into a batch and a completed write. This corrects current failure
reporting as an explicit behavior change, without moving encryption or storage
lifetime in the same slice.

Next complete the consumer boundary in the existing Wi-Fi owner: move storage
initialization behind an operation, expose a bounded copied view without
credentials for screens and a count query for help, and migrate consumers.
Specify list selection identity together with that view: OLED and glasses
currently queue `openwifi --index`, and an in-range index can identify a different
network after reorder/removal. Keep the existing authorization path for actions.

Consistent views require a policy for every writer. The radio mutation guard
protects scans and driver changes; it does not protect the saved array. Settings
currently loads it only during boot, before command-task creation, while Wi-Fi
helpers, promotion and successful connection bookkeeping mutate it later.
The CLI-mode wizard already applies through the command executor. HTTP settings
serialization and display readers can read outside that executor, so serializing
commands alone is not a consistent-read policy.
`connectWiFiIndex` retains an array reference across its waits and records success
through the original index. Capture the connection target deliberately and keep
list locks out of radio waits and persistence. These are code-level risks to
cover during the change, not claims of a reproduced device race.

The persistence boundary follows the existing per-module JSON integration
pattern used by BLE peers and ESP-NOW. Keep key selection, encrypted-blob
preservation, failed-load protection, empty-list persistence and settings batch
semantics with their current owners. Once all access passes through deliberate
interfaces, the raw array/count can become private. Restore currently bypasses
interactive upsert validation; retain that distinction so existing saved data
and blank-password semantics do not change during encapsulation.

Success is observable: startup and screens stop depending on the array layout,
the saved list has one mutation policy, and setup handles rejected input and
failed persistence consistently. Record enabled/disabled compilation and
default/display image headroom; use hardware for connection/reboot persistence
when the lifecycle changes. The separate build-rule consolidation remains a
later workstream, preserving the editable build header and guided tool.

**Implementation record — 2026-10-08, saved-WiFi ownership and setup saving.**

The scanned changes above are implemented. `System_WiFiNetworks.cpp` owns the
saved array, count, allocation, validation, ordering, updates and JSON adapters.
Startup calls `initWiFiNetworks()` at the same point before settings restore,
retaining internal-RAM allocation, placement construction and fatal allocation
failure handling. The mutable array/count and credential-bearing record type
are private. No new settings file or persistent ID field was introduced.

The public interface gives OLED, glasses, CLI and Wi-Fi diagnostics copied views
without passwords. A task mutex covers all list reads and writes; radio waits,
crypto, output, events and persistence run after releasing it. Connection code
receives one owned credential copy through the internal radio adapter and marks
success by entry ID. It no longer retains a reference into a mutable array or
updates whichever entry later occupies an old index.

Screens retain the IDs that produced their displayed rows. IDs survive sorting
and are not reused when an entry is removed/re-created during the boot. Connect
uses `openwifi --saved-id`; removal uses `wifirmid`, through the existing
authenticated executors. The separate removal command preserves the existing
raw/quoted `wifirm <ssid>` syntax, including flag-like names. Existing index
commands remain supported. Deleted selections fail instead of targeting a
replacement. OLED confirmation/session guards and glasses page/command
generation checks remain in use.

All three setup paths call `saveSetupWiFiNetwork` and consume its explicit
result. Rejected input leaves state unchanged. Failed persistence leaves the
list edit in RAM, restores the previous auto-start value and reports failure.
An accepted request includes auto-start in the same settings write; its message
does not claim that a batched write has already finished. Backup setup returns
to its existing menu after a failed save instead of trying a different saved
network. This failure reporting is the deliberate behavior correction from
the scan; shared MQTT setup and unrelated wizard settings retain their paths.

Settings still owns key-epoch selection, failed-load protection, write batching,
file replacement and the existing `putSecretPreserving` policy. Its JSON hooks
retain legacy restore acceptance, encrypted-blob recovery by SSID and empty-list
persistence. Serialization works from a complete locked copy, omits plaintext
copies for password-excluded views, and reports allocation/JSON overflow failure.
That failure propagates through the settings writer, HTTP, glasses settings and
ESP-NOW transfer paths instead of publishing or saving a partial document.

Verification:

- Six production-helper characterization cases passed against the original
  snapshot and the new owner: input validation, password retention, capacity,
  tie ordering, persistence failure and final-entry removal.
- Nine additional owner/JSON/command cases cover stable IDs, setup results,
  concurrent consistent copies, allocation failures, overflow, real removal
  handlers and preserved crypto behavior. Tests use real ArduinoJson and the
  production Settings secret helpers with controlled crypto boundaries.
- Production UI functions run with platform substitutes to cover stale list
  positions, deletion/re-creation, confirmations, session reset, guest guards and
  failed glasses page admission. Source contracts check the authorized command
  wiring, boot/load ordering and failure propagation.
- All 14 targeted host suites passed with ASan/UBSan enabled, including existing
  command, help, settings and MQTT setup regressions. Disabled-WiFi owner/API
  checks passed both on the host and with the actual target compiler. These are
  module checks, not a full disabled-network firmware qualification. The
  aggregate-header issue was still separate at this point and is addressed by
  the network/web follow-up below.

| Profile | Image (bytes) | Change from immediate baseline | Remaining app space (bytes) |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,889,040 | +5,952 | 95,216 |
| `xiao-display` | 5,737,440 | +7,216 | 246,816 |

Both firmware builds passed with matching saved/resolved choices, SDK settings
and partition hashes. Target object inspection shows stack frames of 320 bytes
for the public snapshot, 560 for restore and 624 for JSON serialization; these
figures exclude callees and do not establish runtime stack headroom.

No device was flashed in this slice. Connection, menu interaction and reboot
persistence remain part of the user's deferred hardware testing. In particular,
check add/update/remove (including the final entry), failed save reporting and
selection after another interface changes the list. The structural result is
one saved-list owner, three shared setup callers, and consumers that no longer
depend on credential storage or array layout.


**Implementation record — 2026-10-08, network/web build rules and disabled ESP-NOW.**

CMake no longer parses network/web feature levels or HTTPS, or derives its own
HTTP-server decision. Core HTTP/web and migration sources and their HTTP(S) SDK
requirements are always available; the existing C++ guards use
`System_BuildConfig.h` as their rule owner. This follows the existing
unconditional-source pattern and handles IDF's early requirement discovery
without a new resolver or generated policy table. Other application source
filters remain for later bounded work. The editable flags and configuration
precedence are preserved; the existing web asset consistency check also runs
for builds without the web UI.

The disabled ESP-NOW interface now declares the same MAC getters as the enabled
interface. Their disabled implementations live in the existing `.cpp` file:
Wi-Fi-only uses the SDK driver, networking-off clears the output and returns
`ESP_ERR_NOT_SUPPORTED`, and null outputs return `ESP_ERR_INVALID_ARG`. Enabled
getters and their Hosted-radio cache behavior are unchanged. No new module or
adapter was introduced.

A full restore-only build also exposed two existing web-only assumptions.
`appendCommandToFeed` now guards its web-mirror work with `ENABLE_HTTP_SERVER`;
the glasses HTTP menu uses the existing `isHttpServerRunning()` operation.
These changes let the caller compile without importing disabled web state.
QT Py verification also exposed missing direct input/disabled-GPS declarations
in `OLED_Mode_Map.cpp`; that caller now includes the existing `HAL_Input` and
`System_SensorStubs` headers, preserving its existing behavior.

Verification includes production-header/CMake checks across 14 inputs and both
IDF phases, disabled ESP-NOW implementation tests with ASan/UBSan, and target
compilation of 21 affected files for HTTP-off, restore-only and networking-off
configurations. Existing setup-tool, SDK-policy, MQTT, command-header, NTP,
saved-WiFi and disabled-G2 checks also pass (13 distinct targeted host suites).


| Build | Image (bytes) | Free app space (bytes) |
| --- | ---: | ---: |
| `xiao-default` | 5,889,040 | 95,216 |
| XIAO restore-only (Wi-Fi on, web UI off) | 3,927,408 | 2,056,848 |
| XIAO networking-off | 3,271,104 | 2,713,152 |
| `qtpy_esp32` | 3,876,480 | 2,107,776 |

All four builds passed. The default XIAO image size, saved choices, SDK settings,
resolved features and board hardware match the immediate baseline. The two
isolated XIAO configurations were built from clean directories and their
compiler flags were rechecked against the final header. Linked-symbol checks
show restore-only retains its restore server without the regular web server or
backup-export handler; networking-off has neither HTTP nor HTTPS startup code.
This verifies that listing SDK dependencies does not itself enable those services.

QT Py used the current pinned SDK after its older build cache was repaired.
Its older artifact used different SDK/package versions, so its size change is
not attributed to this refactor. The build-generated ESP32 dependency-lock
change was preserved with the verification artifacts and restored to its
starting contents. The normal wrapper and incremental rebuild path were also
exercised. No device was flashed; connection, restore and reboot behavior still
require the user's deferred hardware testing.


**Implementation record — 2026-10-08, GPS reading ownership.**

`i2csensor_pa1010d` retains its existing public snapshot and sensor-envelope
functions. `GPSCache` now contains only reading values, with explicit units and
validity documentation. The live cache, its retained mutex and the Adafruit
driver pointer have internal linkage. Unused disabled cache/driver globals
and the OLED driver declaration were removed; shared lifecycle integration
remains unchanged.

MQTT, the web sensor endpoint, both glasses readouts, sensor logging and the GPS
OLED renderer now request snapshots. Maps, automation, CLI/accessors and the
ESP-NOW serializer already followed that interface. Existing 5/10/50 ms reader
budgets, output keys, numeric precision and units are preserved. MQTT now waits
up to 10 ms for a consistent copy and omits GPS on a failed copy; it previously
read unsynchronized live fields. Its existing raw-knots speed versus km/h
discovery label is documented for a separate correction.

The private cache lock covers only reading copies and validity updates.
Publication releases it before tracking/events/display callbacks. Shutdown
clears all validity gates together, retrying in the existing task loop on
contention before driver destruction. Startup resets the full reading,
including old date/time and timestamp, and aborts if that reset cannot obtain
the lock. Running/connected flags are still sampled separately; this is not a
new task lifecycle or a guarantee of immediate stop completion.

Verification:

- `gps_snapshot` passes through registered CTest with ASan/UBSan. It executes
  production snapshot, JSON, invalidation, startup, publication and shutdown
  code, the real cache guard, and extracted web/MQTT/G2/logging consumers.
- Direct comparison against the captured original functions matches existing
  wire/display output. A saved fixture keeps those assertions independent
  of temporary baseline files. Failure clearing, wait budgets, buffer limits,
  reset failure, shutdown retry and callback execution after unlocking pass.
- The disabled public header compiles and reports unavailable with cleared
  outputs. OLED has a source-boundary check plus actual firmware compilation;
  its pre-existing glyph escape prevents unchanged desktop Clang rendering
  tests. Real NMEA reception and RTOS scheduling are not simulated.
- Both full firmware builds below pass. Linked symbols confirm the owner state
  is local and callers reference the snapshot function. SDK settings and
  resolved features match each build's baseline; dependency locks are unchanged.

| Build | Image (bytes) | Change from controlled baseline | Free app space (bytes) |
| --- | ---: | ---: | ---: |
| XIAO GPS-enabled verification profile | 5,303,440 | -272 | 680,816 |
| GPS-disabled `xiao-default` | 5,889,008 | -32 | 95,248 |

The temporary GPS profile enables GPS, MQTT, HTTP sensor pages, OLED, G2, Maps,
sensor logging and automation together. Camera, ESP-NOW and LLM features are
omitted for capacity. The initial broader baseline was already 156,000 bytes
over its partition before this refactor. GPS+ESP-NOW owner/serializer consumers
also pass separate checks with the actual target compiler and both flags on.
No user configurations, shared build flags or partition sizes were changed.

That reduced baseline exposed one existing include error in the touched MQTT
file: its device-statistics header was inside the ESP-NOW condition even though
MQTT always uses those functions. The include now sits with MQTT's ordinary
dependencies. The identical prerequisite fix was applied to the captured
baseline before comparing GPS code sizes.

No device was flashed. Physical GPS acquisition, presentation and start/stop
behavior remain part of the user's deferred device testing.


**Implementation record — 2026-10-08, STHS34PF80 reading ownership.**

The presence module follows the existing GPS pattern: `PresenceCache` is a
value-only reading and `presenceCacheSnapshot` owns the short synchronized copy.
The live cache and retained mutex are private. MQTT, web, G2, OLED, logging and
the module's CLI/JSON builders use snapshots; the unused `getPresenceDataJson`
web helper and disabled dummy cache were removed. Existing web card/JavaScript
content is unchanged. Disabled reading functions live with the public interface
and report unavailable with cleared outputs.

Existing reader waits remain 5 ms for G2, 10 ms for OLED/logging, 50 ms for web
and sensor JSON, and 100 ms for the CLI read command. MQTT and CLI status now
use bounded 10 ms copies instead of unlocked reads. MQTT omits presence on
failure; status reports `dataValid=0` while still reporting lifecycle state.
Each consumer retains its previous validity policy, including OLED/logging's
lack of a `dataValid` gate. Detection flags, register scaling, output precision
and event behavior were preserved; a contradictory bit-mapping comment was
corrected to match the existing code.

Startup now clears the full reading, including the old timestamp, and aborts
before hardware initialization if the 100 ms reset lock cannot be acquired.
Shutdown clears only `dataValid`, as before, but now under the private mutex;
a contended 50 ms attempt yields 10 ms and retries before task exit. Detection
values remain stored behind that validity flag. These bounded individual waits
do not guarantee a total shutdown deadline. Lifecycle flags and rapid restart
coordination remain in the existing shared task infrastructure.

Verification:

- Sanitized CTest `presence_snapshot` and the shared-consumer GPS regression
  both pass (2/2). Presence tests execute the production polling function,
  snapshot, JSON, CLI, startup reset and shutdown blocks, real cache guard,
  and MQTT/web/G2/OLED/logging consumers with platform substitutes.
- Baseline comparison preserves output and event sequences. A saved fixture
  keeps future runs independent of the temporary baseline. Tests cover all
  detection-bit combinations, signed/raw scaling, each register-read failure,
  no new data, I2C admission failure, ten-clear-poll debounce and motion edges.
- Failure clearing, all reader wait budgets, JSON buffer limits, disabled
  headers, reset failure and shutdown retry pass. Formatting/drawing/events run
  outside the cache lock, and cache waits follow release of the shared I2C bus.
- Both firmware builds below pass with matching SDK/resolved choices. Linked
  symbols show private presence storage and real consumer references to the
  snapshot API; disabled firmware contains no presence cache. Dependency locks
  and user configurations remain unchanged.

| Build | Image (bytes) | Change from immediate baseline | Free app space (bytes) |
| --- | ---: | ---: | ---: |
| XIAO GPS + presence verification profile | 5,322,384 | -176 | 661,872 |
| GPS/presence-disabled `xiao-default` | 5,889,008 | 0 | 95,248 |

The private enabled profile extends the GPS verification configuration with
`CUSTOM_ENABLE_PRESENCE=1`; MQTT, HTTP/web sensors, OLED, G2, Maps and automation
remain enabled. ESP-NOW integration additionally passes actual target-compiler
checks with GPS, presence and ESP-NOW enabled together. No new framework, board
restriction, partition change or setup-tool rule was introduced.

No device was flashed. Host substitutes and firmware builds do not qualify
physical detection accuracy, display behavior, bus timing or RTOS scheduling;
those remain part of the user's deferred hardware testing.


**Implementation record — 2026-10-08, remaining local sensor readings.**

The initial scan found three different needs: ordinary readings, thermal frame
buffers and physical input events. The existing GPS/presence snapshot pattern
now covers IMU, ToF, APDS, RTC and FM. Each module owns its live reading, driver
and synchronization; its public cache type contains values only. Callers copy
readings or use the owner's existing JSON builder. Display, logging, automation,
Bluetooth, web and ESP-NOW consumers no longer access these private caches.
Consumer validity policies and ordinary payload units/precision are preserved.

Thermal provides separate metadata and frame-copy operations. Consumers supply
their own storage, so rendering and serialization do not borrow a buffer that
the owner can retire. Raw frames remain signed centidegrees; the existing
interpolated JSON format remains whole degrees. JSON uses a temporary 6 KiB
buffer, while raw OLED/ESP-NOW copies use 1.5 KiB. These allocations prefer PSRAM
and are released after each operation; unavailable storage produces an
unavailable result. Writer-side interpolation remains within its existing
private synchronization and was not redesigned.

`HAL_Input` owns the shared input reading and button latches. Screens peek;
dispatch atomically copies and consumes presses. Backend publication uses the
small internal `HAL_InputInternal.h` interface. ANO keeps its driver-specific
detent operation. Failed activity reads preserve the previous sequence, so
cache contention cannot create false activity. GPIO failed-start cleanup now
releases hardware even if a second cache reset is contended; a normal contended
stop retains the running state for retry.

FM uses a private short critical section for its fixed-size reading, following
the existing battery pattern. Hardware operations and their cache commits use
the existing I2C transaction order; critical sections contain bounded data work
only. This avoids a second seek state machine and prevents successful commands
from losing or reversing cache updates. Enabled snapshots always acquire that
short critical section; the timeout argument remains for interface
compatibility. The existing failed-bus success responses for tune/volume/mute
remain a separate command-error issue.

Battery, fuel-gauge, camera, microphone and actuator interfaces already expose
owner operations or copied/caller-owned data. Those patterns were retained.
Unused battery-state and servo-driver/profile exports were made private; no
generic sensor class or registry was added. Static memory diagnostics now ask
owners for their storage size instead of inspecting exported cache objects.

Intentional failure-path changes include rejecting truncated ToF/thermal JSON,
reporting unavailable thermal web data instead of the broken empty-body fallback,
removing FM unlocked fallback reads, and reporting unavailable ANO CLI readings.
Startup reset and shutdown invalidation use private synchronization. Individual
waits are bounded; lifecycle flags, total shutdown deadlines, rapid restarts
and existing IMU action-update coordination remain outside this milestone.

Verification:

- All 19 selected registered host tests pass in the sanitizer-enabled build,
  including GPS/presence regressions, four new sensor suites, input backends,
  configuration/header checks and parent/ToF lifecycle coverage. The battery
  production harness also passes with ASan/UBSan.
- Pre-change production comparisons preserve small-sensor JSON/OLED output,
  thermal raw/interpolated frames in all four rotations and integer ESP-NOW
  output, FM RDS/JSON/OLED output, and shared G2/Bluetooth/logging/automation
  behavior. Checked-in fixtures retain these comparisons for future runs.
- Failure tests cover unavailable copies, capacities, retired frame storage,
  startup/stop admission, button latches, failed activity reads, GPIO cleanup,
  seek timing and bus-ordered FM commits. Platform substitutes do not simulate
  real RTOS scheduling or electrical behavior.
- Twelve actual target-compiler probes pass for GPIO-input consumers and the
  enabled servo owner. GPIO also runs through the host integration suite with
  both three- and five-button wiring; it has not received a full firmware link
  or physical test in this pass.

All three firmware builds pass:

| Build | Image (bytes) | Change from immediate baseline | Free app space (bytes) |
| --- | ---: | ---: | ---: |
| XIAO all-sensor / Seesaw verification profile | 5,625,632 | -3,680 | 358,624 |
| XIAO all-sensor / ANO + ESP-NOW verification profile | 4,626,368 | No same-profile baseline | 1,357,888 |
| Sensors-disabled `xiao-default` | 5,888,592 | -416 | 95,664 |

The primary private profile enables thermal, IMU, ToF, APDS, RTC, FM, GPS,
presence and Seesaw alongside MQTT, web sensor pages, OLED, G2, Maps and
automation. The secondary profile selects ANO, ESP-NOW and servo, with
Bluetooth/G2/Maps disabled. These temporary verification selections do not
replace any user profile. Primary/disabled SDK settings match their respective
baselines. Linked enabled symbols confirm private cache storage and public
snapshot operations.

The comparison uses a pre-change baseline captured at the start of this pass,
after the GPS/presence work. User-editable build flags, saved configurations,
dependency locks and partition geometry are unchanged. No device was flashed;
physical sensor operation, display behavior and timing remain for deferred
hardware testing.

**Hardware follow-up — 2026-10-08, QT Py ESP32 Pico.** The saved
`qtpy-sensors` profile has now been flashed to a replacement board and tested
with ToF, Seesaw gamepad, presence and GPS. Live testing confirmed sensor
communication and exposed a ToF stack overflow plus a failed task restart
after shutdown. Corrected firmware passed three completed stop/start cycles
and a simultaneous four-sensor soak. Results and qualification limits are in the
[QT Py sensor bench record](QTPY_SENSOR_TEST.md), which distinguishes observed
physical results from remaining qualification limits.

**Implementation follow-up — 2026-10-08, shared I2C discovery.** The remaining
boot receive error led to a focused review of the scan paths. A diagnostic run
reproduced a transient ACK at an unattached address followed by the old
scanner's raw byte-read error; all four attached sensors accepted that read in
the same run. The original boot-error address is unknown, so this is evidence
of the scan failure mechanism rather than a diagnosis of a particular sensor.

`I2CDeviceManager` now owns the address-only probe, including its bus mutex,
clock selection/restoration and confirmation of a positive reply with a second
ACK. CLI, OLED, boot discovery and hardware detection reuse it. Probing does
not read device payloads or create sensor health/transaction records. The
duplicate smart-scan bus initialization and clock changes, plus an unused full
scanner, were removed. Primary-address labels take precedence over alternate
matches; drivers still establish device identity. This reduces four scan
implementations to one bus operation without adding a registry or framework.

Three focused I2C host checks pass, with the compiled harnesses using
ASan/UBSan. The QT Py profile, XIAO all-sensor profile and sensors-disabled XIAO
baseline also build. The flashed QT Py passed a 237.87-second run with 20 scans
(ten stopped and ten polling), discovery/detection in both states, three
completed stop/start cycles and a 120-second final simultaneous soak. All four
sensor readings stayed fresh, memory remained stable, and the completed run
had no raw I2C errors, GPIO warnings, panics or resets. Both joystick axes and
all four face buttons responded in captured readings; full axis travel and
Select/Start remain unqualified. GPS returned fresh readings without an indoor
fix. The detailed measures, earlier abbreviated-run warnings and limits are in the
[bench record](QTPY_SENSOR_TEST.md#discovery-follow-up--2026-10-08).
Rapid reopening during cleanup and misleading start-preflight failures remain
separate, focused lifecycle follow-ups. Electrical fault recovery and the
original ToF overflow path were not reproduced by these normal-operation tests.

**Implementation follow-up — 2026-10-08, display hardware lifecycle.** The
peripheral audit found duplicated OLED initialization in the existing HAL and
boot UI, plus brightness and sleep/wake hardware commands in UI code. Boot,
manual startup, stop and wake now use `HAL_Display` as their hardware owner.
The UI retains startup policy, animation, navigation, saved presentation
settings and display-session authentication. This is a consolidation of the
existing interface, without a new device framework.

Explicit readiness replaces pointer existence as the initialization test.
New failed SSD1306 allocations are not published; successful canvases stay
valid across stop/restart because legacy renderers borrow their pointers.
This retains a 1 KiB SSD1306 framebuffer plus its object after stop, matching
the P4 backend's existing lifetime policy. The selected bus remains latched
with its driver; manager transactions, including legacy setup-screen flushes,
use that bus. The library no longer reinitializes Wire or changes its clock
behind the manager's cached state.

Brightness and power commands share protected HAL operations. The existing
power-gated board's rail delays, recovery and charge-pump sequence remain in
place, including manual retry after failed wake. Repeated prepare/resume calls
preserve state, and closing while asleep prevents automatic reopening.
The new `qtpy-oled` saved profile enables SSD1306 alongside ToF, presence and
GPS with physical input disabled; shared defaults and board wiring remain
unchanged.

Four focused host checks pass with ASan/UBSan, including the production HAL
with controlled platform boundaries and the production P4 backend lifecycle
harness. The `qtpy-oled`, `xiao-display` and `xiao-default` firmware builds pass.
The attached QT Py completed a 194.19-second automated run: early boot,
13 consistent four-device scans, three OLED stop/start cycles, brightness
commands, light sleep with the OLED both active and stopped, and a 90-second
simultaneous sensor soak. Three sensor readings remained fresh, all six
diagnostic samples had identical free RAM/PSRAM, and health/allocation errors
stayed zero. The sole restart was deliberate; no crash was recorded.

The user confirmed that the OLED works after the run; individual contrast
levels and every off/on transition were not separately confirmed. The QT Py's always-on
bus does not physically qualify power-gated rail restoration, and P4 hardware
was not exercised. Details and image sizes are in the
[OLED verification record](QTPY_OLED_TEST.md). Driver write methods still
cannot confirm every command ACK, and retaining borrowed storage does not
synchronize all legacy framebuffer access across tasks.

**Implementation follow-up — 2026-10-08, PCA9685 and FM command outcomes.**
Both owners now follow the existing command path in the same order: validate
input, perform the protected operation, then report its outcome. Invalid
numbers, ranges and extra command arguments are rejected before initialization
or writes. Servo profile names retain their existing free-form grammar.
This reuses `CommandArgs`, standard numeric parsing and the I2C transaction
helpers; no device framework or second command layer was introduced.

PCA9685 commands check bus admission and the driver's PWM write result. An
optional frequency request and PWM write share one bus lock; a failed PWM
write reports that the preceding frequency request may already have changed
the chip. Servo movement and calibration describe a request as issued because
the library's microsecond setter returns no write result. Calibration
instructions appear only after that request runs, and initialization preserves
profiles entered before the first hardware command or a retry.

FM tune, volume and mute update their cache and publish command feedback only
after bus admission. Unmute shares mute's implementation. Offline volume
storage, command names and the existing JSON/OLED data formats are preserved.
Frequency parsing checks finite values and range before narrowing, then rounds
to the library's 10 kHz input units. The library still chooses the actual band
and tuning grid; command acceptance does not prove reception or a device ACK.

The focused `servo_command_outcomes`, `fm_radio_snapshot` and
`sensor_parent_lifecycle` CTest suites pass with ASan/UBSan. They execute
production parsing and owner operations with controlled bus/driver failures,
including retry, blocked-command state preservation and driver write failure.

Both firmware builds pass, with compiler-resolved feature flags checked and
the enabled command handlers present in the linked image:

| Verification build | Image (bytes) | Free app space (bytes) |
| --- | ---: | ---: |
| XIAO all-sensor profile with FM and servo enabled | 5,639,152 | 345,104 |
| `xiao-default`, FM and servo disabled | 5,888,608 | 95,648 |

The enabled selection is a private verification profile; user configurations,
shared defaults and dependency locks are unchanged. The disabled firmware image
is byte-for-byte identical to the immediate pre-change baseline.

Physical PCA9685 output and FM reception remain untested; the connected QT Py
OLED firmware was not replaced for this change. Bus-setting changes still
require the existing reboot flow.

**Implementation follow-up — 2026-10-09, sensor lifecycle and ESP-NOW setup.**
The existing I2C queue now coordinates both start and stop requests. A stop
atomically cancels pending starts for that device and has independent capacity
when the start ring is full. The worker applies stops after any active
initialization returns; it skips cancelled requests and suppresses their start
announcements. Start results come from the owner's return value. Stop events
wait until the previous task publishes cleanup completion by clearing its
handle. Queue position and depth continue to describe pending starts only.

All sensor start commands, RTC and first-time I2C input setup use the shared
admission path. It distinguishes an address NACK from an unavailable/busy bus
or transfer error. Owner startup refuses unfinished cleanup before touching
resources, and failed task creation rolls back state. Existing restart
cooldowns remain. ANO clears both input flags, APDS has an explicit lifecycle
latch separate from its measurement modes, and gamepad reconnection no longer
turns on its lifecycle. GPIO input keeps its independent backend.

FM initialization uses its existing task handshake; its task owns teardown.
The worker also handles FM/RTC initialization that never created a polling
task. ToF shutdown writes now use the manager transaction, including during
reinitialization. Driver destruction remains outside the bus lock and still
releases resources if a best-effort stop write cannot run. Positive ESP-NOW
lifecycle announcements moved into the worker; nine sensor owners no longer
include ESP-NOW transport headers.

ESP-NOW identity setup now shares `wizardApplyESPNowSettings` across serial,
CLI mode and OLED, following the existing MQTT setup operation. Accepted page
completion applies the collected fields. Early Next at Name retains the
current device name; OLED Back/cancel before page completion no longer applies
partial names. Empty optional fields, stationary defaults, skip behavior and
final persistence retain their prior contracts. Cancelling the whole wizard
still does not roll back settings accepted on earlier pages.

The focused host suites execute the production manager queue, full worker,
all nine sensor starters with both I2C input backends, and the three setup page
handlers. Failure injection covers queue cancellation, initialization and task
creation, cleanup still in progress, delayed stop events, and taskless radio
and RTC shutdown. These controlled interleavings do not prove every RTOS
schedule or general concurrent peripheral-command safety. In particular, the
APDS mode command's existing driver-access versus teardown synchronization is
outside this lifecycle change.

Nineteen focused host suites pass, with ASan/UBSan enabled for executable
harnesses. Firmware builds pass for `qtpy-oled`, both XIAO I2C input variants
with all queued sensor owners and ESP-NOW enabled, and the disabled-sensor
`xiao-default` baseline. The QT Py image decreased by 576 bytes; the default
XIAO image increased by 32 bytes relative to the immediate baseline.

The QT Py completed a 153.39-second physical run covering rapid requests,
cancellation, three completed restarts and a final 60-second soak. GPS, ToF
and presence recovered fresh readings, OLED stayed available, and no new
crashes, I2C health errors or allocation failures occurred. Three immediate
ToF restarts were refused within the existing cooldown, then succeeded later.
The [sensor lifecycle verification record](QTPY_SENSOR_LIFECYCLE_TEST.md)
contains the build sizes, image fingerprints, memory measurements and limits.

**Implementation follow-up — 2026-10-09, cached read commands.** The audit
found that IMU/GPS text commands already use owner snapshots, while APDS's
three dedicated readers and ToF's text reader sampled hardware independently
of their polling tasks. APDS color could wait indefinitely, gesture reads
could consume data outside the event producer, and a denied ToF transaction
could return a default zero as a successful distance.

Those four command-specific hardware readers have been removed. APDS color,
proximity and gesture commands now copy its existing cache, and ToF text selects
the nearest valid detected object from its four cached slots. ToF text and
JSON use the same filtered/smoothed published distance; the text path emits one
distance line. Commands report stopped, unavailable/busy, or no reading instead
of claiming a completed read. APDS gesture reports the latest sampled value
without consuming it or producing another event.

APDS records per-field availability in its snapshot value, because a
successful polling transaction does not mean color conversion was ready. The
task publishes those bits together with the values, and startup/shutdown clear
them. A color-only cycle with no ready data now reports invalid rather than
valid zero RGBC. JSON keys and existing output for supplied valid readings are
unchanged. Vendor scalar/void read methods still cannot confirm every physical
I2C acknowledgement; these bits identify which sampling paths ran.

Eight focused host suites pass with ASan/UBSan, including two new command
suites. APDS readers compile in a fixture without hardware/event APIs; its
actual task sampling block covers 32 bus/mode/readiness combinations plus
admission failures and event ownership. Existing snapshot, consumer, parent
cleanup, startup and command-registry checks pass. The runtime code is 86 lines
smaller, including the new availability handling and updated help.

This completes the read-command boundary for these two owners. APDS mode
writes versus teardown, its proximity/gesture mode interaction, and the
vendor gesture reader's potentially extended execution remain distinct
follow-ups; this change does not claim to fix those control paths.

All three firmware builds pass. `qtpy-oled` is 2,876,544 bytes and the enabled
XIAO sensor/ANO profile is 4,617,808 bytes, each 624 bytes smaller than its
immediate baseline. `xiao-default` remains 5,888,640 bytes. The 67.81-second QT
Py check exercised unavailable and successful cached reads, stop/restart, and
coexistence with GPS, presence and OLED; tracked I2C errors and allocation
failures stayed zero, with unchanged free memory and no new crash. APDS remains
host/build-qualified for this change. Details are in the
[cached-read follow-up record](QTPY_SENSOR_LIFECYCLE_TEST.md#cached-read-follow-up--2026-10-09).

**Hardware follow-up — 2026-10-09, APDS attached.** The saved `qtpy-oled`
selection now includes APDS; the OLED was physically removed to make room.
The image builds with 3,090,896 bytes free. APDS startup, cached color/proximity
reads and two stop/restart cycles work alongside GPS, ToF and presence. The
missing optional OLED reports unavailable without preventing sensor startup.

Gesture sampling stalled: cached commands remained responsive, but the APDS
producer stopped publishing until gesture mode was disabled. The existing
vendor read loop's extendable timeout is the likely cause. The focused next
correction belongs in that existing driver: bound a gesture read and verify
sustained directional input cannot hold the shared bus indefinitely. Do not
replace the cache or add another sensor framework. Full results and limits are in the
[APDS hardware record](QTPY_APDS_TEST.md).

**Implementation follow-up — 2026-10-09, APDS gesture polling.** The vendor
reader now consumes one available FIFO batch per call, without waiting under
the shared I2C lock. Existing direction markers persist between polls with a
300 ms inactivity expiry. Complete four-byte FIFO datasets are decoded only
after a successful transfer; invalid data clears partial recognition state.
This stays in the existing driver, with the cache and event producer unchanged.

A 300 ms limit alone stopped the original freeze but still delayed GPS's NMEA
processing. Short polling removes that bus occupancy instead of adding a new
scheduler or changing GPS. The dedicated production-method regression checks
no waits, one batch per call, cross-call gestures, inactivity, transport
failures, full batches and clock rollover. Four focused sanitized suites and
the enabled QT Py/disabled XIAO builds pass. The QT Py image grew by 48 bytes.
Hardware results and remaining physical-gesture limits are recorded in the
[gesture follow-up](QTPY_APDS_TEST.md#gesture-fix-follow-up--2026-10-09).


## Shared setup completion — 2026-10-09

**Purpose:** make saving and activating a completed setup changeable in one
place. The audit found the same Wi-Fi save, main/debug settings writes, runtime
application and NTP refresh in `runAndApplyFeatureWizard` and
`wizardMode_onExit`. MQTT/ESP-NOW page operations and the saved-Wi-Fi owner
already provided the intended pattern.

`wizardSaveAndApplySettings` now owns that sequence in `System_SetupWizard`.
Both engines call it after their existing completion checks. An incomplete
result has no completion side effects. The CLI adapter no longer includes NTP,
Wi-Fi storage, the Wi-Fi SDK or `System_Utils`, and its manual persistence
externs are removed. This slice removes 27 lines across the three runtime files.

Existing finalization placement, output, save-failure warnings and application
order are preserved. Both settings files are attempted even when one fails;
runtime application still follows. Cancellation skips this final save but does
not roll back values already changed by earlier pages. This extraction does not
change that pre-existing behavior or move certificate generation.

Seven focused host suites pass with sanitizer-enabled configuration: the new
production completion/adapters test, saved-Wi-Fi and downstream-persistence
contracts, MQTT and ESP-NOW page application, Wi-Fi storage behavior, and the
settings write-batch contract. The completion test runs with Wi-Fi enabled and
disabled, covering independent save failures, operation order, connectivity
after runtime application, incomplete results and cancellation/timeout cleanup.
The dialog, storage and network boundaries are simulated; this does not qualify
physical UI interaction or durable device writes.

Firmware builds pass for `qtpy-oled` (2,893,328 bytes, 3,090,928 bytes free) and
`xiao-default` (5,888,512 bytes, 95,744 bytes free), respectively 80 and 128 bytes
smaller than their immediate baselines. Saved choices and dependency locks are
unchanged. No device was flashed for this settings-only change. The previous
QT Py image and physical-test records are preserved separately from the new
build.

**Next audited candidate:** CPU frequency changes. `System_Power`, the
`cpufreq` command and idle/wake currently have separate mutation paths; only the
idle/wake helper drains I2C activity, and it ignores mutex/HAL failures. A
bounded follow-up can give `System_Power` one operation using the existing
`PollPauseGuard` and bus mutexes, with explicit failure/retry handling. Existing
menus already dispatch through the command interfaces and need no new UI
framework. This candidate is recorded for the next slice, not implemented here.


## Shared CPU clock operation — 2026-10-09

**Purpose:** give every CPU-frequency change the same protection and outcome
checks. Power mode application and `cpufreq` previously called the HAL directly;
only idle/wake drained I2C, and that helper switched even when a bus lock timed
out. Existing mode tables and UI command routing already had the right owners.

`powerSetCpuFrequencyMhz` in `System_Power` now owns the operation. It uses a private
FreeRTOS mutex to refuse overlapping/recursive transitions, the existing nested
polling guard and ordered bus locks, and verifies the HAL result against the
live clock. A non-allocating manager lookup keeps early settings application
independent of I2C startup. The private idle-only implementation is removed;
mode policy, command parsing and authorization stay at their existing callers.

Idle entry commits display/cooldown/events only after a confirmed clock change.
Failed wake retains its original target and retries without needing new input.
An optional output identifies changes made by this call, avoiding a race where
a refused idle request could otherwise restore over another accepted command.
No new scheduler or power-management framework was introduced.

Both firmware profiles pass: QT Py is 2,893,456 bytes (+128) with 3,090,800 bytes
free; XIAO is 5,888,384 bytes (-128) with 95,872 bytes free. Host verification
covers six owner build combinations, 56 idle/wake scenarios, the command
catalogue and existing power profile/JSON/menu tests. The application-only QT Py
flash and 110.63-second hardware test passed all frequency/preset requests with
four sensors advancing and no new tracked errors; original power settings were
restored. Physical OLED sleep/wake remains unqualified because it is absent.
Details and reproducible host commands are in the
[power verification record](QTPY_POWER_TEST.md) and
[host test guide](../components/hardwareone/test/host/README.md#shared-cpu-clock-changes).


## Shared icon lookup and drawing — 2026-10-09

**Purpose:** make icon changes belong to the existing asset and UI modules.
The audit found generated assets in `System_Icons`, a named adapter in
`OLED_UI`, and lookup/mapping/rendering code still in `System_Utils`, with
copied and sometimes mismatched declarations in consumers.

Implementation is complete. `System_Icons.h` now exposes availability and
extension mapping; its handwritten sibling implements that policy without
editing generated payloads. `OLED_UI` owns rendering through the shared
`Adafruit_GFX` canvas and its existing named adapter. Callers include the
owning headers. The old utility implementation, redundant declarations and
unused helpers are removed; no new icon framework was added.

Four focused suites pass with ASan/UBSan. The icon regression executes
production code against fixtures captured from the previous implementation,
covering four OLED/web feature combinations, SSD1306/ST7789 aliases, asset
bytes, extension mapping, scaling, clipping and invalid inputs. The existing
small-sensor snapshot, FM snapshot and display-lifecycle suites also pass.

Both saved profiles build: the final QT Py is 2,893,520 bytes with 3,090,736 bytes
free (+64); XIAO is 5,888,272 bytes with 95,984 bytes free (-112). Assets and saved
choices are unchanged, dependency locks were restored, and the existing P4
mesh overlay was intentionally preserved. The hash-verified application ran Menu,
File Browser and System Status alongside fresh GPS, ToF and presence readings,
with zero tracked errors during rendering. A stationary follow-up recorded one
ToF failure counter increment during shutdown; it remains a lifecycle/accounting
follow-up, not a clean-run claim. File Browser is left open with the requested
display login. The user confirmed rendering and noted that the icon sat too
high. A local layout correction now centers the icon and its label together
below the header, using an absolute bottom bound for the label fit check.
Fingerprints, raw test outcomes and limits are in the
[icon verification record](QTPY_ICON_TEST.md).


## ToF shared-bus polling ownership — 2026-10-09

**Purpose:** keep normal shutdown, sample readiness and cache contention out
of hardware-failure accounting, while giving other I2C devices time to run.
The preceding display test found one ToF error during restoration. A host
reproduction of the old code demonstrated a cancelled poll being counted as
NACK on both manager buses, even though the address probe succeeded.

Implementation is complete. `tofPoll()` now owns one transaction and checks
readiness once, reusing the presence sensor's existing bus-result/sample-ready
pattern. Readiness, sample-read and rearm errors propagate. Filtering and cache
publication run after releasing the bus; contention cannot become a bus fault.
The task calls that owner directly, removing its outer transaction wrapper.
Filtering, smoothing and published values remain unchanged.

An asynchronous stop may let one in-flight operation finish because the task
retains its driver until cleanup. Post-operation and cache-admission checks
prevent publishing when stop has been observed. Errors returned by actual
hardware operations remain counted, while the stopped task skips auto-disable
reporting. No new manager API or polling framework was introduced.

Both profiles build: QT Py is 2,893,392 bytes with 3,090,864 bytes free (-128);
XIAO is 5,888,272 bytes with 95,984 bytes free (unchanged). Six focused host
suites pass with ASan/UBSan, including unchanged snapshot fixtures. The
143.42-second QT Py run passed six completed ToF stop/restarts and a shared-bus
soak with GPS, presence and OLED active. No new I2C errors, crashes or allocation
failures occurred, and settings were preserved. Sensors are stopped; File
Browser remains logged in. The vendor's existing internal read-status overwrite
remains a documented limit, outside this slice. Fingerprints,
verification status and remaining limits are in the
[ToF polling record](QTPY_TOF_POLL_TEST.md).

## Shared filesystem directory traversal — 2026-10-09

**Purpose:** give web/CLI listings, display browsers and ESP-NOW one definition
of a directory entry, while retaining each interface's visibility and output.

`VFS::DirectoryReader` now owns virtual-first/backend ordering, SD-relative
name translation, direct-child validation and full VFS entry paths. It borrows
an already guarded-open directory under the caller's filesystem lock, closes
child handles before returning owned metadata, and never grants access or sends
network data. The four former walks in `System_Filesystem`, `FileManager`'s
cache and fallback, and `System_ESPNow_FsList` use this reader.

The FileManager cache and fallback also use the same visibility predicate.
Filtering precedes counting and caching, so later picker matches remain
reachable. The fallback includes virtual mounts in its indexes. Both original
failures were reproduced against the saved pre-change production methods:
with a mounted SD and 256 physical root entries, index 256 was missing; at 257,
it returned the wrong file. Picker matches beyond the first 256 physical entries
were omitted at both root and an SD subdirectory.

Caller policies remain explicit: web/CLI may hide admin paths and count all
children; display browsers hide dotfiles, retain the 256-entry cache and optional
99-child count; ESP-NOW hides dotfiles and retains its existing paging, identity
and wire limits. Permission metadata uses complete paths before display/wire
name truncation. Picker predicates use name/type/size before permission/count
metadata is populated. Guarded I/O still performs live authorization.

Verification: four focused host suites pass (`filesystem_psram_listing`,
`filemanager_directory`, `espnow_directory`, and the listing-permission source
contract), with ASan/UBSan for executable harnesses. The browser harness runs
14 scenarios and 14,710 assertions; the remote handler runs 1,208 checks; the
web/CLI suite passes with and without PSRAM. A separate comparison of 72
pre-change and new text/JSON/envelope cases is byte-identical. Independent review
found no blocking issue. Physical SD, display and radio qualification remains
separate from these host checks.

Both saved profiles build with ESP-IDF 5.5.5 and unchanged resolved feature
choices. QT Py is 2,890,896 bytes with 3,093,360 bytes free (2,496 bytes smaller);
XIAO is 5,882,336 bytes with 101,920 bytes free (5,936 bytes smaller). The runtime
and public-header change removes 182 lines overall. Saved profiles and the
dependency lock are unchanged. Neither image was flashed during this slice;
the QT Py retains its preceding firmware. Current build notes and hardware-test
indexes explicitly mark the new images unqualified, with the previous images
and test records preserved under `historical-before-directory-reader-2026-10-09`.

## Checked text and atomic saves — 2026-10-09

**Purpose:** make the shared persistence helpers reject incomplete writes and
prevent competing saves from consuming one another's temporary file.

The existing `writeText()` and `writeTextAtomic()` implementations remain in
`System_Utils` with unchanged signatures and trusted internal VFS access.
Direct writes now check the returned byte count, reported write error and file
size after flushing, and reject invalid paths or directory handles. Both full
paths are constructed with checked allocation before an atomic save performs
I/O. Atomic saves hold one existing filesystem lock and polling guard through
temporary writing, promotion and best-effort temporary-file cleanup. Nested
callers retain their own guards. No failed save falls back to deleting or
truncating the destination; direct `writeText()` retains its explicit
create/truncate behavior and can leave partial output when it fails.

Before changing the helpers, production-extraction tests reproduced two bugs:
a two-byte short write returned success and replaced the live file with `re`,
and a deterministically interleaved second save consumed the first save's temp.
The completed suite runs the actual write helpers, `FsLockGuard` implementation
and `PollPauseGuard`, using filesystem/RTOS boundary doubles. All 30 scenarios
pass with ASan/UBSan, covering short/zero writes, reported errors, size mismatch,
empty content, allocation/open/rename/cleanup failures, stale temporary files,
nested guards and competing same-target saves. Independent review found no
blocking issue. The test is registered as `checked_text_writes` in CTest.

Both saved profiles build using the established ESP-IDF 5.5.5 environment:
QT Py is 2,891,072 bytes with 3,093,184 bytes free (+176 bytes); XIAO is
5,882,592 bytes with 101,664 bytes free (+256 bytes). Compiler-resolved choices,
saved profiles and both dependency locks are unchanged. The current build
status files record hashes and explicitly mark these images unflashed and
physically unqualified; previous builds are preserved under
`historical-before-atomic-writes-2026-10-09`.

This verifies checked completion, not power-loss durability: the Arduino backend
ignores `fflush`/`fsync` return codes and does not populate `getWriteError()` for
those failures. Byte-count and size checks use the completion information it
currently exposes. A cleanup failure can leave a temporary file but still
returns failure with the original destination preserved. Web upload cleanup
and capacity/status consistency remain separate follow-ups.

## Streamed upload lifetime and cleanup — 2026-10-09

**Purpose:** make upload error paths release their resources consistently,
without requiring each return to repeat buffer, file, mutex and polling cleanup.

The existing `handleFileUpload()` now uses `PollPauseGuard`, standard owning
buffers with the same `free` deleter used by other web handlers, and a deferred
`FsLockGuard`. The filesystem lock is still acquired when the validated
destination opens. The receive/write block returns an error to one finalization
point, which closes the file, attempts removal of failed partial output while
still locked, and releases upload buffers and lock before success hooks.
Removal applies only to a successfully opened destination and retains the
existing authenticated-user-first/system-fallback policy. An outer caller's
lock and polling depth remain owned by that caller.

Before applying the change, the production-extraction tests reproduced four
invalid/denied/open-failure returns that leaked a polling pause, and interrupted
uploads that left an empty or partially written file without attempting cleanup.
Requests with no body or no content field also reported success; those now fail
without running post-save hooks. Valid empty uploads remain supported. The
streamed text/base64 parser, request authorization, destination routing and
post-save behavior otherwise retain their existing contracts. No new public
interface or upload framework was introduced; the handler shrank from 493 to
452 lines, with two direct includes added.

Verification: `upload_lifecycle` runs the actual handler, filesystem guard and
polling guard against HTTP/authentication/allocation/file boundary doubles.
All 35 scenarios pass with map hooks both enabled and disabled (70 executions),
using ASan/UBSan and C++23 to match the firmware's language semantics. Coverage
includes both buffer allocation failures, denied/open failures, interrupted
receives, short writes, space exhaustion, failed cleanup, nested guards, empty
and multi-buffer uploads, percent/base64 chunk boundaries and success-only
hooks after upload resources are released. The registered CTest passes, and
independent review found no blockers.

Both saved profiles build with ESP-IDF 5.5.5. QT Py is 2,890,704 bytes with
3,093,552 bytes free (368 bytes smaller); XIAO is 5,882,368 bytes with 101,888
bytes free (224 bytes smaller). Saved profiles, resolved feature choices and
dependency locks are unchanged. Build notes and `upload-lifecycle-test-status.json`
record image hashes and verification scope. Prior images are preserved under
`historical-before-upload-lifecycle-2026-10-09`. Neither image was flashed or
physically qualified; no device settings were changed.

Uploads continue opening their destination with `"w"`; this change does not
preserve a prior file on failed replacement. Partial-file removal is best-effort
and can leave output if both cleanup attempts fail. Host tests do not qualify
real HTTP/SD behavior, String allocation failures or power-loss durability.
Capacity/status consistency and any future atomic-upload policy remain separate
milestones.

## Shared storage routing and capacity validity — 2026-10-09

**Purpose:** make VFS the single owner of storage-path routing and capacity
sample validity, so consumers do not invent a different SD-prefix rule or
report a failed capacity query as a successful zero-byte volume.

`processStatDeferred()` now uses the existing `VFS::getStorageType()` instead
of its own prefix test. `/sd` and its descendants select SD; names such as
`/sdata` and `/sdcard` stay internal, even when SD is unavailable. Existing
RPC authorization, identity scopes, status codes and packed wire fields remain
unchanged. Web and CLI already share `buildFilesStatsJson()`; that builder now
checks the capacity-query result and reports `Storage statistics unavailable`
on failure, retaining its specific unmounted-SD message and successful JSON.

The VFS sampler now has one validity flag instead of separate public-success
and cacheability flags. A reported LittleFS info error or a zero-total volume
fails without changing caller outputs or publishing a cache sample. Both
storage backends share the free-byte calculation and sample timestamp. All
capacity callers were inspected before tightening this contract: ignored-result
callers initialize outputs, and uninitialized-output callers read them only
after success. The G2 capacity comment was updated; its display behavior was
not refactored.

Fresh `getStats()` still samples the backend. Presentation snapshots keep their
explicit age limit, second cache check after locking, generation fencing and
per-tier invalidation. A failed fresh read leaves a prior valid cached sample
available only within the presentation caller's allowed age. No extra cache,
storage interface or backend abstraction was added. Runtime/public-header
changes remove 20 lines overall, including the G2 comment adjustment.

Verification reproduced 12 false-success scenarios and 12 routing failures
against the saved pre-change sources. The actual sampler/APIs/cache/JSON suite
passes 34 scenarios with ASan/UBSan, including backend errors with nonzero
outputs, output preservation, empty volumes, free-space clamping, caching, invalidation,
nested locking and 64-bit SD capacity. The actual ESP-NOW handlers pass 44 STAT
scenarios plus 108 existing privacy/path checks and ordinary/error behavior.
Its older harness was updated to compile the shared directory reader, and the
suite is now registered in CTest. Four focused CTest suites pass; final review
found no blockers.

Both saved profiles build with ESP-IDF 5.5.5. QT Py is 2,890,720 bytes with
3,093,536 bytes free (+16 bytes); XIAO is 5,882,128 bytes with 102,128 bytes
free (-240 bytes). Saved profiles, resolved choices and dependency locks are
unchanged. Build notes and `storage-stats-test-status.json` record image hashes;
prior artifacts are preserved under `historical-before-storage-stats-2026-10-09`.
Neither image was flashed or physically qualified.

The owner can only reject errors exposed by its backends. In particular, SD's
used-byte accessor returns zero both for an empty volume and for a failed read;
zero usage alone remains valid. Host tests do not qualify physical SD/radio
behavior or detect errors hidden inside a driver. Atomic upload replacement
remains a separate possible milestone.

## Image storage inventory through shared VFS — 2026-10-09

**Purpose:** remove repeated SD/internal implementations from image inventory
while keeping image policy in `ImageManager` and storage mechanics in VFS.

The code and its consumers were inspected before editing. `ensureCaptureFolder`,
`listImages`, `getImageInfo`, `deleteImage` and `getStorageStats` now select the
storage tier once and use one shared operation. Listing reuses the existing
`VFS::DirectoryReader`; image metadata, deletion and reads use
`VFS::getStorageType` for routing. This removes 81 lines from `ImageManager`,
including three unused backend includes, without introducing another storage
interface. Cached availability, authorization reasons, JPEG filtering, backend
order and the existing internal-storage interpretation of `BOTH` for inventory
remain intact. Capture, save policy and the retention comparator are unchanged.

The scan found incorrect `/sd` prefix checks in metadata/deletion: `/sdata`
could be treated as unavailable when SD was absent. It also found that SD
listing and metadata reads released the guarded-open lock before the handle's
remaining work. The shared operations hold an existing filesystem guard through
traversal, metadata access and close. Internal capacity-query failures now
return unavailable, as SD failures already did. `getImageInfo` has no current
live consumer; its routing and lifetime fix keeps the public API consistent.

`DirectoryEntry` exposes an opt-in modification time for SD image retention.
Default readers, virtual entries, directories and internal image listings leave
it zero, avoiding extra backend stats for existing consumers. SD inventory
requests timestamps for all regular entries before its JPEG filter, so non-JPEG
files incur an additional stat compared with the old image-specific loop.
Returned paths use the reader's canonical direct-child handling, including root
folders and rejection of malformed/non-child backend names.

Verification reproduced two routing failures, three lock-lifetime failures and
the internal capacity false-success against the saved pre-change sources. The
new production-extraction suite passes 42 scenarios and 988 assertions with
ASan/UBSan, covering these failures, availability, audit reasons, folder/open/
delete failures, roots, filtering, timestamps, retention selection and partial
deletion, nested guards and JPEG-read compatibility. Five registered CTest
suites pass, including the existing web/CLI, FileManager and ESP-NOW directory
consumers. Independent final review found no actionable defects or critical
test gaps. Mocked filesystem/RTOS boundaries verify scoped ownership and
outcomes, not physical-card behavior or real task contention.

Both saved profiles build with ESP-IDF 5.5.5. QT Py is 2,890,928 bytes with
3,093,328 bytes free (+208 bytes); XIAO is 5,879,728 bytes with 104,528 bytes
free (-2,400 bytes). Saved profiles, resolved feature choices and dependency
locks are unchanged. Build notes and `image-storage-test-status.json` record
hashes and verification scope; prior images and status are preserved under
`historical-before-image-storage-2026-10-09`. Neither image was flashed or
physically qualified. Image-save write completion and cleanup remain a separate
follow-up; this inventory change does not repair or qualify those writes.

## Checked image saves with one per-tier routine — 2026-10-09

**Purpose:** make SD, internal and dual-destination image saves follow one
completion and cleanup policy, so an opened file is not mistaken for a saved
image.

Inspection found four repeated write branches, unchecked write results and
folder-creation failures, overflowing reserve arithmetic, and dual saves that
logged insufficient space but still wrote the rejected tier. The existing
`writeText` completion checks provided the pattern; its text-only trusted API
was not appropriate for binary image buffers and guarded authorization.
`ImageManager::saveImage` now has one local per-tier routine using the existing
VFS and filesystem guard. The function shrinks from 125 to 76 lines, with no
new public helper or storage abstraction.

Each requested available tier validates its destination, requires a successful
fresh capacity query and sufficient reserve, then checks folder creation before
opening. One guard covers admission, write, flush, completion checks, close and
failed-output cleanup. A save must report the full byte count, no write error
and the expected final size. Incomplete regular output is closed and removed
best-effort using its existing save authorization identity; failed opens and
directory handles are never removed. Actual internal bytes update the existing
VFS write-pressure hint, and an actual incomplete SD write invalidates its
writable-health flag. Admission or authorization rejection does not report a
hardware write fault.

The normalized path must route to the requested tier. This deliberately rejects
an internal save configured with a folder such as `/sd/photos`, which previously
checked internal capacity before writing SD. Root, repeated-slash and dot-segment
paths use the shared normalizer; returned paths are canonical. Suffix appends
are checked before I/O, but unchanged filename/folder/normalization helpers still
use Arduino String allocation behavior; this is not general allocation hardening.

Dual saves still attempt internal storage and then SD independently, succeed
if either completes, and prefer the internal path when both complete. One
`photo_saved` event and the existing retention call follow resource release,
only after a completed save. The original retention location argument is
preserved, including the existing internal-only interpretation of `BOTH` for
inventory/retention. One filename is generated for both attempts; rejected
admission may now consume a counter value. No capture hardware code, settings
or feature choices were changed.

Before applying the change, the extracted production implementation reproduced
11 failures: five short-write cases, two ignored folder failures, three rejected
capacity cases in BOTH mode, and reserve overflow. The completed suite passes
74 scenarios and 1,526 assertions with ASan/UBSan. Coverage includes zero/short
writes, reported errors, final-size mismatch, cleanup failure, per-tier fallback,
invalid/routed paths, nested lock ownership, existing-file preservation before
an unsuccessful open, accounting/health hints and success-only hooks. The
existing 42-scenario image-inventory suite also passes; both registered CTests
pass. Independent review found no production defect or critical coverage gap.

The camera-enabled XIAO profile builds with ESP-IDF 5.5.5: 5,878,800 bytes with
105,456 bytes free (928 bytes smaller). The QT Py baseline, which disables the
camera, also builds; its 2,890,928-byte image is byte-identical to the preceding
build, with 3,093,328 bytes free. Saved profiles, resolved choices and dependency
locks are unchanged. Build notes and `image-writes-test-status.json` record
hashes and verification; prior artifacts are preserved under
`historical-before-image-writes-2026-10-09`. Neither image was flashed or
physically qualified.

Saves retain direct create/truncate behavior at generated filenames. A filename
collision can overwrite an earlier photo, and failed-write cleanup can remove
that truncated destination; cleanup refusal can leave partial output. This is
not atomic replacement or power-loss durability. Arduino flush/close do not
expose their backend error results, and host filesystem/RTOS doubles do not
qualify real SD behavior or cross-task contention.

## Filesystem hardware follow-up — 2026-10-09

The accumulated filesystem work now has physical QT Py verification in addition
to its host coverage. A fresh cumulative selection passed 21 host suites. Temporary
diagnostic builds passed 41 real LittleFS/VFS/ImageManager checks and 28 real HTTP
loopback checks, including two-task atomic-save contention, rename refusal,
binary image-buffer roundtrips and interrupted-upload cleanup. Private fixtures
were removed and original runtime feature choices restored.

The normal 2,890,928-byte QT Py application was rebuilt byte-identically, flashed
with hash verification, and passed CLI write/read/list/rename/delete checks.
Crash count remained zero; the temporary hooks are absent from source and the
restored image. No permanent firmware fix was needed. Two test-harness assumptions
were corrected, with failed attempts retained in local evidence.

See [filesystem self-testing](QTPY_STORAGE_TEST.md) for image identities, evidence
and the distinction between diagnostic checks and the normal-image smoke test.
Physical SD, camera capture and an ESP-NOW peer remain untested; HTTP used device
loopback with the Wi-Fi radio off. Earlier slice entries and XIAO qualification
status remain historical and are not broadened by this QT Py run.

## Real camera and peer-radio qualification — 2026-10-09

The XIAO/QT Py follow-up closes the camera-capture and ESP-NOW peer gaps in the
filesystem work. Five real OV3660 JPEGs passed storage/readback and independent
decoding, including capture with ESP-NOW active. Encrypted text worked in both
directions, camera images arrived byte-identically, and a reverse binary file
transfer matched exactly. A temporary QT Py diagnostic passed 24 checks against
the production binary LIST/STAT/GET APIs, including pagination and error paths.

`qtpy-espnow` copies the existing OLED/sensor profile with network level 3; the
normal camera-enabled XIAO profile is unchanged. Both normal firmware images
were verified on-device, pairing survived reboot, and all test fixtures and
temporary hooks were removed. Camera settings were restored; the requested
encrypted pairing remains configured. No permanent runtime implementation fix
was needed. Startup warnings and a non-increasing pre-test send-failure counter
are recorded alongside the successful operations; SD remains unqualified.

See [camera and ESP-NOW verification](XIAO_QTPY_CAMERA_ESPNOW_TEST.md) for exact
firmware identities, results, final device state and scope limits.

## Complete NeoPixel ownership — 2026-10-09

The pre-change scan found that `System_NeoPixel` already owned the LED driver,
color/effect operations and settings schema. OLED and G2 screens already used
the authorized command executor. The remaining split was `System_Hardware.cpp`:
all 105 lines implemented six persistent LED commands, including direct access
to the public NeoPixel driver instance.

Those commands and their table now live beside the existing settings schema in
`System_NeoPixel.cpp`. The driver and six handlers have internal linkage. The
public header no longer imports the Adafruit driver or exports its instance;
startup's unused driver include/declaration and six unused handler declarations
in `System_Settings.h` are removed. The obsolete source and its CMake entry are
gone. This removes a module and external driver access without adding a wrapper,
new interface, or alternate configuration path.

The `led` and `neopixel` command groups, order, names, permission flags, output
strings, settings schema and `ENABLE_NEOPIXEL` guards are preserved. Board power
rail initialization remains available independently of the LED feature. Parsing,
effect timing and the existing startup brightness fallback are unchanged.

Verification:

- Compared all six moved handler bodies and their table/count against the
  immediate pre-change snapshots; they match apart from private linkage and
  removal of the obsolete driver declaration. The catalog is byte-identical.
- Five existing host suites pass: `command_catalogue`, `command_header_boundary`,
  `command_lookup_core_tests`, `oled_without_input`, and `g2_disabled_header`.
- Built both profiles below. Symbol inspection confirms a private driver and
  handlers with exported registration tables in the enabled build, and their
  absence with the independent power-rail function retained in the disabled one.
- Saved inputs, effective flags, SDK settings, partitions and dependency locks
  match the immediate baseline. An independent source review and whitespace
  check passed.

| Profile | NeoPixel | App bytes | Change | Free app bytes |
| --- | --- | ---: | ---: | ---: |
| `qtpy-espnow` | Enabled | 3,443,584 | -48 | 2,540,672 |
| `xiao-default` | Disabled | 5,878,800 | 0 | 105,456 |

This is a behavior-preserving ownership change; neither board was reflashed.
The camera/radio hardware results above continue to identify the earlier images
still on the boards. Each build's `BUILD_NOTES.md` and
`led-owner-build-status.json` distinguish this compile check from that physical
qualification. No SD work was performed.

## One boolean parser for MQTT toggles — 2026-10-09

The scan found an existing shared parser in `System_Utils.h`, already used by
camera and ESP-NOW handlers. MQTT instead maintained four parsing sites for
thirteen commands: client enable, auto-start, external subscriptions and the
ten publish toggles generated by `MQTT_PUBLISH_CMD`.

Those four sites now call `parseBoolArg` and reject its invalid result before
`setSetting` or `stopMQTT`. No new parser, wrapper or settings mechanism was
introduced. MQTT retains its own query/response wording, including ON/OFF
queries, subscription restart advice and asynchronous stop acknowledgement.
The generic `settingBoolToggle` was not substituted because its response shape
does not cover these existing differences.

Two deliberate behavior corrections accompany this consolidation:

- Invalid nonempty input no longer silently disables client, auto-start or
  publish settings, or schedules a client stop. It returns an `Error:` response
  without changing state or requesting persistence/setting-change events.
- All thirteen commands accept `1/0`, `true/false`, `on/off` and
  `enable/disable`, case-insensitively with surrounding whitespace. This extends
  the older subscription spellings and fixes the inconsistent `enable` behavior.

Empty queries and validation-only early returns are preserved. Explicit client
disable still requests a stop even when the setting is already false; enabling
the setting does not start MQTT. The existing changed-only settings persistence,
notification order and persistence-failure handling remain unchanged. Command
names, registrations, permissions and saved configuration choices are intact.

Verification:

- The new `mqtt_boolean_commands` suite compiles the production handlers,
  publish macro, parser, validation macro and settings mutator. It passes 6,448
  cases across the Gamepad and ANO Encoder labels under ASan/UBSan, covering
  aliases, malformed values, queries, unrelated settings, response strings,
  unchanged-value suppression and persistence/notification/stop ordering.
  The same expectations reproduce the former regressions against pre-change
  source snapshots. Host boundaries substitute String storage, persistence
  requests, notifications and stop scheduling.
- All five selected CTests pass: that suite plus `mqtt_feature_config`,
  `mqtt_lifecycle_gate_tests`, `mqtt_lifecycle_contract` and
  `setup_mqtt_application`.
- A fresh pre-change MQTT build establishes the immediate size baseline below.
  Both final builds retain their resolved features, SDK settings, partitions
  and dependency locks. The disabled image is byte-identical to its baseline.

| Profile | MQTT | App bytes | Change | Free app bytes |
| --- | --- | ---: | ---: | ---: |
| `xiao-mqtt` | Enabled | 5,778,112 | -1,520 | 206,144 |
| `xiao-default` | Disabled | 5,878,800 | 0 | 105,456 |

Neither image was flashed and no live broker connection was tested. This slice
changes command parsing, not the MQTT transport/lifecycle implementation.
Build notes and `mqtt-toggle-test-status.json` identify the compiled images
separately from earlier hardware qualification. SD work remains deferred.

## Bluetooth-owned structured status — 2026-10-09

The scan found the desired pattern in the existing sensor JSON builders and
G2 status snapshots. Bluetooth's web handler instead executed `bleinfo json`
as an internal command, copied the result into a static 4 KiB buffer, parsed
another JSON document, then copied that document into its response.

`Bluetooth.cpp` now owns `buildBLEInfoJson(JsonObject)`. Both the CLI JSON
command and web handler call it with their own document. The intermediate
command, buffer and parse are removed. Successful CLI JSON and text output
retain their existing schema and wording; command mutation/authentication paths
remain unchanged. The web handler still authenticates before reading status.

Live connection fields and strings are copied while holding the existing BLE
lifecycle mutex. Matching state publication, advertising-state and command
counter writes now use that same guard. Serialization and newly touched BLE
stack calls stay outside those guard scopes. Before the first server start,
the builder returns uninitialized status without dereferencing unpublished
state. The disabled-feature header reports unavailable.

Busy/OOM failures produce explicit error responses rather than a successful
partial document. The CLI also detects output larger than its existing buffer.
Tests caught an existing G2 detail-string lifetime bug in the same HTTP handler:
the document now copies that temporary snapshot string before it goes out of
scope. The web refresh preserves its last display on HTTP failures.

This is a small shared formatter, not a new service framework or a full BLE
lifecycle rewrite. Settings retain their previous synchronization contract;
raw text/OLED state readers and existing advertising timing races remain
separate work. The optional ring status still uses its existing command path.
The unchanged initial web mode loader still falls back to server mode on an
error; the next successful poll corrects it.

Verification:

- `ble_info_status` executes the production builder, CLI, HTTP handler and
  relevant callback/advertising methods under ASan/UBSan. G2-disabled and
  G2-enabled variants pass 2,653 and 2,788 assertions. Cases include ownership,
  concurrent readers and a guarded writer, lock refusal, allocation failure,
  oversized CLI output, HTTP authentication and G2/ring compatibility.
- A pre-change source comparison passes 66 baseline assertions and verifies
  byte-for-byte matching successful CLI JSON/text. The actual Bluetooth-disabled
  public header also compiles and reports unavailable.
- All four selected CTests pass: `ble_info_status`, `hal_bluetooth`,
  `command_ingress_guards` and `g2_disabled_header`. The standalone
  `test_stt_ble_privacy.py --sanitize` check also passes.
- Both saved profiles build with unchanged saved choices, generated feature
  inputs, SDK configuration, partitions, resolved flags and dependency locks.
  QT Py's executable/data bytes are unchanged; its embedded ELF hash and derived
  image checksums differ after rebuilding the comment-edited source.

| Profile | Bluetooth | App bytes | Change | Free app bytes |
| --- | --- | ---: | ---: | ---: |
| `xiao-default` | Enabled | 5,879,312 | +512 | 104,944 |
| `qtpy-espnow` | Disabled | 3,443,584 | 0 | 2,540,672 |

The normal XIAO image was app-only flashed with hash verification, preserving
its filesystem and NVS. Three server start/stop cycles and twelve status reads
passed on hardware: advertising when started, uninitialized after stopping,
with empty client lists. A final reboot restored client mode, G2 idle and the
server uninitialized; the crash count remained zero. Saved settings match except
the two mesh secrets' randomized at-rest ciphertexts after normal saves.
ESP-NOW status still matches the baseline: initialized, channel 6, one saved
peer. Serial was logged out and closed. One USB re-enumeration interrupted the
restore monitor; reconnect and final checks succeeded without a recorded crash.
The build's `ble-status-test-status.json` records the current image and results.
QT Py was compiled but not reflashed.
Connected BLE clients and live HTTP were not physically tested; the HTTP path
was exercised by the host harness. SD work remains deferred.

## Ring-owned structured status — 2026-10-09

The remaining Bluetooth web status bridge executed `ringstatus json`, copied
its output into a static 512-byte buffer, parsed another document and retained
a text fallback with a separate browser parser. The existing ring telemetry
and control snapshots were inspected first: they cover different data and omit
the connection identity, MTU and counters needed by this panel.

`G2_Ring.cpp` now owns `g2RingBuildStatusJson(JsonObject)`, shared by the CLI
and web handler. The twelve-field schema, ordinary CLI JSON/text output,
validation-only behavior and command permissions are preserved. Identity
strings belong to the caller's JSON document, including the former empty-string
fallback for an invalid Arduino String. Pending status uses the existing
`g2RingConnectInFlight()` reader. The disabled API reports unavailable.

The web handler fills its response directly. Its internal command executor,
intermediate buffer/document and text fallback are removed, along with the
browser's `parseRingStatus` function. Status reads no longer need
`System_Utils.h` in this translation unit. User commands still use their existing
authorized command path. CLI allocation/size failures and HTTP allocation or
availability failures produce explicit errors; the CLI no longer silently
drops fields to fit its 320-byte buffer. Web output is independent of that cap.

Switching the CLI serializer exposed a compatibility issue in the vendored
ArduinoJson: unnamed control bytes were emitted raw, whereas CompactJson escaped
them. The shared `TextFormatter::writeChar` now escapes every control byte,
preserving named escapes and UTF-8. This fixes the responsible serializer
instead of adding ring-specific escaping. The changed function matches the
vendor's clang-format style.

Concurrency scope remains explicit: ring fields are independently sampled.
Identity strings still have preexisting unguarded writers in both ring and G2
scan paths, and no single existing lock covers all connection fields. The old
HTTP command execution ran directly in the HTTP context too. This extraction
does not claim an atomic ring snapshot or change connection lifecycle/writers;
wrapping only the reader in a mutex would not resolve those races.

Verification:

- Eight selected CTests pass: `ble_info_status`, `ring_info_status`,
  `g2_disabled_header`, `command_ingress_guards`, `web_batch_handlers`,
  `web_file_response_policy`, `filesystem_psram_listing` and `ota_psram_replies`.
  Production C++ harnesses use ASan/UBSan where supported.
- The ring suite passes 1,299 assertions, including owned strings, invalid
  String fallback, uptime wrap, stack sampling, allocation failures and exact
  319/320-byte capacity boundaries. Actual disabled headers run with either or
  both feature gates disabled. A 320-assertion pre-change CompactJson baseline
  matches ordinary CLI/text bytes and parsed escaped values.
- Strict Python JSON decoding checks all 32 control bytes in keys/values,
  explicit-length embedded NUL, quotes, slashes and UTF-8, with compact/pretty
  output and measured lengths. The same test rejects the pre-fix serializer.
  Updated BLE HTTP tests cover authentication before owner access, unavailable
  and allocation responses, and ring output larger than the CLI buffer.
- Both builds retain saved choices, generated feature inputs, resolved flags,
  SDK settings, partitions and dependency locks.

| Profile | Bluetooth/ring | App bytes | Change | Free app bytes |
| --- | --- | ---: | ---: | ---: |
| `xiao-default` | Enabled | 5,872,864 | -6,448 | 111,392 |
| `qtpy-espnow` | Disabled | 3,443,888 | +304 | 2,540,368 |

Neither image was flashed. This pass changes reporting and serialization;
physical ring connections, live HTTP and device timing were not qualified.
Build notes and `ring-status-test-status.json` distinguish these images from
the earlier hardware-tested images. SD work remains deferred.

## Typed G2 connection status — 2026-10-09

The remaining G2 status path formatted a text line in `getG2Status`, then used
browser regular expressions to recover connection flags, MTUs, batteries and
packet counts. Existing `g2ControlStatusSnapshot` demonstrated the caller-owned
snapshot pattern, while `g2LinkSnapshot` already grouped the two link flags.
Neither exposed the complete set of readings this panel needs.

`G2ConnectionStatus` and `g2ConnectionStatusSnapshot` now provide those readings
through the glasses owner. `getG2Status` retains its public API and exact text
format for CLI/OLED consumers. A shared `formatG2Status` lets HTTP produce the
legacy display text from the same captured values without sampling again.
The disabled snapshot reports unavailable without changing caller data, and
disabled text functions respect zero-capacity buffers.

The HTTP response adds `g2.connection` beside the existing firmware, receive
queue and control metadata. `g2Text` remains available for display and response
compatibility. The browser's `parseG2Status` function is removed; connection
state and numerical readings come directly from the structured fields.
Explicit numeric rendering preserves zero values and the `-1` unknown-battery
marker. Off versus initialized idle, partial connections and the browser's
existing link-based state precedence remain unchanged. Missing structured data
and failed requests retain the previous display. Mutations still use the
existing authorized commands.

The snapshot reuses the existing topology reader for link flags. Lifecycle
state, batteries, MTUs and counters retain their independently sampled read
contract. In particular, `gG2State` can be freed during teardown; this pass does
not introduce a shared lifecycle lock or claim whole-device snapshot coherence.
No lifecycle, callback, connection or state-publication behavior was changed.
The later runtime-ownership record below supersedes this lifecycle limitation
for the three-field client record and its status readers; independent telemetry
and other temple consumers still retain their existing contracts.

Verification:

- Six selected CTests pass: `g2_connection_status`, `g2_status_browser`,
  `ble_info_status`, `ring_info_status`, `g2_disabled_header` and
  `command_ingress_guards`. Applicable C++ harnesses run with ASan/UBSan.
- Production text matches the saved pre-change implementation byte-for-byte
  across FSM/link combinations and output capacities. Snapshot tests also
  cover off/idle, numerical bounds, independent copies, captured formatting,
  and null/zero-length buffers. The real disabled header runs under all three
  existing feature/SDK-off combinations.
- Browser tests pass 449 checks, including comparison against the former DOM
  output. Conflicting diagnostic text no longer overrides structured status.
  HTTP tests retain authentication, allocation-failure handling, ring fields
  and existing G2 controls while checking the new connection fields.
- Both firmware profiles build with unchanged saved choices, resolved flags,
  SDK settings, partitions and dependency locks.

| Profile | G2 | App bytes | Change | Free app bytes |
| --- | --- | ---: | ---: | ---: |
| `xiao-default` | Enabled | 5,873,424 | +560 | 110,832 |
| `qtpy-espnow` | Disabled | 3,443,888 | 0 | 2,540,368 |

Neither image was flashed. Physical glasses, live HTTP/browser layout and
device timing were not qualified here. Build notes and
`g2-status-test-status.json` identify these compiled images separately from
earlier hardware-tested firmware. SD work remains deferred.


## Completed: private G2 runtime ownership (2026-10-09)

The former public `G2ClientState` contained fifteen fields, but only
`initialized`, `state` and `connectedSince` had production uses. Its heap
allocation, placement construction and three destruction/free paths also left
status readers outside the worker teardown barriers. The replacement is a
three-field file-private `G2ClientRuntime` with program lifetime, protected by
the existing `gControlLifecycleMux`; no new task, mutex, or lifecycle framework
was added.

All reads and writes of those fields now share the guard. Init publishes ready
only at its existing success point, rollback clears the record, and successful
teardown resets it after the existing worker, scan and temple barriers. A
refused/deferred teardown retains readable initialized state. Connection
completion publishes state and session timestamp together; missing-temple repair
retains the session timestamp. Existing role-transition and initialization claims
handle startup in progress. Connect entry points now test readiness rather than
mistaking an allocated-but-unready pointer for an initialized client. The two
page/tap admission functions read readiness inside their existing critical
sections, without recursively taking the lifecycle guard.

Status captures lifecycle and topology in the explicit lifecycle-then-topology
lock order, and derives its state name from those captured values. Init rebuilds
the String-bearing temple objects outside locks, so status uses the existing
initialization claim to skip temple/topology/telemetry reads during that window
and reports off/default readings until publication. Stable off-state diagnostic
values, CLI/OLED text, public enums/functions, the web schema, partial-link state
mapping and disabled behavior remain compatible. Live MTUs, battery values and
traffic counters still have independent writers; other standalone temple readers
are outside this change. This is not whole-G2 concurrency hardening.

Downstream checks covered web/OLED presentation, radio readiness consumers,
recovery/session diagnostics and UI worker submission. The experimental
`test_g2_submit_lifetime.py` fixture supports both the new private runtime and
its historical heap-backed source. The pinned overlay/source artifacts were not
rewritten; the default prepared `private/app` is absent locally, so that test was
run against the current root and a saved legacy source snapshot instead.

Verification:

- Seven focused CTests pass: `g2_client_lifecycle`, `g2_connection_status`,
  `g2_status_browser`, `ble_info_status`, `ring_info_status`,
  `g2_disabled_header` and `command_ingress_guards`.
- Actual production initialization/teardown control flow matches the saved
  baseline for 13 startup refusals, 6 rollback combinations, 8 UI-worker
  availability combinations, and 12 deferred teardown cases. Driver and worker
  operations are controlled host boundaries. Page/tap admission exercises the
  production functions and checks lock re-entry and prerequisite handling.
- Four concurrent readers exercise production helpers across 12,000 runtime
  publication/reset cycles and 100 actual init/deinit control-flow cycles with
  real host mutexes. ASan/UBSan and a separate ThreadSanitizer run pass; the latter
  recorded 114,518 snapshots. These test synchronization under the host model,
  not physical BLE timing or radio resource retirement.
- Status text matches the pre-change production implementation byte-for-byte;
  147,067 assertions also cover startup read exclusion, state/link combinations,
  independent caller copies and buffer boundaries. Browser/HTTP consumers and
  disabled header combinations pass their existing tests.
- Fifteen additional downstream experiment tests pass against the current root
  (enqueue lifetime, native protocol/role behavior, ring notify/security), plus
  two legacy-source enqueue tests. The old guard-placement negative control
  still detects the original enqueue lifetime defect.
- Both saved profiles build. `xiao-default` is 5,874,624 bytes (+1,200), with
  109,632 bytes free; `qtpy-espnow` is 3,443,888 bytes (unchanged), with 2,540,368
  bytes free. Saved/resolved feature choices, SDK settings, partitions and
  dependency locks match the baseline. `git diff --check` passes.

The normal XIAO image was app-only flashed, hash-verified and matched to its
boot ELF. Five stop/start cycles (ten init calls including already-ready checks,
ten deinit calls including already-off checks, sixteen status reads) pass with
Wi-Fi/ESP-NOW temporarily stopped to satisfy the unchanged memory floor. Boot
count remains 22 throughout the cycle run and crash count remains zero. Final
reboot restores G2 initialized/idle, BLE server uninitialized, ESP-NOW channel 6
with one peer, Wi-Fi disconnected and HTTP stopped. Saved settings match exactly;
serial is logged out and closed. QT Py was built, not flashed.

Hardware qualification and evidence are recorded with this build's
`g2-lifecycle-test-status.json`. Physical G2/R1 links are unavailable, and live
HTTP polling needs Wi-Fi setup (the XIAO has no saved network). A direct
runtime-only stop/start with the normal radio load is refused by the existing
84 KB memory floor while 82 KB is free. The same refusal was reproduced by
flashing the saved pre-change image; memory admission policy was not changed.

Evidence: the private G2 lifecycle evidence archive, including saved source/build
baselines, host/sanitizer logs, downstream commands, build identity comparisons
and private serial logs. Build-specific summaries retain artifact hashes; older
hardware records describe their own images. SD remains deferred.

## Completed: G2 memory admission after BLE cleanup (2026-10-09)

The previous G2 startup checked its 86,016-byte memory requirement while the
host from a runtime-only stop was still resident. On the XIAO, the baseline
refused with 84,975 bytes available; completing the existing BLE shutdown path
returned enough memory for the unchanged gate. Server startup already performs
cleanup before checking its own requirement.

`initG2Client` now uses that same sequence: acquire the existing role/init
claims, complete checked server/host cleanup and both temple-quiescence checks,
then consult the existing `g2client` entry before reconstructing temples or
starting a new host or workers. The shared TX mutex remains available for
cleanup. No new owner, budget calculator, board exception, or threshold was
introduced. Debug-only probes report free internal memory and its largest block
before cleanup, at admission, after BLE initialization, and at ready publication.

Downstream behavior is explicit: cleanup failure or a busy owner still stops
startup; a memory refusal after successful cleanup leaves the previous role
stopped. Both claims release so a later attempt can succeed. The cold-start
floor stays conservative when page/tap/ring workers persist. The session worker
is cooperatively stopped and recreated. The gate checks total internal memory;
it does not guarantee contiguous allocations or qualify concurrent radio use.

Verification:

- Seven focused CTests pass: G2 lifecycle, connection status, browser status,
  BLE/ring status, disabled G2 headers and command ingress guards.
- The lifecycle harness extracts the production registry row, shared memory
  checker and actual init/deinit control flow. Fifteen admission scenarios cover
  below/exactly/above the threshold on cold start, retained-host restart and
  server switch; cleanup failures take precedence over simultaneous shortage.
  Same-instance retries after refusal verify claim release, persistent worker
  reuse and exactly one session-worker recreation. Already-ready init remains
  idempotent. Driver/worker operations are host boundaries, not emulated BLE.
- ASan/UBSan and separate TSan runs pass against current and saved source.
  Saved source reproduces the old premature refusal; unchanged lifecycle
  outcomes still match. Concurrent status/publication coverage remains active.
- Both saved profiles build. XIAO is 5,875,168 bytes (+544), leaving 109,088
  bytes in the app partition. G2-disabled QT Py remains 3,443,888 bytes, leaving
  2,540,368 bytes. Saved choices, resolved features, SDK settings, partitions
  and the shared dependency lock match the baseline. QT Py was not flashed.

The app-only XIAO flash was hash-verified and matched to the boot ELF. A runtime
restart with the original radio load now passes: free internal memory rises
from 84,983 to 141,039 bytes after cleanup and is 74,235 bytes when ready;
the largest block remains 31,744 bytes. Following the user's direction, further
testing uses BLE with ESP-NOW/Wi-Fi stopped. Three stop/start cycles (including
repeated already-off/already-ready calls) and a server-to-G2 switch pass without
a reboot or crash record; the cycle run keeps boot count 26 and crash count 0.
Cold manual startup with BLE auto-start temporarily disabled also passes:
207,751 bytes at admission, 151,739 after BLE initialization, 113,967 when ready.
Auto-start is restored and the final reboot reports G2 idle, BLE server stopped,
ESP-NOW active on channel 6, Wi-Fi disconnected and HTTP stopped, with no crash
record. Saved settings match except two AES secret ciphertexts rewritten with
fresh IVs by normal settings saves; other JSON values match. Serial is logged
out and closed. Low-memory/failure injection is host-tested, not forced on the
physical device.

Physical G2/R1 connections and live HTTP polling were not qualified. Memory
slimming and support for simultaneous BLE/ESP-NOW load are deferred; this slice
only corrects when the existing requirement is measured. Evidence is in
the private G2 memory-admission evidence archive; per-build `g2-memory-test-status.json`
records image identities and hardware scope separately from earlier tests.

## Completed: Bluetooth controls follow their existing owners (2026-10-09)

The Bluetooth page previously issued the retired `g2mic` command, assumed
server mode when a duplicate startup request failed, and changed mode/reconnect
controls without confirming the device's result. A rejected reconnect command
can contain `NOT enabled`, so checking for the word `enabled` was also unsafe.

The page now starts with mode unknown and keeps dependent controls unavailable
until its normal status request succeeds. That response is the single source
for mode; the duplicate `loadMode` path and rendering-triggered extra fetch are
removed. `/api/ble/status` adds G2/R1 `autoReconnect` booleans using the existing
guarded `blePeerAutoReconnectEnabled` owner function. Authentication and all
mutations still use the existing endpoint and queued command executor.

Mode and reconnect actions share a small page-local command/readback function.
It retains confirmed display values, disables overlapping edits, shows the
literal reply or transport error, then reads actual owner state regardless of
whether the request succeeded. A failed readback leaves these controls disabled
until a later status response confirms state. A request sequence rejects stale
responses from before an action or a later manual refresh. Periodic polling
waits for completion before scheduling its next read, so slow responses cannot
be continually discarded by newer polls. Separate reconnect feedback elements
keep a connected ring's rendering from erasing a setting refusal.

The obsolete microphone button is replaced by a link to `/sensors#mic-card`,
guarded by G2, web Sensors and microphone availability. Following it changes
neither the source preference nor an active recording; capture/source operations
remain in the shared microphone controls. The destination's empty-grid check
now counts the existing microphone card, preserving it when no I2C sensor or
camera is available. Command-catalogue audio help describes the current shared
capture path and distinguishes the existing diagnostic commands.

Verification:

- Eight focused CTests pass: `g2_status_browser`, `sensor_mic_card`,
  `ble_info_status`, `ring_info_status`, `g2_connection_status`,
  `g2_disabled_header`, `command_catalogue`, and `command_ingress_guards`.
- Browser coverage includes 361 status-rendering checks and 222 full-page
  control checks. The shipping script runs with DOM, request and timer
  boundaries: failed/invalid bootstrap, HTTP-200 refusal text, request/readback
  failure and recovery, repeat events, stale responses, repeatedly slow polls,
  connected-ring feedback, server-only operation and all eight link-guard
  combinations. Microphone navigation emits no capture/source command.
- Production C++ status handlers pass ASan/UBSan with G2 enabled and disabled,
  including all four reconnect-policy combinations and authorization before
  owner access. Five production JavaScript empty-grid cases pass; the saved
  baseline fails the microphone-only preservation case as expected.
- Independent review found and verified fixes for slow-poll starvation and
  connected-ring feedback clearing. The final review has no blocking findings.
- Both saved profiles build with unchanged saved choices, resolved feature
  inputs, SDK configuration, partition tables and shared dependency lock.

| Profile | App bytes | Change | Free app bytes |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,876,464 | +1,296 | 107,792 |
| `qtpy-espnow` | 3,443,920 | +32 | 2,540,336 |

These images were not flashed. Live HTTP/browser layout and physical microphone,
G2 or R1 operation were not qualified by this pass. Existing unrelated server
configuration and session-toggle handlers were not redesigned. No memory policy,
radio implementation or SD behavior changed. Evidence is in
the private Bluetooth controls evidence archive, and per-build
`bluetooth-controls-test-status.json` identifies these artifacts separately from
the previously flashed G2 memory-admission image.

## Completed: shared G2 command-completion redraw submission (2026-10-09)

Eight implementations in Network, ESP-NOW, Automations, LED, Settings, Users,
Camera Settings and the embedded LLM page repeated allocation, command-cookie
copying, queue submission and failure cleanup. They now call `g2EnqueueRedraw`
in the existing `G2_HijackCmd` module. The helper owns those mechanics and
returns whether the existing lens queue accepted the job. This removes 186 net
production lines without adding a queue, task or module.

The original command's generation, full sequence, page and submode are copied;
the helper never recaptures current navigation or renders inline. The existing
applier still drops redraws whose menu generation is stale. Header comments now
accurately distinguish that enforced check from sequence/page/submode metadata.
The existing command identity, owner-session fence and bounded enqueue wait
remain in place. Settings retains local-generation checks and clears its pending
result if queuing fails; Users retains its published result for tap-to-recover.
Page-specific success/error parsing and rendering remain with each page.

Nonblocking ESP-NOW event, Notes and Transcription refreshes keep their existing
queue and retry policies. Independent review compared the queue, worker,
disposer, command admission and owner-fenced completion against the saved source;
those implementations are unchanged.

Verification:

- Six focused CTests pass: `g2_redraw`, `g2_disabled_header`,
  `command_ingress_guards`, `settings_downstream_persistence_contract`,
  `user_settings_transaction_guards`, and `wifi_saved_ui`.
- The new redraw test executes the shipping helper, cookie/job types and actual
  worker Redraw branch. Its 102 checks pass with ASan/UBSan, covering preserved
  metadata, navigation before completion and after admission, deferred render,
  both allocation failures, queue rejection and ownership cleanup.
- The production command admission/completion owner-epoch test passes with
  ASan/UBSan; saved-Wi-Fi consumer contracts pass. Independent review found no
  regressions in renderer selection or page-owned result recovery.
- Both saved profiles build; all 15 saved/generated configuration, SDK and
  partition input hashes match the baseline. The resolved build-report feature
  choices are unchanged. `git diff --check` passes.

| Profile | App bytes | Change | Free app bytes |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,873,872 | -2,592 | 110,384 |
| `qtpy-espnow` | 3,443,920 | +0 | 2,540,336 |

These images were not flashed. Physical G2 display behavior and device scheduling
remain unqualified by this pass. No memory policy or SD behavior changed.
Evidence is in the private G2 redraw evidence archive; per-build
`g2-redraw-test-status.json` records artifact identities and verification scope
separately from older hardware records.

## Completed: G2 Power, microphone and Logging completion ownership (2026-10-09)

Power's completion callbacks previously displayed banners, slept for 1.4 seconds
on the command executor, and restored menus using only a current-page check or
no navigation check. They now copy a short result under a page-local critical
section and queue the existing redraw helper with the original command cookie.
The lens worker consumes a matching result, reads current power settings and
renders normal menu rows. Errors remain in the back row (within the existing
row width) until another action; the full result is also logged. All subview
transitions and new requests invalidate earlier results. Allocation/queue
failure clears only the matching result, and another request can proceed.

Submission refusals use the same menu result on the tap dispatcher without a
long banner delay. The pre-destructive-command 150 ms notification pause remains
on the tap path. A second confirm tap cannot accidentally submit a fallback
restart after the first request cleared its pending action. CPU frequency and
I2C-drain policy, command authorization and the power service are unchanged.

Microphone runtime, recording and source completions retain their command policy
but queue one shared refresh request. Once the lens applier accepts the original
cookie, that request wakes the existing live-text session worker. The worker
owns both teardown and recreation, preventing a periodic readout update from
running concurrently with this teardown on another worker. Navigation changes
before consumption or during shutdown prevent recreation. Shutdown failure
uses the live worker's existing failure exit, without sending CREATE afterward.
The recreation forces CREATE even if the asynchronous container mirror has not
yet caught up, and rebinds the exit callback to the exact committed presentation
and lifecycle. Stream rearming remains on the following live tick.

Logging completion now queues the original cookie and rebuilds its rows on the
lens applier, with a page check there. Logger selection/storage behavior and
non-command page entry remain unchanged. No new queue, task or timer was added.

Verification:

- Seven focused CTests pass: `g2_redraw`, `g2_power_completion`,
  `g2_mic_completion`, `g2_disabled_header`, `command_ingress_guards`,
  `power_cpu_frequency`, and `power_save_tick`. The production command
  admission/completion owner-epoch test also passes with ASan/UBSan.
- The full production Power page, redraw helper and worker branch pass 954
  sanitizer checks. These cover borrowed result lifetime, error/refusal text,
  same-page submenu navigation, leave/reopen, overlapping requests, duplicate
  confirmations, submission refusal and allocation/queue rejection. The saved
  baseline fails because its completion accesses the display inline. Independent
  Power review found no blocking issues.
- Production microphone/Logging callbacks, shared queue/dispatch and live
  renderer pass 314 sanitizer checks with microphone support enabled/disabled.
  Cases include stale handoffs, shutdown/navigation/lifecycle failure, delayed
  state publication, exit-callback identity, preserved microphone command
  policy and deferred Logging success/error refreshes.
- Both saved profiles build. All 15 configuration/SDK/partition input hashes
  and resolved build-report choices match the baseline. `git diff --check`
  passes.

| Profile | App bytes | Change | Free app bytes |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,874,864 | +992 | 109,392 |
| `qtpy-espnow` | 3,443,920 | +0 | 2,540,336 |

These builds were not flashed. Tests cover controlled interleavings and production
logic against transport/RTOS boundaries; physical glasses behavior and arbitrary
device scheduling remain unqualified. Memory slimming and SD work remain deferred.
Evidence is in the private G2 completion evidence archive; per-build
`g2-completions-test-status.json` identifies the artifacts separately from earlier
hardware records.

## Completed: consistent G2 rendering interface (2026-10-09)

Several G2-disabled rendering stubs omitted geometry/child arguments or replaced
a typed pointer with `void*`. `g2ShowTextPageRebuildProbe` had no disabled stub.
The same rendering caller therefore could not compile unchanged across feature
selections, even though both branches represented the same operation.

`G2_RenderTypes.h` now owns the existing container geometry, 23 named presets
and text-child specification. Both public rendering and wire-protocol headers
include it; it has no Arduino, configuration or BLE dependency. Struct bodies,
field order and preset declarations were compared byte-for-byte with the saved
source and are unchanged. `G2_Glasses.h` no longer imports the wire-protocol
header, and the text-entry page includes the narrower types it uses. An audit of
consumers found no missing direct protocol dependencies.

The public header defines navigation callback and text-page chrome types once,
outside its feature gate. Eight rendering calls now have matching enabled and
disabled signatures, including the same default geometry/callback arguments.
Disabled stubs report unavailable without calling callbacks or modifying output.
Enabled rendering implementations, protocol builders and runtime behavior were
not changed. Feature ownership and the human-editable build flags are unchanged.

Verification:

- `g2_disabled_header` and `g2_conversate` pass with ASan/UBSan enabled. The first
  now contains six tests/seven compile units: enabled and three disabled gates,
  standalone types, and both protocol/public include orders. It checks the same
  eight function types and 16 call forms across selections, disabled output
  preservation, and that the public header does not import the protocol header.
- The saved disabled header fails the new signature checks as expected. Existing
  protocol/session coverage passes against shipping builders and session code.
- Independent review found no field/preset changes, consumer dependency problems
  or rendering signature mismatches.
- Both saved profiles build with unchanged image sizes. All 15 configuration,
  SDK and partition input hashes, plus resolved build-report choices, match the
  baseline. `git diff --check` passes.

| Profile | App bytes | Change | Free app bytes |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,874,864 | +0 | 109,392 |
| `qtpy-espnow` | 3,443,920 | +0 | 2,540,336 |

These builds were not flashed; physical glasses behavior was not qualified.
Memory slimming and SD work remain deferred. Evidence is in
the private G2 render-interface evidence archive; per-build
`g2-render-api-test-status.json` identifies these artifacts separately from
older hardware records.


## Completed: shared sensor logging selection (2026-10-09)

G2 and OLED duplicated the logger's eight sensor names, bits and labels. Each
screen toggled a bit from its last read, rebuilt a complete comma-separated
selection and queued a replacement command. Two queued actions based on the
same old selection could therefore overwrite each other. The logger also
cleared its live mask before validating a replacement list, so an unknown name
could leave a partial live selection without updating the saved setting.

`System_SensorLogging.h` now defines one immutable sensor catalog with the
existing persisted bits, names, labels and order. The logger uses it for command
lookup, selected-name output and the selection query. G2 and OLED use it for
rows and submit `sensorlog sensors toggle <sensor>` through their existing
authorized command executors. The logger resolves that intent when executed.
OLED's interval/format/back row positions derive from the catalog size.

Replacement parsing now builds and validates a candidate before updating live
state and calling the existing settings mutator. Existing aliases (`input`,
`health`, `ring`), all/none responses, list grammar and persistence behavior are
preserved. A toggle accepts exactly one named sensor and rejects malformed
quotes. Both replacement and toggle retain the running-CSV refusal and hint;
TEXT live changes remain allowed. The G2 completion cookie/redraw path and
OLED guest gate remain intact. No new service, queue or task was introduced.

Verification:

- Five focused CTests pass: `sensorlog_selection`, `sensorlog_ui`,
  `g2_mic_completion`, `command_ingress_guards` and `command_catalogue`, with
  ASan/UBSan enabled for the compiled harnesses.
- The owner harness executes the shipping catalog, CommandArgs parser,
  selection helpers, settings mutator and status branch: 43,388 checks cover
  all mask bits, queued toggle intent, repeated/intervening changes, invalid
  requests leaving both selections unchanged, persistence/notification counts,
  debug-buffer failure, CSV refusal and query/status output.
- The UI harness executes shipping G2/OLED render/input code: 306 checks cover
  labels and checkmarks, repeated taps from stale displayed state, invalid
  indexes, guest refusal, G2 cookie/callback forwarding, start/stop and OLED's
  existing interval/format/back controls.
- The saved baseline reproduces partial mutation and missing-toggle failures;
  its 270 replacement-grammar and 15,360 status assertions pass. The TRACK
  format branch is byte-for-byte unchanged. Independent review found and
  resolved a malformed-quote case; final review found no remaining issue.
- Both saved profiles build. All 15 configuration/SDK/partition hashes and
  resolved build-report choices match the baseline. `git diff --check` passes.

| Profile | App bytes | Change | Free app bytes |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,874,544 | -320 | 109,712 |
| `qtpy-espnow` | 3,443,216 | -704 | 2,541,040 |

These builds were not flashed; physical G2/OLED behavior is not qualified by
this pass. The scalar selection read is explicitly not a synchronized logger
snapshot. Legacy direct mask writers in settings, boot restoration and health
logging remain: this change prevents lost updates between queued UI commands,
not arbitrary concurrent writes. A follow-up should bring those existing
writers under the same owner policy; the raw mask setting currently bypasses
the CSV guard. Session encryption is also unchanged: it is fixed per file,
so adding R1 to a running plaintext TEXT session retains that session's mode.
Health restart/format and TRACK overwrite semantics remain as before.

Memory slimming and SD work remain deferred. Evidence is in
the private sensor-selection evidence archive; per-build
`sensorlog-selection-test-status.json` records the artifacts separately from
older hardware qualification.


## Completed: logger state ownership and health command routing (2026-10-09)

The remaining settings-side writers copied saved mask/format values into live
logger state even after a query or a range error. Numeric mask writes also
bypassed the logger's running-CSV refusal. G2 and web Health directly invoked
the health session mutator outside the existing command executor; OLED queued
an on/off value computed from an earlier settings read.

`System_SensorLogging` now privately owns the selection mask, format and health
session mutator. Small scalar accessors serve presentation; they do not claim
a synchronized snapshot. Numeric `sensorlogmask`/`sensorlogformat` handlers moved
into the logger, with their existing settings-editor registrations, admin
requirements, command keys and settings schema preserved. They reuse the real
generic settings editor for numeric parsing, range validation, persistence,
broadcasts and events. Queries and rejected edits no longer copy saved values
into live state.

Named and numeric selection commands share the existing CSV refusal. Any
nonempty numeric write during a running CSV session receives that refusal,
including same-value or out-of-range writes; queries remain available. This
matches the named selection command's guard precedence. Otherwise the generic
editor's existing parsing and range-error behavior remains unchanged.

Named and numeric format commands share one operation. Numeric Track now also
selects and persists GPS-only, matching `sensorlog format track`; its command
help states that it replaces the saved selection. The named operation's prior
publication order is preserved: publish both live TRACK/GPS values before
settings persistence. Existing immediate format behavior remains, including
while logging, because `gpslog` depends on it. This pass does not redesign
active-session format transitions, encryption or boot import behavior.

G2 Health's portable model emits a consumable request like its existing poll
request. Its transport worker submits `healthlogging toggle` through the G2
command bridge with the current page/subview cookie. Pending requests clear on
entry/exit; completions only log results while the existing worker owns display
updates. Web uses `executeUnifiedWebCommand` with the authenticated request and
an owned result string, preserving success/409 responses while gaining existing
command authorization and live-session checks. OLED sends the same toggle
intent through its existing authorized executor. Health restart/coercion,
autostart persistence, stop conditions and failure behavior are preserved.

Verification:

- Eight focused CTests pass: `sensorlog_selection`, `sensorlog_ui`,
  `sensorlog_settings`, `health_logging_ui`, `command_catalogue`,
  `command_ingress_guards`, `settings_downstream_persistence_contract` and
  `g2_mic_completion`. Compiled harnesses use ASan/UBSan.
- The new settings harness executes shipping handlers, generic settings editor,
  parser and mutator: 10,822 checks with R1 disabled and 11,000 enabled. It
  observes persistence/broadcast/event behavior and format publication order,
  with 178 checks of actual private health setter/restart policy using session
  startup/path-shaping boundaries. Saved pre-change handlers fail 106 expected
  assertions per mode covering old live-state resets, CSV bypass and divergent
  numeric TRACK behavior.
- Health routing has 65 checks executing model/action, worker dispatch, OLED
  input, HTTP handler and the real unified web helper. Cases include request
  clearing/coalescing, submission rejection, guest refusal, identity/session
  flags, stale-session result rejection and unaffected actions. Existing owner
  selection and UI suites pass 43,388 and 306 checks respectively.
- Independent review found a TRACK publication-order change; it was corrected
  and covered before the final builds. No remaining introduced issue was found.
- Both saved profiles build. All 15 configuration/SDK/partition hashes and
  resolved build-report choices match baseline; compiled source timestamps were
  checked against final edits. `git diff --check` passes.
- The broader `settings_registry.py check` still fails four existing HT16K33
  bindings (`matrixbrightness`, `matrixrotation`, `matrixsquare`, `matrixpanel`).
  An actual baseline replay over the component source produces byte-identical
  output: 946 commands, 295 rows and the same four failures. They are unrelated
  to this pass and were not changed.

| Profile | App bytes | Change | Free app bytes |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,875,376 | +832 | 108,880 |
| `qtpy-espnow` | 3,443,216 | +0 | 2,541,040 |

These builds were not flashed. Host tests qualify command routing and controlled
policy cases, not physical glasses/display operation, filesystem writes or
arbitrary task scheduling. The main-loop logger and boot/session lifecycle still
have shared mutable state; privatizing selection/format is not a general
concurrency fix. Memory slimming and SD work remain deferred.

Evidence is in the private logger-ownership evidence archive; per-build
`logger-ownership-test-status.json` identifies these artifacts independently of
older hardware qualification.


## Completed: shared logger startup and checked GPS setup (2026-10-09)

The GPS convenience command still wrote the logger's interval directly, ignored
child-operation errors and claimed GPS had started even when its request was
only queued. It used boot restoration for manual startup, allowing enabled
health policy to change a requested GPS track into health capture. Boot and
health startup also reconstructed command strings; boot paths with spaces and
health paths containing literal quotes could be parsed incorrectly.

`System_SensorLogging` now owns a private typed start operation reused by the
explicit command, configured-session entry point, boot restoration and health
restart. The extracted file/admission/publication body is unchanged apart from
indentation. Paths stay literal instead of passing through command parsing.
Saved-parameter restoration is shared; boot-only health coercion stays in the
boot wrapper. Existing explicit-command precedence and interval clamping,
including the legacy negative-to-unsigned behavior, are preserved.

`gpslog` validates its entire interval argument before effects, refuses an
existing session or enabled health capture, requests GPS through its existing
queue adapter, and sets Track/interval through logger commands. It preserves the
borrowed GPS result before another command can replace it, checks every outcome,
and enables boot auto-start only after confirmed logger startup. Its response
reports the actual GPS request result separately from the running logger.
Command help now describes that distinction and the existing interval limits.

Interval, maximum size and rotation count are private to the logger. OLED reads
the interval through a scalar accessor and retains its authorized edit command.
No new framework or configuration schema is introduced; command registrations,
permissions and board selections are preserved.

Verification:

- Eight focused CTests pass: `sensorlog_selection`, `sensorlog_ui`,
  `sensorlog_settings`, `gpslog_start`, `health_logging_ui`, `command_catalogue`,
  `command_ingress_guards` and `settings_downstream_persistence_contract`.
  Compiled harnesses use ASan/UBSan.
- New production-extraction tests pass 341 GPS orchestration checks and 215
  logger-start checks. They cover strict input, preflight, borrowed replies,
  queued/running outcomes, child failures, auto-start ordering, literal paths,
  saved bounds, boot health policy, controlled admission and session publication.
  Replaying the actual old GPS handler at those boundaries yields 197 failed
  assertions reproducing its old behavior.
- Settings tests pass 10,822 checks with R1 disabled and 11,012 enabled. The
  latter includes 190 health-transition checks, now exercising typed startup
  and interval limits through `UINT32_MAX`. Replaying the old health restart
  also passes; its prior limits remain intact.
- The extracted file/admission/publication body matches the saved baseline.
  Independent production and test reviews found no remaining introduced issue.
- Both saved profiles build. All 15 configuration/SDK/partition/dependency
  input hashes and resolved build choices match baseline. Final changed sources
  included by each profile have newer compiled objects. `git diff --check`
  passes.

| Profile | App bytes | Change | Free app bytes |
| --- | ---: | ---: | ---: |
| `xiao-default` | 5,875,552 | +176 | 108,704 |
| `qtpy-espnow` | 3,443,792 | +576 | 2,540,464 |

These builds were not flashed. VFS, sealing, clock/path shaping, persistence and
queue outcomes are controlled host boundaries; this does not qualify physical
storage, GPS reception or arbitrary task scheduling. A failed setup can leave
GPS requested and Track/interval saved; previously enabled auto-start preferences
remain unchanged. The operation reports failure without claiming rollback.
Logger lifecycle state still spans tasks; scalar accessors are not a general
concurrency fix. Legacy positive-only saved-mask restoration also remains.

OLED/Web Map startup remains a separate follow-up: those temporary capture flows
must not inherit `gpslog`'s persistent auto-start policy. Memory slimming and SD
work remain deferred. Evidence is in
the private GPS logging startup evidence archive; per-build
`gpslog-start-test-status.json` separates these artifacts from older hardware
qualification.


### Logger hardware follow-up — 2026-10-09

The focused logger regression set passes 8/8 again. Both boards passed live
selection/settings/refusal and file-output checks. QT Py additionally produced
real ToF/presence/GPS CSV rows and maintained valid GPS communication indoors
without a fix. XIAO passed health session start/repeat/stop/restart policy.
The G2 user confirmed the GPS checkmark and Start/Stop redraws after a
clear/reopen recovery in the first pass; the subsequent manual-reopen correction
and its hardware result are recorded below.

Testing found one small integration omission: the existing OLED Logging mode
had no `logging` slug decoder. `OLED_Utils.cpp` now accepts it through the
existing mode/authentication path and lists it in help. No new UI or routing
framework was added. Both profiles rebuilt: QT Py is 3,443,824 bytes (+32 for
this correction), XIAO remains 5,875,552 bytes. The corrected images were
flashed application-only and passed saved logger boot restoration; QT Py also
resumed GPS and accepted the Logging route. All test logger preferences were
restored, temporary services/debug flags cleaned up and no crashes recorded.

In a second pass, authenticated Web Health requests on the 5,875,552-byte XIAO
image passed authentication, invalid-action, offline-ring, logging-toggle and
logout checks. The retrieved sealed-session file contained its marker and
plaintext structural header only; that HTTP test does not establish row
decryption. Separately, the 3,492,096-byte `qtpy-logging-test` image enabled
gamepad/OLED input. The user confirmed the ToF checkmark changed, while command
records and CSV readback verified physical interval/format/Start/Stop actions.
The final visual answer was "I think it worked", so Start/Stop redraws remain
without definite visual confirmation. An actual gamepad capture verified 11
encrypted rows and authorized serial-viewer decryption with matching schema.

The G2 correction reuses the existing connection/lifecycle-guarded teardown
before shared startup for a manual `g2reopen`; real firmware `MenuStartUp`
(command 17) preserves its existing ordering. It adds no parallel clear or
startup implementation. Production-path tests passed 532 assertions, with
owner/epoch and command-catalogue checks also passing. The 5,875,968-byte XIAO
image built with 108,288 bytes free and was flashed with hash verification.
The user saw `REOPEN TEST`, then a single `g2reopen` produced a responding menu
without a separate clear command, and its GPS checkmark changed. A repeat with
the Health lease active recorded cleanup, `ShutdownSuccess`, `CreateSuccess`
and a main-menu return, without separate visual confirmation. These results
qualify replacing a live display; asleep or unresponsive plugin recovery was
not established.

R1 firmware 2.3.2.0007 remains unsupported: an audit found no matching evidence
among 93 raw and 17 decoded captures, and the exact profile gate was preserved.
Live R1 readings and its connected Health indicator remain unqualified. QT Py
second-pass logger/GPS preferences and capture mode were restored. XIAO's
original logger and Bluetooth settings were restored exactly, with logging
inactive, BLE deinitialized, ESP-NOW running with auto-start, and debug flags
cleared. User-added Wi-Fi entries were retained; both final crash queries
report no record. Cleanup evidence includes `remaining-xiao-restore.log` and
`remaining-xiao-final.json` / `remaining-xiao-final.log`. Memory and SD work
remain deferred. See
[the logger verification record](LOGGER_TEST.md) and private evidence
`g2-reopen-visible-result.json` / `g2-reopen-health-repeat.json` in
the private logger hardware-retest evidence archive. The record separates the initial,
OLED-route, OLED-input and G2-reopen image results.


## Completed: shared Map live recording (2026-10-09)

OLED and web each started GPS, changed the logger format/selection, opened the
same capture path and changed the Map live flag independently. They ignored
child-operation failures; web always reported success. Both stop paths stopped
the global logger without establishing that it was still their capture.

The existing `gpstrack` command now accepts `live start`, `live stop` and
`live toggle`. `GPSTrackManager` owns that sequence and reuses the existing GPS
request and logger commands. OLED submits one result-aware display command;
web uses `executeUnifiedWebCommand` with the authenticated request. Both show
failure instead of claiming recording started. Web preserves its successful
response shape, retains polling on failed stop, rejects overlapping clicks and
discards responses from replaced polls. OLED also displays track load/delete
failures and requests a redraw after successful changes.

Startup checks the logger master switch, existing capture and health policy
before effects, and checks allocation/GPS/logger outcomes. It retains an
imported track until startup succeeds, resets all statistics for a new track,
and reports the actual queued/already-running GPS result. The literal
`/logging_captures/tracks/live.csv` path and 1000 ms interval are preserved.
GPS and logger boot auto-start preferences are unchanged. As before, starting
saves last-used logger parameters; a later startup failure may leave GPS
requested and Track format saved. This is checked failure reporting, not a
transactional rollback of every preceding operation.

The logger issues a nonzero boot-local session ID on each successful start.
Maps retains that ID; the logger checks identity and stops together inside a
short critical section. A CLI or health restart at the same path gets a new ID,
so Maps cannot stop that replacement. File rotation retains session identity.
An external logger stop/replacement also makes the Map live predicate false.
Clear/load refuse to replace an active live buffer until recording stops.
GPS-disabled builds retain offline track functions and return unavailable for
live startup. No new service, task, command queue or board-specific policy was
introduced.

Independent review caught a stale live flag during restart after an external
logger stop: adopting the new ID could expose an incompletely reset track.
Startup now revokes that flag before adopting the ID and publishes live only
when initialization is complete. A controlled observation after ID adoption
covers the ordering; removing the early revocation from a temporary generated
unit reproduces its assertion failure.

Verification:

- Nine registered suites pass: `map_live_tracking`, `map_live_ui`,
  `map_live_browser`, `gpslog_start`, `sensorlog_settings`,
  `sensorlog_selection`, `health_logging_ui`, `command_catalogue` and
  `command_ingress_guards`. Compiled suites use ASan/UBSan; browser checks
  execute shipping JavaScript with controlled requests, timers and DOM.
- Map owner tests exercise GPS enabled/disabled, allocation and child failures,
  boot preferences, imported-track preservation, active clear/load refusal,
  same-path replacement and restart publication. UI tests cover authentication,
  source/session identity, refusal, escaped JSON and result-based presentation.
  Existing logger startup tests cover failed-start identity, stale/zero tokens,
  repeated stop and generation wrap. The path-change identity check is a host
  boundary check, not a physical midnight-rotation test.
- Three firmware builds pass with fresh objects for their changed sources.
  All 38 pre-existing configuration/dependency input hashes and both baseline
  profiles' effective feature sets are unchanged. `qtpy-maps-test` was created
  through the setup CLI from `qtpy-logging-test`, changing only `ENABLE_MAPS`
  from 0 to 1. Independent production/UI review and `git diff --check` pass.

| Profile | Relevant combination | App bytes | Free app bytes |
| --- | --- | ---: | ---: |
| `qtpy-maps-test` | Maps, GPS, OLED and web | 3,702,576 | 2,281,680 |
| `xiao-default` | Maps and web, GPS disabled | 5,878,384 | 105,872 |
| `qtpy-logging-test` | GPS/OLED, Maps disabled | 3,492,192 | 2,492,064 |

These images were built, not flashed. Devices retain the firmware qualified in
the previous logger hardware record. Tests do not qualify physical GPS fixes,
OLED layout, actual map-capture writes or arbitrary device scheduling. Track
render/save/web readers still borrow buffers and statistics while GPS appends;
the existing broader buffer synchronization is not solved by command ownership
or the session-ID guard. Memory slimming and SD work remain deferred.

Evidence is in the private Map live-recording evidence archive, including
`host-tests.log`, `publication-mutation-check.log`, `build-results.json` and
`configuration-checks.json`. Each build's `map-live-test-status.json` identifies
its new artifacts separately from older hardware qualification.

**Implementation record — 2026-10-10, step 6: saved Wi-Fi module layout.**

Moved `System_WiFiNetworks.cpp`, `System_WiFiNetworks.h` and
`System_WiFiNetworksInternal.h` into `components/hardwareone/network/wifi/`.
All three files are byte-identical to the checkpoint versions. External
callers use the qualified include path; module-local sibling includes remain
unchanged. The existing component CMake list names the new source explicitly,
following its established nested-source pattern. No behavior, feature choice,
settings schema, command, API signature or ownership policy changed.

The directory README maps list edits, UI snapshots, radio connection copies,
settings serialization, setup acceptance and boot initialization to their
existing owners. Shared screens, radio control and persistence policy stay in
their existing files. Tests remain in the common host suite and copy the nested
layout when executing the owner, so they exercise the actual include boundary.

Verification:

- Nine existing focused suites passed: `wifi_networks`,
  `wifi_network_consumers`, `wifi_saved_ui`, `setup_completion`,
  `network_build_selection`, `saved_build_config`, `build_config_workflow`,
  `build_config_wizard` and `settings_downstream_persistence_contract`.
- Executable owner/UI/setup tests used ASan/UBSan. Wi-Fi-enabled and disabled
  interfaces ran; build selection checked 14 inputs in both early discovery
  and configured phases, with exactly one relocated source and no old path.
- `qtpy-maps-test` built at 3,702,576 bytes, leaving 2,281,680 bytes free.
  `xiao-default` built at 5,878,384 bytes, leaving 105,872 bytes free.
  Both sizes match the checkpoint builds, and each compile database contains
  one owner at its new path with a compiled object.
- Build/profile/dependency selections remain unchanged. File hashes, local
  documentation links and whitespace checks passed. Independent review found
  no missed production callers or affected registry-discovery tools.

These relocated-source builds were not flashed. Hardware validation of the
previous Map milestone uses the preserved checkpoint image and is recorded
separately. This relocation establishes a readable source layout, not new
Wi-Fi connection or persistence behavior.

## Completed: consistent GPS track reads (2026-10-10)

The audit found that OLED status, web responses, validation, saving and rendering
read the track through separate getters or borrowed pointers. Loading/clearing
could free that buffer while a reader still used it; live GPS appends could
change the count and statistics partway through a response. Existing sensor
snapshots and the private Wi-Fi owner mutex provided the local patterns to reuse.

`GPSTrackManager` now returns a value status or an owned point snapshot. A
snapshot captures points and metadata under one private state mutex, then
releases it before rendering, sending HTTP chunks or writing a file. Live web
responses still include the newest 500 points while reporting the full count.
Save responses report the saved copy's count. Validation messages are returned
by value, removing another mutable shared buffer.

A separate lifecycle try-lock refuses overlapping/reentrant start, stop, load,
clear and delete operations without waiting while another task owns a map or
filesystem lock. GPS append uses the state mutex. Map bounds are copied under
the existing map guard before track state is locked; logger commands and file
operations run outside the state lock. This is private owner synchronization,
not a new shared synchronization framework.

Loads parse privately and publish once, returning that same result to the
caller. CLI/OLED request metadata only; HTTP requests owned points. Bounds
rejection stays inside the load operation, removing the caller's unsafe
load/validate/clear sequence. Intentional behavior improvement: missing,
invalid, allocation-failed or out-of-bounds rejected loads preserve the previous
track. OLED reports rejected loads and failed saves; web save errors no longer
claim an empty track when copying or opening the file failed. Commands, authorization,
logging preferences, GPS startup and logger session ownership remain unchanged.

Verification:

- Ten registered suites passed: `map_live_tracking`, `map_live_ui`,
  `map_live_browser`, `gps_snapshot`, `gpslog_start`, `sensorlog_settings`,
  `sensorlog_selection`, `sensorlog_ui`, `command_ingress_guards` and
  `settings_downstream_persistence_contract`. Compiled suites used ASan/UBSan
  where supported; Map owner/UI run with GPS enabled and disabled.
- ThreadSanitizer passed the production owner with GPS enabled and disabled.
  Concurrent tests alternate loaded generations, clear while reading, and
  append live points while reading. Removing the snapshot state lock in a
  temporary generated unit caused race reports, confirming the test detects
  that regression.
- Controlled filesystem/render/HTTP/OLED callbacks replace or clear the owner
  during output. Captured metadata, point arrays, validation and saved counts
  remain consistent, with no track state lock held across those callbacks.
- Allocation failures leave empty outputs and preserve the prior owner state.
  A 10,001-point input caps at 10,000; full and 500-point tail copies allocate
  exactly their required point storage and remain valid after owner clear.
- All three saved profiles built and passed compiler-based feature checks:

| Profile | Relevant selection | Image bytes | App headroom bytes |
|---|---|---:|---:|
| `qtpy-maps-test` | Maps, GPS, OLED and web | 3,705,600 | 2,278,656 |
| `xiao-default` | Maps and web, GPS disabled | 5,880,800 | 103,456 |
| `qtpy-logging-test` | GPS/OLED, Maps disabled | 3,492,192 | 2,492,064 |

The current images were not flashed. These checks establish host concurrency,
capacity correctness and firmware compatibility; they do not qualify physical
GPS fixes, display appearance or device timing/allocation pressure with large
tracks. A full snapshot can require about 120 KB of temporary point storage on
the device; status reads allocate no point buffer. Map tile-cache/waypoint
ownership, memory slimming and SD qualification remain separate work.

Build sizes/hashes, configuration checks, sanitizer logs and the final CTest
report are retained in the private Map snapshot evidence archive. Each build's
`map-snapshot-test-status.json` explicitly records that its image was not flashed.

## Completed: automation logging ownership (2026-10-10)

The pre-change audit found logging state and start/stop in
`System_Automation.cpp`, the writer in `System_Utils.cpp`, and mutable state
exported through both headers and local extern declarations. Command execution
read the active flag directly. Start published active state before its filename;
the writer could observe a filename and captured identity from different
sessions. Partial writes were accepted as successful, and stop ignored failure
to write its final marker.

The session, writer, status and lifecycle now live together in
`System_Automation.cpp`. Its public interface exposes `automationLogActive()` as
a formatting hint and `appendAutoLogEntry()` as the checked operation. Command
execution keeps its existing post-authorization COMMAND/OUTPUT hooks, redaction,
length limits and newline handling. The Automation header owns the declarations
and explicit disabled-feature stubs; the old logging globals and Utils writer
declaration/implementation are removed.

Start/stop/append hold the existing filesystem guard throughout their operation,
so replacing a session cannot change an in-flight write's identity or destination.
A private state mutex only protects publication and status copies; it is released
before file access. Status therefore remains available during a blocked write.
The active hint is atomic, and append rechecks it after acquiring the filesystem
lock. Existing callers that already hold the filesystem lock retain its normal
reentrant guard behavior.

Captured transport, user, path, IP and scope are preserved, including live user
role checks at every guarded VFS operation. Request pointers and session tokens
are discarded from the durable context. Failed owning-string copies, including
scope copies, fail closed. The existing overflow resolver still chooses the
destination; its output buffer now fits the full path plus an overflow prefix.

Intentional behavior fixes:

- A new session is published only after its complete LOG_START entry succeeds.
  A failed replacement leaves the previous session active.
- Appends follow the existing transcript writer's checked pattern: exact byte
  count, flush, write-error flag and expected final size. A failed append may
  leave a partial line; it does not truncate or replace previous log data.
- Stop still deactivates if LOG_STOP writing fails, but returns an error that
  explicitly says the final marker was not written. Missing filesystem lock
  ownership leaves logging unchanged and reports that failure separately.

Verification:

- Seven registered suites passed: `automation_log`,
  `automation_scheduler_contract`, `command_header_boundary`, `command_catalogue`,
  `command_resolution_contract`, `command_ingress_guards` and
  `checked_text_writes`. The separate STT privacy suite also passed.
- The new suite executes the production owner, command parser, filesystem guard,
  autolog command, output redactor and executor hooks under ASan/UBSan and
  ThreadSanitizer. It covers allocation/copy failures, permission revocation,
  partial/zero/error writes, flush/size failures, directories, missing storage,
  long overflow paths, failed replacement and accurate stop responses.
- Controlled overlapping append/replacement/stop operations and threaded stress
  preserve identity/destination pairs. Status responds during blocked file I/O;
  nested filesystem guards and unavailable-lock behavior are covered.
- The actual automation-disabled public header compiles and links without
  authentication, VFS or RTOS implementations.
- QT Py and XIAO saved profiles built and passed compiler-based feature checks.
  A temporary QT Py profile with `ENABLE_AUTOMATION=0` also built; its compile
  database excludes `System_Automation.cpp` and its public API links through
  explicit stubs. The temporary profile was archived privately and removed
  from the saved configuration directory after verification.

| Profile | Automation | Image bytes | App headroom bytes |
|---|---|---:|---:|
| `qtpy-maps-test` | Enabled | 3,706,528 | 2,277,728 |
| `xiao-default` | Enabled | 5,882,128 | 102,128 |
| Temporary QT Py disabled variant | Disabled | 3,620,304 | 2,363,952 |

These are host logic checks with controlled storage and permission boundaries.
They do not qualify physical flash durability or SD overflow hardware. Scheduler,
trigger and condition-language behavior, board defaults and memory slimming are
outside this package.

No device was flashed. Build hashes, effective choices and test logs are in the
private automation logging evidence archive. The per-build
`automation-log-test-status.json` identifies these unflashed images separately
from earlier hardware qualification.

## Completed: automation scheduler ownership (2026-10-10)

The main loop previously assembled the scheduler's dirty, due, event and safety
checks from exported state. Command tasks could stop the scheduler and free its
cache while the main task scanned or rebuilt it. A rebuild could also hide a
concurrent invalidation. The shared file-save hook requested refresh only when
duplicate-ID repair changed the uploaded rules.

`System_Automation` now owns that coordination. The main task calls
`automationTick()` while the existing enabled setting permits execution. Boot
and RamFlush query `automationSchedulerRunning()`; start, stop and notification
remain the other integration points. Due checks and full ticks are private.
Unused memo arrays and their exported declarations are removed. Filesystem and
web consumers include the owner header instead of handwritten declarations,
including two declarations that incorrectly gave the atomic writer a void
return type.

A short private mutex protects cache allocation, scanning, publication and
release. File reads, JSON deserialization and rule execution run outside it.
Stop prevents new passes and frees the cache safely; an already claimed pass
may finish dispatching, but cannot recreate a stopped cache. Stop does not
cancel commands already queued. Pending refreshes and event hints are atomic;
the main task consumes refresh only before a pass, preserving changes that
arrive during its file read or publication. Unknown subscriptions wake
conservatively until the cache is rebuilt. The 250 ms event limit, 60-second
safety pass and existing condition backoff remain in the owner.

Intentional fixes: successful owner writes and every matching external file
save request refresh, even when duplicate repair makes no change. Failed owner
writes leave the previous schedule intact. Cache release and rebuild are
synchronized rather than relying on volatile state. Trigger syntax, condition
evaluation, authorization, file formats and boot preferences are unchanged.
Rule-store read/modify/write transactions remain outside this slice.

Combined verification for this slice and the recent saved Wi-Fi layout, Map
snapshots and automation logging work:

- A fresh host build and all **137 registered CTest suites passed**, with
  ASan/UBSan enabled and Node browser suites included. The initial pre-scheduler
  run also passed all 136 then-registered suites.
- Scheduler, automation logging and Map owner ThreadSanitizer runs passed;
  Maps covered GPS enabled/disabled. The separate STT privacy sanitizer suite
  passed as well.
- Scheduler behavior tests execute the production lifecycle, cache parser and
  polling decision, including missing/malformed files, allocation/write failure,
  due/backoff timing, event subscriptions, timer wrap, concurrent notifications,
  and stop/restart during an older blocked rebuild. The full rule executor is a
  host boundary double, exercised separately on the device.
- Negative controls confirmed the new tests detect both a lost refresh and an
  unlocked cache data race. The actual disabled header compiles and links
  without platform implementations.
- All four firmware configurations built at `0.99.97.1`; compiler-resolved
  flags and source selection matched their saved choices. The temporary
  automation-disabled profile was removed after checking its exact archived
  and generated configuration copies.

| Profile | Relevant selection | Image bytes | App headroom bytes |
|---|---|---:|---:|
| `qtpy-maps-test` | Maps/GPS/automation enabled | 3,706,640 | 2,277,616 |
| `xiao-default` | Maps/automation enabled, GPS disabled | 5,882,448 | 101,808 |
| `qtpy-logging-test` | GPS/automation enabled, Maps disabled | 3,493,280 | 2,490,976 |
| Temporary QT Py disabled variant | Automation disabled | 3,620,304 | 2,363,952 |

Build hashes, effective configuration checks, sanitizer output and the final
CTest report are retained in the private combined-cleanup evidence archive.
Memory slimming and SD qualification remain deferred.

The combined `qtpy-maps-test` application was then flashed without erasing
storage or replacing the partition table. **45 device checks passed**, including
delayed and event-triggered execution, queued command/output log entries, failed
log-session replacement, final stop markers and repeated scheduler stop/start.
A rule uploaded through `filewrite`, with no duplicate repair or subsequent
automation edit, responded to a setting-change event before the safety interval.
Map import/status, preservation after a failed load, live recording and shutdown,
saved Wi-Fi listing and a valid indoor GPS driver sample also passed.

Original automation rules were restored byte for byte; automation, GPS and
logger preferences were checked against their pre-flash values. Temporary files
were removed, both loggers stopped, the track cleared and GPS closed. OLED status
reported a connected display on the authenticated main menu. The final crash
record was empty and the serial port was released.

The other three images were built but not flashed. This device pass used serial
commands; it did not retest physical buttons, G2/R1, on-device HTTP routes or OLED
appearance. Indoor GPS had no satellite fix, so coordinate accuracy and large
track allocation pressure remain unqualified.

## Completed: shared runtime setting mutation (2026-10-10)

The generic settings handler and feature command previously assigned values and
requested persistence independently of the existing typed `setSetting` helper.
The generic handler also emitted its own setting events, saved unchanged values,
accepted integer prefixes such as `32junk`, wrapped narrow integer overflow,
and interpreted unknown boolean text as false. That made equivalent entry points
behave differently and required separate maintenance of persistence and events.

All three `setSetting` overloads now enter one completion function in
`System_Settings`. The generic handler and feature toggles use those overloads.
The owner requests persistence and publishes one setting event/repaint only
when the value changes. Event formatting and secret masking share one function.
Generic commands supply their descriptor to retain command-specific alias labels;
ordinary typed setters continue to use the first registered row for that field.
The notification buffer uses the existing event payload capacity.

Generic commands validate the entire input before mutation: decimal integers
must fit both their storage type and declared range, floats must be finite and
in range, and booleans use the existing `parseBoolArg` tokens. Invalid input and
read-only writes return an error without changing values, requesting a save or
publishing a change. Strings retain the entire trimmed argument and reserve
destination capacity before copying, preserving the old value on allocation
failure. Empty input remains a query, including masked secret queries. Metadata
options remain editor hints rather than a new validation rule.

Compatibility boundaries were checked before consolidation:

- Generic master-disable ordering remains value change, persistence request,
  service stop, then setting event. Repeated `off` still requests service stop
  without another save or setting event. Autostart preferences do not stop a
  service. Dedicated commands retain their existing runtime actions.
- Save failure leaves the accepted live value and existing retry intent intact.
  The result remains accepted so sensor-logging and UART wrappers apply that
  live value. Owner-scoped batches still coalesce only their own writes; unrelated
  sessions and background work do not inherit a batch.
- Feature toggles retain their compiled/toggleable checks and transition-only
  feature events, while adding the shared setting notification. Extra arguments
  and unterminated quoted toggle values are rejected. The listing's invalid enum
  sentinel, exposed by UBSan during this work, was replaced with an integer.
- Authorization and command routing are unchanged. Validate-only calls preserve
  their existing early `VALID` response without parsing or mutation.

This completes the ordinary mutation slice. Direct typed String callers retain
their previous assignment behavior; allocation checks were added to the generic
command path only. Wizard draft/apply/cancellation and broader service lifecycle
unification remain separate work. Build flags, saved configuration selection,
memory slimming and SD qualification are outside this package.

Verification:

- All **138 registered host CTest suites passed** in a fresh ASan/UBSan-enabled
  build, including the Node browser suites. The final settings suite was rerun
  after its last coverage additions and passed.
- The new settings suite executes the production generic handler, mutators,
  notification/completion functions, feature command, persistence adapter and
  batch core. Its **1,032 checks** cover OLED enabled/disabled, storage-width
  canaries, invalid/range input, aliases, secrets, no-ops, allocation failures,
  owner/background batching, retained save retry and exact disable ordering.
  Controlled registry, String, storage and service boundaries are host doubles.
- Updated sensor logging, selection, GPS startup and MQTT command suites passed
  with sanitizers; the persistence contract guard now checks the shared owner
  instead of counting the removed duplicate writes. Independent review found
  no blocking downstream issue.
- QT Py Maps and XIAO default firmware built at `0.99.97.1`, with saved choices
  confirmed by compiler-based checks. All 566 captured production/configuration
  inputs remained stable through the builds. The changed Settings and Feature
  Registry files emitted no compiler warnings.

| Profile | Image bytes | App headroom bytes |
|---|---:|---:|
| `qtpy-maps-test` | 3,707,568 | 2,276,688 |
| `xiao-default` | 5,884,016 | 100,240 |

Neither image was flashed. This milestone has host and build verification, not
new device qualification. The private settings-mutation evidence archive holds
test logs, build hashes and effective choices; each build directory contains
`settings-mutation-test-status.json` to distinguish it from previously flashed
firmware.

## Completed: shared wizard draft and completion (2026-10-10)

Wizard controls previously pointed into live settings. Accepted MQTT/ESP-NOW
pages and leaving the System page changed RAM before completion; cancellation
only skipped the final file writes. The blocking dialog also finalized after
cancellation, allowing timezone/name changes and HTTPS certificate generation.
That made Cancel incomplete and tied each input adapter to setting ownership.

`System_SetupWizard` now owns a bounded draft of wizard-editable choices, their
starting values and the existing registry bindings. Feature/network rows and
mode selectors bind to draft values. Visibility and the heap estimate read the
same draft. Serial, CLI mode and OLED adapters stage accepted page patches;
backing out of a partially entered page does not publish its scratch values.
Reopening a page uses the accepted draft. Reopening the wizard captures current
settings afresh. The whole `Settings` object is never copied or restored.

`wizardSaveAndApplySettings` is the only completion boundary. It checks completion
and cancellation, collects final system choices, validates them and reserves all
edited String destinations before publishing any value. It applies only fields
that differ from their starting draft, using the existing `setSetting` and debug
setting paths. Unedited settings changed through another interface survive.
The old live page-application helpers and separate finalizer are removed.

A short `SettingsWriteScope` groups persistence requests during Apply using the
existing batch core and flush/retry adapter. It is task-local, uses no allocation,
leaves existing command batches intact, and supports nested synchronous scopes.
It uses the previously unused FreeRTOS TLS slot 3; configurations with too few
slots refuse the scope before mutation. It is never held during human input.
After its flush, completion applies runtime settings, refreshes NTP when connected
and generates an HTTPS certificate if selected. A failed file save leaves the
accepted RAM state with pending retry and reports a warning; main/debug/Wi-Fi
storage is not an all-or-nothing transaction.

Cancel, timeout and CLI session loss discard draft and scratch credentials.
Blocking serial input accepts `cancel`, including inside mode and text pages;
mode pickers now honor timeout. Local display Back returns through the pages,
and Back on the first page cancels. Keyboard and Wi-Fi selection loops recognize
wizard cancellation and release their UI/scan state. Their extra cancellation
handling is gated to feature-wizard ownership, preserving account/Basic/Import
setup behavior. The CLI mode keeps its existing exact-session authorization and
its own inactivity policy. Its exit callback now revalidates that owner and the
idle deadline before accepting a completed draft. This closes the gap where a
session could disappear after the final input or while a physical Finish was
waiting for the executor. Accepted work may finish after a later disconnect.

Intentional adjacent fixes:

- The CLI device-name detour now respects the 19-character plus NUL buffer.
- Unhandled System-page buttons no longer count as Finish; only the shared
  completion state does.
- MQTT ports require a whole decimal in 1..65535; blank retains the draft port.
  Blank credentials retain existing values, matching their documented patch
  semantics. Skipping MQTT disables its draft auto-start in all adapters.
- A blank stationary choice retains its displayed value. Unchanged custom NTP,
  timezone and LED values are preserved rather than replaced by preset defaults.
- OLED System rows use the shared item mapping instead of separately deriving
  visibility from live settings. Feature toggle indexes are checked before use.

P4 reuse was traced through the board definitions, build rules, display HAL and
boot path. The SPI LCD already exposes the same 128x64 canvas, scaled onto its
240x240 panel. The build enables the shared display UI independently of I2C;
boot initializes that display and GPIO wheel/buttons before first-time setup.
The draft and adapters therefore cover P4 without a second wizard or startup
special case. Display layout redesign is separate from this milestone.

Basic/Import setup, archetype seeding, account creation, host build configuration,
board defaults, memory slimming and SD qualification remain outside this slice.
Input adapters retain their existing Arduino String allocation behavior; the
draft's checked copies and commit preflight do not claim to qualify every input
allocation or physical flash durability.

Verification:

- All **142 registered host suites passed** with ASan/UBSan enabled and the
  Node browser suites included: the bulk run passed 141/141, followed by the
  newly registered CLI completion-owner suite. No production source changed
  between those runs.
- The real draft/completion owner passed **321 checks** across minimal/full
  feature builds. Completion adapters passed **252 checks** across Wi-Fi
  enabled/disabled builds, including completed drafts refused after session
  loss. MQTT/ESP-NOW adapter tests verify staging without live writes.
- Display cancellation passed all four Wi-Fi on/off and GPIO/gamepad variants,
  executing the actual keyboard, Wi-Fi picker and button/tick paths. The full
  CLI mode implementation passed nine lifecycle scenarios covering serial and
  deferred display Finish, expired/replaced sessions, idle expiry and executor
  ownership. Rendering, input delivery and session/platform services remain
  controlled host boundaries.
- The real settings scope/batch adapter passed **192 checks**, including TLS
  refusal, nesting, other tasks, outer command ownership, save failures and
  retry. Its concurrent host cases also passed ThreadSanitizer.
- QT Py Maps, XIAO default and P4 board-default firmware all built at
  `0.99.97.1`, including the final session-loss guard. The P4 build used the
  existing SDK preparation script and a temporary saved profile with no feature
  overrides; no feature was removed to fit the image. All 588 captured production
  and configuration inputs remained unchanged across the final builds. Compiler
  checks confirmed P4 display type 4, input type 3, shared display UI and GPIO
  controls enabled, with I2C disabled. The temporary profile was archived and
  removed after matching it to the build snapshot.

| Profile | Image bytes | App headroom bytes |
|---|---:|---:|
| `qtpy-maps-test` | 3,716,128 | 2,268,128 |
| `xiao-default` | 5,895,472 | 88,784 |
| P4 X Eye board defaults | 6,502,064 | 923,984 |

No image was flashed. This milestone has host and build verification; physical
P4 first-time setup and display/input behavior remain unqualified. The private
wizard-draft evidence archive holds test/build logs, saved build inputs and
binary/source hashes; build-local status records distinguish these images from
previously flashed firmware.

## Completed: shared initial-admin creation (2026-10-10)

Basic/Advanced setup duplicated the initial-account JSON, password storage and
publication logic already used after a partial restore. The restore caller
checked failure, while the normal path logged failed writes and continued into
`SETUP_FINISHED`, `SETUP_NOT_NEEDED` and its success message.

The existing private `createInitialAdminUser` in `System_FirstTimeSetup` now
serves both callers and is available when migration support is disabled. The
normal path returns to its existing setup menu on failure, before automation
initialization, completion markers, final settings application, Wi-Fi connection
or reboot. Previously accepted feature settings remain in place on retry.

One implementation now owns the account's existing ID, owner role, creation
metadata, password/theme file and `nextId`. Empty credentials or a failed hash
are rejected. The user-index document must fit and serialize completely before
any file write. `saveUserSettings` retains responsibility for checked credential
storage; only its success permits atomic publication of `users.json`. A failed
publication leaves an orphan credential file that the next attempt can replace.
Boot still detects first-time setup from the absence of the user index.

NVS boot-counter synchronization and pending timestamp resolution remain after
successful publication. Account formats, hashing policy, theme choices and
restore behavior are preserved. No new public API, module or transaction layer
was added. Archetype seeding, earlier feature configuration and general user
creation/approval flows remain separate work.

Verification:

- **Nine focused CTest suites passed** with ASan/UBSan enabled: the new
  initial-admin suite plus existing wizard, display-cancel, CLI owner, user
  settings, Wi-Fi consumer, persistence and network-build checks.
- The new suite passed **2,318 checks** across migration and OLED enabled/disabled
  combinations. It executes the actual shared helper, full post-restore
  credential flow and normal setup's contiguous save-through-success tail,
  using vendored ArduinoJson. Hashing, allocation/String faults, file APIs,
  earlier input collection and reboot are controlled boundaries. It checks
  failure, retry, account fields/theme, publication order and skipped success
  effects; it does not simulate physical power loss.
- QT Py Maps, XIAO default and P4 board-default firmware built successfully at
  `0.99.97.1`. All 588 captured production/configuration inputs stayed unchanged.
  These linked profiles enable migration; three additional checks compiled the
  actual setup translation unit with migration explicitly disabled under each
  board's real toolchain. Those checks are standalone objects, not additional
  linked firmware images.

| Profile | Image bytes | App headroom bytes |
|---|---:|---:|
| `qtpy-maps-test` | 3,715,392 | 2,268,864 |
| `xiao-default` | 5,891,488 | 92,768 |
| P4 X Eye board defaults | 6,500,768 | 925,280 |

The temporary P4 profile was removed after comparison with its archived build
inputs. Independent source review found no additional issue. No image was
flashed and no device account was changed. The private initial-admin evidence
archive contains test/build logs, effective choices and source/binary hashes;
build-local status files identify these unflashed images.

## Completed: shared setup menu organization (2026-10-10)

The setup wizard previously put display controls on its Sensors page, sensor
startup flags on Network, and general system features beside connectivity.
Serial and display maintained separate row/action mappings; the I2C registry
entry was omitted, and the opening menu used different Import availability
rules in its two adapters.

`System_SetupWizard` now builds four groups from the existing feature registry:
Connections, Display & Controls, Sensors and System. Registry rows are sorted
by their displayed names; Wi-Fi auto-connect, ESP-NOW mesh, MQTT auto-start and
LED effect stay beside their parent feature. Only compiled features with a
mutable setting become controls. I2C and input are grouped with the local UI
without changing their global registry categories. Empty groups are skipped.

One row type, formatter and activation operation replace the separate feature,
network and system models. Both serial engines and the OLED/P4 adapter use
these rows and the shared page order. Values distinguish ordinary On/Off from
Boot on/off; Edge Impulse retains enablement semantics despite its sensor
category. Menu capacity failures invalidate the draft and report an error
instead of silently hiding settings. The existing heap estimate remains.

Connection detail pages now precede Display & Controls, Sensors and System.
Wi-Fi acceptance or Skip advances through the remaining pages, rather than
completing setup early. Back preserves an accepted network choice; explicit
Skip clears the pending credentials without deleting saved networks. First-page
Back cancels even when earlier groups are absent. Finish remains the existing
shared commit boundary, and adapters no longer override the core's completion
result.

Device Name is available under System even when ESP-NOW is compiled. Both entry
points share the accepted name draft, including Back, revisiting ESP-NOW and
reverting an edit. Compact timezone values retain the abbreviation and UTC
offset on the display. Scroll arrows have a reserved column, numeric row input
is validated before index arithmetic, Wi-Fi Back restores the menu prompt, and
shared group output uses the existing broadcast path for remote CLI clients.

`System_FirstTimeSetup` owns one Basic/Advanced/Import list and parser, used by
serial and display. Import requires migration and Wi-Fi support, independent of
the normal web interface. Blank input selects Basic; malformed or unavailable
choices remain at the chooser. Basic's description reflects its existing preset
selection. Build flags, saved configuration formats and feature dependencies
are unchanged; the existing editable build header and host setup tool retain
their roles.

Verification:

- **All 146 registered host suites passed**, with ASan/UBSan and the Node-based
  checks enabled. The strengthened menu matrix and draft suite also passed
  final focused reruns. The draft suite's **475 checks** include device-name
  Back/revisit, reverting edits, concurrent updates and untouched long names.
- The production registry, build header and wizard model passed **3,619 checks**
  across QT Py Maps, XIAO defaults, P4 defaults, XIAO with Edge Impulse/MQTT and
  a minimal configuration. Coverage includes every compiled mutable registry
  entry exactly once, grouping/order, adjacent companion rows, startup semantics,
  bound draft actions, empty groups, menu output, malformed/extreme CLI input
  and refusal to finish an invalid draft.
- The shared chooser passed **434 checks** across all eight combinations of
  migration, Wi-Fi and regular HTTP support. Wi-Fi navigation passed **128
  checks** across the serial, CLI and display adapters, covering acceptance,
  Skip, Back, rescan, expired scan selections and continued page navigation.
  Existing draft, cancellation, MQTT/ESP-NOW and initial-account checks passed.
- QT Py Maps, XIAO default and P4 board-default firmware built at `0.99.97.1`.
  All **589 captured production/configuration inputs** remained unchanged
  during the builds. Effective selections and linked images were checked;
  the temporary P4 profile was removed after matching its build snapshot.

| Profile | Image bytes | App headroom bytes |
|---|---:|---:|
| `qtpy-maps-test` | 3,714,608 | 2,269,648 |
| `xiao-default` | 5,894,528 | 89,728 |
| P4 X Eye board defaults | 6,499,920 | 926,128 |

The tests execute production menu and input code with controlled hardware and
storage boundaries. No firmware was flashed. Physical display readability,
button input and first-boot hardware behavior remain unqualified for this menu
change. The private setup-menu evidence archive contains host/build logs,
source and binary hashes, resolved choices and unflashed build status records.

## Checkpoint: 0.99.97.2 (2026-10-10)

This checkpoint includes the follow-on cleanup through shared setup menu
organization. The production source matches the verified menu milestone;
release preparation changes the version and release notes. The 146-suite host
run and final focused checks remain its regression baseline. All 52 OTA tests
passed, and release rebuilds for QT Py Maps, XIAO default and P4 board defaults
embed `0.99.97.2` with the same feature selections and image sizes listed above.
All 5,526 captured production/configuration/tool inputs stayed unchanged during
those builds. The temporary P4 profile was removed after snapshot comparison.
The release-scope privacy review found no actionable personal-information or
credential findings. No firmware was flashed. See the
[changelog](../CHANGELOG.md#099972---2026-10-10) for the release summary.
