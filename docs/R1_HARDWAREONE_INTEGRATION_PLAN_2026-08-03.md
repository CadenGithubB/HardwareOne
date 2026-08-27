# R1 Ring integration plan for HardwareOne

**Status:** implemented in source; firmware/device validation pending  
**Evidence baseline:** official Even app 2.2.7 (121), R1 firmware 2.2.7.0005  
**Last updated:** 2026-08-03

This plan turns the August 3 official-app captures into a safe, testable R1
integration. It supersedes the protocol assumptions in
[`R1_HEALTH_FIXES_PLAN.md`](R1_HEALTH_FIXES_PLAN.md) and the R1 rows in
[`DEVICE_SETTINGS_BACKLOG.md`](DEVICE_SETTINGS_BACKLOG.md) wherever those
documents disagree with the evidence summarized here.

**Cutover policy:** all HardwareOne devices will be erased and reprovisioned
after this work lands. Do not implement persisted-setting migration, storage
migration, old JSON compatibility, command aliases, transitional public APIs,
or an older R1 protocol profile. This implementation targets the observed R1
firmware 2.2.7.0005 and fails closed on any other firmware until it receives its
own official-app evidence.

## Implementation result

The source implementation now includes the serialized R1 owner, packet ACKs,
exact `2.2.7.0005` profile gate and dual-temple setup, separate ring controls
and local Health Logging, typed daily parsing/coordinator, exact-key local
history persistence, and updated G2/OLED/web/CLI surfaces described below.
Activity remains explicitly partial/unverified, health collection remains
ACKed-unverified because no GET is proven, and user-profile writes remain
absent. The remaining release gate is controlled on-device validation; no R1
or G2 erase is part of that process.

## Outcome

HardwareOne should be able to:

1. Read and explicitly change the R1's health-collection and low-power
   settings without overwriting the Even app's choices on first connection.
2. Fetch the ring's available daily history and correctly decode HR, HRV,
   SpO2, steps, and calorie data.
3. Store and present that information locally; HardwareOne must not reproduce
   the Even cloud upload.
4. Reliably distinguish an enqueued BLE write, a transmitted frame, an R1
   acknowledgement, and a verified setting value.
5. Identify firmware 2.2.7.0005 exactly and disable version-sensitive behavior
   on any other firmware.

Profile writing, sleep decoding, and speculative ring controls are not part of
the first implementation.

## Non-negotiable boundaries

- Implement only behavior observed from the official app or proven by a
  read-only response. Do not infer a write merely from an enum or decompiled
  method name.
- Never calculate or send an age. The official app sent `age=0` after a real
  birthday save in this session, which proves HardwareOne must not derive a
  value the ring/app did not report. Personal profile writes and profile UI
  remain outside this implementation even if future captures clarify fields;
  adding them would require a separate, explicit safety decision.
- Do not send health, profile, or low-power writes automatically on the first
  HardwareOne connection. The default desired state is **Preserve**.
- Never blindly replay a captured frame. New requests need a new serial and,
  where the payload calls for one, a current timestamp.
- Do not add Even account or cloud behavior. Captured health data remains
  local to HardwareOne unless the user deliberately exports it.
- Keep G2 settings on the G2 protocol. Dominant hand and long-press behavior
  are not R1 settings.
- Do not expose speculative reset, OTA, touch, power-control, or raw system
  writes through a normal UI.

## What the official app established

| Capability | Proven wire behavior | HardwareOne implication |
|---|---|---|
| R1 health collection | `system/system/0x0E`, SET status, 12-byte payload: `u32LE epoch`, `u8 enabled`, seven zero bytes | Add an explicit tri-state control: Preserve / On / Off |
| R1 low-power mode | `system/system/0x0F`; GET returns a 12-byte state payload; SET payload is `u32LE epoch`, `u8 switchType=0`, `u8 enabled`, six zero bytes | Add GET/cache first, then explicit Preserve / On / Off |
| R1 user profile | `system/system/0x04`, payload `gender`, `age`, `heightCm`, `weightKg`, padding; the official birthday save still emitted `age=0` | Decode for diagnostics only; do not write or synthesize age |
| Ring hand | No R1 hand bit was observed. The app changed G2 dominant-hand field 11 and sent the same R1 `advStart` payload in both directions | Keep hand on G2; refresh R1 routing after the G2 change |
| R1 `advStart` | On 2.2.7.0005 the payload contains reversed right-temple MAC followed by reversed left-temple MAC | Replace the current builder with the proven 12-byte form; reject other firmware |
| Long press while display off | G2 settings field 10, with three gesture entries; no R1 traffic | Optional, separate G2 parity tranche |
| Include audio recordings | App/privacy API only; no R1 frame | No HardwareOne firmware work |
| Enabling health collection | After the setting ACK, the app immediately fetched HR, HRV, SpO2, sleep/skin-temperature, then activity | Use this exact sweep order; do not issue a standalone temperature-daily request |
| Manual health refresh | Same history sweep, normally throttled to ten minutes | Provide normal refresh plus a clearly explicit force-refresh action |
| Daily data notify | The app replies with `system/system/packetAck (0x7E)` | Add packet acknowledgement before depending on history transfer |
| Empty sleep result | Command ACK arrived, then sleep and skin-temperature timed out without a data notify | Treat as “no data,” not connection failure |

The private capture index and action timeline live under `.scratch/btsnoop/`;
durable protocol documentation should contain sanitized schemas and fixture
bytes only, never a phone serial, user profile, or real device MAC.

## Target architecture

### 1. One serialized R1 control path

The current global `R1Encoder` and direct `ringWrite()` calls are used by
multiple paths. In addition, `ringWrite()` returns `true` when a frame is only
queued ([`G2_Ring.cpp`](../components/hardwareone/G2_Ring.cpp)), while setup
uses fixed delays and does not correlate ACKs. Build an R1 control owner with:

- an intent queue, rather than callers constructing and writing frames;
- exclusive ownership of the encoder and serial counter;
- one active transaction at a time for control and history requests;
- matching by request serial plus module/cmd/subcommand;
- explicit states: `queued`, `written`, `acked`, `verified`, `refused`,
  `timedOut`, and `disconnected`;
- a notify callback that only decodes, updates small state, and signals the
  owner—never waits;
- retry rules per operation. An idempotent request may be rebuilt with a fresh
  serial; captured bytes are never replayed verbatim;
- connection-generation tagging so a response from an old link cannot satisfy
  a new transaction.

The first security fix in this tranche is to make `ringquery raw` admin-only
and require a confirmation token for dangerous system SET operations. It is
currently registered as non-admin at the bottom of
[`G2_Ring.cpp`](../components/hardwareone/G2_Ring.cpp).

### 2. Firmware-profiled protocol codec

Extend [`System_R1_Protocol.h`](../components/hardwareone/System_R1_Protocol.h)
and [`System_R1_Protocol.cpp`](../components/hardwareone/System_R1_Protocol.cpp)
with a small `R1ProtocolProfile` selected from the decoded device-info firmware
version. Initially support only:

- `Fw227_0005`: dual reversed temple MACs and the daily layouts proven on
  2.2.7.0005.
- `Unknown`: safe reads and diagnostics only; no version-sensitive setting
  writes or history decoding.

Do not heuristically accept a payload as whichever layout happens to fit.
Reject it with a useful diagnostic when the active profile and payload disagree.

Add typed builders/parsers for:

- health-collection SET and empty ACK (no GET/readback is proven);
- low-power GET, SET, and response;
- 2.2.7 dual-MAC `advStart`;
- `packetAck` for a received data-notify serial;
- common daily pages and activity daily pages;
- user-info decode only.

Deprecate `buildHealthReportEnable()`. It targets a speculative
`health/healthSetting/reportEnable` operation that was not used by the shipping
app and must not be presented as the health-collection toggle.

### 3. Separate settings and state

The existing `gSettings.healthTrackingEnabled` means “run HardwareOne's local
polling/logger” ([`System_Settings.h`](../components/hardwareone/System_Settings.h)
and [`System_SensorLogging.cpp`](../components/hardwareone/System_SensorLogging.cpp)).
Rename it directly to `healthLoggingEnabled` and expose it as **Health logging**
or **Log R1 health**. Because devices are erased at cutover, do not retain the
old persisted key or `healthtrack` command alias.

Add distinct state for the ring:

```text
ringHealthCollectionDesired = Preserve | Off | On
ringHealthCollectionObserved = Unknown | Off | On
ringLowPowerDesired = Preserve | Off | On
ringLowPowerObserved = Unknown | Off | On
```

`Observed` is runtime state updated only where a GET response is capture-proven.
That means low power can become Off or On, while health collection remains
Unknown even after an explicit SET is ACKed. `Desired` is persisted only after
an explicit user choice. Preserve never generates a SET. After the user chooses
On or Off, a later reconnect may reassert that explicit choice only after the
protocol profile is known and initial setup has succeeded. Low power can then
be verified with GET; health collection must remain labelled ACKed-unverified.

### 4. History coordinator and typed data

Replace the current generic `R1DailyResult` with typed page models. For the
2.2.7 profile, the proven layouts are:

```text
Common prefix:
  count:u8, timezoneMinutes:i16LE, dayStart:u32LE

HR / SpO2 record stream:
  latestTimestamp:u32LE, latestValue:u8,
  repeated { hourSlot:u8, average:u8, maximum:u8, minimum:u8 },
  trailer:u32LE

HRV record stream:
  latestTimestamp:u32LE, latestValue:u16LE,
  repeated { hourSlot:u8, average:u16LE, maximum:u16LE, minimum:u16LE },
  trailer:u32LE

Activity record stream:
  repeated { tenMinuteSlot:u8, steps:u16LE,
             activeKcal:u16LE, totalKcal:u16LE },
  trailer:u32LE

Derived restingKcal = totalKcal - activeKcal, after validating total >= active.
```

Keep the latest point and aggregated bucket as separate facts. Later captures
proved that `dayStart` may be zero and that `latestTimestamp` may independently
be an absolute epoch or seconds within a day. Preserve the raw values and tag
their modes. Compute `dayStart + slot * 3600` for common metrics or
`dayStart + slot * 600` for activity only when the same page supplies a
plausible timezone-aligned day boundary. Normalize seconds-within-day only
against that anchored page; never derive a date from the host clock, receive
time, page order, or a neighboring response. Retain the signed timezone from
the payload as metadata instead of silently converting the wire day boundary.

The fetch state machine is:

```text
HR daily -> HRV daily -> SpO2 daily -> sleep daily -> activity daily
```

For each step: send the query, wait for its command ACK, accept the matching
data notify, send `packetAck`, parse, and persist. A valid command ACK followed
by no sleep data becomes a successful empty result after the bounded timeout.
Other timeouts remain visible as partial-fetch errors, without discarding
already committed metrics.

Temperature is not separately queried during this sweep. Reserve a typed
sleep/skin-temperature result, but do not decode it until a real non-empty
official-app capture establishes its shape.

### 5. Local model, persistence, and surfaces

Extend the health model in
[`G2_Health.cpp`](../components/hardwareone/G2_Health.cpp) without treating
activity freshness as vital-sign freshness. Store:

- HR/HRV/SpO2 hourly average, min, max, and bucket time;
- the separately timestamped latest sample;
- steps, active kcal, resting kcal, total kcal, and ten-minute bucket time;
- fetch status, source firmware/profile, timezone metadata, and parse errors.

The existing RAM cache remains the live fast path. Replace the old health CSV
contract with a clean live-health schema and use a separate v1 idempotent daily
history store/export for aggregates and activity. The new files must reuse the
health-data encryption policy, define retention, and pass atomic write/recovery
tests before automatic refresh is enabled. No old-file reader or converter is
required. Do not upload to Even or another service.

Update these surfaces together:

- [`OLED_Mode_R1_Health.cpp`](../components/hardwareone/OLED_Mode_R1_Health.cpp)
- [`WebPage_R1_Health.cpp`](../components/hardwareone/WebPage_R1_Health.cpp)
- CLI commands in [`G2_Ring.cpp`](../components/hardwareone/G2_Ring.cpp)
- the app/JSON contract if the controls are exposed remotely

Recommended labels:

- **Ring health collection:** Preserve / On / Off
- **Health logging:** On / Off
- **Ring low power:** Preserve / On / Off, with current observed value
- **History:** last successful refresh, partial errors, normal Refresh, and
  explicit Force refresh

Do not add a profile editor in this tranche.

## Dependency order and release gates

The work packages below are deliberately more granular than the delivery
phases. They are the units that should be implemented and reviewed. A later
package must not work around an incomplete prerequisite with fixed delays or a
second private queue.

| Work package | Depends on | May ship independently? |
|---|---|---|
| WP0 Evidence and executable fixtures | Nothing | Yes |
| WP1 Serialized command owner | WP0 | Yes, with existing features routed through it |
| WP2 Data-notify packet ACK | WP1 | Yes |
| WP3 Firmware profile, setup, and time policy | WP0-WP1 | Yes |
| WP4 Ring health-collection control | WP1-WP3 | Yes, defaulting to Preserve |
| WP5 History coordinator | WP1-WP4 and packet ACK | Only as an explicit/manual feature |
| WP6 Typed daily parsers | WP0, WP3, WP5 | Yes for proven single-frame data; full-day support has a separate gate |
| WP7 Local history and user surfaces | WP4-WP6 | Yes, after a clean device erase |
| WP8 Low-power control | WP1 and WP3 | Yes, defaulting to Preserve |
| WP9 Optional G2 settings parity | Stable core R1 path | Yes; it must not block R1 health work |

The critical path is `WP0 -> WP1 -> WP2/WP3 -> WP4 -> WP5 -> WP6 -> WP7`.
Low power and G2 parity stay off that path.

## Detailed implementation work packages

### WP0 — Evidence, fixtures, and test harness

**Purpose:** make the captured official-app behavior executable before changing
the transport.

**Changes**

- Add sanitized byte fixtures for every proven setting request, response, ACK,
  and daily payload. Remove real MACs, phone serials, device serials, and profile
  values; generated fixture MACs must be unmistakably synthetic.
- For each fixture record the app version, R1 firmware profile, direction,
  opcode tuple, status byte, expected typed values, and whether the app sent a
  packet ACK.
- Add negative fixtures: bad CRC32, declared-length mismatch, truncation,
  unknown status, wrong firmware profile, count overflow, and kcal underflow.
- Create an executable first-party test target for the portable protocol code.
  The repository currently has only the boot-time `r1ProtocolSelfTest()` for
  this subsystem. Keep a small boot smoke test, but do not make device logs the
  only test oracle.
- Make `r1ProtocolSelfTest()` failure visible and fail closed for R1 writes. Its
  result is currently discarded by `g2RingInit()`.

**Likely files**

- `System_R1_Protocol.h/.cpp`
- a new host/IDF protocol test target and fixture directory
- `components/hardwareone/CMakeLists.txt`
- sanitized protocol documentation under `docs/`

**Problems and downstream effects**

- `System_R1_Protocol.h` includes Arduino types, which makes a host test target
  harder. Keep the binary codec/data structs in a portable unit and leave
  Arduino logging/adapters outside it, or use an intentionally small Arduino
  compatibility shim. Do not write a second parser in Python and test that
  instead of the production C++.
- Captured timestamps and health values are not inherently identifiers, but a
  complete capture can still expose personal data. Store only the minimum bytes
  needed for a protocol assertion.
- New CMake sources must remain conditional when Bluetooth, G2, or R1 Health is
  disabled.

**Acceptance gate**

- Tests reproduce every known outbound frame and decoded official-app value.
- At least one bit flip fails CRC validation and cannot reach a typed parser.
- A failing boot self-test prevents outbound R1 traffic and produces an
  unconditional diagnostic, not only a debug-flagged line.

**Rollback:** fixture/test additions are non-runtime. The portable extraction
must preserve the existing public codec API until WP1 is complete.

### WP1 — Serialized R1 command and transaction owner

**Purpose:** remove races and give every caller an honest completion result.

**Changes**

- Introduce a fixed-capacity `R1Intent` queue owned by one normal FreeRTOS task
  or one existing BLE worker context. Only that owner may allocate a serial,
  call the encoder, or submit a ring GATT write.
- Convert these current producers to submit intents:
  - connection setup and corrective clock sync;
  - dark-clock HR probes;
  - OLED Poll Now;
  - web/CLI Poll Now;
  - G2 Health Poll Now and Trends;
  - Health logging's timed mine;
  - direct CLI queries;
  - future settings and history coordinators.
- Preserve the existing lock order `bleCentralTx -> gRing.writeMutex`. The
  owner must not hold its own state lock while waiting for either BLE lock.
- Replace the boolean write result with a transaction handle/snapshot containing
  intent id, connection generation, request serial/opcodes, lifecycle state,
  enqueue/write/ACK timestamps, refusal code, retry count, and final error.
- Match responses by connection generation, serial, module, command, and
  subcommand. A module/cmd match without a serial match remains unsolicited
  telemetry and must not complete the transaction.
- Keep passive telemetry ingestion independent. Point data can update the live
  cache even when no caller is waiting, after integrity checks.
- Reserve queue capacity and priority classes:
  1. packet acknowledgements and connection setup;
  2. active setting/history transaction;
  3. user point poll;
  4. logger/background poll.
  Low-priority polls may coalesce or drop; setup, control, and packet ACKs may
  not be evicted by them.
- On disconnect, atomically advance the connection generation, cancel queued
  link-bound intents, finish the active transaction as `disconnected`, clear
  old response state, then reset the encoder inside the owner.
- Replace fire-and-forget public APIs such as `g2RingPollVital()` across all
  callers in the same change. Do not retain transitional wrappers whose boolean
  could still be mistaken for protocol success.

**Problems and downstream effects**

- The current eight-slot raw byte queue drops the oldest frame when full and
  reports `true` for an enqueued frame. Reusing it below the new transaction
  queue would leave two sources of truth. Either the owner must directly own a
  redesigned transport queue, or the lower queue must report an actual
  `written` callback keyed to the transaction.
- The BLE notify callback cannot block, perform filesystem work, or wait on the
  transaction owner. It should validate/copy into a bounded mailbox and signal.
- Current polling code advances its cursor even if `ringWrite()` only queued or
  failed. Each producer must advance only when its intent is accepted; the
  coordinator advances the protocol step only on the required completion.
- Fixed delays in setup, Trends, Health Track, OLED, and the G2 worker will
  otherwise fight the new state machine. Remove their protocol pacing while
  retaining UI animation timing.
- A single active acknowledged transaction can reduce apparent polling speed.
  Coalescing duplicate point requests and allowing unsolicited telemetry keeps
  the user-facing cache responsive without permitting parallel serial use.
- Serial wrap must be defined. Do not reuse a serial while a transaction from
  the same connection generation is active; wrap only after outstanding state
  is empty.
- The owner task and mailboxes consume RAM. Use fixed records and bounded
  payload ownership; avoid allocating on every health notification.

**Acceptance gate**

- Stress tests submit all current producers concurrently without duplicate or
  out-of-order serial allocation.
- Tests distinguish accepted, queued, written, ACKed, refused, timed out, and
  disconnected states.
- Queue saturation discards/coalesces background polls first and never loses a
  setup frame, setting write, or packet ACK.
- No BLE callback waits on the owner; lock-order instrumentation shows no
  inversion with glasses image traffic.

**Rollback:** do not retain a runtime switch between two encoder owners; that
would recreate the race. Roll back the whole WP1 firmware change and erase the
device again if its stress and reconnect gates fail.

### WP2 — Packet acknowledgement for daily data notifies

**Purpose:** respond to R1 history data the way the official app does.

**Changes**

- Add an exact `system/system/packetAck (0x7E)` builder from a sanitized capture
  fixture. Document which received serial/status fields it echoes rather than
  assuming the command ACK format.
- When the notify path receives a CRC32-valid, length-valid daily data frame,
  copy/enqueue the frame for processing and immediately enqueue a high-priority
  packet-ACK intent. Do not wait for parsing or filesystem persistence.
- Dedupe storage by a stable page identity, but ACK an identical retransmitted
  valid data frame again; it may be the ring retrying because the prior ACK was
  lost.
- Track ACK enqueue latency, actual write latency, and failure count in R1
  diagnostics.

**Problems and downstream effects**

- ACKing a corrupt or incomplete frame tells the ring data was received when it
  was not. Require valid CRC32 and declared model length first. The current
  telemetry path parses frames even when those flags are false; that must be
  corrected for all typed ingestion.
- Parsing/persistence before ACK can exceed the ring's retry deadline,
  especially when flash or SD is busy. ACK and processing need separate queues.
- The ACK may itself allocate a serial or echo the received one. The golden
  fixture, not convention from another opcode, decides this.
- If the high-priority mailbox is full, record the page as incomplete and let
  the fetch fail visibly; do not silently ingest data that was never ACKed.

**Acceptance gate**

- A btsnoop comparison shows the same packet-ACK shape and ordering as the
  official app.
- Duplicate delivery produces multiple ACKs but one stored page.
- Bad-CRC, wrong-length, and wrong-generation frames are neither ACKed nor
  ingested.

**Rollback:** the history coordinator remains disabled if packet ACK is not
available. Point telemetry can continue through WP1.

### WP3 — Firmware profile, setup sequence, `advStart`, and clock policy

**Purpose:** stop applying one firmware's layout to another and fix current
2.2.7 setup parity.

**Changes**

- Parse device-info into bounded firmware and hardware strings and cache them
  as runtime identity. Do not log the device serial as part of profile
  selection.
- Make setup an ACK-driven state machine:
  `pairAuth -> deviceInfo -> select profile -> systemTime -> advStart`.
- Prove on hardware that device-info is available after pairAuth and before
  `advStart`. If it is not, add an explicit bootstrap rule backed by a capture;
  do not guess the profile from payload length or ring name.
- Match firmware `2.2.7.0005` exactly, not a loose `2.2` or `2.2.7` prefix. A
  firmware update on the same MAC must switch the runtime profile to Unknown.
- For `Fw227_0005`, build the 12-byte `advStart` from reversed right-temple MAC
  then reversed left-temple MAC. Fetch both MACs before encoding and report
  which is missing. Never reuse the same six-byte array for both halves.
- Delete the six-byte builder and old daily decoder instead of carrying a
  second protocol implementation. `Unknown` may authenticate and perform
  version-independent reads, but it must not send version-sensitive
  setup/settings writes.
- Route both standard setup and the compiled-but-unregistered ring-bridge code
  through the profiled builder so stale code cannot later reintroduce the old
  format.
- Replace both hard-coded timezone-zero system-time call sites—including the
  later corrective push—with one policy function. For official-app parity the
  candidate value is `Clock::tzOffsetMinutes()`, never an inferred timezone.
  Record the exact sent offset in diagnostics.
- Do not send an invalid/dark epoch. Preserve the existing ring-clock custody
  behavior, but express its probes and corrective sync as transactions so a
  pre-auth corrective push cannot consume serial 1.

**Problems and downstream effects**

- Waiting for device-info adds a setup round trip. UI should show
  `identifying` separately from `connected`; auto-reconnect must not declare the
  ring ready before profile/setup completion.
- Temple MACs may not be available when the ring connects before the glasses.
  The owner should hold `advStart` pending for a bounded time and allow a later
  idempotent routing refresh. Sending zeros should be a named fallback with an
  official-app evidence justification, not the normal 2.2.7.0005 path.
- Current comments, self-tests, and bridge diagnostics assert that a single MAC
  is not reversed. Those must change in the same commit as the builder.
- Changing the sent timezone from UTC to HardwareOne's configured offset will
  change future ring day boundaries. Existing records must retain their payload
  `{dayStart, timezoneMinutes}` and must not be rebucketed. The first transition
  can yield a partial/overlapping day and should be labeled as such.
- HardwareOne stores a fixed absolute offset, not an IANA timezone with
  automatic DST rules. The plan must not claim automatic DST changes. If the
  user changes the configured offset while connected, enqueue one fresh
  system-time transaction; never rewrite existing history.
- `Unknown` profile is intentionally less functional. That is safer than
  silently sending the wrong directed-advertising payload after an R1 update.

**Acceptance gate**

- 2.2.7 hardware emits byte-identical `advStart` payload semantics to the
  official app using synthetic test MACs and correct live temple ordering.
- Unknown firmware performs no version-sensitive write and exposes a useful
  “unsupported firmware” status.
- Reconnect/setup never duplicates serial 1, and no corrective clock frame is
  sent before pairAuth completes.
- Daily data captured after setup reports the configured offset; a timezone
  transition does not mutate previously stored day keys.

**Rollback:** selection can fail closed to `Unknown`. Settings/history controls
remain unavailable, while connection diagnostics and version-independent point
telemetry remain usable.

### WP4 — Safe R1 health-collection control

**Purpose:** expose the proven Privacy > Health Tracking ring setting without
confusing it with HardwareOne's logger.

**Changes**

- Add a persisted integer enum with stable values such as
  `0=Preserve, 1=Off, 2=On`; add compile-time assertions and registry bounds.
- Keep observed state runtime-only. For health collection it remains `Unknown`
  because only an empty SET ACK was observed; expose ACKed-but-unverified
  separately from observed state. A saved desire is not evidence of ring state.
- Implement the exact timestamped health SET builder for the active firmware
  profile. Do not add a GET/readback path: none was observed in the captures.
- Add a dedicated admin command rather than a generic setting editor with no
  live-apply hook. The settings registry may point its `cmdKey` at that command
  so web/OLED settings surfaces still apply through the transaction owner.
- Define command semantics:
  - `Preserve`: persist Preserve, cancel future reconciliation, send no SET;
  - `Off`/`On` while offline: persist explicit desire and report “pending”;
  - `Off`/`On` while online: persist desire, SET explicitly, report the empty
    ACK as accepted, and leave observed state Unknown/unverified;
  - refusal/timeout: keep desired state, expose failure, and retry at most once
    per connection unless the user explicitly retries.
- On reconnect, identify/profile/setup first, then reassert only when desired
  is not Preserve. Preserve sends no health-setting frame.
- Start the history coordinator only after the explicit On SET is ACKed. Skip a
  normal refresh when the persisted desired policy is Off; there is no proven
  readback from which to infer the ring's current state under Preserve.
- Rename product-facing `Health Track` labels and code to `Health logging` or
  `Log R1 health`. Replace the command with `healthlogging` and the persisted
  field with `healthLoggingEnabled`; do not retain aliases.

**Problems and downstream effects**

- The generic settings framework persists a scalar and returns success but has
  no transaction-completion callback. Using it directly would let the UI claim
  the ring changed when only flash changed. All mutating surfaces must use the
  dedicated command/status model.
- Persisting desired state before hardware ACK is intentional: it represents
  user policy and supports offline configuration. UI must show Desired and
  Observed separately so a failed apply is not mistaken for success.
- HardwareOne Health logging may remain On while ring collection is Off. Do not
  silently re-enable collection. Show that logging is active but data may be
  stale/unavailable.
- Another app can change the setting later, and HardwareOne cannot read that
  health state back. Preserve deliberately sends nothing; an explicit On/Off
  policy reasserts only on the next HardwareOne connection/user action, not in
  a tight continuous fight.
- Turning Off during a fetch must cancel unsent history steps, finish the
  active transaction safely, and retain already committed data.
- This is a privacy-related control. CLI/API mutation must require admin access;
  status reads can remain available to authenticated non-admin users.
- No profile field belongs in this UI. In particular there is no age
  calculation or birthday-to-age conversion anywhere in the dependency graph.

**Acceptance gate**

- A freshly erased/default connection sends no health-setting SET.
- Every surface shows desired, observed, pending, and last error consistently.
- Official-app-equivalent On/Off frames are produced only after an explicit
  choice; On begins one history fetch only after ACK.
- Logger On + collection Off remains possible and does not trigger an implicit
  ring write.

**Rollback:** set desired to Preserve and hide/disable the mutating surface.
The read-only observed value and existing local logger remain useful.

### WP5 — One explicit history-fetch coordinator

**Purpose:** replace independent fire-and-forget daily requests with one
observable operation.

**Changes**

- Create one coordinator used by G2 Trends, web, CLI, post-enable sync, and any
  future scheduled refresh. Remove the daily queue from `G2_Health.cpp` once
  its callers use the coordinator.
- Give each fetch an id, source, connection generation, start/end time,
  normal/forced mode, current metric, per-metric result, page count, and final
  state (`complete`, `partial`, `empty`, `failed`, `cancelled`).
- Use the official order:
  `HR -> HRV -> SpO2 -> sleep/skin temperature -> activity`.
  Do not issue temperature-daily separately.
- For each metric: send the query, wait for command ACK, collect matching data
  pages/notifies, packet-ACK them, then decide completion using only observed
  protocol behavior.
- Treat sleep ACK plus bounded no-data timeout as `empty`. Do not generalize
  that exception to HR/HRV/SpO2/activity without evidence.
- Enforce the ordinary ten-minute throttle globally, not once per UI. Force
  refresh is an explicit admin/user action and still cannot overlap an active
  fetch.
- During a history fetch, pause background point mines and coalesce user point
  polls for execution afterward. Unsolicited telemetry continues to update the
  live cache.
- On disconnect, mark the fetch partial/cancelled. Do not silently resume and
  write after reconnect; a post-enable fetch may be retried once as a new fetch
  with a new id.

**Problems and downstream effects**

- Morning activity captures fit in one frame, but a full active day may contain
  up to 144 ten-minute slots. That can exceed the negotiated notification size
  and `R1_MAX_PAYLOAD`. Before declaring full-day support, capture a late-day,
  high-cardinality official-app fetch and determine whether the ring emits
  multiple complete pages, BLE fragments, only nonzero records, or another
  completion marker.
- Until that evidence exists, single-frame activity support must be labeled
  `single-page verified`; an additional page/fragment is stored diagnostically
  and makes the fetch partial rather than being concatenated by guesswork.
- The current decoder treats each BLE notification as a complete envelope and
  caps payload at 256 bytes. If the ring uses fragmentation, add bounded
  reassembly keyed by connection generation and protocol identifiers, with
  timeout and maximum-size limits. If it uses complete pages, do not conflate
  that with BLE reassembly.
- A ten-minute throttle based on wall clock fails before time sync or when the
  clock moves backward. Use monotonic time for the current boot. Persisted
  last-fetch metadata is informational and must not permanently block refresh.
- Low-power mode may lengthen response times. Timeouts should be operation and
  observed-mode aware, but never unbounded.
- G2 lens image pushes occupy the same BLE controller. Do not refresh a graph
  until the fetch commits or times out; pushing an image between every metric
  can starve packet ACKs.

**Acceptance gate**

- All surfaces observe the same fetch id/state and cannot start parallel
  sweeps.
- The captured order and packet ACK sequence match the official app.
- Sleep no-data yields `complete with empty sleep`, while a lost HR page yields
  a visible partial result.
- Point logging resumes after success, failure, cancellation, and disconnect.
- Full-day activity is not labeled complete until the pagination/fragment gate
  has been satisfied on hardware.

**Rollback:** disable new history starts while leaving point telemetry and
settings controls intact. Previously committed local history remains readable.

### WP6 — Typed daily decoding and integrity rules

**Purpose:** decode known 2.2.7 history without polluting live data with old or
malformed assumptions.

**Changes**

- Replace generic `R1DailyResult` and the old guessed daily decoder with typed
  2.2.7.0005 common, HRV, and activity page results.
- Require valid CRC32, valid model length, known profile, exact derived payload
  size, bounded record count, valid slot ranges, and safe arithmetic before a
  record enters the model.
- Store common-metric latest timestamp/value separately from hourly aggregate
  records. Do not insert the latest value as another hourly bucket.
- Compute bucket epochs with checked 64-bit intermediates only for an explicitly
  anchored day mode, then validate that they fit the firmware's epoch type.
- HR and SpO2 records use one-byte aggregate values; HRV uses two-byte values.
  Preserve min/max/average rather than collapsing every record to one byte.
- Activity stores steps, active kcal, total kcal, and derived resting kcal.
  Reject a record where `total < active`; never allow unsigned subtraction to
  wrap.
- Preserve `timezoneMinutes`, raw `dayStart`, day mode, raw/normalized latest
  timestamp and its mode, opaque trailer, profile, and source frame/page
  identity as metadata.
- Remove the speculative sleep parser from production ingestion. It may remain
  in an explicitly experimental annotator, but cannot populate user data.
- Return structured parse errors (`badCrc`, `length`, `wrongProfile`,
  `unsupportedLayout`, `slotRange`, `valueRange`, `tooLarge`) for diagnostics
  and fetch status.

**Problems and downstream effects**

- The existing live/Trend series accepts only `values[]`, `startTs`, and
  `endTs`; it synthesizes evenly spaced timestamps. Typed bucket timestamps
  require new ingestion APIs and will change graph spacing and summary math.
- HRV is currently vulnerable to byte truncation in daily history. Correct
  16-bit values can change displayed averages and graph axes; that is a bug fix,
  but snapshots/screenshots may visibly differ.
- Existing code accepts decoded frames without gating on CRC32/model length.
  Tightening integrity checks may make previously visible questionable samples
  disappear. Diagnostics must explain rejection rather than showing `--`
  forever.
- Activity slot 143 is valid for 23:50; common hour slot 23 is valid. Bounds
  must be metric-specific.
- Do not assume the observed four-byte trailer is padding or a sentinel. Retain
  it as ring-owned metadata, but do not reject a structurally valid page when
  it changes; official-app and direct-host captures proved that it varies.
- Stack copies of maximum frames plus typed arrays can be expensive on BLE
  callback stacks. Copy once into bounded owner storage and parse in a normal
  task.

**Acceptance gate**

- Golden HR, HRV, SpO2, and both activity fixtures reproduce the official app's
  values and timestamps exactly.
- Every malformed fixture returns the intended reason and leaves prior good
  data unchanged.
- Latest values and hourly aggregates remain distinguishable in API and UI.
- Any payload received under Unknown firmware fails cleanly without falling
  back to the deleted parser.

**Rollback:** a profile parser can be disabled independently. Unknown or
disabled layouts remain available as sanitized diagnostics but cannot update
health state.

### WP7 — Local history store, logging, JSON, web, OLED, G2 lens, and CLI

**Purpose:** make the newly decoded data useful without corrupting existing
logs or breaking constrained user surfaces.

**Changes: data model and persistence**

- Keep `G2RingTelemetry` and the existing 96-sample `HealthSeries` as the live
  point-data path. Add a separate typed daily-history store; do not squeeze
  aggregates and activity into live scalar fields.
- Key a ring day by local peer identity plus `{dayStart, timezoneMinutes}` so
  switching rings or offsets cannot merge unrelated days.
- Upsert records by metric and slot. Re-fetching the same day replaces a bucket
  rather than appending duplicates; partial fetches merge only validated
  metrics/pages.
- Use a v1 on-disk schema and atomic temp-write/rename. Write at most once per
  completed/partial sweep and skip the write when the content hash is unchanged
  to limit flash/SD wear. Future schema changes may require another erase;
  implement no migration framework now.
- Treat the new store as health data for `captureEncryptMode`. Reuse/refactor the
  existing sealed-capture writer; do not create a plaintext side channel when
  Health-at-rest encryption is enabled.
- Replace the existing live `health.csv` contract with a clean v1 live-health
  schema at cutover. Keep daily aggregates/activity in the separate history
  store because they have different timestamps and update semantics, not for
  backward compatibility.
- Define a storage budget/retention policy and recovery for interrupted atomic
  writes before enabling automatic daily persistence.

**Changes: shared status/API**

- Replace health status with one clean v1 contract and update every in-tree
  consumer atomically. No old-key compatibility branch is required.
- Put record arrays behind a separate paged/range endpoint or export command;
  do not put a day of activity into the current fixed 768-byte status buffer.
- Expose protocol profile, setup readiness, desired/observed settings, active
  transaction, history-fetch summary, per-metric freshness/error, and storage
  status. Do not expose raw personal profile data.
- Add dedicated authenticated/admin POST actions for ring-setting changes and
  forced refresh. Do not route privacy-affecting controls through an
  unauditable generic `ringquery raw` call.

**Changes: web**

- Rename Track to Health logging and show it separately from Ring health
  collection.
- Show desired versus observed setting values, pending/error state, normal and
  force refresh, last successful/partial fetch, steps and calorie totals, and
  an empty-sleep state.
- Make the page display unsupported firmware rather than hiding controls
  silently. It only needs to understand the new contract.

**Changes: OLED**

- Replace the current fixed two-action cursor assumptions with a small submenu
  or paged action model. Keep Poll vitals, Health logging, Ring collection,
  History refresh, and Low power readable on the available display.
- Show action pending/failure; do not optimistically flip the observed value.
  Avoid rendering full hourly history on the OLED in the first release.

**Changes: G2 lens Health app**

- Replace the worker's hard-coded row loop/count assumptions with the menu row
  count returned by `g2HealthMenuRows()`.
- Move daily request ownership out of `G2_Health.cpp` into WP5.
- Add Activity as a separate view/summary. A full 144-slot activity day exceeds
  the current 96-sample series, so use a correctly sized activity series or
  aggregate to the display width without truncating stored data.
- Keep graph pushes one-shot after a fetch commit. Each image is substantial
  BLE traffic and must remain lower priority than R1 packet ACK/control.

**Changes: CLI and logging terminology**

- Replace `healthtrack` with `healthlogging` everywhere.
- Add typed read commands for ring settings, transaction/fetch status, and
  history summary/export. Mutating setting and force-refresh commands require
  admin.
- Update `LOG_R1` comments and status text so “R1 Health” does not imply the
  logger controls the ring's sensor privacy setting.

**Problems and downstream effects**

- The existing `/api/health/status` static buffer is 768 bytes. Adding a full
  history or verbose transaction object can truncate JSON. Keep status compact,
  size it with worst-case tests, and separate history records.
- Adding an activity enum changes switches, graph axes, menu arrays, stubs for
  disabled builds, sensor snapshots, and compile-time feature combinations.
- A separate encrypted daily store may need a small refactor of capture
  encryption APIs currently oriented around streaming sessions. Security and
  power-loss behavior must be reviewed in source.
- Atomic replace on SD/LittleFS, storage exhaustion, and a removed card need
  explicit error states. Data persistence failure must not cause the protocol
  fetch to retry continuously.
- Values fetched from the ring can overlap with live points. UI must state
  whether it is showing latest point, hourly aggregate, or activity bucket.
- Settings, commands, help, web actions, OLED actions, G2 labels, and JSON keys
  must change atomically because no compatibility aliases remain.

**Acceptance gate**

- Re-fetching the same fixtures produces identical local records with no
  duplicates and no extra flash write when content is unchanged.
- Power loss during a test write leaves either the prior complete day or the
  new complete day, not a corrupt primary file.
- Health encryption mode never leaves the new store/export plaintext at rest.
- Every in-tree status consumer uses the new keys; the history endpoint handles
  maximum expected records without truncation.
- OLED and G2 navigation reach every item without fixed-index overflow; feature
  disabled builds link through complete stubs.
- A freshly erased device creates only the new live-health and daily-history
  schemas.

**Rollback:** flash the prior firmware and erase/reprovision again. Do not add an
old-file reader to support downgrade. UI/history writing can also be disabled
while live telemetry, settings, and export diagnostics continue to work.

### WP8 — R1 low-power control

**Purpose:** add the second fully observed R1 setting after the control path is
stable.

**Changes**

- Reuse the desired/observed Preserve/Off/On state pattern and dedicated admin
  command from WP4.
- Implement the proven system-settings GET/cache before enabling SET.
- Encode the timestamp, `switchType=0`, enabled byte, and padding exactly; reject
  unknown switch types in the known profile.
- Re-read on reconnect and after an explicit SET when a readback is available.
- Surface current, desired, pending, and last failure independently from
  HardwareOne's own power-saving settings.

**Problems and downstream effects**

- Low-power mode may alter connection interval, notification cadence, fetch
  latency, or how quickly the ring advertises after disconnect. Measure those
  effects and tune transaction timeouts by observed mode rather than declaring
  the SET failed too early.
- A setting ACK may arrive before the lower-power behavior takes effect. UI can
  say ACKed while observed confirmation is pending; it must not call that
  verified.
- Turning low power On during an active history fetch could change its timing.
  Serialize the SET after the fetch, or require the user to cancel the fetch;
  do not interleave it.
- Default Preserve is essential because the Even app already owns this user
  preference.

**Acceptance gate**

- First connection emits no low-power SET.
- GET and SET values match btsnoop, and reconnect reports the retained observed
  state.
- Fetch, reconnect, point polling, and advertising behavior are tested in both
  modes, with measured timeout values documented.

**Rollback:** set desired to Preserve and hide mutation. GET/cache remains a
useful diagnostic.

### WP9 — Optional G2 dominant-hand and display-off long-press parity

**Purpose:** incorporate the other captured settings without mislabeling them
as R1 features.

**Changes**

- Add dedicated typed G2 builders for dominant-hand field 11 and gesture-list
  field 10. Keep the generic G2 settings allowlist restrictive.
- Encode the captured three-operation gesture list for display-off long press.
- After a dominant-hand ACK, refresh R1 `advStart` through WP1/WP3 using the
  same dual-MAC payload regardless of left/right selection. There is no R1 hand
  bit.
- Expose G2 observed/desired state through G2 settings UI, not R1 Health.

**Problems and downstream effects**

- Dominant-hand controls G2 behavior, while `advStart` controls R1-to-temple
  routing. Treating them as one atomic setting would report failure if the ring
  is offline even though the G2 change succeeded. Report the G2 result and R1
  routing refresh separately.
- Gesture-list SET replaces a structured list. A partial builder could erase
  other gestures; the captured full list must be preserved exactly unless a
  GET/merge format is proven.
- Hand or gesture changes should not broaden permissions for unrelated G2
  settings fields.

**Acceptance gate**

- G2 frames match official-app captures, left/right changes never alter an R1
  hand byte, and an offline R1 does not roll back a successful G2 setting.
- Long-press On/Off preserves all three captured operations.

**Rollback:** omit the optional UI/builders. No R1 health dependency relies on
this package.

## Cross-cutting cutover and release concerns

### Clean contract replacement

- Replace point-poll, cache, settings, JSON, CLI, OLED, web, and G2 consumers in
  one coordinated change. There is no transitional public API layer.
- Replace the old health JSON with one new contract and update every in-tree
  consumer at the same time.
- Rename `healthTrackingEnabled` to `healthLoggingEnabled` and `healthtrack` to
  `healthlogging` directly. Erased devices start from the new defaults.
- Add a cutover checklist: erase settings and capture storage, flash the new
  firmware, reprovision HardwareOne, reconnect G2/R1, then run acceptance. Do
  not erase the R1 ring or G2 unless a separately proven procedure requires it.

### Privacy and authorization

- Read-only status may remain available to authenticated users. Ring settings,
  force refresh, raw system SET, export of health history, and deletion of
  history require the repository's appropriate admin/permission checks.
- Sanitize diagnostics: firmware/hardware version and profile are useful; phone
  serial, ring serial, profile fields, raw MAC, and raw health pages are not
  suitable for routine logs.
- Apply the existing health-data at-rest policy to every new persistent file.

### Resource and performance budgets

- Measure owner queue high-water mark, ACK latency, heap/PSRAM delta, task stack
  high-water mark, maximum history file size, and G2 image contention.
- No notify callback allocation, filesystem operation, JSON building, or image
  push.
- Full-day activity is the sizing case: up to 144 logical ten-minute slots,
  separate from the 96-entry live series.

### Failure and recovery vocabulary

Every surface should use the same distinctions:

```text
offline -> connected -> identifying -> ready
accepted -> queued -> written -> acked -> verified
refused | not-supported | timed-out | disconnected | parse-error
fetch: idle | throttled | running | partial | empty | complete | cancelled
```

“Sent” alone is not a terminal success state.

### Internal integration order and rollback

These are development gates before the clean-cut firmware is installed; they
do not imply shipping compatibility code.

1. Land fixtures and replace all producers with the command owner.
2. Enable profile/setup diagnostics and packet ACK while settings remain
   Preserve and history UI stays hidden.
3. Enable read-only settings/history diagnostics.
4. Enable explicit health collection and manual history behind the exact
   2.2.7.0005 profile.
5. Enable durable storage and UI only after full-day activity transport and
   encryption/power-loss gates pass.
6. Enable low power; consider G2 parity separately.

The global emergency posture is `Unknown profile + desired Preserve + history
starts disabled`. It retains connection/read diagnostics without issuing new
settings writes or ingesting an unrecognized layout.

## Delivery sequence

### Phase 0 — Freeze the evidence and correct the contract

- Add sanitized golden fixtures for the August 3 settings and daily frames.
- Document the exact 2.2.7.0005 schemas and mark contradicted older assumptions
  as unsupported historical research, not code paths.
- Add exact parser expectations matching the official app's decoded values.
- Record the supported R1 firmware/profile matrix.

**Exit:** fixtures contain no device identifiers or user profile, and every new
write has a cited official-app observation.

### Phase 1 — Transaction and acknowledgement foundation

- Introduce the serialized control owner and transaction result model.
- Route setup, CLI queries, periodic point polling, and history work through it.
- Implement response matching, refusal/timeout/disconnect handling, and
  connection-generation protection.
- Implement `packetAck` for received daily data notifies.
- Harden `ringquery raw`; remove `report` from ordinary user-facing controls.

**Exit:** tests prove that queued is not reported as acknowledged, two callers
cannot race the encoder, duplicate notifies are harmless, and stale-link ACKs
cannot complete a current transaction.

### Phase 2 — Protocol profiles and settings cache

- Parse device-info early and select `Fw227_0005` or `Unknown`.
- Add the 2.2.7 dual-MAC `advStart` builder and use both temple getters.
- Make pairAuth, identity, time sync, and `advStart` one ACK-driven setup state
  machine; apply the shared timezone policy to setup and corrective sync.
- Add the proven low-power GET/response cache. Health collection has no proven
  readback and remains Unknown/ACKed-unverified.
- Add the health SET builder, but keep it test-only until transaction/ACK tests
  pass. Low-power SET remains deferred to Phase 6.

**Exit:** reconnect setup is ACK-driven; the emitted `advStart` matches the
selected profile; timezone behavior is recorded; low-power GET is observable;
health ACKed/unverified is represented honestly; unknown firmware fails closed.

### Phase 3 — Safe health-collection control

- Add Preserve/On/Off desired state with clean erased-device defaults.
- Expose ring health collection with desired/observed/pending/error state.
- Rename the local setting, command, and every surface from Health Track to
  Health logging without aliases.
- After a health-collection SET is ACKed, leave observed state Unknown and label
  the result ACKed-unverified. An explicit On choice may permit the normal
  history policy, but Preserve must not be interpreted as On and Off cancels it.

**Exit:** first connection causes no settings SET; explicit changes are ACKed,
read back where supported, survive reconnect as designed, and never touch the
profile.

### Phase 4 — Daily history and activity

- Implement profile-specific common and activity daily parsers.
- Implement the sequential history coordinator and ten-minute normal throttle.
- Capture a late-day/high-activity official-app fetch and resolve complete-page
  versus BLE-fragment/pagination behavior before claiming full-day support.
- Store typed records and expose partial/no-data/unsupported-layout states.

**Exit:** the official fixtures decode exactly, activity is no longer dropped,
all data notifies receive packet ACKs, and an empty sleep response does not
fail the whole sweep. Full-day activity is either proven complete or explicitly
labeled single-page/partial.

### Phase 5 — Durable local history, UI, and observability

- Add the v1 encrypted-at-rest daily store and separate history export.
- Replace the live health CSV with its clean new schema.
- Show observed versus desired setting state and pending/error status.
- Show the last fetch time and per-metric result.
- Present activity separately from vital signs.
- Make protocol profile, last transaction outcome, ACK latency, and parser
  rejection reason available in diagnostics without dumping personal history.

**Exit:** persistence is idempotent and power-loss safe; OLED, web, CLI, G2
lens, and JSON use the same state model and terminology; a freshly erased
device creates only the new settings, JSON, and storage contracts.

### Phase 6 — Low-power control

- Add low-power Preserve/Off/On using the proven GET/SET format.
- Measure notification, fetch, reconnect, and advertising behavior in both
  states before fixing timeouts.
- Serialize a low-power change against active history work and verify readback.

**Exit:** the first connection sends no SET; explicit changes match official
traffic, report ACKed versus verified honestly, and do not make fetch/reconnect
appear spuriously broken.

### Phase 7 — Optional G2 parity

- Add dedicated typed builders for G2 dominant-hand field 11 and gesture list
  field 10. Do not broaden the generic G2 settings allowlist.
- On dominant-hand change, update G2 first, then refresh the R1's profiled
  dual-MAC `advStart`. Do not invent an R1 hand field.
- Add the captured display-off long-press On/Off gesture lists.

This phase is useful, but it is independent of R1 health/history and should not
delay the core integration.

### Phase 8 — Hardware acceptance and documentation

- Compare HardwareOne frames with a fresh official-app capture on the same R1
  firmware, using sanitized diffs.
- Test reconnects, interrupted fetches, an unworn ring, low battery, empty
  history, duplicate notifies, and rapid UI actions.
- Run a 24-hour coexistence test with normal G2 and R1 traffic.
- Update [`R1_RING_PROTOCOL.md`](R1_RING_PROTOCOL.md),
  [`R1_RING_IMPLEMENTERS_BRIEF.md`](R1_RING_IMPLEMENTERS_BRIEF.md),
  [`DEVICE_SETTINGS_BACKLOG.md`](DEVICE_SETTINGS_BACKLOG.md),
  [`COMMAND_REFERENCE.md`](COMMAND_REFERENCE.md), and the relevant JSON
  contract.
- After first-party source changes, follow the private `docs2` review workflow:
  `docsctl.py update --changed`, review/accept every affected document, then
  `docsctl.py check`.

## Test matrix

### Codec tests

- Golden vectors: health Off/On, low-power Off/On, low-power GET response,
  2.2.7 dual-MAC `advStart`, and packet ACK.
- Golden parses: HR, HRV, SpO2, one- and two-record activity payloads.
- Bounds: zero records, truncated prefix, count overflow, incorrect width,
  arbitrary trailer preservation, `totalKcal < activeKcal`, extreme signed
  timezone, and payload from the wrong protocol profile.
- User-info: decode the known field widths, but assert that no production
  builder or UI can write it.

### Transaction tests

- ACK success, explicit refusal, timeout, disconnect, and reconnect generation.
- Data before command ACK, ACK without data, duplicate data, and late data.
- Central transport busy: queued versus actually written.
- Two simultaneous callers cannot allocate the same/out-of-order serial.
- Retry rebuilds a fresh frame and is restricted to approved idempotent intents.

### Build and hardware tests

- Build with Bluetooth/G2/R1 feature combinations enabled and disabled.
- Confirm Preserve emits no setting writes on a clean first connection.
- Confirm On/Off byte parity with the official app and setting persistence after
  ring and HardwareOne restarts.
- Confirm the history sequence and packet ACKs from a btsnoop capture.
- Confirm local timestamps and activity totals against the official app for the
  same capture window.
- Confirm no real MAC, phone serial, birthday, height, weight, or raw capture is
  added to tracked fixtures or `docs2`.

## Recommended first milestone

The smallest useful release is:

1. serialized R1 transactions plus packet ACK;
2. 2.2.7 firmware profile and dual-MAC setup;
3. health-collection Preserve/On/Off;
4. explicit history refresh;
5. correct HR/HRV/SpO2/activity parsing, with full-day activity completeness
   gated on a late-day capture;
6. versioned local storage/export that follows health-data encryption policy;
7. consistent status and controls across web, OLED, G2 lens, CLI, and JSON.

Low-power control follows immediately because its GET and SET semantics are
also proven. Profile writing, sleep/skin-temperature decoding, and optional G2
gesture parity remain deferred until their evidence and core dependencies are
complete.
