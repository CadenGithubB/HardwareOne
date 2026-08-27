# G2 / R1 2.2.9 compatibility remediation report

**Date:** 2026-08-22  
**Scope:** G2 glasses firmware 2.2.9, R1 ring firmware `2.2.9.0003`, Blocks hijack lifecycle, native hold-menu coexistence, and R1 health/history compatibility  
**Status:** implementation and final build completed; scoped documentation and final adversarial sign-off in progress, with connected-device hardware acceptance still required

## Executive decision

The ordinary ring tap commands were not renumbered. The update added native
long-press behavior, but the captured click, scroll, and double-click event
numbers used by the Blocks hijack are unchanged. The breakage instead spans
three independent compatibility boundaries:

1. The pre-change HardwareOne path established the two BLE/GATT links but did
   not perform the native G2 session-promotion sequence now emitted by the
   official app. On the updated glasses, that path repeatedly lost LEFT after roughly 23--24
   seconds and RIGHT a few seconds later with remote reason `0x13`. The stock
   app sends per-arm authentication, a RIGHT pipe-role command, time sync, and
   direct DevCfg command-14 traffic. This is strong evidence for a missing
   session ritual, but the available capture does not isolate which omitted
   command causes the disconnect.
2. The pre-change firmware decoded SID `0x0D` as a gesture/display-state
   payload. The new capture and app schema show that it is `SyncInfo`: a
   wrapper containing
   background and foreground application IDs. Native hold-menu and brightness
   transitions therefore entered the hijack's user-activity/display-off paths by
   mistake. The Blocks/List/Text/Sys input and teardown path is SID `0xE0`;
   SID `0x0D` is not a gesture channel. SID `0x01` also carries dashboard
   interaction/status traffic and is unchanged by this work.
3. The updated ring identifies itself as exact firmware `2.2.9.0003`.
   The pre-change firmware intentionally recognized only exact profile
   `2.2.7.0005`, so setup stopped at `profile-unknown`. The captured 2.2.9 setup
   and daily-health layouts are compatible with a subset of 2.2.7, but low-power and several
   other features were not exercised and must remain disabled rather than
   inheriting a blanket compatibility alias.

The implementation is therefore a narrow protocol compatibility patch,
not a tap remapping:

- it adds capture-exact DevCfg wire shapes and ACK correlation to a per-arm,
  generation-cancelled G2 promotion orchestrator, plus a best-effort per-arm
  DevCfg command-14 cadence. Cross-arm timing/order remains hardware-gated;
- it parses SID `0x0D` as app lifecycle, uses SID `0xE0` as the gesture/teardown
  authority, and makes real `SYSTEM_EXIT` invalidate all local container state;
- it adds an exact R1 2.2.9 runtime profile with feature capabilities, while
  mapping its proven daily pages to the existing persisted daily-layout ID;
- it preserves all unknown-version fail-closed behavior and all existing 2.2.7
  behavior.

## Evidence and confidence

This report uses four evidence classes:

- **Wire-proven:** seen in the updated official-app Bluetooth capture or in the
  updated device's HardwareOne serial log, with framing/CRC validation where
  applicable.
- **Source-proven:** established directly in the source version named by the
  claim. Baseline defects are explicitly marked pre-change; implementation
  outcomes refer to the final source.
- **Inferred:** the evidence is strongly correlated but does not uniquely prove
  causality or semantics.
- **Unproven:** absent from the capture; it must not be enabled by analogy.

At investigation start, the relevant private `docs2` subsystem pages were
stale according to `docs2/docsctl.py status`. They were used as discovery aids
only; every material claim below was rechecked against the pre-change or final
source named by the claim and sanitized capture output. The scoped pages
affected by this work are being reviewed against the frozen final source under
the repository's explicit acceptance workflow. No device address, health
value, account data, or raw user capture belongs in this report or in `docs2`.

### What the gesture capture proves

| Observation | Confidence | Consequence |
|---|---|---|
| Tetris still emits click `0`, scroll-top `1`, scroll-bottom `2`, and double-click `3` through the established event path. | Wire-proven | Do not renumber existing taps. |
| Selecting Blocks still uses menu command `17` with widget ID `10509`. | Wire-proven | Keep the existing Blocks trigger. |
| The updated app schema contains long-press `9` and release `10`. | Source/static-schema proven | These are additions, not shifts of `0..3`. Do not invent delivery to Blocks without a wire observation. |
| Holding in Blocks foregrounds a native contextual menu; selecting brightness changes native app context and sends brightness settings traffic. | Wire-proven | Treat the hold menu as an overlay owned by the glasses. It must not auto-exit or rebuild the hijacked page. |
| A later dismissal sequence contains a double-click, the phone's native shutdown command, system-close foreground state, `SYSTEM_EXIT`, and then no-active-app state. | Wire-proven | SID `0xE0 SYSTEM_EXIT` is the teardown authority; the capture does not prove the double-click alone independently creates each later step. |

The captures do **not** prove that long-press `9` or release `10` are forwarded
to the third-party Blocks surface. The firmware appears to consume the hold to
open its own menu. Supporting a HardwareOne action on hold is therefore out of
scope; coexistence with the native menu is the required behavior.

### What the G2 connection captures prove

| Observation | Confidence | Consequence |
|---|---|---|
| HardwareOne repeatedly reaches GATT-ready state, then LEFT drops around 23--24 seconds and RIGHT around 26--27 seconds, normally with remote reason `0x13`. | Wire-proven | This is not a scan, pairing, or tap-enumeration failure. |
| The pre-change HardwareOne path sent the legacy fixed AppLaunch prelude and an optional RIGHT time sync, but no per-arm DevCfg authentication or RIGHT pipe-role promotion. | Source-proven | That path differed materially from the official app. |
| The official 2.2.9 app sends AUTH to LEFT and RIGHT, RIGHT pipe-role, RIGHT time sync, then direct DevCfg command `14` to both arms. | Wire-proven | Mirror the observed sequence and wire shape. |
| Official AUTH and command `14` use envelope flag `0x00`; RIGHT role and time use request flag `0x20`. Every observed outbound operation uses the same fresh byte for envelope sequence and protobuf magic. | Wire-proven | The pre-change AUTH/heartbeat builders were not capture-exact and fixed magic values are unsafe for correlation. |
| Direct DevCfg command-14 acknowledgements return on the arm that received the write. EvenCore heartbeat acknowledgements for both arm writes are routed through RIGHT on current firmware. | Wire-proven | DevCfg ACK tracking must be per arm; the existing LEFT EvenCore miss policy must not be described as a generally silent LEFT notify channel. |
| The combined settings snapshot carries LEFT software version in field 5 and RIGHT software version in field 6. The pre-change parser read only field 5 and assigned it to whichever callback received the snapshot. Both length-delimited values in the captured frame are exactly `2.2.9.22`; a raw ASCII scan misleadingly appends the following protobuf tags (`0x32`/`0x38`) and makes them look like `.222`/`.228`. | Wire/source-proven | Parse and publish both arm fields independently, but do not infer asymmetric firmware or reply routing from bytes outside either string's declared length. |
| The pre-change code treated any inbound envelope as proof that the EvenCore rendering plugin recovered. | Source-proven bug | Periodic DevCfg ACKs would mask the plugin watchdog unless recovery is restricted to a validated SID-`0xE0` command-12 ACK. |
| Long gaps in command-14 traffic occur while the official app remains linked. | Wire-proven | Do not claim or implement a strict two-miss/30-second disconnect lease. |
| Omitting the entire promotion sequence correlates with HardwareOne's repeatable disconnects. | Inferred | Promotion is the leading remediation, but hardware validation must identify whether it fixes the drops. |

The legacy AppLaunch prelude was not observed in the new stock capture. It is
an existing HardwareOne rendering dependency with prior local evidence, so it
was retained after native promotion and still requires 2.2.9 hardware
validation. The report
must not call it part of the updated official sequence or claim this capture
independently proves its necessity.

### What SID `0x0D` proves

The pre-change `classifyStateEvent()` treated byte patterns on SID `0x0D` as
`USER_ACTIVITY` or `DISPLAY_OFF`. The captured payload instead has the
protobuf structure:

```text
SyncInfoWrapper {
  command
  magic
  data {
    background_app_id
    foreground_app_id
  }
}
```

Observed, sanitized app-state transitions include:

- background `224`, foreground `3`: native menu foregrounded;
- background `224`, foreground `273`: native brightness surface
  (the name is inferred from its timing beside brightness changes);
- background `224`, foreground `0`: native overlay dismissed and EvenHub
  background context resumed;
- background `0`, foreground `0`: no active app after native teardown;
- background `4094`: transition/sentinel context seen around app boundaries.

Only the numeric transitions and correlations are wire-proven. Names for
undocumented IDs must remain diagnostic labels, and unknown nonzero foreground
IDs must be handled conservatively as native overlays.

Command-1 present-empty data is effectively background/foreground `0/0`, but
it is transient during normal launches and app switches as well as present
after teardown. It is therefore neither a physical display-off signal nor a
terminal event by itself.

### What the R1 2.2.9 capture proves

The updated ring reports exact firmware `2.2.9.0003` and hardware
`603MV1.9.3`. The following 2.2.9 operations use the same validated layout as
their 2.2.7 counterparts:

- pair authentication, device information, system time, and device status;
- dual-temple-MAC advertising start;
- health-collection SET with `enabled=1`;
- single-frame daily heart rate, HRV, SpO2, and activity data pages;
- a sleep command ACK followed by no data during the bounded response window;
- the 10-byte daily packet acknowledgement for captured heart-rate, HRV, SpO2,
  and activity pages.

The captured daily pages satisfy the current CRC32, envelope, command, length,
slot uniqueness/range, timestamp-mode, and value-range checks when only the
exact-profile gate is removed. Their day bases classify as anchored epoch
boundaries under the captured signed timezone, so these particular pages also
meet the stricter history-ingress identity gate. Their final word remains
opaque metadata. A value of `6` occurred in the new capture, while older 2.2.7
sessions contain multiple values; it is not a firmware discriminator.

Protocol validity and history identity are deliberately different gates. The
daily codec also returns `R1_PARSE_OK` for structurally valid zero-base or
unknown-day pages, preserving their raw fields and slots with zero bucket
epochs. This is established 2.2.7 behavior: such a page may be packet-ACKed for
ring flow control and may satisfy the data half of a command transaction, but
it cannot enter immediate trends, the typed history key, or persistence. For a
nonempty daily metric, transport `VERIFIED` therefore means that its command
ACK and structurally valid response were both observed; it is not a claim that
the response was persistable. The updated 2.2.9 capture did not exercise an
unanchored day, so no new rebasing or stricter peer-flow inference is made.

The following are unproven on 2.2.9 and must stay disabled or diagnostic-only:

- system-settings/low-power GET, SET, and typed status parsing;
- dedicated wear-status and typed user-info semantics;
- point/measure health queries and typed ingestion;
- health-collection SET with `enabled=0`;
- a sleep-data packet acknowledgement (the captured sleep result was empty and
  did not exercise that ACK target);
- live multi-fragment activity behavior.
- temperature-daily query/data behavior.

The ring remained linked for more than ten minutes without a phone-originated
R1 heartbeat. No new R1 heartbeat requirement should be invented.

This evidence came from hardware revision `603MV1.9.3`. Selecting by the exact
firmware string reasonably assumes another hardware revision reporting
`2.2.9.0003` shares these layouts, but that cross-hardware assumption is
inferred, not demonstrated by this capture.

## Required implementation invariants

1. BLE notification callbacks remain copy-only. The existing G2 control owner
   remains the sole decoder of command-channel RX.
2. The G2 control owner never waits for an acknowledgement that only it can
   decode. Bounded waits may run on the connect worker while the control owner
   continues draining RX.
3. A DevCfg acknowledgement can complete a transaction only when RX flag `0x00`,
   side, connection generation, command, fresh magic, expected nested-field
   identity, and empty ACK body all match. A same-command unsolicited status
   frame, stale semaphore token, or late prior-generation ACK cannot promote
   the session. Envelope RX sequence is not an ACK correlation key.
4. A native promotion command never races the adaptive time-sync tick or the
   ordinary EvenCore heartbeat path for an unpromoted arm.
5. Failure of a required promotion stage leaves the arm not ready and follows
   the existing generation-fenced disconnect/reconnect path. Invalid host time
   skips time sync; it does not turn a valid authentication into a failure.
6. DevCfg command `14` is compatibility traffic, not a new local health
   watchdog. A missed ACK is diagnostic and retried later; it does not force a
   disconnect.
7. DevCfg/native-session RX and EvenCore/plugin health remain independent.
   Only a validated SID-`0xE0` command-12 ACK clears the plugin miss/dead state.
8. SID `0x0D` never synthesizes a tap, user-activity callback, or display-off
   action. SID `0xE0` remains authoritative for gestures and `SYSTEM_EXIT`.
9. Unknown app IDs remain safe: an unknown foreground app may suppress custom
   input while present, but cannot delete content or start a new app.
10. SyncInfo state reduction is side- and generation-bound, idempotent, and
    presence-aware. A command-0 request/response, absent data, duplicate mirror,
    or stale generation cannot mutate unsolicited app state.
11. R1 runtime firmware identity remains exact. `2.2.9.0003` is distinguishable
   from `2.2.7.0005`; near matches and all other strings remain unknown.
12. R1 capabilities are per feature and, where evidence is directional, per
   operation/value. Enabling proven 2.2.9 daily traffic must not enable
   unobserved low-power, user-info, wear, point/measure, health-collection OFF,
   or sleep-data-ACK paths.
13. Persisted history keys represent a wire-layout family, not the exact
   runtime firmware. A same-day update or downgrade must reuse the existing
   compatible file without a reload loop or data loss.
14. Existing enum values, serialized history layout ID `1`, paths,
   content-hash algorithm, and 2.2.7 wire/profile behavior remain stable. Hash
   values still change when content changes; the deliberate merge-by-slot fix
   changes old whole-metric replacement behavior to prevent sparse-page loss.
15. Older G2 compatibility is negotiated rather than assumed: attempt the
    2.2.9 capture-exact AUTH flag `0x00`; on bounded no-ACK, make one legacy
    flag-`0x20` attempt with a fresh token and retain the successful AUTH/base
    heartbeat shape for that connection generation. If older hardware cannot
    be tested, preservation is a release uncertainty rather than a proven claim.

## Approved source-change plan (historical design record)

### 1. Capture-exact G2 DevCfg model and tests

Files: `System_G2_Protocol.h`, `System_G2_Protocol.cpp`

- Add a transport-free DevCfg RX parser that reports command/magic presence,
  nested-body presence, and parse validity without logging or touching BLE.
- Add capture-exact promotion/command-14 encoding. AUTH and direct base
  heartbeat must use flag `0x00`; role/time retain flag `0x20`. For every
  production DevCfg operation, the caller supplies one fresh nonzero token used
  for both envelope sequence and protobuf magic. HardwareOne may retain its
  safer skip-zero allocator even though the stock stream wraps through zero;
  zero is an internal reassembly sentinel here and no evidence requires it.
- Preserve explicit builders/CLI behavior where compatibility matters rather
  than silently changing unrelated experimental commands.
- Extend `g2ProtocolGoldenSelfTest()` with sanitized vectors for AUTH, role,
  time, command `14`, ACK parsing, malformed/truncated protobuf, unknown fields,
  and magic values wider than one byte. No live address or user payload is used.

### 2. Generation-safe G2 session promotion

File: `G2_Glasses.cpp`

- Add per-temple native-session state: promotion phase, promoted generation,
  last command-14 TX/ACK timestamps, and next due time. Reset it before every
  subscribe attempt and on every disconnect/generation advance.
- Extend SID `0x80` handling so the control owner publishes matching DevCfg
  replies into a small fixed transaction slot per temple. Match RX flag `0x00`,
  exact side, generation, command, fresh magic, expected nested field, and
  empty ACK body.
  Drain/arm its semaphore before sending. Do not reuse the current fixed AUTH
  magic because the capture contains nonempty unsolicited AUTH status traffic
  that can collide. Wait in short slices, aborting on cancellation or generation
  change, and never hold a TX gate, write mutex, or critical section while
  waiting.
- On the connect worker, after notify registration:

  1. authenticate the newly connected arm with the capture-exact wire shape;
  2. when RIGHT is available, set RIGHT pipe role;
  3. send RIGHT time only when the host clock is valid;
  4. retain the legacy AppLaunch prelude needed by custom rendering;
  5. schedule the first direct command `14` approximately 15 seconds after
     promotion rather than adding an unobserved immediate seed.

- Required AUTH and RIGHT role stages use bounded retries and matching empty
  receipt ACKs. They prove protocol receipt, not an explicit semantic
  success result. A later same-generation `secAuth=1` status may be logged
  during a short settle, but it is not magic-correlated and never satisfies the
  transaction. The control owner stays runnable during waits. Time sync is
  retryable through the existing clock-custody tick. Command `14` is best
  effort after each successful generation-fenced write because its lease
  semantics are unproven.
- Permit only one blocking DevCfg transaction per arm. Drain stale semaphore
  state, publish the expected identity, then arm before generation-fenced TX.
  Send failure, timeout, disconnect, cancellation, and retry disarm atomically;
  every retry uses a fresh token. Concurrent CLI requests serialize or return
  Busy. Check cancellation/generation every 25--50 ms so explicit disconnect
  stays within the existing teardown bound.
- Bind time-custody bookkeeping to the promoted connection generation.
  Connect-time time sync records “pushed” only after its matching ACK. The first
  promoted main-loop tick preserves that record instead of zeroing it and
  duplicating the write. If connect-time sync is skipped or unacknowledged, the
  post-ready loop uses a generation-fenced send with no semantic-ACK wait and
  records GATT admission rather than a semantic ACK; later timezone/clock drift
  re-arms it. It uses a short central-gate admission timeout but still performs
  one synchronous `writeValue` call on the main-loop task. Runtime time custody
  is weaker than promotion-time correlation and must not be cited as proof that
  the glasses applied the update. Native time and command `14` are not gated on
  EvenCore `pluginDead`.
- Gate the existing EvenCore heartbeat, adaptive time-sync tick, microphone
  reassert, policy/config reconciliation, render operations, and DevCfg CLI
  paths on the current generation's
  promotion state. Repair can leave the timer running while `connected` is
  published early, so checking only physical transport state is insufficient.
  Mark final session readiness only after required promotion and the retained
  AppLaunch settle complete.
- Schedule direct command `14` independently per connected/promoted arm at the
  observed nominal 15-second cadence, with skip-and-retry behavior when the
  shared central TX gate is busy. Preserve ordinary EvenCore watchdog
  scheduling; do not add a missed-command-14 disconnect policy or assume one
  native heartbeat plane is higher priority than the other.
- Keep the currently unregistered Ring-bridge command-14 task dormant; if that
  feature is revived, route it through the common scheduler instead of starting
  another task. Two independent senders with different wire shapes/fixed magic
  would create needless traffic and ambiguous ACKs.
- Remove the generic “any inbound envelope means plugin alive” recovery in
  `g2DispatchCompleteEnvelope()`. Clear EvenCore miss/dead state and post WORN
  only from a validated SID-`0xE0` command-12 ACK. A DevCfg command-14 ACK is
  native-session telemetry and must not heal or flap the rendering watchdog.
- Rename/narrow `firmwareSilencesLeftNotify()` to the behavior it actually
  protects: current firmware routes ordinary EvenCore heartbeat ACKs through
  RIGHT. Direct DevCfg per-arm ACKs must still be accepted on LEFT. Prefer
  observed ACK routing over a broad version wildcard. Add a dual-version
  settings parser and publish field-5 LEFT and field-6 RIGHT versions to their
  proper temple records. The captured pair reports `2.2.9.22` on both fields;
  preserve their independent presence/generation even though the values happen
  to match. If an exact routing table remains necessary, key it to the decoded
  pair plus separately observed packet direction, retain fail-visible
  diagnostics for unknown pairs, and never derive it from raw ASCII adjacency.
  Retire aggregate `gFwVersion` from compatibility decisions. Use an exact
  `firmwareRoutesEvenHubRepliesToRight()`-style predicate only for EvenHub
  SID-`0xE0`/create/image routing and LEFT E0 miss exemption; never apply it to
  direct SID-`0x80` ACKs, which bind to their callback arm and generation.
- Before changing pre-version AUTH/base traffic for older glasses, negotiate
  one bounded legacy flag-`0x20` fallback after a failed flag-`0x00` attempt and
  remember the successful shape for that generation. Older supported hardware
  remains a required regression target.

### 3. Parse SyncInfo as lifecycle, not gesture input

Files: `System_G2_Protocol.h`, `System_G2_Protocol.cpp`, `G2_Glasses.h`,
`G2_Glasses.cpp`, `G2_Page_Settings.cpp`, `G2_Page_TestSuite.cpp`,
`docs/G2_PROTOCOL.md`, and `docs/G2_MIC_SOURCE_PLAN.md`

- Rename the canonical constant to `G2_SID_SYNC_INFO`, retaining the old
  `G2_SID_STATE_EVENT` name as a deprecated numeric alias for source/API
  compatibility.
- Add a bounded protobuf parser for SID `0x0D` with presence bits for wrapper
  command, magic, data, background ID, and foreground ID. Preserve absent data
  versus present-empty data; decode signed int32 values without using them as
  array indices. Accept field reordering, unknown fields, singular last-wins,
  and repeated-data merge semantics. A known field with the wrong wire type or
  malformed/truncated input rejects the whole packet without mutating state.
- Replace `classifyStateEvent()`/`dispatchEventPayload()` behavior for this SID
  with a generation-bound SyncInfo lifecycle handler. Only command `1` on the
  notify path may mutate unsolicited state. Command-0 requests/responses are
  correlation traffic; the captured response omits the default command, so a
  matched response cannot require command presence. No SyncInfo packet invokes
  `gEventCallback`, text-view auto-exit, or legacy display-off shutdown.
- Keep a raw per-temple snapshot for diagnostics. RIGHT/display-pipe state is
  authoritative while RIGHT is connected; LEFT is fallback only while RIGHT is
  absent. Replace temporal raw-byte deduplication with idempotent state
  reduction so mirrors/duplicates do not refresh timers or fire actions.
- Track foreground/background IDs and update the native-overlay gate:

  - a nonzero foreground under stable background `224` is a native overlay,
    regardless of whether the ID is known (including new `273`);
  - foreground `0` under background `224` marks overlay dismissal and starts
    the existing short exit grace;
  - foreground-only snapshots may update/repair foreground overlay state while
    retaining stable background `224`, but only when the same current generation
    already established EvenHub ownership; otherwise they are diagnostic and
    cannot inherit a stale prior hijack;
  - transition/sentinel background `4094` and unknown IDs are logged and
    handled conservatively; `4094` alone neither replaces stable context nor
    tears down content. For observed `4094/foreground-34`, retain the prior
    stable background; if that prior context is the current EvenHub hijack, the
    nonzero foreground may still suppress custom work;
  - background/foreground `0/0` means no app IDs were reported, not physical
    display-off, and never directly synthesizes a gesture/shutdown.

- Keep SID `0xE0` event types `0..3` as gesture input, `4/5` as native
  foreground boundaries, and `7 SYSTEM_EXIT` as the teardown authority.
- Route E0 and SyncInfo overlay observations through one edge helper. Redundant
  E0+SyncInfo confirmations must not extend `upSince`, `lastExit`, or grace.
  E0 foreground exit immediately clears older SyncInfo evidence. A
  SyncInfo-confirmed nonzero foreground does not expire under the current
  30-second E0-only optimism cap; it remains suppressive until E0 exit, a newer
  authoritative SyncInfo state, or lifecycle reset.
  Reset overlay/app IDs on `SYSTEM_EXIT`, RIGHT disconnect/generation advance,
  a fresh Blocks startup/hijack generation, and terminal fallback.
- On a terminal, current-generation, non-echo `SYSTEM_EXIT`, invalidate
  per-arm container-ready, text-view, presentation/lens, and page-swap cache
  state even when the hijack FSM is not active. Emit a hijack exit only when
  the FSM actually owns one. This prevents removal of the bogus SID-`0x0D`
  display-off path from leaving a CLI-created non-hijack container falsely
  marked ready.
- Preserve the existing expected-echo rule: a delayed reason-1 `SYSTEM_EXIT`
  produced by HardwareOne's own page-swap shutdown must not destroy the
  replacement CREATE. Overlay state is still reset, but terminal invalidation
  occurs only when the event is outside the generation/expected-echo window.
  Centralize and make the terminal reset idempotent: stop live text/page workers;
  clear text callbacks, keyboard ownership, both container-ready flags, list
  and page-swap caches; advance presentation/queued-input epochs; clear lens and
  fused overlay state; abort/hold image probes as appropriate; and emit exactly
  one hijack exit only when an owned hijack is actually ending.
- Preserve a dropped-`SYSTEM_EXIT` fallback without equating empty SyncInfo to
  display-off. If authoritative state was stably background `224` with a live
  container and then becomes empty or another stable background, start a
  300--500 ms generation-bound absence debounce. Cancel it on return to `224`
  or transition `4094`, and suppress it during an expected page swap/shutdown
  echo. Expiry calls one centralized widget-gone invalidation path. E0 remains
  immediate, and E0 plus later empty state must produce exactly one teardown.
- Correct semantically wrong SID-`0x0D` comments in the public header, settings
  page, test-suite page, and protocol documentation without changing public
  enum values or removing `g2SetEventCallback()`.

### 4. Exact R1 2.2.9 profile with capabilities

Files: `System_R1_Protocol.h`, `System_R1_Protocol.cpp`, `G2_Ring.h`,
`G2_Ring.cpp`, and `WebPage_R1_Health.cpp`

- Append `R1_PROFILE_FW_2_2_9_0003 = 2`; preserve profile `1` for
  `2.2.7.0005`. Select it only for the exact firmware string. Add the matching
  public G2 Ring profile value/name and a compile-time mirror check where
  possible.
- Replace the blanket known-profile predicate with explicit capabilities, at
  minimum:

  - advertising-start layout;
  - health-collection ON and OFF operations;
  - per-command single-frame daily-v1 query/parse layout;
  - activity reassembly-v1 layout;
  - per-metric daily packet ACK targets;
  - low-power/system-settings layout;
  - typed user-info layout;
  - point/measure query and typed-ingestion layout;
  - wear-status layout.

- For exact 2.2.9, enable advertising start, health-collection ON, single-frame
  daily queries/parsers for captured HR/HRV/SpO2/activity plus the observed
  no-data sleep transaction, and packet ACK only for captured
  HR/HRV/SpO2/activity targets. Keep health-collection OFF, temperature daily,
  activity reassembly, low-power, user-info, wear, point/measure, positive sleep
  data, and sleep-data ACK 2.2.7-only. Generic pair-auth, device-info,
  system-time, and device-status paths remain profile-independent as they are
  today.
- Make low-power query take a profile and fail before consuming a serial on an
  unsupported profile, matching the existing fail-closed SET/parser behavior.
- In Ring control reconciliation, do not enqueue or retry automatic low-power
  GET/SET for 2.2.9. Mark the requested feature unsupported with an appended
  stable error value; never renumber existing API/status errors. Persisted
  On/Off intent must not turn into an unproven write after reconnect.
- Set exact unsupported state on a known 2.2.9 link: pending false, observed
  unknown, transaction handle zero, local feature-unsupported error, desired
  preference retained, and no queue/serial/TX. Setter and refresh APIs must
  converge on that same status/no-wire state on every owner lap. Their boolean
  results intentionally differ: the setter may accept and save the preference,
  while refresh returns false because it queued no query. On a later 2.2.7
  reconnect, clear the unsupported result and resume normal GET/SET
  reconciliation. Update the health web endpoint so it reports unsupported
  instead of falsely saying “awaiting verified readback” or “refresh queued.”
- Gate every point producer and consumer, not only explicit commands. This
  includes dark-clock setup/tick probing, Poll Now/logging, ring-to-glasses
  spoof refresh, and unsolicited typed point ingestion. A 2.2.9 ring connected
  while the host clock is dark cannot use the unproven point path as a clock
  source; setup must wait for/fail with clock unavailable rather than sending a
  speculative query.
- Allow only validated, captured single-frame daily-v1 parsers and ACK targets
  for 2.2.9. A synthetic whole-model test proves decoder correctness but cannot
  prove the updated ring's fragment transport, so 2.2.9 activity reassembly
  remains disabled until a live fragmented capture exists. The old 2.2.7 path
  and its tests remain unchanged.
- Move packet-ACK eligibility behind successful typed payload validation for
  2.2.9. The current single-frame lane mints/queues an ACK after only generic
  frame/opcode validation, before the metric parser runs; a CRC-valid but
  malformed/unsupported page could therefore be ACKed and then discarded,
  preventing the ring from retrying it. HR/HRV/SpO2/activity ACK only after the
  corresponding typed parser succeeds. Parser success here is structural wire
  validity, not persistence admission: a valid zero-base/unknown-day page keeps
  the established flow-control ACK and transaction semantics but is rejected
  by the later anchored-day history gate. Empty sleep has no data ACK; positive
  sleep data and fragmented activity remain separately gated until their exact
  policy is proven.

### 5. Normalize the compatible daily-history layout

Files: `G2_Health.cpp`, `R1_HealthHistoryStore.h`,
`R1_HealthHistoryStore.cpp`

The history filename is keyed by peer/day/timezone but omits profile. The loaded
in-memory key currently compares the exact profile. If 2.2.9 profile value `2`
were written directly, an existing same-day profile-1 file would load from the
same filename, retain value `1`, fail the following exact-key comparison, and
be reloaded repeatedly without merging new data.

The safe design is to separate exact runtime identity from the persisted
daily-layout family:

- validate decoded results with `daily-v1` capability;
- introduce a distinct history-layout type/name and normalize both exact 2.2.7
  and 2.2.9 profiles to existing daily-v1 layout ID `1` before forming the
  pending/store key; unknown profiles have no layout;
- keep decoded results and Ring status on their exact runtime profile;
- keep on-disk schema, filename, content-hash algorithm, and numeric layout ID
  unchanged; stored raw ID `2` remains invalid. The actual hash value must
  change when merged content or metadata changes;
- document that the existing stored `protocolProfile` field is a wire-layout
  identifier (renaming it in source is optional and must not alter storage);
- Change common and HRV daily application from whole-metric replacement to
  merge-by-slot, matching the already sparse-merge activity behavior. Preserve
  unrelated valid buckets, overwrite only slots present in the validated page,
  recompute count/have, and update latest metadata only when the incoming
  timestamp is at least as new. Without this downstream change, a smaller
  post-ACK delta page can erase previously persisted hourly buckets even when
  the layout key is correct.

This supports a same-day firmware update and later downgrade without migration,
file duplication, or loss caused by either the layout-key transition or sparse
delta replacement.

## Downstream impact analysis

### Connection and task ownership

- Promotion ACK waits add bounded connect latency. They run on the serialized
  connect worker, not `g2_ctrl_owner`, and must honor generation cancellation.
- The RX parser needs no new task, queue, or unbounded allocation. Per-temple
  state and transaction slots are fixed-size.
- Publishing `connected` before promotion is currently visible to public
  predicates and the main loop, not merely an internal transport fact. Audit
  every consumer and introduce a generation-bound ready predicate where native
  readiness matters. External link-up completion and automated rendering,
  time, policy, mic, or heartbeat work must not proceed until required
  promotion succeeds.
- Direct command-14 scheduling must share the existing per-temple write mutex
  and process-wide central TX gate. Busy is a skipped opportunity, not a miss.

### Hijack and native UI coexistence

- Native hold/brightness menus no longer masquerade as taps, so text-view
  auto-exit and hijack shutdown should not fire underneath them.
- Overlay dismissal should expose the existing custom content without a CREATE
  race. No content rebuild is triggered solely by an app-ID transition.
- Unconditional local invalidation on real `SYSTEM_EXIT` affects non-hijack
  CLI text/image flows beneficially: their next display must CREATE instead of
  incorrectly REBUILDing a firmware-destroyed container.
- Public `G2EventType` numeric values remain unchanged for API compatibility,
  even if SID `0x0D` no longer produces the legacy generic events.

### Ring setup, settings, and API status

- R1 setup should reach READY on 2.2.9 when the host already has valid time (or
  another independently proven clock source), because its captured
  setup primitives are supported. The official capture proves the individual
  operations, not HardwareOne's exact auth->info->time->adv ordering, so READY
  remains a hardware acceptance result. With a dark host, 2.2.9 must wait or
  fail clock-unavailable rather than using the unproven point query that
  currently solicits ring time.
- Health collection and daily history become available. Low-power controls
  should report unsupported instead of timing out or silently appearing to
  work. Web/status consumers receive the exact new profile name through the
  existing public profile mapping.
- Existing saved low-power preference remains stored for a future supported
  profile but is not transmitted on 2.2.9.
- Unknown ring firmware remains diagnostic-only; no shape-based fallback is
  introduced.

### Persistence and downgrade behavior

- Existing layout-1 files remain readable with no schema migration.
- A 2.2.9 fetch merges into the same compatible peer/day/timezone model once,
  not a reload loop. A later 2.2.7 session sees the same layout.
- Exact firmware remains visible in live Ring status; history summaries expose
  the layout-family value already stored, avoiding a false claim that old
  records were collected by one exact firmware.

### Diagnostics and privacy

- Add phase/command/side/generation logs without printing addresses, auth data,
  or health values.
- Distinguish `transport-connected`, `native-promoted`, and `render-ready` in
  debug output so another disconnect can be attributed to an exact stage.
- Do not copy raw captures into source, tests, `docs2`, or this report.

## Implementation outcome

The source work described above is complete. The implementation remained a
protocol/lifecycle compatibility change; it did not renumber tap events or add
a synthetic long-hold gesture to Blocks.

### G2 glasses

- Added typed DevCfg builders/parsers and capture-derived golden self-tests for
  AUTH, pipe role, time, direct command `14`, strict ACK bodies, SyncInfo, and
  malformed/truncated inputs.
- Added generation-cancelled per-arm native promotion: capture-exact flag-`0x00`
  AUTH with one legacy flag fallback, required RIGHT pipe role, optional
  ACK-correlated RIGHT time, the existing AppLaunch prelude, and readiness
  publication only after the required stages succeed. Automated native work is
  gated on that ready generation. Direct command `14` is per-arm, best effort,
  and does not disconnect on a miss.
- Replaced the SID-`0x0D` gesture interpretation with a presence-aware SyncInfo
  reducer. RIGHT is authoritative while present; LEFT is fallback. Native
  foreground overlays suppress competing custom refresh, and stable absence is
  debounced. Canonical SID-`0xE0` terminal events remain the teardown authority;
  only the exact owned reason-1 Shutdown echo is exempt.
- Hardened lifecycle concurrency found during adversarial review: exact
  generation/lifecycle/presentation RX stamps, pair-wide CREATE lease and ACK
  ownership, rotating CREATE tokens, ambiguous-failure quarantine and exact
  cleanup, callback-slot serials, terminal Cmd9-before-replacement-CREATE
  ordering, post-CREATE image fences, and callback-free terminal text-entry
  tombstone cleanup on the FIFO tap worker. An obsolete cleanup now rechecks
  its lifecycle beside every physical ATT write and again before clearing the
  authoritative per-arm mirror. Text-entry startup, indexed taps, rowless
  double selection, dictation mutation, and destructive cleanup now share a
  nonblocking exact-session execution lease, so terminal teardown cannot free
  pad buffers beneath an off-worker startup or an in-flight tap-worker action.
  Indexed routing first acquires a sentinel-capable route lease, including when
  no text-entry session is yet published; a tap is dropped while an off-worker
  startup/replacement owns that lease or while local state is tombstoned.
  Indexed taps capture the exact session's secret policy while holding the
  lease, recheck their queued physical-presentation epoch under it, and retain
  it across redacted logging and recursive mutation; an old queued tap cannot
  bind to a completed off-tap replacement. Lock-free active/secret queries now
  include the secret bit in the full published-identity snapshot before exact
  coordinator validation, removing both the plain-field replacement race and
  the query/log redaction TOCTOU.
- Connect-time time custody is ACK-correlated. The later main-loop retry waits
  for no semantic ACK and records local GATT acceptance, not peer confirmation;
  it may still spend the short central-gate admission timeout plus one
  synchronous `writeValue` call. Clock drift, timezone change, or a new
  generation re-arms it.

### R1 ring and consumers

- Added exact runtime profile `2.2.9.0003` with operation-level capabilities.
  Only capture-proven setup and daily operations are enabled; low-power,
  wear/point/measure, health-OFF, temperature daily, positive sleep data/ACK,
  and fragmented activity remain fail-closed.
- Made daily completion accept either captured ACK/data order, start its bounded
  data window from the first matching ACK, and refuse duplicate-ACK deadline
  extension. A daily packet becomes ACK-eligible only after its metric-specific
  typed parser succeeds. That protocol-valid result may complete the transport
  transaction even when its day is unanchored; only an epoch-anchored result is
  admitted to trends/history/persistence. A corrupt positive sleep candidate
  prevents a false EMPTY result but is never ACKed, stored, or reported as
  success.
- Added connection-generation fencing and mutex protection around telemetry
  cache reset/publication. Reset failure aborts setup before `connected=true`,
  and an old frame can neither repopulate nor clear a replacement generation.
  The public getter now copies cache provenance plus telemetry generation,
  link generation/online state, and exact profile under the established
  telemetry-to-transport lock order. It rejects an old direct cache paired
  with a reconnect or disconnected link instead of relabeling it current.
  Sid-`0x90` temple-forwarded telemetry remains intentionally available with
  `connected=false`; explicit provenance transitions atomically clear both the
  value cache and `G2_Health` live series, and setup reasserts clean direct
  ownership before publishing the new generation online. Forwarded frames are
  ignored while the direct R1 link is online. Conversely, direct extraction
  now requires that its RX generation equal both the prepared telemetry and an
  online transport generation, and uses the profile copied with that identity;
  an already-admitted old direct frame cannot erase newer offline forwarded
  values or graph samples. The health-render lock order was also changed to
  remove the telemetry/series inversion found by the audit.
- Made activity reassembly state owner-task-only and fenced finalization with
  telemetry custody plus exact online generation/profile revalidation before
  parsing, before history application, and before ACK/transaction completion.
  Setup publishes the history peer under the same telemetry fence, so a stale
  fully assembled model cannot cross a reconnect and enter another peer's
  history. Deferred history-peer selection is now one HistoryLock-linearized
  current/requested/revision state: reasserting current peer A cancels an older
  deferred B; while a different peer remains requested, new history-key
  ingestion is rejected. A dirty cross-peer switch stays deferred until the
  store reports the exact dirty generation committed; that revalidation,
  dirty clearing, peer selection, and RAM reset occur under the same lock, so B
  can install only while it remains the latest requested peer.
- Kept exact runtime profile identity separate from persisted compatible layout
  ID `1`: both supported runtime profiles are mapped to layout `1` before
  history key/load/ingest, while an on-disk raw layout/profile ID `2` remains
  invalid rather than being migrated. Same-day history updates now use sparse
  slot merges so smaller delta pages do not erase existing buckets. OLED, G2,
  web, sensor-log, and history refresh paths now honor the same capability
  gates.

### Verification completed so far

- Focused compiles passed for every changed G2/R1 translation unit used by this
  compatibility slice.
- The full configured FeatherS3 build completed successfully and produced
  `build-feathers3/hardwareone-idf.bin` (size `0x5767f0`; `0x9e810`, or 10%,
  remained in the smallest app partition). Existing unrelated compiler
  warnings remain; this change introduced no build error.
- Scoped `git diff --check` passed. Protocol/self-test code compiled into the
  image, but boot-time self-tests have not been executed on hardware.
- No firmware was flashed and none of the connected-device acceptance steps
  below has been claimed as passing.

## Verification plan

### Static and boot self-tests

1. G2 exact TX and ACK golden bytes for AUTH, role, time, and command `14`,
   including envelope flags, nested bodies, CRCs, unrelated RX envelope
   sequence, `seq == magic`, the official token-zero fixture, and HardwareOne's
   documented skip-zero production policy.
2. DevCfg ACK parser/transaction: matching and wrong RX flag/side/generation/
   command/magic/body; same-magic nonempty unsolicited AUTH; malformed/truncated
   protobuf; unknown fields; stale semaphore; ACK-after-arm-before-wait;
   timeout-edge/duplicate ACK; generation change; retry with a fresh token; and
   concurrent CLI Busy/serialization.
3. Promotion state/concurrency: explicit disconnect during AUTH, role, time,
   and AppLaunch exits within the teardown bound; right-only, left-only, and
   missing-arm repair while the timer is active; no automated TX before the
   final ready generation; exactly one ACK-correlated valid-clock cold-connect
   time sync; an invalid-clock connect later attempts a no-ACK-wait,
   generation-fenced runtime sync after time becomes valid, without claiming
   semantic ACK; image/fragment TX makes command `14`
   skip/retry without lock inversion or watchdog increments; SID-`0x80`
   command-14/RING_CONNECT_INFO never clears plugin state or emits WORN; and the
   captured field-5/field-6 vector decodes both exact `2.2.9.22` values, while
   the right-routed EvenCore policy is validated independently from packet
   direction rather than a false version asymmetry.
4. SyncInfo parser goldens: exact command-0 request and command-omitted response,
   command-1 background 1, foreground-only 3, present-empty data,
   background-224/foreground-3, exact new foreground `273`, foreground `34`,
   background `4094`, and background-4094/foreground-34. Cover absent versus
   empty data, reordered/unknown fields, repeated scalar last-wins, repeated
   data merge, wrong known wire type, truncated varint/length, and unchanged
   output/state on failure.
5. SyncInfo/E0 reducer goldens: E0 enter then SyncInfo-273 remains up; E0 exit
   then SyncInfo-down does not extend grace; SyncInfo can repair a missed E0;
   same-side duplicates and LEFT mirrors cause no extra edge while RIGHT is
   authoritative; background `4094` transition and page-swap empty state never
   tear down; delayed expected reason-1 `SYSTEM_EXIT` cannot clobber replacement
   CREATE; real E0 terminal plus later empty causes one teardown; dropped-E0
   debounce causes one fallback; disconnect/generation reset immediately and
   cancel pending debounce across millis wrap. No SID-`0x0D` vector invokes a
   public callback or text navigation/exit, while existing E0 List/Text/Sys
   inputs continue to navigate.
6. R1 exact selector accepts only `2.2.7.0005` and `2.2.9.0003`; near matches,
   prefixes/suffixes, empty, and null remain unknown.
7. Per-command capability matrix proves 2.2.9 single-frame
   HR/HRV/SpO2/activity daily, no-data sleep transaction, adv, health-ON, and
   captured-metric ACK support, plus explicit health-OFF, temperature-daily,
   activity-reassembly, low-power, user-info, wear, point/measure, positive
   sleep data, and sleep-data-ACK rejection.
8. For the same sanitized inputs, 2.2.9 adv-start, health-ON, and captured
   HR/HRV/SpO2/activity packet ACKs are byte-identical to 2.2.7. Unsupported
   health-OFF/point/low-power paths do not enqueue, parse, mutate cache, or
   consume a serial. A CRC-valid but invalid/unsupported daily payload produces
   no ACK, store mutation, or transaction verification. Separately, preserve
   the existing zero-base/unknown-day distinction: a structurally valid page
   may be ACKed and complete its transport transaction, but must produce no
   bucket epochs, typed-history key, graph backfill, or store mutation.
9. Existing sanitized HR/HRV/SpO2/activity fixtures run under both exact
   profiles, including opaque trailer `6`; unknown profiles still fail.
10. Existing synthetic reassembled activity remains accepted for 2.2.7 and is
   explicitly rejected for 2.2.9 pending live fragment evidence. Packet-ACK
   descriptor tests cover only the per-profile captured commands.
11. Low-power state tests cover fresh 2.2.9 link, setter, refresh, repeated
    owner laps/reconnect, exact local unsupported UI/API response, and a later
    2.2.7 reconnect that resumes existing reconciliation.
12. History test loads an existing layout-1 peer/day/timezone model, applies a
    disjoint profile-2 delta, commits/reloads, then applies a disjoint profile-1
    delta. Old buckets remain, loading occurs once per key, path/schema/layout
    stay stable, and the recomputed valid content hash changes with content.
    Stored raw ID `2` and unsupported profiles remain invalid.
13. Telemetry snapshot boot tests force the former cache-copy/reconnect/
    link-copy interleaving and require the old direct generation to publish no
    valid flags or receive stamps. They also require same-generation offline
    direct data to remain hidden, an online direct link to reject forwarded
    mutation without clearing its cache, an offline source-identified
    sid-`0x90` cache to remain visible with `connected=false`, and the same
    production source-transition primitive to clear values in both ownership
    directions. When R1 Health is enabled, the test seeds the live HR series
    and checks its public count before/after both guarded transitions. It then
    invokes the direct extractor with an admitted old same-generation point
    frame after offline forwarded publication and requires the forwarded cache
    and series count to remain unchanged. The same test pauses a fully assembled
    old activity model before finalize, lets disconnect/forwarded custody win,
    and requires finalize to leave packet-ACK state, forwarded cache, and live
    series unchanged.
14. History-peer custody boot tests force dirty A -> deferred B -> reasserted A
    both before selection and at the former extraction/apply gap; stale B must
    not install. They also prove an unsuperseded B does install and that only a
    store acknowledgement matching the exact dirty generation can clear dirty
    state before deferred-peer admission.
15. Preserve every existing 2.2.7 protocol and storage self-test, plus verify
    legacy G2 flag negotiation on an older exact firmware profile where
    hardware or a sanitized fixture is available.

### Build and source review

- Run the relevant protocol self-tests and host tests available in this tree.
- Build the configured FeatherS3 firmware with the repository's normal build
  flags.
- Review the final diff for accidental broad profile gates, fixed-magic ACK
  acceptance, control-owner waits, unbounded allocations, or changes outside
  this report.
- Run `docs2/docsctl.py update --changed`, manually update/review every affected
  per-file and subsystem document, explicitly accept reviewed records, then run
  `docs2/docsctl.py check`.

### Hardware acceptance on the connected device

1. Cold-connect with the official phone app fully out of the connection path.
   Confirm AUTH ACK per arm, RIGHT role ACK, optional valid-clock time ACK, and
   first scheduled command-14 ACKs on their own arms approximately 15 seconds
   after readiness.
2. Remain connected for at least ten minutes. Confirm the old repeatable
   23--27-second reason-`0x13` sequence is absent; a command-14 miss by itself
   must not cause a local disconnect.
3. Open Blocks, navigate and play Tetris with ordinary tap/scroll/double-tap.
   Confirm mappings are unchanged.
4. In the hijacked dashboard, hold to open the native menu, enter brightness,
   change brightness, dismiss it, and confirm the custom page remains usable
   without an auto-exit, blank background, or CREATE/REBUILD race.
   Leave the overlay open for more than 30 seconds and confirm custom refresh
   remains suppressed until genuine exit.
5. Dismiss the app through the native path and confirm one `SYSTEM_EXIT`
   invalidates local container state. Reopen Blocks and confirm a clean CREATE.
   Also exercise an inactivity/idle teardown and a non-FSM CLI-created
   container to validate the dropped-E0 fallback without exposing a synthetic
   `DISPLAY_OFF` callback.
6. Connect R1 `2.2.9.0003`; confirm setup READY and exact profile status. Verify
   no system-settings/low-power, wear, point/measure, or health-OFF command is
   sent. Repeat once with host time deliberately unavailable and confirm a
   bounded clock-unavailable result without a point query.
7. Run a full daily sweep, verify accepted/ACKed HR, HRV, SpO2, and single-frame
   activity pages, plus only the observed sleep command-ACK/no-data behavior.
   Then reconnect and repeat against existing same-day history to rule out
   reload loops and sparse-delta replacement loss.
8. Re-run a 2.2.7 ring if available to prove downgrade and legacy behavior.
9. On older supported G2 firmware, verify negotiated AUTH/base flags, cold
   connect, and AppLaunch custom rendering. If older hardware is unavailable,
   record that preservation as unverified rather than claiming it.
10. A/B the full promotion with recurring command `14` disabled after the first
    scheduled send. This is required before attributing retention to a recurring
    lease.

## Rollback boundaries

The work is separable into three reversible commits/diff groups: G2 promotion,
SyncInfo lifecycle, and R1 profile/history. If G2 promotion does not stop the
reason-`0x13` drops, retain its diagnostics but do not escalate to speculative
commands. Capture the promoted failure and compare the last positively ACKed
stage. If SyncInfo state proves insufficient for overlay timing, SID `0xE0`
remains the authority and the parser can stay diagnostic-only. If an R1 2.2.9
daily subtype violates current checks, reject that subtype without disabling
the independently proven setup/profile support.

## Adversarial review record

Three independent read-only reviews challenged the draft against current source,
the sanitized updated-device captures, persistence behavior, and failure-order
variants. No first-party source was edited during the report/review phase. The
report was revised before implementation authorization.

### Rejected or corrected draft decisions

- **Rejected:** taps were renumbered. Existing Blocks/Tetris input values are
  unchanged; the new hold is native-overlay behavior.
- **Rejected:** a strict two-miss/30-second command-14 lease. Stock sessions
  survived much longer gaps. Command `14` remains best-effort compatibility
  traffic until A/B hardware evidence says otherwise.
- **Rejected:** immediate command-14 seed and priority over the established
  EvenCore heartbeat. First due is approximately 15 seconds after readiness,
  with ordinary watchdog scheduling preserved.
- **Rejected:** ACK matching on only side/generation/command/magic. Matching now
  also requires RX flag `0x00`, expected empty nested body, atomic transaction
  lifecycle, and fresh retry tokens; unsolicited AUTH cannot false-promote.
- **Rejected:** any RX proves the rendering plugin alive. SID-`0x80` health is
  separate; only validated SID-`0xE0` command-12 ACK recovers plugin state.
- **Rejected:** LEFT is globally notification-silent. EvenHub replies route
  through RIGHT for exact known pairs, while direct DevCfg ACKs are per-arm.
  The two version fields must still be decoded independently.
- **Rejected:** treating raw printable bytes around a protobuf string as the
  string itself. The declared eight-byte f5/f6 values are both `2.2.9.22`;
  their following `0x32` and `0x38` field tags are printable ASCII and caused
  the draft's incorrect `.222`/`.228` claim. Typed length-delimited parsing is
  authoritative, and no asymmetric policy may be based on that artifact.
- **Rejected:** SID-`0x0D` empty data is display-off or terminal. It is SyncInfo,
  is transient during launches, and needs command/flag/side/generation guards,
  idempotent reduction, expected-echo protection, and a delayed widget-absence
  fallback rather than a public synthetic event.
- **Rejected:** unconditional `SYSTEM_EXIT` reset. Only terminal,
  current-generation, non-echo events invalidate replacement state; teardown is
  centralized and idempotent.
- **Rejected:** blanket R1 2.2.9 compatibility. Capabilities are per command,
  direction, value, and framing mode. Health-OFF, point/wear/low-power,
  temperature daily, positive sleep data/ACK, and activity reassembly remain
  disabled.
- **Rejected:** generic integrity is sufficient before packet ACK. A 2.2.9
  data page must pass its typed parser before ACK or transaction verification.
  Typed-parser success proves the wire/layout, not a durable day identity;
  storage separately requires an epoch-anchored day. Zero-base/unknown-day
  pages retain the established protocol-flow behavior without reaching history.
- **Rejected:** a synthetic reassembly test proves 2.2.9 fragmentation. It
  proves decoder integrity only; live fragment transport remains unproven.
- **Corrected:** persisting exact profile `2` would loop when loading an
  existing layout-1 file. Exact runtime identity and persisted layout ID are
  separated.
- **Corrected:** “preserve hash” means preserve the algorithm, not the value.
  Sparse common/HRV pages must merge by slot so later deltas do not erase old
  buckets.

### Residual uncertainties retained as release gates

- The missing promotion sequence strongly correlates with repeat reason-`0x13`
  drops, but no capture isolates AUTH, role, time, or recurring command `14` as
  the individual cause.
- Matching empty ACKs prove receipt, not an explicit semantic success code.
- The post-ready time retry is intentionally send-only. Local GATT acceptance
  self-quenches it until clock drift, timezone change, or reconnect re-arms the
  path; a peer-side silent drop can therefore remain unobserved in between.
  It avoids a semantic-ACK wait but can occupy the main-loop task for the short
  central-gate timeout plus the synchronous `writeValue` duration.
- Flag-`0x00` AUTH/base behavior is proven on 2.2.9 but requires negotiated
  legacy fallback and older-hardware regression before claiming preservation.
- Terminal Cmd9 ordering is deliberately fail-closed for a still-current BLE
  generation. If the lens worker remains permanently unavailable, replacement
  CREATE stays blocked until reconnect rather than violating wire order. The
  terminal gate retires on local `writeValue` acceptance, not a ShutdownResp or
  peer semantic acknowledgement; the guarantee is physical API ordering, not
  proof that the peer applied Cmd9.
- After an ambiguous sent CREATE, a compensation Cmd9 without an exact ACK
  deliberately leaves pair-wide quarantine active for that origin generation,
  even if the local GATT write was accepted. Same-generation CREATE remains
  unavailable until a trusted cmd17 lifecycle boundary or reconnect; this is
  intentional safety by availability loss.
- On-wire ACK correlation tokens are eight bits. Both promotion DevCfg and
  CREATE therefore cannot distinguish a same-generation, same-command/body ACK
  delayed across a complete token cycle. Promotion's blocking window is only
  750 ms and reconnect changes generation; CREATE additionally has a local
  32-bit serial plus quarantine/lifecycle boundaries. Those controls make the
  collision remote, not impossible. The corresponding 32-bit serial and epoch
  counters, including typed-history generations used for staged commits, also
  retain a theoretical wraparound ABA residual.
- The authoritative per-arm teardown mirror is completion-ordered. Its matching
  `gLens` clear is a zero-wait FIFO FSM post; extreme queue saturation can leave
  a stale-old eventual mirror, though obsolete cleanup cannot clear a later
  replacement. A few early image-probe exits can likewise leave an old surface
  present rather than risk touching a replacement.
- The R1 evidence is from hardware `603MV1.9.3`; same-firmware behavior on a
  different hardware revision is inferred.
- HardwareOne's complete R1 setup ordering, positive sleep data, temperature
  daily, point/wear/low-power, and 2.2.9 fragmentation remain unproven.
- The Q33 keyboard A/B diagnostic still shares its test level/row globals
  between its session worker and tap callback. It is test-only and independent
  of the production TextEntry execution lease, but Q33 must not be cited as a
  concurrency proof until that harness is separately serialized.
- The 2.2.9 capture contains anchored daily pages only. Its behavior for a
  structurally valid zero-base/unknown-day response is therefore inherited from
  the compatible daily-v1/2.2.7 flow-control model, not independently proven;
  HardwareOne still refuses to invent a day or persist that response.
- Static reasoning, self-tests, and a successful build cannot close BLE timing,
  overlay-duration, disconnect-race, or physical persistence acceptance. The
  connected-device test plan above remains mandatory before release.
