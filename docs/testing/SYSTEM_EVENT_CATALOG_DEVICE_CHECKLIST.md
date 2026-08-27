# System Event Catalog device checklist

This checklist records physical-device and fault-injection evidence for the
System Event Catalog implementation plan. It is not evidence by itself. Every
formal Phase 0, Phase 2, and Phase 3 physical/fault row below starts **PENDING**
and remains pending until the production change and, where named, the
default-off UART-only device harness and external client exist.

Host tests and source inspection may support a row, but they never turn an
OLED, UART, web, executor, filesystem, or concurrency row into a device pass.

## Automated implementation checkpoint — 2026-08-26

The Phase 0 safety work, Phase 1 typed-provider extraction, Phase 2 shared
JSON/text adapters, and Phase 3 direct-indexed OLED consumer changes are
present. The current repository-local host suite passes 18/18 registered
native/Python tests, with sanitizers enabled on the native targets. It covers
the real provider, serializer and text core, exact typed-to-JSON fixture parity,
escaping, sizing, sink failure, checked text packing, source ownership
boundaries, and source contracts for both OLED consumers. The compact
production catalog is exactly 2,877 bytes, or 2,878 bytes with the command
buffer's trailing NUL, and contains 12 families / 152 canonical kinds.

All five **Phase 1** provider-only board/gate matrix profiles passed. The full
default **Phase 2** matrix now independently passes the same five profiles with
both provider and JSON-adapter ownership required, then passes ordinary
FeatherS3 and XIAO-S3 recovery builds and restores `System_BuildConfig.h`
bytes, mode, and nanosecond mtime exactly. These results establish only the
named host, build, object, and regression evidence.

An authenticated live-browser acceptance against the final XIAO image on
2026-08-26 exercised the catalog-backed event picker while the Automation
subsystem remained disabled. Two fresh page loads produced the same ordered 12
groups and 152 unique canonical values, from `peer_online` through
`display_init_failed`. The reserved command/alias tokens `boot`, `none`, `set`,
`patch`, `all`, and `list` and all numeric-only values were absent;
`ota_rolled_back`, `ota_recovery_entered`, and `automation_action_dropped` were
present. Selecting `display_init_failed` survived an Event -> Interval -> Event
transition, the page reported no warnings or errors, and the test reloaded the
page to its default state without enabling the subsystem or saving or mutating
an automation. Browser control did not expose the raw response headers or exact
2,877-byte body, so this remains acceptance evidence rather than a formal
`P2-HTTP-JSON` pass. The generic Secure-BLE catalog client exists and its pinned
offline lane passes; its physical run and the default-off device harness remain
outstanding, and every formal physical/fault case below remains `PENDING`.

## Phase 1 provider memory and link evidence — 2026-08-25

These are automated build/object records, not substitutes for OLED, G2, BLE,
UART, HTTP fault-injection, or disposable-admin device acceptance. An
`ESTABLISHED (automated)` row means only the stated repository-local evidence
was observed.

| Evidence ID | State | Established result |
|---|---|---|
| `P1-PROVIDER-HOST` | `ESTABLISHED (automated)` | 15/15 sanitizer-backed host/structure tests pass, including exact 12-family/152-kind traversal, immutable concurrent reads, zero provider allocations, legacy fallbacks, and source ownership. |
| `P1-XIAO-BUILD` | `ESTABLISHED (automated)` | The ordinary `xiao_s3` profile builds with unconditional `System_EventCatalog.cpp` integration. |
| `P1-XIAO-OBJECT` | `ESTABLISHED (automated)` | The new provider object has `.data = 0` and `.bss = 0`. Its immutable family-label pointers, kind-name pointers, kind-family table, and family-order index are 48 + 608 + 152 + 177 bytes. |
| `P1-XIAO-DELTA` | `ESTABLISHED (automated)` | Same-toolchain pre-extraction tables were 48 + 612 + 153 bytes. Omitting the `none` slots and linking the 177-byte index gives a net +172 bytes of immutable metadata. |
| `P1-XIAO-LINK-GC` | `ESTABLISHED (automated)` | Current Phase 1 compatibility callers retain existing behavior; link section garbage collection removes unreferenced typed traversal functions. No second runtime descriptor table, mutable cache, `.data`, or `.bss` owner was found in the provider object. |
| `P1-PROVIDER-MATRIX` | `ESTABLISHED (automated)` | All five full-clean profiles passed their resolved-gate, compile ownership, link-map, binary, manifest, and provider single-owner checks: ordinary FeatherS3; FeatherS3 with G2 on and Automation off; ordinary XIAO-S3; ordinary classic ESP32; and XIAO-S3 with optional consumers off. Cleanup rebuilt ordinary FeatherS3 in the driver; after its first XIAO-S3 cleanup configure failed before compiler selection, a clean ordinary XIAO-S3 retry passed. The source header's original bytes, mode, and modification time were restored. |

Phase 1 changes catalog ownership and adds typed native traversal only. It does
not migrate or change JSON output, OLED pickers, or G2 behavior; those surfaces
remain assigned to later phases.

## Phase 2 adapter automated evidence — 2026-08-25

These rows do not substitute for a physical UART/BLE client, a live HTTP
failure injection, or the restoring five-profile artifact matrix.

| Evidence ID | State | Established result |
|---|---|---|
| `P2-ADAPTER-HOST` | `ESTABLISHED (automated)` | The complete host suite passes 18/18, with sanitizers enabled on native targets, including the real JSON adapter, provider-shaped JSON/text cores, hostile escaping/sizing/sink fixtures, independent typed/JSON dump comparison, and structure guards. |
| `P2-V1-PAYLOAD` | `ESTABLISHED (automated)` | The production serializer emits the reviewed compact v1 fixture byte-for-byte: 2,877 UTF-8 bytes, 12 families, and 152 unique canonical kinds. Required command capacity is 2,878 bytes including NUL, within `CMD_RESULT_MAX = 4096`. |
| `P2-XIAO-BUILD` | `ESTABLISHED (automated)` | The ordinary `xiao_s3` firmware builds with `System_EventCatalog.cpp` and `System_EventCatalogJson.cpp` in the unconditional component source list and with CLI/HTTP consumers migrated. |
| `P2-ADAPTER-MATRIX` | `ESTABLISHED (automated)` | The default `tools/build_event_catalog_coverage.sh` mode passes all five board/gate rows with provider and JSON-adapter object/symbol ownership, then passes ordinary FeatherS3/XIAO recovery and exact config restoration. |
| `P2-BLE-OFFLINE` | `ESTABLISHED (automated)` | The hash-pinned wrapper passes 24 Secure Channel/protocol tests and validates the offline catalog as 12 families, 152 unique kinds, and 2,877 compact JSON bytes without scanning or connecting. |

The implemented command adapter writes the exact JSON into the 4,096-byte
result buffer and fails explicitly if it no longer fits. The implemented HTTP
endpoint preflights the same serializer, streams through
`httpd_resp_send_chunk()`, and aborts after an attempted sink failure rather
than appending an error tail. Those are source/host-verified implementation
claims; the live command, HTTP-fault, and BLE procedures below remain pending.

## Phase 3 local OLED adapter automated evidence — 2026-08-26

The automation wizard and notification editor now use the typed provider's
indexed family/kind traversal directly. Their fixed 24-entry projections and
build scans are absent, the automation wizard stores provider ordinals and
re-resolves before command submission, and notification actions submit the full
resolved canonical name. These are source/host facts, not observations from a
display.

| Evidence ID | State | Established result |
|---|---|---|
| `P3-OLED-SOURCE` | `ESTABLISHED (automated)` | Registered source guards require `systemEventCatalogFamilyCount()`, `systemEventCatalogFamilyAt()`, and `systemEventCatalogFamilyKindAt()` in both OLED consumers; reject both old 24-entry caches/builders and the old scalar scans; and require provider-ordinal automation selection with re-resolution at submission. |
| `P3-OLED-HOST` | `ESTABLISHED (automated)` | The complete sanitizer-backed host suite passes 18/18. The real provider test traverses every production family row, rejects invalid indices without modifying output storage, and covers a non-contiguous synthetic family with 35 kinds. This does not execute OLED rendering or button input. |
| `P3-FEATHER-NONOLED` | `ESTABLISHED (automated)` | The ordinary FeatherS3 build after these edits succeeded, but its current `DISPLAY_TYPE=0` configuration excluded `OLED_Mode_Automations.cpp` and compiled only the display-disabled side of `OLED_Utils.cpp`. This establishes only that the non-OLED profile still builds. |
| `P3-OLED-BUILD` | `ESTABLISHED (automated)` | A temporary FeatherS3 compile-coverage profile asserted custom OLED/gamepad enabled, `DISPLAY_TYPE=1`, `INPUT_DEVICE_TYPE=1`, and Automation enabled, then completed with zero compile errors. Both active picker objects were newer than their source; archive inspection showed each object referring once to all three native indexed provider operations and no catalog-JSON symbol. The coverage config was restored byte-identically. This image is diagnostic and must not be flashed. |
| `P3-OLED-PHYSICAL` | `PENDING` | No real OLED navigation, high-kind mutation, invalid-ordinal injection, persistence/reboot, or display-auth procedure has been completed. The four `P0-OLED-*` rows below remain the required physical boundary evidence despite retaining their Phase 0 safety identifiers. |

The authenticated live-browser acceptance recorded above remains a passed web
UI observation: it reached all 12 families and 152 kinds without mutating an
automation. It did not exercise either OLED consumer, expose raw response
headers/body bytes, or satisfy the formal `P2-HTTP-JSON` row.

## Safety and evidence rules

- Use a FeatherS3 test device and a disposable named admin identity. Never use
  an operator's normal notification preferences or credentials.
- The future harness must be compiled only with
  `HW1_EVENT_CATALOG_DEVICE_TEST`, must expose only the UART command
  `eventcatalogtest <case-id> <case> [args...]`, and must reject `AuthBypass`,
  anonymous, non-admin, and non-UART callers.
- One-shot fault hooks must auto-disarm on consumption, timeout, case end, or
  disconnect. End every run by querying the harness for armed hooks.
- Store scrubbed evidence under the repo-relative directory
  `output/event_catalog_device/<run-id>/`. Do not commit passwords, session
  cookies, device identifiers, raw user-settings files, private keys, radio
  captures, or unsanitized logs.
- Event numeric IDs are build-internal. The IDs below identify the frozen
  Phase 0 fixture; verification and persistence use canonical names.
- A short-write result proves only the observable serialized count and file
  size. It does not claim that the current void `flush()` reports media
  durability.
- After a test-firmware run, restore an ordinary full-clean build and verify
  that the harness command and hook symbols are absent.

## Status legend

| Status | Meaning |
|---|---|
| `ESTABLISHED (automated)` | The named source/host/build fact was observed; no physical or fault-injection pass is implied |
| `PENDING` | Implementation, harness, device run, or evidence is not complete |
| `BLOCKED` | A named external prerequisite failed; record it without claiming a pass |
| `PASS` | Expected device-visible result and required evidence were both reviewed |
| `FAIL` | The procedure ran and contradicted at least one expected result |

## Run record

Copy this table for each physical run. Keep every path repo-relative and every
identity disposable.

| Field | Value |
|---|---|
| Run ID | `PENDING` |
| Date/time and timezone | `PENDING` |
| Operator/reviewer | `PENDING` |
| Firmware revision | `PENDING` |
| Dirty-tree description or patch ID | `PENDING` |
| Board and serial label | `feathers3` / `PENDING-NONSENSITIVE-LABEL` |
| Build configuration | `PENDING` |
| Harness option and link-map absence/presence evidence | `PENDING` |
| Disposable username | `PENDING-NONSENSITIVE-NAME` |
| Evidence directory | `output/event_catalog_device/<run-id>/` |
| Ordinary firmware restored | `PENDING` |
| Leftover hooks | `PENDING` (must be `none`) |

The planned harness build entry point is
`tools/build_event_catalog_device_test.sh`; it does not exist at this baseline
stage. Until it lands, every harness-dependent row below remains `PENDING`.
The ordinary restoration commands are:

```sh
tools/build_board.sh feathers3 fullclean
tools/build_board.sh feathers3 build
```

## Phase 0 case index

| Case ID | Surface | Initial status | Harness prerequisite |
|---|---|---|---|
| `P0-OLED-127` | Real OLED personal-mute picker | `PENDING` | OLED-enabled build, flash, and physical run |
| `P0-OLED-128` | Real OLED first high-word kind | `PENDING` | OLED-enabled build, flash, and physical run |
| `P0-OLED-152` | Real OLED final fixture kind | `PENDING` | OLED-enabled build, flash, and physical run |
| `P0-OLED-PRESERVE` | Low toggle preserving high mutes | `PENDING` | OLED-enabled build, disposable user, and physical run |
| `P0-CMD-UART-SYNC` | UART whole-line and executor boundary | `PENDING` | One-shot executor probe |
| `P0-CMD-WEB-SYNC` | Authenticated web synchronous boundary | `PENDING` | UART-armed executor probe |
| `P0-CMD-ASYNC` | Direct asynchronous submission boundary | `PENDING` | `async_input_boundary` harness case |
| `P0-CMD-BLE-RAW` | Plain BLE record boundary | `PENDING` | Companion raw-write runner + executor probe |
| `P0-CMD-BLE-PREPARED` | BLE prepared-write rejection | `PENDING` | Companion ATT prepare/execute runner |
| `P0-CMD-BLE-SECURE` | Secure-frame classification boundary | `PENDING` | Companion Secure Channel runner |
| `P0-SETTINGS-RMW` | Serialized disjoint settings mutations | `PENDING` | `settings_rmw_barrier` harness case |
| `P0-SETTINGS-PRECOMMIT` | Temp open/zero-write failure | `PENDING` | `settings_precommit_fail` harness case |
| `P0-SETTINGS-TEMP-SHORT` | Nonzero short temporary write | `PENDING` | `settings_temp_short_write` harness case |
| `P0-SETTINGS-DEST-TOUCHED` | Direct fallback touched destination | `PENDING` | `settings_destination_touched_fail` harness case |
| `P0-SETTINGS-DEST-SHORT` | Nonzero short direct fallback write | `PENDING` | `settings_destination_short_write` harness case |
| `P0-SETTINGS-CACHE-RACE` | Stale cache-fill publication race | `PENDING` | `settings_cache_fill_race` harness case |

## OLED boundary kinds

### `P0-OLED-127` — `ota_rolled_back`

**Purpose:** exercise the final bit before the old four-word OLED boundary.

1. Log into the physical OLED as the disposable user and enter Notifications
   → configuration → My mutes → Firmware & OTA.
2. Navigate to canonical kind `ota_rolled_back` and record the displayed row.
3. Toggle it on. Through authenticated UART, show the same user's mute list
   and confirm the full canonical name appears exactly once.
4. Leave and re-enter the OLED screen, then reboot and log in again. Confirm
   the checkbox remains selected.
5. Toggle it off and confirm the name is absent.

Expected: no crash or reset; no neighbouring bit changes; display truncation,
if any, never changes the persisted canonical name. Capture before/after mute
lists, OLED photos without personal data, reset reason, and the command result.

### `P0-OLED-128` — `ota_recovery_entered`

**Purpose:** exercise the first bit that previously indexed beyond
`uint32_t mask[4]` in the OLED editor.

Repeat `P0-OLED-127` for `ota_recovery_entered`. Also capture the device's
reset reason and stack/high-water diagnostics immediately after the toggle.

Expected: the kind renders and toggles normally, survives screen rebuild and
reboot, and does not corrupt the OLED task, viewer mask, or adjacent kind 127.

### `P0-OLED-152` — `automation_action_dropped`

**Purpose:** exercise the final canonical kind in the Phase 0 fixture.

Repeat `P0-OLED-127` in the Automation family for
`automation_action_dropped`. Verify it is the last fixture option, its full
canonical name is saved, and an out-of-range next navigation action clamps or
wraps according to the OLED screen's existing policy without issuing a save.

Expected: the final kind is reachable and persistent; no local 24-row or
128-bit limit hides or aliases it.

### `P0-OLED-PRESERVE` — low toggle retains high bits

1. Using bounded one-kind commands as the disposable user, mute
   `ota_recovery_entered` (128) and `automation_action_dropped` (152).
2. Confirm both names through the command interface and the OLED checkboxes.
3. Through the real OLED picker, toggle low kind `peer_online` (1) on, then
   off.
4. Re-resolve the viewer after each successful command and after reboot.
5. Confirm kinds 128 and 152 remained selected throughout. Clear both as
   cleanup.

Expected: the OLED submits only the selected one-kind mutation; no generated
whole-catalog command appears in logs; both high kinds remain byte-for-byte
canonical and selected after the low toggle and reboot.

## Command ingress boundaries

Every length below counts command bytes only, excluding a UART newline, JSON
request framing, and the trailing NUL in `ExecReq::line`. Generate payloads in
the runner; do not hand-copy them. Use a unique, harmless unknown-command
prefix and a one-shot executor probe so admission is distinguishable from
transport rejection without performing a real action.

### `P0-CMD-UART-SYNC`

1. Over the authenticated physical test UART, arm the one-shot executor probe.
2. Send a complete 2,047-byte benign line plus the normal line terminator.
3. Confirm the line reaches the executor exactly once and returns the ordinary
   unknown-command result.
4. Re-arm with a new case ID and send the corresponding 2,048-byte line.
5. Confirm UART discards the complete overlength line, the executor probe
   remains untouched, and no prefix result or action is emitted.

Evidence: sent byte counts, UART discard/result lines, executor probe count,
allocation counters, and the case-ID terminal record.

### `P0-CMD-WEB-SYNC`

1. Arm the one-shot executor probe over physical UART; the harness must offer
   no web arming endpoint.
2. From an authenticated disposable web session, submit a 2,047-byte command
   string to the ordinary synchronous CLI endpoint.
3. Confirm one executor admission and the ordinary unknown-command response.
4. Repeat with 2,048 bytes and a fresh case ID.
5. Confirm explicit input-limit rejection occurs before direct execution,
   request allocation, or queueing, and the executor probe remains untouched.

Evidence: HTTP status/body, command byte count before JSON wrapping, UART
case-ID records, and the executor/allocation deltas. Do not retain cookies.

### `P0-CMD-ASYNC`

Invoke over the physical test UART:

```text
eventcatalogtest <case-id> async_input_boundary
```

The harness calls the real `submitCommandAsync()` with two benign unknown
commands. The 2,047-byte request must be admitted and produce exactly one
ordinary error callback. The 2,048-byte request must return `false` before
allocation or queueing and must never call back. Record request-allocation and
queue-depth deltas for both attempts.

### `P0-CMD-BLE-RAW`

With plaintext BLE explicitly allowed on the disposable device, send one
authenticated benign command as a single characteristic write at 511 raw
bytes. Confirm exactly one executor admission. Repeat at 512 raw bytes using
both printable padding and NUL/control padding after a valid command prefix.

Expected: both 512-byte records are discarded before normalization and produce
one same-session limit error, with zero login/logout/native-command handling
and zero executor admissions. A filtered tail must never hide raw overflow.

### `P0-CMD-BLE-PREPARED`

Use an ATT client that exposes prepare/execute writes. Attempt a one-fragment
prepare, a multi-fragment 511-byte value, a 512-byte value, cancel, empty
execute, overlapping/out-of-order offsets, and interleaved attempts from two
connections against the command-request characteristic.

Expected: every prepare fragment receives `ESP_GATT_NOT_LONG`; execute never
invokes the command callback, replays the characteristic's previous value, or
combines connections. Ordinary single-write commands remain functional.

### `P0-CMD-BLE-SECURE`

Establish the application Secure Channel, then send valid HELLO/CONFIRM/DATA
records at their supported sizes. Inject a recognized secure-frame type above
the 517-byte classifier bound through the lowest-level test runner, and inject
authentication, replay, allocation, and queue failures for in-bound records.

Expected: valid bounded records are consumed once. Every oversized or failed
recognized frame is rejected/dropped as a secure frame and never falls through
to plaintext parsing, regardless of whether plaintext BLE is otherwise
allowed. Do not retain keys or decrypted payload captures as evidence.

## Settings transaction and failure cases

All cases in this section are **PENDING until the named default-off harness
case exists**. Use only the disposable identity. Each command must emit
`EVENTCAT_TEST <case-id> PASS|FAIL ...`; a timeout or missing terminal record
is not a pass.

### `P0-SETTINGS-RMW` — `settings_rmw_barrier`

Pause transaction A after loading the disposable user's settings. Start a
disjoint transaction B from the web-style writer and prove it waits outside
the transaction. Resume A; B must then load A's committed document and add its
own edit. Repeat with two disjoint notification-list mutations and with a
password/gamepad-style patch whose expensive hash was prepared before lock
entry.

Expected: every final document contains both unrelated edits; the barrier
records lock-entry/commit order; no writer performs an out-of-transaction
load→save pair.

### `P0-SETTINGS-PRECOMMIT` — `settings_precommit_fail`

Inject a one-shot temporary-file open or zero-write failure before rename.
Expected: `Error:`, no rename, old destination byte-identical, incomplete temp
removed, preference cache invalidated, hook disarmed, and authoritative reload
matching the old destination.

### `P0-SETTINGS-TEMP-SHORT` — `settings_temp_short_write`

Accept a nonzero strict prefix of the pre-measured compact JSON into the temp
file. Expected: serialized count or observed file size differs from expected;
the temp is never renamed, the old destination stays byte-identical, the temp
is removed, cache state is invalidated, and the command returns `Error:`.
Record expected, attempted, accepted, serialized, temp-size, and final-size
values without claiming a checked flush result.

### `P0-SETTINGS-DEST-TOUCHED` — `settings_destination_touched_fail`

Force rename failure, open/truncate the disposable destination for the direct
fallback, then fail before serialization. Expected: `Error:`, no rollback
claim, cache invalidation, and reload of the actual authoritative destination
state. Cleanup reconstructs or removes only the disposable user's file.

### `P0-SETTINGS-DEST-SHORT` — `settings_destination_short_write`

Force rename failure and accept a nonzero strict prefix during the direct
fallback. Expected: a positive write count is still failure; cache state is
invalidated; the command reports `Error:`; authoritative reload reflects the
actual short destination; and cleanup removes the disposable identity. Record
the same byte metrics as the temp-short case.

### `P0-SETTINGS-CACHE-RACE` — `settings_cache_fill_race`

Pause a viewer resolver after it loads the old settings from flash but before
cache-generation validation. Commit a notification mutation and invalidate
the cache, then resume the resolver.

Expected: the stale load is discarded, the resolver retries, the caller sees
the new rule, and no valid cache slot contains the pre-commit masks. Record
captured/committed/published generations and lock diagnostics proving the
cache mutex was not held across filesystem I/O.

## Mandatory cleanup and recovery

1. Query the harness and verify no hook remains armed.
2. Restore or remove the disposable user's settings, then delete the
   disposable identity through the normal authenticated interface.
3. Restore any changed device-wide notification kind to `all`.
4. Remove temporary files created for the disposable settings destination.
5. Reboot and record the reset reason and a clean settings load.
6. Full-clean, build, and flash the ordinary FeatherS3 profile.
7. Verify `eventcatalogtest`, its module row, and all test-hook symbols are
   absent from the ordinary link map and rejected at runtime.
8. Review and scrub `output/event_catalog_device/<run-id>/`; commit no secret
   or personal material.

## Phase 0 completion gate

Phase 0 device acceptance is complete only when every Phase 0 row has reviewed
`PASS` evidence, mandatory cleanup is recorded, and the ordinary production
artifact is restored with no harness symbols. A host fixture pass alone does
not satisfy this gate.

## Phase 2 physical/transport case index

| Case ID | Surface | Initial status | Prerequisite |
|---|---|---|---|
| `P2-CMD-UART-JSON` | Authenticated UART host request/reply | `PENDING` | Physical UART collector and disposable admin |
| `P2-HTTP-JSON` | Guest-readable `/api/events/kinds` stream, subject to normal web authentication policy | `PENDING` | Ordinary HTTP device session |
| `P2-HTTP-SINK-FAIL` | Deterministic partial-stream failure | `PENDING` | UART-only `http_sink_fail_n` harness hook |
| `P2-BLE-SECURE-CATALOG` | Phone-server Secure Channel | `PENDING` | Hash-pinned generic physical client and disposable login |
| `P2-BLE-PLAINTEXT-REFUSAL` | Unsupported unfragmented plaintext lane | `PENDING` | Generic client refusal check; do not send the full payload |

### `P2-CMD-UART-JSON`

Issue `events kinds json` through the authenticated UART host request/reply
lane. Capture exactly one complete result and compare its raw 2,877 bytes and
parsed 12-family/152-kind order with the reviewed fixture. Confirm the command
result is not the interactive console's 255-byte display path, contains none
of `boot`, `none`, `set`, `patch`, `all`, or `list`, contains no numeric ids,
and leaves no persistent catalog file/cache.

### `P2-HTTP-JSON`

From an ordinary authorized device web session (the guest role is allowed),
fetch `GET /api/events/kinds`. Compare the complete body byte-for-byte with the
UART command result and fixture. Record response status/type, body size,
family/kind counts, and server health after a second request. This proves a
live endpoint; the host parity test alone does not mark this row passed.

### `P2-HTTP-SINK-FAIL`

Arm `http_sink_fail_n` only over the physical test UART with a unique case id,
then make one authorized catalog request (the guest role is allowed). The sink
must accept exactly N serializer chunks, reject the next, and make the handler
return/close without a chunk terminator or JSON error tail. Record the
correlated server terminal line and client/packet result. A client-side reset
without the deterministic hook is not equivalent evidence.

### `P2-BLE-SECURE-CATALOG`

Use the hash-pinned generic client to establish a fresh application Secure
Channel and disposable authenticated session, issue `events kinds json`,
reassemble the paced reply frames, and compare the complete 2,877-byte payload
with the reviewed fixture. Record only scrubbed framing/count evidence; retain
no credentials, session keys, device address, or decrypted capture.

### `P2-BLE-PLAINTEXT-REFUSAL`

With no established Secure Channel, ask the generic client to fetch the full
catalog. It must refuse locally before writing the command, because the
plaintext reply lane provides one unfragmented notification and cannot carry the
2,877-byte v1 payload. Do not treat an attempted oversized notification, missing
reply, or asynchronous send failure as a successful compatibility test.

## Phase 2 completion gate

Phase 2 acceptance remains incomplete until every Phase 2 physical/transport
row above has reviewed evidence, all fault hooks are disarmed, disposable
identity material is removed, and an ordinary production artifact is restored.
The 18/18 host pass, pinned offline BLE pass, and complete restoring build
matrix do not satisfy the physical acceptance gate by themselves.

## Phase 3 completion gate

Phase 3 source implementation and OLED-enabled compilation are established,
but physical OLED acceptance remains incomplete until real-display navigation,
kinds 127/128/152, low-toggle preservation, invalid-ordinal,
persistence/reboot, and authorization procedures have reviewed evidence and
cleanup. The 18/18 host pass, diagnostic OLED build, successful
`DISPLAY_TYPE=0` FeatherS3 build, and web-browser observation do not satisfy
that physical gate individually or together.
