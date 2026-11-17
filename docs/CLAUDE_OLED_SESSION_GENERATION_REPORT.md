# OLED output delivery generations: independent investigation report

Answer to `docs/CLAUDE_OLED_SESSION_GENERATION_INVESTIGATION.md`. Investigation
only: no production source was changed, nothing was flashed. Line numbers are
from the working tree on 2026-11-03 (uncommitted CLI Output and list-text work
included). Probe sources and the derived prototype are preserved under
`docs/oled_session_generation_probes/`; each probe prints what it checked and
is rerunnable with `python3 -I probe/run_repro.py "$PWD"` and
`python3 -I proto/run_proto.py "$PWD"` from that directory.

## 1. The defect is real; Codex's explanation is correct

**Reproduced** with the shipping `OledConsoleBuffer` methods spliced out of
`OLED_ConsoleBuffer.cpp` exactly as `test_oled_console_buffer.py` does
(`probe/repro_current.cpp`, R1):

```
R1 clear@100, append@100+0x80000001: REFUSED (fenced 0 -> 1)
R2 edges: +2^31-1 retained, +2^31 refused, +2^32-1 refused
R5 day-by-day after one boundary (.=retained X=refused):
   .........................XXXXXXXXXXXXXXXXXXXXXXXXX.........................XXXXXXXXXXXXXXXXXXXXXXXXX.
   refused from day 25, retained again from day 50 (period 2^32 ms = 49.71 days, half = 24.86 days)
R3 equal tick refused, next tick retained
R4 rollover: older-before-wrap refused, after-wrap retained
```

**Cause.** `OLED_ConsoleBuffer.cpp:88` refuses when
`(int32_t)(timestampMs - fenceMs_) <= 0`. The unsigned difference lands in the
negative half of `int32_t` whenever it is at least `2^31`. So every envelope
stamped between `fenceMs_ + 2^31` and `fenceMs_ + 2^32 - 1` milliseconds
(24.86 to 49.71 days after the last `clear()`) is refused, then accepted again
for 24.86 days, and so on with a 49.71-day period. The edges are exact
(R2). Ordinary rollover inside the half range works (R4), and the equal tick is
refused by design (`<= 0`, R3).

**Why it persists.** `clear()` is the only writer of `fenceMs_`, it is called
from one place (`OLED_Utils.cpp:6973`, inside
`oledResetLocalDisplaySessionTransients`), and nothing ever disarms the fence.
That function runs on the OLED owner task from
`oledApplyPendingSessionBoundary` (`OLED_Utils.cpp:7005`) for every boundary
after the first owner-side snapshot: login, logout, idle logout, remote
replacement, auth-policy rotation (`System_User.cpp:523-590` publish them
through `oledNotifyLocalDisplayAuthChanged`, `OLED_Utils.cpp:7074`). The fence
is therefore re-armed at "now" by any boundary, and only by a boundary.

**Exact affected behaviour.** Only ingestion into the OLED CLI Output history.
During a refusal window every envelope routed to the OLED sink is counted in
`fencedCount()` and not stored; the page shows `droppedCount()` only
(`OLED_Mode_CLI.cpp:256`), so the loss is silent. Who can sit in the window:

- A display user who stays logged in for 25 days without a boundary. Physical
  input refreshes the idle clock but not the fence; with
  `sessionIdleDisplay = 0` ("off", `System_Settings.cpp:2560`) or regular use
  inside the idle window, their own command echoes and results stop appearing
  on CLI Output from day 25 to day 49 after their login.
- A logged-out device (or one with `localDisplayRequireAuth` off) loses
  ambient/[EVENT] lines from OLED history in the same window. Anyone logging in
  afterwards triggers a boundary, which erases the ring anyway and re-arms the
  fence, so for authenticated-only devices the practical symptom is confined
  to the first case.

Command execution, the inline result screen on CLI Input, serial, web mirror,
file log, BLE, G2, the error/event/EVLOG files and ESP-NOW streaming are all
separate branches of `debugOutputTask` (`System_Debug.cpp:261-468`) and are
untouched.

## 2. Recommendation: a dedicated delivery generation owned by the console

Add `std::atomic<uint32_t> delivery_` to `OledConsoleBuffer`, advanced inside
`clear()` while the producer mutex is held, read by producers through a relaxed
load at enqueue, and compared for equality in `append()` under the mutex.
Timestamps stay in the record as metadata. This is the smallest change that
removes both the half-range failure and the equal-tick ambiguity, keeps the
boundary linearized where it is today (the owner's `clear()`), and keeps the
token in the module that owns the history.

Rejected alternatives, with reasons:

- **Content generation** (`generation_`): moves on every append and clear; using
  it as the fence would refuse every envelope queued behind the first one of a
  burst (prototype P3 shows 60 envelopes captured once must all survive 60
  content changes).
- **`TransportSessionEpoch` for `SOURCE_LOCAL_DISPLAY`**: (a) reading it needs
  `portENTER_CRITICAL` (`System_User.cpp:339-380`), not acceptable in
  `enqueueChunk`'s ISR path; (b) it is zero while logged out with auth
  required, so consecutive logged-out periods are indistinguishable; (c)
  `localDisplayTransportSessionSnapshot` lazily opens a new epoch from any task
  when auth is off (`System_User.cpp:300-308`), so the token can change with no
  UI boundary; (d) auth-policy rotation closes it only when unauthenticated,
  while the OLED applies a boundary on every notification; (e) the cutover
  happens on cmd_exec up to a frame before the owner wipes the ring. It is the
  right token for command ownership (section 4) and the wrong one for queue
  delivery.
- **`sOledSessionBoundaryRequested`**: atomic and bumped exactly once per
  boundary publication, so it is the closest existing counter. But it is
  private to `OLED_Utils.cpp`, it advances at the cutover rather than at the
  wipe, so output enqueued between cutover and `clear()` would be accepted
  where today it is refused, and it couples `System_Debug` to display UI state.
- **64-bit timestamps**: removes the wrap but keeps time as the identity
  decision and the equal-tick case; `DebugMessage` would grow (8 bytes against
  the 7 bytes of padding, unverified) and every `[%lu]` formatter changes.
- **Monotonic sequence number**: equivalent reasoning but an ordered compare
  reintroduces a half range (2^31 envelopes) for nothing; an equality compare
  on a generation has no range at all.
- **Disarm the fence on the first accepted envelope**: the stamp order is not
  strictly the queue order (two producers can stamp then enqueue in the other
  order), and a quiet device still hits the window on its first line.

## 3. Sequencing and races

Capture, compare and clear points for the recommended design:

| Step | Task | Synchronization |
|---|---|---|
| capture `G = delivery_.load(relaxed)` | any producer, ISR included, inside `enqueueChunk` | none; a 32-bit lock-free load (verified on Xtensa and RISC-V, section 5) |
| `clear()`: `ring_.clear()`, `delivery_.fetch_add(1)`, content generation | OLED owner task | producer mutex (`portMAX_DELAY`) |
| `append(text, ts, G)`: refuse if `G != delivery_`, else store | debugOutputTask | producer mutex (10 ms bounded wait; timeout counts a drop) |

Interleavings (C = capture, X = clear, A = append of that envelope):

| Order | Outcome | Correct? |
|---|---|---|
| C, X, A | refused: queued before the boundary, drained after | yes, the case the fence exists for |
| X, C, A | accepted: emitted after the boundary | yes |
| C, A, X | stored, then erased by the clear | yes |
| X concurrent with A | impossible: both hold the mutex | n/a |
| C concurrent with X | either value; a producer that formats at the boundary instant is accepted or refused like any post- or pre-boundary line | yes (section 4 covers the semantic limit) |

Equal tick is defined by this: timestamps never decide, so output stamped in
the same millisecond as the boundary is retained when captured after the
clear and refused when captured before it (prototype P4). Repeated clears
refuse everything captured before the last one (P6). Reuse of a generation
value needs 2^32 boundaries while one envelope sits in a 192-slot queue; not
reachable. Reboot: `delivery_` is .bss, the queue and pool are rebuilt in
`initDebugSystem`, and no RTC, NVS or file state holds any of this (the
`ramflush` overlay does not touch the output queue or the ring).

The display owner's private copies are already wiped before the ring
(`oledCliViewerResetSessionState()` then `gOledConsole.clear()`,
`OLED_Utils.cpp:6970-6973`); drawing stays outside the producer mutex (the page
copies under a 10 ms try-lock and draws from the copies). Nothing in the
recommendation changes that.

## 4. Command and session fences over late output

Verified protections (read in the current tree; "tested" marks host coverage):

- **Admission.** A queued command whose epoch died before it started is refused
  with an audit line (`System_Utils.cpp:4380-4395`). OLED commands set
  `REQUIRE_LIVE_SESSION` and the UI epoch (`OLED_Utils.cpp:3167-3185`).
- **Final result, OLED origin.** `executeOledCommandWithResultImpl` validates
  the epoch under the display writer, copies, re-checks and wipes
  (`OLED_Utils.cpp:3230-3255`); CLI Input re-checks before publishing the reply
  as an OLED-only envelope and before showing the result screen
  (`OLED_Mode_CLIInput.cpp:224-260`). The async path gates the reply in
  `oledDeliverCommandReply` (`OLED_Utils.cpp:3352-3360`); tested by
  `test_oled_command_feedback.py` ("a reply from a replaced session shows
  nothing") and `test_oled_commit_window.py`.
- **Final result, serial origin.** `deliverCommandResult` checks the epoch
  before the queued copy, before the direct write and before the mirror
  (`HardwareOne.cpp:859-970`). Web and BLE handlers answer in their own reply
  channel; their shared-mirror copy goes through `broadcastOutput(out, ctx)`
  (`WebServer_Server.cpp:3492,5412`, `System_Utils.cpp:5001`) with OLED and G2
  forced into the route (`HardwareOne.cpp:806-808`), prefixed by origin and
  user. The BLE direct reply is epoch-fenced per session.
- **Direct replies versus mirrors.** The OLED reply is an explicit
  `MSG_ROUTE_OLED` envelope published only after the epoch check; it is the
  command's one copy on the page. The web mirror is never cleared on an
  identity change (`gWebMirror.clear()` is reached only from `webconsole`
  commands, `System_CLI.cpp:1030,1161`); it is a shared surface by design.

Gaps, verified by reading and shown by probe R7 / prototype P9, that neither
the stamp fence nor the generation closes:

- **G1, streamed lines of a command already running at the boundary.**
  `broadcastOutputCore` (`System_Debug.cpp:957-1034`) has no epoch check; the
  route adds OLED unconditionally for command context; the line is stamped (or
  captured) after the clear and lands in the replacement identity's history.
  Example: a long listing started by user A, A is logged out remotely, the rest
  of the listing is visible to B on CLI Output. The result itself is dropped by
  the fences above, the streamed body is not.
- **G2, service work that outlives its command.** Output from worker tasks has
  no command context and routes `MSG_ROUTE_ALL`. Dictation is already reset at
  the boundary (`dictationResetForSessionBoundary`); other workers were not
  traced individually, and this is inference, not an observed leak.
- **G3, ambient output.** It has no identity. Today it is refused when queued
  before the clear and accepted after; the page documents itself as "one mixed
  stream". Keep that: the clear is what protects the next user, and ambient
  lines stuck in the queue at a boundary are an acceptable, deliberate loss.
  Do not add an authorization policy for ambient lines as part of this change.
- **The refused count is invisible** on the page, which is why the current
  defect was silent. Surfacing `fencedCount()` is optional and separate.

Closing G1 would be a per-line check on cmd_exec in `broadcastOutputCore`
(context present, `REQUIRE_LIVE_SESSION`, `transportSessionEpochIsLive`), which
costs two critical sections per streamed line and is a behaviour change for
every sink, so it should be its own measured change, not part of the fence
fix.

## 5. Files, interfaces, costs, persistence

Call graph confirmed: the single ingestion point is `System_Debug.cpp:409`
(`gOledConsole.append(msg->text, (uint32_t)msg->timestamp)`); the single
producer primitive is `enqueueChunk` (`System_Debug.cpp:791-828`), reached by
`broadcastOutputCore`, `debugQueuePrintf`, `logSystemEvent`, `debugQueueLine`,
`flushPreInitEvents` and the IDF log bridge `idfLogVprintf`
(`System_Debug.cpp:1133-1150`). The only other writes to the free queue return
slots to the pool (lines 466, 527, 695). `init()` runs from both display
bring-up paths (`OLED_Utils.cpp:3880,5316`) and is idempotent.

Changes if the fix is adopted:

| File | Change |
|---|---|
| `System_Debug.h:208` | `DebugMessage` gains `uint32_t oledGeneration` after `routing` |
| `System_Debug.cpp:813` | `enqueueChunk` captures `gOledConsole.deliveryGeneration()` under `#if ENABLE_OLED_DISPLAY`, else 0; this is the one place every pool slot, grown slot, pre-init flush, routed line and ISR line is filled |
| `System_Debug.cpp:409` | pass `msg->oledGeneration` to `append()`; no other branch of the drain changes, a mismatch returns from `append()` only |
| `OLED_ConsoleBuffer.h/.cpp` | `delivery_` replaces `fenceMs_`/`fenceArmed_`; `append(text, ts, gen)`; `deliveryGeneration()`; header comment |
| `OLED_Utils.cpp` | none (the `clear()` call stays where it is) |
| `test_oled_console_buffer.py` | source-guard string and the harness fence cases |
| `oled_cli_output_harness.cpp:176-178` | `append`/`clearConsole` helpers (the `gNow += 10` step becomes unnecessary) |
| `docs/OLED_CLI_OUTPUT.md:102,122` | the "stamped at or before the boundary time" description becomes the delivery generation |

Unaffected test doubles: `command_task_isolation_harness.cpp:60` and
`ble_command_output_harness.cpp:90` fake `enqueueChunk` with its current
signature, which does not change; `test_phone_pair_sink.py:83` fakes a
one-argument `append` that the spliced code never calls.

Non-OLED routing: no change. The generation is consulted only inside the
`MSG_ROUTE_OLED` branch; serial, web, file, BLE, G2, the error and event files
and ESP-NOW streaming keep their own gates. The drain must never `continue` on
a stale OLED token.

Display-disabled builds: `System_Debug.cpp` already includes the
dependency-free `OLED_ConsoleBuffer.h` unconditionally and references
`gOledConsole` only under `ENABLE_OLED_DISPLAY`; the field is set to 0 and
never read there. Allocation failure or no display: `isReady()` is false,
`append` and `clear` are no-ops, `deliveryGeneration()` stays 0, and envelopes
queued before `init()` carry 0 and are accepted afterwards (prototype P7, same
as today's unarmed fence).

Measured sizes (`sizeof/dm.cpp`, ESP-IDF GCC 14.2 for `esp32`, `esp32s3`,
`esp32p4` rv32imafc/ilp32f and `esp32c6` rv32imac/ilp32):

| Layout | sizeof | text offset |
|---|---|---|
| shipped `DebugMessage` | 304 | 41 |
| with `uint32_t oledGeneration` after `routing` | 304 | 48 |

`alignof(DebugFlagMask)` is 8 on all four, so the field is absorbed by padding:
0 bytes per message, 0 bytes for the 64-slot internal pool and the 96 to 192
slot PSRAM pool. `OledConsoleBuffer` itself shrinks by about 4 bytes of .bss.
`std::atomic<uint32_t>::is_always_lock_free` is true on all four, so the ISR
capture is a plain aligned load. No persisted per-user data, settings or model
files are involved; nothing migrates.

## 6. Tests run

Baseline, all passing on this tree: `test_oled_console_buffer.py`,
`test_oled_cli_output.py` (four geometries), `test_oled_command_feedback.py`,
`test_oled_commit_window.py`, `test_command_task_isolation.py`,
`test_ble_command_output.py`.

Probes (ASan/UBSan, `-Wall -Wextra -Werror`):

- `probe/repro_current.cpp`, production methods unmodified: R1 Codex's case,
  R2 exact edges, R3 equal tick, R4 rollover, R5 101-day walk, R6 unarmed
  before any boundary, R7 FIFO drain across a boundary (old refused, the old
  command's late line and the new identity's line both retained). All held.
- `proto/proto_probe.cpp`, a **prototype** derived from the shipping header and
  methods by five targeted substitutions (`proto/make_proto.py`; the derived
  sources are kept beside it): P1 long uptime retained on all 101 days, P2
  pre-boundary queue refused, P3 60 envelopes captured once all retained, P4
  equal tick retained with its timestamp kept, P5 rollover, P6 repeated
  clears, P7 unallocated owner, P8 two producers against a clearer (80,000
  attempts, 803 refused, 0 dropped, every retained record carries the final
  generation), P9 the late-command limit. All held. Not installed.

Limitations: `debugOutputTask`, `enqueueChunk`, the cmd_exec delivery paths
and the owner-side boundary are FreeRTOS code and were verified by reading,
not run on the host; no firmware was built or flashed for this pass; the
sizeof probe uses a copy of the struct's data members, not the real header.

## 7. Bounded implementation plan

1. `OLED_ConsoleBuffer`: replace the two fence members with `delivery_`,
   change `append` to take the captured generation, add
   `deliveryGeneration()`, advance in `clear()` under the mutex, update the
   header comment ("Delivery fence" paragraph).
2. `DebugMessage`: add `oledGeneration` after `routing`; capture it in
   `enqueueChunk`; pass it at `System_Debug.cpp:409`.
3. Tests: move the prototype cases P1 to P8 into
   `oled_console_buffer_harness.cpp` in place of the stamp-fence block, update
   the source guard in `test_oled_console_buffer.py`, simplify the two helpers
   in `oled_cli_output_harness.cpp`, and add a `static_assert` or host check
   that `sizeof(DebugMessage)` is unchanged.
4. Build the current board gate set (the field is in a shared header; a
   display-less board must compile with the field unused) and run the six host
   tests above.
5. Hardware check on the bench board: log in, run a command, log out via the
   web `logout display` while a long listing streams, log in again, confirm
   the ring holds nothing queued before the wipe; then a short `millis()`
   offset is not testable on hardware, so rely on P1.

Acceptance criteria:

- Codex's case (clear at 100, append stamped `100 + 0x80000001`) is retained,
  and the 101-day walk retains every day.
- An envelope captured before `clear()` is refused after it, counted in
  `fencedCount()`, and leaves the ring empty.
- Sixty envelopes captured in one period are all retained.
- Same-millisecond output after the boundary is retained; `timestampMs` is
  still stored on the record.
- Serial, web mirror, file log, BLE and G2 branches are reached for an envelope
  whose OLED token is stale (code review of the drain; no `continue`).
- `sizeof(DebugMessage)` is 304 on all four targets; the display-less gate
  builds.
- `test_oled_console_buffer.py`, `test_oled_cli_output.py` and the four
  command-fence tests pass.

G1 (streamed lines of an already-running command) stays open and is reported
separately above; it is not an acceptance criterion for this fix.

## 8. Implementation record (2026-11-03, after the report)

Applied as planned in section 7, uncommitted, not flashed:

- `OLED_ConsoleBuffer.h/.cpp`: `delivery_` (atomic) replaces `fenceMs_` and
  `fenceArmed_`; `append(text, timestampMs, deliveryGeneration)` compares under
  the mutex; `clear()` advances the generation under the mutex;
  `deliveryGeneration()` added; header comment rewritten.
- `System_Debug.h`: `DebugMessage::oledGeneration` after `routing`, with
  `static_assert(sizeof(DebugMessage) == 304)`.
- `System_Debug.cpp`: `enqueueChunk` captures the generation (0 when the
  display is compiled out); the OLED branch of the drain passes it through.
- Tests: `oled_console_buffer_harness.cpp` carries the boundary cases from the
  prototype (queued-before-boundary refused in the boundary tick, 24.9 days
  later and across the rollover; a 60-envelope burst under one generation;
  three boundaries; an unallocated owner; a 40,000-append two-producer run
  against 200 boundaries) and `test_oled_console_buffer.py` guards the capture
  site and the struct size; `oled_cli_output_harness.cpp` appends through
  `appendAt`, which captures the live generation as the queue would.
- `docs/OLED_CLI_OUTPUT.md`: the fence paragraph and the test list.

Results:

| Check | Outcome |
|---|---|
| `test_oled_console_buffer.py` | pass; 40,000 attempts across 200 boundaries, 399 refused, 0 dropped, every retained record at the final generation |
| `test_oled_cli_output.py` (four geometries) | pass |
| `test_oled_command_feedback.py`, `test_oled_commit_window.py`, `test_command_task_isolation.py`, `test_ble_command_output.py`, `test_phone_pair_sink.py`, `test_r1_protocol.py` (compiles the real `System_Debug.h`) | pass |
| `tools/build_board.sh waveshare_p4_touch_lcd_5 --config waveshare-p4-bringup build` | rc 0, no errors; app 0x4c28a0 B, 41% free; ELF links `append(char const*, unsigned long, unsigned long)` and `deliveryGeneration()`, the two-argument symbol is gone |
| `tools/build_board.sh xiao_s3 --config xiao-default reconfigure build` (display compiled out) | rc 0, no errors; app 0x42fe60 B, 27% free; no `OledConsoleBuffer` symbol in the ELF, drain task present |

Still open: the hardware check in section 7 step 5 (log in, stream a long
listing, remote `logout display`, log in again, confirm nothing queued before
the wipe survives), and gap G1 as described in section 4.

## 9. Bench record (2026-11-03, Waveshare ESP32-P4, `waveshare-p4-bringup`)

Flashed app-only at 0x10000 over the CH343 bridge (`/dev/cu.usbmodem`,
4,991,136 B, hash verified, bootloader and partition table untouched). Boots
captured over the same port with an EN pulse, 115200 baud:

| Boot | Result |
|---|---|
| #64 (first captured reset) | Core 1 `Instruction access fault`, `MEPC 0x00000000`, about 15.5 s in, right after `transport: Transport is already up`; rebooted as #65 (`reset=panic(4)`), which then ran clean for the rest of the 50 s window |
| #66 (second reset, 100 s) | clean: debug system up, Waveshare HX8394 online, Wi-Fi connected, `Setup complete`, BLE scanning; no panic |

The #64 fault is not in the console or debug-queue path. Decoded against the
flashed ELF: the faulting task is `arduino_events` (its name sits on the dumped
stack), `RA` is the `std::function` handler for a plain function-pointer
callback (`_Function_handler<void(arduino_event_id_t, arduino_event_info_t),
void(*)(...)>::_M_invoke`), and the registers hold the vector relocation path
(`construct_at<NetworkEvents::NetworkEventCbList_t>`) and
`WiFiGenericClass::onEvent`. In the vendored Arduino core,
`NetworkEvents::_checkForEvent` iterates `_cbEventList` and `onEvent` appends
to it, and both lock `_mtx` only under `#if defined NETWORK_EVENTS_MUTEX &&
SOC_CPU_CORES_NUM > 1`. `NETWORK_EVENTS_MUTEX` is defined nowhere in this
tree (not in the vendored sources, root `CMakeLists.txt`, or sdkconfig), so on
every dual-core board the dispatcher walks the vector unlocked while the lazy
Wi-Fi init registers `wifiEventLogger` twice (`System_WiFi.cpp:2864-2865`) from
the background boot connect. A reallocation during dispatch leaves the
dispatcher reading freed storage, which produces exactly a non-empty-looking
entry whose function pointer is zero. No record of this signature exists in
`docs/` or the companion logs; it is timing-dependent and did not repeat on
#66. Follow-up proposed separately: define `NETWORK_EVENTS_MUTEX` for the
dual-core boards (root `CMakeLists.txt`, beside
`ARDUINO_NETWORK_EVENT_TASK_STACK_SIZE`), after checking that no registered
callback registers or removes callbacks from inside the dispatcher (a
`std::mutex` is not recursive; `wifiCoexEvent` and `wifiEventLogger` are
atomics-only and snapshot-only today).

Not done on the bench: the generation behaviour itself needs a display login,
a streamed listing, a remote `logout display` and a second login, which take
the user's credentials and the physical controls. Serial on this board also
requires a login, so no host-driven check was attempted.

## 10. Follow-up applied (2026-11-03): `NETWORK_EVENTS_MUTEX` on every dual-core board

**Change.** Root `CMakeLists.txt` only; no vendored file was touched
(`components/arduino/` is gitignored). A new block before `project()`, right
after the `HW1_*_CONFIG_HEADER` definitions, now carries
`add_compile_definitions(NETWORK_EVENTS_MUTEX ARDUINO_NETWORK_EVENT_TASK_STACK_SIZE=4096)`,
and the old `ARDUINO_NETWORK_EVENT_TASK_STACK_SIZE=4096` line at the end of the
file is gone.

**Why before `project()`.** ESP-IDF creates every component target inside
`project()`, and a directory-level `add_compile_definitions()` placed after it
reaches none of them. The old stack-size line sat in that position:
`compile_commands.json` in all 31 existing build directories showed it absent
from the `NetworkEvents.cpp` compile line (only the `HW1_*` headers set before
`project()` and Arduino's own `-DARDUINO_*` were there), so it had been inert
since it was written and the task ran with 4096 only because the vendored
`#ifndef` default is also 4096. With the block moved, every translation unit
of every lane carries both defines (P4 2543/2543, S3 2173/2173, classic ESP32
2427/2427). Uniform scope is also a correctness requirement: the macro adds a
`std::mutex` member to `NetworkEvents`, so every unit that includes
`NetworkEvents.h` must see one layout. `XIAO_ESP32S3_SENSE_ENABLED` at the end
of the file is in the same inert position; `System_BuildConfig.h` defines it
itself, so nothing depends on the CMake copy, and it was left alone.

**Re-entrancy audit.** The mutex is ESP-IDF's default `PTHREAD_MUTEX_NORMAL`
(`pthread.c`), so a relock from the dispatcher task would block forever, and
the dispatch loop holds it while callbacks run. Every callback that can run
there was read: ours are `wifiCoexEvent` (atomics only) and `wifiEventLogger`
(spinlocked snapshot, pending flags, and `espnowNoteWifiChannelMayHaveChanged`,
which sets one atomic); Arduino's `onSysEvent` handlers are
`_onStaArduinoEvent` (status bits, `esp_wifi_disconnect`/`esp_wifi_connect`),
`_onApArduinoEvent` (IPv6 link-local) and `WiFiGenericClass::_eventCallback`
(scan-done; its SmartConfig branch would call `WiFi.begin`, but this firmware
never starts SmartConfig and that branch only runs with STA already enabled, so
`mode()` returns before registering anything). None of them calls `onEvent`,
`removeEvent` or `mode()`. The callers that do take the lock, `WiFi.onEvent`
at boot and in the lazy Wi-Fi init and `mode()` through
`STAClass`/`APClass::onEnable`/`onDisable`, all run in task context outside
the dispatcher. Rule for new code: a Network/WiFi event callback must not
register or remove callbacks or change the Wi-Fi mode.

**Builds** (`tools/build_board.sh`, existing build directories, nothing new
created):

| Lane | Chip | rc | App image | Partition headroom |
|---|---|---|---|---|
| `waveshare-p4-bringup` | ESP32-P4 | 0 | 0x4c3980 | 40% free |
| `xiao-default` | ESP32-S3 | 0 | 0x4313e0 | 27% free |
| `feather_esp32_v2` | ESP32 | 0 | 0x521c30 | 10% free |

Only pre-existing warnings (`G2_Page_Network.cpp:675-678`, unused variables
in the user's working tree).

**Object-code proof.** `NetworkEvents.cpp.obj` was copied out of each build
directory before the rebuild and compared with the new one (same lane, same
flags otherwise):

| Lane | Before: pthread refs / `_checkForEvent` calls | After | Text delta |
|---|---|---|---|
| P4 (RISC-V) | none / `xQueueReceive`, `memcpy`, `vTaskDelete`, `operator delete`, `__throw_bad_function_call` | `pthread_mutex_init/lock/unlock/destroy` undefined; `_checkForEvent` adds `std::mutex::lock` and `unique_lock::unlock` (two sites) | +484 B |
| S3 (Xtensa) | none | same four undefined; `_checkForEvent` sections add `pthread_mutex_lock`, `pthread_mutex_unlock`, `__throw_system_error` (lock inlined) | +639 B |
| ESP32 (Xtensa) | none | same four undefined; literal pool adds `std::mutex::lock`, `unique_lock::unlock` | +470 B |

No new library code: `pthread.c.obj`, `system_error.o` and `functexcept.o`
were already linked in every lane (checked against maps that predate the
change). The whole-image delta on the P4 against this morning's build is
+4,320 B, more than the object accounts for; no first-party source changed
between the two builds by mtime and no archive member was added, so the
remainder is not attributed. It is below 0.1% on every lane.

**RAM.** `pthread_mutex_t` is one `unsigned int` (4 B) inside the `Network`
global; `pthread_mutex_init` runs from the `NetworkEvents` constructor at
static init and creates one FreeRTOS mutex plus an 8 B wrapper on the internal
heap, once per boot. `arduino_events` stack high-water: 2,276 of 4,096 B on
boot #67 against 2,268 B on boot #66.

**Bench.** The rebuilt `waveshare-p4-bringup` app was flashed app-only to the
Waveshare P4 (hash verified, 47.9 s at 921600) and boot #67 recorded for
100 s through the CH343 reset sequence: no panic, Wi-Fi joined and
`[EVENT][WIFI] connected` posted (that line is produced from `wifiEventLogger`,
a callback dispatched under the new lock), HX8394 online, `Setup complete`,
BLE scans running to the end of the window. Two `task_wdt` IDLE0 warnings at
9.9 s and 18.4 s match boot #66 (9.8 s and 16.3 s, `main` on CPU 0 during
boot) and pre-date this change.

**Not verified.** The race was seen once and is timing-dependent; one clean
boot shows the locked dispatcher does not deadlock on the normal boot path,
not that the race is gone. Nothing is committed.
