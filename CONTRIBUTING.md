# Developing Hardware One

Keep boards general-purpose and features independently selectable. Choose a
small change with a visible result, reuse the existing path, and remove an old
dependency or duplicate when replacing it. Apply these rules to code being
changed; unrelated subsystems do not need a simultaneous rewrite.

## The 0.99.97.x cleanup series

The focus of `0.99.97.x` is long-term readability, editability and consistent
module design. Each milestone should make an existing feature easier to find,
understand and change. Useful outcomes include one owner for shared behavior,
fewer dependencies between unrelated systems, simpler control flow, clearer
names and interfaces, and related code grouped in predictable places.

Before editing, inspect the current implementation and its downstream callers,
including existing examples that already solve the problem well. Define one
bounded area, the concrete maintenance problem and a stopping condition. Reuse
the project's existing mechanisms; a new abstraction must reduce the work a
reader or caller needs to do. Remove replaced paths once callers have migrated.

Preserve supported behavior and configuration during structural changes.
Identify intentional bug fixes or behavior changes separately and verify their
downstream effects. Keep file relocation separate from behavior changes where
practical so reviewers can follow both. Use the verification guidance below.

Complete and record each milestone before expanding its scope. Judge progress
by how easily someone can trace and edit a feature, rather than lines removed
or files split. Memory slimming and SD qualification remain deferred until
explicitly resumed.

## Give each decision one owner

| Decision | Owner |
|---|---|
| Chip, flash/PSRAM geometry, wiring, attached hardware, bus ownership | Board/assembly definitions in `boards/` and the existing board sections/headers |
| Ordinary feature defaults | Shared editable section of `System_BuildConfig.h`, then `boards/<board>.features.h` |
| Optional SDK policy | `presets/`; saved SDK overrides use its allowed choices |
| A user's build choices | `configurations/<name>.json`, managed by `tools/build_config.py` |
| Feature prerequisites and conflicts | Derived rules in `System_BuildConfig.h`; SDK prerequisites resolve through Kconfig |
| Release identity, partitions, signing and provisioning | Existing deployment/OTA contracts |
| Runtime feature availability and toggles | `System_FeatureRegistry`, using compiled predicates and persisted settings |
| Ordinary runtime setting mutation, persistence requests and change events | `setSetting` in `System_Settings`; generic commands validate their `SettingEntry` before using it. Feature toggles use the same path; service owners retain their live actions. |
| On-device wizard drafts, page choices and completion | `System_SetupWizard`; serial and local-display adapters edit its draft. `wizardSaveAndApplySettings` is the sole commit boundary; cancellation discards the draft. |
| Initial admin creation | `System_FirstTimeSetup`'s shared `createInitialAdminUser`; Basic/Advanced and post-restore setup require success before completion. Store credentials before publishing `users.json`, which boot uses to detect completed setup. |
| Command module grouping, help order and availability callbacks | `System_CommandRegistry.cpp`; command tables stay with their owning modules |
| Clock state, timezone and shared clock-step duties | `System_Clock`; network synchronization and reply tracking use `System_NTP` |
| Saved Wi-Fi entries, lifetime and consistent reads | [`network/wifi`](components/hardwareone/network/wifi/README.md) (`System_WiFiNetworks`); screens use credential-free snapshots and boot-local entry IDs; Settings retains crypto and file-write policy |
| Sensor readings, cache synchronization and sensor-envelope serialization | The sensor module; consumers use its snapshot/JSON functions and keep presentation outside the cache lock. Thermal frames are copied into caller-owned buffers. |
| GPS track lifetime, recording transitions and consistent reads | `GPSTrackManager` in `System_Maps`; consumers use value status or owned point snapshots. Rendering, HTTP and file writes run after the track state lock is released. |
| Automation log session, captured writer identity and checked appends | `System_Automation`; command/event callers use its active hint and append operation. The existing filesystem guard serializes writes and session changes; status uses a short private state lock. |
| Automation scheduling cadence, pending changes and due-cache lifetime | `System_Automation`; the main task calls `automationTick()`, while other tasks use start/stop/notify and the atomic runtime-state query. Stop prevents new passes; an already claimed pass may finish. |
| I2C sensor start/stop request ordering | The existing I2C queue worker; sensor owners initialize, roll back failures and release their resources. A stop request cancels pending starts; cleanup completion permits restarting. |
| Shared physical input readings and latched button presses | `HAL_Input`; backends publish through its internal producer API. Screens peek at snapshots; dispatch consumes presses atomically. Driver-specific detents remain with the encoder backend. |
| Display hardware initialization, readiness, brightness, power and sleep/wake | `HAL_Display`; UI code owns boot policy, screen drawing and authentication. A retained canvas does not imply that the display is ready. |
| Embedded icon availability and file-extension mapping; icon drawing | `System_Icons` owns lookup policy beside the generated assets; `OLED_UI` owns rendering on the shared graphics canvas. |
| CPU clock changes and I2C draining during a change | `System_Power`; commands and idle/wake choose frequencies through its existing policy functions and check the operation's result. |
| NeoPixel colors, effects, brightness, startup settings and driver access | `System_NeoPixel`; screens use the existing authorized commands, and the driver instance stays private. |
| Directory entry order, backend name translation and virtual mounts | `VFS::DirectoryReader` in `System_VFS`; callers retain guarded opens, filesystem lock ownership, visibility, permission metadata, caching and output formats. |
| Storage-path routing, capacity sampling and sample validity | `System_VFS`; consumers use `getStorageType` and check capacity-query results. Admission uses fresh `getStats`; presentation may explicitly opt into bounded-age snapshots. |
| A service's behavior, resources and lifecycle | Its owning module, exposed through a small public interface |

A board default is a starting choice. A restriction needs a concrete reason:
missing wiring, unsupported backend, shared-resource conflict, or measured
capacity. Keep optional applications out of hardware identity checks. A missing
attachment should report unavailable without breaking unrelated services.

## Keep interfaces small

- Put hardware-specific work behind the existing HAL/backend interface. Shared
  services should ask for an operation or capability instead of checking a board
  name. New abstraction layers need an actual caller and a repeated concern.
- I2C discovery uses the manager's address probes, locking and clock handling.
  A scan result is an address match; the driver owns protocol initialization
  and identity checks. Do not read arbitrary device data to confirm presence.
- Keep a module's state and resource lifetime with its implementation. Use its
  existing settings/command registries for integration; avoid exporting mutable
  globals merely to make another module work.
- Include what a file uses. Public headers expose their own types and operations;
  implementation-only SDK/driver includes belong in `.cpp` files. Command table
  declarations use `System_CommandRegistry.h`; command parsing uses
  `System_Command.h`. Broad utility includes are compatibility paths to reduce
  incrementally, rather than the default for new code.
- UI and transport code collect input and present results. Reuse service methods
  and the existing authorized command executor for command requests. Preserve
  source/session identity and permission checks across every entry point.
- Ordinary setting updates use `setSetting` so unchanged values do not request
  another save or change event. Generic scalar commands use the registered type
  and range; dedicated service commands keep their runtime actions. A failed
  persistence request retains the accepted live value and existing save retry;
  it is not a rollback of the setting or its runtime effects.
- Treat display, physical input, networking and storage as separate consumers.
  Gate the driver that is absent, not shared state needed by another consumer.
  Disabled-feature APIs keep compatible signatures and report unavailable;
  they must not pretend an operation succeeded.
- Do not expose a sensor mutex or borrowed cache buffer to a screen or transport.
  A reading snapshot does not consume events; use the owner's explicit consume
  operation for latched input. Keep reading validity separate from lock success.
- Keep I2C transaction results separate from sample readiness and cache
  publication. Check readiness once per poll, release the bus before waiting
  for a cache lock, and do not count cancellation or cache contention as a
  hardware fault. Preserve errors returned by actual driver operations.
- Document ownership and asynchronous outcomes where callers need them. For
  example, “start accepted” and “connected” are different states, as are
  “stop requested” and “resources released.”
- Validate command arguments before initializing or writing hardware. Report
  success and publish related events only after the operation runs; check a
  driver's result when available. A void driver call establishes that a
  request was issued, not that the hardware accepted it.
- Atomic file saves must check the complete temporary write before promotion.
  Keep the filesystem lock through writing, renaming and failed-temp cleanup;
  a failed replacement must not delete or truncate the previous destination.
  Reuse the existing text-write helpers for their trusted persistence callers.
- Use scope-owned buffers and existing lock/polling guards for request resources.
  Give streaming writes one completion/cleanup path before success hooks; close
  and clean up failed partial files while the filesystem lock is still held.

## Keep the setup tool thin

The host configuration tool chooses what enters the firmware. The on-device
wizard configures features already compiled into it. They serve different
stages, while each reuses its existing shared model and validation.

Host CLI commands and guided setup must call the same discovery, editing,
validation and save functions. Store explicit choices; removing an override
restores inheritance. Support previewing changes, keep edits atomic, and
leave builds in their own directories. Report actual compiled results through
the compiler/Kconfig path and actual size through the linked image. Do not add
another dependency table or silently drop features to make an image fit.

On-device wizard pages read and edit the shared wizard draft, including page
visibility and estimates. Keep live setting writes, persistence and runtime
effects in the completion owner. Apply only values edited from the starting
draft so unrelated settings changed elsewhere survive. Use `SettingsWriteScope`
for the short synchronous commit; never hold a persistence scope while waiting
for user input. A failed save retains accepted live values and retry intent,
and must be reported. It is not a transaction across settings files.

Keep the wizard's Connections, Display & Controls, Sensors and System groups
in its shared menu model. Serial and display adapters use the same rows,
labels, actions and navigation. Feature rows come from the runtime registry;
distinguish feature enablement from startup behavior without changing the
registry's categories just to arrange a screen. First-time setup's
Basic/Advanced/Import choices likewise share one list and availability rule.

Preserve `System_BuildConfig.h` as the human-editable entry point for shared
build flags. Saved configurations and guided setup are additional supported
entry points; generated inputs belong in the build directory.

Core HTTP/web and migration sources and their HTTP(S) SDK dependencies stay in
the build graph; their existing C++ guards use the rules in
`System_BuildConfig.h`. This keeps restore-only builds independent of the regular
web UI. Application filters such as MQTT, Maps and LLM still remain in CMake.
Do not add a second network/web resolver to CMake or the setup tool.

Other source-selection rules still exist in CMake as well as C++. Until those
are consolidated, preserve their agreement and cover both with a configuration
test. IDF discovers component requirements before its final SDK configuration;
a replacement must preserve that ordering. Avoid extending this duplication
into the setup UI.

## When adding or changing a feature

1. Identify its owner and existing interfaces. Start with one module and the
   consumers that need it; split by responsibility rather than by file length.
2. Add or reuse the editable choice and its prerequisites in their existing
   owners. Board files change only for relevant hardware or default choices.
   Register commands/settings/runtime visibility through the existing registries.
3. Verify enabled and disabled behavior, required dependencies, and the affected
   combination. Exercise the production implementation or header; avoid tests
   that reproduce its logic in a second implementation.
4. For firmware/configuration-rule changes, build the affected saved profile and
   a relevant baseline; check resolved choices, linking and image headroom.
   Host-tool-only changes normally need CLI, persistence and validation tests.
5. Record what changed and what was verified. Distinguish a successful build
   from physical hardware qualification. Remove superseded helpers/rules when
   their consumers have migrated.

Use the [saved configuration workflow](configurations/README.md),
[host test instructions](components/hardwareone/test/host/README.md), and
[configuration ownership audit](docs/CONFIGURATION_OWNERSHIP.md) for details.
The [code simplification plan](docs/MODULARIZATION_PLAN.md) describes the next
incremental changes and their completion criteria.
Success means fewer places to change a feature, smaller include dependencies,
and supported selections that work independently.
