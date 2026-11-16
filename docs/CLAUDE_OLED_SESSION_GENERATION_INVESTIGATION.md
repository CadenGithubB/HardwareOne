# Investigation request: OLED output delivery generations

Please independently investigate the OLED console's delivery fence and the
proposed session-generation replacement below. Treat the claims as hypotheses
to verify or refute from current production code and tests. Explain where you
agree, disagree, or would choose a different design. This request is for
investigation and a recommendation; do not change production behavior or flash
firmware as part of this pass.

Repository: `~/esp/hardwareone-idf`. Read `AGENTS.md` and
`CONTRIBUTING.md`. Preserve all current/concurrent working-tree changes,
including your CLI Output and list-text work. Read the current implementation
rather than assuming this description still matches it.

## Background and reproduced observation

The reviewed `OLEDConsoleBuffer::clear()` erases history, saves `millis()` as
`fenceMs_`, and arms a fence. `append(text, timestampMs)` rejects a message when:

```cpp
fenceArmed_ && (int32_t)(timestampMs - fenceMs_) <= 0
```

The intent is useful: a message queued before logout must not drain into the
next display user's history after that history has been cleared.

Codex reproduced this with the production owner methods and the existing host
clock/mutex doubles:

1. Initialize the console, set `millis()` to 100 and call `clear()`.
2. Without another session change, advance to `100u + 0x80000001u`.
3. Append valid new output stamped with that current time.
4. Actual result: no retained record, and the fenced count increases by one.

The suspected cause is the half-range restriction on signed modular time
comparisons. More than approximately 24.9 days after the boundary, genuinely
new timestamps appear older than the saved fence. This affects OLED history
capture, not whether commands execute or whether the device remains running.
Verify that reasoning, the exact failure interval, and whether the current
code still has this problem. Also examine normal rollover and equal-tick cases.

## Proposed replacement to evaluate

Use a dedicated OLED history-ownership generation:

1. Keep an atomic generation for the current OLED console ownership period.
2. Capture it when an OLED-routed message enters the shared output queue.
3. At a local-display identity boundary, advance the generation and erase the
   history/owned reader state with correctly ordered synchronization.
4. On delivery to the OLED console, accept a queued message only if its captured
   generation still matches the current generation.
5. Retain timestamps as display metadata, not as the ownership decision.

Do not assume this outline solves every race. Determine the correct capture,
comparison and clear linearization points. Prefer the smallest correct change
that fits existing ownership and session mechanisms.

## Questions requiring particular care

### A. Distinguish three kinds of generation/identity

The console already has a content generation used to invalidate/redraw the
viewer as records arrive or history changes. That counter must not be reused
as the delivery fence: ordinary appends must not invalidate other messages
queued for the same user.

Stable record IDs are also distinct from a session-generation token. Record
IDs identify retained entries; content generation signals changed history;
delivery generation identifies the ownership period of pending output.

Verify whether an existing owner/session epoch can safely supply the required
token instead of adding a redundant counter. State precisely when it changes
and how anonymous/guest periods, login/logout, user replacement and role or
identity-policy changes interact with it.

### B. Queued output versus commands finishing later

A tag captured at enqueue can reject messages already queued before the
boundary. It does not, by itself, establish ownership of a command that began
under the old user but produces or completes output after the boundary:
without another guard, that later output could capture the new generation.

Trace the actual command execution and output paths. Identify existing
transport/session checks and determine whether they fence:

- final command return values;
- intermediate broadcasts while a command is running;
- direct/targeted replies versus shared mirrors;
- asynchronous service work that outlives the initiating command;
- ambient background events with no command/display identity.

Do not infer privacy from the presence of a counter. Report verified protections
and concrete gaps separately. Explain how ambient output should behave across
a display boundary without inventing new authorization policy.

### C. Shared queue impact

Likely touchpoints are `OLED_ConsoleBuffer.h/.cpp`, `System_Debug.h/.cpp`, the
local-display boundary/reset path in `OLED_Utils.cpp`, and relevant tests.
Verify the real call graph, including `HardwareOne.cpp`, `OLED_Mode_CLIInput.cpp`
and direct console callers. Reconcile this with the current working tree.

If a generation field is added to `DebugMessage`:

- Every producer path must initialize it, including reusable pool slots,
  explicitly routed output, pre-init events and interrupt-origin output.
- Capturing the token must be safe for each producer context; do not add a
  blocking lock, allocation or display rendering operation to ISR paths.
- An OLED generation mismatch must skip only OLED history ingestion. Serial,
  web, BLE, G2 and file sinks retain their existing routing and independent
  authorization/delivery checks. Never discard the whole shared message just
  because the OLED token is stale.
- Inspect startup, disabled-display builds, initialization/allocation failure,
  and messages queued before the console becomes available.

Determine whether the new field changes `sizeof(DebugMessage)` on the relevant
embedded targets. Padding may absorb some or all of it; do not claim a fixed
RAM increase without measuring. Include initial/grown pool costs and atomic
access cost/constraints. No per-user persisted data or model-file migration is
expected, but verify that conclusion.

### D. Token lifecycle and concurrency

Investigate counter wrap/reuse, repeated clears, explicit history clears versus
identity changes, and reboot behavior. Reboot normally clears both queue and
in-memory token, but verify applicable retained-state paths.

Show that all interleavings of enqueue, append and boundary-clear either retain
output for the correct ownership period or deliberately reject it. Preserve
short critical sections and the existing rule that display drawing happens
outside the console producer mutex. Keep any private reader copies correctly
wiped across boundaries.

## Verification to perform

Use temporary or focused host probes that exercise production code. At least:

- Reproduce/refute the long-uptime timestamp-fence case above.
- Queue old-generation output, change ownership/clear, then drain: it must not
  refill new history.
- Queue multiple messages in one ownership period: accepting one must not
  reject the rest as a side effect of content-generation changes.
- Accept valid new output after a boundary, including a long uptime and clock
  rollover. Define equal-tick behavior explicitly.
- Demonstrate the distinction between a pre-boundary queued message and an
  old command emitting after the boundary; test the applicable command fences.
- Verify stale OLED delivery still permits the message's independently valid
  non-OLED sinks to run.
- Cover concurrent enqueue/append/clear and the token capture/check races.
- Cover display-disabled/uninitialized startup and all enqueue entry points.

Tests of a hypothetical generation design may be prototypes; label them as such
and distinguish them from tests of current production behavior. Do not quietly
install the prototype while describing this as an investigation. Clean up your
temporary binaries/logs/source copies after preserving useful evidence.

## Report to return

Please provide a self-contained report for the user to bring back to Codex:

1. Whether you independently reproduce the current defect and agree with its
   explanation, with evidence and exact affected behavior.
2. Whether you recommend a dedicated generation, an existing epoch, or another
   mechanism, including rejected alternatives and reasons where useful.
3. Queue-entry-to-delivery and boundary sequencing, including a small race
   table or diagram if that makes correctness clearer.
4. Which existing command/session fences cover late output, and any specific
   unresolved gaps. Separate observation from inference.
5. Exact files/interfaces affected, non-OLED routing effects, disabled-build
   compatibility, measured/estimated resource costs and persistence effects.
6. Tests actually run, outcomes and limitations.
7. A bounded implementation plan and acceptance criteria if a fix is warranted.

Please challenge Codex's conclusions where the evidence warrants it. The goal
is a correct, proportionate design, not agreement for its own sake.
